/*
 * db_stmt.c — 事务控制 + 语句执行（v0.9 拆文件）
 *
 * 设计说明见 src/db.c 的文件头注释。
 *
 * 【为什么单独成文件】
 * 硬规则：单文件不超过 800 行。db.c 装下生命周期 + 事务 + 执行 + blob API
 * 之后到了 1075 行。blob API 拆到 db_blob.c，事务与执行拆到这里。
 */

#include "db_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "query/parser.h"

#include "common/latch.h"

/* 真正的实现 */
static db_err db_exec_impl(DB *db, const char *sql, Result **out);

/*
 * 事务的开始与结束。
 *
 * 【为什么要把事务生命周期从 db_exec 里抽出来】
 *
 * v0.6：每条语句自动包一个事务，用户看不到事务的概念。
 * v0.7：用户可以显式 BEGIN ... COMMIT，多条语句是一个原子单位。
 *
 * 抽出来的原因：显式事务里，"一条语句失败"不等于"整个事务结束"。
 * 语句失败 → 事务进入 aborted 状态（PG 的 "in failed transaction"），
 * 后续语句一律报错，直到用户显式 ROLLBACK。
 *
 * 为什么这样设计：很多数据库都这样（PG 就是）。原因是
 * "语句失败后事务处于什么状态"这个问题如果不统一处理，
 * 各个语句的失败路径会写出不一致的语义。
 */
static int txn_start(DB *db, bool explicit_begin)
{
    u32 xid;
    int rc = txn_begin(&db->inst->txns, &xid);
    if (rc != OK)
        return rc;

    db->xid = xid;
    db->db.cur_xid = xid;
    db->in_txn = true;
    db->txn_failed = false;

    /*
     * 新事务 → 隔离级别重新可设置，且回到默认（RC）。
     *
     * PG 的 SET TRANSACTION 只对当前事务生效 ——
     * 下一个事务重新从 session 默认值（RC）开始。
     */
    db->iso_locked = false;
    db->iso_level = ISO_READ_COMMITTED;

    /*
     * 日志侧：记 BEGIN，xid 要对上。
     *
     * xid 显式传 —— 不能用 wal_set_xid 那种共享状态，
     * 多连接下会串号（见 wal.h 里的说明）。
     */
    wal_append_xid(db->inst->wal, WAL_BEGIN, xid, PAGE_ID_NIL, 0, 0,
                   NULL, 0, NULL, 0);

    /*
     * 快照。
     *
     * RC（语句级）：每条语句都建新快照 → 能看到别人刚提交的。
     * RR（事务级）：整个事务用同一个快照 → 事务开始后提交的看不到。
     *
     * 显式事务（BEGIN）用 RR —— 用户明确说了"这几条语句是一个原子单位"，
     * 那它们看到的必须是同一个世界快照。
     *
     * 自动事务（每语句一个）用 RC —— 语句级快照和事务级快照效果一样，
     * 但 RC 的实现更简单（每条语句刷新）。
     */
    mvcc_snapshot(&db->inst->txns, xid, &db->db.snap);

    (void)explicit_begin;
    return OK;
}

/* 事务正常结束：提交 */
static int txn_end_commit(DB *db)
{
    wal_append_xid(db->inst->wal, WAL_COMMIT, db->xid, PAGE_ID_NIL, 0, 0,
                   NULL, 0, NULL, 0);
    int rc = wal_fsync(db->inst->wal);
    if (rc != OK)
        return rc;

    txn_commit(&db->inst->txns, db->xid);

    db->db.cur_xid = 0;
    db->in_txn = false;
    db->txn_failed = false;
    return OK;
}

/* 事务异常结束：中止 */
static int txn_end_abort(DB *db)
{
    wal_append_xid(db->inst->wal, WAL_ABORT, db->xid, PAGE_ID_NIL, 0, 0,
                   NULL, 0, NULL, 0);
    wal_fsync(db->inst->wal);

    txn_abort(&db->inst->txns, db->xid);

    db->db.cur_xid = 0;
    db->in_txn = false;
    db->txn_failed = false;
    return OK;
}

/*
 * db_exec —— 语句执行的公共入口。
 *
 * ==================== v1.2 步骤 4d：大锁退役 ====================
 *
 * v0.10 的答案是"整个语句执行过程持全局 latch"。这一版把它拆掉了 ——
 * 因为语句期间访问的实例级共享状态，现在**各有各的域锁**：
 *
 * | 状态 | 保护者 |
 * |---|---|
 * | 表/列/索引元数据 | catalog 读写锁（步骤 3） |
 * | next_xid / clog / 活跃表 | txn 域锁（步骤 4a） |
 * | 日志缓冲 / LSN 计数 / 日志文件 | wal 域锁（步骤 4b） |
 * | 页内容 / 缓冲池账本 | frame 域锁（步骤 4c） |
 * | 空闲页空间索引 | freelist 域锁（步骤 4c） |
 *
 * 这些域锁合起来覆盖了原来一把大锁的**全部保护范围** ——
 * 步骤 4c 的缩小实验已经证明：**去掉大锁后全套测试 58 PASS / 0 FAIL**。
 *
 * 【但模态语句还留着大锁】
 *
 * blob / graph / vector 三个模态的**内部状态**（blob 的对象目录、
 * graph 的邻接表、vector 的索引）**还没有自己的域锁** ——
 * 它们一直靠这把全局 latch 兜着。
 *
 * 所以：语句碰到**模态表**时，才把全局 latch 取回来。
 * 关系模态的语句（占了绝大多数）**完全不再持这把锁**，
 * 两条语句可以真正并行。
 *
 * 模态存储各自配锁是后续工作（那三个存储的内部结构比 catalog 复杂，
 * 锁序要单独论证）。在此之前，模态路径保持"一把锁串行"这个现状。
 *
 * 【为什么用"是不是模态表"来判断，而不是给模态打标签】
 *
 * 注册表（SpiRegistry）里装的**就是**模态提供的虚拟表；
 * 关系模态的表在 catalog 里，不在注册表里。
 * 所以"语句的表在不在注册表里"就是"这条语句是不是模态语句"的判据，
 * 不需要另设一个标签位。
 *
 * 【锁序】
 *
 * 全局 latch 不在按域的全序里（它是"模态域"的粗粒度锁）。
 * 它的位置是**最外层**：模态语句持它 → 模态存储 → pager（frame 域）→ wal。
 * 与全序不冲突 —— 全局 latch 只在**语句入口**取一次，
 * 不会与域锁形成环（域锁之间有全序断言把关）。
 */
db_err db_exec(DB *db, const char *sql, Result **out)
{
    return db_exec_impl(db, sql, out);
}

static db_err db_exec_impl(DB *db, const char *sql, Result **out)
{
    if (!db || !sql)
        return DB_E_INVAL;
    if (out)
        *out = NULL;

    /*
     * 【顺序很重要：先解析，再决定要不要开事务】
     *
     * BEGIN / COMMIT / ROLLBACK 是事务控制语句，它们自己管理事务，
     * 不该被"自动事务"包一层。所以必须先解析出来才知道。
     *
     * 我第一版把 txn_start 放在 parse 之前，结果 BEGIN 语句
     * 一进来 txn_start 就把 in_txn 置成了 true，接着 BEGIN 分支
     * 又判断"已经在事务中"而报错 —— 自己和自己打架。
     */
    Stmt st;
    char err[256] = {0};
    int rc = parse_sql(sql, &st, err, (int)sizeof(err));
    if (rc != OK) {
        snprintf(db->errmsg, sizeof(db->errmsg), "%s", err[0] ? err : "语法错误");
        return DB_E_PARSE;
    }

    bool is_txn_ctl = (st.kind == ST_BEGIN || st.kind == ST_COMMIT ||
                       st.kind == ST_ROLLBACK);

    /*
     * SET TRANSACTION ISOLATION LEVEL
     *
     * 规则（对齐 PG）：
     *   - 必须在事务内设置（事务外设置报错）
     *   - 必须在事务的第一条语句之前设置（改了之后就不能再改）
     *
     * PG 允许事务外设置（作为下一个事务的默认值），
     * 但那样"什么时候生效"很容易误解 —— 用户以为设了，
     * 结果下一个 BEGIN 之后才生效。这里收紧成"必须在事务内"。
     */
    if (st.kind == ST_SET_ISOLATION) {
        if (!db->in_txn) {
            snprintf(db->errmsg, sizeof(db->errmsg),
                     "SET TRANSACTION 必须在事务内执行");
            free_stmt(&st);
            return DB_E_TXN;
        }
        if (db->iso_locked) {
            snprintf(db->errmsg, sizeof(db->errmsg),
                     "隔离级别只能在事务开始时设置一次");
            free_stmt(&st);
            return DB_E_TXN;
        }
        db->iso_level = (st.iso_level == 1) ? ISO_REPEATABLE_READ
                                            : ISO_READ_COMMITTED;
        db->iso_locked = true;
        free_stmt(&st);
        if (out) {
            Result *r = calloc(1, sizeof(*r));
            if (!r)
                return DB_E_IO;
            r->qr = NULL;
            *out = r;
        }
        return DB_OK;
    }

    /*
     * 事务控制语句的处理。
     *
     * 语义（对齐 PG）：
     *   - BEGIN：不在事务中 → 开一个；在事务中 → 报错（不做隐式提交）
     *   - COMMIT / ROLLBACK：不在事务中 → 报错
     *   - 事务失败后：只有 ROLLBACK 被接受，其他语句报 DB_E_TXN
     */
    if (is_txn_ctl) {
        db_err terr = DB_OK;

        if (st.kind == ST_BEGIN) {
            if (db->in_txn) {
                snprintf(db->errmsg, sizeof(db->errmsg), "已经在事务中");
                terr = DB_E_TXN;
            } else {
                rc = txn_start(db, true);
                if (rc != OK)
                    terr = map_rc(rc);
                else
                    db->txn_failed = false;
            }
        } else if (st.kind == ST_COMMIT) {
            if (!db->in_txn) {
                snprintf(db->errmsg, sizeof(db->errmsg), "不在事务中");
                terr = DB_E_TXN;
            } else {
                rc = txn_end_commit(db);
                if (rc != OK)
                    terr = map_rc(rc);
            }
        } else {   /* ST_ROLLBACK */
            if (!db->in_txn) {
                snprintf(db->errmsg, sizeof(db->errmsg), "不在事务中");
                terr = DB_E_TXN;
            } else {
                rc = txn_end_abort(db);
                if (rc != OK)
                    terr = map_rc(rc);
            }
        }

        free_stmt(&st);
        if (terr != DB_OK)
            return terr;

        if (out) {
            Result *r = calloc(1, sizeof(*r));
            if (!r)
                return DB_E_IO;
            r->qr = NULL;
            *out = r;
        }
        return DB_OK;
    }

    /*
     * 已经在一个失败的事务里 → 什么都不许做，只能 ROLLBACK。
     * （PG 的 "current transaction is aborted" 语义）
     */
    if (db->txn_failed && db->in_txn) {
        snprintf(db->errmsg, sizeof(db->errmsg), "事务已失败，需要 ROLLBACK");
        return DB_E_TXN;
    }

    /* 每条语句一个事务（除非已经在显式事务里） */
    bool own_txn = !db->in_txn;
    if (own_txn) {
        rc = txn_start(db, false);
        if (rc != OK)
            return map_rc(rc);
    }

    /*
     * 【RC 的关键：每条语句刷新快照】
     *
     * RC（Read Committed）的定义是"每条语句看到一致的快照"。
     * 实现上就是：语句开始时重新建一次快照。
     *
     * RR（Repeatable Read）则跳过这里 —— 快照在事务开始时建好，
     * 整个事务复用同一个（见 txn_start）。
     *
     * 这就是两种隔离级别的**全部差别**。
     * 可见性判断逻辑（mvcc_visible）完全一样。
     *
     * 为什么刷新放在 exec_stmt 之前而不是之后：
     * 快照要反映"这条语句开始时"的世界状态。
     *
     * 注意自己写的数据始终可见 —— my_xid 的特判在 mvcc_visible 里，
     * 刷新快照不会让自己看不见自己刚插的行。
     */
    if (db->iso_level == ISO_READ_COMMITTED && db->in_txn) {
        mvcc_snapshot(&db->inst->txns, db->xid, &db->db.snap);
    }

    QueryResult *qr = NULL;
    db->errmsg[0] = '\0';            /* 让执行器写新的错误描述 */

    /*
     * 模态语句才取全局 latch（理由见 db_exec 的注释）。
     *
     * 判据：这条语句的表在**注册表**里 → 它是模态提供的虚拟表。
     * 关系模态的表在 catalog 里，不在注册表里。
     */
    bool modality = (st.table[0] != '\0' &&
                     spi_registry_find(db->inst->reg, st.table) != NULL);

    if (modality)
        latch_lock();
    rc = exec_stmt(&db->db, &st, &qr, db->errmsg, (int)sizeof(db->errmsg));
    if (modality)
        latch_unlock();

    free_stmt(&st);

    if (rc != OK) {
        if (own_txn)
            txn_end_abort(db);
        else
            db->txn_failed = true;
        if (!db->errmsg[0])
            snprintf(db->errmsg, sizeof(db->errmsg), "执行失败: %s", err_name(rc));
        return map_rc(rc);
    }

    if (own_txn) {
        rc = txn_end_commit(db);
        if (rc != OK) {
            qr_free(qr);
            return map_rc(rc);
        }
    }

    if (out) {
        Result *r = calloc(1, sizeof(*r));
        if (!r) {
            qr_free(qr);
            return DB_E_IO;
        }
        r->qr = qr;
        *out = r;
    } else {
        qr_free(qr);
    }
    return DB_OK;
}
