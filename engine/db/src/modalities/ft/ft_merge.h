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
    /*
     * 合并路径读过的**字典页**数。
     *
     * 读老段的倒排页已经记在那些段的 page_reads 里（ft_scan.c 的规矩），
     * 而字典页是合并的游标自己读的（不在段的计数里）——
     * 所以这一项补上"合并读字典"的那半边，两半加起来才是合并的总页读。
     */
    u64 page_reads;
} FtMergeResult;

/*
 * 挑合并窗口（合并策略）。
 *
 * 在所有长度为 2..n_segs 的**相邻窗口**里，挑"文档数之和最小"的那个；
 * 并列时的取法是确定的（见 ft_merge.c 的注释）。
 */
void ft_merge_pick_window(u32 n_segs, FtSegment *const *segs,
                           u32 *start_out, u32 *width_out);

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
/*
 * ==================== 合并与"旧段失效"的时序（v1.3 欠账 2） ====================
 *
 * 【为什么时序要写在这里】
 *
 * 合并要把旧段的页**回收**（还给空闲页池），而"旧段什么时候算死"
 * 是一件不能含糊的事 —— 含糊了就等于把还在用的页发出去，
 * 老段的数据被新段踩掉，而且**不报错**（读出来像是对的）。
 *
 * 完整时序（ft_index.c 的 ft_index_merge 负责推进）：
 *
 *   1. 标 MERGING（老段指向新段号）—— 必须在写新段**之前**（坑 M4-21）；
 *   2. 写新段（新段的页可以来自空闲页池）；
 *   3. 标 DEAD（老段算死，磁盘状态与恢复时的判定一致）；
 *   4. **回收老段的页** —— 把老段的页区间登记进空闲页池。
 *
 * 【为什么第 4 步必须在第 3 步之后】
 *
 * 第 3 步之前，老段还**可能被回滚**：合并失败时
 * ft_catalog_abort_merge 把老段改回 LIVE，老段必须还能用。
 * 如果第 4 步提前了，那些页就被发给了新段 —— 老段的数据没了。
 *
 * 【合并失败时新段的页怎么办】
 *
 * **不回收**（只浪费，不污染）：
 *
 *   - 新段的页在失败时可能已经写了一部分，是**半成品**；
 *   - 半成品页不是"干净的空闲页"（页头、run 链都是断的），
 *     直接发给别的段就等于把垃圾当成好页用；
 *   - 所以失败时**不把新段的页交给空闲页池** ——
 *     它们还在文件里，但没人引用（浪费空间，不影响正确性）。
 *     这与"旧段的页不回收"是同一类欠账，
 *     等下一组的"开机回收"（段目录页）一起解决。
 *
 * 【回滚安全的底线】
 *
 *   - 失败 → 老段仍然是 LIVE、内容不变（ft_catalog_abort_merge）；
 *   - 失败 → 空闲页池**一个页都不许多**（老段的页不能提前进池）；
 *   - 成功 → 老段的页一次性进池，且此后不再被任何段引用。
 */
int ft_merge_segments(Pager *pg, FtPagePool *pool,
                      LogHookFn wal, void *wal_ctx, u32 xid,
                      FtSegment *const *segs, u32 n_segs,
                      const u32 *tomb_doc_ids, u32 n_tombs,
                      u32 new_seg_id, FtMergeResult *out);


#endif /* DB_MODALITIES_FT_MERGE_H */
