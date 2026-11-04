#ifndef DB_MODALITIES_FT_TOMB_H
#define DB_MODALITIES_FT_TOMB_H

#include "common/types.h"
#include "storage/pager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ft_tomb.h —— tombstone 集合落盘的**对外小接口**（v1.3 欠账 1 · 坑 M4-24）
 *
 * 【为什么单独开一个头文件】
 *
 * 删除的**写路径**在 ft_tomb.c 里（往 tombstone 页追加一条 doc_id），
 * 但"这条追加怎么写日志"只有**上层**知道 —— WAL 对象在 db 层手里，
 * 而 modalities/ 不许依赖 db/（分层铁律）。
 *
 * 于是这里定义一个**回调**：上层（db_ft.c）注册一个函数，
 * ft_tomb.c 在写日志的时候调它。
 *
 * 【注册的是"函数"，不是"函数 + 上下文"】
 *
 * 上下文（ctx）由**调用方**每次传：ft_tomb.c 传的是 ix->wal_ctx
 * —— 也就是这个索引写日志时用的上下文（db 层里是 DbInstance）。
 *
 * 这么做的原因：注册是**进程级**的一份（跟 wal/recovery.h 的
 * recovery_set_hook 同一个套路），如果把上下文也注册成全局的，
 * 同时开两个库时后开的那个会覆盖先开的那个 ——
 * 先开的库删文档时，日志会写进**另一个库**的日志文件（静默的数据损坏）。
 *
 * 现在注册的只是一个"不认识具体实例"的通用函数，
 * 实例从每次调用的 ctx 里拿 —— 同时开几个库都不串。
 *
 * 坑 M4-28：进程级注册的东西只能注册"与实例无关的函数"，
 * 实例相关的上下文必须由调用方每次传下去。
 */

/*
 * 写一条 tombstone 追加记录的回调。
 *
 * ctx      —— 调用方传的上下文（ix->wal_ctx；db 层里是 DbInstance）；
 * xid      —— 这次删除属于哪个事务（0 = 系统事务 = 永远已提交）；
 * page_id  —— 这次追加落在哪张 tombstone 页上；
 * doc_id   —— 被删的文档号；
 * *lsn_out —— 写成功时回填这条记录的 LSN（调用方要写进页头）。
 *
 * 返回非 OK → 日志没写好 → 调用方**不许改页**。
 */
typedef int (*FtTombLogFn)(void *ctx, u32 xid, u32 page_id, u32 doc_id, u32 *lsn_out);

/* 注册 / 注销"写 tombstone 追加记录"的回调（进程级一份，注册后不变）。 */
void ft_tomb_set_log_hook(FtTombLogFn fn);

/*
 * 恢复期用：把 doc_id 追加到 tombstone 页的**末尾**。
 *
 * **幂等**：这一页里已经有这个 doc_id 就什么都不做
 * （崩溃后重放同一条记录不能追加出两份）。
 *
 * **先写日志，后改页**：页的 LSN 设成 lsn ——
 * 刷盘前的 WAL 规则检查（页 LSN <= 已落盘的日志 LSN）靠的就是它。
 *
 * lsn —— 这条记录的 LSN。
 */
int ft_tomb_page_append(Pager *pg, u32 page_id, u32 doc_id, u32 lsn);

/*
 * 恢复期用：把 doc_id 从 tombstone 页里**移除**（按值找）。
 *
 * 只移除自己那一条：找到就把它后面的条目往前挪一格、count 减一；
 * 找不到就什么都不做（已经撤过了 / 从没生效过）。
 *
 * 【为什么按值找而不是按位置】
 *
 * 同一页上，已提交的追加可能排在未提交的追加**后面** ——
 * 撤销中间那条会让后面的条目移位，所以必须按 doc_id 找。
 */
int ft_tomb_page_remove(Pager *pg, u32 page_id, u32 doc_id, u32 lsn);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_FT_TOMB_H */
