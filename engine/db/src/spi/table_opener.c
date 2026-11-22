#include "spi/table_opener.h"

#include <string.h>

#include "spi/registry.h"

/*
 * 虚拟表打开器：从 SPI 注册表里按名字取出已注册的 StorageAM。
 *
 * 【为什么不直接在执行器里调 spi_registry_find】
 * 那样执行器就得知道"还有注册表这个东西"，等于换一个模态名继续认识。
 * 包装成 TableOpener 之后，执行器只认"按顺序问几个 opener"这一种形状 ——
 * 加第 9 个模态只是多挂一个 opener（或者往注册表里多注册一张表），
 * 执行器一行不改。
 */
static int virt_open(const TableOpener *op, void *owner, const char *name,
                     TableOpenResult *out,
                     ColDesc *cols_buf, IndexInfo *idxs_buf,
                     void *priv, size_t priv_bytes)
{
    (void)op;
    (void)idxs_buf;
    (void)priv;
    (void)priv_bytes;

    if (!out || !cols_buf)
        return E_INVAL;

    SpiRegistry *reg = (SpiRegistry *)owner;
    StorageAM *am = spi_registry_find(reg, name);
    if (!am)
        return E_NOTFOUND;

    /*
     * 模态给的列定义是**模态自己的**数组（静态常量或实例级稳定），
     * 本身不会悬空 —— 但还是拷一份到调用方的缓冲，
     * 这样"tc->cols 永远指向自己的缓冲区"这条规则就没有例外。
     */
    const ColDesc *vcols = NULL;
    int vn = 0;
    int rc = am->columns(am, &vcols, &vn);
    if (rc != OK)
        return rc;
    if (vn > SQL_MAX_COLS)
        return E_NOSPACE;
    if (vn > 0) {
        if (!vcols)
            return E_CORRUPT;
        memcpy(cols_buf, vcols, (size_t)vn * sizeof(ColDesc));
    }

    out->am         = am;
    out->cols       = cols_buf;
    out->ncols      = vn;
    out->idxs       = NULL;
    out->nidx       = 0;
    out->tid        = 0;
    out->is_virtual = true;
    return OK;
}

static int virt_open_by_id(const TableOpener *op, void *owner, u32 tid,
                           TableOpenResult *out,
                           ColDesc *cols_buf, IndexInfo *idxs_buf,
                           void *priv, size_t priv_bytes)
{
    (void)op;
    (void)owner;
    (void)tid;
    (void)out;
    (void)cols_buf;
    (void)idxs_buf;
    (void)priv;
    (void)priv_bytes;

    /* 虚拟表没有 id —— 按 id 打开只对 catalog 里的用户表有意义。 */
    return E_NOTSUP;
}

const TableOpener spi_table_opener = {
    "virtual",
    virt_open,
    virt_open_by_id,
    0
};
