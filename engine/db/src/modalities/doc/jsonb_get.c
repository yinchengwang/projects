/*
 * jsonb_get.c —— JSONB 的"**不建树直接取值**"（v1.6 步骤 1 · T13）
 *
 * 契约见 jsonb.h（**不改**那份头文件）。
 *
 * ==================== 这一层解决什么 ====================
 *
 * jsonb 相对裸 JSON 文本的**真实收益**就在这里：
 * 查 `$.a.b` **不必把整棵树建出来** ——
 * 直接在字节流上跳着走，跳到要的那个值就停。
 *
 * 对大文档尤其重要：一棵树是几十 KB 的堆内存，
 * 而这里全程**零分配**。
 *
 * ==================== ⭐ 给下游（步骤 2 的路径提取）的接口说明 ====================
 *
 * ⚠️ **"取值"这三个函数返回的是什么**（这是下游最需要知道的一条）：
 *
 *   jsonb_obj_get / jsonb_arr_get 返回的 val / val_len 指向
 *   **那个值的完整编码** —— 也就是 **tag 字节打头、后面跟 payload**。
 *
 *   所以：
 *     val[0]        ⇒ tag（可以直接 jsonb_tag_of(val, val_len, &tag)）
 *     val_len       ⇒ tag + payload 的总长度
 *
 *   ⚠️ **不是**"payload 的起点"、**不是**"原始文本" ——
 *      下游拿它去判类型时必须带上 tag，否则会把 int32 的头字节当成数值。
 *
 *   例：buf 里存着 {"a":1}，则 jsonb_obj_get 找到的 val 指向
 *       [03][01 00 00 00]（tag=INT32，值=1），val_len = 5。
 *
 *   另：值**不是对象**时 jsonb_obj_get 返回 **E_INVAL**（不是 E_NOTFOUND）——
 *       "这里没有这个成员"与"这里根本不是对象"是两回事，别混。
 *
 * ==================== 查找的复杂度：线性扫描，不是二分 ====================
 *
 * jsonb.h 的注释里说"按路径取值是两次二分"。**本实现是线性扫描** ——
 * 这不是偷懒，是**格式本身的限制**（坑 M7-13）：
 *
 *   对象的成员是变长的（[u32 klen][key][值]）⇒ 第 i 个成员的**偏移不是常数**
 *   ⇒ 没有偏移表就没法 O(log n) 地跳到第 i 个成员。
 *
 *   折中：扫描时**先比键长**（一次 u32 读），长度不等就直接跳过整个成员
 *   —— 比"逐字节比"快很多，但最坏情况仍是 O(成员数)。
 *
 *   要真正的二分，得给格式加一张偏移表 —— 那是**改契约**，本轮不做。
 *   对下游的影响：接口不变，只是"大对象 + 短路径"的常数大了一点。
 *
 * ==================== 跳过语义 ====================
 *
 * 扫描时要"跳过"一个不关心的值 —— skip_value() 走一遍结构算出它的长度。
 * ⚠️ 它**只做结构校验**（长度够不够、tag 认不认识），**不分配内存**。
 *
 * ⚠️ 字节流被破坏时，这三个取值函数可能返回 E_CORRUPT ——
 *    它们是**防御性**的，不是"随便读读"。
 */

#include "modalities/doc/jsonb.h"

#include <string.h>

/* 嵌套深度上限（与 jsonb.c 一致：解码侧的防线，这里是遍历侧的） */
#define JSONB_GET_MAX_DEPTH 200u

/* ---------------- 不建树直接取值 ---------------- */

/*
 * 跳过一个值：算出它在 buf 里的编码长度，并推进 *pos。
 *
 * 用于"扫描成员时跳过不关心的那个值"。
 */
static int skip_value(const u8 *buf, u32 len, u32 *pos, u32 depth)
{
    u8 tag;
    u32 count;
    u32 i;

    if (depth > JSONB_GET_MAX_DEPTH) {
        return E_CORRUPT;
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
        return OK;
    case JSONB_INT32:
        if (len - *pos < 4u) {
            return E_CORRUPT;
        }
        *pos += 4u;
        return OK;
    case JSONB_INT64:
    case JSONB_FLOAT64:
        if (len - *pos < 8u) {
            return E_CORRUPT;
        }
        *pos += 8u;
        return OK;
    case JSONB_STRING:
        if (len - *pos < 4u) {
            return E_CORRUPT;
        }
        count = get_u32(buf + *pos);
        *pos += 4u;
        if (len - *pos < count) {
            return E_CORRUPT;
        }
        *pos += count;
        return OK;
    case JSONB_ARRAY:
        if (len - *pos < 4u) {
            return E_CORRUPT;
        }
        count = get_u32(buf + *pos);
        *pos += 4u;
        for (i = 0; i < count; i++) {
            int rc = skip_value(buf, len, pos, depth + 1u);

            if (rc != OK) {
                return rc;
            }
        }
        return OK;
    case JSONB_OBJECT:
        if (len - *pos < 4u) {
            return E_CORRUPT;
        }
        count = get_u32(buf + *pos);
        *pos += 4u;
        for (i = 0; i < count; i++) {
            u32 klen;
            int rc;

            if (len - *pos < 4u) {
                return E_CORRUPT;
            }
            klen = get_u32(buf + *pos);
            *pos += 4u;
            if (len - *pos < klen) {
                return E_CORRUPT;
            }
            *pos += klen;
            rc = skip_value(buf, len, pos, depth + 1u);
            if (rc != OK) {
                return rc;
            }
        }
        return OK;
    default:
        return E_CORRUPT;
    }
}

/*
 * 取对象的一个成员（**线性扫描**：理由见文件头）。
 *
 * 找到 ⇒ val 指向**那个值的完整编码**（tag 打头），val_len 是它的总长。
 * 找不到 ⇒ E_NOTFOUND。
 * 值不是对象 ⇒ E_INVAL（"这里根本不是对象" ≠ "这里没有这个成员"）。
 */
int jsonb_obj_get(const u8 *buf, u32 len, const char *key, u32 klen,
                  const u8 **val, u32 *val_len)
{
    u32 count;
    u32 i;
    u32 pos;

    if (!buf || !val || !val_len || (!key && klen != 0u)) {
        return E_INVAL;
    }
    *val = NULL;
    *val_len = 0u;

    if (len == 0u || buf[0] != (u8)JSONB_OBJECT) {
        return E_INVAL;       /* 不是对象 */
    }

    pos = 1u;
    if (len - pos < 4u) {
        return E_CORRUPT;
    }
    count = get_u32(buf + pos);
    pos += 4u;

    for (i = 0; i < count; i++) {
        u32 klen2;
        u32 vstart;

        if (len - pos < 4u) {
            return E_CORRUPT;
        }
        klen2 = get_u32(buf + pos);
        pos += 4u;
        if (len - pos < klen2) {
            return E_CORRUPT;
        }

        /* 先比长度（长度不等就不必比字节） */
        if (klen2 == klen &&
            (klen == 0u || memcmp(buf + pos, key, klen) == 0)) {
            vstart = pos + klen2;
            *val = buf + vstart;
            /* 值的长度：从 tag 开始算 */
            {
                u32 p = vstart;
                u32 vlen = 0;
                int rc = skip_value(buf, len, &p, 0u);

                if (rc != OK) {
                    return rc;
                }
                vlen = p - vstart;
                *val_len = vlen;
            }
            return OK;
        }

        /* 不是它 ⇒ 跳过这个成员 */
        pos += klen2;
        {
            int rc = skip_value(buf, len, &pos, 0u);

            if (rc != OK) {
                return rc;
            }
        }
    }

    return E_NOTFOUND;
}

/* 取数组的第 i 个元素（越界 ⇒ E_NOTFOUND；不是数组 ⇒ E_INVAL） */
int jsonb_arr_get(const u8 *buf, u32 len, u32 idx, const u8 **val, u32 *val_len)
{
    u32 count;
    u32 i;
    u32 pos;

    if (!buf || !val || !val_len) {
        return E_INVAL;
    }
    *val = NULL;
    *val_len = 0u;

    if (len == 0u || buf[0] != (u8)JSONB_ARRAY) {
        return E_INVAL;
    }

    pos = 1u;
    if (len - pos < 4u) {
        return E_CORRUPT;
    }
    count = get_u32(buf + pos);
    pos += 4u;

    if (idx >= count) {
        return E_NOTFOUND;
    }

    for (i = 0; i < idx; i++) {
        int rc = skip_value(buf, len, &pos, 0u);

        if (rc != OK) {
            return rc;
        }
    }

    *val = buf + pos;
    {
        u32 p = pos;
        u32 vlen = 0;
        int rc = skip_value(buf, len, &p, 0u);

        if (rc != OK) {
            return rc;
        }
        vlen = p - pos;
        *val_len = vlen;
    }
    return OK;
}

/* 取一个值的 tag（buf/len 指向一个值的**完整编码**） */
int jsonb_tag_of(const u8 *buf, u32 len, JsonbTag *tag)
{
    if (!buf || !tag) {
        return E_INVAL;
    }
    if (len < 1u) {
        return E_CORRUPT;
    }
    if (buf[0] > (u8)JSONB_OBJECT) {
        return E_CORRUPT;      /* 不认识的 tag */
    }
    *tag = (JsonbTag)buf[0];
    return OK;
}
