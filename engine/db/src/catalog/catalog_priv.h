#ifndef DB_CATALOG_PRIV_H
#define DB_CATALOG_PRIV_H

#include "catalog/catalog.h"
#include "common/rwlock.h"

#include <pthread.h>

/*
 * catalog 的内部结构（catalog.c / catalog_stats.c 共享）。
 *
 * 【为什么抽这个头】
 * catalog.c 已经 927 行（历史超限，PLAN §四 #13 排给 v1.2 拆）。
 * v1.1 步骤 4 要给 catalog 加"维护 nrows_est"的能力 ——
 * 再往 catalog.c 里塞就是明知故犯。
 *
 * 所以：统计相关的实现放 catalog_stats.c，
 * 内部结构（TableEnt / Catalog）抽到这里，两个 .c 共享。
 *
 * 这也是 v1.2「catalog copy-out 重构」的铺路：
 * 到时候要把"返回内部指针"的读接口全改成"拷出来"，
 * 那些接口的边界正好沿着这个头划。
 *
 * 【v1.2 步骤 1 已完成】
 * 三个读接口（catalog_table_meta / catalog_table_cols / catalog_table_indexes）
 * 已经改成拷贝出参 —— 外部拿不到这里面的指针了。
 * 这个头现在只剩两个用处：
 *   1. catalog.c 与 catalog_stats.c 共享内部结构
 *   2. 测试（test/integration/test_copyout.c）要**证明**旧指针确实会悬空
 */

typedef struct {
    TableMeta  meta;
    Heap      *heap;
    FreeList  *fl;
    ColDesc   *cols;    /* 深拷贝，归 catalog 所有 */
    int        ncols;
    IndexInfo *idxs;    /* 本表的索引（含已打开的 btree） */
    int        nidx;

    /*
     * 这张表在 sys_table 里的那一行的位置。
     *
     * 【为什么需要它】
     * nrows_est 要能**原地重写**（heap_write_ver，slot/RID 不变）。
     * 不记 RID 的话，每次更新都要写一个新版本 ——
     * 而 sys_table 的扫描不做可见性过滤，多版本会被重复登记成两张表。
     *
     * 系统表（含 sys_table 自己）在 bootstrap 时才写行，
     * 那时 RID 拿到后就回填；has_rid = false 表示"还没写过行"。
     */
    Rid        meta_rid;
    bool       has_rid;
} TableEnt;

struct Catalog {
    Pager    *pager;
    HeapWalFn wal_fn;
    void     *wal_ctx;
    u32       catalog_root;
    u32       next_table_id;
    u32       next_index_id;
    TableEnt *t;
    int       n;
    int       cap;

    /*
     * 最近一次 checkpoint 的 LSN（v1.2 步骤 6）。
     *
     * 存在 meta 页里 —— 也就是"这个库自己的根指针页"。
     * 恢复阶段从它开始重放：lsn <= checkpoint_lsn 的记录都不用管
     * （它们造成的效果已经在数据文件里了）。
     */
    u32       checkpoint_lsn;

    /*
     * catalog 域的读写锁（v1.2 步骤 3）。
     *
     * catalog 是**实例级共享**的 —— 所有连接看同一份元数据。
     * 元数据读多写少（只有建表 / 建索引 / VACUUM 才写），
     * 所以用读写锁：读读不互斥。
     *
     * 【为什么不是 pthread_rwlock】（v1.2 步骤 7 修正）
     *
     * winpthreads 的 pthread_rwlock **不允许读者并发**，
     * 实测 4 线程比 1 线程还慢 5 倍（见 src/common/rwlock.h）。
     * 换句话说，catalog 域的"读锁"实际上是一把更慢的互斥量 ——
     * 每条语句都要过一次它，四线程就排成一条队。
     * 换成自研的 db_rwlock（读者真并发）之后，这条排队才消失。
     *
     * 锁的原语与线程本地递归状态在 catalog_lock.c 里。
     */
    DbRwLock rw;
};

/* 按 table_id 找内存里的表项（catalog.c 实现） */
TableEnt *catalog_ent_by_id(Catalog *cat, u32 table_id);

/*
 * ---------------- 不加锁的内部实现（catalog_lock.c 的包装层调用） ----------------
 *
 * 【为什么要拆成"_locked" + 公开包装】
 *
 * 加锁写在**公开 API** 上，内部调用一律走 `_locked` 版本。
 * 这样：
 *   - 每个公开 API 只有**一个出口** → 加锁/解锁不会漏路径
 *   - 内部函数之间互调**不会递归取锁**（读写锁递归取读锁是未定义行为，
 *     而且有写者在等时会死锁）
 *
 * 公开的那一份在 catalog_lock.c 里：
 *     lock → 调 _locked → unlock → 返回
 */
int  catalog_find_table_locked(Catalog *cat, const char *name, u32 *table_id_out);
int  catalog_table_meta_locked(Catalog *cat, u32 table_id, TableMeta *out);
int  catalog_table_cols_locked(Catalog *cat, u32 table_id, ColDesc *buf, int cap, int *ncols);
int  catalog_table_indexes_locked(Catalog *cat, u32 table_id, IndexInfo *buf, int cap, int *n_out);
Heap *catalog_heap_locked(Catalog *cat, u32 table_id);
void catalog_set_wal_locked(Catalog *cat, HeapWalFn fn, void *ctx);
u32  catalog_table_id_at_locked(const Catalog *cat, int i);
u32  catalog_ntables_locked(const Catalog *cat);
u32  catalog_next_table_id_locked(const Catalog *cat);
int  catalog_create_table_locked(Catalog *cat, u32 xid, const char *name,
                                 const ColDesc *cols, int ncols, u32 *table_id_out);
int  catalog_create_index_locked(Catalog *cat, u32 xid, const char *name, u32 table_id,
                                 u32 col_no, u32 *index_id_out);
int  catalog_nrows_locked(Catalog *cat, u32 table_id, i64 *out);
void catalog_set_nrows_locked(Catalog *cat, u32 table_id, i64 n);
void catalog_add_nrows_locked(Catalog *cat, u32 table_id, i64 delta);
int  catalog_persist_nrows_locked(Catalog *cat, u32 xid, u32 table_id);
int  catalog_set_checkpoint_lsn_locked(Catalog *cat, u32 xid, u32 lsn);
u32  catalog_checkpoint_lsn_locked(Catalog *cat);

/*
 * 把 TableMeta 编码成 sys_table 的一行（catalog.c 实现）。
 *
 * 两处共用同一份编码：建表时写新行、更新统计时原地重写。
 * 编码写两遍的话，字段顺序一变就是"读出来全是错值"。
 */
int catalog_encode_table_row(const TableMeta *m, u8 *buf, u16 cap,
                             u16 *len_out);

#endif /* DB_CATALOG_PRIV_H */
