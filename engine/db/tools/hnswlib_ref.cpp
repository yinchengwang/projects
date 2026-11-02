/*
 * hnswlib_ref.cpp — 用 hnswlib（业界事实标准）算同一个数据集的 recall
 *
 * 【为什么要有这个文件】
 *
 * PLAN §1.0 A3 要求"对照 hnswlib：同一批向量、同参数构建，
 * 我们的 recall@10 与 hnswlib 差 < 2 点"。
 *
 * 这不是为了好看 —— 它是**唯一能证明"我们的 recall 低是因为数据难，
 * 而不是因为实现错了"的对照**。
 *
 * 之前 A3 走的是退化方案（numpy 独立 GT 交叉验证）。
 * 那验的是"GT 算得对不对"，验不了"同样的数据、同样的参数，
 * 别人能到多少"。uniform 随机向量在 128 维下 recall 只有 0.39，
 * 光看自己的数字**无法判断**这是正常还是 bug。
 *
 * 【hnswlib 怎么来的：没有 wheel，用 MinGW 源码编译】
 *
 *   pip 装不上（本平台无预编译包，且 hnswlib 的 setup.py 要 MSVC）。
 *   但 hnswlib 的核心是**纯 C++ 头文件**（header-only），
 *   只要 g++ 就能编译 —— 不需要 Python 绑定。
 *
 *   所以这里直接用它的 C++ API：
 *     hnswlib::L2Space + hnswlib::HierarchicalNSW
 *
 *   头文件放在 tools/third_party/hnswlib/（MIT 协议，LICENSE 一并带上）。
 *
 * 【为什么不用 pybind11 的 Python 绑定】
 *
 * 试过：pybind11 3.1.0 + hnswlib 0.8.0 的绑定编译能过，
 * 但 add_items 一定段错误（pybind11 3.x 与 hnswlib 0.8.0 的绑定不兼容）。
 * 既然核心是头文件，直接写 C++ 程序更稳，也少一层依赖。
 *
 * 【输入格式：和 bench/vec_bench.c 的导出完全一致】
 *
 *   <base>.data   u32 dim, u32 n_vec, u32 n_query, u32 k, float data[]
 *   <base>.result u32 n_query, u32 k, f32 c_recall,
 *                u32 gt_ids[], u32 ann_ids[], float ann_dists[]
 *
 * 【量的是什么】
 *
 *   1. hnswlib 在各个 ef 下的 recall@10（GT 用**本地暴力扫**算，
 *      不读 C 侧的 GT —— 这样两边的 GT 也是独立实现的）
 *   2. 和 C 侧自报的 recall 对比 → A3 的判定（差 < 2 点）
 *
 * 用法:
 *   hnswlib_ref <base> [ef ...]
 *   例: hnswlib_ref test/tmp/vec_bench_data.bin.uniform 16 32 64 128 256 512
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <queue>
#include <string>
#include <vector>

#include "hnswlib.h"

namespace {

struct DataFile {
    uint32_t dim = 0;
    uint32_t n_vec = 0;
    uint32_t n_query = 0;
    uint32_t k = 0;
    std::vector<float> all;   /* (n_vec + n_query) * dim */
};

struct ResultFile {
    uint32_t n_query = 0;
    uint32_t k = 0;
    uint32_t ef = 0;          /* 主测量用的 ef */
    float c_recall = 0.0f;    /* 主测量（那个 ef）的 recall */
    std::vector<uint16_t> curve_efs;   /* 注意是 u16 —— 和写端的格式一致 */
    std::vector<float> curve_recalls;
    std::vector<uint32_t> gt_ids;
    std::vector<uint32_t> ann_ids;
    std::vector<float> ann_dists;
};

template <typename T>
bool read_raw(FILE *f, std::vector<T> &out, size_t count)
{
    out.resize(count);
    if (count == 0)
        return true;
    return fread(out.data(), sizeof(T), count, f) == count;
}

bool load_data(const std::string &path, DataFile &d)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    uint32_t hdr[4];
    bool ok = fread(hdr, sizeof(hdr), 1, f) == 1;
    if (ok) {
        d.dim = hdr[0];
        d.n_vec = hdr[1];
        d.n_query = hdr[2];
        d.k = hdr[3];
        ok = read_raw(f, d.all, (size_t)(d.n_vec + d.n_query) * d.dim);
    }
    fclose(f);
    return ok;
}

bool load_result(const std::string &path, ResultFile &r)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    uint32_t hdr[3];
    bool ok = fread(hdr, sizeof(hdr), 1, f) == 1;
    if (ok) {
        r.n_query = hdr[0];
        r.k = hdr[1];
        r.ef = hdr[2];
        ok = fread(&r.c_recall, sizeof(float), 1, f) == 1;
        uint32_t n_curve = 0;
        if (ok)
            ok = fread(&n_curve, sizeof(uint32_t), 1, f) == 1;
        if (ok && n_curve) {
            r.curve_efs.resize(n_curve);
            ok = fread(r.curve_efs.data(), sizeof(uint16_t), n_curve, f) == n_curve;
        }
        if (ok && n_curve)
            ok = read_raw(f, r.curve_recalls, n_curve);
        if (ok)
            ok = read_raw(f, r.gt_ids, (size_t)r.n_query * r.k);
        if (ok)
            ok = read_raw(f, r.ann_ids, (size_t)r.n_query * r.k);
        if (ok)
            ok = read_raw(f, r.ann_dists, (size_t)r.n_query * r.k);
    }
    fclose(f);
    return ok;
}

/*
 * 暴力扫的 GT（本地实现，**不读 C 侧的结果**）。
 *
 * 距离用平方 L2 —— hnswlib 的 L2Space 返回的也是平方距离（不开根号），
 * 两边同口径。
 */
void brute_gt(const float *vecs, uint32_t n, uint32_t dim,
              const float *queries, uint32_t nq, uint32_t k,
              std::vector<uint32_t> &out)
{
    out.assign((size_t)nq * k, 0);

    std::vector<std::pair<float, uint32_t>> cand(n);
    for (uint32_t i = 0; i < nq; i++) {
        const float *q = queries + (size_t)i * dim;
        for (uint32_t j = 0; j < n; j++) {
            const float *v = vecs + (size_t)j * dim;
            float d = 0.0f;
            for (uint32_t x = 0; x < dim; x++) {
                float t = v[x] - q[x];
                d += t * t;
            }
            cand[j] = { d, j };
        }
        /* 部分选择排序：只排前 k 个（k 很小，O(nk) 足够） */
        std::partial_sort(cand.begin(), cand.begin() + k, cand.end(),
                          [](const std::pair<float, uint32_t> &a,
                             const std::pair<float, uint32_t> &b) {
                              return a.first < b.first;
                          });
        for (uint32_t j = 0; j < k; j++)
            out[(size_t)i * k + j] = cand[j].second;
    }
}

}  /* namespace */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "用法: hnswlib_ref <base> [ef ...]\n"
                "  <base>.data / <base>.result 由 bench/vec_bench 导出\n");
        return 2;
    }

    std::string base = argv[1];
    std::vector<uint32_t> efs;
    for (int i = 2; i < argc; i++)
        efs.push_back((uint32_t)atoi(argv[i]));
    if (efs.empty())
        efs = { 16, 32, 64, 128, 256, 512 };

    DataFile d;
    if (!load_data(base + ".data", d)) {
        fprintf(stderr, "读 %s.data 失败\n", base.c_str());
        return 2;
    }

    ResultFile r;
    bool have_result = load_result(base + ".result", r);

    const float *vecs = d.all.data();
    const float *queries = d.all.data() + (size_t)d.n_vec * d.dim;

    printf("hnswlib 参照实现（业界事实标准）\n");
    printf("  数据: %s.data\n", base.c_str());
    printf("  dim=%u, n_vec=%u, n_query=%u, k=%u\n",
           d.dim, d.n_vec, d.n_query, d.k);

    /* ---- 本地暴力扫 GT（独立于 C 侧）---- */
    std::vector<uint32_t> gt;
    brute_gt(vecs, d.n_vec, d.dim, queries, d.n_query, d.k, gt);

    /* ---- 和 C 侧的 GT 交叉核对（两边的"尺子"必须一致）---- */
    if (have_result) {
        uint32_t diff = 0;
        for (size_t i = 0; i < gt.size() && i < r.gt_ids.size(); i++)
            if (gt[i] != r.gt_ids[i])
                diff++;
        printf("  与 C 侧 GT 的 row_id 不同数: %u / %zu\n", diff, gt.size());
        if (diff) {
            printf("  → 两边的 GT 不一致 —— 对照实验的前提不成立，先查 GT\n");
            return 1;
        }
        printf("  → 两边 GT 完全一致，对照成立\n");
    }

    /* ---- 建索引 ---- */
    hnswlib::L2Space space((int)d.dim);
    hnswlib::HierarchicalNSW<float> index(&space, d.n_vec, 16, 200, 100);

    printf("\n  建索引中（M=16, efC=200, 随机种子 100）...\n");
    /*
     * 逐个插入（hnswlib 的 C++ API 是 addPoint）。
     *
     * 顺序和 C 侧一致（0, 1, 2 …），这样两边建出来的图结构可比 ——
     * 虽然随机种子不同，图本来就不会逐位相同，但插入顺序一致
     * 至少排除了"顺序不同"这个变量。
     */
    for (uint32_t i = 0; i < d.n_vec; i++)
        index.addPoint(vecs + (size_t)i * d.dim, (hnswlib::labeltype)i);

    printf("\n  hnswlib 的 recall@%u：\n", d.k);
    std::vector<double> ref_recalls(efs.size(), 0.0);
    for (size_t e = 0; e < efs.size(); e++) {
        index.setEf((size_t)efs[e]);
        uint64_t hits = 0;
        for (uint32_t i = 0; i < d.n_query; i++) {
            auto res = index.searchKnn(queries + (size_t)i * d.dim, d.k);
            while (!res.empty()) {
                uint32_t id = (uint32_t)res.top().second;
                res.pop();
                for (uint32_t j = 0; j < d.k; j++) {
                    if (gt[(size_t)i * d.k + j] == id) {
                        hits++;
                        break;
                    }
                }
            }
        }
        ref_recalls[e] = (double)hits / (double)(d.n_query * d.k);
        printf("    ef=%3u → recall@%u = %.4f\n", efs[e], d.k, ref_recalls[e]);
    }

    if (have_result) {
        /*
         * 【A3 判定：必须在同一个 ef 上比】
         *
         * 第一版拿"最后一档 ef"去和 C 侧比 —— 而 C 侧导出的 recall
         * 是它自己的主测量 ef。两档不是同一个 ef，差 43 个点，
         * 报了个假的 FAIL。
         *
         * 所以：先在 C 侧的曲线上找到**同一个 ef** 那一档，
         * 没有就退回用 C 侧自报的 c_recall（它就是主测量 ef 的）。
         */
        printf("\n【A3 判定】同一批数据、同样的 M/efC、**同一个 ef**：\n");

        double our_at_ef = r.c_recall;
        bool found = false;
        for (size_t i = 0; i < r.curve_efs.size(); i++) {
            if (r.curve_efs[i] == r.ef) {
                our_at_ef = r.curve_recalls[i];
                found = true;
                break;
            }
        }
        (void)found;

        double ours = 0.0;
        double theirs = 0.0;
        bool have_pair = false;
        for (size_t e = 0; e < efs.size(); e++) {
            if (efs[e] != r.ef)
                continue;
            ours = our_at_ef;
            theirs = ref_recalls[e];
            have_pair = true;
            break;
        }
        if (!have_pair) {
            /* 用户没给主测量那一档 ef —— 单独补测一档 */
            index.setEf((size_t)r.ef);
            uint64_t hits = 0;
            for (uint32_t i = 0; i < d.n_query; i++) {
                auto res = index.searchKnn(queries + (size_t)i * d.dim, d.k);
                while (!res.empty()) {
                    uint32_t id = (uint32_t)res.top().second;
                    res.pop();
                    for (uint32_t j = 0; j < d.k; j++) {
                        if (gt[(size_t)i * d.k + j] == id) {
                            hits++;
                            break;
                        }
                    }
                }
            }
            ours = our_at_ef;
            theirs = (double)hits / (double)(d.n_query * d.k);
            have_pair = true;
            printf("    （ef=%u 不在命令行给的档位里，单独补测了一档）\n",
                   r.ef);
        }

        printf("    ef=%u：hnswlib = %.4f，我们 = %.4f\n",
               r.ef, theirs, ours);
        double diff = theirs - ours;
        printf("    差 %.4f（%+.1f 个点，负 = 我们更好）\n",
               diff, diff * 100.0);

        /*
         * 【判定的方向】
         *
         * A3 原文是"我们的 recall@10 与 hnswlib 差 < 2 点"。
         * 直译成"绝对值 < 2 点"是**错的**：
         * 两边是不同实现、不同随机种子建出来的图，本来就不会逐位相同；
         * 而且**我们比参照实现更好**不是缺陷，是好事。
         *
         * A3 想抓的是"我们比业界标准差很多"（那说明有 bug）。
         * 所以判据是：**我们不比 hnswlib 差 2 点以上**。
         */
        bool pass = (diff <= 0.02);
        printf("    %s\n", pass
               ? "PASS（我们不比 hnswlib 差 2 点以上）"
               : "**FAIL（我们比 hnswlib 差 2 点以上 —— 有 bug 的嫌疑）**");

        /* ---- 逐档曲线对比 ---- */
        if (!r.curve_efs.empty()) {
            printf("\n    逐档对比（C 侧导出的曲线 vs 本次实测）：\n");
            printf("      %-8s %-12s %-12s %s\n",
                   "ef", "hnswlib", "我们", "差");
            for (size_t i = 0; i < r.curve_efs.size(); i++) {
                double ref = -1.0;
                for (size_t e = 0; e < efs.size(); e++) {
                    if ((uint32_t)efs[e] == (uint32_t)r.curve_efs[i]) {
                        ref = ref_recalls[e];
                        break;
                    }
                }
                if (ref < 0)
                    continue;
                double dd = ref - r.curve_recalls[i];
                printf("      %-8u %-12.4f %-12.4f %+.1f 点%s\n",
                       (uint32_t)r.curve_efs[i], ref, r.curve_recalls[i],
                       dd * 100.0, dd < -0.02 ? "  ← 我们更好"
                                             : (dd > 0.02 ? "  ← 我们更差" : ""));
            }
        }

        if (!pass)
            return 1;
    }

    return 0;
}
