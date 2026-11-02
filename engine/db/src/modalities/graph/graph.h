#ifndef DB_MODALITIES_GRAPH_GRAPH_H
#define DB_MODALITIES_GRAPH_GRAPH_H

#include "common/config.h"
#include "common/error.h"
#include "common/loghook.h"
#include "common/types.h"
#include "storage/pager.h"
#include "storage/value.h"
#include "storage/gstore.h"

/*
 * graph —— 图存储：定长记录 + 双向邻接链（v0.11 步骤 2）
 *
 * 【这一层解决什么】
 *
 * gstore 是"定长槽页原语" —— 它只管槽的分配/寻址/读写，
 * 不知道图是什么。这一层把「节点」「边」「邻接链」翻译成槽操作。
 *
 * 【记录布局】（PLAN-v0.11 §1.2）
 *
 * 节点记录（定长，大小由 schema 决定）：
 *   [0]  in_use      u8   gstore 公共头（in_use / next_free）
 *   [4]  first_out   u64  第一条出边的 RID —— 出链的头
 *   [12] first_in    u64  第一条入边的 RID —— 入链的头
 *   [20] prop_bitmap 变长  NULL 位图（ceil(ncols/8) 字节）
 *   [...] prop 区     变长  各列定长值，按列序排布
 *
 * 边记录（定长 GR_EDGE_SIZE = 52）：
 *   [0]  in_use    u8
 *   [4]  src       u64  起点节点 RID
 *   [12] dst       u64  终点节点 RID
 *   [20] out_prev  u64  起点出边链的前驱
 *   [28] out_next  u64  起点出边链的后继
 *   [36] in_prev   u64  终点入边链的前驱
 *   [44] in_next   u64  终点入边链的后继
 *
 * 【为什么一条边要挂两条链】
 *
 * 3 跳查询走的是**有向**遍历（沿出边）；但"谁指向我"的反向查询
 * 在图应用里同样高频。没有入链，反向查询 = 全表扫边 ——
 * 100 万边每次几毫秒，p99 必炸（PLAN §1.2）。
 *
 * 【双向链的维护规则】
 *
 * 一条边同时在两个双向链表里：
 *   - src 节点的**出链**（用 out_prev / out_next 串）
 *   - dst 节点的**入链**（用 in_prev / in_next 串）
 *
 * 删边必须**两个链表都摘**，否则一个方向查得到、另一个方向查不到 ——
 * 这就是 A1「双向一致性」要验的东西。
 *
 * 【RID 的编码与"无指针"】
 *
 * 记录里存 u64（rid_to_u64 / rid_from_u64）。
 * 无指针用 GR_RID_NIL = 0xFFFFFFFFFFFFFFFF。
 *
 * 为什么不用 0 当"无指针"：page 0 是元数据页，但 gstore 单独用时
 * page 0 可能是合法的图页 → 0 可能是合法 RID → 不能拿它当空指针。
 *
 * 更重要的是**不能依赖"零填充恰好是空指针"**：
 * gstore 分配槽时会把载荷清零，如果"空指针"恰好等于 0，
 * 那么"刚分配、还没写内容"的槽会看起来像"指向 page 0 的边"。
 * 所以定长记录的空指针必须是一个**不会被自动产生**的显式值，
 * 并且分配时**一次性把完整记录写进去**（见 gstore_alloc_with）——
 * 这就是 PLAN §1.7 R3「先 in_use=1 + 全字段写完才挂链」的落地。
 *
 * 【为什么节点属性用"定长列"而不是变长】
 *
 * 定长槽页的前提就是记录定长。变长属性会让节点记录变长，
 * "定长"这个前提就没了 —— 见 PLAN §1.3 的表。
 * 所以列宽在建图时固定：INT4/INT8/BOOL 用固有宽度，
 * TEXT 用建图时声明的宽度（超长报错，不截断 ——
 * 截断会让 SQL 返回的值和实际不符，是静默的数据错误）。
 *
 * 【为什么属性 schema 存在 Graph 里而不是 catalog 里】
 *
 * 硬规则 6：catalog 不知道任何模态。图的属性 schema 是图自己的事，
 * 存在图对象里；catalog 只知道"有个叫 graph_nodes 的虚拟表"。
 */

typedef struct Graph Graph;

/*
 * 图的属性列定义。
 *
 * 为什么不用 storage/record.h 的 ColDesc：ColDesc 没有宽度字段，
 * 而定长 TEXT 必须有宽度（"限 INT / FLOAT / TEXT(定长)"——
 * PLAN §1.2）。所以这里在 ColDesc 的三要素之外补一个 width。
 */
typedef struct {
    char name[COL_NAME_MAX];
    u8   type;        /* ColType：T_INT4 / T_INT8 / T_BOOL / T_TEXT */
    u16  width;       /* 定长字节数（TEXT 必填，其余由类型决定） */
    bool nullable;
} GraphCol;

/*
 * 属性列数上限。
 *
 * 15 而不是 16 —— 因为节点表在 SQL 里还要多占一列（node_id），
 * 而执行器的一行缓冲是 DbValue row[SQL_MAX_COLS]（16 列）。
 * 16 + 1 = 17 > 16 → 越界。
 *
 * 所以：属性列 ≤ 15，节点表总列数 ≤ 16，与执行器的行缓冲对齐。
 * 这条约束写在图层（而不是等 SQL 层报错），是为了让"建图时就拒"，
 * 而不是"建图成功、接进 SQL 才炸"。
 */
#define GR_MAX_COLS 15

/* 属性区总字节上限（防呆：列宽写错也不至于把页撑爆） */
#define GR_PROP_MAX 256

/* 记录内字段偏移 */
enum {
    GR_N_FIRST_OUT = GS_REC_HDR_SIZE + 0,    /* 4  */
    GR_N_FIRST_IN  = GS_REC_HDR_SIZE + 8,    /* 12 */
    GR_N_BITMAP    = GS_REC_HDR_SIZE + 16,   /* 20 */

    GR_E_SRC       = GS_REC_HDR_SIZE + 0,    /* 4  */
    GR_E_DST       = GS_REC_HDR_SIZE + 8,    /* 12 */
    GR_E_OUT_PREV  = GS_REC_HDR_SIZE + 16,   /* 20 */
    GR_E_OUT_NEXT  = GS_REC_HDR_SIZE + 24,   /* 28 */
    GR_E_IN_PREV   = GS_REC_HDR_SIZE + 32,   /* 36 */
    GR_E_IN_NEXT   = GS_REC_HDR_SIZE + 40,   /* 44 */

    GR_EDGE_SIZE   = GS_REC_HDR_SIZE + 48    /* 52 */
};

/* 无指针 */
#define GR_RID_NIL 0xFFFFFFFFFFFFFFFFull

/* ---------------- 记录内字段的读写助手 ---------------- */

/*
 * 这些 inline 放在头文件里，是因为**定义必须只有一处**：
 * graph.c（节点增删）、graph_edge.c（链的挂摘）、graph_walk.c（遍历）
 * 都要读 first_out / first_in / src / dst / 四个链指针。
 *
 * 各自写一份静态副本的话，字段偏移改一处就会漂移 ——
 * "删除只摘链"改错一个偏移，表现是"某个方向的邻居查不到"，
 * 而且不报错。这种 bug 极难查。
 *
 * 同一个道理见 gstore.h 的说明（页格式的唯一定义地）。
 */

/* u64 RID 的读写 */
static inline u64 gr_rid_get(const u8 *buf, u32 off)
{
    return get_u64(buf + off);
}

static inline void gr_rid_set(u8 *buf, u32 off, u64 v)
{
    put_u64(buf + off, v);
}

/* 节点记录里的 first_out / first_in */
static inline u64 gr_n_first_out(const u8 *rec) { return gr_rid_get(rec, GR_N_FIRST_OUT); }
static inline u64 gr_n_first_in(const u8 *rec)  { return gr_rid_get(rec, GR_N_FIRST_IN); }

/* 边记录里的四个链指针 + 两个端点 */
static inline u64 gr_e_src(const u8 *rec)      { return gr_rid_get(rec, GR_E_SRC); }
static inline u64 gr_e_dst(const u8 *rec)      { return gr_rid_get(rec, GR_E_DST); }
static inline u64 gr_e_out_prev(const u8 *rec) { return gr_rid_get(rec, GR_E_OUT_PREV); }
static inline u64 gr_e_out_next(const u8 *rec) { return gr_rid_get(rec, GR_E_OUT_NEXT); }
static inline u64 gr_e_in_prev(const u8 *rec)  { return gr_rid_get(rec, GR_E_IN_PREV); }
static inline u64 gr_e_in_next(const u8 *rec)  { return gr_rid_get(rec, GR_E_IN_NEXT); }

/* ---------------- 建 / 拆 ---------------- */

/*
 * 建图。cols 是属性列 schema（可为 NULL / ncols = 0）。
 *
 * 内部建三个 gstore：
 *   - schema 页（PT_GRAPH_SCHEMA）：存属性 schema 本身
 *   - 节点页（PT_GRAPH_NODE）
 *   - 边页（PT_GRAPH_EDGE）
 *
 * 【schema 是图的身份】
 *
 * 属性 schema 决定节点记录的**物理布局**（列宽之和 = 记录长度），
 * 所以它不是"可以后改的元数据"，而是图的身份证：
 *
 *   - 磁盘上已有 schema，且与传入的一致 → **接管**这张图
 *     （重启场景：同一个 schema 再开一次，接管上次的图）
 *   - 磁盘上已有 schema，但与传入的不一致 → E_INVAL
 *     （记录布局不同，旧记录没法解释 —— 宁可开不了，也不能读出垃圾）
 *   - 磁盘上没有 schema → **新建**，并把 schema 写进磁盘
 *
 * 【为什么 schema 必须落盘】
 *
 * 重启后要能解码节点记录，就必须知道每列的宽度和类型。
 * 这些信息只存在于 schema 里 —— 不落盘 = 重启后图变成一堆看不懂的字节。
 * A5（崩溃恢复）要求"恢复后节点数、边数、3 跳结果全对"，
 * 前提就是 schema 能被找回来。
 */
int  graph_create(Pager *pg, LogHookFn fn, void *ctx,
                  const GraphCol *cols, int ncols, Graph **out);

/*
 * 打开磁盘上已有的图。schema 从磁盘读，调用方不用（也不该）再提供。
 *
 * 磁盘上没有图 → E_NOTFOUND（这是"还没建图"，不是错误）。
 * 有图但 schema 记录损坏 → E_CORRUPT。
 */
int  graph_open(Pager *pg, LogHookFn fn, void *ctx, Graph **out);

void graph_destroy(Graph *g);

/* 这个图是不是"已就绪"（有 schema、两个 gstore 都接管完了） */
bool graph_ready(const Graph *g);

/* schema 查询 */
int            graph_col_count(const Graph *g);
const GraphCol *graph_cols(const Graph *g);
u16            graph_node_record_size(const Graph *g);

/* ---------------- 节点 ---------------- */

/*
 * 建一个节点。props 的个数必须等于列数。
 *
 * 返回的 RID 就是这个节点的"节点 id"——
 * 定长记录的天然好处：id 不需要额外分配。
 */
int graph_create_node(Graph *g, u32 xid, const DbValue *props,
                      int nprops, Rid *out);

/* 删一个节点：连带删掉它的所有出边和入边，然后释放节点槽 */
int graph_remove_node(Graph *g, u32 xid, Rid node);

/* 读节点属性。out 容量至少 graph_col_count(g) */
int graph_node_props(Graph *g, Rid node, DbValue *out, int cap);

/* ---------------- 边 ---------------- */

/* 加一条 src → dst 的边 */
int graph_add_edge(Graph *g, u32 xid, Rid src, Rid dst, Rid *out);

/* 删一条边（两个链表都摘） */
int graph_remove_edge(Graph *g, u32 xid, Rid edge);

/* 读边的两个端点 */
int graph_edge_endpoints(Graph *g, Rid edge, Rid *src, Rid *dst);

/* ---------------- 遍历 ---------------- */

/*
 * 邻居查询。out 是结果缓冲（cap 个 Rid），*n 是实际个数。
 *
 * 出邻居 = 沿出链走一步（每个出邻居是 dst 节点 RID）
 * 入邻居 = 沿入链走一步（每个入邻居是 src 节点 RID）
 *
 * 这是 PLAN §1.5 说的"遍历原语"——SQL 表达不了递归，跳出走这里。
 */
int graph_out_neighbors(Graph *g, Rid node, Rid *out, u32 cap, u32 *n);
int graph_in_neighbors(Graph *g, Rid node, Rid *out, u32 cap, u32 *n);

/* ---------------- 边链的增量游标 ---------------- */

/*
 * 沿某条链一步步走（每次一条边）。
 *
 * 【为什么需要它，而不只是 graph_out_neighbors】
 *
 * graph_out_neighbors 一次把整条链的结果倒进调用方的数组，
 * 适合"我要全部邻居"。但**下推**要的是"一行一行地吐" ——
 * SPI 的行源是"给我下一行"，不能先攒齐再给。
 *
 * 边链真下推（WHERE src = X → 只枚举 X 的出链）就必须有增量游标。
 *
 * 【out_dir 的含义】
 *
 *   true  = 走出链（节点的第一条出边开始，沿 out_next 走）
 *   false = 走入链（节点的第一条入边开始，沿 in_next 走）
 */
typedef struct {
    Rid  cur;        /* 当前边 RID（rid_is_nil 为真 = 链走完） */
    bool out_dir;    /* true = 出链；false = 入链 */
    u32  steps;      /* 已走的边数（环检测用） */
} GraphEdgeCursor;

/*
 * 打开游标：定位到节点的第一条出边 / 入边。
 * 节点不存在 → E_NOTFOUND。
 */
int graph_edge_cursor_open(Graph *g, Rid node, bool out_dir, GraphEdgeCursor *c);

/*
 * 取下一条边。链走完返回 E_EOF。
 * edge / src / dst 都可以传 NULL（不想要就别填）。
 */
int graph_edge_cursor_next(Graph *g, GraphEdgeCursor *c,
                           Rid *edge, Rid *src, Rid *dst);

/* ---------------- 统计 / 枚举 ---------------- */

u64 graph_node_count(Graph *g);
u64 graph_edge_count(Graph *g);

/* 枚举所有节点 / 所有边（回调返回非 OK 则停止） */
typedef int (*GraphEachFn)(void *ctx, Rid rid);
int graph_each_node(Graph *g, GraphEachFn fn, void *ctx);
int graph_each_edge(Graph *g, GraphEachFn fn, void *ctx);

/* ---------------- 内部结构（实现细节，不属于公开 API） ---------------- */

/*
 * 图对象。公开 API 全走指针，这个结构只有 graph.c / graph_prop.c 用。
 *
 * 为什么两个 gstore 而不是两个页类型塞进一个：
 * gstore 的页链是"同类页串成链"，两种记录大小不同
 * （节点大小随 schema 变、边固定 52），一个 gstore 只能有一种 rec_size。
 */
struct Graph {
    Pager      *pg;
    LogHookFn   wal;
    void       *wal_ctx;

    GStore     *schema;     /* PT_GRAPH_SCHEMA：属性 schema 本身 */
    GStore     *nodes;      /* PT_GRAPH_NODE */
    GStore     *edges;      /* PT_GRAPH_EDGE */

    GraphCol    cols[GR_MAX_COLS];
    int         ncols;
    u16         col_off[GR_MAX_COLS];  /* 各列在记录中的偏移 */
    u16         bitmap_bytes;          /* NULL 位图字节数 */
    u16         node_size;             /* 节点记录总长 */

    bool        ready;      /* schema + 两个 gstore 都就位了 */
};

/*
 * 节点 / 边记录的读写助手。
 *
 * 同样放在头文件里（理由同上：定义只有一处）。
 * 它们是一行一层的转发 —— 图层的数据都住在两个 gstore 里，
 * 读写记录就是"按 RID 取/写一个槽"，没有别的逻辑。
 */
static inline int gr_read_node(Graph *g, Rid rid, u8 *buf)
{
    return gstore_get(g->nodes, rid, buf, g->node_size, NULL);
}

static inline int gr_read_edge(Graph *g, Rid rid, u8 *buf)
{
    return gstore_get(g->edges, rid, buf, GR_EDGE_SIZE, NULL);
}

static inline int gr_write_node(Graph *g, u32 xid, Rid rid, const u8 *buf)
{
    return gstore_put(g->nodes, xid, rid, buf, g->node_size);
}

static inline int gr_write_edge(Graph *g, u32 xid, Rid rid, const u8 *buf)
{
    return gstore_put(g->edges, xid, rid, buf, GR_EDGE_SIZE);
}

/* graph_prop.c：把 DbValue[] 编解码成记录里的定长属性区 */
int gr_prop_encode(Graph *g, const DbValue *vals, int nvals, u8 *buf);
int gr_prop_decode(Graph *g, const u8 *buf, DbValue *out, int cap);

/*
 * schema 记录的磁盘格式（graph_schema.c 使用）。
 *
 * 格式常量放在头文件里，是因为**定义必须只有一处**：
 * graph.c 要用 GR_SCHEMA_REC_SIZE 去建 gstore（rec_size 必须精确），
 * graph_schema.c 要用它来编解码。两边各写一份 = 迟早对不上。
 */
#define GR_SCHEMA_VER      1
#define GR_SCHEMA_HEAD     4                         /* version + ncols + node_size */
#define GR_SCHEMA_COL      (COL_NAME_MAX + 4)       /* name + type + width + nullable */
#define GR_SCHEMA_PAYLOAD  (GR_SCHEMA_HEAD + GR_MAX_COLS * GR_SCHEMA_COL)

/*
 * 记录总长 = gstore 公共头 + 载荷。
 *
 * 【为什么载荷前面要留 GS_REC_HDR_SIZE】
 *
 * gstore 的槽布局是：[in_use(1)][保留(1)][next_free(2)][载荷…]。
 * gstore_alloc_with 拷贝的只是**载荷区**（从 init 的第 4 字节起）——
 * 公共头归 gstore 自己管。
 *
 * 所以 schema 的数据必须放在记录的 GS_REC_HDR_SIZE 之后，
 * 否则前 4 个字节（version / ncols / node_size）会被丢掉 ——
 * 而且丢得很安静：写的时候不报错，读的时候才发现 version 不对。
 */
#define GR_SCHEMA_REC_SIZE (GS_REC_HDR_SIZE + GR_SCHEMA_PAYLOAD)

/*
 * graph_schema.c：属性 schema 的落盘 / 读回。
 *
 * schema 存在一个**独立的 gstore** 里（页类型 PT_GRAPH_SCHEMA），
 * 而不是塞进节点记录、也不是塞进 catalog：
 *
 *   - 不塞进节点记录：schema 是"所有节点的共同约定"，
 *     每条记录都存一份是纯浪费（而且改 schema 会改所有记录）。
 *   - 不塞进 catalog：硬规则 6 —— catalog 不知道任何模态。
 *     图的属性 schema 是图自己的事，存在图对象里。
 *
 * 独立 gstore 的好处是**零新代码**：页的分配 / WAL / 重启接管
 * 全是 gstore 现成的能力（页链头靠 GS_FLAG_DIR 标志扫全文件找）。
 */
int gr_schema_read(GStore *gs, GraphCol *cols, int max, int *ncols, u16 *node_size);
int gr_schema_write(GStore *gs, const GraphCol *cols, int ncols, u16 node_size);
int gr_schema_equal(const GraphCol *a, int na, u16 sa,
                    const GraphCol *b, int nb, u16 sb);

#endif /* DB_MODALITIES_GRAPH_GRAPH_H */
