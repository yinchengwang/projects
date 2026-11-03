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
    WAL_PAGEDATA,      /* 整页镜像：before/after 各 4KB（元数据页用）*/
    /*
     * "这个事务已经回滚完了"。
     *
     * 与 WAL_ABORT 的区别（这是 v1.2 步骤 6 加的）：
     *   WAL_ABORT      —— 事务失败时记的。**回滚还没做**
     *                      （本内核的删除是物理删除，运行时不做 undo）。
     *   WAL_ABORT_DONE —— 恢复阶段把未提交事务回滚完之后记的。
     *                      "前像已经用掉了，日志里这份可以扔"。
     *
     * 日志截断的前提就是靠这个区分：
     * 只有 WAL_ABORT_DONE 的事务，它的 before 镜像才可以跟着日志一起扔掉。
     */
    WAL_ABORT_DONE,
    /*
     * 【逻辑记录】全文检索的 tombstone（v1.3 步骤 5）。
     *
     * 载荷 = u32 doc_id（写在 after 镜像里，blen = 0）。
     *
     * 【为什么 WAL 层要有这条记录】
     *
     * WAL 层**不认识** FT 语义（分层铁律：wal/ 是 L1，不许 include modalities/）。
     * 它只负责"把这类记录原样转交给上层" —— 恢复阶段遇到非页类记录时，
     * 通过重放钩子（见 recovery.h）把 (type, xid, payload) 交给 db 层，
     * 由 db 层解释它。
     *
     * 【为什么页镜像之外还要这条逻辑记录】
     *
     * tombstone 的**权威存储**是 tombstone 页（整页镜像进 WAL）。
     * 但"这篇文档被删了"这件事本身，用一条 4 字节的逻辑记录表达更直接：
     *   - 重放时不必依赖"页镜像在不在"；
     *   - 未提交事务的删除可以被**撤销**（语义：这次删除没发生过）。
     */
    WAL_FT_TOMB
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
 * 日志文件当前占用的字节数。
 *
 * 用途：判断"要不要做一次 checkpoint"。
 * 因为每次 checkpoint 都会截断日志，所以文件大小 ≈ 上次 checkpoint 之后的日志量。
 */
u64  wal_bytes(Wal *w);

/*
 * 截断日志：删掉 lsn <= keep_lsn 的记录。
 *
 * ==================== 为什么能截 ====================
 *
 * 截断的前提是"这些记录已经不需要了" —— 也就是它们造成的效果
 * 已经**写进数据文件**了（页已经刷盘）。这正是 checkpoint 的定义。
 *
 * 截断之后：恢复阶段从 checkpoint_lsn 开始重放，
 * 恢复时间只取决于"上次 checkpoint 之后的日志量"，而不是日志总量。
 *
 * ==================== 两条硬约束 ====================
 *
 * 1. **LSN 不能倒退**
 *    日志被截空之后，重开库时 last_lsn 会从文件里重新算出来 = 0，
 *    新记录的 LSN 就从 1 重新开始 —— 而 checkpoint_lsn 可能是 5000，
 *    于是新记录全部被恢复阶段跳过（lsn <= checkpoint_lsn）→ 数据丢。
 *    所以：一条都留不住时，宁可保留最后一条。
 *
 * 2. **clog 必须先落盘**
 *    提交状态记在 clog 文件里，不在日志里。
 *    日志一截，"哪些事务提交过"就只剩 clog 文件了。
 *    所以调用方必须先 fsync clog，再调本函数。顺序反了 = 静默丢数据。
 */
int  wal_truncate(Wal *w, u32 keep_lsn);

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
