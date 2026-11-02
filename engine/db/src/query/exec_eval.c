/*
 * exec_eval.c — 列式表达式求值（v1.1 步骤 2）
 *
 * ==================== 这里干什么 ====================
 *
 * 把 WHERE 表达式在**一列紧凑数组**上求值，产出选择向量。
 *
 * ==================== 三条纪律（A1 全等的前提） ====================
 *
 *   1. **比较语义一律照着 val_cmp 复刻**，不自己另立一套规则：
 *        - 任一边为 NULL → 空
 *        - 两边都是 TEXT → 按字节比（先比共同长度，再比长度）
 *        - 任一边是 REAL → 按 double 比
 *        - 其余 → 按 i64 比
 *   2. **三值逻辑用三态标记**（假 / 真 / 空），AND/OR 用查表 ——
 *      bool 装不下 NULL，用 bool 就一定会把 NULL 当成假。
 *   3. **形状不支持就整条退回逐行路径**，不做"批内再降级"——
 *      同一份语义只有一处实现，不会分叉。
 */

#include "query/exec_batch.h"

#include <string.h>

/* ---------------- 三态标记 ---------------- */

/* AND / OR 的三态真值表（索引 [左][右]） */
static const u8 AND3[3][3] = {
    /* 假 */ { M_FALSE, M_FALSE, M_FALSE },
    /* 真 */ { M_FALSE, M_TRUE,  M_NULL  },
    /* 空 */ { M_FALSE, M_NULL,  M_NULL  }
};
static const u8 OR3[3][3] = {
    /* 假 */ { M_FALSE, M_TRUE, M_NULL },
    /* 真 */ { M_TRUE,  M_TRUE,  M_TRUE  },
    /* 空 */ { M_NULL,  M_TRUE,  M_NULL  }
};

/* ---------------- 形状检查 ---------------- */

/*
 * 这一列能不能参与列式比较？
 *
 * TEXT 不行：外部值要先物化才能比（val_cmp 里就是这么做的），
 * 而批式不物化外部值 —— 遇到 TEXT 列的谓词，整条语句退回逐行路径。
 */
static bool col_cmp_ok(const ColDesc *cols, int ncols, int idx)
{
    if (idx < 0 || idx >= ncols)
        return false;
    u8 t = cols[idx].type;
    return t == T_INT4 || t == T_INT8 || t == T_BOOL || t == T_REAL;
}

/* 是不是"列 vs 常量"的比较形状？如果是，把列号与（可能翻转的）操作符写出来 */
static bool is_cmp_shape(const Expr *e, const ColDesc *cols, int ncols,
                         int *col_out, int *op_out)
{
    if (!e || e->kind != EX_BIN)
        return false;

    int op = e->op;
    if (op != '=' && op != '<' && op != '>' && op != 'L' && op != 'G')
        return false;

    if (e->l && e->l->kind == EX_COL && e->r && e->r->kind == EX_LIT) {
        if (!col_cmp_ok(cols, ncols, e->l->col_idx))
            return false;
        *col_out = e->l->col_idx;
        *op_out = op;
        return true;
    }
    if (e->r && e->r->kind == EX_COL && e->l && e->l->kind == EX_LIT) {
        if (!col_cmp_ok(cols, ncols, e->r->col_idx))
            return false;
        *col_out = e->r->col_idx;
        *op_out = flip_op(op);        /* 常量在左边：5 < id 等价于 id > 5 */
        return true;
    }
    return false;
}

bool where_supported(const Expr *e, const ColDesc *cols, int ncols,
                     int depth, int *need_depth)
{
    if (!e)
        return true;
    if (depth > MARK_DEPTH / 2 - 2)
        return false;                       /* 层数不够 → 退回逐行 */

    switch (e->kind) {
    case EX_LIT:
        return true;

    case EX_UN:
        if (e->op == EX_OP_NOT) {
            if (!where_supported(e->l, cols, ncols, depth + 1, need_depth))
                return false;
        } else if (e->op == EX_OP_ISNULL || e->op == EX_OP_ISNOTNULL) {
            /* IS NULL 只看 NULL 位图，不需要比较 —— TEXT 列也可以 */
            if (!e->l || e->l->kind != EX_COL)
                return false;
            if (e->l->col_idx < 0 || e->l->col_idx >= ncols)
                return false;
        } else {
            return false;
        }
        if (depth + 2 > *need_depth)
            *need_depth = depth + 2;
        return true;

    case EX_BIN:
        if (e->op == EX_OP_AND || e->op == EX_OP_OR) {
            if (!where_supported(e->l, cols, ncols, depth + 1, need_depth))
                return false;
            if (!where_supported(e->r, cols, ncols, depth + 1, need_depth))
                return false;
        } else {
            int c, op;
            if (!is_cmp_shape(e, cols, ncols, &c, &op))
                return false;
        }
        if (depth + 3 > *need_depth)
            *need_depth = depth + 3;
        return true;

    default:
        return false;
    }
}

/* ---------------- 比较 ---------------- */

/*
 * 比较一个列与一个常量，逐行给出三态结果。
 *
 * 【语义必须与 val_cmp 一致】逐条对照：
 *   - 任一边为 NULL → 空
 *   - 两边都是 TEXT → 按字节比（先比共同长度，再比长度）
 *   - **一边 TEXT 一边非 TEXT → 按类型序（数值 < 文本），永不相等**
 *   - 任一边是 REAL → 按 double 比
 *   - 其余 → 按 i64 比
 */
static void cmp_col_lit(const AmBatchCol *col, const DbValue *lit, int op,
                        u8 *dst, u32 n)
{
    if (lit->is_null) {
        for (u32 i = 0; i < n; i++)
            dst[i] = M_NULL;
        return;
    }

    bool text_cmp = (col->type == T_TEXT && lit->type == T_TEXT);
    bool real_cmp = (col->type == T_REAL || lit->type == T_REAL);
    /* 类型序：数值 < 文本（对齐 SQLite 与 val_cmp） */
    bool mixed_text = (col->type == T_TEXT) != (lit->type == T_TEXT);

    for (u32 i = 0; i < n; i++) {
        if (bit_get(col->nulls, i)) {
            dst[i] = M_NULL;
            continue;
        }

        int c;
        if (mixed_text) {
            c = (col->type == T_TEXT) ? 1 : -1;      /* 文本 > 数值 */
        } else if (text_cmp) {
            const u8 *s = col->tpool + col->toff[i];
            u16 sl = col->tlen[i];
            u16 m = sl < lit->len ? sl : lit->len;
            c = m ? memcmp(s, lit->s, m) : 0;
            if (c == 0)
                c = (sl == lit->len) ? 0 : (sl < lit->len ? -1 : 1);
        } else if (real_cmp) {
            double x = (col->type == T_REAL) ? col->reals[i]
                                              : (double)col->ints[i];
            double y = (lit->type == T_REAL) ? lit->d : (double)lit->i;
            c = (x < y) ? -1 : (x > y ? 1 : 0);
        } else {
            i64 x = col->ints[i];
            i64 y = lit->i;
            c = (x < y) ? -1 : (x > y ? 1 : 0);
        }

        bool r;
        switch (op) {
        case '=': r = (c == 0); break;
        case 'N': r = (c != 0); break;
        case '<': r = (c < 0);  break;
        case 'L': r = (c <= 0); break;
        case '>': r = (c > 0);  break;
        case 'G': r = (c >= 0); break;
        default:  r = false;    break;
        }
        dst[i] = r ? M_TRUE : M_FALSE;
    }
}

/* ---------------- 求值 ---------------- */

int eval_where(const Expr *e, BatchCtx *bc, const ColDesc *cols, int ncols,
               u8 *dst, int depth)
{
    if (!e) {
        for (u32 i = 0; i < bc->nrows; i++)
            dst[i] = M_TRUE;
        return OK;
    }

    switch (e->kind) {
    case EX_LIT:
        for (u32 i = 0; i < bc->nrows; i++)
            dst[i] = M_TRUE;     /* 常量当谓词：恒真（与逐行路径一致） */
        return OK;

    case EX_UN: {
        if (e->op == EX_OP_NOT) {
            int rc = eval_where(e->l, bc, cols, ncols, dst, depth + 1);
            if (rc != OK)
                return rc;
            for (u32 i = 0; i < bc->nrows; i++)
                dst[i] = (dst[i] == M_NULL) ? M_NULL
                     : (dst[i] == M_TRUE ? M_FALSE : M_TRUE);
            return OK;
        }
        if (e->op == EX_OP_ISNULL || e->op == EX_OP_ISNOTNULL) {
            const AmBatchCol *col = &bc->col[e->l->col_idx];
            bool want_null = (e->op == EX_OP_ISNULL);
            for (u32 i = 0; i < bc->nrows; i++) {
                bool isnull = bit_get(col->nulls, i);
                dst[i] = (isnull == want_null) ? M_TRUE : M_FALSE;
            }
            return OK;
        }
        return E_NOTSUP;
    }

    case EX_BIN: {
        if (e->op == EX_OP_AND || e->op == EX_OP_OR) {
            u8 *l = bc->marks + (size_t)(depth + 1) * VEC_BATCH;
            u8 *r = bc->marks + (size_t)(depth + 2) * VEC_BATCH;
            int rc = eval_where(e->l, bc, cols, ncols, l, depth + 1);
            if (rc != OK)
                return rc;
            rc = eval_where(e->r, bc, cols, ncols, r, depth + 2);
            if (rc != OK)
                return rc;
            for (u32 i = 0; i < bc->nrows; i++) {
                if (e->op == EX_OP_AND)
                    dst[i] = AND3[l[i]][r[i]];
                else
                    dst[i] = OR3[l[i]][r[i]];
            }
            return OK;
        }

        int c, op;
        if (!is_cmp_shape(e, cols, ncols, &c, &op))
            return E_NOTSUP;
        /* 常量在哪一边：形状检查已经确认了有一边是 EX_LIT */
        const DbValue *lit = (e->r && e->r->kind == EX_LIT) ? &e->r->lit
                                                           : &e->l->lit;
        cmp_col_lit(&bc->col[c], lit, op, dst, bc->nrows);
        return OK;
    }

    default:
        return E_NOTSUP;
    }
}

/* ---------------- 用到哪些列 ---------------- */

void collect_needed(const Expr *e, u16 *needed)
{
    if (!e)
        return;
    switch (e->kind) {
    case EX_COL:
        if (e->col_idx >= 0 && e->col_idx < SQL_MAX_COLS)
            *needed |= (u16)(1u << e->col_idx);
        break;
    case EX_UN:
        collect_needed(e->l, needed);
        break;
    case EX_BIN:
        collect_needed(e->l, needed);
        collect_needed(e->r, needed);
        break;
    default:
        break;
    }
}
