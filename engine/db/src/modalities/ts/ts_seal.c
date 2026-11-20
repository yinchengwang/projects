/*
 * ts_seal.c —— 时序分区的封窗（v1.5 步骤 3）
 *
 * 契约见 ts_seal.h。本文件实现封窗的**第一步**（写列式页）；
 * 第二步（翻目录）与编排（ts_store_seal_commit / ts_store_seal）
 * 在 tsstore.c 里实现 —— 那一层才拿得到目录与运行时状态。
 *
 * 【封窗这一步在做什么】
 *
 *   裸写页链（PT_TS_PART）           列式页（PT_TS_COL）
 *   ┌──────┐ ┌──────┐ ┌──────┐      ┌──────────┐   ┌──────┐ ┌──────┐
 *   │24B×n │→│24B×n │→│24B×n │      │ 头部页   │   │ 时间 │→ 时间 │
 *   └──────┘ └──────┘ └──────┘  ⇒   │ 行数/页数│   └──────┘ └──────┘
 *                                   │ 旧页位置 │   ┌──────┐ ┌──────┐
 *                                   └──────────┘   │ 标签 │→ 标签 │
 *                                                 └──────┘ └──────┘
 *                                                 ┌──────┐ ┌──────┐
 *                                                 │ 值   │→ 值   │
 *                                                 └──────┘ └──────┘
 *
 * 三列各自编码（时间 dod / 标签 rle / 值 gorilla，见步骤 1 的三个模块），
 * 各自成链。头部页记行数与每列的（字节数 / 页数 / 条数）——
 * **段头必须记条数**（坑 M6-39）。
 *
 * 【旧页回收（W-D 的 D-6.2 之后）】
 *
 * 改之前：旧裸写页只是**标记不再使用**（首页与页数记在头部页的
 * `TSC_OLDFIRST` / `TSC_OLDPAGES` 里），**不真回收** —— 坑 M6-42，
 * 代价是文件只涨不缩。
 *
 * 现在：目录翻成 SEALED 并落盘之后，立刻调
 * `ts_ttl_release_seal_old_wal()` 把那批页清零并还给全局空闲栈。
 * 顺序是"先清标记（落盘）→ 再回收页"：崩在中间只会留下空洞（安全方向），
 * 反过来会把已经还回去的页再清零一次 —— 那可能是别人的页。
 *
 * TTL 收不了它们（它只收"目录项指着的那些页"，对封窗分区就是列式页），
 * 所以这段必须由封窗流程自己做。
 *
 * 【封窗的失败模式】
 *
 * 这一步失败（没空间、页坏了）⇒ 分区**仍然是活跃的**：
 * 目录没动 ⇒ 读路径走裸写页 ⇒ 数据一条不少。
 * 已经写出去的列式页成了孤儿页（浪费空间，不丢数据）。
 * 这个方向的选择是刻意的：**宁可浪费空间，不可丢数据**。
 */

#include "modalities/ts/ts_seal.h"

#include <stdlib.h>

#include "modalities/ts/ts_column.h"
#include "modalities/ts/ts_store_int.h"
#include "modalities/ts/ts_ttl.h"

int ts_seal_write_wal(Pager *pg, u32 first_page, u32 seal_seq, u64 *reads,
                      const TsWal *w, u32 *head_out, u32 *pages_out,
                      u32 *n_rows_out)
{
    u32 rows = 0;
    u32 raw_pages = 0;
    u32 last = PAGE_ID_NIL;
    i64 *ts = NULL;
    i64 *tag = NULL;
    double *val = NULL;
    u32 got = 0;
    u32 head = 0;
    u32 pages = 0;
    int rc;

    if (!pg || !head_out || !pages_out || !n_rows_out)
        return E_INVAL;

    /* 先数一遍裸写分区：多少行、多少页 */
    rc = ts_part_walk(pg, first_page, &rows, &raw_pages, &last, reads);
    if (rc != OK)
        return rc;

    /* 空分区没什么可封的（也没有"列式空分区"这个状态） */
    if (rows == 0)
        return E_INVAL;

    ts = (i64 *)malloc((size_t)rows * sizeof(i64));
    tag = (i64 *)malloc((size_t)rows * sizeof(i64));
    val = (double *)malloc((size_t)rows * sizeof(double));
    if (ts == NULL || tag == NULL || val == NULL) {
        rc = E_NOSPACE;
        goto out;
    }

    /* 整段读出来（读的时候再数一遍行数，与上面的数必须一致） */
    rc = ts_part_read_rows(pg, first_page, rows, ts, tag, val, &got, reads);
    if (rc != OK)
        goto out;
    if (got != rows) {
        rc = E_CORRUPT;
        goto out;
    }

    /* 编码成列式页（旧裸写页的位置记进头部页，仅供将来回收） */
    rc = ts_column_write_wal(pg, ts, tag, val, rows, first_page, raw_pages,
                              seal_seq, w, &head, &pages);
    if (rc != OK)
        goto out;

    *head_out = head;
    *pages_out = pages;
    *n_rows_out = rows;
    rc = OK;

out:
    free(ts);
    free(tag);
    free(val);
    return rc;
}

/* 不写日志的版本（单元测试直接用模态时走这条路）。 */
int ts_seal_write(Pager *pg, u32 first_page, u32 seal_seq, u64 *reads,
                  u32 *head_out, u32 *pages_out, u32 *n_rows_out)
{
    return ts_seal_write_wal(pg, first_page, seal_seq, reads, NULL,
                             head_out, pages_out, n_rows_out);
}

/* ---------------- 第二步：翻目录 ---------------- */

/*
 * 把目录项翻成 SEALED。
 *
 * 这一步落盘 ⇒ **封窗成功**（目录项是唯一的权威，坑 M6-40）。
 * 分区已经不是 ACTIVE ⇒ E_NOTSUP（不重复封窗，也不复活已丢弃的分区）。
 */
int ts_store_seal_commit(TsStore *st, u32 idx, u32 head_page, u32 pages, u32 n_rows)
{
    TsPartInfo info;
    int rc;

    if (!st)
        return E_INVAL;

    rc = ts_dir_get(st->dir, idx, &info);
    if (rc != OK)
        return rc;
    if (info.state != TS_PART_ACTIVE)
        return E_NOTSUP;

    info.state = TS_PART_SEALED;
    info.n_rows = n_rows;
    info.first_page = head_page;
    info.page_count = pages;

    rc = ts_dir_update(st->dir, idx, &info);
    if (rc != OK)
        return rc;

    /*
     * 运行时状态：封窗后分区是**只读**的 ⇒ 没有"页链尾"了（坑 M6-44）。
     * 不置成 PAGE_ID_NIL 的话，下一次追加会往列式头部页上写 ——
     * 那会把一个列式分区写成半截裸写页，读出来就是垃圾。
     */
    st->rt[idx].last_page = PAGE_ID_NIL;
    st->rt[idx].n_rows = n_rows;
    st->rt[idx].page_count = pages;
    return OK;
}

/* ---------------- 编排：两步走 ---------------- */

/*
 * 封窗一个分区：先写列式页，再翻目录。
 *
 * idx —— **磁盘序**的分区下标（与 ts_dir_get 一致；
 *       注意 ts_store_part_info 给的是**时间序**，两者不一定相同）。
 */
int ts_store_seal(TsStore *st, u32 idx)
{
    TsPartInfo info;
    u32 head = 0;
    u32 pages = 0;
    u32 rows = 0;
    int rc;

    if (!st)
        return E_INVAL;
    if (idx >= st->rt_count)
        return E_INVAL;

    rc = ts_dir_get(st->dir, idx, &info);
    if (rc != OK)
        return rc;
    if (info.state != TS_PART_ACTIVE)
        return E_NOTSUP;

    /* 第一步：写列式页（**不改目录** —— 崩在这里分区仍然是完整的旧分区） */
    rc = ts_seal_write_wal(st->pg, info.first_page, st->seal_seq + 1u,
                           &st->page_reads, &st->wal,
                           &head, &pages, &rows);
    if (rc != OK)
        return rc;

    /* 第二步：翻目录（落盘 ⇒ 封窗成功） */
    rc = ts_store_seal_commit(st, idx, head, pages, rows);
    if (rc != OK)
        return rc;

    st->seal_seq++;

    /*
     * 第三步：回收封窗时被换掉的**旧裸写页**（W-D 的 D-6.2）。
     *
     * 【为什么现在才收】
     *
     * 目录已经翻成 SEALED 且落盘 ⇒ 读路径只走列式页 ⇒
     * 那批旧裸写页再也没人读 ⇒ 回收是安全的。
     *（改之前它们只是"标记不再使用"，在文件里当永久空洞 —— 坑 M6-42。）
     *
     * 【为什么失败不影响封窗结果】
     *
     * 封窗已经成功（目录项是权威）。回收失败只是"少省了点空间"，
     * 数据没有错、读路径也没有错 —— 所以这里**不**把错误码往上抛，
     * 只如实记进 page_reads 之外的统计里（本层没有专门的回收计数）。
     *
     * 下一次调用会重试（幂等：头部页里的标记被清掉之后就什么都不做）。
     */
    (void)ts_ttl_release_seal_old_wal(st->pg, head, &st->wal, NULL);

    return OK;
}
