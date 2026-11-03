/*
 * heap_compact.c — 页内 compact（v1.2 步骤 5）
 *
 * 本文件负责：
 *   - heap_compact_page：整理一页（回收页内空洞）
 *   - heap_compact_table：整理整张表（逐页独立）
 *   - heap_compact_frag_pct：全表碎片率
 *
 * 【核心不变量】
 *   整理只改**物理位置**，slot 编号不变
 *   → RID = (page, slot) 不变 → 索引项、游标全都有效。
 *
 * 【崩溃安全】
 *   先写 WAL（before / after 两份整页镜像），日志写好了才改真页。
 */

#include "storage/heap_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---------------- 页内 compact（v1.2 步骤 5） ---------------- */

/*
 * 整理一页。
 *
 * ==================== 为什么在副本上整理 ====================
 *
 * WAL 规则要求"**先写日志，再改页**"。
 * 而整理的 after 镜像必须**整理完才知道** —— 这看着像个死结。
 *
 * 解法：在**副本**上整理。
 *
 *     before = 当前页的副本
 *     after  = 当前页的副本（在这个副本上整理）
 *     → 写日志（before / after）
 *     → 日志写好了，才把 after 盖回真页
 *
 * 这样"改页"这一步永远发生在"日志写好"之后，
 * 崩溃时不会出现"页变了、日志里没有"的窗口。
 *
 * ==================== 为什么只要一条日志记录 ====================
 *
 * 一条记录里带 before 和 after 两份整页镜像：
 *   - 重放（redo）：盖上 after → 最终态
 *   - 回滚（undo）：盖回 before → 整理被撤销
 *
 * 比"整理前后各写一条"好的地方是：日志里的信息是**原子的**，
 * 不会出现"日志里只有前半截"的状态。
 */
int heap_compact_page(Heap *h, u32 xid, u32 page_id,
                      u16 frag_threshold_pct, HeapPageCompact *out)
{
    HeapPageCompact res;
    memset(&res, 0, sizeof(res));

    if (!h || page_id == PAGE_ID_NIL)
        return E_INVAL;
    if (out)
        *out = res;

    PageFrame *f;
    int rc = pager_get(h->pager, page_id, &f);
    if (rc != OK)
        return rc;

    /* 不是本表的页 → 不碰（别人的页不能被整理） */
    if (f->data[H_TYPE] != PT_HEAP || page_table_id(f->data) != h->tid) {
        pager_unpin(h->pager, page_id, false);
        return E_NOTFOUND;
    }

    res.frag_before = page_fragmentation(f->data);
    res.used_before = page_used_bytes(f->data);
    res.live_before = page_live_bytes(f->data);

    if (res.frag_before == 0 ||
        (frag_threshold_pct && res.frag_before < frag_threshold_pct)) {
        pager_unpin(h->pager, page_id, false);
        return OK;
    }

    u16 free_before = page_free_bytes(f->data);

    /* before = 整理前；after = 在副本上整理后的结果 */
    u8 before[PAGE_SIZE];
    u8 after[PAGE_SIZE];
    memcpy(before, f->data, PAGE_SIZE);
    memcpy(after, f->data, PAGE_SIZE);
    page_compact(after);

    /* 先写日志 */
    u32 lsn = 0;
    rc = heap_wal_log(h, xid, LOG_PAGEDATA, page_id, 0, LOG_NO_SLOT_OFF,
                      before, (u16)PAGE_SIZE, after, (u16)PAGE_SIZE, &lsn);
    if (rc != OK) {
        /* 日志没写好 → 页不许动 */
        pager_unpin(h->pager, page_id, false);
        return rc;
    }

    /* 日志写好了，才把整理结果落到真页上 */
    memcpy(f->data, after, PAGE_SIZE);
    if (lsn)
        page_set_lsn(f->data, lsn);

    /*
     * 空间回收了 → freelist 的档位必须更新。
     *
     * 不更新的后果：这一页在 freelist 里还挂着"旧的（更小的）档位"，
     * 找页的时候会被当成"塞不下"而跳过 —— 空间明明空着却用不上。
     */
    freelist_set(h->fl, page_id, page_free_bytes(f->data));

    res.compacted = true;
    {
        u16 free_now = page_free_bytes(f->data);
        /* 回收量 = 整理后的可用空间 − 整理前的可用空间（整理只会变大） */
        res.freed = (u64)(u16)(free_now - free_before);
    }

    if (out)
        *out = res;

    return pager_unpin(h->pager, page_id, true);
}

/*
 * 整理整张表。
 *
 * 逐页独立：一页整理失败不影响别的页 —— 维护操作不该因为
 * "某一页有点怪"就整体失败。
 */
int heap_compact_table(Heap *h, u32 xid, u16 frag_threshold_pct,
                       HeapCompactStats *stats_out)
{
    HeapCompactStats st;
    memset(&st, 0, sizeof(st));
    if (!h)
        return E_INVAL;

    /*
     * 只整本表的页 —— 页号表快照，不再遍历整个页空间
     * （老代码遍历 0..max_page 再靠 page_table_id 过滤）。
     */
    u32 *pages = NULL;
    u32 npages = 0;
    int rc = heap_plist_snapshot(h, &pages, &npages);
    if (rc != OK)
        return rc;

    for (u32 i = 0; i < npages; i++) {
        HeapPageCompact pc;
        rc = heap_compact_page(h, xid, pages[i], frag_threshold_pct, &pc);
        if (rc == E_NOTFOUND)
            continue;               /* 不是本表的页 */
        if (rc != OK) {
            if (stats_out)
                *stats_out = st;
            free(pages);
            return rc;
        }
        st.pages_scanned++;
        st.used_bytes += pc.used_before;
        st.live_bytes += pc.live_before;
        if (pc.compacted) {
            st.pages_compacted++;
            st.bytes_freed += pc.freed;
        }
    }

    free(pages);
    if (stats_out)
        *stats_out = st;
    return OK;
}

u16 heap_compact_frag_pct(const HeapCompactStats *st)
{
    if (!st || st->used_bytes == 0)
        return 0;
    u64 hole = st->used_bytes - st->live_bytes;
    return (u16)((hole * 100u) / st->used_bytes);
}
