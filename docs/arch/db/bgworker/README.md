> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# 后台任务调度器 - 架构设计

## 概述

后台任务调度器（bgworker）负责管理数据库中的周期性后台任务，包括索引维护、统计信息收集、CCEH 哈希表缩容等。采用单线程事件循环 + 任务槽位的架构，类似 PostgreSQL 的 bgworker 机制。

---

## 一、子系统架构概览

```mermaid
flowchart TB
    subgraph "后台任务调度器"
        subgraph "公共 API"
            START[db_bgworker_start<br/>启动调度器]
            STOP[db_bgworker_stop<br/>停止调度器]
            REGISTER[db_bgworker_register<br/>注册任务]
            UNREGISTER[db_bgworker_unregister<br/>注销任务]
            PAUSE[db_bgworker_pause<br/>暂停任务]
            RESUME[db_bgworker_resume<br/>恢复任务]
            STATS[db_bgworker_get_stats<br/>获取统计]
            STATUS[db_bgworker_get_status<br/>获取状态]
        end

        subgraph "调度器核心"
            SCHEDULER[db_bg_worker<br/>调度器实例]
            MAIN_LOOP[_db_bgworker_main_loop<br/>调度器主循环]
            SLOTS[任务槽位数组<br/>16 个槽位]
            MUTEX[互斥锁<br/>pthread_mutex_t]
        end

        subgraph "任务接口"
            TASK_IFACE[任务定义<br/>db_bg_task_t]
            TASK_CTX[任务上下文<br/>db_bg_task_context_t]
            INIT_CB[init 初始化回调]
            CHECK_CB[check 检查回调]
            EXEC_CB[execute 执行回调]
            CLEANUP_CB[cleanup 清理回调]
        end

        subgraph "内置任务"
            CCEH_SHRINK[CCEH 缩容任务<br/>task_cceh_shrink.c]
            STATS_COLLECTOR[统计收集任务<br/>task_stats.c]
        end
    end

    subgraph "上层调用者"
        DB_INIT[数据库初始化<br/>initdb]
        GUC[GUC 配置系统<br/>bgworker.enabled]
        CCEH_IDX[CCEH 哈希索引]
    end

    DB_INIT --> START
    DB_INIT --> REGISTER
    GUC --> START
    REGISTER --> CCEH_SHRINK
    REGISTER --> STATS_COLLECTOR
    CCEH_SHRINK --> CCEH_IDX
    START --> MAIN_LOOP
    MAIN_LOOP --> SLOTS
    MAIN_LOOP --> MUTEX
    SLOTS --> TASK_IFACE
    TASK_IFACE --> INIT_CB
    TASK_IFACE --> CHECK_CB
    TASK_IFACE --> EXEC_CB
    TASK_IFACE --> CLEANUP_CB
```

---

## 二、核心数据结构

### 2.1 调度器结构

```mermaid
classDiagram
    class db_bg_worker {
        +pthread_t thread
        +db_bg_task_slot_t slots[16]
        +uint32_t num_slots
        +atomic_bool running
        +atomic_bool stopping
        +uint64_t start_time_ms
        +uint32_t tick_ms
        +pthread_mutex_t mutex
    }

    class db_bg_task_slot {
        +db_bg_task_t* task
        +uint64_t next_check_time
        +bool in_use
    }

    class db_bg_task {
        +const char* name
        +uint32_t interval_ms
        +db_bg_task_init_fn init
        +db_bg_task_check_fn check
        +db_bg_task_execute_fn execute
        +db_bg_task_cleanup_fn cleanup
        +db_bg_task_context_t* ctx
    }

    class db_bg_task_context {
        +void* user_data
        +uint64_t last_check_time
        +uint32_t consecutive_trigger
        +atomic_uint status
    }

    class db_bgworker_stats {
        +uint32_t task_count
        +uint64_t running_time_ms
        +uint64_t total_cycles
        +uint64_t total_executions
    }

    db_bg_worker "1" --> "*" db_bg_task_slot : 包含
    db_bg_task_slot "1" --> "1" db_bg_task : 引用
    db_bg_task "1" --> "1" db_bg_task_context : 包含
    db_bg_worker --> db_bgworker_stats : 统计
```

### 2.2 任务状态机

```mermaid
stateDiagram-v2
    [*] --> IDLE: 任务注册

    IDLE --> CHECKING: 达到检查时间
    CHECKING --> EXECUTING: check 返回 > 0
    CHECKING --> IDLE: check 返回 = 0

    EXECUTING --> IDLE: execute 完成
    EXECUTING --> PAUSED: pause 调用

    PAUSED --> IDLE: resume 调用
    PAUSED --> STOPPED: unregister 调用

    IDLE --> STOPPED: unregister 调用
    IDLE --> PAUSED: pause 调用

    CHECKING --> STOPPED: 调度器停止
    EXECUTING --> STOPPED: 调度器停止

    STOPPED --> [*]: 清理完成

    note right of IDLE: 等待 interval_ms
    note right of EXECUTING: 可能耗时
```

---

## 三、调度器主循环

### 3.1 主循环流程

```mermaid
flowchart TD
    Start[调度器主循环启动] --> Loop{atomic_load running?}

    Loop -->|是| Tick[等待 tick_ms<br/>默认 1000ms]

    Tick --> Lock[加锁]
    Lock --> Iterate[遍历所有槽位]

    Iterate --> CheckSlot{slot in_use?}

    CheckSlot -->|否| NextSlot[下一个槽位]
    NextSlot --> Iterate

    CheckSlot -->|是| CheckTime{当前时间 >=<br/>next_check_time?}

    CheckTime -->|否| NextSlot

    CheckTime -->|是| SetChecking[设置状态: CHECKING]
    SetChecking --> Unlock[解锁]

    Unlock --> CallCheck[调用 task->check(ctx)]

    CallCheck --> CheckResult{返回值}

    CheckResult -->|> 0 满足条件| SetExecuting[设置状态: EXECUTING]
    CheckResult -->|= 0 不满足| SetIdle1[设置状态: IDLE]

    SetExecuting --> CallExecute[调用 task->execute(ctx)]
    CallExecute --> SetIdle2[设置状态: IDLE]

    SetIdle1 --> UpdateNextCheck[更新 next_check_time<br/>= 当前时间 + interval_ms]
    SetIdle2 --> UpdateNextCheck

    UpdateNextCheck --> Lock2[加锁]
    Lock2 --> Loop

    Loop -->|否| Stop[停止调度器]
    Stop --> Cleanup[清理所有任务]
    Cleanup --> Done[调度器退出]
```

### 3.2 主循环序列图

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant Scheduler as 调度器
    participant Slot1 as 槽位 1 (CCEH)
    participant Slot2 as 槽位 2 (Stats)

    Caller->>Scheduler: db_bgworker_start()

    Scheduler->>Scheduler: 创建后台线程
    Scheduler->>Scheduler: _db_bgworker_main_loop

    loop 每个 tick (1000ms)
        Scheduler->>Slot1: 检查 next_check_time
        Slot1-->>Scheduler: 已到检查时间

        Scheduler->>Slot1: 设置 CHECKING 状态
        Scheduler->>Slot1: check(ctx)
        Slot1-->>Scheduler: 返回 1（满足条件）

        Scheduler->>Slot1: 设置 EXECUTING 状态
        Scheduler->>Slot1: execute(ctx) 执行缩容
        Slot1-->>Scheduler: 执行完成

        Scheduler->>Slot1: 设置 IDLE 状态
        Scheduler->>Slot1: 更新 next_check_time

        Scheduler->>Slot2: 检查 next_check_time
        Slot2-->>Scheduler: 已到检查时间

        Scheduler->>Slot2: 设置 CHECKING 状态
        Scheduler->>Slot2: check(ctx)
        Slot2-->>Scheduler: 返回 1（总是满足）

        Scheduler->>Slot2: 设置 EXECUTING 状态
        Scheduler->>Slot2: execute(ctx) 收集统计
        Slot2-->>Scheduler: 执行完成

        Scheduler->>Slot2: 设置 IDLE 状态
        Scheduler->>Slot2: 更新 next_check_time
    end

    Caller->>Scheduler: db_bgworker_stop()
    Scheduler->>Scheduler: 设置 atomic_stopping = true
    Scheduler->>Scheduler: 等待线程退出 (30s 超时)
    Scheduler->>Scheduler: 清理所有任务
    Scheduler-->>Caller: 停止完成
```

---

## 四、任务生命周期

### 4.1 任务注册流程

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant API as db_bgworker_register
    participant Scheduler as 调度器
    participant Task as 任务定义

    Caller->>API: db_bgworker_register(task, slot_out)

    API->>Scheduler: 加锁

    API->>Scheduler: 查找空闲槽位

    alt 无空闲槽位（已满 16 个）
        API->>Scheduler: 解锁
        API-->>Caller: 返回 -1
    else 找到空闲槽位
        API->>Task: 分配上下文 _db_bgworker_alloc_context
        API->>Task: 调用 task->init(ctx)
        Task-->>API: 初始化成功

        API->>Scheduler: 写入槽位
        API->>Scheduler: 设置 in_use = true
        API->>Scheduler: 设置 next_check_time = now

        API->>Scheduler: 解锁
        API-->>Caller: 返回 0，slot_out
    end
```

### 4.2 任务注销流程

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant API as db_bgworker_unregister
    participant Scheduler as 调度器
    participant Task as 任务

    Caller->>API: db_bgworker_unregister(name)

    API->>Scheduler: 加锁

    API->>Scheduler: 按名称查找槽位

    alt 未找到
        API->>Scheduler: 解锁
        API-->>Caller: 返回 -1
    else 找到
        API->>Task: 设置状态 STOPPED
        API->>Task: 调用 task->cleanup(ctx)
        Task-->>API: 清理完成

        API->>Scheduler: 清除槽位数据
        API->>Scheduler: 设置 in_use = false
        API->>Scheduler: 释放上下文

        API->>Scheduler: 解锁
        API-->>Caller: 返回 0
    end
```

### 4.3 任务暂停/恢复

```mermaid
stateDiagram-v2
    [*] --> IDLE: 注册

    IDLE --> PAUSED: pause
    PAUSED --> IDLE: resume

    state IDLE {
        [*] --> Waiting
        Waiting --> Checking: 时间到
        Checking --> Executing: 条件满足
        Checking --> Waiting: 条件不满足
        Executing --> Waiting: 完成
    }

    state PAUSED {
        [*] --> Skipped: 跳过所有检查
    }

    note right of PAUSED: 调度器跳过<br/>此槽位检查
```

---

## 五、内置任务

### 5.1 CCEH 缩容任务

```mermaid
flowchart TB
    subgraph "CCEH 缩容任务"
        INIT[初始化<br/>cche_shrink_init]
        CHECK[检查<br/>cceh_shrink_check]
        EXECUTE[执行<br/>cceh_shrink_execute]
        CLEANUP[清理<br/>cceh_shrink_cleanup]
    end

    subgraph "检查条件"
        LOAD_FACTOR[负载因子 < 0.25]
        GLOBAL_DEPTH[全局深度 > 最小深度]
        SEGMENT_CNT[有可合并的 segment]
    end

    subgraph "执行逻辑"
        FIND_PAIR[查找可合并的 segment 对]
        MERGE[合并相邻 segment]
        UPDATE_DIR[更新目录指针]
        RECYCLE[标记旧 segment 回收]
    end

    INIT --> CHECK
    CHECK --> LOAD_FACTOR
    CHECK --> GLOBAL_DEPTH

    LOAD_FACTOR -->|满足| EXECUTE
    GLOBAL_DEPTH -->|满足| EXECUTE
    LOAD_FACTOR -->|不满足| IDLE[返回 IDLE]
    GLOBAL_DEPTH -->|不满足| IDLE

    EXECUTE --> FIND_PAIR
    FIND_PAIR --> MERGE
    MERGE --> UPDATE_DIR
    UPDATE_DIR --> RECYCLE
    RECYCLE --> DONE[执行完成]

    subgraph "配置参数"
        INTERVAL[检查间隔: 5000ms]
        THRESHOLD[负载因子阈值: 0.25]
        MIN_DEPTH[最小全局深度: 1]
    end
```

### 5.2 统计收集任务

```mermaid
flowchart TB
    subgraph "统计收集任务"
        INIT[初始化<br/>stats_init]
        CHECK[检查<br/>stats_check]
        EXECUTE[执行<br/>stats_execute]
        CLEANUP[清理<br/>stats_cleanup]
    end

    subgraph "统计指标"
        BUF_HIT[缓冲区命中率]
        INDEX_USAGE[索引使用情况]
        QUERY_LATENCY[查询延迟分布]
        MEM_USAGE[内存使用情况]
    end

    subgraph "配置参数"
        INTERVAL[检查间隔: 60000ms]
        CHECK_MODE[检查模式: 总是触发]
    end

    INIT --> CHECK
    CHECK -->|总是返回 1| EXECUTE
    EXECUTE --> BUF_HIT
    EXECUTE --> INDEX_USAGE
    EXECUTE --> QUERY_LATENCY
    EXECUTE --> MEM_USAGE
    BUF_HIT --> DONE
    INDEX_USAGE --> DONE
    QUERY_LATENCY --> DONE
    MEM_USAGE --> DONE
    DONE[执行完成]
```

---

## 六、线程模型

### 6.1 线程交互

```mermaid
flowchart TB
    subgraph "主线程"
        APP[应用程序]
        API_CALL[API 调用<br/>register/unregister/pause/resume]
    end

    subgraph "后台线程"
        SCHEDULER[调度器主循环]
        TASK1[CCEH 缩容任务]
        TASK2[统计收集任务]
    end

    APP -->|db_bgworker_start| SCHEDULER
    APP -->|db_bgworker_stop| SCHEDULER
    API_CALL -->|互斥锁| SCHEDULER
    SCHEDULER -->|检查| TASK1
    SCHEDULER -->|检查| TASK2

    note right of SCHEDULER: 单线程事件循环<br/>无锁内部执行
    note left of API_CALL: 需要加锁<br/>保护槽位访问
```

### 6.2 并发安全

```mermaid
sequenceDiagram
    participant Main as 主线程 (API)
    participant Bg as 后台线程
    participant Mutex as 互斥锁

    Note over Main,Bg: 注册任务

    Main->>Mutex: lock()
    Main->>Bg: 写入槽位
    Main->>Mutex: unlock()

    Note over Main,Bg: 调度器检查

    Bg->>Mutex: lock()
    Bg->>Bg: 读取槽位
    Bg->>Mutex: unlock()

    Bg->>Bg: 执行任务（无锁）

    Note over Main,Bg: 停止调度器

    Main->>Bg: atomic_store(stopping, true)
    Bg->>Bg: 检测 stopping 标志
    Bg->>Bg: 退出循环

    Note over Main,Bg: 等待线程退出

    Main->>Main: pthread_join(thread, 30s 超时)
    Main->>Bg: 清理任务

    Note over Main: 使用 atomic 标志<br/>避免死锁
```

---

## 七、配置与集成

### 7.1 GUC 集成

```mermaid
flowchart LR
    subgraph "GUC 配置参数"
        BGW_ENABLED[bgworker.enabled<br/>bool, 默认 true]
        BGW_TICK_MS[bgworker.tick_ms<br/>int, 默认 1000ms]
    end

    subgraph "配置流程"
        STARTUP[数据库启动]
        CONF[读取 postgresql.conf]
        APPLY[应用配置]
        INIT[初始化调度器]
    end

    STARTUP --> CONF
    CONF --> APPLY
    APPLY --> BGW_ENABLED
    APPLY --> BGW_TICK_MS
    BGW_ENABLED -->|enabled=true| INIT
    BGW_ENABLED -->|enabled=false| SKIP[跳过]
```

### 7.2 启动/停止流程

```mermaid
sequenceDiagram
    participant DB as 数据库初始化
    participant GUC as GUC 配置
    participant BGW as 调度器
    participant Task as 任务系统

    DB->>GUC: 读取配置参数

    DB->>BGW: db_bgworker_start()

    BGW->>BGW: 检查 bgworker.enabled
    alt 未启用
        BGW-->>DB: 直接返回
    else 启用
        BGW->>BGW: 初始化槽位
        BGW->>BGW: 创建后台线程
        BGW->>BGW: 启动主循环
        BGW-->>DB: 启动成功
    end

    DB->>Task: 注册内置任务
    Task->>BGW: db_bgworker_register(&g_cceh_shrink_task)
    Task->>BGW: db_bgworker_register(&g_stats_task)

    Note over DB,Task: 数据库运行中...

    DB->>BGW: db_bgworker_stop()
    BGW->>BGW: 设置 stopping = true
    BGW->>BGW: 等待线程退出 (30s)
    BGW->>BGW: 清理所有任务
    BGW-->>DB: 停止完成
```

---

## 八、性能指标

| 指标 | 目标值 | 说明 |
|------|--------|------|
| 最大任务数 | 16 | 槽位数量 |
| 检查间隔 | 1-60000 ms | 可配置 |
| 线程模型 | 1 后台线程 | 单线程事件循环 |
| 槽位安全 | 互斥锁保护 | 读写分离 |
| 停止超时 | 30 s | 等待任务执行完成 |
| 状态切换 | 原子操作 | atomic_uint |

---

## 九、关键代码位置

| 功能 | 文件 |
|------|------|
| 公共 API | `engine/include/db/bgworker.h` |
| 调度器实现 | `engine/src/db/bgworker/bgworker.c` |
| API 实现 | `engine/src/db/bgworker/bgworker_api.c` |
| 内部数据结构 | `engine/src/db/bgworker/bgworker_internal.h` |
| 任务接口定义 | `engine/src/db/bgworker/tasks/task_iface.h` |
| CCEH 缩容任务 | `engine/src/db/bgworker/tasks/task_cceh_shrink.c` |
| 统计收集任务 | `engine/src/db/bgworker/tasks/task_stats.c` |
| GUC 配置 | `engine/src/db/bgworker/bgworker_config.c` |