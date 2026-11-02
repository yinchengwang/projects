/*
 * exec_volcano.c — 火山模型执行器（v1.1 改造前的行式路径，冻结副本）
 *
 * ==================== 为什么这个文件存在 ====================
 *
 * v1.1 的核心验收 A1 是「**新旧执行器结果逐行全等**」。
 * 这条验收要成立，必须有一个**改造前**的执行器在旁边对照。
 *
 * 项目没有 git（`fatal: not a git repository`），
 * 所以"留底一个 tag"这条 PLAN 里的原话没法用 —— 只能留**源码副本**。
 * 这个文件就是那份副本：它是 v1.1 开工前 `exec_scan.c` 里
 * `exec_select` 的原样搬家，**一行逻辑都没改**。
 *
 * ==================== 为什么它住在 bench/ 而不是 src/ ====================
 *
 * PLAN §1.6 R5 写得很明确：
 *
 *   > 对拍只在 bench 里做（编译期开关），旧路径不进主构建 ——
 *   > 对拍完成后即删，不留两套执行器
 *
 * 两句都要满足：
 *   1. 旧路径**不进主构建** —— 它在 bench/ 目录，主二进制（db.exe）
 *      只链接 src/ 下的对象，这个文件自然不在里面。
 *   2. 对拍只在 bench 里做 —— exec_bench 同时链上主库和新旧两条路径，
 *      在**同一个进程、同一个库**上跑同一批 SQL，逐格比对。
 *
 * 【为什么"同进程"比"两个二进制"更严】
 *
 * 两个二进制分别跑再比输出文件，看起来一样，但中间隔着：
 * 两次进程启动、两次 WAL 恢复、两次页缓存冷启动。
 * 任何一处不确定（比如 buffer pool 里残留的脏页）都可能让两次跑出不同结果，
 * 而差异会被误判成"执行器不一致"。
 *
 * 同进程比对没有这些中间变量：同一个 Db 句柄、同一个缓冲池、
 * 同一份快照。它测的**只有**"两条执行路径的结果是否一样"这一件事。
 *
 * ==================== 这个文件不参与主构建 ====================
 *
 * Makefile 的 bench 目标会单独把它编成 build/bench/exec_volcano.o，
 * 只链进 exec_bench。test/run.sh 排除 build/bench 目录下的所有目标文件，
 * 所以测试也不会链到它 —— 主二进制里**不存在**第二套执行器。
 *
 * 【冻结的纪律】
 *
 * 这个文件一旦落地就**不再改**。它是"改造前"的定义，
 * 改了它，对拍就变成"新的和新的比"，什么也证明不了。
 * 语义修正确实要改的地方，改在 src/ 侧的共享辅助函数里
 * （eval_expr / val_cmp 那一层），两边同时受益。
 */

#include "query/exec_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 输出一行（火山版：逐行求值） ---------------- */

/*
 * 与 v1.1 开工前 exec_scan.c 里的 emit_row 完全一致。
 *
 * 【LIMIT 的位置很关键】
 * limit 检查在投影**之前**：limit=0 时一行都不吐。
 * 批式路径必须照抄这个语义，否则 LIMIT 0 的行为会分叉。
 */
static int emit_row_volcano(Stmt *st, const TableCtx *tc, Builder *b,
                            const DbValue *row, int *stop)
{
    *stop = 0;

    if (st->where) {
        DbValue w;
        int rc = eval_expr(st->where, row, &w);
        if (rc != OK)
            return rc;
        if (val_truth(&w) != 1)          /* FALSE 和 NULL 都不匹配 */
            return OK;
    }

    /* LIMIT 检查放在投影之前：limit=0 时一行都不吐 */
    if (st->limit >= 0 && b->r->nrows >= st->limit) {
        *stop = 1;
        return OK;
    }

    DbValue proj[SQL_MAX_COLS];
    if (st->star) {
        memcpy(proj, row, sizeof(DbValue) * (size_t)tc->ncols);
    } else {
        for (int i = 0; i < st->ntargets; i++) {
            int rc = eval_expr(st->targets[i], row, &proj[i]);
            if (rc != OK)
                return rc;
        }
    }
    return b_push(b, proj);
}

/* ---------------- SELECT（火山版：逐行 next） ---------------- */

int exec_select_volcano(Db *db, Stmt *st, QueryResult **out, char *err, int errcap)
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
        else
            b.r->colnames[i] = xstrdup("expr");
    }

    AmScan *scan = NULL;
    /*
     * 【v1.1 步骤 4 的接口适配】
     * open_scan 多了一个 Db* 参数（CBO 要读 catalog 的 nrows_est）。
     * 这是**签名适配**，不是逻辑修改 —— 冻结副本的行产生逻辑一行没动。
     *
     * 对拍仍然有意义：两边共用 open_scan，所以"选哪条扫描路径"的决策
     * 是一致的；对拍比的是"行怎么产生"（火山逐行 vs 列批）。
     */
    rc = open_scan(db, &tc, st->where, &scan);
    if (rc != OK) {
        qr_free(b.r);
        return rc;
    }

    AmVisCtx vis;
    make_vis_ctx(db, &vis);

    u8 recbuf[PAGE_SIZE];
    u16 reclen;
    DbValue row[SQL_MAX_COLS];
    Rid rid;

    while ((rc = tc.am->scan_next(tc.am, scan, &rid,
                                  recbuf, sizeof(recbuf), &reclen)) == OK) {
        /* 可见性由模态判断（heap 走 MVCC，对象存储恒可见） */
        if (!tc.am->visible(tc.am, recbuf, reclen, &vis))
            continue;
        if (tc.am->decode(tc.am, recbuf, reclen, row) != OK)
            continue;

        int stop = 0;
        rc = emit_row_volcano(st, &tc, &b, row, &stop);
        if (rc != OK || stop)
            break;
    }
    tc.am->scan_end(tc.am, scan);

    if (rc == E_EOF)
        rc = OK;
    if (rc != OK) {
        qr_free(b.r);
        return rc;
    }

    *out = b.r;
    return OK;
}
