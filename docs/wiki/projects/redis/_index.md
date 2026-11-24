---
project: redis
page_type: overview
display_name: Redis
repo: https://github.com/redis/redis.git
license: BSD-3-Clause
language: C
stars: 77713
stars_as_of: 2026-11-23
refs:
  - path: src/server.c
    symbols: [main]
  - path: src/ae.c
    symbols: [aeMain, aeProcessEvents]
  - path: src/networking.c
    symbols: [readQueryFromClient, processInputBuffer]
  - path: src/db.c
    symbols: [lookupKey, dbAdd, dbGenericDelete]
  - path: src/object.c
    symbols: [createObject, tryObjectEncoding]
  - path: src/rdb.c
    symbols: [rdbSave, rdbLoad]
  - path: src/aof.c
    symbols: [feedAppendOnlyFile, flushAppendOnlyFile]
  - path: src/cluster_legacy.c
    symbols: [clusterInit, clusterProcessPacket]
---

# Redis

## 一句话职责

Redis 是以内存为主要存储介质的键值数据库，兼具数据结构服务器、缓存与消息中间件三种角色。

## 机制是什么

Redis 以单线程事件循环承载全部命令执行，自带的 ae 库把多路复用抽象成文件事件与时间事件两类回调。键空间由哈希表索引，值的编码随规模在紧凑与展开形态之间切换；过期既有惰性判定也有主动扫描，内存压力下按采样近似执行淘汰。持久化有 RDB 快照与 AOF 追加两条路径，复制基于命令流传播，集群与哨兵在此之上组织高可用。

本文库按经审阅的机制单元组织：**13 个机制领域、共 63 篇机制页**，每个机制页对应一个可独立理解、可独立验证的机制单元，而不是一个源文件。

## 怎么实现的

下表的 13 个领域按下面的顺序排列：A 事件循环与命令分派 → B 网络与客户端 → C 键空间与过期 → D 对象与编码 → E 底层数据结构 → F 数据类型 → G 持久化 → H 复制与高可用 → I 阻塞与通知 → J 事务脚本与模块 → K 安全 → L 特殊数据类型 → M 启动运维与后台任务。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

## 机制导航（63 篇 / 13 组）

### A-事件循环与命令分派（6 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-ae-event-loop.md`](a01-ae-event-loop.md) | 事件循环与文件事件分派 |
| [`a02-beforesleep-cycle.md`](a02-beforesleep-cycle.md) | 每轮事件循环前后的固定动作 |
| [`a03-servercron-timers.md`](a03-servercron-timers.md) | serverCron 定时任务 |
| [`a04-command-table.md`](a04-command-table.md) | 命令表与命令元数据 |
| [`a05-command-dispatch-call.md`](a05-command-dispatch-call.md) | 命令执行与传播登记 |
| [`a06-client-input-parsing.md`](a06-client-input-parsing.md) | 客户端输入解析 |

### B-网络与客户端（6 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-client-lifecycle.md`](b01-client-lifecycle.md) | 客户端对象生命周期 |
| [`b02-reply-buffers.md`](b02-reply-buffers.md) | 回复编码与缓冲 |
| [`b03-output-buffer-limits.md`](b03-output-buffer-limits.md) | 输出缓冲上限与超限处置 |
| [`b04-socket-writeback.md`](b04-socket-writeback.md) | 回复写回 socket |
| [`b05-clients-cron-maintenance.md`](b05-clients-cron-maintenance.md) | 客户端定期维护 |
| [`b06-client-eviction.md`](b06-client-eviction.md) | 客户端按内存驱逐 |

### C-键空间与过期（6 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-keyspace-read-lookup.md`](c01-keyspace-read-lookup.md) | 键的读侧查找 |
| [`c02-keyspace-write-delete.md`](c02-keyspace-write-delete.md) | 键的写侧插入与删除 |
| [`c03-expiration-metadata.md`](c03-expiration-metadata.md) | 过期时间元数据 |
| [`c04-active-expire.md`](c04-active-expire.md) | 主动过期扫描 |
| [`c05-memory-eviction.md`](c05-memory-eviction.md) | 内存淘汰 |
| [`c06-keyspace-scan.md`](c06-keyspace-scan.md) | 键空间游标扫描 |

### D-对象与编码（5 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-robj-refcount.md`](d01-robj-refcount.md) | 对象与引用计数 |
| [`d02-object-encoding-transition.md`](d02-object-encoding-transition.md) | 对象编码与转换 |
| [`d03-object-free-by-type.md`](d03-object-free-by-type.md) | 按类型释放值对象 |
| [`d04-intset-encoding.md`](d04-intset-encoding.md) | 整数集合编码 |
| [`d05-ziplist-legacy-encoding.md`](d05-ziplist-legacy-encoding.md) | 旧版紧凑列表编码 |

### E-底层数据结构（7 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-dict-hashtable.md`](e01-dict-hashtable.md) | 哈希表与渐进 rehash |
| [`e02-dict-scan-iteration.md`](e02-dict-scan-iteration.md) | 哈希表的反向遍历与安全游标 |
| [`e03-sds-string.md`](e03-sds-string.md) | 动态字符串 |
| [`e04-rax-radix-tree.md`](e04-rax-radix-tree.md) | 压缩前缀树 |
| [`e05-listpack.md`](e05-listpack.md) | 紧凑列表 listpack |
| [`e06-quicklist.md`](e06-quicklist.md) | quicklist 分片链表 |
| [`e07-memory-allocator.md`](e07-memory-allocator.md) | 内存分配与用量统计 |

### F-数据类型（8 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-string-type.md`](f01-string-type.md) | 字符串类型 |
| [`f02-list-type.md`](f02-list-type.md) | 列表类型 |
| [`f03-hash-type.md`](f03-hash-type.md) | 哈希类型 |
| [`f04-set-type.md`](f04-set-type.md) | 集合类型 |
| [`f05-zset-type.md`](f05-zset-type.md) | 有序集合类型 |
| [`f06-skiplist.md`](f06-skiplist.md) | 跳表 |
| [`f07-stream-type.md`](f07-stream-type.md) | 流类型 |
| [`f08-stream-consumer-groups.md`](f08-stream-consumer-groups.md) | 流的消费者组 |

### G-持久化（2 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-rdb-snapshot.md`](g01-rdb-snapshot.md) | RDB 快照格式与落盘 |
| [`g02-aof-append.md`](g02-aof-append.md) | AOF 追加日志 |

### H-复制与高可用（3 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-replication-feed.md`](h01-replication-feed.md) | 主从复制传播 |
| [`h02-cluster-bus.md`](h02-cluster-bus.md) | 集群总线与槽位 |
| [`h03-sentinel.md`](h03-sentinel.md) | 哨兵监控与故障转移 |

### I-阻塞与通知（2 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-blocked-clients.md`](i01-blocked-clients.md) | 阻塞客户端与唤醒 |
| [`i02-keyspace-notification.md`](i02-keyspace-notification.md) | 键空间通知 |

### J-事务脚本与模块（6 篇）

| 页文件 | 标题 |
|---|---|
| [`j01-multi-exec.md`](j01-multi-exec.md) | 事务与命令排队 |
| [`j02-pubsub.md`](j02-pubsub.md) | 发布订阅通道 |
| [`j03-scripting-eval.md`](j03-scripting-eval.md) | 脚本执行 |
| [`j04-functions-library.md`](j04-functions-library.md) | 函数库 |
| [`j05-module-api.md`](j05-module-api.md) | 模块系统 |
| [`j06-config-runtime.md`](j06-config-runtime.md) | 运行时配置与配置重写 |

### K-安全（2 篇）

| 页文件 | 标题 |
|---|---|
| [`k01-acl.md`](k01-acl.md) | 访问控制列表 |
| [`k02-tls-transport.md`](k02-tls-transport.md) | TLS 传输 |

### L-特殊数据类型（5 篇）

| 页文件 | 标题 |
|---|---|
| [`l01-bitops.md`](l01-bitops.md) | 位与位域操作 |
| [`l02-hyperloglog.md`](l02-hyperloglog.md) | 基数估算 |
| [`l03-geo-index.md`](l03-geo-index.md) | 地理索引与半径查询 |
| [`l04-sort-command.md`](l04-sort-command.md) | SORT 命令 |
| [`l05-active-defrag.md`](l05-active-defrag.md) | 主动碎片整理 |

### M-启动运维与后台任务（5 篇）

| 页文件 | 标题 |
|---|---|
| [`m01-server-bootstrap.md`](m01-server-bootstrap.md) | 服务端启动装配 |
| [`m02-bio-background-jobs.md`](m02-bio-background-jobs.md) | 后台 I/O 线程任务 |
| [`m03-shutdown-sequence.md`](m03-shutdown-sequence.md) | 关停流程 |
| [`m04-slowlog.md`](m04-slowlog.md) | 慢查询日志 |
| [`m05-latency-monitor.md`](m05-latency-monitor.md) | 延迟监控与命令重写 |

## 关键取舍

Redis 用单线程命令执行换取无锁的数据结构访问，代价是单个慢命令会阻塞整个实例，因此耗时操作被移到后台线程或交给 Lua 脚本在受限环境内完成。内存优先的存储模型换来低延迟，代价是必须同时提供持久化与淘汰策略来应对内存上限。

文档层面沿用与 PostgreSQL 相同的取舍：**机制粒度优先于文件粒度**，每一页对应一个可独立验证的机制单元，而不是一个源文件。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `src/server.c` | main | 服务端启动与主循环 |
| `src/ae.c` | aeMain, aeProcessEvents | 事件循环 |
| `src/networking.c` | readQueryFromClient, processInputBuffer | 客户端命令读取与解析 |
| `src/db.c` | lookupKey, dbAdd, dbGenericDelete | 键空间读写与删除 |
| `src/object.c` | createObject, tryObjectEncoding | 对象创建与编码转换 |
| `src/rdb.c` | rdbSave, rdbLoad | RDB 快照生成与载入 |
| `src/aof.c` | feedAppendOnlyFile, flushAppendOnlyFile | AOF 追加与刷盘 |
| `src/cluster_legacy.c` | clusterInit, clusterProcessPacket | 集群总线 |

## 参考

- [Redis 官方文档：命令参考](https://redis.io/docs/latest/commands/)
- [Redis 官方文档：持久化](https://redis.io/docs/latest/operate/oss_and_stack/management/persistence/)
- [Redis 官方文档：客户端处理](https://redis.io/docs/latest/develop/reference/clients/)
- [Redis 官方源码仓库](https://github.com/redis/redis)

## 与主核 / AI 的关系

本仓主数据库内核是从零实现的多模态数据库；Redis 页面用于机制对照，比较时应明确存储模型、并发模型与持久化保证的差异——尤其是「单线程命令执行」与本仓内核的并发模型并不等价。AI 生成的导航不能代替对指定 checkout 源码及版本的核对。
