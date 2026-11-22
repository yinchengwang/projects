#ifndef DB_SPI_TABLE_OPENER_H
#define DB_SPI_TABLE_OPENER_H

#include <stddef.h>

#include "catalog/catalog.h"
#include "common/error.h"
#include "common/types.h"
#include "spi/storage_am.h"

/*
 * 表打开器 —— SPI 的第 4 个契约。
 *
 * 【它补的是哪个洞】
 * 硬规则 7「执行器不许知道模态」。执行器解析表名时
 * （`query/exec_scan.c::resolve_table`）原本自己分两支：
 *
 *   catalog 命中  → catalog_heap + heap_am_build   ← **执行器认得 heap**
 *   注册表命中    → spi_registry_find + am->columns ← 执行器只认 StorageAM ✓
 *
 * 第二支合规，第一支破窗：执行器一旦要支持第二种"存在 catalog 里但
 * 不是堆表"的模态，就得再加一支 if。
 *
 * 【本契约怎么补】
 * 把"怎么把一张表变成一个 StorageAM"抽成接口，两个实现：
 *
 *   - 关系模态的 opener  （`modalities/relational/rel_opener.c`）
 *   - 注册表的 opener    （`spi/table_opener.c`）
 *
 * 执行器只按顺序问这两个，谁先答上就用谁 —— **执行器不再提 heap**。
 *
 * 【为什么不能把 heap_am_build 塞进 StorageAM】
 * StorageAM 回答的是"拿到一行怎么办"（读 / 写 / 可见性 / 下推）；
 * opener 回答的是"这张表从哪儿来"（找到它、给它一个 AM）。
 * 两个问题混在一个结构体里，虚拟表就得填一堆用不上的字段。
 */

/*
 * 打开器的私有状态缓冲上限。
 *
 * 打开器可能需要为这张表准备一些 per-query 的私有状态
 * （关系模态要放 Heap + HeapAmCtx + StorageAM）。
 *
 * 【为什么是"调用方给缓冲"而不是"打开器返回指针"】
 * TableCtx 已经在栈上了（每条查询一个）。让打开器往调用方的缓冲里写，
 * 就少一次堆分配，也就不用管它的生命周期。
 *
 * 【为什么不把这些字段直接塞进 TableOpenResult】
 * 各模态的私有状态大小不同（关系模态 344 字节，虚拟表为 0）。
 * 塞进公共结构体就是"给最胖的那个留位置，全家都付这份内存"。
 *
 * 【512 这个值怎么来的】
 * 实测关系模态的私有状态：`Heap*` 8 + `HeapAmCtx` 184 + `StorageAM` 152
 * = **344 字节**。取 512 给它留一档余量，同时保证下一个模态也有地方放。
 * 关系模态侧有 `_Static_assert` 把它钉死 —— 超了是**编译期**错误，
 * 不是运行期崩溃。
 */
#define SPI_OPENER_PRIV_MAX 512

/* 打开器的产物。调用方持有，必须活到本次查询结束。 */
typedef struct {
    StorageAM       *am;        /* 这张表的 StorageAM */
    const ColDesc   *cols;      /* 列定义（指向调用方给的 cols_buf） */
    int              ncols;
    const IndexInfo *idxs;      /* 索引列表（指向调用方给的 idxs_buf；可为 NULL） */
    int              nidx;
    u32              tid;       /* catalog 表 id；虚拟表为 0 */
    bool             is_virtual;
} TableOpenResult;

typedef struct TableOpener TableOpener;

/*
 * 第四个 SPI 契约：表打开器。
 *
 * ⚠️ `open` / `open_by_id` 都带两个"调用方给的缓冲"参数 ——
 * 因为 `catalog_table_cols` / `catalog_table_indexes` 都是**写进调用方给的
 * 数组、不返回指针**（v1.2 步骤 1 的 copy-out 纪律）。
 * 所以 opener 光返回指针不够：列定义得由**调用方**备好缓冲，
 * `out->cols` / `out->idxs` 指回它 —— 这样"自持副本"纪律才继续成立。
 */
struct TableOpener {
    const char *kind;          /* "catalog" / "virtual" —— 仅诊断用 */

    /*
     * 按表名打开。表不存在返回 E_NOTFOUND（执行器据此去问下一个 opener）；
     * 其它错误码一律原样透传（执行器不会再问下一个）。
     *
     * owner      : 打开器的宿主（Catalog / SpiRegistry / ...）
     * name       : 表名
     * out        : 产物（调用方持有）
     * cols_buf   : 调用方给的列定义缓冲（至少 SQL_MAX_COLS 项 ColDesc）
     * idxs_buf   : 调用方给的索引列表缓冲（至少 SQL_MAX_COLS 项 IndexInfo）
     * priv       : 调用方给的私有状态缓冲（至少 priv_bytes 字节）
     * priv_bytes : priv 的字节数
     *
     * out->cols / out->idxs 会指向 cols_buf / idxs_buf（不是打开器自己的内存）。
     */
    int (*open)(const TableOpener *op, void *owner, const char *name,
                TableOpenResult *out,
                ColDesc *cols_buf, IndexInfo *idxs_buf,
                void *priv, size_t priv_bytes);

    /* 按表 id 打开（只有 catalog opener 支持；虚拟表返回 E_NOTSUP）。参数同上。 */
    int (*open_by_id)(const TableOpener *op, void *owner, u32 tid,
                      TableOpenResult *out,
                      ColDesc *cols_buf, IndexInfo *idxs_buf,
                      void *priv, size_t priv_bytes);

    /* 私有状态需要多少字节。 */
    size_t priv_bytes;
};

/* 注册表侧的打开器（虚拟表）。定义在 `spi/table_opener.c`。 */
extern const TableOpener spi_table_opener;

#endif /* DB_SPI_TABLE_OPENER_H */
