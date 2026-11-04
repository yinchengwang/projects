/*
 * dod.c —— 时间列的 delta-of-delta 编码（v1.5 步骤 1 · 压缩三件套之一）
 *
 * 契约见 dod.h（格式、错误码、返回值都在那里定死了）。
 * 位流基元见 tsbits.h（**不要**在这里重写位流 —— 三个编码共用一份）。
 *
 * ==================== 格式（v1） ====================
 *
 *   [varint] zigzag(ts[0])                 第一个时间戳（绝对值）
 *   [varint] zigzag(d1)                    d1 = ts[1] - ts[0]
 *   对每个 i ≥ 2：
 *       [bit 0]            ⇒ dod = 0（间隔没变，最常见）
 *       [bit 1][varint]    ⇒ dod ≠ 0，值是 zigzag(dod)
 *   其中 dod = (ts[i] - ts[i-1]) - (ts[i-1] - ts[i-2])
 *
 * ==================== 返回值 ====================
 *
 * 按 dod.h 的契约：**成功返回条数 / 字节数（≥ 0），失败返回负的错误码**
 * （-E_NOSPACE / -E_CORRUPT / -E_INVAL）。
 * 注意这与仓里其它模块"rc = E_xxx（正数）"的写法**不一样** ——
 * 这里成功时返回的是**数量**，所以错误必须用负数才能区分开。
 *
 * ==================== 三个必须写下来的坑 ====================
 *
 * 坑 M6-6 · 差值运算必须在**无符号域**做
 * i64 相减可能溢出（ts[0] = INT64_MIN、ts[1] = INT64_MAX 时差值是 2^64-1，
 * 根本不是 i64）。有符号溢出是**未定义行为** —— 编译器会"发挥"，
 * 同一份代码换个优化级别就换一个结果。
 * 这里的做法：差值在 u64 里算（无符号回绕是**有定义**的），
 * 需要当 i64 用时用一个**不依赖实现定义**的补码转换函数换回来。
 *
 * 坑 M6-7 · 输出缓冲必须先清零
 * tsbw_write_bits 只改**单个 bit**（置位用 |=，清位用 &= ~），
 * 它**不会**把整个字节清零。所以缓冲里残留的旧内容会以"多出来的位"
 * 的形式混进结果 —— 而且**不报错**，只是解出来是垃圾。
 *
 * 坑 M6-8 · varint 是**位级**的
 * 每组 8 位：低 7 位是数据、第 7 位（最高位）是续位。
 * 这与常见的"整字节 varint（LEB128）"不是一回事 ——
 * 拿别处的实现来替换，读出来全是垃圾，而且不报错。
 * （这也是为什么三个编码共用 tsbits 这一份位流。）
 */

#include "modalities/ts/dod.h"

#include <string.h>

#include "modalities/ts/tsbits.h"

/* ---------------- 小工具 ---------------- */

/*
 * u64 → i64（补码解释）。
 *
 * 为什么不直接写 (i64)v：C99 里"超出 i64 范围的 u64 转 i64"是
 * **实现定义**的（gcc 是按二补码截断，但标准不保证）。
 * 这里显式算出来，换成任何实现都得到同一个结果。
 */
static i64 dod_i64_of(u64 v)
{
    if (v <= (u64)INT64_MAX)
        return (i64)v;
    /* 负数：~v 是"除符号位外的补码"，取负再减 1 */
    return -(i64)(~v) - 1;
}

/* ---------------- 编码上界 ---------------- */

u32 dod_encoded_bound(u32 n)
{
    u64 b = 20u + 11u * (u64)n;

    if (b > 0xFFFFFFFFull)
        return 0xFFFFFFFFu;    /* 饱和：调用方看到这么大的上界只会放弃 */
    return (u32)b;
}

/* ---------------- 编码 ---------------- */

int dod_encode(const i64 *ts, u32 n, u8 *out, u32 cap)
{
    TsBitWriter w;
    u32 bytes = 0;
    int rc;

    /* 空数组：什么都没有 ⇒ 0 字节（out 可以是 NULL） */
    if (n == 0)
        return 0;
    if (!ts || !out)
        return -E_INVAL;

    /*
     * 坑 M6-7：位写入器只改单个 bit，**不清整个字节** ——
     * 缓冲里残留的旧内容会以"多出来的位"混进结果，且不报错。
     * 所以先把要写的区域清零。
     */
    if (cap > 0)
        memset(out, 0, cap);

    tsbw_init(&w, out, cap);

    /* 第一个时间戳：绝对值，zigzag 编码 */
    rc = tsbw_write_varint(&w, ts_zigzag_i64(ts[0]));
    if (rc != OK)
        return -rc;

    if (n >= 2) {
        /* 坑 M6-6：差值在 u64 里算（回绕有定义） */
        u64 d1 = (u64)ts[1] - (u64)ts[0];

        rc = tsbw_write_varint(&w, ts_zigzag_i64(dod_i64_of(d1)));
        if (rc != OK)
            return -rc;

        if (n >= 3) {
            u64 prev = d1;

            for (u32 i = 2; i < n; i++) {
                u64 cur = (u64)ts[i] - (u64)ts[i-1];
                u64 dod = cur - prev;      /* delta-of-delta */

                if (dod == 0) {
                    /* 最常见的情况：间隔没变 ⇒ 一个 bit 就够 */
                    rc = tsbw_write_bit(&w, false);
                } else {
                    rc = tsbw_write_bit(&w, true);
                    if (rc == OK)
                        rc = tsbw_write_varint(&w, ts_zigzag_i64(dod_i64_of(dod)));
                }
                if (rc != OK)
                    return -rc;
                prev = cur;
            }
        }
    }

    rc = tsbw_finish(&w, &bytes);
    if (rc != OK)
        return -rc;
    return (int)bytes;
}

/* ---------------- 解码 ---------------- */

int dod_decode(const u8 *buf, u32 len, i64 *ts, u32 n)
{
    TsBitReader r;
    u64 z = 0;
    int rc;

    if (n == 0)
        return 0;
    if (!buf || !ts)
        return -E_INVAL;

    tsbr_init(&r, buf, len);

    /* 第一个时间戳 */
    rc = tsbr_read_varint(&r, &z);
    if (rc != OK)
        return -rc;              /* 读坏 ⇒ E_CORRUPT，绝不填 0 蒙混 */
    ts[0] = ts_unzigzag_i64(z);

    if (n >= 2) {
        u64 prev_d;

        rc = tsbr_read_varint(&r, &z);
        if (rc != OK)
            return -rc;
        /* 坑 M6-6：delta 存成 u64（回绕），加法也在 u64 里做 */
        prev_d = (u64)ts_unzigzag_i64(z);
        ts[1] = dod_i64_of((u64)ts[0] + prev_d);

        for (u32 i = 2; i < n; i++) {
            bool nonzero = false;
            u64 cur_d;

            rc = tsbr_read_bit(&r, &nonzero);
            if (rc != OK)
                return -rc;
            if (nonzero) {
                rc = tsbr_read_varint(&r, &z);
                if (rc != OK)
                    return -rc;
                cur_d = prev_d + (u64)ts_unzigzag_i64(z);
            } else {
                cur_d = prev_d;
            }
            ts[i] = dod_i64_of((u64)ts[i - 1] + cur_d);
            prev_d = cur_d;
        }
    }

    /*
     * 缓冲区里可能有多余的位（编码时按位对齐，尾部有 0 填充）——
     * 按契约**忽略尾部**即可。
     *
     * 坑 M6-9：但"截短"一定能被发现 ——
     * 尾部填充最多 7 位，砍掉 1 字节（8 位）必然少掉至少 1 个**有效**位
     * ⇒ 读的时候越界 ⇒ E_CORRUPT。
     */
    return (int)n;
}
