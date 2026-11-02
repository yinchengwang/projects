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
}

void qr_free(QueryResult *r)
{
    if (!r)
        return;
    qr_free_inner(r);
    free(r);
}

/* ---------------- 结果集构造 ---------------- */

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
    return OK;
}

static int b_grow(Builder *b)
{
    int ncap = b->cap * 2;
    char **nc = realloc(b->r->cells, (size_t)ncap * (size_t)b->r->ncols * sizeof(char *));
    if (!nc)
        return E_IO;
    memset(nc + (size_t)b->cap * (size_t)b->r->ncols, 0,
           (size_t)(ncap - b->cap) * (size_t)b->r->ncols * sizeof(char *));
    b->r->cells = nc;
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
