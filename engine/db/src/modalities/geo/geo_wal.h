#ifndef DB_MODALITIES_GEO_GEO_WAL_H
#define DB_MODALITIES_GEO_GEO_WAL_H

#include "common/error.h"
#include "common/loghook.h"
#include "common/types.h"
#include "storage/page.h"
#include "storage/pager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * geo_wal.h —— 地理空间模态的**写日志接线**（v1.4 步骤 6 · A7）
 *
 * ==================== 这一层在干什么 ====================
 *
 * R-tree 的页是**原地改写**的：插入一个点会把叶子页、父页、
 * 根页都改一遍。断电时这些页可能只改了一半 —— 树就废了。
 *
 * 本文件给模态层提供**唯一**的写页出口：改完页先写日志、
 * 再把 LSN 写进页头、最后才把页还给缓冲池。
 *
 * ==================== 分层：模态不许碰 wal/ ====================
 *
 * wal/ 是 L1，modalities/ 是 L4 —— 模态**不能** include wal/。
 * 所以本层用的是 L0 的 common/loghook.h：db 层把钩子注入进来，
 * 模态只管"把页镜像交给钩子"。
 *
 * 接线与全文检索（FT）同一套路：
 *   ft_index_create(pager, db_wal_hook, inst, 0, &ix)
 *   geo_state_create_wal(pager, db_wal_hook, inst, &st)
 *
 * ==================== xid：为什么固定是 0 ====================
 *
 * loghook.h 的铁律是"xid 必须显式传参，不能藏在共享对象里"。
 * geo 这边**没有事务**：公开 API（db_geo_insert 等）不带事务参数，
 * 模态层也拿不到事务上下文。所以每次写日志都显式传 xid = 0
 * （系统事务）—— 见 geo_wal_log_page 的调用点。
 *
 * 后果要说清楚：xid = 0 的记录在恢复阶段**永不回滚**
 * （见 wal/recovery.c 的 xid_committed：0 视为已提交）。
 * 所以 geo 的"前像"只作完整性留痕，不是回滚用的。
 * 将来若要把 geo 的写纳入用户事务，必须同时补真正的 undo。
 *
 * ==================== 代价：整页镜像 ====================
 *
 * 页是定长 4KB 的整块结构（R-tree 的条目是变长数组，删一条
 * 就要挪后面的字节），没法像堆表那样只记 slot 的差异 ——
 * 所以只能整页记：before 4KB + after 4KB = 8KB 一条记录。
 * 这就是 geo 的日志放大量约为数据量的 4 倍。
 */

/* 写日志上下文（模态侧只认这个结构，不认 WAL） */
typedef struct {
    LogHookFn fn;   /* 钩子；NULL = 本库不写日志（单测直接用模态时） */
    void   *ctx;    /* 钩子上下文（db 层传 DbInstance *） */
} GeoWal;

/*
 * 新分配页的"前像"：整页全零。
 *
 * pager_alloc 分配的页是清零的（见 pager.c），所以新页的前像
 * 就是这张全零表 —— 不必每个调用点自己造一个缓冲区。
 */
extern const u8 GEO_ZERO_PAGE[PAGE_SIZE];

/*
 * 把一页的镜像写进日志（LOG_PAGEDATA）。
 *
 * before —— 改之前的整页内容；NULL 表示"没有前像"（记录里 blen = 0）。
 * after  —— 改之后的整页内容（PAGE_SIZE 字节，必填）。
 *
 * 成功时 *lsn_out 拿到这条记录的 LSN（写进页头做幂等判断）。
 * 失败返回非 OK —— 调用方必须**放弃这次修改**。
 */
int geo_wal_log_page(const GeoWal *w, u32 page_id, const u8 *before,
                     const u8 *after, u32 *lsn_out);

/*
 * 写页的统一出口：先写日志 → 写 LSN → 归还页（unpin dirty）。
 *
 * before —— 改之前的整页内容（调用方必须在**改页之前**拍下来）；
 *          NULL 表示没有前像。
 *
 * 日志写失败时：把页**恢复成改之前的样子**、按"不是脏页"归还，
 * 然后把错误码原样返回 —— 半改的页绝不能落盘。
 *
 * 没有日志钩子时（w == NULL 或 w->fn == NULL）：只算校验和、归还页，
 * 行为与"接 WAL 之前"完全一致（单元测试直接用模态时走这条路）。
 */
int geo_pg_commit(Pager *pg, u32 page_id, PageFrame *fr, const GeoWal *w,
                  const u8 *before);

/*
 * rtree_scan 的 WAL 版：标记死条目时把叶子页写进日志。
 *
 * 与 rtree_mark_dead 的区别只有一个：多一个写日志上下文。
 * 死标记改了叶子页 —— 不记日志的话，崩溃重放后这个删除会**消失**，
 * 被删的点会诈尸（这是坑 M4-38 留下的窟窿）。
 *
 * rtree_mark_dead（不带 WAL 的版本）保留原样，供不写日志的场合使用。
 */
int rtree_mark_dead_wal(Pager *pg, u32 root_page, u32 row_id, const GeoWal *wal,
                         u64 *page_reads_out);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_GEO_GEO_WAL_H */
