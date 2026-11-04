/*
 * test_ft_catalog_dir.c —— 段目录（根页 + 目录页）**接线**的验收
 *
 * 对应 v1.3 欠账 3（开库不扫页空间）与欠账 7（段目录有独立的目录页）。
 *
 * 【这个文件在证明什么】
 *
 * 以前开库要**扫整个页空间**才能把段找齐（坑 M4-22）：
 * 开库时间与库大小成正比。
 *
 * 现在段目录是一个**显式、可定位**的对象：
 *
 *   根页（两张，交替写） → 段目录页链（段条目 + tombstone 页条目）
 *
 * 于是开库 = 读两张根页 + 读一条目录链 + 打开目录里点名的那些段，
 * **不再碰页空间**。
 *
 * 5 项验收：
 *
 *   1. 开库不扫页空间 —— 用**页读计数**证明（不是"看起来快"）：
 *      目录路径读的页数 ≪ 文件总页数；老路径（扫页空间）读的页数 ≈ 总页数。
 *   2. 两条路径结果一致 —— 同一个库，"读目录"与"扫页空间"恢复出来的
 *      **活段集合**与**tombstone 集合**必须逐条相同。
 *   3. 老库回退 —— 一个**没有根页**的库照样能开（走扫页空间的回退），
 *      而且开完之后**能把根页写出来**（下次开库就不用扫了）。
 *   4. 崩溃一致性 —— 合并进行到一半崩了（段头页标了 MERGING，
 *      但目录没更新），重开库后段集合必须还是对的：
 *      既不能"目录说有、段没有"，也不能"段在、目录没有"。
 *   5. tombstone 集合重开之后不丢。
 *
 * 【为什么页读计数是可信的证据】
 *
 * pager 提供了三个口径（v1.3 欠账 6）：
 *
 *   pager_logical_reads()  —— 取页次数（缓存命中也计数）
 *   pager_physical_reads() —— 真正从文件读回来的次数
 *   pager_unique_pages()   —— 本窗口内读盘过的不重复页数
 *
 * "扫页空间"这条路的定义就是**每个页都取一遍**，
 * 所以它的唯一页数必然 ≈ 文件总页数；
 * "读目录"这条路只碰目录里点名的页，
 * 唯一页数 = 两张根页 + 目录链 + 每个活段的段头页 + tombstone 页。
 *
 * 两个数字一摆出来，"有没有扫页空间"就不是靠嘴说的了。
 *
 * 【测试自己不做的事】
 *
 * 根页页号的**落盘**（meta 页的两个槽位）在 db 层（db_ft.c）；
 * 这一层测的是"给了页号之后，FT 侧的行为对不对"。
 * 测试里用两个变量代替 meta 页的槽位。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "common/error.h"
#include "common/loghook.h"
#include "common/types.h"
#include "modalities/ft/ft_index.h"
#include "modalities/ft/ft_index_internal.h"
#include "storage/page.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define DB_DIR   "test/tmp/c5_ft_catalog_dir_a.db"
#define DB_SCAN  "test/tmp/c5_ft_catalog_dir_b.db"
#define DB_CRASH "test/tmp/c5_ft_catalog_dir_c.db"

#define XID 7u

/* ---------------- 语料（确定性） ---------------- */

/*
 * 9 段 × 40 篇 = 360 篇，词表 600 个词。
 *
 * 【为什么要这么多】
 *
 * 词表大 → 每段的**字典页**不止一页 → 段本身占好几页；
 * 段数多 → 目录里有好几条条目。
 * 这样"扫页空间"与"读目录"的页读差距才明显（不然都是十来个页，看不出来）。
 *
 * 段数 9 > FT_MERGE_THRESHOLD(8) → 能真的走一次合并，
 * 目录里因此会出现**死段**条目（合并掉的段也要留在目录里，段号才不复用）。
 */
#define N_SEGS    9u
#define PER_SEG   40u
#define DOC_N     (N_SEGS * PER_SEG)
#define VOCAB     600u
#define DOC_WORDS 20u

static char      g_text[DOC_N][DOC_WORDS * 8];
static FtDocText  g_docs[DOC_N];

/* 被删掉的 3 篇（第一篇所在段的前三篇） */
#define N_TOMB 3u
static const u32 g_tomb_ids[N_TOMB] = {1000u, 1001u, 1002u};

static void build_corpus(void)
{
    u32 i;

    for (i = 0; i < DOC_N; i++) {
        char *p = g_text[i];

        for (u32 k = 0; k < DOC_WORDS; k++) {
            u32 w = (i * 37u + k * 53u) % VOCAB;

            p += sprintf(p, "w%03u ", w);
        }
        *p = '\0';
        g_docs[i].doc_id = 1000u + i;
        g_docs[i].text = g_text[i];
        g_docs[i].text_len = (u32)(p - g_text[i]);
    }
}

/* ---------------- 假的 WAL 钩子 ---------------- */

/*
 * 只发 LSN，不落日志 —— 测试不关心日志内容，只关心"WAL 先行"这条路径能跑通。
 */
typedef struct {
    u32 counter;
} FakeWal;

static int fake_wal(void *ctx, u32 xid, u8 logtype, u32 page_id, u16 slot,
                    u16 slot_off, const u8 *before, u16 before_len,
                    const u8 *after, u16 after_len, u32 *lsn_out)
{
    FakeWal *w = (FakeWal *)ctx;

    (void)xid;
    (void)logtype;
    (void)page_id;
    (void)slot;
    (void)slot_off;
    (void)before;
    (void)before_len;
    (void)after;
    (void)after_len;

    *lsn_out = ++w->counter;
    return OK;
}

/* ---------------- 状态快照 ---------------- */

#define MAX_SEGS 64
#define MAX_TOMBS 64

typedef struct {
    u32 nsegs;
    u32 seg_ids[MAX_SEGS];
    u32 head_pages[MAX_SEGS];
    u32 ntomb;             /* 索引说的 tombstone 条数 */
    u32 nfound;            /* 逐个 doc_id 问出来的命中数 */
    u32 tomb_ids[MAX_TOMBS];
} State;

/* 把"当前索引长什么样"拍成一张可比对的快照 */
static void collect_state(const FtIndex *ix, State *st)
{
    u32 n;

    memset(st, 0, sizeof(*st));
    n = ft_index_nsegs(ix);
    st->nsegs = n;
    if (n > MAX_SEGS)
        n = MAX_SEGS;
    for (u32 i = 0; i < n; i++) {
        const FtSegment *seg = NULL;

        if (ft_index_seg_at(ix, i, &seg) != OK)
            continue;
        st->seg_ids[i] = seg->seg_id;
        st->head_pages[i] = seg->head_page;
    }
    st->ntomb = ft_index_ntombstones(ix);
    for (u32 i = 0; i < DOC_N && st->nfound < MAX_TOMBS; i++) {
        if (ft_index_is_tomb(ix, g_docs[i].doc_id))
            st->tomb_ids[st->nfound++] = g_docs[i].doc_id;
    }
}

/* 两份快照必须一模一样（活段集合 + tombstone 集合） */
static bool state_eq(const State *a, const State *b)
{
    if (a->nsegs != b->nsegs || a->ntomb != b->ntomb || a->nfound != b->nfound)
        return false;
    for (u32 i = 0; i < a->nsegs && i < MAX_SEGS; i++) {
        if (a->seg_ids[i] != b->seg_ids[i] || a->head_pages[i] != b->head_pages[i])
            return false;
    }
    for (u32 i = 0; i < a->nfound && i < MAX_TOMBS; i++) {
        if (a->tomb_ids[i] != b->tomb_ids[i])
            return false;
    }
    return true;
}

static void dump_state(const char *tag, const State *st)
{
    printf("    %s: 活段 %u 个 [", tag, st->nsegs);
    for (u32 i = 0; i < st->nsegs && i < MAX_SEGS; i++)
        printf("%s%u", (i ? "," : ""), st->seg_ids[i]);
    printf("]，tombstone %u 条 [", st->ntomb);
    for (u32 i = 0; i < st->nfound && i < MAX_TOMBS; i++)
        printf("%s%u", (i ? "," : ""), st->tomb_ids[i]);
    printf("]\n");
}

/* ---------------- 建库 ---------------- */

/*
 * 建一个库：9 个段 + 3 次删除 + 1 次合并。
 *
 * 返回根页页号（相当于 db 层从 meta 页里读出来的那两个槽位）。
 */
static int build_db(const char *path, FakeWal *wal, u32 *root_a, u32 *root_b)
{
    Pager *pg = NULL;
    FtIndex *ix = NULL;
    int rc;

    remove(path);
    rc = pager_open(path, 4096, &pg);
    if (rc != OK)
        return rc;

    rc = ft_index_create(pg, fake_wal, wal, XID, &ix);
    if (rc != OK)
        goto out;

    for (u32 s = 0; s < N_SEGS; s++) {
        u32 seg_id = 0;

        rc = ft_index_add(ix, XID, &g_docs[s * PER_SEG], PER_SEG, &seg_id);
        if (rc != OK)
            goto out;
    }

    /* 删除 → 建一张 tombstone 页（目录里因此多一条 tombstone 条目） */
    for (u32 i = 0; i < N_TOMB; i++) {
        rc = ft_index_delete(ix, XID, g_tomb_ids[i]);
        if (rc != OK)
            goto out;
    }

    /* 合并一次 → 老段变死段，目录里因此有 DEAD 条目 */
    {
        u32 merged = 0;

        rc = ft_index_merge(ix, XID, &merged);
        if (rc != OK)
            goto out;
        CHECK(merged > 0);      /* 9 段 > 阈值 8 → 必须真的合并了 */
    }

    rc = ft_index_root_pages(ix, root_a, root_b);

out:
    if (ix != NULL)
        ft_index_close(ix);
    if (pg != NULL)
        pager_close(pg);
    return rc;
}

/* 把两张根页**写坏** —— 造一个"文件里确实没有根页"的老库 */
static void kill_root_pages(const char *path, u32 a, u32 b)
{
    Pager *pg = NULL;

    if (pager_open(path, 4096, &pg) != OK)
        return;
    for (int i = 0; i < 2; i++) {
        u32 pid = (i == 0) ? a : b;
        PageFrame *fr = NULL;

        if (pager_get(pg, pid, &fr) != OK)
            continue;
        fr->data[H_TYPE] = 0xEE;     /* 页类型不对 → 根页作废 */
        pager_unpin(pg, pid, true);
    }
    pager_flush_all(pg);
    pager_close(pg);
}

/* ---------------- 恢复（两条路径） ---------------- */

typedef struct {
    u64 logical;      /* 取页次数 */
    u64 physical;     /* 真正读盘的次数 */
    u64 unique;       /* 读盘过的不重复页数 */
    u64 npages;       /* 文件总页数 */
} Reads;

/*
 * 开库并恢复。
 *
 * bind_root != 0 → 绑上根页（目录路径）；
 * bind_root == 0 → 不绑（老库格式 → 回退扫页空间）。
 *
 * 【测量窗口必须紧贴被测操作】（坑 M4-45）
 * 所以 pager_stats_reset 就在 ft_index_recover 前面一行。
 */
static int open_and_recover(const char *path, FakeWal *wal, int bind_root,
                             u32 ra, u32 rb, FtIndex **ix_out, Pager **pg_out,
                             Reads *rd)
{
    Pager *pg = NULL;
    FtIndex *ix = NULL;
    int rc;

    memset(rd, 0, sizeof(*rd));
    rc = pager_open(path, 4096, &pg);
    if (rc != OK)
        return rc;

    rc = ft_index_create(pg, fake_wal, wal, XID, &ix);
    if (rc != OK) {
        pager_close(pg);
        return rc;
    }

    if (bind_root) {
        rc = ft_index_set_root(ix, ra, rb);
        if (rc != OK)
            goto out;
    } else {
        /* 两个都传 0 = "这个库没有根页"（老库格式） */
        rc = ft_index_set_root(ix, 0, 0);
        if (rc != OK)
            goto out;
    }

    rd->npages = pager_npages(pg);
    pager_stats_reset(pg);          /* ← 开窗口 */
    rc = ft_index_recover(ix, NULL, 0, NULL, 0);
    rd->logical = pager_logical_reads(pg);    /* ← 立刻取数 */
    rd->physical = pager_physical_reads(pg);
    rd->unique = pager_unique_pages(pg);

out:
    if (rc != OK) {
        ft_index_close(ix);
        pager_close(pg);
        return rc;
    }
    *ix_out = ix;
    *pg_out = pg;
    return OK;
}

/* 关掉一个索引 + 它的 pager（模拟"进程退出"） */
static void close_index(FtIndex *ix, Pager *pg)
{
    if (ix != NULL)
        ft_index_close(ix);
    if (pg != NULL) {
        pager_flush_all(pg);
        pager_close(pg);
    }
}

/* ---------------- 1 + 2 + 5：目录路径 vs 扫页空间 ---------------- */

static void test_dir_vs_scan(FakeWal *wal, u32 root_a, u32 root_b)
{
    FtIndex *ix = NULL;
    Pager *pg = NULL;
    Reads rd_dir;
    Reads rd_scan;
    State st_dir;
    State st_scan;
    u32 scan_a = 0;
    u32 scan_b = 0;

    printf("  1. 开库不扫页空间（页读计数为证）\n");

    /* --- 目录路径 --- */
    CHECK(open_and_recover(DB_DIR, wal, 1, root_a, root_b, &ix, &pg, &rd_dir) == OK);
    if (ix == NULL)
        return;
    collect_state(ix, &st_dir);
    dump_state("目录路径", &st_dir);

    printf("    文件总页数 %llu；目录路径：逻辑读 %llu / 物理读 %llu / 唯一页 %llu\n",
           (unsigned long long)rd_dir.npages,
           (unsigned long long)rd_dir.logical,
           (unsigned long long)rd_dir.physical,
           (unsigned long long)rd_dir.unique);

    /* 目录路径读的页必须**远少于**文件总页数 */
    CHECK(rd_dir.unique < rd_dir.npages);
    CHECK(rd_dir.unique <= st_dir.nsegs + 8u);
    /* 不变式：物理读 ≤ 逻辑读 */
    CHECK(rd_dir.physical <= rd_dir.logical);

    close_index(ix, pg);
    ix = NULL;
    pg = NULL;

    /* --- 老路径：扫页空间 --- */
    printf("  2. 同一条库走【扫页空间】回退路径 → 结果必须一致\n");

    CHECK(open_and_recover(DB_DIR, wal, 0, 0, 0, &ix, &pg, &rd_scan) == OK);
    if (ix == NULL)
        return;
    collect_state(ix, &st_scan);
    dump_state("扫页空间", &st_scan);

    printf("    扫页空间路径：逻辑读 %llu / 物理读 %llu / 唯一页 %llu\n",
           (unsigned long long)rd_scan.logical,
           (unsigned long long)rd_scan.physical,
           (unsigned long long)rd_scan.unique);

    /* 扫页空间这条路的定义就是"每个页都取一遍" → 唯一页数 ≈ 总页数 */
    CHECK(rd_scan.unique >= rd_scan.npages);
    /* 目录路径明显更省 */
    CHECK(rd_dir.unique * 2u < rd_scan.unique);

    /* 两条路径恢复出来的活段集合与 tombstone 集合必须一致 */
    CHECK(state_eq(&st_dir, &st_scan));

    /* tombstone 集合没丢 */
    CHECK(st_dir.ntomb == N_TOMB);
    CHECK(st_dir.nfound == N_TOMB);

    /* 回退之后**顺手把根页写出来了**（下次开库就不用扫了） */
    CHECK(ft_index_root_pages(ix, &scan_a, &scan_b) == OK);
    CHECK(scan_a != 0 && scan_b != 0);
    printf("    回退之后写出了根页：a=%u b=%u\n", scan_a, scan_b);

    close_index(ix, pg);
    ix = NULL;
    pg = NULL;

    /* 再开一次：绑上刚写出来的根页 → 这次走目录路径，页读应该很少 */
    {
        Reads rd2;
        State st2;

        CHECK(open_and_recover(DB_DIR, wal, 1, scan_a, scan_b, &ix, &pg, &rd2) == OK);
        if (ix == NULL)
            return;
        collect_state(ix, &st2);
        printf("    绑上根页后再开：逻辑读 %llu / 唯一页 %llu\n",
               (unsigned long long)rd2.logical,
               (unsigned long long)rd2.unique);
        CHECK(rd2.unique <= st2.nsegs + 8u);
        CHECK(state_eq(&st_dir, &st2));
        close_index(ix, pg);
        ix = NULL;
        pg = NULL;
    }
}

/* ---------------- 3：老库（文件里没有根页） ---------------- */

static void test_legacy_db(FakeWal *wal, u32 root_a, u32 root_b)
{
    FtIndex *ix = NULL;
    Pager *pg = NULL;
    Reads rd;
    State st_legacy;
    State st_dir;
    u32 new_a = 0;
    u32 new_b = 0;

    printf("  3. 老库（没有根页）→ 回退开库，且开完能写出根页\n");

    /* 把文件里的两张根页写坏 —— 文件里就真的没有根页了 */
    kill_root_pages(DB_SCAN, root_a, root_b);

    CHECK(open_and_recover(DB_SCAN, wal, 0, 0, 0, &ix, &pg, &rd) == OK);
    if (ix == NULL)
        return;
    collect_state(ix, &st_legacy);
    dump_state("老库回退", &st_legacy);
    printf("    老库回退：文件总页数 %llu，唯一页 %llu\n",
           (unsigned long long)rd.npages, (unsigned long long)rd.unique);

    /* 回退路径确实扫了页空间 */
    CHECK(rd.unique >= rd.npages);

    /* 回退之后写出了根页 */
    CHECK(ft_index_root_pages(ix, &new_a, &new_b) == OK);
    CHECK(new_a != 0 && new_b != 0);
    close_index(ix, pg);
    ix = NULL;
    pg = NULL;

    /* 用新根页再开一次 → 走目录路径，页读很少，结果与回退路径一致 */
    {
        Reads rd2;
        State st2;

        CHECK(open_and_recover(DB_SCAN, wal, 1, new_a, new_b, &ix, &pg, &rd2) == OK);
        if (ix == NULL)
            return;
        collect_state(ix, &st2);
        printf("    新根页再开：唯一页 %llu\n", (unsigned long long)rd2.unique);
        CHECK(rd2.unique <= st2.nsegs + 8u);
        CHECK(state_eq(&st_legacy, &st2));
        close_index(ix, pg);
        ix = NULL;
        pg = NULL;
    }

    /* 与"目录路径"那份库比：同一个语料建出来的库，段集合必须一致 */
    {
        FtIndex *ix2 = NULL;
        Pager *pg2 = NULL;
        Reads rdx;

        CHECK(open_and_recover(DB_DIR, wal, 1, root_a, root_b, &ix2, &pg2, &rdx) == OK);
        if (ix2 != NULL) {
            collect_state(ix2, &st_dir);
            CHECK(state_eq(&st_dir, &st_legacy));
            close_index(ix2, pg2);
        }
    }
}

/* ---------------- 4：崩溃一致性（合并进行到一半） ---------------- */

static void test_crash_mid_merge(FakeWal *wal, u32 root_a, u32 root_b)
{
    FtIndex *ix = NULL;
    Pager *pg = NULL;
    Reads rd;
    State st_before;
    State st_after;
    u32 heads[2];
    u32 n = 0;

    printf("  4. 崩溃一致性：合并进行到一半崩了 → 段集合必须还是对的\n");

    /* 先正常开一次，记下"合并前"的段集合 */
    CHECK(open_and_recover(DB_CRASH, wal, 1, root_a, root_b, &ix, &pg, &rd) == OK);
    if (ix == NULL)
        return;
    collect_state(ix, &st_before);
    dump_state("合并前", &st_before);

    /* 模拟"合并开始了"：把两个段标成 MERGING（目标段号是一个不存在的号） */
    for (u32 i = 0; i < st_before.nsegs && n < 2; i++) {
        heads[n++] = st_before.head_pages[i];
    }
    CHECK(n == 2);
    CHECK(ft_catalog_mark_merge(ix, XID, 0x7FFFFF00u, heads, n) == OK);

    /* 目录**不动**（合并还没提交）—— 这正是"目录 = 已提交视图" */

    /* 模拟断电：把脏页刷下去就直接"进程退出"（不回滚、不写目录） */
    close_index(ix, pg);
    ix = NULL;
    pg = NULL;

    /* 重开（目录路径）：段集合必须与合并前一样 */
    CHECK(open_and_recover(DB_CRASH, wal, 1, root_a, root_b, &ix, &pg, &rd) == OK);
    if (ix == NULL)
        return;
    collect_state(ix, &st_after);
    dump_state("重开（目录）", &st_after);
    CHECK(state_eq(&st_before, &st_after));
    close_index(ix, pg);
    ix = NULL;
    pg = NULL;

    /* 重开（老路径扫页空间）：MERGING 且目标段不存在 → 老段仍然活着 */
    CHECK(open_and_recover(DB_CRASH, wal, 0, 0, 0, &ix, &pg, &rd) == OK);
    if (ix == NULL)
        return;
    collect_state(ix, &st_after);
    dump_state("重开（扫页空间）", &st_after);
    CHECK(state_eq(&st_before, &st_after));
    close_index(ix, pg);
    ix = NULL;
    pg = NULL;
}

/* ---------------- main ---------------- */

int main(void)
{
    FakeWal wal;
    u32 root_a = 0;
    u32 root_b = 0;
    u32 root_a2 = 0;
    u32 root_b2 = 0;

    memset(&wal, 0, sizeof(wal));
    build_corpus();

    printf("=== 段目录接线：开库不扫页空间 ===\n");

    /* 建三个内容相同的库（各自独立，免得一个测试的写入污染另一个） */
    CHECK(build_db(DB_DIR, &wal, &root_a, &root_b) == OK);
    CHECK(build_db(DB_SCAN, &wal, &root_a2, &root_b2) == OK);
    CHECK(build_db(DB_CRASH, &wal, &root_a2, &root_b2) == OK);
    if (root_a == 0 || root_b == 0) {
        printf("    FAIL: 建库后没有拿到根页页号\n");
        failures++;
        return 1;
    }
    printf("  根页页号：a=%u b=%u\n", root_a, root_b);

    test_dir_vs_scan(&wal, root_a, root_b);
    test_legacy_db(&wal, root_a2, root_b2);
    test_crash_mid_merge(&wal, root_a2, root_b2);

    remove(DB_DIR);
    remove(DB_SCAN);
    remove(DB_CRASH);

    if (failures == 0) {
        printf("=== 全部通过 ===\n");
        return 0;
    }
    printf("=== %d 处失败 ===\n", failures);
    return 1;
}
