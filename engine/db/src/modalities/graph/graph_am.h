#ifndef DB_MODALITIES_GRAPH_GRAPH_AM_H
#define DB_MODALITIES_GRAPH_GRAPH_AM_H

#include "spi/storage_am.h"
#include "modalities/graph/graph.h"

/*
 * graph_am —— 图模态的 SPI 适配（v0.11 步骤 3）
 *
 * 【为什么这个文件在 modalities/graph/ 而不是 spi/】
 *
 * PLAN-v0.11 §二 步骤 3 写的是 `spi/graph_am.{h,c}`，实际落盘改了位置。
 * 原因是依赖方向：
 *
 *   - spi/ 是 L3，modalities/ 是 L4。
 *   - spi/ **不许依赖** modalities/（storage_am.h 的依赖方向一节写死：
 *     "spi/ 也不许依赖 modalities/ 里的任何东西"）。
 *   - 所以一个住在 spi/ 的 graph_am 必须 include modalities/graph/graph.h
 *     —— 那是 L3 依赖 L4，**反向依赖**，层次就倒了。
 *
 * 反过来，modalities/graph/ 依赖 spi/ 是**向下依赖**，合法
 * （storage_am.h 的原话："模态不许依赖 spi/ 之外的东西"）。
 * blob_am 住在 spi/ 是因为 blob 的存储在 storage/（L1），spi/ → storage/ 合法；
 * 图的存储在 modalities/graph/，所以图的适配器必须跟着住在 modalities/graph/。
 *
 * 【图在 SQL 里长什么样】
 *
 * 两张虚拟表：
 *
 *   graph_nodes   一行 = 一个节点
 *                 node_id / <属性列…>
 *
 *   graph_edges   一行 = 一条边
 *                 src / dst
 *
 * 和 blob 的两张表同款思路：一个模态可以提供多张表，
 * 每张表有自己的 RID 空间（节点槽和边槽互不相干）。
 *
 * 【node_id 是什么】
 *
 * node_id = rid_to_u64(节点 RID) = (页号 << 16) | 槽号。
 *
 * 为什么用 RID 而不是自增序号：
 *   - 定长记录的天然好处就是"id 不需要额外分配"（graph.h 的原话）；
 *   - 自增序号要额外维护一张 id → RID 表，图删除节点后还要回收，
 *     而 RID 是记录的位置，删了就空出来，没有回收问题。
 *
 * src / dst 是**节点 id**（不是边 id）—— 边记录里存的就是
 * rid_to_u64(端点 RID)，所以 SQL 层直接暴露 u64 编码，
 * JOIN 和 WHERE 都不需要转换。
 *
 * 【为什么 node_id 列 nullable = true】
 *
 * node_id 由**存储层分配**（RID 是分配出来的，不是用户给的）。
 * 用户 INSERT 时没有 id 可填，只能填 NULL。
 * 所以这一列必须可空，执行器才不会报"列 node_id 不允许 NULL"。
 *
 * 【写入语义：语句级提交】
 *
 * PLAN §1.4 判决：图不做 MVCC，写路径的 WAL xid 传 0（系统事务）。
 * 所以 AM 的 insert 收到执行器给的 xid 之后，**转成 0** 再传给图层。
 *
 * 这不是"绕过 xid"：
 *   - SPI 的写接口仍然强制带 xid 参数（硬规则 5 在接口层成立）；
 *   - 图模态的版本语义是"没有版本"（PLAN §1.4 判决 b），
 *     它不参与 BEGIN/COMMIT 的多语句事务，
 *     所以它的日志记录必须是"永远视为已提交"的系统事务。
 *   - 反过来（把真实 xid 传给图层）更危险：图层没在 clog 里登记这个 xid，
 *     恢复时 xid_committed() 找不到对应的 COMMIT → **图数据被回滚掉**。
 *
 * 【可见性：恒真】
 *
 * 图没有版本链：记录要么在用（in_use = 1）要么在空闲链上（in_use = 0）。
 * 没有"这个版本对当前快照可不可见"的问题 → visible() 恒真。
 *
 * 【能力位】
 *
 *   SCAN      能扫（所有实现都必须有）
 *   INSERT    能插（节点 / 边）
 *   PUSHDOWN  能下推
 *
 * 不声明 UPDATE / REMOVE / VACUUM / INDEX：
 * 执行器在调用之前就按能力位拒掉（报"表 %s 不支持更新"），
 * 不会调到 NULL 函数指针。回调本身也返回 E_NOTSUP（双保险）。
 *
 * 【下推的两种形态】
 *
 *   graph_edges 的 src / dst 等值 → **真下推**：
 *       沿锚点节点的邻接链走，只读该节点的出链 / 入链。
 *       读的行数 = 该节点的度数，不是全表边数。
 *
 *   graph_nodes 的属性列 → **自滤**：
 *       列值就在记录里，AM 逐行解码自己过滤。
 *       过滤语义与执行器的比较语义**逐字一致**（复用 val_cmp），
 *       所以下推的行数 == 全扫过滤后的行数。
 *
 * 【下推必须保守】
 *
 * 执行器拿到下推的行之后**仍然完整求值一遍 WHERE**。
 * 下推只是"少读一些行"，不是"替你判断"。
 * 所以下推实现错了最多是慢，不会是错 —— 除非它漏行。
 * 因此所有过滤都按"宁可多给，不能少给"的原则设计。
 */

/* AM 的私有状态。调用方持有（像 BlobAmCtx 一样）。 */
typedef struct {
    Graph   *g;
    bool     is_nodes;      /* true = graph_nodes；false = graph_edges */
    ColDesc  cols[GR_MAX_COLS + 1];
    int      ncols;
} GraphAmCtx;

/* 构造节点表的 AM */
int graph_am_build_nodes(Graph *g, StorageAM *out, GraphAmCtx *ctx);

/* 构造边表的 AM */
int graph_am_build_edges(Graph *g, StorageAM *out, GraphAmCtx *ctx);

/* ---------------- 内部结构（实现细节，不属于公开 API） ---------------- */

/*
 * 节点表的总列数上限。
 *
 * 节点表 = node_id + 属性列 → 最多 1 + GR_MAX_COLS = 16 列。
 * 这和执行器的行缓冲 DbValue row[SQL_MAX_COLS]（16 列）正好对齐。
 *
 * 这里不写 SQL_MAX_COLS 是为了不让 modalities/ 依赖 query/ ——
 * 层次要保持"向下"，不能因为一个常量就把查询层拖进来。
 * 两者相等是刻意的（GR_MAX_COLS = 15 就是照着 16 定的）。
 */
#define GA_MAX_COLS (GR_MAX_COLS + 1)

/* 内部记录缓冲：节点记录最大 GR_N_BITMAP + GR_PROP_MAX = 276 字节 */
#define GA_REC_MAX 512

/* 节点行：[u64 node_id][节点记录原样] */
#define GA_NODE_ID_BYTES 8

/* 边行：[u64 src][u64 dst] */
#define GA_EDGE_ROW_BYTES 16

/*
 * 扫描模式。
 *
 * 前两个是全扫，后三个是下推 —— 所有模式的行格式完全一样，
 * 所以 decode / visible 对它们一视同仁。
 */
enum {
    GA_MODE_SCAN_NODES = 0,   /* 全扫节点表 */
    GA_MODE_SCAN_EDGES,       /* 全扫边表 */
    GA_MODE_PUSH_NODE_FILTER, /* 节点表 + 属性列自滤 */
    GA_MODE_PUSH_NODE_ONE,    /* 节点表 + node_id 等值（直接定位一条） */
    GA_MODE_PUSH_EDGE_CHAIN,  /* 边表 + 沿邻接链枚举 */
    GA_MODE_EMPTY             /* 空扫描（锚点不存在等） */
};

/* 扫描状态。 */
typedef struct {
    GraphAmCtx    *ctx;
    int            mode;

    GStoreCursor   cur;       /* 全扫用 */
    GraphEdgeCursor ec;       /* 边链下推用 */

    Rid            one;       /* PUSH_NODE_ONE 的目标 */
    bool           one_done;  /* 已经吐过这一条 */

    int            fcol;      /* 自滤的列下标（表列号，0 = node_id） */
    int            fop;       /* PD_* */
    DbValue        fval;      /* 比较的常量 */

    u8             rec[GA_REC_MAX];   /* 内部记录缓冲 */
} GraphScan;

/* 取出 AM 的私有状态 */
static inline GraphAmCtx *ga_ctx_of(StorageAM *am)
{
    return (GraphAmCtx *)am->impl;
}

/*
 * 精确复刻执行器的比较语义（见 query/exec_scan.c 的 eval_expr）。
 *
 * 为什么必须"精确"而不是"保守"：
 * 自滤会**丢掉**不满足谓词的行 —— 丢错了就是漏数据（静默错误）。
 * 而下推整体是保守的（宁可多给），只有自滤这一步是"减法"。
 * 所以自滤必须和执行器算出一模一样的结果。
 *
 * *matched 只会是 true / false，不会是"不知道"。
 */
int ga_match(int op, const DbValue *a, const DbValue *b, bool *matched);

/*
 * 节点 id（数值）→ Rid。
 *
 * 节点 id = rid_to_u64(节点 RID) = (页号 << 16) | 槽号。
 * 页号是 u32，所以合法的 id 一定 < 2^48。
 *
 * 越界 / 保留值（PAGE_ID_NIL）一律 E_INVAL —— 不静默截断。
 */
int ga_num_to_rid(i64 num, Rid *out);

/* 节点 id（DbValue 形态）→ Rid。要求是整型字面量。 */
int ga_nodeid_to_rid(const DbValue *v, Rid *out);

/* 下推回调（实现见 graph_am_push.c） */
int graph_am_can_push(StorageAM *am, const Predicate *p);
int graph_am_push(StorageAM *am, const Predicate *p, AmScan **out);

#endif /* DB_MODALITIES_GRAPH_GRAPH_AM_H */
