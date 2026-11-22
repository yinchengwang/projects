/*
 * parser.c — 递归下降
 *
 * 文法：
 *   stmt   := create | insert | select | update | delete
 *   create := CREATE TABLE name '(' coldef {',' coldef} ')'
 *   coldef := name type [NOT NULL | NULL]
 *   insert := INSERT INTO name ['(' name {',' name} ')'] VALUES row {',' row}
 *   select := SELECT ('*' | expr {',' expr}) FROM name [WHERE expr] [LIMIT num]
 *   update := UPDATE name SET name '=' expr [WHERE expr]
 *   delete := DELETE FROM name [WHERE expr]
 *   expr   := or
 *   or     := and {OR and}
 *   and    := cmp {AND cmp}
 *   cmp    := add [op add | IS [NOT] NULL]
 *   add    := primary {('+'|'-') primary}
 *   primary:= name | num | str | TRUE | FALSE | NULL | '(' expr ')' | NOT primary
 */

#include "query/parser_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void p_fail(Parser *p, const char *msg)
{
    if (!p->failed) {
        snprintf(p->err, (size_t)p->errcap, "%s (位置 %d)", msg, p->cur.pos);
        p->failed = true;
    }
}

void p_advance(Parser *p)
{
    if (p->failed)
        return;
    if (lex_next(&p->lx, &p->cur) != OK) {
        /* 词法器带回了具体原因就用它（否则只说"词法错误"，没法查） */
        if (p->cur.text[0])
            p_fail(p, p->cur.text);
        else
            p_fail(p, "词法错误");
    }
}

bool p_accept_kw(Parser *p, const char *kw)
{
    if (tok_is_kw(&p->cur, kw)) {
        p_advance(p);
        return true;
    }
    return false;
}

bool p_accept_punct(Parser *p, const char *s)
{
    if (tok_is_punct(&p->cur, s)) {
        p_advance(p);
        return true;
    }
    return false;
}

bool p_expect_punct(Parser *p, const char *s)
{
    if (p_accept_punct(p, s))
        return true;
    p_fail(p, "缺少符号");
    return false;
}

/*
 * 把 token 文本拷进定长的名字缓冲。
 *
 * 【为什么不用 snprintf】
 *
 * `snprintf(out, SQL_NAME_MAX, "%s", src)` 里 src 是 char[256]、目标是 64 ——
 * GCC 15 + glibc 的 -Wformat-truncation 能算出"可能截断"，在 -Werror 下直接红。
 * MinGW 的 GCC 14 不报这个，所以本地一直是绿的：**这是纯 Linux 平台的差异**。
 *
 * 用显式的长度运算表达同一个语义（截到 SQL_NAME_MAX-1 字节 + NUL），
 * 编译器就不替我们"猜"了。截断行为与原来**逐字一致**。
 *
 * ⚠️ 截断意味着两个超长标识符会撞成同一个名字。这是**既有行为**（原来就这样），
 *    本函数不去改它 —— 真要改应该是词法层限制标识符长度并报错，
 *    那是另一件事，不该混进这处移植修补。
 */
static void copy_name(char *out, const char *src)
{
    size_t n = strlen(src);

    if (n >= (size_t)SQL_NAME_MAX)
        n = (size_t)SQL_NAME_MAX - 1u;
    memcpy(out, src, n);
    out[n] = '\0';
}

bool p_expect_ident(Parser *p, char *out)
{
    if (p->cur.type != TK_IDENT) {
        p_fail(p, "需要一个标识符");
        return false;
    }
    copy_name(out, p->cur.text);
    p_advance(p);
    return true;
}

/* ---------------- 表达式 ---------------- */

Expr *p_expr_new(ExprKind k)
{
    Expr *e = calloc(1, sizeof(*e));
    if (e)
        e->kind = k;
    return e;
}

/*
 * 递归释放一棵表达式树。
 *
 * 【为什么现在必须有它】
 * 加入 EX_FUNC（通用函数调用）之后，Expr 可能挂着 `args[]` 指针数组，
 * 每个元素又是一棵独立的子树。原来 free_stmt 只对顶层 `free()`，
 * 加入 args 之后每次函数调用都会泄漏「实参数组 + 全部实参子树」。
 *
 * 【为什么连 EX_BIN / EX_UN / EX_AGG 也一起释放】
 * 它们本来也只在顶层被 free()，子树一律泄漏 —— 这是既有缺陷。
 * 既然要为 EX_FUNC 引入递归释放，顺手把同一函数里的其它分支补全，
 * 否则"有的分支递归、有的分支不递归"是更难解释的半吊子状态。
 * 语义不变（释放本来就没人再用的内存），只是不再漏。
 */
void free_expr(Expr *e)
{
    if (!e)
        return;

    free_expr(e->l);
    free_expr(e->r);

    for (int i = 0; i < e->nargs; i++)
        free_expr(e->args[i]);
    free(e->args);

    free(e);
}

void free_stmt(Stmt *st)
{
    if (!st)
        return;

    for (int i = 0; i < st->ntargets; i++)
        free_expr(st->targets[i]);
    for (int i = 0; i < st->ngroupby; i++)
        free_expr(st->groupby[i]);

    /*
     * INSERT 值矩阵：行数动态（见 Stmt.vals 的注释）。
     *
     * 按 **vals_cap** 遍历而不是 nrows：解析中途失败时 nrows 还没 ++，
     * 但那一行可能已经放过几个 Expr 了 —— 用 nrows 会漏掉它们。
     * 每个槽里的 Expr 无论是否 release 过都要 free（p_expr_new 是 calloc，
     * 失败时不会被写进槽里）。
     *
     * 行内只有前 ncols 个槽是有效的；但列数不一致的行可能在更靠前的
     * 槽就填过值（之后才报"各行的值个数不一致"），所以按 SQL_MAX_COLS
     * 全宽扫一遍最稳，代价是每行 32 次判空。
     */
    if (st->vals) {
        for (int r = 0; r < st->vals_cap; r++) {
            if (!st->vals[r])
                continue;
            for (int c = 0; c < SQL_MAX_COLS; c++)
                free_expr(st->vals[r][c]);
            free(st->vals[r]);
        }
        free(st->vals);
    }

    free_expr(st->where);
    free_expr(st->set_expr);
    memset(st, 0, sizeof(*st));
}

Expr *p_parse_expr(Parser *p);

/*
 * 函数名 → 聚合函数种类。不是聚合函数就返回 AGG_NONE。
 *
 * 【为什么只认这五个】
 * 本版只做聚合（v1.1 步骤 3），别的函数（UPPER/LIKE/…）明确不做 ——
 * 认错了比不认更糟：认成聚合会算出错的结果，不认会明确报错。
 */
static AggKind agg_kind_of(const char *name)
{
    if (strcmp(name, "COUNT") == 0) return AGG_COUNT;
    if (strcmp(name, "SUM") == 0)   return AGG_SUM;
    if (strcmp(name, "AVG") == 0)   return AGG_AVG;
    if (strcmp(name, "MIN") == 0)   return AGG_MIN;
    if (strcmp(name, "MAX") == 0)   return AGG_MAX;
    return AGG_NONE;
}

/*
 * 聚合函数调用：COUNT(*) / COUNT(col) / SUM(col) / AVG(col) / MIN(col) / MAX(col)
 *
 * AST 形状：EX_AGG，op = AggKind，l = 参数表达式（COUNT(*) 时为 NULL）。
 * 用 l 装参数，绑定逻辑（bind_expr 递归进 l）就不用改 ✓。
 */
static Expr *parse_agg_call(Parser *p, const char *fname)
{
    AggKind k = agg_kind_of(fname);
    if (k == AGG_NONE) {
        char msg[128];
        snprintf(msg, sizeof(msg), "不支持的函数: %s", fname);
        p_fail(p, msg);
        return NULL;
    }

    Expr *e = p_expr_new(EX_AGG);
    if (!e) { p_fail(p, "内存不足"); return NULL; }
    e->op = (int)k;
    snprintf(e->name, sizeof(e->name), "%s", fname);

    if (p_accept_punct(p, "*")) {
        /* 只有 COUNT 允许 * */
        if (k != AGG_COUNT) {
            char msg[128];
            snprintf(msg, sizeof(msg), "%s(*) 不合法", fname);
            p_fail(p, msg);
            return NULL;
        }
        e->l = NULL;                  /* COUNT(*)：不数列，数行 */
    } else {
        e->l = p_parse_expr(p);
        if (!e->l)
            return NULL;
    }

    if (!p_expect_punct(p, ")"))
        return NULL;
    return e;
}

/*
 * 通用标量函数调用：`name(arg1, arg2, ...)`。
 *
 * 【与 parse_agg_call 的区别】
 * 聚合（COUNT / SUM / AVG / MIN / MAX）是**已知的五个**，走 EX_AGG，
 * 有专门的聚合求值路径；这里是**任意名字**的标量函数，走 EX_FUNC，
 * 逐行求值时去值层的函数表按名字查 —— 执行器不认识任何具体函数名。
 *
 * 【为什么不在解析期检查函数名是否已注册】
 * 注册发生在 db_open 里，而解析可能在任何时刻。解析期查表会把
 * "函数不存在"从**运行期明确报错**变成"解析期报错"，两者都对；
 * 但解析期会让 parser 依赖值层的注册表状态 —— 那是不必要的耦合。
 * ⇒ 保持解析期只做语法，未注册的名字在求值期返回 E_NOTSUP。
 */
static Expr *parse_func_call(Parser *p, const char *fname)
{
    Expr *e = p_expr_new(EX_FUNC);
    if (!e) {
        p_fail(p, "内存不足");
        return NULL;
    }
    snprintf(e->name, sizeof(e->name), "%s", fname);

    /*
     * 实参数组：先按上限分配，解析完再把 nargs 定下来。
     * 不 realloc —— 上限只有 8，固定分配简单且没有失败路径的复杂度。
     */
    e->args = calloc(EX_FUNC_MAX_ARGS, sizeof(Expr *));
    if (!e->args) {
        p_fail(p, "内存不足");
        free_expr(e);
        return NULL;
    }

    if (p_accept_punct(p, ")"))
        return e;                    /* 零实参函数 */

    for (;;) {
        if (e->nargs >= EX_FUNC_MAX_ARGS) {
            char msg[128];
            snprintf(msg, sizeof(msg), "函数实参超过 %d 个: %s",
                     EX_FUNC_MAX_ARGS, fname);
            p_fail(p, msg);
            free_expr(e);
            return NULL;
        }
        Expr *arg = p_parse_expr(p);
        if (!arg) {
            free_expr(e);
            return NULL;
        }
        e->args[e->nargs++] = arg;

        if (p_accept_punct(p, ","))
            continue;
        break;
    }

    if (!p_expect_punct(p, ")")) {
        free_expr(e);
        return NULL;
    }
    return e;
}

static Expr *parse_primary(Parser *p)
{
    Expr *e;

    if (p_accept_kw(p, "NOT")) {
        e = p_expr_new(EX_UN);
        if (!e) { p_fail(p, "内存不足"); return NULL; }
        e->op = EX_OP_NOT;
        e->l = parse_primary(p);
        return e;
    }

    if (p_accept_punct(p, "(")) {
        e = p_parse_expr(p);
        if (!p_expect_punct(p, ")"))
            return NULL;
        return e;
    }

    /* 一元负号：-5。必须放在二元减号之前处理，
     * 因为 parse_add 的右操作数也是走 parse_primary。 */
    if (p_accept_punct(p, "-")) {
        /*
         * 【为什么负号要在这里就变成字面量】
         * 因为 -9223372036854775808（INT64_MIN）的幅值 2^63 超出 i64 上限：
         * 词法器只能给出幅值，能不能表示要看前面有没有负号 ✓
         *
         * 顺带的好处：`-5` 变成普通的 EX_LIT，
         * 列式求值的形状检查（"列 vs 常量"）就能认它 ——
         * 以前它是 EX_UN(NEG)，列式一律不认，只能退回逐行。
         */
        if (p->cur.type == TK_NUM) {
            if (p->cur.is_real) {
                /* 负实数：-1.5 */
                double mag = p->cur.fnum;
                p_advance(p);
                e = p_expr_new(EX_LIT);
                if (!e) { p_fail(p, "内存不足"); return NULL; }
                val_real(&e->lit, -mag);
                return e;
            }
            u64 mag = p->cur.unum;
            if (mag > (u64)INT64_MAX + 1u) {
                p_fail(p, "负整数常量超出 64 位整数范围");
                return NULL;
            }
            p_advance(p);
            e = p_expr_new(EX_LIT);
            if (!e) { p_fail(p, "内存不足"); return NULL; }
            if (mag == (u64)INT64_MAX + 1u)
                val_int(&e->lit, INT64_MIN);
            else
                val_int(&e->lit, -(i64)mag);
            return e;
        }

        e = p_expr_new(EX_UN);
        if (!e) { p_fail(p, "内存不足"); return NULL; }
        e->op = EX_OP_NEG;
        e->l = parse_primary(p);
        return e;
    }

    if (p_accept_kw(p, "NULL")) {
        e = p_expr_new(EX_LIT);
        if (!e) { p_fail(p, "内存不足"); return NULL; }
        val_null(&e->lit);
        return e;
    }

    if (p_accept_kw(p, "TRUE")) {
        e = p_expr_new(EX_LIT);
        if (!e) { p_fail(p, "内存不足"); return NULL; }
        val_bool(&e->lit, true);
        return e;
    }

    if (p_accept_kw(p, "FALSE")) {
        e = p_expr_new(EX_LIT);
        if (!e) { p_fail(p, "内存不足"); return NULL; }
        val_bool(&e->lit, false);
        return e;
    }

    if (p->cur.type == TK_NUM) {
        if (p->cur.is_real) {
            double f = p->cur.fnum;
            p_advance(p);
            e = p_expr_new(EX_LIT);
            if (!e) { p_fail(p, "内存不足"); return NULL; }
            val_real(&e->lit, f);
            return e;
        }
        if (p->cur.unum > (u64)INT64_MAX) {
            p_fail(p, "整数常量超出 64 位整数范围");
            return NULL;
        }
        e = p_expr_new(EX_LIT);
        if (!e) { p_fail(p, "内存不足"); return NULL; }
        val_int(&e->lit, (i64)p->cur.unum);
        p_advance(p);
        return e;
    }

    if (p->cur.type == TK_STR) {
        e = p_expr_new(EX_LIT);
        if (!e) { p_fail(p, "内存不足"); return NULL; }
        val_text(&e->lit, p->cur.text, p->cur.len);
        p_advance(p);
        return e;
    }

    if (p->cur.type == TK_IDENT) {
        char fname[SQL_NAME_MAX];
        copy_name(fname, p->cur.text);
        p_advance(p);

        /*
         * 后面紧跟 "(" → 函数调用。
         *
         * 已知的五个聚合函数走 EX_AGG（有专门求值路径）；
         * 其余名字一律走**通用的** EX_FUNC —— 按名字查值层函数表。
         * 解析器不认识 json_match，也不需要认识。
         */
        if (p_accept_punct(p, "(")) {
            if (agg_kind_of(fname) != AGG_NONE)
                return parse_agg_call(p, fname);
            return parse_func_call(p, fname);
        }

        e = p_expr_new(EX_COL);
        if (!e) { p_fail(p, "内存不足"); return NULL; }
        snprintf(e->name, sizeof(e->name), "%s", fname);
        e->col_idx = -1;
        return e;
    }

    p_fail(p, "表达式不完整");
    return NULL;
}

static Expr *parse_add(Parser *p)
{
    Expr *l = parse_primary(p);
    while (l && (tok_is_punct(&p->cur, "+") || tok_is_punct(&p->cur, "-"))) {
        int op = tok_is_punct(&p->cur, "+") ? '+' : '-';
        p_advance(p);
        Expr *r = parse_primary(p);
        if (!r) { free(l); return NULL; }
        Expr *e = p_expr_new(EX_BIN);
        if (!e) { p_fail(p, "内存不足"); free(l); free(r); return NULL; }
        e->op = op;
        e->l = l;
        e->r = r;
        l = e;
    }
    return l;
}

static Expr *parse_cmp(Parser *p)
{
    Expr *l = parse_add(p);
    if (!l)
        return NULL;

    if (p_accept_kw(p, "IS")) {
        bool neg = p_accept_kw(p, "NOT");
        if (!p_accept_kw(p, "NULL")) {
            p_fail(p, "IS 后面只支持 NULL");
            free(l);
            return NULL;
        }
        Expr *e = p_expr_new(EX_UN);
        if (!e) { p_fail(p, "内存不足"); free(l); return NULL; }
        e->op = neg ? EX_OP_ISNOTNULL : EX_OP_ISNULL;
        e->l = l;
        return e;
    }

    int op = 0;
    if (tok_is_punct(&p->cur, "="))       op = '=';
    else if (tok_is_punct(&p->cur, "<>")) op = 'N';
    else if (tok_is_punct(&p->cur, "<"))  op = '<';
    else if (tok_is_punct(&p->cur, "<=")) op = 'L';
    else if (tok_is_punct(&p->cur, ">"))  op = '>';
    else if (tok_is_punct(&p->cur, ">=")) op = 'G';

    if (!op)
        return l;

    p_advance(p);
    Expr *r = parse_add(p);
    if (!r) { free(l); return NULL; }

    Expr *e = p_expr_new(EX_BIN);
    if (!e) { p_fail(p, "内存不足"); free(l); free(r); return NULL; }
    e->op = op;
    e->l = l;
    e->r = r;
    return e;
}

static Expr *parse_and(Parser *p)
{
    Expr *l = parse_cmp(p);
    while (l && p_accept_kw(p, "AND")) {
        Expr *r = parse_cmp(p);
        if (!r) { free(l); return NULL; }
        Expr *e = p_expr_new(EX_BIN);
        if (!e) { p_fail(p, "内存不足"); free(l); free(r); return NULL; }
        e->op = EX_OP_AND;
        e->l = l;
        e->r = r;
        l = e;
    }
    return l;
}

Expr *p_parse_expr(Parser *p)
{
    Expr *l = parse_and(p);
    while (l && p_accept_kw(p, "OR")) {
        Expr *r = parse_and(p);
        if (!r) { free(l); return NULL; }
        Expr *e = p_expr_new(EX_BIN);
        if (!e) { p_fail(p, "内存不足"); free(l); free(r); return NULL; }
        e->op = EX_OP_OR;
        e->l = l;
        e->r = r;
        l = e;
    }
    return l;
}

/* ---------------- 语句 ---------------- */

int parse_sql(const char *sql, Stmt *out, char *errbuf, int errcap)
{
    Parser p;
    memset(&p, 0, sizeof(p));
    memset(out, 0, sizeof(*out));

    p.err = errbuf;
    p.errcap = errcap;
    if (errcap > 0)
        errbuf[0] = '\0';

    lex_init(&p.lx, sql);
    p_advance(&p);
    if (p.failed)
        return E_INVAL;

    int rc = E_PARSE_DUMMY;

    /* EXPLAIN 前缀（v1.1 步骤 4）：只包 SELECT */
    if (p_accept_kw(&p, "EXPLAIN"))
        out->explain = true;

    if (p_accept_kw(&p, "CREATE")) {
        if (p_accept_kw(&p, "TABLE")) {
            rc = p_parse_create(&p, out);
        } else if (p_accept_kw(&p, "INDEX")) {
            rc = p_parse_create_index(&p, out);
        } else {
            p_fail(&p, "只支持 CREATE TABLE / CREATE INDEX");
        }
    } else if (p_accept_kw(&p, "INSERT")) {
        if (!p_accept_kw(&p, "INTO")) {
            p_fail(&p, "INSERT 后面缺少 INTO");
        } else {
            rc = p_parse_insert(&p, out);
        }
    } else if (p_accept_kw(&p, "SELECT")) {
        rc = p_parse_select(&p, out);
    } else if (p_accept_kw(&p, "UPDATE")) {
        rc = p_parse_update(&p, out);
    } else if (p_accept_kw(&p, "DELETE")) {
        rc = p_parse_delete(&p, out);
    } else if (p_accept_kw(&p, "BEGIN")) {
        out->kind = ST_BEGIN;
        rc = OK;                  /* 事务控制语句没有子句要解析 */
    } else if (p_accept_kw(&p, "COMMIT")) {
        out->kind = ST_COMMIT;
        rc = OK;
    } else if (p_accept_kw(&p, "ROLLBACK")) {
        out->kind = ST_ROLLBACK;
        rc = OK;
    } else if (p_accept_kw(&p, "VACUUM")) {
        rc = p_parse_vacuum(&p, out);
    } else if (p_accept_kw(&p, "CHECKPOINT")) {
        /* CHECKPOINT：没有子句（v1.2 步骤 6）*/
        out->kind = ST_CHECKPOINT;
        rc = OK;
    } else if (p_accept_kw(&p, "SET")) {
        rc = p_parse_set_isolation(&p, out);
    } else {
        p_fail(&p, "无法识别的语句");
    }

    if (out->explain && rc == OK && out->kind != ST_SELECT) {
        p_fail(&p, "EXPLAIN 只支持 SELECT");
        rc = E_PARSE_DUMMY;
    }

    if (rc == E_PARSE_DUMMY || p.failed) {
        free_stmt(out);
        return E_INVAL;
    }

    /* 允许结尾有分号 */
    (void)p_accept_punct(&p, ";");

    if (p.cur.type != TK_EOF) {
        p_fail(&p, "语句结尾有多余内容");
        free_stmt(out);
        return E_INVAL;
    }
    return OK;
}
