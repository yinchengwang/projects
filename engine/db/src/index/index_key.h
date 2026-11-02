#ifndef DB_INDEX_INDEX_KEY_H
#define DB_INDEX_INDEX_KEY_H

#include "common/error.h"
#include "common/types.h"
#include "storage/value.h"
#include "storage/record.h"

/*
 * 索引 key = **值部分 || RID（8 字节大端）**
 *
 * ==================== 为什么 key 必须带 RID ====================
 *
 * 索引必须能区分"同一列值的多行"。如果 key 只有列值：
 *
 *   1. 叶子分裂时提升的 separator = key[mid]，左页保留 key[0..mid-1]
 *   2. 内部节点的不变式是「左子树所有 key **严格小于** separator」
 *   3. 值重复时，左页里可能有 key[mid-1] == key[mid] —— 不变式破了
 *   4. 查找时 descend 用 upper_bound（第一个 sep > key），
 *      K == sep 会走到**右**子树，左页里那些同值行**再也够不着**
 *
 * 症状是**静默少行**：走索引的查询比全扫少结果，而且不报任何错。
 * （v1.1 的执行器基准第一次跑就撞上了：10 万行里 `val >= 900`
 *   走索引返回 9999 行，全扫返回 10000 行。）
 *
 * 带上 RID 之后 key 全局唯一，separator 严格落在左右两页之间，
 * 不变式恒成立 —— **B+tree 本体一行都不用改**。
 *
 * ==================== 字节序契约 ====================
 *
 * B+tree 用 memcmp 比较 key，所以 key 必须是"字节序 = 数值序"的编码：
 *
 *   整数：大端 + 翻转符号位（-1 必须排在 1 前面）
 *   浮点：符号位翻转（IEEE754 位模式本身不单调）
 *   文本：原字节 + 0x00 转义 + "0x00 0x00" 终止符
 *         终止符的作用是让"值部分"互不为前缀 ——
 *         否则比较复合键时会跑进 RID 部分，顺序就乱了。
 *
 * RID 追加在值部分之后，大端 8 字节 —— 同值多行按 RID 升序。
 *
 * ==================== 长度上限 ====================
 *
 * 值部分最长 = 文本 255 字节（VAL_TEXT_MAX - 1）+ 2 字节终止符 = 257，
 * 加 RID 8 字节 = 265。BT_MAX_KEY 取 288，留出余量。
 */

/* NULL 值返回 E_INVAL —— 索引只存非 NULL 值 */
int index_key_encode(const ColDesc *col, const DbValue *v, u64 rid,
                      u8 *buf, u16 cap, u16 *len_out);

/*
 * 下推边界。
 *
 * 复合键是「值部分 || RID」，RID 有 8 字节取值空间，
 * 所以同一个值的所有行都落在 [lo, hi] 这个区间里：
 *
 *   lo = 值部分 || 00 00 00 00 00 00 00 00   （最小可能 RID）
 *   hi = 值部分 || FF FF FF FF FF FF FF FF   （最大可能 RID）
 *
 * 区间是闭区间，所以等值查询用 [lo, hi] 正好覆盖该值的所有行。
 *
 * 【注意】范围查询（> / <）也用闭区间 —— 下推只是"少读一些行"，
 * 不是"替你判断"，精确过滤由执行器的 WHERE 复检完成。
 */
int index_key_bound_lo(const ColDesc *col, const DbValue *v,
                        u8 *buf, u16 cap, u16 *len_out);
int index_key_bound_hi(const ColDesc *col, const DbValue *v,
                        u8 *buf, u16 cap, u16 *len_out);

#endif /* DB_INDEX_INDEX_KEY_H */
