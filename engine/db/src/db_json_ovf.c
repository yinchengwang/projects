/*
 * db_json_ovf.c —— JSON 列的"文本 → 可写值"（C5.3b-b 接线层）
 *
 * ==================== 这一层解决什么 ====================
 *
 * 前几步已经做好了零件：
 *   - json_ovf_write / read / free（页链本体 + WAL + 幂等）
 *   - record 的 12 字节溢出标记（record_json_ovf.c）
 *   - 值层的注册表（编码器 / 渲染器 / 页链 detoast / 归还 / 内联上限）
 *
 * 还差一件事：**把一列 JSON 文本变成可以写进 tuple 的值**。
 * 这个动作要编码、要按阈值选存放方式、超过阈值还要建页链 ——
 * 而建页链需要 Pager + xid + WAL 钩子，其中 WAL 钩子的 ctx 是 DbInstance。
 *
 * ==================== 为什么这个文件在 db 层 ====================
 *
 * `modalities/doc/` 是 L4，按分层铁律**看不到** DbInstance（db 层内部结构）。
 * `storage/` 是 L1，同样看不到。而建链必须拿到 DbInstance 才能写 WAL ——
 * 因为 WAL 钩子的 ctx 必须是实例（要活得比任何连接都久，见 db_internal.h）。
 *
 * 所以整个"文本 → 可写值"的动作由本文件实现，注册进值层。
 * 这与 db_vec.c / db_geo.c 是同一个先例：那些文件也都在 db 层，
 * 作用就是"把模态装进实例"。
 *
 * 查询层只调 val_json_to_column 这一个入口，**完全不需要知道页链存在**。
 *
 * ==================== 建链的时机（设计 §5.1） ====================
 *
 * 建链在 tuple 写进 heap **之前**：tuple 里要存链首页页号，
 * 而页号只有写完才知道。
 *
 * 于是留下"链已建、tuple 还没写"的窗口。那个窗口里的孤儿由启动扫描回收
 * （设计 §5.2 / §8）—— 这是有意接受的取舍。
 *
 * ⚠️ 运行时 abort **不还链**。那些页已经进过 WAL；还回空闲栈后
 *    别的结构可能立刻分配到并写入已提交数据，而崩溃恢复会 undo 本事务、
 *    把 PT_FREE 前像盖上去 ⇒ 抹掉新主人的数据。
 */

#include "db_internal.h"

#include <stdlib.h>
#include <string.h>

#include "modalities/doc/json_ovf.h"
#include "storage/pager.h"

/*
 * 把 JSONB 字节写成页链。
 *
 * 这是本文件的核心：只有这里同时握有 Pager（页从哪分配）、
 * xid（日志归哪个事务）、DbInstance（WAL 钩子的 ctx）。
 */
static int ovf_store(DbInstance *inst, u32 xid,
                     const u8 *jsonb, u32 len, u64 *head_out)
{
    JsonOvfWal wal;
    u64 head = PAGE_ID_NIL;
    int rc;

    wal.fn = db_wal_hook;
    wal.ctx = inst;          /* 钩子的 ctx 必须是实例，不是 Pager */
    wal.xid = xid;

    rc = json_ovf_write(inst->pager, &wal, jsonb, len, &head);
    if (rc != OK)
        return rc;

    if (head == PAGE_ID_NIL)
        return E_CORRUPT;    /* 写了却拿不到首页 ⇒ 内部 bug */

    *head_out = head;
    return OK;
}

/*
 * 值层注册的实现：一列 JSON 文本 → 可写进 tuple 的值。
 *
 * 三档存放：
 *   <= VAL_INLINE_MAX(255)        内联在 s[]（不付堆代价）
 *   <= inline_max（模态给的阈值）  堆缓冲 + VAL_EXT_MEM
 *   >  inline_max                页链句柄 + VAL_EXT_PAGE_CHAIN
 *
 * 【为什么阈值从值层问，而不是这里写死 2048】
 * 阈值属于文档模态（JSON_INLINE_MAX）。这里问 val_json_inline_max()，
 * 由 doc 模态在 db_json_init 时注册进来 —— 数字只有一处定义。
 */
static int json_to_column_impl(void *ctx, u32 xid,
                               const DbValue *text, DbValue *out)
{
    DbInstance *inst = (DbInstance *)ctx;
    u8 *bytes = NULL;
    u32 len = 0;
    u32 inline_max;
    int rc;

    if (!inst || !inst->pager || !text || !out)
        return E_INVAL;

    rc = val_json_text_to_bytes(text, &bytes, &len);
    if (rc != OK)
        return rc;

    /* 第 1 档：小文档内联 */
    if (len <= VAL_INLINE_MAX) {
        val_null(out);
        out->is_null = false;
        out->type = T_JSON;
        out->len = (u16)len;
        if (len)
            memcpy(out->s, bytes, len);
        free(bytes);
        return OK;
    }

    inline_max = val_json_inline_max();

    /*
     * 第 2 档：中型文档，堆缓冲。
     *
     * 走 VAL_EXT_MEM 而不是内联：DbValue.s[] 只有 256 字节，
     * 256..2048 的 JSONB 拷进去会截断 —— 静默给错答案。
     *
     * 这个缓冲的**所有者是调用方**：val_json_free 会 free 它。
     */
    if (inline_max == 0u || len <= inline_max) {
        val_external_json(out, (u64)(uintptr_t)bytes, len,
                          val_mem_detoast, bytes);
        return OK;
    }

    /* 第 3 档：大文档，建页链 */
    {
        u64 head = 0;

        rc = ovf_store(inst, xid, bytes, len, &head);
        free(bytes);
        if (rc != OK)
            return rc;

        /*
         * ext_ctx 用 Pager：
         *   - 读链（json_ovf_detoast）需要它
         *   - 还链（json_ovf_chain_free）也需要它
         * 两条路都通过值层注册表拿到的函数，签名都是 (void *pager, u64)。
         *
         * ⚠️ VAL_EXT_PAGE_CHAIN 告诉 val_json_free「别 free 这个 ctx」——
         *    Pager 不是堆分配，free 它就是把缓冲池还给堆。
         */
        val_external_json_kind(out, head, (u64)len,
                               val_json_chain_detoast(), inst->pager,
                               (u8)VAL_EXT_PAGE_CHAIN);
        return OK;
    }
}

/*
 * 还链回调（注册给值层的 JsonChainFreeFn）。
 *
 * ctx = Pager*（外部值的 ext_ctx 就是这个）。
 *
 * 【为什么不直接把 json_ovf_free 的函数指针传进去】
 * 签名差一个参数类型（Pager* vs void*）。把函数指针强转成另一种类型
 * 在 C 里是未定义行为（哪怕参数布局恰好一致）。一行适配换零 UB。
 */
static int json_chain_free_impl(void *pager, u64 head)
{
    if (!pager)
        return E_INVAL;
    return json_ovf_free((Pager *)pager, head);
}

int db_json_ovf_init(DbInstance *inst)
{
    int rc;
    int worst = OK;

    if (!inst)
        return E_INVAL;

    /*
     * 两个注册都可能返回 E_INVAL（已注册）。
     * 那是"别的连接已经注册过"，不是失败 —— 多连接场景每个连接都会走一次
     * db_open，不能因此让开库失败。
     */
    rc = val_set_json_to_column(json_to_column_impl, inst);
    if (rc != OK && rc != E_INVAL)
        worst = rc;

    rc = val_set_json_chain_free(json_chain_free_impl);
    if (rc != OK && rc != E_INVAL)
        worst = rc;

    return worst;
}
