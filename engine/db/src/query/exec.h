#ifndef DB_QUERY_EXEC_H
#define DB_QUERY_EXEC_H

#include "catalog/catalog.h"
#include "query/parser.h"
#include "spi/registry.h"
#include "spi/storage_am.h"
#include "storage/value.h"
#include "txn/mvcc.h"
#include "txn/txn.h"

/*
 * 数据库句柄：pager + catalog + 事务表 + 模态注册表。上层（db.c）持有它。
 *
 * v0.7 的结构：
 *   - txns 指向**共享的**事务表（指针，不是副本！）
 *     所有连接共用一个 xid 分配器和 clog。
 *     如果每个连接各持一份拷贝，两个连接会分配出相同的 xid ——
 *     可见性判断立刻失效（未提交的数据被当成已提交）。
 *   - snap 是**连接私有**的快照，读路径靠它做可见性判断
 *   - cur_xid 是当前事务号，写路径靠它打 xmin/xmax
 *
 * 【这里没有 in_txn 字段】
 *
 * v0.7 最初在 exec.h 的 Db 里也放了一个 in_txn，和 db.c 里
 * DB 结构体的 in_txn 重名。两个字段各写各的：
 *   txn_start 只设置 Db.in_txn
 *   db_exec 只检查 DB.in_txn
 * 结果 COMMIT; 永远报"不在事务中"。
 *
 * 教训：**同一个语义只能有一个存放处**。
 * 事务状态属于"连接"的概念，归 db.c 的 DB 结构管；
 * exec.h 的 Db 只放内核执行真正需要的字段（快照、当前 xid）。
 *
 * 【v0.9：reg 是模态注册表】
 *
 * 指向**实例级共享**的注册表（虚拟表：名字 → StorageAM）。
 * 所有连接看到同一批虚拟表 —— 和 catalog 一样是实例级共享。
 */
typedef struct Db {
    Pager       *pager;
    Catalog     *cat;
    TxnTable    *txns;
    SpiRegistry *reg;      /* 模态提供的虚拟表 */
    Snapshot     snap;
    u32          cur_xid;  /* 当前事务号；0 = 系统事务 */
} Db;

/*
 * 结果集：物化。cell 一律是格式化后的字符串（NULL 值存 "NULL"），
 * 这样公开 API 的 db_result_value 可以直接返回 const char*。
 */
typedef struct QueryResult {
    int   ncols;
    int   nrows;
    int   affected;      /* 非 SELECT 语句的受影响行数 */
    char **colnames;
    char **cells;        /* nrows * ncols */
} QueryResult;

/*
 * 执行一条语句。
 *
 * err / errcap 是**错误描述的输出缓冲**（可选，传 NULL 就没有）。
 * 失败时把原因写进去（比如"表 blob_frames 不支持插入"），
 * 由调用方决定怎么呈现。
 */
int  exec_stmt(Db *db, Stmt *st, QueryResult **out, char *err, int errcap);

/*
 * 重建索引。
 *
 * force = true  → 全量重建（崩溃恢复后用：堆可能已经往前走了）
 * force = false → 只重建**格式过期**的索引（正常开库用）
 *
 * v0.6 里索引不写日志，所以索引永远是派生物，重建没有语义风险。
 */
int  exec_rebuild_indexes(Db *db, bool force);
void qr_free(QueryResult *r);

#endif /* DB_QUERY_EXEC_H */
