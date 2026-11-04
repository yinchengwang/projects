#ifndef DB_MODALITIES_TS_TS_AM_EXT_H
#define DB_MODALITIES_TS_TS_AM_EXT_H

#include <stdbool.h>
#include <stddef.h>

#include "common/types.h"
#include "modalities/ts/ts_am.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ts_am_ext.h —— AM 上下文的扩展（v1.5 步骤 5）
 *
 * ==================== 坑 M6-53：契约装不下的东西，放"扩展结构"里 ====================
 *
 * ts_am.h 是**别人定的契约**，本轮不改它。而契约里的上下文只有两个字段：
 *
 *   typedef struct {
 *       TsStore *st;       模态状态
 *       bool     writable; 是否允许 insert
 *   } TsAmCtx;
 *
 * 但本模态的 AM 还需要装**三样**东西：
 *
 *   1. 时间范围（下推的裁剪条件）
 *   2. 这张 AM 是哪一张表（读数表还是分区表 —— 两者列不同、行为不同）
 *   3. 契约里的那两个字段本身
 *
 * 硬塞进 TsAmCtx ⇒ 要改契约 ⇒ 不行。
 * 另开一个全局表（static 数组 + 指针做键）⇒ 进程级共享状态 ⇒ 两个库
 * 同时打开就串味 ⇒ 更糟。
 *
 * ⇒ 用**"首成员技巧"**：定义一个扩展结构，把契约里的 TsAmCtx 放在**首成员**，
 *   于是 `&ext.base == (void *)&ext`，外部传 `&ext.base` 给契约里的函数，
 *   本模块再原样转回 `TsAmExt *` —— 零拷贝、零共享状态、不改契约。
 *
 * 代价：靠的是"首成员偏移为 0"这个布局事实 ⇒ 用 _Static_assert 钉死，
 * 哪天有人把 base 挪到第二个成员，编译期就炸，不会等到运行时才错。
 *
 * 【为什么值得多一个头文件】
 *
 * 扩展结构要**两处都看得见**：本模块（读写它）与 db 层（分配它）。
 * 放在 db 层 ⇒ 本模块要 include db 层的头 ⇒ 依赖方向反了（模态在 L4）。
 * 放在本模块 ⇒ db 层 include 模态的头 ⇒ 方向正确 ✓（与 geo_state.h 同一个套路）。
 */

#define TS_AM_KIND_READINGS   0   /* ts_readings：一条读数一行 */
#define TS_AM_KIND_PARTITIONS 1   /* ts_partitions：一个分区一行 */

typedef struct {
    /*
     * 契约里的上下文（**首成员** ⇒ &base == &ext，见上面的坑 M6-53）。
     */
    TsAmCtx base;

    /* 这张 AM 是哪一张表 */
    int  kind;

    /* 时间范围（下推的裁剪条件）；has_range == false ⇒ 不裁剪（超集语义） */
    bool has_range;
    i64  lo_ms;
    i64  hi_ms;
} TsAmExt;

/* 首成员偏移必须是 0 —— 否则上面那套"原样转回去"的把戏就不成立 */
_Static_assert(offsetof(TsAmExt, base) == 0, "TsAmExt.base 必须是首成员");

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_TS_TS_AM_EXT_H */
