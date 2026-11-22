/*
 * test_value_func.c —— 通用值层函数表（v1.6 C5.2）
 *
 * 4 项：
 *   1. 注册一个函数后能查回来
 *   2. 查不到的名字返回 NULL（不是崩溃、不是返回垃圾）
 *   3. 同名重复注册返回 E_INVAL（不覆盖已有实现）
 *   4. 函数本身能被调用并按 (args, nargs, out) 契约算出结果
 *
 * 【第 4 项是核心】
 * 函数表的价值不在于"能存能取"，而在于**执行器只凭名字就能调用它**。
 * 这条契约一旦破了（比如实现偷偷按名字特判），A7 审计清单就形同虚设。
 *
 * 【为什么用两个假函数而不是真的 json_ 函数】
 * 值层是 L0/L1，它**不许知道任何模态**。测试里用 json_ 命名会让后来者
 * 以为值层认识文档 —— 用 `fn_double` / `fn_concat` 这种纯假名字才对。
 */

#include <stdio.h>
#include <string.h>

#include "storage/value.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* 假函数 1：把 args[0] 的整数翻倍 */
static int fn_double(const DbValue *args, int nargs, DbValue *out)
{
    if (nargs != 1 || args[0].is_null)
        return E_INVAL;
    val_int(out, args[0].i * 2);
    return OK;
}

/* 假函数 2：把两个整数相加 */
static int fn_add(const DbValue *args, int nargs, DbValue *out)
{
    if (nargs != 2)
        return E_INVAL;
    val_int(out, args[0].i + args[1].i);
    return OK;
}

int main(void)
{
    printf("test_value_func:\n");

    /* ---- 1) 注册后能查回来 ---- */
    CHECK(val_func_register("fn_double", fn_double) == OK);
    ValFunc found = val_func_find("fn_double");
    CHECK(found == fn_double);

    /* ---- 2) 查不到的名字返回 NULL ---- */
    CHECK(val_func_find("no_such_func") == NULL);
    CHECK(val_func_find("") == NULL);

    /* ---- 3) 同名重复注册返回 E_INVAL，且不覆盖 ---- */
    CHECK(val_func_register("fn_double", fn_add) == E_INVAL);
    CHECK(val_func_find("fn_double") == fn_double);   /* 还是原来那个 */

    /* ---- 4) 函数能按契约被调用 ---- */
    CHECK(val_func_register("fn_add", fn_add) == OK);

    {
        DbValue args[2], out;
        val_null(&args[0]);
        val_null(&args[1]);
        val_null(&out);

        val_int(&args[0], 21);
        CHECK(val_func_find("fn_double")(args, 1, &out) == OK);
        CHECK(!out.is_null);
        CHECK(out.i == 42);

        val_int(&args[0], 7);
        val_int(&args[1], 8);
        CHECK(val_func_find("fn_add")(args, 2, &out) == OK);
        CHECK(out.i == 15);
    }

    if (failures == 0) {
        printf("  4 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
