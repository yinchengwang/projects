#ifndef DB_MODALITIES_TS_DOD_H
#define DB_MODALITIES_TS_DOD_H

#include "common/error.h"
#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dod.h —— 时间列的 delta-of-delta 编码（v1.5 步骤 1 · 压缩三件套之一）
 *
 * 【为什么时间列要 delta-of-delta】
 *
 * 时序数据的时间戳**间隔通常很稳**（每秒一个、每分钟一个）。
 * 直接存绝对时间戳：8 字节/点，且每个点都不同 ⇒ 压不动。
 * 存 delta（与前一个点的间隔）：间隔稳 ⇒ delta 稳 ⇒ 还是压不动多少。
 * 存 **delta-of-delta**（间隔的变化量）：间隔恒定 ⇒ **全是 0** ⇒
 * 一个 bit 就够 —— 这就是 Gorilla 论文里时间列的编码。
 *
 * 【格式（v1）】
 *
 *   [varint] zigzag(ts[0])                 第一个时间戳（绝对值）
 *   [varint] zigzag(d1)                    d1 = ts[1] - ts[0]
 *   对每个 i ≥ 2：
 *       [bit 0]            ⇒ dod = 0（间隔没变，最常见）
 *       [bit 1][varint]    ⇒ dod ≠ 0，值是 zigzag(dod)
 *   其中 dod = (ts[i] - ts[i-1]) - (ts[i-1] - ts[i-2])
 *
 * 解码必须**逐位全等**地还原原数组（包括乱序时间戳 —— 乱序不是错误，
 * 只是压缩率变差，这是计划里明确记下的代价）。
 *
 * 【变长整数与 zigzag】
 *
 * varint 的定义见 tsbits.h；zigzag 的作用是把负数也变成"小的无符号数"
 * （间隔可以是负的：乱序写入时 delta 为负）。
 *
 * 【错误处理】
 *
 * - 装不下 → E_NOSPACE（调用方应当整块作废，不要用半截数据）
 * - 读坏了 → E_CORRUPT（**绝不**填 0 蒙混过去）
 * - 解出来的条数与 n 不符 → E_CORRUPT
 *
 * ⚠️ **错误码是"负数"**（与 rle.h / gorilla.h 一致）：
 * 成功时返回的是**数量**（字节数 / 条数，≥ 0），
 * 所以失败必须用负数才能区分开 ⇒ 调用方一律用 `rc < 0` 判错，
 * **不要**写 `rc == E_CORRUPT`（那是 2，是正数，永远不成立）。
 * ⚠️ 这一条与仓里其它模块"rc = E_xxx（正数）"的写法**相反**，
 * 是本组三个编码**特有**的约定（坑 **M6-19**）。
 */

/* 编码上限（字节）：每点最坏 1 控制位 + 10 字节 varint，再加头部 20 字节 */
u32 dod_encoded_bound(u32 n);

/*
 * 编码：n 个时间戳 → out[0 .. cap)。
 * 返回写入的字节数；失败返回负的错误码（E_NOSPACE / E_INVAL）。
 */
int dod_encode(const i64 *ts, u32 n, u8 *out, u32 cap);

/*
 * 解码：buf 的前 len 字节 → ts[0 .. n)。
 * 返回解出的条数；失败返回负的错误码（E_CORRUPT / E_INVAL）。
 * ⚠️ 缓冲区里可能有多余的位（编码时按位对齐），**忽略尾部**即可。
 */
int dod_decode(const u8 *buf, u32 len, i64 *ts, u32 n);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_TS_DOD_H */
