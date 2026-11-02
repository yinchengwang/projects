/*
 * heap_am_batch.c — 关系模态的批接口（v1.2 步骤 0）
 *
 * ==================== 这一版要证明什么 ====================
 *
 * v1.1 把执行器批式化了，但 A2（比火山快 ≥ 3 倍）只拿到 1.11×。
 * 实测拆开：逐行 scan_next 占 38%，逐行 decode 占 43%。
 *
 * **瓶颈在 SPI 的逐行接口，不在求值。**
 *
 * 所以本文件给关系模态实现两个可选能力：
 *   - AM_CAP_BATCH_SCAN   一次给一批原始行
 *   - AM_CAP_BATCH_DECODE 一次解一批，直接进列数组（不物化 DbValue）
 *
 * ==================== 为什么只有 heap 实现 ====================
 *
 * PLAN-v1.2 §二 步骤 0 明说：**blob / graph / vector 一行都不用改** ✓
 *
 * 理由：
 *   - heap 有 RecLayout（记录布局），批量解码不用每行重算布局
 *   - 其它模态的行格式各不相同，实现批解码的收益/复杂度比不划算
 *   - 而"指针为 NULL = 不支持"的机制让它们**自动**退回逐行路径
 *
 * 换句话说：批能力是**可选加速**，不是**正确性前提**。
 * 这正是能力协商机制的价值 —— 加能力不影响没加能力的模态。
 */

#include "spi/heap_am_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---------------- 批扫描 ---------------- */

/*
 * 一次给一批原始行。
 *
 * ==================== 全表扫：零拷贝 ====================
 *
 * 全表扫的行本来就躺在**页里**（堆表行内联在页中）。
 * 逐行路径每行要做的事是：
 *     scan_next → scan_next_row → heap_scan_next
 *              → page_get（一次哈希查找 + 一次函数调用）→ memcpy
 * 10 万行 = 50 万次函数调用，而这里面**没有一个字节是必须拷的**。
 *
 * 所以全表扫直接**零拷贝**：行指针指向页内，页保持 pin 住。
 * 有效期的边界就是"下一次 batch_scan"—— 见下。
 *
 * 【为什么零拷贝更快的机理】
 * 实测同一台机器、同一份数据：
 *     页 → 竞技场（一次 memcpy）  2.13 ms
 *     页 → 4 KB 中转 → 竞技场    2.88 ms   ← 更慢
 *     零拷贝（不拷）             2.10 ms
 * 页缓冲是 4 KB 对齐、竞技场是 256 KB 对齐，两者的低 12 位常常相同，
 * 于是"从页里读、向竞技场写"撞上 **4K 别名（store-to-load forwarding 误判）**：
 * CPU 的内存消歧器认为读依赖写，每次都要多等一拍。
 * 不拷就没有这个依赖。
 *
 * ==================== 零拷贝的安全边界 ====================
 *
 * 行指针指向页内，所以**页必须在使用期间保持 pin**。
 * heap_scan_page_batch 保持 s->cur 不释放，直到：
 *   - 下一次调用（换页时释放旧页）
 *   - heap_scan_end
 * 调用方的约定（写在 storage/batch.h 的契约里）：
 * **行字节的有效期到下一次 batch_scan 调用为止。**
 *
 * 目前页的 pin 由全局 latch 兜着（整条语句期间不换页）；
 * v1.2 步骤 4 引入细粒度 latch 后，解码窗口必须被页 latch 覆盖 ——
 * 这一点记在 PLAN-v1.2 §二 步骤 4 的前置条件里。
 *
 * ==================== 索引下推：仍然逐行取 ====================
 *
 * 下推路径的行来自"游标给 RID → 回表取行"，取到的行在页里**不连续**，
 * 零拷贝不成立；而且命中的行本来就少，逐行取的绝对开销很小。
 * 所以下推走竞技场（每行一次 memcpy），全表扫走零拷贝 —— 两条路各取所长。
 *
 * ==================== 为什么不清零竞技场 ====================
 *
 * 清零是 O(竞技场大小) 的写。批扫描是热路径，1024 行一批、
 * 每批清 256 KB 就是纯浪费。竞技场是"用多少填多少"的，
 * 重置只要把 arena_used 归零 —— 旧数据被覆盖不影响正确性，
 * 因为每行的长度都单独记着。
 */
int heap_am_batch_scan(StorageAM *am, AmScan *s, AmRowRef *rows, int cap, int *n_out)
{
    HeapAmCtx *c = (HeapAmCtx *)am->impl;
    if (!c || !s || !rows || !n_out)
        return E_INVAL;

    *n_out = 0;
    if (cap <= 0)
        return OK;

    HeapScanState *st = (HeapScanState *)s;

    /* ---- 全表扫：零拷贝 ---- */
    if (st->kind == SCAN_FULL) {
        int n = 0;
        int rc = heap_scan_page_batch(st->full, rows, cap, &n);
        *n_out = n;
        if (rc == E_EOF)
            return (n > 0) ? OK : E_EOF;
        return rc;
    }

    /* ---- 索引下推：竞技场 ---- */

    /* 竞技场：惰性分配（只有走下推批路径的查询才付这 256 KB） */
    if (!st->arena) {
        st->arena = malloc(AM_BATCH_ARENA_BYTES);
        if (!st->arena)
            return E_IO;
        st->arena_cap = AM_BATCH_ARENA_BYTES;
    }

    /* 新的一批：从头码 */
    st->arena_used = 0;

    /*
     * 【为什么这里要留 PAGE_SIZE 的余量】
     * 下面这行是"取一行"—— 取出来就已经消费掉了。
     * 如果竞技场装不下而我们仍然取，**这一行就丢了**（游标不会回退）。
     * 所以取之前先确认"至少还剩 PAGE_SIZE"，装不下就到此为止。
     */
    u8 one[PAGE_SIZE];
    u16 len = 0;
    Rid rid;

    int n = 0;
    for (;;) {
        if (st->arena_used + PAGE_SIZE > st->arena_cap)
            break;                      /* 竞技场装不下下一行：这一批到此为止 */

        int rc = heap_am_scan_next_row(c, s, &rid, one, PAGE_SIZE, &len);
        if (rc != OK) {
            *n_out = n;
            return (rc == E_EOF && n == 0) ? E_EOF : OK;
        }

        memcpy(st->arena + st->arena_used, one, len);

        rows[n].data = st->arena + st->arena_used;
        rows[n].len = len;
        rows[n].rid = rid;
        st->arena_used += (u32)len;
        n++;

        if (n >= cap)
            break;
    }

    *n_out = n;
    return OK;
}

/* ---------------- 批解码 ---------------- */

/*
 * 把一批原始行直接解进列数组。
 *
 * 解码本身在 storage/batch.c（val_batch_decode）——
 * 放在 storage/ 是因为它是"记录字节 → 列"的通用转换，
 * 与"谁提供记录"无关。heap 只是提供布局与列定义。
 *
 * 注意：解出来的值**不指向页**（文本拷进列自己的文本池），
 * 所以解码完成后行字节就可以还给 AM 了。
 */
int heap_am_batch_decode(StorageAM *am, const AmRowRef *rows, int nrows, AmBatch *out)
{
    HeapAmCtx *c = (HeapAmCtx *)am->impl;
    if (!c)
        return E_INVAL;

    /* 用预计算的布局 —— 不每行重算（见 record.h 的 RecLayout 说明） */
    return val_batch_decode(&c->layout, c->cols, c->ncols, rows, nrows, out);
}
