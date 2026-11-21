> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# 向量引擎 IVF-PQ 集成

## Purpose

将 IVF-PQ 索引集成到向量引擎，实现根据数据规模自动选择最优索引类型。

## Background

此规格源自变更 `2026-07-07-mm-storage-phase1-optimize`，用于扩展向量索引类型，支持 IVF-PQ 压缩索引。

## Requirements

### Requirement: 支持的索引类型

```c
typedef enum {
    VECTOR_INDEX_HNSW = 0,     /**< HNSW 索引（高精度，高内存） */
    VECTOR_INDEX_IVF_PQ = 1,   /**< IVF-PQ 索引（压缩，低内存） */
    VECTOR_INDEX_IVF_HNSW = 2, /**< IVF-HNSW 混合索引（平衡） */
    VECTOR_INDEX_DISKANN = 3,  /**< DiskANN 索引（超大规模） */
} vector_index_type_t;
```

### Requirement: 索引选择决策

根据数据规模自动选择最优索引：

| 数据规模 | 推荐索引 | 说明 |
|----------|----------|------|
| n < 10K | HNSW | 简单场景，无需压缩 |
| n: 10K-1M | IVF-PQ | 平衡内存和精度 |
| n: 1M-10M | IVF-HNSW | 混合优化 |
| n > 10M | DiskANN | 超大规模 |

**特殊情况**：
- `recall_target > 0.95`: 强制 HNSW
- `memory_constrained`: 优先压缩率高的索引

### Requirement: 索引选择器 API

```c
vector_index_type_t vector_index_selector_choose(
    size_t n_vectors,
    int dims,
    float recall_target,
    bool memory_constrained
);
```

### Requirement: IVF-PQ API

| 函数 | 说明 |
|------|------|
| `vector_engine_enable_ivf_pq()` | 启用 IVF-PQ 索引 |
| `vector_engine_train_ivf_pq()` | 训练量化器 |
| `vector_engine_search_ivf_pq()` | IVF-PQ 搜索 |
| `vector_engine_save_ivf_pq()` | 保存索引 |
| `vector_engine_load_ivf_pq()` | 加载索引 |

### Requirement: IVF-PQ 参数建议

| 参数 | 取值范围 | 推荐值 |
|------|----------|--------|
| nlist | 32-65536 | n_vectors / 100 |
| m | 4-64 | min(16, dims/4) |
| nbits | 4-12 | 8 |
| nprobe | 1-nlist | sqrt(nlist) |

### Requirement: 性能目标

| 指标 | 目标 | 说明 |
|------|------|------|
| IVF-PQ 构建速度 | > 100K vectors/s | 含训练 |
| IVF-PQ 搜索延迟 | < 10ms (1M vectors) | 单次查询 |
| 压缩比 | > 10x | 对比原始 float |
| 召回率 | > 0.8 | 标准 benchmark |

### Requirement: 持久化格式

IVF-PQ 索引文件格式：
- Header: magic, version, dims, nlist, m, nbits, n_vectors
- PQ Codebook: nlist * m * (2^nbits) * sizeof(float)
- Inverted Lists: list_offsets, list_ids, list_codes

## Spec References

- IVF-PQ 实现: `include/db/index/vector_index/ivf_pq/`
- 选择器实现: `src/db/index/vector_index/vector_index_selector.c`
- 向量引擎集成: `src/db/storage/vector/vector_engine.c`
- 测试: `test/db/storage/test_vector_index.cpp`
