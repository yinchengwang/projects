/*
 * db_geo_query.c —— 地理空间模态的**查询实现**（v1.4 步骤 5）
 *
 * 【为什么单独一个文件】
 *
 * db_geo.c 是"公开 API + 实例接线"，已经很长了（硬规则：单文件 ≤ 800 行）。
 * 窗口查询与近邻查询是其中最长的两段实现，拆到这里。
 *
 * 公开 API 的声明仍在 include/db.h（位置没变），db_geo.c 保留"公开 API
 * + 实例接线"，窗口/近邻这两个最长的实现搬到这里。
 *
 * 【这两个查询共同的坎】
 *
 * 1. **死条目**（坑 M4-39）：删除是"给叶子条目的 id 打最高位"，而
 *    rtree.c / rtree_knn.c 写得比 rtree_scan.c 早，不认这一位 ——
 *    死条目会被照常吐出来，过滤的活儿落在本层。
 * 2. **距离**（坑 M4-40）：rtree_knn 只返回 row_id，不返回距离；而公开
 *    API 的 db_geo_hit 要带平方距离 —— 所以要**额外扫一遍树**把坐标捞
 *    回来自己算。这一遍是本轮的额外成本，如实记下。
 * 3. **次序**：一律 "(距离升序, row_id 升序)"，与暴力基准
 *    （geo_brute.c）、与计划 §1.2 判决 4 一致 —— 否则"逐位对照"不成立。
 */

#include "db_internal.h"

#include <stdio.h>
#include <stdlib.h>

#include "common/latch.h"
#include "modalities/geo/mbr.h"
#include "modalities/geo/rtree_knn.h"
#include "modalities/geo/rtree_scan.h"

/* 死条目标志：叶子条目 id 的最高位（rtree_scan.h 的约定） */
#define GEO_DEAD_BIT  0x80000000u
#define GEO_ROW_MASK  0x7FFFFFFFu

/* 命中排序：(距离升序, row_id 升序) */
static int dg_hit_cmp(const void *a, const void *b)
{
    const db_geo_hit *x = (const db_geo_hit *)a;
    const db_geo_hit *y = (const db_geo_hit *)b;

    if (x->dist != y->dist)
        return (x->dist < y->dist) ? -1 : 1;
    if (x->row_id != y->row_id)
        return (x->row_id < y->row_id) ? -1 : 1;
    return 0;
}

/*
 * 窗口查询：返回落在矩形内的点的 row_id（闭区间）。
 *
 * 【为什么要多一个临时缓冲】
 *
 * 树里的死条目**也会被窗口命中**（坑 M4-39），所以"树返回的条数"
 * 与"活着的命中数"不是一回事。
 *
 * 做法：先按用户的 max_out 试一次；装不下（E_NOSPACE）就按"总条目数"
 * 再试一次（这时肯定装得下），滤掉死条目之后才判断活着的命中数是否
 * 超过 max_out。
 */
db_err db_geo_window(DB *db, float minx, float miny, float maxx, float maxy,
                     uint32_t *out_ids, uint32_t max_out, uint32_t *n_out)
{
    if (!db || !out_ids || !n_out)
        return DB_E_INVAL;

    DbInstance *inst = db->inst;
    if (!inst->geo_ready) {
        if (db)
            snprintf(db->errmsg, sizeof(db->errmsg), "%s", "几何存储还不存在");
        return DB_E_NOTFOUND;
    }

    GeoState *st = inst->geo;

    latch_lock();

    Mbr win;
    mbr_set_rect(&win, minx, miny, maxx, maxy);   /* 自动归一化 min/max */

    u32 cap = max_out ? max_out : 1;
    u32 *tmp = (u32 *)malloc((size_t)cap * sizeof(u32));
    if (!tmp) {
        latch_unlock();
        return DB_E_IO;
    }

    u32 n = 0;
    int rc = rtree_window(st->tree, &win, tmp, cap, &n);

    if (rc == E_NOSPACE && st->n_total > cap) {
        /* 装不下：按总条目数重试一次（这次肯定装得下） */
        u32 *big = (u32 *)malloc((size_t)st->n_total * sizeof(u32));
        if (!big) {
            free(tmp);
            latch_unlock();
            return DB_E_IO;
        }
        free(tmp);
        tmp = big;
        cap = st->n_total;
        n = 0;
        rc = rtree_window(st->tree, &win, tmp, cap, &n);
    }

    if (rc != OK) {
        free(tmp);
        latch_unlock();
        snprintf(db->errmsg, sizeof(db->errmsg), "%s", "窗口查询失败");
        return map_rc(rc);
    }

    /* 滤掉死条目 */
    u32 m = 0;
    bool overflow = false;
    for (u32 i = 0; i < n; i++) {
        if (tmp[i] & GEO_DEAD_BIT)
            continue;                    /* 死条目不算命中 */
        if (m >= max_out) {
            overflow = true;
            break;
        }
        out_ids[m++] = tmp[i] & GEO_ROW_MASK;
    }

    free(tmp);
    latch_unlock();

    if (overflow) {
        snprintf(db->errmsg, sizeof(db->errmsg), "%s", "窗口结果装不下（max_out 太小）");
        return DB_E_NOSPACE;
    }

    *n_out = m;
    return DB_OK;
}

/*
 * 近邻查询（精确 KNN）。
 *
 * 【为什么要扫两遍树】
 *
 * ① rtree_knn 给出最近的若干 row_id（**不带距离**，坑 M4-40）；
 * ② 为了填 db_geo_hit 的距离字段，要再扫一遍树把坐标捞回来。
 * 第 ② 遍是本轮的**额外成本**，如实记在这里。
 *
 * 树不知道哪些条目是死的 —— 死条目会混在结果里占位，所以向树要
 * "k + 死条目数 + 余量" 个候选，滤掉死条目之后才截到 k 个。
 * 余量是为了挡住"距离并列时边界上的那一个"。
 */
db_err db_geo_knn(DB *db, float qx, float qy, uint32_t k,
                  db_geo_hit *out, uint32_t max_out, uint32_t *n_out)
{
    if (!db || !out || !n_out)
        return DB_E_INVAL;

    if (k == 0) {
        *n_out = 0;
        return DB_OK;
    }

    DbInstance *inst = db->inst;
    if (!inst->geo_ready) {
        snprintf(db->errmsg, sizeof(db->errmsg), "%s", "几何存储还不存在");
        return DB_E_NOTFOUND;
    }

    GeoState *st = inst->geo;

    latch_lock();

    /* ① 摊平全树：拿坐标 + 死条目数 */
    GeoTab tab;
    int rc = db_geo_flatten(inst, &tab);
    if (rc != OK) {
        latch_unlock();
        snprintf(db->errmsg, sizeof(db->errmsg), "%s", "扫描几何树失败");
        return map_rc(rc);
    }

    u32 need = (k < tab.live) ? k : tab.live;

    if (need > max_out) {
        db_geo_tab_free(&tab);
        latch_unlock();
        snprintf(db->errmsg, sizeof(db->errmsg), "%s", "近邻结果装不下（max_out 太小）");
        return DB_E_NOSPACE;
    }

    /* ② 向树要候选：k + 死条目 + 余量 */
    u32 kk = k + tab.dead_n + 16u;
    if (kk > st->n_total)
        kk = st->n_total;
    if (kk < need)
        kk = need;
    if (kk == 0) {
        db_geo_tab_free(&tab);
        latch_unlock();
        *n_out = 0;
        return DB_OK;
    }

    u32 *ids = (u32 *)malloc((size_t)kk * sizeof(u32));
    if (!ids) {
        db_geo_tab_free(&tab);
        latch_unlock();
        return DB_E_IO;
    }

    u32 nid = 0;
    u64 reads = 0;
    rc = rtree_knn(inst->pager, rtree_root_page(st->tree), qx, qy, kk,
                    ids, kk, &nid, &reads);
    st->page_reads += reads;

    if (rc != OK) {
        free(ids);
        db_geo_tab_free(&tab);
        latch_unlock();
        snprintf(db->errmsg, sizeof(db->errmsg), "%s", "近邻查询失败");
        return map_rc(rc);
    }

    /* ③ 滤死条目 + 补距离（结果直接写进 out：need <= max_out） */
    u32 m = 0;
    for (u32 i = 0; i < nid && m < need; i++) {
        u32 id = ids[i] & GEO_ROW_MASK;
        if (id >= tab.cap)
            continue;                    /* 不该发生：row_id 越界 */
        if (tab.p[id].dead)
            continue;                    /* 死条目不算 */
        out[m].row_id = (uint64_t)id;
        out[m].dist = geo_dist2(qx, qy, tab.p[id].x, tab.p[id].y);
        m++;
    }

    free(ids);
    db_geo_tab_free(&tab);

    /*
     * ④ 排序：(距离升序, row_id 升序)。自己再排一遍是因为本层要用
     * **与暴力基准同一套算术**（geo_dist2）定死次序。
     */
    if (m > 1)
        qsort(out, m, sizeof(out[0]), dg_hit_cmp);

    latch_unlock();

    *n_out = m;
    return DB_OK;
}
