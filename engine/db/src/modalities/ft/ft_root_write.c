/*
 * ft_root_write.c —— 根页与目录页的**写**（从 ft_root.c 拆出）
 *
 * 原 ft_root.c 871 行，超了 800 行硬规则。拆出来的是**目录的写路径**：
 *
 *   ft_cat_page_write      写一张目录页
 *   ft_cat_split           条目切页（坑 M4-35：段在前、tomb 在后，零头让位）
 *   ft_root_chain_release  整条旧目录链的页还给供给方（坑 M4-53）
 *   ft_root_page_alloc     取一页：优先外部供给，否则向文件尾追加
 *   ft_root_write_ex       写一版新目录（换根 + 回收旧链）
 *   ft_root_write          单页包装
 *
 * 留在 ft_root.c 的是**读与定位**：磁盘布局、读根页、建根页 / 绑定、
 * 读目录链、读目录。
 *
 * ⚠️ 跨文件的依赖：本文件用 ft_root.c 里的 ft_root_commit /
 * ft_root_page_write（已去 static），声明在 ft_root_internal.h。
 * 尺寸常量也在这个内部头里 —— 读写两侧都要用（读侧校验页头里的条目数）。
 */

#include "modalities/ft/ft_root.h"
#include "modalities/ft/ft_root_internal.h"

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "storage/page.h"

/*
 * 写一张目录页。
 *
 * ns / nt —— 本页放多少段条目 / tombstone 条目；soff / toff —— 它们在调用方
 * 数组里的起始下标；next —— 链上的下一页（PAGE_ID_NIL = 最后一页）。
 */
static int ft_cat_page_write(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                             u32 pid, PageFrame *fr,
                             const FtRootSeg *segs, u32 soff, u32 ns,
                             const FtRootTomb *tombs, u32 toff, u32 nt,
                             u32 prev_cat, u32 next, u32 *lsn_out)
{
    u8 *buf = NULL;
    u8 *p;
    int rc;

    buf = (u8 *)malloc(PAGE_SIZE);
    if (buf == NULL) {
        pager_unpin(pg, pid, false);
        return E_NOSPACE;
    }

    page_init(buf, (u8)FT_CAT_PTYPE);
    buf[FTC_VER] = (u8)FT_CAT_VER;
    buf[FTC_FLAGS] = (u8)(next != PAGE_ID_NIL ? 1u : 0u);
    put_u32(buf + FTC_PREV, prev_cat);
    put_u32(buf + FTC_NSEG, ns);
    put_u32(buf + FTC_NTOMB, nt);
    put_u32(buf + H_NEXT, next);

    p = buf + FTC_DATA;
    for (u32 i = 0; i < ns; i++) {
        put_u32(p, segs[soff + i].seg_id);
        put_u32(p + 4, segs[soff + i].head_page);
        put_u32(p + 8, segs[soff + i].state);
        p += FT_SEG_ENT;
    }
    for (u32 i = 0; i < nt; i++) {
        put_u32(p, tombs[toff + i].page_id);
        put_u32(p + 4, tombs[toff + i].ordinal);
        p += FT_TOMB_ENT;
    }

    rc = ft_root_commit(pg, wal, wal_ctx, xid, pid, fr, buf, lsn_out);
    free(buf);
    return rc;
}

/* ---------------- 写一版新目录 ---------------- */

/*
 * 把条目切到目录页上。
 *
 * 每页条目区 FT_CAT_CAP 字节：段条目 12 字节、tombstone 条目 8 字节。
 * 规则：一页先放段条目，再放 tombstone 条目，还放不下就开新页。
 * **段在前、tomb 在后**，读回来的次序与写进去的一致。
 *
 * 【坑 M4-35：不能让两页都剩零头】段条目 12 字节、tombstone 条目 8 字节，
 * 除不尽会剩几个字节；零头不够放一个最小条目（8 字节）时，
 * 就把段条目让出一个（12 字节）给 tombstone 用 ——
 * 不然会出现"一页放不下任何条目"的死循环。
 */
static int ft_cat_split(u32 n_segs, u32 n_tombs,
                        u32 *seg_off, u32 *seg_cnt,
                        u32 *tomb_off, u32 *tomb_cnt,
                        u32 max_pages, u32 *n_pages)
{
    u32 si = 0;
    u32 ti = 0;
    u32 np = 0;

    while (si < n_segs || ti < n_tombs) {
        u32 space = FT_CAT_CAP;
        u32 ns, nt, rest;

        if (np >= max_pages)
            return E_NOSPACE;

        ns = (n_segs - si < space / FT_SEG_ENT) ? (n_segs - si) : (space / FT_SEG_ENT);
        rest = space - ns * FT_SEG_ENT;
        if (rest < FT_TOMB_ENT && ti < n_tombs && ns > 0) {
            ns--;                       /* 让出一个段条目的位置给 tombstone */
            rest = space - ns * FT_SEG_ENT;
        }
        nt = (n_tombs - ti < rest / FT_TOMB_ENT) ? (n_tombs - ti) : (rest / FT_TOMB_ENT);

        seg_off[np] = si;
        seg_cnt[np] = ns;
        tomb_off[np] = ti;
        tomb_cnt[np] = nt;
        si += ns;
        ti += nt;
        np++;
    }

    *n_pages = np;
    return OK;
}

/*
 * 把一条目录链的页全部回收（顺着页链 H_NEXT 走）。
 *
 * 坑 M4-53：目录链的页不再"只涨不还" —— 链的页也从空闲页池里取，
 * 旧链的页还回池里 → 文件不再随目录更新次数线性增长。
 */
static void ft_root_chain_release(Pager *pg, u32 head, const FtRootPageOps *ops)
{
    u32 pid = head;
    u32 guard = 0;

    if (ops == NULL || ops->release == NULL) {
        return;
    }
    while (pid != PAGE_ID_NIL && guard++ < FT_MAX_CAT_PAGES) {
        PageFrame *fr = NULL;
        u32 next;

        if (pager_get(pg, pid, &fr) != OK) {
            return;      /* 页读不出来：链断了，剩下的不敢动 */
        }
        next = get_u32(fr->data + H_NEXT);
        pager_unpin(pg, pid, false);
        ops->release(ops->ctx, pid);
        pid = next;
    }
}

/* 取一页：优先走外部供给（空闲页池），没有就直接向文件尾追加 */
static int ft_root_page_alloc(Pager *pg, const FtRootPageOps *ops,
                              u32 *pid_out, PageFrame **fr_out)
{
    if (ops != NULL && ops->alloc != NULL) {
        return ops->alloc(ops->ctx, pid_out, fr_out);
    }
    return pager_alloc(pg, pid_out, fr_out);
}

int ft_root_write_ex(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                     FtRootState *st,
                     const FtRootSeg *segs, u32 n_segs,
                     const FtRootTomb *tombs, u32 n_tombs,
                     const FtRootPageOps *ops)
{
    u32 *seg_off = NULL;
    u32 *seg_cnt = NULL;
    u32 *tomb_off = NULL;
    u32 *tomb_cnt = NULL;
    u32 max_pages = 0, n_pages = 0;
    u32 cur_gen = 0, cur_slot = 1, prev_cat = PAGE_ID_NIL;
    u32 target_slot, target_pid, new_gen;
    u32 cat_head = PAGE_ID_NIL, last_lsn = 0;
    u32 old_cat = PAGE_ID_NIL;
    PageFrame *fr = NULL;
    PageFrame *prev_fr = NULL;
    u32 prev_pid = PAGE_ID_NIL;
    u32 prev_soff = 0, prev_scnt = 0, prev_toff = 0, prev_tcnt = 0;
    int rc;

    if (pg == NULL || st == NULL)
        return E_INVAL;
    if ((n_segs > 0 && segs == NULL) || (n_tombs > 0 && tombs == NULL))
        return E_INVAL;
    if (n_segs > FT_MAX_ENTRIES || n_tombs > FT_MAX_ENTRIES)
        return E_INVAL;

    /* 定位当前根（顺带刷新 st） */
    rc = ft_root_load(pg, st);
    if (rc != OK && rc != E_NOTFOUND)
        return rc;
    if (st->loaded) {
        cur_gen = st->generation;
        cur_slot = st->cur_slot;
        prev_cat = st->cat_head;
    }

    /* 新一代写到"当前根的另一张"上 */
    target_slot = 1u - cur_slot;
    target_pid = (target_slot == 0) ? st->root_a : st->root_b;
    if (target_pid == PAGE_ID_NIL)
        return E_INVAL;                  /* 还没绑定根页 */
    new_gen = cur_gen + 1u;

    /* 切页 */
    max_pages = (n_segs * FT_SEG_ENT + n_tombs * FT_TOMB_ENT) / FT_CAT_CAP + 2u;
    seg_off = (u32 *)malloc(sizeof(u32) * (size_t)max_pages);
    seg_cnt = (u32 *)malloc(sizeof(u32) * (size_t)max_pages);
    tomb_off = (u32 *)malloc(sizeof(u32) * (size_t)max_pages);
    tomb_cnt = (u32 *)malloc(sizeof(u32) * (size_t)max_pages);
    if (seg_off == NULL || seg_cnt == NULL || tomb_off == NULL || tomb_cnt == NULL) {
        rc = E_NOSPACE;
        goto done;
    }

    rc = ft_cat_split(n_segs, n_tombs, seg_off, seg_cnt, tomb_off, tomb_cnt,
                      max_pages, &n_pages);
    if (rc != OK)
        goto done;

    /*
     * 写目录链。一次只 pin 两页（当前页 + 上一页）——
     * 写上一页时得先知道这一页的页号（链是向前指的）。
     */
    for (u32 i = 0; i < n_pages; i++) {
        u32 pid = PAGE_ID_NIL;

        rc = ft_root_page_alloc(pg, ops, &pid, &fr);
        if (rc != OK) {
            if (prev_fr != NULL)
                pager_unpin(pg, prev_pid, false);
            goto done;
        }

        if (prev_fr != NULL) {
            /* 上一页的"下一页"就是刚分配的 pid */
            u32 lsn = 0;

            rc = ft_cat_page_write(pg, wal, wal_ctx, xid, prev_pid, prev_fr,
                                   segs, prev_soff, prev_scnt,
                                   tombs, prev_toff, prev_tcnt,
                                   prev_cat, pid, &lsn);
            if (rc != OK) {
                pager_unpin(pg, pid, false);
                goto done;
            }
            last_lsn = lsn;
            prev_fr = NULL;
        }

        if (cat_head == PAGE_ID_NIL)
            cat_head = pid;

        prev_pid = pid;
        prev_fr = fr;
        prev_soff = seg_off[i];
        prev_scnt = seg_cnt[i];
        prev_toff = tomb_off[i];
        prev_tcnt = tomb_cnt[i];
    }

    /* 最后一页：没有下一页 */
    if (prev_fr != NULL) {
        u32 lsn = 0;

        rc = ft_cat_page_write(pg, wal, wal_ctx, xid, prev_pid, prev_fr,
                               segs, prev_soff, prev_scnt,
                               tombs, prev_toff, prev_tcnt,
                               prev_cat, PAGE_ID_NIL, &lsn);
        if (rc != OK)
            goto done;
        last_lsn = lsn;
        prev_fr = NULL;
    }

    /* 翻根页：指向新链，generation + 1 */
    rc = pager_get(pg, target_pid, &fr);
    if (rc != OK)
        goto done;

    /*
     * 记下"这一张根页原来指着的那条链"——
     * 翻页成功后它就没人指了，可以回收（坑 M4-53）。
     *
     * 读的是**马上要被覆盖**的这一页自己的字段（它此刻还 pin 在手上）。
     * 不是根页 → 当作"没有旧链"（第一次写时就是这样）。
     */
    old_cat = PAGE_ID_NIL;
    if (fr->data[H_TYPE] == (u8)FT_ROOT_PTYPE && fr->data[FTR_VER] == (u8)FT_ROOT_VER) {
        old_cat = get_u32(fr->data + FTR_CAT_HEAD);
    }

    rc = ft_root_page_write(pg, wal, wal_ctx, xid, target_pid, fr,
                            target_slot, new_gen, cur_gen,
                            cat_head, n_pages, n_segs, n_tombs, prev_cat, last_lsn, NULL);
    if (rc != OK)
        goto done;

    st->cur_slot = target_slot;
    st->generation = new_gen;
    st->cat_head = cat_head;
    st->cat_pages = n_pages;
    st->loaded = true;

    /*
     * 【旧目录链回收】（坑 M4-53）
     *
     * 翻页**成功**之后才回收：翻页失败说明这一代没写进去，
     * 那条链还可能被回退路径读到（老的那一代还得是完整的）。
     *
     * 回收哪一条：**被覆盖的这一张根页原来指着的那条**（第 N-1 代那条）。
     * 翻完之后没人指它了 —— 而第 N 代那条链**不能**回收：
     * 另一张根页（回退路径）还要读它。
     *
     * ⚠️ 只能回收这一条：prev_cat 里记的那条（第 N-2 代）**上一轮已经回收过了**
     * —— 再回收一次就是"把还在用的页发出去"（那一轮的页可能已经被新链拿去用了）。
     */
    ft_root_chain_release(pg, old_cat, ops);

    rc = OK;

done:
    free(seg_off);
    free(seg_cnt);
    free(tomb_off);
    free(tomb_cnt);
    return rc;
}

/*
 * 老入口：目录链的页直接向文件尾追加，旧链不回收。
 *
 * 签名被单测固定住了，所以保留它 —— 语义与从前完全一致。
 * 接线方（ft_catalog.c）走 ft_root_write_ex（带页供给与旧链回收）。
 */
int ft_root_write(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                  FtRootState *st,
                  const FtRootSeg *segs, u32 n_segs,
                  const FtRootTomb *tombs, u32 n_tombs)
{
    return ft_root_write_ex(pg, wal, wal_ctx, xid, st, segs, n_segs, tombs, n_tombs, NULL);
}

