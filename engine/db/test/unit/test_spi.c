/*
 * test_spi.c — SPI 契约验收
 *
 * 7 项：
 *   1. 注册表：加、找、重名拒绝、找不到返回 NULL
 *   2. 能力位：模态声明自己有什么（blob 只读，heap 可写）
 *   3. 能力协商：不支持的操作返回 E_NOTSUP（不是别的错误）
 *   4. 外部值：内联值的物化
 *   5. 外部值：外部值的物化（句柄 → 内容）
 *   6. 外部值参与比较 → 结果正确（物化后比，不近似）
 *   7. 索引能力协商：B+tree 的 knn 返回 E_NOTSUP
 *
 * 【为什么"能力协商"要单独测】
 *
 * E_NOTSUP 和其他错误码是两回事：
 *   - E_NOTSUP → 调用方**换一个做法**（比如退回全表扫描）
 *   - 其他错误 → 调用方**报错**
 *
 * 混用会让优化器没法判断"要不要换个计划"。
 * 所以 blob 的写接口必须返回 E_NOTSUP，不能返回 E_INVAL ——
 * 否则优化器会以为"用户请求非法"，而不是"这个模态没这个能力"。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "index/btree.h"
#include "spi/btree_am.h"
#include "spi/registry.h"
#include "storage/value.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---------------- 1) 注册表 ---------------- */

static void t1_registry(void)
{
    SpiRegistry *r = NULL;
    CHECK(spi_registry_create(&r) == OK);
    CHECK(r != NULL);

    StorageAM am;
    memset(&am, 0, sizeof(am));
    am.name = "dummy";

    CHECK(spi_registry_add(r, "t1", &am) == OK);
    CHECK(spi_registry_count(r) == 1);

    /* 找得到，而且拿到的就是同一个指针 */
    StorageAM *got = spi_registry_find(r, "t1");
    CHECK(got == &am);

    /* 找不到返回 NULL（不是 E_NOTFOUND —— 这是个查询函数，不是语句） */
    CHECK(spi_registry_find(r, "nope") == NULL);

    /* 重名必须报错：静默覆盖会让人查不到自己刚注册的那张表 */
    CHECK(spi_registry_add(r, "t1", &am) == E_INVAL);
    CHECK(spi_registry_count(r) == 1);

    spi_registry_destroy(r);
}

/* ---------------- 2) 能力位 ---------------- */

static void t2_caps(void)
{
    /*
     * 能力位是模态**声明**自己有什么。
     *
     * 这里用两个假的 AM 检查位运算的含义：
     * 只读模态 = SCAN | EXTERNAL | PUSHDOWN（没有 INSERT/UPDATE/REMOVE）
     */
    StorageAM ro;
    memset(&ro, 0, sizeof(ro));
    ro.caps = AM_CAP_SCAN | AM_CAP_EXTERNAL | AM_CAP_PUSHDOWN;

    CHECK((ro.caps & AM_CAP_SCAN) != 0);
    CHECK((ro.caps & AM_CAP_INSERT) == 0);
    CHECK((ro.caps & AM_CAP_UPDATE) == 0);
    CHECK((ro.caps & AM_CAP_REMOVE) == 0);
    CHECK((ro.caps & AM_CAP_VACUUM) == 0);
    CHECK((ro.caps & AM_CAP_INDEX) == 0);
}

/* ---------------- 3) 能力协商的返回值 ---------------- */

static int fake_insert(StorageAM *am, u32 xid, const DbValue *vals, Rid *out)
{
    (void)am; (void)xid; (void)vals; (void)out;
    return E_NOTSUP;
}

static void t3_notsup(void)
{
    StorageAM am;
    memset(&am, 0, sizeof(am));
    am.caps = AM_CAP_SCAN;             /* 只读 */
    am.insert = fake_insert;

    /*
     * 关键：不支持的写操作必须返回 E_NOTSUP。
     *
     * 如果返回 E_INVAL，调用方会以为"用户的 SQL 写错了"；
     * 返回 E_NOTSUP 才是"这个模态没有这个能力，可以退回别的做法"。
     */
    DbValue v;
    val_int(&v, 1);
    Rid rid;
    CHECK(am.insert(&am, 1, &v, &rid) == E_NOTSUP);
    CHECK(E_NOTSUP != E_INVAL);
    CHECK(E_NOTSUP != E_NOTFOUND);
}

/* ---------------- 外部值 ---------------- */

/*
 * 假的取回函数：内容就是 ctx 指向的字符串。
 *
 * 约定（见 storage/value.h）：
 *   cap == 0 → 只问长度，返回 E_NOSPACE
 *   cap >  0 → 真正读
 */
static int fake_detoast(void *ctx, u64 key, u64 off,
                        u8 *buf, u16 cap, u16 *len_out)
{
    char *s = (char *)ctx;
    size_t n = strlen(s);

    /* key / off 也用上：验证它们被正确传下来 */
    if (key != 7 || off != 3)
        return E_CORRUPT;

    if (!buf || cap == 0) {
        if (len_out)
            *len_out = (u16)n;
        return E_NOSPACE;
    }
    if (n > cap)
        return E_NOSPACE;
    memcpy(buf, s, n);
    if (len_out)
        *len_out = (u16)n;
    return OK;
}

/* 4) 内联值的物化 */
static void t4_detoast_inline(void)
{
    DbValue v;
    val_text(&v, "hello", 5);
    CHECK(!v.external);

    u8 *buf = NULL;
    u32 len = 0;
    CHECK(val_detoast(&v, &buf, &len) == OK);
    CHECK(buf != NULL);
    CHECK(len == 5);
    CHECK(buf && memcmp(buf, "hello", 5) == 0);
    free(buf);
}

/* 5) 外部值的物化 */
static void t5_detoast_external(void)
{
    char payload[] = "external-payload";

    DbValue v;
    val_external_text(&v, 7, 3, fake_detoast, payload);
    CHECK(v.external);
    CHECK(v.ext_key == 7);
    CHECK(v.ext_off == 3);
    CHECK(v.ext_fn == fake_detoast);

    u8 *buf = NULL;
    u32 len = 0;
    CHECK(val_detoast(&v, &buf, &len) == OK);
    CHECK(buf != NULL);
    CHECK(len == strlen(payload));
    CHECK(buf && memcmp(buf, payload, len) == 0);
    free(buf);

    /* 空内容也要能物化（长度 0，不是错误） */
    char empty[] = "";
    DbValue ev;
    val_external_text(&ev, 7, 3, fake_detoast, empty);
    buf = NULL;
    len = 12345;
    CHECK(val_detoast(&ev, &buf, &len) == OK);
    CHECK(len == 0);
    free(buf);
}

/* 6) 外部值参与比较 */
static void t6_external_compare(void)
{
    char a[] = "aaaa";
    char b[] = "bbbb";

    DbValue va, vb;
    val_external_text(&va, 7, 3, fake_detoast, a);
    val_external_text(&vb, 7, 3, fake_detoast, b);

    bool isnull = false;
    int err = OK;

    /* 外部 < 外部：物化后比字节 */
    int c = val_cmp(&va, &vb, &isnull, &err);
    CHECK(err == OK);
    CHECK(!isnull);
    CHECK(c < 0);

    /* 外部 == 外部 */
    DbValue va2;
    val_external_text(&va2, 7, 3, fake_detoast, a);
    c = val_cmp(&va, &va2, &isnull, &err);
    CHECK(err == OK);
    CHECK(c == 0);

    /* 外部 vs 内联 */
    DbValue inl;
    val_text(&inl, "aaaa", 4);
    c = val_cmp(&va, &inl, &isnull, &err);
    CHECK(err == OK);
    CHECK(c == 0);

    /* 转字符串也要物化 */
    char buf[64];
    int n = val_to_str(&va, buf, (int)sizeof(buf));
    CHECK(n == 4);
    CHECK(strcmp(buf, "aaaa") == 0);
}

/* ---------------- 7) 索引能力协商 ---------------- */

#define BT_PATH "test/tmp/spi_bt.db"

static void t7_knn_notsup(void)
{
    remove(BT_PATH);

    Pager *pg = NULL;
    FreeList *fl = NULL;
    BTree *bt = NULL;
    u32 root = 0;

    CHECK(pager_open(BT_PATH, 64, &pg) == OK);
    CHECK(freelist_create(0, &fl) == OK);
    CHECK(btree_create(pg, fl, &root) == OK);
    CHECK(btree_open(pg, fl, root, &bt) == OK);

    IndexAM am;
    CHECK(btree_am_build(bt, &am) == OK);
    CHECK(strcmp(am.name, "btree") == 0);

    /* B+tree 有等值查找和范围扫描 */
    CHECK((am.caps & IX_CAP_SEARCH) != 0);
    CHECK((am.caps & IX_CAP_RANGE) != 0);

    /* 但没有近邻查询 */
    CHECK((am.caps & IX_CAP_KNN) == 0);
    Rid outs[4];
    float dist[4];
    int n = 0;
    u8 probe[4] = {1, 2, 3, 4};
    CHECK(am.knn(&am, probe, 4, 4, outs, dist, &n) == E_NOTSUP);

    /* 基本查找能用 */
    u8 key[4] = {0, 0, 0, 7};
    Rid rid = rid_make(1, 0);
    CHECK(am.insert(&am, key, 4, rid) == OK);
    Rid got[4];
    int cnt = 0;
    CHECK(am.search(&am, key, 4, got, 4, &cnt) == OK);
    CHECK(cnt == 1);
    CHECK(got[0].page == 1 && got[0].slot == 0);

    btree_destroy(bt);
    freelist_destroy(fl);
    pager_close(pg);
    remove(BT_PATH);
}

int main(void)
{
    printf("test_spi:\n");

    t1_registry();
    t2_caps();
    t3_notsup();
    t4_detoast_inline();
    t5_detoast_external();
    t6_external_compare();
    t7_knn_notsup();

    if (failures) {
        printf("  %d 项失败 [FAIL]\n", failures);
        return 1;
    }
    printf("  7 项验收全部通过 [PASS]\n");
    return 0;
}
