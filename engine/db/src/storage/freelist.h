#ifndef DB_STORAGE_FREELIST_H
#define DB_STORAGE_FREELIST_H

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"

/*
 * 页空间索引（Free Space Map）
 *
 * 解决的问题：插入一行时，怎么快速找到一个放得下的页？
 * 线性扫所有页太慢，所以给每页记一个「剩余空间档位」：
 *
 *   level = free_bytes * 255 / USABLE      (0 = 满, 255 = 整页空闲)
 *
 * 再把档位分桶计数，查找时从「刚好够」的档位往上找第一非空桶 ——
 * 这就是 best-fit，能减少碎片。
 *
 * 这是 PG 的 FSM 的简化版：PG 用三层树做到 O(log n)，
 * 这里用扁平数组 + 桶计数，查找最坏 O(npages)，v0.1 够用。
 *
 * 注意：本模块只管内存索引，不做 IO。
 * 档位是「派生数据」，可以在打开数据库时扫描所有页重建 ——
 * 这样就不需要把 FSM 持久化，也就没有自举问题。
 *
 * 所有权约定：新建/扩容出来的页默认档位 0（不可用），
 * 调用方必须用 freelist_set 显式登记属于自己表的页。
 * 这样多张表各持一个 FreeList 时不会互相串页。
 */

typedef struct FreeList FreeList;

#define FL_MAX_LEVEL 255u
#define FL_USABLE    ((u16)(PAGE_SIZE - HDR_SIZE))

int  freelist_create(u32 npages, FreeList **out);
void freelist_destroy(FreeList *fl);
int  freelist_grow(FreeList *fl, u32 npages);

/* 当前档位 */
u8   freelist_level(const FreeList *fl, u32 page_id);

/* 按剩余字节数设置档位（内部换算成 level） */
void freelist_set(FreeList *fl, u32 page_id, u16 free_bytes);

/* 标记为满（level 0） */
void freelist_set_full(FreeList *fl, u32 page_id);

/*
 * 找一页能放下 need 字节的页（best-fit：刚好够的最小档位）。
 * 找不到返回 PAGE_ID_NIL。
 */
u32  freelist_find(const FreeList *fl, u16 need);

u32  freelist_npages(const FreeList *fl);

/* 某个档位上有多少页（测试用，也便于观察分布） */
u32  freelist_count_at(const FreeList *fl, u8 level);

#endif /* DB_STORAGE_FREELIST_H */
