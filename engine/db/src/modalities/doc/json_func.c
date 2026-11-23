/*
 * json_func.c —— 文档模态向通用值层注册的函数（v1.6 C5.7）
 *
 * ==================== 这里只做三件"接线"的事 ====================
 *
 *   1. `json_extract(doc, path)`  → 调 `json_extract_scalar`（json_scalar.c）
 *   2. `json_match(doc, spec)`    → 调 `json_match_parse` + `json_match_eval`
 *   3. 注册 `jsonb_render`        → 给值层输出 T_JSON 用（C5.3 的钩子）
 *
 * ⚠️ **一行语义都不在这里**。路径解析、标量映射、比较规则、
 *    规格解析全都在各自的文件里 —— 本文件只负责"名字 → 实现"的绑定。
 *    这是判决 1 的纪律（一个语义只能有一个主人）在**接线层**的体现。
 *
 * ==================== 为什么要处理外部 JSONB ====================
 *
 * 执行器传进来的文档有两种形态（见 storage/value.h 的 T_JSON 说明）：
 *   - 内联：JSONB 字节就在 `DbValue.s[]`；
 *   - 外部：`ext_fn` 指向记录缓冲或页链，要先 `val_detoast` 取出来。
 *
 * 两个函数都必须先做这一步 —— 只处理内联的话，256 字节以上的文档
 * 就会静默查不到路径（症状：小文档能查、大文档查不到）。
 *
 * ⚠️ `val_detoast` 分配的缓冲由本文件 free。
 */

#include "modalities/doc/json_func.h"

#include <stdlib.h>
#include <string.h>

#include "modalities/doc/json_match.h"
#include "modalities/doc/json_ovf.h"
#include "modalities/doc/json_parse.h"
#include "modalities/doc/json_path.h"
#include "storage/value.h"

/* 提取的第一个实参：把 DbValue 拿成 (JSONB 字节, 长度)，必要时物化 */
typedef struct {
    const u8 *bytes;
    u32       len;
    u8       *owned;    /* 非 NULL 表示需要 free */
} JsonBytes;

static int take_json(const DbValue *v, JsonBytes *out)
{
    out->bytes = NULL;
    out->len = 0;
    out->owned = NULL;

    if (!v || v->is_null)
        return E_INVAL;

    if (v->external) {
        u8 *buf = NULL;
        u32 len = 0;
        int rc = val_detoast(v, &buf, &len);
        if (rc != OK)
            return rc;
        out->bytes = buf;
        out->len = len;
        out->owned = buf;
        return OK;
    }

    out->bytes = (const u8 *)v->s;
    out->len = v->len;
    return OK;
}

static void release_json(JsonBytes *b)
{
    free(b->owned);
    b->owned = NULL;
}

/*
 * json_extract(doc, '$.a.b')
 *
 * 路径解析失败 ⇒ E_INVAL；路径不存在 ⇒ OK + DbValue(NULL)
 * （三值逻辑：问"这个路径有什么值"，答案是"没有"，不是"查询失败"）。
 */
static int fn_json_extract(const DbValue *args, int nargs, DbValue *out)
{
    JsonPath path;
    JsonBytes doc;
    int rc;

    if (nargs != 2 || !out)
        return E_INVAL;

    /*
     * SQL 三值逻辑：实参为 NULL ⇒ 结果就是 NULL，不是错误。
     *
     * 【为什么必须这样】
     * JSON 列是可空的。`SELECT json_extract(doc, '$.a') FROM t`
     * 一旦扫到 NULL 行就报 E_INVAL 的话，**整条查询都失败** ——
     * 用户只是有一行没填文档而已。
     * 与 SQLite 一致：标量函数对 NULL 输入返回 NULL。
     */
    if (args[0].is_null || args[1].is_null || args[1].len == 0) {
        val_null(out);
        return OK;
    }

    {
        char expr[256];
        u16 n = args[1].len;
        if (n >= sizeof(expr))
            return E_INVAL;
        memcpy(expr, args[1].s, n);
        expr[n] = '\0';

        rc = json_path_parse(expr, &path, NULL, 0);
        if (rc != OK)
            return rc;
    }

    rc = take_json(&args[0], &doc);
    if (rc != OK)
        return rc;

    rc = json_extract_scalar(doc.bytes, doc.len, &path, out);
    release_json(&doc);

    /*
     * 路径不存在：json_extract_scalar 已经把 out 填成 NULL，
     * 但返回 E_NOTFOUND。对外它**不是错误** —— 归一成 OK。
     */
    if (rc == E_NOTFOUND)
        return OK;
    return rc;
}

/*
 * json_match(doc, '$.a = 5')
 *
 * 返回 1 / 0 的 INT 值（不是 bool，因为 SQL 的谓词层用数值真值）。
 */
static int fn_json_match(const DbValue *args, int nargs, DbValue *out)
{
    JsonMatchSpec spec;
    JsonBytes doc;
    bool matched = false;
    int rc;

    if (nargs != 2 || !out)
        return E_INVAL;
    if (args[0].is_null || args[1].is_null || args[1].len == 0) {
        val_int(out, 0);          /* NULL 文档 ⇒ 不匹配（谓词层只看 0/1） */
        return OK;
    }

    {
        char text[256];
        u16 n = args[1].len;
        if (n >= sizeof(text))
            return E_INVAL;
        memcpy(text, args[1].s, n);
        text[n] = '\0';

        rc = json_match_parse(text, &spec);
        if (rc != OK)
            return rc;
    }

    rc = take_json(&args[0], &doc);
    if (rc != OK)
        return rc;

    rc = json_match_eval(doc.bytes, (u16)doc.len, &spec, &matched);
    release_json(&doc);
    if (rc != OK)
        return rc;

    val_int(out, matched ? 1 : 0);
    return OK;
}

/*
 * 分配式 JSON 编码器（值层注册表里的一项）。
 *
 * 【为什么自己分配】
 * "编码后多大算合规"是**文档模态**的知识：JSON_INLINE_MAX（2048，
 * 见 json_ovf.h）。值层只提供机制，不认识这个数字 ——
 * 若让调用方给固定缓冲，2048 就被写死在 query 层，违反 A7 审计。
 *
 * 【为什么超过上限就报错】
 * >2048 字节的文档需要 PT_JSON_OVF 页链，而页链的 tuple 所有权、
 * abort/WAL 恢复、VACUUM 回收尚未设计（C5.3b-b）。
 * 现在放行只会造出没有主的页 ⇒ 明确拒绝。
 *
 * 【两趟】
 * 第一趟 json_parse_to_jsonb(NULL, 0) 只算长度；
 * 超上限就直接报错（连缓冲都不分配）；
 * 第二趟按精确长度 malloc 并真正编码。
 */
static int fn_json_encode_alloc(const char *text, u32 text_len,
                                u8 **bytes_out, u32 *len_out)
{
    u32 need = 0;
    u8 *buf = NULL;
    int rc;

    if (!text || !bytes_out || !len_out)
        return E_INVAL;
    *bytes_out = NULL;
    *len_out = 0;

    /* 第一趟：量长度（out == NULL 时只算长度） */
    rc = json_parse_to_jsonb(text, text_len, NULL, 0, &need);
    if (rc != OK)
        return rc;

    if (need > JSON_INLINE_MAX)
        return E_NOSPACE;

    buf = malloc(need ? need : 1u);
    if (!buf)
        return E_IO;

    /* 第二趟：真正编码 */
    rc = json_parse_to_jsonb(text, text_len, buf, need, len_out);
    if (rc != OK) {
        free(buf);
        *len_out = 0;
        return rc;
    }
    *bytes_out = buf;
    return OK;
}

int db_json_init(void)
{
    int rc;
    int worst = OK;

    /*
     * 三个注册都可能返回 E_INVAL（已注册）。
     * 那是"别的连接已经注册过"，不是失败 —— 多连接场景每个连接
     * 都会走一次 db_open，不能因此让开库失败。
     */
    rc = val_func_register("json_extract", fn_json_extract);
    if (rc != OK && rc != E_INVAL)
        worst = rc;

    rc = val_func_register("json_match", fn_json_match);
    if (rc != OK && rc != E_INVAL)
        worst = rc;

    rc = val_set_json_renderer(jsonb_render);
    if (rc != OK && rc != E_INVAL)
        worst = rc;

    rc = val_set_json_encoder(fn_json_encode_alloc);
    if (rc != OK && rc != E_INVAL)
        worst = rc;

    return worst;
}
