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
    PT_VEC_LINK    = 0x0E,  /* HNSW 邻接表页（定长槽页，每节点一条记录） */

    PT_FT_HEAD     = 0x0F,  /* 全文检索的段头页（v1.3） */
    PT_FT_DICT     = 0x10,  /* 全文检索的字典页（v1.3） */
    PT_FT_POST     = 0x11,  /* 全文检索的倒排页（v1.3） */
    /*
     * 全文检索的 tombstone 页（v1.3 步骤 5）。
     *
     * 这是"被删文档集合"的**权威存储**：页里是一串 u32 的 doc_id。
     * 光靠 WAL 重放是不够的 —— checkpoint 会把日志截断，
     * 老的删除记录就没了，被删的文档会"复活"（坑 B）。
     */
    PT_FT_TOMB     = 0x12,

    /*
     * 全文检索的**根页**与**段目录页**（v1.3 欠账 3 / 7）。
     *
     * 根页（两张，交替写）指向"当前这一版段目录"；
     * 段目录页记"有哪些段、状态是什么、tombstone 页在哪"。
     * 开库时读两张根页就能定位目录 —— 不用再扫整个页空间。
     *
     * ⚠️ 数值与 modalities/ft/ft_root.h 里的 FT_ROOT_PTYPE / FT_CAT_PTYPE
     * 必须一致（磁盘格式的契约）。改这里就要同步改那边。
     */
    PT_FT_ROOT     = 0x13,  /* 全文检索的根页（双页交替） */
    PT_FT_CATALOG  = 0x14,  /* 全文检索的段目录页 */

    /*
     * **空闲页标记**（W-D 的 D-6.3）。
     *
     * 一页被 pager_free() 归还给全局页分配器时，页类型改成这个。
     * 开库时在 WAL 重放**之后**扫一遍页空间，凡是这个类型的就是空闲页，
     * 据此把内存里的空闲栈重建出来 —— 这就是"空闲栈落盘"。
     *
     * ⚠️ **页类型是全局命名空间**（坑 M6-27）。已占用：
     *   0x01–0x14 本文件；0x15 PT_RTREE、0x16 PT_GEO_META（geo）、
     *   0x17–0x19 PT_TS_META/DIR/PART、0x1A PT_TS_COL（ts）、0x1B PT_JSON_OVF（doc）。
     *   所以这里取 **0x1C**（新的值必须自己往后排，不许复用）。
     *
     * 【为什么不写 WAL 记录】
     *
     * 这个标记**不承载任何用户数据**，它只表达"这一页没有主人"。
     * 崩溃时无论丢不丢这个标记，两个方向都是安全的：
     *
     *   - 标记丢了（页还在盘上是旧内容）⇒ 那页没人引用 ⇒ 只是**空洞**（浪费空间）
     *   - 标记留着 ⇒ 开库重建成空闲页 ⇒ 下次分配复用它 ⇒ 正确
     *
     * 绝不会出现"标记留着、页却已经有主人"：
     * 新主人拿到这页之后第一件事就是整页清零（pager_alloc 的 memset），
     * 页类型随之变成新类型；而那次写盘又被 WAL 规则护着
     *（页刷盘前它的日志必须先落盘）⇒ 崩溃恢复重放后页一定属于新主人。
     *
     * 所以这里刻意**不**引入新的日志记录类型 —— 少一个失败模式。
     */
    PT_FREE        = 0x1C   /* 空闲页（已归还全局页分配器） */
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
