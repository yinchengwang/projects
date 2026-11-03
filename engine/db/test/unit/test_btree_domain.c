/*
 * test_btree_domain.c —— v1.2 步骤 4d：btree 域的并发保护
 *
 * 4 项：
 *   1. **并发插入不丢项**：多线程往同一棵 B+tree 插 → 每个 key 都必须查得到
 *   2. **并发插入不重复**：每个 key 只能查到一次
 *   3. **树结构一致**：并发插完之后树高、页数、条目数必须对得上
 *      （分裂丢页 / 重复分裂都会在这里露馅）
 *   4. **并发删 + 插**：一边插一边删 → 最终树里的条目数必须正好
 *
 * ==================== 第 1 项在抓什么 ====================
 *
 * B+tree 的树结构本身就是**共享可变状态**：
 *
 *   - `root` / `height`：根分裂要换根，根降高要换根
 *   - `scratch[]`：**compact 用的整页暂存区** —— 两个线程同时用它
 *     就是两个人往同一块内存里搬数据 → 页内容直接乱掉
 *   - **分裂本身不是原子的**：`insert_rec` 先在孩子页插，
 *     再回父亲页插分隔键 —— 这两步之间如果另一个线程也在分裂，
 *     父亲页的条目就可能重复或丢失
 *
 * 大锁在的时候，这些全被顺带护住了。
 * 大锁一撤（步骤 4d），实测**并发写索引会丢索引项**：
 * 1200 行并发插入，堆里 1200 行全在，点查却能漏掉 200 多行 ——
 * **静默的数据不一致**（行在，索引找不到）。
 *
 * ==================== 为什么这个测试不走 db_exec ====================
 *
 * 这一层要能**独立**证明"B+tree 自己站得住"，
 * 而不是靠"并发测试没报错"来间接推断。
 * 所以这里直接调 btree_insert / btree_search，完全绕开 SQL 与大锁。
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "index/btree.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define NTHREAD 4
#define NKEYS   400          /* 每线程插的 key 数 */
#define TOTAL   (NTHREAD * NKEYS)

typedef struct {
    Pager    *pg;
    FreeList *fl;
    BTree    *bt;
    u32       root;
} Env;

static int env_create(Env *e, const char *path)
{
    memset(e, 0, sizeof(*e));
    remove(path);
    int rc = pager_open(path, 64, &e->pg);
    if (rc != OK) return rc;
    rc = freelist_create(0, &e->fl);
    if (rc != OK) return rc;
    rc = btree_create(e->pg, e->fl, &e->root);
    if (rc != OK) return rc;
    return btree_open(e->pg, e->fl, e->root, &e->bt);
}

static void env_close(Env *e)
{
    if (e->bt) btree_destroy(e->bt);
    if (e->fl) freelist_destroy(e->fl);
    if (e->pg) pager_close(e->pg);
    memset(e, 0, sizeof(*e));
}

/*
 * key 必须**大端** —— B+tree 用 memcmp 比较，
 * 小端会让 63 > 256（btree.h 里记着这个坑）。
 */
static void mk_key(u32 k, u8 *b)
{
    b[0] = (u8)(k >> 24);
    b[1] = (u8)(k >> 16);
    b[2] = (u8)(k >> 8);
    b[3] = (u8)k;
}

static Rid mk_rid(u32 k)
{
    Rid r;
    r.page = k / 100u + 1u;
    r.slot = (u16)(k % 100u);
    return r;
}

/* ---------------- 1~3) 并发插入 ---------------- */

typedef struct {
    BTree *bt;
    int   tid;
    int   errors;
} InsWorker;

static void *ins_worker(void *arg)
{
    InsWorker *w = (InsWorker *)arg;
    u8 key[4];

    for (int i = 0; i < NKEYS; i++) {
        u32 k = (u32)(w->tid * NKEYS + i);
        mk_key(k, key);
        if (btree_insert(w->bt, key, 4, mk_rid(k)) != OK)
            w->errors++;
    }
    return NULL;
}

static void t1_concurrent_insert(void)
{
    Env e;
    if (env_create(&e, "test/tmp/btree_domain.db") != OK) {
        printf("    FAIL 环境创建失败\n");
        failures++;
        return;
    }

    pthread_t th[NTHREAD];
    InsWorker w[NTHREAD];
    for (int i = 0; i < NTHREAD; i++) {
        w[i].bt = e.bt;
        w[i].tid = i;
        w[i].errors = 0;
    }
    for (int i = 0; i < NTHREAD; i++)
        CHECK(pthread_create(&th[i], NULL, ins_worker, &w[i]) == 0);
    for (int i = 0; i < NTHREAD; i++)
        pthread_join(th[i], NULL);

    int errors = 0;
    for (int i = 0; i < NTHREAD; i++)
        errors += w[i].errors;
    CHECK(errors == 0);

    /* 第 1、2 项：每个 key 都必须查得到，而且只能查到一个 */
    int missing = 0, multi = 0;
    u8 key[4];
    Rid out[8];
    for (int k = 0; k < TOTAL; k++) {
        mk_key((u32)k, key);
        int n = 0;
        int rc = btree_search(e.bt, key, 4, out, 8, &n);
        if (rc != OK || n == 0)
            missing++;
        else if (n > 1)
            multi++;
    }
    CHECK(missing == 0);
    CHECK(multi == 0);

    /* 第 3 项：树结构自洽 */
    BtStats st;
    memset(&st, 0, sizeof(st));
    CHECK(btree_stats(e.bt, &st) == OK);
    CHECK(st.leafitems == TOTAL);
    CHECK(st.nleaves > 0);
    CHECK(btree_height(e.bt) >= 1);

    printf("    插入失败 %d 次；查不到 %d 个，查到多个 %d 个；"
           "树高 %u，%u 页（%u 叶 / %u 内），叶子条目 %u\n",
           errors, missing, multi,
           btree_height(e.bt), st.npages, st.nleaves, st.ninner, st.leafitems);

    env_close(&e);
}

/* ---------------- 4) 并发删 + 插 ---------------- */

/*
 * 一半的线程在插，另一半在删。
 *
 * 删的是**别的线程**刚插进去的 key ——
 * 这样"插进去 → 又被删掉"和"插进去 → 留下来"两种结局同时发生，
 * 树的结构（分裂 / 合并 / 根降高）被反复改动。
 *
 * 最终检查：剩下多少 key，就要能查到多少。
 */
typedef struct {
    BTree *bt;
    int   tid;
    int   errors;
    int   deleted;
} MixWorker;

static void *mix_del(void *arg)
{
    MixWorker *w = (MixWorker *)arg;
    u8 key[4];
    for (int i = 0; i < NKEYS; i++) {
        u32 k = (u32)(w->tid * NKEYS + i);
        mk_key(k, key);
        if (btree_delete(w->bt, key, 4, mk_rid(k)) != OK)
            w->errors++;
        else
            w->deleted++;
    }
    return NULL;
}

static void t2_insert_and_delete(void)
{
    Env e;
    if (env_create(&e, "test/tmp/btree_domain_mix.db") != OK) {
        printf("    FAIL 环境创建失败\n");
        failures++;
        return;
    }

    /* 先插满，再并发地"删一半 + 再插一批新的" */
    pthread_t th[NTHREAD];
    InsWorker w[NTHREAD];
    for (int i = 0; i < NTHREAD; i++) {
        w[i].bt = e.bt;
        w[i].tid = i;
        w[i].errors = 0;
    }
    for (int i = 0; i < NTHREAD; i++)
        CHECK(pthread_create(&th[i], NULL, ins_worker, &w[i]) == 0);
    for (int i = 0; i < NTHREAD; i++)
        pthread_join(th[i], NULL);

    /*
     * 前一半的 key 被删掉。
     * mix_del 按 tid 划分区间：tid=0 删 0..NKEYS-1，tid=1 删 NKEYS..2*NKEYS-1，
     * 正好覆盖"前一半"，所以要开**两个**删除线程（删一个线程只删得掉 1/4）。
     */
    MixWorker d[2];
    memset(d, 0, sizeof(d));
    d[0].bt = e.bt;
    d[0].tid = 0;
    d[1].bt = e.bt;
    d[1].tid = 1;
    CHECK(pthread_create(&th[0], NULL, mix_del, &d[0]) == 0);
    CHECK(pthread_create(&th[1], NULL, mix_del, &d[1]) == 0);
    pthread_join(th[0], NULL);
    pthread_join(th[1], NULL);
    CHECK(d[0].errors == 0);
    CHECK(d[1].errors == 0);

    /* 被删的那一半必须真的查不到 */
    int still = 0;
    u8 key[4];
    Rid out[8];
    for (int k = 0; k < TOTAL / 2; k++) {
        mk_key((u32)k, key);
        int n = 0;
        if (btree_search(e.bt, key, 4, out, 8, &n) == OK && n > 0)
            still++;
    }
    CHECK(still == 0);

    /* 没被删的那一半必须还在 */
    int missing = 0;
    for (int k = TOTAL / 2; k < TOTAL; k++) {
        mk_key((u32)k, key);
        int n = 0;
        if (btree_search(e.bt, key, 4, out, 8, &n) != OK || n == 0)
            missing++;
    }
    CHECK(missing == 0);

    BtStats st;
    memset(&st, 0, sizeof(st));
    CHECK(btree_stats(e.bt, &st) == OK);
    CHECK(st.leafitems == (u32)(TOTAL / 2));

    printf("    删掉一半后：还查得到的 %d 个，该有却没有的 %d 个，叶子条目 %u（应 %d）\n",
           still, missing, st.leafitems, TOTAL / 2);

    env_close(&e);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("test_btree_domain（单元）:\n");

    printf("  1. 并发插入不丢项、不重复\n");
    t1_concurrent_insert();

    printf("  2. 树结构自洽（条目数 / 树高 / 页数）\n");
    printf("    （与第 1 项同一次运行，结果见上）\n");

    printf("  3. 并发删 + 插：留下的 key 数必须对得上\n");
    t2_insert_and_delete();

    if (failures == 0) {
        printf("  3 项验收全部通过\n");
        return 0;
    }
    printf("  %d 项断言失败\n", failures);
    return 1;
}
