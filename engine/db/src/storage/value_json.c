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
