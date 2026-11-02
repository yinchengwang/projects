/*
 * graph_schema.c —— 图的属性 schema 的持久化（v0.11 步骤 4）
 *
 * 设计说明见 modalities/graph/graph.h。
 *
 * 【为什么 schema 必须落盘】
 *
 * 节点记录是**定长**的，长度 = 公共头 + 位图 + 各列宽度之和。
 * 也就是说：记录里没有任何"这一列多宽"的信息 —— 宽度只存在于 schema 里。
 *
 * 所以重启之后，如果没有 schema：
 *   - 不知道该用多大的 rec_size 去解释节点页 → gstore_recover 直接 E_CORRUPT
 *   - 就算硬解出来，也不知道第 20+ 字节之后哪 4 字节是 int、哪 8 字节是 bigint
 *
 * 结论：**schema 是图的一部分，不是"建图时的一次性参数"**。
 * 它必须和节点记录同生共死。
 *
 * 【为什么用独立 gstore，而不是现成的三种办法】
 *
 *   (a) 放进节点记录  → 每条节点记录都存一份 schema = 纯浪费；
 *                      而且改 schema 会改所有记录的长度（定长槽页直接废掉）。
 *   (b) 放进 catalog  → 硬规则 6：catalog 不知道任何模态。
 *                      图的属性 schema 是图自己的事。
 *   (c) 放进内存      → 重启就没了 —— 正是本文件要解决的问题。
 *
 * 剩下两条路：独立页，或者独立 gstore。选 gstore 的理由是**零新代码**：
 * 页的分配、WAL（整页镜像）、重启接管（扫全文件找 GS_FLAG_DIR 链头）
 * 全是 gstore 现成的能力，而且已经被 test_gstore / test_graph 验过。
 *
 * 【记录格式】
 *
 *   [0]  u8   version
 *   [1]  u8   ncols
 *   [2]  u16  node_size      ← 节点记录总长，交叉校验用
 *   [4]  每列 36 字节：
 *          name[32]（NUL 填充的定长名）
 *          type(1) + width(2) + nullable(1)
 *
 * 总长 = 4 + 15 × 36 = 544 字节（定长，所以能进定长槽页）。
 *
 * 【为什么显式打包而不直接 memcpy(struct)】
 *
 * 直接 memcpy 一个结构体，磁盘格式就绑死了编译器的对齐和填充规则：
 * 换一个编译器、换一个对齐设置、或者有人在结构体中间插一个字段，
 * 老库里的 schema 就全解错了 —— 而且**不报错**（解出来的宽度乱七八糟，
 * 但每一项都"看起来合法"）。
 *
 * 显式打包的代价是几行 put_u16/memcpy，
 * 换来的是：磁盘格式由这份文件单独定义，改结构体不会影响它。
 * 这是所有持久化格式都该付的代价。
 *
 * 【node_size 为什么要冗余存一份】
 *
 * 因为列宽算出来的记录长度，和磁盘上实际存着的记录长度，
 * 是两个**独立来源**的事实。存下来做交叉校验：
 * 一旦对不上，说明 schema 记录和节点页不是同一套布局
 * （比如两次建图用了不同的 schema）→ E_CORRUPT，而不是读出垃圾。
 */

#include "modalities/graph/graph.h"

#include <string.h>

/* ---------------- 格式常量 ---------------- */

/* GR_SCHEMA_* 的定义在 graph.h —— 定义必须只有一处（见那里的说明） */

/* 列内字段偏移（相对列起始） */
enum {
    GR_SC_NAME  = 0,
    GR_SC_TYPE  = COL_NAME_MAX,
    GR_SC_WIDTH = COL_NAME_MAX + 1,
    GR_SC_NULL  = COL_NAME_MAX + 3
};

/* ---------------- 编解码 ---------------- */

static void gr_schema_encode(u8 *buf, const GraphCol *cols, int ncols,
                              u16 node_size)
{
    memset(buf, 0, GR_SCHEMA_REC_SIZE);

    /* 数据写在载荷区（公共头由 gstore 管） */
    u8 *pay = buf + GS_REC_HDR_SIZE;

    pay[0] = GR_SCHEMA_VER;
    pay[1] = (u8)ncols;
    put_u16(pay + 2, node_size);

    for (int i = 0; i < ncols; i++) {
        u8 *p = pay + GR_SCHEMA_HEAD + i * GR_SCHEMA_COL;

        /* 名字：定长拷贝，剩余字节补 0（读回来靠 NUL 截尾） */
        memcpy(p + GR_SC_NAME, cols[i].name, COL_NAME_MAX);
        p[GR_SC_NAME + COL_NAME_MAX - 1] = '\0';

        p[GR_SC_TYPE] = cols[i].type;
        put_u16(p + GR_SC_WIDTH, cols[i].width);
        p[GR_SC_NULL] = cols[i].nullable ? 1 : 0;
    }
}

/*
 * 解码。返回 E_CORRUPT 表示磁盘上的 schema 记录看不懂
 * （版本不对 / 列数越界 / 类型不认识）。
 */
static int gr_schema_decode(const u8 *buf, GraphCol *cols, int max,
                            int *ncols, u16 *node_size)
{
    const u8 *pay = buf + GS_REC_HDR_SIZE;

    if (pay[0] != GR_SCHEMA_VER)
        return E_CORRUPT;

    int n = pay[1];
    if (n > GR_MAX_COLS || n > max)
        return E_CORRUPT;

    u16 nsize = get_u16(pay + 2);

    for (int i = 0; i < n; i++) {
        const u8 *p = pay + GR_SCHEMA_HEAD + i * GR_SCHEMA_COL;

        memcpy(cols[i].name, p + GR_SC_NAME, COL_NAME_MAX);
        cols[i].name[COL_NAME_MAX - 1] = '\0';

        cols[i].type = p[GR_SC_TYPE];
        cols[i].width = get_u16(p + GR_SC_WIDTH);
        cols[i].nullable = (p[GR_SC_NULL] != 0);

        /* 类型必须是属性系统支持的那几种 */
        switch (cols[i].type) {
        case T_INT4:
        case T_INT8:
        case T_BOOL:
            break;
        case T_TEXT:
            if (cols[i].width == 0)
                return E_CORRUPT;
            break;
        default:
            return E_CORRUPT;
        }
    }

    *ncols = n;
    *node_size = nsize;
    return OK;
}

/* ---------------- 读 ---------------- */

/*
 * 从 schema gstore 里读出 schema。
 *
 * 没有记录 → E_NOTFOUND（"还没建图"）。
 */
int gr_schema_read(GStore *gs, GraphCol *cols, int max, int *ncols, u16 *node_size)
{
    if (!gs || !cols || !ncols || !node_size)
        return E_INVAL;

    u8 buf[GR_SCHEMA_REC_SIZE];

    GStoreCursor c;
    gstore_cursor_init(gs, &c);

    Rid rid;
    u16 len = 0;
    int rc = gstore_cursor_next(gs, &c, &rid, buf, sizeof(buf), &len);
    if (rc == E_EOF)
        return E_NOTFOUND;     /* 没有 schema = 还没建图 */
    if (rc != OK)
        return rc;

    return gr_schema_decode(buf, cols, max, ncols, node_size);
}

/* ---------------- 写 ---------------- */

/*
 * 把 schema 写进 schema gstore。
 *
 * 已有记录 → **原地覆盖**（不新开一条）。
 * 为什么不能每次新开一条：schema gstore 是"读第一条"的语义，
 * 多写一条会让"哪条是真的"变得不确定 —— 而 schema 必须是唯一的。
 */
int gr_schema_write(GStore *gs, const GraphCol *cols, int ncols, u16 node_size)
{
    if (!gs)
        return E_INVAL;
    if (ncols < 0 || ncols > GR_MAX_COLS)
        return E_INVAL;
    if (ncols > 0 && !cols)
        return E_INVAL;

    u8 buf[GR_SCHEMA_REC_SIZE];
    gr_schema_encode(buf, cols, ncols, node_size);

    u32 head = gstore_head_page(gs);
    if (head != PAGE_ID_NIL) {
        Rid r = rid_make(head, 0);
        u8 probe[GR_SCHEMA_REC_SIZE];
        u16 len = 0;
        int rc = gstore_get(gs, r, probe, sizeof(probe), &len);
        if (rc == OK)
            return gstore_put(gs, 0, r, buf, GR_SCHEMA_REC_SIZE);
        if (rc != E_NOTFOUND)
            return rc;
    }

    /* xid = 0（系统事务）：图走语句级提交，见 PLAN-v0.11 §1.4 */
    Rid r;
    return gstore_alloc_with(gs, 0, buf, GR_SCHEMA_REC_SIZE, &r);
}

/* ---------------- 比对 ---------------- */

/*
 * 两个 schema 是否完全一致。
 *
 * 【为什么要逐字段比，而不是只比 node_size】
 *
 * node_size 相同不代表 schema 相同：
 *   列 A: INT4 + INT4   → 8 字节
 *   列 B: INT8          → 8 字节
 * 长度一样，但布局完全不同 —— 用 B 去解释 A 的记录，读出来的是垃圾。
 *
 * 所以必须逐列比：名字、类型、宽度、可空性。
 */
int gr_schema_equal(const GraphCol *a, int na, u16 sa,
                    const GraphCol *b, int nb, u16 sb)
{
    if (na != nb || sa != sb)
        return 0;

    for (int i = 0; i < na; i++) {
        if (strcmp(a[i].name, b[i].name) != 0)
            return 0;
        if (a[i].type != b[i].type)
            return 0;
        if (a[i].width != b[i].width)
            return 0;
        if (a[i].nullable != b[i].nullable)
            return 0;
    }
    return 1;
}
