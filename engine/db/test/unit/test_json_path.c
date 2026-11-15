/*
 * test_json_path.c —— 路径表达式 + 通用标量映射的验收（v1.6 步骤 2 · 判决 1）
 *
 * 七组验收：
 *   1. **路径解析（合法）**：路径语法全表。
 *   2. **路径解析（非法）**：不支持的写法一律 E_INVAL（不许猜）。
 *   3. **提取结果集**：对照 SQLite JSON1 的 json_extract 语义。
 *   4. **映射表逐条**：判决 1 的映射表 8 行，每行都有用例。
 *   5. **类型不敏感的边界**：字符串 "5" 与数字 5 不是一回事。
 *   6. **NULL 的三值逻辑**：NULL 参与比较 ⇒ "不知道"，不是 FALSE。
 *   7. **不建树取值**：字节流上直接取 与 树上取，结果必须一致。
 *   8. **超长字符串的外部值兜底**：E_NOSPACE ⇒ 取位置 ⇒ detoast 原样读回。
 *
 * 【为什么"缺一条"与"多一条"同样致命】
 *
 * 提取少给 ⇒ 静默漏数据；提取多给 ⇒ 静默给错答案
 * （比如把对象当成字符串，或者把 "abc" 偷偷转成 0）。
 * 所以本测试看的不是"提取得准"，而是"**不多不少、不改口径**"。
 *
 * 【对照裁判：SQLite JSON1】
 *
 * 计划 §1.1 说"SQLite 当语法裁判"——
 * 路径语义以 SQLite 的 json_extract 为参照系，
 * 但**映射到通用标量**这一步是本项目自己的（SQLite 没有 DbValue）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/doc/json_path.h"
#include "modalities/doc/jsonb.h"
#include "storage/value.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---------------- 样例文档 ---------------- */

/*
 * 造一棵样例树：
 *
 *   {
 *     "s": "5",            字符串
 *     "i": 1,              整数
 *     "d": 1.5,            小数
 *     "b": true,           布尔
 *     "bf": false,         布尔
 *     "n": null,           null
 *     "obj": {"x": 1},     对象
 *     "arr": [10, 20, 30], 数组
 *     "a b": 7,            键里有空格
 *     "a.b": 8,            键里有点
 *     "mid": 255 字节的串,  刚好放得进内联文本
 *     "big": 300 字节的串,  超长 ⇒ 走外部值
 *   }
 */

/* "mid" 的长度：255 字节 —— 内联文本的上限（VAL_TEXT_MAX = 256，含结尾） */
#define MID_LEN 255
/* "big" 的长度：300 字节 —— 超过内联上限 */
#define BIG_LEN 300

static char g_mid[MID_LEN + 1];
static char g_big[BIG_LEN + 1];

/* 造两条定长字符串（内容可预测：a..z 循环） */
static void make_strings(void)
{
    int i;

    for (i = 0; i < MID_LEN; i++)
        g_mid[i] = (char)('a' + (i % 26));
    g_mid[MID_LEN] = '\0';

    for (i = 0; i < BIG_LEN; i++)
        g_big[i] = (char)('a' + (i % 26));
    g_big[BIG_LEN] = '\0';
}

static JsonbValue *build_doc(void)
{
    JsonbValue *root = jsonb_new_object();
    JsonbValue *obj = jsonb_new_object();
    JsonbValue *arr = jsonb_new_array();

    if (root == NULL || obj == NULL || arr == NULL)
        return NULL;

    (void)jsonb_obj_set(root, "s", 1, jsonb_new_string("5", 1));
    (void)jsonb_obj_set(root, "i", 1, jsonb_new_int(1));
    (void)jsonb_obj_set(root, "d", 1, jsonb_new_double(1.5));
    (void)jsonb_obj_set(root, "b", 1, jsonb_new_bool(true));
    (void)jsonb_obj_set(root, "bf", 2, jsonb_new_bool(false));
    (void)jsonb_obj_set(root, "n", 1, jsonb_new_null());

    (void)jsonb_obj_set(obj, "x", 1, jsonb_new_int(1));
    (void)jsonb_obj_set(root, "obj", 3, obj);

    (void)jsonb_arr_push(arr, jsonb_new_int(10));
    (void)jsonb_arr_push(arr, jsonb_new_int(20));
    (void)jsonb_arr_push(arr, jsonb_new_int(30));
    (void)jsonb_obj_set(root, "arr", 3, arr);

    (void)jsonb_obj_set(root, "a b", 3, jsonb_new_int(7));
    (void)jsonb_obj_set(root, "a.b", 3, jsonb_new_int(8));

    /* 两条定长字符串：255 字节（放得进）与 300 字节（超长） */
    (void)jsonb_obj_set(root, "mid", 3, jsonb_new_string(g_mid, MID_LEN));
    (void)jsonb_obj_set(root, "big", 3, jsonb_new_string(g_big, BIG_LEN));

    return root;
}

/* ---------------- 值的比较 ---------------- */

/* 两个 DbValue 是不是同一个（类型 + 内容 + NULL 标志） */
static bool same_value(const DbValue *a, const DbValue *b)
{
    if (a->is_null != b->is_null)
        return false;
    if (a->is_null)
        return true;   /* 都是 NULL：内容无意义 */
    if (a->type != b->type)
        return false;

    switch (a->type) {
    case T_INT4:
    case T_INT8:
    case T_BOOL:
        return a->i == b->i;
    case T_REAL:
        return a->d == b->d;
    case T_TEXT:
        return a->len == b->len && memcmp(a->s, b->s, a->len) == 0;
    default:
        return true;
    }
}

/* ---------------- 1. 路径解析（合法） ---------------- */

static void check_ok_path(const char *expr, u32 want_steps)
{
    JsonPath p;
    char err[128];
    int rc;

    memset(&p, 0xA5, sizeof(p));   /* 故意填脏：解析函数必须自己清干净 */
    rc = json_path_parse(expr, &p, err, (u32)sizeof(err));
    if (rc != OK) {
        printf("    FAIL 合法路径被判非法: '%s'（err=%s）\n", expr, err);
        failures++;
        return;
    }
    if (p.nsteps != want_steps) {
        printf("    FAIL 路径 '%s' 的步数 = %u，应为 %u\n", expr, p.nsteps, want_steps);
        failures++;
    }
}

static void t1_parse_ok(void)
{
    JsonPath p;
    int rc;

    printf("  1. 路径解析（合法）\n");

    check_ok_path("$", 0u);
    check_ok_path("$.a", 1u);
    check_ok_path("$.a.b", 2u);
    check_ok_path("$.a.b.c.d", 4u);
    check_ok_path("$.a.b[0].c", 4u);
    check_ok_path("$[\"a b\"]", 1u);
    check_ok_path("$[\"a.b\"]", 1u);
    check_ok_path("$[0]", 1u);
    check_ok_path("$[10][20]", 2u);
    check_ok_path("$[4294967295]", 1u);  /* u32 最大下标仍然合法 */
    check_ok_path("$._x9", 1u);
    check_ok_path("$.a1", 1u);

    /* 步数上限 16：正好 16 步合法 */
    check_ok_path("$.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a", 16u);

    /* 步的种类与内容 */
    rc = json_path_parse("$.a.b[0].c", &p, NULL, 0);
    CHECK(rc == OK);
    CHECK(p.nsteps == 4u);
    CHECK(p.steps[0].kind == JSONP_MEMBER);
    CHECK(strcmp(p.steps[0].name, "a") == 0);
    CHECK(p.steps[0].name_len == 1u);
    CHECK(p.steps[1].kind == JSONP_MEMBER);
    CHECK(strcmp(p.steps[1].name, "b") == 0);
    CHECK(p.steps[2].kind == JSONP_INDEX);
    CHECK(p.steps[2].index == 0u);
    CHECK(p.steps[3].kind == JSONP_MEMBER);
    CHECK(strcmp(p.steps[3].name, "c") == 0);

    rc = json_path_parse("$[\"a b\"]", &p, NULL, 0);
    CHECK(rc == OK);
    CHECK(p.nsteps == 1u);
    CHECK(p.steps[0].kind == JSONP_MEMBER);
    CHECK(strcmp(p.steps[0].name, "a b") == 0);
    CHECK(p.steps[0].name_len == 3u);

    rc = json_path_parse("$[10][20]", &p, NULL, 0);
    CHECK(rc == OK);
    CHECK(p.nsteps == 2u);
    CHECK(p.steps[0].kind == JSONP_INDEX);
    CHECK(p.steps[0].index == 10u);
    CHECK(p.steps[1].index == 20u);
}

/* ---------------- 2. 路径解析（非法） ---------------- */

static void check_bad_path(const char *expr)
{
    JsonPath p;
    char err[128];
    int rc;

    memset(err, 0, sizeof(err));
    rc = json_path_parse(expr, &p, err, (u32)sizeof(err));
    if (rc != E_INVAL) {
        printf("    FAIL 非法路径没被判非法: '%s'（rc=%d）\n", expr, rc);
        failures++;
        return;
    }
    if (err[0] == '\0') {
        printf("    FAIL 非法路径 '%s' 没给出错误说明\n", expr);
        failures++;
    }
}

static void t2_parse_bad(void)
{
    printf("  2. 路径解析（非法）\n");

    check_bad_path("");            /* 空字符串 */
    check_bad_path("a.b");         /* 没有 $ */
    check_bad_path("$..a");        /* 空名字 */
    check_bad_path("$.a[");        /* 没闭合 */
    check_bad_path("$.a[]");       /* 空下标 */
    check_bad_path("$[");          /* 没闭合 */
    check_bad_path("$[*]");        /* 通配符：不支持 */
    check_bad_path("$..");         /* 递归下降：不支持 */
    check_bad_path("$.");          /* 末尾的点 */
    check_bad_path("$.a.");        /* 末尾的点 */
    check_bad_path("$[\"a\"");     /* 引号没闭合 */
    check_bad_path("$[\"a\"]x");   /* 后面还有垃圾 */
    check_bad_path("$.a-b");       /* '-' 不是标识符字符 */
    check_bad_path("$[a]");        /* 下标不是数字 */
    check_bad_path("$[4294967296]"); /* 超过 u32 */
    check_bad_path("$[18446744073709551616]"); /* u64 溢出后不得回绕 */
    check_bad_path("$[-1]");       /* 负下标：语法里没有 */
    check_bad_path("$[1.5]");      /* 小数下标 */
    check_bad_path("$['a']");      /* 单引号：不支持 */
    check_bad_path("$.a[*]");      /* 通配符 */
    check_bad_path("$.a[0:2]");    /* 切片：不支持 */

    /* 步数超上限：报错（不是截断） */
    check_bad_path("$.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a");
}

/* ---------------- 3~4. 提取结果集 + 映射表 ---------------- */

/* 一条提取用例 */
typedef struct {
    const char *expr;
    int         want_rc;      /* json_extract_scalar 的期望返回码 */
    bool        want_null;    /* 期望是 NULL 值 */
    u8          want_type;    /* 非 NULL 时的期望类型 */
    i64         want_i;       /* 整数型的期望值 */
    double      want_d;       /* 实数型的期望值 */
    const char *want_s;       /* 文本型的期望值 */
} Case;

/*
 * 用例表：左边是路径，右边是"提取 + 映射"之后的期望结果。
 *
 * ⚠️ E_NOTFOUND 与 OK 都可能出现：路径不存在 ⇒ E_NOTFOUND（值是 NULL）；
 *    路径存在但是 null / object / array ⇒ OK（值也是 NULL）。
 *    两者对外都是 DbValue(NULL)（坑 M7-7）。
 */
static const Case k_cases[] = {
    /* --- 映射表第 1 行：字符串 ⇒ TEXT --- */
    { "$.s",      OK,         false, T_TEXT, 0, 0.0, "5" },
    /* --- 第 2 行：整数 ⇒ INT --- */
    { "$.i",      OK,         false, T_INT8, 1, 0.0, NULL },
    /* --- 第 3 行：小数 ⇒ REAL --- */
    { "$.d",      OK,         false, T_REAL, 0, 1.5, NULL },
    /* --- 第 4 行：true / false ⇒ INT 1 / 0 --- */
    { "$.b",      OK,         false, T_INT8, 1, 0.0, NULL },
    { "$.bf",     OK,         false, T_INT8, 0, 0.0, NULL },
    /* --- 第 5 行：null ⇒ NULL --- */
    { "$.n",      OK,         true,  T_TEXT, 0, 0.0, NULL },
    /* --- 第 6 行：路径不存在 ⇒ NULL --- */
    { "$.missing", E_NOTFOUND, true,  T_TEXT, 0, 0.0, NULL },
    /* --- 第 7 行：类型不匹配 ⇒ NULL --- */
    { "$.s.x",    E_NOTFOUND, true,  T_TEXT, 0, 0.0, NULL },  /* 标量上取成员 */
    { "$.obj[0]", E_NOTFOUND, true,  T_TEXT, 0, 0.0, NULL },  /* 对象上取下标 */
    { "$.arr[5]", E_NOTFOUND, true,  T_TEXT, 0, 0.0, NULL },  /* 下标越界 */
    /* --- 第 8 行：object / array ⇒ NULL（不是错误） --- */
    { "$.obj",    OK,         true,  T_TEXT, 0, 0.0, NULL },
    { "$.arr",    OK,         true,  T_TEXT, 0, 0.0, NULL },
    { "$",        OK,         true,  T_TEXT, 0, 0.0, NULL },  /* 根是对象 ⇒ NULL */
    /* --- 正常取到标量 --- */
    { "$.arr[0]", OK,         false, T_INT8, 10, 0.0, NULL },
    { "$.arr[1]", OK,         false, T_INT8, 20, 0.0, NULL },
    { "$.arr[2]", OK,         false, T_INT8, 30, 0.0, NULL },
    { "$.obj.x",  OK,         false, T_INT8, 1, 0.0, NULL },
    { "$[\"a b\"]", OK,       false, T_INT8, 7, 0.0, NULL },
    { "$[\"a.b\"]", OK,       false, T_INT8, 8, 0.0, NULL }
};

static void t3_extract_and_map(const JsonbValue *root, const u8 *buf, u32 len)
{
    u32 i;

    printf("  3. 提取结果集（对照 SQLite JSON1）\n");
    printf("  4. 映射表逐条（判决 1）\n");

    for (i = 0; i < (u32)(sizeof(k_cases) / sizeof(k_cases[0])); i++) {
        const Case *c = &k_cases[i];
        JsonPath p;
        DbValue vb;
        DbValue vt;
        int rc_b;
        int rc_t;

        if (json_path_parse(c->expr, &p, NULL, 0) != OK) {
            printf("    FAIL 用例路径解析失败: '%s'\n", c->expr);
            failures++;
            continue;
        }

        /* 字节流上取（不建树） */
        rc_b = json_extract_scalar(buf, len, &p, &vb);
        /* 树上取 */
        rc_t = json_extract_scalar_tree(root, &p, &vt);

        /* 两条路必须给出同一个答案（坑 M7-4） */
        if (rc_b != rc_t) {
            printf("    FAIL '%s' 两条路返回码不同：字节流=%d 树=%d\n",
                   c->expr, rc_b, rc_t);
            failures++;
            continue;
        }
        if (!same_value(&vb, &vt)) {
            printf("    FAIL '%s' 两条路的值不同\n", c->expr);
            failures++;
            continue;
        }

        if (rc_b != c->want_rc) {
            printf("    FAIL '%s' 返回码 = %d，应为 %d\n",
                   c->expr, rc_b, c->want_rc);
            failures++;
            continue;
        }
        if (vb.is_null != c->want_null) {
            printf("    FAIL '%s' is_null = %d，应为 %d\n",
                   c->expr, (int)vb.is_null, (int)c->want_null);
            failures++;
            continue;
        }
        if (c->want_null)
            continue;

        if (vb.type != c->want_type) {
            printf("    FAIL '%s' 类型 = %d，应为 %d\n",
                   c->expr, (int)vb.type, (int)c->want_type);
            failures++;
            continue;
        }
        if (c->want_type == T_REAL) {
            if (vb.d != c->want_d) {
                printf("    FAIL '%s' 实数值 = %f，应为 %f\n",
                       c->expr, vb.d, c->want_d);
                failures++;
            }
        } else if (c->want_type == T_TEXT) {
            if (vb.len != (u16)strlen(c->want_s) ||
                memcmp(vb.s, c->want_s, vb.len) != 0) {
                printf("    FAIL '%s' 文本值 = '%s'，应为 '%s'\n",
                       c->expr, vb.s, c->want_s);
                failures++;
            }
        } else {
            if (vb.i != c->want_i) {
                printf("    FAIL '%s' 整数值 = %lld，应为 %lld\n",
                       c->expr, (long long)vb.i, (long long)c->want_i);
                failures++;
            }
        }
    }
}

/* ---------------- 5. 类型不敏感的边界 ---------------- */

static void t5_type_edges(const JsonbValue *root, const u8 *buf, u32 len)
{
    JsonPath p;
    DbValue v;
    DbValue other;
    bool eq = false;
    int rc;

    printf("  5. 类型不敏感的边界\n");

    /* 取一个字符串 "5" */
    CHECK(json_path_parse("$.s", &p, NULL, 0) == OK);
    CHECK(json_extract_scalar(buf, len, &p, &v) == OK);
    CHECK(v.type == T_TEXT);

    /* "5" = 5 ⇒ 不是 TRUE（字符串不等于数字） */
    val_int(&other, 5);
    rc = json_scalar_eq(&v, &other, &eq);
    CHECK(rc == OK);
    CHECK(eq == false);

    /* "5" = "5" ⇒ TRUE */
    val_text(&other, "5", 1);
    rc = json_scalar_eq(&v, &other, &eq);
    CHECK(rc == OK);
    CHECK(eq == true);

    /* "5" = "6" ⇒ FALSE */
    val_text(&other, "6", 1);
    rc = json_scalar_eq(&v, &other, &eq);
    CHECK(rc == OK);
    CHECK(eq == false);

    /* true（映射成 INT 1）= 1 ⇒ TRUE */
    CHECK(json_path_parse("$.b", &p, NULL, 0) == OK);
    CHECK(json_extract_scalar(buf, len, &p, &v) == OK);
    CHECK(v.type == T_INT8);
    CHECK(v.i == 1);
    val_int(&other, 1);
    rc = json_scalar_eq(&v, &other, &eq);
    CHECK(rc == OK);
    CHECK(eq == true);

    /* 整数 1 = 1.0 ⇒ TRUE（INT 与 REAL 数值相等即相等） */
    CHECK(json_path_parse("$.i", &p, NULL, 0) == OK);
    CHECK(json_extract_scalar(buf, len, &p, &v) == OK);
    val_real(&other, 1.0);
    rc = json_scalar_eq(&v, &other, &eq);
    CHECK(rc == OK);
    CHECK(eq == true);

    /* 整数 1 = 2.0 ⇒ FALSE */
    val_real(&other, 2.0);
    rc = json_scalar_eq(&v, &other, &eq);
    CHECK(rc == OK);
    CHECK(eq == false);

    /* 树上的那条路给出同样的结论 */
    CHECK(json_extract_scalar_tree(root, &p, &v) == OK);
    val_int(&other, 1);
    rc = json_scalar_eq(&v, &other, &eq);
    CHECK(rc == OK);
    CHECK(eq == true);
}

/* ---------------- 6. NULL 的三值逻辑 ---------------- */

static void t6_null_logic(void)
{
    DbValue a;
    DbValue b;
    bool eq = true;
    int rc;

    printf("  6. NULL 的三值逻辑\n");

    /* NULL 与任何值比 ⇒ "不知道"（E_NOTFOUND），不是 FALSE */
    val_null(&a);
    val_int(&b, 1);
    rc = json_scalar_eq(&a, &b, &eq);
    CHECK(rc == E_NOTFOUND);

    val_null(&b);
    val_int(&a, 1);
    rc = json_scalar_eq(&a, &b, &eq);
    CHECK(rc == E_NOTFOUND);

    /* NULL 与 NULL ⇒ 还是"不知道" */
    val_null(&a);
    val_null(&b);
    rc = json_scalar_eq(&a, &b, &eq);
    CHECK(rc == E_NOTFOUND);

    /* 非法参数 */
    rc = json_scalar_eq(NULL, &b, &eq);
    CHECK(rc == E_INVAL);
    rc = json_scalar_eq(&a, &b, NULL);
    CHECK(rc == E_INVAL);
}

/* ---------------- 7. 不建树取值 ---------------- */

static void t7_bytes_vs_tree(const JsonbValue *root, const u8 *buf, u32 len)
{
    static const char *paths[] = {
        "$", "$.s", "$.i", "$.d", "$.b", "$.n", "$.obj", "$.arr",
        "$.arr[0]", "$.arr[2]", "$.arr[9]", "$.obj.x", "$.obj[0]",
        "$.missing", "$.s.x", "$[\"a b\"]", "$[\"a.b\"]"
    };
    u32 i;

    printf("  7. 不建树取值（字节流 vs 树）\n");

    for (i = 0; i < (u32)(sizeof(paths) / sizeof(paths[0])); i++) {
        JsonPath p;
        DbValue vb;
        DbValue vt;
        const u8 *raw = NULL;
        u32 raw_len = 0;
        int rc_b;
        int rc_t;
        int rc_raw;

        if (json_path_parse(paths[i], &p, NULL, 0) != OK) {
            printf("    FAIL 路径解析失败: '%s'\n", paths[i]);
            failures++;
            continue;
        }

        /* 不建树：直接在字节流上按路径取值 */
        rc_raw = jsonb_extract(buf, len, &p, &raw, &raw_len);
        /* 两条取值路 + 一条映射路 */
        rc_b = json_extract_scalar(buf, len, &p, &vb);
        rc_t = json_extract_scalar_tree(root, &p, &vt);

        if (rc_b != rc_t) {
            printf("    FAIL '%s' 返回码不同：%d vs %d\n", paths[i], rc_b, rc_t);
            failures++;
            continue;
        }
        if (!same_value(&vb, &vt)) {
            printf("    FAIL '%s' 值不同\n", paths[i]);
            failures++;
            continue;
        }
        /* 原始取值的成败必须与映射一致 */
        if ((rc_raw == OK) != (rc_b == OK)) {
            printf("    FAIL '%s' 原始取值(%d)与映射(%d)不一致\n",
                   paths[i], rc_raw, rc_b);
            failures++;
        }
    }
}

/* ---------------- 8. 超长字符串的外部值兜底 ---------------- */

/*
 * 【这一组验的是 lead 裁决的那条链】
 *
 *   json_extract_scalar  ⇒ E_NOSPACE（纯函数，不截断、不打断查询）
 *        ↓
 *   json_extract_string_ext ⇒ 拿到那个字符串在 JSONB 缓冲里的位置与长度
 *        ↓
 *   json_detoast_mem ⇒ 两次调用（先问长度、再取内容），内容与原文逐字节相同
 *
 * ⚠️ 顺带验一条**边界**：255 字节的字符串仍然**内联**（不报错、不截断）——
 *    边界内外行为不同是正常的，但必须**在边界上**就对。
 */
static void t8_long_string_ext(const u8 *buf, u32 len)
{
    JsonPath p;
    DbValue v;
    const char *sp = NULL;
    u32 slen = 0;
    u16 need = 0;
    u16 got = 0;
    u8 out[BIG_LEN + 16];
    u8 *mat = NULL;
    u32 matlen = 0;

    printf("  8. 超长字符串的外部值兜底\n");

    /* ---- 边界内：255 字节 ⇒ 正常内联，一个字节都不许少 ---- */
    CHECK(json_path_parse("$.mid", &p, NULL, 0) == OK);
    CHECK(json_extract_scalar(buf, len, &p, &v) == OK);
    CHECK(v.is_null == false);
    CHECK(v.type == T_TEXT);
    CHECK(v.len == (u16)MID_LEN);
    CHECK(memcmp(v.s, g_mid, MID_LEN) == 0);

    /* ---- 边界外：300 字节 ⇒ E_NOSPACE（**不截断**、不打断查询） ---- */
    CHECK(json_path_parse("$.big", &p, NULL, 0) == OK);
    CHECK(json_extract_scalar(buf, len, &p, &v) == E_NOSPACE);
    /* 失败时不能留下半个字符串（out 是干净的 NULL，不是被截断的残骸） */
    CHECK(v.is_null == true);

    /* ---- 取位置与长度 ---- */
    CHECK(json_extract_string_ext(buf, len, &p, &sp, &slen) == OK);
    CHECK(sp != NULL);
    CHECK(slen == (u32)BIG_LEN);
    CHECK(memcmp(sp, g_big, BIG_LEN) == 0);

    /* ---- detoast：两次调用语义 ---- */
    /* 第一次：只问长度（cap == 0 ⇒ E_NOSPACE + 填长度） */
    CHECK(json_detoast_mem((void *)sp, (u64)BIG_LEN, 0, NULL, 0, &need) == E_NOSPACE);
    CHECK(need == (u16)BIG_LEN);

    /* 第二次：真正读，内容逐字节相同 */
    memset(out, 0xCC, sizeof(out));
    CHECK(json_detoast_mem((void *)sp, (u64)BIG_LEN, 0, out, (u16)BIG_LEN, &got) == OK);
    CHECK(got == (u16)BIG_LEN);
    CHECK(memcmp(out, g_big, BIG_LEN) == 0);

    /* 缓冲不够 ⇒ 不写 buf，只报长度（value.h 的约定） */
    memset(out, 0xCC, sizeof(out));
    got = 0;
    CHECK(json_detoast_mem((void *)sp, (u64)BIG_LEN, 0, out, 10, &got) == E_NOSPACE);
    CHECK(got == (u16)BIG_LEN);
    CHECK(out[0] == 0xCC);

    /* off != 0 ⇒ E_INVAL（本实现只支持"从开头读"） */
    CHECK(json_detoast_mem((void *)sp, (u64)BIG_LEN, 1, out, (u16)BIG_LEN, &got) == E_INVAL);

    /*
     * 长度超过 u16（65535）⇒ E_NOTSUP（**不是** E_NOSPACE）。
     *
     * ⚠️ 必须是 E_NOTSUP：DetoastFn 的"问长度"那一步，
     *    E_NOSPACE 的含义是"长度问到了"——
     *    若这里返回 E_NOSPACE 却没填长度，
     *    val_detoast 会以为内容是空的 ⇒ **静默给错答案**。
     */
    need = 0;
    CHECK(json_detoast_mem((void *)sp, 70000u, 0, NULL, 0, &need) == E_NOTSUP);

    /* ---- 整条链：外部值 ⇒ val_detoast 原样读回 ---- */
    {
        DbValue ev;

        val_external_text(&ev, (u64)BIG_LEN, 0, json_detoast_mem, (void *)sp);
        CHECK(ev.external == true);
        CHECK(val_detoast(&ev, &mat, &matlen) == OK);
        CHECK(mat != NULL);
        CHECK(matlen == (u32)BIG_LEN);
        if (mat != NULL)
            CHECK(memcmp(mat, g_big, BIG_LEN) == 0);
        free(mat);
    }

    /* ---- 不是字符串 ⇒ E_INVAL（外部值这条路只对字符串开） ---- */
    CHECK(json_path_parse("$.i", &p, NULL, 0) == OK);
    CHECK(json_extract_string_ext(buf, len, &p, &sp, &slen) == E_INVAL);

    /* ---- 路径不存在 ⇒ E_NOTFOUND ---- */
    CHECK(json_path_parse("$.missing", &p, NULL, 0) == OK);
    CHECK(json_extract_string_ext(buf, len, &p, &sp, &slen) == E_NOTFOUND);
}

/* ---------------- 主流程 ---------------- */

int main(void)
{
    JsonbValue *root = NULL;
    u8 buf[2048];   /* 装得下两条长字符串（255 + 300 字节） */
    u32 len = 0;

    printf("test_json_path —— 路径表达式 + 通用标量映射\n");

    /* 两条定长字符串（样例文档要用） */
    make_strings();

    root = build_doc();
    if (root == NULL) {
        printf("    FAIL 造不出样例树\n");
        failures++;
        printf("  %d 项断言失败\n", failures);
        return 1;
    }

    /* 编码成 JSONB 字节流（后面的"不建树"那条路用它） */
    CHECK(jsonb_encode(root, buf, (u32)sizeof(buf), &len) == OK);
    CHECK(len > 0u);

    t1_parse_ok();
    t2_parse_bad();
    t3_extract_and_map(root, buf, len);
    t5_type_edges(root, buf, len);
    t6_null_logic();
    t7_bytes_vs_tree(root, buf, len);
    t8_long_string_ext(buf, len);

    jsonb_free(root);

    if (failures == 0) {
        printf("  8 项验收全部通过\n");
        return 0;
    }
    printf("  %d 项断言失败\n", failures);
    return 1;
}
