> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# 磁盘 I/O 管理

## 概述

本文档描述数据库存储引擎的磁盘 I/O 子系统，负责底层文件读写、页面分配和元数据管理。

---

## 一、磁盘管理器结构

### 1.1 核心结构

```mermaid
classDiagram
    class db_file_s {
        +int fd
        +char* path
        +size_t page_size
        +page_id_t total_pages
        +bool is_open
        +pthread_rwlock_t rwlock
        +int open_flags
    }

    class DiskOps {
        +db_file_t* open(path, page_size)
        +void close(file)
        +page_t* read_page(file, page_id)
        +int write_page(file, page_id, page)
        +page_t* alloc_page(file, type, out_page_id)
        +int free_page(file, page_id)
        +int sync(file)
    }

    class FileHeader {
        +uint32_t magic
        +uint32_t version
        +uint32_t page_size
        +uint32_t num_pages
        +uint32_t first_free_page
        +uint32_t checksum
        +uint8_t reserved[48]
    }

    DiskOps --> db_file_s : 操作
    db_file_s --> FileHeader : 文件头部
```

### 1.2 文件布局

```mermaid
flowchart TB
    subgraph "数据库文件布局"
        HEADER[文件头部<br/>64 字节]
        PAGE0[Page 0<br/>8KB 元数据页]
        PAGE1[Page 1<br/>8KB 数据页]
        PAGE2[Page 2<br/>8KB 数据页]
        PAGE3[Page 3<br/>8KB 索引页]
        ELLIPSIS[...]
        PAGE_N[Page N<br/>8KB ...]
    end

    subgraph "文件头部结构"
        MAGIC[魔数 0x44421021<br/>4 字节]
        VERSION[版本号<br/>4 字节]
        PAGE_SZ[页面大小<br/>4 字节]
        NUM_PAGES[总页数<br/>4 字节]
        FREE_PAGE[空闲页链表头<br/>4 字节]
        CKSUM[校验和<br/>4 字节]
        RESERVED[保留<br/>40 字节]
    end

    HEADER --> MAGIC
    HEADER --> VERSION
    HEADER --> PAGE_SZ
    HEADER --> NUM_PAGES
    HEADER --> FREE_PAGE
    HEADER --> CKSUM
    HEADER --> RESERVED
```

---

## 二、读写流程

### 2.1 页面读取

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant DiskMgr as 磁盘管理器
    participant File as 文件系统
    participant Cache as 页面缓存

    Caller->>DiskMgr: disk_read_page(file, page_id)

    DiskMgr->>DiskMgr: 计算文件偏移<br/>offset = page_id * page_size + 64

    DiskMgr->>File: pread(fd, buf, page_size, offset)
    File-->>DiskMgr: 返回页面数据

    alt 读取成功
        DiskMgr->>DiskMgr: 验证校验和
        DiskMgr->>DiskMgr: 解析页面头部
        DiskMgr-->>Caller: 返回 page_t* 指针
    else 校验和错误
        DiskMgr->>DiskMgr: 记录错误日志
        DiskMgr-->>Caller: 返回 NULL
    else 读取失败
        DiskMgr-->>Caller: 返回 NULL
    end
```

### 2.2 页面写入

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant DiskMgr as 磁盘管理器
    participant File as 文件系统

    Caller->>DiskMgr: disk_write_page(file, page_id, page)

    DiskMgr->>DiskMgr: 计算文件偏移<br/>offset = page_id * page_size + 64

    DiskMgr->>DiskMgr: 更新页面校验和

    DiskMgr->>File: pwrite(fd, page, page_size, offset)
    File-->>DiskMgr: 写入完成

    alt 写入成功
        DiskMgr-->>Caller: 返回 0
    else 写入失败
        DiskMgr-->>Caller: 返回 -1
    end
```

### 2.3 页面分配

```mermaid
flowchart TD
    Start[disk_alloc_page] --> CheckFree{检查空闲页链表}

    CheckFree -->|有空闲页| PopFree[从空闲链表取出]
    CheckFree -->|无空闲页| Extend[扩展文件末尾]

    PopFree --> InitPage[初始化页面头部]
    Extend --> GrowFile[文件追加一页大小]
    GrowFile --> InitPage

    InitPage --> SetType[设置页面类型]
    SetType --> ClearData[清空数据区]
    ClearData --> UpdateMeta[更新文件元数据<br/>num_pages++]

    UpdateMeta --> SyncMeta[同步文件头部]
    SyncMeta --> Return[返回新页面指针]

    Return --> Success[分配成功]
```

---

## 三、空闲空间管理

### 3.1 空闲页链表

```mermaid
flowchart LR
    subgraph "空闲页链表"
        HEAD[文件头部<br/>first_free_page]
        FP1[空闲页 5<br/>next_free: 12]
        FP2[空闲页 12<br/>next_free: 23]
        FP3[空闲页 23<br/>next_free: 0]
    end

    HEAD -->|指向第一个空闲页| FP1
    FP1 -->|链表指针| FP2
    FP2 -->|链表指针| FP3
    FP3 -->|0 表示链表尾| NULL[NULL]
```

### 3.2 分配与释放

```mermaid
stateDiagram-v2
    [*] --> Idle

    Idle --> Allocating: 请求新页面
    Allocating --> CheckFreeList: 检查空闲链表
    CheckFreeList --> PopFromList: 链表非空
    CheckFreeList --> ExtendFile: 链表为空

    PopFromList --> UpdateList: 更新链表头指针
    ExtendFile --> GrowFile: 增加文件大小
    GrowFile --> UpdateHeader: 更新头部信息

    UpdateList --> ReturnPage: 返回页面
    UpdateHeader --> ReturnPage
    ReturnPage --> Idle

    Idle --> Freeing: 释放页面
    Freeing --> MarkFree: 标记为空闲
    MarkFree --> PushToList: 加入空闲链表
    PushToList --> UpdateHeader: 更新头部
    UpdateHeader --> Idle
```

---

## 四、元数据操作

### 4.1 元数据读写

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant DiskMgr as 磁盘管理器
    participant File as 文件系统

    Caller->>DiskMgr: disk_get_meta(file, key, value, len)

    DiskMgr->>DiskMgr: 计算元数据区偏移
    DiskMgr->>File: 读取元数据块
    File-->>DiskMgr: 返回元数据

    alt 元数据存在
        DiskMgr->>DiskMgr: 按 key 查找
        DiskMgr-->>Caller: 返回 0，填充 value
    else 元数据不存在
        DiskMgr-->>Caller: 返回 1
    end

    Caller->>DiskMgr: disk_set_meta(file, key, value, len)

    DiskMgr->>DiskMgr: 定位或创建元数据槽
    DiskMgr->>File: 写入元数据
    File-->>DiskMgr: 写入完成
    DiskMgr-->>Caller: 返回 0
```

### 4.2 元数据存储格式

```mermaid
flowchart TB
    subgraph "元数据区结构"
        META_HEADER[元数据块头部<br/>16 字节]
        ENTRY1[条目 1<br/>key_hash + key + value]
        ENTRY2[条目 2<br/>key_hash + key + value]
        ELLIPSIS[...]
        ENTRY_N[条目 N]
        FREE[空闲空间]
    end

    subgraph "元数据条目格式"
        K_HASH[key_hash<br/>4 字节]
        K_LEN[key_len<br/>2 字节]
        V_LEN[value_len<br/>2 字节]
        KEY[key 数据<br/>变长]
        VALUE[value 数据<br/>变长]
    end

    ENTRY1 --> K_HASH
    ENTRY1 --> K_LEN
    ENTRY1 --> V_LEN
    ENTRY1 --> KEY
    ENTRY1 --> VALUE
```

---

## 五、写放大与优化

### 5.1 写放大问题

```mermaid
flowchart LR
    subgraph "写放大因素"
        PAGE_WRITE[页面写入<br/>8KB 最小单位]
        FS_OVERHEAD[文件系统<br/>4KB 块对齐]
        JOURNAL[日志写入<br/>WAL 记录]
        CHECKPOINT[检查点<br/>刷脏页]
    end

    PAGE_WRITE --> AMPLIFY[写放大系数<br/>2x ~ 10x]
    FS_OVERHEAD --> AMPLIFY
    JOURNAL --> AMPLIFY
    CHECKPOINT --> AMPLIFY
```

### 5.2 优化策略

```mermaid
flowchart TB
    subgraph "优化策略矩阵"
        BATCH_WRITE[批量写入<br/>合并小 I/O 为大 I/O]
        DIRECT_IO[直接 I/O<br/>绕过文件系统缓存]
        PREALLOC[预分配<br/>避免频繁扩展]
        ASYNC_IO[异步 I/O<br/>不阻塞调用者]
        O_DIRECT[O_DIRECT 模式<br/>减少内核缓存]
    end

    BATCH_WRITE -->|效果| REDUCE_FSYNC[减少 fsync 次数]
    DIRECT_IO -->|效果| AVOID_DOUBLE[避免双缓冲]
    PREALLOC -->|效果| REDUCE_FRAG[减少文件碎片]
    ASYNC_IO -->|效果| IMPROVE_THROUGHPUT[提升吞吐量]
```

---

## 六、性能指标

| 指标 | 目标值 | 说明 |
|------|--------|------|
| 顺序读取吞吐 | > 500 MB/s | SSD 上 |
| 随机读取延迟 | < 100 μs | 页面命中文件系统缓存 |
| 随机写入延迟 | < 500 μs | 页面写入 |
| 写入放大系数 | < 3x | 不含 WAL |
| 空闲页分配 | O(1) | 从空闲链表头取出 |
| 文件扩展 | O(1) 均摊 | 预分配减少频繁扩展 |

---

## 七、关键代码位置

| 功能 | 头文件 | 源文件 |
|------|--------|--------|
| 磁盘 I/O 主接口 | `engine/include/db/disk.h` | `engine/src/db/storage/kv/disk.c` |
| 空闲页面管理 | `engine/include/db/disk.h` | `engine/src/db/storage/kv/disk.c` |
| 元数据读写 | `engine/include/db/disk.h` | `engine/src/db/storage/kv/disk.c` |