#ifndef DB_MODALITIES_FT_INDEX_H
#define DB_MODALITIES_FT_INDEX_H

/*
 * ft_index.h —— 全文检索的**索引**（多个段的集合）（v1.3 步骤 3）
 *
 * 一个段是不可变的；一个**索引**是若干个段 + 一份删除标记（tombstone）：
 *
 *   索引 = [段0, 段1, 段2, ...] + tombstone 集合
 *
 * 【为什么要分段】
 *
 * 段是不可变的，写入只追加 —— 所以索引是"追加 + 定期合并"的形态，
 * 这与 LSM 树是同一个思路：写入便宜，合并贵，但合并可以后台做。
 *
 * 【跨段的统计量怎么聚合】
 *
 * BM25 的 idf 用 N（语料文档数）和 df（词的文档数），它们是**全局量**。
 * 分段之后它们被摊在各段的段头里，检索时实时聚合：
 *
 *   N     = Σ 各段 doc_count
 *   df(t) = Σ 各段 df(t)         ← 一个词可能在好几段里都有
 *   avgdl = Σ 各段 total_len / N
 *
 * 段数 ≤ 阈值（FT_MERGE_THRESHOLD = 8）时，聚合代价可忽略。
 *
 * 【tombstone 的粒度】
 *
 * ⚠️ **与原计划 §1.3 不同**：原计划说"按 seg_id 组织的 doc_id 位图"，
 * 本版改成**一份全局的 doc_id 集合**。理由：
 *
 *   1. doc_id 是调用方给的，**不连续** —— 位图需要稠密的序号映射，
 *      而段的格式里没有"文档表"（见 ft_internal.h 关于 dl 的说明）。
 *   2. 删除是"删一篇文档"，调用方只知道 doc_id，不知道它在哪个段里；
 *      按段存就得先反查文档在哪个段（要么全扫、要么再维护一张表）。
 *   3. 合并时按全局集合过滤，语义更直白："这个 doc_id 被删了"。
 *
 * 代价：集合只增不减（删掉的文档在合并后其实可以清掉，
 * 但"它还在别的段里吗"不好判断）—— 见 MODALITIES.md 的如实记录。
 *
 * 【tombstone 对统计量的影响】
 *
 * 被删的文档**不进结果**，但**仍然计入 N 与 df**（直到合并把它物理丢掉）。
 * 这与 Lucene 的做法一致（删除的文档在合并前一直占着 docFreq）。
 * 后果：删除之后 idf 会略有偏差 —— 如实记录在案。
 */

#include "common/loghook.h"
#include "common/types.h"
#include "modalities/ft/ft_segment.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FtIndex FtIndex;

/* 段数超过这个阈值就触发合并（Tiered） */
#define FT_MERGE_THRESHOLD 8u

/* 新建一个空索引 */
int ft_index_create(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid, FtIndex **out);

/* 关闭索引（释放内存，不刷页、不关库） */
void ft_index_close(FtIndex *ix);

/*
 * 加一批文档 → **建一个新段**追加到索引（写入只追加，不改老段）。
 *
 * seg_id_out —— 新段的段号（段号在索引内唯一、递增）。
 */
int ft_index_add(FtIndex *ix, u32 xid, const FtDocText *docs, u32 n_docs, u32 *seg_id_out);

/*
 * 跨所有段检索（语义与单段检索、与线性扫描一致）：
 *
 *   - 只有命中至少一个 query 词的文档才进结果；
 *   - query 里的重复词不去重（M4-12）；
 *   - 排序：分数降序、分数相同按 doc_id 升序（M4-11）；
 *   - 被 tombstone 的 doc_id 不进结果；
 *   - top_k > max_out → E_NOSPACE（不静默截断）。
 */
int ft_index_search(const FtIndex *ix, const char *query, u32 query_len,
                    u32 top_k, u32 max_out, FtHit *out, u32 *n_out);

/* 同上，但用教科书式 TF-IDF 打分（对照组） */
int ft_index_search_tfidf(const FtIndex *ix, const char *query, u32 query_len,
                          u32 top_k, u32 max_out, FtHit *out, u32 *n_out);

/*
 * 【崩溃恢复】（v1.3 步骤 5）
 *
 * 从磁盘把索引重建出来：
 *   - 段目录：扫页空间，找到还活着的段并打开（死段不算数）；
 *   - tombstone 集合：页里的内容 ∪ (WAL 重放看到的 − WAL 回滚撤销的)。
 *
 * redo_ids / n_redo —— 恢复重放时看到的删除（由 db 层的恢复钩子收集）。
 * undo_ids / n_undo —— 恢复回滚时撤销的删除（未提交事务的）。
 *
 * 必须在**恢复重放之后**调 —— 否则 WAL 里的删除还没进集合。
 */
int ft_index_recover(FtIndex *ix, const u32 *redo_ids, u32 n_redo,
                     const u32 *undo_ids, u32 n_undo);

/*
 * 删一篇文档（打 tombstone）。
 *
 * 立刻生效：之后的检索不再返回它。
 * doc_id 不存在也**不报错**（删一个不存在的文档是合法的空操作）。
 */
int ft_index_delete(FtIndex *ix, u32 xid, u32 doc_id);

/*
 * 触发一次 Tiered 合并。
 *
 * 段数 ≤ FT_MERGE_THRESHOLD 时**什么都不做**，直接返回 OK。
 * 否则挑"文档数之和最小"的相邻若干段合并成一个新段，
 * 合并时：字典多路归并 + posting 归并（**重算 delta**）+ 丢弃 tombstone 的文档。
 *
 * merged_out —— 被合并掉的段数（0 表示没合并）。
 */
int ft_index_merge(FtIndex *ix, u32 xid, u32 *merged_out);

/* 统计 */
u32 ft_index_nsegs(const FtIndex *ix);
u32 ft_index_ndocs(const FtIndex *ix);        /* 各段 doc_count 之和（含 tombstone 的） */
u32 ft_index_ntombstones(const FtIndex *ix);
u64 ft_index_page_reads(const FtIndex *ix);

/*
 * 索引级**累计**的"合并页读"（v1.3 步骤 5 补）。
 *
 * 【为什么与 ft_index_page_reads 分开】
 *
 *   - ft_index_page_reads()      —— **检索**页读：A5 测"查一个词读了几页"，
 *                                  口径是各段计数的**差量**，与语料量无关；
 *   - ft_index_merge_page_reads() —— **合并**页读：A4 测"合并一次要读几页"。
 *
 * 两条是不同的 IO 曲线（合并读的页比检索多一个数量级），
 * 混在一起就看不出量级了 —— 所以必须是两个计数器。
 *
 * 【口径】
 *
 * 每次 ft_index_merge() 把这次合并读过的页数累加进来：
 *   - 读老段**倒排页**的那部分（各段 page_reads 的增量）；
 *   - 合并自己读的**字典页**（游标读的，不计入段的计数）。
 *
 * 为什么必须单独记：合并时被合并掉的段随后就被释放了，
 * 它们的 page_reads 事后读不到 —— 不在合并返回前把增量收上来，
 * 这次的合并代价就永远丢了。
 */
u64 ft_index_merge_page_reads(const FtIndex *ix);

/*
 * ---------------- 空闲页池（v1.3 欠账 2） ----------------
 *
 * 合并把旧段合掉之后，旧段的页没人引用了 —— 它们被记进"段级空闲页池"，
 * 以后建段/合并就优先从这里取页（磁盘文件不再随合并次数线性增长）。
 *
 * 这两个函数是**只读诊断**用的：
 *   - ft_index_free_pages()     —— 池里现在有多少空闲页；
 *   - ft_index_recycled_pages() —— 累计回收过多少页。
 *
 * ⚠️ 空闲页池是**内存态**的：进程重启后表是空的，
 * 那些"没人引用的页"就白占着位置（开机回收要等下一组的"段目录页"）。
 */
u32 ft_index_free_pages(const FtIndex *ix);
u64 ft_index_recycled_pages(const FtIndex *ix);

/*
 * ---------------- 只读访问器（v1.3 步骤 4 给 ft_docs 虚拟表用） ----------------
 *
 * 【为什么需要它们】
 *
 * ft_docs 这张虚拟表要**逐段**扫倒排链，才能把 (doc_id, doc_len, nterms)
 * 聚合出来（段的格式里没有"文档表"，见 ft_internal.h 关于 dl 的说明）。
 * 而段列表与 tombstone 集合都是索引的**私有**状态 ——
 * 没有只读访问器，模态就看不到它们。
 *
 * 这两个函数是**纯只读**的：不改索引的任何内容、不动页读统计。
 *
 * 【为什么不把整个结构体公开】
 *
 * 公开结构体 = 公开"怎么加字段、怎么排序"这些实现细节 ——
 * 改一次布局就要改所有使用者。所以只给两个函数。
 */

/* 取第 i 个段。i 越界 → E_NOTFOUND（不是错误，是"没有那么多段"）。 */
int ft_index_seg_at(const FtIndex *ix, u32 i, const FtSegment **out);

/* doc_id 是否已被删除（tombstone）。返回 1 = 已删，0 = 没删。 */
int ft_index_is_tomb(const FtIndex *ix, u32 doc_id);

/*
 * ==================== 根页（v1.3 欠账 3 / 7 的接线口） ====================
 *
 * 【为什么要这两个函数】
 *
 * 根页的页号存在**库的 meta 页**里（catalog 的两个通用槽位）。
 * 但 meta 页是 db 层的东西，而根页机制是 FT 层的东西 ——
 * 中间需要一个"谁存页号"的接缝：
 *
 *   - db 层读出 meta 页里的两个页号 → ft_index_set_root() 交给索引；
 *   - 索引建好根页之后 → ft_index_root_pages() 把页号报回给 db 层落盘。
 *
 * 这样 FT 层完全不需要知道 meta 页的存在（分层铁律：
 * modalities/ 不许碰 catalog/db 的内部布局）。
 */

/*
 * 绑定一对已有的根页（重开库时调）。
 *
 * 传 0（两个都传 0）表示"这个库还没有根页"（老库格式）：
 * 恢复时走"扫页空间"的回退路径，并在恢复之后把根页建出来。
 */
int ft_index_set_root(FtIndex *ix, u32 root_a, u32 root_b);

/* 取当前的根页页号（没建过 → 两个都填 0） */
int ft_index_root_pages(const FtIndex *ix, u32 *root_a, u32 *root_b);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_FT_INDEX_H */
