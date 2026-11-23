/*
 * value_record.c —— 值层与记录之间的编解码（L1）
 *
 * ==================== 为什么单独一个文件 ====================
 *
 * `storage/value.c` 承担"值本身"（构造、比较、转字符串、通用函数表），
 * 800 行的硬上限摆在那里 —— 加上记录编解码就超了。
 *
 * 而这两件事本来就该分开：
 *   - 值操作：DbValue 怎么构造、怎么比、怎么打印
 *   - 记录编解码：DbValue[] ↔ tuple buffer 里的 Value[]
 *
 * 依赖方向不变：都在 L1（storage），彼此平等，不反向依赖。
 *
 * ==================== 本文件的核心契约 ====================
 *
 * 变长列（T_TEXT / T_JSON）在 DbValue 里的表示：
 *   - <= VAL_INLINE_MAX(255) ⇒ 内联在 s[]
 *   - 更长 ⇒ 外部值（external = true），ext_fn 负责按需物化
 *
 * ⚠️ 外部 T_TEXT / T_JSON 进记录时**必须先物化**：
 * 外部值的 len 恒为 0、s[] 里没有内容，直接写进去就是空串 ——
 * 静默丢数据，比报错难查得多。
 */

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "storage/record.h"
#include "storage/value.h"


/*
 * 记录变长列 → DbValue。
 *
 * `record.c` 已经把这列字节写进 tuple buffer（小文档内联）。
 *
 * 【为什么 256..2048 要走外部值】
 * DbValue 里只有 255 字节内联空间；把更长的 JSONB 拷进 s[] 会截断 ——
 * 截断 = 静默给错答案。这里用通用外部值描述来源（指向记录缓冲），
 * 需要时再物化，语义上等同于"不截断"。
 * ⚠️ 记录缓冲的生命周期：一次 scan/decode/求值内有效。跨扫描保存的
 * JSON 值必须先复制，或重新绑定到稳定的页链句柄（C5.3b 负责接线）。
 */
static int dbvalue_from_record_var(u8 col_type, const Value *src,
                                   DbValue *out)
{
    if (src->is_null) {
        val_null(out);
        return OK;
    }

    if (col_type == T_TEXT) {
        val_text(out, (const char *)src->u.text.p, src->u.text.n);
        return OK;
    }

    if (col_type == T_JSON) {
        const u8 *bytes = src->u.text.p;
        u32 len = src->u.text.n;
        if (len <= VAL_INLINE_MAX) {
            val_null(out);
            out->is_null = false;
            out->type = T_JSON;
            out->len = (u16)len;
            if (len)
                memcpy(out->s, bytes, len);
            return OK;
        }

        val_external_json(out, (u64)(uintptr_t)bytes, len,
                          val_mem_detoast, (void *)bytes);
        return OK;
    }
    return E_INVAL;
}

static int values_to_dbvalues(const ColDesc *cols, int ncols,
                              const Value *tmp, DbValue *out)
{
    for (int i = 0; i < ncols; i++) {
        if (col_is_var(cols[i].type)) {
            int rc = dbvalue_from_record_var(cols[i].type, &tmp[i], &out[i]);
            if (rc != OK)
                return rc;
            continue;
        }
        if (tmp[i].is_null) {
            val_null(&out[i]);
            continue;
        }
        switch (cols[i].type) {
        case T_BOOL:
            val_bool(&out[i], tmp[i].u.b);
            break;
        case T_INT4:
            val_int(&out[i], tmp[i].u.i4);
            break;
        case T_REAL:
            val_real(&out[i], tmp[i].u.f8);
            break;
        case T_INT8:
        default:
            val_int(&out[i], tmp[i].u.i8);
            break;
        }
    }
    return OK;
}

int val_from_record(const ColDesc *cols, int ncols,
                    const u8 *rec, u16 len, DbValue *out)
{
    Value tmp[REC_MAX_COLS];

    if (ncols > REC_MAX_COLS)
        return E_INVAL;

    int rc = rec_decode(cols, ncols, rec, len, tmp);
    if (rc != OK)
        return rc;

    return values_to_dbvalues(cols, ncols, tmp, out);
}

int val_from_record_layout(const RecLayout *L, const ColDesc *cols, int ncols,
                            const u8 *rec, u16 len, DbValue *out)
{
    Value tmp[REC_MAX_COLS];

    if (ncols > REC_MAX_COLS)
        return E_INVAL;

    int rc = rec_decode_layout(L, cols, rec, len, tmp);
    if (rc != OK)
        return rc;

    return values_to_dbvalues(cols, ncols, tmp, out);
}

int val_to_record(const ColDesc *cols, int ncols, const DbValue *vals,
                  u8 *buf, u16 cap, u16 *out_len)
{
    Value tmp[REC_MAX_COLS];
    /* 外部 JSON 需要先物化成连续字节；物化出的缓冲要在 rec_encode 之后才释放 */
    u8 *ext_bufs[REC_MAX_COLS];
    bool ext_owned[REC_MAX_COLS];
    int rc;

    if (ncols > REC_MAX_COLS)
        return E_INVAL;

    memset(ext_bufs, 0, sizeof(ext_bufs));
    memset(ext_owned, 0, sizeof(ext_owned));
    /*
     * 整个数组一次清零：rec_encode 拿到的是 `tmp`（REC_MAX_COLS 项），
     * 而循环只填前 ncols 项 —— 只清前 ncols 项会让编译器认为
     * 后面那些项未初始化（-Wmaybe-uninitialized）。
     */
    memset(tmp, 0, sizeof(tmp));

    for (int i = 0; i < ncols; i++) {
        if (vals[i].is_null) {
            tmp[i].is_null = true;
            continue;
        }
        switch (cols[i].type) {
        case T_TEXT:
            /*
             * 外部 T_TEXT 必须先物化 —— 否则写进记录的是**空串**。
             *
             * 【为什么现在才暴露】
             * 外部 T_TEXT 的 len 恒为 0（协议是"长度靠 val_detoast 问"），
             * s[] 里也没有内容。以前 SQL 字面量最长 255B，这条分支碰不到；
             * 词法器放宽到 8192B 之后，长字面量一律是外部值 ⇒ 必须物化。
             *
             * 与 T_JSON 分支同一套记账（ext_bufs / ext_owned）。
             */
            if (vals[i].external) {
                u8 *bytes = NULL;
                u32 blen = 0;
                rc = val_detoast(&vals[i], &bytes, &blen);
                if (rc != OK) {
                    for (int k = 0; k < i; k++) {
                        if (ext_owned[k])
                            free(ext_bufs[k]);
                    }
                    return rc;
                }
                ext_bufs[i] = bytes;
                ext_owned[i] = true;
                tmp[i].u.text.p = bytes;
                tmp[i].u.text.n = (u16)blen;
            } else {
                tmp[i].u.text.p = (const u8 *)vals[i].s;
                tmp[i].u.text.n = vals[i].len;
            }
            break;
        case T_JSON:
            /*
             * T_JSON 与 T_TEXT 共用变长区，区别只在 cols[i].type。
             *
             * 外部 JSON（内部 inline 装不下的）必须先物化成连续字节，
             * 否则写进 record 的会是半截内容 —— 静默丢数据。
             */
            if (vals[i].external) {
                u8 *bytes = NULL;
                u32 blen = 0;
                rc = val_detoast(&vals[i], &bytes, &blen);
                if (rc != OK) {
                    for (int k = 0; k < i; k++) {
                        if (ext_owned[k])
                            free(ext_bufs[k]);
                    }
                    return rc;
                }
                ext_bufs[i] = bytes;
                ext_owned[i] = true;
                tmp[i].u.text.p = bytes;
                tmp[i].u.text.n = (u16)blen;
            } else {
                tmp[i].u.text.p = (const u8 *)vals[i].s;
                tmp[i].u.text.n = vals[i].len;
            }
            break;
        case T_BOOL:
            tmp[i].u.b = vals[i].i != 0;
            break;
        case T_INT4:
            tmp[i].u.i4 = (i32)vals[i].i;
            break;
        case T_REAL:
            tmp[i].u.f8 = vals[i].d;
            break;
        case T_INT8:
        default:
            tmp[i].u.i8 = vals[i].i;
            break;
        }
    }

    rc = rec_encode(cols, ncols, tmp, buf, cap, out_len);

    for (int i = 0; i < ncols; i++) {
        if (ext_owned[i])
            free(ext_bufs[i]);
    }
    return rc;
}
