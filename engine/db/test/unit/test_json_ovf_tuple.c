/*
 * test_json_ovf_tuple.c —— 页链值在 record 里的往返（C5.3b-b）
 *
 * ==================== 这个测试在钉什么 ====================
 *
 * 一个"页链来源"的 JSON 值（ext_kind = VAL_EXT_PAGE_CHAIN）经过
 * val_to_record 之后，record 变长区里必须是 **12 字节标记**，
 * 而不是被物化成 64KB 字节流。
 *
 * 反过来，val_from_record_ctx 看到标记时要还原成
 * "external + VAL_EXT_PAGE_CHAIN + ext_off = chain_len" 的值。
 *
 * 四条不能退让的性质：
 *
 *   1. **不物化**：物化再存回 tuple 等于把页链白建了一遍，
 *      而且 64KB 撞上 tuple 的 u16 长度必然 E_NOSPACE。
 *   2. **ext_off 是链长度**：这是"只走一趟"的支点
 *      （见 json_ovf_detoast 对 off 的说明）。
 *   3. **没有读链上下文就明确报错**：val_from_record（老入口）
 *      遇到标记返回 E_NOTSUP，而不是给出一个 ext_ctx == NULL、
 *      一读就崩的值。
 *   4. **val_json_free 不碰链**：ctx 是实例级稳定对象，
 *      free 它等于把表上下文还给堆。
 *
 * 【为什么第 4 条要单独测】
 * 它是最容易被"顺手改回来"的一条 —— 老实现对所有 external
 * 都 free(ext_ctx)，看起来更简洁，但页链接进来就是内存踩踏。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "storage/record.h"
#include "storage/value.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* 假的"页链归还"登记：只记被调了几次、参数是什么 */
static int      g_free_calls;
static void    *g_free_pager;
static u64      g_free_head;

static int fake_chain_free(void *pager, u64 head)
{
    g_free_calls++;
    g_free_pager = pager;
    g_free_head = head;
    return OK;
}

/* 假的页链 detoast：只证明"注册表里的函数被拿到了" */
static int fake_chain_detoast(void *ctx, u64 key, u64 off,
                              u8 *buf, u16 cap, u16 *len_out)
{
    (void)ctx; (void)key;
    if (!len_out)
        return E_INVAL;
    *len_out = (u16)off;
    if (buf == NULL)
        return (off > 0) ? E_NOSPACE : OK;
    if (cap < (u16)off)
        return E_NOSPACE;
    return OK;      /* 不填内容：本测试只关心句柄字段 */
}

/* 造一列 T_JSON 的 schema */
static void make_json_col(ColDesc *col)
{
    memset(col, 0, sizeof(*col));
    snprintf(col->name, sizeof(col->name), "doc");
    col->type = T_JSON;
    col->nullable = true;
}

/* ---------------- 第 1 组：页链值进记录 ⇒ 12 字节标记 ---------------- */

static void t_encode_marker(void)
{
    ColDesc col;
    DbValue v;
    u8 rec[1024];
    u16 rlen = 0;
    u32 head = 0;
    u32 clen = 0;
    int rc;

    printf("  [1] 页链值进记录写 12 字节标记\n");

    make_json_col(&col);

    /*
     * 造一个"页链来源"的值。
     *
     * ext_ctx 用 0x1000 这个**明显不是堆指针**的值：
     * 如果有人误把这当成 malloc 出来的缓冲去 free，测试会立刻崩。
     */
    val_external_json_kind(&v, 777u, 5000u, val_mem_detoast,
                           (void *)(uintptr_t)0x1000,
                           (u8)VAL_EXT_PAGE_CHAIN);
    CHECK(v.ext_kind == (u8)VAL_EXT_PAGE_CHAIN);

    rc = val_to_record(&col, 1, &v, rec, sizeof(rec), &rlen);
    CHECK(rc == OK);
    /* 记录 = header + bitmap + 偏移数组 + 12 字节标记（不是 5000 字节） */
    CHECK(rlen < 100u);

    /* 解变长区，确认它是标记 */
    Value tmp[REC_MAX_COLS];
    CHECK(rec_decode(&col, 1, rec, rlen, tmp) == OK);
    CHECK(tmp[0].u.text.n == (u16)REC_JSON_OVF_MARK_SIZE);
    CHECK(rec_json_ovf_is_marker(tmp[0].u.text.p, tmp[0].u.text.n));
    CHECK(rec_json_ovf_decode(tmp[0].u.text.p, tmp[0].u.text.n,
                              &head, &clen) == OK);
    CHECK(head == 777u);
    CHECK(clen == 5000u);
}

/* ---------------- 第 2 组：标记 ⇒ 页链值 ---------------- */

static void t_decode_marker(void)
{
    ColDesc col;
    DbValue v;
    DbValue out;
    u8 rec[1024];
    u16 rlen = 0;
    void *ctx = (void *)(uintptr_t)0x2000;   /* 假的 Pager */

    printf("  [2] 标记还原成页链值\n");

    make_json_col(&col);
    val_external_json_kind(&v, 42u, 3000u, val_mem_detoast,
                           (void *)(uintptr_t)0x1000,
                           (u8)VAL_EXT_PAGE_CHAIN);
    CHECK(val_to_record(&col, 1, &v, rec, sizeof(rec), &rlen) == OK);

    /* 带上下文：还原成页链值，ext_off == chain_len */
    memset(&out, 0, sizeof(out));
    CHECK(val_from_record_ctx(&col, 1, rec, rlen, ctx, &out) == OK);
    CHECK(out.type == T_JSON);
    CHECK(out.external);
    CHECK(out.ext_kind == (u8)VAL_EXT_PAGE_CHAIN);
    CHECK(out.ext_key == 42u);
    CHECK(out.ext_off == 3000u);     /* 这是"只走一趟"的支点 */
    CHECK(out.ext_ctx == ctx);       /* ctx 被原样放进去了 */

    /*
     * 不带上下文：明确报错。
     *
     * 【为什么不能返回一个"看起来能用"的值】
     * ext_ctx == NULL 的页链值，第一次 detoast 就 E_INVAL；
     * 而报错点会远离"调用方忘了传 ctx"这个真正的原因。
     */
    {
        DbValue bad;
        CHECK(val_from_record(&col, 1, rec, rlen, &bad) == E_NOTSUP);
    }
}

/* ---------------- 第 3 组：内联 JSONB 不受影响 ---------------- */

/*
 * 回归防线：marker 判别不能误伤小文档。
 * 合法 JSONB 的首字节是 tag 0..8，而标记 tag 首字节是 0x56 —— 不会撞。
 */
static void t_inline_unaffected(void)
{
    ColDesc col;
    DbValue v;
    u8 rec[1024];
    u16 rlen = 0;
    DbValue out;

    printf("  [3] 内联 JSONB 不受标记影响\n");

    make_json_col(&col);

    /* 20 字节 JSONB（首字节 0x08 = 合法 tag，模拟最小对象） */
    u8 bytes[20];
    bytes[0] = 0x08;
    memset(bytes + 1, 0x41, sizeof(bytes) - 1);

    val_null(&v);
    v.is_null = false;
    v.type = T_JSON;
    v.len = (u16)sizeof(bytes);
    memcpy(v.s, bytes, sizeof(bytes));

    CHECK(val_to_record(&col, 1, &v, rec, sizeof(rec), &rlen) == OK);
    CHECK(val_from_record_ctx(&col, 1, rec, rlen, NULL, &out) == OK);
    CHECK(!out.external);
    CHECK(out.type == T_JSON);
    CHECK(out.len == (u16)sizeof(bytes));
    CHECK(memcmp(out.s, bytes, sizeof(bytes)) == 0);
}

/* ---------------- 第 4 组：释放语义分化 ---------------- */

static void t_free_semantics(void)
{
    DbValue chain;
    DbValue mem;
    u8 *heap_buf;

    printf("  [4] val_json_free 不碰页链\n");

    /* 页链值：ctx 不是堆指针，free 它会让进程崩（或用 0x1000 静默越界） */
    val_external_json_kind(&chain, 9u, 100u, val_mem_detoast,
                           (void *)(uintptr_t)0x1000,
                           (u8)VAL_EXT_PAGE_CHAIN);
    val_json_free(&chain);
    /* 只清标记，不 free */
    CHECK(chain.external == false);
    CHECK(chain.ext_ctx == NULL);
    CHECK(chain.ext_kind == (u8)VAL_EXT_NONE);

    /* 内存外部值：必须真的 free（这里用 malloc 出来的缓冲验证） */
    heap_buf = (u8 *)malloc(64);
    CHECK(heap_buf != NULL);
    if (heap_buf) {
        memset(heap_buf, 0x5A, 64);
        val_external_json(&mem, (u64)(uintptr_t)heap_buf, 64,
                          val_mem_detoast, heap_buf);
        CHECK(mem.ext_kind == (u8)VAL_EXT_MEM);
        val_json_free(&mem);
        CHECK(mem.external == false);
        CHECK(mem.ext_ctx == NULL);
    }
}

/* ---------------- 第 5 组：主动归还页链 ---------------- */

static void t_release_chain(void)
{
    DbValue chain;
    DbValue mem;

    printf("  [5] val_json_release_chain 真的还链\n");

    /* 没注册时明确说"不支持" */
    g_free_calls = 0;
    CHECK(val_set_json_chain_free(fake_chain_free) == OK);
    /* 重复注册被拒（覆盖是静默换行为） */
    CHECK(val_set_json_chain_free(fake_chain_free) == E_INVAL);

    val_external_json_kind(&chain, 123u, 456u, val_mem_detoast,
                           (void *)(uintptr_t)0x3000,
                           (u8)VAL_EXT_PAGE_CHAIN);
    CHECK(val_json_release_chain(&chain) == OK);
    CHECK(g_free_calls == 1);
    CHECK(g_free_pager == (void *)(uintptr_t)0x3000);   /* ctx 就是 pager */
    CHECK(g_free_head == 123u);
    /* 归还之后标记清掉：不能重复归还（重复 = double-free） */
    CHECK(chain.external == false);
    CHECK(chain.ext_ctx == NULL);
    CHECK(val_json_release_chain(&chain) == E_INVAL);

    /* 非页链值调它是用错了，明确拒 */
    val_external_json(&mem, 1u, 2u, val_mem_detoast, NULL);
    CHECK(val_json_release_chain(&mem) == E_INVAL);
}

/* ---------------- 第 6 组：标记里的非法值传入 record ---------------- */

static void t_reject_bad_marker(void)
{
    ColDesc col;
    DbValue v;
    u8 rec[1024];
    u16 rlen = 0;

    printf("  [6] 非法页链值写记录时被拒\n");

    make_json_col(&col);

    /* chain_len 超过 65535 ⇒ 编码标记时就拒（不等到读） */
    val_external_json_kind(&v, 5u, 65536u, val_mem_detoast,
                           (void *)(uintptr_t)0x1000,
                           (u8)VAL_EXT_PAGE_CHAIN);
    CHECK(val_to_record(&col, 1, &v, rec, sizeof(rec), &rlen) == E_NOSPACE);

    /* head 是 PAGE_ID_NIL ⇒ 标记指向空处，拒 */
    val_external_json_kind(&v, 0xFFFFFFFFu, 100u, val_mem_detoast,
                           (void *)(uintptr_t)0x1000,
                           (u8)VAL_EXT_PAGE_CHAIN);
    CHECK(val_to_record(&col, 1, &v, rec, sizeof(rec), &rlen) == E_INVAL);
}

/* ---------------- 主流程 ---------------- */

int main(void)
{
    printf("test_json_ovf_tuple —— 页链值在 record 里的往返\n");

    /*
     * 注册假的页链 detoast。
     *
     * 【为什么测试要自己注册】
     * 生产路径由 db_json_init 注册（doc 模态），而本测试是值层单测 ——
     * 它不链接模态的初始化。不注册的话 val_from_record_ctx 遇到标记
     * 会返回 E_NOTSUP（那本身是对的，第 2 组末尾单独验了那条）。
     */
    CHECK(val_set_json_chain_detoast(fake_chain_detoast) == OK);

    t_encode_marker();
    t_decode_marker();
    t_inline_unaffected();
    t_free_semantics();
    t_release_chain();
    t_reject_bad_marker();

    if (failures == 0) {
        printf("  6 项验收全部通过\n");
        return 0;
    }
    printf("  %d 项断言失败\n", failures);
    return 1;
}
