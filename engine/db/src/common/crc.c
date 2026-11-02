#include "crc.h"

#include <pthread.h>

/*
 * 【为什么现在是查表版】
 *
 * 原版是逐位算的：每个字节 8 次移位异或。
 *   - 页校验和要算 4KB  = 32768 次位运算
 *   - WAL 记录要算 8KB+ = 65536 次位运算
 *
 * 而图的写路径每加一条边要改 4 个页（边记录 / 原链头 / src / dst），
 * 每条边 = 4 次页校验和 + 4 次 WAL 记录 ≈ 26 万次位运算。
 *
 * v0.11 步骤 6 的性能基准实测：10 万条边的装载时间里，
 * **绝大部分花在这两项上**（不是 IO，也不是图逻辑）。
 *
 * 这个文件原来的注释写着：
 *   "不做查表优化。第一版要的是"能读懂、能验算"，
 *    等 bench 显示它成为瓶颈再换成查表版。"
 *
 * 现在就是那个时候 —— 基准测试的意义就在这儿：
 * 它不只是用来验收的，是用来**找瓶颈**的。
 *
 * 【换完值不变】
 *
 * 查表版和逐位版算的是同一个多项式、同一种位序，输出**逐字节相同**。
 * 这不是"应该一样"，是 test_crc.c 里逐字节比对出来的 ——
 * 那份测试里保留了一份逐位参考实现。
 *
 * 为什么必须一样：页校验和存在磁盘上（WAL 里也是）。
 * 值变了 = 老的库全部读不出来，而且不报错（读出来的是垃圾）。
 *
 * 【表的构造为什么用 pthread_once】
 *
 * 表要是每次调用现算，就等于没优化。
 * 现算一次 + 存在静态数组里，用 pthread_once 保证"只算一次"且线程安全
 * （和 common/latch.c 用 pthread_once 是同一个理由）。
 *
 * 用 pthread_once 而不是 "if (!ready) build()"，是因为后者在多线程下会 race：
 * 两个线程同时进 build_tables，写同一张表 → 表被写坏 → CRC 全错。
 */

/* crc16：MSB 优先，多项式 0x1021（CRC-16/CCITT） */
static u16 crc16_tab[256];

/* crc32：反射式，多项式 0xEDB88320（和 zlib 的 crc32 一样） */
static u32 crc32_tab[256];

static pthread_once_t g_tab_once = PTHREAD_ONCE_INIT;

static void build_tables(void)
{
    for (int i = 0; i < 256; i++) {
        /* crc16：把 i 当成高 8 位，推进 8 次 */
        u16 c16 = (u16)(i << 8);
        for (int k = 0; k < 8; k++) {
            if (c16 & 0x8000u)
                c16 = (u16)((u16)(c16 << 1) ^ 0x1021u);
            else
                c16 = (u16)(c16 << 1);
        }
        crc16_tab[i] = c16;

        /* crc32：反射式，同样推进 8 次 */
        u32 c32 = (u32)i;
        for (int k = 0; k < 8; k++) {
            u32 mask = (u32)(-(i32)(c32 & 1u));
            c32 = (c32 >> 1) ^ (0xEDB88320u & mask);
        }
        crc32_tab[i] = c32;
    }
}

/*
 * crc32 的分段版本。
 *
 * 为什么需要分段：WAL 记录的 crc 覆盖 "header + payload"，
 * 而 payload 是分两次写进去的（before 镜像、after 镜像）。
 * 与其把 4KB 拷到临时缓冲再算一次，不如分段累加。
 *
 * 种子约定和 crc32 一致：初始传 0xFFFFFFFFu，最后取反。
 * 所以 "crc32(a) == crc32_update(0xFFFFFFFF, a)"，
 * 但 "crc32(a) + crc32(b)" 不等于 "crc32(a+b)" —— 要用分段版。
 */
u32 crc32_update(u32 crc, const void *buf, size_t n)
{
    const u8 *p = (const u8 *)buf;

    pthread_once(&g_tab_once, build_tables);

    while (n--) {
        crc = (crc >> 8) ^ crc32_tab[(crc ^ *p++) & 0xFFu];
    }
    return crc;
}

u32 crc32(const void *buf, size_t n)
{
    return ~crc32_update(0xFFFFFFFFu, buf, n);
}

/*
 * crc16 的分段版本。
 *
 * 同样的分段理由：页校验和要跳过 H_CKSUM 字段，
 * 分段算比拷 4KB 便宜（见 storage/page.c 的 page_verify_checksum）。
 */
u16 crc16_update(u16 crc, const void *buf, size_t n)
{
    const u8 *p = (const u8 *)buf;

    pthread_once(&g_tab_once, build_tables);

    while (n--) {
        crc = (u16)((u16)(crc << 8) ^ crc16_tab[((crc >> 8) ^ *p++) & 0xFFu]);
    }
    return crc;
}

u16 crc16(const void *buf, size_t n)
{
    return crc16_update(0xFFFFu, buf, n);
}
