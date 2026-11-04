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
 *   【池级锁 pg->mu（读写锁）】—— 管**帧的元数据**：
 *       哈希表（page_id → 帧下标）、Clock 手、pin_count、npages。
 *       这几样是"整个池子共用的账本"，必须互斥。
 *
 *   【内容锁 pg->latches[i]】—— 管**一帧的页内容**：
 *       从 pager_get / pager_alloc 拿到帧，一直持有到 pager_unpin。
 *       同一时刻只有一个线程能改这一页的内容。
 *
 * ==================== v1.2 步骤 7：池级锁从互斥量换成读写锁 ====================
 *
 * 【为什么换】
 *
 * 步骤 7 的 A3（细粒度 vs 大锁基线 ≥ 2×）实测只拿到 0.78x–1.01x，
 * gdb 采样里 4 个工作线程有 3 个堵在 pool_lock。
 * 根因不是 latch 分得不够细，而是**这把池级互斥量把所有页访问串成了一条线**：
 * 一次页访问要抢它 4 次（查 npages / 查哈希 / 查哈希 / 减 pin_count），
 * 4 个线程就排成一条队 —— 细粒度 latch 的收益全被它吃掉了。
 *
 * 【怎么换】
 *
 * 这把锁保护的是账本，访问方式**读多写少**：
 *   - 读：哈希查找（每一次页访问一次）—— 现在用**读锁**，读读不互斥
 *   - 写：逐出（摘映射 / 载页）、分配新页 —— 用**写锁**，独占
 *
 * 另外把两个纯计数器从锁里挪出来：
 *   - `npages` → 原子量（pager_get 的越界检查不再碰锁）
 *   - `pin_count` → 原子量（pager_unpin 的减一不再碰锁）
 *
 * 效果：一次页访问从 **4 次全局锁** 降到 **2 次读锁**，
 * 而读锁之间**不互斥** —— 多线程终于能并行跑。
 *
 * 【为什么这样还是安全的】
 *
 * 不变式没变：
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
 * 读锁与写锁互斥 ⇒ 逐出路径挑受害者时，
 * 绝不会挑到一个"刚被读者加过 pin"却还没反映出来的帧 ✓。
 *
 * 【逐出路径的 IO 还在写锁里】
 *
 * 逐出要读盘/写盘，现在仍在写锁里做 ——
 * 临界区偏长，但换来的是"逐出和载入是原子的"。
 * 命中路径不再被它挡住（读锁与写锁互斥，但逐出本身很少发生）。
 * 把 IO 挪出写锁是下一步的事（需要把"帧在途"这件事显式化）。
 *
 * 【锁的全序】
 *
 * 内容锁与池级锁都属于 frame 域（序号 3）。
 * WAL 钩子是在**持有内容锁**的时候调的 →
 * 锁序是 frame(3) → wal(4)，符合全序 ✓。
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

#include "storage/pager_internal.h"

#include <stdlib.h>

#include "common/latch.h"
#include "common/lockstat.h"
#include "storage/page.h"

/* 4GB 以上文件必须用 64 位偏移的 fseek。MinGW 用 _fseeki64。 */
#ifdef _WIN32
#define FSEEK _fseeki64
#define FTELL _ftelli64
#else
#define FSEEK fseeko
#define FTELL ftello
#endif

/* struct Pager 的定义在 pager_internal.h（pager.c 与 pager_frame.c 共享） */

/* ---------------- 池级锁 ---------------- */

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
 *
 * （内容锁在 pager_frame.c 里，两把锁的分工见本文件头部的说明。）
 */
void pager_pool_rdlock(Pager *pg)
{
    LS_ENTER(LS_POOL);
    db_rwlock_rdlock(&pg->mu);
    LS_LEAVE(LS_POOL);
    latch_order_acquire(LATCH_FRAME);
}

void pager_pool_rdunlock(Pager *pg)
{
    latch_order_release(LATCH_FRAME);
    db_rwlock_rdunlock(&pg->mu);
}

void pager_pool_wrlock(Pager *pg)
{
    db_rwlock_wrlock(&pg->mu);
    latch_order_acquire(LATCH_FRAME);
}

void pager_pool_wrunlock(Pager *pg)
{
    latch_order_release(LATCH_FRAME);
    db_rwlock_wrunlock(&pg->mu);
}

/* 旧名保留：改账本的地方一律用它（等价于写锁） */
void pager_pool_lock(Pager *pg)
{
    pager_pool_wrlock(pg);
}

void pager_pool_unlock(Pager *pg)
{
    pager_pool_wrunlock(pg);
}

/* 内容锁（pager_frame_lock / pager_frame_unlock）在 pager_frame.c 里 */

/* 帧哈希表（pager_hash_init / pager_hash_find）在 pager_frame.c 里 */

/* ---------------- 度量计数器（v1.3 欠账 6） ---------------- */

/*
 * 保证 phys_epoch 装得下 page_id。
 *
 * 必须在**持有池级写锁**时调用 —— 唯一的调用方（读盘路径）本来就持着。
 * 扩不动时返回错误：调用方要能容忍（计数照记，只是这一页去重不上）。
 */
static int pager_phys_epoch_grow(Pager *pg, u32 page_id)
{
    u32 need = page_id + 1u;
    u32 cap = pg->phys_epoch_cap;
    u32 *p;

    if (need <= cap)
        return OK;
    if (cap == 0)
        cap = 1024u;                    /* 先给 1024 页（4 KB 表） */
    while (cap < need) {
        if (cap > 0x20000000u)          /* 8 亿页 = 3 TB，别再扩了 */
            return E_NOSPACE;
        cap *= 2u;
    }
    p = (u32 *)realloc(pg->phys_epoch, (size_t)cap * sizeof(u32));
    if (p == NULL)
        return E_IO;
    /* 新扩出来的部分是 0 = "从没被读盘过" */
    memset(p + pg->phys_epoch_cap, 0,
           (size_t)(cap - pg->phys_epoch_cap) * sizeof(u32));
    pg->phys_epoch = p;
    pg->phys_epoch_cap = cap;
    return OK;
}

/*
 * 记一次"真正从文件读回了一页"。
 *
 * 调用点：pager_read_page 里 fread 读满 PAGE_SIZE 的那一条分支。
 * 必须在持有池级写锁时调用（唯一的调用方 pager_fetch_frame 就持着）。
 */
static void pager_note_physical_read(Pager *pg, u32 page_id)
{
    u32 prev;

    atomic_fetch_add(&pg->physical_reads, 1);

    if (pager_phys_epoch_grow(pg, page_id) != OK)
        return;     /* 扩不动：物理读盘照记，只是这一页去重不上 */

    prev = pg->phys_epoch[page_id];
    if (prev == 0)
        pg->unique_total++;        /* 这一页有史以来第一次被读盘 */
    if (prev != pg->epoch) {
        pg->phys_epoch[page_id] = pg->epoch;
        pg->unique_window++;       /* 这一页在本窗口里第一次被读盘 */
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

int pager_read_page(Pager *pg, u32 page_id, u8 *buf)
{
    long long off = (long long)((u64)page_id * PAGE_SIZE);
    if (FSEEK(pg->f, off, SEEK_SET) != 0)
        return E_IO;
    size_t n = fread(buf, 1, PAGE_SIZE, pg->f);
    if (n == PAGE_SIZE) {
        /*
         * 真的从文件读回了一页 —— 这才是一次**物理读盘**（v1.3 欠账 6）。
         *
         * 这里是全库唯一一处"数据真的从盘上来了"的地方，
         * 所以物理读盘的计数点就放在这里（而不是 pager_get ——
         * 那里数的是逻辑访问，帧池命中时根本没碰盘）。
         */
        pager_note_physical_read(pg, page_id);
        return OK;
    }
    if (n == 0) {
        /* 读到 EOF：分配过但还没刷盘的新页，逻辑上是全零页 */
        memset(buf, 0, PAGE_SIZE);
        /*
         * 不计入物理读盘：文件里根本还没有这一页，
         * 这一次没有从盘上取回任何数据。
         * （否则"分配页"会被算成读盘，把物理 IO 的数字凭空抬高。）
         */
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
int pager_flush_page(Pager *pg, u32 page_id, const u8 *buf)
{
    if (pg->wal_flush) {
        u32 lsn = get_u32(buf + H_LSN);
        int rc = pg->wal_flush(pg->wal_ctx, page_id, lsn);
        if (rc != OK)
            return rc;      /* 日志没落盘，不许刷 */
    }
    return write_page(pg, page_id, buf);
}

/* Clock 逐出（pick_victim）与取帧（pager_fetch_frame）在 pager_frame.c 里 */

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
    pg->epoch = 1u;      /* 测量窗口从 1 开始；0 保留作"这页从没被读盘过" */

    db_rwlock_init(&pg->mu);

    u64 sz = 0;
    rc = file_size(f, &sz);
    if (rc != OK)
        goto fail;
    /* 尾部若有半页（上次写一半断了），先忽略，npages 向下取整。
     * 等 WAL 的 FPI 机制上线后再回头处理撕页。 */
    atomic_store(&pg->npages, sz / PAGE_SIZE);

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

    pager_hash_init(pg);
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
        db_rwlock_destroy(&pg->mu);
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
    free(pg->phys_epoch);
    db_rwlock_destroy(&pg->mu);
    free(pg);
    return rc;
}

int pager_get(Pager *pg, u32 page_id, PageFrame **out)
{
    if (!pg || !out)
        return E_INVAL;
    /* npages 是原子量 → 越界检查**完全不碰锁** ✓ */
    u64 np = atomic_load(&pg->npages);
    if (page_id >= (u32)np)
        return E_NOTFOUND;
    int rc = pager_fetch_frame(pg, page_id, out);
    if (rc == OK) {
        /*
         * 逻辑页读 +1（v1.3 欠账 6）。
         *
         * **帧池命中也计数** —— 这正是"逻辑读"与"物理读盘"的区别：
         * 命中时这次取页照样是一次逻辑访问，但它没碰盘，
         * 所以不会进物理读盘的计数。
         */
        atomic_fetch_add(&pg->logical_reads, 1);
    }
    return rc;
}

int pager_alloc(Pager *pg, u32 *page_id, PageFrame **out)
{
    u32 id;
    bool reused = false;

    if (!pg || !page_id || !out)
        return E_INVAL;

    /*
     * 先问**全局空闲栈**（v1.5 步骤 4 · 判决 4）。
     *
     * 有空闲页就复用 —— 页号空间是共享的，回收的页该给谁就给谁。
     * 没有才在文件尾追加（原来的行为）。
     */
    if (pager_free_pop(pg, &id)) {
        reused = true;
    } else {
        /*
         * 先占号再取帧。
         *
         * 占号必须在取帧**之前** —— 否则两个线程会拿到同一个页号。
         * 用原子 fetch_add 一次搞定"读号 + 加一"，不用碰锁 ✓
         */
        id = (u32)atomic_fetch_add(&pg->npages, 1);
    }

    int rc = pager_fetch_frame(pg, id, out);
    if (rc != OK) {
        if (!reused) {
            /* 取帧失败：把号退回去（否则文件里会留下一个空洞） */
            atomic_fetch_sub(&pg->npages, 1);
        } else {
            /* 复用的页取不到帧 ⇒ 放回空闲栈，别把它弄丢了 */
            (void)pager_free(pg, id);
        }
        return rc;
    }
    /* 分配也是一次"取页" —— 逻辑页读要算它（v1.3 欠账 6） */
    atomic_fetch_add(&pg->logical_reads, 1);

    memset((*out)->data, 0, PAGE_SIZE);
    (*out)->dirty = true;    /* 新页视为已改，close 时落盘 */
    (*out)->rec_lsn = 0;     /* 新页还没有"第一次改动"的 LSN */
    *page_id = id;
    return OK;
}

/* 空闲栈（pager_free / pager_free_count）在 pager_free.c 里实现 */

int pager_unpin(Pager *pg, u32 page_id, bool dirty)
{
    if (!pg)
        return E_INVAL;

    /*
     * 查哈希表用**读锁**（读读不互斥）。
     *
     * dirty / rec_lsn 的更新不用池级锁保护 ——
     * 它们由**内容锁**保护，而内容锁此刻就在我们手里
     * （pager_unpin 的调用方一直持有到我们 pager_frame_unlock）。
     */
    pager_pool_rdlock(pg);
    u32 fi;
    if (!pager_hash_find(pg, page_id, &fi)) {
        pager_pool_rdunlock(pg);
        return E_NOTFOUND;
    }
    PageFrame *f = &pg->frames[fi];
    if (atomic_load(&f->pin_count) <= 0) {
        pager_pool_rdunlock(pg);
        return E_INVAL;      /* unpin 不配对：这是调用方的 bug */
    }
    pager_pool_rdunlock(pg);

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

    /*
     * 关键顺序：**先放内容锁，再减 pin_count**。
     *
     * 反过来的话会出现一个窗口："pin_count 已经是 0，
     * 但内容锁还捏在手里"。这时候逐出线程会挑中这一帧，
     * 拿着写锁去等这个内容锁 —— 而内容锁的持有者
     * （我们）正要去拿写锁减 pin_count → 死锁。
     *
     * 现在这个顺序下，"持有内容锁 ⇒ pin_count ≥ 1" 恒成立 ✓
     *
     * 减一用**原子操作**，不再碰池级锁 ——
     * 这样一次页访问的池级锁次数从 4 次降到 2 次（读锁）✓
     */
    int rc = pager_frame_unlock(pg, fi);
    if (rc != OK)
        return rc;

    atomic_fetch_sub(&f->pin_count, 1);
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
        pager_frame_lock(pg, i);
        PageFrame *f = &pg->frames[i];
        if (f->page_id != PAGE_ID_NIL && f->dirty) {
            int rc = pager_flush_page(pg, f->page_id, f->data);
            if (rc != OK) {
                pager_frame_unlock(pg, i);
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
        pager_frame_unlock(pg, i);
    }
    return OK;
}

void pager_set_wal_flush(Pager *pg, PagerFlushLogFn fn, void *ctx)
{
    if (!pg)
        return;
    pager_pool_lock(pg);
    pg->wal_flush = fn;
    pg->wal_ctx = ctx;
    pager_pool_unlock(pg);
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
        pager_frame_lock(m, i);
        PageFrame *f = &m->frames[i];
        if (f->page_id != PAGE_ID_NIL && f->dirty && f->rec_lsn != 0) {
            if (!any || f->rec_lsn < min_lsn)
                min_lsn = f->rec_lsn;
            any = true;
        }
        pager_frame_unlock(m, i);
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
    free(pg->phys_epoch);
    free(pg->free_ids);
    db_rwlock_destroy(&pg->mu);
    free(pg);
}

int pager_extend_to(Pager *pg, u32 npages)
{
    if (!pg)
        return E_INVAL;
    /* 只增不减：用 CAS 循环，npage 更小的时候不动它 */
    u64 cur = atomic_load(&pg->npages);
    while ((u64)npages > cur) {
        if (atomic_compare_exchange_weak(&pg->npages, &cur, (u64)npages))
            break;
        /* cur 已被刷新为最新值，循环重试 */
    }
    return OK;
}

u64 pager_npages(const Pager *pg)
{
    if (!pg)
        return 0;
    /* npages 是原子量 → 读它不需要锁 */
    return atomic_load(&pg->npages);
}

u32 pager_frames_used(const Pager *pg)
{
    if (!pg)
        return 0;
    /* 见 freelist.c 的同类说明：const 是调用方视角的约束，
     * 锁的状态当然要变。 */
    Pager *m = (Pager *)pg;
    pager_pool_rdlock(m);
    u32 n = 0;
    for (u32 i = 0; i < m->nframes; i++)
        if (m->frames[i].page_id != PAGE_ID_NIL)
            n++;
    pager_pool_rdunlock(m);
    return n;
}

/* ---------------- 度量计数器（v1.3 欠账 6）的公开 API ---------------- */

u64 pager_logical_reads(const Pager *pg)
{
    if (!pg)
        return 0;
    /* 原子量 → 读它不需要锁 */
    return atomic_load(&pg->logical_reads);
}

u64 pager_physical_reads(const Pager *pg)
{
    if (!pg)
        return 0;
    return atomic_load(&pg->physical_reads);
}

u64 pager_unique_pages(const Pager *pg)
{
    if (!pg)
        return 0;
    /* unique_window 由读盘路径在**写锁**下改 → 这里取读锁读它才一致 */
    Pager *m = (Pager *)pg;
    pager_pool_rdlock(m);
    u64 n = m->unique_window;
    pager_pool_rdunlock(m);
    return n;
}

u64 pager_unique_pages_total(const Pager *pg)
{
    if (!pg)
        return 0;
    Pager *m = (Pager *)pg;
    pager_pool_rdlock(m);
    u64 n = m->unique_total;
    pager_pool_rdunlock(m);
    return n;
}

void pager_stats_reset(Pager *pg)
{
    if (!pg)
        return;

    /*
     * 写锁：要把"本窗口去重"的计数清掉，并把窗口号加一。
     *
     * 【为什么窗口号加一而不是清表】
     *
     * 清表会把"全程去重"也一起清掉 —— 那个数是**数据足迹**，
     * 跨窗口累加才有意义。清表等于把它抹了。
     * 加窗口号则两全其美：
     *   - 旧窗口读过的页，phys_epoch[pid] != 新窗口号 → 不算进新窗口
     *   - 但它们 phys_epoch[pid] != 0 → 仍然算在"全程去重"里 ✓
     */
    pager_pool_wrlock(pg);
    atomic_store(&pg->logical_reads, 0);
    atomic_store(&pg->physical_reads, 0);
    pg->unique_window = 0;
    if (pg->epoch == 0xFFFFFFFFu) {
        /*
         * 窗口号用完了（4 亿次重置才会到这里）：清表重来。
         * 表一清，"全程去重"的信息也就没了 —— 只好一起归零。
         */
        if (pg->phys_epoch != NULL) {
            memset(pg->phys_epoch, 0,
                   (size_t)pg->phys_epoch_cap * sizeof(u32));
        }
        pg->epoch = 1u;
        pg->unique_total = 0;
    } else {
        pg->epoch++;
    }
    pager_pool_wrunlock(pg);
}
