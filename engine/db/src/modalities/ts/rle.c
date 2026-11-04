/*
 * rle.c —— 整数列的 delta + RLE 编码（v1.5 步骤 1 · 压缩三件套之二）
 *
 * 契约见 rle.h（格式、返回值都在那里定死了）。
 * 这里补实现层面的说明与踩过的坑。
 *
 * ==================== 格式（v1） ====================
 *
 *   一串游程，每段：
 *       [varint] count          这一段有多少个值（≥ 1）
 *       [varint] zigzag(delta)  段内每个值 = 段首值；
 *                                段首值 = 上一段的段首值 + delta
 *   第一段的"上一段段首值"取 0。
 *
 * delta 是相对**上一段的段首值**，不是相对上一个点 ——
 * 这样一段里所有值共用一个 delta，游程才有意义。
 *
 * ==================== 返回码：负数 = 失败 ====================
 *
 * rle_encode 成功时返回**写入的字节数**，rle_decode 成功时返回**解出的条数**，
 * 两者都是 ≥ 0 的数；失败必须返回**负的**错误码（-E_NOSPACE / -E_CORRUPT / -E_INVAL）。
 *
 * ⚠️ 这与项目里其它地方（错误码是**正数**的 E_*）相反 ——
 * 但必须这么写：成功时要返回字节数，如果错误码是正数，
 * "写了 1 个字节"和"E_NOSPACE(=1)"就分不出来了。
 * 调用方按 `rc < 0` 判失败。
 *
 * ==================== 坑 M6-10：delta 用"模 2^64"语义算 ====================
 *
 * delta = 本段段首值 - 上一段段首值。**直接用 i64 相减会溢出**（UB）：
 *
 *   v = [INT64_MIN, INT64_MAX]
 *   INT64_MAX - INT64_MIN 在 i64 里根本表达不出来。
 *
 * 而 i64 边界附近的序列恰恰是真实数据（时间戳溢出、累加计数回绕、
 * 传感器异常值）。所以这里用**无符号回绕**语义：
 *
 *   delta_u = (u64)cur - (u64)prev        （模 2^64，自然回绕）
 *   编码时把 delta_u 的**位模式**当成 i64 送进 zigzag；
 *   解码时用无符号加法把 delta 加回 acc（同样模 2^64）。
 *
 * 二进制补码下"模 2^64 的差"与"有符号的差"是同一个位模式，
 * 所以**只要两端都用回绕加法，逐位全等还原恒成立**，
 * 连 INT64_MIN → INT64_MAX 这种极端序列也能原样还原。
 *
 * ==================== 坑 M6-11：位级 varint 每组占满一个字节 ====================
 *
 * tsbits 的 varint 是**位级**的：7 位一组，每组写满 8 位（续位占第 7 位）。
 * 于是"7 位有效数据"要占**一整个字节** ⇒ 一个 i64 的 delta 最坏 10 字节。
 *
 * 后果：**随机整数列的编码可能比原始数据还大**（原始 8 字节/点，
 * 随机 delta 编码后约 6~11 字节/点）。这不是 bug，是这个 varint 的性质 ——
 * 它换来的是"delta 很小"时的极致压缩（delta=1 只占 1 字节）。
 * A3 的 bench 要按这个口径看压缩比，别把它当成"压缩算法一定变小"。
 *
 * ==================== 坑 M6-12：解码端必须吃干净整条流 ====================
 *
 * 解出 n 个值之后，流里**必须正好用完**：
 * 尾部多一个字节 ⇒ E_CORRUPT。
 *
 * 为什么不"读够 n 个就收工"：那样多出来的垃圾会被静默忽略 ——
 * 一段坏数据就变成了"看起来对"的数据（这个库里最不能忍的一类错误）。
 *
 * ==================== 坑 M6-13：count = 0 是畸形输入 ====================
 *
 * 编码器**永不**写出 count = 0（游程至少 1 个值），
 * 所以解到 count = 0 就是数据坏了 ⇒ E_CORRUPT。
 * 同理 count 超过"还差多少个值"也是坏的 ⇒ E_CORRUPT。
 */

#include <limits.h>
#include <string.h>

#include "modalities/ts/rle.h"
#include "modalities/ts/tsbits.h"

/* 一个 varint 最多 10 字节（64 位，每组 7 位 ⇒ 10 组） */
#define RLE_VARINT_MAX 10u

/* 一段最坏 2 个 varint（count + delta） */
#define RLE_RUN_MAX (2u * RLE_VARINT_MAX)

/*
 * 位模式互转（memcpy，避开"有符号↔无符号"转换的实现定义行为）。
 */
static i64 rle_u64_to_i64(u64 v)
{
    i64 r;

    memcpy(&r, &v, sizeof(r));
    return r;
}

/* 编码上限：段数 ≤ n，每段最坏 2 个 varint */
u32 rle_encoded_bound(u32 n)
{
    u64 bound = (u64)n * (u64)RLE_RUN_MAX;

    if (bound > 0xFFFFFFFFull) {
        return 0xFFFFFFFFu;   /* 夹住：u32 表达不了就返回最大值 */
    }
    return (u32)bound;
}

/*
 * 编码：n 个整数 → out[0 .. cap)。
 *
 * 返回写入的字节数；失败返回**负的**错误码。
 */
int rle_encode(const i64 *v, u32 n, u8 *out, u32 cap)
{
    TsBitWriter w;
    u64 prev = 0;        /* 上一段段首值的位模式（首段取 0） */
    u32 i = 0;
    u32 bytes = 0;
    int rc;

    if (!v && n > 0) {
        return -E_INVAL;
    }
    if (!out && cap > 0) {
        return -E_INVAL;
    }
    if (n > (u32)INT_MAX) {
        return -E_INVAL;  /* 返回值是 int，装不下 */
    }

    tsbw_init(&w, out, cap);

    while (i < n) {
        u32 j = i + 1u;
        u64 delta;

        /* 找游程尾：连续相同的值 */
        while (j < n && v[j] == v[i]) {
            j++;
        }

        /* count（≥ 1） */
        rc = tsbw_write_varint(&w, (u64)(j - i));
        if (rc != OK) {
            return -E_NOSPACE;   /* 写不下（写器只可能因越界失败） */
        }

        /*
         * delta：相对**上一段的段首值**（坑 M6-10：模 2^64 回绕）。
         */
        delta = (u64)v[i] - prev;
        rc = tsbw_write_varint(&w, ts_zigzag_i64(rle_u64_to_i64(delta)));
        if (rc != OK) {
            return -E_NOSPACE;
        }

        prev = (u64)v[i];
        i = j;
    }

    rc = tsbw_finish(&w, &bytes);
    if (rc != OK) {
        return -E_NOSPACE;
    }
    if (bytes > (u32)INT_MAX) {
        return -E_NOSPACE;
    }
    return (int)bytes;
}

/*
 * 解码：buf 的前 len 字节 → v[0 .. n)。
 *
 * 返回解出的条数；失败返回**负的**错误码。
 */
int rle_decode(const u8 *buf, u32 len, i64 *v, u32 n)
{
    TsBitReader r;
    u64 prev = 0;        /* 上一段段首值的位模式 */
    u32 produced = 0;
    int rc;

    if (!buf && len > 0) {
        return -E_INVAL;
    }
    if (!v && n > 0) {
        return -E_INVAL;
    }
    if (n > (u32)INT_MAX) {
        return -E_INVAL;
    }

    tsbr_init(&r, buf, len);

    while (produced < n) {
        u64 cnt = 0;
        u64 z = 0;
        u64 acc;
        u32 c;
        u32 k;

        rc = tsbr_read_varint(&r, &cnt);
        if (rc != OK) {
            return -E_CORRUPT;   /* 读坏 / 越界 */
        }
        /* 坑 M6-13：count = 0 是畸形输入（编码器永不写出） */
        if (cnt == 0) {
            return -E_CORRUPT;
        }
        /* 条数对不上：这一段比"还差多少个值"还多 */
        if (cnt > (u64)(n - produced)) {
            return -E_CORRUPT;
        }
        c = (u32)cnt;

        rc = tsbr_read_varint(&r, &z);
        if (rc != OK) {
            return -E_CORRUPT;
        }

        /*
         * 段首值 = 上一段段首值 + delta（模 2^64 回绕，坑 M6-10）。
         */
        acc = prev + (u64)ts_unzigzag_i64(z);

        /* 段内每个值都等于段首值 */
        for (k = 0; k < c; k++) {
            v[produced + k] = rle_u64_to_i64(acc);
        }
        produced += c;
        prev = acc;
    }

    /*
     * 坑 M6-12：必须正好用完整条流。
     * 尾部还有字节 ⇒ 数据里有多余的东西 ⇒ 坏数据。
     */
    if (!tsbr_eof(&r)) {
        return -E_CORRUPT;
    }

    return (int)produced;
}
