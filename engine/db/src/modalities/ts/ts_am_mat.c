/*
 * ts_am_mat.c —— 时序虚拟表的物化（v1.5 步骤 5）
 *
 * 说明见 ts_am_mat.h。
 *
 * ==================== 物化在干什么 ====================
 *
 * 物理形态有两种，分区的状态决定读哪一种：
 *
 *   ACTIVE   —— 裸写页（一页装若干行，行内偏移递增）
 *   SEALED   —— 列式页（time / tag / value 各一列）
 *
 * ts_store_scan 把这两种形态统一成"一条条读数"吐出来，
 * 本文件只负责**摊平成行字节** + **按分区裁剪**。
 *
 * ==================== 裁剪的语义 ====================
 *
 * ⚠️ **裁剪只能少读，不能少给**（坑 M6-54）：
 * 谓词不可解析 ⇒ 全部分区。"我猜这个谓词大概只涉及最近 3 个分区"
 * 就是静默漏数据。
 *
 * 实现上按**分区**裁剪（不是按行）：
 * 命中范围的分区**整块**给出去 ——
 * 分区是查询的最小单位，而且分区内部的时间是**乱序**的
 * （迟到写入是允许的，只要不落在已封窗的分区里），
 * 按行裁剪的边界很容易写错 ⇒ 少给行 = 静默漏数据。
 *
 * ⚠️ **DROPPED 的分区不选**：
 * 它已经被 TTL 回收了 —— 页都还回去了，选它等于去读**别人的页**。
 */

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/ts/ts_am_mat.h"
#include "modalities/ts/tsstore.h"

/* ---------------- 生命周期 ---------------- */

void ts_am_mat_init(TsAmMat *m, u32 row_bytes)
{
    if (!m) {
        return;
    }
    memset(m, 0, sizeof(*m));
    m->row_bytes = row_bytes;
}

void ts_am_mat_free(TsAmMat *m)
{
    if (!m) {
        return;
    }
    free(m->rows);
    free(m->pstart);
    free(m->pend);
    memset(m, 0, sizeof(*m));
}

/* ---------------- 小工具 ---------------- */

/*
 * double → 位模式（memcpy，不用指针双关）。
 *
 * 磁盘格式只允许定宽无符号整数（common/types.h 铁律 1），
 * 而读数是 double ⇒ 存的是它的**位模式**。
 */
static u64 ts_am_d2b(double d)
{
    u64 bits;

    memcpy(&bits, &d, sizeof(bits));
    return bits;
}

/* ---------------- 追加 ---------------- */

/* 追加一行 */
static int ts_am_mat_append(TsAmMat *m, const u8 *row)
{
    if (m->nrows == m->cap_rows) {
        u32 ncap = (m->cap_rows == 0u) ? 256u : m->cap_rows * 2u;
        u8 *nr = (u8 *)realloc(m->rows, (size_t)ncap * m->row_bytes);

        if (nr == NULL) {
            return E_NOSPACE;
        }
        m->rows = nr;
        m->cap_rows = ncap;
    }
    memcpy(m->rows + (size_t)m->nrows * m->row_bytes, row, m->row_bytes);
    m->nrows++;
    return OK;
}

/* 追加一个选中的分区区间 */
static int ts_am_mat_add_part(TsAmMat *m, i64 start_ms, i64 end_ms)
{
    if (m->nparts == m->cap_parts) {
        u32 ncap = (m->cap_parts == 0u) ? 16u : m->cap_parts * 2u;
        i64 *ns = (i64 *)realloc(m->pstart, (size_t)ncap * sizeof(i64));
        i64 *ne;

        if (ns == NULL) {
            return E_NOSPACE;
        }
        m->pstart = ns;
        ne = (i64 *)realloc(m->pend, (size_t)ncap * sizeof(i64));
        if (ne == NULL) {
            return E_NOSPACE;
        }
        m->pend = ne;
        m->cap_parts = ncap;
    }
    m->pstart[m->nparts] = start_ms;
    m->pend[m->nparts] = end_ms;
    m->nparts++;
    return OK;
}

/* ---------------- 裁剪 ---------------- */

/*
 * 这个时间戳落在选中的分区里吗？
 *
 * 二分找到"最后一个 start <= t"的区间，再往回看一格 ——
 * 往回看是因为**分区区间理论上可能重叠**（改过分区窗口之后，
 * 新老窗口的边界可能套在一起，见 tsstore.h 的说明）。
 * 正常情况下往回看的那一格不重叠 ⇒ 白看一次而已。
 */
static bool ts_am_in_parts(const TsAmMat *m, i64 t)
{
    u32 lo = 0;
    u32 hi = m->nparts;

    if (m->nparts == 0u) {
        return true;        /* 不裁剪 ⇒ 全要 */
    }

    /* 二分：找第一个 start > t 的位置 */
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;

        if (m->pstart[mid] <= t) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    /* lo == 第一个 start > t 的下标 ⇒ lo-1 是最后一个 start <= t 的区间 */
    while (lo > 0u) {
        lo--;
        if (t < m->pend[lo]) {
            return true;    /* 区间覆盖 t */
        }
        /* 这一格不覆盖 t：只有当下一格也不覆盖时才继续往回看 */
        if (lo == 0u || m->pend[lo] <= m->pstart[lo - 1u]) {
            break;           /* 没有重叠 ⇒ 往回看也没用 */
        }
    }
    return false;
}

/*
 * 选出与 [lo, hi] 相交的分区。
 *
 * 分区是左闭右开 ⇒ 相交条件是 start <= hi 且 end > lo。
 */
static int ts_am_select_parts(TsStore *st, bool has_range, i64 lo, i64 hi, TsAmMat *m)
{
    u32 np;
    u32 i;

    if (!has_range) {
        return OK;
    }

    np = ts_store_part_count(st);
    for (i = 0u; i < np; i++) {
        TsPartInfo info;
        int rc = ts_store_part_info(st, i, &info);

        if (rc != OK) {
            return rc;
        }
        if (info.state == TS_PART_DROPPED) {
            continue;
        }
        /* 区间相交？（分区是左闭右开） */
        if (info.start_ms > hi || info.end_ms <= lo) {
            continue;
        }
        rc = ts_am_mat_add_part(m, info.start_ms, info.end_ms);
        if (rc != OK) {
            return rc;
        }
    }
    return OK;
}

/* ---------------- 读数的物化 ---------------- */

/* 枚举回调：把一条读数收成一行 */
static int ts_am_collect_reading(const TsReading *r, void *vctx)
{
    TsAmMat *m = (TsAmMat *)vctx;
    u8 row[TS_READING_ROW_BYTES];

    /* 裁剪：不在选中分区里的读数直接丢掉 */
    if (!ts_am_in_parts(m, r->time)) {
        return OK;
    }

    put_u64(row + TS_ROW_TIME_OFF, (u64)r->time);
    put_u64(row + TS_ROW_TAG_OFF, (u64)r->tag);
    put_u64(row + TS_ROW_VAL_OFF, ts_am_d2b(r->value));
    return ts_am_mat_append(m, row);
}

int ts_am_mat_readings(TsStore *st, bool has_range, i64 lo, i64 hi, TsAmMat *m)
{
    int rc;

    if (!st || !m) {
        return E_INVAL;
    }

    ts_am_mat_init(m, TS_READING_ROW_BYTES);

    rc = ts_am_select_parts(st, has_range, lo, hi, m);
    if (rc != OK) {
        ts_am_mat_free(m);
        return rc;
    }
    rc = ts_store_scan(st, ts_am_collect_reading, m);
    if (rc != OK) {
        ts_am_mat_free(m);
        return rc;
    }
    return OK;
}

/* ---------------- 分区目录的物化 ---------------- */

int ts_am_mat_partitions(TsStore *st, TsAmMat *m)
{
    u32 np;
    u32 i;
    int rc = OK;

    if (!st || !m) {
        return E_INVAL;
    }

    ts_am_mat_init(m, TS_PART_ROW_BYTES);

    np = ts_store_part_count(st);
    for (i = 0u; i < np; i++) {
        TsPartInfo info;
        u8 row[TS_PART_ROW_BYTES];

        rc = ts_store_part_info(st, i, &info);
        if (rc != OK) {
            break;
        }
        put_u64(row + TS_PROW_START_OFF, (u64)info.start_ms);
        put_u64(row + TS_PROW_END_OFF, (u64)info.end_ms);
        put_u32(row + TS_PROW_STATE_OFF, (u32)info.state);
        put_u64(row + TS_PROW_ROWS_OFF, (u64)info.n_rows);
        put_u64(row + TS_PROW_PAGES_OFF, (u64)info.page_count);

        rc = ts_am_mat_append(m, row);
        if (rc != OK) {
            break;
        }
    }

    if (rc != OK) {
        ts_am_mat_free(m);
        return rc;
    }
    return OK;
}
