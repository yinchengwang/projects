#ifndef DB_MODALITIES_FT_WRITER_H
#define DB_MODALITIES_FT_WRITER_H

/*
 * ft_writer.h —— 段写入器（内部接口，v1.3 步骤 3）
 *
 * 【为什么要有这一层】
 *
 * "把一批 posting 写成一个段"这件事有**两个**调用方：
 *
 *   1. 建段（ft_segment_build.c）：从文档切词、聚合出 posting，再写段；
 *   2. 合并（ft_merge.c）：把若干段的 posting 归并后，写成一个新段。
 *
 * 两边要写的物理结构**完全一样**（同样的三种页、同样的编码）。
 * 如果各写一份，迟早会写出两种不一样的段 —— 然后检索时两种都能读？
 * 不，只会对其中一种读对，另一种静默返回错结果。
 *
 * 所以把"写页"这一层抽出来，两边共用。
 * 抽象的边界画在"posting 的输入"上：
 *   上层负责**算出 posting 序列**（怎么算的是上层的事），
 *   这一层只负责**把 posting 序列写成段**（怎么写的是这一层的事）。
 *
 * 【内存里的 posting 与磁盘上的 posting 是两回事】
 *
 * 磁盘上的 posting 是 12 字节定长三字段（delta / tf / dl），
 * 而且 doc_id 是 delta 编码的 —— 那层编码在这一层里做。
 * 这一层对外的输入是**绝对 doc_id** 的 FtPosting（内存结构）。
 */

#include "common/loghook.h"
#include "common/types.h"
#include "modalities/ft/ft_segment.h"
#include "storage/pager.h"

/* 一条 posting（内存形态 FtPosting）定义在 ft_segment.h —— 段与合并都要用 */

/* 段写入器的对外句柄（结构体不透明，实现在 ft_writer.c 里） */
typedef struct FtPostWriter FtPostWriter;
typedef struct FtDictWriter FtDictWriter;

/* ---------------- 连续页块（v1.3 欠账 2） ---------------- */

/*
 * FtBlock（一个段的连续页块）的定义与实现都在 **ft_segment.h / ft_segment.c**
 * —— 它是"段怎么占页"的概念，属于段层；写入器只是**用**它。
 */

/* ---------------- 倒排页写入器 ---------------- */

/*
 * 新建一个倒排页写入器。
 *
 * 页是**按需分配**的：第一次写 posting 时才分配第一页，
 * 所以"一个词都没写"的段不占倒排页。
 *
 * block —— 段的连续页块（见上面的说明）：倒排页与字典页都从它里面顺序取，
 * 保证"段头页 → 倒排页 → 字典页"三者连号。
 */
FtPostWriter *ft_post_writer_new(Pager *pg, FtBlock *block,
                                 LogHookFn wal, void *wal_ctx, u32 xid);
void ft_post_writer_free(FtPostWriter *w);

/*
 * 写一个词的 posting（按 doc_id 递增传入）。
 *
 * ps / n —— 这个词的 posting（**必须按 doc_id 严格递增**，
 *   delta 编码依赖这个顺序；不递增会算出负 delta）。
 *
 * *first_ptr_out —— 返回这个词**第一个 run** 的位置（写进字典词条）。
 *
 * 放不下就拆段：当前页写能放下的部分，剩下的写进下一页，
 * 用 FTR_NEXT_RUN 串起来（契约见 ft_internal.h）。
 */
int ft_post_writer_put(FtPostWriter *w, const FtPosting *ps, u32 n, u64 *first_ptr_out);

/* 把当前页写盘（收尾时必须调，否则最后一页不落盘） */
int ft_post_writer_flush(FtPostWriter *w);

/* 已分配的倒排页区间 */
u32 ft_post_writer_start(const FtPostWriter *w);
u32 ft_post_writer_count(const FtPostWriter *w);
/* 最后一次写日志拿到的 LSN（段头要记它） */
u32 ft_post_writer_last_lsn(const FtPostWriter *w);

/* ---------------- 字典页写入器 ---------------- */

/* 字典写入器与倒排写入器共用同一个块（见上面的说明） */
FtDictWriter *ft_dict_writer_new(Pager *pg, FtBlock *block,
                                 LogHookFn wal, void *wal_ctx, u32 xid);
void ft_dict_writer_free(FtDictWriter *w);

/*
 * 写一个词条。**调用方必须按字典序递增传入**（页内 slot 顺序 = 字典序，
 * 跨页二分靠的就是这个序）。
 */
int ft_dict_writer_put(FtDictWriter *w, const char *term, u32 term_len,
                       u32 df, u64 post_ptr);

/* 把当前页写盘（收尾时必须调） */
int ft_dict_writer_flush(FtDictWriter *w);

u32 ft_dict_writer_start(const FtDictWriter *w);
u32 ft_dict_writer_count(const FtDictWriter *w);
u32 ft_dict_writer_last_lsn(const FtDictWriter *w);

/* ---------------- 页数模拟器（W-D 的 D-2） ---------------- */

/*
 * 按**写入器的真实分页规则**模拟"这批输入要占几页"。
 *
 * 【为什么要有它 —— 老公式为什么不够】
 *
 * 合并出新段之前要先算"新段要占多少页"，好一次性预留一块连号的字典区。
 * 老公式是「字节数 ÷ (每页容量 − 每页最多浪费的字节) + 1」，
 * 那个减数（35 / 最长词条）是**猜出来的最坏余量**：
 * 它假设每页都刚好浪费掉一整个最长词条的空间，而实际浪费通常小得多。
 *
 * 后果两头都有：算多了 → 池里凑不出那么长的连号段 → 每轮合并都向文件尾
 * 追加 → 文件随合并次数线性增长（页回收等于白做）；算少了 → 写入时页块
 * 耗尽 → E_NOSPACE。
 *
 * 【为什么"逐条模拟"是精确的】
 *
 * 写入器的打包规则是**确定的**，没有任何随机或数据依赖的余量：
 *   倒排页：本页剩余 < 12 + 12 就换页；否则放入 min(剩余条数, 容量) 条，
 *           每个词在本页还要 12 字节 run 头；词没写完就在新页从 FPH_DATA
 *           处另起一个续接 run 头（**也要 12 字节**）。
 *   字典页：used + (词条长 + 4) > FT_DICT_CAPACITY 就换页。
 * 照这个规则走一遍，得到的页数与真正写出来的**逐个相等**。
 *
 * ⚠️ 这两条规则**必须与 ft_writer.c 的 ft_post_writer_put /
 * ft_dict_writer_put 保持一致**。哪天写入器改了分页规则，这里要跟着改，
 * 否则算出来的上界会与实际不符（上界小于实际 ⇒ 合并失败）。
 * 一致性由 test_ft_merge_pages.c 的四条边界测试盯着。
 */
typedef struct {
    u32 pages;   /* 已开的页数（0 = 一页都还没开，空输入就是 0） */
    u32 used;    /* **当前页**数据区已用字节（不含页头） */
} FtPageSim;

void ft_pagesim_reset(FtPageSim *s);

/* 追加 n 条 posting（同一个词）。n == 0 时不占页（空词不进段）。 */
void ft_pagesim_add_postings(FtPageSim *s, u32 n);

/* 追加一个词条（词长 term_len）。 */
void ft_pagesim_add_dict_entry(FtPageSim *s, u32 term_len);

/* ---------------- 段头页 ---------------- */

/*
 * 写段头页。
 *
 * head_pid —— 段头页的页号（**必须先分配、内容最后写**：
 *   段头要记字典/倒排的页区间，那些要等页都分配完才知道）。
 *
 * create_lsn —— 建段的 LSN（取最后一次内容页写入的 LSN）。
 *   不能用段头页自己的 LSN：那个值要等段头页自己的日志写完才知道，
 *   而日志镜像里必须已经带着它，否则重放出来的页和原页不一样。
 */
int ft_write_segment_head(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                          u32 head_pid, u32 seg_id,
                          u32 doc_count, u32 max_doc_id,
                          u32 dict_start, u32 dict_count,
                          u32 post_start, u32 post_count,
                          u32 n_terms, u64 total_len, u32 create_lsn);

/* ---------------- 通用整页写（v1.3 步骤 5） ---------------- */

/*
 * 把 buf 的内容写进 pid 这一页（写日志 → 填 LSN → 算校验和 → 标脏放开）。
 *
 * 【与建段时的整页写有什么不同】
 *
 *   - 前像（before 镜像）= **这一页现在的内容**，不是全 0。
 *     建段时那些页是刚分配的（前像 = 0）；而 tombstone 页、段头页
 *     是**反复改写**的老页 —— 前像必须是改之前的内容，
 *     否则未提交事务的修改没法撤销（undo 拿到的会是垃圾）。
 *   - buf 是一份**独立的副本**，不能直接传 fr->data
 *     （那样"前像"会取到已经被改过的内容）。
 *
 * 页的帧（fr）在调用前必须已 pin；**无论成功失败，本函数都会放开它**。
 */
int ft_page_write(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                   u32 pid, PageFrame *fr, const u8 *buf, u32 *lsn_out);

/*
 * 改段头页的状态字段（FTH_STATE / FTH_MERGE_TO）。
 *
 * 合并时用它把老段标成 MERGING / DEAD（契约见 ft_internal.h）。
 * 整页镜像进 WAL —— 状态也是数据，改错了要能撤销。
 */
int ft_write_segment_state(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                           u32 head_pid, u32 state, u32 merge_to);

#endif /* DB_MODALITIES_FT_WRITER_H */
