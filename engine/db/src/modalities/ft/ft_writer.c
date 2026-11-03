/*
 * ft_writer.c —— 段写入器（v1.3 步骤 3）
 *
 * 【这一层管什么】
 *
 * "把一批 posting 写成一个段"有两个调用方：
 *   1. 建段（ft_segment_build.c）：从文档切词、聚合出 posting，再写段；
 *   2. 合并（ft_merge.c）：把若干段的 posting 归并后，写成一个新段。
 *
 * 两边要写的物理结构**完全一样**（同样的三种页、同样的编码），
 * 所以写页这一层只能有一份 —— 各写一份的话，迟早会写出两种不一样的段，
 * 而检索端只认其中一种，另一种会静默返回错结果。
 *
 * 抽象的边界画在"posting 的输入"上：
 *   上层负责**算出 posting 序列**（怎么算是上层的事），
 *   这一层只负责**把 posting 序列写成段**（怎么写是这一层的事）。
 *
 * 【输入是绝对 doc_id，落盘时才变 delta】
 *
 * 磁盘上的 posting 是 12 字节定长三字段（delta / tf / dl），
 * 而且 doc_id 是 delta 编码的 —— 那层编码在这一层里做。
 * 对外的输入是**绝对 doc_id** 的 FtPosting（内存结构，定义在 ft_segment.h）。
 *
 * 【格式要点】
 *
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

#include "modalities/ft/ft_writer.h"

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "modalities/ft/ft_internal.h"
#include "modalities/ft/tokenizer.h"
#include "storage/page.h"

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

struct FtPostWriter {
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
};

FtPostWriter *ft_post_writer_new(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid)
{
    FtPostWriter *w = (FtPostWriter *)calloc(1, sizeof(FtPostWriter));

    if (!w)
        return NULL;

    w->pg = pg;
    w->wal = wal;
    w->wal_ctx = wal_ctx;
    w->xid = xid;
    w->cur_pid = PAGE_ID_NIL;
    w->post_start = PAGE_ID_NIL;

    w->buf = (u8 *)malloc(PAGE_SIZE);
    if (!w->buf) {
        free(w);
        return NULL;
    }
    memset(w->buf, 0, PAGE_SIZE);
    return w;
}

/*
 * 释放写入器。
 *
 * 当前页如果还没写盘（出错路径），这里放开它 —— 放开时**不标脏**，
 * 那一页的内容不算改动（它本来就是新分配的页）。
 */
void ft_post_writer_free(FtPostWriter *w)
{
    if (!w)
        return;

    if (w->cur_fr)
        pager_unpin(w->pg, w->cur_pid, false);
    free(w->buf);
    free(w);
}

/* 把当前页写盘（并把它从"当前页"的位置上摘下来） */
int ft_post_writer_flush(FtPostWriter *w)
{
    u32 lsn = 0;
    u32 pid = w->cur_pid;
    PageFrame *fr = w->cur_fr;
    int rc;

    if (!w)
        return E_INVAL;
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
 */
static int ft_post_writer_open_next(FtPostWriter *w, u32 *pid_out, PageFrame **fr_out)
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
int ft_post_writer_put(FtPostWriter *w, const FtPosting *ps, u32 n, u64 *first_ptr_out)
{
    u32 i = 0;
    int rc;

    if (!w || !ps || !first_ptr_out)
        return E_INVAL;

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

u32 ft_post_writer_start(const FtPostWriter *w)
{
    return w ? w->post_start : PAGE_ID_NIL;
}

u32 ft_post_writer_count(const FtPostWriter *w)
{
    return w ? w->post_count : 0u;
}

u32 ft_post_writer_last_lsn(const FtPostWriter *w)
{
    return w ? w->last_lsn : 0u;
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
struct FtDictWriter {
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
};

FtDictWriter *ft_dict_writer_new(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid)
{
    FtDictWriter *w = (FtDictWriter *)calloc(1, sizeof(FtDictWriter));

    if (!w)
        return NULL;

    w->pg = pg;
    w->wal = wal;
    w->wal_ctx = wal_ctx;
    w->xid = xid;
    w->cur_pid = PAGE_ID_NIL;
    w->dict_start = PAGE_ID_NIL;

    w->buf = (u8 *)malloc(PAGE_SIZE);
    if (!w->buf) {
        free(w);
        return NULL;
    }
    memset(w->buf, 0, PAGE_SIZE);
    return w;
}

/* 释放写入器（当前页没写盘时放开它，规则同倒排写入器） */
void ft_dict_writer_free(FtDictWriter *w)
{
    if (!w)
        return;

    if (w->cur_fr)
        pager_unpin(w->pg, w->cur_pid, false);
    free(w->buf);
    free(w);
}

int ft_dict_writer_flush(FtDictWriter *w)
{
    u32 lsn = 0;
    u32 pid = w->cur_pid;
    PageFrame *fr = w->cur_fr;
    int rc;

    if (!w)
        return E_INVAL;
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
int ft_dict_writer_put(FtDictWriter *w, const char *term, u32 term_len,
                        u32 df, u64 post_ptr)
{
    /* 最长词条 = 2（长度头）+ 63（词字节）+ 12（df + post_ptr） */
    u8 entry[FT_MAX_TOKEN_LEN + FT_DICT_HDR_BYTES + FT_DICT_TAIL_BYTES];
    u32 entry_len;
    u32 need;
    u16 slot;
    int rc;

    if (!w || !term)
        return E_INVAL;

    entry_len = ft_dict_entry_bytes(term_len);
    need = entry_len + SLOT_SIZE;

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

u32 ft_dict_writer_start(const FtDictWriter *w)
{
    return w ? w->dict_start : PAGE_ID_NIL;
}

u32 ft_dict_writer_count(const FtDictWriter *w)
{
    return w ? w->dict_count : 0u;
}

u32 ft_dict_writer_last_lsn(const FtDictWriter *w)
{
    return w ? w->last_lsn : 0u;
}

/* ---------------- 段头页 ---------------- */

/*
 * 写段头页。
 *
 * 页区间用 0 表示"没有页"（而不是 PAGE_ID_NIL）：
 * 检索端常见写法是 `for (pid = start; pid < start + count; pid++)`，
 * start = PAGE_ID_NIL 时 start + count 会回绕成 0，循环反而会跑起来。
 * 所以"没有页"这件事统一在这里归一化，调用方不用自己记得。
 */
int ft_write_segment_head(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                          u32 head_pid, u32 seg_id,
                          u32 doc_count, u32 max_doc_id,
                          u32 dict_start, u32 dict_count,
                          u32 post_start, u32 post_count,
                          u32 n_terms, u64 total_len, u32 create_lsn)
{
    u8 *buf = NULL;
    PageFrame *fr = NULL;
    int rc;

    if (!pg)
        return E_INVAL;

    buf = (u8 *)malloc(PAGE_SIZE);
    if (!buf)
        return E_IO;

    memset(buf, 0, PAGE_SIZE);
    rc = page_init(buf, PT_FT_HEAD);
    if (rc != OK) {
        free(buf);
        return rc;
    }

    buf[FTH_VER] = (u8)FT_HEAD_VER;
    put_u32(buf + FTH_SEG_ID, seg_id);
    put_u32(buf + FTH_DOC_COUNT, doc_count);
    put_u32(buf + FTH_MAX_DOC_ID, max_doc_id);
    put_u32(buf + FTH_DICT_START, (dict_count > 0) ? dict_start : 0u);
    put_u32(buf + FTH_DICT_COUNT, dict_count);
    put_u32(buf + FTH_POST_START, (post_count > 0) ? post_start : 0u);
    put_u32(buf + FTH_POST_COUNT, post_count);
    put_u32(buf + FTH_NTERMS, n_terms);
    put_u64(buf + FTH_TOTAL_LEN, total_len);
    /*
     * FTH_CREATE_LSN = 建段时的 LSN（取最后一次内容页写入的 LSN）。
     * 不能用段头页自己的 LSN：那个值要等段头页自己的日志写完才知道，
     * 而日志镜像里必须已经带着它，否则重放出来的页和原页不一样。
     */
    put_u32(buf + FTH_CREATE_LSN, create_lsn);

    /*
     * 段头页的页是调用方**先分配**的（页号要最小，内容最后写）。
     * 这里用 pager_get 取它 —— 所以调用方在分配之后必须已经放开那一帧，
     * 否则同一帧会被拿两次（内容锁是递归的，但 pin 会配不上）。
     */
    rc = pager_get(pg, head_pid, &fr);
    if (rc != OK) {
        free(buf);
        return rc;
    }

    rc = ft_write_page(pg, wal, wal_ctx, xid, head_pid, fr, buf, NULL);
    free(buf);
    return rc;
}
