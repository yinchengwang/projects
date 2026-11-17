#ifndef DB_QUERY_PARSER_H
#define DB_QUERY_PARSER_H

#include "common/error.h"
#include "common/types.h"
#include "query/lexer.h"
#include "storage/value.h"

#define SQL_NAME_MAX 64

/*
 * 单表 / 单语句的列数上限。
 *
 * 【为什么是 32 而不是 16】
 * 16 是早期"够用就行"的取法，它同时卡住了两件不相干的事：
 * 真实表的宽度，和索引个数（exec_create_index 复用了这个常量）。
 *
 * 32 不是随便挑的：磁盘行格式的上限是 **REC_MAX_COLS = 32**
 * （storage/record.h），所以在 32 以内，行编码/解码完全不用改。
 * 超过 32 要动 record 的布局数组，那是另一件事。
 *
 * 【提升它的代价（已实测）】
 * 所有 `DbValue row[SQL_MAX_COLS]` 的栈缓冲翻倍 —— DbValue 是 320 字节
 * （主要是内联的 256 字节文本区），所以一个 32 列的 row 占 10 KB。
 * 栈最深的 exec_select_agg 会因此从 ~22 KB 涨到 ~30 KB。
 * Windows 默认线程栈 1 MB，这个量级仍然安全；但不要再往上加了 ——
 * 真要支持更宽的表，得把行缓冲改成堆分配。
 *
 * 【与模态的关系】
 * 图模态的 GR_MAX_COLS = 15 是**落盘格式**（决定 gstore 的 rec_size），
 * 它的约束是 "1 + 属性列 ≤ 执行器行缓冲"。32 下这条依然成立
 * （从"正好相等"变成"留有余量"），所以这个常量提升不影响任何模态的
 * 磁盘格式 —— 磁盘上的行布局由 storage/record.h 的 REC_MAX_COLS 管，
 * 那个值本来就是 32。
 */
#define SQL_MAX_COLS 32

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

#endif /* DB_QUERY_PARSER_H */
