#ifndef DB_MODALITIES_DOC_JSONB_H
#define DB_MODALITIES_DOC_JSONB_H

/*
 * jsonb.h —— JSONB 二进制格式（v1.6 步骤 1）
 *
 * ==================== 为什么要 JSONB ====================
 *
 * 裸 JSON 文本每次查路径都要**从头解析一遍**。
 * JSONB 是"解析一次、反复用"：解析成树形二进制之后，
 * 按路径取值是**两次二分**（对象键按长度+字典序存放），不是全文扫描。
 *
 * 格式（判决 3）：
 *
 *   值 = [tag][payload]
 *     tag = 0  null      payload = 空
 *     tag = 1  true      payload = 空
 *     tag = 2  false     payload = 空
 *     tag = 3  int32     payload = 4 字节（小端，有符号）
 *     tag = 4  int64     payload = 8 字节（小端，有符号）
 *     tag = 5  float64   payload = 8 字节（IEEE754 小端）
 *     tag = 6  string    payload = [u32 len][utf-8 bytes]
 *     tag = 7  array     payload = [u32 count][元素]×count
 *     tag = 8  object    payload = [u32 count][[u32 klen][key][值]]×count
 *
 * ⚠️ **int32 与 int64 分开**：JSON 的整数没有宽度。
 * PG 的做法是"能放下就 int32，放不下就 int64" ——
 * 这样绝大多数整数只占 4 字节，而大整数不丢精度。
 *
 * ⚠️ **object 的键按 (长度, 字典序) 排序** ⇒ 二分可查。
 * ⚠️ **重复键保留首个**（解析时后出现的同名字段被丢弃）——
 * 语义写死，与 SQLite JSON1 一致。
 *
 * ==================== 本文件只管"二进制格式" ====================
 *
 * 解析（文本 → 树）见 json_parse.h；
 * 路径提取（树 → 标量）见 json_path.h。
 * 三层分开，是因为它们各自的验收点不同：
 * 解析的坑是**语法边角**（转义、代理对、数字精度），
 * 路径的坑是**语义**（类型映射、超集铁律）。
 */

#include "common/error.h"
#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- tag ---------------- */

typedef enum {
    JSONB_NULL    = 0,
    JSONB_TRUE    = 1,
    JSONB_FALSE   = 2,
    JSONB_INT32   = 3,
    JSONB_INT64   = 4,
    JSONB_FLOAT64 = 5,
    JSONB_STRING  = 6,
    JSONB_ARRAY   = 7,
    JSONB_OBJECT  = 8
} JsonbTag;

/* ---------------- 树 ====================
 *
 * 解析出来的中间表示。**不是** JSONB 的字节格式 ——
 * JSONB 是这个树序列化之后的字节流。
 *
 * 之所以要有中间表示：解析要先建树才能排序键、
 * 才能判断"能不能放进 int32"、才能做重复键处理。
 */
typedef struct JsonbValue JsonbValue;

struct JsonbValue {
    JsonbTag tag;

    union {
        /* JSONB_INT32 / JSONB_INT64 */
        i64  i;
        /* JSONB_FLOAT64 */
        double d;
        /* JSONB_STRING：指向 UTF-8 字节（**不**含结尾 NUL，长度在 len） */
        struct {
            const char *p;
            u32 len;
        } s;
        /* JSONB_ARRAY：count 个元素 */
        struct {
            JsonbValue **items;
            u32 count;
        } arr;
        /* JSONB_OBJECT：count 个成员（键已排序、已去重） */
        struct {
            const char **keys;      /* 各键的 UTF-8 字节 */
            u32        *key_lens;
            JsonbValue **vals;
            u32         count;
        } obj;
    } u;
};

/* ---------------- 树的构造与释放 ---------------- */

/* 造一个叶子值（null / bool / 数字） */
JsonbValue *jsonb_new_null(void);
JsonbValue *jsonb_new_bool(bool b);
JsonbValue *jsonb_new_int(i64 x);      /* 能放下 int32 就用 INT32 */
JsonbValue *jsonb_new_double(double d);
JsonbValue *jsonb_new_string(const char *p, u32 len);

/* 造容器（随后用 jsonb_arr_push / jsonb_obj_set 填） */
JsonbValue *jsonb_new_array(void);
JsonbValue *jsonb_new_object(void);

/* 数组追加（拷贝元素的所有权） */
int jsonb_arr_push(JsonbValue *arr, JsonbValue *item);

/*
 * 对象设键（**替换**已存在的同名键）。
 *
 * ⚠️ 键在对象里**按 (长度, 字典序) 排序**，所以插入是 O(n) 的移动，
 * 但对象通常很小 —— 与 PG 的 JSONB 同一个取舍。
 *
 * 重复键保留首个的语义在**解析层**实现（json_parse），
 * 这里只是"同名则替换"。
 */
int jsonb_obj_set(JsonbValue *obj, const char *key, u32 klen, JsonbValue *val);

/* 释放一棵树（深度优先） */
void jsonb_free(JsonbValue *v);

/* ---------------- 编码 / 解码 ---------------- */

/*
 * 编码成 JSONB 字节流。
 *
 * out / out_len 由调用方给缓冲。
 * 缓冲不够 ⇒ 返回 E_NOSPACE 且 **不写任何东西**（先算总长再写）。
 *
 * 传入 out == NULL ⇒ 只算长度（**两次遍历**：一次算长、一次写）。
 */
int jsonb_encode(const JsonbValue *v, u8 *out, u32 cap, u32 *out_len);

/*
 * 解码 JSONB 字节流。
 *
 * 字节流损坏 ⇒ E_CORRUPT（**绝不**返回半截的树）。
 * 解出来的树是**独立**的（不引用输入缓冲）。
 */
int jsonb_decode(const u8 *buf, u32 len, JsonbValue **out);

/* ---------------- 读取（不建树） ====================
 *
 * ⚠️ 这组函数是 JSONB 相对裸文本的**真实收益**所在：
 * 直接在字节流上按路径取值，**不建整棵树**。
 *
 * 对大文档尤其重要：查 `$.a.b` 不必把整棵树建出来。
 */

/*
 * 取对象的一个成员（键必须**已排序**，用二分查）。
 *
 * 找到 ⇒ out 指向 buf 内部的字节（**不**拷贝、不分配）。
 * 找不到 ⇒ E_NOTFOUND。
 */
int jsonb_obj_get(const u8 *buf, u32 len, const char *key, u32 klen,
                  const u8 **val, u32 *val_len);

/* 取数组的第 i 个元素（越界 ⇒ E_NOTFOUND） */
int jsonb_arr_get(const u8 *buf, u32 len, u32 idx, const u8 **val, u32 *val_len);

/* 取一个值的 tag（buf/len 指向一个值的完整编码） */
int jsonb_tag_of(const u8 *buf, u32 len, JsonbTag *tag);

/* ---------------- 渲染（JSONB → JSON 文本） ====================
 *
 * 【为什么要它】
 * JSONB 是二进制。用户 `SELECT doc FROM t` 时要是直接输出原始字节，
 * 会在内嵌 NUL、控制字符、非文本 tag 上截断或打印乱码。
 * 值层（`storage/value.c` 的 T_JSON 分支）明确拒绝渲染原始字节，
 * 指向的正是这里。
 *
 * 【调用口径：与 DetoastFn 一致】
 *   out == NULL       ⇒ 只算长度，写进 *out_len，返回 E_NOSPACE
 *   out != NULL       ⇒ 按 cap 写进 out，返回 OK
 *   cap 不够          ⇒ 返回 E_NOSPACE，且**一个字节都不写**
 *
 * ⚠️ **缓冲不足时不许写半个文本** —— 半个 JSON 看起来像合法 JSON，
 *    那比明确报错危险得多。
 *
 * 【输出形态（A1「往返全等」的口径）】
 *   - 整数原样：INT32 / INT64 出整数字面量，**绝不**走浮点格式
 *   - float64：保证 double 往返无损（1.5 → "1.5"，1.0 → "1.0"）
 *   - 字符串：引号/反斜杠/控制字符转义；**非 ASCII 字节直接输出**
 *             （不转 \uXXXX，否则中文文档读出来的与输入不一致）
 *   - 对象键按 JSONB 内部的排序输出（不是原始输入顺序）
 *
 * 实现见 jsonb_render.c（独立文件：jsonb.c 已接近 800 行硬上限）。
 */
int jsonb_render(const u8 *buf, u32 len, char *out, u32 cap, u32 *out_len);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_DOC_JSONB_H */
