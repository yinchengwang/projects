/*
 * ts_dir_query.c —— 分区目录的**只读那一半**（v1.5 步骤 6 · T10）
 *
 * 契约见 ts_dir.h；私有结构见 ts_dir_int.h。
 *
 * 【为什么把查询拆出来】
 *
 * ts_dir.c 原来就已经顶在 800 行的上限上（799 行），
 * 而步骤 6（T10）要往每个改页的写点插 WAL 的接线（拍前像 + 写日志 +
 * 写 LSN）⇒ 只会更挤。
 *
 * 目录这一层天然分两半：
 *   - 写的那半（ts_dir.c）：建目录、追目录链、追加/改写目录项；
 *   - 读的那半（本文件）：取目录项、按时间找分区、计数与诊断。
 *
 * 读的那半一行页都不碰 ⇒ 它对 WAL 一无所知也不需要知道。
 * 拆开之后 ts_dir.c 回到 750 行上下，两边都好读。
 *
 * ⚠️ 对外的契约（ts_dir.h）**一个字都没改** ——
 * 函数签名、语义、错误码全部照旧。
 *
 * 【这一层不做解释】
 *
 * 它只回答"目录里现在有什么"，不判断"该不该有"：
 * 状态怎么流转（DROPPED 是谁标的、SEALED 是谁翻的）由写的那半决定。
 */

#include "modalities/ts/ts_dir.h"

#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/ts/ts_dir_int.h"
#include "storage/pager.h"

/* ---------------- 取目录项 ---------------- */

int ts_dir_get(const TsDir *d, u32 idx, TsPartInfo *out)
{
    if (!d || !out) {
        return E_INVAL;
    }
    if (idx >= d->n_parts) {
        return E_INVAL;
    }
    *out = d->items[idx];
    return OK;
}

/*
 * 取**按起始时间升序**的第 i 条。
 *
 * tsstore.h 的 ts_store_part_info() 承诺"按起始时间升序"，
 * 而磁盘上的顺序是**追加顺序**（写入通常是时间递增的，
 * 但回填旧数据就会乱序）—— 所以这一层维护了一份排序下标。
 */
int ts_dir_get_by_time(const TsDir *d, u32 i, TsPartInfo *out)
{
    if (!d || !out) {
        return E_INVAL;
    }
    if (i >= d->n_parts) {
        return E_INVAL;
    }
    *out = d->items[d->sorted[i]];
    return OK;
}

/*
 * 找包含 t_ms 的分区。
 *
 * 找不到 → E_NOTFOUND（那个时间窗还没写过 / 已被 TTL 丢弃）。
 * **DROPPED 的分区会被跳过**（页已回收，不该再被查到）。
 *
 * idx_out —— 命中项的**磁盘序**下标（可为 NULL）。
 */
int ts_dir_find(const TsDir *d, i64 t_ms, u32 *idx_out, TsPartInfo *out)
{
    u32 lo;
    u32 hi;
    u32 cand;

    if (!d) {
        return E_INVAL;
    }
    if (d->n_parts == 0u) {
        return E_NOTFOUND;
    }

    /* 二分找"起始时间 ≤ t_ms 的最后一项"；没有候选项 ⇒ t_ms 早于所有分区 */
    lo = 0;
    hi = d->n_parts;
    cand = PAGE_ID_NIL;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        u32 idx = d->sorted[mid];

        if (d->items[idx].start_ms <= t_ms) {
            cand = idx;
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (cand == PAGE_ID_NIL) {
        return E_NOTFOUND;
    }
    if (t_ms >= d->items[cand].end_ms) {
        return E_NOTFOUND;   /* 落在两个分区之间的空档 */
    }
    if (d->items[cand].state == TS_PART_DROPPED) {
        /* 已回收的分区不该再被查到 */
        return E_NOTFOUND;
    }
    if (idx_out) {
        *idx_out = cand;
    }
    if (out) {
        *out = d->items[cand];
    }
    return OK;
}

/* ---------------- 计数与诊断 ---------------- */

u32 ts_dir_count(const TsDir *d)
{
    return d ? d->n_parts : 0u;
}

u64 ts_dir_total_rows(const TsDir *d)
{
    u64 total = 0;
    u32 i;

    if (!d) {
        return 0;
    }
    for (i = 0; i < d->n_parts; i++) {
        total += d->items[i].n_rows;
    }
    return total;
}

u32 ts_dir_span_sec(const TsDir *d)
{
    return d ? d->span_sec : 0u;
}

u32 ts_dir_page_count(const TsDir *d)
{
    return d ? d->n_dir_pages : 0u;
}

u64 ts_dir_page_reads(const TsDir *d)
{
    return d ? d->page_reads : 0;
}
