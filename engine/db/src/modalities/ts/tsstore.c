/*
 * tsstore.c —— 时序模态的分区存储（v1.5 步骤 2）
 *
 * 契约见 tsstore.h；目录的存与取交给 ts_dir 层（ts_dir.h 写明了分工：
 * "这一层只管目录的存与取；路由、写入分区、封窗由 tsstore.c 管"）。
 * 本文件**不自己实现目录** —— 自己再实现一遍，等于同一张磁盘上出现
 * **两种目录格式**，重开库时谁都读不懂谁的。
 *
 * 【这一层做什么】
 *   写入：时间戳 → 路由到分区 → 追加到该分区的页链尾
 *   读取：遍历所有分区的页链 → 逐行回调
 *
 * 【磁盘结构】分区数据页（PT_TS_PART，1..N 张，链式）
 *   定长行 24 字节（time i64 + tag i64 + value f64）**追加**；
 *   定长 ⇒ 第 i 行的页内偏移是**纯算术**（28 + i×24），不需要 slot 表。
 *   页内布局（自己的字段从偏移 20 起，与 ts_dir 同风格）：
 *     20 TSSP_NROWS u32 本页行数 / 24 TSSP_NEXT u32 下一张数据页 / 28 行数组
 *   meta 页与目录页由 ts_dir 管（PT_TS_META / PT_TS_DIR）。
 *
 * 【重要取舍：分区 = 一条页链 = 多个页】
 *   计划措辞是"分区写满了就新开一个分区（spill）"；本实现取的是
 *   **页写满了就续开一张页（spill），分区数不变**。理由（不变式，不是偷懒）：
 *     - 契约里 TsPartInfo 有 page_count 字段 —— 一个分区恒等于一页时它永远是 1；
 *     - "一个时间窗 ↔ 一个分区"是路由不变式（R3）的基础；一页满就新开分区 ⇒
 *       同一时间窗出现多个分区 ⇒ 目录查询不知道该回哪一个 ⇒ 静默漏数据；
 *     - 封窗（转成列式压缩页）是步骤 3 的事，步骤 2 不封窗 ⇒ 分区只能靠多页长大。
 *
 * 【坑】
 *   M6-26 路由的除法必须**向下取整**：C 的整数除法向零截断，-1/86400000 = 0，
 *         而 0 号窗口是 [0, +∞) ⇒ -1ms 落进错误的分区 ⇒ 按分区裁剪时漏数据。
 *   M6-27 页类型号是**全局**的：PT_RTREE=0x15 / PT_GEO_META=0x16 /
 *         PT_TS_META=0x17 / PT_TS_DIR=0x18 已占用 ⇒ 分区数据页取 0x19。
 *   M6-28 pin/unpin 必须配对：多还一次 ⇒ E_INVAL；少还一次 ⇒ 帧被永久钉死 ⇒
 *         帧池耗尽（pager_get 返 E_NOSPACE）⇒ 后面的 latch 顺序断言 abort。
 *   M6-29 spill 的顺序**先写新页再接链**：反过来崩在中间 ⇒ 链指向空页 ⇒
 *         静默丢行；先写新页 ⇒ 崩了只留孤儿页（浪费空间，不丢数据）。
 *   M6-30 目录里的 n_rows 是**水位线**：只在分区 spill 时落盘，其余以内存计数为准，
 *         开库时以页链实际数出来的为准并回写目录。理由：每写一行就改一次目录 =
 *         一行一次 4KB 页写，写放大离谱。代价：崩在两次 spill 之间 ⇒ 目录行数偏小，
 *         但开库会数回来 ⇒ 不会漏数据，只是多读一遍页链。
 *   M6-31 本模态**没有 WAL**：契约里没有 WAL 钩子参数 ⇒ 崩在写入中间会丢行，
 *         或目录与数据页不一致（开库报 E_CORRUPT）。补 WAL 要改契约头文件（本步不改）。
 *   M6-32 开库成本随数据量线性增长：扫页空间找 meta 页（ts_dir 的坑 M6-22）+
 *         走一遍分区页链数行（坑 M6-30 的回填代价），与 geo 的坑 M5-9 同量级。
 *   M6-33 i64 ↔ 位模式一律走 memcpy：超过 INT64_MAX 的 u64 转 i64 在 C99 是
 *         **实现定义**的，不能直接强转。
 *   M6-34 改分区窗口后新老窗口**可能重叠**：契约说改窗口只影响之后的写入 ⇒
 *         库里会同时存在"1 天窗口的老分区"与"7 天窗口的新分区"。当前规则是
 *         **起点靠后的分区优先**（与 ts_dir_find 的二分口径一致），不是"最窄优先"。
 */

#include "modalities/ts/tsstore.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "modalities/ts/ts_dir.h"
#include "storage/page.h"

/* ---------------- 页类型（模态私有，见坑 M6-27） ---------------- */

#define PT_TS_PART 0x19u

/* 分区数据页的页内布局 */
enum {
    TSSP_NROWS = 20,   /* u32 本页行数 */
    TSSP_NEXT  = 24,   /* u32 下一张数据页 */
    TSSP_DATA  = 28    /* 行数组起点 */
};

/* 行 */
#define TS_ROW_SIZE      24u
#define TS_ROWS_PER_PAGE ((PAGE_SIZE - TSSP_DATA) / TS_ROW_SIZE)  /* 169 */

/* 分区窗口上限：100 年。再大就没意义了（还容易把 i64 撑爆）。 */
#define TS_MAX_SPAN_SEC 3153600000u

/* ---------------- 内部结构 ---------------- */

/*
 * 一个分区的**运行时**状态（按目录的磁盘序下标）。
 *
 * 目录项里存的是"水位线"（坑 M6-30），真正的行数与页链尾放在这里。
 */
typedef struct {
    u32 last_page;    /* 页链尾（PAGE_ID_NIL = 空分区） */
    u32 n_rows;       /* 真实行数 */
    u32 page_count;   /* 真实页数 */
} TsPartRt;

struct TsStore {
    Pager  *pg;
    TsDir  *dir;      /* 目录（存与取交给 ts_dir） */
    u32     span_sec;

    TsPartRt *rt;     /* 运行时状态，按目录磁盘序 */
    u32       rt_count;   /* 已用个数（= 目录里的分区数） */
    u32       rt_cap;     /* 容量 */

    /*
     * 时间序下标 → 磁盘序下标。
     *
     * ts_dir 内部有一份排序数组，但不暴露"时间序第 i 个是磁盘序第几个"。
     * 而 ts_store_part_info 承诺的是"按起始时间升序" ⇒ 这里自己维护一份。
     */
    u32      *by_time;
    u32       by_time_count;
    u32       by_time_cap;

    u64 total_rows;   /* 活着的读数总数 */
    u64 page_reads;   /* 累计逻辑页读（口径同 db_geo_page_reads） */
};

/* ---------------- 小工具 ---------------- */

/* 位模式互转（坑 M6-33） */
static i64 ts_i64_of(u64 v)
{
    i64 r;

    memcpy(&r, &v, sizeof(r));
    return r;
}

static u64 ts_double_bits(double d)
{
    u64 bits;

    memcpy(&bits, &d, sizeof(bits));
    return bits;
}

static double ts_bits_double(u64 bits)
{
    double d;

    memcpy(&d, &bits, sizeof(d));
    return d;
}

/* ---------------- 时间路由（纯函数） ---------------- */

i64 ts_route_partition(i64 t_ms, i64 span_ms)
{
    i64 q;
    i64 r;
    u64 start;
    u64 end;

    /*
     * span_ms 必须是正数。契约说"必须 > 0"，
     * 这里不崩（返回 0 = 第 0 个分区的起点），但把话说死。
     */
    if (span_ms <= 0)
        return 0;

    q = t_ms / span_ms;
    r = t_ms % span_ms;

    /*
     * 坑 M6-26：向零截断 ⇒ 负数要再减 1（向下取整）。
     *
     * 举例（span = 1 天 = 86400000 ms）：
     *   t = -1        → q = 0, r = -1  → q = -1 → start = -86400000 ✓
     *   t = -86400000 → q = -1, r = 0  → q = -1 → start = -86400000 ✓
     *   t = 0         → q = 0, r = 0  → q = 0  → start = 0 ✓
     */
    if (r < 0)
        q -= 1;

    /* 用 u64 乘：|q × span| ≤ |t| ≤ 2^63，u64 装得下，且回绕有定义 */
    start = (u64)q * (u64)span_ms;
    end = start + (u64)span_ms;

    /*
     * 计划风险 R3 的断言：**part_start ≤ t < part_end**。
     *
     * 路由算错 ⇒ 数据落进错误的分区 ⇒ 查询按分区裁剪时**静默漏数据**。
     * 这类错不报错、只是"查不全"，所以在这里就地断言。
     */
    assert(ts_i64_of(start) <= t_ms);
    if (t_ms >= 0) {
        /* t 非负时 end 不会溢出 u64，直接比 */
        assert((u64)t_ms < end);
    }
    /* t 为负时，start ≤ t < 0 ≤ end 恒成立，不必再判 */

    return ts_i64_of(start);
}

/* ---------------- 运行时数组 ---------------- */

static int ts_rt_reserve(TsStore *st, u32 need)
{
    u32 cap;
    TsPartRt *np;

    if (need <= st->rt_cap)
        return OK;

    cap = st->rt_cap ? st->rt_cap : 8;
    while (cap < need)
        cap *= 2;

    np = (TsPartRt *)realloc(st->rt, (size_t)cap * sizeof(TsPartRt));
    if (np == NULL)
        return E_NOSPACE;

    memset(np + st->rt_cap, 0, (size_t)(cap - st->rt_cap) * sizeof(TsPartRt));
    st->rt = np;
    st->rt_cap = cap;
    return OK;
}

/* ---------------- 时间序索引 ---------------- */

static int ts_by_time_reserve(TsStore *st, u32 need)
{
    u32 cap;
    u32 *np;

    if (need <= st->by_time_cap)
        return OK;

    cap = st->by_time_cap ? st->by_time_cap : 8;
    while (cap < need)
        cap *= 2;

    np = (u32 *)realloc(st->by_time, (size_t)cap * sizeof(u32));
    if (np == NULL)
        return E_NOSPACE;

    st->by_time = np;
    st->by_time_cap = cap;
    return OK;
}

/* 取磁盘序第 idx 个分区的起始时间 */
static i64 ts_part_start(const TsStore *st, u32 idx)
{
    TsPartInfo info;

    if (ts_dir_get(st->dir, idx, &info) != OK)
        return 0;
    return info.start_ms;
}

/*
 * 把磁盘序下标 idx 插进时间序数组（按 start_ms 升序）。
 *
 * 相同 start_ms 的情况：新的排后面（与 ts_dir 内部的口径一致）。
 *
 * 调用前必须先 ts_by_time_reserve(st, count + 1)。
 */
static void ts_by_time_insert(TsStore *st, u32 idx)
{
    i64 key = ts_part_start(st, idx);
    u32 lo = 0;
    u32 hi = st->by_time_count;
    u32 pos;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;

        if (ts_part_start(st, st->by_time[mid]) <= key)
            lo = mid + 1;
        else
            hi = mid;
    }
    pos = lo;

    if (pos < st->by_time_count) {
        memmove(&st->by_time[pos + 1], &st->by_time[pos],
                (size_t)(st->by_time_count - pos) * sizeof(u32));
    }
    st->by_time[pos] = idx;
    st->by_time_count++;
}

/* ---------------- 数据页 ---------------- */

/*
 * 走一遍分区的页链：数行数、数页数、找链尾。
 *
 * 开库时用（把目录里的水位线校正成真值，坑 M6-30）。
 */
static int ts_walk_part(TsStore *st, u32 first_page, u32 *rows_out,
                        u32 *pages_out, u32 *last_out)
{
    u32 page = first_page;
    u32 rows = 0;
    u32 pages = 0;
    u32 last = PAGE_ID_NIL;

    while (page != PAGE_ID_NIL) {
        PageFrame *fr = NULL;
        u32 n;
        u32 next;
        int rc;

        rc = pager_get(st->pg, page, &fr);
        if (rc != OK)
            return rc;
        st->page_reads++;

        if (fr->data[H_TYPE] != (u8)PT_TS_PART) {
            (void)pager_unpin(st->pg, page, false);
            return E_CORRUPT;
        }
        if (!page_verify_checksum(fr->data)) {
            (void)pager_unpin(st->pg, page, false);
            return E_CORRUPT;
        }
        n = get_u32(fr->data + TSSP_NROWS);
        if (n > TS_ROWS_PER_PAGE) {
            (void)pager_unpin(st->pg, page, false);
            return E_CORRUPT;
        }

        rows += n;
        pages++;
        last = page;
        next = get_u32(fr->data + TSSP_NEXT);
        rc = pager_unpin(st->pg, page, false);
        if (rc != OK)
            return rc;
        page = next;
    }

    *rows_out = rows;
    *pages_out = pages;
    *last_out = last;
    return OK;
}

/*
 * 往分区追加一行。
 *
 * 页没满 ⇒ 直接追加；页满了 ⇒ spill：开一张新页，先把行写进新页，
 * 再把旧页的 next 指过去（坑 M6-29：顺序不能反）。
 */
static int ts_append_row(TsStore *st, u32 idx, const TsReading *r)
{
    TsPartRt *rt = &st->rt[idx];
    PageFrame *fr = NULL;
    u32 pid = rt->last_page;
    u32 n;
    u8 *row;
    bool spilled = false;
    int rc;

    rc = pager_get(st->pg, pid, &fr);
    if (rc != OK)
        return rc;
    st->page_reads++;

    if (fr->data[H_TYPE] != (u8)PT_TS_PART) {
        (void)pager_unpin(st->pg, pid, false);
        return E_CORRUPT;
    }

    n = get_u32(fr->data + TSSP_NROWS);
    if (n >= TS_ROWS_PER_PAGE) {
        /* spill：新开一页 */
        PageFrame *nf = NULL;
        u32 npid = 0;

        rc = pager_alloc(st->pg, &npid, &nf);
        if (rc != OK) {
            (void)pager_unpin(st->pg, pid, false);
            return rc;
        }
        page_init(nf->data, (u8)PT_TS_PART);
        put_u32(nf->data + TSSP_NROWS, 0);
        put_u32(nf->data + TSSP_NEXT, PAGE_ID_NIL);

        /* 旧页接上新页（先写新页的内容，再接链 —— 见坑 M6-29） */
        put_u32(fr->data + TSSP_NEXT, npid);
        page_set_checksum(fr->data);
        (void)pager_unpin(st->pg, pid, true);

        rt->page_count++;
        rt->last_page = npid;
        fr = nf;
        pid = npid;
        n = 0;
        spilled = true;
    }

    row = fr->data + TSSP_DATA + (size_t)n * TS_ROW_SIZE;
    put_u64(row + 0, (u64)r->time);
    put_u64(row + 8, (u64)r->tag);
    put_u64(row + 16, ts_double_bits(r->value));
    put_u32(fr->data + TSSP_NROWS, n + 1);
    page_set_checksum(fr->data);

    rc = pager_unpin(st->pg, pid, true);
    if (rc != OK)
        return rc;

    rt->n_rows++;
    st->total_rows++;

    /*
     * 页数变了 ⇒ 目录里的水位线要更新（坑 M6-30）。
     * 只在 spill 时更新：每写一行就改一次目录 = 每行一次 4KB 页写。
     */
    if (spilled) {
        TsPartInfo info;

        rc = ts_dir_get(st->dir, idx, &info);
        if (rc != OK)
            return rc;
        info.n_rows = rt->n_rows;
        info.page_count = rt->page_count;
        rc = ts_dir_update(st->dir, idx, &info);
        if (rc != OK)
            return rc;
    }

    return OK;
}

/* ---------------- 新建 ---------------- */

int ts_store_create(Pager *pg, u32 span_sec, TsStore **out)
{
    TsStore *st = NULL;
    TsDir *dir = NULL;
    int rc;

    if (!pg || !out)
        return E_INVAL;
    *out = NULL;

    if (span_sec == 0 || span_sec > TS_MAX_SPAN_SEC)
        return E_INVAL;

    /*
     * 库里已经有时序存储了吗？
     *
     * 不查的话，第二次 create 会再开一张 meta 页 ⇒ 两个时序存储并存 ⇒
     * ts_store_open 只会找到**第一个**，第二个的数据**静默不可见**。
     * 这种错不报错、只是"写进去的东西读不出来"，所以这里明确拦下来。
     */
    rc = ts_dir_open(pg, &dir);
    if (rc == OK) {
        ts_dir_destroy(dir);
        return E_CORRUPT;
    }
    if (rc != E_NOTFOUND)
        return rc;

    rc = ts_dir_create(pg, span_sec, &dir);
    if (rc != OK)
        return rc;

    st = (TsStore *)calloc(1, sizeof(TsStore));
    if (st == NULL) {
        ts_dir_destroy(dir);
        return E_NOSPACE;
    }
    st->pg = pg;
    st->dir = dir;
    st->span_sec = span_sec;
    *out = st;
    return OK;
}

/* ---------------- 打开 ---------------- */

int ts_store_open(Pager *pg, TsStore **out)
{
    TsStore *st = NULL;
    TsDir *dir = NULL;
    u32 n;
    u32 i;
    int rc;

    if (!pg || !out)
        return E_INVAL;
    *out = NULL;

    rc = ts_dir_open(pg, &dir);
    if (rc != OK)
        return rc;

    st = (TsStore *)calloc(1, sizeof(TsStore));
    if (st == NULL) {
        ts_dir_destroy(dir);
        return E_NOSPACE;
    }
    st->pg = pg;
    st->dir = dir;
    st->span_sec = ts_dir_span_sec(dir);

    n = ts_dir_count(dir);
    rc = ts_rt_reserve(st, n);
    if (rc != OK) {
        ts_store_destroy(st);
        return rc;
    }
    rc = ts_by_time_reserve(st, n);
    if (rc != OK) {
        ts_store_destroy(st);
        return rc;
    }
    st->rt_count = n;

    /* 时间序索引：先填 0..n-1，再按 start_ms 插入排序 */
    for (i = 0; i < n; i++)
        st->by_time[i] = i;
    st->by_time_count = n;
    for (i = 1; i < n; i++) {
        u32 key = st->by_time[i];
        i64 ks = ts_part_start(st, key);
        u32 j = i;

        while (j > 0 && ts_part_start(st, st->by_time[j - 1]) > ks) {
            st->by_time[j] = st->by_time[j - 1];
            j--;
        }
        st->by_time[j] = key;
    }

    /*
     * 走一遍每个分区的页链：把真实行数 / 页数数出来（坑 M6-30 的回填）。
     * 这一步的开库代价随数据量线性增长（坑 M6-32），如实记下。
     */
    for (i = 0; i < n; i++) {
        TsPartInfo info;
        u32 rows = 0;
        u32 pages = 0;
        u32 last = PAGE_ID_NIL;

        rc = ts_dir_get(dir, i, &info);
        if (rc != OK) {
            ts_store_destroy(st);
            return rc;
        }

        rc = ts_walk_part(st, info.first_page, &rows, &pages, &last);
        if (rc != OK) {
            ts_store_destroy(st);
            return rc;
        }

        st->rt[i].n_rows = rows;
        st->rt[i].page_count = pages;
        st->rt[i].last_page = last;
        st->total_rows += rows;

        /* 目录里存的是水位线 ⇒ 与真值不一致就回写（写一次目录页） */
        if (info.n_rows != rows || info.page_count != pages) {
            info.n_rows = rows;
            info.page_count = pages;
            rc = ts_dir_update(dir, i, &info);
            if (rc != OK) {
                ts_store_destroy(st);
                return rc;
            }
        }
    }

    *out = st;
    return OK;
}

/* ---------------- 释放 ---------------- */

void ts_store_destroy(TsStore *st)
{
    if (!st)
        return;
    /* 页归 pager 管，这里只还内存 */
    if (st->dir)
        ts_dir_destroy(st->dir);
    free(st->rt);
    free(st->by_time);
    free(st);
}

/* ---------------- 窗口 ---------------- */

u32 ts_store_span_sec(const TsStore *st)
{
    if (!st)
        return 0;
    return st->span_sec;
}

int ts_store_set_span_sec(TsStore *st, u32 span_sec)
{
    int rc;

    if (!st)
        return E_INVAL;
    if (span_sec == 0 || span_sec > TS_MAX_SPAN_SEC)
        return E_INVAL;

    rc = ts_dir_set_span_sec(st->dir, span_sec);
    if (rc != OK)
        return rc;
    st->span_sec = span_sec;
    return OK;
}

/* ---------------- 写 ---------------- */

int ts_store_insert(TsStore *st, const TsReading *r)
{
    i64 span_ms;
    i64 start;
    i64 end;
    u32 idx = 0;
    TsPartInfo info;
    PageFrame *fr = NULL;
    u32 pid = 0;
    u8 *row;
    int rc;

    if (!st || !r)
        return E_INVAL;

    span_ms = (i64)st->span_sec * 1000;

    /* 路由：算这个时间戳属于哪个时间窗 */
    start = ts_route_partition(r->time, span_ms);

    /* 已经有分区装着这个时间戳 ⇒ 直接追加 */
    rc = ts_dir_find(st->dir, r->time, &idx, &info);
    if (rc == OK) {
        if (info.state != TS_PART_ACTIVE) {
            /*
             * 步骤 2 不会产生非活跃分区（封窗是步骤 3）。
             * 这里只是防御：真出现了就明确报错，而不是往只读分区里写。
             */
            return E_NOTSUP;
        }
        return ts_append_row(st, idx, r);
    }
    if (rc != E_NOTFOUND)
        return rc;

    /* 新分区：先把数据页准备好（首页 + 第一行） */
    rc = pager_alloc(st->pg, &pid, &fr);
    if (rc != OK)
        return rc;

    page_init(fr->data, (u8)PT_TS_PART);
    put_u32(fr->data + TSSP_NROWS, 0);
    put_u32(fr->data + TSSP_NEXT, PAGE_ID_NIL);

    row = fr->data + TSSP_DATA;
    put_u64(row + 0, (u64)r->time);
    put_u64(row + 8, (u64)r->tag);
    put_u64(row + 16, ts_double_bits(r->value));
    put_u32(fr->data + TSSP_NROWS, 1);
    page_set_checksum(fr->data);

    rc = pager_unpin(st->pg, pid, true);
    if (rc != OK)
        return rc;

    /* 目录项落盘 */
    memset(&info, 0, sizeof(info));
    info.start_ms = start;
    /* 结束时间饱和到 INT64_MAX（避免 i64 溢出） */
    end = (start > INT64_MAX - span_ms) ? INT64_MAX : start + span_ms;
    info.end_ms = end;
    info.state = TS_PART_ACTIVE;
    info.n_rows = 1;
    info.first_page = pid;
    info.page_count = 1;

    rc = ts_dir_append(st->dir, &info);
    if (rc != OK)
        return rc;

    /* 目录项已落盘 ⇒ 运行时状态可以更新了（新项在磁盘序的末尾） */
    rc = ts_rt_reserve(st, st->rt_count + 1);
    if (rc != OK)
        return rc;
    rc = ts_by_time_reserve(st, st->by_time_count + 1);
    if (rc != OK)
        return rc;
    idx = ts_dir_count(st->dir) - 1;
    assert(idx == st->rt_count);
    st->rt[idx].last_page = pid;
    st->rt[idx].n_rows = 1;
    st->rt[idx].page_count = 1;
    st->rt_count++;
    ts_by_time_insert(st, idx);
    st->total_rows++;

    return OK;
}

/* ---------------- 读 ---------------- */

int ts_store_scan(TsStore *st, TsReadFn fn, void *ctx)
{
    u32 np;
    u32 k;

    if (!st || !fn)
        return E_INVAL;

    np = ts_dir_count(st->dir);

    /*
     * 遍历顺序 = 分区的**创建顺序**（目录的磁盘序）。
     *
     * 为什么不按时间顺序：ts_dir 的时间序下标是内部数组，
     * 拿不到"时间序第 i 个 = 磁盘序第几个"的映射，
     * 硬凑就只能每个分区线性扫一遍 ⇒ O(分区数²)。
     * 需要按时间读分区时用 ts_store_part_info（它给的正是时间序）。
     */
    for (k = 0; k < np; k++) {
        TsPartInfo info;
        u32 page;
        int rc;

        rc = ts_dir_get(st->dir, k, &info);
        if (rc != OK)
            return rc;

        page = info.first_page;
        while (page != PAGE_ID_NIL) {
            PageFrame *fr = NULL;
            u32 n;
            u32 next;

            rc = pager_get(st->pg, page, &fr);
            if (rc != OK)
                return rc;
            st->page_reads++;

            if (fr->data[H_TYPE] != (u8)PT_TS_PART) {
                (void)pager_unpin(st->pg, page, false);
                return E_CORRUPT;
            }

            n = get_u32(fr->data + TSSP_NROWS);
            if (n > TS_ROWS_PER_PAGE) {
                (void)pager_unpin(st->pg, page, false);
                return E_CORRUPT;
            }

            {
                u32 j;

                for (j = 0; j < n; j++) {
                    TsReading rr;
                    const u8 *row = fr->data + TSSP_DATA + (size_t)j * TS_ROW_SIZE;

                    rr.time = ts_i64_of(get_u64(row + 0));
                    rr.tag = ts_i64_of(get_u64(row + 8));
                    rr.value = ts_bits_double(get_u64(row + 16));

                    rc = fn(&rr, ctx);
                    if (rc != OK) {
                        (void)pager_unpin(st->pg, page, false);
                        return rc;  /* 回调要求停止：原样返回它的值 */
                    }
                }
            }

            next = get_u32(fr->data + TSSP_NEXT);
            rc = pager_unpin(st->pg, page, false);
            if (rc != OK)
                return rc;
            page = next;
        }
    }
    return OK;
}

u32 ts_store_row_count(const TsStore *st)
{
    if (!st)
        return 0;
    return (u32)st->total_rows;
}

/* ---------------- 目录查询 ---------------- */

u32 ts_store_part_count(const TsStore *st)
{
    if (!st)
        return 0;
    return ts_dir_count(st->dir);
}

int ts_store_part_info(TsStore *st, u32 i, TsPartInfo *out)
{
    u32 disk;
    int rc;

    if (!st || !out)
        return E_INVAL;
    if (i >= st->by_time_count)
        return E_INVAL;

    /* 按起始时间升序（第 i 个） */
    disk = st->by_time[i];
    rc = ts_dir_get(st->dir, disk, out);
    if (rc != OK)
        return rc;

    /*
     * 行数 / 页数用**运行时的真值**覆盖目录里的水位线（坑 M6-30）：
     * 目录项只在 spill 时落盘，中间的行数是内存里的准数。
     */
    out->n_rows = st->rt[disk].n_rows;
    out->page_count = st->rt[disk].page_count;
    return OK;
}

int ts_store_find_part(TsStore *st, i64 t_ms, TsPartInfo *out)
{
    u32 disk = 0;
    int rc;

    if (!st || !out)
        return E_INVAL;

    rc = ts_dir_find(st->dir, t_ms, &disk, out);
    if (rc != OK)
        return rc;

    out->n_rows = st->rt[disk].n_rows;
    out->page_count = st->rt[disk].page_count;
    return OK;
}

/* ---------------- 诊断 ---------------- */

u64 ts_store_page_reads(const TsStore *st)
{
    if (!st)
        return 0;
    return st->page_reads + ts_dir_page_reads(st->dir);
}
