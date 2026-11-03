/*
 * test_latch.c —— v1.2 步骤 2：按域 latch 框架的验收
 *
 * 6 项：
 *   1. 按全序获取 5 个域 → 全 OK，记账正确
 *   2. **逆序获取被拦下**（且**不记账**）
 *   3. 可重入：同域重复获取 → 计数正确
 *   4. crabbing 例外：同域内允许；跨域**不允许**
 *   5. 持锁集合是**线程本地**的（两个线程互不影响）
 *   6. 默认行为是 **abort**（真·机器证明，用子进程验）
 *
 * ==================== 为什么第 6 项要开子进程 ====================
 *
 * 违规的默认处理是 abort() —— 那会直接杀死测试进程。
 * 想验证"它真的会 abort"，就只能让另一个进程去撞它。
 *
 * 这里用 `system()` 重新跑一遍自己（带一个环境变量当开关），
 * 检查退出码非 0 —— 也就是"确实死了"。
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/latch.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---------------- 违规处理（测试用） ---------------- */

static int g_violations = 0;
static LatchId g_last_violation_id = (LatchId)0;

static void on_violation(LatchId id, const char *msg)
{
    (void)msg;
    g_violations++;
    g_last_violation_id = id;
}

/* 清空当前线程的持锁集合（每个用例开始前调用） */
static void reset_order(void)
{
    for (int i = 1; i < LATCH_NDOMAIN; i++) {
        while (latch_order_count((LatchId)i) > 0)
            latch_order_release((LatchId)i);
    }
    g_violations = 0;
    g_last_violation_id = (LatchId)0;
}

/* ---------------- 1) 全序获取 ---------------- */

/*
 * ⚠️ **v1.2 步骤 4c 勘误：全序变了。**
 *
 * 计划（PLAN-v1.2 §1.3）写的全序是：
 *
 *     catalog → txn → wal → freelist → frame
 *
 * 但真实代码不是这样嵌套的（详见 common/latch.h 的说明）：
 * WAL 钩子和 freelist_set 都是在**页还捏在手里**的时候调的，
 * 所以页比它们更外层。真实全序是：
 *
 *     catalog(1) → txn(2) → frame(3) → wal(4) → freelist(5)
 *
 * 这一项按**新全序**取域。
 */
static void t1_in_order(void)
{
    reset_order();

    CHECK(latch_order_depth() == 0);

    /* 全序：catalog → txn → frame → wal → freelist */
    CHECK(latch_order_acquire(LATCH_CATALOG) == OK);
    CHECK(latch_order_acquire(LATCH_TXN) == OK);
    CHECK(latch_order_acquire(LATCH_FRAME) == OK);
    CHECK(latch_order_acquire(LATCH_WAL) == OK);
    CHECK(latch_order_acquire(LATCH_FREELIST) == OK);

    CHECK(latch_order_depth() == 5);
    CHECK(latch_order_held(LATCH_CATALOG));
    CHECK(latch_order_held(LATCH_TXN));
    CHECK(latch_order_held(LATCH_FRAME));
    CHECK(latch_order_held(LATCH_WAL));
    CHECK(latch_order_held(LATCH_FREELIST));
    CHECK(g_violations == 0);

    /* 逆序释放（后取先放） */
    CHECK(latch_order_release(LATCH_FREELIST) == OK);
    CHECK(latch_order_release(LATCH_WAL) == OK);
    CHECK(latch_order_release(LATCH_FRAME) == OK);
    CHECK(latch_order_release(LATCH_TXN) == OK);
    CHECK(latch_order_release(LATCH_CATALOG) == OK);
    CHECK(latch_order_depth() == 0);
}

/* ---------------- 2) 逆序被拦 ---------------- */

static void t2_reverse_order_rejected(void)
{
    reset_order();
    latch_set_violation_handler(on_violation);

    /* 先拿最高序号的 freelist（步骤 4c 之后全序最高位是 freelist = 5） */
    CHECK(latch_order_acquire(LATCH_FREELIST) == OK);
    CHECK(latch_order_depth() == 1);

    /* 再拿 txn（序号 2 < 5）→ 逆序 */
    CHECK(latch_order_acquire(LATCH_TXN) != OK);
    CHECK(g_violations == 1);
    CHECK(g_last_violation_id == LATCH_TXN);

    /*
     * 【关键】被拦下的获取**不能记账** ——
     * 否则持锁集合会记录一把"其实没拿到"的锁，
     * 后面所有判断都建立在假状态上。
     */
    CHECK(!latch_order_held(LATCH_TXN));
    CHECK(latch_order_depth() == 1);

    /* 逆序释放之后，正序获取就应该正常了 */
    CHECK(latch_order_release(LATCH_FREELIST) == OK);
    CHECK(latch_order_acquire(LATCH_TXN) == OK);
    CHECK(latch_order_held(LATCH_TXN));
    CHECK(g_violations == 1);

    reset_order();
    latch_set_violation_handler(NULL);
}

/* ---------------- 3) 可重入 ---------------- */

static void t3_reentrant(void)
{
    reset_order();

    CHECK(latch_order_acquire(LATCH_CATALOG) == OK);
    CHECK(latch_order_acquire(LATCH_CATALOG) == OK);
    CHECK(latch_order_count(LATCH_CATALOG) == 2);
    CHECK(latch_order_depth() == 1);          /* 域的个数，不是次数 */

    CHECK(latch_order_release(LATCH_CATALOG) == OK);
    CHECK(latch_order_count(LATCH_CATALOG) == 1);
    CHECK(latch_order_depth() == 1);

    CHECK(latch_order_release(LATCH_CATALOG) == OK);
    CHECK(latch_order_count(LATCH_CATALOG) == 0);
    CHECK(latch_order_depth() == 0);
}

/* ---------------- 4) crabbing 例外 ---------------- */

static void t4_crabbing(void)
{
    reset_order();
    latch_set_violation_handler(on_violation);

    /* 同域内：合法 */
    CHECK(latch_order_acquire(LATCH_FRAME) == OK);
    CHECK(latch_order_acquire_crabbing(LATCH_FRAME) == OK);
    CHECK(latch_order_count(LATCH_FRAME) == 2);
    CHECK(g_violations == 0);

    /* 跨域：crabbing 不适用 —— 那是普通逆序 */
    CHECK(latch_order_acquire_crabbing(LATCH_TXN) != OK);
    CHECK(g_violations == 1);
    CHECK(!latch_order_held(LATCH_TXN));

    reset_order();
    latch_set_violation_handler(NULL);
}

/* ---------------- 5) 线程本地 ---------------- */

typedef struct {
    int  violations_seen;
    int  depth_seen;
    bool got_catalog;
} OtherThread;

static void *other_thread(void *arg)
{
    OtherThread *o = (OtherThread *)arg;

    latch_set_violation_handler(on_violation);
    CHECK(latch_order_acquire(LATCH_TXN) == OK);
    o->got_catalog = latch_order_held(LATCH_TXN);
    o->depth_seen = latch_order_depth();
    CHECK(latch_order_release(LATCH_TXN) == OK);
    latch_set_violation_handler(NULL);
    return NULL;
}

static void t5_thread_local(void)
{
    reset_order();

    /* 主线程持有 catalog */
    CHECK(latch_order_acquire(LATCH_CATALOG) == OK);

    /* 另一个线程的持锁集合与主线程**互不影响** */
    OtherThread o = { 0, 0, false };
    pthread_t th;
    CHECK(pthread_create(&th, NULL, other_thread, &o) == 0);
    pthread_join(th, NULL);

    CHECK(o.got_catalog);            /* 线程自己拿到了 txn */
    CHECK(o.depth_seen == 1);        /* 它只看得到自己的持有 */
    CHECK(latch_order_depth() == 1); /* 主线程仍然只持有 catalog */
    CHECK(latch_order_held(LATCH_CATALOG));
    CHECK(!latch_order_held(LATCH_TXN));

    CHECK(latch_order_release(LATCH_CATALOG) == OK);
}

/* ---------------- 6) 默认行为是 abort ---------------- */

/*
 * 子进程：撞一次逆序，让默认的 abort() 发作。
 *
 * 用环境变量当开关 —— test/run.sh 不会传命令行参数给测试。
 */
static int crash_child(void)
{
    /*
     * 先打一个标记 —— 父进程要靠它确认"子进程真的跑到撞点了"。
     *
     * 没有这个标记的话，"命令没找到"也会是非 0 退出码，
     * 测试就会**假通过**。
     */
    printf("CHILD-REACHED-CHECKPOINT\n");
    fflush(stdout);

    /* 手里没有任何域的锁，先拿 frame，再拿 txn → 逆序 → abort */
    latch_order_acquire(LATCH_FRAME);
    latch_order_acquire(LATCH_TXN);

    /* 走到这里说明**没有 abort** —— 那就是失败 */
    return 0;
}

static void t6_default_aborts(void)
{
    reset_order();
    latch_set_violation_handler(NULL);   /* 恢复默认：abort */

#ifdef _WIN32
    int rc = system("set DB_LATCH_CRASH=1 && .\\build\\test_latch.exe > build\\latch_crash.out 2>NUL");
#else
    int rc = system("DB_LATCH_CRASH=1 ./build/test_latch > build/latch_crash.out 2>/dev/null");
#endif
    CHECK(rc != 0);                     /* 非 0 = 撞死 = 断言真的生效 */

    /* 并且确认它**确实撞到了点**（不是"命令没找到"造成的非 0） */
    char buf[256];
    memset(buf, 0, sizeof(buf));
    FILE *f = fopen("build/latch_crash.out", "r");
    CHECK(f != NULL);
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        CHECK(n > 0);
        CHECK(strstr(buf, "CHILD-REACHED-CHECKPOINT") != NULL);
    }
}

/* ---------------- main ---------------- */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* 子进程模式：只做"撞一次逆序" */
    if (getenv("DB_LATCH_CRASH")) {
        return crash_child();
    }

    printf("test_latch:\n");

    printf("  1. 按全序获取 5 个域\n");
    t1_in_order();

    printf("  2. 逆序获取被拦下（且不记账）\n");
    t2_reverse_order_rejected();

    printf("  3. 可重入计数\n");
    t3_reentrant();

    printf("  4. crabbing 例外：同域允许 / 跨域拒绝\n");
    t4_crabbing();

    printf("  5. 持锁集合是线程本地的\n");
    t5_thread_local();

    printf("  6. 默认行为是 abort（子进程实撞）\n");
    t6_default_aborts();

    if (failures == 0) {
        printf("  6 项验收全部通过\n");
        return 0;
    }
    printf("  %d 项断言失败\n", failures);
    return 1;
}
