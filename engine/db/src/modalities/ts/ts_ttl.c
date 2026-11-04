/*
 * ts_ttl.c —— TTL 整分区过期回收（v1.5 步骤 4 · T8）
 *
 * 契约见 ts_ttl.h。
 *
 * 【这一层做什么】
 *
 *   1. 找出结束时间早于 now − TTL 的分区；
 *   2. 把目录项改成 DROPPED（**落盘**）；
 *   3. 把这个分区的页清零并归还全局页分配器。
 *
 * 顺序不能反，理由见 ts_ttl.h 的"坑 M6-49"。
 *
 * 【回收的页去哪儿】
 *
 * 计划判决 4：进**全局页分配器的空闲**（页号空间共享，
 * 下一个分配页的任何结构都可能拿到它），**不是**某张表的 freelist。
 *
 * 本内核原来**没有**这个对侧 —— pager_alloc 只会往文件尾追加页，
 * 没有地方"还回去"。所以这一步顺带补上了 pager_free()
 * （storage/pager_free.c）：它挂在 pager 上，**不属于任何表**。
 *
 * 【两种物理形态，分开处理】
 *
 *   活跃分区：裸写页链（PT_TS_PART，用 TSSP_NEXT 串）
 *   封窗分区：列式页（PT_TS_COL：1 张头部页 + 3 条列链，列链用 H_NEXT 串）
 *
 * 走错形态的后果：把列式页当成裸写链走 ⇒ 顺着 TSSP_NEXT 读到垃圾
 * ⇒ 回收了**别人的页**。所以每一步都核对页类型与标志，
 * 页数还必须与目录里记的 page_count 相等（坑 M6-51）。
 */

#include "modalities/ts/ts_ttl.h"

#include <stdlib.h>
#include <string.h>

#include "storage/page.h"

/* ---------------- 阈值 ---------------- */

/*
 * 过期阈值 = now − TTL（毫秒），饱和计算，免得 i64 溢出。
 *
 * 保留期大到不可能过期（或者 now 是极端负数）⇒ 阈值取 INT64_MIN
 * ⇒ 没有任何分区会过期。
 */
static i64 ttl_threshold(i64 now_ms, u64 ttl_sec)
{
    i64 ttl_ms;

    if (ttl_sec > (u64)INT64_MAX / 1000u)
        return INT64_MIN;
    ttl_ms = (i64)ttl_sec * 1000;
    if (ttl_ms > 0 && now_ms < INT64_MIN + ttl_ms)
        return INT64_MIN;
    return now_ms - ttl_ms;
}

/* ---------------- 回收页 ---------------- */

/*
 * 归还一页：先清零，再交给全局页分配器。
 *
 * 清零的用意：下一个主人拿到的是一张**空页**，
 * 而不是上一个主人的残留。
 * 万一有谁还拿着旧引用，读到的是"空页"（页类型不对 ⇒ 报错），
 * 而不是**别人的数据**（静默错）。
 *
 * 步骤 6：清零也是一次**改页** ⇒ 同样进 WAL（整页镜像）。
 * 这样"这页已经空了"这件事也有据可查：重放时把它再清一次，
 * 得到的页与当初清零后的页**逐字节相同**。
 *
 * ⚠️ 清零**不能**代替"目录项改成 DROPPED"：
 *    页空了但目录项还说这个分区在 ⇒ 读路径会读到空页（E_CORRUPT），
 *    而不是"分区已过期"。权威永远是目录项（坑 M6-49、M6-63）。
 */
static int retire_page(Pager *pg, u32 pid, const TsWal *w, u32 *freed)
{
    PageFrame *fr = NULL;
    u8 before[PAGE_SIZE];
    int rc;

    rc = pager_get(pg, pid, &fr);
    if (rc != OK)
        return rc;

    ts_wal_snap(fr->data, before);
    memset(fr->data, 0, PAGE_SIZE);
    rc = ts_pg_commit(pg, pid, fr, w, before);
    if (rc != OK)
        return rc;

    rc = pager_free(pg, pid);
    if (rc != OK)
        return rc;

    (*freed)++;
    return OK;
}

/*
 * 裸写分区的页链：首页 → TSSP_NEXT → …
 *
 * expect —— 目录里记的页数；链长必须与它**正好相等**。
 */
static int release_raw_chain(Pager *pg, u32 first, u32 expect, const TsWal *w,
                             u32 *freed)
{
    u32 page = first;
    u32 seen = 0;

    while (page != PAGE_ID_NIL) {
        PageFrame *fr = NULL;
        u32 next;
        int rc;

        if (seen >= expect)
            return E_CORRUPT;   /* 链比目录里记的长 */

        rc = pager_get(pg, page, &fr);
        if (rc != OK)
            return rc;

        if (fr->data[H_TYPE] != (u8)PT_TS_PART ||
            !page_verify_checksum(fr->data)) {
            (void)pager_unpin(pg, page, false);
            return E_CORRUPT;   /* 不是分区数据页 / 页被撕坏了 */
        }

        next = get_u32(fr->data + TSSP_NEXT);
        rc = pager_unpin(pg, page, false);
        if (rc != OK)
            return rc;

        seen++;

        rc = retire_page(pg, page, w, freed);
        if (rc != OK)
            return rc;

        page = next;
    }

    if (seen != expect)
        return E_CORRUPT;       /* 链比目录里记的短 */
    return OK;
}

/*
 * 列式分区的页：1 张头部页 + 三条列链（数据页用 H_NEXT 串）。
 *
 * expect —— 目录里记的页数（含头部页）；必须与实际回收数相等。
 */
static int release_column_part(Pager *pg, u32 head, u32 expect, const TsWal *w,
                               u32 *freed)
{
    u32 first[3];
    u32 pages[3];
    u32 total;
    u32 seen = 0;
    u32 col;
    int rc;

    /* 先读头部页：拿到三列的首页与页数 */
    {
        PageFrame *fr = NULL;
        u32 npages;

        rc = pager_get(pg, head, &fr);
        if (rc != OK)
            return rc;

        if (fr->data[H_TYPE] != (u8)PT_TS_COL ||
            !page_verify_checksum(fr->data) ||
            get_u32(fr->data + TSC_FLAGS) != TS_COL_FLAG_HEADER) {
            (void)pager_unpin(pg, head, false);
            return E_CORRUPT;
        }

        npages = get_u32(fr->data + TSC_NPAGES);
        first[TSCOL_TIME] = get_u32(fr->data + TSC_TFIRST);
        first[TSCOL_TAG] = get_u32(fr->data + TSC_GFIRST);
        first[TSCOL_VALUE] = get_u32(fr->data + TSC_VFIRST);
        pages[TSCOL_TIME] = get_u32(fr->data + TSC_TPAGES);
        pages[TSCOL_TAG] = get_u32(fr->data + TSC_GPAGES);
        pages[TSCOL_VALUE] = get_u32(fr->data + TSC_VPAGES);

        rc = pager_unpin(pg, head, false);
        if (rc != OK)
            return rc;

        total = npages + 1u;    /* 数据页 + 头部页 */
        if (total != expect)
            return E_CORRUPT;   /* 头部页说的页数与目录对不上 */
    }

    /* 走三条列链 */
    for (col = 0; col < 3u; col++) {
        u32 page = first[col];
        u32 n = 0;

        while (page != PAGE_ID_NIL) {
            PageFrame *fr = NULL;
            u32 next;
            int r2;

            if (n >= pages[col])
                return E_CORRUPT;   /* 链比头部页记的长 */

            r2 = pager_get(pg, page, &fr);
            if (r2 != OK)
                return r2;

            if (fr->data[H_TYPE] != (u8)PT_TS_COL ||
                !page_verify_checksum(fr->data) ||
                get_u32(fr->data + TSC_FLAGS) != TS_COL_FLAG_DATA) {
                (void)pager_unpin(pg, page, false);
                return E_CORRUPT;
            }

            next = get_u32(fr->data + H_NEXT);
            r2 = pager_unpin(pg, page, false);
            if (r2 != OK)
                return r2;

            n++;
            seen++;

            r2 = retire_page(pg, page, w, freed);
            if (r2 != OK)
                return r2;

            page = next;
        }

        if (n != pages[col])
            return E_CORRUPT;   /* 链比头部页记的短 */
    }

    /* 最后回收头部页（它记着三列的首页） */
    rc = retire_page(pg, head, w, freed);
    if (rc != OK)
        return rc;
    seen++;

    if (seen != expect)
        return E_CORRUPT;
    return OK;
}

int ts_ttl_release_pages_wal(Pager *pg, TsPartState state, u32 first_page,
                             u32 page_count, const TsWal *w, u32 *freed_out)
{
    u32 freed = 0;
    int rc;

    if (freed_out)
        *freed_out = 0;
    if (!pg)
        return E_INVAL;

    if (first_page == PAGE_ID_NIL)
        return OK;             /* 空分区：没有页可回收 */
    if (page_count == 0u)
        return E_CORRUPT;      /* 有首页却没页数：不变量破了 */

    if (state == TS_PART_SEALED) {
        rc = release_column_part(pg, first_page, page_count, w, &freed);
    } else {
        rc = release_raw_chain(pg, first_page, page_count, w, &freed);
    }
    if (rc != OK)
        return rc;

    if (freed_out)
        *freed_out = freed;
    return OK;
}

/* 不写日志的版本（单元测试直接用模态时走这条路）。 */
int ts_ttl_release_pages(Pager *pg, TsPartState state, u32 first_page,
                          u32 page_count, u32 *freed_out)
{
    return ts_ttl_release_pages_wal(pg, state, first_page, page_count, NULL,
                                    freed_out);
}

/* ---------------- TTL 主流程 ---------------- */

int ts_ttl_expire(Pager *pg, TsDir *d, i64 now_ms, u64 ttl_sec,
                  TsTtlStats *stats)
{
    TsTtlStats st;
    i64 thr;
    u32 n;
    u32 i;
    const TsWal *w;
    int rc;

    if (!pg || !d)
        return E_INVAL;

    memset(&st, 0, sizeof(st));

    /*
     * 写日志上下文从**目录**那里拿（步骤 6）。
     *
     * 目录对象自己存着一份（建目录时挂上的）——
     * TTL 这一层没有"存储"对象，只有目录。
     */
    w = ts_dir_wal(d);

    thr = ttl_threshold(now_ms, ttl_sec);

    n = ts_dir_count(d);
    for (i = 0; i < n; i++) {
        TsPartInfo info;
        TsPartInfo upd;
        u32 freed = 0;

        rc = ts_dir_get(d, i, &info);
        if (rc != OK)
            return rc;

        /* 幂等：已经是 DROPPED 的分区不碰（页早就还回去了） */
        if (info.state == TS_PART_DROPPED) {
            st.already_dropped++;
            continue;
        }

        /* 结束时间不早于阈值 ⇒ 还没到期 */
        if (info.end_ms >= thr) {
            st.kept_parts++;
            continue;
        }

        /*
         * 到期了。
         *
         * 第一步：目录项改成 DROPPED，同时清空首页与页数
         *（坑 M6-50：目录不能再指着已经回收的页）。
         */
        upd = info;
        upd.state = TS_PART_DROPPED;
        upd.first_page = PAGE_ID_NIL;
        upd.page_count = 0;

        rc = ts_dir_update(d, i, &upd);
        if (rc != OK)
            return rc;

        /*
         * 第二步：**落盘**。
         *
         * "落盘"必须是真的写进文件（pager_flush_all），
         * 不能只是"标脏等以后刷" ——
         * 否则崩在刷盘之前，磁盘上的目录项还是"这个分区还在"，
         * 而页已经被回收了 ⇒ 读到别人的数据（坑 M6-49）。
         *
         * 步骤 6：目录项的改动**也进了 WAL**（ts_dir_update 里写日志），
         * 而 pager_flush_all 刷目录页之前会先问 WAL 钩子
         * "这条记录落盘了吗" ⇒ 没落盘就先落盘（WAL 规则）。
         * 所以"目录说已过期"这件事在盘上永远是**先于**"页被回收"的 ✓。
         */
        rc = pager_flush_all(pg);
        if (rc != OK)
            return rc;

        /*
         * 第三步：回收页（清零 + 归还全局页分配器）。
         *
         * ⚠️ 封窗分区的**旧裸写页**（头部页里记着的那一段）本层**不收**：
         * 那是步骤 3 的封窗流程该收的（它知道那批页已经被换掉了）。
         * 本层只收"这个分区现在正在用的页"。
         */
        rc = ts_ttl_release_pages_wal(pg, info.state, info.first_page,
                                      info.page_count, w, &freed);
        if (rc != OK)
            return rc;

        st.expired_parts++;
        st.freed_pages += freed;
    }

    if (stats)
        *stats = st;
    return OK;
}
