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

/*
 * 缓冲池的默认页数。
 *
 * ==================== 为什么不是 256 ====================
 *
 * 一页 4 KB，256 页 = **1 MB**。这个池装不下一页 4 MB 的表（≈1064 页），
 * 于是每次扫表都要把整张表从磁盘读一遍 —— 量出来的是磁盘，不是执行器。
 *
 * 实测（10 万行、选择性 10%）：
 *     1 MB 池   扫描阶段 4.0 – 5.0 ms   （绝大部分时间在 fread）
 *     16 MB 池  扫描阶段 0.60 – 0.72 ms （页都在内存里）
 * 差 7 倍。pager 用 setvbuf(_IONBF) + 每页一次 fread 系统调用，
 * 池小的时候这个开销直接盖过执行器本身。
 *
 * 16 MB（4096 页）是嵌入式数据库的合理起点，
 * 还能用 DB_BP_FRAMES 环境变量调 —— 见下面 bp_frames()。
 */
#define BP_FRAMES_DEFAULT 4096u

/*
 * 缓冲池页数：默认值 + 环境变量覆盖。
 *
 * ==================== 为什么给一个环境变量 ====================
 *
 * 没有配置文件机制（db_open 只有 path 一个参数），
 * 而"池多大"在不同场景下差很多：
 *   - 小嵌入式设备：1 MB 就够
 *   - 基准测试：必须装得下数据集，否则量的是磁盘
 *
 * 环境变量是唯一不破坏现有 API 的办法：
 * 默认行为不变，显式设置了才生效。
 */
static u32 bp_frames(void)
{
    const char *e = getenv("DB_BP_FRAMES");
    if (e && e[0]) {
        long v = strtol(e, NULL, 10);
        /* 下限 16 页（连一页都放不下就没意义了），上限 100 万页（4 GB） */
        if (v >= 16 && v <= 1000000L)
            return (u32)v;
    }
    return BP_FRAMES_DEFAULT;
}

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
    int rc = pager_open(path, bp_frames(), &pg);
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
     * 初始化事务表。
     *
     * 必须在 clog_open 之前 —— txn_table_init 会把分块 clog 的块指针表清空，
     * 而 clog_open 要往里填文件的内容。
     */
    txn_table_init(&inst->txns, max_xid);

    /*
     * clog 文件（v1.2 步骤 6）：<db>-clog
     *
     * 【为什么需要它】
     * "哪些事务提交过"这条信息必须有个**不在日志里**的存放处 ——
     * 因为步骤 6 的 checkpoint 会截断日志，
     * 提交记录被截掉之后，可见性判断就只剩这个文件可依。
     *
     * 【陈旧 clog 的处置，和陈旧日志一样】
     * 数据文件不存在（全新库）→ 同名 clog 一定是上一个同名库的孤儿 → 删掉。
     * 反过来（数据文件存在但 clog 不存在）→ 那是"老版本建的库"，
     * 从日志重建就行（下面就是这么做的）。
     */
    inst->clogpath = malloc(plen + 10);
    if (!inst->clogpath) {
        txn_clog_close(&inst->txns);
        wal_close(inst->wal);
        pager_close(pg);
        free(inst->clogpath);
        free(inst->walpath);
        free(inst);
        free(db);
        return DB_E_IO;
    }
    snprintf(inst->clogpath, plen + 10, "%s-clog", path);

    if (!datafile_exists)
        remove(inst->clogpath);       /* 孤儿 clog：删掉（不存在也不报错）*/

    rc = txn_clog_open(&inst->txns, inst->clogpath);
    if (rc != OK) {
        wal_close(inst->wal);
        pager_close(pg);
        free(inst->clogpath);
        free(inst->walpath);
        free(inst);
        free(db);
        return map_rc(rc);
    }

    /*
     * xid 还要和 clog 文件里的最大 xid 取大。
     *
     * 日志被截断之后，日志里的最大 xid 会变小 ——
     * 真正"用过的最大 xid"记在 clog 文件里。
     * 不取大的话，新会话会重用旧 xid → 旧事务的提交状态被覆盖
     * → 可见性判断错乱。
     */
    txn_table_bump_xid(&inst->txns, txn_clog_max_xid(&inst->txns));

    /*
     * 从日志重建 clog（补上 clog 文件里没有的部分）。
     *
     * 为什么必须重建：MVCC 可见性判断完全依赖 clog —— "这个 xid 提交了没有"。
     * 如果重启后 clog 是空的，所有历史事务都会被当成"进行中"，
     * 于是重启后**所有已提交的数据都看不见了**。
     *
     * 两份信息的分工：
     *   clog 文件 —— checkpoint 之前的历史（提交记录可能已被截断）
     *   日志      —— checkpoint 之后的事务（提交记录还在日志里）
     */
    RecoveredState xs;
    recovered_state_init(&xs);
    rc = recovery_scan_xids(inst->wal, &xs);
    if (rc != OK) {
        recovered_state_free(&xs);
        txn_clog_close(&inst->txns);
        wal_close(inst->wal);
        pager_close(pg);
        free(inst->clogpath);
        free(inst->walpath);
        free(inst);
        free(db);
        return map_rc(rc);
    }
    for (u32 i = 0; i < xs.cap; i++) {
        if (xs.slots[i].xid == 0)
            continue;               /* 空槽 */
        u32 xid = xs.slots[i].xid;
        switch (xs.slots[i].state) {
        case REC_XID_COMMITTED:
            txn_clog_set(&inst->txns, xid, TXN_CLOG_COMMITTED);
            break;
        case REC_XID_ABORTED:
            txn_clog_set(&inst->txns, xid, TXN_CLOG_ABORTED);
            break;
        case REC_XID_ABORT_DONE:
            /* 恢复阶段已经回滚完了 → 同样是"中止" */
            txn_clog_set(&inst->txns, xid, TXN_CLOG_ABORTED);
            break;
        default:
            /*
             * 未知：日志里有它的数据记录，但没看到提交/中止记录。
             *
             * 两种可能：
             *   (a) 它在崩溃时还没结束 → 该按"中止"处理（会被回滚）
             *   (b) 它的提交记录已经被 checkpoint 截掉了 → 它其实是已提交的
             *
             * 怎么区分：看 clog 文件。clog 文件是 checkpoint 时同步落盘的，
             * 提交状态在那儿是权威的 —— 文件说"已提交"就信它。
             */
            if (txn_clog_state(&inst->txns, xid) != TXN_CLOG_COMMITTED)
                txn_clog_set(&inst->txns, xid, TXN_CLOG_ABORTED);
            break;
        }
    }
    recovered_state_free(&xs);

    /*
     * 恢复起点：checkpoint_lsn（记在 meta 页里）。
     *
     * 必须在 catalog_open **之前**读 —— 那时 catalog 还没打开，
     * 而 meta 页的布局常量是公开的（catalog.h），可以直接读。
     *
     * 读到的值可能比日志里的实际情况旧（meta 页没来得及刷盘），
     * 那就多重放一遍 —— 无害。反过来（新值比日志新）不可能发生：
     * meta 页落盘之前它的日志记录必须先落盘（WAL 规则）。
     */
    u32 ckpt_lsn = 0;
    if (pager_npages(pg) > 0) {
        PageFrame *mf;
        if (pager_get(pg, PAGE_ID_META, &mf) == OK) {
            if (get_u32(mf->data + M_MAGIC) == CAT_MAGIC)
                ckpt_lsn = get_u32(mf->data + M_CKPT_LSN);
            pager_unpin(pg, PAGE_ID_META, false);
        }
    }

    /*
     * 恢复必须在打开 catalog 之前：catalog 也是普通表，也要恢复。
     *
     * 【全文检索的恢复钩子】（v1.3 步骤 5）
     *
     * WAL 层不认识 FT 语义（wal/ 是 L1，不许 include modalities/），
     * 所以恢复阶段遇到**非页类**记录（逻辑记录）时，
     * 由钩子把 (type, xid, payload) 转交给 db 层解释。
     *
     * 钩子是**进程级**的一份（recovery_set_hook 是全局状态），
     * 所以注册与注销必须配对：注册 → recovery_run → 注销。
     */
    bool did_recover = false;
    recovery_set_hook(db_ft_recovery_hook, inst);
    rc = recovery_run(pg, inst->wal, ckpt_lsn, &did_recover);
    recovery_set_hook(NULL, NULL);
    if (rc != OK) {
        db_ft_replay_free(&inst->ft_replay_redo);
        db_ft_replay_free(&inst->ft_replay_undo);
        txn_clog_close(&inst->txns);
        wal_close(inst->wal);
        pager_close(pg);
        free(inst->clogpath);
        free(inst->walpath);
        free(inst);
        free(db);
        return map_rc(rc);
    }

    /*
     * 全文检索模态：**先把索引从磁盘恢复出来**（v1.3 步骤 5）。
     *
     * 为什么放在这里（catalog 之前）：
     *   - 恢复重放刚跑完，WAL 里的 tombstone 记录已经收进了
     *     inst->ft_replay_* —— 现在正好用来装配删除标记集合；
     *   - 扫页空间找段只需要 pager，不依赖 catalog。
     *
     * ft_docs 虚拟表的注册是**另一步**（注册表在后面才建）。
     */
    rc = db_ft_open(inst);
    if (rc != OK) {
        txn_clog_close(&inst->txns);
        wal_close(inst->wal);
        pager_close(pg);
        free(inst->clogpath);
        free(inst->walpath);
        free(inst);
        free(db);
        return map_rc(rc);
    }

    Catalog *cat = NULL;
    rc = catalog_open(pg, db_wal_hook, inst, &cat);
    if (rc != OK) {
        db_ft_close(inst);
        txn_clog_close(&inst->txns);
        wal_close(inst->wal);
        pager_close(pg);
        free(inst->clogpath);
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
        db_ft_close(inst);
        txn_clog_close(&inst->txns);
        wal_close(inst->wal);
        pager_close(pg);
        free(inst->clogpath);
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
        db_ft_close(inst);
        txn_clog_close(&inst->txns);
        wal_close(inst->wal);
        pager_close(pg);
        free(inst->clogpath);
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
        db_ft_close(inst);
        txn_clog_close(&inst->txns);
        wal_close(inst->wal);
        pager_close(pg);
        free(inst->clogpath);
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

    /*
     * 全文检索模态（v1.3）：**注册 ft_docs 虚拟表**。
     *
     * 索引本体已经在恢复之后建好了（db_ft_open，见上），
     * 这一步只把它接进注册表 —— 和 blob / graph / vector 同一个套路：
     * 注册表只知道"名字 → 一个 StorageAM"，它不需要知道全文检索是什么。
     *
     * 磁盘上**没有** FT 数据（新库）→ 索引是空的 → ft_docs 是一张空表。
     * 这与 graph 的"没有图 → 查它得到表不存在"是两种不同的状态，
     * 如实区分开（见 db_ft_open 的注释）。
     */
    if (rc == OK) {
        rc = db_ft_register(inst);
    }

    if (rc != OK) {
        db_ft_close(inst);
        graph_destroy(inst->graph);
        spi_registry_destroy(inst->reg);
        blob_store_destroy(inst->blobs);
        catalog_close(cat);
        txn_clog_close(&inst->txns);
        wal_close(inst->wal);
        pager_close(pg);
        free(inst->clogpath);
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

/*
 * 关库 / 关连接的实现在 db_instance.c，
 * 结果集访问器在 db_result.c ——
 * 拆出去是为了守住"单文件不超过 800 行"这条硬规则。
 */
