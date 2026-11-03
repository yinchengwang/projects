/*
 * ft_index_view.c —— 索引的**只读访问器**（v1.3 步骤 4）
 *
 * 为什么单独一个文件：ft_index.c 已经贴着 800 行的硬上限，
 * 访问器放进来会顶破它；而访问器要读索引的私有字段，
 * 就得能看到布局（ft_index_internal.h）。
 *
 * 【这两个函数是纯只读的】
 *
 * 不改索引的任何内容、不动页读统计、不加锁（调用方自己持锁）。
 * 它们存在的唯一理由是：**别的模态文件不许碰索引的内部结构**，
 * 但 ft_docs 虚拟表需要"逐段"看一眼。
 */

#include "modalities/ft/ft_index.h"

#include "modalities/ft/ft_index_internal.h"

int ft_index_seg_at(const FtIndex *ix, u32 i, const FtSegment **out)
{
    if (ix == NULL || out == NULL) {
        return E_INVAL;
    }
    *out = NULL;
    if (i >= ix->n_segs) {
        return E_NOTFOUND;   /* 没有那么多段 —— 不是错误 */
    }
    *out = ix->segs[i];
    return OK;
}

/*
 * doc_id 是否在 tombstone 集合里。
 *
 * 集合按升序维护（ft_index.h 里说明了为什么），所以用二分。
 * 不用哈希：集合只增不减、查的次数很少（每篇文档查一次），
 * 二分已经足够，而且**不引入第三份状态**。
 */
int ft_index_is_tomb(const FtIndex *ix, u32 doc_id)
{
    u32 lo;
    u32 hi;

    if (ix == NULL) {
        return 0;
    }

    lo = 0;
    hi = ix->n_tombs;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;

        if (ix->tomb[mid] < doc_id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < ix->n_tombs && ix->tomb[lo] == doc_id) {
        return 1;
    }
    return 0;
}
