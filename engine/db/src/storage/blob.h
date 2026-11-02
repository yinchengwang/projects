#ifndef DB_STORAGE_BLOB_H
#define DB_STORAGE_BLOB_H

#include "common/config.h"
#include "common/error.h"
#include "common/loghook.h"
#include "common/types.h"
#include "storage/blob_hdr.h"
#include "storage/pager.h"

/*
 * blob 模态 —— 独立的大对象存储（v0.8）
 *
 * 【blob 模态是什么】
 *
 * 一个**对象**（object）：一串大的字节，逻辑上切成**等长的帧**（frame）。
 * 帧是 IO 的最小单位。读对象的第 N 帧，IO 量 ≈ 一帧（4KB），
 * **与对象有多大无关**。
 *
 * 这正是它和关系模态的大字段的区别：
 *
 *   关系模态的大字段（一行走 overflow 链）：
 *     读第 3000 个字节 → 要读前 3000 字节经过的所有页
 *     IO 量 = O(位置)
 *
 *   blob 模态：
 *     读第 3000 帧 → 只读那一帧所在的那一页
 *     IO 量 = O(1)
 *
 * 对标业界：S3/MinIO 的对象存储、MongoDB GridFS、
 * Parquet/Lance 的分块文件 —— 共同机制都是「对象 + 帧 + 随机访问」。
 *
 * 【为什么 blob 是独立模态，不是关系模态的一个类型】
 *
 * 1. 寻址方式不同：blob 用 blob_id（页号）寻址，关系模态用 RID (page, slot)。
 * 2. 生命周期不同：blob 对象不可变，没有 xmin/xmax，没有 MVCC。
 * 3. 页管理不同：blob 的页不进任何表的 freelist（表页是"能放下 N 字节的行"，
 *    blob 页是"一整页装一帧"，语义不同，混用会写坏数据）。
 *
 * 但它们共享底座：pager（缓冲池 + 页读）、WAL（崩溃安全）、
 * 页格式（20B 页头 + crc16 校验）。
 *
 * 【帧号 → 页号：纯算术】
 *
 * 这是本版最重要的设计点。
 *
 * 一个对象占**一段连续的页**：对象头页 + nframes 个帧页。
 * 因为对象是**一次写完**的（不可变对象），页是连续分配的。
 *
 *   第 N 帧 → 页号 base_page + N
 *
 * 于是读第 N 帧**不需要查任何索引** —— 一次算术运算 + 一次页读。
 * IO 量 = 4KB ✓（正好是验收标准要的）
 *
 * 【帧索引为什么还要建】
 *
 * 因为"页连续"是**当前实现恰好成立**，不是本质约束。
 * 一旦支持增量写（对象边写边扩），页就不连续了，
 * 那时就需要一张「帧号 → 页号」的映射表。
 *
 * 所以本版把映射表的**数据结构**建好（索引块），
 * 只是快路径绕开了它。
 * 这是"为扩展留好接口，但不提前付代价"。
 *
 * 【不可变对象带来的额外好处】
 *
 * 每个页只写一次 ⇒ 日志不需要区分"首次修改记全页 / 后续只记增量"
 * ⇒ 直接用 WAL_PAGEDATA 整页镜像即可，**恢复逻辑零改动**。
 */

/* 默认帧大小。帧 = 一页的数据区（页头 20B 之外的部分） */
#define BLOB_FRAME_SIZE   (PAGE_SIZE - HDR_SIZE)

/*
 * 一个索引块覆盖多少帧。
 *
 * 每个索引项 4B（页号），一页能放 ~1019 项 → 取 1024 帧/块。
 * 1GB 对象 / 4KB = 262144 帧 = 256 个索引块。
 */
#define BLOB_FRAMES_PER_INDEX 1024u

/*
 * 对象头页的布局见 storage/blob_hdr.h（v0.9 拆成了独立文件）。
 *
 * 一句话：magic / version / nframes / frame_size / total_len /
 * base_page / n_index_blk / index_blk[] / hdr_crc。
 */
/*
 * 对象 id = 对象头页的页号 **+ 1**。
 *
 * 【为什么要 +1】
 *
 * 第一个对象的头页是第 0 页，如果 blob_id 直接用页号，
 * 首个对象的 id 就是 0 —— 和「无效 id」无法区分。
 *
 * 而 id=0 必须能表达"没有对象"，因为 API 的常见用法是
 * "先查有没有，有才用"。让 id=0 天然表示无效，省掉一堆特判。
 *
 * 所以：**blob_id = 页号 + 1**，反解是 **页号 = blob_id - 1**。
 * blob_id → base_page 依然是 O(1) 算术。
 */
typedef u64 BlobId;

#define BLOB_ID_INVALID  0ull

/* 观察用的统计 */
typedef struct {
    u64 n_objects;
    u64 n_frames;
    u64 bytes_written;
    u64 bytes_read;
    u64 page_reads;      /* 读帧引起的页读次数 —— 用来验证 IO 量 */
} BlobStats;

typedef struct BlobStore BlobStore;

/*
 * 创建 blob 存储。
 *
 * fn/ctx 是 WAL 钩子。blob 页必须写日志，
 * 否则崩溃后大对象会丢（页内容在盘上，日志里没有 → 重放不回来）。
 */
int  blob_store_create(Pager *pg, LogHookFn fn, void *ctx, BlobStore **out);
void blob_store_destroy(BlobStore *bs);

/*
 * 创建一个对象：分配 1 个头页 + nframes 个帧页。
 *
 * 页是连续分配的（一次 pager_alloc 到底），
 * 这让"帧号 → 页号"变成纯算术。
 *
 * blob_id_out 拿到对象 id（= 头页页号）。
 */
int  blob_create(BlobStore *bs, u64 nframes, u32 frame_size, BlobId *blob_id_out);

/*
 * 写整个对象。
 *
 * 对象不可变 —— 一次写完。len <= nframes * frame_size。
 * 写完最后一帧没写满的部分，视为填充 0。
 *
 * xid 显式传 —— WAL 日志里的 xid 必须是真正干活的这个事务。
 */
int  blob_write(BlobStore *bs, u32 xid, BlobId id,
                const u8 *data, u64 len);

/*
 * 读第 frame_no 帧。
 *
 * buf 容量至少 frame_size。*len_out 是这一帧实际有多少字节
 * （最后一帧可能不满）。
 *
 * IO 量：1 次页读（头页 + 帧页是 2 次，头页可缓存）。
 */
int  blob_read_frame(BlobStore *bs, BlobId id, u64 frame_no,
                     u8 *buf, u32 cap, u32 *len_out);

/*
 * 读整个对象。
 *
 * buf 容量至少 total_len。
 */
int  blob_read(BlobStore *bs, BlobId id, u8 *buf, u64 cap, u64 *len_out);

/* 对象元信息 */
int  blob_stat(BlobStore *bs, BlobId id,
               u64 *nframes, u32 *frame_size, u64 *total_len);

/*
 * 取对象头的完整信息（多一个 base_page）。
 *
 * base_page 是帧数据的起始页号。
 * 【为什么 blob_stat 不给 base_page】
 * base_page 是**实现细节**（帧从哪一页开始），一般用不上；
 * 需要它的场合（外部引用 = 文件偏移）单独提供一个函数，
 * 免得每个调用者都多拿一个用不上的字段。
 */
int  blob_header(BlobStore *bs, BlobId id,
                 u64 *nframes, u32 *frame_size, u64 *total_len, u32 *base_page);

/*
 * 取第 frame_no 帧的**定位信息**（不读帧内容）。
 *
 * off_out  这一帧数据在数据文件中的字节偏移
 *           = (base_page + frame_no) * PAGE_SIZE + HDR_SIZE
 * len_out  这一帧实际有多少字节（最后一帧可能不满）
 *
 * 【这是"外部引用"】
 * offset + length 就是业界说的外部定位信息 ——
 * Parquet 的 column chunk、ClickHouse 的外部字符串都是这个形态。
 * 拿到它，任何人都能直接去文件里读，不必经过数据库。
 *
 * 注意：这里**不读帧页**，只读对象头（头页会被缓冲池缓存）。
 */
int  blob_frame_info(BlobStore *bs, BlobId id, u64 frame_no,
                     u64 *off_out, u32 *len_out);

/* 观察用 */
const BlobStats *blob_stats(const BlobStore *bs);

/*
 * 列出所有对象。
 *
 * 【为什么 blob 有自己的目录，而不用 catalog】
 *
 * catalog 是**关系模态**的元数据（表、列、索引）。
 * 如果 blob 依赖它，就等于让 blob 模态依赖关系模态 ——
 * 模态之间互相依赖是 SPI 明确禁止的（"模态之间不许互相 include"）。
 *
 * 所以 blob 自己有一个目录页，记录所有对象的 id。
 *
 * 目录页是 blob 模态的私有页（PT_BLOB 类型，带目录标志）。
 * 它的页号固定存放在 BlobStore 里（内存），重启后靠扫描重建。
 */
u32  blob_count(const BlobStore *bs);

/* 取第 i 个对象的 id。i 越界返回 BLOB_ID_INVALID。 */
BlobId blob_id_at(const BlobStore *bs, u32 i);

#endif /* DB_STORAGE_BLOB_H */
