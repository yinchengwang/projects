/*
 * value_json.c —— 值层的 JSONB 渲染器注册（v1.6 C5.3）
 *
 * ==================== 为什么单独一个文件 ====================
 *
 * `storage/value.c` 已经接近 800 行的硬上限（值操作 + 记录编解码 +
 * 函数表都在里面）。渲染器注册虽然只有一个函数指针，但它带着一整套
 * 契约注释与两条失败路径 —— 塞回 value.c 会直接超限。
 *
 * 而它本来就该独立：注册的是**寄生模态与值层的接口**，
 * 与"怎么比较值、怎么在记录里编解码"不是同一件事。
 *
 * ==================== 这条契约在解决什么 ====================
 *
 * `val_to_str` 要把 DbValue 转成可打印文本。T_TEXT 直接输出 s[]；
 * 但 T_JSON 的载荷是 **JSONB 二进制** —— 当 C 字符串输出会在内嵌 NUL、
 * 控制字符、非文本 tag 上截断或打印乱码，而且**看起来还像合法 JSON**。
 *
 * 值层是 L1，文档模态是 L4。`storage/` include `modalities/doc/` 是
 * L1→L4 反向依赖，`tools/check_deps.sh` 会直接红。所以值层只持一个
 * **函数指针**，"怎么渲染"由文档模态在 `db_open` 时注册进来。
 *
 * 这与通用函数表（`val_func_register`）是同一条纪律：
 * 值层提供机制，模态提供实现，两者通过一个很窄的契约相连。
 *
 * ==================== 为什么不在这里做适配 ====================
 *
 * `JsonRenderFn` 的签名与 `jsonb_render` **完全相同**，文档模态注册时
 * 直接把 `jsonb_render` 传进来即可，中间不出现第二份语义。
 * （"一个语义只能有一个主人"—— 与 v1.5 的 M6-63 同源。）
 */

#include "storage/value.h"

#include <stdlib.h>
#include <string.h>

/*
 * 【为什么是进程级静态而不是实例级】
 * 渲染器是**纯代码目录**，不持有任何状态 —— 它只是"JSONB 字节 →
 * JSON 文本"这一个函数的地址。挂在实例上会让"渲染器"与"实例"
 * 的生命周期搅在一起，且两个实例注册互相看不见。
 *
 * 【并发】
 * 注册发生在 `db_open` 里，任何查询开始之前；之后只读不写 ⇒
 * 并发读安全。
 */
static JsonRenderFn g_json_renderer = NULL;

int val_set_json_renderer(JsonRenderFn fn)
{
    if (!fn)
        return E_INVAL;
    /*
     * 已注册就不再接受第二个实现。
     *
     * 【为什么是 E_INVAL 而不是静默覆盖】
     * 覆盖是静默换行为 —— 两个模态都注册时会变成"谁后注册谁生效"，
     * 而调用方完全看不出来。明确报错让人在启动时就发现冲突。
     */
    if (g_json_renderer)
        return E_INVAL;
    g_json_renderer = fn;
    return OK;
}

bool val_json_renderer_is(JsonRenderFn fn)
{
    if (!fn)
        return g_json_renderer != NULL;
    return g_json_renderer == fn;
}

JsonRenderFn val_json_render(void)
{
    return g_json_renderer;
}

/*
 * ==================== JSON 文本编码器 ====================
 *
 * 与渲染器对称的一条路：渲染器管"JSONB → JSON 文本"，
 * 编码器管"JSON 文本 → JSONB 字节"。
 *
 * 【为什么编码器是分配式的】
 * "编码后多大算合规"是**文档模态**的知识（JSON_INLINE_MAX），
 * 值层不认识这个数字 —— 认识了就等于 L1 依赖 L4 的表示细节。
 * 所以由实现自己 malloc、自己判上限、自己报错。
 */

/* T_JSON 写入编码器：SQL 文本 JSON → JSONB 字节（由 doc 模态注册） */
static JsonEncodeFn g_json_encoder = NULL;

int val_set_json_encoder(JsonEncodeFn fn)
{
    if (!fn)
        return E_INVAL;
    /*
     * 已注册就不再接受第二个实现 —— 覆盖是静默换行为，
     * 调用方完全看不出来（同 val_set_json_renderer 的取舍）。
     */
    if (g_json_encoder)
        return E_INVAL;
    g_json_encoder = fn;
    return OK;
}

int val_json_from_text(const DbValue *text, DbValue *value)
{
    u8 *text_bytes = NULL;
    u32 text_len = 0;
    u8 *out_bytes = NULL;
    u32 out_len = 0;
    int rc;

    if (!text || !value)
        return E_INVAL;
    if (text->is_null || text->type != T_TEXT)
        return E_INVAL;
    if (!g_json_encoder)
        return E_NOTSUP;

    /*
     * 长 SQL 字面量是外部值（len 不设），必须先物化成连续字节。
     *
     * 【为什么要拆成两个缓冲】
     * 不能把 text_bytes 直接当作编码器的输出缓冲 ——
     * 输入与输出别名时，编码器写输出的那一刻输入已经被覆盖了。
     */
    if (text->external) {
        rc = val_detoast(text, &text_bytes, &text_len);
        if (rc != OK)
            return rc;
    } else {
        text_bytes = (u8 *)text->s;
        text_len = text->len;
    }

    rc = g_json_encoder((const char *)text_bytes, text_len,
                        &out_bytes, &out_len);
    if (text->external)
        free(text_bytes);
    if (rc != OK)
        return rc;

    val_null(value);
    value->is_null = false;
    if (out_len <= VAL_INLINE_MAX) {
        /* 小文档：内联在 s[] 里（不付堆代价） */
        value->type = T_JSON;
        value->len = (u16)out_len;
        if (out_len)
            memcpy(value->s, out_bytes, out_len);
        free(out_bytes);
    } else {
        /* 中型文档：外部值，缓冲归调用方（val_json_free 释放） */
        val_external_json(value, (u64)(uintptr_t)out_bytes, out_len,
                          val_mem_detoast, out_bytes);
    }
    return OK;
}

void val_json_free(DbValue *value)
{
    if (!value)
        return;
    if (value->type != T_JSON || !value->external)
        return;

    /*
     * ============ 按来源分派（这是必须的，不是优化） ============
     *
     * VAL_EXT_MEM：ext_ctx 是 malloc 出来的缓冲 ⇒ free 它。
     *
     * VAL_EXT_PAGE_CHAIN：ext_ctx 是**实例级稳定对象**（表上下文 /
     * 锁对象，不是堆分配）⇒ **绝不能 free**。free 它等于把表锁的
     * 内存还回堆，之后所有读这条链的路径都会踩已释放内存。
     *
     * 页链的**所有权在 tuple 上**：这里只清运行时标记，
     * 链本身由 VACUUM 或启动孤儿扫描回收（设计 §6.4 / §7）。
     * 写入失败要主动还链的话，用 val_json_release_chain（显式表达意图）。
     */
    if (value->ext_kind == (u8)VAL_EXT_PAGE_CHAIN) {
        value->ext_ctx = NULL;
        value->ext_fn = NULL;
        value->external = false;
        value->ext_kind = (u8)VAL_EXT_NONE;
        value->ext_key = 0;
        value->ext_off = 0;
        return;
    }

    if (value->ext_ctx) {
        free(value->ext_ctx);
        value->ext_ctx = NULL;
    }
    value->ext_fn = NULL;
    value->external = false;
    value->ext_kind = (u8)VAL_EXT_NONE;
}

/*
 * ============ 主动归还页链（写路径失败补偿专用） ============
 *
 * 【为什么不能复用 val_json_free】
 * val_json_free 的语义是"清掉运行时标记" —— 它**不碰链**。
 * 那条纪律保证"拷贝出去的值"不会因为副本被清而把共享的链释放掉。
 *
 * 而写路径失败时需要真的把链还回去（否则空间泄漏到下次启动）。
 * 两者语义相反，必须是两个函数：一个清标记、一个还链。
 *
 * 【为什么要注册回调而不是直接调 json_ovf_free】
 * 值层是 L1，json_ovf 在 L4。直接调就是 L1→L4 反向依赖，
 * tools/check_deps.sh 会红。所以由 doc 模态注册一个归还函数。
 */
static int (*g_json_chain_free)(void *pager, u64 head);

int val_set_json_chain_free(int (*fn)(void *pager, u64 head))
{
    if (!fn)
        return E_INVAL;
    if (g_json_chain_free)
        return E_INVAL;      /* 重复注册：覆盖是静默换行为 */
    g_json_chain_free = fn;
    return OK;
}

int val_json_release_chain(DbValue *value)
{
    u64 head;
    void *pager;
    int rc;

    if (!value)
        return E_INVAL;
    if (value->type != T_JSON || !value->external)
        return E_INVAL;
    if (value->ext_kind != (u8)VAL_EXT_PAGE_CHAIN)
        return E_INVAL;      /* 不是页链值，不该走这条路 */
    if (!g_json_chain_free)
        return E_NOTSUP;

    head = value->ext_key;
    pager = value->ext_ctx;   /* doc 模态注册时约定：ctx 就是 Pager */

    rc = g_json_chain_free(pager, head);

    /* 无论成功与否都清标记：链要么还了、要么留成孤儿由启动扫描回收，
     * 但**不能重复归还**（重复归还 = double-free）。 */
    value->ext_ctx = NULL;
    value->ext_fn = NULL;
    value->external = false;
    value->ext_kind = (u8)VAL_EXT_NONE;
    value->ext_key = 0;
    value->ext_off = 0;
    return rc;
}

/*
 * 渲染成新分配的 C 字符串。
 *
 * 【为什么要单独一个函数】
 * val_to_str() 有 cap 上限（REPL / 日志等"短输出"场景用它）。
 * 结果集投影必须给完整文本 —— 512 字节的栈缓冲会截断长文档，
 * 用户看到的是一个语法不完整的 JSON，比报错更难查。
 *
 * 两次调用渲染器：先问长度，再写内容（jsonb_render 的两趟协议）。
 */
int val_to_json_alloc(const DbValue *v, char **out, u32 *len)
{
    u8 *bytes = NULL;
    u32 blen = 0;
    u32 need = 0;
    u32 got = 0;
    int rc;
    JsonRenderFn render = val_json_render();

    if (!v || !out)
        return E_INVAL;
    *out = NULL;
    if (len)
        *len = 0;
    if (!render)
        return E_NOTSUP;

    if (v->external) {
        rc = val_detoast(v, &bytes, &blen);
        if (rc != OK)
            return rc;
    } else {
        bytes = (u8 *)v->s;
        blen = v->len;
    }

    /* 第一趟：问需要多少字节 */
    rc = render(bytes, blen, NULL, 0, &need);
    if (rc != E_NOSPACE && rc != OK) {
        if (v->external)
            free(bytes);
        return rc;
    }
    if (need == 0) {
        if (v->external)
            free(bytes);
        return E_CORRUPT;
    }

    char *buf = malloc((size_t)need + 1u);
    if (!buf) {
        if (v->external)
            free(bytes);
        return E_IO;
    }

    /* 第二趟：按长度写内容 */
    rc = render(bytes, blen, buf, need, &got);
    if (v->external)
        free(bytes);
    if (rc != OK || got != need) {
        free(buf);
        return rc == OK ? E_CORRUPT : rc;
    }
    buf[got] = '\0';
    *out = buf;
    if (len)
        *len = got;
    return OK;
}
