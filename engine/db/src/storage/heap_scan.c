/*
 * heap_scan.c — 堆表扫描：走本表页号表，不依赖页链
 *
 * 本文件负责：
 *   - heap_scan_begin / heap_scan_next / heap_scan_end：逐行扫描
 *   - heap_scan_page_batch：零拷贝批扫描
 *   - heap_scan_release_page：公开的"放开当前页"
 *
 * 【扫描语义】
 *   - 扫的是**本表页号表的快照**（扫描期间并发追加的新页不影响本次扫描）
 *   - 空洞（len == 0）跳过；越界的 slot 跳过；非本表的页跳过
 *   - 行的物理顺序不变（页号递增、slot 递增）
 */

#include "storage/heap_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---------------- 扫描：走本表页号表，不依赖页链 ---------------- */

int heap_scan_begin(Heap *h, HeapScan **out)
{
    if (!h || !out)
        return E_INVAL;
    HeapScan *s = calloc(1, sizeof(*s));
    if (!s)
        return E_IO;
    s->h = h;
    s->page_id = 0;
    s->next_slot = 0;
    s->idx = 0;

    /* 拍一份页号表快照：扫描期间并发追加的新页不影响本次扫描 */
    int rc = heap_plist_snapshot(h, &s->pages, &s->npages);
    if (rc != OK) {
        free(s);
        return rc;
    }

    *out = s;
    return OK;
}

/* 放开当前 pin 着的页（如果有） */
void heap_scan_release(HeapScan *s)
{
    if (s->cur) {
        pager_unpin(s->h->pager, s->page_id, false);
        s->cur = NULL;
    }
}

/*
 * 公开版：放开当前 pin 着的页。
 *
 * 为什么要给调用方用 —— 见 heap.h 里的说明：
 * 扫描器会一直 pin 着当前页，调用方拿着扫描器的时候手里就有 frame 域的锁，
 * 这时候去取 B+tree 的锁（btree 域）就变成了 "frame → btree" 的逆序。
 */
void heap_scan_release_page(HeapScan *s)
{
    if (s)
        heap_scan_release(s);
}

int heap_scan_next(HeapScan *s, Rid *rid, u8 *buf, u16 bufsize, u16 *len_out)
{
    if (!s || !rid || !buf || !len_out)
        return E_INVAL;

    Heap *h = s->h;

    for (;;) {
        if (s->idx >= s->npages) {
            heap_scan_release(s);
            return E_EOF;
        }

        /*
         * 当前页还 pin 着就直接用 —— 省掉每行两次哈希查找。
         * 换页时才重新 get。
         */
        PageFrame *f = s->cur;
        if (!f) {
            u32 pid = s->pages[s->idx];
            s->page_id = pid;
            int rc = pager_get(h->pager, pid, &f);
            if (rc != OK) {
                s->idx++;
                s->next_slot = 0;
                continue;
            }
            s->cur = f;          /* 保持 pin 住，扫完这页再放开 */

            /* 安全网：页号表里理论上都是本表的 heap 页 */
            if (f->data[H_TYPE] != PT_HEAP || page_table_id(f->data) != h->tid) {
                heap_scan_release(s);
                s->idx++;
                s->next_slot = 0;
                continue;
            }
        }

        while (s->next_slot < page_slot_count(f->data)) {
            u16 slot = s->next_slot++;
            const u8 *rec;
            u16 len;
            if (page_get(f->data, slot, &rec, &len) != OK)
                continue;

            if (len > bufsize) {
                heap_scan_release(s);
                return E_NOSPACE;
            }
            memcpy(buf, rec, len);
            *len_out = len;
            *rid = rid_make(s->page_id, slot);
            return OK;
        }

        /* 这一页扫完了 → 换下一页 */
        heap_scan_release(s);
        s->idx++;
        s->next_slot = 0;
    }
}

void heap_scan_end(HeapScan *s)
{
    if (!s)
        return;
    heap_scan_release(s);
    free(s->pages);
    free(s);
}

/* ---------------- 批量扫描（v1.2 步骤 0） ---------------- */

/*
 * 零拷贝批扫描：返回当前页里剩下的行，**行指针直接指向页内**。
 *
 * ==================== 为什么零拷贝 ====================
 *
 * 堆表的行是**内联在页里**的，所以"取一行"本来不需要拷贝 ——
 * 逐行路径的 memcpy 纯粹是接口逼出来的。
 *
 * 批路径把这件事摊开：一次给出整页的行指针，
 * 页保持 pin 住，指针就一直有效。
 *
 * 顺带消掉的还有 **4K 别名**：页缓冲（4 KB）和竞技场（256 KB）
 * 常常低 12 位相同，"从页里读、向竞技场写"撞上 store-to-load forwarding 误判，
 * 内存消歧器每次都要多等一拍 —— 实测这一项就占了 30% 的扫描时间。
 * 不拷就没有这个依赖。
 *
 * ==================== 语义必须与 heap_scan_next 一致 ====================
 *
 *   - 空洞（len == 0）跳过
 *   - 越界（off < HDR_SIZE 或 off + len > PAGE_SIZE）跳过
 *   - 非本表的页跳过
 *   - 行的物理顺序不变（页号递增、slot 递增）
 *
 * ==================== 有效期 ====================
 *
 * 行指针指向页内，所以**页必须在使用期间保持 pin**：
 * 本函数保持 s->cur 不释放，直到下一次调用（换页时释放旧页）或 heap_scan_end。
 * 调用方的约定：**行字节的有效期到下一次 batch_scan 调用为止。**
 *
 * 批次大小 = 一页里的行数（几十行），不是 1024。
 */
int heap_scan_page_batch(HeapScan *s, AmRowRef *out, int max, int *n_out)
{
    if (!s || !out || !n_out || max <= 0)
        return E_INVAL;

    *n_out = 0;

    Heap *h = s->h;

    for (;;) {
        if (s->idx >= s->npages) {
            heap_scan_release(s);
            return E_EOF;
        }

        PageFrame *f = s->cur;
        if (!f) {
            u32 pid = s->pages[s->idx];
            s->page_id = pid;
            int rc = pager_get(h->pager, pid, &f);
            if (rc != OK) {
                s->idx++;
                s->next_slot = 0;
                continue;
            }
            s->cur = f;

            /* 安全网：页号表里理论上都是本表的 heap 页 */
            if (f->data[H_TYPE] != PT_HEAP || page_table_id(f->data) != h->tid) {
                heap_scan_release(s);
                s->idx++;
                s->next_slot = 0;
                continue;
            }
        }

        const u8 *pg = f->data;
        const u16 slots = page_slot_count(pg);
        int n = 0;

        while (s->next_slot < slots && n < max) {
            const u16 slot = s->next_slot;
            const u16 len = page_slot_len_raw(pg, slot);

            if (len == 0) {                 /* 已删除（空洞） */
                s->next_slot++;
                continue;
            }

            const u16 off = page_slot_off_raw(pg, slot);
            if (off < HDR_SIZE || (u32)off + (u32)len > PAGE_SIZE) {
                s->next_slot++;
                continue;
            }

            out[n].data = pg + off;         /* 零拷贝：直接指向页内 */
            out[n].len = len;
            out[n].rid = rid_make(s->page_id, slot);
            n++;
            s->next_slot++;
        }

        *n_out = n;
        if (n > 0)
            return OK;                     /* 页保持 pin 住 */

        /* 这一页没有行 → 换下一页 */
        heap_scan_release(s);
        s->idx++;
        s->next_slot = 0;
    }
}
