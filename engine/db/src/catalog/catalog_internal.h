#ifndef DB_CATALOG_INTERNAL_H
#define DB_CATALOG_INTERNAL_H

/*
 * catalog_internal.h — catalog 内部的跨文件声明（v1.2 步骤 7）
 *
 * 【为什么有这个头】
 * v1.2 步骤 7 把原来 1036 行的 catalog.c 按职责拆成三个 .c：
 *   catalog.c       —— 生命周期 + 公共入口 + *_locked 包装
 *   catalog_write.c —— 写路径（meta 页镜像、写系统表行）
 *   catalog_load.c  —— 加载路径（读系统表恢复内存元数据）
 *
 * 只有**真的跨文件共享**的东西才放这里：
 *   - 系统表 schema（写路径要按它编码，加载路径要按它解码）
 *   - 写路径与加载路径互相调用的那几个内部函数
 * 只被单个文件使用的辅助函数一律留在那个文件里 static 化。
 *
 * 【注意】本头只被 catalog/ 内部的文件 include，
 * 不改变任何公开 API 的语义（公开头仍是 catalog.h）。
 */

#include "catalog/catalog_priv.h"

/* ---------------- 系统表 schema（硬编码） ----------------
 *
 * 定义在 catalog_load.c（与 sys_cols_of 同居），这里是声明。
 *
 * 为什么必须硬编码：sys_column 的行要靠 schema 才能解码，
 * 而 schema 又存在 sys_column 里 —— 不硬编码就是自举死锁。
 */
#define N_SYSTABLE_COLS 6
extern const ColDesc SYS_TABLE_COLS[N_SYSTABLE_COLS];

#define N_SYSCOLUMN_COLS 5
extern const ColDesc SYS_COLUMN_COLS[N_SYSCOLUMN_COLS];

#define N_SYSINDEX_COLS 7
extern const ColDesc SYS_INDEX_COLS[N_SYSINDEX_COLS];

#define N_SYSINDEXCOL_COLS 3
extern const ColDesc SYS_INDEXCOL_COLS[N_SYSINDEXCOL_COLS];

/* ---------------- 加载路径（catalog_load.c 实现） ---------------- */

/* 取系统表的硬编码 schema；不是系统表返回 NULL（*ncols = 0） */
const ColDesc *sys_cols_of(u32 table_id, int *ncols);

/* 按 catalog_root 把磁盘上的元数据读进内存 */
int load_all(Catalog *cat);

/* ---------------- 写路径（catalog_write.c 实现） ---------------- */

/* 往内存里的表项数组追加一项（必要时扩容） */
int ent_add(Catalog *cat, const TableMeta *m, Heap *h, FreeList *fl,
            const ColDesc *cols, int ncols, Rid meta_rid);

/* meta 页整页镜像进日志并落盘 */
int meta_write(Catalog *cat, u32 xid);

/* 写系统表的行 */
int write_table_row(Catalog *cat, u32 xid, const TableMeta *m, Rid *rid_out);
int write_col_rows(Catalog *cat, u32 xid, u32 table_id,
                   const ColDesc *cols, int ncols);
int write_index_row(Catalog *cat, u32 xid, const IndexMeta *m);
int write_indexcol_row(Catalog *cat, u32 xid, u32 index_id, u32 seq, u32 col_no);

#endif /* DB_CATALOG_INTERNAL_H */
