#include "storage/value.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ==================== DbValue 的"轻量初始化" ====================
 *
 * 【为什么要改】
 * DbValue 有 272 字节（VAL_TEXT_MAX=256 + 若干字段），
 * 而原来每个值都是**整结构 memset**：
 *
 *   每行 4 列 × 272 字节 = 1088 字节/行
 *   10 万行 = **约 108 MB 的内存清零**
 *
 * v1.1 的执行器基准实测：解码阶段占了整条查询的 40%，
 * 而这 40% 里绝大部分就是在清那 256 字节的文本缓冲 ——
 * 对 INT / REAL / BOOL 列来说，那 256 字节**一个字节都用不到**。
 *
 * 【安全性】
 * 改完之后唯一"没清"的是 s[] 里的字节。
 * 而 s[] 只在 type == T_TEXT 时才被读，
 * 每个 val_* 都显式写 type —— 所以"读 s[] 之前 type 一定是 T_TEXT" ✓。
 *
 * 文本值本来就要把内容 memcpy 进 s[]，事先清零纯属多余。
 */
static void val_reset(DbValue *v)
{
    v->is_null = false;
    /*
     * ext_kind 必须和 external 一起清 —— 否则一个"曾经是页链句柄"的值
     * 被复用成内联值时，ext_kind 还留着 VAL_EXT_PAGE_CHAIN。
     * 现在读路径只看 external，但"残留的字段"是典型的定时炸弹。
     */
    v->ext_kind = (u8)VAL_EXT_NONE;
    v->type = 0;
    v->i = 0;
    v->d = 0.0;
    v->len = 0;
    v->external = false;
    v->ext_key = 0;
    v->ext_off = 0;
    v->ext_fn = NULL;
    v->ext_ctx = NULL;
}

void val_null(DbValue *v)
{
    val_reset(v);
    v->is_null = true;
}

void val_int(DbValue *v, i64 x)
{
    val_reset(v);
    v->type = T_INT8;
    v->i = x;
}

void val_real(DbValue *v, double x)
{
    val_reset(v);
    v->type = T_REAL;
    v->d = x;
}

void val_bool(DbValue *v, bool b)
{
    val_reset(v);
    v->type = T_BOOL;
    v->i = b ? 1 : 0;
}

void val_text(DbValue *v, const char *s, u16 len)
{
    val_reset(v);
    v->type = T_TEXT;
    if (len >= VAL_TEXT_MAX)
        len = VAL_TEXT_MAX - 1;
    memcpy(v->s, s, len);
    v->s[len] = '\0';
    v->len = len;
}

/*
 * 拷贝一个值。
 *
 * 【为什么不整结构 memcpy】
 * 272 字节全拷，对 INT / REAL / BOOL 列来说有 256 字节是纯搬运。
 * 表达式求值每取一列就要拷一次 —— 这是过滤阶段的大头。
 *
 * 只拷"有意义的部分"：小字段 + （文本时）文本内容。
 * 文本内容的长度由 len 决定，拷贝后补一个 '\0'，
 * 保证 s[] 仍然能当 C 字符串用。
 */
void val_copy(DbValue *dst, const DbValue *src)
{
    dst->is_null = src->is_null;
    dst->type = src->type;
    dst->i = src->i;
    dst->d = src->d;
    dst->len = src->len;
    dst->external = src->external;
    dst->ext_kind = src->ext_kind;
    dst->ext_key = src->ext_key;
    dst->ext_off = src->ext_off;
    dst->ext_fn = src->ext_fn;
    dst->ext_ctx = src->ext_ctx;

    if (src->type == T_TEXT || (src->type == T_JSON && !src->external)) {
        u16 n = src->len;
        if (n >= VAL_TEXT_MAX)
            n = VAL_TEXT_MAX - 1;
        if (n)
            memcpy(dst->s, src->s, n);
        dst->s[n] = '\0';
    }
}

/*
 * 造一个外部值（v0.9）。
 *
 * 内容不在这里，只记「去哪儿取」。type 保持 T_TEXT —— 这是既有 blob/vector
 * 外部值的契约。T_JSON 的外部值由 val_external_json() 创建，二者共享
 * DetoastFn 机制，但 type 不混淆。
 */
void val_external_text(DbValue *v, u64 key, u64 off,
                       DetoastFn fn, void *ctx)
{
    val_reset(v);
    v->type = T_TEXT;
    v->external = true;
    v->ext_kind = (u8)VAL_EXT_MEM;
    v->ext_key = key;
    v->ext_off = off;
    v->ext_fn = fn;
    v->ext_ctx = ctx;
}

/*
 * 造一个外部 JSONB 值。
 *
 * `key/off/fn/ctx` 与普通外部值相同，但 type 固定为 T_JSON ——
 * 这样调用方可以在 detoast 后按 JSONB 解析，而不会把二进制字节
 * 当成 UTF-8 文本输出。
 */
void val_external_json_kind(DbValue *v, u64 key, u64 off,
                            DetoastFn fn, void *ctx, u8 kind)
{
    val_reset(v);
    v->type = T_JSON;
    v->external = true;
    v->ext_kind = (kind == (u8)VAL_EXT_NONE) ? (u8)VAL_EXT_MEM : kind;
    v->ext_key = key;
    v->ext_off = off;
    v->ext_fn = fn;
    v->ext_ctx = ctx;
}

void val_external_json(DbValue *v, u64 key, u64 off,
                       DetoastFn fn, void *ctx)
{
    /* 默认来源是"内存外部值" —— 现有 4 处调用点都是这一种 */
    val_external_json_kind(v, key, off, fn, ctx, (u8)VAL_EXT_MEM);
}

/*
 * 物化（v0.9）。
 *
 * 内联值：拷一份。
 * 外部值：调 ext_fn 读回来 —— 字节数事先不知道，所以先问一次长度。
 *
 * 【为什么先问长度】
 * ext_fn 的约定是"buf 不够就返回 E_NOSPACE"。
 * 而外部值的长度事先不可知（blob 的帧可能不满）。
 * 所以这里分两步：先用一个 0 长度缓冲问长度，再分配、真正读。
 *
 * 模态的实现要配合这个约定：cap 不足时不要写 buf，
 * 只填 len_out 并返回 E_NOSPACE。
 */
int val_detoast(const DbValue *v, u8 **buf_out, u32 *len_out)
{
    if (!v || !buf_out || !len_out)
        return E_INVAL;

    *buf_out = NULL;
    *len_out = 0;

    if (v->is_null)
        return E_INVAL;

    if (!v->external) {
        u32 n = v->len;
        u8 *buf = malloc(n ? n : 1);
        if (!buf)
            return E_IO;
        if (n)
            memcpy(buf, v->s, n);
        *buf_out = buf;
        *len_out = n;
        return OK;
    }

    if (!v->ext_fn)
        return E_CORRUPT;

    /* 第一步：问长度 */
    u16 need = 0;
    int rc = v->ext_fn(v->ext_ctx, v->ext_key, v->ext_off, NULL, 0, &need);
    if (rc != E_NOSPACE) {
        /* 模态要么直接读成功（长度为 0 的极端情况），要么就是真出错 */
        if (rc != OK)
            return rc;
        need = 0;
    }

    u8 *buf = malloc(need ? need : 1);
    if (!buf)
        return E_IO;

    /* 长度是 0：内容为空，没必要再读一次（读也是 E_NOSPACE） */
    if (need == 0) {
        *buf_out = buf;
        *len_out = 0;
        return OK;
    }

    u16 got = 0;
    rc = v->ext_fn(v->ext_ctx, v->ext_key, v->ext_off, buf, need, &got);
    if (rc != OK) {
        free(buf);
        return rc;
    }

    *buf_out = buf;
    *len_out = got;
    return OK;
}

int val_truth(const DbValue *v)
{
    if (v->is_null)
        return -1;

    /*
     * 外部值：真值取决于"是不是空串"，得知道长度 → 物化一次。
     *
     * 物化失败（比如帧读不出来）时返回 -1 —— 当成 NULL 处理，
     * 这一行不会匹配任何 WHERE。
     *
     * 【为什么这里不报错】
     * val_truth 的返回值只有 -1/0/1，没有错误通道。
     * 改签名会波及表达式求值的三处调用点。
     * 而"拿外部大值直接当条件"本来就是罕见用法，
     * 失败时保守地当成 NULL（不匹配）是安全的方向。
     * 真正的错误会在物化那一步（输出路径）报出来。
     */
    if (v->external) {
        u8 *buf = NULL;
        u32 len = 0;
        if (val_detoast(v, &buf, &len) != OK)
            return -1;
        free(buf);
        return len > 0 ? 1 : 0;
    }

    if (v->type == T_TEXT)
        return v->len > 0 ? 1 : 0;
    return v->i != 0 ? 1 : 0;
}

int val_cmp(const DbValue *a, const DbValue *b, bool *is_null, int *err)
{
    *is_null = false;
    if (err)
        *err = OK;

    if (a->is_null || b->is_null) {
        *is_null = true;
        return 0;
    }

    /* 都是文本：按字节比 */
    bool a_text = (a->type == T_TEXT);
    bool b_text = (b->type == T_TEXT);

    /* JSONB 是不透明二进制，不定义通用排序/相等语义。 */
    if (a->type == T_JSON || b->type == T_JSON) {
        if (err)
            *err = E_NOTSUP;
        return 0;
    }

    if (a_text && b_text) {
        /*
         * 外部值必须**物化之后**再比。
         *
         * 【为什么不许"近似比较"】
         * 近似比较 = 给一个可能错的结果。
         * 一个错的 WHERE 结果比一个明确的报错坏得多 ——
         * 报错会让人停下，错的结果会一路流下去。
         */
        if (a->external || b->external) {
            u8 *ab = NULL;
            u8 *bb = NULL;
            u32 an = 0;
            u32 bn = 0;

            int rc = val_detoast(a, &ab, &an);
            if (rc == OK)
                rc = val_detoast(b, &bb, &bn);
            if (rc != OK) {
                free(ab);
                free(bb);
                if (err)
                    *err = rc;
                return 0;
            }

            u32 n = an < bn ? an : bn;
            int c = n ? memcmp(ab, bb, n) : 0;
            free(ab);
            free(bb);

            if (c != 0)
                return c;
            if (an == bn)
                return 0;
            return an < bn ? -1 : 1;
        }

        u16 n = a->len < b->len ? a->len : b->len;
        int c = memcmp(a->s, b->s, n);
        if (c != 0)
            return c;
        if (a->len == b->len)
            return 0;
        return a->len < b->len ? -1 : 1;
    }

    /*
     * 一边文本、一边非文本：**按类型序**，不按数值。
     *
     * 【为什么必须分开】
     * 原来的代码落到"其余按数值比"分支去比 a->i 和 b->i ——
     * 而文本值的 i 字段**恒为 0**（val_text 不碰它）。
     * 结果：`WHERE int_col = '5'` 会把**值为 0 的行**判成相等！
     *
     * 那不是"文本等于 5"，那是"文本的垃圾字段等于 0" ——
     * 静默给出错答案（v1.1 步骤 3 顺带查出来的既有 bug）。
     *
     * 【类型序：对齐 SQLite】
     *   NULL < 数值 < 文本 < 二进制
     * 所以文本永远大于数值，两者永不相等 ✓
     */
    if (a_text != b_text)
        return a_text ? 1 : -1;

    /*
     * 其余按数值比。
     *
     * 【REAL 与整数混比】
     * 只要有一边是 REAL，就按 double 比 —— 这样
     *   AVG(x) 的结果（REAL）能和原列（INT）正确比较。
     * 两边都是整数时用 i64 比（避免 double 丢精度：
     * 2^53 以上的 i64 转 double 会失真）。
     */
    if (a->type == T_REAL || b->type == T_REAL) {
        double x = (a->type == T_REAL) ? a->d : (double)a->i;
        double y = (b->type == T_REAL) ? b->d : (double)b->i;
        if (x < y)
            return -1;
        if (x > y)
            return 1;
        return 0;
    }

    if (a->i < b->i)
        return -1;
    if (a->i > b->i)
        return 1;
    return 0;
}


/*
 * 渲染器从 value_json.c 取 —— 值层提供机制，模态提供实现。
 * 详见 value.h 的 JsonRenderFn 契约与 storage/value_json.c 的文件头。
 */

int val_to_str(const DbValue *v, char *buf, int cap)
{
    int n;

    if (v->is_null) {
        n = snprintf(buf, (size_t)cap, "NULL");
    } else if (v->external) {
        /*
         * 外部值：物化后按文本输出。
         *
         * 内容可能超过 cap（调用方给 512 字节缓冲）→ 截断。
         * 这是"输出格式"的限制，不是数据丢失：结果集那条路径
         * （exec.c 的 b_push）会自己分配足够大的缓冲。
         */
        u8 *tmp = NULL;
        u32 len = 0;
        if (val_detoast(v, &tmp, &len) != OK) {
            n = snprintf(buf, (size_t)cap, "<外部值读取失败>");
            if (n < 0)
                n = 0;
            if (n >= cap)
                n = cap - 1;
            return n;
        }
        n = snprintf(buf, (size_t)cap, "%.*s", (int)len, tmp);
        free(tmp);
    } else if (v->type == T_JSON) {
        /*
         * JSONB 是二进制值，不是可打印文本。
         *
         * 两条路：
         *   1. 文档模态注册过渲染器（db_json_init 里注册）⇒ 走它，出合法 JSON；
         *   2. 没注册 ⇒ **绝不**把原始字节当 C 字符串输出
         *      （内嵌 NUL 会截断、控制字符会乱码，结果看起来还像 JSON）。
         *      给一个明确的诊断标记，让人去调 db_json_decode。
         *
         * ⚠️ 内联 JSONB 与外部 JSONB 都要能渲染。前者字节就在 s[]，
         *    后者要先物化成连续缓冲（物化出的 buf 由这里 free）。
         */
        int rc = E_INVAL;
        u32 need = 0;
        u8 *tmp = NULL;
        u32 blen = 0;
        const u8 *bytes = NULL;
        JsonRenderFn render = val_json_render();

        if (!render) {
            n = snprintf(buf, (size_t)cap, "<JSONB: 请使用 db_json_decode 解码>");
            goto json_done;
        }

        if (v->external) {
            rc = val_detoast(v, &tmp, &blen);
            if (rc != OK) {
                n = snprintf(buf, (size_t)cap, "<JSONB 读取失败>");
                goto json_done;
            }
            bytes = tmp;
        } else {
            bytes = (const u8 *)v->s;
            blen = v->len;
        }

        rc = render(bytes, blen, NULL, 0, &need);
        if (rc != E_NOSPACE || need == 0 || (int)need >= cap) {
            n = snprintf(buf, (size_t)cap, "<JSONB 渲染失败>");
            goto json_done;
        }

        {
            u32 got = 0;
            rc = render(bytes, blen, buf, (u32)cap - 1u, &got);
            if (rc != OK) {
                n = snprintf(buf, (size_t)cap, "<JSONB 渲染失败>");
                goto json_done;
            }
            n = (int)got;
        }

    json_done:
        free(tmp);
        /*
         * 渲染器只负责写 cap 字节，**不写 NUL 终止符**（它的契约与
         * jsonb_render / DetoastFn 同一口径：按长度交付，不做 C 字符串）。
         * 而 val_to_str 的调用方（结果集、REPL）要的是 C 字符串 ⇒
         * 终止符必须在这里补 —— 少写这一行，输出就会带上后面残留的垃圾。
         */
        if (n >= 0 && n < cap)
            buf[n] = '\0';
    } else if (v->type == T_TEXT) {
        n = snprintf(buf, (size_t)cap, "%.*s", (int)v->len, v->s);
    } else if (v->type == T_REAL) {
        /*
         * REAL 的输出格式必须和 SQLite 一致 —— A3 要对照 SQLite 全等。
         *
         * SQLite 的浮点转文本用 "%.15g"，但**结果里没有小数点时会补 ".0"**：
         *   3.0 → "3.0"（不是 "3"）
         *   2.5 → "2.5"
         *
         * 这个差别不是吹毛求疵 —— 它是"结果全等"能不能过的关键。
         */
        n = snprintf(buf, (size_t)cap, "%.15g", v->d);
        if (n > 0 && n < cap) {
            bool has_dot = false;
            for (int i = 0; i < n; i++) {
                if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E' ||
                    buf[i] == 'n' || buf[i] == 'i') {  /* nan / inf */
                    has_dot = true;
                    break;
                }
            }
            if (!has_dot && n + 2 < cap) {
                buf[n] = '.';
                buf[n + 1] = '0';
                buf[n + 2] = '\0';
                n += 2;
            }
        }
    } else if (v->type == T_BOOL) {
        n = snprintf(buf, (size_t)cap, "%s", v->i ? "true" : "false");
    } else {
        n = snprintf(buf, (size_t)cap, "%lld", (long long)v->i);
    }
    if (n < 0)
        n = 0;
    if (n >= cap)
        n = cap - 1;
    return n;
}


/* Value[]（指向记录内部）→ DbValue[]（自己持有内容） */
/*
 * 值层的通用「内存外部值」读者。
 *
 * 【为什么它在值层而不是 doc 模态】
 * 外部值是**通用机制**（blob 按帧读、vector 整体读、大 JSON 按页链读）。
 * "内容就在这块内存里"是最基础的一种 —— 每个层都可能用到，而且它
 * 完全不需要知道载荷是文本还是 JSONB 字节。
 * 放到模态里会让 storage 反过来依赖 modalities（L1→L4，分层门禁直接红）。
 *
 * 【参数口径】
 *   ctx → 内存首字节；key = 首字节地址（重复给一次，便于只看 key 的调用方）；
 *   off = 这段内容有多少字节。
 */
int val_mem_detoast(void *ctx, u64 key, u64 off,
                    u8 *buf, u16 cap, u16 *len_out)
{
    (void)key;
    const u8 *src = (const u8 *)ctx;
    u16 len = (u16)off;

    if (!len_out)
        return E_INVAL;
    *len_out = len;
    if (!src && len)
        return E_INVAL;
    if (!buf || cap < len)
        return E_NOSPACE;
    if (len)
        memcpy(buf, src, len);
    return OK;
}


/*
 * ==================== 通用值层函数表（v1.6） ====================
 *
 * 实现极简：一个静态数组 + 线性查找。
 *
 * 【为什么不做哈希表】
 * 函数数量是个位数（目前 2 个）。哈希表在这里是纯负担 ——
 * 多 100 行代码、多一处可能出错的地方，换来的性能差异在
 * 「每条查询查几次」这个量级上根本测不出来。
 *
 * 【为什么是进程级静态数组而不是实例级】
 * 函数表是**纯代码目录**，不持有任何状态。
 * 挂在实例上会让「函数表」与「实例」的生命周期搅在一起 ——
 * 两个实例注册同一个函数还会互相看不见。
 *
 * 【为什么名字用 const char* 不拷贝】
 * 注册方传的都是字符串字面量（静态存储期），拷贝反而是多一次分配
 * 与"谁负责 free"的麻烦。这条隐含约定写在契约注释里。
 */
#define VAL_FUNC_MAX 16

static struct {
    const char *name;
    ValFunc     fn;
} g_val_funcs[VAL_FUNC_MAX];
static int g_val_func_count = 0;

int val_func_register(const char *name, ValFunc fn)
{
    if (!name || !*name || !fn)
        return E_INVAL;

    /* 同名不许重复注册（不覆盖已有实现 —— 覆盖是静默换行为） */
    for (int i = 0; i < g_val_func_count; i++) {
        if (strcmp(g_val_funcs[i].name, name) == 0)
            return E_INVAL;
    }
    if (g_val_func_count >= VAL_FUNC_MAX)
        return E_NOSPACE;

    g_val_funcs[g_val_func_count].name = name;
    g_val_funcs[g_val_func_count].fn = fn;
    g_val_func_count++;
    return OK;
}

ValFunc val_func_find(const char *name)
{
    if (!name || !*name)
        return NULL;
    for (int i = 0; i < g_val_func_count; i++) {
        if (strcmp(g_val_funcs[i].name, name) == 0)
            return g_val_funcs[i].fn;
    }
    return NULL;
}

/*
 * ==================== DbValue 布局断言 ====================
 *
 * `ext_kind` 是**塞进原有填充字节**的：external 在偏移 282，
 * ext_key 在 288，中间 283..287 有 5 字节空洞。
 *
 * 这条断言存在的意义：全仓有多处按值假设的地方
 * （memcpy(row, DbValue×n)、与固定偏移相关的调试代码）。
 * 一旦有人给 DbValue 加字段把布局撑大了，这里会编译失败 ——
 * 比"某个性能基准悄悄变慢"或"某处越界读"好得多。
 */
_Static_assert(sizeof(DbValue) == 320u,
               "DbValue 布局变了：检查 ext_kind 是否挤占了别的字段");
_Static_assert(offsetof(DbValue, external) == 282u,
               "DbValue.external 偏移变了");
_Static_assert(offsetof(DbValue, ext_key) == 288u,
               "DbValue.ext_key 偏移变了");
