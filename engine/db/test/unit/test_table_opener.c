/*
 * test_table_opener.c —— 第四个 SPI 契约（表打开器）验收
 *
 * 5 项：
 *   1. 通过 rel_table_opener 按名打开用户表：拿到 AM / 列定义 / 索引
 *   2. 列定义与索引列表落在**调用方给的缓冲**里（不是打开器自己的内存）
 *   3. 按 id 打开 与 按名打开 得到同一张表
 *   4. 名不存在 → E_NOTFOUND（执行器据此去问下一个 opener）
 *   5. spi_table_opener：虚拟表能打开、按 id 打开返回 E_NOTSUP
 *
 * 【第 2 项是核心】
 * opener 的契约写着"out->cols 指向调用方给的 cols_buf"。
 * 如果哪天有人图省事改成指向 catalog 的内部数组，
 * v1.2 步骤 1 好不容易做出来的 copy-out 纪律就白费了 ——
 * 而且症状很隐蔽（读得到数据，但另一连接一建表就悬空）。
 */

#include <stdio.h>
#include <string.h>

#include "catalog/catalog.h"
#include "modalities/relational/rel_opener.h"
#include "spi/registry.h"
#include "spi/table_opener.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/table_opener.db"

static ColDesc USERS_COLS[] = {
    { "id",   T_INT4, false },
    { "name", T_TEXT, true  },
};
#define N_USERS 2

/* 一个假的虚拟表 AM：只有 columns 一定要实现 */
static int fake_columns(StorageAM *am, const ColDesc **cols, int *ncols)
{
    static ColDesc fake[] = {
        { "v", T_INT4, false },
    };
    (void)am;
    *cols = fake;
    *ncols = 1;
    return OK;
}

int main(void)
{
    Pager   *pg = NULL;
    Catalog *cat = NULL;
    u32      users_tid = 0;

    remove(PATH);
    remove(PATH "-wal");

    printf("test_table_opener:\n");

    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(catalog_open(pg, NULL, NULL, &cat) == OK);
    CHECK(catalog_create_table(cat, 0, "users", USERS_COLS, N_USERS,
                               &users_tid) == OK);

    /*
     * 1) 按名打开用户表。
     *
     * priv 用一个足够大的本地缓冲 —— 与执行器的 TableCtx 同一种形状。
     */
    {
        TableOpenResult res;
        ColDesc    cols_buf[SQL_MAX_COLS];
        IndexInfo  idxs_buf[SQL_MAX_COLS];
        unsigned char priv[SPI_OPENER_PRIV_MAX];
        memset(&res, 0, sizeof(res));
        memset(cols_buf, 0, sizeof(cols_buf));
        memset(idxs_buf, 0, sizeof(idxs_buf));
        memset(priv, 0, sizeof(priv));

        int rc = rel_table_opener.open(&rel_table_opener, (void *)cat, "users",
                                       &res, cols_buf, idxs_buf,
                                       priv, sizeof(priv));
        CHECK(rc == OK);
        CHECK(res.am != NULL);
        CHECK(res.ncols == N_USERS);
        CHECK(!res.is_virtual);
        CHECK(res.tid == users_tid);

        /* 2) 列定义必须指回调用方的缓冲 */
        CHECK(res.cols == cols_buf);
        CHECK(res.idxs == idxs_buf);
        CHECK(strcmp(cols_buf[0].name, "id") == 0);
        CHECK(cols_buf[0].type == T_INT4);
        CHECK(strcmp(cols_buf[1].name, "name") == 0);
        CHECK(cols_buf[1].type == T_TEXT);

        /* AM 自己也认得同一批列（它拿的就是这份副本） */
        const ColDesc *amcols = NULL;
        int amn = 0;
        CHECK(res.am->columns(res.am, &amcols, &amn) == OK);
        CHECK(amn == N_USERS);
    }

    /* 3) 按 id 打开 → 同一张表 */
    {
        TableOpenResult res;
        ColDesc    cols_buf[SQL_MAX_COLS];
        IndexInfo  idxs_buf[SQL_MAX_COLS];
        unsigned char priv[SPI_OPENER_PRIV_MAX];
        memset(&res, 0, sizeof(res));
        memset(priv, 0, sizeof(priv));

        int rc = rel_table_opener.open_by_id(&rel_table_opener, (void *)cat,
                                             users_tid, &res,
                                             cols_buf, idxs_buf,
                                             priv, sizeof(priv));
        CHECK(rc == OK);
        CHECK(res.tid == users_tid);
        CHECK(res.ncols == N_USERS);
        CHECK(res.cols == cols_buf);
    }

    /* 4) 表不存在 → E_NOTFOUND */
    {
        TableOpenResult res;
        ColDesc    cols_buf[SQL_MAX_COLS];
        IndexInfo  idxs_buf[SQL_MAX_COLS];
        unsigned char priv[SPI_OPENER_PRIV_MAX];
        memset(&res, 0, sizeof(res));
        memset(priv, 0, sizeof(priv));

        int rc = rel_table_opener.open(&rel_table_opener, (void *)cat, "nope",
                                       &res, cols_buf, idxs_buf,
                                       priv, sizeof(priv));
        CHECK(rc == E_NOTFOUND);
    }

    /*
     * private 缓冲不够时必须是 E_INVAL，而不是越界写。
     * （这条防的是"priv_bytes 没填对"那类错误 —— 一次 E_INVAL 比一次踩内存好。）
     */
    {
        TableOpenResult res;
        ColDesc    cols_buf[SQL_MAX_COLS];
        IndexInfo  idxs_buf[SQL_MAX_COLS];
        unsigned char priv[8];   /* 故意太小 */
        memset(&res, 0, sizeof(res));
        memset(priv, 0, sizeof(priv));

        int rc = rel_table_opener.open(&rel_table_opener, (void *)cat, "users",
                                       &res, cols_buf, idxs_buf,
                                       priv, sizeof(priv));
        CHECK(rc == E_INVAL);
    }

    /* 5) 虚拟表 opener */
    {
        SpiRegistry *reg = NULL;
        StorageAM    am;
        memset(&am, 0, sizeof(am));
        am.name = "fake";
        am.caps = AM_CAP_SCAN;
        am.columns = fake_columns;

        CHECK(spi_registry_create(&reg) == OK);
        CHECK(spi_registry_add(reg, "fake_virtual", &am) == OK);

        TableOpenResult res;
        ColDesc    cols_buf[SQL_MAX_COLS];
        IndexInfo  idxs_buf[SQL_MAX_COLS];
        memset(&res, 0, sizeof(res));

        int rc = spi_table_opener.open(&spi_table_opener, (void *)reg,
                                       "fake_virtual", &res,
                                       cols_buf, idxs_buf, NULL, 0);
        CHECK(rc == OK);
        CHECK(res.is_virtual);
        CHECK(res.am == &am);
        CHECK(res.ncols == 1);
        CHECK(res.tid == 0);
        /* 虚拟表没有索引 */
        CHECK(res.nidx == 0);

        /* 表不存在 */
        rc = spi_table_opener.open(&spi_table_opener, (void *)reg, "no_such",
                                   &res, cols_buf, idxs_buf, NULL, 0);
        CHECK(rc == E_NOTFOUND);

        /* 按 id 打开 → 虚拟表不支持 */
        rc = spi_table_opener.open_by_id(&spi_table_opener, (void *)reg, 7,
                                         &res, cols_buf, idxs_buf, NULL, 0);
        CHECK(rc == E_NOTSUP);

        spi_registry_destroy(reg);
    }

    catalog_close(cat);
    pager_close(pg);

    if (failures == 0) {
        printf("  5 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
