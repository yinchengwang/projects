/*
 * rwlock.h —— 读写锁（读多写少）
 *
 * ==================== 为什么不用 pthread_rwlock ====================
 *
 * 本工程在 Windows 上跑 MinGW-w64，pthread 来自 winpthreads。
 * 它的 pthread_rwlock **不允许多个读者同时进入**——
 * 实测（bench/probe_scale.c，4 线程纯 rdlock/rdunlock 循环）：
 *
 *   | 线程数 | pthread_rwlock 读锁 | pthread_mutex |
 *   |---|---|---|
 *   | 1 | 16.5 M 次/秒 | 86.0 M 次/秒 |
 *   | 2 | 5.0 M 次/秒 | 39.0 M 次/秒 |
 *   | 4 | 3.1 M 次/秒 | 44.9 M 次/秒 |
 *
 * 也就是说：**4 个线程同时读，比 1 个线程还慢 5 倍**，
 * 而且单线程也比互斥量慢 4 倍。
 * 换句话说，winpthreads 的 pthread_rwlock 在这里
 * **就是一把更慢的互斥量**，拿它当"读锁"用等于白拿。
 *
 * 既然 v1.2 步骤 3 的 catalog 域锁、v1.2 步骤 7 的缓冲池池级锁
 * 都是按"读写锁"设计的（读多写少），那这个平台的缺陷必须补上 ——
 * 否则"细粒度"只是换了个名字的全局串行点。
 *
 * ==================== 这个实现保证什么 ====================
 *
 *   - **读者之间完全并发**：N 个读者可以同时持有读锁
 *   - **写者独占**：有写者时，读者进不来
 *   - **写者之间互斥**：同一时刻只有一个写者
 *   - **写者优先**（近似）：一旦有写者在等，新读者让路
 *     → 写者不会饿死
 *
 * ==================== 不变式 ====================
 *
 *   持读锁的线程数（readers）≥ 0
 *   持写锁的线程数 ≤ 1
 *   有写者时（writer == 1）→ readers 必为 0
 *
 * ==================== 锁序 ====================
 *
 * 这把锁本身不参与 common/latch.h 的按域全序 ——
 * 它是"域内"的实现细节：每个域自己决定用不用它。
 * 调用它的一方负责在**外层**做域记账（latch_order_acquire）。
 */

#ifndef DB_RWLOCK_H
#define DB_RWLOCK_H

#include <pthread.h>
#include <stdatomic.h>

typedef struct {
    atomic_int readers;   /* 当前持有读锁的线程数 */
    atomic_int waiting;   /* 等待写锁的线程数（>0 时新读者要让路） */
    atomic_int writer;    /* 0/1：写锁是否被持有 */
    pthread_mutex_t m;    /* 写者之间串行；写者从 wrlock 一直持有到 wrunlock */
} DbRwLock;

/* 初始化 / 销毁 */
void db_rwlock_init(DbRwLock *rw);
void db_rwlock_destroy(DbRwLock *rw);

/* 读锁：读者之间并发 */
void db_rwlock_rdlock(DbRwLock *rw);
void db_rwlock_rdunlock(DbRwLock *rw);

/* 写锁：独占 */
void db_rwlock_wrlock(DbRwLock *rw);
void db_rwlock_wrunlock(DbRwLock *rw);

#endif /* DB_RWLOCK_H */
