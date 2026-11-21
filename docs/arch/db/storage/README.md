> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# 存储核心子系统 - 架构设计

## 概述

存储核心子系统是 db 数据库存储引擎的基础层，负责数据的持久化存储、内存缓存、日志管理和元数据管理。

---

## 一、子系统架构概览

```mermaid
flowchart TB
    subgraph "存储核心子系统"
        subgraph "Buffer Pool 管理"
            BUF_MGR[Buffer Manager<br/>bufmgr.c]
            BUF_TABLE[Buffer Hash 表<br/>buf_table.c]
            CLOCK[Clock-Sweep 算法]
        end

        subgraph "磁盘管理"
            DISK_MGR[Disk Manager<br/>disk.c]
            FILE_MGR[文件管理]
            PAGE_ALLOC[页面分配器]
        end

        subgraph "页面管理"
            PAGE_MGR[Page Manager<br/>page.c]
            FREE_SPACE[空闲空间管理]
            ITEM_PTR[项指针管理]
        end

        subgraph "WAL 日志"
            WAL_MGR[WAL Manager<br/>wal.c]
            WAL_BUF[WAL Buffer<br/>wal_buf.c]
            WAL_RECOVER[恢复管理]
        end

        subgraph "Catalog 系统"
            CAT_MGR[Catalog Manager<br/>catalog.c]
            PG_CLASS[pg_class 表]
            PG_ATTR[pg_attribute 表]
            PG_INDEX[pg_index 表]
        end
    end

    subgraph "上层调用者"
        HEAP_AM[HeapAM]
        BTREE_AM[BTreeAM]
        INDEX[Index 系统]
    end

    HEAP_AM --> BUF_MGR
    BTREE_AM --> BUF_MGR
    INDEX --> BUF_MGR

    BUF_MGR --> BUF_TABLE
    BUF_MGR --> CLOCK
    BUF_MGR --> DISK_MGR
    BUF_MGR --> WAL_MGR

    DISK_MGR --> FILE_MGR
    DISK_MGR --> PAGE_ALLOC
    DISK_MGR --> PAGE_MGR

    PAGE_MGR --> FREE_SPACE
    PAGE_MGR --> ITEM_PTR

    WAL_MGR --> WAL_BUF
    WAL_MGR --> WAL_RECOVER

    BUF_MGR --> CAT_MGR
    CAT_MGR --> PG_CLASS
    CAT_MGR --> PG_ATTR
    CAT_MGR --> PG_INDEX
```

---

## 二、模块清单

| 模块 | 核心文件 | 功能 |
|------|----------|------|
| **Buffer Pool** | `buf.h`, `bufmgr.c` | 页面缓存、Clock-Sweep 置换 |
| **Buffer Hash** | `buf_table.c` | page_id 到 buffer_id 的 Hash 映射 |
| **Disk Manager** | `disk.h`, `disk.c` | 文件读写、页面分配 |
| **Page Manager** | `page.h`, `page.c` | 页面结构、元组插入/读取 |
| **WAL Manager** | `wal.h`, `wal.c` | WAL 写入、检查点、恢复 |
| **WAL Buffer** | `wal_buf.h`, `wal_buf.c` | WAL 内存缓冲 |
| **Catalog** | `catalog.h`, `catalog.c` | 系统表管理、OID 分配 |

---

## 三、小特性索引

| 小特性 | 文档 | 说明 |
|--------|------|------|
| Buffer Pool 核心流程 | [buffer-pool.md](buffer-pool.md) | 页面获取、释放、置换 |
| Clock-Sweep 算法 | [clock-sweep.md](clock-sweep.md) | 页面置换策略 |
| 磁盘 I/O | [disk-io.md](disk-io.md) | 文件读写、页面分配 |
| 页面结构 | [page-structure.md](page-structure.md) | 页面布局、元组管理 |
| WAL 写入流程 | [wal-write.md](wal-write.md) | WAL 记录写入、刷写 |
| WAL 恢复流程 | [wal-recovery.md](wal-recovery.md) | 检查点回放、故障恢复 |
| Catalog 管理 | [catalog-mgmt.md](catalog-mgmt.md) | 系统表 CRUD、OID 分配 |

---

## 四、关键设计决策

### 4.1 Buffer Pool 设计

**决策**: 使用固定大小的 Buffer 数组 + Hash 表查找

**原因**:
- 固定数组便于内存管理和统计
- Hash 表提供 O(1) 的页面查找
- Clock-Sweep 算法平衡了访问频率和最近使用时间

### 4.2 WAL 设计

**决策**: WAL Buffer + 批量刷写

**原因**:
- 减少 fsync 调用次数，提高性能
- 支持组提交，多个事务共享一次刷写
- 保证 ACID 的 Durability 特性

### 4.3 Catalog 设计

**决策**: 内存缓存 + 磁盘持久化

**原因**:
- 频繁访问的元数据缓存在内存中
- 系统表变更记录 WAL，支持恢复
- Hash 表加速元数据查找

---

## 五、性能指标

| 指标 | 目标值 | 说明 |
|------|--------|------|
| Buffer Pool 命中率 | > 95% | 页面在缓存中的比例 |
| 页面获取延迟 | < 1ms | Buffer 命中时 |
| 页面获取延迟 | < 10ms | 需要从磁盘读取时 |
| WAL 刷写延迟 | < 5ms | fsync 调用 |
| 检查点间隔 | 5 分钟 | 平衡恢复时间和性能 |

---

## 六、相关代码位置

| 模块 | 头文件 | 源文件 |
|------|--------|--------|
| Buffer Pool | `engine/include/db/buf.h` | `engine/src/db/storage/buffer/` |
| Disk Manager | `engine/include/db/disk.h` | `engine/src/db/storage/kv/` |
| Page Manager | `engine/include/db/page.h` | `engine/src/db/storage/page/` |
| WAL | `engine/include/db/wal.h` | `engine/src/db/storage/wal/` |
| Catalog | `engine/include/db/catalog.h` | `engine/src/db/storage/catalog/` |
