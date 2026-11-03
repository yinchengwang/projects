/*
 * ft_search.c —— 全文检索模态的检索端：单段检索（v1.3 步骤 2）
 *
 * ==================== 这一层在做什么 ====================
 *
 * 段（segment）是磁盘上的不可变倒排索引：段头页 + 字典页 + 倒排页。
 * 构建端（ft_segment.c）负责把它写出来，本文件负责在里面**查**。
 *
 * 检索路径只有三步：
 *
 *   1. 切查询词（分词器与线性扫描共用同一个 → 切法必然一致）
 *   2. 每个 query 词查一次字典（跨页二分 + 页内二分）拿到 df 与倒排链头
 *   3. 顺着倒排链走一遍，把每个命中文档的分数累加进一张哈希表
 *
 * ==================== 语义必须与线性扫描逐位一致 ====================
 *
 * bm25.c 的 ft_linear_scan 是**基准实现**（A1 验收就是拿它对照）。
 * 本文件的所有"语义决定"都以它为准：
 *
 *   - 只有**至少命中一个 query 词**的文档才进结果；
 *   - query 里的重复词**不去重**（坑 M4-12），各贡献一次分数；
 *   - 排序：分数降序、分数相同按 doc_id 升序（坑 M4-11）；
 *   - df 是"含有该词的文档数"，dl 是"该文档的词数"，
 *     avgdl = total_len / doc_count（与 ft_segment_avgdl 同一个算式）。
 *
 * ==================== 与线性扫描的差别只在"怎么找到 posting" ====================
 *
 * 线性扫描把每篇文档都切一遍词，本文件只走"字典 → 倒排"这条路。
 * 分数公式、累加方式、排序规则都直接调用 bm25.c 里的同一个函数，
 * 所以两边算出来的分数是同一个 double（不是"差不多"，是同一个值）。
 */

#include "modalities/ft/ft_internal.h"

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "modalities/ft/tokenizer.h"
#include "storage/page.h"

/* 一个词的倒排链最多允许串多少段（防御：坏数据导致的死循环） */
#define FT_MAX_RUN_CHAIN 65536u

/* ---------------- 统计：页读次数 ---------------- */

/*
 * 记一次页读。
 *
 * seg 是 const —— 但 page_reads 只是个**统计字段**，
 * 我们只改它，不改段里的任何内容（段是只读的，页也不会被标脏）。
 * 所以这里强转一下，并把理由写明。
 */
static void ft_count_page_read(const FtSegment *seg)
{
    FtSegment *m = (FtSegment *)seg;   /* 只改统计字段，不改段的内容 */
    m->page_reads++;
}

/* ---------------- 切查询词 ---------------- */

/*
 * 切一次查询串，词表放在 *buf 里（不够就自动长）。
 *
 * 两遍法：先用一块固定大小的缓冲试切，装不下就按"需要的词数"扩容重来。
 * 和 bm25.c 的 ft_collect_tokens 同一套写法 —— 分词器对"装不下"的约定
 * 是"一个词都不写、只报需要的数量"，所以重来一次是安全的。
 */
static int ft_collect_tokens(const char *text, u32 text_len, FtToken **buf, u32 *cap, u32 *n_out)
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
        /* need = 总词数，按它重新分配再来一次 */
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

/* ---------------- 分数累加表（开放寻址哈希） ---------------- */

/*
 * (doc_id → 累计分数)。
 *
 * 【为什么必须用哈希表而不是数组】
 *
 * 命中文档的 doc_id 是**调用方给的、不连续的**（不是 0..N-1），
 * 而且数量事先不知道。用数组就得按 max_doc_id 开空间（浪费），
 * 用"线性扫数组找有没有这个 doc"就是 O(n²) —— 十万文档直接卡死。
 *
 * 开放寻址 + 线性探测：条目数 <= 命中文档数，容量取 2 的幂，
 * 装填因子 <= 0.5（满了就翻倍重建）。
 *
 * 散列函数用 Knuth 乘法散列，和字典页那一套是同一个味道。
 */
typedef struct {
    u32    *keys;    /* doc_id */
    double *vals;    /* 累计分数 */
    u8     *used;    /* 该槽有没有被占（doc_id 可以是任意值，不能拿 0 当空） */
    u32     cap;     /* 槽数（2 的幂） */
    u32     count;   /* 已占槽数 */
} FtScoreMap;

static u32 ft_map_hash(u32 doc_id)
{
    return doc_id * 2654435761u;   /* Knuth 乘法散列 */
}

static void ft_map_init(FtScoreMap *m)
{
    memset(m, 0, sizeof(*m));
}

static void ft_map_free(FtScoreMap *m)
{
    free(m->keys);
    free(m->vals);
    free(m->used);
    memset(m, 0, sizeof(*m));
}

static int ft_map_alloc(FtScoreMap *m, u32 cap)
{
    m->keys = (u32 *)calloc(cap, sizeof(u32));
    m->vals = (double *)calloc(cap, sizeof(double));
    m->used = (u8 *)calloc(cap, sizeof(u8));
    if (!m->keys || !m->vals || !m->used) {
        ft_map_free(m);
        return E_NOSPACE;
    }
    m->cap = cap;
    m->count = 0;
    return OK;
}

/* 容量翻倍并把老条目重插进去 */
static int ft_map_grow(FtScoreMap *m)
{
    u32 ncap = m->cap * 2;
    u32 *nk = (u32 *)calloc(ncap, sizeof(u32));
    double *nv = (double *)calloc(ncap, sizeof(double));
    u8 *nu = (u8 *)calloc(ncap, sizeof(u8));
    u32 i;

    if (!nk || !nv || !nu) {
        free(nk);
        free(nv);
        free(nu);
        return E_NOSPACE;
    }

    for (i = 0; i < m->cap; i++) {
        u32 j;
        if (!m->used[i]) {
            continue;
        }
        j = ft_map_hash(m->keys[i]) & (ncap - 1);
        while (nu[j]) {
            j = (j + 1) & (ncap - 1);
        }
        nu[j] = 1;
        nk[j] = m->keys[i];
        nv[j] = m->vals[i];
    }

    free(m->keys);
    free(m->vals);
    free(m->used);
    m->keys = nk;
    m->vals = nv;
    m->used = nu;
    m->cap = ncap;
    return OK;
}

/* 给 doc_id 累加一分。成功返回 0，内存不足返回 -1 */
static int ft_map_add(FtScoreMap *m, u32 doc_id, double score)
{
    u32 i;

    if (m->cap == 0) {
        if (ft_map_alloc(m, 64) != OK) {
            return -1;
        }
    }
    /* 装填因子 <= 0.5：满了先翻倍 */
    if ((u64)m->count * 2 >= (u64)m->cap) {
        if (ft_map_grow(m) != OK) {
            return -1;
        }
    }

    i = ft_map_hash(doc_id) & (m->cap - 1);
    for (;;) {
        if (!m->used[i]) {
            m->used[i] = 1;
            m->keys[i] = doc_id;
            m->vals[i] = score;
            m->count++;
            return 0;
        }
        if (m->keys[i] == doc_id) {
            m->vals[i] += score;   /* 多个 query 词命中同一篇 → 分数累加 */
            return 0;
        }
        i = (i + 1) & (m->cap - 1);
    }
}

/* ---------------- 字典查找（跨页二分 + 页内二分） ---------------- */

/*
 * 取字典页自定义区里的"本页第一个词"。
 *
 * 字典页是标准 slotted page，slot i 就是第 i 个词（插入顺序 = 字典序）。
 * 页头的自定义区额外存了本页第一个词 —— 有了它才能做**跨页二分**：
 * 不用把所有页的第一个词都读出来，靠"读一页比一次"就能定位到候选页。
 */
/* ---------------- 倒排遍历 ---------------- */

/*
 * 这个位置看起来像一个合法的 run 头吗？
 *
 * 判据（三条都要满足）：
 *   1. run 头落在页内、且在数据区里；
 *   2. npost > 0；
 *   3. run 头 + 它带的 posting 整段都在页内。
 *
 * 这三条是**结构上的合理性校验**：段格式写死了一种布局，
 * 校验不通过就是段坏了（E_CORRUPT），而不是"换个位置再猜一次"。
 */
static bool ft_run_looks_valid(const u8 *page, u32 off)
{
    u32 npost;

    if (off < FPH_DATA || off + FT_RUN_HDR_BYTES > PAGE_SIZE) {
        return false;
    }
    npost = ft_run_npost(page + off);
    if (npost == 0) {
        return false;
    }
    if (off + FT_RUN_HDR_BYTES + npost * FT_POSTING_BYTES > PAGE_SIZE) {
        return false;
    }
    return true;
}

/*
 * 顺着倒排链走一遍，把每个命中文档的分数累加进 map。
 *
 * 【posting 的解码】
 *
 * 每个 run 都从**自己的** FTR_FIRST_DOC 起步，再逐条 doc += FTP_DELTA。
 *
 * 为什么不是"跨 run 一直累加下去"：
 * FTR_FIRST_DOC 是"**本段**第一个 posting 的**绝对** doc_id"，
 * 而本段第一个 posting 的 delta 恒为 0（构建端写的是 doc - first_doc）。
 * 所以每个 run 都必须从 first_doc 重新起步 —— 上一段累加下来的 doc
 * 加上本段的 delta 会把 first_doc 重复算一遍，doc_id 直接错掉。
 * （run 之间的连续性由"本段 first_doc > 上一段最后一个 doc"保证。）
 *
 * 契约里 FTP_DELTA 写的是"第一个是相对 first_doc_id"，
 *   所以第一条 posting 的 doc_id = first_doc + delta[0]。
 *
 * 【链的走法：只有一条路】
 *
 * 契约写死了倒排页的布局：
 *
 *   FTR_NEXT_RUN = 下一个 run 所在**页号**（0 = 链尾）；
 *   续接的 run 一定落在那一页的 FPH_DATA（=28）处。
 *
 * 所以读链就是：`(next_run 的页号, FPH_DATA)`，没有第二种解释。
 *
 * 为什么不写"几种可能的约定、猜中了就用"：
 * 契约已经把布局定死，多写几条猜测路径等于把"布局写错了"
 * 变成"静默也能读出来" —— 那是最坏的一种错（错误被掩盖，
 * 等到合并/恢复时才炸）。所以这里只有一条路 +
 * 结构上的合理性校验（不合法就报 E_CORRUPT）。
 *
 * 【另一个终止条件：df】
 *
 * df 是"含有该词的文档数"，也就是**这个词的 posting 条数**。
 * 所以读到 df 条 posting 就该收工了 —— 这是与链的走法无关的第二道保险，
 * 链上多出一段时不会把别的词的 posting 吃进来。
 */
static int ft_post_scan(const FtSegment *seg, u64 post_ptr, u32 df, const FtCorpus *cs,
                         FtScoreMap *map, int use_bm25)
{
    u32 page = ft_ptr_page(post_ptr);
    u32 off = ft_ptr_off(post_ptr);
    PageFrame *fr = NULL;
    u32 cur_page = 0;
    bool have_frame = false;
    u32 last_doc = 0;
    bool have_last = false;
    u32 collected = 0;
    u32 guard = 0;
    int rc = OK;

    for (;;) {
        const u8 *data;
        u32 npost;
        u32 next_run;
        u32 first_doc;
        u32 doc;

        if (++guard > FT_MAX_RUN_CHAIN) {
            rc = E_CORRUPT;     /* 链太长：段坏了 */
            goto done;
        }

        /* 需要换页才重新 pin —— 同一页里的多段共用一次页读 */
        if (!have_frame || page != cur_page) {
            if (have_frame) {
                pager_unpin(seg->pg, cur_page, false);
                have_frame = false;
            }
            rc = pager_get(seg->pg, page, &fr);
            if (rc != OK) {
                goto done;
            }
            ft_count_page_read(seg);
            cur_page = page;
            have_frame = true;
        }

        data = fr->data;
        if (!ft_run_looks_valid(data, off)) {
            /* 结构不合法：段坏了（布局只有一种，不合法就是坏了） */
            rc = E_CORRUPT;
            goto done;
        }
        npost = ft_run_npost(data + off);
        next_run = ft_run_next_run(data + off);
        first_doc = ft_run_first_doc(data + off);
        if (have_last && first_doc <= last_doc) {
            /*
             * 倒排是按 doc_id 递增的：本段首文档必须大于上一段最后一个。
             * 不满足 = 段坏了 —— 宁可报错，也不要返回错的 doc_id。
             */
            rc = E_CORRUPT;
            goto done;
        }

        doc = first_doc;
        for (u32 i = 0; i < npost; i++) {
            const u8 *p = data + off + FTR_POSTS + i * FT_POSTING_BYTES;
            u32 tf;
            u32 dl;
            double s;

            doc += ft_posting_delta(p);
            tf = ft_posting_tf(p);
            dl = ft_posting_dl(p);

            if (use_bm25) {
                s = ft_bm25_score(cs, df, tf, dl);
            } else {
                s = ft_tfidf_score(cs, df, tf);
            }
            if (ft_map_add(map, doc, s) != 0) {
                rc = E_NOSPACE;
                goto done;
            }
        }

        collected += npost;
        last_doc = doc;
        have_last = true;

        /* 这个词的 posting 读完了 */
        if (df != 0 && collected >= df) {
            break;
        }

        /* ---- 下一段：只有一条路 ---- */
        if (next_run == 0) {
            break;      /* 链尾 */
        }
        page = next_run;        /* 下一段所在页的页号 */
        off = FPH_DATA;         /* 续接的 run 一定落在那一页的 FPH_DATA 处 */
    }

done:
    if (have_frame) {
        pager_unpin(seg->pg, cur_page, false);
    }
    return rc;
}

/* ---------------- 排序 ---------------- */

/*
 * 结果排序：分数降序、分数相同按 doc_id 升序（坑 M4-11）。
 *
 * 为什么不用 epsilon 比较分数：浮点相等必须用 == 判，
 * 用 epsilon 会让"本来同分"的两条被排成不同序 —— 与线性扫描对不上。
 * doc_id 是唯一的，所以这个比较是**全序**，qsort 的结果唯一。
 */
static int ft_hit_cmp(const void *a, const void *b)
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

/* ---------------- 检索主流程 ---------------- */

/*
 * 单段检索的主流程。use_bm25 != 0 走 BM25，否则走 TF-IDF。
 *
 * 两个公开入口只是这个函数的两个壳 —— 语义必须完全一样，
 * 所以只留一个实现，不复制两份。
 */
static int ft_segment_search_impl(const FtSegment *seg, const char *query, u32 query_len,
                                   u32 top_k, u32 max_out, FtHit *out, u32 *n_out,
                                   int use_bm25)
{
    FtToken *qbuf = NULL;
    u32 qcap = 0;
    u32 nq = 0;
    FtScoreMap map;
    FtCorpus cs;
    FtHit *hits = NULL;
    u32 n = 0;
    u32 k = 0;
    int rc = OK;

    /* ---- 参数检查（顺序与 bm25.c 的线性扫描完全一致） ---- */
    if (seg == NULL) {
        return E_INVAL;
    }
    if (n_out == NULL) {
        return E_INVAL;
    }
    /* 出错路径上 *n_out 一律是 0：调用方不能拿到"半截结果" */
    *n_out = 0;

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
         *
         * 为什么不做"命中几条就返回几条"：
         *   - 两个 API（段检索 / 线性扫描）必须行为一致 ——
         *     同样的 (top_k, max_out) 两边给同样的返回码，A1 的对照才成立；
         *   - 静默截断出来的结果"看起来对其实错"（调用方以为拿到了 top_k 条，
         *     实际只有 max_out 条，少的往往是排名靠后的），比报错坏得多。
         */
        return E_NOSPACE;
    }

    /* 语料为空 / 不要结果 → 没有可返回的东西 */
    if (seg->doc_count == 0 || top_k == 0) {
        return OK;
    }

    /* ---- 切查询词 ---- */
    rc = ft_collect_tokens(query, query_len, &qbuf, &qcap, &nq);
    if (rc != OK) {
        goto done;
    }
    if (nq == 0) {
        rc = OK;   /* 查询串里没有词 → 没有命中 */
        goto done;
    }

    /* ---- 语料级统计量（与线性扫描同一个算式） ---- */
    cs.n_docs = seg->doc_count;
    cs.avgdl = ft_segment_avgdl(seg);

    ft_map_init(&map);

    /*
     * ---- 逐个 query 词查一遍 ----
     *
     * 坑 M4-12：query 里的重复词**不去重** ——
     * 每个位置各查一次倒排、各加一次分，与线性扫描完全一致。
     */
    for (u32 q = 0; q < nq; q++) {
        u32 df = 0;
        u64 post_ptr = FT_PTR_NIL;

        rc = ft_segment_lookup(seg, qbuf[q].text, qbuf[q].len, &df, &post_ptr);
        if (rc == E_NOTFOUND) {
            /* 字典里没这个词 → 它不贡献任何分数 */
            rc = OK;
            continue;
        }
        if (rc != OK) {
            goto done_map;
        }
        rc = ft_post_scan(seg, post_ptr, df, &cs, &map, use_bm25);
        if (rc != OK) {
            goto done_map;
        }
    }

    /* ---- 收集结果并排序 ---- */
    n = map.count;
    if (n > 0) {
        u32 i = 0;
        hits = (FtHit *)malloc(sizeof(FtHit) * (size_t)n);
        if (hits == NULL) {
            rc = E_NOSPACE;
            goto done_map;
        }
        for (u32 s = 0; s < map.cap; s++) {
            if (!map.used[s]) {
                continue;
            }
            hits[i].doc_id = map.keys[s];
            hits[i].score = map.vals[s];
            i++;
        }
        qsort(hits, n, sizeof(FtHit), ft_hit_cmp);
    }

    /* ---- 取前 top_k 条 ---- */
    k = (n < top_k) ? n : top_k;
    /* k <= top_k <= max_out（top_k > max_out 已在参数检查里报错），
     * 所以这里写出去的一定装得下 —— 不需要再判一次。 */

    if (k > 0) {
        memcpy(out, hits, sizeof(FtHit) * (size_t)k);
    }
    *n_out = k;
    rc = OK;

done_map:
    ft_map_free(&map);
done:
    free(qbuf);
    free(hits);
    return rc;
}

/* ---------------- 公开 API ---------------- */

int ft_segment_search(const FtSegment *seg, const char *query, u32 query_len,
                      u32 top_k, u32 max_out, FtHit *out, u32 *n_out)
{
    return ft_segment_search_impl(seg, query, query_len, top_k, max_out, out, n_out, 1);
}

int ft_segment_search_tfidf(const FtSegment *seg, const char *query, u32 query_len,
                            u32 top_k, u32 max_out, FtHit *out, u32 *n_out)
{
    return ft_segment_search_impl(seg, query, query_len, top_k, max_out, out, n_out, 0);
}
