/*
 * ts_bench_io.c —— 时序模态的**统一 IO 口径**基准（W-D 的 D-3.3）
 *
 * ==================== 这个基准要回答什么 ====================
 *
 * A8（PLAN-v1.5）要求「分区裁剪真的少读了页」，可 v1.5 收尾时只做了
 * **压缩比**基准（ts_bench.c，量的是编码后的字节数，一个 IO 指标都没有）。
 * 于是「裁剪省了 IO」这句话当时**没有可复现的证据**。
 *
 * 本基准给出那条证据：同一批数据、同一套代码，跑两条路径 ——
 *
 *   ① **全表扫描**（无时间谓词，ts_store_scan）
 *   ② **分区裁剪**（带只覆盖 1 个分区的时间窗，ts_prune_scan）
 *
 * 统一输出六项指标（一条可复现命令，见文末用法）：
 *
 *   rows / partitions scanned / logical reads / physical reads / bytes read / elapsed ms
 *
 * ==================== 判据（四条，各证明一件事） ====================
 *
 *  T1 **裁剪是真的少读**：裁剪路径的 `partitions scanned` 与 `logical reads`
 *     必须显著小于全扫（阈值：逻辑读 ≤ 全扫的 60%）。这一条抓「裁剪没生效」。
 *
 *  T2 **裁剪没有少给**（超集铁律，坑 M6-47/M6-48）：
 *     裁剪路径读到的行数必须**等于**「全扫结果里时间落在窗口内」的行数。
 *     这一条抓「裁剪过头」—— 少给是**静默漏数据**，比多给坏得多。
 *
 *  T3 **逻辑读与帧池无关**：同一份数据用大帧池与小帧池各跑一遍，
 *     逻辑读两次必须**逐位一致**（它是「访问了几次页」，与缓存命中无关）。
 *
 *  T4 **物理读与帧池有关**（这才是「物理读」的定义）：
 *     对同一个分区**连扫两遍**（一个测量窗口内）——
 *       - 大帧池（装得下）⇒ 第二遍全命中 ⇒ 物理读 ≤ 唯一页数；
 *       - 小帧池（装不下）⇒ 第一遍末尾把开头挤掉了 ⇒ 第二遍必须回盘 ⇒
 *         物理读 **> 唯一页数**。
 *
 *     ⚠️ **不能**用「小帧池下物理读必须上升，否则失败」来卡 ——
 *     纯顺序单趟扫描**永远**不会重访页，帧池再小也不会多回盘。
 *     那时「物理读不上升」是**正确行为**，不是缺陷。
 *     所以本判据用「连扫两遍」把重访**造出来**，再证明两个口径各自独立。
 *     这正是 C7 空判据踩过的坑：判据必须能被真实差异触发，否则就是空转。
 *
 * ==================== 为什么用 DbInstance 内部 API 而不是公开 API ====================
 *
 * 公开 API（include/db.h）能建存储、能插数据、能 SELECT 读回，
 * 但**拿不到** pager 的三个计数器（它们不在公开面）。
 * 而本基准的全部意义就是量那三个数 —— 所以走 inst->pager / inst->ts，
 * 与 test_ft_reclaim_boot.c 用 ft_index_seg_pool 是同一个理由：
 * **公共接口造不出这个观测**。
 *
 * ==================== 三点必须先知道的实现约束 ====================
 *
 *  ① **每分区行数必须 < TS_SEAL_ROW_THRESHOLD（10000）**。
 *     到阈值就自动封窗（tsstore.c:339），封窗后分区只读、**不能再写**
 *     ⇒ 灌第 10001 条时 db_ts_insert 返回「落在已封窗的分区里」。
 *     本基准取 9000 行/分区，留一点余量。
 *
 *  ② **灌完必须 db_checkpoint()**。
 *     否则数据页还只在 WAL / 帧池里，数据文件可能一个页都没有 ——
 *     重开库时「读到 EOF 按全零页处理」不计入物理读盘（坑 M4-46）
 *     ⇒ 物理读永远是 0，T4 判不出来。
 *
 *  ③ **时间基点必须对齐到分区边界**。
 *     分区按 `floor(t_ms / span_ms) * span_ms` 路由，边界对齐的是 **epoch 的
 *     span 整数倍**，不是「本地某天 0 点」。不对齐的话「只覆盖第 1 个分区的
 *     窗口」会切到两个分区，T2 拿「1 个分区的行数」去比必然红 ——
 *     那不是裁剪的错，是基准自己算错了窗口。
 *     另外窗口右端要写 `base + day - 1`：ts_prune 的相交判据是
 *     `start <= t_hi`（两端都含、宁可多给），写 `base + day` 会把第 2 个分区也选上。
 *
 * ==================== 用法 ====================
 *
 *   sh bench/ts_bench_io_run.sh                 # 3 分区 × 9000 行
 *   sh bench/ts_bench_io_run.sh 4 9000          # 自定分区数（行数仍要 < 10000）
 *
 * 帧池两档写死在基准里（大 = 4096 帧装得下，小 = 48 帧装不下），
 * 因为 T3/T4 判的就是「同一份数据在两档帧池下的差别」——
 * 若允许外部覆盖，判据就失去参照点了。
 *
 * 退出码：0 = 四条判据全过；1 = 有判据不达标；2 = 基准自身出错。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "db.h"
#include "db_internal.h"
#include "modalities/ts/ts_prune.h"
#include "modalities/ts/ts_seal.h"
#include "modalities/ts/tsstore.h"
#include "storage/pager.h"

#define TSIO_DB            "test/tmp/ts_bench_io.db"
#define TSIO_DEFAULT_DAYS  3
#define TSIO_DEFAULT_ROWS  9000      /* 必须 < TS_SEAL_ROW_THRESHOLD（10000） */
#define TSIO_BIG_FRAMES    4096u
#define TSIO_SMALL_FRAMES  48u       /* 单个分区约 53 页 ⇒ 装不下，第二遍必须回盘 */

/* ---------------- 计数上下文 ---------------- */

typedef struct {
    u64 n;              /* 读到的行数（**每次遍历**都累加，两遍就是两倍） */
    i64 sum_tag;
    i64 t_lo;
    i64 t_hi;
    bool only_in_window; /* true = 手工按窗口过滤（做对拍标尺用） */
} ScanCtx;

static int scan_cb(const TsReading *r, void *ctx)
{
    ScanCtx *c = (ScanCtx *)ctx;

    /*
     * 「手工过滤」的窗口口径必须与 ts_prune 的**完全一致**。
     *
     * ts_prune_window_hits 把查询范围当成**两端都含**的闭区间
     * （见 ts_prune.c:107 那段注：`start <= t_hi` 而不是 `start < t_hi`，
     * 理由是「宁可多给，不可少给」）。
     * 所以这里也是 `time > t_hi` 才排除 —— 若写成 `>=`，
     * 对拍标尺会比裁剪少一条边界行，T2 会给出假的「少给」告警。
     */
    if (c->only_in_window && (r->time < c->t_lo || r->time > c->t_hi)) {
        return 0;
    }
    c->n++;
    c->sum_tag += r->tag;
    return 0;
}

/* ---------------- 六个指标 ---------------- */

typedef struct {
    u64    rows;
    u32    parts_scanned;
    u64    logical;
    u64    physical;
    u64    unique;
    u64    bytes;
    double ms;
    u32    frames;
    u32    passes;      /* 同一窗口内连扫了几遍 */
} TsIoMetrics;

static TsIoMetrics g_m;

/* ---------------- 工具 ---------------- */

static void tsio_rm_db(void)
{
    char buf[256];

    remove(TSIO_DB);
    snprintf(buf, sizeof(buf), "%s-wal", TSIO_DB);   remove(buf);
    snprintf(buf, sizeof(buf), "%s-clog", TSIO_DB);  remove(buf);
    snprintf(buf, sizeof(buf), "%s-shm", TSIO_DB);   remove(buf);
}

/*
 * 帧池大小由 db_open 内部的 bp_frames() 读 **DB_BP_FRAMES** 决定
 * （见 src/db.c:61/76）。所以跨轮改帧池的办法就是改这一项再**重新开库** ——
 * pager 是每次 db_open 重建的。
 */
static void tsio_set_frames(u32 n)
{
#ifdef _WIN32
    char v[32];
    _snprintf_s(v, sizeof(v), sizeof(v) - 1, "%u", n);
    _putenv_s("DB_BP_FRAMES", v);
#else
    char v[32];
    snprintf(v, sizeof(v), "%u", n);
    setenv("DB_BP_FRAMES", v, 1);
#endif
}

static double tsio_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/* 建库 + 建时序存储 + 灌 days × rows_per_day 条数据（每天一个分区） */
static int tsio_load(int days, int rows_per_day, i64 base_ms)
{
    DB *db = NULL;
    i64 day_ms = 24LL * 3600LL * 1000LL;
    int d, i;

    tsio_rm_db();
    tsio_set_frames(TSIO_BIG_FRAMES);   /* 灌数用大帧池，别让写路径被抖动干扰 */
    if (db_open(TSIO_DB, &db) != DB_OK) {
        printf("  开库失败：%s\n", db_last_error(db));
        return -1;
    }
    if (db_ts_create(db) != DB_OK) {
        printf("  建时序存储失败：%s\n", db_last_error(db));
        db_close(db);
        return -1;
    }

    for (d = 0; d < days; d++) {
        for (i = 0; i < rows_per_day; i++) {
            db_ts_reading r;

            r.time  = base_ms + (i64)d * day_ms + (i64)i * 1000LL;
            r.tag   = (i64)(d * 1000 + i);
            r.value = (double)(d * 1000 + i) * 0.5;
            if (db_ts_insert(db, &r) != DB_OK) {
                printf("  插入失败（第 %d 天第 %d 条）：%s\n", d, i, db_last_error(db));
                db_close(db);
                return -1;
            }
        }
    }

    /*
     * ⚠️ 必须 checkpoint（见文件头约束 ②）：
     * 不刷脏页的话数据页可能一个都没落盘，重开库时读页走「EOF 按全零」
     * ⇒ 不计入物理读盘 ⇒ T4 永远测不出差别。
     */
    if (db_checkpoint(db) != DB_OK) {
        printf("  checkpoint 失败：%s\n", db_last_error(db));
        db_close(db);
        return -1;
    }

    {
        uint64_t n = 0;
        uint32_t np = 0;

        if (db_ts_count(db, &n) != DB_OK || db_ts_part_count(db, &np) != DB_OK) {
            printf("  统计失败\n");
            db_close(db);
            return -1;
        }
        printf("  灌数：%d 天 × %d 条 = %llu 条，%u 个分区（已 checkpoint 落盘）\n",
               days, rows_per_day, (unsigned long long)n, np);
        if ((int)n != days * rows_per_day) {
            printf("    FAIL 灌进去的条数不对：期望 %d，实际 %llu\n",
                   days * rows_per_day, (unsigned long long)n);
            db_close(db);
            return -1;
        }
    }

    db_close(db);
    return 0;
}

/*
 * 跑一次扫描，填 g_m。
 *
 * mode:   0 = 全表扫描（无谓词，ts_store_scan）
 *         1 = 分区裁剪（带时间窗，ts_prune_scan）
 * passes: 同一测量窗口内连扫几遍（T4 靠它把「重访」造出来）
 *
 * ⚠️ 测量窗口必须**紧贴**被测操作（坑 M4-45）：
 * 开窗口 → 连扫 passes 遍 → 立刻取数。中间不能夹别的读页操作。
 * 「分区数」这类统计放在扫描**之后**（它也可能读页）。
 */
static int tsio_scan(int mode, i64 t_lo, i64 t_hi, u32 frames, u32 passes)
{
    DB *db = NULL;
    DbInstance *inst;
    ScanCtx ctx;
    int rc = OK;
    u64 logical, physical, unique;
    double ms;
    u32 p;

    memset(&g_m, 0, sizeof(g_m));
    memset(&ctx, 0, sizeof(ctx));
    ctx.t_lo = t_lo;
    ctx.t_hi = t_hi;

    if (db_open(TSIO_DB, &db) != DB_OK) {
        printf("  开库失败\n");
        return -1;
    }
    inst = db->inst;

    pager_stats_reset(inst->pager);
    {
        double t0 = tsio_now_ms();

        for (p = 0; p < passes && rc == OK; p++) {
            ScanCtx one = ctx;      /* 每遍从零开始数，行数最后累加 */
            u64 reads = 0;

            if (mode == 0) {
                one.only_in_window = false;
                rc = ts_store_scan(inst->ts, scan_cb, &one);
            } else {
                one.only_in_window = false;
                rc = ts_prune_scan(inst->pager, inst->ts, t_lo, t_hi, true,
                                   scan_cb, &one, &reads);
            }
            ctx.n += one.n;         /* 两遍 ⇒ 行数翻倍（这是连扫的证据） */
            ctx.sum_tag += one.sum_tag;
        }
        ms = tsio_now_ms() - t0;
    }
    /* 立刻取三个 pager 计数器（此时还没有别的读页操作） */
    logical  = pager_logical_reads(inst->pager);
    physical = pager_physical_reads(inst->pager);
    unique   = pager_unique_pages(inst->pager);

    if (rc != OK) {
        printf("  扫描失败（rc=%d）\n", rc);
        db_close(db);
        return -1;
    }

    g_m.rows     = ctx.n;
    g_m.logical  = logical;
    g_m.physical = physical;
    g_m.unique   = unique;
    /* 「字节读」按页粒度算 —— pager 不直接记字节，但每次读盘就是一页 */
    g_m.bytes    = physical * (u64)PAGE_SIZE;
    g_m.ms       = ms;
    g_m.frames   = frames;
    g_m.passes   = passes;

    /*
     * 「访问了几个分区」：
     *   全扫 = 分区总数（一个不落）
     *   裁剪 = ts_prune_select 算出来的选区数（纯函数，不读页）
     */
    {
        u32 part_total = ts_store_part_count(inst->ts);

        if (mode == 0) {
            g_m.parts_scanned = part_total;
        } else {
            TsPruneWindow *wins = NULL;
            u32 nw = 0;
            u32 nsel = 0;

            if (ts_prune_windows_of_store(inst->ts, &wins, &nw) == OK) {
                TsPruneSet set;
                u32 i;

                if (ts_prune_set_init(&set) == OK) {
                    if (ts_prune_select(&set, wins, nw, t_lo, t_hi, true) == OK) {
                        for (i = 0; i < nw; i++) {
                            i64 s = 0, e = 0;
                            u32 st = 0;

                            if (!ts_prune_window_at(&set, i, &s, &e, &st)) {
                                continue;
                            }
                            if (ts_prune_allows_window(&set, s, e)) {
                                nsel++;
                            }
                        }
                    }
                    ts_prune_set_destroy(&set);
                }
                free(wins);
            }
            g_m.parts_scanned = nsel;
        }
    }

    db_close(db);
    return 0;
}

/* 全扫 + 手工过滤窗口 ⇒ 对拍标尺（单遍） */
static int tsio_expect_rows(i64 t_lo, i64 t_hi, u64 *n_out)
{
    DB *db = NULL;
    ScanCtx c;

    memset(&c, 0, sizeof(c));
    c.t_lo = t_lo;
    c.t_hi = t_hi;
    c.only_in_window = true;

    if (db_open(TSIO_DB, &db) != DB_OK) {
        return -1;
    }
    if (ts_store_scan(db->inst->ts, scan_cb, &c) != OK) {
        db_close(db);
        return -1;
    }
    db_close(db);

    *n_out = c.n;
    return 0;
}

static void tsio_print_row(const char *label, const TsIoMetrics *m, u64 part_total)
{
    printf("  %-20s rows=%-7llu 分区=%-2u/%-2llu 逻辑读=%-6llu 物理读=%-6llu "
           "唯一页=%-5llu 字节=%-8llu 耗时=%7.2f ms  帧池=%-5u 遍数=%u\n",
           label,
           (unsigned long long)m->rows,
           m->parts_scanned, (unsigned long long)part_total,
           (unsigned long long)m->logical,
           (unsigned long long)m->physical,
           (unsigned long long)m->unique,
           (unsigned long long)m->bytes,
           m->ms, m->frames, m->passes);
}

int main(int argc, char **argv)
{
    int days = (argc > 1) ? atoi(argv[1]) : TSIO_DEFAULT_DAYS;
    int rows = (argc > 2) ? atoi(argv[2]) : TSIO_DEFAULT_ROWS;
    int ok = 1;
    i64 base_ms;
    i64 day_ms = 24LL * 3600LL * 1000LL;
    i64 t_lo, t_hi;
    u64 part_total = 0;
    u64 n_expect = 0;
    TsIoMetrics full, pruned, big_twice, small_twice;

    if (days < 2) {
        days = 2;
    }
    if (rows < 10) {
        rows = 10;
    }
    if (rows >= (int)TS_SEAL_ROW_THRESHOLD) {
        printf("  ⚠️ 每分区行数 %d ≥ TS_SEAL_ROW_THRESHOLD(%u)：分区会自动封窗，"
               "之后再写入会被拒绝。已收到 %u。\n",
               rows, (unsigned)TS_SEAL_ROW_THRESHOLD,
               (unsigned)TS_SEAL_ROW_THRESHOLD - 1u);
        rows = (int)TS_SEAL_ROW_THRESHOLD - 1;
    }

    printf("=== 时序模态 · 统一 IO 口径（W-D 的 D-3.3 / A8 证据） ===\n");

    /* 时间基点：今天 0 点，再**向下对齐到分区边界**（见文件头约束 ③） */
    {
        time_t now = time(NULL);
        struct tm *lt = localtime(&now);
        struct tm lt0;

        memcpy(&lt0, lt, sizeof(lt0));
        lt0.tm_hour = lt0.tm_min = lt0.tm_sec = 0;
        lt0.tm_isdst = -1;
        base_ms = (i64)(mktime(&lt0) * 1000LL);
    }
    base_ms = (base_ms / day_ms) * day_ms;

    if (tsio_load(days, rows, base_ms) != 0) {
        tsio_rm_db();
        return 2;
    }

    /* 分区总数（打印用；单独一次开库，不混进任何测量窗口） */
    {
        DB *db = NULL;

        if (db_open(TSIO_DB, &db) == DB_OK) {
            part_total = ts_store_part_count(db->inst->ts);
            db_close(db);
        }
    }

    t_lo = base_ms;
    /* 右端收在第 1 个分区的最后一毫秒（见文件头约束 ③） */
    t_hi = base_ms + day_ms - 1LL;

    /* ---------- 第 1 轮：大帧池，各扫一遍 ---------- */
    tsio_set_frames(TSIO_BIG_FRAMES);
    printf("\n--- 第 1 轮：帧池 %u 帧（装得下） ---\n", TSIO_BIG_FRAMES);
    if (tsio_scan(0, 0, 0, TSIO_BIG_FRAMES, 1) != 0) {
        tsio_rm_db();
        return 2;
    }
    full = g_m;
    tsio_print_row("全扫（无谓词）", &full, part_total);

    if (tsio_scan(1, t_lo, t_hi, TSIO_BIG_FRAMES, 1) != 0) {
        tsio_rm_db();
        return 2;
    }
    pruned = g_m;
    tsio_print_row("裁剪（1 个分区）", &pruned, part_total);

    /* ---------- 第 2 轮：同一分区连扫两遍，两档帧池 ---------- */
    printf("\n--- 第 2 轮：同一分区连扫两遍（T4 靠它造出「重访」） ---\n");
    tsio_set_frames(TSIO_BIG_FRAMES);
    if (tsio_scan(1, t_lo, t_hi, TSIO_BIG_FRAMES, 2) != 0) {
        tsio_rm_db();
        return 2;
    }
    big_twice = g_m;
    tsio_print_row("裁剪×2（大帧池）", &big_twice, part_total);

    tsio_set_frames(TSIO_SMALL_FRAMES);
    if (tsio_scan(1, t_lo, t_hi, TSIO_SMALL_FRAMES, 2) != 0) {
        tsio_rm_db();
        return 2;
    }
    small_twice = g_m;
    tsio_print_row("裁剪×2（小帧池）", &small_twice, part_total);

    if (tsio_expect_rows(t_lo, t_hi, &n_expect) != 0) {
        tsio_rm_db();
        return 2;
    }

    /* ---------- 判据 ---------- */
    printf("\n--- 判据 ---\n");

    /* T1：裁剪真的少读 */
    {
        int parts_ok = (pruned.parts_scanned < full.parts_scanned);
        int reads_ok = (full.logical > 0)
                       && ((double)pruned.logical / (double)full.logical <= 0.60);

        if (parts_ok && reads_ok) {
            printf("  [PASS] T1 裁剪少读了：分区 %u→%u，逻辑读 %llu→%llu"
                   "（%.1f%%，要求 ≤ 60%%）\n",
                   full.parts_scanned, pruned.parts_scanned,
                   (unsigned long long)full.logical,
                   (unsigned long long)pruned.logical,
                   (full.logical > 0)
                       ? (100.0 * (double)pruned.logical / (double)full.logical) : 0.0);
        } else {
            printf("  [FAIL] T1 裁剪没生效：分区 %u→%u；逻辑读 %llu→%llu（要求 ≤ 60%%）\n",
                   full.parts_scanned, pruned.parts_scanned,
                   (unsigned long long)full.logical,
                   (unsigned long long)pruned.logical);
            ok = 0;
        }
    }

    /* T2：裁剪没少给（超集铁律） */
    if (pruned.rows == n_expect) {
        printf("  [PASS] T2 裁剪没少给：裁剪读到 %llu 条，"
               "全扫手工过滤窗口内也是 %llu 条\n",
               (unsigned long long)pruned.rows,
               (unsigned long long)n_expect);
    } else {
        printf("  [FAIL] T2 裁剪少给了：裁剪读到 %llu 条，应为 %llu 条"
               "（少给是**静默漏数据**）\n",
               (unsigned long long)pruned.rows,
               (unsigned long long)n_expect);
        ok = 0;
    }

    /* T3：逻辑读与帧池无关 */
    if (big_twice.logical == small_twice.logical) {
        printf("  [PASS] T3 逻辑读与帧池无关：两轮都是 %llu 次"
               "（连扫 2 遍 × 每遍约 %llu 次；缓存命中与否不影响它）\n",
               (unsigned long long)small_twice.logical,
               (unsigned long long)(small_twice.logical / 2u));
    } else {
        printf("  [FAIL] T3 逻辑读随帧池变了：%llu → %llu"
               "（它不该受帧池影响 ⇒ 计数器口径错了）\n",
               (unsigned long long)big_twice.logical,
               (unsigned long long)small_twice.logical);
        ok = 0;
    }

    /* T4：物理读与帧池有关（连扫两遍才测得出来） */
    if (big_twice.physical == 0 && small_twice.physical > small_twice.unique) {
        printf("  [PASS] T4 物理读与帧池有关：大帧池 %llu 次（唯一页 %llu）"
               "—— 开库时找 meta 页要扫一遍页空间，%u 帧装得下 ⇒ 整库被带进帧池，"
               "查询全程不回盘；小帧池 %llu 次 > 唯一页 %llu"
               "（第一遍末尾把开头挤掉 ⇒ 第二遍必须回盘）\n",
               (unsigned long long)big_twice.physical,
               (unsigned long long)big_twice.unique,
               TSIO_BIG_FRAMES,
               (unsigned long long)small_twice.physical,
               (unsigned long long)small_twice.unique);
    } else if (big_twice.unique == 0 && small_twice.unique == 0) {
        printf("  [FAIL] T4 测不出来：两轮唯一页都是 0 —— 数据页没落盘？"
               "（灌完必须 db_checkpoint）\n");
        ok = 0;
    } else {
        printf("  [FAIL] T4 物理读没体现出帧池差异：大帧池 %llu 次 / 唯一页 %llu；"
               "小帧池 %llu 次 / 唯一页 %llu"
               "（帧池 %u→%u，第二遍本应从小帧池多回盘）\n",
               (unsigned long long)big_twice.physical,
               (unsigned long long)big_twice.unique,
               (unsigned long long)small_twice.physical,
               (unsigned long long)small_twice.unique,
               TSIO_BIG_FRAMES, TSIO_SMALL_FRAMES);
        ok = 0;
    }

    printf("\nRESULT ts_io days=%d rows_per_day=%d parts=%llu "
           "full_rows=%llu full_logical=%llu full_physical=%llu full_unique=%llu "
           "pruned_rows=%llu pruned_logical=%llu "
           "big2_logical=%llu big2_physical=%llu big2_unique=%llu "
           "small2_logical=%llu small2_physical=%llu small2_unique=%llu "
           "verdict=%s\n",
           days, rows, (unsigned long long)part_total,
           (unsigned long long)full.rows,
           (unsigned long long)full.logical,
           (unsigned long long)full.physical,
           (unsigned long long)full.unique,
           (unsigned long long)pruned.rows,
           (unsigned long long)pruned.logical,
           (unsigned long long)big_twice.logical,
           (unsigned long long)big_twice.physical,
           (unsigned long long)big_twice.unique,
           (unsigned long long)small_twice.logical,
           (unsigned long long)small_twice.physical,
           (unsigned long long)small_twice.unique,
           ok ? "PASS" : "FAIL");

    tsio_rm_db();
    return ok ? 0 : 1;
}
