/*
 * ft_segment_pool.c —— 段级空闲页池 + 连续页块（v1.3 欠账 2）
 *
 * 从 ft_segment.c 拆出来（原文件 1130 行，超了 800 行硬规则）。
 *
 * 拆分的界线是"**管什么**"，不是"多少行"：
 *   ft_segment.c        段对象：读段头 / 打开 / 关闭 / 只读访问器
 *   ft_segment_pool.c   本文件：**页的账本** —— 空闲页池 + 连号页块
 *   ft_segment_build.c  建段（切词、聚合、写页）
 *
 * 为什么"页的账本"值得单独一个文件：
 *
 * 这两块（FtPagePool / FtBlock）是**纯几何 + 纯记账**，
 * 不认词的语义、不认倒排格式，只跟页号和位图打交道。
 * 它们也是"M4-23 连号页"和"M4-25 复用页残留"两条坑的全部所在地 ——
 * 放在最显眼的位置，比埋在一个 1130 行文件的中段更容易被翻到。
 *
 * 磁盘格式的契约在 ft_internal.h；这里不碰任何页内布局。
 */

#include "modalities/ft/ft_internal.h"

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "storage/page.h"

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
