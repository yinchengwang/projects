#ifndef DB_MODALITIES_VECTOR_VSTORE_H
#define DB_MODALITIES_VECTOR_VSTORE_H

#include "common/config.h"
#include "common/error.h"
#include "common/loghook.h"
#include "common/types.h"
#include "storage/gstore.h"
#include "storage/pager.h"

/*
 * vstore —— 向量模态的列式存储（v1.0）
 *
 * 【列式是什么，以及为什么是列式】
 *
 * 关系模态按**行**存：一行 = 若干列，行内字段挨着。
 * 向量模态按**列**存：一页连续放 K 个向量，同一维度的分量挨着。
 *
 *   行式（slotted page）：  128 维 fp32 = 512B，一页只能放 7 个向量，
 *                          而且相邻向量分散在页的各处
 *   列式（本文件）：        一页 7 个向量**连续**排列，
 *                          第 i 个向量的位置 = 页头 + i × dim × 4
 *
 * 列式赢在三件事：
 *   1. 顺序扫描时一页一次 IO 就拿到 K 个向量（不是 K 次）
 *   2. 将来做量化/训练时要按维度遍历，列式是连续访存
 *   3. 第 i 个向量和第 i 行标量属性靠**序号**对应，不需要指针
 *
 * 对标：Lance / Parquet 的列式布局，Milvus 的 segment 存储。
 *
 * 【页布局】
 *
 *   向量页（PT_VEC_DATA）：
 *     0  u8  页类型
 *     1  u8  标志（bit0 = 链头）
 *     2  u16 本页向量数
 *     4  u32 下一页
 *     8  u32 本页序号（= 第几个数据页，用来校验寻址）
 *     12 u32 LSN          ← H_LSN，所有页类型共用
 *     16 u16 crc16        ← H_CKSUM，所有页类型共用
 *     18 u16 保留
 *     20 ... 向量 0 | 向量 1 | ... （每个 dim × 4 字节，连续）
 *
 * 【为什么页头是 20B，而不是规格里写的 8B】
 *
 * `向量模态实现规格.html` 写的是"页头 8B（页类型 + 向量计数）"。
 * 按 8B 写会**直接压坏两个公共字段**：
 *
 *   H_LSN   = 12  —— 重放幂等判断读它（`page_lsn`）
 *   H_CKSUM = 16  —— 校验和固定写在这里（`page_set_checksum`）
 *
 * 这两个偏移是**所有页类型共用**的（config.h 里写死），
 * 不归某个页类型自己支配。页头取 8B 的话，第 0 个向量就从偏移 8 开始，
 * 它的前 10 个字节正好被 LSN 和校验和覆盖。
 *
 * 后果和 gstore 那次的坑一模一样：编译不报错、测试也可能过，
 * 只是**向量数据被静默改写**（而且只坏每页的第一个向量）。
 *
 * 所以页头取 20B（= HDR_SIZE），和数据区从 20 开始 ——
 * 与 slotted page、定长槽页保持同一个约定。
 * 代价：K 从 7.98 变成 7.96，对 128 维**没有影响**（都是 7）。
 *
 * 头页（PT_VEC_HDR）同理，字段全部避开 12..17：
 *     0  u8  页类型        20 u32 第一个数据页
 *     1  u8  版本          24 u32 最后一个数据页
 *     2  u16 dim           28 u32 数据页数
 *     4  u16 k（每页向量数）
 *     6  u16 保留
 *     8  u32 保留
 *     12 u32 LSN / 16 u16 crc16 / 18 u16 保留
 *
 * 【寻址：row_id → 位置，纯算术】
 *
 *   row_id = 全局序号（单调递增，不复用）
 *   序号   = row_id / k        第几个数据页
 *   槽号   = row_id % k        页内第几个向量
 *   偏移   = 20 + 槽号 × dim × 4
 *
 * 【内存目录：为什么向量要它，blob 不要】
 *
 * blob 的对象是**一次写完**的，写的时候没有别的东西插进来分配页，
 * 所以一个对象的帧页是**连续**的 → `帧号 → 页号` 是加法。
 *
 * 向量不是：每插 k 个向量才分配一个数据页，而**标量页会在中间插进来**
 * （标量表是 gstore，它自己也会分配页）。所以数据页的页号**不连续**，
 * `序号 → 页号` 必须查表。
 *
 * 这张表放在内存里（`pages[]`），打开时顺着页链走一遍重建 ——
 * 和 blob 重建目录、gstore 重算 alive 是同一个套路：
 * **磁盘上只存链，索引在内存里重建**。
 *
 * 代价：打开时要读一遍数据页（10 万向量 ≈ 1.4 万页）。
 * 好处：查询路径上 `row_id → 页号` 是 O(1) 数组下标，一次页读都不用。
 *
 * 【列对齐：本模态最容易静默错的地方】
 *
 * 第 i 个向量 ↔ 标量表第 i 行，靠**序号**对应，没有任何指针。
 * 一旦序号错位（插入跳过了一个 row_id、或者回收复用了序号），
 * 查出来的向量和它的 label 就是**错配**的 —— 不报错，只是答案是错的。
 *
 * 防线（对应 MODALITIES.md 坑 M3-2）：
 *   1. row_id 全局单调递增，**永不复用**（本版只插入，不删除）
 *   2. 标量记录里**显式存 row_id**，读的时候断言它等于序号
 *   3. 向量页头里存本页序号，重建时断言链序 == 序号
 *
 * 【本版只追加，不改写】
 *
 * 向量写进去就不再改（没有 UPDATE）。所以：
 *   - 数据页一旦写满就再也不碰
 *   - 不需要空闲链、不需要 compact
 *   - 删除留到 v1.2（标记删除 + 重建索引）
 *
 * 这条约束让"崩溃恢复"简单很多：数据页是只追加的，
 * 恢复后按页头里的 count 就知道每页有几个向量。
 */

/* 向量页页头 = HDR_SIZE（见上面"为什么不是 8B"） */
#define VEC_PAGE_HDR  HDR_SIZE

/* 每页能放多少个向量。整数除法天然取下界 —— 坑 M3-1 */
#define VEC_K(dim)  ((u32)((PAGE_SIZE - VEC_PAGE_HDR) / ((u32)(dim) * 4u)))

/*
 * 标量记录：载荷 = row_id(8) + label(4) = 12 字节。
 *
 * 【注意：这不是 gstore 的记录长度】
 *
 * gstore 的槽布局是 [in_use(1)][保留(1)][next_free(2)][载荷…] ——
 * 前 4 个字节是 gstore 自己管的公共头，**不归记录内容**。
 * 所以 gstore 的 rec_size 必须是 4 + 12 = 16。
 *
 * 这两件事搞混的后果（v0.11 已经踩过一次，坑 M2-6）：
 *   - rec_size 写 12 → 载荷只剩 8 字节，label 被静默截断
 *   - 读的时候从第 0 字节取 row_id → 取到的是公共头 + row_id 的前 4 字节
 * 两种都不报错，只是数据错。
 */
#define VEC_META_PAYLOAD  12u
#define VEC_META_REC      (GS_REC_HDR_SIZE + VEC_META_PAYLOAD)

/* 向量页页头字段偏移 */
enum {
    VEC_H_TYPE    = 0,   /* u8  页类型 */
    VEC_H_FLAGS   = 1,   /* u8  标志 */
    VEC_H_COUNT   = 2,   /* u16 本页向量数 */
    VEC_H_NEXT    = 4,   /* u32 下一页 */
    VEC_H_ORDINAL = 8,   /* u32 本页序号 */
    VEC_H_LSN     = H_LSN,
    VEC_H_CKSUM   = H_CKSUM
};

/* 头页字段偏移 */
enum {
    VECH_H_TYPE   = 0,
    VECH_H_VER    = 1,
    VECH_H_DIM    = 2,
    VECH_H_K      = 4,
    VECH_H_HEAD   = 20,
    VECH_H_TAIL   = 24,
    VECH_H_NPAGES = 28
};

#define VEC_HDR_VER 1u

/* 链头标志（与 gstore 的 GS_FLAG_DIR 同款：靠它找到链的起点） */
#define VEC_FLAG_HEAD 0x01u

typedef struct VStore VStore;

/*
 * 建向量存储（或接管磁盘上已有的）。
 *
 * dim 是向量维度，**是存储的身份**：磁盘上已有存储且维度不同 → E_INVAL。
 * 理由同图的属性 schema：维度决定每页放几个向量、每个向量多长，
 * 改了维度旧数据就解释不了。
 */
int  vstore_create(Pager *pg, LogHookFn fn, void *ctx, u16 dim, VStore **out);

/*
 * 打开磁盘上已有的向量存储。没有 → E_NOTFOUND。
 *
 * 打开时做三件事：读头页拿 dim/链头 → 顺链重建内存目录 → 重建标量表的 RID 表。
 */
int  vstore_open(Pager *pg, LogHookFn fn, void *ctx, VStore **out);

void vstore_destroy(VStore *vs);

/*
 * 追加一个向量。row_id 由本函数分配（= 当前总数）。
 *
 * vec 必须指向 dim 个 float。label 是标量属性（供过滤用）。
 *
 * 写路径：1 个数据页 + 1 个标量槽。**每 k 次插入才多写一个头页**
 * （只有分配新数据页时头页才变）。
 */
int  vstore_insert(VStore *vs, u32 xid, const float *vec, i32 label,
                   u64 *row_id_out);

/*
 * 取第 row_id 个向量。out 必须能放 dim 个 float。
 * 找不到 → E_NOTFOUND。
 */
int  vstore_get(VStore *vs, u64 row_id, float *out);

/* 取第 row_id 个向量的标量属性（label） */
int  vstore_label(VStore *vs, u64 row_id, i32 *out);

/*
 * 顺序扫描全部向量（列式的快路径：一页一次 IO）。
 *
 * fn 返回非 0 时提前结束，那个返回值原样透传出去。
 */
typedef int (*VecScanFn)(void *ctx, u64 row_id, const float *vec, i32 label);
int  vstore_scan(VStore *vs, VecScanFn fn, void *ctx);

/*
 * 把全部向量读进一块连续内存。
 *
 * buf 要能放 count × dim 个 float。*got_out 返回实际读出的条数。
 *
 * 【谁在用】
 *
 *   - 重建 HNSW 索引：构建要按顺序拿到每个向量
 *   - 加载磁盘索引：读回时要填 HNSW 的内部数组
 *   - 精确近邻查询：这就是"暴力扫"，读全部向量页
 *
 * 它是**唯一**一个要读全部向量页的地方 ——
 * 也正是 A4 里那个"重"的对照组。
 */
int  vstore_scan_all(VStore *vs, float *buf, u64 *got_out);

/* ---- 地址算术（不读盘，单元测试直接验它）---- */
void vstore_slot_of(const VStore *vs, u64 row_id,
                    u32 *ordinal_out, u16 *slot_out, u16 *off_out);

/* ---- 只读访问 ---- */
u16  vstore_dim(const VStore *vs);
u16  vstore_k(const VStore *vs);
u64  vstore_count(const VStore *vs);
u32  vstore_npages(const VStore *vs);
u32  vstore_page_id(const VStore *vs, u32 ordinal);

/* 累计读了多少个数据页（诊断用：验"ANN 的页读 ≪ 暴力扫"） */
u64  vstore_page_reads(const VStore *vs);

/* 累计读了多少条标量记录 */
u64  vstore_meta_reads(const VStore *vs);

/*
 * 由数据页号反查它的序号（ordinal）。
 *
 * 【为什么需要反查】
 *
 * 寻址是单向的算术：row_id → (ordinal, slot)。但 RID 反解成 row_id 时
 * 需要"这个页是第几个数据页" —— 那是**反方向**。
 *
 * 靠二分查找解决：数据页是**按顺序分配**的（vec_new_page 每次都追加），
 * 所以它们的页号**严格递增**，pages[] 天然有序。
 *
 * 这也是"列式布局是纯算术"这句话的一个例外 ——
 * 正向纯算术，反向要一次二分（O(log n)，不是 O(1)）。
 */
int  vstore_ordinal_of_page(const VStore *vs, u32 page_id, u32 *ordinal_out);

/*
 * 标量表的 gstore。
 *
 * 【为什么把它暴露出去】
 *
 * 同模态的 AM（vector_am）需要**拉式**地扫标量表
 * （一行一行给，不是回调），好把 label 下推做成一个行源。
 * 而 vstore 的对外 API 都是推式的（回调）。
 *
 * 暴露 gstore 而不是再造一套游标 API，是因为 gstore 本来就是
 * 存储层的公共原语（v1.0 步骤 1 刚把它下沉到 storage/）。
 */
GStore *vstore_meta(VStore *vs);

/* 解码一条标量记录（buf 是 gstore 返回的整条记录，含公共头） */
void vstore_meta_decode(const u8 *rec, u64 *row_id, i32 *label);

#endif /* DB_MODALITIES_VECTOR_VSTORE_H */
