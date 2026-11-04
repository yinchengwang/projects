#ifndef DB_SRC_DB_INTERNAL_H
#define DB_SRC_DB_INTERNAL_H

/*
 * db.c / db_blob.c / db_instance.c / db_result.c 共享的内部定义。
 *
 * 【为什么要拆文件】
 * 硬规则：单文件不超过 800 行。db.c 一度到 1075 行，后来又到 888 行。
 * blob 的公开 API 是一块完整的功能，单独成文件正好；
 * 关库路径（db_instance.c）和结果集访问器（db_result.c）同理。
 *
 * 这些定义**不是**公共 API —— 公共 API 只有 include/db.h。
 */

#include "db.h"

#include "catalog/catalog.h"
#include "modalities/ft/ft_am.h"
#include "modalities/ft/ft_index.h"
#include "modalities/geo/geo_am.h"
#include "modalities/geo/geo_state.h"
#include "modalities/graph/graph.h"
#include "modalities/graph/graph_am.h"
#include "modalities/vector/hnsw_am.h"
#include "modalities/vector/vector_am.h"
#include "modalities/vector/vector_state.h"
#include "query/exec.h"
#include "spi/blob_am.h"
#include "spi/registry.h"
#include "storage/blob.h"
#include "storage/pager.h"
#include "txn/mvcc.h"
#include "txn/txn.h"
#include "wal/recovery.h"
#include "wal/wal.h"

/*
 * 恢复重放集合：一份 doc_id 的动态数组。
 *
 * 恢复阶段的钩子把 WAL 里的 tombstone 记录收进这里，
 * ft_index_recover() 再拿它们去装配最终的删除标记集合。
 */
typedef struct {
    u32 *ids;
    u32 n;
    u32 cap;
} FtReplaySet;

/*
 * 实例：所有连接共享的东西。
 *
 * 用引用计数：最后一个连接关掉时才真正关库。
 */
typedef struct DbInstance {
    Pager    *pager;
    Catalog  *cat;
    Wal      *wal;
    BlobStore *blobs;       /* blob 模态（实例级共享：所有连接看同一片对象空间）*/
    TxnTable  txns;         /* 全局事务表：xid 分配 + clog */

    /*
     * 模态注册表（v0.9）：名字 → StorageAM。
     *
     * 实例级共享 —— 所有连接看到同一批虚拟表。
     * 里面登记的是 blob 模态提供的两张表（blob_frames / blob_objects），
     * 以及图模态的两张表（graph_nodes / graph_edges）。
     */
    SpiRegistry *reg;

    /* blob 模态的 SPI 适配（实例级：所有连接用同一份） */
    StorageAM  blob_am_frames;
    StorageAM  blob_am_objects;
    BlobAmCtx  blob_ctx_frames;
    BlobAmCtx  blob_ctx_objects;

    /*
     * 图模态（v0.11）。
     *
     * 和 blob 一样是**实例级共享**：所有连接看同一张图。
     *
     * graph 为 NULL 表示"这个库还没建过图"——
     * 这时 graph_nodes / graph_edges 这两张虚拟表也不存在，
     * 查它们会得到"表不存在"。
     */
    Graph      *graph;
    bool        graph_ready;
    StorageAM   graph_am_nodes;
    StorageAM   graph_am_edges;
    GraphAmCtx  graph_ctx_nodes;
    GraphAmCtx  graph_ctx_edges;

    /*
     * 向量模态（v1.0）。
     *
     * 同样是**实例级共享**：所有连接看同一批向量。
     *
     * vstate 为 NULL 表示"这个库还没建过向量存储"——
     * 这时 vec_items 这张虚拟表也不存在，查它会得到"表不存在"。
     *
     * 向量的三样东西（数据 / 内存索引 / 磁盘索引）绑在 vstate 里，
     * 见 modalities/vector/vector_state.h。
     */
    VectorState *vstate;
    bool         vec_ready;
    StorageAM    vec_am;
    VectorAmCtx  vec_ctx;
    IndexAM      vec_index_am;      /* HNSW 的 Index AM（knn 的真实现） */
    HnswAmCtx    vec_index_ctx;

    /*
     * 全文检索模态（v1.3）。
     *
     * 同样是**实例级共享**：所有连接看同一批文档。
     *
     * ⚠️ **本版的索引只在内存里**：段的内容在磁盘上，
     * 但段的清单与 tombstone 集合不落盘（步骤 5 才做）。
     * 所以重开库得到的是一个**空索引** —— 打开时建一个空的，
     * 不假装能从磁盘上恢复（见 db_ft_open 的注释）。
     */
    FtIndex     *ft;
    bool         ft_ready;
    StorageAM    ft_am;
    FtAmCtx      ft_ctx;

    /*
     * 全文检索的**恢复重放集合**（v1.3 步骤 5）。
     *
     * 恢复阶段，WAL 层的钩子把日志里的 tombstone 记录收进这两个集合：
     *   - redo：重放看到的删除；
     *   - undo：回滚撤销的删除（未提交事务的）。
     *
     * 它们只活到 ft_index_recover() 跑完（用完即释放）。
     */
    FtReplaySet  ft_replay_redo;
    FtReplaySet  ft_replay_undo;

    /*
     * 地理空间模态（v1.4）。
     *
     * 同样是**实例级共享**：所有连接看同一批点。
     *
     * geo 为 NULL 表示"这个库还没建过几何存储"——
     * 这时 geo_points 这张虚拟表也不存在，查它会得到"表不存在"，
     * 而不是"查出一张空表"（与图模态同一个口径）。
     *
     * ⚠️ **geo 的写没有事务边界**：树页与 meta 页进 WAL
     *    （整页镜像，v1.4 步骤 6 · A7；钩子由 db 层注入 —— 见 db_geo.c），
     *    但日志**只用于 redo，不用于 undo**（xid 固定 0 = 系统事务）。
     *    将来要把 geo 的写纳入用户事务，必须同时补真正的 undo。
     *    另外坑 M5-16：点只存在树里（没有第二份行存），
     *    WAL 是**唯一**的恢复路径 —— 没有 WAL ⇒ 崩溃后这批点就是没了。
     *
     * geo_tree_reads_seen —— 上一次记账时 rtree_page_reads(tree) 的值。
     *
     * 为什么要在实例里存一份：rtree_page_reads 是**累计值**，
     * 每次查询后再加一遍会把历史读数重复计入。
     * 所以这里存"上次看到的累计值"，每次只把**增量**加进
     * GeoState.page_reads（那个才是对外的总数）。
     *
     * reindex 换了一棵新树 → 累计值从头开始 → 增量算出来是 0，
     * 逻辑自然成立（见 db_geo.c 的 geo_account_tree_reads）。
     */
    GeoState    *geo;
    bool         geo_ready;
    StorageAM    geo_am;
    GeoAmCtx     geo_ctx;
    u64          geo_tree_reads_seen;

    char     *walpath;
    char     *clogpath;     /* <db>-clog：提交状态日志（v1.2 步骤 6）*/
    int       nconns;       /* 引用计数 */
} DbInstance;

/*
 * ---------------- checkpoint（v1.2 步骤 6，db_ckpt.c 实现） ----------------
 */

/*
 * 做一次 checkpoint：刷页 + 写 meta 页的重放点 + 落 clog + 截日志。
 *
 * allow_truncate = false 时只刷页、不截日志。
 */
int  db_checkpoint_internal(DbInstance *inst, bool allow_truncate);

/* 阈值触发：日志超过阈值就顺手做一次 checkpoint。 */
void db_checkpoint_maybe(DbInstance *inst);

/*
 * 日志里出现过的每个 xid，是不是都有 COMMIT 记录？
 *
 * 关库时用它决定"能不能写干净关闭标记"——
 * 还有没提交的事务却写了标记，恢复阶段会跳过重放和回滚，
 * 那个事务的物理删除就永久生效了。
 */
bool db_wal_all_committed(Wal *w, bool *ok);

/*
 * 把向量模态的两样东西接进注册表（db_vec.c 实现）。
 *
 * 注册表只知道"名字 → 一个 StorageAM"，
 * 所以加第 3 个模态不需要改注册表，更不需要改执行器。
 */
int db_vec_register(DbInstance *inst);

/* 打开磁盘上已有的向量存储（没有 → E_NOTFOUND） */
int  db_vec_open(DbInstance *inst);

/* 关库：落盘索引 + 释放（必须在 pager_close 之前调） */
void db_vec_close(DbInstance *inst);

/*
 * ---------------- 全文检索模态（v1.3，db_ft.c 实现） ----------------
 */

/* 往集合里追加一个 doc_id */
int  db_ft_replay_add(FtReplaySet *set, u32 doc_id);

/* 释放集合 */
void db_ft_replay_free(FtReplaySet *set);

/*
 * 恢复钩子（WAL 层不认识 FT 语义，由这一层解释）。
 *
 * WAL 层在重放/回滚时遇到**非页类**记录就调这个钩子，
 * 把 (type, xid, page_id, lsn, payload, payload_len, phase) 原样转交过来。
 *
 * 这里认两种记录：
 *   - WAL_FT_TOMB（旧的，已退役）：载荷 = u32 doc_id。
 *       phase = REC_HOOK_REDO → 收进 redo 集合（这个删除生效了）；
 *       phase = REC_HOOK_UNDO → 收进 undo 集合（未提交事务 → 这个删除没发生过）。
 *   - WAL_FT_TOMB_APPEND（v1.3 欠账 1）：载荷 = u32 doc_id，
 *     page_id = 落在哪张 tombstone 页上。
 *       直接改页：重放 = 追加（幂等），回滚 = 只移除那一条。
 */
int db_ft_recovery_hook(void *ctx, u8 type, u32 xid, u32 page_id, u32 lsn,
                        const u8 *payload, u16 payload_len, RecHookPhase phase);

/*
 * 把 ft_docs 虚拟表接进注册表（仿 db_vec_register）。
 *
 * 注册表只知道"名字 → 一个 StorageAM"，
 * 所以加第 4 个模态不需要改注册表，更不需要改执行器。
 */
int db_ft_register(DbInstance *inst);

/*
 * 开库时建索引 + **从磁盘恢复**（扫页空间找段与 tombstone 页）。
 *
 * 必须在恢复重放之后调 —— WAL 里的删除要先进集合。
 */
int db_ft_open(DbInstance *inst);

/* 关库：释放索引（必须在 pager_close 之前调） */
void db_ft_close(DbInstance *inst);

/*
 * ---------------- 地理空间模态（v1.4，db_geo.c 实现） ----------------
 */

/*
 * 摊平后的点表：按 row_id 索引存坐标与死活。
 *
 * 【为什么按 row_id 索引】
 *
 * 查询返回的是 row_id，要给它补上坐标才能算距离 —— 索引查才 O(1)。
 * 容量取"总条目数"：row_id 从 0 连续分配，所以 row_id < n_total
 * 恒成立（不成立就是数据坏了 → E_CORRUPT）。
 *
 * db_geo.c 与 db_geo_query.c 共用，所以定义放在这里。
 */
typedef struct {
    float x;
    float y;
    bool  dead;
} GeoPtEntry;

typedef struct {
    GeoPtEntry *p;
    u32    cap;            /* 容量（= 总条目数） */
    u32    n;              /* 扫到的条目数 */
    u32    live;           /* 活条目数 */
    u32    dead_n;         /* 死条目数 */
} GeoTab;

/* 扫一遍全树，把点摊平进表里（db_geo.c 实现，db_geo_query.c 也用） */
int  db_geo_flatten(DbInstance *inst, GeoTab *tab);

/* 释放摊平表 */
void db_geo_tab_free(GeoTab *tab);

/*
 * 把 geo_points 虚拟表接进注册表（仿 db_vec_register）。
 *
 * 注册表只知道"名字 → 一个 StorageAM"，
 * 所以加第 5 个模态不需要改注册表，更不需要改执行器。
 */
int db_geo_register(DbInstance *inst);

/*
 * 开库时接管磁盘上已有的几何存储。
 *
 * 磁盘上没有 → E_NOTFOUND（**不是错误**：那是个还没建过几何存储的库，
 * 和图模态一样），由调用方决定是不是错误。
 */
int db_geo_open(DbInstance *inst);

/*
 * 关库：释放模态状态（必须在 pager_close 之前调）。
 *
 * **幂等**：inst->geo == NULL 时直接返回，
 * 所以多条失败清理路径重复调用也安全。
 */
void db_geo_close(DbInstance *inst);

/*
 * WAL 钩子（db.c 实现，db_graph.c 复用）。
 *
 * 【为什么不是 db.c 的 static 函数】
 *
 * 图模态的页也要进 WAL —— 邻接链指针是数据，日志里没有就重放不回来。
 * 而钩子的 ctx 必须是**实例**（不是某个连接句柄）：
 * 钩子要活得比任何单个连接都久，owner 先关掉时它的 ctx 不能被 free。
 *
 * 所以它是"实例级共享资源"的一部分，声明放在 db_internal.h。
 */
int db_wal_hook(void *ctx, u32 xid, u8 logtype, u32 page_id, u16 slot,
                u16 slot_off, const u8 *before, u16 blen,
                const u8 *after, u16 alen, u32 *lsn_out);

/*
 * 把图的两张虚拟表接进注册表（db_graph.c 实现）。
 *
 * 单独一个函数，是因为"建 AM + 注册"这一步在两处发生：
 *   - db_open：打开时磁盘上已经有图 → 注册
 *   - db_graph_create：本次会话里现建图 → 注册
 */
int db_graph_register(DbInstance *inst);

/*
 * 连接：一个客户端会话。
 *
 * 大部分字段和 v0.6 的 DB 结构体一样，但事务相关的是私有的。
 */
struct DB {
    DbInstance *inst;
    Db          db;         /* exec 层看到的内核结构（pager/cat/txns/snap）*/

    u32     xid;            /* 当前事务号（连接私有）*/
    char    errmsg[256];    /* 连接私有 */
    bool    in_txn;         /* 是否处于显式事务中 */
    bool    txn_failed;     /* 事务内出过错，需要 ROLLBACK */
    bool    is_owner;       /* 是否是拥有实例的句柄（db_open 返回的那个）*/

    /*
     * 隔离级别（连接私有）。
     *
     * 默认 RC（对齐 PG）。RR 需要用户显式声明：
     *   SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;
     *
     * 【为什么隔离级别是连接私有的】
     *
     * 它描述的是"这个连接看世界的方式"，不是库的属性。
     * 两个连接可以有不同级别 —— A 用 RR 做报表（要一致的数据），
     * B 用 RC 做交互（要看到最新数据）。
     */
    TxnIsolation iso_level;

    /*
     * 隔离级别是否已锁定。
     *
     * PG 的规则：事务里改了隔离级别后，就不能再改 —— 必须先提交/回滚，
     * 再开新事务。这样一个事务内所有语句的隔离级别必然一致。
     *
     * （注意：不能用"cur_xid 是否为 0"来判断"是不是第一条语句"——
     *   cur_xid 在事务开始时就非 0 了，判断不出这个。）
     */
    bool iso_locked;
};

struct Result {
    QueryResult *qr;
};

/* 内部错误码 → 公共错误码 */
db_err map_rc(int rc);

#endif /* DB_SRC_DB_INTERNAL_H */
