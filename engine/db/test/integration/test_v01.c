/*
 * test_v01.c — v0.1 端到端：pager + page + record + freelist 四层合起来
 *
 * 这是 v0.1 的真正验收：
 *   写 3000 行到文件 → 关闭 → 重开 → 按 RID 逐行读回 → 逐字节比对。
 *
 * 顺带验证两件容易漏的事：
 *   1. freelist 是派生数据，重开后要能从页内容重建（不需要持久化）
 *   2. 跨页、跨 slot、含 NULL 和变长列的行都能正确往返
 */

#include <stdio.h>
#include <string.h>

#include "storage/freelist.h"
#include "storage/page.h"
#include "storage/pager.h"
#include "storage/record.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define NROWS  3000
#define PATH   "test/tmp/v01.db"

static ColDesc schema[] = {
    { "id",    T_INT4, false },
    { "name",  T_TEXT, true  },
    { "score", T_INT8, false },
    { "note",  T_TEXT, true  },
};

/* Rid 已在 common/types.h 定义（v0.2 起统一） */
static Rid rids[NROWS];

/* 行内容只由 id 决定，便于比对 */
static int make_row(u32 id, u8 *buf, u16 bufsize, u16 *out_len)
{
    char name[32], note[64];
    Value vals[4];

    memset(vals, 0, sizeof vals);
    vals[0].u.i4 = (i32)id;
    vals[1].is_null = (id % 5 == 0);            /* 每 5 行 name 为 NULL */
    snprintf(name, sizeof name, "user-%u", id);
    vals[1].u.text.p = (const u8 *)name;
    vals[1].u.text.n = (u16)strlen(name);
    vals[2].u.i8 = (i64)id * 1000003LL;
    vals[3].is_null = (id % 3 == 0);            /* 每 3 行 note 为 NULL */
    snprintf(note, sizeof note, "note for %u with some padding", id);
    vals[3].u.text.p = (const u8 *)note;
    vals[3].u.text.n = (u16)strlen(note);

    return rec_encode(schema, 4, vals, buf, bufsize, out_len);
}

static bool row_matches(u32 id, const u8 *buf, u16 len)
{
    char name[32], note[64];
    Value out[4];

    if (rec_decode(schema, 4, buf, len, out) != OK)
        return false;
    if (out[0].is_null || out[0].u.i4 != (i32)id)
        return false;
    if (out[2].is_null || out[2].u.i8 != (i64)id * 1000003LL)
        return false;

    if (id % 5 == 0) {
        if (!out[1].is_null)
            return false;
    } else {
        snprintf(name, sizeof name, "user-%u", id);
        if (out[1].is_null || out[1].u.text.n != strlen(name))
            return false;
        if (memcmp(out[1].u.text.p, name, strlen(name)) != 0)
            return false;
    }

    if (id % 3 == 0) {
        if (!out[3].is_null)
            return false;
    } else {
        snprintf(note, sizeof note, "note for %u with some padding", id);
        if (out[3].is_null || out[3].u.text.n != strlen(note))
            return false;
        if (memcmp(out[3].u.text.p, note, strlen(note)) != 0)
            return false;
    }
    return true;
}

int main(void)
{
    Pager *pg = NULL;
    FreeList *fl = NULL;
    PageFrame *frame = NULL;
    u8 rec[512];
    u16 len;
    u32 i;
    u32 pages_used = 0;
    int rc;

    remove(PATH);

    /* ---------------- 写入阶段 ---------------- */
    rc = pager_open(PATH, 64, &pg);
    CHECK(rc == OK);
    if (rc != OK)
        return 1;

    rc = freelist_create((u32)pager_npages(pg), &fl);
    CHECK(rc == OK);

    for (i = 0; i < NROWS; i++) {
        CHECK(make_row(i, rec, sizeof rec, &len) == OK);

        u16 need = (u16)(len + SLOT_SIZE);
        u32 pid = freelist_find(fl, need);

        if (pid == PAGE_ID_NIL) {
            /* 没有页放得下：开新页 */
            rc = pager_alloc(pg, &pid, &frame);
            CHECK(rc == OK);
            if (rc != OK)
                break;
            pages_used++;
            CHECK(page_init(frame->data, PT_HEAP) == OK);
            rc = freelist_grow(fl, (u32)pager_npages(pg));
            CHECK(rc == OK);
            freelist_set(fl, pid, FL_USABLE);
        } else {
            rc = pager_get(pg, pid, &frame);
            CHECK(rc == OK);
            if (rc != OK)
                break;
        }

        u16 slot;
        rc = page_insert(frame->data, rec, len, &slot);
        CHECK(rc == OK);
        if (rc != OK) {
            pager_unpin(pg, pid, false);
            break;
        }

        /* 档位随实际剩余空间更新 —— 这是 freelist 的唯一数据来源 */
        freelist_set(fl, pid, page_free_bytes(frame->data));
        rids[i] = (Rid){ pid, slot };

        rc = pager_unpin(pg, pid, true);
        CHECK(rc == OK);
        if (failures > 3)
            break;
    }

    CHECK(i == NROWS);
    CHECK(pages_used > 1);        /* 确认真的跨了多页 */

    freelist_destroy(fl);
    CHECK(pager_close(pg) == OK);

    /* ---------------- 重开 + 读回阶段 ---------------- */
    rc = pager_open(PATH, 16, &pg);      /* 故意用小池子，强制逐出 */
    CHECK(rc == OK);
    if (rc != OK)
        return 1;

    u32 bad = 0;
    for (i = 0; i < NROWS; i++) {
        rc = pager_get(pg, rids[i].page, &frame);
        CHECK(rc == OK);
        if (rc != OK)
            break;

        const u8 *got;
        u16 got_len;
        rc = page_get(frame->data, rids[i].slot, &got, &got_len);
        CHECK(rc == OK);
        if (rc == OK && !row_matches(i, got, got_len))
            bad++;

        rc = pager_unpin(pg, rids[i].page, false);
        CHECK(rc == OK);
        if (bad > 3 || failures > 3)
            break;
    }
    CHECK(bad == 0);

    /* ---------------- freelist 重建 ---------------- */
    {
        u32 npages = (u32)pager_npages(pg);
        CHECK(freelist_create(npages, &fl) == OK);

        /* 扫描所有页，从 page_free_bytes 重建档位 */
        for (u32 p = 0; p < npages; p++) {
            rc = pager_get(pg, p, &frame);
            CHECK(rc == OK);
            if (rc != OK)
                break;
            freelist_set(fl, p, page_free_bytes(frame->data));
            pager_unpin(pg, p, false);
        }

        /* 重建后必须还能找到放得下小记录的页 */
        CHECK(freelist_find(fl, 100) != PAGE_ID_NIL);
        printf("    （%u 行 / %u 页，重开后全部读回一致；"
               "freelist 从页内容重建成功）\n", NROWS, npages);
        freelist_destroy(fl);
    }

    pager_close(pg);

    if (failures == 0) {
        printf("  v0.1 端到端通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
