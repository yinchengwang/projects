/*
 * exec_agg.c — 聚合 + GROUP BY（v1.1 步骤 3）
 *
 * ==================== 这一版消化的是什么 ====================
 *
 * v0.4 顺延的"无 GROUP BY 聚合"与 v0.3.5 顺延的"GROUP BY / hash 分组"。
 *
 * ==================== 为什么建在列批上 ====================
 *
 * 聚合是"一行进来、累加器动一下"的算子 —— 它天生吃列式数据：
 * 批里的每一列都是紧凑数组，累加器扫数组就行，
 * 不用像逐行路径那样每行物化一个 272 字节的 DbValue 再拆开看。
 *
 * 所以本文件直接用 exec_batch.c 的批（攒批 + 转置 + 列式 WHERE 求值），
 * 在**选中的行**上做累加 ✓。
 *
 * ==================== 语义必须与 SQLite 一致 ====================
 *
 *   COUNT(*)     → 行数
 *   COUNT(col)   → 非 NULL 的个数
 *   SUM(col)     → 和；全 NULL → NULL；整数列 → 整数，**溢出报错**
 *   AVG(col)     → 平均；**总是浮点**；全 NULL → NULL
 *   MIN/MAX(col) → 最小/大；全 NULL → NULL
 *
 * 【为什么整数溢出要报错，而不是转浮点】
 * SQLite 报 "integer overflow"，PG 报 "bigint out of range" ——
 * 两个主流实现都报错，本版照它们。
 *
 * 转浮点会**静默丢精度**：
 *   9223372036854775807 + 2 = 9223372036854775809
 * 转成 double 是 9223372036854775808.0 —— 差 1，但结果"看起来对"。
 * 本项目的铁律是"错的答案比明确的报错坏得多"，所以报错 ✓
 *
 * （浮点列不报错：SQLite 的规则是"只要有一个实数，和就是实数"，
 *   实数相加没有溢出这回事。）
 *
 * ==================== 组数超限不 spill ====================
 *
 * 内存 hash 表，组数上限 MAX_GROUPS（config.h）。
 * 超限**明确报错**并把上限写进错误信息 ——
 * 静默截断会给出"看起来对其实错"的结果（见 config.h 的注释）。
 */

#include "query/exec_group.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 累加器 ---------------- */

/* AggAcc 定义在 exec_group.h（它住在组里，见那里的说明） */

/* 聚合函数清单（从投影里收出来的） */
typedef struct {
    AggKind kind;
    int     col;        /* -1 = COUNT(*) */
    int     target;     /* 它出现在第几个投影里 */
} AggSpec;

typedef struct {
    int      ngroupby;
    int      groupcol[SQL_MAX_COLS];   /* 分组列的下标 */
    int      naggs;
    AggSpec  aggs[SQL_MAX_COLS];

    /* 无 GROUP BY：单组 */
    AggAcc   single[SQL_MAX_COLS];

    /* 有 GROUP BY：hash 表（在 exec_group.c 里） */
    GroupTab gt;
} AggCtx;

/* ---------------- 上下文 ---------------- */

/* 收集这条语句的分组列与聚合函数 */
static int agg_ctx_init(AggCtx *ac, const Stmt *st, const TableCtx *tc,
                        char *err, int errcap)
{
    memset(ac, 0, sizeof(*ac));

    /* 分组列：只支持列引用 */
    ac->ngroupby = st->ngroupby;
    for (int i = 0; i < st->ngroupby; i++) {
        const Expr *g = st->groupby[i];
        if (!g || g->kind != EX_COL || g->col_idx < 0 || g->col_idx >= tc->ncols) {
            snprintf(err, (size_t)errcap, "GROUP BY 只支持列引用");
            return E_NOTSUP;
        }
        ac->groupcol[i] = g->col_idx;
    }

    /* 聚合函数：只支持直接出现在投影里的 */
    for (int t = 0; t < st->ntargets; t++) {
        const Expr *e = st->targets[t];
        if (!e || e->kind != EX_AGG)
            continue;
        if (ac->naggs >= SQL_MAX_COLS) {
            snprintf(err, (size_t)errcap, "聚合函数个数超限");
            return E_NOTSUP;
        }
        AggSpec *sp = &ac->aggs[ac->naggs++];
        sp->kind = (AggKind)e->op;
        sp->target = t;
        if (!e->l) {
            sp->col = -1;                 /* COUNT(*) */
        } else if (e->l->kind == EX_COL && e->l->col_idx >= 0) {
            sp->col = e->l->col_idx;
        } else {
            snprintf(err, (size_t)errcap, "聚合函数只支持单列参数");
            return E_NOTSUP;
        }
    }

    if (ac->naggs == 0 && ac->ngroupby == 0) {
        /*
         * 没有 GROUP BY 也没有聚合函数 → 这条语句根本不该走聚合路径。
         * （stmt_has_agg 已经保证不会到这里；这里是防御。）
         */
        snprintf(err, (size_t)errcap, "没有可用的聚合函数");
        return E_NOTSUP;
    }

    /* 单组模式的累加器 */
    for (int i = 0; i < ac->naggs; i++) {
        ac->single[i].kind = ac->aggs[i].kind;
        ac->single[i].col = ac->aggs[i].col;
    }

    if (ac->ngroupby > 0) {
        /* 组表（键编码 + hash 分组）在 exec_group.c 里 */
        GroupCols gc;
        memset(&gc, 0, sizeof(gc));
        gc.ncols = ac->ngroupby;
        for (int i = 0; i < ac->ngroupby; i++)
            gc.col[i] = ac->groupcol[i];
        if (group_tab_init(&ac->gt, &gc, ac->naggs) != OK)
            return E_IO;
    }

    return OK;
}

static void agg_ctx_free(AggCtx *ac)
{
    group_tab_free(&ac->gt);
    memset(ac, 0, sizeof(*ac));
}

/* ---------------- 累加 ---------------- */

static int agg_step(AggAcc *a, const DbValue *v)
{
    /* COUNT(*)：数行 */
    if (a->kind == AGG_COUNT && a->col < 0) {
        a->count++;
        a->seen = true;
        return OK;
    }

    if (!v || v->is_null)
        return OK;                 /* NULL 不参与聚合 */

    a->count++;
    a->seen = true;

    switch (a->kind) {
    case AGG_COUNT:
        break;

    case AGG_SUM:
        /*
         * 【为什么分两支】
         * 整数列要出整数结果（SQLite：SUM(整数列) 是整数）；
         * 出现实数就整体转实数（SQLite 的规则：
         * "只要有一个实数，和就是实数"）。
         * 转了之后再来整数，必须**继续加到浮点累加器上** ——
         * 否则会把之前累的浮点部分冲掉（值就错了）。
         */
        if (v->type == T_REAL) {
            a->is_real = true;
            a->dsum += v->d;
        } else if (a->is_real) {
            a->dsum += (double)v->i;        /* 已经转实数了 */
        } else {
            i64 x = v->i;
            i64 s = a->isum;
            if ((x > 0 && s > INT64_MAX - x) || (x < 0 && s < INT64_MIN - x))
                return E_NOSPACE;           /* 整数溢出 → 报错（见文件头） */
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
    case AGG_MAX: {
        /*
         * 【为什么用 count == 1 判"第一个值"】
         * seen 在进 switch 之前就被置真了（NULL 不参与，
         * 非 NULL 才置真），所以这里不能再看 seen。
         * count 刚好数的是非 NULL 的个数 —— count == 1 就是第一个。
         */
        if (a->count == 1) {
            val_copy(&a->mm, v);            /* 第一个值直接收下 */
            break;
        }
        bool isnull;
        int cerr = OK;
        int c = val_cmp(v, &a->mm, &isnull, &cerr);
        if (cerr != OK)
            break;                          /* 外部值物化失败：不更新 */
        bool take = (a->kind == AGG_MIN) ? (c < 0) : (c > 0);
        if (take)
            val_copy(&a->mm, v);
        break;
    }

    default:
        break;
    }
    return OK;
}

/* 累加器的结果值 */
static void agg_result(const AggAcc *a, DbValue *out)
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
        val_real(out, a->dsum / (double)a->count);   /* AVG 恒为浮点 */
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

/* ---------------- 主流程 ---------------- */

/* 这条语句要不要走聚合路径 */
bool stmt_has_agg(const Stmt *st)
{
    if (st->ngroupby > 0)
        return true;
    for (int i = 0; i < st->ntargets; i++) {
        if (st->targets[i] && st->targets[i]->kind == EX_AGG)
            return true;
    }
    return false;
}

/*
 * 把一个组的投影行算出来。
 *
 * 【允许什么】
 *   - 聚合函数（EX_AGG）
 *   - 分组列（EX_COL，且必须是某个分组列）
 *
 * 【为什么不允许投影非分组列】
 * SQLite 允许（取该组某一行��值），但那是**不确定的** ——
 * 同一份数据换个扫描顺序结果就可能不同。
 * 本版明确报错：不确定的语义宁可报错，也不要给"看起来对"的结果。
 */
static int group_project(const AggCtx *ac, const Stmt *st, const Group *g,
                         DbValue *proj, char *err, int errcap)
{
    for (int t = 0; t < st->ntargets; t++) {
        const Expr *e = st->targets[t];

        if (e && e->kind == EX_AGG) {
            bool found = false;
            for (int a = 0; a < ac->naggs; a++) {
                if (ac->aggs[a].target == t) {
                    agg_result(&g->acc[a], &proj[t]);
                    found = true;
                    break;
                }
            }
            if (!found) {
                snprintf(err, (size_t)errcap, "聚合函数的投影项对不上");
                return E_INVAL;
            }
        } else if (e && e->kind == EX_COL) {
            bool found = false;
            for (int c = 0; c < ac->ngroupby; c++) {
                if (ac->groupcol[c] == e->col_idx) {
                    val_copy(&proj[t], &g->rep[c]);
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (ac->ngroupby == 0)
                    snprintf(err, (size_t)errcap,
                             "没有 GROUP BY 时，投影里只能有聚合函数");
                else
                    snprintf(err, (size_t)errcap,
                             "分组查询的投影里出现了非分组列: %s", e->name);
                return E_INVAL;
            }
        } else {
            snprintf(err, (size_t)errcap,
                     "分组查询的投影只支持分组列与聚合函数");
            return E_INVAL;
        }
    }
    return OK;
}

/*
 * 聚合查询。
 *
 * 【输出的行数】
 *   无 GROUP BY → 恒 1 行（表空也有：COUNT(*) = 0，SUM = NULL）✓ 与 SQLite 一致
 *   有 GROUP BY → 每组 1 行；表空 → 0 行 ✓
 *
 * 【输出的顺序】按组键排序 → 结果确定 ✓（SQLite 的分组顺序是未定义的，
 *   我们给一个确定顺序，测试才好逐行比）
 *
 * 【WHERE 的两种走法】
 *   列式能求值 → 批内求值（快）
 *   列式不能   → 逐行求值，进批之前就滤掉（语义只有一处：eval_expr）
 *   两条路的语义必须一致 —— 所以列式的比较语义照着 val_cmp 复刻。
 */
int exec_select_agg(Db *db, Stmt *st, TableCtx *tc, Builder *b,
                    char *err, int errcap)
{
    if (st->star) {
        snprintf(err, (size_t)errcap, "SELECT * 不能和聚合一起用");
        return E_INVAL;
    }

    AggCtx ac;
    memset(&ac, 0, sizeof(ac));

    int rc = agg_ctx_init(&ac, st, tc, err, errcap);
    if (rc != OK) {
        agg_ctx_free(&ac);
        return rc;
    }

    /* WHERE 的形状：能列式就列式 */
    int need_depth = 0;
    bool colwhere = where_supported(st->where, tc->cols, tc->ncols, 0,
                                    &need_depth);

    /* 需要的列：聚合的列 + 分组列（+ 列式 WHERE 用的列） */
    u16 needed = 0;
    for (int i = 0; i < ac.naggs; i++) {
        if (ac.aggs[i].col >= 0)
            needed |= (u16)(1u << ac.aggs[i].col);
    }
    for (int i = 0; i < ac.ngroupby; i++)
        needed |= (u16)(1u << ac.groupcol[i]);
    if (colwhere)
        collect_needed(st->where, &needed);

    BatchCtx bc;
    rc = batch_ctx_init(&bc, tc->cols, tc->ncols, needed);
    if (rc != OK) {
        agg_ctx_free(&ac);
        batch_ctx_free(&bc);
        snprintf(err, (size_t)errcap, "内存不足");
        return E_IO;
    }

    AmScan *scan = NULL;
    rc = open_scan(db, tc, st->where, &scan);
    if (rc != OK) {
        agg_ctx_free(&ac);
        batch_ctx_free(&bc);
        return rc;
    }

    AmVisCtx vis;
    make_vis_ctx(db, &vis);

    u8 recbuf[PAGE_SIZE];
    u16 reclen;
    DbValue row[SQL_MAX_COLS];
    Rid rid;

    for (;;) {
        batch_reset(&bc);

        /* 攒批：逐行取 → 可见性 → 解码 →（必要时）逐行滤 WHERE → 转置 */
        while (bc.nrows < VEC_BATCH) {
            rc = tc->am->scan_next(tc->am, scan, &rid,
                                    recbuf, sizeof(recbuf), &reclen);
            if (rc != OK)
                break;
            if (!tc->am->visible(tc->am, recbuf, reclen, &vis))
                continue;
            if (tc->am->decode(tc->am, recbuf, reclen, row) != OK)
                continue;

            if (!colwhere && st->where) {
                DbValue w;
                int erc = eval_expr(st->where, row, &w);
                if (erc != OK) {
                    tc->am->scan_end(tc->am, scan);
                    agg_ctx_free(&ac);
                    batch_ctx_free(&bc);
                    return erc;
                }
                if (val_truth(&w) != 1)      /* FALSE 和 NULL 都不匹配 */
                    continue;
            }

            int arc = batch_add_row(&bc, row);
            if (arc != OK) {
                tc->am->scan_end(tc->am, scan);
                agg_ctx_free(&ac);
                batch_ctx_free(&bc);
                return arc;
            }
        }

        if (bc.nrows == 0)
            break;

        /* 列式求值 WHERE → 标记 */
        u8 *marks = bc.marks;
        if (colwhere) {
            int erc = eval_where(st->where, &bc, tc->cols, tc->ncols, marks, 0);
            if (erc != OK) {
                tc->am->scan_end(tc->am, scan);
                agg_ctx_free(&ac);
                batch_ctx_free(&bc);
                snprintf(err, (size_t)errcap, "聚合查询的 WHERE 求值失败");
                return erc;
            }
        } else {
            for (u32 i = 0; i < bc.nrows; i++)
                marks[i] = M_TRUE;          /* 已在进批前滤过 */
        }

        /* 累加 */
        for (u32 i = 0; i < bc.nrows; i++) {
            if (marks[i] != M_TRUE)
                continue;

            if (ac.ngroupby == 0) {
                for (int a = 0; a < ac.naggs; a++) {
                    DbValue v;
                    if (ac.single[a].col >= 0)
                        batch_cell(&bc, ac.single[a].col, i, &v);
                    int arc = agg_step(&ac.single[a],
                                       ac.single[a].col >= 0 ? &v : NULL);
                    if (arc != OK) {
                        tc->am->scan_end(tc->am, scan);
                        agg_ctx_free(&ac);
                        batch_ctx_free(&bc);
                        snprintf(err, (size_t)errcap,
                                 "整数求和溢出（超出 64 位整数范围）");
                        return arc;
                    }
                }
                continue;
            }

            /* 找组 / 建组（组键编码在 exec_group.c 里） */
            int isnew = 0;
            int slot = group_slot(&ac.gt, &bc, i, &isnew);
            if (slot == -1) {
                tc->am->scan_end(tc->am, scan);
                agg_ctx_free(&ac);
                batch_ctx_free(&bc);
                snprintf(err, (size_t)errcap,
                         "分组数超上限（%u 组）—— 本版不做落盘分组，请缩小分组基数",
                         (unsigned)MAX_GROUPS);
                return E_NOSPACE;
            }
            if (slot < 0) {
                tc->am->scan_end(tc->am, scan);
                agg_ctx_free(&ac);
                batch_ctx_free(&bc);
                snprintf(err, (size_t)errcap,
                         "分组键编码失败（文本过长或外部值读取失败）或分组表内存不足");
                return E_NOSPACE;
            }

            Group *g = &ac.gt.tab[slot];
            if (isnew) {
                /* 新组：把聚合的种类与列号填进累加器 */
                for (int a = 0; a < ac.naggs; a++) {
                    g->acc[a].kind = ac.aggs[a].kind;
                    g->acc[a].col = ac.aggs[a].col;
                }
            }
            for (int a = 0; a < ac.naggs; a++) {
                DbValue v;
                if (g->acc[a].col >= 0)
                    batch_cell(&bc, g->acc[a].col, i, &v);
                int arc = agg_step(&g->acc[a], g->acc[a].col >= 0 ? &v : NULL);
                if (arc != OK) {
                    tc->am->scan_end(tc->am, scan);
                    agg_ctx_free(&ac);
                    batch_ctx_free(&bc);
                    snprintf(err, (size_t)errcap,
                             "整数求和溢出（超出 64 位整数范围）");
                    return arc;
                }
            }
        }

        if (rc != OK)          /* 扫描结束 */
            break;
    }

    tc->am->scan_end(tc->am, scan);
    if (rc == E_EOF)
        rc = OK;
    if (rc != OK) {
        agg_ctx_free(&ac);
        batch_ctx_free(&bc);
        return rc;
    }

    /* ---------------- 输出 ---------------- */

    DbValue proj[SQL_MAX_COLS];

    if (ac.ngroupby == 0) {
        /* 单组：恒一行（表空也有） */
        Group g;
        memset(&g, 0, sizeof(g));
        g.acc = ac.single;

        int prc = group_project(&ac, st, &g, proj, err, errcap);
        if (prc != OK) {
            agg_ctx_free(&ac);
            batch_ctx_free(&bc);
            return prc;
        }
        int brc = b_push(b, proj);
        agg_ctx_free(&ac);
        batch_ctx_free(&bc);
        return brc;
    }

    /* 有 GROUP BY：按组键排序后逐组输出 */
    {
        u32 n = ac.gt.ngroups;
        u32 *order = malloc((size_t)(n ? n : 1) * sizeof(u32));
        if (!order) {
            agg_ctx_free(&ac);
            batch_ctx_free(&bc);
            snprintf(err, (size_t)errcap, "内存不足");
            return E_IO;
        }

        u32 k = 0;
        for (u32 i = 0; i < ac.gt.tabcap; i++)
            if (ac.gt.tab[i].used)
                order[k++] = i;

        /* 插入排序：组数不多（≤ MAX_GROUPS），简单够用 */
        for (u32 i = 1; i < n; i++) {
            u32 v = order[i];
            u32 j = i;
            while (j > 0) {
                const Group *g1 = &ac.gt.tab[order[j - 1]];
                const Group *g2 = &ac.gt.tab[v];
                u32 m = g1->keylen < g2->keylen ? g1->keylen : g2->keylen;
                int c = m ? memcmp(g1->key, g2->key, m) : 0;
                if (c == 0)
                    c = (g1->keylen == g2->keylen) ? 0
                       : (g1->keylen < g2->keylen ? -1 : 1);
                if (c <= 0)
                    break;
                order[j] = order[j - 1];
                j--;
            }
            order[j] = v;
        }

        for (u32 i = 0; i < n; i++) {
            if (st->limit >= 0 && b->r->nrows >= st->limit)
                break;

            const Group *g = &ac.gt.tab[order[i]];
            int prc = group_project(&ac, st, g, proj, err, errcap);
            if (prc != OK) {
                free(order);
                agg_ctx_free(&ac);
                batch_ctx_free(&bc);
                return prc;
            }
            int brc = b_push(b, proj);
            if (brc != OK) {
                free(order);
                agg_ctx_free(&ac);
                batch_ctx_free(&bc);
                return brc;
            }
        }

        free(order);
    }

    agg_ctx_free(&ac);
    batch_ctx_free(&bc);
    return OK;
}
