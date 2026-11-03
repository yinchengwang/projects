/*
 * btree_internal.h — B+tree 的内部共享定义（**私有头**，不对外暴露）
 *
 * btree.c 原来是一个 1294 行的巨文件（v1.2 步骤 4d 加上域锁之后涨上来的）。
 * 把它拆成若干个 .c 之后，**跨文件共享**的东西集中放在这里：
 *
 *   - 页格式宏
 *   - struct BTree 的完整定义
 *   - 树锁（bt_lock / bt_unlock）
 *   - 页内原语与页操作
 *
 * 【为什么不塞进 btree.h】
 * btree.h 是**公开 API**，对外承诺"只有这些函数、这些类型"。
 * 页内偏移、页头布局都是实现细节 —— 暴露出去就等于把磁盘格式
 * 变成公开契约，以后想改页格式就会被公开头卡住。
 *
 * 页格式（docs/PLAN-v0.3.md §1.2）：
 *   [header 28B][slot 数组 u16 ...][── free ──][条目区 ←]
 *
 * 三条最容易写错的规矩：
 *   1. 叶子分裂：提升的 key **两边都保留**
 *      内部节点分裂：提升的 key **不在任何一边**（被移走）
 *   2. 内部节点查找用 upper_bound（第一个 sep > key），走 pos-1 的孩子
 *   3. compact 必须借 scratch buffer 重排，不能原地搬
 */

#ifndef DB_INDEX_BTREE_INTERNAL_H
#define DB_INDEX_BTREE_INTERNAL_H

#include <pthread.h>

#include "index/btree.h"

/* ---------------- 页格式 ---------------- */

#define BT_HDR       28u
#define BT_TYPE      0u
#define BT_FLAGS     1u
#define BT_NITEMS    2u
#define BT_FREEOFF   4u
#define BT_FREEBYTES 6u
#define BT_RIGHT     8u
#define BT_LEFTMOST  12u
#define BT_LEVEL     16u
#define BT_LSN       20u
#define BT_CKSUM     24u

#define BT_INNER 0x03u
#define BT_LEAF  0x04u

/*
 * 索引 key 的格式版本（存在页头的 BT_FLAGS 字节里）。
 *
 * 【为什么需要它】
 * 索引 key 的编码是可以变的（v1.1 就把「只有列值」改成了「列值 + RID」）。
 * 变了之后，磁盘上**已有的**索引页里存的是旧格式的 key ——
 * 拿新代码去读，结果是错的，而且不报错。
 *
 * 所以页头里记一个版本号：版本对不上就重建这个索引。
 * 索引本来就是派生物（能从堆重建），重建一次没有语义风险。
 *
 * 版本 1 = 旧格式（key 只有列值，有重复值丢行的 bug）
 * 版本 2 = 复合键（列值 + RID）
 */
#define BT_FMTVER 2u

struct BTree {
    Pager    *pager;
    FreeList *fl;
    u32       root;
    u32       height;
    u32       npages;
    u8        scratch[PAGE_SIZE];   /* compact 用，避免原地搬 */

    /*
     * ==================== v1.2 步骤 4d：btree 域的锁 ====================
     *
     * 【为什么 B+tree 必须有自己的锁】
     *
     * 树结构本身就是**共享可变状态**，而且缺一不可：
     *
     *   - `root` / `height`：根分裂要换根，根降高要换根
     *   - `scratch[]`：**compact 用的整页暂存区** —— 两个线程同时用它
     *     就是两个人往同一块内存里搬数据 → 页内容直接乱掉
     *   - **分裂本身不是原子的**：`insert_rec` 先在孩子页插，
     *     再回父亲页插分隔键 —— 这两步之间如果另一个线程也在分裂，
     *     父亲页的条目就可能重复或丢失
     *
     * 大锁在的时候，这些全被顺带护住了。
     * 大锁一撤（步骤 4d），实测**并发写索引会丢索引项**：
     * 1200 行并发插入，堆里 1200 行全在，点查却能漏掉 200 多行 ——
     * **静默的数据不一致**（行在，索引找不到）。
     *
     * 【为什么是"每棵树一把"】
     *
     * 锁的粒度 = 索引的粒度。不同索引互不阻塞，
     * 同一索引上的写才排队 —— 这正是索引该有的并发度。
     *
     * 【锁的全序】
     *
     * btree 在全序里排第 3（catalog → txn → btree → frame → wal → freelist）。
     * 因为树的操作要拿页（`pager_get` → frame 域），
     * 所以 btree 比 frame 更外层 ✓
     *
     * 反过来没有边：**没有任何路径是"持有页锁再去取树锁"的** ——
     * 堆的写路径是 `heap_insert`（页锁在里面取好又放掉）
     * 然后才 `index_insert_row`（树锁），两步是**先后**不是嵌套 ✓
     */
    pthread_mutex_t mu;
};

/* ---------------- 树锁 ---------------- */

/*
 * 取域锁 + 向 latch 框架记账。
 *
 * 记账让"catalog → txn → btree → frame → wal → freelist"这条全序
 * 从步骤 2 起就是**机器检查**的。
 */
void bt_lock(BTree *bt);
void bt_unlock(BTree *bt);

/* ---------------- header / slot 访问 ---------------- */

u8   bt_type(const u8 *pg);
u16  bt_nitems(const u8 *pg);
u16  bt_free_off(const u8 *pg);
u16  bt_free_bytes(const u8 *pg);
u32  bt_right(const u8 *pg);
u32  bt_leftmost(const u8 *pg);
u32  bt_level(const u8 *pg);

void set_nitems(u8 *pg, u16 v);
void set_right(u8 *pg, u32 v);
void set_leftmost(u8 *pg, u32 v);
void set_level(u8 *pg, u32 v);

u16  bt_slot(const u8 *pg, u16 i);
void set_slot(u8 *pg, u16 i, u16 off);

void recalc_free(u8 *pg);
void bt_checksum(u8 *pg);

/* ---------------- 条目访问 ---------------- */

u16           item_len(const u8 *pg, u16 i);
const u8     *item_key(const u8 *pg, u16 i, u16 *klen);
u64           item_val(const u8 *pg, u16 i);
int           key_cmp(const u8 *a, u16 alen, const u8 *b, u16 blen);

/* 第一个 >= key 的位置 */
u16 lower_bound(const u8 *pg, const u8 *key, u16 klen);

/* 第一个 > key 的位置 */
u16 upper_bound(const u8 *pg, const u8 *key, u16 klen);

/* ---------------- 页初始化 / 整理 ---------------- */

int  bt_init(u8 *pg, u8 type, u32 level);
void bt_compact(BTree *bt, u8 *pg);

/* 在第 pos 个位置插入一项 */
int insert_at(BTree *bt, u8 *pg, u16 pos,
              const u8 *key, u16 klen, u64 val);

int remove_at(u8 *pg, u16 pos);

/* ---------------- 页分配 / 回收 ---------------- */

int bt_alloc_page(BTree *bt, u8 type, u32 level,
                  u32 *pid_out, PageFrame **f_out);

/* ---------------- 查找 ---------------- */

int descend(BTree *bt, const u8 *key, u16 klen, u32 *leaf_out);

/* ---------------- 分裂产物 ---------------- */

/*
 * 分裂的产物：提升的分隔键 + 新页号。
 *
 * `split` 为 false 表示"没有发生分裂"，调用方据此决定要不要回父亲页插分隔键。
 */
typedef struct {
    u8  sep[BT_MAX_KEY];
    u16 sep_len;
    u32 right;
    bool split;
} Split;

/* ---------------- 观察接口 ---------------- */

/*
 * 【观察接口为什么要取锁】
 *
 * `root` / `height` / `npages` 是树结构的可变状态，
 * 写线程会改它们（根分裂、根降高）。不加锁读就是数据竞争。
 *
 * `const BTree *` 的意思是"调用方不许改这棵树"，
 * 不是"这个对象里的每个字节都不可变"——
 * 锁的状态当然要变。所以取锁时显式去掉 const
 * （与 freelist.c 的同类说明一致）。
 */
BTree *bt_mut(const BTree *bt);

#endif /* DB_INDEX_BTREE_INTERNAL_H */
