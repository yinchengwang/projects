#ifndef DB_CATALOG_CATALOG_H
#define DB_CATALOG_CATALOG_H

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "index/btree.h"
#include "storage/heap.h"
#include "storage/record.h"

/*
 * 元数据（catalog）
 *
 * 设计要点（DESIGN.md §10.3）：
 *   1. 系统表 id 硬编码，**绝不查表**。否则自举死锁：
 *      "查 sys_table 得先知道 sys_table 在哪"。
 *   2. 系统表自己的 schema 也硬编码在 catalog.c 里。理由同上：
 *      sys_column 的行要靠 schema 才能解码，而 schema 又存在 sys_column 里。
 *   3. 用户表的 schema 存在 sys_column 里，运行时从磁盘读。
 *
 * 第 0 页（meta）只存 bootstrap 指针，内容从 HDR_SIZE 之后开始，
 * 避免和 slotted page 的页头冲突。
 */

#define SYS_TABLE_ID      1u
#define SYS_COLUMN_ID     2u
#define SYS_INDEX_ID      3u
#define SYS_INDEXCOL_ID   4u
#define CAT_FIRST_USER_ID 5u

#define CAT_MAGIC   0x3142444Du    /* "MDB1" */
#define CAT_VERSION 1u

#define TABLE_NAME_MAX 64

typedef struct {
    u32  table_id;
    char name[TABLE_NAME_MAX];
    u32  root_page;
    u32  ncols;
    i64  nrows_est;
    u32  flags;
} TableMeta;

typedef struct Catalog Catalog;

/* 索引元数据。v0.5 只做单列索引，col_no 就是那一列。 */
typedef struct {
    u32  index_id;
    char name[TABLE_NAME_MAX];
    u32  table_id;
    u32  root_page;
    u32  col_no;
    u8   is_unique;
} IndexMeta;

/* 索引句柄：元数据 + 已打开的 B+tree。fl 是内部用的，外部别碰。 */
typedef struct {
    IndexMeta meta;
    BTree    *bt;
    FreeList *fl;
} IndexInfo;

/*
 * 打开（必要时初始化）。首次打开会建 4 张系统表。
 * fn/ctx 是 WAL 钩子，会传给每张表的堆 —— 必须在这里传，
 * 因为建表（heap_create）时就要写日志。
 */
int  catalog_open(Pager *pg, HeapWalFn fn, void *ctx, Catalog **out);
void catalog_close(Catalog *cat);

/*
 * 建表：写 sys_table 一行 + sys_column 每列一行，并创建空堆表。
 *
 * xid 是当前事务号 —— 元数据写入也要记日志，日志里的 xid 必须是
 * 真正的写入者，不能是"某个共享状态里的当前值"。
 */
int  catalog_create_table(Catalog *cat, u32 xid, const char *name,
                          const ColDesc *cols, int ncols, u32 *table_id_out);

int  catalog_find_table(Catalog *cat, const char *name, u32 *table_id_out);

/*
 * ---------------- v1.2 步骤 1：copy-out ----------------
 *
 * 下面三个接口原来返回的是 **catalog 内部的指针**。
 * 那是 v0.10 "锁只能画粗" 的根因之一：
 * 函数返回后锁就放了，catalog 随后扩容 / 重建 → 指针悬空。
 *
 * 现在统一改成 **拷贝出参**：调用方给缓冲区，函数把内容拷进去。
 * 拷出来之后，**catalog 之后怎么变都与这份副本无关**。
 *
 * 【两段式用法】（缓冲区该多大，事先不知道）
 *
 *     int n = 0;
 *     catalog_table_cols(cat, tid, NULL, 0, &n);   // 先问要多少
 *     ColDesc buf[SQL_MAX_COLS];
 *     rc = catalog_table_cols(cat, tid, buf, SQL_MAX_COLS, &n);
 *
 * 缓冲区不够 → E_NOSPACE（*ncols 仍然报实际需要的个数）。
 */

/* 取表元数据（拷贝一份到 out）。out 不能为 NULL。 */
int  catalog_table_meta(Catalog *cat, u32 table_id, TableMeta *out);

/* 取列定义（拷贝到 buf）。 */
int  catalog_table_cols(Catalog *cat, u32 table_id, ColDesc *buf, int cap, int *ncols);

/* 取某张表的堆表句柄，用于行操作。 */
Heap *catalog_heap(Catalog *cat, u32 table_id);

/*
 * 建索引：创建空 B+tree 并写元数据。
 * 注意：这里**不填充数据**，调用方要自己扫表灌进去（catalog 不知道行格式）。
 */
int  catalog_create_index(Catalog *cat, u32 xid, const char *name, u32 table_id,
                          u32 col_no, u32 *index_id_out);

/* 取某张表的所有索引（含已打开的 btree）—— 拷贝到 buf。 */
int  catalog_table_indexes(Catalog *cat, u32 table_id,
                           IndexInfo *buf, int cap, int *n_out);

/*
 * ---------------- v1.2 步骤 3：catalog 域的读写锁 ----------------
 *
 * catalog 是实例级共享的元数据，**读多写少**
 * （只有建表 / 建索引 / VACUUM 才写）→ 用读写锁，读读不互斥。
 *
 * 【锁在哪一层拿】
 *
 * 公开 API **自己**拿锁（内部实现是 `_locked` 版本，见 catalog_priv.h）。
 * 调用方**不要**自己拿 —— 拿两次就是递归取读写锁，未定义行为。
 *
 * 【读锁可重入，写锁不可】
 *
 *   - 同一线程重复取读锁 → 计数 +1，不重复碰底层的 rwlock
 *     （POSIX 明确规定"重复取读锁"是未定义行为，有写者在等时会死锁）
 *   - 持有读锁时再取写锁（**升级**）→ 直接报错退出
 *   - 持有写锁时再取读锁 → 直接报错退出
 *
 * 【与全局大锁的关系】
 *
 * 步骤 3 里 `db_exec` 的大锁**保留**，所以 catalog 锁暂时是"备而不用"的。
 * 步骤 4 大锁退役后，它才是真正的保护。
 */
void catalog_read_lock(const Catalog *cat);
void catalog_read_unlock(const Catalog *cat);
void catalog_write_lock(Catalog *cat);
void catalog_write_unlock(Catalog *cat);

/*
 * 违反上面三条纪律时的处理函数（**测试专用**）。
 *
 * 默认行为是"打印 + abort()" —— "不可能发生"要有机器证明。
 * 装上自己的函数之后就不再 abort，但调用方也拿不到"成功"的保证。
 */
typedef void (*CatalogViolationFn)(const char *msg);
void catalog_set_violation_handler(CatalogViolationFn fn);

/* 给所有表的堆注册 WAL 钩子（v0.6 起每条修改都写日志） */
void catalog_set_wal(Catalog *cat, HeapWalFn fn, void *ctx);

/*
 * 【没有 catalog_set_xid —— 这是有意的】
 *
 * v0.7 最初有 catalog_set_xid(cat, xid)，给所有堆设置"当前事务号"。
 * 但那是**共享状态**：catalog 是实例级共享的，连接 A 设 xid=5、
 * 连接 B 设 xid=6，A 的写就带上了 B 的 xid，可见性判断立刻失效。
 *
 * 现在 xid 一路作为参数传下去（catalog_create_table → heap_insert → 日志钩子）。
 */

/*
 * ---------------- 行数估算（v1.1 步骤 4） ----------------
 *
 * 规则 CBO 的**唯一统计来源**。
 *
 *   catalog_nrows        读（给代价模型）
 *   catalog_set_nrows    写绝对值（内存，O(1)）
 *   catalog_add_nrows    写增量（内存，O(1)）—— 写路径用这个
 *   catalog_persist_nrows 落盘（原地重写 sys_table 那一行）
 *
 * 【内存 vs 落盘的取舍】
 * 写路径只改内存（每次写都落盘 = 每插一行多一次页写）；
 * 落盘只在 VACUUM 时做一次 —— 与 PostgreSQL 的 reltuples 同一策略。
 * 代价：重启后 nrows_est 回到上次 VACUUM 的值（估算，够用）。
 * 详见 catalog_stats.c 的文件头。
 */
int  catalog_nrows(Catalog *cat, u32 table_id, i64 *out);
void catalog_set_nrows(Catalog *cat, u32 table_id, i64 n);
void catalog_add_nrows(Catalog *cat, u32 table_id, i64 delta);
int  catalog_persist_nrows(Catalog *cat, u32 xid, u32 table_id);

/* 按下标取 table_id，用于遍历 */
u32  catalog_table_id_at(const Catalog *cat, int i);

u32  catalog_ntables(const Catalog *cat);
u32  catalog_next_table_id(const Catalog *cat);

#endif /* DB_CATALOG_CATALOG_H */
