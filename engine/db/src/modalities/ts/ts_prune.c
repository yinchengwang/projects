/*
 * ts_prune.c —— 时间谓词下推 → 分区裁剪（v1.5 步骤 4 · T7）
 *
 * 契约见 ts_prune.h；这一层只做两件事：
 *
 *   1. 把时间范围换算成分区集合（纯函数，可独立单测）；
 *   2. 给扫描路径一个"按这个集合过滤"的接法。
 *
 * 【为什么裁剪逻辑这么短】（坑 M6-47）
 *
 * 因为"少给"比"多给"坏得多：多给只是慢一点，
 * 少给是**静默漏数据**（查询按分区裁剪，漏掉的分区永远查不到）。
 *
 * 所以本层的判定只有**一条**规则（窗口与范围是否相交），
 * 而且写入侧与读出侧用的是**同一个函数**
 * （ts_prune_window_hits）—— 只有一处可能出错，也就只有一处要测。
 *
 * 【不可解析 ⇒ 全部分区】（坑 M6-48）
 *
 * 没有谓词 / 谓词里有 OR / 谓词被函数包着 / 范围反了，
 * 这些情况下调用方**给不出**一个可信的时间范围。
 * 此时必须返回**全部分区**：宁可全读，也不能猜。
 *
 * "我猜这个谓词大概只涉及最近 3 个分区"就是本层的反面教材：
 * 猜错了不报错，只是数据不见了。
 *
 * 【本层不复制页格式】
 *
 * 分区的页链格式归 ts_column.h（步骤 3 把格式常量集中到了那个头文件）。
 * 本层只调用它暴露的两个读页函数（ts_part_scan / ts_column_scan），
 * **不复制**那份格式、也**不自己**去跳页链。
 *
 * 本层做的是"该读哪些分区"；扫描时**整分区跳过**（连页都不碰），
 * 所以裁剪在 IO 上是真的省下来的 —— 不是"读完了再把行扔掉"。
 *
 * 【为什么跳过整个分区是安全的】
 *
 * 路由不变式：一条读数的时间戳一定落在它所在分区的时间窗里。
 * 于是"查询范围包含这条读数" ⇒ "该分区的窗口与范围相交"
 * ⇒ 该分区一定被选中 ⇒ 这条读数一定会被读到 ⇒ 裁剪不会少给。
 */

#include "modalities/ts/ts_prune.h"

#include <stdlib.h>
#include <string.h>

/* ---------------- 集合管理 ---------------- */

int ts_prune_set_init(TsPruneSet *s)
{
    if (!s)
        return E_INVAL;
    memset(s, 0, sizeof(*s));
    s->all = false;
    return OK;
}

void ts_prune_set_clear(TsPruneSet *s)
{
    if (!s)
        return;
    s->count = 0;
    s->all = false;
}

void ts_prune_set_destroy(TsPruneSet *s)
{
    if (!s)
        return;
    free(s->wins);
    s->wins = NULL;
    s->count = 0;
    s->cap = 0;
    s->all = false;
}

/* 往集合里追加一个窗口 */
static int set_push(TsPruneSet *s, const TsPruneWindow *w)
{
    if (s->count == s->cap) {
        u32 ncap = s->cap ? s->cap * 2u : 16u;
        TsPruneWindow *nw = (TsPruneWindow *)realloc(s->wins,
                                                     (size_t)ncap * sizeof(*nw));
        if (nw == NULL)
            return E_NOSPACE;
        s->wins = nw;
        s->cap = ncap;
    }
    s->wins[s->count++] = *w;
    return OK;
}

/* ---------------- 纯函数 ---------------- */

bool ts_prune_window_hits(i64 start_ms, i64 end_ms, i64 t_lo, i64 t_hi)
{
    /*
     * 相交判据（超集语义，见 ts_prune.h 的说明）：
     *   分区窗口 [start, end) 与查询范围 [t_lo, t_hi] 相交
     *   ⇔ start <= t_hi 且 end > t_lo
     *
     * 为什么是 `end > t_lo` 而不是 `end >= t_lo`：
     * 窗口是左闭右开 —— end == t_lo 意味着 t_lo **不在**这个窗口里
     * （它属于下一个窗口），所以这个分区不用读。
     *
     * 为什么是 `start <= t_hi` 而不是 `start < t_hi`：
     * 查询范围两端都含 —— t_hi == start 时 t_hi 这个点就在这个窗口里，
     * 所以要读。宁可多给（超集），不可少给。
     */
    if (start_ms >= end_ms)
        return false;   /* 坏窗口（时间窗倒挂）：不读，但也不该崩 */
    return (start_ms <= t_hi) && (end_ms > t_lo);
}

int ts_prune_select(TsPruneSet *out, const TsPruneWindow *wins, u32 n,
                    i64 t_lo, i64 t_hi, bool has_pred)
{
    u32 i;
    int rc;

    if (!out)
        return E_INVAL;
    if (n > 0u && !wins)
        return E_INVAL;

    /*
     * 这个函数**接管** out：先把内容清干净。
     *
     * 为什么要这么狠：out 是个结构体，里面有 wins 指针与 cap。
     * 调用方若传进来一个没初始化过的栈变量，后面 realloc(cap) 就会
     * 拿着垃圾值去干危险的事（踩内存）。
     *
     * 代价：out 里原有的内容会被丢弃（调用方不该往里先放东西）。
     */
    memset(out, 0, sizeof(*out));

    /*
     * 范围不可解析 ⇒ 全部分区（坑 M6-48）。
     *
     * "全部分区"在这里是**真的全给**：集合里放全部窗口，
     * all = true。这样 contains_time() 恒真 ⇒ 扫描与全扫一个样。
     */
    if (!has_pred || t_lo > t_hi) {
        for (i = 0; i < n; i++) {
            rc = set_push(out, &wins[i]);
            if (rc != OK) {
                ts_prune_set_destroy(out);
                return rc;
            }
        }
        out->all = true;
        return OK;
    }

    /* 范围可解析 ⇒ 只留相交的窗口（超集） */
    for (i = 0; i < n; i++) {
        if (!ts_prune_window_hits(wins[i].start_ms, wins[i].end_ms, t_lo, t_hi))
            continue;
        rc = set_push(out, &wins[i]);
        if (rc != OK) {
            ts_prune_set_destroy(out);
            return rc;
        }
    }
    out->all = false;
    return OK;
}

/* ---------------- 取窗口 ---------------- */

int ts_prune_windows_of_store(TsStore *st, TsPruneWindow **out, u32 *n_out)
{
    TsPruneWindow *wins = NULL;
    u32 n;
    u32 i;

    if (!st || !out || !n_out)
        return E_INVAL;

    *out = NULL;
    *n_out = 0;

    n = ts_store_part_count(st);
    if (n == 0u) {
        return OK;
    }

    wins = (TsPruneWindow *)calloc(n, sizeof(*wins));
    if (wins == NULL)
        return E_NOSPACE;

    for (i = 0; i < n; i++) {
        TsPartInfo info;

        if (ts_store_part_info(st, i, &info) != OK) {
            free(wins);
            return E_CORRUPT;
        }
        wins[i].start_ms = info.start_ms;
        wins[i].end_ms = info.end_ms;
        wins[i].state = (u32)info.state;
    }

    *out = wins;
    *n_out = n;
    return OK;
}

/* ---------------- 判定 ---------------- */

bool ts_prune_all(const TsPruneSet *s)
{
    return s ? s->all : true;
}

/*
 * 取集合里第 i 个窗口（磁盘序）。
 *
 * 越界 → false（**不**是错误：调用方只是想知道"有没有第 i 个"）。
 */
bool ts_prune_window_at(const TsPruneSet *s, u32 i, i64 *start_ms, i64 *end_ms,
                        u32 *state_out)
{
    if (!s || i >= s->count || !s->wins) {
        return false;
    }
    if (start_ms) {
        *start_ms = s->wins[i].start_ms;
    }
    if (end_ms) {
        *end_ms = s->wins[i].end_ms;
    }
    if (state_out) {
        *state_out = s->wins[i].state;
    }
    return true;
}

u32 ts_prune_count(const TsPruneSet *s)
{
    return s ? s->count : 0u;
}

bool ts_prune_contains_time(const TsPruneSet *s, i64 t_ms)
{
    u32 i;

    if (!s)
        return true;
    if (s->all)
        return true;   /* 没裁剪 ⇒ 什么都要读 */

    for (i = 0; i < s->count; i++) {
        if (t_ms >= s->wins[i].start_ms && t_ms < s->wins[i].end_ms)
            return true;
    }
    return false;
}

bool ts_prune_allows_window(const TsPruneSet *s, i64 start_ms, i64 end_ms)
{
    u32 i;

    if (!s)
        return true;
    if (s->all)
        return true;

    for (i = 0; i < s->count; i++) {
        if (s->wins[i].start_ms == start_ms && s->wins[i].end_ms == end_ms)
            return true;
    }
    return false;
}

/* ---------------- 扫描接法 ---------------- */

/*
 * 读一个分区（按状态二选一：裸写页 / 列式页）。
 *
 * ⚠️ 这个分支与 ts_store_scan 里的是同一件事（读路径按分区状态分派）。
 * 之所以在这里重复一遍：ts_store_scan 不接受"要跳过哪些分区"的参数，
 * 而裁剪必须**整分区跳过**（跳页链）才谈得上"少读"。
 *
 * 等步骤 3 的读路径统一之后（读路径自己认裁剪集合），
 * 这里可以退化成一行过滤。
 */
static int scan_one_part(Pager *pg, const TsPartInfo *info, TsReadFn fn,
                         void *ctx, u64 *reads)
{
    if (info->state == TS_PART_SEALED) {
        /* 封窗分区：读列式页（解码后逐行回调） */
        return ts_column_scan(pg, info->first_page, info->n_rows, fn, ctx, reads);
    }
    /* 活跃分区：读裸写页链 */
    return ts_part_scan(pg, info->first_page, fn, ctx, reads);
}

int ts_prune_scan(Pager *pg, TsStore *st, i64 t_lo, i64 t_hi, bool has_pred,
                  TsReadFn fn, void *ctx, u64 *reads_out)
{
    TsPruneSet set;
    TsPruneWindow *wins = NULL;
    u64 reads = 0;
    u32 n = 0;
    u32 np;
    u32 i;
    int rc;

    if (!pg || !st || !fn)
        return E_INVAL;

    rc = ts_prune_set_init(&set);
    if (rc != OK)
        return rc;

    rc = ts_prune_windows_of_store(st, &wins, &n);
    if (rc != OK) {
        ts_prune_set_destroy(&set);
        return rc;
    }

    rc = ts_prune_select(&set, wins, n, t_lo, t_hi, has_pred);
    free(wins);
    if (rc != OK)
        return rc;

    np = ts_store_part_count(st);
    for (i = 0; i < np; i++) {
        TsPartInfo info;

        rc = ts_store_part_info(st, i, &info);
        if (rc != OK)
            break;

        /* 不在裁剪集合里 ⇒ **整个分区跳过**（连页都不碰） */
        if (!ts_prune_allows_window(&set, info.start_ms, info.end_ms))
            continue;

        rc = scan_one_part(pg, &info, fn, ctx, &reads);
        if (rc != OK)
            break;
    }

    ts_prune_set_destroy(&set);
    if (reads_out)
        *reads_out = reads;
    return rc;
}
