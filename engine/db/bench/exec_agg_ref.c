/*
 * exec_agg_ref.c — 聚合的**逐行参照实现**（只给 bench 用）
 *
 * ==================== 为什么需要它 ====================
 *
 * v1.1 的验收 A1 是「新旧执行器结果逐行全等」，
 * 对拍集要覆盖 WHERE / 投影 / 聚合 / GROUP BY / LIMIT / 索引路径 / 下推路径。
 *
 * 但冻结的火山副本（bench/exec_volcano.c）是 **v1.1 开工前**的执行器 ——
 * 那时**根本没有聚合**。让它跑聚合查询是不可能的。
 *
 * 所以聚合这一块换一种对拍方式：
 *   当前实现（列批 + hash 分组） vs 本文件（**逐行 + 线性分组**）
 *
 * 两份实现：
 *   - 走不同的扫描方式（攒批 vs 逐行）
 *   - 走不同的分组方式（hash 表 vs 线性数组）
 *   - 走不同的输出排序（编码键的字节序 vs val_cmp 的值序）
 *
 * 【为什么"输出排序"也能拿来对拍】
 * 生产实现按**编码后的键字节**排序，参照实现按**值序**（val_cmp）排序。
 * 两者只有在"编码的字节序 == 数值序"时才一致 ——
 * 而那正是键编码设计要保证的不变式。
 * 所以顺序不一致 = 键编码错了 = 真 bug ✓
 *
 * ==================== 共享什么、不共享什么 ====================
 *
 * 共享：eval_expr（表达式求值）、val_cmp（比较语义）、b_push（结果集构造）。
 *       —— 这些是项目刻意集中的"语义唯一实现"，两边共用才有意义。
 * 不共享：扫描、分组、累加、排序 —— **这才是要对的拍**。
 *
 * ==================== 这个文件不参与主构建 ====================
 *
 * 它住在 bench/ 目录，只链进 exec_bench；主二进制（db.exe）里不存在它。
 */

#include "query/exec_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 参照实现的累加器 ---------------- */

typedef struct {
    AggKind kind;
    int     col;        /* 聚合的列；COUNT(*) = -1 */
    u64     count;
    i64     isum;
    double  dsum;
    bool    is_real;
    bool    seen;
    DbValue mm;         /* MIN/MAX */
} RefAgg;

/* 一个组 */
typedef struct {
    DbValue *key;       /* 分组列的值（ngroupby 个） */
    RefAgg  *aggs;      /* 累加器（naggs 个） */
} RefGroup;

typedef struct {
    int       ngroupby;
    int       groupcol[SQL_MAX_COLS];
    int       naggs;
    AggKind   aggkind[SQL_MAX_COLS];
    int       aggcol[SQL_MAX_COLS];
    int       aggtarget[SQL_MAX_COLS];

    RefGroup *groups;
    int       ngroups;
    int       cap;
} RefCtx;

/* ---------------- 组键比较（值序，NULL 最小） ---------------- */

static int cmp_group(const RefCtx *rc, const DbValue *a, const DbValue *b)
{
    for (int i = 0; i < rc->ngroupby; i++) {
        if (a[i].is_null && b[i].is_null)
            continue;
        if (a[i].is_null)
            return -1;                  /* NULL 排最前 */
        if (b[i].is_null)
            return 1;

        bool isnull;
        int cerr = OK;
        int c = val_cmp(&a[i], &b[i], &isnull, &cerr);
        if (cerr != OK)
            return 0;
        if (c != 0)
            return c;
    }
    return 0;
}

/* ---------------- 组表（线性数组） ---------------- */

static int ref_add_group(RefCtx *rc, const DbValue *key, int *out_idx)
{
    if (rc->ngroups >= rc->cap) {
        int ncap = rc->cap ? rc->cap * 2 : 16;
        RefGroup *ng = realloc(rc->groups, (size_t)ncap * sizeof(RefGroup));
        if (!ng)
            return -1;
        rc->groups = ng;
        rc->cap = ncap;
    }

    RefGroup *g = &rc->groups[rc->ngroups];
    memset(g, 0, sizeof(*g));

    g->key = calloc((size_t)(rc->ngroupby ? rc->ngroupby : 1), sizeof(DbValue));
    g->aggs = calloc((size_t)(rc->naggs ? rc->naggs : 1), sizeof(RefAgg));
    if (!g->key || !g->aggs)
        return -1;

    for (int i = 0; i < rc->ngroupby; i++)
        val_copy(&g->key[i], &key[i]);
    for (int i = 0; i < rc->naggs; i++) {
        g->aggs[i].kind = rc->aggkind[i];
        g->aggs[i].col = rc->aggcol[i];
    }

    *out_idx = rc->ngroups;
    rc->ngroups++;
    return 0;
}

/* ---------------- 累加 ---------------- */

static int ref_step(RefAgg *a, const DbValue *v)
{
    if (a->kind == AGG_COUNT && a->col < 0) {
        a->count++;
        a->seen = true;
        return OK;
    }
    if (!v || v->is_null)
        return OK;

    a->count++;
    a->seen = true;

    switch (a->kind) {
    case AGG_COUNT:
        break;
    case AGG_SUM:
        if (v->type == T_REAL) {
            a->is_real = true;
            a->dsum += v->d;
        } else if (a->is_real) {
            a->dsum += (double)v->i;
        } else {
            i64 x = v->i;
            i64 s = a->isum;
            if ((x > 0 && s > INT64_MAX - x) || (x < 0 && s < INT64_MIN - x))
                return E_NOSPACE;         /* 整数溢出 → 报错（同生产实现） */
            a->isum = s + x;
            a->dsum = (double)a->isum;
        }
        break;
    case AGG_AVG:
        if (v->type == T_REAL)
            a->dsum += v->d;
        else
            a->dsum += (double)v->i;
        break;
    case AGG_MIN:
    case AGG_MAX:
        if (a->count == 1) {
            val_copy(&a->mm, v);
            break;
        }
        {
            bool isnull;
            int cerr = OK;
            int c = val_cmp(v, &a->mm, &isnull, &cerr);
            if (cerr != OK)
                break;
            bool take = (a->kind == AGG_MIN) ? (c < 0) : (c > 0);
            if (take)
                val_copy(&a->mm, v);
        }
        break;
    default:
        break;
    }
    return OK;
}

static void ref_result(const RefAgg *a, DbValue *out)
{
    switch (a->kind) {
    case AGG_COUNT:
        val_int(out, (i64)a->count);
        break;
    case AGG_SUM:
        if (!a->seen) { val_null(out); break; }
        if (a->is_real) val_real(out, a->dsum);
        else            val_int(out, a->isum);
        break;
    case AGG_AVG:
        if (!a->seen) { val_null(out); break; }
        val_real(out, a->dsum / (double)a->count);
        break;
    case AGG_MIN:
    case AGG_MAX:
        if (!a->seen) { val_null(out); break; }
        val_copy(out, &a->mm);
        break;
    default:
        val_null(out);
        break;
    }
}

/* ---------------- 聚合列名（与生产实现一致，格式不同就要被对拍抓到） ---------------- */

static void ref_colname(const Expr *e, char *buf, int cap)
{
    if (!e->l)
        snprintf(buf, (size_t)cap, "%s(*)", e->name);
    else if (e->l->kind == EX_COL)
        snprintf(buf, (size_t)cap, "%s(%s)", e->name, e->l->name);
    else
        snprintf(buf, (size_t)cap, "%s(expr)", e->name);
}

/* ---------------- 主流程 ---------------- */

int exec_select_agg_ref(Db *db, Stmt *st, QueryResult **out,
                         char *err, int errcap)
{
    TableCtx tc;
    int rc = resolve_table(db, st->table, &tc, err, errcap);
    if (rc != OK)
        return rc;

    /* 绑定 */
    for (int i = 0; i < st->ntargets; i++) {
        rc = bind_expr(st->targets[i], tc.cols, tc.ncols, err, errcap);
        if (rc != OK)
            return rc;
    }
    if (st->where) {
        rc = bind_expr(st->where, tc.cols, tc.ncols, err, errcap);
        if (rc != OK)
            return rc;
    }
    for (int i = 0; i < st->ngroupby; i++) {
        rc = bind_expr(st->groupby[i], tc.cols, tc.ncols, err, errcap);
        if (rc != OK)
            return rc;
    }

    if (st->star) {
        snprintf(err, (size_t)errcap, "SELECT * 不能和聚合一起用");
        return E_INVAL;
    }

    RefCtx rcx;
    memset(&rcx, 0, sizeof(rcx));

    /* 分组列 */
    rcx.ngroupby = st->ngroupby;
    for (int i = 0; i < st->ngroupby; i++) {
        const Expr *g = st->groupby[i];
        if (!g || g->kind != EX_COL || g->col_idx < 0 || g->col_idx >= tc.ncols) {
            snprintf(err, (size_t)errcap, "GROUP BY 只支持列引用");
            return E_NOTSUP;
        }
        rcx.groupcol[i] = g->col_idx;
    }

    /* 聚合函数 */
    for (int t = 0; t < st->ntargets; t++) {
        const Expr *e = st->targets[t];
        if (!e || e->kind != EX_AGG)
            continue;
        if (rcx.naggs >= SQL_MAX_COLS) {
            snprintf(err, (size_t)errcap, "聚合函数个数超限");
            return E_NOTSUP;
        }
        rcx.aggkind[rcx.naggs] = (AggKind)e->op;
        rcx.aggtarget[rcx.naggs] = t;
        if (!e->l) {
            rcx.aggcol[rcx.naggs] = -1;
        } else if (e->l->kind == EX_COL && e->l->col_idx >= 0) {
            rcx.aggcol[rcx.naggs] = e->l->col_idx;
        } else {
            snprintf(err, (size_t)errcap, "聚合函数只支持单列参数");
            return E_NOTSUP;
        }
        rcx.naggs++;
    }

    /* 结果集外壳（列名与生产实现一致） */
    Builder b;
    rc = b_init(&b, st->ntargets, 64);
    if (rc != OK) {
        qr_free(b.r);
        return rc;
    }
    for (int i = 0; i < st->ntargets; i++) {
        if (st->targets[i]->kind == EX_COL)
            b.r->colnames[i] = xstrdup(st->targets[i]->name);
        else if (st->targets[i]->kind == EX_AGG) {
            char nm[SQL_NAME_MAX * 2 + 16];
            ref_colname(st->targets[i], nm, (int)sizeof(nm));
            b.r->colnames[i] = xstrdup(nm);
        } else {
            b.r->colnames[i] = xstrdup("expr");
        }
    }

    /* 逐行扫（全表扫，不下推 —— 参照实现要的是"另一种走法"） */
    AmScan *scan = NULL;
    rc = tc.am->scan_begin(tc.am, &scan);
    if (rc != OK) {
        qr_free(b.r);
        return rc;
    }

    AmVisCtx vis;
    make_vis_ctx(db, &vis);

    u8 recbuf[PAGE_SIZE];
    u16 reclen;
    DbValue row[SQL_MAX_COLS];
    DbValue key[SQL_MAX_COLS];
    Rid rid;

    while ((rc = tc.am->scan_next(tc.am, scan, &rid,
                                  recbuf, sizeof(recbuf), &reclen)) == OK) {
        if (!tc.am->visible(tc.am, recbuf, reclen, &vis))
            continue;
        if (tc.am->decode(tc.am, recbuf, reclen, row) != OK)
            continue;

        if (st->where) {
            DbValue w;
            if (eval_expr(st->where, row, &w) != OK)
                continue;
            if (val_truth(&w) != 1)
                continue;
        }

        /* 找组（线性查找） */
        int gi = -1;
        if (rcx.ngroupby == 0) {
            if (rcx.ngroups == 0) {
                if (ref_add_group(&rcx, key, &gi) != 0)
                    goto fail_io;
            } else {
                gi = 0;
            }
        } else {
            for (int c = 0; c < rcx.ngroupby; c++)
                val_copy(&key[c], &row[rcx.groupcol[c]]);
            for (int i = 0; i < rcx.ngroups; i++) {
                if (cmp_group(&rcx, rcx.groups[i].key, key) == 0) {
                    gi = i;
                    break;
                }
            }
            if (gi < 0) {
                if (ref_add_group(&rcx, key, &gi) != 0)
                    goto fail_io;
            }
        }

        /* 累加 */
        for (int a = 0; a < rcx.naggs; a++) {
            DbValue v;
            int col = rcx.aggcol[a];
            if (col >= 0)
                val_copy(&v, &row[col]);
            int erc = ref_step(&rcx.groups[gi].aggs[a], col >= 0 ? &v : NULL);
            if (erc != OK) {
                tc.am->scan_end(tc.am, scan);
                qr_free(b.r);
                snprintf(err, (size_t)errcap,
                         "整数求和溢出（超出 64 位整数范围）");
                return erc;
            }
        }
    }
    tc.am->scan_end(tc.am, scan);
    if (rc == E_EOF)
        rc = OK;
    if (rc != OK)
        goto fail_qr;

    /*
     * 没有 GROUP BY 时，**恒有一组**（哪怕一行都没匹配）——
     * COUNT(*) = 0、SUM = NULL，这一条与 SQLite 一致 ✓
     * （有 GROUP BY 时，空结果集就是 0 行。）
     */
    if (rcx.ngroupby == 0 && rcx.ngroups == 0) {
        int gi = 0;
        if (ref_add_group(&rcx, key, &gi) != 0)
            goto fail_alloc;
    }

    /* 按值序排序（插入排序：组数不多） */
    for (int i = 1; i < rcx.ngroups; i++) {
        RefGroup v = rcx.groups[i];
        int j = i;
        while (j > 0 && cmp_group(&rcx, rcx.groups[j - 1].key, v.key) > 0) {
            rcx.groups[j] = rcx.groups[j - 1];
            j--;
        }
        rcx.groups[j] = v;
    }

    /* 输出 */
    for (int g = 0; g < rcx.ngroups; g++) {
        if (st->limit >= 0 && b.r->nrows >= st->limit)
            break;

        DbValue proj[SQL_MAX_COLS];
        for (int t = 0; t < st->ntargets; t++) {
            const Expr *e = st->targets[t];
            if (e->kind == EX_AGG) {
                bool found = false;
                for (int a = 0; a < rcx.naggs; a++) {
                    if (rcx.aggtarget[a] == t) {
                        ref_result(&rcx.groups[g].aggs[a], &proj[t]);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    snprintf(err, (size_t)errcap, "聚合函数的投影项对不上");
                    goto fail_qr;
                }
            } else if (e->kind == EX_COL) {
                bool found = false;
                for (int c = 0; c < rcx.ngroupby; c++) {
                    if (rcx.groupcol[c] == e->col_idx) {
                        val_copy(&proj[t], &rcx.groups[g].key[c]);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    snprintf(err, (size_t)errcap,
                             "分组查询的投影里出现了非分组列: %s", e->name);
                    goto fail_qr;
                }
            } else {
                snprintf(err, (size_t)errcap,
                         "分组查询的投影只支持分组列与聚合函数");
                goto fail_qr;
            }
        }
        rc = b_push(&b, proj);
        if (rc != OK)
            goto fail_qr;
    }

    /* 释放 */
    for (int i = 0; i < rcx.ngroups; i++) {
        free(rcx.groups[i].key);
        free(rcx.groups[i].aggs);
    }
    free(rcx.groups);

    *out = b.r;
    return OK;

fail_io:
    tc.am->scan_end(tc.am, scan);
    goto fail_alloc;

fail_alloc:
    for (int i = 0; i < rcx.ngroups; i++) {
        free(rcx.groups[i].key);
        free(rcx.groups[i].aggs);
    }
    free(rcx.groups);
    qr_free(b.r);
    snprintf(err, (size_t)errcap, "内存不足");
    return E_IO;

fail_qr:
    for (int i = 0; i < rcx.ngroups; i++) {
        free(rcx.groups[i].key);
        free(rcx.groups[i].aggs);
    }
    free(rcx.groups);
    qr_free(b.r);
    return rc == OK ? E_IO : rc;
}
