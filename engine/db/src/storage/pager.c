/*
 * pager.c — 页读写 + buffer pool（Clock 替换）
 *
 * 设计取舍（都是教学导向的）：
 *   1. 用 stdio 而不是 pread/pwrite：可移植（MinGW 没有 pread），
 *      单线程阶段完全够用。等做并发时再换，那时你会知道为什么。
 *   2. 关掉 stdio 自己的缓冲（_IONBF）：我们整页读写，stdio 再缓冲
 *      一层就是白拷贝一次。
 *   3. 哈希表用开放定址 + 墓碑删除：比拉链缓存友好，且没有
 *      malloc/free 的碎片问题。规模固定（条目数 <= 帧数），永不扩容。
 *
 * ==================== v1.2 步骤 4c：frame 域的锁 ====================
 *
 * 帧这一层的锁分两把：
 *
 *   【池级锁 pg->mu】—— 管**帧的元数据**：
 *       哈希表（page_id → 帧下标）、Clock 手、pin_count、npages。
 *       这几样是"整个池子共用的账本"，必须互斥。
 *
 *   【内容锁 pg->latches[i]】—— 管**一帧的页内容**：
 *       从 pager_get / pager_alloc 拿到帧，一直持有到 pager_unpin。
 *       同一时刻只有一个线程能改这一页的内容。
 *
 * 【为什么内容锁要"get 到 unpin"这么久】
 *
 * 拿页的路径是：
 *
 *     pid = ...;
 *     pager_get(pager, pid, &f);    ← 拿到内存里的这一页
 *     page_insert(f->data, ...);    ← 直接改它的内容
 *     pager_unpin(pager, pid, true);
 *
 * 从 get 到 unpin 之间，页的内容是**借给调用方**的。
 * 这段时间里如果另一个线程把这一帧逐出并载入了别的页，
 * 调用方手里的指针就指向了**别的页的内容** —— 写进去的数据张冠李戴，
 * 而且没有任何报错。所以内容锁必须覆盖整个借用期。
 *
 * 【内容锁是递归的】
 *
 * 同一线程可能对同一页 get 两次（比如 B+tree 先取父页再取子页，
 * 中间又回头看一眼父页）。不可重入的锁会自己把自己锁死。
 *
 * 【锁的全序】
 *
 * 内容锁与池级锁都属于 frame 域（序号 3）。
 * WAL 钩子是在**持有内容锁**的时候调的 →
 * 锁序是 frame(3) → wal(4)，符合全序 ✓。
 *
 * 【死锁怎么排除的：一条不变式】
 *
 *     **持有内容锁 ⇒ 该帧的 pin_count ≥ 1**
 *
 * 由它推出两条：
 *
 *   1. pin_count == 0 的帧，**内容锁一定是空的**
 *      → 逐出路径（持池级锁 → 取受害者内容锁）不会阻塞在内容锁上
 *      → 不存在"池级锁等内容锁、内容锁持有者等池级锁"的环
 *   2. 逐出路径只可能挑 pin_count == 0 的帧（Clock 的规矩），
 *      所以内容锁的持有者不可能是"正在被逐出的那一帧"
 *
 * 另外，**任何等待池级锁的路径都不持有内容锁**，
 * 唯一例外是 unpin —— 而 unpin 里持锁的顺序是
 * "先查表（池级锁）→ 放内容锁 → 减 pin_count（池级锁）"，
 * 它在等池级锁时**还没持有内容锁** ✓。
 *
 * 【为什么不把 IO 挪出池级锁】
 *
 * 逐出路径要读盘/写盘，现在是在池级锁里做的 ——
 * 临界区偏长，但换来的是"逐出和载入是原子的"。
 * 步骤 4d 退役大锁之后会重新审视这里的并发度。
 */

#include "storage/pager.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/latch.h"
#include "storage/page.h"

/* 4GB 以上文件必须用 64 位偏移的 fseek。MinGW 用 _fseeki64。 */
#ifdef _WIN32
#define FSEEK _fseeki64
#define FTELL _ftelli64
#else
#define FSEEK fseeko
#define FTELL ftello
#endif

struct Pager {
    FILE       *f;
    u64         npages;      /* 页数 = 文件大小 / PAGE_SIZE */
    u32         nframes;
    u32         hand;        /* Clock 手 */
    PageFrame  *frames;
    pthread_mutex_t  mu;     /* 池级锁：哈希表 / Clock 手 / pin_count / npages */
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

/* ---------------- 两把锁 ---------------- */

/*
 * 池级锁 —— frame 域（序号 3）。
 *
 * 它保护的是"整个池子共用的账本"，所以它和内容锁同属一个域。
 */
static void pool_lock(Pager *pg)
{
    pthread_mutex_lock(&pg->mu);
    latch_order_acquire(LATCH_FRAME);
}

static void pool_unlock(Pager *pg)
{
    latch_order_release(LATCH_FRAME);
    pthread_mutex_unlock(&pg->mu);
}

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
static void frame_lock(Pager *pg, u32 fi)
{
    /*
     * 先拿锁，再记 owner。
     *
     * 顺序反了会出这种错：线程 A 先写 owner=A，
     * 线程 B 也写 owner=B（还没拿到锁），A 就再也对不上了。
     */
    pthread_mutex_lock(&pg->latches[fi]);
    if (pg->latch_depth[fi] == 0)
        pg->latch_owner[fi] = pthread_self();
    pg->latch_depth[fi]++;
    latch_order_acquire(LATCH_FRAME);
}

/*
 * 释放内容锁。
 *
 * 返回 E_INVAL 表示"这把锁不是当前线程拿的" ——
 * 那是调用方的 bug（unpin 了一个自己没 get 过的页）。
 * 这种情况**绝不能真的去 unlock**：POSIX 里释放非自己持有的锁是未定义行为，
 * 可能把锁的状态搅坏。
 */
static int frame_unlock(Pager *pg, u32 fi)
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

static void hash_init(Pager *pg)
{
    pg->hash_cap = next_pow2(pg->nframes * 2);   /* 装填因子 <= 0.5 */
    pg->hash_keys = malloc(pg->hash_cap * sizeof(u32));
    pg->hash_vals = malloc(pg->hash_cap * sizeof(u32));
    for (u32 i = 0; i < pg->hash_cap; i++)
        pg->hash_keys[i] = KEY_EMPTY;
}

/* 以下哈希函数都必须在池级锁下调用 */

static bool hash_find(Pager *pg, u32 key, u32 *val)
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

/* ---------------- 文件读写 ---------------- */

static int file_size(FILE *f, u64 *out)
{
    if (FSEEK(f, 0, SEEK_END) != 0)
        return E_IO;
    long long sz = (long long)FTELL(f);
    if (sz < 0)
        return E_IO;
    *out = (u64)sz;
    return OK;
}

static int read_page(Pager *pg, u32 page_id, u8 *buf)
{
    long long off = (long long)((u64)page_id * PAGE_SIZE);
    if (FSEEK(pg->f, off, SEEK_SET) != 0)
        return E_IO;
    size_t n = fread(buf, 1, PAGE_SIZE, pg->f);
    if (n == PAGE_SIZE)
        return OK;
    if (n == 0) {
        /* 读到 EOF：分配过但还没刷盘的新页，逻辑上是全零页 */
        memset(buf, 0, PAGE_SIZE);
        return OK;
    }
    return E_CORRUPT;   /* 半页：文件被外部动过 */
}

static int write_page(Pager *pg, u32 page_id, const u8 *buf)
{
    long long off = (long long)((u64)page_id * PAGE_SIZE);
    if (FSEEK(pg->f, off, SEEK_SET) != 0)
        return E_IO;
    if (fwrite(buf, 1, PAGE_SIZE, pg->f) != PAGE_SIZE)
        return E_IO;
    return OK;
}

/*
 * 刷一个脏页。
 *
 * 刷之前先问钩子："这个页的 page_lsn 对应的日志落盘了吗？"
 * 没落盘就刷 = 违反 WAL 规则 = 崩溃后这段数据回不来。
 *
 * 这就是 pager.c 里原来那个 TODO 的答案 —— 钩子的职责不是"报错"，
 * 而是"把该落盘的东西落盘"（db 层的实现就是 wal_fsync）。
 *
 * 调用时**必须持有该帧的内容锁** ——
 * 这样"WAL 先落盘、页后落盘"这条规则是在内容锁的保护下完成的，
 * 而且锁序是 frame(3) → wal(4) ✓。
 */
static int flush_page(Pager *pg, u32 page_id, const u8 *buf)
{
    if (pg->wal_flush) {
        u32 lsn = get_u32(buf + H_LSN);
        int rc = pg->wal_flush(pg->wal_ctx, page_id, lsn);
        if (rc != OK)
            return rc;      /* 日志没落盘，不许刷 */
    }
    return write_page(pg, page_id, buf);
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
        if (f->pin_count == 0) {
            if (f->clock_ref == 0) {
                *victim = pg->hand;
                pg->hand = (pg->hand + 1) % pg->nframes;
                return OK;
            }
            f->clock_ref = 0;   /* 第二次机会用掉了 */
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
static int fetch_frame(Pager *pg, u32 page_id, PageFrame **out)
{
    int rc;

    pool_lock(pg);

    /* 命中：pin 住直接给 */
    u32 fi;
    if (hash_find(pg, page_id, &fi)) {
        PageFrame *f = &pg->frames[fi];
        f->pin_count++;
        f->clock_ref = 1;

        /*
         * 内容锁在**池级锁之外**拿。
         *
         * 反过来（持池级锁等内容锁）会死锁：
         *   线程 A 持池级锁，等页 P 的内容锁；
         *   线程 B 持 P 的内容锁（还没 unpin），等池级锁减 pin_count。
         * A 等 B，B 等 A。
         */
        pool_unlock(pg);
        frame_lock(pg, fi);
        *out = f;
        return OK;
    }

    /* 未命中：挑一个受害者 */
    rc = pick_victim(pg, &fi);
    if (rc != OK) {
        pool_unlock(pg);
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
    frame_lock(pg, fi);

    /* 受害者若还缓存着别的页：脏的先刷盘，再从哈希表摘掉 */
    if (f->page_id != PAGE_ID_NIL) {
        if (f->dirty) {
            /* WAL 钩子在这里被调 —— 锁序 frame → wal ✓ */
            rc = flush_page(pg, f->page_id, f->data);
            if (rc != OK) {
                frame_unlock(pg, fi);
                pool_unlock(pg);
                return rc;
            }
        }
        hash_del(pg, f->page_id);
        f->page_id = PAGE_ID_NIL;
        f->dirty = false;
    }

    rc = read_page(pg, page_id, f->data);
    if (rc != OK) {
        frame_unlock(pg, fi);
        pool_unlock(pg);
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
    if (hash_find(pg, page_id, &other)) {
        f->page_id = PAGE_ID_NIL;
        f->dirty = false;
        frame_unlock(pg, fi);

        PageFrame *of = &pg->frames[other];
        of->pin_count++;
        of->clock_ref = 1;
        pool_unlock(pg);
        frame_lock(pg, other);
        *out = of;
        return OK;
    }

    if (pg->hash_tombs > pg->hash_cap / 4)
        hash_rebuild(pg);   /* 墓碑回收，防探测死循环 */

    f->page_id = page_id;
    f->pin_count = 1;
    f->clock_ref = 1;
    /*
     * 帧被复用了：上一任主人留下的 rec_lsn 与新页无关。
     * 不清零的话，新页的第一次改动会继承别人的 recLSN →
     * 重放点被压低（安全方向，但白费功夫）。
     */
    f->rec_lsn = 0;
    hash_ins(pg, page_id, fi);

    /* 内容锁已经持有着（frame_lock 那一步），不用再拿一次 */
    pool_unlock(pg);
    *out = f;
    return OK;
}

/* ---------------- 公开 API ---------------- */

int pager_open(const char *path, u32 nframes, Pager **out)
{
    if (!path || nframes == 0 || !out)
        return E_INVAL;

    int rc = OK;

    FILE *f = fopen(path, "r+b");
    if (!f) {
        f = fopen(path, "wb");        /* 不存在则创建 */
        if (!f)
            return E_IO;
        fclose(f);
        f = fopen(path, "r+b");
        if (!f)
            return E_IO;
    }
    setvbuf(f, NULL, _IONBF, 0);      /* 我们整页读写，stdio 别再缓冲一层 */

    Pager *pg = calloc(1, sizeof(*pg));
    if (!pg) {
        rc = E_IO;
        goto fail;
    }
    pg->f = f;
    pg->nframes = nframes;

    if (pthread_mutex_init(&pg->mu, NULL) != 0) {
        rc = E_IO;
        goto fail;
    }

    u64 sz = 0;
    rc = file_size(f, &sz);
    if (rc != OK)
        goto fail;
    /* 尾部若有半页（上次写一半断了），先忽略，npages 向下取整。
     * 等 WAL 的 FPI 机制上线后再回头处理撕页。 */
    pg->npages = sz / PAGE_SIZE;

    pg->frames = calloc(nframes, sizeof(PageFrame));
    if (!pg->frames) {
        rc = E_IO;
        goto fail;
    }
    for (u32 i = 0; i < nframes; i++)
        pg->frames[i].page_id = PAGE_ID_NIL;

    /* 每帧一把内容锁：递归型（同线程可以重复拿同一页） */
    pg->latches = calloc(nframes, sizeof(pthread_mutex_t));
    pg->latch_owner = calloc(nframes, sizeof(pthread_t));
    pg->latch_depth = calloc(nframes, sizeof(int));
    if (!pg->latches || !pg->latch_owner || !pg->latch_depth) {
        rc = E_IO;
        goto fail;
    }
    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        for (u32 i = 0; i < nframes; i++)
            pthread_mutex_init(&pg->latches[i], &attr);
        pthread_mutexattr_destroy(&attr);
    }

    hash_init(pg);
    if (!pg->hash_keys || !pg->hash_vals) {
        rc = E_IO;
        goto fail;
    }

    *out = pg;
    return OK;

fail:
    if (pg) {
        if (pg->latches) {
            for (u32 i = 0; i < nframes; i++)
                pthread_mutex_destroy(&pg->latches[i]);
            free(pg->latches);
        }
        free(pg->latch_owner);
        free(pg->latch_depth);
        free(pg->frames);
        free(pg->hash_keys);
        free(pg->hash_vals);
        pthread_mutex_destroy(&pg->mu);
        free(pg);
    }
    fclose(f);
    return rc;
}

int pager_close(Pager *pg)
{
    if (!pg)
        return E_INVAL;
    int rc = pager_flush_all(pg);
    fclose(pg->f);
    for (u32 i = 0; i < pg->nframes; i++)
        pthread_mutex_destroy(&pg->latches[i]);
    free(pg->latches);
    free(pg->latch_owner);
    free(pg->latch_depth);
    free(pg->frames);
    free(pg->hash_keys);
    free(pg->hash_vals);
    pthread_mutex_destroy(&pg->mu);
    free(pg);
    return rc;
}

int pager_get(Pager *pg, u32 page_id, PageFrame **out)
{
    if (!pg || !out)
        return E_INVAL;
    pool_lock(pg);
    u64 np = pg->npages;
    pool_unlock(pg);
    if (page_id >= (u32)np)
        return E_NOTFOUND;
    return fetch_frame(pg, page_id, out);
}

int pager_alloc(Pager *pg, u32 *page_id, PageFrame **out)
{
    if (!pg || !page_id || !out)
        return E_INVAL;

    /*
     * 先占号再取帧。
     *
     * 占号必须在池级锁里做，而且要在取帧**之前** ——
     * 否则两个线程会拿到同一个页号。
     */
    pool_lock(pg);
    u32 id = (u32)pg->npages;
    pg->npages++;
    pool_unlock(pg);

    int rc = fetch_frame(pg, id, out);
    if (rc != OK) {
        /* 取帧失败：把号退回去（否则文件里会留下一个空洞） */
        pool_lock(pg);
        pg->npages--;
        pool_unlock(pg);
        return rc;
    }

    memset((*out)->data, 0, PAGE_SIZE);
    (*out)->dirty = true;    /* 新页视为已改，close 时落盘 */
    (*out)->rec_lsn = 0;     /* 新页还没有"第一次改动"的 LSN */
    *page_id = id;
    return OK;
}

int pager_unpin(Pager *pg, u32 page_id, bool dirty)
{
    if (!pg)
        return E_INVAL;

    pool_lock(pg);
    u32 fi;
    if (!hash_find(pg, page_id, &fi)) {
        pool_unlock(pg);
        return E_NOTFOUND;
    }
    PageFrame *f = &pg->frames[fi];
    if (f->pin_count <= 0) {
        pool_unlock(pg);
        return E_INVAL;      /* unpin 不配对：这是调用方的 bug */
    }
    if (dirty) {
        /*
         * 记 rec_lsn：**这一页自上次刷盘以来的第一次改动**的 LSN。
         *
         * 两种情况要重置：
         *   - 这一页之前是干净的 → 这次就是"自上次刷盘以来的第一次改动"
         *   - rec_lsn 还是 0（新分配的页 / 之前那次改动没写日志）
         */
        if (!f->dirty || f->rec_lsn == 0)
            f->rec_lsn = page_lsn(f->data);
        f->dirty = true;
    }
    pool_unlock(pg);

    /*
     * 关键顺序：**先放内容锁，再减 pin_count**。
     *
     * 反过来的话会出现一个窗口："pin_count 已经是 0，
     * 但内容锁还捏在手里"。这时候逐出线程会挑中这一帧，
     * 拿着池级锁去等这个内容锁 —— 而内容锁的持有者
     * （我们）正要去拿池级锁减 pin_count → 死锁。
     *
     * 现在这个顺序下，"持有内容锁 ⇒ pin_count ≥ 1" 恒成立 ✓
     */
    int rc = frame_unlock(pg, fi);
    if (rc != OK)
        return rc;

    pool_lock(pg);
    f->pin_count--;
    pool_unlock(pg);
    return OK;
}

int pager_flush_all(Pager *pg)
{
    if (!pg)
        return E_INVAL;
    for (u32 i = 0; i < pg->nframes; i++) {
        /*
         * 逐帧取内容锁，**不持池级锁** ——
         * 持着池级锁去等别人的内容锁 = 死锁。
         */
        frame_lock(pg, i);
        PageFrame *f = &pg->frames[i];
        if (f->page_id != PAGE_ID_NIL && f->dirty) {
            int rc = flush_page(pg, f->page_id, f->data);
            if (rc != OK) {
                frame_unlock(pg, i);
                return rc;
            }
            f->dirty = false;
            /*
             * 刷盘之后，这一页在盘上的内容就是最新的 ——
             * 下一次改动要重新记一个 recLSN（"自上次刷盘以来的第一次改动"）。
             * 不清零的话，recLSN 会一直停在刷盘前那个老值，
             * 重放点被压得很低 → 白白多重放一遍。
             */
            f->rec_lsn = 0;
        }
        frame_unlock(pg, i);
    }
    return OK;
}

void pager_set_wal_flush(Pager *pg, PagerFlushLogFn fn, void *ctx)
{
    if (!pg)
        return;
    pool_lock(pg);
    pg->wal_flush = fn;
    pg->wal_ctx = ctx;
    pool_unlock(pg);
}

/*
 * 取脏页里最小的 rec_lsn（checkpoint 的重放起点）。
 *
 * 【逐帧取内容锁，不持池级锁】
 * 与 pager_flush_all 同样的理由：持着池级锁去等别人的内容锁 = 死锁。
 *
 * rec_lsn == 0 的页被跳过 —— 那是"故意不写日志"的页
 * （向量索引），日志里没有它们的记录，不约束重放点。
 */
u32 pager_min_dirty_lsn(const Pager *pg)
{
    if (!pg)
        return 0;

    Pager *m = (Pager *)pg;     /* 取内容锁需要非 const 的 Pager */
    u32 min_lsn = 0;
    bool any = false;

    for (u32 i = 0; i < m->nframes; i++) {
        frame_lock(m, i);
        PageFrame *f = &m->frames[i];
        if (f->page_id != PAGE_ID_NIL && f->dirty && f->rec_lsn != 0) {
            if (!any || f->rec_lsn < min_lsn)
                min_lsn = f->rec_lsn;
            any = true;
        }
        frame_unlock(m, i);
    }

    if (!any)
        return 0;               /* 没有能约束重放点的脏页 */
    return min_lsn;
}

void pager_abandon(Pager *pg)
{
    if (!pg)
        return;
    fclose(pg->f);
    for (u32 i = 0; i < pg->nframes; i++)
        pthread_mutex_destroy(&pg->latches[i]);
    free(pg->latches);
    free(pg->latch_owner);
    free(pg->latch_depth);
    free(pg->frames);
    free(pg->hash_keys);
    free(pg->hash_vals);
    pthread_mutex_destroy(&pg->mu);
    free(pg);
}

int pager_extend_to(Pager *pg, u32 npages)
{
    if (!pg)
        return E_INVAL;
    pool_lock(pg);
    if ((u64)npages > pg->npages)
        pg->npages = npages;
    pool_unlock(pg);
    return OK;
}

u64 pager_npages(const Pager *pg)
{
    if (!pg)
        return 0;
    /* 见 freelist.c 的同类说明：const 是调用方视角的约束，
     * 锁的状态当然要变。 */
    Pager *m = (Pager *)pg;
    pool_lock(m);
    u64 n = pg->npages;
    pool_unlock(m);
    return n;
}

u32 pager_frames_used(const Pager *pg)
{
    if (!pg)
        return 0;
    Pager *m = (Pager *)pg;
    pool_lock(m);
    u32 n = 0;
    for (u32 i = 0; i < pg->nframes; i++)
        if (pg->frames[i].page_id != PAGE_ID_NIL)
            n++;
    pool_unlock(m);
    return n;
}
