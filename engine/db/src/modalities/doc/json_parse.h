#ifndef DB_MODALITIES_DOC_JSON_PARSE_H
#define DB_MODALITIES_DOC_JSON_PARSE_H

/*
 * json_parse.h —— 文本 JSON → 树（v1.6 步骤 1 · T13）
 *
 * 【这一层解决什么】
 *
 * jsonb.h 管的是**二进制格式**（怎么存、怎么取），
 * 这一层管的是**文本语法**（怎么把一段 JSON 文本变成树）。
 *
 * 分开是因为两者的验收点不同：
 *   解析的坑是**语法边角**（转义、代理对、数字精度、重复键）；
 *   路径的坑是**语义**（类型映射、超集铁律）。
 *
 * 【语法：严格的标准 JSON】
 *
 *   值   := 对象 | 数组 | 字符串 | 数字 | true | false | null
 *   对象 := '{' [ 字符串 ':' 值 { ',' 字符串 ':' 值 } ] '}'
 *   数组 := '[' [ 值 { ',' 值 } ] ']'
 *   字符串 := '"' 字符* '"'
 *   数字 := '-'? ( '0' | [1-9][0-9]* ) ( '.' [0-9]+ )? ( [eE] [+-]? [0-9]+ )?
 *
 * ⚠️ **严格**：尾逗号、单引号、没引号的键、前导零、"+"开头、".5"、"1."、
 *    裸的 NaN / Infinity —— 一律**报错**（E_INVAL），**不许猜一个意思**。
 *
 * 【重复键保留首个】
 *
 *   {"a":1,"a":2} ⇒ 保留 **1**，后出现的同名字段被**丢弃**。
 *   语义写死，与 SQLite JSON1 一致（坑 M7-17）。
 *
 * ⚠️ 注意：**被丢弃的字段仍然要完整解析** ——
 *    `{"a":1,"a":}` 是**非法**的（不能因为"反正要丢"就跳过语法检查）。
 *
 * 【数字的归类】
 *
 *   整数（文本里没有 '.' / 'e' / 'E'）：
 *       能放进 int32  ⇒ INT32
 *       能放进 int64  ⇒ INT64
 *       溢出 int64    ⇒ FLOAT64（`9223372036854775808`）
 *   带 '.' 或 'e' ⇒ FLOAT64
 *
 * ⚠️ **不许丢精度**：`1` 是 INT32、`1.0` 是 FLOAT64 —— 两者必须能区分开。
 * ⚠️ **不许报错**：`1e999` 溢出 ⇒ FLOAT64（inf），`1E-999` 下溢 ⇒ FLOAT64（0）
 *    —— SQLite 也接受，不许因为"超出范围"就失败。
 *
 * 【字符串里的 NUL】
 *
 *   `"\u0000"` 解出来是**带 NUL 字节**的字符串 ⇒
 *   长度必须一路带着走，**绝不能用 strlen**（坑 M7-15）。
 *
 * 【代理对】
 *
 *   `"\uD83D\uDE00"` ⇒ 😀（U+1F600）⇒ 解出来是 **4 字节 UTF-8**。
 *   ⚠️ **落单的代理项（半个代理对）判为非法**（E_INVAL）——
 *      它在 UTF-8 里**没有表示**（坑 M7-18）。
 *
 * 【裸 UTF-8】
 *
 *   不走 \u、直接是中文 / emoji 的字节 ⇒ 合法 UTF-8 原样保留（不重新编码）。
 *   非法首字节、续字节、截断序列、overlong、surrogate 或超出 U+10FFFF
 *   ⇒ E_INVAL，不产生半截树。
 *   （输入合同要求字符串中的原始 UTF-8 合法。）
 *
 * 【深度上限】
 *
 *   嵌套超过 JSON_PARSE_MAX_DEPTH ⇒ **E_INVAL**（不是崩）。
 *   递归下降的 parser 遇上深嵌套会爆栈 ⇒ 必须有上限。
 *
 *   ⚠️ 深度按**路径上的值的个数**计（最外层的那个值算第 1 个）⇒
 *      一条路径上最多 200 个值。
 *      换算成"嵌套几层"：嵌套 N 层对象（{"a":{"a":{...}}}）再加最里层的那个值，
 *      路径上就是 N+1 个值 ⇒ **最多嵌套 199 层**。
 */

#include "common/error.h"
#include "common/types.h"
#include "modalities/doc/jsonb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 嵌套深度上限（超过 ⇒ E_INVAL） */
#ifndef JSON_PARSE_MAX_DEPTH
#define JSON_PARSE_MAX_DEPTH 200
#endif

/*
 * 文本 JSON → 树。
 *
 * text / len —— 输入文本与它的**长度**。
 * ⚠️ 用显式长度而不是 strlen：输入里可能有 NUL（虽然那是非法的），
 *    而且显式长度让"截断的输入"能被判出来。
 *
 * 成功 ⇒ *out 是一棵**独立的**树（调用方负责 jsonb_free）。
 * 失败 ⇒ *out 置 NULL，**绝不返回半截的树**。
 *
 * 返回 E_INVAL（语法错 / 太深）/ E_NOSPACE（内存不够）。
 */
int json_parse(const char *text, u32 len, JsonbValue **out);

/*
 * 同上，但输入是 NUL 结尾的 C 字符串（按 strlen 取长度）。
 *
 * ⚠️ 用 strlen ⇒ **输入里有 NUL 就会被截断** ——
 *    要处理带 NUL 的输入必须用 json_parse()。
 */
int json_parse_str(const char *text, JsonbValue **out);

/*
 * 便利函数：文本 → JSONB 字节流（解析 + 编码，一步到位）。
 *
 * out == NULL ⇒ 只算长度（*out_len）。
 * 缓冲不够 ⇒ E_NOSPACE（*out_len 报出需要多少）。
 *
 * ⚠️ 这只是**省事**：省掉"先建树、再编码"时树在内存里的那份开销。
 *    大文档要省内存的话，应该直接写一个"边解析边编码"的流式版本（步骤 3）。
 */
int json_parse_to_jsonb(const char *text, u32 len, u8 *out, u32 cap,
                        u32 *out_len);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_DOC_JSON_PARSE_H */
