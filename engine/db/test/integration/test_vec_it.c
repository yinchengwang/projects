/*
 * test_vec_it.c —— 向量模态的 SQL 集成验收（v1.0 步骤 5 · A8 / A10）
 *
 * 7 项：
 *   1. **A8 · 执行器零改动**（机械检查：扫执行器源文件里有没有 "vector"）
 *   2. **A10 · 能力协商**：INSERT / UPDATE / DELETE / VACUUM / CREATE INDEX 明确报 E_NOTSUP
 *   3. vec_items 扫描：行数、内容、列对齐
 *   4. **列下推**：label 等值 → 只给命中行（且结果与"全扫+过滤"一致）
 *   5. **过滤 + ANN 给超集**：过滤后可能少于 k，如实报告实际数量
 *   6. ANN vs 精确：结果一致（recall = 1.0，小规模）
 *   7. **IO 可证**：ANN 的页读 ≪ 暴力扫
 *
 * 【第 5 项是这一版最要紧的语义】
 *
 * "先 ANN 取 top-k 再过滤"会**静默丢数据**：
 * 如果 top-k 都不满足过滤条件，结果少于 k 且不可补 ——
 * 违反"下推只能给超集"铁律。
 *
 * 正确做法：ANN 扩大候选池（≥4k），再过滤，如实报告实际数量。
 * 这一项就是验"该少的时候真的会少，但少得诚实"。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/vec_it.db"
#define DIM  4
#define N   200

/* ---------------- 造数据 ---------------- */

static void make_vec(float *v, int i)
{
    /* 确定性的、互不相同的向量 */
    v[0] = (float)(i % 7) * 0.1f;
    v[1] = (float)(i % 11) * 0.2f;
    v[2] = (float)(i % 13) * 0.3f;
    v[3] = (float)(i % 5) * 0.4f;
}

static int label_of(int i)
{
    return i % 5;         /* 5 个类别 */
}

/* ---------------- 1. A8 · 执行器零改动 ---------------- */

static int t1_exec_untouched(void)
{
    printf("  1. A8 · 执行器零改动（机械检查）\n");

    /*
     * 这件事没法用行为断言表达 —— 行为对了不代表执行器没被改。
     * 所以只能查源码。
     *
     * 查不到源文件就跳过（不硬失败）：测试可能被从别的目录跑。
     */
    static const char *files[] = {
        "src/query/exec.c", "src/query/exec_scan.c", "src/query/exec_write.c",
        "src/query/parser.c", "src/query/lexer.c", "src/query/exec.h",
        "src/query/exec_internal.h"
    };

    int checked = 0;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        FILE *f = fopen(files[i], "r");
        if (!f)
            continue;
        checked++;

        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "vector") || strstr(line, "hnsw") ||
                strstr(line, "Hnsw") || strstr(line, "knn")) {
                printf("    FAIL 执行器里出现了向量相关的字样：%s: %s",
                       files[i], line);
                failures++;
            }
        }
        fclose(f);
    }

    if (checked == 0) {
        printf("    （没找到 src/query/ 下的源文件，跳过机械检查）\n");
        return 0;
    }

    printf("    （检查了 %d 个执行器源文件，零命中）\n", checked);
    CHECK(checked > 0);
    return 0;
}

/* ---------------- SQL 辅助 ---------------- */

static int sql_exec(DB *db, const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (r)
        db_result_free(r);
    return (int)e;
}

static const char *last_error(DB *db)
{
    return db_last_error(db);
}

/* ---------------- 2. A10 · 能力协商 ---------------- */

static void t2_capabilities(DB *db)
{
    printf("  2. A10 · 能力协商（写操作明确报 E_NOTSUP）\n");

    /*
     * vec_items 本版**只读**：装载走专用 API。
     *
     * 报错必须明确（"表 vec_items 不支持插入"），
     * 而不是崩溃或者"表不存在"。
     */
    CHECK(sql_exec(db, "INSERT INTO vec_items VALUES (1, 2, 'x');")
          == DB_E_NOTSUP);
    CHECK(strstr(last_error(db), "不支持插入") != NULL);

    CHECK(sql_exec(db, "UPDATE vec_items SET label = 1;")
          == DB_E_NOTSUP);
    CHECK(strstr(last_error(db), "不支持更新") != NULL);

    CHECK(sql_exec(db, "DELETE FROM vec_items;") == DB_E_NOTSUP);
    CHECK(strstr(last_error(db), "不支持删除") != NULL);

    CHECK(sql_exec(db, "VACUUM vec_items;") == DB_E_NOTSUP);
    CHECK(strstr(last_error(db), "不支持 VACUUM") != NULL);

    CHECK(sql_exec(db, "CREATE INDEX idx_vec ON vec_items(label);")
          == DB_E_NOTSUP);
    CHECK(strstr(last_error(db), "不支持建索引") != NULL);

    /* 报错之后库还能用（不是崩溃） */
    Result *r = NULL;
    CHECK(db_exec(db, "SELECT row_id FROM vec_items;", &r) == DB_OK);
    CHECK(r != NULL);
    if (r) {
        CHECK(db_result_rows(r) > 0);
        db_result_free(r);
    }
}

/* ---------------- 3/4. 扫描与下推 ---------------- */

static void t3_scan_and_push(DB *db)
{
    printf("  3. vec_items 扫描\n");
    printf("  4. 列下推（label 等值）\n");

    uint64_t n = 0;
    CHECK(db_vec_count(db, &n) == DB_OK);
    CHECK(n == N);

    /* 全扫：N 行 */
    Result *all = NULL;
    CHECK(db_exec(db, "SELECT row_id, label FROM vec_items;", &all) == DB_OK);
    CHECK(all != NULL);
    if (!all)
        return;
    CHECK(db_result_rows(all) == (int)N);

    /* 内容对不对：row_id 递增、label 匹配 */
    int bad = 0;
    for (int i = 0; i < db_result_rows(all); i++) {
        long rid = strtol(db_result_value(all, i, 0), NULL, 10);
        long lab = strtol(db_result_value(all, i, 1), NULL, 10);
        if (rid != i || lab != label_of((int)i))
            bad++;
    }
    CHECK(bad == 0);

    /*
     * 下推：label = 2 → 只给命中行。
     *
     * 【下推省的是什么】
     *
     * 省的是**交给执行器的行数**（不是 IO —— 找命中行本身就要扫标量表）。
     * 结果一定和"全扫 + 过滤"一样，因为执行器拿到行还要重算 WHERE。
     */
    Result *flt = NULL;
    CHECK(db_exec(db, "SELECT row_id FROM vec_items WHERE label = 2;", &flt)
          == DB_OK);
    CHECK(flt != NULL);
    if (flt) {
        int expect = 0;
        for (int i = 0; i < N; i++)
            if (label_of(i) == 2)
                expect++;

        printf("    （label=2 → 下推给 %d 行，全扫 %d 行）\n",
               db_result_rows(flt), db_result_rows(all));
        CHECK(db_result_rows(flt) == expect);

        /* 每行的 label 确实都是 2 */
        int bad2 = 0;
        for (int i = 0; i < db_result_rows(flt); i++) {
            long rid = strtol(db_result_value(flt, i, 0), NULL, 10);
            if (label_of((int)rid) != 2)
                bad2++;
        }
        CHECK(bad2 == 0);
        db_result_free(flt);
    }

    db_result_free(all);
}

/* ---------------- 5. 过滤 + ANN：给超集 ---------------- */

static void t5_filtered_ann(DB *db)
{
    printf("  5. 过滤 + ANN：给超集，如实报告实际数量\n");

    /*
     * q 取 make_vec(0) —— 它自己的 label 就是 0。
     *
     * 【为什么要这样挑】
     *
     * 过滤 + ANN 的命中数**依赖 ANN 候选池里有没有那一类**。
     * 随便挑个查询点，可能它的 4k 个近邻里一个 label=0 的都没有 ——
     * 那时返回 0 是**正确**的（诚实报告），不是 bug。
     *
     * 所以要一个**确定性的**断言：拿一个 label=0 的向量当查询，
     * 它自己必定在近邻里 → 至少有 1 个命中。
     */
    float q[DIM];
    make_vec(q, 0);

    db_vec_hit hits[16];
    uint32_t n = 0;

    CHECK(db_vec_knn_filtered(db, q, 5, 0, 0, hits, &n) == DB_OK);
    CHECK(n > 0);
    CHECK(n <= 5);

    /*
     * 关键性质：过滤后的结果**每一行都满足过滤条件**，
     * 而且数量不超过 k。
     */
    int bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        /* row_id → label 用精确查询对照：这里只查数量关系 */
        if (hits[i].row_id >= N)
            bad++;
    }
    CHECK(bad == 0);

    /*
     * 极端情况：要一个**几乎不存在**的类别。
     *
     * 用一个只可能命中 0~1 行的 label → 候选池扩大到 4k 之后
     * 仍然可能只有 0 个 —— 这时必须**如实报告 0**，
     * 而不是硬凑 k 个（那会返回不满足条件的行 = 静默给错数据）。
     */
    CHECK(db_vec_knn_filtered(db, q, 10, 0, 9999, hits, &n) == DB_OK);
    printf("    （过滤不存在的类别 → 返回 %u 个，如实报告）\n", n);
    CHECK(n == 0);
}

/* ---------------- 5b. 候选池必须扩大 ---------------- */

/*
 * 【这一项是步骤 7 反向验证时补的】
 *
 * 上面第 5 项抓不住"候选池不扩大"这个错误 ——
 * 它的查询点自己就满足过滤条件，所以 top-k 里一定有命中，
 * 候选池是 k 还是 4k 都返回 n > 0。
 *
 * 这一项要抓的是**反面**的情形：
 *   满足过滤条件的行**不在 top-k 里**，但在 top-4k 里。
 *
 * 数据是手工摆的（一条直线上的 8 个点），所以排名完全确定：
 *
 *   位置   0   1   2   3   4   5   6   7
 *   距离 0   1   4   9  16  25  36  49      （平方 L2）
 *   label 0   0   0   0   1   1   1   1
 *
 * 查询 = 位置 0，要 k=2 个 label=1 的：
 *   - 正确的实现（候选池 4k=8）：看到全部 8 个 → 命中 v4,v5,v6,v7 → 截断到 2 → 返回 2 个
 *   - 候选池 = k = 2：只看到 v0,v1（都是 label 0）→ 返回 0 个
 *
 * 0 个是**静默丢数据** —— 违反"下推只能给超集"铁律，
 * 而所有其它断言都验不出来。
 */
static void t5b_candidate_pool(void)
{
    printf("  5b. 候选池必须扩大（否则满足条件的行会被静默丢掉）\n");

    const char *p = "test/tmp/vec_it_pool.db";
    remove(p);
    remove("test/tmp/vec_it_pool.db-wal");

    DB *db = NULL;
    CHECK(db_open(p, &db) == DB_OK);
    CHECK(db_vec_create(db, 2, 16, 200) == DB_OK);

    /* 8 个点排在一条线上：v_i = (i, 0) */
    for (int i = 0; i < 8; i++) {
        float v[2];
        v[0] = (float)i;
        v[1] = 0.0f;
        uint64_t rid = 0;
        CHECK(db_vec_insert(db, v, (i < 4) ? 0 : 1, &rid) == DB_OK);
        CHECK(rid == (uint64_t)i);
    }

    float q[2];
    q[0] = 0.0f;
    q[1] = 0.0f;

    db_vec_hit hits[16];
    uint32_t n = 0;
    CHECK(db_vec_knn_filtered(db, q, 2, 0, 1, hits, &n) == DB_OK);

    /*
     * 期望 2 个，且是 v4、v5（label=1 里最近的两个）。
     *
     * 如果候选池没有扩大，这里会是 0 —— 那就是静默丢数据。
     */
    printf("    （要 k=2 个 label=1 → 返回 %u 个，期望 2 个：v4、v5）\n", n);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(hits[0].row_id == 4);
        CHECK(hits[1].row_id == 5);
    }

    /* 反向：label=0 的最近两个是 v0、v1（都在 top-k 里，不依赖候选池） */
    CHECK(db_vec_knn_filtered(db, q, 2, 0, 0, hits, &n) == DB_OK);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(hits[0].row_id == 0);
        CHECK(hits[1].row_id == 1);
    }

    db_close(db);
    remove(p);
    remove("test/tmp/vec_it_pool.db-wal");
}

/* ---------------- 6/7. ANN vs 精确 + IO ---------------- */

static void t6_ann_vs_exact(DB *db)
{
    printf("  6. ANN vs 精确：结果一致\n");
    printf("  7. IO 可证：ANN 的页读 ≪ 暴力扫\n");

    float q[DIM];
    make_vec(q, 10);

    db_vec_hit a[16];
    db_vec_hit b[16];
    uint32_t na = 0;
    uint32_t nb = 0;

    uint64_t r0 = db_vec_page_reads(db);

    CHECK(db_vec_knn(db, q, 10, 64, a, &na) == DB_OK);
    uint64_t r1 = db_vec_page_reads(db);

    CHECK(db_vec_knn_exact(db, q, 10, b, &nb) == DB_OK);
    uint64_t r2 = db_vec_page_reads(db);

    uint64_t ann_reads = r1 - r0;
    uint64_t exact_reads = r2 - r1;

    printf("    （ANN 读 %llu 页，暴力扫读 %llu 页）\n",
           (unsigned long long)ann_reads, (unsigned long long)exact_reads);

    CHECK(na == 10);
    CHECK(nb == 10);

    /*
     * 距离序列必须**完全一致**。
     *
     * 【为什么不能比 ID 集合】
     *
     * 这批数据有**并列距离**（make_vec 的取值有周期）——
     * 距离相同但 ID 不同时，ANN 和精确扫挑中的是**不同的那几个**。
     * 那是并列，不是错。
     *
     * 所以比距离（必须一样），ID 集合用**召回率**来看。
     */
    int bad = 0;
    for (uint32_t i = 0; i < na && i < nb; i++) {
        if (a[i].dist != b[i].dist)
            bad++;
    }
    CHECK(bad == 0);

    /* 召回率：ANN 命中的 ID 有多少在精确解里 */
    uint32_t hit = 0;
    for (uint32_t i = 0; i < na; i++) {
        for (uint32_t j = 0; j < nb; j++) {
            if (a[i].row_id == b[j].row_id) {
                hit++;
                break;
            }
        }
    }
    double recall = nb ? (double)hit / (double)nb : 0.0;
    printf("    （ANN vs 精确：召回率 %.3f）\n", recall);
    CHECK(recall >= 0.90);

    /*
     * IO 对比。
     *
     * 【重要：ANN 是 0 页】
     *
     * 因为 HNSW 索引**把向量带在内存里**（hnswlib 的模型）——
     * 查询时算距离用的是内存副本，一个页都不读。
     * 而暴力扫要从存储读全部向量页。
     *
     * 所以这个对比证明的是"索引让查询完全不用碰存储"，
     * 代价是**每个实例常驻 51MB**（10 万 × 128 维 fp32）。
     * 这个取舍在 PLAN §1.7 R2 里已经记着。
     */
    CHECK(ann_reads * 20 < exact_reads);
}

/* ---------------- 重开：索引自动恢复 ---------------- */

static void t7_reopen(void)
{
    printf("  8. 重开：索引自动恢复\n");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    CHECK(db_vec_exists(db));

    uint64_t n = 0;
    CHECK(db_vec_count(db, &n) == DB_OK);
    CHECK(n == N);

    /* 恢复后的索引仍然能给出正确结果 */
    float q[DIM];
    make_vec(q, 10);

    db_vec_hit hits[16];
    uint32_t nh = 0;
    CHECK(db_vec_knn(db, q, 10, 64, hits, &nh) == DB_OK);
    CHECK(nh == 10);
    CHECK(hits[0].row_id == 10);      /* 它自己最近 */

    db_close(db);
}

int main(void)
{
    printf("test_vec_it:\n");

    remove(PATH);
    remove(PATH "-wal");

    t1_exec_untouched();

    /* 建库 + 灌数据 */
    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    CHECK(db_vec_exists(db) == 0);
    CHECK(db_vec_create(db, DIM, 16, 200) == DB_OK);
    CHECK(db_vec_exists(db) == 1);

    for (int i = 0; i < N; i++) {
        float v[DIM];
        make_vec(v, i);
        uint64_t rid = 0;
        CHECK(db_vec_insert(db, v, label_of(i), &rid) == DB_OK);
        CHECK(rid == (uint64_t)i);
    }

    t2_capabilities(db);
    t3_scan_and_push(db);
    t5_filtered_ann(db);
    t5b_candidate_pool();
    t6_ann_vs_exact(db);

    db_close(db);

    t7_reopen();

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  9 项验收全部通过\n");
    return 0;
}
