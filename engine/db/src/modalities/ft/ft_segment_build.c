/*
 * ft_segment_build.c —— 全文检索段的构建端（v1.3 步骤 2 / 步骤 3）
 *
 * 建段 = 把一批文档变成一个**不可变**的倒排索引：
 *
 *   PT_FT_HEAD（1 页，段头）/ PT_FT_DICT（若干页，字典）/ PT_FT_POST（若干页，倒排）
 *
 * 磁盘格式的契约全在 modalities/ft/ft_internal.h。
 *
 * 【这一层只管"算 posting"，不管"怎么写"】
 *
 * 写页（倒排页 / 字典页 / 段头页）的活儿全在 ft_writer.c ——
 * 合并（ft_merge.c）写新段时用的是同一套写入器，
 * 这样"建段写出来的段"和"合并写出来的段"在物理结构上必然一致。
 *
 * 【构建流程】切词 → 文档内去重计数 → 全局排序（词 asc, doc_id asc）
 * → 先写倒排页（posting 的位置要写进字典词条，所以倒排必须先落地）
 * → 再写字典页（词条 = 词 + df + post_ptr）→ 最后写段头页。
 */

#include "modalities/ft/ft_internal.h"
#include "modalities/ft/ft_writer.h"

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "modalities/ft/tokenizer.h"

/* ---------------- 构建期的内存表示 ---------------- */

/*
 * 一条 posting（构建期）。
 * 词存定长数组而不是指针：排序时少一层间接。
 */
typedef struct {
    char term[FT_MAX_TOKEN_LEN];  /* 词字节（'\0' 结尾），长度见 tlen */
    u16  tlen;
    u32  doc_id;
    u32  tf;   /* 该词在这篇文档里的出现次数 */
    u32  dl;   /* 文档长度（词数） */
} FtBuildPost;

/*
 * 全局排序：先按词（字节序），同词再按 doc_id 升序。
 * doc_id 也要排：同词的 posting 按 doc_id 递增，delta 编码依赖这个顺序。
 */
static int ft_build_post_cmp(const void *pa, const void *pb)
{
    const FtBuildPost *a = (const FtBuildPost *)pa;
    const FtBuildPost *b = (const FtBuildPost *)pb;
    int c = ft_term_cmp(a->term, a->tlen, b->term, b->tlen);

    if (c != 0)
        return c;
    if (a->doc_id < b->doc_id)
        return -1;
    if (a->doc_id > b->doc_id)
        return 1;
    return 0;
}

/* 文档内排序用：只比词（ft_term_cmp 是字典序的唯一权威） */
static int ft_token_cmp(const void *pa, const void *pb)
{
    const FtToken *a = (const FtToken *)pa;
    const FtToken *b = (const FtToken *)pb;

    return ft_term_cmp(a->text, a->len, b->text, b->len);
}

/*
 * 把一篇文档切成词，词表放在 *buf 里（不够就自动长）。
 * 两遍法：先用小缓冲试，返 E_NOSPACE 时按 need 扩容再来。
 */
static int ft_collect_tokens(const char *text, u32 text_len,
                             FtToken **buf, u32 *cap, u32 *n_out)
{
    u32 need = 0;
    int rc;

    if (*buf == NULL) {
        *cap = 256;
        *buf = (FtToken *)malloc(sizeof(FtToken) * (size_t)*cap);
        if (*buf == NULL)
            return E_IO;
    }

    rc = ft_tokenize(text, text_len, *buf, *cap, &need);
    if (rc == E_NOSPACE) {
        /* need = 这篇文档的总词数，按它重新分配再来一次 */
        FtToken *nb = (FtToken *)realloc(*buf, sizeof(FtToken) * (size_t)(need ? need : 1));
        if (nb == NULL)
            return E_NOSPACE;
        *buf = nb;
        *cap = need;
        rc = ft_tokenize(text, text_len, *buf, *cap, &need);
    }
    if (rc != OK)
        return rc;

    *n_out = need;
    return OK;
}

/* posting 数组的动态扩容 */
static int ft_postings_push(FtBuildPost **arr, u32 *cap, u32 *n, const FtBuildPost *p)
{
    if (*n == *cap) {
        u32 ncap = (*cap == 0) ? 64u : (*cap * 2u);
        void *np;

        if (ncap < *cap)
            return E_NOSPACE;              /* 翻倍溢出 */
        np = realloc(*arr, (size_t)ncap * sizeof(FtBuildPost));
        if (!np)
            return E_NOSPACE;
        *arr = (FtBuildPost *)np;
        *cap = ncap;
    }
    (*arr)[(*n)++] = *p;
    return OK;
}

/* doc_id 排序（用来查重） */
static int ft_u32_cmp(const void *pa, const void *pb)
{
    u32 a = *(const u32 *)pa;
    u32 b = *(const u32 *)pb;

    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

/*
 * 第一步：切词 + 文档内去重计数，产出 (词, doc_id, tf, dl) 的集合。
 * 文档内先合并成 tf：倒排表里一个 (词, 文档) 只有一条 posting。
 */
static int ft_build_postings(const FtDocText *docs, u32 n_docs,
                             FtBuildPost **posts_out, u32 *n_posts_out,
                             u64 *total_len_out, u32 *max_doc_id_out)
{
    FtToken *tok = NULL;
    u32 tok_cap = 0;
    FtBuildPost *posts = NULL;
    u32 n_posts = 0, cap = 0;
    u32 *ids = NULL;
    u64 total_len = 0;
    u32 max_doc_id = 0;
    u32 d;
    int rc = OK;

    *posts_out = NULL;
    *n_posts_out = 0;
    *total_len_out = 0;
    *max_doc_id_out = 0;

    /* doc_id 必须互不相同（ft_segment.h 里写死了这个约束）：
     * 同 id 的两篇文档在段里会合并成一条 posting，
     * 而线性扫描那边还是两条 —— A1 的对照就不成立了。 */
    if (n_docs > 0) {
        ids = (u32 *)malloc(sizeof(u32) * (size_t)n_docs);
        if (!ids)
            return E_IO;
        for (d = 0; d < n_docs; d++)
            ids[d] = docs[d].doc_id;
        qsort(ids, (size_t)n_docs, sizeof(u32), ft_u32_cmp);
        for (d = 1; d < n_docs; d++) {
            if (ids[d] == ids[d - 1]) {
                rc = E_INVAL;
                goto done;
            }
        }
    }

    for (d = 0; d < n_docs; d++) {
        u32 nt = 0;
        u32 i;

        rc = ft_collect_tokens(docs[d].text, docs[d].text_len, &tok, &tok_cap, &nt);
        if (rc != OK)
            goto done;

        /* dl = 这篇文档的词数（不是字节数） */
        total_len += (u64)nt;
        if (docs[d].doc_id > max_doc_id)
            max_doc_id = docs[d].doc_id;

        /* 排序后相同的词就挨在一起，扫一遍就能数出 tf */
        if (nt > 1)
            qsort(tok, (size_t)nt, sizeof(FtToken), ft_token_cmp);

        i = 0;
        while (i < nt) {
            u32 j = i + 1;
            FtBuildPost p;

            while (j < nt &&
                   ft_term_cmp(tok[i].text, tok[i].len, tok[j].text, tok[j].len) == 0)
                j++;

            memset(&p, 0, sizeof(p));
            memcpy(p.term, tok[i].text, (size_t)tok[i].len);
            p.tlen = tok[i].len;
            p.doc_id = docs[d].doc_id;
            p.tf = j - i;
            p.dl = nt;

            rc = ft_postings_push(&posts, &cap, &n_posts, &p);
            if (rc != OK)
                goto done;

            i = j;
        }
    }

    *posts_out = posts;
    *n_posts_out = n_posts;
    *total_len_out = total_len;
    *max_doc_id_out = max_doc_id;
    posts = NULL;

done:
    free(ids);
    free(tok);
    free(posts);
    return rc;
}

/* ---------------- 两趟写页（坑 M4-23） ---------------- */

/*
 * 【为什么要分两趟】
 *
 * 段头只记**连续区间**：dict_start + dict_count、post_start + post_count。
 * 读侧（跨页二分、词条游标）都按"连续区间"读页 ——
 * 所以字典页必须**连号**，倒排页也必须连号。
 *
 * 曾经的写法是"一个词先写倒排、再写字典"，两个写入器各自 pager_alloc
 * → 倒排页与字典页**交替分配**，页号不连号。
 * 后果：字典超过一页时，读侧按连续区间读到的其实是倒排页 →
 *   - ft_segment_lookup 返回 E_NOTFOUND（查不到）；
 *   - 检索命中 0 篇；
 *   - 合并时读到垃圾 → 报错，或者**静默丢词**。
 *
 * 单页字典时这个 bug 测不出来（词表 ≤ 约 177 个词时字典正好一页），
 * 所以必须有"字典跨页"的用例盯着（见 test_ft_segment.c 的字典跨页用例）。
 *
 * 【两趟怎么分工】
 *
 *   第一趟：只写倒排，顺手把每个词的倒排链头（post_ptr）记下来；
 *   第二趟：按同一个词序写字典，用第一趟记下的链头。
 *
 * 词本身不用另存 —— posting 数组还在内存里，第二趟重新走一遍词组
 * 就能拿到同样的词序（同样的输入 → 同样的顺序）。
 */

/* 一个词组（同一个词的 posting 连在一起） */
typedef int (*FtGroupFn)(void *ctx, const FtBuildPost *grp, u32 n, u32 term_len);

/*
 * 按词分组遍历已排序的 posting 数组。
 *
 * posts 已按 (词 asc, doc_id asc) 排好 → 同一个词的 posting 连续。
 */
static int ft_build_each_group(FtBuildPost *posts, u32 n_posts, FtGroupFn fn, void *ctx)
{
    u32 i = 0;

    while (i < n_posts) {
        u32 j = i + 1;
        u32 tlen = posts[i].tlen;
        int rc;

        while (j < n_posts &&
               ft_term_cmp(posts[i].term, tlen, posts[j].term, posts[j].tlen) == 0) {
            j++;
        }
        rc = fn(ctx, &posts[i], j - i, tlen);
        if (rc != OK) {
            return rc;
        }
        i = j;
    }
    return OK;
}

/* 第一趟的上下文：写倒排 + 记链头 */
typedef struct {
    FtPostWriter *pw;
    FtPosting    *scratch;
    u64          *ptrs;      /* 每个词的倒排链头 */
    u32           n_ptrs;
    u32           cap_ptrs;
} FtPass1Ctx;

static int ft_build_pass1(void *vctx, const FtBuildPost *grp, u32 n, u32 term_len)
{
    FtPass1Ctx *c = (FtPass1Ctx *)vctx;
    u64 ptr = FT_PTR_NIL;
    u32 k;
    int rc;

    (void)term_len;

    for (k = 0; k < n; k++) {
        c->scratch[k].doc_id = grp[k].doc_id;
        c->scratch[k].tf = grp[k].tf;
        c->scratch[k].dl = grp[k].dl;
    }
    rc = ft_post_writer_put(c->pw, c->scratch, n, &ptr);
    if (rc != OK) {
        return rc;
    }

    if (c->n_ptrs == c->cap_ptrs) {
        u32 ncap = (c->cap_ptrs == 0) ? 64u : c->cap_ptrs * 2u;
        u64 *np = (u64 *)realloc(c->ptrs, sizeof(u64) * (size_t)ncap);

        if (np == NULL) {
            return E_NOSPACE;
        }
        c->ptrs = np;
        c->cap_ptrs = ncap;
    }
    c->ptrs[c->n_ptrs++] = ptr;
    return OK;
}

/* 第二趟的上下文：写字典（链头取第一趟记下的） */
typedef struct {
    FtDictWriter  *dw;
    const u64     *ptrs;
    u32            n_ptrs;
    u32            idx;
} FtPass2Ctx;

static int ft_build_pass2(void *vctx, const FtBuildPost *grp, u32 n, u32 term_len)
{
    FtPass2Ctx *c = (FtPass2Ctx *)vctx;
    u64 ptr;

    if (c->idx >= c->n_ptrs) {
        return E_CORRUPT;     /* 两趟的词数对不上（不该发生） */
    }
    ptr = c->ptrs[c->idx++];
    return ft_dict_writer_put(c->dw, grp->term, term_len, n, ptr);
}

/* ---------------- 建段 ---------------- */

int ft_segment_build(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                     const FtDocText *docs, u32 n_docs, u32 seg_id,
                     u32 *head_page_out)
{
    FtBuildPost *posts = NULL;
    u32 n_posts = 0;
    u64 total_len = 0;
    u32 max_doc_id = 0;
    FtPostWriter *pw = NULL;
    FtDictWriter *dw = NULL;
    FtPosting *scratch = NULL;  /* 一个词的 posting 暂存区（喂给写入器） */
    u32 head_pid = PAGE_ID_NIL;
    u32 n_terms = 0;
    u32 d;      /* 文档下标 */
    int rc = OK;

    if (!pg || !head_page_out)
        return E_INVAL;
    if (!docs && n_docs > 0)
        return E_INVAL;

    for (d = 0; d < n_docs; d++) {
        if (!docs[d].text && docs[d].text_len > 0)
            return E_INVAL;
    }

    pw = ft_post_writer_new(pg, wal, wal_ctx, xid);
    dw = ft_dict_writer_new(pg, wal, wal_ctx, xid);
    if (!pw || !dw) {
        rc = E_IO;
        goto done;
    }

    /* 第一步：切词 + 文档内去重计数 */
    rc = ft_build_postings(docs, n_docs, &posts, &n_posts, &total_len, &max_doc_id);
    if (rc != OK)
        goto done;

    /* 第二步：全局排序（词 asc，doc_id asc） */
    if (n_posts > 1)
        qsort(posts, (size_t)n_posts, sizeof(FtBuildPost), ft_build_post_cmp);

    /*
     * 段头页**先分配**（页号最小、最好记），内容最后写。
     * 分配完立刻放开这一帧：内容由 ft_write_segment_head 通过 pager_get 取。
     */
    {
        PageFrame *head_fr = NULL;

        rc = pager_alloc(pg, &head_pid, &head_fr);
        if (rc != OK)
            goto done;
        pager_unpin(pg, head_pid, false);
    }

    /* 一个词的 posting 条数不会超过文档数（doc_id 互不相同），暂存区够用 */
    if (n_docs > 0) {
        scratch = (FtPosting *)malloc(sizeof(FtPosting) * (size_t)n_docs);
        if (!scratch) {
            rc = E_IO;
            goto done;
        }
    }

    /*
     * 第三步：**两趟**写页（坑 M4-23）—— 先把倒排写完，再写字典。
     *
     * 两趟分开是为了让**字典页连号**（段头只记连续区间，
     * 读侧按连续区间读页；混着写就会读到倒排页）。
     *
     * 一个词的 posting 连续、组内 doc_id 递增（都是排序保证的），
     * 所以 df = 组大小，posting 的 delta 全是非负的。
     */
    {
        FtPass1Ctx p1;
        FtPass2Ctx p2;

        memset(&p1, 0, sizeof(p1));
        memset(&p2, 0, sizeof(p2));
        p1.pw = pw;
        p1.scratch = scratch;

        rc = ft_build_each_group(posts, n_posts, ft_build_pass1, &p1);
        if (rc != OK) {
            free(p1.ptrs);
            goto done;
        }
        n_terms = p1.n_ptrs;

        /* 收尾：最后半页倒排写盘（页链到此为止，H_NEXT = 0 表示没有下一页） */
        rc = ft_post_writer_flush(pw);
        if (rc != OK) {
            free(p1.ptrs);
            goto done;
        }

        /*
         * 第二趟：按**同一个词序**写字典。
         *
         * posting 数组还在内存里，重新走一遍词组就是同一个顺序 ——
         * 所以词本身不用另存一份（省内存）。
         */
        p2.dw = dw;
        p2.ptrs = p1.ptrs;
        p2.n_ptrs = p1.n_ptrs;
        rc = ft_build_each_group(posts, n_posts, ft_build_pass2, &p2);
        free(p1.ptrs);
        if (rc != OK) {
            goto done;
        }
    }

    /*
     * 收尾：最后半页字典写盘。
     *
     * 字典页只在"写满"时才 flush，所以最后那一页必须在这里显式落盘 ——
     * 漏掉它，磁盘上的字典页就是一片空白，检索端一个词都查不到。
     */
    rc = ft_dict_writer_flush(dw);
    if (rc != OK)
        goto done;

    /* 第四步：写段头页 */
    rc = ft_write_segment_head(pg, wal, wal_ctx, xid, head_pid, seg_id,
                                n_docs, max_doc_id,
                                ft_dict_writer_start(dw), ft_dict_writer_count(dw),
                                ft_post_writer_start(pw), ft_post_writer_count(pw),
                                n_terms, total_len,
                                ft_dict_writer_last_lsn(dw) ? ft_dict_writer_last_lsn(dw)
                                                           : ft_post_writer_last_lsn(pw));
    if (rc != OK)
        goto done;

    *head_page_out = head_pid;
    head_pid = PAGE_ID_NIL;

done:
    if (pw)
        ft_post_writer_free(pw);
    if (dw)
        ft_dict_writer_free(dw);
    free(posts);
    free(scratch);
    return rc;
}
