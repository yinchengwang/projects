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
#include "modalities/ft/ft_root.h"
#include "modalities/ft/ft_segment.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 死段（被合并掉的段）的登记项。
 *
 * 【为什么死段也要记】
 *
 * 段号**永不复用**：磁盘上留着那个死段的页（内容还在，只是没人引用了），
 * 如果段号被复用，磁盘上就有两个同号的段 —— 恢复时按 (段号, 页号) 排序
 * 取第一个，取到的可能是那个死段 → 文档被算成"活的" → 同一篇文档数两遍。
 *
 * 所以段目录里**一直带着死段**（状态 = DEAD），段号自然就不会被复用。
 */
typedef struct {
    u32 seg_id;      /* 段号 */
    u32 head_page;   /* 段头页号（留着是为了诊断；恢复时死段不用打开） */
} FtDeadSeg;

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
     * 段级**空闲页池**（v1.3 欠账 2）。
     *
     * 合并把旧段合掉之后，旧段的页没人引用了 —— 把它们记在这张表里，
     * 以后建段/合并就优先从这里取页，磁盘文件就不随合并次数**线性增长**了。
     *
     * 它是**内存态**的优化：进程重启后表是空的，那些页就白占着位置
     *（开机回收要等"段目录页"，见 ft_segment_reclaimable_pages）。
     * 宁可浪费空间，也不能因为表丢了就**读错数据**。
     *
     * 【归还的时机】只能在**合并提交之后**（老段已经算死）归还；
     * 合并失败时老段还要回滚 —— 提前归还 = 把还在用的页发出去。
     */
    FtPagePool *pool;

    /*
     * 目录链专用的**空闲页池**（坑 M4-53 收尾）。
     *
     * 【为什么要单独一个池】
     *
     * 段级池（pool）的语义是"这里放的是**段**的页"：
     * 合并失败回滚时，段页不能提前进池（老段还要用）；
     * 测试也按这个口径断言"合并失败 → 空闲页池一个页都不许多"。
     *
     * 目录链的页是**元数据页**，跟段页混在一个池里会带来两个麻烦：
     *   - 目录链换代时把旧链页还进段池 → 段池里混进了"不是段"的页，
     *     合并失败回滚的口径就被污染；
     *   - 合并失败时新写的目录链页从段池拿走 → 段池凭空少一个页，
     *     同样破坏"失败不消费段页"的不变式。
     *
     * 单独一个池之后：目录链页只在目录池里流转（换代时旧链还池、
     * 新链从池里取），段池的语义保持干净，文件也不会随合并次数线性增长。
     */
    FtPagePool *cat_pool;

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

    /*
     * ==================== 段目录的落盘形态（v1.3 欠账 3 / 7） ====================
     *
     * root —— 两张根页（双页交替）+ 当前目录链的位置。
     *
     * 【为什么挂在索引上】
     *
     * 根页的页号是**每个库各自的**（不能放全局变量），
     * 而"当前根是哪一张、目录链头在哪"是索引的运行时状态 ——
     * 放索引上最自然。
     *
     * root.root_a / root.root_b == 0 表示"这个库还没有根页"（老库）：
     * 恢复时走"扫页空间"的回退路径（见 ft_catalog.c）。
     */
    FtRootState root;

    /* 死段（被合并掉的）：段目录里一直带着它们，段号才不会复用 */
    FtDeadSeg *dead;
    u32        n_dead;
    u32        cap_dead;
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
 * 段目录恢复：**先读目录**（有根页时）；目录不可用才回退"扫页空间"。
 */
int ft_catalog_recover(FtIndex *ix);

/*
 * 把索引当前的状态写进段目录（写一整条新链 + 翻根页）。
 *
 * 【谁在什么时候调它】
 *
 *   - 建段之后（ft_index_add 成功）—— 新段要进目录；
 *   - 合并提交之后（ft_index_merge 换完内存里的段）——
 *     老段标死、新段进目录，**这一写就是合并的提交点**；
 *   - 新建 tombstone 页时（ft_tomb_append）—— 页号列表变了；
 *   - 老库首次打开（回退路径扫完页空间之后）—— 把扫到的东西写进目录。
 *
 * 【为什么"删除"不用每次都写目录】
 *
 * tombstone 页里的**条数**存在页里（FTM_COUNT），目录只记**页号**。
 * 页号列表只在一张 tombstone 页被**新建**时才变 ——
 * 页内追加不用碰目录（省掉一次全目录重写）。
 */
int ft_catalog_sync(FtIndex *ix, u32 xid);

/*
 * 建根页（还没有根页时）+ 把当前状态写进目录。
 *
 * 老库首次打开走的就是这条路径：扫完页空间 → 建两张根页 →
 * 把扫到的段与 tombstone 页写进目录 → 之后开库就不用扫了。
 */
int ft_catalog_root_init(FtIndex *ix, u32 xid);

/* 登记一个死段（合并时被合掉的段） */
int ft_catalog_add_dead(FtIndex *ix, u32 seg_id, u32 head_page);

/* 清掉"半拉子"的恢复状态（目录读不通、回退扫页空间之前用） */
void ft_catalog_reset(FtIndex *ix);

/* 合并三态：标 MERGING → 合并成功后标 DEAD → 失败时回滚成 LIVE */
int ft_catalog_mark_merge(FtIndex *ix, u32 xid, u32 new_seg_id,
                           const u32 *head_pages, u32 n);
int ft_catalog_end_merge(FtIndex *ix, u32 xid, const u32 *head_pages, u32 n);
int ft_catalog_abort_merge(FtIndex *ix, u32 xid, const u32 *head_pages, u32 n);


#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_FT_INDEX_INTERNAL_H */
