/*
 * ts_record.c —— 时序存储的记录层（从 tsstore.c 拆出）
 *
 * 原 tsstore.c 808 行，超了 800 行硬规则。拆出来的这一层是
 * **"一行读数怎么落进页里、又从页里读回来"**：
 *
 *   小工具          位模式互转（坑 M6-33）
 *   时间路由        t → 分区号（纯函数，坑 M6-26 的负数取整）
 *   运行时数组      分区数组 / by-time 索引的扩容
 *   时间序索引      by-time 的有序插入与二分
 *   数据页          走页链 / 追加一行 / 提交页
 *
 * 留在 tsstore.c 的是**存储对象的生命周期与查询入口**：
 *   新建 / 打开 / 释放 / 窗口 / 写 / 读 / 目录查询 / 诊断
 *
 * 界线是"记录 vs 对象"：本文件不认识 TsStore 的整体语义，
 * 只负责把一行 48 字节的读数写进（或读出）页链。
 */

#include "modalities/ts/ts_store_int.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "modalities/ts/ts_column.h"
#include "modalities/ts/ts_dir.h"
#include "storage/page.h"

/* ---------------- 小工具 ---------------- */

/* 位模式互转（坑 M6-33） */
static i64 ts_i64_of(u64 v)
{
    i64 r;

    memcpy(&r, &v, sizeof(r));
    return r;
}

u64 ts_double_bits(double d)
{
    u64 bits;

    memcpy(&bits, &d, sizeof(bits));
    return bits;
}

/* ---------------- 时间路由（纯函数） ---------------- */

i64 ts_route_partition(i64 t_ms, i64 span_ms)
{
    i64 q;
    i64 r;
    u64 start;
    u64 end;

    /*
     * span_ms 必须是正数。契约说"必须 > 0"，
     * 这里不崩（返回 0 = 第 0 个分区的起点），但把话说死。
     */
    if (span_ms <= 0)
        return 0;

    q = t_ms / span_ms;
    r = t_ms % span_ms;

    /*
     * 坑 M6-26：向零截断 ⇒ 负数要再减 1（向下取整）。
     *
     * 举例（span = 1 天 = 86400000 ms）：
     *   t = -1        → q = 0, r = -1  → q = -1 → start = -86400000 ✓
     *   t = -86400000 → q = -1, r = 0  → q = -1 → start = -86400000 ✓
     *   t = 0         → q = 0, r = 0  → q = 0  → start = 0 ✓
     */
    if (r < 0)
        q -= 1;

    /* 用 u64 乘：|q × span| ≤ |t| ≤ 2^63，u64 装得下，且回绕有定义 */
    start = (u64)q * (u64)span_ms;
    end = start + (u64)span_ms;

    /*
     * 计划风险 R3 的断言：**part_start ≤ t < part_end**。
     *
     * 路由算错 ⇒ 数据落进错误的分区 ⇒ 查询按分区裁剪时**静默漏数据**。
     * 这类错不报错、只是"查不全"，所以在这里就地断言。
     */
    assert(ts_i64_of(start) <= t_ms);
    if (t_ms >= 0) {
        /* t 非负时 end 不会溢出 u64，直接比 */
        assert((u64)t_ms < end);
    }
    /* t 为负时，start ≤ t < 0 ≤ end 恒成立，不必再判 */

    return ts_i64_of(start);
}

/* ---------------- 运行时数组 ---------------- */

int ts_rt_reserve(TsStore *st, u32 need)
{
    u32 cap;
    TsPartRt *np;

    if (need <= st->rt_cap)
        return OK;

    cap = st->rt_cap ? st->rt_cap : 8;
    while (cap < need)
        cap *= 2;

    np = (TsPartRt *)realloc(st->rt, (size_t)cap * sizeof(TsPartRt));
    if (np == NULL)
        return E_NOSPACE;

    memset(np + st->rt_cap, 0, (size_t)(cap - st->rt_cap) * sizeof(TsPartRt));
    st->rt = np;
    st->rt_cap = cap;
    return OK;
}

/* ---------------- 时间序索引 ---------------- */

int ts_by_time_reserve(TsStore *st, u32 need)
{
    u32 cap;
    u32 *np;

    if (need <= st->by_time_cap)
        return OK;

    cap = st->by_time_cap ? st->by_time_cap : 8;
    while (cap < need)
        cap *= 2;

    np = (u32 *)realloc(st->by_time, (size_t)cap * sizeof(u32));
    if (np == NULL)
        return E_NOSPACE;

    st->by_time = np;
    st->by_time_cap = cap;
    return OK;
}

/* 取磁盘序第 idx 个分区的起始时间 */
i64 ts_part_start(const TsStore *st, u32 idx)
{
    TsPartInfo info;

    if (ts_dir_get(st->dir, idx, &info) != OK)
        return 0;
    return info.start_ms;
}

/*
 * 把磁盘序下标 idx 插进时间序数组（按 start_ms 升序）。
 *
 * 相同 start_ms 的情况：新的排后面（与 ts_dir 内部的口径一致）。
 *
 * 调用前必须先 ts_by_time_reserve(st, count + 1)。
 */
void ts_by_time_insert(TsStore *st, u32 idx)
{
    i64 key = ts_part_start(st, idx);
    u32 lo = 0;
    u32 hi = st->by_time_count;
    u32 pos;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;

        if (ts_part_start(st, st->by_time[mid]) <= key)
            lo = mid + 1;
        else
            hi = mid;
    }
    pos = lo;

    if (pos < st->by_time_count) {
        memmove(&st->by_time[pos + 1], &st->by_time[pos],
                (size_t)(st->by_time_count - pos) * sizeof(u32));
    }
    st->by_time[pos] = idx;
    st->by_time_count++;
}

/* ---------------- 数据页 ---------------- */

/*
 * 走一遍分区的页链：数行数、数页数、找链尾。
 *
 * 开库时用（把目录里的水位线校正成真值，坑 M6-30）。
 * 裸写页的格式与读都在 ts_column 层（页格式常量只有一份，见那个头文件）。
 */
int ts_walk_part(TsStore *st, u32 first_page, u32 *rows_out,
                        u32 *pages_out, u32 *last_out)
{
    return ts_part_walk(st->pg, first_page, rows_out, pages_out, last_out,
                        &st->page_reads);
}

/*
 * 往分区追加一行。
 *
 * 页没满 ⇒ 直接追加；页满了 ⇒ spill：开一张新页，先把行写进新页，
 * 再把旧页的 next 指过去（坑 M6-29：顺序不能反）。
 *
 * 步骤 6：每次改页都走 ts_pg_commit（先写日志、再写 LSN、最后归还页）。
 * 写日志失败 ⇒ 页恢复原样 ⇒ 这次追加作废（半截行绝不能落盘）。
 */
int ts_append_row(TsStore *st, u32 idx, const TsReading *r)
{
    TsPartRt *rt = &st->rt[idx];
    PageFrame *fr = NULL;
    u32 pid = rt->last_page;
    u32 n;
    u8 *row;
    u8 before[PAGE_SIZE];      /* 要改的那一页的前像 */
    u8 newbefore[PAGE_SIZE];   /* spill 时新页的前像 */
    bool spilled = false;
    int rc;

    rc = pager_get(st->pg, pid, &fr);
    if (rc != OK)
        return rc;
    st->page_reads++;

    if (fr->data[H_TYPE] != (u8)PT_TS_PART) {
        (void)pager_unpin(st->pg, pid, false);
        return E_CORRUPT;
    }

    n = get_u32(fr->data + TSSP_NROWS);
    if (n >= TS_ROWS_PER_PAGE) {
        /* spill：新开一页 */
        PageFrame *nf = NULL;
        u32 npid = 0;

        rc = pager_alloc(st->pg, &npid, &nf);
        if (rc != OK) {
            (void)pager_unpin(st->pg, pid, false);
            return rc;
        }
        ts_wal_snap(nf->data, newbefore);   /* 新页的前像（此刻还是全零） */
        page_init(nf->data, (u8)PT_TS_PART);
        put_u32(nf->data + TSSP_NROWS, 0);
        put_u32(nf->data + TSSP_NEXT, PAGE_ID_NIL);

        /* 旧页接上新页（先写新页的内容，再接链 —— 见坑 M6-29） */
        ts_wal_snap(fr->data, before);
        put_u32(fr->data + TSSP_NEXT, npid);
        page_set_checksum(fr->data);
        rc = ts_pg_commit(st->pg, pid, fr, &st->wal, before);
        if (rc != OK) {
            (void)pager_unpin(st->pg, npid, false);
            return rc;
        }

        rt->page_count++;
        rt->last_page = npid;
        fr = nf;
        pid = npid;
        n = 0;
        spilled = true;
    }

    if (!spilled) {
        ts_wal_snap(fr->data, before);
    }
    row = fr->data + TSSP_DATA + (size_t)n * TS_ROW_SIZE;
    put_u64(row + 0, (u64)r->time);
    put_u64(row + 8, (u64)r->tag);
    put_u64(row + 16, ts_double_bits(r->value));
    put_u32(fr->data + TSSP_NROWS, n + 1);
    page_set_checksum(fr->data);

    rc = ts_pg_commit(st->pg, pid, fr, &st->wal, spilled ? newbefore : before);
    if (rc != OK)
        return rc;

    rt->n_rows++;
    st->total_rows++;

    /*
     * 页数变了 ⇒ 目录里的水位线要更新（坑 M6-30）。
     * 只在 spill 时更新：每写一行就改一次目录 = 每行一次 4KB 页写。
     */
    if (spilled) {
        TsPartInfo info;

        rc = ts_dir_get(st->dir, idx, &info);
        if (rc != OK)
            return rc;
        info.n_rows = rt->n_rows;
        info.page_count = rt->page_count;
        rc = ts_dir_update(st->dir, idx, &info);
        if (rc != OK)
            return rc;
    }

    return OK;
}

