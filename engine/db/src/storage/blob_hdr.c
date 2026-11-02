/*
 * blob_hdr.c — blob 对象头的编解码（v0.9 拆文件）
 *
 * 设计说明见 blob_hdr.h。
 */

#include "storage/blob_hdr.h"

#include <string.h>

#include "common/crc.h"
#include "common/error.h"
#include "storage/blob.h"

/* 对象头里除了 crc 之外的所有字节都要参与校验 */
static u32 blob_hdr_crc(const u8 *hdr)
{
    u32 n_index_blk = get_u32(hdr + BH_N_INDEX_BLK);
    u32 crc_off = BH_INDEX_BLK + 4u * n_index_blk;
    /* crc 字段本身不参与计算 */
    return crc32(hdr, crc_off);
}

int blob_hdr_parse(const u8 *pg, BlobHdr *h)
{
    const u8 *hdr = pg + BLOB_HDR_OFF;

    if (get_u32(hdr + BH_MAGIC) != BLOB_MAGIC)
        return E_CORRUPT;
    if (get_u32(hdr + BH_VERSION) != BLOB_HDR_VERSION)
        return E_CORRUPT;

    u32 n_index_blk = get_u32(hdr + BH_N_INDEX_BLK);
    if (n_index_blk > BLOB_MAX_INDEX_BLOCKS)
        return E_CORRUPT;

    /* 校验和：对象头损坏 → 整个对象不可用，必须报错而不是返回垃圾 */
    u32 crc_off = BH_INDEX_BLK + 4u * n_index_blk;
    if (get_u32(hdr + crc_off) != blob_hdr_crc(hdr))
        return E_CORRUPT;

    h->nframes     = get_u64(hdr + BH_NFRAMES);
    h->frame_size  = get_u32(hdr + BH_FRAME_SIZE);
    h->total_len   = get_u64(hdr + BH_TOTAL_LEN);
    h->base_page   = get_u32(hdr + BH_BASE_PAGE);
    h->n_index_blk = n_index_blk;
    for (u32 i = 0; i < n_index_blk; i++)
        h->index_blk[i] = get_u32(hdr + BH_INDEX_BLK + 4u * i);

    return OK;
}

void blob_hdr_write(u8 *pg, const BlobHdr *h)
{
    u8 *hdr = pg + BLOB_HDR_OFF;

    put_u32(hdr + BH_MAGIC, BLOB_MAGIC);
    put_u32(hdr + BH_VERSION, BLOB_HDR_VERSION);
    put_u64(hdr + BH_NFRAMES, h->nframes);
    put_u32(hdr + BH_FRAME_SIZE, h->frame_size);
    put_u64(hdr + BH_TOTAL_LEN, h->total_len);
    put_u32(hdr + BH_BASE_PAGE, h->base_page);
    put_u32(hdr + BH_N_INDEX_BLK, h->n_index_blk);
    for (u32 i = 0; i < h->n_index_blk; i++)
        put_u32(hdr + BH_INDEX_BLK + 4u * i, h->index_blk[i]);

    u32 crc_off = BH_INDEX_BLK + 4u * h->n_index_blk;
    put_u32(hdr + crc_off, blob_hdr_crc(hdr));
}
