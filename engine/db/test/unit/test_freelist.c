/*
 * test_freelist.c — 页空间索引验收
 *
 * 核心要保证的性质：find 返回的页一定真的放得下（无假阳性）。
 * 反向不保证——档位是量化的（255 档 / 4076 字节 ≈ 16 字节粒度），
 * 可能漏掉"刚好够"的页，此时调用方新开一页即可。这个取舍写进了文档。
 */

#include <stdio.h>
#include <string.h>

#include "storage/freelist.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* 把前 n 页登记为「整页空闲」——新语义下必须显式登记 */
static void register_all(FreeList *fl, u32 n)
{
    for (u32 i = 0; i < n; i++)
        freelist_set(fl, i, FL_USABLE);
}

/* ---- 1) 创建：新页默认「不可用」，必须显式登记 ---- */
static void t1_create(void)
{
    FreeList *fl = NULL;
    CHECK(freelist_create(100, &fl) == OK);
    CHECK(fl != NULL);
    CHECK(freelist_npages(fl) == 100);

    /* 未登记：档位 0，find 不会返回 */
    CHECK(freelist_level(fl, 0) == 0);
    CHECK(freelist_count_at(fl, 0) == 100);
    CHECK(freelist_find(fl, 1) == PAGE_ID_NIL);

    register_all(fl, 100);
    CHECK(freelist_level(fl, 0) == FL_MAX_LEVEL);
    CHECK(freelist_level(fl, 99) == FL_MAX_LEVEL);
    CHECK(freelist_count_at(fl, (u8)FL_MAX_LEVEL) == 100);
    CHECK(freelist_count_at(fl, 0) == 0);
    CHECK(freelist_find(fl, 100) != PAGE_ID_NIL);
    freelist_destroy(fl);
}

/* ---- 2) best-fit：优先选刚好够的页，而不是随便一个空页 ---- */
static void t2_best_fit(void)
{
    FreeList *fl = NULL;
    CHECK(freelist_create(100, &fl) == OK);
    register_all(fl, 100);

    freelist_set(fl, 5, 100);      /* 剩 100 字节 */
    freelist_set(fl, 7, 2000);     /* 剩 2000 字节 */

    /* 要 50 字节：页 5（剩 100）比空页更合适 */
    CHECK(freelist_find(fl, 50) == 5);
    /* 要 1500 字节：只有页 7 和空页够，页 7 更合适 */
    CHECK(freelist_find(fl, 1500) == 7);
    /* 要 3000 字节：只有空页够 */
    {
        u32 p = freelist_find(fl, 3000);
        CHECK(p != PAGE_ID_NIL);
        CHECK(freelist_level(fl, p) == FL_MAX_LEVEL);
    }
    freelist_destroy(fl);
}

/* ---- 3) 无假阳性：find 说放得下，就真的放得下 ---- */
static void t3_no_false_positive(void)
{
    FreeList *fl = NULL;
    u32 tested = 0;

    CHECK(freelist_create(1, &fl) == OK);

    for (u32 free_b = 0; free_b <= FL_USABLE; free_b += 11) {
        freelist_set(fl, 0, (u16)free_b);
        for (u32 need = 1; need <= FL_USABLE; need += 37) {
            u32 p = freelist_find(fl, (u16)need);
            if (p == 0) {
                if (free_b < need) {
                    printf("    FAIL 假阳性：剩 %u 却声称能放 %u\n", free_b, need);
                    failures++;
                    freelist_destroy(fl);
                    return;
                }
                tested++;
            }
        }
    }
    CHECK(tested > 1000);          /* 确认确实覆盖了大量"能放下"的情形 */
    freelist_destroy(fl);
}

/* ---- 4) 全满：找不到任何页 ---- */
static void t4_all_full(void)
{
    FreeList *fl = NULL;
    CHECK(freelist_create(50, &fl) == OK);
    for (u32 i = 0; i < 50; i++)
        freelist_set_full(fl, i);

    CHECK(freelist_count_at(fl, 0) == 50);
    CHECK(freelist_count_at(fl, (u8)FL_MAX_LEVEL) == 0);
    CHECK(freelist_find(fl, 1) == PAGE_ID_NIL);
    freelist_destroy(fl);
}

/* ---- 5) 请求超过整页容量 ---- */
static void t5_oversize(void)
{
    FreeList *fl = NULL;
    CHECK(freelist_create(10, &fl) == OK);
    register_all(fl, 10);
    CHECK(freelist_find(fl, (u16)(FL_USABLE + 1)) == PAGE_ID_NIL);
    CHECK(freelist_find(fl, FL_USABLE) != PAGE_ID_NIL);
    freelist_destroy(fl);
}

/* ---- 6) 扩容：新增页默认不可用 ---- */
static void t6_grow(void)
{
    FreeList *fl = NULL;
    CHECK(freelist_create(10, &fl) == OK);
    register_all(fl, 10);
    CHECK(freelist_grow(fl, 30) == OK);
    CHECK(freelist_npages(fl) == 30);
    CHECK(freelist_level(fl, 29) == 0);                 /* 新增的 20 页未登记 */
    CHECK(freelist_count_at(fl, 0) == 20);
    CHECK(freelist_count_at(fl, (u8)FL_MAX_LEVEL) == 10);

    freelist_set(fl, 29, FL_USABLE);
    CHECK(freelist_level(fl, 29) == FL_MAX_LEVEL);

    CHECK(freelist_grow(fl, 5) == OK);          /* 缩小请求直接忽略 */
    CHECK(freelist_npages(fl) == 30);
    freelist_destroy(fl);
}

/* ---- 7) 档位单调：剩余空间越大档位越高 ---- */
static void t7_monotonic(void)
{
    FreeList *fl = NULL;
    CHECK(freelist_create(1, &fl) == OK);

    u16 prev_level = 0;
    for (u32 free_b = 0; free_b <= FL_USABLE; free_b += 7) {
        freelist_set(fl, 0, (u16)free_b);
        u16 lv = freelist_level(fl, 0);
        CHECK(lv >= prev_level);
        prev_level = lv;
    }
    /* 循环步长 7 除不尽 4076，最后停在 4074（档位 254）。
     * 这里显式测两个端点，验证映射的上下界。 */
    freelist_set(fl, 0, 0);
    CHECK(freelist_level(fl, 0) == 0);
    freelist_set(fl, 0, FL_USABLE);
    CHECK(freelist_level(fl, 0) == FL_MAX_LEVEL);
    freelist_destroy(fl);}

/* ---- 8) 桶计数与实际分布一致 ---- */
static void t8_bucket_consistency(void)
{
    FreeList *fl = NULL;
    u32 sum = 0;

    CHECK(freelist_create(200, &fl) == OK);
    for (u32 i = 0; i < 200; i++)
        freelist_set(fl, i, (u16)((i * 20u) % (FL_USABLE + 1)));

    for (u32 lv = 0; lv <= FL_MAX_LEVEL; lv++) {
        u32 actual = 0;
        for (u32 i = 0; i < 200; i++)
            if (freelist_level(fl, i) == (u8)lv)
                actual++;
        CHECK(freelist_count_at(fl, (u8)lv) == actual);
        sum += actual;
    }
    CHECK(sum == 200);
    freelist_destroy(fl);
}

int main(void)
{
    printf("test_freelist:\n");
    t1_create();
    t2_best_fit();
    t3_no_false_positive();
    t4_all_full();
    t5_oversize();
    t6_grow();
    t7_monotonic();
    t8_bucket_consistency();

    if (failures == 0) {
        printf("  8 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
