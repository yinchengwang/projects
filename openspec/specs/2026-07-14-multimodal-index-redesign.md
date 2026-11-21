> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# 多模态数据库索引子系统重构设计

**版本**: v1.0
**日期**: 2026-07-14
**状态**: 设计中

---

## 1. 概述

本文档定义了多模态数据库索引子系统的重构设计方案，涵盖四个核心架构决策：

1. **分层架构**：算法层与存储层分离
2. **持久化开关**：索引级持久化配置
3. **统一向量存储**：消除向量膨胀
4. **文档体系**：索引原理与实现文档

---

## 2. 分层架构设计

### 2.1 设计目标

将索引实现从 Faiss 的内存模型迁移到页面管理的持久化模型，同时保持算法的纯净性和可测试性。

### 2.2 架构分层

```
┌─────────────────────────────────────────────────────────────┐
│                    索引配置层 (Index Config)                 │
│  - 存储后端选择 (storage_backend)                            │
│  - 持久化开关 (persist_enabled)                              │
│  - 其他参数 (M, ef_construction, ef_search, ...)            │
└─────────────────────┬───────────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────────┐
│                  算法层 (Algorithm Layer)                    │
│  - 纯算法逻辑，不直接操作存储                                │
│  - 通过回调函数访问数据                                      │
│  - 示例: faiss_hnsw_insert() → 回调 → storage_put()         │
└─────────────────────┬───────────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────────┐
│                  存储层 (Storage Layer)                      │
│  - 页面管理器 (Buffer Pool)                                  │
│  - 持久化接口 (Storage Backend)                              │
│  - WAL/Redo 支持 (当 persist_enabled=true)                  │
└─────────────────────────────────────────────────────────────┘
```

### 2.3 存储后端类型

| 后端类型 | 标识符 | 说明 |
|----------|--------|------|
| **纯内存** | `STORAGE_BACKEND_MEMORY` | 不持久化，快速测试 |
| **页面文件** | `STORAGE_BACKEND_PAGE_FILE` | 基于 Buffer Pool 的页面文件 |
| **内存映射** | `STORAGE_BACKEND_MMAP` | mmap 文件，操作系统管理换页 |
| **Faiss 原生** | `STORAGE_BACKEND_FAISS` | 兼容 Faiss 原生格式（只读） |

### 2.4 统一接口定义

```c
// 存储后端接口
typedef struct storage_backend {
    storage_backend_type_t type;
    void *ctx;

    // 页面操作
    page_id_t (*alloc_page)(struct storage_backend *backend);
    int (*read_page)(struct storage_backend *backend, page_id_t page_id, page_t *page);
    int (*write_page)(struct storage_backend *backend, page_id_t page_id, const page_t *page);
    int (*flush_page)(struct storage_backend *backend, page_id_t page_id);

    // 批量操作
    int (*batch_write)(struct storage_backend *backend, page_id_t *page_ids, page_t **pages, int count);

    // 生命周期
    int (*sync)(struct storage_backend *backend);
    int (*close)(struct storage_backend *backend);
} storage_backend_t;

// 索引配置
typedef struct index_config {
    // 存储配置
    storage_backend_type_t storage_type;
    const char *storage_path;           // 持久化路径（可选）

    // 持久化开关
    bool persist_enabled;

    // 算法参数
    int32_t dims;
    int32_t M;
    int32_t ef_construction;
    int32_t ef_search;
    distance_metric_t metric;

    // 其他...
} index_config_t;
```

---

## 3. 持久化开关设计

### 3.1 配置参数

```c
// 索引配置中的持久化开关
typedef struct index_config {
    bool persist_enabled;    // true: 完整持久化 | false: 纯内存
    // ...
} index_config_t;
```

### 3.2 两种模式对比

| 特性 | persist_enabled=true | persist_enabled=false |
|------|---------------------|----------------------|
| **WAL 日志** | ✅ 完整 Redo 日志 | ❌ 不写 |
| **崩溃恢复** | ✅ 支持 | ❌ 不支持 |
| **Undo 日志** | ✅ 事务回滚 | ✅ 事务回滚 |
| **Buffer Pool** | ✅ 脏页刷盘 | ✅ 仅缓存 |
| **Checkpoint** | ✅ 支持 | ❌ 无意义 |
| **适用场景** | 生产环境 | 测试/临时分析/缓存 |

### 3.3 模式切换行为

```
创建索引时:
    persist_enabled = true:
        - 分配页面文件
        - 初始化 WAL
        - 启用 Checkpoint

    persist_enabled = false:
        - 仅分配内存
        - 禁用 WAL 写入
        - 无 Checkpoint
```

---

## 4. 统一向量存储设计

### 4.1 问题背景

传统实现中存在三份向量拷贝：

| 存储位置 | 内容 | 膨胀率贡献 |
|----------|------|-----------|
| **Heap** | 完整向量 | 1x |
| **HNSW/DiskANN 图** | 完整向量 | ~1x |
| **量化索引 (PQ/OPQ)** | 量化编码 | ~0.2x |
| **总计** | - | **~2.2x** |

### 4.2 解决方案：Single Source of Truth

采用 Heap 为主存储，索引只存引用：

```
┌─────────────────────────────────────────────────────────────┐
│                         Heap (主存储)                        │
│  ┌─────────┬─────────┬─────────┬─────────┬─────────┐       │
│  │ vector_0│ vector_1│ vector_2│ vector_3│ ...     │       │
│  │ (完整)  │ (完整)  │ (完整)  │ (完整)  │         │       │
│  └────┬────┴────┬────┴────┬────┴────┬────┴─────────┘       │
│       │         │         │         │                       │
│       │ page_id │ page_id │ page_id │                       │
│       │ offset  │ offset  │ offset  │                       │
└───────┼─────────┼─────────┼─────────┼───────────────────────┘
        │         │         │         │
        ▼         ▼         ▼         ▼
┌─────────────────────────────────────────────────────────────┐
│                    索引 (只存引用)                           │
│                                                             │
│  ┌─────────────────────────────────────────────────────┐   │
│  │ HNSW 图索引                                          │   │
│  │  - 节点: {id, layer, neighbors[]}                   │   │
│  │  - 向量引用: {heap_page_id, offset}                 │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
│  ┌─────────────────────────────────────────────────────┐   │
│  │ 量化索引 (PQ/OPQ)                                    │   │
│  │  - 压缩编码 (必须存储，量化后无法还原)               │   │
│  │  - centroid 表 (共享，可引用)                        │   │
│  └─────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

### 4.3 查询流程（重排序）

```
Search(query, k):
    1. 图索引搜索 → 获取 top-k 的 id 列表
    2. 对于每个 id，从 Heap 获取完整向量（可能批量）
    3. 计算精确距离（re-ranking）
    4. 返回排序后的 top-k 结果
```

### 4.4 向量引用结构

```c
// 向量引用（索引中存储）
typedef struct vector_ref {
    page_id_t heap_page_id;    // Heap 页面 ID
    uint32_t offset;           // 页内偏移
    uint32_t length;           // 向量长度（bytes）
} vector_ref_t;

// Heap 存储接口
typedef struct {
    page_id_t (*insert_vector)(heap_t *heap, const float *vector, int32_t dims);
    int (*get_vector)(heap_t *heap, page_id_t page_id, uint32_t offset, float *out_vector);
    // ...
} heap_ops_t;
```

### 4.5 膨胀率对比

| 实现方式 | 膨胀率 |
|----------|--------|
| **传统三拷贝** | ~2.2x |
| **优化后（引用）** | ~1.2x |
| **理想（无量化）** | 1.0x |

---

## 5. 文档体系设计

### 5.1 目录结构

```
docs/
├── index/
│   ├── README.md                    # 索引总览
│   │
│   ├── theory/                      # 索引原理（理论为主）
│   │   ├── hnsw-theory.md          # HNSW 算法原理
│   │   ├── diskann-theory.md       # DiskANN 算法原理
│   │   ├── ivf-theory.md           # IVF 系列原理
│   │   ├── lsh-theory.md           # LSH 原理
│   │   ├── btree-theory.md         # BTree/B+Tree 原理
│   │   ├── art-theory.md           # ART 原理
│   │   ├── cceh-theory.md          # CCEH 原理
│   │   ├── gin-theory.md           # GIN 原理
│   │   └── ...
│   │
│   └── implementation/              # 索引实现（代码为主）
│       ├── hnsw-impl.md            # HNSW 完整实现流程
│       ├── diskann-impl.md         # DiskANN 完整实现流程
│       ├── ivf-impl.md             # IVF 完整实现流程
│       ├── btree-impl.md           # BTree 完整实现流程
│       └── ...
│
└── storage-architecture.md          # 页面管理架构
```

### 5.2 原理文档结构（theory/）

每个原理文档包含：

```markdown
# <索引名> 算法原理

## 1. 算法概述
   - 核心思想
   - 适用场景
   - 优缺点

## 2. 数据结构
   - 核心数据结构
   - 关键字段说明
   - 图示（ASCII 或 Mermaid）

## 3. 核心算法
   - 构建算法
   - 搜索算法
   - 更新/删除算法

## 4. 时间/空间复杂度
   - 构建复杂度
   - 搜索复杂度
   - 空间复杂度

## 5. 参数调优
   - 参数说明
   - 调优建议
   - 典型值

## 6. 与其他索引的对比
   - vs xxx
   - vs xxx
```

### 5.3 实现文档结构（implementation/）

每个实现文档包含：

```markdown
# <索引名> 实现详解

## 1. 文件结构
   - 头文件
   - 源文件
   - 测试文件

## 2. 核心数据结构
   - C 结构体定义
   - 关键字段含义

## 3. 完整流程

### 3.1 创建索引
   - 入口函数
   - 参数校验
   - 初始化步骤
   - 页面分配

### 3.2 插入向量
   - 入口函数
   - 算法流程
   - 存储操作
   - 持久化（可选）

### 3.3 搜索向量
   - 入口函数
   - 两阶段搜索
   - 结果重排序
   - 性能优化点

### 3.4 删除向量
   - 入口函数
   - 逻辑删除 vs 物理删除
   - 回收策略

### 3.5 持久化
   - 检查点机制
   - 页面刷盘
   - 崩溃恢复

## 4. 代码片段
   - 关键代码
   - 注释说明

## 5. 测试用例
   - 单元测试
   - 集成测试
```

---

## 6. 实现计划

### Phase 1: 基础设施重构
- [ ] 定义存储后端接口 `storage_backend_t`
- [ ] 实现四种存储后端
- [ ] 改造现有索引支持配置注入
- [ ] 添加 `persist_enabled` 开关支持

### Phase 2: 向量存储重构
- [ ] 定义向量引用结构 `vector_ref_t`
- [ ] 实现 Heap 主存储
- [ ] 改造 HNSW 使用引用而非拷贝
- [ ] 实现批量获取和预取

### Phase 3: 文档编写
- [ ] 创建 `docs/index/` 目录结构
- [ ] 编写索引原理文档
- [ ] 编写索引实现文档
- [ ] 更新 `docs/storage-architecture.md`

---

## 7. 附录

### A. 相关文件

| 文件 | 说明 |
|------|------|
| `engineering/include/db/index/index.h` | 索引子系统入口 |
| `engineering/include/db/heapam.h` | Heap 访问方法 |
| `engineering/include/db/storage_engine.h` | 存储引擎定义 |
| `engineering/src/db/index/vector_index/` | 向量索引实现 |

### B. 参考资料

- HNSW: Malkov & Yashunin (2018) - "Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs"
- DiskANN: Subramanya et al. (2019) - "DiskANN: Fast Accurate Billion-point Nearest Neighbor Search on a Single Node"
- Zero-copy vector storage: Milvus 架构文档
