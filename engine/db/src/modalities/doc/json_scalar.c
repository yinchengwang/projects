/*
 * json_scalar.c —— 判决 1 的映射（JSON 值 → 通用标量）+ 标量比较
 *
 * ==================== 这个文件是唯一的主人 ====================
 *
 * ⚠️ **全项目唯一做"JSON 值 → DbValue"这件事的地方。**
 *
 * 路径索引按提取值建键，执行器重判 WHERE 时也用同一套提取规则 ——
 * 两边调的是**同一个**函数（json_extract_scalar / json_extract_scalar_tree），
 * 所以不可能语义分叉。
 *
 * 这与 v1.5 的坑 M6-63（超集铁律只能有一个主人）是同一条纪律：
 * **一个语义只能有一个主人**。任何别处想"顺手转一下"都必须调这里。
 *
 * ==================== 映射表（写死，一个字都不能差） ====================
 *
 *   JSON 字符串            → DbValue(TEXT)
 *   JSON 整数              → DbValue(INT)
 *   JSON 小数              → DbValue(REAL)
 *   JSON true / false      → DbValue(INT 1 / 0)
 *   JSON null              → DbValue(NULL)
 *   路径不存在             → DbValue(NULL)
 *   类型不匹配             → DbValue(NULL)
 *   object / array         → DbValue(NULL)
 *
 * ⚠️ **"类型不匹配"必须落到 NULL，不能落到"转换后的值"**：
 * 若偷偷把字符串 "abc" 转成 0，那 `json_extract(doc,'$.a') = 0`
 * 会**误判成 TRUE** —— 那是**静默给错答案**。
 *
 * ⚠️ **NULL 不是"空字符串"、也不是"0"**：
 * DbValue 的 is_null = true，type 保持 T_TEXT。
 *
 * ==================== 比较（json_match 的 `=`） ====================
 *
 * ⚠️ **NULL 参与比较 ⇒ 返回 E_NOTFOUND（"不知道"）**，不是 FALSE。
 * 这是 SQL 三值逻辑的坑：把"不知道"当成"假"，会把"这行满足条件"判反。
 *
 * ⚠️ INT 与 REAL：数值相等即相等（1 == 1.0 ⇒ TRUE）。
 * ⚠️ TEXT：按字节比（SQLite JSON1 同款，不做 Unicode 归一化）。
 * ⚠️ **TEXT 与数字永不相等**（"5" ≠ 5）—— 不做隐式转换。
 *
 * ==================== 坑 ====================
 *
 * 坑 M7-5：超长的字符串**不许截断**。
 *   DbValue 的内联文本只有 255 字节，val_text() 会**默默截断** ——
 *   而截断 = 静默给错答案（查到的值与文档里的值不是一回事）。
 *   这里的选择是**报错**（E_NOSPACE）：宁可这条查询失败，
 *   也不要给出一个"看起来对"的短字符串。
 *   （要根治得靠外部值，见 value.h 的外部值机制。）
 *
 * 坑 M7-6：object / array 的提取结果是 NULL，**不是错误**。
 *   判据"json_extract(doc,'$.a') = 5"里，若 $.a 是对象，
 *   正确的答案是"不是 5"（NULL ⇒ 未知），不是"报个错把查询打断"。
 *
 * 坑 M7-7：路径不存在（E_NOTFOUND）与"值就是 JSON null"（OK + null）
 *   对外**都是** DbValue(NULL)，区别只在诊断信息上。
 *   所以 json_extract_scalar 在路径不存在时**既**返回 E_NOTFOUND
 *   **又**把 out 填成 NULL 值 —— 调用方无论看哪一边，行为都一样。
 *
 * 坑 M7-11：**类型不同永不相等**，不做任何隐式转换。
 *   字符串 "5" 与数字 5 不是一回事（SQLite JSON1 同款）。
 *   偷偷转过去 = 静默给错答案（与坑 M7-5 同源）。
 *
 * 坑 M7-12：INT 与 REAL 比较的**精度坑**。
 *   能精确转成 i64 的实数走 i64 比较，
 *   否则 (double)int 的转换会丢精度
 *   （9007199254740993 与 9007199254740992.0 会被判成相等）。
 */

#include "modalities/doc/json_path.h"

#include <math.h>
#include <string.h>

/* ---------------- 小工具 ---------------- */

/* 造一个"NULL 值"：is_null = true，type 保持 T_TEXT */
static void make_null(DbValue *out)
{
    val_null(out);
    /*
     * ⚠️ val_null() 把 type 清成 0；这里显式改成 T_TEXT：
     * NULL 不是"空字符串"、也不是"0"，但值的类型槽位要保持"文本型"，
     * 否则外部值机制与结果集序列化会把它当成"没有类型"的值。
     */
    out->type = T_TEXT;
}

/* 小端读 u32 / u64 */
static u32 rd_u32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 rd_u64(const u8 *p)
{
    return (u64)rd_u32(p) | ((u64)rd_u32(p + 4) << 32);
}

/* ---------------- 字节流 → DbValue ---------------- */

static int map_bytes(const u8 *val, u32 len, DbValue *out)
{
    JsonbTag tag = JSONB_NULL;
    int rc;

    rc = jsonb_tag_of(val, len, &tag);
    if (rc != OK)
        return rc;

    switch (tag) {
    case JSONB_NULL:
        make_null(out);
        return OK;

    case JSONB_TRUE:
        val_int(out, 1);
        return OK;

    case JSONB_FALSE:
        val_int(out, 0);
        return OK;

    case JSONB_INT32:
        if (len < 5u)
            return E_CORRUPT;
        val_int(out, (i64)(i32)rd_u32(val + 1));
        return OK;

    case JSONB_INT64:
        if (len < 9u)
            return E_CORRUPT;
        val_int(out, (i64)rd_u64(val + 1));
        return OK;

    case JSONB_FLOAT64: {
        u64 bits;
        double d;

        if (len < 9u)
            return E_CORRUPT;
        bits = rd_u64(val + 1);
        memcpy(&d, &bits, sizeof(d));
        val_real(out, d);
        return OK;
    }

    case JSONB_STRING: {
        u32 slen;

        if (len < 5u)
            return E_CORRUPT;
        slen = rd_u32(val + 1);
        if (slen > len - 5u)
            return E_CORRUPT;
        /* 坑 M7-5：不许截断 ⇒ 超长直接报错 */
        if (slen >= (u32)VAL_TEXT_MAX)
            return E_NOSPACE;
        val_text(out, (const char *)(val + 5), (u16)slen);
        return OK;
    }

    case JSONB_ARRAY:
    case JSONB_OBJECT:
        /* 坑 M7-6：object / array ⇒ NULL（不是错误） */
        make_null(out);
        return OK;

    default:
        return E_CORRUPT;
    }
}

/* ---------------- 树 → DbValue ---------------- */

static int map_tree(const JsonbValue *v, DbValue *out)
{
    switch (v->tag) {
    case JSONB_NULL:
        make_null(out);
        return OK;

    case JSONB_TRUE:
        val_int(out, 1);
        return OK;

    case JSONB_FALSE:
        val_int(out, 0);
        return OK;

    case JSONB_INT32:
    case JSONB_INT64:
        val_int(out, v->u.i);
        return OK;

    case JSONB_FLOAT64:
        val_real(out, v->u.d);
        return OK;

    case JSONB_STRING:
        /* 坑 M7-5：不许截断 */
        if (v->u.s.len >= (u32)VAL_TEXT_MAX)
            return E_NOSPACE;
        val_text(out, v->u.s.p, (u16)v->u.s.len);
        return OK;

    case JSONB_ARRAY:
    case JSONB_OBJECT:
        make_null(out);
        return OK;

    default:
        return E_CORRUPT;
    }
}

/* ---------------- 判决 1 的两个入口 ---------------- */

int json_extract_scalar(const u8 *buf, u32 len, const JsonPath *path,
                        DbValue *out)
{
    const u8 *val = NULL;
    u32 val_len = 0;
    int rc;

    if (out == NULL)
        return E_INVAL;

    /* 失败路径也留一个干净的 NULL 值（不留脏数据） */
    make_null(out);

    rc = jsonb_extract(buf, len, path, &val, &val_len);
    if (rc == E_NOTFOUND) {
        /* 坑 M7-7：路径不存在 ⇒ 对外也是 DbValue(NULL) */
        return E_NOTFOUND;
    }
    if (rc != OK)
        return rc;

    return map_bytes(val, val_len, out);
}

int json_extract_scalar_tree(const JsonbValue *root, const JsonPath *path,
                             DbValue *out)
{
    const JsonbValue *node = NULL;
    int rc;

    if (out == NULL)
        return E_INVAL;

    make_null(out);

    rc = jsonb_extract_tree(root, path, &node);
    if (rc == E_NOTFOUND)
        return E_NOTFOUND;   /* 坑 M7-7 */
    if (rc != OK)
        return rc;

    return map_tree(node, out);
}

/* ---------------- 比较辅助 ---------------- */

/* 这个值是不是"数值"（整数 / 布尔 / 实数） */
static bool is_numeric(const DbValue *v)
{
    return v->type == T_INT4 || v->type == T_INT8 ||
           v->type == T_BOOL || v->type == T_REAL;
}

/* 这个值是不是"整数型"（布尔也算：true/false 映射成 INT 1/0） */
static bool is_integral(const DbValue *v)
{
    return v->type == T_INT4 || v->type == T_INT8 || v->type == T_BOOL;
}

/*
 * 数值相等？
 *
 * ⚠️ 整数与实数比：**能精确转成 i64 的实数走 i64 比较**，
 * 否则 (double)int 的转换会丢精度
 * （9007199254740993 与 9007199254740992.0 会被判成相等）。坑 M7-12。
 */
static bool num_eq(const DbValue *a, const DbValue *b)
{
    if (is_integral(a) && is_integral(b))
        return a->i == b->i;

    if (is_integral(a) || is_integral(b)) {
        const DbValue *iv = is_integral(a) ? a : b;
        const DbValue *rv = is_integral(a) ? b : a;
        double dr = rv->d;

        if (dr == floor(dr) && dr >= -9223372036854775808.0 &&
            dr < 9223372036854775808.0)
            return iv->i == (i64)dr;

        return (double)iv->i == dr;
    }

    return a->d == b->d;   /* 两个都是 REAL：NaN 自然不相等 */
}

int json_scalar_eq(const DbValue *a, const DbValue *b, bool *out)
{
    if (a == NULL || b == NULL || out == NULL)
        return E_INVAL;

    /*
     * ⚠️ **NULL 参与比较 ⇒ "不知道"，不是 FALSE**。
     * 这是 SQL 三值逻辑：把"不知道"当成"假"会把满足条件的行判反。
     */
    if (a->is_null || b->is_null)
        return E_NOTFOUND;

    if (is_numeric(a) && is_numeric(b)) {
        *out = num_eq(a, b);
        return OK;
    }

    if (a->type == T_TEXT && b->type == T_TEXT) {
        /* 按字节比（SQLite JSON1 同款，不做 Unicode 归一化） */
        *out = (a->len == b->len) && (memcmp(a->s, b->s, a->len) == 0);
        return OK;
    }

    /*
     * 类型不同 ⇒ 不相等。
     * ⚠️ **不做隐式转换**：字符串 "5" 与数字 5 不是一回事
     *    （偷偷转过去 = 静默给错答案，坑 M7-11）。
     */
    *out = false;
    return OK;
}

/* ---------------- 超长字符串：外部值兜底（lead 裁决 · M7-5 的根治） ---------------- */

/*
 * 取路径上的字符串值，返回它在 JSONB 缓冲里的位置。
 *
 * ⚠️ **只对 JSONB_STRING 有效**；其它类型 ⇒ E_INVAL。
 * 路径不存在 ⇒ E_NOTFOUND。
 *
 * 为什么要单独一个函数：`json_extract_scalar()` 是**纯函数**，
 * 它不碰 DbValue 之外的东西 ⇒ 遇到超长字符串只能报 E_NOSPACE。
 * 而调用方需要知道"那个字符串在哪"才能造外部值 ⇒ 这里给它一个出口。
 *
 * ⚠️ 返回的 slen 是**真实长度**（不截断）——
 *    若它超过 65535 字节，外部值机制表达不了
 *    （DetoastFn 的长度出口是 u16），json_detoast_mem 会返回 E_NOTSUP。
 */
int json_extract_string_ext(const u8 *buf, u32 len, const JsonPath *path,
                            const char **sp, u32 *slen)
{
    const u8 *val = NULL;
    u32 val_len = 0;
    int rc;

    if (buf == NULL || path == NULL || sp == NULL || slen == NULL)
        return E_INVAL;

    *sp = NULL;
    *slen = 0;

    rc = jsonb_extract(buf, len, path, &val, &val_len);
    if (rc != OK)
        return rc;

    /* 值的第一个字节是 tag */
    if (val_len < 1u)
        return E_CORRUPT;
    if (val[0] != (u8)JSONB_STRING)
        return E_INVAL;          /* 不是字符串 */

    if (val_len < 5u)
        return E_CORRUPT;
    {
        u32 sl = rd_u32(val + 1);

        if (sl > val_len - 5u)
            return E_CORRUPT;
        *sp = (const char *)(val + 5);
        *slen = sl;
    }
    return OK;
}

/*
 * "从内存里读"的 detoast 函数。
 *
 * ctx 指向一块内存，key = 长度，off = 0。
 *
 * ⚠️ **ctx 指向的内存必须活到值被用完** ——
 * 外部值不持有内存，只持有句柄与函数指针（value.h 的说明）。
 * 调用方的责任。
 *
 * 契约与 DetoastFn 一致：先问长度、再取内容；不够 ⇒ E_NOSPACE。
 */
int json_detoast_mem(void *ctx, u64 key, u64 off,
                     u8 *buf, u16 cap, u16 *len_out)
{
    const char *src = (const char *)ctx;
    u32 n;

    if (src == NULL)
        return E_CORRUPT;

    /* 本实现只有"从开头读"这一种位置（off 恒为 0） */
    if (off != 0)
        return E_INVAL;

    /*
     * ⚠️ **长度出口只有 u16**（DetoastFn 的 len_out 是 u16）⇒
     *    超过 65535 字节的字符串在这套机制里**表达不了**。
     *
     * ⚠️ 这里**必须**返回 E_NOTSUP，不能返回 E_NOSPACE：
     *    "问长度"那一步的 E_NOSPACE 的含义是"长度问到了"，
     *    val_detoast 拿到 E_NOSPACE 就直接用 len_out 里的长度去读；
     *    若此时 len_out 没填，val_detoast 会以为内容是空的
     *    ⇒ **静默给错答案**（超长字符串变成空串）。
     */
    if (key > 0xFFFFu)
        return E_NOTSUP;

    n = (u32)key;

    /* 第一次调用：只问长度（契约：填 len_out，返回 E_NOSPACE） */
    if (buf == NULL || cap == 0u) {
        if (len_out)
            *len_out = (u16)n;
        return E_NOSPACE;
    }

    /*
     * 缓冲不够：**不写 buf**，只填 len_out 并返回 E_NOSPACE
     * （value.h 里 val_detoast 的约定）。
     */
    if (cap < n) {
        if (len_out)
            *len_out = (u16)n;
        return E_NOSPACE;
    }

    if (n)
        memcpy(buf, src, n);
    if (len_out)
        *len_out = (u16)n;
    return OK;
}
