/*
 * heap_internal.h — 堆表 AM 的内部共享定义（**私有头**，不对外暴露）
 *
 * heap.c 原来是一个 1081 行的巨文件。把它拆成
 *     heap.c        —— 生命周期 + 写路径 + 观察
 *     heap_scan.c   —— 扫描（逐行 / 零拷贝批）
 *     heap_compact.c—— 页内 compact
 * 之后，跨文件共享的东西集中放在这里：
 *
 *   - struct Heap / struct HeapScan 的完整定义
 *   - WAL 钩子封装（wal_log）
 *   - 本表页号表（heap_plist_add / heap_plist_snapshot）
 *   - 扫描器的放页辅助（heap_scan_release）
 *
 * 【为什么不塞进 heap.h】
 * heap.h 是**公开 API**，对外承诺"只有这些函数、这些类型"。
 * 堆表实例的内部布局是实现细节 —— 暴露出去就等于把
 * "每表页号表"这种实现变成公开契约，以后想换实现就被公开头卡住。
 *
 * 四条纪律（沿用自 heap.c）：
 *   1. 每次修改页之前先调 WAL 钩子，拿到 LSN 写进页头。
 *   2. 任何时刻最多 pin 一页。扫描器每行 get/unpin 一次。
 *   3. 返回值一律拷贝到调用方缓冲区，不返回页内指针。
 *   4. 不依赖页链：扫描走「本表页号表」（见下），崩溃恢复不会丢页。
 */

#ifndef DB_STORAGE_HEAP_INTERNAL_H
#define DB_STORAGE_HEAP_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>

#include "storage/heap.h"

struct Heap {
    Pager    *pager;
    FreeList *fl;
    u32       root;

    /*
     * 本表的页数。
     *
     * 【为什么是原子量】
     *
     * Heap 是**实例级共享**的（catalog 里每张表一个，所有连接共用）。
     * 追加新页时 `npages++` 会被多个线程同时做 ——
     * 不加保护就是数据竞争（UB），加普通锁又太重。
     *
     * 这个计数只用于观察（heap_npages），所以用 C11 原子量最省事 ✓
     */
    _Atomic u32 npages;

    /*
     * 本表的页号表（只追加，不重排）。
     *
     * 【为什么需要它 —— v1.2 步骤 7 A3 的根因】
     *
     * v0.6 起扫描是"按页号区间走，靠页头的 table_id 过滤"：
     *     for (pid = 0; pid < pager_npages(); pid++) { ... }
     * 也就是说**每张表的每次扫描都要把整个数据库的页空间走一遍**。
     *
     * 单表小库上这只是"慢一点"；但多表并发时它是致命的：
     * 4 个线程各扫自己的表，物理上却都在 touch 全部 4 张表的页
     * → 页的内容锁（frame 域）退化成全局串行点。
     * A3 基准实测到 12713 次真实内容锁冲突，根因就在这里。
     *
     * 【为什么不改回页链】
     *
     * 页链的更新（tail->next）本身是一次页修改，不写日志的话
     * 崩溃恢复后新页就成了孤儿 —— 整页数据"消失"。
     * 这里维护的页号表**只在内存里**，打开时由 heap_open 扫页重建，
     * 不进 WAL，不影响崩溃恢复的正确性 ✓
     */
    pthread_mutex_t plist_mu;
    u32          *plist;      /* 页号数组 */
    u32           plist_n;    /* 有效个数 */
    u32           plist_cap;  /* 数组容量 */

    u16       tid;
    HeapWalFn wal;
    void     *wal_ctx;

    /* WAL 调用次数（同上：实例级共享的计数器） */
    _Atomic u64 wal_calls;
};

struct HeapScan {
    Heap *h;
    u32   page_id;      /* 当前页号（页号表里的值） */
    u16   next_slot;
    u32   idx;          /* 页号表下标 */

    /*
     * 页号表快照。
     *
     * 扫描开始时拍一份拷贝，之后并发追加新页不会影响本次扫描
     * —— 语义与老代码"扫描开始时固定 max_page"一致 ✓
     */
    u32  *pages;
    u32   npages;

    /*
     * 当前 pin 着的页。
     *
     * 【为什么扫一页只 get 一次】
     * pager_get 和 pager_unpin **各自都要做一次哈希查找**
     * （get 里查一次定位帧，unpin 里又查一次）。原来每行都 get+unpin 一轮，
     * 10 万行就是 20 万次哈希查找 —— 实测占了扫描阶段的绝大部分。
     *
     * 一页能装约 100 行，所以 pin 住整页、扫完再放开，
     * 哈希查找就从"每行 2 次"降到"每页 1 次"。
     *
     * 安全性：pin 住的页不会被逐出（pick_victim 只挑 pin_count == 0 的帧），
     * 也不会被换出缓冲池。扫描结束（heap_scan_end）一定会放开 ✓
     */
    PageFrame *cur;
};

/* ---------------- WAL 钩子 ---------------- */

/*
 * 写一条日志。
 *
 * slot_off 是这个 slot 在页内的字节偏移 —— undo 时要靠它确认
 * "记录还在原来的位置"，否则回写会写坏别的 slot。
 *
 * xid 由调用方显式传进来（见 loghook.h 里关于"共享状态"的说明）。
 */
int heap_wal_log(Heap *h, u32 xid, u8 logtype, u32 page_id, u16 slot,
                 u16 slot_off,
                 const u8 *before, u16 blen,
                 const u8 *after, u16 alen, u32 *lsn_out);

/* ---------------- 本表页号表 ---------------- */

/*
 * 追加一个页号到本表的页号表。
 *
 * 失败（内存不足）返回 E_IO —— 宁可让调用方报错，
 * 也不能"页存在但扫不到"（那等于静默丢数据）。
 */
int heap_plist_add(Heap *h, u32 pid);

/*
 * 拍一份页号表快照。
 *
 * 拷贝一份而不是直接读共享数组：扫描期间别的线程会追加新页
 * （append_page），共享数组还可能 realloc 换指针 —— 直接读会读到半个数组。
 */
int heap_plist_snapshot(Heap *h, u32 **out, u32 *n_out);

/* ---------------- 扫描辅助 ---------------- */

/* 放开当前 pin 着的页（如果没有） */
void heap_scan_release(HeapScan *s);

#endif /* DB_STORAGE_HEAP_INTERNAL_H */
