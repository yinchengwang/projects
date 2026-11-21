> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# Buffer Pool 核心流程

## 概述

本文档详细描述 Buffer Pool 的核心流程，包括页面获取、释放和置换。

---

## 一、Buffer Pool 结构图

```mermaid
classDiagram
    class BufferPool {
        +buffer_t[] buffers
        +hash_table_t hash_table
        +uint64_t[] clock
        +int num_buffers
        +buffer_t* get_page(page_id_t page_id)
        +void unpin_page(page_id_t page_id)
        +void flush_page(page_id_t page_id)
        +void flush_all()
    }

    class Buffer {
        +page_t* page
        +page_id_t page_id
        +int ref_count
        +bool is_dirty
        +uint64_t usage_count
        +pthread_mutex_t lock
        +pthread_cond_t iov
    }

    class HashTable {
        +hash_entry_t** buckets
        +int num_buckets
        +buffer_id_t lookup(page_id_t page_id)
        +void insert(page_id_t page_id, buffer_id_t buf_id)
        +void delete(page_id_t page_id)
    }

    BufferPool "1" --> "*" Buffer
    BufferPool "1" --> "1" HashTable
```

---

## 二、页面获取流程

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant BufMgr as Buffer Manager
    participant HashTab as Hash 表
    participant BufArray as Buffer 数组
    participant Clock as Clock-Sweep
    participant Disk as 磁盘管理

    Caller->>BufMgr: get_page(page_id)
    BufMgr->>HashTab: lookup(page_id)

    alt 页面在缓存中
        HashTab-->>BufMgr: buffer_id
        BufMgr->>BufArray: 获取 buffer[buffer_id]
        BufArray->>BufArray: ref_count++
        BufArray->>BufArray: usage_count++
        BufArray-->>BufMgr: 返回 buffer
        BufMgr-->>Caller: 返回页面指针
    else 页面不在缓存中
        HashTab-->>BufMgr: NOT_FOUND
        BufMgr->>Clock: 选择淘汰 buffer
        Clock-->>BufMgr: victim_buffer_id

        BufMgr->>BufArray: 检查 victim buffer
        alt victim 是脏页
            BufMgr->>Disk: 刷写脏页
            Disk-->>BufMgr: 刷写完成
        end

        BufMgr->>HashTab: 删除旧映射
        BufMgr->>Disk: 读取新页面
        Disk-->>BufMgr: 页面数据

        BufMgr->>BufArray: 更新 buffer
        BufMgr->>HashTab: 插入新映射
        BufArray-->>BufMgr: 返回 buffer
        BufMgr-->>Caller: 返回页面指针
    end
```

---

## 三、页面释放流程

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant BufMgr as Buffer Manager
    participant BufArray as Buffer 数组
    participant HashTab as Hash 表

    Caller->>BufMgr: unpin_page(page_id)
    BufMgr->>HashTab: lookup(page_id)
    HashTab-->>BufMgr: buffer_id

    BufMgr->>BufArray: 获取 buffer[buffer_id]
    BufArray->>BufArray: ref_count--

    alt ref_count == 0
        BufArray->>BufArray: 唤醒等待者
    end

    BufMgr-->>Caller: 释放成功
```

---

## 四、脏页刷写流程

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant BufMgr as Buffer Manager
    participant BufArray as Buffer 数组
    participant WAL as WAL 管理
    participant Disk as 磁盘管理

    Caller->>BufMgr: flush_page(page_id)

    BufMgr->>BufArray: 获取 buffer
    BufArray-->>BufMgr: 返回 buffer

    alt 是脏页
        BufMgr->>WAL: 确保 WAL 已刷写到页面 LSN
        WAL-->>BufMgr: WAL 已刷写

        BufMgr->>Disk: write_page(page_id, page_data)
        Disk-->>BufMgr: 写入完成

        BufMgr->>BufArray: 清除 dirty 标志
    end

    BufMgr-->>Caller: 刷写完成
```

---

## 五、并发控制

### 5.1 Buffer 锁模型

```mermaid
stateDiagram-v2
    [*] --> Unlocked
    Unlocked --> Shared: 获取共享锁
    Unlocked --> Exclusive: 获取排他锁

    Shared --> Shared: 共享锁可重入
    Shared --> Waiting: 升级为排他锁
    Shared --> Unlocked: 释放共享锁

    Exclusive --> Unlocked: 释放排他锁

    Waiting --> Exclusive: 共享锁全部释放
    Waiting --> Unlocked: 获取失败
```

### 5.2 死锁避免策略

```mermaid
flowchart TD
    Start[请求锁] --> Check{检查锁状态}

    Check -->|无锁| Acquire[获取锁]
    Check -->|共享锁| CheckShared{需要排他锁?}
    Check -->|排他锁| Wait[等待释放]

    CheckShared -->|是| Upgrade[尝试升级]
    CheckShared -->|否| AcquireShared[获取共享锁]

    Upgrade --> CheckUpgrade{升级成功?}
    CheckUpgrade -->|是| Acquire
    CheckUpgrade -->|否| Wait

    Wait --> Timeout{超时?}
    Timeout -->|是| Abort[返回错误]
    Timeout -->|否| Check

    Acquire --> Success[成功]
    AcquireShared --> Success
```

---

## 六、性能优化点

### 6.1 预读策略

```mermaid
flowchart LR
    subgraph "预读场景"
        SeqScan[顺序扫描] --> PrefetchNext[预读下一页]
        IndexScan[索引扫描] --> PrefetchLeaf[预读叶子页]
        VectorSearch[向量搜索] --> PrefetchNeighbor[预读邻居节点]
    end

    PrefetchNext --> AsyncIO[异步 I/O]
    PrefetchLeaf --> AsyncIO
    PrefetchNeighbor --> AsyncIO
```

### 6.2 批量刷写

```mermaid
sequenceDiagram
    participant BGWriter as 后台写进程
    participant BufMgr as Buffer Manager
    participant Disk as 磁盘管理

    loop 定期刷写
        BGWriter->>BufMgr: 获取脏页列表
        BufMgr-->>BGWriter: 返回脏页数组

        BGWriter->>BGWriter: 排序按 LSN

        loop 批量写入
            BGWriter->>Disk: write_batch(pages)
            Disk-->>BGWriter: 写入完成
        end
    end
```

---

## 七、关键代码位置

| 功能 | 源文件 |
|------|--------|
| Buffer Pool 主逻辑 | `engine/src/db/storage/buffer/bufmgr.c` |
| Hash 表实现 | `engine/src/db/storage/buffer/buf_table.c` |
| Clock-Sweep 算法 | `engine/src/db/storage/buffer/bufmgr.c` |
| 预读逻辑 | `engine/src/db/storage/buffer/prefetch.c` |