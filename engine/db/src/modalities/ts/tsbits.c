/*
 * tsbits.c —— 时序模态的位流基元（v1.5 步骤 1）
 *
 * 契约见 tsbits.h。
 *
 * 【位序】
 *
 * LSB-first：一个字节里第 0 个写的 bit 落在最低位。
 * 写的时候低位先填、填满 8 位换下一个字节；
 * 读的时候同样从低位取。
 *
 * 【变长整数（varint）】
 *
 * 7 位一组，**低位在前**，每组前面跟 1 个"续位"（1 = 后面还有组）。
 * 这是 LEB128 的位序版本 —— 和字节序版 LEB128 的区别只是组的顺序反过来，
 * 但**必须写清楚**，否则读出来的数是另一个数（而且不报错）。
 *
 * 注意：本文件的 varint 是**位级**的（续位和数据位混在同一个字节里），
 * 和常见的"整字节 varint"不是一回事 —— 别拿别处的实现来替换它。
 */

#include "modalities/ts/tsbits.h"

void tsbw_init(TsBitWriter *w, u8 *buf, u32 cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->nbits = 0;
    w->overflow = false;
}

int tsbw_write_bits(TsBitWriter *w, u64 value, u32 nbits)
{
    u32 i;

    if (!w)
        return E_INVAL;
    if (nbits > 64)
        return E_INVAL;

    for (i = 0; i < nbits; i++) {
        u32 byte_idx;
        u32 bit_idx;
        u8  mask;

        if ((w->nbits >> 3) >= w->cap) {
            w->overflow = true;
            return E_NOSPACE;
        }

        byte_idx = w->nbits >> 3;
        bit_idx = w->nbits & 7u;
        mask = (u8)(1u << bit_idx);

        if (value & (1ULL << i))
            w->buf[byte_idx] |= mask;
        else
            w->buf[byte_idx] &= (u8)~mask;

        w->nbits++;
    }

    if ((w->nbits >> 3) > w->len)
        w->len = w->nbits >> 3;
    return OK;
}

int tsbw_write_bit(TsBitWriter *w, bool bit)
{
    return tsbw_write_bits(w, bit ? 1ULL : 0ULL, 1);
}

int tsbw_write_varint(TsBitWriter *w, u64 value)
{
    int rc;

    for (;;) {
        u64 group = value & 0x7Fu;

        value >>= 7;
        if (value == 0) {
            /* 最后一组：第 7 位（续位）= 0 ⇒ 结束。**照样写满 8 位**，
               否则读端按 8 位一组去读就会把下一个数据位读成续位。 */
            rc = tsbw_write_bits(w, group, 8);
            return rc;
        }
        /* 续位 1 + 7 位数据（数据位在前，续位在第 7 位） */
        rc = tsbw_write_bits(w, group | 0x80ULL, 8);
        if (rc != OK)
            return rc;
    }
}

u32 tsbw_bit_length(const TsBitWriter *w)
{
    return w ? w->nbits : 0;
}

u32 tsbw_byte_length(const TsBitWriter *w)
{
    if (!w)
        return 0;
    return (w->nbits + 7u) / 8u;
}

int tsbw_finish(TsBitWriter *w, u32 *bytes_out)
{
    u32 need;

    if (!w)
        return E_INVAL;
    if (w->overflow)
        return E_NOSPACE;

    need = tsbw_byte_length(w);
    if (need > w->cap)
        return E_NOSPACE;

    w->len = need;
    if (bytes_out)
        *bytes_out = need;
    return OK;
}

void tsbr_init(TsBitReader *r, const u8 *buf, u32 len)
{
    r->buf = buf;
    r->len = len;
    r->nbits = len * 8u;
    r->pos = 0;
    r->bad = false;
}

int tsbr_read_bits(TsBitReader *r, u32 nbits, u64 *out)
{
    u64 v = 0;
    u32 i;

    if (!r || !out)
        return E_INVAL;
    if (nbits > 64)
        return E_INVAL;
    if (r->bad)
        return E_CORRUPT;
    if (r->pos + nbits > r->nbits) {
        r->bad = true;
        return E_CORRUPT;
    }

    for (i = 0; i < nbits; i++) {
        u32 byte_idx = (r->pos + i) >> 3;
        u32 bit_idx = (r->pos + i) & 7u;

        if (r->buf[byte_idx] & (u8)(1u << bit_idx))
            v |= (1ULL << i);
    }
    r->pos += nbits;
    *out = v;
    return OK;
}

int tsbr_read_bit(TsBitReader *r, bool *out)
{
    u64 v = 0;
    int rc = tsbr_read_bits(r, 1, &v);

    if (rc != OK)
        return rc;
    if (out)
        *out = (v != 0);
    return OK;
}

int tsbr_read_varint(TsBitReader *r, u64 *out)
{
    u64 v = 0;
    u32 shift = 0;
    int  rc;

    if (!r || !out)
        return E_INVAL;
    if (r->bad)
        return E_CORRUPT;

    for (;;) {
        u64 group = 0;

        rc = tsbr_read_bits(r, 8, &group);
        if (rc != OK)
            return rc;

        v |= (group & 0x7Fu) << shift;
        if ((group & 0x80u) == 0)
            break;

        shift += 7;
        if (shift >= 64) {
            /* 变长整数超过 64 位 ⇒ 格式错 */
            r->bad = true;
            return E_CORRUPT;
        }
    }
    *out = v;
    return OK;
}

bool tsbr_eof(const TsBitReader *r)
{
    if (!r)
        return true;
    return r->pos >= r->nbits;
}
