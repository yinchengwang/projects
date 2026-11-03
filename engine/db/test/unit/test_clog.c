/*
 * test_clog.c — v1.2 步骤 6 单元验收：分块 clog + clog 落盘
 *
 * 5 项：
 *   1. 分块：老 xid 的状态**不会被覆盖**（环形窗口的老毛病）
 *   2. 块边界：xid 65535 与 65536 落在不同的块里，状态互不干扰
 *   3. 落盘：sync 之后重开文件，状态原样还在
 *   4. 文件布局：第 xid 个字节就是 xid 的状态；文件长度 = 最大 xid + 1
 *   5. xid 接续：bump_xid 把 next_xid 推到 clog 里出现过的最大 xid 之后
 *
 * 【为什么第 1 项最重要】
 *
 * v0.7 的 clog 是个 4096 槽的环形窗口（clog[xid % 4096]）。
 * 窗口一满，老 xid 的状态就被新 xid 覆盖 →
 * 那些行**读不出来**（静默丢数据），而且不报错。
 *
 * 步骤 6 改成按高位分块（每块 65536 个 xid），老状态不再被覆盖。
 * 这一项就是守着这个改动的回归测试。
 *
 * 【为什么第 3 项是"静默丢数据"的守门人】
 *
 * WAL 截断之后，"哪些事务提交过"就只剩 clog 文件了。
 * 如果 clog 文件写错了（或者写完没落盘就截了日志），
 * 重启后所有历史事务都会被当成"进行中" → 已提交的数据全部不可见。
 * 这个 bug 不报错，只是"查不到东西了"。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "txn/txn.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define CLOG_PATH "test/tmp/clog_unit.clog"

/* ---- 1) 分块：老 xid 的状态不会被覆盖 ---- */
static void t1_chunked_no_overwrite(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    /*
     * 跑 5000 个事务 —— 超过老环形窗口的 4096 槽。
     * 一半提交、一半中止，好让两种状态都留下。
     */
    u32 first_xid = 0;
    u32 last_xid = 0;
    for (int i = 0; i < 5000; i++) {
        u32 x = 0;
        CHECK(txn_begin(&tt, &x) == OK);
        if (i == 0)
            first_xid = x;
        last_xid = x;
        if (i % 2)
            CHECK(txn_commit(&tt, x) == OK);
        else
            CHECK(txn_abort(&tt, x) == OK);
    }

    /* 最早那个事务的状态还在（环形窗口下这里早就被覆盖了） */
    /* i = 0 是偶数 → 中止 */
    CHECK(txn_clog_state(&tt, first_xid) == TXN_CLOG_ABORTED);
    CHECK(txn_aborted(&tt, first_xid) == true);

    /* 最后一个事务的状态也是对的（i = 4999 是奇数 → 提交） */
    CHECK(txn_clog_state(&tt, last_xid) == TXN_CLOG_COMMITTED);
    CHECK(txn_committed(&tt, last_xid) == true);

    /* 中间抽查几个：奇数号提交、偶数号中止 */
    int bad = 0;
    for (int i = 0; i < 5000; i += 97) {
        u32 x = first_xid + (u32)i;
        u8 want = (i % 2) ? TXN_CLOG_COMMITTED : TXN_CLOG_ABORTED;
        if (txn_clog_state(&tt, x) != want)
            bad++;
    }
    CHECK(bad == 0);
}

/* ---- 2) 块边界：两个块里的状态互不干扰 ---- */
static void t2_chunk_boundary(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    /*
     * 65535 是第 0 块的最后一个 xid，65536 是第 1 块的第一个 xid。
     * 两者必须各归各的块，状态不能串。
     */
    const u32 a = 65535u;
    const u32 b = 65536u;

    /*
     * next_xid 要先推上去 ——
     * "xid >= next_xid 视为进行中"这条规则会挡住还没分配过的高位 xid。
     * （db_open 里也是先 bump 再查 clog 的。）
     */
    txn_table_bump_xid(&tt, 200000u);

    txn_clog_set(&tt, a, TXN_CLOG_COMMITTED);
    txn_clog_set(&tt, b, TXN_CLOG_ABORTED);

    CHECK(txn_clog_state(&tt, a) == TXN_CLOG_COMMITTED);
    CHECK(txn_clog_state(&tt, b) == TXN_CLOG_ABORTED);

    /* 再在第二块里放一个，验证同一块内也独立 */
    txn_clog_set(&tt, 200000u, TXN_CLOG_COMMITTED);
    CHECK(txn_clog_state(&tt, 200000u) == TXN_CLOG_COMMITTED);
    CHECK(txn_clog_state(&tt, a) == TXN_CLOG_COMMITTED);
    CHECK(txn_clog_state(&tt, b) == TXN_CLOG_ABORTED);
}

/* ---- 3) 落盘：重开文件，状态原样还在 ---- */
static void t3_file_roundtrip(void)
{
    remove(CLOG_PATH);

    TxnTable a;
    txn_table_init(&a, 0);
    CHECK(txn_clog_open(&a, CLOG_PATH) == OK);

    txn_clog_set(&a, 5, TXN_CLOG_COMMITTED);
    txn_clog_set(&a, 9, TXN_CLOG_COMMITTED);
    txn_clog_set(&a, 70000, TXN_CLOG_ABORTED);   /* 第 1 块 */
    txn_clog_set(&a, 3, TXN_CLOG_ABORTED);

    CHECK(txn_clog_sync(&a) == OK);
    CHECK(txn_clog_max_xid(&a) == 70000);
    txn_clog_close(&a);

    /* 重新打开：db_open 走的就是这条路 */
    TxnTable b;
    txn_table_init(&b, 0);
    CHECK(txn_clog_open(&b, CLOG_PATH) == OK);

    /* 文件里的最大 xid 读出来了 */
    CHECK(txn_clog_max_xid(&b) == 70000);

    /*
     * next_xid 要推到 clog 里出现过的最大 xid 之后 ——
     * 否则"xid >= next_xid 视为进行中"的规则会把它们全挡掉。
     * db_open 里就是这么做的（txn_table_bump_xid）。
     */
    txn_table_bump_xid(&b, txn_clog_max_xid(&b));

    CHECK(txn_clog_state(&b, 5) == TXN_CLOG_COMMITTED);
    CHECK(txn_clog_state(&b, 9) == TXN_CLOG_COMMITTED);
    CHECK(txn_clog_state(&b, 70000) == TXN_CLOG_ABORTED);
    CHECK(txn_clog_state(&b, 3) == TXN_CLOG_ABORTED);

    txn_clog_close(&b);
}

/* ---- 4) 文件布局：第 xid 个字节就是 xid 的状态 ---- */
static void t4_file_layout(void)
{
    remove(CLOG_PATH);

    TxnTable a;
    txn_table_init(&a, 0);
    CHECK(txn_clog_open(&a, CLOG_PATH) == OK);

    txn_clog_set(&a, 0 + 1, TXN_CLOG_COMMITTED);   /* xid = 1 */
    txn_clog_set(&a, 10, TXN_CLOG_ABORTED);
    CHECK(txn_clog_sync(&a) == OK);
    txn_clog_close(&a);

    FILE *f = fopen(CLOG_PATH, "rb");
    CHECK(f != NULL);
    if (!f)
        return;

    unsigned char buf[16];
    memset(buf, 0, sizeof(buf));

    /* 第 1 个字节 = xid 1 的状态 */
    CHECK(fseek(f, 1, SEEK_SET) == 0);
    CHECK(fread(buf, 1, 1, f) == 1);
    CHECK(buf[0] == TXN_CLOG_COMMITTED);

    /* 第 10 个字节 = xid 10 的状态 */
    CHECK(fseek(f, 10, SEEK_SET) == 0);
    CHECK(fread(buf, 1, 1, f) == 1);
    CHECK(buf[0] == TXN_CLOG_ABORTED);

    /* 没用到的字节是 0 = 进行中 */
    CHECK(fseek(f, 5, SEEK_SET) == 0);
    CHECK(fread(buf, 1, 1, f) == 1);
    CHECK(buf[0] == TXN_CLOG_INPROGRESS);

    /* 文件长度 = 用过的最大 xid + 1 */
    CHECK(fseek(f, 0, SEEK_END) == 0);
    CHECK(ftell(f) == 11);

    fclose(f);
}

/* ---- 5) xid 接续 ---- */
static void t5_bump_xid(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    u32 x = 0;
    CHECK(txn_begin(&tt, &x) == OK);
    CHECK(txn_commit(&tt, x) == OK);

    /* 往后推：next_xid 必须大于被推的 xid */
    txn_table_bump_xid(&tt, 12345);
    u32 nx = 0;
    CHECK(txn_begin(&tt, &nx) == OK);
    CHECK(nx > 12345);

    /* 往回推无效（取 max） */
    u32 before = nx;
    txn_table_bump_xid(&tt, 3);
    u32 ny = 0;
    CHECK(txn_begin(&tt, &ny) == OK);
    CHECK(ny == before + 1);
}

int main(void)
{
    printf("test_clog（单元）:\n");

    t1_chunked_no_overwrite();
    t2_chunk_boundary();
    t3_file_roundtrip();
    t4_file_layout();
    t5_bump_xid();

    if (failures) {
        printf("  %d 项失败 [FAIL]\n", failures);
        return 1;
    }
    printf("  5 项验收全部通过 [PASS]\n");
    return 0;
}
