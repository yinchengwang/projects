/*
 * test_lock_inversion.c —— v1.2 步骤 7 的 A7「反向验证」
 *
 * ==================== 这一项在验什么 ====================
 *
 * 前面几步已经把"按域的锁 + 获取全序断言"落地了：
 *
 *     catalog(1) → txn(2) → btree(3) → frame(4) → wal(5) → freelist(6)
 *
 * 全序断言的机制是"每取一次域锁就检查一次"（common/latch.c）。
 * 但**机制存在**不等于**它真的能抓住东西** ——
 * 断言可能写错了、可能根本没人触发、可能装了处理函数就名存实亡。
 *
 * 所以这一项做的是**反向验证** —— 用机器手段证明两句话：
 *
 *   (1) "锁序全序断言"真的能抓住逆序获取
 *   (2) "没有断言 = 没有防线"
 *
 * 4 项：
 *   1. 真实调用点上的逆序会被断言拦下
 *   2. 默认行为真的是 abort（子进程实撞）
 *   3. 只记录不中止的 handler 拦不住逆序：那把锁照样被取到
 *   4. 断言彻底关掉 → 真死锁 → 看门狗抓得到
 *
 * ==================== 逆序点选在"真的"生产函数上 ====================
 *
 * 逆序点不是在框架里手搓的，是两条**真实生产调用点**：
 *
 *     pager_get(pg, pid, &frame)   ← 真实调用点：拿到 frame 域（序号 4）
 *     txn_begin(&tt, &xid)         ← 真实调用点：要取 txn 域（序号 2）
 *
 * 手里已经握着序号更大的域（frame=4）却要取序号更小的域（txn=2）
 * —— 这就是**逆序获取**。
 *
 * 为什么这两行值得测：
 * `pager_get` 之后页的**内容锁还在手里**（要等 pager_unpin 才放），
 * 所以"拿着页去开事务"这个形状一旦出现在生产代码里，
 * 就是货真价实的 frame → txn 逆序。
 *
 * ==================== 坑 M8-1：违规的"取"不记账，但配对的"放"会 abort ====================
 *
 * latch_order_acquire 被断言拦下时**不记账**（这是对的：
 * 不能记录一把"其实没拿到"的锁）。
 * 但生产代码里，每个域的获取都**配一个释放**：
 *
 *     txn_begin → txn_lock（取 txn 域）→ ... → txn_unlock（放 txn 域）
 *
 * 于是"取的时候被拦下"会让"放的时候"报
 * "释放未持有的域 txn"并 abort。
 *
 * 也就是说：**只记录不中止的 handler 并不能真的让逆序活下来** ——
 * 进程只是换了个地方死。
 *
 * 第 3 项要证明的恰恰是"逆序真的被执行下去了"，
 * 所以那个 handler 里必须把**记账**补平（见 on_violation_fixup）：
 * 变的只是记账，**真实锁的持有关系一点没变**。
 *
 * 坑 M8-2：latch 断言是**事后**检查（真实锁拿到之后才记账）。
 *   它能抓住"逆序获取成功了"，抓不住"两次逆序获取都阻塞在真实锁上"
 *   —— 那种情况下断言根本没有发作的机会（见第 4 项的说明）。
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "catalog/catalog.h"
#include "common/error.h"
#include "common/latch.h"
#include "storage/pager.h"
#include "txn/txn.h"

/* 断言宏、失败计数、屏障（测试自己的同步原语）都在这里 */
#include "conc_common.h"

#define PATH_T1 "test/tmp/lockinv_t1.db"
#define PATH_CRASH "test/tmp/lockinv_crash.db"
#define CRASH_OUT "build/lock_inversion_crash.out"

/* 违规处理：只记录，不中止 */
static int  g_violations = 0;
static char g_msg[256];
static LatchId g_last_id = (LatchId)0;

/* handler 重入保护（记账修复内部会再次触发 latch 的检查） */
static int g_in_handler = 0;

/*
 * 只计数的违规处理函数。
 *
 * 装上它之后，latch_order_acquire 不再 abort，而是返回 E_INVAL。
 */
static void on_violation_count(LatchId id, const char *msg)
{
    if (g_in_handler)
        return;
    g_in_handler = 1;

    g_violations++;
    g_last_id = id;
    snprintf(g_msg, sizeof(g_msg), "%s", msg ? msg : "");

    g_in_handler = 0;
}

/*
 * 只记录 + **记账修复**的违规处理函数（坑 M8-1）。
 *
 * 为什么必须修复记账：
 *   被拦下的获取不记账 → 配对的释放会报"释放未持有的域"并 abort。
 *   进程死了，就没法证明"逆序真的被执行下去了"。
 *
 * 修复的三个动作：
 *   1. 先把 frame 的记账放掉（**真实锁不动** —— 内容锁还捏在手里）
 *   2. 按合法顺序取 txn（记账）
 *   3. 再把 frame 的记账取回来
 *
 * 修完之后，账本和真实锁的持有关系重新一致，
 * 配对的 latch_order_release 也就不会 abort 了。
 *
 * ⚠️ 变的只是**记账**，不是锁的顺序：
 *    逆序（frame → txn）在真实锁层面**照样发生了**。
 */
static void on_violation_fixup(LatchId id, const char *msg)
{
    if (g_in_handler)
        return;
    g_in_handler = 1;

    g_violations++;
    g_last_id = id;
    snprintf(g_msg, sizeof(g_msg), "%s", msg ? msg : "");

    if (id == LATCH_TXN && latch_order_held(LATCH_FRAME)) {
        latch_order_release(LATCH_FRAME);
        latch_order_acquire(LATCH_TXN);
        latch_order_acquire(LATCH_FRAME);
    }

    g_in_handler = 0;
}

/*
 * 空的 no-op 违规处理函数：检查照跑，但不中止。
 *
 * 装上它 = **关掉断言**（第 4 项用）。
 */
static void on_violation_noop(LatchId id, const char *msg)
{
    (void)id;
    (void)msg;
    /* 什么都不做 —— 断言被"关掉"了 */
}

/* ---------------- 公共准备 ---------------- */

/* 造一个只有一个页的 pager，返回页号 */
static int make_pager(const char *path, Pager **out, u32 *pid_out)
{
    remove(path);

    Pager *pg = NULL;
    if (pager_open(path, 16, &pg) != OK)
        return -1;

    u32 pid = 0;
    PageFrame *f = NULL;
    if (pager_alloc(pg, &pid, &f) != OK) {
        pager_close(pg);
        return -1;
    }
    pager_unpin(pg, pid, true);

    *out = pg;
    *pid_out = pid;
    return 0;
}

/* ---------------- 1) 真实调用点上的逆序被拦下 ---------------- */

/*
 * 两段：
 *
 *   1a 框架层：逆序获取返回 E_INVAL（生产代码把返回值丢掉了，
 *      这里自己拿一份，证明**它返回的确实是 E_INVAL**）
 *   1b 真实生产调用点：pager_get → txn_begin 真的触发断言
 */
static void t1_real_call_site(void)
{
    Pager *pg = NULL;
    u32 pid = 0;
    if (make_pager(PATH_T1, &pg, &pid) != 0) {
        printf("    FAIL 造不出 pager\n");
        failures++;
        return;
    }

    /* ---- 1a：逆序获取被拦下，返回 E_INVAL ---- */
    latch_set_violation_handler(on_violation_count);
    g_violations = 0;
    g_msg[0] = '\0';

    PageFrame *f = NULL;
    CHECK(pager_get(pg, pid, &f) == OK);
    CHECK(latch_order_held(LATCH_FRAME));      /* 真实调用点确实拿到了 frame 域 */

    int rc = latch_order_acquire(LATCH_TXN);   /* 逆序：手里是 frame(4)，要取 txn(2) */
    CHECK(rc == E_INVAL);
    CHECK(g_violations == 1);
    CHECK(g_last_id == LATCH_TXN);
    CHECK(strstr(g_msg, "frame") != NULL);     /* 报错信息里同时出现两个域 */
    CHECK(strstr(g_msg, "txn") != NULL);
    CHECK(!latch_order_held(LATCH_TXN));        /* 被拦下的获取不记账 */

    CHECK(pager_unpin(pg, pid, false) == OK);
    printf("    逆序获取返回 %d（E_INVAL），违规计数 %d，报错：%s\n",
           rc, g_violations, g_msg);

    /* ---- 1b：真实生产调用点 ---- */
    latch_set_violation_handler(on_violation_fixup);
    g_violations = 0;
    g_msg[0] = '\0';

    TxnTable tt;
    txn_table_init(&tt, 0);

    CHECK(pager_get(pg, pid, &f) == OK);
    CHECK(latch_order_held(LATCH_FRAME));

    u32 xid = 0;
    CHECK(txn_begin(&tt, &xid) == OK);         /* ← 真实生产函数上的逆序 */
    CHECK(g_violations == 1);
    CHECK(strstr(g_msg, "frame") != NULL);
    CHECK(strstr(g_msg, "txn") != NULL);
    CHECK(xid != 0);                           /* txn 段真的跑了 → 逆序真的执行了 */

    CHECK(pager_unpin(pg, pid, false) == OK);
    CHECK(txn_commit(&tt, xid) == OK);

    latch_set_violation_handler(NULL);
    pager_close(pg);
    remove(PATH_T1);

    printf("    生产调用点 pager_get → txn_begin：违规计数 %d，报错：%s\n",
           g_violations, g_msg);
}

/* ---------------- 2) 默认行为真的是 abort ---------------- */

/*
 * 子进程：撞一次**真实调用点**上的逆序，让默认的 abort() 发作。
 *
 * 用环境变量当开关 —— test/run.sh 不会给测试传命令行参数。
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

    Pager *pg = NULL;
    u32 pid = 0;
    if (make_pager(PATH_CRASH, &pg, &pid) != 0)
        return 2;

    /* 不装 handler → 走默认的"打印 + abort" */
    latch_set_violation_handler(NULL);

    PageFrame *f = NULL;
    pager_get(pg, pid, &f);          /* 真实调用点：持有 frame 域 */

    TxnTable tt;
    txn_table_init(&tt, 0);
    u32 xid = 0;
    txn_begin(&tt, &xid);            /* 真实调用点：逆序 → abort */

    /* 走到这里说明**没有 abort** —— 那就是失败 */
    printf("CHILD-NOT-ABORTED\n");
    fflush(stdout);
    return 0;
}

static void t2_default_aborts(void)
{
    latch_set_violation_handler(NULL);     /* 恢复默认：abort */

#ifdef _WIN32
    int rc = system("set DB_LOCKINV_CRASH=1 && .\\build\\test_lock_inversion.exe > build\\lock_inversion_crash.out 2>NUL");
#else
    int rc = system("DB_LOCKINV_CRASH=1 ./build/test_lock_inversion > build/lock_inversion_crash.out 2>/dev/null");
#endif
    CHECK(rc != 0);                       /* 非 0 = 撞死 = 断言真的生效 */

    /* 并且确认它**确实撞到了点**（不是"命令没找到"造成的非 0） */
    char buf[512];
    memset(buf, 0, sizeof(buf));
    FILE *fp = fopen(CRASH_OUT, "r");
    CHECK(fp != NULL);
    if (fp) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        CHECK(n > 0);
        CHECK(strstr(buf, "CHILD-REACHED-CHECKPOINT") != NULL);
        /* 反过来也确认一下：它没走到"没有 abort"那一行 */
        CHECK(strstr(buf, "CHILD-NOT-ABORTED") == NULL);
    }
    remove(PATH_CRASH);

    printf("    子进程退出码 %d（≠0 = abort），撞点标记已确认\n", rc);
}

/* ---------------- 3) 只记录不中止 → 逆序照样执行下去 ---------------- */

/*
 * 这一项证明的是："断言是唯一的防线"。
 *
 * 装一个**只记录不中止**的 handler 之后：
 *   - 违规照样被记录（计数 +1）
 *   - 但**那把锁照样被取到**，逆序**真的被执行下去了**
 *
 * "真的被执行"的证据不是"没报错"，而是
 * **txn 域的临界区真的跑了**：
 *   - xid 被分配出来了
 *   - 事务进了活跃表
 * 这两件事都发生在 txn_lock 与 txn_unlock 之间 ——
 * 也就是说，**txn 域的真实互斥量确实被本线程取到了**，
 * 而那时 frame 域还握在手里（逆序成立）。
 */
static void t3_record_only_handler(void)
{
    Pager *pg = NULL;
    u32 pid = 0;
    if (make_pager(PATH_T1, &pg, &pid) != 0) {
        printf("    FAIL 造不出 pager\n");
        failures++;
        return;
    }

    latch_set_violation_handler(on_violation_fixup);
    g_violations = 0;
    g_msg[0] = '\0';

    TxnTable tt;
    txn_table_init(&tt, 0);

    PageFrame *f = NULL;
    CHECK(pager_get(pg, pid, &f) == OK);
    CHECK(latch_order_held(LATCH_FRAME));      /* 逆序的前提：手里握着 frame */

    u32 xid = 0;
    int rc = txn_begin(&tt, &xid);             /* 逆序：被记录，但没有被拦下 */

    CHECK(rc == OK);                           /* ← 没被拦下：事务开成了 */
    CHECK(g_violations == 1);                  /* 违规照样被记录 */
    CHECK(xid != 0);                           /* xid 被分配 → txn 临界区跑过了 */
    int nactive = txn_nactive(&tt);
    CHECK(nactive == 1);                       /* 进了活跃表 → 同上 */
    CHECK(latch_order_held(LATCH_FRAME));       /* 逆序期间 frame 域一直握着 */

    CHECK(pager_unpin(pg, pid, false) == OK);
    CHECK(txn_commit(&tt, xid) == OK);

    latch_set_violation_handler(NULL);
    pager_close(pg);
    remove(PATH_T1);

    printf("    违规计数 %d，事务照样开成：xid=%u，活跃事务 %d"
           "（txn 域的真实锁被取到了，逆序被执行）\n",
           g_violations, xid, nactive);
}

/* ---------------- 4) 断言彻底关掉 → 真死锁被看门狗抓到 ---------------- */

/*
 * 构造一处**真实的互等**（真死锁，不是模拟）：
 *
 *   线程 A：pager_get(页 P) 拿到 frame 内容锁 → 再去取 catalog 写锁
 *   线程 B：先取 catalog 写锁 → 再 pager_get(同一个页 P)
 *
 * 两边各自握着对方要的那把锁 → 互等。
 *
 * "关掉断言"= 装一个空的 no-op violation handler
 *（检查照跑，但不中止）—— 等效于关掉断言。
 *
 * 【屏障保证时序】
 *   A 先拿到 frame 锁、B 先拿到 catalog 锁，两人都拿到之后才同时去要对方的锁。
 *   少了这一步，谁先谁后不确定，就可能一方在另一方拿到之前就把两把锁都拿到 ——
 *   那样就构不成互等了。
 *
 * ⚠️ 坑 M8-2：这一处死锁里，latch 断言**没有发作的机会** ——
 *    因为两次"逆序获取"都阻塞在**真实锁**的获取里，
 *    而 latch 断言是在真实锁拿到**之后**才检查的。
 *    也就是说：这一项证明的是"没有断言就真的会死锁"，
 *    顺带也暴露了"断言是事后检查"这个边界。
 *
 * ⚠️ 死锁发生后那两个线程永远醒不过来，
 *    所以主线程必须用 _exit(0) 直接退出（不能 return）。
 */
static Pager  *g_pg4 = NULL;
static Catalog *g_cat4 = NULL;
static u32     g_pid4 = 0;

static volatile int g_a_has_frame = 0;
static volatile int g_b_has_cat = 0;
static volatile int g_a_done = 0;
static volatile int g_b_done = 0;

/*
 * 两个"探针"线程 —— 用来把"两个线程没结束"升级成
 * "**两把锁真的还互相挡着**"的机器证据：
 *
 *   探针 1 要 catalog 写锁 → 会被 B 挡住（B 一直持有）
 *   探针 2 要页 P 的内容锁 → 会被 A 挡住（A 一直持有）
 *
 * 两个探针都在 300 毫秒内没拿到锁 —— 说明这不是"线程慢"，
 * 而是**互等**：A 等 B 的锁，B 等 A 的锁。
 */
static volatile int g_probe_cat_done = 0;
static volatile int g_probe_frame_done = 0;

static void *probe_catalog(void *arg)
{
    (void)arg;
    catalog_write_lock(g_cat4);          /* ← 被 B 挡住 */
    g_probe_cat_done = 1;
    return NULL;
}

static void *probe_frame(void *arg)
{
    (void)arg;
    PageFrame *f = NULL;
    pager_get(g_pg4, g_pid4, &f);         /* ← 被 A 挡住 */
    g_probe_frame_done = 1;
    return NULL;
}

static void *worker_a(void *arg)
{
    (void)arg;

    PageFrame *f = NULL;
    if (pager_get(g_pg4, g_pid4, &f) != OK) {
        g_a_done = 1;
        return NULL;
    }
    g_a_has_frame = 1;

    barrier_wait();                       /* 等 B 也拿到 catalog 写锁 */

    catalog_write_lock(g_cat4);           /* ← 阻塞在这里：B 持着写锁 */
    g_a_done = 1;                         /* 到不了这里（除非没死锁） */
    return NULL;
}

static void *worker_b(void *arg)
{
    (void)arg;

    catalog_write_lock(g_cat4);           /* 拿到 catalog 写锁 */
    g_b_has_cat = 1;

    barrier_wait();                       /* 等 A 也拿到 frame 内容锁 */

    PageFrame *f = NULL;
    pager_get(g_pg4, g_pid4, &f);          /* ← 阻塞在这里：A 持着内容锁 */
    g_b_done = 1;
    return NULL;
}

static void t4_deadlock_watchdog(void)
{
    const char *path = "test/tmp/lockinv_t4.db";
    remove(path);

    Pager *pg = NULL;
    if (pager_open(path, 16, &pg) != OK) {
        printf("    FAIL 造不出 pager\n");
        failures++;
        printf("  %d 项断言失败\n", failures);
        fflush(stdout);
        _exit(1);
    }

    /*
     * catalog 要在**空库**上开（catalog_open 看到空库才会 bootstrap），
     * 所以先开 catalog，再造测试用的页。
     */
    Catalog *cat = NULL;
    if (catalog_open(pg, NULL, NULL, &cat) != OK) {
        printf("    FAIL 开不出 catalog\n");
        failures++;
        fflush(stdout);
        _exit(1);
    }

    u32 pid = 0;
    PageFrame *f = NULL;
    if (pager_alloc(pg, &pid, &f) != OK) {
        printf("    FAIL 造不出页\n");
        failures++;
        fflush(stdout);
        _exit(1);
    }
    pager_unpin(pg, pid, true);

    g_pg4 = pg;
    g_cat4 = cat;
    g_pid4 = pid;

    /* 关掉断言：装一个空的 no-op 处理函数 */
    latch_set_violation_handler(on_violation_noop);

    barrier_init(2);

    pthread_t th_a, th_b;
    if (pthread_create(&th_a, NULL, worker_a, NULL) != 0 ||
        pthread_create(&th_b, NULL, worker_b, NULL) != 0) {
        printf("    FAIL 起不了线程\n");
        failures++;
        fflush(stdout);
        _exit(1);
    }

    /* 看门狗：等 5 秒 */
    for (int i = 0; i < 5; i++) {
        struct timespec ts;
        ts.tv_sec = 1;
        ts.tv_nsec = 0;
        nanosleep(&ts, NULL);
        if (g_a_done || g_b_done)
            break;
    }

    int deadlocked = (!g_a_done) && (!g_b_done);

    /*
     * 机器证据：两个探针分别去要那两把锁，
     * 300 毫秒内都拿不到 → 两把锁真的互相挡着（不是"线程慢"）。
     */
    pthread_t th_pc, th_pf;
    pthread_create(&th_pc, NULL, probe_catalog, NULL);
    pthread_create(&th_pf, NULL, probe_frame, NULL);
    {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 300 * 1000 * 1000;      /* 300 毫秒 */
        nanosleep(&ts, NULL);
    }
    CHECK(!g_probe_cat_done);      /* catalog 写锁仍被 B 持有 */
    CHECK(!g_probe_frame_done);    /* frame 内容锁仍被 A 持有 */

    if (deadlocked) {
        printf("    A 拿到 frame 锁=%d，B 拿到 catalog 锁=%d；"
               "5 秒内两个线程都没结束 → 真死锁被抓到\n",
               g_a_has_frame, g_b_has_cat);
        printf("    探针确认：catalog 写锁仍被占=%d，"
               "frame 内容锁仍被占=%d（互等，不是线程慢）\n",
               !g_probe_cat_done, !g_probe_frame_done);
    } else {
        printf("    FAIL 预期死锁，但工作线程结束了（A done=%d, B done=%d）\n",
               g_a_done, g_b_done);
        failures++;
    }

    if (failures == 0) {
        printf("  4 项验收全部通过 [PASS]\n");
    } else {
        printf("  %d 项断言失败\n", failures);
    }
    fflush(stdout);

    /*
     * 死锁了，那两个线程永远不会醒 ——
     * 必须直接退出进程，不能 return（return 会等它们，测试就永远挂住了）。
     *
     * 用 _exit 而不是 exit：exit 会去冲 stdio 缓冲区，
     * 而死锁的线程可能正卡在 printf 里、攥着 stdout 的锁 —— 那就卡死了。
     */
    _exit(failures == 0 ? 0 : 1);
}

/* ---------------- main ---------------- */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* 子进程模式：只做"撞一次真实调用点上的逆序" */
    if (getenv("DB_LOCKINV_CRASH"))
        return crash_child();

    printf("test_lock_inversion（集成）:\n");

    printf("  1. 真实调用点上的逆序会被断言拦下\n");
    t1_real_call_site();

    printf("  2. 默认行为真的是 abort（子进程实撞）\n");
    t2_default_aborts();

    printf("  3. 只记录不中止的 handler 拦不住逆序\n");
    t3_record_only_handler();

    printf("  4. 断言彻底关掉 → 真死锁被看门狗抓到\n");
    t4_deadlock_watchdog();   /* 内部 _exit —— 死锁后线程永远醒不过来 */

    return 1;   /* 不会走到这里 */
}
