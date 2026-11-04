/*
 * rtree_split.c —— R-tree 的**二次分裂**（从 rtree.c 拆出来）
 *
 * ==================== 为什么拆 ====================
 *
 * rtree.c 贴着 800 行的上限，而 v1.4 步骤 6（A7）要在里面加
 * 写日志的接线。二次分裂这一段自成一体（不碰 pager、不碰页），
 * 拆出来最干净 —— 与 rtree_knn.c / rtree_scan.c 同样的理由。
 *
 * ==================== 算法：Guttman 1984 §3.3 ====================
 *
 * 原始版 QuadraticSplit：每次都**现算**代价，不维护"最坏情况"
 * 这类启发式状态。判据都是明确的数，写错了能立刻从
 * "树退化成链"看出来。
 */

#include "modalities/geo/rtree_priv.h"

#include <string.h>

/* 把一组下标排成升序（原地） */
void rt_sort_idx(u32 *a, u32 n)
{
    u32 i;

    for (i = 1; i < n; i++) {
        u32 v = a[i];
        u32 j = i;

        while (j > 0 && a[j - 1] > v) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = v;
    }
}

/*
 * QuadraticSplit（Guttman 1984 §3.3）。
 *
 * 输入：溢出的节点页（n = M + 1 = 204 条）；输出：两组条目的下标。
 *
 * PickSeeds：取**合并后 MBR 面积最大**的那一对做种子（往往离得最远）。
 *
 * PickNext：剩下的条目里挑"对两组的**扩张面积之差**最大"的那个 ——
 * 差得越大归属越明确，先放进去最省事；分配给**扩张面积较小**的那组。
 * 差相同时给面积更小的那组；面积也相同时给条目更少的那组。
 *
 * 坑 M4-39：每组至少 m = ceil(M/2) = 102 个。循环里有一条**保底规则**：
 * 若把某条目放进偏好组之后另一组就再也吃不到 m 个了，那它必须改投另一
 * 组。没有这条规则，树会长出"瘦长条"的节点 —— 退化链的起点。
 */
void rt_quadratic_split(const RtEntry *ents, u32 n,
                        u32 *g1, u32 *n1_out, Mbr *mbr1,
                        u32 *g2, u32 *n2_out, Mbr *mbr2)
{
    bool used[RT_CAP + 1];
    u32 s1 = 0;
    u32 s2 = 0;
    u32 n1 = 0;
    u32 n2 = 0;
    u32 remaining;
    float best_area = -1.0f;
    Mbr m1;
    Mbr m2;
    u32 i;
    u32 j;

    memset(used, 0, sizeof(used));

    /* PickSeeds：合并后面积最大的一对 */
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            Mbr mi, mj, u;
            float a;

            rt_ent_mbr(&ents[i], &mi);
            rt_ent_mbr(&ents[j], &mj);
            mbr_union(&mi, &mj, &u);
            a = mbr_area(&u);
            if (a > best_area) {
                best_area = a;
                s1 = i;
                s2 = j;
            }
        }
    }

    g1[n1++] = s1;
    g2[n2++] = s2;
    used[s1] = true;
    used[s2] = true;
    rt_ent_mbr(&ents[s1], &m1);
    rt_ent_mbr(&ents[s2], &m2);

    /* PickNext：逐个分配剩下的条目 */
    remaining = n - 2;
    while (remaining > 0) {
        u32 pick = 0;
        float best_diff = -1.0f;
        float best_sum = -1.0f;
        float d1, d2;
        int to;
        Mbr mi;

        for (i = 0; i < n; i++) {
            float e1, e2, diff, sum;

            if (used[i])
                continue;
            rt_ent_mbr(&ents[i], &mi);
            e1 = mbr_enlarge_area(&m1, &mi);
            e2 = mbr_enlarge_area(&m2, &mi);
            diff = (e1 > e2) ? (e1 - e2) : (e2 - e1);
            sum = e1 + e2;
            if (diff > best_diff || (diff == best_diff && sum > best_sum)) {
                best_diff = diff;
                best_sum = sum;
                pick = i;
            }
        }

        rt_ent_mbr(&ents[pick], &mi);
        d1 = mbr_enlarge_area(&m1, &mi);
        d2 = mbr_enlarge_area(&m2, &mi);

        /* 分配给扩张面积较小的那一组 */
        if (d1 < d2) {
            to = 0;
        } else if (d2 < d1) {
            to = 1;
        } else {
            float a1 = mbr_area(&m1);
            float a2 = mbr_area(&m2);

            if (a1 < a2)
                to = 0;
            else if (a2 < a1)
                to = 1;
            else
                to = (n1 <= n2) ? 0 : 1;
        }

        /* 保底：另一组必须还吃得到 m 个（坑 M4-39） */
        {
            u32 other = (to == 0) ? n2 : n1;

            if (other + (remaining - 1) < RT_MIN)
                to = 1 - to;
        }

        if (to == 0) {
            g1[n1++] = pick;
            mbr_union(&m1, &mi, &m1);
        } else {
            g2[n2++] = pick;
            mbr_union(&m2, &mi, &m2);
        }
        used[pick] = true;
        remaining--;
    }

    /* 两组各自升序 —— 写回时条目顺序与原下标一致，结果可复现 */
    rt_sort_idx(g1, n1);
    rt_sort_idx(g2, n2);
    *n1_out = n1;
    *n2_out = n2;
    *mbr1 = m1;      /* m1 / m2 就是两组的包围盒（循环里一直在维护） */
    *mbr2 = m2;
}

/* 把一组（解码后的）条目写进目标页的条目区 */
void rt_group_write(u8 *dst, const RtEntry *src, const u32 *g, u32 n)
{
    u32 w;

    for (w = 0; w < n; w++)
        rt_ent_put(dst + (size_t)w * RT_ENT_SIZE, &src[g[w]]);
}
