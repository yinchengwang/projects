/*
 * catalog_stats.c — nrows_est 的维护（v1.1 步骤 4）
 *
 * ==================== 这一层为什么存在 ====================
 *
 * 规则 CBO 的**唯一统计来源**就是 nrows_est（PLAN §1.4 / DESIGN §10.3）。
 * 而它原来是"留空"的：sys_table 里那一列从来没人写过，
 * 读出来永远是 0 —— 代价模型拿到 0 就没有代价可言了。
 *
 * 所以这一层只干一件事：**让 nrows_est 有值，而且尽量准**。
 *
 * ==================== 两条路径 ====================
 *
 *   内存（快、准、O(1)）
 *     INSERT 时 +n，DELETE 时 -n —— 精确值，当场可用。
 *
 *   落盘（慢、低频）
 *     **只在 VACUUM 时**原地重写 sys_table 的那一行。
 *
 * ==================== 为什么不每次写都落盘 ====================
 *
 * 每次 INSERT 都重写元数据行 = 每插一行多一次页写 + 一次 WAL。
 * 10 万行插入就是 10 万次元数据写 —— 那是在给"统计"付比业务更高的代价。
 *
 * 这也正是 PostgreSQL 的做法：`pg_class.reltuples` 由 VACUUM / ANALYZE 更新，
 * 平时读的是**估算值**。估算值的意义就是"便宜且大致对"，
 * 不是"精确且实时"。
 *
 * ==================== 重启后会怎样（如实说明） ====================
 *
 * 重启后 nrows_est 回到**上次 VACUUM 时**的值。
 * 如果 VACUUM 之后又插了很多行，估算会偏小 → CBO 可能选全扫（保守但正确）。
 * 这是已知的取舍，不是 bug：
 *   - "打开时扫全表统计" 会把启动变成 O(n)
 *   - 显式的 ANALYZE 语句列进 v1.2（本版不做语法）
 *
 * ==================== 为什么原地重写而不是插新版本 ====================
 *
 * sys_table 的扫描（load_all）**不做可见性过滤** —— 它直接遍历所有 slot。
 * 所以插新版本会让同一张表被登记两次（catalog 里出现两张同名表）。
 * `heap_write_ver` 原地改内容（slot / RID 不变），正好避开这个问题。
 *
 * 元数据表不需要"时间旅行"：没有谁会去查"上个版本的 nrows_est 是多少"。
 */

#include "catalog/catalog_priv.h"

#include <string.h>

int catalog_nrows_locked(Catalog *cat, u32 table_id, i64 *out)
{
    if (!cat || !out)
        return E_INVAL;

    TableEnt *e = catalog_ent_by_id(cat, table_id);
    if (!e)
        return E_NOTFOUND;

    *out = e->meta.nrows_est;
    return OK;
}

void catalog_set_nrows_locked(Catalog *cat, u32 table_id, i64 n)
{
    if (!cat)
        return;

    TableEnt *e = catalog_ent_by_id(cat, table_id);
    if (!e)
        return;

    /* 行数不可能为负 —— 估算值也不该出现负数 */
    e->meta.nrows_est = n < 0 ? 0 : n;
}

void catalog_add_nrows_locked(Catalog *cat, u32 table_id, i64 delta)
{
    if (!cat)
        return;

    TableEnt *e = catalog_ent_by_id(cat, table_id);
    if (!e)
        return;

    i64 n = e->meta.nrows_est + delta;
    e->meta.nrows_est = n < 0 ? 0 : n;
}

int catalog_persist_nrows_locked(Catalog *cat, u32 xid, u32 table_id)
{
    if (!cat)
        return E_INVAL;

    TableEnt *e = catalog_ent_by_id(cat, table_id);
    if (!e)
        return E_NOTFOUND;

    /* 系统表自己那一行也是写过的，一样可以更新 */
    if (!e->has_rid)
        return OK;                 /* 没有行可改（bootstrap 之前的窗口） */

    TableEnt *st = catalog_ent_by_id(cat, SYS_TABLE_ID);
    if (!st || !st->heap)
        return E_CORRUPT;

    u8 buf[256];
    u16 len;
    int rc = catalog_encode_table_row(&e->meta, buf, sizeof(buf), &len);
    if (rc != OK)
        return rc;

    return heap_write_ver(st->heap, xid, e->meta_rid, buf, len);
}
