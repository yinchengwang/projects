/*
 * test_json_ovf.c —— 大 JSON 文档外置页链的验收（v1.6 步骤 3 · T15）
 *
 * 七组验收：
 *   1. **尺寸档位**：0 B / 1 B / 正好一页 / 两页 / 三页 / 末页不满。
 *   2. **逐字节往返**：N 从 0 到 20000 逐步长，写进去读出来**逐字节相同**。
 *   3. **两次调用语义**：buf == NULL ⇒ 只报长度；cap 不够 ⇒ E_NOSPACE
 *      （并报出需要多少）；缓冲够 ⇒ OK 且内容正确。
 *      另外**用值层走一遍**（val_external_text + val_detoast），
 *      证明"契约一行不改"这句话是真的。
 *   4. **页数**：json_ovf_page_count 与**独立走一遍链**数出来的页数相同。
 *   5. **坏数据必须报错**（不是返回垃圾）：页类型被改 / 校验和被改 /
 *      链被截断（next 指向不存在的页）/ payload_len 超过页容量。
 *   6. **页真的还回去了**：释放后全局空闲页计数增加，且能立刻被重新分配。
 *   7. **阈值判定**：json_should_ovf 的分界点必须是 JSON_INLINE_MAX。
 *
 * 【为什么"坏数据必须报错"要单独一组】
 *
 * 外置页链是**惰性读**的：行里只有一个句柄，内容在读的时候才从页链里取。
 * 如果读路径不校验，用户拿到的就是**被改过的文档**，而且没有任何报错 ——
 * 比"查不到"糟糕得多（查不到至少知道出事了）。
 *
 * ⚠️ 步骤 5 的"不建树的读路径"不在本步骤范围内（那是读路径的整体设计）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "modalities/doc/json_ovf.h"
#include "storage/page.h"
#include "storage/pager.h"
#include "storage/value.h"

#define PATH "test/tmp/g15_json_ovf.db"

typedef struct {
    u32 calls;
    u32 next_lsn;
    u32 bad_calls;
} WalCapture;

static int capture_wal(void *ctx, u32 xid, u8 logtype, u32 page_id,
                       u16 slot, u16 slot_off,
                       const u8 *before, u16 before_len,
                       const u8 *after, u16 after_len,
                       u32 *lsn_out)
{
    WalCapture *cap = (WalCapture *)ctx;
    (void)xid;
    (void)slot;
    (void)slot_off;
    if (logtype != LOG_PAGEDATA || page_id == PAGE_ID_NIL ||
        before == NULL || before_len != PAGE_SIZE ||
        after == NULL || after_len != PAGE_SIZE || lsn_out == NULL) {
        cap->bad_calls++;
        return E_INVAL;
    }
    cap->calls++;
    *lsn_out = ++cap->next_lsn;
    return OK;
}

static int failures = 0;
static Pager *pg = NULL;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---------------- 小工具 ---------------- */

/* 造一份可复现的内容（同一个 N 永远得到同样的字节） */
static void fill(u8 *buf, u32 n, u32 seed)
{
    u32 i;

    for (i = 0; i < n; i++)
        buf[i] = (u8)((i * 37u) ^ (i >> 5) ^ (seed * 11u) ^ 0x5Au);
}

/* 这条链应该有几页（按页容量算） */
static u32 expect_pages(u32 len)
{
    if (len == 0)
        return 1u;   /* 空文档也有明确表示：一个 payload_len = 0 的页 */
    return (len + JSON_OVF_PAY_CAP - 1u) / JSON_OVF_PAY_CAP;
}

/* ---------------- 第 1 组：尺寸档位 ---------------- */

static void t_sizes(void)
{
    static const u32 sizes[] = {
        0u,                        /* 空文档 */
        1u,                        /* 最小 */
        JSON_OVF_PAY_CAP,          /* 正好一页（满） */
        JSON_OVF_PAY_CAP + 1u,     /* 两页，第二页只 1 字节 */
        JSON_OVF_PAY_CAP * 2u,     /* 两页都满 */
        JSON_OVF_PAY_CAP * 2u + 1u,/* 三页，末页不满 */
        JSON_OVF_PAY_CAP * 3u      /* 三页都满 */
    };
    u32 si;

    printf("  [1] 尺寸档位\n");

    for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        u32 n = sizes[si];
        u8 *src = (u8 *)malloc(n ? n : 1u);
        u8 *dst = (u8 *)malloc(n ? n : 1u);
        u64 key = 0;
        u32 npages = 0;
        u16 len = 0;
        int rc;

        if (src == NULL || dst == NULL) {
            CHECK(0);
            free(src);
            free(dst);
            return;
        }

        fill(src, n, si + 1u);

        rc = json_ovf_write(pg, NULL, src, n, &key);
        CHECK(rc == OK);
        if (rc != OK) {
            free(src);
            free(dst);
            return;
        }

        /* 页数必须与算出来的一致 */
        rc = json_ovf_page_count(pg, key, &npages);
        CHECK(rc == OK);
        CHECK(npages == expect_pages(n));

        /* 读回来：长度与内容 */
        memset(dst, 0xCC, n ? n : 1u);
        rc = json_ovf_read(pg, key, dst, (u16)n, &len);
        CHECK(rc == OK);
        CHECK(len == n);
        if (n > 0)
            CHECK(memcmp(src, dst, n) == 0);

        /* 释放 */
        CHECK(json_ovf_free(pg, key) == OK);

        free(src);
        free(dst);
    }
}

/* ---------------- 第 2 组：逐字节往返 ---------------- */

/*
 * N 从 0 到 20000 逐步长，每个 N 都写进去、读出来、逐字节比较。
 *
 * ⚠️ 每一轮都释放 ⇒ 页被回收进空闲栈，下一轮复用 ⇒ 文件不会长到 20000 页。
 */
static void t_roundtrip(void)
{
    u32 cap = 20001u;
    u8 *src = (u8 *)malloc(cap);
    u8 *dst = (u8 *)malloc(cap);
    u32 n;

    printf("  [2] 逐字节往返（N = 0..20000）\n");

    if (src == NULL || dst == NULL) {
        CHECK(0);
        free(src);
        free(dst);
        return;
    }

    for (n = 0; n < cap; n++) {
        u64 key = 0;
        u16 len = 0;
        int rc;

        fill(src, n, n);

        rc = json_ovf_write(pg, NULL, src, n, &key);
        if (rc != OK) {
            CHECK(rc == OK);
            break;
        }

        memset(dst, 0xCC, cap);
        rc = json_ovf_read(pg, key, dst, (u16)n, &len);
        if (rc != OK || len != n || (n > 0 && memcmp(src, dst, n) != 0)) {
            CHECK(rc == OK);
            CHECK(len == n);
            CHECK(n == 0 || memcmp(src, dst, n) == 0);
            (void)json_ovf_free(pg, key);
            break;
        }

        rc = json_ovf_free(pg, key);
        if (rc != OK) {
            CHECK(rc == OK);
            break;
        }
    }

    free(src);
    free(dst);
}

/* ---------------- 第 3 组：两次调用语义 ---------------- */

/*
 * DetoastFn 的约定（见 src/storage/value.h）：
 *   第一次 buf == NULL ⇒ 只报长度；
 *   第二次 buf != NULL ⇒ 取内容；cap 不够 ⇒ E_NOSPACE（并报出需要多少）。
 */
static void t_two_call(void)
{
    u32 n = 5000u;
    u8 *src = (u8 *)malloc(n);
    u8 *buf = (u8 *)malloc(n);
    u8 *small = (u8 *)malloc(64);
    u64 key = 0;
    u16 len = 0;
    int rc;

    printf("  [3] 两次调用语义\n");

    if (src == NULL || buf == NULL || small == NULL) {
        CHECK(0);
        free(src); free(buf); free(small);
        return;
    }

    fill(src, n, 99u);
    CHECK(json_ovf_write(pg, NULL, src, n, &key) == OK);

    /* 第一次：只问长度 */
    len = 0;
    rc = json_ovf_read(pg, key, NULL, 0, &len);
    CHECK(rc == E_NOSPACE || rc == OK);
    CHECK(len == n);

    /* cap 不够 ⇒ E_NOSPACE，并报出需要多少 */
    len = 0;
    rc = json_ovf_read(pg, key, small, 64, &len);
    CHECK(rc == E_NOSPACE);
    CHECK(len == n);

    /* 缓冲够 ⇒ OK + 内容正确 */
    len = 0;
    memset(buf, 0xCC, n);
    rc = json_ovf_read(pg, key, buf, (u16)n, &len);
    CHECK(rc == OK);
    CHECK(len == n);
    CHECK(memcmp(src, buf, n) == 0);

    /* 适配器（DetoastFn）也是同一套语义 */
    len = 0;
    rc = json_ovf_detoast(pg, key, 0, NULL, 0, &len);
    CHECK(rc == E_NOSPACE || rc == OK);
    CHECK(len == n);

    /* 走值层：val_external_text + val_detoast —— "契约一行不改"的实证 */
    {
        DbValue v;
        u8 *mat = NULL;
        u32 mat_len = 0;

        val_external_text(&v, key, 0, json_ovf_detoast, pg);
        rc = val_detoast(&v, &mat, &mat_len);
        CHECK(rc == OK);
        CHECK(mat_len == n);
        if (mat != NULL && mat_len == n)
            CHECK(memcmp(src, mat, n) == 0);
        free(mat);
    }

    CHECK(json_ovf_free(pg, key) == OK);

    free(src); free(buf); free(small);
}

/* ---------------- 第 4 组：页数 ---------------- */

/* 独立走一遍链（不调 json_ovf_page_count）数出真实页数 */
static u32 walk_chain(Pager *p, u64 key)
{
    u32 pid = (u32)key;
    u32 cnt = 0;

    while (pid != PAGE_ID_NIL && cnt < 1024u) {
        PageFrame *fr = NULL;
        u32 cur = pid;

        if (pager_get(p, cur, &fr) != OK)
            break;
        pid = get_u32(fr->data + JSON_OVF_NEXT_OFF);
        (void)pager_unpin(p, cur, false);
        cnt++;
    }
    return cnt;
}

static void t_page_count(void)
{
    u32 n = JSON_OVF_PAY_CAP * 3u + 7u;
    u8 *src = (u8 *)malloc(n);
    u64 key = 0;
    u32 npages = 0;
    u32 real = 0;

    printf("  [4] 页数\n");

    if (src == NULL) {
        CHECK(0);
        return;
    }

    fill(src, n, 7u);
    CHECK(json_ovf_write(pg, NULL, src, n, &key) == OK);

    CHECK(json_ovf_page_count(pg, key, &npages) == OK);
    real = walk_chain(pg, key);
    CHECK(npages == real);
    CHECK(npages == expect_pages(n));

    CHECK(json_ovf_free(pg, key) == OK);
    free(src);
}

/* ---------------- 第 5 组：坏数据必须报错 ---------------- */

/*
 * 四个坏法，每一个都必须**报错**，不能返回垃圾：
 *   (a) 页类型被改   → 页类型检查抓到
 *   (b) 校验和被改   → 校验和检查抓到
 *   (c) 链被截断     → next 指向不存在的页 ⇒ 取页失败
 *   (d) payload_len 超过页的实际容量 → 长度检查抓到
 *
 * ⚠️ (a)(c)(d) 改完之后要**重算校验和** ——
 *   否则被校验和检查先抓到，测的就不是"页类型/链/长度"这三件事了。
 */
static void t_corruption(void)
{
    u32 n = JSON_OVF_PAY_CAP + 100u;
    u8 *src = (u8 *)malloc(n);
    u8 *buf = (u8 *)malloc(n);

    printf("  [5] 坏数据必须报错\n");

    if (src == NULL || buf == NULL) {
        CHECK(0);
        free(src); free(buf);
        return;
    }

    fill(src, n, 11u);

    /* --- (a) 页类型被改 --- */
    {
        u64 key = 0;
        u16 len = 0;
        PageFrame *fr = NULL;

        CHECK(json_ovf_write(pg, NULL, src, n, &key) == OK);
        CHECK(pager_get(pg, (u32)key, &fr) == OK);
        if (fr != NULL) {
            fr->data[H_TYPE] = 0x02;   /* 假装自己是 heap 页 */
            page_set_checksum(fr->data);
            CHECK(pager_unpin(pg, (u32)key, true) == OK);
        }
        CHECK(json_ovf_read(pg, key, buf, (u16)n, &len) != OK);

        /* 改回来，好让这一条链还能正常释放 */
        CHECK(pager_get(pg, (u32)key, &fr) == OK);
        if (fr != NULL) {
            fr->data[H_TYPE] = (u8)PT_JSON_OVF;
            page_set_checksum(fr->data);
            CHECK(pager_unpin(pg, (u32)key, true) == OK);
        }
        CHECK(json_ovf_free(pg, key) == OK);
    }

    /* --- (b) 校验和被改 --- */
    {
        u64 key = 0;
        u16 len = 0;
        PageFrame *fr = NULL;

        CHECK(json_ovf_write(pg, NULL, src, n, &key) == OK);
        CHECK(pager_get(pg, (u32)key, &fr) == OK);
        if (fr != NULL) {
            fr->data[JSON_OVF_DATA_OFF + 5] ^= 0xFFu;   /* 改内容，不改校验和 */
            CHECK(pager_unpin(pg, (u32)key, true) == OK);
        }
        CHECK(json_ovf_read(pg, key, buf, (u16)n, &len) != OK);

        /* 改回来 */
        CHECK(pager_get(pg, (u32)key, &fr) == OK);
        if (fr != NULL) {
            fr->data[JSON_OVF_DATA_OFF + 5] ^= 0xFFu;
            page_set_checksum(fr->data);
            CHECK(pager_unpin(pg, (u32)key, true) == OK);
        }
        CHECK(json_ovf_free(pg, key) == OK);
    }

    /* --- (c) 链被截断：next 指向不存在的页 --- */
    {
        u64 key = 0;
        u16 len = 0;
        PageFrame *fr = NULL;

        CHECK(json_ovf_write(pg, NULL, src, n, &key) == OK);
        CHECK(pager_get(pg, (u32)key, &fr) == OK);
        if (fr != NULL) {
            put_u32(fr->data + JSON_OVF_NEXT_OFF, 0x0FFFFFFEu);  /* 不存在的页 */
            page_set_checksum(fr->data);
            CHECK(pager_unpin(pg, (u32)key, true) == OK);
        }
        CHECK(json_ovf_read(pg, key, buf, (u16)n, &len) != OK);

        /* 改回来 */
        CHECK(pager_get(pg, (u32)key, &fr) == OK);
        if (fr != NULL) {
            put_u32(fr->data + JSON_OVF_NEXT_OFF, PAGE_ID_NIL);
            page_set_checksum(fr->data);
            CHECK(pager_unpin(pg, (u32)key, true) == OK);
        }
        CHECK(json_ovf_free(pg, key) == OK);
    }

    /* --- (d) payload_len 超过页容量 --- */
    {
        u64 key = 0;
        u16 len = 0;
        PageFrame *fr = NULL;

        CHECK(json_ovf_write(pg, NULL, src, n, &key) == OK);
        CHECK(pager_get(pg, (u32)key, &fr) == OK);
        if (fr != NULL) {
            put_u32(fr->data + JSON_OVF_LEN_OFF, 0xFFFFFFFFu);
            page_set_checksum(fr->data);
            CHECK(pager_unpin(pg, (u32)key, true) == OK);
        }
        CHECK(json_ovf_read(pg, key, buf, (u16)n, &len) != OK);

        /* 改回来 */
        CHECK(pager_get(pg, (u32)key, &fr) == OK);
        if (fr != NULL) {
            put_u32(fr->data + JSON_OVF_LEN_OFF, JSON_OVF_PAY_CAP);
            page_set_checksum(fr->data);
            CHECK(pager_unpin(pg, (u32)key, true) == OK);
        }
        CHECK(json_ovf_free(pg, key) == OK);
    }

    free(src); free(buf);
}

/* ---------------- 第 6 组：页真的还回去了 ---------------- */

static void t_free(void)
{
    u32 n = JSON_OVF_PAY_CAP * 2u + 5u;
    u8 *src = (u8 *)malloc(n);
    u8 *buf = (u8 *)malloc(n);
    u64 key = 0;
    u32 npages = 0;
    u32 before = 0;
    u32 after = 0;
    u16 len = 0;

    printf("  [6] 页真的还回去了\n");

    if (src == NULL || buf == NULL) {
        CHECK(0);
        free(src); free(buf);
        return;
    }

    fill(src, n, 23u);

    CHECK(json_ovf_write(pg, NULL, src, n, &key) == OK);
    CHECK(json_ovf_page_count(pg, key, &npages) == OK);
    CHECK(npages == expect_pages(n));

    before = pager_free_count(pg);
    CHECK(json_ovf_free(pg, key) == OK);
    after = pager_free_count(pg);
    CHECK(after == before + npages);

    /* 释放之后不能再读到内容（页已经清零 ⇒ 页类型不对 ⇒ 报错） */
    CHECK(json_ovf_read(pg, key, buf, (u16)n, &len) != OK);

    /* 空闲页真的能被重新分配出来（同一个会话内） */
    {
        PageFrame *fr = NULL;
        u32 pid = 0;

        CHECK(pager_alloc(pg, &pid, &fr) == OK);
        if (fr != NULL)
            CHECK(pager_unpin(pg, pid, true) == OK);
    }

    free(src); free(buf);
}

/* ---------------- 第 9 组：写入中途失败的回滚 ---------------- */

/*
 * 【为什么不再伪造"页池耗尽"】
 *
 * 老实现是"边分配边写"，第一页还 pin 在手里就要求第二帧 ⇒
 * 单帧池自然触发 E_NOSPACE。新实现改成"先一次拿齐全部页号、再逐页写内容"，
 * 每页拿到后立刻 unpin ⇒ **同时只 pin 一帧**，池子再小也不会因为
 * "占着不放"而耗尽。那条故障路径已经不存在了。
 *
 * 现在真正可达的失败点是 **WAL 写失败**：第 N 页的日志写不进去，
 * 这一页就不许改，整条链必须回滚。这正是本组要验的。
 *
 * 要求：
 *   - 返回那个 WAL 错误，不是"看起来成功"；
 *   - ext_key_out 保持 PAGE_ID_NIL（调用方拿不到句柄）；
 *   - 整条链的页全部回到空闲栈（一页都不泄漏）；
 *   - 之后还能正常写一份新文档（帧确实没被占死）。
 */
typedef struct {
    u32 fail_at_call;   /* 第几次 wal 调用开始失败（1 起算；0 = 从不失败） */
    u32 calls;
} WalFail;

static int failing_wal(void *ctx, u32 xid, u8 logtype, u32 page_id,
                       u16 slot, u16 slot_off,
                       const u8 *before, u16 before_len,
                       const u8 *after, u16 after_len,
                       u32 *lsn_out)
{
    WalFail *f = (WalFail *)ctx;
    (void)xid; (void)logtype; (void)page_id;
    (void)slot; (void)slot_off; (void)before; (void)before_len;
    (void)after; (void)after_len;

    f->calls++;
    if (f->fail_at_call != 0 && f->calls >= f->fail_at_call)
        return E_IO;               /* 日志写不进去 */
    if (lsn_out)
        *lsn_out = f->calls;
    return OK;
}

static void t_write_wal_fail_rollback(void)
{
    const char *path = "test/tmp/g15_json_ovf_rollback.db";
    u32 n = JSON_OVF_PAY_CAP * 2u;   /* 恰好两页 */
    u8 *src = (u8 *)malloc(n);
    u64 key = 999;
    u32 free_before;
    WalFail wf;
    JsonOvfWal wal;
    Pager *own = NULL;
    int rc;

    printf("  [9] 写入中途 WAL 失败：整条链回滚\n");

    if (src == NULL) {
        CHECK(0);
        return;
    }
    fill(src, n, 91u);

    /*
     * 用**独立的、空闲栈为空**的 pager：
     * 主 pg 的空闲栈里已有别的测试回收的页，写两页时可能先从栈里弹出 ⇒
     * "回滚后空闲页 +2" 这个断言会被分配时的 pop 抵掉，变得不可算。
     */
    remove(path);
    CHECK(pager_open(path, 32u, &own) == OK);
    if (own == NULL) {
        CHECK(0);
        free(src);
        return;
    }
    CHECK(pager_free_count(own) == 0u);

    /* 第 2 页的日志失败 ⇒ 第 1 页已经写好，必须一起回滚 */
    memset(&wf, 0, sizeof(wf));
    wf.fail_at_call = 2u;
    wal.fn = failing_wal;
    wal.ctx = &wf;
    wal.xid = 5u;

    free_before = pager_free_count(own);
    rc = json_ovf_write(own, &wal, src, n, &key);
    CHECK(rc != OK);
    CHECK(rc == E_IO);
    CHECK(key == (u64)PAGE_ID_NIL);        /* 调用方拿不到句柄 */

    /* 两页都要回到空闲栈 —— 一页都不许泄漏 */
    CHECK(pager_free_count(own) == free_before + 2u);

    /* 帧确实没被占死：同一池子还能再写一份两页文档并读回来 */
    {
        WalCapture capture;
        JsonOvfWal okwal;
        u64 key2 = PAGE_ID_NIL;
        u8 *dst = (u8 *)malloc(n);
        u16 len = 0;

        memset(&capture, 0, sizeof(capture));
        okwal.fn = capture_wal;
        okwal.ctx = &capture;
        okwal.xid = 6u;

        CHECK(json_ovf_write(own, &okwal, src, n, &key2) == OK);
        CHECK(key2 != (u64)PAGE_ID_NIL);
        if (dst != NULL) {
            CHECK(json_ovf_read(own, key2, dst, (u16)n, &len) == OK);
            CHECK(len == (u16)n);
            CHECK(memcmp(src, dst, n) == 0);
            free(dst);
        }
        CHECK(json_ovf_free(own, key2) == OK);
    }

    (void)pager_close(own);
    remove(path);
    free(src);
}

/* ---------------- 第 8 组：WAL 写入与幂等释放 ---------------- */

static void t_wal_and_idempotent_free(void)
{
    const u32 n = JSON_OVF_PAY_CAP + 9u;
    u8 *src = (u8 *)malloc(n);
    u64 key = PAGE_ID_NIL;
    WalCapture capture;
    JsonOvfWal wal;

    printf("  [8] WAL 写入与幂等释放\n");
    memset(&capture, 0, sizeof(capture));
    wal.fn = capture_wal;
    wal.ctx = &capture;
    wal.xid = 7u;
    if (src == NULL) {
        CHECK(0);
        return;
    }
    fill(src, n, 17u);

    CHECK(json_ovf_write(pg, &wal, src, n, &key) == OK);
    CHECK(capture.calls == expect_pages(n));
    CHECK(capture.bad_calls == 0);
    CHECK(json_ovf_free(pg, key) == OK);
    /* 第二次不能重复把同一页压入 freelist。 */
    CHECK(json_ovf_free(pg, key) == OK);
    CHECK(capture.bad_calls == 0);
    free(src);
}

/* ---------------- 第 7 组：阈值判定 ---------------- */
/*
 * 内联阈值的分界点必须是 JSON_INLINE_MAX ——
 * 写入路径与读取路径用**同一个**函数（json_ovf.h 里写明了这条纪律）。
 */
static void t_threshold(void)
{
    printf("  [7] 阈值判定\n");

    CHECK(json_should_ovf(0) == false);
    CHECK(json_should_ovf(JSON_INLINE_MAX) == false);
    CHECK(json_should_ovf(JSON_INLINE_MAX + 1u) == true);
    CHECK(json_should_ovf(100000u) == true);
}

/* ---------------- 主流程 ---------------- */

int main(void)
{
    int rc;

    printf("test_json_ovf —— 大 JSON 文档的外置页链\n");

    remove(PATH);
    rc = pager_open(PATH, 64, &pg);
    if (rc != OK || pg == NULL) {
        printf("  FAIL: pager_open rc=%d\n", rc);
        return 1;
    }

    t_sizes();
    t_roundtrip();
    t_two_call();
    t_page_count();
    t_corruption();
    t_free();
    t_threshold();
    t_write_wal_fail_rollback();
    t_wal_and_idempotent_free();

    (void)pager_close(pg);
    pg = NULL;
    remove(PATH);

    if (failures == 0) {
        printf("  7 项验收全部通过\n");
        return 0;
    }
    printf("  %d 项断言失败\n", failures);
    return 1;
}
