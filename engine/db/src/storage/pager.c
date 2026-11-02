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
 */

#include "storage/pager.h"

#include <stdio.h>
#include <stdlib.h>

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
    u32        *hash_keys;   /* page_id -> frame 下标 */
    u32        *hash_vals;
    u32         hash_cap;    /* 2 的幂 */
    u32         hash_tombs;  /* 墓碑数。会无限累积，
                                不回收的话探测序列总有一天遇不到 EMPTY，
                                hash_find 就死循环了 —— 写 1 万页必触发 */
    PagerFlushLogFn wal_flush;   /* WAL 规则：刷页前保证日志落盘 */
    void          *wal_ctx;
};

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

static int fetch_frame(Pager *pg, u32 page_id, PageFrame **out)
{
    int rc;

    /* 命中：pin 住直接给 */
    u32 fi;
    if (hash_find(pg, page_id, &fi)) {
        PageFrame *f = &pg->frames[fi];
        f->pin_count++;
        f->clock_ref = 1;
        *out = f;
        return OK;
    }

    /* 未命中：挑一个受害者 */
    rc = pick_victim(pg, &fi);
    if (rc != OK)
        return rc;

    PageFrame *f = &pg->frames[fi];

    /* 受害者若还缓存着别的页：脏的先刷盘，再从哈希表摘掉 */
    if (f->page_id != PAGE_ID_NIL) {
        if (f->dirty) {
            rc = flush_page(pg, f->page_id, f->data);
            if (rc != OK)
                return rc;
        }
        hash_del(pg, f->page_id);
        f->page_id = PAGE_ID_NIL;
        f->dirty = false;
    }

    rc = read_page(pg, page_id, f->data);
    if (rc != OK)
        return rc;

    if (pg->hash_tombs > pg->hash_cap / 4)
        hash_rebuild(pg);   /* 墓碑回收，防探测死循环 */

    f->page_id = page_id;
    f->pin_count = 1;
    f->clock_ref = 1;
    hash_ins(pg, page_id, fi);
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

    hash_init(pg);
    if (!pg->hash_keys || !pg->hash_vals) {
        rc = E_IO;
        goto fail;
    }

    *out = pg;
    return OK;

fail:
    if (pg) {
        free(pg->frames);
        free(pg->hash_keys);
        free(pg->hash_vals);
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
    free(pg->frames);
    free(pg->hash_keys);
    free(pg->hash_vals);
    free(pg);
    return rc;
}

int pager_get(Pager *pg, u32 page_id, PageFrame **out)
{
    if (!pg || !out)
        return E_INVAL;
    if (page_id >= (u32)pg->npages)
        return E_NOTFOUND;
    return fetch_frame(pg, page_id, out);
}

int pager_alloc(Pager *pg, u32 *page_id, PageFrame **out)
{
    if (!pg || !page_id || !out)
        return E_INVAL;
    u32 id = (u32)pg->npages;
    int rc = fetch_frame(pg, id, out);
    if (rc != OK)
        return rc;
    memset((*out)->data, 0, PAGE_SIZE);
    (*out)->dirty = true;    /* 新页视为已改，close 时落盘 */
    pg->npages++;
    *page_id = id;
    return OK;
}

int pager_unpin(Pager *pg, u32 page_id, bool dirty)
{
    if (!pg)
        return E_INVAL;
    u32 fi;
    if (!hash_find(pg, page_id, &fi))
        return E_NOTFOUND;
    PageFrame *f = &pg->frames[fi];
    if (f->pin_count <= 0)
        return E_INVAL;      /* unpin 不配对：这是调用方的 bug */
    f->pin_count--;
    if (dirty)
        f->dirty = true;
    return OK;
}

int pager_flush_all(Pager *pg)
{
    if (!pg)
        return E_INVAL;
    for (u32 i = 0; i < pg->nframes; i++) {
        PageFrame *f = &pg->frames[i];
        if (f->page_id == PAGE_ID_NIL || !f->dirty)
            continue;
        int rc = flush_page(pg, f->page_id, f->data);
        if (rc != OK)
            return rc;
        f->dirty = false;
    }
    return OK;
}

void pager_set_wal_flush(Pager *pg, PagerFlushLogFn fn, void *ctx)
{
    if (!pg)
        return;
    pg->wal_flush = fn;
    pg->wal_ctx = ctx;
}

void pager_abandon(Pager *pg)
{
    if (!pg)
        return;
    fclose(pg->f);
    free(pg->frames);
    free(pg->hash_keys);
    free(pg->hash_vals);
    free(pg);
}

int pager_extend_to(Pager *pg, u32 npages)
{
    if (!pg)
        return E_INVAL;
    if ((u64)npages > pg->npages)
        pg->npages = npages;
    return OK;
}

u64 pager_npages(const Pager *pg)
{
    return pg ? pg->npages : 0;
}

u32 pager_frames_used(const Pager *pg)
{
    if (!pg)
        return 0;
    u32 n = 0;
    for (u32 i = 0; i < pg->nframes; i++)
        if (pg->frames[i].page_id != PAGE_ID_NIL)
            n++;
    return n;
}
