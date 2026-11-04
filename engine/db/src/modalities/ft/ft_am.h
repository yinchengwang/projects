#ifndef DB_MODALITIES_FT_AM_H
#define DB_MODALITIES_FT_AM_H

#include <stdbool.h>

#include "common/types.h"
#include "spi/storage_am.h"
#include "modalities/ft/ft_index.h"

/*
 * ft_am —— 全文检索模态的 SPI 适配（v1.3 步骤 4）
 *
 * 【为什么这个文件在 modalities/ft/ 而不是 spi/】
 *
 * 计划稿写的是 spi/ft_am.{h,c}，但**分层门禁不允许**：
 * spi/ 是 L3，modalities/ 是 L4，L3 不许依赖 L4。
 * 所以适配器必须跟着模态住（与向量模态的 vector_am.c 同款做法）。
 *
 * 【ft_docs 在 SQL 里长什么样】
 *
 *   ft_docs   一行 = 一篇文档
 *             doc_id / doc_len / nterms / sealed
 *
 * 四列（全是标量列）：
 *   doc_id   INT8  文档标识（调用方给的，不连续）
 *   doc_len  INT4  文档的词数
 *   nterms   INT4  该文档命中的**不同**词数
 *   sealed   BOOL  该文档是否已被 tombstone（已打删除标记）
 *
 * 【为什么要"现算"而不是存一张文档表】
 *
 * 段的物理格式里**没有文档表**（见 ft_internal.h 关于 dl 的说明）：
 * 文档长度 dl 跟着 posting 走。所以 ft_docs 的行只能从倒排链里**现算**：
 *
 *   逐段 → 逐词（ft_segment_scan_terms）→ 逐 posting（ft_segment_postings）
 *   → 按 doc_id 聚合出 (doc_len = posting 的 dl, nterms = 命中次数)
 *
 * 内存开销 O(文档数)。这是**元数据路径**，不是热路径 ——
 * 真正的检索走 db_ft_search（专用 API），不在 SQL 里做相关性排序。
 *
 * 【本版只读】
 *
 * 能力位里没有 INSERT / UPDATE / REMOVE / VACUUM：
 * 文档的写入走专用 API（db_ft_add），SQL 侧只读。
 * 理由与向量模态同款：写入不只是"加一行"，它还要建段（写页）。
 */

/* FtAmCtx 的魔数（见结构体里 magic 字段的说明） */
#define FT_AM_CTX_MAGIC 0x46414D43u   /* "FAMC" */

/* ft_docs 的一行：doc_id / doc_len / nterms / sealed */
typedef struct {
    u32 doc_id;
    u32 doc_len;   /* 词数 */
    u32 nterms;    /* 命中的不同词数 */
    u32 sealed;    /* 1 = 已被 tombstone */
} FtDocRow;

/*
 * 物化结果的持有者（引用计数）。
 *
 * 缓存里的行是**共享**的：几个并发扫描可能同时在读同一份行。
 * 引用数归零 ⇒ 没人在用了 ⇒ 真正释放。
 * （实现见 ft_am_cache.c；坑 M4-29）
 */
typedef struct FtDocBuf FtDocBuf;

struct FtDocBuf {
    u32      refcnt;   /* 引用数：缓存 1 份 + 每个在扫的扫描 1 份 */
    u32      nrows;
    FtDocBuf *next;    /* 全局存活链表（ft_am_cache.c 用） */
    FtDocRow rows[1];  /* 变长：实际按 nrows 个分配 */
};

/* AM 的私有状态 */
typedef struct {
    FtIndex *ix;   /* 索引（实例级共享：所有连接看同一批文档） */

    /*
     * 魔数：用来判断"这个 ctx 是不是已经初始化过"。
     *
     * 为什么需要：ft_am_ctx_release() 会去摘缓存里的指针 ——
     * 如果 ctx 是**未初始化**的栈变量/垃圾内存，那个指针是野的。
     * 所以释放前先对一下魔数，对不上就当"没有缓存"处理。
     */
    u32       magic;

    /*
     * ---------------- 物化缓存（v1.3 欠账 5） ----------------
     *
     * ft_docs 每次扫描都要把**整个索引**物化一遍（读全部倒排页）。
     * 索引没变时，第二次扫描可以直接复用上次的行。
     *
     * 失效判据是**索引签名**（段数 / 文档数 / tombstone 数）：
     * 任何会改内容的写路径都必然改签名（论证见 ft_am.c 的 ft_am_sig 注释）。
     *
     * cache 是"上次物化的行"（不透明指针，结构定义在 ft_am.c 里）。
     */
    void     *cache;          /* FtDocBuf* —— 上次物化的行 */
    bool      cache_valid;    /* cache 里的行是否还对得上当前索引 */
    u32       sig_nsegs;      /* 缓存对应的签名（三元组） */
    u32       sig_ndocs;
    u32       sig_ntombs;

    /* 诊断计数器（测试断言用；不是公开 API，所以不带 db_ 前缀） */
    u64       n_materialize;  /* 真正做过物化的次数 */
    u64       n_hit;          /* 命中缓存的次数 */
} FtAmCtx;

/* 构造 ft_docs 的 AM */
int ft_am_build_docs(FtIndex *ix, StorageAM *out, FtAmCtx *ctx);

/*
 * ---------------- 物化缓存（实现见 ft_am_cache.c） ----------------
 */

/* 把整个索引物化成行（ft_am.c 实现，ft_am_cache.c 调用） */
int ft_am_materialize_index(const FtIndex *ix, FtDocBuf **out);

/* 新建一个物化缓冲（ft_am_cache.c 实现，ft_am.c 造缓冲时用） */
FtDocBuf *ft_am_buf_new(u32 nrows);

/*
 * 取"可以用的行"。
 *
 * 命中缓存 → 借用缓存里的行；没命中 → 物化一遍并放进缓存。
 * 返回的行是**借来的**：用完必须调 ft_am_rows_release()。
 */
int  ft_am_rows_acquire(FtAmCtx *c, FtDocBuf **buf, FtDocRow **rows, u32 *n);
void ft_am_rows_release(FtDocBuf *buf);

/*
 * ---------------- 诊断接口（供测试断言用） ----------------
 *
 * 【为什么不用 db_ 前缀】
 *
 * db_ 前缀是**公开 API** 的前缀（include/db.h 里的 db_ft_* 之类）。
 * 这些是模态内部的诊断口，测试直接 include 本文件调用 —— 不进公开面。
 */

/* 物化次数 / 缓存命中次数 */
u64 ft_am_stat_materialize(const FtAmCtx *ctx);
u64 ft_am_stat_cache_hit(const FtAmCtx *ctx);

/* 强制让缓存失效（测试用来对比"有缓存"与"没缓存"的行内容） */
void ft_am_cache_invalidate(FtAmCtx *ctx);

/*
 * 释放缓存占用的内存。
 *
 * 索引/实例销毁时必须调用它 —— 否则缓存里的行会一直占着内存（泄漏）。
 */
void ft_am_ctx_release(FtAmCtx *ctx);

#endif /* DB_MODALITIES_FT_AM_H */
