#ifndef DB_CATALOG_PRIV_H
#define DB_CATALOG_PRIV_H

#include "catalog/catalog.h"

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
};

/* 按 table_id 找内存里的表项（catalog.c 实现） */
TableEnt *catalog_ent_by_id(Catalog *cat, u32 table_id);

/*
 * 把 TableMeta 编码成 sys_table 的一行（catalog.c 实现）。
 *
 * 两处共用同一份编码：建表时写新行、更新统计时原地重写。
 * 编码写两遍的话，字段顺序一变就是"读出来全是错值"。
 */
int catalog_encode_table_row(const TableMeta *m, u8 *buf, u16 cap,
                             u16 *len_out);

#endif /* DB_CATALOG_PRIV_H */
