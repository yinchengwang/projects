/*
 * ft_am_docmap.c —— 物化用的 (doc_id → 统计) 哈希表（从 ft_am.c 拆出）
 *
 * 原 ft_am.c 834 行，超了 800 行硬规则。拆出来的这一块是**纯数据结构**：
 *
 *   开放寻址哈希：doc_id → (doc_len, nterms, seg_id)
 *
 * 为什么值得单独一个文件：它的三条规则都是"一旦写错就静默错"的：
 *   - 装填因子 <= 0.5（否则线性探测退化）
 *   - seg_id 大者胜（坑 M4-19：更新段覆盖旧段的统计）
 *   - 同一 doc_id 的 dl 必须一致（坑 M4-18，不一致即损坏）
 *
 * 留在 ft_am.c 的是 SPI 适配本身：扫描 / 取行 / 解码 / 可见性 / 下推 / 构造。
 * 界线是"数据结构 vs 接口实现"。
 */

#include "modalities/ft/ft_am_docmap.h"

#include <stdlib.h>
#include <string.h>

#include "common/error.h"

/* ---------------- 聚合用的哈希表 ---------------- */

/*
 * doc_id → (doc_len, nterms)。
 *
 * doc_id 由调用方给、**不连续**，数量事先未知：
 * 用数组浪费，用线性查找就是 O(n²)。所以用开放寻址哈希
 * （装填因子 <= 0.5，Knuth 乘法散列 —— 与 ft_index.c 的做法一致）。
 */

static u32 ft_docmap_hash(u32 doc_id)
{
    return doc_id * 2654435761u;
}

void ft_docmap_init(FtDocMap *m)
{
    memset(m, 0, sizeof(*m));
}

void ft_docmap_free(FtDocMap *m)
{
    free(m->slots);
    memset(m, 0, sizeof(*m));
}

/* 容量翻倍并把老条目重插进去 */
static int ft_docmap_grow(FtDocMap *m)
{
    u32 ncap = m->cap * 2;
    FtDocSlot *ns = (FtDocSlot *)calloc(ncap, sizeof(FtDocSlot));
    u32 i;

    if (ns == NULL) {
        return E_NOSPACE;
    }
    for (i = 0; i < m->cap; i++) {
        u32 j;

        if (!m->slots[i].used) {
            continue;
        }
        j = ft_docmap_hash(m->slots[i].doc_id) & (ncap - 1);
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

/*
 * 记一次 (doc_id, dl) 命中。
 *
 * dl —— 该文档的词数（来自 posting）
 * seg_id —— 这次命中来自哪个段
 */
int ft_docmap_add(FtDocMap *m, u32 doc_id, u32 dl, u32 seg_id)
{
    u32 i;

    if (m->cap == 0) {
        m->slots = (FtDocSlot *)calloc(64, sizeof(FtDocSlot));
        if (m->slots == NULL) {
            return E_NOSPACE;
        }
        m->cap = 64;
    }
    /* 装填因子 <= 0.5：满了先翻倍 */
    if ((u64)m->count * 2 >= (u64)m->cap) {
        if (ft_docmap_grow(m) != OK) {
            return E_NOSPACE;
        }
    }

    i = ft_docmap_hash(doc_id) & (m->cap - 1);
    for (;;) {
        FtDocSlot *s = &m->slots[i];

        if (!s->used) {
            s->used = 1;
            s->doc_id = doc_id;
            s->doc_len = dl;
            s->nterms = 1;
            s->seg_id = seg_id;
            m->count++;
            return OK;
        }
        if (s->doc_id == doc_id) {
            if (seg_id < s->seg_id) {
                /* 旧段的命中：更新的段已经赢了，忽略（坑 M4-19） */
                return OK;
            }
            if (seg_id > s->seg_id) {
                /* 更新的段：这份文档以它为准，旧的统计作废 */
                s->seg_id = seg_id;
                s->doc_len = dl;
                s->nterms = 0;
            }
            /* 同一个 doc_id 的 dl 必须一致（坑 M4-18） */
            if (s->doc_len != dl) {
                return E_CORRUPT;
            }
            s->nterms++;
            return OK;
        }
        i = (i + 1) & (m->cap - 1);
    }
}

