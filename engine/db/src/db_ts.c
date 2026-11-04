/*
 * db_ts.c —— 时序模态的公开 API 与实例接线（v1.5 步骤 5）
 *
 * 这一层负责：把模态状态（TsStore：分区目录 + 数据页）挂进实例、
 * 把两张虚拟表接进注册表、全部入口加锁（硬规则 8）、
 * 把内部错误码翻译成公共错误码（map_rc）、提供 TTL 这条回收路径。
 *
 * ==================== 为什么要"写走专用 API、读走 SQL" ====================
 *
 *   写：db_ts_insert     —— 要**按时间戳路由到分区**，这条逻辑藏在 AM 内部；
 *                          SQL 的 INSERT 走执行器的通用写路径，
 *                          要在那里补一份路由就得把模态的知识漏进执行器。
 *   读：ts_readings / ts_partitions —— 纯视图，走 SPI 最自然。
 *
 * 与 blob / graph / vector / geo 模态同一个口径。
 *
 * ==================== 与地理模态的差别 ====================
 *
 * geo 的写**没有事务边界**（xid 固定 0），时序也一样：
 * 读数写进去就一直在，唯一的删除路径是 TTL 的整分区回收。
 *
 * 【WAL：v1.5 步骤 6（T10）已接入】
 *
 * 分区页 / 列式页 / 目录页 / meta 页现在都进日志（整页镜像，见 ts_wal.h）：
 * 本层把 db_wal_hook 注入模态，模态每个改页的写点都"先写日志再改页"，
 * 崩溃后由 WAL 重放把页补回来。
 *
 * ⚠️ 但时序的写**没有事务边界**：日志只用于 redo，**不用于 undo**
 * （xid 固定 0 = 系统事务，恢复时永不回滚）。
 * 将来要把时序的写纳入用户事务，必须同时补真正的 undo。
 *
 * ⭐ 时序比地理多一条好处：**读数不可变**（计划 §1.1）⇒ undo 永远用不上。
 *
 * ⚠️ 坑 M5-16 的同款事实：时序的读数**只存在分区页里**（没有第二份行存），
 * 所以 WAL 是**唯一**的恢复路径 —— 没有 WAL ⇒ 崩了就是这批读数没了。
 *
 * ==================== TTL 的接口依赖 ====================
 *
 * TTL 的**实现在步骤 4（ts_ttl.c）**，本层只负责：
 *   - 取"当前时间"（可复现的测试要求 ⇒ 由这里传进去）；
 *   - 把存储**自己的**分区目录交给它。
 *
 * ⚠️ **必须用存储自己的目录**（坑 M6-57）：
 * 另开一份目录副本（ts_dir_open 扫出来的）去跑 TTL ⇒
 * TTL 改了"那份"目录，而存储内存里的目录还是老的 ⇒
 * 存储仍以为那个分区是活的 ⇒ 查询会去读**已经被回收的页**
 * ⇒ 读到别人的数据，而且不报错。
 *
 * 目录的取法走对外契约里的 `ts_store_dir()`（tsstore.h）
 * —— 早期版本这里直接 include 了模态的私有头 ts_store_int.h，
 * 那是"越过契约"，现已消掉。
 */

#include "db_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/latch.h"
#include "modalities/ts/ts_am.h"
#include "modalities/ts/ts_am_ext.h"
#include "modalities/ts/ts_dir.h"       /* TsDir 的类型（TTL 的接口要用） */
#include "modalities/ts/ts_ttl.h"
#include "modalities/ts/ts_wal.h"
#include "modalities/ts/tsstore.h"

/* 分区窗口的默认值：1 天（与 tsstore.h 的说法一致） */
#define TS_DEFAULT_SPAN_SEC 86400u

/* ---------------- 小工具 ---------------- */

/*
 * 时序模态的写日志上下文（v1.5 步骤 6 · T10）。
 *
 * 模态只认 L0 的 LogHookFn，不碰 wal/（分层：模态不能依赖 L1）。
 * 钩子的实现是 db_wal_hook（src/db.c），它把记录翻译成 WAL 记录。
 *
 * ctx 传 DbInstance *（与 geo / blob / catalog 同一个口径）：
 * WAL 是实例级共享的，日志钩子的生命周期必须跟实例一致。
 */
static void dt_wal(TsWal *w, DbInstance *inst)
{
    w->fn = db_wal_hook;
    w->ctx = inst;
}

static void dt_err(DB *db, const char *msg)
{
    if (db) {
        snprintf(db->errmsg, sizeof(db->errmsg), "%s", msg);
    }
}

static DbInstance *dt_inst(DB *db)
{
    return db ? db->inst : NULL;
}

/*
 * 现在的毫秒时间戳。
 *
 * ⭐ **为什么要自己取，而不是让 TTL 层取**：
 * 时间藏在函数里，测试就没法复现 ——
 * 只能"造一批刚好在几分钟前的数据"这种脆弱写法，而且机器一慢就翻车。
 * TTL 层接受 now_ms 参数，这里把真实的当前时间传进去 ✓
 * （与 ts_ttl.h 的约定一致）。
 */
static i64 dt_now_ms(void)
{
    return (i64)time(NULL) * 1000;
}

/* ---------------- 注册表接入 ---------------- */

/*
 * 把两张虚拟表接进注册表。
 *
 * 和 blob / graph / vec / geo 同款：注册表只知道"名字 → 一个 StorageAM"，
 * 它不知道时序是什么。加第 6 个模态不需要改注册表，更不需要改执行器。
 */
int db_ts_register(DbInstance *inst)
{
    int rc;

    if (!inst || !inst->ts) {
        return E_INVAL;
    }

    rc = ts_am_build_readings(inst->ts, &inst->ts_am_readings,
                              &inst->ts_ext_readings.base);
    if (rc != OK) {
        return rc;
    }
    rc = spi_registry_add(inst->reg, "ts_readings", &inst->ts_am_readings);
    if (rc != OK) {
        return rc;
    }

    rc = ts_am_build_partitions(inst->ts, &inst->ts_am_partitions,
                                &inst->ts_ext_partitions.base);
    if (rc != OK) {
        return rc;
    }
    return spi_registry_add(inst->reg, "ts_partitions", &inst->ts_am_partitions);
}

/* ---------------- 建 / 开 / 关 ---------------- */

/*
 * 开库时接管磁盘上已有的时序存储。
 *
 * 磁盘上没有 → E_NOTFOUND（**不是错误**：那是个还没建过时序存储的库，
 * 和图模态、地理模态同一个口径），由调用方决定算不算错。
 */
int db_ts_open(DbInstance *inst)
{
    TsStore *st = NULL;
    TsWal w;
    int rc;

    if (!inst) {
        return E_INVAL;
    }

    /* 注入写日志钩子（v1.5 步骤 6 · T10）—— 理由见 dt_wal 的说明 */
    dt_wal(&w, inst);

    rc = ts_store_open_wal(inst->pager, &w, &st);
    if (rc != OK) {
        inst->ts = NULL;
        inst->ts_ready = false;
        return rc;              /* E_NOTFOUND = 还没建过 */
    }

    inst->ts = st;
    inst->ts_ready = true;

    rc = db_ts_register(inst);
    if (rc != OK) {
        ts_store_destroy(st);
        inst->ts = NULL;
        inst->ts_ready = false;
        return rc;
    }
    return OK;
}

/*
 * 关库：释放模态状态。
 *
 * 【幂等】inst->ts == NULL 时直接返回 —— 开库的失败清理路径有好多条，
 * 重复调用必须安全。ts 的页是**普通脏页**，pager_close 会刷盘，
 * 数据不依赖"先释放还是先关 pager"这个顺序。
 */
void db_ts_close(DbInstance *inst)
{
    if (!inst || !inst->ts) {
        return;
    }

    ts_store_destroy(inst->ts);
    inst->ts = NULL;
    inst->ts_ready = false;
}

/* ---------------- 公开 API ---------------- */

/*
 * 建时序存储（一个库只能建一次）。
 *
 * 第二次建 → DB_E_INVAL（与向量 / 地理模态同一个口径：
 * "没有存储"与"存储是空的"要能被区分开，所以不允许建第二次）。
 */
db_err db_ts_create(DB *db)
{
    DbInstance *inst;
    TsStore *st = NULL;
    TsWal w;
    int rc;

    if (!db) {
        return DB_E_INVAL;
    }
    inst = dt_inst(db);

    latch_lock();

    if (inst->ts_ready) {
        latch_unlock();
        dt_err(db, "时序存储已经建过了");
        return DB_E_INVAL;
    }

    /* 注入写日志钩子（v1.5 步骤 6 · T10）——
     * 目录的 meta 页是建库时写的 ⇒ 上下文必须在建库之前挂上。 */
    dt_wal(&w, inst);

    rc = ts_store_create_wal(inst->pager, TS_DEFAULT_SPAN_SEC, &w, &st);
    if (rc == OK) {
        inst->ts = st;
        inst->ts_ready = true;

        rc = db_ts_register(inst);
        if (rc != OK) {
            /* 注册失败要回滚：不能留下"半建"的状态 */
            ts_store_destroy(st);
            inst->ts = NULL;
            inst->ts_ready = false;
        }
    }

    latch_unlock();

    if (rc != OK) {
        dt_err(db, "建时序存储失败");
        return map_rc(rc);
    }
    return DB_OK;
}

/* 这个库有没有时序存储 */
int db_ts_exists(DB *db)
{
    DbInstance *inst = dt_inst(db);

    if (!inst) {
        return 0;
    }

    latch_lock();
    int r = inst->ts_ready ? 1 : 0;
    latch_unlock();
    return r;
}

/*
 * 追加一条读数。
 *
 * 【按时间戳路由到分区】：分区不存在就建一个（存储层的事）。
 *
 * ⚠️ **落进已封窗的分区 ⇒ DB_E_NOTSUP**（坑 M6-56）：
 * 封窗是**不可逆**的 —— 那批数据已经按列压缩好了，
 * 再往里追加会破坏列存的不变式。
 * 而"悄悄换个分区写"更糟：查那个时间窗会查到**另一个分区**，
 * 于是数据看起来"丢了"，而且不报错。
 * 所以这里必须**明确报错**。
 *
 * 【时间戳不用做有效性检查】
 *
 * i64 总是有限的（没有 NaN / Inf）；value 是 double，
 * NaN 是**允许**的 —— 传感器读数就是会这样（见 db.h 的说明）。
 */
db_err db_ts_insert(DB *db, const db_ts_reading *r)
{
    DbInstance *inst;
    TsReading tr;
    int rc;

    if (!db || !r) {
        return DB_E_INVAL;
    }
    inst = dt_inst(db);
    if (!inst->ts_ready) {
        dt_err(db, "时序存储还不存在");
        return DB_E_NOTFOUND;
    }

    tr.time = r->time;
    tr.tag = r->tag;
    tr.value = r->value;

    latch_lock();
    rc = ts_store_insert(inst->ts, &tr);
    /*
     * 日志落盘（WAL 规则：日志先落盘，脏页才能落盘）。
     *
     * 时序的写**不在用户事务里**（没有事务边界），所以每次写完
     * 就把日志刷到盘上 —— 与地理模态 db_geo_insert 同一个口径。
     * 不这么做的话，断电时日志还在缓冲区里（页也没落盘）⇒ 这批读数没了。
     */
    if (rc == OK)
        rc = wal_fsync(inst->wal);
    latch_unlock();   /* 成对释放（硬规则 8，坑 M6-60：忘了 unlock 就把整个库卡死） */

    if (rc != OK) {
        if (rc == E_NOTSUP) {
            dt_err(db, "这个时间戳落在已封窗的分区里，不能再写");
        } else {
            dt_err(db, "写入读数失败");
        }
        return map_rc(rc);
    }
    return DB_OK;
}

/*
 * TTL：把结束时间早于"当前时间 − 保留秒数"的分区整块回收。
 *
 * 幂等：已经回收过的分区不会被回收第二次。
 * retention_sec = 0 ⇒ 保留 0 秒 ⇒ 除"正在写"的那个分区外全回收。
 *
 * 【实现在步骤 4 的 ts_ttl.c】本层只做三件事：
 *   - 确认存储存在；
 *   - 取当前时间；
 *   - 把存储**自己的**目录交给它（坑 M6-57）。
 */
db_err db_ts_retention(DB *db, uint32_t retention_sec)
{
    DbInstance *inst;
    TsTtlStats stats;
    i64 now_ms;
    int rc;

    if (!db) {
        return DB_E_INVAL;
    }
    inst = dt_inst(db);
    if (!inst->ts_ready) {
        dt_err(db, "时序存储还不存在");
        return DB_E_NOTFOUND;
    }

    now_ms = dt_now_ms();
    memset(&stats, 0, sizeof(stats));

    latch_lock();
    rc = ts_ttl_expire(inst->pager, (TsDir *)ts_store_dir(inst->ts), now_ms,
                       (u64)retention_sec, &stats);
    /*
     * 日志落盘（理由同 db_ts_insert）。
     *
     * TTL 的第一步（目录项改成 DROPPED）已经进了 WAL，
     * 这里把日志刷到盘上 ⇒ "这个分区已过期"这件事落盘了，
     * 才轮到页被回收（坑 M6-49）。
     */
    if (rc == OK)
        rc = wal_fsync(inst->wal);
    latch_unlock();

    if (rc != OK) {
        dt_err(db, "TTL 回收失败");
        return map_rc(rc);
    }
    return DB_OK;
}

/*
 * 读数总数。
 *
 * ⚠️ **不能直接用存储的总数**（坑 M6-58）：
 * TTL 回收分区时不会去改存储内存里的"总行数"（那是步骤 2 的运行时状态，
 * 步骤 4 的 TTL 只动目录与页）⇒ 直接用那个数会把**已经回收掉的读数**
 * 也算进来。
 *
 * 所以这里按分区求和，并且**跳过 DROPPED 的分区** ——
 * 被回收的分区在目录里还留着历史（时间窗、状态、行数），
 * 那是"曾经有过多少"的记录，不是"现在还有多少"。
 */
db_err db_ts_count(DB *db, uint64_t *n)
{
    DbInstance *inst;
    TsStore *st;
    uint64_t total = 0;
    u32 np;
    u32 i;
    int rc = OK;

    if (!db || !n) {
        return DB_E_INVAL;
    }
    inst = dt_inst(db);
    if (!inst->ts_ready) {
        dt_err(db, "时序存储还不存在");
        return DB_E_NOTFOUND;
    }
    st = inst->ts;

    latch_lock();
    np = ts_store_part_count(st);
    for (i = 0u; i < np; i++) {
        TsPartInfo info;

        rc = ts_store_part_info(st, i, &info);
        if (rc != OK) {
            break;
        }
        if (info.state == TS_PART_DROPPED) {
            continue;           /* 已回收：不计入现在的读数 */
        }
        total += (uint64_t)info.n_rows;
    }
    latch_unlock();

    if (rc != OK) {
        dt_err(db, "统计读数失败");
        return map_rc(rc);
    }
    *n = total;
    return DB_OK;
}

/* 分区个数（含已回收的 —— 目录里留着历史） */
db_err db_ts_part_count(DB *db, uint32_t *n)
{
    DbInstance *inst;
    u32 np;

    if (!db || !n) {
        return DB_E_INVAL;
    }
    inst = dt_inst(db);
    if (!inst->ts_ready) {
        dt_err(db, "时序存储还不存在");
        return DB_E_NOTFOUND;
    }

    latch_lock();
    np = ts_store_part_count(inst->ts);
    latch_unlock();

    *n = np;
    return DB_OK;
}

/*
 * 诊断：累计读了多少页。口径与 db_geo_page_reads / db_vec_page_reads 一致：
 * **逻辑页读**（pager_get 的次数，含缓冲池命中），不是磁盘 IO。
 */
uint64_t db_ts_page_reads(DB *db)
{
    DbInstance *inst = dt_inst(db);

    if (!inst || !inst->ts_ready) {
        return 0;
    }
    return ts_store_page_reads(inst->ts);
}
