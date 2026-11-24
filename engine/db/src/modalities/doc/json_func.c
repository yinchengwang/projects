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

    /*
     * 上限从 JSON_INLINE_MAX 放到 JSON_OVF_MAX_LEN。
     *
     * 【为什么必须放】
     * 原来这里卡在 2048：>2048 直接 E_NOSPACE ⇒ 页链那条路**永远走不到**。
     * 页链接进来之后，2048 不再是"能存多大"的边界，而是
     * "内联还是走页链"的分界 —— 判断它的地方在写路径（json_should_ovf），
     * 不在编码器。
     *
     * 编码器只负责"文本 → JSONB 字节"，真正的存放方式由调用方决定。
     * 仍保留 65535 上限：那是 DetoastFn 的长度出口决定的物理上限，
     * 超过就根本读不回来。
     */
    if (need > JSON_OVF_MAX_LEN)
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

/*
 * 页链归还适配器（值层注册表里的一项）。
 *
 * 签名要与 val_set_json_chain_free 的契约一致：ctx 是 Pager*，
 * head 是链首页页号。
 *
 * 【为什么要包一层而不是直接把 json_ovf_free 强转】
 * json_ovf_free 的第二个参数是 u64 ext_key，与这里一致；
 * 但**签名类型不同**（Pager* vs void*）—— 强转函数指针在 C 里是 UB。
 * 一行适配换来零 UB，值得。
 */
static int fn_json_chain_free(void *pager, u64 head)
{
    if (!pager)
        return E_INVAL;
    return json_ovf_free((Pager *)pager, head);
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

    /*
     * 页链的两个接线点（C5.3b-b）。
     *
     * 【为什么由 doc 模态注册】
     * 值层是 L1、页链在 L4。读链要知道页内布局、还链要知道页分配器语义，
     * 两者都只有 doc 模态知道。值层只持函数指针。
     *
     * 【为什么 ctx 就是 Pager】
     * json_ovf_detoast 的 ctx 签名是 void*，实际只用它的 Pager 部分；
     * 归还函数同款。record 解码时由 heap AM 把 Pager 从 Heap 里取出来
     * 当 decode_ctx 传进来 —— 这样值层始终不知道"Pager 是什么"。
     */
    rc = val_set_json_chain_detoast(json_ovf_detoast);
    if (rc != OK && rc != E_INVAL)
        worst = rc;

    rc = val_set_json_chain_free(fn_json_chain_free);
    if (rc != OK && rc != E_INVAL)
        worst = rc;

    /*
     * 把内联阈值交给值层保管。
     *
     * 查询层要做"内联还是页链"的分流，但它不认识 JSON_INLINE_MAX ——
     * A7 审计禁止查询层写死模态的表示细节。注册表是那条窄缝。
     */
    rc = val_set_json_inline_max(JSON_INLINE_MAX);
    if (rc != OK && rc != E_INVAL)
        worst = rc;

    return worst;
}
