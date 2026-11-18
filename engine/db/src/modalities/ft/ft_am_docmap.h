#ifndef DB_MODALITIES_FT_AM_DOCMAP_H
#define DB_MODALITIES_FT_AM_DOCMAP_H

/*
 * ft_am_docmap.h —— 物化用的哈希表的内部契约
 *
 * 只在 ft_am.c（用）与 ft_am_docmap.c（定义）之间共享。
 * 不开到 ft_am.h：这是实现细节，对外只暴露 StorageAM。
 */

#include <stdbool.h>

#include "common/types.h"

/* doc_id → (doc_len, nterms, seg_id) 的一个槽 */
typedef struct {
    u32 doc_id;
    u32 doc_len;
    u32 nterms;
    u32 seg_id;    /* 权威来源段（段号大者胜，坑 M4-19） */
    u8  used;
} FtDocSlot;

typedef struct {
    FtDocSlot *slots;
    u32        cap;      /* 槽数（2 的幂） */
    u32        count;    /* 已占槽数 */
} FtDocMap;

void ft_docmap_init(FtDocMap *m);
void ft_docmap_free(FtDocMap *m);
int  ft_docmap_add(FtDocMap *m, u32 doc_id, u32 dl, u32 seg_id);

#endif /* DB_MODALITIES_FT_AM_DOCMAP_H */
