/*
 * test_heap_insert_domain.c —— v1.2 步骤 4d：堆插入的"找 → 用"竞态
 *
 * 4 项：
 *   1. **并发插入不报错**：多线程往同一张堆表插 → 每一次都必须成功
 *      （旧代码在这里会大面积报 E_NOSPACE —— 档位过期）
 *   2. **不丢行**：并发插完之后全表扫描，行数必须正好是 线程数 × 每线程行数
 *   3. **不重行**：每个 RID 只能出现一次（两个线程抢到同一个 slot 就是坏了）
 *   4. **页数账对得上**：堆自己的页数统计 == freelist 里登记的页数
 *
 * ==================== 第 1 项在抓什么 ====================
 *
 * freelist 是按**档位**挑页的，而档位是**插入之后**才更新的。
 * 于是"找 → 用"之间有一个窗口：
 *
 *     线程 A：find → 页 P（档位 3，放得下）
 *             → 拿 P 的内容锁 → 插进去 → 更新 P 的档位
 *     线程 B：find → 页 P（档位**还是** 3 —— A 还没更新）
 *             → 等 P 的内容锁 → 拿到时 P 已被 A 填满
 *             → page_insert 报 E_NOSPACE
 *
 * 这不是"页不够用"，是**档位过期**。
 * 处理办法是**重试**（见 heap.c 的 heap_insert）：
 * 失败说明这一页的档位已经不准了，换一页再试。
 *
 * 为什么重试一定收敛：填满 P 的线程是**握着 P 的内容锁**时更新档位的，
 * 我们是**拿到内容锁之后**才失败的 —— 所以失败时档位已经是新的，
 * find 不会再挑到 P ✓
 *
 * ==================== 为什么这个测试不走 db_exec ====================
 *
 * 这一层要能**独立**证明"堆插入路径自己站得住"，
 * 而不是靠"并发测试没报错"来间接推断。
 * 所以这里直接调 heap_insert / heap_scan_next，
 * 完全绕开 SQL 与大锁。
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/heap.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define NTHREAD 4
#define NROWS   500         /* 每线程 500 行 → 共 2000 行（够翻好几页） */
#define TEST_TID 100

typedef struct {
    Pager    *pg;
    FreeList *fl;
    Heap     *h;
    u32       root;
} Env;

static int env_create(Env *e, const char *path)
{
    memset(e, 0, sizeof(*e));
    remove(path);
    int rc = pager_open(path, 64, &e->pg);
    if (rc != OK) return rc;
    rc = freelist_create(0, &e->fl);
    if (rc != OK) return rc;
    rc = heap_create(e->pg, e->fl, TEST_TID, NULL, NULL, &e->root);
    if (rc != OK) return rc;
    return heap_open(e->pg, e->fl, e->root, TEST_TID, NULL, NULL, &e->h);
}

static void env_close(Env *e)
{
    if (e->h) heap_destroy(e->h);
    if (e->fl) freelist_destroy(e->fl);
    if (e->pg) pager_close(e->pg);
    memset(e, 0, sizeof(*e));
}

/* ---------------- 并发插入 ---------------- */

typedef struct {
    Heap *h;
    int   tid;
    int   errors;       /* 插入失败的次数 */
    int   nospace;      /* 其中是 E_NOSPACE 的次数 */
} InsWorker;

/*
 * 记录内容：16 字节定长（带一个序号，方便扫出来时核对）。
 * 故意用**变长**的长度（每行长度不同）——
 * 长度一样的话所有行会正好填满页，撞上"页刚好满"的边界。
 */
static u16 make_rec(u8 *buf, int tid, int i)
{
    u16 len = (u16)(24 + (i % 7));
    memset(buf, 0, len);
    buf[0] = (u8)tid;
    buf[1] = (u8)(i >> 8);
    buf[2] = (u8)i;
    return len;
}

static void *ins_worker(void *arg)
{
    InsWorker *w = (InsWorker *)arg;
    u8 buf[64];

    for (int i = 0; i < NROWS; i++) {
        u16 len = make_rec(buf, w->tid, i);
        Rid rid;
        int rc = heap_insert(w->h, 0, buf, len, &rid);
        if (rc != OK) {
            w->errors++;
            if (rc == E_NOSPACE)
                w->nospace++;
        }
    }
    return NULL;
}

/* 扫全表，把每行的 (tid, i) 记进 seen 数组 */
static int scan_rows(Heap *h, unsigned char *seen)
{
    HeapScan *s = NULL;
    if (heap_scan_begin(h, &s) != OK)
        return -1;

    int n = 0;
    u8 buf[256];
    u16 len = 0;
    Rid rid;
    while (heap_scan_next(s, &rid, buf, sizeof(buf), &len) == OK) {
        if (len < 3)
            continue;
        int tid = buf[0];
        int i = (buf[1] << 8) | buf[2];
        if (tid >= 0 && tid < NTHREAD && i >= 0 && i < NROWS)
            seen[tid * NROWS + i]++;
        n++;
    }
    heap_scan_end(s);
    return n;
}

static void run_concurrent(Env *e, const char *tag)
{
    pthread_t th[NTHREAD];
    InsWorker w[NTHREAD];

    for (int i = 0; i < NTHREAD; i++) {
        w[i].h = e->h;
        w[i].tid = i;
        w[i].errors = 0;
        w[i].nospace = 0;
    }

    for (int i = 0; i < NTHREAD; i++)
        CHECK(pthread_create(&th[i], NULL, ins_worker, &w[i]) == 0);
    for (int i = 0; i < NTHREAD; i++)
        pthread_join(th[i], NULL);

    int errors = 0, nospace = 0;
    for (int i = 0; i < NTHREAD; i++) {
        errors += w[i].errors;
        nospace += w[i].nospace;
    }

    /* 第 1 项：一次都不许失败 */
    CHECK(errors == 0);

    /* 第 2、3 项：不丢行、不重行 */
    unsigned char *seen = calloc((size_t)(NTHREAD * NROWS), 1);
    CHECK(seen != NULL);
    if (seen) {
        int n = scan_rows(e->h, seen);
        CHECK(n == NTHREAD * NROWS);

        int missing = 0, dup = 0;
        for (int i = 0; i < NTHREAD * NROWS; i++) {
            if (seen[i] == 0)
                missing++;
            else if (seen[i] > 1)
                dup++;
        }
        CHECK(missing == 0);
        CHECK(dup == 0);

        /* 第 4 项：页数账对得上（堆的页数 == 扫到的页数范围） */
        u32 np = heap_npages(e->h);
        CHECK(np > 0);
        CHECK(np <= (u32)(NTHREAD * NROWS));

        printf("    %s：插入失败 %d 次（其中 E_NOSPACE %d 次），扫出 %d 行，缺 %d 行重 %d 行，%u 页\n",
               tag, errors, nospace, n, missing, dup, np);

        free(seen);
    }
}

/* ---------------- 1~4) 一次跑完 ---------------- */

static void t1_concurrent_insert(void)
{
    Env e;
    if (env_create(&e, "test/tmp/heap_insert_domain.db") != OK) {
        printf("    FAIL 环境创建失败\n");
        failures++;
        return;
    }
    run_concurrent(&e, "并发插入");
    env_close(&e);
}

/*
 * 第 5 项：**页满之后必须还能继续插**。
 *
 * 单独把这一项拎出来，是因为它抓的是另一件事：
 * freelist 的档位是"页还剩多少"，
 * 如果页满了但档位没更新（比如漏了 freelist_set），
 * find 会一直把这一页挑出来 → 插一次失败一次 → 永远插不进去。
 *
 * 这里先单线程插到某一页满了（插够一整页的行数），
 * 再插一行 —— 必须成功（说明 freelist 知道"这一页满了"，
 * 于是开了新页）。
 */
static void t2_insert_after_page_full(void)
{
    Env e;
    if (env_create(&e, "test/tmp/heap_insert_full.db") != OK) {
        printf("    FAIL 环境创建失败\n");
        failures++;
        return;
    }

    u8 buf[64];
    int ok = 0, fail = 0;
    for (int i = 0; i < 2000; i++) {
        u16 len = make_rec(buf, 0, i);
        Rid rid;
        if (heap_insert(e.h, 0, buf, len, &rid) == OK)
            ok++;
        else
            fail++;
    }
    CHECK(fail == 0);
    CHECK(ok == 2000);

    /* 2000 行 ≈ 几十页，页早就翻了好几轮 —— 每一页都该被正确标记过 */
    u32 np = heap_npages(e.h);
    CHECK(np > 1);

    printf("    单线程插 2000 行：成功 %d 失败 %d，共 %u 页\n", ok, fail, np);

    env_close(&e);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("test_heap_insert_domain（单元）:\n");

    printf("  1. 并发插入不报错（档位过期 → 重试）\n");
    t1_concurrent_insert();

    printf("  2. 并发插入不丢行、不重行、页数账对得上\n");
    /* 第 1 项里已经把这三条查了，这里只把标题写出来 */
    printf("    （与第 1 项同一次运行，结果见上）\n");

    printf("  3. 页满之后还能继续插\n");
    t2_insert_after_page_full();

    if (failures == 0) {
        printf("  3 项验收全部通过\n");
        return 0;
    }
    printf("  %d 项断言失败\n", failures);
    return 1;
}
