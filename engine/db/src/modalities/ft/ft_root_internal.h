#ifndef DB_MODALITIES_FT_ROOT_INTERNAL_H
#define DB_MODALITIES_FT_ROOT_INTERNAL_H

/*
 * ft_root_internal.h —— 根页 / 目录页"写"路径的内部契约
 *
 * 从 ft_root.c 拆出 ft_root_write.c 时（原文件 871 行超限），
 * 读写两侧都要的东西集中在这里：
 *
 *   ① 尺寸常量：一页条目区放多少字节、条目多少字节
 *   ② 两个写页原语的**对外声明**（原 static，跨文件后去 static）
 *
 * 为什么不放进公共契约 ft_root.h：
 * 那里是**磁盘格式字段偏移**（FTR_* / FTC_*），改它等于改格式；
 * 这里是"一页能装多少"与内部函数，改它们的后果不同。
 *
 * ⚠️ 与 ft_root.h 的联动：
 *   改 FTC_DATA / PAGE_SIZE → FT_CAT_CAP 自动跟着变
 *   改 FT_SEG_ENT / FT_TOMB_ENT → ft_cat_split 的让位逻辑必须复查（坑 M4-35）
 */

#include "common/types.h"
#include "modalities/ft/ft_root.h"

#define FT_SEG_ENT   12u                       /* seg_id + head_page + state */
#define FT_TOMB_ENT   8u                       /* page_id + ordinal */
#define FT_CAT_CAP   (PAGE_SIZE - FTC_DATA)    /* 一页条目区字节数 */

#define FT_GEN_MAX        0x7FFFFFFFu          /* generation 可信上限 */
#define FT_MAX_CAT_PAGES  (1u << 20)           /* 一次更新最多几页（防死循环） */
#define FT_MAX_ENTRIES    (1u << 20)           /* 条目数可信上限 */

/* 写一页：WAL 先行 → 填 LSN → 算校验和 → 标脏放开 */
int ft_root_commit(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                   u32 pid, PageFrame *fr, const u8 *buf, u32 *lsn_out);

/* 写一张根页 */
int ft_root_page_write(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                       u32 pid, PageFrame *fr,
                       u32 slot, u32 gen, u32 prev_gen,
                       u32 cat_head, u32 cat_pages,
                       u32 n_seg, u32 n_tomb, u32 prev_cat, u32 lsn,
                       u32 *lsn_out);

#endif /* DB_MODALITIES_FT_ROOT_INTERNAL_H */
