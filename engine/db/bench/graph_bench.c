/*
 * graph_bench.c —— 图模态性能基准（v0.11 步骤 6，A6）
 *
 * 用法：
 *   graph_bench [节点数] [边数] [跳数] [轮数] [缓冲池帧数]
 *   默认：100000 1000000 3 1000 32768
 *
 * 测三件事：
 *   1. 装载吞吐（节点/秒、边/秒）
 *   2. depth 跳遍历的延迟分布（p50 / p90 / p99 / max）
 *   3. 与 PLAN §1.6 A6 的目标对比（3 跳 p99 < 10ms）
 *
 * ---------------- 【为什么这个基准在"模态层"而不是"公开 API 层"】 ----------------
 *
 * 因为装载的代价完全被 WAL 吃掉了：
 *
 *   加一条边要改 4 个页（边记录 / 原链头 / src / dst），
 *   每改一个页就是一条 WAL_PAGEDATA 整页镜像 = 8KB 日志。
 *   100 万条边 = 400 万条日志 ≈ **32 GB**。
 *
 * 那不是"慢一点"，那是磁盘放不下。
 *
 * 所以基准直接用 modalities/graph 的接口装载（不挂 WAL 钩子）——
 * 测的是**图引擎的遍历性能**，不是持久化路径的性能。
 * 持久化路径由 test/integration/test_graph_crash.c 单独验。
 *
 * 公开 API（db_graph_walk）在这条路径上只多两样东西：
 *   - 每跳一次 malloc + memcpy（把 Rid[] 转成 u64[]）
 *   - BFS 去重的哈希集
 * 量级上是几十微秒，相对 p99（毫秒级）可以忽略。
 *
 * ---------------- 【遍历模型】 ----------------
 *
 * 用的是 PLAN §1.5 的模型：
 *
 *   3 跳 = 3 轮 hop，每轮邻居数 × 每邻居 1 页读
 *   10 万节点 / 100 万边 = 平均出度 10
 *   → 3 跳最坏 1 + 10 + 100 + 1000 = 1111 次页读
 *
 * 这里按**不重复**的展开计数（和模型一致）。
 * 去重版（BFS）只读**更少**的页 —— 所以不去重是更保守的口径。
 *
 * ---------------- 【缓冲池大小为什么是参数】 ----------------
 *
 * 遍历的访问模式是"随机点查"，工作集比关系模态的顺序扫描大得多：
 * 1111 次页读散布在上万个页上。
 *
 * 缓冲池 256 帧（1MB，库的默认值）装不下 → 每次未命中都要从 OS 页缓存
 * 或磁盘读回来。这不是图的慢，是**工作集装不下**。
 *
 * 所以帧数是参数：报告里会同时给出池大小和图的页数，
 * 让数字可解释（缓冲池装得下 vs 装不下，是两个完全不同的问题）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "modalities/graph/graph.h"
#include "storage/pager.h"

/* ---------------- 计时 ---------------- */

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/* ---------------- 随机数 ---------------- */

/*
 * xorshift64*：固定种子 → 每次跑出来的图一样。
 *
 * 【为什么要固定种子】
 *
 * 基准的可比性要求"同样的图"上比较。
 * 随机图每次都不一样，p99 的波动就分不清是代码变了还是图变了。
 */
static u64 rng_state = 0x9E3779B97F4A7C15ull;

static void rng_seed(u64 s)
{
    rng_state = s ? s : 1;
}

static u64 rng_next(void)
{
    u64 x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static u32 rng_below(u32 n)
{
    return (u32)(rng_next() % n);
}

/* ---------------- 遍历 ---------------- */

/*
 * 一次 depth 跳的展开。
 *
 * 【为什么用邻接链游标而不是 graph_out_neighbors】
 *
 * 两者读的是同一批边记录，但游标是**增量**的：
 * 一次给一条边，不用先把整条链的结果攒进数组。
 * 基准要的就是"摊销到每条边"的成本。
 *
 * 返回展开的边数（= 页读次数的近似）。
 */
typedef struct {
    Rid  *v;
    u32   n;
    u32   cap;
} RidBuf;

static int rb_push(RidBuf *b, Rid r)
{
    if (b->n == b->cap) {
        u32 ncap = b->cap ? b->cap * 2 : 256;
        Rid *nv = (Rid *)realloc(b->v, (size_t)ncap * sizeof(Rid));
        if (!nv)
            return E_IO;
        b->v = nv;
        b->cap = ncap;
    }
    b->v[b->n++] = r;
    return OK;
}

static void rb_free(RidBuf *b)
{
    free(b->v);
    b->v = NULL;
    b->n = 0;
    b->cap = 0;
}

/* 一次展开的上限（防止长尾节点把内存吃光） */
#define EXPAND_CAP 2000000u

/*
 * 从 start 出发，展开 depth 层出边。
 * 返回展开的边总数（读了多少条边记录）。
 */
static u64 expand(Graph *g, Rid start, u32 depth, int *hit_cap)
{
    RidBuf frontier = { NULL, 0, 0 };
    RidBuf next = { NULL, 0, 0 };
    u64 edges = 0;
    u64 total = 0;

    *hit_cap = 0;
    rb_push(&frontier, start);

    for (u32 d = 0; d < depth; d++) {
        next.n = 0;

        for (u32 i = 0; i < frontier.n; i++) {
            GraphEdgeCursor c;
            if (graph_edge_cursor_open(g, frontier.v[i], true, &c) != OK)
                continue;

            for (;;) {
                Rid e, s, dst;
                int rc = graph_edge_cursor_next(g, &c, &e, &s, &dst);
                if (rc != OK)
                    break;
                edges++;
                total++;
                if (total > EXPAND_CAP) {
                    *hit_cap = 1;
                    goto done;
                }
                if (rb_push(&next, dst) != OK)
                    goto done;
            }
        }

        /* 下一层 = 本层展开出来的邻居 */
        RidBuf t = frontier;
        frontier = next;
        next = t;

        if (frontier.n == 0)
            break;
    }

done:
    rb_free(&frontier);
    rb_free(&next);
    return edges;
}

/* ---------------- 统计 ---------------- */

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* 分位数：p 取 0..1 */
static double percentile(const double *sorted, u32 n, double p)
{
    if (n == 0)
        return 0;
    u32 idx = (u32)(p * (double)(n - 1));
    if (idx >= n)
        idx = n - 1;
    return sorted[idx];
}

/* ---------------- 主流程 ---------------- */

int main(int argc, char **argv)
{
    u32 n_nodes = (argc > 1) ? (u32)atoi(argv[1]) : 100000;
    u32 n_edges = (argc > 2) ? (u32)atoi(argv[2]) : 1000000;
    u32 depth   = (argc > 3) ? (u32)atoi(argv[3]) : 3;
    u32 rounds  = (argc > 4) ? (u32)atoi(argv[4]) : 1000;
    u32 frames  = (argc > 5) ? (u32)atoi(argv[5]) : 32768;

    const char *path = "test/tmp/graph_bench.db";

    /* 固定种子：每次跑的是同一张图，跨版本可比 */
    rng_seed(0x1234567890ABCDEFull);

    printf("图模态性能基准\n");
    printf("  规模：%u 节点 / %u 边 / %u 跳 / %u 轮 / 缓冲池 %u 帧 (%.1f MB)\n",
           n_nodes, n_edges, depth, rounds, frames,
           (double)frames * PAGE_SIZE / (1024.0 * 1024.0));

    remove(path);
    remove("test/tmp/graph_bench.db-wal");

    Pager *pg = NULL;
    if (pager_open(path, frames, &pg) != OK) {
        fprintf(stderr, "打开 pager 失败\n");
        return 1;
    }

    /* 两个属性列：INT4 + 定长 TEXT(16) */
    GraphCol cols[2];
    memset(cols, 0, sizeof(cols));
    strcpy(cols[0].name, "id");
    cols[0].type = T_INT4;
    strcpy(cols[1].name, "name");
    cols[1].type = T_TEXT;
    cols[1].width = 16;

    Graph *g = NULL;
    double t0 = now_ms();
    if (graph_create(pg, NULL, NULL, cols, 2, &g) != OK) {
        fprintf(stderr, "建图失败\n");
        return 1;
    }

    Rid *ids = (Rid *)malloc((size_t)n_nodes * sizeof(Rid));
    if (!ids) {
        fprintf(stderr, "内存不足\n");
        return 1;
    }

    DbValue vals[2];
    char nm[16];
    for (u32 i = 0; i < n_nodes; i++) {
        val_int(&vals[0], (i64)i);
        vals[0].type = T_INT4;
        snprintf(nm, sizeof(nm), "n%u", i);
        val_text(&vals[1], nm, (u16)strlen(nm));
        vals[1].type = T_TEXT;

        if (graph_create_node(g, 0, vals, 2, &ids[i]) != OK) {
            fprintf(stderr, "建节点 %u 失败\n", i);
            return 1;
        }
    }
    double t1 = now_ms();

    for (u32 i = 0; i < n_edges; i++) {
        Rid src = ids[rng_below(n_nodes)];
        Rid dst = ids[rng_below(n_nodes)];
        Rid e;
        if (graph_add_edge(g, 0, src, dst, &e) != OK) {
            fprintf(stderr, "加边 %u 失败\n", i);
            return 1;
        }
    }
    double t2 = now_ms();

    printf("\n【装载】\n");
    printf("  节点：%u 个，%.0f ms（%.0f 个/秒）\n",
           n_nodes, t1 - t0, n_nodes / ((t1 - t0) / 1000.0));
    printf("  边：  %u 条，%.0f ms（%.0f 条/秒）\n",
           n_edges, t2 - t1, n_edges / ((t2 - t1) / 1000.0));
    printf("  平均出度：%.1f\n", (double)n_edges / (double)n_nodes);
    printf("  图占 %u 个页（节点 %u + 边 %u + schema 1）\n",
           gstore_page_count(g->nodes) + gstore_page_count(g->edges) + 1,
           gstore_page_count(g->nodes), gstore_page_count(g->edges));
    printf("  数据量约 %.1f MB\n",
           ((double)n_nodes * 41.0 + (double)n_edges * 52.0) / (1024.0 * 1024.0));

    /* ---------------- 预热 ---------------- */

    printf("\n【预热】\n");
    double t3 = now_ms();
    u64 warm_edges = 0;
    for (u32 i = 0; i < 200; i++) {
        int cap = 0;
        warm_edges += expand(g, ids[rng_below(n_nodes)], depth, &cap);
    }
    double t4 = now_ms();
    printf("  200 次 %u 跳：%.0f ms（读了 %llu 条边）\n",
           depth, t4 - t3, (unsigned long long)warm_edges);

    /* ---------------- 测量 ---------------- */

    printf("\n【测量】%u 次随机起点的 %u 跳\n", rounds, depth);

    double *samples = (double *)malloc((size_t)rounds * sizeof(double));
    if (!samples) {
        fprintf(stderr, "内存不足\n");
        return 1;
    }

    int capped = 0;
    u64 total_edges_read = 0;
    double t5 = now_ms();
    for (u32 i = 0; i < rounds; i++) {
        Rid start = ids[rng_below(n_nodes)];
        int cap = 0;

        double s = now_ms();
        u64 e = expand(g, start, depth, &cap);
        samples[i] = now_ms() - s;

        total_edges_read += e;
        if (cap)
            capped++;
    }
    double t6 = now_ms();

    qsort(samples, rounds, sizeof(double), cmp_double);

    double p50 = percentile(samples, rounds, 0.50);
    double p90 = percentile(samples, rounds, 0.90);
    double p99 = percentile(samples, rounds, 0.99);
    double pmax = samples[rounds - 1];
    double avg = 0;
    for (u32 i = 0; i < rounds; i++)
        avg += samples[i];
    avg /= rounds;

    printf("  p50    %8.3f ms\n", p50);
    printf("  p90    %8.3f ms\n", p90);
    printf("  p99    %8.3f ms\n", p99);
    printf("  max    %8.3f ms\n", pmax);
    printf("  平均   %8.3f ms\n", avg);
    printf("  每次平均读 %llu 条边\n", (unsigned long long)(total_edges_read / rounds));
    printf("  吞吐 %0.0f 次遍历/秒\n", rounds / ((t6 - t5) / 1000.0));
    if (capped)
        printf("  （%d 次撞到展开上限 %u —— 长尾节点）\n", capped, EXPAND_CAP);

    /* ---------------- 结论 ---------------- */

    printf("\n【结论】PLAN §1.6 A6：%u 跳 p99 < 10ms —— ", depth);
    if (p99 < 10.0)
        printf("达标（p99 = %.3f ms）\n", p99);
    else
        printf("**未达标**（p99 = %.3f ms，是目标的 %.1f 倍）\n",
               p99, p99 / 10.0);

    free(samples);
    free(ids);
    graph_destroy(g);
    pager_close(pg);

    remove(path);
    remove("test/tmp/graph_bench.db-wal");
    return 0;
}
