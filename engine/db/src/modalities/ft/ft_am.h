#ifndef DB_MODALITIES_FT_AM_H
#define DB_MODALITIES_FT_AM_H

#include "spi/storage_am.h"
#include "modalities/ft/ft_index.h"

/*
 * ft_am —— 全文检索模态的 SPI 适配（v1.3 步骤 4）
 *
 * 【为什么这个文件在 modalities/ft/ 而不是 spi/】
 *
 * 计划稿写的是 spi/ft_am.{h,c}，但**分层门禁不允许**：
 * spi/ 是 L3，modalities/ 是 L4，L3 不许依赖 L4。
 * 所以适配器必须跟着模态住（与向量模态的 vector_am.c 同款做法）。
 *
 * 【ft_docs 在 SQL 里长什么样】
 *
 *   ft_docs   一行 = 一篇文档
 *             doc_id / doc_len / nterms / sealed
 *
 * 四列（全是标量列）：
 *   doc_id   INT8  文档标识（调用方给的，不连续）
 *   doc_len  INT4  文档的词数
 *   nterms   INT4  该文档命中的**不同**词数
 *   sealed   BOOL  该文档是否已被 tombstone（已打删除标记）
 *
 * 【为什么要"现算"而不是存一张文档表】
 *
 * 段的物理格式里**没有文档表**（见 ft_internal.h 关于 dl 的说明）：
 * 文档长度 dl 跟着 posting 走。所以 ft_docs 的行只能从倒排链里**现算**：
 *
 *   逐段 → 逐词（ft_segment_scan_terms）→ 逐 posting（ft_segment_postings）
 *   → 按 doc_id 聚合出 (doc_len = posting 的 dl, nterms = 命中次数)
 *
 * 内存开销 O(文档数)。这是**元数据路径**，不是热路径 ——
 * 真正的检索走 db_ft_search（专用 API），不在 SQL 里做相关性排序。
 *
 * 【本版只读】
 *
 * 能力位里没有 INSERT / UPDATE / REMOVE / VACUUM：
 * 文档的写入走专用 API（db_ft_add），SQL 侧只读。
 * 理由与向量模态同款：写入不只是"加一行"，它还要建段（写页）。
 */

/* AM 的私有状态 */
typedef struct {
    FtIndex *ix;   /* 索引（实例级共享：所有连接看同一批文档） */
} FtAmCtx;

/* 构造 ft_docs 的 AM */
int ft_am_build_docs(FtIndex *ix, StorageAM *out, FtAmCtx *ctx);

#endif /* DB_MODALITIES_FT_AM_H */
