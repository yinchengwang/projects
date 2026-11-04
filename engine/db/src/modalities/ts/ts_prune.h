#ifndef DB_MODALITIES_TS_TS_PRUNE_H
#define DB_MODALITIES_TS_TS_PRUNE_H

/*
 * ts_prune.h —— 时间谓词下推 → 分区裁剪（v1.5 步骤 4 · T7）
 *
 * 【这一层解决什么】
 *
 * 时序数据的查询绝大多数带时间范围（"最近一天"、"某月"）。
 * 而数据是**按时间分区**存的 ⇒ 只要知道要读的时间范围，
 * 就知道该读哪几个分区，其余的页**根本不用碰**。
 *
 * 这一层只干一件事：**把一个时间范围换算成分区集合**。
 *
 * 【铁律：裁剪只能少读，不能少给】（计划坑 M6-5）
 *
 * 裁剪算错了，后果是**静默漏数据**：
 * 查询按分区裁剪，漏掉的分区里的数据就永远查不到，
 * 而且没有任何报错 —— 用户只会看到"某些时间点的数据不见了"。
 *
 * 所以本层的规则是：
 *
 *   - 范围**能解析** ⇒ 只返回与范围相交的分区（**超集**语义：
 *     宁可多给几个分区，也不许少给）；
 *   - 范围**不能解析**（没有谓词 / OR / 函数包裹 / 范围反了）
 *     ⇒ **返回全部分区**。
 *
 * ⚠️ **绝不允许"猜"**。
 * "我猜这个谓词大概只涉及最近 3 个分区"就是坑 M6-5 的原型：
 * 猜错了不报错，只是数据不见了。
 * 宁可多读几个分区（慢一点），也不能少读（数据丢了）。
 *
 * 【超集语义的具体口径】
 *
 * 分区窗口是 [start_ms, end_ms)（左闭右开），
 * 查询范围 [t_lo, t_hi] 按**两端都含**处理。
 *
 * 相交判据：`start_ms <= t_hi && end_ms > t_lo`
 *
 * 于是：
 *   - t_hi 恰好落在某个分区的起点上 ⇒ 那个分区**也**算命中
 *     （因为 t_hi 本身含在范围内）；
 *   - t_lo 恰好等于某个分区的终点 ⇒ 那个分区**不算**命中
 *     （它的窗口是左闭右开，t_lo 不在它里面）；
 *   - 相邻分区之间有空档 ⇒ 空档不影响裁剪（少读的是空档，不是数据）。
 *
 * 调用方手里若是**左闭右开**的范围 [lo, hi)，
 * 传 t_hi = hi - 1 即可（毫秒精度）。
 *
 * 【本层不碰页】
 *
 * 这一层只算"该读哪些分区"，**不负责真的跳过页** ——
 * 分区的页链格式是 tsstore.c 的私有约定（见该文件的说明：
 * 同一张磁盘上不能出现两种目录/页格式）。
 *
 * 所以本层给两样东西：
 *   1. 裁剪结果（分区集合）—— 纯函数，可独立单测；
 *   2. 扫描时的过滤接法（ts_prune_scan）——
 *      用时间窗把行过滤掉，**保证不漏**；
 *      真正的"少读页"由拥有页链格式的读路径来用 ts_prune_allows_window()。
 */

#include "common/error.h"
#include "common/types.h"
#include "modalities/ts/ts_column.h"
#include "modalities/ts/tsstore.h"
#include "storage/pager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 裁剪的基本单位 ---------------- */

/*
 * 一个分区窗口（裁剪的输入）。
 *
 * state 是 TsPartState（存成 u32 只是为了这个结构不依赖 tsstore.h 的枚举）。
 */
typedef struct {
    i64 start_ms;
    i64 end_ms;
    u32 state;
} TsPruneWindow;

/* ---------------- 裁剪结果 ---------------- */

/*
 * 裁剪出来的分区集合。
 *
 * all == true 表示"没有裁剪"（范围不可解析）⇒ 集合里是**全部分区**。
 */
typedef struct {
    TsPruneWindow *wins;   /* 命中的窗口（按调用方给的顺序） */
    u32            count;
    u32            cap;
    bool           all;
} TsPruneSet;

/* 建一个空集合（malloc 失败 ⇒ E_NOSPACE） */
int  ts_prune_set_init(TsPruneSet *s);

/* 清空（保留已分配的内存） */
void ts_prune_set_clear(TsPruneSet *s);

/* 释放（连内存一起） */
void ts_prune_set_destroy(TsPruneSet *s);

/* ---------------- 纯函数 ---------------- */

/*
 * 这个窗口与 [t_lo, t_hi] 相交吗？（超集语义，见头文件的说明）
 *
 * 这是本层**唯一**的判定逻辑，裁剪与过滤都用它 ⇒ 只有一处可能出错。
 */
bool ts_prune_window_hits(i64 start_ms, i64 end_ms, i64 t_lo, i64 t_hi);

/*
 * 从 n 个窗口里挑出与 [t_lo, t_hi] 相交的。
 *
 * ⚠️ 本函数**接管** out：调用前 out 里的内容会被丢弃
 * （这样"传进来一个没初始化过的栈变量"也不会出事）。
 *
 * has_pred == false（没有可解析的时间谓词）
 *   ⇒ 集合 = 全部分区，out->all = true。
 *
 * t_lo > t_hi（范围反了 ⇒ 不可解析）
 *   ⇒ 同上：全部分区。
 *
 * 其余 ⇒ 集合 = 相交的窗口（**超集**：宁可多给）。
 *
 * 返回 OK / E_INVAL / E_NOSPACE。
 */
int ts_prune_select(TsPruneSet *out, const TsPruneWindow *wins, u32 n,
                    i64 t_lo, i64 t_hi, bool has_pred);

/* ---------------- 取窗口 ---------------- */

/*
 * 从 store 里取全部分区窗口（**按起始时间升序**）。
 *
 * out / n_out 由调用方 free（malloc 的）。
 * 返回 OK / E_INVAL / E_NOSPACE。
 */
int ts_prune_windows_of_store(TsStore *st, TsPruneWindow **out, u32 *n_out);

/* ---------------- 判定 ---------------- */

/* 是不是"没有裁剪"（范围不可解析） */
bool ts_prune_all(const TsPruneSet *s);

/* 集合里有几个分区 */
u32  ts_prune_count(const TsPruneSet *s);

/* 这个时间点要不要读？（裁剪后的行过滤用） */
bool ts_prune_contains_time(const TsPruneSet *s, i64 t_ms);

/* 这个分区要不要读？（读路径跳过页链时用） */
bool ts_prune_allows_window(const TsPruneSet *s, i64 start_ms, i64 end_ms);

/* ---------------- 扫描接法 ---------------- */

/*
 * 按裁剪结果扫描：**整分区跳过**，命中分区才读它的页。
 *
 * pg   —— 页分配器（读分区页要用；store 的页就在这个 pager 里）
 * st   —— 时序存储
 * 范围 —— [t_lo, t_hi]（两端都含）
 * has_pred —— 有没有可解析的时间谓词
 * fn / ctx —— 与 ts_store_scan 同型的回调
 * reads_out —— 本次读了多少页（可为 NULL）
 *
 * 有可解析的范围 ⇒ 只读命中分区的页；
 * 没有 ⇒ 与 ts_store_scan 读到的**集合**完全一样（只是遍历顺序不同：
 * 本函数按时间序，ts_store_scan 按创建序）。
 *
 * 【为什么"跳过整个分区"是安全的】（坑 M6-47 的路由不变式）
 *
 * 路由不变式：一条读数的时间戳一定落在它所在分区的时间窗里。
 * 于是"查询范围包含这条读数" ⇒ "该分区的时间窗与范围相交"
 * ⇒ 该分区一定被选中 ⇒ 这条读数一定会被读到。
 *
 * 所以按分区裁剪**不可能少给**。
 * （反过来，如果哪天允许"时间戳落在分区窗口之外"的回填数据，
 *  这个推理就不成立 —— 那时这条不变式必须先补上。）
 *
 * fn 返回非 0 ⇒ 停止扫描，原样返回它的值。
 */
int ts_prune_scan(Pager *pg, TsStore *st, i64 t_lo, i64 t_hi, bool has_pred,
                  TsReadFn fn, void *ctx, u64 *reads_out);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_TS_TS_PRUNE_H */
