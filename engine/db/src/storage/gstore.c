/*
 * gstore.c — 定长槽页原语（v1.0 从 modalities/graph/ 下沉）
 *
 * 设计说明见 storage/gstore.h。
 *
 * 【WAL 策略】
 *
 * 定长槽页的改动走 WAL_PAGEDATA（整页镜像）—— 与 blob 同款。
 *
 * 为什么不用 LOG_INSERT/LOG_UPDATE（行级日志）：
 *   行级日志要记 slot + 页内偏移，undo 时按 offset 回写。
 *   定长槽页的槽位置是纯算术，不需要 offset 来定位 ——
 *   但行级日志的 undo 逻辑（判断"记录还在不在原位"）是按
 *   slotted page 的语义写的（slot 可能被 compact 挪走）。
 *   定长槽页不会 compact（记录位置永远不变），
 *   所以行级日志的那套判断反而是多余的复杂度。
 *
 *   整页镜像更简单，而且图页本来就要整体重写
 *   （改一个邻接指针 = 改一条链上的两个端点 = 2 个页）。
 *
 *   代价：改 1 个槽要写 4KB 日志。图的工作集小，可接受。
 */

#include "storage/gstore.h"

#include "common/crc.h"
#include "storage/page.h"

#include <stdlib.h>
#include <string.h>

/* 页头读写 / 槽寻址 / 槽内字段 —— 全部在 gstore.h 里做 static inline。
 * gstore.c 与 gstore_cursor.c 共用同一份实现，
 * 避免"页格式出现两份定义"这种必然会漂移的隐患。
 */

/* ---------------- 页初始化 ---------------- */

static void gs_init_page(u8 *pg, u8 page_type, u16 rec_size)
{
    memset(pg, 0, PAGE_SIZE);
    pg[GS_H_TYPE] = page_type;
    pg[GS_H_FLAGS] = 0;
    gs_set_rec_size(pg, rec_size);
    gs_set_nslots(pg, (u16)((PAGE_SIZE - GS_HDR_SIZE) / rec_size));
    gs_set_free_head(pg, GS_NO_SLOT);
    gs_set_next(pg, PAGE_ID_NIL);
    put_u32(pg + GS_H_LSN, 0);
    page_set_checksum(pg);
}

/* ---------------- WAL 钩子 ---------------- */

/*
 * 改页前写日志（整页镜像），改完把 LSN 写进页头。
 *
 * 调用顺序是硬要求：先写日志拿到 LSN → 再改页 → 把 LSN 写进页头。
 * 反过来的话页可能先于日志落盘 → 崩溃后日志里缺这段 → 静默数据丢失。
 *
 * 【page_id 必须是真实页号】
 *
 * WAL_PAGEDATA 的重放逻辑是"把 after 镜像盖到 r->page_id 那一页"。
 * 这里传 0 的话，崩溃恢复时整页镜像会盖到 page 0（元数据页）——
 * 元数据页被随机内容覆盖，整个库报废，而且**不报任何错**
 * （重放返回 OK，只是内容错了）。
 */
static int gs_log_page(GStore *gs, u32 xid, u32 page_id, const u8 *before,
                        const u8 *after, u32 *lsn_out)
{
    u32 lsn = 0;
    int rc = OK;

    if (gs->wal) {
        rc = gs->wal(gs->wal_ctx, xid, LOG_PAGEDATA, page_id,
                     PAGE_SLOT_NONE, LOG_NO_SLOT_OFF,
                     before, (u16)PAGE_SIZE,
                     after, (u16)PAGE_SIZE,
                     &lsn);
        if (rc != OK)
            return rc;
    }
    if (lsn_out)
        *lsn_out = lsn;
    return OK;
}

/*
 * 取页并做好写入准备。
 *
 * 返回时 frame 已 pin，调用方负责 unpin。
 * before_buf / after_buf 是调用方提供的镜像缓冲（PAGE_SIZE 字节）。
 */
static int gs_fetch(GStore *gs, u32 page_id, PageFrame **frame)
{
    int rc = pager_get(gs->pg, page_id, frame);
    if (rc != OK)
        return rc;
    return OK;
}

/*
 * 写页的统一出口：取 before 镜像 → 写日志 → 改页 → 写 LSN → unpin。
 *
 * mutator 是可选的：传 NULL 表示只读（不写日志、不标脏）。
 */
typedef int (*GsMutator)(void *ctx, u8 *pg);

static int gs_write_page(GStore *gs, u32 xid, u32 page_id,
                         GsMutator mut, void *mut_ctx)
{
    PageFrame *fr = NULL;
    int rc = gs_fetch(gs, page_id, &fr);
    if (rc != OK)
        return rc;

    u8 before[PAGE_SIZE];
    u8 after[PAGE_SIZE];
    memcpy(before, fr->data, PAGE_SIZE);

    if (mut) {
        rc = mut(mut_ctx, fr->data);
        if (rc != OK) {
            pager_unpin(gs->pg, page_id, false);
            return rc;
        }
    }
    memcpy(after, fr->data, PAGE_SIZE);

    /* 先写日志，拿到 LSN */
    u32 lsn = 0;
    rc = gs_log_page(gs, xid, page_id, before, after, &lsn);
    if (rc != OK) {
        /* 日志写失败 → 放弃这次修改，把页还原 */
        memcpy(fr->data, before, PAGE_SIZE);
        pager_unpin(gs->pg, page_id, false);
        return rc;
    }

    /* 把 LSN 写进页头 + 重算校验和 */
    put_u32(fr->data + GS_H_LSN, lsn);
    page_set_checksum(fr->data);

    pager_unpin(gs->pg, page_id, true);
    return OK;
}

/* ---------------- 构造 / 析构 ---------------- */

int gstore_create(Pager *pg, LogHookFn fn, void *ctx,
                  u16 rec_size, u8 page_type, GStore **out)
{
    if (!pg || !out || rec_size == 0)
        return E_INVAL;

    /* 记录必须放得下页头 + 至少一个槽 */
    if (GS_HDR_SIZE + rec_size > PAGE_SIZE)
        return E_INVAL;

    GStore *gs = calloc(1, sizeof(*gs));
    if (!gs)
        return E_IO;

    gs->pg = pg;
    gs->wal = fn;
    gs->wal_ctx = ctx;
    gs->rec_size = rec_size;
    gs->page_type = page_type;
    gs->head_page = PAGE_ID_NIL;
    gs->tail_page = PAGE_ID_NIL;
    gs->npages = 0;

    *out = gs;
    return OK;
}

void gstore_destroy(GStore *gs)
{
    free(gs);
}

u16 gstore_rec_size(const GStore *gs) { return gs ? gs->rec_size : 0; }
u8  gstore_page_type(const GStore *gs) { return gs ? gs->page_type : 0; }
u32 gstore_head_page(const GStore *gs) { return gs ? gs->head_page : PAGE_ID_NIL; }
u32 gstore_page_count(const GStore *gs) { return gs ? gs->npages : 0; }
u64 gstore_alive_count(GStore *gs) { return gs ? gs->alive : 0; }
u64 gstore_read_count(const GStore *gs) { return gs ? gs->rec_reads : 0; }

/* ---------------- 新开一页 ---------------- */

/*
 * 新开一页并挂到页链尾。
 *
 * 为什么要页链：全量遍历（gstore_foreach / 图的扫描）需要走遍所有页。
 * 页链把同类页串起来，遍历 = 从 head 顺着 next 走。
 *
 * 【和 heap 的页链的区别】
 * heap 的页链是"表的行按插入顺序串起来"。
 * gstore 的页链是"所有同类页串起来"，与记录顺序无关 ——
 * 因为定长槽页的槽位置本身就是寻址方式，遍历顺序不影响正确性。
 */
static int gs_new_page(GStore *gs, u32 xid, u32 *page_id_out, PageFrame **frame_out)
{
    u32 pid = 0;
    PageFrame *fr = NULL;
    int rc = pager_alloc(gs->pg, &pid, &fr);
    if (rc != OK)
        return rc;

    gs_init_page(fr->data, gs->page_type, gs->rec_size);

    /*
     * 链头页打 GS_FLAG_DIR 标志。
     *
     * 为什么需要：页链是单向的（head → next → ...），
     * 没有任何一页记着「我前面是谁」。重启后光看页链找不到头。
     *
     * 有了这个标志，gstore_recover 扫一遍全文件就能定位链头，
     * 顺着 next 把整条链重建出来（见 gstore.h 的说明）。
     *
     * 标志只加在**链头**上；新页挂到链尾时不能加。
     */
    bool is_head = (gs->head_page == PAGE_ID_NIL);
    if (is_head)
        fr->data[GS_H_FLAGS] |= GS_FLAG_DIR;

    /* 挂到链尾 */
    if (gs->tail_page != PAGE_ID_NIL) {
        /* 改尾页的 next 字段（单独一次页改写 + WAL） */
        PageFrame *tf = NULL;
        rc = pager_get(gs->pg, gs->tail_page, &tf);
        if (rc != OK) {
            pager_unpin(gs->pg, pid, true);
            return rc;
        }
        u8 before[PAGE_SIZE];
        memcpy(before, tf->data, PAGE_SIZE);
        gs_set_next(tf->data, pid);
        u32 lsn = 0;
        rc = gs_log_page(gs, xid, gs->tail_page, before, tf->data, &lsn);
        if (rc != OK) {
            memcpy(tf->data, before, PAGE_SIZE);
            pager_unpin(gs->pg, gs->tail_page, false);
            pager_unpin(gs->pg, pid, true);
            return rc;
        }
        put_u32(tf->data + GS_H_LSN, lsn);
        page_set_checksum(tf->data);
        pager_unpin(gs->pg, gs->tail_page, true);
    }

    if (is_head)
        gs->head_page = pid;
    gs->tail_page = pid;
    gs->npages++;

    if (page_id_out)
        *page_id_out = pid;
    if (frame_out)
        *frame_out = fr;      /* 仍 pin 着，交给调用方 */
    else
        pager_unpin(gs->pg, pid, true);
    return OK;
}

/* ---------------- 分配槽 ---------------- */

/*
 * 分配槽的公共实现。
 *
 * init != NULL 时，把 init 的载荷区（从 GS_REC_HDR_SIZE 起）一起写进新槽 ——
 * 这就是 gstore_alloc_with 的"分配即写全字段"。
 *
 * 为什么要"分配即写全"：
 *   调用方的空指针通常是显式值（如图的全 1 GR_RID_NIL），
 *   而 gstore 分配时把载荷清零 —— 全零对图来说是"指向 page 0"，
 *   是个**看起来合法**的指针。分两步（先分配再写）的话，
 *   中间那一瞬间槽是"在用但内容为零"的幽灵状态。
 *   崩在中间 = 图里多出一条指向 page 0 的幽灵边。
 *   所以这里一步到位：内容写好才让它可见（PLAN §1.7 R3）。
 */
static int gs_alloc(GStore *gs, u32 xid, const u8 *init, u16 len, Rid *out)
{
    if (!gs || !out)
        return E_INVAL;
    if (init && len != gs->rec_size)
        return E_INVAL;

    /* 先在已有页里找空闲槽 */
    u32 pid = gs->head_page;
    while (pid != PAGE_ID_NIL) {
        PageFrame *fr = NULL;
        int rc = pager_get(gs->pg, pid, &fr);
        if (rc != OK)
            return rc;

        u16 fh = gs_free_head(fr->data);
        if (fh != GS_NO_SLOT) {
            u16 nslots = gs_nslots(fr->data);
            if (fh >= nslots) {
                pager_unpin(gs->pg, pid, false);
                return E_CORRUPT;
            }
            u32 off = gs_slot_off(fr->data, fh);
            u8 *rec = fr->data + off;

            u16 next_free = gs_next_free(rec);

            /* 先写日志 */
            u8 before[PAGE_SIZE];
            memcpy(before, fr->data, PAGE_SIZE);

            /* 摘链 + 置 in_use */
            gs_set_free_head(fr->data, next_free);
            gs_set_in_use(rec, true);
            gs_set_next_free(rec, 0);
            memset(rec + GS_REC_HDR_SIZE, 0, gs->rec_size - GS_REC_HDR_SIZE);
            if (init)
                memcpy(rec + GS_REC_HDR_SIZE, init + GS_REC_HDR_SIZE,
                       gs->rec_size - GS_REC_HDR_SIZE);

            u32 lsn = 0;
            rc = gs_log_page(gs, xid, pid, before, fr->data, &lsn);
            if (rc != OK) {
                memcpy(fr->data, before, PAGE_SIZE);
                pager_unpin(gs->pg, pid, false);
                return rc;
            }
            put_u32(fr->data + GS_H_LSN, lsn);
            page_set_checksum(fr->data);

            Rid r = rid_make(pid, fh);
            pager_unpin(gs->pg, pid, true);
            gs->alive++;
            *out = r;
            return OK;
        }

        u32 next = gs_next(fr->data);
        pager_unpin(gs->pg, pid, false);
        pid = next;
    }

    /* 没有空闲槽 → 新开一页 */
    u32 newpid = 0;
    PageFrame *fr = NULL;
    int rc = gs_new_page(gs, xid, &newpid, &fr);
    if (rc != OK)
        return rc;

    u16 nslots = gs_nslots(fr->data);
    if (nslots == 0) {
        pager_unpin(gs->pg, newpid, true);
        return E_NOSPACE;
    }

    u8 before[PAGE_SIZE];
    memcpy(before, fr->data, PAGE_SIZE);

    /*
     * 槽 0 分配给调用方；槽 1..nslots-1 全部串进空闲链。
     *
     * 【为什么必须建链】
     * 新页只有槽 0 可用的话，下一次分配又会开新页 ——
     * 每页只放 1 个记录，页内空间全浪费。
     *
     * 链的形状：free_head = 1，槽 i 的 next_free = i+1，
     * 最后一个槽的 next_free = GS_NO_SLOT。
     */
    u8 *rec0 = fr->data + gs_slot_off(fr->data, 0);
    memset(rec0, 0, gs->rec_size);
    gs_set_in_use(rec0, true);
    gs_set_next_free(rec0, 0);
    if (init)
        memcpy(rec0 + GS_REC_HDR_SIZE, init + GS_REC_HDR_SIZE,
               gs->rec_size - GS_REC_HDR_SIZE);

    if (nslots > 1) {
        for (u16 i = 1; i < nslots; i++) {
            u8 *rec = fr->data + gs_slot_off(fr->data, i);
            memset(rec, 0, gs->rec_size);
            gs_set_in_use(rec, false);
            gs_set_next_free(rec, (i + 1 < nslots) ? (u16)(i + 1) : GS_NO_SLOT);
        }
        gs_set_free_head(fr->data, 1);
    } else {
        gs_set_free_head(fr->data, GS_NO_SLOT);
    }

    u32 lsn = 0;
    rc = gs_log_page(gs, xid, newpid, before, fr->data, &lsn);
    if (rc != OK) {
        memcpy(fr->data, before, PAGE_SIZE);
        pager_unpin(gs->pg, newpid, true);
        return rc;
    }
    put_u32(fr->data + GS_H_LSN, lsn);
    page_set_checksum(fr->data);

    Rid r = rid_make(newpid, 0);
    pager_unpin(gs->pg, newpid, true);
    gs->alive++;
    *out = r;
    return OK;
}

int gstore_alloc(GStore *gs, u32 xid, Rid *out)
{
    return gs_alloc(gs, xid, NULL, 0, out);
}

int gstore_alloc_with(GStore *gs, u32 xid, const u8 *init, u16 len, Rid *out)
{
    if (!init)
        return E_INVAL;
    return gs_alloc(gs, xid, init, len, out);
}

/* ---------------- 读槽 ---------------- */

int gstore_get(GStore *gs, Rid rid, u8 *buf, u16 cap, u16 *len)
{
    if (!gs || !buf)
        return E_INVAL;
    if (cap < gs->rec_size)
        return E_NOSPACE;

    PageFrame *fr = NULL;
    int rc = pager_get(gs->pg, rid.page, &fr);
    if (rc != OK)
        return rc;

    /*
     * 页类型必须对得上。
     *
     * 节点 gstore 和边 gstore 共享同一个 pager，但页类型不同
     * （PT_GRAPH_NODE / PT_GRAPH_EDGE）。
     * 如果拿着边页的 RID 去节点 gstore 里取，不校验页类型就会
     * 按**节点的记录大小**去解释边页的字节 —— 读出一堆垃圾，
     * 而且不报错（最坏情况）。
     *
     * 判据是"这一页是不是我这个 gstore 的页"，不是"看起来像不像"。
     */
    if (fr->data[GS_H_TYPE] != gs->page_type) {
        pager_unpin(gs->pg, rid.page, false);
        return E_NOTFOUND;
    }

    u16 nslots = gs_nslots(fr->data);
    if (rid.slot >= nslots) {
        pager_unpin(gs->pg, rid.page, false);
        return E_NOTFOUND;
    }

    const u8 *rec = fr->data + gs_slot_off(fr->data, rid.slot);
    if (!gs_in_use(rec)) {
        pager_unpin(gs->pg, rid.page, false);
        return E_NOTFOUND;
    }

    memcpy(buf, rec, gs->rec_size);
    if (len)
        *len = gs->rec_size;
    gs->rec_reads++;
    pager_unpin(gs->pg, rid.page, false);
    return OK;
}

/* ---------------- 写槽 ---------------- */

/* mutator 回调：把调用方给的 buf 写进页 */
typedef struct {
    u16  slot;
    u16  rec_size;
    const u8 *src;
} GsPutCtx;

static int gs_put_mutator(void *ctx, u8 *pg)
{
    GsPutCtx *c = (GsPutCtx *)ctx;
    u32 off = gs_slot_off(pg, c->slot);
    u8 *rec = pg + off;

    if (!gs_in_use(rec))
        return E_NOTFOUND;

    /* 保留公共字段（in_use / next_free），只覆盖模态字段区 */
    memcpy(rec + GS_REC_HDR_SIZE, c->src + GS_REC_HDR_SIZE,
           c->rec_size - GS_REC_HDR_SIZE);
    return OK;
}

int gstore_put(GStore *gs, u32 xid, Rid rid, const u8 *buf, u16 len)
{
    if (!gs || !buf)
        return E_INVAL;
    if (len != gs->rec_size)
        return E_INVAL;      /* 定长槽页不接受变长写入 */

    GsPutCtx c;
    c.slot = rid.slot;
    c.rec_size = gs->rec_size;
    c.src = buf;

    return gs_write_page(gs, xid, rid.page, gs_put_mutator, &c);
}

/* ---------------- 释放槽 ---------------- */

typedef struct {
    u16 slot;
} GsFreeCtx;

static int gs_free_mutator(void *ctx, u8 *pg)
{
    GsFreeCtx *c = (GsFreeCtx *)ctx;
    u16 nslots = gs_nslots(pg);
    if (c->slot >= nslots)
        return E_NOTFOUND;

    u32 off = gs_slot_off(pg, c->slot);
    u8 *rec = pg + off;
    if (!gs_in_use(rec))
        return E_NOTFOUND;

    /* 挂回空闲链头 */
    gs_set_in_use(rec, false);
    gs_set_next_free(rec, gs_free_head(pg));
    gs_set_free_head(pg, c->slot);
    return OK;
}

int gstore_free(GStore *gs, u32 xid, Rid rid)
{
    if (!gs)
        return E_INVAL;

    GsFreeCtx c;
    c.slot = rid.slot;

    int rc = gs_write_page(gs, xid, rid.page, gs_free_mutator, &c);
    if (rc == OK && gs->alive > 0)
        gs->alive--;
    return rc;
}

/* ---------------- 查询 ---------------- */

bool gstore_slot_alive(GStore *gs, Rid rid)
{
    if (!gs)
        return false;

    PageFrame *fr = NULL;
    if (pager_get(gs->pg, rid.page, &fr) != OK)
        return false;

    u16 nslots = gs_nslots(fr->data);
    if (rid.slot >= nslots) {
        pager_unpin(gs->pg, rid.page, false);
        return false;
    }

    const u8 *rec = fr->data + gs_slot_off(fr->data, rid.slot);
    bool alive = gs_in_use(rec);
    pager_unpin(gs->pg, rid.page, false);
    return alive;
}

/* ---------------- 遍历 ---------------- */

int gstore_foreach(GStore *gs, GStoreEachFn fn, void *ctx)
{
    if (!gs || !fn)
        return E_INVAL;

    u32 pid = gs->head_page;
    while (pid != PAGE_ID_NIL) {
        PageFrame *fr = NULL;
        int rc = pager_get(gs->pg, pid, &fr);
        if (rc != OK)
            return rc;

        u16 nslots = gs_nslots(fr->data);
        for (u16 i = 0; i < nslots; i++) {
            const u8 *rec = fr->data + gs_slot_off(fr->data, i);
            if (!gs_in_use(rec))
                continue;
            rc = fn(ctx, rid_make(pid, i), rec);
            if (rc != OK) {
                pager_unpin(gs->pg, pid, false);
                return rc;
            }
        }

        u32 next = gs_next(fr->data);
        pager_unpin(gs->pg, pid, false);
        pid = next;
    }
    return OK;
}

/* ---------------- 重启接管 ---------------- */

/*
 * 扫全文件找链头。
 *
 * 【为什么必须扫全文件】
 *
 * 页链是单向的（head → next → ...），每一页只记着「我的下一个是谁」，
 * 没有任何一页记着「我的上一个是谁」。所以光看链找不到头在哪。
 *
 * 解法：链头页带 GS_FLAG_DIR 标志。扫一遍全文件，
 * 找「类型对 + 带标志」的页 → 链头。
 *
 * 这与 blob 同源：blob_dir_scan 扫全文件找带 BLOB_FLAG_HEAD 的页
 * （storage/blob.c）。blob 的目录页页号也只在内存里，重启同样靠扫描重建。
 *
 * 【扫到的页可能不是本 gstore 的】
 *
 * 一个数据库文件里混着 heap 页 / btree 页 / blob 页 / 图的节点页和边页。
 * 所以判据是「页类型 == 本 gstore 的 page_type」，
 * 而不是「页看起来像不像图页」。
 */
static int gs_find_head(GStore *gs, u32 *head_out)
{
    *head_out = PAGE_ID_NIL;

    u64 np = pager_npages(gs->pg);
    for (u64 i = 0; i < np; i++) {
        PageFrame *fr = NULL;
        int rc = pager_get(gs->pg, (u32)i, &fr);
        if (rc != OK)
            return rc;

        if (fr->data[GS_H_TYPE] == gs->page_type &&
            (fr->data[GS_H_FLAGS] & GS_FLAG_DIR)) {
            pager_unpin(gs->pg, (u32)i, false);
            *head_out = (u32)i;
            return OK;
        }
        pager_unpin(gs->pg, (u32)i, false);
    }
    return OK;
}

int gstore_recover(GStore *gs)
{
    if (!gs)
        return E_INVAL;

    u32 head = PAGE_ID_NIL;
    int rc = gs_find_head(gs, &head);
    if (rc != OK)
        return rc;

    /* 找不到链头 = 这是个空库（不是错误） */
    if (head == PAGE_ID_NIL) {
        gs->head_page = PAGE_ID_NIL;
        gs->tail_page = PAGE_ID_NIL;
        gs->npages = 0;
        gs->alive = 0;
        return OK;
    }

    /* 顺着 next 走一遍链，重建 head/tail/npages/alive */
    u32 pid = head;
    u32 tail = PAGE_ID_NIL;
    u32 npages = 0;
    u64 alive = 0;

    while (pid != PAGE_ID_NIL) {
        PageFrame *fr = NULL;
        rc = pager_get(gs->pg, pid, &fr);
        if (rc != OK)
            return rc;

        /* 链上的页必须都是本 gstore 的页 */
        if (fr->data[GS_H_TYPE] != gs->page_type) {
            pager_unpin(gs->pg, pid, false);
            return E_CORRUPT;
        }
        /* 记录大小必须一致 —— 不一致说明两种 gstore 的页混进了同一条链 */
        if (gs_rec_size(fr->data) != gs->rec_size) {
            pager_unpin(gs->pg, pid, false);
            return E_CORRUPT;
        }

        u16 nslots = gs_nslots(fr->data);
        for (u16 i = 0; i < nslots; i++) {
            const u8 *rec = fr->data + gs_slot_off(fr->data, i);
            if (gs_in_use(rec))
                alive++;
        }

        u32 next = gs_next(fr->data);
        pager_unpin(gs->pg, pid, false);

        tail = pid;
        npages++;
        pid = next;

        /* 环检测：链最长不超过文件页数，绕回来了就是断链 */
        if (npages > pager_npages(gs->pg))
            return E_CORRUPT;
    }

    gs->head_page = head;
    gs->tail_page = tail;
    gs->npages = npages;
    gs->alive = alive;
    return OK;
}

/* ---------------- 读整页 ---------------- */

int gstore_read_page(GStore *gs, u32 page_id, u8 *buf, u16 cap)
{
    if (!gs || !buf)
        return E_INVAL;
    if (cap < PAGE_SIZE)
        return E_NOSPACE;

    PageFrame *fr = NULL;
    int rc = pager_get(gs->pg, page_id, &fr);
    if (rc != OK)
        return rc;

    memcpy(buf, fr->data, PAGE_SIZE);
    pager_unpin(gs->pg, page_id, false);
    return OK;
}
