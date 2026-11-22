#ifndef DB_MODALITIES_DOC_JSON_MATCH_H
#define DB_MODALITIES_DOC_JSON_MATCH_H

/*
 * json_match.h —— 匹配规格解析与求值（v1.6 C5.7）
 *
 * `json_match(doc, '$.a.b = 5')` 的第二个实参就是这里的"规格"。
 *
 * ==================== 它管什么、不管什么 ====================
 *
 * 管：把规格字符串拆成 (路径, 比较符, 字面量)，并在 JSONB 上求值。
 * 不管：路径解析（`json_path.c`）、标量映射（`json_scalar.c`）、
 *       比较语义（`val_cmp`）。**一个语义只有一个主人。**
 *
 * 这一条是判决 1 的核心承诺：路径索引建键用的是
 * `json_extract_scalar`，执行器重判 WHERE 走本文件，最终也调
 * `json_extract_scalar` ⇒ 两边不可能语义分叉。
 *
 * ==================== 规格语法（故意极简） ====================
 *
 *      <路径> <比较符> <字面量>
 *
 * 例：`$.a.b = 5`、`$.name != "x"`、`$.age >= 18`、`$.ok = true`、`$.n = null`
 *
 * 比较符：`=`  `!=`  `<>`  `<`  `<=`  `>`  `>=`
 * 字面量：整数 / 小数 / 单或双引号字符串 / `true` / `false` / `null`
 *
 * ⚠️ **不支持 AND / OR / 括号 / 嵌套**。
 * 组合交给 SQL 层：`json_match(d,'$.a=1') AND json_match(d,'$.b=2')`
 * 与"规格里写 AND"语义一样，但不需要在这里再造一套表达式求值器。
 *
 * ⚠️ **字符串字面量不做转义还原**（规格是给机器拼的）。
 * 要匹配含引号的值，用 `json_extract` + 普通比较，别用 `json_match`。
 *
 * ⚠️ 路径必须以 `$` 开头；不以 `$` 开头 ⇒ E_INVAL（不猜）。
 */

#include "common/error.h"
#include "common/types.h"
#include "modalities/doc/json_path.h"
#include "storage/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 比较符 */
typedef enum {
    JSON_OP_EQ = 0,
    JSON_OP_NE,
    JSON_OP_LT,
    JSON_OP_LE,
    JSON_OP_GT,
    JSON_OP_GE
} JsonMatchOp;

typedef struct {
    JsonPath   path;    /* 解析好的路径 */
    int        op;      /* JsonMatchOp */
    DbValue    lit;     /* 右边的字面量 */
} JsonMatchSpec;

/*
 * 解析匹配规格。
 *
 * 失败返回 E_INVAL（语法错、路径解析失败、长度超限都在这里）。
 * ⚠️ 失败时 out 的内容**未定义** —— 调用方不要读它。
 */
int  json_match_parse(const char *text, JsonMatchSpec *out);

/*
 * 对一个 JSONB 值求值这条规格。
 *
 * out = true 表示匹配。
 *
 * 【三值逻辑】
 * 路径不存在 / 值是 JSON null / 类型不匹配 ⇒ 比较结果是"未知" ⇒
 * **所有比较符（含 `!=`）都给 false**。
 *
 * ⚠️ 这一点容易写错：`NULL != 5` 在 SQL 里是"未知"，不是 true。
 *    把"未知"当 true 会让 WHERE 多返回行 —— 静默错。
 *
 * ⚠️ 路径不存在**不是**错误（返回 OK + false），不打断查询。
 *    判据里"这个文档没有 $.a"的正确答案是"这一行不满足"。
 */
int  json_match_eval(const u8 *jsonb, u16 len, const JsonMatchSpec *spec,
                     bool *out);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_DOC_JSON_MATCH_H */
