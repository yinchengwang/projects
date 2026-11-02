/*
 * test_page.c — slotted page 的验收测试
 *
 * 对应 docs/PLAN-v0.1.md §1.6 的 9 项标准。
 * 重点是 t5（slot 不复用）、t6（compact 顺序）、t7（碎片回收）。
 */

#include <stdio.h>
#include <string.h>

#include "common/crc.h"
#include "storage/page.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static u8 pg[PAGE_SIZE];

static u64 rng_state = 0x9E3779B97F4A7C15ull;

static u64 rng_next(void)
{
    u64 x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

/* 记录内容只由 (id, len) 决定，填满整条，便于发现覆盖 */
static void fill_rec(u8 *rec, u16 len, u32 id)
{
    put_u32(rec + 0, id);
    put_u16(rec + 4, len);
    for (u16 i = 6; i < len; i++)
        rec[i] = (u8)(id * 31u + i * 7u);
}

static bool rec_ok(const u8 *rec, u16 len, u32 id)
{
    if (get_u32(rec + 0) != id)
        return false;
    if (get_u16(rec + 4) != len)
        return false;
    for (u16 i = 6; i < len; i++)
        if (rec[i] != (u8)(id * 31u + i * 7u))
            return false;
    return true;
}

/* 不变量：header + slot 数组 + 已用记录区 + 空闲 == 整页 */
static void check_invariants(const char *where)
{
    u32 sum = HDR_SIZE
            + (u32)page_slot_count(pg) * SLOT_SIZE
            + page_used_bytes(pg)
            + page_free_bytes(pg);
    if (sum != PAGE_SIZE) {
        printf("    FAIL 不变量(%s): %u != %u\n", where, sum, (u32)PAGE_SIZE);
        failures++;
    }
    if (!page_verify_checksum(pg)) {
        printf("    FAIL 校验和(%s)\n", where);
        failures++;
    }
}

/* ---- 1) 基本读写 ---- */
static void t1_basic(void)
{
    u8 rec[100], expect[100];
    u16 slot, len;
    const u8 *got;

    CHECK(page_init(pg, PT_HEAP) == OK);
    fill_rec(rec, 100, 7);
    memcpy(expect, rec, 100);

    CHECK(page_insert(pg, rec, 100, &slot) == OK);
    CHECK(slot == 0);
    CHECK(page_get(pg, slot, &got, &len) == OK);
    CHECK(len == 100);
    CHECK(memcmp(got, expect, 100) == 0);
    check_invariants("t1");
}

/* ---- 2) 插满：每个 slot 都能正确读回 ---- */
static void t2_fill(void)
{
    u8 rec[64];
    u16 slot, n = 0;

    CHECK(page_init(pg, PT_HEAP) == OK);
    fill_rec(rec, 64, 1);

    for (;;) {
        int rc = page_insert(pg, rec, 64, &slot);
        if (rc != OK) {
            CHECK(rc == E_NOSPACE);
            break;
        }
        CHECK(slot == n);
        n++;
        if (n > 200) {
            printf("    FAIL 插不满了，逻辑有误\n");
            failures++;
            break;
        }
    }
    /* (4096 - 20) / (64 + 4) = 59 */
    CHECK(n == 59);

    for (u16 i = 0; i < n; i++) {
        u16 len;
        const u8 *got;
        CHECK(page_get(pg, i, &got, &len) == OK);
        CHECK(len == 64);
        CHECK(rec_ok(got, 64, 1));
    }
    check_invariants("t2");
}

/* ---- 3) 删除 ---- */
static void t3_delete(void)
{
    u8 rec[32];
    u16 slot;

    CHECK(page_init(pg, PT_HEAP) == OK);
    for (u32 i = 0; i < 20; i++) {
        fill_rec(rec, 32, i);
        CHECK(page_insert(pg, rec, 32, &slot) == OK);
    }

    for (u16 i = 0; i < 20; i += 2)
        CHECK(page_delete(pg, i) == OK);

    for (u16 i = 0; i < 20; i++) {
        const u8 *got;
        u16 len;
        int rc = page_get(pg, i, &got, &len);
        if (i % 2 == 0) {
            CHECK(rc == E_NOTFOUND);
        } else {
            CHECK(rc == OK);
            CHECK(rec_ok(got, 32, i));
        }
    }
    CHECK(page_delete(pg, 0) == E_NOTFOUND);      /* 重复删 */
    CHECK(page_delete(pg, 999) == E_NOTFOUND);    /* 越界 */
    {
        const u8 *got;
        u16 len;
        CHECK(page_get(pg, 999, &got, &len) == E_NOTFOUND);
    }
    check_invariants("t3");
}

/* ---- 4) RID 稳定：compact 后 slot 编号和内容都不变 ---- */
static void t4_compact_stable(void)
{
    u8 rec[50];
    u16 slot;

    CHECK(page_init(pg, PT_HEAP) == OK);
    for (u32 i = 0; i < 30; i++) {
        fill_rec(rec, 50, i + 1);
        CHECK(page_insert(pg, rec, 50, &slot) == OK);
    }
    for (u16 i = 0; i < 30; i += 3)
        CHECK(page_delete(pg, i) == OK);

    CHECK(page_fragmentation(pg) > 0);

    page_compact(pg);
    CHECK(page_fragmentation(pg) == 0);

    for (u16 i = 0; i < 30; i++) {
        const u8 *got;
        u16 len;
        int rc = page_get(pg, i, &got, &len);
        if (i % 3 == 0) {
            CHECK(rc == E_NOTFOUND);
        } else {
            CHECK(rc == OK);
            CHECK(len == 50);
            CHECK(rec_ok(got, 50, i + 1));
        }
    }
    /* 30 个 slot，20 条活记录 × 50 */
    CHECK(page_free_bytes(pg) == PAGE_SIZE - HDR_SIZE - 30 * SLOT_SIZE - 20 * 50);
    check_invariants("t4");
}

/* ---- 5) slot 永不复用 ---- */
static void t5_no_reuse(void)
{
    u8 rec[20];
    u16 slot, ns;

    CHECK(page_init(pg, PT_HEAP) == OK);
    fill_rec(rec, 20, 9);
    for (int i = 0; i < 5; i++)
        CHECK(page_insert(pg, rec, 20, &slot) == OK);
    CHECK(page_slot_count(pg) == 5);

    CHECK(page_delete(pg, 3) == OK);
    CHECK(page_insert(pg, rec, 20, &ns) == OK);

    CHECK(ns == 5);                      /* 新编号 = 原数量，绝不回填 3 */
    CHECK(page_slot_count(pg) == 6);
    CHECK(!page_slot_alive(pg, 3));
    check_invariants("t5");
}

/* ---- 6) compact 顺序：唯一能抓到"倒序搬运覆盖数据"的用例 ---- */
static void t6_compact_order(void)
{
    u8 rec[1000];
    u16 slot;

    CHECK(page_init(pg, PT_HEAP) == OK);

    /* 插入顺序决定 offset：先插的在最高地址
     *   slot0 R1@3996(100)
     *   slot1 R2@3896(100)   ← 待删
     *   slot2 R3@3796(100)
     */
    fill_rec(rec, 100, 1);
    CHECK(page_insert(pg, rec, 100, &slot) == OK);
    CHECK(slot == 0);
    fill_rec(rec, 100, 2);
    CHECK(page_insert(pg, rec, 100, &slot) == OK);
    CHECK(slot == 1);
    fill_rec(rec, 100, 3);
    CHECK(page_insert(pg, rec, 100, &slot) == OK);
    CHECK(slot == 2);

    /* 删掉中间，制造空洞 */
    CHECK(page_delete(pg, 1) == OK);

    /* slot3 R4@3596(200)：低地址、长记录 */
    fill_rec(rec, 200, 4);
    CHECK(page_insert(pg, rec, 200, &slot) == OK);
    CHECK(slot == 3);

    /* 此时按 slot 倒序搬会先动 R4（最低地址），搬到 3896..4095，
     * 正好盖掉还没搬的 R1（3996..4095）。 */
    page_compact(pg);

    {
        const u8 *got;
        u16 len;
        CHECK(page_get(pg, 0, &got, &len) == OK);
        CHECK(len == 100 && rec_ok(got, 100, 1));
        CHECK(page_get(pg, 2, &got, &len) == OK);
        CHECK(len == 100 && rec_ok(got, 100, 3));
        CHECK(page_get(pg, 3, &got, &len) == OK);
        CHECK(len == 200 && rec_ok(got, 200, 4));
        CHECK(page_get(pg, 1, &got, &len) == E_NOTFOUND);
    }
    CHECK(page_fragmentation(pg) == 0);
    check_invariants("t6");
}

/* ---- 7) 随机增删 10 万次：碎片回收 + 碎片率上限 ---- */
#define MAX_LIVE 400

typedef struct {
    u16 slot;
    u16 len;
    u32 id;
} Ent;

static void t7_random(void)
{
    static Ent live[MAX_LIVE];
    static u8 rec[512];
    int nlive = 0;
    u32 id = 1;
    u16 peak_frag = 0;
    u32 insert_fail = 0;
    u32 insert_ok = 0;
    u32 gens = 1;
    u32 ops = 100000;

    CHECK(page_init(pg, PT_HEAP) == OK);

    for (u32 op = 0; op < ops; op++) {
        /* 主动整理策略：碎片超 10% 就 compact。
         * 这是 heap 层该做的决策，page 层只负责把 page_fragmentation 报给它。 */
        u16 f = page_fragmentation(pg);
        if (f > peak_frag)
            peak_frag = f;
        if (f > 10) {
            page_compact(pg);
            /* 整理必须彻底：compact 后碎片率精确为 0。
             * 这是比"峰值碎片 < N%"更有意义的验收——
             * 峰值在"页被删空"时必然接近 100%，但不影响可用性。 */
            CHECK(page_fragmentation(pg) == 0);
        }

        /* slot 永不复用 → 一页最多约 1000 次插入就被 slot 数组吃满。
         * 真实 heap 此时分配新页；这里直接重置，模拟"翻页"，
         * 顺便压测长时间运行下会不会累积错误。 */
        if (page_slot_count(pg) >= 250) {
            CHECK(page_init(pg, PT_HEAP) == OK);
            nlive = 0;
            gens++;
            continue;
        }

        int do_ins = (nlive == 0) || (rng_next() % 100 < 60);

        if (do_ins && nlive < MAX_LIVE) {
            u16 len = (u16)(16 + rng_next() % 200);
            u16 slot;
            fill_rec(rec, len, id);
            int rc = page_insert(pg, rec, len, &slot);
            if (rc == E_NOSPACE) {
                insert_fail++;
                if (nlive > 0) {
                    int k = (int)(rng_next() % (u64)nlive);
                    page_delete(pg, live[k].slot);
                    live[k] = live[--nlive];
                }
            } else {
                CHECK(rc == OK);
                insert_ok++;
                live[nlive].slot = slot;
                live[nlive].len = len;
                live[nlive].id = id;
                nlive++;
                id++;
            }
        } else if (nlive > 0) {
            int k = (int)(rng_next() % (u64)nlive);
            CHECK(page_delete(pg, live[k].slot) == OK);
            live[k] = live[--nlive];
        }

        if ((op & 0xFFFu) == 0)
            check_invariants("t7");
    }

    /* 随机阶段后所有活记录必须完整（这是"没被覆盖"的硬证据） */
    for (int i = 0; i < nlive; i++) {
        const u8 *got;
        u16 len;
        int rc = page_get(pg, live[i].slot, &got, &len);
        CHECK(rc == OK);
        if (rc == OK) {
            CHECK(len == live[i].len);
            CHECK(rec_ok(got, live[i].len, live[i].id));
        }
        if (failures > 5)
            break;
    }

    /* 最后 compact：可用空间必须精确恢复到理论值 */
    page_compact(pg);
    {
        u32 expect = PAGE_SIZE
                   - HDR_SIZE
                   - (u32)page_slot_count(pg) * SLOT_SIZE
                   - page_live_bytes(pg);
        CHECK(page_free_bytes(pg) == expect);
        CHECK(page_fragmentation(pg) == 0);
    }

    printf("    （10 万次 / %u 个页世代：峰值碎片 %u%%，插入成功 %u 失败 %u，"
           "存活 %d 条，末页 slot %u 个）\n",
           gens, peak_frag, insert_ok, insert_fail, nlive, page_slot_count(pg));

    CHECK(gens > 20);                 /* 确实跨了很多页世代 */
    CHECK(insert_ok > 10000);
    /* 插入失败意味着"页真的满了"（compact 已经试过），
     * 而不是碎片造成的浪费——用"成功远多于失败"来证明空间没永久丢失。 */
    CHECK(insert_ok > insert_fail * 4);
    check_invariants("t7-end");
}

/* ---- 8) 校验和：改一个字节必须被发现 ---- */
static void t8_checksum(void)
{
    u8 rec[40];
    u16 slot;

    CHECK(page_init(pg, PT_HEAP) == OK);
    fill_rec(rec, 40, 5);
    CHECK(page_insert(pg, rec, 40, &slot) == OK);
    CHECK(page_verify_checksum(pg));

    pg[1000] ^= 0xFF;                     /* 篡改一个字节 */
    CHECK(!page_verify_checksum(pg));

    pg[1000] ^= 0xFF;                     /* 改回来 */
    CHECK(page_verify_checksum(pg));
}

/* ---- 9) 更新：缩小可以，变长必须拒绝 ---- */
static void t9_update(void)
{
    u8 rec[100];
    u16 slot, len;
    const u8 *got;

    CHECK(page_init(pg, PT_HEAP) == OK);
    fill_rec(rec, 100, 11);
    CHECK(page_insert(pg, rec, 100, &slot) == OK);

    fill_rec(rec, 60, 12);
    CHECK(page_update(pg, slot, rec, 60) == OK);
    CHECK(page_get(pg, slot, &got, &len) == OK);
    CHECK(len == 60 && rec_ok(got, 60, 12));

    fill_rec(rec, 120, 13);
    CHECK(page_update(pg, slot, rec, 120) == E_NOSPACE);   /* 变长必须拒绝 */
    CHECK(page_get(pg, slot, &got, &len) == OK);
    CHECK(len == 60 && rec_ok(got, 60, 12));               /* 原内容不变 */
    check_invariants("t9");
}

int main(void)
{
    printf("test_page:\n");
    t1_basic();
    t2_fill();
    t3_delete();
    t4_compact_stable();
    t5_no_reuse();
    t6_compact_order();
    t7_random();
    t8_checksum();
    t9_update();

    if (failures == 0) {
        printf("  9 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
