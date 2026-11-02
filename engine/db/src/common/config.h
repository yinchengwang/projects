#ifndef DB_COMMON_CONFIG_H
#define DB_COMMON_CONFIG_H

/*
 * 所有"魔法数字"集中在这里。改参数只改这个文件，
 * 这样你才能做"把页大小从 4KB 改成 16KB，先预测后果再验证"的练习。
 */

#define PAGE_SIZE        4096u   /* 页大小；改成 16384 是很好的实验 */
#define HDR_SIZE         20u     /* slotted page 头部 */
#define SLOT_SIZE        4u      /* offset(u16) + length(u16) */

/* slotted page 头部字段偏移 */
enum {
    H_TYPE       = 0,   /* u8  页类型 */
    H_FLAGS      = 1,   /* u8  bit0 dirty / bit1 has_overflow / bit2 needs_compact */
    H_SLOTS      = 2,   /* u16 已分配 slot 数（含空洞） */
    H_FREE_OFF   = 4,   /* u16 记录区起始（从页尾往上） */
    H_FREE_BYTES = 6,   /* u16 中间连续空隙 */
    H_NEXT       = 8,   /* u32 叶子链 / overflow 链 */
    H_LSN        = 12,  /* u32 本页最后修改的 LSN */
    H_CKSUM      = 16,  /* u16 crc16 */
    H_TID        = 18   /* u16 所属表 id（heap 扫描用）*/
};

/* 页类型 */
enum {
    PT_META        = 0x01,
    PT_HEAP        = 0x02,
    PT_BTREE_INNER = 0x03,
    PT_BTREE_LEAF  = 0x04,
    PT_OVERFLOW    = 0x05,
    PT_BLOB        = 0x06,  /* blob 模态的帧页（v0.8） */
    PT_GRAPH_NODE  = 0x07,  /* 图模态的节点页（v0.11） */
    PT_GRAPH_EDGE  = 0x08,  /* 图模态的边页（v0.11） */
    PT_GRAPH_SCHEMA = 0x09, /* 图模态的属性 schema 页（v0.11 步骤 4） */

    PT_VEC_HDR     = 0x0A,  /* 向量存储的头页（v1.0：dim / 链头） */
    PT_VEC_DATA    = 0x0B,  /* 向量数据页（列式：一页 K 个向量） */
    PT_VEC_META    = 0x0C,  /* 向量标量属性页（定长槽页，走 gstore） */
    PT_VEC_INDEX   = 0x0D,  /* HNSW 索引头页（v1.0 步骤 4） */
    PT_VEC_LINK    = 0x0E   /* HNSW 邻接表页（定长槽页，每节点一条记录） */
};

/* buffer pool：帧数必须是 2 的幂，Clock 算法取模更快 */
#define BP_FRAMES        1024u
#define BP_MAX_PIN_TRIES 10000

/* 超过这个长度就走 overflow 链（blob 模态的雏形） */
#define INLINE_THRESHOLD 2048u

/*
 * 批大小（v1.1 步骤 2）：一批攒多少行。
 *
 * 【为什么是 1024】
 * 批的工作集要能待在 L2 里：
 *   列式一批 1024 行 × 4 列 × 8 字节 ≈ 32 KB
 *   批太大就掉到 L3/内存，列式的紧凑优势就没了。
 * 批太小则每批的固定开销（开扫描、收尾）摊薄不够。
 */
#define VEC_BATCH 1024u

/*
 * GROUP BY 的组数上限（v1.1 步骤 3）。
 *
 * 【为什么不 spill】
 * spill（组太多时把中间结果落盘）是另一个数量级的工程 —— 本版明确不做。
 * 但**不能静默截断**：截断出来的结果"看起来对其实错"，
 * 比报错坏得多 —— 报错会让人停下，错的结果会一路流下去。
 * 所以超限就明确报错，并把上限写进错误信息。
 */
#define MAX_GROUPS 4096u

/* WAL */
#define WAL_GROUP_COMMIT_US  1000   /* 攒批窗口（微秒） */
#define WAL_GROUP_COMMIT_MAX 128    /* 或攒够这么多事务就刷 */

/* 文件扩展粒度：一次扩多少页 */
#define EXTEND_PAGES 16u

#endif /* DB_COMMON_CONFIG_H */
