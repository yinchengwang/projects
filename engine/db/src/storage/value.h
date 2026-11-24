#ifndef DB_STORAGE_VALUE_H
#define DB_STORAGE_VALUE_H

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "storage/record.h"

/*
 * 运行时值。自包含、不用 malloc、memcpy 即拷贝。
 *
 * 为什么不做指针 + 动态分配：值要在表达式求值、结果集、临时数组之间
 * 到处传，用指针就得管生命周期，一处漏 free 就是泄漏或野指针。
 * 代价是每值 272 字节、TEXT 最长 255 —— 够用。
 *
 * 【为什么这个文件在 storage/ 而不是 query/（v0.9 挪下来的）】
 *
 * 值是**存储层和查询层共用的语言**：record 编解码要用它（存进去），
 * 表达式求值和结果集也要用它（取出来）。而 SPI（模态和执行器之间的
 * 契约）也要用它 —— 模态把行解成值，执行器拿值做投影和过滤。
 *
 * 放在 query/ 会让 spi/ 反过来依赖 query/，层次就倒了。
 * 放在 storage/（L1）则 spi/ → storage/、query/ → storage/ 都成立。
 *
 * 【外部值：v0.9 新增】
 *
 * 内联值最多 255 字节文本。但 blob 模态的一帧是 4076 字节 —— 塞不进来。
 *
 * 三条路：
 *   1. 截断      → SQL 返回的值和实际内容不符 = 静默的数据错误。否决
 *   2. 加大值类型 → 值变大不会让扫描变便宜（扫描时仍要读每帧）。否决
 *   3. 外部值    → 值里只放"句柄 + 取回方法"，内容被需要时才读。采纳
 *
 * 第 3 条是业界通例：
 *   - PG 的 TOAST：大值存进 TOAST 表，行里留 18 字节指针，读时 detoast
 *   - ClickHouse：大字符串单独存成外部表，值是一个引用
 *   - Parquet / Lance：列块单独存，行里是 offset + length
 *
 * 外部值的价值不只是"装得下"，更是**惰性**：
 * `SELECT blob_id, frame_no FROM blob_frames` 不该读帧页，
 * `SELECT data FROM blob_frames` 才读。
 * 这靠的就是"外部值在过滤/投影的中间过程不物化"。
 *
 * 【T_JSON：寄生文档模态的表示】
 *
 * `T_JSON` 的载荷是 **JSONB 字节**（不是文本）。根据 `len`：
 *   - `len <= VAL_INLINE_MAX`（255）：DbValue.s[] 内联 JSONB 字节；
 *   - `len > VAL_INLINE_MAX`：external 句柄指向本次扫描仍有效的记录缓冲，
 *     ext_fn 仅在需要物化时从这块内存拷贝；
 *   - 超过 `JSON_INLINE_MAX`（2048）的文档由 doc 模态存到 `PT_JSON_OVF`
 *     页链，record 里只存页链 key，再由 `json_ovf_detoast` 读取。
 *
 * ⚠️ `value.h` 不 include `modalities/doc/json_ovf.h` —— 这是 L1→L4 反向依赖。
 * 页链写入 / 读取由 doc 模态的 record 适配层负责；值层只知道通用 DetoastFn。
 */

/*
 * 运行时内联文本 / 小 JSONB 的字节上限。
 *
 * DbValue 本身有 256 字节的 s[]，但要留一个 NUL 终止符，
 * 因而实际最多内联 255 字节。JSONB 是二进制，不需要 NUL；
 * 仍沿用同一上限，避免 DbValue 布局变化。
 */
#define VAL_TEXT_MAX 256
#define VAL_INLINE_MAX (VAL_TEXT_MAX - 1)

/*
 * ================= v1.1：REAL 类型 =================
 *
 * 加 REAL 不是为了 v1.1 自己 —— 是因为**后面三版都要它**：
 *   - v1.1 的 AVG（浮点平均值）
 *   - v1.4 的地理坐标（经纬度）
 *   - v1.5 的时序 value 列
 *   - v1.6 的 JSON number
 *
 * 越晚加，改动面越大（每加一版都要动一遍值层）。
 * 越早加，越便宜。
 *
 * 【为什么 DbValue 里要单独一个 d 字段，而不是复用 i】
 * i 是 i64（整数精确表示到 2^63），d 是 double。
 * 浮点数不能塞进 i64 —— 那会丢精度。
 * 两个都要，所以各占一个字段。
 */

/*
 * 取回外部值的内容。
 *
 * ctx     模态上下文（blob 传 BlobStore*）
 * key     模态内部的对象标识（blob 传 blob_id）
 * off     模态内部的位置（blob 传 frame_no）
 * buf     调用方给的缓冲
 * len_out 这一值实际有多少字节
 */
typedef int (*DetoastFn)(void *ctx, u64 key, u64 off,
                         u8 *buf, u16 cap, u16 *len_out);

/*
 * 外部值的来源种类。
 *
 * 【为什么用枚举而不是 bool 标志位】
 * 以后还会有别的来源（比如按帧读的 blob、按整体读的 vector 都可能复用
 * T_JSON 之外的路径）。一个 u8 计数能放到 255 种，而 bool 到第二种就要重构。
 */
typedef enum {
    VAL_EXT_NONE       = 0,   /* 不是外部值 */
    VAL_EXT_MEM        = 1,   /* 内容在堆缓冲里：谁分配谁释放 */
    VAL_EXT_PAGE_CHAIN = 2    /* 内容在页链里：ctx 是稳定对象，链归 tuple */
} ValExtKind;

typedef struct {
    bool is_null;
    u8   type;                  /* ColType */
    i64  i;                     /* INT4 / INT8 / BOOL 共用 */
    double d;                   /* REAL（v1.1 加；AVG / 坐标 / 时序数值 / JSON 数字） */
    u16  len;
    char s[VAL_TEXT_MAX];

    /*
     * 外部值。
     *
     * external = true 时，内容**不在** s[] 里，
     * 而是由 ext_fn 按 (ext_key, ext_off) 去别处取。
     *
     * 外部值**不持有堆内存** —— 它只持句柄和函数指针。
     * 物化出来的缓冲由调用方负责释放（见 val_detoast）。
     * 这样 DbValue 依然可以按值随意拷贝，不会出现"谁负责 free"的问题。
     */
    bool      external;

    /*
     * 外部值的**来源种类**（仅 external == true 时有效）。
     *
     * 【为什么必须有它】
     * 外部 JSON 有两种来源，释放语义完全不同：
     *
     *   VAL_EXT_MEM        内容在堆缓冲里（256..2048B 的临时外部值）
     *                      ⇒ 谁分配谁释放，val_json_free 可以 free(ext_ctx)
     *
     *   VAL_EXT_PAGE_CHAIN 内容在 PT_JSON_OVF 页链里（>2048B）
     *                      ⇒ ext_ctx 是**实例级稳定对象**（不是堆分配），
     *                        绝不能 free；链的所有权在 tuple 上
     *
     * 用 ext_fn 的指针身份判断来源是不行的：函数指针相等比较虽然合法，
     * 但那把"释放策略"藏进了实现细节，且换一个 detoast 包装就失效。
     *
     * 【为什么它是 u8 而不是枚举类型字段】
     * 布局：实测 sizeof(DbValue) == 320，external 在偏移 282、
     * ext_key 在 288 ⇒ 283..287 是 5 字节填充。
     * 把 u8 放进这个空洞里，sizeof 与所有字段偏移**一个字节都不变**
     * —— 全仓有 memcpy(row, DbValue*n) 这类按值假设的地方，布局不能动。
     * 文件末尾有 _Static_assert 钉住。
     */
    u8        ext_kind;      /* ValExtKind */
    u64       ext_key;
    u64       ext_off;
    DetoastFn ext_fn;
    void     *ext_ctx;
} DbValue;

void val_null(DbValue *v);
void val_int(DbValue *v, i64 x);
void val_real(DbValue *v, double x);
void val_bool(DbValue *v, bool b);
void val_text(DbValue *v, const char *s, u16 len);
void val_copy(DbValue *dst, const DbValue *src);

/*
 * 造一个外部值。
 *
 * type 只能是 T_TEXT —— 外部大值目前只有文本形态。
 */
void val_external_text(DbValue *v, u64 key, u64 off,
                       DetoastFn fn, void *ctx);

/*
 * 造一个外部 JSONB 值（type = T_JSON，与文本外部值保持类型区别）。
 *
 * 【谁调用】
 *   - 256..2048B 的 JSON：heap record 里是内联字节，ext_key/ext_off 指向
 *     本次查询仍有效的 record buffer（使用限制见后文 C5.3b 接线记录）；
 *   - >2048B 的 JSON：doc 模态用 PT_JSON_OVF 页链，ext_fn = json_ovf_detoast。
 *
 * value 层只收通用句柄和 DetoastFn，不依赖 json_ovf 或任何模态头。
 *
 * ⚠️ 本函数产出的值 ext_kind == VAL_EXT_MEM（默认）。
 *    页链来源请用 val_external_json_kind()。
 */
void val_external_json(DbValue *v, u64 key, u64 off,
                       DetoastFn fn, void *ctx);

/*
 * 同上，但显式指定来源种类。
 *
 * 【为什么需要单独的入口而不是给原函数加参数】
 * 现有 4 处调用点都是"内存外部值"，加参数会让它们全部要改 ——
 * 而它们改错的后果是 val_json_free 走错释放路径（free 一个非堆指针）。
 * 保留原函数 = 默认安全，新来源显式声明。
 */
void val_external_json_kind(DbValue *v, u64 key, u64 off,
                            DetoastFn fn, void *ctx, u8 kind);

/*
 * 通用「内存外部值」读者：内容就在 ctx 指向的那块内存里。
 *
 * ctx = 内存首字节；off = 这段内容有多少字节。
 * 契约与其它 DetoastFn 一致：先问长度（返回 E_NOSPACE 并填 len_out），
 * 再按 cap 读内容。
 *
 * 【为什么它属于值层】
 * "内容在内存里"不携带任何模态知识；而 blob / vector / 大 JSON 各自
 * 的外部值机制都建立在同一个 DetoastFn 契约上。
 */
int  val_mem_detoast(void *ctx, u64 key, u64 off,
                     u8 *buf, u16 cap, u16 *len_out);

/*
 * ==================== JSONB 渲染器注册（v1.6） ====================
 *
 * 【问题】
 * `val_to_str` 要把 DbValue 转成可打印文本。T_TEXT 直接输出 s[]；
 * 但 T_JSON 的载荷是 **JSONB 二进制**，把它当 C 字符串输出会在
 * 内嵌 NUL / 控制字符 / 非文本 tag 上截断或打印乱码。
 *
 * 【为什么用注册而不是让值层直接依赖文档模态】
 * 值层是 L1，文档模态是 L4。`storage/` include `modalities/doc/` 是
 * L1→L4 反向依赖 —— 分层门禁（tools/check_deps.sh）会直接红。
 * 所以值层只持一个**函数指针**，"怎么渲染 JSONB"由模态在初始化时注册
 * 进来。这与通用函数表（val_func_register）同一条纪律：值层提供机制，
 * 模态提供实现，两者通过一个很窄的契约相连。
 *
 * 【签名与 jsonb_render 完全相同】
 * 刻意不做适配层：文档模态注册时直接把 jsonb_render 传进来，
 * 中间不出现第二份语义（"一个语义只能有一个主人"）。
 *
 * 【调用口径】
 *   out == NULL  ⇒ 只报长度（写 *out_len），返回 E_NOSPACE
 *   out != NULL  ⇒ 按 cap 写；装不下 ⇒ E_NOSPACE 且**一个字节都不写**
 *
 * 【生命周期】
 * 注册发生在 `db_open` 里（db_json_init 调），之后只读不写 ⇒
 * 并发读安全。未注册时 val_to_str 输出明确标记，绝不吐原始字节。
 */
typedef int (*JsonRenderFn)(const u8 *buf, u32 len, char *out, u32 cap,
                            u32 *out_len);

/*
 * 注册 JSONB 渲染器。同名重复注册（即已有实现）返回 E_INVAL ——
 * 覆盖是静默换行为，与 val_func_register 同一取舍。
 */
int  val_set_json_renderer(JsonRenderFn fn);

/*
 * 查询当前是否已注册 / 已注册的是不是 fn。
 *
 * fn == NULL 时回答"目前有没有渲染器"；给具体 fn 时回答"是不是它"。
 * （测试与诊断用；生产路径不需要它。）
 */
bool val_json_renderer_is(JsonRenderFn fn);

/*
 * 取出当前渲染器（未注册返回 NULL）。
 *
 * 实现见 `storage/value_json.c` —— 它单独一个文件，因为 `value.c`
 * 已经接近 800 行上限（值操作 + 记录编解码 + 函数表都在里面）。
 *
 * ⚠️ 这个访问器是给 `val_to_str` 用的；调用方拿到之后**不要缓存**，
 *    每次用之前现取（注册只发生一次，代价可以忽略）。
 */
JsonRenderFn val_json_render(void);

/*
 * ==================== JSON 文本编码器注册 ====================
 *
 * 用途：SQL 层把 `T_TEXT` 的 JSON 文本转成 `T_JSON` 的 JSONB 字节。
 *
 * 【为什么是分配式】
 * "编码后多大算合规"是**文档模态**的知识（JSON_INLINE_MAX = 2048，
 * 见 modalities/doc/json_ovf.h）。若让调用方给固定缓冲，
 * 2048 这个数字就被写死在 query 层 —— 违反 A7 审计（查询层不得认识
 * 具体模态的表示细节）。
 *
 * 契约：
 *   - 实现自己分配 *bytes_out（调用方用 val_json_free 释放）
 *   - *len_out = 编码后的字节数
 *   - 超过模态上限 ⇒ 返回 E_NOSPACE，并且**不留下**已分配的缓冲
 *   - 文本非法 ⇒ 返回具体错误码，同样不留缓冲
 *
 * 【和渲染器同一条纪律】
 * 值层提供机制、模态提供实现，两者通过一个很窄的契约相连。
 * 渲染器管"JSONB → 文本"，编码器管"文本 → JSONB"，两条路对称。
 */
typedef int (*JsonEncodeFn)(const char *text, u32 text_len,
                            u8 **bytes_out, u32 *len_out);

/* 注册编码器（重复注册返回 E_INVAL —— 覆盖是静默换行为） */
int  val_set_json_encoder(JsonEncodeFn fn);

/*
 * 把 T_TEXT 值（内联或外部）编码成 T_JSON 值。
 *
 * 入参 text 必须是 T_TEXT 且非 NULL。外部值先物化（长 SQL 字面量就是外部值）。
 *
 * 出参 value：
 *   - 长度 <= VAL_INLINE_MAX ⇒ 内联在 value->s[]
 *   - 更长 ⇒ external = true，ext_ctx 指向编码器分配的缓冲
 *     （**调用方必须用 val_json_free 释放**）
 *
 * 未注册编码器 ⇒ E_NOTSUP；文本非法或超过模态上限 ⇒ 编码器的错误码原样透出。
 */
int  val_json_from_text(const DbValue *text, DbValue *value);

/*
 * ============ 释放 JSON 外部值 ============
 *
 * 两种来源的处理完全不同（见 DbValue.ext_kind 的说明）：
 *
 *   VAL_EXT_MEM        free(ext_ctx) —— 缓冲是这里分配的
 *   VAL_EXT_PAGE_CHAIN **不碰链** —— ctx 是实例级稳定对象，链归 tuple
 *
 * 两种情况都会清掉 external 标记，所以可以安全地重复调用。
 *
 * ⚠️ 别指望它归还页链：那是 val_json_release_chain 的职责。
 */
void val_json_free(DbValue *value);

/*
 * ============ JSON 内联上限（模态裁决，值层只保管） ============
 *
 * "多长算小文档、可以直接内联在 tuple 里"是**文档模态**的知识
 * （JSON_INLINE_MAX = 2048）。查询层若写死这个数字，就是 A7 审计
 * 明令禁止的"查询层认识具体模态的表示细节"。
 *
 * 所以由 doc 模态注册、查询层只问。
 *
 * 【为什么需要它】
 * 编码器只负责"文本 → JSONB 字节"，不决定存放方式。
 * 写路径要做"内联还是走页链"的分流，就必须知道这个阈值 ——
 * 但数字不许出现在 query/ 里。注册表就是那条窄缝。
 *
 * 未注册返回 0（调用方据此保守处理）。
 */
int  val_set_json_inline_max(u32 n);
u32  val_json_inline_max(void);

/*
 * ============ 已编码 JSONB 的字节数 ============
 *
 * 内联值取 len；外部值取 ext_off（val_json_from_text 把长度放在那里）。
 *
 * 【为什么不直接用 v->len】
 * 外部值的 len 恒为 0（协议是"长度靠 detoast 问"）——
 * 直接读 len 会得到 0，于是"超限检查"永远不触发。这是个静默错。
 */
u32  val_json_encoded_len(const DbValue *v);

/*
 * ============ 页链 detoast 注册（读路径用） ============
 *
 * 从 record 解出页链标记之后，要用什么函数去读那条链？
 * 答案只能是 L4 的 json_ovf_detoast（它才知道页链的页内布局）。
 * 值层是 L1 ⇒ 通过注册把它接进来，与渲染器/编码器同一条纪律。
 *
 * 【为什么用 DetoastFn 这个既有类型而不是自定义签名】
 * json_ovf_detoast 的签名本来就与 DetoastFn 完全一致
 * （ctx=Pager*、key=链首页、off=链长度）。再包一层适配只会多一份语义。
 */
int       val_set_json_chain_detoast(DetoastFn fn);
DetoastFn val_json_chain_detoast(void);

/*
 * ============ 带解码上下文的记录解码 ============
 *
 * 【为什么必须有 decode_ctx 这个参数】
 * 页链值的 ext_ctx 必须是一个**能读这条链的稳定对象**（Pager*）。
 * 而 rec_decode 只拿到 (rec, schema)，不知道 Pager 在哪 ——
 * 所以由调用方（heap AM）把上下文传进来。
 *
 * decode_ctx 会被原样放进页链值的 ext_ctx。调用方必须保证它活到
 * 所有用这些值读完为止。
 *
 * 原来的 val_from_record / val_from_record_layout 保留为
 * "传 NULL 的薄封装"：它们遇到页链标记会返回 E_NOTSUP
 * （明确报错，不是静默给出不可读的值）。
 */
int  val_from_record_ctx(const ColDesc *cols, int ncols,
                         const u8 *rec, u16 len, void *decode_ctx,
                         DbValue *out);
int  val_from_record_layout_ctx(const RecLayout *L, const ColDesc *cols,
                                int ncols, const u8 *rec, u16 len,
                                void *decode_ctx, DbValue *out);

/*
 * ============ 主动归还页链（写路径失败补偿专用） ============
 *
 * 与 val_json_free 的分工：
 *   val_json_free        清掉运行时标记，**不碰链**（保证拷贝安全）
 *   val_json_release_chain  真的把链还回页分配器
 *
 * 【为什么必须分开】
 * val_copy 会拷 ext_ctx 指针 —— 副本被清时若顺手释放共享的链，
 * 原值就悬空了。所以清标记和还链是两个语义相反的动作。
 *
 * 【为什么通过注册而不是直接调】
 * 值层是 L1、json_ovf 在 L4；直接调就是反向依赖，分层门禁会红。
 * doc 模态在 db_json_init 里注册归还函数。
 *
 * 非页链值调用本函数返回 E_INVAL（调用方用错了）。
 */
int  val_set_json_chain_free(int (*fn)(void *pager, u64 head));
int  val_json_release_chain(DbValue *value);

/*
 * 渲染成新分配的 C 字符串（调用方 free）。
 *
 * 【为什么不能只靠 val_to_str】
 * val_to_str 有 cap 上限（REPL / 日志等"短输出"场景用它）。
 * 结果集投影必须给完整文本 —— 512 字节的栈缓冲会截断长文档，
 * 用户看到的是一个语法不完整的 JSON，比报错更难查。
 *
 * 实现口径：先问渲染器要长度，再按该长度分配并写内容
 * （jsonb_render 的两趟调用协议）。
 */
int  val_to_json_alloc(const DbValue *v, char **out, u32 *len);

/*
 * 物化：把值的内容读进新分配的缓冲。
 *
 * 内联值 → 拷一份出来。
 * 外部值 → 调 ext_fn 读回来。
 *
 * 返回的缓冲由**调用方 free**。失败返回错误码，*buf_out 保持 NULL。
 *
 * 为什么单独提供这个函数：外部值的字节数事先不知道，
 * 调用方没法自己定缓冲大小。物化是唯一能拿到准确长度的时机。
 */
int  val_detoast(const DbValue *v, u8 **buf_out, u32 *len_out);

/*
 * 比较两个值。
 *
 * 返回值：-1 / 0 / 1（大小关系）。三值逻辑下任一为 NULL 则返回 0
 * 且 *is_null = true。
 *
 * *err：出错时的错误码（正常为 OK）。外部值物化失败会写在这里。
 *
 * 【为什么要单独一个 err 参数】
 * 比较结果是 -1/0/1，而错误码 E_* 从 1 开始 ——
 * 1 既是"大于"也是 E_NOSPACE。混用一个返回值必然有一处要靠猜。
 * 分开就没有歧义。
 */
int  val_cmp(const DbValue *a, const DbValue *b, bool *is_null, int *err);

/* 真值判断：NULL 返回 -1（既非真也非假） */
int  val_truth(const DbValue *v);

/* 转字符串。返回写入长度。外部值会先物化。 */
int  val_to_str(const DbValue *v, char *buf, int cap);

/* 记录 <-> 值数组 */
int  val_from_record(const ColDesc *cols, int ncols,
                      const u8 *rec, u16 len, DbValue *out);

/*
 * 用预计算好的布局解码 —— 热路径用这个。
 *
 * 布局只跟列定义有关，算一次用一辈子（见 record.h 的 RecLayout 说明）。
 */
int  val_from_record_layout(const RecLayout *L, const ColDesc *cols, int ncols,
                             const u8 *rec, u16 len, DbValue *out);
int  val_to_record(const ColDesc *cols, int ncols, const DbValue *vals,
                   u8 *buf, u16 cap, u16 *out_len);

/*
 * ==================== 通用值层函数表（v1.6） ====================
 *
 * 【它是什么】
 * 一个"名字 → 实现"的注册表。SQL 里的函数调用表达式（`EX_FUNC`）
 * 按名字在这里查表执行。
 *
 * 【为什么放在值层而不是 query 层】
 * 值层是"通用标量 + 外部值"的地方，是所有模态的公共词汇表。
 * 函数表放这里 ⇒ query 层只做一次查表，**不认识任何具体函数**。
 * 这与硬规则 7（执行器不许知道模态）是同一条纪律，只是换了个对象。
 *
 * 【谁注册】
 * 提供这个函数的模态在自己的初始化入口注册
 * （例：文档模态在 `db_json_init()` 里注册 `json_match` / `json_extract`）。
 *
 * 【并发与生命周期】
 * 它是**进程级只读全局**：注册发生在 `db_open` 里，任何查询开始之前；
 * 之后只读不写 ⇒ 并发读安全。
 *
 * 【为什么函数签名里没有 DbValue 之外的东西】
 * 签名 `(args, nargs, out)` 是**按行**的、无上下文的 —— 这样执行器
 * 不需要知道函数的任何语义，也不需要给它传实例状态。
 * 需要外部状态的函数（如读页链）通过 `DbValue` 的外部值机制拿
 * （`val_detoast`），不需要额外参数。
 */
typedef int (*ValFunc)(const DbValue *args, int nargs, DbValue *out);

/* 注册一个函数。同名返回 E_INVAL（不覆盖已有实现）。 */
int  val_func_register(const char *name, ValFunc fn);

/* 查一个函数。找不到返回 NULL。 */
ValFunc val_func_find(const char *name);

#endif /* DB_STORAGE_VALUE_H */
