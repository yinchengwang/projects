/*
 * ts_bench.c —— 时序模态的压缩比基准（v1.5 步骤 6 · A3）
 *
 * 【要回答的问题】
 *
 * 时序模态的三种列编码，在三种数据形态下各能压到多少：
 *
 *   列        编码            数据性质
 *   --------  --------------  ------------------------------------------
 *   时间列    delta-of-delta  i64 毫秒时间戳，等间隔
 *   整数列    delta + RLE     i64 标签 id（设备/指标编号），重复为主
 *   浮点列    Gorilla XOR     double 读数
 *
 *   形态
 *   ------  --------------------------------------------------
 *   恒定    所有点完全一样
 *   缓变    相邻值很接近（±1% 抖动、等间隔、重复为主）
 *   随机    相邻值毫无关系（白噪声）
 *
 * 判据：**恒定形态的压缩比明显优于随机形态**。
 *
 * ==================== 坑 M6-35：压缩比可能小于 1 ====================
 *
 * 位级 varint 编码在"数据毫无相关性"时，编码后**比原始还大**
 * （本基准实测：随机形态下 RLE 10.24 字节/点，而原始是 8 字节/点；
 *  Gorilla 7.25 字节/点，压缩比也只有 1.10）。
 * 原因很直白：每个点都要付"控制位 + varint / 窗口描述"的开销，
 * 而原始就是定长的 8 字节，一点冗余都没有。
 *
 * 所以：**判据里绝不能写"压缩比 ≤ 1"或"编码后一定更小"** ——
 * 那是把格式的性质当成了 bug。能写的判据只有"恒定形态明显优于随机形态"。
 *
 * ==================== 坑 M6-36：三列的数据类型要对得上 ====================
 *
 * 时间列与整数列是 **i64**，浮点列才是 **double**。
 * 拿 double 去喂 dod/rle（或者反过来）类型就不对了，
 * 而且是**静默**的错 —— 不会报错，只是压缩比变得毫无意义。
 * 所以本基准按列分开造数据、分开造数组，不共用一份缓冲。
 *
 * ==================== 坑 M6-37：先验往返，再报数字 ====================
 *
 * 压缩比必须建立在**正确的数据**上：编码 → 解码 → 逐位比对，
 * 不一致就直接判 FAIL，不许"数字先记下来，回头再说"。
 *
 * ==================== 坑 M6-38：造数据的精度不能偷懒 ====================
 *
 * 压缩比是"编码器 vs 数据"的联合结果，数据造得假，结论就是假的。
 *
 * 典型翻车：用 `lcg_next() >> 8`（24 位）造浮点读数。
 * 24 位精度 ⇒ 抖动量的有效位只有 24 位 ⇒ XOR 的尾零常常很整齐
 * ⇒ Gorilla 的窗口能一直复用 ⇒ "随机"形态竟然压得比"缓变"还小，
 * 结论完全反了。
 *
 * 所以浮点列一律用**满 53 位尾数**的随机数（真实读数就是这个精度），
 * 三种形态之间只差"相邻值相不相关"，不差精度。
 *
 * ==================== 其它约定 ====================
 *
 * - 随机数：自己写 LCG，不用 rand()（rand() 跨平台不可复现）。
 * - 默认 10 万点，可用命令行参数改：ts_bench [点数]
 * - 退出码：0 = 达标；1 = 判据不达标；2 = 基准自身出错。
 * - ⚠️ 三个编码的错误码是**负数**（坑 M6-19），判错要用 `rc < 0`。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/ts/dod.h"
#include "modalities/ts/gorilla.h"
#include "modalities/ts/rle.h"

/* 原始大小口径：i64 与 double 都是 8 字节/点 */
#define TS_RAW_PER_POINT 8u

/* 时间列的起点（2023-11-14 附近，毫秒） */
#define TS_BASE_MS 1700000000000LL

/* ---------------- 确定性伪随机（LCG） ---------------- */

/*
 * 为什么不用 rand()：rand() 的实现各平台不一致 ⇒ 同一个种子在不同
 * 机器上跑出来的数据集不同 ⇒ 基准的结论没法复现。
 */
static u64 g_state = 1u;

static void lcg_seed(u64 s)
{
    g_state = s ? s : 1u;
}

static u64 lcg_next64(void)
{
    g_state = g_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return g_state;
}

static u32 lcg_next(void)
{
    return (u32)(lcg_next64() >> 32);
}

/*
 * [0,1) 的浮点数，**满 53 位尾数**。
 *
 * 为什么不用 24 位（坑 M6-38）：24 位随机数只能给出 2^24 个不同的值，
 * 于是"随机"形态的浮点读数其实只有 24 位有效精度 ⇒ 相邻两点的 XOR 常常
 * 落在整齐的二进制边界上 ⇒ Gorilla 测出来的压缩比会**偏好看**。
 * 一旦换成真实的 53 位尾数，"随机"形态就真的没有冗余了，
 * 测出来的数字才是真实传感器数据的样子。
 */
static double lcg_unit(void)
{
    return (double)(lcg_next64() >> 11) * (1.0 / 9007199254740992.0);
}

/* ---------------- 数据形态 ---------------- */

typedef enum {
    SHAPE_CONST = 0,   /* 恒定：所有点完全一样 */
    SHAPE_SMOOTH = 1,  /* 缓变：相邻值很接近 */
    SHAPE_NOISE = 2,   /* 随机：相邻值毫无关系 */
    SHAPE_COUNT = 3
} Shape;

static const char *shape_name(Shape s)
{
    switch (s) {
    case SHAPE_CONST:
        return "恒定（所有点完全一样）";
    case SHAPE_SMOOTH:
        return "缓变（相邻值很接近：等间隔 / 重复为主 / ±1% 抖动）";
    case SHAPE_NOISE:
        return "随机（相邻值毫无关系：白噪声）";
    default:
        return "?";
    }
}

/* 每种形态一句解释（数字取自实测，机制见各自的说明） */
static const char *shape_why(Shape s)
{
    switch (s) {
    case SHAPE_CONST:
        return "三列都没有任何变化 ⇒ dod 的间隔差全是 0、Gorilla 的 XOR 全是 0 ⇒ "
               "每点只付 1 bit（0.125 字节/点），整数列更是只剩一个游程（整列 4 字节）—— "
               "这就是三个编码各自能到的最好成绩。";
    case SHAPE_SMOOTH:
        return "相邻值相关 ⇒ 时间列的间隔差多半是 0（0.31 字节/点）、整数列只有少数几个游程"
               "（1.04 字节/点）；浮点列 Gorilla 只压到 6.38 字节/点（压缩比 1.26）—— "
               "因为 Gorilla 每点的成本是 2 bit 控制位 + 窗口宽度，而 ±1% 抖动下相邻读数的 XOR "
               "仍会改到尾数高位（前导零只剩约 17 位）⇒ 窗口宽约 46 bit，省不掉。";
    case SHAPE_NOISE:
        return "相邻值毫无关系 ⇒ 冗余被吃干净：浮点列的 XOR 改到更高位（前导零只剩约 12 位）"
               "⇒ 窗口更宽，7.25 字节/点；整数列 RLE 每点都要付\"游程头 + 值\"，10.24 字节/点，"
               "比原始的 8 字节还大（压缩比 0.78）—— 这是位级 varint 的性质，不是 bug（坑 M6-35）。";
    default:
        return "";
    }
}

/* ---------------- 数据集 ---------------- */

typedef struct {
    i64    *ts;     /* 时间列（毫秒） */
    i64    *tag;    /* 整数列（tag id） */
    double *val;    /* 浮点列（读数） */
    u32     n;
} TsData;

static void ts_data_free(TsData *d)
{
    free(d->ts);
    free(d->tag);
    free(d->val);
    memset(d, 0, sizeof(*d));
}

/*
 * 按形态造三列数据。
 *
 * 注意（坑 M6-36）：三列**各自造**，类型与数据性质都对得上：
 *   时间列 ⇒ 等间隔（缓变）/ 间隔随机（随机）
 *   整数列 ⇒ 重复为主（缓变）/ 每次都换（随机）
 *   浮点列 ⇒ ±1% 抖动（缓变）/ ±50% 白噪声（随机）
 */
static int ts_gen(Shape shape, u32 n, TsData *d)
{
    u32 i;

    memset(d, 0, sizeof(*d));
    d->n = n;
    d->ts = (i64 *)calloc(n > 0u ? n : 1u, sizeof(i64));
    d->tag = (i64 *)calloc(n > 0u ? n : 1u, sizeof(i64));
    d->val = (double *)calloc(n > 0u ? n : 1u, sizeof(double));
    if (d->ts == NULL || d->tag == NULL || d->val == NULL) {
        ts_data_free(d);
        return -1;
    }

    lcg_seed(0xA11CEu + (u64)shape * 7919u);

    if (shape == SHAPE_CONST) {
        for (i = 0u; i < n; i++) {
            d->ts[i] = TS_BASE_MS;
            d->tag[i] = 7;
            d->val[i] = 42.5;
        }
    } else if (shape == SHAPE_SMOOTH) {
        i64    t = TS_BASE_MS;
        i64    tag = 7;

        for (i = 0u; i < n; i++) {
            /* 时间列：等间隔 1000 ms，其中 10% 的点 ±1 ms 抖动 */
            t += 1000;
            if (lcg_next() % 10u == 0u) {
                t += (lcg_next() % 2u == 0u) ? 1 : -1;
            }
            d->ts[i] = t;

            /* 整数列：90% 保持同一个 tag，10% 换一个（设备在换指标） */
            if (lcg_next() % 10u == 0u) {
                tag = (i64)(lcg_next64() & 0x7FFFFFFFFFFFFFFFULL);
            }
            d->tag[i] = tag;

            /* 浮点列：基线固定在 1000，±1% 的抖动（真实监控读数的样子） */
            d->val[i] = 1000.0 * (1.0 + (lcg_unit() - 0.5) * 0.02);
        }
    } else {
        i64 t = TS_BASE_MS;

        for (i = 0u; i < n; i++) {
            /* 时间列：间隔随机（1..2000 ms） */
            t += 1 + (i64)(lcg_next() % 2000u);
            d->ts[i] = t;

            /* 整数列：每次都换一个 tag（id 空间与上面两种形态一致） */
            d->tag[i] = (i64)(lcg_next64() & 0x7FFFFFFFFFFFFFFFULL);

            /* 浮点列：±50% 的白噪声 */
            d->val[i] = 1000.0 * (1.0 + (lcg_unit() - 0.5));
        }
    }

    return 0;
}

/* ---------------- 一列的测量结果 ---------------- */

typedef struct {
    u64    raw_bytes;      /* 原始大小 = 8 字节 × 点数 */
    u64    enc_bytes;      /* 编码后大小 */
    double bytes_per_point;/* 编码后字节 / 点数 */
    double ratio;          /* 压缩比 = 原始 / 编码后（**可能 < 1**） */
    int    ok;             /* 往返是否逐位全等 */
} ColStat;

static void col_stat_set(ColStat *st, u32 n, u64 enc)
{
    st->raw_bytes = (u64)n * (u64)TS_RAW_PER_POINT;
    st->enc_bytes = enc;
    st->bytes_per_point = (n > 0u) ? ((double)enc / (double)n) : 0.0;
    st->ratio = (enc > 0u) ? ((double)st->raw_bytes / (double)enc) : 0.0;
}

/* 时间列：delta-of-delta */
static int bench_dod(const i64 *src, u32 n, ColStat *st, i64 *out)
{
    u32 cap = dod_encoded_bound(n);
    u8 *buf = (u8 *)malloc(cap > 0u ? cap : 1u);
    int rc;
    u32 i;

    st->ok = 0;
    if (buf == NULL) {
        return -1;
    }

    rc = dod_encode(src, n, buf, cap);
    if (rc < 0) {
        printf("    错误：dod_encode 失败（%d）\n", rc);
        free(buf);
        return -1;
    }
    col_stat_set(st, n, (u64)rc);

    rc = dod_decode(buf, (u32)rc, out, n);
    if (rc != (int)n) {
        printf("    错误：dod_decode 返回 %d，期望 %u\n", rc, n);
        free(buf);
        return -1;
    }
    for (i = 0u; i < n; i++) {
        if (out[i] != src[i]) {
            printf("    错误：dod 往返第 %u 个时间戳不一致：%lld vs %lld\n",
                   i, (long long)src[i], (long long)out[i]);
            free(buf);
            return -1;
        }
    }

    st->ok = 1;
    free(buf);
    return 0;
}

/* 整数列：delta + RLE */
static int bench_rle(const i64 *src, u32 n, ColStat *st, i64 *out)
{
    u32 cap = rle_encoded_bound(n);
    u8 *buf = (u8 *)malloc(cap > 0u ? cap : 1u);
    int rc;
    u32 i;

    st->ok = 0;
    if (buf == NULL) {
        return -1;
    }

    rc = rle_encode(src, n, buf, cap);
    if (rc < 0) {
        printf("    错误：rle_encode 失败（%d）\n", rc);
        free(buf);
        return -1;
    }
    col_stat_set(st, n, (u64)rc);

    rc = rle_decode(buf, (u32)rc, out, n);
    if (rc != (int)n) {
        printf("    错误：rle_decode 返回 %d，期望 %u\n", rc, n);
        free(buf);
        return -1;
    }
    for (i = 0u; i < n; i++) {
        if (out[i] != src[i]) {
            printf("    错误：rle 往返第 %u 个整数不一致：%lld vs %lld\n",
                   i, (long long)src[i], (long long)out[i]);
            free(buf);
            return -1;
        }
    }

    st->ok = 1;
    free(buf);
    return 0;
}

/* 浮点列：Gorilla XOR（比**位模式**，不比浮点相等） */
static int bench_gorilla(const double *src, u32 n, ColStat *st, double *out)
{
    u32 cap = gorilla_encoded_bound(n);
    u8 *buf = (u8 *)malloc(cap > 0u ? cap : 1u);
    int rc;
    u32 i;

    st->ok = 0;
    if (buf == NULL) {
        return -1;
    }

    rc = gorilla_encode(src, n, buf, cap);
    if (rc < 0) {
        printf("    错误：gorilla_encode 失败（%d）\n", rc);
        free(buf);
        return -1;
    }
    col_stat_set(st, n, (u64)rc);

    rc = gorilla_decode(buf, (u32)rc, out, n);
    if (rc != (int)n) {
        printf("    错误：gorilla_decode 返回 %d，期望 %u\n", rc, n);
        free(buf);
        return -1;
    }
    for (i = 0u; i < n; i++) {
        u64 a;
        u64 b;

        memcpy(&a, &src[i], sizeof(a));
        memcpy(&b, &out[i], sizeof(b));
        if (a != b) {
            printf("    错误：gorilla 往返第 %u 个浮点位模式不一致：%016llx vs %016llx\n",
                   i, (unsigned long long)a, (unsigned long long)b);
            free(buf);
            return -1;
        }
    }

    st->ok = 1;
    free(buf);
    return 0;
}

/* ---------------- 参考：基线漂移的浮点列（不参与判据） ---------------- */

/*
 * 【为什么要有这个参考数字】
 *
 * Gorilla 的"窗口"（前导零 + 宽度）只在**前导零变少**时才重开，
 * 而重开时新窗口往往更宽，且宽窗口会一直沿用。
 *
 * 基线固定在 1000 时，±1% 抖动不会跨过 2 的幂 ⇒ 指数位不变 ⇒ 窗口很稳；
 * 基线随时间漂移（随机游走）时，值迟早跨过 1024 ⇒ 跨过那一点的 XOR
 * 高位全变 ⇒ 前导零骤减 ⇒ 窗口被撑成宽的 ⇒ 压缩比掉一截。
 *
 * 这个对照**不参与判据**，只作为"用的时候要知道"的背景。
 */
static int bench_gorilla_drift(u32 n, ColStat *st)
{
    double *v;
    double *out;
    u32     i;
    int     rc;

    v = (double *)calloc(n > 0u ? n : 1u, sizeof(double));
    out = (double *)calloc(n > 0u ? n : 1u, sizeof(double));
    if (v == NULL || out == NULL) {
        free(v);
        free(out);
        return -1;
    }

    lcg_seed(0xA11CEu + (u64)SHAPE_SMOOTH * 7919u);
    {
        double cur = 1000.0;

        for (i = 0u; i < n; i++) {
            cur *= 1.0 + (lcg_unit() - 0.5) * 0.02;
            v[i] = cur;
        }
    }

    rc = bench_gorilla(v, n, st, out);
    free(v);
    free(out);
    return rc;
}

/* ---------------- 跑一组形态 ---------------- */

typedef struct {
    ColStat ts;       /* delta-of-delta */
    ColStat tag;      /* delta + RLE */
    ColStat val;      /* Gorilla */
} ShapeStat;

static void print_shape(Shape shape, u32 n, const ShapeStat *st)
{
    printf("\n== %s ==\n", shape_name(shape));
    printf("    %-16s %12s %14s %10s %12s\n",
           "列", "原始(字节)", "编码后(字节)", "字节/点", "压缩比");
    printf("    %-16s %12llu %14llu %10.3f %12.3f\n",
           "时间(dod)", (unsigned long long)st->ts.raw_bytes,
           (unsigned long long)st->ts.enc_bytes,
           st->ts.bytes_per_point, st->ts.ratio);
    printf("    %-16s %12llu %14llu %10.3f %12.3f\n",
           "整数(rle)", (unsigned long long)st->tag.raw_bytes,
           (unsigned long long)st->tag.enc_bytes,
           st->tag.bytes_per_point, st->tag.ratio);
    printf("    %-16s %12llu %14llu %10.3f %12.3f\n",
           "浮点(gorilla)", (unsigned long long)st->val.raw_bytes,
           (unsigned long long)st->val.enc_bytes,
           st->val.bytes_per_point, st->val.ratio);
    printf("    解释：%s\n", shape_why(shape));
    printf("    往返全等：时间 %s / 整数 %s / 浮点 %s（点数 %u）\n",
           st->ts.ok ? "OK" : "不一致", st->tag.ok ? "OK" : "不一致",
           st->val.ok ? "OK" : "不一致", n);
}

/* ---------------- 主流程 ---------------- */

int main(int argc, char **argv)
{
    u32 n = 100000u;
    ShapeStat st[SHAPE_COUNT];
    int shape;
    int ok = 1;
    int rc = 0;

    if (argc > 1) {
        n = (u32)strtoul(argv[1], NULL, 10);
    }
    if (n == 0u) {
        printf("用法：ts_bench [点数]\n");
        return 2;
    }

    memset(st, 0, sizeof(st));

    printf("ts_bench —— 时序模态的压缩比基准（A3）\n");
    printf("  点数 = %u\n", n);
    printf("  原始大小口径：8 字节/点（i64 与 double 同宽）\n");
    printf("  压缩比 = 原始 / 编码后（**可能 < 1**：位级 varint 的性质，坑 M6-35）\n");

    for (shape = 0; shape < SHAPE_COUNT; shape++) {
        TsData d;
        i64    *out_ts;
        i64    *out_tag;
        double *out_val;

        if (ts_gen((Shape)shape, n, &d) != 0) {
            printf("  错误：生成数据失败（内存不足）\n");
            return 2;
        }

        out_ts = (i64 *)calloc(n > 0u ? n : 1u, sizeof(i64));
        out_tag = (i64 *)calloc(n > 0u ? n : 1u, sizeof(i64));
        out_val = (double *)calloc(n > 0u ? n : 1u, sizeof(double));
        if (out_ts == NULL || out_tag == NULL || out_val == NULL) {
            free(out_ts);
            free(out_tag);
            free(out_val);
            ts_data_free(&d);
            printf("  错误：内存不足\n");
            return 2;
        }

        if (bench_dod(d.ts, n, &st[shape].ts, out_ts) != 0 ||
            bench_rle(d.tag, n, &st[shape].tag, out_tag) != 0 ||
            bench_gorilla(d.val, n, &st[shape].val, out_val) != 0) {
            /* 往返不一致 ⇒ 数字没有意义（坑 M6-37） */
            free(out_ts);
            free(out_tag);
            free(out_val);
            ts_data_free(&d);
            printf("\n=== 结论：往返校验失败，基准数据不可信 ===\n");
            printf("RESULT ts verdict=FAIL\n");
            return 1;
        }

        print_shape((Shape)shape, n, &st[shape]);

        /* 缓变形态的补充对照：基线漂移会毁掉 Gorilla 的窗口（不参与判据） */
        if (shape == SHAPE_SMOOTH) {
            ColStat drift;

            if (bench_gorilla_drift(n, &drift) == 0) {
                printf("    参考（不参与判据）：浮点列若基线随时间漂移 ⇒ %.3f 字节/点"
                       "（压缩比 %.3f），比基线固定时差 ——\n"
                       "            原因是基线漂移时值会跨过 2 的幂，跨过那一点的 XOR 高位全变 ⇒ "
                       "前导零骤减 ⇒ 窗口被撑成宽的，而宽窗口会一直沿用。\n",
                       drift.bytes_per_point, drift.ratio);
            }
        }

        free(out_ts);
        free(out_tag);
        free(out_val);
        ts_data_free(&d);
    }

    printf("\n== 判据 ==\n");

    /* 判据 1：往返全等（上面每一组都验过，不一致早就退出了） */
    printf("  判据 1：往返逐位全等（3 形态 × 3 列）⇒ [PASS]\n");

    /* 判据 2：每一列，恒定形态的压缩比明显优于随机形态（≥ 2 倍） */
    {
        const char *names[3] = { "时间(dod)", "整数(rle)", "浮点(gorilla)" };
        const ColStat *cols_const[3] = { &st[SHAPE_CONST].ts, &st[SHAPE_CONST].tag,
                                         &st[SHAPE_CONST].val };
        const ColStat *cols_noise[3] = { &st[SHAPE_NOISE].ts, &st[SHAPE_NOISE].tag,
                                         &st[SHAPE_NOISE].val };
        int c;

        for (c = 0; c < 3; c++) {
            double rc_const = cols_const[c]->ratio;
            double rc_noise = cols_noise[c]->ratio;
            int pass = (rc_const >= 2.0 * rc_noise);

            printf("  判据 2：%s 恒定压缩比 %.3f ≥ 2 × 随机压缩比 %.3f ⇒ [%s]\n",
                   names[c], rc_const, rc_noise, pass ? "PASS" : "FAIL");
            if (!pass) {
                ok = 0;
            }
        }
    }

    /* 判据 3：Gorilla 对缓变优于随机（A3 的那句话本身） */
    {
        double rc_smooth = st[SHAPE_SMOOTH].val.ratio;
        double rc_noise = st[SHAPE_NOISE].val.ratio;
        int pass = (rc_smooth > rc_noise);

        printf("  判据 3：浮点列 缓变压缩比 %.3f > 随机压缩比 %.3f ⇒ [%s]"
               "（注：浮点列的优势不大 —— Gorilla 的窗口宽度由\"XOR 的最高变化位\"决定，"
               "±1%% 抖动与白噪声的差别主要体现在窗口宽约 46 vs 51 bit）\n",
               rc_smooth, rc_noise, pass ? "PASS" : "FAIL");
        if (!pass) {
            ok = 0;
        }
    }

    printf("\n");
    if (ok) {
        printf("=== 结论：A3 达标（压缩比可解释：恒定 ≫ 缓变 > 随机；随机形态下压缩比 < 1 是格式性质）===\n");
        printf("RESULT ts verdict=PASS\n");
    } else {
        printf("=== 结论：未达标 ===\n");
        printf("RESULT ts verdict=FAIL\n");
        rc = 1;
    }

    return rc;
}
