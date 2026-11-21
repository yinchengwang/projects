> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# 多模态数据库索引文档

本文档包含索引子系统的理论原理、实现详解、审查报告和快速开始指南。当前文档体系已完成 Phase 1-3 全部内容。

## 目录结构

```
docs/arch/
├── README.md                   # 本文档（总入口）
├── ROADMAP.md                  # 后续扩展路线图（Phase 1-4 任务清单）
├── theory/                     # 索引原理（理论为主）
│   ├── hnsw-theory.md
│   ├── diskann-theory.md
│   ├── ivf-theory.md
│   ├── lsh-theory.md
│   └── opq-theory.md
├── implementation/             # 索引实现（代码为主）
│   ├── hnsw-impl.md
│   ├── diskann-impl.md
│   ├── ivf-impl.md
│   └── lsh-impl.md
└── review/                     # 代码审查报告
    ├── index-implementation-review.md
    └── storage-subsystem-review.md

engine/test/db/index/
├── unit/                       # 单元测试
│   ├── vector_index/           # 索引核心单元测试
│   ├── heap/                   # Heap Store 单元测试
│   └── storage_backend/        # 存储后端单元测试
└── integration/                # 集成与基准测试
    ├── integration_test.cpp
    ├── hnsw_benchmark.cpp
    ├── diskann_benchmark.cpp
    ├── ivf_benchmark.cpp
    ├── lsh_benchmark.cpp
    ├── multi_index_joint_test.cpp
    ├── persistence_test.cpp
    └── benchmark_config.h
```

---

## 索引分类

### 向量索引（ANN）

| 索引 | 说明 | 理论文档 | 实现文档 | 测试位置 | 状态 |
|------|------|----------|----------|----------|------|
| HNSW | 层次可导航小世界图 | [theory/hnsw-theory.md](theory/hnsw-theory.md) | [implementation/hnsw-impl.md](implementation/hnsw-impl.md) | `test/vector_index/hnsw/` + `integration/hnsw_benchmark.cpp` | ✅ |
| DiskANN | 磁盘友好型 ANN（Vamana 图） | [theory/diskann-theory.md](theory/diskann-theory.md) | [implementation/diskann-impl.md](implementation/diskann-impl.md) | `test/vector_index/diskann/` + `integration/diskann_benchmark.cpp` | ✅ |
| IVF | 倒排文件索引（K-Means 划分） | [theory/ivf-theory.md](theory/ivf-theory.md) | [implementation/ivf-impl.md](implementation/ivf-impl.md) | `test/vector_index/ivf/` + `integration/ivf_benchmark.cpp` | ✅ |
| LSH | 局部敏感哈希 | [theory/lsh-theory.md](theory/lsh-theory.md) | [implementation/lsh-impl.md](implementation/lsh-impl.md) | `test/vector_index/lsh/` + `integration/lsh_benchmark.cpp` | ✅ |
| OPQ | 有序乘积量化（量化器） | [theory/opq-theory.md](theory/opq-theory.md) | 待补全（Phase 3.1） | `test/vector_index/opq/` | ✅ 实现 / 📝 文档待补 |
| BM25 | 文本排序 | — | — | `test/vector_index/BM25/` | ✅ |

### 树索引

| 索引 | 说明 | 头文件 | 测试 |
|------|------|--------|------|
| BTree | B 树 | `engine/include/db/index/btree/btree.h` | `test/db/index/` |
| B+Tree | B+ 树 | `engine/include/db/index/bplus_tree/bptree.h` | `test/db/index/` |
| RTree | 空间索引 | `engine/include/db/index/rtree/rtree.h` | `test/db/index/` |
| ART | 自适应基数树 | `engine/include/db/index/art/art.h` | `test/db/index/` |
| Radix Tree | 基数树 | `engine/include/db/index/radix_tree/radix_tree.h` | `test/db/index/` |
| Skip List | 跳表 | `engine/include/db/index/skip_list/skip_list.h` | `test/db/index/` |
| T-Tree | T 树 | `engine/include/db/index/ttree/ttree.h` | `test/db/index/` |

### 哈希索引

| 索引 | 说明 | 头文件 | 测试 |
|------|------|--------|------|
| CCEH | 可扩展哈希（CCEH） | `engine/include/db/index/hash/cceh.h` | `test/db/index/` |
| PG Linear Hash | PG 风格线性哈希 | `engine/include/db/index/hash/pg_hash.h` | `test/db/index/` |
| Bloom | 布隆过滤器 | `engine/include/db/index/hash/bloom.h` | `test/db/index/` |
| Cuckoo | Cuckoo 过滤器 | `engine/include/db/index/hash/cuckoo.h` | `test/db/index/` |
| Xor Filter | Xor 过滤器 | `engine/include/db/index/hash/xor_filter.h` | `test/db/index/` |

### 全文索引

| 索引 | 说明 | 头文件 |
|------|------|--------|
| Fulltext | 通用全文索引 | `engine/include/db/index/fulltext/fulltext.h` |
| GIN | 通用倒排索引 | `engine/include/db/index/gin/gin.h` |
| GIST | 通用搜索树 | `engine/include/db/index/gist/gist.h` |
| BRIN | 块范围索引 | `engine/include/db/index/brin/brin.h` |
| Bitmap | 位图索引 | `engine/include/db/index/bitmap/bitmap_index.h` |

---

## 文档导航

### 理论文档（theory/）

| 文档 | 内容 |
|------|------|
| [theory/hnsw-theory.md](theory/hnsw-theory.md) | HNSW 多层图结构、概率跃迁、搜索路径分析 |
| [theory/diskann-theory.md](theory/diskann-theory.md) | Vamana 图、Robust Prune、PQ/SQ/LVQ/RQ 压缩、FreshDiskANN |
| [theory/ivf-theory.md](theory/ivf-theory.md) | K-Means 划分、倒排列表结构、nlist/nprobe 调参 |
| [theory/lsh-theory.md](theory/lsh-theory.md) | Bitwise/p-stable/SimHash 三类哈希族、多表冗余 |
| [theory/opq-theory.md](theory/opq-theory.md) | OPQ 旋转优化、ADC 距离计算、与 IVF/HNSW 的结合 |

### 实现文档（implementation/）

| 文档 | 内容 |
|------|------|
| [implementation/hnsw-impl.md](implementation/hnsw-impl.md) | HNSW 模块结构、核心数据结构、Heap Store 集成 |
| [implementation/diskann-impl.md](implementation/diskann-impl.md) | DiskANN 模块结构、Vamana 构建、SPANN/FreshDiskANN/Merge-Vamana 扩展 |
| [implementation/ivf-impl.md](implementation/ivf-impl.md) | IVF 模块结构、训练流程、DirectMap 与 Heap Store 协同 |
| [implementation/lsh-impl.md](implementation/lsh-impl.md) | LSH 单文件实现、哈希表结构、三类哈希族 API |

### 审查报告（review/）

| 报告 | 范围 | 关键结论 |
|------|------|----------|
| [review/index-implementation-review.md](review/index-implementation-review.md) | HNSW/DiskANN/IVF/LSH 四个向量索引 | HNSW B 级（2 致命/4 严重），DiskANN C+（8 致命/11 严重），IVF C（6 致命/7 严重），LSH C-（4 致命/7 严重） |
| [review/storage-subsystem-review.md](review/storage-subsystem-review.md) | heap_vector_store、storage_backend、vector_ref | API 设计 B+，扩展性 A-，mmap 后端存在 P0 数据安全风险 |

### 其他架构文档

| 文档 | 内容 |
|------|------|
| [docs/storage-architecture.md](../storage-architecture.md) | PostgreSQL 风格存储架构（Catalog/BufferPool/HeapAM/BTreeAM/WAL） |
| [openspec/specs/2026-07-14-multimodal-index-redesign.md](../openspec/specs/2026-07-14-multimodal-index-redesign.md) | 多模态索引重构设计规格 |

---

## 索引对比矩阵

> **数据规模**：随机均匀分布，10k 向量 / 128 维 / k=10
> **数据来源**：`integration_test/` 基准测试产物（见 [docs/arch/review/index-implementation-review.md](review/index-implementation-review.md)）
> **数据时间**：2026-07-15 基准测试结果

### 核心指标对比

| 索引 | 召回率（@10） | 构建时间 | 查询延迟（平均） | 内存占用（10k 向量） | 适用规模 |
|------|---------------|----------|------------------|---------------------|----------|
| **HNSW** | ~0.45 | 数秒 | < 1ms | ~80 MB | 百万级（全内存） |
| **DiskANN** | ~0.40 | 数十秒 | < 2ms | ~5 MB（仅 PQ 码本） | 十亿级（SSD 辅助） |
| **IVF** | ~0.40（nprobe=20） | 数十秒（K-Means 训练） | < 0.5ms | ~25 MB | 百万级以上 |
| **LSH** | ~0.15–0.20 | < 1秒 | < 0.5ms | ~40 MB（多表） | 亿级（粗筛） |
| **OPQ** | — （压缩比 32x） | 训练耗时长 | ADC < 0.1ms | 原始向量 × 1/32 | 量化压缩（辅助） |

> **召回率说明**：随机均匀分布上随机向量精确近邻本身就不稳定，召回率参考审查报告中"实际可达值"。对真实 SIFT/GloVe 等聚簇数据，HNSW/IVF 召回率通常可达 0.90+。

### 构建/查询特性对比

| 特性 | HNSW | DiskANN | IVF | LSH | OPQ |
|------|------|---------|-----|-----|-----|
| 增量插入 | ✅ 支持 | ✅ FreshDiskANN | ⚠️ 需增量训练 | ✅ 支持 | ❌ 需重训码本 |
| 删除支持 | ⚠️ 墓碑标记 | ✅ FreshDiskANN | ⚠️ DirectMap 重建 | ✅ 支持 | N/A |
| 持久化 | ⚠️ 需扩展 | ✅ 原生支持 | ⚠️ 需扩展 | ⚠️ 需扩展 | ✅ 码本可序列化 |
| 并发搜索 | ⚠️ 读安全 | ✅ 支持 | ✅ 读安全 | ✅ 读安全 | ✅ 读安全 |
| 距离度量 | L2 / IP | L2 | L2 | L2 / Cosine / Jaccard | L2（ADC） |
| 参数调优难度 | 中（M/ef） | 高（R/L/alpha） | 中（nlist/nprobe） | 高（L/k/w） | 中（m/nbits） |

### 适用场景推荐

| 场景 | 推荐索引 | 理由 |
|------|----------|------|
| **中等规模 + 高召回** | HNSW | 召回率稳定、查询快、参数相对友好 |
| **超大规模 + 内存受限** | DiskANN | 十亿级向量、内存仅需 PQ 码本 |
| **粗筛 + 召回可控** | IVF | nprobe 直接调节召回/延迟权衡 |
| **超快粗筛（低精度可接受）** | LSH | O(1) 查询、构建快 |
| **向量压缩 + 距离加速** | OPQ | 与 IVF/HNSW 结合使用（子模块） |
| **磁盘优先 + 增量更新** | DiskANN + FreshDiskANN | 唯一原生支持磁盘 + 增量插入 |

### 索引与存储后端适配

| 索引 | memory 后端 | page_file 后端 | mmap 后端 |
|------|-------------|----------------|-----------|
| HNSW | ✅ | ⚠️ 待验证 | ⚠️ 待验证 |
| DiskANN | ✅ | ✅ | ✅（设计目标） |
| IVF | ✅ | ⚠️ 待验证 | ⚠️ 待验证 |
| LSH | ✅ | ⚠️ 待验证 | ⚠️ 待验证 |
| OPQ（量化器） | ✅ | ✅ | ✅ |

> **审查中发现的风险**：见 [review/index-implementation-review.md](review/index-implementation-review.md) 七、P0 修复清单。当前生产推荐 **memory 后端 + HNSW** 或 **mmap 后端 + DiskANN** 组合。

---

## 快速开始指南

### 1. 环境准备

```bash
# 克隆并进入项目目录
cd book

# 配置工程轨道构建
cmake -S engineering -B build/engineering -DCMAKE_BUILD_TYPE=Release

# 编译所有目标（索引库 + 单元测试 + 集成测试）
cmake --build build/engineering -j$(nproc)
```

> **可选编译**：仅构建 vector_index 子集
> ```bash
> cmake --build build/engineering --target vector_index -j$(nproc)
> cmake --build build/engineering --target hnsw diskann ivf lsh opq -j$(nproc)
> ```

### 2. 运行基准测试

基准测试位于 `engine/test/db/index/integration/`，统一接入 `integration_test` 可执行文件。

#### 2.1 运行全部集成测试

```bash
ctest --test-dir build/engineering -R integration_test --output-on-failure
```

#### 2.2 单独运行 HNSW 基准

```bash
ctest --test-dir build/engineering -R "HNSWBenchmark" --output-on-failure
# 或直接运行二进制
./build/engineering/integration_test --gtest_filter="*HNSW*"
```

#### 2.3 单独运行 DiskANN/IVF/LSH 基准

```bash
./build/engineering/integration_test --gtest_filter="*DiskANN*"
./build/engineering/integration_test --gtest_filter="*IVF*"
./build/engineering/integration_test --gtest_filter="*LSH*"
```

#### 2.4 调整基准参数

编辑 `engine/test/db/index/integration/benchmark_config.h`：

```c
#define BENCHMARK_DIMS 128        /* 向量维度 */
#define BENCHMARK_N_VECTORS 10000  /* 索引向量数 */
#define BENCHMARK_N_QUERIES 100    /* 查询数量 */
#define BENCHMARK_K 10             /* top-k */

#define HNSW_RECALL_THRESHOLD 0.45f      /* HNSW 召回率下限 */
#define DISKANN_RECALL_THRESHOLD 0.40f   /* DiskANN 召回率下限 */
#define IVF_RECALL_THRESHOLD 0.40f       /* IVF 召回率下限 */
#define LSH_RECALL_THRESHOLD 0.15f       /* LSH 召回率下限 */

#define MAX_QUERY_LATENCY_MS 2.0          /* 查询延迟上限 */
```

> 阈值含义详见各 task 报告（task-1.2 ~ task-1.5）。

#### 2.5 多索引联合测试

```bash
./build/engineering/integration_test --gtest_filter="*MultiIndex*"
# 对比 HNSW/IVF/LSH 在同一数据集上的 top-k 一致性
```

#### 2.6 持久化端到端测试

```bash
./build/engineering/integration_test --gtest_filter="*Persistence*"
# 验证：插入 → 持久化 → 重新加载 → 搜索结果一致
```

### 3. 编写新索引测试

#### 3.1 单元测试（C++，位于 `engine/test/db/index/vector_index/<新索引>/`）

**模板文件**：`engine/test/db/index/vector_index/lsh/lsh_test.cpp`

```cpp
// 新索引单元测试模板
#include <gtest/gtest.h>
#include <cstdlib>
#include <vector>

extern "C" {
#include "db/index/vector_index/<新索引>/<新索引>.h"
}

TEST(NewIndexTest, CreateDestroy) {
    new_index_t *idx = new_index_create(128 /* dims */, /* 参数 */);
    ASSERT_NE(idx, nullptr);
    new_index_destroy(idx);
}

TEST(NewIndexTest, InsertSearch) {
    new_index_t *idx = new_index_create(128, /* 参数 */);
    float vec[128];
    for (int i = 0; i < 128; i++) vec[i] = (float)rand() / RAND_MAX;

    int32_t id = new_index_insert(idx, vec);
    EXPECT_EQ(id, 0);

    int32_t result_ids[10];
    float   result_dists[10];
    int n = new_index_search(idx, vec, 10, result_ids, result_dists);
    EXPECT_EQ(n, 1);

    new_index_destroy(idx);
}
```

#### 3.2 集成基准测试（位于 `engine/test/db/index/integration/`）

**模板文件**：`integration_test.cpp` + `<索引>_benchmark.cpp`

三步接入流程：

1. **添加 TEST_F**：在 `<索引>_benchmark.cpp` 内编写 `TEST_F(NewIndexBenchmark, RecallDegradation)`、`TEST_F(NewIndexBenchmark, LatencyUnder2ms)` 等用例。
2. **更新 CMakeLists.txt**：在 `engine/test/db/index/integration/CMakeLists.txt` 中将新源文件加入 `add_project_test(integration_test ...)` 的通配范围。
3. **更新阈值配置**：在 `benchmark_config.h` 添加 `NEW_INDEX_RECALL_THRESHOLD` 常量。

#### 3.3 持久化测试

参考 `engine/test/db/index/integration/persistence_test.cpp`，关键流程：

```cpp
TEST(NewIndexPersistenceTest, WriteLoadSearch) {
    // 1. 创建索引 + 后端（mmap/page_file）
    storage_backend_t *backend = storage_backend_create(STORAGE_BACKEND_MMAP, "test.idx", 8192);
    new_index_t *idx = new_index_create_with_backend(128, backend);

    // 2. 插入 + 持久化
    new_index_insert(idx, vec);
    new_index_save(idx, "test.idx");

    // 3. 销毁
    new_index_destroy(idx);
    storage_backend_destroy(backend);

    // 4. 重新加载 + 验证一致性
    new_index_t *idx2 = new_index_load("test.idx");
    /* ... 验证搜索结果一致 ... */
    new_index_destroy(idx2);
}
```

#### 3.4 测试断言风格

- 单元测试使用 gtest `EXPECT_*` / `ASSERT_*`
- 基准测试使用 `EXPECT_GE(recall, THRESHOLD)` / `EXPECT_LT(latency_ms, LIMIT)`
- 浮点比较：`EXPECT_NEAR(actual, expected, 1e-5)`（距离）、`EXPECT_FLOAT_EQ`（比率）

### 4. 调试与性能分析

#### 4.1 启用调试日志

```c
// 在测试 main 中启用索引内部日志
ivf_log_set_level(IVF_LOG_DEBUG);
```

#### 4.2 性能热点定位

使用 perf（Linux）/ Instruments（macOS）/ Visual Studio Profiler（Windows）：

```bash
# Linux 示例
perf record -g ./build/engineering/integration_test --gtest_filter="*HNSW*"
perf report --sort=pcalls
```

#### 4.3 内存泄漏检测

```bash
# Linux
valgrind --leak-check=full ./build/engineering/integration_test

# Windows（推荐集成到 CI）
# 使用 Application Verifier + UMDH / Debug CRT
```

### 5. 常见问题

| 问题 | 排查路径 |
|------|----------|
| 召回率不达标 | 检查维度一致性 → 距离度量选择 → ef_search / nprobe 调参 → 数据归一化 |
| 查询延迟超标 | 减少 ef_search / nprobe → 切换为 IVFPQ / HNSWPQ 混合索引 |
| 编译链接失败 | 检查 `engine/src/db/index/vector_index/CMakeLists.txt` 是否纳入新文件 |
| 持久化加载崩溃 | 确认 `page_size` 与后端一致（详见 review/storage-subsystem-review.md A1） |

---

## 相关文档索引

| 类别 | 路径 |
|------|------|
| 总体架构 | [docs/storage-architecture.md](../storage-architecture.md) |
| 路线图 | [docs/arch/ROADMAP.md](ROADMAP.md) |
| 多模态索引设计 | [openspec/specs/2026-07-14-multimodal-index-redesign.md](../openspec/specs/2026-07-14-multimodal-index-redesign.md) |
| 索引实现审查 | [docs/arch/review/index-implementation-review.md](review/index-implementation-review.md) |
| 存储子系统审查 | [docs/arch/review/storage-subsystem-review.md](review/storage-subsystem-review.md) |
| 测试用例 | `engine/test/db/index/` |

---

*文档最后更新：2026-07-15*
