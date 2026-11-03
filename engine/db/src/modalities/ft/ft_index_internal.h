#ifndef DB_MODALITIES_FT_INDEX_INTERNAL_H
#define DB_MODALITIES_FT_INDEX_INTERNAL_H

/*
 * ft_index_internal.h —— 索引对象的**内部布局**（不是公共契约）
 *
 * 【为什么把结构体从 ft_index.c 挪出来】
 *
 * 索引对象本来是 ft_index.c 里的私有结构体。步骤 4（SPI 接线）要新增
 * 一个"只读访问器"（ft_index_seg_at / ft_index_is_tomb），
 * 供 ft_docs 虚拟表逐段扫描用 —— 而访问器要读 ix->segs / ix->tomb，
 * 就必须能看到结构体。
 *
 * 两条路：
 *   (a) 把访问器写进 ft_index.c —— 但那个文件已经贴着 800 行上限，
 *       再加就把硬规则顶破了；
 *   (b) 把结构体挪到这个内部头文件里，访问器单独放一个 .c ——
 *       ft_index.c 反而**变短**，两个文件都不超限。
 *
 * 选 (b)。这是纯粹的"挪位置"，不改变任何语义：
 * 索引的字段、字段的含义、字段的用法一个字都没动。
 *
 * 【为什么它不是公共契约】
 *
 * 公共契约是 ft_index.h（对外的函数 + 语义说明）。
 * 这个头文件里只有**布局**，任何人都能改它 ——
 * 所以外部文件只允许"通过 ft_index.h 声明的访问器"去读它，
 * 不许直接碰字段。
 */

#include "common/loghook.h"
#include "common/types.h"
#include "modalities/ft/ft_segment.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 索引对象。
 *
 * 【为什么段列表在内存里】
 *
 * 段的**内容**在磁盘上，段的**清单**（段头页号）本版只在内存里 ——
 * 清单要能落盘才能"重开库还能找到段"，那是步骤 4/5（段目录页）的事。
 */
struct FtIndex {
    Pager   *pg;
    LogHookFn wal;
    void    *wal_ctx;
    FtSegment **segs;      /* 段数组，按段号递增 */
    u32      n_segs;
    u32      cap_segs;
    u32     *tomb;         /* 被删的 doc_id，按升序 */
    u32      n_tombs;
    u32      cap_tombs;
    u32      next_seg_id;  /* 下一个新段的段号 */
    u64      page_reads_total;  /* 索引级**累计**页读次数（诊断用，坑 M4-16） */
};

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_FT_INDEX_INTERNAL_H */
