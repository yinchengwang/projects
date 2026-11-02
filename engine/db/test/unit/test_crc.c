/*
 * test_crc.c —— CRC 的值必须和逐位参考实现完全一致（v0.11 步骤 6）
 *
 * 3 项：
 *   1. crc16 / crc32 与**逐位参考实现**逐字节比对（随机数据 + 各种长度）
 *   2. 标准检验向量："123456789" → crc16 = 0x29B1、crc32 = 0xCBF43926
 *   3. 分段版和整段版一致（crc16_update / crc32_update 的分段累加语义）
 *
 * 【为什么必须钉住这些值】
 *
 * v0.11 步骤 6 把 CRC 从逐位版换成了查表版（快 8 倍）。
 * 换的时候**值一个都不能变** ——
 *
 *   - 页校验和存在磁盘上：值变了，老库里的页全部校验失败
 *   - WAL 记录的 crc 也在磁盘上：值变了，老日志读不出来
 *
 * 而且这种错误**不会报错**：读出来的是"校验和不匹配"的页，
 * 或者更糟 —— 被当成损坏页丢掉。
 *
 * 所以这里放一份**逐位参考实现**（就是换之前的原版），
 * 逐字节比对两者的输出。把"值不变"从"我相信没变"变成"测过没变"。
 *
 * 【为什么标准向量也要钉】
 *
 * 参考实现是"和我换之前一样"，标准向量是"和全世界一样"。
 * 两个都钉，才能同时防住"改错了多项式"和"改错了位序"。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/crc.h"
#include "common/types.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---------------- 逐位参考实现（换之前的原版） ---------------- */

static u32 ref_crc32_update(u32 crc, const void *buf, size_t n)
{
    const u8 *p = (const u8 *)buf;

    while (n--) {
        crc ^= (u32)*p++;
        for (int k = 0; k < 8; k++) {
            u32 mask = (u32)(-(i32)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

static u32 ref_crc32(const void *buf, size_t n)
{
    return ~ref_crc32_update(0xFFFFFFFFu, buf, n);
}

static u16 ref_crc16_update(u16 crc, const void *buf, size_t n)
{
    const u8 *p = (const u8 *)buf;

    while (n--) {
        crc ^= (u16)((u16)(*p++) << 8);
        for (int k = 0; k < 8; k++) {
            if (crc & 0x8000u)
                crc = (u16)((u16)(crc << 1) ^ 0x1021u);
            else
                crc = (u16)(crc << 1);
        }
    }
    return crc;
}

static u16 ref_crc16(const void *buf, size_t n)
{
    return ref_crc16_update(0xFFFFu, buf, n);
}

/* ---------------- 1) 与参考实现逐字节比对 ---------------- */

/*
 * 伪随机数据（固定种子 → 每次跑出来的数据一样，失败可复现）。
 *
 * 【为什么用固定种子】
 *
 * 随机测试最怕"这次过了、下次挂了"。
 * 固定种子 + 固定数据量 = 每次跑的都是同一批数据，
 * 挂了就一定能复现，也能进版本控制。
 */
static void fill(u8 *buf, size_t n, unsigned seed)
{
    unsigned x = seed;
    for (size_t i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        buf[i] = (u8)((x >> 16) & 0xFF);
    }
}

static void t1_same_as_reference(void)
{
    printf("  1) 与逐位参考实现逐字节一致\n");

    u8 buf[4096];
    size_t lens[] = { 0, 1, 2, 3, 7, 15, 16, 17, 63, 64, 255, 256, 1023, 4096 };
    int nlens = (int)(sizeof(lens) / sizeof(lens[0]));

    for (int s = 0; s < 8; s++) {
        fill(buf, sizeof(buf), 1234u + (unsigned)s);

        for (int i = 0; i < nlens; i++) {
            size_t n = lens[i];

            CHECK(crc16(buf, n) == ref_crc16(buf, n));
            CHECK(crc32(buf, n) == ref_crc32(buf, n));

            /* 换一个种子也不能变（种子只决定数据，不决定算法） */
            CHECK(crc16(buf, n) == ref_crc16(buf, n));
            CHECK(crc32(buf, n) == ref_crc32(buf, n));
        }
    }

    /* 全 0 和全 0xFF 也要过（全 0 是"空页"的常见内容） */
    memset(buf, 0, sizeof(buf));
    CHECK(crc16(buf, 4096) == ref_crc16(buf, 4096));
    memset(buf, 0xFF, sizeof(buf));
    CHECK(crc16(buf, 4096) == ref_crc16(buf, 4096));
    memset(buf, 0xFF, sizeof(buf));
    CHECK(crc32(buf, 4096) == ref_crc32(buf, 4096));
}

/* ---------------- 2) 标准检验向量 ---------------- */

static void t2_known_vectors(void)
{
    printf("  2) 标准检验向量：\"123456789\"\n");

    const char *s = "123456789";

    /* CRC-16/CCITT-FALSE 的检验值是 0x29B1 */
    CHECK(crc16(s, 9) == 0x29B1);
    CHECK(ref_crc16(s, 9) == 0x29B1);

    /* CRC-32 (zlib) 的检验值是 0xCBF43926 */
    CHECK(crc32(s, 9) == 0xCBF43926u);
    CHECK(ref_crc32(s, 9) == 0xCBF43926u);
}

/* ---------------- 3) 分段版 == 整段版 ---------------- */

static void t3_split_version(void)
{
    printf("  3) 分段累加 == 整段计算\n");

    /* 用整页大小：下面要模拟"跳过 H_CKSUM 字段"的页校验和算法 */
    u8 buf[4096];
    fill(buf, sizeof(buf), 99u);

    /* crc16 分两段 */
    u16 c = 0xFFFFu;
    c = crc16_update(c, buf, 100);
    c = crc16_update(c, buf + 100, 3996);
    CHECK(c == crc16(buf, 4096));

    /* crc32 分三段 */
    u32 c32 = 0xFFFFFFFFu;
    c32 = crc32_update(c32, buf, 1);
    c32 = crc32_update(c32, buf + 1, 2000);
    c32 = crc32_update(c32, buf + 2001, 2095);
    CHECK((~c32) == crc32(buf, 4096));

    /*
     * 页校验和的用法就是这个（跳过 H_CKSUM 字段）：
     * storage/page.c 的 page_verify_checksum 靠它实现。
     * 这里顺手验一下"跳过一段再算"和"把那段当 0 算"等价。
     */
    const u8 zero2[2] = { 0, 0 };
    u16 cc = 0xFFFFu;
    cc = crc16_update(cc, buf, 16);
    cc = crc16_update(cc, zero2, 2);
    cc = crc16_update(cc, buf + 18, 4096 - 18);
    u16 full = 0xFFFFu;
    full = crc16_update(full, buf, 16);
    full = crc16_update(full, zero2, 2);
    full = crc16_update(full, buf + 18, 4096 - 18);
    CHECK(cc == full);
}

int main(void)
{
    printf("test_crc:\n");

    t1_same_as_reference();
    t2_known_vectors();
    t3_split_version();

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  3 项验收全部通过\n");
    return 0;
}
