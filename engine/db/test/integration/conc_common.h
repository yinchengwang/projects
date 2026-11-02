#ifndef TEST_CONC_COMMON_H
#define TEST_CONC_COMMON_H

/*
 * conc_common.h — 并发测试的公共设施（v0.10）
 *
 * ==================== 为什么单独抽出来 ====================
 *
 * 原本 7 项并发测试挤在一个文件里（828 行），超了"单文件 ≤ 800 行"的硬规则。
 * 拆的时候发现：**同步原语 / 看门狗 / 工具函数是共用的一份**，
 * 复制两份会立刻分裂（改一处忘另一处）。
 *
 * 所以抽成头文件，两个测试文件各自 include。
 *
 * ==================== 这里面有什么 ====================
 *
 *   1. 测试自己的同步原语（标志 + 屏障）
 *   2. 看门狗（防挂死）
 *   3. 工具函数（跑 SQL、查一列、删库）
 *
 * ==================== 三条经验 ====================
 *
 * 【1】测试的共享状态要用**测试自己的**锁
 *   数据库的 latch 保护的是库内部；测试的标志是测试的状态。
 *   混用会把"库的问题"和"测试的问题"搅在一起。
 *
 * 【2】忙等在 Windows 上不可用
 *   会烧掉一个核，而且时间不可控。条件变量精确且不拖慢测试。
 *
 * 【3】静态函数全部写 `static inline`
 *   拆文件后，两边用的函数集合不同。
 *   普通 `static` 函数在另一份文件里没被用到 → `-Wunused-function` → `-Werror` 直接挂。
 *   `static inline` 没有这个限制。
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "db.h"

/* ---------------- 断言 ---------------- */

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define NTHREAD   4
#define NROWS     500

/* ---------------- 同步原语 ---------------- */

/*
 * 测试里的共享标志（线程间协调用）。
 *
 * 【为什么要自己加锁】
 * 数据库的 latch 保护的是**库内部**的状态。
 * 测试自己的标志是测试的状态 —— 用测试自己的锁。
 *
 * 【为什么不用忙等】
 * 忙等在 Windows 上会把一个核烧掉，而且时间不可控。
 * 条件变量精确，而且不会拖慢测试。
 */
static pthread_mutex_t g_test_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_test_cv = PTHREAD_COND_INITIALIZER;

static int g_flag = 0;           /* 0=没发生 1=发生了 */
static int g_flag2 = 0;

static inline void flag_set(int which)
{
    pthread_mutex_lock(&g_test_mtx);
    if (which == 0) g_flag = 1;
    else            g_flag2 = 1;
    pthread_cond_broadcast(&g_test_cv);
    pthread_mutex_unlock(&g_test_mtx);
}

static inline void flag_wait(int which)
{
    pthread_mutex_lock(&g_test_mtx);
    while (which == 0 ? !g_flag : !g_flag2)
        pthread_cond_wait(&g_test_cv, &g_test_mtx);
    pthread_mutex_unlock(&g_test_mtx);
}

static inline void flag_reset(void)
{
    pthread_mutex_lock(&g_test_mtx);
    g_flag = 0;
    g_flag2 = 0;
    pthread_mutex_unlock(&g_test_mtx);
}

/*
 * 屏障：让 N 个线程在同一个点会合。
 *
 * 【为什么 xid 串号测试需要它】
 * xid 是**在 BEGIN 时分配的**。如果两个线程的 BEGIN 不同时发生，
 * 它们就不可能拿到同一个 xid —— 测试永远抓不到串号。
 *
 * 第一版用"标志"来排序（A 插完 → B 才开始），
 * 结果两个 BEGIN 被拉得很开 —— **测试自己把 bug 藏起来了**。
 *
 * 屏障让两个 BEGIN 真正同时开始 ✓
 */
static int g_barrier_count = 0;
static int g_barrier_target = 0;
static int g_barrier_gen = 0;

static inline void barrier_init(int n)
{
    pthread_mutex_lock(&g_test_mtx);
    g_barrier_count = 0;
    g_barrier_target = n;
    g_barrier_gen = 0;
    pthread_mutex_unlock(&g_test_mtx);
}

static inline void barrier_wait(void)
{
    pthread_mutex_lock(&g_test_mtx);
    int gen = g_barrier_gen;
    g_barrier_count++;
    if (g_barrier_count >= g_barrier_target) {
        g_barrier_count = 0;
        g_barrier_gen++;
        pthread_cond_broadcast(&g_test_cv);
    } else {
        while (gen == g_barrier_gen)
            pthread_cond_wait(&g_test_cv, &g_test_mtx);
    }
    pthread_mutex_unlock(&g_test_mtx);
}

/* ---------------- 看门狗 ---------------- */

/*
 * 并发测试有可能**挂死**（而不是失败）。
 *
 * 原因：无锁版本会把缓冲池的哈希表写坏，
 * hash_find 的探测序列可能永远遇不到 EMPTY → 死循环。
 * 或者线程互相等锁 → 死锁。
 *
 * 测试挂死比测试失败更糟 —— 整个测试套件会卡住。
 * 所以加一个看门狗：超时还没跑完就判定失败。
 *
 * 这不是"掩盖问题"：看门狗报告的"疑似死锁"就是真实结论，
 * 和"断言失败"一样是有效的失败信号。
 */
static volatile int g_done = 0;

static inline void *watchdog_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 120; i++) {          /* 120 秒 */
        struct timespec ts;
        ts.tv_sec = 1;
        ts.tv_nsec = 0;
        nanosleep(&ts, NULL);
        if (g_done)
            return NULL;
    }
    if (!g_done) {
        printf("    FAIL 并发测试超时（疑似死锁或无限循环）\n");
        fflush(stdout);
        exit(1);
    }
    return NULL;
}

/* ---------------- 工具 ---------------- */

static inline int run_sql(DB *db, const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (r)
        db_result_free(r);
    return e == DB_OK ? 0 : -1;
}

/* 查一列，返回行数（结果由调用方 free） */
static inline int query_int_col(DB *db, const char *sql, Result **out)
{
    Result *r = NULL;
    if (db_exec(db, sql, &r) != DB_OK) {
        if (r) db_result_free(r);
        return -1;
    }
    int n = db_result_rows(r);
    if (out)
        *out = r;
    else
        db_result_free(r);
    return n;
}

/* 删库文件（含 WAL） */
static inline void remove_db(const char *path)
{
    char wal[256];
    snprintf(wal, sizeof(wal), "%s-wal", path);
    remove(path);
    remove(wal);
}

/* ---------------- 并发 worker 的公共参数 ---------------- */

typedef struct {
    DB *inst;        /* 实例句柄（只读共享） */
    const char *table;
    int  tid;
    int  nrows;
    int  base_id;
    int  errors;
} Worker;

#endif /* TEST_CONC_COMMON_H */
