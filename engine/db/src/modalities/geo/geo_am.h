#ifndef DB_MODALITIES_GEO_AM_H
#define DB_MODALITIES_GEO_AM_H

#include "common/error.h"
#include "common/types.h"
#include "modalities/geo/geo_state.h"
#include "spi/storage_am.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * geo_am.h —— 地理空间模态的 SPI 适配：geo_points 虚拟表（v1.4 步骤 5）
 *
 * 【为什么这个文件在 modalities/geo/ 而不是 spi/】
 *
 * 计划工程里写的是 `spi/geo_am.{h,c}`，但**分层门禁不允许**：
 * spi/ 是 L3，modalities/ 是 L4，L3 不许依赖 L4。
 * 适配器必须**跟模态走** —— 与向量模态的 vector_am.c、全文检索的
 * ft_am.c 同一个做法。门禁见 tools/check_deps.sh。
 *
 * 【geo_points 是什么】
 *
 * 一张**只读的元数据视图**：把树里的每个点摊平成一行。
 *
 *   row_id   INT8   点的标识（插入顺序分配，单调递增）
 *   x        REAL   横坐标
 *   y        REAL   纵坐标
 *   deleted  BOOL   是否已被标记删除
 *
 * 【为什么 deleted 是一列，而不是"删了就不出现"】
 *
 * 与 ft_docs 的 sealed 列同一个理由：删除语义是"打标记"，
 * 状态由**行内容**表达，不由可见性表达。
 * 再用可见性藏一次，就是同一件事说两遍。
 *
 * 【写入不走这条路】
 *
 * geo_points **只读**：insert / update / remove / vacuum 一律返回 E_NOTSUP。
 * 写入走专用 API（db_geo_insert / db_geo_delete）——
 * 与向量模态、FT 模态同款：写路径不只是"加一行"，它还要**更新索引**。
 *
 * 【为什么没有窗口/KNN 的下推】
 *
 * 窗口查询要的是一个**空间谓词**，而 SQL 谓词系统里没有这个算子
 * （计划 §1.1 明确把空间谓词划在范围外）。
 * 窗口/KNN 走专用 API（db_geo_window / db_geo_knn）。
 */

/* 列号 */
enum {
    GEO_COL_ID   = 0,   /* row_id，INT8 */
    GEO_COL_X    = 1,   /* x，REAL */
    GEO_COL_Y    = 2,   /* y，REAL */
    GEO_COL_DEAD = 3    /* deleted，BOOL */
};

#define GEO_POINTS_NCOLS 4

/* AM 的私有状态 */
typedef struct {
    GeoState *st;   /* 模态状态（实例级共享：所有连接看同一批点） */
} GeoAmCtx;

/*
 * 构造 geo_points 的 AM。
 *
 * out 与 ctx 由**调用方持有**（通常嵌在 DbInstance 里），
 * 本函数只往里填，不分配。
 */
int geo_am_build_points(GeoState *st, StorageAM *out, GeoAmCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_GEO_AM_H */
