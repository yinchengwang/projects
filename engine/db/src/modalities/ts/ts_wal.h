#ifndef DB_MODALITIES_TS_TS_WAL_H
#define DB_MODALITIES_TS_TS_WAL_H

/*
 * ts_wal.h —— 时序模态的**写日志接线**（v1.5 步骤 6 · T10）
 *
 * ==================== 这一层在干什么 ====================
 *
 * 时序模态的页全是**原地改写**的：
 *   - 追加一行读数 ⇒ 改分区数据页（PT_TS_PART）；
 *   - 封窗 ⇒ 写列式页（PT_TS_COL）+ 翻目录项；
 *   - 追加分区 / 改目录项 ⇒ 改目录页（PT_TS_DIR）与 meta 页（PT_TS_META）；
 *   - TTL ⇒ 改目录项 + 清零回收页。
 *
 * 断电时这些页可能只改了一半 ⇒ 分区读出来是**半截数据**
 * （而且往往"看起来正常"，比直接报错坏得多）。
 *
 * 本文件给模态层提供**唯一**的写页出口：改完页先写日志、
 * 再把 LSN 写进页头、最后才把页还给缓冲池。
 * 与地理模态（geo_wal.h）同一个套路。
 *
 * ==================== 分层：模态不许碰 wal/ ====================
 *
 * wal/ 是 L1，modalities/ 是 L4 —— 模态**不能** include wal/。
 * 所以本层用的是 L0 的 common/loghook.h：db 层把钩子注入进来，
 * 模态只管"把页镜像交给钩子"。
 *
 * 接线（db 层，src/db_ts.c）：
 *   TsWal w = { db_wal_hook, inst };
 *   ts_store_create_wal(inst->pager, span_sec, &w, &st);
 *   ts_store_open_wal(inst->pager, &w, &st);
 *
 * ==================== xid：为什么固定是 0 ====================
 *
 * loghook.h 的铁律是"xid 必须显式传参，不能藏在共享对象里"。
 * 时序这边**没有事务**：公开 API（db_ts_insert 等）不带事务参数，
 * 模态层也拿不到事务上下文。所以每次写日志都显式传 xid = 0
 * （系统事务）—— 见 ts_wal_log_page 的调用点。
 *
 * 后果要说清楚：xid = 0 的记录在恢复阶段**永不回滚**
 * （见 wal/recovery.c 的 xid_committed：0 视为已提交）。
 * 所以时序的"前像"只作完整性留痕，不是回滚用的。
 *
 * ⭐ 时序比地理还多一条好处：**读数是不可变的**
 * （计划 §1.1 明确不做更新 / 删除单行，唯一的删除路径是 TTL 整分区回收）。
 * 不变的行没有"回滚到某个旧值"的需求 ⇒ undo 永远用不上。
 *
 * ==================== 代价：整页镜像 ====================
 *
 * 裸写分区页是**定长行**（24 字节一行，页内偏移是纯算术），
 * 但页头的行数、链上的 next 都要一起改 ⇒ 没法只记"这一行"，
 * 只能整页记：before 4KB + after 4KB = 8KB 一条记录。
 *
 * ⚠️ **放大比地理还难看**（坑 M6-67）：
 * 时序是**一行一次页镜像**（每追加一行就改一次页），
 * 而一行只有 24 字节 ⇒ 8KB 的日志记 24 字节的数据，
 * 放大 300 多倍。地理是"一次树操作一页"，放大 4 倍。
 *
 * 这是"没有行存、只有页"的结构必然的代价。
 * 真要压下来，得给裸写页加 slot 目录（变成变长记录），
 * 或者改成"页写满时才记日志"（代价：崩了丢最后一页的行）。
 * 本轮如实记账，不假装没有。
 *
 * ==================== 顺序是硬要求 ====================
 *
 *   写日志 → 改页 → LSN 写进页头 → 归还页（标脏）
 *
 * 反过来的话（先改页再写日志）：页可能先于日志落盘 ——
 * 崩溃后日志里缺这条记录，页上却有它的内容，数据"没根"。
 *
 * 日志写失败 ⇒ **这次修改作废**：页恢复成改之前的样子、
 * 按"不是脏页"归还，返回错误码。半改的页绝不能落盘。
 */

#include "common/error.h"
#include "common/loghook.h"
#include "common/types.h"
#include "modalities/ts/ts_dir.h"
#include "modalities/ts/tsstore.h"
#include "storage/page.h"
#include "storage/pager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 写日志上下文（模态侧只认这个结构，不认 WAL） */
typedef struct {
    LogHookFn fn;   /* 钩子；NULL = 本库不写日志（单测直接用模态时） */
    void   *ctx;    /* 钩子上下文（db 层传 DbInstance *） */
} TsWal;

/*
 * 拍一页的**前像**（改页之前调用）。
 *
 * buf 至少要有 PAGE_SIZE 字节。前像必须**在改页之前**拍 ——
 * 改完再拍，拍到的是改后的内容，undo 就没意义了。
 */
void ts_wal_snap(const u8 *page, u8 *buf);

/*
 * 把一页的镜像写进日志（LOG_PAGEDATA）。
 *
 * before —— 改之前的整页内容；NULL 表示"没有前像"（记录里 blen = 0）。
 * after  —— 改之后的整页内容（PAGE_SIZE 字节，必填）。
 *
 * 成功时 *lsn_out 拿到这条记录的 LSN（写进页头做幂等判断）。
 * 失败返回非 OK —— 调用方必须**放弃这次修改**。
 */
int ts_wal_log_page(const TsWal *w, u32 page_id, const u8 *before,
                    const u8 *after, u32 *lsn_out);

/*
 * 写页的统一出口：先写日志 → 写 LSN → 归还页（unpin dirty）。
 *
 * before —— 改之前的整页内容（调用方必须在**改页之前**拍下来）。
 *
 * 日志写失败时：把页**恢复成改之前的样子**、按"不是脏页"归还，
 * 然后把错误码原样返回 —— 半改的页绝不能落盘。
 *
 * 没有日志钩子时（w == NULL 或 w->fn == NULL）：只算校验和、归还页，
 * 行为与"接 WAL 之前"完全一致（单元测试直接用模态时走这条路）。
 */
int ts_pg_commit(Pager *pg, u32 page_id, PageFrame *fr, const TsWal *w,
                 const u8 *before);

/* ---------------- 接线（db 层用） ---------------- */

/*
 * 新建时序存储并挂上写日志上下文。
 *
 * w —— 写日志上下文（NULL = 不写日志）。
 *
 * ⚠️ 目录的 meta 页是**建库时**写的 ⇒ 上下文必须在建目录**之前**挂上，
 *    否则那张 meta 页不进日志 ⇒ 崩在"建完库还没写第一条读数"时，
 *    重开库会找不到时序存储（看起来像"没建过"）。
 */
int ts_store_create_wal(Pager *pg, u32 span_sec, const TsWal *w, TsStore **out);

/* 打开磁盘上已有的时序存储并挂上写日志上下文。 */
int ts_store_open_wal(Pager *pg, const TsWal *w, TsStore **out);

/* 给目录挂上写日志上下文（建目录时用；w == NULL = 不写日志）。 */
int ts_dir_create_wal(Pager *pg, u32 span_sec, const TsWal *w, TsDir **out);

/* 换掉目录的写日志上下文（开库后补挂用；w == NULL = 不写日志）。 */
void ts_dir_set_wal(TsDir *d, const TsWal *w);

/* 取目录的写日志上下文（TTL 这类"只有目录没有存储"的层要用）。 */
const TsWal *ts_dir_wal(const TsDir *d);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_TS_TS_WAL_H */
