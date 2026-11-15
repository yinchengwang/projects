/*
 * json_parse.c —— 文本 JSON → 树（v1.6 步骤 1 · T13）
 *
 * 契约见 json_parse.h（语法、语义、数字归类都在那儿写死了）。
 *
 * ==================== 为什么要递归下降 ====================
 *
 * JSON 的语法本身就是递归的（值里可以套值）⇒ 递归下降是最自然的写法：
 * 一个 parse_value() 处理六种值，每种值自己往下走。
 *
 * ⚠️ **递归必须有深度上限**（JSON_PARSE_MAX_DEPTH）：
 *    递归下降遇上深嵌套会爆栈 ⇒ 上限之外**报错**，不是崩。
 *    （解码那边也有一份上限 —— 字节流同样能构造出深嵌套。）
 *
 * ==================== 数字为什么交给 strtoll / strtod ====================
 *
 * ⚠️ **不自制浮点解析**：自己写的浮点转换几乎一定有精度误差
 *    （十进制 → 二进制的舍入规则很麻烦）⇒ 用 C 库的 strtod，
 *    它是**正确舍入**的（glibc / mingw 都保证）。
 *
 * ⚠️ **不丢精度**：`1` 与 `1.0` 必须能区分开 ⇒
 *    "文本里有没有 '.' / 'e' / 'E'"决定它是整数还是浮点，
 *    整数再按**能不能放下**分 INT32 / INT64。
 *
 * ⚠️ **不因溢出而失败**：`1e999` ⇒ strtod 给 inf ⇒ FLOAT64；
 *    `9223372036854775808` ⇒ strtoll 溢出 ⇒ 退到 FLOAT64。
 *    SQLite 也接受这些，不许报错。
 *
 * ⚠️ **区域设置**：strtod 的小数点取自 LC_NUMERIC。
 *    本内核**从不调用 setlocale** ⇒ 一直是 "C" 区域（小数点是 '.'）✓。
 *    （哪天有人加了 setlocale，这里就会出错 —— 记一笔，坑 M7-19。）
 */

#include "modalities/doc/json_parse.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 解析器状态 ---------------- */

typedef struct {
    const char *p;
    u32         len;
    u32         pos;
    u32         depth;
} Parser;

/* ---------------- 小工具 ---------------- */

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static void skip_ws(Parser *ps)
{
    while (ps->pos < ps->len && is_ws(ps->p[ps->pos])) {
        ps->pos++;
    }
}

/* 读 4 位十六进制 */
static bool read_hex4(Parser *ps, u32 *out)
{
    u32 v = 0;
    u32 i;

    if (ps->len - ps->pos < 4u) {
        return false;
    }
    for (i = 0; i < 4u; i++) {
        char c = ps->p[ps->pos + i];
        u32 d;

        if (c >= '0' && c <= '9') {
            d = (u32)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (u32)(c - 'a') + 10u;
        } else if (c >= 'A' && c <= 'F') {
            d = (u32)(c - 'A') + 10u;
        } else {
            return false;
        }
        v = (v << 4) | d;
    }
    ps->pos += 4u;
    *out = v;
    return true;
}

/*
 * 把一个码点写成 UTF-8。
 *
 * ⚠️ 代理项（U+D800~U+DFFF）**不会**走到这里 ——
 *    落单的代理项在 UTF-8 里没有表示，解析阶段就报错（坑 M7-18）。
 */
static u32 utf8_put(char *out, u32 cp)
{
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1u;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2u;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3u;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4u;
}

/* ---------------- 字符串 ---------------- */

/*
 * 解析一个字符串（当前字符必须是 '"'）。
 *
 * out / out_len 拿到**新分配**的字节（调用方负责 free）。
 * ⚠️ 字符串里**可以带 NUL 字节** ⇒ 长度必须带着走（坑 M7-15）。
 */
/*
 * 校验一个裸 UTF-8 序列的长度及码点边界。
 * 返回 0 表示非法，返回 1..4 表示合法序列的字节数。
 */
static u32 utf8_seq_len(const u8 *p, u32 remain)
{
    u8 b0;

    if (remain == 0u)
        return 0u;
    b0 = p[0];
    if (b0 <= 0x7Fu)
        return 1u;
    if (b0 >= 0xC2u && b0 <= 0xDFu) {
        if (remain < 2u || (p[1] & 0xC0u) != 0x80u)
            return 0u;
        return 2u;
    }
    if (b0 >= 0xE0u && b0 <= 0xEFu) {
        u8 b1;
        if (remain < 3u || (p[1] & 0xC0u) != 0x80u ||
            (p[2] & 0xC0u) != 0x80u)
            return 0u;
        b1 = p[1];
        if ((b0 == 0xE0u && b1 < 0xA0u) ||
            (b0 == 0xEDu && b1 > 0x9Fu))
            return 0u; /* overlong 或 UTF-8 surrogate */
        return 3u;
    }
    if (b0 >= 0xF0u && b0 <= 0xF4u) {
        u8 b1;
        if (remain < 4u || (p[1] & 0xC0u) != 0x80u ||
            (p[2] & 0xC0u) != 0x80u || (p[3] & 0xC0u) != 0x80u)
            return 0u;
        b1 = p[1];
        if ((b0 == 0xF0u && b1 < 0x90u) ||
            (b0 == 0xF4u && b1 > 0x8Fu))
            return 0u; /* overlong 或超出 U+10FFFF */
        return 4u;
    }
    return 0u;
}

static int parse_string(Parser *ps, char **out, u32 *out_len)
{
    char *buf;
    u32 cap;
    u32 n = 0;

    if (ps->pos >= ps->len || ps->p[ps->pos] != '"') {
        return E_INVAL;
    }
    ps->pos++;

    /*
     * 解码后的长度**不会超过**原文的长度
     * （转义只会变短：\n 两个字节 → 一个字节；代理对 12 字节 → 4 字节）⇒
     * 剩下的输入长度就是上界。
     */
    cap = ps->len - ps->pos;
    buf = (char *)malloc(cap ? (size_t)cap : 1u);
    if (!buf) {
        return E_NOSPACE;
    }

    for (;;) {
        char c;

        if (ps->pos >= ps->len) {
            free(buf);
            return E_INVAL;          /* 截断 */
        }
        c = ps->p[ps->pos];

        if (c == '"') {
            ps->pos++;
            break;
        }

        if (c == '\\') {
            char e;

            ps->pos++;
            if (ps->pos >= ps->len) {
                free(buf);
                return E_INVAL;      /* 截断 */
            }
            e = ps->p[ps->pos++];
            switch (e) {
            case '"':  buf[n++] = '"';  break;
            case '\\': buf[n++] = '\\'; break;
            case '/':  buf[n++] = '/';  break;
            case 'b':  buf[n++] = '\b'; break;
            case 'f':  buf[n++] = '\f'; break;
            case 'n':  buf[n++] = '\n'; break;
            case 'r':  buf[n++] = '\r'; break;
            case 't':  buf[n++] = '\t'; break;
            case 'u': {
                u32 cu;

                if (!read_hex4(ps, &cu)) {
                    free(buf);
                    return E_INVAL;
                }
                if (cu >= 0xD800u && cu <= 0xDBFFu) {
                    /*
                     * 高代理项：**必须**跟着一个低代理项
                     * （`\uD83D\uDE00` ⇒ U+1F600）⇒ 4 字节 UTF-8。
                     *
                     * ⚠️ 落单的高代理项 ⇒ 非法（坑 M7-18）：
                     *    它在 UTF-8 里没有表示，硬编会造出"看着像 emoji 的垃圾"。
                     */
                    u32 lo;
                    u32 cp;

                    if (ps->len - ps->pos < 2u ||
                        ps->p[ps->pos] != '\\' || ps->p[ps->pos + 1u] != 'u') {
                        free(buf);
                        return E_INVAL;
                    }
                    ps->pos += 2u;
                    if (!read_hex4(ps, &lo)) {
                        free(buf);
                        return E_INVAL;
                    }
                    if (lo < 0xDC00u || lo > 0xDFFFu) {
                        free(buf);
                        return E_INVAL;
                    }
                    cp = 0x10000u + ((cu - 0xD800u) << 10) + (lo - 0xDC00u);
                    n += utf8_put(buf + n, cp);
                } else if (cu >= 0xDC00u && cu <= 0xDFFFu) {
                    free(buf);
                    return E_INVAL;  /* 落单的低代理项 */
                } else {
                    n += utf8_put(buf + n, cu);
                }
                break;
            }
            default:
                free(buf);
                return E_INVAL;      /* 不认识的转义 */
            }
            continue;
        }

        if ((unsigned char)c < 0x20u) {
            /* 裸的控制字符：JSON 规定必须转义 */
            free(buf);
            return E_INVAL;
        }

        /* 裸 UTF-8：验证完整码点序列，再原样复制。 */
        {
            u32 seq_len = utf8_seq_len((const u8 *)ps->p + ps->pos,
                                       ps->len - ps->pos);
            if (seq_len == 0u) {
                free(buf);
                return E_INVAL;
            }
            memcpy(buf + n, ps->p + ps->pos, seq_len);
            n += seq_len;
            ps->pos += seq_len;
        }
    }

    *out = buf;
    *out_len = n;
    return OK;
}

/* ---------------- 数字 ---------------- */

/*
 * 解析一个数字。
 *
 * 语法（严格）：'-'? ( '0' | [1-9][0-9]* ) ( '.' [0-9]+ )? ( [eE] [+-]? [0-9]+ )?
 *
 * 归类：
 *   整数 → int32 / int64（放不下就 int64，再放不下退 FLOAT64）
 *   带 '.' 或 'e' → FLOAT64
 */
static int parse_number(Parser *ps, JsonbValue **out)
{
    u32 start = ps->pos;
    bool is_float = false;
    u32 litlen;
    char stackbuf[64];
    char *lit;
    JsonbValue *v = NULL;
    int rc = OK;

    if (ps->pos < ps->len && ps->p[ps->pos] == '-') {
        ps->pos++;
    }

    if (ps->pos >= ps->len || !is_digit(ps->p[ps->pos])) {
        return E_INVAL;              /* 减号后面必须跟数字 */
    }
    if (ps->p[ps->pos] == '0') {
        ps->pos++;                   /* '0' 后面不能再跟数字（前导零非法） */
    } else {
        while (ps->pos < ps->len && is_digit(ps->p[ps->pos])) {
            ps->pos++;
        }
    }

    if (ps->pos < ps->len && ps->p[ps->pos] == '.') {
        is_float = true;
        ps->pos++;
        if (ps->pos >= ps->len || !is_digit(ps->p[ps->pos])) {
            return E_INVAL;          /* 小数点后必须跟数字 */
        }
        while (ps->pos < ps->len && is_digit(ps->p[ps->pos])) {
            ps->pos++;
        }
    }

    if (ps->pos < ps->len &&
        (ps->p[ps->pos] == 'e' || ps->p[ps->pos] == 'E')) {
        is_float = true;
        ps->pos++;
        if (ps->pos < ps->len &&
            (ps->p[ps->pos] == '+' || ps->p[ps->pos] == '-')) {
            ps->pos++;
        }
        if (ps->pos >= ps->len || !is_digit(ps->p[ps->pos])) {
            return E_INVAL;          /* 指数位必须有数字 */
        }
        while (ps->pos < ps->len && is_digit(ps->p[ps->pos])) {
            ps->pos++;
        }
    }

    litlen = ps->pos - start;

    /* 字面量要 NUL 结尾才能交给 strtoll / strtod */
    if (litlen + 1u > sizeof(stackbuf)) {
        lit = (char *)malloc((size_t)litlen + 1u);
        if (!lit) {
            return E_NOSPACE;
        }
    } else {
        lit = stackbuf;
    }
    memcpy(lit, ps->p + start, litlen);
    lit[litlen] = '\0';

    if (!is_float) {
        long long sv;

        errno = 0;
        sv = strtoll(lit, NULL, 10);
        if (errno == ERANGE) {
            /* 超出 int64 ⇒ 退到 FLOAT64（**不报错**） */
            v = jsonb_new_double(strtod(lit, NULL));
        } else {
            v = jsonb_new_int((i64)sv);
        }
    } else {
        /*
         * ⚠️ strtod 溢出（1e999 ⇒ inf）**不报错** ——
         *     errno 是 ERANGE，但 SQLite 也接受这种输入。
         */
        v = jsonb_new_double(strtod(lit, NULL));
    }

    if (lit != stackbuf) {
        free(lit);
    }
    if (!v) {
        return E_NOSPACE;
    }
    *out = v;
    return rc;
}

/* ---------------- 布尔与 null ---------------- */

static int parse_literal(Parser *ps, JsonbValue **out)
{
    u32 left = ps->len - ps->pos;

    if (left >= 4u && memcmp(ps->p + ps->pos, "true", 4) == 0) {
        ps->pos += 4u;
        *out = jsonb_new_bool(true);
        return (*out) ? OK : E_NOSPACE;
    }
    if (left >= 5u && memcmp(ps->p + ps->pos, "false", 5) == 0) {
        ps->pos += 5u;
        *out = jsonb_new_bool(false);
        return (*out) ? OK : E_NOSPACE;
    }
    if (left >= 4u && memcmp(ps->p + ps->pos, "null", 4) == 0) {
        ps->pos += 4u;
        *out = jsonb_new_null();
        return (*out) ? OK : E_NOSPACE;
    }
    return E_INVAL;
}

/* ---------------- 对象 ---------------- */

/* parse_value 在下面才定义（对象/数组里要调它） */
static int parse_value(Parser *ps, JsonbValue **out);

/* 这个对象里已经有这个键了吗（字节相同） */
static bool obj_has_key(const JsonbValue *obj, const char *key, u32 klen)
{
    u32 i;

    for (i = 0; i < obj->u.obj.count; i++) {
        if (obj->u.obj.key_lens[i] == klen &&
            (klen == 0u || memcmp(obj->u.obj.keys[i], key, klen) == 0)) {
            return true;
        }
    }
    return false;
}

static int parse_object(Parser *ps, JsonbValue **out)
{
    JsonbValue *obj = jsonb_new_object();

    if (!obj) {
        return E_NOSPACE;
    }
    ps->pos++;                       /* '{' */

    skip_ws(ps);
    if (ps->pos < ps->len && ps->p[ps->pos] == '}') {
        ps->pos++;
        *out = obj;
        return OK;
    }

    for (;;) {
        char *key = NULL;
        u32 klen = 0;
        JsonbValue *val = NULL;
        int rc;

        skip_ws(ps);
        rc = parse_string(ps, &key, &klen);
        if (rc != OK) {
            jsonb_free(obj);
            return rc;
        }

        skip_ws(ps);
        if (ps->pos >= ps->len || ps->p[ps->pos] != ':') {
            free(key);
            jsonb_free(obj);
            return E_INVAL;
        }
        ps->pos++;

        rc = parse_value(ps, &val);
        if (rc != OK) {
            free(key);
            jsonb_free(obj);
            return rc;
        }

        if (obj_has_key(obj, key, klen)) {
            /*
             * 重复键 ⇒ **保留首个**（坑 M7-17）：
             * 后出现的同名字段连同它的值一起丢掉。
             *
             * ⚠️ 注意值**照样完整解析过**了 ——
             *    `{"a":1,"a":}` 必须报错，不能因为"反正要丢"就跳过语法检查。
             */
            jsonb_free(val);
        } else {
            rc = jsonb_obj_set(obj, key, klen, val);
            if (rc != OK) {
                jsonb_free(val);
                free(key);
                jsonb_free(obj);
                return rc;
            }
        }
        free(key);

        skip_ws(ps);
        if (ps->pos < ps->len && ps->p[ps->pos] == ',') {
            ps->pos++;
            continue;                /* 下一个成员 */
        }
        if (ps->pos < ps->len && ps->p[ps->pos] == '}') {
            ps->pos++;
            break;
        }
        jsonb_free(obj);
        return E_INVAL;              /* 缺 ',' 或 '}'（尾逗号也走这里） */
    }

    *out = obj;
    return OK;
}

/* ---------------- 数组 ---------------- */

static int parse_array(Parser *ps, JsonbValue **out)
{
    JsonbValue *arr = jsonb_new_array();

    if (!arr) {
        return E_NOSPACE;
    }
    ps->pos++;                       /* '[' */

    skip_ws(ps);
    if (ps->pos < ps->len && ps->p[ps->pos] == ']') {
        ps->pos++;
        *out = arr;
        return OK;
    }

    for (;;) {
        JsonbValue *item = NULL;
        int rc = parse_value(ps, &item);

        if (rc != OK) {
            jsonb_free(arr);
            return rc;
        }
        rc = jsonb_arr_push(arr, item);
        if (rc != OK) {
            jsonb_free(item);
            jsonb_free(arr);
            return rc;
        }

        skip_ws(ps);
        if (ps->pos < ps->len && ps->p[ps->pos] == ',') {
            ps->pos++;
            continue;
        }
        if (ps->pos < ps->len && ps->p[ps->pos] == ']') {
            ps->pos++;
            break;
        }
        jsonb_free(arr);
        return E_INVAL;              /* 缺 ',' 或 ']'（尾逗号也走这里） */
    }

    *out = arr;
    return OK;
}

/* ---------------- 值 ---------------- */

static int parse_value(Parser *ps, JsonbValue **out)
{
    char c;
    int rc;

    if (ps->depth >= (u32)JSON_PARSE_MAX_DEPTH) {
        return E_INVAL;              /* 太深：报错，不是崩 */
    }
    skip_ws(ps);
    if (ps->pos >= ps->len) {
        return E_INVAL;              /* 截断 */
    }

    c = ps->p[ps->pos];
    ps->depth++;
    switch (c) {
    case '{':
        rc = parse_object(ps, out);
        break;
    case '[':
        rc = parse_array(ps, out);
        break;
    case '"': {
        char *s = NULL;
        u32 slen = 0;

        rc = parse_string(ps, &s, &slen);
        if (rc == OK) {
            *out = jsonb_new_string(s, slen);
            free(s);
            if (!*out) {
                rc = E_NOSPACE;
            }
        }
        break;
    }
    case 't':
    case 'f':
    case 'n':
        rc = parse_literal(ps, out);
        break;
    case '-':
        rc = parse_number(ps, out);
        break;
    default:
        if (is_digit(c)) {
            rc = parse_number(ps, out);
        } else {
            rc = E_INVAL;            /* 不认识的字符 */
        }
        break;
    }
    ps->depth--;
    return rc;
}

/* ---------------- 入口 ---------------- */

int json_parse(const char *text, u32 len, JsonbValue **out)
{
    Parser ps;
    JsonbValue *tree = NULL;
    int rc;

    if (!out) {
        return E_INVAL;
    }
    *out = NULL;
    if (!text) {
        return E_INVAL;
    }

    ps.p = text;
    ps.len = len;
    ps.pos = 0;
    ps.depth = 0;

    rc = parse_value(&ps, &tree);
    if (rc != OK) {
        return rc;
    }

    /* 后面还有东西 ⇒ 非法（"{} x" 走这里） */
    skip_ws(&ps);
    if (ps.pos != ps.len) {
        jsonb_free(tree);
        return E_INVAL;
    }

    *out = tree;
    return OK;
}

int json_parse_str(const char *text, JsonbValue **out)
{
    if (!text) {
        if (out) {
            *out = NULL;
        }
        return E_INVAL;
    }
    return json_parse(text, (u32)strlen(text), out);
}

int json_parse_to_jsonb(const char *text, u32 len, u8 *out, u32 cap,
                        u32 *out_len)
{
    JsonbValue *tree = NULL;
    int rc;

    if (!out_len) {
        return E_INVAL;
    }
    *out_len = 0;

    rc = json_parse(text, len, &tree);
    if (rc != OK) {
        return rc;
    }
    rc = jsonb_encode(tree, out, cap, out_len);
    jsonb_free(tree);
    return rc;
}
