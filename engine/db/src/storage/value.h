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
 */

#define VAL_TEXT_MAX 256

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

#endif /* DB_STORAGE_VALUE_H */
