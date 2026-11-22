#include "modalities/relational/rel_opener.h"

#include <stddef.h>

#include "modalities/relational/heap_am.h"
#include "storage/heap.h"

/*
 * 关系模态的表打开器。
 *
 * 【为什么它住在这个模态目录里】
 * 它做的事是"catalog → 堆句柄 → StorageAM"，三样都是关系模态的内部知识。
 * 放在 spi/ 下就是让契约目录替模态做决定（S9② 明确禁止）。
 * 执行器只知道有个 opener 能被问，不知道它背后是堆。
 *
 * ⚠️ 文件位置与 include 路径是**两件事**：本文件住在
 * `modalities/relational/`，但 C1 阶段 `heap_am.h` 还在 `spi/` 下
 * （C2 才 git mv 过来）。搬家时这里的 include 要一起改。
 */

/*
 * 关系模态的私有状态：堆句柄 + AM 上下文 + AM 本体。
 *
 * 【为什么 AM 本体也在私有状态里】
 * heap 的 AM 是**每条查询现构造**的：它要绑定到具体的堆句柄 + 列定义 +
 * 索引列表，而表是运行期动态创建的。所以 AM 与它的上下文同生共死，
 * 一起放进调用方给的缓冲里。
 */
typedef struct {
    Heap       *heap;
    HeapAmCtx   am_ctx;
    StorageAM   am_storage;
} RelPriv;

/*
 * 私有状态的大小契约 —— 越界就是**编译期**错误，不是运行期崩溃。
 *
 * 实测 sizeof(RelPriv) = Heap*(8) + HeapAmCtx(184) + StorageAM(152) = 344。
 */
_Static_assert(sizeof(RelPriv) <= SPI_OPENER_PRIV_MAX,
               "关系模态的私有状态超出 SPI_OPENER_PRIV_MAX");

/* 把 AM 装进 TableOpenResult。 */
static int rel_fill(const RelPriv *rp, const ColDesc *cols, int ncols,
                    const IndexInfo *idxs, int nidx, u32 tid,
                    TableOpenResult *out)
{
    out->am         = (StorageAM *)&rp->am_storage;
    out->cols       = cols;
    out->ncols      = ncols;
    out->idxs       = idxs;
    out->nidx       = nidx;
    out->tid        = tid;
    out->is_virtual = false;
    return OK;
}

static int rel_open_by_id(const TableOpener *op, void *owner, u32 tid,
                          TableOpenResult *out,
                          ColDesc *cols_buf, IndexInfo *idxs_buf,
                          void *priv, size_t priv_bytes)
{
    (void)op;

    if (!owner || !out || !cols_buf || !idxs_buf || !priv)
        return E_INVAL;
    if (priv_bytes < sizeof(RelPriv))
        return E_INVAL;

    Catalog *cat = (Catalog *)owner;
    RelPriv *rp = (RelPriv *)priv;

    Heap *heap = catalog_heap(cat, tid);
    if (!heap)
        return E_CORRUPT;

    /*
     * 列定义：catalog 写进**调用方给的数组**，不返回指针。
     * （catalog.h 的两段式用法：先问要多少，再拷。）
     */
    int ncols = 0;
    int rc = catalog_table_cols(cat, tid, cols_buf, SQL_MAX_COLS, &ncols);
    if (rc != OK)
        return rc;

    /*
     * 索引：同样拷进调用方给的数组。
     *
     * 少了这一步，heap_am_build 拿到的 nidx = 0，建出来的 AM **不认任何索引**
     * ⇒ 索引扫描静默失效（查出来的结果看着永远对，但实际走的全是全扫，
     * EXPLAIN 的理由还会误导人）。
     */
    int nidx = 0;
    rc = catalog_table_indexes(cat, tid, idxs_buf, SQL_MAX_COLS, &nidx);
    if (rc != OK)
        return rc;

    /*
     * 构造关系模态的 StorageAM。
     * AM 本体与它的上下文都住在**调用方给的 priv 缓冲**里。
     */
    rc = heap_am_build(heap, cols_buf, ncols, idxs_buf, nidx,
                       &rp->am_storage, &rp->am_ctx);
    if (rc != OK)
        return rc;

    rp->heap = heap;
    return rel_fill(rp, cols_buf, ncols, idxs_buf, nidx, tid, out);
}

/*
 * 按名打开 —— 契约里的主入口（SQL 里的表名解析走的就是它）。
 *
 * 查不到表 ⇒ 返回 E_NOTFOUND，这是契约约定的"我没这个表"，
 * 执行器据此去问下一个 opener。其它错误码原样透传。
 */
static int rel_open(const TableOpener *op, void *owner, const char *name,
                    TableOpenResult *out,
                    ColDesc *cols_buf, IndexInfo *idxs_buf,
                    void *priv, size_t priv_bytes)
{
    if (!owner || !name)
        return E_INVAL;

    Catalog *cat = (Catalog *)owner;
    u32 tid = 0;
    if (catalog_find_table(cat, name, &tid) != OK)
        return E_NOTFOUND;   /* 我没这张表 —— 让执行器去问下一个 opener */

    /* 找到之后与按 id 打开完全同一条路 */
    return rel_open_by_id(op, owner, tid, out, cols_buf, idxs_buf,
                          priv, priv_bytes);
}

/*
 * ⚠️ `priv_bytes` 必须填 `sizeof(RelPriv)`。
 * 填 0 的话执行器按 0 字节给缓冲，`priv_bytes < sizeof(RelPriv)` 恒真
 * ⇒ **每一次 open 都返回 E_INVAL** ⇒ 表永远打不开。
 */
const TableOpener rel_table_opener = {
    "catalog",
    rel_open,
    rel_open_by_id,
    sizeof(RelPriv)
};
