/*
 * json_path.c —— 路径表达式解析 + JSONB 上的路径提取（v1.6 步骤 2 · 判决 1）
 *
 * 契约见 json_path.h。本文件负责三件事：
 *
 *   1. json_path_parse()      —— 把路径表达式解析成"步序列"；
 *   2. jsonb_extract()        —— 在 **编码后的 JSONB 字节流**上取值（不建树）；
 *   3. jsonb_extract_tree()   —— 在 **树**上取值。
 *
 * ==================== 为什么"提取"与"映射"分成两个文件 ====================
 *
 * 判决 1 的映射（JSON 值 → DbValue）是**全项目唯一做这件事的地方**，
 * 单独放在 json_scalar.c 里（那个文件的头注释写了这句话）。
 * 本文件只管"路径怎么走、走到哪"，**不碰 DbValue** ——
 * 这样"路径语义"与"值语义"两层各自独立，改一层不会悄悄改另一层。
 *
 * ==================== 路径语法（只支持这一种） ====================
 *
 *   $            根
 *   .name        对象成员（name 是标识符：字母 / 数字 / 下划线）
 *   ["name"]     对象成员（引号形式，名字里可以有任何字符）
 *   [N]          数组第 N 个元素（N 是非负整数）
 *
 * ⚠️ **不支持通配符、不支持递归下降、不支持切片** ——
 * 遇到就解析失败（E_INVAL），**不许"猜一个意思"**。
 * 理由与 v1.5 的超集铁律同源："猜"出来的路径会静默漏数据。
 *
 * ==================== 提取失败 = E_NOTFOUND，不是"空值" ====================
 *
 * 路径不存在（键没有、下标越界、对标量用 [0]、对数组用 .name）
 * ⇒ 一律 E_NOTFOUND。
 *
 * ⚠️ **"路径不存在"与"值就是 JSON null"是两回事**：
 * 前者 E_NOTFOUND，后者 OK + 值是 null。
 * 映射层（json_scalar.c）负责把两者都变成对外的 DbValue(NULL)。
 *
 * ==================== 坑 ====================
 *
 * 坑 M7-1：路径语法必须"封闭" —— 不认识的写法一律解析失败。
 *   反例（反面教材）：把 `$.a[*]` 当成"数组全部元素"、
 *   把 `$..a` 当成"递归找 a"。这两种"猜"都会静默给出**与用户预期不同**的结果。
 *
 * 坑 M7-2：`.name` 的 name 是标识符；含特殊字符的键必须用 ["name"]。
 *   同一个键写成 `$.a-b`（解析失败）与 `$["a-b"]`（合法）——
 *   两条路给出**不同的**答案时，用户一定会踩坑，所以这里写死。
 *
 * 坑 M7-3：步数上限 16 是**报错**，不是截断。
 *   截断 = 路径被悄悄改短 = 查到别的东西去；宁可解析失败。
 *
 * 坑 M7-4：字节流上的提取与树上的提取必须给出**同一个**答案。
 *   两条路是同一套语义的两个实现（一个走字节、一个走节点），
 *   所以任何一条改了语义，另一条必须跟着改 ——
 *   这也是本步骤的验收点之一（test_json_path.c 第 2 组）。
 *
 * 坑 M7-8：["name"] 里的转义**只认 8 种简单转义**
 *   （\" \\ \/ \b \f \n \r \t）。
 *   \uXXXX（Unicode 转义）⇒ E_INVAL。
 *   理由与 M7-1 同源：半支持 = 用户以为支持了。
 *   真要支持 Unicode 转义，得在**解析层**统一处理（json_parse），
 *   不能在路径表达式这里另起一套口径。
 *
 * 坑 M7-9：成员名最多 127 字节（JsonpStep.name 是 128 字节定长数组）。
 *   超长 ⇒ E_INVAL，**不截断**（截断 = 查到的键不是用户写的那一个）。
 *
 * 坑 M7-10：下标只认**非负整数**。
 *   `$.a[-1]` 在**解析期**就是 E_INVAL —— 不是"倒数第 1 个"。
 *   "猜一个意思"在这里特别危险：同一个 `[-1]` 在别的引擎里可能是
 *   "最后一个"，在我们这里若实现成"从尾部数"，索引键就与别的引擎对不上。
 */

#include "modalities/doc/json_path.h"

#include <string.h>

/* ---------------- 小工具 ---------------- */

/* 写错误信息（err 可为 NULL） */
static void path_err(char *err, u32 err_len, const char *msg)
{
    if (err != NULL && err_len > 0u) {
        size_t n = strlen(msg);

        if (n >= (size_t)err_len)
            n = (size_t)err_len - 1u;
        memcpy(err, msg, n);
        err[n] = '\0';
    }
}

/* 标识符字符：字母 / 数字 / 下划线 */
static bool is_ident_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* ---------------- 路径解析 ---------------- */

int json_path_parse(const char *expr, JsonPath *out, char *err, u32 err_len)
{
    u32 i;
    u32 nsteps = 0;

    if (out == NULL) {
        path_err(err, err_len, "路径输出结构为空");
        return E_INVAL;
    }
    memset(out, 0, sizeof(*out));

    if (expr == NULL || expr[0] == '\0') {
        path_err(err, err_len, "路径表达式为空");
        return E_INVAL;
    }
    if (expr[0] != '$') {
        path_err(err, err_len, "路径必须以 '$' 开头");
        return E_INVAL;
    }
    i = 1u;

    while (expr[i] != '\0') {
        JsonpStep *st;

        /* 步数上限：超了**报错**，不截断（坑 M7-3） */
        if (nsteps >= JSONP_MAX_STEPS) {
            path_err(err, err_len, "路径步数超过 16");
            return E_INVAL;
        }
        st = &out->steps[nsteps];

        /* ---- .name ---- */
        if (expr[i] == '.') {
            u32 len = 0;

            i++;
            if (!is_ident_char(expr[i])) {
                path_err(err, err_len, "'.' 后面必须是标识符（字母/数字/下划线）");
                return E_INVAL;
            }
            while (is_ident_char(expr[i])) {
                len++;
                i++;
            }
            if (len >= sizeof(st->name)) {
                path_err(err, err_len, "成员名太长（最多 127 字节）");   /* 坑 M7-9 */
                return E_INVAL;
            }
            memcpy(st->name, expr + i - len, len);
            st->name[len] = '\0';
            st->name_len = len;
            st->kind = JSONP_MEMBER;
            nsteps++;
            continue;
        }

        /* ---- [ ... ] ---- */
        if (expr[i] == '[') {
            i++;

            /* ["name"] */
            if (expr[i] == '"') {
                char tmp[sizeof(st->name)];
                u32 len = 0;

                i++;
                for (;;) {
                    char c = expr[i];

                    if (c == '\0') {
                        path_err(err, err_len, "[\" 未闭合");
                        return E_INVAL;
                    }
                    if (c == '"') {
                        i++;
                        break;
                    }
                    if (c == '\\') {
                        char e = expr[i + 1];

                        i += 2;
                        switch (e) {
                        case '"':  c = '"';  break;
                        case '\\': c = '\\'; break;
                        case '/':  c = '/';  break;
                        case 'b':  c = '\b'; break;
                        case 'f':  c = '\f'; break;
                        case 'n':  c = '\n'; break;
                        case 'r':  c = '\r'; break;
                        case 't':  c = '\t'; break;
                        default:
                            /* \uXXXX 之类不猜：报错（坑 M7-1、坑 M7-8） */
                            path_err(err, err_len,
                                     "只支持 \\ \\\" \\\\ \\/ \\b \\f \\n \\r \\t 这几种转义");
                            return E_INVAL;
                        }
                    } else {
                        i++;
                    }
                    if (len >= sizeof(tmp)) {
                        path_err(err, err_len, "成员名太长（最多 127 字节）");  /* 坑 M7-9 */
                        return E_INVAL;
                    }
                    tmp[len++] = c;
                }
                if (expr[i] != ']') {
                    path_err(err, err_len, "[\"...\"] 后面缺 ']'");
                    return E_INVAL;
                }
                i++;
                memcpy(st->name, tmp, len);
                st->name[len] = '\0';
                st->name_len = len;
                st->kind = JSONP_MEMBER;
                nsteps++;
                continue;
            }

            /* [N] —— 只接受非负整数 */
            {
                u64 idx = 0;

                if (expr[i] < '0' || expr[i] > '9') {
                    /* 负数 / 小数 / 变量名 ⇒ 一律 E_INVAL（坑 M7-10） */
                    path_err(err, err_len, "只支持 [N]（非负整数下标）或 [\"name\"]");
                    return E_INVAL;
                }
                while (expr[i] >= '0' && expr[i] <= '9') {
                    idx = idx * 10u + (u64)(expr[i] - '0');
                    if (idx > 0xFFFFFFFFULL) {
                        path_err(err, err_len, "下标太大");
                        return E_INVAL;
                    }
                    i++;
                }
                if (expr[i] != ']') {
                    path_err(err, err_len, "[ ] 里面不是纯数字");
                    return E_INVAL;
                }
                i++;
                st->kind = JSONP_INDEX;
                st->index = (u32)idx;
                nsteps++;
                continue;
            }
        }

        /* 不认识的写法 ⇒ 解析失败（坑 M7-1） */
        path_err(err, err_len, "只支持 $ / .name / [\"name\"] / [N] 这几种走法");
        return E_INVAL;
    }

    out->nsteps = nsteps;
    return OK;
}

/* ---------------- 字节流上的提取 ---------------- */

int jsonb_extract(const u8 *buf, u32 len, const JsonPath *path,
                  const u8 **val, u32 *val_len)
{
    const u8 *cur = buf;
    u32 cur_len = len;
    u32 i;

    if (buf == NULL || path == NULL || val == NULL || val_len == NULL)
        return E_INVAL;
    if (len == 0u)
        return E_CORRUPT;   /* 空字节流不是一个合法值 */

    for (i = 0; i < path->nsteps; i++) {
        const JsonpStep *st = &path->steps[i];
        JsonbTag tag = JSONB_NULL;
        const u8 *next = NULL;
        u32 next_len = 0;
        int rc;

        rc = jsonb_tag_of(cur, cur_len, &tag);
        if (rc != OK)
            return rc;

        if (st->kind == JSONP_MEMBER) {
            /* 对非对象用 .name ⇒ 路径不存在（类型不匹配） */
            if (tag != JSONB_OBJECT)
                return E_NOTFOUND;
            rc = jsonb_obj_get(cur, cur_len, st->name, st->name_len, &next, &next_len);
        } else {
            /* 对非数组用 [N] ⇒ 路径不存在（类型不匹配） */
            if (tag != JSONB_ARRAY)
                return E_NOTFOUND;
            rc = jsonb_arr_get(cur, cur_len, st->index, &next, &next_len);
        }
        if (rc != OK)
            return rc;   /* E_NOTFOUND 原样透传 */
        cur = next;
        cur_len = next_len;
    }

    *val = cur;
    *val_len = cur_len;
    return OK;
}

/* ---------------- 树上的提取 ---------------- */

/* 键比较：先比长度、再比字节（与 JSONB 的排序口径一致） */
static int key_cmp(const char *ka, u32 la, const char *kb, u32 lb)
{
    u32 n = (la < lb) ? la : lb;
    int c;

    if (la != lb)
        return (la < lb) ? -1 : 1;
    c = (n == 0u) ? 0 : memcmp(ka, kb, n);
    if (c != 0)
        return (c < 0) ? -1 : 1;
    return 0;
}

int jsonb_extract_tree(const JsonbValue *root, const JsonPath *path,
                       const JsonbValue **out)
{
    const JsonbValue *cur = root;
    u32 i;

    if (root == NULL || path == NULL || out == NULL)
        return E_INVAL;

    for (i = 0; i < path->nsteps; i++) {
        const JsonpStep *st = &path->steps[i];

        if (st->kind == JSONP_MEMBER) {
            u32 lo = 0;
            u32 hi = cur->u.obj.count;
            u32 mid;

            if (cur->tag != JSONB_OBJECT)
                return E_NOTFOUND;

            /*
             * 键按 (长度, 字典序) 排序 ⇒ 二分。
             * ⚠️ 这里假定 jsonb_obj_set / json_parse 保证了排序口径
             *    （jsonb.h 里写死了）—— 口径变了，这里必须跟着改。
             */
            while (lo < hi) {
                mid = lo + (hi - lo) / 2u;
                if (key_cmp(cur->u.obj.keys[mid], cur->u.obj.key_lens[mid],
                            st->name, st->name_len) < 0)
                    lo = mid + 1u;
                else
                    hi = mid;
            }
            if (lo >= cur->u.obj.count)
                return E_NOTFOUND;
            if (key_cmp(cur->u.obj.keys[lo], cur->u.obj.key_lens[lo],
                        st->name, st->name_len) != 0)
                return E_NOTFOUND;
            cur = cur->u.obj.vals[lo];
        } else {
            if (cur->tag != JSONB_ARRAY)
                return E_NOTFOUND;
            if (st->index >= cur->u.arr.count)
                return E_NOTFOUND;
            cur = cur->u.arr.items[st->index];
        }
    }

    *out = cur;
    return OK;
}
