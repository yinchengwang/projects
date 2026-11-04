/*
 * test_ft_tomb_undo.c —— tombstone 撤销的崩溃恢复验收（v1.3 欠账 1 · 坑 M4-24）
 *
 * 【这个测试盯的是什么】
 *
 * "一篇文档被删了"这件事记在 tombstone 页上。
 * 删除分两种：
 *   - 已提交事务的删除 → 必须**留着**（那篇文档不能再被检索到）；
 *   - 未提交事务的删除 → 必须**撤销**（这个删除没发生过，文档照旧能检索到）。
 *
 * 老实现里 tombstone 页走**整页镜像**进 WAL，
 * 撤销 = 把整页盖回"改之前的样子"。
 *
 * 坑 M4-24：整页前像只是"某一时刻的快照"，
 * 撤销其中一条记录 = 把"那之后的所有改动"一起撤销掉。
 *
 *   同一页上，未提交事务 T2 先追加了 B，已提交事务 T1 后追加了 A，
 *   那 T2 那条记录的整页前像 = [B]（**不含** A），
 *   撤销 T2 时整页盖回 [B] → **A 被连带抹掉** → 已提交的删除"复活"。
 *
 * ==================== 两个场景，顺序不同 ====================
 *
 * 场景 1（先提交、后未提交）：
 *   T1 提交删 A → T2 未提交删 B → 断电 → 重开
 *   老实现这里**碰巧是对的**（T2 的前像里已经有 A），
 *   所以它是**回归护栏**：修好之后也不能坏。
 *
 * 场景 2（先未提交、后提交）：
 *   T2 未提交删 B → T1 提交删 A → 断电 → 重开
 *   这才是真正会复现坑 M4-24 的顺序（老实现这里是**红的**）。
 *
 * ==================== 为什么要多"checkpoint + 再开一次" ====================
 *
 * 断电重开之后，内存里的删除标记集合是
 * "页里的内容 ∪ (WAL 重放看到的 − WAL 回滚撤销的)"。
 *
 * 老实现在这一步看着还是对的（A 还在集合里），
 * 但**页已经空了** —— 两者对不上。
 *
 * 只有等 checkpoint 把日志截断掉、页成为唯一权威之后，
 * 这个不一致才会露出来：A 复活了。
 *
 * 所以每个场景都做三步断言：断电重开 → checkpoint → 再开一次。
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

/* 临时库文件：带 a1_ 前缀（本次改动的标记），用完删掉 */
#define PATH_A "test/tmp/a1_tomb_a.db"
#define PATH_B "test/tmp/a1_tomb_b.db"

#define N_DOCS  6
#define TOP_K   16
#define QUERY   "alpha gamma delta"

#define DOC_A   3       /* 已提交删掉的那篇：任何时候都不该再被检索到 */
#define DOC_B   4       /* 未提交删掉的那篇：撤销之后应该复活 */

/* ---------------- 小工具 ---------------- */

static void remove_db(const char *path)
{
    char buf[256];

    remove(path);
    snprintf(buf, sizeof(buf), "%s-wal", path);
    remove(buf);
    snprintf(buf, sizeof(buf), "%s-clog", path);
    remove(buf);
}

/* 检索结果里有没有这篇文档 */
static bool contains(DB *db, uint32_t doc_id)
{
    db_ft_hit out[TOP_K];
    uint32_t n = 0;

    if (db_ft_search(db, QUERY, TOP_K, out, TOP_K, &n) != DB_OK) {
        printf("    FAIL 检索失败\n");
        failures++;
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (out[i].doc_id == doc_id)
            return true;
    }
    return false;
}

/* ---------------- 一个场景 ---------------- */

/*
 * committed_first = true  → 先提交地删 A，再未提交地删 B（回归护栏）
 * committed_first = false → 先未提交地删 B，再提交地删 A（复现坑 M4-24）
 */
static void run_scenario(const char *path, bool committed_first, const char *tag)
{
    DB *db = NULL;
    DB *db2 = NULL;
    DB *db3 = NULL;
    DB *db4 = NULL;
    Result *r = NULL;
    char text[N_DOCS][128];

    printf("  %s：%s\n", tag,
           committed_first ? "先提交删 A，再未提交删 B"
                           : "先未提交删 B，再提交删 A");

    remove_db(path);

    CHECK(db_open(path, &db) == DB_OK);
    if (!db)
        return;

    /* 灌文档 */
    for (int i = 0; i < N_DOCS; i++) {
        snprintf(text[i], sizeof(text[i]), "alpha beta gamma delta doc%04d", i);
        if (db_ft_add(db, (uint32_t)i, text[i], (uint32_t)strlen(text[i])) != DB_OK) {
            printf("    FAIL 灌文档 %d 失败\n", i);
            failures++;
            db_abandon(db);
            return;
        }
    }

    /*
     * 先做一次 checkpoint：让段页落盘。
     *
     * 断电（db_abandon）**不刷脏页** —— 段页要是没落盘，
     * 断电后连文档本身都没了，测的就不是"删除有没有被撤销"了。
     */
    CHECK(db_checkpoint(db) == DB_OK);

    /* 第二个连接：用来开一个**不提交**的事务 */
    CHECK(db_connect(db, &db2) == DB_OK);
    if (!db2) {
        db_abandon(db);
        return;
    }

    if (committed_first) {
        /* T1：已提交（不在事务里 → xid=0 = 系统事务 = 永远已提交） */
        CHECK(db_ft_delete(db, DOC_A) == DB_OK);
        /* T2：未提交 */
        CHECK(db_exec(db2, "BEGIN", &r) == DB_OK);
        CHECK(db_ft_delete(db2, DOC_B) == DB_OK);
    } else {
        /* T2：未提交（**先**） */
        CHECK(db_exec(db2, "BEGIN", &r) == DB_OK);
        CHECK(db_ft_delete(db2, DOC_B) == DB_OK);
        /* T1：已提交（**后**）—— 排在未提交的那条后面 */
        CHECK(db_ft_delete(db, DOC_A) == DB_OK);
    }
    if (r) {
        db_result_free(r);
        r = NULL;
    }

    /* 断电：脏页全丢，日志还在（两个连接都要放弃，最后一个才真的销毁实例） */
    db_abandon(db2);
    db_abandon(db);

    /* ---------------- 断电后重开：恢复 ---------------- */
    CHECK(db_open(path, &db3) == DB_OK);
    if (!db3)
        return;

    /* 已提交的删除必须还在 */
    if (contains(db3, DOC_A)) {
        printf("    FAIL 已提交删掉的文档 %u 复活了\n", DOC_A);
        failures++;
    }
    /* 未提交的删除必须撤销 —— 这篇文档应该照旧能检索到 */
    if (!contains(db3, DOC_B)) {
        printf("    FAIL 未提交删掉的文档 %u 没复活\n", DOC_B);
        failures++;
    }
    /* 删除标记只剩一条（A） */
    if (db_ft_ntombstones(db3) != 1) {
        printf("    FAIL tombstone 数不对：期望 1，实际 %u\n", db_ft_ntombstones(db3));
        failures++;
    }

    /*
     * checkpoint：刷页 + 截断日志。
     *
     * 截断之后，tombstone 页就是**唯一权威** ——
     * 页里对不对，一眼就能看出来。
     */
    CHECK(db_checkpoint(db3) == DB_OK);
    db_close(db3);

    /* ---------------- 再开一次：只看页 ---------------- */
    CHECK(db_open(path, &db4) == DB_OK);
    if (!db4)
        return;

    if (contains(db4, DOC_A)) {
        printf("    FAIL checkpoint 后，已提交删掉的文档 %u 复活了\n", DOC_A);
        failures++;
    }
    if (!contains(db4, DOC_B)) {
        printf("    FAIL checkpoint 后，未提交删掉的文档 %u 没复活\n", DOC_B);
        failures++;
    }
    if (db_ft_ntombstones(db4) != 1) {
        printf("    FAIL checkpoint 后 tombstone 数不对：期望 1，实际 %u\n",
               db_ft_ntombstones(db4));
        failures++;
    }

    db_close(db4);
    remove_db(path);
}

int main(void)
{
    printf("test_ft_tomb_undo:\n");

    run_scenario(PATH_A, true, "场景 1");
    run_scenario(PATH_B, false, "场景 2");

    remove_db(PATH_A);
    remove_db(PATH_B);

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  2 个场景全部通过\n");
    return 0;
}
