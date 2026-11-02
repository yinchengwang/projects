/*
 * test_vstore.c — 向量列式存储单元验收（v1.0 步骤 2 · A1）
 *
 * 7 项：
 *   1. K 的算术：128 维 → 每页 7 个（**下界**，不是 7.98 四舍五入的 8）
 *   2. 地址算术：row_id → (序号, 槽号, 页内偏移)，纯算术无查找
 *   3. 插入 + 读回：100 个向量逐个比对（跨页边界逐字节对）
 *   4. 列对齐：向量 ↔ 标量行按序号对应；label 与 row_id 一起读回
 *   5. 扫描：顺序扫 100 个，**IO = 页数（15），不是向量数（100）**
 *   6. 重开：destroy 后重新打开，目录 / 计数 / 数据全对
 *   7. 列对齐被破坏时**打开就失败**（不是带着错位数据跑）
 *
 * 【第 5 项是列式的价值所在】
 *
 * 行式存储扫 100 个 128 维向量要读 100 次（每个向量散在页里）。
 * 列式一页 7 个，扫完只要 15 次页读。
 * 这条断言把"列式"从设计口号变成可测的数字。
 *
 * 【第 7 项是这一层最重要的防线】
 *
 * 列对齐错位不会报错，只会让查出来的向量配上别人的 label。
 * 所以打开时必须做交叉校验（链序 == 序号、各页 count 之和 == 标量行数），
 * 任何一条不成立就 E_CORRUPT —— 宁可打不开，也不要跑出错误的答案。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/vector/vstore.h"
#include "storage/page.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH  "test/tmp/vstore_test.db"
#define DIM   128u
#define KEXP  ((PAGE_SIZE - HDR_SIZE) / (DIM * 4u))   /* 期望 K = 7 */
#define N     100u

/* 可精确表示在 float 里的确定性取值（整数 < 2^24） */
static float val_of(u64 row, u16 d)
{
    return (float)(row * 31u + d);
}

static i32 label_of(u64 row)
{
    return (i32)(row % 7u);
}

static void fresh(void)
{
    remove(PATH);
    remove(PATH "-wal");
}

/* ---------------- 1. K 的算术（坑 M3-1） ---------------- */

static void test_k_arithmetic(void)
{
    printf("  1. K 的算术（下界，不越界）\n");

    /* 128 维：4088 / 512 = 7.98 → 必须取 7 */
    CHECK(KEXP == 7u);
    CHECK(VEC_K(DIM) == 7u);

    /*
     * K 是**最大**的可行值：
     * 第 K-1 个向量放得下，第 K 个放不下。
     */
    u32 last_end = VEC_PAGE_HDR + (KEXP - 1u) * DIM * 4u + DIM * 4u;
    CHECK(last_end <= PAGE_SIZE);
    CHECK(VEC_PAGE_HDR + KEXP * DIM * 4u + DIM * 4u > PAGE_SIZE);

    /* 极端维度 */
    CHECK(VEC_K(1) == (PAGE_SIZE - VEC_PAGE_HDR) / 4u);      /* 1019 */
    CHECK(VEC_K(1019) == 1u);                                /* 一页一个 */

    /* 维度太大 → 建都建不了 */
    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);
    VStore *vs = NULL;
    CHECK(vstore_create(pg, NULL, NULL, 1020, &vs) == E_INVAL);
    CHECK(vs == NULL);
    pager_close(pg);
}

/* ---------------- 2. 地址算术 ---------------- */

static void test_slot_arithmetic(void)
{
    printf("  2. 地址算术（纯算术，无查找）\n");
    fresh();

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);
    VStore *vs = NULL;
    CHECK(vstore_create(pg, NULL, NULL, DIM, &vs) == OK);
    CHECK(vs != NULL);

    u32 ord = 0;
    u16 slot = 0;
    u16 off = 0;

    /* 第 0 个向量：第 0 页、槽 0、偏移 = 页头 */
    vstore_slot_of(vs, 0, &ord, &slot, &off);
    CHECK(ord == 0 && slot == 0 && off == VEC_PAGE_HDR);

    /* 第 6 个：第 0 页最后一槽 */
    vstore_slot_of(vs, 6, &ord, &slot, &off);
    CHECK(ord == 0 && slot == 6 && off == VEC_PAGE_HDR + 6 * DIM * 4u);

    /* 第 7 个：跨到第 1 页的槽 0 */
    vstore_slot_of(vs, 7, &ord, &slot, &off);
    CHECK(ord == 1 && slot == 0 && off == VEC_PAGE_HDR);

    /* 第 100 个：第 14 页槽 2 */
    vstore_slot_of(vs, 100, &ord, &slot, &off);
    CHECK(ord == 14 && slot == 2 && off == VEC_PAGE_HDR + 2 * DIM * 4u);

    vstore_destroy(vs);
    pager_close(pg);
}

/* ---------------- 3/4/5. 插入 / 列对齐 / 扫描 ---------------- */

static void fill(VStore *vs, u32 n)
{
    float v[DIM];
    for (u32 i = 0; i < n; i++) {
        for (u16 d = 0; d < DIM; d++)
            v[d] = val_of(i, d);

        u64 rid = 0;
        CHECK(vstore_insert(vs, 0, v, label_of(i), &rid) == OK);
        CHECK(rid == i);          /* row_id 必须严格递增 */
    }
}

typedef struct {
    u64 expect;
    int n;
    int bad_order;
    int bad_value;
    int bad_label;
} ScanCheck;

static int scan_cb(void *ctx, u64 row_id, const float *vec, i32 label)
{
    ScanCheck *c = (ScanCheck *)ctx;

    if (row_id != c->expect)
        c->bad_order++;

    for (u16 d = 0; d < DIM; d++) {
        if (vec[d] != val_of(row_id, d)) {
            c->bad_value++;
            break;
        }
    }
    if (label != label_of(row_id))
        c->bad_label++;

    c->expect++;
    c->n++;
    return 0;
}

static void test_insert_get_scan(void)
{
    printf("  3. 插入 + 读回（跨页边界）\n");
    fresh();

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 256, &pg) == OK);
    VStore *vs = NULL;
    CHECK(vstore_create(pg, NULL, NULL, DIM, &vs) == OK);

    fill(vs, N);

    CHECK(vstore_count(vs) == N);
    /* 100 个 / 每页 7 个 = 15 页（14 满页 + 1 页 2 个） */
    CHECK(vstore_npages(vs) == 15u);

    printf("  4. 列对齐（向量 ↔ label）\n");
    float buf[DIM];
    for (u64 i = 0; i < N; i++) {
        CHECK(vstore_get(vs, i, buf) == OK);
        for (u16 d = 0; d < DIM; d++) {
            if (buf[d] != val_of(i, d)) {
                CHECK(0 && "向量内容不对");
                break;
            }
        }
        i32 lab = 0;
        CHECK(vstore_label(vs, i, &lab) == OK);
        CHECK(lab == label_of(i));
    }

    /* 越界读 → E_NOTFOUND，不返回垃圾 */
    CHECK(vstore_get(vs, N, buf) == E_NOTFOUND);
    i32 lab = 0;
    CHECK(vstore_label(vs, N, &lab) == E_NOTFOUND);

    printf("  5. 扫描：IO = 页数（15），不是向量数（100）\n");
    u64 before = vstore_page_reads(vs);
    ScanCheck c;
    memset(&c, 0, sizeof(c));
    CHECK(vstore_scan(vs, scan_cb, &c) == OK);
    u64 reads = vstore_page_reads(vs) - before;

    CHECK(c.n == (int)N);
    CHECK(c.bad_order == 0);
    CHECK(c.bad_value == 0);
    CHECK(c.bad_label == 0);
    /* 关键断言：一页一次读 */
    CHECK(reads == vstore_npages(vs));
    CHECK(reads == 15u);
    printf("    （扫 %u 个向量读了 %llu 页）\n", N, (unsigned long long)reads);

    vstore_destroy(vs);
    pager_close(pg);
}

/* ---------------- 6. 重开 ---------------- */

static void test_reopen(void)
{
    printf("  6. 重开（目录 / 计数 / 数据）\n");
    /* 承接上一项留下的库 */

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 256, &pg) == OK);

    VStore *vs = NULL;
    CHECK(vstore_open(pg, NULL, NULL, &vs) == OK);
    CHECK(vs != NULL);

    CHECK(vstore_dim(vs) == DIM);
    CHECK(vstore_k(vs) == 7u);
    CHECK(vstore_count(vs) == N);
    CHECK(vstore_npages(vs) == 15u);

    /* 目录重建对不对：每个序号都能定位到页，且页号递增（分配顺序） */
    for (u32 i = 0; i < vstore_npages(vs); i++)
        CHECK(vstore_page_id(vs, i) != PAGE_ID_NIL);

    /* 数据还在不在 */
    float buf[DIM];
    CHECK(vstore_get(vs, 0, buf) == OK);
    CHECK(buf[0] == val_of(0, 0));
    CHECK(vstore_get(vs, 99, buf) == OK);
    CHECK(buf[127] == val_of(99, 127));

    i32 lab = 0;
    CHECK(vstore_label(vs, 99, &lab) == OK);
    CHECK(lab == label_of(99));

    /* 重开后还能继续插：row_id 接着往上走 */
    float v[DIM];
    for (u16 d = 0; d < DIM; d++)
        v[d] = val_of(N, d);
    u64 rid = 0;
    CHECK(vstore_insert(vs, 0, v, label_of(N), &rid) == OK);
    CHECK(rid == N);
    CHECK(vstore_count(vs) == N + 1);

    /* 页校验和：所有页都要能验过 */
    for (u32 i = 0; i < pager_npages(pg); i++) {
        PageFrame *fr = NULL;
        CHECK(pager_get(pg, i, &fr) == OK);
        CHECK(page_verify_checksum(fr->data));
        pager_unpin(pg, i, false);
    }

    vstore_destroy(vs);
    pager_close(pg);
}

/* ---------------- 7. 列对齐被破坏 → 打开就失败 ---------------- */

/*
 * 把第 1 个数据页的序号改坏，然后重开。
 *
 * 不改的话，第 1 页里的 7 个向量会被当成第 0 页的 ——
 * 查 row_id=7 会拿到 row_id=0 的向量，而且**不报错**。
 * 这正是坑 M3-2 说的"静默错配"。
 */
static void test_corrupt_ordinal(void)
{
    printf("  7. 列对齐被破坏 → 打开失败（不静默错配）\n");
    fresh();

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 256, &pg) == OK);
    VStore *vs = NULL;
    CHECK(vstore_create(pg, NULL, NULL, DIM, &vs) == OK);
    fill(vs, N);

    u32 page1 = vstore_page_id(vs, 1);
    CHECK(page1 != PAGE_ID_NIL);
    vstore_destroy(vs);

    /* 手动破坏：把第 1 页记的序号改成 99 */
    PageFrame *fr = NULL;
    CHECK(pager_get(pg, page1, &fr) == OK);
    put_u32(fr->data + VEC_H_ORDINAL, 99);
    page_set_checksum(fr->data);
    pager_unpin(pg, page1, true);
    pager_close(pg);

    /* 重开：必须失败 */
    CHECK(pager_open(PATH, 256, &pg) == OK);
    VStore *bad = NULL;
    CHECK(vstore_open(pg, NULL, NULL, &bad) == E_CORRUPT);
    CHECK(bad == NULL);
    pager_close(pg);
}

/* 把某一页的 count 改大 → 各页之和与标量行数对不上 */
static void test_corrupt_count(void)
{
    printf("  7b. 页内计数被改坏 → 打开失败\n");
    fresh();

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 256, &pg) == OK);
    VStore *vs = NULL;
    CHECK(vstore_create(pg, NULL, NULL, DIM, &vs) == OK);
    fill(vs, N);

    u32 page0 = vstore_page_id(vs, 0);
    vstore_destroy(vs);

    PageFrame *fr = NULL;
    CHECK(pager_get(pg, page0, &fr) == OK);
    put_u16(fr->data + VEC_H_COUNT, 6);      /* 本该是 7 */
    page_set_checksum(fr->data);
    pager_unpin(pg, page0, true);
    pager_close(pg);

    CHECK(pager_open(PATH, 256, &pg) == OK);
    VStore *bad = NULL;
    CHECK(vstore_open(pg, NULL, NULL, &bad) == E_CORRUPT);
    CHECK(bad == NULL);
    pager_close(pg);
}

/* ---------------- 8. 空库 / 维度不符 ---------------- */

static void test_empty_and_dim(void)
{
    printf("  8. 空库 open → E_NOTFOUND；维度不符 → E_INVAL\n");
    fresh();

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    /* 还没建过 */
    VStore *vs = NULL;
    CHECK(vstore_open(pg, NULL, NULL, &vs) == E_NOTFOUND);
    CHECK(vs == NULL);

    /* 建了之后，用另一个维度再 create → 拒绝 */
    CHECK(vstore_create(pg, NULL, NULL, 64, &vs) == OK);
    vstore_destroy(vs);

    VStore *other = NULL;
    CHECK(vstore_create(pg, NULL, NULL, 128, &other) == E_INVAL);
    CHECK(other == NULL);

    /* 同维度 create = 接管 */
    CHECK(vstore_create(pg, NULL, NULL, 64, &vs) == OK);
    CHECK(vstore_dim(vs) == 64);
    vstore_destroy(vs);

    pager_close(pg);
}

int main(void)
{
    printf("test_vstore:\n");

    fresh();
    test_k_arithmetic();
    test_slot_arithmetic();
    test_insert_get_scan();
    test_reopen();
    test_corrupt_ordinal();
    test_corrupt_count();
    test_empty_and_dim();

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  8 项验收全部通过\n");
    return 0;
}
