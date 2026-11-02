#ifndef DB_STORAGE_HEAP_H
#define DB_STORAGE_HEAP_H

#include "common/config.h"
#include "common/error.h"
#include "common/loghook.h"
#include "common/types.h"
#include "storage/freelist.h"
#include "storage/page.h"
#include "storage/pager.h"

/*
 * 堆表 AM
 *
 * v0.6 起不再依赖页链：页头里记 table_id，扫描直接遍历页号区间，
 * 只看 H_TYPE == heap 且 H_TID == 本表 的页。
 *
 * 为什么去掉页链：链的更新（tail->next）也是一次页修改，不写日志的话
 * 崩溃恢复后新页就成了孤儿 —— 扫描沿链走不到，整页数据"消失"。
 * 按页号扫就没有这个问题。
 */

typedef struct Heap     Heap;
typedef struct HeapScan HeapScan;

/* 日志钩子的类型定义在 common/loghook.h（堆表和 catalog 共用） */
typedef LogHookFn HeapWalFn;

int  heap_create(Pager *pg, FreeList *fl, u16 table_id,
                 HeapWalFn fn, void *ctx, u32 *root_page_out);
int  heap_open(Pager *pg, FreeList *fl, u32 root_page, u16 table_id,
               HeapWalFn fn, void *ctx, Heap **out);
void heap_destroy(Heap *h);

void heap_set_wal(Heap *h, HeapWalFn fn, void *ctx);

/*
 * 【为什么所有写接口都带 xid 参数】
 *
 * 因为 xid 是**连接私有**的当前事务号，而堆表是**实例级共享**的。
 * 把 xid 存在堆表里（比如 heap_set_xid）就是存共享状态：
 * 连接 A 设 xid=5、连接 B 设 xid=6，A 的写就会带上 B 的 xid，
 * 可见性判断立刻失效。
 *
 * 所以 xid 必须随每次写入显式传下来。0 表示"系统事务"
 * （bootstrap、操作元数据页这类非用户事务的写入）。
 */

/*
 * 写入一条记录到指定 slot（带 WAL 日志）。
 *
 * 语义与 page_update 相同，但会写日志并更新 page_lsn。
 * MVCC 用它来"给旧版本打 xmax 标记"—— 内容变了，但 slot 和 RID 不变。
 *
 * len 必须 <= 该 slot 当前的记录长度（page_update 的限制）。
 */
int  heap_write_ver(Heap *h, u32 xid, Rid rid, const u8 *rec, u16 len);

/* 插入。行编码后超过页容量返回 E_NOSPACE（overflow 是 v0.8 的事）。 */
int  heap_insert(Heap *h, u32 xid, const u8 *rec, u16 len, Rid *rid_out);

/* 按 RID 取行，拷贝到 buf。buf 不够返回 E_NOSPACE。 */
int  heap_get(Heap *h, Rid rid, u8 *buf, u16 bufsize, u16 *len_out);

/*
 * 更新。len <= 原长度时原地改，*new_rid == rid；
 * len > 原长度时 delete + insert，*new_rid 可能不同（旧 RID 失效）。
 */
int  heap_update(Heap *h, u32 xid, Rid rid, const u8 *rec, u16 len, Rid *new_rid);

int  heap_delete(Heap *h, u32 xid, Rid rid);

/* 全表扫描。返回 E_EOF 表示结束。 */
int  heap_scan_begin(Heap *h, HeapScan **out);
int  heap_scan_next(HeapScan *s, Rid *rid, u8 *buf, u16 bufsize, u16 *len_out);
void heap_scan_end(HeapScan *s);

/* 观察用 */
u32  heap_npages(const Heap *h);
u64  heap_wal_calls(const Heap *h);

#endif /* DB_STORAGE_HEAP_H */
