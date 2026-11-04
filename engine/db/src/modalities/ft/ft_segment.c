/*
 * ft_segment.c —— 全文检索段的"段对象"（v1.3 步骤 2）
 *
 * 一个段 = 一批文档构建出来的**不可变**倒排索引：
 *
 *   PT_FT_HEAD（1 页，段头）
 *   PT_FT_DICT（若干页，字典：有序的 term 数组）
 *   PT_FT_POST（若干页，倒排：posting 数组）
 *
 * 磁盘格式的契约全在 modalities/ft/ft_internal.h —— 本文件只负责"读段头"，
 * 建段（切词、聚合、写页）在 ft_segment_build.c 里。
 *
 * 【段在内存里只有段头】
 *
 * 字典页和倒排页都是**按需从页里读**的：
 * 段对象里只有段头的字段（文档数、词数、页区间……），
 * 真正的 posting 一个都不缓存 —— 这正是"IO 可证"（验收 A5）的前提：
 * 查一个词的页读 = 字典二分的页数 + 该词的 posting 页数，与总文档量无关。
 *
 * 【段是不可变的】
 *
 * 建完之后一个字都不改。所以段对象只有"读"的方法，
 * 唯一的写字段是 page_reads（页读计数器，纯统计）。
 */

#include "modalities/ft/ft_internal.h"

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "storage/page.h"

/*
 * 从段头页里把字段读出来。
 *
 * 段头页的格式（ft_internal.h 的 FTH_*）：
 *
 *   20 FTH_VER         u8   版本
 *   24 FTH_SEG_ID      u32  段号
 *   28 FTH_DOC_COUNT   u32  文档数 N
 *   32 FTH_MAX_DOC_ID  u32  段内最大 doc_id
 *   36 FTH_DICT_START  u32  字典页起始页号
 *   40 FTH_DICT_COUNT  u32  字典页数
 *   44 FTH_POST_START  u32  倒排页起始页号
 *   48 FTH_POST_COUNT  u32  倒排页数
 *   52 FTH_NTERMS      u32  段的词数
 *   56 FTH_TOTAL_LEN   u64  所有文档的词数之和（avgdl = 它 / 文档数）
 *   64 FTH_CREATE_LSN  u32  建段时的 LSN
 */
static void ft_segment_load_head(FtSegment *seg, const u8 *head)
{
    seg->seg_id = get_u32(head + FTH_SEG_ID);
    seg->doc_count = get_u32(head + FTH_DOC_COUNT);
    seg->total_len = get_u64(head + FTH_TOTAL_LEN);
    seg->max_doc_id = get_u32(head + FTH_MAX_DOC_ID);
    seg->n_terms = get_u32(head + FTH_NTERMS);
    seg->dict_start = get_u32(head + FTH_DICT_START);
    seg->dict_count = get_u32(head + FTH_DICT_COUNT);
    seg->post_start = get_u32(head + FTH_POST_START);
    seg->post_count = get_u32(head + FTH_POST_COUNT);
    /*
     * page_reads 记的是**检索**引起的页读（ft_segment.h 的措辞）。
     * 打开段头页不算检索 —— 所以这里不计数。
     */
    seg->page_reads = 0;
}

int ft_segment_open(Pager *pg, u32 head_page, FtSegment **out)
{
    PageFrame *fr = NULL;
    FtSegment *seg;
    int rc;

    if (!pg || !out || head_page == PAGE_ID_NIL)
        return E_INVAL;

    rc = pager_get(pg, head_page, &fr);
    if (rc != OK)
        return rc;

    /*
     * 页类型或版本不对 → E_CORRUPT。
     *
     * 宁可打不开，也不要带着错数据跑 ——
     * 段里的 posting 会直接决定 BM25 的分数，
     * 页型不对还硬读的话，出来的是"看起来像结果"的错分数。
     */
    if (fr->data[H_TYPE] != (u8)PT_FT_HEAD ||
        fr->data[FTH_VER] != (u8)FT_HEAD_VER) {
        pager_unpin(pg, head_page, false);
        return E_CORRUPT;
    }

    seg = (FtSegment *)calloc(1, sizeof(*seg));
    if (!seg) {
        pager_unpin(pg, head_page, false);
        return E_IO;
    }

    seg->pg = pg;
    seg->head_page = head_page;
    ft_segment_load_head(seg, fr->data);

    pager_unpin(pg, head_page, false);
    *out = seg;
    return OK;
}

void ft_segment_close(FtSegment *seg)
{
    free(seg);
}

u64 ft_segment_page_reads(const FtSegment *seg)
{
    return seg ? seg->page_reads : 0;
}

u32 ft_segment_nterms(const FtSegment *seg)
{
    return seg ? seg->n_terms : 0;
}

/* ---------------- 段级空闲页池（v1.3 欠账 2） ---------------- */

/*
 * 空闲页池：**升序**的页号数组。
 *
 * 为什么排序：登记时要判重（同一个页发两次 = 两个段共用一页 = 数据互相踩踏），
 * 排序之后判重是一次二分查找；取页时从**末尾**取（最大的页号先复用）。
 *
 * 为什么先复用大的页号：大的页号在文件靠后，
 * 先把它们用掉可以让文件的"尾巴"保持紧凑 ——
 * 文件总长只取决于"用过的最大页号 + 1"，与中间有多少空闲页无关。
 */
struct FtPagePool {
    u32 *ids;
    u32  n;
    u32  cap;
    u64  added_total;   /* 累计登记过的页数（诊断用） */
};

FtPagePool *ft_pool_new(void)
{
    FtPagePool *pool = (FtPagePool *)calloc(1, sizeof(FtPagePool));

    return pool;   /* 允许返回 NULL：调用方要么检查，要么用"无池"路径 */
}

void ft_pool_free(FtPagePool *pool)
{
    if (pool == NULL) {
        return;
    }
    free(pool->ids);
    free(pool);
}

/* 二分：这个页号在池里吗 */
static bool ft_pool_has(const FtPagePool *pool, u32 page_id)
{
    u32 lo = 0;
    u32 hi = pool->n;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;

        if (pool->ids[mid] == page_id) {
            return true;
        }
        if (pool->ids[mid] < page_id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

int ft_pool_add(FtPagePool *pool, u32 page_id)
{
    u32 lo, hi, mid, i;

    if (pool == NULL) {
        return E_INVAL;
    }
    if (ft_pool_has(pool, page_id)) {
        return E_CORRUPT;    /* 同一个页不能发两次（内部不变量被破坏） */
    }
    if (pool->n == pool->cap) {
        u32 ncap = (pool->cap == 0) ? 64u : pool->cap * 2;
        u32 *ni;

        if (ncap < pool->cap) {
            return E_NOSPACE;     /* 翻倍溢出 */
        }
        ni = (u32 *)realloc(pool->ids, sizeof(u32) * (size_t)ncap);
        if (ni == NULL) {
            return E_NOSPACE;
        }
        pool->ids = ni;
        pool->cap = ncap;
    }
    /* 找插入点（保持升序） */
    lo = 0;
    hi = pool->n;
    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        if (pool->ids[mid] < page_id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (i = pool->n; i > lo; i--) {
        pool->ids[i] = pool->ids[i - 1];
    }
    pool->ids[lo] = page_id;
    pool->n++;
    pool->added_total++;
    return OK;
}

u32 ft_pool_count(const FtPagePool *pool)
{
    return (pool == NULL) ? 0u : pool->n;
}

u64 ft_pool_added_total(const FtPagePool *pool)
{
    return (pool == NULL) ? 0u : pool->added_total;
}

/* 从池里取一个页号（取最大的那个）；取不到返回 PAGE_ID_NIL */
static u32 ft_pool_take(FtPagePool *pool)
{
    u32 pid;

    if (pool == NULL || pool->n == 0) {
        return PAGE_ID_NIL;
    }
    pid = pool->ids[pool->n - 1];
    pool->n--;
    return pid;
}

int ft_pool_alloc(FtPagePool *pool, Pager *pg, u32 *pid_out, PageFrame **fr_out)
{
    u32 pid;
    PageFrame *fr = NULL;
    int rc;

    if (pg == NULL || pid_out == NULL || fr_out == NULL) {
        return E_INVAL;
    }
    pid = ft_pool_take(pool);
    if (pid != PAGE_ID_NIL) {
        rc = pager_get(pg, pid, &fr);
        if (rc != OK) {
            /*
             * 这个页读不出来（文件被截短了？）：把它**放回**池里，
             * 免得"取出来就丢了" —— 丢了的页等于白占文件空间。
             */
            (void)ft_pool_add(pool, pid);
            return rc;
        }
        /*
         * 【彻底重置】
         *
         * 复用页最怕旧段的字节残留：页头（H_TYPE / H_LSN）与私有区
         * 一旦残留，读侧就会拿旧段的格式去解释新段的内容。
         * 所以整页清零 —— 后面写入器会把整页覆盖一遍，
         * 但"清零"这一步不能省：写入器只写它自己用到的字节。
         */
        memset(fr->data, 0, PAGE_SIZE);
        *pid_out = pid;
        *fr_out = fr;
        return OK;
    }
    /* 池里没有 → 向文件尾追加 */
    return pager_alloc(pg, pid_out, fr_out);
}

/*
 * 登记一段连续的空闲页（[start, start+len)）。
 *
 * 合并把旧段的页还回来时，整段一起登记比一页一页登记省事
 * （旧段的页本来就是连号的：段头页 + 字典区 + 倒排区）。
 */
int ft_pool_add_range(FtPagePool *pool, u32 start, u32 len)
{
    u32 i;
    int rc;

    if (pool == NULL) {
        return E_INVAL;
    }
    for (i = 0; i < len; i++) {
        rc = ft_pool_add(pool, start + i);
        if (rc != OK && rc != E_CORRUPT) {
            return rc;
        }
    }
    return OK;
}

/*
 * 从池里**取走**一段连号的页（复用空闲页池里的页）。
 *
 * 【为什么必须是"连号的一段"】
 *
 * 字典区按"连续区间"读页 → 字典页必须连号；
 * 而空闲页池里的页是零散的（来自若干个旧段）——
 * 一页一页分配出来的页一定不连号。
 *
 * 所以这里找池里**最长的连号段**，取它的前 need 页；
 * 池里没有这么长的一段 → E_NOTFOUND（调用方改走向文件尾追加）。
 *
 * *start_out —— 这段页的第一页页号。
 */
int ft_pool_take_run(FtPagePool *pool, u32 need, u32 *start_out)
{
    u32 i = 0;
    u32 best_start = 0;
    u32 best_len = 0;

    if (pool == NULL || start_out == NULL || need == 0) {
        return E_INVAL;
    }

    /* 池里的页号是升序的 → 连号的页在数组里也是连续的 */
    while (pool != NULL && i < pool->n) {
        u32 j = i + 1;

        while (j < pool->n && pool->ids[j] == pool->ids[j - 1] + 1) {
            j++;
        }
        if ((j - i) > best_len) {
            best_len = j - i;
            best_start = i;
        }
        i = j;
    }

    if (best_len < need) {
        /*
         * 池里没有这么长的一段 → E_NOTFOUND。
         *
         * 调用方改走"向文件尾追加"那条路：这时文件会继续长，
         * 但**正确性**不受影响（页连号就行，页号在哪儿都无所谓）。
         */
        return E_NOTFOUND;
    }

    {
        u32 start = pool->ids[best_start];

        /* 把这一段从池里摘掉（后面的往前挪） */
        for (i = best_start + need; i < pool->n; i++) {
            pool->ids[i - need] = pool->ids[i];
        }
        pool->n -= need;
        pool->added_total += need;
        *start_out = start;
        return OK;
    }
}

/*
 * 取走池里**最长的那一整段**连号页（整段取走，不切分）。
 *
 * 坑 M4-52：切分长段会把它变成两段短段 —— 短段对"要连号"的人没用，
 * 对"不要连号"的人也只会更碎。整段取走 → 池里的段只变短、不变碎。
 */
int ft_pool_take_longest_run(FtPagePool *pool, u32 *start_out, u32 *len_out)
{
    u32 i = 0;
    u32 best_start = 0;
    u32 best_len = 0;

    if (pool == NULL || start_out == NULL || len_out == NULL) {
        return E_INVAL;
    }

    while (i < pool->n) {
        u32 j = i + 1;

        while (j < pool->n && pool->ids[j] == pool->ids[j - 1] + 1) {
            j++;
        }
        if ((j - i) > best_len) {
            best_len = j - i;
            best_start = i;
        }
        i = j;
    }

    if (best_len == 0) {
        return E_NOTFOUND;      /* 池是空的 */
    }

    {
        u32 start = pool->ids[best_start];

        /* 整段摘掉（后面的往前挪） */
        for (i = best_start + best_len; i < pool->n; i++) {
            pool->ids[i - best_len] = pool->ids[i];
        }
        pool->n -= best_len;
        pool->added_total += best_len;
        *start_out = start;
        *len_out = best_len;
        return OK;
    }
}

/*
 * 取走池里**最短的、够长的一段**（最佳适配）。
 *
 * 坑 M4-52：长段是稀缺资源（字典区必须连号），
 * 要 1 页却从 25 页的长段上砍 → 长段变短、短段堆积。
 * 最佳适配把"刚好够用"的那一段给出去 → 长段留给真正需要的地方。
 */
int ft_pool_take_run_best_fit(FtPagePool *pool, u32 need, u32 *start_out)
{
    u32 i = 0;
    u32 best_start = 0;
    u32 best_len = 0;

    if (pool == NULL || start_out == NULL || need == 0) {
        return E_INVAL;
    }

    while (i < pool->n) {
        u32 j = i + 1;

        while (j < pool->n && pool->ids[j] == pool->ids[j - 1] + 1) {
            j++;
        }
        if ((j - i) >= need && ((best_len == 0) || ((j - i) < best_len))) {
            best_len = j - i;
            best_start = i;
        }
        i = j;
    }

    if (best_len == 0) {
        return E_NOTFOUND;      /* 池里没有够长的一段 */
    }

    {
        u32 start = pool->ids[best_start];

        /* 只取走 need 页，剩下的（还是连号的）留在池里 */
        for (i = best_start + need; i < pool->n; i++) {
            pool->ids[i - need] = pool->ids[i];
        }
        pool->n -= need;
        pool->added_total += need;
        *start_out = start;
        return OK;
    }
}

/*
 * 新段的页数上界（合并用）。
 *
 * 见 ft_segment.h 里 FtSegBound 的说明：源段的总量就是新段的上界，
 * 合并只会减少（重复词合并、被删的文档物理丢弃）。
 */
void ft_seg_bound_merge(FtSegment *const *segs, u32 n_segs, FtSegBound *out)
{
    u64 sum_post = 0;
    u64 sum_dict = 0;
    u32 post_cap;
    u32 dict_cap;
    u32 dict_max_need;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    for (u32 i = 0; i < n_segs; i++) {
        if (segs[i] == NULL) {
            continue;
        }
        sum_post += segs[i]->post_count;
        sum_dict += segs[i]->dict_count;
    }

    post_cap = PAGE_SIZE - FPH_DATA;    /* 倒排页的数据区 */
    dict_cap = FT_DICT_CAPACITY;        /* 字典页的容量 */
    /* 字典页换页时浪费的空间：最大词条 + 一个 slot */
    dict_max_need = ft_dict_entry_bytes(FT_MAX_TOKEN_LEN) + SLOT_SIZE;

    /*
     * 倒排页上界：
     *   源段的总字节 ≤ Σ 页数 × 每页容量；
     *   新段每页最多浪费 23 字节（页尾放不下 run 头 + 一条 posting）
     *   \+ 12 字节（多出来的一个 run 头）→ 有效容量 = 容量 − 35。
     */
    out->post_pages = (u32)((sum_post * (u64)post_cap) / (u64)(post_cap - 35u)) + 1u;
    /* 字典页上界：每页最多浪费"最大词条 + slot"字节 */
    out->dict_pages = (u32)((sum_dict * (u64)dict_cap) / (u64)(dict_cap - dict_max_need)) + 1u;
    out->head_pages = 1u;
    out->total = out->head_pages + out->post_pages + out->dict_pages;
}

/* ---------------- 连续页块（v1.3 欠账 2） ---------------- */

/*
 * 段的页必须连号（坑 M4-23），而空闲页池里的页是零散的 ——
 * 所以复用的粒度是"一段连号的页"（FtBlock）。
 *
 *   - **有界块**（来自空闲页池）：next..end 是已经存在的页。
 *     取页 = pager_get(next++)；整页**清零**（复用页最怕旧段的字节残留：
 *     页头 H_TYPE / H_LSN 与私有区一旦残留，读侧就会拿旧段的格式去解释新段）。
 *   - **无界块**（end = PAGE_ID_NIL）：页按需向文件尾追加，
 *     行为与"不复用"的老口径完全一致（单测走的就是这条路）。
 */
void ft_block_init(FtBlock *b, Pager *pg, u32 start, u32 len)
{
    if (b == NULL)
        return;
    b->pg = pg;
    b->start = start;
    b->next = start;
    b->end = (len == 0xFFFFFFFFu) ? PAGE_ID_NIL : (start + len);
    b->pool = NULL;
    b->chunk_next = 0;
    b->chunk_end = 0;
    b->n_used = 0;
}

/*
 * 初始化一个**池模式**的块（坑 M4-52）。
 *
 * 页从池里按"整段连号"取：取到一段就用这一段，
 * 这一段用完了再取下一段（段与段之间由写入器用页链 H_NEXT 串起来）。
 * 池里没有页了 → 向文件尾追加（页号天然连号，正确性不受影响）。
 */
void ft_block_init_pool(FtBlock *b, Pager *pg, FtPagePool *pool)
{
    if (b == NULL)
        return;
    b->pg = pg;
    b->start = PAGE_ID_NIL;
    b->next = PAGE_ID_NIL;
    b->end = PAGE_ID_NIL;
    b->pool = pool;
    b->chunk_next = 0;
    b->chunk_end = 0;
    b->n_used = 0;
}

u32 ft_block_used(const FtBlock *b)
{
    if (b == NULL)
        return 0u;
    if (b->pool != NULL)
        return b->n_used;      /* 池模式：页不一定连号，只能数个数 */
    return b->next - b->start;
}

/*
 * 把池模式块里**没用完的那一段**的尾巴还给空闲页池（坑 M4-52）。
 *
 * 尾巴是连号的（一整段的剩余部分）→ 归还后池里仍然是一段连号页 ✓
 * （不会把池切碎）。
 */
int ft_block_pool_release_tail(FtBlock *b, FtPagePool *pool)
{
    if (b == NULL || pool == NULL)
        return E_INVAL;
    if (b->pool == NULL)
        return OK;             /* 不是池模式的块：没有"尾巴"这一说 */
    if (b->chunk_next >= b->chunk_end)
        return OK;             /* 尾巴已经用完了 */
    return ft_pool_add_range(pool, b->chunk_next, b->chunk_end - b->chunk_next);
}

int ft_block_alloc(FtBlock *b, u32 *pid_out, PageFrame **fr_out)
{
    u32 pid;
    PageFrame *fr = NULL;
    int rc;

    if (b == NULL || b->pg == NULL || pid_out == NULL || fr_out == NULL)
        return E_INVAL;

    /* ---- 池模式（坑 M4-52）：页从池里按"整段连号"取 ---- */
    if (b->pool != NULL) {
        if (b->chunk_next >= b->chunk_end) {
            /* 当前这一段用完了 → 再取一段（整段取走，不切分） */
            u32 rs = 0;
            u32 rl = 0;

            rc = ft_pool_take_longest_run(b->pool, &rs, &rl);
            if (rc != OK) {
                /*
                 * 池里没有页了 → 向文件尾追加。
                 *
                 * 页号天然连号（文件尾往后长），正确性不受影响；
                 * 只是文件会长大 —— 池里确实没页可复用了。
                 *
                 * ⚠️ pager_alloc **已经把这一帧 pin 住了**（内容锁也拿到了），
                 * 所以这里**不能**再 pager_get 一次 ——
                 * 同一帧被取两次 → 内容锁的重入深度变成 2，
                 * 而调用方只会 unpin 一次 → 那一把内容锁**永远不放开**：
                 * 内容锁不放开 ⇒ frame 域的锁计数多 1 ⇒
                 * 后面任何"拿 txn 域"的调用都会撞上"违反获取全序"（坑 M4-54）。
                 */
                rc = pager_alloc(b->pg, &pid, &fr);
                if (rc != OK)
                    return rc;
                /*
                 * 这一段只有刚分配的那一页 —— **而且已经用掉了**：
                 * chunk_next 必须指到它后面，否则下一次取页会把它**再发一次**
                 * （两个区共用一页 → 数据互相踩踏）。
                 */
                b->chunk_next = pid + 1u;
                b->chunk_end = pid + 1u;
                /* pager_alloc 已经把整页清零了（见 pager.c） */
                b->n_used++;
                *pid_out = pid;
                *fr_out = fr;
                return OK;
            }
            b->chunk_next = rs;
            b->chunk_end = rs + rl;
        }
        pid = b->chunk_next++;
        rc = pager_get(b->pg, pid, &fr);
        if (rc != OK) {
            b->chunk_next--;     /* 取不到就退回去，不浪费这一段里的位置 */
            return rc;
        }
        memset(fr->data, 0, PAGE_SIZE);   /* 复用页必须清干净（见上面的说明） */
        b->n_used++;
        *pid_out = pid;
        *fr_out = fr;
        return OK;
    }

    if (b->end == PAGE_ID_NIL) {
        /* 无界块：向文件尾追加（连续，因为中间没人插队分配） */
        rc = pager_alloc(b->pg, &pid, &fr);
        if (rc != OK)
            return rc;
        b->next = pid + 1;
        *pid_out = pid;
        *fr_out = fr;
        return OK;
    }

    if (b->next >= b->end) {
        /*
         * 块用完了 —— **不越界取页**。
         *
         * 越界的页可能属于别的段：写进去就是"两个段共用一页"，
         * 数据互相踩踏，而且**不报错**（读出来像是对的）。
         * 所以这里明确报错，让调用方按"上界算错了"处理。
         */
        return E_NOSPACE;
    }

    pid = b->next++;
    rc = pager_get(b->pg, pid, &fr);
    if (rc != OK) {
        b->next--;           /* 取不到就退回去，不浪费块里的位置 */
        return rc;
    }
    /* 彻底重置：复用页上的旧字节必须清干净（见上面的说明） */
    memset(fr->data, 0, PAGE_SIZE);
    *pid_out = pid;
    *fr_out = fr;
    return OK;
}

/*
 * 在文件尾**预留**一段连号的页（len 页），做成一个"有界块"。
 *
 * 【为什么需要它】
 *
 * 页是"谁写谁占"的：两个区交替向文件尾要页，字典页就会被倒排页顶掉 →
 * 字典区不再连号 → 读侧按连续区间读页就会读到倒排页（坑 M4-23）。
 * 所以字典区必须**先占好一整段**，再开始写。
 *
 * 做法：连着 len 次 pager_alloc 把页号占住（页号单调递增，
 * 所以占到的这段必然连号），随后把每一页放开 —— 放开只是不写盘，
 * 页号仍然被占着（谁也不会再分配到它）。内容由写入器稍后写。
 *
 * 代价：这一段页在**内容写出来之前**就是"占了号的空页" ——
 * 合并失败时它们白占位置（v1.3 欠账：失败合并的新页只浪费不回收）。
 */
int ft_block_reserve_at_end(Pager *pg, u32 len, FtBlock *blk)
{
    u32 first = PAGE_ID_NIL;

    if (pg == NULL || blk == NULL) {
        return E_INVAL;
    }
    if (len == 0) {
        ft_block_init(blk, pg, 0u, 0u);   /* 空块：一页都不占 */
        return OK;
    }
    for (u32 i = 0; i < len; i++) {
        u32 pid = PAGE_ID_NIL;
        PageFrame *fr = NULL;
        int rc = pager_alloc(pg, &pid, &fr);

        if (rc != OK) {
            return rc;
        }
        if (i == 0) {
            first = pid;
        }
        /* 只占号：内容还没写，放开（不标脏 → 这一页暂时不落盘） */
        pager_unpin(pg, pid, false);
    }
    ft_block_init(blk, pg, first, len);
    return OK;
}

/*
 * 给"**必须连号**"的区预留页块（字典区用）。
 *
 *   1. 池里有够长的连号段 → 借它（有界块，复用旧页，文件不增长）；
 *   2. 池里凑不出 → 在文件尾预留一段（页号天然连号）。
 *
 * ⚠️ 两条路都是**有界块**：字典页从头到尾都从这一段里取 →
 * 别的区（倒排区）再怎么写也顶不掉它 → 字典区一定连号 ✓。
 *
 * 返回 OK / E_NOSPACE / E_INVAL（预留失败时**不**给出一个坏块）。
 */
int ft_seg_block_reserve_strict(FtPagePool *pool, Pager *pg, u32 need, FtBlock *blk)
{
    u32 start = 0;

    if (blk == NULL || pg == NULL) {
        return E_INVAL;
    }
    if (need == 0) {
        ft_block_init(blk, pg, 0u, 0u);
        return OK;
    }
    if (pool != NULL && ft_pool_take_run_best_fit(pool, need, &start) == OK) {
        ft_block_init(blk, pg, start, need);      /* 有界块（复用旧页） */
        return OK;
    }
    return ft_block_reserve_at_end(pg, need, blk);
}

/*
 * 给一个段**预留**它的连续页块。
 *
 * 池里有够长的连号段 → 有界块（复用旧页，文件不增长）；
 * 否则 → 无界块（向文件尾追加，页号天然连号，正确性不受影响）。
 *
 * 【取哪一段：最佳适配】（坑 M4-52）
 *
 * 池里够长的段可能有好几段，取**最短的那一段**（刚好够用的优先）——
 * 长段留给真正需要长连号页的地方（字典区），别为 1 页的段头砍掉长段。
 */
void ft_seg_block_reserve(FtPagePool *pool, Pager *pg, u32 need, FtBlock *blk)
{
    u32 start = 0;

    if (blk == NULL)
        return;
    if (pool != NULL && ft_pool_take_run_best_fit(pool, need, &start) == OK) {
        ft_block_init(blk, pg, start, need);      /* 有界块（复用旧页） */
    } else {
        ft_block_init(blk, pg, 0u, 0xFFFFFFFFu);   /* 无界块（向文件尾追加） */
    }
}
/*
 * 顺着倒排页的**页链**走一遍（坑 M4-52）。
 *
 * 倒排区的页不必连号（读侧顺着 posting 的链走），
 * 所以 post_start + post_count 不是一个页号区间 ——
 * 要知道这一区到底占了哪些页，只能顺着页链（H_NEXT）走。
 *
 * fn 会对链上的每一个页号调一次。链断了（页读不出来 / 链头为 0）就提前收工 ——
 * 宁可少回收几个页，也不能把别人的页当成自己的登记进池。
 */
typedef void (*ft_page_visit_fn)(void *ctx, u32 pid);

static void ft_post_chain_visit(Pager *pg, u32 post_start, u32 post_count,
                                ft_page_visit_fn fn, void *ctx)
{
    u32 pid = post_start;

    for (u32 i = 0; i < post_count; i++) {
        PageFrame *fr = NULL;
        u32 next = 0;

        if (pid == PAGE_ID_NIL || pid == 0u) {
            break;      /* 链头为 0 = 链到头了（或链坏了） */
        }
        if (pager_get(pg, pid, &fr) != OK) {
            break;      /* 页读不出来：链断了 */
        }
        next = get_u32(fr->data + H_NEXT);
        pager_unpin(pg, pid, false);
        fn(ctx, pid);
        pid = next;
    }
}

/* 登记一个页进池（ft_post_chain_visit 的回调） */
static void ft_pool_add_visit(void *ctx, u32 pid)
{
    (void)ft_pool_add((FtPagePool *)ctx, pid);
}

/* 标记"这个页正在被某个活着的段用着"（ft_post_chain_visit 的回调） */
typedef struct {
    u8 *used;
    u32 npages;
} FtUsedMark;

static void ft_mark_used_visit(void *ctx, u32 pid)
{
    FtUsedMark *m = (FtUsedMark *)ctx;

    if (pid < m->npages) {
        m->used[pid] = 1;
    }
}

int ft_seg_recycle_pages(FtPagePool *pool, const FtSegment *seg)
{
    int rc;

    if (pool == NULL || seg == NULL) {
        return E_INVAL;
    }
    /* 段头页 */
    rc = ft_pool_add(pool, seg->head_page);
    if (rc != OK && rc != E_CORRUPT) {
        return rc;
    }
    /* 字典区（连续区间） */
    rc = ft_pool_add_range(pool, seg->dict_start, seg->dict_count);
    if (rc != OK) {
        return rc;
    }
    /* 倒排区：顺着页链走（坑 M4-52：这一区的页不必连号） */
    ft_post_chain_visit(seg->pg, seg->post_start, seg->post_count,
                        ft_pool_add_visit, pool);
    return OK;
}

/*
 * 算出"没人引用"的 FT 页（开机回收用的接口，本轮只留接口）。
 *
 * 判据：页类型是 FT 自己的三种页之一（HEAD / DICT / POST），
 * 且不属于任何一个**存活**段（段头页 + 字典区 + 倒排区）。
 * tombstone 页不在其中：它是删除集合的权威存储，由 tomb 那一路自己管。
 */
u32 ft_segment_reclaimable_pages(Pager *pg, FtSegment *const *segs, u32 n_segs,
                                 u32 *out, u32 max_out)
{
    u32 npages;
    u8 *used = NULL;
    u32 n_out = 0;
    u32 pid;

    if (pg == NULL || (out == NULL && max_out > 0)) {
        return 0;
    }
    npages = pager_npages(pg);
    if (npages == 0) {
        return 0;
    }
    used = (u8 *)calloc(npages, 1);
    if (used == NULL) {
        return 0;
    }
    /* 标出存活段用到的页 */
    for (u32 s = 0; s < n_segs; s++) {
        const FtSegment *seg = segs[s];
        u32 p;

        if (seg == NULL) {
            continue;
        }
        if (seg->head_page < npages) {
            used[seg->head_page] = 1;
        }
        for (p = seg->dict_start; p < seg->dict_start + seg->dict_count; p++) {
            if (p < npages) {
                used[p] = 1;
            }
        }
        /* 倒排区：顺着页链标（坑 M4-52：这一区的页不必连号） */
        {
            FtUsedMark m;

            m.used = used;
            m.npages = npages;
            ft_post_chain_visit(pg, seg->post_start, seg->post_count,
                                ft_mark_used_visit, &m);
        }
    }
    /* 扫页空间：没人引用的 FT 页就是垃圾 */
    for (pid = 0; pid < npages; pid++) {
        PageFrame *fr = NULL;
        u8 type;

        if (used[pid]) {
            continue;
        }
        if (pager_get(pg, pid, &fr) != OK) {
            continue;
        }
        type = fr->data[H_TYPE];
        pager_unpin(pg, pid, false);
        if (type != (u8)PT_FT_HEAD && type != (u8)PT_FT_DICT &&
            type != (u8)PT_FT_POST) {
            continue;
        }
        if (n_out >= max_out) {
            break;      /* 缓冲区满了 */
        }
        out[n_out++] = pid;
    }
    free(used);
    return n_out;
}

/* ---------------- 带页缓存的 posting 读取（v1.3 欠账 4） ---------------- */

void ft_post_cache_init(FtPostCache *cache, Pager *pg)
{
    if (cache == NULL) {
        return;
    }
    memset(cache, 0, sizeof(*cache));
    cache->pg = pg;
}

void ft_post_cache_release(FtPostCache *cache)
{
    if (cache == NULL || !cache->valid) {
        return;
    }
    pager_unpin(cache->pg, cache->page, false);
    cache->fr = NULL;
    cache->valid = false;
}

/*
 * 页读计数（与 ft_scan.c 里那份同口径）。
 *
 * seg 是 const —— 但 page_reads 只是**统计字段**，我们只改它，
 * 不改段里的任何内容（段是不可变的）。
 */
static void ft_segment_count_page_read(const FtSegment *seg)
{
    FtSegment *m = (FtSegment *)seg;

    m->page_reads++;
}

/*
 * 这个位置看起来像一个合法的 run 头吗？（与 ft_scan.c 里那份同规则）
 *
 * 判据（三条都要满足）：
 *   1. run 头落在页内、且在数据区里；
 *   2. npost > 0；
 *   3. run 头 + 它带的 posting 整段都在页内。
 */
static bool ft_run_looks_valid(const u8 *page, u32 off)
{
    u32 npost;

    if (off < FPH_DATA || off + FT_RUN_HDR_BYTES > PAGE_SIZE) {
        return false;
    }
    npost = ft_run_npost(page + off);
    if (npost == 0) {
        return false;
    }
    if (off + FT_RUN_HDR_BYTES + npost * FT_POSTING_BYTES > PAGE_SIZE) {
        return false;
    }
    return true;
}

/*
 * 读一个词的**全部** posting（带页缓存）。
 *
 * 【与 ft_segment_postings 的唯一区别】
 *
 * 那一版每个词都重新 pager_get 一次它的那张页；
 * 这一版先看"缓存里是不是就是这一页" —— 是就直接用，
 * 不是才真的去取（并把缓存换成新的一页）。
 *
 * 语义（解码规则、错误码、截断口径）**必须与 ft_segment_postings 一致**：
 * 合并拿到的 posting 要与段里的一模一样，否则合并出来的段是错的。
 *
 * ⚠️ 页读口径：只算**真的取过**的页（缓存命中不计）——
 * 记进 seg->page_reads，与 ft_scan.c 那份同口径。
 */
int ft_segment_postings_cached(const FtSegment *seg, FtPostCache *cache,
                                u64 post_ptr, u32 df,
                                FtPosting *out, u32 max_out, u32 *n_out)
{
    u32 page = ft_ptr_page(post_ptr);
    u32 off = ft_ptr_off(post_ptr);
    u32 n = 0;
    u32 last_doc = 0;
    bool have_last = false;
    u32 guard = 0;
    PageFrame *fr = NULL;
    u32 cur_page = 0;
    bool have_frame = false;
    int rc = OK;

    if (seg == NULL || n_out == NULL || cache == NULL) {
        return E_INVAL;
    }
    *n_out = 0;

    if (out == NULL) {
        return (df > 0) ? E_NOSPACE : E_INVAL;
    }
    if (df > max_out) {
        return E_NOSPACE;      /* 装不下 → 明确报错，不静默截断 */
    }

    for (;;) {
        const u8 *data;
        u32 npost;
        u32 next_run;
        u32 first_doc;
        u32 doc;

        if (++guard > 65536u) {
            rc = E_CORRUPT;     /* 链太长：段坏了 */
            goto done;
        }

        /* 换页才重新 pin —— 同一张页就用缓存里那一份（省一次逻辑页读） */
        if (!have_frame || page != cur_page) {
            if (have_frame) {
                pager_unpin(seg->pg, cur_page, false);
                have_frame = false;
            }
            if (cache->valid && cache->page == page) {
                /* 缓存命中：直接用，不重新取 */
                fr = cache->fr;
                cur_page = page;
                have_frame = true;
                cache->valid = false;    /* 帧的所有权转交给本次读取 */
            } else {
                rc = pager_get(seg->pg, page, &fr);
                if (rc != OK) {
                    goto done;
                }
                ft_segment_count_page_read(seg);
                cur_page = page;
                have_frame = true;
            }
        }

        data = fr->data;
        if (!ft_run_looks_valid(data, off)) {
            rc = E_CORRUPT;
            goto done;
        }
        npost = ft_run_npost(data + off);
        next_run = ft_run_next_run(data + off);
        first_doc = ft_run_first_doc(data + off);
        if (have_last && first_doc <= last_doc) {
            /* 倒排按 doc_id 递增：本段首文档必须大于上一段的最后一个 */
            rc = E_CORRUPT;
            goto done;
        }

        doc = first_doc;
        for (u32 i = 0; i < npost; i++) {
            const u8 *p = data + off + FTR_POSTS + i * FT_POSTING_BYTES;

            doc += ft_posting_delta(p);
            if (n >= max_out) {
                rc = E_NOSPACE;
                goto done;
            }
            out[n].doc_id = doc;
            out[n].tf = ft_posting_tf(p);
            out[n].dl = ft_posting_dl(p);
            n++;
        }

        last_doc = doc;
        have_last = true;

        if (df != 0 && n >= df) {
            break;      /* 这个词的 posting 读完了 */
        }
        if (next_run == 0) {
            break;      /* 链尾 */
        }
        page = next_run;
        off = FPH_DATA;     /* 续接的 run 一定落在那一页的 FPH_DATA 处 */
    }

    *n_out = n;
    rc = OK;

done:
    /*
     * 把最后一页**留在缓存里**（pin 着）——
     * 下一个词多半还在这一页上（词是按字典序写的，倒排页也是按这个词序排的）。
     */
    if (have_frame) {
        if (cache->valid) {
            pager_unpin(seg->pg, cache->page, false);
        }
        cache->fr = fr;
        cache->page = cur_page;
        cache->valid = true;
    }
    return rc;
}
