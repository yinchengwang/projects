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
                                   void *decode_ctx, DbValue *out)
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

        /*
         * ============ 先看是不是页链标记 ============
         *
         * 12 字节、tag 0x4A534F56、首字节 0x56 不在合法 JSONB tag（0..8）内
         * ⇒ 字节层面不可能是"恰好长得像标记的合法 JSONB"。
         *
         * 必须在"长度 <= VAL_INLINE_MAX ⇒ 内联"这条判断**之前**分流：
         * 标记本身只有 12 字节，会掉进内联分支 ——
         * 那样用户拿到的"文档"就是 12 个字节的二进制垃圾。
         */
        if (rec_json_ovf_is_marker(bytes, len)) {
            u32 head = 0;
            u32 chain_len = 0;
            DetoastFn fn;

            int mrc = rec_json_ovf_decode(bytes, len, &head, &chain_len);
            if (mrc != OK)
                return mrc;

            fn = val_json_chain_detoast();
            if (fn == NULL)
                return E_NOTSUP;   /* doc 模态没注册 ⇒ 明确报错 */
            if (decode_ctx == NULL)
                return E_NOTSUP;   /* 调用方没给读链的上下文 */

            /*
             * ext_off 一定是 chain_len —— 这是"只走一趟"的支点。
             * 见 json_ovf_detoast 里对 off 的说明。
             */
            val_external_json_kind(out, (u64)head, (u64)chain_len,
                                   fn, decode_ctx,
                                   (u8)VAL_EXT_PAGE_CHAIN);
            return OK;
        }

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
                              const Value *tmp, void *decode_ctx,
                              DbValue *out)
{
    for (int i = 0; i < ncols; i++) {
        if (col_is_var(cols[i].type)) {
            int rc = dbvalue_from_record_var(cols[i].type, &tmp[i],
                                             decode_ctx, &out[i]);
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

int val_from_record_ctx(const ColDesc *cols, int ncols,
                        const u8 *rec, u16 len, void *decode_ctx,
                        DbValue *out)
{
    Value tmp[REC_MAX_COLS];

    if (ncols > REC_MAX_COLS)
        return E_INVAL;

    int rc = rec_decode(cols, ncols, rec, len, tmp);
    if (rc != OK)
        return rc;

    return values_to_dbvalues(cols, ncols, tmp, decode_ctx, out);
}

int val_from_record_layout_ctx(const RecLayout *L, const ColDesc *cols,
                               int ncols, const u8 *rec, u16 len,
                               void *decode_ctx, DbValue *out)
{
    Value tmp[REC_MAX_COLS];

    if (ncols > REC_MAX_COLS)
        return E_INVAL;

    int rc = rec_decode_layout(L, cols, rec, len, tmp);
    if (rc != OK)
        return rc;

    return values_to_dbvalues(cols, ncols, tmp, decode_ctx, out);
}

/*
 * 老入口保留为"没有读链上下文"的薄封装。
 *
 * 遇到页链标记时它们返回 E_NOTSUP —— **明确报错**，
 * 而不是给出一个 ext_ctx == NULL、一读就崩的值。
 *
 * 为什么不让它们也走 Pager：这两个函数只拿到 (rec, schema)，
 * 拿 Pager 的唯一办法是让调用方传进来 —— 那就是 _ctx 版本。
 */
int val_from_record(const ColDesc *cols, int ncols,
                    const u8 *rec, u16 len, DbValue *out)
{
    return val_from_record_ctx(cols, ncols, rec, len, NULL, out);
}

int val_from_record_layout(const RecLayout *L, const ColDesc *cols, int ncols,
                            const u8 *rec, u16 len, DbValue *out)
{
    return val_from_record_layout_ctx(L, cols, ncols, rec, len, NULL, out);
}

int val_to_record(const ColDesc *cols, int ncols, const DbValue *vals,
                  u8 *buf, u16 cap, u16 *out_len)
{
    Value tmp[REC_MAX_COLS];
    /* 外部 JSON 需要先物化成连续字节；物化出的缓冲要在 rec_encode 之后才释放 */
    u8 *ext_bufs[REC_MAX_COLS];
    bool ext_owned[REC_MAX_COLS];
    /*
     * 页链标记的暂存区（每列 12 字节）。
     *
     * 【为什么不用栈上单个缓冲复用】
     * rec_encode 在循环**结束后**才读 tmp[i].u.text.p ——
     * 所有列的标记必须同时有效。复用一块会让前面的列全指向同一处。
     */
    u8 marker_bufs[REC_MAX_COLS][REC_JSON_OVF_MARK_SIZE];
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
             * ============ 页链值：写 12 字节标记，不物化 ============
             *
             * 【为什么绝不能在这里物化】
             * 两个理由，任一个都足够致命：
             *
             * ① 静默截断：tmp[i].u.text.n 是 u16，而链最长 65535 ——
             *    只要实际长度不超上限倒是装得下，但**物化本身**要读整条链、
             *    拷 64KB 进 tuple，而 tuple 各层长度都是 u16、页只有 4KB
             *    ⇒ rec_encode 会 E_NOSPACE。用户看到的是"插不进去"，
             *    而我们明明有页链可以用。
             *
             * ② 语义错位：页链存在的意义就是"内容不进 tuple"。
             *    物化之后再存回 tuple，等于把页链白建了一遍。
             *
             * 所以这里只写标记：tag + 链首页 + 链长度。
             */
            if (vals[i].external &&
                vals[i].ext_kind == (u8)VAL_EXT_PAGE_CHAIN) {
                u8 *mark = marker_bufs[i];

                rc = rec_json_ovf_encode((u32)vals[i].ext_key,
                                         (u32)vals[i].ext_off,
                                         mark, REC_JSON_OVF_MARK_SIZE);
                if (rc != OK) {
                    for (int k = 0; k < i; k++) {
                        if (ext_owned[k])
                            free(ext_bufs[k]);
                    }
                    return rc;
                }
                tmp[i].u.text.p = mark;
                tmp[i].u.text.n = (u16)REC_JSON_OVF_MARK_SIZE;
                break;
            }

            /*
             * 其余外部 JSON（256..2048B 的临时外部值）必须先物化成连续字节，
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
