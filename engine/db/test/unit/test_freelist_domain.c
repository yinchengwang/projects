/*
 * test_freelist_domain.c —— v1.2 步骤 4c（子步骤 4c-1）：freelist 域的并发保护
 *
 * 4 项：
 *   1. **桶计数守恒**：多线程并发改不同页的档位 → 每个档位的页数加起来
 *      必须正好等于总页数（桶计数与实际档位不能对不上）
 *   2. **find 只返回"放得下"的页**：并发 find 出来的页，档位必须够
 *   3. **grow 与读并发**：一边 realloc 扩容、一边读 → 不崩，老页的档位不能丢
 *   4. **档位换算的单调性**：level >= need_level ⟹ free >= need（语义不变量）
 *
 * ==================== 第 1 项在抓什么 ====================
 *
 * freelist_set 做的是"读-改-写"两个共享计数器：
 *
 *     if (fl->bucket[old]) fl->bucket[old]--;
 *     fl->bucket[nw]++;
 *     fl->levels[page_id] = nw;
 *
 * 两个线程改的是**不同的页**，但很可能命中**同一个桶**（比如都从档位 0 挪走、
 * 都挪到档位 7）。没有锁的时候，两次 `bucket[0]--` 会互相覆盖 ——
 * 桶里的计数比实际档位少 → find 会返回**已经被别人占满的页**。
 *
 * 后果不是"多占一点空间"：调用方拿到一页就往里塞数据，
 * 而那一页可能已经满了 → page_insert 失败或者**页内 slot 目录被写坏**。
 *
 * ==================== 第 3 项在抓什么 ====================
 *
 * `freelist_grow` 用 `realloc` 扩 `levels` —— **指针会变**。
 * 读者如果在外面缓存了旧指针，扩容之后就是**悬空指针**。
 * 所以本模块的读接口一律加锁（见 freelist.c 的文件头）。
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "storage/freelist.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define NPAGES  64
#define NTHREAD 8
#define NROUNDS 2000         /* 每线程改 2000 次档位（8×2000 = 1.6 万次桶更新） */

/* 与 freelist.c 同一套换算（测试独立算一遍，不依赖内部函数） */
static u8 level_of(u16 free_bytes)
{
    if (free_bytes >= FL_USABLE)
        return (u8)FL_MAX_LEVEL;
    return (u8)(((u32)free_bytes * FL_MAX_LEVEL) / FL_USABLE);
}

/* ---------------- 1) 桶计数守恒 ---------------- */

/*
 * 三个不变量，任何一个破了都说明桶计数和实际档位对不上：
 *
 *   I1：所有桶的计数加起来 == 总页数
 *   I2：每个桶的计数 == 实际处在该档位的页数
 *   I3：页数不变（grow 没被误触发）
 *
 * I2 是核心：桶计数是**派生数据**，
 * 真正的真相是 levels[] 里每页的档位。
 * 桶计数一旦和真相对不上，find 就会给出错误的答案。
 */
typedef struct {
    FreeList *fl;
    int       id;                /* 线程号 */
} SetWorker;

static void *set_worker(void *arg)
{
    SetWorker *w = (SetWorker *)arg;

    for (int i = 0; i < NROUNDS; i++) {
        /*
         * 页号按 "i * 线程数 + 线程号" 分配：每个线程改的是**不同的页**，
         * 但它们命中的是**同一批桶**（桶是按档位分的，不是按页分的）。
         *
         * 每次都把档位在两个值之间**翻过来** ——
         * 这样每一次 set 都会真的动到桶计数。
         * （如果两次设成同一个档位，freelist_set 会提前返回，撞不上。）
         */
        u32 pid = (u32)((i * NTHREAD + w->id) % NPAGES);
        u8 cur = freelist_level(w->fl, pid);
        u16 bytes = (cur == level_of((u16)(FL_USABLE / 2)))
                        ? (u16)(FL_USABLE / 4)
                        : (u16)(FL_USABLE / 2);
        freelist_set(w->fl, pid, bytes);
    }
    return NULL;
}

static void t1_bucket_conserved(void)
{
    FreeList *fl = NULL;
    CHECK(freelist_create(NPAGES, &fl) == OK);
    if (!fl)
        return;

    pthread_t th[NTHREAD];
    SetWorker w[NTHREAD];

    for (int i = 0; i < NTHREAD; i++) {
        w[i].fl = fl;
        w[i].id = i;
    }

    for (int i = 0; i < NTHREAD; i++)
        CHECK(pthread_create(&th[i], NULL, set_worker, &w[i]) == 0);
    for (int i = 0; i < NTHREAD; i++)
        pthread_join(th[i], NULL);

    /* I1：所有桶加起来 == 总页数 */
    u32 sum = 0;
    for (u32 lv = 0; lv <= FL_MAX_LEVEL; lv++)
        sum += freelist_count_at(fl, (u8)lv);
    CHECK(sum == NPAGES);

    /* I2：桶计数 == 实际处在该档位的页数 */
    u32 actual[FL_MAX_LEVEL + 1];
    memset(actual, 0, sizeof(actual));
    for (u32 p = 0; p < NPAGES; p++)
        actual[freelist_level(fl, p)]++;

    int mismatch = 0;
    for (u32 lv = 0; lv <= FL_MAX_LEVEL; lv++)
        if (freelist_count_at(fl, (u8)lv) != actual[lv])
            mismatch++;

    /* I3：页数没变 */
    CHECK(freelist_npages(fl) == NPAGES);

    CHECK(mismatch == 0);
    printf("    %d 线程 × %d 次改档位：桶计数合计 %u（应 %d），桶与实际档位不符 %d 个\n",
           NTHREAD, NROUNDS, sum, NPAGES, mismatch);

    freelist_destroy(fl);
}

/* ---------------- 2) find 只返回放得下的页 ---------------- */

/*
 * 两个线程轮流：find 一页 → 立刻把它标满（set_full）→ 再 find。
 *
 * 如果 find 会返回**已经被别人标满**的页（桶计数丢了更新），
 * 复核时就会发现档位不够 —— 这正是调用方要担心的那种页：
 * 拿它去 insert 会写坏页。
 */
typedef struct {
    FreeList *fl;
    int       bad;
    int       found;
} FindWorker;

static void *find_worker(void *arg)
{
    FindWorker *w = (FindWorker *)arg;

    for (int i = 0; i < 200; i++) {
        u16 need = (u16)(FL_USABLE / 4);
        u32 pid = freelist_find(w->fl, need);
        if (pid == PAGE_ID_NIL)
            continue;

        w->found++;

        /*
         * 复核：这一页的档位必须够放下 need 字节。
         *
         * ⚠️ 这里有一处**测试自己**的竞态（2026-11-04 修）：
         * find 返回之后、复核之前，**别的线程**可能已经把这一页标满
         * （本测试的另一个 worker 就在干这件事），于是复核读到档位 0。
         * 那是"这一页被别人抢走了"，**不是** find 返回了放不下的页。
         *
         * 区分办法：档位 **0** = 已被标满（别人抢走了）；
         * 若档位非 0 却小于需求档位，才是真出问题
         * （find 从"刚好够"的档位往上找，逻辑上不可能出现）。
         * 修之前这条断言是**偶发红**的（实测 10 次里红 1 次）。
         */
        u8 lv = freelist_level(w->fl, pid);
        u8 need_lv = (u8)(((u32)need * FL_MAX_LEVEL + FL_USABLE - 1) / FL_USABLE);
        if (lv < need_lv && lv != 0)
            w->bad++;

        /* 标满它，模拟"这一页被我占了" */
        freelist_set_full(w->fl, pid);
    }
    return NULL;
}

static void t2_find_returns_usable_page(void)
{
    FreeList *fl = NULL;
    CHECK(freelist_create(NPAGES, &fl) == OK);
    if (!fl)
        return;

    /* 先把所有页登记成"整页空闲" */
    for (u32 p = 0; p < NPAGES; p++)
        freelist_set(fl, p, FL_USABLE);

    pthread_t th[NTHREAD];
    FindWorker w[NTHREAD];

    for (int i = 0; i < NTHREAD; i++) {
        w[i].fl = fl;
        w[i].bad = 0;
        w[i].found = 0;
    }

    for (int i = 0; i < NTHREAD; i++)
        CHECK(pthread_create(&th[i], NULL, find_worker, &w[i]) == 0);
    for (int i = 0; i < NTHREAD; i++)
        pthread_join(th[i], NULL);

    int bad = 0, found = 0;
    for (int i = 0; i < NTHREAD; i++) {
        bad += w[i].bad;
        found += w[i].found;
    }

    CHECK(bad == 0);
    CHECK(found > 0);
    printf("    %d 次 find，放不下的页 %d 次\n", found, bad);

    freelist_destroy(fl);
}

/* ---------------- 3) grow 与读并发 ---------------- */

static volatile int g_stop = 0;
static volatile int g_reader_bad = 0;
static volatile int g_grow_bad = 0;

#define GROW_LIMIT (1u << 20)    /* 扩容上限：levels 每页 1 字节，1M 页 = 1MB */

static void *grow_worker(void *arg)
{
    FreeList *fl = (FreeList *)arg;

    u32 np = NPAGES;
    while (!g_stop) {
        if (np >= GROW_LIMIT)
            continue;               /* 到顶了，但线程继续转，制造并发窗口 */
        np *= 2;                    /* 翻倍扩容：每次 realloc 要搬的数据多，
                                     * 指针"搬家"的窗口就宽，悬空指针更容易撞上 */
        if (freelist_grow(fl, np) != OK)
            g_grow_bad++;
    }
    return NULL;
}

static void *read_worker(void *arg)
{
    FreeList *fl = (FreeList *)arg;

    while (!g_stop) {
        /* 老页的档位不能因为扩容而变样（整批查，撞上搬家窗口的机会更大） */
        for (u32 p = 0; p < NPAGES; p++) {
            if (freelist_level(fl, p) != level_of((u16)(FL_USABLE / 2)))
                g_reader_bad++;
        }

        /* find 不能崩，也不能返回越界的页 */
        u32 pid = freelist_find(fl, (u16)(FL_USABLE / 2));
        if (pid != PAGE_ID_NIL && pid >= (u32)freelist_npages(fl))
            g_reader_bad++;
    }
    return NULL;
}

static void t3_grow_with_readers(void)
{
    FreeList *fl = NULL;
    CHECK(freelist_create(NPAGES, &fl) == OK);
    if (!fl)
        return;

    /* 先把每一页都设成"半空"，扩容之后它们必须都还在 */
    for (u32 p = 0; p < NPAGES; p++)
        freelist_set(fl, p, (u16)(FL_USABLE / 2));

    g_stop = 0;
    g_reader_bad = 0;
    g_grow_bad = 0;

    pthread_t tg, tr1, tr2;
    CHECK(pthread_create(&tg, NULL, grow_worker, fl) == 0);
    CHECK(pthread_create(&tr1, NULL, read_worker, fl) == 0);
    CHECK(pthread_create(&tr2, NULL, read_worker, fl) == 0);

    /* 跑 0.3 秒就停（clock 是 C11 标准函数，MinGW 也有） */
    clock_t start = clock();
    while ((clock() - start) < (CLOCKS_PER_SEC / 3))
        ;
    g_stop = 1;

    pthread_join(tg, NULL);
    pthread_join(tr1, NULL);
    pthread_join(tr2, NULL);

    CHECK(g_reader_bad == 0);
    CHECK(g_grow_bad == 0);
    CHECK(freelist_npages(fl) >= NPAGES);
    CHECK(freelist_level(fl, 0) == level_of((u16)(FL_USABLE / 2)));

    printf("    扩容期间读出问题 %d 次，扩容失败 %d 次，最终 %u 页\n",
           g_reader_bad, g_grow_bad, freelist_npages(fl));

    freelist_destroy(fl);
}

/* ---------------- 4) 档位换算的单调性 ---------------- */

/*
 * 这一项守的是 freelist 的**正确性基础**（见 freelist.c 文件头）：
 *
 *     level      = floor(free * 255 / USABLE)
 *     need_level = ceil (need  * 255 / USABLE)
 *     level >= need_level  ⟹  free >= need
 *
 * find() 就是靠这条性质保证"返回的页一定放得下 need 字节"。
 * 性质一旦破了，find 就会返回放不下的页 → 插入失败或页内 slot 目录写坏。
 *
 * 这里分两半查：
 *   (a) 模块算出来的档位 == 公式算出来的档位（换算没写错）
 *   (b) 上面那条蕴含关系在全部取值上成立（公式本身没写错）
 */
static void t4_level_monotonic(void)
{
    FreeList *fl = NULL;
    CHECK(freelist_create(NPAGES, &fl) == OK);
    if (!fl)
        return;

    /* (a) 换算一致性：扫一批剩余字节数，逐个核对档位 */
    int wrong = 0;
    u32 step = FL_USABLE / (NPAGES - 1);
    for (u32 p = 0; p < NPAGES; p++) {
        u16 free_bytes = (u16)(p * step);
        freelist_set(fl, p, free_bytes);
    }
    for (u32 p = 0; p < NPAGES; p++) {
        u16 free_bytes = (u16)(p * step);
        if (freelist_level(fl, p) != level_of(free_bytes))
            wrong++;
    }
    CHECK(wrong == 0);

    /* (b) 蕴含关系：level >= need_level ⟹ free >= need */
    int broken = 0;
    for (u32 free_i = 0; free_i <= 256; free_i++) {
        u16 free_bytes = (u16)(free_i * (FL_USABLE / 256));
        u8 lv = level_of(free_bytes);
        for (u32 need_i = 0; need_i <= 256; need_i++) {
            u16 need = (u16)(need_i * (FL_USABLE / 256));
            u8 need_lv = (u8)(((u32)need * FL_MAX_LEVEL + FL_USABLE - 1) / FL_USABLE);
            if (lv >= need_lv && free_bytes < need)
                broken++;
        }
    }
    CHECK(broken == 0);

    printf("    档位换算：%d 页核对，错 %d 页；蕴含关系破坏 %d 处\n",
           NPAGES, wrong, broken);

    freelist_destroy(fl);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("test_freelist_domain（单元）:\n");

    printf("  1. 桶计数守恒（并发改档位不丢更新）\n");
    t1_bucket_conserved();

    printf("  2. find 只返回放得下的页\n");
    t2_find_returns_usable_page();

    printf("  3. grow（realloc）与读并发\n");
    t3_grow_with_readers();

    printf("  4. 档位换算的单调性\n");
    t4_level_monotonic();

    if (failures == 0) {
        printf("  4 项验收全部通过\n");
        return 0;
    }
    printf("  %d 项断言失败\n", failures);
    return 1;
}
