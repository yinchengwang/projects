/*
 * test_ft_reclaim_boot.c —— 开机页回收（v1.3 欠账 1）
 *
 * ==================== 这个测试防的是什么 ====================
 *
 * 段级的空闲页池是**纯内存**的：合并提交时把老段的页还进池、新段再从池里取。
 * 进程一退出，池就空了 —— 那些"已经没人引用的 FT 页"继续躺在文件里。
 * 下次开库时新段只能往文件尾追加，文件随合并次数单调增长。
 *
 * 原来 `ft_segment_reclaimable_pages()` 只有实现、**零调用方**
 * （注释写着"本轮只留接口"）。现在 db_ft_open 在恢复之后用它把垃圾页
 * 装回池里，重启也能复用。
 *
 * ==================== 怎么验 ====================
 *
 * "回收没回收"在查询结果上是等价的（只影响文件大小），
 * 所以这里验两件可观察的事：
 *
 *   ① 写 → 删 → 合并 → 关库 → 重开：数据正确（回收不能破坏存活段）
 *   ② 重开之后能继续写、且第二次重开仍然正确（池里的页确实可用，
 *      不是把还在用的页发出去）
 *
 * ③ 断言池里**确实有页** —— 这是唯一能区分"回收跑了"和"回收没跑"的观测量。
 *    变异验证过：把 db_ft_open 里的回收块禁掉，重开后池从 6 变 0。
 *
 * 池大小靠内部 API 读（ft_index_seg_pool），公开 API 不暴露它 ——
 * 与 test_catalog.c 用 catalog_priv.h 是同一个理由：公共接口造不出这个观测。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"

/* 池的大小是本测试的**核心观测量**，只能从内部拿到（公开 API 不暴露它） */
#include "db_internal.h"
#include "modalities/ft/ft_index.h"
#include "modalities/ft/ft_segment.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/ft_reclaim_boot.db"

static void rm_db(void)
{
    char aux[256];
    remove(PATH);
    snprintf(aux, sizeof(aux), "%s-wal", PATH);   remove(aux);
    snprintf(aux, sizeof(aux), "%s-clog", PATH);  remove(aux);
}

/* 灌 n 篇文档：每篇都含 alpha，方便后面靠检索验"谁还在" */
static int load_docs(DB *db, int base, int n)
{
    for (int i = 0; i < n; i++) {
        char text[96];
        snprintf(text, sizeof(text), "doc%d alpha beta gamma", base + i);
        if (db_ft_add(db, (uint32_t)(base + i), text, (uint32_t)strlen(text)) != DB_OK) {
            printf("    FAIL db_ft_add(%d): %s\n", base + i, db_last_error(db));
            failures++;
            return -1;
        }
    }
    return 0;
}

/* 检索 alpha，数命中；顺便带上"应当只有多少篇活着"的期望 */
static int search_count(DB *db, const char *query, int expect)
{
    db_ft_hit hits[256];
    uint32_t n = 0;
    db_err e = db_ft_search(db, query, 256, hits, 256, &n);
    if (e != DB_OK) {
        printf("    FAIL db_ft_search(): %s\n", db_last_error(db));
        failures++;
        return -1;
    }
    if ((int)n != expect) {
        printf("    检索 \"%s\" 命中 %u 篇，期望 %d 篇\n", query, n, expect);
    }
    return (int)n;
}

int main(void)
{
    printf("test_ft_reclaim_boot:\n");
    rm_db();

    /* ---- ① 灌数据 + 删一半 + 触发合并 + 关库 ---- */
    {
        DB *db = NULL;
        CHECK(db_open(PATH, &db) == DB_OK);

        CHECK(load_docs(db, 0, 40) == 0);
        CHECK(search_count(db, "alpha", 40) == 40);

        for (int i = 1; i <= 20; i++) {
            if (db_ft_delete(db, (uint32_t)i) != DB_OK) {
                printf("    FAIL db_ft_delete(%d)\n", i);
                failures++;
            }
        }
        CHECK(search_count(db, "alpha", 20) == 20);

        /* 段数 > 阈值（8）才触发合并；40 篇 = 40 个段，足够 */
        {
            uint32_t merged = 0;
            db_err e = db_ft_merge(db, &merged);
            CHECK(e == DB_OK);
            if (e == DB_OK)
                printf("  [1] 灌 40 篇、删 20 篇、合并掉 %u 段\n", merged);
            else
                printf("    FAIL db_ft_merge: %s\n", db_last_error(db));
        }
        /* 合并之后被删的物理消失 */
        CHECK(search_count(db, "alpha", 20) == 20);
        db_close(db);
    }

    /* ---- ② 重开：开机回收跑过，数据仍对，且池里真的有页 ---- */
    {
        DB *db = NULL;
        CHECK(db_open(PATH, &db) == DB_OK);

        int n = search_count(db, "alpha", 20);
        CHECK(n == 20);

        /*
         * 核心断言：上一轮合并留下的垃圾页，开库时应该被装进池。
         * 若回收没跑（或回收时把存活段也判成垃圾），这个数会是 0 或者
         * 后续断言会红。
         */
        {
            FtPagePool *pool = ft_index_seg_pool(db->inst->ft);
            u32 pooled = ft_pool_count(pool);
            CHECK(pooled > 0);
            if (pooled == 0) {
                printf("    -> 重开后池里 0 个页：开机回收没生效\n");
            } else {
                printf("    重开后池里 %u 个页（上一轮合并的垃圾页已回收）\n", pooled);
            }
        }

        /* 继续写：新段可能用池里的页（也可能追加文件尾，两条都对） */
        CHECK(load_docs(db, 200, 10) == 0);
        CHECK(search_count(db, "alpha", 30) == 30);
        printf("  [2] 重开后 20 篇还在，继续写 10 篇 → 30 篇\n");
        db_close(db);
    }

    /* ---- ③ 第二次重开：数据仍然对（池里的页没踩坏老数据） ---- */
    {
        DB *db = NULL;
        CHECK(db_open(PATH, &db) == DB_OK);
        CHECK(search_count(db, "alpha", 30) == 30);
        printf("  [3] 第二次重开仍是 30 篇\n");
        db_close(db);
    }

    rm_db();

    if (failures == 0) {
        printf("  3 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
