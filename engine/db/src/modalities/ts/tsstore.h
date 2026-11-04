#ifndef DB_MODALITIES_TS_TSSTORE_H
#define DB_MODALITIES_TS_TSSTORE_H

#include "common/error.h"
#include "common/types.h"
#include "storage/pager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * tsstore.h —— 时序模态的分区存储（v1.5 步骤 2）
 *
 * 【这一层解决什么】
 *
 * 时序数据的写入形态：**按时间追加、几乎不更新、旧数据变冷**。
 * 所以存储形态是：
 *
 *   时间路由 → 落到某个分区 → 分区里裸写（slotted page 追加）
 *              ↓ 分区满了 / 时间窗滑过
 *            封窗（不可逆）→ 列式压缩页
 *
 * 这一层负责前半截：**分区目录 + 时间路由 + 活跃分区写**。
 * 封窗压缩与读路径是步骤 3。
 *
 * 【为什么分区目录"自管"，不进 catalog】
 *
 * 计划判决 1：catalog **不知道任何模态**（blob / graph / vector / ft 都没进 catalog，
 * 时序凭什么进）。分区元数据放在模态自己的一张 meta 页里，
 * 用户可见性由虚拟表 `ts_partitions` 解决（步骤 5 接 SPI）。
 *
 * 【分区窗口怎么算】
 *
 * 分区窗口 = `partition_span` 秒（默认 1 天，可配）。
 * 时间戳 t 属于分区 `floor(t / span)` —— 即
 * `part_start = (t / span) * span`，`part_end = part_start + span`。
 *
 * ⚠️ **时间戳的单位是毫秒**（i64），而窗口是**秒**。
 * 换算：`part_start_ms = (t_ms / (span_sec * 1000)) * (span_sec * 1000)`。
 * 用"先除再乘"而不是"先乘再除"，是为了保证**分区边界对齐**
 * （否则 1.5 天的窗口会让边界随时间漂移）。
 *
 * ⚠️ **路由必须断言 `part_start ≤ t < part_end`**（计划风险 R3）：
 * 算错窗口 ⇒ 数据落进错误的分区 ⇒ 查询时按分区裁剪就**静默漏数据**。
 * 所以 `ts_route_partition()` 内部有断言，负例进测试。
 *
 * 【负时间戳怎么办】
 *
 * 时间戳可以是负数（1970 之前）。除法要**向下取整**而不是向零截断 ——
 * 否则 -1ms 会被算进"第 0 个分区"以外的地方，边界就乱了。
 * 实现用 `t_ms / span_ms` 之后再判余数：余数为负就减 1。
 *
 * 【一行读数的结构】
 *
 * 单指标模型（计划 §1.1 明确不做多指标宽表）：
 *
 *   time  i64   毫秒时间戳
 *   tag   i64   标签（设备号 / 状态码）
 *   value f64   读数
 *
 * 行在**活跃分区**里是定长 24 字节（3 个 8 字节字段）。
 * 定长是为了"追加"最简单：不需要变长记录的槽偏移表。
 * ⚠️ 封窗后的列存里三列各自压缩（时间 dod / 标签 RLE / 值 Gorilla）——
 * 那是步骤 3 的事。
 */

typedef struct TsStore TsStore;

/* 一条读数（用户视角） */
typedef struct {
    i64    time;    /* 毫秒时间戳 */
    i64    tag;     /* 标签 */
    double value;   /* 读数 */
} TsReading;

/* 分区的状态 */
typedef enum {
    TS_PART_ACTIVE = 0,   /* 活跃：裸写 */
    TS_PART_SEALED = 1,   /* 封闭：列式压缩，只读 */
    TS_PART_DROPPED = 2   /* 已过期：页已回收，目录里留着历史 */
} TsPartState;

/* 分区目录里的一项（用户可见的那些） */
typedef struct {
    i64         start_ms;   /* 分区起始时间（含） */
    i64         end_ms;     /* 分区结束时间（不含） */
    TsPartState state;
    u32         n_rows;     /* 行数（封窗后是压缩前的行数） */
    u32         first_page; /* 分区首页号（PAGE_ID_NIL = 空分区） */
    u32         page_count; /* 占用的页数 */
} TsPartInfo;

/* ---------------- 时间路由（纯函数，可独立单测） ---------------- */

/*
 * 算出时间戳 t 属于哪个分区（返回分区的起始时间）。
 *
 * span_ms = 分区窗口（毫秒）；必须 > 0。
 * 负时间戳**向下取整**（见头文件的说明）。
 */
i64 ts_route_partition(i64 t_ms, i64 span_ms);

/* ---------------- 目录与生命周期 ---------------- */

/*
 * 新建一个时序存储：分配 meta 页（分区目录），span_sec = 默认 1 天。
 *
 * 返回 OK / E_NOSPACE / E_INVAL。
 */
int ts_store_create(Pager *pg, u32 span_sec, TsStore **out);

/*
 * 打开磁盘上已有的时序存储。
 *
 * 磁盘上没有 meta 页 → E_NOTFOUND（**不是错误**：
 * 那是个还没建过时序存储的库，和图模态、地理模态同一个口径）。
 */
int ts_store_open(Pager *pg, TsStore **out);

/* 释放（**不释放页** —— 页归 pager 管） */
void ts_store_destroy(TsStore *st);

/* 当前的分区窗口（秒） */
u32 ts_store_span_sec(const TsStore *st);

/* 改了分区窗口（只影响**之后**的写入；已有分区不动） */
int ts_store_set_span_sec(TsStore *st, u32 span_sec);

/* ---------------- 写 ---------------- */

/*
 * 追加一条读数。
 *
 * 路由到分区 → 必要时新建分区 → 追加到该活跃分区的最后一页。
 * 分区满了怎么办：**步骤 2 先 spill**（见下），封窗是步骤 3 的事。
 *
 * 返回 OK / E_NOSPACE / E_CORRUPT。
 */
int ts_store_insert(TsStore *st, const TsReading *r);

/* ---------------- 读 ---------------- */

/*
 * 遍历所有分区里的读数（**不做时间裁剪** —— 裁剪是步骤 4 的事）。
 *
 * 回调返回非 0 ⇒ 停止遍历，把该值原样返回给调用方。
 */
typedef int (*TsReadFn)(const TsReading *r, void *ctx);

int ts_store_scan(TsStore *st, TsReadFn fn, void *ctx);

/* 活着的读数总数 */
u32 ts_store_row_count(const TsStore *st);

/* ---------------- 目录查询 ---------------- */

/* 分区个数 */
u32 ts_store_part_count(const TsStore *st);

/*
 * 取第 i 个分区的信息（按起始时间升序）。
 * i 越界 → E_INVAL。
 */
int ts_store_part_info(TsStore *st, u32 i, TsPartInfo *out);

/*
 * 找到包含 t_ms 的分区；找不到 → E_NOTFOUND。
 * 找不到的常见原因：那个时间窗还没写过 / 已经被 TTL 丢弃。
 */
int ts_store_find_part(TsStore *st, i64 t_ms, TsPartInfo *out);

/*
 * 取出存储内部持有的分区目录（**只读用途**）。
 *
 * 【为什么需要这个出口】
 *
 * TTL 要操作目录（把过期分区标记成 DROPPED、回收它的页），
 * 而 `ts_ttl_expire()` 的接口是 `(Pager*, TsDir*, ...)` ——
 * 需要拿到**存储自己那份**目录。
 *
 * ⚠️ **必须是存储自己那份，不能另开一个副本**：
 * TTL 改的是副本的话，存储内存里的目录还是老的 ⇒
 * 查询会去读**已被回收的页** ⇒ 读到别人的数据而且不报错（坑 M6-57）。
 *
 * ⚠️ **越权用法**：调用方拿到这个指针**只能读**（查分区、遍历目录）。
 * 改目录要走 `ts_dir_update()`，那由 ts_ttl / ts_seal 这些**模态内部**的层去做。
 * 对外暴露这个出口是为了让 db 层能把 TTL 接到存储上，
 * 而不是让 db 层去理解目录的内部结构。
 */
struct TsDir;
const struct TsDir *ts_store_dir(const TsStore *st);

/* ---------------- 诊断 ---------------- */

/* 累计逻辑页读（与 db_geo_page_reads / db_ft_page_reads 同一口径） */
u64 ts_store_page_reads(const TsStore *st);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_TS_TSSTORE_H */
