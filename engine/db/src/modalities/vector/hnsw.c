/*
 * hnsw.c — 分层可导航小世界图（v1.0 步骤 3）
 *
 * 设计说明见 modalities/vector/hnsw.h。
 *
 * 【实现顺序：先让 recall 对，再让它快】
 *
 * 这里的每一处都按论文 / hnswlib 的原文来，没有"我觉得这样更好"的改动。
 * 原因：HNSW 的错法几乎都是**静默**的 —— 修剪判据抄错、层级分配写偏，
 * 结果照样返回 k 个结果，只是 recall 从 0.95 掉到 0.6。
 * 一旦自己发挥，出问题时就分不清是"参数不对"还是"实现不对"。
 *
 * 【本文件不碰 pager】
 *
 * 纯内存。落盘（邻接页序列化）在步骤 4 —— 两件事分开，
 * recall 不达标时不用怀疑是 IO 的问题。
 */

#include "modalities/vector/hnsw_internal.h"

/* ---------------- 向量访问 ---------------- */

const float *hnsw_vec(const Hnsw *h, u32 id)
{
    return h->vecs + (size_t)id * h->dim;
}

/* ---------------- 随机数与层级分配 ---------------- */

static u64 rng_next(Hnsw *h)
{
    /* splitmix64：状态推进 + 搅匀，低位也够乱 */
    u64 z = (h->rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/*
 * 层级分配。
 *
 * 论文的式子是 level = floor(-ln(uniform) × mL)，mL = 1/ln(M)，
 * 等价于 P(level >= k) = M^-k。
 *
 * 这里用**几何分布**直接抽：每层以 1/M 的概率继续往上。
 * 分布完全一样（P(level>=k) = M^-k），但不需要 libm ——
 * 一次浮点 log 都不用算，也不用给整个项目加 -lm。
 *
 * 平均层数 = 1/(1 - 1/M)，M=16 时约 1.067 层；
 * 也就是说 ~6% 的节点有第 1 层，~0.4% 有第 2 层。
 */
static u16 random_level(Hnsw *h)
{
    u16 lvl = 0;
    while (lvl < HNSW_MAX_LEVEL && (rng_next(h) % (u64)h->m) == 0)
        lvl++;
    return lvl;
}

/* ---------------- 容量管理 ---------------- */

static int ensure_cap(Hnsw *h, u32 need)
{
    if (need <= h->cap)
        return OK;

    u32 nc = h->cap ? h->cap : 64u;
    while (nc < need)
        nc *= 2u;

    float *nv = (float *)realloc(h->vecs,
                                 (size_t)nc * h->dim * sizeof(float));
    if (!nv)
        return E_IO;
    h->vecs = nv;

    u8 *nl = (u8 *)realloc(h->level, nc);
    if (!nl)
        return E_IO;
    h->level = nl;

    u32 *no = (u32 *)realloc(h->link_off, (size_t)nc * sizeof(u32));
    if (!no)
        return E_IO;
    h->link_off = no;

    u16 *ncnt = (u16 *)realloc(h->cnt, (size_t)nc *
                               (HNSW_MAX_LEVEL + 1) * sizeof(u16));
    if (!ncnt)
        return E_IO;
    h->cnt = ncnt;

    u32 *nvis = (u32 *)realloc(h->visited, (size_t)nc * sizeof(u32));
    if (!nvis)
        return E_IO;
    memset(nvis + h->cap, 0, (size_t)(nc - h->cap) * sizeof(u32));
    h->visited = nvis;
    h->vcap = nc;

    h->cap = nc;
    return OK;
}

static int pool_reserve(Hnsw *h, u32 add)
{
    if (h->pool_used + add <= h->pool_cap)
        return OK;

    u32 nc = h->pool_cap ? h->pool_cap : 1024u;
    while (nc < h->pool_used + add)
        nc *= 2u;

    u32 *p = (u32 *)realloc(h->pool, (size_t)nc * sizeof(u32));
    if (!p)
        return E_IO;
    h->pool = p;
    h->pool_cap = nc;
    return OK;
}

static int disc_reserve(Hnsw *h, u32 need)
{
    if (need <= h->disc_cap)
        return OK;

    u32 nc = h->disc_cap ? h->disc_cap : 64u;
    while (nc < need)
        nc *= 2u;

    u32 *p = (u32 *)realloc(h->disc, (size_t)nc * sizeof(u32));
    if (!p)
        return E_IO;
    h->disc = p;
    h->disc_cap = nc;
    return OK;
}

/* ---------------- 搜索一层 ---------------- */

/*
 * 在第 lc 层做宽度为 ef 的贪心广度搜索（论文 Algorithm 2）。
 *
 * 两个堆：
 *   cand —— 最小堆，下一个要展开的点
 *   res  —— 最大堆，当前最好的 ef 个（堆顶最远，用来判断"还要不要继续"）
 *
 * 终止条件：下一个候选比"当前结果里最远的那个"还远 ——
 * 再走只会更远，没有意义。这是 HNSW 快的关键。
 */
int hnsw_search_layer(Hnsw *h, const float *q, u32 ep, u16 ef, u16 lc,
                      Heap *res)
{
    /* visited 用 epoch 标记：自增一次就等于全部清零，O(1) */
    if (h->epoch == 0xFFFFFFFFu) {
        memset(h->visited, 0, (size_t)h->vcap * sizeof(u32));
        h->epoch = 0;
    }
    h->epoch++;

    h_clear(&h->cand);
    h_clear(res);

    /* 本次搜索访问的节点数（诊断用；见 hnsw_internal.h 里的说明） */
    h->visits = 0;

    float d0 = vec_dist(h, q, hnsw_vec(h, ep));
    h->visited[ep] = h->epoch;
    h->visits++;

    int rc = h_push(&h->cand, d0, ep);
    if (rc != OK)
        return rc;
    rc = h_push(res, d0, ep);
    if (rc != OK)
        return rc;

    while (h->cand.n > 0) {
        HItem c = h_pop(&h->cand);

        if (c.d > h_top(res).d)
            break;

        u16 cnt = *cnt_at(h, c.id, lc);
        const u32 *ls = links_at(h, c.id, lc);

        for (u16 i = 0; i < cnt; i++) {
            u32 e = ls[i];
            if (e >= h->n)
                return E_CORRUPT;
            if (h->visited[e] == h->epoch)
                continue;
            h->visited[e] = h->epoch;
            h->visits++;

            float de = vec_dist(h, q, hnsw_vec(h, e));

            if (res->n < ef || de < h_top(res).d) {
                rc = h_push(&h->cand, de, e);
                if (rc != OK)
                    return rc;
                rc = h_push(res, de, e);
                if (rc != OK)
                    return rc;
                if (res->n > ef)
                    h_pop(res);
            }
        }
    }
    return OK;
}

/* 上层用的贪心下降：走到局部最近为止（ef=1 的特例，比 search_layer 快） */
u32 hnsw_greedy_step(Hnsw *h, const float *q, u32 ep, u16 lc)
{
    float best = vec_dist(h, q, hnsw_vec(h, ep));

    bool moved = true;
    while (moved) {
        moved = false;
        u16 cnt = *cnt_at(h, ep, lc);
        const u32 *ls = links_at(h, ep, lc);

        for (u16 i = 0; i < cnt; i++) {
            u32 e = ls[i];
            if (e >= h->n)
                continue;
            float d = vec_dist(h, q, hnsw_vec(h, e));
            if (d < best) {
                best = d;
                ep = e;
                moved = true;
            }
        }
    }
    return ep;
}

/* ---------------- 邻居选择（论文 Algorithm 4） ---------------- */

/*
 * 启发式选邻居。
 *
 * 朴素做法是"取最近的 m 个"，但那样会让邻居**全挤在同一个方向**：
 * 从任何方向进来都只能看到那一小片，图变成一条条孤立的带子。
 *
 * 启发式加了一条判据：候选 e 只有在"它离 q 比离已选的任何一个邻居都近"
 * 时才入选。换句话说 —— **e 必须在某个新方向上**。
 * 这样邻居集合是"发散"的，从任何方向都能找到路。
 *
 * 论文 Algorithm 4 的 e0/e1/e2 判据就是这个意思（三个特例合并成一句比较）。
 *
 * 选不满 m 个时（方向不够发散）用被丢弃的补上（keepPruned）——
 * 否则度数太低，图会断。
 */
static int select_heuristic(Hnsw *h, HItem *cand, u32 ncand,
                            u16 mmax, u32 *out, u32 *nsel_out)
{
    if (ncand == 0) {
        *nsel_out = 0;
        return OK;
    }

    qsort(cand, ncand, sizeof(HItem), cmp_asc);

    int rc = disc_reserve(h, ncand);
    if (rc != OK)
        return rc;

    u32 nsel = 0;
    u32 ndisc = 0;

    for (u32 i = 0; i < ncand && nsel < mmax; i++) {
        const float *ev = hnsw_vec(h, cand[i].id);
        bool good = true;

        for (u32 j = 0; j < nsel; j++) {
            float d = vec_dist(h, ev, hnsw_vec(h, out[j]));
            /* e 离某个已选邻居比离 q 还近 → 冗余，不要 */
            if (d < cand[i].d) {
                good = false;
                break;
            }
        }

        if (good)
            out[nsel++] = cand[i].id;
        else
            h->disc[ndisc++] = cand[i].id;
    }

    /* 保底：填满，别让度数太低 */
    for (u32 i = 0; i < ndisc && nsel < mmax; i++)
        out[nsel++] = h->disc[i];

    *nsel_out = nsel;
    return OK;
}

/* ---------------- 建 / 拆 ---------------- */

Hnsw *hnsw_create(u16 dim, HnswMetric metric, u16 m, u16 efc, u64 seed)
{
    if (dim == 0 || m == 0 || m > HNSW_MAX_M)
        return NULL;

    Hnsw *h = (Hnsw *)calloc(1, sizeof(Hnsw));
    if (!h)
        return NULL;

    h->dim = dim;
    h->m = m;
    h->m0 = (u16)(m * 2u);
    h->efc = efc ? efc : (u16)200;
    h->metric = (u8)metric;
    h->rng = seed ? seed : 0x243F6A8885A308D3ull;
    h->entry = 0;
    h->max_level = 0;

    h->cand.max = false;   /* 候选：最小堆 */
    h->res.max = true;     /* 结果：最大堆 */

    return h;
}

void hnsw_destroy(Hnsw *h)
{
    if (!h)
        return;
    free(h->vecs);
    free(h->level);
    free(h->link_off);
    free(h->cnt);
    free(h->pool);
    free(h->visited);
    free(h->cand.a);
    free(h->res.a);
    free(h->scratch);
    free(h->disc);
    free(h);
}

/* ---------------- 插入 ---------------- */

/*
 * 加一条出边。**满了不能直接丢** —— 这是本文件最容易写错的地方。
 *
 * 错误的写法（第一版就是这么写的）：
 *   if (满了) { 重挑现有邻居 → 还是满的 → 丢弃新边 }
 * 后果：每个节点的表一旦填满就**再也不接受新邻居**，
 * 图退化成"新点只指向旧点"的有向图。从入口点出发只能走到
 * 入口点附近那一小片 —— 实测 2000 个点里只有 35 个可达，
 * recall 掉到 0.1 以下，而且**不报任何错**。
 *
 * 正确写法：把**新候选也放进池子**，和现有邻居一起重挑 mmax 个。
 * 新的可能挤掉一个旧的，也可能自己落选 —— 由同一个启发式判据决定。
 * hnswlib 的 getNeighborsByHeuristic2 就是这么做的。
 */
static int link_merge(Hnsw *h, u32 id, u32 to, u16 lc)
{
    u16 mmax = max_links(h, lc);
    u16 *pc = cnt_at(h, id, lc);
    u32 *ls = links_at(h, id, lc);

    if (*pc < mmax) {
        ls[*pc] = to;
        (*pc)++;
        return OK;
    }

    int rc = scratch_reserve(h, (u32)(*pc + 1));
    if (rc != OK)
        return rc;

    const float *self = hnsw_vec(h, id);
    for (u16 i = 0; i < *pc; i++) {
        h->scratch[i].id = ls[i];
        h->scratch[i].d = vec_dist(h, self, hnsw_vec(h, ls[i]));
    }
    h->scratch[*pc].id = to;
    h->scratch[*pc].d = vec_dist(h, self, hnsw_vec(h, to));

    u32 nsel = 0;
    rc = select_heuristic(h, h->scratch, (u32)(*pc + 1), mmax, ls, &nsel);
    if (rc != OK)
        return rc;

    *pc = (u16)nsel;
    return OK;
}

int hnsw_add(Hnsw *h, const float *vec, u32 *id_out)
{
    if (!h || !vec)
        return E_INVAL;

    int rc = ensure_cap(h, h->n + 1);
    if (rc != OK)
        return rc;

    u32 id = h->n;
    memcpy(h->vecs + (size_t)id * h->dim, vec,
           (size_t)h->dim * sizeof(float));

    u16 lvl = random_level(h);
    h->level[id] = (u8)lvl;
    memset(cnt_at(h, id, 0), 0, (HNSW_MAX_LEVEL + 1) * sizeof(u16));

    u32 nslots = (u32)h->m0 + (u32)lvl * h->m;
    rc = pool_reserve(h, nslots);
    if (rc != OK)
        return rc;
    h->link_off[id] = h->pool_used;
    h->pool_used += nslots;
    h->n++;

    /* 第一个点：它就是入口 */
    if (h->n == 1) {
        h->entry = id;
        h->max_level = lvl;
        if (id_out)
            *id_out = id;
        return OK;
    }

    u32 ep = h->entry;

    /* 1) 从最高层贪心下降到 lvl+1（这一段只走单点，不算距离集） */
    for (int lc = (int)h->max_level; lc > (int)lvl; lc--)
        ep = hnsw_greedy_step(h, vec, ep, (u16)lc);

    /* 2) 从 min(lvl, max_level) 逐层往下，每层做一次 ef 宽的搜索 */
    int top = ((int)lvl < (int)h->max_level) ? (int)lvl : (int)h->max_level;

    u32 sel[HNSW_MAX_M * 2];

    for (int lc = top; lc >= 0; lc--) {
        rc = hnsw_search_layer(h, vec, ep, h->efc, (u16)lc, &h->res);
        if (rc != OK)
            return rc;

        u16 mmax = max_links(h, (u16)lc);

        /* 结果集搬出来排序（select 会原地 qsort） */
        u32 ncand = h->res.n;
        rc = scratch_reserve(h, ncand);
        if (rc != OK)
            return rc;
        for (u32 i = 0; i < ncand; i++)
            h->scratch[i] = h->res.a[i];

        u32 nsel = 0;
        rc = select_heuristic(h, h->scratch, ncand, mmax, sel, &nsel);
        if (rc != OK)
            return rc;

        /* 本节点在这一层的邻居表 */
        u32 *mine = links_at(h, id, (u16)lc);
        for (u32 i = 0; i < nsel; i++)
            mine[i] = sel[i];
        *cnt_at(h, id, (u16)lc) = (u16)nsel;

        /*
         * 双向连：对方也要记住我。
         *
         * 为什么必须双向：HNSW 的搜索是沿出边走，
         * 只连单向的话新点永远搜不到（没人指向它）。
         *
         * 对方满了就修剪 —— 用同一个启发式判据，
         * 把"最该留的 m 个"重新挑一遍。
         */
        for (u32 i = 0; i < nsel; i++) {
            rc = link_merge(h, sel[i], id, (u16)lc);
            if (rc != OK)
                return rc;
        }

        /* 3) 下一层的入口：本层结果里最近的那个 */
        ep = h->res.a[0].id;
        float bd = h->res.a[0].d;
        for (u32 i = 1; i < h->res.n; i++) {
            if (h->res.a[i].d < bd) {
                bd = h->res.a[i].d;
                ep = h->res.a[i].id;
            }
        }
    }

    /* 4) 比现有最高层还高 → 换入口 */
    if (lvl > h->max_level) {
        h->max_level = lvl;
        h->entry = id;
    }

    if (id_out)
        *id_out = id;
    return OK;
}

/* ---------------- 反序列化专用 ---------------- */

int hnsw_append_raw(Hnsw *h, const float *vec, u16 level,
                    const u16 *cnts, const u32 *links)
{
    if (!h || !vec || !cnts || !links)
        return E_INVAL;
    if (level > HNSW_MAX_LEVEL)
        return E_CORRUPT;

    int rc = ensure_cap(h, h->n + 1);
    if (rc != OK)
        return rc;

    u32 id = h->n;
    memcpy(h->vecs + (size_t)id * h->dim, vec,
           (size_t)h->dim * sizeof(float));

    h->level[id] = (u8)level;
    memset(cnt_at(h, id, 0), 0, (HNSW_MAX_LEVEL + 1) * sizeof(u16));

    u32 nslots = (u32)h->m0 + (u32)level * h->m;
    rc = pool_reserve(h, nslots);
    if (rc != OK)
        return rc;

    h->link_off[id] = h->pool_used;
    memcpy(h->pool + h->pool_used, links, (size_t)nslots * sizeof(u32));
    h->pool_used += nslots;

    for (u16 lc = 0; lc <= level; lc++) {
        if (cnts[lc] > max_links(h, lc))
            return E_CORRUPT;
        *cnt_at(h, id, lc) = cnts[lc];
    }

    h->n++;
    return OK;
}

int hnsw_set_entry(Hnsw *h, u32 entry, u16 max_level)
{
    if (!h || h->n == 0)
        return E_INVAL;
    if (entry >= h->n || max_level > HNSW_MAX_LEVEL)
        return E_CORRUPT;
    /* 入口点必须真的在最高层，否则搜索的下降路径就断了 */
    if (h->level[entry] != max_level)
        return E_CORRUPT;

    h->entry = entry;
    h->max_level = max_level;
    return OK;
}

/* ---------------- 只读访问 ---------------- */

u32 hnsw_count(const Hnsw *h)      { return h ? h->n : 0; }
u16 hnsw_max_level(const Hnsw *h)  { return h ? h->max_level : 0; }
u32 hnsw_entry(const Hnsw *h)      { return h ? h->entry : 0; }
u16 hnsw_dim(const Hnsw *h)        { return h ? h->dim : 0; }
u16 hnsw_m(const Hnsw *h)          { return h ? h->m : 0; }

u16 hnsw_level_of(const Hnsw *h, u32 id)
{
    if (!h || id >= h->n)
        return 0;
    return (u16)h->level[id];
}

u32 hnsw_neighbors(const Hnsw *h, u32 id, u16 level, u32 *out, u32 cap)
{
    if (!h || !out || id >= h->n || level > h->max_level)
        return 0;

    u16 cnt = *(&h->cnt[(size_t)id * (HNSW_MAX_LEVEL + 1) + level]);
    const u32 *ls = links_at(h, id, level);

    u32 n = (cnt < cap) ? cnt : cap;
    for (u32 i = 0; i < n; i++)
        out[i] = ls[i];
    return n;
}
