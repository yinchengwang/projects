#include "txn/txn.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define CLOG_FSYNC(f) _commit(_fileno(f))
#define FSEEK _fseeki64
#define FTELL _ftelli64
#else
#include <unistd.h>
#define CLOG_FSYNC(f) fsync(fileno(f))
#define FSEEK fseeko
#define FTELL ftello
#endif

#include "common/latch.h"

/*
 * 事务表 + clog 实现。
 *
 * 核心不变量：
 *   1. next_xid 单调递增，永不回退
 *   2. clog 环形窗口：clog[xid % SLOTS] 存 xid 的状态
 *   3. 一个 xid 在活跃表里最多出现一次
 *
 * 第 3 条靠"分配时扫描找空槽"保证 —— 事务数很少（几十个），
 * 线性扫描比任何哈希结构都简单且不会出错。
 *
 * ==================== v1.2 步骤 4：txn 域的锁 ====================
 *
 * txn 域保护的东西：next_xid、clog、活跃表。
 *
 * 【写路径：域锁】
 *   xid 分配 / 活跃表增删 / clog 写入 —— 一律在域锁里。
 *   域锁是**进程级**的一把互斥量：一个进程里同时开两个库的情况极少，
 *   而进程级锁换来"不需要 init/destroy、不需要改任何调用点"。
 *
 * 【读路径：无锁】
 *   可见性判断每读一行就要查一次 clog —— 这是最热的读路径。
 *   每次取锁会把 10 万行的扫描拖慢好几毫秒（A9 门槛会破），
 *   所以读路径走**原子读**（见 txn.h 里 TxnTable 的说明）。
 *
 * 【为什么写路径不能也用原子】
 *   分配 xid 要"找一个空槽 + 占住它"，两个线程可能同时看上同一个空槽 →
 *   必须互斥。clog 的读-改-写也是同理（这正是改成"一 xid 一字节"的原因：
 *   2bit 打包时两个线程改同一字节里的不同 xid 会互相覆盖）。
 */

/* ---- txn 域的锁（进程级） ---- */

static pthread_mutex_t g_txn_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * 取域锁 + 向 latch 框架记账。
 *
 * 记账让"catalog → txn → wal → freelist → frame"这条全序
 * 从步骤 2 起就是**机器检查**的（步骤 2 只立框架，步骤 3/4 才接线）。
 */
static void txn_lock(void)
{
    pthread_mutex_lock(&g_txn_lock);
    latch_order_acquire(LATCH_TXN);
}

static void txn_unlock(void)
{
    latch_order_release(LATCH_TXN);
    pthread_mutex_unlock(&g_txn_lock);
}

/* ---- clog：分块数组（原子读，无锁）+ 落盘文件 ---- */

/*
 * 取（必要时分配）xid 所在的那一块。
 *
 * 调用方必须已持 txn 域锁（或处于单线程的初始化阶段）——
 * 分配块要写块指针表，不能让两个线程同时分配同一块。
 *
 * 块的默认值是 0 = TXN_CLOG_INPROGRESS，与 clog 文件里的默认值一致。
 */
static u8 *clog_chunk_w(TxnTable *t, u32 xid)
{
    u32 ci = xid >> TXN_CLOG_CHUNK_BITS;
    if (ci >= TXN_CLOG_CHUNKS)
        return NULL;

    u8 *c = atomic_load(&t->clog_chunk[ci]);
    if (c)
        return c;

    c = calloc(TXN_CLOG_CHUNK_SIZE, 1);
    if (!c)
        return NULL;

    atomic_store(&t->clog_chunk[ci], c);
    return c;
}

/*
 * 读 xid 的状态 —— **无锁**（原子读）。
 *
 * 块指针用 atomic_load 读：写者可能正在分配新块，
 * 但已经发布出去的块指针不会被改，读者拿到的永远是完整的一块。
 */
static u8 clog_get(const TxnTable *t, u32 xid)
{
    u32 ci = xid >> TXN_CLOG_CHUNK_BITS;
    if (ci >= TXN_CLOG_CHUNKS)
        return TXN_CLOG_COMMITTED;      /* 超出可表示范围：当作早已结束 */

    u8 *c = atomic_load(&t->clog_chunk[ci]);
    if (!c)
        return TXN_CLOG_COMMITTED;      /* 这一块从没被用过 */

    return atomic_load(&c[xid & (TXN_CLOG_CHUNK_SIZE - 1u)]);
}

/*
 * 写 xid 的状态 —— 调用方必须已持 txn 域锁。
 *
 * 顺带把 xid 记进"待落盘区间"。
 *
 * 【为什么区间是闭区间而不是位图】
 * xid 是连续分配的，落盘时按区间整段写最省事。
 * 区间可能很大（几万字节），但一次 fwrite 就完了。
 */
static void clog_set(TxnTable *t, u32 xid, u8 state)
{
    u8 *c = clog_chunk_w(t, xid);
    if (!c)
        return;                       /* 分配失败：状态存不下来（OOM） */

    atomic_store(&c[xid & (TXN_CLOG_CHUNK_SIZE - 1u)], state);

    if (t->clog_hi == 0) {            /* 之前没有脏区间 */
        t->clog_lo = xid;
        t->clog_hi = xid;
    } else {
        if (xid < t->clog_lo)
            t->clog_lo = xid;
        if (xid > t->clog_hi)
            t->clog_hi = xid;
    }
    if (xid > t->clog_max_xid)
        t->clog_max_xid = xid;
}

void txn_clog_set(TxnTable *t, u32 xid, u8 state)
{
    if (!t || xid == 0)
        return;
    txn_lock();
    clog_set(t, xid, state);
    txn_unlock();
}

void txn_table_init(TxnTable *t, u32 start_xid)
{
    /*
     * 【为什么不用 memset 清整个结构】
     *
     * 结构里有原子成员。memset 是"按字节写"，对原子对象来说
     * 不是合法的初始化方式（虽然实际效果一样）。逐个 atomic_init 更干净。
     */
    for (int i = 0; i < TXN_MAX_ACTIVE; i++) {
        atomic_init(&t->active[i].xid, 0);
        atomic_init(&t->active[i].state, TXN_IDLE);
    }
    atomic_init(&t->n_active, 0);
    atomic_init(&t->committed_xmax, 0);
    atomic_init(&t->aborted_xmax, 0);

    /*
     * next_xid 从 start_xid + 1 开始。
     *
     * 为什么 +1：start_xid 是"上一个会话用到的最大 xid"，
     * 这个 xid 本身已经结束了（提交或中止），新事务不能用它。
     */
    u32 nx = start_xid + 1;
    if (nx == 0)
        nx = 1;        /* 0 保留给系统事务 */
    atomic_init(&t->next_xid, nx);

    /*
     * clog 的默认值是"进行中"。
     *
     * 为什么不是"已提交"：clog 只对**已分配**的 xid 有意义，
     * 而已分配的 xid 一律先被标成"进行中"，提交/中止时再改。
     * 还没分配的 xid 由 `xid >= next_xid` 那条规则挡住。
     *
     * 分块数组不在这里初始化 —— 块是**按需分配**的：
     * 分配 64KB 一块来存几个字节太浪费，而且 calloc 出来的
     * 默认值正好就是"进行中"（0）。
     */
    for (u32 i = 0; i < TXN_CLOG_CHUNKS; i++)
        atomic_init(&t->clog_chunk[i], NULL);

    /* 落盘状态（v1.2 步骤 6） */
    t->clog_fp = NULL;
    t->clog_lo = 0;
    t->clog_hi = 0;
    t->clog_max_xid = 0;
    t->clog_ready = false;
}

int txn_begin(TxnTable *t, u32 *xid_out)
{
    if (!t || !xid_out)
        return E_INVAL;

    txn_lock();

    int rc = E_NOSPACE;
    for (int i = 0; i < TXN_MAX_ACTIVE; i++) {
        if (atomic_load(&t->active[i].state) != TXN_IDLE)
            continue;

        u32 xid = atomic_load(&t->next_xid);
        u32 nx = xid + 1;
        if (nx == 0)
            nx = 1;    /* 回绕保护（实际上 40 亿次才可能）*/
        /*
         * 【发布顺序：先 clog，再 next_xid】
         *
         * 反过来的话，读者可能看到 "xid < next_xid"（像是已分配）
         * 但 clog[xid] 还是默认值 —— 默认值是"进行中"，所以其实也安全？
         * 不对：默认值是 INPROGRESS，读者会判成"不可见"，那是**保守**的，
         * 安全。真正危险的是把默认值当成"已提交"。
         *
         * 分块之后这条更简单：块是按需分配的，
         * 分配 xid 时顺手把那一字节置成"进行中"，
         * 读者就不会看到"没分配过的槽位残留着上一个 xid 的已提交"。
         */
        clog_set(t, xid, TXN_CLOG_INPROGRESS);
        atomic_store(&t->next_xid, nx);

        atomic_store(&t->active[i].xid, xid);
        atomic_store(&t->active[i].state, TXN_ACTIVE);
        atomic_fetch_add(&t->n_active, 1);

        *xid_out = xid;
        rc = OK;
        break;
    }

    txn_unlock();
    return rc;
}

/* 结束一个事务（调用方已持域锁） */
static int txn_finish(TxnTable *t, u32 xid, u8 state)
{
    for (int i = 0; i < TXN_MAX_ACTIVE; i++) {
        if (atomic_load(&t->active[i].state) == TXN_ACTIVE &&
            atomic_load(&t->active[i].xid) == xid) {
            /* 先从活跃表摘掉，再改 clog —— 顺序反过来会让快照多看见一个活跃事务（保守，安全） */
            atomic_store(&t->active[i].xid, 0);
            atomic_store(&t->active[i].state, TXN_IDLE);
            atomic_fetch_sub(&t->n_active, 1);
            clog_set(t, xid, state);
            return OK;
        }
    }
    return E_TXN;      /* 不在活跃表里：已经结束过，或根本不是本会话的事务 */
}

int txn_commit(TxnTable *t, u32 xid)
{
    if (!t)
        return E_INVAL;

    txn_lock();
    int rc = txn_finish(t, xid, TXN_CLOG_COMMITTED);
    if (rc == OK) {
        /* 写者都持域锁 → 直接读写即可，不需要 CAS */
        u32 cur = atomic_load(&t->committed_xmax);
        if (xid > cur)
            atomic_store(&t->committed_xmax, xid);
    }
    txn_unlock();
    return rc;
}

int txn_abort(TxnTable *t, u32 xid)
{
    if (!t)
        return E_INVAL;

    txn_lock();
    int rc = txn_finish(t, xid, TXN_CLOG_ABORTED);
    if (rc == OK) {
        u32 cur = atomic_load(&t->aborted_xmax);
        if (xid > cur)
            atomic_store(&t->aborted_xmax, xid);
    }
    txn_unlock();
    return rc;
}

/*
 * 查 clog 的提交状态 —— **无锁**（原子读）。
 *
 * 这是最热的读路径（每读一行都要调），所以一把锁都不取。
 */
u8 txn_clog_state(const TxnTable *t, u32 xid)
{
    if (!t)
        return TXN_CLOG_COMMITTED;
    if (xid == 0)
        return TXN_CLOG_COMMITTED;      /* 系统事务 */

    /*
     * 超过 next_xid 的 xid 还没分配出来 → 进行中。
     *
     * next_xid 只读一次：两次读之间它可能又涨了，
     * 用同一个值判断才不会前后不一致。
     */
    u32 nx = atomic_load(&t->next_xid);
    if (xid >= nx)
        return TXN_CLOG_INPROGRESS;      /* 还没分配出来的事务 */

    /*
     * 其它情况一律查 clog 块。
     *
     * v0.7 的环形窗口有个"窗口外一律当已提交"的兜底。
     * 分块之后**不再需要**这个兜底 ——
     * 老 xid 的状态不会被覆盖，查到的就是真实状态。
     *
     * （块不存在 = 这个 xid 从来没被分配过 → 按已提交兜底。
     *   这种情况只可能出现在"没有 clog 文件的老库"上。）
     */
    return clog_get(t, xid);
}

bool txn_committed(const TxnTable *t, u32 xid)
{
    return txn_clog_state(t, xid) == TXN_CLOG_COMMITTED;
}

bool txn_aborted(const TxnTable *t, u32 xid)
{
    return txn_clog_state(t, xid) == TXN_CLOG_ABORTED;
}

/* ---- 活跃表（读路径也是无锁的：原子读） ---- */

bool txn_has_active(const TxnTable *t)
{
    return t && atomic_load(&t->n_active) > 0;
}

bool txn_has_active_except(const TxnTable *t, u32 exclude_xid)
{
    if (!t)
        return false;
    for (int i = 0; i < TXN_MAX_ACTIVE; i++) {
        if (atomic_load(&t->active[i].state) != TXN_ACTIVE)
            continue;
        if (atomic_load(&t->active[i].xid) == exclude_xid)
            continue;         /* 忽略自己 */
        return true;
    }
    return false;
}

u32 txn_oldest_active(const TxnTable *t)
{
    if (!t)
        return 0;

    u32 oldest = 0;
    for (int i = 0; i < TXN_MAX_ACTIVE; i++) {
        if (atomic_load(&t->active[i].state) != TXN_ACTIVE)
            continue;
        u32 xid = atomic_load(&t->active[i].xid);
        if (oldest == 0 || xid < oldest)
            oldest = xid;
    }
    return oldest;
}

int txn_nactive(const TxnTable *t)
{
    return t ? atomic_load(&t->n_active) : 0;
}

/* ---------------- clog 落盘（v1.2 步骤 6） ---------------- */

/*
 * 打开 clog 文件，并把文件里的内容读进内存的 clog 块。
 *
 * 【为什么必须整个读进来】
 * 恢复阶段判断可见性时要查 clog。日志被截断之后，
 * 那些 xid 的提交记录已经不在日志里了 —— 唯一的依据就是 clog 文件。
 * 只记个文件偏移是不够的：查询是随机的，要能直接读到那一字节。
 */
int txn_clog_open(TxnTable *t, const char *path)
{
    if (!t || !path)
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

    if (FSEEK(f, 0, SEEK_END) != 0) {
        fclose(f);
        return E_IO;
    }
    long long sz = FTELL(f);
    if (sz < 0)
        sz = 0;

    t->clog_fp = f;
    t->clog_max_xid = (sz > 0) ? (u32)(sz - 1) : 0;
    t->clog_ready = true;

    if (sz > 0) {
        if (FSEEK(f, 0, SEEK_SET) != 0) {
            fclose(f);
            t->clog_fp = NULL;
            return E_IO;
        }

        u64 pos = 0;
        while (pos < (u64)sz) {
            u32 ci = (u32)(pos >> TXN_CLOG_CHUNK_BITS);
            u32 off = (u32)(pos & (TXN_CLOG_CHUNK_SIZE - 1u));
            u32 n = TXN_CLOG_CHUNK_SIZE - off;
            if (pos + n > (u64)sz)
                n = (u32)((u64)sz - pos);

            /* 按块读：文件偏移 = xid，块内偏移 = xid & (块大小-1) */
            u8 *c = clog_chunk_w(t, ci << TXN_CLOG_CHUNK_BITS);
            if (!c) {
                fclose(f);
                t->clog_fp = NULL;
                return E_IO;
            }
            if (fread(c + off, 1, n, f) != n) {
                fclose(f);
                t->clog_fp = NULL;
                return E_IO;
            }
            pos += n;
        }
    }
    return OK;
}

/*
 * 把待落盘的 xid 区间写进文件并 fsync。
 *
 * 【调用时机的铁律】
 * 截断 WAL 之前必须先调它。顺序反了 = 提交状态随日志一起消失
 * → 重启后已提交的数据全部不可见（静默丢数据）。
 */
int txn_clog_sync(TxnTable *t)
{
    if (!t)
        return E_INVAL;
    if (!t->clog_fp)
        return OK;               /* 没开落盘：没什么可做的 */
    if (t->clog_hi == 0)
        return OK;               /* 没有脏区间 */

    txn_lock();
    u32 lo = t->clog_lo;
    u32 hi = t->clog_hi;
    t->clog_lo = 0;
    t->clog_hi = 0;
    txn_unlock();

    /*
     * 逐块写。
     *
     * 一段脏区间可能横跨多块（xid 每 65536 个换一块），
     * 所以按块边界切分后逐段 fwrite。
     */
    u64 x = lo;
    while (x <= (u64)hi) {
        u32 ci = (u32)(x >> TXN_CLOG_CHUNK_BITS);
        u32 off = (u32)(x & (TXN_CLOG_CHUNK_SIZE - 1u));
        u32 n = TXN_CLOG_CHUNK_SIZE - off;
        if (x + n > (u64)hi + 1)
            n = (u32)((u64)hi + 1 - x);

        u8 *c = atomic_load(&t->clog_chunk[ci]);
        if (c) {
            if (FSEEK(t->clog_fp, (long long)x, SEEK_SET) != 0)
                return E_IO;
            if (fwrite(c + off, 1, n, t->clog_fp) != n)
                return E_IO;
        }
        x += n;
    }

    fflush(t->clog_fp);
    if (CLOG_FSYNC(t->clog_fp) != 0)
        return E_IO;
    return OK;
}

void txn_clog_close(TxnTable *t)
{
    if (!t)
        return;
    if (!t->clog_fp)
        return;
    (void)txn_clog_sync(t);
    fclose(t->clog_fp);
    t->clog_fp = NULL;
    t->clog_ready = false;
}

void txn_clog_discard(TxnTable *t)
{
    if (!t)
        return;
    if (!t->clog_fp)
        return;
    /* 不 sync：模拟断电 —— 没落盘的东西就该没有 */
    fclose(t->clog_fp);
    t->clog_fp = NULL;
    t->clog_ready = false;
}

/*
 * 把 next_xid 推到 xid 之后。
 *
 * 【为什么需要】
 * next_xid 的初值来自日志里的最大 xid。但日志可能被截断过
 * （步骤 6 的 checkpoint 会截），截断之后日志里的最大 xid 会变小 ——
 * 这时真正的"用过的最大 xid"在 clog 文件里。
 *
 * 如果不推一把，新会话会**重用**旧 xid → 旧事务的提交状态
 * 被新事务覆盖 → 可见性判断错乱（静默的数据错误）。
 */
void txn_table_bump_xid(TxnTable *t, u32 xid)
{
    if (!t)
        return;
    txn_lock();
    u32 nx = atomic_load(&t->next_xid);
    if (xid >= nx) {
        u32 v = xid + 1;
        if (v == 0)
            v = 1;                /* 回绕保护 */
        atomic_store(&t->next_xid, v);
    }
    txn_unlock();
}

u32 txn_clog_max_xid(const TxnTable *t)
{
    if (!t)
        return 0;
    txn_lock();
    u32 v = t->clog_max_xid;
    txn_unlock();
    return v;
}
