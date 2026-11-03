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
#include "modalities/ft/ft_index.h"
#include "modalities/ft/ft_segment.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 索引对象。
 *
 * 【为什么段列表在内存里】
 *
 * 段的**内容**在磁盘上；段的**清单**（哪些段还在）也落盘了 ——
 * 落盘的方式是"段头页里的状态字段"（坑 M4-20），
 * 恢复时扫页空间把段找齐（见 ft_catalog.c）。
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

    /*
     * 索引级**累计**的"合并路径页读"（与 page_reads_total 分开）。
     *
     * 【为什么要有两个计数器】
     *
     *   - page_reads_total   —— **检索**页读：A5 测的是"查一个词读了几页"，
     *                           口径是"各段计数的差量"，与语料量无关；
     *   - merge_page_reads   —— **合并**页读：A4 测的是"合并一次要读几页"，
     *                           口径是"这次合并读过的页数"。
     *
     * 两条是不同的 IO 曲线，混在一起就看不出量级了
     * （合并读的页比检索多一个数量级）。
     *
     * 为什么要单独记：合并时被合并掉的段随后就被 ft_segment_close() 释放了，
     * 它们的 page_reads **事后读不到** —— 不在合并返回前把增量收上来，
     * 这次的合并代价就永远丢了。
     */
    u64      merge_page_reads;

    /*
     * tombstone 集合的**落盘形态**（v1.3 步骤 5，坑 B）。
     *
     * tomb_pages —— 存着这个集合的页号（一张页存不下就再开一张）。
     * tomb_fill  —— 最后一张页里已经用了多少条（追加时接着往后写）。
     *
     * 为什么页号要留在内存里：删除是**追加**操作，
     * 每次都要找到"当前那张还没写满的页"，
     * 每次都去扫页空间找一遍太慢。
     */
    u32     *tomb_pages;
    u32      n_tomb_pages;
    u32      cap_tomb_pages;
    u32      tomb_fill;
};

/* ---------------- 内部接口（ft_tomb.c / ft_catalog.c 实现） ---------------- */

/* 内存里的 tombstone 集合（升序数组）的操作 */
u32  ft_tomb_lb(const u32 *tomb, u32 n, u32 doc_id);
bool ft_tomb_is(const FtIndex *ix, u32 doc_id);
int  ft_tomb_insert(FtIndex *ix, u32 doc_id);   /* 1=插入 0=已存在 -1=内存不足 */

/*
 * 追加一个 doc_id 到落盘的 tombstone 集合（读-改-写当前页，满了就开新页）。
 */
int ft_tomb_append(FtIndex *ix, u32 xid, u32 doc_id);

/*
 * 恢复时的集合装配：
 *
 *   最终集合 = 页里的内容 ∪ (WAL 重放看到的 − WAL 回滚撤销的)
 *
 * 页里的内容是**权威**的（它不会被 checkpoint 截断），
 * WAL 那一路只是补上"页还没落盘、但日志里已经记了"的删除。
 */
int ft_tomb_assemble(FtIndex *ix, const u32 *redo_ids, u32 n_redo,
                      const u32 *undo_ids, u32 n_undo);

/* 把扫描到的 tombstone 页登记进索引（ft_catalog.c 调） */
int ft_tomb_add_page(FtIndex *ix, u32 page_id, u32 count);

/* 释放落盘形态的辅助数组 */
void ft_tomb_free(FtIndex *ix);

/* 把一个段追加进段数组（ft_index.c 实现；恢复时也要用） */
int ft_index_push_seg(FtIndex *ix, FtSegment *seg);

/*
 * 段目录恢复：扫页空间，把还活着的段找出来并打开（按段号递增）。
 */
int ft_catalog_recover(FtIndex *ix);

/* 合并三态：标 MERGING → 合并成功后标 DEAD → 失败时回滚成 LIVE */
int ft_catalog_mark_merge(FtIndex *ix, u32 xid, u32 new_seg_id,
                           const u32 *head_pages, u32 n);
int ft_catalog_end_merge(FtIndex *ix, u32 xid, const u32 *head_pages, u32 n);
int ft_catalog_abort_merge(FtIndex *ix, u32 xid, const u32 *head_pages, u32 n);


#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_FT_INDEX_INTERNAL_H */
