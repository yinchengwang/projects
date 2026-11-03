#ifndef DB_QUERY_PARSER_H
#define DB_QUERY_PARSER_H

#include "common/error.h"
#include "common/types.h"
#include "query/lexer.h"
#include "storage/value.h"

#define SQL_NAME_MAX 64
#define SQL_MAX_COLS 16
#define SQL_MAX_ROWS 16

typedef enum {
    EX_LIT = 0,
    EX_COL,
    EX_BIN,
    EX_UN,
    EX_AGG                    /* v1.1 步骤 3：聚合函数调用 */
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

typedef struct Expr Expr;
struct Expr {
    ExprKind kind;
    int      op;                   /* EX_AGG 时是 AggKind */
    char     name[SQL_NAME_MAX];   /* EX_COL 的列名；EX_AGG 的函数名 */
    int      col_idx;              /* 绑定期填 */
    DbValue  lit;                  /* EX_LIT */
    Expr    *l;                    /* EX_AGG：聚合的参数（COUNT(*) 为 NULL）*/
    Expr    *r;
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
    Expr *vals[SQL_MAX_ROWS][SQL_MAX_COLS];
    int   nrows;
    int   ncols;                    /* INSERT: 每行的值个数；CREATE: 列数 */

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

#endif /* DB_QUERY_PARSER_H */
