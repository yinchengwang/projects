/*
 * graph_prop.c — 节点属性的定长编解码（v0.11 步骤 2）
 *
 * 【为什么单独一个文件】
 *
 * 属性编解码和邻接链维护是两件正交的事：
 *   - 邻接链错了 → 图的结构坏掉（A1/A2 红）
 *   - 属性错了 → 查出来的值不对（A4 红）
 * 混在一个文件里，出问题时不好定位是"结构"还是"值"。
 *
 * 【布局】
 *
 *   [NULL 位图 ceil(ncols/8) 字节][列 0 的定长值][列 1 的定长值]...
 *
 * 每列的偏移在建图时算好（Graph.col_off），
 * 读属性就是"按偏移 memcpy 固定字节数"—— 没有解析、没有分支。
 *
 * 【为什么 NULL 用位图而不是每列一个标志字节】
 *
 * 位图更紧凑，但更重要的是：位图的位数在**建图时**就定了，
 * 记录大小随之定死 → 满足定长槽页的要求。
 * 变长方案（比如 NULL 就整个不存）会破坏这个前提。
 *
 * 【TEXT 的定长与"不截断"】
 *
 * TEXT 列宽在建图时定死。写入时若内容超过宽度 → 报 E_INVAL，
 * **不截断**。截断会让 SQL 返回的值和实际存进去的不一样 ——
 * 那是静默的数据错误，比报错难查得多。
 * （blob 模态的 TOAST 才是长文本的正道，但那是变长路径，本版不做。）
 */

#include "modalities/graph/graph.h"

#include <string.h>

/* 位图里的第 i 位 */
static inline bool gr_bit_get(const u8 *bitmap, int i)
{
    return (bitmap[i / 8] >> (i % 8)) & 1u;
}

static inline void gr_bit_set(u8 *bitmap, int i)
{
    bitmap[i / 8] |= (u8)(1u << (i % 8));
}

/* ---------------- 编码 ---------------- */

int gr_prop_encode(Graph *g, const DbValue *vals, int nvals, u8 *buf)
{
    if (!g || !buf)
        return E_INVAL;
    if (nvals != g->ncols)
        return E_INVAL;
    if (g->ncols > 0 && !vals)
        return E_INVAL;

    /* 位图先清零 */
    memset(buf + GR_N_BITMAP, 0, g->bitmap_bytes);

    for (int i = 0; i < g->ncols; i++) {
        const GraphCol *c = &g->cols[i];
        const DbValue *v = &vals[i];
        u8 *dst = buf + g->col_off[i];

        if (v->is_null) {
            if (!c->nullable)
                return E_INVAL;      /* 非空列不能给 NULL */
            gr_bit_set(buf + GR_N_BITMAP, i);
            memset(dst, 0, c->width);
            continue;
        }

        switch (c->type) {
        case T_INT4:
            if (v->type != T_INT4 && v->type != T_INT8)
                return E_INVAL;
            put_u32(dst, (u32)(i32)v->i);
            break;

        case T_INT8:
            if (v->type != T_INT4 && v->type != T_INT8)
                return E_INVAL;
            put_u64(dst, (u64)v->i);
            break;

        case T_BOOL:
            if (v->type != T_BOOL)
                return E_INVAL;
            dst[0] = v->i ? 1 : 0;
            break;

        case T_TEXT: {
            if (v->type != T_TEXT)
                return E_INVAL;
            /* 定长文本：超长报错，不截断 */
            if (v->len > c->width)
                return E_INVAL;
            memset(dst, 0, c->width);
            if (v->len)
                memcpy(dst, v->s, v->len);
            break;
        }

        default:
            return E_INVAL;
        }
    }

    return OK;
}

/* ---------------- 解码 ---------------- */

int gr_prop_decode(Graph *g, const u8 *buf, DbValue *out, int cap)
{
    if (!g || !buf || !out)
        return E_INVAL;
    if (cap < g->ncols)
        return E_NOSPACE;

    const u8 *bitmap = buf + GR_N_BITMAP;

    for (int i = 0; i < g->ncols; i++) {
        const GraphCol *c = &g->cols[i];
        const u8 *src = buf + g->col_off[i];
        DbValue *v = &out[i];

        if (gr_bit_get(bitmap, i)) {
            val_null(v);
            v->type = c->type;
            continue;
        }

        switch (c->type) {
        case T_INT4:
            val_int(v, (i32)get_u32(src));
            v->type = T_INT4;
            break;

        case T_INT8:
            val_int(v, (i64)get_u64(src));
            v->type = T_INT8;
            break;

        case T_BOOL:
            val_bool(v, src[0] != 0);
            v->type = T_BOOL;
            break;

        case T_TEXT: {
            /* 定长文本按 NUL 截尾（写入时用 0 填充剩余） */
            u16 n = 0;
            while (n < c->width && src[n] != 0)
                n++;
            val_text(v, (const char *)src, n);
            v->type = T_TEXT;
            break;
        }

        default:
            return E_CORRUPT;
        }
    }

    return OK;
}
