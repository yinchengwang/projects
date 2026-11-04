#ifndef DB_MODALITIES_TS_TS_AM_H
#define DB_MODALITIES_TS_TS_AM_H

#include "common/error.h"
#include "common/types.h"
#include "modalities/ts/tsstore.h"
#include "spi/storage_am.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ts_am.h —— 时序模态的 SPI 适配（v1.5 步骤 5）
 *
 * 【为什么这个文件在 modalities/ts/ 而不是 spi/】
 *
 * 计划工程里写的是 `spi/ts_am.{h,c}`，但**分层门禁不允许**：
 * spi/ 是 L3，modalities/ 是 L4，L3 不许依赖 L4。
 * 适配器必须**跟模态走** —— 与向量模态的 vector_am.c、全文检索的 ft_am.c、
 * 地理空间的 geo_am.c 同一个做法。门禁见 tools/check_deps.sh。
 *
 * 【两张虚拟表】
 *
 *   ts_readings    一行 = 一条读数（time / tag / value）
 *   ts_partitions  一行 = 一个分区（分区元数据）
 *
 * 【时间谓词能不能下推？】
 *
 * **能**，而且这是本模态最重要的性能杠杆：
 * 时间范围谓词 → 只读命中的分区（**超集**语义）。
 * 裁剪发生在 **AM 的 scan_begin 里** —— 执行器从头到尾不需要知道分区存在
 * （计划判决 1 的同一个理由）。
 *
 * ⚠️ **裁剪只能少读，不能少给**（坑 M6-5）：
 * 谓词不可解析（OR / 函数包裹 / 没有谓词）⇒ **全部分区**。
 * "我猜这个谓词大概只涉及最近 3 个分区"就是静默漏数据。
 *
 * 【写入不走这条路吗？】
 *
 * `ts_readings` 的 **insert 是支持的**（计划步骤 5 明确写了
 * "`insert` 支持但 `update/remove` 返回 `E_NOTSUP`"）——
 * 这与 geo / vector / ft 模态不同（那三个只读）。
 * 理由：时序的写入就是"追加一条读数"，没有"更新索引"那层复杂度。
 * ⚠️ 但 update / remove 仍然 E_NOTSUP —— 时序不可变是建模前提
 * （计划 §1.1 明确不做更新 / 删除单行；唯一的删除路径是 TTL 的整分区）。
 *
 * 【行的字节格式】
 *
 * 固定 24 字节（小端），与活跃分区里的行格式**完全一致**：
 *
 *   偏移 0   i64   time（毫秒时间戳）
 *   偏移 8   i64   tag
 *   偏移 16  u64   value 的**位模式**（double）
 *
 * 浮点存位模式（common/types.h 铁律 1：磁盘格式只用定宽无符号整数），
 * 互转走 memcpy，不用指针双关。
 */

/* ---------------- ts_readings 的列 ---------------- */

enum {
    TS_COL_TIME  = 0,   /* time，INT8（毫秒时间戳） */
    TS_COL_TAG   = 1,   /* tag，INT8 */
    TS_COL_VALUE = 2    /* value，REAL */
};

#define TS_READINGS_NCOLS 3

/* ---------------- ts_partitions 的列 ---------------- */

enum {
    TS_COL_PART_START = 0,   /* start_ms，INT8 */
    TS_COL_PART_END   = 1,   /* end_ms，INT8 */
    TS_COL_PART_STATE = 2,   /* state，INT4（ACTIVE/SEALED/DROPPED） */
    TS_COL_PART_ROWS  = 3,   /* n_rows，INT8 */
    TS_COL_PART_PAGES = 4    /* page_count，INT8 */
};

#define TS_PARTITIONS_NCOLS 5

/* AM 的私有状态 */
typedef struct {
    TsStore *st;          /* 模态状态（实例级共享：所有连接看同一批读数） */
    bool     writable;    /* ts_readings 是否允许 insert（建表时可关） */
} TsAmCtx;

/*
 * 构造 ts_readings 的 AM。
 *
 * out 与 ctx 由**调用方持有**（通常嵌在 DbInstance 里），
 * 本函数只往里填，不分配。
 */
int ts_am_build_readings(TsStore *st, StorageAM *out, TsAmCtx *ctx);

/*
 * 构造 ts_partitions 的 AM。
 *
 * ts_partitions **只读**：它只是分区目录的一层视图，
 * 分区的增删由 ts_store 的写路径与 TTL 决定（计划判决 1：
 * 用户可见性由虚拟表解决，不需要系统表）。
 */
int ts_am_build_partitions(TsStore *st, StorageAM *out, TsAmCtx *ctx);

/*
 * 设置"时间范围下推"的过滤条件。
 *
 * ⭐ **这是裁剪的入口**：调用方（db 层从谓词里解析出时间范围后）
 * 把范围交给 AM，AM 的 scan_begin 就只扫命中的分区。
 *
 * 没有下推条件（NULL / 没有有效范围）⇒ **全部分区**（超集语义）。
 *
 * ⚠️ **下推必须给全"或"**：范围解析不出来时**不要**给条件，
 * 宁可全扫，也不能少给。
 */
void ts_am_set_time_range(TsAmCtx *ctx, bool has_range, i64 lo_ms, i64 hi_ms);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_TS_TS_AM_H */
