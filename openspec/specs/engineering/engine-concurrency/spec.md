> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# 存储引擎并发安全集成

## Purpose

将统一的锁系统（`lock.h`）集成到存储引擎的各个层次，实现多线程安全访问。

## Background

此规格源自变更 `2026-07-07-mm-storage-phase1-optimize`，用于解决多模态存储引擎缺乏并发安全的问题。

## Requirements

### Requirement: 锁集成层次

锁 SHALL 在以下层次集成：

| 层次 | 保护对象 | 锁模式 |
|------|----------|--------|
| 数据库级锁 | 全局元数据、系统表 | EXCLUSIVE |
| 表级锁 | 表元数据、索引定义 | SHARED/EXCLUSIVE/INTENTION_* |
| 页面级锁 | 数据页面内容 | SHARED/EXCLUSIVE |
| 行级锁（可选） | 单行数据 | FOR UPDATE/FOR SHARE |

### Requirement: 引擎锁上下文

```c
typedef struct engine_lock_ctx_s {
    lock_t *db_lock;           /**< 数据库级锁 */
    rwlock_t *rw_lock;        /**< 读写锁（用于扫描） */
    bool use_fine_grained;     /**< 是否使用细粒度锁 */
    uint32_t ref_count;        /**< 引用计数 */
} engine_lock_ctx_t;
```

### Requirement: 锁 API

| 函数 | 说明 |
|------|------|
| `engine_lock_init()` | 初始化锁上下文 |
| `engine_lock_acquire()` | 获取锁 |
| `engine_lock_release()` | 释放锁 |
| `engine_lock_destroy()` | 销毁锁上下文 |
| `engine_read_lock_acquire()` | 获取读锁 |
| `engine_write_lock_acquire()` | 获取写锁 |

### Requirement: 向量引擎锁策略

| 操作 | 锁模式 | 说明 |
|------|--------|------|
| 插入向量 | WRITE_LOCK | 写索引 |
| 搜索 | READ_LOCK | 只读索引 |
| 构建索引 | EXCLUSIVE | 独占访问 |
| 删除向量 | WRITE_LOCK | 写索引 |

### Requirement: 图引擎锁策略

| 操作 | 锁模式 | 说明 |
|------|--------|------|
| 添加顶点 | WRITE_LOCK | 写顶点表 |
| 添加边 | WRITE_LOCK | 写边表 |
| 查询顶点/边 | READ_LOCK | 只读 |
| CSR Compact | EXCLUSIVE | 独占操作 |

### Requirement: 时序引擎锁策略

| 操作 | 锁模式 | 说明 |
|------|--------|------|
| 插入数据点 | WRITE_LOCK | 写段 |
| 查询（聚合） | READ_LOCK | 只读 |
| 压缩 | EXCLUSIVE | 独占 |
| 设置 TTL | WRITE_LOCK | 修改元数据 |

### Requirement: 文档引擎锁策略

| 操作 | 锁模式 | 说明 |
|------|--------|------|
| 插入文档 | WRITE_LOCK | 写文档+索引 |
| 搜索 | READ_LOCK | 只读索引 |
| 更新文档 | WRITE_LOCK | 写文档 |
| 删除文档 | WRITE_LOCK | 标记删除 |

### Requirement: 死锁检测

- 使用等待图（Wait-for Graph）检测死锁
- 检测周期：100ms
- 超时时间：可配置，默认 5s

### Requirement: 性能目标

| 指标 | 目标 | 说明 |
|------|------|------|
| 单次加锁延迟 | < 1μs | 轻量锁 |
| 并发吞吐量 | 提升 2-4x | 对比无锁实现 |
| 死锁检测 | < 10ms | 图遍历时间 |

## Spec References

- 锁系统实现: `include/db/lock.h`
- Relation 层集成: `src/db/storage/rel.c`
- Buffer Pool 集成: `src/db/storage/buffer/bufmgr.c`
- 测试: `test/db/storage/test_concurrency.cpp`
