/*
 * ts_am.c —— 时序模态的 SPI 适配：ts_readings / ts_partitions（v1.5 步骤 5）
 *
 * 契约见 ts_am.h（列定义、列号、构造函数签名都在那里）。
 *
 * ==================== 这一层在干什么 ====================
 *
 * 两张**虚拟表**：
 *
 *   ts_readings     一行 = 一条读数（time / tag / value）
 *   ts_partitions   一行 = 一个分区（起止时间 / 状态 / 行数 / 页数）
 *
 * 它们都不是"存出来的表"，而是**现算的视图**：
 * 读数在分区里（裸写页或列式页），分区在目录里，
 * 执行器要的是"一行一行的值" ⇒ 本层负责把物理形态摊平成行。
 *
 * 与 ft_docs / geo_points 同一个取舍：这是**元数据路径**，不是热路径，
 * 物化一次换掉"每行都去翻一遍物理结构"是对的。
 *
 * ==================== 文件为什么要拆 ====================
 *
 * 新文件的硬上限是 800 行。这个文件原本把"物化"也装下了 ⇒ 超限。
 * 所以物化那一块拆到 ts_am_mat.{h,c}：
 *
 *   ts_am_mat.c   物化（把物理形态摊平成行字节）+ 按分区裁剪
 *   ts_am.c       AM 本体（构造 / 扫描 / 解码 / 下推 / 可见性 / 写路径）
 *
 * 拆分的边界是**内聚**的：物化层只管"行从哪来"，
 * AM 层只管"行怎么交出去"。
 *
 * ==================== 行的字节格式 ====================
 *
 * ts_readings：固定 24 字节（小端），与活跃分区里的行格式**完全一致**：
 *
 *   偏移 0   i64   time（毫秒时间戳）
 *   偏移 8   i64   tag
 *   偏移 16  u64   value 的**位模式**（double）
 *
 * ts_partitions：固定 40 字节（小端）：
 *
 *   偏移 0   i64   start_ms（分区起始，含）
 *   偏移 8   i64   end_ms（分区结束，不含）
 *   偏移 16  i32   state（ACTIVE / SEALED / DROPPED）
 *   偏移 20  ——   填充（对齐到 8 字节）
 *   偏移 24  i64   n_rows
 *   偏移 32  i64   page_count
 *
 * 【为什么浮点存位模式】
 *
 * 磁盘格式只允许定宽无符号整数（common/types.h 铁律 1），
 * 而读数是 double —— 所以存的是 double 的**位模式**。
 * 互转走 memcpy，不用 union、**不用指针双关**。
 *
 * ==================== RID 是什么 ====================
 *
 * ts_readings / ts_partitions 都**没有物理行号**：
 * 行是现算出来的，看数的是分区里的物理位置。
 * 所以 RID 用**输出序号**定位（与 geo_points / ft_docs 同一口径）：
 * 同一批物化结果内稳定，跨物化不保证 —— 这是"视图表"的固有性质。
 *
 * ==================== 时间范围下推 ====================
 *
 * ⭐ 这是本模态最重要的性能杠杆：查一个时间窗 ⇒ 只读命中的分区。
 *
 * ⚠️ **裁剪只能少读，不能少给**（坑 M6-54）：
 * 谓词不可解析（OR / 函数包裹 / 没有谓词）⇒ **全部分区**。
 * "我猜这个谓词大概只涉及最近 3 个分区"就是静默漏数据。
 *
 * 实现上采取**分区级**裁剪（不是行级）：
 * 命中范围的分区**整块**给出去 —— 因为分区是查询的最小单位，
 * 而分区内部的时间是乱序的（迟到写入），按行裁剪反而容易写错边界。
 * 执行器拿到行之后**仍然完整求值一遍 WHERE** ⇒ 多给的行被过滤掉，
 * 少给的行就是 bug ⇒ 所以裁剪一律取**超集**。
 */

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/ts/ts_am.h"
#include "modalities/ts/ts_am_ext.h"
#include "modalities/ts/ts_am_mat.h"
#include "modalities/ts/tsstore.h"
#include "spi/pushdown.h"
#include "spi/storage_am.h"
#include "storage/record.h"
#include "storage/value.h"

/* ---------------- 小工具 ---------------- */

/* 位模式 → double（不用指针双关） */
static double ts_am_b2d(u64 bits)
{
    double d;

    memcpy(&d, &bits, sizeof(d));
    return d;
}

/*
 * 从契约里的上下文还原扩展结构（首成员技巧，见 ts_am_ext.h 的坑 M6-53）。
 */
static TsAmExt *ts_am_ext(TsAmCtx *c)
{
    return (TsAmExt *)c;
}

/* ---------------- 列定义 ---------------- */

/*
 * ts_readings：time（毫秒时间戳）/ tag（设备号、状态码）/ value（读数）。
 *
 * 三列都是**非空**的：一条读数缺时间或缺值就没有意义，
 * 与其让 NULL 混进来，不如在写入口就拒掉。
 */
static const ColDesc TS_READING_COLS[TS_READINGS_NCOLS] = {
    { "time",  T_INT8, false },
    { "tag",   T_INT8, false },
    { "value", T_REAL, false }
};

/*
 * ts_partitions：分区目录的一层视图。
 *
 * state 是 INT4 —— 枚举值（0/1/2），用整数列最省事，
 * 用户要看的语义（ACTIVE / SEALED / DROPPED）由文档解释。
 */
static const ColDesc TS_PART_COLS[TS_PARTITIONS_NCOLS] = {
    { "start_ms",   T_INT8, false },
    { "end_ms",     T_INT8, false },
    { "state",      T_INT4, false },
    { "n_rows",     T_INT8, false },
    { "page_count", T_INT8, false }
};

/* ---------------- 扫描状态 ---------------- */

typedef struct {
    u8 *rows;
    u32 nrows;
    u32 cur;
    u32 row_bytes;
} TsAmScan;

/* ---------------- schema ---------------- */

static int ts_am_columns(StorageAM *am, const ColDesc **cols, int *ncols)
{
    TsAmExt *ext = ts_am_ext((TsAmCtx *)am->impl);

    if (!ext || !cols || !ncols) {
        return E_INVAL;
    }
    if (ext->kind == TS_AM_KIND_PARTITIONS) {
        *cols = TS_PART_COLS;
        *ncols = TS_PARTITIONS_NCOLS;
    } else {
        *cols = TS_READING_COLS;
        *ncols = TS_READINGS_NCOLS;
    }
    return OK;
}

/* ---------------- 扫描 ---------------- */

/*
 * 开一个扫描（带裁剪范围）。
 *
 * scan_begin 走"上下文里的默认范围"，push 走"谓词算出来的范围" ——
 * 两者共用这一段。
 */
static int ts_am_open_scan(StorageAM *am, bool has_range, i64 lo, i64 hi, AmScan **out)
{
    TsAmCtx *c = (TsAmCtx *)am->impl;
    TsAmExt *ext;
    TsAmMat m;
    TsAmScan *s;
    int rc;

    if (!c || !c->st || !out) {
        return E_INVAL;
    }
    ext = ts_am_ext(c);

    memset(&m, 0, sizeof(m));
    if (ext->kind == TS_AM_KIND_PARTITIONS) {
        rc = ts_am_mat_partitions(c->st, &m);
    } else {
        rc = ts_am_mat_readings(c->st, has_range, lo, hi, &m);
    }
    if (rc != OK) {
        ts_am_mat_free(&m);
        return rc;
    }

    s = (TsAmScan *)calloc(1, sizeof(TsAmScan));
    if (s == NULL) {
        ts_am_mat_free(&m);
        return E_IO;
    }
    s->rows = m.rows;
    s->nrows = m.nrows;
    s->row_bytes = m.row_bytes;
    s->cur = 0u;
    *out = (AmScan *)s;
    return OK;
}

static int ts_am_scan_begin(StorageAM *am, AmScan **out)
{
    TsAmExt *ext = ts_am_ext((TsAmCtx *)am->impl);

    if (!ext) {
        return E_INVAL;
    }
    return ts_am_open_scan(am, ext->has_range, ext->lo_ms, ext->hi_ms, out);
}

static int ts_am_scan_next(StorageAM *am, AmScan *sc, Rid *rid,
                           u8 *buf, u16 cap, u16 *len)
{
    TsAmScan *s = (TsAmScan *)sc;

    (void)am;
    if (!s) {
        return E_INVAL;
    }
    if (cap < s->row_bytes) {
        return E_NOSPACE;
    }
    if (s->cur >= s->nrows) {
        return E_EOF;
    }
    memcpy(buf, s->rows + (size_t)s->cur * s->row_bytes, s->row_bytes);
    if (len) {
        *len = (u16)s->row_bytes;
    }
    if (rid) {
        /*
         * RID = (输出序号, 0)。
         *
         * 这两张表都没有物理行 —— 行是现算出来的，
         * 所以 RID 用输出序号定位（与 geo_points / ft_docs 同一口径）。
         */
        *rid = rid_make(s->cur, 0);
    }
    s->cur++;
    return OK;
}

static int ts_am_scan_end(StorageAM *am, AmScan *sc)
{
    TsAmScan *s = (TsAmScan *)sc;

    (void)am;
    if (!s) {
        return OK;
    }
    free(s->rows);
    free(s);
    return OK;
}

/* ---------------- 按 RID 取一行 ---------------- */

static int ts_am_get(StorageAM *am, Rid rid, u8 *buf, u16 cap, u16 *len)
{
    TsAmExt *ext = ts_am_ext((TsAmCtx *)am->impl);
    TsAmMat m;
    int rc;

    if (!ext || !ext->base.st) {
        return E_INVAL;
    }

    memset(&m, 0, sizeof(m));
    if (ext->kind == TS_AM_KIND_PARTITIONS) {
        rc = ts_am_mat_partitions(ext->base.st, &m);
    } else {
        rc = ts_am_mat_readings(ext->base.st, ext->has_range, ext->lo_ms, ext->hi_ms, &m);
    }
    if (rc != OK) {
        ts_am_mat_free(&m);
        return rc;
    }
    if (rid.page >= m.nrows) {
        ts_am_mat_free(&m);
        return E_NOTFOUND;
    }
    if (cap < m.row_bytes) {
        ts_am_mat_free(&m);
        return E_NOSPACE;
    }
    memcpy(buf, m.rows + (size_t)rid.page * m.row_bytes, m.row_bytes);
    if (len) {
        *len = (u16)m.row_bytes;
    }
    ts_am_mat_free(&m);
    return OK;
}

/* ---------------- 解码 ---------------- */

static int ts_am_decode(StorageAM *am, const u8 *buf, u16 len, DbValue *out)
{
    TsAmExt *ext = ts_am_ext((TsAmCtx *)am->impl);

    if (!ext || !buf || !out) {
        return E_INVAL;
    }

    if (ext->kind == TS_AM_KIND_PARTITIONS) {
        if (len < TS_PART_ROW_BYTES) {
            return E_CORRUPT;
        }
        val_int(&out[TS_COL_PART_START], (i64)get_u64(buf + TS_PROW_START_OFF));
        out[TS_COL_PART_START].type = T_INT8;

        val_int(&out[TS_COL_PART_END], (i64)get_u64(buf + TS_PROW_END_OFF));
        out[TS_COL_PART_END].type = T_INT8;

        val_int(&out[TS_COL_PART_STATE], (i64)get_u32(buf + TS_PROW_STATE_OFF));
        out[TS_COL_PART_STATE].type = T_INT4;

        val_int(&out[TS_COL_PART_ROWS], (i64)get_u64(buf + TS_PROW_ROWS_OFF));
        out[TS_COL_PART_ROWS].type = T_INT8;

        val_int(&out[TS_COL_PART_PAGES], (i64)get_u64(buf + TS_PROW_PAGES_OFF));
        out[TS_COL_PART_PAGES].type = T_INT8;
        return OK;
    }

    if (len < TS_READING_ROW_BYTES) {
        return E_CORRUPT;
    }
    val_int(&out[TS_COL_TIME], (i64)get_u64(buf + TS_ROW_TIME_OFF));
    out[TS_COL_TIME].type = T_INT8;

    val_int(&out[TS_COL_TAG], (i64)get_u64(buf + TS_ROW_TAG_OFF));
    out[TS_COL_TAG].type = T_INT8;

    val_real(&out[TS_COL_VALUE], ts_am_b2d(get_u64(buf + TS_ROW_VAL_OFF)));
    out[TS_COL_VALUE].type = T_REAL;
    return OK;
}

/* ---------------- 可见性 ---------------- */

/*
 * 时序的行**恒可见**。
 *
 * 理由与 blob / graph / vector / ft_docs / geo_points 一致：
 * 时序没有版本 —— 一条读数写进去就一直在（不可变），
 * 删除的唯一路径是 TTL 的**整分区**回收（那是在分区层面发生的）。
 */
static bool ts_am_visible(StorageAM *am, const u8 *buf, u16 len,
                          const AmVisCtx *vis)
{
    (void)am; (void)buf; (void)len; (void)vis;
    return true;
}

/* ---------------- 写路径 ---------------- */

/*
 * 插入一条读数。
 *
 * 【为什么 insert 走这里而不是专用 API】
 *
 * 契约（ts_am.h）明确：ts_readings 的 insert 是支持的 ——
 * 与 geo / vector / ft 模态不同（那三个只读）。
 * 理由：时序的写入就是"追加一条读数"，没有"更新索引"那层复杂度。
 *
 * 【xid 不用】
 *
 * 时序的写**不在用户事务里**（没有事务边界），
 * xid 传进来也不记 —— 与 geo 模态同一个口径。
 * 硬规则 5 要的是"xid 必须显式传"，不是"必须用"。
 *
 * ⚠️ **迟到写入会被拒**（坑 M6-56）：
 * ts_store_insert 遇到"时间戳落在已封窗的分区里"会返回 E_NOTSUP，
 * 执行器据此报"这个表不支持写"这一类的错 ——
 * 真正的原因要靠 db_ts_insert 的错误说明（"落进已封窗的分区"）。
 * 绝不能悄悄换个分区写 —— 那会静默改变查询结果。
 */
static int ts_am_insert(StorageAM *am, u32 xid, const DbValue *vals, Rid *out)
{
    TsAmCtx *c = (TsAmCtx *)am->impl;
    TsReading r;
    int rc;

    (void)xid;
    if (!c || !c->st || !vals) {
        return E_INVAL;
    }
    if (!c->writable) {
        return E_NOTSUP;
    }
    if (vals[TS_COL_TIME].is_null || vals[TS_COL_TAG].is_null ||
        vals[TS_COL_VALUE].is_null) {
        return E_INVAL;
    }

    r.time = vals[TS_COL_TIME].i;
    r.tag = vals[TS_COL_TAG].i;
    r.value = vals[TS_COL_VALUE].d;

    rc = ts_store_insert(c->st, &r);
    if (rc != OK) {
        return rc;
    }
    if (out) {
        /*
         * 没有物理行号 ⇒ 给一个**输出序号**当 RID。
         * 执行器不关心这个值（INSERT 的返回值只有 affected 行数），
         * 但不能给 PAGE_ID_NIL —— 那会被当成"无效行号"记下来。
         */
        *out = rid_make(ts_store_row_count(c->st), 0);
    }
    return OK;
}

/*
 * update / remove 一律 E_NOTSUP。
 *
 * 坑 M6-55：时序**不可变**是建模前提（计划 §1.1 明确不做更新 / 删除单行）。
 * 唯一的删除路径是 TTL 的整分区回收。
 * 允许"改一行"会让"按时间戳定位一条读数"这件事失去意义 ——
 * 而那正是时序查询的全部。
 */
static int ts_am_update(StorageAM *am, u32 xid, Rid rid,
                        const DbValue *vals, Rid *new_rid)
{
    (void)am; (void)xid; (void)rid; (void)vals; (void)new_rid;
    return E_NOTSUP;
}

static int ts_am_remove(StorageAM *am, u32 xid, Rid rid)
{
    (void)am; (void)xid; (void)rid;
    return E_NOTSUP;
}

static int ts_am_vacuum(StorageAM *am, TxnTable *txns, u32 xid,
                         AmVacuumStats *out)
{
    (void)am; (void)txns; (void)xid; (void)out;
    return E_NOTSUP;   /* 没有版本 → 没有可回收的东西 */
}

/* 建索引 / 重建索引：时序没有"索引"这一层（分区就是它的组织方式） */
static int ts_am_reindex(StorageAM *am, u32 index_id, u64 *rows_out)
{
    (void)am; (void)index_id; (void)rows_out;
    return E_NOTSUP;
}

/* ---------------- 谓词下推 ---------------- */

/*
 * 只有"时间列 vs 常量"这种谓词能下推。
 *
 * 其余一律 E_NOTSUP —— 执行器看到 E_NOTSUP 就退回全表扫描。
 *
 * ⚠️ **必须保守**（坑 M6-54）：
 * 认错了谓词 ⇒ 少给行 ⇒ 静默漏数据。
 * 所以这里只认四种算子（>= / > / <= / <）与整数常量，
 * 其余（OR、函数包裹、列比列、浮点比较）都不认。
 */
static int ts_am_can_push(StorageAM *am, const Predicate *p)
{
    TsAmExt *ext = ts_am_ext((TsAmCtx *)am->impl);

    if (!ext || !p) {
        return E_INVAL;
    }
    if (ext->kind != TS_AM_KIND_READINGS) {
        return E_NOTSUP;
    }
    if (p->col_idx != TS_COL_TIME) {
        return E_NOTSUP;
    }
    if (p->val.is_null) {
        return E_NOTSUP;
    }
    if (p->val.type != T_INT8 && p->val.type != T_INT4) {
        return E_NOTSUP;
    }
    if (p->op != PD_GE && p->op != PD_GT && p->op != PD_LE && p->op != PD_LT) {
        return E_NOTSUP;
    }
    return OK;
}

/*
 * 把谓词翻译成时间范围，然后按这个范围开扫描。
 *
 * 【为什么翻译成"范围"而不是"精确边界"】
 *
 * 执行器拿到行之后**仍然完整求值一遍 WHERE** ⇒ 下推只要给**超集**。
 * 所以 >= v 翻译成 [v, +∞)（而不是 [v, v]）——
 * 少给才是 bug，多给只是慢一点。
 */
static int ts_am_push(StorageAM *am, const Predicate *p, AmScan **out)
{
    i64 lo = INT64_MIN;
    i64 hi = INT64_MAX;

    if (ts_am_can_push(am, p) != OK) {
        return E_NOTSUP;
    }

    if (p->op == PD_GE || p->op == PD_GT) {
        lo = p->val.i;
    } else {
        hi = p->val.i;
    }
    return ts_am_open_scan(am, true, lo, hi, out);
}

/* ---------------- 构造 ---------------- */

int ts_am_build_readings(TsStore *st, StorageAM *out, TsAmCtx *ctx)
{
    TsAmExt *ext;

    if (!st || !out || !ctx) {
        return E_INVAL;
    }
    ext = ts_am_ext(ctx);

    ctx->st = st;
    ctx->writable = true;      /* 读数表允许 insert（见 ts_am.h） */

    ext->kind = TS_AM_KIND_READINGS;
    ext->has_range = false;
    ext->lo_ms = 0;
    ext->hi_ms = 0;

    out->name = "ts_readings";
    /*
     * 能力位：能扫 + 能插。
     *
     * 没有 UPDATE / REMOVE / VACUUM / INDEX ——
     * 时序不可变（坑 M6-55），也没有"版本"可回收。
     * 有 PUSHDOWN：时间范围谓词可以下推（这是本模态最大的性能杠杆）。
     */
    out->caps = AM_CAP_SCAN | AM_CAP_INSERT | AM_CAP_PUSHDOWN;

    out->columns = ts_am_columns;
    out->scan_begin = ts_am_scan_begin;
    out->scan_next = ts_am_scan_next;
    out->scan_end = ts_am_scan_end;
    out->get = ts_am_get;
    out->decode = ts_am_decode;
    /* 批能力：时序模态不支持（与 blob / vector / ft / geo 同款） */
    out->batch_scan = NULL;
    out->batch_decode = NULL;
    out->visible = ts_am_visible;
    out->insert = ts_am_insert;
    out->update = ts_am_update;
    out->remove = ts_am_remove;
    out->vacuum = ts_am_vacuum;
    out->reindex = ts_am_reindex;
    out->impl = ctx;
    out->can_push = ts_am_can_push;
    out->push = ts_am_push;

    return OK;
}

int ts_am_build_partitions(TsStore *st, StorageAM *out, TsAmCtx *ctx)
{
    TsAmExt *ext;

    if (!st || !out || !ctx) {
        return E_INVAL;
    }
    ext = ts_am_ext(ctx);

    ctx->st = st;
    ctx->writable = false;     /* 分区目录只读：增删由写路径与 TTL 决定 */

    ext->kind = TS_AM_KIND_PARTITIONS;
    ext->has_range = false;
    ext->lo_ms = 0;
    ext->hi_ms = 0;

    out->name = "ts_partitions";
    /* 只读：分区目录是存储自己的东西，用户改不了（要改走 TTL） */
    out->caps = AM_CAP_SCAN;

    out->columns = ts_am_columns;
    out->scan_begin = ts_am_scan_begin;
    out->scan_next = ts_am_scan_next;
    out->scan_end = ts_am_scan_end;
    out->get = ts_am_get;
    out->decode = ts_am_decode;
    out->batch_scan = NULL;
    out->batch_decode = NULL;
    out->visible = ts_am_visible;
    out->insert = ts_am_insert;
    out->update = ts_am_update;
    out->remove = ts_am_remove;
    out->vacuum = ts_am_vacuum;
    out->reindex = ts_am_reindex;
    out->impl = ctx;
    /* 分区表不下推：它的列是分区元数据，不是读数的时间 */
    out->can_push = NULL;
    out->push = NULL;

    return OK;
}

/* ---------------- 时间范围下推的入口 ---------------- */

/*
 * 设置"时间范围下推"的过滤条件（契约见 ts_am.h）。
 *
 * 没有下推条件 ⇒ 全部分区（超集语义）。
 *
 * ⚠️ **下推必须给全"或"**：范围解析不出来时**不要**给条件，
 * 宁可全扫，也不能少给（坑 M6-54）。
 *
 * 【为什么这个入口是"上下文级"的】
 *
 * 契约把范围挂在上下文上 ⇒ 范围是**实例级**的（所有连接共享）。
 * 所以它只能当"默认范围"用 ——
 * 真正的查询路径走 push()（范围在**扫描状态**里，不共享）。
 */
void ts_am_set_time_range(TsAmCtx *ctx, bool has_range, i64 lo_ms, i64 hi_ms)
{
    TsAmExt *ext = ts_am_ext(ctx);

    if (!ext) {
        return;
    }
    ext->has_range = has_range;
    ext->lo_ms = lo_ms;
    ext->hi_ms = hi_ms;
}
