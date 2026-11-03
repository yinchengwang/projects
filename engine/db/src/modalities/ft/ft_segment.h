#ifndef DB_MODALITIES_FT_SEGMENT_H
#define DB_MODALITIES_FT_SEGMENT_H

/*
 * ft_segment.h —— 全文检索的"段"（segment）（v1.3 步骤 2）
 *
 * 一个段 = 一批文档构建出来的**不可变**倒排索引：
 *
 *   段头页（1 页）+ 字典页（若干）+ 倒排页（若干）
 *
 * 本版（步骤 2）只做**单段**：建一个段、在这个段里检索。
 * 多段、删除标记、合并是步骤 3 的事。
 *
 * 【为什么不把段做成"隐式的"】
 *
 * 段是磁盘上的物理结构，它的边界（哪些页属于哪个段）必须是显式的。
 * 本版把段的清单放在内存里（调用方自己拿 head_page 存着），
 * 落盘的段目录留到步骤 4/5（那时才需要"重开库还能找到段"）。
 */

#include "common/loghook.h"
#include "common/types.h"
#include "modalities/ft/bm25.h"
#include "storage/pager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 段的内存表示：**只有段头**在内存里，
 * 字典页和倒排页都是按需从页里读的（这是 A5 "IO 可证"的前提）。
 */
typedef struct {
    Pager *pg;
    u32    head_page;
    u32    seg_id;
    u32    doc_count;      /* N */
    u64    total_len;      /* 所有文档词数之和 */
    u32    max_doc_id;
    u32    n_terms;
    u32    dict_start;
    u32    dict_count;
    u32    post_start;
    u32    post_count;
    u64    page_reads;     /* 检索引起的页读次数（诊断用，A5 的度量） */
} FtSegment;

/* 平均文档长度（词数）。avgdl = 0 的坑见 bm25.h 的 M4-9。 */
static inline double ft_segment_avgdl(const FtSegment *seg)
{
    if (seg->doc_count == 0)
        return 0.0;
    return (double)seg->total_len / (double)seg->doc_count;
}

/*
 * 一条 posting（内存形态）：绝对 doc_id + 该词的 tf + 文档长度（词数）。
 *
 * 【为什么内存里的 posting 是绝对 doc_id，磁盘上却是 delta】
 *
 * delta 编码是**存盘时**的事（省空间），而且它要求 posting 按 doc_id 递增。
 * 上层（建段、合并）手里的 posting 都是绝对 doc_id —— 合并时要把两个段的
 * posting **归并**（不是拼接），归并完才能算 delta。
 * 所以"绝对 → delta"这一步必须由写页的那一层做（ft_writer.c）。
 */
typedef struct {
    u32 doc_id;
    u32 tf;
    u32 dl;
} FtPosting;

/*
 * 建一个段：把一批文档建成不可变的倒排索引。
 *
 * pg / wal / wal_ctx / xid —— 段要占页，页的改动必须进 WAL
 *   （用 LOG_PAGEDATA 整页镜像，和向量模态的做法一致）。
 *   wal 为 NULL 时不写日志（单测用）。
 *
 * docs / n_docs —— 语料。**doc_id 必须互不相同**
 *   （同 id 的两篇文档在段里会合并成一条 posting，
 *   而线性扫描那边还是两条 —— A1 的对照就不成立了）。
 *
 * seg_id —— 段号，由调用方给（步骤 3 的段目录要按它排序）。
 *
 * *head_page_out —— 返回段头页号（段的唯一标识）。
 *
 * 返回值：OK / E_INVAL / E_NOSPACE / E_CORRUPT。
 */
int ft_segment_build(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                     const FtDocText *docs, u32 n_docs, u32 seg_id,
                     u32 *head_page_out);

/*
 * 打开一个已存在的段（读段头页）。
 *
 * 段头页的类型或版本不对 → E_CORRUPT（宁可打不开，也不要跑出错误的答案）。
 */
int ft_segment_open(Pager *pg, u32 head_page, FtSegment **out);

/* 释放段的内存表示（不刷页、不关库） */
void ft_segment_close(FtSegment *seg);

/*
 * 在一个段里检索。
 *
 * query / query_len —— 查询串，内部用分词器切词
 *   （与线性扫描那边用同一个分词器，语义逐位一致）。
 *
 * top_k —— 要返回多少条。
 * max_out —— out 的容量。**top_k > max_out 直接报 E_NOSPACE**，
 *   不做静默截断（截断出来的结果"看起来对其实错"，比报错坏得多）。
 *
 * 语义（与 bm25.h 的线性扫描完全一致）：
 *   - 至少命中一个 query 词的文档才进结果；
 *   - query 里的重复词**不去重**，各贡献一次（M4-12）；
 *   - 排序：分数降序、分数相同按 doc_id 升序（M4-11）。
 */
int ft_segment_search(const FtSegment *seg, const char *query, u32 query_len,
                      u32 top_k, u32 max_out, FtHit *out, u32 *n_out);

/* 同上，但用教科书式 TF-IDF 打分（对照组，验收 A2 用） */
int ft_segment_search_tfidf(const FtSegment *seg, const char *query, u32 query_len,
                            u32 top_k, u32 max_out, FtHit *out, u32 *n_out);

/* 检索引起的页读次数（A5 的度量：单 term 检索的页读应与文档量无关） */
u64 ft_segment_page_reads(const FtSegment *seg);

/* 段的词数 */
u32 ft_segment_nterms(const FtSegment *seg);

/*
 * 查字典：找 term 的 df 与倒排链头（跨段统计量靠它聚合）。
 *
 * 找不到返回 E_NOTFOUND（词不在这个段里 —— 这是**正常结果**，不是错误）。
 */
int ft_segment_lookup(const FtSegment *seg, const char *term, u32 term_len,
                      u32 *df_out, u64 *post_ptr_out);

/*
 * 按字典序遍历段里的**全部词条**（合并时用）。
 *
 * 回调收到：词、词长、df、倒排链头。
 * 回调返回非 OK → 立刻停止遍历，并把该值原样返回。
 */
typedef int (*FtTermFn)(void *ctx, const char *term, u32 term_len, u32 df, u64 post_ptr);
int ft_segment_scan_terms(const FtSegment *seg, FtTermFn fn, void *ctx);

/*
 * 读一个词的**全部 posting**（合并时用）。
 *
 * post_ptr / df —— 字典词条里的倒排链头与 posting 条数。
 * out / max_out —— 输出缓冲与容量；装不下返回 E_NOSPACE（**不静默截断**）。
 * n_out —— 实际读出的条数。
 *
 * 读出来的 posting 按 doc_id 递增（格式保证）。
 */
int ft_segment_postings(const FtSegment *seg, u64 post_ptr, u32 df,
                        FtPosting *out, u32 max_out, u32 *n_out);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_FT_SEGMENT_H */
