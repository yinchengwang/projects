#ifndef DB_MODALITIES_TS_TS_DIR_H
#define DB_MODALITIES_TS_TS_DIR_H

/*
 * ts_dir.h —— 时序模态的**分区目录持久化**（v1.5 步骤 2 · T5）
 *
 * 【这一层解决什么】
 *
 * 时序数据按时间落进"分区"（partition）。分区目录要回答两个问题：
 *
 *   1. **现在有哪些分区？** 每个分区的时间窗、状态、行数、页在哪。
 *   2. **重开库时怎么把它们找回来？**（目录必须**在磁盘上**，
 *      不能只活在内存里 —— 否则重开库就是"空库"。）
 *
 * 这一层只管目录的**存与取**；路由、写入分区、封窗由 tsstore.c 管。
 *
 * 【为什么要单独一层，而不是塞进 tsstore.c】
 *
 * tsstore.c 要同时管"时间路由"和"分区页的写"，已经很满了。
 * 把目录的磁盘格式单独拎出来，好处是：
 *   - 磁盘格式只有一处定义，测试可以**直接按字节构造坏数据**；
 *   - 目录的读写路径可以**独立单测**（不牵扯封窗、压缩、SPI）。
 *
 * 【目录放在哪：模态自管，不进 catalog】
 *
 * 计划判决 1：catalog **不知道任何模态**。所以分区目录放在
 * 模态自己的页里（PT_TS_META / PT_TS_DIR），不进 catalog。
 *
 * 【磁盘格式】
 *
 *   一张 **meta 页**（PT_TS_META）
 *       └─ 记：格式版本、分区窗口秒数、目录链的头/尾、条目数（水位线）
 *   若干张 **目录页**（PT_TS_DIR），用 H_NEXT 串成一条链
 *       └─ 每张目录页最多 126 条**定长**目录项
 *
 * 目录项（32 字节，定长）：
 *
 *   +0  i64  start_ms      分区起始时间（含）
 *   +8  i64  end_ms        分区结束时间（不含）
 *   +16 u32  state         TS_PART_ACTIVE / SEALED / DROPPED
 *   +20 u32  n_rows        行数
 *   +24 u32  first_page    分区首页号（PAGE_ID_NIL = 空分区）
 *   +28 u32  page_count    占用的页数
 *
 * 【为什么目录项是定长的】（坑 M6-21）
 *
 * 目录项的字段是**契约定死的**（tsstore.h 的 TsPartInfo），
 * 每条都是 32 字节 —— 天然就是定长的。
 *
 * 定长的好处：
 *   - 追加一个分区 = 往尾页的下一个槽位写 32 字节，
 *     **不用重排整页**，也不用维护槽位偏移表；
 *   - 读的时候可以按序号直接算出偏移（`TSD_DATA + i*32`），
 *     不用先扫槽位目录；
 *   - 页里能放多少条是**编译期常量**（`(PAGE_SIZE-36)/32 = 126`），
 *     "放不下"这件事一眼就能看出来。
 *
 * 变长（槽位目录）能换来"以后加字段不用改格式"，但代价是
 * 自己维护槽位表 —— 而这一层的字段本来就是契约固定的，
 * 变长的好处拿不到。**格式版本号 + 严格的版本校验**已经解决了
 * "以后要加字段"的问题（见坑 M6-25）。
 *
 * 放不下时的行为：**自动扩页**（新分配一张目录页接在链尾），
 * 绝不静默丢分区（坑 M6-23）。
 *
 * 【怎么在开库时找到 meta 页】（坑 M6-22）
 *
 * 扫页空间：0 .. pager_npages()-1 逐页看页类型。
 *
 * ⚠️ 这是**已知欠账**（与地理模态的坑 M5-9 同一个病根）：
 * 开库时间随库大小线性增长。
 *
 * 能不能做得更好？在这个内核里**不能** —— 模态不许碰 catalog，
 * 而"页号 → 模态"的登记处正好在 catalog 手里（catalog 不知道任何模态）。
 * 要根治，就得让 db 层给模态**发一个页号**（那要改 db 层，超出本任务范围）。
 *
 * 所以本层提供两条路：
 *   - `ts_dir_open()`      —— 扫页空间找（现状，与 geo 同口径）；
 *   - `ts_dir_open_hint()` —— 调用方**已经知道**页号时直接用（零扫描）。
 * 后者是给将来 db 层接线用的 —— 接线时只要把页号传下来，
 * 开库就变成 O(1)，不用再扫。
 *
 * 顺带一提：meta 页通常在文件**靠前**的位置（建时序存储时分配），
 * 扫到就停 ⇒ 绝大多数情况下扫不到几页。**最坏情况**才是 O(npages)。
 *
 * 【崩溃一致性：现在只做到"不静默丢"，还没做到"不丢"】（坑 M6-23）
 *
 * 本层**不进 WAL**（WAL 是步骤 3+ 的事，tsstore.h 里也没有 WAL 钩子）。
 * 所以一次断电可能停在"尾目录页写了一半" ⇒ 该页校验和不过
 * ⇒ 开库报 **E_CORRUPT**（而不是读出半截目录当正常）。
 *
 * 这是"宁可报错，不可静默错"的取舍：
 *   - 目录项的**内容**是就地改写（状态/行数变化），写坏了整页校验和就废；
 *   - 但**追加**新分区只写链尾的新页，老页不动 ⇒ 断电最多丢掉
 *     "最后没写完的那一个分区"，其余分区完好。
 *
 * 真正的解法是 WAL（写日志 → 再改页 → LSN 写进页头）。
 * 本层把页格式设计成"将来能接 WAL"的样子（页头留了 H_LSN）。
 *
 * 【meta 页里的条目数是"水位线"】（坑 M6-24）
 *
 * meta 页记的 n_parts 是**目录链上条目数的下界**：
 * 写顺序是"先写目录页，后写 meta 页"，所以断电只可能让
 * 链上的条数**多于** meta 记的（meta 那次写没赶上）。
 *
 * 于是：
 *   - 链上条数 **>** meta 记的 ⇒ 正常（meta 落后了），以链为准；
 *   - 链上条数 **<** meta 记的 ⇒ **链被截断了**（目录页丢了）⇒ E_CORRUPT。
 *
 * 这一条是"不许读出半截数据当正常"的关键：没有它，
 * 链断在半路时开库会"成功地"返回一个少了几个分区的目录 ——
 * 那是**静默漏数据**，比报错坏得多。
 *
 * 【目录项的不变量：读写两侧都要校验】（坑 M6-25）
 *
 * 每条目录项必须满足：
 *   - start_ms < end_ms（时间窗非空且不倒挂）；
 *   - state ∈ {ACTIVE, SEALED, DROPPED}；
 *   - page_count == 0 ⇔ first_page == PAGE_ID_NIL（有页就有首页，没页就没首页）。
 *
 * 写入时校验（挡住调用方的错），读出时**再校验一遍**
 * （挡住坏数据）。读出时校验是必须的：磁盘上的东西可能来自
 * 旧版本、可能坏了一半、可能被别的程序改过 —— 一条不满足不变量
 * 的项就是**假数据**，会让"查这个时间窗"返回错的分区。
 *
 * 【本层与 tsstore.c 的分工】
 *
 * 本层**只存不解释**：它不判断一个分区该不该存在、该不该被 TTL 丢掉，
 * 只负责把 (start, end, state, rows, first_page, page_count) 这六个字段
 * **原样存取**。状态怎么流转由 tsstore.c 决定。
 *
 * 唯一的例外是 `ts_dir_find()`：它**跳过 DROPPED 的分区**
 * （已回收的分区不该再被查到 —— 这与 tsstore.h 里
 * "找不到的常见原因之一是已被 TTL 丢弃"一致）。
 */

#include "common/error.h"
#include "common/types.h"
#include "modalities/ts/tsstore.h"
#include "storage/pager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 页类型与版本 ---------------- */

/*
 * 页类型接在 PT_GEO_META(0x16) 之后。
 *
 * ⚠️ 与 geo_state.h / ft_root.h 一样，**没有动 common/config.h**
 * （那是全局页类型表，改它要全局对齐）。数值不与任何人冲突。
 */
#define PT_TS_META 0x17u   /* 时序分区目录的 meta 页 */
#define PT_TS_DIR  0x18u   /* 时序分区目录的目录页 */

/* 格式版本（各自一个） */
#define TS_DIR_META_VER 1u
#define TS_DIR_PAGE_VER 1u

/* ---------------- 磁盘格式 ---------------- */

/*
 * 页头的前 20 字节是 slotted page 的标准头（HDR_SIZE），
 * 本层的自定义字段一律从偏移 20 开始 —— 不走 slot 目录。
 */

/*
 * meta 页的自定义区：
 *
 *  20 TSM_VER    u32  格式版本（TS_DIR_META_VER）
 *  24 TSM_SPAN   u32  分区窗口（秒）
 *  28 TSM_HEAD   u32  目录链首页（PAGE_ID_NIL = 空目录）
 *  32 TSM_TAIL   u32  目录链尾页（冗余字段：以链为准）
 *  36 TSM_NPARTS u32  条目数**水位线**（链上条数只会 ≥ 它）
 */
enum {
    TSM_VER    = 20,
    TSM_SPAN   = 24,
    TSM_HEAD   = 28,
    TSM_TAIL   = 32,
    TSM_NPARTS = 36
};

/*
 * 目录页的自定义区：
 *
 *  20 TSD_VER   u32  格式版本（TS_DIR_PAGE_VER）
 *  24 TSD_NENT  u32  本页条目数（> TS_DIR_MAX_ENT ⇒ E_CORRUPT）
 *  28 TSD_NEXT  u32  下一张目录页（PAGE_ID_NIL = 链尾）
 *  32 TSD_PREV  u32  上一张目录页（PAGE_ID_NIL = 链首）
 *  36 TSD_DATA  ...  目录项数组：每条 TS_DIR_ENT_SIZE 字节
 */
enum {
    TSD_VER   = 20,
    TSD_NENT  = 24,
    TSD_NEXT  = 28,
    TSD_PREV  = 32,
    TSD_DATA  = 36
};

/* 目录项大小（定长，32 字节） */
#define TS_DIR_ENT_SIZE 32u

/* 一张目录页最多放多少条目录项：(4096 - 36) / 32 = 126 */
#define TS_DIR_MAX_ENT 126u

/* 目录项内部的字段偏移（相对条目起始处） */
enum {
    TSE_START = 0,   /* i64 分区起始时间（含） */
    TSE_END   = 8,   /* i64 分区结束时间（不含） */
    TSE_STATE = 16,  /* u32 状态 */
    TSE_ROWS  = 20,  /* u32 行数 */
    TSE_FIRST = 24,  /* u32 分区首页号 */
    TSE_PAGES = 28   /* u32 分区占用页数 */
};

/* ---------------- 目录对象 ---------------- */

/*
 * 目录状态（调用方持有）。
 *
 * 里面既有磁盘上的位置（meta 页 / 目录链），也有内存里的镜像
 * （条目数组 + 按时间排序的下标数组）。
 *
 * 【为什么还要一份内存镜像】
 *
 * 查一个分区（"t 落在哪个分区"）是**读路径上的热操作**。
 * 每次都去磁盘上走一遍目录链 ⇒ 一次查询读 O(目录页数) 个页。
 * 目录在内存里就不用了。
 *
 * 内存镜像的**权威是磁盘**：任何写操作都先落盘再改内存，
 * 重开库时从磁盘重建（ts_dir_open）。
 */
typedef struct TsDir TsDir;

/* ---------------- API ---------------- */

/*
 * 新建一个分区目录：分配一张 meta 页（空目录）。
 *
 * span_sec —— 分区窗口（秒），必须 > 0。
 *
 * 返回 OK / E_NOSPACE / E_INVAL。
 */
int ts_dir_create(Pager *pg, u32 span_sec, TsDir **out);

/*
 * 打开磁盘上已有的分区目录（**扫页空间**找 meta 页，坑 M6-22）。
 *
 * 磁盘上没有 meta 页 → E_NOTFOUND（**不是错误**：
 * 那是个还没建过时序存储的库 —— 与 geo / graph 同一个口径）。
 * meta 页或目录页读不懂 → E_CORRUPT（**不返回半截目录**）。
 */
int ts_dir_open(Pager *pg, TsDir **out);

/*
 * 同上，但调用方**已经知道** meta 页号 —— 零扫描。
 *
 * 这是给将来的 db 层接线用的：只要把页号传下来，
 * 开库就是 O(1)，不用扫页空间（坑 M6-22 的出路）。
 *
 * 页号不对 / 页读不懂 → E_NOTFOUND（页号不对）或 E_CORRUPT（页坏了）。
 */
int ts_dir_open_hint(Pager *pg, u32 meta_page, TsDir **out);

/* 释放（**不释放页** —— 页归 pager 管）。 */
void ts_dir_destroy(TsDir *d);

/*
 * 追加一个分区（目录里加一条）。
 *
 * 尾目录页放不下 ⇒ **自动扩页**（新分配一张目录页接在链尾），
 * 绝不静默丢分区。真的没空间了（pager 分配失败）才返回 E_NOSPACE。
 *
 * info 里的字段必须满足目录项的不变量（坑 M6-25），
 * 否则返回 E_INVAL。
 *
 * 返回 OK / E_NOSPACE / E_INVAL。
 */
int ts_dir_append(TsDir *d, const TsPartInfo *info);

/*
 * 原地更新第 idx 条（**磁盘序**，见 ts_dir_get）。
 *
 * 典型用法：分区被封窗（state 改 SEALED、n_rows 定了）或行数变了。
 * 落盘是**就地改写**那一页的对应槽位。
 *
 * idx 越界 → E_INVAL；字段不满足不变量 → E_INVAL。
 */
int ts_dir_update(TsDir *d, u32 idx, const TsPartInfo *info);

/*
 * 取第 idx 条（**磁盘序** = 追加顺序）。
 *
 * idx 越界 → E_INVAL。
 */
int ts_dir_get(const TsDir *d, u32 idx, TsPartInfo *out);

/*
 * 取**按起始时间升序**的第 i 条。
 *
 * tsstore.h 的 ts_store_part_info() 承诺"按起始时间升序"，
 * 而磁盘上的顺序是**追加顺序**（写入通常是时间递增的，
 * 但回填旧数据就会乱序）—— 所以这一层维护了一份排序下标。
 *
 * i 越界 → E_INVAL。
 */
int ts_dir_get_by_time(const TsDir *d, u32 i, TsPartInfo *out);

/*
 * 找包含 t_ms 的分区。
 *
 * 找不到 → E_NOTFOUND（那个时间窗还没写过 / 已被 TTL 丢弃）。
 * **DROPPED 的分区会被跳过**（页已回收，不该再查到）。
 *
 * idx_out —— 命中项的**磁盘序**下标（可为 NULL）。
 */
int ts_dir_find(const TsDir *d, i64 t_ms, u32 *idx_out, TsPartInfo *out);

/* 分区个数 */
u32 ts_dir_count(const TsDir *d);

/* 所有分区的行数之和 */
u64 ts_dir_total_rows(const TsDir *d);

/* 当前分区窗口（秒） */
u32 ts_dir_span_sec(const TsDir *d);

/* 改分区窗口（只影响**之后**的写入；已有分区不动）。落盘到 meta 页。 */
int ts_dir_set_span_sec(TsDir *d, u32 span_sec);

/* 目录链有几张目录页（诊断用） */
u32 ts_dir_page_count(const TsDir *d);

/* 累计逻辑页读（诊断用，与 db_*_page_reads 同一口径） */
u64 ts_dir_page_reads(const TsDir *d);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_TS_TS_DIR_H */
