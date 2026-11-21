> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# db 数据库存储引擎 - 物理视图

## 概述

本文档描述 db 数据库存储引擎的物理视图，展示系统的部署拓扑、运行环境和资源分配。

---

## 一、部署拓扑架构

### 1.1 单节点部署

```mermaid
flowchart TB
    subgraph "客户端层"
        CLI[命令行客户端<br/>vdb_cli]
        APP[应用程序<br/>使用 db_driver]
        WEB[Web 应用<br/>HTTP API]
    end

    subgraph "数据库服务器节点"
        subgraph "内存区域"
            BUF_POOL[Buffer Pool<br/>shared_buffers: 128MB]
            WAL_BUF[WAL Buffer<br/>16MB]
            WORK_MEM[工作内存<br/>work_mem: 4MB]
        end

        subgraph "进程模型"
            MAIN[主进程<br/>db_server]
            BG_WRITER[后台写进程]
            BG_WORKER[后台工作进程]
            WAL_WRITER[WAL 写进程]
        end

        subgraph "数据存储"
            DATA_DIR[数据目录<br/>data/]
            WAL_FILES[WAL 文件<br/>data/pg_wal/]
            LOG_FILES[日志文件<br/>data/log/]
        end
    end

    CLI -->|Wire 协议| MAIN
    APP -->|db_driver| MAIN
    WEB -->|HTTP/REST| MAIN

    MAIN --> BUF_POOL
    MAIN --> WAL_BUF
    BG_WRITER --> BUF_POOL
    WAL_WRITER --> WAL_BUF
    BG_WORKER --> BUF_POOL

    BUF_POOL --> DATA_DIR
    WAL_BUF --> WAL_FILES
    MAIN --> LOG_FILES
```

### 1.2 分布式部署架构

```mermaid
flowchart TB
    subgraph "客户端层"
        CLIENTS[客户端应用]
    end

    subgraph "协调节点层"
        COORD1[协调节点1<br/>Coordinator]
        COORD2[协调节点2<br/>Coordinator]
    end

    subgraph "数据节点层"
        subgraph "分片1"
            SHARD1_PRI[主节点<br/>Primary]
            SHARD1_REP1[从节点1<br/>Replica]
            SHARD1_REP2[从节点2<br/>Replica]
        end

        subgraph "分片2"
            SHARD2_PRI[主节点<br/>Primary]
            SHARD2_REP1[从节点1<br/>Replica]
            SHARD2_REP2[从节点2<br/>Replica]
        end

        subgraph "分片3"
            SHARD3_PRI[主节点<br/>Primary]
            SHARD3_REP1[从节点1<br/>Replica]
            SHARD3_REP2[从节点2<br/>Replica]
        end
    end

    CLIENTS -->|负载均衡| COORD1
    CLIENTS -->|负载均衡| COORD2

    COORD1 -->|路由查询| SHARD1_PRI
    COORD1 -->|路由查询| SHARD2_PRI
    COORD1 -->|路由查询| SHARD3_PRI

    COORD2 -->|路由查询| SHARD1_PRI
    COORD2 -->|路由查询| SHARD2_PRI
    COORD2 -->|路由查询| SHARD3_PRI

    SHARD1_PRI -.->|Raft 复制| SHARD1_REP1
    SHARD1_PRI -.->|Raft 复制| SHARD1_REP2

    SHARD2_PRI -.->|Raft 复制| SHARD2_REP1
    SHARD2_PRI -.->|Raft 复制| SHARD2_REP2

    SHARD3_PRI -.->|Raft 复制| SHARD3_REP1
    SHARD3_PRI -.->|Raft 复制| SHARD3_REP2
```

---

## 二、进程/线程模型

### 2.1 单节点进程架构

```mermaid
flowchart TB
    subgraph "主进程"
        MAIN_PROC[主进程 PID<br/>监听端口 5432]

        subgraph "工作线程池"
            WORKER1[工作线程1<br/>处理客户端连接]
            WORKER2[工作线程2<br/>处理客户端连接]
            WORKERN[工作线程N<br/>处理客户端连接]
        end

        subgraph "后台进程"
            BG_WRITER[后台写进程<br/>刷写脏页]
            WAL_WRITER[WAL 写进程<br/>刷写 WAL]
            CHECKPOINTER[检查点进程<br/>执行检查点]
            STATS[统计收集进程<br/>收集运行时指标]
        end
    end

    subgraph "系统资源"
        CPU[CPU 核心]
        MEM[内存]
        DISK[磁盘]
    end

    MAIN_PROC --> WORKER1
    MAIN_PROC --> WORKER2
    MAIN_PROC --> WORKERN

    MAIN_PROC --> BG_WRITER
    MAIN_PROC --> WAL_WRITER
    MAIN_PROC --> CHECKPOINTER
    MAIN_PROC --> STATS

    WORKER1 --> CPU
    WORKER2 --> CPU
    WORKERN --> CPU
    BG_WRITER --> CPU

    WORKER1 --> MEM
    BG_WRITER --> MEM

    BG_WRITER --> DISK
    WAL_WRITER --> DISK
```

### 2.2 连接处理模型

```mermaid
flowchart LR
    subgraph "客户端连接"
        CONN1[连接1]
        CONN2[连接2]
        CONN3[连接3]
        CONNN[连接N]
    end

    subgraph "服务器端"
        LISTENER[监听器<br/>端口 5432]
        DISPATCHER[连接分发器]

        subgraph "线程池"
            THREAD1[线程1]
            THREAD2[线程2]
            THREAD3[线程3]
        end

        subgraph "会话管理"
            SESSION1[会话1<br/>用户A]
            SESSION2[会话2<br/>用户B]
            SESSION3[会话3<br/>用户C]
        end
    end

    CONN1 -->|TCP/IP| LISTENER
    CONN2 -->|TCP/IP| LISTENER
    CONN3 -->|TCP/IP| LISTENER
    CONNN -->|TCP/IP| LISTENER

    LISTENER --> DISPATCHER
    DISPATCHER --> THREAD1
    DISPATCHER --> THREAD2
    DISPATCHER --> THREAD3

    THREAD1 --> SESSION1
    THREAD2 --> SESSION2
    THREAD3 --> SESSION3
```

---

## 三、内存布局

### 3.1 内存区域划分

```mermaid
flowchart TB
    subgraph "进程内存空间"
        subgraph "共享内存区"
            SHARED_BUF[Buffer Pool<br/>shared_buffers: 128MB]
            SHARED_WAL[WAL Buffer<br/>16MB]
            SHARED_LOCK[锁表<br/>约 1MB]
            SHARED_CATALOG[Catalog 缓存<br/>约 2MB]
        end

        subgraph "每个连接私有内存"
            WORK_MEM[工作内存<br/>work_mem: 4MB<br/>排序/哈希]
            MAINT_MEM[维护内存<br/>maintenance_work_mem: 64MB<br/>VACUUM/INDEX]
            TEMP_MEM[临时内存<br/>temp_buffers: 8MB]
        end

        subgraph "代码段"
            CODE[可执行代码]
            RODATA[只读数据]
        end
    end

    subgraph "内存管理"
        ALLOCATOR[内存分配器<br/>malloc/free]
    end

    SHARED_BUF --> ALLOCATOR
    WORK_MEM --> ALLOCATOR
```

### 3.2 Buffer Pool 内存结构

```mermaid
flowchart TB
    subgraph "Buffer Pool 结构"
        BUFFER_ARRAY[Buffer 数组<br/>buffer_t buffers NUM_BUFFERS]

        subgraph "单个 Buffer"
            HEADER[Buffer Header<br/>page_id, ref_count, is_dirty]
            PAGE[Page Data<br/>8KB 页面数据]
        end

        subgraph "辅助结构"
            HASH_TABLE[Hash 表<br/>page_id -> buffer_id]
            CLOCK[Clock 指针<br/>用于 Clock-Sweep]
            FREE_LIST[空闲列表<br/>可用 Buffer]
        end
    end

    BUFFER_ARRAY --> HEADER
    HEADER --> PAGE

    HASH_TABLE --> BUFFER_ARRAY
    CLOCK --> BUFFER_ARRAY
    FREE_LIST --> BUFFER_ARRAY
```

---

## 四、磁盘存储布局

### 4.1 数据目录结构

```
data/                           # 数据目录
├── postgresql.conf             # 主配置文件
├── pg_hba.conf                 # 访问控制配置
├── postmaster.pid              # 主进程 PID 文件
├── postmaster.opts             # 启动参数
│
├── base/                       # 数据库文件
│   ├── 1/                      # 数据库 OID=1 (template1)
│   │   ├── 1259                # pg_class 表文件
│   │   ├── 1259_fsm            # pg_class FSM 文件
│   │   ├── 1259_vm             # pg_class VM 文件
│   │   └── ...
│   ├── 13067/                  # 用户数据库
│   │   ├── 16384               # 用户表文件
│   │   ├── 16384_fsm
│   │   ├── 16384_vm
│   │   ├── 16385               # 索引文件
│   │   └── ...
│   └── ...
│
├── global/                     # 全局数据
│   ├── pg_control              # 控制文件
│   ├── pg_filenode.map         # 文件节点映射
│   └── ...
│
├── pg_wal/                     # WAL 日志
│   ├── 000000010000000000000001    # WAL 段文件
│   ├── 000000010000000000000002
│   ├── archive_status/         # 归档状态
│   └── ...
│
├── pg_xact/                    # 事务提交日志
│   ├── 0000
│   └── ...
│
├── log/                        # 服务器日志
│   ├── postgresql-2026-07-16.log
│   └── ...
│
└── pg_stat/                    # 统计数据
    ├── pgstat.stat
    └── ...
```

### 4.2 文件存储格式

```mermaid
flowchart TB
    subgraph "表文件结构"
        TABLE_FILE[表文件<br/>OID 文件号]

        subgraph "页面布局"
            PAGE0[页面0<br/>8KB]
            PAGE1[页面1<br/>8KB]
            PAGEn[页面N<br/>8KB]
        end

        subgraph "页面内部结构"
            HEADER[页头<br/>24 字节]
            ITEM_PTR[项指针数组<br/>变长]
            FREE_SPACE[空闲空间]
            TUPLES[元组数据<br/>从后向前]
            SPECIAL[特殊空间<br/>索引专用]
        end
    end

    TABLE_FILE --> PAGE0
    TABLE_FILE --> PAGE1
    TABLE_FILE --> PAGEn

    PAGE0 --> HEADER
    HEADER --> ITEM_PTR
    ITEM_PTR --> FREE_SPACE
    FREE_SPACE --> TUPLES
    TUPLES --> SPECIAL
```

### 4.3 WAL 文件格式

```mermaid
flowchart TB
    subgraph "WAL 段文件"
        WAL_FILE[WAL 段文件<br/>16MB]

        subgraph "WAL 页面"
            WAL_PAGE0[WAL 页面 0<br/>8KB]
            WAL_PAGE1[WAL 页面 1<br/>8KB]
            WAL_PAGEn[WAL 页面 N<br/>8KB]
        end

        subgraph "WAL 页面结构"
            PAGE_HDR[页面头<br/>XLogPageHeader]
            RECORD0[WAL 记录 0]
            RECORD1[WAL 记录 1]
            RECORDN[WAL 记录 N]
        end

        subgraph "WAL 记录结构"
            REC_HDR[记录头<br/>rmid, info, tot_len]
            BLOCK_HDR[块头<br/>表空间/数据库/关系]
            DATA[数据<br/>实际修改]
        end
    end

    WAL_FILE --> WAL_PAGE0
    WAL_FILE --> WAL_PAGE1
    WAL_FILE --> WAL_PAGEn

    WAL_PAGE0 --> PAGE_HDR
    PAGE_HDR --> RECORD0
    RECORD0 --> RECORD1
    RECORD1 --> RECORDN

    RECORD0 --> REC_HDR
    REC_HDR --> BLOCK_HDR
    BLOCK_HDR --> DATA
```

---

## 五、网络通信

### 5.1 Wire 协议架构

```mermaid
flowchart LR
    subgraph "客户端"
        APP[应用程序]
        DRIVER[db_driver]
    end

    subgraph "协议层"
        TCP[TCP 连接<br/>端口 5432]

        subgraph "消息类型"
            STARTUP[StartupMessage<br/>协议握手]
            QUERY[Query<br/>SQL 查询]
            BIND[Bind<br/>参数绑定]
            EXECUTE[Execute<br/>执行语句]
            SYNC[Sync<br/>同步点]
            CLOSE[Close<br/>关闭语句]
            TERMINATE[Terminate<br/>关闭连接]
        end
    end

    subgraph "服务器"
        SERVER[db_server]
    end

    APP --> DRIVER
    DRIVER --> TCP
    TCP --> STARTUP
    TCP --> QUERY
    TCP --> BIND
    TCP --> EXECUTE
    TCP --> SYNC
    TCP --> CLOSE
    TCP --> TERMINATE

    STARTUP --> SERVER
    QUERY --> SERVER
    BIND --> SERVER
    EXECUTE --> SERVER
    SYNC --> SERVER
    CLOSE --> SERVER
    TERMINATE --> SERVER
```

### 5.2 消息流程示例

```mermaid
sequenceDiagram
    participant Client as 客户端
    participant Server as 服务器

    Note over Client,Server: 连接建立阶段
    Client->>Server: StartupMessage(version=3.0, user, database)
    Server->>Client: AuthenticationOk
    Server->>Client: ParameterStatus(server_version, ...)
    Server->>Client: BackendKeyData(PID, key)
    Server->>Client: ReadyForQuery(I)

    Note over Client,Server: 简单查询阶段
    Client->>Server: Query("SELECT * FROM table")
    Server->>Client: RowDescription(列定义)
    Server->>Client: DataRow(行数据)
    Server->>Client: DataRow(行数据)
    Server->>Client: CommandComplete(SELECT 2)
    Server->>Client: ReadyForQuery(I)

    Note over Client,Server: 关闭连接阶段
    Client->>Server: Terminate
    Server->>Client: 关闭 TCP 连接
```

---

## 六、资源配置

### 6.1 GUC 配置参数

| 参数类别 | 参数名 | 默认值 | 说明 |
|----------|--------|--------|------|
| **连接** | `port` | 5432 | 监听端口 |
| | `listen_addresses` | '*' | 监听地址 |
| | `max_connections` | 100 | 最大连接数 |
| **内存** | `shared_buffers` | 128MB | Buffer Pool 大小 |
| | `work_mem` | 4MB | 排序/哈希内存 |
| | `maintenance_work_mem` | 64MB | 维护操作内存 |
| | `wal_buffers` | 16MB | WAL Buffer 大小 |
| **WAL** | `wal_level` | replica | WAL 级别 |
| | `fsync` | on | 强制刷写 |
| | `synchronous_commit` | on | 同步提交 |
| | `checkpoint_timeout` | 5min | 检查点间隔 |
| **日志** | `log_destination` | stderr | 日志目标 |
| | `logging_collector` | off | 日志收集器 |
| | `log_level` | info | 日志级别 |

### 6.2 资源限制

```mermaid
flowchart TB
    subgraph "连接限制"
        MAX_CONN[max_connections: 100]
        MAX_POOL[连接池: 可配置]
    end

    subgraph "内存限制"
        SHARED_LIMIT[shared_buffers: 128MB]
        WORK_LIMIT[work_mem: 4MB × 连接数]
        TOTAL_MEM[总内存: < 系统内存 75%]
    end

    subgraph "磁盘限制"
        MAX_WAL[WAL 空间: 自动管理]
        MAX_DATA[数据空间: 自动扩展]
        MIN_FREE[最小剩余: 10%]
    end

    subgraph "CPU 限制"
        MAX_WORKERS[工作线程: CPU 核心数]
        BG_PROCS[后台进程: 可配置]
    end
```

---

## 七、容灾与高可用

### 7.1 主从复制架构

```mermaid
flowchart LR
    subgraph "主节点"
        PRIMARY[主节点<br/>读写服务]
        WAL_SENDER[WAL 发送器]
    end

    subgraph "从节点1"
        STANDBY1[从节点1<br/>只读服务]
        WAL_RECEIVER1[WAL 接收器]
        REPLAY1[WAL 回放]
    end

    subgraph "从节点2"
        STANDBY2[从节点2<br/>只读服务]
        WAL_RECEIVER2[WAL 接收器]
        REPLAY2[WAL 回放]
    end

    PRIMARY -->|生成 WAL| WAL_SENDER
    WAL_SENDER -.->|流复制| WAL_RECEIVER1
    WAL_SENDER -.->|流复制| WAL_RECEIVER2

    WAL_RECEIVER1 --> REPLAY1
    WAL_RECEIVER2 --> REPLAY2

    REPLAY1 --> STANDBY1
    REPLAY2 --> STANDBY2
```

### 7.2 故障切换流程

```mermaid
stateDiagram-v2
    [*] --> 正常运行
    正常运行 --> 主节点故障: 检测到主节点失效
    主节点故障 --> 选举新主: Raft 选举
    选举新主 --> 数据同步: 新主开始服务
    数据同步 --> 正常运行: 所有从节点同步完成

    正常运行 --> 网络分区: 网络故障
    网络分区 --> 分区恢复: 网络恢复
    分区恢复 --> 数据同步
```

---

## 八、监控与诊断

### 8.1 监控指标

```mermaid
mindmap
  root((监控指标))
    性能指标
      QPS 每秒查询数
      延迟 P50/P95/P99
      吞吐量 MB/s
    资源指标
      Buffer Pool 命中率
      内存使用量
      磁盘 I/O
      CPU 使用率
    连接指标
      活跃连接数
      空闲连接数
      等待队列长度
    事务指标
      事务提交率
      事务回滚率
      死锁次数
    WAL 指标
      WAL 生成速率
      WAL 刷写延迟
      检查点频率
```

### 8.2 诊断工具

```mermaid
flowchart LR
    subgraph "内置工具"
        PG_CTL[pg_ctl<br/>服务控制]
        INITDB[initdb<br/>初始化]
        STATS[统计视图<br/>pg_stat_*]
    end

    subgraph "日志分析"
        SLOW_LOG[慢查询日志]
        ERROR_LOG[错误日志]
        AUDIT_LOG[审计日志]
    end

    subgraph "外部工具"
        PROMETHEUS[Prometheus<br/>指标采集]
        GRAFANA[Grafana<br/>可视化]
        ALERT[AlertManager<br/>告警]
    end

    PG_CTL --> SLOW_LOG
    STATS --> PROMETHEUS
    PROMETHEUS --> GRAFANA
    GRAFANA --> ALERT
```

---

## 九、安全配置

### 9.1 访问控制

```mermaid
flowchart TB
    subgraph "认证层"
        HOST_BASED[pg_hba.conf<br/>基于主机的访问控制]
        PASSWORD[密码认证<br/>MD5/SCRAM]
        CERT[证书认证<br/>SSL/TLS]
    end

    subgraph "授权层"
        ROLE[角色管理<br/>CREATE ROLE]
        PERM[权限控制<br/>GRANT/REVOKE]
        RLS[行级安全<br/>Row Level Security]
    end

    subgraph "加密层"
        SSL_CONN[SSL 连接加密]
        DATA_ENC[数据加密<br/>透明加密]
        WAL_ENC[WAL 加密]
    end

    HOST_BASED --> PASSWORD
    HOST_BASED --> CERT

    ROLE --> PERM
    PERM --> RLS

    SSL_CONN --> DATA_ENC
    DATA_ENC --> WAL_ENC
```

### 9.2 pg_hba.conf 示例

```
# TYPE  DATABASE        USER            ADDRESS                 METHOD
local   all             all                                     trust
host    all             all             127.0.0.1/32            md5
host    all             all             ::1/128                 md5
hostssl all             all             0.0.0.0/0               scram-sha-256
```

---

## 十、性能调优参数

### 10.1 内存调优

| 参数 | 推荐值 | 调优说明 |
|------|--------|----------|
| `shared_buffers` | 系统内存 25% | 不超过系统内存 40% |
| `work_mem` | 4MB - 64MB | 根据并发查询数调整 |
| `maintenance_work_mem` | 512MB - 1GB | VACUUM/INDEX 创建时使用 |
| `wal_buffers` | 16MB - 64MB | 大批量写入时增加 |
| `effective_cache_size` | 系统内存 75% | 查询优化器参考值 |

### 10.2 WAL 调优

| 参数 | 推荐值 | 调优说明 |
|------|--------|----------|
| `wal_level` | replica | 主从复制时使用 |
| `synchronous_commit` | on | 高可靠性场景 |
| | off | 高性能场景，可能丢事务 |
| `wal_compression` | on | 压缩 WAL，节省空间 |
| `checkpoint_completion_target` | 0.9 | 平滑刷写脏页 |

### 10.3 并发调优

| 参数 | 推荐值 | 调优说明 |
|------|--------|----------|
| `max_connections` | 100 - 200 | 根据应用需求 |
| `max_worker_processes` | CPU 核心数 | 并行查询使用 |
| `max_parallel_workers_per_gather` | 2 - 4 | 单个查询并行度 |
| `max_parallel_workers` | CPU 核心数 | 总并行工作线程 |

---

## 十一、运行环境要求

### 11.1 操作系统支持

| 系统 | 版本 | 说明 |
|------|------|------|
| **Linux** | Ubuntu 20.04+, CentOS 7+ | 推荐生产环境 |
| **Windows** | Windows 10/11, Server 2016+ | 开发测试环境 |
| **macOS** | 10.15+ | 开发测试环境 |

### 11.2 硬件要求

| 资源 | 最低配置 | 推荐配置 |
|------|----------|----------|
| **CPU** | 2 核 | 8+ 核 |
| **内存** | 4 GB | 16+ GB |
| **磁盘** | 20 GB | SSD 100+ GB |
| **网络** | 100 Mbps | 1 Gbps |

### 11.3 编译依赖

| 依赖 | 版本 | 说明 |
|------|------|------|
| **CMake** | 3.20+ | 构建系统 |
| **GCC** | 9.0+ | Linux 编译器 |
| **MSVC** | 2019+ | Windows 编译器 |
| **Clang** | 10.0+ | macOS 编译器 |
| **GoogleTest** | vendored | 测试框架 |

---

## 十二、关键代码位置

| 功能 | 源文件 |
|------|--------|
| 主服务器 | `engine/src/db/core/db_server.c` |
| pg_ctl 控制 | `engine/src/db/core/pg_ctl.c` |
| initdb 初始化 | `engine/src/db/core/initdb.c` |
| GUC 配置 | `engine/src/db/bgworker/guc.c` |
| Wire 协议 | `engine/include/db/sql/pgwire.h` |
| WAL 写入 | `engine/src/db/storage/wal/` |
| 检查点 | `engine/src/db/storage/wal/checkpoint.c` |
| 流复制 | `engine/src/db/replication/` |
| Raft 共识 | `engine/src/db/consensus/raft.c` |
