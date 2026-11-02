/*
 * vec_bench.h — 向量模态基准的公共部件（v1.0 步骤 6）
 *
 * 【为什么单独拆一个文件】
 *
 * bench/vec_bench.c 把「数据生成 + GT 计算 + 测量 + 公开 API 端到端」
 * 全塞在一起时已经 975 行 —— 超过本项目的 800 行红线。
 *
 * 红线的意义不是好看，是**一个文件塞太多东西之后，
 * 改一处会牵动太多处**。所以按"变化理由"拆：
 *
 *   - vec_bench_util.c  纯工具：随机数、样本统计、GT 计算
 *                        （这些东西改一次要改很久，且几乎不变）
 *   - vec_bench.c       测量流程（每次调整参数都会动）
 *
 * 公共的东西放头文件，**不共享状态** ——
 * 两个 .c 之间只靠函数调用传数据，没有全局可变变量。
 */

#ifndef DB_BENCH_VEC_BENCH_H
#define DB_BENCH_VEC_BENCH_H

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"

/* ---------------- 随机数（固定种子：结果可复现） ---------------- */

extern u64 g_rng;

u64    rng_next(void);
float  rng_f(void);

/* ---------------- 数据生成 ---------------- */

/* 均匀随机：每个分量独立 U(0,1) —— 图索引的最坏情况 */
void gen_uniform(float *v, u32 n, u16 dim);

/*
 * 聚簇：K 个中心 + 扰动 —— 接近真实 embedding 的形态。
 *
 * 【为什么拆成"造中心"和"绕中心生成"两步】
 *
 * 数据和查询**必须用同一批中心**，否则查询就落在分布外 ——
 * 那时 recall 量的是"能不能从数据分布外找到一个近邻"，
 * 而不是"能不能找到近邻"。真实场景里文档和查询用同一个 embedding 模型，
 * 分布是一样的。
 *
 * v1.0 步骤 6 第一版把两边分开造中心，A5 的 recall 一路从 0.98 掉到 0.72，
 * 一度以为增量插入有 bug —— 真相比是"查询在分布外"，
 * 而增量插入的图和批量建的图**逐位相同**（HNSW 插入是纯顺序的，
 * 停下来再插不改变任何一步）。这个结论是靠受控实验定下来的，
 * 不是猜的。
 */
void gen_centers(float *centers, u32 ncenters, u16 dim);
void gen_clustered_around(const float *centers, u32 ncenters,
                          float *v, u32 n, u16 dim);

/* ---------------- 计时 / 文件 ---------------- */

double now_us(void);

/* 文件字节数（不存在 → 0） */
u64 file_size(const char *path);

/* ---------------- 延迟样本 ---------------- */

typedef struct {
    double *us;
    u32     n;
    u32     cap;
} Samples;

void   samples_init(Samples *s, u32 cap);
void   samples_free(Samples *s);
double pct(Samples *s, double p);

/* ---------------- 精确 top-k（ground truth） ---------------- */

/*
 * 暴力扫的 ground truth。
 *
 * 【为什么要两份 GT】
 *
 *   - 内存版：从**原始数据数组**算 —— 独立于 vstore 和 hnsw
 *   - 存储版：从**存储读回来的数据**算 —— 验的是"存进去的和算出来的一样"
 *
 * 两份必须**给出同样的答案** —— 否则 recall 就是拿错的尺子量的。
 */
void topk_exact(const float *vecs, u32 n, u16 dim,
                const float *q, u32 k, u32 *out_ids, float *out_d);

/* ---------------- 数据落盘（给 numpy 交叉验证用） ---------------- */

/*
 * 写出一个二进制文件：维度 + 向量 + 查询。
 *
 * 【为什么要写文件】
 *
 * 因为 GT 要用**另一套实现**（numpy）算。
 * 两边必须看到**同一批数据**才谈得上对照 ——
 * 各生成各的随机数，比的就不是算法而是随机数生成器了。
 *
 * 格式（小端）：
 *   u32 dim
 *   u32 n_vec
 *   u32 n_query
 *   u32 k
 *   float data[(n_vec + n_query) * dim]
 */
int dump_data(const char *path, u16 dim, u32 nvec, u32 nq, u32 k,
              const float *vecs, const float *queries);

/*
 * 写出 C 侧算出来的结果：GT 的 row_id、ANN 的 row_id 和距离。
 *
 * 【为什么要把结果也导出】
 *
 * 因为交叉验证要验的不只是"GT 对不对"，还有"用这个 GT 算出来的 recall 对不对"。
 * 有了这一份文件，numpy 可以：
 *   1. 用自己的实现重算 GT，和 C 侧逐条比 —— 验"尺子"
 *   2. 用自己的 GT 去算 C 侧 ANN 结果的 recall —— 验"结论"
 *
 * 只导数据不导结果的话，numpy 只能验第 1 步；
 * 第 2 步（recall 的算法）还是只有 C 一份实现，等于没验。
 *
 * 格式（小端）：
 *   u32 n_query
 *   u32 k
 *   u32 ef            主测量用的 ef（对照时必须用同一个）
 *   f32 c_recall      主测量（那个 ef）的 recall
 *   u32 n_curve
 *   u32 curve_efs[n_curve]
 *   f32 curve_recalls[n_curve]
 *   u32 gt_ids[n_query * k]
 *   u32 ann_ids[n_query * k]
 *   float ann_dists[n_query * k]
 *
 * 【为什么要把整条 ef 曲线也导出】
 *
 * 因为 A3 的对照必须**在同一个 ef 上比**。
 * 第一版只导了一个 recall，对照程序不知道那是哪个 ef 的，
 * 结果拿 ef=512 的去和 ef=64 的比 —— 差 43 个点，报了个假的 FAIL。
 *
 * 而且曲线比单点更有说服力：它能显示两边的**形状**是不是一致
 * （同一套算法在不同 ef 下的表现趋势应该一样）。
 */
int dump_result(const char *path, u32 nq, u32 k, u16 ef, float c_recall,
                const u16 *curve_efs, const float *curve_recalls, u32 n_curve,
                const u32 *gt_ids, const u32 *ann_ids,
                const float *ann_dists);

/* ---------------- 一轮测量的结果 ---------------- */

typedef struct {
    double recall;
    u64    ann_pages;        /* ANN 查询期间的页读 */
    u64    scan_pages;       /* 一次暴力扫（读全部向量页）的页读 */
    u64    exact_pages;      /* nq 次暴力扫的总页读 */
    double p50, p90, p99, max;
    double load_ms;
    double load_per_sec;
    u32    gt_tie;           /* GT 交叉校验：id 不同但距离相同（并列，可接受） */
    u32    gt_bad;           /* GT 交叉校验：距离也不同（真错） */
    u64    index_bytes;      /* HNSW 索引常驻内存的字节数（估算） */
} Round;

#endif /* DB_BENCH_VEC_BENCH_H */
