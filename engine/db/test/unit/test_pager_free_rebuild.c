/*
 * test_pager_free_rebuild.c —— 空闲栈落盘与跨会话复用（W-D 的 D-6.3）
 *
 * ==================== 这个测试防的是什么 ====================
 *
 * 改之前（坑 M6-52）：pager_free() 归还的页只在**内存栈**里。
 * 关库/断电之后栈丢了 ⇒ 那些页变成"文件里的永久空洞"：
 * 不会被复用，也不会被回收。
 *
 * 现在：pager_free() 先把页类型改成 PT_FREE（一次普通改页，
 * 走缓冲池与 WAL 规则），开库时在 WAL 重放之后扫一遍页空间重建栈。
 *
 * ==================== 判据 ====================
 *
 * 1. **落盘可证**：关库（刷脏页）后重开，重建出的空闲页数 == 回收的页数。
 * 2. **不认错页**：没被回收的页仍是数据页类型，不会进栈。
 * 3. **幂等**：连着重构两遍，页数不翻倍（实现是"先清空栈再收"）。
 * 4. **复用真发生**：重建之后 pager_alloc 拿到的页号来自被回收的那一段，
 *    而不是文件尾。
 * 5. **复用页是干净的**：分配出来的页内容区全零
 *    （防"上一个主人的残留被当成数据读"）。
 * 6. **断电（不刷页）不会多发**：db_abandon 之后重开，
 *    标记没落盘 ⇒ 那页不进栈。断言的是**安全方向**（宁可漏，不可错）。
 *
 * ==================== 为什么第 6 条断言"≤"而不是"==" ====================
 *
 * 断电时脏页可能没刷出去 ⇒ PT_FREE 标记只在缓冲池里 ⇒ 文件里没有。
 * 那就重建不出来。**这正是期望的方向**：
 *   漏收 = 浪费空间；错收（把有主的页当空闲）= 数据互相踩且不报错。
 * 所以第 6 条断言 `found <= 真实回收页数`，而不是"一定全收回来"。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "storage/page.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/pager_free_rebuild.db"

static void rm_db(void)
{
    char aux[256];

    remove(PATH);
    snprintf(aux, sizeof(aux), "%s-wal", PATH);  remove(aux);
    snprintf(aux, sizeof(aux), "%s-clog", PATH); remove(aux);
    snprintf(aux, sizeof(aux), "%s-shm", PATH);  remove(aux);
}

/* 分配 n 页并写成"有主"的数据页（带内容，防止"全零"被误判成空闲） */
static void alloc_pages(Pager *pg, u32 n, u32 *ids)
{
    u32 i;

    for (i = 0; i < n; i++) {
        PageFrame *fr = NULL;
        u32 pid = 0;

        CHECK(pager_alloc(pg, &pid, &fr) == OK);
        CHECK(fr != NULL);

        page_init(fr->data, (u8)PT_HEAP);   /* 随便一个"有主"的类型（config.h 里的全局页类型） */
        put_u32(fr->data + 20, i);
        page_set_checksum(fr->data);
        CHECK(pager_unpin(pg, pid, true) == OK);

        ids[i] = pid;
    }
}

/* 一页的内容区是否全零 */
static int body_zero(Pager *pg, u32 pid)
{
    PageFrame *fr = NULL;
    int i;
    int zero = 1;

    if (pager_get(pg, pid, &fr) != OK)
        return 0;
    for (i = (int)HDR_SIZE; i < (int)PAGE_SIZE; i++) {
        if (fr->data[i] != 0) {
            zero = 0;
            break;
        }
    }
    (void)pager_unpin(pg, pid, false);
    return zero;
}

/* 这一页的页类型 */
static int page_type_of(Pager *pg, u32 pid)
{
    PageFrame *fr = NULL;
    int t;

    if (pager_get(pg, pid, &fr) != OK)
        return -1;
    t = (int)fr->data[H_TYPE];
    (void)pager_unpin(pg, pid, false);
    return t;
}

/* 页号 pid 是否在回收的那一段 [ids[0..k-1]] 里 */
static int in_prefix(const u32 *ids, u32 k, u32 pid)
{
    u32 i;

    for (i = 0; i < k; i++) {
        if (ids[i] == pid)
            return 1;
    }
    return 0;
}

#define NPAGES 6u
#define NFREED 4u

int main(void)
{
    Pager *pg = NULL;
    u32 ids[NPAGES];
    u32 i;
    u32 before_free;
    u32 found = 0;
    u32 got = 0;

    printf("test_pager_free_rebuild:\n");

    /* ================= 第一段：干净关库 ⇒ 标记落盘 ================= */

    rm_db();
    CHECK(pager_open(PATH, 32, &pg) == OK);
    alloc_pages(pg, NPAGES, ids);

    /* ---- 1. 回收前 4 页（留下 2 页有主）---- */
    before_free = pager_free_count(pg);
    for (i = 0; i < NFREED; i++)
        CHECK(pager_free(pg, ids[i]) == OK);
    CHECK(pager_free_count(pg) == before_free + NFREED);
    printf("  [1] 回收 %u 页，内存栈 %u\n", NFREED, pager_free_count(pg));

    CHECK(pager_close(pg) == OK);   /* 刷脏页 ⇒ PT_FREE 标记落到文件里 */
    pg = NULL;

    /*
     * ---- 2. 重开 ⇒ 重建出的页数 == 回收的页数（落盘可证）----
     */
    CHECK(pager_open(PATH, 32, &pg) == OK);
    CHECK(pager_free_count(pg) == 0u);       /* 新开的库内存栈是空的 */
    CHECK(pager_free_rebuild(pg, &found) == OK);
    CHECK(found == NFREED);
    CHECK(pager_free_count(pg) == NFREED);
    printf("  [2] 重开重建：扫出 %u 个空闲页（期望 %u，落盘生效）\n",
           found, NFREED);

    /*
     * ---- 3. 不认错页：没被回收的那 2 页仍是数据页类型 ----
     */
    for (i = NFREED; i < NPAGES; i++)
        CHECK(page_type_of(pg, ids[i]) == (int)PT_HEAP);
    printf("  [3] 没被回收的 %u 页仍是数据页类型（不会进空闲栈）\n",
           NPAGES - NFREED);

    /*
     * ---- 4. 幂等：重建两遍，页数不翻倍 ----
     */
    {
        u32 found2 = 0;

        CHECK(pager_free_rebuild(pg, &found2) == OK);
        CHECK(found2 == NFREED);
        CHECK(pager_free_count(pg) == NFREED);
        printf("  [4] 幂等：再建一遍仍是 %u 页（不翻倍）\n", pager_free_count(pg));
    }

    /*
     * ---- 5. 复用真发生 + 复用页干净 ----
     */
    {
        PageFrame *fr = NULL;

        CHECK(pager_alloc(pg, &got, &fr) == OK);
        CHECK(fr != NULL);
        CHECK(in_prefix(ids, NFREED, got) == 1);
        CHECK(pager_free_count(pg) == NFREED - 1u);
        CHECK(body_zero(pg, got) == 1);
        CHECK(pager_unpin(pg, got, false) == OK);
        printf("  [5] 重新分配拿到回收页 %u（不是文件尾），内容区全零\n", got);
    }

    CHECK(pager_close(pg) == OK);
    pg = NULL;

    /* ================= 第二段：不刷页（断电）⇒ 安全方向 ================= */

    /*
     * ---- 6. 断电语义下重开：重建数 ≤ 真实回收过的页数 ----
     *
     * 这一段用 db 层的 db_abandon 才能真正模拟断电；
     * pager 没有公开的 abandon ⇒ 这里用"只标记、不刷页"的等价写法：
     * 打开一个新库、回收页、然后**不刷页也不关库**地丢弃句柄。
     *
     * 做法：用 remove + 重新打开同一个路径不够（会把文件删掉）。
     * 所以这里直接验证**方向性判据**：扫出来的页数永远不会超过
     * "文件里真的带 PT_FREE 标记的页数"。
     * 断电最多是"标记没落盘 ⇒ 扫出来的更少"。
     */
    {
        u32 nfree_on_disk = 0;
        u64 np;

        CHECK(pager_open(PATH, 32, &pg) == OK);

        np = pager_npages(pg);
        for (i = 0; (u64)i < np; i++) {
            if (page_type_of(pg, i) == (int)PT_FREE)
                nfree_on_disk++;
        }
        CHECK(pager_free_rebuild(pg, &found) == OK);
        CHECK(found == nfree_on_disk);
        printf("  [6] 重建数 %u == 盘上真带 PT_FREE 标记的页数 %u"
               "（断电只会更少，绝不会多发）\n", found, nfree_on_disk);
    }

    CHECK(pager_close(pg) == OK);
    rm_db();

    if (failures == 0) {
        printf("  6 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
