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

#include "common/error.h"

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

/*
 * ==================== v1.2 步骤 2：按域的 latch 框架 ====================
 *
 * 【持锁集合是线程本地的】
 *
 * "当前持有哪些域的锁"是**每个线程各自的状态**，不是共享状态 ——
 * 和 t_depth 同一个道理（硬规则 5）。
 *
 * 【域编号 = 获取序号】
 *
 * 枚举值就是获取序号（catalog=1 … frame=5），
 * 所以"全序检查"就是一句"已持有的域的序号必须都比我小"。
 *
 * 【违规处理函数是共享的 —— 但它不是"当前是谁"】
 *
 * 它是**配置**：进程启动时设一次（只有测试会设），
 * 之后只读。所以不需要保护。
 */

static _Thread_local int t_held[LATCH_NDOMAIN];   /* 每个域的持有次数 */
static _Thread_local int t_domains;               /* 一共持有几个域 */

static LatchViolationFn g_violation_fn;

const char *latch_domain_name(LatchId id)
{
    switch (id) {
    case LATCH_CATALOG:  return "catalog";
    case LATCH_TXN:      return "txn";
    case LATCH_WAL:      return "wal";
    case LATCH_FREELIST: return "freelist";
    case LATCH_FRAME:    return "frame";
    default:             return "?";
    }
}

/*
 * 违反全序时的处理。
 *
 * 默认：打印 + abort()。
 *
 * 为什么是 abort 而不是返回错误 ——
 * 逆序获取意味着**可能死锁**，而"可能死锁"的代码一旦跑起来，
 * 症状是"程序卡住"，排查要靠猜。
 * 当场抓住，成本差一个数量级。
 */
static void latch_violation(LatchId id, const char *msg)
{
    if (g_violation_fn) {
        g_violation_fn(id, msg);
        return;
    }
    fprintf(stderr, "latch: 违反获取全序: %s\n", msg);
    fflush(stderr);
    abort();
}

void latch_set_violation_handler(LatchViolationFn fn)
{
    g_violation_fn = fn;
}

/*
 * 全序检查：已持有的域，序号必须**都比我小**。
 *
 * 同域重复获取（可重入）不算逆序 —— 那是嵌套调用同一把锁，
 * 顺序上当然"没动过"。
 */
static bool latch_order_violates(LatchId id)
{
    for (int h = 1; h < LATCH_NDOMAIN; h++) {
        if (t_held[h] > 0 && h >= (int)id)
            return true;
    }
    return false;
}

int latch_order_acquire(LatchId id)
{
    if (id <= 0 || id >= LATCH_NDOMAIN)
        return E_INVAL;

    /* 可重入：同域重复获取 → 计数 +1 */
    if (t_held[id] > 0) {
        t_held[id]++;
        return OK;
    }

    if (latch_order_violates(id)) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "获取 %s（序号 %d）时已持有更高序号的域",
                 latch_domain_name(id), (int)id);
        latch_violation(id, msg);
        return E_INVAL;              /* 只有装了处理函数才会走到这 */
    }

    t_held[id] = 1;
    t_domains++;
    return OK;
}

int latch_order_acquire_crabbing(LatchId id)
{
    if (id <= 0 || id >= LATCH_NDOMAIN)
        return E_INVAL;

    /*
     * crabbing 只在**页与页之间**成立 —— 也就是"手里已经有同一域的锁"。
     *
     * 如果手里握的是别的域，那就不是页内重排，而是**跨域逆序** ——
     * 那必须走正常的全序检查，不能借 crabbing 的名义绕过。
     *
     * 域级别检查**管不到**同域内部的顺序（域只看"frame 这个域"），
     * 所以 crabbing 在域级别等价于普通获取 ——
     * 它存在的意义是**在代码里留下显式标记**：
     * 让读代码的人知道这条路径是经过论证的例外，而不是随手写的。
     * 页与页之间的树高顺序，步骤 4 的 per-frame 检查才真正管。
     */
    if (t_domains > 0 && t_held[id] <= 0) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "crabbing 只能用于同域内：手里有别的域，却要获取 %s",
                 latch_domain_name(id));
        latch_violation(id, msg);
        return E_INVAL;
    }

    if (t_held[id] == 0)
        t_domains++;
    t_held[id]++;
    return OK;
}

int latch_order_release(LatchId id)
{
    if (id <= 0 || id >= LATCH_NDOMAIN)
        return E_INVAL;

    if (t_held[id] <= 0) {
        fprintf(stderr,
                "latch: 释放未持有的域 %s（当前线程）\n",
                latch_domain_name(id));
        fflush(stderr);
        abort();
    }

    t_held[id]--;
    if (t_held[id] == 0)
        t_domains--;
    return OK;
}

bool latch_order_held(LatchId id)
{
    if (id <= 0 || id >= LATCH_NDOMAIN)
        return false;
    return t_held[id] > 0;
}

int latch_order_count(LatchId id)
{
    if (id <= 0 || id >= LATCH_NDOMAIN)
        return 0;
    return t_held[id];
}

int latch_order_depth(void)
{
    return t_domains;
}

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
