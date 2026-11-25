/*
 * db_json_reclaim.c —— 开库时回收无主的 JSON 溢出页链（C5.3b-b Task 6）
 *
 * ==================== 它在解决什么 ====================
 *
 * `db_json_ovf.c` 里写着「链已建、tuple 还没写」的那个窗口里的孤儿
 * 由启动扫描回收（设计 §5.2 / §8）。本文件就是那个回收器的**接线端**。
 *
 * 崩溃 / 运行时 abort 会留下三类"没人引用的链"：
 *
 *   A. 链建好了、tuple 没写成（INSERT 的窗口）
 *   B. 运行时 abort 后留下的链（设计 §6.4：宁可浪费空间，不可破坏数据）
 *   C. undo 把页前像（全 0）盖回去之后的页
 *
 * 它们的共同点：页头还是 PT_JSON_OVF，但**没有任何 tuple 的 12 字节
 * 溢出标记指着它的首页**。
 *
 * ==================== 为什么引用集合在这一层收集 ====================
 *
 * "哪个 tuple 引用了哪条链"只有 catalog + heap + schema 三者凑齐了才知道。
 * 而 `modalities/doc/` 是 L4，按分层铁律看不到 catalog（L2）——
 * 所以回收器 `json_ovf_reclaim_orphans` 只接受一个**页号集合**，
 * 收集工作留在这里（db 层，本来就同时握着 catalog / heap / schema）。
 *
 * 这条分工是设计 §8 写明的：L4 不许反向依赖 catalog。
 *
 * ==================== 为什么扫 tuple 而不是只扫页 ====================
 *
 * 只沿页链的 next 走一遍、把"没被到达过"的页当孤儿，只能发现
 * "整条链都是孤儿"的情况，发现不了"首页被引用、后续页被孤儿化"的混合链。
 * 设计 §8 因此明确选择**扫 tuple**（准确），接受启动时 O(tuple 数) 的开销。
 *
 * ==================== 失败语义：只告警，不阻塞开库 ====================
 *
 * 设计 §8 / §11 的裁决：回收失败最多是"这次没能把空间要回来"
 * （下次开库还有机会），不是数据错误。所以本文件的入口**永不**返回
 * 会让 `db_open` 失败的错误码 —— 全部折成 OK + 一条 loghook 诊断。
 *
 * 窗口期必须解释清楚：回收器跑在 `catalog_open` **之后**、
 * 模态注册**之前**。此时还没有任何连接能读到 JSON 值，
 * 因此"释放链"与"读链"在这条路径上天然互斥 —— 不会读到刚被回收的页。
 */

#include "db_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog/catalog.h"
#include "modalities/doc/json_ovf.h"
#include "modalities/doc/json_ovf_reclaim.h"
#include "storage/heap.h"
#include "storage/record.h"

/*
 * 引用集合的初始容量。
 *
 * 【为什么给一个初值而不是从 0 开始翻倍】
 * 没有大 JSON 的库（绝大多数）走的是"扫完发现一个引用都没有"的快路径，
 * 这时一次 malloc(64) 比反复 realloc 便宜，也避免 0 容量时的边界推断。
 */
#define RECLAIM_REFS_INIT 64

typedef struct {
    u64 *heads;
    u32  n;
    u32  cap;
    bool oom;       /* 扩容失败 ⇒ 整个收集作废（宁可这次不回收） */
} HeadSet;

/*
 * 已经收过这个首页了吗。
 *
 * 【为什么要去重】
 * 同一条链可能被多个 tuple 版本引用（UPDATE 之后老版本还活着时不会，
 * 但表达式复制、以及后续可能的链共享会），重复项不影响正确性，
 * 却会让回收器的线性查找变慢 —— 顺手去重比让调用方猜便宜。
 */
static bool headset_has(const HeadSet *s, u64 head)
{
    for (u32 i = 0; i < s->n; i++) {
        if (s->heads[i] == head)
            return true;
    }
    return false;
}

static void headset_add(HeadSet *s, u64 head)
{
    if (s->oom || headset_has(s, head))
        return;

    if (s->n >= s->cap) {
        u32 ncap = s->cap ? s->cap * 2u : RECLAIM_REFS_INIT;
        u64 *nh = realloc(s->heads, (size_t)ncap * sizeof(u64));
        if (!nh) {
            s->oom = true;
            return;
        }
        s->heads = nh;
        s->cap = ncap;
    }
    s->heads[s->n++] = head;
}

/*
 * 从一行记录里挑出所有 JSON 溢出标记的首页。
 *
 * 【为什么用 rec_decode_layout 而不是自己扒字节】
 * "第几个变长列是 T_JSON"这件事由布局算，本来就有预计算版本；
 * 自己扒字节等于把 tuple 布局重新实现一遍，早晚与 record.c 漂移。
 *
 * 解码出来的 T_JSON 变长值是**原始字节**（12 字节标记就是原样 12 字节），
 * 所以这里能做纯字节判别 —— 不需要认识 JSONB 内容。
 */
static void collect_row_heads(const RecLayout *L, const ColDesc *cols,
                              int ncols, const u8 *rec, u16 len, HeadSet *set)
{
    Value vals[REC_MAX_COLS];

    if (rec_decode_layout(L, cols, rec, len, vals) != OK)
        return;

    for (int i = 0; i < ncols; i++) {
        u32 head = 0;
        u32 chain_len = 0;

        if (cols[i].type != T_JSON || vals[i].is_null)
            continue;
        if (!rec_json_ovf_is_marker(vals[i].u.text.p, vals[i].u.text.n))
            continue;

        /*
         * 解不开的标记（head 非法 / chain_len 超 u16）**跳过而不是报错**：
         * 那是一行坏数据，读它的人会拿到 E_CORRUPT —— 但回收器不该
         * 因为一行坏数据就放弃整次扫描。跳过之后这条链会被当成孤儿回收，
         * 这正是我们想要的：坏标记指向的链没人能读，留着只是占地方。
         */
        if (rec_json_ovf_decode(vals[i].u.text.p, vals[i].u.text.n,
                                &head, &chain_len) != OK)
            continue;

        (void)chain_len;        /* 只按首页判归属，长度用不上 */
        headset_add(set, (u64)head);
    }
}

/*
 * 扫一张表，把它的所有 JSON 溢出页链首页收进集合。
 *
 * 【为什么逐表 try 而不是整体失败就退出】
 * 某张表扫不动（页坏了、内存紧）不该让别的表的引用也丢掉 ——
 * 丢掉引用 = 把活链误判成孤儿 = **释放活数据**。
 * 所以这里的错误一律按"这张表没扫到"处理，并在最后统一判定：
 * 只要有任何一张表没扫全，就**整次回收作废**（宁可这次不回收）。
 */
static bool collect_table_heads(Catalog *cat, u32 tid, HeadSet *set)
{
    Heap *h = catalog_heap(cat, tid);
    if (!h)
        return true;            /* 不是关系表（或还没建）⇒ 它没有 tuple */

    ColDesc cols[REC_MAX_COLS];
    int ncols = 0;
    if (catalog_table_cols(cat, tid, cols, REC_MAX_COLS, &ncols) != OK || ncols <= 0)
        return false;

    RecLayout L;
    if (rec_layout(cols, ncols, &L) != OK)
        return false;

    HeapScan *scan = NULL;
    if (heap_scan_begin(h, &scan) != OK)
        return false;

    bool ok = true;
    u8 recbuf[PAGE_SIZE];
    u16 reclen = 0;
    Rid rid;

    int rc;
    while ((rc = heap_scan_next(scan, &rid, recbuf, sizeof(recbuf), &reclen)) == OK) {
        collect_row_heads(&L, cols, ncols, recbuf, reclen, set);
        if (set->oom) {
            ok = false;
            break;
        }
    }
    if (rc != E_EOF && rc != OK)
        ok = false;

    heap_scan_end(scan);
    return ok;
}

/*
 * 回收入口：由 db_open 在 catalog 打开之后调用。
 *
 * 返回 OK 永远是期望值。**不要**把它的返回值当成开库的成败条件 ——
 * 设计 §11 写明"孤儿链扫描失败不阻塞开库"。
 */
int db_reclaim_json_orphans(DbInstance *inst, u32 *freed_pages_out)
{
    if (freed_pages_out)
        *freed_pages_out = 0;
    if (!inst || !inst->pager || !inst->cat)
        return E_INVAL;

    /*
     * 快路径：库里一个 PT_JSON_OVF 页都没有 ⇒ 连 tuple 都不用扫。
     *
     * 【为什么值得单列】
     * 绝大多数库从不写大 JSON，而扫 tuple 是 O(行数)。
     * 设计 §8 明确要求"没有 PT_JSON_OVF 页时快速返回"。
     */
    bool has_ovf_page = false;
    u32 npages = (u32)pager_npages(inst->pager);
    for (u32 pid = 0; pid < npages; pid++) {
        PageFrame *fr = NULL;
        if (pager_get(inst->pager, pid, &fr) != OK)
            continue;
        if (fr->data[H_TYPE] == (u8)PT_JSON_OVF)
            has_ovf_page = true;
        (void)pager_unpin(inst->pager, pid, false);
        if (has_ovf_page)
            break;
    }
    if (!has_ovf_page)
        return OK;

    HeadSet set;
    memset(&set, 0, sizeof(set));

    u32 ntab = catalog_ntables(inst->cat);
    bool all_scanned = true;
    for (u32 i = 0; i < ntab; i++) {
        if (!collect_table_heads(inst->cat, catalog_table_id_at(inst->cat, i), &set)) {
            all_scanned = false;
            break;
        }
    }

    /*
     * 引用集合不完整 ⇒ 不敢回收。
     *
     * 【这条判断为什么不能省】
     * 回收器的判据是"首页不在引用集合里就是孤儿"。集合少了一项，
     * 那条**活链**就会被整条释放 ⇒ 活 tuple 的 JSON 变成断链 ⇒
     * 下次读它报 E_CORRUPT。空间浪费安全，释放活数据不安全。
     */
    if (!all_scanned || set.oom) {
        fprintf(stderr, "[json-ovf] 孤儿回收已跳过：tuple 引用集合没能完整收集"
                        "（宁可这次不回收，也不误释放活链）\n");
        free(set.heads);
        return OK;
    }

    u32 freed = 0;
    int rc = json_ovf_reclaim_orphans(inst->pager, set.heads, set.n, &freed);
    free(set.heads);

    if (rc != OK) {
        fprintf(stderr, "[json-ovf] 孤儿回收失败（不阻塞开库，下次开库再试）\n");
        return OK;
    }

    if (freed_pages_out)
        *freed_pages_out = freed;
    return OK;
}
