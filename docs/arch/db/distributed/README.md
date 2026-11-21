> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# 分布式能力子系统 - 架构设计

## 概述

分布式能力子系统为 db 数据库存储引擎提供水平扩展能力，包括数据分片（Sharding）和 Raft 共识协议两大核心模块。分片模块负责数据的水平拆分与路由，Raft 模块保障分布式环境下的数据一致性与高可用。

---

## 一、子系统架构概览

```mermaid
flowchart TB
    subgraph "分布式能力子系统"
        subgraph "分片模块 (Sharding)"
            SHARD_CONFIG[分片配置<br/>shard_config_t]
            SHARD_ROUTER[分片路由器<br/>shard_router_t]
            SHARD_INFO[分片信息<br/>shard_info_t]
            CROSS_QUERY[跨分片查询<br/>cross_shard_query_t]
            VECTOR_SHARD[向量分片<br/>vector_shard_*]
            REBALANCE[再平衡<br/>shard_rebalance]
        end

        subgraph "Raft 共识模块"
            RAFT_SERVER[Raft 服务器<br/>RaftServer_t]
            RAFT_LOG[Raft 日志<br/>RaftLogEntry_t]
            RAFT_RPC[RPC 处理<br/>RequestVote/AppendEntries]
            RAFT_SNAPSHOT[快照压缩<br/>Snapshot]
            RAFT_CLUSTER[集群管理<br/>RaftCluster_t]
        end
    end

    subgraph "上层调用者"
        SQL_EXEC[SQL 执行器]
        STORAGE[存储引擎]
        VECTOR_INDEX[向量索引]
    end

    SQL_EXEC --> SHARD_ROUTER
    STORAGE --> RAFT_SERVER
    VECTOR_INDEX --> VECTOR_SHARD

    SHARD_CONFIG --> SHARD_ROUTER
    SHARD_ROUTER --> SHARD_INFO
    SHARD_ROUTER --> CROSS_QUERY
    SHARD_ROUTER --> REBALANCE
    SHARD_ROUTER --> VECTOR_SHARD

    RAFT_SERVER --> RAFT_LOG
    RAFT_SERVER --> RAFT_RPC
    RAFT_SERVER --> RAFT_SNAPSHOT
    RAFT_CLUSTER --> RAFT_SERVER
```

---

## 二、分片模块

### 2.1 分片配置与策略

```mermaid
classDiagram
    class shard_strategy_t {
        <<enumeration>>
        SHARD_HASH
        SHARD_RANGE
        SHARD_LIST
    }

    class shard_key_type_t {
        <<enumeration>>
        SHARD_KEY_INT
        SHARD_KEY_STRING
        SHARD_KEY_COMPOSITE
    }

    class shard_config_t {
        +shard_strategy_t strategy
        +shard_key_type_t key_type
        +int num_shards
        +int shard_key_column
        +char* shard_key_name
        +int replication_factor
        +bool consistent_hashing
    }

    class shard_info_t {
        +int shard_id
        +char* shard_name
        +char* host
        +int port
        +int64_t min_value
        +int64_t max_value
        +uint64_t row_count
        +bool is_primary
    }

    class shard_router_t {
        <<opaque>>
        +shard_router_create(config)
        +shard_router_destroy(router)
        +shard_router_add(router, shard)
        +shard_router_remove(router, shard_id)
    }

    shard_config_t --> shard_strategy_t
    shard_config_t --> shard_key_type_t
    shard_router_t --> shard_config_t
    shard_router_t --> shard_info_t
```

### 2.2 分片策略对比

```mermaid
flowchart LR
    subgraph "Hash 分片"
        HASH_KEY[分片键] --> HASH_FUNC[哈希函数<br/>MurmurHash/FNV]
        HASH_FUNC --> HASH_MOD[取模运算<br/>hash % num_shards]
        HASH_MOD --> HASH_SHARD[目标分片]
    end

    subgraph "Range 分片"
        RANGE_KEY[分片键] --> RANGE_CMP[范围比较]
        RANGE_CMP --> RANGE_LOOKUP[查找分片表]
        RANGE_LOOKUP --> RANGE_SHARD[目标分片<br/>min ≤ key < max]
    end

    subgraph "List 分片"
        LIST_KEY[分片键] --> LIST_MAP[映射表查找]
        LIST_MAP --> LIST_SHARD[目标分片<br/>key → shard_id]
    end
```

| 策略 | 适用场景 | 优点 | 缺点 |
|------|---------|------|------|
| **Hash** | 随机访问、均匀分布 | 数据均匀、实现简单 | 范围查询需扫描全部分片 |
| **Range** | 范围查询、时间序列 | 范围查询高效 | 可能数据倾斜 |
| **List** | 地域分布、业务隔离 | 业务语义清晰 | 需要手动管理映射 |

### 2.3 分片路由流程

```mermaid
sequenceDiagram
    participant Client as 客户端
    participant Router as 分片路由器
    participant Hash as 哈希计算
    participant ShardMap as 分片映射表
    participant Shard as 目标分片

    Client->>Router: shard_route(key, key_len)

    Router->>Hash: shard_calculate_hash(key, key_len)
    Hash-->>Router: 返回 hash 值

    alt Hash 分片
        Router->>Router: shard_id = hash % num_shards
    else Range 分片
        Router->>ShardMap: 二分查找范围
        ShardMap-->>Router: 返回 shard_id
    else List 分片
        Router->>ShardMap: 查找映射表
        ShardMap-->>Router: 返回 shard_id
    end

    Router->>ShardMap: shard_get_info(shard_id)
    ShardMap-->>Router: 返回 shard_info_t

    Router-->>Client: 返回 shard_id
```

### 2.4 跨分片查询

```mermaid
flowchart TD
    Start[跨分片查询请求] --> Parse[解析查询语句]
    Parse --> Analyze[分析分片键]
    Analyze --> Determine{涉及分片数}

    Determine -->|单分片| Single[直接路由到目标分片]
    Determine -->|多分片| Multi[创建 cross_shard_query_t]

    Multi --> AddShards[添加涉及的分片 ID]
    AddShards --> Distribute[并行分发查询]
    Distribute --> WaitAll[等待所有分片响应]
    WaitAll --> Merge[合并结果集]
    Merge --> Return[返回合并结果]

    Single --> Return

    subgraph "并行分发"
        Distribute --> S1[分片 1]
        Distribute --> S2[分片 2]
        Distribute --> S3[分片 3]
        S1 --> WaitAll
        S2 --> WaitAll
        S3 --> WaitAll
    end
```

### 2.5 跨分片查询结构

```mermaid
classDiagram
    class cross_shard_query_t {
        +int shard_count
        +int* shard_ids
        +char* query_template
        +void** params
        +int param_count
    }

    cross_shard_query_t --> cross_shard_query_create
    cross_shard_query_t --> cross_shard_query_add_shard
    cross_shard_query_t --> cross_shard_query_destroy

    note for cross_shard_query_t "封装跨分片查询的元信息\n包含目标分片列表和参数"
```

### 2.6 向量分片支持

```mermaid
classDiagram
    class vector_shard_key_t {
        +int64_t vector_id
        +uint64_t hash
    }

    class vector_shard_result_t {
        +int64_t id
        +float distance
        +int shard_id
    }

    vector_shard_key_t --> vector_shard_key_create
    vector_shard_key_t --> vector_consistent_hash

    vector_shard_result_t --> vector_shard_merge_results

    note for vector_shard_key_t "向量分片键\n支持基于向量 ID 或哈希的分片"
    note for vector_shard_result_t "向量搜索结果\n包含向量 ID、距离和来源分片"
```

### 2.7 向量搜索路由与合并

```mermaid
flowchart TD
    Start[向量搜索请求] --> Route[vector_shard_route_search]
    Route --> CalcHash[计算查询向量哈希]
    CalcHash --> SelectShards[选择目标分片]

    SelectShards --> Parallel[并行分发到各分片]
    Parallel --> HNSW1[分片 1 HNSW 搜索]
    Parallel --> HNSW2[分片 2 HNSW 搜索]
    Parallel --> HNSW3[分片 3 HNSW 搜索]

    HNSW1 --> Collect1[收集 top-K 结果]
    HNSW2 --> Collect2[收集 top-K 结果]
    HNSW3 --> Collect3[收集 top-K 结果]

    Collect1 --> Merge[vector_shard_merge_results]
    Collect2 --> Merge
    Collect3 --> Merge

    Merge --> Sort[按距离排序]
    Sort --> TopK[取全局 top-K]
    TopK --> Return[返回合并结果]
```

### 2.8 分片再平衡

```mermaid
flowchart TD
    Start[再平衡检查] --> Check{shard_needs_rebalance}

    Check -->|数据倾斜 > 阈值| Analyze[分析数据分布]
    Check -->|均衡| Skip[跳过]

    Analyze --> CalcMove[计算迁移计划]
    CalcMove --> Plan{迁移方案}

    Plan -->|Hash 重哈希| Rehash[重新计算哈希分布]
    Plan -->|Range 调整| SplitMerge[分裂/合并范围]

    Rehash --> Execute[执行迁移]
    SplitMerge --> Execute

    Execute --> Lock[锁定相关分片]
    Lock --> Migrate[迁移数据]
    Migrate --> Update[更新路由表]
    Update --> Unlock[解锁分片]
    Unlock --> Done[再平衡完成]
```

---

## 三、Raft 共识模块

### 3.1 角色状态机

```mermaid
stateDiagram-v2
    [*] --> Follower: 初始化

    Follower --> Candidate: 选举超时<br/>未收到心跳

    Candidate --> Follower: 收到更高 term<br/>的 RequestVote
    Candidate --> Follower: 收到有效 AppendEntries
    Candidate --> Leader: 获得多数投票

    Leader --> Follower: 收到更高 term<br/>的 RPC
    Leader --> Follower: 选举超时<br/>(step down)

    state Follower {
        [*] --> 等待心跳
        等待心跳 --> 处理日志: AppendEntries
        处理日志 --> 等待心跳
        等待心跳 --> 投票: RequestVote
        投票 --> 等待心跳
    }

    state Candidate {
        [*] --> 发起选举
        发起选举 --> 等待投票
        等待投票 --> 统计票数
        统计票数 --> 发起选举: 未获多数
    }

    state Leader {
        [*] --> 发送心跳
        发送心跳 --> 处理客户端请求
        处理客户端请求 --> 日志复制
        日志复制 --> 推进 commit
        推进 commit --> 发送心跳
    }
```

### 3.2 核心数据结构

```mermaid
classDiagram
    class RaftRole_t {
        <<enumeration>>
        RAFT_ROLE_FOLLOWER
        RAFT_ROLE_CANDIDATE
        RAFT_ROLE_LEADER
    }

    class RaftServerConfig_t {
        +uint64_t node_id
        +uint32_t cluster_size
        +uint32_t heartbeat_interval_ms
        +uint32_t election_timeout_min_ms
        +uint32_t election_timeout_max_ms
    }

    class RaftStateConfig_t {
        +const char* state_path
    }

    class RaftLogEntry_t {
        +uint64_t term
        +uint64_t index
        +void* data
        +size_t data_size
    }

    class RaftServer_t {
        <<opaque>>
        +raft_server_create(cfg)
        +raft_server_create_ex(cfg, state)
        +raft_server_start(srv)
        +raft_server_stop(srv)
        +raft_server_destroy(srv)
        +raft_tick(srv)
        +raft_submit(srv, data, size)
    }

    RaftServer_t --> RaftServerConfig_t
    RaftServer_t --> RaftStateConfig_t
    RaftServer_t --> RaftLogEntry_t
    RaftServer_t --> RaftRole_t
```

### 3.3 Raft 服务器配置

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `node_id` | 必填 | 节点唯一标识符 |
| `cluster_size` | 必填 | 集群节点总数，用于 quorum 计算 |
| `heartbeat_interval_ms` | 150 | Leader 心跳间隔 |
| `election_timeout_min_ms` | 1000 | 选举超时下限 |
| `election_timeout_max_ms` | 2000 | 选举超时上限 |
| `state_path` | NULL | 持久化路径，NULL 表示纯内存模式 |

### 3.4 选举流程

```mermaid
sequenceDiagram
    participant F1 as Follower 1
    participant F2 as Follower 2
    participant F3 as Follower 3
    participant C as Candidate (F1)

    Note over F1,F3: 选举超时，F1 成为 Candidate

    C->>C: term++, 成为 Candidate
    C->>C: 投票给自己

    C->>F2: RequestVote(term, candidate_id, last_log_index, last_log_term)
    C->>F3: RequestVote(term, candidate_id, last_log_index, last_log_term)

    F2->>F2: 检查 term 和日志完整性
    F3->>F3: 检查 term 和日志完整性

    F2-->>C: RequestVoteResult(term, vote_granted=true)
    F3-->>C: RequestVoteResult(term, vote_granted=true)

    C->>C: 统计投票: 3/3 (获得多数)

    C->>C: 成为 Leader

    C->>F2: AppendEntries(term, leader_id, ...)
    C->>F3: AppendEntries(term, leader_id, ...)

    F2-->>C: AppendEntriesResult(term, success=true)
    F3-->>C: AppendEntriesResult(term, success=true)

    Note over C: 确立 Leader 地位
```

### 3.5 日志复制流程

```mermaid
sequenceDiagram
    participant Client as 客户端
    participant Leader as Leader
    participant F1 as Follower 1
    participant F2 as Follower 2
    participant Log as 日志存储

    Client->>Leader: 提交命令 (data)

    Leader->>Log: 追加日志条目 (term, index, data)
    Log-->>Leader: 返回 index

    Note over Leader: 初始化 next_index[], match_index[]

    Leader->>F1: AppendEntries(term, leader_id, prev_log_index, prev_log_term, entries, leader_commit)
    Leader->>F2: AppendEntries(term, leader_id, prev_log_index, prev_log_term, entries, leader_commit)

    F1->>F1: 检查 prev_log_index/term
    F2->>F2: 检查 prev_log_index/term

    F1->>Log: 追加日志条目
    F2->>Log: 追加日志条目

    F1-->>Leader: AppendEntriesResult(term, success=true, match_index)
    F2-->>Leader: AppendEntriesResult(term, success=true, match_index)

    Leader->>Leader: 更新 match_index[]

    Leader->>Leader: 计算 commit_index<br/>(match_index 排序取中位数)

    Note over Leader: commit_index 推进

    Leader->>Leader: 应用已提交日志到状态机

### 3.6 RPC 消息类型

```mermaid
classDiagram
    class RaftRPCType_t {
        <<enumeration>>
        RAFT_RPC_REQUEST_VOTE
        RAFT_RPC_APPEND_ENTRIES
    }

    class RaftRequestVoteArgs_t {
        +uint64_t term
        +uint64_t candidate_id
        +uint64_t last_log_index
        +uint64_t last_log_term
    }

    class RaftRequestVoteResult_t {
        +uint64_t term
        +bool vote_granted
    }

    class RaftAppendEntriesArgs_t {
        +uint64_t term
        +uint64_t leader_id
        +uint64_t prev_log_index
        +uint64_t prev_log_term
        +uint64_t leader_commit
        +const RaftLogEntry_t* entries
        +size_t entry_count
    }

    class RaftAppendEntriesResult_t {
        +uint64_t term
        +bool success
        +uint64_t match_index
    }

    RaftRPCType_t --> RaftRequestVoteArgs_t
    RaftRPCType_t --> RaftAppendEntriesArgs_t
    RaftRequestVoteArgs_t --> RaftRequestVoteResult_t
    RaftAppendEntriesArgs_t --> RaftAppendEntriesResult_t
```

### 3.7 Snapshot 与日志压缩

```mermaid
flowchart TD
    Start[快照触发条件] --> Check{日志条目数<br/>超过阈值}

    Check -->|是| CreateSnapshot[raft_take_snapshot]
    Check -->|否| Skip[跳过]

    CreateSnapshot --> CollectState[收集应用层状态]
    CollectState --> WriteSnapshot[写入快照文件]
    WriteSnapshot --> TruncateLog[截断已提交日志]
    TruncateLog --> UpdateIndex[更新 last_included_index]
    UpdateIndex --> Done[快照完成]

    subgraph "快照文件结构"
        HEADER[文件头<br/>last_included_index<br/>last_included_term]
        DATA[应用层数据<br/>状态机快照]
    end

    WriteSnapshot --> HEADER
    WriteSnapshot --> DATA
```

### 3.8 快照安装流程

```mermaid
sequenceDiagram
    participant Leader as Leader
    participant Follower as Lagging Follower
    participant FS as 文件系统

    Note over Leader,Follower: Follower 日志落后太多<br/>next_index < last_included_index

    Leader->>Leader: raft_get_snapshot()
    Leader->>FS: 读取快照数据
    FS-->>Leader: 返回快照数据

    Leader->>Follower: InstallSnapshot(term, leader_id, snapshot_data, last_included_index, last_included_term)

    Follower->>Follower: 清空当前日志
    Follower->>FS: 写入快照文件
    Follower->>Follower: 更新 last_included_index/term
    Follower->>Follower: 重置 commit_index

    Follower-->>Leader: 成功响应

    Note over Follower: 快照安装完成
```

### 3.9 集群管理

```mermaid
classDiagram
    class RaftCluster_t {
        <<opaque>>
        +uint32_t size
        +uint64_t base_id
        +raft_cluster_create(size, base_id)
        +raft_cluster_destroy(cl)
        +raft_cluster_advance_ms(cl, ms)
        +raft_cluster_force_election(cl)
        +raft_cluster_get_node(cl, node_id)
        +raft_cluster_count_leaders(cl)
        +raft_cluster_get_leader_id(cl)
    }

    class RaftNodeRef_t {
        <<opaque>>
        +节点引用结构
    }

    RaftCluster_t "1" --> "*" RaftNodeRef_t
    RaftNodeRef_t --> RaftServer_t

    note for RaftCluster_t "进程内集群抽象\n用于测试和单机多节点部署"
```

### 3.10 集群时间推进

```mermaid
flowchart TD
    Start[raft_cluster_advance_ms] --> Loop[遍历所有节点]

    Loop --> Tick[raft_tick_advance_ms]
    Tick --> ProcessRPC[处理 RPC 消息]

    ProcessRPC --> PendingMsg{有待发消息?}

    PendingMsg -->|是| Deliver[投递到目标节点]
    PendingMsg -->|否| NextNode{还有节点?}

    Deliver --> NextNode

    NextNode -->|是| Loop
    NextNode -->|否| CommitCheck{Leader 存在?}

    CommitCheck -->|是| TickCommit[raft_cluster_tick_leader_commit]
    CommitCheck -->|否| Done[推进完成]

    TickCommit --> Done
```

---

## 四、持久化与状态管理

### 4.1 状态持久化流程

```mermaid
flowchart TD
    Start[状态变更] --> Check{需要持久化?}

    Check -->|是| Serialize[序列化状态]
    Check -->|否| Skip[跳过]

    Serialize --> WriteBin[写入 bin 文件]
    WriteBin --> Fsync[fsync 刷盘]
    Fsync --> Done[持久化完成]

    subgraph "持久化内容"
        CONTENT1[当前 term]
        CONTENT2[voted_for]
        CONTENT3[日志条目]
        CONTENT4[commit_index]
    end

    Serialize --> CONTENT1
    Serialize --> CONTENT2
    Serialize --> CONTENT3
    Serialize --> CONTENT4
```

### 4.2 启动恢复流程

```mermaid
flowchart TD
    Start[服务器启动] --> CheckFile{状态文件存在?}

    CheckFile -->|是| LoadState[raft_server_load_state]
    CheckFile -->|否| InitState[初始化默认状态]

    LoadState --> ReadHeader[读取文件头]
    ReadHeader --> RestoreTerm[恢复 term]
    RestoreTerm --> RestoreVote[恢复 voted_for]
    RestoreVote --> RestoreLog[恢复日志条目]
    RestoreLog --> Done[恢复完成]

    InitState --> Done

    subgraph "状态文件结构"
        HEADER[魔数 + 版本]
        META[term + voted_for + commit_index]
        LOG_COUNT[日志条目数]
        LOG_ENTRIES[日志条目数组]
    end

    ReadHeader --> HEADER
    HEADER --> META
    META --> LOG_COUNT
    LOG_COUNT --> LOG_ENTRIES
```

### 4.3 状态管理与持久化关系

```mermaid
erDiagram
    RAFT_STATE {
        uint64_t current_term
        uint64_t voted_for
        uint64_t commit_index
        uint64_t last_applied
    }

    RAFT_LOG {
        uint64_t term PK
        uint64_t index PK
        blob data
        size_t data_size
    }

    SNAPSHOT {
        uint64_t last_included_index PK
        uint64_t last_included_term
        blob snapshot_data
        size_t data_size
    }

    RAFT_STATE ||--o{ RAFT_LOG : contains
    RAFT_STATE ||--o| SNAPSHOT : references

    RAFT_LOG {
        note "日志条目按 index 排序"
    }

    SNAPSHOT {
        note "快照包含 last_included_index 之前的状态"
    }
```

---

## 五、分片与 Raft 协作

### 5.1 分片副本管理

```mermaid
flowchart TB
    subgraph "分片 1 (Shard 1)"
        S1_P1[Primary<br/>Leader]
        S1_R1[Replica 1<br/>Follower]
        S1_R2[Replica 2<br/>Follower]
    end

    subgraph "分片 2 (Shard 2)"
        S2_P1[Primary<br/>Leader]
        S2_R1[Replica 1<br/>Follower]
        S2_R2[Replica 2<br/>Follower]
    end

    subgraph "Raft Group 1"
        S1_P1 --> S1_R1
        S1_P1 --> S1_R2
    end

    subgraph "Raft Group 2"
        S2_P1 --> S2_R1
        S2_P1 --> S2_R2
    end

    S1_R1 --> S1_P1: 投票
    S1_R2 --> S1_P1: 投票
    S2_R1 --> S2_P1: 投票
    S2_R2 --> S2_P1: 投票
```

### 5.2 写请求处理流程

```mermaid
sequenceDiagram
    participant Client as 客户端
    participant Router as 分片路由器
    participant Leader as 分片 Leader
    participant Raft as Raft 模块
    participant Follower as 分片 Follower

    Client->>Router: 写请求 (key, value)

    Router->>Router: shard_route(key)
    Router-->>Client: 返回 shard_id

    Client->>Leader: 发送写请求到分片 Leader

    Leader->>Raft: raft_submit(data)

    Raft->>Raft: 追加日志
    Raft->>Follower: AppendEntries RPC

    Follower->>Follower: 追加日志
    Follower-->>Raft: success=true

    Raft->>Raft: 推进 commit_index
    Raft-->>Leader: 返回 index

    Leader->>Leader: 应用到状态机
    Leader-->>Client: 写入成功
```

### 5.3 读请求处理流程

```mermaid
sequenceDiagram
    participant Client as 客户端
    participant Router as 分片路由器
    participant Node as 分片节点

    Client->>Router: 读请求 (key)

    Router->>Router: shard_route(key)
    Router-->>Client: 返回 shard_id

    Client->>Node: 发送读请求到分片节点

    Node->>Node: 检查是否为 Leader

    alt 是 Leader
        Node->>Node: 直接读取
        Node-->>Client: 返回结果
    else 是 Follower
        Node->>Node: 检查租约有效性

        alt 租约有效
            Node->>Node: 读取本地数据
            Node-->>Client: 返回结果
        else 租约过期
            Node-->>Client: 重定向到 Leader
        end
    end
```

---

## 六、关键设计决策

### 6.1 分片策略选择

| 场景 | 推荐策略 | 原因 |
|------|---------|------|
| **OLTP 交易系统** | Hash 分片 | 随机访问、均匀分布 |
| **时序数据** | Range 分片 | 时间范围查询高效 |
| **多租户系统** | List 分片 | 租户隔离、业务语义清晰 |
| **向量搜索** | 一致性哈希 | 减少扩容时的数据迁移 |

### 6.2 Raft 配置建议

| 集群规模 | 心跳间隔 | 选举超时 | 说明 |
|---------|---------|---------|------|
| **3 节点** | 100-150ms | 1-2s | 小集群，快速故障检测 |
| **5 节点** | 150-200ms | 2-3s | 标准配置，平衡性能与稳定性 |
| **7+ 节点** | 200-300ms | 3-5s | 大集群，避免频繁选举 |

### 6.3 性能优化点

```mermaid
flowchart LR
    subgraph "分片优化"
        SHARD_OPT1[预计算路由缓存]
        SHARD_OPT2[异步跨分片查询]
        SHARD_OPT3[批量再平衡]
    end

    subgraph "Raft 优化"
        RAFT_OPT1[批量日志提交]
        RAFT_OPT2[异步日志复制]
        RAFT_OPT3[快照压缩]
    end

    SHARD_OPT1 --> PERF[性能提升]
    SHARD_OPT2 --> PERF
    SHARD_OPT3 --> PERF
    RAFT_OPT1 --> PERF
    RAFT_OPT2 --> PERF
    RAFT_OPT3 --> PERF
```

---

## 七、性能指标

### 7.1 分片模块指标

| 指标 | 目标值 | 说明 |
|------|--------|------|
| 路由计算延迟 | < 1 μs | Hash 分片 |
| 路由计算延迟 | < 10 μs | Range/List 分片 |
| 跨分片查询延迟 | < 100 ms | 3 分片并行 |
| 再平衡数据迁移 | > 100 MB/s | 网络带宽限制 |

### 7.2 Raft 模块指标

| 指标 | 目标值 | 说明 |
|------|--------|------|
| Leader 选举时间 | < 3s | 故障检测 + 选举 |
| 日志提交延迟 | < 10 ms | 多数节点确认 |
| 吞吐量 | > 10,000 ops/s | 小日志条目 |
| 快照创建时间 | < 1s | 1GB 状态机 |

---

## 八、限制与未实现功能

| 功能 | 状态 | 说明 |
|------|------|------|
| 分布式事务 | ❌ 未实现 | dist_txn.h 未开发 |
| 协调节点 | ❌ 未实现 | coordinator.h 未开发 |
| PreVote | ❌ 未实现 | 简化版 Raft |
| Joint Consensus | ❌ 未实现 | 配置变更简化 |
| 日志持久化 | ✅ 已实现 | raft_server_save/load_state |
| Snapshot | ✅ 已实现 | raft_take/install/get_snapshot |
| 集群管理 | ✅ 已实现 | RaftCluster_t 进程内集群 |

---

## 九、关键代码位置

| 功能 | 头文件 | 源文件 |
|------|--------|--------|
| 分片策略与配置 | `engine/include/db/sharding/sharding.h` | `engine/src/db/sharding/` |
| Raft 服务器 | `engine/include/db/consensus/raft.h` | `engine/src/db/consensus/raft.c` |
| Raft 集群 | `engine/include/db/consensus/raft_cluster.h` | `engine/src/db/consensus/raft_cluster.c` |
| 分片测试 | - | `engine/test/db/sharding/` |
| Raft 测试 | - | `engine/test/db/consensus/` |

---

## 十、相关文档

- [存储核心子系统](../storage/README.md) - 底层存储支持
- [事务与并发子系统](../transaction/README.md) - 本地事务支持
- [索引系统](../index/README.md) - 向量索引支持
- [架构概览](../README.md) - 整体架构
