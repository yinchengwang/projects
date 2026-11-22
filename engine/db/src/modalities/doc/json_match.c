/*
 * json_match.c —— 匹配规格解析与求值（v1.6 C5.7）
 *
 * ==================== 这一层负责什么 ====================
 *
 * `json_match(doc, '$.a.b = 5')` 里的第二个实参是一条**规格字符串**。
 * 本文件把这条字符串解析成 `JsonMatchSpec`，并在 JSONB 上求值。
 *
 * 路径解析、标量映射、标量比较都**不在这里** —— 它们是
 * `json_path.c` / `json_scalar.c` 的事。本文件只做"把规格拼起来"：
 *
 *     规格字符串  →  (路径, 比较符, 字面量)  →  在 JSONB 上求值
 *
 * 这与判决 1 的纪律一致：**一个语义只能有一个主人**。
 * 提取值的那套映射只在 json_scalar.c，这里调它，不重写。
 *
 * ==================== 规格语法（故意极简） ====================
 *
 *      <路径> <比较符> <字面量>
 *
 * 例子：
 *      $.a.b = 5
 *      $.name != "x"
 *      $.age >= 18
 *      $.ok = true
 *      $.note = null
 *
 * 比较符：=  !=  <>  <  <=  >  >=
 * 字面量：整数 / 小数 / 单引号或双引号字符串 / true / false / null
 *
 * ⚠️ **不支持 AND / OR / 括号 / 函数**。
 * 组合留给 SQL 层：`WHERE json_match(d,'$.a=1') AND json_match(d,'$.b=2')`
 * 与"规格里写 AND"语义相同，但前者由 SQL 的布尔层负责，不需要在
 * 这里实现一套表达式求值器 —— 那是把 SQL 重写一遍。
 *
 * ⚠️ **路径必须是规格的第一个 token**，且必须以 `$` 开头。
 * 不以 `$` 开头 ⇒ E_INVAL（不猜）。
 *
 * ==================== 求值的三值逻辑 ====================
 *
 * `json_match_eval` 的 out 只有 true / false 两种结果，但它内部遵守
 * SQL 的三值逻辑：
 *   - 路径不存在 / 值是 JSON null / 类型不匹配  ⇒ 比较结果是"未知"
 *   - "未知" 参与 `=` / `!=` / 大小比较  ⇒ **不是 true**
 * ⇒ 对外返回 false（这一行不匹配），而不是报错。
 *
 * 为什么不是"返回错误把查询打断"：判据里"这个文档没有 $.a"
 * 的正确答案是"这一行不满足条件"，不是"整条查询失败"。
 *
 * ==================== 与索引的关系 ====================
 *
 * 路径索引按 `json_extract_scalar` 建的键，执行器重判 WHERE 时用的是
 * **同一个** `json_extract_scalar`（经本文件调）。两边共用一份实现
 * ⇒ 不可能语义分叉（判决 1 的核心承诺）。
 */

#include "modalities/doc/json_match.h"

#include <stdlib.h>
#include <string.h>

/* 规格字符串的长度上限（超过就不解析 —— 防跑飞） */
#define MATCH_SPEC_MAX 256

/* ---------------- 小工具 ---------------- */

static bool is_space(char c)
{
    return c == ' ' || c == '\t';
}

/* 跳过前导空白，返回新的位置 */
static const char *skip_ws(const char *p)
{
    while (*p && is_space(*p))
        p++;
    return p;
}

/*
 * 读一个路径 token。
 *
 * 从 `$` 开始，一直到空白或比较符为止。
 * 路径里不出现空白是硬约定（JSONPath 本身也不含空白）。
 */
static int read_path(const char **pp, JsonPath *out)
{
    const char *p = skip_ws(*pp);
    const char *start;
    char buf[JSONP_MAX_STEPS * 8 + 8];   /* 路径文本缓冲 */
    size_t n;

    if (*p != '$')
        return E_INVAL;
    start = p;

    while (*p && !is_space(*p) &&
           *p != '=' && *p != '!' && *p != '<' && *p != '>')
        p++;

    n = (size_t)(p - start);
    if (n == 0 || n >= sizeof(buf))
        return E_INVAL;

    memcpy(buf, start, n);
    buf[n] = '\0';

    *pp = p;
    return json_path_parse(buf, out, NULL, 0);
}

/* 读比较符 */
static int read_op(const char **pp, int *op_out)
{
    const char *p = skip_ws(*pp);

    if (p[0] == '=') {
        *op_out = JSON_OP_EQ;
        *pp = p + 1;
        return OK;
    }
    if (p[0] == '!' && p[1] == '=') {
        *op_out = JSON_OP_NE;
        *pp = p + 2;
        return OK;
    }
    if (p[0] == '<' && p[1] == '>') {
        *op_out = JSON_OP_NE;
        *pp = p + 2;
        return OK;
    }
    if (p[0] == '<' && p[1] == '=') {
        *op_out = JSON_OP_LE;
        *pp = p + 2;
        return OK;
    }
    if (p[0] == '>' && p[1] == '=') {
        *op_out = JSON_OP_GE;
        *pp = p + 2;
        return OK;
    }
    if (p[0] == '<') {
        *op_out = JSON_OP_LT;
        *pp = p + 1;
        return OK;
    }
    if (p[0] == '>') {
        *op_out = JSON_OP_GT;
        *pp = p + 1;
        return OK;
    }
    return E_INVAL;
}

/*
 * 读一个字面量。
 *
 * 支持：整数、小数、单/双引号字符串、true / false / null。
 *
 * ⚠️ **字符串字面量不做转义还原**（规格是给机器拼的，不是给手写的）。
 *    要匹配含引号的值，用 json_extract + 比较，别用 json_match。
 *    这个限制写进契约，免得有人以为支持 `\"`。
 */
static int read_literal(const char **pp, DbValue *out)
{
    const char *p = skip_ws(*pp);

    if (*p == '"' || *p == '\'') {
        char q = *p++;
        const char *s = p;
        while (*p && *p != q)
            p++;
        if (*p != q)
            return E_INVAL;
        val_text(out, s, (u16)(p - s));
        *pp = p + 1;
        return OK;
    }

    if (strncmp(p, "true", 4) == 0 && !p[4]) {
        val_int(out, 1);
        *pp = p + 4;
        return OK;
    }
    if (strncmp(p, "false", 5) == 0 && !p[5]) {
        val_int(out, 0);
        *pp = p + 5;
        return OK;
    }
    if (strncmp(p, "null", 4) == 0 && !p[4]) {
        val_null(out);
        *pp = p + 4;
        return OK;
    }

    /* 数字：走 strtod，整数与小数都能认（REAL 用 d，整数用 i） */
    {
        char *end = NULL;
        bool is_real = false;
        const char *scan;

        for (scan = p; *scan; scan++) {
            if (*scan == '.' || *scan == 'e' || *scan == 'E') {
                is_real = true;
                break;
            }
        }
        if (!is_real) {
            long long v = strtoll(p, &end, 10);
            if (end == p)
                return E_INVAL;
            val_int(out, (i64)v);
            *pp = end;
            return OK;
        }
        {
            double d = strtod(p, &end);
            if (end == p)
                return E_INVAL;
            val_real(out, d);
            *pp = end;
            return OK;
        }
    }
}

/* ---------------- 契约实现 ---------------- */

int json_match_parse(const char *text, JsonMatchSpec *out)
{
    const char *p;
    int rc;

    if (!text || !out)
        return E_INVAL;
    if (strlen(text) >= MATCH_SPEC_MAX)
        return E_INVAL;

    memset(out, 0, sizeof(*out));
    p = text;

    rc = read_path(&p, &out->path);
    if (rc != OK)
        return rc;

    rc = read_op(&p, &out->op);
    if (rc != OK)
        return rc;

    rc = read_literal(&p, &out->lit);
    if (rc != OK)
        return rc;

    /* 字面量之后只允许空白 */
    p = skip_ws(p);
    if (*p != '\0')
        return E_INVAL;

    return OK;
}

/*
 * 在 JSONB 上求值一条规格。
 *
 * 【三值逻辑怎么落地】
 * `cmp` 先得到"路径值 vs 字面量"的比较结果；若任一边是 NULL
 * （JSON null / 路径不存在 / 类型不匹配），`is_null` 为 true ⇒
 * 只有 `!=` 之外的结果都是 false。
 *
 * ⚠️ SQL 里 NULL != 5 的结果是**未知**（不是 true）。所以
 *    任一边 NULL 时，**所有**比较符都给 false —— 包括 `!=`。
 *    把 NULL != x 当成 true 是经典的三值逻辑错误。
 */
int json_match_eval(const u8 *jsonb, u16 len, const JsonMatchSpec *spec,
                    bool *out)
{
    DbValue got;
    bool is_null = false;
    int cerr = OK;
    int c;

    if (!spec || !out)
        return E_INVAL;
    *out = false;

    int rc = json_extract_scalar(jsonb, (u32)len, &spec->path, &got);
    if (rc == E_NOTFOUND) {
        /*
         * 路径不存在 ⇒ 这一行不匹配（不是错误）。
         * json_extract_scalar 已经把 got 填成 NULL，但"没有路径"
         * 与"值是 null"在这一层行为相同 —— 都让比较落到"未知"。
         */
        return OK;
    }
    if (rc != OK)
        return rc;

    if (spec->lit.is_null || got.is_null) {
        /*
         * 任一边是 NULL ⇒ 三值逻辑的"未知" ⇒ 所有比较都给 false。
         *
         * ⚠️ 这里**不能**对 JSON_OP_NE 破例给 true。
         *    `NULL != 5` 的答案是"不知道"，不是"是"。
         */
        is_null = true;
        return OK;
    }

    c = val_cmp(&got, &spec->lit, &is_null, &cerr);
    if (cerr != OK)
        return cerr;
    if (is_null)
        return OK;

    switch (spec->op) {
    case JSON_OP_EQ: *out = (c == 0); break;
    case JSON_OP_NE: *out = (c != 0); break;
    case JSON_OP_LT: *out = (c < 0);  break;
    case JSON_OP_LE: *out = (c <= 0); break;
    case JSON_OP_GT: *out = (c > 0);  break;
    case JSON_OP_GE: *out = (c >= 0); break;
    default: return E_INVAL;
    }
    return OK;
}
