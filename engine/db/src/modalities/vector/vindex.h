#ifndef DB_MODALITIES_VECTOR_VINDEX_H
#define DB_MODALITIES_VECTOR_VINDEX_H

#include "common/config.h"
#include "common/error.h"
#include "common/loghook.h"
#include "common/types.h"
#include "modalities/vector/hnsw.h"
#include "storage/gstore.h"
#include "storage/pager.h"

/*
 * vindex —— HNSW 索引的持久化（v1.0 步骤 4）
 *
 * 【索引落盘的定位：checkpoint，不是日志】
 *
 * 关系模态的 B+tree 早就是这个策略（db.c 里那句"索引不写日志，
 * 崩溃后可能和堆不一致 —— 重建一遍"）。理由对 HNSW 同样成立，
 * 而且更强：
 *
 *   1. 索引是可以**从数据重建**的（向量在 vstore 里，图是算出来的）
 *   2. 索引页的写放大极大 —— 插入一个点要改它自己和最多 2M 个邻居，
 *      逐条记日志会让一次插入变成几十条整页镜像
 *   3. 图结构的"中间态"没有意义：崩在插入中途，那个点要么在要么不在，
 *      重建一遍就干净了
 *
 * 所以：
 *   - 正常关闭（或显式 flush）→ 把整份索引写进磁盘
 *   - 崩溃 → 打开时发现 count 对不上 → 重建（reindex）
 *
 * 代价：flush 是 O(n) 的。10 万点约 4700 万字节，1~2 秒。
 * 换来的是：插入路径上一条索引日志都不用写。
 *
 * 【记录格式】
 *
 * 每个节点一条**定长**记录（定长才能放进定长槽页，寻址才是算术）：
 *
 *   [u32 id]          节点号（读回来要断言它等于序号 —— 列对齐的防线）
 *   [u8  level]       最高层号
 *   [u8  保留]
 *   [u16 cnt[0..L]]   每层邻居数（L = HNSW_MAX_LEVEL）
 *   [u32 links[]]     第 0 层 2m 个，之后每层 m 个
 *
 * 定长意味着**上层也要按最大层数预留**。这是空间换简单：
 * 10 万点 × 470 字节 ≈ 47MB（向量数据本身 51MB）。
 * 想省的话可以拆成两个 gstore（第 0 层 / 上层），上层只有 ~6% 的点需要 ——
 * 但那是优化，不是正确性，留到需要的时候再做。
 *
 * 【为什么记录里要存 id】
 *
 * 不存也能读（第 i 条记录就是第 i 个节点），但那样**错位了也不会报错**。
 * 存一个 id，读的时候断言 id == i —— 和 vstore 标量表的做法一致。
 * 这类"错位不报错"的坑在 vstore 那边已经踩过一次，这里提前防。
 */

/* 每节点记录载荷长度（不含 gstore 的 4 字节公共头） */
#define VINDEX_PAYLOAD(m) \
    (4u + 2u + ((u32)(HNSW_MAX_LEVEL) + 1u) * 2u + \
     ((u32)(m) * 2u + (u32)(HNSW_MAX_LEVEL) * (u32)(m)) * 4u)

/* 记录总长（gstore 的记录长度） */
#define VINDEX_REC(m)  (GS_REC_HDR_SIZE + VINDEX_PAYLOAD(m))

typedef struct VIndex VIndex;

/*
 * 建索引存储（或接管已有的）。
 *
 * dim / m / efc / metric 是索引的身份：和磁盘上的不一致 → E_INVAL。
 * 理由同向量维度：m 决定每层能放几个邻居、记录多长，改了旧记录就没法解释。
 */
int  vindex_create(Pager *pg, LogHookFn fn, void *ctx,
                   u16 dim, u16 m, u16 efc, u8 metric, VIndex **out);

/* 打开已有的索引存储。没有 → E_NOTFOUND */
int  vindex_open(Pager *pg, LogHookFn fn, void *ctx, VIndex **out);

void vindex_destroy(VIndex *ix);

/*
 * 把内存里的索引整份写进磁盘。
 *
 * 已存在的节点**原地改写**（Rid 不变），新节点追加 —— 所以反复 flush
 * 不会让记录越堆越多。
 *
 * 返回写入了多少个节点。
 */
int  vindex_flush(VIndex *ix, u32 xid, const Hnsw *h);

/*
 * 读回索引。
 *
 * vecs 是 n × dim 的向量数组（从 vstore 读出来的，索引本身不存向量 ——
 * 向量只有一个真相来源）。
 *
 * 磁盘上的节点数 != expect_count → E_NOTFOUND（说明索引过期或缺失，
 * 调用方应该走重建路径）。**不返回一个"差不多"的索引** ——
 * 索引和数据对不上时，宁可重建也不要拿错的图去搜。
 */
int  vindex_load(VIndex *ix, u32 expect_count, const float *vecs, Hnsw **out);

u32 vindex_count(const VIndex *ix);
u16 vindex_m(const VIndex *ix);
u16 vindex_dim(const VIndex *ix);

#endif /* DB_MODALITIES_VECTOR_VINDEX_H */
