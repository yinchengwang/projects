/*
 * test_frame_domain.c —— v1.2 步骤 4c（子步骤 4c-2）：frame 域的并发保护
 *
 * 5 项：
 *   1. **同一页并发读-改-写不丢更新**：8 个线程同时给同一页的计数器 +1，
 *      结果必须正好是 8×N —— 少一个就是丢了更新（页内容被撕裂）
 *   2. **内容锁真的互斥**：两个线程不能同时站在同一页的临界区里
 *   3. **pin/unpin 配对**：pin_count 与内容锁同步，重复 get 靠递归锁不死
 *   4. **WAL 钩子在 frame 域内被调**：锁序 frame → wal 的机器检查
 *   5. **逐出不串页**：池子很小、页很多，页内容不能张冠李戴
 *
 * ==================== 第 1 项在抓什么 ====================
 *
 * 帧的内容锁（content latch）保护的是"页的内容在借用期不被改"。
 *
 * 拿页的路径是：
 *
 *     pager_get(pager, pid, &f);    ← 借到这一页
 *     读 f->data → 改 f->data      ← 直接改内存里的页
 *     pager_unpin(pager, pid, true);
 *
 * 没有内容锁时，两个线程可以**同时**借到同一页：
 *
 *   线程 A 读到计数 = 5
 *   线程 B 也读到计数 = 5      ← B 在 A 写回去之前读的
 *   A 写 6，B 也写 6          ← 一次 +1 被吞掉了
 *
 * 计数器少涨了 = 有人写的那一份**被覆盖**了。
 * 在数据库里，这一幕对应的是"两行数据写进同一个 slot" ——
 * 不是少一行，而是**页内容被撕裂**。
 *
 * ==================== 第 4 项在抓什么 ====================
 *
 * WAL 铁律："页落盘前，对应的日志必须先落盘"。
 * pager 的做法是刷页前问钩子，钩子去 fsync 日志。
 *
 * 这个钩子必须在 **frame 域**（内容锁）之内被调 ——
 * 否则"日志已落盘"和"页内容没被改"这两件事之间就有缝：
 * 另一个线程可以在钩子返回之后、页写盘之前把页改了。
 *
 * 钩子里查 latch_order_held(LATCH_FRAME) 就是在查这件事。
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/latch.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH   "test/tmp/frame_domain.db"

#define NTHREAD 8
#define ITERS   2000        /* 每线程的 +1 次数 */
#define NFRAMES 16          /* 池子故意做小，逼出逐出 */
#define NPAGES  64          /* 页数远大于帧数 → 一定发生逐出 */

/* 页尾 8 字节做测试用：
 *   [PAGE_SIZE-8] 计数器（并发 +1 的目标）
 *   [PAGE_SIZE-4] 标记（写进去的页号，用来验"没有串页"） */
#define CTR_OFF  (PAGE_SIZE - 8)
#define TAG_OFF  (PAGE_SIZE - 4)

/* ---------------- 1) 同一页并发读-改-写 ---------------- */

typedef struct {
    Pager *pg;
    u32    pid;
} IncWorker;

static void *inc_worker(void *arg)
{
    IncWorker *w = (IncWorker *)arg;

    for (int i = 0; i < ITERS; i++) {
        PageFrame *f = NULL;
        if (pager_get(w->pg, w->pid, &f) != OK)
            continue;

        /*
         * 故意**不做原子读-改-写** ——
         * 这一项验的就是"内容锁挡住了两个线程同时改同一页"，
         * 用原子操作的话锁没了也测不出来。
         */
        u32 v = get_u32(f->data + CTR_OFF);
        put_u32(f->data + CTR_OFF, v + 1);

        pager_unpin(w->pg, w->pid, true);
    }
    return NULL;
}

static void t1_no_lost_update(void)
{
    remove(PATH);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, NFRAMES, &pg) == OK);
    if (!pg)
        return;

    /* 建一页当目标 */
    u32 pid = 0;
    PageFrame *f = NULL;
    CHECK(pager_alloc(pg, &pid, &f) == OK);
    put_u32(f->data + CTR_OFF, 0);
    pager_unpin(pg, pid, true);

    pthread_t th[NTHREAD];
    IncWorker w[NTHREAD];
    for (int i = 0; i < NTHREAD; i++) {
        w[i].pg = pg;
        w[i].pid = pid;
        CHECK(pthread_create(&th[i], NULL, inc_worker, &w[i]) == 0);
    }
    for (int i = 0; i < NTHREAD; i++)
        pthread_join(th[i], NULL);

    /* 读回来核对 */
    CHECK(pager_get(pg, pid, &f) == OK);
    u32 got = get_u32(f->data + CTR_OFF);
    pager_unpin(pg, pid, false);

    u32 want = (u32)(NTHREAD * ITERS);
    CHECK(got == want);
    printf("    %d 线程 × %d 次 +1：计数器 %u（应为 %u）\n",
           NTHREAD, ITERS, got, want);

    pager_close(pg);
    remove(PATH);
}

/* ---------------- 2) 内容锁互斥 ---------------- */

static _Atomic int g_in_cs;      /* 当前在临界区里的线程数 */
static _Atomic int g_overlap;    /* 发现"两个线程同时在临界区"的次数 */

typedef struct {
    Pager *pg;
    u32    pid;
} CsWorker;

static void *cs_worker(void *arg)
{
    CsWorker *w = (CsWorker *)arg;

    for (int i = 0; i < 200; i++) {
        PageFrame *f = NULL;
        if (pager_get(w->pg, w->pid, &f) != OK)
            continue;

        /* 站在"临界区"里 */
        int prev = atomic_fetch_add(&g_in_cs, 1);
        if (prev != 0)
            atomic_fetch_add(&g_overlap, 1);

        /* 做点事，把临界区撑开一点 */
        u32 v = get_u32(f->data + CTR_OFF);
        put_u32(f->data + CTR_OFF, v + 1);
        for (volatile int k = 0; k < 200; k++) { }

        atomic_fetch_sub(&g_in_cs, 1);

        pager_unpin(w->pg, w->pid, true);
    }
    return NULL;
}

static void t2_content_latch_exclusive(void)
{
    remove(PATH);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, NFRAMES, &pg) == OK);
    if (!pg)
        return;

    u32 pid = 0;
    PageFrame *f = NULL;
    CHECK(pager_alloc(pg, &pid, &f) == OK);
    put_u32(f->data + CTR_OFF, 0);
    pager_unpin(pg, pid, true);

    atomic_store(&g_in_cs, 0);
    atomic_store(&g_overlap, 0);

    pthread_t th[NTHREAD];
    CsWorker w[NTHREAD];
    for (int i = 0; i < NTHREAD; i++) {
        w[i].pg = pg;
        w[i].pid = pid;
        CHECK(pthread_create(&th[i], NULL, cs_worker, &w[i]) == 0);
    }
    for (int i = 0; i < NTHREAD; i++)
        pthread_join(th[i], NULL);

    int overlap = atomic_load(&g_overlap);
    CHECK(overlap == 0);
    printf("    临界区重叠 %d 次\n", overlap);

    pager_close(pg);
    remove(PATH);
}

/* ---------------- 3) pin/unpin 配对 ---------------- */

static void t3_pin_unpin_pairing(void)
{
    remove(PATH);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, NFRAMES, &pg) == OK);
    if (!pg)
        return;

    u32 pid = 0;
    PageFrame *f = NULL;
    CHECK(pager_alloc(pg, &pid, &f) == OK);
    pager_unpin(pg, pid, true);

    /* 重新拿回来 */
    CHECK(pager_get(pg, pid, &f) == OK);
    CHECK(f->pin_count == 1);

    /* 同线程重复 get —— 靠内容锁是**递归**的才死不了 */
    PageFrame *f2 = NULL;
    CHECK(pager_get(pg, pid, &f2) == OK);
    CHECK(f2 == f);
    CHECK(f->pin_count == 2);

    CHECK(pager_unpin(pg, pid, false) == OK);
    CHECK(f->pin_count == 1);
    CHECK(pager_unpin(pg, pid, false) == OK);
    CHECK(f->pin_count == 0);

    /* 多退一次 → E_INVAL（不配对是调用方的 bug，要当场暴露） */
    CHECK(pager_unpin(pg, pid, false) == E_INVAL);

    printf("    pin_count 走 0→1→2→1→0，多退一次报 E_INVAL\n");

    pager_close(pg);
    remove(PATH);
}

/* ---------------- 4) WAL 钩子在 frame 域内 ---------------- */

static _Atomic int g_hook_calls;
static _Atomic int g_hook_in_frame;

static int wal_hook(void *ctx, u32 page_id, u32 page_lsn)
{
    (void)ctx;
    (void)page_id;
    (void)page_lsn;

    atomic_fetch_add(&g_hook_calls, 1);
    if (latch_order_held(LATCH_FRAME))
        atomic_fetch_add(&g_hook_in_frame, 1);
    return OK;
}

static void t4_wal_hook_inside_frame_domain(void)
{
    remove(PATH);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, NFRAMES, &pg) == OK);
    if (!pg)
        return;

    atomic_store(&g_hook_calls, 0);
    atomic_store(&g_hook_in_frame, 0);
    pager_set_wal_flush(pg, wal_hook, NULL);

    /* 造一个脏页，然后强制刷盘 */
    u32 pid = 0;
    PageFrame *f = NULL;
    CHECK(pager_alloc(pg, &pid, &f) == OK);
    pager_unpin(pg, pid, true);

    CHECK(pager_flush_all(pg) == OK);

    int calls = atomic_load(&g_hook_calls);
    int inside = atomic_load(&g_hook_in_frame);
    CHECK(calls > 0);
    CHECK(calls == inside);
    printf("    WAL 钩子被调 %d 次，其中 %d 次在 frame 域内\n", calls, inside);

    pager_close(pg);
    remove(PATH);
}

/* ---------------- 5) 逐出不串页 ---------------- */

/*
 * 池子只有 NFRAMES 帧，页有 NPAGES 页 → 必然发生逐出。
 * 每个线程写自己的页号进去，再读回来核对：
 * 读到的标记必须等于这一页的页号。
 *
 * 这一项守的是"内容锁 + pin_count 一起工作"：
 * 只有 pin_count 而没有内容锁时，
 * 线程 A 捏着页 X、B 把 X 逐出并载入页 Y，
 * A 写进 X 的内容其实落在了 Y 的帧里 → 标记对不上。
 */
typedef struct {
    Pager *pg;
    u32    base;
    u32    n;
} TagWorker;

static void *tag_worker(void *arg)
{
    TagWorker *w = (TagWorker *)arg;

    for (u32 k = 0; k < w->n; k++) {
        u32 pid = w->base + k;

        PageFrame *f = NULL;
        if (pager_get(w->pg, pid, &f) != OK)
            continue;
        put_u32(f->data + TAG_OFF, pid);
        pager_unpin(w->pg, pid, true);

        /* 再取回来核对 */
        if (pager_get(w->pg, pid, &f) != OK)
            continue;
        u32 tag = get_u32(f->data + TAG_OFF);
        if (tag != pid)
            atomic_fetch_add(&g_overlap, 1);   /* 借用 g_overlap 当"串页"计数 */
        pager_unpin(w->pg, pid, false);
    }
    return NULL;
}

static void t5_no_page_mixup(void)
{
    remove(PATH);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, NFRAMES, &pg) == OK);
    if (!pg)
        return;

    /* 先把页都建出来 */
    for (u32 i = 0; i < NPAGES; i++) {
        u32 pid = 0;
        PageFrame *f = NULL;
        if (pager_alloc(pg, &pid, &f) != OK)
            break;
        put_u32(f->data + TAG_OFF, pid);
        pager_unpin(pg, pid, true);
    }

    atomic_store(&g_overlap, 0);

    pthread_t th[NTHREAD];
    TagWorker w[NTHREAD];
    u32 per = NPAGES / NTHREAD;
    for (int i = 0; i < NTHREAD; i++) {
        w[i].pg = pg;
        w[i].base = (u32)(i * per);
        w[i].n = per;
        CHECK(pthread_create(&th[i], NULL, tag_worker, &w[i]) == 0);
    }
    for (int i = 0; i < NTHREAD; i++)
        pthread_join(th[i], NULL);

    int mixup = atomic_load(&g_overlap);
    CHECK(mixup == 0);
    printf("    %d 帧池 / %d 页并发读写：串页 %d 次\n", NFRAMES, NPAGES, mixup);

    pager_close(pg);
    remove(PATH);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("test_frame_domain（单元）:\n");

    printf("  1. 同一页并发读-改-写不丢更新\n");
    t1_no_lost_update();

    printf("  2. 内容锁真的互斥\n");
    t2_content_latch_exclusive();

    printf("  3. pin/unpin 配对（含同线程重复 get）\n");
    t3_pin_unpin_pairing();

    printf("  4. WAL 钩子在 frame 域内被调\n");
    t4_wal_hook_inside_frame_domain();

    printf("  5. 逐出不串页\n");
    t5_no_page_mixup();

    if (failures == 0) {
        printf("  5 项验收全部通过\n");
        return 0;
    }
    printf("  %d 项断言失败\n", failures);
    return 1;
}
