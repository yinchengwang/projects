/*
 * pager_frame.c — 帧这一层：哈希表、Clock 逐出、内容锁、取帧
 *
 * ==================== 这一层为什么存在 ====================
 *
 * v1.2 步骤 7 之后 pager.c 有 1002 行，超过"单文件不超过 800 行"的硬规则。
 * 把"帧"这件事单独拆出来：
 *
 *   pager_frame_lock / pager_frame_unlock  每帧一把内容锁（递归）
 *   pager_hash_init / pager_hash_find      page_id → 帧下标
 *   pick_victim                             Clock 逐出挑受害者
 *   pager_fetch_frame                       取帧（命中 / 逐出 / 载入）
 *
 * ==================== 锁的设计（务必先读 pager.c 的文件头） ====================
 *
 * 两把锁，分工完全不同：
 *
 *   【池级锁 pg->mu（读写锁）】—— 管**帧的元数据**：
 *       哈希表（page_id → 帧下标）、Clock 手、pin_count、npages。
 *       这几样是"整个池子共用的账本"，必须互斥。
 *       读多写少 → 用读锁，读读不互斥。
 *
 *   【内容锁 pg->latches[i]】—— 管**一帧的页内容**：
 *       从 pager_get / pager_alloc 拿到帧，一直持有到 pager_unpin。
 *       同一时刻只有一个线程能改这一页的内容。
 *
 * 不变式（死锁推理的全部依据）：
 *
 *     **持有内容锁 ⇒ 该帧的 pin_count ≥ 1**
 *
 * 由它推出两条：
 *
 *   1. pin_count == 0 的帧，**内容锁一定是空的**
 *      → 逐出路径（持写锁 → 取受害者内容锁）不会阻塞在内容锁上
 *      → 不存在"池级锁等内容锁、内容锁持有者等池级锁"的环
 *   2. 逐出路径只可能挑 pin_count == 0 的帧（Clock 的规矩），
 *      所以内容锁的持有者不可能是"正在被逐出的那一帧"
 *
 * 命中路径上的顺序也没变：**先加 pin_count，再取内容锁**
 * （在读锁里做完加一，放开读锁才去取内容锁）——
 * 所以"取内容锁时 pin_count 一定已经 ≥ 1" ✓。
 *
 * ==================== 纪律 ====================
 *
 * 本文件是**纯搬移**：锁的语义、不变量、调用顺序一个字节都没改，
 * 只是从 pager.c 里搬出来，方便以后各改各的。
 */

#include "storage/pager_internal.h"

#include <stdlib.h>

#include "common/latch.h"
#include "common/lockstat.h"
#include "storage/page.h"

/* ---------------- 内容锁 ---------------- */

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
void pager_frame_lock(Pager *pg, u32 fi)
{
    /*
     * 先拿锁，再记 owner。
     *
     * 顺序反了会出这种错：线程 A 先写 owner=A，
     * 线程 B 也写 owner=B（还没拿到锁），A 就再也对不上了。
     */
    LS_ENTER(LS_FRAME);
#ifdef DB_LOCKSTAT
    if (pthread_mutex_trylock(&pg->latches[fi]) != 0) {
        /*
         * trylock 失败有两种情况，要分开记：
         *   - 锁被**别的线程**占着 → 真争用
         *   - 锁被**本线程**占着（内容锁是递归的）→ 不是争用
         */
        if (pg->latch_depth[fi] > 0 &&
            pthread_equal(pg->latch_owner[fi], pthread_self()))
            atomic_fetch_add(&g_ls_recur[LS_FRAME], 1ull);
        else {
            atomic_fetch_add(&g_ls_cont[LS_FRAME], 1ull);
            atomic_fetch_add(&g_ls_page_cont[pg->frames[fi].page_id % 4096], 1ull);
            atomic_fetch_add(&g_ls_tbl_cont[page_table_id(pg->frames[fi].data) % 64], 1ull);
        }
        pthread_mutex_lock(&pg->latches[fi]);
    }
#else
    pthread_mutex_lock(&pg->latches[fi]);
#endif
    LS_LEAVE(LS_FRAME);
    if (pg->latch_depth[fi] == 0)
        pg->latch_owner[fi] = pthread_self();
    pg->latch_depth[fi]++;
    latch_order_acquire(LATCH_FRAME);}

/*
 * 释放内容锁。
 *
 * 返回 E_INVAL 表示"这把锁不是当前线程拿的" ——
 * 那是调用方的 bug（unpin 了一个自己没 get 过的页）。
 * 这种情况**绝不能真的去 unlock**：POSIX 里释放非自己持有的锁是未定义行为，
 * 可能把锁的状态搅坏。
 */
int pager_frame_unlock(Pager *pg, u32 fi)
{
    if (pg->latch_depth[fi] <= 0)
        return E_INVAL;
    if (!pthread_equal(pg->latch_owner[fi], pthread_self()))
        return E_INVAL;

    pg->latch_depth[fi]--;
    latch_order_release(LATCH_FRAME);
    pthread_mutex_unlock(&pg->latches[fi]);
    return OK;
}

/* ---------------- 哈希表：开放定址 + 线性探测 + 墓碑 ---------------- */

#define KEY_EMPTY ((u32)0xFFFFFFFFu)   /* 即 PAGE_ID_NIL，合法页号不会是它 */
#define KEY_TOMB  ((u32)0xFFFFFFFEu)

static u32 hash_u32(u32 k)
{
    return k * 2654435761u;   /* Knuth 乘法散列 */
}

static u32 next_pow2(u32 n)
{
    u32 p = 16;
    while (p < n)
        p <<= 1;
    return p;
}

void pager_hash_init(Pager *pg)
{
    pg->hash_cap = next_pow2(pg->nframes * 2);   /* 装填因子 <= 0.5 */
    pg->hash_keys = malloc(pg->hash_cap * sizeof(u32));
    pg->hash_vals = malloc(pg->hash_cap * sizeof(u32));
    for (u32 i = 0; i < pg->hash_cap; i++)
        pg->hash_keys[i] = KEY_EMPTY;
}

/* 以下哈希函数都必须在池级锁下调用 */

bool pager_hash_find(Pager *pg, u32 key, u32 *val)
{
    u32 mask = pg->hash_cap - 1;
    u32 i = hash_u32(key) & mask;
    for (;;) {
        u32 k = pg->hash_keys[i];
        if (k == KEY_EMPTY)
            return false;
        if (k == key) {
            *val = pg->hash_vals[i];
            return true;
        }
        i = (i + 1) & mask;
    }
}

static void hash_ins(Pager *pg, u32 key, u32 val)
{
    u32 mask = pg->hash_cap - 1;
    u32 i = hash_u32(key) & mask;
    while (pg->hash_keys[i] != KEY_EMPTY &&
           pg->hash_keys[i] != KEY_TOMB &&
           pg->hash_keys[i] != key)
        i = (i + 1) & mask;
    pg->hash_keys[i] = key;
    pg->hash_vals[i] = val;
}

static void hash_del(Pager *pg, u32 key)
{
    u32 mask = pg->hash_cap - 1;
    u32 i = hash_u32(key) & mask;
    for (;;) {
        u32 k = pg->hash_keys[i];
        if (k == KEY_EMPTY)
            return;               /* 不在里面，静默返回 */
        if (k == key) {
            pg->hash_keys[i] = KEY_TOMB;
            pg->hash_tombs++;
            return;
        }
        i = (i + 1) & mask;
    }
}

/*
 * 墓碑回收：清表重插。真值来源是 frames 数组（page_id 字段），
 * 所以重建不需要额外的持久化结构。
 * 触发条件：墓碑超过容量 1/4。活跃条目 <= 帧数 <= 容量/2，
 * 加上 <=1/4 的墓碑，总占用 < 3/4 容量，探测永远能遇到 EMPTY。
 */
static void hash_rebuild(Pager *pg)
{
    for (u32 i = 0; i < pg->hash_cap; i++)
        pg->hash_keys[i] = KEY_EMPTY;
    pg->hash_tombs = 0;
    for (u32 i = 0; i < pg->nframes; i++) {
        u32 pid = pg->frames[i].page_id;
        if (pid != PAGE_ID_NIL)
            hash_ins(pg, pid, i);
    }
}

/* ---------------- Clock 替换 ---------------- */

/*
 * 找一个可逐出的帧。pin_count == 0 且 clock_ref == 0 的帧被选中；
 * ref == 1 的给第二次机会（清零放行）。
 * 全部被 pin 时返回 E_NOSPACE —— 宁可报错，绝不丢别人正拿着的页。
 *
 * 必须在池级锁下调用。
 */
static int pick_victim(Pager *pg, u32 *victim)
{
    u32 bound = BP_MAX_PIN_TRIES * pg->nframes;
    for (u32 tries = 0; tries < bound; tries++) {
        PageFrame *f = &pg->frames[pg->hand];
        if (atomic_load(&f->pin_count) == 0) {
            if (atomic_load(&f->clock_ref) == 0) {
                *victim = pg->hand;
                pg->hand = (pg->hand + 1) % pg->nframes;
                return OK;
            }
            atomic_store(&f->clock_ref, 0);   /* 第二次机会用掉了 */
        }
        pg->hand = (pg->hand + 1) % pg->nframes;
    }
    return E_NOSPACE;
}

/* ---------------- 核心：命中 / 逐出 / 载入 ---------------- */

/*
 * 取一页并 pin 住（内容锁也一并拿到）。
 *
 * 返回时：池级锁已释放，**内容锁由调用方持有**，
 * 一直到 pager_unpin 才释放。
 */
int pager_fetch_frame(Pager *pg, u32 page_id, PageFrame **out)
{
    int rc;
    u32 fi;

    /*
     * ---- 命中快路径：只读账本 ----
     *
     * 用**读锁**：读读不互斥，多个线程可以同时查哈希表 ✓
     * 这是 A3 的关键 —— 以前这里是互斥量，所有页访问排成一条队。
     */
    pager_pool_rdlock(pg);
    if (pager_hash_find(pg, page_id, &fi)) {
        PageFrame *f = &pg->frames[fi];
        atomic_fetch_add(&f->pin_count, 1);
        atomic_store(&f->clock_ref, 1);
        pager_pool_rdunlock(pg);
        /*
         * 内容锁在**池级锁之外**拿。
         *
         * 反过来（持池级锁等内容锁）会死锁：
         *   线程 A 持池级锁，等页 P 的内容锁；
         *   线程 B 持 P 的内容锁（还没 unpin），等池级锁减 pin_count。
         * A 等 B，B 等 A。
         *
         * 读锁放开之后才去取内容锁：
         * 取内容锁时 pin_count 已经 ≥ 1（上面加过了），
         * 所以这一帧不会被逐出线程挑走 ✓
         */
        pager_frame_lock(pg, fi);
        *out = f;
        return OK;
    }
    pager_pool_rdunlock(pg);

    /*
     * ---- 未命中：改账本（逐出 / 载入），用写锁 ----
     *
     * 写锁独占：逐出期间不允许别的线程改哈希表。
     */
    pager_pool_wrlock(pg);

    /*
     * 放锁再上锁的这段时间里，别人可能已经把这一页载进来了
     * （同一页被两个线程同时取）→ 重查一次，命中就走快路径。
     */
    if (pager_hash_find(pg, page_id, &fi)) {
        PageFrame *f = &pg->frames[fi];
        atomic_fetch_add(&f->pin_count, 1);
        atomic_store(&f->clock_ref, 1);
        pager_pool_wrunlock(pg);
        pager_frame_lock(pg, fi);
        *out = f;
        return OK;
    }

    /* 未命中：挑一个受害者 */
    rc = pick_victim(pg, &fi);
    if (rc != OK) {
        pager_pool_wrunlock(pg);
        return rc;
    }

    PageFrame *f = &pg->frames[fi];

    /*
     * 受害者的内容锁。
     *
     * 按不变式（持有内容锁 ⇒ pin_count ≥ 1），
     * 受害者的 pin_count == 0 ⇒ 此刻**没人持有**它的内容锁
     * ⇒ 这一步不会阻塞 ✓
     */
    pager_frame_lock(pg, fi);

    /* 受害者若还缓存着别的页：脏的先刷盘，再从哈希表摘掉 */
    if (f->page_id != PAGE_ID_NIL) {
        if (f->dirty) {
            /* WAL 钩子在这里被调 —— 锁序 frame → wal ✓ */
            rc = pager_flush_page(pg, f->page_id, f->data);
            if (rc != OK) {
                pager_frame_unlock(pg, fi);
                pager_pool_unlock(pg);
                return rc;
            }
        }
        hash_del(pg, f->page_id);
        f->page_id = PAGE_ID_NIL;
        f->dirty = false;
    }

    rc = pager_read_page(pg, page_id, f->data);
    if (rc != OK) {
        pager_frame_unlock(pg, fi);
        pager_pool_unlock(pg);
        return rc;
    }

    /*
     * 同一页被两个线程同时载入的兜底。
     *
     * 正常情况下不会发生（页号是分配器给的，别人拿不到），
     * 但万一发生（恢复路径按日志里的页号取页），
     * 哈希表里就会有两个条目指向同一页 → 哈希和帧对不上 → 页内容错乱。
     * 这里以"先到先得"为准：后来者丢掉自己这份，用先到的那份。
     */
    u32 other;
    if (pager_hash_find(pg, page_id, &other)) {
        f->page_id = PAGE_ID_NIL;
        f->dirty = false;
        pager_frame_unlock(pg, fi);

        PageFrame *of = &pg->frames[other];
        atomic_fetch_add(&of->pin_count, 1);
        atomic_store(&of->clock_ref, 1);
        pager_pool_unlock(pg);
        pager_frame_lock(pg, other);
        *out = of;
        return OK;
    }

    if (pg->hash_tombs > pg->hash_cap / 4)
        hash_rebuild(pg);   /* 墓碑回收，防探测死循环 */

    f->page_id = page_id;
    atomic_store(&f->pin_count, 1);
    atomic_store(&f->clock_ref, 1);
    /*
     * 帧被复用了：上一任主人留下的 rec_lsn 与新页无关。
     * 不清零的话，新页的第一次改动会继承别人的 recLSN →
     * 重放点被压低（安全方向，但白费功夫）。
     */
    f->rec_lsn = 0;
    hash_ins(pg, page_id, fi);

    /* 内容锁已经持有着（pager_frame_lock 那一步），不用再拿一次 */
    pager_pool_unlock(pg);
    *out = f;
    return OK;
}
