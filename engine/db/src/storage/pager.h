#ifndef DB_STORAGE_PAGER_H
#define DB_STORAGE_PAGER_H

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"

typedef struct Pager Pager;

/*
 * 一帧 = 一页的内存镜像。返回的 frame 指针在 unpin 之前一直有效。
 *
 * 注意：data 是"物理页"，pager 不解释页内结构——
 * header / slot / 记录怎么摆，是 page.c 的职责。分层就在这里。
 */
typedef struct PageFrame {
    u8   data[PAGE_SIZE];
    u32  page_id;     /* PAGE_ID_NIL = 空帧 */
    int  pin_count;   /* >0 表示有人正拿着指针，不许逐出 */
    bool dirty;
    int  clock_ref;   /* Clock 算法的引用位 */
} PageFrame;

int pager_open(const char *path, u32 nframes, Pager **out);
int pager_close(Pager *pg);

/*
 * 取页。返回的 frame 已 pin，用完必须 unpin。
 *   page_id 越界      -> E_NOTFOUND
 *   池子被 pin 死     -> E_NOSPACE（绝不死循环，也绝不偷偷丢页）
 */
int pager_get(Pager *pg, u32 page_id, PageFrame **frame);

/* 新分配一页（文件尾），返回已 pin、已清零、标记 dirty 的 frame。 */
int pager_alloc(Pager *pg, u32 *page_id, PageFrame **frame);

/*
 * 归还一页。dirty = 本次是否改过内容。
 * 多退一次返回 E_INVAL —— 这意味着 pin/unpin 不配对，是 bug，
 * 越早发现越好。
 */
int pager_unpin(Pager *pg, u32 page_id, bool dirty);

/* 刷所有脏页。pager_close 会自动调。 */
int pager_flush_all(Pager *pg);

u64 pager_npages(const Pager *pg);

/*
 * 把逻辑页数扩到至少 npages。恢复时用：日志里出现的页号
 * 可能大于当前文件大小（分配了但没来得及刷盘）。
 */
int pager_extend_to(Pager *pg, u32 npages);

/*
 * 丢掉这个 pager：**不刷脏页**、不 fsync，直接关文件。
 * 只给"模拟断电"的测试用 —— 正常关闭请用 pager_close。
 */
void pager_abandon(Pager *pg);
u32 pager_frames_used(const Pager *pg);   /* 测试用：当前占用帧数 */

/*
 * WAL 规则钩子：**保证日志在页之前落盘**。
 *
 * WAL（Write-Ahead Logging）的铁律：
 *   "页刷盘前，对应的日志必须已落盘"
 *
 * 违反的后果是静默的数据丢失：
 *   页带着"还没落盘的日志"写进了数据文件 → 崩溃后日志里缺这段内容
 *   → 重放时无从恢复 → 这段数据就这么没了，而且不报任何错。
 *
 * 怎么执行：pager 每刷一个脏页之前，先问钩子
 * "page_lsn 对应的日志落盘了吗"。没落盘的话，钩子负责把它刷下去
 * （db 层的实现就是 wal_fsync）。
 *
 * 不传钩子 = 不检查（测试和早期开发用）。
 * 钩子返回非 OK 时 pager 会放弃这次刷盘并报错。
 */
typedef int (*PagerFlushLogFn)(void *ctx, u32 page_id, u32 page_lsn);

void pager_set_wal_flush(Pager *pg, PagerFlushLogFn fn, void *ctx);

#endif /* DB_STORAGE_PAGER_H */
