#ifndef DB_MODALITIES_VECTOR_VECTOR_AM_H
#define DB_MODALITIES_VECTOR_VECTOR_AM_H

#include "spi/storage_am.h"
#include "modalities/vector/vector_state.h"
#include "modalities/vector/vstore.h"

/*
 * vector_am —— 向量模态的 SPI 适配（v1.0 步骤 5）
 *
 * 【为什么这个文件在 modalities/vector/ 而不是 spi/】
 *
 * 同一份理由见 graph_am.h：
 *   spi/ 是 L3，modalities/ 是 L4，spi/ **不许依赖** modalities/。
 *   向量的存储在 modalities/vector/，所以它的适配器必须跟着住在这儿。
 *   （blob_am 住在 spi/ 是因为 blob 的存储在 storage/（L1），
 *   spi/ → storage/ 是向下依赖，合法。）
 *
 * 【vec_items 在 SQL 里长什么样】
 *
 *   vec_items   一行 = 一个向量
 *               row_id / label / vec
 *
 * 三列：
 *   row_id  INT8  向量的全局序号（= 插入顺序，从 0 开始）
 *   label   INT4  标量属性（过滤用）
 *   vec     TEXT  向量本体 —— **外部值**，选中时才读
 *
 * 【vec 为什么要做成外部值】
 *
 * 128 维 fp32 = 512 字节，塞不进 SQL 的标量值（文本上限 255）。
 * 更重要的是**惰性**：只有真正 SELECT vec 的时候才读向量页。
 *
 * 于是 `SELECT row_id, label FROM vec_items`（10 万向量）
 * **一个向量页都不读** —— 只读标量表。
 * 这是列式布局在 SQL 层最直接的兑现。
 *
 * 【RID】
 *
 *   vec_items 的 RID = (数据页号, 槽号)
 *
 * 和 blob 同款思路：RID 是**行在存储里的位置**，不是行号。
 * row_id 是逻辑序号（= 列对齐的键），RID 是物理位置。两者都要。
 *
 * 【本版只读】
 *
 * 装载走专用 API（db_vec_insert），SQL 侧不给 INSERT/UPDATE/DELETE ——
 * 能力位里就没有那几个，执行器直接报"表不支持写"。
 *
 * 理由：向量的写入不只是"加一行" —— 它还要更新 HNSW 索引，
 * 而索引是**实例级共享**的结构。把它做成一个 SQL INSERT，
 * 就要在 SQL 层处理"插入向量 → 更新索引 → 事务边界"这一整套，
 * 而本版的写路径是语句级的、专用的。
 * 这和图模态"UPDATE 报 E_NOTSUP"是同一个判断：
 * **能力协商靠能力位，不靠函数指针为 NULL。**
 */

/* AM 的私有状态 */
typedef struct {
    VectorState *st;   /* 模态的实例状态（数据 + 索引） */
} VectorAmCtx;

/* 构造 vec_items 的 AM */
int vector_am_build_items(VectorState *st, StorageAM *out, VectorAmCtx *ctx);

#endif /* DB_MODALITIES_VECTOR_VECTOR_AM_H */
