#include "wal/wal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/crc.h"
#include "common/latch.h"
#include "common/lockstat.h"
#include "wal/wal_hdr.h"
#include "wal/wal_internal.h"
/*
 * 取域锁 + 向 latch 框架记账。
 *
 * 记账让"catalog → txn → wal → freelist → frame"这条全序
 * 从步骤 2 起就是**机器检查**的。
 *
 * 【为什么不再是 static】
 *
 * W-D 的 D-7 把 group commit 的逻辑拆到了 wal_group.c（wal.c 已经 793 行，
 * 硬上限 800）。那一层也要取同一把域锁 —— 锁的定义留在本文件（它依赖
 * 本文件里的 latch 记账），只是可见性放开到 wal/ 目录内部（见 wal_internal.h）。
 */
void wal_lock(Wal *w)
{
    LS_ENTER(LS_WAL);
#ifdef DB_LOCKSTAT
    if (pthread_mutex_trylock(&w->mu) != 0) {
        atomic_fetch_add(&g_ls_cont[LS_WAL], 1ull);
        pthread_mutex_lock(&w->mu);
    }
#else
    pthread_mutex_lock(&w->mu);
#endif
    LS_LEAVE(LS_WAL);
    latch_order_acquire(LATCH_WAL);
}

void wal_unlock(Wal *w)
{
    latch_order_release(LATCH_WAL);
    pthread_mutex_unlock(&w->mu);
}

/* 内部版：调用方必须已持有 wal 域锁 */
static int  wal_scan_begin_locked(Wal *w);
static int  wal_scan_next_locked(Wal *w, WalRec *r);
static void wal_scan_end_locked(Wal *w);

/* 取某事务当前的 undo 链尾（没有就是 0） */
static u32 wal_prev_lsn_of(const Wal *w, u32 xid)
{
    for (int i = 0; i < WAL_PREV_SLOTS; i++) {
        if (w->prev[i].xid == xid)
            return w->prev[i].prev_lsn;
    }
    return 0;
}

/* 更新某事务的 undo 链尾 */
static void wal_set_prev_lsn(Wal *w, u32 xid, u32 lsn)
{
    for (int i = 0; i < WAL_PREV_SLOTS; i++) {
        if (w->prev[i].xid == xid) {
            w->prev[i].prev_lsn = lsn;
            return;
        }
    }
    /* 空槽就用它；满了就复用第一个（活跃事务数不会超过这个量）*/
    for (int i = 0; i < WAL_PREV_SLOTS; i++) {
        if (w->prev[i].xid == 0) {
            w->prev[i].xid = xid;
            w->prev[i].prev_lsn = lsn;
            return;
        }
    }
    w->prev[0].xid = xid;
    w->prev[0].prev_lsn = lsn;
}

int wal_open(const char *path, Wal **out)
{
    if (!path || !out)
        return E_INVAL;

    FILE *f = fopen(path, "r+b");
    if (!f) {
        f = fopen(path, "wb");
        if (!f)
            return E_IO;
        fclose(f);
        f = fopen(path, "r+b");
        if (!f)
            return E_IO;
    }
    setvbuf(f, NULL, _IONBF, 0);

    Wal *w = calloc(1, sizeof(*w));
    if (!w) {
        fclose(f);
        return E_IO;
    }
    w->f = f;
    w->path = malloc(strlen(path) + 1);
    if (!w->path) {
        fclose(f);
        free(w);
        return E_IO;
    }
    strcpy(w->path, path);

    /* ---------- 文件头校验 / 补写（v1.3 欠账 6 / W-D 的 D-1） ---------- */
    {
        u64 sz = 0;
        int rc = wal_hdr_load(f, &sz);

        if (rc != OK) {
            fclose(f);
            free(w->path);
            free(w);
            return rc;
        }
        /*
         * 字节数只算**记录区**（不含文件头）。
         *
         * 为什么不含：wal_bytes() 是「还在日志里的记录占多少字节」，
         * 与 checkpoint 阈值比。把头算进去会让阈值永远多 32 字节，
         * 而且每 reset 一次就漂一次 —— 口径不一致导致判据不可复现。
         */
        atomic_store(&w->bytes, (sz > WAL_FILE_HDR_SIZE)
                                ? (sz - WAL_FILE_HDR_SIZE) : 0u);
    }

    w->scratch = malloc(WAL_HDR + WAL_MAX_PAYLOAD + 4);
    if (!w->scratch) {
        fclose(f);
        free(w->path);
        free(w);
        return E_IO;
    }
    if (pthread_mutex_init(&w->mu, NULL) != 0) {
        fclose(f);
        free(w->path);
        free(w->scratch);
        free(w);
        return E_IO;
    }

    /* group commit 的等待队列（W-D 的 D-7） */
    wal_group_init(w);

    /*
     * 扫一遍确定 last_lsn、记录数、出现过的最大 xid。
     *
     * 这一步用的是**内部版**扫描函数（不取域锁）：
     * 对象刚建好，还没交给任何人，此刻是单线程的。
     */
    WalRec r;
    wal_scan_begin_locked(w);
    while (wal_scan_next_locked(w, &r) == OK) {
        if (r.lsn > atomic_load(&w->last_lsn))
            atomic_store(&w->last_lsn, r.lsn);
        if (r.xid > atomic_load(&w->max_xid))
            atomic_store(&w->max_xid, r.xid);
        atomic_store(&w->nrecords, atomic_load(&w->nrecords) + 1);
    }
    wal_scan_end_locked(w);

    /* 日志文件里已有的内容本来就是落盘的 */
    atomic_store(&w->flushed_lsn, atomic_load(&w->last_lsn));

    *out = w;
    return OK;
}

int wal_reset(Wal *w)
{
    if (!w || !w->f || !w->path)
        return E_INVAL;

    wal_lock(w);

    /* 关掉再以 "wb" 重开 —— 截断到 0 字节，日志回到"刚创建"的状态 */
    if (fclose(w->f) != 0) {
        w->f = NULL;
        wal_unlock(w);
        return E_IO;
    }
    w->f = fopen(w->path, "wb");
    if (!w->f) {
        wal_unlock(w);
        return E_IO;
    }
    setvbuf(w->f, NULL, _IONBF, 0);

    /*
     * ⚠️ "wb" 会把**文件头一起截掉**，所以必须立刻补写一个。
     * 漏了这一步 = 下次开库读到「截断的头」⇒ 自己把自己锁死。
     */
    if (wal_hdr_write(w->f) != OK) {
        wal_unlock(w);
        return E_IO;
    }

    atomic_store(&w->last_lsn, 0);
    atomic_store(&w->flushed_lsn, 0);
    atomic_store(&w->nrecords, 0);
    atomic_store(&w->max_xid, 0);
    atomic_store(&w->bytes, 0);
    w->scan_pos = (long)WAL_FILE_HDR_SIZE;
    memset(w->prev, 0, sizeof(w->prev));
    wal_unlock(w);
    return OK;
}

void wal_close(Wal *w)
{
    if (!w)
        return;

    /*
     * 关库前先把等待者喊醒（W-D 的 D-7）。
     *
     * 【为什么必须喊醒，而不是让它们继续睡】
     *
     * 等待者等的是"本代 fsync 结束"。关库之后再也不会有哪一代结束 ——
     * 不唤醒就是**永久挂死**。而且它们醒来必须拿到**失败**：
     * 日志文件马上就关了，它们的数据并没有落盘，当成功就是假成功。
     */
    wal_lock(w);
    w->closing = true;
    pthread_cond_broadcast(&w->cond);
    if (w->f) {
        WAL_FSYNC(w->f);
        fclose(w->f);
        w->f = NULL;
    }
    wal_unlock(w);

    wal_group_destroy(w);
    pthread_mutex_destroy(&w->mu);
    free(w->path);
    free(w->scratch);
    free(w);
}

/*
 * 读路径：原子读，不取域锁。
 *
 * 这几个接口在热路径上被频繁调用（每条语句结束都要查 flushed_lsn），
 * 取域锁会把语句级吞吐拖下去。它们读的都是**单个 32 位值**，
 * 原子读就够了 —— 不需要和其它状态保持一致。
 */
u32 wal_last_lsn(const Wal *w) { return w ? atomic_load(&w->last_lsn) : 0; }
u32 wal_nrecords(const Wal *w) { return w ? atomic_load(&w->nrecords) : 0; }
u32 wal_max_xid(const Wal *w)  { return w ? atomic_load(&w->max_xid) : 0; }
u32 wal_flushed_lsn(const Wal *w) { return w ? atomic_load(&w->flushed_lsn) : 0; }

/*
 * 追加一条记录，返回它的 LSN。
 *
 * 【为什么 xid 必须显式传】
 *
 * 早期版本靠 wal_set_xid(w, xid) 先把 xid 写进 WAL 对象再调用 append。
 * 那是**共享状态** —— WAL 是实例级共享的（所有连接一个日志文件）：
 *
 *   连接 A：wal_set_xid(w, 5)
 *   连接 B：wal_set_xid(w, 6)      ← 覆盖了 A 的设置
 *   连接 A 写数据 → 日志记录的 xid 是 6，不是 5
 *
 * 后果非常严重：恢复阶段分析"哪些事务提交了"时，
 * A 的记录被算到 B 名下 —— **未提交的数据可能被判定为已提交**。
 * 这就是 v0.6 里踩过的"xid 跨会话混淆"的翻版，只是换成了"跨连接"。
 *
 * 所以 xid 必须随每次调用显式传下去，不能存在共享对象里。
 *
 * 【prev_lsn 也有同样的问题】
 *
 * prev_lsn 是"同一事务的上一条记录"，用来串 undo 链。
 * 它也是 WAL 上的共享状态 —— 多连接下会串。
 *
 * v0.7 的处理：prev_lsn 按 xid 分别维护（一个小表）。
 * 这样每个事务的 undo 链是独立的。
 *
 * xid = 0 表示"系统事务"（bootstrap 阶段、操作元数据页）。
 * 恢复时永远视为已提交。
 */
static u32 wal_append_locked(Wal *w, u8 type, u32 xid, u32 page_id, u16 slot,
                             u16 slot_off,
                             const u8 *before, u16 blen, const u8 *after, u16 alen)
{
    u32 lsn = atomic_load(&w->last_lsn) + 1;
    u8 *b = w->scratch;

    put_u32(b + 0, lsn);
    put_u32(b + 4, wal_prev_lsn_of(w, xid));
    put_u32(b + 8, xid);
    b[12] = type;
    b[13] = b[14] = b[15] = 0;
    put_u32(b + 16, page_id);
    put_u16(b + 20, slot);
    put_u16(b + 22, blen);
    put_u16(b + 24, alen);
    put_u16(b + 26, slot_off);

    if (blen)
        memcpy(b + WAL_HDR, before, blen);
    if (alen)
        memcpy(b + WAL_HDR + blen, after, alen);

    u32 n = WAL_HDR + blen + alen;
    put_u32(b + n, crc32(b, n));

    if (FSEEK(w->f, 0, SEEK_END) != 0)
        return 0;
    if (fwrite(b, 1, (size_t)n + 4, w->f) != (size_t)n + 4)
        return 0;

    /*
     * 发布顺序：**先写文件，后发布 LSN**。
     *
     * 反过来的话，读者会看到一个"已经分配但没落盘"的 LSN ——
     * 页刷盘时按 page_lsn <= flushed_lsn 判断，会以为日志已经落盘了。
     */
    atomic_store(&w->last_lsn, lsn);
    atomic_store(&w->nrecords, atomic_load(&w->nrecords) + 1);
    /* 字节数跟着记录走 —— wal_bytes() 靠它（原子读，不取锁） */
    atomic_fetch_add(&w->bytes, (u64)n + 4);
    if (xid > atomic_load(&w->max_xid))
        atomic_store(&w->max_xid, xid);

    /* 记下这条是哪个事务的，下一条同事务的记录用它做 prev_lsn */
    wal_set_prev_lsn(w, xid, lsn);
    return lsn;
}

u32 wal_append_xid(Wal *w, u8 type, u32 xid, u32 page_id, u16 slot, u16 slot_off,
                   const u8 *before, u16 blen, const u8 *after, u16 alen)
{
    if (!w)
        return 0;
    if ((u32)blen + alen > WAL_MAX_PAYLOAD)
        return 0;

    /*
     * 取号 → 定位文件尾 → 写记录，必须独占。
     *
     * 这三步拆开就有两个致命后果：
     *   - 两个线程拿到同一个 LSN → 恢复时日志顺序错乱
     *   - 两个线程的 fwrite 字节交错 → CRC 失败 → 恢复**截断** → 后面全丢
     */
    wal_lock(w);
    u32 lsn = wal_append_locked(w, type, xid, page_id, slot, slot_off,
                                before, blen, after, alen);
    wal_unlock(w);
    return lsn;
}
/*
 * 把 WAL 刷到磁盘（group commit，W-D 的 D-7）。
 *
 * ==================== 改之前是什么样的 ====================
 *
 * 每次调用都做**一次真实 fsync**。单线程下这没问题；多线程并发提交时
 * 它变成纯粹的浪费：磁盘一次 fsync 的代价主要由"等介质确认"决定，
 * 而"刷多少字节"影响很小 ⇒ N 个线程同时提交，共享一次就够了。
 *
 * ==================== 改之后 ====================
 *
 * 真正的协议在 `wal_group.c`（本文件不能无限膨胀：硬上限 800 行），
 * 这里只是入口：把"我要保证到 target 落盘"这件事交给那一层。
 *
 * target 取**调用时刻的 last_lsn**：
 *   - 它之前的记录此刻都已经写进内核（`setvbuf(_IONBF)`，见 wal_open）；
 *   - 它之后的记录是别人并发 append 的，**不该**由我的 fsync 负责。
 *     推进过头 = 谎报落盘 = 静默丢数据。
 *
 * 【为什么 target 在这里取，而不是在 group 里取】
 *
 * group 那边还要做一次 fflush 再选发起者。若在那边取 target，
 * 取到的是"fflush 之后"的值 —— 而那一批字节是否真被**这一次** fsync
 * 覆盖，取决于内核怎么排序。取小的那个是安全方向。
 *
 * 【返回值】
 *
 * E_IO 时调用方**必须**让事务失败。攒批之后一个事务的成败可能取决于
 * 另一个线程的 fsync 结果 —— 这正是"不许假成功"最容易被破坏的地方，
 * wal_group.c 里对那件事有专门的说明。
 */
int wal_fsync(Wal *w)
{
    u32 target;

    if (!w || !w->f)
        return E_INVAL;

    target = atomic_load(&w->last_lsn);
    if (target == 0u)
        return OK;              /* 日志还空着，没有要落盘的东西 */

    return wal_group_durable(w, target);
}

/*
 * 日志文件当前占用的字节数。
 *
 * 【读路径不取域锁】（v1.2 步骤 7 / A3）
 *
 * 字节数由 append 维护在一个原子量里，读的时候原子读一下就行 ——
 * 不需要 fseek/ftell，也就不需要动 FILE* 的位置，
 * 更不需要和 append 抢那把全局锁。
 *
 * 这条路径在**每条语句结束**时都会走一遍
 * （db_checkpoint_maybe 拿它和阈值比）——
 * 之前它每条语句都要排在别的线程的 COMMIT fsync 后面，
 * 是四线程吞吐的头号瓶颈（实测 WAL 锁等待 56.5 us/次）。
 */
u64 wal_bytes(Wal *w)
{
    if (!w)
        return 0;
    return atomic_load(&w->bytes);
}

/*
 * 截断日志：删掉 lsn <= keep_lsn 的记录。
 *
 * 【做法：把要保留的后缀挪到文件头，再把文件截短】
 *
 * LSN 在文件里是递增的，所以"要保留的记录"就是文件的一个**后缀**。
 * 把后缀整体前移，再截短文件，就完成了截断。
 *
 * 前移用固定大小的缓冲（64KB）流式做 —— 不把整个日志读进内存。
 *
 * 【为什么一条都留不住时要保留最后一条】
 * 见 wal.h 里的说明：LSN 不能倒退。
 * 日志被截空之后重开库，last_lsn 会算成 0，
 * 新记录的 LSN 从 1 重新开始 —— 而 checkpoint_lsn 可能是 5000，
 * 于是新记录全部落在 checkpoint_lsn 之下，恢复时被跳过 → 数据丢。
 */
int wal_truncate(Wal *w, u32 keep_lsn)
{
    if (!w || !w->f)
        return E_INVAL;

    wal_lock(w);

    /* 扫一遍：找切点、算保留部分的边界和统计量 */
    long long cut = -1;          /* 第一条要保留的记录的偏移 */
    long long last_off = -1;     /* 最后一条完整记录的偏移 */
    long long last_end = 0;      /* 最后一条完整记录的结束偏移 */
    u32  max_lsn = 0;            /* 保留部分的最大 lsn */
    u32  last_lsn = 0;
    u32  last_xid = 0;
    u32  max_xid = 0;            /* 保留部分的最大 xid */
    u32  nrec = 0;               /* 保留部分的记录条数 */

    WalRec r;
    wal_scan_begin_locked(w);
    while (wal_scan_next_locked(w, &r) == OK) {
        if (cut < 0 && r.lsn > keep_lsn)
            cut = r.off;
        /*
         * 只有切点之后的记录才被保留 —— 统计量必须只算保留部分，
         * 否则截断后 nrecords / max_xid 会带着"已经扔掉的记录"，
         * 重开库时 xid 会从一个虚高的值往下走（空洞），max_xid 也不准。
         */
        if (cut >= 0) {
            nrec++;
            if (r.lsn > max_lsn)
                max_lsn = r.lsn;
            if (r.xid > max_xid)
                max_xid = r.xid;
        }
        last_off = r.off;
        last_end = r.off + (long long)WAL_HDR + (long long)r.blen + r.alen + 4;
        last_lsn = r.lsn;
        last_xid = r.xid;
    }
    wal_scan_end_locked(w);

    if (nrec == 0) {
        /*
         * 一条都留不住 → 保留最后一条（LSN 不能倒退）。
         * 日志里总共一条记录都没有时，没什么可截的。
         */
        if (last_off < 0) {
            wal_unlock(w);
            return OK;             /* 日志本来就是空的 */
        }
        cut = last_off;
        max_lsn = last_lsn;
        max_xid = last_xid;
        nrec = 1;
    }

    /*
     * 把 [cut, last_end) 挪到**文件头之后**（v1.3 欠账 6 / W-D 的 D-1）。
     *
     * ⚠️ dst 的起点是 WAL_FILE_HDR_SIZE，不是 0：
     * 挪到 0 会把文件头覆盖掉 ⇒ 下次开库读到「被记录覆盖的垃圾头」⇒ 报
     * E_CORRUPT ⇒ 库打不开。这是加文件头之后最容易踩的一处。
     */
    long long newlen = last_end - cut;
    if (newlen > 0) {
        u8 buf[65536];
        long long src = cut;
        long long dst = (long long)WAL_FILE_HDR_SIZE;
        while (src < last_end) {
            long long n = last_end - src;
            if (n > (long long)sizeof(buf))
                n = (long long)sizeof(buf);
            if (FSEEK(w->f, src, SEEK_SET) != 0) {
                wal_unlock(w);
                return E_IO;
            }
            if (fread(buf, 1, (size_t)n, w->f) != (size_t)n) {
                wal_unlock(w);
                return E_IO;
            }
            if (FSEEK(w->f, dst, SEEK_SET) != 0) {
                wal_unlock(w);
                return E_IO;
            }
            if (fwrite(buf, 1, (size_t)n, w->f) != (size_t)n) {
                wal_unlock(w);
                return E_IO;
            }
            src += n;
            dst += n;
        }
    }

    /* 截短文件 —— 尾部那些不要的字节必须真的消失（长度含文件头） */
    if (WAL_TRUNCATE(w->f, (long long)WAL_FILE_HDR_SIZE + newlen) != 0) {
        wal_unlock(w);
        return E_IO;
    }

    atomic_store(&w->last_lsn, max_lsn);
    atomic_store(&w->nrecords, nrec);
    atomic_store(&w->max_xid, max_xid);
    /* 保留的记录本来就已经落盘了（截断之前调用方 fsync 过） */
    atomic_store(&w->flushed_lsn, max_lsn);
    /* 文件长度变了 → 字节数跟着变（wal_bytes() 的口径：**不含文件头**） */
    atomic_store(&w->bytes, (u64)newlen);
    w->scan_pos = (long)WAL_FILE_HDR_SIZE;

    /* 截断本身也要落盘（文件长度变了） */
    fflush(w->f);
    WAL_FSYNC(w->f);

    wal_unlock(w);
    return OK;
}

/* ---------------- 扫描 ---------------- */

/*
 * 扫描用**同一把** wal 域锁。
 *
 * 原因：扫描与 append 争的是同一个 FILE* 和同一个 scratch 缓冲。
 * 不加锁的话，扫描会读到"写了一半"的记录（CRC 失败 → 恢复截断）。
 */

static int wal_scan_begin_locked(Wal *w)
{
    /*
     * 从**文件头之后**开始扫（v1.3 欠账 6 / W-D 的 D-1）。
     * 从 0 开始的话，那 32 字节头会被当成一条记录：
     * lsn 会读成 WAL_MAGIC（天文数字），载荷长度是垃圾 ⇒ CRC 失败 ⇒
     * 扫描在第一条就 E_EOF ⇒ **整份日志一个字都读不出来**。
     */
    w->scan_pos = (long)WAL_FILE_HDR_SIZE;
    return OK;
}

static void wal_scan_end_locked(Wal *w)
{
    w->scan_pos = (long)WAL_FILE_HDR_SIZE;
}

static int wal_scan_next_locked(Wal *w, WalRec *r)
{
    u8 *b = w->scratch;

    if (FSEEK(w->f, (long long)w->scan_pos, SEEK_SET) != 0)
        return E_EOF;
    if (fread(b, 1, WAL_HDR, w->f) != WAL_HDR)
        return E_EOF;                 /* 文件尾 */

    r->lsn = get_u32(b + 0);
    r->prev_lsn = get_u32(b + 4);
    r->xid = get_u32(b + 8);
    r->type = b[12];
    r->page_id = get_u32(b + 16);
    r->slot = get_u16(b + 20);
    r->blen = get_u16(b + 22);
    r->alen = get_u16(b + 24);
    r->slot_off = get_u16(b + 26);
    r->off = w->scan_pos;

    u32 payload = (u32)r->blen + r->alen;
    if (payload > WAL_MAX_PAYLOAD)
        return E_EOF;                 /* 记录头损坏，当作日志到此为止 */

    if (payload && fread(b + WAL_HDR, 1, payload, w->f) != payload)
        return E_EOF;                 /* 载荷不完整（写到一半断电）*/

    u32 stored = 0;
    if (fread(b + WAL_HDR + payload, 1, 4, w->f) != 4)
        return E_EOF;
    stored = get_u32(b + WAL_HDR + payload);

    if (crc32(b, WAL_HDR + payload) != stored)
        return E_EOF;                 /* 校验失败：截断到最后一条完整记录 */

    w->scan_pos += (long)(WAL_HDR + payload + 4);
    return OK;
}

int wal_scan_begin(Wal *w)
{
    if (!w)
        return E_INVAL;
    wal_lock(w);
    wal_scan_begin_locked(w);
    wal_unlock(w);
    return OK;
}

void wal_scan_end(Wal *w)
{
    if (!w)
        return;
    wal_lock(w);
    wal_scan_end_locked(w);
    wal_unlock(w);
}

int wal_scan_next(Wal *w, WalRec *r)
{
    if (!w || !r)
        return E_INVAL;

    wal_lock(w);
    int rc = wal_scan_next_locked(w, r);
    wal_unlock(w);
    return rc;
}

int wal_read_payload(Wal *w, const WalRec *r, u8 *buf, u16 cap)
{
    if (!w || !r || !buf)
        return E_INVAL;
    u32 n = (u32)r->blen + r->alen;
    if (n > cap)
        return E_NOSPACE;
    if (n == 0)
        return OK;

    /* 和 append 抢同一个 FILE* → 必须独占 */
    wal_lock(w);
    int rc = OK;
    if (FSEEK(w->f, (long long)(r->off + WAL_HDR), SEEK_SET) != 0)
        rc = E_IO;
    else if (fread(buf, 1, n, w->f) != n)
        rc = E_IO;
    wal_unlock(w);
    return rc;
}
