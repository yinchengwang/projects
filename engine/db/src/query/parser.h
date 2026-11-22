#ifndef DB_QUERY_PARSER_H
#define DB_QUERY_PARSER_H

#include "common/error.h"
#include "common/types.h"
#include "query/lexer.h"
#include "storage/value.h"

#define SQL_NAME_MAX 64

/*
 * 单表 / 单语句的列数上限 —— **定义已挪到 `common/types.h`**（L0）。
 *
 * 【为什么挪】
 * SPI 的表打开器契约（`spi/table_opener.h`）要用它给调用方的列定义缓冲
 * 定量，而 spi/ 不许反过来依赖 query/（L3 同层，方向反了会把
 * query/ 与 common/ 都绑到 spi/ 上）。它本质是"一个表能有多宽"。
 *
 * 值仍是 **32**（与 REC_MAX_COLS 一致），语义一字未改。
 */


/*
 * 单条 INSERT 的初始行容量（**不是上限**）。
 *
 * 行数现在按需翻倍增长（见 Stmt.vals 的注释），这个常量只决定
 * 第一次分配多少行。取 16 是因为它正好覆盖"手写小批量插入"
 * 的绝大多数情况，省掉一次 realloc；真正的批量导入会自然扩到更大。
 */
#define SQL_ROWS_INIT 16

typedef enum {
    EX_LIT = 0,
    EX_COL,
    EX_BIN,
    EX_UN,
    EX_AGG,                   /* v1.1 步骤 3：聚合函数调用 */
    EX_FUNC                   /* v1.6：通用标量函数调用（值层函数表按名字查） */
} ExprKind;

/* 聚合函数（v1.1 步骤 3） */
typedef enum {
    AGG_NONE = 0,
    AGG_COUNT,                /* COUNT(*) / COUNT(col) */
    AGG_SUM,
    AGG_AVG,
    AGG_MIN,
    AGG_MAX
} AggKind;

/* 二元/一元运算符编码 */
#define EX_OP_AND      100
#define EX_OP_OR       101
#define EX_OP_NOT      102
#define EX_OP_ISNULL   103
#define EX_OP_ISNOTNULL 104
#define EX_OP_NEG      105
/* 比较和算术直接用字符：'=' '<' '>' 'L'(<=) 'G'(>=) 'N'(<>) '+' '-' */

/*
 * 函数调用的实参个数上限。
 *
 * 【为什么 8】
 * 值层函数签名是 `(args[], nargs, out)`，实参数组在**栈上**放在求值入口里。
 * 8 个 DbValue = 8 × 272 ≈ 2.2 KB，栈安全；再多就该走堆分配了，
 * 而目前没有任何函数需要超过 2 个实参（`json_match(doc, spec)`、
 * `json_extract(doc, path)` 都是 2 个）。
 *
 * 超了返回 E_NOSPACE —— 明确报错，不静默截断实参（截断 = 给错答案）。
 */
#define EX_FUNC_MAX_ARGS 8

typedef struct Expr Expr;
struct Expr {
    ExprKind kind;
    int      op;                   /* EX_AGG 时是 AggKind */
    char     name[SQL_NAME_MAX];   /* EX_COL 的列名；EX_AGG / EX_FUNC 的函数名 */
    int      col_idx;              /* 绑定期填 */
    DbValue  lit;                  /* EX_LIT */
    Expr    *l;                    /* EX_AGG：聚合的参数（COUNT(*) 为 NULL）*/
    Expr    *r;

    /*
     * EX_FUNC 的实参。
     *
     * 【为什么用指针数组而不是直接塞进 l / r】
     * 标量函数的实参个数是可变的（0~EX_FUNC_MAX_ARGS），l / r 只够两个。
     * 指针数组让"函数机制"与"二元运算机制"互不干扰 ——
     * l / r 的既有语义一个字都不用改。
     *
     * 所有权：args 数组与每个 args[i] 都归这个 Expr，释放时必须递归。
     */
    Expr   **args;
    int      nargs;
};

typedef enum {
    ST_SELECT = 0,
    ST_INSERT,
    ST_UPDATE,
    ST_DELETE,
    ST_CREATE,
    ST_CREATE_INDEX,
    ST_BEGIN,       /* BEGIN：开始显式事务 */
    ST_COMMIT,      /* COMMIT：提交 */
    ST_ROLLBACK,    /* ROLLBACK：回滚 */
    ST_VACUUM,      /* VACUUM t：回收死版本 */
    ST_CHECKPOINT,  /* CHECKPOINT：刷页 + 截断日志（v1.2 步骤 6）*/
    ST_SET_ISOLATION/* SET TRANSACTION ISOLATION LEVEL ... */
} StmtKind;

typedef struct {
    StmtKind kind;

    /*
     * EXPLAIN 前缀（v1.1 步骤 4）。
     *
     * 【为什么用标志位而不是嵌套一个 Stmt】
     * 嵌套 Stmt 要递归释放、要两份 ~2KB 的结构，而 EXPLAIN 只包一层、
     * 只支持 SELECT —— 一个 bool 足够，free_stmt 一行都不用改。
     */
    bool     explain;

    char     table[SQL_NAME_MAX];

    /* SELECT */
    bool  star;
    Expr *targets[SQL_MAX_COLS];
    int   ntargets;
    Expr *where;
    int   limit;                    /* -1 表示无 */

    /* GROUP BY（v1.1 步骤 3）。绑定期把 groupby[i]->col_idx 填好。 */
    Expr *groupby[SQL_MAX_COLS];
    int   ngroupby;

    /* INSERT */
    /*
     * 值矩阵：**行数动态**，列数固定。
     *
     * 【为什么行数是动态的】
     * 原来是 `Expr *vals[SQL_MAX_ROWS][SQL_MAX_COLS]` 定长二维数组，
     * 单条 INSERT 到第 17 行就报「VALUES 行数超限」，整条语句一行都不插。
     * 批量导入 20 行数据因此被整条拒绝 —— 这是用户可见的缺陷，
     * 而且 `sizeof(Stmt)` 里有 2 KB 是这个定长数组，Stmt 又开在栈上。
     *
     * 现在 vals 是 "行指针数组"，每行按需分配：
     *   vals[i]  → Expr *[ncols]
     * 所以 `vals[i][c]` 的语法在调用方（exec_write.c）**一字不改**。
     *
     * 【为什么列数仍然定长】
     * SQL_MAX_COLS（16）不只是解析器的上限：执行器的行缓冲是
     * `DbValue row[SQL_MAX_COLS]`、TableCtx 的列副本是
     * `ColDesc cols_buf[SQL_MAX_COLS]`、批量接口的列数组也是 16。
     * 放宽列数要同时改执行器、SPI 与所有模态的行表示 ——
     * 那是另一个切片，不该和"批量行数"混在一起。
     *
     * 【谁负责释放】
     * free_stmt() 按 vals_cap 遍历 vals 数组：已分配的行要 free 行内
     * 每个 Expr 再 free 行本身；未分配的行（NULL）跳过。
     * 用 cap 而不是 nrows 遍历，是为了覆盖"解析到一半失败"的中间态：
     * 那时 nrows 还没 ++，但行内可能已经放过几个 Expr。
     */
    Expr    ***vals;                /* vals[nrows_cap][ncols] */
    int       vals_cap;             /* vals 已分配的行数容量 */
    int       nrows;
    int       ncols;                /* INSERT: 每行的值个数；CREATE: 列数 */

    /* UPDATE */
    char  set_col[SQL_NAME_MAX];
    Expr *set_expr;

    /* CREATE TABLE */
    char  col_names[SQL_MAX_COLS][SQL_NAME_MAX];
    u8    col_types[SQL_MAX_COLS];
    bool  col_nullable[SQL_MAX_COLS];

    /* CREATE INDEX（table 存表名，index_name 存索引名） */
    char  index_name[SQL_NAME_MAX];
    char  index_col[SQL_NAME_MAX];

    /* SET TRANSACTION ISOLATION LEVEL（0=RC, 1=RR）*/
    int   iso_level;
} Stmt;

/* 解析一条语句。失败时把原因写进 errbuf 并返回错误码。 */
int  parse_sql(const char *sql, Stmt *out, char *errbuf, int errcap);

void free_stmt(Stmt *st);

/*
 * 递归释放一整棵表达式树（含 EX_FUNC 的 args）。
 *
 * free_stmt 内部用的就是它；单独暴露出来是给"解析到一半失败"的路径用的
 * （那些地方手里只有一个 Expr*，没有 Stmt）。
 */
void free_expr(Expr *e);

#endif /* DB_QUERY_PARSER_H */
