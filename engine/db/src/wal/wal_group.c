#include "wal/wal_internal.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/*
 * wal/wal_group.c —— group commit（W-D 的 D-7）
 *
 * ==================== 这个文件解决什么 ====================
 *
 * `wal.c` 原来的 wal_fsync 每次调用都做一次真实的 fsync 系统调用。
 * 单线程下那是必须的；**多线程并发提交**时它变成纯粹的浪费：
 *
 *   线程 A 提交 → fsync（毫秒级）
 *   线程 B 提交 → fsync（毫秒级）      ← 这期间 A 已经落盘的东西又刷了一遍
 *   线程 C 提交 → fsync（毫秒级）
 *
 * 磁盘一次 fsync 的代价与"要刷多少字节"关系不大，与"要不要等介质确认"关系很大。
 * 所以 N 个线程同时提交，共享一次就够了 —— 这就是 group commit。
 *
 * ==================== 状态机 ====================
 *
 *   提交者 → 写 COMMIT 记录（拿 lsn，这一步在 wal.c 里）
 *        → 进 wal_group_durable()
 *             ├─ flushed_lsn 已经 >= 我的 target   ⇒ 直接返回（别人的 fsync 覆盖了我）
 *             ├─ 没人在跑本代                       ⇒ 我当**发起者**：追平式 flush
 *             └─ 有人在跑                           ⇒ 我当**等待者**：等这一代结束
 *
 * ==================== 四个必须定清楚的取舍 ====================
 *
 * ① **谁执行 fsync**：第一个到达且发现"没人在跑"的线程。
 *    它是这一代的发起者，其余线程只是等 —— 不是各自 fsync（那还是 N 次）。
 *
 * ② **等待者怎么知道要等哪一代**：等**代数**（`fsync_gen`），不是只能等水位。
 *    ⚠️ 这是最容易做错的一点：
 *       - 只看 `flushed_lsn` ⇒ fsync **失败**时水位不涨，等待者永远醒不来（挂死）；
 *       - 只看代数 ⇒ 醒来还得再查水位，否则可能被"上一代成功"误导。
 *    所以两者都要：**用代数判断"轮到我了没有"，用水位判断"我的数据落盘了没有"**。
 *
 * ③ **fsync 失败怎么通知等待者**：`fsync_ok` + `fsync_failed_lsn` 记下刚结束那一代，
 *    等待者醒来后如果发现自己依赖的那一代失败 ⇒ **返回 E_IO**。
 *    ⚠️ 绝不能让等待者"看到水位涨了就当成功" —— 那是让事务**假成功**，
 *       崩溃后这些"已提交"的数据全没了。这是本 Task 最危险的失败模式。
 *
 * ④ **abort / close 时怎么处理等待者**：`closing` 置位 + 广播 ⇒ 等待者全部
 *    以 E_IO 返回。宁可让它们报错，也不能让它们挂死或假成功。
 *
 * ==================== 为什么等待期间不能持域锁 ====================
 *
 * `pthread_cond_wait(&w->cond, &w->mu)` 会**先释放 mu 再睡**，醒了再拿回来。
 * 所以 appender / 别的提交者不会被一个等待者堵住。
 * 发起者做真 fsync 时也是**放开 mu** 的（继承原有设计：fsync 不在域锁里做）。
 */

/* 微秒级等待（只给测试用的慢速 fsync 用） */
static void grp_sleep_us(u32 us)
{
#ifdef _WIN32
    Sleep((DWORD)(us / 1000u > 0u ? us / 1000u : 1u));
#else
    struct timespec ts;

    ts.tv_sec = (time_t)(us / 1000000u);
    ts.tv_nsec = (long)(us % 1000000u) * 1000L;
    nanosleep(&ts, NULL);
#endif
}

/*
 * 真正落盘的那一次系统调用。
 *
 * 整个 wal.c / wal_group.c 里只剩这一处做"提交语义的落盘"
 * （另一处在 wal_close，那是关闭前兜底）。
 * 所以"这次用了几个 fsync"数得清：`w->fsync_calls`。
 */
static int grp_do_fsync(Wal *w)
{
    /*
     * 仅测试用：放慢这一代，让别的线程真的能 join 进来。
     *
     * ⚠️ **顺序很重要：先慢、再判失败。**
     * 反过来（先判失败、立刻返回）会让这一代只存在几微秒 ——
     * 别的线程还没走到域锁就已经结束了，于是它们各自变成新的发起者、
     * 各自成功 ⇒ 测不到"等待者收到失败"这个分支。
     */
    if (w->test_slow_us > 0u)
        grp_sleep_us(w->test_slow_us);

    /* 仅测试用：模拟"fsync 会失败"（见 wal_internal.h 的说明） */
    {
        _Atomic u32 *slot = (_Atomic u32 *)&w->test_fail_next;
        u32 left = atomic_load(slot);

        while (left > 0u && !atomic_compare_exchange_weak(slot, &left, left - 1u)) {
            /* left 已被更新，重试 */
        }
        if (left > 0u) {
            w->fsync_calls++;
            return E_IO;
        }
    }

    w->fsync_calls++;
    if (WAL_FSYNC(w->f) != 0)
        return E_IO;
    return OK;
}

/* 把落盘水位推进到 target（只前进，不后退） */
static void grp_raise_flushed(Wal *w, u32 target)
{
    u32 cur = atomic_load(&w->flushed_lsn);

    while (target > cur &&
           !atomic_compare_exchange_weak(&w->flushed_lsn, &cur, target)) {
        /* cur 已被刷新为当前值，重试 */
    }
}

/*
 * 发布一代结束（调用时须持有 mu）。
 *
 * 失败代要把"失败覆盖到哪儿"记下来 —— 等待者据此判断自己是否必须失败。
 */
static void grp_publish(Wal *w, u32 gen, int rc, u32 target)
{
    if (rc == OK)
        grp_raise_flushed(w, target);
    w->fsync_ok = (rc == OK);
    w->fsync_failed_lsn = (rc == OK) ? 0u : target;
    w->fsync_gen = gen + 1u;
    w->fsync_running = false;
    pthread_cond_broadcast(&w->cond);
}

void wal_group_init(Wal *w)
{
    pthread_condattr_t attr;

    if (!w)
        return;
    w->fsync_gen = 0;
    w->fsync_running = false;
    w->fsync_ok = true;
    w->fsync_failed_lsn = 0u;
    w->fsync_calls = 0;
    w->closing = false;
    w->test_fail_next = 0u;
    w->test_slow_us = 0u;

    pthread_condattr_init(&attr);
#ifdef _WIN32
    /* Windows 的 condvar 默认毫秒级分辨率；显式指定 monotonic 时钟，
     * 避免系统时间被改动时等待时长异常。 */
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&w->cond, &attr);
    pthread_condattr_destroy(&attr);
}

void wal_group_destroy(Wal *w)
{
    if (!w)
        return;
    pthread_cond_destroy(&w->cond);
}

/*
 * 发起者的"追平式 flush"（**进: mu 不持有、发起者角色已认领**；
 * 返回时 mu 不持有、发起者角色已释放）。
 *
 * ==================== 谁认领发起者角色 ====================
 *
 * `wal_group_durable` 在**持有 mu 的时候**把 `fsync_running` 置 true 并
 * 返回本函数。这一点是必须的：
 *
 *   若在解锁之后才置位，两个线程会同时看到 `!fsync_running` 并都进来，
 *   于是有两个"发起者"各自 publish 同一代 —— 代数被写乱、`fsync_ok`
 *   互相覆盖。这种错只在"恰好同时到达"时出现，本地跑一百次可能一次都不
 *   现形，但崩溃恢复里会变成"水位比真实落盘位置高" ⇒ 静默丢数据。
 *
 * ==================== 为什么不是"刷一次就走" ====================
 *
 * 第一版是刷一次就释放角色。实测（4 线程 × 25 事务、每次 fsync 2 ms）
 * 只把 100 次降到 **53** 次 —— 线程跑成了"齐步走"：
 *
 *   A 发起 gen1，只覆盖到 A 自己的 target；B/C/D 的 target 更大 ⇒ 没被覆盖
 *   ⇒ B 发起 gen2 覆盖 B ⇒ C 发起 gen3 ⇒ D 发起 gen4 ⇒ 一轮 4 次，白攒了
 *
 * ==================== 现在的做法：同一个发起者追到平 ====================
 *
 * 发起者在**同一个角色里**循环：fflush → 取 target → fsync → 推进水位，
 * 直到 last_lsn 不再增长。于是它**一次覆盖所有已经到达的等待者**。
 *
 * ==================== 为什么这样能覆盖别人 —— 唯一需要"想清楚"的地方 ====================
 *
 * 覆盖规则：**在我这次 fsync 调用开始之前已经进了内核的字节，都被它覆盖**。
 * 而每个等待者进入等待**之前**已经在自己的 `wal_group_durable` 里做过
 * fflush（那一刻还在域锁内）⇒ 它的字节早就在内核里了。
 *
 * 所以发起者在**下一轮** fflush 之后读到的 last_lsn 里包含的所有字节，
 * 全都在本轮 fsync 开始之前进了内核 ⇒ 推进到那里是**安全的**。
 *
 * ⚠️ 反过来说：**不能**只凭"fsync 期间 last_lsn 涨了"就推进水位 ——
 * 那会把还没 fflush 的字节也算成落盘（谎报 = 静默丢数据）。
 * 所以每一轮都必须先 fflush、再取 target、再 fsync，三者缺一不可。
 *
 * ==================== 为什么中途不 publish ====================
 *
 * 每轮结束就 publish 会广播等待者，它们醒来发现水位不够又睡回去 ——
 * 白白唤醒若干次（惊群）。所以整段只**发布一次**：结束或失败。
 *
 * ==================== 轮数上限 ====================
 *
 * 一直追平会让一个线程长期占住发起者角色。极端情况下（持续有新记录进来）
 * 就永远追不平，别人也没机会发起。上限 4 轮之后释放 —— 公平性优先于
 * 极限攒批。
 */
static int grp_flush_loop(Wal *w, u32 my_target)
{
    u32 target = my_target;
    int rounds;

    for (rounds = 0; rounds < 4; rounds++) {
        u32 cur;
        int rc;

        wal_lock(w);
        if (w->closing) {
            grp_publish(w, w->fsync_gen, E_IO, target);
            wal_unlock(w);
            return E_IO;
        }

        /*
         * fflush 必须在域锁内（两个线程同时 fflush 同一个 FILE* 是未定义行为）。
         * 它同时把"此刻之前 append 的所有字节"推进内核 —— 这正是下面取
         * target 的依据。
         */
        if (fflush(w->f) != 0) {
            grp_publish(w, w->fsync_gen, E_IO, target);
            wal_unlock(w);
            return E_IO;
        }

        /* 这一轮能覆盖到哪儿：只认 fflush 之后的水位 */
        cur = atomic_load(&w->last_lsn);
        if (cur > target)
            target = cur;

        wal_unlock(w);                 /* fsync 期间放开域锁 */

        rc = grp_do_fsync(w);

        wal_lock(w);
        if (rc != OK) {
            grp_publish(w, w->fsync_gen, E_IO, target);
            wal_unlock(w);
            return E_IO;
        }

        /*
         * 本轮成功 ⇒ 可以先推进水位（等待者还没被唤醒，推到哪儿都不会
         * 让它们误判）。但**不**广播、**不**释放角色。
         */
        grp_raise_flushed(w, target);

        /*
         * 追平判据：还有比 target 更晚的记录 ⇒ 再来一轮（仍是我发起）。
         * 没有 ⇒ 收工：发布成功的一代。
         */
        if (atomic_load(&w->last_lsn) <= target) {
            grp_publish(w, w->fsync_gen, OK, target);
            wal_unlock(w);
            return OK;
        }
        wal_unlock(w);                 /* 下一轮重新取锁 + fflush */
    }

    /* 轮数用完：发布成功（已经推进到的水位是安全的），释放角色 */
    wal_lock(w);
    grp_publish(w, w->fsync_gen, OK, target);
    wal_unlock(w);
    return OK;
}

/*
 * 让调用者的数据落盘（group commit 的入口）。
 *
 * 进: mu 未持有。出: mu 未持有。
 * 返回 OK = 调用者"我的 target 之前的所有日志都已落盘"；
 *     E_IO = 没落成（调用方必须让事务失败，不许假成功）。
 */
int wal_group_durable(Wal *w, u32 target)
{
    if (!w || !w->f)
        return E_INVAL;

    for (;;) {
        wal_lock(w);

        /*
         * 判据用**水位**：它才是"我的数据落盘了没有"的权威。代数只用来
         * 判断"轮到我了没有"。两者分工，不能混用 ——
         * 只看代数会把"上一代成功但我没被覆盖"误判成完成；
         * 只看水位会在 fsync **失败**时永远醒不来（水位不涨）。
         */
        if (atomic_load(&w->flushed_lsn) >= target) {
            wal_unlock(w);
            return OK;
        }
        if (w->closing) {
            wal_unlock(w);
            return E_IO;              /* 库在关：不许挂死，也不许假成功 */
        }

        if (!w->fsync_running) {
            /* ---------- 认领发起者角色（必须在锁内认领，见 grp_flush_loop 的说明） ---------- */
            w->fsync_running = true;

            /*
             * fflush 在锁内做：它既满足 stdio 的线程安全要求，又把
             * "我这个 target 之前的字节"推进内核 —— 发起者后面的 fsync
             * 才能覆盖到我。
             */
            if (fflush(w->f) != 0) {
                grp_publish(w, w->fsync_gen, E_IO, target);
                wal_unlock(w);
                return E_IO;
            }

            wal_unlock(w);
            if (grp_flush_loop(w, target) != OK)
                return E_IO;
            continue;              /* 成功：回顶部复查水位 */
        }

        /* ---------- 我是等待者 ---------- */
        {
            u32 gen = w->fsync_gen;

            while (w->fsync_gen == gen && w->fsync_running && !w->closing)
                pthread_cond_wait(&w->cond, &w->mu);

            /*
             * 醒来的三种情形：
             *   ① closing            ⇒ 库要关了（不许挂死、不许假成功）
             *   ② 那一代失败且**覆盖到我** ⇒ 我依赖它 ⇒ 返回 E_IO
             *   ③ 其余（成功 / 失败但没覆盖到我）⇒ 回顶部，可能成为新发起者
             *
             * ⚠️ ② 的"覆盖到我"条件是必须的：失败那一代若只覆盖到比我更早的
             * 记录，我的数据还能靠下一代落盘 —— 判它失败等于让一个本来能成功
             * 的事务平白失败。
             */
            if (w->closing) {
                wal_unlock(w);
                return E_IO;
            }
            if (w->fsync_gen != gen && !w->fsync_ok &&
                w->fsync_failed_lsn >= target) {
                wal_unlock(w);
                return E_IO;
            }
            wal_unlock(w);
        }
    }
}
void wal_test_fail_fsync(Wal *w, u32 n)
{
    if (!w)
        return;
    w->test_fail_next = n;
}

void wal_test_slow_fsync(Wal *w, u32 us)
{
    if (!w)
        return;
    w->test_slow_us = us;
}

u64 wal_group_fsync_calls(const Wal *w)
{
    if (!w)
        return 0;
    return w->fsync_calls;
}
