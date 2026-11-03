/*
 * test_ft_crash.c —— 全文检索模态崩溃恢复验收（v1.3 步骤 5，A7）
 *
 * "断电"用 db_abandon 模拟：不刷脏页、不写 CHECKPOINT，直接丢句柄。
 * 等价于进程被 kill -9 —— 日志已 fsync，页还在内存里没落盘。
 *
 * 4 项：
 *   1. 灌文档 → 断电 → 重开：段数、文档数、tombstone 数全对
 *   2. 断电后：检索结果与断电前**逐位一致**（doc_id 与 score 都用 == 比）
 *   3. 断电后：被删的文档**没有复活**，也没多出别的文档
 *   4. 合并 → 断电 → 重开：合并的结果也活下来（段目录 + 死段判定）
 *
 * 【为什么第 2 项最难，也最值钱】
 *
 * 段数和文档数只是**计数**。
 *
 * 检索结果是**内容**：分数是 BM25 算出来的，
 * 而 BM25 的 idf / avgdl 依赖"全局统计量"——
 * 这些统计量是**逐段**记在段头页里的。
 *
 * 如果恢复时找回来的段**少了一个**或者**多了几个**，
 * 平均文档长度就变了 → 所有文档的分数全变了 →
 * 而结果集可能还是"看着差不多"的那几篇。
 *
 * 这就是"静默地错"：不报错、不崩，只是排序和分数悄悄不对了。
 * 所以第 2 项把断电前的完整结果集（含分数）存下来，
 * 断电后重算，**逐个用 == 比**（不用容差 ——
 * 容差会把"分数差了 1e-9"这种真问题也放过）。
 *
 * 【为什么第 3 项是本步骤的核心命题】
 *
 * 删掉一篇文档 = 打一个 tombstone。
 *
 * 坑 B 说得很直白：**只靠 WAL 重放是不够的** ——
 * checkpoint 会把日志截断，日志里的 tombstone 记录就没了，
 * 而"这篇文档被删了"这件事必须活过 checkpoint。
 *
 * 所以 tombstone 集合**自己也落盘**（专门的 tombstone 页），
 * 恢复时取"页里的内容 ∪ (WAL 重放看到的 − WAL 回滚撤销的)"。
 * 这一项就是盯着这个集合的：断电后那篇文档必须还是删着的。
 *
 * 【为什么第 4 项要单独测】
 *
 * 合并会在段头页里改"状态"（live → dead），
 * 而段目录**没有独立的目录页** —— 状态就记在段头页里（坑 A）。
 * 所以合并完断电，恢复时必须能分辨"哪些段还活着"：
 * 判据是"段头页里状态是 live，且它指向的新段确实存在"。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "db.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/ft_crash.db"
#define PATH2 "test/tmp/ft_crash2.db"

#define N_DOCS   24        /* 灌 24 篇 → 24 个段（超过合并阈值 8） */
#define DEL_DOC  7         /* 被删掉的那一篇 */
#define TOP_K    N_DOCS

/* ---------------- 语料 ---------------- */

/*
 * 每篇文档都由"公共词 + 私有词"组成：
 *   - 公共词让检索有多篇命中（分数才有多样性）；
 *   - 私有词让每篇文档的 dl / 词表都不一样。
 */
static char text_buf[N_DOCS][256];

static void build_corpus(void)
{
    static const char *common[] = {
        "alpha", "beta", "gamma", "delta", "epsilon",
        "zeta", "eta", "theta"
    };
    int n_common = (int)(sizeof(common) / sizeof(common[0]));

    for (int i = 0; i < N_DOCS; i++) {
        char *p = text_buf[i];
        size_t left = sizeof(text_buf[i]);

        p[0] = '\0';
        for (int k = 0; k < n_common; k++) {
            int n = snprintf(p, left, "%s ", common[k]);
            if (n > 0) { p += n; left -= (size_t)n; }
        }
        /* 私有词：doc0007 这样的词被分词器切成 doc0007 */
        snprintf(p, left, "doc%04d", i);
    }
}

/* ---------------- 检索结果快照 ---------------- */

static db_ft_hit snap_before[TOP_K];
static uint32_t   snap_n;

static void snapshot(DB *db)
{
    db_ft_hit out[TOP_K];
    uint32_t n = 0;

    memset(out, 0, sizeof(out));
    if (db_ft_search(db, "alpha gamma delta", TOP_K, out, TOP_K, &n) != DB_OK) {
        printf("    FAIL 检索失败\n");
        failures++;
        snap_n = 0;
        return;
    }
    snap_n = n;
    memcpy(snap_before, out, sizeof(db_ft_hit) * (size_t)n);
}

/* 断电后重算，与断电前逐个比（doc_id 与 score 都用 ==） */
static void compare(DB *db, const char *tag)
{
    db_ft_hit out[TOP_K];
    uint32_t n = 0;

    memset(out, 0, sizeof(out));
    if (db_ft_search(db, "alpha gamma delta", TOP_K, out, TOP_K, &n) != DB_OK) {
        printf("    FAIL %s：检索失败\n", tag);
        failures++;
        return;
    }
    if (n != snap_n) {
        printf("    FAIL %s：命中数变了：断电前 %u，断电后 %u\n", tag, snap_n, n);
        failures++;
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (out[i].doc_id != snap_before[i].doc_id) {
            printf("    FAIL %s：第 %u 个结果的 doc_id 变了：%u → %u\n",
                   tag, i, snap_before[i].doc_id, out[i].doc_id);
            failures++;
            return;
        }
        if (out[i].score != snap_before[i].score) {
            printf("    FAIL %s：第 %u 个结果（doc %u）的分数变了：%.17g → %.17g\n",
                   tag, i, out[i].doc_id,
                   snap_before[i].score, out[i].score);
            failures++;
            return;
        }
    }
}

/* 结果里有没有这篇文档 */
static bool contains(DB *db, uint32_t doc_id)
{
    db_ft_hit out[TOP_K];
    uint32_t n = 0;

    if (db_ft_search(db, "alpha gamma delta", TOP_K, out, TOP_K, &n) != DB_OK) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (out[i].doc_id == doc_id)
            return true;
    }
    return false;
}

/* ---------------- 1~3) 灌文档 → 断电 → 重开 ---------------- */

static void t1_crash_once(void)
{
    uint32_t nsegs_before = 0;
    uint32_t ndocs_before = 0;
    uint32_t ntomb_before = 0;

    printf("  1~3) 灌文档 → 删一篇 → 断电 → 重开：计数 / 结果逐位一致 / 删掉的没复活\n");

    remove(PATH);
    remove("test/tmp/ft_crash.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    if (!db)
        return;

    for (int i = 0; i < N_DOCS; i++) {
        if (db_ft_add(db, (uint32_t)i, text_buf[i], (uint32_t)strlen(text_buf[i])) != DB_OK) {
            printf("    FAIL 灌文档 %d 失败\n", i);
            failures++;
            db_abandon(db);
            return;
        }
    }

    /* 删掉一篇 */
    CHECK(db_ft_delete(db, DEL_DOC) == DB_OK);

    nsegs_before = db_ft_nsegs(db);
    ndocs_before = db_ft_ndocs(db);
    ntomb_before = db_ft_ntombstones(db);

    CHECK(nsegs_before == N_DOCS);
    CHECK(ndocs_before == N_DOCS);
    CHECK(ntomb_before == 1);

    /* 断电前的结果（被删的那篇不该在里面） */
    snapshot(db);
    CHECK(snap_n == N_DOCS - 1);
    CHECK(!contains(db, DEL_DOC));

    /* 断电：不刷脏页、不写 CHECKPOINT */
    db_abandon(db);
    db = NULL;

    /* 重开 → 恢复重放 → 扫页空间找段 → 恢复 tombstone 集合 */
    CHECK(db_open(PATH, &db) == DB_OK);
    if (!db)
        return;

    /* 1) 计数 */
    if (db_ft_nsegs(db) != nsegs_before) {
        printf("    FAIL 段数变了：断电前 %u，断电后 %u\n",
               nsegs_before, db_ft_nsegs(db));
        failures++;
    }
    if (db_ft_ndocs(db) != ndocs_before) {
        printf("    FAIL 文档数变了：断电前 %u，断电后 %u\n",
               ndocs_before, db_ft_ndocs(db));
        failures++;
    }
    if (db_ft_ntombstones(db) != ntomb_before) {
        printf("    FAIL tombstone 数变了：断电前 %u，断电后 %u\n",
               ntomb_before, db_ft_ntombstones(db));
        failures++;
    }

    /* 2) 结果逐位一致 */
    compare(db, "断电后");

    /* 3) 被删的文档没有复活（坑 B 的核心命题） */
    if (contains(db, DEL_DOC)) {
        printf("    FAIL 被删的文档 %u 复活了\n", DEL_DOC);
        failures++;
    }

    db_close(db);
    remove(PATH);
    remove("test/tmp/ft_crash.db-wal");
}

/* ---------------- 4) 合并 → 断电 → 重开 ---------------- */

/*
 * 合并的窗口是"相邻窗口里文档数之和最小的那个" ——
 * 每段一篇文档时，合并一次收掉 2 段（和为 2 最小）。
 * 所以要一直合到"合不动了"（merged_out == 0）。
 */
static void t4_merge_crash(void)
{
    uint32_t nsegs_before = 0;
    uint32_t ndocs_before = 0;

    printf("  4) 合并到只剩一个段 → 断电 → 重开：段目录与死段判定都对\n");

    remove(PATH2);
    remove("test/tmp/ft_crash2.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH2, &db) == DB_OK);
    if (!db)
        return;

    for (int i = 0; i < N_DOCS; i++) {
        if (db_ft_add(db, (uint32_t)i, text_buf[i], (uint32_t)strlen(text_buf[i])) != DB_OK) {
            printf("    FAIL 灌文档 %d 失败\n", i);
            failures++;
            db_abandon(db);
            return;
        }
    }
    CHECK(db_ft_delete(db, DEL_DOC) == DB_OK);

    /* 一直合并，直到合不动 */
    for (int round = 0; round < 64; round++) {
        uint32_t merged = 0;

        if (db_ft_merge(db, &merged) != DB_OK) {
            printf("    FAIL 合并失败（第 %d 轮）\n", round);
            failures++;
            break;
        }
        if (merged == 0)
            break;      /* 合不动了 */
    }

    nsegs_before = db_ft_nsegs(db);
    ndocs_before = db_ft_ndocs(db);

    /*
     * 合并到"合不动"为止 —— 段数降到合并阈值（8）时内核就不再合了
     * （段数太少，合出来的收益抵不上代价）。
     * 所以这里的段数是"阈值附近"，不是 1。
     */
    CHECK(nsegs_before > 1 && nsegs_before <= 8);
    /* 被删的那篇在合并时被物理丢弃 → 文档数少一篇 */
    CHECK(ndocs_before == N_DOCS - 1);

    snapshot(db);
    CHECK(snap_n == N_DOCS - 1);

    /* 断电 */
    db_abandon(db);
    db = NULL;

    CHECK(db_open(PATH2, &db) == DB_OK);
    if (!db)
        return;

    /* 死段不能被算成活段 —— 否则文档数会翻好几倍 */
    if (db_ft_nsegs(db) != nsegs_before) {
        printf("    FAIL 合并后段数变了：断电前 %u，断电后 %u\n",
               nsegs_before, db_ft_nsegs(db));
        failures++;
    }
    if (db_ft_ndocs(db) != ndocs_before) {
        printf("    FAIL 合并后文档数变了：断电前 %u，断电后 %u\n",
               ndocs_before, db_ft_ndocs(db));
        failures++;
    }

    compare(db, "合并后断电");
    if (contains(db, DEL_DOC)) {
        printf("    FAIL 合并后断电，被删的文档 %u 复活了\n", DEL_DOC);
        failures++;
    }

    db_close(db);
    remove(PATH2);
    remove("test/tmp/ft_crash2.db-wal");
}

int main(void)
{
    printf("test_ft_crash:\n");

    build_corpus();

    t1_crash_once();
    t4_merge_crash();

    remove(PATH);
    remove("test/tmp/ft_crash.db-wal");
    remove(PATH2);
    remove("test/tmp/ft_crash2.db-wal");

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  4 项验收全部通过\n");
    return 0;
}
