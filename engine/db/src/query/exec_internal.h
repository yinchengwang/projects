#ifndef DB_QUERY_EXEC_INTERNAL_H
#define DB_QUERY_EXEC_INTERNAL_H

#include "query/exec.h"
#include "spi/table_opener.h"

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
 * 【v1.2 步骤 1：cols / idxs 是自持的副本】
 *
 * cols / idxs 原来直接指向 catalog 内部的数组。那是**真悬空**：
 * catalog 建表时 realloc 表项数组、建索引时 realloc 索引数组 ——
 * 另一个连接一改，指针就废了。
 *
 * 现在改成：打开表时把内容**拷进下面这两个缓冲区**，
 * cols / idxs 一律指向自己的副本（由 opener 负责拷）。
 *
 * 缓冲区大小用 SQL_MAX_COLS（=32）：
 *   - 列数上限就是 32（REC_MAX_COLS 是同一个数）
 *   - 索引数上限也卡到 32（见 exec_create_index 的显式报错）
 *
 * 【v2.0 C-1：执行器不再持有模态的任何结构】
 *
 * 这里原来放的是关系模态的私有结构（堆句柄 + AM 上下文 + AM 本体）——
 * 那是"执行器认得某种具体模态"的证据。现在换成两个不透明的东西：
 *
 *   - open_result   打开器的产物（执行器只读它的公共字段）
 *   - priv          打开器的私有状态缓冲（字节数组，执行器不知道里面是什么）
 *
 * 私有状态里装什么由**提供 opener 的那一方**决定，执行器只负责
 * 给它一块够大的地方（`SPI_OPENER_PRIV_MAX`）；虚拟表一个字节都不用。
 *
 * 关键判据：`src/query/` 里不该再出现任何具体模态的结构名。
 */
typedef struct {
    StorageAM        *am;          /* 从 open_result 拷过来的便捷指针 */
    const ColDesc    *cols;
    int               ncols;
    u32               tid;         /* catalog 表 id；虚拟表为 0 */
    bool              is_virtual;

    /* 自持的副本（v1.2 步骤 1，由 opener 写入） */
    ColDesc           cols_buf[SQL_MAX_COLS];
    IndexInfo         idxs_buf[SQL_MAX_COLS];
    const IndexInfo  *idxs;
    int               nidx;

    /* 打开器的产物（不透明） */
    TableOpenResult   open_result;

    /* 打开器的私有状态缓冲（不透明；容量见 SPI_OPENER_PRIV_MAX） */
    unsigned char     priv[SPI_OPENER_PRIV_MAX];
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

/*
 * 通用标量函数调用的公共求值体（EX_FUNC）。
 *
 * 【为什么两条求值路径都调它】
 * 执行器有逐行（`exec_scan.c::eval_expr`）与列式（`exec_eval.c::eval_where`）
 * 两条平行实现，两边都要支持函数调用。各写一遍 ⇒ 两份实现迟早漂移，
 * 而 A3 的验收判据正是"两路全等"。共用这一份，全等在结构上成立。
 */
int eval_func_common(const Expr *e, const DbValue *row, DbValue *out);

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

    /*
     * 下推的**合取谓词组**（pushed = true 时 nconj >= 1）。
     *
     * 【为什么是数组而不是单个谓词】
     *
     * `WHERE time >= a AND time < b` 是两个谓词。第一版只能推一个 ⇒
     * 双边界范围实际只推一边（实测：`>= 第2天` 与 `>= 第2天 AND < 第3天`
     * 读的页数**一模一样**）。契约加宽之后一次带走整组。
     *
     * ⚠️ nconj 可能小于 WHERE 里的 AND 项数（模态可能只吃得下一部分，
     *    或者超过 PD_CONJ_MAX）。执行器永远完整求值 WHERE，所以少推是安全的。
     */
    Predicate conj[PD_CONJ_MAX];
    u32       nconj;

    bool      have_stats;  /* 有没有行数估算 */
    i64       nrows;       /* 估算行数 */
    double    sel;         /* 这一组的合并选择率 */
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
