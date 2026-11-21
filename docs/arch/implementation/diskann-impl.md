> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# DiskANN 实现详解

## 1. 文件结构

```
engine/include/db/index/vector_index/diskann/
├── diskann.h                   # 公共 API 头文件
├── diskann_fresh.h             # FreshDiskANN 扩展
├── diskann_merge.h             # Merge-Vamana 扩展
├── diskann_partition.h         # 分区策略
└── diskann_spann.h             # SPANN 分区索引

engine/src/db/index/vector_index/diskann/
├── diskann_config.c            # 配置管理
├── diskann_create.c            # 索引创建与生命周期
├── diskann_insert.c            # 向量插入
├── diskann_search.c            # 向量搜索
├── diskann_graph.c             # Vamana 图构建
├── diskann_persist.c           # 持久化
├── diskann_quantization.c      # 量化压缩
├── diskann_delete.c            # 删除操作
├── diskann_fresh.c             # FreshDiskANN 实现
├── diskann_merge.c             # Merge-Vamana 实现
├── diskann_partition.c         # 分区策略实现
├── diskann_spann.c             # SPANN 实现
└── diskann_utils.c             # 工具函数

engine/test/db/index/vector_index/diskann/
└── diskann_test.cpp            # 单元测试
```

## 2. 核心数据结构

### 2.1 索引配置结构

```c
/**
 * 统一配置入口
 */
typedef struct diskann_config {
    /* 基础参数 */
    int32_t dims;                    // 向量维度
    int32_t index_size;              // R: 目标邻居数
    int32_t build_search_list_size;  // L: 构图候选数
    distance_metric_t metric;        // 距离度量

    /* 各模块配置 */
    diskann_quantization_config_t quantization; // 量化配置
    diskann_merge_config_t merge;               // Merge-Vamana 配置
    diskann_spann_config_t spann;               // SPANN 配置
    diskann_fresh_config_t fresh;               // FreshDiskANN 配置
    diskann_storage_config_t storage;           // 存储配置
} diskann_config_t;

/**
 * 量化配置
 */
typedef struct diskann_quantization_config {
    bool enabled;                    // 是否启用量化
    quantization_type_t type;        // 量化类型: PQ/SQ/LVQ/RQ
    int32_t subquantizers;           // m: 子空间数 (PQ 专用)
    int32_t bits;                    // bits: 4/6/8
    int32_t train_max_vectors;       // 训练时使用的最大向量数
} diskann_quantization_config_t;
```

### 2.2 索引主体结构（diskann_private.h）

```c
struct diskann {
    /* 基础属性 */
    int32_t dims;                    // 向量维度
    int32_t index_size;              // 目标邻居数 R
    int32_t build_search_list_size;  // 构图候选数 L
    distance_metric_t metric;        // 距离度量
    float alpha;                     // Robust Prune 参数

    /* 向量存储 */
    float *vectors;                  // 向量数据（原始模式）
    float *norms;                    // 向量范数（用于余弦距离）
    int32_t n_total;                 // 向量总数
    int32_t active_count;            // 活跃向量数（未删除）
    int32_t vector_capacity;         // 向量容量

    /* 图结构 */
    int32_t *neighbors;              // 邻居数组 [n_total * index_size]
    int32_t *neighbor_counts;        // 每节点邻居数 [n_total]
    int32_t entry_point;             // 入口点

    /* 冻结点种子 */
    int32_t *frozen_points;          // 冻结点数组
    int32_t frozen_capacity;         // 冻结点容量

    /* 量化 */
    quantizer_t *quantizer;          // 量化器
    quantizer_config_t quantizer_config;
    uint8_t *codes;                  // 量化编码 [n_total * pq_code_size]
    int32_t pq_code_size;            // 编码大小
    int32_t pq_trained_codebook_size;

    /* 删除标记 */
    bool *deleted;                   // 删除标记位图 [n_total]

    /* 构建状态 */
    bool built;                      // 是否已构建图

    /* 存储参数 */
    diskann_storage_params_t storage_params;
    diskann_quantization_params_t quantization_params;

    /* Phase 2: 存储后端集成 */
    storage_backend_t *storage;      // 存储后端
    heap_vector_store_t *heap_store; // 向量主存储
    vector_ref_t *vector_refs;       // 向量引用数组
    uint32_t vector_refs_size;       // 引用数组大小
    persist_control_t *persist_ctrl; // 持久化控制

    /* 扩展上下文 */
    diskann_config_t *config;        // 配置副本
    void *merge_ctx;                 // Merge-Vamana 上下文
    void *spann_ctx;                 // SPANN 上下文
    void *fresh_ctx;                 // FreshDiskANN 上下文
};
```

## 3. 完整流程

### 3.1 创建索引

**入口函数**: `diskann_index_create()` / `diskann_index_create_with_config()`

```c
diskann_t *diskann_index_create_with_config(const diskann_config_t *config)
{
    // 1. 验证配置
    if (diskann_config_validate(config) != 1) {
        return NULL;
    }

    // 2. 创建基础索引
    diskann_t *index = diskann_index_create(
        config->dims,
        config->index_size,
        config->build_search_list_size,
        config->metric
    );

    // 3. 设置 alpha 参数
    index->alpha = 1.2f;

    // 4. 设置存储参数
    diskann_storage_params_t params = {
        config->storage.page_size,
        config->storage.frozen_point_count
    };
    diskann_index_set_storage_params(index, &params);

    // 5. 设置量化参数（如果启用）
    if (config->quantization.enabled) {
        diskann_quantization_params_t params = {
            config->quantization.subquantizers,
            config->quantization.bits,
            config->quantization.train_max_vectors,
            true
        };
        diskann_index_set_quantization_params(index, &params);
    }

    // 6. 保存配置副本
    index->config = diskann_config_clone(config);

    return index;
}
```

**流程步骤**：
1. 验证配置有效性
2. 分配索引结构体内存
3. 设置基础参数（dims, index_size, build_search_list_size, metric）
4. 初始化默认值（alpha=1.2, entry_point=-1）
5. 分配向量、邻居、冻结点数组的初始容量
6. 设置存储和量化参数
7. 返回索引指针

### 3.2 插入向量

**入口函数**: `diskann_index_add()`

**Phase 2 改造后的流程**（使用 heap_store）：

```c
int32_t diskann_index_add(diskann_t *index, int32_t n, const float *vectors)
{
    for (int32_t i = 0; i < n; i++) {
        int32_t id = index->n_total++;

        // 阶段 1: 存储向量
        if (index->heap_store != NULL) {
            // Heap 模式：写入 Heap 并记录引用
            vector_ref_t ref = heap_vector_insert(
                index->heap_store,
                vectors + i * index->dims
            );
            if (!vector_ref_is_valid(&ref)) {
                return -1;
            }
            // 扩展 vector_refs 数组
            // ... 扩容逻辑 ...
            index->vector_refs[id] = ref;
        } else {
            // 兼容模式：直接存入 vectors 数组
            memcpy(
                &index->vectors[id * index->dims],
                vectors + i * index->dims,
                index->dims * sizeof(float)
            );
        }

        // 阶段 2: 量化（如果启用）
        if (index->quantizer != NULL) {
            quantizer_encode(
                index->quantizer,
                vectors + i * index->dims,
                &index->codes[id * index->pq_code_size]
            );
        }

        // 阶段 3: 更新删除标记
        index->deleted[id] = false;
    }

    return 0;
}
```

**关键差异**：
- **Heap 模式**：向量存储在 `heap_vector_store_t`，索引只存 `vector_ref_t`
- **兼容模式**：向量直接存储在 `index->vectors` 数组

### 3.3 构建图

**入口函数**: `diskann_index_build()`

```c
int32_t diskann_index_build(diskann_t *index)
{
    // 1. 初始化邻居数组
    for (int32_t i = 0; i < index->n_total; i++) {
        index->neighbor_counts[i] = 0;
    }

    // 2. 选择冻结点种子
    diskann_select_frozen_points(index);

    // 3. 对每个节点执行 Robust Prune
    for (int32_t i = 0; i < index->n_total; i++) {
        // 搜索候选邻居
        int32_t candidates[ index->build_search_list_size ];
        int32_t n_candidates = diskann_search_candidates(
            index, i, candidates
        );

        // Robust Prune 剪枝
        diskann_robust_prune(
            index,
            i,
            candidates,
            n_candidates,
            index->alpha
        );
    }

    // 4. 多轮剪枝优化（α 从 1.0 到 1.2 递增）
    for (float alpha = 1.0f; alpha <= 1.2f; alpha *= 1.2f) {
        // 重新执行 Robust Prune
        // ...
    }

    // 5. 设置入口点（选择连接度最高的节点）
    index->entry_point = diskann_select_entry_point(index);
    index->built = true;

    return 0;
}
```

**Robust Prune 算法**：

```c
void diskann_robust_prune(
    diskann_t *index,
    int32_t node_id,
    int32_t *candidates,
    int32_t n_candidates,
    float alpha
) {
    // 1. 按距离排序候选
    sort_candidates_by_distance(index, node_id, candidates, n_candidates);

    // 2. 从近到远依次选择邻居
    int32_t selected = 0;
    for (int32_t i = 0; i < n_candidates && selected < index->index_size; i++) {
        int32_t candidate = candidates[i];
        bool occluded = false;

        // 3. 检查是否被已有邻居"遮挡"
        for (int32_t j = 0; j < selected; j++) {
            int32_t existing = index->neighbors[node_id * index->index_size + j];

            // 计算遮挡条件
            float dist_existing_to_candidate = distance(
                index, existing, candidate
            );
            float dist_node_to_candidate = distance(
                index, node_id, candidate
            );

            if (alpha * dist_existing_to_candidate <= dist_node_to_candidate) {
                occluded = true;  // 被遮挡，跳过
                break;
            }
        }

        // 4. 如果未被遮挡，加入邻居列表
        if (!occluded) {
            index->neighbors[node_id * index->index_size + selected] = candidate;
            selected++;
        }
    }

    index->neighbor_counts[node_id] = selected;
}
```

### 3.4 搜索向量

**入口函数**: `diskann_index_search()`

**Phase 2 改造后的两阶段搜索**：

```c
int32_t diskann_index_search(
    diskann_t *index,
    const float *query,
    int32_t k,
    int32_t search_list_size,
    int32_t max_iterations,
    float *distances,
    int32_t *labels
) {
    // 阶段 1: 图索引搜索，获取候选 ID
    int32_t candidates[ search_list_size ];
    int32_t n_candidates = diskann_graph_search(
        index,
        query,
        search_list_size,
        max_iterations,
        candidates
    );

    // 阶段 2: 从 Heap 获取完整向量（如果使用 heap_store）
    float (*candidate_vectors)[index->dims];
    if (index->heap_store != NULL) {
        candidate_vectors = malloc(n_candidates * index->dims * sizeof(float));
        for (int32_t i = 0; i < n_candidates; i++) {
            int32_t id = candidates[i];
            vector_ref_t ref = index->vector_refs[id];
            heap_vector_get(index->heap_store, &ref, candidate_vectors[i]);
        }
    } else {
        // 兼容模式：直接从 vectors 数组读取
        candidate_vectors = (float(*)[])index->vectors;
    }

    // 阶段 3: 计算精确距离（重排序）
    float candidate_distances[ n_candidates ];
    for (int32_t i = 0; i < n_candidates; i++) {
        int32_t id = candidates[i];
        if (index->heap_store != NULL) {
            candidate_distances[i] = distance_l2(
                query, candidate_vectors[i], index->dims
            );
        } else {
            candidate_distances[i] = distance_l2(
                query, &index->vectors[id * index->dims], index->dims
            );
        }
    }

    // 阶段 4: 排序取 top-k
    partial_sort_top_k(candidates, candidate_distances, n_candidates, k);

    // 阶段 5: 复制结果
    for (int32_t i = 0; i < k; i++) {
        labels[i] = candidates[i];
        distances[i] = candidate_distances[i];
    }

    // 清理
    if (index->heap_store != NULL && candidate_vectors != NULL) {
        free(candidate_vectors);
    }

    return k;
}
```

**图搜索算法**（Greedy Search）：

```c
int32_t diskann_graph_search(
    diskann_t *index,
    const float *query,
    int32_t search_list_size,
    int32_t max_iterations,
    int32_t *candidates
) {
    // 1. 从冻结点出发
    int32_t current = index->entry_point;

    // 2. 贪心搜索
    bool visited[ index->n_total ];
    memset(visited, false, sizeof(visited));

    priority_queue_t pq;  // 候选队列
    priority_queue_init(&pq, search_list_size * 2);

    // 3. 从所有冻结点开始
    for (int32_t i = 0; i < index->storage_params.frozen_point_count; i++) {
        int32_t fp = index->frozen_points[i];
        if (fp >= 0 && fp < index->n_total && !index->deleted[fp]) {
            float dist = distance(index, query, fp);
            priority_queue_push(&pq, dist, fp);
            visited[fp] = true;
        }
    }

    // 4. 贪心遍历
    int32_t result_count = 0;
    while (!priority_queue_empty(&pq) && result_count < search_list_size) {
        // 取出最近邻
        pq_node_t top = priority_queue_pop(&pq);
        int32_t node = top.id;

        candidates[result_count++] = node;

        // 遍历邻居
        for (int32_t i = 0; i < index->neighbor_counts[node]; i++) {
            int32_t neighbor = index->neighbors[node * index->index_size + i];
            if (!visited[neighbor] && !index->deleted[neighbor]) {
                float dist = distance(index, query, neighbor);
                priority_queue_push(&pq, dist, neighbor);
                visited[neighbor] = true;
            }
        }
    }

    priority_queue_destroy(&pq);
    return result_count;
}
```

### 3.5 删除向量

**入口函数**: `diskann_index_delete()`

```c
int32_t diskann_index_delete(diskann_t *index, int32_t label)
{
    if (label < 0 || label >= index->n_total) {
        return -1;
    }

    // 逻辑删除：标记删除位图
    index->deleted[label] = true;
    index->active_count--;

    // 注意：图结构不立即更新
    // 搜索时会跳过已删除的节点

    return 0;
}
```

**物理删除**：通过 `diskann_index_compact()` 异步重建。

### 3.6 持久化

**入口函数**: `diskann_index_save()` / `diskann_index_load()`

```c
int32_t diskann_index_save(const diskann_t *index, const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    // 1. 写入元数据
    diskann_meta_page_t meta = {
        .dims = index->dims,
        .n_total = index->n_total,
        .index_size = index->index_size,
        .metric = index->metric,
        .has_quantization = (index->quantizer != NULL)
    };
    fwrite(&meta, sizeof(meta), 1, fp);

    // 2. 写入图结构
    fwrite(index->neighbors, sizeof(int32_t),
           (size_t)index->n_total * index->index_size, fp);
    fwrite(index->neighbor_counts, sizeof(int32_t),
           index->n_total, fp);

    // 3. 写入向量数据（如果启用 heap_store）
    if (index->heap_store != NULL) {
        // 从 Heap 持久化后端同步
        storage_backend_sync(index->heap_store->backend);
    } else {
        // 直接写入 vectors 数组
        fwrite(index->vectors, sizeof(float),
               (size_t)index->n_total * index->dims, fp);
    }

    // 4. 写入量化编码（如果有）
    if (index->quantizer != NULL) {
        fwrite(index->codes, sizeof(uint8_t),
               (size_t)index->n_total * index->pq_code_size, fp);
    }

    fclose(fp);
    return 0;
}
```

## 4. Phase 2 改造要点

### 4.1 存储抽象化

**Before**: 直接使用 `float *vectors` 存储
```c
index->vectors = (float *)malloc(n_total * dims * sizeof(float));
```

**After**: 通过 Heap 存储，使用向量引用
```c
vector_ref_t ref = heap_vector_insert(index->heap_store, vector);
index->vector_refs[id] = ref;
```

### 4.2 两阶段搜索

**Before**: 单阶段搜索，直接访问 vectors 数组
```c
float dist = distance(query, &index->vectors[id * dims], dims);
```

**After**: 两阶段搜索，Heap 获取向量 + 重排序
```c
// 阶段 1: 图搜索获取候选 ID
int32_t candidates[search_list_size];
diskann_graph_search(index, query, candidates);

// 阶段 2: 从 Heap 获取向量
for (int32_t i = 0; i < n_candidates; i++) {
    heap_vector_get(index->heap_store, &index->vector_refs[candidates[i]], vector);
    distances[i] = distance(query, vector, dims);
}

// 阶段 3: 重排序
partial_sort_top_k(candidates, distances, k);
```

### 4.3 兼容模式

保持向后兼容：当 `heap_store == NULL` 时，回退到原始模式。

```c
if (index->heap_store != NULL) {
    // Heap 模式
    vector_ref_t ref = heap_vector_insert(index->heap_store, vector);
    index->vector_refs[id] = ref;
} else {
    // 兼容模式
    memcpy(&index->vectors[id * dims], vector, dims * sizeof(float));
}
```

## 5. 测试用例

### 单元测试

```cpp
TEST(DiskANNTest, CreateAndSearch)
{
    // 1. 创建配置
    diskann_config_t *config = diskann_config_default(128, DISTANCE_L2);
    config->index_size = 32;
    config->build_search_list_size = 100;

    // 2. 创建索引
    diskann_t *index = diskann_index_create_with_config(config);

    // 3. 插入向量
    float vectors[1000][128];
    for (int i = 0; i < 1000; i++) {
        fill_random(vectors[i], 128);
    }
    diskann_index_add(index, 1000, (float *)vectors);

    // 4. 构建图
    diskann_index_build(index);

    // 5. 搜索
    float query[128];
    fill_random(query, 128);

    int32_t k = 10;
    float distances[10];
    int32_t labels[10];
    diskann_index_search(index, query, k, 50, 10, distances, labels);

    EXPECT_EQ(k, 10);
    EXPECT_GT(labels[0], -1);  // 至少有一个结果

    diskann_index_drop(index);
    diskann_config_free(config);
}
```

### Heap 模式测试

```cpp
TEST(DiskANNTest, HeapStoreMode)
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

    // 创建 DiskANN 索引并绑定 Heap
    diskann_t *index = diskann_index_create(128, 32, 100, DISTANCE_L2);
    // ... 绑定 heap_store 到 index ...

    // 插入、构建、搜索
    // ...

    // 验证向量存储在 Heap 中
    EXPECT_GT(heap_vector_count(heap_store), 0);

    heap_vector_store_destroy(heap_store);
    storage_backend_destroy(backend);
    diskann_index_drop(index);
}
```

## 6. 参考资料

- Jayaram Subramanya et al. (2019). "DiskANN: Fast Accurate Billion-point Nearest Neighbor Search on a Single Node"
- Suhas Jayaram et al. (2021). "FreshDiskANN: A Fast and Accurate Graph-Based ANN Index for Streaming Data"
- Ravishankar et al. (2022). "SPANN: Highly-efficient Billion-scale Approximate Nearest Neighbor Search"
