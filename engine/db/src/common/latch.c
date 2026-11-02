/*
 * latch.c — 全局 latch（可重入互斥量）
 *
 * 设计说明见 common/latch.h。
 *
 * 【为什么用 pthread 而不是 Windows API】
 *
 * 一次写，两边跑。MinGW-w64 的 winpthreads 提供了 pthread 接口，
 * Linux 上 pthread 是标准。Windows 原生的 CRITICAL_SECTION 要写两套代码 ——
 * 那会让"线程"这件事在代码里变得比它应该有的更复杂。
 *
 * 【为什么用可重入而不是普通互斥量】
 *
 * 语句级大锁会在嵌套中用到（db_exec → pager → WAL 钩子 → wal_fsync）。
 * 不可重入的锁会在那里自己把自己锁死 —— 而且症状是"卡住"，
 * 排查起来非常费时间。
 *
 * 【为什么深度计数是线程本地的】
 *
 * "当前持锁深度"是**每个线程各自的状态**，不是共享状态。
 * 放在共享对象里就会变成"连接 A 的深度影响连接 B"——
 * 那是硬规则 5 说的那类错误。
 */

#include "common/latch.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_mutex_t g_mtx;
static pthread_once_t  g_once = PTHREAD_ONCE_INIT;

/*
 * 当前线程的持锁深度。
 *
 * _Thread_local 是 C11 的标准写法。GCC/Clang/MinGW 都支持。
 */
static _Thread_local int t_depth;

static void latch_init_once(void)
{
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_mtx, &attr);
    pthread_mutexattr_destroy(&attr);
}

void latch_init(void)
{
    pthread_once(&g_once, latch_init_once);
}

void latch_lock(void)
{
    latch_init();                 /* 幂等：第一次调用时真正初始化 */
    pthread_mutex_lock(&g_mtx);
    t_depth++;
}

void latch_unlock(void)
{
    /*
     * 解锁次数多于加锁次数 → 编程错误。
     *
     * 立刻报错退出，而不是让它变成"程序卡住"。
     * 忘了解锁的典型症状是死锁，排查要靠猜；
     * 这里当场抓住，成本差一个数量级。
     */
    if (t_depth <= 0) {
        fprintf(stderr,
                "latch: 解锁次数多于加锁次数（当前线程）\n");
        fflush(stderr);
        abort();
    }

    t_depth--;
    pthread_mutex_unlock(&g_mtx);
}

int latch_depth(void)
{
    return t_depth;
}
