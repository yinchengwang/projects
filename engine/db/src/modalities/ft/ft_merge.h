#ifndef DB_MODALITIES_FT_MERGE_H
#define DB_MODALITIES_FT_MERGE_H

/*
 * ft_merge.h —— 段合并（内部接口，v1.3 步骤 3）
 *
 * 【合并是什么】
 *
 * 把若干个段合并成一个新段：
 *
 *   1. **字典多路归并**：各段的词表都是有序的 → k 路归并出全局有序的词表；
 *   2. **posting 归并**：同一个词在几个段里都有 posting →
 *      几段各自的 posting 都是按 doc_id 递增的 → 归并（**不是拼接**）；
 *   3. **重算 delta**：归并后的 doc_id 顺序变了，delta 必须重新算
 *      （坑 M4-2：只拼接不重算，解出来的 doc_id 全错，而且**不报错**）；
 *   4. **丢弃 tombstone 的文档**：被删的文档在这里**物理丢弃**；
 *   5. **重建段头**：新段的 doc_count / total_len / avgdl 都要重算。
 *
 * 【为什么新段要重算 doc_count 与 total_len】
 *
 * 被丢弃的文档不再计入新段的语料：
 *
 *   doc_count(新) = Σ doc_count(各段) - 被丢弃的**不同** doc_id 数
 *   total_len(新) = Σ total_len(各段) - 被丢弃文档的 dl 之和
 *
 * 注意是"**不同** doc_id 数"：一个文档有几个词就有几条 posting，
 * 按 posting 条数扣会重复扣。
 *
 * 【doc_id 在索引内必须唯一】
 *
 * 同一个 doc_id 出现在两个段里，意味着"这篇文档被索引了两次"。
 * 归并时它会变成两条 posting（分数会重复累加），与线性扫描对不上。
 * 所以合并时**检测到重复就报 E_CORRUPT**，而不是随便挑一条留下。
 */

#include "common/loghook.h"
#include "common/types.h"
#include "modalities/ft/ft_segment.h"

/* 合并的结果 */
typedef struct {
    u32 head_page;    /* 新段的段头页号 */
    u32 doc_count;    /* 新段的文档数 */
    u64 total_len;    /* 新段所有文档的词数之和 */
    u32 n_terms;      /* 新段的词数 */
    u32 max_doc_id;   /* 新段的最大 doc_id */
} FtMergeResult;

/*
 * 把 segs[0..n_segs) 合并成一个新段（新段直接写到 pg 里）。
 *
 * segs —— 要合并的段（**必须已经打开**，且它们的词表互不重叠地覆盖同一批词）。
 *
 * tomb_doc_ids / n_tombs —— 被删除的 doc_id 集合，
 *   **必须按 doc_id 升序**（合并时用二分判断成员，省一次哈希）。
 *   合并时命中的 posting 会被**物理丢弃**。
 *
 * new_seg_id —— 新段的段号。
 *
 * out —— 合并结果。
 *
 * 返回值：OK / E_INVAL / E_NOSPACE / E_CORRUPT。
 */
int ft_merge_segments(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                      FtSegment *const *segs, u32 n_segs,
                      const u32 *tomb_doc_ids, u32 n_tombs,
                      u32 new_seg_id, FtMergeResult *out);

#endif /* DB_MODALITIES_FT_MERGE_H */
