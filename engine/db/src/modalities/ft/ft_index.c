/*
 * ft_index.c —— 全文检索模态的**索引**（多段 + 删除标记 + 合并触发）（v1.3 步骤 3）
 *
 * 段（segment）是不可变的：写进去就再也不改。"一个索引里有很多篇文档"靠的是
 * **多个段**（索引 = [段0, 段1, ...] + tombstone 集合）—— 与 LSM 树同理：写入便宜
 * （只追加一个段），合并贵，但可以延后做。本文件：建段、跨段检索、tombstone、合并。
 *
 * BM25 的 idf 依赖语料级的 df（含有该词的**文档数**），而 df 要等所有段都查过
 * 字典才知道。所以跨段检索只能两遍：第一遍，每个 query 词 × 每个段 → 查字典，
 * 拿该段的 df 与倒排链头，把各段的 df **加起来**得全局 df；第二遍，每个 query 词
 * × 每个段 → 读 posting，用**全局**的 df / N / avgdl 打分。
 *
 * ⚠️ 打分**必须**用全局统计量：误用"每个段自己的 df / N / avgdl"，同一篇文档
 * 在不同段里拿到的分数不可比，排序会乱，也与线性扫描对不上。
 *
 * 与线性扫描逐位一致的三个要点：
 *   1. 统计量：N = Σ 各段 doc_count；df(t) = Σ 各段 df(t)；avgdl = Σ total_len / N。
 *   2. 累加顺序：按 query 词**出现的顺序**累加（浮点加法不满足结合律）；一篇文档
 *      只会落在一个段里（索引内 doc_id 唯一），两边顺序一致。
 *   3. 排序：分数降序、分数相同按 doc_id 升序（坑 M4-11）。
 *
 * tombstone 是一份**全局的 doc_id 集合**（按升序维护，理由见 ft_index.h）：被删的
 * 文档**不进结果**，但**仍计入 N 与 df**（直到合并物理丢掉），与 Lucene 一致；
 * 后果是删除后 idf 略有偏差 —— 如实记录在案。集合只增不减。
 *
 * 合并是 Tiered：段数超过阈值时，在所有长度为 2..n 的**相邻窗口**里挑"文档数之和
 * 最小"的那个合成一个新段（代价正比于参与的文档数）。
 * ⚠️ **旧段的页不做回收** —— 本版没有 ft 自己的 freelist，这是已知欠账。
 */

#include "modalities/ft/ft_index.h"

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "modalities/ft/ft_index_internal.h"
#include "modalities/ft/ft_internal.h"
#include "modalities/ft/ft_merge.h"
#include "modalities/ft/tokenizer.h"

/* 段数组 / tombstone 数组的初始容量（按 2 的幂增长） */
#define FT_IDX_INIT_SEGS  4u
#define FT_IDX_INIT_TOMBS 16u

/*
 * 各段页读计数之和 —— **瞬时值**，不是索引的累计值（坑 M4-16）。
 *
 * 段里的 page_reads 是该段自打开以来的累计值；把各段加起来只是"此刻所有段
 * 一共读过多少页"，一旦合并把老段释放掉，这个和就会**缩水**。
 */
static u64 ft_idx_seg_page_reads(const FtIndex *ix)
{
    u64 total = 0;

    for (u32 s = 0; s < ix->n_segs; s++) {
        total += ix->segs[s]->page_reads;
    }
    return total;
}
/* ---------------- 分数累加表（开放寻址哈希） ---------------- */

/*
 * (doc_id → 累计分数) 的开放寻址哈希表。
 *
 * 命中文档的 doc_id 由调用方给、不连续，数量事先未知：用数组浪费，用线性查找
 * 就是 O(n²)（十万文档直接卡死）。容量取 2 的幂，装填因子 <= 0.5，Knuth 乘法散列。
 *
 * 【doc_id 可以是任意值】—— 不能拿 doc_id == 0 当"空槽"标记，故另设 used 标志。
 */
typedef struct {
    u32    key;
    double val;
    u8     used;
} FtIdxSlot;

typedef struct {
    FtIdxSlot *slots;
    u32        cap;      /* 槽数（2 的幂） */
    u32        count;    /* 已占槽数 */
} FtIdxScoreMap;

static u32 ft_idx_map_hash(u32 doc_id)
{
    return doc_id * 2654435761u;   /* Knuth 乘法散列 */
}
static void ft_idx_map_init(FtIdxScoreMap *m)
{
    memset(m, 0, sizeof(*m));
}
static void ft_idx_map_free(FtIdxScoreMap *m)
{
    free(m->slots);
    memset(m, 0, sizeof(*m));
}
/* 容量翻倍并把老条目重插进去 */
static int ft_idx_map_grow(FtIdxScoreMap *m)
{
    u32 ncap = m->cap * 2;
    FtIdxSlot *ns = (FtIdxSlot *)calloc(ncap, sizeof(FtIdxSlot));
    u32 i;

    if (!ns) {
        return E_NOSPACE;
    }
    for (i = 0; i < m->cap; i++) {
        u32 j;

        if (!m->slots[i].used) {
            continue;
        }
        j = ft_idx_map_hash(m->slots[i].key) & (ncap - 1);
        while (ns[j].used) {
            j = (j + 1) & (ncap - 1);
        }
        ns[j] = m->slots[i];
    }
    free(m->slots);
    m->slots = ns;
    m->cap = ncap;
    return OK;
}
/* 给 doc_id 累加一分。成功返回 0，内存不足返回 -1 */
static int ft_idx_map_add(FtIdxScoreMap *m, u32 doc_id, double score)
{
    u32 i;

    if (m->cap == 0) {
        m->slots = (FtIdxSlot *)calloc(64, sizeof(FtIdxSlot));
        if (!m->slots) {
            return -1;
        }
        m->cap = 64;
    }
    /* 装填因子 <= 0.5：满了先翻倍 */
    if ((u64)m->count * 2 >= (u64)m->cap) {
        if (ft_idx_map_grow(m) != OK) {
            return -1;
        }
    }
    i = ft_idx_map_hash(doc_id) & (m->cap - 1);
    for (;;) {
        if (!m->slots[i].used) {
            m->slots[i].used = 1;
            m->slots[i].key = doc_id;
            m->slots[i].val = score;
            m->count++;
            return 0;
        }
        if (m->slots[i].key == doc_id) {
            m->slots[i].val += score;   /* 多个 query 词命中同一篇 → 分数累加 */
            return 0;
        }
        i = (i + 1) & (m->cap - 1);
    }
}
/* ---------------- 结果排序 ---------------- */

/*
 * 分数降序、分数相同按 doc_id 升序（坑 M4-11）。
 * 不用 epsilon：浮点相等必须用 == 判，否则同分的两条会被排成不同序。
 */
static int ft_idx_hit_cmp(const void *a, const void *b)
{
    const FtHit *x = (const FtHit *)a;
    const FtHit *y = (const FtHit *)b;

    if (x->score != y->score) {
        return (x->score > y->score) ? -1 : 1;
    }
    if (x->doc_id != y->doc_id) {
        return (x->doc_id < y->doc_id) ? -1 : 1;
    }
    return 0;
}
/* ---------------- 切查询词 ---------------- */

/*
 * 切一次查询串，词表放在 *buf 里（不够就自动长）。
 *
 * 两遍法：先用一块固定大小的缓冲试切，装不下就按"需要的词数"扩容重来 —— 分词器
 * 对"装不下"的约定是"一个词都不写、只报需要的数量"，所以重来是安全的。
 * 与 bm25.c / ft_search.c 同一套写法（分词器共用，切法必然一致）。
 */
static int ft_idx_collect_tokens(const char *text, u32 text_len, FtToken **buf, u32 *cap, u32 *n_out)
{
    u32 need = 0;
    int rc;

    if (*buf == NULL) {
        *cap = 16;
        *buf = (FtToken *)malloc(sizeof(FtToken) * (size_t)*cap);
        if (*buf == NULL) {
            return E_NOSPACE;
        }
    }
    rc = ft_tokenize(text, text_len, *buf, *cap, &need);
    if (rc == E_NOSPACE) {
        FtToken *nb = (FtToken *)realloc(*buf, sizeof(FtToken) * (size_t)(need ? need : 1));
        if (nb == NULL) {
            return E_NOSPACE;
        }
        *buf = nb;
        *cap = need;
        rc = ft_tokenize(text, text_len, *buf, *cap, &need);
    }
    if (rc != OK) {
        return rc;
    }
    *n_out = need;
    return OK;
}
/* ---------------- tombstone 集合 ---------------- */

/*
 * 二分找 doc_id 在升序集合里的位置（返回第一个 >= doc_id 的下标，也就是插入点）。
 *
 * ft_merge.h 要求传过去的 tomb 集合**必须升序**（那边用二分判断成员）。
 */
static u32 ft_idx_tomb_lb(const u32 *tomb, u32 n, u32 doc_id)
{
    u32 lo = 0;
    u32 hi = n;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;

        if (tomb[mid] < doc_id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}
/* doc_id 是否已被删除 */
static bool ft_idx_is_tomb(const FtIndex *ix, u32 doc_id)
{
    u32 pos = ft_idx_tomb_lb(ix->tomb, ix->n_tombs, doc_id);

    return (pos < ix->n_tombs && ix->tomb[pos] == doc_id);
}
/* ---------------- 段数组 ---------------- */

/* 追加一个段（段数组按 2 的幂增长） */
static int ft_idx_push_seg(FtIndex *ix, FtSegment *seg)
{
    if (ix->n_segs == ix->cap_segs) {
        u32 ncap = (ix->cap_segs == 0) ? FT_IDX_INIT_SEGS : ix->cap_segs * 2;
        FtSegment **ns = (FtSegment **)realloc(ix->segs, sizeof(FtSegment *) * (size_t)ncap);

        if (ns == NULL) {
            return E_NOSPACE;
        }
        ix->segs = ns;
        ix->cap_segs = ncap;
    }
    ix->segs[ix->n_segs] = seg;
    ix->n_segs++;
    return OK;
}
/* ---------------- 全局统计量 ---------------- */

/*
 * 语料级的 N 与 avgdl：N = Σ 各段 doc_count，avgdl = Σ 各段 total_len / N。
 *
 * 【为什么先加总再除，而不是各段 avgdl 求平均】
 *
 * 各段 avgdl 的平均 ≠ 全局 avgdl（段的大小不一样时差别很大），
 * 而且"先除后加"还会多引入几次舍入 —— 与线性扫描对不上。
 */
static void ft_idx_corpus_stats(const FtIndex *ix, u32 *n_docs, double *avgdl)
{
    u32 n = 0;
    u64 total = 0;
    u32 i;

    for (i = 0; i < ix->n_segs; i++) {
        n += ix->segs[i]->doc_count;
        total += ix->segs[i]->total_len;
    }
    *n_docs = n;
    *avgdl = (n > 0) ? ((double)total / (double)n) : 0.0;
}
/* ---------------- 检索主流程 ---------------- */

/*
 * 跨段检索的主流程。use_bm25 != 0 走 BM25，否则走 TF-IDF。
 *
 * 两个公开入口只是这个函数的两个壳 —— 语义必须完全一样，所以只留一个实现。
 */
static int ft_index_search_impl(const FtIndex *ix, const char *query, u32 query_len,
                                u32 top_k, u32 max_out, FtHit *out, u32 *n_out, int use_bm25)
{
    FtToken *qbuf = NULL;
    u32  qcap = 0;
    u32  nq = 0;
    u32 *cache_df = NULL;      /* nq × n_segs：该段里这个词的 df（0 = 这个段没有） */
    u64 *cache_ptr = NULL;     /* nq × n_segs：该段里这个词的倒排链头 */
    u32 *gdf = NULL;           /* nq：全局 df */
    FtPosting *pbuf = NULL;    /* 读 posting 的缓冲 */
    u32  pcap = 0;
    FtIdxScoreMap map;
    FtCorpus cs;
    FtHit *hits = NULL;
    u32  n = 0;
    u32  k = 0;
    u64  pr_before = 0;     /* 本次检索前的各段页读之和（取差量用） */
    size_t slots;
    int rc = OK;

    /* 累加表先建好：出错路径上统一在 done 处释放（free(NULL) 是安全的） */
    ft_idx_map_init(&map);

    /* ---- 参数检查（顺序与段检索、线性扫描完全一致） ---- */
    if (ix == NULL || n_out == NULL) {
        return E_INVAL;
    }
    /* 出错路径上 *n_out 一律是 0：调用方不能拿到"半截结果" */
    *n_out = 0;
    /* 页读基准点：必须在任何页读之前取，后面用差量累加（坑 M4-16） */
    pr_before = ft_idx_seg_page_reads(ix);

    if (out == NULL) {
        /*
         * 没有结果缓冲区：top_k 非 0 就是没地方放结果 → E_NOSPACE；
         * top_k 为 0（不要结果）时这个参数本身就没意义 → E_INVAL。
         */
        return (top_k > 0) ? E_NOSPACE : E_INVAL;
    }
    if (top_k > max_out) {
        /*
         * 严格口径：**top_k > max_out 一律报错**，不看实际命中几条。
         * 三个 API（线性扫描 / 段检索 / 索引检索）必须行为一致，A1 的对照才成立；
         * 静默截断出来的结果"看起来对其实错"，比报错坏得多。
         */
        return E_NOSPACE;
    }
    /* ---- 全局语料统计量 ---- */
    ft_idx_corpus_stats(ix, &cs.n_docs, &cs.avgdl);

    /* 语料为空 / 不要结果 → 没有可返回的东西 */
    if (cs.n_docs == 0 || top_k == 0) {
        return OK;
    }
    /* ---- 切查询词 ---- */
    rc = ft_idx_collect_tokens(query, query_len, &qbuf, &qcap, &nq);
    if (rc != OK) {
        goto done;
    }
    if (nq == 0) {
        rc = OK;   /* 查询串里没有词 → 没有命中 */
        goto done;
    }
    /* ---- 缓存布局：nq × n_segs ---- */
    if ((size_t)nq > ((size_t)-1) / (size_t)ix->n_segs) {
        rc = E_NOSPACE;
        goto done;
    }
    slots = (size_t)nq * (size_t)ix->n_segs;
    cache_df = (u32 *)calloc(slots, sizeof(u32));
    cache_ptr = (u64 *)calloc(slots, sizeof(u64));
    gdf = (u32 *)calloc((size_t)nq, sizeof(u32));
    if (!cache_df || !cache_ptr || !gdf) {
        rc = E_NOSPACE;
        goto done;
    }
    /*
     * ---- 第一遍：查字典，聚合全局 df ----
     *
     * 每个 query 词在**每个段**里查一次字典：这个段没有这个词 → df = 0
     * （不是错误，E_NOTFOUND 是正常结果）；有 → 该段的 df 计入全局 df，
     * 倒排链头缓存下来（第二遍直接用）。
     * 坑 M4-12：query 里的重复词**不去重** —— 每个位置各贡献一次分数。
     */
    for (u32 q = 0; q < nq; q++) {
        u32 sum = 0;

        for (u32 s = 0; s < ix->n_segs; s++) {
            u32 df = 0;
            u64 ptr = FT_PTR_NIL;
            size_t idx = (size_t)q * (size_t)ix->n_segs + (size_t)s;

            rc = ft_segment_lookup(ix->segs[s], qbuf[q].text, qbuf[q].len, &df, &ptr);
            if (rc == E_NOTFOUND) {
                rc = OK;   /* 这个段里没这个词：df = 0，不算错 */
                df = 0;
                ptr = FT_PTR_NIL;
            } else if (rc != OK) {
                goto done;
            }
            cache_df[idx] = df;
            cache_ptr[idx] = ptr;
            sum += df;
        }
        gdf[q] = sum;
    }
    /*
     * ---- 第二遍：读 posting，用**全局**统计量打分 ----
     *
     * 遍历顺序是"query 词在外、段在内" —— 同一篇文档的分数累加顺序就是
     * "按 query 词出现的顺序"，与线性扫描一致（浮点加法不满足结合律）。
     */
    for (u32 q = 0; q < nq; q++) {
        u32 g = gdf[q];

        for (u32 s = 0; s < ix->n_segs; s++) {
            size_t idx = (size_t)q * (size_t)ix->n_segs + (size_t)s;
            u32 df = cache_df[idx];
            u64 ptr = cache_ptr[idx];
            u32 np = 0;

            if (df == 0 || ptr == FT_PTR_NIL) {
                continue;   /* 这个段里没有这个词 */
            }
            /* posting 缓冲：装下这个段里这个词的全部 posting */
            if (df > pcap) {
                FtPosting *nb = (FtPosting *)realloc(pbuf, sizeof(FtPosting) * (size_t)df);
                if (nb == NULL) {
                    rc = E_NOSPACE;
                    goto done;
                }
                pbuf = nb;
                pcap = df;
            }
            rc = ft_segment_postings(ix->segs[s], ptr, df, pbuf, pcap, &np);
            if (rc != OK) {
                goto done;
            }
            for (u32 i = 0; i < np; i++) {
                const FtPosting *p = &pbuf[i];
                double score;

                /* 被删的文档不进结果（但它仍然计入 N 与 df） */
                if (ft_idx_is_tomb(ix, p->doc_id)) {
                    continue;
                }
                if (use_bm25) {
                    score = ft_bm25_score(&cs, g, p->tf, p->dl);
                } else {
                    score = ft_tfidf_score(&cs, g, p->tf);
                }
                if (ft_idx_map_add(&map, p->doc_id, score) != 0) {
                    rc = E_NOSPACE;
                    goto done;
                }
            }
        }
    }
    /* ---- 收集结果并排序 ---- */
    n = map.count;
    if (n > 0) {
        u32 i = 0;

        hits = (FtHit *)malloc(sizeof(FtHit) * (size_t)n);
        if (hits == NULL) {
            rc = E_NOSPACE;
            goto done;
        }
        for (u32 s = 0; s < map.cap; s++) {
            if (!map.slots[s].used) {
                continue;
            }
            hits[i].doc_id = map.slots[s].key;
            hits[i].score = map.slots[s].val;
            i++;
        }
        qsort(hits, n, sizeof(FtHit), ft_idx_hit_cmp);
    }
    /* ---- 取前 top_k 条（k <= top_k <= max_out，装得下） ---- */
    k = (n < top_k) ? n : top_k;
    if (k > 0) {
        memcpy(out, hits, sizeof(FtHit) * (size_t)k);
    }
    *n_out = k;
    rc = OK;

done:
    ft_idx_map_free(&map);
    /*
     * 页读统计：索引级**累计**（坑 M4-16）。
     *
     * 不能用"各段页读之和"直接赋值：合并会把老段释放掉，这个和会**缩水**，
     * 累计值反而变小。正确做法是本次检索的**差量**（after - before）累加上去。
     *
     * ix 是 const —— 但 page_reads_total 只是个**统计字段**，我们只改它，
     * 不改索引的任何内容（段是只读的，页也不会被标脏）。
     */
    {
        FtIndex *m = (FtIndex *)ix;   /* 只改统计字段 */

        m->page_reads_total += (ft_idx_seg_page_reads(ix) - pr_before);
    }
    free(qbuf);
    free(cache_df);
    free(cache_ptr);
    free(gdf);
    free(pbuf);
    free(hits);
    return rc;
}
/* ---------------- 公开 API：建 / 关 ---------------- */

int ft_index_create(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid, FtIndex **out)
{
    FtIndex *ix;

    (void)xid;   /* 本版的段目录只在内存里，落盘是步骤 4/5 的事 */

    if (pg == NULL || out == NULL) {
        return E_INVAL;
    }
    *out = NULL;
    ix = (FtIndex *)calloc(1, sizeof(FtIndex));
    if (ix == NULL) {
        return E_NOSPACE;
    }
    ix->pg = pg;
    ix->wal = wal;
    ix->wal_ctx = wal_ctx;
    ix->next_seg_id = 0;
    *out = ix;
    return OK;
}
void ft_index_close(FtIndex *ix)
{
    u32 i;

    if (ix == NULL) {
        return;
    }
    for (i = 0; i < ix->n_segs; i++) {
        ft_segment_close(ix->segs[i]);
    }
    free(ix->segs);
    free(ix->tomb);
    free(ix);
}
/* ---------------- 公开 API：加文档 ---------------- */

int ft_index_add(FtIndex *ix, u32 xid, const FtDocText *docs, u32 n_docs, u32 *seg_id_out)
{
    u32 head = 0;
    u32 seg_id;
    FtSegment *seg = NULL;
    int rc;

    if (ix == NULL || seg_id_out == NULL) {
        return E_INVAL;
    }
    if ((docs == NULL && n_docs > 0) || n_docs == 0) {
        /* 空批次没有东西可建 —— 建一个 0 文档的段只会给合并添麻烦 */
        return E_INVAL;
    }
    *seg_id_out = 0;

    seg_id = ix->next_seg_id;
    rc = ft_segment_build(ix->pg, ix->wal, ix->wal_ctx, xid, docs, n_docs, seg_id, &head);
    if (rc != OK) {
        return rc;
    }
    /*
     * 段已经写到盘上了（页已经分配），段号就此用掉 ——
     * 段号不能重用，否则磁盘上会留下两个同号的段。
     */
    ix->next_seg_id++;

    rc = ft_segment_open(ix->pg, head, &seg);
    if (rc != OK) {
        /* 段头页已经写下去了，但打不开 → 如实报错（那几页成了孤儿，欠账） */
        return rc;
    }
    rc = ft_idx_push_seg(ix, seg);
    if (rc != OK) {
        ft_segment_close(seg);
        return rc;
    }
    *seg_id_out = seg_id;
    return OK;
}
/* ---------------- 公开 API：检索 ---------------- */

int ft_index_search(const FtIndex *ix, const char *query, u32 query_len,
                    u32 top_k, u32 max_out, FtHit *out, u32 *n_out)
{
    return ft_index_search_impl(ix, query, query_len, top_k, max_out, out, n_out, 1);
}
int ft_index_search_tfidf(const FtIndex *ix, const char *query, u32 query_len,
                          u32 top_k, u32 max_out, FtHit *out, u32 *n_out)
{
    return ft_index_search_impl(ix, query, query_len, top_k, max_out, out, n_out, 0);
}
/* ---------------- 公开 API：删除 ---------------- */

int ft_index_delete(FtIndex *ix, u32 xid, u32 doc_id)
{
    u32 pos;

    (void)xid;   /* tombstone 集合本版只在内存里，落盘是步骤 4/5 的事 */

    if (ix == NULL) {
        return E_INVAL;
    }
    pos = ft_idx_tomb_lb(ix->tomb, ix->n_tombs, doc_id);
    if (pos < ix->n_tombs && ix->tomb[pos] == doc_id) {
        /* 已经删过了：再删一次是合法的空操作，不报错 */
        return OK;
    }
    if (ix->n_tombs == ix->cap_tombs) {
        u32 ncap = (ix->cap_tombs == 0) ? FT_IDX_INIT_TOMBS : ix->cap_tombs * 2;
        u32 *nt = (u32 *)realloc(ix->tomb, sizeof(u32) * (size_t)ncap);

        if (nt == NULL) {
            return E_NOSPACE;
        }
        ix->tomb = nt;
        ix->cap_tombs = ncap;
    }
    /*
     * 插到 pos 处（保持升序）。
     *
     * 【为什么是"有序插入"而不是"追加后整体排序"】
     *
     *   - 有序插入：插入 O(n)（memmove），但**查找**是 O(log n) 二分；
     *     追加后排序：插入 O(n log n)，查找也只能 O(n) 线性扫。
     *   - 删除是**低频**操作，检索是**高频**操作 —— 该优化的是检索。
     *   - ft_merge.h 要求传过去的 tomb 集合必须升序（那边用二分判断成员），
     *     有序插入天然满足。
     */
    if (pos < ix->n_tombs) {
        memmove(&ix->tomb[pos + 1], &ix->tomb[pos],
                sizeof(u32) * (size_t)(ix->n_tombs - pos));
    }
    ix->tomb[pos] = doc_id;
    ix->n_tombs++;
    return OK;
}
/* ---------------- 公开 API：合并 ---------------- */

/*
 * 挑合并窗口：在所有长度为 2..n_segs 的**相邻窗口**里，挑"文档数之和最小"的那个。
 * 并列时的取法（必须确定性）：文档数之和更小的胜 → 并列取**窗口更大**的
 * → 还并列取**起点更小**的。
 *
 * 【为什么挑文档数之和最小的】合并要读这些段的字典与倒排、再写出一个新段，代价
 * 正比于参与的文档数；而且 Tiered 的思路就是"小段先合"。
 * 【为什么并列时选更大的窗口】窗口更大 = 一次把更多的段收成一段 → 段数降得更多
 * → 后续合并触发得更晚；同样的代价下少合并几轮更划算。
 *
 * 窗口个数 O(n²)、每个窗口求和 O(n) → 总共 O(n³)；段数被阈值卡在十几以内，
 * 这个代价可以忽略（真要担心，把前缀和存下来即可）。
 */
static void ft_idx_pick_window(const FtIndex *ix, u32 *start_out, u32 *width_out)
{
    u32 best_start = 0;
    u32 best_w = 0;
    u64 best_sum = 0;
    bool have = false;

    for (u32 w = 2; w <= ix->n_segs; w++) {
        for (u32 st = 0; st + w <= ix->n_segs; st++) {
            u64 sum = 0;
            bool better;

            for (u32 k = 0; k < w; k++) {
                sum += ix->segs[st + k]->doc_count;
            }
            better = (!have) ||
                     (sum < best_sum) ||
                     (sum == best_sum && w > best_w) ||
                     (sum == best_sum && w == best_w && st < best_start);
            if (better) {
                have = true;
                best_sum = sum;
                best_w = w;
                best_start = st;
            }
        }
    }
    *start_out = best_start;
    *width_out = best_w;
}
int ft_index_merge(FtIndex *ix, u32 xid, u32 *merged_out)
{
    u32 start = 0;
    u32 w = 0;
    FtMergeResult res;
    FtSegment *newseg = NULL;
    FtSegment **old = NULL;
    int rc;

    if (ix == NULL || merged_out == NULL) {
        return E_INVAL;
    }
    *merged_out = 0;

    /* 段数不够多 → 什么都不做（合并的收益还抵不上代价） */
    if (ix->n_segs <= FT_MERGE_THRESHOLD) {
        return OK;
    }
    ft_idx_pick_window(ix, &start, &w);
    if (w < 2) {
        return OK;   /* 理论上到不了这里（n_segs > 阈值 ≥ 8） */
    }
    memset(&res, 0, sizeof(res));
    rc = ft_merge_segments(ix->pg, ix->wal, ix->wal_ctx, xid,
                           ix->segs + start, w, ix->tomb, ix->n_tombs,
                           ix->next_seg_id, &res);
    if (rc != OK) {
        /* 合并失败 → 索引状态**完全不变**（段列表、tombstone 都不动） */
        return rc;
    }
    rc = ft_segment_open(ix->pg, res.head_page, &newseg);
    if (rc != OK) {
        /*
         * 合并本身成功了，但新段打不开 → 索引状态不变（还是老的那几个段）。
         * 新段的页成了孤儿 —— 与"旧段的页不回收"是同一类欠账，如实记录。
         */
        return rc;
    }
    /* ---- 换段：窗口里的 w 个段换成新段，新段放在窗口原来的位置 ---- */
    old = (FtSegment **)malloc(sizeof(FtSegment *) * (size_t)w);
    if (old == NULL) {
        ft_segment_close(newseg);
        return E_NOSPACE;
    }
    memcpy(old, ix->segs + start, sizeof(FtSegment *) * (size_t)w);

    /*
     * 窗口后面的段整体左移 (w-1) 位（新段占掉窗口的第一个位置）。
     *
     * ⚠️ 位移量是 **w-1** 不是 w（坑 M4-17）：w 个段换成 1 个段，段数只减少 w-1。
     * 写成 w 会把队尾多挪一格 —— 丢掉一个段，还留下一个野指针（释放时崩）。
     */
    if (ix->n_segs > start + w) {
        memmove(ix->segs + start + 1, ix->segs + start + w,
                sizeof(FtSegment *) * (size_t)(ix->n_segs - start - w));
    }
    ix->n_segs = ix->n_segs - (w - 1);

    /* 老段释放掉（只释放内存表示，不回收它们的页 —— 已知欠账） */
    for (u32 k = 0; k < w; k++) {
        ft_segment_close(old[k]);
    }
    free(old);

    ix->segs[start] = newseg;
    ix->next_seg_id++;
    *merged_out = w;
    return OK;
}
/* ---------------- 公开 API：统计 ---------------- */

u32 ft_index_nsegs(const FtIndex *ix)
{
    return (ix == NULL) ? 0 : ix->n_segs;
}
u32 ft_index_ndocs(const FtIndex *ix)
{
    u32 n = 0;

    if (ix == NULL) {
        return 0;
    }
    /* 各段 doc_count 之和（**含** tombstone 的文档，与 Lucene 一致） */
    for (u32 i = 0; i < ix->n_segs; i++) {
        n += ix->segs[i]->doc_count;
    }
    return n;
}
u32 ft_index_ntombstones(const FtIndex *ix)
{
    return (ix == NULL) ? 0 : ix->n_tombs;
}
u64 ft_index_page_reads(const FtIndex *ix)
{
    /* 索引级**累计**值（坑 M4-16）：跨检索累加，合并后也不会缩水 */
    return (ix == NULL) ? 0 : ix->page_reads_total;
}
