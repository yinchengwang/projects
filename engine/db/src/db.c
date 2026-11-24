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

/* 表打开器：执行器只认契约，两个实现分别属于关系模态与 SPI 注册表 */
#include "modalities/relational/rel_opener.h"
#include "spi/table_opener.h"

/* db_json_init：注册文档模态的 json_extract / json_match 与 JSONB 渲染器 */
#include "modalities/doc/json_func.h"

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
/*
 * db_open 失败路径的唯一清理点。
 *
 * ==================== 为什么所有失败路径共用这一个 ====================
 *
 * 原来这里散着 **10 份**手写的清理块，每份按"初始化到哪一步"列出该释放的
 * 东西。问题是：加一个模态就得多改 10 处，而且**顺序**（先 pager 后 wal）
 * 是靠人记住的 —— 坑 M4-48 的教训正是这条顺序写反会 use-after-free
 * （pager_close 刷脏页会触发 WAL 钩子，而 wal 已被 free）。
 *
 * 现在所有析构函数都是 NULL 安全的：
 *   db_ft_close / db_geo_close / db_ts_close  检查 inst->xxx
 *   graph_destroy / spi_registry_destroy / blob_store_destroy / catalog_close / wal_close
 *                                             全部 if (!p) return
 *   txn_clog_close                            检查 t->clog_fp
 *   free(NULL)                                合法
 * 所以"提前释放未初始化的东西"是安全的，一个函数就能覆盖全部阶段。
 *
 * 顺序（RULE，不许改）：
 *   1. 模态（它们持有 pager 的页引用）
 *   2. catalog（btree 持有 pager 引用）
 *   3. txn_clog（clog 文件）
 *   4. **pager 在 wal 之前**（pager_close 刷脏页 → WAL 钩子 → wal_fsync）
 *   5. 最后 free 路径与结构体
 */
static void open_fail_cleanup(DbInstance *inst, DB *db, Pager *pg)
{
#if DB_HAS_FT
    db_ft_close(inst);
#endif
#if DB_HAS_GEO
    db_geo_close(inst);
#endif
#if DB_HAS_TS
    db_ts_close(inst);
#endif
#if DB_HAS_GRAPH
    graph_destroy(inst->graph);
#endif
    spi_registry_destroy(inst->reg);
#if DB_HAS_BLOB
    blob_store_destroy(inst->blobs);
#endif
    catalog_close(inst->cat);
    txn_clog_close(&inst->txns);

    pager_close(pg);        /* 必须在 wal_close 之前，见上 */
    wal_close(inst->wal);

    free(inst->clogpath);
    free(inst->walpath);
    free(inst);
    free(db);
}

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

    /*
     * 【失败清理路径的顺序铁律（坑 M4-48）】
     *
     * 下面每一条失败路径都必须 **先 pager_close、再 wal_close**。
     *
     * 反过来会踩 use-after-free：
     *   pager_close → 刷脏页 → 触发 WAL 的刷页钩子 wal_flush_before_page
     *              → wal_fsync(inst->wal)
     * 而 wal_close 已经把 Wal 结构体 free 掉、互斥量 destroy 了
     * → 在**已销毁**的互斥量上 fflush
     * → 表现随机：有时段错误，有时**挂死**在 `RtlEnterCriticalSection`。
     *
     * 为什么以前没炸：正常关闭路径（db_instance.c）本来就是 pager → wal，
     * 写反的只有失败路径；而 db_open 长期几乎不会失败 → 这颗雷一直埋着。
     * 是"开库必失败"把它引爆的 —— 失败路径一被走到，全套 db 层测试一起崩。
     *
     * 【教训】异常路径的代码没人跑，就更要把顺序写成**惯例**而不是"每次想一下"。
     */
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

    /*
     * 恢复阶段：开 WAL → 陈旧日志处置 → clog 初始化 → 从日志重建 clog
     * → 读 checkpoint_lsn → recovery_run。
     *
     * 这一段自成一体，已抽到 db_recover.c（见该文件的顺序铁律说明）。
     * 失败时**不清理**任何资源 —— 由 open_fail_cleanup 按坑 M4-48 的
     * 顺序（先 pager_close、再 wal_close）统一处理。
     */
    bool did_recover = false;
    rc = db_recover_stage(path, datafile_exists, pg, inst, db, &did_recover);
    if (rc != OK) {
        open_fail_cleanup(inst, db, pg);
        return map_rc(rc);
    }

    /*
     * 重建全局空闲页栈（W-D 的 D-6.3）。
     *
     * 【为什么必须放在这里：重放之后、任何模态打开之前】
     *
     * 重放会把"已提交的页镜像"盖回页里 —— 那些页是**有主**的，不是空闲页。
     * 在重放之前扫 ⇒ 会把有主的页当空闲收进栈 ⇒ 下次分配发出去 ⇒
     * 两个结构写同一页且都不报错。
     *
     * 放任何模态打开之前：重建只依赖页头类型（PT_FREE），不依赖模态状态；
     * 放前面就不会出现"某个模态已经把页拿去用了、空闲栈里还留着它"这种窗口。
     *
     * 【失败不影响开库】
     *
     * 重建失败最多是"这次会话复用不了回收过的页"（退化成旧行为：只往文件尾追加）。
     * 那只是浪费空间，不是数据错误 ⇒ 不当致命错误。
     */
    if (pager_npages(pg) > 0)
        (void)pager_free_rebuild(pg, NULL);

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
#if DB_HAS_FT
    rc = db_ft_open(inst);
#endif
    if (rc != OK) {
        open_fail_cleanup(inst, db, pg);
        return map_rc(rc);
    }

    Catalog *cat = NULL;
    rc = catalog_open(pg, db_wal_hook, inst, &cat);
    if (rc != OK) {
        open_fail_cleanup(inst, db, pg);
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
#if DB_HAS_BLOB
    rc = blob_store_create(pg, db_wal_hook, inst, &inst->blobs);
#endif
    if (rc != OK) {
        open_fail_cleanup(inst, db, pg);
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
        open_fail_cleanup(inst, db, pg);
        return map_rc(rc);
    }

#if DB_HAS_BLOB
    rc = blob_am_build_frames(inst->blobs, &inst->blob_am_frames,
                              &inst->blob_ctx_frames);
    if (rc == OK)
        rc = blob_am_build_objects(inst->blobs, &inst->blob_am_objects,
                                   &inst->blob_ctx_objects);
    if (rc == OK)
        rc = spi_registry_add(inst->reg, "blob_frames", &inst->blob_am_frames);
    if (rc == OK)
        rc = spi_registry_add(inst->reg, "blob_objects", &inst->blob_am_objects);
#endif
    if (rc != OK) {
        open_fail_cleanup(inst, db, pg);
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
#if DB_HAS_GRAPH
    rc = graph_open(pg, db_wal_hook, inst, &inst->graph);
    if (rc == OK) {
        inst->graph_ready = true;
        rc = db_graph_register(inst);
    } else if (rc == E_NOTFOUND) {
        inst->graph = NULL;
        inst->graph_ready = false;
        rc = OK;
    }
#endif
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
#if DB_HAS_VECTOR
    if (rc == OK) {
        int vrc = db_vec_open(inst);
        if (vrc == E_NOTFOUND) {
            vrc = OK;       /* 这个库还没建向量存储 —— 不是错误 */
        }
        rc = vrc;
    }

#endif
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
#if DB_HAS_FT
    if (rc == OK) {
        rc = db_ft_register(inst);
    }
#endif

    /*
     * 地理空间模态（v1.4）。
     *
     * 和 blob / graph / vector 一样是**实例级共享**：所有连接看同一批点。
     *
     * 【打开时可能根本没有几何存储】
     *
     * 磁盘上没建过 → db_geo_open 返回 E_NOTFOUND。
     * 这不是错误（是个还没建过几何存储的库），所以 inst->geo 留空，
     * geo_points 这张虚拟表也不注册 ——
     * 查它会得到"表不存在"，而不是"查出一张空表"。
     *
     * 【注册在 db_geo_open 里面做】
     *
     * 与 db_vec_open 同一个套路：打开成功就意味着"磁盘上有几何存储"，
     * 那 geo_points 就该存在。建（db_geo_create）那次也注册一次 ——
     * 两条路互斥（没建过才建，建过就不许再建），不会重名。
     */
#if DB_HAS_GEO
    if (rc == OK) {
        int grc = db_geo_open(inst);
        if (grc == E_NOTFOUND) {
            grc = OK;       /* 这个库还没建几何存储 —— 不是错误 */
        }
        rc = grc;
    }

#endif
    /*
     * 时序模态（v1.5）。
     *
     * 和 blob / graph / vector / geo 一样是**实例级共享**：所有连接看同一批读数。
     *
     * 【打开时可能根本没有时序存储】
     * 磁盘上没建过 → db_ts_open 返回 E_NOTFOUND。
     * 这不是错误（是个还没建过时序存储的库），所以 inst->ts 留空，
     * ts_readings / ts_partitions 这两张虚拟表也不注册 ——
     * 查它们会得到"表不存在"，而不是"查出一张空表"。
     *
     * 【注册在 db_ts_open 里面做】
     * 与 db_geo_open 同一个套路：打开成功就意味着"磁盘上有时序存储"，
     * 那两张虚拟表就该存在。建（db_ts_create）那次也注册一次 ——
     * 两条路互斥（没建过才建，建过就不许再建），不会重名。
     */
#if DB_HAS_TS
    if (rc == OK) {
        int trc = db_ts_open(inst);
        if (trc == E_NOTFOUND) {
            trc = OK;       /* 这个库还没建时序存储 —— 不是错误 */
        }
        rc = trc;
    }

#endif
    if (rc != OK) {
        open_fail_cleanup(inst, db, pg);
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

    /*
     * 安装表打开器 —— 执行器只按顺序问这两个，谁先答上就用谁。
     *
     * ⚠️ `db_connect()` 另建连接时**也要装一遍**（见那处的注释），
     * 否则第二个连接起两个 opener 都是 NULL ⇒ 所有表都报「表不存在」。
     */
    db->db.opener_catalog = &rel_table_opener;
    db->db.opener_virtual = &spi_table_opener;

    /*
     * 文档模态（寄生模态：无存储、无"打开"）：注册两个标量函数 + 渲染器。
     *
     * ⚠️ 守卫是必须的 —— 计划 C5.14 写"无条件调用"的前提是 `DB_HAS_DOC`
     * 还不存在，但 W-B P1 已落地：`MM_FILES_doc` 关闭时会把 doc 源文件
     * 整个裁掉 ⇒ 无条件调用就是链接失败（实测于 MM=relational）。
     */
#if DB_HAS_DOC
    /*
     * 页链的建链/还链要先注册：它们需要 DbInstance（写 WAL），
     * 而 db_json_init 只负责纯模态内的注册（渲染器/编码器/函数表）。
     */
    (void)db_json_ovf_init(inst);
    db_json_init();
#endif

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

    /*
     * ⚠️ 两个连接入口都要装 opener。
     * 只装 db_open 会让 db_connect 出来的连接两个 opener 都是 NULL
     * ⇒ **第二个连接起所有表都报「表不存在」**，而且现象很像"权限问题"。
     */
    conn->db.opener_catalog = &rel_table_opener;
    conn->db.opener_virtual = &spi_table_opener;

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
