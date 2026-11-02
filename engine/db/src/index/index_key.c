#include "index/index_key.h"

#include <string.h>

/*
 * ==================== 值部分 ====================
 *
 * 值部分只编码"这一列的值"，不带 RID。
 * 复合键 = 值部分 || RID。
 */

/* RID 部分：8 字节大端，保证字节序 == RID 数值序 */
static void encode_rid(u64 rid, u8 *buf)
{
    for (int i = 0; i < 8; i++)
        buf[i] = (u8)(rid >> (56 - 8 * i));
}

/*
 * 文本的字节编码：原字节 + 转义 + 终止符。
 *
 * 【为什么要终止符】
 * 复合键 = 值部分 || RID。如果值部分可能互为前缀（"ab" 是 "abc" 的前缀），
 * 比较两个复合键时就会比较到 RID 字节去 —— 顺序全乱。
 * 加上终止符后，值部分互不为前缀，比较一定在值部分里结束。
 *
 * 【为什么要转义]
 * 文本里可能有 0x00 字节（DbValue 用显式长度，不是 C 字符串）。
 * 如果不转义，"a\0b" 的终止符会被认成 "a" 的终止符。
 * 约定：0x00 → 0x00 0xFF，终止符 = 0x00 0x00。
 * 编码后仍保持字典序（"a" < "a\0" < "a\0b"）。
 */
static int encode_text(const DbValue *v, u8 *buf, u16 cap, u16 *len_out)
{
    u16 n = 0;
    u16 tl = v->len;

    for (u16 i = 0; i < tl; i++) {
        if (n + 2u > cap)
            return E_NOSPACE;
        u8 ch = (u8)v->s[i];
        buf[n++] = ch;
        if (ch == 0x00)
            buf[n++] = 0xFF;        /* 转义 */
    }

    if (n + 2u > cap)
        return E_NOSPACE;
    buf[n++] = 0x00;                /* 终止符 */
    buf[n++] = 0x00;

    *len_out = n;
    return OK;
}

/*
 * 常量的类型能不能用列的类型编码？
 *
 * 【为什么要有这个闸门】
 * 索引键是**按列的类型**编码的，而常量的类型来自字面量。
 * 两者不一定一致 —— 不一致时如果硬编码，读到的字段就是错的：
 *
 *   int 列 + 文本常量 '5' → 读 v->i（文本的 i 恒为 0）→ 边界编码成 0
 *                        → 索引扫出"值为 0 的行"，真正的 5 被漏掉
 *   real 列 + 整数常量 5  → 读 v->d（整数的 d 恒为 0）→ 边界编码成 0.0
 *   text 列 + 整数常量 5  → 读 v->len（整数的 len 恒为 0）→ 边界编码成空串
 *
 * 三种都是**静默漏行**。所以类型对不上就**拒绝编码** ——
 * 上层（heap_am_push）收到 E_NOTSUP 就退回全表扫描 ✓
 * （慢一点，但绝不漏行 —— "多给行 = 慢；少给行 = 数据错"。）
 */
static bool type_ok(u8 col_type, u8 val_type)
{
    switch (col_type) {
    case T_INT4:
    case T_INT8:
        /* 整数列：整数常量可以；实数常量不行（要按 double 比，语义不同） */
        return val_type == T_INT4 || val_type == T_INT8 || val_type == T_BOOL;
    case T_BOOL:
        return val_type == T_BOOL || val_type == T_INT4 || val_type == T_INT8;
    case T_REAL:
        /*
         * 实数列：实数常量可以；整数常量也可以 ——
         * 因为比较语义就是"转成 double 再比"（见 val_cmp），
         * 边界也转成 double，**与比较语义完全一致** ✓
         */
        return val_type == T_REAL || val_type == T_INT4 ||
               val_type == T_INT8 || val_type == T_BOOL;
    case T_TEXT:
        return val_type == T_TEXT;
    default:
        return false;
    }
}

/* 值部分：把一列的值编码成"字节序 = 数值序"的字节串 */
static int encode_value_part(const ColDesc *col, const DbValue *v,
                              u8 *buf, u16 cap, u16 *len_out)
{
    if (!col || !v || !buf || !len_out)
        return E_INVAL;

    if (v->is_null)
        return E_INVAL;             /* 索引不存 NULL */

    if (!type_ok(col->type, v->type))
        return E_NOTSUP;            /* 类型对不上 → 不许编码（见 type_ok） */

    switch (col->type) {
    case T_INT4: {
        if (cap < 4)
            return E_NOSPACE;
        u32 u = (u32)(i32)v->i ^ 0x80000000u;    /* 翻转符号位 */
        buf[0] = (u8)(u >> 24);
        buf[1] = (u8)(u >> 16);
        buf[2] = (u8)(u >> 8);
        buf[3] = (u8)u;
        *len_out = 4;
        return OK;
    }

    case T_INT8: {
        if (cap < 8)
            return E_NOSPACE;
        u64 u = (u64)v->i ^ 0x8000000000000000ull;
        for (int i = 0; i < 8; i++)
            buf[i] = (u8)(u >> (56 - 8 * i));
        *len_out = 8;
        return OK;
    }

    case T_BOOL:
        if (cap < 1)
            return E_NOSPACE;
        buf[0] = v->i ? 1 : 0;
        *len_out = 1;
        return OK;

    case T_REAL: {
        /*
         * 浮点的 key 编码必须让 **memcmp 的字节序 == 数值序**。
         *
         * 【为什么不能直接存 IEEE754 位模式】
         * IEEE754 的位模式不是单调的：
         *   -1.0  = 0xBFF0000000000000
         *   -2.0  = 0xC000000000000000
         *   +0.0  = 0x0000000000000000
         * 位模式比大小的话，-2.0 反而"小于" -1.0 —— 索引顺序全乱。
         *
         * 【标准做法：符号位翻转】
         *   正数（含 +0）：把符号位置 1   → 0x8000... 变成最大的一半
         *   负数：         全部位取反     → 负数域变成"更小"的域
         * 变换之后，位模式的无符号大小 == 浮点的数值大小。
         *
         * 然后仍然要**大端**（和整数一样）—— memcmp 是从高位比的。
         *
         * （-0.0 与 +0.0 变换后不相等，但它们数值相等。
         *  这个边界学习项目不特殊处理 —— SQLite 也不处理。）
         */
        if (cap < 8)
            return E_NOSPACE;
        /* 整数常量按比较语义转成 double（与 val_cmp 一致） */
        double d = (v->type == T_REAL) ? v->d : (double)v->i;
        u64 bits;
        memcpy(&bits, &d, 8);
        u64 key = (bits & 0x8000000000000000ull)
                ? ~bits
                : (bits | 0x8000000000000000ull);
        for (int i = 0; i < 8; i++)
            buf[i] = (u8)(key >> (56 - 8 * i));
        *len_out = 8;
        return OK;
    }

    case T_TEXT:
    default:
        return encode_text(v, buf, cap, len_out);
    }
}

/* ---------------- 复合键 ---------------- */

int index_key_encode(const ColDesc *col, const DbValue *v, u64 rid,
                      u8 *buf, u16 cap, u16 *len_out)
{
    if (!col || !v || !buf || !len_out)
        return E_INVAL;

    u16 n = 0;
    int rc = encode_value_part(col, v, buf, cap, &n);
    if (rc != OK)
        return rc;

    if ((u32)n + 8u > cap)
        return E_NOSPACE;
    encode_rid(rid, buf + n);

    *len_out = (u16)(n + 8u);
    return OK;
}

/* ---------------- 下推边界 ---------------- */

int index_key_bound_lo(const ColDesc *col, const DbValue *v,
                        u8 *buf, u16 cap, u16 *len_out)
{
    if (!col || !v || !buf || !len_out)
        return E_INVAL;

    u16 n = 0;
    int rc = encode_value_part(col, v, buf, cap, &n);
    if (rc != OK)
        return rc;

    if ((u32)n + 8u > cap)
        return E_NOSPACE;
    memset(buf + n, 0x00, 8);        /* 最小可能 RID */

    *len_out = (u16)(n + 8u);
    return OK;
}

int index_key_bound_hi(const ColDesc *col, const DbValue *v,
                        u8 *buf, u16 cap, u16 *len_out)
{
    if (!col || !v || !buf || !len_out)
        return E_INVAL;

    u16 n = 0;
    int rc = encode_value_part(col, v, buf, cap, &n);
    if (rc != OK)
        return rc;

    if ((u32)n + 8u > cap)
        return E_NOSPACE;
    memset(buf + n, 0xFF, 8);        /* 最大可能 RID */

    *len_out = (u16)(n + 8u);
    return OK;
}
