#ifndef DB_COMMON_LATCH_H
#define DB_COMMON_LATCH_H

/*
 * 全局 latch —— 一把进程级的可重入互斥量（v0.10）
 *
 * 【为什么可重入】
 *
 * 语句级大锁会在**嵌套**中用到：
 *   db_exec → pager 刷页 → WAL 钩子 → wal_fsync
 * 不可重入的锁在这里会自己把自己锁死。
 *
 * 【为什么只有一把】
 *
 * 一把锁 = 不可能有锁顺序问题 = 不可能死锁。
 * 细粒度化（多把锁）才需要定顺序 —— 那是下一版的事。
 * **并发 bug 只在特定时序暴露，同时调两个不确定，调试成本会吃掉全部学习收益。**
 *
 * 【它保护什么】
 *
 * 所有**实例级共享**的可变状态：缓冲池、freelist、WAL、事务表、
 * catalog、blob 目录。连接的私有状态（当前 xid、快照）不在保护范围内 ——
 * 那些是每个连接自己的。
 *
 * 【它不保护什么】
 *
 * 语句**之间**不持锁。显式事务的多个语句之间可以有别的线程插入 ——
 * 这正是 MVCC 要处理的情况，不是锁要处理的情况。
 */

#include <stdbool.h>

/*
 * 初始化。**幂等**，可以多处调用（内部用 pthread_once）。
 */
void latch_init(void);

/*
 * 加锁 / 解锁。可重入。
 *
 * unlock 在"解锁次数多于加锁次数"时**直接报错退出** ——
 * 这是编程错误（忘了解锁），让它当场暴露，
 * 而不是变成"程序 mysteriously 卡住"。
 */
void latch_lock(void);
void latch_unlock(void);

/*
 * 当前线程的持锁深度（调试用）。
 *
 * 用**线程本地**存储 —— 不是共享状态
 * （硬规则 5 的精神："当前是谁"的信息不能放在共享对象里）。
 */
int latch_depth(void);

/*
 * ==================== v1.2 步骤 2：按域的 latch 框架 ====================
 *
 * 从"一把大锁"走向"编号的一组小锁"的第一步：**先把纪律立起来**，
 * 再一把一把地换（步骤 3 换 catalog，步骤 4 换 txn / wal / freelist / frame / btree）。
 *
 * 【本步骤只做两件事】
 *
 *   1. **记账**：当前线程持有哪些域的锁（**线程本地**，不是共享状态）
 *   2. **全序断言**：获取顺序必须符合全序，违反 → 立刻 abort
 *
 * **本步骤不加真实互斥** —— 真实互斥在步骤 3/4 接线（那时才改调用点）。
 * 所以这一步对现有行为是**零变化**：现有代码一行都不用动。
 *
 * 【获取全序（绝不逆序）】
 *
 *     catalog → txn → btree → frame → wal → freelist
 *
 * ⚠️ **v1.2 步骤 4c 勘误：全序与计划里写的不同。**
 *
 * 计划（PLAN-v1.2 §1.3）写的是 `catalog → txn → wal → freelist → frame`，
 * 理由是"写路径先 wal 后页"。**但真实代码不是这样** ——
 * 页内容锁一旦落地，真实嵌套关系是：
 *
 *   1. **frame → wal**：堆的写路径是
 *      `pager_get(帧锁) → page_insert(改页) → wal_log(写日志)`，
 *      日志钩子是在**页还捏在手里**的时候调的；
 *      刷页路径同理（`flush_page` 先问钩子再写页）。
 *   2. **frame → freelist**：`freelist_set(fl, pid, page_free_bytes(f->data))`
 *      要读页内容算剩余空间，页还捏在手里；
 *      B+tree 回收孩子页时也一样。
 *
 * 也就是说：**页比日志"更外层"**，不是相反。
 * 计划那句"先 wal 后页"说的是**语义顺序**（日志必须先落盘才能刷页），
 * 把它当成**锁的获取顺序**就错了 ——
 * 语义顺序靠 WAL 规则保证（WAL 钩子 + page_lsn），不靠锁顺序。
 *
 * ⚠️ **v1.2 步骤 4d 勘误：全序里多了一个 btree 域。**
 *
 * 退役大锁时实测发现：**B+tree 一把锁都没有**（`btree.c` 里没有任何
 * pthread 调用），它的树结构（`root` / `height` / `scratch` 缓冲）
 * 全是裸的共享可变状态。大锁在的时候它被顺带护住了，
 * 大锁一撤，并发写索引就会**丢索引项**（实测：1200 行里点查能漏 200 行）。
 *
 * 所以补一个 **btree 域**：每棵树一把锁，护住整棵树的结构。
 * 它必须在 **frame 之前**取 —— 树的操作要拿页（frame 域），
 * 所以 btree 比 frame 更外层。
 *
 * 现在的全序由**真实调用图**反推，每一条边都有代码位置支撑：
 *
 *   catalog → txn    绑定先查 catalog，再开事务
 *   catalog → frame  catalog 写元数据页时持 catalog 锁
 *   txn     → frame  事务表与页写入不交叉（xid 先取号再写页）
 *   btree   → frame  B+tree 下降 / 分裂要拿页（btree.c 的 pager_get）
 *   frame   → wal    heap.c 的 wal_log / pager.c 的 flush_page 钩子
 *   frame   → freelist  heap.c / btree.c 的 freelist_set
 *
 * 反过来没有一条边：wal 锁里不会去碰页，freelist 锁里也不会去碰页
 * （`freelist_find` 先返回、`freelist_grow` 在页被捏住之后才调）。
 *
 * 【编号 = 获取序号】
 *
 * 按**获取序号**编号（= 全序位置），对应关系：
 *
 *     L1 catalog → LATCH_CATALOG  = 1
 *     L2 txn     → LATCH_TXN      = 2
 *     L3 btree   → LATCH_BTREE    = 3
 *     L4 frame   → LATCH_FRAME    = 4
 *     L5 wal     → LATCH_WAL      = 5
 *     L6 freelist→ LATCH_FREELIST = 6
 *
 * 【为什么断言比测试可靠】
 *
 * 测试是**抽样**（跑一万次不一定撞上那个时序），
 * 断言是**全量**（每一次获取都检查）。
 * "死锁不可能发生"要有机器证明 —— 断言就是那个证明。
 *
 * 【断言一直开，没有调试/发布之分】
 *
 * 开销是每个域 5 次整数比较 —— 可以忽略。
 * 一个能抓到逆序的断言，不该在发布版里消失。
 */

/* 域编号（按获取全序排列，值 = 获取序号） */
typedef enum {
    LATCH_CATALOG  = 1,   /* L1：表/列/索引元数据缓存（读写锁） */
    LATCH_TXN      = 2,   /* L2：next_xid、clog、活跃表（独占，很短） */
    LATCH_BTREE    = 3,   /* L3：B+tree 树结构（每棵树一把，独占） */
    LATCH_FRAME    = 4,   /* L4：单页数据 + pin_count（每帧一把，极短） */
    LATCH_WAL      = 5,   /* L5：日志缓冲、LSN 计数、append（独占） */
    LATCH_FREELIST = 6,   /* L6：空闲页链（独占，很短） */
    LATCH_NDOMAIN  = 7
} LatchId;

/*
 * 违反全序时的处理。
 *
 * 默认行为：**打印 + abort()** —— 死锁"不可能发生"要有机器证明，
 * 抓不到就等于没证明。
 *
 * 装上自己的处理函数之后，返回值才可能非 OK ——
 * **测试专用**（用来断言"确实被拦住了"而不杀死测试进程）。
 */
typedef void (*LatchViolationFn)(LatchId id, const char *msg);

/* 装违规处理函数（传 NULL 恢复默认的 abort）。测试专用。 */
void latch_set_violation_handler(LatchViolationFn fn);

/*
 * 获取 / 释放一个域的锁（**记账 + 全序断言**）。
 *
 * 返回 OK 或 E_INVAL（被断言拦下时）。
 * 默认配置下不会返回 E_INVAL —— 直接 abort 了。
 *
 * 可重入：同一个域重复获取 → 计数 +1，不算逆序。
 */
int latch_order_acquire(LatchId id);
int latch_order_release(LatchId id);

/*
 * 合法例外：**crabbing**（锁降级）。
 *
 * B+tree 分裂时的路径是"先锁子页再锁父页" ——
 * 页与页之间的获取序按**树高从大到小**，与全局全序冲突。
 * 这条路径必须显式声明，不能默默绕过断言。
 *
 * **约束**：只能用于**同域内** —— 手里握的必须已经是这个域的锁。
 * 手里是别的域还来 crabbing，就是**跨域逆序**，那是普通违规。
 *
 * 说明：域级别的检查**管不到**同域内部的顺序（域只看"frame 这个域"），
 * 所以它在域级别等价于普通获取 ——
 * 它的价值是**在代码里留下显式标记**：
 * 让读代码的人知道这条路径是经过论证的例外，而不是随手写的。
 * 页与页之间的树高顺序，由步骤 4 的 per-frame 检查真正管。
 */
int latch_order_acquire_crabbing(LatchId id);

/* 查询当前线程的持锁情况（调试 / 测试用） */
bool latch_order_held(LatchId id);       /* 这个域持着吗 */
int  latch_order_count(LatchId id);      /* 持有了几次（可重入计数） */
int  latch_order_depth(void);            /* 一共持有几个域 */

/* 域的名字（错误信息用） */
const char *latch_domain_name(LatchId id);

#endif /* DB_COMMON_LATCH_H */
