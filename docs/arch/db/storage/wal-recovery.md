> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# WAL 恢复流程

## 概述

本文档描述 WAL 的恢复流程，包括崩溃恢复算法、redo/undo 过程和检查点回放。

---

## 一、恢复策略概览

### 1.1 恢复三阶段

```mermaid
flowchart TB
    subgraph "崩溃恢复三个阶段"
        ANALYZE[Phase 1: 分析<br/>扫描 WAL<br/>确定检查点和活动事务]
        REDO[Phase 2: Redo<br/>重做已提交事务<br/>从检查点开始]
        UNDO[Phase 3: Undo<br/>撤销未提交事务<br/>回滚到事务开始]
    end

    CRASH[系统崩溃] --> ANALYZE
    ANALYZE --> REDO
    REDO --> UNDO
    UNDO --> DONE[恢复完成<br/>系统就绪]
```

### 1.2 恢复入口点

```mermaid
flowchart TD
    Start[数据库启动] --> CheckWAL{WAL 文件存在?}

    CheckWAL -->|否| NormalStart[正常启动<br/>无需恢复]
    CheckWAL -->|是| CheckCrash{上次是否<br/>正常关闭?}

    CheckCrash -->|是| NormalStart
    CheckCrash -->|否| DoRecovery[执行崩溃恢复]

    DoRecovery --> Analyze[分析 WAL]
    Analyze --> Redo[执行 Redo]
    Redo --> Undo[执行 Undo]
    Undo --> Done[恢复完成]
```

---

## 二、分析阶段 (Analyze)

### 2.1 分析流程

```mermaid
sequenceDiagram
    participant Recovery as 恢复管理器
    participant WAL as WAL 文件
    participant Catalog as Catalog 系统

    Recovery->>WAL: wal_analyze(path, info)

    Recovery->>WAL: 读取 WAL 文件头部
    WAL-->>Recovery: 文件头 (magic, version)

    Recovery->>Recovery: 定位最后一个检查点

    loop 扫描 WAL 记录
        Recovery->>WAL: 读取下一条记录
        WAL-->>Recovery: 日志记录

        alt BEGIN 记录
            Recovery->>Recovery: 记录事务到 active_txns
        else COMMIT 记录
            Recovery->>Recovery: 从 active_txns 移除
        else ABORT 记录
            Recovery->>Recovery: 从 active_txns 移除
        else CHECKPOINT 记录
            Recovery->>Recovery: 更新 checkpoint_lsn
        end
    end

    Recovery->>Recovery: 确定 redo_start_lsn
    Recovery->>Recovery: 确定 undo_txns 列表

    Recovery-->>Caller: 返回 wal_recovery_info_t
```

### 2.2 确定 Redo 起点

```mermaid
flowchart TD
    Start[确定 redo 起点] --> FindCKPT[查找最后一个<br/>完整检查点]

    FindCKPT --> Extract[提取检查点的<br/>checkpoint_lsn]
    Extract --> RedoStart[redo_start_lsn = checkpoint_lsn]

    RedoStart --> Verify{检查点前的记录<br/>是否都已刷盘?}

    Verify -->|是| Done[确定 redo 起点]
    Verify -->|否| Adjust[向前调整起点<br/>到更早的 LSN]

    Adjust --> Done
```

---

## 三、Redo 阶段

### 3.1 Redo 执行流程

```mermaid
sequenceDiagram
    participant Recovery as 恢复管理器
    participant WAL as WAL 文件
    participant Buffer as Buffer Pool
    participant Disk as 磁盘

    Recovery->>WAL: wal_redo(path, start_lsn, apply_fn, ctx)

    Recovery->>WAL: 定位到 start_lsn

    loop 读取每条日志记录
        Recovery->>WAL: 读取日志记录
        WAL-->>Recovery: 返回记录

        alt INSERT 记录
            Recovery->>Buffer: 获取目标页面
            Recovery->>Buffer: 重做插入操作
            Buffer->>Disk: 写入页面
        else UPDATE 记录
            Recovery->>Buffer: 获取目标页面
            Recovery->>Buffer: 应用新值
            Buffer->>Disk: 写入页面
        else DELETE 记录
            Recovery->>Buffer: 获取目标页面
            Recovery->>Buffer: 重做删除操作
            Buffer->>Disk: 写入页面
        else COMMIT 记录
            Recovery->>Recovery: 标记事务已提交
        else CHECKPOINT 记录
            Recovery->>Recovery: 跳过 (已处理)
        end

        alt 页面 LSN >= 日志 LSN
            Recovery->>Recovery: 跳过 (已应用)
        end
    end

    Recovery-->>Caller: Redo 完成
```

### 3.2 Redo 幂等性

```mermaid
flowchart TD
    Start[处理日志记录] --> GetPageLSN[读取页面的<br/>page_lsn]

    GetPageLSN --> Compare{page_lsn<br/>>=<br/>record_lsn?}

    Compare -->|page_lsn >= record_lsn| Skip[跳过<br/>已应用]
    Compare -->|page_lsn < record_lsn| Apply[应用修改]

    Apply --> UpdatePageLSN[更新 page_lsn<br/>= record_lsn]
    UpdatePageLSN --> MarkDirty[标记脏页]
    MarkDirty --> Done[完成]

    Skip --> Done
```

---

## 四、Undo 阶段

### 4.1 Undo 执行流程

```mermaid
sequenceDiagram
    participant Recovery as 恢复管理器
    participant WAL as WAL 文件
    participant Buffer as Buffer Pool

    loop 每个未提交事务
        Recovery->>WAL: wal_undo(path, txn_id, start_lsn, apply_fn, ctx)

        Recovery->>WAL: 定位到事务的最后一条日志

        loop 逆序遍历日志链
            Recovery->>WAL: 读取日志记录
            WAL-->>Recovery: 返回记录

            alt UPDATE 记录
                Recovery->>Buffer: 获取目标页面
                Recovery->>Buffer: 恢复旧值 (undo)
            else INSERT 记录
                Recovery->>Buffer: 获取目标页面
                Recovery->>Buffer: 删除插入的数据
            else DELETE 记录
                Recovery->>Buffer: 获取目标页面
                Recovery->>Buffer: 恢复删除的数据
            end

            Recovery->>Recovery: prev_lsn 定位上条记录
        end

        Recovery->>WAL: 写入 ABORT 日志
        Recovery-->>Caller: Undo 完成
    end
```

### 4.2 逆序撤销

```mermaid
flowchart LR
    subgraph "事务日志链 (正向)"
        OP1[INSERT<br/>LSN=100]
        OP2[UPDATE<br/>LSN=101]
        OP3[DELETE<br/>LSN=102]
    end

    subgraph "Undo 顺序 (逆向)"
        UNDO3[撤销 DELETE<br/>恢复数据]
        UNDO2[撤销 UPDATE<br/>恢复旧值]
        UNDO1[撤销 INSERT<br/>删除数据]
    end

    OP1 --> OP2
    OP2 --> OP3
    OP3 -.->|逆序| UNDO3
    UNDO3 -.->|prev_lsn=101| UNDO2
    UNDO2 -.->|prev_lsn=100| UNDO1
```

---

## 五、检查点回放

### 5.1 检查点记录结构

```mermaid
classDiagram
    class CheckpointRecord {
        +uint64_t checkpoint_lsn
        +uint64_t redo_start_lsn
        +uint32_t num_dirty_pages
        +uint32_t* dirty_page_ids
        +uint32_t num_active_txns
        +uint32_t* active_txn_ids
    }

    class CheckpointData {
        +CheckpointRecord record
        +time_t checkpoint_time
        +uint64_t wal_file_offset
    }

    class RecoveryInfo {
        +uint64_t last_checkpoint_lsn
        +uint32_t* active_txns
        +size_t active_txn_count
    }

    CheckpointData --> CheckpointRecord
    RecoveryInfo --> CheckpointRecord : 从检查点恢复
```

### 5.2 检查点回放流程

```mermaid
sequenceDiagram
    participant Recovery as 恢复管理器
    participant WAL as WAL 文件
    participant Buffer as Buffer Pool

    Recovery->>WAL: 读取检查点记录

    Recovery->>Recovery: 提取 dirty_page_ids
    Recovery->>Recovery: 提取 active_txn_ids

    loop 验证脏页
        Recovery->>Buffer: 检查页面状态
        Buffer-->>Recovery: 页面 LSN 信息
        Recovery->>Recovery: 确认页面是否需要 redo
    end

    Recovery->>Recovery: 初始化活动事务列表
    Recovery->>Recovery: 设置 redo_start_lsn

    Recovery-->>Caller: 检查点信息加载完成
```

---

## 六、恢复状态机

```mermaid
stateDiagram-v2
    [*] --> Startup: 系统启动

    Startup --> CheckWAL: 检测 WAL 文件
    CheckWAL --> NormalMode: WAL 正常关闭
    CheckWAL --> RecoveryMode: WAL 异常中断

    RecoveryMode --> Analyze: 阶段 1: 分析
    Analyze --> Redo: 阶段 2: Redo
    Redo --> Undo: 阶段 3: Undo

    Undo --> NormalMode: 恢复完成
    NormalMode --> Shutdown: 正常关闭
    Shutdown --> [*]

    NormalMode --> Crash: 系统崩溃
    Crash --> RecoveryMode: 下次启动恢复

    note right of Analyze: 扫描 WAL<br/>确定活动事务
    note right of Redo: 重做已提交<br/>但未刷盘的修改
    note right of Undo: 撤销未提交<br/>事务的修改
```

---

## 七、恢复性能指标

| 指标 | 目标值 | 说明 |
|------|--------|------|
| 恢复时间目标 (RTO) | < 30 秒 | 从崩溃到可用 |
| 恢复点目标 (RPO) | 0 (不丢数据) | 同步提交模式 |
| WAL 分析速度 | > 100 MB/s | 扫描 WAL 文件 |
| Redo 速度 | > 50 MB/s | 重做日志记录 |
| 检查点间隔 | 5 分钟 | 控制恢复时间 |

---

## 八、关键代码位置

| 功能 | 头文件 | 源文件 |
|------|--------|--------|
| WAL 恢复主接口 | `engine/include/db/wal.h` | `engine/src/db/storage/wal/wal.c` |
| 恢复分析 | `engine/include/db/wal.h` | `engine/src/db/storage/wal/wal.c` |
| Redo 执行 | `engine/include/db/wal.h` | `engine/src/db/storage/wal/wal.c` |
| Undo 执行 | `engine/include/db/wal.h` | `engine/src/db/storage/wal/wal.c` |
| WAL-Buffer 协调恢复 | `engine/include/db/wal_buf.h` | `engine/src/db/storage/wal/wal_buf.c` |