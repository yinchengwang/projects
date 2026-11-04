/*
 * test_ts_gorilla.c —— Gorilla XOR 浮点列编码的验收（v1.5 步骤 1 · T3）
 *
 * 契约见 src/modalities/ts/gorilla.h。
 *
 * 【为什么只比位模式，不比浮点相等】
 *
 * 解出来的 double 必须与原值的 **IEEE754 位模式完全相同**：
 *   - NaN 的 payload 要一样；
 *   - -0.0 与 0.0 必须分得开（用 `==` 比，它们是"相等"的，
 *     于是"符号位丢了"这种 bug 根本测不出来）。
 * 所以测试一律 memcpy 出 64 位来比。
 *
 * 【验收分组】
 *
 *   1. 边界与参数：n=0 / n=1 / 空指针 / 容量 0
 *   2. 随机长度随机值往返（0..1000）
 *   3. 恒定值：x 恒为 0 ⇒ 每点 1 bit
 *   4. 缓变序列（±1% 抖动）：最常见的真实场景
 *   5. 完全不同的相邻值：窗口的最坏情况
 *   6. 极值：±0.0 / DBL_MAX / DBL_MIN / subnormal / ±Inf / NaN（含 payload）
 *   7. fuzz：固定种子的 LCG，2000 轮随机数组
 *   8. 截断：短 1 字节必须 E_CORRUPT
 *   9. 容量不足：E_NOSPACE
 *  10. bound 足够性 + 多余字节 / 条数对不上 ⇒ E_CORRUPT
 *
 * 【压缩比为什么要打印】
 *
 * 步骤 1 的三件套是要给"时序列的压缩率"一个可解释的数。
 * 三个形态各打一个（字节/点）：
 *   - 恒定：理论下界 ≈ 1 bit/点（首值 64 bit 摊薄）
 *   - 缓变：XOR 后有效位还剩多少 —— 决定 Gorilla 对真实监控数据的效果
 *   - 随机：最坏情况（几乎每个 bit 都是新的）⇒ 应当 ≥ 8 字节/点
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/ts/gorilla.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---------------- 错误码名字（自带，不依赖 common/error.c） ---------------- */

/*
 * 为什么不直接用 err_name()：它在 common/error.c 里，
 * 而本单测的编译命令只带 gorilla.c + tsbits.c 两个源文件
 * （lead 给的构建命令就是这样）⇒ 多一个依赖就链不上。
 * 错误码就这几个，自己列一遍更省事。
 */
static const char *rc_name(int rc)
{
    switch (rc < 0 ? -rc : rc) {
    case OK:        return "OK";
    case E_NOSPACE: return "E_NOSPACE";
    case E_CORRUPT: return "E_CORRUPT";
    case E_IO:      return "E_IO";
    case E_NOTFOUND:return "E_NOTFOUND";
    case E_INVAL:   return "E_INVAL";
    case E_EOF:     return "E_EOF";
    case E_NOTSUP:  return "E_NOTSUP";
    default:        return "未知错误码";
    }
}

/* ---------------- 随机数：自己写 LCG，不用 rand() ---------------- */

/*
 * 固定种子的 LCG（Knuth 的参数）。
 * 为什么不用 rand()：rand() 的实现各平台不同 ⇒ 同一个种子在不同
 * 机器上跑出来的序列不一样 ⇒ 失败没法复现。自己写就是自己说了算。
 */
static u64 rng_state = 0x2545F4914F6CDD1DULL;

static u64 rng_next(void)
{
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return rng_state;
}

static void rng_seed(u64 s)
{
    rng_state = s;
}

/* 随机 64 位模式当成 double（会造出 NaN / Inf / subnormal —— 正好当压力测试） */
static double rng_double(void)
{
    u64 b = rng_next();
    double d;

    memcpy(&d, &b, sizeof(d));
    return d;
}

/* ---------------- 位模式比较 ---------------- */

static u64 bits_of(double d)
{
    u64 b;

    memcpy(&b, &d, sizeof(b));
    return b;
}

static int same_bits(double a, double b)
{
    return bits_of(a) == bits_of(b);
}

/* ---------------- 往返：编码 → 解码 → 逐位比 ---------------- */

/*
 * 返回 1 = 全部逐位相同；0 = 有出入。
 * bytes_out（可空）拿到编码后的字节数。
 */
static int roundtrip(const double *v, u32 n, int *bytes_out)
{
    u32    cap = gorilla_encoded_bound(n);
    u8    *buf = (u8 *)malloc(cap > 0u ? cap : 1u);
    double *out = (double *)malloc((n > 0u ? n : 1u) * sizeof(double));
    int    rc;
    int    ok = 1;
    u32    i;

    if (buf == NULL || out == NULL) {
        free(buf);
        free(out);
        printf("    FAIL 内存分配失败\n");
        failures++;
        return 0;
    }

    rc = gorilla_encode(v, n, buf, cap);
    if (rc < 0) {
        printf("    FAIL 编码返回 %d（%s）\n", rc, rc_name(rc));
        failures++;
        ok = 0;
        goto done;
    }
    if (bytes_out != NULL)
        *bytes_out = rc;

    rc = gorilla_decode(buf, (u32)rc, out, n);
    if (rc != (int)n) {
        printf("    FAIL 解码返回 %d，期望 %u（%s）\n", rc, n, rc_name(rc));
        failures++;
        ok = 0;
        goto done;
    }

    for (i = 0u; i < n; i++) {
        if (!same_bits(v[i], out[i])) {
            printf("    FAIL 第 %u 个值不一致：原 %016llx 解 %016llx\n", i,
                   (unsigned long long)bits_of(v[i]), (unsigned long long)bits_of(out[i]));
            failures++;
            ok = 0;
            break;
        }
    }

done:
    free(buf);
    free(out);
    return ok;
}

/* ---------------- 1：边界与参数 ---------------- */

static void t_edge(void)
{
    double one = 1.5;
    double back = 0.0;
    u8 buf[64];
    int bytes = 0;

    /* n = 0：什么都不写 */
    CHECK(gorilla_encoded_bound(0) == 0u);
    CHECK(gorilla_encode(NULL, 0u, NULL, 0u) == 0);
    CHECK(gorilla_decode(NULL, 0u, NULL, 0u) == 0);

    /* n = 1：只有首值的 64 位 ⇒ 8 字节 */
    CHECK(gorilla_encoded_bound(1) == 8u);
    CHECK(gorilla_encode(&one, 1u, buf, (u32)sizeof(buf)) == 8);
    CHECK(gorilla_decode(buf, 8u, &back, 1u) == 1);
    CHECK(same_bits(one, back));

    /* 参数非法 */
    CHECK(gorilla_encode(NULL, 1u, buf, (u32)sizeof(buf)) == -E_INVAL);
    CHECK(gorilla_encode(&one, 1u, NULL, 8u) == -E_INVAL);
    CHECK(gorilla_encode(&one, 1u, buf, 0u) == -E_NOSPACE);
    CHECK(gorilla_decode(NULL, 8u, &back, 1u) == -E_INVAL);
    CHECK(gorilla_decode(buf, 8u, NULL, 1u) == -E_INVAL);

    /* 往返（含字节数） */
    CHECK(roundtrip(&one, 1u, &bytes) == 1);
    CHECK(bytes == 8);
}

/* ---------------- 2：随机长度随机值往返 ---------------- */

static void t_random_lengths(void)
{
    static const u32 lens[] = { 0u, 1u, 2u, 3u, 7u, 8u, 9u, 15u, 16u, 31u, 32u,
                                63u, 64u, 65u, 100u, 255u, 256u, 999u, 1000u };
    double v[1000];
    u32    i;
    u32    k;

    rng_seed(0xC0FFEE123456789ULL);

    for (k = 0u; k < sizeof(lens) / sizeof(lens[0]); k++) {
        u32 n = lens[k];

        for (i = 0u; i < n; i++)
            v[i] = rng_double();

        if (!roundtrip(v, n, NULL))
            printf("    （长度 %u 那一组没通过）\n", n);
        else
            CHECK(1);
    }
}

/* ---------------- 3：恒定值（每点 1 bit） ---------------- */

static void t_constant(void)
{
    double v[1000];
    u32    i;
    int    bytes = 0;

    for (i = 0u; i < 1000u; i++)
        v[i] = 42.5;

    CHECK(roundtrip(v, 1000u, &bytes) == 1);

    /* 首值 64 位 + 999 个 1 位 = 1063 位 ⇒ 133 字节 */
    CHECK(bytes == (int)((64u + 999u * 1u + 7u) / 8u));
    printf("    恒定：%d 字节 / 1000 点 = %.3f 字节/点（%.2f bit/点）\n",
           bytes, (double)bytes / 1000.0, (double)bytes * 8.0 / 1000.0);
}

/* ---------------- 4：缓变序列（±1% 抖动） ---------------- */

static void t_smooth(void)
{
    double v[1000];
    double cur = 1000.0;
    u32    i;
    int    bytes = 0;

    rng_seed(0x1234567890ABCDEFULL);

    for (i = 0u; i < 1000u; i++) {
        /* ±1% 抖动：真实的监控读数大概长这样 */
        double j = ((double)(rng_next() % 20001u) / 10000.0) - 1.0;

        cur = cur * (1.0 + j * 0.01);
        v[i] = cur;
    }

    CHECK(roundtrip(v, 1000u, &bytes) == 1);
    printf("    缓变：%d 字节 / 1000 点 = %.3f 字节/点（%.2f bit/点）\n",
           bytes, (double)bytes / 1000.0, (double)bytes * 8.0 / 1000.0);
}

/* ---------------- 5：相邻值完全不同（窗口最坏情况） ---------------- */

static void t_worst_window(void)
{
    double v[1000];
    u32    i;
    int    bytes = 0;

    /* 两个差别极大的值交替 ⇒ 每个点都要开新窗口 */
    for (i = 0u; i < 1000u; i++)
        v[i] = (i % 2u == 0u) ? 0.0 : 1e300;

    CHECK(roundtrip(v, 1000u, &bytes) == 1);
    printf("    完全不同：%d 字节 / 1000 点 = %.3f 字节/点（%.2f bit/点）\n",
           bytes, (double)bytes / 1000.0, (double)bytes * 8.0 / 1000.0);
}

/* ---------------- 5b：随机值（真正的最坏情况，也是要报的第三组数） ---------------- */

static void t_random_shape(void)
{
    double v[1000];
    u32    i;
    int    bytes = 0;

    rng_seed(0x13579BDF2468ACE0ULL);
    for (i = 0u; i < 1000u; i++)
        v[i] = rng_double();

    CHECK(roundtrip(v, 1000u, &bytes) == 1);
    printf("    随机：%d 字节 / 1000 点 = %.3f 字节/点（%.2f bit/点）\n",
           bytes, (double)bytes / 1000.0, (double)bytes * 8.0 / 1000.0);
}

/* ---------------- 5c：小步长（Gorilla 最擅长的场景） ---------------- */

static void t_small_step(void)
{
    double v[1000];
    u32    i;
    int    bytes = 0;

    /* 每次只加 0.25 ⇒ XOR 只落在低几位 ⇒ 窗口复用率极高 */
    for (i = 0u; i < 1000u; i++)
        v[i] = 1000.0 + (double)i * 0.25;

    CHECK(roundtrip(v, 1000u, &bytes) == 1);
    printf("    小步长：%d 字节 / 1000 点 = %.3f 字节/点（%.2f bit/点）\n",
           bytes, (double)bytes / 1000.0, (double)bytes * 8.0 / 1000.0);
}

/* ---------------- 6：极值 ---------------- */

static void t_extremes(void)
{
    double v[16];
    u64    nan_payload;
    double nan_d;
    int    bytes = 0;
    u32    i;

    nan_payload = 0x7FF8000000000001ULL;   /* 带 payload 的 NaN */
    memcpy(&nan_d, &nan_payload, sizeof(nan_d));

    v[0] = 0.0;
    v[1] = -0.0;               /* 与 v[0] 只差符号位 */
    v[2] = DBL_MAX;
    v[3] = -DBL_MAX;
    v[4] = DBL_MIN;            /* 最小正规数 */
    v[5] = -DBL_MIN;
    v[6] = DBL_TRUE_MIN;       /* 最小次正规数 */
    v[7] = -DBL_TRUE_MIN;
    v[8] = INFINITY;
    v[9] = -INFINITY;
    v[10] = nan_d;
    v[11] = -nan_d;
    v[12] = 1.0;
    v[13] = -1.0;
    v[14] = 1e-300;
    v[15] = 5e-324;

    CHECK(roundtrip(v, 16u, &bytes) == 1);

    /* 单独验：-0.0 与 0.0 必须分得开（用 == 比是"相等"的，测不出来） */
    {
        double out[16];
        u8 buf[512];

        CHECK(gorilla_encode(v, 16u, buf, (u32)sizeof(buf)) > 0);
        CHECK(gorilla_decode(buf, (u32)bytes, out, 16u) == 16);
        CHECK(bits_of(out[0]) != bits_of(out[1]));
        CHECK(bits_of(out[0]) == 0x0000000000000000ULL);
        CHECK(bits_of(out[1]) == 0x8000000000000000ULL);
    }

    /* NaN 的 payload 必须原样保留 */
    {
        double out[2];
        u8 buf[64];

        CHECK(gorilla_encode(&v[10], 1u, buf, (u32)sizeof(buf)) == 8);
        CHECK(gorilla_decode(buf, 8u, out, 1u) == 1);
        CHECK(bits_of(out[0]) == 0x7FF8000000000001ULL);
    }

    /* 逐个都过一遍（单值也要能往返） */
    for (i = 0u; i < 16u; i++)
        CHECK(roundtrip(&v[i], 1u, NULL) == 1);
}

/* ---------------- 7：fuzz（固定种子，2000 轮） ---------------- */

static void t_fuzz(void)
{
    double v[64];
    int    round;
    int    bad = 0;

    rng_seed(0xDEADBEEFCAFEBABEULL);

    for (round = 0; round < 2000; round++) {
        u32 n = (u32)(rng_next() % 65u);   /* 0..64 */
        u32 i;

        for (i = 0u; i < n; i++)
            v[i] = rng_double();

        if (!roundtrip(v, n, NULL))
            bad++;
    }

    CHECK(bad == 0);
    printf("    fuzz：2000 轮随机数组，失败 %d 轮\n", bad);
}

/* ---------------- 8：截断必须报 E_CORRUPT ---------------- */

static void t_truncated(void)
{
    double v[100];
    u8    *buf;
    int    bytes = 0;
    u32    i;
    int    rc;

    rng_seed(0x5EED0000BEEF0001ULL);
    for (i = 0u; i < 100u; i++)
        v[i] = rng_double();

    CHECK(roundtrip(v, 100u, &bytes) == 1);
    CHECK(bytes > 1);

    buf = (u8 *)malloc((size_t)bytes);
    CHECK(buf != NULL);
    if (buf == NULL)
        return;

    /* 短 1 字节 ⇒ 必须硬失败，不许"读不出来就填 0" */
    {
        u8 *enc = (u8 *)malloc((size_t)bytes);

        CHECK(enc != NULL);
        if (enc != NULL) {
            int n = gorilla_encode(v, 100u, enc, (u32)bytes);

            CHECK(n == bytes);
            memcpy(buf, enc, (size_t)bytes);

            {
                double out[100];

                rc = gorilla_decode(buf, (u32)(bytes - 1), out, 100u);
                CHECK(rc == -E_CORRUPT);

                rc = gorilla_decode(buf, (u32)(bytes / 2), out, 100u);
                CHECK(rc == -E_CORRUPT);

                rc = gorilla_decode(buf, 0u, out, 100u);
                CHECK(rc == -E_CORRUPT);
            }
            free(enc);
        }
    }

    free(buf);
}

/* ---------------- 9：容量不足 ⇒ E_NOSPACE ---------------- */

static void t_nospace(void)
{
    double v[100];
    u8     buf[4096];
    u32    i;
    int    bytes = 0;

    rng_seed(0x0FF1CE0000000001ULL);
    for (i = 0u; i < 100u; i++)
        v[i] = rng_double();

    CHECK(gorilla_encode(v, 100u, buf, (u32)sizeof(buf)) > 0);
    bytes = gorilla_encode(v, 100u, buf, (u32)sizeof(buf));

    /* 差一个字节就装不下 ⇒ E_NOSPACE */
    CHECK(gorilla_encode(v, 100u, buf, (u32)(bytes - 1)) == -E_NOSPACE);
    /* 一字节都没有 ⇒ E_NOSPACE */
    CHECK(gorilla_encode(v, 100u, buf, 0u) == -E_NOSPACE);
    /* bound 给的容量必须够用 */
    CHECK(gorilla_encode(v, 100u, buf, gorilla_encoded_bound(100u)) == bytes);
}

/* ---------------- 10：bound 足够性 / 多余数据 / 条数对不上 ---------------- */

static void t_bound_and_mismatch(void)
{
    double v[257];
    u8    *buf;
    u32    i;
    int    bytes = 0;

    rng_seed(0xABCD0000DCBA0001ULL);
    for (i = 0u; i < 257u; i++)
        v[i] = rng_double();

    buf = (u8 *)malloc((size_t)gorilla_encoded_bound(257u));
    CHECK(buf != NULL);
    if (buf == NULL)
        return;

    /* bound 必须装得下，且装得下时字节数 ≤ bound */
    CHECK(gorilla_encode(v, 257u, buf, gorilla_encoded_bound(257u)) > 0);
    bytes = gorilla_encode(v, 257u, buf, gorilla_encoded_bound(257u));
    CHECK((u32)bytes <= gorilla_encoded_bound(257u));

    /* 多给一个字节 ⇒ 数据比声明的多 ⇒ E_CORRUPT */
    {
        double out[257];

        CHECK(gorilla_decode(buf, (u32)bytes + 1u, out, 257u) == -E_CORRUPT);
    }

    /* 声明的条数比实际的少 ⇒ 读不够 ⇒ E_CORRUPT */
    {
        double out[258];

        CHECK(gorilla_decode(buf, (u32)bytes, out, 258u) == -E_CORRUPT);
    }

    free(buf);
}

/* ---------------- 主流程 ---------------- */

int main(void)
{
    printf("test_ts_gorilla ...\n");

    printf("  1. 边界与参数\n");
    t_edge();
    printf("  2. 随机长度随机值往返\n");
    t_random_lengths();
    printf("  3. 恒定值（每点 1 bit）\n");
    t_constant();
    printf("  4. 缓变序列（±1%% 抖动）\n");
    t_smooth();
    printf("  5. 相邻值完全不同（窗口最坏）\n");
    t_worst_window();
    printf("  5b. 随机值（最坏情况 / 要报的第三组压缩比）\n");
    t_random_shape();
    printf("  5c. 小步长（Gorilla 最擅长的场景）\n");
    t_small_step();
    printf("  6. 极值（含 NaN payload / ±0.0）\n");
    t_extremes();
    printf("  7. fuzz（2000 轮）\n");
    t_fuzz();
    printf("  8. 截断必须 E_CORRUPT\n");
    t_truncated();
    printf("  9. 容量不足必须 E_NOSPACE\n");
    t_nospace();
    printf("  10. bound 足够性 / 条数对不上\n");
    t_bound_and_mismatch();

    printf("  10 组验收%s\n", (failures == 0) ? "全部通过" : "有失败");

    if (failures == 0) {
        printf("  [PASS]\n");
        return 0;
    }
    printf("  [FAIL] %d 处断言失败\n", failures);
    return 1;
}
