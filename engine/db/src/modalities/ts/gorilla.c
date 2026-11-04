/*
 * gorilla.c —— 浮点列的 Gorilla XOR 编码（v1.5 步骤 1 · 压缩三件套之三）
 *
 * 契约见 gorilla.h，位流基元见 tsbits.h（两个文件都不是我写的，格式照它们）。
 *
 * 【编码格式（v1）】
 *
 *   第一个值：[64 位原始位模式]
 *   后续每个值：x = bits(cur) ^ bits(prev)
 *       [bit 0]                        ⇒ x == 0（与前值完全相同）
 *       [bit 1][bit 0]                ⇒ 复用上一个窗口
 *       [bit 1][bit 1][6 位前导零][7 位有效位长度][有效位]
 *
 * 【窗口是什么】
 *
 * 窗口 = (前导零个数 L, 有效位长度 len)。
 * 它描述的是 x 的**有效位区间**：
 *     最高有效位在第 63 - L 位，最低有效位在第 tz 位，
 *     其中 tz（尾零个数）= 64 - L - len（**由 L 与 len 推出来，不用单独传**）。
 * 有效位就是这段区间里的位，从高位往低位读，共 len 个。
 *
 * 复用条件（头文件里写死的）：新的 L ≥ 旧的 L，
 * 且 旧窗口能覆盖新的有效位区间（旧 L + 旧 len ≥ 新 L + 新 len，
 * 换成尾零的说法就是 **新 tz ≥ 旧 tz** —— 新的最低有效位不能比旧的更低，
 * 否则那个位就掉到窗口外面去了）。
 *
 * 复用时只写"旧窗口宽度"的有效位，省掉 13 位的窗口描述。
 *
 * 【坑 M6-14：有效位是"掐掉尾零"的，所以还原时移的是尾零，不是前导零】
 *
 * 写出去的有效位是 x 的第 [tz, 63-L] 位（右对齐，共 len 个），
 * 于是还原必须写成  x = val << tz，**不是** x = val << L。
 * 这两个差着一个"有效位在窗口里的位置"，搞混了会**丢掉低位**，
 * 而且丢得很像样：高位全对，只有最低几位不对 ——
 * 用浮点 `==` 去比根本发现不了（差得小的时候连肉眼都看不出来），
 * 只有比位模式才抓得住。
 * 我第一版就是写成了 `val << L`，随机往返里 977/2000 轮挂着。
 *
 * 【坑 M6-15：复用窗口之后，"当前窗口"必须保持不变】
 *
 * 复用时我们**知道**新 x 的真实 L 与 len（可能比旧窗口大），
 * 但**没有写进流里** —— 解码端无从得知。
 * 所以编解码两端都必须把"当前窗口"继续当成**旧窗口**，
 * 下一轮复用判断也用旧窗口。
 * 如果编码端偷偷把窗口更新成新算出来的 (L, len)，
 * 两端的窗口状态就分叉了 ⇒ 解出来的东西"看着像"但全错，
 * 而且不会报任何错 —— 这是最坏的一类 bug。
 *
 * 【坑 M6-16：double ↔ 位模式必须走 memcpy】
 *
 * 指针强转（(u64 *)&d）是把 double 当成 u64 访问，
 * 严格别名规则下是 UB（`-fno-strict-aliasing` 只是把症状压下去，不是修好）。
 * 一律 memcpy。
 *
 * 【坑 M6-17：解码只许硬失败】
 *
 * 读坏了 / 条数对不上 ⇒ E_CORRUPT。
 * 绝不能"读不出来就填 0"—— 那样一段坏数据会静默变成一段看着正常的数据。
 * 本文件末尾的"剩余位必须 < 8"就是"条数对不上"的检查：
 * 声明 n 条却多出一整个字节以上的数据 ⇒ E_CORRUPT。
 */

#include "modalities/ts/gorilla.h"
#include "modalities/ts/tsbits.h"

#include <string.h>

/* 每个值最坏要的位数：1 控制位 + 1 控制位 + 6 前导零 + 7 长度 + 64 有效位 */
#define GORILLA_BITS_PER_VALUE  79u
#define GORILLA_FIRST_BITS      64u

/* ---------------- double ↔ 位模式（坑 M6-16） ---------------- */

static u64 g_bits_of(double d)
{
    u64 b;

    memcpy(&b, &d, sizeof(b));
    return b;
}

static double g_double_of(u64 b)
{
    double d;

    memcpy(&d, &b, sizeof(d));
    return d;
}

/* ---------------- 位运算小工具 ---------------- */

/* 低 n 位全 1（n == 64 时是全 1） */
static u64 g_mask(u32 n)
{
    if (n >= 64u)
        return ~(u64)0;
    return (((u64)1) << n) - 1u;
}

/* 64 位里的前导零个数（x != 0 时调用） */
static u32 g_clz64(u64 x)
{
    u32 n = 0;
    u64 m = (u64)1 << 63;

    while (n < 64u && (x & m) == 0u) {
        n++;
        m >>= 1;
    }
    return n;
}

/* 64 位里的尾零个数（x != 0 时调用） */
static u32 g_ctz64(u64 x)
{
    u32 n = 0;

    while (n < 64u && (x & (((u64)1) << n)) == 0u)
        n++;
    return n;
}

/* ---------------- 编码 ---------------- */

u32 gorilla_encoded_bound(u32 n)
{
    u64 bits;

    if (n == 0)
        return 0;

    /* 首值 64 位 + (n-1) 个值各最坏 79 位 */
    bits = (u64)GORILLA_FIRST_BITS + (u64)(n - 1u) * (u64)GORILLA_BITS_PER_VALUE;
    bits = (bits + 7u) / 8u;

    if (bits > (u64)0xFFFFFFFFu)
        return 0xFFFFFFFFu;   /* n 大到这个份上，缓冲区本身就不可能存在 */
    return (u32)bits;
}

int gorilla_encode(const double *v, u32 n, u8 *out, u32 cap)
{
    TsBitWriter w;
    u64  prev = 0;
    u32  win_lead = 0;      /* 当前窗口的前导零个数 L */
    u32  win_len = 0;       /* 当前窗口的有效位长度 */
    bool have_win = false;  /* 有没有可复用的窗口 */
    u32  i;
    int  rc;

    if ((v == NULL && n > 0u) || (out == NULL && cap > 0u))
        return -E_INVAL;
    if (n == 0u)
        return 0;
    if (cap == 0u)
        return -E_NOSPACE;

    tsbw_init(&w, out, cap);

    /* 首值：原始 64 位 */
    prev = g_bits_of(v[0]);
    rc = tsbw_write_bits(&w, prev, GORILLA_FIRST_BITS);
    if (rc != OK)
        return -rc;

    for (i = 1u; i < n; i++) {
        u64 cur = g_bits_of(v[i]);
        u64 x = cur ^ prev;

        if (x == 0u) {
            /* 与前值完全相同：1 个位 */
            rc = tsbw_write_bit(&w, false);
            if (rc != OK)
                return -rc;
        } else {
            u32 lead = g_clz64(x);       /* 前导零个数 */
            u32 tz = g_ctz64(x);          /* 尾零个数 */
            u32 len = 64u - lead - tz;    /* 有效位个数（>= 1） */

            if (have_win && lead >= win_lead && (win_lead + win_len) >= (lead + len)) {
                /*
                 * 复用窗口：按**旧窗口的帧**写有效位。
                 * 旧帧的最低有效位在第 tz_old 位（由旧窗口推出来）。
                 */
                u32  tz_old = 64u - win_lead - win_len;
                u64  val = (x >> tz_old) & g_mask(win_len);

                rc = tsbw_write_bit(&w, true);
                if (rc == OK)
                    rc = tsbw_write_bit(&w, false);
                if (rc == OK)
                    rc = tsbw_write_bits(&w, val, win_len);
                if (rc != OK)
                    return -rc;
                /* 窗口**保持不变**（坑 M6-15）—— 解码端看不到新的 L/len */
            } else {
                /* 新窗口：1 1 + 6 位前导零 + 7 位长度 + 有效位 */
                u64 val = (x >> tz) & g_mask(len);

                rc = tsbw_write_bit(&w, true);
                if (rc == OK)
                    rc = tsbw_write_bit(&w, true);
                if (rc == OK)
                    rc = tsbw_write_bits(&w, (u64)lead, 6u);
                if (rc == OK)
                    rc = tsbw_write_bits(&w, (u64)len, 7u);
                if (rc == OK)
                    rc = tsbw_write_bits(&w, val, len);
                if (rc != OK)
                    return -rc;

                win_lead = lead;
                win_len = len;
                have_win = true;
            }
        }

        prev = cur;
    }

    {
        u32 bytes = 0;

        rc = tsbw_finish(&w, &bytes);
        if (rc != OK)
            return -rc;

        /*
         * 坑 M6-18：末尾那个字节里没写到的补齐位，**填 1**（不是 0）。
         *
         * 为什么：补齐位如果不置成确定的 1，它们就是"调用方缓冲区里的
         * 垃圾"，解码端分不清"这是补齐"还是"这是第 n+1 个值的数据"。
         * 填成 1 之后，多要一个值必然撞上：控制位读到 1 ⇒ 还要再读一位
         * ⇒ 要读 13 位的窗口描述 ⇒ 位不够 ⇒ E_CORRUPT。
         * 换句话说：**"条数对不上"从此能被可靠地抓住**。
         * （解码端不检查补齐位的取值，所以别人写的块（补 0）照样能解。）
         */
        {
            u32 used = tsbw_bit_length(&w);
            u32 rem = used % 8u;
            u32 b;

            if (rem != 0u && bytes > 0u) {
                u8 last = out[bytes - 1u];

                for (b = rem; b < 8u; b++)
                    last |= (u8)(1u << b);
                out[bytes - 1u] = last;
            }
        }

        return (int)bytes;
    }
}

/* ---------------- 解码 ---------------- */

int gorilla_decode(const u8 *buf, u32 len, double *v, u32 n)
{
    TsBitReader r;
    u64  prev = 0;
    u32  win_lead = 0;
    u32  win_len = 0;
    bool have_win = false;
    u32  i;
    u64  bits = 0;
    int  rc;

    if ((buf == NULL && len > 0u) || (v == NULL && n > 0u))
        return -E_INVAL;
    if (n == 0u)
        return 0;

    tsbr_init(&r, buf, len);

    /* 首值：原始 64 位 */
    rc = tsbr_read_bits(&r, GORILLA_FIRST_BITS, &bits);
    if (rc != OK)
        return -rc;
    prev = bits;
    v[0] = g_double_of(prev);

    for (i = 1u; i < n; i++) {
        bool flag = false;
        u64  cur = 0;

        rc = tsbr_read_bit(&r, &flag);
        if (rc != OK)
            return -rc;

        if (!flag) {
            /* x == 0：与前值完全相同 */
            cur = prev;
        } else {
            bool new_win = false;

            rc = tsbr_read_bit(&r, &new_win);
            if (rc != OK)
                return -rc;

            if (!new_win) {
                u64  val = 0;
                u32  tz;

                /* 复用窗口：窗口描述在流里没有，只能用手上这个 */
                if (!have_win)
                    return -E_CORRUPT;
                rc = tsbr_read_bits(&r, win_len, &val);
                if (rc != OK)
                    return -rc;

                tz = 64u - win_lead - win_len;
                /* 坑 M6-14：移的是尾零 tz，不是前导零 */
                cur = prev ^ ((win_len == 0u) ? (u64)0 : (val << tz));
                /* 窗口保持不变（坑 M6-15） */
            } else {
                u64 lead = 0;
                u64 lenbits = 0;
                u64 val = 0;
                u32 tz;

                rc = tsbr_read_bits(&r, 6u, &lead);
                if (rc != OK)
                    return -rc;
                rc = tsbr_read_bits(&r, 7u, &lenbits);
                if (rc != OK)
                    return -rc;
                /*
                 * 长度字段能表示 0..127，但 double 只有 64 位 ⇒ 越界就是坏流。
                 * 硬失败（E_CORRUPT），不要"凑合读"。
                 */
                if (lead + lenbits > 64u)
                    return -E_CORRUPT;

                rc = tsbr_read_bits(&r, (u32)lenbits, &val);
                if (rc != OK)
                    return -rc;

                tz = (u32)(64u - lead - lenbits);
                cur = prev ^ ((lenbits == 0u) ? (u64)0 : (val << tz));

                win_lead = (u32)lead;
                win_len = (u32)lenbits;
                have_win = true;
            }
        }

        v[i] = g_double_of(cur);
        prev = cur;
    }

    /*
     * 坑 M6-17：条数对不上 ⇒ 硬错误。
     * 解完 n 条之后，剩下的位只能是最后一个字节里的补齐位（< 8 位）。
     * 多出一整个字节以上 ⇒ 流里的东西比声明的多 ⇒ E_CORRUPT。
     */
    if (r.nbits - r.pos >= 8u)
        return -E_CORRUPT;

    return (int)n;
}
