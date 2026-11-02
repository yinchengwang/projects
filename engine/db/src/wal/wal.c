#include "wal/wal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define WAL_FSYNC(f) _commit(_fileno(f))
#define FSEEK _fseeki64
#define FTELL _ftelli64
#else
#include <unistd.h>
#define WAL_FSYNC(f) fsync(fileno(f))
#define FSEEK fseeko
#define FTELL ftello
#endif

#include "common/crc.h"

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

struct Wal {
    FILE *f;
    char *path;        /* wal_reset 要重开文件，得记住路径 */
    u32   last_lsn;
    u32   flushed_lsn; /* 已经 fsync 落盘的最大 LSN */
    u32   nrecords;
    u32   max_xid;
    long  scan_pos;
    u8   *scratch;      /* 一条完整记录的临时缓冲 */
    PrevSlot prev[WAL_PREV_SLOTS];   /* 按 xid 维护的 undo 链 */
};

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
    w->scratch = malloc(WAL_HDR + WAL_MAX_PAYLOAD + 4);
    if (!w->scratch) {
        fclose(f);
        free(w->path);
        free(w);
        return E_IO;
    }

    /* 扫一遍确定 last_lsn、记录数、出现过的最大 xid */
    WalRec r;
    wal_scan_begin(w);
    while (wal_scan_next(w, &r) == OK) {
        if (r.lsn > w->last_lsn)
            w->last_lsn = r.lsn;
        if (r.xid > w->max_xid)
            w->max_xid = r.xid;
        w->nrecords++;
    }
    wal_scan_end(w);

    /* 日志文件里已有的内容本来就是落盘的 */
    w->flushed_lsn = w->last_lsn;

    *out = w;
    return OK;
}

int wal_reset(Wal *w)
{
    if (!w || !w->f || !w->path)
        return E_INVAL;

    /* 关掉再以 "wb" 重开 —— 截断到 0 字节，日志回到"刚创建"的状态 */
    if (fclose(w->f) != 0) {
        w->f = NULL;
        return E_IO;
    }
    w->f = fopen(w->path, "wb");
    if (!w->f)
        return E_IO;
    setvbuf(w->f, NULL, _IONBF, 0);

    w->last_lsn = 0;
    w->flushed_lsn = 0;
    w->nrecords = 0;
    w->max_xid = 0;
    w->scan_pos = 0;
    memset(w->prev, 0, sizeof(w->prev));
    return OK;
}

void wal_close(Wal *w)
{
    if (!w)
        return;
    if (w->f) {
        WAL_FSYNC(w->f);
        fclose(w->f);
    }
    free(w->path);
    free(w->scratch);
    free(w);
}

u32 wal_last_lsn(const Wal *w) { return w ? w->last_lsn : 0; }
u32 wal_nrecords(const Wal *w) { return w ? w->nrecords : 0; }
u32 wal_max_xid(const Wal *w)  { return w ? w->max_xid : 0; }
u32 wal_flushed_lsn(const Wal *w) { return w ? w->flushed_lsn : 0; }

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
u32 wal_append_xid(Wal *w, u8 type, u32 xid, u32 page_id, u16 slot, u16 slot_off,
                   const u8 *before, u16 blen, const u8 *after, u16 alen)
{
    if (!w)
        return 0;
    if ((u32)blen + alen > WAL_MAX_PAYLOAD)
        return 0;

    u32 lsn = w->last_lsn + 1;
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

    w->last_lsn = lsn;
    w->nrecords++;
    if (xid > w->max_xid)
        w->max_xid = xid;

    /* 记下这条是哪个事务的，下一条同事务的记录用它做 prev_lsn */
    wal_set_prev_lsn(w, xid, lsn);
    return lsn;
}

int wal_fsync(Wal *w)
{
    if (!w || !w->f)
        return E_INVAL;
    fflush(w->f);
    if (WAL_FSYNC(w->f) != 0)
        return E_IO;
    w->flushed_lsn = w->last_lsn;   /* 到这一刻为止的都落盘了 */
    return OK;
}

/* ---------------- 扫描 ---------------- */

int wal_scan_begin(Wal *w)
{
    if (!w)
        return E_INVAL;
    w->scan_pos = 0;
    return OK;
}

void wal_scan_end(Wal *w)
{
    if (w)
        w->scan_pos = 0;
}

int wal_scan_next(Wal *w, WalRec *r)
{
    if (!w || !r)
        return E_INVAL;

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

int wal_read_payload(Wal *w, const WalRec *r, u8 *buf, u16 cap)
{
    if (!w || !r || !buf)
        return E_INVAL;
    u32 n = (u32)r->blen + r->alen;
    if (n > cap)
        return E_NOSPACE;
    if (n == 0)
        return OK;

    if (FSEEK(w->f, (long long)(r->off + WAL_HDR), SEEK_SET) != 0)
        return E_IO;
    if (fread(buf, 1, n, w->f) != n)
        return E_IO;
    return OK;
}
