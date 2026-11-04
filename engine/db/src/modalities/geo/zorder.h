#ifndef DB_MODALITIES_GEO_ZORDER_H
#define DB_MODALITIES_GEO_ZORDER_H

#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Z-order —— 地理空间模态的**第二条**索引路线（对照用）。
 *
 * 【为什么要有第二条路】
 *
 * R-tree 是"跟着数据长"的：MBR 是算出来的，所以它对**聚簇**的数据很友好。
 * 代价是**插入要维护平衡、要分裂、要写 WAL**，结构复杂。
 * Z-order（Morton 曲线）是"先把空间铺成一条线"的：结构极简、只追加不分裂，
 * 但**网格是固定的** —— 数据聚在哪里，它不在乎。
 * A5 验收（`bench/geo_bench.c`）要把这两条路在三组分布下的窗口 IO 摆在一起比，
 * 结论必须来自实测，不能靠"听说"。
 *
 * 【本文件是什么，不是什么】
 *
 * 是：Morton 编码 + 固定层级的网格桶 + 窗口查询（桶 = 一页的 IO 单位）。
 * 不是：计划里写的"BigMin 区间分解 + 塞进现有 B+tree"。
 *      ⚠️ **这一条如实标注为"未做"**：BigMin 要解"一个矩形在 Z 序上拆成
 *      几个连续区间"，实现复杂且容易错；而固定网格桶天然就是**超集**
 *      （落在窗口里的单元整桶扫、再按矩形过滤），正好满足 R4 的超集断言。
 *      B+tree 未接 ⇒ IO 口径用"桶访问数"（一桶 ≈ 一页），
 *      与 R-tree 的 `rtree_page_reads()` 一一对应。
 *
 * 【量化：坐标必须先变成整数】
 *
 * Morton 码是**整数**的位交错，所以 double 坐标要先量化成 u32
 * （每个轴 32 位 ⇒ 码 64 位）。量化用**线性映射 + 截断**：
 *   q = (uint32)((v - lo) / (hi - lo) * 4294967295.0)
 * 两端都**闭区间**（与 mbr.h 的闭区间语义一致）。
 * ⚠️ 量化的代价：double 坐标被压成 32 位 ⇒ **精度丢失**。
 *   所以本模块的窗口查询判据是"量化坐标在窗口内"，
 *   而 R-tree 与暴力基准用的是**原始 float 坐标** ——
 *   三者对同一点的判断可能有 1 ulp 级别的差异，
 *   验收时必须**按同一套量化坐标**比较（见 `bench/geo_bench.c` 的做法）。
 */

#define GEO_Z_BITS         32u   /* 每个坐标轴的位数 */
#define GEO_Z_ENTRY_BYTES  12u   /* 一个桶条目：u64 code + u32 row_id */
#define GEO_Z_PAGE_BYTES   4096u /* 一个页 */
#define GEO_Z_BUCKET_CAP   (GEO_Z_PAGE_BYTES / GEO_Z_ENTRY_BYTES) /* 341 条/桶 ≈ 一页 */

/* ---------------- 量化 ---------------- */

/*
 * 把 double 坐标量化成 u32（0 .. 2^32-1）。
 *
 * 约定：
 * - v < lo 或 v > hi ⇒ **截断到两端**（不报错）——
 *   量化是"投影"，投影到边界之外就落在边界上；
 * - hi <= lo（退化区间）⇒ 一律返回 0（所有点落进同一个格子里）。
 */
u32 geo_zorder_quantize(double v, double lo, double hi);

/* 反量化（量化坐标 → double），用同一个 [lo, hi]。 */
double geo_zorder_dequantize(u32 q, double lo, double hi);

/* ---------------- Morton 编码 ---------------- */

/*
 * Morton（Z-order）编码：把 (x, y) 的 32 位**逐位交错**成一个 u64。
 *
 *   约定：x 的第 i 位 → 码的第 2i 位；y 的第 i 位 → 码的第 2i+1 位。
 *
 * 这是**双射**（32×32 位 ↔ 64 位）：任何两个不同的点，码一定不同；
 * 任何码都能唯一解回 (x, y)。窗口过滤要靠这一点（从码还原坐标再判断）。
 */
u64 geo_zorder_encode(u32 x, u32 y);

/* 解码（encode 的逆）。 */
void geo_zorder_decode(u64 code, u32 *x, u32 *y);

/* ---------------- 索引对象 ---------------- */

typedef struct GeoZIndex GeoZIndex;

/*
 * 建一棵 Z-order 索引。
 *
 * 参数：
 * - qx / qy：量化后的坐标数组（长度 n）
 * - row_ids：与坐标一一对应的行号（长度 n）
 * - n：点数
 * - cell_bits：网格层级 G —— 每个轴切成 2^G 格（0 .. 16）。
 *   G = 0 ⇒ 整个空间一个格子（退化成全扫）。
 *   建议按"每格平均 ≈ GEO_Z_BUCKET_CAP 个点"来选：
 *   G ≈ log2(sqrt(n / GEO_Z_BUCKET_CAP))。
 *
 * 桶的组织：**按格子的 Morton 序**排（同格子的点连在一起），
 * 一个桶最多 GEO_Z_BUCKET_CAP 个点（超出就再开一个桶）。
 * 桶 = 一个页的 IO 单位。
 *
 * 返回 NULL 表示参数非法（n > 0 但坐标指针为空 / cell_bits > 16）或内存不足。
 */
GeoZIndex *geo_zorder_build(const u32 *qx, const u32 *qy, const u32 *row_ids,
                            u32 n, u32 cell_bits);

void geo_zorder_free(GeoZIndex *ix);

/* 点数 / 桶数 / 网格层级 / 非空格子数 */
u32 geo_zorder_n_points(const GeoZIndex *ix);
u32 geo_zorder_n_buckets(const GeoZIndex *ix);
u32 geo_zorder_cell_bits(const GeoZIndex *ix);
u32 geo_zorder_n_cells(const GeoZIndex *ix);

/* ---------------- 窗口查询 ---------------- */

/*
 * 窗口查询（坐标已量化，闭区间）。
 *
 * 命中判据：点落在 [x0,x1] × [y0,y1] 内（**闭区间**，边界算命中）。
 * 超集保证：凡落在窗口内的点，其所在格子一定与窗口相交 ⇒ 一定被扫到 ⇒
 *          不会漏点（代价是可能多扫一些**不在**窗口里的点）。
 *
 * 输出：
 * - out_row_ids：命中点的 row_id（**顺序按 row_id 升序**，便于与暴力逐位对照）
 * - n_out：命中点数；超过 max_out 返回 E_NOSPACE（不静默截断）
 * - buckets_touched（如可为 NULL）：本次扫了多少个桶（= IO 单位数）
 * - points_scanned（如可为 NULL）：本次扫了多少个点（含被过滤掉的）
 */
int geo_zorder_window(const GeoZIndex *ix,
                      u32 x0, u32 x1, u32 y0, u32 y1,
                      u32 *out_row_ids, u32 max_out, u32 *n_out,
                      u64 *buckets_touched, u64 *points_scanned);

/*
 * 只算 IO、不取结果（基准用：把"扫多少桶"与"取多少结果"分开量）。
 * 参数与 geo_zorder_window 相同，out_row_ids 传 NULL 即可。
 */
int geo_zorder_window_io(const GeoZIndex *ix,
                          u32 x0, u32 x1, u32 y0, u32 y1,
                          u64 *buckets_touched, u64 *points_scanned);

/*
 * 取**候选集**：本次扫到的所有点（**没有**按窗口过滤）。
 *
 * 这是给 R4 的超集断言用的：计划要求"分解出的区间并集 ⊇ 窗口内所有点"，
 * 而"窗口内的点"是过滤之后才知道的 —— 所以必须能单独拿到"过滤之前"的集合，
 * 才能断言 **候选 ⊇ 窗口点**（不漏点）。
 *
 * 顺序按 row_id 升序（与 geo_zorder_window 的输出顺序一致，便于逐位比较）。
 */
int geo_zorder_window_candidates(const GeoZIndex *ix,
                                  u32 x0, u32 x1, u32 y0, u32 y1,
                                  u32 *out_row_ids, u32 max_out, u32 *n_out,
                                  u64 *buckets_touched, u64 *points_scanned);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_GEO_ZORDER_H */
