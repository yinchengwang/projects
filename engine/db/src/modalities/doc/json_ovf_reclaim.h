#ifndef DB_MODALITIES_DOC_JSON_OVF_RECLAIM_H
#define DB_MODALITIES_DOC_JSON_OVF_RECLAIM_H

#include "common/types.h"
#include "storage/pager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 回收启动扫描中没有任何 tuple 引用的 JSON 页链。 */
int json_ovf_reclaim_orphans(Pager *pg, const u64 *referenced_heads, u32 nrefs,
                             u32 *freed_pages_out);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_DOC_JSON_OVF_RECLAIM_H */
