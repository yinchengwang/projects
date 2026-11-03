#ifndef DB_TXN_TXN_H
#define DB_TXN_TXN_H

#include <stdatomic.h>
#include <stdio.h>

#include "common/error.h"
#include "common/types.h"

/*
 * 事务管理 + 提交日志（clog）
 *
 * v0.7 引入。v0.6 是"每条语句一个事务"，没有事务对象，
 * 也就没有"提交/回滚"的概念 —— 一条语句要么全成功，要么整条没发生。
 *
 * 【为什么必须有 clog】
 *
 * MVCC 可见性判断需要回答一个问题：**这个 xid 到底提交了没有？**
 *
 * 最直觉的做法是维护"已提交的最大 xid"（commit counter），
 * 用 xid <= committed_xmax 表示"已提交"。但这个做法有致命缺陷：
 *
 *   时刻 1：事务 A (xid=5) 开始
 *   时刻 2：事务 B (xid=6) 开始并提交 → committed_xmax = 6
 *   时刻 3：建立快照 → snap_xmax = 6
 *   时刻 4：事务 A 提交 → committed_xmax 仍是 6
 *
 * 快照里的 snap_xmax 是"时刻 3"的值 = 6。而 A 的 xid = 5 <= 6，
 * 于是快照会认为"A 在快照建立前就提交了" —— **错**。
 * A 是在快照之后才提交的，其他事务本不该看到 A 写的数据，
 * 现在却看到了。这就是"脏读"。
 *
 * 根本原因：commit counter 表达的是"**现在**的最大已提交 xid"，
 * 而快照需要的是"**快照那一刻**的提交状态"。两者不是一回事。
 *
 * PG 的解法正是 clog（commit log）：给每个 xid 存 2 bit 的状态，
 *   0 = 进行中（还没结束）
 *   1 = 已提交
 *   2 = 已中止
 * 快照时逐个查询，得到的是精确的提交状态。
 *
 * 【v1.2 步骤 6：分块 clog + 落盘】
 *
 * v0.7 刚做 clog 时用的是**环形窗口**（4096 个槽位，xid % 4096）。
 * 那个设计有个硬伤：
 *
 *   窗口一满，老 xid 的状态就被新 xid **覆盖**了。
 *   同一个会话里跑过 4096 个事务之后，早先事务的"已提交"标记就没了
 *   → 那些行**读不出来**（静默丢数据）。
 *
 *   跨会话更糟：clog 只在内存里，重启后从 WAL 重建。
 *   而 WAL 一旦被截断（步骤 6 的目标），重建的原料就没了
 *   → 可见性判断失去依据。
 *
 * 所以步骤 6 把 clog 改成两件事：
 *   1. **分块**（chunked）：按 xid 高位分块，块指针表固定大小、
 *      读路径无锁（原子读块指针）→ 老 xid 的状态**不会被覆盖**。
 *   2. **落盘**：`<db>-clog` 文件，第 xid 个字节就是它的状态。
 *      WAL 截断之前先把 clog 文件 fsync —— 于是"提交状态"这条信息
 *      永远有地方可查（要么在 clog 文件里，要么还在 WAL 里）。
 */

typedef enum {
    TXN_IDLE = 0,     /* 还没开始 */
    TXN_ACTIVE,       /* 进行中 */
    TXN_COMMITTED,    /* 已提交 */
    TXN_ABORTED       /* 已中止 */
} TxnState;

/*
 * 隔离级别。
 *
 * RC（Read Committed，语句级快照）：
 *   每条语句开始时重建快照 → 能看到别人刚提交的数据。
 *
 * RR（Repeatable Read，事务级快照）：
 *   事务开始时建一次快照，整个事务复用 →
 *   事务开始后提交的数据，本事务始终看不见。
 *
 * 【为什么 RC 是默认】
 *
 * PG 的默认就是 RC。选它是因为 RC 的语义更宽松（能看见更多），
 * 符合"用户没特殊要求就别 restrictive"的直觉。
 * RR 需要用户显式声明 —— 因为它会掩盖"这条数据刚被别人改了"的事实。
 *
 * 【两种级别的实现差别只有一处】
 *
 * 快照什么时候建：
 *   RC → 每条语句建一次
 *   RR → 事务开始建一次，整个事务复用
 *
 * 可见性判断逻辑完全一样（同一套 mvcc_visible）。
 * 这就是快照隔离的优雅之处：**隔离级别不是两套机制，
 * 而是同一个机制的两种"快照刷新时机"**。
 */
typedef enum {
    ISO_READ_COMMITTED = 0,   /* RC：语句级快照 */
    ISO_REPEATABLE_READ      /* RR：事务级快照 */
} TxnIsolation;

/* clog 里存的状态值（v1.2 步骤 4 起：每个 xid 一字节，不再 2bit 打包） */
#define TXN_CLOG_INPROGRESS 0u
#define TXN_CLOG_COMMITTED  1u
#define TXN_CLOG_ABORTED    2u

#define TXN_MAX_ACTIVE 64

/*
 * ==================== v1.2 步骤 6：分块 clog ====================
 *
 * 原来是环形窗口：clog[xid % TXN_CLOG_SLOTS]。
 *
 * 环形窗口有个硬伤 —— **老状态会被新状态覆盖**。
 * 一个会话里跑过 4096 个事务之后，最早那批事务的"已提交"标记就没了，
 * 那些行**读不出来**（静默丢数据）。
 *
 * 分块的做法：按 xid 的高位切块，一块覆盖 65536 个 xid。
 *
 *   块号 = xid >> 16          （0 .. 1023）
 *   块内偏移 = xid & 0xFFFF
 *
 * 块指针表固定 1024 项，读的时候**原子读指针**（不取锁）——
 * 这是可见性判断这条最热的读路径能保持无锁的关键。
 *
 * 覆盖范围：1024 × 65536 = 67108864 个 xid。
 * 超过这个数就当"提交"处理（理论上到不了）。
 */
#define TXN_CLOG_CHUNK_BITS 16
#define TXN_CLOG_CHUNK_SIZE (1u << TXN_CLOG_CHUNK_BITS)   /* 65536 字节/块 */
#define TXN_CLOG_CHUNKS    1024u                          /* 块指针表长度 */

typedef struct {
    _Atomic u32 xid;
    _Atomic u8  state;
} TxnEntry;

/*
 * ==================== v1.2 步骤 4：txn 域的并发保护 ====================
 *
 * 事务表是**实例级共享**的（xid 必须全局唯一），所以它是 txn 域要保护的东西。
 *
 * 【写路径：域锁（互斥）】
 *
 *   next_xid 分配、活跃表增删、clog 写入 —— 一律在 txn 域的锁里做。
 *   域锁是**进程级**的一把互斥量（txn.c 里的静态锁）：
 *   一个进程里同时开两个库的情况极少，而进程级锁换来的是
 *   "不需要 init/destroy、不需要改任何调用点"的干净实现。
 *
 * 【读路径：无锁（原子）】
 *
 *   **可见性判断每读一行就要查一次 clog** —— 这是整个内核最热的读路径。
 *   如果每次查 clog 都取一次 rwlock，10 万行的扫描会多花几毫秒，
 *   A9 的门槛（扫描 + 过滤 ≥ 3×）会直接破。
 *   所以读路径走**原子读**，一把锁都不取。
 *
 *   为此 clog 改成**每个 xid 一字节**（原来是一个字节存 4 个 2bit 状态）：
 *   - 2bit 打包的话，两个线程改同一字节里的**不同** xid 会互相覆盖
 *     （读-改-写竞争）→ 提交状态丢失 → "已提交的数据读不出来"。
 *   - 一字节一 xid 之后，每个 xid 的状态独占一个字节 → 只存不读改，
 *     原子读写不会丢更新。
 *   代价：clog 从 1 KB 变成 4 KB（4096 个槽位）。微不足道。
 *
 * 【发布顺序不能反】
 *
 *   分配 xid 时：**先**把 clog[xid] 标成"进行中"，**再**发布 next_xid。
 *   反过来的话，读者可能看到 "xid < next_xid"（像是已分配）
 *   但 clog 还是默认值 → 被判成"已提交" → **脏读**。
 */
typedef struct TxnTable {
    TxnEntry  active[TXN_MAX_ACTIVE];   /* 活跃事务 */
    _Atomic int n_active;                /* 活跃事务数 */
    _Atomic u32 next_xid;                /* 下一个可分配的 xid */
    _Atomic u32 committed_xmax;          /* 已提交事务的最大 xid（诊断用） */
    _Atomic u32 aborted_xmax;            /* 已中止事务的最大 xid（诊断用） */

    /*
     * clog：分块数组（v1.2 步骤 6）。
     *
     * clog_chunk[i] 指向覆盖 xid [i<<16, (i+1)<<16) 的那一块。
     * NULL = 这一块还没被用到（读的时候按"提交"兜底）。
     */
    _Atomic(u8 *) clog_chunk[TXN_CLOG_CHUNKS];

    /*
     * clog 的落盘状态（v1.2 步骤 6）。
     *
     * 【为什么 clog 必须落盘】
     *
     * WAL 截断之后，"哪些事务提交过"这条信息就只剩 clog 文件了。
     * 如果 clog 只在内存里，截断日志 = 把可见性判断的依据一起扔了
     * → 重启后所有历史事务都变成"进行中" → **已提交的数据全看不见**。
     *
     * 所以截断日志之前必须先把 clog fsync 落盘 —— 顺序不能反。
     *
     * 文件格式：第 xid 个字节就是 xid 的状态（0/1/2，同内存里的编码）。
     * 一字节一 xid，简单直接；文件长度 = 用过的最大 xid + 1。
     */
    FILE *clog_fp;      /* <db>-clog 文件；NULL = 没开落盘 */
    u32   clog_lo;      /* 待落盘的最小 xid（含） */
    u32   clog_hi;      /* 待落盘的最大 xid（含）；0 = 没有待落盘的 */
    u32   clog_max_xid; /* 文件里出现过的最大 xid */
    bool  clog_ready;   /* clog 文件已就绪 —— 可以安全按"已提交"兜底 */
} TxnTable;

void txn_table_init(TxnTable *t, u32 start_xid);

/*
 * 开始一个事务。返回分配到的 xid。
 * 找不到空槽返回 E_NOSPACE（活跃事务太多）。
 */
int  txn_begin(TxnTable *t, u32 *xid_out);

/*
 * 提交。提交后：
 *   - 事务从活跃表移除
 *   - clog 标记为已提交
 *   - committed_xmax 推进
 */
int  txn_commit(TxnTable *t, u32 xid);

/* 中止。事务从活跃表移除，clog 标记为已中止。 */
int  txn_abort(TxnTable *t, u32 xid);

/*
 * 查询 xid 的提交状态（读 clog）。
 *
 * 这是可见性判断的核心查询：
 *   TXN_ACTIVE    → 还在跑（或快照后启动的）
 *   TXN_COMMITTED → 已提交
 *   TXN_ABORTED   → 已中止
 *
 * xid = 0 → 系统事务，永远已提交。
 * xid >= next_xid → 还没分配，视为进行中。
 * 块不存在（这个 xid 从来没被分配过）→ 按已提交兜底。
 */
u8   txn_clog_state(const TxnTable *t, u32 xid);

/* 便捷判断 */
bool txn_committed(const TxnTable *t, u32 xid);
bool txn_aborted(const TxnTable *t, u32 xid);

/* 当前是否还有活跃事务 */
bool txn_has_active(const TxnTable *t);

/*
 * 除了 exclude_xid 之外是否还有活跃事务。
 *
 * 【为什么需要这个】
 *
 * vacuum 自己跑在一个事务里 —— 这个事务就是活跃事务。
 * 如果用 txn_has_active 判断"还有没有别人可能看到这个版本"，
 * 结果永远是 true（因为 vacuum 自己算一个），
 * 死版本就永远回收不掉 —— vacuum 变成一个只会空转的语句。
 *
 * 所以判断时必须排除执行者自己：
 * "除了我之外，还有没有别的活跃事务可能看到这个版本？"
 *
 * 这和 PG 的思路一致 —— PG 比较的是"其他会话的最老快照"，
 * 而不是"包括自己在内的所有活跃事务"。
 */
bool txn_has_active_except(const TxnTable *t, u32 exclude_xid);

/* 活跃事务里最小的 xid */
u32  txn_oldest_active(const TxnTable *t);

/* 活跃事务数 */
int  txn_nactive(const TxnTable *t);

/*
 * 直接设置 clog 状态。恢复阶段用 —— 从 WAL 里重建提交状态。
 * 用途：崩溃后，未提交事务要回滚，但 clog 里的"已提交"标记要保留，
 * 否则恢复后已提交的事务会被误判为进行中。
 */
void txn_clog_set(TxnTable *t, u32 xid, u8 state);

/*
 * ==================== v1.2 步骤 6：clog 落盘 ====================
 */

/*
 * 打开（必要时创建）clog 文件，并把文件里的内容读进内存的 clog 块。
 *
 * path 一般是 "<db>-clog"。
 *
 * 【读进来为什么重要】
 * 恢复阶段要判断"这个 xid 提交了没有"。日志被截断之后，
 * 提交记录已经不在日志里了 —— 唯一的依据就是 clog 文件。
 * 所以文件内容必须**整个**读进内存的块里，不能只留个偏移。
 */
int  txn_clog_open(TxnTable *t, const char *path);

/*
 * 把待落盘的 xid 区间写进文件并 fsync。
 *
 * 什么时候必须调：**截断 WAL 之前**。
 * 顺序反了（先截日志后落 clog）= 提交状态随日志一起没了
 * → 重启后已提交的数据全部不可见。
 */
int  txn_clog_sync(TxnTable *t);

/* 关闭 clog 文件（会先 sync 一次）。 */
void txn_clog_close(TxnTable *t);

/*
 * 丢弃 clog 文件（**不** sync）—— 模拟断电。
 *
 * db_abandon（断电模拟）用这个：真的断电时，
 * 没落盘的东西就该没有。同步落盘会让"断电模拟"变得太温柔，
 * 把本该暴露的问题藏起来。
 */
void txn_clog_discard(TxnTable *t);

/* clog 文件里出现过的最大 xid（0 = 从来没有）。 */
u32  txn_clog_max_xid(const TxnTable *t);

/*
 * 把 next_xid 推到 xid 之后（取 max）。
 *
 * 用途：日志被截断之后，日志里的最大 xid 会变小，
 * 而"真正用过的最大 xid"记在 clog 文件里 —— 开库时要把两边取大。
 */
void txn_table_bump_xid(TxnTable *t, u32 xid);

#endif /* DB_TXN_TXN_H */
