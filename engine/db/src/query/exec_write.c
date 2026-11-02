/*
 * exec_write.c — INSERT / UPDATE / DELETE / VACUUM / CREATE
 *
 * 【写路径的原则】
 *
 * 1. **能力协商**：模态不支持写就明确报错（E_NOTSUP），
 *    不静默成功，也不崩。
 * 2. **xid 显式传**（硬规则 5）：每次写都带当前事务号。
 * 3. **不边扫边改**：先把匹配的 RID 收集齐，再逐个改 ——
 *    边扫边改会打乱扫描器。
 */

#include "query/exec_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 类型转换 ---------------- */

/*
 * 把值转换成列的类型。
 *
 * 【为什么必须有这一步】
 *
 * 值的类型来自**字面量**（写 5 就是整数），列的类型来自 **CREATE TABLE**。
 * 两者不一定一致 —— 而记录编码（val_to_record）是**按列的类型重解释**的：
 *
 *   往 INT4 列写 5000000000 → 截断成 705032704（静默写错）
 *   往 REAL 列写 5          → 存 0.0（静默写错）
 *   往 TEXT 列写 5          → 存空串（静默写错）
 *
 * 这三种都是"看起来成功、其实数据错了" ——
 * 按本项目的铁律，必须**显式转换 + 范围检查**，
 * 转不过去就报错，绝不写一个错值 ✓
 *
 * 【转换规则】（对齐 SQLite 的列亲和性，简化版）
 *   数值列 + 整数 → 转成列的类型；装不下 → 报错
 *   数值列 + 实数 → 整数值的实数才能进整数列，否则报错
 *   文本列 + 任意标量 → 转成文本（SQLite 的 TEXT 亲和性）
 *   布尔列 + 任意 → 非零即真
 */
static int coerce_to_col(const ColDesc *col, DbValue *v,
                         char *err, int errcap)
{
    if (v->is_null)
        return OK;

    switch (col->type) {
    case T_INT4:
    case T_INT8: {
        if (v->type == T_REAL) {
            double d = v->d;
            /* 先判能不能放进 i64 —— 直接 (i64)d 是 UB */
            if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) {
                snprintf(err, (size_t)errcap,
                         "列 %s：值超出 64 位整数范围", col->name);
                return E_NOSPACE;
            }
            i64 x = (i64)d;
            if ((double)x != d) {
                snprintf(err, (size_t)errcap,
                         "列 %s：小数值不能存进整数列", col->name);
                return E_INVAL;
            }
            v->i = x;
        }
        if (col->type == T_INT4 && (v->i > INT32_MAX || v->i < INT32_MIN)) {
            snprintf(err, (size_t)errcap,
                     "列 %s：值 %lld 超出 32 位整数范围", col->name,
                     (long long)v->i);
            return E_NOSPACE;
        }
        v->type = T_INT8;
        break;
    }

    case T_REAL:
        if (v->type != T_REAL) {
            v->d = (double)v->i;
            v->type = T_REAL;
        }
        break;

    case T_TEXT:
        if (v->type != T_TEXT || v->external) {
            char buf[64];
            val_to_str(v, buf, (int)sizeof(buf));
            val_text(v, buf, (u16)strlen(buf));
        }
        break;

    case T_BOOL:
        val_bool(v, v->type == T_REAL ? (v->d != 0.0) : (v->i != 0));
        break;

    default:
        break;
    }
    return OK;
}

/* ---------------- INSERT ---------------- */

int exec_insert(Db *db, Stmt *st, QueryResult **out, char *err, int errcap)
{
    TableCtx tc;
    int rc = resolve_table(db, st->table, &tc, err, errcap);
    if (rc != OK)
        return rc;

    /* 能力协商：模态不支持写就明确报错 */
    if (!(tc.am->caps & AM_CAP_INSERT)) {
        snprintf(err, (size_t)errcap, "表 %s 不支持插入", st->table);
        return E_NOTSUP;
    }

    if (st->ncols != tc.ncols) {
        snprintf(err, (size_t)errcap, "列数不匹配：表有 %d 列，给了 %d 个值",
                 tc.ncols, st->ncols);
        return E_INVAL;
    }

    QueryResult *r = calloc(1, sizeof(QueryResult));
    if (!r)
        return E_IO;

    DbValue row[SQL_MAX_COLS];

    for (int i = 0; i < st->nrows; i++) {
        for (int c = 0; c < st->ncols; c++) {
            rc = eval_expr(st->vals[i][c], NULL, &row[c]);
            if (rc != OK)
                goto fail;
            if (row[c].is_null && !tc.cols[c].nullable) {
                snprintf(err, (size_t)errcap, "列 %s 不允许 NULL", tc.cols[c].name);
                rc = E_INVAL;
                goto fail;
            }
            /* 转换成列的类型：装不下就报错，不写错值 */
            rc = coerce_to_col(&tc.cols[c], &row[c], err, errcap);
            if (rc != OK)
                goto fail;
        }

        /* xid 显式传（硬规则 5） */
        Rid rid;
        rc = tc.am->insert(tc.am, db->cur_xid, row, &rid);
        if (rc != OK)
            goto fail;

        r->affected++;
    }

    /*
     * 更新行数估算（v1.1 步骤 4）。
     *
     * 只改内存（O(1)）—— 每次插入都落盘元数据等于给"统计"付比业务更高的代价。
     * 落盘在 VACUUM 时做一次（见 catalog_stats.c）。
     *
     * 放在循环**之后**：中途失败会走 fail 分支，事务回滚、行也不存在，
     * 计数就不该变 ✓
     */
    catalog_add_nrows(db->cat, tc.tid, r->affected);

    *out = r;
    return OK;

fail:
    qr_free(r);
    return rc;
}

/* ---------------- 收集匹配行 ---------------- */

/*
 * 收集匹配行的 RID —— 不能边扫边改，会打乱扫描器。
 *
 * 这里用**全表扫描**（不下推）：改操作要按顺序处理每一行，
 * 用索引拿到的 RID 顺序和堆顺序不同，会让"更新多行"的行为依赖实现细节。
 * 下推目前只用于 SELECT。
 */
static int collect_matches(Db *db, TableCtx *tc, Expr *where,
                           Rid **out, int *n_out)
{
    AmScan *scan = NULL;
    int rc = tc->am->scan_begin(tc->am, &scan);
    if (rc != OK)
        return rc;

    int cap = 64, n = 0;
    Rid *rids = malloc((size_t)cap * sizeof(Rid));
    if (!rids) {
        tc->am->scan_end(tc->am, scan);
        return E_IO;
    }

    AmVisCtx vis;
    make_vis_ctx(db, &vis);

    u8 recbuf[PAGE_SIZE];
    u16 reclen;
    Rid rid;
    DbValue row[SQL_MAX_COLS];

    while ((rc = tc->am->scan_next(tc->am, scan, &rid,
                                   recbuf, sizeof(recbuf), &reclen)) == OK) {
        if (!tc->am->visible(tc->am, recbuf, reclen, &vis))
            continue;
        if (tc->am->decode(tc->am, recbuf, reclen, row) != OK)
            break;

        if (where) {
            DbValue w;
            rc = eval_expr(where, row, &w);
            if (rc != OK)
                break;
            if (val_truth(&w) != 1)
                continue;
        }

        if (n >= cap) {
            int ncap = cap * 2;
            Rid *nr = realloc(rids, (size_t)ncap * sizeof(Rid));
            if (!nr) {
                rc = E_IO;
                break;
            }
            rids = nr;
            cap = ncap;
        }
        rids[n++] = rid;
    }
    tc->am->scan_end(tc->am, scan);

    if (rc == E_EOF)
        rc = OK;
    if (rc != OK) {
        free(rids);
        return rc;
    }

    *out = rids;
    *n_out = n;
    return OK;
}

/* ---------------- DELETE ---------------- */

int exec_delete(Db *db, Stmt *st, QueryResult **out, char *err, int errcap)
{
    TableCtx tc;
    int rc = resolve_table(db, st->table, &tc, err, errcap);
    if (rc != OK)
        return rc;

    if (!(tc.am->caps & AM_CAP_REMOVE)) {
        snprintf(err, (size_t)errcap, "表 %s 不支持删除", st->table);
        return E_NOTSUP;
    }

    if (st->where) {
        rc = bind_expr(st->where, tc.cols, tc.ncols, err, errcap);
        if (rc != OK)
            return rc;
    }

    Rid *rids = NULL;
    int n = 0;
    rc = collect_matches(db, &tc, st->where, &rids, &n);
    if (rc != OK)
        return rc;

    QueryResult *r = calloc(1, sizeof(QueryResult));
    if (!r) {
        free(rids);
        return E_IO;
    }

    for (int i = 0; i < n; i++) {
        if (tc.am->remove(tc.am, db->cur_xid, rids[i]) == OK)
            r->affected++;
    }
    free(rids);

    /* 更新行数估算（与 INSERT 同一套：只改内存，落盘留给 VACUUM） */
    catalog_add_nrows(db->cat, tc.tid, -(i64)r->affected);

    *out = r;
    return OK;
}

/* ---------------- UPDATE ---------------- */

int exec_update(Db *db, Stmt *st, QueryResult **out, char *err, int errcap)
{
    TableCtx tc;
    int rc = resolve_table(db, st->table, &tc, err, errcap);
    if (rc != OK)
        return rc;

    if (!(tc.am->caps & AM_CAP_UPDATE)) {
        snprintf(err, (size_t)errcap, "表 %s 不支持更新", st->table);
        return E_NOTSUP;
    }

    int set_idx = -1;
    for (int i = 0; i < tc.ncols; i++) {
        if (strcmp(tc.cols[i].name, st->set_col) == 0) {
            set_idx = i;
            break;
        }
    }
    if (set_idx < 0) {
        snprintf(err, (size_t)errcap, "列不存在: %s", st->set_col);
        return E_NOTFOUND;
    }

    rc = bind_expr(st->set_expr, tc.cols, tc.ncols, err, errcap);
    if (rc != OK)
        return rc;
    if (st->where) {
        rc = bind_expr(st->where, tc.cols, tc.ncols, err, errcap);
        if (rc != OK)
            return rc;
    }

    Rid *rids = NULL;
    int n = 0;
    rc = collect_matches(db, &tc, st->where, &rids, &n);
    if (rc != OK)
        return rc;

    QueryResult *r = calloc(1, sizeof(QueryResult));
    if (!r) {
        free(rids);
        return E_IO;
    }

    u8 recbuf[PAGE_SIZE];
    u16 reclen;
    DbValue row[SQL_MAX_COLS];
    DbValue nv;

    for (int i = 0; i < n; i++) {
        /* 重新读一遍：收集阶段之后行可能被前一次更新挪动过 */
        rc = tc.am->get(tc.am, rids[i], recbuf, sizeof(recbuf), &reclen);
        if (rc != OK)
            continue;

        rc = tc.am->decode(tc.am, recbuf, reclen, row);
        if (rc != OK)
            continue;

        rc = eval_expr(st->set_expr, row, &nv);
        if (rc != OK)
            continue;
        if (nv.is_null && !tc.cols[set_idx].nullable)
            continue;

        /* 转换成列的类型（与 INSERT 同一套规则） */
        rc = coerce_to_col(&tc.cols[set_idx], &nv, err, errcap);
        if (rc != OK) {
            /* 转换失败 → 整条语句失败（不能"更新了一半还报成功"） */
            free(rids);
            qr_free(r);
            return rc;
        }

        val_copy(&row[set_idx], &nv);

        /* MVCC 更新的两段式（打 xmax + 插新版本）在 AM 里做 */
        Rid new_rid;
        rc = tc.am->update(tc.am, db->cur_xid, rids[i], row, &new_rid);
        if (rc != OK)
            continue;

        r->affected++;
    }
    free(rids);

    *out = r;
    return OK;
}

/* ---------------- VACUUM ---------------- */

/*
 * VACUUM <table> —— 回收死版本。
 *
 * 【为什么 vacuum 要在事务里做】
 *
 * vacuum 的删除动作本身会写 WAL 日志（日志里的 xid 必须是
 * 真正干活的这个事务）。所以它必须跑在一个有 xid 的事务里，
 * 否则日志里的 xid=0，恢复时会被当成系统事务直接生效 ——
 * 崩溃后可能留下半回收的表。
 *
 * 好在 db_exec 已经保证了：任何非事务控制语句都在事务里跑。
 *
 * 【为什么删掉的版本不用管索引】
 *
 * 索引项指向 RID，而 vacuum 删的是"死版本"的 slot。
 * 索引里可能还有指向死版本的项 —— 但那些项对应的行
 * 对任何快照都不可见，所以扫描时会自然被过滤掉。
 *
 * 更重要的是：**slot 永不复用**。所以那个索引项指向的
 * slot 永远不会再被新记录占用，不会读到别人的数据。
 */
int exec_vacuum(Db *db, Stmt *st, QueryResult **out, char *err, int errcap)
{
    TableCtx tc;
    int rc = resolve_table(db, st->table, &tc, err, errcap);
    if (rc != OK)
        return rc;

    if (!(tc.am->caps & AM_CAP_VACUUM)) {
        snprintf(err, (size_t)errcap, "表 %s 不支持 VACUUM", st->table);
        return E_NOTSUP;
    }

    AmVacuumStats stats;
    rc = tc.am->vacuum(tc.am, db->txns, db->cur_xid, &stats);
    if (rc != OK) {
        snprintf(err, (size_t)errcap, "VACUUM 失败: %s", err_name(rc));
        return rc;
    }

    /*
     * 顺便把行数估算**校准并落盘**（v1.1 步骤 4）。
     *
     * 【为什么 VACUUM 是落盘的好时机】
     * 它本来就要扫全表 —— 数行数是顺手的事，不额外多一次扫描。
     * 而写路径只改内存（每次插入都写元数据页太贵），
     * 所以内存里的值可能已经和磁盘上的差了一截；
     * VACUUM 把两者拉平，重启后才有一个靠谱的估算值。
     *
     * 这与 PostgreSQL 的 reltuples 由 VACUUM / ANALYZE 更新是同一策略。
     *
     * tc.tid = 0 表示虚拟表（模态提供的表），没有 catalog 行 → 跳过。
     */
    if (tc.tid) {
        catalog_set_nrows(db->cat, tc.tid, (i64)stats.kept);
        catalog_persist_nrows(db->cat, db->cur_xid, tc.tid);
    }

    /*
     * 汇报成一行三列：scanned / removed / kept。
     *
     * 用 affected = removed —— 这样调用方（db_result_affected）
     * 拿到的就是"回收了多少版本"，和 PG 的 VACUUM 语义接近。
     */
    QueryResult *r = calloc(1, sizeof(QueryResult));
    if (!r)
        return E_IO;

    r->ncols = 3;
    r->nrows = 1;
    r->affected = (int)stats.removed;

    r->colnames = calloc(3, sizeof(char *));
    r->cells = calloc(3, sizeof(char *));
    if (!r->colnames || !r->cells) {
        qr_free(r);
        return E_IO;
    }

    r->colnames[0] = strdup("scanned");
    r->colnames[1] = strdup("removed");
    r->colnames[2] = strdup("kept");

    char buf[3][32];
    snprintf(buf[0], sizeof(buf[0]), "%llu", (unsigned long long)stats.scanned);
    snprintf(buf[1], sizeof(buf[1]), "%llu", (unsigned long long)stats.removed);
    snprintf(buf[2], sizeof(buf[2]), "%llu", (unsigned long long)stats.kept);

    for (int i = 0; i < 3; i++) {
        r->cells[i] = strdup(buf[i]);
        if (!r->cells[i]) {
            qr_free(r);
            return E_IO;
        }
    }

    *out = r;
    return OK;
}

/* ---------------- CREATE INDEX ---------------- */

int exec_create_index(Db *db, Stmt *st, QueryResult **out, char *err, int errcap)
{
    TableCtx tc;
    int rc = resolve_table(db, st->table, &tc, err, errcap);
    if (rc != OK)
        return rc;

    if (!(tc.am->caps & AM_CAP_INDEX)) {
        snprintf(err, (size_t)errcap, "表 %s 不支持建索引", st->table);
        return E_NOTSUP;
    }

    int col = -1;
    for (int i = 0; i < tc.ncols; i++) {
        if (strcmp(tc.cols[i].name, st->index_col) == 0) {
            col = i;
            break;
        }
    }
    if (col < 0) {
        snprintf(err, (size_t)errcap, "列不存在: %s", st->index_col);
        return E_NOTFOUND;
    }

    u32 iid = 0;
    /*
     * 索引数上限 = SQL_MAX_COLS（v1.2 步骤 1）。
     *
     * 原因是执行器解析表时会把索引列表拷进一个固定大小的缓冲区
     * （TableCtx.idxs_buf）。超限的话不是"拷不下"这么温和，
     * 而是那条查询直接失败 —— 与其让它在查询时炸，
     * 不如在建索引时就明确报错。
     */
    if (tc.nidx >= SQL_MAX_COLS) {
        snprintf(err, (size_t)errcap, "索引过多：每张表最多 %d 个", SQL_MAX_COLS);
        return E_NOSPACE;
    }

    rc = catalog_create_index(db->cat, db->cur_xid, st->index_name, tc.tid,
                              (u32)col, &iid);
    if (rc != OK) {
        snprintf(err, (size_t)errcap, "建索引失败（可能重名）");
        return rc;
    }

    /* 重新解析表上下文，才能拿到刚建好的索引句柄 */
    rc = resolve_table(db, st->table, &tc, err, errcap);
    if (rc != OK)
        return rc;

    QueryResult *r = calloc(1, sizeof(QueryResult));
    if (!r)
        return E_IO;

    /* 把已有数据灌进新索引（索引不写日志，所以灌完才算完） */
    u64 rows = 0;
    rc = tc.am->reindex(tc.am, iid, &rows);
    if (rc != OK) {
        qr_free(r);
        return rc;
    }
    r->affected = (int)rows;

    *out = r;
    return OK;
}

/* ---------------- CREATE TABLE ---------------- */

int exec_create(Db *db, Stmt *st, QueryResult **out, char *err, int errcap)
{
    if (st->ncols == 0) {
        snprintf(err, (size_t)errcap, "至少需要一列");
        return E_INVAL;
    }

    ColDesc cols[SQL_MAX_COLS];
    memset(cols, 0, sizeof(cols));
    for (int i = 0; i < st->ncols; i++) {
        snprintf(cols[i].name, COL_NAME_MAX, "%s", st->col_names[i]);
        cols[i].type = st->col_types[i];
        cols[i].nullable = st->col_nullable[i];
    }

    u32 tid = 0;
    int rc = catalog_create_table(db->cat, db->cur_xid, st->table, cols,
                                  st->ncols, &tid);
    if (rc != OK) {
        snprintf(err, (size_t)errcap, "建表失败（可能重名）");
        return rc;
    }

    QueryResult *r = calloc(1, sizeof(QueryResult));
    if (!r)
        return E_IO;
    r->affected = 0;
    *out = r;
    return OK;
}

/* ---------------- 重建索引 ---------------- */

/*
 * 什么时候需要重建？
 *
 *   force = true  → 崩溃恢复之后。索引不写日志，堆可能已经往前走了，
 *                   索引里的 RID 可能指向已被覆盖的旧版本 —— 必须全量重建。
 *
 *   force = false → 正常开库。只重建**格式过期**的索引。
 *                   索引 key 的编码是可以变的（v1.1 就从"只有列值"
 *                   改成了"列值 + RID"），磁盘上已有的索引页存的是
 *                   旧格式的 key，拿新代码去读结果是错的而且不报错。
 *                   页头里记着格式版本，对不上就重建。
 *
 * 【为什么可以只重建格式过期的】
 * 索引是**派生物**：它的内容完全由堆决定，重建一次没有语义风险。
 * 所以"格式变了"这件事永远可以用重建来兜底，不需要版本迁移代码。
 */
int exec_rebuild_indexes(Db *db, bool force)
{
    if (!db)
        return E_INVAL;

    u32 nt = catalog_ntables(db->cat);
    for (u32 i = 0; i < nt; i++) {
        u32 tid = catalog_table_id_at(db->cat, (int)i);
        TableCtx tc;
        if (resolve_table_id(db, tid, &tc) != OK)
            continue;

        /* 索引列表已经在 resolve_table_id 里拷进 tc.idxs_buf（v1.2 步骤 1） */
        for (int k = 0; k < tc.nidx; k++) {
            if (!force && btree_format_ok(tc.idxs[k].bt))
                continue;              /* 格式没变 → 不用动 */

            /* 索引是派生物：从表数据重建（AM 自己知道怎么重建） */
            (void)tc.am->reindex(tc.am, tc.idxs[k].meta.index_id, NULL);
        }
    }
    return OK;
}
