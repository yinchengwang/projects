#ifndef DB_MODALITIES_TS_TS_AM_MAT_H
#define DB_MODALITIES_TS_TS_AM_MAT_H

#include <stdbool.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/ts/tsstore.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ts_am_mat.h —— 时序虚拟表的**物化**（v1.5 步骤 5）
 *
 * 【为什么要单独一个文件】
 *
 * ts_am.c 要同时装：AM 的构造、扫描、解码、谓词下推、可见性、写路径。
 * 全塞在一起就超了 800 行的上限（新增文件的硬上限）。
 * 而"把物理形态摊平成行"这件事本身是**独立的一块** ——
 * 拆出来之后，AM 层只管"怎么把行交出去"，物化层只管"行从哪来"。
 *
 * 【物化是什么】
 *
 * ts_readings / ts_partitions 都是**视图**：行不是存出来的，
 * 是每次查询现算的。物化 = 一次性把这一批行摊平成
 * "每行 row_bytes 字节"的连续块，扫描时按序吐出去。
 *
 * 与 ft_docs / geo_points 同一个取舍：这是**元数据路径**，不是热路径。
 */

/* ---------------- 一行的字节数 ---------------- */

#define TS_READING_ROW_BYTES 24u
#define TS_PART_ROW_BYTES    40u

/* 读数行内偏移 */
#define TS_ROW_TIME_OFF 0u
#define TS_ROW_TAG_OFF  8u
#define TS_ROW_VAL_OFF  16u

/* 分区行内偏移 */
#define TS_PROW_START_OFF 0u
#define TS_PROW_END_OFF   8u
#define TS_PROW_STATE_OFF 16u
#define TS_PROW_ROWS_OFF  24u
#define TS_PROW_PAGES_OFF 32u

/*
 * 一次物化的结果：行字节流 + 裁剪用的分区区间表。
 *
 * 行是变长吗？不是 —— 两种表各自的行宽是固定的，
 * 所以这里存的是"每行 row_bytes 字节"的连续块。
 */
typedef struct {
    u8 *rows;        /* 行字节流 */
    u32 nrows;
    u32 cap_rows;
    u32 row_bytes;

    /* 选中的分区区间（按 start 升序）；nparts == 0 ⇒ 不裁剪 */
    i64 *pstart;
    i64 *pend;
    u32  nparts;
    u32  cap_parts;
} TsAmMat;

/* 初始化（row_bytes 决定行宽） */
void ts_am_mat_init(TsAmMat *m, u32 row_bytes);

/* 释放（可以重复调用） */
void ts_am_mat_free(TsAmMat *m);

/*
 * 物化读数。
 *
 * has_range == false ⇒ 全部分区（超集语义）。
 * has_range == true  ⇒ 只取与 [lo, hi] 相交的分区（区间相交，不是包含）。
 */
int ts_am_mat_readings(TsStore *st, bool has_range, i64 lo, i64 hi, TsAmMat *m);

/* 物化分区目录（**不裁剪** —— 分区表就是目录本身） */
int ts_am_mat_partitions(TsStore *st, TsAmMat *m);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_TS_TS_AM_MAT_H */
