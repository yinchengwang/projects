/*
 * ft_tomb.c —— tombstone（删除标记）集合的**落盘**与恢复（v1.3 步骤 5）
 *
 * 【这一层管什么】
 *
 * "这篇文档被删了"这件事必须存在**两个地方**：
 *
 *   1. 内存里的 ix->tomb（升序的 doc_id 集合）—— 检索时用来过滤；
 *   2. 磁盘上的 tombstone 页（PT_FT_TOMB）—— **权威存储**。
 *
 * 为什么第 2 份是必需的（坑 B）：
 *
 * 只靠 WAL 记录是不够的。WAL 会被 checkpoint **截断** ——
 * 截断之后老的删除记录就没了，而那些文档还在段里，
 * 于是它们"复活"了：删掉的东西又出现在检索结果里。
 *
 * 所以 tombstone 集合必须自己落盘（WAL 只负责"崩溃时怎么把它找回来"）。
 *
 * 【落盘形态】
 *
 * 一页就是一个稠密的 u32 数组（格式见 ft_internal.h 的 FTM_*）：
 *
 *   20 FTM_COUNT  u32   本页的 doc_id 条数
 *   24 FTM_DATA   ...   doc_id 数组
 *
 * 写满就**再开一页**（每个 tombstone 页都是独立的，
 * 全体的并集 = 集合）。这样就不需要链头，也就不怕"链断在半路"。
 *
 * 【每次删除都要写整页吗】
 *
 * 是的 —— 每次删除把当前页**整页重写**（读-改-写），WAL 里记整页镜像
 * （LOG_PAGEDATA，和向量模态的做法一致）。
 *
 * 代价：一次删除 = 4KB 的写放大。
 * 好处：崩溃恢复只要"把页盖回去"就行，不用理解页内结构，
 * 而且未提交事务的删除可以被**撤销**（前像就是这一页改之前的内容）。
 *
 * 将来要优化的话，方向是把这一页换成"日志式追加"（页内放一条条
 * (doc_id, lsn) 记录，满了就开新页），本版不做 —— 整页重写够用且好懂。
 */

#include "modalities/ft/ft_index_internal.h"

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "modalities/ft/ft_internal.h"
#include "modalities/ft/ft_writer.h"
#include "storage/page.h"

/* ---------------- 小工具 ---------------- */

/* u32 升序比较（qsort 用） */
static int ft_tomb_u32_cmp(const void *pa, const void *pb)
{
    u32 a = *(const u32 *)pa;
    u32 b = *(const u32 *)pb;

    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

/* ---------------- 内存里的集合（升序数组） ---------------- */

/*
 * 二分找 doc_id 在升序集合里的位置（返回第一个 >= doc_id 的下标，也就是插入点）。
 *
 * ft_merge.h 要求传过去的 tomb 集合**必须升序**（那边用二分判断成员）。
 */
u32 ft_tomb_lb(const u32 *tomb, u32 n, u32 doc_id)
{
    u32 lo = 0;
    u32 hi = n;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;

        if (tomb[mid] < doc_id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* doc_id 是否已被删除 */
bool ft_tomb_is(const FtIndex *ix, u32 doc_id)
{
    u32 pos = ft_tomb_lb(ix->tomb, ix->n_tombs, doc_id);

    return (pos < ix->n_tombs && ix->tomb[pos] == doc_id);
}

/*
 * 升序插入一个 doc_id。
 *
 * 返回 1 = 插进去了，0 = 已经在集合里（空操作），-1 = 内存不足。
 *
 * 【为什么是"有序插入"而不是"追加后整体排序"】
 *
 *   - 有序插入：插入 O(n)（memmove），但**查找**是 O(log n) 二分；
 *     追加后排序：插入 O(n log n)，查找也只能 O(n) 线性扫。
 *   - 删除是**低频**操作，检索是**高频**操作 —— 该优化的是检索。
 *   - ft_merge.h 要求传过去的 tomb 集合必须升序（那边用二分判断成员），
 *     有序插入天然满足。
 */
int ft_tomb_insert(FtIndex *ix, u32 doc_id)
{
    u32 pos = ft_tomb_lb(ix->tomb, ix->n_tombs, doc_id);

    if (pos < ix->n_tombs && ix->tomb[pos] == doc_id)
        return 0;               /* 已经删过了：再删一次是合法的空操作 */

    if (ix->n_tombs == ix->cap_tombs) {
        u32 ncap = (ix->cap_tombs == 0) ? 16u : ix->cap_tombs * 2u;
        u32 *nt = (u32 *)realloc(ix->tomb, sizeof(u32) * (size_t)ncap);

        if (nt == NULL)
            return -1;
        ix->tomb = nt;
        ix->cap_tombs = ncap;
    }
    if (pos < ix->n_tombs) {
        memmove(&ix->tomb[pos + 1], &ix->tomb[pos],
                sizeof(u32) * (size_t)(ix->n_tombs - pos));
    }
    ix->tomb[pos] = doc_id;
    ix->n_tombs++;
    return 1;
}

/* ---------------- 页的登记与追加 ---------------- */

/*
 * 登记一张 tombstone 页（扫描页空间时调）。
 *
 * count —— 这一页里有多少条（调用方已经读过了）。
 *
 * 【"当前页"是谁】
 *
 * 约定：**最后登记的那张页**就是当前页（tomb_fill = 它的条数）。
 * 追加时先往它后面写，写满了就再开一页（新页自然成了最后一张）。
 *
 * 扫描是按页号递增做的，所以最后一张 = 页号最大的那张。
 */
int ft_tomb_add_page(FtIndex *ix, u32 page_id, u32 count)
{
    if (ix == NULL)
        return E_INVAL;

    if (ix->n_tomb_pages == ix->cap_tomb_pages) {
        u32 ncap = (ix->cap_tomb_pages == 0) ? 4u : ix->cap_tomb_pages * 2u;
        u32 *np = (u32 *)realloc(ix->tomb_pages, sizeof(u32) * (size_t)ncap);

        if (np == NULL)
            return E_NOSPACE;
        ix->tomb_pages = np;
        ix->cap_tomb_pages = ncap;
    }
    ix->tomb_pages[ix->n_tomb_pages++] = page_id;
    ix->tomb_fill = count;
    return OK;
}

/*
 * 追加一个 doc_id 到落盘的集合。
 *
 * 【为什么是"读-改-写整页"】
 *
 * 页里没有"插入"这个操作 —— 它就是一个数组，
 * 追加 = 把 id 写到第 count 个位置，count 加一。
 *
 * 【写日志的时机】
 *
 * 改完之后调 ft_page_write：它把"改之前的内容"当前像写进 WAL。
 * 未提交事务的删除因此可以被撤销（undo 把前像盖回去）。
 */
int ft_tomb_append(FtIndex *ix, u32 xid, u32 doc_id)
{
    u8 *buf = NULL;
    PageFrame *fr = NULL;
    u32 pid = PAGE_ID_NIL;
    u32 count = 0;
    bool fresh = false;
    int rc;

    if (ix == NULL)
        return E_INVAL;

    buf = (u8 *)malloc(PAGE_SIZE);
    if (buf == NULL)
        return E_NOSPACE;

    if (ix->n_tomb_pages > 0 && ix->tomb_fill < FT_TOMB_CAP) {
        /* 当前页还没满 → 接着它写 */
        pid = ix->tomb_pages[ix->n_tomb_pages - 1];
        rc = pager_get(ix->pg, pid, &fr);
        if (rc != OK) {
            free(buf);
            return rc;
        }
        memcpy(buf, fr->data, PAGE_SIZE);
        count = get_u32(buf + FTM_COUNT);
        if (count >= FT_TOMB_CAP) {
            /* 页里的 count 与内存里记的对不上（不该发生）→ 当它满了，另开一页 */
            pager_unpin(ix->pg, pid, false);
            fr = NULL;
            pid = PAGE_ID_NIL;
        }
    }

    if (fr == NULL) {
        /* 开一张新页 */
        rc = pager_alloc(ix->pg, &pid, &fr);
        if (rc != OK) {
            free(buf);
            return rc;
        }
        fresh = true;
        memset(buf, 0, PAGE_SIZE);
        buf[H_TYPE] = (u8)PT_FT_TOMB;
        put_u32(buf + FTM_COUNT, 0);
        count = 0;
    }

    /* 追加这一条 */
    put_u32(buf + FTM_DATA + count * 4u, doc_id);
    put_u32(buf + FTM_COUNT, count + 1u);

    rc = ft_page_write(ix->pg, ix->wal, ix->wal_ctx, xid, pid, fr, buf, NULL);
    free(buf);
    if (rc != OK)
        return rc;

    if (fresh) {
        rc = ft_tomb_add_page(ix, pid, count + 1u);
        if (rc != OK)
            return rc;
    } else {
        ix->tomb_fill = count + 1u;
    }
    return OK;
}

/* ---------------- 恢复：装配集合 ---------------- */

/*
 * 从 tombstone 页里读出全部 doc_id（追加到 arr，返回写到的位置）。
 */
static int ft_tomb_read_page(FtIndex *ix, u32 pid, u32 *arr, u32 cap, u32 *pos)
{
    PageFrame *fr = NULL;
    u32 count;
    int rc;

    rc = pager_get(ix->pg, pid, &fr);
    if (rc != OK)
        return rc;

    if (fr->data[H_TYPE] != (u8)PT_FT_TOMB) {
        pager_unpin(ix->pg, pid, false);
        return E_CORRUPT;
    }
    count = get_u32(fr->data + FTM_COUNT);
    if (count > FT_TOMB_CAP)
        count = FT_TOMB_CAP;         /* 页坏了也不能越界读 */

    if (*pos + count > cap) {
        pager_unpin(ix->pg, pid, false);
        return E_NOSPACE;
    }
    for (u32 i = 0; i < count; i++)
        arr[(*pos)++] = get_u32(fr->data + FTM_DATA + i * 4u);

    pager_unpin(ix->pg, pid, false);
    return OK;
}

/*
 * 装配最终的 tombstone 集合。
 *
 *   最终集合 = 页里的内容 ∪ (WAL 重放看到的 − WAL 回滚撤销的)
 *
 * 【为什么是"并集"而不是"取其一"】
 *
 * 页里的内容是权威的（不会被 checkpoint 截断），
 * WAL 那一路补的是"页还没落盘、日志里已经记了"的删除。
 * 两者都要 —— 只取页会丢掉最后几笔删除，只取 WAL 会在日志被截断后丢历史。
 *
 * 【为什么还要减掉"撤销"的那一份】
 *
 * 未提交事务的删除 = "这个删除没发生过"。
 * 页里的那份会被 undo 的前像恢复掉，
 * 但 WAL 那一路仍然会看到这条删除记录 —— 不减掉它，
 * 未提交事务的删除就"诈尸"了（页面恢复了，内存集合却还删着）。
 *
 * 【doc_id 在集合里被删过几次，只算一次】
 *
 * 集合是**集合**：排序后去重。
 */
int ft_tomb_assemble(FtIndex *ix, const u32 *redo_ids, u32 n_redo,
                      const u32 *undo_ids, u32 n_undo)
{
    u32 *arr = NULL;
    u32 n = 0;
    u32 cap;
    u32 total = 0;
    int rc;

    if (ix == NULL)
        return E_INVAL;

    /* 页里一共多少条 */
    for (u32 i = 0; i < ix->n_tomb_pages; i++) {
        PageFrame *fr = NULL;

        if (pager_get(ix->pg, ix->tomb_pages[i], &fr) != OK)
            continue;
        if (fr->data[H_TYPE] == (u8)PT_FT_TOMB) {
            u32 c = get_u32(fr->data + FTM_COUNT);

            if (c <= FT_TOMB_CAP)
                total += c;
        }
        pager_unpin(ix->pg, ix->tomb_pages[i], false);
    }

    cap = total + n_redo + 1u;
    arr = (u32 *)malloc(sizeof(u32) * (size_t)cap);
    if (arr == NULL)
        return E_NOSPACE;

    /* 页里的内容 */
    for (u32 i = 0; i < ix->n_tomb_pages; i++) {
        rc = ft_tomb_read_page(ix, ix->tomb_pages[i], arr, cap, &n);
        if (rc != OK) {
            free(arr);
            return rc;
        }
    }

    /* WAL 重放看到的（未被回滚撤销的） */
    for (u32 i = 0; i < n_redo; i++) {
        bool undone = false;

        for (u32 k = 0; k < n_undo; k++) {
            if (undo_ids[k] == redo_ids[i]) {
                undone = true;
                break;
            }
        }
        if (undone)
            continue;
        if (n >= cap) {
            free(arr);
            return E_NOSPACE;
        }
        arr[n++] = redo_ids[i];
    }

    /* 排序 + 去重 */
    if (n > 1)
        qsort(arr, n, sizeof(u32), ft_tomb_u32_cmp);
    {
        u32 k = 0;

        for (u32 i = 0; i < n; i++) {
            if (k > 0 && arr[k - 1] == arr[i])
                continue;
            arr[k++] = arr[i];
        }
        n = k;
    }

    /* 装进索引的内存集合 */
    free(ix->tomb);
    ix->tomb = arr;
    ix->n_tombs = n;
    ix->cap_tombs = cap;
    return OK;
}

void ft_tomb_free(FtIndex *ix)
{
    if (ix == NULL)
        return;
    free(ix->tomb_pages);
    ix->tomb_pages = NULL;
    ix->n_tomb_pages = 0;
    ix->cap_tomb_pages = 0;
    ix->tomb_fill = 0;
}
