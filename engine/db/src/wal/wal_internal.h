#ifndef DB_WAL_WAL_INTERNAL_H
#define DB_WAL_WAL_INTERNAL_H

/*
 * wal/wal_internal.h —— WAL 的内部结构（**只给 wal/ 这一层自己用**）
 *
 * 【为什么要把 struct Wal 搬到头文件里】
 *
 * W-D 的 D-7（group commit）要把 fsync 策略单独放进 `wal_group.c`，
 * 而 `wal.c` 已经 793 行（硬上限 800）—— 再往里加就破了。
 *
 * 拆文件的代价是"结构体与实现分居两个编译单元"，所以 struct Wal 必须
 * 有个共同可见的定义。做法与 `storage/pager_internal.h` 一致：
 * **只在 wal/ 目录内部可见**，对外仍然是 `wal.h` 里那个不透明 `typedef struct Wal Wal;`。
 *
 * ⚠️ 别把本头文件往外传：它暴露了 FILE*、互斥量与 undo 链槽位。
 *    对外契约只有 `wal.h` 那一份。
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"

/* ---------------- 平台差异（原来在 wal.c 顶部，搬过来给两个 .c 共用） ---------------- */

#ifdef _WIN32
#include <io.h>
#define WAL_FSYNC(f) _commit(_fileno(f))
#define FSEEK _fseeki64
#define FTELL _ftelli64
/* 把文件截到指定长度（Windows 用 _chsize_s，POSIX 用 ftruncate） */
#define WAL_TRUNCATE(f, len) _chsize_s(_fileno(f), (long)(len))
#else
#include <unistd.h>
#define WAL_FSYNC(f) fsync(fileno(f))
#define FSEEK fseeko
#define FTELL ftello
#define WAL_TRUNCATE(f, len) ftruncate(fileno(f), (off_t)(len))
#endif

/*
 * 每个事务的 undo 链尾。
 *
 * prev_lsn 记录"同一事务的上一条日志"，恢复阶段靠它倒序回滚。
 * 多连接下它是**按 xid 分别维护**的 —— 共享一个变量会让事务串链，
 * 回滚时会把别的事务的记录也回滚掉。
 */
#define WAL_PREV_SLOTS 64

typedef struct {
    u32 xid;
    u32 prev_lsn;
} PrevSlot;

/* wal.h 对外把它声明成不透明类型；内部头在这里补上短名。 */
typedef struct Wal Wal;

struct Wal {
    FILE *f;
    char *path;        /* wal_reset 要重开文件，得记住路径 */
    _Atomic u32 last_lsn;
    _Atomic u32 flushed_lsn; /* 已经 fsync 落盘的最大 LSN */
    _Atomic u32 nrecords;
    _Atomic u32 max_xid;
    /*
     * 日志文件当前占用的字节数（v1.2 步骤 7 / A3）。
     *
     * 【为什么自己记，而不是每次去问文件】
     *
     * 原来 wal_bytes() 是 fseek 到文件尾 + ftell —— 要动 FILE* 的位置，
     * 所以必须取域锁。而 wal_bytes() 在**每条语句结束**时都会被调一次
     * （db_checkpoint_maybe 拿它和 checkpoint 阈值比），
     * 于是**每条只读语句也要抢 WAL 的全局锁**。
     *
     * 而 COMMIT 的 fsync 是**持着这把锁**做的 ——
     * 于是每条 SELECT 都要排在别的线程的 fsync 后面。
     * 实测（bench/probe_lockstat.sh，4 线程）：
     * WAL 锁累计等待 1048 ms / 18561 次 = **56.5 us/次**，
     * 而池级锁只有 0.06 us/次 —— 瓶颈根本不在缓冲池，在 WAL。
     *
     * 记一个字节数就绕开了整件事：append 时加、truncate/reset 时重置，
     * 读的时候原子读一下就行，**一把锁都不用碰** ✓
     */
    _Atomic u64 bytes;
    long  scan_pos;
    u8   *scratch;      /* 一条完整记录的临时缓冲 */
    PrevSlot prev[WAL_PREV_SLOTS];   /* 按 xid 维护的 undo 链 */
    pthread_mutex_t mu; /* wal 域锁 */
    pthread_cond_t  cond; /* group commit：等待"这一代 fsync"结束 */

    /* ---------------- group commit 的状态（W-D 的 D-7） ---------------- */

    /*
     * 已经结束的 fsync 代数（成功与失败都算）。
     *
     * 等待者的判据是「代数变了」而不是「水位涨了」——
     * 因为一次 fsync **失败**时水位不会涨，而等待者必须被唤醒并知道失败。
     * 两者放在一起就分不清"还没轮到"和"失败了"。
     */
    u32  fsync_gen;
    bool fsync_running;   /* 有人正在执行本代 fsync（受 mu 保护） */
    bool fsync_ok;        /* 最近结束的那一代是否成功（受 mu 保护） */
    u32  fsync_failed_lsn; /* 最近失败一代覆盖到的水位；同一批等待者都必须失败 */
    u64  fsync_calls;     /* 真实 fsync 系统调用次数（诊断与测试用） */
    bool closing;         /* 库在关：等待者全部以失败返回（不许挂死、不许假成功） */

    /* ---------------- 仅测试用的故障注入（见 wal_group.c 的说明） ---------------- */
    u32  test_fail_next;    /* >0 ⇒ 接下来这么多次 fsync 强制失败 */
    u32  test_slow_us;      /* >0 ⇒ 每次 fsync 前睡这么多微秒 */
};

/* 取/放 wal 域锁（附带 latch 框架记账）。定义在 wal.c。 */
void wal_lock(Wal *w);
void wal_unlock(Wal *w);

/*
 * group commit 的实现（wal_group.c）。
 *
 * 声明在这里是因为 wal.c 的 wal_open / wal_close 要建/销毁等待队列，
 * 而 wal_fsync 要调 wal_group_durable —— 具体协议在 group 文件里。
 *
 * ⚠️ 没有"唤醒全部"的公开入口：wal_close 里直接 broadcast，
 * 因为那时 `closing` 已经置位，广播的语义就是"全部以失败返回"，
 * 不需要额外一层。
 */
void wal_group_init(Wal *w);
void wal_group_destroy(Wal *w);
int  wal_group_durable(Wal *w, u32 target);

/*
 * ---------------- 仅测试用的故障注入 ----------------
 *
 * ⚠️ **生产代码不许调用**。存在的理由只有一条：
 * "失败的 fsync 不能让事务假成功"这条判据，如果不注入失败就**没法测**。
 *
 * 不用"把日志目录设成只读"那种做法：Windows 上的只读语义与 POSIX 不同，
 * 而且会污染测试环境的清理路径。
 */
void wal_test_fail_fsync(Wal *w, u32 n);
void wal_test_slow_fsync(Wal *w, u32 us);
u64  wal_group_fsync_calls(const Wal *w);

#endif /* DB_WAL_WAL_INTERNAL_H */
