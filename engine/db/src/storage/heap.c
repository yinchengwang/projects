/*
 * heap.c — 堆表 AM：生命周期 + 写路径 + 观察
 *
 * 四条纪律：
 *   1. 每次修改页之前先调 WAL 钩子，拿到 LSN 写进页头。
 *   2. 任何时刻最多 pin 一页。扫描器每行 get/unpin 一次。
 *   3. 返回值一律拷贝到调用方缓冲区，不返回页内指针。
 *   4. 不依赖页链：扫描走「本表页号表」（见下），崩溃恢复不会丢页。
 *
 * 本文件负责：heap_create / heap_open / heap_destroy / heap_set_wal、
 * heap_insert / heap_write_ver / heap_update / heap_delete / heap_get、
 * heap_npages / heap_wal_calls。
 *
 * 扫描在 heap_scan.c，页内 compact 在 heap_compact.c。
 * 共享的宏、类型与内部辅助在 storage/heap_internal.h。
 */

#include "storage/heap_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/record.h"

/* ---------------- WAL 钩子 ---------------- */

/*
 * 写一条日志。
 *
 * slot_off 是这个 slot 在页内的字节偏移 —— undo 时要靠它确认
 * "记录还在原来的位置"，否则回写会写坏别的 slot。
 *
 * xid 由调用方显式传进来（见 loghook.h 里关于"共享状态"的说明）。
 */
int heap_wal_log(Heap *h, u32 xid, u8 logtype, u32 page_id, u16 slot,
                 u16 slot_off,
                 const u8 *before, u16 blen,
                 const u8 *after, u16 alen, u32 *lsn_out)
{
    *lsn_out = 0;
    if (!h->wal)
        return OK;
    atomic_fetch_add(&h->wal_calls, 1);
    return h->wal(h->wal_ctx, xid, logtype, page_id, slot, slot_off,
                  before, blen, after, alen, lsn_out);
}

void heap_set_wal(Heap *h, HeapWalFn fn, void *ctx)
{
    if (!h)
        return;
    h->wal = fn;
    h->wal_ctx = ctx;
}

/*
 * 【为什么堆表层没有 xid 字段】
 *
 * v0.7 最初的设计是"堆表知道自己属于哪个事务，写入时自动打 xmin"。
 * 但这个设计在「共享实例 + 多连接」下会崩：
 *
 *   catalog 是**共享**的（所有连接一份），所以 heap_set_xid(cat, xid)
 *   设置的是全局状态。连接 A 开事务设 xid=5，连接 B 开事务设 xid=6，
 *   A 的写就被打上了 B 的 xid —— 可见性判断立刻失效。
 *
 * 正确的做法：xid 是**连接私有**的，必须随每次写入显式传下去，
 * 不能存在共享对象里。所以堆表的写接口都带 xid 参数。
 *
 * 而版本号最终由 exec 层负责打（stamp_version / stamp_deleted），
 * 堆表根本不需要知道 xid —— 它只是把 xid 转交给日志钩子而已。
 */

/* ---------------- 本表页号表 ---------------- */

/*
 * 追加一个页号到本表的页号表。
 *
 * 失败（内存不足）返回 E_IO —— 宁可让调用方报错，
 * 也不能"页存在但扫不到"（那等于静默丢数据）。
 */
int heap_plist_add(Heap *h, u32 pid)
{
    int rc = OK;
    pthread_mutex_lock(&h->plist_mu);
    if (h->plist_n == h->plist_cap) {
        u32 cap = h->plist_cap ? h->plist_cap * 2 : 16;
        u32 *np = realloc(h->plist, (size_t)cap * sizeof(u32));
        if (!np) {
            rc = E_IO;
        } else {
            h->plist = np;
            h->plist_cap = cap;
        }
    }
    if (rc == OK)
        h->plist[h->plist_n++] = pid;
    pthread_mutex_unlock(&h->plist_mu);
    return rc;
}

/*
 * 拍一份页号表快照。
 *
 * 拷贝一份而不是直接读共享数组：扫描期间别的线程会追加新页
 * （append_page），共享数组还可能 realloc 换指针 —— 直接读会读到半个数组。
 */
int heap_plist_snapshot(Heap *h, u32 **out, u32 *n_out)
{
    u32 *buf = NULL;
    u32 n = 0;

    pthread_mutex_lock(&h->plist_mu);
    n = h->plist_n;
    if (n) {
        buf = malloc((size_t)n * sizeof(u32));
        if (!buf) {
            pthread_mutex_unlock(&h->plist_mu);
            return E_IO;
        }
        memcpy(buf, h->plist, (size_t)n * sizeof(u32));
    }
    pthread_mutex_unlock(&h->plist_mu);

    *out = buf;
    *n_out = n;
    return OK;
}

/* ---------------- 生命周期 ---------------- */

int heap_create(Pager *pg, FreeList *fl, u16 table_id,
                HeapWalFn fn, void *ctx, u32 *root_page_out)
{
    if (!pg || !fl || !root_page_out)
        return E_INVAL;

    PageFrame *f;
    u32 pid;
    int rc = pager_alloc(pg, &pid, &f);
    if (rc != OK)
        return rc;

    rc = page_init(f->data, PT_HEAP);
    if (rc != OK) {
        pager_unpin(pg, pid, false);
        return rc;
    }
    page_set_table_id(f->data, table_id);

    /* 新页初始化也要写日志：否则崩溃后这一页既没有 H_TYPE 也没有
     * table_id，扫描会当它不存在，整页数据就"消失"了 */
    u32 lsn = 0;
    if (fn) {
        rc = fn(ctx, 0, LOG_PAGEINIT, pid, 0, LOG_NO_SLOT_OFF,
                NULL, 0, f->data, HDR_SIZE, &lsn);
        if (rc != OK) {
            pager_unpin(pg, pid, false);
            return rc;
        }
        if (lsn)
            page_set_lsn(f->data, lsn);
    }

    rc = freelist_grow(fl, pid + 1);
    if (rc != OK) {
        pager_unpin(pg, pid, false);
        return rc;
    }
    freelist_set(fl, pid, FL_USABLE);

    rc = pager_unpin(pg, pid, true);
    if (rc != OK)
        return rc;

    *root_page_out = pid;
    return OK;
}

int heap_open(Pager *pg, FreeList *fl, u32 root_page, u16 table_id,
              HeapWalFn fn, void *ctx, Heap **out)
{
    if (!pg || !fl || !out || root_page == PAGE_ID_NIL)
        return E_INVAL;

    Heap *h = calloc(1, sizeof(*h));
    if (!h)
        return E_IO;
    if (pthread_mutex_init(&h->plist_mu, NULL) != 0) {
        free(h);
        return E_IO;
    }
    h->pager = pg;
    h->fl = fl;
    h->root = root_page;
    h->tid = table_id;
    h->wal = fn;
    h->wal_ctx = ctx;

    int rc = freelist_grow(fl, (u32)pager_npages(pg));
    if (rc != OK) {
        heap_destroy(h);
        return rc;
    }

    /*
     * 扫一遍：登记本表的页，顺带统计页数，并把页号收进页号表。
     *
     * 页号表在打开时重建 —— 它不进 WAL，崩溃恢复后靠这一遍重建 ✓
     */
    u32 np = (u32)pager_npages(pg);
    for (u32 p = 0; p < np; p++) {
        PageFrame *f;
        if (pager_get(pg, p, &f) != OK)
            continue;
        bool mine = (f->data[H_TYPE] == PT_HEAP && page_table_id(f->data) == table_id);
        if (mine) {
            freelist_set(fl, p, page_free_bytes(f->data));
            atomic_fetch_add(&h->npages, 1);
            if (heap_plist_add(h, p) != OK) {
                pager_unpin(pg, p, false);
                heap_destroy(h);
                return E_IO;
            }
        }
        pager_unpin(pg, p, false);
    }

    *out = h;
    return OK;
}

void heap_destroy(Heap *h)
{
    if (!h)
        return;
    pthread_mutex_destroy(&h->plist_mu);
    free(h->plist);
    free(h);
}

/* ---------------- 内部：新开一页 ---------------- */

static int append_page(Heap *h, u32 *pid_out, PageFrame **frame_out)
{
    PageFrame *nf;
    u32 pid;
    int rc = pager_alloc(h->pager, &pid, &nf);
    if (rc != OK)
        return rc;

    rc = page_init(nf->data, PT_HEAP);
    if (rc != OK) {
        pager_unpin(h->pager, pid, false);
        return rc;
    }
    page_set_table_id(nf->data, h->tid);

    u32 lsn = 0;
    rc = heap_wal_log(h, 0, LOG_PAGEINIT, pid, 0, LOG_NO_SLOT_OFF,
                      NULL, 0, nf->data, HDR_SIZE, &lsn);
    if (rc != OK) {
        pager_unpin(h->pager, pid, false);
        return rc;
    }
    if (lsn)
        page_set_lsn(nf->data, lsn);

    rc = freelist_grow(h->fl, pid + 1);
    if (rc != OK) {
        pager_unpin(h->pager, pid, false);
        return rc;
    }
    /*
     * 新页必须先进页号表，再进 freelist。
     *
     * 顺序不能反：freelist 先登记的话，别的线程可能挑中这一页插入数据，
     * 而它还没进页号表 → 数据"插进去了但扫不到"。
     *
     * 失败（内存不足）就让整个 append 失败：
     * 报了错，调用方不会拿着一个"看不见的页"继续跑。
     */
    rc = heap_plist_add(h, pid);
    if (rc != OK) {
        pager_unpin(h->pager, pid, false);
        return rc;
    }

    freelist_set(h->fl, pid, FL_USABLE);
    atomic_fetch_add(&h->npages, 1);

    *pid_out = pid;
    *frame_out = nf;
    return OK;
}

/* ---------------- 写路径 ---------------- */

/*
 * 一次插入最多重试多少次。
 *
 * 每次重试都意味着"我们挑的那页被别人抢先填满了"。
 * 同一时刻能抢的线程数 = 写同一张表的并发连接数，
 * 所以重试次数只要**大于并发写者数**就够收敛。
 * 取 64 是为了给"多表多线程"的极端情况留余量。
 */
#define HEAP_INSERT_MAX_RETRY 64

/*
 * ==================== 为什么插入要重试 ====================
 *
 * freelist 是按**档位**挑页的，而档位是**插入之后**才更新的。
 * 于是"找 → 用"之间有一个窗口：
 *
 *     线程 A：find → 页 P（档位 3，放得下）
 *             → 拿 P 的内容锁 → 插进去 → 更新 P 的档位
 *     线程 B：find → 页 P（档位**还是** 3 —— A 还没更新）
 *             → 等 P 的内容锁 → 拿到时 P 已被 A 填满
 *             → page_insert 报 E_NOSPACE
 *
 * 这不是"页不够用"，是**档位过期**。
 * 真实数据库也有这个窗口（PG 的 FSM 本来就是近似的），
 * 标准的处理办法就是**重试**：换一页再试。
 *
 * ==================== 为什么重试一定收敛 ====================
 *
 * 填满 P 的那个线程是在**持有 P 的内容锁**的时候更新档位的，
 * 而我们是**拿到内容锁之后**才失败的 ——
 * 所以失败时 P 的档位已经是新的（free < need ⟹ 档位 < need_level）。
 *
 * 而 freelist_find 只挑"档位 >= need_level"的页 ⟹ 不会再挑到 P ✓
 *
 * 换句话说：**每次失败都会把候选页从候选集里划掉**，
 * 所以重试次数有上限，不会死循环。
 */
int heap_insert(Heap *h, u32 xid, const u8 *rec, u16 len, Rid *rid_out)
{
    if (!h || !rec || !rid_out || len == 0)
        return E_INVAL;

    u32 need = (u32)len + SLOT_SIZE;
    if (need > PAGE_SIZE - HDR_SIZE)
        return E_NOSPACE;

    for (int attempt = 0; attempt < HEAP_INSERT_MAX_RETRY; attempt++) {
        u32 pid = freelist_find(h->fl, (u16)need);
        PageFrame *f = NULL;
        int rc;

        if (pid == PAGE_ID_NIL) {
            /* 没有放得下的页了 → 开新页（新页一定是空的） */
            rc = append_page(h, &pid, &f);
            if (rc != OK)
                return rc;
        } else {
            rc = pager_get(h->pager, pid, &f);
            if (rc != OK)
                return rc;
        }

        u16 slot;
        rc = page_insert(f->data, rec, len, &slot);
        if (rc != OK) {
            pager_unpin(h->pager, pid, false);
            if (rc != E_NOSPACE)
                return rc;
            /* 档位过期（别人抢先把这页填满了）→ 换一页重试 */
            continue;
        }

        /*
         * 顺序很重要：先写日志，再把 LSN 写进页头。
         * 反过来的话，页上会带着一个"日志里还没有"的 LSN，
         * 崩溃后重放会因为 page_lsn >= lsn 而跳过这条记录 —— 内容就丢了。
         */
        u16 slot_off = page_slot_offset(f->data, slot);
        u32 lsn = 0;
        rc = heap_wal_log(h, xid, LOG_INSERT, pid, slot, slot_off, NULL, 0, rec, len, &lsn);
        if (rc != OK) {
            pager_unpin(h->pager, pid, false);
            return rc;
        }
        if (lsn)
            page_set_lsn(f->data, lsn);

        freelist_set(h->fl, pid, page_free_bytes(f->data));
        *rid_out = rid_make(pid, slot);
        return pager_unpin(h->pager, pid, true);
    }

    /*
     * 重试到上限还没插进去 —— 说明页确实全满了（记录比页还大，
     * 但那在前面就被 need > PAGE_SIZE - HDR_SIZE 拦住了）。
     * 走到这里只能是"页被反复抢走"的极端情况，报错不静默丢数据。
     */
    return E_NOSPACE;
}

/*
 * 写入一条记录到指定 slot（带 WAL 日志）。
 *
 * MVCC 用它给旧版本打 xmax 标记：内容变了，slot 和 RID 不变。
 *
 * 为什么单独提供这个原语，而不是让调用方自己
 * "pager_get → 改字节 → page_update → page_set_lsn"：
 * 那条路径漏一步 WAL 规则就会造成静默数据丢失
 * （页带着未落盘的日志上了盘，崩溃后内容回不来）。
 * 把"写日志 → 改页 → 写 page_lsn"这套流程封在堆表里，才安全。
 */
int heap_write_ver(Heap *h, u32 xid, Rid rid, const u8 *rec, u16 len)
{
    if (!h || !rec || rid_is_nil(rid) || len == 0)
        return E_INVAL;

    PageFrame *f;
    int rc = pager_get(h->pager, rid.page, &f);
    if (rc != OK)
        return rc;

    const u8 *old;
    u16 old_len;
    rc = page_get(f->data, rid.slot, &old, &old_len);
    if (rc != OK) {
        pager_unpin(h->pager, rid.page, false);
        return rc;
    }

    u16 slot_off = page_slot_offset(f->data, rid.slot);
    u32 lsn = 0;
    rc = heap_wal_log(h, xid, LOG_UPDATE, rid.page, rid.slot, slot_off,
                      old, old_len, rec, len, &lsn);
    if (rc != OK) {
        pager_unpin(h->pager, rid.page, false);
        return rc;
    }
    if (lsn)
        page_set_lsn(f->data, lsn);

    rc = page_update(f->data, rid.slot, rec, len);
    if (rc != OK) {
        pager_unpin(h->pager, rid.page, false);
        return rc;
    }

    freelist_set(h->fl, rid.page, page_free_bytes(f->data));
    return pager_unpin(h->pager, rid.page, true);
}

/*
 * 删除一行 —— v0.6 语义：真的抹掉 slot 的内容。
 *
 * v0.7 的 MVCC 不再用这个函数做业务删除（业务删除是"打 xmax 标记"，
 * 见 heap_write_ver）。这个函数保留给两个用途：
 *   1. vacuum 回收死版本
 *   2. 存储层自己的测试
 *
 * 保留"真删"和"打标记"两种语义是必要的：
 * 它们对应不同的语义 —— "这行从不存在" vs "这行在某个时刻被删了"。
 */
int heap_delete(Heap *h, u32 xid, Rid rid)
{
    if (!h || rid_is_nil(rid))
        return E_INVAL;

    PageFrame *f;
    int rc = pager_get(h->pager, rid.page, &f);
    if (rc != OK)
        return rc;

    const u8 *old;
    u16 old_len;
    rc = page_get(f->data, rid.slot, &old, &old_len);
    if (rc != OK) {
        pager_unpin(h->pager, rid.page, false);
        return rc;
    }

    u16 slot_off = page_slot_offset(f->data, rid.slot);
    u32 lsn = 0;
    rc = heap_wal_log(h, xid, LOG_DELETE, rid.page, rid.slot, slot_off, old, old_len, NULL, 0, &lsn);
    if (rc != OK) {
        pager_unpin(h->pager, rid.page, false);
        return rc;
    }
    if (lsn)
        page_set_lsn(f->data, lsn);

    rc = page_delete(f->data, rid.slot);
    if (rc != OK) {
        pager_unpin(h->pager, rid.page, false);
        return rc;
    }

    freelist_set(h->fl, rid.page, page_free_bytes(f->data));
    return pager_unpin(h->pager, rid.page, true);
}

/*
 * 更新一行 —— v0.6 语义：长度不变就原地改，变长就删旧插新。
 *
 * 【注意：这个函数不做 MVCC】
 *
 * v0.7 的 MVCC 更新是「给旧版本打 xmax 标记 + 插新版本」，
 * 但那需要知道 tuple 格式里 xmax 在哪 —— 而堆表层不该知道这个。
 *
 * 所以 MVCC 的更新由上层（exec）用两个原语组合：
 *   heap_write_ver(rid, 打了 xmax 的旧行)   ← 标记旧版本
 *   heap_insert(打了 xmin 的新行, &new_rid) ← 插新版本
 *
 * 这个函数保留给不需要多版本的场景（存储层测试、系统表维护）。
 */
int heap_update(Heap *h, u32 xid, Rid rid, const u8 *rec, u16 len, Rid *new_rid)
{
    if (!h || !rec || !new_rid || rid_is_nil(rid) || len == 0)
        return E_INVAL;

    PageFrame *f;
    int rc = pager_get(h->pager, rid.page, &f);
    if (rc != OK)
        return rc;

    const u8 *old;
    u16 old_len;
    rc = page_get(f->data, rid.slot, &old, &old_len);
    if (rc != OK) {
        pager_unpin(h->pager, rid.page, false);
        return rc;
    }

    if (len <= old_len) {
        u16 slot_off = page_slot_offset(f->data, rid.slot);
        u32 lsn = 0;
        rc = heap_wal_log(h, xid, LOG_UPDATE, rid.page, rid.slot, slot_off,
                          old, old_len, rec, len, &lsn);
        if (rc != OK) {
            pager_unpin(h->pager, rid.page, false);
            return rc;
        }
        if (lsn)
            page_set_lsn(f->data, lsn);

        rc = page_update(f->data, rid.slot, rec, len);
        if (rc != OK) {
            pager_unpin(h->pager, rid.page, false);
            return rc;
        }
        freelist_set(h->fl, rid.page, page_free_bytes(f->data));
        *new_rid = rid;
        return pager_unpin(h->pager, rid.page, true);
    }

    /* 变长：删旧插新，RID 会变 */
    u16 slot_off = page_slot_offset(f->data, rid.slot);
    u32 dlsn = 0;
    rc = heap_wal_log(h, xid, LOG_DELETE, rid.page, rid.slot, slot_off,
                      old, old_len, NULL, 0, &dlsn);
    if (rc != OK) {
        pager_unpin(h->pager, rid.page, false);
        return rc;
    }
    if (dlsn)
        page_set_lsn(f->data, dlsn);

    rc = page_delete(f->data, rid.slot);
    if (rc != OK) {
        pager_unpin(h->pager, rid.page, false);
        return rc;
    }
    freelist_set(h->fl, rid.page, page_free_bytes(f->data));
    rc = pager_unpin(h->pager, rid.page, true);
    if (rc != OK)
        return rc;

    return heap_insert(h, xid, rec, len, new_rid);
}

/* ---------------- 读路径 ---------------- */

int heap_get(Heap *h, Rid rid, u8 *buf, u16 bufsize, u16 *len_out)
{
    if (!h || !buf || !len_out || rid_is_nil(rid))
        return E_INVAL;

    PageFrame *f;
    int rc = pager_get(h->pager, rid.page, &f);
    if (rc != OK)
        return rc;

    const u8 *rec;
    u16 len;
    rc = page_get(f->data, rid.slot, &rec, &len);
    if (rc == OK) {
        if (len > bufsize) {
            rc = E_NOSPACE;
        } else {
            memcpy(buf, rec, len);
            *len_out = len;
        }
    }
    pager_unpin(h->pager, rid.page, false);
    return rc;
}

/* ---------------- 观察 ---------------- */

u32 heap_npages(const Heap *h)
{
    return h ? (u32)atomic_load(&h->npages) : 0;
}

u64 heap_wal_calls(const Heap *h)
{
    return h ? (u64)atomic_load(&h->wal_calls) : 0;
}
