#ifndef DB_MODALITIES_TS_RLE_H
#define DB_MODALITIES_TS_RLE_H

#include "common/error.h"
#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * rle.h —— 整数列的 delta + RLE 编码（v1.5 步骤 1 · 压缩三件套之二）
 *
 * 【为什么是 "delta + RLE" 而不是纯 RLE】
 *
 * 时序里的"标签"列（tag id、设备号、状态码）有两个性质：
 * ① 相邻值**常常重复**（同一台设备连着上报同一个状态）⇒ RLE 有效；
 * ② 相邻值**常常只差一点点**（计数器、累加值）⇒ delta 有效。
 * 两个性质同时用上，就是 "delta 之后 RLE"：
 * 先把值变成"与前一个**游程首值**的差"，再对相同差值的连续段做游程压缩。
 *
 * 【格式（v1）】
 *
 *   一串游程，每段：
 *       [varint] count          这一段有多少个值（≥ 1）
 *       [varint] zigzag(delta)  段内每个值 = 段首值；
 *                                段首值 = 上一段的段首值 + delta
 *   第一段的"上一段段首值"取 0。
 *
 * 注意：delta 是相对**上一段的段首值**，不是相对上一个点 ——
 * 这样一段里所有值共用一个 delta，游程才有意义。
 *
 * 【错误处理】
 *
 * - 装不下 → E_NOSPACE
 * - 读坏了 / 解出来的条数与 n 不符 → E_CORRUPT
 *
 * ⚠️ **错误码是"负数"**（与 dod.h / gorilla.h 一致）：
 * 成功时返回的是**数量**（字节数 / 条数，≥ 0），
 * 所以失败必须用负数才能区分开 ⇒ 调用方一律用 `rc < 0` 判错，
 * **不要**写 `rc == E_CORRUPT`（那是 2，是正数，永远不成立）。
 * ⚠️ 这一条与仓里其它模块"rc = E_xxx（正数）"的写法**相反**，
 * 是本组三个编码**特有**的约定（坑 **M6-19**）。
 */

/* 编码上限（字节）：每段最坏 2 个 varint（各 10 字节），段数 ≤ n */
u32 rle_encoded_bound(u32 n);

/*
 * 编码：n 个整数 → out[0 .. cap)。
 * 返回写入的字节数；失败返回负的错误码。
 */
int rle_encode(const i64 *v, u32 n, u8 *out, u32 cap);

/*
 * 解码：buf 的前 len 字节 → v[0 .. n)。
 * 返回解出的条数；失败返回负的错误码。
 */
int rle_decode(const u8 *buf, u32 len, i64 *v, u32 n);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_TS_RLE_H */
