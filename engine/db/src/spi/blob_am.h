#ifndef DB_SPI_BLOB_AM_H
#define DB_SPI_BLOB_AM_H

#include "spi/storage_am.h"
#include "storage/blob.h"

/*
 * blob 模态的 SPI 适配（v0.9）。
 *
 * 【blob 在 SQL 里长什么样】
 *
 * 两张虚拟表：
 *
 *   blob_frames   一行 = 一帧
 *                 blob_id / frame_no / data_len / data_off / data
 *
 *   blob_objects  一行 = 一个对象
 *                 blob_id / nframes / frame_size / total_len
 *
 * 为什么两张而不是一张：对象级元信息（帧数 / 帧大小 / 总长）
 * 放在帧表里是每行冗余一遍。一张表要么冗余要么缺，两张各自干净。
 * 顺带证明了一件事：**一个模态可以提供多张表**。
 *
 * 【RID】
 *
 *   blob_frames   RID = (页号 = base_page + frame_no, slot = 0)
 *   blob_objects  RID = (头页页号 = blob_id - 1, slot = 0)
 *
 * 每张表有自己的 RID 空间，互不相干。
 *
 * 【为什么"帧"才是行，而不是"对象"】
 *
 * 帧是 blob 唯一的可寻址单位。对象是帧的集合，RID 要能定位到
 * 最小可读单位 —— 那就是帧。
 *
 * 粒度上也和别的模态对齐：图模态的一行 = 一个节点，
 * 向量模态的一行 = 一个向量，blob 的一行 = 一帧。
 * 都是"该模态的最小可寻址单元"。
 *
 * 【data 列是外部值】
 *
 * 一帧 4076 字节，塞不进 SQL 的标量值（文本上限 255）。
 * 所以 data 挂的是**外部引用**：句柄 = (blob_id, frame_no)，
 * 内容被需要时才读（见 storage/value.h 的外部值说明）。
 *
 * 于是：
 *   SELECT blob_id, frame_no FROM blob_frames   ← 不读帧页
 *   SELECT data FROM blob_frames                ← 才读帧页
 */

/* AM 的私有状态 */
typedef struct {
    BlobStore *bs;
    bool       is_frames;     /* true = 帧表；false = 对象表 */
} BlobAmCtx;

/* 构造帧表的 AM */
int blob_am_build_frames(BlobStore *bs, StorageAM *out, BlobAmCtx *ctx);

/* 构造对象表的 AM */
int blob_am_build_objects(BlobStore *bs, StorageAM *out, BlobAmCtx *ctx);

#endif /* DB_SPI_BLOB_AM_H */
