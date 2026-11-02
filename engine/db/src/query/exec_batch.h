#ifndef DB_QUERY_EXEC_BATCH_H
#define DB_QUERY_EXEC_BATCH_H

#include "query/exec_internal.h"

/*
 * 批式（列式）执行路径（v1.1 步骤 2）的内部接口。
 *
 * 拆成两个 .c：
 *   exec_batch.c —— 攒批、转置、投影、主循环、逐行退路
 *   exec_eval.c  —— 列式表达式求值（形状检查 + 比较 + 三值逻辑）
 *
 * 【为什么批里不存 DbValue】
 * DbValue 一个 272 字节（VAL_TEXT_MAX=256 + 若干字段）。
 * 一行四列 = 1088 字节，一批 1024 行 = 1.1 MB ——
 * 逐行过滤时在这 1.1 MB 里跳着读，全在 L2/L3 之间来回。
 * 列式：一列一个紧凑数组（INT 列每行 8 字节），一批 1024 行 = 8 KB，全在 L1 里。
 */

/* 求值用的临时缓冲层数（每层 VEC_BATCH 字节） */
#define MARK_DEPTH 32

/*
 * 三态标记：bool 装不下 NULL，所以 WHERE 的中间结果用三态。
 * 这三个值与 val_truth 的返回约定对齐：1 = 真，0 = 假，-1 = 空。
 */
#define M_FALSE 0u
#define M_TRUE  1u
#define M_NULL  2u

/*
 * 外部值（blob 模态）：内容不在库里，只记"去哪儿取"。
 *
 * 批里必须能装下它 —— blob 模态的表也要走批式路径，
 * 而执行器不许认模态（硬规则），所以只能通用地装下。
 */
typedef struct {
    u64       key;
    u64       off;
    DetoastFn fn;
    void     *ctx;
} ExtRef;

/* 一列的批数据 */
typedef struct {
    u8      type;      /* ColType：这一列的类型（一批内不变） */
    bool    used;      /* 这一列要不要进批（只进用得上的列） */
    u16     n;         /* 本批的行数 */
    u8     *nulls;     /* NULL 位图 */
    i64    *ints;      /* T_INT4 / T_INT8 / T_BOOL */
    double *reals;     /* T_REAL */
    u32    *toff;      /* T_TEXT（内联）：文本池里的偏移 */
    u16    *tlen;      /* T_TEXT（内联）：长度 */
    u8     *tpool;     /* 文本池 */
    u32     tused;
    u32     tcap;
    u8     *extflag;   /* 位图：1 = 该行是外部值 */
    ExtRef *exts;      /* 外部句柄（用到才分配） */
} BatchCol;

typedef struct {
    int      ncols;
    u16      nrows;     /* 本批行数 */
    BatchCol col[SQL_MAX_COLS];

    u8      *marks;     /* MARK_DEPTH × VEC_BATCH 的标记层栈（一次分配） */
    u16     *sel;       /* 选择向量：匹配的行号 */
} BatchCtx;

/* ---------------- 位图小工具 ---------------- */

static inline void bit_set(u8 *bm, u32 i)
{
    bm[i >> 3] |= (u8)(1u << (i & 7u));
}

static inline bool bit_get(const u8 *bm, u32 i)
{
    return ((bm[i >> 3] >> (i & 7u)) & 1u) != 0;
}

/* ---------------- 批的自身（exec_batch.c） ---------------- */

int  batch_ctx_init(BatchCtx *bc, const ColDesc *cols, int ncols, u16 needed);
void batch_ctx_free(BatchCtx *bc);
void batch_reset(BatchCtx *bc);
int  batch_add_row(BatchCtx *bc, const DbValue *row);
void batch_cell(const BatchCtx *bc, int c, u32 i, DbValue *out);

/* ---------------- 列式求值（exec_eval.c） ---------------- */

/*
 * 形状检查：这条 WHERE 能不能列式求值？
 * 不能的话整条语句退回逐行路径（不做"批内再降级"——
 * 同一份语义只有一处实现，不会分叉）。
 */
bool where_supported(const Expr *e, const ColDesc *cols, int ncols,
                     int depth, int *need_depth);

/* 求值成三态标记（写在 dst 里） */
int  eval_where(const Expr *e, BatchCtx *bc, const ColDesc *cols, int ncols,
                u8 *dst, int depth);

/* 收集一条语句要用到的列（位图） */
void collect_needed(const Expr *e, u16 *needed);

#endif /* DB_QUERY_EXEC_BATCH_H */
