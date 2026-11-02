#ifndef DB_STORAGE_BLOB_HDR_H
#define DB_STORAGE_BLOB_HDR_H

#include "common/config.h"
#include "common/types.h"

/*
 * blob 对象头的格式（v0.9 拆文件）
 *
 * 【为什么要拆出来】
 * 硬规则：单文件不超过 800 行。对象头的编解码是**自成一块**的格式知识
 * （对齐 record.c 之于 tuple 格式的做法）。
 *
 * 【对象头里存什么】
 *
 *   magic       u32   "BLOB"
 *   version     u32
 *   nframes     u64   总帧数
 *   frame_size  u32   每帧字节数
 *   total_len   u64   实际数据总长（<= nframes * frame_size）
 *   base_page   u32   帧数据起始页号（= 头页页号 + 1）
 *   n_index_blk u32   索引块个数
 *   index_blk[] u32   每个索引块的页号
 *   hdr_crc     u32   上面所有字段的 crc32
 *
 * 布局写在页头之后（BLOB_HDR_OFF = HDR_SIZE）。
 */

#define BLOB_MAGIC        0x424C4F42u   /* "BLOB" */
#define BLOB_HDR_VERSION  1u

/* 对象头写在页头之后（避开 slotted page 的页头） */
#define BLOB_HDR_OFF  HDR_SIZE

/* 对象头里能存多少个索引块页号 */
#define BLOB_MAX_INDEX_BLOCKS 64u

/* 字段偏移（相对 BLOB_HDR_OFF） */
#define BH_MAGIC       0
#define BH_VERSION     4
#define BH_NFRAMES     8
#define BH_FRAME_SIZE  16
#define BH_TOTAL_LEN   20
#define BH_BASE_PAGE   28
#define BH_N_INDEX_BLK 32
#define BH_INDEX_BLK   36
/* BH_HDR_CRC 在 index_blk[] 之后，偏移 = 36 + 4 * n_index_blk */

/* 到 index_blk[] 之前的固定部分大小 */
#define BLOB_HDR_FIXED_SIZE 44u

/* 解析出来的对象头 */
typedef struct {
    u64 nframes;
    u32 frame_size;
    u64 total_len;
    u32 base_page;
    u32 n_index_blk;
    u32 index_blk[BLOB_MAX_INDEX_BLOCKS];
} BlobHdr;

/* 解析对象头（pg 指向整页）。magic/version/crc 不对返回 E_CORRUPT。 */
int  blob_hdr_parse(const u8 *pg, BlobHdr *h);

/* 写对象头到整页（pg 指向整页） */
void blob_hdr_write(u8 *pg, const BlobHdr *h);

#endif /* DB_STORAGE_BLOB_HDR_H */
