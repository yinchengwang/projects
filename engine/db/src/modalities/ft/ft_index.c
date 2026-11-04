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
 * 在不同段里拿到的分数不可比，排序会乱，也与线性扫描对不上。与线性扫描逐位
 * 一致的三个要点：① 统计量用全局的（N = Σ doc_count，df(t) = Σ 各段 df(t)，
 * avgdl = Σ total_len / N）；② 累加顺序 = query 词出现的顺序（浮点加法不满足
 * 结合律；一篇文档只落在一个段里，两边顺序一致）；③ 排序：分数降序、同分按
 * doc_id 升序（坑 M4-11）。
 *
 * tombstone 是一份**全局的 doc_id 集合**（按升序维护，理由见 ft_index.h）：被删的
 * 文档**不进结果**，但**仍计入 N 与 df**（直到合并物理丢掉），与 Lucene 一致；
 * 后果是删除后 idf 略有偏差 —— 如实记录在案。集合只增不减。
 *
 * 合并是 Tiered：段数超过阈值时，在所有长度为 2..n 的**相邻窗口**里挑"文档数之和
 * 最小"的那个合成一个新段（代价正比于参与的文档数）。旧段的页在**合并提交之后**
 * 还给段级空闲页池（v1.3 欠账 2）→ 文件不再随合并次数线性增长。
 *
 * 【段目录的落盘形态】（v1.3 欠账 3 / 7）：建段 / 合并提交之后要写段目录
 * （ft_catalog_sync），目录机制与恢复在 ft_catalog.c。
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
 * 【doc_id 可以是任意值】—— 不能拿 doc_id == 0 当"空槽"标记，故另设 used 标志。
 */typedef struct {
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
 * 两遍法：先用一块固定大小的缓冲试切，装不下就按"需要的词数"扩容重来 ——
 * 分词器对"装不下"的约定是"一个词都不写、只报需要的数量"，所以重来是安全的。
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

/* tombstone 的内存集合（升序数组）与落盘形态都在 ft_tomb.c 里 —— 本文件只调用
 * （二分查找与有序插入必须只有一份）。 */

/* ---------------- 段数组 ---------------- */

/* 追加一个段（段数组按 2 的幂增长） */
int ft_index_push_seg(FtIndex *ix, FtSegment *seg)
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
        /* 没有结果缓冲区：top_k 非 0 → E_NOSPACE；top_k 为 0 → E_INVAL（没意义） */
        return (top_k > 0) ? E_NOSPACE : E_INVAL;
    }
    if (top_k > max_out) {
        /*
         * 严格口径：**top_k > max_out 一律报错**，不看实际命中几条 —— 三个 API
         * 必须行为一致；静默截断出来的结果"看起来对其实错"，比报错坏得多。
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
     * 每个 query 词在**每个段**里查一次字典：这个段没有这个词 → df = 0
     * （E_NOTFOUND 是正常结果）；有 → 该段的 df 计入全局 df，倒排链头缓存下来。
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
                if (ft_tomb_is(ix, p->doc_id)) {
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

    (void)xid;   /* 段目录落盘用的是段头页里的状态字段（见 ft_catalog.c） */

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
    ix->pool = ft_pool_new();   /* 段级空闲页池（v1.3 欠账 2） */
    ix->cat_pool = ft_pool_new(); /* 目录链专用空闲页池（坑 M4-53 收尾） */
    if (ix->pool == NULL || ix->cat_pool == NULL) {
        ft_pool_free(ix->pool);
        ft_pool_free(ix->cat_pool);
        free(ix);
        return E_NOSPACE;
    }
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
    ft_tomb_free(ix);
    ft_pool_free(ix->pool);
    ft_pool_free(ix->cat_pool);
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
    rc = ft_segment_build_ex(ix->pg, ix->pool, ix->wal, ix->wal_ctx, xid,
                              docs, n_docs, seg_id, &head);
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
    rc = ft_index_push_seg(ix, seg);
    if (rc != OK) {
        ft_segment_close(seg);
        return rc;
    }
    *seg_id_out = seg_id;

    /*
     * 【写段目录 = 加文档的提交点】目录写不进去 → 这次加文档**不算数**（报错）。
     * 段已经落在盘上了，但目录没列它 → 恢复时看不见它 —— 那正是"没提交"的样子
     * （文档没丢，只是这次操作没成功）。
     */
    rc = ft_catalog_sync(ix, xid);
    if (rc != OK) {
        return rc;
    }
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
    int rc;

    if (ix == NULL) {
        return E_INVAL;
    }
    if (ft_tomb_is(ix, doc_id)) {
        /* 已经删过了：再删一次是合法的空操作，不报错 */
        return OK;
    }
    /*
     * 【先落盘，再进内存集合】tombstone 页是**权威存储**（坑 B）：它不会被
     * checkpoint 截断，而内存集合一重启就没了。顺序反过来的话（先进内存、
     * 写页失败），重开库后那条删除就又冒出来了。
     */
    rc = ft_tomb_append(ix, xid, doc_id);
    if (rc != OK) {
        return rc;
    }
    if (ft_tomb_insert(ix, doc_id) < 0) {
        return E_NOSPACE;
    }
    return OK;
}
/* ---------------- 公开 API：合并 ---------------- */

/*
 * 挑合并窗口的活儿在 ft_merge.c 里（ft_merge_pick_window）——
 * 它是"合并策略"，与合并的执行放在一起更好懂。
 */
int ft_index_merge(FtIndex *ix, u32 xid, u32 *merged_out)
{
    u32 start = 0;
    u32 w = 0;
    FtMergeResult res;
    FtSegment *newseg = NULL;
    FtSegment **old = NULL;
    u32 *heads = NULL;
    u64 pr_before = 0;
    u64 pr_after = 0;
    int rc;

    if (ix == NULL || merged_out == NULL) {
        return E_INVAL;
    }
    *merged_out = 0;

    /* 段数不够多 → 什么都不做（合并的收益还抵不上代价） */
    if (ix->n_segs <= FT_MERGE_THRESHOLD) {
        return OK;
    }
    ft_merge_pick_window(ix->n_segs, ix->segs, &start, &w);
    if (w < 2) {
        return OK;   /* 理论上到不了这里（n_segs > 阈值 ≥ 8） */
    }

    /* 老段的段头页号（改状态时要用） */
    heads = (u32 *)malloc(sizeof(u32) * (size_t)w);
    if (heads == NULL) {
        return E_NOSPACE;
    }
    for (u32 k = 0; k < w; k++) {
        heads[k] = ix->segs[start + k]->head_page;
        pr_before += ix->segs[start + k]->page_reads;
    }

    /*
     * 第一步：把老段标成 MERGING（指向新段的段号）。
     *
     * 【必须先标，再写新段】（坑 M4-21）：反过来的话，"新段已存在、老段还没
     * 标死"时崩溃，重开库就会把两批文档都算进去（同一篇文档数两遍）。
     * 标了 MERGING 之后就不用怕了：恢复时看"目标段在不在"就能定死活。
     */
    rc = ft_catalog_mark_merge(ix, xid, ix->next_seg_id, heads, w);
    if (rc != OK) {
        free(heads);
        return rc;
    }

    memset(&res, 0, sizeof(res));
    rc = ft_merge_segments(ix->pg, ix->pool, ix->wal, ix->wal_ctx, xid,
                           ix->segs + start, w, ix->tomb, ix->n_tombs,
                           ix->next_seg_id, &res);
    if (rc != OK) {
        /* 合并失败 → 状态回滚（老段还是活的），索引状态**完全不变** */
        (void)ft_catalog_abort_merge(ix, xid, heads, w);
        free(heads);
        return rc;
    }
    /* 新段的段号就此用掉 —— 段号不能重用（磁盘上已经有一个同号的段了） */
    ix->next_seg_id++;

    rc = ft_segment_open(ix->pg, res.head_page, &newseg);
    if (rc != OK) {
        /*
         * 合并本身成功了，但新段打不开 → 老段必须算死（否则同一批文档被数两遍）。
         * 新段的页成了孤儿 —— 与"旧段的页不回收"是同一类欠账，如实记录。
         */
        (void)ft_catalog_end_merge(ix, xid, heads, w);
        free(heads);
        return rc;
    }

    /*
     * 第三步：老段标死。标死了磁盘上的状态与"恢复时的判定"完全一致；标死失败
     * 也不影响正确性（恢复时 MERGING + 目标段存在 = 死，坑 M4-21）—— 这里如实
     * 返回错误，但**内存状态照样换掉**（与磁盘一致）。
     */
    rc = ft_catalog_end_merge(ix, xid, heads, w);

    /*
     * 收"合并页读"（必须在老段被释放之前收 —— 释放后就读不到了）。口径 = 参与
     * 合并的段的页读**增量** + 合并自己读的字典页数（游标读的，不在段的计数里）。
     */
    for (u32 k = 0; k < w; k++) {
        pr_after += ix->segs[start + k]->page_reads;
    }
    ix->merge_page_reads += (pr_after - pr_before) + res.page_reads;

    /* ---- 换段：窗口里的 w 个段换成新段，新段放在窗口原来的位置 ---- */
    old = (FtSegment **)malloc(sizeof(FtSegment *) * (size_t)w);
    if (old == NULL) {
        ft_segment_close(newseg);
        free(heads);
        return E_NOSPACE;
    }
    memcpy(old, ix->segs + start, sizeof(FtSegment *) * (size_t)w);

    /*
     * 窗口后面的段整体左移 (w-1) 位（新段占掉窗口的第一个位置）。⚠️ 位移量是 **w-1**
     * 不是 w（坑 M4-17）：写成 w 会丢掉一个段，还留下一个野指针（释放时崩）。
     */
    if (ix->n_segs > start + w) {
        memmove(ix->segs + start + 1, ix->segs + start + w,
                sizeof(FtSegment *) * (size_t)(ix->n_segs - start - w));
    }
    ix->n_segs = ix->n_segs - (w - 1);

    /*
     * 老段释放掉，页还给空闲页池（v1.3 欠账 2）。⚠️ 必须在标死之后归还：标死之前
     * 老段还可能被回滚 —— 提前归还 = 把还在用的页发出去 = 老段的数据被踩。
     */
    for (u32 k = 0; k < w; k++) {
        /* 登记成死段：段号从此不复用（磁盘上还留着那个段的页） */
        int rc2 = ft_catalog_add_dead(ix, old[k]->seg_id, old[k]->head_page);

        if (rc2 != OK && rc == OK)
            rc = rc2;
        (void)ft_seg_recycle_pages(ix->pool, old[k]);
        ft_segment_close(old[k]);
    }
    free(old);
    free(heads);

    ix->segs[start] = newseg;
    *merged_out = w;

    /*
     * 【写段目录 = 合并的提交点】目录里：老段 → DEAD，新段 → LIVE。这一写之前，
     * 合并都算"没发生"（老段还是活的，文档还在老段里）。写不进去 → 如实报错
     * （此时内存里已经是新段了：多出来的那个成了孤儿页，但文档既没丢也没重复）。
     */
    {
        int rc2 = ft_catalog_sync(ix, xid);

        if (rc2 != OK)
            return rc2;
    }
    return rc;
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

/*
 * 索引级**累计**的"合并页读"（与 ft_index_page_reads 分开）。
 *
 * 口径：每次 ft_index_merge() 把这次合并读过的页数累加进来 ——
 * 读老段倒排页的那部分（各段 page_reads 的增量）
 * + 合并自己读的字典页（游标读的）。
 */
u64 ft_index_merge_page_reads(const FtIndex *ix)
{
    return (ix == NULL) ? 0 : ix->merge_page_reads;
}

/*
 * 空闲页池里现在有多少页（v1.3 欠账 2，诊断用）：
 * "合并腾出来、还没被再用掉"的页 —— 还在文件里占着位置，但没人引用了。
 */
u32 ft_index_free_pages(const FtIndex *ix)
{
    return (ix == NULL) ? 0u : ft_pool_count(ix->pool);
}
/* 累计回收过多少页（v1.3 欠账 2，诊断用） */
u64 ft_index_recycled_pages(const FtIndex *ix)
{
    return (ix == NULL) ? 0u : ft_pool_added_total(ix->pool);
}
