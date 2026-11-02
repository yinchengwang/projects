/*
 * test_pager.c — pager 的验收测试
 *
 * 覆盖：1 万页写入/读回（强制逐出）、随机访问、改写后重开、
 *       pin 满报错不死锁、unpin 配对检查。
 *
 * 每页内容 = 页号 + 轮次 + 伪随机填充 + crc16，
 * 由 (page_id, round) 完全确定，失败可精确复现。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/crc.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* xorshift64，种子固定：失败可复现 */
static u64 rng_state = 0x123456789ABCDEF0ull;

static u64 rng_next(void)
{
    u64 x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

/* 页内容只由 (page_id, round) 决定 */
static void fill_page(u8 *data, u32 page_id, u32 round)
{
    u64 x = ((u64)page_id << 32) ^ (u64)round ^ 0x9E3779B97F4A7C15ull;
    u8 *p = data + 16;
    for (u32 i = 0; i < PAGE_SIZE - 16; i += 8) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        put_u64(p + i, x);
    }
    put_u32(data + 0, page_id);
    put_u32(data + 4, round);
    put_u16(data + 8, 0xBEEF);
    put_u16(data + 10, crc16(data + 16, PAGE_SIZE - 16));
    put_u32(data + 12, 0xC0DEC0DEu);   /* 12..15 必须显式写：
                                           否则 memcmp 比到的是未初始化的栈字节 */
}

static int verify_page(const u8 *data, u32 page_id, u32 round)
{
    u8 tmp[PAGE_SIZE];
    fill_page(tmp, page_id, round);
    return memcmp(data, tmp, PAGE_SIZE) == 0;
}

#define NPAGES  10000u
#define NFRAMES 16u          /* 故意很小，逼出逐出路径 */
#define PATH    "test/tmp/pager_test.db"

int main(void)
{
    Pager *pg = NULL;
    PageFrame *f = NULL;
    int rc;
    u32 id, i;

    remove(PATH);   /* 上一轮的残留会让 npages 对不上 */

    /* ---- 1) 分配 1 万页并写入模式（池 64 帧） ---- */
    rc = pager_open(PATH, 64, &pg);
    CHECK(rc == OK);
    CHECK(pg != NULL);

    for (id = 0; id < NPAGES; id++) {
        u32 got;
        rc = pager_alloc(pg, &got, &f);
        CHECK(rc == OK);
        CHECK(got == id);
        fill_page(f->data, id, 1);
        rc = pager_unpin(pg, id, true);
        CHECK(rc == OK);
        if (failures)
            goto done;
    }
    CHECK(pager_npages(pg) == NPAGES);
    rc = pager_close(pg);
    CHECK(rc == OK);

    /* ---- 2) 重开（16 帧），顺序读回：不断逐出 ---- */
    rc = pager_open(PATH, NFRAMES, &pg);
    CHECK(rc == OK);
    CHECK(pager_npages(pg) == NPAGES);

    for (id = 0; id < NPAGES; id++) {
        rc = pager_get(pg, id, &f);
        CHECK(rc == OK);
        if (rc != OK)
            break;
        if (!verify_page(f->data, id, 1)) {
            printf("    FAIL 顺序读回：页 %u 内容不符\n", id);
            failures++;
            pager_unpin(pg, id, false);
            break;
        }
        rc = pager_unpin(pg, id, false);
        CHECK(rc == OK);
    }

    /* ---- 3) 随机访问 2 万次 ---- */
    for (i = 0; i < 20000; i++) {
        id = (u32)(rng_next() % NPAGES);
        rc = pager_get(pg, id, &f);
        CHECK(rc == OK);
        if (rc != OK)
            break;
        if (!verify_page(f->data, id, 1)) {
            printf("    FAIL 随机读：页 %u 内容不符\n", id);
            failures++;
            pager_unpin(pg, id, false);
            break;
        }
        pager_unpin(pg, id, false);
    }

    /* ---- 4) 改写偶数页（round=2），重开验证新旧混合 ---- */
    for (id = 0; id < NPAGES; id += 2) {
        rc = pager_get(pg, id, &f);
        CHECK(rc == OK);
        fill_page(f->data, id, 2);
        rc = pager_unpin(pg, id, true);
        CHECK(rc == OK);
    }
    pager_close(pg);

    rc = pager_open(PATH, NFRAMES, &pg);
    CHECK(rc == OK);
    for (id = 0; id < NPAGES; id++) {
        u32 expect_round = (id % 2 == 0) ? 2u : 1u;
        rc = pager_get(pg, id, &f);
        CHECK(rc == OK);
        if (rc != OK)
            break;
        if (!verify_page(f->data, id, expect_round)) {
            printf("    FAIL 混合读回：页 %u（期望 round %u）不符\n",
                   id, expect_round);
            failures++;
            pager_unpin(pg, id, false);
            break;
        }
        pager_unpin(pg, id, false);
    }

    /* ---- 5) 池子 pin 满：必须报 E_NOSPACE，绝不死循环 ---- */
    {
        PageFrame *hold[NFRAMES];
        for (i = 0; i < NFRAMES; i++) {
            rc = pager_get(pg, i, &hold[i]);   /* 页 0..15，16 个不同页 */
            CHECK(rc == OK);
            if (rc != OK)
                break;
        }
        if (rc == OK) {
            rc = pager_get(pg, 999, &f);
            CHECK(rc == E_NOSPACE);

            /* 释放两页，又能继续取 */
            pager_unpin(pg, 0, false);
            pager_unpin(pg, 1, false);
            rc = pager_get(pg, 999, &f);
            CHECK(rc == OK);
            pager_unpin(pg, 999, false);

            for (i = 2; i < NFRAMES; i++)
                pager_unpin(pg, i, false);
        }
    }

    /* ---- 6) unpin 不配对必须报错 ---- */
    rc = pager_get(pg, 7, &f);
    CHECK(rc == OK);
    rc = pager_unpin(pg, 7, false);
    CHECK(rc == OK);
    rc = pager_unpin(pg, 7, false);   /* 多退一次 */
    CHECK(rc == E_INVAL);

    /* 越界页 */
    rc = pager_get(pg, NPAGES + 5, &f);
    CHECK(rc == E_NOTFOUND);

done:
    if (pg)
        pager_close(pg);

    if (failures == 0) {
        printf("test_pager: %u 页写入/逐出读回/随机访问/改写混合/"
               "pin 满报错/unpin 配对 全部通过", NPAGES);
        return 0;
    }
    printf("test_pager: %d 项失败", failures);
    return 1;
}
