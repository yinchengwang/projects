/*
 * exec_batch.c — 批式（列式）执行路径（v1.1 步骤 2）
 *
 * ==================== 火山 vs 批式 ====================
 *
 * 火山（一行一行）：
 *     scan_next → 求值 WHERE → 求值投影 → 塞结果集
 *
 * 批式（PLAN §1.2）：
 *     攒够 VEC_BATCH 行 → 按列求值 WHERE（选择向量）→ 按列投影 → 批进结果集
 *
 * ==================== 这个文件干什么 ====================
 *
 *   - 攒批：逐行取 → 可见性 → 解码 → **转置**进列批
 *   - 投影：从列批还原成 DbValue，推进结果集
 *   - 逐行退路：列式不支持的形状走这里
 *
 * 列式求值本身在 exec_eval.c，批的数据结构在 exec_batch.h。
 *
 * ==================== 结果必须逐行全等（A1 的门禁） ====================
 *
 * 攒批不改变行的物理顺序，选择向量也不改变行的顺序 ——
 * 所以批式与火山的果集必须**逐格全等**。三条纪律见 exec_eval.c 顶部。
 *
 * ==================== 这一版能拿到多少 ====================
 *
 * 省的是**求值与投影**的逐行开销，不是扫描与解码 ——
 * 后两者由 SPI 的逐行接口决定（PLAN §1.2 的判决：攒批在执行器内部）。
 * 实测扫描 + 解码占整条查询的 81%，所以单靠批式求值拿不到 3 倍；
 * 详见 docs/PLAN-v1.1.md §3 的 R2 判定（SPI 批接口列入 v1.2）。
 *
 * ==================== v1.2 步骤 0：攒批改走 SPI 的批接口 ====================
 *
 * 本文件原来只能"一行一行问 SPI 要"（scan_next + decode），
 * 然后自己转置成列批 —— 这正是 v1.1 只拿到 1.11× 的原因：
 * 81% 的时间花在**每行一次的 SPI 调用**和**每行一次的 DbValue 物化**上。
 *
 * 现在多了一条路：模态声明了 AM_CAP_BATCH_SCAN / AM_CAP_BATCH_DECODE 时，
 * 攒批改成"一次要一批原始行 + 一次解一批，直接进列数组"。
 *
 * 【两种路径必须给出完全一样的结果】
 * 批式与火山的果集必须**逐格全等**（A1 门禁）。
 * 两条路径共用同一套列数组、同一个 batch_cell、同一个投影逻辑 ——
 * 差别只在于"列数组里的数是谁填的"。
 */

#include "query/exec_batch.h"

#include <stdlib.h>
#include <string.h>

/* ---------------- 小工具 ---------------- */

static void *xcalloc(size_t n)
{
    void *p = calloc(n ? n : 1, 1);
    return p;
}

/* ---------------- 生命周期 ---------------- */

void batch_ctx_free(BatchCtx *bc)
{
    for (int c = 0; c < bc->ncols; c++) {
        AmBatchCol *col = &bc->col[c];
        free(col->nulls);
        free(col->ints);
        free(col->reals);
        free(col->toff);
        free(col->tlen);
        free(col->tpool);
        free(col->extflag);
        free(col->exts);
    }
    free(bc->marks);
    free(bc->sel);
    free(bc->refs);
    memset(bc, 0, sizeof(*bc));
}

/*
 * 只给"用得上的列"建数组。
 *
 * 【为什么要挑列】
 * 批里每列的数组都要按 VEC_BATCH 开（INT 列 8 KB，TEXT 列还要一个文本池）。
 * 而一条查询通常只碰其中几列 ——
 * `SELECT id, grp FROM t WHERE val >= 900` 只需要 3 列，
 * 第四列（TEXT）根本不用进批：省掉它的数组分配，也省掉每行拷贝它的文本。
 *
 * needed 是列号的位图（ncols ≤ 16，u16 够用）。
 */
int batch_ctx_init(BatchCtx *bc, const ColDesc *cols, int ncols, u16 needed)
{
    memset(bc, 0, sizeof(*bc));
    bc->ncols = ncols;

    for (int c = 0; c < ncols; c++) {
        AmBatchCol *col = &bc->col[c];
        col->type = cols[c].type;
        col->used = ((needed >> c) & 1u) != 0;
        if (!col->used)
            continue;

        col->nulls = xcalloc((VEC_BATCH + 7u) / 8u);
        col->extflag = xcalloc((VEC_BATCH + 7u) / 8u);
        if (!col->nulls || !col->extflag)
            return E_IO;

        switch (cols[c].type) {
        case T_REAL:
            col->reals = xcalloc(sizeof(double) * VEC_BATCH);
            if (!col->reals)
                return E_IO;
            break;
        case T_TEXT:
            col->toff = xcalloc(sizeof(u32) * VEC_BATCH);
            col->tlen = xcalloc(sizeof(u16) * VEC_BATCH);
            /* 文本池按最坏情况给：每行 VAL_TEXT_MAX-1 字节 */
            col->tcap = (u32)VEC_BATCH * VAL_TEXT_MAX;
            col->tpool = xcalloc(col->tcap);
            if (!col->toff || !col->tlen || !col->tpool)
                return E_IO;
            break;
        default:
            col->ints = xcalloc(sizeof(i64) * VEC_BATCH);
            if (!col->ints)
                return E_IO;
            break;
        }
    }

    /* 标记层栈：一次分配，按层切片 */
    bc->marks = xcalloc((size_t)MARK_DEPTH * VEC_BATCH);
    bc->sel = xcalloc(sizeof(u16) * VEC_BATCH);
    if (!bc->marks || !bc->sel)
        return E_IO;

    /*
     * 批扫描的行引用缓冲（v1.2 步骤 0）。
     *
     * 一次给 AM 一批原始行 —— AM 把行字节码进自己的竞技场，
     * 这里只存"指向竞技场里的哪一段"。用完立刻失效，
     * 所以不需要在批之间保留。
     */
    bc->refs = xcalloc(sizeof(AmRowRef) * AM_BATCH_MAX_ROWS);
    if (!bc->refs)
        return E_IO;

    return OK;
}

/* 一批用完了：把行数与文本池复位（位图要清，否则上一批的 NULL 会残留） */
void batch_reset(BatchCtx *bc)
{
    bc->nrows = 0;
    for (int c = 0; c < bc->ncols; c++) {
        AmBatchCol *col = &bc->col[c];
        if (!col->used)
            continue;
        col->n = 0;
        col->tused = 0;
        memset(col->nulls, 0, (VEC_BATCH + 7u) / 8u);
        memset(col->extflag, 0, (VEC_BATCH + 7u) / 8u);
    }
}

/* ---------------- 填充：把一行转置进列批 ---------------- */

int batch_add_row(BatchCtx *bc, const DbValue *row)
{
    u32 i = bc->nrows;

    for (int c = 0; c < bc->ncols; c++) {
        AmBatchCol *col = &bc->col[c];
        const DbValue *v = &row[c];

        if (!col->used)
            continue;                   /* 这一列不进批 */

        if (v->is_null) {
            bit_set(col->nulls, i);
            continue;
        }

        if (v->external) {
            if (!col->exts) {
                col->exts = xcalloc(sizeof(AmExtRef) * VEC_BATCH);
                if (!col->exts)
                    return E_IO;
            }
            col->exts[i].key = v->ext_key;
            col->exts[i].off = v->ext_off;
            col->exts[i].fn = v->ext_fn;
            col->exts[i].ctx = v->ext_ctx;
            bit_set(col->extflag, i);
            continue;
        }

        switch (col->type) {
        case T_REAL:
            col->reals[i] = v->d;
            break;
        case T_TEXT: {
            u16 n = v->len;
            if ((u32)col->tused + n > col->tcap)
                return E_NOSPACE;
            if (n)
                memcpy(col->tpool + col->tused, v->s, n);
            col->toff[i] = col->tused;
            col->tlen[i] = n;
            col->tused = (u32)col->tused + n;
            break;
        }
        default:
            col->ints[i] = v->i;
            break;
        }
    }

    bc->nrows++;
    return OK;
}

/*
 * 取批里第 i 行第 c 列的值，还原成 DbValue。
 *
 * 【必须和 val_from_record 产出的完全一样】
 * 逐行路径的投影直接用解码出来的 DbValue；批式路径是重新还原的 ——
 * 两边不一致的话，对拍会红。
 */
void batch_cell(const BatchCtx *bc, int c, u32 i, DbValue *out)
{
    const AmBatchCol *col = &bc->col[c];

    if (bit_get(col->nulls, i)) {
        val_null(out);
        return;
    }
    if (bit_get(col->extflag, i)) {
        const AmExtRef *e = &col->exts[i];
        val_external_text(out, e->key, e->off, e->fn, e->ctx);
        return;
    }

    switch (col->type) {
    case T_REAL:
        val_real(out, col->reals[i]);
        break;
    case T_TEXT:
        val_text(out, (const char *)col->tpool + col->toff[i], col->tlen[i]);
        break;
    case T_BOOL:
        val_bool(out, col->ints[i] != 0);
        break;
    default:
        val_int(out, col->ints[i]);
        break;
    }
}

/* ---------------- 投影 ---------------- */

/*
 * 把这一批里选中的行投影进结果集。
 *
 * 【LIMIT 的位置】和逐行路径一样：**先查 LIMIT 再投影** ——
 * limit = 0 时一行都不吐。位置不一样结果就不一样。
 */
static int project_batch(BatchCtx *bc, const Stmt *st, const TableCtx *tc,
                         Builder *b, const u16 *sel, int nsel)
{
    int nout = st->star ? tc->ncols : st->ntargets;

    for (int k = 0; k < nsel; k++) {
        if (st->limit >= 0 && b->r->nrows >= st->limit)
            break;                          /* 与逐行路径同位置 */

        u32 i = sel[k];
        DbValue proj[SQL_MAX_COLS];

        for (int j = 0; j < nout; j++) {
            int c = st->star ? j : st->targets[j]->col_idx;
            batch_cell(bc, c, i, &proj[j]);
        }

        int rc = b_push(b, proj);
        if (rc != OK)
            return rc;
    }
    return OK;
}

/* ---------------- 批式主流程 ---------------- */

/*
 * 这个模态支持批接口吗？
 *
 * 【能力协商】（见 spi/storage_am.h）
 * caps 位 + 函数指针非 NULL，两个条件都满足才算支持。
 *
 * blob / graph / vector 三个模态的这两个指针保持 NULL ——
 * 它们自动走逐行路径，一行都不用改。
 */
static bool am_supports_batch(const StorageAM *am)
{
    if (!am)
        return false;
    if ((am->caps & (AM_CAP_BATCH_SCAN | AM_CAP_BATCH_DECODE)) !=
        (AM_CAP_BATCH_SCAN | AM_CAP_BATCH_DECODE))
        return false;
    return am->batch_scan != NULL && am->batch_decode != NULL;
}

/*
 * 攒批（批接口路径）：一次要一批原始行 → 可见性过滤 → 一次解进列数组。
 *
 * ==================== 为什么这样能快 ====================
 *
 * 逐行路径每行两次 SPI 调用（scan_next + decode），外加一次 272 字节
 * DbValue 的物化。批路径把 N 行压成 2 次调用，而且**根本没有 DbValue** ——
 * 解码直接写进列数组。
 *
 * ==================== 可见性过滤放在哪 ====================
 *
 * 解码之前。不可见的行没必要解 —— 批路径白拿的一笔。
 *
 * ==================== 就地压缩 ====================
 *
 * 可见的行往前挪（refs[nvis++] = refs[i]），不额外开缓冲。
 */
static int fill_batch_via_am(const TableCtx *tc, BatchCtx *bc, AmScan *scan,
                             const AmVisCtx *vis, bool *eof)
{
    *eof = false;

    int n = 0;
    int rc = tc->am->batch_scan(tc->am, scan, bc->refs, AM_BATCH_MAX_ROWS, &n);
    if (rc == E_EOF) {
        *eof = true;
        return OK;
    }
    if (rc != OK)
        return rc;
    if (n <= 0)
        return OK;

    /* 可见性过滤：就地压缩 */
    int nvis = 0;
    for (int i = 0; i < n; i++) {
        if (tc->am->visible(tc->am, bc->refs[i].data, bc->refs[i].len, vis))
            bc->refs[nvis++] = bc->refs[i];
    }
    if (nvis == 0)
        return OK;

    /* 一次解一批，直接进列数组（写游标从 0 开始） */
    AmBatch ab;
    ab.ncols = bc->ncols;
    ab.nrows = 0;
    ab.cols = bc->col;

    rc = tc->am->batch_decode(tc->am, bc->refs, nvis, &ab);
    if (rc != OK)
        return rc;

    bc->nrows = ab.nrows;
    return OK;
}

/*
 * 攒批（逐行退路）：一行一行问 SPI 要 → 可见性 → 解码 → 转置进列批。
 *
 * ==================== 为什么还留着它 ====================
 *
 * 模态没声明批能力（blob / graph / vector），或者还没实现批解码。
 *
 * ==================== 语义只有一处实现 ====================
 *
 * 两条路径最后都填同一套列数组、同一套投影逻辑 ——
 * 差别只在于"列数组里的数是谁填的"。
 */
static int fill_batch_rowwise(const TableCtx *tc, BatchCtx *bc, AmScan *scan,
                              const AmVisCtx *vis, bool *eof)
{
    *eof = false;

    u8 recbuf[PAGE_SIZE];
    u16 reclen;
    DbValue row[SQL_MAX_COLS];
    Rid rid;

    for (;;) {
        int rc = tc->am->scan_next(tc->am, scan, &rid,
                                   recbuf, sizeof(recbuf), &reclen);
        if (rc == E_EOF) {
            *eof = true;
            break;
        }
        if (rc != OK)
            return rc;

        /* 可见性由模态判断（heap 走 MVCC，对象存储恒可见） */
        if (!tc->am->visible(tc->am, recbuf, reclen, vis))
            continue;
        if (tc->am->decode(tc->am, recbuf, reclen, row) != OK)
            continue;

        int arc = batch_add_row(bc, row);
        if (arc != OK)
            return arc;

        if (bc->nrows >= VEC_BATCH)
            break;
    }
    return OK;
}

static int exec_select_batch(Db *db, Stmt *st, TableCtx *tc, Builder *b)
{
    /* 只把用得上的列放进批 */
    u16 needed = 0;
    if (st->star) {
        for (int c = 0; c < tc->ncols && c < SQL_MAX_COLS; c++)
            needed |= (u16)(1u << c);
    } else {
        for (int i = 0; i < st->ntargets; i++)
            collect_needed(st->targets[i], &needed);
    }
    collect_needed(st->where, &needed);

    BatchCtx bc;
    int rc = batch_ctx_init(&bc, tc->cols, tc->ncols, needed);
    if (rc != OK) {
        batch_ctx_free(&bc);
        return rc;
    }

    AmScan *scan = NULL;
    rc = open_scan(db, tc, st->where, &scan);
    if (rc != OK) {
        batch_ctx_free(&bc);
        return rc;
    }

    AmVisCtx vis;
    make_vis_ctx(db, &vis);

    /* 模态支持批接口就走批路径，否则退回逐行 */
    const bool use_batch = am_supports_batch(tc->am);

    for (;;) {
        batch_reset(&bc);

        /* 攒批 */
        bool eof = false;
        int frc = use_batch
                  ? fill_batch_via_am(tc, &bc, scan, &vis, &eof)
                  : fill_batch_rowwise(tc, &bc, scan, &vis, &eof);

        /*
         * 一批里可能**一行都不剩**（全是不可见的版本）。
         *
         * 【这里为什么不能因为"本批 0 行"就收工】
         * 批路径一次只取一页的行，而一页里的行可能恰好全都不可见
         * （MVCC：另一个事务插了还没提交 / 已删除已提交）。
         * 此时后面还有页、还有可见的行 —— 收工就等于把结果截断了。
         *
         * 逐行路径没有这个问题：它一直取到 EOF 才会返回 0 行。
         * 批路径必须靠 eof 判断，不靠 nrows 判断。
         */
        if (bc.nrows > 0) {
            /* 列式求值 WHERE → 选择向量 */
            u8 *marks = bc.marks;
            int erc = eval_where(st->where, &bc, tc->cols, tc->ncols, marks, 0);
            if (erc != OK) {
                tc->am->scan_end(tc->am, scan);
                batch_ctx_free(&bc);
                return erc;
            }

            int nsel = 0;
            for (u32 i = 0; i < bc.nrows; i++) {
                if (marks[i] == M_TRUE)
                    bc.sel[nsel++] = (u16)i;
            }

            /* 投影 */
            rc = project_batch(&bc, st, tc, b, bc.sel, nsel);
            if (rc != OK) {
                tc->am->scan_end(tc->am, scan);
                batch_ctx_free(&bc);
                return rc;
            }
        }

        /* 扫描中途出的真错误：这一批已经吐完了，接着报 */
        if (frc != OK) {
            tc->am->scan_end(tc->am, scan);
            batch_ctx_free(&bc);
            return frc;
        }

        if (eof)
            break;
    }

    tc->am->scan_end(tc->am, scan);
    batch_ctx_free(&bc);
    return OK;
}

/* ---------------- 逐行路径（火山模型） ---------------- */

/*
 * 【为什么还要留着它】
 * 批式只覆盖"能列式求值"的形状；其余（表达式投影、TEXT 列的谓词）走这条路。
 *
 * 聚合（步骤 3）有自己的路径（exec_agg.c），但它的 WHERE 也分两走：
 * 能列式就列式，不能就落到这里用的 eval_expr —— 语义只有一处实现。
 *
 * 两条路径的结果必须逐格全等 —— 它们共用 eval_expr / val_cmp，
 * 所以语义只有一处实现。
 */

/* *stop = 1 表示已达 LIMIT */
static int emit_row_rowwise(Stmt *st, const TableCtx *tc, Builder *b,
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

static int exec_select_rowwise(Db *db, Stmt *st, TableCtx *tc, Builder *b)
{
    AmScan *scan = NULL;
    int rc = open_scan(db, tc, st->where, &scan);
    if (rc != OK)
        return rc;

    AmVisCtx vis;
    make_vis_ctx(db, &vis);

    u8 recbuf[PAGE_SIZE];
    u16 reclen;
    DbValue row[SQL_MAX_COLS];
    Rid rid;

    while ((rc = tc->am->scan_next(tc->am, scan, &rid,
                                   recbuf, sizeof(recbuf), &reclen)) == OK) {
        /* 可见性由模态判断（heap 走 MVCC，对象存储恒可见） */
        if (!tc->am->visible(tc->am, recbuf, reclen, &vis))
            continue;
        if (tc->am->decode(tc->am, recbuf, reclen, row) != OK)
            continue;

        int stop = 0;
        rc = emit_row_rowwise(st, tc, b, row, &stop);
        if (rc != OK || stop)
            break;
    }
    tc->am->scan_end(tc->am, scan);

    if (rc == E_EOF)
        rc = OK;
    return rc;
}

/* ---------------- 对外入口 ---------------- */

/* 投影的每一项都必须是简单的列引用（表达式投影退回逐行） */
static bool targets_supported(const Stmt *st)
{
    if (st->star)
        return true;
    for (int i = 0; i < st->ntargets; i++) {
        if (!st->targets[i] || st->targets[i]->kind != EX_COL)
            return false;
    }
    return true;
}

int exec_select_run(Db *db, Stmt *st, TableCtx *tc, Builder *b,
                    char *err, int errcap)
{
    /* 聚合：先分派出去（形状不对就明确报错，不能悄悄退回逐行算错的） */
    if (stmt_has_agg(st))
        return exec_select_agg(db, st, tc, b, err, errcap);

    int need_depth = 0;

    if (targets_supported(st) &&
        where_supported(st->where, tc->cols, tc->ncols, 0, &need_depth))
        return exec_select_batch(db, st, tc, b);

    /* 形状不支持列式 → 逐行 */
    return exec_select_rowwise(db, st, tc, b);
}
