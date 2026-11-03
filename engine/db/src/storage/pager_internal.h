#ifndef DB_STORAGE_PAGER_INTERNAL_H
#define DB_STORAGE_PAGER_INTERNAL_H

/*
 * pager_internal.h — pager 内部的跨文件声明
 *
 * ==================== 为什么有这个头 ====================
 *
 * v1.2 步骤 7 之后 pager.c 有 1002 行，超过"单文件不超过 800 行"的硬规则。
 * 按职责拆成两个 .c：
 *
 *   pager.c        —— 缓冲池的公共 API、页生命周期、文件读写、池级锁包装
 *   pager_frame.c  —— 帧哈希表、Clock-Sweep 逐出、帧内容锁、取帧（命中/逐出/载入）
 *
 * 拆分是**纯搬移**：锁的语义、不变量、调用顺序一个字节都没改。
 *
 * ==================== 这个头里放什么 ====================
 *
 * 只放**真的跨文件共享**的东西：
 *   - struct Pager 的定义（两个 .c 都要看它的字段）
 *   - 池级锁的六个包装（pager.c 定义、两边都用）
 *   - 帧内容锁的两个原语（pager_frame.c 定义、两边都用）
 *   - 帧哈希表的两个入口（pager_frame.c 定义、两边都用）
 *   - 页的读/刷（pager.c 定义，pager_frame.c 用）
 *   - 取帧（pager_frame.c 定义，pager.c 用）
 *
 * 只被单个文件用的辅助函数一律留在那个文件里 static 化。
 *
 * 【注意】这里的东西**不是公开 API**：
 * 公开的接口一个字节都没动，仍然只有 storage/pager.h 里的那些。
 * 这些内部符号都带 pager_ 前缀，避免和别的模块撞名。
 */

#include "storage/pager.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#include "common/rwlock.h"

struct Pager {
    FILE       *f;
    /*
     * npages：页数 = 文件大小 / PAGE_SIZE。
     *
     * 【为什么是原子量】（v1.2 步骤 7 / A3）
     *
     * 它只被两处碰：pager_alloc 分配新页时加一、pager_get 判越界时读。
     * 这两处原本都要抢池级锁 —— 而"判越界"在**每一次页访问**上都要做，
     * 属于纯读。改成原子量之后，读路径上这一处**完全不碰锁** ✓。
     */
    atomic_uint_least64_t npages;
    u32         nframes;
    u32         hand;        /* Clock 手 */
    PageFrame  *frames;
    /*
     * 池级锁：哈希表 / Clock 手 / pin_count / npages。
     *
     * 【为什么是读写锁而不是互斥量】（v1.2 步骤 7 / A3 的根因）
     *
     * 这把锁保护的是"整个池子共用的账本"，访问方式**读多写少**：
     *   - 读：哈希查找（每一次页访问都要做一次）
     *   - 写：逐出（摘映射 / 载页）、分配新页（极少）
     *
     * 互斥量会把**所有页访问**串成一条线：
     * v1.2 步骤 7 的 A3 实测 0.78x–1.01x，gdb 采样里
     * 4 个工作线程有 3 个堵在 pool_lock —— 就是这一把锁。
     *
     * 换成读写锁之后，**读读不互斥**：
     * 命中路径上的哈希查找可以并发做，逐出（写）才独占。
     *
     * 【为什么是自研的 db_rwlock 而不是 pthread_rwlock】
     *
     * winpthreads 的 pthread_rwlock **不允许读者并发**（实测 4 线程
     * 比 1 线程还慢 5 倍，见 src/common/rwlock.h）——
     * 用它当"读锁"等于换了个名字的互斥量。
     * 自研的 db_rwlock 读者是真并发：4 线程 29 M 次/秒 vs
     * pthread_rwlock 的 3.2 M 次/秒。
     */
    DbRwLock mu;
    pthread_mutex_t *latches; /* 每帧一把内容锁（递归） */
    pthread_t  *latch_owner; /* 内容锁的持有线程（用来发现"不是自己拿的锁"） */
    int         *latch_depth;/* 内容锁的重入深度 */
    u32        *hash_keys;   /* page_id -> frame 下标 */
    u32        *hash_vals;
    u32         hash_cap;    /* 2 的幂 */
    u32         hash_tombs;  /* 墓碑数。会无限累积，
                                不回收的话探测序列总有一天遇不到 EMPTY，
                                hash_find 就死循环了 —— 写 1 万页必触发 */
    PagerFlushLogFn wal_flush;   /* WAL 规则：刷页前保证日志落盘 */
    void          *wal_ctx;
};

/* ---------------- 池级锁（pager.c 实现，两边共用） ---------------- */

/*
 * 池级锁 —— frame 域（序号 3）。
 *
 * 它保护的是"整个池子共用的账本"，所以它和内容锁同属一个域。
 *
 * 【读写锁的三种拿法】
 *
 *   pager_pool_rdlock   —— 只读账本（哈希查找）。**读读不互斥**，
 *                         多个线程可以同时查哈希表 ✓
 *   pager_pool_wrlock   —— 改账本（逐出 / 分配）。独占
 *   pager_pool_lock     —— 兼容旧名字，等价于 pager_pool_wrlock
 *
 * 域的记账（latch_order_acquire）两种拿法都要做 ——
 * 它记的是"本线程持有哪些域"，与锁本身的共享/独占无关。
 */
void pager_pool_rdlock(Pager *pg);
void pager_pool_rdunlock(Pager *pg);
void pager_pool_wrlock(Pager *pg);
void pager_pool_wrunlock(Pager *pg);
void pager_pool_lock(Pager *pg);      /* 旧名保留：等价于写锁 */
void pager_pool_unlock(Pager *pg);    /* 旧名保留：等价于写锁 */

/* ---------------- 帧内容锁（pager_frame.c 实现，两边共用） ---------------- */

/*
 * 内容锁 —— frame 域（序号 3），**递归**。
 *
 * 递归的原因：同一线程可能对同一页 get 两次。
 * 不可重入的锁会自己把自己锁死。
 *
 * depth[] 既是重入计数，也是"这把锁有没有人拿着"的标志：
 * depth == 0 ⇒ 无人持有（因此 owner 字段此刻无意义）。
 *
 * owner[] 记的是**持有者线程** ——
 * 用途是发现"释放了一把不是自己拿的锁"（调用方的 bug）。
 */
void pager_frame_lock(Pager *pg, u32 fi);
int  pager_frame_unlock(Pager *pg, u32 fi);

/* ---------------- 帧哈希表（pager_frame.c 实现，两边共用） ---------------- */

/* 建哈希表（装填因子 <= 0.5） */
void pager_hash_init(Pager *pg);

/* 查表：找到返回 true 并写出帧下标；没找到返回 false */
bool pager_hash_find(Pager *pg, u32 key, u32 *val);

/* ---------------- 页读写（pager.c 实现，pager_frame.c 用） ---------------- */

/* 把一页读进 buf（读到 EOF 视为全零页） */
int pager_read_page(Pager *pg, u32 page_id, u8 *buf);

/* 刷一个脏页（先问 WAL 钩子，日志没落盘就不许刷） */
int pager_flush_page(Pager *pg, u32 page_id, const u8 *buf);

/* ---------------- 取帧（pager_frame.c 实现，pager.c 用） ---------------- */

/*
 * 取一页并 pin 住（内容锁也一并拿到）。
 *
 * 返回时：池级锁已释放，**内容锁由调用方持有**，
 * 一直到 pager_unpin 才释放。
 */
int pager_fetch_frame(Pager *pg, u32 page_id, PageFrame **out);

#endif /* DB_STORAGE_PAGER_INTERNAL_H */
