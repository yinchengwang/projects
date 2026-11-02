/*
 * exec_scan.c — 绑定 + 求值 + 表解析 + 谓词下推 + SELECT
 *
 * 【这个文件里没有"堆表"的知识】
 * 表是什么、能不能扫、可见性怎么判、谓词能不能下推 ——
 * 全都问 StorageAM。执行器只负责把 SQL 的东西翻译成接口的东西。
 */

#include "query/exec_cost.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 绑定：列名 → 列号 ---------------- */

int bind_expr(Expr *e, const ColDesc *cols, int ncols,
              char *err, int errcap)
{
    if (!e)
        return OK;

    if (e->kind == EX_COL) {
        for (int i = 0; i < ncols; i++) {
            if (strcmp(cols[i].name, e->name) == 0) {
                e->col_idx = i;
                return OK;
            }
        }
        snprintf(err, (size_t)errcap, "列不存在: %s", e->name);
        return E_NOTFOUND;
    }

    int rc = bind_expr(e->l, cols, ncols, err, errcap);
    if (rc != OK)
        return rc;
    return bind_expr(e->r, cols, ncols, err, errcap);
}

/* ---------------- 表达式求值 ---------------- */

int eval_expr(Expr *e, const DbValue *row, DbValue *out)
{
    DbValue a, b;

    switch (e->kind) {
    case EX_LIT:
        val_copy(out, &e->lit);
        return OK;

    case EX_COL:
        if (e->col_idx < 0)
            return E_INVAL;
        val_copy(out, &row[e->col_idx]);
        return OK;

    case EX_UN: {
        int rc = eval_expr(e->l, row, &a);
        if (rc != OK)
            return rc;

        if (e->op == EX_OP_NOT) {
            int t = val_truth(&a);
            if (t < 0) {
                val_null(out);
            } else {
                val_bool(out, t == 0);
            }
            return OK;
        }
        if (e->op == EX_OP_ISNULL) {
            val_bool(out, a.is_null);
            return OK;
        }
        if (e->op == EX_OP_ISNOTNULL) {
            val_bool(out, !a.is_null);
            return OK;
        }
        if (e->op == EX_OP_NEG) {
            if (a.is_null) {
                val_null(out);
            } else {
                /*
                 * 用无符号回绕，避免 -INT64_MIN 的 UB。
                 * （-INT64_MIN 仍然是 INT64_MIN —— 两补码回绕，
                 *   SQLite 在这里给的是实数，我们不跟这个细节。）
                 */
                val_int(out, (i64)(0u - (u64)a.i));
            }
            return OK;
        }
        return E_INVAL;
    }

    case EX_BIN: {
        /* AND / OR 先短路求值，三值逻辑 */
        if (e->op == EX_OP_AND || e->op == EX_OP_OR) {
            int rc = eval_expr(e->l, row, &a);
            if (rc != OK)
                return rc;
            int ta = val_truth(&a);

            if (e->op == EX_OP_AND && ta == 0) {      /* false AND x = false */
                val_bool(out, false);
                return OK;
            }
            if (e->op == EX_OP_OR && ta == 1) {       /* true OR x = true */
                val_bool(out, true);
                return OK;
            }

            rc = eval_expr(e->r, row, &b);
            if (rc != OK)
                return rc;
            int tb = val_truth(&b);

            if (e->op == EX_OP_AND) {
                if (tb == 0) { val_bool(out, false); return OK; }
                if (ta < 0 || tb < 0) { val_null(out); return OK; }
                val_bool(out, true);
                return OK;
            }
            /* OR */
            if (tb == 1) { val_bool(out, true); return OK; }
            if (ta < 0 || tb < 0) { val_null(out); return OK; }
            val_bool(out, false);
            return OK;
        }

        int rc = eval_expr(e->l, row, &a);
        if (rc != OK)
            return rc;
        rc = eval_expr(e->r, row, &b);
        if (rc != OK)
            return rc;

        /* 算术 */
        if (e->op == '+' || e->op == '-') {
            if (a.is_null || b.is_null) {
                val_null(out);
                return OK;
            }
            val_int(out, e->op == '+' ? a.i + b.i : a.i - b.i);
            return OK;
        }

        /* 比较 */
        {
            bool isnull;
            int cerr = OK;
            int c = val_cmp(&a, &b, &isnull, &cerr);
            if (cerr != OK)
                return cerr;          /* 外部值物化失败：明确报错，不给错答案 */
            if (isnull) {
                val_null(out);
                return OK;
            }
            bool r = false;
            switch (e->op) {
            case '=': r = (c == 0); break;
            case 'N': r = (c != 0); break;
            case '<': r = (c < 0);  break;
            case 'L': r = (c <= 0); break;
            case '>': r = (c > 0);  break;
            case 'G': r = (c >= 0); break;
            default: return E_INVAL;
            }
            val_bool(out, r);
            return OK;
        }
    }

    default:
        return E_INVAL;
    }
}

/* ---------------- 表解析 ---------------- */

/*
 * 解析表名。
 *
 * 顺序：**catalog 优先，注册表其次**。
 *
 * 【为什么用户表优先】
 * 用户显式 CREATE 出来的表应该赢。
 * 如果模态的虚拟表能覆盖同名用户表，用户会困惑
 * "我建的表怎么查不到我的列"。
 */
int resolve_table(Db *db, const char *name, TableCtx *tc,
                  char *err, int errcap)
{
    memset(tc, 0, sizeof(*tc));

    /* 1. 用户表（catalog） */
    u32 tid = 0;
    if (catalog_find_table(db->cat, name, &tid) == OK) {
        tc->tid = tid;
        tc->heap = catalog_heap(db->cat, tid);
        if (!tc->heap)
            return E_CORRUPT;

        int rc = catalog_table_cols(db->cat, tid, &tc->cols, &tc->ncols);
        if (rc != OK)
            return rc;

        const IndexInfo *idxs = NULL;
        int nidx = 0;
        rc = catalog_table_indexes(db->cat, tid, &idxs, &nidx);
        if (rc != OK)
            return rc;

        rc = heap_am_build(tc->heap, tc->cols, tc->ncols, idxs, nidx,
                           &tc->am_storage, &tc->am_ctx);
        if (rc != OK)
            return rc;

        tc->am = &tc->am_storage;
        tc->is_virtual = false;
        return OK;
    }

    /* 2. 虚拟表（模态提供的表） */
    StorageAM *am = spi_registry_find(db->reg, name);
    if (am) {
        tc->am = am;
        tc->is_virtual = true;
        tc->tid = 0;
        int rc = am->columns(am, &tc->cols, &tc->ncols);
        if (rc != OK)
            return rc;
        return OK;
    }

    snprintf(err, (size_t)errcap, "表不存在: %s", name);
    return E_NOTFOUND;
}

/* 按表 id 解析（只可能是用户表） */
int resolve_table_id(Db *db, u32 tid, TableCtx *tc)
{
    memset(tc, 0, sizeof(*tc));

    tc->tid = tid;
    tc->heap = catalog_heap(db->cat, tid);
    if (!tc->heap)
        return E_CORRUPT;

    int rc = catalog_table_cols(db->cat, tid, &tc->cols, &tc->ncols);
    if (rc != OK)
        return rc;

    const IndexInfo *idxs = NULL;
    int nidx = 0;
    rc = catalog_table_indexes(db->cat, tid, &idxs, &nidx);
    if (rc != OK)
        return rc;

    rc = heap_am_build(tc->heap, tc->cols, tc->ncols, idxs, nidx,
                       &tc->am_storage, &tc->am_ctx);
    if (rc != OK)
        return rc;

    tc->am = &tc->am_storage;
    return OK;
}

/* ---------------- 谓词 → 下推 ---------------- */

/*
 * 取常量值。
 * 注意 -10 在 AST 里是 EX_UN(NEG) 包着 EX_LIT(10)，不是纯字面量 ——
 * 只认 EX_LIT 的话，所有带负数的条件都会悄悄退回全表扫。
 */
static bool as_const(Expr *e, DbValue *out)
{
    if (e->kind == EX_LIT) {
        val_copy(out, &e->lit);
        return true;
    }
    if (e->kind == EX_UN && e->op == EX_OP_NEG && e->l && e->l->kind == EX_LIT) {
        if (e->l->lit.is_null)
            return false;
        val_int(out, -e->l->lit.i);
        return true;
    }
    return false;
}

int flip_op(int op)
{
    switch (op) {
    case '<': return '>';
    case '>': return '<';
    case 'L': return 'G';
    case 'G': return 'L';
    default:  return op;
    }
}

/*
 * 把一个叶子表达式翻译成 Predicate（列 vs 常量）。
 *
 * 【执行器只做翻译，不做判断】
 * "这个谓词能不能下推"由模态回答（am->can_push）。
 * 执行器不知道哪些列有索引、不知道 blob 能不能按属性过滤 ——
 * 那是模态的知识。
 */
static bool leaf_predicate(Expr *e, int ncols, Predicate *out)
{
    if (e->kind != EX_BIN)
        return false;
    if (e->op != '=' && e->op != '<' && e->op != '>' && e->op != 'L' && e->op != 'G')
        return false;

    Expr *col_e = NULL;
    DbValue cv;
    int op = e->op;

    if (e->l->kind == EX_COL && as_const(e->r, &cv)) {
        col_e = e->l;
    } else if (e->r->kind == EX_COL && as_const(e->l, &cv)) {
        col_e = e->r;
        op = flip_op(op);          /* 5 < id 等价于 id > 5 */
    } else {
        return false;
    }

    if (col_e->col_idx < 0 || col_e->col_idx >= ncols)
        return false;
    if (cv.is_null)
        return false;

    out->col_idx = col_e->col_idx;
    out->op = op;                 /* '=' '<' '>' 'L' 'G' 与 PD_* 编码一致 */
    val_copy(&out->val, &cv);
    return true;
}

/*
 * 在 AND 树里收集"能下推"的叶子谓词，挑**选择率最小**的那个。
 *
 * 【为什么不再"找到第一个就推"】
 * `WHERE a = 1 AND b > 900` 里两个谓词可能都能下推，
 * 但 `a = 1` 命中 1 行、`b > 900` 命中三分之一 ——
 * 推第一个就是白读 3 万行。挑最窄的那个才是"用索引"的本意。
 *
 * 【为什么只用 AND 树】
 * OR 的两个分支不能只推一个（推了会漏掉另一边的行）。
 * 所以只沿 AND 往下走，OR 整个不碰。
 */
static void collect_pushable(Expr *e, TableCtx *tc, i64 nrows,
                             Predicate *best, bool *have, double *best_sel)
{
    if (!e)
        return;

    if (e->kind == EX_BIN && e->op == EX_OP_AND) {
        collect_pushable(e->l, tc, nrows, best, have, best_sel);
        collect_pushable(e->r, tc, nrows, best, have, best_sel);
        return;
    }

    Predicate p;
    if (!leaf_predicate(e, tc->ncols, &p))
        return;

    /* 门槛：模态说"能不能"（能力协商，与代价无关） */
    if (tc->am->can_push(tc->am, &p) != OK)
        return;

    double sel = exec_cost_sel(p.op, nrows);
    if (!*have || sel < *best_sel) {
        *best = p;
        *best_sel = sel;
        *have = true;
    }
}

/*
 * 下推决策：门槛（能不能）+ 代价（值不值）。
 *
 * 【两层都要过】
 *   只有门槛没有代价 → 10 行的小表也走索引（比全扫还慢）
 *   只有代价没有门槛 → 对不支持的模态瞎指挥
 *
 * 【虚拟表为什么不做代价决策】
 * 模态提供的表（blob_frames / graph_nodes / vec_items）**没有 catalog 行**，
 * 也就没有 nrows_est。而它们的 push 是模态自己的优化 ——
 * 模态知道自己的数据长什么样，执行器不该替它算。
 * 所以：拿不到行数就退回"can_push 就推"的老行为 ✓
 */
/*
 * 下推计划：**EXPLAIN 与真正的执行共用这一个函数**。
 *
 * 【为什么必须共用】
 * 如果 EXPLAIN 自己算一遍、执行再算一遍，两份代码迟早会分叉 ——
 * 那时 EXPLAIN 说"走索引"、实际走了全扫，用户就再也不信它了。
 *
 * 一份实现，两个调用方：执行器（try_push）和 EXPLAIN（exec_explain.c）。
 */
int plan_push(Db *db, TableCtx *tc, Expr *where, PushPlan *out)
{
    memset(out, 0, sizeof(*out));

    if (!where || !tc->am->can_push || !tc->am->push)
        return E_NOTSUP;

    i64 nrows = 0;
    out->have_stats = (tc->tid != 0) &&
                      (catalog_nrows(db->cat, tc->tid, &nrows) == OK);
    out->nrows = nrows;

    bool have = false;
    double best_sel = 2.0;
    collect_pushable(where, tc, out->have_stats ? nrows : 1,
                     &out->pred, &have, &best_sel);

    if (!have) {
        out->why = "没有可下推的谓词";
        return E_NOTSUP;
    }

    out->sel = best_sel;

    if (out->have_stats) {
        exec_cost_decide(nrows, best_sel, &out->plan);
        out->pushed = out->plan.use_index;
        out->why = out->plan.why;
    } else {
        /*
         * 虚拟表（模态表）：没有 catalog 行，也就没有行数估算。
         * 它的 push 是模态自己的优化 —— 执行器不替它做代价决策。
         */
        out->pushed = true;
        out->why = "虚拟表：无行数估算，按模态自己的能力决定";
        out->plan.use_index = true;
        out->plan.cost_seq = 0.0;
        out->plan.cost_idx = 0.0;
    }
    return OK;
}

static bool try_push(Db *db, Expr *where, TableCtx *tc, AmScan **out)
{
    PushPlan plan;
    if (plan_push(db, tc, where, &plan) != OK || !plan.pushed)
        return false;

    if (tc->am->push(tc->am, &plan.pred, out) != OK)
        return false;

    return true;
}

/*
 * 打开扫描源：能下推就下推，否则全表扫。
 *
 * 【下推只是"少读一些行"，不是"替你判断"】
 * 执行器拿到行之后仍然完整求值一遍 WHERE。
 * 所以下推实现错了最多是慢，不会是错（除非漏行）。
 */
int open_scan(Db *db, TableCtx *tc, Expr *where, AmScan **out)
{
    AmScan *scan = NULL;
    if (try_push(db, where, tc, &scan)) {
        *out = scan;
        return OK;
    }
    return tc->am->scan_begin(tc->am, out);
}

/* ---------------- 可见性上下文 ---------------- */

/*
 * 可见性上下文每次现构造。
 *
 * 快照是连接私有的（db->snap），事务表是实例共享的（db->txns）。
 * 显式传给模态 —— 不存进任何共享对象（硬规则 5）。
 */
void make_vis_ctx(Db *db, AmVisCtx *vis)
{
    vis->snap = &db->snap;
    vis->txns = db->txns;
}

/* ---------------- SELECT ---------------- */

/*
 * 聚合列的名字：COUNT(*) / SUM(x)。
 *
 * 【为什么和 SQLite 一样】
 * SQLite 用表达式的形式当列名（COUNT(*)、SUM(x)）。
 * A3 要对照 SQLite —— 列名一致，输出才好逐格比。
 */
static void agg_colname(const Expr *e, char *buf, int cap)
{
    if (!e->l)
        snprintf(buf, (size_t)cap, "%s(*)", e->name);
    else if (e->l->kind == EX_COL)
        snprintf(buf, (size_t)cap, "%s(%s)", e->name, e->l->name);
    else
        snprintf(buf, (size_t)cap, "%s(expr)", e->name);
}

/*
 * 绑定 + 结果集外壳。行怎么产生交给 exec_select_run
 * （聚合 / 批式列求值 / 逐行，见 exec_agg.c 与 exec_batch.c）。
 */
int exec_select(Db *db, Stmt *st, QueryResult **out, char *err, int errcap)
{
    TableCtx tc;
    int rc = resolve_table(db, st->table, &tc, err, errcap);
    if (rc != OK)
        return rc;

    for (int i = 0; i < st->ntargets; i++) {
        rc = bind_expr(st->targets[i], tc.cols, tc.ncols, err, errcap);
        if (rc != OK)
            return rc;
    }
    if (st->where) {
        rc = bind_expr(st->where, tc.cols, tc.ncols, err, errcap);
        if (rc != OK)
            return rc;
    }
    /* GROUP BY 的列也要绑定（v1.1 步骤 3） */
    for (int i = 0; i < st->ngroupby; i++) {
        rc = bind_expr(st->groupby[i], tc.cols, tc.ncols, err, errcap);
        if (rc != OK)
            return rc;
    }

    int nout = st->star ? tc.ncols : st->ntargets;
    if (nout == 0)
        return E_INVAL;

    Builder b;
    rc = b_init(&b, nout, st->limit > 0 ? st->limit : 64);
    if (rc != OK) {
        qr_free(b.r);
        return rc;
    }

    for (int i = 0; i < nout; i++) {
        if (st->star)
            b.r->colnames[i] = xstrdup(tc.cols[i].name);
        else if (st->targets[i]->kind == EX_COL)
            b.r->colnames[i] = xstrdup(st->targets[i]->name);
        else if (st->targets[i]->kind == EX_AGG) {
            char nm[SQL_NAME_MAX * 2 + 16];
            agg_colname(st->targets[i], nm, (int)sizeof(nm));
            b.r->colnames[i] = xstrdup(nm);
        }
        else
            b.r->colnames[i] = xstrdup("expr");
    }

    rc = exec_select_run(db, st, &tc, &b, err, errcap);
    if (rc != OK) {
        qr_free(b.r);
        return rc;
    }

    *out = b.r;
    return OK;
}
