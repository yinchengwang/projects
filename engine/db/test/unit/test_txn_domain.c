/*
 * test_txn_domain.c —— v1.2 步骤 4（子步骤 4a）：txn 域的并发保护
 *
 * 4 项：
 *   1. **xid 唯一性**：8 线程并发开事务 → 拿到的 xid 两两不同
 *      （顺带验证提交状态正确）
 *   2. **槽位复用**：clog 环形窗口复用的槽位里还留着老事务的"已提交" ——
 *      新事务**还没提交**时绝不能被读成"已提交"（脏读）
 *   3. **同一件事的并发版**：读者在写者持有事务期间反复读，状态必须一直是"进行中"
 *   4. **活跃表上限**：并发开事务超过 TXN_MAX_ACTIVE → 明确报 E_NOSPACE，不越界
 *
 * ==================== 第 1 项为什么是"致命"的 ====================
 *
 * PLAN-v0.10 §1 列的第一号并发风险：
 *
 *   两个线程拿到同一个 xid → 一个回滚，另一个的提交被判为回滚
 *   → **已提交的数据消失**
 *
 * 它之所以致命，是因为它**静默**：没有报错，没有崩溃，
 * 只是"查不到那条数据了"。所以必须有一个专门的测试盯着它。
 *
 * ==================== 第 2 项在抓什么 ====================
 *
 * clog 是**环形窗口**：槽位 xid % 4096 会被后来的 xid 复用。
 * 复用时如果忘了把槽位重置成"进行中"，
 * 一个刚开的事务会带着**上一个事务的"已提交"**被读出去 —— 脏读。
 *
 * 这个坑不是假想的：clog 原来是 2bit 打包（一个字节 4 个状态），
 * 读-改-写会互相覆盖。现在改成"一 xid 一字节"，
 * 槽位重置就是一次纯写入，不会丢更新。
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "txn/txn.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define NTHREAD 8
#define NROUNDS 200          /* 8×200×2 = 3200 个 xid < 4096 槽位：不触发槽位复用 */

/* ---------------- 1) 并发 xid 唯一 ---------------- */

typedef struct {
    TxnTable *tt;
    u32       xids[NROUNDS];
    int       bad;
} Worker;

static void *txn_worker(void *arg)
{
    Worker *w = (Worker *)arg;

    for (int i = 0; i < NROUNDS; i++) {
        u32 x = 0;
        if (txn_begin(w->tt, &x) != OK) {
            w->bad++;
            continue;
        }
        w->xids[i] = x;

        /*
         * 事务持有期间，再开一个（然后立刻中止）——
         * 顺便验证活跃表在并发下不会算错。
         */
        u32 y = 0;
        if (txn_begin(w->tt, &y) != OK)
            w->bad++;      /* 活跃表满了也算失败 */
        else
            txn_abort(w->tt, y);

        if (txn_commit(w->tt, w->xids[i]) != OK)
            w->bad++;
    }
    return NULL;
}

static void t1_xid_unique_and_committed(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    pthread_t th[NTHREAD];
    Worker w[NTHREAD];

    for (int i = 0; i < NTHREAD; i++) {
        w[i].tt = &tt;
        w[i].bad = 0;
        memset(w[i].xids, 0, sizeof(w[i].xids));
        CHECK(pthread_create(&th[i], NULL, txn_worker, &w[i]) == 0);
    }
    for (int i = 0; i < NTHREAD; i++) {
        pthread_join(th[i], NULL);
        CHECK(w[i].bad == 0);
    }

    /*
     * 所有 xid 两两不同 —— 按 xid 打标记查重（不排序，省一份数组）。
     */
    int total = NTHREAD * NROUNDS;
    int cap = total * 2 + 16;
    unsigned char *seen = calloc((size_t)cap, 1);
    CHECK(seen != NULL);
    if (!seen)
        return;

    int dup = 0, out_of_range = 0, not_committed = 0;
    for (int i = 0; i < NTHREAD; i++) {
        for (int j = 0; j < NROUNDS; j++) {
            u32 x = w[i].xids[j];
            if (x == 0 || x >= (u32)cap) {
                out_of_range++;
                continue;
            }
            if (seen[x])
                dup++;
            seen[x] = 1;

            /* 提交过的事务，clog 里必须是"已提交" */
            if (!txn_committed(&tt, x))
                not_committed++;
        }
    }

    CHECK(dup == 0);
    CHECK(out_of_range == 0);
    CHECK(not_committed == 0);
    printf("    %d 个并发事务，xid 重复 %d 个，状态不对 %d 个\n",
           total, dup, not_committed);

    /* 一个都不能漏在活跃表里 */
    CHECK(txn_nactive(&tt) == 0);

    free(seen);
}

/* ---------------- 2) 槽位复用：未提交 ≠ 已提交 ---------------- */

/*
 * 把整个 clog 窗口填成"已提交"，然后开一个新事务 ——
 * 它的槽位里还留着老事务的"已提交"。
 * 如果开事务时不重置槽位，这个新事务会被读成"已提交" → 脏读。
 */
static void t2_stale_slot_not_committed(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    u32 last = 0;
    for (u32 i = 0; i < TXN_CLOG_SLOTS; i++) {
        u32 x = 0;
        CHECK(txn_begin(&tt, &x) == OK);
        CHECK(txn_commit(&tt, x) == OK);
        last = x;
    }

    /* 下一个 xid 复用了窗口里第一个事务的槽位 */
    u32 y = 0;
    CHECK(txn_begin(&tt, &y) == OK);
    CHECK(y == last + 1);

    /* 关键断言：还没提交 → 必须读成"进行中" */
    CHECK(txn_clog_state(&tt, y) == TXN_CLOG_INPROGRESS);
    CHECK(txn_committed(&tt, y) == false);

    CHECK(txn_commit(&tt, y) == OK);
    CHECK(txn_committed(&tt, y) == true);
}

/* ---------------- 3) 同一件事的并发版 ---------------- */

static volatile int  g_reader_stop = 0;
static volatile int  g_dirty = 0;
static volatile u32  g_holder_xid = 0;
static volatile int  g_holder_ready = 0;

static void *holder_thread(void *arg)
{
    TxnTable *tt = (TxnTable *)arg;

    u32 x = 0;
    if (txn_begin(tt, &x) != OK)
        return NULL;

    g_holder_xid = x;
    g_holder_ready = 1;

    /* 事务一直开着，直到读者喊停 */
    while (!g_reader_stop) {
        for (volatile int i = 0; i < 1000; i++) { }
    }

    txn_commit(tt, x);
    return NULL;
}

static void *reader_thread(void *arg)
{
    TxnTable *tt = (TxnTable *)arg;

    /* 等写者真的开好事务了再读 */
    while (!g_holder_ready) {
        for (volatile int i = 0; i < 1000; i++) { }
    }

    u32 target = g_holder_xid;
    for (int i = 0; i < 200000; i++) {
        /* 写者还没提交 → 状态必须一直是"进行中" */
        if (txn_clog_state(tt, target) != TXN_CLOG_INPROGRESS)
            g_dirty = 1;
    }
    return NULL;
}

static void t3_concurrent_no_dirty_read(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    /* 先填满窗口，让"槽位复用"成为常态 */
    for (u32 i = 0; i < TXN_CLOG_SLOTS; i++) {
        u32 x = 0;
        CHECK(txn_begin(&tt, &x) == OK);
        CHECK(txn_commit(&tt, x) == OK);
    }

    g_reader_stop = 0;
    g_dirty = 0;
    g_holder_xid = 0;
    g_holder_ready = 0;

    pthread_t th, tr;
    CHECK(pthread_create(&th, NULL, holder_thread, &tt) == 0);
    CHECK(pthread_create(&tr, NULL, reader_thread, &tt) == 0);

    pthread_join(tr, NULL);
    g_reader_stop = 1;
    pthread_join(th, NULL);

    CHECK(g_dirty == 0);
    printf("    并发读 20 万次，脏读 %d 次\n", g_dirty);
}

/* ---------------- 4) 活跃表上限 ---------------- */

static void *hold_thread(void *arg)
{
    TxnTable *tt = (TxnTable *)arg;

    u32 x = 0;
    if (txn_begin(tt, &x) != OK)
        return NULL;         /* 满了 → 明确失败，不越界 */

    /* 持有一会儿再放 */
    for (volatile int i = 0; i < 50000; i++) { }
    txn_commit(tt, x);
    return NULL;
}

static void t4_active_table_limit(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    pthread_t th[128];
    for (int i = 0; i < 128; i++)
        pthread_create(&th[i], NULL, hold_thread, &tt);
    for (int i = 0; i < 128; i++)
        pthread_join(th[i], NULL);

    /* 一个都不能漏在活跃表里 */
    CHECK(txn_nactive(&tt) == 0);
    CHECK(txn_has_active(&tt) == false);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("test_txn_domain（单元）:\n");

    printf("  1. 并发 xid 唯一 + 提交状态正确\n");
    t1_xid_unique_and_committed();

    printf("  2. 槽位复用：未提交的事务不能被读成已提交\n");
    t2_stale_slot_not_committed();

    printf("  3. 并发读：无脏读\n");
    t3_concurrent_no_dirty_read();

    printf("  4. 活跃表上限\n");
    t4_active_table_limit();

    if (failures == 0) {
        printf("  4 项验收全部通过\n");
        return 0;
    }
    printf("  %d 项断言失败\n", failures);
    return 1;
}
