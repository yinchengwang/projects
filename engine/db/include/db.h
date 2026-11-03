#ifndef DB_DB_H
#define DB_DB_H

/*
 * 唯一公共 API。外部只 include 这个文件，其余一律是内部实现。
 * 目标：让"能不能对外用"和"内部怎么组织"彻底解耦。
 */

#include <stdint.h>
#include <stddef.h>

typedef struct DB     DB;      /* opaque */
typedef struct Result Result;  /* opaque */

typedef enum {
    DB_OK = 0,
    DB_E_IO,
    DB_E_NOSPACE,
    DB_E_CORRUPT,
    DB_E_PARSE,
    DB_E_TXN,
    DB_E_INVAL,
    DB_E_NOTFOUND,
    DB_E_NOTSUP       /* 操作不支持（能力协商：这张表/这个索引没这个能力） */
} db_err;

/* 生命周期 */
db_err db_open(const char *path, DB **out);
db_err db_close(DB *db);

/*
 * 放弃这个句柄：**不刷脏页、不写 CHECKPOINT**，等价于进程被 kill -9。
 * 只用于崩溃恢复测试。生产代码请用 db_close。
 */
void   db_abandon(DB *db);

/*
 * 开一个新连接，共享同一个数据库实例（缓冲池、catalog、WAL）。
 *
 * 【为什么要连接】
 *
 * v0.7 之前只有一个 DB 句柄 = 一个连接 = 一个事务上下文。
 * 但 MVCC 的意义在于**多个连接同时看同一个库，各自看到不同的时间点**。
 * 没有连接抽象就没法测这个。
 *
 * 【连接之间共享什么、不共享什么】
 *
 * 共享：缓冲池（同一份数据页）、catalog、WAL 文件、事务表（全局 xid 分配）
 * 不共享：当前事务号、当前快照、事务状态（in_txn / failed）
 *
 * 这正是 PG 的 postmaster/backend 模型：
 *   postmaster（共享：缓冲池、catalog）→ backend（每个连接一份：事务状态）
 *
 * 【句柄的所有权】
 *
 * db_open 返回的句柄是「实例句柄」，它同时也是第一个连接。
 * db_connect 在它上面开新连接。新连接必须用 db_conn_close 关
 * （不能关实例句柄）。
 */
db_err db_connect(DB *db, DB **out);
void   db_conn_close(DB *conn);

/* 执行：一条语句，结果放 Result（非 SELECT 语句返回 affected 行数） */
db_err db_exec(DB *db, const char *sql, Result **out);

/*
 * ---------------- checkpoint（v1.2 步骤 6） ----------------
 *
 * 手动做一次 checkpoint：刷所有脏页、把重放点写进 meta 页、
 * 提交状态落盘、然后截断日志。
 *
 * 效果：恢复时间只取决于"上次 checkpoint 之后写了多少日志"，
 * 与库的总大小无关。
 *
 * 什么时候用：
 *   - 想让下次启动更快（写完一大批数据之后）
 *   - 想让日志文件别一直涨
 *
 * 代价：checkpoint 要刷所有脏页 + fsync，是一次明显的停顿。
 *
 * 【什么时候不会截断日志】
 * 还有未提交的事务时不截 —— 未提交事务的日志记录是回滚的依据，
 * 截掉了那行被物理删掉的数据就再也回不来了。
 * （刷页和写重放点照做。）
 */
db_err db_checkpoint(DB *db);

/* 结果集访问（row-major）。NULL 值返回字符串 "NULL"。 */
int         db_result_rows(const Result *r);
int         db_result_cols(const Result *r);
int         db_result_affected(const Result *r);
const char *db_result_colname(const Result *r, int col);
const char *db_result_value(const Result *r, int row, int col);
void        db_result_free(Result *r);

/* 错误描述 */
const char *db_strerror(db_err e);

/*
 * 最近一次失败的**详细原因**（连接私有）。
 *
 * db_strerror 给的是错误类别（"操作不支持"），
 * 这个给的是具体句子（"表 blob_frames 不支持插入"）。
 *
 * 没有失败过返回空串。
 */
const char *db_last_error(DB *db);

/*
 * ---------------- blob 模态（v0.8） ----------------
 *
 * 独立的大对象存储：对象切成等长的帧，读第 N 帧的 IO 量与对象大小无关。
 *
 * 【为什么是独立的一层，不是关系模态的一个类型】
 *
 *   寻址方式不同：blob 用 blob_id 寻址，关系模态用 RID (page, slot)。
 *   生命周期不同：blob 对象不可变，没有 xmin/xmax，没有 MVCC。
 *   页管理不同：blob 的页不进任何表的 freelist。
 *
 * 但共享底座：pager（缓冲池）、WAL（崩溃安全）、页格式。
 *
 * 【对象不可变】
 *
 * 创建 → 写 → 定稿。写完就不能改（和 S3 的对象一样）。
 * 这带来两个好处：
 *   1. 每个页只写一次 ⇒ WAL 只需整页镜像，恢复逻辑零改动。
 *   2. 没有并发写同一对象的问题 ⇒ 不需要锁。
 */

/* blob 对象 id。0 = 无效（保留给"没有对象"） */
typedef uint64_t db_blob_id;

/*
 * 创建一个对象，预留 nframes 帧的空间。
 *
 * frame_size 传 0 表示用默认帧大小（一页的数据区）。
 * 创建后对象是空的，用 db_blob_write 写入内容。
 */
db_err db_blob_create(DB *db, uint64_t nframes, uint32_t frame_size,
                      db_blob_id *blob_id_out);

/*
 * 写对象内容。
 *
 * 对象不可变 —— 这是"最后一次写"，写完内容定稿。
 * len 必须 <= 创建时预留的容量。
 *
 * 不参与事务：blob 对象写完就是写完，不存在"回滚到旧版本"。
 * 但它会写 WAL，所以崩溃后数据不丢。
 */
db_err db_blob_write(DB *db, db_blob_id id, const void *data, uint64_t len);

/*
 * 读第 frame_no 帧 —— 核心操作。
 *
 * IO 量 ≈ 一帧（4KB），与对象大小、帧号都无关。
 * buf 容量至少一帧的大小；*len_out 是这一帧实际有多少字节。
 */
db_err db_blob_read_frame(DB *db, db_blob_id id, uint64_t frame_no,
                           void *buf, uint32_t cap, uint32_t *len_out);

/* 读整个对象。buf 容量至少对象总长。 */
db_err db_blob_read(DB *db, db_blob_id id, void *buf, uint64_t cap, uint64_t *len_out);

/* 对象元信息。参数传 NULL 表示不关心该项。 */
db_err db_blob_stat(DB *db, db_blob_id id,
                    uint64_t *nframes, uint32_t *frame_size, uint64_t *total_len);

/*
 * 列出所有对象。
 *
 * blob 有自己的目录（不依赖关系模态的 catalog）——
 * 模态之间不许互相依赖，这是 SPI 的硬约束。
 */
uint32_t db_blob_count(DB *db);

/* 取第 i 个对象的 id。越界返回 0（无效 id）。 */
db_blob_id db_blob_id_at(DB *db, uint32_t i);

/*
 * 诊断：读帧引起的页读次数。
 *
 * 用来观察 IO 量 —— 比如验证"只查元数据列不读帧页，
 * 取内容列才读"。这是外部值惰性的直接证据。
 */
uint64_t db_blob_page_reads(DB *db);

/*
 * ---------------- 图模态（v0.11） ----------------
 *
 * 图 = 节点 + 有向边。节点和边都是**定长记录**，各自有自己的 RID 空间
 * （节点槽和边槽互不相干，id 不能互相比较）。
 *
 * 【为什么遍历要走 C API，而不是 SQL】
 *
 * SQL 表达不了递归。"3 跳"= 沿出边走 3 层 —— 没有递归 CTE 就没有 SQL 写法。
 * 所以图模态的公开 API 提供"跳一步"和"跳 N 步"两个原语，
 * 而标量查询（WHERE prop = X、WHERE src = X）走虚拟表。
 *
 * 【为什么属性 schema 是"建图时一次性声明"的】
 *
 * 节点记录是定长的：长度 = 公共头 + NULL 位图 + 各列宽度之和。
 * 列宽一旦定下，记录长度就定死（定长槽页的前提）。
 * 所以 schema 是图的身份证，不能改 —— 改了旧记录就没法解释。
 * schema 会**落盘**，重启后自动接回来。
 *
 * 【事务语义】
 *
 * 图走**语句级提交**：每条语句一个隐式事务，不参与 BEGIN/COMMIT。
 * 可见性恒真（没有版本链）。写操作进 WAL，所以崩溃不丢数据。
 */

/* 节点或边的标识（RID 的 u64 编码）。不透明，调用方只用来回传。 */
typedef uint64_t db_graph_rid;

/* 列名 / 文本的长度上限（与内部值层一致） */
#define DB_GRAPH_NAME_MAX 32
#define DB_GRAPH_TEXT_MAX 256

/* 属性列的类型。限 INT / BOOL / 定长 TEXT —— 变长属性本版不做。 */
typedef enum {
    DB_GRAPH_INT4 = 1,
    DB_GRAPH_INT8 = 2,
    DB_GRAPH_BOOL = 3,
    DB_GRAPH_TEXT = 4
} db_graph_type;

/*
 * 属性列定义（建图时声明）。
 *
 * width 是**定长字节数**，只有 TEXT 需要填：
 * TEXT 必须定宽，否则节点记录就不是定长的了。
 * 声明了宽度就要守住它 —— 超长报错，不截断（截断 = 静默的数据错误）。
 */
typedef struct {
    char          name[DB_GRAPH_NAME_MAX];
    db_graph_type type;
    int           width;      /* TEXT：字节数；其余忽略 */
    int           nullable;
} db_graph_col;

/* 属性值。自包含（不持有堆内存），可以按值随意拷贝。 */
typedef struct {
    db_graph_type type;
    int           is_null;
    int64_t       i;                       /* INT4 / INT8 / BOOL */
    char          s[DB_GRAPH_TEXT_MAX];    /* TEXT */
} db_graph_value;

/*
 * 建图。
 *
 * 一个库里有**一张**图。schema 一经确定不可更改
 * （记录布局定死，改了就解释不了旧记录）。
 * 已经建过图再调 → DB_E_INVAL。
 */
db_err db_graph_create(DB *db, const db_graph_col *cols, int ncols);

/* 这个库建过图了吗？（没建图时 graph_nodes / graph_edges 这两张表也不存在） */
int    db_graph_exists(DB *db);

/* 建一个节点。props 的个数必须等于建图时声明的列数。 */
db_err db_graph_add_node(DB *db, const db_graph_value *props, int nprops,
                         db_graph_rid *out);

/* 加一条 src → dst 的边。 */
db_err db_graph_add_edge(DB *db, db_graph_rid src, db_graph_rid dst,
                         db_graph_rid *out);

/* 删节点（连带删掉它的所有出边和入边）/ 删边（两个链都摘）。 */
db_err db_graph_remove_node(DB *db, db_graph_rid node);
db_err db_graph_remove_edge(DB *db, db_graph_rid edge);

/*
 * 跳一步：node 的出邻居 / 入邻居。
 *
 * 返回的数组是**堆上分配的**，调用方用 db_graph_free 释放。
 * （用调用方的缓冲更省事，但"出度未知"这件事会让调用方很难受：
 *  要么先问一次度数，要么给一个猜的容量然后重试。）
 *
 * node 不存在 → DB_E_NOTFOUND。
 */
db_err db_graph_hop(DB *db, db_graph_rid node, db_graph_rid **out, uint32_t *n_out);
db_err db_graph_hop_in(DB *db, db_graph_rid node, db_graph_rid **out, uint32_t *n_out);

/*
 * 跳 N 步（广度优先，**去重**）。
 *
 * depth = 1 等价于 db_graph_hop。depth = 0 返回起点自己。
 * 结果里同一个节点只出现一次（一个节点可以从多条路径到达）。
 */
db_err db_graph_walk(DB *db, db_graph_rid node, uint32_t depth,
                     db_graph_rid **out, uint32_t *n_out);

/* 读节点的属性。out 的容量至少是列数。 */
db_err db_graph_get_node(DB *db, db_graph_rid node,
                         db_graph_value *out, int cap, int *n_out);

/* 读边的两个端点（节点 id）。 */
db_err db_graph_get_edge(DB *db, db_graph_rid edge,
                         db_graph_rid *src, db_graph_rid *dst);

/* 属性列数（建图时声明了多少列就是多少）。没建图 → 0。 */
int db_graph_col_count(DB *db);

/*
 * 读回图的属性 schema。
 *
 * out 的容量至少是 db_graph_col_count(db) 那么多。
 * 为什么还要能读回来：schema 是建图时定的，但用图的人未必是建图的人 ——
 * 光写代码不看 schema 的话，插节点时根本不知道该填几个值、什么类型。
 */
db_err db_graph_schema(DB *db, db_graph_col *out, int cap, int *n_out);

/* 列出所有节点 / 所有边（堆上分配，用 db_graph_free 释放）。 */
db_err db_graph_list_nodes(DB *db, db_graph_rid **out, uint32_t *n_out);
db_err db_graph_list_edges(DB *db, db_graph_rid **out, uint32_t *n_out);

/* 统计。参数传 NULL 表示不关心该项。 */
db_err db_graph_count(DB *db, uint64_t *nodes, uint64_t *edges);

/*
 * 诊断：图模态累计取出的记录条数（节点表 / 边表分开）。
 *
 * 【为什么需要这个可观测量】
 *
 * 用来区分"真下推"和"全扫 + 过滤"——
 * 这两种实现的**返回行集完全一样**，光看查询结果验不出效率差异。
 * 但它们读的记录数差一个数量级：
 *   - 真下推：只读匹配链上那几条（边的度数）
 *   - 全扫：读了全表每一条
 *
 * 和 db_blob_page_reads 是同一个思路：**用可观测量证明"少读了"**，
 * 而不是靠读代码相信。
 */
db_err db_graph_reads(DB *db, uint64_t *node_reads, uint64_t *edge_reads);

/* 释放 db_graph_hop / hop_in / walk / list_* 返回的数组。 */
void db_graph_free(void *p);

/* ---------------- 向量模态（v1.0） ---------------- */

/*
 * 向量 = 模态 3（列式布局 + HNSW）。
 *
 * 【和图模态的分工】
 *
 * 图模态的遍历走 C API（SQL 表达不了递归）。
 * 向量模态的**近邻查询**同理 —— 排序键是计算出来的距离，
 * 不是列值，SQL 里没有这种算子。
 *
 * 所以本模态在 SQL 里只提供 `vec_items` 虚拟表（元数据查询 + 标量过滤），
 * 近邻查询走下面的 API。**这是有意的范围收缩，不是遗漏。**
 */

/* 一个近邻命中 */
typedef struct {
    uint64_t row_id;   /* 向量的全局序号 */
    float    dist;     /* 距离（越小越近；L2 是平方距离） */
} db_vec_hit;

/*
 * 建向量存储。
 *
 * dim 是向量维度，m 是 HNSW 的每层邻居数，efc 是构建期的候选池宽度。
 * 默认值：m = 16、efc = 200（hnswlib 同款）。传 0 表示用默认值。
 *
 * 维度是存储的身份：磁盘上已有且维度不同 → 报错，不覆盖。
 * 只能建一次（第二次建 → DB_E_INVAL）。
 */
db_err db_vec_create(DB *db, uint16_t dim, uint16_t m, uint16_t efc);

/* 这个库有没有向量存储 */
int db_vec_exists(DB *db);

/* 向量存储的维度 */
uint16_t db_vec_dim(DB *db);

/*
 * 插入一个向量。
 *
 * 向量按插入顺序分配 row_id（从 0 开始，单调递增，不复用）。
 * label 是标量属性（供过滤用）。
 *
 * 插入会同时更新 HNSW 索引 —— 索引和向量在同一个临界区里保持一致。
 */
db_err db_vec_insert(DB *db, const float *vec, int32_t label,
                     uint64_t *row_id_out);

/*
 * 近邻查询（走 HNSW 索引）。
 *
 * ef 是候选池宽度：越大越准越慢。传 0 表示用默认值（64）。
 * out 至少要能放 k 个。*n_out 返回实际找到几个。
 *
 * 返回的是"**近似**最近邻" —— 召回率随 ef 上升。
 * 要精确结果用 db_vec_knn_exact。
 */
db_err db_vec_knn(DB *db, const float *q, uint32_t k, uint16_t ef,
                  db_vec_hit *out, uint32_t *n_out);

/*
 * 精确近邻查询（暴力扫全部向量）。
 *
 * 这是 db_vec_knn 的对照基准：它读**全部**向量页，
 * 而 ANN 只读索引碰到的那些 —— 页读数差一个数量级（可用 db_vec_page_reads 量）。
 */
db_err db_vec_knn_exact(DB *db, const float *q, uint32_t k,
                        db_vec_hit *out, uint32_t *n_out);

/*
 * 带过滤的近邻查询。
 *
 * 【语义：ANN 给超集，如实报告实际数量】
 *
 * 内部把候选池拉高到至少 4k（PLAN §1.7 判决 c），
 * 再按 label 过滤。**过滤后可能少于 k 个** ——
 * 这是 ANN 的固有性质（近似），不是 bug。
 *
 * 为什么不能"先取 top-k 再过滤"：那样如果 top-k 都不满足过滤条件，
 * 结果会**少于 k 且不可补** —— 静默丢数据，违反"下推只能给超集"铁律。
 * 所以宁可多取候选（多花一点时间），也不给一个残缺的结果。
 */
db_err db_vec_knn_filtered(DB *db, const float *q, uint32_t k, uint16_t ef,
                           int32_t label, db_vec_hit *out, uint32_t *n_out);

/* 向量总数 */
db_err db_vec_count(DB *db, uint64_t *n);

/*
 * 诊断：累计读了多少页（向量页 + 标量记录）。
 *
 * 用来验证"ANN 的页读 ≪ 暴力扫"（A4）。
 * 和 db_blob_page_reads / db_graph_reads 是同一个思路。
 */
uint64_t db_vec_page_reads(DB *db);

/*
 * 把 HNSW 索引写进磁盘。
 *
 * 索引是 **checkpoint 式**的（不写日志）：正常关闭时会自动 flush，
 * 崩溃后靠重建（db_vec_reindex / 打开时自动检测）。
 *
 * 显式调它是为了"插了一批数据后落一次盘"，不用等关库。
 */
db_err db_vec_flush(DB *db);

/*
 * 从向量数据重建 HNSW 索引。
 *
 * 什么时候需要：索引过期（插了数据没 flush）或损坏。
 * 打开库时如果发现索引对不上，会自动走这条路。
 */
db_err db_vec_reindex(DB *db);

/* ---------------- 全文检索模态（v1.3） ---------------- */

/*
 * 全文检索 = 模态 4（倒排索引 + BM25）。
 *
 * 【两条路径，各管各的】
 *
 *   db_ft_search()   专用 API：按 BM25 相关性排序返回 (doc_id, score)
 *   ft_docs 虚拟表   元数据路径：SELECT doc_id, doc_len, nterms, sealed FROM ft_docs
 *
 * 为什么相关性排序**不进 SQL 谓词**（PLAN-v1.3 §1.2 的判决）：
 * idf / avgdl 是**全局统计量**，随语料和段合并而变，
 * 执行器拿不到、也没法独立重求值 ——
 * "下推给超集、执行器重判"这条契约在相关性排序上**不成立**。
 * 与向量模态的"距离排序不进 SQL"是同一个判断。
 *
 * 【索引现在落盘了】（v1.3 步骤 5）
 *
 * 段的内容本来就在磁盘上；现在**段的清单**与 **tombstone 集合**
 * 也落盘了：
 *   - 段清单 = 段头页里的状态字段（live / dead / merging），
 *     恢复时扫页空间把段找齐；
 *   - tombstone 集合 = 专门的 tombstone 页（PT_FT_TOMB），
 *     恢复时取"页里的内容 ∪ (WAL 重放 − WAL 回滚)"。
 *
 * 所以重开库**恢复得回来**：上次灌进去的文档还在，被删的还在删着。
 */

/* 一个检索命中 */
typedef struct {
    uint32_t doc_id;   /* 文档标识（调用方给的） */
    double   score;    /* BM25 分数 */
} db_ft_hit;

/*
 * 灌一篇文档进索引。
 *
 * text / len —— 原文（内部用分词器切词）。
 * doc_id —— 调用方给的文档标识，**必须互不相同**
 *   （同一个 id 灌两次会得到两个段里的同一篇文档；
 *    ft_docs 以"段号大者胜"取后者，检索时两篇都会命中）。
 *
 * 写入是**追加**语义：每次调用建一个新段（写只追加，不改老段）。
 * 段数超过阈值（8）时该触发合并，但本版**不自动合并** ——
 * 合并由 db_ft_merge 显式触发。
 */
db_err db_ft_add(DB *db, uint32_t doc_id, const char *text, uint32_t len);

/*
 * 检索（主路径）。
 *
 * query —— 查询串，内部用分词器切词（与线性扫描同一个分词器）。
 * top_k —— 要返回多少条。
 * max_out —— out 的容量。**top_k > max_out 直接报 DB_E_NOSPACE**，
 *   不做静默截断（截断出来的结果"看起来对其实错"）。
 *
 * 排序：分数降序、分数相同按 doc_id 升序。
 * 被删除（tombstone）的文档不进结果。
 *
 * *n_out —— 实际返回几条（可能少于 top_k：命中不足时）。
 */
db_err db_ft_search(DB *db, const char *query, uint32_t top_k,
                    db_ft_hit *out, uint32_t max_out, uint32_t *n_out);

/*
 * 删一篇文档（打 tombstone）。
 *
 * 立刻生效：之后的检索不再返回它，ft_docs 里它的 sealed = 1。
 * 删一个不存在的文档是**合法的空操作**（不报错）。
 */
db_err db_ft_delete(DB *db, uint32_t doc_id);

/*
 * 触发一次 Tiered 合并。
 *
 * 段数 ≤ 8 时什么都不做（*merged_out = 0）。
 * 否则挑相邻窗口里"文档数之和最小"的几个段合成一个新段，
 * 合并时**物理丢弃**被 tombstone 的文档。
 *
 * *merged_out —— 被合并掉的段数（0 = 没合并）。
 */
db_err db_ft_merge(DB *db, uint32_t *merged_out);

/* 这个库有没有全文索引（打开库就会建一个空的，所以恒为真） */
int db_ft_exists(DB *db);

/* 索引里的文档数（各段 doc_count 之和，**含**已删除的） */
uint32_t db_ft_ndocs(DB *db);

/* 段数 / 删除标记数（诊断用） */
uint32_t db_ft_nsegs(DB *db);
uint32_t db_ft_ntombstones(DB *db);

/*
 * 诊断：索引累计读了多少页。
 *
 * 与 db_vec_page_reads / db_blob_page_reads 同一个思路：
 * **用可观测量证明"少读了"**（A5 的度量）。
 */
uint64_t db_ft_page_reads(DB *db);

#endif /* DB_DB_H */
