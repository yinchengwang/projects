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

#endif /* DB_COMMON_LATCH_H */
