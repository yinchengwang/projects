/**
 * @file vectors.c
 * @brief 向量模型公共 API（add/search/get/delete）
 *
 * 距离度量：L2（欧氏距离平方）。后续可在 P2 引入余弦/内积。
 * 索引：内存精确扁平索引（N >= 阈值时构建），小数据量直接扫 SQLite。
 */
#include "sdk/mmdb_vectors.h"
#include "sdk/impl/mmdb_internal.h"
#include "sdk/impl/vectors.h"
#include "sdk/impl/sqlite_backend.h"
#include "sdk/impl/filter_parser.h"
#include "sdk/impl/metrics_internal.h"  /* P6-M1.2：监控埋点 */

/* SDK 自持的精确扁平向量索引（原为冻结内核的 HNSW） */
#include "sdk/impl/index_flat.h"
/* P5-2: CRoaring bitmap 兼容层 */
#include "roaring_bitmap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* SIMD 加速（AVX2） */
#if defined(__AVX2__) || defined(_MSC_VER)
#include <immintrin.h>

/* 跨平台 AVX2 运行时检测 */
static int cpu_has_avx2(void) {
#if defined(__AVX2__)
    return 1;  /* 编译时已启用 AVX2 */
#elif defined(_MSC_VER)
    int regs[4];
    __cpuidex(regs, 7, 0);
    return (regs[1] & (1 << 5)) != 0;
#elif defined(__GNUC__) || defined(__clang__)
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        return (ebx >> 5) & 1;  /* EBX bit 5 = AVX2 */
    }
    return 0;
#else
    return 0;
#endif
}

/* 水平求和 __m256 */
static float hsum256_ps(__m256 v) {
    __m128 hi = _mm256_extractf128_ps(v, 1);
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 sum = _mm_add_ps(hi, lo);
    sum = _mm_hadd_ps(sum, sum);
    sum = _mm_hadd_ps(sum, sum);
    return _mm_cvtss_f32(sum);
}

/* AVX2 加速的 L2 平方距离 */
static float l2_distance_simd(const float* a, const float* b, size_t dim) {
    __m256 acc = _mm256_setzero_ps();
    size_t i = 0;

    /* 每次处理 8 个 float */
    for (; i + 8 <= dim; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        __m256 diff = _mm256_sub_ps(va, vb);
        acc = _mm256_add_ps(acc, _mm256_mul_ps(diff, diff));
    }

    float result = hsum256_ps(acc);

    /* 处理剩余元素 */
    for (; i < dim; i++) {
        float d = a[i] - b[i];
        result += d * d;
    }

    return result;
}
#endif

/* 标量 L2 平方距离（fallback） */
static float l2_distance_scalar(const float* a, const float* b, size_t dim) {
    float dist = 0.0f;
    for (size_t k = 0; k < dim; k++) {
        float d = a[k] - b[k];
        dist += d * d;
    }
    return dist;
}

/* ================================================================== */
/* P6-M1.1：分页辅助函数                                                  */
/* ================================================================== */

/**
 * @brief 在已填充的 mmdb_result_t 上应用 offset/limit 分页
 *
 * 前置条件：out->items 已分配，out->count 已设置为 top-K 结果数。
 * 后置条件：out->count 被裁剪到分页窗口，total_count/has_more/returned 被填充。
 *
 * 向后兼容：当 offset=0 且 limit=0 时，total_count = count，has_more = false，
 * returned = count，行为与分页 API 引入前完全一致。
 */
static void apply_pagination(mmdb_result_t* out, const mmdb_query_t* q) {
    if (!out || !q) return;

    if (q->offset > 0 || q->limit > 0) {
        uint32_t total = (uint32_t)out->count;
        uint32_t skip = q->offset;
        uint32_t take = q->limit > 0 ? q->limit : total;

        /* 计算实际返回数量（处理越界） */
        uint32_t actual_count =
            (skip >= total) ? 0
            : ((skip + take > total) ? (total - skip) : take);

        out->total_count = total;
        out->has_more = (skip + actual_count < total);
        out->returned = actual_count;

        /* 移动结果数组（跳过 offset 条）；只在确实需要移动时调用 memmove */
        if (skip > 0 && skip < total && actual_count > 0) {
            memmove(out->items, out->items + skip,
                    actual_count * sizeof(mmdb_result_item_t));
        }
        out->count = actual_count;
    } else {
        /* 向后兼容：offset=0, limit=0 时行为不变 */
        out->total_count = (uint32_t)out->count;
        out->has_more = false;
        out->returned = (uint32_t)out->count;
    }
}

/* ================================================================== */
/* 内存向量索引集成                                                     */
/* ================================================================== */

/*
 * 内存索引的构建门槛：向量数低于此值时直接扫 SQLite，不建内存副本。
 *
 * 【为什么有这个门槛】
 * 内存索引 = 全部向量的一份连续内存副本。向量少时，
 * 复制一遍再扫一遍比直接扫 SQLite 更慢（多了一次拷贝）。
 *
 * 【门槛为什么从 10000 降到 1000】
 * 原来用近似索引（构建成本高），门槛定得高；
 * 换成精确扁平索引后构建只是 memcpy，成本低一个数量级。
 */
#define VEC_INDEX_BUILD_THRESHOLD 1000

/* ================================================================== */
/* P5-1：sdk_id → vec_id 哈希表                                        */
/* 替换 build_filter_ctx 中的 O(N) 线性扫描，实现 O(1) 查找            */
/* ================================================================== */

/* FNV-1a 哈希：对可变长 sdk_id 字节序列生成 64 位哈希 */
static uint64_t fnv1a_hash(const uint8_t* data, size_t len) {
    uint64_t h = 14695981039346656037ULL;  /* FNV offset basis */
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 1099511628211ULL;  /* FNV prime */
    }
    return h;
}

/* 哈希表条目状态 */
#define HASH_SLOT_EMPTY   0
#define HASH_SLOT_USED    1
#define HASH_SLOT_DELETED 2

/* sdk_id → vec_id 哈希表（open addressing + 线性探测） */
typedef struct {
    uint64_t*  hashes;     /* sdk_id 的 FNV-1a 哈希值 */
    uint8_t**  sdk_id_keys;/* sdk_id 原始字节（碰撞校验用） */
    size_t*    sdk_id_lens;/* sdk_id 字节长度 */
    int32_t*   vec_ids;    /* 对应的索引内 vec_id */
    uint8_t*   states;     /* 空/已用/已删除 */
    size_t     capacity;   /* 槽数（2 的幂） */
    size_t     count;      /* 已用槽数 */
} sdk_id_hash_t;

static sdk_id_hash_t* sdk_id_hash_create(size_t initial_cap) {
    /* 确保 capacity 是 2 的幂 */
    if (initial_cap < 16) initial_cap = 16;
    size_t cap = 16;
    while (cap < initial_cap) cap <<= 1;

    sdk_id_hash_t* h = (sdk_id_hash_t*)calloc(1, sizeof(sdk_id_hash_t));
    if (!h) return NULL;
    h->capacity = cap;
    h->hashes = (uint64_t*)calloc(cap, sizeof(uint64_t));
    h->sdk_id_keys = (uint8_t**)calloc(cap, sizeof(uint8_t*));
    h->sdk_id_lens = (size_t*)calloc(cap, sizeof(size_t));
    h->vec_ids = (int32_t*)malloc(cap * sizeof(int32_t));
    h->states = (uint8_t*)calloc(cap, sizeof(uint8_t));
    if (!h->hashes || !h->sdk_id_keys || !h->sdk_id_lens || !h->vec_ids || !h->states) {
        free(h->hashes);
        free(h->sdk_id_keys);
        free(h->sdk_id_lens);
        free(h->vec_ids);
        free(h->states);
        free(h);
        return NULL;
    }
    for (size_t i = 0; i < cap; i++) h->vec_ids[i] = -1;
    return h;
}

static void sdk_id_hash_free(sdk_id_hash_t* h) {
    if (!h) return;
    for (size_t i = 0; i < h->capacity; i++) {
        if (h->states[i] == HASH_SLOT_USED && h->sdk_id_keys[i]) {
            free(h->sdk_id_keys[i]);
        }
    }
    free(h->hashes);
    free(h->sdk_id_keys);
    free(h->sdk_id_lens);
    free(h->vec_ids);
    free(h->states);
    free(h);
}

/* 插入映射：sdk_id → vec_id。返回 0 成功，-1 失败。 */
static int sdk_id_hash_insert(sdk_id_hash_t* h, const uint8_t* sdk_id,
                               size_t sdk_id_len, int32_t vec_id) {
    if (!h || !sdk_id || sdk_id_len == 0) return -1;
    /* 负载因子 > 0.75 时扩容 */
    if (h->count * 4 >= h->capacity * 3) {
        size_t new_cap = h->capacity << 1;
        uint64_t* new_hashes = (uint64_t*)calloc(new_cap, sizeof(uint64_t));
        uint8_t** new_keys = (uint8_t**)calloc(new_cap, sizeof(uint8_t*));
        size_t* new_lens = (size_t*)calloc(new_cap, sizeof(size_t));
        int32_t* new_ids = (int32_t*)malloc(new_cap * sizeof(int32_t));
        uint8_t* new_states = (uint8_t*)calloc(new_cap, sizeof(uint8_t));
        if (!new_hashes || !new_keys || !new_lens || !new_ids || !new_states) {
            free(new_hashes); free(new_keys); free(new_lens); free(new_ids); free(new_states);
            return -1;
        }
        for (size_t i = 0; i < new_cap; i++) new_ids[i] = -1;
        /* 重新哈希所有已用条目 */
        size_t old_cap = h->capacity;
        for (size_t i = 0; i < old_cap; i++) {
            if (h->states[i] == HASH_SLOT_USED) {
                uint64_t mask = new_cap - 1;
                size_t idx = h->hashes[i] & mask;
                while (new_states[idx] == HASH_SLOT_USED) {
                    idx = (idx + 1) & mask;
                }
                new_hashes[idx] = h->hashes[i];
                new_keys[idx] = h->sdk_id_keys[i];
                new_lens[idx] = h->sdk_id_lens[i];
                new_ids[idx] = h->vec_ids[i];
                new_states[idx] = HASH_SLOT_USED;
            }
        }
        free(h->hashes); free(h->sdk_id_keys); free(h->sdk_id_lens); free(h->vec_ids); free(h->states);
        h->hashes = new_hashes;
        h->sdk_id_keys = new_keys;
        h->sdk_id_lens = new_lens;
        h->vec_ids = new_ids;
        h->states = new_states;
        h->capacity = new_cap;
    }
    uint64_t hash = fnv1a_hash(sdk_id, sdk_id_len);
    uint64_t mask = h->capacity - 1;
    size_t idx = hash & mask;
    while (h->states[idx] == HASH_SLOT_USED) {
        idx = (idx + 1) & mask;
    }
    h->hashes[idx] = hash;
    h->sdk_id_keys[idx] = (uint8_t*)malloc(sdk_id_len);
    if (!h->sdk_id_keys[idx]) return -1;
    memcpy(h->sdk_id_keys[idx], sdk_id, sdk_id_len);
    h->sdk_id_lens[idx] = sdk_id_len;
    h->vec_ids[idx] = vec_id;
    h->states[idx] = HASH_SLOT_USED;
    h->count++;
    return 0;
}

/* 标记删除：sdk_id → vec_id 的反向查找由 id_map 完成，此处仅标记状态 */
static void sdk_id_hash_delete(sdk_id_hash_t* h, const uint8_t* sdk_id,
                                size_t sdk_id_len) {
    if (!h || !sdk_id || sdk_id_len == 0) return;
    uint64_t hash = fnv1a_hash(sdk_id, sdk_id_len);
    uint64_t mask = h->capacity - 1;
    size_t idx = hash & mask;
    while (h->states[idx] != HASH_SLOT_EMPTY) {
        if (h->states[idx] == HASH_SLOT_USED && h->hashes[idx] == hash &&
            h->sdk_id_lens[idx] == sdk_id_len &&
            memcmp(h->sdk_id_keys[idx], sdk_id, sdk_id_len) == 0) {
            /* 完全匹配（哈希 + 字节），标记删除 */
            free(h->sdk_id_keys[idx]);
            h->sdk_id_keys[idx] = NULL;
            h->sdk_id_lens[idx] = 0;
            h->states[idx] = HASH_SLOT_DELETED;
            h->vec_ids[idx] = -1;
            h->count--;
            return;
        }
        idx = (idx + 1) & mask;
    }
}

/* P5-1：查找 sdk_id 对应的 vec_id（O(1) 均摊）。找不到返回 -1。 */
static int32_t sdk_id_hash_lookup(const sdk_id_hash_t* h, const uint8_t* sdk_id,
                                   size_t sdk_id_len) {
    if (!h || !sdk_id || sdk_id_len == 0) return -1;
    uint64_t hash = fnv1a_hash(sdk_id, sdk_id_len);
    uint64_t mask = h->capacity - 1;
    size_t idx = hash & mask;
    while (h->states[idx] != HASH_SLOT_EMPTY) {
        if (h->states[idx] == HASH_SLOT_USED && h->hashes[idx] == hash &&
            h->sdk_id_lens[idx] == sdk_id_len &&
            memcmp(h->sdk_id_keys[idx], sdk_id, sdk_id_len) == 0) {
            return h->vec_ids[idx];
        }
        idx = (idx + 1) & mask;
    }
    return -1;
}

/* ID 映射表：int32_t 索引内 ID ↔ 可变长 SDK ID (BLOB) */
typedef struct {
    uint8_t**  sdk_ids;      /* sdk_ids[i] = 对应索引内 ID i 的 SDK 字节数组 ID */
    size_t*    sdk_id_lens;  /* sdk_id_lens[i] = sdk_ids[i] 的字节长度 */
    size_t     count;        /* 当前条目数 */
    size_t     capacity;     /* 已分配容量 */
} hnsw_id_map_t;

static hnsw_id_map_t* hnsw_id_map_create(size_t initial_cap) {
    hnsw_id_map_t* m = (hnsw_id_map_t*)calloc(1, sizeof(hnsw_id_map_t));
    if (!m) return NULL;
    m->capacity = initial_cap > 0 ? initial_cap : 256;
    m->sdk_ids = (uint8_t**)calloc(m->capacity, sizeof(uint8_t*));
    m->sdk_id_lens = (size_t*)calloc(m->capacity, sizeof(size_t));
    if (!m->sdk_ids || !m->sdk_id_lens) {
        free(m->sdk_ids);
        free(m->sdk_id_lens);
        free(m);
        return NULL;
    }
    return m;
}

static void hnsw_id_map_free(hnsw_id_map_t* m) {
    if (!m) return;
    for (size_t i = 0; i < m->count; i++) {
        free(m->sdk_ids[i]);
    }
    free(m->sdk_ids);
    free(m->sdk_id_lens);
    free(m);
}

/* 追加一条映射，返回索引内 ID（从 0 开始递增） */
static int32_t hnsw_id_map_add(hnsw_id_map_t* m, const uint8_t* id, size_t id_len) {
    if (m->count >= m->capacity) {
        size_t new_cap = m->capacity * 2;
        uint8_t** new_ids = (uint8_t**)realloc(m->sdk_ids, new_cap * sizeof(uint8_t*));
        size_t* new_lens = (size_t*)realloc(m->sdk_id_lens, new_cap * sizeof(size_t));
        if (!new_ids || !new_lens) return -1;
        m->sdk_ids = new_ids;
        m->sdk_id_lens = new_lens;
        memset(m->sdk_ids + m->capacity, 0, (new_cap - m->capacity) * sizeof(uint8_t*));
        memset(m->sdk_id_lens + m->capacity, 0, (new_cap - m->capacity) * sizeof(size_t));
        m->capacity = new_cap;
    }
    int32_t hnsw_id = (int32_t)m->count;
    m->sdk_ids[hnsw_id] = (uint8_t*)malloc(id_len);
    if (!m->sdk_ids[hnsw_id]) return -1;
    memcpy(m->sdk_ids[hnsw_id], id, id_len);
    m->sdk_id_lens[hnsw_id] = id_len;
    m->count++;
    return hnsw_id;
}

/* 标记某索引内 ID 为已删除（释放其 SDK ID 内存，设为 NULL） */
static void hnsw_id_map_mark_deleted(hnsw_id_map_t* m, int32_t hnsw_id) {
    if (hnsw_id < 0 || (size_t)hnsw_id >= m->count) return;
    free(m->sdk_ids[hnsw_id]);
    m->sdk_ids[hnsw_id] = NULL;
    m->sdk_id_lens[hnsw_id] = 0;
}

/* 内存索引包装：索引实例 + ID 映射表，挂在 collection 的 vec_index 字段上 */
typedef struct {
    mmdb_flat_index_t* index;       /* 精确扁平索引实例 */
    hnsw_id_map_t*  id_map;         /* int32_t ↔ SDK ID 映射 */
    sdk_id_hash_t*  sdk_id_hash;    /* P5-1：sdk_id → vec_id 哈希表 */
    size_t          deleted_count;   /* 已标记删除的条目数 */
} index_wrapper_t;

/* P4-T4.5：metadata filter 谓词上下文。
 * bitmap[i] = 1 表示索引内 vec_id i 通过 metadata filter；0 表示被过滤。
 * 通过一次性 SQLite 查询构建，避免每个候选单独查询 metadata。 */
typedef struct {
    int8_t* bitmap;         /* 兼容：小数据集仍用原始 bitmap */
    size_t  bitmap_size;    /* bitmap 元素数（与索引内当前向量数一致） */
    void*   roaring;        /* P5-2：roaring bitmap 句柄（NULL 表示未启用） */
} hnsw_filter_ctx_t;

/* P4-T4.5：metadata filter 谓词回调（mmdb_flat_filter_fn 类型） */
static int hnsw_filter_predicate(int32_t vec_id, void* user_data) {
    hnsw_filter_ctx_t* ctx = (hnsw_filter_ctx_t*)user_data;
    if (!ctx) return 1;  /* 无 filter ctx 时全部通过 */

    /* P5-2：大数据集优先使用 roaring bitmap */
    if (ctx->roaring) {
        return roaring_bitmap_contains((const roaring_bitmap_t*)ctx->roaring,
                                       (uint32_t)vec_id);
    }

    /* 兼容：原始 bitmap */
    if (!ctx->bitmap) return 1;
    if (vec_id < 0 || (size_t)vec_id >= ctx->bitmap_size) return 0;
    return ctx->bitmap[vec_id];
}

static index_wrapper_t* index_wrapper_create(void) {
    index_wrapper_t* w = (index_wrapper_t*)calloc(1, sizeof(index_wrapper_t));
    return w;
}

static void index_wrapper_free(index_wrapper_t* w) {
    if (!w) return;
    if (w->index) mmdb_flat_index_drop(w->index);
    hnsw_id_map_free(w->id_map);
    sdk_id_hash_free(w->sdk_id_hash);
    free(w);
}

/* P4-T4.5：从 SQLite 查询符合 filter 的 SDK ID，写入 bitmap（按索引内 vec_id 索引）。
 * 用于内存索引加速路径下的 metadata 过滤：bitmap[i] = 1 表示索引内 vec_id i 通过 filter。
 *
 * 注意：此函数必须在已持有 coll_lock 读锁的情况下调用（SQLite 操作需要稳定快照）。
 *
 * @param c       collection
 * @param where   mmdb_filter_compile 生成的 WHERE 片段
 * @param fp      filter 绑定参数
 * @param ctx     输出上下文（调用方负责 free_filter_ctx）
 * @return        MMDB_OK 或错误码
 */
/* build_table_name 前向声明（定义在文件后半部分） */
static int build_table_name(char* out, size_t out_cap, const char* coll);

/* P5-2：roaring bitmap 启用阈值（超过此数量时切换到压缩存储） */
#define ROARING_THRESHOLD 100000

static int build_filter_ctx(mmdb_collection_t* c, const char* where,
                             mmdb_filter_params_t* fp,
                             hnsw_filter_ctx_t* ctx) {
    index_wrapper_t* w = (index_wrapper_t*)c->vec_index;
    memset(ctx, 0, sizeof(*ctx));
    ctx->bitmap_size = w->id_map->count;
    if (ctx->bitmap_size == 0) return MMDB_OK;

    /* P5-2：大数据集启用 roaring bitmap 压缩存储 */
    int use_roaring = (ctx->bitmap_size > ROARING_THRESHOLD);

    if (use_roaring) {
        ctx->roaring = roaring_bitmap_create();
        if (!ctx->roaring) return MMDB_ERR_NOMEM;
        /* bitmap 字段保持 NULL，不分配原始数组 */
    } else {
        ctx->bitmap = (int8_t*)calloc(ctx->bitmap_size, sizeof(int8_t));
        if (!ctx->bitmap) return MMDB_ERR_NOMEM;
    }

    char tname[128];
    build_table_name(tname, sizeof(tname), c->name);

    char sql[512];
    int n = snprintf(sql, sizeof(sql), "SELECT id FROM %s WHERE %s;", tname, where);
    if (n < 0 || (size_t)n >= sizeof(sql)) {
        if (ctx->roaring) { roaring_bitmap_free(ctx->roaring); ctx->roaring = NULL; }
        else { free(ctx->bitmap); ctx->bitmap = NULL; }
        return MMDB_ERR_INVALID;
    }

    sqlite3_stmt* stmt = mmdb_sqlite_prepare(c->sdb, sql, NULL, 0);
    if (!stmt) {
        if (ctx->roaring) { roaring_bitmap_free(ctx->roaring); ctx->roaring = NULL; }
        else { free(ctx->bitmap); ctx->bitmap = NULL; }
        return MMDB_ERR_IO;
    }
    int bind_idx = mmdb_filter_bind(stmt, fp, 1);
    if (bind_idx < 0) {
        sqlite3_finalize(stmt);
        if (ctx->roaring) { roaring_bitmap_free(ctx->roaring); ctx->roaring = NULL; }
        else { free(ctx->bitmap); ctx->bitmap = NULL; }
        return MMDB_ERR_INVALID;
    }

    /* 对每个匹配的 SDK ID，通过哈希表 O(1) 查找对应 vec_id 并置位 */
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const void* id_blob = sqlite3_column_blob(stmt, 0);
        int id_bytes = sqlite3_column_bytes(stmt, 0);
        if (id_bytes <= 0) continue;
        /* P5-1 优化：哈希表 O(1) 查找，替换原有 O(N) 线性扫描 */
        int32_t vec_id = sdk_id_hash_lookup(w->sdk_id_hash,
                                            (const uint8_t*)id_blob, (size_t)id_bytes);
        if (vec_id >= 0 && (size_t)vec_id < ctx->bitmap_size) {
            if (ctx->roaring) {
                roaring_bitmap_add((roaring_bitmap_t*)ctx->roaring, (uint32_t)vec_id);
            } else {
                ctx->bitmap[vec_id] = 1;
            }
        }
    }

    sqlite3_finalize(stmt);
    return MMDB_OK;
}

/* P4-T4.5：释放 filter 上下文 */
static void free_filter_ctx(hnsw_filter_ctx_t* ctx) {
    if (!ctx) return;
    if (ctx->roaring) {
        roaring_bitmap_free((roaring_bitmap_t*)ctx->roaring);
        ctx->roaring = NULL;
    }
    if (ctx->bitmap) {
        free(ctx->bitmap);
        ctx->bitmap = NULL;
    }
    ctx->bitmap_size = 0;
}

/* 释放 collection 的内存索引 */
void mmdb_vectors_index_free(mmdb_collection_t* c) {
    if (!c || !c->vec_index) return;
    index_wrapper_free((index_wrapper_t*)c->vec_index);
    c->vec_index = NULL;
}

/* ------------------------------------------------------------------ */
/* 内存索引重建：从 SQLite 全表扫描 bulk-load                          */
/* ------------------------------------------------------------------ */

/* 获取表中向量数量 */
static size_t vec_count_vectors(mmdb_collection_t* c, const char* tname) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s;", tname);
    sqlite3_stmt* stmt = mmdb_sqlite_prepare(c->sdb, sql, NULL, 0);
    if (!stmt) return 0;
    size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = (size_t)sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

/* build_table_name 前向声明（定义在文件后半部分） */
static int build_table_name(char* out, size_t out_cap, const char* coll);

/* 从 SQLite 重建内存索引（在 coll_lock 已持有的情况下调用） */
int mmdb_vectors_index_rebuild(mmdb_collection_t* c) {
    index_wrapper_t* w = (index_wrapper_t*)c->vec_index;
    if (!w) return MMDB_ERR_INVALID;

    char tname[128];
    build_table_name(tname, sizeof(tname), c->name);

    /* 查询所有向量 */
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT id, vector FROM %s;", tname);
    sqlite3_stmt* stmt = mmdb_sqlite_prepare(c->sdb, sql, NULL, 0);
    if (!stmt) return MMDB_ERR_IO;

    size_t dim = c->schema.vector_dim;
    size_t total = vec_count_vectors(c, tname);
    if (total == 0) {
        sqlite3_finalize(stmt);
        return MMDB_OK;
    }

    /* P6-M1.2：记录内存索引构建耗时 */
    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    /* 一次性分配所有向量数据（连续内存，mmdb_flat_index_add 要求） */
    float* all_vectors = (float*)calloc(total * dim, sizeof(float));
    if (!all_vectors) {
        sqlite3_finalize(stmt);
        return MMDB_ERR_NOMEM;
    }

    /* 重建 ID 映射表 */
    hnsw_id_map_free(w->id_map);
    w->id_map = hnsw_id_map_create(total);
    if (!w->id_map) {
        free(all_vectors);
        sqlite3_finalize(stmt);
        return MMDB_ERR_NOMEM;
    }

    /* P5-1：重建 sdk_id → vec_id 哈希表 */
    sdk_id_hash_free(w->sdk_id_hash);
    w->sdk_id_hash = sdk_id_hash_create(total);
    if (!w->sdk_id_hash) {
        free(all_vectors);
        sqlite3_finalize(stmt);
        return MMDB_ERR_NOMEM;
    }

    /* 释放旧索引，准备创建新的 */
    if (w->index) {
        mmdb_flat_index_drop(w->index);
        w->index = NULL;
    }

    /* 扫描 SQLite，填充向量数组和 ID 映射 */
    size_t row_idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && row_idx < total) {
        const void* id_blob = sqlite3_column_blob(stmt, 0);
        int id_bytes = sqlite3_column_bytes(stmt, 0);
        const void* vec_blob = sqlite3_column_blob(stmt, 1);
        int vec_bytes = sqlite3_column_bytes(stmt, 1);

        if (id_bytes <= 0 || (size_t)vec_bytes != dim * sizeof(float)) continue;

        /* 追加到 ID 映射表 */
        int32_t hnsw_id = hnsw_id_map_add(w->id_map, (const uint8_t*)id_blob, (size_t)id_bytes);
        if (hnsw_id < 0) continue;

        /* P5-1：同时更新 sdk_id → vec_id 哈希表（用于 filter bitmap 构建） */
        if (w->sdk_id_hash) {
            sdk_id_hash_insert(w->sdk_id_hash, (const uint8_t*)id_blob, (size_t)id_bytes, hnsw_id);
        }

        /* 拷贝向量数据到连续数组 */
        memcpy(all_vectors + row_idx * dim, vec_blob, dim * sizeof(float));
        row_idx++;
    }
    sqlite3_finalize(stmt);

    if (row_idx == 0) {
        free(all_vectors);
        return MMDB_OK;
    }

    /* 创建扁平索引并批量插入 */
    w->index = mmdb_flat_index_create((int32_t)dim);
    if (!w->index) {
        free(all_vectors);
        return MMDB_ERR_NOMEM;
    }

    /*
     * 批量导入前先一次性预分配容量。
     * 逐条 add 会让容量反复翻倍 realloc，额外搬运总量是 O(N)；
     * 预分配把这一步压成一次分配。
     */
    if (mmdb_flat_index_reserve(w->index, (int32_t)row_idx) != 0) {
        free(all_vectors);
        mmdb_flat_index_drop(w->index);
        w->index = NULL;
        return MMDB_ERR_NOMEM;
    }

    int32_t add_rc = mmdb_flat_index_add(w->index, (int32_t)row_idx, all_vectors);
    free(all_vectors);

    /* mmdb_flat_index_add 返回成功添加的向量数；正常情况下等于 row_idx。
     * 万一不等（内存不足等），降级为直扫模式。
     * 原代码误用 `add_rc != 0` 检查，导致所有成功构建也被丢弃 —— 那个错别再犯。 */
    if (add_rc != (int32_t)row_idx) {
        mmdb_flat_index_drop(w->index);
        w->index = NULL;
        return MMDB_OK;  /* 不报错，降级到直扫 */
    }

    /* 记录内存索引构建耗时（公开指标名沿用 hnsw_build_*，保 API 稳定） */
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double build_time_ms =
        (double)(t_end.tv_sec - t_start.tv_sec) * 1000.0 +
        (double)(t_end.tv_nsec - t_start.tv_nsec) / 1e6;
    mmdb_metrics_record_hnsw_build(build_time_ms);

    w->deleted_count = 0;
    return MMDB_OK;
}

/* ------------------------------------------------------------------ */
/* 内存索引懒创建：N >= 阈值时才建                                       */
/* ------------------------------------------------------------------ */

int mmdb_vectors_index_ensure(mmdb_collection_t* c) {
    if (!c || !c->has_vector) return MMDB_ERR_INVALID;

    /* 已有内存索引，直接返回 */
    if (c->vec_index) return MMDB_OK;

    /* 确保表存在 */
    if (mmdb_vectors_ensure_table(c) != MMDB_OK) return MMDB_ERR_IO;

    char tname[128];
    build_table_name(tname, sizeof(tname), c->name);

    /* 检查当前行数 */
    size_t count = vec_count_vectors(c, tname);
    if (count < VEC_INDEX_BUILD_THRESHOLD) return MMDB_OK;  /* 不值得建 */

    /*
     * 【原来的决策从哪来】
     * 冻结内核的索引选择器会按规模/维度/内存/QPS/召回率
     * 在 HNSW / IVF-PQ / DiskANN / 暴力之间选。SDK 现在只有一种索引，
     * 决策退化成"建不建" —— 上面那个门槛就是全部决策逻辑。
     */
    index_wrapper_t* w = index_wrapper_create();
    if (!w) return MMDB_ERR_NOMEM;
    c->vec_index = w;

    int rc = mmdb_vectors_index_rebuild(c);
    if (rc != MMDB_OK) {
        /* 构建失败，降级为直扫模式（不报错 —— 查询仍可用，只是慢一点） */
        index_wrapper_free(w);
        c->vec_index = NULL;
        return MMDB_OK;
    }
    return MMDB_OK;
}

/* ------------------------------------------------------------------ */
/* DDL：创建/校验 collection 数据表                                    */
/* ------------------------------------------------------------------ */

static int build_table_name(char* out, size_t out_cap, const char* coll) {
    int n = snprintf(out, out_cap, "mmdb_vec_%s", coll);
    if (n < 0 || (size_t)n >= out_cap) return MMDB_ERR_INVALID;
    return MMDB_OK;
}

int mmdb_vectors_ensure_table(mmdb_collection_t* coll) {
    if (!coll || !coll->has_vector) return MMDB_ERR_INVALID;

    char tname[128];
    if (build_table_name(tname, sizeof(tname), coll->name) != MMDB_OK)
        return MMDB_ERR_INVALID;

    /* 检查 sqlite_master 中是否已存在 */
    sqlite3_stmt* stmt = mmdb_sqlite_prepare(
        coll->sdb,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;", NULL, 0);
    if (!stmt) return MMDB_ERR_IO;
    mmdb_sqlite_bind_text(stmt, 1, tname);
    int exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    if (exists) return MMDB_OK;

    char ddl[256];
    snprintf(ddl, sizeof(ddl),
             "CREATE TABLE IF NOT EXISTS %s ("
             "  id BLOB PRIMARY KEY,"
             "  vector BLOB NOT NULL,"
             "  metadata TEXT,"
             "  text TEXT,"
             "  created_at INTEGER NOT NULL"
             ");",
             tname);
    return mmdb_sqlite_exec(coll->sdb, ddl);
}

/* ------------------------------------------------------------------ */
/* 公共 API                                                            */
/* ------------------------------------------------------------------ */

int mmdb_vectors_add(mmdb_collection_t* c, const mmdb_vector_t* vecs, size_t n) {
    if (!c || !vecs || n == 0) return MMDB_ERR_INVALID;
    if (!c->has_vector) return MMDB_ERR_INVALID;
    if (mmdb_vectors_ensure_table(c) != MMDB_OK) return MMDB_ERR_IO;

    /* 写操作：获取写锁 */
    mmdb_rwlock_wrlock(c->coll_lock);

    /* 优化：批量写入前临时禁用自动 checkpoint，完成后恢复 */
    sqlite3_exec(c->sdb, "PRAGMA wal_autocheckpoint=0;", NULL, NULL, NULL);

    /* BEGIN IMMEDIATE 获取写锁，避免 WAL 锁竞争 */
    sqlite3_exec(c->sdb, "BEGIN IMMEDIATE;", NULL, NULL, NULL);

    char tname[128];
    build_table_name(tname, sizeof(tname), c->name);
    char sql[256];
    snprintf(sql, sizeof(sql),
             "INSERT INTO %s(id, vector, metadata, text, created_at) "
             "VALUES (?, ?, ?, ?, ?);",
             tname);
    sqlite3_stmt* stmt = mmdb_sqlite_prepare(c->sdb, sql, NULL, 0);
    if (!stmt) {
        sqlite3_exec(c->sdb, "ROLLBACK;", NULL, NULL, NULL);
        mmdb_rwlock_unlock(c->coll_lock, 1);
        sqlite3_exec(c->sdb, "PRAGMA wal_autocheckpoint=1000;", NULL, NULL, NULL);
        return MMDB_ERR_IO;
    }

    int rc = MMDB_OK;
    int64_t now = (int64_t)time(NULL);  /* 只获取一次时间戳，避免 10K 次系统调用 */

    for (size_t i = 0; i < n; i++) {
        const mmdb_vector_t* v = &vecs[i];
        if (v->dim != c->schema.vector_dim) {
            rc = MMDB_ERR_INVALID;
            break;
        }
        sqlite3_reset(stmt);

        /*
         * 优化要点：
         * 1. 使用 SQLITE_STATIC 代替 SQLITE_TRANSIENT，避免每行 malloc+memcpy
         *    （向量数据 512 字节 × 10K = 5MB 的无谓拷贝）
         * 2. 直接调用 sqlite3_bind_* 跳过封装函数开销
         * 3. 省略 sqlite3_clear_bindings —— 后续 bind 会覆盖旧值，无需显式清空
         */
        sqlite3_bind_blob(stmt, 1, v->id, (int)v->id_len, SQLITE_STATIC);
        sqlite3_bind_blob(stmt, 2, v->vector, (int)(v->dim * sizeof(float)),
                          SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3,
                          v->metadata_json ? v->metadata_json : "",
                          -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4,
                          v->text ? v->text : "",
                          -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 5, now);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            rc = MMDB_ERR_IO;
            break;
        }
    }
    sqlite3_finalize(stmt);

    if (rc == MMDB_OK) {
        sqlite3_exec(c->sdb, "COMMIT;", NULL, NULL, NULL);
        /* P6-M1.2：成功插入的向量数计入总数 */
        mmdb_metrics_inc_vectors_total(n);
    } else {
        sqlite3_exec(c->sdb, "ROLLBACK;", NULL, NULL, NULL);
    }

    /* 恢复自动 checkpoint */
    sqlite3_exec(c->sdb, "PRAGMA wal_autocheckpoint=1000;", NULL, NULL, NULL);

    mmdb_rwlock_unlock(c->coll_lock, 1);
    return rc;
}

int mmdb_vectors_upsert(mmdb_collection_t* c, const mmdb_vector_t* vecs,
                        size_t n) {
    /* 使用 INSERT OR REPLACE 实现 upsert 语义 */
    if (!c || !vecs || n == 0) return MMDB_ERR_INVALID;
    if (!c->has_vector) return MMDB_ERR_INVALID;
    if (mmdb_vectors_ensure_table(c) != MMDB_OK) return MMDB_ERR_IO;

    /* 写操作：获取写锁 */
    mmdb_rwlock_wrlock(c->coll_lock);

    /* 批量写入前临时禁用自动 checkpoint，完成后恢复 */
    sqlite3_exec(c->sdb, "PRAGMA wal_autocheckpoint=0;", NULL, NULL, NULL);

    /* BEGIN IMMEDIATE 获取写锁 */
    sqlite3_exec(c->sdb, "BEGIN IMMEDIATE;", NULL, NULL, NULL);

    char tname[128];
    build_table_name(tname, sizeof(tname), c->name);
    char sql[256];
    snprintf(sql, sizeof(sql),
             "INSERT OR REPLACE INTO %s(id, vector, metadata, text, created_at) "
             "VALUES (?, ?, ?, ?, ?);",
             tname);
    sqlite3_stmt* stmt = mmdb_sqlite_prepare(c->sdb, sql, NULL, 0);
    if (!stmt) {
        sqlite3_exec(c->sdb, "ROLLBACK;", NULL, NULL, NULL);
        mmdb_rwlock_unlock(c->coll_lock, 1);
        sqlite3_exec(c->sdb, "PRAGMA wal_autocheckpoint=1000;", NULL, NULL, NULL);
        return MMDB_ERR_IO;
    }

    int rc = MMDB_OK;
    int64_t now = (int64_t)time(NULL);

    for (size_t i = 0; i < n; i++) {
        const mmdb_vector_t* v = &vecs[i];
        if (v->dim != c->schema.vector_dim) {
            rc = MMDB_ERR_INVALID;
            break;
        }
        sqlite3_reset(stmt);

        sqlite3_bind_blob(stmt, 1, v->id, (int)v->id_len, SQLITE_STATIC);
        sqlite3_bind_blob(stmt, 2, v->vector, (int)(v->dim * sizeof(float)),
                          SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3,
                          v->metadata_json ? v->metadata_json : "",
                          -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4,
                          v->text ? v->text : "",
                          -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 5, now);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            rc = MMDB_ERR_IO;
            break;
        }
    }
    sqlite3_finalize(stmt);

    if (rc == MMDB_OK) {
        sqlite3_exec(c->sdb, "COMMIT;", NULL, NULL, NULL);
    } else {
        sqlite3_exec(c->sdb, "ROLLBACK;", NULL, NULL, NULL);
    }

    sqlite3_exec(c->sdb, "PRAGMA wal_autocheckpoint=1000;", NULL, NULL, NULL);

    mmdb_rwlock_unlock(c->coll_lock, 1);
    return rc;
}

int mmdb_vectors_delete(mmdb_collection_t* c, const uint8_t* id, size_t id_len) {
    if (!c || !id || id_len == 0) return MMDB_ERR_INVALID;
    if (!c->has_vector) return MMDB_ERR_INVALID;

    char tname[128];
    build_table_name(tname, sizeof(tname), c->name);
    char sql[256];
    snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE id = ?;", tname);
    sqlite3_stmt* stmt = mmdb_sqlite_prepare(c->sdb, sql, NULL, 0);
    if (!stmt) return MMDB_ERR_IO;
    /* 写操作：获取写锁 */
    mmdb_rwlock_wrlock(c->coll_lock);
    mmdb_sqlite_bind_blob(stmt, 1, id, id_len);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? MMDB_OK : MMDB_ERR_IO;
    sqlite3_finalize(stmt);

    /* P6-M1.2：成功删除的向量从总数扣除 */
    if (rc == MMDB_OK) {
        mmdb_metrics_dec_vectors_total(1);
    }

    /* 内存索引同步 — 在 ID 映射表中标记删除，后续 rebuild 自然过滤 */
    if (rc == MMDB_OK && c->vec_index) {
        index_wrapper_t* w = (index_wrapper_t*)c->vec_index;
        /* P5-1：从 sdk_id 哈希表中删除映射（O(1)），再标记 id_map */
        if (w->sdk_id_hash) {
            sdk_id_hash_delete(w->sdk_id_hash, id, id_len);
        }
        /* 遍历映射表找到匹配的索引内 ID 并标记删除 */
        for (size_t i = 0; i < w->id_map->count; i++) {
            if (w->id_map->sdk_ids[i] != NULL &&
                w->id_map->sdk_id_lens[i] == id_len &&
                memcmp(w->id_map->sdk_ids[i], id, id_len) == 0) {
                hnsw_id_map_mark_deleted(w->id_map, (int32_t)i);
                /*
                 * 扁平索引没有"删除"这个概念：向量按位置固定存放，
                 * 删除靠上面的 id_map 置空来表达 —— 搜索时按 id_map 跳过。
                 * （HNSW 时代还有一层内部 bitmap，扁平索引没有这回事。）
                 */
                w->deleted_count++;
                break;
            }
        }
    }

    mmdb_rwlock_unlock(c->coll_lock, 1);
    return rc;
}

int mmdb_vectors_get(mmdb_collection_t* c, const uint8_t* id, size_t id_len,
                     mmdb_vector_t* out) {
    if (!c || !id || !out) return MMDB_ERR_INVALID;
    if (!c->has_vector) return MMDB_ERR_INVALID;

    char tname[128];
    build_table_name(tname, sizeof(tname), c->name);
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT vector, metadata, text FROM %s WHERE id = ?;", tname);
    sqlite3_stmt* stmt = mmdb_sqlite_prepare(c->sdb, sql, NULL, 0);
    if (!stmt) return MMDB_ERR_NOT_FOUND;

    /* 读操作：获取读锁（支持并发读） */
    mmdb_rwlock_rdlock(c->coll_lock);
    mmdb_sqlite_bind_blob(stmt, 1, id, id_len);
    int rc = MMDB_ERR_NOT_FOUND;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        /* 注：out 字段为 const，仅作返回值载体 */
        const void* vec_blob = sqlite3_column_blob(stmt, 0);
        int vec_bytes = sqlite3_column_bytes(stmt, 0);
        size_t dim = vec_bytes / sizeof(float);
        /* 由于 mmdb_vector_t 的字段都是 const，调用方需自行保证生命周期 */
        memcpy((void*)out->vector, vec_blob, vec_bytes);
        out->dim = dim;
        out->id = id;
        out->id_len = id_len;
        rc = MMDB_OK;
    }
    sqlite3_finalize(stmt);
    mmdb_rwlock_unlock(c->coll_lock, 0);
    return rc;
}

/* ------------------------------------------------------------------ */
/* KNN 搜索（Flat 索引：暴力扫描）                                     */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* 最大堆（push/pop top-K 最近邻）                                     */
/* ------------------------------------------------------------------ */

/* 堆元素 */
typedef struct {
    float   dist;
    uint8_t id[64];
    size_t  id_len;
} knn_cand_t;

/* 下沉：维持最大堆性质（堆顶最大） */
static void heap_siftdown(knn_cand_t* heap, size_t size, size_t i) {
    while (1) {
        size_t largest = i;
        size_t left = 2 * i + 1;
        size_t right = 2 * i + 2;
        if (left < size && heap[left].dist > heap[largest].dist)
            largest = left;
        if (right < size && heap[right].dist > heap[largest].dist)
            largest = right;
        if (largest == i) break;
        /* 交换 */
        knn_cand_t tmp = heap[i];
        heap[i] = heap[largest];
        heap[largest] = tmp;
        i = largest;
    }
}

/* 上浮：新元素放入堆尾后上浮 */
static void heap_siftup(knn_cand_t* heap, size_t i) {
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (heap[parent].dist >= heap[i].dist) break;
        knn_cand_t tmp = heap[i];
        heap[i] = heap[parent];
        heap[parent] = tmp;
        i = parent;
    }
}

/* 比较函数（用于最终排序） */
static int cand_cmp(const void* a, const void* b) {
    float da = ((const knn_cand_t*)a)->dist;
    float db = ((const knn_cand_t*)b)->dist;
    return (da < db) ? -1 : (da > db) ? 1 : 0;
}

int mmdb_vectors_search(mmdb_collection_t* c, const mmdb_query_t* q,
                        mmdb_result_t* out) {
    if (!c || !q || !out || !q->query_vector) return MMDB_ERR_INVALID;
    if (!c->has_vector) return MMDB_ERR_INVALID;
    if (q->dim != c->schema.vector_dim) return MMDB_ERR_INVALID;
    memset(out, 0, sizeof(*out));

    if (mmdb_vectors_ensure_table(c) != MMDB_OK) return MMDB_ERR_IO;

    /* P6-M1.2：监控埋点 - 记录查询延迟与成功/失败 */
    struct timespec q_start, q_end;
    clock_gettime(CLOCK_MONOTONIC, &q_start);

    /* 自动决策是否建内存索引（门槛见 VEC_INDEX_BUILD_THRESHOLD） */
    mmdb_vectors_index_ensure(c);

    /* 编译 filter */
    mmdb_filter_params_t fp;
    char* where = mmdb_filter_compile(q->filter_json, &fp);
    if (!where) {
        mmdb_filter_params_free(&fp);
        return MMDB_ERR_INVALID;
    }

    size_t top_k = q->top_k > 0 ? q->top_k : 10;

    /* 内存索引加速搜索路径（P4-T4.5：支持 metadata filter）
     * 当内存索引可用时，filter 为空走原路径，filter 非空走 filter 路径。
     * 两者最终都使用 mmdb_flat_search_filtered()：filter 为空时传 NULL 谓词。 */
    if (c->vec_index) {
        index_wrapper_t* w = (index_wrapper_t*)c->vec_index;
        if (w->index && mmdb_flat_index_size(w->index) > 0) {
            /* P4-T4.5：filter 非空 → 构建 bitmap 谓词；filter 为空 → NULL 谓词 */
            hnsw_filter_ctx_t filter_ctx;
            memset(&filter_ctx, 0, sizeof(filter_ctx));  /* P6-M1.3：未初始化指针 free 会段错误 */
            int has_filter = (where[0] != '\0');
            int filter_rc = MMDB_OK;
            if (has_filter) {
                mmdb_rwlock_rdlock(c->coll_lock);
                filter_rc = build_filter_ctx(c, where, &fp, &filter_ctx);
                mmdb_rwlock_unlock(c->coll_lock, 0);
                if (filter_rc != MMDB_OK) {
                    free(where);
                    mmdb_filter_params_free(&fp);
                    return filter_rc;
                }
            }

            /* 索引搜索：获取候选 ID 和距离 */
            int32_t search_k = (int32_t)(top_k * 2);  /* 多取一些候选以补偿删除标记 */
            float* distances = (float*)calloc(search_k, sizeof(float));
            int32_t* hnsw_ids = (int32_t*)calloc(search_k, sizeof(int32_t));
            if (!distances || !hnsw_ids) {
                free(distances);
                free(hnsw_ids);
                free(where);
                free_filter_ctx(&filter_ctx);
                mmdb_filter_params_free(&fp);
                return MMDB_ERR_NOMEM;
            }

            /* 读操作：获取读锁（支持并发读） */
            mmdb_rwlock_rdlock(c->coll_lock);
            int32_t found = mmdb_flat_search_filtered(
                w->index, (const float*)q->query_vector,
                search_k,
                has_filter ? hnsw_filter_predicate : NULL,
                has_filter ? (void*)&filter_ctx : NULL,
                hnsw_ids, distances);
            mmdb_rwlock_unlock(c->coll_lock, 0);

            if (found <= 0) {
                free(distances);
                free(hnsw_ids);
                free(where);
                free_filter_ctx(&filter_ctx);
                mmdb_filter_params_free(&fp);
                /* P6-M1.2：记录查询（空结果也算成功） */
                clock_gettime(CLOCK_MONOTONIC, &q_end);
                mmdb_metrics_record_query(
                    (double)(q_end.tv_sec - q_start.tv_sec) * 1000.0 +
                    (double)(q_end.tv_nsec - q_start.tv_nsec) / 1e6, 1);
                return MMDB_OK;  /* 无结果 */
            }

            /* 构建候选堆（过滤已删除的向量） */
            knn_cand_t* heap = (knn_cand_t*)calloc(top_k + 16, sizeof(knn_cand_t));
            if (!heap) {
                free(distances);
                free(hnsw_ids);
                free(where);
                free_filter_ctx(&filter_ctx);
                mmdb_filter_params_free(&fp);
                return MMDB_ERR_NOMEM;
            }
            size_t heap_size = 0;

            for (int32_t i = 0; i < found; i++) {
                int32_t h_id = hnsw_ids[i];
                /* 跳过已删除的向量 */
                if (h_id < 0 || (size_t)h_id >= w->id_map->count) continue;
                if (w->id_map->sdk_ids[h_id] == NULL) continue;  /* 已标记删除 */

                size_t sdk_id_len = w->id_map->sdk_id_lens[h_id];
                const uint8_t* sdk_id = w->id_map->sdk_ids[h_id];

                /* 最大堆维护 top-K */
                if (heap_size < top_k) {
                    knn_cand_t* cand = &heap[heap_size];
                    cand->dist = distances[i];
                    size_t cp = sdk_id_len < sizeof(cand->id) ? sdk_id_len : sizeof(cand->id);
                    memcpy(cand->id, sdk_id, cp);
                    cand->id_len = cp;
                    heap_siftup(heap, heap_size);
                    heap_size++;
                } else if (distances[i] < heap[0].dist) {
                    heap[0].dist = distances[i];
                    size_t cp = sdk_id_len < sizeof(heap[0].id) ? sdk_id_len : sizeof(heap[0].id);
                    memcpy(heap[0].id, sdk_id, cp);
                    heap[0].id_len = cp;
                    heap_siftdown(heap, heap_size, 0);
                }
            }

            free(distances);
            free(hnsw_ids);
            free(where);
            free_filter_ctx(&filter_ctx);

            /* 排序候选（按距离升序） */
            qsort(heap, heap_size, sizeof(knn_cand_t), cand_cmp);

            out->items = (mmdb_result_item_t*)calloc(heap_size, sizeof(mmdb_result_item_t));
            if (!out->items) {
                free(heap);
                mmdb_filter_params_free(&fp);
                return MMDB_ERR_NOMEM;
            }
            out->count = heap_size;

            /* 复制 ID + distance（不依赖 SQLite） */
            for (size_t i = 0; i < heap_size; i++) {
                out->items[i].id = (uint8_t*)malloc(heap[i].id_len);
                if (!out->items[i].id) {
                    for (size_t j = 0; j < i; j++) free(out->items[j].id);
                    free(out->items);
                    out->items = NULL;
                    free(heap);
                    mmdb_filter_params_free(&fp);
                    return MMDB_ERR_NOMEM;
                }
                memcpy((void*)out->items[i].id, heap[i].id, heap[i].id_len);
                out->items[i].id_len = heap[i].id_len;
                out->items[i].distance = heap[i].dist;
                out->items[i].metadata_json = NULL;
                out->items[i].text = NULL;
            }

            /* 批量回查 SQLite：一次查询取所有 metadata/text（O(1) round-trip） */
            if (heap_size > 0) {
                char tname[128];
                build_table_name(tname, sizeof(tname), c->name);

                /* 构造 WHERE id IN (?, ?, ?, ...) SQL */
                char back_sql[512];
                int pos = snprintf(back_sql, sizeof(back_sql),
                                   "SELECT id, metadata, text FROM %s WHERE id IN (",
                                   tname);
                for (size_t i = 0; i < heap_size && pos < (int)sizeof(back_sql) - 4; i++) {
                    int n = snprintf(back_sql + pos, sizeof(back_sql) - pos,
                                     "%s?", (i == 0) ? "" : ",");
                    if (n < 0 || n >= (int)sizeof(back_sql) - pos) break;
                    pos += n;
                }
                if (pos < (int)sizeof(back_sql) - 2) {
                    back_sql[pos++] = ')';
                    back_sql[pos++] = ';';
                    back_sql[pos] = '\0';
                }

                /* 读操作：单次获取读锁，整批回查 */
                mmdb_rwlock_rdlock(c->coll_lock);
                sqlite3_stmt* back_stmt = mmdb_sqlite_prepare(c->sdb, back_sql, NULL, 0);
                if (back_stmt) {
                    /* 绑定所有 ID */
                    for (size_t i = 0; i < heap_size; i++) {
                        sqlite3_bind_blob(back_stmt, (int)(i + 1),
                                          heap[i].id, (int)heap[i].id_len,
                                          SQLITE_TRANSIENT);
                    }
                    /* 单次扫描建立 id → (metadata, text) 映射 */
                    while (sqlite3_step(back_stmt) == SQLITE_ROW) {
                        const void* row_id = sqlite3_column_blob(back_stmt, 0);
                        int row_id_len = sqlite3_column_bytes(back_stmt, 0);
                        const char* meta = (const char*)sqlite3_column_text(back_stmt, 1);
                        const char* txt = (const char*)sqlite3_column_text(back_stmt, 2);

                        /* 在 out->items 中查找匹配的 ID（线性扫描，K 通常 <= 10） */
                        for (size_t i = 0; i < heap_size; i++) {
                            if (out->items[i].id_len == (size_t)row_id_len &&
                                memcmp(out->items[i].id, row_id, row_id_len) == 0) {
                                if (meta && meta[0] != '\0') {
                                    out->items[i].metadata_json = strdup(meta);
                                }
                                if (txt && txt[0] != '\0') {
                                    out->items[i].text = strdup(txt);
                                }
                                break;
                            }
                        }
                    }
                    sqlite3_finalize(back_stmt);
                }
                mmdb_rwlock_unlock(c->coll_lock, 0);
            }

            free(heap);
            mmdb_filter_params_free(&fp);
            /* P6-M1.1：在返回前应用 offset/limit 分页 */
            apply_pagination(out, q);
            /* P6-M1.2：记录查询成功 */
            clock_gettime(CLOCK_MONOTONIC, &q_end);
            mmdb_metrics_record_query(
                (double)(q_end.tv_sec - q_start.tv_sec) * 1000.0 +
                (double)(q_end.tv_nsec - q_start.tv_nsec) / 1e6, 1);
            return MMDB_OK;
        }
    }

    free(where);  /* 释放 filter（内存索引路径已提前释放） */

    /* Flat 暴力扫描路径 */
    char tname[128];
    build_table_name(tname, sizeof(tname), c->name);
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT id, vector, metadata, text FROM %s;", tname);

    sqlite3_stmt* stmt = mmdb_sqlite_prepare(c->sdb, sql, NULL, 0);
    if (!stmt) {
        mmdb_filter_params_free(&fp);
        return MMDB_ERR_IO;
    }

    int bind_idx = mmdb_filter_bind(stmt, &fp, 1);
    mmdb_filter_params_free(&fp);
    if (bind_idx < 0) {
        sqlite3_finalize(stmt);
        return MMDB_ERR_INVALID;
    }

    knn_cand_t* heap = (knn_cand_t*)calloc(top_k + 16, sizeof(knn_cand_t));
    if (!heap) {
        sqlite3_finalize(stmt);
        return MMDB_ERR_NOMEM;
    }
    size_t heap_size = 0;

    /* 读操作：获取读锁（支持并发读） */
    mmdb_rwlock_rdlock(c->coll_lock);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const void* id_blob = sqlite3_column_blob(stmt, 0);
        size_t id_len = (size_t)sqlite3_column_bytes(stmt, 0);
        const void* vec_blob = sqlite3_column_blob(stmt, 1);
        int vec_bytes = sqlite3_column_bytes(stmt, 1);
        size_t dim = (size_t)vec_bytes / sizeof(float);

        if (dim != q->dim) continue;

        /* L2 平方距离（SIMD 加速） */
        const float* a = (const float*)q->query_vector;
        const float* b = (const float*)vec_blob;
        float dist = 0.0f;

#if defined(__AVX2__) || defined(_MSC_VER)
        static int avx2_checked = 0;
        static int avx2_available = 0;
        if (!avx2_checked) {
            avx2_available = cpu_has_avx2();
            avx2_checked = 1;
        }
        if (avx2_available) {
            dist = l2_distance_simd(a, b, dim);
        } else {
            dist = l2_distance_scalar(a, b, dim);
        }
#else
        dist = l2_distance_scalar(a, b, dim);
#endif

        /* 使用最大堆维护 top-K 候选（堆顶为最大距离，新距离更小则替换） */
        if (heap_size < top_k) {
            /* 堆未满，直接放入堆尾并上浮 */
            knn_cand_t* cand = &heap[heap_size];
            cand->dist = dist;
            size_t cp = id_len < sizeof(cand->id) ? id_len : sizeof(cand->id);
            memcpy(cand->id, id_blob, cp);
            cand->id_len = cp;
            heap_siftup(heap, heap_size);
            heap_size++;
        } else if (dist < heap[0].dist) {
            /* 新距离小于堆顶（最大值），替换堆顶并下沉 */
            heap[0].dist = dist;
            size_t cp = id_len < sizeof(heap[0].id) ? id_len : sizeof(heap[0].id);
            memcpy(heap[0].id, id_blob, cp);
            heap[0].id_len = cp;
            heap_siftdown(heap, heap_size, 0);
        }
    }
    sqlite3_finalize(stmt);
    mmdb_rwlock_unlock(c->coll_lock, 0);

    /* 排序并填充结果 */
    qsort(heap, heap_size, sizeof(knn_cand_t), cand_cmp);

    out->items = (mmdb_result_item_t*)calloc(heap_size, sizeof(mmdb_result_item_t));
    if (!out->items) {
        free(heap);
        return MMDB_ERR_NOMEM;
    }

    out->count = heap_size;
    for (size_t i = 0; i < heap_size; i++) {
        out->items[i].id = (uint8_t*)malloc(heap[i].id_len);
        if (!out->items[i].id) {
            /* 回滚已分配的 id */
            for (size_t j = 0; j < i; j++) free(out->items[j].id);
            free(out->items);
            out->items = NULL;
            free(heap);
            return MMDB_ERR_NOMEM;
        }
        memcpy((void*)out->items[i].id, heap[i].id, heap[i].id_len);
        out->items[i].id_len = heap[i].id_len;
        out->items[i].distance = heap[i].dist;
        out->items[i].metadata_json = NULL;
        out->items[i].text = NULL;
    }
    free(heap);
    /* P6-M1.1：在返回前应用 offset/limit 分页 */
    apply_pagination(out, q);
    /* P6-M1.2：记录查询成功 */
    clock_gettime(CLOCK_MONOTONIC, &q_end);
    mmdb_metrics_record_query(
        (double)(q_end.tv_sec - q_start.tv_sec) * 1000.0 +
        (double)(q_end.tv_nsec - q_start.tv_nsec) / 1e6, 1);
    return MMDB_OK;
}
/* ------------------------------------------------------------------ */
/* P5-6：能力开关 API                                                  */
/* ------------------------------------------------------------------ */

/* 为非 VECTOR 集合启用向量检索能力。
 *
 * 行为：
 *   1. 集合 has_vector 已为 1：幂等返回 MMDB_OK
 *   2. 集合 has_vector 为 0：置位 has_vector = 1 并立即创建 mmdb_vec_<name> 表，
 *      使后续 mmdb_vectors_add/search/get/delete/upsert 在该集合上生效
 *
 * 注：此 API 不会修改 collection->model，仅切换运行时 capability 标志。
 *     VECTOR 集合调用此 API 也是幂等的（has_vector 早已为 1）。*/
int mmdb_vectors_enable(mmdb_collection_t* c) {
    if (!c) return MMDB_ERR_INVALID;
    if (c->has_vector) return MMDB_OK;
    c->has_vector = 1;
    return mmdb_vectors_ensure_table(c);
}
