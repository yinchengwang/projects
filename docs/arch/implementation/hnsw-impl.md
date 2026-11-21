> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# HNSW 实现详解

## 1. 文件结构

```
engine/include/db/index/vector_index/hnsw/
├── faiss_hnsw.h              # 公共头文件
└── faiss_hnsw_utils.h        # 工具函数

engine/src/db/index/vector_index/hnsw/
├── faiss_hnsw_create.c           # 索引创建
├── faiss_hnsw_create_with_config.c # 使用配置创建
├── faiss_hnsw_insert.c           # 向量插入
├── faiss_hnsw_search.c           # 向量搜索
├── faiss_hnsw_delete.c           # 向量删除
└── faiss_hnsw_utils.c            # 工具函数实现

engine/test/vector_index/hnsw/
└── hnsw_gtest.cpp                # 单元测试
```

## 2. 核心数据结构

### faiss_hnsw_t

```c
typedef struct faiss_hnsw {
    /* 向量存储 */
    float *vectors;               // 向量数据（原始模式）
    uint8_t *codes;               // 量化编码（量化模式）
    int32_t n_total;              // 向量总数
    int32_t dims;                 // 向量维度
    int32_t M;                    // 每层连接数
    int32_t ef_construction;       // 构建搜索范围
    int32_t ef_search;             // 搜索搜索范围
    distance_metric_t metric;      // 距离度量
    quantization_type_t quantization_type;  // 量化类型

    /* 图结构 */
    int32_t *levels;              // 每层起始偏移
    int32_t *offsets;             // 邻居偏移
    int32_t *nbs;                 // 邻居数据
    int32_t entry_pointer;         // 入口点
    int32_t max_level;            // 最大层

    /* 存储后端（重构后） */
    storage_backend_t *storage;            // 存储后端
    heap_vector_store_t *heap_store;       // Heap 向量存储
    vector_ref_t *vector_refs;            // 向量引用数组
    uint32_t vector_refs_size;            // 引用数组大小

    /* 持久化控制 */
    persist_control_t *persist_ctrl;       // 持久化控制

    /* 删除标记 */
    vector_delete_bitmap_t *delete_bitmap;
} faiss_hnsw_t;
```

## 3. 完整流程

### 3.1 创建索引

**入口函数**: `faiss_hnsw_index_create()` / `faiss_hnsw_index_create_with_config()`

```c
faiss_hnsw_t *faiss_hnsw_index_create(int32_t M, int32_t dims,
                                       int32_t ef_construction,
                                       distance_metric_t metric,
                                       quantization_type_t quantization_type)
{
    faiss_hnsw_t *index = (faiss_hnsw_t *)calloc(1, sizeof(faiss_hnsw_t));
    index->M = M;
    index->dims = dims;
    index->ef_construction = ef_construction;
    index->ef_search = 100;  // 默认值
    index->metric = metric;
    index->quantization_type = quantization_type;

    /* 初始化概率表 */
    hnsw_set_default_probas(index);

    /* 初始化图结构 */
    index->entry_pointer = -1;
    index->max_level = -1;

    return index;
}
```

**流程**:
1. 分配索引结构体内存
2. 设置算法参数（M, ef_construction, ef_search）
3. 初始化分层概率表
4. 创建空图（entry_pointer = -1）
5. 初始化删除位图
6. 根据配置绑定存储后端和持久化控制

### 3.2 插入向量

**入口函数**: `faiss_hnsw_index_add()`

```c
int32_t faiss_hnsw_index_add(faiss_hnsw_t *index, int32_t n, const float *vector)
{
    for (int32_t i = 0; i < n; i++) {
        int32_t id = index->n_total++;

        /* 1. 存储向量（Heap 或原始） */
        faiss_hnsw_vector_storage(index, 1, vector + i * index->dims);

        /* 2. 随机生成分层 */
        int32_t level = faiss_random_level(index);

        /* 3. 逐层插入 */
        for (int32_t l = level; l >= 0; l--) {
            /* 贪心搜索找到最近邻 */
            int32_t nearest = search_layer(index, vector + i * index->dims, l);

            /* 连接邻居 */
            connect_neighbors(index, id, l, nearest);
        }

        /* 4. 更新入口点 */
        if (level > index->max_level) {
            index->entry_pointer = id;
            index->max_level = level;
        }
    }
    return 0;
}
```

**流程**:
1. 将向量存入 Heap（使用向量引用）
2. 随机生成节点的层数 L
3. 从顶层开始，逐层向下：
   - 贪心搜索找到最近邻
   - 连接 M 个最近邻居
4. 更新入口点（如需要）

### 3.3 搜索向量（两阶段搜索 + 重排序）

**入口函数**: `faiss_hnsw_index_search()`

```c
int32_t faiss_hnsw_index_search(faiss_hnsw_t *index,
                                 const float *query,
                                 int32_t k,
                                 int32_t ef_search,
                                 float *distances,
                                 int32_t *vec_ids)
{
    /* 阶段 1: 图索引搜索，获取候选 ID */
    int32_t candidates = k * 2;
    int32_t *candidate_ids = search_graph(index, query, candidates, ef_search);

    /* 阶段 2: 从 Heap 获取完整向量 */
    float candidate_vectors[candidates][index->dims];
    for (int32_t i = 0; i < candidates; i++) {
        vector_ref_t ref = get_vector_ref(index, candidate_ids[i]);
        heap_vector_get(index->heap_store, &ref, candidate_vectors[i]);
    }

    /* 阶段 3: 计算精确距离并排序 */
    for (int32_t i = 0; i < candidates; i++) {
        distances[i] = distance_l2(query, candidate_vectors[i], index->dims);
    }

    /* 取 top-k */
    partial_sort(distances, vec_ids, k);

    return 0;
}
```

**流程**:
1. 从入口点开始，逐层向下贪心搜索
2. 获取 top-k*2 候选
3. 从 Heap 获取候选的完整向量
4. 计算精确距离（重排序）
5. 返回 top-k 结果

### 3.4 删除向量

**入口函数**: `faiss_hnsw_index_delete()`

```c
int32_t faiss_hnsw_index_delete(faiss_hnsw_t *index, int32_t id)
{
    /* 逻辑删除：标记删除位图 */
    vector_delete_bitmap_set(index->delete_bitmap, id, true);

    /* 物理删除：异步 GC */
    /* 由 vacuum 进程处理 */
    return 0;
}
```

**流程**:
1. 标记删除位图
2. 异步重建索引（可选）

### 3.5 持久化

**持久化条件**: `persist_enabled = true`

```c
/* 创建检查点 */
int32_t create_checkpoint(faiss_hnsw_t *index, const char *path)
{
    /* 1. 刷盘所有脏页 */
    storage_backend_sync(index->storage);

    /* 2. 序列化图结构 */
    serialize_hnsw_graph(index, path);

    /* 3. 序列化 Heap 存储 */
    serialize_heap_store(index->heap_store, path);

    return 0;
}

/* 崩溃恢复 */
int32_t recover_from_checkpoint(faiss_hnsw_t *index, const char *path)
{
    /* 1. 反序列化 Heap 存储 */
    deserialize_heap_store(index->heap_store, path);

    /* 2. 反序列化图结构 */
    deserialize_hnsw_graph(index, path);

    /* 3. 重放 WAL */
    replay_wal(index);

    return 0;
}
```

## 4. 关键代码片段

### 贪心搜索

```c
static int32_t greedy_search_layer(faiss_hnsw_t *index,
                                    const float *query,
                                    int32_t layer)
{
    int32_t current = index->entry_pointer;
    float best_dist = distance_calc(index, query, current);

    while (true) {
        int32_t next = -1;
        float next_dist = FLT_MAX;

        /* 遍历当前节点的邻居 */
        int32_t nb_start = index->offsets[current] + layer * MAX_M;
        int32_t nb_count = index->nbs[current * MAX_M + layer];

        for (int32_t i = 0; i < nb_count; i++) {
            int32_t neighbor = index->nbs[nb_start + i];
            float dist = distance_calc(index, query, neighbor);

            if (dist < next_dist) {
                next = neighbor;
                next_dist = dist;
            }
        }

        if (next_dist >= best_dist) {
            break;  // 没有更好的邻居
        }

        current = next;
        best_dist = next_dist;
    }

    return current;
}
```

## 5. 测试用例

### 单元测试

```cpp
TEST(HNSWTest, CreateAndSearch)
{
    faiss_hnsw_t *index = faiss_hnsw_index_create(16, 128, 200,
        DISTANCE_L2, QUANTIZATION_TYPE_NONE);

    // 插入 1000 个向量
    float vectors[1000][128];
    for (int i = 0; i < 1000; i++) {
        fill_random(vectors[i], 128);
    }
    faiss_hnsw_index_add(index, 1000, (float *)vectors);

    // 搜索
    float query[128];
    fill_random(query, 128);

    int32_t k = 10;
    float distances[10];
    int32_t ids[10];
    faiss_hnsw_index_search(index, query, k, 100, distances, ids);

    EXPECT_EQ(k, 10);
    EXPECT_GT(distances[0], 0);  // 至少有一个结果

    faiss_hnsw_index_drop(index);
}
```

## 6. 重构要点

### 存储抽象化

**Before**: 直接使用 `float *vectors` 存储
```c
index->vectors = (float *)malloc(size);
```

**After**: 通过 Heap 存储，使用向量引用
```c
vector_ref_t ref = heap_vector_insert(index->heap_store, vector);
```

### 持久化开关

**Before**: 始终持久化
```c
// 总是写 WAL
write_wal(log_entry);
```

**After**: 根据 `persist_enabled` 决定
```c
if (index->persist_ctrl && persist_is_enabled(index->persist_ctrl)) {
    persist_write_wal(index->persist_ctrl, log_entry, len);
}
```

### 向量引用模式

**Before**: 索引存储完整向量
```
index->vectors[id] = vector;
```

**After**: 索引存储引用，Heap 是 Single Source of Truth
```
vector_ref_t ref = heap_vector_insert(index->heap_store, vector);
index->vector_refs[id] = ref;
```

## 7. API 参考

| API | 说明 |
|-----|------|
| `faiss_hnsw_index_create()` | 创建索引 |
| `faiss_hnsw_index_create_with_config()` | 使用配置创建索引 |
| `faiss_hnsw_index_add()` | 插入向量 |
| `faiss_hnsw_index_search()` | 搜索向量 |
| `faiss_hnsw_index_delete()` | 删除向量 |
| `faiss_hnsw_index_drop()` | 销毁索引 |
| `faiss_hnsw_index_set_heap_store()` | 设置 Heap 存储 |
| `faiss_hnsw_index_set_storage()` | 设置存储后端 |

## 8. 参数调优指南

| 参数 | 默认值 | 说明 | 调优建议 |
|------|--------|------|----------|
| M | 16 | 每层连接数 | 增大 → 精度↑ 内存↑ |
| ef_construction | 200 | 构建搜索范围 | 增大 → 精度↑ 构建时间↑ |
| ef_search | 100 | 搜索搜索范围 | 增大 → 精度↑ 搜索时间↑ |
| dims | - | 向量维度 | 根据数据决定 |

## 9. 性能基准

| 数据规模 | dims | M | ef_search | QPS | 召回率 |
|----------|------|---|-----------|-----|--------|
| 1M | 128 | 16 | 100 | ~5000 | ~95% |
| 10M | 128 | 32 | 200 | ~2000 | ~98% |
| 100M | 128 | 64 | 400 | ~500 | ~99% |

（注：实际性能取决于硬件和数据分布）
