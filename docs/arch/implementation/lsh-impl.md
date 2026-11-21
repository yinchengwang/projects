> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# LSH 实现详解

## 1. 文件结构

```
engine/include/db/index/vector_index/lsh/
└── lsh.h                      # 公共 API 头文件

engine/src/db/index/vector_index/lsh/
└── lsh.c                      # 完整实现（单文件）

engine/test/db/index/vector_index/lsh/
└── lsh_test.cpp               # 单元测试
```

## 2. 核心数据结构

### 2.1 LSH 索引结构

```c
struct lsh_index {
    /* 基础属性 */
    lsh_type_t type;             // LSH 类型（Bitwise/p-stable/SimHash）
    int n_hash;                  // 每个表的哈希函数数
    int n_tables;                // 哈希表数量
    int dims;                    // 向量维度
    int n_vectors;               // 已添加向量数
    int max_vectors;             // 最大向量数

    /* 哈希函数 */
    lsh_hash_func_t *hash_funcs; // 哈希函数 [n_tables][n_hash]

    /* 哈希表 */
    lsh_table_t *tables;         // 哈希表 [n_tables]

    /* 向量存储（用于精确距离计算） */
    float *vectors;              // 浮点向量 [max_vectors, dims]
    uint8_t *binary_vectors;     // 二值向量 [max_vectors, binary_size]
    int *vector_ids;             // 向量 ID [max_vectors]

    /* Phase 2: 存储后端集成 */
    storage_backend_t *storage;         // 存储后端
    heap_vector_store_t *heap_store;    // 向量主存储
    vector_ref_t *vector_refs;          // 向量引用数组
    uint32_t vector_refs_size;          // 引用数组大小
    persist_control_t *persist_ctrl;    // 持久化控制
};
```

### 2.2 哈希函数结构

```c
/* p-stable LSH 哈希函数参数 */
typedef struct lsh_pstable_func {
    float *a;       // 随机投影向量 [dims]
    float b;        // 偏移量 [0, W)
} lsh_pstable_func_t;

/* SimHash 哈希函数参数 */
typedef struct lsh_simhash_func {
    float *v;       // 随机超平面向量 [dims]
} lsh_simhash_func_t;

/* 哈希函数联合结构 */
typedef struct lsh_hash_func {
    lsh_type_t type;
    union {
        struct {
            int *indices;       // 采样的维度索引
            float *thresholds;  // 阈值
            int n_samples;
        } bitwise;
        lsh_pstable_func_t pstable;
        lsh_simhash_func_t simhash;
    } params;
} lsh_hash_func_t;
```

### 2.3 哈希表结构

```c
/* 哈希桶条目 */
typedef struct lsh_bucket_entry {
    int id;                     // 向量 ID
    float *vector;              // 向量副本（用于精确距离计算）
    int is_binary;              // 是否为二值向量
    struct lsh_bucket_entry *next;
} lsh_bucket_entry_t;

/* 哈希表 */
typedef struct lsh_table {
    lsh_bucket_entry_t **buckets;  // 桶数组
    int n_buckets;                  // 桶数量
    int *bucket_sizes;              // 每个桶的大小
    int capacity;                   // 初始桶容量
} lsh_table_t;
```

## 3. 完整流程

### 3.1 创建索引

**入口函数**: `lsh_create()`

```c
lsh_index_t *lsh_create(lsh_type_t type, int n_hash, int n_tables, int dims)
{
    lsh_index_t *idx = (lsh_index_t *)calloc(1, sizeof(lsh_index_t));

    idx->type = type;
    idx->n_hash = n_hash;
    idx->n_tables = n_tables;
    idx->dims = dims;
    idx->max_vectors = 10000;  // 默认容量

    // 分配哈希函数
    idx->hash_funcs = (lsh_hash_func_t *)calloc(
        n_tables * n_hash, sizeof(lsh_hash_func_t)
    );

    // 分配哈希表
    idx->tables = (lsh_table_t *)calloc(n_tables, sizeof(lsh_table_t));
    for (int t = 0; t < n_tables; t++) {
        idx->tables[t].n_buckets = 1 << n_hash;  // 2^n_hash
        idx->tables[t].buckets = (lsh_bucket_entry_t **)calloc(
            idx->tables[t].n_buckets, sizeof(lsh_bucket_entry_t *)
        );
    }

    // 分配向量存储
    idx->vectors = (float *)calloc(idx->max_vectors * dims, sizeof(float));
    idx->vector_ids = (int *)calloc(idx->max_vectors, sizeof(int));

    // 生成随机哈希函数
    lsh_generate_hash_functions(idx);

    return idx;
}
```

### 3.2 训练（生成哈希函数）

**入口函数**: `lsh_train()`

```c
int lsh_train(lsh_index_t *idx, int n, const float *vectors)
{
    // 根据类型生成哈希函数参数
    for (int t = 0; t < idx->n_tables; t++) {
        for (int h = 0; h < idx->n_hash; h++) {
            lsh_hash_func_t *func = &idx->hash_funcs[t * idx->n_hash + h];
            func->type = idx->type;

            switch (idx->type) {
                case LSH_PSTABLE:
                    // 随机投影向量（高斯分布）
                    func->params.pstable.a = generate_random_vector(idx->dims);
                    func->params.pstable.b = random_float(0.0f, LSH_W);
                    break;

                case LSH_SIMHASH:
                    // 随机超平面
                    func->params.simhash.v = generate_random_unit_vector(idx->dims);
                    break;

                case LSH_BITWISE:
                    // 随机采样维度
                    func->params.bitwise.indices = generate_random_indices(idx->n_hash);
                    func->params.bitwise.n_samples = idx->n_hash;
                    break;
            }
        }
    }

    return 0;
}
```

### 3.3 添加向量

**入口函数**: `lsh_add()`

**Phase 2 改造后的流程**：

```c
int lsh_add(lsh_index_t *idx, int n, const float *vectors, const int *ids)
{
    for (int i = 0; i < n; i++) {
        const float *vector = &vectors[i * idx->dims];
        int vec_id = ids[i];

        // 阶段 1: 存储向量
        if (idx->heap_store != NULL) {
            // Heap 模式：写入 Heap 并记录引用
            vector_ref_t ref = heap_vector_insert(idx->heap_store, vector);
            idx->vector_refs[vec_id] = ref;
        } else {
            // 兼容模式：直接存入 vectors 数组
            memcpy(&idx->vectors[vec_id * idx->dims], vector,
                   idx->dims * sizeof(float));
        }

        // 阶段 2: 计算哈希码并插入所有表
        for (int t = 0; t < idx->n_tables; t++) {
            uint64_t signature = lsh_compute_signature(idx, vector, t);
            lsh_bucket_entry_t *entry = lsh_bucket_entry_create(
                vec_id, vector, idx->dims
            );
            lsh_table_insert(&idx->tables[t], signature, entry);
        }

        idx->n_vectors++;
    }

    return 0;
}
```

**关键差异**：
- **Heap 模式**：向量存储在 `heap_vector_store_t`，索引只存 `vector_ref_t`
- **兼容模式**：向量直接存储在 `idx->vectors` 数组

### 3.4 搜索向量

**入口函数**: `lsh_search()`

```c
int lsh_search(const lsh_index_t *idx, const float *query, int k,
               int *ids, float *distances)
{
    // 阶段 1: 收集候选集
    int candidates[LSH_MAX_CANDIDATES];
    int candidate_count = 0;

    // 遍历所有哈希表
    for (int t = 0; t < idx->n_tables; t++) {
        // 计算查询哈希码
        uint64_t signature = lsh_compute_signature(idx, query, t);

        // 从桶中取出所有向量 ID
        lsh_bucket_entry_t *entry = idx->tables[t].buckets[signature];
        while (entry != NULL && candidate_count < LSH_MAX_CANDIDATES) {
            candidates[candidate_count++] = entry->id;
            entry = entry->next;
        }
    }

    // 去重
    candidate_count = deduplicate_candidates(candidates, candidate_count);

    // 阶段 2: 精确距离计算（重排序）
    float candidate_dists[LSH_MAX_CANDIDATES];
    float candidate_vectors[LSH_MAX_CANDIDATES * idx->dims];

    for (int i = 0; i < candidate_count; i++) {
        int vec_id = candidates[i];

        if (idx->heap_store != NULL) {
            // Heap 模式：从 Heap 获取向量
            heap_vector_get(idx->heap_store,
                           &idx->vector_refs[vec_id],
                           &candidate_vectors[i * idx->dims]);
        } else {
            // 兼容模式：直接从 vectors 数组读取
            memcpy(&candidate_vectors[i * idx->dims],
                   &idx->vectors[vec_id * idx->dims],
                   idx->dims * sizeof(float));
        }

        // 计算距离
        candidate_dists[i] = distance_l2(
            query, &candidate_vectors[i * idx->dims], idx->dims
        );
    }

    // 阶段 3: 排序取 top-k
    partial_sort_top_k(candidates, candidate_dists, candidate_count, k);

    // 填充输出
    for (int i = 0; i < k && i < candidate_count; i++) {
        ids[i] = candidates[i];
        distances[i] = candidate_dists[i];
    }

    return (k < candidate_count) ? k : candidate_count;
}
```

### 3.5 哈希码计算

**p-stable LSH**：

```c
static uint64_t lsh_compute_signature_pstable(
    const lsh_index_t *idx,
    const float *vec,
    int table_idx
) {
    uint64_t signature = 0;

    for (int h = 0; h < idx->n_hash; h++) {
        lsh_hash_func_t *func = &idx->hash_funcs[table_idx * idx->n_hash + h];

        // 计算投影值
        float projection = 0.0f;
        for (int d = 0; d < idx->dims; d++) {
            projection += func->params.pstable.a[d] * vec[d];
        }

        // 桶索引 = floor((projection + b) / W)
        int bucket = (int)floorf((projection + func->params.pstable.b) / LSH_W);

        // 组合到签名
        signature = (signature << 1) | (bucket & 1);
    }

    return signature;
}
```

**SimHash**：

```c
static uint64_t lsh_compute_signature_simhash(
    const lsh_index_t *idx,
    const float *vec,
    int table_idx
) {
    uint64_t signature = 0;

    for (int h = 0; h < idx->n_hash; h++) {
        lsh_hash_func_t *func = &idx->hash_funcs[table_idx * idx->n_hash + h];

        // 计算超平面投影
        float projection = 0.0f;
        for (int d = 0; d < idx->dims; d++) {
            projection += func->params.simhash.v[d] * vec[d];
        }

        // 符号函数
        signature = (signature << 1) | (projection >= 0 ? 1 : 0);
    }

    return signature;
}
```

## 4. Phase 2 改造要点

### 4.1 向量存储辅助函数

**存储向量**（lsh.c:125-159）：
```c
static int _lsh_store_vector(lsh_index_t *idx, const float *vector, int vec_id)
{
    if (idx->heap_store != NULL) {
        // 扩展 vector_refs 数组
        uint32_t new_size = vec_id + 1;
        vector_ref_t *new_refs = realloc(
            idx->vector_refs,
            new_size * sizeof(vector_ref_t)
        );
        idx->vector_refs = new_refs;
        idx->vector_refs_size = new_size;

        // 写入 Heap 并记录引用
        vector_ref_t ref = heap_vector_insert(idx->heap_store, vector);
        idx->vector_refs[vec_id] = ref;
        return 0;
    }

    // 兼容模式
    memcpy(&idx->vectors[vec_id * idx->dims], vector,
           idx->dims * sizeof(float));
    return 0;
}
```

**加载向量**（lsh.c:169-191）：
```c
static int _lsh_load_vector(const lsh_index_t *idx, int vec_id, float *out)
{
    if (idx->heap_store != NULL) {
        return heap_vector_get(
            idx->heap_store,
            &idx->vector_refs[vec_id],
            out
        );
    }

    // 兼容模式
    memcpy(out, &idx->vectors[vec_id * idx->dims],
           idx->dims * sizeof(float));
    return 0;
}
```

### 4.2 两阶段搜索模式

**阶段 1**：哈希碰撞获取候选 ID（快速）

**阶段 2**：从 Heap 获取向量，计算精确距离（精确）

```c
// 搜索代码片段
for (int i = 0; i < candidate_count; i++) {
    float vec_buf[idx->dims];

    if (idx->heap_store != NULL) {
        // Heap 模式：从 Heap 取向量
        heap_vector_get(idx->heap_store, &idx->vector_refs[candidates[i]], vec_buf);
    } else {
        // 兼容模式：从数组取向量
        memcpy(vec_buf, &idx->vectors[candidates[i] * idx->dims], idx->dims * sizeof(float));
    }

    distances[i] = distance_l2(query, vec_buf, idx->dims);
}
```

## 5. 参数调优

### 5.1 参数选择

| 参数 | 作用 | 增大时影响 |
|------|------|-----------|
| `n_tables` (L) | 召回率 | 召回率↑ 内存↑ |
| `n_hash` (k) | 精度 | 精度↑ 召回率↓ |
| `W` (桶宽度) | 容错 | 召回率↑ 精度↓ |

### 5.2 典型配置

| 数据规模 | n_tables | n_hash | W | 预期召回率 |
|----------|----------|--------|---|-----------|
| 100万 | 32 | 18 | 4.0 | ~80% |
| 1000万 | 64 | 20 | 4.0 | ~75% |
| 1亿 | 128 | 22 | 4.0 | ~70% |

## 6. 测试用例

### 单元测试

```cpp
TEST(LSHTest, CreateAndSearch)
{
    // 1. 创建索引
    lsh_index_t *idx = lsh_create(LSH_PSTABLE, 20, 8, 128);

    // 2. 训练（生成哈希函数）
    float train_vectors[1000][128];
    for (int i = 0; i < 1000; i++) {
        fill_random(train_vectors[i], 128);
    }
    lsh_train(idx, 1000, (float *)train_vectors);

    // 3. 添加向量
    float vectors[100][128];
    int ids[100];
    for (int i = 0; i < 100; i++) {
        fill_random(vectors[i], 128);
        ids[i] = i;
    }
    lsh_add(idx, 100, (float *)vectors, ids);

    // 4. 搜索
    float query[128];
    fill_random(query, 128);

    int result_ids[10];
    float result_dists[10];
    int count = lsh_search(idx, query, 10, result_ids, result_dists);

    EXPECT_GT(count, 0);  // 至少有候选
    EXPECT_LT(count, 100);  // 不超过候选上限

    lsh_destroy(idx);
}
```

### Heap 模式测试

```cpp
TEST(LSHTest, HeapStoreMode)
{
    // 创建存储后端
    storage_backend_t *backend = storage_memory_create(8192);

    // 创建 Heap 向量存储
    heap_vector_config_t config = {
        .backend = backend,
        .dims = 128,
        .page_size = 8192
    };
    heap_vector_store_t *heap_store = heap_vector_store_create(&config);

    // 创建 LSH 索引并绑定 Heap
    lsh_index_t *idx = lsh_create(LSH_PSTABLE, 20, 8, 128);
    idx->heap_store = heap_store;

    // ... 训练、添加、搜索 ...

    // 验证向量存储在 Heap 中
    EXPECT_GT(heap_vector_count(heap_store), 0);

    heap_vector_store_destroy(heap_store);
    storage_backend_destroy(backend);
    lsh_destroy(idx);
}
```

## 7. 参考资料

- Indyk & Motwani (1998). "Approximate Nearest Neighbors: Towards Removing the Curse of Dimensionality"
- Datar et al. (2004). "Locality-Sensitive Hashing Scheme Based on p-Stable Distributions"
- Gionis et al. (1999). "Similarity Search in High Dimensions via Hashing"