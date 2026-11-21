/*
 * smoke_doc.c —— 文档模态的链接期独立证明
 *
 * 链接集合 = 底座 + relational + doc。链接能过 ⇒ doc 没引用别的模态。
 *
 * 【为什么这个 smoke 不走「开库 → 建对象 → 读写」】
 * 文档模态是**寄生模态**：JSONB 存在关系模态的 heap 行里，
 * 它没有自己的虚拟表，也还没有公开 API（`db_json_*` 是 W-C 的事）。
 * 所以它走内部 API 的**编解码往返**：
 *   构造 {"a":1,"b":"x"} → jsonb_encode → jsonb_decode →
 *   再从编码里取字段，断言值能对得上。
 *
 * 【为什么不违反分层】
 * 这里 include 的是 `modalities/doc/jsonb.h`（内部头）。smoke 在 `test/` 下，
 * 不在 `src/query/` 里，`check_deps.sh` 的 `forbid src/query modalities/`
 * 管不到它。
 */
#include <stdio.h>

#include "modalities/doc/jsonb.h"

#define FAIL(tag, ...)                                                         \
    do {                                                                       \
        fprintf(stderr, "smoke_doc: %s: ", (tag));                             \
        fprintf(stderr, __VA_ARGS__);                                          \
        fputc('\n', stderr);                                                   \
        return 1;                                                              \
    } while (0)

int main(void)
{
    JsonbValue *obj = NULL;
    JsonbValue *decoded = NULL;
    u8          buf[256];
    u32         out_len = 0;
    int         rc;

    /* ---- 1. 构造 {"a": 1, "b": "x"} ---- */
    obj = jsonb_new_object();
    if (!obj)
        FAIL("jsonb_new_object", "返回 NULL");

    rc = jsonb_obj_set(obj, "a", 1, jsonb_new_int(1));
    if (rc != 0)
        FAIL("jsonb_obj_set(a)", "rc=%d", rc);

    rc = jsonb_obj_set(obj, "b", 1, jsonb_new_string("x", 1));
    if (rc != 0)
        FAIL("jsonb_obj_set(b)", "rc=%d", rc);

    /* ---- 2. 编码 ---- */
    rc = jsonb_encode(obj, buf, (u32)sizeof(buf), &out_len);
    if (rc != 0)
        FAIL("jsonb_encode", "rc=%d", rc);
    if (out_len == 0)
        FAIL("编码长度", "期望 >0，实际 0");

    /* ---- 3. 解码回一棵树（证明编码是可逆的）---- */
    rc = jsonb_decode(buf, out_len, &decoded);
    if (rc != 0)
        FAIL("jsonb_decode", "rc=%d", rc);
    if (!decoded)
        FAIL("decoded", "返回 NULL");

    /* ---- 4. 从编码里取字段，断言值能对得上 ---- */
    {
        const u8 *val = NULL;
        u32       val_len = 0;

        rc = jsonb_obj_get(buf, out_len, "a", 1, &val, &val_len);
        if (rc != 0)
            FAIL("obj_get(a)", "rc=%d", rc);
        if (!val || val_len == 0)
            FAIL("obj_get(a) 内容", "取到了空值");

        rc = jsonb_obj_get(buf, out_len, "b", 1, &val, &val_len);
        if (rc != 0)
            FAIL("obj_get(b)", "rc=%d", rc);
        if (!val || val_len == 0)
            FAIL("obj_get(b) 内容", "取到了空值");

        /* 不存在的键必须报 E_NOTFOUND，不能"取到一个空值" */
        rc = jsonb_obj_get(buf, out_len, "zzz", 3, &val, &val_len);
        if (rc == 0)
            FAIL("obj_get(zzz)", "不存在的键居然取到了值");
    }

    /* ---- 5. 释放 ---- */
    jsonb_free(decoded);
    jsonb_free(obj);
    printf("smoke_doc: OK\n");
    return 0;
}
