/*
 * test_json_ovf_read.c —— 用已知长度一趟读完（C5.3b-b）
 *
 * ==================== 这个测试在钉什么 ====================
 *
 * 老的读路径是**两趟 walk**：第一趟沿链走出总长度，第二趟再沿链拷内容。
 * 两趟之间链没有任何保护 —— 若 VACUUM 此刻回收了这条链，
 * 第二趟读到的就是已清零的页（E_CORRUPT，甚至脏数据）。
 *
 * 把链的总长度持久化进 tuple 之后（record 的溢出标记，见设计 §4.2），
 * 长度不必"走过去量" ⇒ 只走一趟，那条竞态窗口随之消失。
 *
 * 本测试验的就是 json_ovf_read_len / json_ovf_read_once 这条新路：
 *   1. 已知长度正确 ⇒ 一趟读完，内容逐字节相同
 *   2. buf == NULL ⇒ 只报长度（E_NOSPACE + 长度）
 *   3. 已知长度比实际短 / 长 ⇒ E_CORRUPT（绝不返回半截内容）
 *   4. 超过 u16 上限的"已知长度" ⇒ E_NOSPACE，不去碰链
 *   5. chain_len == 0 ⇒ 退化成老的两趟 walk（兼容未接线的调用点）
 *   6. detoast 适配器把 off 当已知长度用
 *
 * 【为什么"长度不符必须报错"是核心】
 * 长度是期望值，链可能被改坏。若放过去，用户拿到的是一个
 * **长度对不上内容的文档** —— 比报错难查得多。
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

#define PATH "test/tmp/g15_json_ovf_read.db"

static int failures = 0;
static Pager *pg = NULL;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

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

/* ---------------- 第 1 组：已知长度一趟读完 ---------------- */

static void t_read_once(void)
{
    u32 n = JSON_OVF_PAY_CAP * 2u + 17u;
    u8 *src = (u8 *)malloc(n);
    u8 *dst = (u8 *)malloc(n);
    u64 key = PAGE_ID_NIL;
    u32 got_len = 0;
    u16 len = 0;
    int rc;

    printf("  [1] 已知长度一趟读完\n");

    if (src == NULL || dst == NULL) {
        CHECK(0);
        free(src); free(dst);
        return;
    }
    fill(src, n, 53u);

    CHECK(json_ovf_write(pg, NULL, src, n, &key) == OK);

    /* 只问长度（用已知长度交叉校验） */
    CHECK(json_ovf_read_len(pg, key, n, &got_len) == OK);
    CHECK(got_len == n);

    /* 已知长度正确 ⇒ 一趟读完，内容相同 */
    len = 0;
    memset(dst, 0xCC, n);
    rc = json_ovf_read_once(pg, key, n, dst, (u16)n, &len);
    CHECK(rc == OK);
    CHECK(len == (u16)n);
    CHECK(memcmp(src, dst, n) == 0);

    /* 只报长度（buf == NULL）⇒ E_NOSPACE + 长度 */
    len = 0;
    rc = json_ovf_read_once(pg, key, n, NULL, 0, &len);
    CHECK(rc == E_NOSPACE);
    CHECK(len == (u16)n);

    CHECK(json_ovf_free(pg, key) == OK);
    free(src); free(dst);
}

/* ---------------- 第 2 组：长度不符必须报错 ---------------- */

static void t_len_mismatch(void)
{
    u32 n = JSON_OVF_PAY_CAP + 100u;
    u8 *src = (u8 *)malloc(n);
    u8 *dst = (u8 *)malloc(n);
    u64 key = PAGE_ID_NIL;
    u32 got_len = 0;
    u16 len = 0;

    printf("  [2] 长度不符必须报错\n");

    if (src == NULL || dst == NULL) {
        CHECK(0);
        free(src); free(dst);
        return;
    }
    fill(src, n, 61u);
    CHECK(json_ovf_write(pg, NULL, src, n, &key) == OK);

    /* 比实际短 1 字节 ⇒ 报错，不返回半截 */
    len = 0;
    CHECK(json_ovf_read_once(pg, key, n - 1u, dst, (u16)n, &len) == E_CORRUPT);

    /* 比实际长 1 字节 ⇒ 同样报错 */
    CHECK(json_ovf_read_once(pg, key, n + 1u, dst, (u16)n, &len) == E_CORRUPT);

    /*
     * read_len **不**做这个校验：它的职责只是"报一个分配缓冲的大小"。
     * 若它也为校验走一遍链，"两次 walk"就又被请回来了 ——
     * 那正是这个 API 存在的意义所在。校验在 read_once 里做（上一行）。
     */
    CHECK(json_ovf_read_len(pg, key, n + 1u, &got_len) == OK);
    CHECK(got_len == n + 1u);

    /* 超过 u16 上限的"已知长度"⇒ E_NOSPACE，不去碰链 */
    CHECK(json_ovf_read_once(pg, key, JSON_OVF_MAX_LEN + 1u, dst,
                             (u16)n, &len) == E_NOSPACE);
    CHECK(json_ovf_read_len(pg, key, JSON_OVF_MAX_LEN + 1u,
                            &got_len) == E_NOSPACE);

    CHECK(json_ovf_free(pg, key) == OK);
    free(src); free(dst);
}

/* ---------------- 第 3 组：chain_len == 0 退化成两趟 walk ---------------- */

static void t_zero_len_fallback(void)
{
    u32 n = 3000u;
    u8 *src = (u8 *)malloc(n);
    u8 *dst = (u8 *)malloc(n);
    u64 key = PAGE_ID_NIL;
    u16 len = 0;
    int rc;

    printf("  [3] chain_len == 0 退化成两趟 walk\n");

    if (src == NULL || dst == NULL) {
        CHECK(0);
        free(src); free(dst);
        return;
    }
    fill(src, n, 71u);
    CHECK(json_ovf_write(pg, NULL, src, n, &key) == OK);

    /* 没给长度 ⇒ 走老路径，仍然正确 */
    len = 0;
    rc = json_ovf_read_once(pg, key, 0, dst, (u16)n, &len);
    CHECK(rc == OK);
    CHECK(len == (u16)n);
    CHECK(memcmp(src, dst, n) == 0);

    CHECK(json_ovf_free(pg, key) == OK);
    free(src); free(dst);
}

/* ---------------- 第 4 组：detoast 适配器把 off 当已知长度 ---------------- */

static void t_detoast_uses_off(void)
{
    u32 n = JSON_OVF_PAY_CAP + 33u;
    u8 *src = (u8 *)malloc(n);
    u8 *dst = (u8 *)malloc(n);
    u64 key = PAGE_ID_NIL;
    u16 len = 0;
    int rc;

    printf("  [4] detoast 把 off 当已知长度\n");

    if (src == NULL || dst == NULL) {
        CHECK(0);
        free(src); free(dst);
        return;
    }
    fill(src, n, 83u);
    CHECK(json_ovf_write(pg, NULL, src, n, &key) == OK);

    /* 第一次调用（buf == NULL）只问长度 */
    len = 0;
    rc = json_ovf_detoast(pg, key, (u64)n, NULL, 0, &len);
    CHECK(rc == E_NOSPACE || rc == OK);
    CHECK(len == (u16)n);

    /* 第二次调用取内容 */
    len = 0;
    memset(dst, 0xCC, n);
    rc = json_ovf_detoast(pg, key, (u64)n, dst, (u16)n, &len);
    CHECK(rc == OK);
    CHECK(len == (u16)n);
    CHECK(memcmp(src, dst, n) == 0);

    /* off 与实际长度不符 ⇒ 报错（不是静默接受） */
    CHECK(json_ovf_detoast(pg, key, (u64)(n - 1u), dst, (u16)n,
                           &len) == E_CORRUPT);

    /* off 超过 u16 上限 ⇒ E_NOSPACE（u16 是 DetoastFn 的出口类型） */
    CHECK(json_ovf_detoast(pg, key, (u64)JSON_OVF_MAX_LEN + 1u, dst,
                           (u16)n, &len) == E_NOSPACE);

    CHECK(json_ovf_free(pg, key) == OK);
    free(src); free(dst);
}

/* ---------------- 第 5 组：空文档 ---------------- */

/*
 * 空文档的合法长度就是 0 —— 这正是"不能用 off==0 当非法句柄"的原因。
 * 本组确认：空文档走 off=0 这条路能正确读到 0 字节，而不是被当成"没给长度"出错。
 */
static void t_empty_doc(void)
{
    u64 key = PAGE_ID_NIL;
    u32 npages = 0;
    u32 got_len = 12345u;
    u8 buf[8];
    u16 len = 99;
    int rc;

    printf("  [5] 空文档\n");

    CHECK(json_ovf_write(pg, NULL, NULL, 0, &key) == OK);
    CHECK(key != (u64)PAGE_ID_NIL);

    /* 空文档也占一个页（payload_len = 0），不靠 ext_key==0 当哨兵 */
    CHECK(json_ovf_page_count(pg, key, &npages) == OK);
    CHECK(npages == expect_pages(0));

    /* 已知长度为 0 ⇒ 读回 0 字节，不报错 */
    CHECK(json_ovf_read_len(pg, key, 0, &got_len) == OK);
    CHECK(got_len == 0);

    len = 99;
    rc = json_ovf_read_once(pg, key, 0, buf, (u16)sizeof(buf), &len);
    CHECK(rc == OK);
    CHECK(len == 0);

    /* detoast 同样：off=0 是合法长度，读回 0 字节 */
    len = 99;
    rc = json_ovf_detoast(pg, key, 0, buf, (u16)sizeof(buf), &len);
    CHECK(rc == OK);
    CHECK(len == 0);

    CHECK(json_ovf_free(pg, key) == OK);
}

/* ---------------- 主流程 ---------------- */

int main(void)
{
    int rc;

    printf("test_json_ovf_read —— 已知长度的一趟读\n");

    remove(PATH);
    rc = pager_open(PATH, 64, &pg);
    if (rc != OK || pg == NULL) {
        printf("  FAIL: pager_open rc=%d\n", rc);
        return 1;
    }

    t_read_once();
    t_len_mismatch();
    t_zero_len_fallback();
    t_detoast_uses_off();
    t_empty_doc();

    (void)pager_close(pg);
    pg = NULL;
    remove(PATH);

    if (failures == 0) {
        printf("  5 项验收全部通过\n");
        return 0;
    }
    printf("  %d 项断言失败\n", failures);
    return 1;
}
