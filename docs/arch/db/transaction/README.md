> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# 事务与并发子系统 - 架构设计

## 概述

事务与并发子系统负责管理数据库事务的 ACID 属性，包括事务生命周期、锁管理、MVCC 多版本并发控制、死锁检测和 VACUUM 垃圾回收。

---

## 一、子系统架构概览

```mermaid
flowchart TB
    subgraph "事务与并发子系统"
        subgraph "事务管理器"
            TXN_MGR[事务管理器<br/>txn.c]
            TXN_LIFECYCLE[事务生命周期<br/>BEGIN/COMMIT/ABORT]
            XID_MGR[事务 ID 分配<br/>XID 生成器]
            SNAPSHOT[快照管理<br/>XIP 列表]
        end

        subgraph "锁管理器"
            LOCK_MGR[锁管理器<br/>lock.c]
            LOCK_HASH[锁哈希表<br/>1024 桶]
            DEADLOCK[死锁检测<br/>DFS 环形检测]
            LOCK_ESCALATION[锁升级<br/>行→页→表]
        end

        subgraph "MVCC 引擎"
            MVCC[MVCC Core<br/>mvcc.h]
            VERSION_CHAIN[版本链<br/>xmin/xmax/ctid]
            UNDO_LOG[Undo 日志<br/>回滚记录]
            VISIBILITY[可见性判断<br/>快照隔离]
        end

        subgraph "GC 系统"
            VACUUM[VACUUM<br/>死亡元组清理]
            AUTO_VACUUM[自动 VACUUM<br/>后台任务]
            UNDO_GC[Undo 日志清理<br/>过期回收]
        end
    end

    subgraph "上层调用者"
        DML[DML 操作<br/>INSERT/UPDATE/DELETE]
        DDL[DDL 操作<br/>CREATE/ALTER/DROP]
        SQL_EXEC[SQL 执行器<br/>多语句事务]
    end

    DML --> TXN_MGR
    DML --> LOCK_MGR
    DML --> MVCC
    DDL --> TXN_MGR
    DDL --> LOCK_MGR
    SQL_EXEC --> TXN_MGR

    TXN_MGR --> XID_MGR
    TXN_MGR --> SNAPSHOT
    LOCK_MGR --> LOCK_HASH
    LOCK_MGR --> DEADLOCK
    LOCK_MGR --> LOCK_ESCALATION
    MVCC --> VERSION_CHAIN
    MVCC --> UNDO_LOG
    MVCC --> VISIBILITY
    VACUUM --> MVCC
    AUTO_VACUUM --> VACUUM
    UNDO_GC --> UNDO_LOG
```

---

## 二、事务管理器

### 2.1 事务核心结构

```mermaid
classDiagram
    class txn_context_s {
        +txn_id_t xid
        +txn_state_t state
        +txn_type_t type
        +txn_cid_t current_cid
        +uint64_t start_time
        +txn_xip_list_t* snapshot
        +txn_savepoint_t* savepoints
        +int savepoint_count
        +txn_subxact_t* subxacts
        +int subxact_depth
        +void* undo_chain
        +lock_ctx_t* lock_ctx
    }

    class txn_xip_list {
        +txn_xip_entry_t* entries
        +int count
        +int capacity
    }

    class txn_savepoint {
        +char name[64]
        +txn_cid_t cid
        +txn_id_t subxact_id
        +void* undo_ptr
    }

    class txn_subxact {
        +txn_id_t parent_xid
        +txn_id_t subxid
        +txn_cid_t cid
        +void* undo_head
    }

    txn_context_s "1" --> "1" txn_xip_list
    txn_context_s "1" --> "*" txn_savepoint
    txn_context_s "1" --> "*" txn_subxact
```

### 2.2 事务状态机

```mermaid
stateDiagram-v2
    [*] --> Active: txn_begin()

    Active --> Committed: txn_commit()
    Active --> Aborted: txn_rollback()
    Active --> Prepared: 两阶段提交 prepare

    Prepared --> Committed: 第二阶段提交
    Prepared --> Aborted: 协调节点回滚

    Committed --> [*]
    Aborted --> [*]

    state Active {
        [*] --> Running
        Running --> Savepoint: txn_savepoint()
        Savepoint --> Running: 继续执行
        Running --> SubTransaction: txn_begin_subxact()
        SubTransaction --> Running: txn_commit_subxact()
        SubTransaction --> Running: txn_rollback_subxact()
        Running --> [*]
    }
```

### 2.3 事务生命周期

```mermaid
sequenceDiagram
    participant Client as 客户端
    participant TXN as 事务管理器
    participant XID as XID 分配器
    participant Snapshot as 快照
    participant WAL as WAL 日志

    Client->>TXN: txn_begin(db, type)

    TXN->>XID: 分配 XID
    XID-->>TXN: 返回 xid=42

    TXN->>Snapshot: 创建快照
    Snapshot-->>TXN: 返回 xmin/xmax/xip_list

    TXN->>TXN: 初始化事务上下文
    TXN->>WAL: wal_write_begin(txn=42)
    TXN-->>Client: 返回 txn_context_t*

    Note over Client,TXN: 事务执行中...

    loop 执行 SQL 语句
        Client->>TXN: SQL 操作
        TXN->>TXN: txn_next_cid() 获取 CID
        TXN->>TXN: 执行操作
    end

    Client->>TXN: txn_commit(txn)

    TXN->>WAL: wal_write_commit(txn=42)
    WAL->>WAL: fsync (刷盘)
    WAL-->>TXN: 提交 LSN

    TXN->>Snapshot: 释放快照
    TXN->>TXN: 释放锁
    TXN->>XID: 注销活跃事务
    TXN-->>Client: 返回 0
```

---

## 三、锁管理器

### 3.1 锁结构

```mermaid
classDiagram
    class lock_manager_s {
        +lock_entry_t* lock_table[1024]
        +bool deadlock_detection_enabled
        +uint32_t deadlock_timeout_ms
        +lock_escalation_policy_t escalation_policy
        +atomic_uint_t lock_requests
        +atomic_uint_t lock_waits
        +atomic_uint_t deadlocks
    }

    class lock_entry_s {
        +lock_object_type_t obj_type
        +uint64_t object_id
        +uint64_t table_id
        +lock_mode_t granted_mode
        +txn_t* exclusive_holder
        +lock_request_t* shared_holders
        +lock_request_t* wait_queue
        +atomic_int_t ref_count
    }

    class lock_request_s {
        +txn_t* txn
        +lock_mode_t mode
        +bool granted
        +uint64_t wait_start_time
        +int recursion
        +lock_request_t* next
        +lock_request_t* prev
    }

    class lock_ctx_s {
        +lock_manager_t* manager
        +txn_t* txn
        +lock_record_t* held_locks
        +uint32_t wait_depth
        +bool marked
        +uint64_t wait_start
    }

    lock_manager_s "1" --> "*" lock_entry_s
    lock_entry_s "1" --> "*" lock_request_s
    lock_ctx_s --> lock_manager_s
    lock_ctx_s --> lock_request_s
```

### 3.2 锁兼容矩阵

```mermaid
flowchart TB
    subgraph "锁兼容矩阵"
        COMPAT[锁兼容性]
    end

    COMPAT --> S_SHARED[S 共享锁<br/>与其他 S/IS 兼容]
    COMPAT --> X_EXCLUSIVE[X 排他锁<br/>与任何锁不兼容]
    COMPAT --> IS_IS[IS 意向共享<br/>与 S/IS/IX 兼容]
    COMPAT --> IX_IX[IX 意向排他<br/>与 IS/IX 兼容]
    COMPAT --> SIX_SIX[SIX 共享意向排他<br/>与 IS 兼容]
```

| 请求\已授予 | S | X | IS | IX | SIX |
|------------|---|---|----|----|-----|
| **S** | ✓ | ✗ | ✓ | ✗ | ✗ |
| **X** | ✗ | ✗ | ✗ | ✗ | ✗ |
| **IS** | ✓ | ✗ | ✓ | ✓ | ✓ |
| **IX** | ✗ | ✗ | ✓ | ✓ | ✗ |
| **SIX** | ✗ | ✗ | ✓ | ✗ | ✗ |

### 3.3 锁获取流程

```mermaid
sequenceDiagram
    participant TXN as 事务
    participant LockMgr as 锁管理器
    participant Hash as 锁哈希表
    participant Wait as 等待队列

    TXN->>LockMgr: lock_acquire(mgr, txn, obj_type, obj_id, mode)

    LockMgr->>Hash: 计算哈希槽

    alt 锁不存在
        LockMgr->>Hash: 创建新锁条目
        LockMgr->>Hash: 授予锁
        LockMgr-->>TXN: LOCK_OK
    else 锁存在
        LockMgr->>LockMgr: 检查兼容性

        alt 兼容
            LockMgr->>Hash: 授予锁（共享锁重入）
            LockMgr-->>TXN: LOCK_OK
        else 不兼容
            LockMgr->>Wait: 加入等待队列（FIFO）

            alt 超时
                LockMgr-->>TXN: LOCK_TIMEOUT
            else 死锁检测
                LockMgr->>LockMgr: 检测死锁
                alt 检测到死锁
                    LockMgr-->>TXN: LOCK_DEADLOCK
                else 无死锁
                    LockMgr->>LockMgr: 等待锁释放
                    LockMgr-->>TXN: LOCK_OK
                end
            end
        end
    end
```

### 3.4 死锁检测

```mermaid
flowchart TD
    Start[lock_acquire] --> CheckWait{进入等待队列}

    CheckWait -->|首次等待| RecordStart[记录等待开始时间]
    RecordStart --> CheckTimeout{超时检测}

    CheckTimeout -->|未超时| Wait[等待锁]
    Wait --> CheckTimeout

    CheckTimeout -->|超时| StartDetection[触发死锁检测]

    StartDetection --> BuildGraph[构建等待图]
    BuildGraph --> DFS[DFS 遍历找环]

    DFS --> CheckCycle{找到环?}

    CheckCycle -->|是| SelectVictim[选择牺牲者<br/>按权重：年龄/修改量]
    SelectVictim --> Rollback[回滚牺牲事务]
    Rollback --> Release[释放牺牲事务的锁]

    CheckCycle -->|否| NoDeadlock[无死锁<br/>继续等待]

    Release --> NotifyVictim[通知牺牲事务]
    NotifyVictim --> ReturnDeadlock[返回 LOCK_DEADLOCK]
    NoDeadlock --> Return[继续等待]

    Wait --> Grant[锁被授予]
    Grant --> ReturnOK[返回 LOCK_OK]
```

---

## 四、MVCC 多版本并发控制

### 4.1 MVCC 版本链

```mermaid
flowchart LR
    subgraph "版本链"
        V1[版本 1<br/>xmin=10, xmax=15<br/>name=Alice]
        V2[版本 2<br/>xmin=15, xmax=20<br/>name=Bob]
        V3[版本 3<br/>xmin=20, xmax=0<br/>name=Charlie]
    end

    subgraph "事务时间线"
        T10[事务 10<br/>INSERT]
        T15[事务 15<br/>UPDATE]
        T20[事务 20<br/>UPDATE]
    end

    V1 -->|ctid| V2
    V2 -->|ctid| V3

    T10 -->|创建| V1
    T15 -->|删除+xmin=15| V1
    T15 -->|创建| V2
    T20 -->|删除+xmax=20| V2
    T20 -->|创建| V3
```

### 4.2 可见性判断

```mermaid
flowchart TD
    Start[可见性判断] --> CheckXmin{xmin 事务已提交?}

    CheckXmin -->|未提交| CheckSelf{xmin == 当前事务?}
    CheckSelf -->|是| CheckXmax
    CheckSelf -->|否| Invisible[不可见]

    CheckXmin -->|已提交| CheckXmax{xmax 值}

    CheckXmax -->|xmax == 0| Visible[可见<br/>未被删除]
    CheckXmax -->|xmax == 当前事务| CheckXmax2[当前事务删除?]
    CheckXmax -->|xmax 已提交| Invisible

    CheckXmax2 -->|是| Invisible
    CheckXmax2 -->|否| CheckXminActive

    CheckXminActive{xmin 在<br/>活跃列表?}

    CheckXminActive -->|是| Invisible
    CheckXminActive -->|否| Visible
```

### 4.3 快照隔离

```mermaid
sequenceDiagram
    participant TXN1 as 事务 A (xid=10)
    participant TXN2 as 事务 B (xid=15)
    participant MVCC as MVCC 引擎
    participant Row as 行数据

    Note over TXN1,TXN2: 事务 A 开始，创建快照 xmin=10, xmax=16

    TXN1->>Row: 读取行
    Row-->>TXN1: xmin=5, xmax=0, value=100

    TXN2->>MVCC: 开始事务 (xid=15)
    TXN2->>Row: 更新行 value=200
    Row->>Row: 创建新版本 xmin=15, xmax=0
    Row->>Row: 旧版本 xmax=15

    TXN1->>Row: 再次读取（使用旧快照）
    Row-->>TXN1: xmin=5, xmax=0, value=100
    Note over TXN1: 快照隔离：看到旧值

    TXN2->>MVCC: 提交事务
    TXN1->>Row: 再次读取（使用旧快照）
    Row-->>TXN1: xmin=5, xmax=0, value=100
    Note over TXN1: 仍看到旧值<br/>(Repeatable Read)

    TXN1->>MVCC: 提交事务
    TXN3->>MVCC: 新事务，新快照
    TXN3->>Row: 读取
    Row-->>TXN3: xmin=15, xmax=0, value=200
    Note over TXN3: 看到事务 B 的修改
```

---

## 五、Undo 日志

### 5.1 Undo 链

```mermaid
flowchart LR
    subgraph "Undo 日志链"
        TXN_HEAD[事务 42<br/>Undo 链头]
        UNDO1[Undo 记录 1<br/>UPDATE<br/>old_value=100]
        UNDO2[Undo 记录 2<br/>INSERT<br/>old_value=null]
        UNDO3[Undo 记录 3<br/>DELETE<br/>old_value=200]
    end

    TXN_HEAD -->|prev_undo| UNDO1
    UNDO1 -->|prev_undo| UNDO2
    UNDO2 -->|prev_undo| UNDO3
    UNDO3 -->|prev_undo=0| NULL
```

### 5.2 回滚流程

```mermaid
sequenceDiagram
    participant TXN as 事务管理器
    participant Undo as Undo 日志
    participant Row as 行数据

    TXN->>TXN: txn_rollback(txn)

    loop 逆序处理 Undo 链
        TXN->>Undo: 获取下一条 Undo 记录

        alt INSERT
            TXN->>Row: 删除插入的行
        else UPDATE
            TXN->>Row: 恢复旧值
        else DELETE
            TXN->>Row: 恢复被删除的行
        end

        TXN->>Undo: prev_undo 定位上一条
    end

    TXN->>TXN: 释放锁
    TXN->>TXN: 释放快照
    TXN->>WAL: 写入 ABORT 日志
    TXN-->>Caller: 回滚完成
```

---

## 六、VACUUM 垃圾回收

### 6.1 GC 触发条件

```mermaid
flowchart TD
    Start[VACUUM 决策] --> CheckAuto{自动 VACUUM<br/>启用?}

    CheckAuto -->|是| CheckTimer{距离上次 GC<br/>超过 naptime?}
    CheckAuto -->|否| Manual[手动 VACUUM]

    CheckTimer -->|是| CheckThreshold{死亡元组<br/>超过阈值?}

    CheckThreshold -->|是| CheckScale{死亡元组比例<br/>超过 scale_factor?}

    CheckThreshold -->|否| Skip[跳过]
    CheckScale -->|是| DoVacuum[执行 VACUUM]
    CheckScale -->|否| Skip

    Manual --> DoVacuum
```

### 6.2 VACUUM 执行流程

```mermaid
sequenceDiagram
    participant VAC as VACUUM
    participant Table as 表数据
    participant Index as 索引
    participant FSM as 空闲空间映射

    VAC->>Table: 扫描表页面

    VAC->>Table: 获取事务快照（确定可见性）

    loop 遍历页面
        VAC->>Table: 读取页面

        loop 遍历页面元组
            VAC->>VAC: 检查元组可见性

            alt 死亡元组（对所有事务不可见）
                VAC->>Index: 清理索引条目
                VAC->>Table: 标记死亡元组为空闲
                VAC->>FSM: 更新空闲空间
            else 最近死亡元组
                VAC->>VAC: 跳过（保留给运行中的事务）
            end
        end

        alt 页面全空
            VAC->>Table: 释放页面
        else 部分空闲
            VAC->>Table: 压缩元组
        end
    end

    VAC->>Table: 更新统计信息
    VAC->>Table: 更新 pg_class 的 npages/ntupes
```

---

## 七、事务隔离级别

| 隔离级别 | 脏读 | 不可重复读 | 幻读 | 实现方式 |
|---------|------|-----------|------|---------|
| **Read Uncommitted** | 可能 | 可能 | 可能 | 不检查 xmax |
| **Read Committed** | 避免 | 可能 | 可能 | 每语句新快照 |
| **Repeatable Read** | 避免 | 避免 | 可能 | 事务开始快照 |
| **Serializable** | 避免 | 避免 | 避免 | 快照 + 冲突检测 |

---

## 八、性能指标

| 指标 | 目标值 | 说明 |
|------|--------|------|
| 事务吞吐量 | > 1000 tps | 短事务 |
| 锁获取延迟 | < 10 μs | 无冲突 |
| 死锁检测 | < 1 ms | 100 个活跃事务 |
| MVCC 可见性判断 | < 1 μs | 缓存命中 |
| VACUUM 速率 | > 1000 死亡元组/s | 正常负载 |
| 最大并发事务 | 1024 | TXN_MAX_ACTIVE |

---

## 九、关键代码位置

| 功能 | 头文件 | 源文件 |
|------|--------|--------|
| 事务管理器 | `engine/include/db/txn.h` | `engine/src/db/txn/` |
| 锁管理器 | `engine/include/db/lock.h` | `engine/src/db/lock/` |
| MVCC 引擎 | `engine/include/db/concurrency/mvcc.h` | `engine/src/db/concurrency/` |
| 乐观锁 | `engine/include/db/optimistic.h` | `engine/src/db/optimistic/` |