#ifndef DB_QUERY_EXEC_INTERNAL_H
#define DB_QUERY_EXEC_INTERNAL_H

#include "query/exec.h"
#include "spi/heap_am.h"

/*
 * 执行器内部共享的东西。
 *
 * 【为什么要拆文件】
 * 硬规则：单文件不超过 800 行。
 * v0.9 改造前 exec.c 是 1183 行，改造后 1131 行 —— 都超了。
 *
 * 拆成三个：
 *   exec.c        入口 + 结果集构造
 *   exec_scan.c   绑定 + 求值 + 表解析 + 下推 + SELECT
 *   exec_write.c  INSERT / UPDATE / DELETE / VACUUM / CREATE
 *
 * 拆分的界线是"**干什么**"，不是"多少行"：
 *   - 只读的（scan）和只写的（write）分开 —— 改写路径不会碰到读路径
 *   - 结果集构造单独放 —— 它被读和写两边都用
 */

/* ---------------- 结果集构造器 ---------------- */

/* 复制一个字符串（malloc）。结果由调用方 free。 */
char *xstrdup(const char *s);

typedef struct {
    QueryResult *r;
    int cap;
} Builder;

int b_init(Builder *b, int ncols, int nrows_hint);
int b_push(Builder *b, const DbValue *vals);

/* ---------------- 表上下文 ---------------- */

/*
 * 一张表的执行期上下文。
 *
 * 【为什么关系模态要单独放 heap / am_ctx / am_storage】
 * 关系模态的 AM 是**每条查询现构造**的：它要绑定到具体的
 * 堆句柄 + 列定义 + 索引列表，而表是运行期动态创建的。
 * 所以 AM 本体和它的私有状态都放在 TableCtx（栈上）。
 *
 * blob 的 AM 相反 —— 它是实例级的，挂在 DbInstance 上，
 * TableCtx 里只存一个指针。
 */
typedef struct {
    StorageAM    *am;          /* 这张表的 StorageAM */
    const ColDesc *cols;
    int           ncols;
    u32           tid;         /* catalog 表 id；虚拟表为 0 */
    bool          is_virtual;

    /*
     * ---------------- v1.2 步骤 1：自持的副本 ----------------
     *
     * cols / idxs 原来直接指向 catalog 内部的数组。
     * 那是**真悬空**：catalog 建表时 realloc 表项数组、
     * 建索引时 realloc 索引数组 —— 另一个连接一改，指针就废了。
     *
     * 现在改成：解析表时把内容**拷进下面这两个缓冲区**，
     * cols / idxs 一律指向自己的副本。
     * 副本的代价是每条语句拷几十个字节，换来的是
     * "catalog 之后怎么变都与本次执行无关"。
     *
     * 缓冲区大小用 SQL_MAX_COLS（=16）：
     *   - 列数上限就是 16（parser 层已卡）
     *   - 索引数上限也卡到 16（见 exec_create_index 的显式报错）
     */
    ColDesc       cols_buf[SQL_MAX_COLS];
    IndexInfo     idxs_buf[SQL_MAX_COLS];
    const IndexInfo *idxs;
    int           nidx;

    /* 关系模态专用 */
    Heap         *heap;
    HeapAmCtx     am_ctx;
    StorageAM     am_storage;
} TableCtx;

/*
 * 解析表名。顺序：catalog（用户表）优先，注册表（虚拟表）其次。
 */
int resolve_table(Db *db, const char *name, TableCtx *tc, char *err, int errcap);

/* 按表 id 解析（只可能是用户表） */
int resolve_table_id(Db *db, u32 tid, TableCtx *tc);

/* ---------------- 绑定与求值 ---------------- */

int bind_expr(Expr *e, const ColDesc *cols, int ncols, char *err, int errcap);
int eval_expr(Expr *e, const DbValue *row, DbValue *out);

/* ---------------- 扫描 ---------------- */

/* ---------------- 代价与下推计划（v1.1 步骤 4） ---------------- */

/* 两个代价 + 决策（公式见 exec_cost.c） */
typedef struct {
    i64    nrows;       /* 估算行数 */
    double sel;         /* 选择率 */
    double cost_seq;    /* 全表扫描代价 */
    double cost_idx;    /* 索引扫描代价 */
    bool   use_index;   /* 决策：走索引吗 */
    const char *why;    /* 决策理由（EXPLAIN 用，指向静态字符串） */
} CostPlan;

/*
 * 下推决策的结果。EXPLAIN 与真正的执行**共用同一份**（见 exec_scan.c 的 plan_push）。
 */
typedef struct {
    bool      pushed;      /* 决策：下推吗 */
    Predicate pred;        /* 下推的谓词（pushed = true 时有效） */
    bool      have_stats;  /* 有没有行数估算 */
    i64       nrows;       /* 估算行数 */
    double    sel;         /* 选中谓词的选择率 */
    CostPlan  plan;        /* 两个代价与理由 */
    const char *why;       /* 决策理由（静态字符串） */
} PushPlan;

int plan_push(Db *db, TableCtx *tc, Expr *where, PushPlan *out);

/* 打开扫描源：能下推就下推，否则全表扫 */
int open_scan(Db *db, TableCtx *tc, Expr *where, AmScan **out);

/* 可见性上下文（快照连接私有 + 事务表实例共享），每次现构造 */
void make_vis_ctx(Db *db, AmVisCtx *vis);

/* ---------------- 各语句的实现 ---------------- */

/*
 * 把比较操作符翻过来（常量在左边时用）：5 < id 等价于 id > 5。
 *
 * 共享给下推（exec_scan.c）和列式求值（exec_batch.c）——
 * 两处必须翻得一模一样，所以只有一份实现。
 */
int flip_op(int op);

/*
 * 执行 SELECT 的行产生部分（绑定与结果集外壳在 exec_scan.c 里）。
 *
 * 内部按形状分派：
 *   有聚合        → exec_select_agg（exec_agg.c）
 *   能列式        → 批式（exec_batch.c）
 *   否则          → 逐行
 *
 * err / errcap 透传给聚合路径 —— 聚合的形状错误必须**明确报错**，
 * 不能退回去悄悄算个错的。
 */
int exec_select_run(Db *db, Stmt *st, TableCtx *tc, Builder *b,
                    char *err, int errcap);

/* ---------------- 聚合（exec_agg.c） ---------------- */

/* 这条语句要不要走聚合路径 */
bool stmt_has_agg(const Stmt *st);

/* 聚合 + GROUP BY 的执行 */
int exec_select_agg(Db *db, Stmt *st, TableCtx *tc, Builder *b,
                    char *err, int errcap);

int exec_select(Db *db, Stmt *st, QueryResult **out, char *err, int errcap);

/* EXPLAIN：打印执行计划（与执行共用 plan_push，见 exec_explain.c） */
int exec_explain(Db *db, Stmt *st, QueryResult **out, char *err, int errcap);
int exec_insert(Db *db, Stmt *st, QueryResult **out, char *err, int errcap);
int exec_update(Db *db, Stmt *st, QueryResult **out, char *err, int errcap);
int exec_delete(Db *db, Stmt *st, QueryResult **out, char *err, int errcap);
int exec_vacuum(Db *db, Stmt *st, QueryResult **out, char *err, int errcap);
int exec_create_index(Db *db, Stmt *st, QueryResult **out, char *err, int errcap);
int exec_create(Db *db, Stmt *st, QueryResult **out, char *err, int errcap);

#endif /* DB_QUERY_EXEC_INTERNAL_H */
