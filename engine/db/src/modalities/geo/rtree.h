#ifndef DB_MODALITIES_GEO_RTREE_H
#define DB_MODALITIES_GEO_RTREE_H

#include "common/error.h"
#include "common/types.h"
#include "modalities/geo/geo_wal.h"
#include "modalities/geo/mbr.h"
#include "storage/pager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * R-tree —— 地理空间模态的索引结构（Guttman 1984 原始版）。
 *
 * 【为什么空间索引不能照抄 B+tree】
 *
 * B+tree 的键是**一维全序**：两个键要么小于、要么大于、要么相等。
 * 空间数据没有全序 —— 两个矩形可以"相交，但谁也不大于谁"。
 * 所以 R-tree 的键是**矩形本身**，树里的"比较"不是大小比较，
 * 而是三种几何关系：相交 / 包含 / 距离。
 *
 * 树的价值全在**剪枝**上：窗口查询时，内部节点里那些
 * "包围盒与窗口不相交"的子树，整棵都不用下去看。
 * 剪枝判据与叶子命中判据**必须同源**（都走 mbr.h），
 * 否则边界点会在两边得出不同结论，"结果全等"这条验收就废了。
 *
 * 【本轮范围】
 *
 * 做：插入 + 二次分裂（QuadraticSplit）+ 窗口查询。
 * 不做：WAL / 崩溃恢复（步骤 6）、SPI 与公开 API（步骤 5）、
 *       删除与 reindex（步骤 6）、KNN（步骤 3）。
 *
 * 【页类型为什么本地定义】
 *
 * PT_RTREE = 0x15 定义在**本文件**里，不进 src/common/config.h。
 * 原因：config.h 是全局页类型表，改它要跟所有模态对齐；
 * 而 0x13 / 0x14 已经被全文检索（FT）预留给根页与目录页
 * （见 ft_root.h 的 FT_ROOT_PTYPE / FT_CAT_PTYPE），
 * 本模态取 0x15 避开它们。将来若要登记进 config.h，
 * 只要**数值不变**，磁盘格式完全兼容。
 */

/* ---------------- 页类型 ---------------- */

/*
 * R-tree 节点页的页类型。
 *
 * 接在 PT_FT_CAT_PTYPE(0x14) 之后 —— 0x13 / 0x14 归 FT，
 * 本模态不碰。接线时若决定登记进 common/config.h，数值不要变。
 */
#define PT_RTREE  0x15u

/* ---------------- 页内布局 ---------------- */

/*
 * 节点页的**自定义区**（从偏移 HDR_SIZE = 20 开始）。
 *
 * 为什么不用 slotted page 的变长记录：
 * 条目**定长**才能做紧凑存储与顺序访问 —— 条目长度一变，
 * 就没有"第 i 条在第几个字节"这种可直接算出来的东西，
 * 遍历就得逐条走 slot 链。
 *
 * 偏移  长度  字段
 *   20    1   flags      bit0 = 1 → 叶子节点；其余位保留
 *   21    1   reserved   保留（对齐用）
 *   22    2   n_entries  本页条目数
 *   24    4   level      节点层级：叶子 = 0，根的 level 最大
 *   28    4   n_points   见下方说明
 *   32   ...  entries[]  条目数组，每条 20 字节
 *
 * 【n_points 这个字段怎么用】
 *
 * 它记的是**整棵树的点数**，但**只写在根页上**；
 * 非根页一律为 0。
 *
 * 为什么不每个节点都记自己子树的点数：内部节点的子树点数
 * = 各孩子子树点数之和，而条目里没有地方放"孩子的点数"
 * （条目只有 4 字节 id + 16 字节 MBR）。要算就得把每个孩子页
 * 都读一遍 —— 分裂时那一下就是两百多次页读，太贵。
 *
 * 只在根上记就没有这个问题：每次插入根页必然被改写一次，
 * 根的点数 +1 是顺手的事；重开树时读一次根页就知道总点数，
 * 不用扫全树。
 */
#define RT_FLAGS_OFF    20u
#define RT_RSVD_OFF     21u
#define RT_NENT_OFF     22u
#define RT_LEVEL_OFF    24u
#define RT_NPOINTS_OFF  28u
#define RT_ENT_OFF      32u

/*
 * 条目：20 字节。
 *
 * 叶子条目：id = row_id，MBR = 该点的退化矩形（min == max）。
 * 内部条目：id = child_page，MBR = **整棵子树**的包围盒。
 *
 * 两种条目一样长 —— 节点页就不用区分叶/内两种布局，
 * 代码路径只有一份。
 */
#define RT_ENT_SIZE  20u

/* ---------------- 死条目标记（v1.4 步骤 5） ---------------- */

/*
 * 叶子条目 id 的最高位 = 已删除标志；低 31 位是 row_id。
 *
 * 【为什么用最高位，而不是给条目加一个字段】
 *
 * 条目是**定长 20 字节**（4 字节 id + 4 个 4 字节 float），
 * 一页塞得下 203 条（32 + 203×20 = 4092 字节）。
 * 加一个"是否删除"的字节就要重排条目布局 —— MBR 的 4 字节对齐被打破，
 * 访问器得改成变长步长，页容量也跟着变；
 * 而"最高位当标志"**一个字节都不用加**：id 本来就是 4 字节。
 *
 * 【代价：row_id 只有 31 位】
 *
 * row_id 上限 RT_ROWID_MAX = 2^31-1（≈21 亿）。
 * 超出 ⇒ rtree_insert 报 E_INVAL —— 与其让两个不同的 row_id 撞成
 * 同一个键（那会静默删错点），不如直接拒绝。
 *
 * 【内部节点的 id 不带这个标志】
 *
 * 内部条目的 id 是 child_page（页号），**不带** RT_ENT_DEAD；
 * 区分叶子与内部靠页里的 level 字段：level == 0 才是叶子。
 */
#define RT_ENT_DEAD   0x80000000u
#define RT_ROWID_MAX  0x7FFFFFFFu

/* 一页能放多少条目：(4096 - 32) / 20 = 203 */
#define RT_CAP       203u

/* 分裂后每组至少这么多条目：ceil(203 / 2) = 102 */
#define RT_MIN       102u

/* 树的最大层数（防御用：环 / 坏页不能把递归带进死循环） */
#define RT_MAX_DEPTH 64u

/* ---------------- 浮点与字节序 ---------------- */

/*
 * float 与 u32 的位级互转。
 *
 * 磁盘格式只允许 u8/u16/u32/u64（见 common/types.h 的铁律 1），
 * 而 Mbr 里存的是 float —— 所以页里存的是 float 的**位模式**。
 *
 * 用 memcpy 而不是 union 或指针双关：指针双关在
 * -fno-strict-aliasing 下才安全，而 memcpy 任何时候都安全，
 * 而且编译器会把它优化成一次寄存器搬运。
 */
static inline u32 rt_f2u(float f)
{
    u32 v;

    memcpy(&v, &f, sizeof(v));
    return v;
}

static inline float rt_u2f(u32 v)
{
    float f;

    memcpy(&f, &v, sizeof(f));
    return f;
}

/* ---------------- 索引对象 ---------------- */

typedef struct RTree RTree;

/*
 * 新建一棵**空树**（不分配任何页）。
 *
 * 根页在第一次插入时才分配 —— 空树没有根页可言。
 * 返回 OK / E_NOSPACE（内存不足）/ E_INVAL。
 */
int rtree_create(Pager *pg, RTree **out);

/*
 * 从**已有的根页**打开一棵树（步骤 5 接线用）。
 *
 * 根页页号从哪来是接线方的事（放不放库的元数据页里都行）。
 * root_page == PAGE_ID_NIL 表示打开一棵空树。
 *
 * 打开时会校验页类型与校验和；不对就返回 E_CORRUPT，
 * 不返回一棵"看起来能查但结果悄悄不对"的树。
 */
int rtree_open(Pager *pg, u32 root_page, RTree **out);

/*
 * 接上写日志钩子（v1.4 步骤 6 · A7）。
 *
 * 接上之后，树里每个改页的写点都会**先写日志再改页**
 * （整页镜像，见 geo_wal.h）。
 *
 * fn == NULL ⇒ 不写日志（单元测试直接用模态时）。
 * ctx —— 钩子自己的上下文（db 层传 DbInstance *）。
 *
 * 必须在这棵树**第一次写页之前**接上 —— 漏接的写点就是没进日志的页。
 */
void rtree_set_wal(RTree *t, LogHookFn fn, void *ctx);

/* 释放树对象。**只释放内存，不释放页**（页归 pager 管）。 */
void rtree_destroy(RTree *t);

/* 根页页号（PAGE_ID_NIL = 空树）。 */
u32 rtree_root_page(const RTree *t);

/* 树高（层数）。叶根 = 1；空树 = 0。 */
u32 rtree_height(const RTree *t);

/* 树里的点数。 */
u32 rtree_npoints(const RTree *t);

/*
 * 逻辑页读计数（本对象自建立起 pager_get 的调用次数）。
 *
 * 这是**逻辑**页读，不是磁盘 IO：同一页取两次算两次。
 * 用途是量"剪枝到底省了多少页访问"（A4 验收）。
 */
u64 rtree_page_reads(const RTree *t);

/*
 * 插入一个点 (row_id, x, y)。
 *
 * 走 ChooseSubtree → 叶子追加 → 溢出则二次分裂 → 父层递归处理，
 * 根分裂则新建根（根页号会变，用 rtree_root_page() 取新的）。
 *
 * 坐标必须是有限值（不是 NaN / Inf）—— NaN 会让所有比较返回
 * false，剪枝会**静默失效**，所以这里直接拒绝。
 *
 * 返回 OK / E_NOSPACE（页或内存不足）/ E_INVAL / E_CORRUPT。
 */
int rtree_insert(RTree *t, u32 row_id, float x, float y);

/*
 * 窗口查询：返回所有落在窗口（含边界）内的 row_id。
 *
 * 剪枝：内部条目的 MBR 与窗口**不相交**（mbr_overlap 为 0）
 * 就整棵子树都不下去看。
 *
 * 命中：叶子条目的 MBR 与窗口相交（mbr_contains_point，
 * **闭区间** —— 边界上的点算命中）。
 *
 * 命中数 > max_out 时返回 E_NOSPACE（**不静默截断**）。
 *
 * 【输出顺序不是 row_id 序】
 *
 * R-tree 的遍历顺序是"树序"，跟 row_id 没关系。
 * 与暴力基准逐位对照前**两边都要各自按 row_id 排好**
 * （用 geo_ids_sort()）：集合全等才是判据，顺序只是整理手段。
 */
int rtree_window(RTree *t, const Mbr *win, u32 *out_ids, u32 max_out, u32 *n_out);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_GEO_RTREE_H */
