/*
 * ft_segment_build.c —— 全文检索段的构建端（v1.3 步骤 2）
 *
 * 建段 = 把一批文档变成一个**不可变**的倒排索引：
 *
 *   PT_FT_HEAD（1 页，段头）/ PT_FT_DICT（若干页，字典）/ PT_FT_POST（若干页，倒排）
 *
 * 磁盘格式的契约全在 modalities/ft/ft_internal.h —— 本文件只负责"写"，
 * 检索端（ft_search.c）只负责"读"，两边都不许自己另搞一套格式。
 *
 * 【构建流程】切词 → 文档内去重计数 → 全局排序（词 asc, doc_id asc）
 * → 先写倒排页（posting 的位置要写进字典词条，所以倒排必须先落地）
 * → 再写字典页（词条 = 词 + df + post_ptr）→ 最后写段头页。
 *
 * 【格式要点】
 * - 字典页是**标准 slotted page**：词条用 page_insert 插，slot 顺序 = 字典序；
 *   "本页第一个词"就是 slot 0 的记录（坑 M4-13，字典页不能有自定义区）。
 * - 倒排页是**自定义密集格式**：页头之后 FPH_NRUNS / FPH_USED，
 *   数据区从 FPH_DATA 起，一个 run = 12 字节头 + n×12 字节 posting。
 * - 一个词的 posting 跨页时：FTR_NEXT_RUN 存"下一段所在**页号**"（0 = 链尾），
 *   下一段固定落在那一页的 FPH_DATA 处 —— 续接总是**先开新页**再写续段。
 *
 * 【写页的顺序（硬要求）】写日志（拿 LSN）→ 填 H_LSN → 算校验和 → 标脏放开。
 * 反过来的话，页会带着"日志里还没有"的 LSN 落盘，崩溃后重放会跳过这条记录。
 */

#include "modalities/ft/ft_internal.h"

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "modalities/ft/tokenizer.h"
#include "storage/page.h"

/* ---------------- 构建期的内存表示 ---------------- */

/* 一条 posting（构建期）。词存定长数组而不是指针：排序时少一层间接。 */
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
 * 两遍法：先用小缓冲试，返 E_NOSPACE 时按 need 扩容重来。
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

/* ---------------- 写一页（WAL 整页镜像） ---------------- */

/*
 * 把 buf 的内容写进 pid 这一页（写日志 → 填 LSN → 算校验和 → 标脏放开）。
 * before 镜像用全 0：这些页刚 pager_alloc 出来，写之前没人碰过。
 */
static int ft_write_page(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                         u32 pid, PageFrame *fr, const u8 *buf, u32 *lsn_out)
{
    static const u8 zeros[PAGE_SIZE] = { 0 };
    u32 lsn = 0;
    int rc;

    memcpy(fr->data, buf, PAGE_SIZE);

    if (wal) {
        rc = wal(wal_ctx, xid, LOG_PAGEDATA, pid, PAGE_SLOT_NONE, LOG_NO_SLOT_OFF,
                 zeros, (u16)PAGE_SIZE, fr->data, (u16)PAGE_SIZE, &lsn);
        if (rc != OK) {
            /* 日志没写好 → 这一页不许改 */
            memset(fr->data, 0, PAGE_SIZE);
            pager_unpin(pg, pid, false);
            return rc;
        }
    }

    page_set_lsn(fr->data, lsn);
    page_set_checksum(fr->data);

    rc = pager_unpin(pg, pid, true);
    if (rc != OK)
        return rc;

    if (lsn_out)
        *lsn_out = lsn;
    return OK;
}

/* ---------------- 倒排页写入器 ---------------- */

typedef struct {
    Pager    *pg;
    LogHookFn wal;
    void     *wal_ctx;
    u32       xid;

    u8        *buf;        /* 当前页的内容 */
    u32        cur_pid;
    PageFrame *cur_fr;
    u32        cur_used;   /* 数据区已用字节 */
    u32        cur_runs;   /* 本页的 run 数 */

    u32        post_start; /* 第一个倒排页的页号 */
    u32        post_count; /* 倒排页数 */
    u32        last_lsn;   /* 最后一次成功写日志的 LSN */
} FtPostWriter;

static void ft_post_writer_init(FtPostWriter *w, Pager *pg, LogHookFn wal,
                                 void *wal_ctx, u32 xid)
{
    w->pg = pg;
    w->wal = wal;
    w->wal_ctx = wal_ctx;
    w->xid = xid;
    w->buf = NULL;
    w->cur_pid = PAGE_ID_NIL;
    w->cur_fr = NULL;
    w->cur_used = 0;
    w->cur_runs = 0;
    w->post_start = PAGE_ID_NIL;
    w->post_count = 0;
    w->last_lsn = 0;
}

/* 把当前页写盘（并把它从"当前页"的位置上摘下来） */
static int ft_post_writer_flush(FtPostWriter *w)
{
    u32 lsn = 0;
    u32 pid = w->cur_pid;
    PageFrame *fr = w->cur_fr;
    int rc;

    if (pid == PAGE_ID_NIL)
        return OK;

    /* 页头：类型 / run 数 / 已用字节 */
    w->buf[H_TYPE] = (u8)PT_FT_POST;
    put_u32(w->buf + FPH_NRUNS, w->cur_runs);
    put_u16(w->buf + FPH_USED, (u16)w->cur_used);

    /* 先把"当前页"摘掉：ft_write_page 一定会放开这一页 */
    w->cur_pid = PAGE_ID_NIL;
    w->cur_fr = NULL;
    w->cur_used = 0;
    w->cur_runs = 0;

    rc = ft_write_page(w->pg, w->wal, w->wal_ctx, w->xid, pid, fr, w->buf, &lsn);
    if (rc != OK)
        return rc;
    if (lsn != 0)
        w->last_lsn = lsn;
    return OK;
}

/* 分配一个新页（页号从文件尾往后排，所以连续分配就是连续页号） */
static int ft_post_writer_alloc(FtPostWriter *w, u32 *pid_out, PageFrame **fr_out)
{
    u32 pid = PAGE_ID_NIL;
    PageFrame *fr = NULL;
    int rc = pager_alloc(w->pg, &pid, &fr);

    if (rc != OK)
        return rc;

    if (w->post_start == PAGE_ID_NIL)
        w->post_start = pid;
    w->post_count++;

    *pid_out = pid;
    *fr_out = fr;
    return OK;
}

/* 把刚分配的页设成"当前页" */
static void ft_post_writer_attach(FtPostWriter *w, u32 pid, PageFrame *fr)
{
    memset(w->buf, 0, PAGE_SIZE);
    w->cur_pid = pid;
    w->cur_fr = fr;
    w->cur_used = 0;
    w->cur_runs = 0;
}

/*
 * 开一个新页当当前页（第一个倒排页走这里）。
 * 新页是空的，所以落在它上面的第一个 run 正好在 FPH_DATA 处 ——
 * 这就是"续接的 run 固定落在续接页的 FPH_DATA"这条约定的由来。
 */
static int ft_post_writer_new_page(FtPostWriter *w)
{
    u32 pid = PAGE_ID_NIL;
    PageFrame *fr = NULL;
    int rc = ft_post_writer_alloc(w, &pid, &fr);

    if (rc != OK)
        return rc;
    ft_post_writer_attach(w, pid, fr);
    return OK;
}

/*
 * 分配下一页，并把当前页的页链指针 H_NEXT 指过去。
 * 注意：**当前页这时还没写盘** —— 调用方还要往里写东西
 * （比如把 run 头的 FTR_NEXT_RUN 改成续接页的页号），写完再 flush。
 */static int ft_post_writer_open_next(FtPostWriter *w, u32 *pid_out, PageFrame **fr_out)
{
    u32 pid = PAGE_ID_NIL;
    PageFrame *fr = NULL;
    int rc = ft_post_writer_alloc(w, &pid, &fr);

    if (rc != OK)
        return rc;

    put_u32(w->buf + H_NEXT, pid);

    *pid_out = pid;
    *fr_out = fr;
    return OK;
}

/*
 * 把一个词的 posting 写进倒排页，返回**第一个 run 的位置**（post_ptr）。
 * 当前页放不下时，当前页放能放下的那部分，剩下的放到新开的一页，
 * 续接的 run 落在新页的 FPH_DATA 处，前一个 run 的 FTR_NEXT_RUN 填新页页号。
 *
 * delta 编码：run 头的 FTR_FIRST_DOC 是这一段第一个 posting 的**绝对** doc_id，
 * 所以第一条 posting 的 delta = 0（相对自己）；
 * 后面的 delta = doc_id - 前一个 doc_id。检索端从 first_doc 起步逐条累加。
 */
static int ft_post_writer_put_term(FtPostWriter *w, const FtBuildPost *ps, u32 n,
                                   u64 *first_ptr_out)
{
    u32 i = 0;
    int rc;

    *first_ptr_out = FT_PTR_NIL;

    while (i < n) {
        u32 space, cap, take, run_off;
        u32 first_doc = ps[i].doc_id;
        u32 k;

        if (w->cur_pid == PAGE_ID_NIL) {
            rc = ft_post_writer_new_page(w);
            if (rc != OK)
                return rc;
        }

        space = PAGE_SIZE - FPH_DATA - w->cur_used;
        if (space < FT_RUN_HDR_BYTES + FT_POSTING_BYTES) {
            /*
             * 本页连"一个 run 头 + 一条 posting"都放不下 → 换页。
             * 本页最后一个 run 是链尾（FTR_NEXT_RUN = 0），不用改。
             */
            u32 next_pid = PAGE_ID_NIL;
            PageFrame *next_fr = NULL;

            rc = ft_post_writer_open_next(w, &next_pid, &next_fr);
            if (rc != OK)
                return rc;
            rc = ft_post_writer_flush(w);
            if (rc != OK) {
                pager_unpin(w->pg, next_pid, false);
                return rc;
            }
            ft_post_writer_attach(w, next_pid, next_fr);
            continue;
        }

        cap = (space - FT_RUN_HDR_BYTES) / FT_POSTING_BYTES;
        take = (n - i < cap) ? (n - i) : cap;
        run_off = FPH_DATA + w->cur_used;

        /* run 头：first_doc / 条数 / 预留 / 下一段（先按链尾写，拆的时候再改） */
        ft_run_encode(w->buf + run_off, first_doc, (u16)take, 0);

        for (k = 0; k < take; k++) {
            u32 doc = ps[i + k].doc_id;
            u32 delta = (k == 0) ? (doc - first_doc)
                                 : (doc - ps[i + k - 1].doc_id);
            ft_posting_encode(w->buf + run_off + FT_RUN_HDR_BYTES
                              + FT_POSTING_BYTES * k,
                              delta, ps[i + k].tf, ps[i + k].dl);
        }
        w->cur_used += FT_RUN_HDR_BYTES + FT_POSTING_BYTES * take;
        w->cur_runs++;

        if (i == 0)
            *first_ptr_out = ft_ptr_make(w->cur_pid, run_off);

        i += take;
        if (i < n) {
            /*
             * 这个词还没写完 → 剩下的放到新开的一页。
             *
             * 顺序（硬要求）：先开新页（页号这时才知道）→ 改 run 头的
             * FTR_NEXT_RUN → 把当前页写盘 → 新页成为当前页
             * （它的第一个 run 就是续接段，落在 FPH_DATA）。
             */
            u32 next_pid = PAGE_ID_NIL;
            PageFrame *next_fr = NULL;

            rc = ft_post_writer_open_next(w, &next_pid, &next_fr);
            if (rc != OK)
                return rc;

            /* 改写 run 头：next_run = 续接段所在页的页号 */
            ft_run_encode(w->buf + run_off, first_doc, (u16)take, next_pid);

            rc = ft_post_writer_flush(w);
            if (rc != OK) {
                pager_unpin(w->pg, next_pid, false);
                return rc;
            }
            ft_post_writer_attach(w, next_pid, next_fr);
        }
    }

    return OK;
}

/* ---------------- 字典页写入器 ---------------- */

/*
 * 字典页是**标准 slotted page**：词条变长，用 page_insert 插，
 * slot 顺序 = 插入顺序 = 字典序。
 *
 * "本页第一个词"不另存 —— 直接取 **slot 0 的记录**（坑 M4-13）：
 * slot 目录从 HDR_SIZE=20 往上长，page_insert 会盖掉偏移 20 处的自定义字段。
 *
 * 一页额度：FT_DICT_CAPACITY = PAGE_SIZE - HDR_SIZE（记录字节 + slot）。
 */
typedef struct {
    Pager    *pg;
    LogHookFn wal;
    void     *wal_ctx;
    u32       xid;

    u8        *buf;
    u32        cur_pid;
    PageFrame *cur_fr;
    u32        used;       /* 已用字节（记录长 + SLOT_SIZE） */
    u32        n_entries;  /* 本页的词条数 */

    u32        dict_start;
    u32        dict_count;
    u32        last_lsn;
} FtDictWriter;

static void ft_dict_writer_init(FtDictWriter *w, Pager *pg, LogHookFn wal,
                                 void *wal_ctx, u32 xid)
{
    w->pg = pg;
    w->wal = wal;
    w->wal_ctx = wal_ctx;
    w->xid = xid;
    w->buf = NULL;
    w->cur_pid = PAGE_ID_NIL;
    w->cur_fr = NULL;
    w->used = 0;
    w->n_entries = 0;
    w->dict_start = PAGE_ID_NIL;
    w->dict_count = 0;
    w->last_lsn = 0;
}

static int ft_dict_writer_flush(FtDictWriter *w)
{
    u32 lsn = 0;
    u32 pid = w->cur_pid;
    PageFrame *fr = w->cur_fr;
    int rc;

    if (pid == PAGE_ID_NIL)
        return OK;

    /* 先把"当前页"摘掉：ft_write_page 一定会放开这一页 */
    w->cur_pid = PAGE_ID_NIL;
    w->cur_fr = NULL;
    w->used = 0;
    w->n_entries = 0;

    rc = ft_write_page(w->pg, w->wal, w->wal_ctx, w->xid, pid, fr, w->buf, &lsn);
    if (rc != OK)
        return rc;
    if (lsn != 0)
        w->last_lsn = lsn;
    return OK;
}

/* 开一个新字典页 */
static int ft_dict_writer_new_page(FtDictWriter *w)
{
    u32 pid = PAGE_ID_NIL;
    PageFrame *fr = NULL;
    int rc = pager_alloc(w->pg, &pid, &fr);

    if (rc != OK)
        return rc;

    memset(w->buf, 0, PAGE_SIZE);
    page_init(w->buf, PT_FT_DICT);

    if (w->dict_start == PAGE_ID_NIL)
        w->dict_start = pid;
    w->dict_count++;

    w->cur_pid = pid;
    w->cur_fr = fr;
    w->used = 0;
    w->n_entries = 0;
    return OK;
}

/*
 * 写一个词条。
 * term 必须是**已经排好序**的词流里的下一个词 ——
 * 页内 slot 顺序 = 插入顺序 = 字典序，页与页之间也按词的先后排列。
 */
static int ft_dict_writer_put(FtDictWriter *w, const char *term, u32 term_len,
                              u32 df, u64 post_ptr)
{
    /* 最长词条 = 2（长度头）+ 63（词字节）+ 12（df + post_ptr） */
    u8 entry[FT_MAX_TOKEN_LEN + FT_DICT_HDR_BYTES + FT_DICT_TAIL_BYTES];
    u32 entry_len = ft_dict_entry_bytes(term_len);
    u32 need = entry_len + SLOT_SIZE;
    u16 slot;
    int rc;

    if (w->cur_pid == PAGE_ID_NIL) {
        rc = ft_dict_writer_new_page(w);
        if (rc != OK)
            return rc;
    } else if (w->used + need > FT_DICT_CAPACITY) {
        /* 本页装不下了 → 先把当前页写盘，再开新页 */
        rc = ft_dict_writer_flush(w);
        if (rc != OK)
            return rc;
        rc = ft_dict_writer_new_page(w);
        if (rc != OK)
            return rc;
    }

    ft_dict_entry_encode(entry, term, term_len, df, post_ptr);
    rc = page_insert(w->buf, entry, (u16)entry_len, &slot);
    if (rc != OK)
        return rc;

    w->used += need;
    w->n_entries++;
    return OK;
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
    FtPostWriter pw;
    FtDictWriter dw;
    u8 *head_buf = NULL;
    u32 head_pid = PAGE_ID_NIL;
    PageFrame *head_fr = NULL;
    u32 n_terms = 0;
    u32 d;      /* 文档下标 */
    u32 i;      /* posting 下标 */
    int rc = OK;

    if (!pg || !head_page_out)
        return E_INVAL;
    if (!docs && n_docs > 0)
        return E_INVAL;

    for (d = 0; d < n_docs; d++) {
        if (!docs[d].text && docs[d].text_len > 0)
            return E_INVAL;
    }

    ft_post_writer_init(&pw, pg, wal, wal_ctx, xid);
    ft_dict_writer_init(&dw, pg, wal, wal_ctx, xid);

    pw.buf = (u8 *)malloc(PAGE_SIZE);
    dw.buf = (u8 *)malloc(PAGE_SIZE);
    head_buf = (u8 *)malloc(PAGE_SIZE);
    if (!pw.buf || !dw.buf || !head_buf) {
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
     */
    rc = pager_alloc(pg, &head_pid, &head_fr);
    if (rc != OK)
        goto done;

    /*
     * 第三步：按词分组，先写倒排、再写字典。
     * 一个词的 posting 连续、组内 doc_id 递增（都是排序保证的），
     * 所以 df = 组大小，posting 的 delta 全是非负的。
     */
    i = 0;
    while (i < n_posts) {
        u32 j = i + 1;
        u32 df;
        u64 post_ptr = FT_PTR_NIL;

        while (j < n_posts &&
               ft_term_cmp(posts[i].term, posts[i].tlen,
                           posts[j].term, posts[j].tlen) == 0)
            j++;

        df = j - i;

        rc = ft_post_writer_put_term(&pw, &posts[i], df, &post_ptr);
        if (rc != OK)
            goto done;

        rc = ft_dict_writer_put(&dw, posts[i].term, posts[i].tlen, df, post_ptr);
        if (rc != OK)
            goto done;

        n_terms++;
        i = j;
    }

    /*
     * 收尾：最后半页写盘。
     *
     * 倒排页之间**不靠页链**（链在 run 头的 FTR_NEXT_RUN 里），
     * 所以 H_NEXT 一律是 PAGE_ID_NIL —— 由 ft_post_writer_flush 统一写。
     */
    rc = ft_post_writer_flush(&pw);
    if (rc != OK)
        goto done;

    /*
     * 收尾：最后半页字典写盘。
     *
     * 字典页只在"写满"时才 flush，所以最后那一页必须在这里显式落盘 ——
     * 漏掉它，磁盘上的字典页就是一片空白，检索端一个词都查不到。
     */
    rc = ft_dict_writer_flush(&dw);
    if (rc != OK)
        goto done;

    /* 第四步：写段头页 */
    memset(head_buf, 0, PAGE_SIZE);
    rc = page_init(head_buf, PT_FT_HEAD);
    if (rc != OK)
        goto done;

    head_buf[FTH_VER] = (u8)FT_HEAD_VER;
    put_u32(head_buf + FTH_SEG_ID, seg_id);
    put_u32(head_buf + FTH_DOC_COUNT, n_docs);
    put_u32(head_buf + FTH_MAX_DOC_ID, max_doc_id);
    /*
     * 页区间用 0 表示"没有页"（而不是 PAGE_ID_NIL）：
     * 检索端常见写法是 `for (pid = start; pid < start + count; pid++)`，
     * start = PAGE_ID_NIL 时 start + count 会回绕成 0，循环反而会跑起来。
     */
    put_u32(head_buf + FTH_DICT_START, (dw.dict_count > 0) ? dw.dict_start : 0u);
    put_u32(head_buf + FTH_DICT_COUNT, dw.dict_count);
    put_u32(head_buf + FTH_POST_START, (pw.post_count > 0) ? pw.post_start : 0u);
    put_u32(head_buf + FTH_POST_COUNT, pw.post_count);
    put_u32(head_buf + FTH_NTERMS, n_terms);
    put_u64(head_buf + FTH_TOTAL_LEN, total_len);
    /*
     * FTH_CREATE_LSN = 最后一次**内容页**（倒排/字典）写入的 LSN。
     * 为什么不是段头页自己的 LSN：那个值要等段头页自己的日志写完才知道，
     * 而日志镜像里必须已经带着它（否则重放出来的页和原页不一样）。
     */
    put_u32(head_buf + FTH_CREATE_LSN,
            (dw.last_lsn != 0) ? dw.last_lsn : pw.last_lsn);
    rc = ft_write_page(pg, wal, wal_ctx, xid, head_pid, head_fr, head_buf, NULL);
    /* 无论成败，这一页都已经放开了（成功时写成，失败时 ft_write_page 会放开） */
    head_fr = NULL;
    if (rc != OK)
        goto done;

    *head_page_out = head_pid;
    head_pid = PAGE_ID_NIL;

done:
    if (head_fr) {
        /* 出错：段头页还没写，放开它（内容是全 0，不算改动） */
        pager_unpin(pg, head_pid, false);
    }
    if (pw.cur_fr) {
        /* 出错：倒排的当前页还没写，放开它 */
        pager_unpin(pg, pw.cur_pid, false);
    }
    if (dw.cur_fr) {
        /* 出错：字典的当前页还没写，放开它 */
        pager_unpin(pg, dw.cur_pid, false);
    }
    free(posts);
    free(pw.buf);
    free(dw.buf);
    free(head_buf);
    return rc;
}
