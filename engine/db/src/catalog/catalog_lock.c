/*
 * catalog_lock.c — catalog 域的读写锁（v1.2 步骤 3）
 *
 * ==================== 这一层为什么存在 ====================
 *
 * catalog 是**实例级共享**的元数据：所有连接看同一份。
 * 读多写少 —— 只有建表 / 建索引 / 改统计才写。
 * 所以用**读写锁**：读读不互斥，写才排他。
 *
 * ==================== 为什么要拆出 "_locked" 版本 ====================
 *
 * 加锁写在**公开 API** 上（就在本文件里），内部实现一律走 `_locked` 版本
 * （catalog.c / catalog_stats.c 实现）。
 *
 * 这么拆有两个硬理由：
 *
 *   1. 每个公开 API 只有**一个出口** —— 加锁/解锁不会漏路径。
 *      如果把 lock/unlock 写在每个公开函数体内，多一个 return 就漏一次解锁。
 *
 *   2. 内部函数之间互调**不会递归取锁**。
 *      POSIX 规定"重复取读锁"是未定义行为，有写者在等时会死锁；
 *      重复取写锁更是直接自锁。
 *
 * ==================== 三条使用纪律 ====================
 *
 *   1. **读锁可重入**：同一线程重复取读锁 → 计数 +1，
 *      不重复碰底层 rwlock（POSIX 明确禁止重复取读锁）。
 *
 *   2. **写锁不可重入**：持写锁再取写锁 → 违规。
 *
 *   3. **不许升级 / 降级**：持读锁取写锁（升级）、持写锁取读锁（降级）
 *      → 违规。读写锁的升级没有正确的实现方式（读者一多就死锁）。
 *
 * 违规的默认处理：**打印 + abort()** ——
 * "不可能发生"要有机器证明，当场暴露比运行时神秘崩溃好查。
 * 测试可以装自己的处理函数（catalog_set_violation_handler）。
 *
 * ==================== 与全局大锁的关系 ====================
 *
 * 步骤 3 里 `db_exec` 的大锁**保留**，所以 catalog 这把锁暂时"备而不用"。
 * 步骤 4 大锁退役之后，它才是真正的保护。
 *
 * 取锁的**嵌套顺序**必须是：先大锁、后 catalog 锁。
 * 本文件的包装层自己取 catalog 锁，而大锁在 db_exec 里更外层 ——
 * 顺序天然一致，不会出现"拿 catalog 锁再等大锁"的死锁。
 *
 * ==================== 与 latch 框架的关系 ====================
 *
 * 本文件在取锁的同时向 latch 框架**记账**（LATCH_CATALOG，序号 1 = 全序最前）。
 * 记账让步骤 4 的分域改造有全序断言可依：
 * catalog 必须排在 txn / wal / freelist / frame 之前。
 */

#include "catalog/catalog_priv.h"

#include "common/latch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 违规处理 ---------------- */

/*
 * 违规处理函数（测试专用）。
 *
 * 默认是 NULL —— 走"打印 + abort()"。
 * 装上之后由调用方决定怎么处理（测试里用来断言"确实被拦住了"，
 * 而不是把整个测试进程干掉）。
 */
static CatalogViolationFn t_violation_fn = NULL;

void catalog_set_violation_handler(CatalogViolationFn fn)
{
    t_violation_fn = fn;
}

static void catalog_violation(const char *msg)
{
    if (t_violation_fn) {
        t_violation_fn(msg);
        return;
    }
    fprintf(stderr, "[catalog-lock] 违反锁使用纪律: %s\n", msg);
    fflush(stderr);
    abort();
}

/* ---------------- 线程本地的持锁状态 ---------------- */

/*
 * 【为什么要线程本地】
 *
 * "我持有什么锁"是**当前线程**的属性，不是共享状态
 * （和 latch 框架一样的硬规则：这类信息不能放在共享对象里）。
 *
 * 槽位表而不是单个变量：理论上进程里可以有多个 catalog 实例
 * （多库），一个槽位记不下。实际用不到那么多，4 个够。
 */
#define CAT_LOCK_SLOTS 4

typedef struct {
    Catalog *cat;
    int     rd;    /* 读锁重入计数 */
    int     wr;    /* 写锁持有标志（写锁不可重入，所以是标志不是计数） */
} CatLockSlot;

static _Thread_local CatLockSlot t_slots[CAT_LOCK_SLOTS];

/*
 * 找到 cat 对应的槽位。
 *
 * create = false：只找"当前正持有 cat 的"槽位，找不到返回 NULL
 *                （调用方报"解锁多于加锁"）。
 * create = true ：找不到就占一个**空槽或已释放的槽**；
 *                全满返回 NULL（报违规）。
 *
 * 【槽位会回收】
 * 一个线程可能先后用过很多个 catalog 实例（每个测试文件开一个库）。
 * 槽位记的是"当前正持锁的 catalog"，释放之后就**可以给别的 catalog 用** ——
 * 否则用满 4 个实例就再也拿不到槽位了。
 */
static CatLockSlot *slot_of(Catalog *cat, bool create)
{
    CatLockSlot *reuse = NULL;

    for (int i = 0; i < CAT_LOCK_SLOTS; i++) {
        CatLockSlot *s = &t_slots[i];
        if (s->cat == cat && (s->rd > 0 || s->wr > 0))
            return s;                       /* 正持有这个 catalog */
        if ((s->rd == 0 && s->wr == 0) && !reuse)
            reuse = s;                      /* 空槽或已释放的槽 */
    }
    if (!create)
        return NULL;
    if (!reuse)
        return NULL;
    reuse->cat = cat;
    reuse->rd = 0;
    reuse->wr = 0;
    return reuse;
}

/* 底层 rwlock 的入口（Catalog 对外是不完整的，rw 是内部成员） */
static pthread_rwlock_t *rw_of(const Catalog *cat)
{
    return (pthread_rwlock_t *)&cat->rw;
}

/* ---------------- 四个原语 ---------------- */

/*
 * 取读锁。
 *
 * 可重入：计数 +1，**只有第一次**才真的碰底层 rwlock。
 */
void catalog_read_lock(const Catalog *cat)
{
    if (!cat)
        return;

    CatLockSlot *s = slot_of((Catalog *)cat, true);
    if (!s) {
        catalog_violation("锁槽位已满，无法记录持锁状态");
        return;
    }
    if (s->wr) {
        catalog_violation("持有写锁时再取读锁（降级）");
        return;
    }
    if (s->rd == 0) {
        if (pthread_rwlock_rdlock(rw_of(cat)) != 0) {
            catalog_violation("pthread_rwlock_rdlock 失败");
            return;
        }
    }
    s->rd++;

    /* 向 latch 框架记账：catalog 域排在全序第 1 位 */
    latch_order_acquire(LATCH_CATALOG);
}

void catalog_read_unlock(const Catalog *cat)
{
    if (!cat)
        return;

    CatLockSlot *s = slot_of((Catalog *)cat, false);
    if (!s || s->rd == 0) {
        catalog_violation("读锁释放次数多于获取次数");
        return;
    }
    s->rd--;
    latch_order_release(LATCH_CATALOG);
    if (s->rd == 0)
        pthread_rwlock_unlock(rw_of(cat));
}

/*
 * 取写锁。
 *
 * 不可重入，也**不许从读锁升级**。
 */
void catalog_write_lock(Catalog *cat)
{
    if (!cat)
        return;

    CatLockSlot *s = slot_of(cat, true);
    if (!s) {
        catalog_violation("锁槽位已满，无法记录持锁状态");
        return;
    }
    if (s->wr) {
        catalog_violation("写锁不可重入");
        return;
    }
    if (s->rd > 0) {
        catalog_violation("持有读锁时取写锁（升级）");
        return;
    }
    if (pthread_rwlock_wrlock(rw_of(cat)) != 0) {
        catalog_violation("pthread_rwlock_wrlock 失败");
        return;
    }
    s->wr = 1;

    latch_order_acquire(LATCH_CATALOG);
}

void catalog_write_unlock(Catalog *cat)
{
    if (!cat)
        return;

    CatLockSlot *s = slot_of(cat, false);
    if (!s || s->wr == 0) {
        catalog_violation("写锁释放次数多于获取次数");
        return;
    }
    s->wr = 0;
    latch_order_release(LATCH_CATALOG);
    pthread_rwlock_unlock(rw_of(cat));
}

/* ---------------- 公开 API 的包装层 ---------------- */

/*
 * 每个公开 API 的形状都一样：
 *
 *     lock → 调 _locked → unlock → 返回
 *
 * 【为什么不多套一层宏】
 * 宏会藏掉"哪几个 API 取读锁、哪几个取写锁"这件事 ——
 * 分成两段（读的一段、写的一段）反而更清楚。
 */

/* ---- 读锁组：只读不改 ---- */

int catalog_find_table(Catalog *cat, const char *name, u32 *table_id_out)
{
    catalog_read_lock(cat);
    int rc = catalog_find_table_locked(cat, name, table_id_out);
    catalog_read_unlock(cat);
    return rc;
}

int catalog_table_meta(Catalog *cat, u32 table_id, TableMeta *out)
{
    catalog_read_lock(cat);
    int rc = catalog_table_meta_locked(cat, table_id, out);
    catalog_read_unlock(cat);
    return rc;
}

int catalog_table_cols(Catalog *cat, u32 table_id, ColDesc *buf, int cap, int *ncols)
{
    catalog_read_lock(cat);
    int rc = catalog_table_cols_locked(cat, table_id, buf, cap, ncols);
    catalog_read_unlock(cat);
    return rc;
}

int catalog_table_indexes(Catalog *cat, u32 table_id, IndexInfo *buf, int cap, int *n_out)
{
    catalog_read_lock(cat);
    int rc = catalog_table_indexes_locked(cat, table_id, buf, cap, n_out);
    catalog_read_unlock(cat);
    return rc;
}

Heap *catalog_heap(Catalog *cat, u32 table_id)
{
    catalog_read_lock(cat);
    Heap *h = catalog_heap_locked(cat, table_id);
    catalog_read_unlock(cat);
    return h;
}

u32 catalog_table_id_at(const Catalog *cat, int i)
{
    catalog_read_lock(cat);
    u32 id = catalog_table_id_at_locked(cat, i);
    catalog_read_unlock(cat);
    return id;
}

u32 catalog_ntables(const Catalog *cat)
{
    catalog_read_lock(cat);
    u32 n = catalog_ntables_locked(cat);
    catalog_read_unlock(cat);
    return n;
}

u32 catalog_next_table_id(const Catalog *cat)
{
    catalog_read_lock(cat);
    u32 id = catalog_next_table_id_locked(cat);
    catalog_read_unlock(cat);
    return id;
}

int catalog_nrows(Catalog *cat, u32 table_id, i64 *out)
{
    catalog_read_lock(cat);
    int rc = catalog_nrows_locked(cat, table_id, out);
    catalog_read_unlock(cat);
    return rc;
}

/* ---- 写锁组：会改 catalog 内部状态 ---- */

int catalog_create_table(Catalog *cat, u32 xid, const char *name,
                         const ColDesc *cols, int ncols, u32 *table_id_out)
{
    catalog_write_lock(cat);
    int rc = catalog_create_table_locked(cat, xid, name, cols, ncols, table_id_out);
    catalog_write_unlock(cat);
    return rc;
}

int catalog_create_index(Catalog *cat, u32 xid, const char *name, u32 table_id,
                         u32 col_no, u32 *index_id_out)
{
    catalog_write_lock(cat);
    int rc = catalog_create_index_locked(cat, xid, name, table_id, col_no, index_id_out);
    catalog_write_unlock(cat);
    return rc;
}

void catalog_set_wal(Catalog *cat, HeapWalFn fn, void *ctx)
{
    catalog_write_lock(cat);
    catalog_set_wal_locked(cat, fn, ctx);
    catalog_write_unlock(cat);
}

void catalog_set_nrows(Catalog *cat, u32 table_id, i64 n)
{
    catalog_write_lock(cat);
    catalog_set_nrows_locked(cat, table_id, n);
    catalog_write_unlock(cat);
}

void catalog_add_nrows(Catalog *cat, u32 table_id, i64 delta)
{
    catalog_write_lock(cat);
    catalog_add_nrows_locked(cat, table_id, delta);
    catalog_write_unlock(cat);
}

int catalog_persist_nrows(Catalog *cat, u32 xid, u32 table_id)
{
    catalog_write_lock(cat);
    int rc = catalog_persist_nrows_locked(cat, xid, table_id);
    catalog_write_unlock(cat);
    return rc;
}

int catalog_set_checkpoint_lsn(Catalog *cat, u32 xid, u32 lsn)
{
    catalog_write_lock(cat);
    int rc = catalog_set_checkpoint_lsn_locked(cat, xid, lsn);
    catalog_write_unlock(cat);
    return rc;
}

u32 catalog_checkpoint_lsn(Catalog *cat)
{
    catalog_read_lock(cat);
    u32 lsn = catalog_checkpoint_lsn_locked(cat);
    catalog_read_unlock(cat);
    return lsn;
}
