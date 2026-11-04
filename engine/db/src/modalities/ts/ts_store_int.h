#ifndef DB_MODALITIES_TS_TS_STORE_INT_H
#define DB_MODALITIES_TS_TS_STORE_INT_H

#include "common/types.h"
#include "modalities/ts/ts_dir.h"
#include "modalities/ts/tsstore.h"
#include "storage/pager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ts_store_int.h —— 时序存储的**私有**结构（v1.5 步骤 3）
 *
 * 【为什么要有这个文件】
 *
 * 封窗的第二步（把目录项翻成 SEALED）与封窗的编排要碰两样私有东西：
 * 目录对象（TsDir）与分区的运行时状态（rt[]）。
 *
 * 这两样在 tsstore.c 里是**私有的**（tsstore.h 是对外契约，不能塞私货）。
 * 两种选择：
 *   - 在 tsstore.c 里实现封窗 ⇒ tsstore.c 会涨到 900 行（**上限 800**）；
 *   - 把私有结构单独放一个头文件 ⇒ ts_seal.c 也能看到它们。
 *
 * 选后者。代价是多一个文件，收益是：
 *   - tsstore.c 保持在 800 行以内（项目铁律）；
 *   - 私有结构只有一份定义（两处各写一份 ⇒ 迟早字段对不上）。
 *
 * ⚠️ 这个头文件**不是对外契约**：只有 tsstore.c 与 ts_seal.c 包含它。
 *    对外的接口一律走 tsstore.h / ts_seal.h。
 */

/*
 * 一个分区的**运行时**状态（按目录的磁盘序下标）。
 *
 * 目录项里存的是"水位线"（坑 M6-30），真正的行数与页链尾放在这里。
 */
typedef struct {
    u32 last_page;    /* 页链尾（PAGE_ID_NIL = 空分区 / 已封窗的只读分区） */
    u32 n_rows;       /* 真实行数 */
    u32 page_count;   /* 真实页数 */
} TsPartRt;

/* 时序存储对象 */
struct TsStore {
    Pager  *pg;
    TsDir  *dir;      /* 目录（存与取交给 ts_dir） */
    u32     span_sec;

    TsPartRt *rt;     /* 运行时状态，按目录磁盘序 */
    u32       rt_count;   /* 已用个数（= 目录里的分区数） */
    u32       rt_cap;     /* 容量 */

    /*
     * 时间序下标 → 磁盘序下标。
     *
     * ts_dir 内部有一份排序数组，但不暴露"时间序第 i 个是磁盘序第几个"。
     * 而 ts_store_part_info 承诺的是"按起始时间升序" ⇒ 这里自己维护一份。
     */
    u32      *by_time;
    u32       by_time_count;
    u32       by_time_cap;

    u64 total_rows;   /* 活着的读数总数 */
    u64 page_reads;   /* 累计逻辑页读（口径同 db_geo_page_reads） */
    u32 seal_seq;     /* 封窗序号（单调递增；没有 WAL ⇒ 没有 LSN，见坑 M6-41） */
};

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_TS_TS_STORE_INT_H */
