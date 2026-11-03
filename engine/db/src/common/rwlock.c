/*
 * rwlock.c —— 读写锁的实现
 *
 * ==================== 算法 ====================
 *
 * 三个原子量：
 *
 *   readers  持读锁的线程数
 *   waiting  在等写锁的线程数
 *   writer   写锁是否被持有（0/1）
 *
 * 【读者进锁】
 *
 *   快路径：writer == 0 且 waiting == 0 → 直接加一进临界区。
 *
 *   加一之后**再查一遍**：如果这时来了写者（waiting 变成了 1），
 *   说明我们插了队 —— 退回去（减一）重试，让写者先进。
 *   这一步是"写者不饿死"的关键。
 *
 *   两个条件都不满足（有写者 / 有写者在等）→ 让出 CPU 重试。
 *   写者很罕见，所以这条路径基本不走。
 *
 * 【写者进锁】
 *
 *   waiting 加一（挡住新读者）→ 拿互斥量（写者之间串行）
 *   → 等 readers 清零 → writer 置 1 → waiting 减一。
 *
 *   互斥量**一直持有到 wrunlock** —— 这样"同一时刻只有一个写者"
 *   是由互斥量保证的，不需要额外标志位。
 *
 * 【读者出锁】
 *
 *   readers 减一即可。写者是用"轮询 readers"来等清零的，
 *   所以这里不需要发信号。
 *
 * ==================== 为什么写者要轮询而不是条件变量 ====================
 *
 * 写者等的是"读者清零"这个**瞬时状态**。
 * 条件变量适合"等某个状态成立"，但读者的进出是高频事件，
 * 每一次进出都去 signal/broadcast 反而更慢。
 * 而且写锁很罕见（缓冲池的逐出、catalog 的 DDL），
 * 轮询的 CPU 浪费可以忽略 —— 读者在 waiting > 0 时会让路，
 * 读者数很快就清零了。
 */

#include "rwlock.h"

#include <sched.h>

void db_rwlock_init(DbRwLock *rw)
{
    if (!rw)
        return;
    atomic_init(&rw->readers, 0);
    atomic_init(&rw->waiting, 0);
    atomic_init(&rw->writer, 0);
    pthread_mutex_init(&rw->m, NULL);
}

void db_rwlock_destroy(DbRwLock *rw)
{
    if (!rw)
        return;
    pthread_mutex_destroy(&rw->m);
}

void db_rwlock_rdlock(DbRwLock *rw)
{
    if (!rw)
        return;

    for (;;) {
        /* 快路径：没有写者、也没有等待的写者 */
        if (atomic_load(&rw->writer) == 0 &&
            atomic_load(&rw->waiting) == 0) {
            atomic_fetch_add(&rw->readers, 1);

            /*
             * 复查：加一之后如果来了写者，我们插队了 → 退回去。
             *
             * 这一步同时保证了一个更强的性质：
             * **持读锁 ⟹ 那一刻没有写者**，
             * 写者也就不会在读者还在临界区时进来。
             */
            if (atomic_load(&rw->writer) == 0 &&
                atomic_load(&rw->waiting) == 0)
                return;

            atomic_fetch_sub(&rw->readers, 1);
        }

        /* 有写者（或有写者在等）→ 让出 CPU */
        sched_yield();
    }
}

void db_rwlock_rdunlock(DbRwLock *rw)
{
    if (!rw)
        return;
    atomic_fetch_sub(&rw->readers, 1);
}

void db_rwlock_wrlock(DbRwLock *rw)
{
    if (!rw)
        return;

    /* 先挡住新读者：waiting > 0 时读者的快路径不成立 */
    atomic_fetch_add(&rw->waiting, 1);

    /* 写者之间串行。这把互斥量一直持有到 wrunlock。 */
    pthread_mutex_lock(&rw->m);

    /* 等读者清零 */
    while (atomic_load(&rw->readers) > 0)
        sched_yield();

    atomic_store(&rw->writer, 1);
    atomic_fetch_sub(&rw->waiting, 1);
}

void db_rwlock_wrunlock(DbRwLock *rw)
{
    if (!rw)
        return;
    atomic_store(&rw->writer, 0);
    pthread_mutex_unlock(&rw->m);
}
