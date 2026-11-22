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
    rc = bind_expr(e->r, cols, ncols, err, errcap);
    if (rc != OK)
        return rc;

    /*
     * EX_FUNC 的实参**必须递归绑定**。
     *
     * 不绑的后果：`json_match(doc, '$.a')` 里 `doc` 那一列的 col_idx
     * 停在初值 -1 ⇒ 求值时取不到值 ⇒ 结果静默错误（看着像"文档里没这个字段"）。
     * 这类错最难查，因为查询本身不报错。
     */
    if (e->kind == EX_FUNC) {
        /*
         * 顺便在这里检查函数是否已注册。
         *
         * 【为什么在绑定期而不是解析期】
         * 解析期查表会让 parser 依赖值层的注册表状态 —— 不必要的耦合。
         * 绑定期已经要拿 err 缓冲写列名错误了，顺手报"函数不存在"最自然。
         *
         * 【为什么不能只在求值期报】
         * 求值期拿不到 err 缓冲（签名是 (e, row, out)），错误信息只能退化成
         * 一个光秃秃的 E_NOTSUP —— 用户看不出是哪个函数没注册。
         * 而这条错误在**扫第一行之前**就能确定，本来就不该等到求值。
         */
        if (!val_func_find(e->name)) {
            snprintf(err, (size_t)errcap, "不支持的函数: %s", e->name);
            return E_NOTSUP;
        }
    }

    for (int i = 0; i < e->nargs; i++) {
        rc = bind_expr(e->args[i], cols, ncols, err, errcap);
        if (rc != OK)
            return rc;
    }
    return OK;
}

/* ---------------- 表达式求值 ---------------- */

/*
 * 通用标量函数调用的**公共求值体**。
 *
 * 【为什么抽成公共函数而不是两路各写一遍】
 * 执行器有两条平行的求值路径：逐行（`eval_expr`）与列式（`eval_where`）。
 * 两边都要支持 `EX_FUNC`。如果各写一遍，两份实现迟早会漂移 ——
 * 而 A3 的验收判据正是"两路全等"。抽到这里，两路调的是**同一份**代码，
 * 全等在结构上就成立了，不靠人对齐。
 *
 * 【为什么这里可以没有模态知识】
 * 函数名由 AST 带过来，实现从**值层的函数表**按名字查。
 * 执行器只做"查表 + 调用某个函数指针"，它不知道 json_match 是什么。
 * 这是 A7 审计清单里落点 ② 的全部内容。
 */
int eval_func_common(const Expr *e, const DbValue *row, DbValue *out)
{
    DbValue args[EX_FUNC_MAX_ARGS];

    if (!e || e->kind != EX_FUNC || !out)
        return E_INVAL;
    if (e->nargs > EX_FUNC_MAX_ARGS)
        return E_NOSPACE;

    for (int i = 0; i < e->nargs; i++) {
        int rc = eval_expr(e->args[i], row, &args[i]);
        if (rc != OK)
            return rc;
    }

    ValFunc fn = val_func_find(e->name);
    if (!fn)
        return E_NOTSUP;     /* 函数没注册 —— 明确报错，不静默给 0 */

    return fn(args, e->nargs, out);
}

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

    case EX_FUNC:
        return eval_func_common(e, row, out);

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
 *
 * 【为什么改成"按顺序问 opener"】
 * 旧实现这里自己分两支：用户表那一支直接去要堆句柄并现搭 AM ——
 * **执行器认得堆**，那是硬规则 7 的破窗。
 * 现在执行器只知道"有两个 opener，谁先答上就用谁"，
 * 它既不知道用户表那一侧用的是堆，也不知道虚拟表那一侧是谁注册的。
 */
int resolve_table(Db *db, const char *name, TableCtx *tc,
                  char *err, int errcap)
{
    memset(tc, 0, sizeof(*tc));

    const TableOpener *ops[2];
    void *owners[2];
    ops[0]    = db->opener_catalog;
    owners[0] = (void *)db->cat;
    ops[1]    = db->opener_virtual;
    owners[1] = (void *)db->reg;

    for (int i = 0; i < 2; i++) {
        if (!ops[i])
            continue;
        int rc = ops[i]->open(ops[i], owners[i], name, &tc->open_result,
                              tc->cols_buf, tc->idxs_buf,
                              tc->priv, sizeof(tc->priv));
        if (rc == OK) {
            tc->am         = tc->open_result.am;
            tc->cols       = tc->open_result.cols;
            tc->ncols      = tc->open_result.ncols;
            tc->idxs       = tc->open_result.idxs;
            tc->nidx       = tc->open_result.nidx;
            tc->tid        = tc->open_result.tid;
            tc->is_virtual = tc->open_result.is_virtual;
            return OK;
        }
        /* E_NOTFOUND = "我没这张表" ⇒ 去问下一个；其它错误原样透传 */
        if (rc != E_NOTFOUND)
            return rc;
    }

    snprintf(err, (size_t)errcap, "表不存在: %s", name);
    return E_NOTFOUND;
}

/* 按表 id 解析（只可能是用户表） */
int resolve_table_id(Db *db, u32 tid, TableCtx *tc)
{
    memset(tc, 0, sizeof(*tc));

    if (!db->opener_catalog)
        return E_INVAL;

    int rc = db->opener_catalog->open_by_id(db->opener_catalog, (void *)db->cat,
                                            tid, &tc->open_result,
                                            tc->cols_buf, tc->idxs_buf,
                                            tc->priv, sizeof(tc->priv));
    if (rc != OK)
        return rc;

    tc->am         = tc->open_result.am;
    tc->cols       = tc->open_result.cols;
    tc->ncols      = tc->open_result.ncols;
    tc->idxs       = tc->open_result.idxs;
    tc->nidx       = tc->open_result.nidx;
    tc->tid        = tc->open_result.tid;
    tc->is_virtual = tc->open_result.is_virtual;
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
 * 在 AND 树里收集**所有**能下推的叶子谓词（按出现顺序，最多 PD_CONJ_MAX 条）。
 *
 * 【为什么收集成一组，而不是挑一个】
 *
 * 第一版只挑选择率最小的**一个**。它对 `WHERE a = 1 AND b > 900` 是对的选择
 *（`a = 1` 命中 1 行，推 `b > 900` 就是白读 3 万行）。
 *
 * 但它在**双边界范围**上是错的：`WHERE t >= a AND t < b` 里两条各占一边，
 * 挑任何**一条**都只能削掉一半 —— 而两条一起才能把范围收成真正的区间。
 * 实测（ts，3 个分区）：`>= 第1天` 读 6 页，`>= 第1天 AND < 第2天` **也读 6 页**，
 * 因为推的是 `>= 第1天`（全覆盖）。两条一起推才读到 4 页。
 *
 * ⇒ 所以这里把**全部**能推的叶子收上来，交给模态自己决定吃几条。
 * 挑最窄的那条退化成"模态只吃一条时"的兜底（见 plan_push）。
 *
 * 【为什么只用 AND 树】
 * OR 的两个分支不能只推一个（推了会漏掉另一边的行）。
 * 所以只沿 AND 往下走，OR 整个不碰。
 *
 * 【超过 PD_CONJ_MAX 怎么办】
 * 只收前 PD_CONJ_MAX 条，剩下的丢掉 —— **丢是安全的**：
 * 合取取子集得到的是超集，执行器反正会完整求值 WHERE。
 * （反过来"为了凑数而截断语义"才危险，那会让人以为推了其实没推。）
 */
static u32 collect_pushable(Expr *e, TableCtx *tc, Predicate *out, u32 cap)
{
    u32 n = 0;

    /* 用显式栈做中序遍历，避免递归时把 n 传丢 */
    Expr *stack[32];
    int sp = 0;

    if (!e)
        return 0;
    stack[sp++] = e;

    while (sp > 0) {
        Expr *cur = stack[--sp];

        if (!cur)
            continue;

        if (cur->kind == EX_BIN && cur->op == EX_OP_AND) {
            /* 先压右再压左 ⇒ 弹出顺序与源码书写顺序一致 */
            if (sp + 2 <= (int)(sizeof(stack) / sizeof(stack[0]))) {
                stack[sp++] = cur->r;
                stack[sp++] = cur->l;
            }
            continue;
        }

        if (n >= cap)
            continue;               /* 收满了：剩下的丢掉（安全方向） */

        Predicate p;
        if (!leaf_predicate(cur, tc->ncols, &p))
            continue;

        /*
         * 门槛：模态说"能不能"（能力协商，与代价无关）。
         *
         * 这里问的是**单条**能不能 —— 用来筛掉本模态完全不认识的列/算子。
         * 整组能不能吃由 plan_push 再问一次（can_push 的 n 参数）。
         */
        if (tc->am->can_push(tc->am, &p, 1u) != OK)
            continue;

        out[n++] = p;
    }

    return n;
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

    Predicate cand[PD_CONJ_MAX];
    i64 sel_rows = out->have_stats ? nrows : 1;
    u32 ncand = collect_pushable(where, tc, cand, PD_CONJ_MAX);

    if (ncand == 0u) {
        out->why = "没有可下推的谓词";
        return E_NOTSUP;
    }

    /*
     * 先问模态：**整组**吃得下吗？
     *
     * 吃得下就用整组（双边界范围只有这样才收成一个区间）。
     * 吃不下就退回老行为：逐条按选择率挑最窄的一条。
     * 两条路都不会漏行 —— 执行器永远完整求值 WHERE。
     */
    if (tc->am->can_push(tc->am, cand, ncand) == OK) {
        memcpy(out->conj, cand, (size_t)ncand * sizeof(Predicate));
        out->nconj = ncand;

        /*
         * 合并选择率：按**独立假设**取连乘（教科书口径）。
         *
         * ⚠️ 同一列上的双边界范围会被低估（两个各 1/3 连乘成 1/9，
         * 而实际常是 1/3）。低估的方向是"更倾向走索引" —— 那是**保守**的
         * 一侧：可能多走一次索引，不会算错结果。反过来（高估）会让本该
         * 走索引的查询退回全扫，只是慢；也不会错。两边都不影响正确性，
         * 所以这里不为了好看去编一个系数。
         */
        double sel = 1.0;
        for (u32 i = 0; i < ncand; i++)
            sel *= exec_cost_sel(out->conj[i].op, sel_rows);
        out->sel = sel;
    } else {
        u32 best = 0;
        double best_sel = exec_cost_sel(cand[0].op, sel_rows);

        for (u32 i = 1; i < ncand; i++) {
            double s = exec_cost_sel(cand[i].op, sel_rows);
            if (s < best_sel) {
                best_sel = s;
                best = i;
            }
        }
        memcpy(&out->conj[0], &cand[best], sizeof(Predicate));
        out->nconj = 1u;
        out->sel = best_sel;
    }

    if (out->have_stats) {
        exec_cost_decide(nrows, out->sel, &out->plan);
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

    if (tc->am->push(tc->am, plan.conj, plan.nconj, out) != OK)
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
