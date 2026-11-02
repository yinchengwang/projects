#ifndef DB_SPI_PUSHDOWN_H
#define DB_SPI_PUSHDOWN_H

#include "common/error.h"
#include "common/types.h"
#include "storage/value.h"

/*
 * 谓词下推（v0.9 定稿）。
 *
 * 【为什么需要它】
 *
 * 向量检索的"过滤 + ANN"是同一件事的两面：
 * 前过滤（pre-filter）让索引失效 —— 索引只对"不加过滤"的查询友好；
 * 后过滤（post-filter）召回不足 —— 先取 top-k 再过滤，可能过滤完只剩几条。
 *
 * 真正管用的做法是**把过滤吸收进索引**（比如 DiskANN 的过滤感知遍历）。
 * 而"哪些过滤能吸收、怎么吸收"只有模态自己知道 ——
 * 所以它必须是一个接口，而不是优化器里的 if-else。
 *
 * 【下推的结果是一个行源】
 *
 * push() 返回的 AmScan 和 scan_begin() 返回的**完全同构**：
 * 都是"给我下一行的 (rid, 字节串)"。
 *
 * 于是执行器只有一种取行路径 —— 不需要为下推新增算子类型。
 * 这和 §5.2 的"能力协商靠返回值"是同一个思路：
 * **新机制不改变老机制的形状。**
 *
 * 【下推不改变正确性】
 *
 * 执行器拿到下推的行之后，**仍然完整求值一遍 WHERE**。
 * 下推只是"少读一些行"，不是"替你判断"。
 *
 * 于是下推实现错了，最多变慢，不会是错 —— 除非它漏行。
 * 所以下推的实现必须**保守**：宁可多给行，不能少给。
 *
 * 【谁决定能不能下推】
 *
 * can_push 由**模态**回答："这个谓词我能处理吗"。
 * 执行器不自己判断 —— 它不知道 blob 有没有索引、不知道图模态
 * 能不能按属性过滤。执行器只负责把表达式翻译成 Predicate。
 */

/* 谓词的比较算子。query 层必须翻译成这套编码（见 query/exec.c）。 */
#define PD_EQ   '='
#define PD_LT   '<'
#define PD_GT   '>'
#define PD_LE   'L'   /* <= */
#define PD_GE   'G'   /* >= */
#define PD_NE   'N'   /* <> */

/*
 * 一个谓词：列号 + 比较符 + 常量。
 *
 * 只支持"列 和 常量"这种形状 —— 这是能被下推的谓词的公共子集。
 * 更复杂的（列 vs 列、子查询、函数）留在执行器里求值。
 */
typedef struct {
    int     col_idx;       /* 列号（模态的列定义里的下标） */
    int     op;            /* PD_* */
    DbValue val;           /* 比较的常量 */
} Predicate;

/* 前置声明（避免和 storage_am.h 互相 include） */
typedef struct StorageAM StorageAM;
typedef struct AmScan    AmScan;

/*
 * 模态说"这个谓词我能处理吗"。
 * 返回 OK 表示可以（随后调 push），E_NOTSUP 表示不行。
 */
typedef int (*CanPushFn)(StorageAM *am, const Predicate *p);

/*
 * 把谓词交给模态，模态返回一个**行源**。
 * 行源和全表扫描同构：scan_next 给 (rid, 字节串)。
 */
typedef int (*PushFn)(StorageAM *am, const Predicate *p, AmScan **out);

#endif /* DB_SPI_PUSHDOWN_H */
