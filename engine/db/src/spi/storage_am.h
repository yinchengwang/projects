#ifndef DB_SPI_STORAGE_AM_H
#define DB_SPI_STORAGE_AM_H

#include "common/error.h"
#include "common/types.h"
#include "spi/pushdown.h"
#include "storage/batch.h"
#include "storage/record.h"
#include "storage/value.h"
#include "txn/mvcc.h"

/*
 * Storage AM —— 模态向执行器暴露的唯一接口（v0.9 定稿）。
 *
 * 【这个接口解决什么问题】
 *
 * 多模态数据库里，"模态"不是"在 SQL 里加语法"，而是**实现同一套接口**。
 * 执行器只认接口，不认模态。加第 9 个模态 = 多写一个 .c 文件，
 * 执行器一行不改。
 *
 * 【DESIGN.md §5.1 的草图漏掉了五件事，这里补齐了】
 *
 *   1. schema 访问器   —— 执行器要绑列名、要填结果集的列名
 *   2. 解码回调       —— 模态的字节串不是列式记录，执行器不能自己解
 *   3. 可见性归属     —— 每种模态的版本语义不同（heap 有 xmin/xmax，blob 没有）
 *   4. 能力协商       —— blob 只读，B+tree 没有 KNN
 *   5. 写路径的 xid   —— 硬规则 5：xid 必须显式传，不能藏进共享对象
 *
 * 补这五件事的详细理由见 docs/PLAN-v0.9.md §1.2。
 *
 * 【依赖方向】
 *
 * spi/ 是 L3，依赖 storage/（L1）和 txn/（L2）✓。
 * **模态不许依赖 spi/ 之外的东西，spi/ 也不许依赖 modalities/ 里的任何东西** ——
 * 模态之间的隔离靠的就是这一条（见 tools/check_deps.sh）。
 *
 * 【为什么值类型在 storage/】
 *
 * DbValue 从 query/ 挪到了 storage/：它是存储层、查询层、SPI 三方共用的
 * 契约语言。放在 query/ 会让 spi/ 反过来依赖 query/，层次就倒了。
 */

typedef struct StorageAM StorageAM;
typedef struct AmScan    AmScan;

/*
 * 能力位。
 *
 * 【能力协商靠返回值，不靠类型判断】（DESIGN.md §5.2）
 *
 * 优化器看到 E_NOTSUP 就退回全表扫描。加新模态不需要改优化器。
 * caps 是"这个模态声明自己有什么"，用于**提前**拒绝
 * （比如对只读的 blob 表执行 INSERT，直接报"这个表不支持写"，
 * 而不是调进去发现是 NULL 函数指针）。
 */
#define AM_CAP_SCAN     0x01u   /* 能扫描（所有实现都必须有） */
#define AM_CAP_INSERT   0x02u   /* 支持 insert */
#define AM_CAP_UPDATE   0x04u   /* 支持 update */
#define AM_CAP_REMOVE   0x08u   /* 支持 remove */
#define AM_CAP_EXTERNAL 0x10u   /* 支持外部值（大值不进值层） */
#define AM_CAP_PUSHDOWN 0x20u   /* 支持谓词下推 */
#define AM_CAP_VACUUM   0x40u   /* 支持版本回收 */
#define AM_CAP_INDEX    0x80u   /* 支持建索引 / 重建索引 */

/*
 * ==================== v1.2 步骤 0：批能力 ====================
 *
 * 这两个是**可选**能力：caps 位 + 函数指针为 NULL = 不支持。
 *
 * 【为什么要加它们】
 *
 * v1.1 把执行器批式化了，但 A2（比火山快 ≥ 3 倍）只拿到 1.11×。
 * 实测拆开来看：逐行 scan_next 占 38%，逐行 decode 占 43% ——
 * **瓶颈在 SPI 的逐行接口**，不在求值。
 *
 * 批扫描省掉"每行一次 SPI 调用"，批解码省掉"每行物化一个 272 字节的 DbValue"。
 *
 * 【为什么不会破坏已有模态】
 *
 * blob / graph / vector 三个模态的这两个指针**保持 NULL**，
 * 执行器看到 NULL 就自动退回逐行路径 —— 一行都不用改。
 * 这正是 v1.1 §1.2 担心被打破的那条底线，而能力协商本来就是项目自己的机制。
 */
#define AM_CAP_BATCH_SCAN   0x100u  /* 支持一次给一批原始行 */
#define AM_CAP_BATCH_DECODE 0x200u  /* 支持一次解一批，直接进列数组 */

/* 版本回收（VACUUM）的统计 */
typedef struct {
    u64 scanned;
    u64 removed;
    u64 kept;
    u64 compacted_pages; /* 页内 compact 整理过的页数 */
    u64 freed_bytes;     /* 页内 compact 回收的空间（字节） */
    u64 frag_pct;        /* 整理前的全表碎片率（百分比） */
} AmVacuumStats;

/*
 * 可见性上下文。
 *
 * 【为什么必须显式传，不能存在 AM 里】
 *
 * 快照是**连接私有**的（v0.7 的实例/连接分离），clog 是**实例共享**的。
 * 把它们存进 AM（实例级共享）= 存共享状态 = 连接 A 的读会用到连接 B 的快照。
 * 这正是 v0.7 付过血泪代价的那类错误（xid 藏在共享对象里）。
 *
 * 所以：调用方（执行器）每次调用都显式传进来。
 */
typedef struct {
    const Snapshot *snap;      /* 当前连接的快照 */
    const TxnTable *txns;      /* 实例共享的事务表（clog 在里面） */
} AmVisCtx;

/*
 * Storage AM。
 *
 * 【行的表示：字节串 + 模态自己的解码】
 *
 * scan_next / get 返回的是**模态自己的字节串**，不是列式记录。
 * 关系模态返回 tuple 格式的记录；blob 模态返回它自己的行格式。
 *
 * 为什么不统一成一种字节格式：那种格式就是关系模态的格式 ——
 * 强行统一等于逼所有模态都长成关系模态，那就不叫多模态了。
 *
 * 那执行器怎么拿到值？调 `decode` —— **由模态自己解**。
 * 这是"最小公共表示"：字节串 + 一个解码函数。
 *
 * 【scan_next 的 buf / len 约定】
 *
 * buf 是调用方给的缓冲，cap 是它的容量。
 * 模态把这一行的字节写进去，*len 是实际长度。
 * 缓冲不够返回 E_NOSPACE（调用方据此决定要不要换大缓冲）。
 */
struct StorageAM {
    const char *name;          /* "heap" / "blob" / 将来的 "graph" / "vector" */
    u32         caps;          /* AM_CAP_* 的按位或 */

    /*
     * schema：模态自己的列定义。
     *
     * 指针归 AM 所有（heap 转发 catalog 的列定义，blob 用自己的静态数组）。
     * 调用方**只能读，不能改**。
     */
    int (*columns)(StorageAM *am, const ColDesc **cols, int *ncols);

    /*
     * 扫描。scan_next 返回 E_EOF 表示结束（不是错误）。
     */
    int (*scan_begin)(StorageAM *am, AmScan **out);
    int (*scan_next)(StorageAM *am, AmScan *s, Rid *rid, u8 *buf, u16 cap, u16 *len);
    int (*scan_end)(StorageAM *am, AmScan *s);

    /*
     * 按 RID 取一行。
     *
     * RID 是**表内**的寻址方式：(page, slot)。
     * 每张表有自己的 RID 空间 —— blob_frames 和 blob_objects 的 RID
     * 互不相干，也不该互相比较。
     */
    int (*get)(StorageAM *am, Rid rid, u8 *buf, u16 cap, u16 *len);

    /*
     * 把模态自己的字节串解成值。
     *
     * out 是调用方给的数组，长度 = columns() 报的 ncols。
     */
    int (*decode)(StorageAM *am, const u8 *buf, u16 len, DbValue *out);

    /*
     * ==================== 批接口（v1.2 步骤 0，可选） ====================
     *
     * 两个指针都是 NULL 表示"这个模态不支持批"，
     * 执行器自动退回逐行路径（scan_next + decode）。
     *
     * 【batch_scan】
     *
     * 一次给一批原始行。rows 由调用方给（至少 cap 项）。
     * *n_out 是本批实际返回的行数（可能少于 cap —— 模态的竞技场装不下了）。
     *
     * 行的字节串由 AM 持有，**有效期到下一次 batch_scan 调用为止**。
     * 所以调用方必须在下一次 batch_scan 之前把数据用掉。
     *
     * 返回 E_EOF 表示没有更多行（不是错误）。
     *
     * 【batch_decode】
     *
     * 把一批原始行直接解进列数组，**不物化 DbValue**。
     * out->nrows 是写游标：调用前为这一批的起点，返回后前进 nrows 行。
     *
     * 只写 out->cols[c].used 为真的列。
     */
    int (*batch_scan)(StorageAM *am, AmScan *s, AmRowRef *rows, int cap, int *n_out);
    int (*batch_decode)(StorageAM *am, const AmRowRef *rows, int nrows, AmBatch *out);

    /*
     * 这一行对当前快照可见吗？
     *
     * 【为什么可见性归 AM，不归执行器】
     *
     * heap 的行有 xmin/xmax，要按快照判断；blob 的行没有版本，恒可见。
     * **每种模态的版本语义不同** —— 统一到执行器，就等于让执行器
     * 知道所有模态的内部结构，SPI 就白设了。
     *
     * 所以：谁定义版本语义，谁负责判断。
     */
    bool (*visible)(StorageAM *am, const u8 *buf, u16 len, const AmVisCtx *vis);

    /*
     * 写路径。
     *
     * 【xid 必须显式传】（硬规则 5）
     *
     * AM 是实例级共享的。xid 是连接私有的当前事务号。
     * 把 xid 存在 AM 里 = 存共享状态 = 连接 A 的写带上连接 B 的 xid
     * → 恢复时 A 的未提交数据被判为已提交。
     *
     * blob 的写接口返回 E_NOTSUP（对象不可变，见 DESIGN.md §5.4）。
     */
    int (*insert)(StorageAM *am, u32 xid, const DbValue *vals, Rid *out);
    int (*update)(StorageAM *am, u32 xid, Rid rid, const DbValue *vals, Rid *new_rid);
    int (*remove)(StorageAM *am, u32 xid, Rid rid);

    /*
     * 版本回收（VACUUM）。
     *
     * 只有"有版本"的模态需要（关系模态的行有 xmin/xmax）。
     * blob 的对象不可变 → 没有版本 → 返回 E_NOTSUP。
     *
     * txns 显式传（实例共享的事务表）—— 和可见性一样，
     * 不能存在 AM 里当共享状态。
     */
    int (*vacuum)(StorageAM *am, TxnTable *txns, u32 xid, AmVacuumStats *out);

    /*
     * 重建索引：把已有行灌进索引。
     *
     * index_id = 0 表示重建这张表的**全部**索引。
     * rows_out 报告灌了多少行（可为 NULL）。
     *
     * 【什么时候需要】
     *   - 建索引之后（新索引是空的，得灌数据）
     *   - 崩溃恢复之后（索引不写日志，恢复后要重建）
     */
    int (*reindex)(StorageAM *am, u32 index_id, u64 *rows_out);

    /*
     * 模态私有状态。上层不许碰。
     *
     * heap：指向 HeapAmCtx（堆句柄 + 列定义 + 索引列表）
     * blob：指向 BlobStore
     */
    void *impl;

    /*
     * 谓词下推（见 spi/pushdown.h）。
     *
     * NULL 表示这个模态不支持下推 —— 执行器直接走全表扫描。
     *
     * 【为什么放在 StorageAM 里而不是单独一个结构】
     * 下推的产物是一个行源，行源是"这张表怎么被扫描"的一部分 ——
     * 它和扫描是同一件事的两种做法。拆开反而要多一层间接。
     */
    CanPushFn can_push;
    PushFn    push;
};

#endif /* DB_SPI_STORAGE_AM_H */
