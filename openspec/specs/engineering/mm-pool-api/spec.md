> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# 统一内存池接口

## Purpose

定义多模态存储引擎的统一内存池接口，支持 Slab 分配器和 Arena 分配器两种模式。

## Background

此规格源自变更 `2026-07-07-mm-storage-phase1-optimize`，用于解决多模态存储引擎的内存碎片化问题。

## Requirements

### Requirement: 内存池类型

统一内存池 SHALL 支持两种类型：

```c
typedef enum {
    MM_POOL_SLAB = 0,   /**< Slab 分配器（固定块大小） */
    MM_POOL_ARENA = 1,  /**< Arena 分配器（可变块大小） */
} mm_pool_type_t;
```

#### Scenario: Slab 分配器场景

- **WHEN** 创建 `type=MM_POOL_SLAB` 的内存池
- **THEN** 所有分配对齐到 `block_size`，超出部分截断

#### Scenario: Arena 分配器场景

- **WHEN** 创建 `type=MM_POOL_ARENA` 的内存池
- **THEN** 按需增长，支持大内存块

### Requirement: 内存池配置

```c
typedef struct mm_pool_config_s {
    mm_pool_type_t type;        /**< 池类型 */
    size_t block_size;          /**< 块大小（Slab）或增长步长（Arena） */
    size_t max_size;            /**< 最大内存限制（0=无限制） */
    size_t initial_size;        /**< 初始预分配大小 */
    const char *name;           /**< 池名称（用于调试） */
    bool thread_safe;           /**< 是否线程安全 */
} mm_pool_config_t;
```

### Requirement: 核心 API

系统 SHALL 提供以下内存池 API：

| 函数 | 说明 |
|------|------|
| `mm_pool_create()` | 创建内存池 |
| `mm_pool_alloc()` | 分配内存 |
| `mm_pool_free()` | 释放内存 |
| `mm_pool_destroy()` | 销毁内存池 |
| `mm_pool_get_stats()` | 获取统计信息 |
| `mm_pool_reset()` | 重置内存池 |

### Requirement: Slab 分配器行为

- 预分配固定大小的内存块
- 分配时从空闲块链表取出
- 释放时将块放回空闲链表
- 不移动已分配的内存

### Requirement: Arena 分配器行为

- 按 chunk 增长分配内存
- 分配时从当前 chunk 分配，不够则创建新 chunk
- 释放时延迟处理，只在 reset/destroy 时真正释放
- 适合生命周期明确的临时内存

### Requirement: 线程安全

- 如果 `config->thread_safe` 为 true，所有操作使用互斥锁保护
- 建议每个线程使用独立的 Arena 池
- 建议 Slab 池在多线程间共享

### Requirement: 引擎集成

以下引擎 SHALL 集成统一内存池：

| 引擎 | 池类型 | 用途 |
|------|--------|------|
| 向量引擎 | Slab | 向量数据块 |
| 图引擎 | Slab | CSR 边数组 |
| 时序引擎 | Arena | 时间段数据 |
| 文档引擎 | Arena | 倒排列表 |

## Spec References

- 实现代码: `include/db/mm_pool.h`, `src/db/storage/mm_pool.c`
- 测试: `test/db/storage/mm_pool_test.cpp`
