/*
 * ts_wal.c —— 时序模态的**写日志接线**（v1.5 步骤 6 · T10）
 *
 * 契约见 ts_wal.h（为什么这么接线、代价是什么都在那里）。
 * 这里只放实现。
 *
 * ==================== 顺序是硬要求 ====================
 *
 * loghook.h 定的规矩：
 *   1. 调用方已经算好这次修改会影响哪个 slot
 *   2. 调钩子写日志，拿到 LSN
 *   3. 改页，把 LSN 写进页头
 *
 * 反过来的话（先改页再写日志）：页可能先于日志落盘 —— 崩溃后
 * 日志里缺这条记录，页上却有它的内容，数据"没根"。
 *
 * 模态侧的写页点全都是**原地改写**（行数组、页头的行数与链指针），
 * 所以"改页"这步在调用方手里就已经做完了；
 * 本模块负责的是第 2 步（写日志）与第 3 步的后半（写 LSN）。
 *
 * ==================== 失败怎么办 ====================
 *
 * 日志写失败 ⇒ 这次修改**作废**：把页恢复成改之前的样子，
 * 按"不是脏页"归还给缓冲池，返回错误码。
 * 半改的页一旦落盘，读出来就是**半截分区** —— 比这次写入失败严重得多。
 *
 * ==================== 为什么前像要 4096 字节 ====================
 *
 * 前像在时序这边**不会被用来回滚**（xid 固定 0 ⇒ 永不回滚），
 * 但它仍然要记：它是"这次改动之前这一页长什么样"的完整留痕，
 * 与地理模态同一个口径（geo_wal.h 里也解释了它的用途）。
 *
 * ⚠️ 前像 + 后像 = 8192 字节，正好等于 WAL_MAX_PAYLOAD
 * （wal/wal.h）—— 不多一字节，所以**前像不能更长**。
 */

#include "modalities/ts/ts_wal.h"

#include <string.h>

#include "storage/page.h"

/*
 * 拍前像：整页拷贝。
 *
 * 调用方必须在**改页之前**调用。
 */
void ts_wal_snap(const u8 *page, u8 *buf)
{
    if (!page || !buf)
        return;
    memcpy(buf, page, PAGE_SIZE);
}

int ts_wal_log_page(const TsWal *w, u32 page_id, const u8 *before,
                    const u8 *after, u32 *lsn_out)
{
    u32 lsn = 0;
    int rc;

    if (lsn_out)
        *lsn_out = 0;
    if (!w || !w->fn || !after)
        return E_INVAL;

    /*
     * xid 显式传 0（系统事务）——
     * 时序的公开 API 不带事务参数，模态层也拿不到事务上下文，
     * 所以这里没有"当前事务"可言。xid = 0 的记录在恢复时永不回滚
     * （见 wal/recovery.c 的 xid_committed）。
     *
     * slot / slot_off 传"无"：整页镜像不涉及页内某个 slot。
     */
    rc = w->fn(w->ctx, 0u, LOG_PAGEDATA, page_id, PAGE_SLOT_NONE,
               LOG_NO_SLOT_OFF,
               before, (u16)(before ? (u32)PAGE_SIZE : 0u),
               after, (u16)PAGE_SIZE, &lsn);
    if (rc != OK)
        return rc;
    if (lsn == 0)
        return E_IO;          /* 钩子说"写好了"却没给 LSN ⇒ 当失败处理 */

    if (lsn_out)
        *lsn_out = lsn;
    return OK;
}

int ts_pg_commit(Pager *pg, u32 page_id, PageFrame *fr, const TsWal *w,
                 const u8 *before)
{
    u32 lsn = 0;
    int rc;

    if (!pg || !fr)
        return E_INVAL;

    /* 没有钩子：照常写页，只是不进日志（单元测试直接用模态时） */
    if (!w || !w->fn) {
        page_set_checksum(fr->data);
        return pager_unpin(pg, page_id, true);
    }

    rc = ts_wal_log_page(w, page_id, before, fr->data, &lsn);
    if (rc != OK) {
        /* 日志没写成功 ⇒ 这次修改作废，页恢复原样 */
        if (before)
            memcpy(fr->data, before, PAGE_SIZE);
        pager_unpin(pg, page_id, false);
        return rc;
    }

    /*
     * 写 LSN（page_set_lsn 会顺带重算校验和）。
     *
     * 顺序：日志里存的 after 镜像**不含**这条 LSN（写日志时还不知道
     * LSN 是多少）；重放时 recovery 会 memcpy 整页镜像、再补上
     * 记录的 LSN 并重算校验和 —— 两边算出来的页**逐字节相同**。
     */
    page_set_lsn(fr->data, lsn);
    return pager_unpin(pg, page_id, true);
}
