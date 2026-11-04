/*
 * ft_bench.h —— 全文检索基准（A4 / A5）的公共小工具
 *
 * 基准分三个源文件（ft_bench.c 是入口，ft_bench_a4.c 跑 A4，ft_bench_a5.c 跑 A5），
 * 公共的东西（计时、中位数）放这里 —— 写成 static inline，
 * 省得再开一个 .c 文件（基准文件越散越难读）。
 *
 * ==================== 口径说明（两个基准共用） ====================
 *
 * 1) **检索页读**：`ft_index_page_reads()` 是索引级**累计**（坑 M4-16），
 *    量"一次检索读了几页"必须取**检索前后的差值**。
 *
 * 2) **合并页读**：不在那个计数器里（它只统计检索），
 *    现在有两个口径 ——
 *    - 结构口径：读 = 参与合并的旧段页数（假设每页只读一遍），
 *                写 = 新段的页数；
 *    - 实测取页次数：`ft_index_merge_page_reads()`，合并路径真正取了几次页。
 *    两者差得很远（倒排按 term 分散，合并时每个 term 单独取页）。
 *
 * 3) **逻辑页读 / 物理读盘 / 唯一页数**（v1.3 欠账 6，新增）：
 *
 *    【为什么要拆成三个】（坑 M4-32）
 *
 *    "取页次数"**不是磁盘 IO**。pager 底下有帧池：同一页取第二次时
 *    页还在帧里，**根本没碰磁盘**。所以拿"取页次数"当磁盘 IO 会把
 *    真实 IO 夸大几十倍。
 *
 *    现在 pager 层直接给三个口径（见 storage/pager.h）：
 *
 *    | 口径 | pager API | 含义 |
 *    |---|---|---|
 *    | 逻辑页读 | `pager_logical_reads()` | 取页次数（`pager_get`/`pager_alloc`），**缓存命中也计数** |
 *    | 物理读盘 | `pager_physical_reads()` | **真正从文件读回一页**的次数；帧池未命中且文件里确有这一页才 +1 |
 *    | 唯一页数 | `pager_unique_pages()` | 本测量窗口内**读盘过的不重复页号**个数（帧池无限大时的下界） |
 *
 *    另有一个 `pager_unique_pages_total()`：**全程**去重（数据足迹），
 *    它**不随测量窗口重置**。
 *
 *    【不变式：物理读盘 ≤ 逻辑页读】
 *    每次物理读盘都对应一次取页，所以物理读盘不可能超过逻辑页读。
 *    基准把它当自检 —— 出现反例就是计数器坏了。
 *
 *    【测量窗口】
 *    `pager_stats_reset(pg)` 开一个新的窗口：两个计数清零、
 *    "本窗口去重"清零（"全程去重"不受影响）。
 *    基准在每次合并 / 每次检索**之前**开窗口，**之后**立刻取数 ——
 *    中间不能夹别的读页操作，否则会混进来。
 *
 *    【为什么"新分配的页"不算读盘】
 *    新页在文件里还不存在（读到 EOF，按全零页处理），
 *    那一次没有从盘上取回任何数据 —— 不计入物理读盘。
 */

#ifndef DB_BENCH_FT_BENCH_H
#define DB_BENCH_FT_BENCH_H

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "common/types.h"
#include "modalities/ft/ft_index.h"
#include "modalities/ft/ft_segment.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 单调时钟（毫秒） */
static inline double ft_bench_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static int ft_bench_cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;

    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

/* 中位数（就地排序后取中间那个；偶数个取靠前的） */
static inline double ft_bench_median(double *v, u32 n)
{
    if (n == 0) {
        return 0.0;
    }
    qsort(v, (size_t)n, sizeof(double), ft_bench_cmp_double);
    return v[n / 2u];
}

/* ---------------- FT 索引的观察工具（A4 / A5 共用） ---------------- */

/*
 * 一个段的页数 = 段头 1 页 + 字典页 + 倒排页。
 *
 * A4 用它算"旧段页数"，A5 用它算"段的总页数" ——
 * 两个口径必须一样，所以只留一份实现。
 */
static inline u64 ft_bench_seg_pages_of(const FtSegment *s)
{
    return 1ull + (u64)s->dict_count + (u64)s->post_count;
}

/* 索引里所有"活着的"段的总页数 */
static inline u64 ft_bench_index_live_pages(const FtIndex *ix)
{
    u64 total = 0;
    u32 n = ft_index_nsegs(ix);
    u32 i;

    for (i = 0; i < n; i++) {
        const FtSegment *s = NULL;

        if (ft_index_seg_at(ix, i, &s) == OK && s != NULL) {
            total += ft_bench_seg_pages_of(s);
        }
    }
    return total;
}

/*
 * 删掉基准的临时库文件（库本体 + wal / clog / shm 三件套）。
 *
 * A4 与 A5 各建各的临时库，但删法一模一样 —— 合成一份，
 * 免得哪天加了一个后缀只改一处、另一个基准留下垃圾文件。
 */
static inline void ft_bench_remove_db(const char *path)
{
    char buf[256];

    remove(path);
    snprintf(buf, sizeof(buf), "%s-wal", path);
    remove(buf);
    snprintf(buf, sizeof(buf), "%s-clog", path);
    remove(buf);
    snprintf(buf, sizeof(buf), "%s-shm", path);
    remove(buf);
}

/* ---------------- 段快照工具 ---------------- */

/*
 * 段的快照（用来做合并前后的差分）。
 *
 * 这一块放在公共头里：一它是自成一体的一组工具（快照 → 差分），
 * 二 ft_bench_a4.c 要守住 800 行上限，装不下这一整块。
 */
typedef struct {
    u32 head;    /* 段头页号（段的唯一标识） */
    u32 docs;    /* 段里的文档数 */
    u32 terms;   /* 段里的词数 */
    u64 pages;   /* 段的页数 */
    u32 dict;    /* 字典页数 */
    u32 post;    /* 倒排页数 */
} SegSnap;

static inline u32 ft_bench_snap_segs(const FtIndex *ix, SegSnap *buf, u32 cap)
{
    u32 n = ft_index_nsegs(ix);
    u32 i;

    if (n > cap) {
        n = cap;
    }
    for (i = 0; i < n; i++) {
        const FtSegment *s = NULL;

        if (ft_index_seg_at(ix, i, &s) != OK || s == NULL) {
            buf[i].head = 0xFFFFFFFFu;
            buf[i].docs = 0;
            buf[i].terms = 0;
            buf[i].pages = 0;
            buf[i].dict = 0;
            buf[i].post = 0;
            continue;
        }
        buf[i].head = s->head_page;
        buf[i].docs = s->doc_count;
        buf[i].terms = s->n_terms;
        buf[i].pages = ft_bench_seg_pages_of(s);
        buf[i].dict = s->dict_count;
        buf[i].post = s->post_count;
    }
    return n;
}

static inline int ft_bench_snap_has(const SegSnap *buf, u32 n, u32 head)
{
    u32 i;

    for (i = 0; i < n; i++) {
        if (buf[i].head == head) {
            return 1;
        }
    }
    return 0;
}

/*
 * 合并前后的差分：谁没了（被合并掉的旧段）、谁来了（新写的段）。
 * 差分按**段头页号**做 —— 段头页号是段的唯一标识。
 */
static inline void ft_bench_diff_snaps(const SegSnap *before, u32 nb,
                                        const SegSnap *after, u32 na,
                                        u64 *removed_pages, u32 *removed_n, u64 *removed_docs,
                                        u64 *removed_terms, u32 *removed_start,
                                        u64 *added_pages, u32 *added_n, u32 *added_docs,
                                        u32 *added_terms, u32 *max_removed_terms)
{
    u32 i;
    u32 first = 0xFFFFFFFFu;

    *removed_pages = 0;
    *removed_n = 0;
    *removed_docs = 0;
    *removed_terms = 0;
    *removed_start = 0;
    *added_pages = 0;
    *added_n = 0;
    *added_docs = 0;
    *added_terms = 0;
    *max_removed_terms = 0;

    for (i = 0; i < nb; i++) {
        if (!ft_bench_snap_has(after, na, before[i].head)) {
            *removed_pages += before[i].pages;
            *removed_docs += before[i].docs;
            *removed_terms += before[i].terms;
            (*removed_n)++;
            if (i < first) {
                first = i;
            }
            if (before[i].terms > *max_removed_terms) {
                *max_removed_terms = before[i].terms;
            }
        }
    }
    /*
     * 被合并掉的旧段是**相邻**的一段（合并窗口的定义就是相邻），
     * 所以 removed_start 就是窗口的起点下标。
     */
    *removed_start = (first == 0xFFFFFFFFu) ? 0 : first;
    for (i = 0; i < na; i++) {
        if (!ft_bench_snap_has(before, nb, after[i].head)) {
            *added_pages += after[i].pages;
            (*added_n)++;
            *added_docs += after[i].docs;
            *added_terms += after[i].terms;
        }
    }
}

/* ---------------- 一次检索（只测耗时） ---------------- */

typedef struct {
    double ms;
    u32   n_hit;
    int   rc;
} FtBenchQueryStat;

/*
 * 跑一次检索，只量耗时与命中条数。
 *
 * hits 的容量固定 64 —— A4 / A5 的 max_out 都是 16，够用；
 * 传进来的 max_out 不得超过 64。
 */
static inline FtBenchQueryStat ft_bench_time_search(const FtIndex *ix, const char *q,
                                                     u32 qlen, u32 topk, u32 maxout)
{
    FtBenchQueryStat st;
    FtHit hits[64];
    u32 n = 0;
    double t0;

    memset(&st, 0, sizeof(st));
    t0 = ft_bench_now_ms();
    st.rc = ft_index_search(ix, q, qlen, topk, maxout, hits, &n);
    st.ms = ft_bench_now_ms() - t0;
    st.n_hit = n;
    return st;
}

/* ---------------- 四个 IO 口径的公共打印（v1.3 欠账 6） ---------------- */

/*
 * 一次操作（合并 / 检索）测出来的四个口径。
 *
 * 每个字段的口径见 storage/pager.h 的说明，这里只负责装数与打印。
 */
typedef struct {
    u64 logical;       /* 逻辑页读：取页次数（缓存命中也计） */
    u64 physical;      /* 物理读盘：真正从文件读回一页的次数 */
    u64 unique;        /* 唯一页数：本窗口内读盘过的不重复页号 */
    u64 unique_total;  /* 唯一页数：全程去重（数据足迹） */
    u64 seg_pages;     /* 段页数：段的结构页数 */
} FtBenchIo;

/*
 * 打印四个口径，每个数字后面一句话说清它是什么。
 *
 * 返回 0 = 自检通过；1 = 自检失败（物理读盘 > 逻辑页读 ⇒ 计数器坏了）。
 */
static inline int ft_bench_print_io(const char *title, const FtBenchIo *m)
{
    int bad = 0;

    printf("--- %s 的四个 IO 口径 ---\n", title);
    printf("  ① 逻辑页读        : %llu 次（取页次数；**缓存命中也计数**"
           "⇒ 它是逻辑访问量，不是磁盘 IO）\n", (unsigned long long)m->logical);
    printf("  ② 物理读盘        : %llu 次（**真正从文件读回一页**的次数；"
           "帧池未命中且文件里确有这一页才 +1 ⇒ 这才是真实磁盘 IO）\n",
           (unsigned long long)m->physical);
    printf("  ③ 唯一页数（去重）: %llu 页（本窗口内读盘过的不重复页号；"
           "全程去重 = %llu 页）\n",
           (unsigned long long)m->unique, (unsigned long long)m->unique_total);
    printf("  ④ 段页数          : %llu 页（段的结构页数 —— "
           "「整段都在帧池里」时的 IO 下界）\n", (unsigned long long)m->seg_pages);
    if (m->physical > m->logical) {
        printf("  ❌ 自检失败：物理读盘 (%llu) > 逻辑页读 (%llu) —— 计数器坏了！\n",
               (unsigned long long)m->physical, (unsigned long long)m->logical);
        bad = 1;
    }
    printf("  ⚠️ 口径提醒：「物理读盘」= **从文件读回一页**（read 系统调用级别），"
           "不是硬件磁盘 IO ——\n");
    printf("     基准数据由本进程刚写入，操作系统页缓存会把它服务在内存里。\n");
    printf("\n");
    return bad;
}

/* 一次合并测出来的四个口径 */
typedef struct {
    u64 old_pages;   /* 旧段页数（结构口径：参与合并的旧段的页数） */
    u64 logical;     /* 逻辑页读 */
    u64 physical;    /* 物理读盘 */
    u64 unique;      /* 唯一页数（本窗口去重） */
} FtBenchMergeIo;

/*
 * 打印每次合并的 IO 明细 + 「只看真正碰了盘的合并」的小结。
 */
static inline void ft_bench_print_merge_io(const FtBenchMergeIo *rows, u32 n)
{
    u32 i;
    u32 n_disk = 0;
    u64 disk_phys = 0;
    u64 disk_old = 0;

    printf("\n--- 每次合并的 IO 口径（pager 层）---\n");
    printf("%4s %12s %12s %12s %12s %12s\n",
           "#", "旧段页数", "逻辑页读", "物理读盘", "唯一页数", "读盘/逻辑");
    for (i = 0; i < n; i++) {
        double r = (rows[i].logical > 0)
                   ? ((double)rows[i].physical / (double)rows[i].logical) : 0.0;

        printf("%4u %12llu %12llu %12llu %12llu %12.4f\n", i + 1u,
               (unsigned long long)rows[i].old_pages,
               (unsigned long long)rows[i].logical,
               (unsigned long long)rows[i].physical,
               (unsigned long long)rows[i].unique, r);
        if (rows[i].physical > 0) {
            n_disk++;
            disk_phys += rows[i].physical;
            disk_old += rows[i].old_pages;
        }
    }
    printf("  --- 只看真正碰了盘的合并（%u / %u 次）---\n", n_disk, n);
    printf("  这 %u 次合并：Σ 物理读盘 = %llu 页，Σ 旧段页数 = %llu 页，"
           "物理读盘 / 旧段页数 = %.3f\n",
           n_disk, (unsigned long long)disk_phys, (unsigned long long)disk_old,
           (disk_old > 0) ? ((double)disk_phys / (double)disk_old) : 0.0);
    if (disk_old > 0 && (double)disk_phys / (double)disk_old >= 0.8) {
        printf("  ⇒ 比值接近 1：**每个旧页只真正读盘一次** ⇒ 真实 IO 是 **O(页数)**；\n");
    } else {
        printf("  ⇒ 比值 ≤ 1：**每个旧页最多真正读盘一次** ⇒ 真实 IO 是 **O(页数)**；\n");
        printf("     比值 < 1 说明还有一部分旧页当时仍留在帧池里，没回盘。\n");
    }
    printf("    而逻辑页读是 **O(词数)** —— 两个不是一回事。\n");
}

/* A4（合并代价）在 ft_bench_a4.c 里 */
int ft_bench_run_a4(u32 n_docs, u32 batch);

/* A5（单 term 检索的页读）在另一个源文件里 */
int ft_bench_run_a5(void);

#ifdef __cplusplus
}
#endif

#endif /* DB_BENCH_FT_BENCH_H */
