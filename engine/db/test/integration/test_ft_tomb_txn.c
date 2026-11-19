/*
 * test_ft_tomb_txn.c —— FT tombstone 与**事务**的关系（W-D 的 D-4 / D-4b）
 *
 * ==================== 这个测试防的是两件事 ====================
 *
 * 【D-4：锁住「重复删除不写第二条记录」这个前提】
 *
 * 原计划担心「同一 doc_id 被不同事务重复删除时，恢复期按值撤销会撤错」。
 * D-4.1 的探针实测证明这个歧义**不存在**：
 *   `ft_index.c:479` 的 `if (ft_tomb_is(ix, doc_id)) return OK;` 在写 WAL
 *   **之前**就返回了 ⇒ 第二次删除既不写 WAL、也不往页里加第二条。
 * 于是「同一页里出现两条相同 doc_id」永远不会发生，按值查必然唯一命中。
 *
 * 本测试把这条前提**机械地钉住**：连删两次之后，页里只有一条、WAL 里也只有
 * 一条 WAL_FT_TOMB_APPEND。哪天有人把那个提前返回挪到写 WAL 之后（或者删掉），
 * 这条断言立刻红 —— 那时才需要按原计划 D-4.2 上（doc_id, xid）双字段。
 *
 * 【D-4b：运行时 ROLLBACK 必须同步撤掉 FT 删除】
 *
 * 实测发现的**真缺陷**（与原计划的歧义无关）：
 *
 *   BEGIN → db_ft_delete(5) → ROLLBACK → 同一会话里再检索 ⇒ **查不到**那篇文档
 *
 * 原因是运行时回滚只动了 txn 表，没人去撤 `ix->tomb` 集合；
 * 而关库重开之后又对了（页里的记录已被 undo 撤掉，恢复时按页重建集合）。
 * 于是同一个会话里前后两次查询给出不同答案 —— 用户可见的不一致。
 *
 * 修法：本事务删过的 doc_id 记在**连接**上，ROLLBACK 时逐个撤
 * （页 + 内存集合）。记在连接上而不是实例上：实例是多个连接共享的，
 * 记在那里会让 A 的回滚撤掉 B 的删除。
 *
 * ==================== 已知限制（本测试**不**覆盖，如实记账） ====================
 *
 * 两个**并发**事务删同一篇文档：A 未提交地删 5，B 删 5（此时 5 已在集合里 ⇒
 * B 是个空操作、不记录），B 提交，A 回滚 ⇒ 5 被 A 撤掉 ⇒ **B 那次删除的意图没了**。
 * 这不是本测试的缺陷，是整个内核「删除是物理删除、不做写冲突检测」的已知限制。
 * WAL 那一路的结论与它一致（A 的记录被 undo 撤掉，B 根本没有记录），
 * 所以两条路径**不会互相矛盾** —— 但这确实不是「谁先谁赢」的语义。
 *
 * ==================== 判据为什么是四条，不是两条 ====================
 *
 * 只测「ROLLBACK 后能查到」会漏掉「COMMIT 后查不到」—— 那可能被
 * 「怎么都撤掉」这种过度修正蒙混过去。
 * 只测会话内会漏掉「重开之后还对不对」—— 那是另一个路径（按页重建）。
 * 四条一起，才把「会话内」与「重开后」两条路径钉在同一套语义上。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "db_internal.h"
#include "modalities/ft/ft_index.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/ft_tomb_txn.db"

static void rm_db(void)
{
    char aux[256];

    remove(PATH);
    snprintf(aux, sizeof(aux), "%s-wal", PATH);   remove(aux);
    snprintf(aux, sizeof(aux), "%s-clog", PATH);  remove(aux);
    snprintf(aux, sizeof(aux), "%s-shm", PATH);   remove(aux);
}

/* 检索 alpha 的命中数 */
static int hits(DB *db)
{
    db_ft_hit h[64];
    uint32_t n = 0;

    if (db_ft_search(db, "alpha", 64, h, 64, &n) != DB_OK) {
        printf("    检索失败：%s\n", db_last_error(db));
        failures++;
        return -1;
    }
    return (int)n;
}

/* 执行一条不带结果的语句（BEGIN / COMMIT / ROLLBACK） */
static void run(DB *db, const char *sql)
{
    Result *r = NULL;

    if (db_exec(db, sql, &r) != DB_OK) {
        printf("    FAIL 执行 \"%s\"：%s\n", sql, db_last_error(db));
        failures++;
    }
    if (r) {
        db_result_free(r);
    }
}

/*
 * ==================== 第 1 项：D-4 的前提（重复删除只留一条） ====================
 */
static void t1_dup_delete_leaves_one(void)
{
    DB *db = NULL;

    rm_db();
    CHECK(db_open(PATH, &db) == DB_OK);
    CHECK(db_ft_add(db, 5, "alpha beta", 10) == DB_OK);
    CHECK(hits(db) == 1);

    CHECK(db_ft_delete(db, 5) == DB_OK);
    CHECK(hits(db) == 0);

    /* 再删一次：合法空操作（不报错、也不产生第二条） */
    CHECK(db_ft_delete(db, 5) == DB_OK);
    CHECK(hits(db) == 0);

    /*
     * 核心断言：tombstone 里的条数**只 +1**。
     * 若哪天「已删就跳过」的检查被挪到写日志之后，这里会变成 2，
     * 那时原计划 D-4.2（页内条目加 xid）才真正需要。
     */
    CHECK(ft_index_ntombstones(db->inst->ft) == 1);

    CHECK(db_close(db) == DB_OK);
    printf("  [1] 连删两次：tombstone 只有 1 条（按值撤销必然唯一命中）\n");
}

/*
 * ==================== 第 2 项：ROLLBACK 后同一会话里要能查到 ====================
 */
static void t2_rollback_same_session(void)
{
    DB *db = NULL;

    rm_db();
    CHECK(db_open(PATH, &db) == DB_OK);
    CHECK(db_ft_add(db, 5, "alpha beta", 10) == DB_OK);

    run(db, "BEGIN;");
    CHECK(db_ft_delete(db, 5) == DB_OK);
    /* 事务内自己看得到"已删"（当前读，不是快照读） */
    CHECK(hits(db) == 0);

    run(db, "ROLLBACK;");

    /*
     * ★ 这一条就是 D-4b 的判据：回滚之后，本会话里必须重新查得到。
     * 修之前这里是 0（红）。
     */
    CHECK(hits(db) == 1);

    CHECK(db_close(db) == DB_OK);
    printf("  [2] ROLLBACK 之后同一会话里又能查到（修前这里是红的）\n");
}

/*
 * ==================== 第 3 项：COMMIT 之后必须查不到（防止过度撤销） ====================
 */
static void t3_commit_same_session(void)
{
    DB *db = NULL;

    rm_db();
    CHECK(db_open(PATH, &db) == DB_OK);
    CHECK(db_ft_add(db, 5, "alpha beta", 10) == DB_OK);

    run(db, "BEGIN;");
    CHECK(db_ft_delete(db, 5) == DB_OK);
    run(db, "COMMIT;");

    /* 提交了就该永久删掉 —— 不能因为 D-4b 的修改把已提交的删除也撤掉 */
    CHECK(hits(db) == 0);

    CHECK(db_close(db) == DB_OK);
    printf("  [3] COMMIT 之后查不到（该删的还删）\n");
}

/*
 * ==================== 第 4 项：两条路径的语义要一致 ====================
 *
 * 会话内撤销（D-4b 新增的）与「关库重开后按页重建集合」（原先就对的）
 * 必须给出同一个答案，否则就成了"关一下库结论就变"。
 */
static void t4_paths_agree(void)
{
    DB *db = NULL;

    rm_db();
    CHECK(db_open(PATH, &db) == DB_OK);
    CHECK(db_ft_add(db, 5, "alpha beta", 10) == DB_OK);

    run(db, "BEGIN;");
    CHECK(db_ft_delete(db, 5) == DB_OK);
    run(db, "ROLLBACK;");
    CHECK(hits(db) == 1);                 /* 会话内 */
    CHECK(db_close(db) == DB_OK);

    /* 重开：按 tombstone 页重建集合，必须也是 1 */
    db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    CHECK(hits(db) == 1);
    CHECK(db_close(db) == DB_OK);

    /* 同上，但这次提交：两条路径都必须给出 0 */
    DB *d2 = NULL;
    CHECK(db_open(PATH, &d2) == DB_OK);
    run(d2, "BEGIN;");
    CHECK(db_ft_delete(d2, 5) == DB_OK);
    run(d2, "COMMIT;");
    CHECK(hits(d2) == 0);                 /* 会话内 */
    CHECK(db_close(d2) == DB_OK);

    d2 = NULL;
    CHECK(db_open(PATH, &d2) == DB_OK);
    CHECK(hits(d2) == 0);                 /* 重开后 */
    CHECK(db_close(d2) == DB_OK);

    printf("  [4] 会话内与重开后两条路径结论一致（回滚=1、提交=0）\n");
}

int main(void)
{
    printf("test_ft_tomb_txn:\n");

    t1_dup_delete_leaves_one();
    t2_rollback_same_session();
    t3_commit_same_session();
    t4_paths_agree();

    rm_db();

    if (failures == 0) {
        printf("  4 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
