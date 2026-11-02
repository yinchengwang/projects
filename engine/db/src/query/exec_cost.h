#ifndef DB_QUERY_EXEC_COST_H
#define DB_QUERY_EXEC_COST_H

#include "query/exec_internal.h"

/*
 * 规则 CBO 的代价模型（v1.1 步骤 4）。
 *
 * ==================== 为什么叫"规则 CBO" ====================
 *
 * 真正的 CBO 要有列统计、直方图、相关性、页数……
 * 本版只有**一个数字**：nrows_est。
 * 一个数字撑不起真代价模型，但足够回答一个问题：
 *
 *   「这条谓词，走索引真的比全表扫便宜吗？」
 *
 * 所以这一层是**可替换的骨架**：公式将来可以换成更细的，
 * 决策框架（门槛 → 代价 → 排序）不用动。
 *
 * ==================== 两层决策 ====================
 *
 *   门槛：模态说"能不能下推"（can_push）—— 这是**能力**，不是代价
 *   排序：我们算"值不值得"（cost_idx vs cost_seq）—— 这是**代价**
 *
 * 两层都要过。只有能力没有代价 → 小表也走索引（比全扫还慢）；
 * 只有代价没有能力 → 对不支持的模态瞎指挥。
 *
 * ==================== 代价公式 ====================
 *
 *   cost_seq = nrows × 1.0                    每行一次元组访问
 *   cost_idx = 树高 + sel × nrows × 1.2       树高次页访问 + 回表（1.2 是回表的惩罚系数）
 *
 * 选择率 sel：
 *   等值  = 1 / max(nrows, 1)
 *   范围  = 1/3        （无直方图的教科书默认值）
 *   其余  = 1.0        （不知道就按"全中"算，倾向全扫）
 *
 * 决策规则：
 *   nrows < CBO_SMALL_TABLE → 恒全扫（小表索引必亏，见下）
 *   否则 cost_idx < cost_seq 才走索引
 */

/* 小于这个行数就恒全扫 */
#define CBO_SMALL_TABLE 64

/* 回表惩罚系数：索引给出 RID 后还要回堆取行，比顺序扫贵 */
#define CBO_HEAP_FETCH 1.2

/* CostPlan 定义在 exec_internal.h（它要出现在 PushPlan 里，而 PushPlan 在那儿） */

/*
 * 一个谓词的选择率。
 *
 * op 用执行器内部的编码（'=' '<' '>' 'L' 'G'），与 Predicate.op 一致。
 */
double exec_cost_sel(int op, i64 nrows);

/*
 * 算两个代价并做决策。
 *
 * nrows 必须 > 0 —— 调用方拿不到行数时**不要调这个函数**
 * （虚拟表没有 catalog 行，它们的下推由模态自己负责）。
 */
void exec_cost_decide(i64 nrows, double sel, CostPlan *out);

/*
 * 估算 B+tree 的高度。
 *
 * 【为什么不问真实的树高】
 * 执行器**不认识 B+tree** —— 那是 SPI 下面的实现细节（分层纪律）。
 * 而树高对决策的影响很小：索引赢不赢主要看选择率，
 * 树高只贡献几个页访问的固定开销。
 *
 * 所以按"每页装多少条"反推：
 *   叶子约 170 条（4KB 页 / 24 字节复合键），每层扇出也按 170 估。
 * nrows=10 万 → 高度 3；nrows=1000 万 → 高度 4。
 *
 * （"实测树高缓存"列进 v1.2 —— 那时会顺带把 SPI 的批接口一起做。）
 */
double exec_cost_tree_height(i64 nrows);

#endif /* DB_QUERY_EXEC_COST_H */
