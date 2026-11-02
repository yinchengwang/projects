#ifndef DB_COMMON_LOGHOOK_H
#define DB_COMMON_LOGHOOK_H

#include "common/types.h"

/* "这个 slot 没有有效的页内偏移"（已删除 / 不存在 / 不涉及 slot） */
#define LOG_NO_SLOT_OFF ((u16)0xFFFFu)

/*
 * 写日志钩子（v0.6）
 *
 * 为什么放在 L0（common）：它要被三层同时用 ——
 *   storage 层用它给堆页写日志
 *   catalog 层用它给元数据页写日志
 *   db 层实现它，把调用翻译成 WAL 记录
 * 放在 storage/heap.h 的话，catalog 就得依赖 storage（虽然允许），
 * 但语义上"写日志"和"堆表"是两件事，混在一起以后加第 9 个模态会很别扭。
 *
 * 调用顺序是硬要求（WAL 规则的直接体现）：
 *   1. 调用方已经算好这次修改会影响哪个 slot
 *   2. 调钩子，把 before/after 镜像写进日志，拿到这条记录的 LSN
 *   3. 调用方改页，把 LSN 写进页头（page_lsn）
 *
 * 第 2 步和第 3 步反过来的话（先改页再写日志）：
 *   - 页可能先于日志落盘 → 崩溃后日志里缺这条记录，
 *     页上却已经有它的内容 → 数据"没根"，谁也说不清它算不算提交
 *   - 幂等判断也错：page_lsn 会晚于日志 LSN
 *
 * 钩子返回非 OK 时，调用方必须放弃这次修改（页不能改）。
 */

typedef enum {
    LOG_INSERT = 1,     /* after = 整行新内容，before 空 */
    LOG_DELETE,         /* before = 整行旧内容，after 空 */
    LOG_UPDATE,         /* before/after 都是整行，slot 不变 */
    LOG_PAGEINIT,       /* after = 页头 20 字节（新页初始化） */
    LOG_PAGEDATA        /* before/after = 整个页的镜像（4KB，元数据页用） */
} LogType;

/*
 * 返回 OK 表示日志已经写好；*lsn_out 返回该记录的 LSN（0 表示没有日志）。
 * lsn_out 是调用方写进页头做幂等判断的依据，不能是随手填的数。
 *
 * slot_off 是该 slot 在页内的字节偏移。undo 时要靠它确认
 * "这条记录还在原来的位置" —— 页被 compact 过的话记录会挪位置，
 * 光靠 slot 号回写会写坏别的 slot。
 * 不涉及具体 slot 的记录（PAGEINIT / PAGEDATA）传 PAGE_SLOT_NONE。
 *
 * 【为什么 xid 必须显式传进来（v0.7 的血泪教训）】
 *
 * 最初的签名里没有 xid —— 钩子实现自己去 WAL 对象上取 w->xid。
 * 看着挺顺，但那是**共享状态**：
 *
 *   WAL 是实例级共享的（所有连接一个日志文件）。
 *   连接 A：wal_set_xid(w, 5)
 *   连接 B：wal_set_xid(w, 6)      ← 覆盖了 A 的设置
 *   连接 A 写数据 → 日志记录的 xid 是 6，不是 5
 *
 * 后果非常严重：恢复阶段分析"哪些事务提交了"时，
 * A 的记录被算到 B 名下 —— **未提交的数据可能被判定为已提交**。
 *
 * 教训和 v0.6 的"xid 跨会话混淆"是同一个：
 *   **任何"当前是谁"的信息都不能藏在共享对象里，必须随操作显式传递。**
 *
 * 现在 xid 由调用方（堆表层）从上层拿到的当前事务号传下来。
 * 0 表示"系统事务"（bootstrap 等非用户事务的操作）。
 */
typedef int (*LogHookFn)(void *ctx, u32 xid, u8 logtype, u32 page_id, u16 slot,
                         u16 slot_off,
                         const u8 *before, u16 before_len,
                         const u8 *after,  u16 after_len,
                         u32 *lsn_out);

#endif /* DB_COMMON_LOGHOOK_H */
