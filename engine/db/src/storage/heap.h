#ifndef DB_STORAGE_HEAP_H
#define DB_STORAGE_HEAP_H

#include "common/config.h"
#include "common/error.h"
#include "common/loghook.h"
#include "common/types.h"
#include "storage/batch.h"
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
 *
 * ⚠️ **v1.3 步骤 0 起扫描不再遍历页号区间**：
 * 每张表在内存里维护一张**页号表**（`Heap.plist`，见 `heap_internal.h`），
 * 扫描只走本表的页。
 *
 * 为什么要改：按页号区间扫，意味着**每个 SELECT 都要把整个数据库的页空间
 * 走一遍**（≈79 页/次，而本表只有 ≈17 页）。4 个线程各扫自己的表时，
 * 物理上却都在 touch 全部 4 张表的页 —— 页的内容锁因此退化成全局串行点
 * （实测 12,713 次真实内容锁冲突）。
 *
 * 页号表**只在内存里**，不进 WAL：打开时由 `heap_open` 扫页重建，
 * 所以崩溃恢复的正确性不受影响 ✓
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

/*
 * 放开当前 pin 着的页（没 pin 着就是空操作）。
 *
 * ==================== 为什么需要它 ====================
 *
 * 堆扫描为了省开销，会把一页**一直 pin 着**（扫完这页再放开）——
 * 少一次哈希查找。但这样一来，**只要调用方还拿着这个扫描器**，
 * 手里就捏着一个页的内容锁（frame 域）。
 *
 * 如果这时候再去取 B+tree 的锁（btree 域），锁序就变成
 * "frame → btree"；而 B+tree 的操作本身是 "btree → frame"
 * （树的操作要拿页）。**两个方向同时存在 = 死锁的配方。**
 *
 * 所以：**行已经拷走、页可以放开的时候，就调一下这个函数**。
 * 典型例子是重建索引（拿到行 → 灌进 B+tree）。
 *
 * 代价：下一次 `heap_scan_next` 会重新 get 一次这个页
 * （一次哈希查找）—— 重建索引这种低频路径完全值得。
 */
void heap_scan_release_page(HeapScan *s);

/*
 * 零拷贝批扫描（v1.2 步骤 0）。
 *
 * ==================== 它解决什么问题 ====================
 *
 * 逐行路径每行的调用链是五层：
 *     am->scan_next → heap_am_scan_next → heap_am_scan_next_row
 *                  → heap_scan_next → page_get → memcpy
 * 10 万行就是 50 万次函数调用 —— 实测这是扫描阶段的全部开销。
 *
 * 本函数把整段塞进**一次调用**：slot 目录在页头里是连续的，
 * 顺序读 slot_off / slot_len 就行。
 *
 * ==================== 零拷贝 ====================
 *
 * 堆表的行内联在页里，所以 out[i].data **直接指向页内**，不拷。
 * 顺带避开 4K 别名（页缓冲与竞技场低 12 位相同导致的
 * store-to-load forwarding 误判）—— 实测这一项占扫描时间的 30%。
 *
 * ==================== 有效期 ====================
 *
 * 页被 pin 住，指针在**下一次调用本函数（或 heap_scan_end）之前**有效。
 *
 * 批次大小 = 一页里的行数。
 */
int  heap_scan_page_batch(HeapScan *s, AmRowRef *out, int max, int *n_out);

/* 观察用 */
u32  heap_npages(const Heap *h);
u64  heap_wal_calls(const Heap *h);

/* ---------------- 页内 compact（v1.2 步骤 5） ---------------- */

/*
 * 碎片率阈值：碎片率超过这个百分比的页才值得整理。
 *
 * 为什么要阈值而不是"每次都整理"：
 *   整理要写两次整页镜像进 WAL（8KB/页），很贵。
 *   而碎片率低的时候整理几乎回收不到空间 —— 白花钱。
 *
 * 50% 是计划里定的值：整理一次能把页内空间回收一半以上。
 */
#define HEAP_COMPACT_FRAG_PCT 50u

/* compact 的统计 */
typedef struct {
    u32 pages_scanned;   /* 看过的页数 */
    u32 pages_compacted; /* 真正整理过的页数 */
    u64 bytes_freed;     /* 回收的空间（整理前后 free_bytes 之差） */
    u64 used_bytes;      /* 整理前：全表已占字节（含空洞） */
    u64 live_bytes;      /* 整理前：全表活记录字节 */
} HeapCompactStats;

/* 单页整理的结果 */
typedef struct {
    bool compacted;   /* 这一页有没有被整理 */
    u16  frag_before; /* 整理前的碎片率（百分比） */
    u16  used_before; /* 整理前已占字节（含空洞） */
    u16  live_before; /* 整理前活记录字节 */
    u64  freed;       /* 回收的字节 */
} HeapPageCompact;

/*
 * 整理一页：把活记录压到页尾，回收页内空洞。
 *
 * ==================== RID 不变 ====================
 *
 * 整理只改**物理位置**，slot 编号不变
 * → RID = (page, slot) 不变 → 索引项、游标全都有效。
 * 这是"compact"而不是"重表"的分界线。
 *
 * ==================== 崩溃安全 ====================
 *
 * 写一条 WAL_PAGEDATA 整页镜像记录：
 *   before = 整理前的整页
 *   after  = 整理后的整页
 *
 * - 重放（redo）：直接盖上 after → 最终态 ✓
 * - 回滚（undo）：盖回 before → 整理被撤销 ✓
 *
 * 只写一条而不是"前后各一条"：一条记录里两份镜像，
 * 日志里的信息是**原子的** —— 不会出现"日志里只有前半截"的状态。
 *
 * ==================== 锁 ====================
 *
 * 整理全程在 pager_get / pager_unpin 之间，
 * 页的内容锁（frame 域）一直持有 ✓
 * 写 WAL 时是 frame(4) → wal(5)，符合获取全序 ✓
 *
 * 返回值：
 *   OK      —— 成功（out 告诉你到底有没有整理）
 *   E_NOTFOUND —— 这一页不属于本表
 *   其它    —— 出错
 *
 * frag_threshold_pct = 0 表示"只要有碎片就整理"。
 */
int  heap_compact_page(Heap *h, u32 xid, u32 page_id,
                       u16 frag_threshold_pct, HeapPageCompact *out);

/*
 * 整理整张表：逐页看碎片率，超过阈值的就整理。
 *
 * 逐页独立 —— 一页整理失败不影响别的页。
 */
int  heap_compact_table(Heap *h, u32 xid, u16 frag_threshold_pct,
                        HeapCompactStats *stats_out);

/*
 * 全表碎片率（百分比）：(已占 - 活记录) / 已占。
 *
 * 用的是 heap_compact_table **统计的整理前**的字节数 ——
 * 也就是说，它回答的是"这次 VACUUM 收拾的是多碎的一张表"。
 */
u16  heap_compact_frag_pct(const HeapCompactStats *st);

#endif /* DB_STORAGE_HEAP_H */
