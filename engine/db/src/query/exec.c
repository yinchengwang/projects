/*
 * exec.c — 执行器入口 + 结果集构造
 *
 * ==================== v0.9：执行器变成"模态无关" ====================
 *
 * 改造前：执行器直接用 heap_* / btree_*，堆表的知识散落在查询层。
 * 改造后：执行器只认 **StorageAM** 接口。
 *
 *   扫描   → am->scan_begin / scan_next / scan_end
 *   取行   → am->get
 *   解码   → am->decode
 *   可见性 → am->visible
 *   写     → am->insert / update / remove
 *   维护   → am->vacuum / reindex
 *   下推   → am->can_push / push
 *
 * 【验收标准里的那一条】
 * "执行器里没有一行 blob 特判" —— 本文件里 grep "blob" 应该只有注释。
 * 模态的知识全在 spi/ 里，执行器只负责翻译。
 *
 * 【三个文件的分工】
 *   exec.c        入口 + 结果集构造（这里）
 *   exec_scan.c   绑定 + 求值 + 表解析 + 下推 + SELECT
 *   exec_write.c  INSERT / UPDATE / DELETE / VACUUM / CREATE
 */

#include "query/exec_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 小工具 ---------------- */

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static void qr_free_inner(QueryResult *r)
{
    if (!r)
        return;
    if (r->colnames) {
        for (int i = 0; i < r->ncols; i++)
            free(r->colnames[i]);
        free(r->colnames);
    }
    if (r->cells) {
        int n = r->nrows * r->ncols;
        for (int i = 0; i < n; i++)
            free(r->cells[i]);
        free(r->cells);
    }
    free(r->nullbits);
    r->nullbits = NULL;
    r->nullcap = 0;
}

void qr_free(QueryResult *r)
{
    if (!r)
        return;
    qr_free_inner(r);
    free(r);
}

/* ---------------- 结果集构造 ---------------- */

/* 位图字节数：每格 1 位，向上取整到字节 */
static int null_bytes(int ncells)
{
    return (ncells + 7) / 8;
}

int b_init(Builder *b, int ncols, int nrows_hint)
{
    memset(b, 0, sizeof(*b));
    b->r = calloc(1, sizeof(QueryResult));
    if (!b->r)
        return E_IO;
    b->r->ncols = ncols;
    b->r->colnames = calloc((size_t)ncols, sizeof(char *));
    if (!b->r->colnames)
        return E_IO;

    int cap = nrows_hint > 0 ? nrows_hint : 8;
    b->cap = cap;
    b->r->cells = calloc((size_t)cap * (size_t)ncols, sizeof(char *));
    if (!b->r->cells)
        return E_IO;

    /*
     * NULL 位图和 cells 同容量、同索引口径。
     * calloc 已把全部位清零 = "全部非 NULL"，b_push 只在是 NULL 时置 1。
     */
    b->r->nullbits = calloc((size_t)null_bytes(cap * ncols), 1);
    if (!b->r->nullbits)
        return E_IO;
    b->r->nullcap = cap;
    return OK;
}

/*
 * 扩容：cells 与 NULL 位图**必须一次提交**。
 *
 * 【为什么不能各扩各的】
 * 两个数组若一个 realloc 成功、另一个失败，QueryResult 里就会留下
 * "cells 已指新缓冲、nullbits 还是旧缓冲"的尺寸不一致状态；
 * 此时若直接返回错误，调用方 qr_free 只会按新容量释放，旧缓冲既漏又可能被误读。
 *
 * 所以这里先把两个新缓冲**全部分配好并拷完旧数据**，最后才一次性
 * 写回 cells / nullbits / cap 三个字段。中途任何一步失败只释放临时缓冲，
 * QueryResult 保持扩容前原样，调用方按原有路径清理即可。
 */
static int b_grow(Builder *b)
{
    int ncap = b->cap * 2;
    size_t ncells_old = (size_t)b->cap * (size_t)b->r->ncols;
    size_t ncells_new = (size_t)ncap * (size_t)b->r->ncols;
    int old_bytes = null_bytes((int)ncells_old);
    int new_bytes = null_bytes((int)ncells_new);

    char **nc = calloc(ncells_new, sizeof(char *));
    if (!nc)
        return E_IO;

    u8 *nb = calloc((size_t)new_bytes, 1);
    if (!nb) {
        free(nc);
        return E_IO;
    }

    /* 拷旧内容：cells 逐个指针，位图整段拷贝（旧位对齐规则与新位一致） */
    memcpy(nc, b->r->cells, ncells_old * sizeof(char *));
    if (b->r->nullbits)
        memcpy(nb, b->r->nullbits, (size_t)old_bytes);

    /* 两个新缓冲都已就绪 —— 到这里才允许改 QueryResult */
    free(b->r->cells);
    free(b->r->nullbits);
    b->r->cells = nc;
    b->r->nullbits = nb;
    b->r->nullcap = ncap;
    b->cap = ncap;
    return OK;
}

int b_push(Builder *b, const DbValue *vals)
{
    if (b->r->nrows >= b->cap) {
        int rc = b_grow(b);
        if (rc != OK)
            return rc;
    }
    int base = b->r->nrows * b->r->ncols;
    for (int i = 0; i < b->r->ncols; i++) {
        char *cell = NULL;

        if (vals[i].external) {
            /*
             * 外部值：物化成精确大小的缓冲。
             *
             * 内容可能几 KB（一帧 4076 字节），
             * 512 字节的栈缓冲装不下 → 自己分配。
             *
             * 【这是外部值唯一的物化点】
             * 扫描、过滤、投影全程不物化 —— 所以
             * "只查 blob_id" 不读帧页。
             */
            u8 *tmp = NULL;
            u32 n = 0;
            int rc = val_detoast(&vals[i], &tmp, &n);
            if (rc != OK)
                return rc;

            cell = malloc((size_t)n + 1);
            if (!cell) {
                free(tmp);
                return E_IO;
            }
            if (n)
                memcpy(cell, tmp, n);
            cell[n] = '\0';
            free(tmp);
        } else {
            char buf[512];
            val_to_str(&vals[i], buf, (int)sizeof(buf));
            cell = xstrdup(buf);
            if (!cell)
                return E_IO;
        }

        b->r->cells[base + i] = cell;
    }

    /*
     * 填 NULL 位：与 cells 同一 row-major 索引。
     * 位图容量是 cap（不是 nrows）—— 此刻 nrows 仍是旧行数，
     * 按 nrows 判界会把满容量的最后一行误判越界。
     */
    if (b->r->nullbits) {
        for (int i = 0; i < b->r->ncols; i++) {
            if (!vals[i].is_null)
                continue;
            int bit = base + i;
            b->r->nullbits[bit / 8] |= (u8)(1u << (bit % 8));
        }
    }

    b->r->nrows++;
    return OK;
}

/* ---------------- 入口 ---------------- */

int exec_stmt(Db *db, Stmt *st, QueryResult **out, char *err, int errcap)
{
    char local_err[256] = {0};

    if (!db || !st || !out)
        return E_INVAL;

    /* 错误缓冲由调用方给；没给就用局部的 */
    if (!err || errcap <= 0) {
        err = local_err;
        errcap = (int)sizeof(local_err);
    }

    *out = NULL;

    switch (st->kind) {
    case ST_SELECT:
        /* EXPLAIN 前缀：只打印计划，不执行（v1.1 步骤 4） */
        if (st->explain)
            return exec_explain(db, st, out, err, errcap);
        return exec_select(db, st, out, err, errcap);
    case ST_INSERT: return exec_insert(db, st, out, err, errcap);
    case ST_UPDATE: return exec_update(db, st, out, err, errcap);
    case ST_DELETE: return exec_delete(db, st, out, err, errcap);
    case ST_CREATE: return exec_create(db, st, out, err, errcap);
    case ST_CREATE_INDEX: return exec_create_index(db, st, out, err, errcap);
    case ST_VACUUM: return exec_vacuum(db, st, out, err, errcap);
    default:        return E_INVAL;
    }
}
