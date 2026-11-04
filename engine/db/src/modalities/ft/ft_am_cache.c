/*
 * ft_am_cache.c —— ft_docs 的**物化缓存**（v1.3 欠账 5）
 *
 * 【为什么要有缓存】
 *
 * ft_docs 每次扫描都要把**整个索引**物化一遍：
 * 逐段 → 逐词 → 逐 posting → 按 doc_id 聚合。
 * 那是 O(文档数) 的内存 + 读**全部**倒排页 ——
 * 连续扫两次就是白扫两遍。索引没变时，第二遍完全可以复用第一遍的行。
 *
 * 【失效判据：索引签名】
 *
 * 签名 = (ft_index_nsegs(), ft_index_ndocs(), ft_index_ntombstones())。
 * **写路径覆盖性论证**（为什么这个签名足够当失效判据）
 * 写在 **ft_am.c 顶部**的注释里 —— 那是本功能的设计说明。
 *
 * 【并发】
 *
 * 缓存由一把**全局互斥量**保护（g_cache_lock）。
 * 它是**叶子锁**：只在改缓存状态时持有，
 * 从不在持有它的时候去拿别的锁（物化在锁外做）——
 * 所以不会与 CATALOG → TXN → BTREE → FRAME → WAL → FREELIST 的锁序冲突。
 *
 * 引用计数的加减也在同一把锁里做（不是原子操作）——
 * 这样"引用数归零 ⇒ 释放"与"别人还在加引用"不会交错。
 *
 * 【泄漏兜底】
 *
 * 正常路径：索引/实例销毁时调 ft_am_ctx_release() 把缓存还掉。
 * 万一漏调（调用方没接上），进程退出时 atexit 钩子会把**所有还活着的缓冲**
 * 一并释放 —— 宁可进程退出时兜底，也不能让内存无限涨。
 *
 * 【为什么单独一个文件】
 *
 * ft_am.c 原本就 700 多行；把缓存塞进去会顶破 800 行的上限。
 * 所以缓存的实现放这里，ft_am.c 只留"怎么用"。
 */

#include "modalities/ft/ft_am.h"

#include <pthread.h>
#include <stdlib.h>

/* 索引签名：三个数合起来就是"索引有没有变过"的判据 */
typedef struct {
    u32 nsegs;
    u32 ndocs;
    u32 ntombs;
} FtAmSig;

/*
 * 缓存锁：进程级一把，保护所有实例的缓存状态与缓冲的引用计数。
 *
 * 它是**叶子锁**：持有它的时候不会再去拿别的锁。
 */
static pthread_mutex_t g_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static void ft_am_cache_lock(void)
{
    pthread_mutex_lock(&g_cache_lock);
}

static void ft_am_cache_unlock(void)
{
    pthread_mutex_unlock(&g_cache_lock);
}

/* ---------------- 缓冲的引用计数 ---------------- */

/*
 * 物化结果的持有者（引用计数）—— 类型定义在 ft_am.h。
 *
 * 【为什么要有引用计数】
 *
 * 缓存里的行是**共享**的：几个并发扫描可能同时在读同一份行。
 * 缓存失效时要把这份行丢掉，但正在扫的线程还在读它 ——
 * 所以谁在用谁拿一份引用，最后一个走的那个负责释放。
 * 没有引用计数就会变成"用已释放的内存"（坑 M4-29）。
 */

/*
 * 还活着的缓冲的全局链表（泄漏兜底用）。
 *
 * 正常情况下这个链表是空的（缓存释放后就把缓冲摘掉了）。
 */
static FtDocBuf *g_live_bufs = NULL;

/* atexit 钩子是否已经挂上（避免重复注册） */
static bool g_atexit_hooked = false;

static void ft_am_atexit_cleanup(void)
{
    FtDocBuf *b;

    ft_am_cache_lock();
    b = g_live_bufs;
    g_live_bufs = NULL;
    ft_am_cache_unlock();

    while (b != NULL) {
        FtDocBuf *next = b->next;

        free(b);
        b = next;
    }
}

/*
 * 新建一个物化缓冲（引用数 1 ⇒ 归创建者所有）。
 *
 * 物化在 ft_am.c 里做（ft_am_materialize_index），
 * 所以"造缓冲"也放这里共享：ft_am.c 造，这里计数与释放。
 */
FtDocBuf *ft_am_buf_new(u32 nrows)
{
    FtDocBuf *b = (FtDocBuf *)malloc(sizeof(FtDocBuf) +
                                     (size_t)(nrows > 0 ? nrows - 1 : 0) *
                                     sizeof(FtDocRow));

    if (b == NULL) {
        return NULL;
    }
    b->refcnt = 1;   /* 归创建者所有 */
    b->nrows = nrows;

    ft_am_cache_lock();
    if (!g_atexit_hooked) {
        g_atexit_hooked = true;
        atexit(ft_am_atexit_cleanup);
    }
    b->next = g_live_bufs;
    g_live_bufs = b;
    ft_am_cache_unlock();
    return b;
}

/* 加引用（**调用时必须持有缓存锁**） */
static void ft_am_buf_ref_locked(FtDocBuf *b)
{
    if (b != NULL) {
        b->refcnt++;
    }
}

/* 减引用；归零 ⇒ 从链表摘掉并真正释放 */
static void ft_am_buf_unref(FtDocBuf *b)
{
    if (b == NULL) {
        return;
    }
    ft_am_cache_lock();
    if (b->refcnt > 0) {
        b->refcnt--;
    }
    if (b->refcnt == 0) {
        FtDocBuf **pp = &g_live_bufs;

        while (*pp != NULL) {
            if (*pp == b) {
                *pp = b->next;
                break;
            }
            pp = &(*pp)->next;
        }
        ft_am_cache_unlock();
        free(b);
        return;
    }
    ft_am_cache_unlock();
}

/* ---------------- 索引签名 ---------------- */

/*
 * 取索引签名。
 *
 * 签名 = (段数, 文档数之和, tombstone 数)。
 * **写路径覆盖性论证**（为什么这个签名足够当失效判据）写在
 * ft_am.c 顶部的注释里 —— 那是本功能的设计说明，读者从 ft_am.c 进来看。
 */
static FtAmSig ft_am_sig(const FtIndex *ix)
{
    FtAmSig s;

    s.nsegs = ft_index_nsegs(ix);
    s.ndocs = ft_index_ndocs(ix);
    s.ntombs = ft_index_ntombstones(ix);
    return s;
}

static bool ft_am_sig_eq(FtAmSig a, FtAmSig b)
{
    return a.nsegs == b.nsegs && a.ndocs == b.ndocs && a.ntombs == b.ntombs;
}

/*
 * 把缓存那份引用摘下来（**调用时持有缓存锁**）。
 *
 * 返回被摘下来的旧缓冲 —— 调用方在**解锁之后**再 unref
 * （缓存里的行可能还有扫描在用 → 延迟到最后一个扫描结束才真正释放）。
 */
static FtDocBuf *ft_am_cache_take_locked(FtAmCtx *c)
{
    FtDocBuf *old = (FtDocBuf *)c->cache;

    c->cache = NULL;
    c->cache_valid = false;
    return old;
}

/* ---------------- 取行 ---------------- */

/*
 * 取"可以用的行"。
 *
 * 命中缓存 → 直接借用缓存里的行（不重新物化）；
 * 没命中   → 物化一遍，并把结果放进缓存。
 *
 * 返回的行是**借来的**：调用方必须在用完之后调 ft_am_rows_release()。
 */
int ft_am_rows_acquire(FtAmCtx *c, FtDocBuf **buf_out,
                        FtDocRow **rows_out, u32 *n_out)
{
    FtAmSig sig;
    FtAmSig sig2;
    FtDocBuf *buf = NULL;
    FtDocBuf *old = NULL;
    int attempt;

    for (attempt = 0; attempt < 3; attempt++) {
        /* ---- 1) 查缓存 ---- */
        ft_am_cache_lock();
        sig = ft_am_sig(c->ix);
        if (c->cache_valid && c->cache != NULL &&
            sig.nsegs == c->sig_nsegs &&
            sig.ndocs == c->sig_ndocs &&
            sig.ntombs == c->sig_ntombs) {
            buf = (FtDocBuf *)c->cache;
            ft_am_buf_ref_locked(buf);
            c->n_hit++;
            ft_am_cache_unlock();
            *buf_out = buf;
            *rows_out = buf->rows;
            *n_out = buf->nrows;
            return OK;
        }
        /* 没命中：把缓存那份引用摘下来（解锁后再 unref） */
        old = ft_am_cache_take_locked(c);
        ft_am_cache_unlock();
        ft_am_buf_unref(old);

        /* ---- 2) 物化（锁外） ---- */
        {
            int rc = ft_am_materialize_index(c->ix, &buf);

            if (rc != OK) {
                /* 原样返回错误码（E_CORRUPT / E_NOSPACE 等）——不要一律改成 E_NOSPACE */
                return rc;
            }
        }
        ft_am_cache_lock();
        c->n_materialize++;
        ft_am_cache_unlock();

        /* ---- 3) 物化期间索引变了吗 ---- */
        sig2 = ft_am_sig(c->ix);
        if (!ft_am_sig_eq(sig, sig2)) {
            if (attempt == 2) {
                /*
                 * 一直在变：这次的结果直接用（不入缓存）。
                 * 这一份引用就归调用方了 —— 扫描结束时会还掉。
                 */
                *buf_out = buf;
                *rows_out = buf->rows;
                *n_out = buf->nrows;
                return OK;
            }
            ft_am_buf_unref(buf);
            buf = NULL;
            continue;      /* 重来一遍 */
        }

        /* ---- 4) 放进缓存 ---- */
        ft_am_cache_lock();
        old = ft_am_cache_take_locked(c);
        c->cache = buf;              /* 缓存接管这份引用 */
        c->cache_valid = true;
        c->sig_nsegs = sig.nsegs;
        c->sig_ndocs = sig.ndocs;
        c->sig_ntombs = sig.ntombs;
        ft_am_buf_ref_locked(buf);   /* 扫描自己也要一份 */
        ft_am_cache_unlock();
        ft_am_buf_unref(old);

        *buf_out = buf;
        *rows_out = buf->rows;
        *n_out = buf->nrows;
        return OK;
    }
    return E_NOSPACE;
}

/* 用完行：放掉那一份引用（最后一个走的负责真正释放） */
void ft_am_rows_release(FtDocBuf *buf)
{
    ft_am_buf_unref(buf);
}

/* ---------------- 诊断接口 ---------------- */

u64 ft_am_stat_materialize(const FtAmCtx *ctx)
{
    return (ctx != NULL) ? ctx->n_materialize : 0;
}

u64 ft_am_stat_cache_hit(const FtAmCtx *ctx)
{
    return (ctx != NULL) ? ctx->n_hit : 0;
}

/*
 * 强制让缓存失效。
 *
 * 只清"缓存里那一份"，**不动**正在扫描的人手里借的那一份
 * （他们还引用着，最后一个走的会把它释放掉）。
 */
void ft_am_cache_invalidate(FtAmCtx *ctx)
{
    FtDocBuf *old;

    if (ctx == NULL || ctx->magic != FT_AM_CTX_MAGIC) {
        return;
    }
    ft_am_cache_lock();
    old = ft_am_cache_take_locked(ctx);
    ft_am_cache_unlock();
    ft_am_buf_unref(old);
}

/*
 * 释放上下文占用的缓存（索引/实例销毁时调）。
 *
 * 释放后缓存为空，计数器归零 —— 这个 ctx 若还要用，
 * 下次扫描会重新物化。
 */
void ft_am_ctx_release(FtAmCtx *ctx)
{
    if (ctx == NULL) {
        return;
    }
    ft_am_cache_invalidate(ctx);
    ctx->n_materialize = 0;
    ctx->n_hit = 0;
    ctx->sig_nsegs = 0;
    ctx->sig_ndocs = 0;
    ctx->sig_ntombs = 0;
}
