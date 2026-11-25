/*
 * json_ovf_reclaim.c —— 启动时的孤儿 JSON 页链回收（C5.3b-b Task 6）
 *
 * ==================== 它在解决什么 ====================
 *
 * 崩溃/abort 会留下三类"没人引用的链"（设计 §8）：
 *
 *   A. 链建好了、tuple 没写成（INSERT 的窗口）
 *   B. 运行时 abort 后留下的链（§6.4 的取舍：宁可浪费空间）
 *   C. undo 把页前像（全 0）盖回去之后的页
 *
 * 这三类页的共同点：**页头还是 PT_JSON_OVF（或分配器仍记着它在用），
 * 但没有任何 tuple 的 12 字节溢出标记指着它的首页**。
 *
 * ==================== 为什么引用集合由调用方传进来 ====================
 *
 * "哪个 tuple 引用了哪条链"只有 catalog/heap 层知道。若本文件自己去
 * 扫表，就是 L4 → L2 的反向依赖（tools/check_deps.sh 会直接红）。
 *
 * 所以 db 层（catalog 打开后）先遍历所有关系表的 heap，把所有 JSOV 标记的
 * chain_head 收成一个集合，再把集合交给这里。本文件只做两件事：
 *
 *   1. 遍历页空间，找出页头是 PT_JSON_OVF 的页；
 *   2. 对每条链的**首页**，判断它是否在引用集合里 —— 不在就整条回收。
 *
 * ==================== 为什么按"首页"判断，而不是按"每一页" ====================
 *
 * 设计 §8 明确否决了"只沿 next 走一遍、剩下没人到达的就算孤儿"的做法：
 * 那只发现得了"整条链都是孤儿"，发现不了"首页被引用、后续页是孤儿"的混合情况。
 *
 * 而按首页判断需要知道"哪些页是链的首页"。判据：**没有任何 PT_JSON_OVF 页的
 * next 指向它**。先在页空间上标出"被 next 指向过的页"，剩下的 PT_JSON_OVF 页
 * 就是首页候选。
 *
 * ⚠️ 混合链（首页被引用、后续页是孤儿）在本算法下**不会被释放** ——
 * 因为回收的单位是整条链，而链是活的。这不是漏回收，而是安全的选择：
 * 释放活链的中间页会让活 tuple 读到断链（数据损坏），代价远大于几页空间。
 * 真正的"孤儿后续页"只会出现在链被改坏的情况下，那时读路径会报 E_CORRUPT。
 *
 * ==================== 幂等 ====================
 *
 * json_ovf_free 本身幂等（首页不是 PT_JSON_OVF ⇒ 空操作），所以重复执行
 * 本函数不会 double-free。启动时只有一个连接，不存在并发 VACUUM。
 */

#include "modalities/doc/json_ovf_reclaim.h"

#include <stdlib.h>
#include <string.h>

#include "modalities/doc/json_ovf.h"
#include "storage/page.h"

/*
 * 在引用集合里线性查找。
 *
 * 【为什么不做成哈希集合】
 * 引用数 = 库里"带大 JSON 的行数"。启动路径上这个数通常很小，
 * 而建哈希表要额外分配 + 处理冲突；先线性查，等真成瓶颈再说。
 * 复杂度写在这里而不是藏着：O(链数 × 引用数)。
 */
static bool head_is_referenced(const u64 *refs, u32 nrefs, u32 head)
{
    for (u32 i = 0; i < nrefs; i++) {
        if (refs[i] == (u64)head)
            return true;
    }
    return false;
}

int json_ovf_reclaim_orphans(Pager *pg, const u64 *referenced_heads, u32 nrefs,
                             u32 *freed_pages_out)
{
    if (!pg)
        return E_INVAL;
    if (nrefs > 0 && !referenced_heads)
        return E_INVAL;

    if (freed_pages_out)
        *freed_pages_out = 0;

    u32 npages = (u32)pager_npages(pg);
    if (npages == 0)
        return OK;

    /*
     * 两遍扫：
     *   第一遍：标出"被某条链的 next 指向过"的页 ⇒ 它们不是首页。
     *   第二遍：对每个 PT_JSON_OVF 且未被 next 指向的页，判断是否被引用。
     *
     * 标记数组用堆分配：页空间可能很大，栈上放不下（PAGE_SIZE 都是 4K，
     * 一页一个 u8 ⇒ 1GB 库要 256KB，栈上不行）。
     */
    u8 *pointed_to = calloc((size_t)npages, 1);
    if (!pointed_to)
        return E_IO;

    u32 scan_failed = 0;

    for (u32 pid = 0; pid < npages; pid++) {
        PageFrame *fr = NULL;
        if (pager_get(pg, pid, &fr) != OK) {
            scan_failed++;
            continue;
        }
        if (fr->data[H_TYPE] == (u8)PT_JSON_OVF &&
            page_verify_checksum(fr->data)) {
            u32 next = get_u32(fr->data + JSON_OVF_NEXT_OFF);
            if (next != PAGE_ID_NIL && next < npages)
                pointed_to[next] = 1;
        }
        (void)pager_unpin(pg, pid, false);
    }

    u32 freed = 0;

    for (u32 pid = 0; pid < npages; pid++) {
        if (pointed_to[pid])
            continue;       /* 是链的中间页，归属它前面那个首页 */

        PageFrame *fr = NULL;
        if (pager_get(pg, pid, &fr) != OK) {
            scan_failed++;
            continue;
        }
        bool ours = (fr->data[H_TYPE] == (u8)PT_JSON_OVF &&
                     page_verify_checksum(fr->data));
        (void)pager_unpin(pg, pid, false);
        if (!ours)
            continue;

        if (head_is_referenced(referenced_heads, nrefs, pid))
            continue;       /* 活链：tuple 还指着它，一个页都不许动 */

        /*
         * 孤儿链。它可能已经被 recovery 的 undo 弄成"首页还在、
         * 中间某页全 0"的断链 —— json_ovf_free 遇到这种会返回 E_CORRUPT
         * 且**一个页都不释放**（它的损坏链保护）。
         *
         * 这里的分工：断链的页留给后面的人工/工具处理，本函数不强行还页。
         * 强行按"首页 + next 链"还页会把"页还可能被别人用"的情况也吞掉。
         */
        u32 before = pager_free_count(pg);
        int rc = json_ovf_free(pg, (u64)pid);

        /*
         * 首页已经不是 PT_JSON_OVF ⇒ json_ovf_free 走空操作返回 OK。
         * 这不算回收（但那页本来就不该被算成孤儿链）。
         */
        if (rc == OK)
            freed += pager_free_count(pg) - before;
    }

    free(pointed_to);

    if (freed_pages_out)
        *freed_pages_out = freed;

    /*
     * 【为什么"取页失败"不升级成整体失败】
     *
     * 设计 §8/§6.4 的取舍：宁可浪费空间，不可阻塞开库。
     * 取不到某页最多是"这条链这次没回收"，下次开库还有机会。
     */
    (void)scan_failed;
    return OK;
}
