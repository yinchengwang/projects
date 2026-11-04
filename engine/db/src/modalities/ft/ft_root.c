/*
 * ft_root.c —— 根页（双页交替）+ 段目录页（v1.3 欠账 3 / 7）
 *
 * 【为什么开库不能再扫整个页空间】
 *
 * 现在段目录是"散落在段头页里的状态"（坑 M4-20）：
 * 重开库必须把整个页空间扫一遍才能把段找齐（坑 M4-22），
 * 开库时间随库大小线性增长 —— 库越大，开库越慢。
 *
 * 这一层把"段目录"变成一个**显式的、可定位的对象**：
 *
 *   根页（两张，固定交替）  —— 指向"当前这一版目录"
 *   段目录页（可跨多页）    —— 段元数据 + tombstone 页的位置
 *
 * 开库 = 读两张根页 → 挑 generation 大的那份 → 顺着链把目录读完。
 *
 * 【双页交替为什么能保证"断电也还有一份完整目录"】
 *
 * 关键在**写序**：
 *
 *   1. 先把新的目录链**整条写完**（每页都是 WAL 先行）；
 *   2. 再把根页写到"当前根的**另一张**"上，generation + 1。
 *
 * 【坑 M4-33：目录更新必须"先写完整条链，再翻根页"】
 *
 * 反过来（先翻根页，再写目录链）的话，根页会指向一条**还没写完**的链 ——
 * 断电后链是断的，根页可信但目录读不出来 → 目录就没了。
 * 而且这种损坏**校验和查不出来**（根页本身是好的），
 * 只能靠"根页写的目录链必须先完整落盘"来保证。
 *
 * 于是任何时刻断电，磁盘上的状态只有三种：
 *
 *   (a) 目录链写了一半、根页还没翻 → 根页仍指向**旧链** → 旧目录可读；
 *   (b) 根页正在被改写（写了一半）    → 这张根页坏了，但**另一张**
 *       （上一代）完好 → 回退到旧目录，可读；
 *   (c) 全部写完 → 新目录可读。
 *
 * 也就是说：**任何时刻，磁盘上至少有一份完整可读的目录**。
 * 读的时候如果大的那份坏了，就静默回退到旧的那份，**不报错**。
 *
 * 【旧目录链不回收】本轮**明确不回收**旧的目录链页（页留在文件里）：
 * 回收的前提是"确认没有读者还在读旧链"，这一层还没有读者计数/版本引用。
 * 代价是文件随目录更新次数增长 —— 本轮接受，注释写明以免后来人当成 bug。
 *
 * 【这一层与索引对象无关】不依赖 struct FtIndex，自带 FtRootState
 * （由调用方持有）；wave 2 接线时挂进 FtIndex 即可。
 */

#include "modalities/ft/ft_root.h"

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "storage/page.h"

/* ---------------- 磁盘布局 ---------------- */

/*
 * 根页与目录页的自定义区（偏移、字段含义）定义在 ft_root.h 里 ——
 * 那是这一层的**磁盘格式契约**，改它就是改格式。
 */

/* 段条目：seg_id + head_page + state = 12 字节 */
#define FT_SEG_ENT  12u
/* tombstone 条目：page_id + ordinal = 8 字节 */
#define FT_TOMB_ENT  8u

/* 目录页的条目区能放多少字节 */
#define FT_CAT_CAP  (PAGE_SIZE - FTC_DATA)

/* generation 的可信上限（超过就是"不可信"，当成坏页处理） */
#define FT_GEN_MAX  0x7FFFFFFFu

/* 一次目录更新最多允许有多少页（防御：链断了也不能无限转） */
#define FT_MAX_CAT_PAGES  (1u << 20)
/* 条目数的可信上限 */
#define FT_MAX_ENTRIES    (1u << 20)

/* ---------------- 读出来的一张根页 ---------------- */

typedef struct {
    bool valid;      /* 这一张能不能信 */
    u32  slot;       /* 0 = A，1 = B */
    u32  gen;        /* generation */
    u32  prev_gen;   /* 上一代的 generation */
    u32  cat_head;   /* 目录链头 */
    u32  cat_pages;  /* 目录链页数 */
    u32  n_seg;      /* 段条目总数 */
    u32  n_tomb;     /* tombstone 条目总数 */
    u32  prev_cat;   /* 被取代的旧链头 */
    u32  lsn;        /* 目录最后一次写页的 LSN */
} FtRootView;

/*
 * 读一张根页并判断它可不可信。
 *
 * 不可信的情形（都当成"这张坏了"，让读方回退到另一张）：
 * 页读不出来 / 页类型不对 / 校验不过 / 版本不认识 /
 * generation 不可信 / 目录链页数与条目数明显不合理。
 *
 * 注意：**这一层不报错** —— 回退是正常路径，不是错误。
 */
static void ft_root_view_read(Pager *pg, u32 pid, u32 slot, FtRootView *v)
{
    PageFrame *fr = NULL;
    u32 n_seg, n_tomb, cat_pages;

    memset(v, 0, sizeof(*v));
    v->slot = slot;

    if (pg == NULL || pid == PAGE_ID_NIL)
        return;
    if (pager_get(pg, pid, &fr) != OK)
        return;

    if (fr->data[H_TYPE] != (u8)FT_ROOT_PTYPE)
        goto out;                          /* 不是根页 */
    if (!page_verify_checksum(fr->data))
        goto out;                          /* 校验不过：写坏了 */
    if (fr->data[FTR_VER] != (u8)FT_ROOT_VER)
        goto out;                          /* 版本不认识 */

    v->gen = get_u32(fr->data + FTR_GEN);
    if (v->gen > FT_GEN_MAX)
        goto out;                          /* generation 不可信 */

    v->prev_gen = get_u32(fr->data + FTR_PREV_GEN);
    v->cat_head = get_u32(fr->data + FTR_CAT_HEAD);
    cat_pages = get_u32(fr->data + FTR_CAT_PAGES);
    n_seg = get_u32(fr->data + FTR_NSEG);
    n_tomb = get_u32(fr->data + FTR_NTOMB);

    if (cat_pages > FT_MAX_CAT_PAGES || n_seg > FT_MAX_ENTRIES || n_tomb > FT_MAX_ENTRIES)
        goto out;                          /* 明显不合理：当成坏页 */

    v->cat_pages = cat_pages;
    v->n_seg = n_seg;
    v->n_tomb = n_tomb;
    v->prev_cat = get_u32(fr->data + FTR_PREV_CAT);
    v->lsn = get_u32(fr->data + FTR_LSN);
    v->valid = true;

out:
    pager_unpin(pg, pid, false);
}

/* ---------------- 写页 ---------------- */

/*
 * 写一页：**写日志 → 填 LSN → 算校验和 → 标脏放开**。
 *
 * 语义与 ft_writer.c 的 ft_page_write **逐条一致**：
 *   1. 前像 = 这一页**现在**的内容（不是全 0 —— 根页是反复改写的老页，
 *      前像必须是"改之前"的内容，否则未提交事务的修改撤不回去）；
 *   2. **先写 WAL**（LOG_PAGEDATA，整页镜像），日志写不好就**不许改页**；
 *   3. 把日志给的 LSN 填进页头（重放幂等判断的依据）；
 *   4. 算校验和 → 标脏 → 放开。
 *
 * 之所以自己写一份：这一层是**独立内核件**，而页的分配路径正在被另一条线
 * 改造（空闲页池）—— 自己写一份，这一层就不会被那边的签名变动带着改。
 *
 * 【WAL 先行的意义】顺序反过来（先改页再写日志）的话，页可能先于日志落盘
 * → 崩溃后日志里缺这段内容，而页上已经有它 → 数据"没根"。
 * 根页是目录的**唯一权威**，这种情况必须被排除。
 *
 * 【坑 M4-36：页头的 LSN 必须就是日志给的那个 LSN】
 *
 * 页头的 LSN 是重放幂等判断的依据（"这条记录我已经重放过没有"）。
 * 如果页上的 LSN 是编的（比如拿旧值填上去），
 * 重放就会跳过这条记录 → 目录停在旧的一代，而且**不报任何错**。
 */
static int ft_root_commit(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                          u32 pid, PageFrame *fr, const u8 *buf, u32 *lsn_out)
{
    u8 *before = NULL;
    u32 lsn = 0;
    int rc;

    if (pg == NULL || fr == NULL || buf == NULL)
        return E_INVAL;

    before = (u8 *)malloc(PAGE_SIZE);
    if (before == NULL) {
        pager_unpin(pg, pid, false);
        return E_NOSPACE;
    }

    /* 前像 = 这一页现在的内容 */
    memcpy(before, fr->data, PAGE_SIZE);
    memmove(fr->data, buf, PAGE_SIZE);   /* buf 可能就是 fr->data（自拷贝） */

    if (wal) {
        rc = wal(wal_ctx, xid, LOG_PAGEDATA, pid, PAGE_SLOT_NONE, LOG_NO_SLOT_OFF,
                 before, (u16)PAGE_SIZE, fr->data, (u16)PAGE_SIZE, &lsn);
        if (rc != OK) {
            /* 日志没写好 → 这一页不许改（还原成原样） */
            memcpy(fr->data, before, PAGE_SIZE);
            free(before);
            pager_unpin(pg, pid, false);
            return rc;
        }
    }
    free(before);

    page_set_lsn(fr->data, lsn);
    page_set_checksum(fr->data);

    rc = pager_unpin(pg, pid, true);
    if (rc != OK)
        return rc;

    if (lsn_out)
        *lsn_out = lsn;
    return OK;
}

/* 写一张根页（走上面那套 WAL 先行的写页路径） */
static int ft_root_page_write(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                              u32 pid, PageFrame *fr,
                              u32 slot, u32 gen, u32 prev_gen,
                              u32 cat_head, u32 cat_pages,
                              u32 n_seg, u32 n_tomb, u32 prev_cat, u32 lsn,
                              u32 *lsn_out)
{
    u8 *buf = NULL;
    int rc;

    buf = (u8 *)malloc(PAGE_SIZE);
    if (buf == NULL) {
        pager_unpin(pg, pid, false);
        return E_NOSPACE;
    }

    page_init(buf, (u8)FT_ROOT_PTYPE);
    buf[FTR_VER] = (u8)FT_ROOT_VER;
    buf[FTR_SLOT] = (u8)slot;
    put_u32(buf + FTR_GEN, gen);
    put_u32(buf + FTR_PREV_GEN, prev_gen);
    put_u32(buf + FTR_CAT_HEAD, cat_head);
    put_u32(buf + FTR_CAT_PAGES, cat_pages);
    put_u32(buf + FTR_NSEG, n_seg);
    put_u32(buf + FTR_NTOMB, n_tomb);
    put_u32(buf + FTR_PREV_CAT, prev_cat);
    put_u32(buf + FTR_LSN, lsn);

    rc = ft_root_commit(pg, wal, wal_ctx, xid, pid, fr, buf, lsn_out);
    free(buf);
    return rc;
}

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

/* ---------------- 建根页 / 绑定 / 定位当前根 ---------------- */

int ft_root_bind(FtRootState *st, u32 root_a, u32 root_b)
{
    if (st == NULL)
        return E_INVAL;

    st->root_a = root_a;
    st->root_b = root_b;
    st->cur_slot = 1;
    st->generation = 0;
    st->cat_head = PAGE_ID_NIL;
    st->cat_pages = 0;
    st->loaded = false;
    return OK;
}

int ft_root_create(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid, FtRootState *st)
{
    PageFrame *fr_a = NULL;
    PageFrame *fr_b = NULL;
    u32 pid_a = PAGE_ID_NIL;
    u32 pid_b = PAGE_ID_NIL;
    int rc;

    if (pg == NULL || st == NULL)
        return E_INVAL;

    rc = pager_alloc(pg, &pid_a, &fr_a);
    if (rc != OK)
        return rc;
    rc = pager_alloc(pg, &pid_b, &fr_b);
    if (rc != OK) {
        pager_unpin(pg, pid_a, false);
        return rc;
    }

    /* A = generation 1（当前根），B = generation 0（备用槽） */
    rc = ft_root_page_write(pg, wal, wal_ctx, xid, pid_a, fr_a,
                            0, 1u, 0u, PAGE_ID_NIL, 0u, 0u, 0u, PAGE_ID_NIL, 0u, NULL);
    if (rc != OK) {
        pager_unpin(pg, pid_b, false);
        return rc;
    }
    rc = ft_root_page_write(pg, wal, wal_ctx, xid, pid_b, fr_b,
                            1, 0u, 0u, PAGE_ID_NIL, 0u, 0u, 0u, PAGE_ID_NIL, 0u, NULL);
    if (rc != OK)
        return rc;

    st->root_a = pid_a;
    st->root_b = pid_b;
    st->cur_slot = 0;
    st->generation = 1u;
    st->cat_head = PAGE_ID_NIL;
    st->cat_pages = 0;
    st->loaded = true;
    return OK;
}

/*
 * 定位当前根页。
 *
 * 规则：两张都可信 → 取 generation 大的；只有一张可信 → 用那一张
 * （**这就是回退**，不报错）；两张 generation 一样 → 取 A（坑 M4-34）。
 *
 * 【坑 M4-34：两张根页 generation 相同是"没写完"的中间态】
 *
 * 正常情况下两张的 generation 一定不同（写一张时另一张还是老的）。
 * 相同只可能是"初始化时都写成 0"或有人手动改过。
 * 这时取 A 是约定，不是判断 —— 后果是"可能读到旧的一代"，
 * 而不会读到"半张页"。
 */
int ft_root_load(Pager *pg, FtRootState *st)
{
    FtRootView va;
    FtRootView vb;
    const FtRootView *v;

    if (pg == NULL || st == NULL)
        return E_INVAL;

    ft_root_view_read(pg, st->root_a, 0, &va);
    ft_root_view_read(pg, st->root_b, 1, &vb);

    if (va.valid && vb.valid)
        v = (va.gen >= vb.gen) ? &va : &vb;
    else if (va.valid)
        v = &va;
    else if (vb.valid)
        v = &vb;
    else {
        /* 一份完整目录都没有：让调用方知道"得从零建" */
        st->loaded = false;
        st->generation = 0;
        st->cur_slot = 1;
        st->cat_head = PAGE_ID_NIL;
        st->cat_pages = 0;
        return E_NOTFOUND;
    }

    st->cur_slot = v->slot;
    st->generation = v->gen;
    st->cat_head = v->cat_head;
    st->cat_pages = v->cat_pages;
    st->loaded = true;
    return OK;
}

/* ---------------- 读目录链 ---------------- */

/*
 * 按根页的指引把整条目录链读完。
 *
 * 返回 E_CORRUPT 的情形（读方据此回退到旧的那一代）：
 * 链上的页类型不对 / 校验不过；链比根页记的页数长或走不到头；
 * 实际读到的条目数与根页记的对不上。
 */static int ft_cat_chain_read(Pager *pg, const FtRootView *v, FtRootCatalog *out)
{
    u32 pid, pages = 0, n_seg = 0, n_tomb = 0;
    int rc;

    out->n_segs = 0;
    out->n_tombs = 0;
    out->segs = NULL;
    out->tombs = NULL;
    out->n_cat_pages = 0;

    if (v->cat_head == PAGE_ID_NIL) {
        /* 空目录：根页说没有链，那条目数也必须是 0 */
        if (v->n_seg != 0 || v->n_tomb != 0)
            return E_CORRUPT;
        return OK;
    }

    if (v->n_seg > 0) {
        out->segs = (FtRootSeg *)malloc(sizeof(FtRootSeg) * (size_t)v->n_seg);
        if (out->segs == NULL)
            return E_NOSPACE;
    }
    if (v->n_tomb > 0) {
        out->tombs = (FtRootTomb *)malloc(sizeof(FtRootTomb) * (size_t)v->n_tomb);
        if (out->tombs == NULL) {
            free(out->segs);
            out->segs = NULL;
            return E_NOSPACE;
        }
    }

    pid = v->cat_head;
    while (pid != PAGE_ID_NIL) {
        PageFrame *fr = NULL;
        u32 ns, nt;
        const u8 *p;

        if (pages >= v->cat_pages || pages >= FT_MAX_CAT_PAGES) {
            rc = E_CORRUPT;              /* 链比记录的还长：断链了 */
            goto fail;
        }

        rc = pager_get(pg, pid, &fr);
        if (rc != OK) {
            rc = E_CORRUPT;              /* 链上的页读不出来 */
            goto fail;
        }

        if (fr->data[H_TYPE] != (u8)FT_CAT_PTYPE ||
            !page_verify_checksum(fr->data) ||
            fr->data[FTC_VER] != (u8)FT_CAT_VER) {
            pager_unpin(pg, pid, false);
            rc = E_CORRUPT;
            goto fail;
        }

        ns = get_u32(fr->data + FTC_NSEG);
        nt = get_u32(fr->data + FTC_NTOMB);
        /* 先卡住上界再算字节数 —— 免得 ns*12 溢出（坏页的 ns 可以是任意值） */
        if (ns > FT_CAT_CAP / FT_SEG_ENT || nt > FT_CAT_CAP / FT_TOMB_ENT ||
            n_seg + ns > v->n_seg || n_tomb + nt > v->n_tomb) {
            pager_unpin(pg, pid, false);
            rc = E_CORRUPT;
            goto fail;
        }

        p = fr->data + FTC_DATA;
        for (u32 i = 0; i < ns; i++) {
            out->segs[n_seg].seg_id = get_u32(p);
            out->segs[n_seg].head_page = get_u32(p + 4);
            out->segs[n_seg].state = get_u32(p + 8);
            n_seg++;
            p += FT_SEG_ENT;
        }
        for (u32 i = 0; i < nt; i++) {
            out->tombs[n_tomb].page_id = get_u32(p);
            out->tombs[n_tomb].ordinal = get_u32(p + 4);
            n_tomb++;
            p += FT_TOMB_ENT;
        }

        /* 先记下一页，再放开本页 —— 页号别覆盖（unpin 要用本页的号） */
        {
            u32 next_pid = get_u32(fr->data + H_NEXT);

            pager_unpin(pg, pid, false);
            pid = next_pid;
        }
        pages++;
    }

    if (pages != v->cat_pages || n_seg != v->n_seg || n_tomb != v->n_tomb) {
        rc = E_CORRUPT;
        goto fail;
    }

    out->n_segs = n_seg;
    out->n_tombs = n_tomb;
    out->n_cat_pages = pages;
    return OK;

fail:
    free(out->segs);
    free(out->tombs);
    out->segs = NULL;
    out->tombs = NULL;
    out->n_segs = 0;
    out->n_tombs = 0;
    return rc;
}

/* ---------------- 读目录 ---------------- */

void ft_root_catalog_free(FtRootCatalog *c)
{
    if (c == NULL)
        return;
    free(c->segs);
    free(c->tombs);
    c->segs = NULL;
    c->tombs = NULL;
    c->n_segs = 0;
    c->n_tombs = 0;
}

/* 挑候选：generation 大的先试（坑 M4-34：一样大时取 A） */
static int ft_root_pick(const FtRootView *va, const FtRootView *vb,
                        const FtRootView **cand)
{
    if (va->valid && vb->valid) {
        cand[0] = (va->gen >= vb->gen) ? va : vb;
        cand[1] = (va->gen >= vb->gen) ? vb : va;
        return 2;
    }
    if (va->valid) {
        cand[0] = va;
        return 1;
    }
    if (vb->valid) {
        cand[0] = vb;
        return 1;
    }
    return 0;
}

int ft_root_read(Pager *pg, const FtRootState *st, FtRootCatalog *out)
{
    FtRootView va;
    FtRootView vb;
    const FtRootView *cand[2];
    int ncand;
    int rc;

    if (pg == NULL || st == NULL || out == NULL)
        return E_INVAL;

    memset(out, 0, sizeof(*out));

    ft_root_view_read(pg, st->root_a, 0, &va);
    ft_root_view_read(pg, st->root_b, 1, &vb);

    ncand = ft_root_pick(&va, &vb, cand);
    if (ncand == 0)
        return E_NOTFOUND;

    /*
     * 依次试：大的那份读不通（根页坏了 / 目录链坏了）就退到小的那份。
     * 整个过程**不报错** —— 回退拿到的是一份完整的旧目录。
     */
    for (int i = 0; i < ncand; i++) {
        rc = ft_cat_chain_read(pg, cand[i], out);
        if (rc == OK) {
            out->generation = cand[i]->gen;
            out->root_slot = cand[i]->slot;
            return OK;
        }
        ft_root_catalog_free(out);
        memset(out, 0, sizeof(*out));
    }

    return E_CORRUPT;
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
