#ifndef DB_MODALITIES_DOC_JSON_PATH_H
#define DB_MODALITIES_DOC_JSON_PATH_H

/*
 * json_path.h —— 路径表达式 + 通用标量映射（v1.6 步骤 2 · 判决 1）
 *
 * ==================== 判决 1：路径提取值 → 通用标量 ====================
 *
 * 下推契约（v0.9）要求"AM 给候选行，执行器**重新求值** WHERE"。
 * 但 WHERE 里的 `json_match(doc, '$.a.b = 5')` 要求执行器能**独立**判断文档。
 *
 * 映射表（写死，不许在别处再写一遍）：
 *
 *   JSON 字符串            → DbValue(TEXT)
 *   JSON 整数              → DbValue(INT)
 *   JSON 小数              → DbValue(REAL)
 *   JSON true / false      → DbValue(INT 1 / 0)
 *   JSON null              → DbValue(NULL)
 *   路径不存在             → DbValue(NULL)
 *   类型不匹配（下面细说） → DbValue(NULL)
 *   object / array         → DbValue(NULL)（语义写死，见计划 §1.1 不做表）
 *
 * ⚠️ **"类型不匹配"必须落到 NULL，不能落到"转换后的值"**：
 * 判据 `json_extract(doc, '$.a') = 5` 里，若 `$.a` 是字符串 "abc"，
 * 那它**不等于** 5 ⇒ 必须是 NULL（SQL 的 NULL 比较语义 ⇒ 不是 TRUE）。
 * 若偷偷转成 0，那就变成 "0 = 5" ⇒ FALSE —— 结果碰巧一样，
 * 但 `$.a = "abc"` 这条就会**误判成 TRUE**（"abc" 变成 0，0 ≠ "abc"，仍不对；
 * 而 `$.a = 0` 就会**误判成 TRUE**）⇒ 那就是**静默给错答案**。
 *
 * ==================== 为什么这个函数是唯一的主人 ====================
 *
 * 路径索引按提取值建键，执行器用**同一套**提取规则重判 ——
 * 两边的映射函数是**同一个**（`json_extract_scalar`），
 * 所以不可能语义分叉。
 *
 * 这与 v1.5 的坑 M6-63 是同一条纪律：**一个语义只能有一个主人**。
 * 任何别处想"顺手转一下"都必须调这里。
 */

#include "common/error.h"
#include "common/types.h"
#include "modalities/doc/jsonb.h"
#include "storage/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 路径表达式 ---------------- */

/*
 * 路径语法（只支持这一种，不多）：
 *
 *   $            根
 *   .name        对象成员（name 是标识符：字母/数字/下划线，可含 '-'?）
 *   ["name"]     对象成员（引号形式，名字里可以有任何字符）
 *   [N]          数组第 N 个元素（N 是非负整数）
 *
 * 例：$.a.b[0].c
 *
 * ⚠️ **不支持通配符、不支持递归下降、不支持切片** ——
 * 计划 §1.1 已经划掉（多值索引不做）。
 * 遇到不支持的语法 ⇒ 解析失败（E_INVAL），**不许"猜一个意思"**。
 */

/* 路径里的一段 */
typedef enum {
    JSONP_MEMBER = 0,   /* .name 或 ["name"] */
    JSONP_INDEX  = 1    /* [N] */
} JsonpStepKind;

typedef struct {
    JsonpStepKind kind;
    char           name[128];   /* JSONP_MEMBER：成员名（NUL 结尾） */
    u32            name_len;
    u32            index;       /* JSONP_INDEX：下标 */
} JsonpStep;

#define JSONP_MAX_STEPS 16

typedef struct {
    JsonpStep steps[JSONP_MAX_STEPS];
    u32       nsteps;
} JsonPath;

/*
 * 解析路径表达式。
 *
 * 语法错 ⇒ E_INVAL（并把 err（可为 NULL）填成人话）。
 * 步数超上限 ⇒ E_INVAL。
 */
int json_path_parse(const char *expr, JsonPath *out, char *err, u32 err_len);

/* ---------------- 提取 ---------------- */

/*
 * 在 **JSONB 字节流** 上按路径取值。
 *
 * val / val_len 指向 buf **内部**（不拷贝、不分配）。
 * 路径不存在 ⇒ E_NOTFOUND（**不是** NULL 值 —— 见下面的区别）。
 *
 * ⚠️ 这条函数是"不建整棵树直接取值"的路径 —— JSONB 相对裸文本的收益所在。
 */
int jsonb_extract(const u8 *buf, u32 len, const JsonPath *path,
                  const u8 **val, u32 *val_len);

/* 在 **树** 上按路径取值（返回树里的节点，不拷贝） */
int jsonb_extract_tree(const JsonbValue *root, const JsonPath *path,
                       const JsonbValue **out);

/* ---------------- 判决 1 的那个函数 ---------------- */

/*
 * 路径提取值 → 通用标量。
 *
 * ⚠️ **这是全项目唯一做这件事的地方**。
 * 路径索引建键用它，执行器重判也用它 ⇒ 两边不可能语义分叉。
 *
 * 返回：
 *   OK          ⇒ out 是标量（可能 is_null = true）
 *   E_NOTFOUND  ⇒ **路径不存在** ⇒ out 已被填成 DbValue(NULL)。
 *                 ⚠️ 调用方**必须**把它当成 NULL（SQL 三值逻辑），
 *                 **不是**硬错误 —— 判据里"这个路径没有值"
 *                 的正确答案是"未知"，不是"把查询打断"。
 *   E_NOSPACE   ⇒ 字符串超过内联上限（见下面的外部值兜底）——
 *                 调用方**必须**处理，否则超长字符串会变成查询失败。
 *   其它        ⇒ 硬错误
 *
 * ⚠️ **E_NOTFOUND 与 "值是 JSON null" 都要映射成 DbValue(NULL)** ——
 * 区别只在诊断信息上，对外行为一样。
 */
int json_extract_scalar(const u8 *buf, u32 len, const JsonPath *path,
                        DbValue *out);

/*
 * 同上，但输入是树（索引维护时用，值在内存里）。
 *
 * ⚠️ **返回码口径与 json_extract_scalar 完全一致**：
 * E_NOTFOUND = 路径不存在（out 已是 DbValue(NULL)）、
 * E_NOSPACE = 字符串超长（调用方走外部值兜底）。
 */
int json_extract_scalar_tree(const JsonbValue *root, const JsonPath *path,
                             DbValue *out);

/* ---------------- 比较辅助（json_match 用） ---------------- */

/*
 * 两个标量比较（json_match 的 `=`）。
 *
 * ⚠️ **NULL 语义**：任一边是 NULL ⇒ 返回 E_NOTFOUND（表示"不知道"），
 * 调用方据此产出 SQL 的 NULL（三值逻辑），**不是 FALSE**。
 *
 * ⚠️ INT 与 REAL 的比较：数值相等即相等（1 == 1.0 ⇒ TRUE）。
 * ⚠️ TEXT 的比较：按字节比（SQLite JSON1 同款，不做 Unicode 归一化）。
 */
int json_scalar_eq(const DbValue *a, const DbValue *b, bool *out);

/* ---------------- 超长字符串：外部值兜底（lead 裁决） ---------------- */

/*
 * ⭐ **超长字符串走外部值，不截断、不报错打断查询。**
 *
 * 【问题】
 * DbValue 的内联文本只有 255 字节（VAL_TEXT_MAX），而 `val_text()` 会
 * **默默截断** —— 截断 = 静默给错答案（查到的值与文档里的值不是一回事）。
 *
 * 【裁决：外部值】
 * 本项目**已经有**外部值机制（value.h 的 ext_fn）：内容不在值里，
 * 由 `ext_fn` 按 (ext_key, ext_off) 去别处取。
 * blob、vector 已经是它的用户 —— 大 JSON 字符串是第 4 个。
 *
 * 而这里有个**便宜的特例**：提取路径上的 JSONB 字节**本来就在内存里**，
 * 所以"去别处取"就是"从这块内存取" ——
 * 不需要页链、不需要 pager、不需要额外的 IO。
 *
 * 【怎么用】
 * `json_extract_scalar()` 保持**纯函数**（它不知道 DbValue 之外的任何事）：
 * 字符串超过 VAL_TEXT_MAX ⇒ 返回 **E_NOSPACE**（不截断）。
 * 调用方（值层 / 执行器）拿到 E_NOSPACE 后：
 *   1. 用 `json_extract_string_ext()` 拿到那个字符串在 JSONB 缓冲里的位置；
 *   2. 用 `json_detoast_mem()` 造一个外部值挂到 DbValue 上。
 * ⇒ 调用方**必须**处理这个 E_NOSPACE，否则超长字符串会变成"查询失败"。
 *
 * ⚠️ **生命周期**：外部值的 ext_fn 读的是**调用方手里那块内存**，
 * 所以那块内存必须活到值被用完。
 * `json_detoast_mem()` 的 ctx 里存的是**指针**，
 * 调用方负责保证它有效（值层的做法：ctx 指向调用方的缓冲，
 * 并在同一次求值内完成使用）。
 */

/*
 * 取路径上的**字符串**值，返回它在 JSONB 缓冲里的位置。
 *
 * 只对 JSONB_STRING 有效；其它类型 ⇒ E_INVAL。
 * 路径不存在 ⇒ E_NOTFOUND。
 */
int json_extract_string_ext(const u8 *buf, u32 len, const JsonPath *path,
                            const char **sp, u32 *slen);

/*
 * 造一个"从内存里读"的 detoast 函数（配合 val_external_text 用）。
 *
 * ctx 指向一块内存（**必须**由调用方保证它在值用完之前有效）。
 * 契约与 DetoastFn 一致：先问长度、再取内容。
 *
 * 【参数口径】
 *   ctx → 字符串首字节；key = 字节长度；off = 0（只支持"从开头读"）。
 *
 * 【长度上限】
 * ⚠️ DetoastFn 的长度出口只有 u16（65535）⇒
 *    **超过 65535 字节的字符串在这套机制里表达不了**，
 *    此时返回 E_NOTSUP（**不是** E_NOSPACE ——
 *    E_NOSPACE 在"问长度"这一步的含义是"长度问到了"，
 *    混用会让 val_detoast 以为内容为空 ⇒ 静默给错答案）。
 */
int json_detoast_mem(void *ctx, u64 key, u64 off,
                     u8 *buf, u16 cap, u16 *len_out);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_DOC_JSON_PATH_H */
