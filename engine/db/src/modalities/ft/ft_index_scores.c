/*
 * ft_index_scores.c —— 检索的打分辅助件（从 ft_index.c 拆出）
 *
 * 原 ft_index.c 808 行，超了 800 行硬规则。拆出来的是**与检索主流程无关的
 * 两件纯工具**：
 *
 *   FtIdxScoreMap      (doc_id → 累计分数) 的开放寻址哈希表
 *   ft_idx_hit_cmp     结果排序比较器（分数降序、同分按 doc_id 升序）
 *
 * 为什么这两件值得单独一个文件：
 *
 * 它们都是"**一旦写错就静默错**"的那类代码：
 *   - 哈希表用错装填因子 → 退化成 O(n²)，十万文档直接卡死
 *   - 比较器写成 `a->score - b->score` → 浮点误差下排序不稳定，同分文档顺序乱飘
 *
 * 归一处分、贴近它们各自的注释（坑 M4-11），比埋在检索主流程中间容易被改错。
 *
 * ⚠️ 这两个符号**只在本模态内使用**（ft_index.c 的检索主流程），
 * 不导出到 ft_index.h —— 所以它们留在本文件、由 ft_index.c include。
 */

#include "modalities/ft/ft_index_internal.h"

#include <stdlib.h>
#include <string.h>

#include "common/error.h"

/* ---------------- 分数累加表（开放寻址哈希） ---------------- */

/* ---------------- 分数累加表（开放寻址哈希） ---------------- */

/*
 * (doc_id → 累计分数) 的开放寻址哈希表。
 *
 * 命中文档的 doc_id 由调用方给、不连续，数量事先未知：用数组浪费，用线性查找
 * 就是 O(n²)（十万文档直接卡死）。容量取 2 的幂，装填因子 <= 0.5，Knuth 乘法散列。
 * 【doc_id 可以是任意值】—— 不能拿 doc_id == 0 当"空槽"标记，故另设 used 标志。
 *
 * 结构体与函数声明在 ft_index_internal.h（ft_index.c 也要看到），这里只有实现。
 */

static u32 ft_idx_map_hash(u32 doc_id)
{
    return doc_id * 2654435761u;   /* Knuth 乘法散列 */
}
void ft_idx_map_init(FtIdxScoreMap *m)
{
    memset(m, 0, sizeof(*m));
}
void ft_idx_map_free(FtIdxScoreMap *m)
{
    free(m->slots);
    memset(m, 0, sizeof(*m));
}
/* 容量翻倍并把老条目重插进去 */
static int ft_idx_map_grow(FtIdxScoreMap *m)
{
    u32 ncap = m->cap * 2;
    FtIdxSlot *ns = (FtIdxSlot *)calloc(ncap, sizeof(FtIdxSlot));
    u32 i;

    if (!ns) {
        return E_NOSPACE;
    }
    for (i = 0; i < m->cap; i++) {
        u32 j;

        if (!m->slots[i].used) {
            continue;
        }
        j = ft_idx_map_hash(m->slots[i].key) & (ncap - 1);
        while (ns[j].used) {
            j = (j + 1) & (ncap - 1);
        }
        ns[j] = m->slots[i];
    }
    free(m->slots);
    m->slots = ns;
    m->cap = ncap;
    return OK;
}
/* 给 doc_id 累加一分。成功返回 0，内存不足返回 -1 */
int ft_idx_map_add(FtIdxScoreMap *m, u32 doc_id, double score)
{
    u32 i;

    if (m->cap == 0) {
        m->slots = (FtIdxSlot *)calloc(64, sizeof(FtIdxSlot));
        if (!m->slots) {
            return -1;
        }
        m->cap = 64;
    }
    /* 装填因子 <= 0.5：满了先翻倍 */
    if ((u64)m->count * 2 >= (u64)m->cap) {
        if (ft_idx_map_grow(m) != OK) {
            return -1;
        }
    }
    i = ft_idx_map_hash(doc_id) & (m->cap - 1);
    for (;;) {
        if (!m->slots[i].used) {
            m->slots[i].used = 1;
            m->slots[i].key = doc_id;
            m->slots[i].val = score;
            m->count++;
            return 0;
        }
        if (m->slots[i].key == doc_id) {
            m->slots[i].val += score;   /* 多个 query 词命中同一篇 → 分数累加 */
            return 0;
        }
        i = (i + 1) & (m->cap - 1);
    }
}

/* ---------------- 结果排序 ---------------- */

/*
 * 分数降序、分数相同按 doc_id 升序（坑 M4-11）。
 * 不用 epsilon：浮点相等必须用 == 判，否则同分的两条会被排成不同序。
 */
int ft_idx_hit_cmp(const void *a, const void *b)
{
    const FtHit *x = (const FtHit *)a;
    const FtHit *y = (const FtHit *)b;

    if (x->score != y->score) {
        return (x->score > y->score) ? -1 : 1;
    }
    if (x->doc_id != y->doc_id) {
        return (x->doc_id < y->doc_id) ? -1 : 1;
    }
    return 0;
}
