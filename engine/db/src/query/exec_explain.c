/*
 * exec_explain.c — EXPLAIN（v1.1 步骤 4）
 *
 * ==================== CBO 不做黑盒 ====================
 *
 * PLAN §1.4 的原话：
 *
 *   > 汇报：`EXPLAIN`（新增）：打印选中的路径与两个代价 ——
 *   > CBO 不做黑盒，每个决定可解释
 *
 * 一个"会自己改执行计划、但不告诉你为什么"的优化器，
 * 用户没法信任它 —— 出问题时只能靠猜。
 * EXPLAIN 的价值不在于好看，在于**让每个决定都能被追问**。
 *
 * ==================== 为什么必须和真正的执行共用逻辑 ====================
 *
 * 如果 EXPLAIN 自己算一遍代价、执行时再算一遍，两份代码迟早分叉：
 * 那时 EXPLAIN 说"走索引"、实际走了全扫，用户就再也不信它了。
 *
 * 所以这里调的是 `plan_push()` —— 与执行器**同一个函数**。
 * 这也是为什么 plan_push 被单独抽出来（见 exec_scan.c）。
 *
 * ==================== 输出格式 ====================
 *
 * 单列多行（像 PG 的 EXPLAIN）：
 *
 *   Seq Scan on bench_t
 *     rows_est = 100000
 *     Index Cond: (val >= 900)  ← 有可下推谓词时
 *     Cost: seq = 100000.00   idx = 40004.00   sel = 0.3333
 *     Decision: seq scan   (cost_idx >= cost_seq)
 */

#include "query/exec_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 谓词的操作符 → 文本 */
static const char *op_text(int op)
{
    switch (op) {
    case '=': return "=";
    case '<': return "<";
    case '>': return ">";
    case 'L': return "<=";
    case 'G': return ">=";
    default:  return "?";
    }
}

int exec_explain(Db *db, Stmt *st, QueryResult **out, char *err, int errcap)
{
    TableCtx tc;
    int rc = resolve_table(db, st->table, &tc, err, errcap);
    if (rc != OK)
        return rc;

    /*
     * 绑定：EXPLAIN 也要能报出"列不存在"这类错误 ——
     * 否则 EXPLAIN 通过、真正执行却失败，那 EXPLAIN 就没意义了。
     */
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
    for (int i = 0; i < st->ngroupby; i++) {
        rc = bind_expr(st->groupby[i], tc.cols, tc.ncols, err, errcap);
        if (rc != OK)
            return rc;
    }

    /*
     * 拿决策（与执行器同一个函数）。
     *
     * 【返回值可能是 E_NOTSUP 但统计仍然有效】
     * "没有可下推的谓词"（比如 WHERE 的列上没有索引）也算失败，
     * 但行数估算已经填好了 —— 那个数字对用户仍然有意义，要显示出来。
     */
    PushPlan plan;
    int prc = plan_push(db, &tc, st->where, &plan);
    bool have_plan = (prc == OK);       /* 有没有可下推的谓词 */
    bool have_stats = plan.have_stats;  /* 有没有行数估算 */

    /* ---- 拼输出 ---- */
    char lines[8][256];
    int n = 0;

    if (have_plan && plan.pushed) {
        snprintf(lines[n++], sizeof(lines[0]),
                 "Index Scan on %s  (列 %s)",
                 st->table, tc.cols[plan.pred.col_idx].name);
    } else {
        snprintf(lines[n++], sizeof(lines[0]), "Seq Scan on %s", st->table);
    }

    snprintf(lines[n++], sizeof(lines[0]), "  rows_est = %lld",
             (long long)(have_stats ? plan.nrows : 0));

    /* 可下推的谓词（不管最后推没推，都告诉你"找到了什么"） */
    if (have_plan && plan.pred.col_idx >= 0 &&
        plan.pred.col_idx < tc.ncols) {
        char vs[128];
        val_to_str(&plan.pred.val, vs, (int)sizeof(vs));
        snprintf(lines[n++], sizeof(lines[0]), "  Index Cond: (%s %s %s)",
                 tc.cols[plan.pred.col_idx].name,
                 op_text(plan.pred.op), vs);
    } else if (st->where) {
        snprintf(lines[n++], sizeof(lines[0]), "  Index Cond: (无)");
    }

    if (have_plan && have_stats) {
        snprintf(lines[n++], sizeof(lines[0]),
                 "  Cost: seq = %.2f   idx = %.2f   sel = %.4f",
                 plan.plan.cost_seq, plan.plan.cost_idx, plan.sel);
        snprintf(lines[n++], sizeof(lines[0]),
                 "  Decision: %s   (%s)",
                 plan.pushed ? "index scan" : "seq scan",
                 plan.why ? plan.why : "");
    } else if (have_stats) {
        /* 有行数但没有可用谓词 → 索引根本不参与竞争 */
        snprintf(lines[n++], sizeof(lines[0]),
                 "  Cost: seq = %.2f   (无可用谓词，索引不参与)",
                 (double)plan.nrows);
        snprintf(lines[n++], sizeof(lines[0]),
                 "  Decision: seq scan   (%s)",
                 plan.why ? plan.why : "没有可下推的谓词");
    } else {
        snprintf(lines[n++], sizeof(lines[0]),
                 "  Cost: (无统计信息 —— 虚拟表或空 catalog)");
        snprintf(lines[n++], sizeof(lines[0]),
                 "  Decision: %s   (%s)",
                 plan.pushed ? "index scan" : "seq scan",
                 plan.why ? plan.why : "不下推");
    }

    /* 聚合 / 分组：这是执行器的另一个算子，也一并交代 */
    if (stmt_has_agg(st)) {
        snprintf(lines[n++], sizeof(lines[0]),
                 "  Aggregate: yes   (GROUP BY %d 列)", st->ngroupby);
    }

    /* ---- 装进结果集（单列多行） ---- */
    Builder b;
    rc = b_init(&b, 1, n);
    if (rc != OK) {
        qr_free(b.r);
        return rc;
    }
    b.r->colnames[0] = xstrdup("QUERY PLAN");
    if (!b.r->colnames[0]) {
        qr_free(b.r);
        return E_IO;
    }

    for (int i = 0; i < n; i++) {
        DbValue v;
        val_text(&v, lines[i], (u16)strlen(lines[i]));
        rc = b_push(&b, &v);
        if (rc != OK) {
            qr_free(b.r);
            return rc;
        }
    }

    *out = b.r;
    return OK;
}
