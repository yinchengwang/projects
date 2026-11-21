> ⚠️ **归档说明**：本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考。
> 当前主核是 `engine/db/`（自带 Makefile，不在 CMake 图内），见 [设计文档](../../superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md)。

# Gap Analysis 子报告 — Stream（事件流 / 日志 / CDC / 订阅）

> 系列：自研多模态数据库 + RAG「自研 vs 业界」差距分析（13 篇子报告之九）
> 本篇：事件流引擎（Kafka 风格 partition + log / Pulsar 风格 ManagedLedger + Tiered Storage / Redpanda 风格 RAFT-native / CDC 订阅 / 结构化日志 / 向量流式索引）
> 时间：2026-10-16
> 状态：初稿
> 上游依赖：`archive/.superpowers/sdd/gap-task-1-inventory.md` §9 stream
> 对照 wiki：
> - `docs/db-wiki/stream/kafka.md`（Apache Kafka 3.7+，partition + ISR + KRaft + Tiered Storage，事实标准）
> - `docs/db-wiki/stream/pulsar.md`（Apache Pulsar 3.x，Broker + BookKeeper + ZK 三层 + Segment + Tiered + Functions + 多协议）
> - `docs/db-wiki/stream/redpanda.md`（Redpanda 24.x，C++20 thread-per-core + RAFT-native + io_uring + BSL 许可）
> 自身参考：`2026-08-29-multimodal-db-gap-analysis.md`（旧版顶层报告，本篇不替代，仅作纵向对照）

## 摘要（TL;DR）

自研流栈已经具备**「Kafka 风格流式核心引擎 + CDC + WAL + 订阅 + 结构化日志（log_engine）+ 向量流式索引 + 增量量化训练 + 并发搜索 + Merge Scheduler」**的核心骨架，覆盖度评级 **B**——本篇是 13 篇子报告里「事件流 / 订阅 / 日志引擎」三个面向都有代码、但缺少高级算子与分布式能力的过渡篇。与 Kafka 的差距集中在 **Partition + Segment + 稀疏索引（offset → file position）+ Time index + Idempotent Producer + Transactional EOS + __consumer_offsets + Kafka Streams DSL + Kafka Connect + Tiered Storage + KIP-848 Consumer Group + KRaft（无 ZK）+ Schema Registry + 5+ 语言官方 SDK + OpenMessaging Benchmark 公开基准**；与 Pulsar 的差距集中在 **ManagedLedger → Ledger → Entry 三层抽象 + BookKeeper Ensemble/Write Quorum/Ack Quorum + Subscription Cursor（独立于 partition）+ Pulsar Functions（轻量流处理）+ 内置 Schema Registry + 4 种订阅模式（Exclusive/Shared/Failover/Key_Shared）+ 多租户（Tenant/Namespace/Topic）+ Tiered Storage Offload + Geo-Replication + KOP Kafka 协议兼容**；与 Redpanda 的差距集中在 **RAFT-native（per-partition RAFT group + 严格 log 副本）+ thread-per-core（sharded service + 无锁 + CPU 亲和）+ io_uring 异步 I/O + Seastar reactor + LSM Tree 二级索引 + Iceberg Datalake 自动 Sink + Cluster Linking 跨集群 + Tiered Storage Offset Translation + Redpanda Console + Cloud（AWS/GCP/Azure 三云托管）**。

简言之：自研流是「**Kafka 风格流式核心引擎 + CDC + WAL + 订阅 + 结构化日志引擎 + 向量流式索引，但无 Partition Segment/Index 物理文件层 + ISR 副本协议 + EOS 幂等/事务 + Consumer Group + KRaft/Tiered/Schema Registry + BookKeeper 三层架构 + Multi-Tenant + Pulsar Functions/IO + RAFT 严格副本 + thread-per-core/io_uring/Iceberg + 跨集群 Geo-Replication/Cluster Linking + 5+ 语言 SDK + Cloud 托管**」。Kafka 是「事实标准 + 生态最广 + Apache 2.0 + KRaft GA」标杆，Pulsar 是「云原生 + 计算存储分离 + 多协议 + 多租户 + Apache 2.0」标杆，Redpanda 是「C++ 重写 + 10x 低延迟 + RAFT-native + Iceberg」标杆。扩展方向应以**「启动流式子系统新增：Partition + Segment 物理文件层 + 稀疏索引 + ISR 副本协议 + Idempotent Producer + 事务性 EOS + Consumer Group Protocol + Tiered Storage + Schema Registry + 5+ 语言客户端 SDK + Pulsar 风格 ManagedLedger/Ledger/Entry + BookKeeper 三副本 + Pulsar Functions 轻量流处理 + 多租户 Tenant/Namespace + Multi-Subscription（Exclusive/Shared/Failover/Key_Shared）+ RAFT-native Per-Partition 副本 + thread-per-core + io_uring 异步 I/O + Cluster Linking 跨集群 + Iceberg 自动 Sink + OpenMessaging Benchmark 公开基准」**为最高优先级，因为它直接决定了 `engine/src/db/storage/stream/` + `engine/src/db/subscription/` + `engine/src/db/storage/log/` + `engine/src/db/index/vector_index/streaming/` 能否从「核心引擎 + CDC + 日志 + 向量流式索引」跨越到「云原生分布式事件流平台」。

---

## 1. 自研现状

> 引用 `archive/.superpowers/sdd/gap-task-1-inventory.md` §9 stream（行 84 索引 + 附录），并补充定位说明。

1. **Kafka 风格流式核心引擎（覆盖度 B）**：
   - `engine/src/db/storage/stream/stream_engine.c`（约 650 行，23327 字节）实现流式核心（topic + partition + message + offset + producer/consumer 接口骨架）；
   - `engine/src/db/storage/stream/stream_engine.h`（约 80 行）声明流式引擎 API；
   - `engine/src/db/storage/stream/CMakeLists.txt`（构建为 `storage_stream` OBJECT 静态库），纳入 `project_includes`；
   - 公共头 `engine/include/db/stream_engine.h` 暴露 `stream_engine_init/create/open/close/produce/consume/seek/commit/ack` 接口；
   - 工程轨绑定测试 `engine/test/db/storage/stream_engine_test.cpp`（12574 字节，gtest）。

2. **CDC + 订阅 + WAL 协调（覆盖度 B）**：
   - `engine/src/db/subscription/cdc.c`（约 300 行）实现 CDC（Change Data Capture，从 DB 事务日志捕获变更事件，发往 stream topic 或外部消费者）；
   - `engine/src/db/subscription/cdc_wal.c`（约 320 行）实现 CDC + WAL 协调（变更事件先写 WAL 再投递，保证 at-least-once）；
   - `engine/src/db/subscription/subscription.c`（约 450 行）实现订阅管理（topic + partition + offset + cursor）；
   - `engine/src/db/subscription/CMakeLists.txt`（构建为 `subscription_lib` STATIC 库）；
   - 公共头 `engine/include/db/subscription/cdc.h` + `subscription.h` 暴露 CDC + 订阅 API。

3. **结构化日志引擎（覆盖度 B）**：
   - `engine/src/db/storage/log/log_engine.c`（8527 字节）实现日志引擎（结构化事件、字段类型、查询接口）；
   - `engine/src/db/storage/log/log_engine_ext.c`（4369 字节）实现日志扩展（window / tag / aggregation 钩子）；
   - `engine/src/db/storage/log/log_aggregate.c`（2165 字节）实现日志聚合（分组 + 计数 + 时间窗口）；
   - `engine/src/db/storage/log/log_label_index.c`（4216 字节）实现 label 索引（按 label 过滤检索）；
   - `engine/src/db/storage/log/CMakeLists.txt`（构建为 `storage_log` OBJECT 静态库）；
   - 公共头 `engine/include/db/log_engine.h` + `engine/include/db/structured_log.h` 暴露日志引擎 + 结构化事件 API。

4. **向量流式索引（自研亮点）**：`engine/src/db/index/vector_index/streaming/`（5 文件 + DESIGN.md）：
   - `streaming_index.c`（13715 字节）流式索引核心（在线构建 + 持续更新）；
   - `vector_write_buffer.c`（7863 字节）向量写缓冲（accumulator + flush + batch）；
   - `concurrent_search.c`（15641 字节）并发搜索（在线服务时 vector 写入不影响搜索）；
   - `merge_scheduler.c`（14127 字节）Merge Scheduler（增量 + 全量合并策略）；
   - `incremental_quant_train.c`（14165 字节）增量量化训练（在线 PQ/HNSW + Quantization）；
   - `DESIGN.md`（10318 字节）流式索引设计文档；
   - 测试 `engine/test/db/index/vector_index/test_streaming_index.cpp`（10743 字节）+ `streaming_benchmark.cpp`。

5. **RPC 流式接口（适配层）**：`engine/include/db/rpc_stream.h` + `engine/src/db/distributed/rpc/rpc_stream.c` 实现分布式 RPC 流式管道（与 stream 不直接耦合），配套测试 `engine/test/db/executor/rpc_stream_test.cpp`。

6. **缺失模块（明确标注）**：与 Kafka / Pulsar / Redpanda 对比，自研流**无** Partition Segment 物理文件层（仅抽象 topic + partition）、**无** 稀疏索引（offset → file position）、**无** Time index（timestamp → offset）、**无** ISR 副本协议 + Leader Epoch + HW、**无** Idempotent Producer + 事务性 EOS（producer ID + sequence + 两阶段 commit）、**无** Consumer Group Protocol（含 KIP-848 新一代服务器侧心跳）、**无** Tiered Storage（hot/cold 分层 + S3/MinIO 适配）、**无** KRaft 共识（依赖 ZK 或 etcd 替代）、**无** Confluent Schema Registry 等 schema 演进机制、**无** Kafka Streams DSL + Kafka Connect 框架 + ksqlDB 持续查询、**无** ManagedLedger/Ledger/Entry 三层抽象 + BookKeeper 三副本 + Write Quorum/Ack Quorum 独立存储层、**无** Pulsar Functions（轻量流处理 + Serverless 自动扩缩）、**无** Multi-Tenant（Tenant/Namespace/Topic 三级）+ 4 种订阅模式（Exclusive/Shared/Failover/Key_Shared）、**无** Subscription Cursor 独立维护、**无** Geo-Replication 跨地域复制、**无** KOP Kafka 协议兼容、**无** RAFT-native Per-Partition 严格副本 + thread-per-core（Seastar sharded service）+ io_uring 异步 I/O、**无** LSM Tree 二级索引（24.x GA）+ Iceberg Datalake 自动 Sink + offset_translation_layer、**无** Cluster Linking 跨集群异步复制、**无** 5+ 语言客户端 SDK（仅 C 嵌入式 API）、**无** OpenMessaging Benchmark 公开基准、**无** 成熟 Web Console（CMAK / AKHQ / Confluent Control Center / Pulsar Manager / Redpanda Console）。

**关键路径速查**（与本报告后续引用一致）：

- 流式核心引擎：`engine/src/db/storage/stream/stream_engine.c`
- 流式核心头：`engine/include/db/stream_engine.h`
- 流式核心目录：`engine/src/db/storage/stream/`
- 流式核心测试：`engine/test/db/storage/stream_engine_test.cpp`
- CDC 引擎：`engine/src/db/subscription/cdc.c` + `cdc_wal.c`
- 订阅管理：`engine/src/db/subscription/subscription.c`
- 公共订阅头：`engine/include/db/subscription/cdc.h` + `subscription.h`
- 订阅目录：`engine/src/db/subscription/`
- 日志引擎：`engine/src/db/storage/log/log_engine.c`
- 日志扩展：`engine/src/db/storage/log/log_engine_ext.c`
- 日志聚合：`engine/src/db/storage/log/log_aggregate.c`
- 日志标签索引：`engine/src/db/storage/log/log_label_index.c`
- 公共日志头：`engine/include/db/log_engine.h` + `structured_log.h`
- 日志目录：`engine/src/db/storage/log/`
- 向量流式索引：`engine/src/db/index/vector_index/streaming/streaming_index.c`
- 向量写缓冲：`engine/src/db/index/vector_index/streaming/vector_write_buffer.c`
- 向量流式目录：`engine/src/db/index/vector_index/streaming/`
- 向量流式测试：`engine/test/db/index/vector_index/test_streaming_index.cpp`
- 向量流式设计：`engine/src/db/index/vector_index/streaming/DESIGN.md`

---

## 2. 业界主流做法

下面三篇对照项目分别代表「事实标准 + 生态最广 + Apache 2.0 + KRaft + Tiered」「云原生 + 计算存储分离 + 多协议 + 多租户」「C++ 重写 + RAFT-native + 10x 低延迟 + Iceberg」三条路线，重点对比「Log 结构 / 分区模型 / 副本一致性 / 流处理 / Schema 演进 / 协议与生态」六个维度。

### 2.1 Apache Kafka 3.7+（事实标准 + KRaft + Tiered Storage）

详细：`docs/db-wiki/stream/kafka.md`

- **整体定位**：由 LinkedIn 起源、Apache 顶级项目的事件流平台；以 **append-only log（顺序写盘）** + **partition + ISR** 为核心，是事实标准的事件流中间件。
- **Log 结构（核心抽象）**：
  - **Partition + Segment 文件**：每个 partition 是严格 append-only 的顺序写日志；物理上被切分为多个 segment 文件（达到 `segment.bytes=1GB` 或 `segment.ms=7天` 滚动）；
  - **稀疏索引（.index）**：每写入一定字节插一项（默认 `index.interval.bytes=4096`），用于二分查找定位 offset；
  - **Time index（.timeindex）**：timestamp → offset 稀疏索引，支持 `offsetsForTimes` API 按时间定位；
  - **Txn index（.txnindex）**：事务 commit/abort marker 索引（KIP-98），支持 EOS 读取过滤；
  - **零拷贝**：Consumer 拉取走 `FileChannel.transferTo` → `sendfile()` 系统调用，避免用户态拷贝；
  - **页缓存友好**：所有读写优先 OS 页缓存，broker 崩溃不丢（依赖副本而非本地持久化）。
- **分区模型 + 副本与一致性（ISR）**：
  - **Partition = shard**（topic 内并行度单位 + 顺序保证单位）；
  - **Hash / RoundRobin / Sticky Partitioner**（default murmur2 hashing）；
  - **ISR（In-Sync Replicas）** + **acks=all** + **min.insync.replicas**；
  - **HW + Leader Epoch（KIP-320）**：选举递增 epoch，取代旧版 HW 恢复（避免脑裂）；
  - **__consumer_offsets** 内部 topic 存 `(group, topic, partition) → offset`，由 **GroupCoordinator** 管理；
  - **KIP-848** Consumer Group Protocol: Consumer（3.7 GA）：服务器侧心跳，消除 stop-the-world rebalance。
- **EOS Exactly-Once Semantics**：
  - **幂等 Producer**：`enable.idempotence=true` 每条带 `(producerId, sequenceNumber)` broker 去重；
  - **事务 Producer**：`transactional.id` 跨 topic-partition + offset commit 原子提交；
  - **__transaction_state** 内部 topic 存事务状态（coordinator partition 选举）+ 两阶段 commit；
  - **read_committed** Consumer 隔离级别，只读已提交事务的 record。
- **Tiered Storage（3.6 GA，KIP-405）**：
  - **Hot tier**：本地磁盘（`local.retention.ms`/`local.retention.bytes`）；
  - **Cold tier**：对象存储（S3 / Azure Blob / GCS / HDFS / MinIO / Ozone）；
  - 启动时异步把已完成 segment 上传到对象存储，consumer 按需从对象存储拉取。
- **流处理 / 协议 / 生态**：
  - **Kafka Streams**（嵌入式 Java 库 + DSL：KStream / KTable / GlobalKTable / window / join）+ RocksDB State Store；
  - **Kafka Connect** + Source/Sink Connector（Debezium CDC）+ ksqlDB 持续 SQL；
  - **Confluent Schema Registry** + Avro/Protobuf/JSON Schema + BACKWARD/FORWARD/FULL 兼容性；
  - **官方 5+ 语言 SDK**（Java / Scala / Python `confluent-kafka-python` / Go / C++ `librdkafka` / .NET / Node.js）+ 社区 Rust/Ruby；
  - **KRaft**（3.0 GA）：完全移除 ZooKeeper；**Debezium CDC 集成**；
  - **Strimzi Operator**（K8s 生产级）。
- **关键路径**：`core/src/main/scala/kafka/server/ReplicaManager.scala`（ISR + HW）+ `core/src/main/scala/kafka/log/LogManager.scala`（Log + Segment 滚动）+ `core/src/main/scala/kafka/raft/KafkaRaftManager.scala`（KRaft）+ `core/src/main/scala/kafka/coordinator/transaction/TransactionCoordinator.scala`（EOS）+ `core/src/main/scala/kafka/coordinator/group/GroupCoordinator.scala`（Consumer Group）+ `clients/src/main/java/org/apache/kafka/clients/producer/KafkaProducer.java`（Producer）+ `streams/src/main/java/org/apache/kafka/streams/KafkaStreams.java`（Streams）。

### 2.2 Apache Pulsar 3.x（云原生 + 计算存储分离 + 多协议 + 多租户）

详细：`docs/db-wiki/stream/pulsar.md`

- **整体定位**：由 Yahoo! 开源、Apache 顶级项目的云原生分层消息流平台；以 **计算与存储分离（Broker 无状态 + BookKeeper 有状态）** + **Segment 级存储** + **多租户（Tenant/Namespace/Topic）** + **内置 Functions / IO Connectors** 为差异化。
- **三层架构（核心）**：
  - **计算层 Broker**（无状态）：协议处理 + Topic 调度 + Dispatcher + Schema 协调 + 事务协调；故障时其他 broker 直接接管（毫秒级）；
  - **存储层 BookKeeper**（有状态）：Segment 持久化 + 副本同步 + 读写路径；
  - **协调层 ZooKeeper（或 etcd）**：集群元数据 + Broker 注册 + Topic 配置。
- **ManagedLedger → Ledger → Entry 三层抽象**：
  - **ManagedLedger**：topic 的逻辑视图（持久 + active ledgers）；
  - **Ledger**：BookKeeper 的一次写入单元（默认 5,000 entries / 100MB 滚动）；
  - **Entry**：单条消息（ledgerId + entryId + payload）；
  - **ManagedCursor**：每个订阅一个 cursor（独立维护位置）。
- **BookKeeper 副本协议（Ensemble / Write Quorum / Ack Quorum）**：
  - **Ensemble (E)**：ledger 的 bookie 集合（如 [B1, B2, B3]）；
  - **Write Quorum (Qw)**：每条 entry 写入的 bookie 数（默认 3）；
  - **Ack Quorum (Qa)**：broker 等待 Qa 个 bookie ack 才返回（默认 2）；
  - `E=3, Qw=3, Qa=2` 容忍 1 bookie 故障；无 leader 概念（与 Kafka ISR 不同）。
- **4 种订阅模式 + Multi-Tenant**：
  - **Exclusive / Shared / Failover / Key_Shared** 四种订阅模式；
  - **Tenant / Namespace / Topic 三级多租户**（Tenant 设配额 / Namespace 设 retention / Topic 消息流）；
  - **Bundle**：namespace 内 topic 的逻辑分组（一致性 hash 分配到 broker + 动态负载均衡）。
- **EOS + Schema Registry + Geo-Replication**：
  - **EOS 事务**（2.5+）：幂等 Producer + bookie catalog（`__transaction_coordinator`）+ 2PC；
  - **内置 Schema Registry**（无需单独部署）：String / JSON / Avro / Protobuf / KeyValue；
  - **Geo-Replication**：跨 cluster 异步复制（最终一致）。
- **Tiered Storage + Offload**：
  - active ledger 关闭后异步上传到 Tiered Storage（S3 / GCS / Azure Blob / HDFS / NFS）；
  - Broker Offloader 读取 + 序列化 + 压缩 + 上传；
  - consumer 按需从对象存储拉取。
- **Pulsar Functions / IO Connectors / KOP / 协议**：
  - **Pulsar Functions**（轻量流处理）：三种部署模式（Local/Cluster/Serverless）+ Java/Python/Go；
  - **Pulsar IO**：内置 30+ Source/Sink Connector（JDBC/ES/Iceberg/Kafka）；
  - **KOP Kafka 兼容协议**（3.0 GA）：Kafka 客户端无需改代码；
  - **多协议**：Pulsar Binary + Kafka + MQTT + AMQP + WebSocket。
- **关键路径**：`pulsar-broker/src/main/java/org/apache/pulsar/broker/PulsarService.java`（启动）+ `service/BrokerService.java`（Topic 管理）+ `service/PersistentTopic.java` + `PersistentSubscription.java`（topic 生命周期）+ `managed-ledger/.../ManagedLedgerImpl.java`（Ledger 管理）+ `BookKeeperClientFactoryImpl.java`（bookie 交互）+ `transaction/`（EOS 协调）+ `functions/runtime/`（Functions 调度）+ `tiered-storage/jcloud/`（Offload 实现）+ `streamnative/kop`（KOP Kafka 兼容）。

### 2.3 Redpanda 24.x（C++ 重写 + RAFT-native + io_uring + Iceberg）

详细：`docs/db-wiki/stream/redpanda.md`

- **整体定位**：由 Vectorized 2019 开发的 C++ 重写版 Kafka 兼容流平台；以 **单二进制 RAFT-native（无 ZooKeeper）** + **thread-per-core** + **io_uring** + **Seastar** 为差异化，号称 "10x faster, 1/10 complexity"。
- **单进程 + C++20 + thread-per-core**：
  - **sharded service**（Seastar `ss::sharded<T>`）：每个 shard 一个 T 实例，跨 shard 用 `ss::future<T>` + `invoke_on` 投递；
  - **CPU 亲和**（`taskset` / `numactl`）+ **无锁** + **缓存友好** + **延迟稳定**（无 GC 无线程切换抖动）；
  - **代价**：核心数需 ≥ 16 / 跨 partition 操作复杂。
- **RAFT-native（per-partition RAFT group）**：
  - **每个 partition 一个 RAFT group**（与 Kafka ISR 不同）；
  - **无独立 Controller**：controller state 存在 RAFT log（`__cluster_metadata`）；
  - **Leader Election**：基于 RAFT term 超时触发预选 + 投票；
  - **Log Entry**：每次写入是一条 RAFT log entry（含 producer batch + producer ID + epoch）；
  - **recovery_throttle**：恢复限流避免落后 replica 拖累集群。
- **异步 I/O + 存储**：
  - **基于 Seastar** + **Linux io_uring**（Linux 5.10+）+ 单次 syscall 提交多个 I/O 请求降低 syscall 开销；
  - **物理文件结构**（与 Kafka 格式兼容）：`data_dir/kafka/<internal-topic>/<partition-id>/{n_n.log, n_n.index, n_n.txn}`；
  - **每段大小** `log_segment_size`（默认 128MB，相比 Kafka 1GB 略小）；
  - **batch_cache + chunk_cache**（读缓存）+ **直接顺序读**（mmap 或 pread）+ **fdatasync**（durability）。
- **Tiered Storage + Iceberg Datalake + Cluster Linking**：
  - **partition_manifest**（每 partition 的 segment 清单）+ **offset_index + offset_translation_layer**（Kafka offset 与 Redpanda offset 不连续时拉取 + 转换）；
  - **S3 / Azure Blob / GCS / MinIO** 适配；
  - **Iceberg Datalake Sink**（`src/v/datalake/` + `src/v/iceberg/`）：直接把 partition data 转 Iceberg 表格式写入对象存储 + 集成 Spark / Trino / Snowflake / Athena；
  - **Cluster Linking**（v24+）：跨集群异步复制（Redpanda Cloud 与自建集群）+ 支持 Schema 转换 + 双向复制（优于 MirrorMaker）。
- **协议 / 流处理 / 生态**：
  - **100% Kafka 协议兼容**：confluent-kafka-go / kafka-clients.jar / rpk CLI 直连；
  - **Data Transforms**（v23+，基于 WebAssembly 的流转换 + Iceberg Sink 替代部分 Kafka Connect 用途）；
  - **External Flink / Spark / Materialize**（兼容 Kafka 协议消费 Redpanda）；
  - **Confluent Schema Registry 兼容**（通过 HTTP API）+ Avro/Protobuf/JSON Schema；
  - **Redpanda Cloud**（AWS / GCP / Azure 三云）+ **Redpanda Enterprise**（Iceberg Sink / Cluster Linking / Data Transforms / SAML/LDAP）+ **Redpanda Console**（基于 Kowl）。
- **性能与许可**：
  - **p99 < 5ms**（vs Kafka ~20ms，单 broker ~1GB/s）+ **10x 更快**（同等硬件）；
  - **BSL 1.1** 许可：可商用（含商业用途），但**不允许将 Redpanda 作为托管服务再销售**；每版本发布后 4 年自动转 Apache 2.0。
- **关键路径**：`src/v/redpanda/application.cc` + `main.cc`（单进程启动）+ `src/v/kafka/server/app.cc`（Kafka 协议层）+ `protocol/`（请求/响应 schema）+ `src/v/storage/disk_log_impl.cc`（主存储路径）+ `src/v/raft/consensus.cc`（RAFT 状态机）+ `src/v/cluster/metadata_cache.cc` + `partition_balancer.cc`（集群调度）+ `src/v/cloud_storage/partition_manifest.cc` + `offset_translation_layer.cc`（对象存储抽象）+ `src/v/datalake/datalake_table_creator.cc`（Iceberg Sink）+ `src/v/cluster/cluster_link/`（跨集群同步）+ `src/v/compaction/{key_offset_map,filter,reducer}.cc`（Compaction）。

---

## 3. 24 维度对照矩阵

> 等级：A=工业级，缺失≤1 个版本号差距 / B=教学+工程可用，关键能力缺失 / C=占位，仅 API / D=空。
> 本表覆盖自研 vs Kafka vs Pulsar vs Redpanda 四方对照。

| # | 维度 | 自研（book） | Kafka | Pulsar | Redpanda | 等级 | 关键说明 |
|---|------|-------------|-------|--------|----------|------|------|
| 1 | 项目定位 | 流式核心 + CDC + 订阅 + 日志 + 向量流式索引 | 事实标准事件流平台 + 生态最广 + Apache 2.0 | 云原生分层消息流平台 + 计算存储分离 + 多协议 + 多租户 | C++ 重写 RAFT-native Kafka 兼容 + 10x 延迟 + Iceberg | B | 自研缺 Partition + Segment + Tiered + 多租户 + RAFT |
| 2 | 发展历史 | 单一维护者内部迭代 | 2010 起，14 年，每年 4-6 版本 | 2012 起，12 年，每年 4-6 版本 | 2019 起，5 年，每 2-4 周 minor | C | 历史沉淀薄，缺版本演进纪律 |
| 3 | 许可限制 | 内部使用，无外部约束 | Apache 2.0（最宽松） | Apache 2.0（最宽松） | BSL 1.1（4 年后转 Apache 2.0） | A | 自研无开源义务，但也缺外部审计 |
| 4 | 使用方案例 | 内部测试 + 多模态 RAG demo 集成 | LinkedIn/Uber/Netflix/字节/美团/阿里 | Yahoo/Tencent/Splunk/Twitter/华为云 | Datadog/Akamai/Cisco/Tencent/Binance | C | 缺外部生产案例 |
| 5 | 对比定位 | 「流式核心 + CDC + 订阅 + 日志 + 向量流式索引」 | 事实标准 + 生态最广 + Apache 2.0 + KRaft | 云原生 + 计算存储分离 + 多租户 + Functions | C++ 重写 + RAFT-native + io_uring + Iceberg | B | 自研缺 Partition+Segment+EOS+Tiered+多租户+RAFT |
| 6 | 整体架构 | 单进程嵌入式 C API（无 server 进程） | 单进程多线程 + KRaft Controller（无 ZK） | 三层服务（Broker 无状态 + BookKeeper 有状态 + ZK） | 单二进制 C++20 + thread-per-core + io_uring | C | 缺独立 server 进程 + 协议监听 + 集群调度 + 三层 |
| 7 | **Log 结构** | **抽象 topic + partition + message + offset API 骨架（无物理文件层）** | **Partition + Segment 文件（1GB 滚动）+ 稀疏 .index + .timeindex + .txnindex + 零拷贝 + 页缓存友好** | **ManagedLedger → Ledger → Entry（5K entries/100MB 滚动）+ BookKeeper .idx + Journal + Entry Log** | **Partition + Segment（128MB）+ .index + .txn + batch_cache + chunk_cache + 直接顺序读 + fdatasync** | **B-** | **自研 API 骨架完整，缺 Segment 物理文件 + 稀疏索引 + Time/Txn index + 页缓存工程化** |
| 8 | **分区模型** | **topic + partition 抽象接口（路由 + ack + offset 推进）** | **Partition（并行度 + 顺序保证单位）+ Hash/RoundRobin/Sticky Partitioner + 动态 partition 增减** | **Topic Partition + Bundle（namespace 内一致性 hash + 动态负载均衡）+ Ledger 三副本** | **Partition 兼容 Kafka + Controller Group + partition_balancer 自动均衡 + Tiered Storage 友好** | **B** | **自研 API 完整，缺 Sticky/Custom Partitioner + 动态 partition 增减 + Bundle 分配** |
| 9 | **副本与一致性** | **CDC WAL 单副本 WAL + at-least-once（局部一致）** | **ISR（In-Sync Replicas）+ acks=all/0/1 + min.insync.replicas + HW + Leader Epoch（KIP-320）+ KRaft Controller** | **BookKeeper Ensemble/Write Quorum/Ack Quorum（默认 E=3 Qw=3 Qa=2）+ Erlang Quorum 协议 + 不依赖 leader 选举** | **RAFT-native Per-Partition RAFT Group + RAFT 多数派 + Leader Election（< 1s）+ recovery_throttle + Snapshot** | **C** | **自研 CDC WAL 单节点副本 + at-least-once，缺 ISR/Ensemble/RAFT 严格副本** |
| 10 | EOS / 事务语义 | 无（仅 at-least-once） | 幂等 Producer + 事务 Producer + __transaction_state + read_committed + 跨 topic-partition 2PC | 幂等 Producer（producerName + sequence）+ 事务（bookie catalog） + 跨 topic + offset 原子 | 幂等 Producer + 事务（RAFT 控制批次）+ read_committed + RAFT log marker | C | 缺 idempotent producer + 事务 producer + 2PC + read_committed |
| 11 | **Consumer Group** | **subscription.c 订阅管理（topic + partition + offset + cursor 接口骨架）** | **__consumer_offsets + GroupCoordinator + KIP-848（服务器侧心跳）+ Auto/Manual Commit + at-most/at-least/exactly-once** | **Subscription Cursor（每个订阅独立 Position(ledgerId, entryId)）+ Individual/Cumulative Ack + Key_Shared + Backlog Quota** | **__consumer_offsets（兼容 Kafka）+ Group Coordinator + KIP-848 部分支持** | **B-** | **自研订阅接口骨架完整，缺 Consumer Group 协议 + Coordinator + KIP-848 + 4 种订阅模式** |
| 12 | Tiered Storage | 无 | 3.6+ GA KIP-405 Hot/Cold tier + S3/Azure Blob/GCS/HDFS/MinIO/Ozone + local.retention | 2.8+ GA active ledger 自动 offload + jcloud（S3/GCS/Azure Blob/HDFS/NFS）+ managedLedgerOffloadThresholdSize | Tiered Storage GA + offset_translation_layer + partition_manifest + S3/Azure/GCS/MinIO | D | 缺对象存储适配 + offset translation + manifest + offload 调度 |
| 13 | **流处理引擎** | **vector streaming index（在线构建 + 增量量化 + 并发搜索 + merge scheduler），但无通用 stream DSL** | **Kafka Streams（嵌入式 Java 库 + KStream/KTable/DSL）+ ksqlDB 持续 SQL + RocksDB State Store + EOS state store + Kafka Connect** | **Pulsar Functions（Local/Cluster/Serverless 三模式）+ Java/Python/Go + Pulsar IO 30+ Connector + StateStore（BookKeeper KV）** | **Data Transforms（WebAssembly 流转换）+ Iceberg Sink（替代部分 Connect）+ 外部 Flink/Spark/Materialize** | **B-** | **自研 vector streaming 是亮点（在线构建 + 增量量化 + merge），缺通用 DSL + Connect 框架 + 函数式流处理** |
| 14 | Schema 演进 | 无（message 仅 string + bytes） | Confluent Schema Registry + Avro/Protobuf/JSON Schema + BACKWARD/FORWARD/FULL 兼容性 + REST API | 内置 Schema Registry（无需独立部署）+ String/JSON/Avro/Protobuf/KeyValue + BACKWARD/FORWARD/FULL + per-namespace | Confluent Schema Registry 兼容（HTTP API）+ Avro/Protobuf/JSON Schema + Iceberg Schema 自动生成 | D | 缺独立 Schema 服务 + 类型系统 + 兼容性检查 |
| 15 | **协议 / 客户端 SDK** | **无协议层（C 嵌入式 API 仅嵌入式 process 调用）** | **Kafka Wire Protocol（TCP）+ Java/Scala（核心）+ Python（confluent-kafka-python）/Go/C++（librdkafka）/.NET/Node.js + 社区 Rust/Ruby + JDBC + Connect REST** | **Pulsar Binary Protocol + Kafka 协议（KOP GA）+ MQTT + AMQP + WebSocket + Java/Go/Python/C++/Node.js/Rust/.NET** | **100% Kafka 协议兼容 + rpk CLI（Go 跨平台）+ Redpanda Console + Admin API（HTTP 9644）+ 所有 Kafka SDK 直连** | **D** | **缺协议层 + 5+ 语言 SDK + JDBC + Web Console** |
| 16 | 多租户 / 隔离 | 无 | 无（仅 ACL + 商业版 Multi-Tenancy） | Tenant / Namespace / Topic 三级 + 配额 + ACL + 资源隔离 | 无（仅 RBAC + Enterprise 多租户） | D | 缺多租户（仅 Pulsar 是原生一等公民） |
| 17 | Geo-Replication / Cluster Linking | 无 | MirrorMaker 2 + Confluent Cluster Linking（商业） | 内置 Geo-Replication（多 cluster 异步 + 基于 namespace + 单方向 + StreamNative Enterprise 多向） | Cluster Linking（v24+）+ 异步复制 + Schema 转换 + 双向（优于 MirrorMaker） | D | 缺跨地域复制 + 集群链接 |
| 18 | **日志引擎 / 可观测性** | **log_engine.c + log_engine_ext.c + log_aggregate.c + log_label_index.c（结构化事件 + 字段类型 + 窗口 + 标签索引 + 聚合）** | **无内置（用户自实现）+ Connect Sink→ES/S3 + Prometheus JMX Exporter（9999）+ CMAK/AKHQ/Confluent Control Center** | **无内置（通过 Pulsar IO → ES/S3）+ Prometheus（8080）+ Pulsar Manager（Web UI by StreamNative）** | **无内置（Sink→S3/Iceberg）+ Prometheus（9644）+ Redpanda Console（基于 Kowl）+ Admin API** | **A-** | **自研结构化日志引擎 + 窗口 + 标签索引是 Pulsar/Redpanda 都没有的差异化亮点；缺 Prometheus 集成 + Web Console + Dashboard** |
| 19 | 复制 HA / Cluster | 无 HA（单节点） | KRaft Controller 选举（无 ZK）+ Partition Leader Election（< 几秒）+ __cluster_metadata topic + MirrorMaker 2 | Broker ZK 自动 leader 选举 + bundle 重分配（毫秒级）+ Bookie ensemble 副本 | RAFT 多数派（默认 3 副本，容忍 1 故障）+ Controller 内嵌 + Cluster Linking 跨集群 | D | 缺 Controller 选举 + Leader 选举 + Multi-Node + DR |
| 20 | 性能基准 | 内部 gtest 通过，缺 OpenMessaging Benchmark | 单 partition 吞吐 ~900 MB/s（pipelined 批 LZ4）+ p99 < 10ms（同区域）+ 千万级消息/秒（LinkedIn） | partition 数百以下与 Kafka 相当；大集群 > 100 broker 吞吐优 + p99 ~ 5ms（bookie） | p99 < 5ms（vs Kafka ~20ms）+ 单 broker ~1GB/s + 10x 更快（同硬件）+ Confluent 2023 验证 | C | 缺公开基准（OpenMessaging / Confluent / Vectorized）+ thread-per-core/io_uring 极致 |
| 21 | 运维可观测性 | 简单 stats + log_engine + log_label_index | JMX 9999 + Prometheus 导出 + CMAK/AKHQ/Confluent Control Center + 慢请求日志 + 备份（MirrorMaker/Tiered Storage） | Prometheus 8080 + Pulsar Manager（Web UI）+ `pulsar-admin` CLI + Tiered Storage 自动异地 | Prometheus 9644 + Redpanda Console + rpk CLI + Admin API + Cluster Linking | C | 缺 Prometheus 集成 + Web Console + CLI 工具链 |
| 22 | 生态集成 | `engine/src/db/index/vector_index/streaming/` 复用向量底座 + `engine/src/db/subscription/` + `engine/src/db/storage/log/` | Debezium CDC + Kafka Connect 100+ Connector + LangChain/LlamaIndex/Flink/Spark/Materialize + 5+ 语言 SDK + K8s Strimzi | Flink/Spark/Pinot/Hudi 一等公民 + LangChain/LlamaIndex + StreamNative/KOP + 多云托管 | Flink/Spark/Materialize + Iceberg/Snowflake/Athena + Datadog/Binance/Hudson River + K8s Redpanda Operator | D | 客户端 SDK + Connector 框架 + 容器镜像 + Cloud 缺失 |
| 23 | 核心论文 | 无独立发表 | Kafka "a Distributed Messaging System for Log Processing"（NetDB 2011）+ Raft（ATC 2014）+ KIP-98 EOS + KIP-405 Tiered + KIP-848 | Pulsar Yahoo 2015 + BookKeeper 2008 + Geo-Replication 2018 + Consistent Hashing | RAFT（Ongaro PhD 2014）+ io_uring + Seastar + LSM Tree（O'Neil 1996）+ Iceberg（SIGMOD 2024 Industry） | A | 自研追踪全部论文但无独立发表 |
| 24 | 源码路径（对照） | `engine/src/db/storage/stream/` + `engine/src/db/subscription/` + `engine/src/db/storage/log/` + `engine/src/db/index/vector_index/streaming/` | `core/src/main/scala/kafka/server/{ReplicaManager,LogManager}.scala` + `kafka/coordinator/{group,transaction}/` + `kafka/raft/` + `clients/` + `streams/` | `pulsar-broker/.../{PulsarService,BrokerService,PersistentTopic,PersistentSubscription}.java` + `managed-ledger/.../ManagedLedgerImpl.java` + `external/bookkeeper/` + `pulsar-functions/` + `pulsar/tiered-storage/` | `src/v/redpanda/application.cc` + `src/v/kafka/server/app.cc` + `src/v/raft/consensus.cc` + `src/v/storage/disk_log_impl.cc` + `src/v/cloud_storage/` + `src/v/datalake/` + `src/v/cluster/cluster_link/` | A | 路径分层清晰，与 Kafka/Pulsar/Redpanda 一一对应 |
| 25 | 与本项目关联 | 即本项目，无需解释 | Kafka 风格流核心 + Tiered Storage + EOS + Schema Registry 是「云原生事件流平台」原型 | 3 层架构 + Multi-Tenant + Functions + KOP + Tiered 是「云原生流平台」原型 | C++ 重写 + RAFT-native + thread-per-core/io_uring + Iceberg 是「10x 延迟流平台」原型 | A | 路径一一对应，可对照 wiki 学习 |

> 注：上表实际为 25 行（含「与本项目关联」一行扩展），24 维度主表保持不变。

**矩阵读法**：

- **等级 A**：维度 3 / 18 / 23 / 24 / 25（共 5 项，其中维度 18「日志引擎」是 Kafka/Pulsar/Redpanda 都没有的自研亮点）
- **等级 B**：维度 1 / 5 / 7 / 8 / 11 / 13（共 6 项）
- **等级 C**：维度 2 / 4 / 6 / 9 / 10 / 19 / 20 / 21（共 8 项）
- **等级 D**：维度 12 / 14 / 15 / 16 / 17 / 22（共 6 项）

**整体评级：B-**——维度 18「结构化日志引擎 + 窗口 + 标签索引」是 Kafka/Pulsar/Redpanda 都没有的差异化亮点，CDC + WAL + 向量流式索引是 B 级骨架，但缺维度 12（Tiered Storage）/ 14（Schema 演进）/ 15（协议 + SDK）/ 16（多租户）/ 17（Geo-Replication）/ 22（生态）六块 D 级补丁 + 维度 9（ISR/Ensemble/RAFT 副本）+ 10（EOS）两块 C 级补丁；建议下月把 P0 集中在维度 6 / 9 / 10 / 11 / 12 / 14 这六个等级 C/D 的联动（**P0 必含「启动流式子系统」**——见 §7）。

---

## 4. 关键差距 Top-5

每条都按「自研代码路径 ↔ 业界做法 ↔ 差距量级 ↔ 影响」四段式给出，并附仓库路径与 wiki 路径两个引用，方便后续任务直接卡条。

### 4.1 Partition + Segment 物理文件层 + 稀疏索引 + Time/Txn index + Tiered Storage + Idempotent Producer + 事务 EOS 七件套全链路空白（等级 B-/C → A）

- **自研现状**：`engine/src/db/storage/stream/stream_engine.c`（23327 字节）实现 topic + partition + message + offset + producer/consumer **API 骨架**；`engine/src/db/subscription/cdc.c` + `cdc_wal.c` + `subscription.c` 实现 CDC + WAL + 订阅；但**无** Partition + Segment 物理文件层（仅抽象）、**无** 稀疏索引（offset → file position）、**无** Time index（timestamp → offset）、**无** Txn index（事务 commit/abort marker）、**无** Idempotent Producer（`enable.idempotence=true`）、**无** 事务 Producer（`transactional.id` + 2PC + `__transaction_state`）、**无** read_committed Consumer 隔离、**无** Tiered Storage（hot/cold tier + S3/MinIO + manifest + offset translation）。
- **业界做法**：Kafka `core/src/main/scala/kafka/log/LogManager.scala`（Segment 滚动 + 稀疏索引 + 零拷贝）+ `core/src/main/scala/kafka/server/ReplicaManager.scala`（ISR + HW + Leader Epoch）+ `core/src/main/scala/kafka/coordinator/transaction/TransactionCoordinator.scala`（EOS 两阶段 commit）+ `kafka/raft/KafkaRaftManager.scala`（KRaft Controller）+ 3.6+ Tiered Storage KIP-405；Pulsar `pulsar/managed-ledger/.../ManagedLedgerImpl.java`（Ledger 滚动 + Journal + Entry Log）+ `external/bookkeeper/`（Ensemble + Write/Ack Quorum）+ Pulsar 事务（bookie catalog `__transaction_coordinator` + 2PC）+ Tiered Storage Offload + jcloud；Redpanda `src/v/storage/disk_log_impl.cc`（segment 物理 + batch_cache + chunk_cache + fdatasync）+ `src/v/raft/consensus.cc`（RAF 严格副本）+ `src/v/cloud_storage/partition_manifest.cc` + `offset_translation_layer.cc`（Tiered offset translation）+ Iceberg Datalake。
- **差距量级**：约 10-15 个文件 / 12000-20000 行 C；包含 Segment 物理文件层（`segment.c` 顺序写 + 滚动 1GB/128MB）+ 稀疏索引 (`index.c` + 二分查找)+ Time index（`timeindex.c`）+ Txn index（`txnindex.c`）+ Producer 流批处理 + Idempotent Producer（producerId + sequenceNumber + broker 端去重）+ 事务 Producer（`__transaction_state` 内部 topic + 2PC Coordinator）+ read_committed 隔离 + Tiered Storage（`tiered_storage/` 目录 + S3/MinIO 适配 + manifest + offset_translation + 异步 offload）。
- **影响**：缺物理 Log 结构 = 流引擎仅抽象层 / 无法承载 GB-TB 级真实日志；缺 ISR + EOS = 节点故障可能丢/重数据（RPO=0 不可达）；缺 Tiered Storage = 无法长期保留（retention 受本地磁盘限制）；与 Kafka / Pulsar / Redpanda 等所有事件流中间件不兼容，外部业务无法对接。

### 4.2 RAFT-native Per-Partition 副本 / KRaft Controller / BookKeeper Ensemble 三种集群协议全链路空白（等级 C → A）

- **自研现状**：`engine/src/db/subscription/cdc_wal.c` 实现 CDC WAL 单副本 + at-least-once；`engine/src/db/storage/wal/wal.c` + `engine/src/db/storage/wal/wal_buf.c` 是底层单节点 WAL；但**无** ISR 副本协议（leader + followers + HW + Leader Epoch）、**无** KRaft Controller（Kafka 3.x 完全移除 ZK 后的 RAFT 共识层）、**无** BookKeeper 三层架构（Broker + Bookie + ZK）+ Ensemble/Write Quorum/Ack Quorum（`E=3, Qw=3, Qa=2`）、**无** RAFT-native Per-Partition RAFT Group（Redpanda 差异化）、**无** Multi-Node 部署与 Partition Leader 选举、**无** MirrorMaker 2 / Cluster Linking 跨集群同步。
- **业界做法**：Kafka `core/src/main/scala/kafka/server/ReplicaManager.scala`（ISR + acks=all + min.insync.replicas）+ `core/src/main/scala/kafka/raft/KafkaRaftManager.scala`（KRaft Controller 选举 + __cluster_metadata）+ MirrorMaker 2（`connect/mirror/`）+ Confluent Cluster Linking（商业）；Pulsar `pulsar-broker/src/main/java/org/apache/pulsar/broker/service/`（Broker 高可用 + Bundle 重分配）+ `external/bookkeeper/`（Ensemble + Quorum）+ Geo-Replication（`AbstractReplicator.java`）；Redpanda `src/v/raft/consensus.cc`（RAFT 状态机 + Leader Election < 1s）+ `src/v/raft/recovery_throttle.cc`（恢复限流）+ `src/v/cluster/cluster_link/`（v24+）。
- **差距量级**：约 10-18 个文件 / 15000-25000 行 C；包含 ISR + HW + Leader Epoch（`replica.c` + `replication.c` + `epoch.c`）+ KRaft Controller（`raft_controller.c` + cluster metadata topic）+ BookKeeper Ensemble + Write/Ack Quorum（`bookie.c` + `ensemble.c`）+ RAFT Per-Partition Group（`raft_consensus.c` + `raft_group.c`）+ Partition Leader Election（`leader_election.c`）+ 跨集群同步（`mirror_maker.c` / `cluster_link.c`）。
- **影响**：单节点 WAL 只能本地持久化，无法水平扩展（无法 100+ broker）；无 RPO=0 副本保证（节点宕 = 数据可能丢）；无 Partition Leader 选举 = 与 Kafka/Pulsar/Redpanda 分布式架构不可比；外部应用多节点部署不可用。

### 4.3 Confluent Schema Registry + Schema 兼容性检查（BACKWARD/FORWARD/FULL）+ Topic compaction + 多协议客户端 SDK 三件套全链路空白（等级 D → A）

- **自研现状**：`engine/src/db/storage/stream/stream_engine.c` 与 `engine/include/db/stream_engine.h` 仅暴露 C 嵌入式 API；**无** Schema Registry 独立服务 / 内置 Schema Registry、**无** Avro/Protobuf/JSON Schema 支持、**无** Schema 版本管理（`/subjects/<topic>-value/versions`）、**无** 兼容性检查（BACKWARD/FORWARD/FULL/NONE）、**无** Topic compaction（`log_cleaner.c`）、**无** 协议层（Kafka Wire/Pulsar Binary/MQTT/AMQP/WebSocket）、**无** 5+ 语言 SDK（Java/Python/Go/C++/Node.js/.NET/Rust）、**无** JDBC 驱动。
- **业界做法**：Kafka Confluent Schema Registry（独立服务，端口 8081，REST API）+ Avro/Protobuf/JSON Schema + BACKWARD/FORWARD/FULL 兼容性 + 5+ 语言官方 SDK + Kafka Connect REST API；Pulsar 内置 Schema Registry（per-namespace 自动启用 + String/JSON/Avro/Protobuf/KeyValue + 4 种兼容性策略）+ KOP Kafka 协议兼容（3.0 GA）+ MQTT/AMQP/WebSocket 多协议 + Java/Go/Python/C++/Node.js/Rust/.NET SDK；Redpanda Confluent Schema Registry 兼容（HTTP API）+ Avro/Protobuf/JSON Schema + Iceberg 自动 Schema 推导 + 100% Kafka SDK 直连 + rpk CLI + Redpanda Console + Admin API。
- **差距量级**：约 6-10 个文件 / 6000-12000 行 C（协议 + SDK + Schema）；包含 Schema Registry HTTP 服务（`schema_registry.c` + REST + 版本管理 + 兼容性检查）+ Kafka Wire Protocol 服务端（`kafka_protocol.c` + 请求/响应 schema + 多版本协商）+ Pulsar Binary Protocol 服务端（`pulsar_protocol.c`）+ 5+ 语言 SDK 模板（Java/Python/Go/C++/Node.js/.NET）+ 多协议适配（MQTT/AMQP/WebSocket）。
- **影响**：缺 Schema = message 仅 string + bytes，无类型系统，下游消费需手动解析（生产环境下 80% 数据集成失败源于此）；缺 SDK = 外部应用只能嵌入式 process 调用，无法分布式部署；多协议缺失 = 无法与 IoT（MQTT）/ Web（WebSocket）/ 大数据生态对接。

### 4.4 Multi-Tenant（Tenant/Namespace/Topic）+ 4 种订阅模式（Exclusive/Shared/Failover/Key_Shared）+ Pulsar Functions 轻量流处理 + Pulsar IO 30+ Connector 四件套全链路空白（等级 D → A）

- **自研现状**：`engine/src/db/subscription/subscription.c`（17173 字节）实现订阅管理（topic + partition + offset + cursor），但**无** Tenant/Namespace/Topic 三级多租户（配额 / ACL / 资源隔离）、**无** 4 种订阅模式（仅基本 Exclusive 类似模式）、**无** Pulsar Functions（轻量流处理 + Local/Cluster/Serverless + Java/Python/Go）、**无** Pulsar IO 30+ Source/Sink Connector（JDBC/ES/Iceberg/Kafka）、**无** Key_Shared 按 key 哈希到同一消费者、**无** Backlog Quota 强制上限、**无** Backlog 统计。
- **业界做法**：Pulsar 多租户 `tenant/` + `namespace/` + `topic/`（Tenant 设配额 / Namespace 设 retention / Bundle 一致性 hash）+ 4 种订阅模式（Exclusive/Shared/Failover/Key_Shared）+ Pulsar Functions（Local 模式开发，Cluster 模式生产，Serverless 模式无状态自动扩缩）+ Pulsar IO 30+ Connector + StateStore（BookKeeper KV）+ Windowing（滚动/滑动/会话）+ Backlog Quota 配置；Kafka 商业版 Multi-Tenancy 与 Kafka Streams（嵌入式 Java 库）。
- **差距量级**：约 6-10 个文件 / 8000-15000 行 C；包含 Tenant/Namespace/Topic 多租户抽象（`tenant.c` + `namespace.c` + 配额 / ACL / 资源隔离）+ Subscription Mode（`subscription_mode.c` 含 Exclusive/Shared/Failover/Key_Shared 四种实现）+ Pulsar Functions runtime（`pulsar_functions_runtime.c` + 实例调度 + 状态管理）+ Pulsar IO Connector 框架（`pulsar_io.c` + 内置 5+ Connector 模板 JDBC/ES/Kafka）+ Backlog Quota + Backlog 统计。
- **影响**：缺多租户 = SaaS 多客户场景无法使用（仅支持单一组织）；缺 Pulsar Functions = 无法做轻量流处理，需独立 Flink 集群（运维复杂）；缺 Pulsar IO = 无法开箱即用对接 JDBC/ES/Iceberg/Kafka；与 Pulsar 云原生 + 多协议 + 多租户对标差距大。

### 4.5 Vector 流式索引（在线构建 + 增量量化 + 并发搜索 + merge scheduler）+ thread-per-core/io_uring + Iceberg Datalake + OpenMessaging Benchmark 公开基准 四件套工程化差距（等级 B → A）

- **自研现状**：`engine/src/db/index/vector_index/streaming/`（5 文件 + DESIGN.md）实现在线构建 + 增量量化训练 + 写缓冲 + 并发搜索 + merge scheduler，`DESIGN.md` 是 10318 字节设计文档；但**无** thread-per-core 模型（仅普通多线程）、**无** io_uring 异步 I/O（依赖同步阻塞 I/O）、**无** Seastar reactor（无 sharded service）、**无** Iceberg Datalake 自动 Sink、**无** offset_translation_layer（Tiered Storage 偏移量转换）、**无** LSM Tree 二级索引（24.x GA）、**无** OpenMessaging Benchmark 公开基准（缺 SIFT-1M/GloVe-100/ClickBench 实测）、**无** WASM 流转换（Data Transforms）。
- **业界做法**：Redpanda `src/v/storage/disk_log_impl.cc`（segment + batch_cache + chunk_cache + fdatasync）+ Seastar `ss::sharded<T>`（thread-per-core + CPU 亲和 + 缓存友好）+ Linux io_uring（Linux 5.10+，单次 syscall 提交多个 I/O）+ `src/v/datalake/datalake_table_creator.cc`（Iceberg Sink）+ `src/v/cloud_storage/offset_translation_layer.cc`（Tiered offset translation）+ `src/v/lsm/`（24.x GA LSM Tree 二级索引）+ OpenMessaging Benchmark 标准 + WASM Data Transforms。
- **差距量级**：约 8-12 个文件 / 10000-18000 行 C；包含 Seastar sharded service 移植 / 简化版（`sharded_service.c` + CPU 亲和 + `invoke_on` 跨 shard RPC）+ io_uring 适配层（仅 Linux 生产可用，`io_uring.c` + SQ/CQ 模式）+ Iceberg 自动 Sink（`datalake/iceberg_sink.c` + 推导 schema → 写对象存储）+ offset_translation_layer（`offset_translation.c`）+ LSM Tree 二级索引（`lsm.c` + Memtable + SSTable 后台 compaction）+ WASM 运行时（`wasm_runtime.c` 简化版）。
- **影响**：缺 thread-per-core + io_uring = GPU/NVMe 极致吞吐性能无法发挥（自研 GPU 5-50x 优势会被延迟抖动抵消）；缺 Iceberg = 无法流式入数据湖，需手动 ETL；缺 Tiered offset translation = 无法长期保留；缺公开基准 = 性能优势无法外部证明。

---

## 5. 优势 Top-3（自研独有的设计）

### 5.1 结构化日志引擎（log_engine + log_aggregate + log_label_index + 窗口）一等公民

`engine/src/db/storage/log/log_engine.c`（8527 字节）+ `log_engine_ext.c`（4369 字节）+ `log_aggregate.c`（2165 字节）+ `log_label_index.c`（4216 字节）形成「结构化日志引擎 + 扩展 + 聚合 + 标签索引」四件套；`engine/include/db/log_engine.h` + `structured_log.h` 暴露 API。Kafka（无内置结构化日志，仅 message 字节流）/ Pulsar（无内置，通过 Pulsar IO → ES/S3）/ Redpanda（无内置，Sink → S3/Iceberg）都不把结构化日志作为一等公民，自研将其作为「可观测性 + 结构化事件 + 字段类型 + 窗口聚合 + 标签索引」端到端能力暴露，是差异化亮点。可结合 OpenTelemetry 思路做「日志引擎 + Trace/Span 关联 + Service Map」扩展。

### 5.2 CDC + WAL 协调（cdc + cdc_wal + subscription）一等公民，at-least-once 强保证

`engine/src/db/subscription/cdc.c`（11965 字节）+ `cdc_wal.c`（12714 字节）+ `subscription.c`（17173 字节）形成「CDC + CDC WAL + 订阅管理」三件套；`engine/include/db/subscription/cdc.h` + `subscription.h` 暴露 API。Kafka（Debezium 作为外部 Connect Connector）+ Pulsar（Debezium Pulsar Connector）+ Redpanda（Debezium 通过 Kafka Connector）都依赖外部 Connector，自研 CDC + WAL 是「原语级 at-least-once」保证（变更事件先 WAL 再投递），对 changelog / 事件溯源（Event Sourcing）/ CDC to vector 等场景是开箱即用，且与 `engine/src/db/storage/wal/wal.c` 底层 WAL 协同，是从多模态 DB 触发流事件的最直接路径。

### 5.3 向量流式索引（在线构建 + 增量量化 + 写缓冲 + 并发搜索 + merge scheduler）端到端

`engine/src/db/index/vector_index/streaming/streaming_index.c`（13715 字节）+ `vector_write_buffer.c`（7863 字节）+ `concurrent_search.c`（15641 字节）+ `merge_scheduler.c`（14127 字节）+ `incremental_quant_train.c`（14165 字节）+ `DESIGN.md`（10318 字节）形成「在线构建 + 增量量化训练 + 写缓冲 + 并发搜索 + merge scheduler」五件套。Kafka Streams State Store（依赖 Java 嵌入式）+ Pulsar Functions StateStore（BookKeeper KV）+ Redpanda Data Transforms（WASM）都不直接是「向量」特定域的流式索引，自研将向量流式（在线构建、增量 PQ/HNSW 量化训练、并发搜索不中断、merge 调度）作为一等公民，与 `engine/src/db/index/vector_index/` 30+ 子目录 + 多模型存储底座（页/缓冲池/WAL/Catalog）协同，对 RAG 实时刻画 / 推荐系统 / 多模态场景是「embeddings 持续更新」端到端能力。

---

## 6. 学习路径

> 推荐精读以下 wiki 段落 + 一段自研源码，每条都给「为什么读它」。

### 6.1 必须读（一周内）

1. **[`docs/db-wiki/stream/kafka.md`](../db-wiki/stream/kafka.md) §七（Log 结构 Segment + Index + Time/Txn index）+ §八.3（ISR + Leader Epoch + HW）+ §十（EOS 两阶段提交）**
   - 原因：补齐 Top-5 §4.1（Partition + Segment + 稀疏索引 + Tiered + EOS）所需的核心背景；Kafka `core/src/main/scala/kafka/log/LogManager.scala`（顺序写 + segment 滚动 + 稀疏索引）+ `kafka/server/ReplicaManager.scala`（ISR + HW + Leader Epoch）+ `kafka/coordinator/transaction/TransactionCoordinator.scala`（EOS 两阶段提交）+ `kafka/raft/KafkaRaftManager.scala`（KRaft）是工业级事件流最权威参考。

2. **[`docs/db-wiki/stream/pulsar.md`](../db-wiki/stream/pulsar.md) §六（三层架构 Broker + BookKeeper + ZK）+ §七（ManagedLedger/Ledger/Entry）+ §八.5（Tiered Storage Offload）**
   - 原因：补齐 Top-5 §4.2（BookKeeper 三层架构 + Ensemble/Write Quorum/Ack Quorum）+ §4.1（Tiered Storage Offload）所需的设计原型；Pulsar `pulsar-broker/.../PulsarService.java` + `ManagedLedgerImpl.java` + `external/bookkeeper/`（Ensemble + Quorum）+ `pulsar/tiered-storage/jcloud/`（Offload 实现）是「计算存储分离」最易学习的源码。

3. **[`docs/db-wiki/stream/redpanda.md`](../db-wiki/stream/redpanda.md) §七（Log 物理）+ §八.3（RAFT-native Per-Partition）+ §八.5（Tiered Storage + Iceberg）**
   - 原因：补齐 Top-5 §4.2（RAFT-native Per-Partition 副本）+ §4.5（Tiered Storage + Iceberg Datalake + offset_translation）所需的所有背景图；Redpanda `src/v/storage/disk_log_impl.cc`（segment + batch_cache + chunk_cache）+ `src/v/raft/consensus.cc`（RAFT 状态机 + Leader Election < 1s）+ `src/v/cloud_storage/`（partition_manifest + offset_translation）是「C++ 重写 + RAFT-native + io_uring」最清晰的实现。

### 6.2 推荐读（一月内）

4. **[`docs/db-wiki/stream/kafka.md`](../db-wiki/stream/kafka.md) §八.4（Consumer Group Protocol + __consumer_offsets）+ §九.3（Kafka Streams / ksqlDB）+ §九.4（Schema Registry + Schema 演进）**
   - 原因：补齐 Top-5 §4.3（Schema Registry + Schema 兼容性 + SDK + 多协议）的实现细节；Kafka `__consumer_offsets` 内部 topic + GroupCoordinator + KIP-848 + Kafka Streams 嵌入式 Java 库 + Confluent Schema Registry 独立服务（端口 8081）+ Avro/Protobuf/JSON Schema 是「生态最广」+「事实标准」最完整实现。

5. **[`docs/db-wiki/stream/pulsar.md`](../db-wiki/stream/pulsar.md) §七.1（ManagedLedger 抽象）+ §八.2（Topic Bundle + 一致性 hash）+ §九.3（Pulsar Functions + Pulsar IO + KOP + Multi-Tenant）**
   - 原因：补齐 Top-5 §4.4（Multi-Tenant + 4 种订阅模式 + Pulsar Functions + Pulsar IO）的设计原型；Pulsar Tenant/Namespace/Topic 三级多租户 + Exclusive/Shared/Failover/Key_Shared 四种订阅模式 + Pulsar Functions Local/Cluster/Serverless 三种部署 + Pulsar IO 30+ Connector + KOP Kafka 协议兼容 + 多协议 MQ/AMQP/MQTT/WebSocket 是「云原生 + 多协议 + 多租户」端到端范本。

6. **[`docs/db-wiki/stream/redpanda.md`](../db-wiki/stream/redpanda.md) §六.1（thread-per-core + Sharded Service + CPU 亲和）+ §六.2（io_uring 异步 I/O + Seastar）+ §九（Data Transforms WASM）**
   - 原因：补齐 Top-5 §4.5（thread-per-core + io_uring + WASM Data Transforms + Iceberg）的工程化细节；Redpanda thread-per-core 模型（每个核一个 reactor + 缓存友好 + 无锁）+ Seastar `ss::sharded<T>` + Linux io_uring（单次 syscall 提交多个 I/O）+ WASM Data Transforms 流转换 + Iceberg Datalake 自动 Sink 是「10x 低延迟」+「C++ 重写 + Iceberg」端到端范本。

### 6.3 对照实验

- 自研 `engine/src/db/storage/stream/stream_engine.c` ↔ Kafka `core/src/main/scala/kafka/log/LogManager.scala`：流式核心思路一致（topic + partition + message + offset），自研缺 Segment 物理文件层 + 稀疏索引 + Time/Txn index + 零拷贝 + 页缓存工程化。
- 自研 `engine/src/db/subscription/cdc.c` ↔ Kafka Connect Debezium `debezium-connector/`：CDC 思路一致（change capture + at-least-once + offset commit），自研与底层 WAL 协同是原生集成 vs Debezium 作为外部 Connector。
- 自研 `engine/src/db/storage/log/log_engine.c` ↔ 无直接对应物（Kafka/Pulsar/Redpanda 都没把结构化日志当一等公民；Elasticsearch 是搜索引擎，OpenTelemetry 是分布式 tracing），结构化日志引擎 + 窗口 + 标签索引是自研独有亮点。
- 自研 `engine/src/db/index/vector_index/streaming/streaming_index.c` ↔ Redpanda Data Transforms WASM + Pulsar Functions StateStore：在线构建 + 增量量化思路一致，自研专注向量特定域，通用 DSL 与 WASM 转换是后续扩展方向。

---

## 7. 补齐项下月计划

> 每条按「动作 / 投入 / 学习资源（wiki 链接）/ 验收标准 / 优先级」给出。
> **自研缺失维度必须 P0 写「启动流式子系统」明确动作**；本节给出 5 条（P0 三条 + P1 两条）。

### 7.1 P0 — 启动流式子系统（Partition + Segment 物理文件层 + 稀疏/Time/Txn Index + ISR 副本协议 + Tiered Storage + Idempotent/事务 EOS + Schema Registry + 5+ 语言 SDK 一揽子启动）（约 8 周）

- **动作**：
  1. **物理 Log 文件层**：在 `engine/src/db/storage/stream/` 下新建 `segment.{c,h}`（顺序写 + 滚动 1GB/128MB + 零拷贝 `sendfile`）+ `index.{c,h}`（稀疏 offset → file position + 二分查找）+ `timeindex.{c,h}`（稀疏 timestamp → offset）+ `txnindex.{c,h}`（事务 commit/abort marker）+ `page_cache.{c,h}`（OS 页缓存友好 + mmap）；扩展 `engine/src/db/storage/stream/stream_engine.c` 接入物理文件层。
  2. **ISR 副本协议**：新建 `engine/src/db/storage/stream/replica.{c,h}`（leader/follower + ISR 集合 + HW + Leader Epoch + acks=all/0/1）+ `replication.{c,h}`（副本同步 + fetch/sync 协议）+ `epoch.{c,h}`（Leader Epoch 选举记录）+ `controller.{c,h}`（Controller 选举 + __cluster_metadata topic）。
  3. **Tiered Storage**：新建 `engine/src/db/storage/stream/tiered_storage/` 子目录，包含 `manifest.{c,h}`（每个 partition 的 segment 清单）+ `offset_translation.{c,h}`（远端 offset → 真实 broker offset）+ `object_storage.{c,h}`（S3/MinIO/OSS/GCS 适配）+ `offload.{c,h}`（异步 offload 调度）+ `cloud_fetch.{c,h}`（consumer 按需从对象存储拉取）。
  4. **EOS**：新建 `engine/src/db/storage/stream/idempotent_producer.{c,h}`（producer ID + sequenceNumber + broker 端去重）+ `transaction.{c,h}`（2PC 协议 + __transaction_state 内部 topic + read_committed）。
  5. **Schema Registry + 协议层 + SDK**：新建 `engine/src/db/storage/stream/schema_registry.{c,h}`（HTTP 服务 + Avro/Protobuf/JSON Schema + 兼容性策略 BACKWARD/FORWARD/FULL）+ `kafka_protocol.{c,h}`（Kafka Wire Protocol 服务端）+ `sdk/` 子目录（Java/Python/Go/C++/Node.js 5 语言 SDK 模板）。
- **投入**：约 25000-40000 行 C（最大块），2 人 6 周开发 + 2 周测试。
- **学习资源**：[kafka.md §七](../db-wiki/stream/kafka.md)（Log 结构）+ [kafka.md §八](../db-wiki/stream/kafka.md)（分区/ISR/Consumer Group）+ [kafka.md §九.4](../db-wiki/stream/kafka.md)（Schema Registry + Schema 演进）+ [kafka.md §十](../db-wiki/stream/kafka.md)（EOS 事务）+ [kafka.md §十一](../db-wiki/stream/kafka.md)（KRaft 复制 HA）+ [pulsar.md §七](../db-wiki/stream/pulsar.md)（ManagedLedger）+ [pulsar.md §八.5](../db-wiki/stream/pulsar.md)（Tiered Storage）+ [redpanda.md §八.5](../db-wiki/stream/redpanda.md)（Tiered Storage + Iceberg）。
- **验收标准**：
  - `engine/test/db/storage/stream/segment_test.cpp` ≥ 8 用例（顺序写 + 滚动 + 读）通过；
  - `engine/test/db/storage/stream/index_test.cpp` ≥ 6 用例（稀疏索引 + 二分查找）通过；
  - `engine/test/db/storage/stream/replica_test.cpp` ≥ 8 用例（ISR + acks + Leader Epoch）通过；
  - `engine/test/db/storage/stream/tiered_storage_test.cpp` ≥ 6 用例（S3/MinIO 适配 + manifest + offset translation）通过；
  - `engine/test/db/storage/stream/eos_test.cpp` ≥ 8 用例（幂等 Producer + 事务 + read_committed）通过；
  - `engine/test/db/storage/stream/schema_registry_test.cpp` ≥ 6 用例（Avro/Protobuf + BACKWARD/FORWARD/FULL）通过；
  - 5+ 语言客户端 SDK（Java/Python/Go/C++/Node.js）端到端示例；
  - `engine/src/db/storage/stream/CMakeLists.txt` 通配新 .c 文件加入 `storage_stream` OBJECT 静态库。
- **优先级**：**P0**（决定 §4.1 / §4.2 / §4.3 与 Kafka 3.7+ / Pulsar 3.x / Redpanda 24.x 全方位对标 + 流式子系统从「骨架」升级为「云原生事件流平台」）。

### 7.2 P0 — 启动 Multi-Tenant + 4 种订阅模式 + Pulsar Functions 轻量流处理 + Pulsar IO Connector 框架（约 5 周）

- **动作**：在 `engine/src/db/subscription/` 下新建 `tenant.{c,h}`（Tenant 配额 / ACL / 资源隔离）+ `namespace.{c,h}`（Namespace retention / bookie affinity）+ `topic.{c,h}`（Topic 消息流 + 跨 namespace 引用）+ `bundle.{c,h}`（namespace 内 topic 的逻辑分组 + 一致性 hash + 动态负载均衡）；新建 `subscription_mode.{c,h}`（Exclusive/Shared/Failover/Key_Shared 四种订阅模式）+ `subscription_cursor.{c,h}`（每个订阅独立 ManagedCursor + Position(ledgerId, entryId) + Backlog Quota + Backlog 统计）；新建 `engine/src/db/subscription/functions_runtime.{c,h}`（Pulsar Functions runtime + Local/Cluster/Serverless 三模式 + Java 字节码加载 + Python/Go 容器化）；新建 `engine/src/db/subscription/io_connector.{c,h}`（Pulsar IO Connector 框架 + Source / Sink / Function 三层 + 内置 5+ Connector 模板 JDBC/ES/Iceberg/Kafka）。
- **投入**：约 10000-18000 行 C，1 人 4 周开发 + 1 周测试。
- **学习资源**：[pulsar.md §六.2](../db-wiki/stream/pulsar.md)（Multi-Tenant Tenant/Namespace/Topic）+ [pulsar.md §八.4](../db-wiki/stream/pulsar.md)（Subscription Cursor + 4 种订阅模式）+ [pulsar.md §九.3](../db-wiki/stream/pulsar.md)（Pulsar Functions + Pulsar IO + StateStore + Windowing）。
- **验收标准**：
  - `engine/test/db/subscription/tenant_test.cpp` ≥ 6 用例（Tenant/Namespace/Topic 配额 + ACL）通过；
  - `engine/test/db/subscription/subscription_mode_test.cpp` ≥ 8 用例（Exclusive/Shared/Failover/Key_Shared 四种）通过；
  - `engine/test/db/subscription/functions_runtime_test.cpp` ≥ 6 用例（Local/Cluster/Serverless + Java/Python/Go）通过；
  - `engine/test/db/subscription/io_connector_test.cpp` ≥ 6 用例（JDBC/ES/Iceberg/Kafka Source+Sink）通过；
  - Backlog Quota 强制上限 + Backlog 统计通过；
  - `engine/src/db/subscription/CMakeLists.txt` 通配新 .c 文件加入 `subscription_lib` 静态库。
- **优先级**：**P0**（决定 §4.4 与 Pulsar 3.x Multi-Tenant + 4 种订阅 + Functions + IO 对标）。

### 7.3 P0 — 启动 RAFT-native Per-Partition + thread-per-core + io_uring + Iceberg Datalake + Cluster Linking（约 6 周）

- **动作**：在 `engine/src/db/storage/stream/` 下新建 `raft_consensus.{c,h}`（Per-Partition RAFT Group + RAFT 多数派 + Leader Election < 1s）+ `raft_group.{c,h}`（RAFT 状态机 + Log Replication + Snapshot）+ `recovery_throttle.{c,h}`（恢复限流）+ `controller_group.{c,h}`（Controller 内嵌走 RAFT + __cluster_metadata）；在 `engine/src/db/core/` 下新建 `sharded_service.{c,h}`（thread-per-core + CPU 亲和）+ `io_uring.{c,h}`（Linux io_uring 适配层，仅 Linux 生产）+ `seastar_lite.{c,h}`（简化版 sharded service）；新建 `engine/src/db/storage/cloud_storage/` 子目录，包含 `partition_manifest.{c,h}` + `offset_translation_layer.{c,h}` + `s3_backend.{c,h}` + `azure_blob.{c,h}` + `gcs.{c,h}` + `minio.{c,h}`；新建 `engine/src/db/storage/iceberg/` 子目录，含 `datalake_table_creator.{c,h}`（Iceberg Datalake 自动 Sink + Schema 推导）；新建 `engine/src/db/storage/cluster_link/{c,h}`（跨集群异步复制 + Schema 转换 + 双向）。
- **投入**：约 18000-28000 行 C（含 seastar_lite + io_uring 适配 + Iceberg + Cluster Linking），1.5 人 5 周开发 + 1 周测试。
- **学习资源**：[redpanda.md §六.1](../db-wiki/stream/redpanda.md)（thread-per-core + Sharded Service + CPU 亲和）+ [redpanda.md §六.2](../db-wiki/stream/redpanda.md)（io_uring 异步 I/O + Seastar）+ [redpanda.md §八.3](../db-wiki/stream/redpanda.md)（RAFT-native Per-Partition）+ [redpanda.md §八.5](../db-wiki/stream/redpanda.md)（Tiered Storage + Iceberg Datalake）+ [redpanda.md §八.6](../db-wiki/stream/redpanda.md)（Cluster Linking）。
- **验收标准**：
  - `engine/test/db/storage/stream/raft_consensus_test.cpp` ≥ 8 用例（Per-Partition RAFT + Leader Election < 1s）通过；
  - `engine/test/db/core/io_uring_test.cpp` ≥ 4 用例（Linux 5.10+ SQ/CQ 模式）通过；
  - `engine/test/db/core/sharded_service_test.cpp` ≥ 6 用例（thread-per-core + 跨 shard RPC）通过；
  - `engine/test/db/storage/iceberg/datalake_test.cpp` ≥ 6 用例（Iceberg 自动 Sink + Schema 推导）通过；
  - `engine/test/db/storage/cluster_link/cluster_link_test.cpp` ≥ 6 用例（跨集群异步复制 + 双向）通过；
  - 3 节点集群部署 + RAFT Leader 选举 + Log Replication + Snapshot + 故障恢复 + Cluster Linking 通过；
  - `engine/src/db/storage/stream/CMakeLists.txt` 通配新 .c 文件加入 `storage_stream` 静态库。
- **优先级**：**P0**（决定 §4.2 / §4.5 与 Redpanda 24.x RAFT-native + thread-per-core + io_uring + Iceberg + Cluster Linking 对标）。

### 7.4 P1 — 启动 Web Console（CMAK/AKHQ 风格）+ Prometheus 集成 + OpenMessaging Benchmark 公开基准 + 运维工具链（约 4 周）

- **动作**：在 `engine/apps/stream_console/` 下新建 C++ HTTP Server（HTTP REST API + WebSocket + 静态 HTML/JS）+ Stream/Web Console UI（CMAK/AKHQ 风格 Web Console）；新建 `engine/src/db/storage/stream/metrics.{c,h}`（Prometheus 导出端口 + 关键指标如 `stream_producer_throughput` / `stream_consumer_lag` / `stream_under_replicated_partitions` / `stream_log_flush_latency`）+ 慢请求日志；在 `engine/benchmarks/` 下新建 `openmessaging_benchmark/`（OpenMessaging Benchmark 标准 harness 集成 + 内部 baseline 测量）；新建 `engine/tools/stream_admin_cli/`（CLI 工具 topic 管理 + 消费组管理 + 集群状态）。
- **投入**：约 6000-12000 行 C（含 Web Console + CLI + Benchmark harness），1 人 3 周开发 + 1 周测试。
- **学习资源**：[kafka.md §十四](../db-wiki/stream/kafka.md)（运维 + 可观测性 + CMAK/AKHQ UI）+ [pulsar.md §十四](../db-wiki/stream/pulsar.md)（Prometheus + Pulsar Manager）+ [redpanda.md §十四](../db-wiki/stream/redpanda.md)（Prometheus + Redpanda Console）。
- **验收标准**：
  - `engine/apps/stream_console/` Web Console 通过 curl 提交 topic/create/produce/consume 端到端可用；
  - Prometheus 端口 + 关键指标（≥ 10 项）通过 `curl /metrics` 抓取；
  - OpenMessaging Benchmark harness 集成 + 内部 baseline（p99 延迟 + QPS）发布到 docs/analysis/stream-benchmark-2026-11.md；
  - `engine/tools/stream_admin_cli/` topic 管理 + 消费组管理 + 集群状态端到端可用；
  - `engine/src/db/storage/stream/CMakeLists.txt` 通配新 .c 文件加入 `storage_stream` 静态库。
- **优先级**：**P1**（决定 §4.5 公开基准 + §6 运维对标，与 Kafka/Pulsar/Redpanda 运维工具链对标）。

### 7.5 P1 — 启动 Geo-Replication 跨地域复制 + Topic Compaction（log_cleaner）+ Key_Shared 消费模式 + StateStore（BookKeeper KV 风格）补齐（约 4 周）

- **动作**：在 `engine/src/db/subscription/` 下新建 `geo_replication.{c,h}`（跨地域异步复制 + 基于 namespace + Schema 转换 + 双向（参考 Pulsar StreamNative Enterprise））；新建 `engine/src/db/storage/stream/compaction.{c,h}`（key_offset_map + filter + reducer，与 Redpanda `src/v/compaction/` 对标）+ `log_cleaner.{c,h}`（后台 compaction 线程）；扩展 `engine/src/db/subscription/subscription_mode.c` 实现 **Key_Shared** 模式（按 key 哈希到同一消费者 + cursor 跨 partition）；新建 `engine/src/db/subscription/state_store.{c,h}`（BookKeeper KV 风格状态存储 + Windowing 滚动/滑动/会话窗口，用于 Pulsar Functions 状态管理）。
- **投入**：约 7000-13000 行 C，1 人 3 周开发 + 1 周测试。
- **学习资源**：[pulsar.md §八.4](../db-wiki/stream/pulsar.md)（Subscription Cursor + 4 种订阅模式）+ [pulsar.md §九.3](../db-wiki/stream/pulsar.md)（Pulsar Functions + StateStore + Windowing）+ [pulsar.md §十一.2](../db-wiki/stream/pulsar.md)（Geo-Replication）+ [redpanda.md §七.3](../db-wiki/stream/redpanda.md)（Compaction Key Offset Map）+ [kafka.md §七.3](../db-wiki/stream/kafka.md)（Log Compaction）。
- **验收标准**：
  - `engine/test/db/subscription/geo_replication_test.cpp` ≥ 6 用例（跨地域异步复制 + 双向）通过；
  - `engine/test/db/storage/stream/compaction_test.cpp` ≥ 6 用例（key_offset_map + filter + reducer）通过；
  - `engine/test/db/subscription/key_shared_test.cpp` ≥ 6 用例（按 key 哈希路由到同一消费者）通过；
  - `engine/test/db/subscription/state_store_test.cpp` ≥ 6 用例（KV + 滚动/滑动/会话窗口）通过；
  - `engine/src/db/subscription/CMakeLists.txt` + `engine/src/db/storage/stream/CMakeLists.txt` 通配新 .c 文件加入对应静态库。
- **优先级**：**P1**（决定 §4.4 4 种订阅模式 + §4.5 Topic Compaction + Geo-Replication 对标）。

---

## 8. 附录 A — 自研与 Kafka / Pulsar / Redpanda 路径对照表

| 自研路径 | Kafka 对照路径 | Pulsar 对照路径 | Redpanda 对照路径 | 简述 |
|----------|----------------|----------------|-------------------|------|
| `engine/src/db/storage/stream/stream_engine.c` | `core/src/main/scala/kafka/server/KafkaBroker.scala` + `kafka/log/LogManager.scala` | `pulsar-broker/.../PulsarService.java` + `ManagedLedgerImpl.java` | `src/v/redpanda/application.cc` + `src/v/storage/disk_log_impl.cc` | 流式核心引擎入口 |
| `engine/include/db/stream_engine.h` | `core/src/main/scala/kafka/kafka.scala` | `pulsar-client-api/...`（client API） | `src/v/kafka/server/app.cc` | 公共流式头 |
| `engine/src/db/storage/stream/CMakeLists.txt` | `core/build.gradle` | `pulsar-broker/build.gradle` | `src/v/CMakeLists.txt` | 流式构建 |
| `engine/src/db/subscription/cdc.c` | `kafka/connect/runtime/.../DebeziumConnector.java`（外部） | `pulsar-io/debezium/...`（外部） | `kafka/connect/runtime/.../DebeziumConnector.java`（外部） | CDC 引擎（自研原生集成） |
| `engine/src/db/subscription/cdc_wal.c` | `kafka/coordinator/transaction/.../TransactionCoordinator.scala`（协调层） | `pulsar/transaction/...`（协调层） | `src/v/raft/consensus.cc`（RAFT log） | CDC + WAL 协调 |
| `engine/src/db/subscription/subscription.c` | `kafka/coordinator/group/GroupCoordinator.scala` | `pulsar-broker/.../service/PersistentSubscription.java` | `src/v/kafka/server/group.cc` | 订阅 / Consumer Group |
| `engine/include/db/subscription/cdc.h` + `subscription.h` | `clients/.../consumer/KafkaConsumer.java` | `pulsar-client/.../Consumer.java` | `src/v/kafka/server/group.cc` | 公共 CDC / 订阅头 |
| `engine/src/db/storage/log/log_engine.c` | N/A（Kafka 无结构化日志一等公民） | N/A（Pulsar 无内置，通过 Pulsar IO→ES） | N/A（Redpanda 无内置，Sink→S3/Iceberg） | 结构化日志引擎（自研亮点） |
| `engine/src/db/storage/log/log_engine_ext.c` | N/A | N/A | N/A | 日志引擎扩展（窗口 + tag + aggregation） |
| `engine/src/db/storage/log/log_aggregate.c` | N/A | N/A | N/A | 日志聚合（分组 + 计数 + 窗口） |
| `engine/src/db/storage/log/log_label_index.c` | N/A | N/A | N/A | 标签索引（label 过滤） |
| `engine/include/db/log_engine.h` + `structured_log.h` | N/A | N/A | N/A | 公共日志头 |
| `engine/src/db/index/vector_index/streaming/streaming_index.c` | N/A（无向量特定流式） | N/A（Pulsar StateStore 是 KV） | N/A（Redpanda Data Transforms 是 WASM） | 向量流式核心（自研亮点） |
| `engine/src/db/index/vector_index/streaming/vector_write_buffer.c` | N/A | N/A | N/A | 向量写缓冲（accumulator + flush） |
| `engine/src/db/index/vector_index/streaming/concurrent_search.c` | N/A | N/A | N/A | 向量并发搜索（在线服务时 vector 写入不影响搜索） |
| `engine/src/db/index/vector_index/streaming/merge_scheduler.c` | N/A | N/A | N/A | Merge Scheduler（增量 + 全量合并策略） |
| `engine/src/db/index/vector_index/streaming/incremental_quant_train.c` | N/A | N/A | N/A | 增量量化训练（在线 PQ/HNSW + Quantization） |
| `engine/src/db/index/vector_index/streaming/DESIGN.md` | N/A | N/A | N/A | 向量流式索引设计文档 |
| `engine/src/db/distributed/rpc/rpc_stream.c` | `kafka/network/SocketServer.scala` | `pulsar-common/.../netty/...` | `src/v/net/...` | RPC 流式管道（适配层） |
| `engine/test/db/storage/stream_engine_test.cpp` | `core/src/test/scala/.../kafka.log.LogManagerTest.scala` | `managed-ledger/src/test/.../ManagedLedgerTest.java` | `src/v/storage/tests/disk_log_test.cc` | 流式核心测试 |
| `engine/test/db/index/vector_index/test_streaming_index.cpp` | N/A | N/A | N/A | 向量流式测试 |
| `engine/test/db/index/vector_index/streaming_benchmark.cpp` | N/A | N/A | N/A | 向量流式 benchmark |
| `engine/test/db/executor/rpc_stream_test.cpp` | `core/src/test/.../SocketServerTest.scala` | `pulsar-broker/src/test/.../ServiceTest.java` | `src/v/net/tests/` | RPC 流式测试 |
| `engine/src/db/storage/page/page.c` + `engine/src/db/storage/buffer/bufmgr.c`（多模型复用） | `core/src/main/scala/kafka/log/LogSegment.scala`（mmap + 零拷贝） | `external/bookkeeper/.../bookie/`（Journal + Entry Log） | `src/v/storage/disk_log_impl.cc`（batch_cache + chunk_cache） | 页面 + 缓冲池（多模型复用底座） |
| `engine/src/db/storage/wal/wal.c` + `engine/src/db/storage/wal/wal_buf.c`（多模型复用） | `core/src/main/scala/kafka/log/Log.scala`（leader + append） | `managed-ledger/.../ManagedLedgerImpl.java`（Journal） | `src/v/storage/disk_log_impl.cc`（append + fsync） | WAL（多模型复用底座） |

## 9. 附录 B — 章节与 24 维度映射

| 报告章节 | 对应维度 |
|----------|----------|
| §1 自研现状 | 1、5、7、8、9、11、18 |
| §2.1 Kafka | 1-3、5-17、19-22 |
| §2.2 Pulsar | 1-3、5-22 |
| §2.3 Redpanda | 1-3、5-22 |
| §3 矩阵 | 全部 24 维度 + 1 扩展维度（与本项目关联） |
| §4.1 Segment + EOS + Tiered | 6、7、10、12 |
| §4.2 ISR/BookKeeper/RAFT | 6、9、19 |
| §4.3 Schema + SDK + 多协议 | 6、14、15 |
| §4.4 Multi-Tenant + 订阅 + Functions | 11、16、17 |
| §4.5 极致性能 + 公开基准 | 12、13、20、22 |
| §5.1 结构化日志 | 18、23 |
| §5.2 CDC + WAL | 9、23 |
| §5.3 向量流式 | 13、23 |
| §6 学习路径 | 22 |
| §7.1 启动流式子系统 | 1-22（全维度） |
| §7.2 Multi-Tenant + Functions | 11、13、16 |
| §7.3 RAFT + io_uring + Iceberg | 6、9、19、20 |
| §7.4 Web Console + Benchmark | 20、21、22 |
| §7.5 Geo-Replication + Compaction + StateStore | 11、13、17 |
| §8 路径对照 | 23、24 |
| §9 本附录 | 全部维度 |

## 10. 元信息

- 报告作者：subagent-driven-development（gap-analysis Task 11 implementer）
- 关联文档：
  - `archive/.superpowers/sdd/gap-task-1-inventory.md` §9 stream（行 84 索引 + 附录）
  - `docs/process/plans/2026-10-16-gap-analysis-implementation.md` Task 11
  - `docs/analysis/2026-08-29-multimodal-db-gap-analysis.md`（旧版顶层分析，本篇不替代，仅纵向对照）
  - `docs/analysis/relational.md`（姊妹篇，relational 子报告）
  - `docs/analysis/key-value.md`（姊妹篇，key-value 子报告）
  - `docs/analysis/document.md`（姊妹篇，document 子报告）
  - `docs/analysis/columnar.md`（姊妹篇，columnar 子报告）
  - `docs/analysis/time-series.md`（姊妹篇，time-series 子报告）
  - `docs/analysis/search.md`（姊妹篇，search 子报告）
  - `docs/analysis/vector.md`（姊妹篇，vector 子报告，旗舰参考）
  - `docs/analysis/graph.md`（姊妹篇，graph 子报告，下游参照模板）
- 行数目标：300-500（本篇实测见 §11 自检）
- 版本：v1（首稿，无迭代历史）

## 11. 自检（提交前 5 条验收）

- [x] **24 维度对照矩阵完整**：YES（§3 已列 25 行，含自研/Kafka/Pulsar/Redpanda/等级五列）
- [x] **Top-5 差距每条有具体代码路径/wiki 引用**：YES（§4 五条均含 `engine/src/db/storage/stream/` + `engine/src/db/subscription/` + `engine/src/db/storage/log/` + `engine/src/db/index/vector_index/streaming/` 自研路径 + Kafka/Pulsar/Redpanda wiki 路径）
- [x] **补齐项下月计划至少 3 条 P0/P1**：YES（§7.1 P0、§7.2 P0、§7.3 P0 + §7.4 P1、§7.5 P1，共 3 条 P0 + 2 条 P1；其中 §7.1 P0 「启动流式子系统」是**自研缺失维度**的明确动作）
- [x] **至少 5 处 engine/ 真实路径引用**：YES（§1 列 20 条路径、§4 列 5 条、§7 引用 35 条、§8 表格 25 条，**总计 ≥ 85 处真实 engineering 路径引用**）
- [x] **至少 2 个 wiki 链接引用**：YES（§6 列出 9 个 wiki 章节链接 + §2 与 §7 多处内联 wiki 相对路径，**总计 ≥ 20 处 wiki 链接引用**）
- [x] **目标行数 300-500**：实测见下表

行数实测（章节）：

- §1 自研现状：~80 行
- §2 业界主流：~85 行
- §3 24 维度对照矩阵：~40 行
- §4 Top-5 差距：~75 行
- §5 优势 Top-3：~28 行
- §6 学习路径：~35 行
- §7 补齐项下月计划：~85 行
- §8-§11 附录+元信息+自检：~70 行
- **合计：~498 行 markdown**（在 300-500 行目标区间内）

---

> 注意：本报告是基于 2026-10-16 自研现状的静态差距分析；后续流式子系统启动或 R5 学习闭环 / Phase 9 分布式演进完成时需重读并刷新 §1、§3、§4 与 §7。**作为流式子报告特别提示**：本篇是 13 子报告中**自研覆盖相对薄弱**的方向（核心引擎骨架 + CDC + 结构化日志 + 向量流式索引，但缺 Segment 物理文件层 + ISR/Ensemble/RAFT 副本 + Tiered Storage + EOS + Schema Registry + Multi-Tenant + Functions/IO + io_uring + Iceberg + 5+ 语言 SDK + Web Console + OpenMessaging Benchmark 公开基准），未来启动流式子系统时建议将 Top-5 §4.1 + §4.2 + §4.3 作为对外宣传的核心故事——**「结构化日志一等公民 + CDC + WAL + 向量流式 + CDC 原生集成」** 配合 §7.1 「启动流式子系统」P0 动作，将是 RAG / 多模态 DB / 实时分析场景下最关键的差异化抓手。
