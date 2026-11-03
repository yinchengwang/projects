#ifndef DB_MODALITIES_FT_INTERNAL_H
#define DB_MODALITIES_FT_INTERNAL_H

/*
 * ft_internal.h —— 全文检索段（segment）的磁盘格式契约（v1.3 步骤 2）
 *
 * 【这个文件是"构建端"和"检索端"之间的唯一约定】
 *
 * 段格式（PLAN-v1.3 §1.3）：
 *
 *   PT_FT_HEAD（1 页，段头）
 *   PT_FT_DICT（若干页，字典：有序的 term 数组）
 *   PT_FT_POST（若干页，倒排：posting 数组）
 *
 * 构建端（ft_segment.c）负责写，检索端（ft_search.c）负责读。
 * 两边都只认本文件里的偏移与编解码函数 ——
 * 一旦有一边自己另搞一套格式，"检索结果 == 线性扫描"这条断言就会以
 * "莫名其妙少一个 term"的方式炸掉，而那种 bug 极难定位。
 *
 * 【页头的前 20 字节是 slotted page 的标准头，不要乱占】
 *
 * 0 H_TYPE / 1 H_FLAGS / 2-3 H_SLOTS / 4-5 H_FREE_OFF / 6-7 H_FREE_BYTES /
 * 8-11 H_NEXT / 12-15 H_LSN / 16-17 H_CKSUM / 18-19 H_TID
 *
 * 所以：页类型放 H_TYPE（0）、链页放 H_NEXT（8）、LSN 放 H_LSN（12），
 * 校验和由 page_set_checksum() 算（覆盖整页，包括自定义区）。
 * 模态自己的字段一律从偏移 20 开始。
 *
 * ⚠️ **但 slotted page 是个例外**（坑 M4-13）：
 * 它的 **slot 目录也从偏移 20 开始往上长**
 * （page.h 的 page_slot_off_raw = HDR_SIZE + i*SLOT_SIZE）。
 * 所以**用 slotted page 的页（字典页）不能有自定义区** ——
 * page_insert 写 slot 目录时会覆盖掉偏移 20 处的自定义字段。
 */

#include "common/config.h"
#include "common/types.h"
#include "modalities/ft/ft_segment.h"
#include "storage/pager.h"

/* ---------------- 页类型版本 ---------------- */

/*
 * 版本号只有**段头页**那一个（FT_HEAD_VER）：
 * 段的三种页是一套格式，版本跟着段头走。
 * 字典页不能再单独存版本 —— 它的 slot 目录从偏移 20 开始，
 * 那里放不下自定义字段（坑 M4-13）。
 */
#define FT_HEAD_VER 1u

/* ---------------- 段头页（PT_FT_HEAD） ---------------- */

enum {
    FTH_VER         = 20,  /* u8  版本 */
    FTH_SEG_ID      = 24,  /* u32  段号 */
    FTH_DOC_COUNT   = 28,  /* u32  文档数 N */
    FTH_MAX_DOC_ID  = 32,  /* u32  段内最大 doc_id */
    FTH_DICT_START  = 36,  /* u32  字典页起始页号 */
    FTH_DICT_COUNT  = 40,  /* u32  字典页数 */
    FTH_POST_START  = 44,  /* u32  倒排页起始页号 */
    FTH_POST_COUNT  = 48,  /* u32  倒排页数 */
    FTH_NTERMS      = 52,  /* u32  段的词数 */
    FTH_TOTAL_LEN   = 56,  /* u64  所有文档的词数之和（avgdl = 它 / 文档数） */
    FTH_CREATE_LSN  = 64   /* u32  建段时的 LSN */
};

/*
 * 【为什么 avgdl 不直接存 double】
 *
 * avgdl = total_len / doc_count，而这两个都是整数。
 * 存 f64 到磁盘就得处理字节序和"结构体填充"，不值当。
 * 存整数、读出来现算，得到的 double 和线性扫描那边算出来的**逐位相同**
 * （两边都是 (double)total_len / (double)doc_count）。
 */

/* ---------------- 字典页（PT_FT_DICT） ---------------- */

/*
 * ⚠️ **字典页没有任何自定义区** —— 这是踩过坑之后定下来的。
 *
 * 坑 M4-13：字典页用的是标准 slotted page，而 slotted page 的
 * **slot 目录从 HDR_SIZE=20 开始往上长**（page.h 的 page_slot_off_raw）。
 * 所以"从偏移 20 开始放模态自己的字段"这个直觉在字典页上是**错的**：
 * page_insert 写 slot 目录时会直接覆盖掉那里的自定义字段，
 * 跨页二分拿到的"本页第一个词"就是垃圾 → 每个查询都返回 0 条。
 *
 * 结论：字典页的"本页第一个词"**不另存**，直接取 **slot 0 的记录**
 * （插入顺序 = 字典序，所以 slot 0 就是本页最小的词）。
 * 段格式的版本号由**段头页**的 FTH_VER 统一管，字典页不再单独记版本。
 */
#define FT_DICT_CAPACITY (PAGE_SIZE - HDR_SIZE)

/* ---------------- 倒排页（PT_FT_POST） ---------------- */

enum {
    FPH_NRUNS = 20,  /* u32  本页的 posting 段（run）数 */
    FPH_USED  = 24,  /* u16  数据区已用字节数 */
    FPH_DATA  = 28   /* 数据区起点 */
};

/* 倒排页的链页放标准头 H_NEXT（8），LSN 放 H_LSN（12） */

/* ---------------- 倒排：一个 run 的布局 ---------------- */

enum {
    FTR_FIRST_DOC = 0,   /* u32  本段第一个文档的绝对 doc_id */
    FTR_NPOST     = 4,   /* u16  本段的 posting 条数 */
    FTR_FLAGS     = 6,   /* u16  预留 */
    FTR_NEXT_RUN  = 8,   /* u32  下一个 run 所在**页号**（0 = 链尾） */
    FTR_POSTS     = 12   /* posting 数组起点 */
};

/*
 * 【FTR_NEXT_RUN 为什么只存页号，不存 post_ptr】
 *
 * post_ptr 是 u64（页号<<32 | 页内偏移），而 run 头里这个字段是 u32。
 * 不用把它撑成 u64，是因为**续接的 run 永远是新页的第一个 run**：
 * 倒排页是"填满就换页"的密集格式，拆 run 时总是先开新页再写续段，
 * 所以续接 run 一定落在那一页的 FPH_DATA 处 —— 页号就足够定位。
 * 词的 post_ptr（写进字典词条的那个）仍然是 u64，位置是精确的。
 */

/* 一个 posting（12 字节） */
enum {
    FTP_DELTA = 0,       /* u32  与前一个 doc_id 的差值（第一个是相对 first_doc_id） */
    FTP_TF    = 4,       /* u32  该词在文档里的出现次数 */
    FTP_DL    = 8        /* u32  文档长度（词数） */
};

#define FT_RUN_HDR_BYTES    12u
#define FT_POSTING_BYTES    12u

/*
 * 【为什么 posting 里带 dl（文档长度）】
 *
 * BM25 的长度归一项需要每篇文档的词数 dl。
 * 段的物理形态里没有"文档表"，所以 dl 只能跟着 posting 走 ——
 * 每个词重复存一遍。代价是 4 字节/条，收益是不用引入第四种页类型。
 * 备选方案（加一张 ordinal → (doc_id, dl) 的表）留到步骤 3 之后再评估。
 */

/* ---------------- post_ptr：把 (页号, 页内偏移) 打包成一个 u64 ---------------- */

static inline u64 ft_ptr_make(u32 page, u32 off)
{
    return ((u64)page << 32) | (u64)off;
}

static inline u32 ft_ptr_page(u64 p)
{
    return (u32)(p >> 32);
}

static inline u32 ft_ptr_off(u64 p)
{
    return (u32)(p & 0xFFFFFFFFu);
}

#define FT_PTR_NIL ((u64)0)

/* ---------------- 词的比较（排序与二分查找必须用同一个） ---------------- */

/*
 * 词的比较：先比字节前缀，长度不同则短的小。
 *
 * 等价于 strcmp（字节序，不是 locale 序）——
 * 字典的"有序"和二分查找的"有序"必须是同一个序，
 * 所以排序（qsort）和查找都用本函数。
 */
static inline int ft_term_cmp(const char *a, u32 alen, const char *b, u32 blen)
{
    u32 n = (alen < blen) ? alen : blen;
    int c = 0;

    if (n > 0)
        c = memcmp(a, b, (size_t)n);
    if (c != 0)
        return c;
    if (alen == blen)
        return 0;
    return (alen < blen) ? -1 : 1;
}

/* ---------------- 字典词条的编解码 ---------------- */

/*
 * 词条（变长，记录在字典页的 slot 里）：
 *
 *   u16 term_len
 *   u8  term[term_len]
 *   u32 df
 *   u64 post_ptr
 *
 * 顺序固定：长度 → 字节 → df → 指针。
 */
#define FT_DICT_HDR_BYTES  2u
#define FT_DICT_TAIL_BYTES 12u   /* df(4) + post_ptr(8) */

/* 词条总长（含 2 字节长度头） */
static inline u32 ft_dict_entry_bytes(u32 term_len)
{
    return FT_DICT_HDR_BYTES + term_len + FT_DICT_TAIL_BYTES;
}

/* 把词条写进 buf（buf 至少 ft_dict_entry_bytes(term_len) 字节） */
static inline void ft_dict_entry_encode(u8 *buf, const char *term, u32 term_len,
                                        u32 df, u64 post_ptr)
{
    put_u16(buf, (u16)term_len);
    memcpy(buf + FT_DICT_HDR_BYTES, term, (size_t)term_len);
    put_u32(buf + FT_DICT_HDR_BYTES + term_len, df);
    put_u64(buf + FT_DICT_HDR_BYTES + term_len + 4, post_ptr);
}

/* 从词条里取字段（buf 是 page_get 拿到的记录） */
static inline u32 ft_dict_entry_term_len(const u8 *buf)
{
    return get_u16(buf);
}

static inline u32 ft_dict_entry_df(const u8 *buf, u32 term_len)
{
    return get_u32(buf + FT_DICT_HDR_BYTES + term_len);
}

static inline u64 ft_dict_entry_post_ptr(const u8 *buf, u32 term_len)
{
    return get_u64(buf + FT_DICT_HDR_BYTES + term_len + 4);
}

/* ---------------- 倒排 run 的编解码 ---------------- */

static inline void ft_run_encode(u8 *buf, u32 first_doc, u16 n_post, u32 next_run)
{
    put_u32(buf + FTR_FIRST_DOC, first_doc);
    put_u16(buf + FTR_NPOST, n_post);
    put_u16(buf + FTR_FLAGS, 0);
    put_u32(buf + FTR_NEXT_RUN, next_run);
}

static inline u32 ft_run_first_doc(const u8 *buf)
{
    return get_u32(buf + FTR_FIRST_DOC);
}

static inline u16 ft_run_npost(const u8 *buf)
{
    return get_u16(buf + FTR_NPOST);
}

static inline u32 ft_run_next_run(const u8 *buf)
{
    return get_u32(buf + FTR_NEXT_RUN);
}

static inline void ft_posting_encode(u8 *buf, u32 delta, u32 tf, u32 dl)
{
    put_u32(buf + FTP_DELTA, delta);
    put_u32(buf + FTP_TF, tf);
    put_u32(buf + FTP_DL, dl);
}

static inline u32 ft_posting_delta(const u8 *buf) { return get_u32(buf + FTP_DELTA); }
static inline u32 ft_posting_tf(const u8 *buf)    { return get_u32(buf + FTP_TF); }
static inline u32 ft_posting_dl(const u8 *buf)    { return get_u32(buf + FTP_DL); }

#endif /* DB_MODALITIES_FT_INTERNAL_H */
