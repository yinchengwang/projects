#ifndef DB_MODALITIES_VECTOR_HNSW_H
#define DB_MODALITIES_VECTOR_HNSW_H

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"

/*
 * hnsw —— 分层可导航小世界图（v1.0 步骤 3）
 *
 * 论文：Malkov & Yashunin, "Efficient and robust approximate nearest
 * neighbor search using Hierarchical Navigable Small World graphs" (2016)。
 * 参数与做法对齐 hnswlib（业界事实标准），这样 recall 才有可比性。
 *
 * 【一句话说清它是什么】
 *
 * 把向量当成图的节点，每个节点连 M 个"近邻"。
 * 第 0 层是完整的图（所有点都在），往上每层只留一小部分点（约 1/M），
 * 越往上越稀疏 —— 像一张"地图的缩放级别"。
 *
 * 查询时从最高层（最稀疏）开始，贪心地往近处走；
 * 走到走不动了就下沉一层，把当前位置当入口继续走。
 * 到了第 0 层再做一次 ef 宽的广度搜索。
 *
 * 为什么这样快：上层跳得远（每跳跨越很大一片区域），
 * 下层走得细（精确定位）。总跳数 ≈ log(N) 而不是 N。
 *
 * 【本版的实现边界】
 *
 * 只做**插入 + 查询**，不做删除（HNSW 的删除很难做对 ——
 * 删掉一个节点会让它的邻居失去"桥"，图会断）。
 * 删除留到 v1.2：标记删除 + 重建索引。
 *
 * 这一层是**纯内存**的：向量和邻接表都在堆上。
 * 落盘在步骤 4（邻接页序列化），本文件不碰 pager。
 * 这样"算法对不对"和"持久化对不对"可以分开验 ——
 * recall 不达标时不用怀疑是 IO 的问题。
 */

/*
 * 层数上限。
 *
 * 为什么是 5 而不是"随便给个 16"：
 *   期望的最大层 ≈ log_M(N)。M=16 时，
 *   1 万点 → 3.3 层，10 万点 → 4.15 层，100 万点 → 4.98 层。
 *   所以 5 已经覆盖到百万级；给 16 只是白占内存
 *   （每节点每层要 2 字节计数 + m 个 4 字节指针）。
 *
 * 这个值还有第二个身份：**它是磁盘格式的一部分**。
 * 落盘时每个节点的记录长度由它决定，所以内存和磁盘必须用同一个值 ——
 * 分成两个常量迟早会漂移，然后读出垃圾。
 */
#define HNSW_MAX_LEVEL 5

/* 每层邻居上限的硬约束（m0 = 2m，栈上临时数组按这个开） */
#define HNSW_MAX_M 32

typedef enum {
    HNSW_L2 = 0,   /* 欧氏距离（用平方距离比较，省一次 sqrt） */
    HNSW_IP = 1    /* 内积（距离 = 1 - dot；cosine 先归一化再用它） */
} HnswMetric;

typedef struct Hnsw Hnsw;

/* 查询结果。dist 越小越相似（IP 也按这个方向归一了） */
typedef struct {
    u32   id;
    float dist;
} HnswHit;

/*
 * 建索引。
 *
 *   dim    向量维度
 *   metric HNSW_L2 / HNSW_IP
 *   m      每层邻居上限（第 0 层是 2m）。hnswlib 默认 16
 *   efc    ef_construction。hnswlib 默认 200
 *   seed   随机种子 —— 层级分配用它。固定种子 = 可复现的索引
 *
 * efc 越大建得越慢、图质量越好。它是**构建期**的旋钮，
 * 查询期的旋钮是 hnsw_search 的 ef，两者别搞混。
 */
Hnsw *hnsw_create(u16 dim, HnswMetric metric, u16 m, u16 efc, u64 seed);
void  hnsw_destroy(Hnsw *h);

/*
 * 插入一个向量，返回它的 id（= 插入顺序，从 0 开始）。
 *
 * id 就是 row_id —— 上层（vstore）拿它去对应标量行和向量页。
 * 本函数不改 id，也不复用 id。
 */
int hnsw_add(Hnsw *h, const float *vec, u32 *id_out);

/*
 * 查 k 近邻。
 *
 * ef 是**候选池宽度**：ef 越大越准越慢，ef >= k 才有意义
 * （ef < k 时最多只能返回 ef 个）。hnswlib 默认 ef = 64。
 *
 * out 至少要能放 k 个。*n_out 返回实际找到几个（可能少于 k ——
 * 索引里的点比 k 少时）。结果按距离升序。
 */
int hnsw_search(Hnsw *h, const float *q, u32 k, u16 ef,
                HnswHit *out, u32 *n_out);

/*
 * 暴力扫（精确 k 近邻）—— 只用来做 ground truth。
 *
 * 【为什么它必须在生产代码里而不是只在测试里】
 *
 * 它是 recall 的**唯一可信基准**：recall@k = |ANN 结果 ∩ 真值| / k。
 * 把基准放在测试里，测试就成了唯一使用者；
 * 放在这里，验收（bench）和"需要精确结果的用户"共用同一份实现 ——
 * 而且它顺便就是 §1.5 说的 `db_vec_knn_exact`。
 *
 * O(n × dim)，10 万 × 128 维一次约 1.3 亿次乘加。
 */
int hnsw_bruteforce(const Hnsw *h, const float *q, u32 k,
                    HnswHit *out, u32 *n_out);

/* ---- 只读访问 ---- */

u32 hnsw_count(const Hnsw *h);
u16 hnsw_max_level(const Hnsw *h);
u32 hnsw_entry(const Hnsw *h);
u16 hnsw_level_of(const Hnsw *h, u32 id);
u16 hnsw_dim(const Hnsw *h);
u16 hnsw_m(const Hnsw *h);
const float *hnsw_vec(const Hnsw *h, u32 id);

/*
 * 读某个节点某层的邻居表（诊断用）。
 *
 * 【为什么需要它】
 *
 * recall 不达标时，第一件事是**看邻居表**：
 * 如果邻居表本身就是一堆远处点，那是修剪判据写错了；
 * 如果邻居表看着没问题但查询结果差，那是搜索路径的问题。
 *
 * 没有这个口子，只能靠 printf 钻进 hnsw.c —— 那是最难复用的一种调试。
 */
u32 hnsw_neighbors(const Hnsw *h, u32 id, u16 level, u32 *out, u32 cap);

/*
 * 最近一次搜索在第 0 层访问了多少个节点（诊断用）。
 *
 * 【为什么需要它】
 *
 * 上层（贪心下降）的作用是"把起点挪近查询，让第 0 层少做距离计算"。
 * 但这件事**在 recall 上几乎看不出来** —— 实测 1 万点：
 * 跳过上层，recall 从 0.999 变成 0.999。
 *
 * 也就是说：上层被关掉这种错误，用 recall 类断言抓不到
 * （步骤 7 的反向验证就是这么发现的）。
 * 只有量"做了多少距离计算"才看得见。
 */
u32 hnsw_last_visited(const Hnsw *h);

/*
 * 最近一次搜索在第 0 层的**实际起点**（诊断用）。
 *
 * 【为什么需要它】
 *
 * 上层（贪心下降）的作用是"把起点从入口点挪到更靠近查询的地方"。
 * 这个作用在**收益**上很难断言 —— 实测 5 万点：
 * 走上层与不走上层的 recall 几乎一样，工作量只差 0.5%。
 *
 * 但在**机制**上一眼可辨：起点变了没有。
 * 起点没变 = 上层是死代码 = 搜索退化成了单层贪心。
 */
u32 hnsw_last_entry(const Hnsw *h);

/*
 * 从**指定**起点做一次第 0 层搜索，返回访问的节点数（诊断用）。
 *
 * 【为什么需要它】
 *
 * 有了它才能对比"经过上层下降"和"不经过上层"的搜索成本：
 *
 *     u32 a = hnsw_last_visited(h);            正常路径（走上层）
 *     u32 b = hnsw_visited_from(h, q, hnsw_entry(h), ef);   不走上层
 *     a 应该明显小于 b —— 上层把起点挪近了
 *
 * 没有这个口子，"上层到底有没有起作用"就只能靠猜。
 *
 * 注意：它会**覆盖**上一次搜索的计数（和正常搜索共用同一份工作区）。
 */
u32 hnsw_visited_from(Hnsw *h, const float *q, u32 start, u16 ef);

#endif /* DB_MODALITIES_VECTOR_HNSW_H */
