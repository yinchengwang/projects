#ifndef DB_WAL_WAL_H
#define DB_WAL_WAL_H

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"

/*
 * 预写日志
 *
 * 记录格式（28 字节头 + 载荷 + crc32）：
 *   0  u32 lsn       本条记录的 LSN，单调递增
 *   4  u32 prev_lsn  同一事务的上一条（undo 链）
 *   8  u32 xid       事务号
 *  12  u8  type
 *  13  u8  pad[3]
 *  16  u32 page_id
 *  20  u16 slot
 *  22  u16 blen      before 镜像长度（0 = 无）
 *  24  u16 alen      after 镜像长度（0 = 无）
 *  26  u16 slot_off  该 slot 在页内的字节偏移（undo 时定位）
 *  28  payload      before[blen] + after[alen]
 *      u32 crc32    覆盖 header + payload
 *
 * LSN 用 u32：页头 H_LSN 只有 4 字节，全系统统一。40 亿条记录够学。
 */

typedef enum {
    WAL_BEGIN = 1,
    WAL_COMMIT,
    WAL_ABORT,
    WAL_INSERT,
    WAL_DELETE,
    WAL_UPDATE,
    WAL_PAGEINIT,      /* 新页初始化：after = 页头 20 字节，写到偏移 0 */
    WAL_CHECKPOINT,
    WAL_PAGEDATA       /* 整页镜像：before/after 各 4KB（元数据页用）*/
} WalType;

#define WAL_HDR         28u
#define WAL_MAX_PAYLOAD 8192u

typedef struct {
    u32  lsn;
    u32  prev_lsn;
    u32  xid;
    u8   type;
    u32  page_id;
    u16  slot;
    u16  blen;
    u16  alen;
    u16  slot_off;   /* 该 slot 在页内的字节偏移（undo 定位用）*/
    long off;        /* 记录在文件里的偏移，恢复时回读镜像用 */
} WalRec;

typedef struct Wal Wal;

int  wal_open(const char *path, Wal **out);
void wal_close(Wal *w);

u32  wal_last_lsn(const Wal *w);
u32  wal_nrecords(const Wal *w);

/*
 * 日志里出现过的最大 xid。
 *
 * 为什么必须持久化这个值：事务号不能跨会话重置。
 * 假设上次会话做到 xid=5 崩了，这次会话又从 xid=1 开始，
 * 恢复阶段分析"哪些事务提交了"时，会把上次那个未提交的 xid=1
 * 误判成"已提交"（因为这次有新的 xid=1 提交了）——
 * 未提交的数据就这么留在了库里。
 *
 * 做法：打开日志时扫一遍，记住最大 xid，新会话从 max+1 开始。
 */
u32  wal_max_xid(const Wal *w);

/*
 * 把日志清空，回到"刚创建"的状态。
 *
 * 什么时候用：数据库文件是空的（新建的），但同名日志文件还在。
 * 这说明日志是"上一个同名数据库"的孤儿 —— 数据文件都没了，
 * 日志留着只会把不属于这个库的记录重放进来。
 */
int  wal_reset(Wal *w);

/*
 * 追加一条记录，返回它的 LSN。
 *
 * 【v0.7 起：xid 必须显式传】
 *
 * 早期版本这里没有 xid 参数，靠 wal_set_xid(w, xid) 先把 xid 写进
 * WAL 对象再调用。这是**共享状态**，多连接下必然出错：
 *
 *   WAL 是实例级共享的（所有连接一个日志文件）。
 *   连接 A：wal_set_xid(w, 5)
 *   连接 B：wal_set_xid(w, 6)      ← 覆盖了 A 的设置
 *   连接 A 写数据 → 日志记录的 xid 是 6，不是 5
 *
 * 后果：恢复阶段分析"哪些事务提交了"时，A 的记录被算到 B 名下 ——
 * **未提交的数据可能被判定为已提交**。这是静默的数据损坏。
 *
 * 所以 xid 必须随每次调用显式传下去，不能存在共享对象里。
 * 同样的道理适用于 undo 链（prev_lsn）—— 它也改成按 xid 分槽维护。
 */
u32  wal_append_xid(Wal *w, u8 type, u32 xid, u32 page_id, u16 slot,
                    u16 slot_off,
                    const u8 *before, u16 blen, const u8 *after, u16 alen);

int  wal_fsync(Wal *w);

/*
 * 已 fsync 到磁盘的最大 LSN。
 *
 * WAL 规则（Write-Ahead Logging）的判定依据：
 *   "页刷盘前，对应的日志必须已经落盘"
 * 也就是刷一个页时，必须满足 page_lsn <= wal_flushed_lsn。
 *
 * 没有这个值就只能靠"猜"，而猜错的后果是静默的数据丢失：
 * 页带着未落盘的日志上了盘，崩溃后日志里缺这段，内容就回不来。
 */
u32  wal_flushed_lsn(const Wal *w);

/* 顺序扫描（恢复用）。E_EOF 表示结束。 */
int  wal_scan_begin(Wal *w);
int  wal_scan_next(Wal *w, WalRec *r);
void wal_scan_end(Wal *w);

/* 把某条记录的 before+after 镜像读进 buf（先 before 后 after） */
int  wal_read_payload(Wal *w, const WalRec *r, u8 *buf, u16 cap);

#endif /* DB_WAL_WAL_H */
