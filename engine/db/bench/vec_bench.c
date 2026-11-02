/*
 * vec_bench.c — 向量模态的性能基准与验收框架（v1.0 步骤 6）
 *
 * 验四件事：
 *
 *   A2 · recall@10（对照暴力扫的 ground truth）
 *   A4 · IO 可证：一次暴力扫读多少页，一次 ANN 查询读多少页
 *   A5 · 增量插入：边插边查，recall 不塌（每插 1/4 查一次，最终 ≥ 0.93）
 *   A3 · 对照基线 —— hnswlib 装不上时的退化方案（见下）
 *
 * ================= 【为什么分成"模态层"和"公开 API"两段】 =================
 *
 * 和 graph_bench 同款做法，理由也是同一个：
 *
 * 插入一个向量要改 **2 个页**（向量数据页 + 标量槽页），
 * 每改一个页就是一条 WAL 整页镜像 = 8KB 日志；
 * 再加每 7 个向量要开一个新数据页（又是几条记录）。
 * 实测 **约 52KB 日志 / 向量** —— 10 万向量 ≈ **5.2GB**。
 *
 * 那不是"慢一点"，是磁盘放不下。而且它与 ANN 的性能**无关** ——
 * 在挂着 WAL 的路径上量，量到的多半是日志 IO，不是算法的速度。
 *
 * 所以分成两段：
 *   - 主测量（A2 / A4 / A5 / 延迟）→ 模态层，不挂 WAL
 *   - 公开 API 的端到端路径 → **单独一小节，小规模**，量装载吞吐与 WAL 体积
 *
 * 公开 API 路径的正确性由 test/integration/test_vec_it.c 负责 ——
 * 基准不重复验正确性，只量性能。
 *
 * ================= 【两种数据形态，都要报】 =================
 *
 * 均匀随机（每个分量独立 U(0,1)）是图索引的**最坏情况**：
 * 高维下所有向量距离都差不多（"维度灾难"），"近邻"本身没有明确结构。
 * 聚簇（K 个中心 + 扰动）接近真实 embedding 的形态。
 *
 * 只报一种都会失真 ——
 * 只报均匀会显得 HNSW 不行（其实是数据没结构），
 * 只报聚簇会掩盖"高维均匀数据"这个真实难点。
 *
 * ================= 【A3 怎么办：hnswlib 装不上】 =================
 *
 * PLAN §1.7 R3 预置了退化方案：hnswlib 装不上时，
 * "对照 hnswlib"改为参数对齐的论文数值区间。
 *
 * 本机情况：pip 找不到 hnswlib 的 wheel（该平台无预编译包），
 * 从源码编译它的 Python 绑定又要求 MSVC。
 *
 * 但 hnswlib 的**核心是纯头文件**（header-only 的 C++ 库）——
 * 只要 g++ 就能用，不需要 Python、不需要 pybind11。
 * 所以 A3 **没有走退化方案**，而是真的对照上了：
 *
 *   tools/hnswlib_ref.cpp   同一批数据、同样 M/efC/ef，各自独立算 GT
 *   tools/gt_check.py       numpy 独立实现的 GT 交叉验证（验"尺子"本身）
 *
 * 结论见 docs/PLAN-v1.0.md §3 步骤 6。
 *
 * 用法:
 *   vec_bench [向量数] [维度] [查询数] [k] [ef] [M] [efC] [公开API规模] [数据文件]
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vec_bench.h"

#include "db.h"
#include "modalities/vector/hnsw.h"
#include "modalities/vector/vindex.h"
#include "modalities/vector/vstore.h"
#include "storage/pager.h"

/* ---------------- 一轮测量（模态层，不挂 WAL） ---------------- */

/*
 * 参与测量的 ef 档位。
 *
 * 文件级常量（不是函数内的 static）—— 因为导出结果文件时
 * 也要用同一组 ef，两处必须是同一个数组。
 */
static const u16 CURVE_EFS[] = { 16, 32, 64, 128, 256, 512 };
#define CURVE_N (sizeof(CURVE_EFS) / sizeof(CURVE_EFS[0]))

/*
 * 跑一轮：装载 → GT → 交叉校验 → ANN → 延迟。
 *
 * gt_ids 是外部给的大缓冲（nq × k），用来存 GT 的 row_id。
 */
static int run_shape(const float *vecs, const float *queries,
                     u32 nvec, u16 dim, u32 nq, u32 k, u16 ef,
                     u16 m, u16 efc, int want_curve,
                     Round *out, u32 *gt_ids, u32 *ann_ids,
                     const char *dump_base)
{
    memset(out, 0, sizeof(*out));

    float curve_recalls[CURVE_N];
    memset(curve_recalls, 0, sizeof(curve_recalls));

    const char *path = "test/tmp/vec_bench_shape.db";
    remove(path);
    remove("test/tmp/vec_bench_shape.db-wal");

    Pager *pg = NULL;
    if (pager_open(path, 32768, &pg) != OK) {
        printf("  打开 pager 失败\n");
        return E_IO;
    }

    VStore *vs = NULL;
    int rc = vstore_create(pg, NULL, NULL, dim, &vs);
    if (rc != OK) {
        pager_close(pg);
        printf("  建向量存储失败\n");
        return rc;
    }

    Hnsw *h = hnsw_create(dim, HNSW_L2, m, efc, 0x5EEDu);
    if (!h) {
        vstore_destroy(vs);
        pager_close(pg);
        printf("  建索引失败\n");
        return E_IO;
    }

    /* ---- 装载：向量进存储 + 进索引 ---- */
    double t0 = now_us();
    for (u32 i = 0; i < nvec; i++) {
        u64 rid = 0;
        rc = vstore_insert(vs, 0, vecs + (size_t)i * dim, (i32)(i % 10), &rid);
        if (rc != OK)
            goto fail;
        u32 hid = 0;
        rc = hnsw_add(h, vecs + (size_t)i * dim, &hid);
        if (rc != OK)
            goto fail;
        /* 索引里的 id 必须等于存储里的 row_id —— 列对齐的防线 */
        if (hid != (u32)rid) {
            printf("  !! 第 %u 个向量：索引 id=%u，存储 row_id=%llu —— 错位了\n",
                   i, hid, (unsigned long long)rid);
            rc = E_CORRUPT;
            goto fail;
        }
    }
    double t1 = now_us();
    out->load_ms = (t1 - t0) / 1000.0;
    out->load_per_sec = (double)nvec / ((t1 - t0) / 1e6);

    /* ---- 内存 GT（从原始数组算，独立于被测实现）---- */
    float *gt_d = (float *)calloc((size_t)nq * k, sizeof(float));
    if (!gt_d) {
        rc = E_IO;
        goto fail;
    }
    for (u32 i = 0; i < nq; i++) {
        topk_exact(vecs, nvec, dim, queries + (size_t)i * dim, k,
                   gt_ids + (size_t)i * k, gt_d + (size_t)i * k);
    }

    /* ---- 存储 GT：读回全部向量，量页读 + 交叉校验 ---- */
    float *buf = (float *)malloc((size_t)nvec * dim * sizeof(float));
    if (!buf) {
        free(gt_d);
        rc = E_IO;
        goto fail;
    }
    u64 pr0 = vstore_page_reads(vs);
    u64 got = 0;
    rc = vstore_scan_all(vs, buf, &got);
    u64 pr1 = vstore_page_reads(vs);
    if (rc != OK || got != nvec) {
        printf("  读回向量失败（rc=%d，读到 %llu，应为 %u）\n",
               rc, (unsigned long long)got, nvec);
        free(buf);
        free(gt_d);
        rc = (rc == OK) ? E_CORRUPT : rc;
        goto fail;
    }
    out->scan_pages = pr1 - pr0;
    out->exact_pages = out->scan_pages * (u64)nq;

    /* 交叉校验：存储 GT 与内存 GT 逐条比 */
    for (u32 i = 0; i < nq; i++) {
        u32 ids[64];
        float ds[64];
        topk_exact(buf, (u32)got, dim, queries + (size_t)i * dim, k, ids, ds);
        for (u32 j = 0; j < k; j++) {
            size_t idx = (size_t)i * k + j;
            if (ids[j] == gt_ids[idx])
                continue;
            float a = ds[j];
            float b = gt_d[idx];
            float tol = 1e-5f * (fabsf(a) + fabsf(b) + 1e-6f);
            if (fabsf(a - b) <= tol)
                out->gt_tie++;     /* 距离并列 —— 可接受 */
            else
                out->gt_bad++;     /* 距离也不同 —— 真错 */
        }
    }
    free(buf);

    /* ---- 预热 ---- */
    for (u32 i = 0; i < nq; i++) {
        HnswHit hits[64];
        u32 nh = 0;
        hnsw_search(h, queries + (size_t)i * dim, k, ef, hits, &nh);
    }

    /* ---- ANN ---- */
    Samples s;
    samples_init(&s, nq);
    if (!s.us) {
        free(gt_d);
        rc = E_IO;
        goto fail;
    }

    u64 a0 = vstore_page_reads(vs);
    u64 hit_total = 0;

    /* ANN 结果的 row_id 和距离也存下来，给 numpy 做交叉验证 */
    float *ann_d = ann_ids ? (float *)calloc((size_t)nq * k, sizeof(float))
                           : NULL;
    if (ann_ids && !ann_d) {
        samples_free(&s);
        free(gt_d);
        rc = E_IO;
        goto fail;
    }

    for (u32 i = 0; i < nq; i++) {
        HnswHit hits[64];
        u32 nh = 0;

        double q0 = now_us();
        rc = hnsw_search(h, queries + (size_t)i * dim, k, ef, hits, &nh);
        double q1 = now_us();

        if (rc != OK) {
            samples_free(&s);
            free(ann_d);
            free(gt_d);
            goto fail;
        }
        s.us[s.n++] = q1 - q0;

        for (u32 x = 0; x < nh; x++) {
            if (ann_ids)
                ann_ids[(size_t)i * k + x] = hits[x].id;
            if (ann_d)
                ann_d[(size_t)i * k + x] = hits[x].dist;
            for (u32 y = 0; y < k; y++) {
                if (hits[x].id == gt_ids[(size_t)i * k + y]) {
                    hit_total++;
                    break;
                }
            }
        }
    }
    u64 a1 = vstore_page_reads(vs);
    out->ann_pages = a1 - a0;

    out->recall = (double)hit_total / (double)(nq * k);

    out->p50 = pct(&s, 0.50);
    out->p90 = pct(&s, 0.90);
    out->p99 = pct(&s, 0.99);
    out->max = pct(&s, 1.0);

    samples_free(&s);

    /*
     * 索引常驻内存的体积。
     *
     * 两部分：
     *   - 向量拷贝：n × dim × 4（hnswlib 模型 —— 索引自带一份向量）
     *   - 图结构：n × VINDEX_PAYLOAD(m)（索引记录的定长载荷）
     *
     * 【为什么用 VINDEX_PAYLOAD 而不是自己估】
     *
     * 它是**磁盘格式的一部分**，写死在那儿的东西。
     * 自己估的话，链接数、层数这些常量一改，估算就漂了 ——
     * 而漂了的估算没人会发现。
     *
     * 它是**上界**：实际上绝大多数节点只在第 0 层（第 0 层有 2m 条边，
     * 上面每层 m 条），所以真实占用比这个数小。
     */
    out->index_bytes = (u64)nvec * (u64)dim * 4ull +
                       (u64)nvec * (u64)VINDEX_PAYLOAD(m);

    /* ---- recall 随 ef 的曲线 ---- */
    /*
     * 两种数据形态都算。
     *
     * 曲线有两个用处：
     *   1. 说明"recall 随 ef 单调上升" —— 这是实现正确的强证据
     *      （参数错的话曲线不会这么规整）
     *   2. 给 A3 的 hnswlib 对照提供逐档数据（见 dump_result 的说明）
     */
    {
        printf("    recall 随 ef 的变化：\n");
        for (size_t e = 0; e < CURVE_N; e++) {
            u64 h64 = 0;
            for (u32 i = 0; i < nq; i++) {
                HnswHit hits[64];
                u32 nh = 0;
                hnsw_search(h, queries + (size_t)i * dim, k, CURVE_EFS[e],
                            hits, &nh);
                for (u32 x = 0; x < nh; x++) {
                    for (u32 y = 0; y < k; y++) {
                        if (hits[x].id == gt_ids[(size_t)i * k + y]) {
                            h64++;
                            break;
                        }
                    }
                }
            }
            double r = (double)h64 / (double)(nq * k);
            curve_recalls[e] = (float)r;
            printf("      ef=%3u → recall@%u = %.4f\n", CURVE_EFS[e], k, r);
        }
        (void)want_curve;
    }

    /* ---- 落盘，给 numpy / hnswlib 做独立交叉验证 ---- */
    if (dump_base && ann_ids) {
        char dpath[512];
        char rpath[512];
        snprintf(dpath, sizeof(dpath), "%s.data", dump_base);
        snprintf(rpath, sizeof(rpath), "%s.result", dump_base);
        if (dump_data(dpath, dim, nvec, nq, k, vecs, queries) == 0 &&
            dump_result(rpath, nq, k, ef, (float)out->recall,
                        CURVE_EFS, curve_recalls, CURVE_N,
                        gt_ids, ann_ids, ann_d) == 0) {
            printf("    已导出 %s / %s（供 tools/gt_check.py 与 "
                   "tools/hnswlib_ref.cpp 交叉验证）\n", dpath, rpath);
        }
    }

    free(ann_d);
    free(gt_d);
    hnsw_destroy(h);
    vstore_destroy(vs);
    pager_close(pg);
    remove(path);
    remove("test/tmp/vec_bench_shape.db-wal");
    return OK;
fail:
    hnsw_destroy(h);
    vstore_destroy(vs);
    pager_close(pg);
    remove(path);
    remove("test/tmp/vec_bench_shape.db-wal");
    return rc;
}

/* ---------------- A5 · 增量插入 ---------------- */

/*
 * 独立的库，从零开始**逐块插入**。
 *
 * 【为什么不能用主流程那个库】
 *
 * 主流程已经把全部向量插过了。再插一遍会得到 2N 个向量
 * （row_id 不同、内容相同），召回率会因为"一半命中是副本"而虚低 ——
 * 那是测量方法的错，不是实现的错。
 *
 * 【数据必须和主测量同一批】
 *
 * 否则两个数字没法比：recall 0.98 vs 0.72 可能只是"两批随机数据不一样"。
 * 基准的价值在于**同一把尺子**。
 */
static int run_incremental(const float *vecs, const float *queries,
                           u32 nvec, u16 dim, u32 nq, u32 k, u16 ef,
                           u16 m, u16 efc, double *last_out)
{
    const char *path = "test/tmp/vec_bench_inc.db";
    remove(path);
    remove("test/tmp/vec_bench_inc.db-wal");

    Pager *pg = NULL;
    if (pager_open(path, 32768, &pg) != OK)
        return E_IO;

    VStore *vs = NULL;
    int rc = vstore_create(pg, NULL, NULL, dim, &vs);
    if (rc != OK) {
        pager_close(pg);
        return rc;
    }

    Hnsw *h = hnsw_create(dim, HNSW_L2, m, efc, 0x5EEDu);
    if (!h) {
        vstore_destroy(vs);
        pager_close(pg);
        return E_IO;
    }

    u32 *g = (u32 *)calloc((size_t)nq * k, sizeof(u32));
    float *gd = (float *)calloc((size_t)nq * k, sizeof(float));
    if (!g || !gd) {
        free(g); free(gd);
        hnsw_destroy(h); vstore_destroy(vs); pager_close(pg);
        return E_IO;
    }

    u32 chunk = nvec / 4;
    if (chunk == 0)
        chunk = 1;

    double last = 0.0;

    for (u32 base = 0; base < nvec; base += chunk) {
        u32 upto = base + chunk;
        if (upto > nvec)
            upto = nvec;

        for (u32 i = base; i < upto; i++) {
            u64 rid = 0;
            if (vstore_insert(vs, 0, vecs + (size_t)i * dim,
                              (i32)(i % 10), &rid) != OK) {
                rc = E_IO;
                goto done;
            }
            u32 hid = 0;
            if (hnsw_add(h, vecs + (size_t)i * dim, &hid) != OK) {
                rc = E_IO;
                goto done;
            }
        }

        /* GT 用**当前已插入的**那些向量重算 */
        for (u32 i = 0; i < nq; i++) {
            topk_exact(vecs, upto, dim, queries + (size_t)i * dim, k,
                       g + (size_t)i * k, gd + (size_t)i * k);
        }

        u64 h64 = 0;
        for (u32 i = 0; i < nq; i++) {
            HnswHit hits[64];
            u32 nh = 0;
            hnsw_search(h, queries + (size_t)i * dim, k, ef, hits, &nh);
            for (u32 x = 0; x < nh; x++) {
                for (u32 y = 0; y < k; y++) {
                    if (hits[x].id == g[(size_t)i * k + y]) {
                        h64++;
                        break;
                    }
                }
            }
        }
        last = (double)h64 / (double)(nq * k);
        printf("    插到 %u 个时 recall@%u = %.4f\n", upto, k, last);
    }

    *last_out = last;

done:
    free(g);
    free(gd);
    hnsw_destroy(h);
    vstore_destroy(vs);
    pager_close(pg);
    remove(path);
    remove("test/tmp/vec_bench_inc.db-wal");
    return rc;
}

/* ---------------- 公开 API 端到端（小规模） ---------------- */

/*
 * 这一段量的是**真实成本**：公开 API 路径上挂着 WAL，
 * 每插一个向量要写几十 KB 日志。
 *
 * 规模故意压小（几千条）：不是测不动，是**没必要** ——
 * 这一段要回答的问题是"公开 API 路径的开销有多大"，
 * 不是"公开 API 路径能达到多少 QPS"。
 */
static void run_public_api(u16 dim, u32 nvec, u32 nq, u32 k, u16 ef,
                           u16 m, u16 efc)
{
    const char *path = "test/tmp/vec_bench_api.db";
    const char *walp = "test/tmp/vec_bench_api.db-wal";
    remove(path);
    remove(walp);

    float *vecs = (float *)malloc((size_t)nvec * dim * sizeof(float));
    float *queries = (float *)malloc((size_t)nq * dim * sizeof(float));
    float *centers = (float *)malloc((size_t)100 * dim * sizeof(float));
    u32 *gt_ids = (u32 *)calloc((size_t)nq * k, sizeof(u32));
    float *gt_d = (float *)calloc((size_t)nq * k, sizeof(float));
    if (!vecs || !queries || !centers || !gt_ids || !gt_d) {
        free(vecs); free(queries); free(centers); free(gt_ids); free(gt_d);
        return;
    }

    gen_centers(centers, 100, dim);
    gen_clustered_around(centers, 100, vecs, nvec, dim);
    gen_clustered_around(centers, 100, queries, nq, dim);
    free(centers);

    DB *db = NULL;
    if (db_open(path, &db) != DB_OK) {
        printf("    开库失败\n");
        free(vecs); free(queries); free(gt_ids); free(gt_d);
        return;
    }
    db_vec_create(db, dim, m, efc);

    /* ---- 装载 ---- */
    double t0 = now_us();
    for (u32 i = 0; i < nvec; i++) {
        u64 rid = 0;
        if (db_vec_insert(db, vecs + (size_t)i * dim, (i32)(i % 10),
                          &rid) != DB_OK) {
            printf("    插入失败（第 %u 个）\n", i);
            db_close(db);
            free(vecs); free(queries); free(gt_ids); free(gt_d);
            return;
        }
    }
    double t1 = now_us();

    /* WAL 体积必须在**关库前**量 —— 关库会把日志合并掉 */
    u64 wal = file_size(walp);

    for (u32 i = 0; i < nq; i++) {
        topk_exact(vecs, nvec, dim, queries + (size_t)i * dim, k,
                   gt_ids + (size_t)i * k, gt_d + (size_t)i * k);
    }

    u64 hits = 0;
    u64 exact_bad = 0;
    u64 pr0 = db_vec_page_reads(db);

    for (u32 i = 0; i < nq; i++) {
        db_vec_hit hh[64];
        u32 nh = 0;
        db_vec_knn(db, queries + (size_t)i * dim, k, ef, hh, &nh);
        for (u32 x = 0; x < nh; x++) {
            for (u32 y = 0; y < k; y++) {
                if (hh[x].row_id == gt_ids[(size_t)i * k + y]) {
                    hits++;
                    break;
                }
            }
        }

        /* 精确查询的结果要和内存 GT 逐条一致 */
        db_vec_hit g[64];
        u32 ng = 0;
        db_vec_knn_exact(db, queries + (size_t)i * dim, k, g, &ng);
        for (u32 j = 0; j < k && j < ng; j++) {
            if (g[j].row_id != gt_ids[(size_t)i * k + j]) {
                float a = g[j].dist;
                float b = gt_d[(size_t)i * k + j];
                float tol = 1e-5f * (fabsf(a) + fabsf(b) + 1e-6f);
                if (fabsf(a - b) > tol)
                    exact_bad++;
            }
        }
    }
    u64 pr1 = db_vec_page_reads(db);

    double recall = (double)hits / (double)(nq * k);

    printf("    装载 %u 条：%.0f ms（%.0f 条/秒）\n",
           nvec, (t1 - t0) / 1000.0, (double)nvec / ((t1 - t0) / 1e6));
    printf("    WAL 体积：%.1f MB（%.1f KB/向量）\n",
           (double)wal / (1024.0 * 1024.0),
           nvec ? (double)wal / (double)nvec / 1024.0 : 0.0);
    printf("    recall@%u = %.4f（公开 API 与模态层同源，应完全一致）\n",
           k, recall);
    printf("    精确查询与内存 GT 不一致的条数：%llu\n",
           (unsigned long long)exact_bad);
    printf("    暴力扫 %u 次共读 %llu 页（每次 %.0f 页）\n",
           nq, (unsigned long long)(pr1 - pr0),
           nq ? (double)(pr1 - pr0) / (double)nq : 0.0);

    /* ---- 重开库：量"索引读回"的真实成本 ---- */
    db_close(db);

    double t2 = now_us();
    DB *db2 = NULL;
    db_open(path, &db2);
    double t3 = now_us();
    if (db2) {
        u64 n = 0;
        db_vec_count(db2, &n);
        printf("    重开库（读回索引）：%.0f ms，%llu 个向量\n",
               (t3 - t2) / 1000.0, (unsigned long long)n);
        db_close(db2);
    } else {
        printf("    重开库失败\n");
    }

    free(vecs);
    free(queries);
    free(gt_ids);
    free(gt_d);
    remove(path);
    remove(walp);
}

/* ---------------- 主流程 ---------------- */

int main(int argc, char **argv)
{
    u32 nvec = (argc > 1) ? (u32)atoi(argv[1]) : 100000u;
    u16 dim = (argc > 2) ? (u16)atoi(argv[2]) : 128u;
    u32 nq = (argc > 3) ? (u32)atoi(argv[3]) : 100u;
    u32 k = (argc > 4) ? (u32)atoi(argv[4]) : 10u;
    u16 ef = (argc > 5) ? (u16)atoi(argv[5]) : 64u;
    u16 m = (argc > 6) ? (u16)atoi(argv[6]) : 16u;
    u16 efc = (argc > 7) ? (u16)atoi(argv[7]) : 200u;
    u32 api_n = (argc > 8) ? (u32)atoi(argv[8]) : 2000u;
    const char *dump = (argc > 9) ? argv[9] : "test/tmp/vec_bench_data.bin";

    printf("向量模态性能基准\n");
    printf("  %u 个向量 × %u 维，%u 个查询，k=%u，ef=%u，M=%u，efC=%u\n",
           nvec, dim, nq, k, ef, m, efc);
    printf("  主测量跑在模态层（不挂 WAL）；公开 API 路径单列 %u 条小规模\n\n",
           api_n);

    u32 *gt_ids = (u32 *)calloc((size_t)nq * k, sizeof(u32));
    u32 *ann_ids = (u32 *)calloc((size_t)nq * k, sizeof(u32));
    if (!gt_ids || !ann_ids)
        return 1;

    char ubase[512];
    char cbase[512];
    snprintf(ubase, sizeof(ubase), "%s.uniform", dump);
    snprintf(cbase, sizeof(cbase), "%s.clustered", dump);

    Round ru;
    Round rc;

    /* ================= 第一轮：均匀随机（最坏情况） ================= */
    {
        float *vecs = (float *)malloc((size_t)nvec * dim * sizeof(float));
        float *queries = (float *)malloc((size_t)nq * dim * sizeof(float));
        if (!vecs || !queries)
            return 1;

        gen_uniform(vecs, nvec, dim);
        gen_uniform(queries, nq, dim);

        printf("【%s】\n", "均匀随机（高维均匀向量 = 图索引的最坏情况）");
        if (run_shape(vecs, queries, nvec, dim, nq, k, ef, m, efc,
                      1, &ru, gt_ids, ann_ids, ubase) != OK) {
            printf("  测量失败\n");
            return 1;
        }

        printf("    装载: %.0f ms（%.0f 向量/秒）\n",
               ru.load_ms, ru.load_per_sec);
        printf("    recall@%u = %.4f   延迟 p50=%.0f p90=%.0f p99=%.0f max=%.0f us\n",
               k, ru.recall, ru.p50, ru.p90, ru.p99, ru.max);
        printf("    页读: ANN %llu，暴力扫一次 %llu，%u 次共 %llu\n",
               (unsigned long long)ru.ann_pages,
               (unsigned long long)ru.scan_pages,
               nq, (unsigned long long)ru.exact_pages);
        printf("    GT 交叉校验: 并列 %u 处，真错 %u 处\n",
               ru.gt_tie, ru.gt_bad);
        printf("    索引常驻内存: %.1f MB\n",
               (double)ru.index_bytes / (1024.0 * 1024.0));

        free(vecs);
        free(queries);
    }

    /* ================= 第二轮：聚簇（接近真实 embedding） ================= */
    float *cvecs = NULL;
    float *cqueries = NULL;
    {
        cvecs = (float *)malloc((size_t)nvec * dim * sizeof(float));
        cqueries = (float *)malloc((size_t)nq * dim * sizeof(float));
        if (!cvecs || !cqueries)
            return 1;

        /*
         * 数据和查询**共用同一批中心** —— 查询必须在数据的分布内。
         * （见 vec_bench.h 里 gen_clustered_around 的说明。）
         */
        float *centers = (float *)malloc((size_t)100 * dim * sizeof(float));
        if (!centers)
            return 1;
        gen_centers(centers, 100, dim);
        gen_clustered_around(centers, 100, cvecs, nvec, dim);
        gen_clustered_around(centers, 100, cqueries, nq, dim);
        free(centers);

        printf("\n【%s】\n", "聚簇（100 个中心 + 扰动，接近真实 embedding）");
        if (run_shape(cvecs, cqueries, nvec, dim, nq, k, ef, m, efc,
                      0, &rc, gt_ids, ann_ids, cbase) != OK) {
            printf("  测量失败\n");
            return 1;
        }

        printf("    装载: %.0f ms（%.0f 向量/秒）\n",
               rc.load_ms, rc.load_per_sec);
        printf("    recall@%u = %.4f   延迟 p50=%.0f p90=%.0f p99=%.0f max=%.0f us\n",
               k, rc.recall, rc.p50, rc.p90, rc.p99, rc.max);
        printf("    页读: ANN %llu，暴力扫一次 %llu，%u 次共 %llu\n",
               (unsigned long long)rc.ann_pages,
               (unsigned long long)rc.scan_pages,
               nq, (unsigned long long)rc.exact_pages);
        printf("    GT 交叉校验: 并列 %u 处，真错 %u 处\n",
               rc.gt_tie, rc.gt_bad);
        printf("    索引常驻内存: %.1f MB（向量副本 %.1f MB + 图结构上界 %.1f MB）\n",
               (double)rc.index_bytes / (1024.0 * 1024.0),
               (double)nvec * dim * 4.0 / (1024.0 * 1024.0),
               (double)nvec * VINDEX_PAYLOAD(m) / (1024.0 * 1024.0));
    }

    /* ================= A5 · 增量插入（同一批数据） ================= */
    printf("\n【A5 · 增量插入】（与上面同一批数据，每插 1/4 查一次）\n");
    double last = 0.0;
    if (run_incremental(cvecs, cqueries, nvec, dim, nq, k, ef, m, efc,
                        &last) != OK)
        printf("  增量插入测量失败\n");
    free(cvecs);
    free(cqueries);

    /* ================= 公开 API 端到端 ================= */
    printf("\n【公开 API 端到端（小规模，挂着 WAL 的真实路径）】\n");
    run_public_api(dim, api_n, nq, k, ef, m, efc);

    /* ================= 判定 ================= */
    printf("\n【判定】\n");
    printf("    A2 均匀随机 recall@%u = %.4f  %s\n", k, ru.recall,
           ru.recall >= 0.95 ? "PASS" : "**未达标（见下面的分析）**");
    printf("    A2 聚簇     recall@%u = %.4f  %s\n", k, rc.recall,
           rc.recall >= 0.95 ? "PASS" : "FAIL");
    /*
     * A4 的判定口径。
     *
     * ANN 查询读 0 页 —— 因为 HNSW 索引**常驻内存**（hnswlib 模型）。
     * 所以"暴力扫 / ANN"这个比值是除以 0，没有意义。
     *
     * 有意义的说法是：
     *   暴力扫一次要读 ceil(N/K) 个向量页；ANN 查询一个页都不读。
     * 代价写在别处 —— 索引常驻内存多少 MB、重开库要读回多少。
     */
    printf("    A4 页读：暴力扫一次 %llu 页 vs ANN 查询 %llu 页  %s\n",
           (unsigned long long)rc.scan_pages,
           (unsigned long long)rc.ann_pages,
           rc.scan_pages >= 20u * (rc.ann_pages ? rc.ann_pages : 1)
               ? "PASS" : "FAIL");
    printf("       （ANN 读 0 页是因为索引常驻内存，代价是 %.1f MB 常驻 + 重开库读回）\n",
           (double)rc.index_bytes / (1024.0 * 1024.0));
    printf("    A5 增量插入后 recall = %.4f  %s\n", last,
           last >= 0.93 ? "PASS" : "FAIL");
    printf("    A3 对照 hnswlib：见 tools/hnswlib_ref.cpp\n");
    printf("       （同一批数据、同样 M/efC/ef，各自独立算 GT）\n");
    printf("    A3 退化方案（numpy 独立 GT 交叉验证）：tools/gt_check.py\n");

    free(gt_ids);
    return 0;
}
