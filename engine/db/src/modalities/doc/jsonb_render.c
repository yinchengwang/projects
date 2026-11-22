/*
 * jsonb_render.c —— JSONB 字节流 → JSON 文本（v1.6 C5.3）
 *
 * ==================== 为什么单独一个文件 ====================
 *
 * `jsonb.c` 已经 764 行（编码 + 解码 + 树的构造/释放），再加渲染就超
 * 800 行的硬上限。而"渲染"与"编码/解码"是两件事：
 *   - 编码/解码管的是**二进制布局**（怎么存）；
 *   - 渲染管的是**对外输出形态**（怎么给人看）。
 * 分开之后两边都不用为了对方的细节读整份文件。
 *
 * ==================== 与 jsonb_decode 的关系 ====================
 *
 * ⭐ **本文件不建树**：直接在字节流上递归往前走。
 * 建树再渲染会多一次完整的内存分配往返，而大文档正是本模态要处理的
 * 主要对象 —— 能省的那一份开销就省掉。
 *
 * 这依赖 `jsonb.h` 的写法约定（[tag][payload]，长度前缀全在字节流里），
 * 也就是"解码器能顺序读出来的东西，这里就能顺序写出来"。
 *
 * ==================== 四个必须守住的语义 ====================
 *
 * ① **整数不许变成浮点**：INT64 就是整数字面量。
 *    这是 C5.3 的核心之一 —— 9223372036854775807 一旦走了
 *    `%.17g` 就会变成 `9.2233720368547758e+18`，静默改值。
 *
 * ② **float64 用最短往返表示**：`%.17g` 足够保证 double 往返无损，
 *    而 JSON 里 `1.5` 不能渲染成 `1.5000000000000000`（能解析，
 *    但与输入不一致，A1「往返全等」验收会失败）。
 *    做法：先按 `%.17g` 打，再把尾随的零去掉（不动指数部分）。
 *
 * ③ **字符串必须转义**：引号、反斜杠、以及所有 < 0x20 的控制字符。
 *    其中常见的几个用短形式（\n \t \r \b \f），其余用 \u00XX。
 *    **非 ASCII 字节（UTF-8）直接输出**，不转 \uXXXX ——
 *    否则中文文档读出来全是 `\u4e2d\u6587`，与输入不一致。
 *
 * ④ **缓冲不足 ⇒ E_NOSPACE 且一个字节都不写**。
 *    先量长度（out == NULL 时走这一趟），确认装得下才写。
 *    "写半个文本"比"报错"危险 —— 半个 JSON 看起来像合法 JSON。
 */

#include "modalities/doc/jsonb.h"

#include <stdio.h>
#include <string.h>

/*
 * 渲染时允许的最大嵌套深度。
 *
 * 与 jsonb.c 的 JSONB_MAX_DEPTH 同值（200）——两边不一致就会出现
 * "编码得进去、渲染不出来"的荒唐情况。这里独立定义是为了不把
 * jsonb.c 的内部宏搬进头文件（它是实现细节，不是契约）。
 */
#define RENDER_MAX_DEPTH 200u

/* 小端读，与 jsonb.c 的解码口径一致 */
static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 rd64(const u8 *p)
{
    return (u64)rd32(p) | ((u64)rd32(p + 4) << 32);
}

/*
 * 输出缓冲的游标。
 *
 * 【为什么用一个结构而不是 (buf, cap, off) 三个参数到处传】
 * 递归调用点有八处，三参数版每处都要写三个实参、每次都要判界。
 * 一个游标结构把"写一个字节 / 写一段字节"收成两个函数，
 * 判界只在这两个地方做 —— 漏判界的机会只有两处，不是二十处。
 */
typedef struct {
    char *buf;      /* 可为 NULL：只量长度 */
    u32   cap;
    u32   off;
} Sink;

static int sink_put(Sink *s, const char *p, u32 n)
{
    /*
     * 量长度那一趟（buf == NULL）**永不失败** —— 只累计字节数。
     *
     * ⚠️ 这一点曾经写错：如果这里也对 NULL 缓冲判界，量长度的第一趟
     * 会在第一个字符就返回 E_NOSPACE，调用方拿到的"需要长度"是 0，
     * 于是所有渲染都失败。量长度本来就不该有容量概念。
     */
    if (!s->buf) {
        s->off += n;
        return OK;
    }
    if (s->off > s->cap || n > s->cap - s->off)
        return E_NOSPACE;
    if (n)
        memcpy(s->buf + s->off, p, n);
    s->off += n;
    return OK;
}

static int sink_putc(Sink *s, char c)
{
    return sink_put(s, &c, 1u);
}

static int sink_puts(Sink *s, const char *str)
{
    return sink_put(s, str, (u32)strlen(str));
}

/*
 * 渲染一个字符串（含外层引号）。
 *
 * ⚠️ 转义表只覆盖 JSON 要求的六个短形式；其余控制字符用 \u00XX。
 * 0x7F（DEL）也按控制字符处理 —— 它不是必经路径，但不该原样输出。
 */
static int render_string(Sink *s, const char *p, u32 n)
{
    int rc = sink_putc(s, '"');
    if (rc != OK)
        return rc;

    for (u32 i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        switch (c) {
        case '"':  rc = sink_puts(s, "\\\""); break;
        case '\\': rc = sink_puts(s, "\\\\"); break;
        case '\b': rc = sink_puts(s, "\\b");  break;
        case '\f': rc = sink_puts(s, "\\f");  break;
        case '\n': rc = sink_puts(s, "\\n");  break;
        case '\r': rc = sink_puts(s, "\\r");  break;
        case '\t': rc = sink_puts(s, "\\t");  break;
        default:
            if (c < 0x20u || c == 0x7Fu) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
                rc = sink_put(s, esc, 6u);
            } else {
                rc = sink_putc(s, (char)c);
            }
            break;
        }
        if (rc != OK)
            return rc;
    }
    return sink_putc(s, '"');
}

/*
 * 渲染一个 double。
 *
 * 【为什么不是直接 %.17g】
 * `%.17g` 对 1.5 给出 `1.5`（好），但对 1.0 给出 `1`（坏 ——
 * 那会变成整数字面量，往返回 JSONB 时 tag 从 FLOAT64 变成 INT32）。
 * 所以：
 *   - 没有小数点、没有指数的，补一个 ".0"；
 *   - 其余按 %.17g 打出来（tail 零不会出现，%g 会自行去除）。
 */
static int render_double(Sink *s, double d)
{
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%.17g", d);

    if (n <= 0 || n >= (int)sizeof(tmp))
        return E_CORRUPT;

    bool has_dot = false;
    for (int i = 0; i < n; i++) {
        if (tmp[i] == '.' || tmp[i] == 'e' || tmp[i] == 'E' ||
            tmp[i] == 'n' || tmp[i] == 'i') {   /* nan / inf */
            has_dot = true;
            break;
        }
    }
    if (!has_dot) {
        if (n + 2 < (int)sizeof(tmp)) {
            tmp[n++] = '.';
            tmp[n++] = '0';
            tmp[n] = '\0';
        }
    }
    return sink_put(s, tmp, (u32)n);
}

/* 前向声明：容器递归 */
static int render_value(Sink *s, const u8 *buf, u32 len, u32 *pos, u32 depth);

/* 渲染一个容器里的成员值（array / object 共用） */
static int render_container(Sink *s, const u8 *buf, u32 len, u32 *pos,
                            u32 count, bool is_object, u32 depth)
{
    int rc = sink_putc(s, is_object ? '{' : '[');
    if (rc != OK)
        return rc;

    for (u32 i = 0; i < count; i++) {
        if (i > 0) {
            rc = sink_putc(s, ',');
            if (rc != OK)
                return rc;
        }

        if (is_object) {
            u32 klen;
            if (*pos > len || len - *pos < 4u)
                return E_CORRUPT;
            klen = rd32(buf + *pos);
            *pos += 4u;
            if (*pos > len || len - *pos < klen)
                return E_CORRUPT;
            rc = render_string(s, (const char *)(buf + *pos), klen);
            if (rc != OK)
                return rc;
            *pos += klen;
            rc = sink_putc(s, ':');
            if (rc != OK)
                return rc;
        }

        rc = render_value(s, buf, len, pos, depth + 1u);
        if (rc != OK)
            return rc;
    }

    return sink_putc(s, is_object ? '}' : ']');
}

static int render_value(Sink *s, const u8 *buf, u32 len, u32 *pos, u32 depth)
{
    u8 tag;

    if (depth > RENDER_MAX_DEPTH)
        return E_CORRUPT;
    if (*pos >= len)
        return E_CORRUPT;

    tag = buf[*pos];
    (*pos)++;

    switch (tag) {
    case JSONB_NULL:
        return sink_puts(s, "null");
    case JSONB_TRUE:
        return sink_puts(s, "true");
    case JSONB_FALSE:
        return sink_puts(s, "false");

    case JSONB_INT32: {
        if (len - *pos < 4u)
            return E_CORRUPT;
        i32 x = (i32)rd32(buf + *pos);
        *pos += 4u;
        {
            char tmp[24];
            int n = snprintf(tmp, sizeof(tmp), "%ld", (long)x);
            if (n <= 0 || n >= (int)sizeof(tmp))
                return E_CORRUPT;
            return sink_put(s, tmp, (u32)n);
        }
    }

    case JSONB_INT64: {
        if (len - *pos < 8u)
            return E_CORRUPT;
        u64 bits = rd64(buf + *pos);
        i64 x;
        memcpy(&x, &bits, sizeof(x));
        *pos += 8u;
        {
            char tmp[32];
            int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)x);
            if (n <= 0 || n >= (int)sizeof(tmp))
                return E_CORRUPT;
            return sink_put(s, tmp, (u32)n);
        }
    }

    case JSONB_FLOAT64: {
        if (len - *pos < 8u)
            return E_CORRUPT;
        u64 bits = rd64(buf + *pos);
        double d;
        memcpy(&d, &bits, sizeof(d));
        *pos += 8u;
        return render_double(s, d);
    }

    case JSONB_STRING: {
        u32 slen;
        if (len - *pos < 4u)
            return E_CORRUPT;
        slen = rd32(buf + *pos);
        *pos += 4u;
        if (len - *pos < slen)
            return E_CORRUPT;
        {
            int rc = render_string(s, (const char *)(buf + *pos), slen);
            *pos += slen;
            return rc;
        }
    }

    case JSONB_ARRAY:
    case JSONB_OBJECT: {
        u32 count;
        if (len - *pos < 4u)
            return E_CORRUPT;
        count = rd32(buf + *pos);
        *pos += 4u;
        return render_container(s, buf, len, pos, count,
                                tag == JSONB_OBJECT, depth);
    }

    default:
        return E_CORRUPT;
    }
}

int jsonb_render(const u8 *buf, u32 len, char *out, u32 cap, u32 *out_len)
{
    Sink s;
    u32 pos = 0;
    u32 need = 0;
    int rc;

    if (!buf || !out_len)
        return E_INVAL;

    /*
     * 第一趟：量长度。永远成功（缓冲 NULL 时不判界）。
     *
     * ⚠️ 为什么要先量而不是直接写：
     * 契约承诺「缓冲不足时一个字节都不写」—— 半个 JSON 看起来像合法 JSON，
     * 比明确报错危险。直接写就只能在写坏之后才发现装不下。
     * 代价是多走一遍字节流（只算不加锁、不分配），换来的是这条承诺成立。
     */
    s.buf = NULL;
    s.cap = 0u;
    s.off = 0u;
    rc = render_value(&s, buf, len, &pos, 0u);
    if (rc != OK)
        return rc;
    if (pos != len)
        return E_CORRUPT;      /* 尾部多余字节 ⇒ 这条 JSONB 是坏的 */
    need = s.off;

    *out_len = need;

    if (!out)
        return E_NOSPACE;      /* 与 DetoastFn 同一口径：只报长度 */
    if (need > cap)
        return E_NOSPACE;      /* 装不下 ⇒ 报出来，一个字节都没写 */

    /* 第二趟：真正写。此时能保证装得下。 */
    pos = 0;
    s.buf = out;
    s.cap = cap;
    s.off = 0u;
    rc = render_value(&s, buf, len, &pos, 0u);
    if (rc != OK)
        return rc;
    if (pos != len || s.off != need)
        return E_CORRUPT;      /* 两趟不一致 ⇒ 内部 bug，明确报错 */

    return OK;
}
