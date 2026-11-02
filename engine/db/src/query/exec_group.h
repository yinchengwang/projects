#ifndef DB_QUERY_EXEC_GROUP_H
#define DB_QUERY_EXEC_GROUP_H

#include "query/exec_batch.h"

/*
 * 分组的数据结构（v1.1 步骤 3）。
 *
 * 拆成两个 .c：
 *   exec_group.c —— 组键编码 + hash 组表
 *   exec_agg.c   —— 聚合执行器（累加 + 输出）
 *
 * 【拆分的界线是"干什么"】
 *   exec_group.c 管"怎么把行变成组"（编码 + 查找）
 *   exec_agg.c   管"怎么在组里累加、怎么输出"
 *
 * ==================== 组键的不变式 ====================
 *
 * 组键的**字节序 == 值的序**。
 * 因为输出按组键排序，编码乱了就等于结果顺序错。
 * 这个不变式由两个独立实现共同保证：
 *   - 生产实现：编码后按字节排序（exec_agg.c）
 *   - 参照实现：按 val_cmp 的值序排序（bench/exec_agg_ref.c）
 * 两边顺序必须一致 —— 不一致就是编码错了（bench 的对拍会抓到）。
 */

#include "common/config.h"
#include "storage/value.h"

/* 分组列 */
typedef struct {
    int ncols;
    int col[SQL_MAX_COLS];
} GroupCols;

/* 一个聚合函数的累加器 */
typedef struct {
    AggKind kind;
    int     col;        /* 聚合的列下标；COUNT(*) = -1 */
    u64     count;      /* 非 NULL 的个数（COUNT(*) 是行数） */
    i64     isum;       /* SUM 的整数部分 */
    double  dsum;       /* SUM/AVG 的浮点部分 */
    bool    is_real;    /* SUM 的结果是不是浮点（出现实数就转） */
    bool    seen;       /* 有没有见过非 NULL 的值 */
    DbValue mm;         /* MIN/MAX 的当前值 */
} AggAcc;

/*
 * 一个组。
 *
 * 【为什么累加器住在组里】
 * hash 表扩容（rehash）会**换槽位** —— 累加器如果放在组外的平行数组里，
 * 扩容后就对不上号了。放在组里，它跟着组一起搬家 ✓
 */
typedef struct {
    bool     used;
    u8      *key;       /* 键字节（malloc） */
    u32      keylen;
    AggAcc  *acc;       /* 各个聚合的累加器 */
    DbValue *rep;       /* 分组列的值（输出用） */
} Group;

/* 组表（开放寻址 hash 表） */
typedef struct {
    GroupCols cols;
    int       naggs;      /* 每个组带几个累加器 */

    Group   *tab;
    u32      tabcap;      /* 2 的幂 */
    u32      ngroups;

    u8      *keybuf;      /* 组键缓冲（一次分配，反复用） */
    u32      keycap;
} GroupTab;

/* 建表（分配 hash 表与键缓冲） */
int  group_tab_init(GroupTab *gt, const GroupCols *cols, int naggs);

/* 释放 */
void group_tab_free(GroupTab *gt);

/*
 * 找组；没有就建一个。
 *
 * 返回值：
 *   >= 0  槽位下标（*isnew 告诉你是不是新建的）
 *   -1    组数超上限（MAX_GROUPS）
 *   -2    内存不足 / 扩容失败
 */
int  group_slot(GroupTab *gt, const BatchCtx *bc, u32 row, int *isnew);

#endif /* DB_QUERY_EXEC_GROUP_H */
