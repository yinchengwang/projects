/*
 * parser_stmt.c — 语句解析（v1.1 拆文件）
 *
 * 拆出来的原因：加了聚合函数、REAL 类型、浮点/大整数字面量之后，
 * parser.c 涨到 816 行 —— 超了 800 行的硬规则。
 *
 * 拆分的界线是"**解析什么**"：
 *   parser.c      词法辅助 + 表达式解析（SQL 的"值"）
 *   parser_stmt.c 语句解析（SQL 的"动作"）
 *
 * 共享的东西（Parser 结构、辅助函数）在 parser_priv.h。
 *
 * 文法：
 *   create := CREATE TABLE name '(' coldef {',' coldef} ')'
 *   coldef := name type [NOT NULL | NULL]
 *   insert := INSERT INTO name ['(' name {',' name} ')'] VALUES row {',' row}
 *   select := SELECT ('*' | expr {',' expr}) FROM name [WHERE expr]
 *             [GROUP BY expr {',' expr}] [LIMIT num]
 *   update := UPDATE name SET name '=' expr [WHERE expr]
 *   delete := DELETE FROM name [WHERE expr]
 */

#include "query/parser_priv.h"

#include <stdlib.h>
#include <string.h>

int p_parse_type(Parser *p, u8 *out)
{
    if (p_accept_kw(p, "INT") || p_accept_kw(p, "INTEGER")) { *out = T_INT4; return true; }
    if (p_accept_kw(p, "BIGINT")) { *out = T_INT8; return true; }
    if (p_accept_kw(p, "BOOL") || p_accept_kw(p, "BOOLEAN")) { *out = T_BOOL; return true; }
    if (p_accept_kw(p, "TEXT")) { *out = T_TEXT; return true; }
    /*
     * REAL（v1.1）。
     *
     * 【为什么现在才加】
     * 值层与记录层早就有 T_REAL 了（AVG 的结果、坐标、时序数值、JSON 数字），
     * 但建表语法一直没开 —— 结果 REAL 列**建不出来**，
     * 有 REAL 的表只能靠手改 catalog 才造得出来。
     * v1.1 的 AVG 要能对实数列求平均，这一条是前提。
     */
    if (p_accept_kw(p, "REAL") || p_accept_kw(p, "DOUBLE") ||
        p_accept_kw(p, "FLOAT")) { *out = T_REAL; return true; }
    p_fail(p, "不支持的类型");
    return false;
}

int p_parse_create(Parser *p, Stmt *st)
{
    st->kind = ST_CREATE;
    if (!p_expect_ident(p, st->table))
        return E_PARSE_DUMMY;
    if (!p_expect_punct(p, "("))
        return E_PARSE_DUMMY;

    for (;;) {
        if (st->ncols >= SQL_MAX_COLS) {
            p_fail(p, "列数超限");
            return E_PARSE_DUMMY;
        }
        int i = st->ncols;
        if (!p_expect_ident(p, st->col_names[i]))
            return E_PARSE_DUMMY;
        if (!p_parse_type(p, &st->col_types[i]))
            return E_PARSE_DUMMY;

        st->col_nullable[i] = true;
        if (p_accept_kw(p, "NOT")) {
            if (!p_accept_kw(p, "NULL")) {
                p_fail(p, "NOT 后面应为 NULL");
                return E_PARSE_DUMMY;
            }
            st->col_nullable[i] = false;
        } else {
            (void)p_accept_kw(p, "NULL");
        }
        st->ncols++;

        if (p_accept_punct(p, ","))
            continue;
        break;
    }

    if (!p_expect_punct(p, ")"))
        return E_PARSE_DUMMY;
    return OK;
}

int p_parse_create_index(Parser *p, Stmt *st)
{
    st->kind = ST_CREATE_INDEX;
    if (!p_expect_ident(p, st->index_name))
        return E_PARSE_DUMMY;
    if (!p_accept_kw(p, "ON")) {
        p_fail(p, "缺少 ON");
        return E_PARSE_DUMMY;
    }
    if (!p_expect_ident(p, st->table))
        return E_PARSE_DUMMY;
    if (!p_expect_punct(p, "("))
        return E_PARSE_DUMMY;
    if (!p_expect_ident(p, st->index_col))
        return E_PARSE_DUMMY;
    if (!p_expect_punct(p, ")"))
        return E_PARSE_DUMMY;
    return OK;
}

/*
 * VACUUM <table> —— 回收死版本。
 *
 * 表名存 st->table（和 SELECT/INSERT 复用同一个字段）。
 */
int p_parse_vacuum(Parser *p, Stmt *st)
{
    st->kind = ST_VACUUM;
    if (!p_expect_ident(p, st->table))
        return E_PARSE_DUMMY;
    return OK;
}

/*
 * SET TRANSACTION ISOLATION LEVEL { READ COMMITTED | REPEATABLE READ }
 *
 * 语义对齐 PG：
 *   - 必须在事务内设置（PG 允许事务外设置，作为下一个事务的默认值；
 *     这里简化成"必须在事务内"，更严格也更不容易误解）
 *   - 必须在事务的第一条语句之前设置（PG 的规则）
 *
 * 我们接受两种拼法：
 *   SET TRANSACTION ISOLATION LEVEL READ COMMITTED
 *   SET ISOLATION LEVEL REPEATABLE READ
 */
int p_parse_set_isolation(Parser *p, Stmt *st)
{
    st->kind = ST_SET_ISOLATION;

    /*
     * 两种拼法都接受：
     *   SET TRANSACTION ISOLATION LEVEL READ COMMITTED
     *   SET ISOLATION LEVEL REPEATABLE READ
     *
     * TRANSACTION 可选，ISOLATION + LEVEL 必需。
     */
    (void)p_accept_kw(p, "TRANSACTION");   /* 可选 */

    if (!p_accept_kw(p, "ISOLATION")) {
        p_fail(p, "SET 后面需要 ISOLATION LEVEL");
        return E_PARSE_DUMMY;
    }
    if (!p_accept_kw(p, "LEVEL")) {
        p_fail(p, "缺少 LEVEL");
        return E_PARSE_DUMMY;
    }

    /* READ COMMITTED */
    if (p_accept_kw(p, "READ")) {
        if (p_accept_kw(p, "COMMITTED")) {
            st->iso_level = 0;             /* ISO_READ_COMMITTED */
            return OK;
        }
        p_fail(p, "READ 后面需要 COMMITTED");
        return E_PARSE_DUMMY;
    }

    /* REPEATABLE READ */
    if (p_accept_kw(p, "REPEATABLE")) {
        if (p_accept_kw(p, "READ")) {
            st->iso_level = 1;             /* ISO_REPEATABLE_READ */
            return OK;
        }
        p_fail(p, "REPEATABLE 后面需要 READ");
        return E_PARSE_DUMMY;
    }

    p_fail(p, "只支持 READ COMMITTED 或 REPEATABLE READ");
    return E_PARSE_DUMMY;
}

int p_parse_insert(Parser *p, Stmt *st)
{
    st->kind = ST_INSERT;
    if (!p_expect_ident(p, st->table))
        return E_PARSE_DUMMY;

    if (p_accept_punct(p, "(")) {
        int n = 0;
        for (;;) {
            char tmp[SQL_NAME_MAX];
            if (!p_expect_ident(p, tmp))
                return E_PARSE_DUMMY;
            n++;
            if (p_accept_punct(p, ","))
                continue;
            break;
        }
        if (!p_expect_punct(p, ")"))
            return E_PARSE_DUMMY;
        st->ncols = n;      /* 仅用于列数校验，暂不支持重排列顺序 */
    }

    if (!p_accept_kw(p, "VALUES")) {
        p_fail(p, "缺少 VALUES");
        return E_PARSE_DUMMY;
    }
    for (;;) {
        if (st->nrows >= SQL_MAX_ROWS) {
            p_fail(p, "VALUES 行数超限");
            return E_PARSE_DUMMY;
        }
        if (!p_expect_punct(p, "("))
            return E_PARSE_DUMMY;

        int c = 0;
        for (;;) {
            if (c >= SQL_MAX_COLS) {
                p_fail(p, "列数超限");
                return E_PARSE_DUMMY;
            }
            Expr *e = p_parse_expr(p);
            if (!e)
                return E_PARSE_DUMMY;
            st->vals[st->nrows][c++] = e;

            if (p_accept_punct(p, ","))
                continue;
            break;
        }
        if (!p_expect_punct(p, ")"))
            return E_PARSE_DUMMY;

        if (st->ncols == 0)
            st->ncols = c;
        else if (st->ncols != c) {
            p_fail(p, "各行的值个数不一致");
            return E_PARSE_DUMMY;
        }
        st->nrows++;

        if (p_accept_punct(p, ","))
            continue;
        break;
    }
    return OK;
}

int p_parse_select(Parser *p, Stmt *st)
{
    st->kind = ST_SELECT;
    st->limit = -1;

    if (p_accept_punct(p, "*")) {
        st->star = true;
    } else {
        for (;;) {
            if (st->ntargets >= SQL_MAX_COLS) {
                p_fail(p, "投影列数超限");
                return E_PARSE_DUMMY;
            }
            Expr *e = p_parse_expr(p);
            if (!e)
                return E_PARSE_DUMMY;
            st->targets[st->ntargets++] = e;
            if (p_accept_punct(p, ","))
                continue;
            break;
        }
    }

    if (!p_accept_kw(p, "FROM")) {
        p_fail(p, "缺少 FROM");
        return E_PARSE_DUMMY;
    }
    if (!p_expect_ident(p, st->table))
        return E_PARSE_DUMMY;

    if (p_accept_kw(p, "WHERE")) {
        st->where = p_parse_expr(p);
        if (!st->where)
            return E_PARSE_DUMMY;
    }

    /* GROUP BY（v1.1 步骤 3）。顺序：WHERE 之后、LIMIT 之前。 */
    if (p_accept_kw(p, "GROUP")) {
        if (!p_accept_kw(p, "BY")) {
            p_fail(p, "GROUP 后面需要 BY");
            return E_PARSE_DUMMY;
        }
        for (;;) {
            if (st->ngroupby >= SQL_MAX_COLS) {
                p_fail(p, "GROUP BY 列数超限");
                return E_PARSE_DUMMY;
            }
            Expr *g = p_parse_expr(p);
            if (!g)
                return E_PARSE_DUMMY;
            st->groupby[st->ngroupby++] = g;

            if (p_accept_punct(p, ","))
                continue;
            break;
        }
    }

    if (p_accept_kw(p, "LIMIT")) {
        if (p->cur.type != TK_NUM || p->cur.is_real) {
            p_fail(p, "LIMIT 后面需要整数");
            return E_PARSE_DUMMY;
        }
        if (p->cur.unum > (u64)INT32_MAX) {
            p_fail(p, "LIMIT 太大");
            return E_PARSE_DUMMY;
        }
        st->limit = (int)p->cur.unum;
        p_advance(p);
    }
    return OK;
}

int p_parse_update(Parser *p, Stmt *st)
{
    st->kind = ST_UPDATE;
    if (!p_expect_ident(p, st->table))
        return E_PARSE_DUMMY;
    if (!p_accept_kw(p, "SET")) {
        p_fail(p, "缺少 SET");
        return E_PARSE_DUMMY;
    }
    if (!p_expect_ident(p, st->set_col))
        return E_PARSE_DUMMY;
    if (!p_expect_punct(p, "="))
        return E_PARSE_DUMMY;

    st->set_expr = p_parse_expr(p);
    if (!st->set_expr)
        return E_PARSE_DUMMY;

    if (p_accept_kw(p, "WHERE")) {
        st->where = p_parse_expr(p);
        if (!st->where)
            return E_PARSE_DUMMY;
    }
    return OK;
}

int p_parse_delete(Parser *p, Stmt *st)
{
    st->kind = ST_DELETE;
    if (!p_accept_kw(p, "FROM")) {
        p_fail(p, "DELETE 后面缺少 FROM");
        return E_PARSE_DUMMY;
    }
    if (!p_expect_ident(p, st->table))
        return E_PARSE_DUMMY;
    if (p_accept_kw(p, "WHERE")) {
        st->where = p_parse_expr(p);
        if (!st->where)
            return E_PARSE_DUMMY;
    }
    return OK;
}

