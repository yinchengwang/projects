> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# IVF 实现详解

## 1. 文件结构

```
engine/include/db/index/vector_index/ivf/
├── IndexIVF.h                 # 公共 API 头文件
├── inverted_lists.h           # 倒排列表接口
├── direct_map.h               # ID 到位置映射
└── ivf_log.h                  # 日志接口

engine/src/db/index/vector_index/ivf/
├── faiss_ivf_core.c           # 核心结构与生命周期
├── faiss_ivf_train.c          # K-Means 训练
├── faiss_ivf_add.c            # 向量添加
├── faiss_ivf_search.c         # 向量搜索
├── faiss_inverted_lists.c     # 倒排列表实现
├── faiss_direct_map.c         # DirectMap 实现
└── faiss_ivf_utils.c          # 工具函数

engine/test/db/index/vector_index/ivf/
└── ivf_test.cpp               # 单元测试
```

## 2. 核心数据结构

### 2.1 索引主体结构

```c
typedef struct faiss_ivf {
    /* 基础属性 */
    int32_t dims;                    // 向量维度
    int32_t nlist;                   // 一级簇数量
    int32_t nlist2;                  // 二级桶数量（每簇）
    int32_t nprobe;                  // 搜索时访问的簇数
    distance_metric_t metric;        // 距离度量
    quantization_type_t quantization_type;

    /* 质心数组 */
    float *primary_centroids;        // 一级中心 [nlist, dims]
    float *secondary_centroids;      // 二级中心 [nlist, nlist2, dims]
    int32_t *secondary_counts;       // 每个一级簇的二级桶数 [nlist]

    /* 向量存储 */
    float *vectors;                  // 向量数据（原始模式）
    int32_t n_total;                 // 向量总数
    int32_t vector_capacity;         // 向量容量

    /* 倒排索引 */
    faiss_inverted_lists_t *invlists; // 倒排列表

    /* DirectMap: id → (bucket_id, offset) */
    faiss_direct_map_t direct_map;

    /* 量化器 */
    quantizer_t *quantizer;          // 量化器（可选）
    uint8_t *codes;                  // 量化编码 [n_total, code_size]
    int32_t code_size;               // 编码大小

    /* 训练状态 */
    bool trained;                    // 是否已训练

    /* Phase 2: 存储后端集成 */
    storage_backend_t *storage;      // 存储后端
    heap_vector_store_t *heap_store; // 向量主存储
    vector_ref_t *vector_refs;       // 向量引用数组
    uint32_t vector_refs_size;       // 引用数组大小
    persist_control_t *persist_ctrl; // 持久化控制
} faiss_ivf_t;
```

### 2.2 倒排列表结构

```c
typedef struct faiss_inverted_lists {
    /* 每个桶的向量 ID 列表 */
    int32_t **ids;                   // ids[bucket_id] → 向量 ID 数组
    uint8_t **codes;                 // codes[bucket_id] → 量化编码（可选）
    size_t *list_sizes;              // list_sizes[bucket_id] → 桶大小
    size_t *list_capacities;         // 桶容量

    size_t nlist;                    // 总桶数
    size_t max_list_size;            // 最大桶大小
} faiss_inverted_lists_t;
```

### 2.3 DirectMap 结构

```c
typedef struct faiss_direct_map {
    /* id → (bucket_id, offset) 映射 */
    int32_t *bucket_ids;             // bucket_ids[vec_id] → 桶 ID
    int64_t *offsets;                // offsets[vec_id] → 桶内偏移

    int32_t size;                    // 映射条目数
    int32_t capacity;                // 容量
} faiss_direct_map_t;
```

## 3. 完整流程

### 3.1 创建索引

**入口函数**: `faiss_ivf_index_create()`

```c
faiss_ivf_t *faiss_ivf_index_create(
    int32_t dims,
    int32_t nlist,
    int32_t nlist2,
    int32_t nprobe,
    distance_metric_t metric,
    quantization_type_t quantization_type
) {
    faiss_ivf_t *index = (faiss_ivf_t *)calloc(1, sizeof(faiss_ivf_t));

    index->dims = dims;
    index->nlist = nlist;
    index->nlist2 = nlist2;
    index->nprobe = nprobe;
    index->metric = metric;
    index->quantization_type = quantization_type;

    // 分配质心数组
    index->primary_centroids = (float *)calloc(nlist * dims, sizeof(float));
    index->secondary_centroids = (float *)calloc(nlist * nlist2 * dims, sizeof(float));
    index->secondary_counts = (int32_t *)calloc(nlist, sizeof(int32_t));

    // 创建倒排列表
    index->invlists = faiss_inverted_lists_create(nlist * nlist2);

    // 初始化 DirectMap
    faiss_direct_map_init(&index->direct_map);

    return index;
}
```

### 3.2 训练（K-Means）

**入口函数**: `faiss_ivf_index_train()`

```c
int32_t faiss_ivf_index_train(faiss_ivf_t *index, int32_t n, const float *vectors)
{
    // 阶段 1: 训练一级质心
    kmeans_result_t *primary_result = kmeans(
        vectors, n, index->dims, index->nlist
    );
    memcpy(index->primary_centroids, primary_result->centroids,
           index->nlist * index->dims * sizeof(float));

    // 阶段 2: 对每个一级簇训练二级质心
    for (int32_t p = 0; p < index->nlist; p++) {
        // 收集属于该簇的向量
        float *cluster_vectors = collect_vectors_for_cluster(
            vectors, n, primary_result->assignments, p
        );
        int32_t cluster_size = count_vectors_for_cluster(
            primary_result->assignments, n, p
        );

        // 训练二级质心
        kmeans_result_t *secondary_result = kmeans(
            cluster_vectors, cluster_size, index->dims, index->nlist2
        );

        // 存储二级质心
        memcpy(
            &index->secondary_centroids[p * index->nlist2 * index->dims],
            secondary_result->centroids,
            index->nlist2 * index->dims * sizeof(float)
        );
        index->secondary_counts[p] = index->nlist2;

        free(cluster_vectors);
        kmeans_result_free(secondary_result);
    }

    // 阶段 3: 训练量化器（如果启用）
    if (index->quantization_type != QUANTIZATION_TYPE_NONE) {
        // 计算残差并训练 PQ
        quantizer_train(index->quantizer, n, vectors);
    }

    index->trained = true;
    return 0;
}
```

### 3.3 添加向量

**入口函数**: `faiss_ivf_index_add()`

**Phase 2 改造后的流程**：

```c
int32_t faiss_ivf_index_add(faiss_ivf_t *index, int32_t n, const float *vectors)
{
    // 扩容存储区
    if (index->heap_store == NULL) {
        // 兼容模式：扩展 vectors 数组
        float *new_vectors = realloc(index->vectors,
            (index->n_total + n) * index->dims * sizeof(float));
        index->vectors = new_vectors;
    }

    // 逐个路由向量
    for (int32_t i = 0; i < n; i++) {
        const float *vector = &vectors[i * index->dims];
        int32_t new_id = index->n_total + i;

        // 阶段 1: 存储向量
        if (index->heap_store != NULL) {
            // Heap 模式：写入 Heap 并记录引用
            vector_ref_t ref = heap_vector_insert(index->heap_store, vector);
            index->vector_refs[new_id] = ref;
        } else {
            // 兼容模式：直接存入 vectors 数组
            memcpy(&index->vectors[new_id * index->dims], vector,
                   index->dims * sizeof(float));
        }

        // 阶段 2: 两级路由
        // 2.1 找最近的一级簇
        int32_t best_primary = find_nearest_primary_centroid(
            index, vector
        );

        // 2.2 在该簇内找最近的二级桶
        int32_t best_secondary = find_nearest_secondary_centroid(
            index, best_primary, vector
        );

        // 2.3 计算桶 ID
        int32_t bucket_id = best_primary * index->nlist2 + best_secondary;

        // 阶段 3: 添加到倒排列表
        size_t offset = faiss_inverted_lists_list_size(
            index->invlists, bucket_id
        );
        faiss_inverted_lists_add_entry(
            index->invlists, bucket_id, new_id, NULL
        );

        // 阶段 4: 更新 DirectMap
        faiss_direct_map_add(&index->direct_map, new_id, bucket_id, offset);

        // 阶段 5: PQ 编码残差（如果启用）
        if (index->quantizer) {
            float residual[index->dims];
            compute_residual(residual, vector,
                &index->secondary_centroids[bucket_id * index->dims]);
            quantizer_encode(index->quantizer, residual,
                &index->codes[new_id * index->code_size]);
        }
    }

    index->n_total += n;
    return 0;
}
```

**关键差异**：
- **Heap 模式**：向量存储在 `heap_vector_store_t`，索引只存 `vector_ref_t`
- **兼容模式**：向量直接存储在 `index->vectors` 数组

### 3.4 搜索向量

**入口函数**: `faiss_ivf_index_search()`

**六阶段搜索流程**：

```c
int32_t faiss_ivf_index_search(
    faiss_ivf_t *index,
    const float *query,
    int32_t k,
    float *distances,
    int32_t *labels
) {
    // ── 第 1 阶段: 粗排 ──
    // 计算查询到所有一级中心的距离，取最近 nprobe 个
    scored_index_t *primary_clusters = malloc(index->nlist * sizeof(scored_index_t));
    for (int32_t i = 0; i < index->nlist; i++) {
        primary_clusters[i].distance = distance_compute(
            query, &index->primary_centroids[i * index->dims], index->dims
        );
        primary_clusters[i].id = i;
    }
    qsort(primary_clusters, index->nlist, sizeof(scored_index_t), compare_distance);

    // ── 第 2 阶段: 桶展开 ──
    // 展开选中的 nprobe 个一级簇的所有二级桶
    int32_t max_buckets = index->nprobe * index->nlist2;
    scored_index_t *bucket_queue = malloc(max_buckets * sizeof(scored_index_t));
    int32_t bucket_count = 0;

    for (int32_t p = 0; p < index->nprobe; p++) {
        int32_t cluster = primary_clusters[p].id;
        for (int32_t s = 0; s < index->secondary_counts[cluster]; s++) {
            int32_t bucket_id = cluster * index->nlist2 + s;
            bucket_queue[bucket_count].id = bucket_id;
            bucket_queue[bucket_count].distance = distance_compute(
                query,
                &index->secondary_centroids[bucket_id * index->dims],
                index->dims
            );
            bucket_count++;
        }
    }
    qsort(bucket_queue, bucket_count, sizeof(scored_index_t), compare_distance);

    // ── 第 3 阶段: 初始化结果大顶堆 ──
    int32_t result_ids[k];
    float result_dists[k];
    int32_t num_results = 0;

    // ── 第 4 阶段: 桶扫描 ──
    float *distance_table = NULL;
    if (index->quantizer) {
        distance_table = malloc(
            quantizer_distance_table_size(index->quantizer) * sizeof(float)
        );
    }

    for (int32_t bi = 0; bi < bucket_count; bi++) {
        int32_t bucket_id = bucket_queue[bi].id;

        // 量化模式：计算距离表
        if (index->quantizer) {
            float residual[index->dims];
            compute_residual(residual, query,
                &index->secondary_centroids[bucket_id * index->dims]);
            quantizer_compute_distance_table(
                index->quantizer, residual, distance_table
            );
        }

        // 遍历桶内向量
        size_t list_size = faiss_inverted_lists_list_size(index->invlists, bucket_id);
        const int32_t *list_ids = faiss_inverted_lists_get_ids(index->invlists, bucket_id);

        for (size_t idx = 0; idx < list_size; idx++) {
            int32_t vec_id = list_ids[idx];
            if (vec_id < 0) continue;  // 跳过墓碑

            float dist;
            if (index->quantizer) {
                // ADC 距离
                dist = quantizer_adc_distance(
                    index->quantizer,
                    &index->codes[vec_id * index->code_size],
                    distance_table
                );
            } else {
                // 完整距离计算
                float vec_buf[index->dims];
                faiss_ivf_load_vector(index, vec_id, vec_buf);
                dist = distance_compute(query, vec_buf, index->dims);
            }

            // 维护 top-k 大顶堆
            if (num_results < k) {
                result_ids[num_results] = vec_id;
                result_dists[num_results] = dist;
                num_results++;
                // 冒泡调整堆
                for (int32_t p = num_results - 1; p > 0; p--) {
                    if (result_dists[p] > result_dists[p - 1]) {
                        swap(&result_dists[p], &result_dists[p - 1]);
                        swap(&result_ids[p], &result_ids[p - 1]);
                    }
                }
            } else if (dist < result_dists[0]) {
                result_ids[0] = vec_id;
                result_dists[0] = dist;
                // 下滤调整堆
                for (int32_t p = 1; p < k; p++) {
                    if (result_dists[p] > result_dists[0]) {
                        swap(&result_dists[0], &result_dists[p]);
                        swap(&result_ids[0], &result_ids[p]);
                    }
                }
            }
        }
    }

    // ── 第 5 阶段: Heap 重排序（Heap 模式专属）──
    if (index->heap_store != NULL && num_results > 0) {
        float candidate_vectors[num_results * index->dims];

        // 从 Heap 批量加载向量
        for (int32_t i = 0; i < num_results; i++) {
            heap_vector_get(
                index->heap_store,
                &index->vector_refs[result_ids[i]],
                &candidate_vectors[i * index->dims]
            );
        }

        // 计算精确距离
        for (int32_t i = 0; i < num_results; i++) {
            result_dists[i] = distance_compute(
                query, &candidate_vectors[i * index->dims], index->dims
            );
        }
    }

    // ── 第 6 阶段: 结果排序输出 ──
    // 选择排序转升序
    for (int32_t out = 0; out < num_results; out++) {
        int32_t best_idx = out;
        for (int32_t j = out + 1; j < num_results; j++) {
            if (result_dists[j] < result_dists[best_idx]) {
                best_idx = j;
            }
        }
        swap(&result_dists[out], &result_dists[best_idx]);
        swap(&result_ids[out], &result_ids[best_idx]);
    }

    // 填充输出
    for (int32_t i = 0; i < num_results; i++) {
        distances[i] = result_dists[i];
        labels[i] = result_ids[i];
    }
    for (int32_t i = num_results; i < k; i++) {
        distances[i] = FLT_MAX;
        labels[i] = -1;
    }

    free(primary_clusters);
    free(bucket_queue);
    free(distance_table);
    return 0;
}
```

**六阶段说明**：
1. **粗排**：计算查询到所有一级中心的距离，取最近 nprobe 个
2. **桶展开**：将选中的 nprobe 个一级簇的所有二级桶展开
3. **堆初始化**：分配大小为 k 的大顶堆
4. **桶扫描**：遍历桶内向量，计算距离，维护 top-k 堆
5. **Heap 重排序**：从 Heap 获取向量，计算精确距离
6. **结果输出**：排序并填充输出数组

### 3.5 删除向量

**入口函数**: `faiss_ivf_index_remove_ids()`

```c
int32_t faiss_ivf_index_remove_ids(
    faiss_ivf_t *index,
    const int32_t *ids_to_remove,
    int32_t n_remove
) {
    for (int32_t i = 0; i < n_remove; i++) {
        int32_t id = ids_to_remove[i];

        // 1. 从 DirectMap 获取位置
        int32_t bucket_id;
        int64_t offset;
        faiss_direct_map_get(&index->direct_map, id, &bucket_id, &offset);

        // 2. 标记为墓碑（id = -1）
        faiss_inverted_lists_mark_deleted(index->invlists, bucket_id, offset);

        // 3. 从 DirectMap 移除
        faiss_direct_map_remove(&index->direct_map, id);
    }

    return 0;
}
```

### 3.6 紧凑化

**入口函数**: `faiss_ivf_index_compact()`

```c
void faiss_ivf_index_compact(faiss_ivf_t *index)
{
    // 遍历所有桶，移除墓碑并重建 DirectMap
    for (size_t bucket_id = 0; bucket_id < index->invlists->nlist; bucket_id++) {
        int32_t *ids = faiss_inverted_lists_get_ids(index->invlists, bucket_id);
        size_t size = faiss_inverted_lists_list_size(index->invlists, bucket_id);

        int32_t write_idx = 0;
        for (size_t read_idx = 0; read_idx < size; read_idx++) {
            if (ids[read_idx] >= 0) {
                ids[write_idx] = ids[read_idx];
                // 更新 DirectMap
                faiss_direct_map_update(
                    &index->direct_map,
                    ids[read_idx],
                    bucket_id,
                    write_idx
                );
                write_idx++;
            }
        }

        // 更新桶大小
        faiss_inverted_lists_set_size(index->invlists, bucket_id, write_idx);
    }
}
```

## 4. Phase 2 改造要点

### 4.1 向量存储辅助函数

**存储向量**（faiss_ivf_add.c:43-77）：
```c
static int faiss_ivf_store_vector(faiss_ivf_t *index, const float *vector, int32_t new_id)
{
    if (index->heap_store != NULL) {
        // 扩展 vector_refs 数组
        vector_ref_t *new_refs = realloc(
            index->vector_refs,
            (new_id + 1) * sizeof(vector_ref_t)
        );
        index->vector_refs = new_refs;
        index->vector_refs_size = new_id + 1;

        // 写入 Heap 并记录引用
        vector_ref_t ref = heap_vector_insert(index->heap_store, vector);
        index->vector_refs[new_id] = ref;
        return 0;
    }

    // 兼容模式
    memcpy(&index->vectors[new_id * index->dims], vector,
           index->dims * sizeof(float));
    return 0;
}
```

**加载向量**（faiss_ivf_search.c:52-76）：
```c
static int faiss_ivf_load_vector(const faiss_ivf_t *index, int32_t vec_id, float *out)
{
    if (index->heap_store != NULL) {
        return heap_vector_get(
            index->heap_store,
            &index->vector_refs[vec_id],
            out
        );
    }

    // 兼容模式
    memcpy(out, &index->vectors[vec_id * index->dims],
           index->dims * sizeof(float));
    return 0;
}
```

### 4.2 两阶段搜索模式

**阶段 1-4**：桶扫描获取候选 ID（可能使用 ADC 近似距离）

**阶段 5**：Heap 重排序（仅 Heap 模式）
- 从 Heap 获取完整向量
- 计算精确距离
- 重新排序取 top-k

```c
// 第 5 阶段代码片段
if (index->heap_store != NULL && num_results > 0) {
    float *candidate_vectors = malloc(num_results * dims * sizeof(float));

    for (int32_t i = 0; i < num_results; i++) {
        heap_vector_get(index->heap_store,
                       &index->vector_refs[result_ids[i]],
                       &candidate_vectors[i * dims]);
    }

    for (int32_t i = 0; i < num_results; i++) {
        result_dists[i] = distance_compute(
            query, &candidate_vectors[i * dims], dims
        );
    }

    free(candidate_vectors);
}
```

## 5. 测试用例

### 单元测试

```cpp
TEST(IVFTest, CreateAndSearch)
{
    // 1. 创建索引
    faiss_ivf_t *index = faiss_ivf_index_create(
        128,   // dims
        100,   // nlist
        10,    // nlist2
        20,    // nprobe
        DISTANCE_L2,
        QUANTIZATION_TYPE_NONE
    );

    // 2. 生成训练数据
    float train_vectors[10000][128];
    for (int i = 0; i < 10000; i++) {
        fill_random(train_vectors[i], 128);
    }

    // 3. 训练
    faiss_ivf_index_train(index, 10000, (float *)train_vectors);
    EXPECT_TRUE(faiss_ivf_index_is_trained(index));

    // 4. 添加向量
    float vectors[1000][128];
    for (int i = 0; i < 1000; i++) {
        fill_random(vectors[i], 128);
    }
    faiss_ivf_index_add(index, 1000, (float *)vectors);

    // 5. 搜索
    float query[128];
    fill_random(query, 128);

    int32_t k = 10;
    float distances[10];
    int32_t labels[10];
    faiss_ivf_index_search(index, query, k, distances, labels);

    EXPECT_GT(labels[0], -1);  // 至少有一个结果

    faiss_ivf_index_drop(index);
}
```

### Heap 模式测试

```cpp
TEST(IVFTest, HeapStoreMode)
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

    // 创建 IVF 索引并绑定 Heap
    faiss_ivf_t *index = faiss_ivf_index_create(128, 100, 10, 20, DISTANCE_L2, QUANTIZATION_TYPE_NONE);
    index->heap_store = heap_store;

    // ... 训练、添加、搜索 ...

    // 验证向量存储在 Heap 中
    EXPECT_GT(heap_vector_count(heap_store), 0);

    heap_vector_store_destroy(heap_store);
    storage_backend_destroy(backend);
    faiss_ivf_index_drop(index);
}
```

## 6. 参考资料

- Jégou et al. (2011). "Product Quantization for Nearest Neighbor Search"
- Facebook AI Research. "Faiss: A library for efficient similarity search"
- IVF-Flat vs IVF-PQ: https://faiss.ai/cpp/faq.html
