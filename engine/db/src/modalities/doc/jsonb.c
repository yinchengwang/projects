/*
 * jsonb.c —— JSONB 二进制格式的实现（v1.6 步骤 1 · T13）
 *
 * 契约见 jsonb.h（**不改**那份头文件）。
 *
 * ==================== 本文件负责三件事 ====================
 *
 *   1. **树**（JsonbValue）的构造与释放；
 *   2. **编码**：树 → JSONB 字节流（先算总长、再写）；
 *   3. **解码**：JSONB 字节流 → 树。
 *
 * "**不建树直接取值**"那三个函数（jsonb_obj_get / jsonb_arr_get /
 * jsonb_tag_of）在 **jsonb_get.c** —— 给下游的接口说明也在那份文件里。
 *
 * 文本 → 树的那一半在 json_parse.c（递归下降 parser）。
 *
 * ==================== 键的排序 ====================
 *
 * 对象的键按 **(长度, 字典序)** 排序（jsonb.h 的约定）。
 * 找键时先比长度、再比字节 —— 与 PG 的 JSONB 同一个口径。
 *
 * ⚠️ **为什么不是纯字典序**：纯字典序下 "b" 排在 "aa" 前面，
 *    按长度排时短键在前 ⇒ 扫描时**先比长度就能砍掉一大半**。
 *
 * ==================== 解码的强校验 ====================
 *
 * 解码出来之后，把树**重新编码**一遍，与输入**逐字节**比对：
 *
 *   不一致 ⇒ E_CORRUPT（并把已建的树释放掉，**绝不返回半截的树**）。
 *
 * 这一道校验抓得到：
 *   - 尾随垃圾（"{} x" 那种）
 *   - 计数被改坏（成员数与实际不符）
 *   - tag 被改坏（改成别的类型 ⇒ 重编码的字节不同）
 *   - 键没排序（重编码会排序 ⇒ 字节不同）
 *   - 重复键（建树时同名会被替换 ⇒ 数量对不上）
 *
 * ⚠️ **抓不到的**：字符串/数字**载荷内部**被改了一个字节
 *    （坑 M7-14）—— JSONB **没有校验和**（格式里没有这个位置），
 *    所以"载荷里的一个 bit 翻转"在原理上就检测不出来。
 *    这是**格式的性质**，不是实现的疏漏；要根治得给格式加校验和（改契约）。
 *    测试里对这一条是**如实断言**的：破坏之后要么报错、要么结果与原来不同，
 *    但**绝不**"悄悄给出和原来一模一样的东西"。
 */

#include "modalities/doc/jsonb.h"

#include <stdlib.h>
#include <string.h>

/* 嵌套深度上限（编码与解码共用） */
#define JSONB_MAX_DEPTH 200u

/* ---------------- 树：构造与释放 ---------------- */

static JsonbValue *node_new(JsonbTag tag)
{
    JsonbValue *v = (JsonbValue *)calloc(1, sizeof(JsonbValue));

    if (v) {
        v->tag = tag;
    }
    return v;
}

JsonbValue *jsonb_new_null(void)
{
    return node_new(JSONB_NULL);
}

JsonbValue *jsonb_new_bool(bool b)
{
    return node_new(b ? JSONB_TRUE : JSONB_FALSE);
}

/*
 * 整数：**能放下 int32 就用 int32**（PG 的 JSONB 同一个口径）。
 *
 * 绝大多数整数只占 4 字节，而大整数不丢精度。
 */
JsonbValue *jsonb_new_int(i64 x)
{
    JsonbValue *v;

    if (x >= -2147483648LL && x <= 2147483647LL) {
        v = node_new(JSONB_INT32);
    } else {
        v = node_new(JSONB_INT64);
    }
    if (v) {
        v->u.i = x;
    }
    return v;
}

JsonbValue *jsonb_new_double(double d)
{
    JsonbValue *v = node_new(JSONB_FLOAT64);

    if (v) {
        v->u.d = d;
    }
    return v;
}

/*
 * 字符串：**按长度拷贝**（坑 M7-15）。
 *
 * ⚠️ JSON 的字符串**可以带 NUL 字节**（`\u0000`）⇒
 *    长度必须一路带着走，**绝不能用 strlen**。
 *    本实现里：树里是 (p, len)，字节流里是 [u32 len][bytes]。
 */
JsonbValue *jsonb_new_string(const char *p, u32 len)
{
    JsonbValue *v = node_new(JSONB_STRING);

    if (!v) {
        return NULL;
    }
    v->u.s.p = (const char *)malloc(len ? (size_t)len : 1u);
    if (!v->u.s.p) {
        free(v);
        return NULL;
    }
    if (len && p) {
        memcpy((void *)v->u.s.p, p, len);
    }
    v->u.s.len = len;
    return v;
}

JsonbValue *jsonb_new_array(void)
{
    return node_new(JSONB_ARRAY);
}

JsonbValue *jsonb_new_object(void)
{
    return node_new(JSONB_OBJECT);
}

/*
 * 数组追加（接管 item 的所有权）。
 *
 * ⚠️ **O(n) 的拷贝**：契约里的 JsonbValue **没有"容量"字段**，
 *    所以每次追加都要把数组**精确**扩一位（realloc）。
 *    ⇒ 建一个 n 元素的数组是 O(n²) 的拷贝量。
 *
 *    这与对象插入是 O(n) 的移动**同一个量级**（契约里也承认了
 *    "对象通常很小"这个取舍）⇒ 这里保持一致，不引入隐藏的容量字段
 *    （隐藏字段会让"手工构造的 JsonbValue"变成未定义行为，坑 M7-16）。
 */
int jsonb_arr_push(JsonbValue *arr, JsonbValue *item)
{
    JsonbValue **nv;
    u32 n;

    if (!arr || !item) {
        return E_INVAL;
    }
    if (arr->tag != JSONB_ARRAY) {
        return E_INVAL;
    }

    n = arr->u.arr.count;
    nv = (JsonbValue **)realloc(arr->u.arr.items,
                                sizeof(JsonbValue *) * (size_t)(n + 1u));
    if (!nv) {
        return E_NOSPACE;
    }
    arr->u.arr.items = nv;
    arr->u.arr.items[n] = item;
    arr->u.arr.count = n + 1u;
    return OK;
}

/* 键比较：先比长度、再比字节（0 = 相等） */
static int key_cmp(const char *a, u32 alen, const char *b, u32 blen)
{
    int c;

    if (alen != blen) {
        return (alen < blen) ? -1 : 1;
    }
    c = (alen == 0u) ? 0 : memcmp(a, b, alen);
    return (c < 0) ? -1 : ((c > 0) ? 1 : 0);
}

/*
 * 对象设键（**替换**已存在的同名键）。
 *
 * 键按 (长度, 字典序) 排序 ⇒ 插入要移动后面的成员。
 * 键的字节**拷贝**一份（调用方的缓冲可能比树活得短）。
 */
int jsonb_obj_set(JsonbValue *obj, const char *key, u32 klen, JsonbValue *val)
{
    const char **nk;
    u32 *nl;
    JsonbValue **nv;
    char *kcopy;
    u32 n;
    u32 pos;
    u32 i;

    if (!obj || !val) {
        return E_INVAL;
    }
    if (obj->tag != JSONB_OBJECT) {
        return E_INVAL;
    }
    if (!key && klen != 0u) {
        return E_INVAL;
    }

    n = obj->u.obj.count;
    pos = n;
    for (i = 0; i < n; i++) {
        int c = key_cmp(obj->u.obj.keys[i], obj->u.obj.key_lens[i], key, klen);

        if (c == 0) {
            /* 同名 ⇒ 替换（键的字节相同，键本身不用换） */
            jsonb_free(obj->u.obj.vals[i]);
            obj->u.obj.vals[i] = val;
            return OK;
        }
        if (c > 0) {
            pos = i;              /* 插在它前面 */
            break;
        }
    }

    /* 三张表各扩一位 */
    nk = (const char **)realloc(obj->u.obj.keys, sizeof(char *) * (size_t)(n + 1u));
    if (!nk) {
        return E_NOSPACE;
    }
    obj->u.obj.keys = nk;

    nl = (u32 *)realloc(obj->u.obj.key_lens, sizeof(u32) * (size_t)(n + 1u));
    if (!nl) {
        return E_NOSPACE;
    }
    obj->u.obj.key_lens = nl;

    nv = (JsonbValue **)realloc(obj->u.obj.vals, sizeof(JsonbValue *) * (size_t)(n + 1u));
    if (!nv) {
        return E_NOSPACE;
    }
    obj->u.obj.vals = nv;

    /*
     * 键的字节拷一份。
     *
     * ⚠️ **多拷一个字节放结尾的 NUL**（坑 M7-20）：
     *    树里的键既有 key_lens（长度权威），又保证**以 NUL 结尾** ——
     *    下游要是拿 const char * 直接 printf("%s") 或用 strcmp，
     *    没有这个 NUL 就会读出越界的垃圾（这个坑真的踩过）。
     *    ⚠️ 键**可能**含 NUL 字节（"\u0000"）⇒ 判断键相等仍必须用
     *       key_lens + memcmp，**不能用 strlen**。
     */
    kcopy = (char *)malloc((size_t)klen + 1u);
    if (!kcopy) {
        return E_NOSPACE;
    }
    if (klen) {
        memcpy(kcopy, key, klen);
    }
    kcopy[klen] = '\0';

    /* 把 pos 之后的成员往后挪一位 */
    if (pos < n) {
        memmove(&obj->u.obj.keys[pos + 1u], &obj->u.obj.keys[pos],
                sizeof(char *) * (size_t)(n - pos));
        memmove(&obj->u.obj.key_lens[pos + 1u], &obj->u.obj.key_lens[pos],
                sizeof(u32) * (size_t)(n - pos));
        memmove(&obj->u.obj.vals[pos + 1u], &obj->u.obj.vals[pos],
                sizeof(JsonbValue *) * (size_t)(n - pos));
    }
    obj->u.obj.keys[pos] = kcopy;
    obj->u.obj.key_lens[pos] = klen;
    obj->u.obj.vals[pos] = val;
    obj->u.obj.count = n + 1u;
    return OK;
}

void jsonb_free(JsonbValue *v)
{
    u32 i;

    if (!v) {
        return;
    }
    switch (v->tag) {
    case JSONB_STRING:
        free((void *)v->u.s.p);
        break;
    case JSONB_ARRAY:
        for (i = 0; i < v->u.arr.count; i++) {
            jsonb_free(v->u.arr.items[i]);
        }
        free(v->u.arr.items);
        break;
    case JSONB_OBJECT:
        for (i = 0; i < v->u.obj.count; i++) {
            free((void *)v->u.obj.keys[i]);
            jsonb_free(v->u.obj.vals[i]);
        }
        free(v->u.obj.keys);
        free(v->u.obj.key_lens);
        free(v->u.obj.vals);
        break;
    default:
        break;              /* 标量没有可释放的东西 */
    }
    free(v);
}

/* ---------------- 编码 ---------------- */

/*
 * 一个值的编码长度。
 *
 * 布局（jsonb.h 的约定）：
 *   [tag][payload]
 *     int32    ⇒ 4 字节
 *     int64    ⇒ 8 字节
 *     float64  ⇒ 8 字节
 *     string   ⇒ [u32 len][bytes]
 *     array    ⇒ [u32 count][元素]×
 *     object   ⇒ [u32 count][[u32 klen][key][值]]×
 */
static int val_encoded_len(const JsonbValue *v, u32 depth, u32 *out)
{
    u32 total = 1u;         /* tag */
    u32 i;

    if (!v) {
        return E_INVAL;
    }
    if (depth > JSONB_MAX_DEPTH) {
        return E_INVAL;     /* 太深：编码/解码都拒绝（防爆栈） */
    }

    switch (v->tag) {
    case JSONB_NULL:
    case JSONB_TRUE:
    case JSONB_FALSE:
        break;
    case JSONB_INT32:
        total += 4u;
        break;
    case JSONB_INT64:
    case JSONB_FLOAT64:
        total += 8u;
        break;
    case JSONB_STRING:
        total += 4u + v->u.s.len;
        break;
    case JSONB_ARRAY:
        total += 4u;
        for (i = 0; i < v->u.arr.count; i++) {
            u32 sub = 0;
            int rc = val_encoded_len(v->u.arr.items[i], depth + 1u, &sub);

            if (rc != OK) {
                return rc;
            }
            total += sub;
        }
        break;
    case JSONB_OBJECT:
        total += 4u;
        for (i = 0; i < v->u.obj.count; i++) {
            u32 sub = 0;
            int rc = val_encoded_len(v->u.obj.vals[i], depth + 1u, &sub);

            if (rc != OK) {
                return rc;
            }
            total += 4u + v->u.obj.key_lens[i] + sub;
        }
        break;
    default:
        return E_CORRUPT;
    }

    *out = total;
    return OK;
}

/* 把一个值写进 out[*off..]，并把 *off 推进到写完的位置 */
static int val_write(const JsonbValue *v, u8 *out, u32 *off, u32 depth)
{
    u32 i;

    if (!v) {
        return E_INVAL;
    }
    if (depth > JSONB_MAX_DEPTH) {
        return E_INVAL;
    }

    out[*off] = (u8)v->tag;
    (*off)++;

    switch (v->tag) {
    case JSONB_NULL:
    case JSONB_TRUE:
    case JSONB_FALSE:
        break;
    case JSONB_INT32:
        put_u32(out + *off, (u32)(i32)v->u.i);
        *off += 4u;
        break;
    case JSONB_INT64: {
        u64 bits;

        memcpy(&bits, &v->u.i, sizeof(bits));
        put_u64(out + *off, bits);
        *off += 8u;
        break;
    }
    case JSONB_FLOAT64: {
        u64 bits;

        memcpy(&bits, &v->u.d, sizeof(bits));
        put_u64(out + *off, bits);
        *off += 8u;
        break;
    }
    case JSONB_STRING:
        put_u32(out + *off, v->u.s.len);
        *off += 4u;
        if (v->u.s.len) {
            memcpy(out + *off, v->u.s.p, v->u.s.len);
        }
        *off += v->u.s.len;
        break;
    case JSONB_ARRAY:
        put_u32(out + *off, v->u.arr.count);
        *off += 4u;
        for (i = 0; i < v->u.arr.count; i++) {
            int rc = val_write(v->u.arr.items[i], out, off, depth + 1u);

            if (rc != OK) {
                return rc;
            }
        }
        break;
    case JSONB_OBJECT:
        put_u32(out + *off, v->u.obj.count);
        *off += 4u;
        for (i = 0; i < v->u.obj.count; i++) {
            int rc;

            put_u32(out + *off, v->u.obj.key_lens[i]);
            *off += 4u;
            if (v->u.obj.key_lens[i]) {
                memcpy(out + *off, v->u.obj.keys[i], v->u.obj.key_lens[i]);
            }
            *off += v->u.obj.key_lens[i];
            rc = val_write(v->u.obj.vals[i], out, off, depth + 1u);
            if (rc != OK) {
                return rc;
            }
        }
        break;
    default:
        return E_CORRUPT;
    }
    return OK;
}

/*
 * 编码：树 → JSONB 字节流。
 *
 * out == NULL ⇒ 只算长度（*out_len）。
 * 缓冲不够 ⇒ E_NOSPACE，且**一个字节都不写**（先算总长再写）。
 */
int jsonb_encode(const JsonbValue *v, u8 *out, u32 cap, u32 *out_len)
{
    u32 need = 0;
    u32 off = 0;
    int rc;

    if (!v || !out_len) {
        return E_INVAL;
    }

    rc = val_encoded_len(v, 0u, &need);
    if (rc != OK) {
        return rc;
    }
    *out_len = need;

    if (!out) {
        return OK;           /* 只算长度 */
    }
    if (cap < need) {
        return E_NOSPACE;    /* 不写任何东西 */
    }

    rc = val_write(v, out, &off, 0u);
    if (rc != OK) {
        return rc;
    }
    return (off == need) ? OK : E_CORRUPT;
}

/* ---------------- 解码 ---------------- */

/*
 * 解一个值：buf[*pos] 起，*pos 推进到值的末尾。
 *
 * 任何不合法的结构 ⇒ E_CORRUPT（调用方负责释放已建的部分）。
 */
static int dec_value(const u8 *buf, u32 len, u32 *pos, u32 depth,
                     JsonbValue **out)
{
    JsonbValue *v = NULL;
    u8 tag;

    if (depth > JSONB_MAX_DEPTH) {
        return E_CORRUPT;    /* 太深：拒绝（防爆栈） */
    }
    if (*pos >= len) {
        return E_CORRUPT;
    }

    tag = buf[*pos];
    (*pos)++;

    switch (tag) {
    case JSONB_NULL:
    case JSONB_TRUE:
    case JSONB_FALSE:
        v = node_new((JsonbTag)tag);
        break;

    case JSONB_INT32: {
        i32 x;

        if (len - *pos < 4u) {
            return E_CORRUPT;
        }
        x = (i32)get_u32(buf + *pos);
        *pos += 4u;
        v = node_new(JSONB_INT32);
        if (v) {
            v->u.i = (i64)x;
        }
        break;
    }

    case JSONB_INT64: {
        u64 bits;
        i64 x;

        if (len - *pos < 8u) {
            return E_CORRUPT;
        }
        bits = get_u64(buf + *pos);
        *pos += 8u;
        memcpy(&x, &bits, sizeof(x));
        v = node_new(JSONB_INT64);
        if (v) {
            v->u.i = x;
        }
        break;
    }

    case JSONB_FLOAT64: {
        u64 bits;
        double d;

        if (len - *pos < 8u) {
            return E_CORRUPT;
        }
        bits = get_u64(buf + *pos);
        *pos += 8u;
        memcpy(&d, &bits, sizeof(d));
        v = node_new(JSONB_FLOAT64);
        if (v) {
            v->u.d = d;
        }
        break;
    }

    case JSONB_STRING: {
        u32 slen;
        char *p;

        if (len - *pos < 4u) {
            return E_CORRUPT;
        }
        slen = get_u32(buf + *pos);
        *pos += 4u;
        if (len - *pos < slen) {
            return E_CORRUPT;
        }
        p = (char *)malloc(slen ? (size_t)slen : 1u);
        if (!p) {
            return E_NOSPACE;
        }
        if (slen) {
            memcpy(p, buf + *pos, slen);
        }
        *pos += slen;
        v = node_new(JSONB_STRING);
        if (!v) {
            free(p);
            return E_NOSPACE;
        }
        v->u.s.p = p;
        v->u.s.len = slen;
        break;
    }

    case JSONB_ARRAY: {
        u32 count;
        u32 i;

        if (len - *pos < 4u) {
            return E_CORRUPT;
        }
        count = get_u32(buf + *pos);
        *pos += 4u;
        v = node_new(JSONB_ARRAY);
        if (!v) {
            return E_NOSPACE;
        }
        for (i = 0; i < count; i++) {
            JsonbValue *item = NULL;
            int rc = dec_value(buf, len, pos, depth + 1u, &item);

            if (rc != OK) {
                jsonb_free(v);
                return rc;
            }
            rc = jsonb_arr_push(v, item);
            if (rc != OK) {
                jsonb_free(item);
                jsonb_free(v);
                return rc;
            }
        }
        break;
    }

    case JSONB_OBJECT: {
        u32 count;
        u32 i;

        if (len - *pos < 4u) {
            return E_CORRUPT;
        }
        count = get_u32(buf + *pos);
        *pos += 4u;
        v = node_new(JSONB_OBJECT);
        if (!v) {
            return E_NOSPACE;
        }
        for (i = 0; i < count; i++) {
            u32 klen;
            u32 kpos;
            JsonbValue *item = NULL;
            int rc;

            if (len - *pos < 4u) {
                jsonb_free(v);
                return E_CORRUPT;
            }
            klen = get_u32(buf + *pos);
            *pos += 4u;
            if (len - *pos < klen) {
                jsonb_free(v);
                return E_CORRUPT;
            }
            kpos = *pos;          /* 键的起点（解值之后 pos 会跑掉） */
            *pos += klen;         /* ⚠️ 跳过键本身 —— **值**在键之后 */
            rc = dec_value(buf, len, pos, depth + 1u, &item);
            if (rc != OK) {
                jsonb_free(v);
                return rc;
            }
            rc = jsonb_obj_set(v, (const char *)(buf + kpos), klen, item);
            if (rc != OK) {
                jsonb_free(item);
                jsonb_free(v);
                return rc;
            }
        }
        break;
    }

    default:
        return E_CORRUPT;    /* 不认识的 tag */
    }

    if (!v) {
        return E_NOSPACE;
    }
    *out = v;
    return OK;
}

/*
 * 解码：JSONB 字节流 → 树。
 *
 * ⚠️ **绝不返回半截的树**：任何一步不对都释放掉已建的部分并返回错误码。
 *
 * ⚠️ **额外的强校验**：解完之后把树**重新编码**一遍，与输入逐字节比对。
 *    不一致 ⇒ E_CORRUPT（理由见文件头的说明）。
 */
int jsonb_decode(const u8 *buf, u32 len, JsonbValue **out)
{
    JsonbValue *tree = NULL;
    u8 *re = NULL;
    u32 pos = 0;
    u32 need = 0;
    u32 written = 0;
    int rc;

    if (!buf || !out) {
        return E_INVAL;
    }
    *out = NULL;
    if (len == 0u) {
        return E_CORRUPT;
    }

    rc = dec_value(buf, len, &pos, 0u, &tree);
    if (rc != OK) {
        return rc;
    }

    /* 尾随垃圾 ⇒ 损坏 */
    if (pos != len) {
        jsonb_free(tree);
        return E_CORRUPT;
    }

    /* 强校验：重新编码，与输入逐字节比对 */
    rc = jsonb_encode(tree, NULL, 0u, &need);
    if (rc != OK || need != len) {
        jsonb_free(tree);
        return E_CORRUPT;
    }
    re = (u8 *)malloc(need ? need : 1u);
    if (!re) {
        jsonb_free(tree);
        return E_NOSPACE;
    }
    rc = jsonb_encode(tree, re, need, &written);
    if (rc != OK || written != len || memcmp(re, buf, len) != 0) {
        free(re);
        jsonb_free(tree);
        return E_CORRUPT;
    }
    free(re);

    *out = tree;
    return OK;
}
