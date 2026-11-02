/*
 * batch.c — 批解码：直接解进列数组，不物化 DbValue（v1.2 步骤 0）
 *
 * 设计说明见 storage/batch.h。这里只讲实现上的关键点。
 *
 * ==================== 为什么不用 rec_decode_layout ====================
 *
 * rec_decode_layout 解到 Value[]（每列一个联合体），再由 values_to_dbvalues
 * 转成 DbValue[]。两步，每行都要：
 *   - 把 Value[] 清一遍（memset(&vals[i].u, 0, ...)）
 *   - 把每个 Value 拷成一个 272 字节的 DbValue
 *
 * 批路径要的就是**跳过第二步**：直接写进列数组。
 *
 * ==================== 三个必须与逐行路径完全一致的点 ====================
 *
 * 批式与火山的果集必须**逐格全等**（PLAN-v1.1 §1.2 的 A1 门禁）。
 * 所以下面三处必须和 storage/value.c 的逐行解码一模一样：
 *
 *   1. NULL 的判法：null bitmap 的第 i 位（REC_HDR_SIZE + i/8 的第 i%8 位）
 *   2. 文本的截断：val_text 把长度截到 VAL_TEXT_MAX-1（255）
 *      —— 批路径也必须截，否则文本池会被撑爆，且两边的值长度不同
 *   3. 整数的宽度：T_INT4 要符号扩展成 i64（T_INT8 才是 i64）
 *
 * 这三处任何一处不一致，对拍就会红 —— 所以下面都写了注释指回逐行版本。
 */

#include "storage/batch.h"

#include <string.h>

int val_batch_decode(const RecLayout *L, const ColDesc *cols, int ncols,
                     const AmRowRef *rows, int nrows, AmBatch *out)
{
    if (!L || !cols || !rows || !out || !out->cols)
        return E_INVAL;
    if (nrows < 0)
        return E_INVAL;
    if (ncols > L->ncols || ncols > out->ncols)
        return E_INVAL;
    if ((int)out->nrows + nrows > (int)AM_BATCH_MAX_ROWS)
        return E_NOSPACE;
    if (nrows == 0)
        return OK;

    /* 写游标：这一批从第 out->nrows 行开始写 */
    const u16 base = out->nrows;

    for (int r = 0; r < nrows; r++) {
        const u8 *buf = rows[r].data;
        const u16 len = rows[r].len;

        if (!buf)
            return E_INVAL;
        if (len < L->data_off)
            return E_CORRUPT;      /* 与 rec_decode_layout 同一判据 */

        for (int c = 0; c < ncols; c++) {
            AmBatchCol *col = &out->cols[c];

            if (!col->used)
                continue;           /* 这一列不进批（与逐行路径同一个决定） */

            const u16 i = (u16)(base + r);

            /* NULL 位图：与 rec_decode_layout 同一判据 */
            const bool isnull =
                ((buf[REC_HDR_SIZE + c / 8] >> (c % 8)) & 1u) != 0;
            if (isnull) {
                bit_set(col->nulls, i);
                continue;
            }

            switch (cols[c].type) {
            case T_TEXT: {
                const u16 k = L->var_col_idx[c];
                const u16 s = get_u16(buf + L->varoff_off + 2u * k);
                const u16 e = (k + 1u < L->nvar)
                            ? get_u16(buf + L->varoff_off + 2u * (k + 1u))
                            : len;
                if (s > e || e > len)
                    return E_CORRUPT;

                /*
                 * 截断到 VAL_TEXT_MAX-1 —— 与 val_text 一致。
                 * 不截的话：① 文本池会被撑爆 ② 两边的值长度不同（对拍会红）。
                 */
                u16 n = (u16)(e - s);
                if (n >= VAL_TEXT_MAX)
                    n = (u16)(VAL_TEXT_MAX - 1);

                if (col->tused + n > col->tcap)
                    return E_NOSPACE;
                if (n)
                    memcpy(col->tpool + col->tused, buf + s, n);
                col->toff[i] = col->tused;
                col->tlen[i] = n;
                col->tused = (u32)col->tused + n;
                break;
            }
            case T_REAL: {
                const u64 bits = get_u64(buf + L->fixed_col_off[c]);
                memcpy(&col->reals[i], &bits, 8);
                break;
            }
            case T_INT4:
                /* 符号扩展成 i64 —— 与 values_to_dbvalues 的 val_int(i4) 一致 */
                col->ints[i] = (i64)(i32)get_u32(buf + L->fixed_col_off[c]);
                break;
            case T_INT8:
                col->ints[i] = (i64)get_u64(buf + L->fixed_col_off[c]);
                break;
            case T_BOOL:
                col->ints[i] = (buf[L->fixed_col_off[c]] != 0) ? 1 : 0;
                break;
            default:
                return E_INVAL;
            }
        }
    }

    out->nrows = (u16)(base + (u16)nrows);
    for (int c = 0; c < ncols; c++) {
        if (out->cols[c].used)
            out->cols[c].n = out->nrows;
    }
    return OK;
}
