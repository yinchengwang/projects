#ifndef DB_STORAGE_PAGE_H
#define DB_STORAGE_PAGE_H

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"

/*
 * slotted page —— 页内变长记录的容器。
 *
 * 所有函数都在 u8[PAGE_SIZE] 上操作，不碰文件、不碰缓冲池。
 * 页从 pager 拿到，改完 unpin 时标脏，这一层不关心。
 *
 * 两条铁律：
 *   1. slot 编号永不复用。删除只把 length 置 0，编号永久保留。
 *      索引存的是 (page_id, slot)，复用 slot 会让索引指向无关记录，
 *      不报错、只返回错数据。
 *   2. compact 必须从后往前压。正序搬运会覆盖还没搬的记录。
 */

int page_init(u8 *pg, u8 page_type);

/*
 * 插入记录。成功时 *slot_out 得到 slot 编号。
 * 空间不足返回 E_NOSPACE —— 注意此时页可能已经 compact 过，
 * 内容有变化但语义无损。
 */
int page_insert(u8 *pg, const u8 *rec, u16 len, u16 *slot_out);

/*
 * 插入到**指定的 slot**。恢复重放专用。
 *
 * 为什么不能直接用 page_insert：它总是分配"下一个"slot，
 * 而 undo 阶段要按日志里记的 slot 撤销。
 * 如果重放时算出来的 slot 和日志里记的不一致（页状态和预期有偏差），
 * 撤销就会删错行 —— 表现是"数据莫名其妙少了一条"。
 *
 * 所以：重放必须用日志里的 slot 号，原地填回去。
 */
int page_insert_at(u8 *pg, u16 slot, const u8 *rec, u16 len);

/*
 * 读取记录。*rec 指向页内，调用方不得修改；
 * 页被逐出缓冲池后指针失效。
 */
int page_get(const u8 *pg, u16 slot, const u8 **rec, u16 *len);

/*
 * 原地改写。要求 len <= 原长度（变长更新请走 delete + insert，RID 会变）。
 * 缩短后留下的空洞计入碎片，由 compact 回收。
 */
int page_update(u8 *pg, u16 slot, const u8 *rec, u16 len);

/* 删除记录。slot 编号永久保留（见铁律 1）。 */
int page_delete(u8 *pg, u16 slot);

/*
 * 碎片整理：把活记录压到页尾，消除空洞。
 * slot 编号与偏移之外的一切不变，RID 因此保持稳定。
 */
void page_compact(u8 *pg);

/* ---- 统计 ---- */
u16  page_free_bytes(const u8 *pg);     /* 可用连续空间 */
u16  page_used_bytes(const u8 *pg);     /* 记录区已占字节（含空洞） */
u16  page_live_bytes(const u8 *pg);     /* 活记录总长 */
u16  page_fragmentation(const u8 *pg);  /* (used-live)/used，百分比 0..100 */
u16  page_slot_count(const u8 *pg);     /* 已分配 slot 数（含空洞） */
bool page_slot_alive(const u8 *pg, u16 slot);

/* ---- 叶子链：heap 页链、overflow 链共用 ---- */
u32  page_next(const u8 *pg);
void page_set_next(u8 *pg, u32 next_page);

/* ---- 页归属（heap 扫描靠它区分表，不再依赖页链）---- */
u16  page_table_id(const u8 *pg);
void page_set_table_id(u8 *pg, u16 tid);

/* ---- WAL 用：页的 LSN ---- */
u32  page_lsn(const u8 *pg);
void page_set_lsn(u8 *pg, u32 lsn);

/*
 * 取某个 slot 在页内的字节偏移。WAL 记录里要存它，
 * 因为 undo 时要判断"这条记录有没有被 compact 挪过位置"。
 * slot 不存在或已删除返回 PAGE_SLOT_NONE。
 */
u16  page_slot_offset(const u8 *pg, u16 slot);

#define PAGE_SLOT_NONE ((u16)0xFFFFu)

/*
 * 把一条记录写回**指定 slot**。undo 专用：
 * page_insert 只会追加，而撤销删除必须在原 slot 恢复，
 * 否则索引里存的 RID 就指向别处了。
 *
 * slot < slot_count：该 slot 必须是空的（已删除状态），就地填回
 * slot == slot_count：等价于追加
 */
int page_restore(u8 *pg, u16 slot, const u8 *rec, u16 len);

/*
 * 把一条记录写回**指定 slot 的指定偏移**。
 *
 * 为什么需要偏移这个参数：undo 的是一次原地 UPDATE 时，
 * 记录可能因为之后的 compact 挪了位置。带着原偏移回来，
 * 才能确认"这条记录确实还在它原来待着的地方"，
 * 而不是把内容写到一个恰好空着的、但属于别的 slot 的位置。
 *
 * 如果偏移对不上（记录挪走了或已被删除），返回 E_NOTFOUND，
 * 恢复层据此放弃这次 undo —— 因为此时页的状态已经无法通过
 * 单条日志记录还原了。
 */
int page_restore_at(u8 *pg, u16 slot, u16 expect_off, const u8 *rec, u16 len);

/* ---- 校验 ---- */
void page_set_checksum(u8 *pg);
bool page_verify_checksum(const u8 *pg);

#endif /* DB_STORAGE_PAGE_H */
