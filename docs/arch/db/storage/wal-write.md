> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# WAL 写入流程

## 概述

本文档描述 WAL (Write-Ahead Logging) 的写入流程，包括日志记录格式、写入路径、刷盘策略和检查点机制。

---

## 一、WAL 文件结构

### 1.1 整体布局

```mermaid
flowchart TB
    subgraph "WAL 文件 (64MB 循环)"
        FILE_HDR[WAL 文件头部<br/>64 字节]
        RECORD1[日志记录 1<br/>24 字节头部 + 数据]
        RECORD2[日志记录 2]
        ELLIPSIS[...]
        RECORD_N[日志记录 N]
        FREE_SPACE[空闲空间<br/>文件末尾]
    end

    subgraph "文件头部 (64 字节)"
        MAGIC[magic<br/>0x57414C31<br/>4 字节]
        VERSION[version<br/>1<br/>4 字节]
        PAGE_SZ[page_size<br/>4 字节]
        CKSUM[checksum<br/>4 字节]
        RESERVED[reserved<br/>48 字节]
    end

    FILE_HDR --> MAGIC
    FILE_HDR --> VERSION
    FILE_HDR --> PAGE_SZ
    FILE_HDR --> CKSUM
    FILE_HDR --> RESERVED
```

### 1.2 日志记录格式

```mermaid
flowchart TB
    subgraph "日志记录 (24 字节头部 + 变长数据)"
        REC_HDR[Record Header<br/>24 字节]
        REC_DATA[Record Data<br/>变长]
    end

    subgraph "记录头部 (24 字节)"
        TYPE[type<br/>1 字节<br/>日志类型]
        SIZE[size<br/>3 字节<br/>记录总大小]
        LSN[lsn<br/>8 字节<br/>日志序列号]
        TXN_ID[txn_id<br/>4 字节<br/>事务 ID]
        PREV_LSN[prev_lsn<br/>4 字节<br/>上条日志 LSN]
        CKSUM[checksum<br/>4 字节<br/>记录校验和]
    end

    REC_HDR --> TYPE
    REC_HDR --> SIZE
    REC_HDR --> LSN
    REC_HDR --> TXN_ID
    REC_HDR --> PREV_LSN
    REC_HDR --> CKSUM
```

---

## 二、日志类型

```mermaid
flowchart LR
    subgraph "日志类型枚举"
        BEGIN[BEGIN=7<br/>事务开始]
        INSERT[INSERT=2<br/>插入记录]
        UPDATE[UPDATE=1<br/>更新记录]
        DELETE[DELETE=3<br/>删除记录]
        COMMIT[COMMIT=4<br/>事务提交]
        ABORT[ABORT=5<br/>事务回滚]
        CKPT[CHECKPOINT=6<br/>检查点]
    end

    BEGIN -->|第一个操作| TXN_GROUP[事务日志组]
    INSERT --> TXN_GROUP
    UPDATE --> TXN_GROUP
    DELETE --> TXN_GROUP
    COMMIT -->|事务结束| TXN_GROUP
    ABORT -->|事务回滚| TXN_GROUP
    CKPT -.->|独立于事务| TXN_GROUP
```

---

## 三、写入流程

### 3.1 完整写入路径

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant WAL as WAL Manager
    participant WALBuf as WAL Buffer
    participant Disk as 磁盘

    Caller->>WAL: wal_write_insert(wal, txn_id, key, value)

    WAL->>WAL: 构造日志记录
    WAL->>WAL: 分配 LSN (递增)
    WAL->>WAL: 填充记录头部
    WAL->>WAL: 计算校验和

    WAL->>WALBuf: 追加到 WAL Buffer
    WALBuf-->>WAL: 写入成功

    WAL-->>Caller: 返回 LSN

    Note over Caller,WAL: 后续事务提交时刷盘

    Caller->>WAL: wal_flush(wal)
    WAL->>WALBuf: 获取 Buffer 中未刷数据
    WAL->>WALBuf: 写入文件 (pwrite)
    WALBuf->>Disk: fsync
    Disk-->>WALBuf: fsync 完成
    WALBuf-->>WAL: 刷盘成功
    WAL-->>Caller: 返回 0
```

### 3.2 组提交流程

```mermaid
sequenceDiagram
    participant TXN1 as 事务 1
    participant TXN2 as 事务 2
    participant TXN3 as 事务 3
    participant WAL as WAL Manager
    participant Disk as 磁盘

    TXN1->>WAL: wal_write_insert(LSN=100)
    TXN2->>WAL: wal_write_insert(LSN=101)
    TXN3->>WAL: wal_write_insert(LSN=102)

    TXN1->>WAL: wal_flush() 请求
    Note over WAL: 开始组提交

    WAL->>WAL: 收集未刷数据 (LSN 100-102)
    WAL->>Disk: pwrite (批量写入)
    WAL->>Disk: fsync
    Disk-->>WAL: fsync 完成

    WAL-->>TXN1: 刷盘完成 (LSN=100)
    WAL-->>TXN2: 刷盘完成 (LSN=101)
    WAL-->>TXN3: 刷盘完成 (LSN=102)
```

---

## 四、事务日志示例

### 4.1 完整事务日志链

```mermaid
flowchart LR
    BEGIN_LSN[BEGIN<br/>LSN=100<br/>txn=42]
    INSERT_LSN[INSERT<br/>LSN=101<br/>txn=42<br/>prev=100]
    UPDATE_LSN[UPDATE<br/>LSN=102<br/>txn=42<br/>prev=101]
    COMMIT_LSN[COMMIT<br/>LSN=103<br/>txn=42<br/>prev=102]

    BEGIN_LSN -->|prev_lsn=0| INSERT_LSN
    INSERT_LSN -->|prev_lsn=100| UPDATE_LSN
    UPDATE_LSN -->|prev_lsn=101| COMMIT_LSN
```

### 4.2 并发事务日志交错

```mermaid
flowchart LR
    TXN1_BEGIN[BEGIN<br/>LSN=100<br/>txn=42]
    TXN2_BEGIN[BEGIN<br/>LSN=101<br/>txn=43]
    TXN1_INSERT[INSERT<br/>LSN=102<br/>txn=42]
    TXN2_INSERT[INSERT<br/>LSN=103<br/>txn=43]
    TXN1_COMMIT[COMMIT<br/>LSN=104<br/>txn=42]
    TXN2_ABORT[ABORT<br/>LSN=105<br/>txn=43]

    TXN1_BEGIN -->|prev=0| TXN1_INSERT
    TXN1_INSERT -->|prev=100| TXN1_COMMIT

    TXN2_BEGIN -->|prev=0| TXN2_INSERT
    TXN2_INSERT -->|prev=101| TXN2_ABORT
```

---

## 五、检查点机制

### 5.1 检查点触发条件

```mermaid
flowchart TD
    Start[检查点决策] --> CheckWALSize{WAL 文件<br/>超过 64MB?}

    CheckWALSize -->|是| Trigger[触发检查点]
    CheckWALSize -->|否| CheckTime{距离上次检查点<br/>超过 5 分钟?}

    CheckTime -->|是| Trigger
    CheckTime -->|否| CheckBuffer{脏页数量<br/>超过阈值?}

    CheckBuffer -->|是| Trigger
    CheckBuffer -->|否| Skip[跳过<br/>等待下次]

    Trigger --> DoCheckpoint[执行检查点]
    Skip --> Idle[空闲]
```

### 5.2 检查点执行流程

```mermaid
sequenceDiagram
    participant WAL as WAL Manager
    participant BufMgr as Buffer Manager
    participant Disk as 磁盘

    WAL->>WAL: 记录 CHECKPOINT 开始

    WAL->>BufMgr: 获取所有脏页列表
    BufMgr-->>WAL: 返回脏页数组

    WAL->>WAL: 计算检查点 LSN<br/>= min(脏页 LSN)

    loop 刷脏页
        WAL->>BufMgr: flush_page(page_id)
        BufMgr->>Disk: write_page
        Disk-->>BufMgr: 写入完成
        BufMgr-->>WAL: 刷页完成
    end

    WAL->>WAL: 记录 CHECKPOINT 结束
    Note over WAL: 记录内容：<br/>- checkpoint_lsn<br/>- 脏页列表<br/>- 活动事务

    WAL->>Disk: fsync
    Disk-->>WAL: fsync 完成

    WAL->>WAL: 更新 checkpoint_lsn
```

---

## 六、WAL Buffer 管理

### 6.1 Buffer 结构

```mermaid
classDiagram
    class wal_t {
        +int fd
        +uint64_t current_lsn
        +uint64_t flushed_lsn
        +uint64_t checkpoint_lsn
        +uint8_t* buffer
        +size_t buffer_size
        +size_t buffer_pos
        +pthread_mutex_t lock
        +pthread_cond_t flush_cond
        +uint32_t active_txns
        +wal_state_t state
    }

    class WALBuffer {
        +uint8_t* data
        +size_t capacity
        +size_t write_pos
        +size_t flush_pos
        +bool need_flush
    }

    wal_t "1" --> "1" WALBuffer
```

### 6.2 Buffer 状态机

```mermaid
stateDiagram-v2
    [*] --> Idle

    Idle --> Writing: 追加日志记录
    Writing --> FlushPending: 达到刷盘阈值
    Writing --> Idle: 写入完成

    FlushPending --> Flushing: 触发 fsync
    Flushing --> Idle: fsync 完成

    FlushPending --> Idle: 取消刷盘

    Idle --> Checkpoint: 触发检查点
    Checkpoint --> Idle: 检查点完成

    Idle --> Recovery: 崩溃恢复
    Recovery --> Idle: 恢复完成
```

---

## 七、性能优化

### 7.1 批量写入

```mermaid
flowchart LR
    subgraph "批量写入策略"
        SMALL_WRITES[小写入<br/>单次 100 字节]
        BATCHING[批量合并<br/>累积到 4KB]
        BIG_WRITE[批量写入<br/>一次 pwrite 4KB]
    end

    SMALL_WRITES -->|累积| BATCHING
    BATCHING -->|一次系统调用| BIG_WRITE
```

### 7.2 延迟刷盘

| 策略 | 说明 | 优势 | 风险 |
|------|------|------|------|
| **同步提交** | 每次 COMMIT 都 fsync | 最大可靠性 | 性能差 |
| **异步提交** | 延迟 fsync (默认 200ms) | 性能好 | 崩溃丢数据 |
| **组提交** | 多个事务共享一次 fsync | 平衡性能与可靠性 | 实现复杂 |

---

## 八、关键代码位置

| 功能 | 头文件 | 源文件 |
|------|--------|--------|
| WAL 主接口 | `engine/include/db/wal.h` | `engine/src/db/storage/wal/wal.c` |
| WAL Buffer | `engine/include/db/wal_buf.h` | `engine/src/db/storage/wal/wal_buf.c` |
| 日志记录写入 | `engine/include/db/wal.h` | `engine/src/db/storage/wal/wal.c` |
| 检查点 | `engine/include/db/wal.h` | `engine/src/db/storage/wal/wal.c` |
| 组提交 | `engine/include/db/wal_buf.h` | `engine/src/db/storage/wal/wal_buf.c` |