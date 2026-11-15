/*
 * jsonb_test_common.h —— JSONB 单测的共用工具（v1.6 步骤 1 · T13）
 *
 * 两个测试文件共用：
 *   test/unit/test_jsonb.c        —— 语法 / 语义 / 编码的验收
 *   test/unit/test_jsonb_crash.c  —— 取值 / 损坏 / 深度 / fuzz 的验收
 *
 * ⚠️ 每个工具函数都带 JSONB_TEST_UNUSED：
 *    两个 .c 各用其中一部分，不带这个属性的话
 *    "另一份没用到"就会被 -Werror=unused-function 挑出来。
 *
 * ⚠️ **为什么不用 memcmp 比较两棵树**：
 *    JSONB 里对象的键是**排过序**的，与输入顺序可能不同；
 *    字符串还可能带 NUL 字节 ⇒ 必须逐字段（深度优先）比较。
 */

#ifndef DB_TEST_UNIT_JSONB_TEST_COMMON_H
#define DB_TEST_UNIT_JSONB_TEST_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/doc/json_parse.h"
#include "modalities/doc/jsonb.h"

#define JSONB_TEST_UNUSED __attribute__((unused))

/* 断言失败计数（由各个 .c 自己的静态变量提供） */
#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* 浮点位模式（比位模式才不会被 NaN 之类钻空子） */
static JSONB_TEST_UNUSED u64 jsonb_test_dbits(double d)
{
    u64 b;

    memcpy(&b, &d, sizeof(b));
    return b;
}

/* 在对象里找一个键（找不到 ⇒ NULL） */
static JSONB_TEST_UNUSED const JsonbValue *jsonb_test_obj_find(const JsonbValue *obj,
                                                                const char *key)
{
    u32 klen = (u32)strlen(key);
    u32 i;

    for (i = 0; i < obj->u.obj.count; i++) {
        if (obj->u.obj.key_lens[i] == klen &&
            memcmp(obj->u.obj.keys[i], key, klen) == 0) {
            return obj->u.obj.vals[i];
        }
    }
    return NULL;
}

/*
 * 按**长度**在对象里找一个键（键的权威长度是 key_lens，不是 strlen ——
 * JSON 的键可以含 NUL 字节）。
 */
static JSONB_TEST_UNUSED const JsonbValue *jsonb_test_obj_find_n(const JsonbValue *obj,
                                                                  const char *key,
                                                                  u32 klen)
{
    u32 i;

    for (i = 0; i < obj->u.obj.count; i++) {
        if (obj->u.obj.key_lens[i] == klen &&
            memcmp(obj->u.obj.keys[i], key, klen) == 0) {
            return obj->u.obj.vals[i];
        }
    }
    return NULL;
}

/* 两棵树是否逐字段相等（深度优先） */
static JSONB_TEST_UNUSED int jsonb_test_tree_equal(const JsonbValue *a,
                                                    const JsonbValue *b)
{
    u32 i;

    if (!a || !b) {
        return a == b;
    }
    if (a->tag != b->tag) {
        return 0;
    }
    switch (a->tag) {
    case JSONB_NULL:
    case JSONB_TRUE:
    case JSONB_FALSE:
        return 1;
    case JSONB_INT32:
    case JSONB_INT64:
        return a->u.i == b->u.i;
    case JSONB_FLOAT64:
        return jsonb_test_dbits(a->u.d) == jsonb_test_dbits(b->u.d);
    case JSONB_STRING:
        return a->u.s.len == b->u.s.len &&
               (a->u.s.len == 0u ||
                memcmp(a->u.s.p, b->u.s.p, a->u.s.len) == 0);
    case JSONB_ARRAY:
        if (a->u.arr.count != b->u.arr.count) {
            return 0;
        }
        for (i = 0; i < a->u.arr.count; i++) {
            if (!jsonb_test_tree_equal(a->u.arr.items[i], b->u.arr.items[i])) {
                return 0;
            }
        }
        return 1;
    case JSONB_OBJECT:
        if (a->u.obj.count != b->u.obj.count) {
            return 0;
        }
        for (i = 0; i < a->u.obj.count; i++) {
            const JsonbValue *bv = jsonb_test_obj_find_n(b, a->u.obj.keys[i],
                                                         a->u.obj.key_lens[i]);

            if (!bv || !jsonb_test_tree_equal(a->u.obj.vals[i], bv)) {
                return 0;
            }
        }
        return 1;
    default:
        return 0;
    }
}

/*
 * 往返：文本 → 树 → JSONB → 树，返回两棵树是否相等。
 *
 * enc_len 不为 NULL 时，顺带把编码后的字节数带出来。
 */
static JSONB_TEST_UNUSED int jsonb_test_roundtrip(const char *text, u32 *enc_len)
{
    JsonbValue *t1 = NULL;
    JsonbValue *t2 = NULL;
    u8 *buf = NULL;
    u32 len = 0;
    int ok = 0;

    if (json_parse(text, (u32)strlen(text), &t1) != OK) {
        return 0;
    }
    if (jsonb_encode(t1, NULL, 0u, &len) != OK || len == 0u) {
        jsonb_free(t1);
        return 0;
    }
    buf = (u8 *)malloc(len);
    if (!buf) {
        jsonb_free(t1);
        return 0;
    }
    if (jsonb_encode(t1, buf, len, &len) != OK) {
        goto out;
    }
    if (jsonb_decode(buf, len, &t2) != OK) {
        goto out;
    }
    ok = jsonb_test_tree_equal(t1, t2);

out:
    free(buf);
    jsonb_free(t1);
    jsonb_free(t2);
    if (enc_len) {
        *enc_len = len;
    }
    return ok;
}

#endif /* DB_TEST_UNIT_JSONB_TEST_COMMON_H */
