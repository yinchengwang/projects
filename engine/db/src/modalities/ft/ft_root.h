#ifndef DB_MODALITIES_FT_ROOT_H
#define DB_MODALITIES_FT_ROOT_H

/*
 * ft_root.h —— 全文检索的**根页 + 段目录页**（v1.3 欠账 3 / 7）
 *
 * 【这一层解决什么】
 *
 * 现在的段目录是"散落在段头页里的状态"（坑 M4-20）：
 * 开库时必须**扫整个页空间**才能把段找齐（坑 M4-22），
 * 开库时间随库大小线性增长。
 *
 * 本层引入两个东西：
 *
 *   1. **根页**（两张，固定交替）—— 只记"当前目录链的头 + 版本号"；
 *   2. **段目录页**（可跨多页）—— 记"有哪些段、状态是什么、
 *      tombstone 页在哪"。
 *
 * 开库时只要读**两张根页**就能定位目录，不用扫页空间。
 *
 * 【为什么要两张根页，而不是一张】
 *
 * 一张根页不够：翻根页是**原地改写**，断电可能停在"写了一半"，
 * 于是这一张根页既不是新版也不是旧版 —— 目录就没了。
 *
 * 两张根页交替写（写当前根的**另一张**），性质变成：
 *
 *   任何时刻，磁盘上**至少有一份完整可读的目录**。
 *
 * 因为新一代的根页只有在**目录链已经写完**之后才被写，
 * 而它被写坏时，上一代那张根页还完完整整地躺在那里。
 *
 * 读的时候取 generation 大的那份；大的那份坏了（页类型不对 /
 * generation 不可信 / 校验不过 / 它指的目录链读不通），
 * 就**回退到旧的那份**，而且**不报错**（旧的那份仍然是一份完整的目录）。
 *
 * 【本层不依赖 struct FtIndex】
 *
 * 目录机制本身跟索引对象无关，所以这一层自带一个独立的状态结构
 * （FtRootState），由**调用方**持有。
 * 接线（wave 2）时由 ft_index / ft_catalog 把 FtRootState 挂进 FtIndex，
 * 这一层完全不需要知道 FtIndex 长什么样。
 *
 * 【页类型】
 *
 * 根页与目录页的页类型**先在这里本地定义**（FT_ROOT_PTYPE / FT_CAT_PTYPE），
 * 没有动 src/common/config.h（那是全局页类型表，改它要全局对齐）。
 * 接线时如果决定把这两个页类型登记进 config.h，
 * 只要**数值不变**，这一层的磁盘格式就完全兼容（见任务汇报）。
 */

#include "common/loghook.h"
#include "common/types.h"
#include "storage/pager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 页类型与版本 ---------------- */

/*
 * 根页与目录页的页类型。
 *
 * 取值接在 PT_FT_TOMB(0x12) 之后（0x13 / 0x14），
 * 与 common/config.h 里的页类型表不冲突（那边现在到 0x12 为止）。
 */
#define FT_ROOT_PTYPE  0x13u   /* 根页（双页交替） */
#define FT_CAT_PTYPE   0x14u   /* 段目录页 */

/* 格式版本：根页与目录页各自一个 */
#define FT_ROOT_VER    1u
#define FT_CAT_VER     1u

/* ---------------- 磁盘格式（这一层的格式契约） ---------------- */

/*
 * 【页头的前 20 字节是 slotted page 的标准头，不要乱占】
 *
 * 0 H_TYPE / 1 H_FLAGS / 2-3 H_SLOTS / 4-5 H_FREE_OFF / 6-7 H_FREE_BYTES /
 * 8-11 H_NEXT / 12-15 H_LSN / 16-17 H_CKSUM / 18-19 H_TID
 *
 * 根页与目录页都是**定长自定义页**（不走 slot 目录），
 * 所以自己的字段一律从偏移 20 开始。
 */

/*
 * 根页的自定义区：
 *
 * 20 FTR_VER      u8   格式版本
 * 21 FTR_SLOT     u8   这一张是 A(0) 还是 B(1)
 * 24 FTR_GEN      u32  generation（单调递增的版本号）
 * 28 FTR_PREV_GEN u32  上一代的 generation
 * 32 FTR_CAT_HEAD u32  当前目录链的头页
 * 36 FTR_CAT_PAGES u32 当前目录链有多少页
 * 40 FTR_NSEG     u32  段条目总数（整个链加起来）
 * 44 FTR_NTOMB    u32  tombstone 页条目总数（整个链加起来）
 * 48 FTR_PREV_CAT u32  被本代取代的旧目录链头（将来回收旧链时用）
 * 52 FTR_LSN      u32  本代目录最后一次写页的 LSN（诊断用）
 */
enum {
    FTR_VER      = 20,
    FTR_SLOT     = 21,
    FTR_GEN      = 24,
    FTR_PREV_GEN = 28,
    FTR_CAT_HEAD = 32,
    FTR_CAT_PAGES = 36,
    FTR_NSEG     = 40,
    FTR_NTOMB    = 44,
    FTR_PREV_CAT = 48,
    FTR_LSN      = 52
};

/*
 * 目录页的自定义区：
 *
 * 20 FTC_VER   u8   格式版本
 * 21 FTC_FLAGS u8   bit0 = 还有下一页
 * 24 FTC_PREV  u32  被本链取代的旧目录链头（PAGE_ID_NIL = 没有）
 * 28 FTC_NSEG  u32  本页的段条目数
 * 32 FTC_NTOMB u32  本页的 tombstone 页条目数
 * 36 FTC_DATA  ...  条目区：先段条目，后 tombstone 条目
 */
enum {
    FTC_VER   = 20,
    FTC_FLAGS = 21,
    FTC_PREV  = 24,
    FTC_NSEG  = 28,
    FTC_NTOMB = 32,
    FTC_DATA  = 36
};

/* ---------------- 目录里的两类条目 ---------------- */

/*
 * 段条目：目录页里记的"一个段"的元数据。
 *
 * state 用 FT_SEG_LIVE / FT_SEG_DEAD / FT_SEG_MERGING
 * （定义在 ft_internal.h；这一层不 include 它 —— 语义由调用方解释，
 * 这一层只负责**原样存取**，不解释状态）。
 */
typedef struct {
    u32 seg_id;      /* 段号 */
    u32 head_page;   /* 段头页号（段的唯一标识） */
    u32 state;       /* 段的生命周期状态 */
} FtRootSeg;

/*
 * tombstone 页条目：目录里**给 tombstone 页链预留的位置**。
 *
 * 【为什么目录里要留 tombstone 页的位置】
 *
 * 现在 tombstone 页号只活在内存里（FtIndex.tomb_pages），
 * 重开库靠扫页空间把它们找回来（又是坑 M4-22 那一套）。
 *
 * 目录里给它们留位置，是为了以后"开库只读目录"时，
 * tombstone 页也能被直接定位 —— 这一轮**只留位置，不接线**
 * （ft_tomb.c / ft_catalog.c 本轮不动）。
 *
 * ordinal —— 该 tombstone 页在页序列里的序号（0 起），
 * 用来保住"哪一页先写、哪一页后写"的次序（追加时要知道当前页是谁）。
 */
typedef struct {
    u32 page_id;     /* tombstone 页号 */
    u32 ordinal;     /* 在 tombstone 页序列里的序号 */
} FtRootTomb;

/* ---------------- 根状态（调用方持有） ---------------- */

/*
 * 根状态：两张根页的页号 + "当前是那张"。
 *
 * 【为什么这个结构归调用方而不是全局变量】
 *
 * 一个进程里可能有多个库（多个 FtIndex），根页的页号是**每个库各自的**。
 * 放进全局变量就变成"上一个库的页号被下一个库覆盖"。
 *
 * 而且这一层不能依赖 struct FtIndex（那是索引对象的内部布局），
 * 所以自带一个独立结构，由调用方持有并传进来传出去。
 */
typedef struct {
    u32 root_a;       /* 根页 A 的页号 */
    u32 root_b;       /* 根页 B 的页号 */
    u32 cur_slot;     /* 当前根页在哪一张：0 = A，1 = B */
    u32 generation;   /* 当前根页的 generation（版本号） */
    u32 cat_head;     /* 当前根页指向的目录链头 */
    u32 cat_pages;    /* 当前目录链有多少页 */
    bool loaded;      /* 是否成功定位到过当前根页 */
} FtRootState;

/* ---------------- 读出来的目录内容 ---------------- */

/*
 * 读出来的目录内容。
 *
 * 用完必须调 ft_root_catalog_free() 释放（数组是 malloc 出来的）。
 */
typedef struct {
    u32 generation;    /* 读到的这份目录的 generation */
    u32 root_slot;     /* 它来自哪张根页：0 = A，1 = B */
    u32 n_cat_pages;   /* 目录链实际读了几页 */
    u32 n_segs;        /* 段条目数 */
    u32 n_tombs;       /* tombstone 页条目数 */
    FtRootSeg  *segs;  /* 段条目（按目录里的次序） */
    FtRootTomb *tombs; /* tombstone 页条目 */
} FtRootCatalog;

/* ---------------- 目录链的页从哪儿来 / 旧链的页还到哪儿去 ---------------- */

/*
 * 目录链的页是"外部供给"的：接线方（ft_catalog.c）给两个回调。
 *
 * 【为什么用回调，而不是把空闲页池直接传进来】
 *
 * 这一层是**独立内核件**：它只认"给我一页"与"把一页还回去"两件事，
 * 不认"空闲页池"这种 FT 段层的概念（那是 modalities/ft 里的东西）。
 * 接线方把空闲页池包成这两个回调传进来，两层就解耦了。
 *
 * 【为什么目录链的页要"外部供给"】
 *
 * 目录链每写一次就要**一整条新链**（旧的留着当回退）——
 * 不回收的话，文件随目录更新次数**线性增长**（每轮合并 +1 页）。
 * 让链的页也从空闲页池里取（旧链的页还回池里），文件就不长了。
 *
 * alloc —— 取一页（pin 住并把帧交出来；帧的写与放开由 ft_root_write 负责）。
 *          失败返回非 OK（调用方整条链都不写）。
 * release —— 回收一页（交回空闲池）。在**根页翻成功之后**才调。
 * ctx —— 回调自己的上下文（接线方传索引对象）。
 */
typedef struct {
    int  (*alloc)(void *ctx, u32 *pid_out, PageFrame **fr_out);
    void (*release)(void *ctx, u32 pid);
    void *ctx;
} FtRootPageOps;

/* ---------------- API ---------------- */

/*
 * 新建一库的根页（建库时调一次）。
 *
 * 分配两张固定的根页 A / B，并写好初始状态：
 *   A = generation 1 的空目录（当前根）
 *   B = generation 0 的空目录（备用槽）
 *
 * 之后每一次目录更新都写到"当前根的**另一张**"上，
 * 于是两张根页轮着当"当前根"。
 *
 * 返回 OK / E_NOSPACE / E_CORRUPT（其它错误码来自 pager / WAL）。
 */
int ft_root_create(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                   FtRootState *st);

/*
 * 绑定一对已有的根页（重开库时调）。
 *
 * 页号从哪来是**接线方**的事（wave 2 建议放进库的元数据页）：
 * 这一层只负责"给我这两张页，我去找当前根"。
 *
 * 绑定后要调 ft_root_load() 才知道当前根是哪一张。
 */
int ft_root_bind(FtRootState *st, u32 root_a, u32 root_b);

/*
 * 读两张根页，定位当前根（generation 大的那张）。
 *
 * - 两张都坏 / 都不存在 → E_NOTFOUND（这一层不负责建目录）；
 * - 只有一张好         → 用那一张，**不报错**（这就是回退）；
 * - 两张都好           → 取 generation 大的；
 * - 两张 generation 一样 → 取 A（约定，见坑 M4-34）。
 *
 * 成功后 st->cur_slot / generation / cat_head / cat_pages / loaded 被填好。
 */
int ft_root_load(Pager *pg, FtRootState *st);

/*
 * 读当前根指向的目录内容。
 *
 * 内部按"generation 从大到小"依次试读：
 * 某一份的目录链读不通（页类型不对 / 校验不过 / 条目数对不上），
 * 就退到下一份。全部读不通才返回 E_CORRUPT。
 *
 * out 由本函数分配内存，用完调 ft_root_catalog_free()。
 */
int ft_root_read(Pager *pg, const FtRootState *st, FtRootCatalog *out);

/*
 * 写一版新目录（目录更新）。
 *
 * 步骤（顺序是硬要求，坑 M4-33）：
 *   1. 先把新的目录链**整页写完**（每页都是 WAL 先行）；
 *   2. 再把根页写到"当前根的另一张"上，generation + 1。
 *
 * 于是断电只可能停在两种状态：
 *   - 停在 1 和 2 之间 → 根页还指着旧链 → 旧目录照样读得到；
 *   - 停在 2 的中间   → 新根页坏了 → 回退到旧根页 → 旧目录照样读得到。
 *
 * 【旧目录链不回收】
 *
 * 旧链的页**留在文件里不回收**：根页只认识最新的一代，
 * 回收要等"确认没有读者还在读旧链"，那是以后的事。
 * 本轮明确不实现（注释里写明，见 ft_root.c）。
 *
 * 返回 OK / E_NOSPACE / E_CORRUPT / E_INVAL。
 */
int ft_root_write(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                  FtRootState *st,
                  const FtRootSeg *segs, u32 n_segs,
                  const FtRootTomb *tombs, u32 n_tombs);

/*
 * 同上，但目录链的页**从外部供给**（FtRootPageOps），并且
 * **旧目录链会被回收**（坑 M4-53）。
 *
 * 【回收哪一条链】
 *
 * 根页交替写（两张），所以任何时刻磁盘上是：
 *
 *   槽 A：第 N 代（cat_head = 第 N 代那条链）
 *   槽 B：第 N-1 代（cat_head = 第 N-1 代那条链，prev_cat = 第 N-2 代那条链）
 *
 * 写第 N+1 代时翻的是槽 B → 翻完之后：
 *
 *   - 第 N 代那条链：**还活着**（槽 A 的回退路径要读它）→ 不能回收；
 *   - 第 N-1 代那条链：没人指了（槽 B 被覆盖了）→ **可以回收**；
 *   - 第 N-2 代那条链：没人指了（它只出现在槽 B 的 prev_cat 里）→ **可以回收**。
 *
 * 所以：翻根页**之前**把槽 B 原来的 cat_head 与 prev_cat 记下来，
 * 翻**成功**之后把这两条链的页还给空闲池。
 *
 * ⚠️ 顺序是硬要求：翻页失败 → 不回收（回收了就等于把还在用的链的页发出去）。
 *
 * ops == NULL → 与 ft_root_write 完全一样（链的页直接向文件尾追加，
 * 旧链不回收）—— 老口径，测试与老调用方都走这一条。
 */
int ft_root_write_ex(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                     FtRootState *st,
                     const FtRootSeg *segs, u32 n_segs,
                     const FtRootTomb *tombs, u32 n_tombs,
                     const FtRootPageOps *ops);

/* 释放 ft_root_read 分配出来的数组 */
void ft_root_catalog_free(FtRootCatalog *c);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_FT_ROOT_H */
