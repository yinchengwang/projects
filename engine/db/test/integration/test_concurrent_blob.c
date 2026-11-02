/*
 * test_concurrent_blob.c — blob 模态的并发验收（v0.10）
 *
 * 1 项：
 *   7. 并发 blob 操作（创建 / 写 / 读，目录必须完整）
 *
 * ==================== 为什么必须单独一个文件 ====================
 *
 * blob 的公开 API **不走 db_exec** → 它有自己的一把锁（`db_blob_*` 各自加锁）。
 * 这条路径和关系模态的路径**完全独立** —— 关系模态的测试全绿，
 * 完全不能说明 blob 的并发路径是安全的。
 *
 * 第一轮反向验证时确认了这个盲区：
 * 只去掉 `db_exec` 的锁而保留 blob 的锁 → 关系模态测试变红，
 * blob 的并发问题一个都测不出来。
 *
 * ==================== 这一项真正测的是什么 ====================
 *
 * 严格说，这一项**对数据竞态不敏感**（如实记录）：
 * blob 的操作 I/O 密集，竞态窗口窄，提高压力后仍未触发失败。
 *
 * 但它测到了另一个更致命的问题 —— **页/pin 泄漏**：
 * 并发创建大量对象时，如果某个 `pager_unpin` 漏了，
 * 缓冲池的 pin 会被耗尽 → `pager_alloc` 返回 `E_NOSPACE` →
 * `db_blob_create` 失败。
 *
 * 事实上 v0.10 就是靠这一项**挖出了 v0.8 遗留的头页 pin 泄漏真 bug**。
 * 修好之后加了单元回归（test_blob 的 t11）。
 *
 * 所以这一项的价值是：**它是并发路径上唯一能稳定抓到资源泄漏的探针。**
 */

#include "conc_common.h"

#define PATH "test/tmp/concurrent_blob.db"

static void *blob_worker(void *arg)
{
    Worker *w = (Worker *)arg;
    DB *conn = NULL;
    if (db_connect(w->inst, &conn) != DB_OK) {
        w->errors++;
        return NULL;
    }

    /*
     * 大部分是"只创建不写"。
     *
     * 【为什么】
     * 竞态窗口在**页分配**上（缓冲池的哈希表 + Clock 手）。
     * 如果每次都写内容，I/O 会把线程拉开，窗口就错过了。
     * 只创建 → 密集地打分配器 → 竞态才有机会发生。
     */
    for (int i = 0; i < 300; i++) {
        db_blob_id id = 0;
        db_err e = db_blob_create(conn, 1, 0, &id);
        if (e != DB_OK) {
            if (w->errors < 3)
                printf("    诊断: db_blob_create 失败 e=%d (%s)\n", (int)e, db_strerror(e));
            w->errors++;
        }
    }

    /* 少量做完整的写-读校验 */
    for (int i = 0; i < 20; i++) {
        db_blob_id id = 0;
        if (db_blob_create(conn, 4, 0, &id) != DB_OK) {
            w->errors++;
            continue;
        }
        char payload[64];
        snprintf(payload, sizeof(payload), "obj-%d-%d", w->tid, i);
        if (db_blob_write(conn, id, payload, strlen(payload)) != DB_OK)
            w->errors++;

        /* 立刻读回来 —— 内容必须和写进去的一致 */
        unsigned char buf[128];
        uint32_t len = 0;
        if (db_blob_read_frame(conn, id, 0, buf, sizeof(buf), &len) != DB_OK)
            w->errors++;
        else if (len != strlen(payload) || memcmp(buf, payload, len) != 0)
            w->errors++;
    }

    db_conn_close(conn);
    return NULL;
}

static void t7_concurrent_blob(void)
{
    remove_db(PATH);

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    if (!db)
        return;

    pthread_t th[NTHREAD];
    Worker w[NTHREAD];

    for (int i = 0; i < NTHREAD; i++) {
        w[i].inst = db;
        w[i].table = "t";
        w[i].tid = i;
        w[i].nrows = 0;
        w[i].base_id = 0;
        w[i].errors = 0;
        CHECK(pthread_create(&th[i], NULL, blob_worker, &w[i]) == 0);
    }
    for (int i = 0; i < NTHREAD; i++) {
        pthread_join(th[i], NULL);
        CHECK(w[i].errors == 0);
    }

    /* 对象目录必须完整：一个不少 */
    uint32_t cnt = db_blob_count(db);
    CHECK(cnt == NTHREAD * (300 + 20));

    /* 每个对象都要能读到（目录被写坏的话这里会失败） */
    uint32_t bad = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        db_blob_id id = db_blob_id_at(db, i);
        uint64_t nf = 0, total = 0;
        uint32_t fs = 0;
        if (db_blob_stat(db, id, &nf, &fs, &total) != DB_OK) { bad++; continue; }
        if (nf == 0) bad++;
    }
    CHECK(bad == 0);

    db_close(db);
    remove_db(PATH);
}

int main(void)
{
    printf("test_concurrent_blob（集成）:\n");

    pthread_t wd;
    pthread_create(&wd, NULL, watchdog_worker, NULL);

    t7_concurrent_blob();

    g_done = 1;
    pthread_join(wd, NULL);

    if (failures) {
        printf("  %d 项失败 [FAIL]\n", failures);
        return 1;
    }
    printf("  1 项验收全部通过 [PASS]\n");
    return 0;
}
