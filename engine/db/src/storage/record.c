/*
 * record.c — tuple 编解码
 *
 * 布局与字节序约定见 record.h 顶部注释。
 * 这里只做纯内存转换，不碰页、不碰文件。
 */

#include "storage/record.h"

#include <string.h>

u16 rec_type_width(u8 type)
{
    switch (type) {
    case T_INT4: return 4;
    case T_INT8: return 8;
    case T_BOOL: return 1;
    case T_REAL: return 8;
    case T_TEXT: return 0;   /* 变长：不占定长区 */
    default:     return 0;
    }
}

/* 一次算清所有区段的位置，避免每个列重复推导 */
int rec_layout(const ColDesc *cols, int ncols, RecLayout *L)
{
    if (!cols || !L || ncols <= 0 || ncols > REC_MAX_COLS)
        return E_INVAL;

    memset(L, 0, sizeof(*L));
    L->bitmap = (u16)((ncols + 7) / 8);
    L->fixed_off = (u16)(REC_HDR_SIZE + L->bitmap);
    L->ncols = (u16)ncols;

    /* 第一遍：算各区段长度 */
    for (int i = 0; i < ncols; i++) {
        u8 t = cols[i].type;
        if (t != T_INT4 && t != T_INT8 && t != T_BOOL && t != T_TEXT &&
            t != T_REAL)
            return E_INVAL;
        if (t == T_TEXT)
            L->nvar++;
        else
            L->fixed_len = (u16)(L->fixed_len + rec_type_width(t));
    }

    L->varoff_off = (u16)(L->fixed_off + L->fixed_len);
    L->data_off = (u16)(L->varoff_off + 2u * L->nvar);

    if (L->data_off > PAGE_SIZE)
        return E_INVAL;

    /* 第二遍：把每列的位置一次算清 —— 解码时 O(1) 取用 */
    u16 off = L->fixed_off;
    u16 k = 0;
    for (int i = 0; i < ncols; i++) {
        if (cols[i].type == T_TEXT) {
            L->fixed_col_off[i] = 0xFFFFu;
            L->var_col_idx[i] = k++;
        } else {
            L->fixed_col_off[i] = off;
            L->var_col_idx[i] = 0xFFFFu;
            off = (u16)(off + rec_type_width(cols[i].type));
        }
    }
    return OK;
}

int rec_fixed_size(const ColDesc *cols, int ncols, u16 *out)
{
    if (!out)
        return E_INVAL;
    RecLayout L;
    int rc = rec_layout(cols, ncols, &L);
    if (rc != OK)
        return rc;
    *out = L.data_off;
    return OK;
}

int rec_encode(const ColDesc *cols, int ncols, const Value *vals,
               u8 *buf, u16 bufsize, u16 *out_len)
{
    if (!buf || !out_len || !vals)
        return E_INVAL;

    RecLayout L;
    int rc = rec_layout(cols, ncols, &L);
    if (rc != OK)
        return rc;
    if (L.data_off > bufsize)
        return E_NOSPACE;

    /* 定长区（含 bitmap、变长偏移数组）先清零：
     * NULL 列的定长值区留 0，偏移数组也不会读到垃圾 */
    memset(buf, 0, L.data_off);

    u16 cur = L.data_off;   /* 变长数据写入位置 */

    for (int i = 0; i < ncols; i++) {
        bool isnull = vals[i].is_null;

        if (isnull && !cols[i].nullable)
            return E_INVAL;

        if (isnull)
            buf[REC_HDR_SIZE + i / 8] |= (u8)(1u << (i % 8));

        if (cols[i].type == T_TEXT) {
            /* 偏移数组对 NULL 列也要写（写当前游标），
             * 这样每列的结束位置总能由"下一个偏移"得到，解码不用特判 */
            u16 k = L.var_col_idx[i];
            put_u16(buf + L.varoff_off + 2u * k, cur);
            if (!isnull) {
                u16 n = vals[i].u.text.n;
                if ((u32)cur + n > bufsize)
                    return E_NOSPACE;
                if (n)
                    memcpy(buf + cur, vals[i].u.text.p, n);
                cur = (u16)(cur + n);
            }
        } else {
            if (isnull)
                continue;                  /* 值区保持 0 */
            u16 off = L.fixed_col_off[i];
            switch (cols[i].type) {
            case T_INT4: put_u32(buf + off, (u32)vals[i].u.i4); break;
            case T_INT8: put_u64(buf + off, (u64)vals[i].u.i8); break;
            case T_BOOL: buf[off] = vals[i].u.b ? 1u : 0u; break;
            case T_REAL: {
                /*
                 * 浮点按 IEEE754 的位模式存（小端）。
                 *
                 * 【为什么不直接 memcpy 8 字节】
                 * 本系统的页内格式一律小端 —— 直接 memcpy 的话，
                 * 大端机上存进去的字节序就反了。
                 * 位模式本身与机器无关（IEEE754 是标准），
                 * 但**字节摆放顺序**与机器有关，所以必须走 put_u64。
                 */
                u64 bits;
                memcpy(&bits, &vals[i].u.f8, 8);
                put_u64(buf + off, bits);
                break;
            }
            default: return E_INVAL;
            }
        }
    }

    *out_len = cur;
    return OK;
}

/*
 * 用预计算好的布局解码 —— 热路径。
 *
 * 每列的位置都在布局里算好了，这里只做取值，不再有任何循环推导。
 */
int rec_decode_layout(const RecLayout *L, const ColDesc *cols,
                      const u8 *buf, u16 len, Value *vals)
{
    if (!L || !cols || !buf || !vals)
        return E_INVAL;
    if (len < L->data_off)
        return E_CORRUPT;

    for (int i = 0; i < L->ncols; i++) {
        bool isnull = ((buf[REC_HDR_SIZE + i / 8] >> (i % 8)) & 1u) != 0;
        memset(&vals[i].u, 0, sizeof(vals[i].u));
        vals[i].is_null = isnull;
        if (isnull)
            continue;

        if (cols[i].type == T_TEXT) {
            u16 k = L->var_col_idx[i];
            u16 s = get_u16(buf + L->varoff_off + 2u * k);
            u16 e = (k + 1u < L->nvar)
                  ? get_u16(buf + L->varoff_off + 2u * (k + 1u))
                  : len;
            if (s > e || e > len)
                return E_CORRUPT;
            vals[i].u.text.p = buf + s;
            vals[i].u.text.n = (u16)(e - s);
        } else {
            u16 off = L->fixed_col_off[i];
            switch (cols[i].type) {
            case T_INT4: vals[i].u.i4 = (i32)get_u32(buf + off); break;
            case T_INT8: vals[i].u.i8 = (i64)get_u64(buf + off); break;
            case T_BOOL: vals[i].u.b = buf[off] != 0; break;
            case T_REAL: {
                u64 bits = get_u64(buf + off);
                memcpy(&vals[i].u.f8, &bits, 8);
                break;
            }
            default: return E_INVAL;
            }
        }
    }
    return OK;
}

/*
 * 每行重算布局的版本 —— 只在不热的路径上用（一次性解码）。
 * 热路径请用 rec_decode_layout。
 */
int rec_decode(const ColDesc *cols, int ncols, const u8 *buf, u16 len, Value *vals)
{
    if (!cols || !buf || !vals)
        return E_INVAL;

    RecLayout L;
    int rc = rec_layout(cols, ncols, &L);
    if (rc != OK)
        return rc;

    return rec_decode_layout(&L, cols, buf, len, vals);
}

int rec_get_col(const ColDesc *cols, int ncols, const u8 *buf, u16 len,
                int col, Value *out)
{
    if (!buf || !out || !cols)
        return E_INVAL;
    if (col < 0 || col >= ncols)
        return E_INVAL;

    RecLayout L;
    int rc = rec_layout(cols, ncols, &L);
    if (rc != OK)
        return rc;
    if (len < L.data_off)
        return E_CORRUPT;

    bool isnull = ((buf[REC_HDR_SIZE + col / 8] >> (col % 8)) & 1u) != 0;
    memset(&out->u, 0, sizeof(out->u));
    out->is_null = isnull;
    if (isnull)
        return OK;

    if (cols[col].type == T_TEXT) {
        u16 k = L.var_col_idx[col];
        u16 s = get_u16(buf + L.varoff_off + 2u * k);
        u16 e = (k + 1u < L.nvar)
              ? get_u16(buf + L.varoff_off + 2u * (k + 1u))
              : len;
        if (s > e || e > len)
            return E_CORRUPT;
        out->u.text.p = buf + s;
        out->u.text.n = (u16)(e - s);
    } else {
        u16 off = L.fixed_col_off[col];
        switch (cols[col].type) {
        case T_INT4: out->u.i4 = (i32)get_u32(buf + off); break;
        case T_INT8: out->u.i8 = (i64)get_u64(buf + off); break;
        case T_BOOL: out->u.b = buf[off] != 0; break;
        case T_REAL: {
            u64 bits = get_u64(buf + off);
            memcpy(&out->u.f8, &bits, 8);
            break;
        }
        default: return E_INVAL;
        }
    }
    return OK;
}

/* ---------------- 头部字段 ---------------- */

u64 rec_xmin(const u8 *buf)  { return get_u64(buf + R_XMIN); }
void rec_set_xmin(u8 *buf, u64 v) { put_u64(buf + R_XMIN, v); }

u64 rec_xmax(const u8 *buf)  { return get_u64(buf + R_XMAX); }
void rec_set_xmax(u8 *buf, u64 v) { put_u64(buf + R_XMAX, v); }

u32 rec_flags(const u8 *buf) { return get_u32(buf + R_FLAGS); }
void rec_set_flags(u8 *buf, u32 v) { put_u32(buf + R_FLAGS, v); }
