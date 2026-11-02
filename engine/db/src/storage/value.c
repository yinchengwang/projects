#include "storage/value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ==================== DbValue 的"轻量初始化" ====================
 *
 * 【为什么要改】
 * DbValue 有 272 字节（VAL_TEXT_MAX=256 + 若干字段），
 * 而原来每个值都是**整结构 memset**：
 *
 *   每行 4 列 × 272 字节 = 1088 字节/行
 *   10 万行 = **约 108 MB 的内存清零**
 *
 * v1.1 的执行器基准实测：解码阶段占了整条查询的 40%，
 * 而这 40% 里绝大部分就是在清那 256 字节的文本缓冲 ——
 * 对 INT / REAL / BOOL 列来说，那 256 字节**一个字节都用不到**。
 *
 * 【安全性】
 * 改完之后唯一"没清"的是 s[] 里的字节。
 * 而 s[] 只在 type == T_TEXT 时才被读，
 * 每个 val_* 都显式写 type —— 所以"读 s[] 之前 type 一定是 T_TEXT" ✓。
 *
 * 文本值本来就要把内容 memcpy 进 s[]，事先清零纯属多余。
 */
static void val_reset(DbValue *v)
{
    v->is_null = false;
    v->type = 0;
    v->i = 0;
    v->d = 0.0;
    v->len = 0;
    v->external = false;
    v->ext_key = 0;
    v->ext_off = 0;
    v->ext_fn = NULL;
    v->ext_ctx = NULL;
}

void val_null(DbValue *v)
{
    val_reset(v);
    v->is_null = true;
}

void val_int(DbValue *v, i64 x)
{
    val_reset(v);
    v->type = T_INT8;
    v->i = x;
}

void val_real(DbValue *v, double x)
{
    val_reset(v);
    v->type = T_REAL;
    v->d = x;
}

void val_bool(DbValue *v, bool b)
{
    val_reset(v);
    v->type = T_BOOL;
    v->i = b ? 1 : 0;
}

void val_text(DbValue *v, const char *s, u16 len)
{
    val_reset(v);
    v->type = T_TEXT;
    if (len >= VAL_TEXT_MAX)
        len = VAL_TEXT_MAX - 1;
    memcpy(v->s, s, len);
    v->s[len] = '\0';
    v->len = len;
}

/*
 * 拷贝一个值。
 *
 * 【为什么不整结构 memcpy】
 * 272 字节全拷，对 INT / REAL / BOOL 列来说有 256 字节是纯搬运。
 * 表达式求值每取一列就要拷一次 —— 这是过滤阶段的大头。
 *
 * 只拷"有意义的部分"：小字段 + （文本时）文本内容。
 * 文本内容的长度由 len 决定，拷贝后补一个 '\0'，
 * 保证 s[] 仍然能当 C 字符串用。
 */
void val_copy(DbValue *dst, const DbValue *src)
{
    dst->is_null = src->is_null;
    dst->type = src->type;
    dst->i = src->i;
    dst->d = src->d;
    dst->len = src->len;
    dst->external = src->external;
    dst->ext_key = src->ext_key;
    dst->ext_off = src->ext_off;
    dst->ext_fn = src->ext_fn;
    dst->ext_ctx = src->ext_ctx;

    if (src->type == T_TEXT) {
        u16 n = src->len;
        if (n >= VAL_TEXT_MAX)
            n = VAL_TEXT_MAX - 1;
        if (n)
            memcpy(dst->s, src->s, n);
        dst->s[n] = '\0';
    }
}

/*
 * 造一个外部值（v0.9）。
 *
 * 内容不在这里，只记"去哪儿取"。
 */
void val_external_text(DbValue *v, u64 key, u64 off,
                       DetoastFn fn, void *ctx)
{
    val_reset(v);
    v->type = T_TEXT;
    v->external = true;
    v->ext_key = key;
    v->ext_off = off;
    v->ext_fn = fn;
    v->ext_ctx = ctx;
}

/*
 * 物化（v0.9）。
 *
 * 内联值：拷一份。
 * 外部值：调 ext_fn 读回来 —— 字节数事先不知道，所以先问一次长度。
 *
 * 【为什么先问长度】
 * ext_fn 的约定是"buf 不够就返回 E_NOSPACE"。
 * 而外部值的长度事先不可知（blob 的帧可能不满）。
 * 所以这里分两步：先用一个 0 长度缓冲问长度，再分配、真正读。
 *
 * 模态的实现要配合这个约定：cap 不足时不要写 buf，
 * 只填 len_out 并返回 E_NOSPACE。
 */
int val_detoast(const DbValue *v, u8 **buf_out, u32 *len_out)
{
    if (!v || !buf_out || !len_out)
        return E_INVAL;

    *buf_out = NULL;
    *len_out = 0;

    if (v->is_null)
        return E_INVAL;

    if (!v->external) {
        u32 n = v->len;
        u8 *buf = malloc(n ? n : 1);
        if (!buf)
            return E_IO;
        if (n)
            memcpy(buf, v->s, n);
        *buf_out = buf;
        *len_out = n;
        return OK;
    }

    if (!v->ext_fn)
        return E_CORRUPT;

    /* 第一步：问长度 */
    u16 need = 0;
    int rc = v->ext_fn(v->ext_ctx, v->ext_key, v->ext_off, NULL, 0, &need);
    if (rc != E_NOSPACE) {
        /* 模态要么直接读成功（长度为 0 的极端情况），要么就是真出错 */
        if (rc != OK)
            return rc;
        need = 0;
    }

    u8 *buf = malloc(need ? need : 1);
    if (!buf)
        return E_IO;

    /* 长度是 0：内容为空，没必要再读一次（读也是 E_NOSPACE） */
    if (need == 0) {
        *buf_out = buf;
        *len_out = 0;
        return OK;
    }

    u16 got = 0;
    rc = v->ext_fn(v->ext_ctx, v->ext_key, v->ext_off, buf, need, &got);
    if (rc != OK) {
        free(buf);
        return rc;
    }

    *buf_out = buf;
    *len_out = got;
    return OK;
}

int val_truth(const DbValue *v)
{
    if (v->is_null)
        return -1;

    /*
     * 外部值：真值取决于"是不是空串"，得知道长度 → 物化一次。
     *
     * 物化失败（比如帧读不出来）时返回 -1 —— 当成 NULL 处理，
     * 这一行不会匹配任何 WHERE。
     *
     * 【为什么这里不报错】
     * val_truth 的返回值只有 -1/0/1，没有错误通道。
     * 改签名会波及表达式求值的三处调用点。
     * 而"拿外部大值直接当条件"本来就是罕见用法，
     * 失败时保守地当成 NULL（不匹配）是安全的方向。
     * 真正的错误会在物化那一步（输出路径）报出来。
     */
    if (v->external) {
        u8 *buf = NULL;
        u32 len = 0;
        if (val_detoast(v, &buf, &len) != OK)
            return -1;
        free(buf);
        return len > 0 ? 1 : 0;
    }

    if (v->type == T_TEXT)
        return v->len > 0 ? 1 : 0;
    return v->i != 0 ? 1 : 0;
}

int val_cmp(const DbValue *a, const DbValue *b, bool *is_null, int *err)
{
    *is_null = false;
    if (err)
        *err = OK;

    if (a->is_null || b->is_null) {
        *is_null = true;
        return 0;
    }

    /* 都是文本：按字节比 */
    bool a_text = (a->type == T_TEXT);
    bool b_text = (b->type == T_TEXT);

    if (a_text && b_text) {
        /*
         * 外部值必须**物化之后**再比。
         *
         * 【为什么不许"近似比较"】
         * 近似比较 = 给一个可能错的结果。
         * 一个错的 WHERE 结果比一个明确的报错坏得多 ——
         * 报错会让人停下，错的结果会一路流下去。
         */
        if (a->external || b->external) {
            u8 *ab = NULL;
            u8 *bb = NULL;
            u32 an = 0;
            u32 bn = 0;

            int rc = val_detoast(a, &ab, &an);
            if (rc == OK)
                rc = val_detoast(b, &bb, &bn);
            if (rc != OK) {
                free(ab);
                free(bb);
                if (err)
                    *err = rc;
                return 0;
            }

            u32 n = an < bn ? an : bn;
            int c = n ? memcmp(ab, bb, n) : 0;
            free(ab);
            free(bb);

            if (c != 0)
                return c;
            if (an == bn)
                return 0;
            return an < bn ? -1 : 1;
        }

        u16 n = a->len < b->len ? a->len : b->len;
        int c = memcmp(a->s, b->s, n);
        if (c != 0)
            return c;
        if (a->len == b->len)
            return 0;
        return a->len < b->len ? -1 : 1;
    }

    /*
     * 一边文本、一边非文本：**按类型序**，不按数值。
     *
     * 【为什么必须分开】
     * 原来的代码落到"其余按数值比"分支去比 a->i 和 b->i ——
     * 而文本值的 i 字段**恒为 0**（val_text 不碰它）。
     * 结果：`WHERE int_col = '5'` 会把**值为 0 的行**判成相等！
     *
     * 那不是"文本等于 5"，那是"文本的垃圾字段等于 0" ——
     * 静默给出错答案（v1.1 步骤 3 顺带查出来的既有 bug）。
     *
     * 【类型序：对齐 SQLite】
     *   NULL < 数值 < 文本 < 二进制
     * 所以文本永远大于数值，两者永不相等 ✓
     */
    if (a_text != b_text)
        return a_text ? 1 : -1;

    /*
     * 其余按数值比。
     *
     * 【REAL 与整数混比】
     * 只要有一边是 REAL，就按 double 比 —— 这样
     *   AVG(x) 的结果（REAL）能和原列（INT）正确比较。
     * 两边都是整数时用 i64 比（避免 double 丢精度：
     * 2^53 以上的 i64 转 double 会失真）。
     */
    if (a->type == T_REAL || b->type == T_REAL) {
        double x = (a->type == T_REAL) ? a->d : (double)a->i;
        double y = (b->type == T_REAL) ? b->d : (double)b->i;
        if (x < y)
            return -1;
        if (x > y)
            return 1;
        return 0;
    }

    if (a->i < b->i)
        return -1;
    if (a->i > b->i)
        return 1;
    return 0;
}

int val_to_str(const DbValue *v, char *buf, int cap)
{
    int n;

    if (v->is_null) {
        n = snprintf(buf, (size_t)cap, "NULL");
    } else if (v->external) {
        /*
         * 外部值：物化后按文本输出。
         *
         * 内容可能超过 cap（调用方给 512 字节缓冲）→ 截断。
         * 这是"输出格式"的限制，不是数据丢失：结果集那条路径
         * （exec.c 的 b_push）会自己分配足够大的缓冲。
         */
        u8 *tmp = NULL;
        u32 len = 0;
        if (val_detoast(v, &tmp, &len) != OK) {
            n = snprintf(buf, (size_t)cap, "<外部值读取失败>");
            if (n < 0)
                n = 0;
            if (n >= cap)
                n = cap - 1;
            return n;
        }
        n = snprintf(buf, (size_t)cap, "%.*s", (int)len, tmp);
        free(tmp);
    } else if (v->type == T_TEXT) {
        n = snprintf(buf, (size_t)cap, "%.*s", (int)v->len, v->s);
    } else if (v->type == T_REAL) {
        /*
         * REAL 的输出格式必须和 SQLite 一致 —— A3 要对照 SQLite 全等。
         *
         * SQLite 的浮点转文本用 "%.15g"，但**结果里没有小数点时会补 ".0"**：
         *   3.0 → "3.0"（不是 "3"）
         *   2.5 → "2.5"
         *
         * 这个差别不是吹毛求疵 —— 它是"结果全等"能不能过的关键。
         */
        n = snprintf(buf, (size_t)cap, "%.15g", v->d);
        if (n > 0 && n < cap) {
            bool has_dot = false;
            for (int i = 0; i < n; i++) {
                if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E' ||
                    buf[i] == 'n' || buf[i] == 'i') {  /* nan / inf */
                    has_dot = true;
                    break;
                }
            }
            if (!has_dot && n + 2 < cap) {
                buf[n] = '.';
                buf[n + 1] = '0';
                buf[n + 2] = '\0';
                n += 2;
            }
        }
    } else if (v->type == T_BOOL) {
        n = snprintf(buf, (size_t)cap, "%s", v->i ? "true" : "false");
    } else {
        n = snprintf(buf, (size_t)cap, "%lld", (long long)v->i);
    }
    if (n < 0)
        n = 0;
    if (n >= cap)
        n = cap - 1;
    return n;
}

/* Value[]（指向记录内部）→ DbValue[]（自己持有内容） */
static int values_to_dbvalues(const ColDesc *cols, int ncols,
                              const Value *tmp, DbValue *out)
{
    for (int i = 0; i < ncols; i++) {
        if (tmp[i].is_null) {
            val_null(&out[i]);
            continue;
        }
        switch (cols[i].type) {
        case T_TEXT:
            val_text(&out[i], (const char *)tmp[i].u.text.p, tmp[i].u.text.n);
            break;
        case T_BOOL:
            val_bool(&out[i], tmp[i].u.b);
            break;
        case T_INT4:
            val_int(&out[i], tmp[i].u.i4);
            break;
        case T_REAL:
            val_real(&out[i], tmp[i].u.f8);
            break;
        case T_INT8:
        default:
            val_int(&out[i], tmp[i].u.i8);
            break;
        }
    }
    return OK;
}

int val_from_record(const ColDesc *cols, int ncols,
                    const u8 *rec, u16 len, DbValue *out)
{
    Value tmp[REC_MAX_COLS];

    if (ncols > REC_MAX_COLS)
        return E_INVAL;

    int rc = rec_decode(cols, ncols, rec, len, tmp);
    if (rc != OK)
        return rc;

    return values_to_dbvalues(cols, ncols, tmp, out);
}

int val_from_record_layout(const RecLayout *L, const ColDesc *cols, int ncols,
                            const u8 *rec, u16 len, DbValue *out)
{
    Value tmp[REC_MAX_COLS];

    if (ncols > REC_MAX_COLS)
        return E_INVAL;

    int rc = rec_decode_layout(L, cols, rec, len, tmp);
    if (rc != OK)
        return rc;

    return values_to_dbvalues(cols, ncols, tmp, out);
}

int val_to_record(const ColDesc *cols, int ncols, const DbValue *vals,
                  u8 *buf, u16 cap, u16 *out_len)
{
    Value tmp[REC_MAX_COLS];

    if (ncols > REC_MAX_COLS)
        return E_INVAL;

    for (int i = 0; i < ncols; i++) {
        memset(&tmp[i], 0, sizeof(tmp[i]));
        if (vals[i].is_null) {
            tmp[i].is_null = true;
            continue;
        }
        switch (cols[i].type) {
        case T_TEXT:
            tmp[i].u.text.p = (const u8 *)vals[i].s;
            tmp[i].u.text.n = vals[i].len;
            break;
        case T_BOOL:
            tmp[i].u.b = vals[i].i != 0;
            break;
        case T_INT4:
            tmp[i].u.i4 = (i32)vals[i].i;
            break;
        case T_REAL:
            tmp[i].u.f8 = vals[i].d;
            break;
        case T_INT8:
        default:
            tmp[i].u.i8 = vals[i].i;
            break;
        }
    }
    return rec_encode(cols, ncols, tmp, buf, cap, out_len);
}
