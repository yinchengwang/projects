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
