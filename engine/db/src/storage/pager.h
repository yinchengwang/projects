#ifndef DB_STORAGE_PAGER_H
#define DB_STORAGE_PAGER_H

#include <stdatomic.h>

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
    /*
     * pin_count：>0 表示有人正拿着指针，不许逐出。
     *
     * 【为什么是原子量】（v1.2 步骤 7 / A3）
     *
     * 读路径上它要被**读多写少**地更新：每次 pager_get 加一、
     * 每次 pager_unpin 减一。以前它被池级互斥量保护，于是
     * "加一"和"减一"都要抢那把全局锁 —— 一次页访问要抢 4 次。
     *
     * 它本身是个纯粹的计数器：加一减一之间不需要跟别的数据结构
     * 保持一致，只要"最终算准"就行。改成原子量之后，
     * unpin 路径上的减一**不再需要池级锁** ✓。
     *
     * 不变式「持有内容锁 ⇒ pin_count ≥ 1」依然成立 ——
     * 见 pager.c 里的说明。
     */
    atomic_int pin_count;
    bool dirty;
    /*
     * clock_ref：Clock 算法的引用位。
     * 命中时置 1（读路径上做），逐出时清零（写路径上做）→ 原子量。
     */
    atomic_int clock_ref;

    /*
     * rec_lsn：**自上次刷盘以来，这一页第一次被改动时**的日志 LSN。
     *
     * 【为什么需要它】
     * 页头的 H_LSN 记的是"最后一次改动"的 LSN —— 刷新盘之后它就变成
     * "最后一次落盘的改动的 LSN"，中间那些改动就看不出来了。
     *
     * 而 checkpoint 需要知道"哪些记录还没落盘"：
     *   - 页是脏的 → 它上面的改动都只在内存里
     *   - 第一次改动（rec_lsn）之后的记录都还没落盘 → 恢复时必须重放
     *
     * 这就是 ARIES 的 dirty page table 的最小形式。
     * 没有它，checkpoint 就只能"把所有记录都重放一遍"（不截断），
     * 或者靠"全局停写"来保证安全（那要加一把全局写锁）。
     *
     * 0 = 未知（这一页被改了但没写日志 —— 那是 bug，
     * checkpoint 会因此拒绝推进截断点）。
     */
    u32  rec_lsn;
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

/*
 * 取脏页里最小的 rec_lsn（checkpoint 的重放起点）。
 *
 * 返回 0 表示"没有能约束重放点的脏页"。
 *
 * 【rec_lsn == 0 的页被跳过】
 * 有一类页是**故意不写日志**的：向量索引（checkpoint 式的，
 * 过期了重建就行，见 db.c 里 db_vec_open 的注释）。
 * 它们的 rec_lsn 恒为 0。
 *
 * 跳过它们是安全的：日志里根本没有它们的记录，
 * 所以它们不约束"哪些记录还没落盘"。
 *
 * 反过来，如果某个**本该写日志**的页 rec_lsn 也是 0，
 * 那说明写它的时候漏了 page_set_lsn —— 那是 bug。
 * 这个 bug 的后果是"那一页的重做信息丢了"，
 * 但它不是 checkpoint 能补救的（重做也救不回来），
 * 所以这里不为此停下整个 checkpoint。
 */
u32 pager_min_dirty_lsn(const Pager *pg);

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
