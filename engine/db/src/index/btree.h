#ifndef DB_INDEX_BTREE_H
#define DB_INDEX_BTREE_H

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "storage/freelist.h"
#include "storage/pager.h"

/*
 * B+tree 索引
 *
 * 不用 slotted page —— 理由见 docs/PLAN-v0.3.md §1.1：
 * slotted page 的 slot 编号必须稳定（RID 依赖），而 B+tree 需要 slot
 * 按 key 有序、插入时在中间插，两者语义冲突。
 *
 * 允许重复 key（非唯一索引）：相同 key 聚在一起，查找时向后扫。
 *
 * ⚠ key 的字节序契约（踩过一次）：
 *   B+tree 用 memcmp 比较 key，所以 **key 必须是"字节序 = 数值序"的编码**。
 *   整数 key 一律用 **大端**，例如 u32 k 编成 4 字节：
 *       b[0]=k>>24; b[1]=k>>16; b[2]=k>>8; b[3]=k;
 *   如果用系统默认的小端（put_u32），63 的字节是 3F 00 00 00、
 *   256 是 00 01 00 00，memcmp 会认为 63 > 256 —— 索引静默错乱。
 *   页内元数据用小端、索引 key 用大端，同一个系统两种字节序，别混。
 */

/*
 * key 的最大字节数。
 *
 * 索引 key 是复合键「值部分 || RID(8 字节)」：
 * 值部分最长 = 文本 255 字节（VAL_TEXT_MAX - 1）+ 2 字节终止符 = 257，
 * 加 RID 8 字节 = 265。取 288 留余量。
 *
 * 【改这个数要同时改 index_key.c 的终止符约定】
 */
#define BT_MAX_KEY 288

typedef struct BTree    BTree;
typedef struct BtCursor BtCursor;

int  btree_create(Pager *pg, FreeList *fl, u32 *root_out);
int  btree_open(Pager *pg, FreeList *fl, u32 root, BTree **out);
void btree_destroy(BTree *bt);

/* 插入。同一 key 可以对应多个 RID。 */
int  btree_insert(BTree *bt, const void *key, u16 klen, Rid rid);

/* 删除指定 (key, rid)。 */
int  btree_delete(BTree *bt, const void *key, u16 klen, Rid rid);

/* 精确查找：同一 key 的所有 RID 写进 out[]，*n 返回实际个数（可能 > max）。 */
int  btree_search(BTree *bt, const void *key, u16 klen, Rid *out, int max, int *n);

/* 范围扫描。lo/hi 传 NULL 表示该侧无界，闭区间 [lo, hi]。 */
int  btree_scan(BTree *bt, const void *lo, u16 lolen,
                const void *hi, u16 hilen, BtCursor **out);
int  btree_cursor_next(BtCursor *c, Rid *rid);
void btree_cursor_close(BtCursor *c);

/*
 * ==================== 游标的契约（v1.2 步骤 4d） ====================
 *
 * 1. **必须 close**：btree_scan 取的树锁一直持有到 btree_cursor_close。
 *    不 close 就等于把这棵索引的锁永久占着 —— 后面的写全堵住。
 * 2. **同一线程用**：锁是创建游标的那个线程持有的，
 *    换线程调用就是在别人的锁上解锁（未定义行为）。
 * 3. 游标活着的时候，同一棵索引上的写会等它扫完 ——
 *    这是"宁可慢，不可少行"的取舍（见 btree.c 里 btree_scan 的注释）。
 */

/* 把整棵树重置成空树，所有页还给 freelist。索引重建用。 */
int  btree_reset(BTree *bt);

/* 观察用 */
u32  btree_height(const BTree *bt);
u32  btree_npages(const BTree *bt);
u32  btree_root(const BTree *bt);

/*
 * 索引 key 的格式版本对不对得上当前代码？
 * 对不上 → 这个索引是用旧编码建的，必须重建才能用。
 */
int  btree_format_ok(const BTree *bt);

typedef struct {
    u32 npages;
    u32 nleaves;
    u32 ninner;
    u32 items;       /* 所有页的条目总数（含内部节点的分隔键） */
    u32 leafitems;   /* 只算叶子页里的条目 —— 真正存着的 key→rid 条目数 */
    u32 fill_pct;    /* 平均填充率：(容量 - 空闲) / 容量 */
} BtStats;

/* 遍历全树统计。验收"页利用率"用这个。 */
int  btree_stats(BTree *bt, BtStats *out);

#endif /* DB_INDEX_BTREE_H */
