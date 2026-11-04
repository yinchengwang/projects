#ifndef DB_MODALITIES_FT_SEGMENT_H
#define DB_MODALITIES_FT_SEGMENT_H

/*
 * ft_segment.h —— 全文检索的"段"（segment）（v1.3 步骤 2）
 *
 * 一个段 = 一批文档构建出来的**不可变**倒排索引：
 *
 *   段头页（1 页）+ 字典页（若干）+ 倒排页（若干）
 *
 * 本版（步骤 2）只做**单段**：建一个段、在这个段里检索。
 * 多段、删除标记、合并是步骤 3 的事。
 *
 * 【为什么不把段做成"隐式的"】
 *
 * 段是磁盘上的物理结构，它的边界（哪些页属于哪个段）必须是显式的。
 * 本版把段的清单放在内存里（调用方自己拿 head_page 存着），
 * 落盘的段目录留到步骤 4/5（那时才需要"重开库还能找到段"）。
 */

#include "common/loghook.h"
#include "common/types.h"
#include "modalities/ft/bm25.h"
#include "storage/pager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 段的内存表示：**只有段头**在内存里，
 * 字典页和倒排页都是按需从页里读的（这是 A5 "IO 可证"的前提）。
 */
typedef struct {
    Pager *pg;
    u32    head_page;
    u32    seg_id;
    u32    doc_count;      /* N */
    u64    total_len;      /* 所有文档词数之和 */
    u32    max_doc_id;
    u32    n_terms;
    u32    dict_start;
    u32    dict_count;
    u32    post_start;
    u32    post_count;
    u64    page_reads;     /* 检索引起的页读次数（诊断用，A5 的度量） */
} FtSegment;

/* 平均文档长度（词数）。avgdl = 0 的坑见 bm25.h 的 M4-9。 */
static inline double ft_segment_avgdl(const FtSegment *seg)
{
    if (seg->doc_count == 0)
        return 0.0;
    return (double)seg->total_len / (double)seg->doc_count;
}

/*
 * 一条 posting（内存形态）：绝对 doc_id + 该词的 tf + 文档长度（词数）。
 *
 * 【为什么内存里的 posting 是绝对 doc_id，磁盘上却是 delta】
 *
 * delta 编码是**存盘时**的事（省空间），而且它要求 posting 按 doc_id 递增。
 * 上层（建段、合并）手里的 posting 都是绝对 doc_id —— 合并时要把两个段的
 * posting **归并**（不是拼接），归并完才能算 delta。
 * 所以"绝对 → delta"这一步必须由写页的那一层做（ft_writer.c）。
 */
typedef struct {
    u32 doc_id;
    u32 tf;
    u32 dl;
} FtPosting;

/* ---------------- 段级空闲页池（v1.3 欠账 2） ---------------- */

/* ---------------- 连续页块（v1.3 欠账 2） ---------------- */

/* 空闲页池（完整定义在下面的"段级空闲页池"一节；这里先前置声明） */
typedef struct FtPagePool FtPagePool;

/*
 * 一个段的页必须**连号**（坑 M4-23）：段头只记**连续区间**
 * （dict_start + dict_count / post_start + post_count），
 * 读侧的跨页二分与词条游标都按"连续区间"读页 ——
 * 页号不连号就会读到别的页（查不到 / 命中 0 / 静默丢词）。
 *
 * 而"复用旧段腾出来的页"（空闲页池）天然是**零散**的：
 * 池里的页号来自若干个旧段，不保证连号。
 *
 * 所以复用的粒度必须是**连续块**：一次性从池里取一段连号的页，
 * 段的页从这块里顺序取用（段头页 → 倒排页 → 字典页）。
 *
 * 三种模式：
 *
 *   1. **有界块**：start..start+len 是已经存在的页（来自空闲页池）。
 *      取到 len 还没用完 → E_NOSPACE（报错，不越界取页）。
 *   2. **无界块**：end = PAGE_ID_NIL，页按需向文件尾追加
 *      （与老行为一致：单测与不回收的老口径）。
 *   3. **池模式**（坑 M4-52）：页从空闲页池里**按整段连号**取，
 *      段与段之间用页链（H_NEXT）串起来 —— 也就是说**这一块的页不必连号**。
 *
 * 【坑 M4-52：为什么倒排区可以"不连号"，字典区不行】
 *
 * 读侧对两个区的要求不一样：
 *
 *   - **字典区**：读侧按"连续区间"（dict_start + i）二分查词 →
 *     页号不连号就会读到别的页（查不到 / 命中 0 / 静默丢词）→ **必须连号**。
 *   - **倒排区**：读侧顺着 posting 的链（run 头的"下一段 run"指针）走，
 *     页与页之间靠页链（H_NEXT）串着 → **页号不必连号**。
 *
 * 而空闲页池里的页天然是零散的（来自若干个旧段），
 * 死守"整块连号"就会让池越用越碎、最后凑不出一块连号页 → 文件只涨不缩。
 * 所以倒排区改成"池模式"：从池里一段一段地取，每段内部连号，段与段之间用页链串。
 *
 * 代价：段的倒排页**不再是一个连续区间** —— 段头里记的 post_start / post_count
 * 变成"链头 + 链长"，回收/回收判定必须**顺着页链走**（不能再按区间算）。
 */
typedef struct {
    Pager *pg;
    u32    start;  /* 块的第一页 */
    u32    next;   /* 下一个可用页号 */
    u32    end;    /* 块尾（不含）；PAGE_ID_NIL = 无界（向文件尾追加） */

    /* 池模式专用（坑 M4-52） */
    FtPagePool *pool;    /* 页从哪个池里取 */
    u32    chunk_next;   /* 当前这一段连号页里下一个可用页号 */
    u32    chunk_end;    /* 当前这一段的尾（不含） */
    u32    n_used;       /* 已经发出去的页数 */
} FtBlock;

/* 初始化一个块：len = 0xFFFFFFFF 表示"无界"（内部换算成 PAGE_ID_NIL） */
void ft_block_init(FtBlock *b, Pager *pg, u32 start, u32 len);

/*
 * 初始化一个**池模式**的块（坑 M4-52）。
 *
 * 页从 pool 里按"整段连号"取：取到一段就用这一段，
 * 这一段用完了再取下一段（段与段之间由写入器用页链串起来）。
 * 池里没有页了 → 向文件尾追加（页号天然连号，正确性不受影响）。
 */
void ft_block_init_pool(FtBlock *b, Pager *pg, FtPagePool *pool);

/* 从块里取一页（已 pin 的帧）。块用尽 → E_NOSPACE。 */
int ft_block_alloc(FtBlock *b, u32 *pid_out, PageFrame **fr_out);

/* 块里已经用掉的页数（用来把没用完的尾巴还给空闲页池） */
u32 ft_block_used(const FtBlock *b);

/*
 * 把池模式块里**没用完的那一段**的尾巴还给空闲页池（坑 M4-52）。
 *
 * 尾巴是连号的（一整段的剩余部分）→ 归还后池里仍然是一段连号页 ✓
 * （不会把池切碎）。
 */
int ft_block_pool_release_tail(FtBlock *b, FtPagePool *pool);

/*
 * 给一个段**预留**它的连续页块（v1.3 欠账 2）。
 *
 * pool != NULL 且池里有够长的连号段 → 用它（有界块，复用旧页）；
 * 否则 → 无界块（向文件尾追加，页号天然连号）。
 *
 * need —— 这个段最多要几页（见 FtSegBound）。
 *
 * 【取哪一段：最佳适配】
 *
 * 池里够长的段可能有好几段，取**最短的那一段**（刚好够用的优先）——
 * 长段留给真正需要长连号页的地方（字典区），别为 1 页的段头砍掉长段。
 */
void ft_seg_block_reserve(FtPagePool *pool, Pager *pg, u32 need, FtBlock *blk);

/*
 * 给"**必须连号**"的区预留页块（字典区用，坑 M4-52）。
 *
 *   1. 池里有够长的连号段 → 借它（有界块，复用旧页，文件不增长）；
 *   2. 池里凑不出 → 在文件尾预留一段连号的页（ft_block_reserve_at_end）。
 *
 * 两条路都是**有界块**：别的区再怎么写也顶不掉这一段 → 这一区一定连号 ✓。
 *
 * 返回 OK / E_NOSPACE / E_INVAL（失败时**不**给出一个坏块）。
 */
int ft_seg_block_reserve_strict(FtPagePool *pool, Pager *pg, u32 need, FtBlock *blk);

/*
 * 在文件尾**预留**一段连号的页（len 页），做成一个"有界块"。
 *
 * 为什么需要它：页是"谁写谁占"的 —— 两个区交替向文件尾要页，
 * 字典页就会被倒排页顶掉 → 字典区不再连号（坑 M4-23）。
 * 所以必须**先占好一整段**，再开始写。
 */
int ft_block_reserve_at_end(Pager *pg, u32 len, FtBlock *blk);

/*
 * 段级**空闲页表**（内存态）。
 *
 * 【为什么需要它】
 *
 * 合并把 f 个旧段合成一个新段之后，旧段的页（段头 + 字典 + 倒排）
 * 就**没人引用**了 —— 但它们还占着文件里的位置。
 * 没有这张表，磁盘文件就随合并次数**线性增长**：
 * 每合并一次，新段的页往后追加，旧段的页永远不还。
 *
 * 【为什么不是"文件级空闲页表"】
 *
 * 本引擎的 pager 没有文件级 freelist（pager_alloc 只能在文件尾追加），
 * 而 storage/freelist.c 那张表是"表内页剩余空间档位表"（btree/heap 在用），
 * 语义完全不同，不能挪用。
 * 所以 FT 在**模态内部**自己记一张"哪些页可以再拿去用"的表。
 *
 * 【为什么是内存态】
 *
 * 它是**优化**，不是正确性的依赖：进程重启后这张表是空的，
 * 那些"已经没人引用"的页就白占着位置（开机回收是 wave 2 的活，
 * 见 ft_segment_reclaimable_pages）。
 * 宁可浪费空间，也不能因为表丢了就**读错数据**。
 *
 * 【为什么不用位图】
 *
 * 空闲页的数量与库的大小同阶，但"空闲"这件事是**零星的**（一次合并才有）。
 * 用一个 u32 数组存页号，内存占用与"真正空闲的页数"成正比 ——
 * 比给整个文件开一张位图省得多。
 */
typedef struct FtPagePool FtPagePool;

/* 新建一个空闲页池（空池） */
FtPagePool *ft_pool_new(void);

/* 释放空闲页池（不碰磁盘上的页 —— 那些页只是"没人引用"） */
void ft_pool_free(FtPagePool *pool);

/*
 * 登记一个空闲页。
 *
 * 重复登记同一个页会被拒绝（返回 E_CORRUPT）：同一个页被发两次
 * = 两个段共用一页 = 数据互相踩踏，必须挡住。
 */
int ft_pool_add(FtPagePool *pool, u32 page_id);

/* 池里现在有多少个空闲页 */
u32 ft_pool_count(const FtPagePool *pool);

/* 累计登记过多少个页（诊断用：回收了多少页） */
u64 ft_pool_added_total(const FtPagePool *pool);

/*
 * 分配一页：**优先**从空闲池里取，取不到才向文件尾追加。
 *
 * 取到的页会被**彻底重置**（整页清零）——
 * 复用页最怕"旧段的字节残留"：页头（H_TYPE / H_LSN）与私有区
 * 一旦有残留，读侧就会把旧段的格式当成新段的格式来解释。
 *
 * *pid_out —— 分配到的页号；*fr_out —— 该页的帧（已 pin，脏）。
 */
int ft_pool_alloc(FtPagePool *pool, Pager *pg, u32 *pid_out, PageFrame **fr_out);

/*
 * 从池里**取走**一段连号的页（取走后它们就不再是空闲页）。
 *
 * 【为什么必须是"连号的一段"】
 *
 * 段头只记**连续区间**（坑 M4-23），段的页必须连号；
 * 而空闲池里的页是零散的（来自若干个旧段）——
 * 一页一页分配出来的页一定不连号，读侧按连续区间读页就会读错。
 *
 * 所以这里找池里**最长的连号段**，取它的前 need 页。
 *
 * need —— 要几页；*start_out —— 这段页的第一页页号。
 *
 * 池里没有这么长的一段 → 返回 **E_NOTFOUND**（调用方改走
 * "向文件尾追加"那条路）—— 池空时不必先占着一段页。
 */
int ft_pool_take_run(FtPagePool *pool, u32 need, u32 *start_out);

/*
 * 从池里取走**最长的那一整段**连号页（整段取走，**不切分**）。
 *
 * 【为什么要"整段取走"】
 *
 * 切分一段长页会把它变成两段短页 —— 短页对"要连号"的用户（字典区）没用，
 * 对"不要连号"的用户（倒排区）却更碎。整段取走 → 池里的段只会变短不会变碎。
 *
 * *start_out / *len_out —— 这一段的首页页号与页数。
 * 池是空的 → E_NOTFOUND。
 */
int ft_pool_take_longest_run(FtPagePool *pool, u32 *start_out, u32 *len_out);

/*
 * 从池里取走**最短的、够长的一段**（"最佳适配"）。
 *
 * 【为什么不用"最长的一段"】
 *
 * 长段是稀缺资源：字典区必须连号（坑 M4-52），只有长段能满足它。
 * 要 1 页却从 25 页的长段上砍 → 长段变短，短段堆积。
 * 最佳适配把"刚好够用"的那一段给出去 → 长段留给真正需要连号的地方。
 *
 * need —— 要几页；*start_out —— 取到的这一段的首页页号。
 * 池里没有够长的一段 → E_NOTFOUND。
 */
int ft_pool_take_run_best_fit(FtPagePool *pool, u32 need, u32 *start_out);

/* 登记一段连续的空闲页（[start, start+len)） */
int ft_pool_add_range(FtPagePool *pool, u32 start, u32 len);

/* ---------------- 新段的页数上界 ---------------- */

/*
 * 一个新段要占多少页的**上界**（段头页 + 倒排页 + 字典页）。
 *
 * 【为什么必须先算出上界】
 *
 * 复用的页是**一段连号的页**（坑 M4-23），一次性从空闲池里取走 ——
 * 所以写页之前就得知道"这个段最多要几页"，
 * 否则要么取少了（写到一半没页了 → 段写坏了），
 * 要么取多了（多出来的页浪费掉，文件反而长得更快）。
 *
 * 【上界怎么来的】
 *
 *   倒排页：源段的总字节 ≤ Σ 源段倒排页数 × 每页容量；
 *           新段每页最多浪费（页尾放不下 run 头 + 一条 posting 的那点空间
 *           \+ 一个 run 头）字节 → 页数 ≤ 总字节 ÷ (容量 − 浪费) + 余量。
 *   字典页：源段的总字节 ≤ Σ 源段字典页数 × 每页容量；
 *           新段每页最多浪费"最大词条 + slot"字节（放不下就换页）。
 *
 * 合并只会**减少**词条与 posting（重复词合并、被删的文档丢弃），
 * 所以源段的总量就是新段的上界 ✓。
 */
typedef struct {
    u32 head_pages;   /* 段头页（恒为 1） */
    u32 post_pages;   /* 倒排页上界 */
    u32 dict_pages;   /* 字典页上界 */
    u32 total;        /* 合计（= head + post + dict） */
} FtSegBound;

/* 按源段的统计量算新段的页数上界（合并时用） */
void ft_seg_bound_merge(FtSegment *const *segs, u32 n_segs, FtSegBound *out);


/*
 * 把一个段的**全部页**（段头 + 字典区 + 倒排区）登记进空闲池。
 *
 * 【只能在合并提交之后调】
 *
 * 段一旦被合并掉，它的页就成了垃圾；但在合并**提交之前**，
 * 老段还可能要被回滚（合并失败时老段必须还能用），
 * 提前登记 = 把还在用的页发出去 = 老段的数据被踩。
 *
 * seg —— 段对象（只读它的页区间字段，不读页内容）。
 *
 * 【坑 M4-52：倒排区要顺着页链走，不能按区间算】
 *
 * 倒排区的页**不必连号**（读侧顺着 posting 的链走），
 * 所以 post_start + post_count 不是一个页号区间 ——
 * 回收时必须从 post_start 顺着页链（H_NEXT）走 post_count 步，
 * 把链上的每一个页号登记进池。
 */
int ft_seg_recycle_pages(FtPagePool *pool, const FtSegment *seg);

/*
 * 算出"页空间里没人引用"的 FT 页（**开机回收用的接口**，本轮只留接口）。
 *
 * 【本轮为什么不做开机回收】
 *
 * 开机时要判断"哪些页是垃圾"，光有"活着的段"还不够：
 * 还得知道**历史上存在过哪些段**（死段的页也是垃圾）。
 * 而"段目录"（哪些段存在过、哪些死了）这一版的落盘方式是
 * "段头页里的状态字段"，恢复时是**扫页空间**把段找齐的（坑 M4-20）——
 * 扫出来的是**活着的**段，死段的页号就再也说不清了。
 * 所以真正的开机回收要等下一组的"段目录页"（wave 2）。
 *
 * segs / n_segs —— 当前**存活**的段（它们的页不能回收）。
 * out / max_out —— 输出缓冲（页号）。
 * 返回写出的页数。
 *
 * 【口径】只认 FT 自己的三种页（PT_FT_HEAD / PT_FT_DICT / PT_FT_POST）；
 * tombstone 页（PT_FT_TOMB）**不算垃圾** —— 它是删除集合的权威存储，
 * 由 tomb 那一路自己管（它没有"根页"，回收它等于丢数据）。
 */
u32 ft_segment_reclaimable_pages(Pager *pg, FtSegment *const *segs, u32 n_segs,
                                 u32 *out, u32 max_out);

/*
 * 建一个段：把一批文档建成不可变的倒排索引。
 *
 * pg / wal / wal_ctx / xid —— 段要占页，页的改动必须进 WAL
 *   （用 LOG_PAGEDATA 整页镜像，和向量模态的做法一致）。
 *   wal 为 NULL 时不写日志（单测用）。
 *
 * docs / n_docs —— 语料。**doc_id 必须互不相同**
 *   （同 id 的两篇文档在段里会合并成一条 posting，
 *   而线性扫描那边还是两条 —— A1 的对照就不成立了）。
 *
 * seg_id —— 段号，由调用方给（步骤 3 的段目录要按它排序）。
 *
 * *head_page_out —— 返回段头页号（段的唯一标识）。
 *
 * 返回值：OK / E_INVAL / E_NOSPACE / E_CORRUPT。
 */
int ft_segment_build(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                     const FtDocText *docs, u32 n_docs, u32 seg_id,
                     u32 *head_page_out);

/*
 * 同上，但**页可以从空闲页池里取**（v1.3 欠账 2）。
 *
 * pool —— 空闲页池；NULL 表示"不回收，直接向文件尾追加"（单测的老口径）。
 *
 * 为什么要有两个入口：ft_segment_build 的签名被单测固定住了，
 * 改签名就要动别人的测试文件。这里多出一个带 pool 的版本，
 * 老入口退化成"pool = NULL 的薄壳"，语义完全不变。
 */
int ft_segment_build_ex(Pager *pg, FtPagePool *pool,
                        LogHookFn wal, void *wal_ctx, u32 xid,
                        const FtDocText *docs, u32 n_docs, u32 seg_id,
                        u32 *head_page_out);

/*
 * 打开一个已存在的段（读段头页）。
 *
 * 段头页的类型或版本不对 → E_CORRUPT（宁可打不开，也不要跑出错误的答案）。
 */
int ft_segment_open(Pager *pg, u32 head_page, FtSegment **out);

/* 释放段的内存表示（不刷页、不关库） */
void ft_segment_close(FtSegment *seg);

/*
 * 在一个段里检索。
 *
 * query / query_len —— 查询串，内部用分词器切词
 *   （与线性扫描那边用同一个分词器，语义逐位一致）。
 *
 * top_k —— 要返回多少条。
 * max_out —— out 的容量。**top_k > max_out 直接报 E_NOSPACE**，
 *   不做静默截断（截断出来的结果"看起来对其实错"，比报错坏得多）。
 *
 * 语义（与 bm25.h 的线性扫描完全一致）：
 *   - 至少命中一个 query 词的文档才进结果；
 *   - query 里的重复词**不去重**，各贡献一次（M4-12）；
 *   - 排序：分数降序、分数相同按 doc_id 升序（M4-11）。
 */
int ft_segment_search(const FtSegment *seg, const char *query, u32 query_len,
                      u32 top_k, u32 max_out, FtHit *out, u32 *n_out);

/* 同上，但用教科书式 TF-IDF 打分（对照组，验收 A2 用） */
int ft_segment_search_tfidf(const FtSegment *seg, const char *query, u32 query_len,
                            u32 top_k, u32 max_out, FtHit *out, u32 *n_out);

/* 检索引起的页读次数（A5 的度量：单 term 检索的页读应与文档量无关） */
u64 ft_segment_page_reads(const FtSegment *seg);

/* 段的词数 */
u32 ft_segment_nterms(const FtSegment *seg);

/*
 * 查字典：找 term 的 df 与倒排链头（跨段统计量靠它聚合）。
 *
 * 找不到返回 E_NOTFOUND（词不在这个段里 —— 这是**正常结果**，不是错误）。
 */
int ft_segment_lookup(const FtSegment *seg, const char *term, u32 term_len,
                      u32 *df_out, u64 *post_ptr_out);

/*
 * 按字典序遍历段里的**全部词条**（合并时用）。
 *
 * 回调收到：词、词长、df、倒排链头。
 * 回调返回非 OK → 立刻停止遍历，并把该值原样返回。
 */
typedef int (*FtTermFn)(void *ctx, const char *term, u32 term_len, u32 df, u64 post_ptr);
int ft_segment_scan_terms(const FtSegment *seg, FtTermFn fn, void *ctx);

/*
 * 读一个词的**全部 posting**（合并时用）。
 *
 * post_ptr / df —— 字典词条里的倒排链头与 posting 条数。
 * out / max_out —— 输出缓冲与容量；装不下返回 E_NOSPACE（**不静默截断**）。
 * n_out —— 实际读出的条数。
 *
 * 读出来的 posting 按 doc_id 递增（格式保证）。
 */
int ft_segment_postings(const FtSegment *seg, u64 post_ptr, u32 df,
                        FtPosting *out, u32 max_out, u32 *n_out);

/* ---------------- 带页缓存的 posting 读取（v1.3 欠账 4） ---------------- */

/*
 * 合并时按**字典序**读 posting，而倒排页也是按这个词序写的 ——
 * 所以**相邻两个词的 posting 往往在同一张页上**。
 *
 * 不缓存的话，每个词都要重新 pager_get 一次它那一页
 * → 逻辑页读是 O(段内词数)，而段只有 O(段页数) 张页。
 * 缓存住"上一次读的那张页"，相邻词命中同一页时就不用再取 ——
 * 逻辑页读降到接近 O(段页数)。
 *
 * ⚠️ **口径：这是逻辑页读（pager_get 的次数），不是物理磁盘 IO。**
 * buffer pool 会把页缓存起来，同一页反复取只是多次**逻辑**读；
 * 真正的物理 IO 由 buffer pool 的换入换出决定。
 * 这个口径与 A4/A5 用的页读口径**完全一致**（都是 pager_get 次数），
 * 所以前后可比。
 */
typedef struct {
    Pager     *pg;
    PageFrame *fr;      /* 当前 pin 着的那张页 */
    u32        page;    /* 它的页号 */
    bool       valid;   /* fr 有效吗 */
} FtPostCache;

/* 初始化一个页缓存（还没有缓存任何页） */
void ft_post_cache_init(FtPostCache *cache, Pager *pg);

/* 放开缓存里 pin 着的页（用完必须调，否则那一页一直被 pin 着） */
void ft_post_cache_release(FtPostCache *cache);

/*
 * 读一个词的**全部** posting（与 ft_segment_postings 语义完全相同），
 * 但会**复用** cache 里已经读进来的页。
 *
 * 语义（必须与 ft_segment_postings 一致，否则合并结果会与段不一致）：
 *   - posting 按 doc_id 递增；
 *   - df > max_out → E_NOSPACE（不静默截断）；
 *   - 页读计数照旧记进 seg->page_reads（**只算真的取过的页**）。
 */
int ft_segment_postings_cached(const FtSegment *seg, FtPostCache *cache,
                                u64 post_ptr, u32 df,
                                FtPosting *out, u32 max_out, u32 *n_out);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_FT_SEGMENT_H */
