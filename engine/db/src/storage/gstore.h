#ifndef DB_STORAGE_GSTORE_H
#define DB_STORAGE_GSTORE_H

#include "common/config.h"
#include "common/error.h"
#include "common/loghook.h"
#include "common/types.h"
#include "storage/pager.h"

/*
 * gstore —— 定长槽页原语（v1.0 从 modalities/graph/ 下沉到 storage/）
 *
 * 【为什么不用 slotted page】
 *
 * storage/page.c 是 slotted page：变长记录 + slot 数组（offset + length）。
 * 那套机制的每一项都是为**变长记录**设计的：
 *   - slot 数组：记录长度不一，要靠它算出第 i 条记录在哪
 *   - 变长区：记录从页尾往上长
 *   - compact：删除后把活记录往下压
 *
 * 而图的记录是**定长**的（节点记录固定 GR_NODE_SIZE，固定列属性）。
 * 定长记录的位置是纯算术可得：
 *
 *   记录 i 的页内偏移 = 页头 + i × rec_size
 *
 * 用 slotted page 的话，slot 数组纯属浪费一倍空间 ——
 * 存 8 字节的 offset+length，只为了算一个乘法就能得到的偏移。
 *
 * 所以 storage 层另有一套页格式：**定长槽页**（图和向量模态共用）。
 *
 * 【定长槽页的页布局】
 *
 *   [页头 20B][槽 0][槽 1][槽 2] ... [槽 nslots-1]
 *    ↑ 槽 i = 固定 rec_size 字节，第 i 槽的偏移 = 20 + i*rec_size
 *
 * 页头字段：
 *   0  u8  页类型（由调用方指定：PT_GRAPH_NODE / PT_VEC_INDEX …）
 *   1  u8  标志位（bit0 = 目录页）
 *   2  u16 rec_size      记录大小（页内固定）
 *   4  u16 nslots        本页槽数 = (PAGE_SIZE - 20) / rec_size
 *   6  u16 free_head     本页空闲链头（槽序号，GS_NO_SLOT = 无）
 *   8  u32 next_page     页链（同类页串成链，用于全量遍历）
 *   12 u32 lsn           本页最后修改的 LSN（WAL 幂等判断）
 *   16 u16 crc16         页校验（与 slotted page 共用 page.c 的实现）
 *   18 u16 保留
 *
 * 【为什么页头是 20B 而不是 16B】
 *
 * 页校验和由 storage/page.c 的 page_set_checksum 统一写，
 * 它固定写在 H_CKSUM = 16（这是所有页类型共用的位置）。
 * 如果页头只有 16B，槽 0 就从偏移 16 开始 ——
 * 校验和会直接覆盖槽 0 的第一个字节（in_use 标志）。
 *
 * 这类"两个模块各写各的，字段悄悄重叠"的错误最难查：
 * 编译不报错、测试也不报错，只有第 4 项那种"读整页看字节"的
 * 断言才能发现。所以页头宽度统一取 20B（= config.h 的 HDR_SIZE），
 * 校验和落在页头内部，槽区从 20 开始。
 *
 * 【空闲链】
 *
 * 页内空闲的槽串成一条链：free_head → 槽的 next_free → ...
 * 每个空闲槽的前 4 字节存"下一个空闲槽的序号"。
 * 分配时优先从链头取；链空则新开一页。
 *
 * 【为什么空闲记录立即复用是可以接受的】
 *
 * 代价：读游标跨语句时可能看到记录被复用（旧游标拿着 RID，
 * 记录被删又建）。缓解：调用方的游标只在单语句内有效（图模态写死了这条）。
 * 这和 heap 游标跨语句失效是同一性质的约束。
 *
 * 【依赖方向】
 *
 * gstore 在 storage/（L1），只依赖 common/。
 *
 * v0.11 它住在 modalities/graph/ —— 那时只有图用它，放模态内部最自然。
 * v1.0 向量模态也要用定长槽页，而硬规则 3 禁止模态之间互相 include，
 * 所以下沉到这里。**纯搬家，逻辑一行没改** ——
 * 搬完立刻跑 v0.11 全部测试当回归门。
 *
 * 这是"第三个用户出现时才抽象"的反面用法：
 * 只有一个人用的抽象是猜的；第二个用户出现时，
 * 边界才真的看得出来（而不是提前猜一个通用接口）。
 */

typedef struct GStore GStore;

/*
 * gstore 的内部结构。
 *
 * 【为什么结构体放在头文件里】
 *
 * 以前它在 gstore.c 里（私有）。但现在有两个 .c 文件要碰它：
 *   - gstore.c        —— 分配 / 读写 / 恢复
 *   - gstore_cursor.c —— 拉式游标（遍历）
 * C 里没有"同一模块的私有头"这个概念，要么把结构体放头文件，
 * 要么把游标也塞进 gstore.c（那样就超 800 行了）。
 *
 * 它是**存储层内部结构**，不是公共 API：
 * 上层（graph / graph_am / 将来的 vector）只该通过 gstore_*.h 里的函数访问它。
 */
struct GStore {
    Pager      *pg;
    LogHookFn  wal;
    void       *wal_ctx;

    u16  rec_size;
    u8   page_type;

    u32  head_page;   /* 页链头（PAGE_ID_NIL = 空） */
    u32  tail_page;   /* 页链尾 */
    u32  npages;      /* 本 gstore 管理的页数 */
    u64  alive;       /* 在用槽数 */

    u64  rec_reads;   /* 累计取出的记录条数（诊断 / 测试用） */
};

/* 页头大小（= config.h 的 HDR_SIZE，槽区从这之后开始） */
#define GS_HDR_SIZE 20u

/* 页头字段偏移 */
enum {
    GS_H_TYPE      = 0,   /* u8  页类型 */
    GS_H_FLAGS     = 1,   /* u8  标志位 */
    GS_H_REC_SIZE  = 2,   /* u16 记录大小 */
    GS_H_NSLOTS    = 4,   /* u16 槽数 */
    GS_H_FREE_HEAD = 6,   /* u16 空闲链头 */
    GS_H_NEXT      = 8,   /* u32 页链 */
    GS_H_LSN       = 12,  /* u32 LSN */
    GS_H_CKSUM     = 16   /* u16 crc16 */
};

/* "没有这个槽" */
#define GS_NO_SLOT ((u16)0xFFFFu)

/*
 * 页标志位
 *
 * GS_FLAG_DIR = 「本 gstore 页链的**头页**」。
 *
 * 【为什么链头要有标记】
 *
 * 页链是「head → next → next → ...」的单链表，next 指针存在每一页里。
 * 但**链头本身**没有反向指针 —— 没有任何一页记着「我前面是谁」。
 * 所以重启后光看页链找不到头在哪。
 *
 * 解法：头页打一个标志位。扫一遍文件找「类型对 + 带 GS_FLAG_DIR」的页，
 * 那就是链头，顺着 next 走就能把整条链和统计信息重建出来。
 *
 * 【为什么不在内存里记链头号】
 *
 * 内存里的东西重启就没了 —— 而 v0.11 的验收 A5（崩溃恢复）
 * 要求重启后节点数、边数、3 跳结果全对。链头找不到 = 整张图看不见。
 *
 * blob 用的是同一套思路（扫全文件找带 BLOB_FLAG_HEAD 的页），
 * 参见 storage/blob.c 的 blob_dir_scan。
 */
#define GS_FLAG_DIR 0x01u

/*
 * 一个槽的公共前导：
 *   0  u8  in_use（1 = 在用，0 = 空闲）
 *   1  u8  保留
 *   2  u16 下一空闲槽（仅 in_use = 0 时有效）
 *   4  ... 模态自己的字段
 *
 * 空闲槽的前 4 字节是空闲链的链接；模态的字段区不动。
 */
#define GS_REC_IN_USE 0
#define GS_REC_NEXT_FREE 2
#define GS_REC_HDR_SIZE 4u

/* ---------------- 页内布局助手 ---------------- */

/*
 * 这些 inline 被 gstore.c 和 gstore_cursor.c 共用。
 *
 * 【为什么放在头文件里】
 *
 * 页格式的**唯一定义地**必须只有一处。gstore.c 管分配/读写/恢复，
 * gstore_cursor.c 管遍历 —— 两个文件都要算"第 i 个槽在哪"、
 * "这个槽在不在用"、"下一页是谁"。
 *
 * 如果各自写一份静态 inline，页头改一个字段（比如加一个 flags 位）
 * 就可能只改一处 → 遍历和分配对页的理解不一致 →
 * "分配说这里有 10 个槽，遍历说只有 8 个"这种极难查的错。
 *
 * 所以：格式定义在头文件，实现细节在 .c。
 */

static inline u16 gs_rec_size(const u8 *pg) { return get_u16(pg + GS_H_REC_SIZE); }
static inline u16 gs_nslots(const u8 *pg)   { return get_u16(pg + GS_H_NSLOTS); }
static inline u16 gs_free_head(const u8 *pg){ return get_u16(pg + GS_H_FREE_HEAD); }
static inline u32 gs_next(const u8 *pg)     { return get_u32(pg + GS_H_NEXT); }

static inline void gs_set_rec_size(u8 *pg, u16 v) { put_u16(pg + GS_H_REC_SIZE, v); }
static inline void gs_set_nslots(u8 *pg, u16 v)   { put_u16(pg + GS_H_NSLOTS, v); }
static inline void gs_set_free_head(u8 *pg, u16 v){ put_u16(pg + GS_H_FREE_HEAD, v); }
static inline void gs_set_next(u8 *pg, u32 v)     { put_u32(pg + GS_H_NEXT, v); }

/* 槽 i 的页内偏移（纯算术 —— 这是定长槽页的核心） */
static inline u32 gs_slot_off(const u8 *pg, u16 i)
{
    return (u32)GS_HDR_SIZE + (u32)i * gs_rec_size(pg);
}

/* 槽内公共字段 */
static inline bool gs_in_use(const u8 *rec) { return rec[GS_REC_IN_USE] != 0; }
static inline u16 gs_next_free(const u8 *rec) { return get_u16(rec + GS_REC_NEXT_FREE); }
static inline void gs_set_in_use(u8 *rec, bool v) { rec[GS_REC_IN_USE] = v ? 1 : 0; }
static inline void gs_set_next_free(u8 *rec, u16 v) { put_u16(rec + GS_REC_NEXT_FREE, v); }

/*
 * 创建 gstore。
 *
 * fn/ctx 是 WAL 钩子：定长槽页改写要走 WAL，否则崩溃后邻接链会丢
 * （链指针是数据，日志里没有 → 重放不回来 → 图变成断链）。
 *
 * rec_size 是这个 gstore 管理的记录大小（全 gstore 统一）。
 * page_type 是页类型（PT_GRAPH_NODE / PT_GRAPH_EDGE）。
 */
int  gstore_create(Pager *pg, LogHookFn fn, void *ctx,
                   u16 rec_size, u8 page_type, GStore **out);
void gstore_destroy(GStore *gs);

/* 本 gstore 的记录大小 / 页类型 */
u16  gstore_rec_size(const GStore *gs);
u8   gstore_page_type(const GStore *gs);

/*
 * 分配一个槽。返回槽的 RID（页号 + 槽号）。
 *
 * 优先复用页内空闲槽；没有就新开一页。
 * 返回的槽内容是**零**（除了 in_use 会被置 1）。
 *
 * xid 显式传（硬规则 5）：写路径的接口都带 xid 参数，
 * 没有任何"设置当前 xid"的 API。图模态传 0（系统事务）。
 */
int  gstore_alloc(GStore *gs, u32 xid, Rid *out);

/*
 * 分配一个槽并**立刻写入初始内容**。
 *
 * 【为什么要有这个】
 *
 * PLAN-v0.11 §1.7 R3：「分配记录时先 in_use=1 + 全字段写完才挂链」。
 *
 * 如果分两步（alloc 出来是零 → 之后再 put），中间那一瞬间
 * 槽是"在用但内容全零"。对图模态这是危险的：
 * 邻接链的空指针是显式值 GR_RID_NIL（全 1），
 * 而全零意味着"指向 page 0" —— 是个**看起来合法**的指针。
 * 崩在中间 = 图里多出一条指向 page 0 的幽灵边。
 *
 * 所以：一次操作里把记录**完整**写好再让它可见。
 * init 的前 GS_REC_HDR_SIZE 字节会被忽略
 * （公共头由 gstore 自己管：in_use / next_free）。
 */
int  gstore_alloc_with(GStore *gs, u32 xid, const u8 *init, u16 len, Rid *out);

/*
 * 读一个槽的内容。buf 容量至少 rec_size。
 *
 * 槽不存在或已空闲 → E_NOTFOUND。
 */
int  gstore_get(GStore *gs, Rid rid, u8 *buf, u16 cap, u16 *len);

/*
 * 写一个槽的内容。len 必须等于 rec_size（定长！）。
 *
 * 槽不存在或已空闲 → E_NOTFOUND。
 * len 不等于 rec_size → E_INVAL（定长槽页不接受变长写入）。
 */
int  gstore_put(GStore *gs, u32 xid, Rid rid, const u8 *buf, u16 len);

/*
 * 释放一个槽（摘除 + 挂回页内空闲链）。
 *
 * 槽不存在或已空闲 → E_NOTFOUND。
 */
int  gstore_free(GStore *gs, u32 xid, Rid rid);

/* 槽是否在用 */
bool gstore_slot_alive(GStore *gs, Rid rid);

/*
 * 遍历所有槽（跨页）。
 *
 * 回调返回非 OK 则停止遍历，返回该值。
 * 被释放的槽**不**回调。
 */
typedef int (*GStoreEachFn)(void *ctx, Rid rid, const u8 *rec);
int  gstore_foreach(GStore *gs, GStoreEachFn fn, void *ctx);

/* 统计：多少个槽在用 / 多少页 */
u64  gstore_alive_count(GStore *gs);
u32  gstore_page_count(const GStore *gs);

/*
 * 统计：累计从本 gstore 取出了多少条记录。
 *
 * 【为什么需要它】
 *
 * 用来区分"真下推"和"全扫 + 过滤"——
 * 这两种实现的**返回行集完全一样**，光看结果区分不出来。
 * 但它们读的记录数差一个数量级：
 *   - 真下推：只读匹配链上的那几条
 *   - 全扫：读了全表每一条
 *
 * 没有这个计数，"src = X 是真下推"就只能靠读代码相信，不能被测试证明。
 */
u64  gstore_read_count(const GStore *gs);

/* ---------------- 拉式游标 ---------------- */

/*
 * 遍历所有在用槽（跨页），一次一个。
 *
 * 【为什么需要它】
 *
 * gstore_foreach 是**推式**（回调）：一次性走完整个 gstore。
 * 但 SPI 的行源是**拉式**的 —— "给我下一行"。
 * 上层（graph_am / 将来的 vector_am）需要边扫边吐，不能先攒齐再给。
 *
 * 两种遍历并存，各有各的用处：
 *   - gstore_foreach：整表统计 / 批量重写（要访问每条记录）
 *   - gstore_cursor  ：流式扫描（只要一条就处理一条）
 */
typedef struct {
    u32  page;      /* 当前页（PAGE_ID_NIL = 走完了） */
    u16  slot;      /* 当前页内下一个要检查的槽 */
} GStoreCursor;

/* 初始化游标（还没开始） */
void gstore_cursor_init(GStore *gs, GStoreCursor *c);

/*
 * 取下一条在用记录。走完返回 E_EOF。
 *
 * rec 容量至少 gstore_rec_size(gs)，不够返回 E_NOSPACE。
 * 已释放的槽自动跳过。
 */
int  gstore_cursor_next(GStore *gs, GStoreCursor *c, Rid *rid,
                        u8 *rec, u16 cap, u16 *len);

/*
 * 页链：同类页串成链，供全量遍历。
 * gstore 内部维护 head 指针；新页挂在链尾。
 */
u32  gstore_head_page(const GStore *gs);

/* 读整页（诊断 / 测试用）。buf 容量 PAGE_SIZE。 */
int  gstore_read_page(GStore *gs, u32 page_id, u8 *buf, u16 cap);

/*
 * 接管已有的页链（重启后调用）。
 *
 * 【为什么必须有这个函数】
 *
 * gstore_create 只在内存里记 head/tail/npages/alive ——
 * 重启后这些全没了。而 v0.11 的验收 A5（崩溃恢复）要求：
 * 重启后节点数、边数、3 跳结果全对。
 *
 * 页链的 next 指针是**持久化**的（在页头里），链头才是问题：
 * 没有任何一页记着「我前面是谁」。
 *
 * 解法：链头页打 GS_FLAG_DIR 标志。扫一遍全文件，
 * 找「类型对 + 带 GS_FLAG_DIR」的页 → 那就是链头 → 顺着 next 走重建。
 *
 * 这与 blob 的做法同源（blob_dir_scan 找 BLOB_FLAG_HEAD 的页）。
 *
 * 【代价】
 * 扫全文件 = O(文件页数) 次页读。blob 也是这个代价量级。
 * 之所以选它而不是维护一个持久化目录页：
 * 目录页和实际链可能对不上（崩溃时），而扫全文件看到的
 * 是**页自己写下的** next 指针 —— 不存在一致性问题。
 *
 * 找不到链头 → 这是个空库，链保持空（不是错误）。
 * 链里某页的 rec_size 和本 gstore 不一致 → E_CORRUPT。
 */
int  gstore_recover(GStore *gs);

#endif /* DB_STORAGE_GSTORE_H */
