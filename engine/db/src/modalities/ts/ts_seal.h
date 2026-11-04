#ifndef DB_MODALITIES_TS_TS_SEAL_H
#define DB_MODALITIES_TS_TS_SEAL_H

#include "common/error.h"
#include "common/types.h"
#include "modalities/ts/tsstore.h"
#include "storage/pager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ts_seal.h —— 时序分区的**封窗**（v1.5 步骤 3）
 *
 * 【这一层解决什么】
 *
 * 步骤 2 的分区只会一直变大（裸写页链）。时间久了老分区占着一大堆页，
 * 而里面的数据**再也不会变**了 —— 冷数据却按最贵的格式存着。
 *
 * 封窗 = 把一个老分区**转成列式压缩页**，并且**只读**。
 * 封窗是不可逆的：转过去之后不能再写回裸写页。
 *
 * 【封窗的原子性：靠"目录项的状态"做唯一权威】（坑 M6-40）
 *
 * 封窗分两步：
 *
 *   第一步 `ts_seal_write()`  —— 把裸写页整段读出来，编码成列式页。
 *                                **只写页，不碰目录。**
 *   第二步 `ts_store_seal_commit()` —— 把目录项翻成 SEALED，
 *                                并把 first_page 指到列式头部页。
 *
 * 为什么是两步、而且**先写页后翻目录**：
 *
 *   崩在两步之间 ⇒ 目录项还是 ACTIVE ⇒ 读路径走裸写页 ⇒
 *   读到的是**完整的旧分区**（列式页成了孤儿：浪费空间，不丢数据）。
 *
 *   反过来（先翻目录后写页）⇒ 目录项说"这个分区是列式的"，
 *   但列式页只写了一半 ⇒ 读路径读到**半截数据** ——
 *   而半截数据往往"看起来正常"，比直接报错坏得多。
 *
 * 所以：**目录项是唯一的权威**。重开库时只看目录项：
 * ACTIVE ⇒ 读裸写页；SEALED ⇒ 读列式页。不会出现"新旧并存"，
 * 也就不会出现"读到半截"。
 *
 * ⚠️ 计划里说的 `sealed_lsn` 落盘即封窗成功 —— 本模态**没有 WAL**
 * （坑 M6-31），没有 LSN 可记 ⇒ 改用**单调递增的封窗序号** seal_seq
 * （记在列式头部页里）。它只用来标识"这是第几次封窗"，
 * **不参与**恢复判断（恢复判断只看目录项的状态）。这是与计划的偏差，
 * 如实记在账上。
 *
 * 【封窗的触发条件：本轮只做"行数 ≥ 阈值"】
 *
 * - ✅ 行数达到阈值 ⇒ 封窗（阈值见 TS_SEAL_ROW_THRESHOLD）。
 * - ❌ "时间窗滑过"（时间往前跑，旧窗口自然变冷）—— **检测不到**：
 *   没有任何人在某个时刻去检查"这个窗口是不是已经过去了"。
 *   要做这件事，要么给写入路径加一个"每次写都检查当前时间"的钩子，
 *   要么加一个后台线程 —— 两者都超出本步范围。
 *   **这一条如实记为未做**，留给步骤 6 / v1.6。
 *
 * 【旧页只标记、不回收】（坑 M6-42）
 *
 * 封窗后旧裸写页**不再使用**（列式头部页里记了它们的首页与页数），
 * 但本模态**没有 freelist** ⇒ 本轮**不真回收**。
 * 代价：文件**只涨不缩** —— 与地理模态 reindex 同样的代价。
 * 真正的回收要等"模态自己的空闲页表"，那是步骤 6 的账。
 *
 * 【迟到写入必须被明确拒绝】（坑 M6-43）
 *
 * 封窗不可逆。迟到的读数（时间戳落在已封窗的窗口里）**必须报错**
 * （E_NOTSUP），**不能**悄悄开一个新分区 ——
 * "一个时间窗 = 一个分区"是不变式（R3），破了它，按分区裁剪的查询
 * 就会**静默漏数据**（查到的是新分区，漏掉的是封窗分区）。
 *
 * 【本头文件里的函数分两处实现】
 *
 *   ts_seal_write()      —— 在 ts_seal.c 里实现（只碰 Pager）。
 *   ts_store_seal_commit() / ts_store_seal() —— 在 tsstore.c 里实现
 *       （要碰目录与运行时状态）。
 *   之所以分两处：目录对象（TsDir）与运行时状态（rt[]）是
 *   tsstore.c 的**私有**的，这一层没有理由把它们暴露出去。
 */

/* 封窗阈值：一个分区的行数达到这么多就封窗 */
#define TS_SEAL_ROW_THRESHOLD 10000u

/*
 * 第一步：把一个**活跃分区**的裸写页转成列式页。
 *
 * first_page —— 裸写分区的首页（ts_store_part_info 能拿到）。
 * seal_seq  —— 封窗序号（单调递增；没有 WAL ⇒ 没有 LSN，见上面的说明）。
 * reads     —— 累计逻辑页读（可为 NULL）。
 *
 * head_out  —— 列式头部页页号；pages_out —— 本次新占的页数（含头部页）；
 * n_rows_out —— 这个分区实际有多少行。
 *
 * ⚠️ 本函数**不改目录**：分区在目录里仍然是 ACTIVE。
 *    要让它变成 SEALED，必须再调 ts_store_seal_commit()。
 *
 * 返回 OK / E_NOSPACE / E_CORRUPT / E_INVAL。
 */
int ts_seal_write(Pager *pg, u32 first_page, u32 seal_seq, u64 *reads,
                  u32 *head_out, u32 *pages_out, u32 *n_rows_out);

/*
 * 第二步：把目录项翻成 SEALED。
 *
 * head_page / pages / n_rows —— 第一步的返回值。
 *
 * 分区已经不是 ACTIVE（已封窗 / 已丢弃）⇒ E_NOTSUP。
 */
int ts_store_seal_commit(TsStore *st, u32 idx, u32 head_page, u32 pages, u32 n_rows);

/*
 * 封窗一个分区（把两步编排起来）。
 *
 * idx —— **磁盘序**的分区下标（ts_store_part_info 给的是时间序，
 * 这里用的是目录的磁盘序 —— 与 ts_dir_get 一致）。
 *
 * 返回 OK / E_NOTSUP（已封窗）/ E_CORRUPT / E_NOSPACE / E_INVAL。
 */
int ts_store_seal(TsStore *st, u32 idx);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_TS_TS_SEAL_H */
