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
#include "modalities/ts/ts_prune.h"
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




/* ---------------- 读数的物化 ---------------- */

/*
 * 枚举回调：把一条读数收成一行。
 *
 * ⚠️ **这里不再做行过滤**（W-D 的 D-6.1）：
 * 上一版在这里用 ts_am_in_parts 丢掉不要的读数 —— 而那时**所有分区都已经读过了**，
 * 过滤只是让行数对，页读一点没省。
 * 现在是 ts_prune_scan 在**分区级**跳过 ⇒ 到这里来的每一条都是要的。
 */
static int ts_am_collect_row(const TsReading *r, void *vctx)
{
    TsAmMat *m = (TsAmMat *)vctx;
    u8 row[TS_READING_ROW_BYTES];

    put_u64(row + TS_ROW_TIME_OFF, (u64)r->time);
    put_u64(row + TS_ROW_TAG_OFF, (u64)r->tag);
    put_u64(row + TS_ROW_VAL_OFF, ts_am_d2b(r->value));
    return ts_am_mat_append(m, row);
}

/*
 * 物化读数（W-D 的 D-6.1 改造：从"全扫 + 行过滤"改成"分区级跳过"）。
 *
 * ==================== 改之前是什么样 ====================
 *
 * 上一版：
 *   ① ts_am_select_parts 算出"要哪几个分区的区间"；
 *   ② ts_store_scan 把**所有分区**的页读一遍；
 *   ③ 逐行用 ts_am_in_parts 过滤掉不要的。
 *
 * 行数是**对的**，但一页都没少读 —— 实测 3 个分区、查其中 1 天：
 *
 *   无 WHERE     900 行 / 6 页
 *   只要第 1 天   300 行 / 6 页   ← 行对了，IO 一页没降
 *
 * 也就是说「时间谓词参与分区裁剪」这句话在**行数上成立、在 IO 上不成立**。
 * 而 T1–T4 那套判据测的是 bench 直调 `ts_prune_scan`，绕开了这条真实路径 ——
 * 所以它们**证明不了**这件事。
 *
 * ==================== 改之后 ====================
 *
 * 走 `ts_prune_scan`：它先算裁剪集合，然后对**不在集合里的分区整段跳过**
 *（连页都不碰）。行过滤那一步就不再需要了 —— 少读的就是跳过的那些分区。
 *
 * 【为什么返回值必须与老的实现完全一致】
 *
 * 两条路径的行集合必须相同：`ts_store_scan` 是"创建序"，
 * `ts_prune_scan` 是"时间序"。这个差别在**本层看不见** ——
 * 物化之后 `scan_next` 按输出序号给 RID，执行器不依赖顺序。
 * 所以换路径不改变任何可见行为，只改变 IO。
 *
 * 【不设范围时必须与全扫全等】
 *
 * `has_range == false` ⇒ `ts_prune_select` 给出 all = true ⇒
 * 每个分区都允许 ⇒ 与 `ts_store_scan` 读到的**集合**完全一样（坑 M6-48）。
 * 这一条由 test_ts_page_prune.c 的第 1 项钉住。
 */
int ts_am_mat_readings(TsStore *st, bool has_range, i64 lo, i64 hi, TsAmMat *m)
{
    Pager *pg;
    u64 reads = 0;
    int rc;

    if (!st || !m) {
        return E_INVAL;
    }

    ts_am_mat_init(m, TS_READING_ROW_BYTES);

    pg = ts_store_pager(st);
    if (pg == NULL) {
        ts_am_mat_free(m);
        return E_INVAL;
    }

    rc = ts_prune_scan(pg, st, lo, hi, has_range, ts_am_collect_row, m, &reads);

    /*
     * ⚠️ 页读要记进存储自己的计数器：诊断口径是
     * "这个模态累计读了多少页"（db_ts_page_reads 读的就是它）。
     * 不记的话裁剪省的页在读路径上看不出来 —— 那 D-6.1 就没有可测的判据。
     */
    ts_store_note_reads(st, reads);

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
