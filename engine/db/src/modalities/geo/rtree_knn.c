#include "modalities/geo/rtree_knn.h"

#include <stdlib.h>

#include "modalities/geo/rtree.h"

/*
 * R-tree 的精确 KNN（v1.4 步骤 3 · A3）。
 *
 * 【算法】
 *
 *   优先队列（最小堆，按 mindist2 升序）：
 *     弹出 → 若是叶子页：逐条算真实距离，进结果集
 *           → 若是内部页：把每个孩子按 mindist2 压栈
 *     结果集满 k 且 **堆顶 mindist2 > 第 k 近的真实距离** → 提前终止
 *
 * 【终止条件为什么必须是「>」而不是「>=」（坑 M4-44）】
 *
 * 堆里每个元素的 mindist2 是"这棵子树里所有点到查询点的距离下界"。
 * 设当前第 k 近的真实距离是 d_k：
 *
 *   - 若堆顶 mindist2 **> d_k**：这棵子树里**不可能**有距离 ≤ d_k 的点
 *     ⇒ 剪掉它是安全的。
 *   - 若堆顶 mindist2 **== d_k**：这棵子树里**可能有距离恰好等于 d_k 的点**，
 *     而按 tie-break（row_id 升序）它可能排在当前第 k 名**之前** ——
 *     剪掉它就会漏掉这个点，KNN 结果与暴力基准不一致。
 *
 * 也就是说：`>=` 只在"距离不会并列"时才对；
 * 一旦引入 row_id tie-break（空间数据里并列是常态：网格点、对称分布），
 * 终止条件就必须**严格大于**。
 * 计划 §1.2 判决 3 原文写的是 `>=` —— 那是计划的一个缺口，
 * 实现按 `>` 走，并在 `docs/PLAN-v1.4.md` 里打了 ⚠️ 修正标记。
 */

/* ---------------- 页内访问（布局见 rtree.h） ---------------- */

static inline bool rtk_is_leaf(const u8 *pg) { return (pg[RT_FLAGS_OFF] & 1u) != 0; }
static inline u16 rtk_nent(const u8 *pg)     { return get_u16(pg + RT_NENT_OFF); }

/* 解码一个条目：id（叶子 = row_id，内部 = child_page）+ MBR */
static inline void rtk_ent_get(const u8 *p, u32 *id, Mbr *m)
{
    *id = get_u32(p);
    m->minx = rt_u2f(get_u32(p + 4));
    m->miny = rt_u2f(get_u32(p + 8));
    m->maxx = rt_u2f(get_u32(p + 12));
    m->maxy = rt_u2f(get_u32(p + 16));
}

/* ---------------- 优先队列（最小堆） ---------------- */

/* 堆元素：一棵**待展开的子树** */
typedef struct {
    float md2;    /* 到这棵子树 MBR 的平方最近距离（剪枝用的下界） */
    u32   page;   /* 子树根页号 */
} KnNode;

typedef struct {
    KnNode *a;
    u32     n;
    u32     cap;
} KnHeap;

/*
 * 堆序比较：mindist2 小的在堆顶。
 * 并列时按页号升序 —— 只为**可复现**（堆序不影响结果，只影响展开顺序）。
 */
static bool kn_node_less(const KnNode *a, const KnNode *b)
{
    if (a->md2 < b->md2)
        return true;
    if (a->md2 > b->md2)
        return false;
    return a->page < b->page;
}

/* 返回 false = 内存不足（堆扩容失败）。 */
static bool kn_heap_push(KnHeap *h, float md2, u32 page)
{
    KnNode nd;
    u32 i;

    if (h->n == h->cap) {
        u32 ncap = (h->cap == 0) ? 64u : h->cap * 2u;
        KnNode *na = (KnNode *)realloc(h->a, sizeof(KnNode) * (size_t)ncap);

        if (na == NULL)
            return false;
        h->a = na;
        h->cap = ncap;
    }

    nd.md2 = md2;
    nd.page = page;
    i = h->n++;
    while (i > 0) {
        u32 parent = (i - 1u) / 2u;

        if (!kn_node_less(&nd, &h->a[parent]))
            break;
        h->a[i] = h->a[parent];
        i = parent;
    }
    h->a[i] = nd;
    return true;
}

static bool kn_heap_pop(KnHeap *h, KnNode *out)
{
    u32 i = 0;

    if (h->n == 0)
        return false;

    *out = h->a[0];
    h->n--;
    if (h->n == 0)
        return true;

    /* 把最后一个元素挪到根上，再往下沉 */
    h->a[0] = h->a[h->n];
    for (;;) {
        u32 l = 2u * i + 1u;
        u32 r = l + 1u;
        u32 best = i;

        if (l < h->n && kn_node_less(&h->a[l], &h->a[best]))
            best = l;
        if (r < h->n && kn_node_less(&h->a[r], &h->a[best]))
            best = r;
        if (best == i)
            break;
        {
            KnNode t = h->a[i];

            h->a[i] = h->a[best];
            h->a[best] = t;
        }
        i = best;
    }
    return true;
}

/* ---------------- 结果集 ---------------- */

typedef struct {
    float d2;
    u32   row_id;
} KnHit;

/*
 * 把一个候选点按 (d2 升序, row_id 升序) 插进结果集。
 *
 * 结果集最多 k 个：满了之后，只有"比最后一个更好"的才挤掉它。
 * 并列时按 row_id 升序 —— 这就是 tie-break（计划 §1.2 判决 4）。
 */
static void kn_res_insert(KnHit *hits, u32 *n, u32 k, float d2, u32 row_id)
{
    u32 i = *n;
    u32 j;

    /* 找插入位置（保持有序） */
    while (i > 0) {
        const KnHit *p = &hits[i - 1u];

        if (p->d2 < d2 || (p->d2 == d2 && p->row_id <= row_id))
            break;
        i--;
    }

    if (*n == k) {
        if (i >= k)
            return;             /* 比第 k 名还差 → 直接丢 */
        (*n)--;                 /* 挤掉最后一个 */
    }
    for (j = *n; j > i; j--)
        hits[j] = hits[j - 1u];
    hits[i].d2 = d2;
    hits[i].row_id = row_id;
    (*n)++;
}

/* ---------------- 读页 ---------------- */

/*
 * 读一个节点页到本地缓冲。
 *
 * 【为什么复制到本地缓冲，而不是一直捏着帧】
 *
 * 帧池有限（4096 帧），而 KNN 会访问很多页；
 * 一直捏着帧会在帧池吃紧时拿不到新帧。
 * 复制一份 4KB 的代价远小于"帧池耗尽"。
 */
static int rtk_load(Pager *pg, u32 pid, u8 *buf, u64 *reads)
{
    PageFrame *fr = NULL;
    int rc;

    rc = pager_get(pg, pid, &fr);
    if (rc != OK)
        return rc;
    (*reads)++;

    if (fr->data[0] != (u8)PT_RTREE) {
        pager_unpin(pg, pid, false);
        return E_CORRUPT;
    }
    memcpy(buf, fr->data, PAGE_SIZE);
    pager_unpin(pg, pid, false);
    return OK;
}

/* ---------------- 对外 API ---------------- */

u32 rtree_knn_npoints(Pager *pg, u32 root_page)
{
    PageFrame *fr = NULL;
    u32 n = 0;

    if (pg == NULL || root_page == PAGE_ID_NIL)
        return 0;
    if (pager_get(pg, root_page, &fr) != OK)
        return 0;
    if (fr->data[0] == (u8)PT_RTREE)
        n = get_u32(fr->data + RT_NPOINTS_OFF);
    pager_unpin(pg, root_page, false);
    return n;
}

int rtree_knn(Pager *pg, u32 root_page, float qx, float qy, u32 k,
              u32 *out_ids, u32 max_out, u32 *n_out, u64 *page_reads_out)
{
    u8 *buf = NULL;
    KnHit *hits = NULL;
    KnHeap heap;
    u32 nhit = 0;
    u32 npoints;
    u32 nres;
    u64 reads = 0;
    int rc = OK;
    u32 i;

    if (pg == NULL || out_ids == NULL || n_out == NULL)
        return E_INVAL;
    *n_out = 0;
    if (page_reads_out != NULL)
        *page_reads_out = 0;
    if (k == 0 || root_page == PAGE_ID_NIL)
        return OK;

    memset(&heap, 0, sizeof(heap));

    buf = (u8 *)malloc(PAGE_SIZE);
    if (buf == NULL)
        return E_NOSPACE;

    /* 根页读一次：拿点数、判容量、看根是叶子还是内部 */
    rc = rtk_load(pg, root_page, buf, &reads);
    if (rc != OK) {
        free(buf);
        return rc;
    }
    npoints = get_u32(buf + RT_NPOINTS_OFF);

    /* 结果个数 = min(k, 点数)；装不下就报错，不静默截断 */
    nres = (k < npoints) ? k : npoints;
    if (nres > max_out) {
        free(buf);
        return E_NOSPACE;
    }

    hits = (KnHit *)calloc((size_t)(k > 0 ? k : 1u), sizeof(KnHit));
    if (hits == NULL) {
        free(buf);
        return E_NOSPACE;
    }

    /*
     * 根节点单独处理：把它的孩子按 mindist2 压栈（根是叶子则直接算候选点）。
     *
     * 根**不进堆** —— 根一定要展开，没有"根的 mindist"这个概念。
     */
    if (rtk_is_leaf(buf)) {
        u16 n = rtk_nent(buf);

        for (i = 0; i < n; i++) {
            const u8 *p = buf + RT_ENT_OFF + (size_t)i * RT_ENT_SIZE;
            u32 id;
            Mbr m;

            rtk_ent_get(p, &id, &m);
            /* 叶子条目的 MBR 是点的退化矩形 ⇒ 取中心就是那个点 */
            kn_res_insert(hits, &nhit, k,
                          geo_dist2(qx, qy, m.minx, m.miny), id);
        }
    } else {
        u16 n = rtk_nent(buf);

        for (i = 0; i < n; i++) {
            const u8 *p = buf + RT_ENT_OFF + (size_t)i * RT_ENT_SIZE;
            u32 child;
            Mbr m;

            rtk_ent_get(p, &child, &m);
            if (!kn_heap_push(&heap, mbr_mindist2(&m, qx, qy), child)) {
                rc = E_NOSPACE;
                goto done;
            }
        }
    }

    /* 主循环：弹堆顶 → 展开 */
    for (;;) {
        KnNode nd;

        if (!kn_heap_pop(&heap, &nd))
            break;

        /*
         * 提前终止（严格大于 —— 见文件头「坑 M4-44」）。
         *
         * 堆顶 mindist2 是"这棵子树里所有点的距离下界"；
         * 它 > 第 k 近的真实距离 ⇒ 这棵子树里不可能有能进前 k 的点。
         */
        if (nhit == k && nd.md2 > hits[k - 1u].d2)
            break;

        rc = rtk_load(pg, nd.page, buf, &reads);
        if (rc != OK)
            goto done;

        if (rtk_is_leaf(buf)) {
            u16 n = rtk_nent(buf);

            for (i = 0; i < n; i++) {
                const u8 *p = buf + RT_ENT_OFF + (size_t)i * RT_ENT_SIZE;
                u32 id;
                Mbr m;

                rtk_ent_get(p, &id, &m);
                kn_res_insert(hits, &nhit, k,
                              geo_dist2(qx, qy, m.minx, m.miny), id);
            }
        } else {
            u16 n = rtk_nent(buf);

            for (i = 0; i < n; i++) {
                const u8 *p = buf + RT_ENT_OFF + (size_t)i * RT_ENT_SIZE;
                u32 child;
                Mbr m;

                rtk_ent_get(p, &child, &m);
                if (!kn_heap_push(&heap, mbr_mindist2(&m, qx, qy), child)) {
                    rc = E_NOSPACE;
                    goto done;
                }
            }
        }
    }

    for (i = 0; i < nhit; i++)
        out_ids[i] = hits[i].row_id;
    *n_out = nhit;

done:
    if (page_reads_out != NULL)
        *page_reads_out = reads;
    free(hits);
    free(buf);
    free(heap.a);
    return rc;
}
