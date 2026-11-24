/*
 * test_json_ovf_marker.c —— record 的 12 字节 JSON 溢出标记
 *
 * ==================== 这个测试在钉什么 ====================
 *
 * JSONB 超过 2048 字节时，record 变长区里存的不再是 JSONB 字节，
 * 而是一个定长标记：`tag(4) + chain_head(4) + chain_len(4)`。
 *
 * 这 12 个字节必须满足三件事，缺一不可：
 *
 *   1. **能被认出来** —— 读侧靠它区分"这是标记"和"这是内联 JSONB"。
 *      不能靠"长度 > 2048 就是标记"这种隐式约定：那是跨层暗契约。
 *   2. **不会误认** —— 合法 JSONB 的首字节是 tag 0..8，
 *      而标记的 tag 是 0x4A，字节层面不可能撞上。
 *   3. **非法值当场拒** —— head 指向空处、chain_len 超过 65535
 *      都必须在这里报错，而不是等到读的时候才发现。
 *
 * 【为什么"当场拒"重要】
 * 放过去的话，错误会在读路径上以 E_CORRUPT 的形式出现 ——
 * 那时已经离"写错了什么"很远，定位成本高得多。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "storage/record.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---------------- 第 1 组：编码与解码往返 ---------------- */

static void t_roundtrip(void)
{
    u8 buf[REC_JSON_OVF_MARK_SIZE];
    u32 head = 0;
    u32 clen = 0;

    printf("  [1] 编码与解码往返\n");

    CHECK(rec_json_ovf_encode(42u, 5000u, buf, sizeof(buf)) == OK);
    CHECK(rec_json_ovf_is_marker(buf, sizeof(buf)));
    CHECK(rec_json_ovf_decode(buf, sizeof(buf), &head, &clen) == OK);
    CHECK(head == 42u);
    CHECK(clen == 5000u);

    /* 边界：空链（长度 0）与最大长度都要能往返 */
    CHECK(rec_json_ovf_encode(7u, 0u, buf, sizeof(buf)) == OK);
    CHECK(rec_json_ovf_decode(buf, sizeof(buf), &head, &clen) == OK);
    CHECK(head == 7u);
    CHECK(clen == 0u);

    CHECK(rec_json_ovf_encode(9u, 65535u, buf, sizeof(buf)) == OK);
    CHECK(rec_json_ovf_decode(buf, sizeof(buf), &head, &clen) == OK);
    CHECK(head == 9u);
    CHECK(clen == 65535u);
}

/* ---------------- 第 2 组：不是标记的必须认出来 ---------------- */

static void t_not_marker(void)
{
    u8 buf[REC_JSON_OVF_MARK_SIZE];

    printf("  [2] 不是标记的要认出来\n");

    /* 全部 0 */
    memset(buf, 0, sizeof(buf));
    CHECK(!rec_json_ovf_is_marker(buf, sizeof(buf)));

    /* 合法 JSONB 的首字节只有 0..8 —— 逐个试，都不能被当成标记 */
    for (u8 tag = 0; tag <= 8u; tag++) {
        memset(buf, 0xAA, sizeof(buf));
        buf[0] = tag;
        CHECK(!rec_json_ovf_is_marker(buf, sizeof(buf)));
    }

    /* 长度不对就不是标记（8 / 13 / 16 字节都不行） */
    memset(buf, 0, sizeof(buf));
    CHECK(rec_json_ovf_encode(1u, 1u, buf, sizeof(buf)) == OK);
    CHECK(!rec_json_ovf_is_marker(buf, 8u));
    CHECK(!rec_json_ovf_is_marker(buf, 13u));
    CHECK(!rec_json_ovf_is_marker(buf, 16u));

    /* NULL / 长度为 0 */
    CHECK(!rec_json_ovf_is_marker(NULL, REC_JSON_OVF_MARK_SIZE));
    CHECK(!rec_json_ovf_is_marker(buf, 0u));
}

/* ---------------- 第 3 组：非法值必须当场拒 ---------------- */

static void t_reject_invalid(void)
{
    u8 buf[REC_JSON_OVF_MARK_SIZE];
    u32 head = 123u;
    u32 clen = 456u;

    printf("  [3] 非法值当场拒\n");

    /* head = PAGE_ID_NIL ⇒ 标记指向空处，拒 */
    CHECK(rec_json_ovf_encode(0xFFFFFFFFu, 100u, buf, sizeof(buf)) == E_INVAL);
    /* 手写一个 head=NIL 的标记，确认 decode 也拒 */
    CHECK(rec_json_ovf_encode(1u, 100u, buf, sizeof(buf)) == OK);
    buf[4] = 0xFF; buf[5] = 0xFF; buf[6] = 0xFF; buf[7] = 0xFF;
    CHECK(rec_json_ovf_is_marker(buf, sizeof(buf)));
    CHECK(rec_json_ovf_decode(buf, sizeof(buf), &head, &clen) == E_INVAL);

    /* chain_len > 65535 ⇒ 超过 DetoastFn 能表达的长度，拒 */
    CHECK(rec_json_ovf_encode(1u, 65536u, buf, sizeof(buf)) == E_NOSPACE);

    /* 缓冲不够 ⇒ E_NOSPACE */
    CHECK(rec_json_ovf_encode(1u, 100u, buf, 8u) == E_NOSPACE);
    CHECK(rec_json_ovf_encode(1u, 100u, NULL, 12u) == E_NOSPACE);

    /* decode 在失败时不留下半成品（输出参数保持 0） */
    head = 999u;
    clen = 999u;
    memset(buf, 0, sizeof(buf));
    CHECK(rec_json_ovf_decode(buf, sizeof(buf), &head, &clen) == E_INVAL);
    CHECK(head == 0u);
    CHECK(clen == 0u);
}

/* ---------------- 第 4 组：字节布局是权威的小端 ---------------- */

/*
 * 布局必须逐字节固定，否则"写入端与读取端都是同一份代码"会掩盖
 * 一个致命的兼容性问题：换语言/换实现读同一个库时对不上。
 */
static void t_byte_layout(void)
{
    u8 buf[REC_JSON_OVF_MARK_SIZE];
    static const u8 expect[REC_JSON_OVF_MARK_SIZE] = {
        /* tag = 0x4A534F56 小端 */
        0x56, 0x4F, 0x53, 0x4A,
        /* head = 0x01020304 小端 */
        0x04, 0x03, 0x02, 0x01,
        /* chain_len = 0x00000506 小端 */
        0x06, 0x05, 0x00, 0x00,
    };

    printf("  [4] 字节布局是小端\n");

    CHECK(rec_json_ovf_encode(0x01020304u, 0x00000506u, buf,
                              sizeof(buf)) == OK);
    CHECK(memcmp(buf, expect, sizeof(expect)) == 0);

    /* 首字节是 'V'（0x56），不在合法 JSONB tag 范围 0..8 内 */
    CHECK(buf[0] == 0x56);
    CHECK(buf[0] > 8u);
}

/* ---------------- 主流程 ---------------- */

int main(void)
{
    printf("test_json_ovf_marker —— record 的 JSON 溢出标记\n");

    t_roundtrip();
    t_not_marker();
    t_reject_invalid();
    t_byte_layout();

    if (failures == 0) {
        printf("  4 项验收全部通过\n");
        return 0;
    }
    printf("  %d 项断言失败\n", failures);
    return 1;
}
