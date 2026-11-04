#ifndef DB_MODALITIES_TS_TS_DIR_INT_H
#define DB_MODALITIES_TS_TS_DIR_INT_H

#include "common/types.h"
#include "modalities/ts/ts_dir.h"
#include "modalities/ts/ts_wal.h"
#include "storage/pager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ts_dir_int.h —— 分区目录的**私有**结构（v1.5 步骤 6 · T10）
 *
 * 【为什么要有这个文件】
 *
 * 目录这一层有两半：
 *   - **写的那半**（ts_dir.c）：建目录、扫页空间、追目录链、
 *     追加目录项、就地改写目录项；
 *   - **读的那半**（ts_dir_query.c）：取目录项、按时间找分区、
 *     计数与诊断。
 *
 * 两半都要看到 struct TsDir 的**字段**（内存镜像就在结构里），
 * 而 struct TsDir 之前是 ts_dir.c 里的私有定义 ⇒ 读的那半拿不到。
 *
 * 两条路：
 *   - 把读的那半塞回 ts_dir.c ⇒ 文件会顶过 800 行的上限
 *     （ts_dir.c 本来就已经 799 行，步骤 6 还要往里加 WAL 的写点）；
 *   - 把结构单独放一个头文件 ⇒ 两半都看得到，行数也分得开。
 *
 * 选后者。这与步骤 3 给 tsstore.c 用的 ts_store_int.h 是**同一个套路**
 * （私有结构单独一个头，对外契约 ts_dir.h 一个字都不改）。
 *
 * ⚠️ 这个头文件**不是对外契约**：只有 ts_dir.c 与 ts_dir_query.c 包含它。
 *    对外的接口一律走 ts_dir.h。
 */

/*
 * 目录对象（调用方持有）。
 *
 * 里面既有磁盘上的位置（meta 页 / 目录链），也有内存里的镜像
 * （条目数组 + 按时间排序的下标数组）。
 *
 * 【内存镜像的权威是磁盘】：任何写操作都先落盘再改内存，
 * 重开库时从磁盘重建（ts_dir_open）。
 */
struct TsDir {
    Pager *pg;

    u32 meta_page;   /* meta 页页号 */
    u32 dir_head;    /* 目录链首页 */
    u32 dir_tail;    /* 目录链尾页 */
    u32 span_sec;    /* 分区窗口（秒） */

    u32 n_parts;      /* 目录项个数 */
    u32 n_dir_pages;  /* 目录页数 */
    u32 meta_nparts;  /* meta 页里的水位线 */

    TsPartInfo *items;   /* 目录项（磁盘序） */
    u32 *item_page;      /* 每项所在目录页 */
    u16 *item_slot;      /* 每项在页内的槽位号 */
    u32 *sorted;         /* 按 start_ms 升序的下标 */
    u32 cap;             /* 上面四个数组的容量 */
    u64 page_reads;      /* 累计逻辑页读 */

    /*
     * 写日志上下文（v1.5 步骤 6 · T10）。
     *
     * fn == NULL = 不写日志（单元测试直接用模态时）。
     * 目录页与 meta 页都由本层写，所以上下文挂在这里 ——
     * ts_dir.c 的每个写点都走 ts_pg_commit（见 ts_wal.h）。
     */
    TsWal wal;
};

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_TS_TS_DIR_INT_H */
