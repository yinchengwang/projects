#ifndef DB_MODALITIES_VECTOR_HNSW_AM_H
#define DB_MODALITIES_VECTOR_HNSW_AM_H

#include "spi/index_am.h"
#include "modalities/vector/hnsw.h"
#include "modalities/vector/vstore.h"

/*
 * hnsw_am —— HNSW 的 Index AM 适配（v1.0 步骤 5）
 *
 * 【knn 的第一个真实现】
 *
 * v0.9 定 IndexAM 的时候留了 `knn` 这个口子，B+tree 实现返回 E_NOTSUP。
 * 现在向量模态把它填上了 ——
 * **能力协商靠返回值，不靠类型判断**这条规矩终于有了两个不同的答案。
 *
 * 【为什么 IndexAM 在 modalities/vector/ 而不是 spi/】
 *
 * 和 graph_am / vector_am 同一个理由：
 * spi/（L3）不许依赖 modalities/（L4）。
 * HNSW 的实现在 modalities/vector/，所以适配器也得住在这儿。
 *
 * 【knn 返回什么】
 *
 *   out[]  每个命中的**行的 RID**（vec_items 的 RID 空间）
 *   dist[] 每个命中的距离（越小越近）
 *   n     实际返回几个
 *
 * RID 而不是 row_id：IndexAM 的契约是"加速结构指向表里的行"，
 * 和 B+tree 的 search 返回 RID 一个道理。
 * 调用方要 row_id 可以自己换（vstore_ordinal_of_page + 槽号）。
 *
 * 【本版没有 SQL 侧的 knn 入口】
 *
 * `ORDER BY vec <-> q LIMIT 10` 需要的排序键是**计算出来的**，
 * 不是列值。执行器要有这个就得加新的算子类型，
 * 违反"下推结果是行源"的同构性。
 *
 * 所以本版：knn 走 IndexAM 接口，但**调用方是公开 API**，
 * 不是 SQL。SQL 侧只提供 vec_items 的元数据查询。
 * 这是**有意的范围收缩，不是遗漏**。
 */

/* AM 的私有状态 */
typedef struct {
    VStore *vs;
    Hnsw   *h;
    u16     dim;

    /*
     * 查询期的 ef（候选池宽度）。
     *
     * 【为什么它在这里，而不是在 knn 的参数里】
     *
     * IndexAM 的 knn 签名是 v0.9 定稿的，没有 ef 这个旋钮 ——
     * 而 ef 是 HNSW 最核心的召回/延迟旋钮。**这是契约的缺口**。
     *
     * 现在的绕法：放在 ctx 里，由调用方在**持锁期间**设好再调。
     * 全局 latch 保证没有并发问题（v0.10 的硬规则 8）。
     *
     * 将来优化器要支持"按距离排序"时，这个缺口得正面解决：
     * 要么改契约（加参数），要么走专用路径（不进 IndexAM）。
     */
    u16     ef;
} HnswAmCtx;

/* 构造 HNSW 的 Index AM */
int hnsw_am_build(VStore *vs, Hnsw *h, u16 dim, IndexAM *out, HnswAmCtx *ctx);

#endif /* DB_MODALITIES_VECTOR_HNSW_AM_H */
