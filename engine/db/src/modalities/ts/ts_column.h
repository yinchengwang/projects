#ifndef DB_MODALITIES_TS_TS_COLUMN_H
#define DB_MODALITIES_TS_TS_COLUMN_H

#include "common/error.h"
#include "common/types.h"
#include "modalities/ts/tsstore.h"
#include "storage/pager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ts_column.h —— 时序分区的**页格式与读路径**（v1.5 步骤 3）
 *
 * 【这一层解决什么】
 *
 * 步骤 2 的活跃分区是**裸写**：定长 24 字节一行（time i64 + tag i64 + value f64），
 * 一页 169 行。裸写简单，但压不动 —— 一行 24 字节里，
 * 时间戳是 8 字节绝对值、值是 8 字节原始位模式。
 *
 * 封窗之后把分区转成**列式**：三列各自编码（时间 dod / 标签 rle / 值 gorilla），
 * 各占一段连续页 ⇒ 同样的数据占的页数**明显更少**。
 *
 * 【这一层为什么同时管"裸写页"和"列式页"】
 *
 * 两种页格式是同一份数据的**两种存法**，读路径要按分区状态二选一。
 * 格式常量散在两个文件里 ⇒ 迟早有一处写错（页内偏移对不上 ⇒ 读到垃圾）。
 * 所以格式常量**统一放在本头文件**，实现放在 ts_column.c：
 *   - 裸写页（活跃分区）：PT_TS_PART
 *   - 列式页（封窗分区）：PT_TS_COL
 *
 * 【列式页的磁盘格式】
 *
 *   头部页（1 张，PT_TS_COL + HEADER 标志）
 *     记：版本、行数、三列各自的（编码字节数 / 页数 / 首页页号 / 条数）、
 *         被换掉的旧裸写页（首页 + 页数，仅供将来回收）
 *   数据页（若干张，PT_TS_COL + DATA 标志）
 *     记：列号、本页有效字节数；数据区从偏移 40 起（每页 4056 字节）
 *     每列一条链（H_NEXT 串起来）
 *
 * ⚠️ **段头必须记条数**（坑 **M6-9** 的教训，坑 **M6-39**）：
 * 三个编码（dod / rle / gorilla）的格式里**没有条数字段** ——
 * "声明的条数比实际多"只能在"位不够"时被抓到。
 * 所以行数记在**头部页**里（分区行数 + 每列的条数），解码时逐列核对：
 * 解出来的条数与头部页记的不一致 ⇒ E_CORRUPT。
 *
 * 【为什么列式页单独一个页类型】
 *
 * 页类型是**全局**的（坑 M6-27）：PT_RTREE=0x15 / PT_GEO_META=0x16 /
 * PT_TS_META=0x17 / PT_TS_DIR=0x18 / PT_TS_PART=0x19 已占用 ⇒ 本层取 0x1A。
 * 读路径靠页类型区分"裸写分区页"与"列式页"，不会把两种格式读串。
 */

/* ---------------- 裸写分区页（活跃分区） ---------------- */

/* 页类型：接在 PT_TS_DIR(0x18) 之后 */
#define PT_TS_PART 0x19u

/* 裸写分区页的页内布局（自定义字段从偏移 20 = HDR_SIZE 起） */
enum {
    TSSP_NROWS = 20,   /* u32 本页行数 */
    TSSP_NEXT  = 24,   /* u32 下一张数据页 */
    TSSP_DATA  = 28    /* 行数组起点 */
};

/* 行：time i64 + tag i64 + value f64 */
#define TS_ROW_SIZE      24u
#define TS_ROWS_PER_PAGE ((PAGE_SIZE - TSSP_DATA) / TS_ROW_SIZE)  /* 169 */

/* ---------------- 列式页（封窗分区） ---------------- */

/* 页类型：接在 PT_TS_PART(0x19) 之后 */
#define PT_TS_COL 0x1Au

/* 列式格式版本 */
#define TS_COL_VER 1u

/* 页标志 */
#define TS_COL_FLAG_HEADER 0x01u
#define TS_COL_FLAG_DATA   0x02u

/* 列号 */
enum {
    TSCOL_TIME = 0,   /* 时间列（dod） */
    TSCOL_TAG = 1,    /* 标签列（rle） */
    TSCOL_VALUE = 2,  /* 值列（gorilla） */
    TSCOL_NCOL = 3    /* 列数 */
};

/*
 * 页内布局（头部页与数据页共用前 40 字节，之后按标志位分）。
 *
 * 头部页：
 *   20 TSC_VER      u32 格式版本
 *   24 TSC_FLAGS    u32 页标志（HEADER）
 *   28 TSC_NROWS    u32 分区行数
 *   32 TSC_NPAGES   u32 数据页总数（不含头部页）
 *   36 TSC_TLEN     u32 时间列字节数
 *   40 TSC_TPAGES   u32 时间列页数
 *   44 TSC_GLEN     u32 标签列字节数
 *   48 TSC_GPAGES   u32 标签列页数
 *   52 TSC_VLEN     u32 值列字节数
 *   56 TSC_VPAGES   u32 值列页数
 *   60 TSC_TFIRST   u32 时间列首页页号
 *   64 TSC_GFIRST   u32 标签列首页页号
 *   68 TSC_VFIRST   u32 值列首页页号
 *   72 TSC_OLDFIRST u32 被换掉的旧裸写页首页（仅供回收；本步不回收）
 *   76 TSC_OLDPAGES u32 旧裸写页页数
 *   80 TSC_SEALSEQ  u32 封窗序号（单调递增；没有 WAL ⇒ 没有 LSN 可记）
 *   84 TSC_TCOUNT   u32 时间列条数
 *   88 TSC_GCOUNT   u32 标签列条数
 *   92 TSC_VCOUNT   u32 值列条数
 *
 * 数据页：
 *   20 TSC_VER    u32 格式版本
 *   24 TSC_FLAGS  u32 页标志（DATA）
 *   28 TSC_COL    u32 列号
 *   32 TSC_CLEN   u32 本页有效字节数
 *   36 TSC_CCAP   u32 本页容量
 *   40 TSC_DATA   数据区起点
 */
enum {
    TSC_VER      = 20,
    TSC_FLAGS    = 24,
    TSC_NROWS    = 28,
    TSC_NPAGES   = 32,
    TSC_TLEN     = 36,
    TSC_TPAGES   = 40,
    TSC_GLEN     = 44,
    TSC_GPAGES   = 48,
    TSC_VLEN     = 52,
    TSC_VPAGES   = 56,
    TSC_TFIRST   = 60,
    TSC_GFIRST   = 64,
    TSC_VFIRST   = 68,
    TSC_OLDFIRST = 72,
    TSC_OLDPAGES = 76,
    TSC_SEALSEQ  = 80,
    TSC_TCOUNT   = 84,
    TSC_GCOUNT   = 88,
    TSC_VCOUNT   = 92,

    TSC_COL      = 28,   /* u32 列号（数据页） */
    TSC_CLEN     = 32,   /* u32 本页有效字节数（数据页） */
    TSC_CCAP     = 36,   /* u32 本页容量（数据页） */
    TSC_DATA     = 40    /* 数据区起点（数据页） */
};

/* 一个数据页能装多少字节 */
#define TSC_CAP ((u32)(PAGE_SIZE - TSC_DATA))   /* 4056 */

/* ---------------- 裸写分区页的读路径 ---------------- */

/*
 * 走一遍裸写分区的页链：数行数、数页数、找链尾（开库时用）。
 *
 * 页类型不对 / 校验和不过 / 行数越界 ⇒ E_CORRUPT（**绝不**读出半截分区）。
 * reads —— 累计逻辑页读（可为 NULL）。
 */
int ts_part_walk(Pager *pg, u32 first_page, u32 *rows_out, u32 *pages_out,
                 u32 *last_out, u64 *reads);

/* 扫描一个裸写分区：逐行回调（回调返回非 0 则停止并原样返回）。 */
int ts_part_scan(Pager *pg, u32 first_page, TsReadFn fn, void *ctx, u64 *reads);

/*
 * 把一个裸写分区的全部行读进三个数组（封窗时用）。
 *
 * max_rows —— 数组能装多少行；分区实际行数 > max_rows ⇒ E_CORRUPT
 * （说明调用方手里的行数不对 —— 与"段头必须记条数"同一个原则）。
 */
int ts_part_read_rows(Pager *pg, u32 first_page, u32 max_rows,
                      i64 *ts, i64 *tag, double *val, u32 *rows_out, u64 *reads);

/* ---------------- 列式页 ---------------- */

/*
 * 写一个分区的列式页。
 *
 * ts / tag / val —— 三列，各 n 个值（调用方保证同长）。
 * old_first / old_pages —— 被换掉的旧裸写页（记进头部页，本步不回收）。
 * seal_seq —— 封窗序号（单调递增）。
 *
 * head_out —— 头部页页号；pages_out —— 本次新占的页数（含头部页）。
 *
 * 返回 OK / E_NOSPACE / E_CORRUPT / E_INVAL。
 *
 * ⚠️ 本函数**只写页，不改目录**：目录翻转是第二步（ts_store_seal_commit）。
 * 两步分开，是为了让"写了一半"的状态可以被测试复现（见 ts_seal.h）。
 */
int ts_column_write(Pager *pg, const i64 *ts, const i64 *tag, const double *val,
                     u32 n, u32 old_first, u32 old_pages, u32 seal_seq,
                     u32 *head_out, u32 *pages_out);

/*
 * 读一个列式分区：解码回三列（调用方给好缓冲，各 n 个）。
 *
 * 返回 OK / E_CORRUPT（页坏了、条数对不上）。
 */
int ts_column_read(Pager *pg, u32 head_page, i64 *ts, i64 *tag, double *val, u32 n);

/*
 * 扫描一个列式分区：解码后逐行回调（与 ts_store_scan 的回调同型）。
 *
 * 返回 OK / E_CORRUPT；回调返回非 0 则停止并原样返回。
 */
int ts_column_scan(Pager *pg, u32 head_page, u32 n_rows, TsReadFn fn, void *ctx,
                   u64 *reads);

/*
 * 只读头部页：拿行数与页数（开库时用）。
 *
 * 返回 OK / E_CORRUPT。
 */
int ts_column_info(Pager *pg, u32 head_page, u32 *n_rows, u32 *n_pages);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_TS_TS_COLUMN_H */
