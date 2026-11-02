/*
 * graph_am_push.c — 图模态的谓词下推（v0.11 步骤 3）
 *
 * 设计说明见 modalities/graph/graph_am.h。
 *
 * 【下推的两种形态】
 *
 *   graph_edges 的 src / dst 等值 → **真下推**
 *       沿锚点节点的邻接链走一圈。只读那条链，不读整张边表。
 *       行数 = 锚点的度数，而不是全表边数。
 *
 *   graph_nodes 的属性列 → **自滤**
 *       属性值就在记录里，AM 逐行解码自己过滤。
 *       行数 = 满足条件的节点数（和全扫过滤后的行数一样）。
 *
 * 【为什么这两种形态的"下推强度"不同，但都合法】
 *
 * SPI 的下推协议只有一条硬要求：**宁可多给，不能少给**
 * （执行器拿到行之后仍然完整求值一遍 WHERE）。
 *
 * 真下推是"少读"—— 读得更少但读到的都是对的。
 * 自滤是"少给"—— 读得一样多，但不给上层不该看到的行。
 *
 * 自滤是"减法"，所以它的比较语义必须和执行器**逐字一致**，
 * 否则就是静默漏数据（比慢严重得多）。见 ga_match。
 *
 * 【边表为什么不推"范围"】
 *
 * WHERE src > X 想真下推，就得按 src 排序或建索引。
 * v0.11 没有这个结构 —— 硬造一个就是把图模态变成关系模态。
 * 所以只推等值：等值能借邻接链，其他算子老老实实退回全扫。
 *
 * 【节点表为什么不推 node_id 之外的等值到"直接定位"】
 *
 * node_id = X 可以直接定位一个节点（O(1)）。
 * 属性列 = X 只能自筛（O(n)）—— 属性没有索引。
 * 这就是"能不能真下推"取决于**数据结构有没有对应的访问路径**，
 * 而不是取决于谓词长什么样。
 */

#include "modalities/graph/graph_am.h"

#include <stdlib.h>
#include <string.h>

/* ---------------- 节点 id ↔ RID ---------------- */

/*
 * 节点 id → RID。
 *
 * 节点 id = rid_to_u64(RID) = (页号 << 16) | 槽号。
 *
 * 【为什么必须做往返校验】
 *
 * rid_from_u64 对 v >= 2^48 会截断（页号只取 u32）。
 * 如果不校验，一个越界的 id 会被"截"成一个看起来合法的节点，
 * 然后下推出**错误的行** —— 静默数据错误。
 *
 * 所以：反解之后必须 rid_to_u64(rid_from_u64(v)) == v。
 * 不成立就是非法 id，调用方据此返回 0 行（不是报错）。
 */
int ga_num_to_rid(i64 num, Rid *out)
{
    if (!out)
        return E_INVAL;
    if (num < 0)
        return E_INVAL;

    u64 v = (u64)num;
    Rid r = rid_from_u64(v);

    /* 往返校验：截断过的 id 到不了这一步 */
    if (rid_to_u64(r) != v)
        return E_INVAL;

    *out = r;
    return OK;
}

int ga_nodeid_to_rid(const DbValue *v, Rid *out)
{
    if (!v || !out)
        return E_INVAL;
    if (v->is_null)
        return E_INVAL;

    /*
     * 只接受整型 id。
     *
     * 节点 id 是 u64 编码的 RID，但它在 SQL 层是 INT8。
     * 文本 id（"123"）不认 —— 静默把文本转成数字就是猜用户的意思。
     */
    if (v->type != T_INT4 && v->type != T_INT8)
        return E_INVAL;

    return ga_num_to_rid(v->i, out);
}

/* ---------------- 比较语义 ---------------- */

/*
 * 精确复刻执行器的比较（query/exec_scan.c 的 eval_expr 比较分支）。
 *
 * 【为什么必须逐字一致】
 *
 * 自滤会**丢掉**不满足谓词的行。
 * 如果这里的语义和执行器有一丁点差别（比如 NULL 的处理），
 * 丢掉的行就是执行器本来会返回的行 —— 静默漏数据。
 *
 * 执行器的规则：
 *   1. 任一侧为 NULL → 结果是 NULL → 行不匹配（val_truth(NULL) = -1 ≠ 1）
 *   2. 两侧都是文本 → 按字节比，长度不同再比长度
 *   3. 其余 → 按数值比
 *
 * op 的编码和执行器逐字对应：
 *   '=' → c == 0    '<' → c < 0    '>' → c > 0
 *   'L' → c <= 0   'G' → c >= 0   'N' → c != 0
 */
int ga_match(int op, const DbValue *a, const DbValue *b, bool *matched)
{
    if (!a || !b || !matched)
        return E_INVAL;

    bool isnull = false;
    int cerr = OK;
    int c = val_cmp(a, b, &isnull, &cerr);

    if (cerr != OK)
        return cerr;         /* 外部值物化失败：明确报错，不给错答案 */

    /* NULL 比较 → NULL → 不是 TRUE → 行不匹配 */
    if (isnull) {
        *matched = false;
        return OK;
    }

    switch (op) {
    case PD_EQ: *matched = (c == 0); break;
    case PD_LT: *matched = (c < 0);  break;
    case PD_GT: *matched = (c > 0);  break;
    case PD_LE: *matched = (c <= 0); break;
    case PD_GE: *matched = (c >= 0); break;
    case PD_NE: *matched = (c != 0); break;
    default:    return E_INVAL;
    }
    return OK;
}

/* ---------------- 能不能推 ---------------- */

/*
 * 模态说"这个谓词我能处理吗"。
 *
 * 返回 OK 表示可以（随后调 push），E_NOTSUP 表示不行
 * —— 执行器会退回全表扫描（open_scan 的逻辑）。
 *
 * 【判据从紧】
 *
 * 下推错了会静默少数据。所以只接：
 *   - 边表的 src / dst 等值（走邻接链）
 *   - 节点表的 node_id 等值（直接定位一个节点）
 *   - 节点表的属性列比较（自滤）
 *
 * 范围比较在边表上推不了（没有按 src 排序的结构）→ E_NOTSUP。
 */

static bool ga_is_int_value(const DbValue *v)
{
    return v && !v->is_null && (v->type == T_INT4 || v->type == T_INT8);
}

int graph_am_can_push(StorageAM *am, const Predicate *p)
{
    GraphAmCtx *c = ga_ctx_of(am);
    if (!c || !p)
        return E_NOTSUP;

    if (p->col_idx < 0 || p->col_idx >= c->ncols)
        return E_NOTSUP;

    if (c->is_nodes) {
        /* node_id（第 0 列）等值 → 直接定位一个节点 */
        if (p->col_idx == 0) {
            if (p->op != PD_EQ)
                return E_NOTSUP;
            if (!ga_is_int_value(&p->val))
                return E_NOTSUP;
            return OK;
        }

        /* 属性列 → 自滤（比较语义与执行器一致，任何算子都能接） */
        switch (p->op) {
        case PD_EQ:
        case PD_LT:
        case PD_GT:
        case PD_LE:
        case PD_GE:
            return OK;
        default:
            return E_NOTSUP;
        }
    }

    /* 边表：只有 src / dst 的等值能真下推 */
    if (p->op != PD_EQ)
        return E_NOTSUP;
    if (!ga_is_int_value(&p->val))
        return E_NOTSUP;
    return OK;
}

/* ---------------- 下推 ---------------- */

/*
 * 把谓词交给模态，模态返回一个行源。
 *
 * 行源和全表扫描同构：scan_next 给 (rid, 字节串)。
 * 于是执行器只有一种取行路径 —— 不需要为下推新增算子类型。
 */
int graph_am_push(StorageAM *am, const Predicate *p, AmScan **out)
{
    GraphAmCtx *c = ga_ctx_of(am);
    if (!c || !p || !out)
        return E_INVAL;

    GraphScan *st = calloc(1, sizeof(*st));
    if (!st)
        return E_IO;

    st->ctx = c;

    if (c->is_nodes) {
        if (p->col_idx == 0) {
            /* node_id = X → 直接定位那一个节点 */
            Rid r;
            if (ga_num_to_rid(p->val.i, &r) != OK) {
                /*
                 * 非法 id：没有任何节点能匹配它。
                 *
                 * 注意这里返回**空扫描**而不是 E_NOTSUP：
                 * 退回全扫也只是白扫一遍，结果还是 0 行。
                 * 空扫描把"0 行"这件事表达得更直接。
                 */
                st->mode = GA_MODE_EMPTY;
                *out = (AmScan *)st;
                return OK;
            }
            st->mode = GA_MODE_PUSH_NODE_ONE;
            st->one = r;
            *out = (AmScan *)st;
            return OK;
        }

        /* 属性列 → 全扫 + 自滤 */
        st->mode = GA_MODE_PUSH_NODE_FILTER;
        st->fcol = p->col_idx - 1;    /* 表列号 → 属性下标 */
        st->fop = p->op;
        val_copy(&st->fval, &p->val);
        gstore_cursor_init(c->g->nodes, &st->cur);
        *out = (AmScan *)st;
        return OK;
    }

    /* 边表：src / dst 等值 → 沿锚点节点的邻接链走 */
    Rid node;
    if (ga_num_to_rid(p->val.i, &node) != OK) {
        st->mode = GA_MODE_EMPTY;
        *out = (AmScan *)st;
        return OK;
    }

    /*
     * src = X → X 的**出链**（X 出发的边）
     * dst = X → X 的**入链**（指向 X 的边）
     *
     * 边表的两列是 src（第 0 列）和 dst（第 1 列）。
     */
    bool out_dir = (p->col_idx == 0);

    int rc = graph_edge_cursor_open(c->g, node, out_dir, &st->ec);
    if (rc == E_NOTFOUND) {
        /*
         * 锚点节点不存在 → 没有任何边的端点是它 → 0 行。
         *
         * 这不是错误：一个不存在的节点不可能是边的端点。
         * （图层保证边不会悬空 —— 加边时端点必须存在。）
         */
        st->mode = GA_MODE_EMPTY;
        *out = (AmScan *)st;
        return OK;
    }
    if (rc != OK) {
        free(st);
        return rc;
    }

    st->mode = GA_MODE_PUSH_EDGE_CHAIN;
    *out = (AmScan *)st;
    return OK;
}
