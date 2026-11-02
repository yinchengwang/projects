#ifndef DB_QUERY_EXEC_BATCH_H
#define DB_QUERY_EXEC_BATCH_H

#include "query/exec_internal.h"
#include "storage/batch.h"

/*
 * 批式（列式）执行路径（v1.1 步骤 2）的内部接口。
 *
 * 拆成两个 .c：
 *   exec_batch.c —— 攒批、转置、投影、主循环、逐行退路
 *   exec_eval.c  —— 列式表达式求值（形状检查 + 比较 + 三值逻辑）
 *
 * ==================== v1.2 步骤 0：列批的类型搬到 storage/ ====================
 *
 * 批的列布局（AmBatchCol）原来定义在这里（BatchCol）。
 * 现在搬到 storage/batch.h —— 因为它变成了 **SPI 的契约**：
 * 模态往里写，执行器从里面读，两边必须是同一套内存布局。
 *
 * 理由与 DbValue 一样（见 storage/value.h）：放在 query/ 会让 spi/ 反过来
 * 依赖 query/，层次就倒了。
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

typedef struct {
    int      ncols;
    u16      nrows;     /* 本批行数 */
    AmBatchCol col[SQL_MAX_COLS];

    u8      *marks;     /* MARK_DEPTH × VEC_BATCH 的标记层栈（一次分配） */
    u16     *sel;       /* 选择向量：匹配的行号 */

    /* 批扫描的行引用缓冲（v1.2 步骤 0）：一次给 AM 一批原始行 */
    AmRowRef *refs;
} BatchCtx;

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
