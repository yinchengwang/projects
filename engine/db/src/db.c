/*
 * db.c — 公开 API 的实现（include/db.h）
 *
 * v0.7 的结构变化：**实例与连接分离**。
 *
 *   Instance（共享）              Connection（每个连接一份）
 *   ├─ 缓冲池 pager               ├─ 当前事务号 cur_xid
 *   ├─ catalog                    ├─ 当前快照 snap
 *   ├─ WAL 文件                   ├─ 事务状态 in_txn / failed
 *   └─ 事务表 txns（全局 xid）    └─ 错误信息 errmsg
 *
 * 为什么必须拆：
 *
 * v0.6 之前一个 DB 句柄 = 一个连接 = 一套缓冲池 + 一份 catalog。
 * 看起来"能开两个句柄连同一个库"，其实是**两个完全独立的数据库**：
 * 各自有缓冲池、各自加载了 catalog 副本、各自维护 freelist。
 * A 建的表 B 看不见，A 写的页 B 也看不见（缓存不一致）。
 *
 * 这不是 bug，是架构的必然边界。MVCC 的意义恰恰是
 * "多个连接看到同一个库的不同时间点"，所以必须有一个共享实例。
 *
 * 这正是 PG 的 postmaster/backend 模型：
 *   postmaster 拥有共享资源（缓冲池、catalog、clog）
 *   backend   每个连接一份（事务状态、快照）
 *
 * 【一个实现上的坑】
 *
 * 事务表 txns 是**共享**的（xid 必须全局唯一），
 * 但快照 snap 是**连接私有**的。
 * 所以 Db 结构体（exec.h 里那个）里的 txns 指向共享的表，
 * 而 snap 是每个连接自己的一份。
 */

#include "db_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "query/parser.h"

#include "common/latch.h"

#define BP_FRAMES_DEFAULT 256u

db_err map_rc(int rc)
{
    switch (rc) {
    case OK:         return DB_OK;
    case E_IO:       return DB_E_IO;
    case E_NOSPACE:  return DB_E_NOSPACE;
    case E_CORRUPT:  return DB_E_CORRUPT;
    case E_INVAL:    return DB_E_INVAL;
    case E_NOTFOUND: return DB_E_NOTFOUND;
    case E_NOTSUP:   return DB_E_NOTSUP;
    default:         return DB_E_INVAL;
    }
}

/*
 * 堆表的每次修改都会经过这里：先落日志，再改页。
 *
 * ctx 指向 DbInstance（不是某个连接句柄）——
 * 因为 WAL 是实例级共享的，日志钩子的生命周期必须跟实例一致。
 * 如果 ctx 指向 owner 那个 DB 结构体，owner 先关掉时它会被 free，
 * 其他连接继续写日志就是访问已释放内存。
 *
 * xid 由调用方（堆表层）显式传下来 —— 不能从共享状态里取。
 * 原因见 common/loghook.h 里关于"共享状态"的说明。
 */
int db_wal_hook(void *ctx, u32 xid, u8 logtype, u32 page_id, u16 slot,
                    u16 slot_off,
                    const u8 *before, u16 blen,
                    const u8 *after, u16 alen, u32 *lsn_out)
{
    DbInstance *inst = (DbInstance *)ctx;
    u8 type;

    switch (logtype) {
    case LOG_INSERT:   type = WAL_INSERT;   break;
    case LOG_DELETE:   type = WAL_DELETE;   break;
    case LOG_UPDATE:   type = WAL_UPDATE;   break;
    case LOG_PAGEINIT: type = WAL_PAGEINIT; break;
    case LOG_PAGEDATA: type = WAL_PAGEDATA; break;
    default: return OK;
    }

    u32 lsn = wal_append_xid(inst->wal, type, xid, page_id, slot, slot_off,
                             before, blen, after, alen);
    if (!lsn)
        return E_IO;

    *lsn_out = lsn;
    return OK;
}

/*
 * WAL 规则执行器：页要刷盘了，先保证它的日志落盘。
 *
 * 这就是 pager.c 里那个 TODO 的答案 —— 不是"检查一下然后报错"，
 * 而是"该 fsync 就 fsync"。因为在正常路径下，这个钩子**永远不该
 * 触发**：db 层每条语句结束时都会 wal_fsync，脏页要到更晚（缓冲池
 * 逐出或 db_close）才被刷盘，那时日志早就在盘上了。
 *
 * 什么时候会触发？缓冲池压力大、逐出频繁的时候 —— 事务还没提交，
 * 日志还在缓冲区。这时 fsync 一下是安全的（只是慢一点），
 * 因为多落盘几条日志不会让未提交的数据变成已提交。
 *
 * 关键点：这里只管"日志落盘"，不管"事务提交"。
 * 提交与否由 WAL_COMMIT 记录决定，和刷盘时机无关。
 */
static int wal_flush_before_page(void *ctx, u32 page_id, u32 page_lsn)
{
    DbInstance *inst = (DbInstance *)ctx;
    u32 flushed = wal_flushed_lsn(inst->wal);
    (void)page_id;

    if (page_lsn <= flushed)
        return OK;            /* 日志已经落盘，规则满足 */

    /* 这个页的日志还在缓冲区 —— 落盘它 */
    return wal_fsync(inst->wal);
}

/*
 * 打开数据库。
 *
 * 这是「实例」的生命周期起点：开缓冲池、开 WAL、跑恢复、开 catalog。
 * 之后可以用 db_connect 开更多连接，共享这一切。
 */
db_err db_open(const char *path, DB **out)
{
    if (!path || !out)
        return DB_E_INVAL;

    /*
     * 数据文件打开前是否存在？
     *
     * 这个信息用来区分两种"数据文件是空的"的情况：
     *
     *   (a) 文件压根不存在 → 这是个全新库。同名 WAL 如果有内容，
     *       那一定是上一个同名库的孤儿，必须清掉。
     *
     *   (b) 文件存在但 0 页 → 上次会话崩了，而且崩在第一次刷页之前。
     *       这时 WAL 里的内容是**这个库的**合法日志，必须重放！
     *
     * 之前把这两种情况混为一谈，用 "npages == 0 && 有日志" 一刀切地
     * 清 WAL，结果 (b) 情况下的数据全丢。
     *
     * 怎么发现的：v0.7 的集成测试里做"回滚 + 崩溃 + 重启"，
     * 重启后表直接消失了 —— 查下去发现 WAL 被当成孤儿清掉了。
     *
     * 怎么区分的：文件是否存在用 stat 判断（不是看页数）。
     * 崩溃后文件是"存在但 0 字节"，被删除的库是"文件不存在"。
     */
    bool datafile_exists = false;
    {
        FILE *probe = fopen(path, "rb");
        if (probe) {
            datafile_exists = true;
            fclose(probe);
        }
    }

    Pager *pg = NULL;
    int rc = pager_open(path, BP_FRAMES_DEFAULT, &pg);
    if (rc != OK)
        return map_rc(rc);

    DbInstance *inst = calloc(1, sizeof(*inst));
    if (!inst) {
        pager_close(pg);
        return DB_E_IO;
    }
    inst->pager = pg;

    DB *db = calloc(1, sizeof(*db));
    if (!db) {
        free(inst);
        pager_close(pg);
        return DB_E_IO;
    }
    db->inst = inst;
    db->is_owner = true;
    inst->nconns = 1;

    /*
     * WAL 钩子的 ctx 用实例（inst），不是某个连接句柄。
     *
     * 原因：钩子要活得比任何单个连接都久。
     * 如果 ctx 指向 owner 那个 DB 结构体，owner 先 db_close 时
     * 它会被 free，而其他连接还在写日志 —— 野指针。
     * 实例的生命周期 = 最后一个连接关闭，正好匹配。
     */
    pager_set_wal_flush(pg, wal_flush_before_page, inst);

    /* 日志文件：<db>-wal */
    size_t plen = strlen(path);
    inst->walpath = malloc(plen + 8);
    if (!inst->walpath) {
        pager_close(pg);
        free(inst);
        free(db);
        return DB_E_IO;
    }
    snprintf(inst->walpath, plen + 8, "%s-wal", path);

    rc = wal_open(inst->walpath, &inst->wal);
    if (rc != OK) {
        pager_close(pg);
        free(inst->walpath);
        free(inst);
        free(db);
        return map_rc(rc);
    }

    /*
     * 陈旧日志检查。
     *
     * 数据文件**不存在**（全新库），但同名日志文件里有内容 ——
     * 这说明日志是上一个同名数据库的孤儿。数据文件都没了，日志留着只会
     * 把不属于这个库的记录重放进来，把新库搞脏。
     *
     * 这种情况在测试里特别容易出现：remove("x.db") 忘了 remove("x.db-wal")。
     *
     * 【注意：只看"文件是否存在"，不看"页数是不是 0"】
     *
     * 如果按页数判断，就会误伤这一种情况：
     *   上次会话建了库、写了数据，但在第一次刷页之前就崩了（断电）。
     *   此时数据文件存在但是 0 字节，而 WAL 里全是这个库的合法日志。
     *   按页数判断会把 WAL 当成孤儿清掉 → 数据全丢。
     *
     * 判断依据是"文件在不在"：被 remove 掉的库，文件不存在；
     * 崩溃的库，文件存在（哪怕是 0 字节，因为 pager_open 创建了它）。
     */
    if (!datafile_exists && wal_nrecords(inst->wal) > 0) {
        rc = wal_reset(inst->wal);
        if (rc != OK) {
            wal_close(inst->wal);
            pager_close(pg);
            free(inst->walpath);
            free(inst);
            free(db);
            return map_rc(rc);
        }
    }

    /*
     * 事务号必须接着日志里的最大 xid 往下走。
     *
     * 跨会话重置 xid 是隐蔽的数据错误：上次会话做到 xid=5 崩了，
     * 这次会话又从 xid=1 开始。恢复阶段判断"哪些事务提交了"时，
     * 上次那个未提交的 xid=1 会被这次新的 xid=1 提交记录"连累"，
     * 判定成已提交 —— 未提交的数据就留在了库里。
     */
    u32 max_xid = wal_max_xid(inst->wal);

    /*
     * 初始化事务表，并**从日志重建 clog**。
     *
     * 为什么必须重建：MVCC 可见性判断完全依赖 clog —— "这个 xid 提交了没有"。
     * 如果重启后 clog 是空的，所有历史事务都会被当成"进行中"，
     * 于是重启后**所有已提交的数据都看不见了**。
     *
     * clog 只在内存里（不持久化），所以每次启动都要从日志重建一遍。
     * 这也意味着：日志被截断之前，必须先把 clog 的信息存到别处
     * （PG 就是这么做的 —— clog 是独立文件）。v0.7 不截断日志，所以没问题。
     */
    txn_table_init(&inst->txns, max_xid);

    RecoveredState xs;
    rc = recovery_scan_xids(inst->wal, &xs);
    if (rc != OK) {
        wal_close(inst->wal);
        pager_close(pg);
        free(inst->walpath);
        free(inst);
        free(db);
        return map_rc(rc);
    }
    for (int i = 0; i < xs.n; i++) {
        txn_clog_set(&inst->txns, xs.xids[i].xid,
                     xs.xids[i].committed ? TXN_CLOG_COMMITTED : TXN_CLOG_ABORTED);
    }

    /* 恢复必须在打开 catalog 之前：catalog 也是普通表，也要恢复 */
    bool did_recover = false;
    rc = recovery_run(pg, inst->wal, &did_recover);
    if (rc != OK) {
        wal_close(inst->wal);
        pager_close(pg);
        free(inst->walpath);
        free(inst);
        free(db);
        return map_rc(rc);
    }

    Catalog *cat = NULL;
    rc = catalog_open(pg, db_wal_hook, inst, &cat);
    if (rc != OK) {
        wal_close(inst->wal);
        pager_close(pg);
        free(inst->walpath);
        free(inst);
        free(db);
        return map_rc(rc);
    }
    inst->cat = cat;
    db->db.cat = cat;
    db->db.pager = pg;

    /*
     * blob 模态（v0.8）。
     *
     * blob 存储是**实例级共享**的 —— 所有连接看同一片对象空间。
     * 和 catalog 一样共享 pager 和 WAL。
     *
     * 它用同一个 wal_hook 写日志：blob 页必须进 WAL，
     * 否则崩溃后大对象会丢（页内容在盘上，日志里没有 → 重放不回来）。
     */
    rc = blob_store_create(pg, db_wal_hook, inst, &inst->blobs);
    if (rc != OK) {
        catalog_close(cat);
        wal_close(inst->wal);
        pager_close(pg);
        free(inst->walpath);
        free(inst);
        free(db);
        return map_rc(rc);
    }

    /*
     * 模态注册表 + blob 的 SPI 适配（v0.9）。
     *
     * 这是"blob 进 SQL"的接线点：
     *   - 注册表把名字（blob_frames / blob_objects）映射到 StorageAM
     *   - 执行器只认 StorageAM，不知道 blob 的存在
     *
     * 注册表是**实例级**的：所有连接看到同一批虚拟表。
     *
     * 【注册表不是持久化的】
     * 每次启动都重新注册 —— blob 自己的目录扫一遍就重建了。
     * 注册表是运行时结构，不进 catalog（模态不许碰关系模态的元数据）。
     */
    rc = spi_registry_create(&inst->reg);
    if (rc != OK) {
        blob_store_destroy(inst->blobs);
        catalog_close(cat);
        wal_close(inst->wal);
        pager_close(pg);
        free(inst->walpath);
        free(inst);
        free(db);
        return map_rc(rc);
    }

    rc = blob_am_build_frames(inst->blobs, &inst->blob_am_frames,
                              &inst->blob_ctx_frames);
    if (rc == OK)
        rc = blob_am_build_objects(inst->blobs, &inst->blob_am_objects,
                                   &inst->blob_ctx_objects);
    if (rc == OK)
        rc = spi_registry_add(inst->reg, "blob_frames", &inst->blob_am_frames);
    if (rc == OK)
        rc = spi_registry_add(inst->reg, "blob_objects", &inst->blob_am_objects);
    if (rc != OK) {
        spi_registry_destroy(inst->reg);
        blob_store_destroy(inst->blobs);
        catalog_close(cat);
        wal_close(inst->wal);
        pager_close(pg);
        free(inst->walpath);
        free(inst);
        free(db);
        return map_rc(rc);
    }

    db->db.reg = inst->reg;

    /*
     * 图模态（v0.11）。
     *
     * 和 blob 一样是**实例级共享**：所有连接看同一张图。
     *
     * 【打开时可能根本没有图】
     *
     * 磁盘上没建过图 → graph_open 返回 E_NOTFOUND。
     * 这不是错误（是个还没建图的库），所以 inst->graph 留空，
     * graph_nodes / graph_edges 这两张虚拟表也就不注册 ——
     * 查它们会得到"表不存在"，而不是"查出一张空表"。
     *
     * 为什么不给空表：空表会让人以为"图是空的"，
     * 而真相是"这个库压根还没有图"。这两件事要能被区分开。
     *
     * 【图坏了就让库打不开】
     *
     * graph_open 返回 E_CORRUPT = 磁盘上的 schema 读不懂。
     * 这时候继续开库 = 带着一份解释不了的图跑，
     * 后面每一次查询都在读垃圾。与其那样，不如开不了。
     */
    rc = graph_open(pg, db_wal_hook, inst, &inst->graph);
    if (rc == OK) {
        inst->graph_ready = true;
        rc = db_graph_register(inst);
    } else if (rc == E_NOTFOUND) {
        inst->graph = NULL;
        inst->graph_ready = false;
        rc = OK;
    }

    /*
     * 向量模态（v1.0）。
     *
     * 和 blob / graph 一样是**实例级共享**：所有连接看同一批向量。
     *
     * 【打开时可能根本没有向量存储】
     *
     * 和图一样：磁盘上没建过 → 不注册 vec_items →
     * 查它得到"表不存在"，而不是"查出一张空表"。
     *
     * 【索引会自动恢复】
     *
     * 索引是 checkpoint 式的（不写日志）：关库时 flush 过，
     * 开库就能直接读回；崩在两次 flush 之间，索引过期 →
     * vec_state_restore 会自动重建。
     *
     * **不需要用户做任何事** —— 这是"索引可以算出来"的好处。
     */
    if (rc == OK) {
        int vrc = db_vec_open(inst);
        if (vrc == E_NOTFOUND) {
            vrc = OK;       /* 这个库还没建向量存储 —— 不是错误 */
        }
        rc = vrc;
    }

    if (rc != OK) {
        graph_destroy(inst->graph);
        spi_registry_destroy(inst->reg);
        blob_store_destroy(inst->blobs);
        catalog_close(cat);
        wal_close(inst->wal);
        pager_close(pg);
        free(inst->walpath);
        free(inst);
        free(db);
        return map_rc(rc);
    }

    /*
     * 索引不写日志，崩溃后可能和堆不一致 —— 重建一遍。
     * 没崩也要看一眼：索引 key 的编码可能变过（比如 v1.1 改成复合键），
     * 旧格式的索引页拿新代码读会读出错的 key —— 那就只重建过期格式的。
     */
    (void)exec_rebuild_indexes(&db->db, did_recover);

    /*
     * 建立初始快照。
     *
     * 刚打开时没有任何活跃事务，所以快照的 xmin = xmax，
     * 意味着"所有已提交的事务都可见，未提交的都不可见"。
     */
    db->db.cur_xid = 0;
    db->in_txn = false;
    db->iso_level = ISO_READ_COMMITTED;   /* 默认 RC，对齐 PG */
    db->db.txns = &inst->txns;        /* 事务表是共享的 */
    mvcc_snapshot(&inst->txns, 0, &db->db.snap);

    *out = db;
    return DB_OK;
}

/*
 * 开一个新连接，共享实例资源。
 *
 * 共享：缓冲池、catalog、WAL、事务表（全局 xid 分配 + clog）
 * 私有：当前事务号、当前快照、事务状态、错误信息
 */
db_err db_connect(DB *db, DB **out)
{
    if (!db || !out)
        return DB_E_INVAL;
    *out = NULL;

    DB *conn = calloc(1, sizeof(*conn));
    if (!conn)
        return DB_E_IO;

    conn->inst = db->inst;
    conn->db.pager = db->inst->pager;
    conn->db.cat = db->inst->cat;
    conn->db.txns = &db->inst->txns;     /* 事务表共享 */
    conn->db.reg = db->inst->reg;        /* 模态注册表共享（实例级） */
    conn->db.cur_xid = 0;
    conn->in_txn = false;
    conn->iso_level = ISO_READ_COMMITTED;   /* 默认 RC，对齐 PG */

    /* 新连接的初始快照：当前已提交的都可见 */
    mvcc_snapshot(&db->inst->txns, 0, &conn->db.snap);

    /*
     * 引用计数是实例级共享状态 → 要持锁。
     * （连接本身是私有的，conn->xid 之类不用锁。）
     */
    latch_lock();
    db->inst->nconns++;
    latch_unlock();

    *out = conn;
    return DB_OK;
}

/* 真正关闭实例：刷页、写 CHECKPOINT、关 WAL */
static void instance_close(DbInstance *inst)
{
    /*
     * 向量索引先落盘。
     *
     * 索引是 checkpoint 式的：不落盘的话，下次开库要重建一遍
     * （读全部向量 + 重新构建）。而且这一步要写页，
     * 所以必须在 pager_close **之前**。
     */
    db_vec_close(inst);

    /*
     * 关闭顺序很重要：**先刷页，再写 CHECKPOINT**。
     *
     * 反过来的话：CHECKPOINT 先落盘，然后刷页时崩溃了 ——
     * 恢复时看到最后一条是 CHECKPOINT，就认为"上次是干净关闭"，
     * 不做任何重放。可实际上有些页还没落盘，数据就丢了。
     *
     * 正确顺序：
     *   1. 刷所有脏页（此时 WAL 钩子会保证日志先落盘）
     *   2. 写 CHECKPOINT 记录并 fsync
     *   3. 之后再有崩溃也不怕：所有页都已经在盘上了
     */
    pager_close(inst->pager);

    wal_append_xid(inst->wal, WAL_CHECKPOINT, 0, PAGE_ID_NIL, 0, 0,
                   NULL, 0, NULL, 0);
    wal_fsync(inst->wal);
    wal_close(inst->wal);

    catalog_close(inst->cat);
    blob_store_destroy(inst->blobs);
    graph_destroy(inst->graph);
    spi_registry_destroy(inst->reg);
    free(inst->walpath);
    free(inst);
}

db_err db_close(DB *db)
{
    if (!db)
        return DB_E_INVAL;

    DbInstance *inst = db->inst;

    /*
     * 整个关闭过程持锁：引用计数 + 实例销毁。
     *
     * 销毁时要 flush 所有脏页、写 CHECKPOINT、关 WAL ——
     * 这些都在动实例级共享状态。
     */
    latch_lock();
    inst->nconns--;

    if (inst->nconns <= 0) {
        instance_close(inst);
    } else {
        /* 还有别的连接在用：只关这个连接 */
        free(db);
    }
    latch_unlock();

    return DB_OK;
}

/*
 * 关一个连接（不是实例所有者那个）。
 *
 * 语义上和 db_close 一样，但明确表示"我只是关连接"。
 * 如果关的是最后一个连接，实例也会被关掉。
 */
void db_conn_close(DB *conn)
{
    if (!conn)
        return;
    db_close(conn);
}

void db_abandon(DB *db)
{
    if (!db)
        return;

    DbInstance *inst = db->inst;

    /* 同 db_close：引用计数 + 实例销毁都在临界区里 */
    latch_lock();
    inst->nconns--;

    if (inst->nconns <= 0) {
        catalog_close(inst->cat);
        blob_store_destroy(inst->blobs);
        graph_destroy(inst->graph);
        /*
         * 向量：断电时**不**落盘索引 ——
         * 索引本来就是 checkpoint 式的，脏页全丢了它就过期，
         * 下次开库自动重建。这正是"索引可以算出来"的价值。
         */
        if (inst->vstate) {
            VectorState *st = inst->vstate;
            if (st->hnsw)
                hnsw_destroy(st->hnsw);
            vindex_destroy(st->vindex);
            vstore_destroy(st->vs);
            free(st);
            inst->vstate = NULL;
        }
        spi_registry_destroy(inst->reg);
        pager_abandon(inst->pager);     /* 脏页全丢 —— 这就是"断电" */
        wal_close(inst->wal);            /* 日志已经 fsync 过了，关掉即可 */
        free(inst->walpath);
        free(inst);
        free(db);
    } else {
        free(db);
    }
    latch_unlock();
}

int db_result_rows(const Result *r)
{
    return (r && r->qr) ? r->qr->nrows : 0;
}

int db_result_cols(const Result *r)
{
    return (r && r->qr) ? r->qr->ncols : 0;
}

int db_result_affected(const Result *r)
{
    return (r && r->qr) ? r->qr->affected : 0;
}

const char *db_result_colname(const Result *r, int col)
{
    if (!r || !r->qr || col < 0 || col >= r->qr->ncols)
        return "";
    return r->qr->colnames[col];
}

const char *db_result_value(const Result *r, int row, int col)
{
    if (!r || !r->qr || row < 0 || row >= r->qr->nrows)
        return "";
    if (col < 0 || col >= r->qr->ncols)
        return "";
    return r->qr->cells[row * r->qr->ncols + col];
}

void db_result_free(Result *r)
{
    if (!r)
        return;
    qr_free(r->qr);
    free(r);
}

const char *db_strerror(db_err e)
{
    switch (e) {
    case DB_OK:         return "OK";
    case DB_E_IO:       return "IO 错误";
    case DB_E_NOSPACE:  return "空间不足";
    case DB_E_CORRUPT:  return "数据损坏";
    case DB_E_PARSE:    return "语法错误";
    case DB_E_TXN:      return "事务错误";
    case DB_E_NOTFOUND: return "对象不存在";
    case DB_E_NOTSUP:   return "操作不支持";
    case DB_E_INVAL:    return "参数非法";
    default:            return "未知错误";
    }
}

const char *db_last_error(DB *db)
{
    return db ? db->errmsg : "";
}
