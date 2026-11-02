/*
 * db_vec.c —— 向量模态的公开 API（v1.0 步骤 5）
 *
 * 【这一层负责什么】
 *
 *   - 把模态的三样东西（vstore / hnsw / vindex）绑进一个 VectorState
 *   - 全部入口加锁（硬规则 8：新入口必须加锁）
 *   - 把错误码翻译成公共错误码
 *   - 提供"重建索引"这条自愈路径
 *
 * 【为什么向量和图的实例结构不一样】
 *
 * 图模态的实例里只有 Graph 一个对象（存储 + 记录 + 链都在里面）。
 * 向量有三层，各管各的：
 *
 *   vstore  向量数据（列式）+ 标量表   —— 真相来源
 *   hnsw    内存索引                     —— 可重建
 *   vindex  磁盘上的索引（checkpoint）   —— 可重建
 *
 * 分开的理由：hnsw 是**纯内存**的（构建快、查询快），
 * 落盘是另一件事（vindex）。这样"算法对不对"和"持久化对不对"能分开验。
 *
 * 【事务语义】
 *
 * 锁 + WAL（语句级），与 graph / blob 同族。
 * 写路径的 WAL xid 传 0（系统事务）—— 见 graph_am.h 里同样的说明：
 * 图层没在 clog 里登记真实 xid，恢复时会被当成未提交而回滚掉。
 */

#include "db_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/latch.h"
#include "modalities/vector/hnsw.h"
#include "modalities/vector/hnsw_am.h"
#include "modalities/vector/vector_am.h"
#include "modalities/vector/vector_state.h"
#include "modalities/vector/vindex.h"
#include "modalities/vector/vstore.h"

/* ---------------- 小工具 ---------------- */

static void dv_err(DB *db, const char *msg)
{
    if (db)
        snprintf(db->errmsg, sizeof(db->errmsg), "%s", msg);
}

static DbInstance *dv_inst(DB *db)
{
    return db ? db->inst : NULL;
}

/* ---------------- 注册表接入 ---------------- */

/*
 * 把 vec_items 接进注册表。
 *
 * 和 blob / graph 同款：注册表只知道"名字 → 一个 StorageAM"，
 * 它不知道向量是什么。所以加第 3 个模态不需要改注册表，更不需要改执行器。
 */
int db_vec_register(DbInstance *inst)
{
    if (!inst || !inst->vstate || !inst->vstate->vs)
        return E_INVAL;

    int rc = vector_am_build_items(inst->vstate, &inst->vec_am, &inst->vec_ctx);
    if (rc != OK)
        return rc;

    rc = hnsw_am_build(inst->vstate->vs, inst->vstate->hnsw,
                       inst->vstate->dim,
                       &inst->vec_index_am, &inst->vec_index_ctx);
    if (rc != OK)
        return rc;

    return spi_registry_add(inst->reg, "vec_items", &inst->vec_am);
}

/* ---------------- 建 / 开 ---------------- */

/*
 * 建库时（磁盘上还没有向量存储）建一个。
 */
static int dv_setup_new(DbInstance *inst, u16 dim, u16 m, u16 efc)
{
    if (!m)
        m = (u16)VEC_DEFAULT_M;
    if (!efc)
        efc = (u16)VEC_DEFAULT_EFC;
    if (m > HNSW_MAX_M)
        return E_INVAL;

    VectorState *st = (VectorState *)calloc(1, sizeof(VectorState));
    if (!st)
        return E_IO;

    st->dim = dim;
    st->m = m;
    st->efc = efc;

    int rc = vstore_create(inst->pager, db_wal_hook, inst, dim, &st->vs);
    if (rc != OK) {
        free(st);
        return rc;
    }

    rc = vindex_create(inst->pager, db_wal_hook, inst, dim, m, efc,
                       (u8)HNSW_L2, &st->vindex);
    if (rc != OK) {
        vstore_destroy(st->vs);
        free(st);
        return rc;
    }

    st->hnsw = hnsw_create(dim, HNSW_L2, m, efc, 0x5EEDu);
    if (!st->hnsw) {
        vindex_destroy(st->vindex);
        vstore_destroy(st->vs);
        free(st);
        return E_IO;
    }

    inst->vstate = st;

    rc = db_vec_register(inst);
    if (rc != OK)
        return rc;

    inst->vec_ready = true;
    return OK;
}

/*
 * 开库时（磁盘上已有向量存储）接管。
 *
 * 三步：
 *   1. 打开 vstore（拿维度）
 *   2. 打开 vindex（拿 m / efc）
 *   3. **恢复索引**：磁盘上的索引能用就读它，不能就重建
 *
 * 第 3 步是崩溃恢复的关键：索引是 checkpoint 式的，
 * 崩在两次 flush 之间时磁盘上的索引一定过期。
 *
 * 磁盘上还没有向量存储 → E_NOTFOUND（**不是错误**：
 * 那是个还没建向量存储的库，和图模态一样）。
 */
int db_vec_open(DbInstance *inst)
{
    if (!inst)
        return E_INVAL;

    VectorState *st = (VectorState *)calloc(1, sizeof(VectorState));
    if (!st)
        return E_IO;

    int rc = vstore_open(inst->pager, db_wal_hook, inst, &st->vs);
    if (rc != OK) {
        free(st);
        return rc;          /* E_NOTFOUND = 还没建过，交给调用方判断 */
    }

    rc = vindex_open(inst->pager, db_wal_hook, inst, &st->vindex);
    if (rc != OK) {
        vstore_destroy(st->vs);
        free(st);
        return rc;
    }

    st->dim = vindex_dim(st->vindex);
    st->m = vindex_m(st->vindex);
    st->efc = (u16)VEC_DEFAULT_EFC;
    if (st->dim == 0 || st->m == 0) {
        vindex_destroy(st->vindex);
        vstore_destroy(st->vs);
        free(st);
        return E_CORRUPT;
    }

    inst->vstate = st;

    /* 恢复索引：能读回就读回，读不回就重建 */
    rc = vec_state_restore(st);
    if (rc != OK)
        return rc;

    rc = db_vec_register(inst);
    if (rc != OK)
        return rc;

    inst->vec_ready = true;
    return OK;
}

/*
 * 关库：把索引落盘，然后释放。
 *
 * 【为什么要落盘】
 *
 * 索引是 checkpoint 式的（不写日志）。不落盘的话，
 * 下次开库就要**重建**一遍（读全部向量 + 重新构建，10 万点几秒）。
 * 落盘之后下次直接读回，快得多。
 *
 * 【调用时机】
 *
 * 必须在 pager_close **之前** —— flush 要写页。
 */
void db_vec_close(DbInstance *inst)
{
    if (!inst || !inst->vstate)
        return;

    VectorState *st = inst->vstate;

    if (st->hnsw && st->vindex) {
        /* 落盘失败也不能阻止关库 —— 索引本来就是可重建的 */
        (void)vindex_flush(st->vindex, 0, st->hnsw);
    }

    if (st->hnsw)
        hnsw_destroy(st->hnsw);
    vindex_destroy(st->vindex);
    vstore_destroy(st->vs);
    free(st);

    inst->vstate = NULL;
    inst->vec_ready = false;
}

/* ---------------- 公开 API ---------------- */

db_err db_vec_create(DB *db, uint16_t dim, uint16_t m, uint16_t efc)
{
    if (!db)
        return DB_E_INVAL;
    if (dim == 0)
        return DB_E_INVAL;

    DbInstance *inst = dv_inst(db);

    latch_lock();

    if (inst->vec_ready) {
        latch_unlock();
        dv_err(db, "向量存储已经建过了");
        return DB_E_INVAL;
    }

    int rc = dv_setup_new(inst, dim, m, efc);
    latch_unlock();

    if (rc != OK) {
        dv_err(db, "建向量存储失败");
        return map_rc(rc);
    }
    return DB_OK;
}

int db_vec_exists(DB *db)
{
    DbInstance *inst = dv_inst(db);
    return (inst && inst->vec_ready) ? 1 : 0;
}

uint16_t db_vec_dim(DB *db)
{
    DbInstance *inst = dv_inst(db);
    return (inst && inst->vec_ready) ? inst->vstate->dim : 0;
}

db_err db_vec_insert(DB *db, const float *vec, int32_t label,
                     uint64_t *row_id_out)
{
    if (!db || !vec)
        return DB_E_INVAL;

    DbInstance *inst = dv_inst(db);
    if (!inst->vec_ready) {
        dv_err(db, "向量存储还不存在");
        return DB_E_NOTFOUND;
    }

    VectorState *st = inst->vstate;

    latch_lock();

    /*
     * 先写数据，再写索引。
     *
     * 顺序很重要：崩在中间时，磁盘上会多一个向量、索引里少一个 ——
     * 打开时 count 对不上 → 走重建路径 → 干净。
     */
    u64 row_id = 0;
    int rc = vstore_insert(st->vs, 0, vec, label, &row_id);
    if (rc == OK) {
        u32 hid = 0;
        rc = hnsw_add(st->hnsw, vec, &hid);
        if (rc == OK && hid != (u32)row_id) {
            /* 索引和数据的序号对不上 —— 致命的不一致 */
            rc = E_CORRUPT;
        }
    }

    latch_unlock();

    if (rc != OK) {
        dv_err(db, "插入向量失败");
        return map_rc(rc);
    }

    if (row_id_out)
        *row_id_out = row_id;
    return DB_OK;
}

/*
 * 近邻查询。
 *
 * 走 IndexAM 的 knn —— v0.9 留的那个口子。
 * 这样"knn 有真实现"不是一句口号，而是公开 API 真的在用它。
 */
db_err db_vec_knn(DB *db, const float *q, uint32_t k, uint16_t ef,
                  db_vec_hit *out, uint32_t *n_out)
{
    if (!db || !q || !out || !n_out)
        return DB_E_INVAL;
    if (k == 0) {
        *n_out = 0;
        return DB_OK;
    }

    DbInstance *inst = dv_inst(db);
    if (!inst->vec_ready) {
        dv_err(db, "向量存储还不存在");
        return DB_E_NOTFOUND;
    }

    Rid *rids = (Rid *)calloc(k, sizeof(Rid));
    float *dists = (float *)calloc(k, sizeof(float));
    if (!rids || !dists) {
        free(rids);
        free(dists);
        return DB_E_IO;
    }

    u16 dim = inst->vstate->dim;

    latch_lock();

    /* ef 是查询参数，IndexAM 的签名里没有 —— 放在 ctx 里（锁内安全） */
    inst->vec_index_ctx.ef = ef ? ef : (u16)VEC_DEFAULT_EF;

    int nn = 0;
    int rc = inst->vec_index_am.knn(&inst->vec_index_am, (const u8 *)q,
                                    (u16)((u32)dim * 4u),
                                    (int)k, rids, dists, &nn);
    latch_unlock();

    if (rc != OK) {
        free(rids);
        free(dists);
        dv_err(db, "近邻查询失败");
        return map_rc(rc);
    }

    /* RID → row_id */
    u16 kk = vstore_k(inst->vstate->vs);
    for (int i = 0; i < nn; i++) {
        u32 ord = 0;
        if (vstore_ordinal_of_page(inst->vstate->vs, rids[i].page, &ord) != OK) {
            free(rids);
            free(dists);
            dv_err(db, "索引与数据不一致");
            return DB_E_NOTFOUND;
        }
        out[i].row_id = (u64)ord * (u64)kk + (u64)rids[i].slot;
        out[i].dist = dists[i];
    }

    free(rids);
    free(dists);
    *n_out = (uint32_t)nn;
    return DB_OK;
}

/*
 * 精确近邻（暴力扫）。
 *
 * 【为什么这里不能用 hnsw_bruteforce】
 *
 * hnsw_bruteforce 算的是 HNSW 内部那份向量拷贝 —— 它在内存里，
 * 一个页都不读。而这里要的是"**从存储扫一遍**"：
 * 它读全部向量页，正是 A4 里那个"重"的对照组。
 *
 * 用同一个数字去比"轻"，比的就不是 IO 了。
 */
db_err db_vec_knn_exact(DB *db, const float *q, uint32_t k,
                        db_vec_hit *out, uint32_t *n_out)
{
    if (!db || !q || !out || !n_out)
        return DB_E_INVAL;
    if (k == 0) {
        *n_out = 0;
        return DB_OK;
    }

    DbInstance *inst = dv_inst(db);
    if (!inst->vec_ready) {
        dv_err(db, "向量存储还不存在");
        return DB_E_NOTFOUND;
    }

    u64 n = 0;
    u16 dim = inst->vstate->dim;

    latch_lock();

    n = vstore_count(inst->vstate->vs);

    float *buf = NULL;
    if (n > 0) {
        buf = (float *)malloc((size_t)n * dim * sizeof(float));
        if (!buf) {
            latch_unlock();
            return DB_E_IO;
        }
        u64 got = 0;
        int rc = vstore_scan_all(inst->vstate->vs, buf, &got);
        if (rc != OK || got != n) {
            free(buf);
            latch_unlock();
            dv_err(db, "扫描向量失败");
            return map_rc(rc == OK ? E_CORRUPT : rc);
        }
    }

    /* 容量 k 的最大堆：堆顶是当前最远的那个 */
    db_vec_hit *heap = (db_vec_hit *)calloc(k, sizeof(db_vec_hit));
    if (!heap) {
        free(buf);
        latch_unlock();
        return DB_E_IO;
    }
    u32 hn = 0;

    for (u64 i = 0; i < n; i++) {
        const float *v = buf + i * dim;
        float d = 0.0f;
        for (u16 x = 0; x < dim; x++) {
            float t = v[x] - q[x];
            d += t * t;
        }

        if (hn < k) {
            heap[hn].row_id = i;
            heap[hn].dist = d;
            hn++;
            /* 上浮（最大堆：越远越靠上） */
            u32 j = hn - 1;
            while (j > 0) {
                u32 p = (j - 1) / 2;
                if (heap[p].dist >= heap[j].dist)
                    break;
                db_vec_hit t2 = heap[p];
                heap[p] = heap[j];
                heap[j] = t2;
                j = p;
            }
        } else if (d < heap[0].dist) {
            /* 换掉最远的，再上浮 */
            heap[0].row_id = i;
            heap[0].dist = d;
            u32 j = 0;
            for (;;) {
                u32 l = 2 * j + 1;
                u32 r = l + 1;
                u32 best = j;
                if (l < hn && heap[l].dist > heap[best].dist)
                    best = l;
                if (r < hn && heap[r].dist > heap[best].dist)
                    best = r;
                if (best == j)
                    break;
                db_vec_hit t2 = heap[j];
                heap[j] = heap[best];
                heap[best] = t2;
                j = best;
            }
        }
    }

    free(buf);

    /*
     * 堆排序：把最大堆排成**降序**，再倒过来写 → 升序。
     *
     * 【坑】
     *
     * 第一版直接写 `out[i] = heap[hn-1-i]`，以为堆的数组是排好的 ——
     * 它不是。堆只保证"堆顶是最大"，其余元素的顺序是任意的。
     * 表现是：结果集里的距离**看起来是对的集合，但顺序乱的**
     * （0.25, 0.0, 0.08, 0.69, 0.34 …）。
     *
     * 下游如果按顺序取前 k 个，就会拿到错的东西。
     * 这类错只有在"和另一个实现逐条对比"时才看得出来 ——
     * 集合对了、顺序错了，普通的断言验不出来。
     */
    for (u32 i = hn; i > 0; i--) {
        db_vec_hit t = heap[0];
        heap[0] = heap[i - 1];
        heap[i - 1] = t;

        u32 size = i - 1;
        u32 j = 0;
        while (j < size) {
            u32 l = 2 * j + 1;
            u32 r = l + 1;
            u32 best = j;
            if (l < size && heap[l].dist > heap[best].dist)
                best = l;
            if (r < size && heap[r].dist > heap[best].dist)
                best = r;
            if (best == j)
                break;
            db_vec_hit t2 = heap[j];
            heap[j] = heap[best];
            heap[best] = t2;
            j = best;
        }
    }

    latch_unlock();

    /*
     * 现在 heap[0..hn-1] 是**升序**。
     *
     * 【别搞反】
     *
     * 最大堆做堆排序（每轮把堆顶的最大换到末尾、堆大小减一），
     * 结果是**升序** —— 最大的被换到了数组末尾，最小的留在开头。
     *
     * 所以直接按顺序抄就行，不需要再倒一次。
     * （我第一版先忘了排、再倒着抄，两处都错 —— 结果是乱的。）
     */
    for (u32 i = 0; i < hn; i++)
        out[i] = heap[i];

    free(heap);
    *n_out = hn;
    return DB_OK;
}

/*
 * 带过滤的近邻查询。
 *
 * 判决 (c)：候选池至少 4k，然后过滤，如实报告实际数量。
 */
db_err db_vec_knn_filtered(DB *db, const float *q, uint32_t k, uint16_t ef,
                           int32_t label, db_vec_hit *out, uint32_t *n_out)
{
    if (!db || !q || !out || !n_out)
        return DB_E_INVAL;
    if (k == 0) {
        *n_out = 0;
        return DB_OK;
    }

    DbInstance *inst = dv_inst(db);
    if (!inst->vec_ready) {
        dv_err(db, "向量存储还不存在");
        return DB_E_NOTFOUND;
    }

    /* 候选池至少 4k —— 这就是"给超集，不给子集"的实现 */
    u32 cand = (u32)k * 4u;
    if (ef > cand)
        cand = ef;
    if (cand < k)
        cand = k;

    db_vec_hit *tmp = (db_vec_hit *)calloc(cand, sizeof(db_vec_hit));
    if (!tmp)
        return DB_E_IO;

    u32 n = 0;
    db_err e = db_vec_knn(db, q, cand, (uint16_t)cand, tmp, &n);
    if (e != DB_OK) {
        free(tmp);
        return e;
    }

    latch_lock();
    u32 m = 0;
    for (u32 i = 0; i < n; i++) {
        i32 lab = 0;
        if (vstore_label(inst->vstate->vs, tmp[i].row_id, &lab) != OK)
            continue;
        if (lab == label)
            out[m++] = tmp[i];
    }
    latch_unlock();

    /*
     * 截断到 k。
     *
     * 【为什么候选池扩大了还要截断】
     *
     * 候选池扩到 4k 是为了**保证有足够的命中项填满 k**，
     * 不是为了返回 4k 个。过滤完还剩 20 个而用户只要 5 个时，
     * 就该给最近的 5 个 —— 多给是另一种错（"返回了用户没要的行"）。
     *
     * 截断之后数量 ≤ k，且可能 < k（命中不足时）。
     */
    if (m > k)
        m = k;

    free(tmp);

    /*
     * 如实报告实际数量 —— 可能少于 k。
     * 这不是 bug，是 ANN 的固有性质（近似）。
     */
    *n_out = m;
    return DB_OK;
}

db_err db_vec_count(DB *db, uint64_t *n)
{
    if (!db || !n)
        return DB_E_INVAL;

    DbInstance *inst = dv_inst(db);
    if (!inst->vec_ready)
        return DB_E_NOTFOUND;

    latch_lock();
    *n = vstore_count(inst->vstate->vs);
    latch_unlock();
    return DB_OK;
}

uint64_t db_vec_page_reads(DB *db)
{
    DbInstance *inst = dv_inst(db);
    if (!inst || !inst->vec_ready)
        return 0;
    return vstore_page_reads(inst->vstate->vs) +
           vstore_meta_reads(inst->vstate->vs);
}

/* ---------------- 索引落盘 / 重建 ---------------- */

db_err db_vec_flush(DB *db)
{
    if (!db)
        return DB_E_INVAL;

    DbInstance *inst = dv_inst(db);
    if (!inst->vec_ready)
        return DB_E_NOTFOUND;

    latch_lock();
    int rc = vindex_flush(inst->vstate->vindex, 0, inst->vstate->hnsw);
    latch_unlock();

    if (rc != OK) {
        dv_err(db, "索引落盘失败");
        return map_rc(rc);
    }
    return DB_OK;
}

db_err db_vec_reindex(DB *db)
{
    if (!db)
        return DB_E_INVAL;

    DbInstance *inst = dv_inst(db);
    if (!inst->vec_ready)
        return DB_E_NOTFOUND;

    latch_lock();
    int rc = vec_state_reindex(inst->vstate, NULL);
    latch_unlock();

    if (rc != OK) {
        dv_err(db, "重建索引失败");
        return map_rc(rc);
    }
    return DB_OK;
}
