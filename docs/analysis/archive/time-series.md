> ⚠️ **归档说明**：本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考。
> 当前主核是 `engine/db/`（自带 Makefile，不在 CMake 图内），见 [设计文档](../../superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md)。

# Gap Analysis 子报告 — Time-Series（时序数据库）

> 系列：自研多模态数据库 + RAG「自研 vs 业界」差距分析（13 篇子报告之五）
> 本篇：时序数据库引擎（time-series / TSM / 压缩 / 分区 / 保留 / 标签索引 / 连续聚合）
> 时间：2026-10-16
> 状态：初稿
> 上游依赖：`archive/.superpowers/sdd/gap-task-1-inventory.md` §5 time-series
> 对照 wiki：
> - `docs/db-wiki/time-series/influxdb.md`（InfluxDB v2.7 LTS，TSM1 + TSI1 + Flux/InfluxQL）
> - `docs/db-wiki/time-series/tdengine.md`（TDengine 3.4.x，TSDB 双层 + 多级存储 + 流计算 + 数据订阅）
> - `docs/db-wiki/time-series/questdb.md`（QuestDB v8.2+，Cairo 列存 + Griffin SQL + PG Wire Protocol）
> 自身参考：`2026-08-29-multimodal-db-gap-analysis.md`（旧版顶层报告，本篇不替代，仅作纵向对照）

## 摘要（TL;DR）

自研时序引擎已经具备**「时序存储 + 5 种压缩 + 分区 + 保留 + 段式存储 + 连续聚合 + 物化视图 + 标签/Tag 索引 + 时序 SQL 函数」**的端到端能力，覆盖度评级 **A**。与 InfluxDB 的差距集中在 **TSM1 引擎家族（Cache + WAL + Compactor 三件套）、TSI1 倒排索引（LogFile/IndexFile/SeriesFile 三层）、Gorilla XOR 浮点压缩（论文级）、InfluxQL 与 Line Protocol 双协议、`max-out-of-order-write-delay` 乱序写入容忍窗口**；与 TDengine 的差距集中在 **TSDB 双层架构（Last Cache 跳表 + Data File 三列存储）、多级存储（内存 → SSD → 对象存储自动迁移）、SMA 流式聚合、TQ 数据订阅（Kafka-like 替代）、Super Table 设备元数据模型、VNode 虚拟化**；与 QuestDB 的差距集中在 **Cairo 原生列存（按列文件 + PageFrame 4096 行）、Symbol 低基数优化 + Bitmap 索引（Roaring）、`SAMPLE BY` / `LATEST ON` / `ASOF JOIN` 时序 SQL 三件套、`designated timestamp` 强类型 + 分区裁剪、PostgreSQL Wire Protocol（完整兼容 psql/JDBC）+ InfluxDB Line Protocol 双协议入口**。

简言之：自研时序是「时序引擎全套 + 标签索引 + 连续聚合 + 列存特化，但无 TSM1 / 在内存 Cache + 乱序写入协议 + 流式订阅 + SQL 扩展 + 多级存储」。InfluxDB 是「时序事实标准 + TSM 引擎 + 完整生态」标杆，TDengine 是「IoT 端到端 + 多级存储 + 内置流计算」标杆，QuestDB 是「SQL 原生 + 列存 + 关系统一」标杆。扩展方向应以「补齐 TSM1 引擎家族（Cache + WAL + Compactor）+ Gorilla 浮点压缩 + 乱序写入容忍窗口 + InfluxQL/Flux SQL 扩展 + 数据订阅 TQ」为最高优先级，因为它直接决定了 `engine/src/db/storage/ts/` 能否从「嵌入式时序库」跨越到「工业级时序数据库」。

---

## 1. 自研现状

> 引用 `archive/.superpowers/sdd/gap-task-1-inventory.md` §5 time-series（行 42 索引 + 附录），并补充定位说明。

1. **时序引擎主体（覆盖度 A）**：`engine/src/db/storage/ts/ts_engine.c`（26346 字节，825 行）+ `engine/include/db/ts_engine.h`（166 行公共头）+ `engine/src/db/storage/ts/CMakeLists.txt`；定义 `ts_aggregate_func_t` 枚举（`AGG_SUM`/`AGG_AVG`/`AGG_MIN`/`AGG_MAX`/`AGG_COUNT`）+ `ts_granularity_t` 粒度（SECOND/MINUTE/HOUR/DAY）；通过 `storage_engine.h` 注册到 `ts_engine_get_ops` 操作表。

2. **压缩实现（5 种）**：`engine/src/db/storage/ts/ts_compress.c`（24397 字节，705 行）实现 Delta-of-Delta 时间戳压缩 + Simple8b 整数压缩 + RLE 游程压缩 + 字典压缩 + Gorilla XOR 浮点压缩（参考 Facebook Gorilla 论文）；公共头 `engine/include/db/ts_compress.h` 暴露 `ts_compress_block` / `ts_decompress_block` 接口。

3. **分区管理**：`engine/src/db/storage/ts/ts_partition.c`（4223 字节，147 行）实现按时间区间分区（年/月/日/小时可配）；与 `engine/src/db/storage/ts/ts_segment.c`（15396 字节，463 行）段式存储协同：每分区多个段，每段独立压缩 + 块索引。

4. **保留策略（TTL）**：`engine/src/db/storage/ts/ts_retention.c`（19030 字节，585 行）实现 `ALTER TABLE ... SET PARAM ttl = N` + 后台 retention worker 周期扫描过期 partition 整批物理删除；与 InfluxDB `DeleteShard` / QuestDB `PartitionPurgeJob` 思路一致。

5. **连续聚合（Continuous Aggregate）**：`engine/src/db/storage/ts/ts_continuous_agg.c`（16019 字节，514 行）实现 TimescaleDB 风格的「连续聚合」—— 按时间窗口增量维护预聚合结果；公共头 `engine/include/db/ts_continuous_agg.h` 暴露 `ts_cagg_create/refresh/drop` 接口。

6. **物化视图（MV）**：`engine/src/db/storage/ts/ts_mview.c`（11646 字节，353 行）实现时序物化视图；与 §5 连续聚合协同：连续聚合是 MV 的一种特例（按时间维度自动分区）。

7. **标签索引**：`engine/src/db/storage/ts/ts_label_index.c`（3725 字节，119 行）实现按 measurement / tag 的索引；与 `engine/src/db/storage/ts/ts_tag_index.c`（21173 字节，638 行）Tag 索引协同—— Tag 索引提供「tag value → series_id」的反向索引能力，类似 InfluxDB TSI1 的简化版。

8. **时序 SQL 函数**：`engine/src/db/storage/ts/ts_sql_functions.c`（16649 字节，538 行）实现 `time_bucket()` / `date_trunc()` / `INTERVAL()` / `FILL()` 等时序 SQL 扩展（暴露给上层 SQL 解析器）。

9. **时序列存特化**：`engine/src/db/storage/ts/ts_columnar.c`（22828 字节，716 行）+ `engine/include/db/storage/ts/ts_columnar.h` 实现时序场景下的列存特化（按时间分区 + 标签索引 + 连续聚合直接走列存）；与 `engine/src/db/storage/columnar/` 通用列存协同。

10. **缺失模块（明确标注）**：与 InfluxDB / TDengine / QuestDB 对比，自研时序**无** TSM1 引擎家族（Cache + WAL + Compactor 三件套）、**无** TSI1 倒排索引（LogFile/IndexFile/SeriesFile 三层）、**无** Last Cache 跳表、`LAST_ROW()` 亚毫秒查询、**无** 多级存储（内存 → SSD → 对象存储自动迁移）、**无** SMA 流式聚合 / TQ 数据订阅、**无** InfluxQL/Flux/SAMPLE BY/LATEST ON/ASOF JOIN SQL 扩展、**无** 乱序写入容忍窗口协议（`max-out-of-order-write-delay`）、**无** 协议层（Line Protocol / PG Wire）。

**关键路径速查**（与本报告后续引用一致）：

- 时序引擎入口：`engine/src/db/storage/ts/ts_engine.c`
- 公共头：`engine/include/db/ts_engine.h`
- 压缩实现：`engine/src/db/storage/ts/ts_compress.c`
- 段式存储：`engine/src/db/storage/ts/ts_segment.c`
- 分区管理：`engine/src/db/storage/ts/ts_partition.c`
- 保留策略：`engine/src/db/storage/ts/ts_retention.c`
- 连续聚合：`engine/src/db/storage/ts/ts_continuous_agg.c`
- 物化视图：`engine/src/db/storage/ts/ts_mview.c`
- 标签索引：`engine/src/db/storage/ts/ts_label_index.c`
- Tag 索引：`engine/src/db/storage/ts/ts_tag_index.c`
- 时序 SQL：`engine/src/db/storage/ts/ts_sql_functions.c`
- 时序列存：`engine/src/db/storage/ts/ts_columnar.c`
- 模块构建：`engine/src/db/storage/ts/CMakeLists.txt`

---

## 2. 业界主流做法

下面三篇对照项目分别代表「时序事实标准 + TSM 引擎 + 完整生态」「IoT 端到端 + 多级存储 + 内置流计算」「SQL 原生 + 列存 + 关系统一」三条路线，重点对比「存储引擎 / 索引机制 / 降采样与保留 / 协议层」四个维度。

### 2.1 InfluxDB（时序数据库事实标准）

详细：`docs/db-wiki/time-series/influxdb.md`

- **存储引擎**：**TSM1（Time-Structured Merge Tree）**（参考 LevelDB/BoltDB 设计）—— 写入路径为 **Cache（in-memory skip-list + ring buffer）+ WAL（mmap append-only，CRC + Length + Type + Data 格式）+ TSM File（不可变 SSTable-like，含 Time Range + SeriesID + Values Block）**；后台 Compactor 周期性合并（Full Compaction + Level Compaction）。
- **压缩**：**Gorilla 浮点 XOR 压缩**（Facebook 2015 论文，XOR + Leading/Trailing Zero 压缩，~12 bytes/sample 平均）+ **Simple8b 整数压缩** + **RLE** + **Bit-packed Boolean** + **Snappy / LZ4 字符串**；TSM File 末尾 IndexBlock 记录 SeriesID → (MinTime, MaxTime, Offset, Size)。
- **TSI1 倒排索引**：三层结构 **LogFile（mmap append-only）+ IndexFile（mmap 段，O(log N) 查找 + 位图交集）+ SeriesFile（SeriesID ↔ SeriesKey 映射）**；后台 LogFile compaction 生成 IndexFile。
- **降采样**：**InfluxDB 1.x CQ（Continuous Query，influxd 内部 ticker 调度）+ InfluxDB 2.x Flux Tasks（task service 调度，支持 every/cron）**；CQ/Tasks 把数据写入目标 measurement（1.x）或 bucket（2.x）。
- **保留策略**：**1.x Retention Policy**（duration + shard group duration + replication）+ **2.x Bucket**（retention period 单一配置）；Shard Group 是删除单位，过期整段删除（不可恢复！）。
- **查询接口**：**InfluxQL（类 SQL，influxql/parser.go → AST → storage/reads/）+ Flux（函数式语言，flux/parser/ → AST → runtime/ → Storage Reader）+ InfluxDB 3 SQL（基于 Apache Arrow DataFusion）**。
- **协议层**：**HTTP API（8086，默认）+ Line Protocol（measurement,tag1=v1,tag2=v2 field1=0.64 1465839830100400200）+ Native TCP（9000）+ Prometheus Remote Write/Read + Telegraf/Grafana 集成**。
- **事务一致性**：单节点 At-Least-Once（WAL + BoltDB 元数据 batch）；集群版（Enterprise）通过 **HashiCorp Raft（HCS Consensus）** 提供 Linearizable；默认 **Eventual Consistency**（读可能落后最近 N 秒）。
- **乱序写入**：**`max-out-of-order-write-delay`** 配置项（默认 1m），数据落到对应 Shard 后等待 Compactor 合并；超过窗口的乱序数据**拒绝写入**。
- **关键路径**：`tsdb/engine/tsm1/DESIGN.md`（官方设计文档）+ `tsdb/engine/tsm1/engine.go:WritePoints`（写入主路径）+ `tsdb/engine/tsm1/compact.go`（Compactor）+ `tsdb/engine/tsm1/cache.go`（Cache + SkipList）+ `tsdb/engine/tsm1/encoding.go`（Gorilla / Simple8b / RLE）+ `tsdb/index/tsi1/log_file.go`（LogFile）+ `tsdb/index/tsi1/index_file.go`（IndexFile）+ `tsdb/index/tsi1/series_file.go`（SeriesFile）。

### 2.2 TDengine（IoT 优化的端到端时序平台）

详细：`docs/db-wiki/time-series/tdengine.md`

- **存储引擎**：**TSDB 双层架构**—— **内存层（Last Cache 跳表 + SubmitCache 写入缓冲）+ 磁盘层（Data File 三列存储：last_value + timestamp + value）**；Block 4096 行/块；列级压缩（整数 Delta-of-Delta + Simple8b，Float64 Gorilla XOR，字符串 LZ4/ZSTD）；后台 commit → data file + merge。
- **VNode 虚拟化**：单 DNode 内可承载多个 VNode（每个 VNode 是独立时序数据库），**资源隔离 + 故障隔离**；MNode 管理集群元数据（RAFT 共识）。
- **Last Cache 跳表**：`source/dnode/vnode/src/tsdb/tsdbCache.c` —— 每子表（Sub Table）一个 **跳表（Skip List）**，缓存每个时间序列的**最新值**；`SELECT LAST_ROW(col)` 直接走内存，**亚毫秒级响应**；金融、电力等场景核心需求。
- **多级存储（Multi-Level Storage）**：**Level 0（内存/SSD）+ Level 1（HDD）+ Level 2（对象存储 S3/OSS/HDFS/MinIO）**；`ALTER DATABASE db KEEP 90,365,3650; STORAGE_LEVEL 0,1,2;` 配置；后台 DataMover 自动迁移；**存储成本下降 10-100x**。
- **SMA 流式聚合（Stream Materialized Aggregator）**：**`CREATE SMA TABLE rollup_5m AS SELECT ... INTERVAL(5m);`** —— 订阅源表增量数据，实时维护 Rollup 表，类似 Materialized View 但专用时序场景；比 MV 资源占用更低。
- **TQ 数据订阅（Kafka-like）**：**`CREATE TOPIC topic_offsets AS SELECT * FROM meters WHERE ...;` + `taos_subscribe(topic)` 客户端消费** —— 订阅源表 WAL/增量数据，维护 Offset，支持多订阅者；v3.2+ 提供 Kafka 协议入口。
- **Super Table 模式**：设备元数据 + 子表自动建表 —— `CREATE STABLE meters (ts TIMESTAMP, current FLOAT, voltage INT) TAGS (groupid INT, location BINARY(64));`，子表继承 schema + 自动分配 tag；非 Super Table 失去设备元数据管理。
- **降采样与连续聚合**：通过 SMA 实现（见上）；**KEEP 90,365,3650** 实现多级保留。
- **协议层**：**taosc 原生协议（C/自研 TCP，默认 6041）+ REST API（/rest/sql）+ JDBC/ODBC + Kafka 协议（v3.2+）**；多语言 SDK（C/C++/Java/Python/Go/Rust/Node.js/C#/R/Spark/Flink Connector）。
- **事务一致性**：**单节点 Atomic Per Row（行级原子）+ 集群 RAFT 一致性（v3.0+，source/dnode/mnode/sdb/）+ 读 Linearizable**；**无 MVCC，读已提交**。
- **乱序写入**：**完全支持**；Block 内乱序数据按 ts 排序后存储；Block 间乱序写入会触发 Last Cache 更新 + 数据搬迁，开销较大。
- **关键路径**：`source/dnode/vnode/src/tsdb/tsdbCache.c`（Last Cache）+ `source/dnode/vnode/src/tsdb/tsdbDataFileRW.c`（Data File）+ `source/dnode/vnode/src/tsdb/tsdbCommit.c/tsdbCommit2.c`（Commit）+ `source/dnode/vnode/src/tsdb/tsdbMemTable.c`（MemTable）+ `source/dnode/vnode/src/tsdb/tsdbMerge.c/tsdbMergeTree.c`（后台 Merge）+ `source/dnode/vnode/src/sma/smaRollup.c`（SMA）+ `source/dnode/vnode/src/tq/tq.c`（TQ 主入口）+ `source/dnode/vnode/src/tq/tqScan.c`（扫描源数据）+ `source/dnode/vnode/src/tq/tqOffset.c`（Offset 管理）。

### 2.3 QuestDB（SQL 原生时序数据库）

详细：`docs/db-wiki/time-series/questdb.md`

- **存储引擎**：**Cairo 引擎（io.questdb.cairo）—— Native Columnar（按列文件 + PageFrame 4096 行）**；表 = 多 Partition（按时间）/ 多 Client（按列）；Symbol 列（低基数字符串）自动转 int 索引 + 字典；WAL 顺序写 + 后台 Checkpoint + Replication（企业版）。
- **索引机制**：**Symbol 列（int 索引 + 字典，重复值压缩率 ~10-100x）+ Bitmap 索引（Roaring Bitmap，等值过滤 + 范围扫描加速 10-100x）+ TIMESTAMP 索引（分区文件直接定位，无 B-Tree）**。
- **时序 SQL 扩展（差异化亮点）**：**`SAMPLE BY 5m`（自动时间桶聚合）+ `LATEST ON <col>`（取每组最新一行）+ `ASOF JOIN`（时间序列非等值连接，金融 Tick 关键）+ `WINDOW` 函数（标准 SQL + 时序语义）**；查询直接走 MV，0 SQL 修改。
- **物化视图（Materialized Views，v7.0+）**：**`CREATE MATERIALIZED VIEW cpu_5m AS (SELECT ... SAMPLE BY 5m) PARTITION BY DAY;`** —— 后台 MatView Refresh Job 增量刷新（根据源表最新时间戳）；支持 Keyed MatView（GROUP BY）。
- **保留策略（Partition TTL，v8.0+）**：**`ALTER TABLE cpu SET PARAM ttl = 30;`** —— 后台 `PartitionPurgeJob`（`io.questdb.cairo.PartitionPurgeJob`）周期扫描过期 partition；**整 partition 物理删除**（不可恢复）；WAL 文件按 `-wal.ttl=24h` 保留。
- **协议层**：**PostgreSQL Wire Protocol（8812，完整兼容 psql/pgx/JDBC）+ InfluxDB Line Protocol（9009，HTTP 接收）+ HTTP REST API（9000，/exec + /imp）**；客户端可直接用 PG 客户端（psql/JDBC/pg_dump 兼容）+ Kafka Connect Sink + Grafana 官方数据源。
- **事务一致性**：**单节点默认 Atomic Per Row（单行原子）+ WAL Crash Recovery + 企业版 Quorum 写入**；读默认 **Read Committed**（读最近 Checkpoint + WAL）；隔离级别基于 MVCC + 锁，每行有 `TxnId`。
- **乱序写入**：完全支持，自动 dedup（`dedup` key 字段，v8.0+，同 key + ts 仅保留 last-write）；生产建议数据源保序写入，乱序场景用 dedup 兜底。
- **关键路径**：`io.questdb.cairo.CairoEngine`（引擎入口）+ `io.questdb.cairo.TableWriter`（写路径）+ `io.questdb.cairo.TableReader`（读路径）+ `io.questdb.cairo.AbstractPageFrameCursor`（PageFrame 游标）+ `io.questdb.cairo.FullPartitionFrameCursor`（整分区游标）+ `io.questdb.cairo.SymbolMap`（符号表）+ `io.questdb.cairo.BitmapIndexWriter`（Bitmap 索引）+ `io.questdb.cairo.wal.seq`（WAL 序列）+ `io.questdb.griffin.SqlParser`（SQL 解析）+ `io.questdb.griffin.SqlOptimiser`（优化器）+ `io.questdb.griffin.SqlCodeGenerator`（代码生成）+ `io.questdb.griffin.engine.groupby.MicroTimestampSampler`（SAMPLE BY）+ `io.questdb.griffin.engine.functions.latest.LatestBy*`（LATEST ON）+ `io.questdb.griffin.engine.join.LtJoin*`（ASOF JOIN）+ `io.questdb.cutlass.pgwire.PgWireContext`（PG 协议）+ `io.questdb.cutlass.line.LineTcpReceiver`（InfluxDB LP）。

---

## 3. 24 维度对照矩阵

> 等级：A=工业级，缺失≤1 个版本号差距 / B=教学+工程可用，关键能力缺失 / C=占位，仅 API / D=空。
> 本表覆盖自研 vs InfluxDB vs TDengine vs QuestDB 四方对照。

| # | 维度 | 自研（book） | InfluxDB | TDengine | QuestDB | 等级 | 关键说明 |
|---|------|-------------|----------|----------|---------|------|------|
| 1 | 项目定位 | 多模态底座时序 + 连续聚合 | 时序事实标准 + TSM + TICK 生态 | IoT 端到端 + 多级存储 + 流计算 | SQL 原生 + 列存 + 关系统一 | B | 自研缺协议层 + 完整生态 |
| 2 | 发展历史 | 单一维护者内部迭代 | 2013 起，13 年，每年 3-4 版本 | 2017 起，9 年，每年 4-6 版本 | 2014 起，12 年，每年 6+ 版本 | C | 历史沉淀薄，缺版本演进纪律 |
| 3 | 许可限制 | 内部使用，无外部约束 | MIT（OSS Core）+ 商业 Enterprise | AGPL 3.0 + 商业 Enterprise | Apache 2.0 + 商业 Enterprise | A | 自研无开源义务，但也缺外部审计 |
| 4 | 使用方案例 | 内部测试+多模态 demo 集成 | Cisco/PayPal/IBM/Bosch/Adobe/Honda | 国家电网/京东/蔚来/比亚迪/宁德时代 | IOG（Cardano）/Telefonica/10xBank/Octopus Energy | C | 缺外部生产案例 |
| 5 | 对比定位 | 「嵌入式时序 + 连续聚合 + 标签索引」 | 时序事实标准 + 完整生态 | IoT 端到端 + 多级存储 | SQL 原生 + 列存 + 关系统一 | B | 自研缺协议层 + 分布式 |
| 6 | 整体架构 | 单进程嵌入式 C API（无 server 进程） | daemon 进程 + HTTP/InfluxQL/Flux | DNode + VNode/MNode/QNode + taosd | 单进程 Java + PG/LP 协议 | C | 缺独立 server 进程 + 协议监听 |
| 7 | 存储引擎 | 时序基础 + 5 种压缩 + 分区 + 段式 + 连续聚合 | TSM1（Cache + WAL + Compactor）+ TSM File | TSDB 双层（Last Cache + Data File）+ Block 4096 | Cairo（Native Columnar + PageFrame 4096） | B | 缺 TSM1 三件套 + 双层架构 |
| 8 | 数据模型 | 时序点（ts + tags + fields）+ measurement + tag set | Line Protocol（measurement + tag set + fields + ts） | Super Table + 子表（schema + tag）+ 标准 SQL | 表 + designated timestamp + Symbol 列 | B | 缺 Super Table + Symbol 优化 |
| 9 | 索引机制 | 标签索引 + Tag 索引 + 列裁剪 | TSI1（LogFile + IndexFile + SeriesFile）+ Bitmap | 无显式索引（Tag 即 Schema） | Symbol（int + 字典）+ Bitmap（Roaring）+ TIMESTAMP | A | 自研索引「种类齐全」是亮点 |
| 10 | 查询接口 | 时序 SQL 函数（time_bucket/date_trunc/INTERVAL/FILL）+ 基础聚合 | InfluxQL + Flux + 200+ 算子 + 1.x CQ + 2.x Tasks | 标准 SQL + INTERVAL/FILL/LAST_ROW + SMA + 窗口函数 | 标准 SQL + SAMPLE BY/LATEST ON/ASOF JOIN + MV | B | 缺 SAMPLE BY/LATEST ON/ASOF JOIN |
| 11 | 压缩算法 | 5 种（Delta-of-Delta + Simple8b + RLE + Dict + Gorilla XOR） | Gorilla XOR（论文级）+ Simple8b + RLE + Bit-packed + LZ4/Snappy | Delta-of-Delta + Simple8b + Gorilla XOR + LZ4/ZSTD | Delta-of-Delta + Simple8b + Gorilla XOR + LZ4 + Symbol 字典 | A | 自研 5 种压缩齐全是亮点 |
| 12 | 连续聚合 / 物化视图 | `ts_continuous_agg.c`（连续聚合）+ `ts_mview.c`（MV） | 1.x CQ + 2.x Flux Tasks（task service） | SMA（Stream Materialized Aggregator） | Materialized Views v7.0+（增量刷新） | A | 自研「连续聚合 + MV」是亮点 |
| 13 | 事务一致性 | 无显式事务（依赖 cf lock） | 单节点 At-Least-Once + Enterprise Raft Linearizable | 单节点 Atomic Per Row + 集群 RAFT | 单节点 Atomic Per Row + WAL + 企业版 Quorum | C | 缺时序级事务隔离 |
| 14 | 复制高可用 | 无 HA 模块（单机） | Enterprise Raft（HCS）+ Anti-entropy + Data Node HA | VGroup RAFT + SNode 异地容灾 + TAOSX 迁移 | 企业版 Replication（Primary → Replica WAL 异步）+ Cloud HA | D | 无 HA 模块，RPO=0 不可达 |
| 15 | 协议 / 客户端 SDK | 无协议层（C 嵌入式 API） | HTTP 8086 + Line Protocol + Native TCP + 10+ SDK | taosc + REST + JDBC/ODBC + Kafka v3.2+ + 10+ SDK | PG Wire Protocol + InfluxDB LP + HTTP + Kafka Connect | D | 缺协议层 + 客户端 SDK |
| 16 | 降采样策略 | 连续聚合（增量）+ MV（手动刷新） | CQ（周期）+ Tasks（Flux 周期/每分钟） | SMA（Trigger + rollover）+ 多级 KEEP | Materialized Views（增量刷新） | B | 缺 CQ/Tasks 周期触发机制 |
| 17 | 乱序写入 | 无显式协议（按 ts 排序插入假设） | `max-out-of-order-write-delay`（默认 1m） | 完全支持（Block 内排序） | 完全支持（dedup key 自动去重） | C | 缺 `max-out-of-order-write-delay` 协议 |
| 18 | 扩展性 | 多模型（KV/列存/向量/图/时序）共享存储层 | Telegraf（300+ Input 插件）+ TICK 生态 | 8+ 角色（VNode/MNode/QNode/SNode/BNode + Edge） | Kafka Connect + Iceberg v8.2+ + JDBC | A | 多模型复用是「独有优势」亮点 |
| 19 | 性能基准 | 内部 gtest 验证通过，缺 TSBS/ClickBench 公开数据 | InfluxDB-comparisons（tsbs）+ TSBS | TDengine/research/benchmark/（自家） | questdb/benchmarks/（自家）+ TSBS | C | 未发布基准，难以横向比较 |
| 20 | 运维可观测性 | 简单 stats 指标 | `_internal` database + `/_metrics` Prometheus + FTDC | taosd 内置 `/metrics` Prometheus + taosExplorer | `/metrics` Prometheus + Web Console | C | 缺 Histogram、IOTrace、慢日志 |
| 21 | 生态集成 | `engine/src/db/storage/ts/CMakeLists.txt` 内部链接 | Telegraf/Grafana/Prometheus Remote/Kafka 双向 | Grafana/Prometheus/Kafka/Flink/Spark/K8s | Grafana/Kafka Connect/Iceberg/K8s/JDBC 全栈 | D | 客户端 SDK、ORM、容器镜像缺失 |
| 22 | 核心论文 | 无独立发表 | Gorilla 论文（Facebook VLDB 2015）+ TSM 博客 | TDengine 论文（Tao 2020）+ Raft 论文 | Gorilla 论文 + Roaring Bitmaps 论文 | D | 学术影响力薄弱 |
| 23 | 源码路径（对照） | `engine/src/db/storage/ts/` + `engine/include/db/ts_engine.h` | `tsdb/engine/tsm1/` + `tsdb/index/tsi1/` | `source/dnode/vnode/src/tsdb/` + `tq/` + `sma/` | `core/src/main/java/io/questdb/cairo/` + `griffin/` | A | 路径分层清晰 |
| 24 | 与本项目关联 | 即本项目，无需解释 | TSM1 + TSI1 是「时序引擎原型」 | TSDB 双层 + TQ 数据订阅是「端到端路线」 | Cairo + ASOF JOIN 是「SQL 原生路线」 | A | 路径一一对应，可对照 wiki 学习 |

**矩阵读法**：

- 等级 A：维度 3 / 9 / 11 / 12 / 18 / 23 / 24（共 7 项，是 13 子报告里维度 11「压缩算法」+ 维度 12「连续聚合」双 A 的少数案例）
- 等级 B：维度 1 / 5 / 7 / 8 / 10 / 16（共 6 项）
- 等级 C：维度 2 / 4 / 6 / 13 / 17 / 19 / 20（共 7 项）
- 等级 D：维度 14 / 15 / 21 / 22（共 4 项）

**整体评级：B+**——方向正确且压缩齐全 + 连续聚合 + 多模型复用亮点，但缺 TSM1 引擎家族 / TSI1 倒排索引 / 协议层 / 分布式 HA / SAMPLE BY/LATEST ON/ASOF JOIN 五块补丁；建议下月把 P0 集中在维度 7、15、17 这三个等级 C/D 的联动。

---

## 4. 关键差距 Top-5

每条都按「自研代码路径 ↔ 业界做法 ↔ 差距量级 ↔ 影响」四段式给出，并附仓库路径与 wiki 路径两个引用，方便后续任务直接卡条。

### 4.1 TSM1 引擎家族（Cache + WAL + Compactor）全链路空白（等级 D → A）

- **自研现状**：`engine/src/db/storage/ts/ts_engine.c`（26346 字节）+ `engine/src/db/storage/ts/ts_compress.c`（24397 字节）+ `engine/src/db/storage/ts/ts_segment.c`（15396 字节）+ `engine/src/db/storage/ts/ts_partition.c`（4223 字节）实现基础时序存储 + 压缩 + 段 + 分区，但**无** TSM1 三件套 —— 没有 in-memory skip-list Cache（依赖通用 `engine/src/algo/skiplist.c`）、**没有** append-only WAL（依赖通用 `engine/src/db/storage/wal/wal.c` 但未针对时序块优化）、**没有** Compactor（Full Compaction + Level Compaction 后台合并）。
- **业界做法**：InfluxDB `tsdb/engine/tsm1/DESIGN.md`（官方设计文档）+ `engine.go:WritePoints`（写入主路径）+ `compact.go`（Compactor）+ `cache.go`（Cache + SkipList）+ `encoding.go`（Gorilla/Simple8b/RLE）；TDengine `source/dnode/vnode/src/tsdb/tsdbCache.c`（Last Cache）+ `tsdbCommit.c/tsdbCommit2.c`（Commit）+ `tsdbMemTable.c`（MemTable）+ `tsdbMerge.c/tsdbMergeTree.c`（后台 Merge）。
- **差距量级**：约 3-5 个文件 / 3000-6000 行 C；包含 TSM Cache（in-memory skip-list + ring buffer）+ TSM WAL（CRC + Length + Type + Data 格式）+ TSM Compactor（Full + Level Compaction）+ TSM File 编码（时序块 + Gorilla + Simple8b）。
- **影响**：缺「写时不可变 + 后台异步合并」的工业级 LSM 范式；Insert 直接覆盖现有数据，无版本概念；写入吞吐受限于「单进程同步写」；无法实现 InfluxDB 风格的「百万级 data points/sec」。

### 4.2 协议层（Line Protocol + PG Wire + taosc）全链路空白（等级 D → A）

- **自研现状**：`engine/include/db/ts_engine.h` 仅暴露嵌入式 C API（`ts_engine_init/create/open/insert/get`），**无** InfluxDB Line Protocol 兼容层、**无** InfluxQL/Flux SQL 解析、**无** PostgreSQL Wire Protocol 兼容层、**无** taosc 原生协议、**无** REST API、**无** SCRAM/认证。
- **业界做法**：InfluxDB HTTP API（8086）+ Line Protocol（measurement,tag1=v1,tag2=v2 field1=0.64 1465839830100400200）+ Native TCP（9000）+ Prometheus Remote Write/Read；TDengine taosc（C/自研 TCP，默认 6041）+ REST API（/rest/sql）+ JDBC/ODBC + Kafka 协议 v3.2+；QuestDB PostgreSQL Wire Protocol（8812，完整兼容 psql/pgx/JDBC）+ InfluxDB Line Protocol（9009）+ HTTP REST（9000）。
- **差距量级**：约 5-8 个文件 / 5000-10000 行 C（协议层 + 认证 + 会话）；包含 TCP/HTTP 监听、wire protocol 帧解码、命令分发表、Line Protocol 解析、认证/会话管理。
- **影响**：外部应用无法直连，仅能进程内调用 API；这是「从嵌入式时序库升级为分布式时序数据库」的最大瓶颈；与 InfluxDB/Telegraf/Prometheus Remote Write/Grafana/InfluxDB Client SDK 生态完全不兼容。

### 4.3 SAMPLE BY / LATEST ON / ASOF JOIN 时序 SQL 扩展缺失（等级 D → B+）

- **自研现状**：`engine/src/db/storage/ts/ts_sql_functions.c`（16649 字节）实现 `time_bucket()` / `date_trunc()` / `INTERVAL()` / `FILL()` 等基础时序 SQL 函数，但**无** QuestDB 风格的 `SAMPLE BY`（自动时间桶聚合）、`LATEST ON <col>`（取每组最新一行）、`ASOF JOIN`（时间序列非等值连接）。
- **业界做法**：QuestDB `io.questdb.griffin.engine.groupby.MicroTimestampSampler`（SAMPLE BY）+ `io.questdb.griffin.engine.functions.latest.LatestBy*`（LATEST ON）+ `io.questdb.griffin.engine.join.LtJoin*`（ASOF JOIN，金融 Tick 关键）；TDengine `INTERVAL(5m)` + `FILL(NULL/PREV/LINEAR/VALUE)` + `LAST_ROW(col)` + `PARTITION BY + SLIMIT + SOFFSET` + 窗口函数（v3.0+）；InfluxDB `GROUP BY time(5m), host`（InfluxQL）+ `aggregateWindow(every: 5m, fn: mean)`（Flux）。
- **差距量级**：约 3-5 个文件 / 3000-6000 行 C；包含 SAMPLE BY（自动时间桶 + FILL）+ LATEST ON（元数据定位最后一行）+ ASOF JOIN（排序键 + 双指针）+ 窗口函数（标准 SQL + 时序语义）。
- **影响**：自研 SQL 表达力弱（无 ASOF JOIN 无法做金融 Tick 关键的非等值连接）；用户需要手动写繁琐的子查询；与 TimescaleDB/QuestDB 应用的迁移成本高。

### 4.4 多级存储（Level 0/1/2 自动迁移）+ Last Cache 跳表缺位（等级 C → A）

- **自研现状**：`engine/src/db/storage/ts/ts_engine.c` + `engine/src/db/storage/ts/ts_segment.c` + `engine/src/db/storage/ts/ts_partition.c` 实现「单级磁盘存储」，**无** 多级存储（内存 → SSD → 对象存储自动迁移）；`engine/src/algo/skiplist.c` 提供通用跳表但**未针对时序 `LAST_ROW()` 优化**（无每子表 skip list）。
- **业界做法**：TDengine `ALTER DATABASE db KEEP 90,365,3650; STORAGE_LEVEL 0,1,2;` 多级配置 + 后台 DataMover 自动迁移 + `source/dnode/vnode/src/tsdb/tsdbCache.c` 每子表 Skip List（`SELECT LAST_ROW(col)` 亚毫秒响应）；InfluxDB OSS 2.x **无内置分层**，Enterprise/Cloud 通过 S3/OSS 间接实现。
- **差距量级**：约 3-5 个文件 / 3000-5000 行 C；包含 Last Cache 跳表（每子表 + 每时序最新值）+ 多级存储策略（KEEP + STORAGE_LEVEL）+ DataMover 后台迁移任务 + 查询路由（按时间区间选 Level 0/1/2）。
- **影响**：缺 IoT 大数据量场景的存储成本优化（10-100x 存储成本差距）；`LAST_ROW()` 查询必须全表扫描（毫秒级 → 亚毫秒级差距）；无法应对「边缘 → 云端」的端到端数据流转。

### 4.5 乱序写入容忍协议 + 复制 HA + SMA 流式聚合三件套全链路空白（等级 C → A）

- **自研现状**：`engine/src/db/storage/ts/ts_engine.c` 假设数据顺序到达，**无** `max-out-of-order-write-delay` 类容忍窗口协议；**无** 复制 HA 模块；`engine/src/db/storage/ts/ts_continuous_agg.c` + `ts_mview.c` 提供连续聚合 + MV，但**无** TDengine 风格的 SMA 流式聚合（订阅源表增量 + 实时 Rollup）。
- **业界做法**：InfluxDB `max-out-of-order-write-delay`（默认 1m）+ Enterprise Raft（HCS Consensus）+ 1.x CQ + 2.x Flux Tasks；TDengine 完全支持乱序写入（Block 内排序 + 自动重排）+ VGroup RAFT + SNode 异地容灾 + SMA `source/dnode/vnode/src/sma/smaRollup.c`；QuestDB dedup key 字段（v8.0+，同 key + ts 仅保留 last-write）+ 企业版 Replication + MatView Refresh Job。
- **差距量级**：约 5-8 个文件 / 5000-10000 行 C；包含乱序写入协议（容忍窗口 + Block 内排序 + 数据搬迁）+ 复制 HA（RAFT + Hinted Handoff + Merkle Tree 对账）+ SMA 流式聚合（增量订阅 + 实时 Rollup + Offset 管理）。
- **影响**：网络抖动 / 多边缘节点采集场景下，乱序写入全部拒绝（生产事故）；单节点故障 = 全量不可用（RPO=0 不可达）；无法实现金融/电力场景的「实时流式预聚合」。

---

## 5. 优势 Top-3（自研独有的设计）

### 5.1 压缩算法齐全（Delta-of-Delta + Simple8b + RLE + Dict + Gorilla XOR）

`engine/src/db/storage/ts/ts_compress.c`（24397 字节，705 行）实现 **5 种压缩算法**：Delta-of-Delta 时间戳压缩（参考 Gorilla 论文）、Simple8b 整数压缩、RLE 游程压缩、字典压缩、Gorilla XOR 浮点压缩。InfluxDB（5 种：Gorilla + Simple8b + RLE + Bit-packed + Snappy/LZ4）+ TDengine（4 种：Delta-of-Delta + Simple8b + Gorilla + LZ4/ZSTD）+ QuestDB（5 种：Delta-of-Delta + Simple8b + Gorilla + LZ4 + Symbol 字典）压缩种类都**接近自研**，但自研把「时序压缩」与 `engine/src/db/storage/columnar/` 通用列存压缩集成，统一接口 `ts_compress_block/decompress_block`，**易学习易扩展**。

### 5.2 连续聚合 + 物化视图双轨设计（TimescaleDB 风格内化）

`engine/src/db/storage/ts/ts_continuous_agg.c`（16019 字节，连续聚合）+ `engine/src/db/storage/ts/ts_mview.c`（11646 字节，物化视图）形成「连续聚合 + 物化视图」双轨。连续聚合（Continuous Aggregate）按时间窗口增量维护预聚合结果（TimescaleDB 风格），物化视图（MV）提供手动刷新机制。InfluxDB 1.x CQ + 2.x Flux Tasks（通过任务调度）+ TDengine SMA（专用流式聚合）+ QuestDB MV（增量刷新）四家都有等价能力，但自研把「连续聚合」作为一等公民内化到引擎核心，而非外部可调用业务层，**语义最清晰**。

### 5.3 多模型复用页面 / 缓冲池 / WAL / Catalog 基础设施 + 时序列存特化

`engine/src/db/storage/page/page.c` + `engine/src/db/storage/buffer/bufmgr.c` + `engine/src/db/storage/wal/wal.c` + `engine/src/db/storage/catalog/catalog.c` 四个模块同时被 `engine/src/db/storage/{kv,columnar,ts,vector,spatial,graph,sparse,yang,doc}/` 引用，是真正意义上的「一个底座支撑 10 余种数据模型」；`engine/src/db/storage/ts/ts_columnar.c`（22828 字节）实现时序场景下的列存特化（按时间分区 + 标签索引 + 连续聚合直接走列存），是 InfluxDB（单一 TSM 引擎）+ TDengine（单一 TSDB 双层）+ QuestDB（单一 Cairo 引擎）都做不到的事。

---

## 6. 学习路径

> 推荐精读以下 wiki 段落 + 一段自研源码，每条都给「为什么读它」。

### 6.1 必须读（一周内）

1. **[`docs/db-wiki/time-series/influxdb.md`](../db-wiki/time-series/influxdb.md) §7（TSM 存储引擎）+ §8（TSI1 倒排索引）+ §十二（事务与一致性）**
   - 原因：补齐 Top-5 §4.1（TSM1 三件套）+ §4.5（乱序写入协议）的整体图；尤其关注 `tsdb/engine/tsm1/DESIGN.md`（官方设计文档）+ `engine.go:WritePoints`（写入主路径）+ `compact.go`（Compactor）+ `max-out-of-order-write-delay` 配置项。

2. **[`docs/db-wiki/time-series/tdengine.md`](../db-wiki/time-series/tdengine.md) §7（TSDB 双层）+ §八（SMA + TQ）+ §九（多级存储）**
   - 原因：补齐 Top-5 §4.4（多级存储）+ §4.5（SMA 流式聚合）的实现细节；TDengine 的 Last Cache 跳表（亚毫秒级 `LAST_ROW()`）+ 多级存储自动迁移（10-100x 成本下降）是 IoT 大数据场景的杀手锏，自研可直接借鉴。

3. **[`docs/db-wiki/time-series/questdb.md`](../db-wiki/time-series/questdb.md) §七（Cairo 存储引擎）+ §八（索引机制）+ §9（物化视图）+ §十一（查询接口层）**
   - 原因：补齐 Top-5 §4.3（SAMPLE BY/LATEST ON/ASOF JOIN 时序 SQL 扩展）的实现细节；QuestDB 的「零修改 SQL + 时序扩展」是 SQL 原生路线的典范，自研可直接借鉴其 `MicroTimestampSampler` + `LatestBy*` + `LtJoin*` 设计。

### 6.2 推荐读（一月内）

4. **[`docs/db-wiki/time-series/influxdb.md`](../db-wiki/time-series/influxdb.md) §9（降采样与 Continuous Query）+ §10（保留策略）**
   - 原因：补齐维度 16（降采样策略）+ 维度 14（保留策略）的设计原型；1.x CQ + 2.x Flux Tasks + Shard Group 删除单位是时序数据库「生命周期管理」的教科书。

5. **[`docs/db-wiki/time-series/tdengine.md`](../db-wiki/time-series/tdengine.md) §十（保留策略与 TTL）+ §十一（查询接口层）+ §十三（复制与高可用）**
   - 原因：补齐维度 14（保留策略）+ 维度 15（协议）+ 维度 13（复制 HA）的设计原型；TDengine 的 VGroup 副本 + SNode 异地容灾 + TAOSX 数据迁移是 IoT 大数据场景的 HA 范本。

6. **[`docs/db-wiki/time-series/questdb.md`](../db-wiki/time-series/questdb.md) §10（保留策略与 TTL）+ §13（复制与高可用）+ §十七（风险与陷阱）**
   - 原因：补齐维度 14（保留策略）+ 维度 13（复制 HA）+ 维度 17（乱序写入）的设计原型；QuestDB 的 Partition TTL + `dedup` key + 企业版 Replication + 风险陷阱清单是时序产物的最佳实践。

### 6.3 对照实验

- 自研 `engine/src/db/storage/ts/ts_compress.c` ↔ InfluxDB `tsdb/engine/tsm1/encoding.go`：5 种压缩算法命名相似（Delta-of-Delta/Simple8b/RLE/Gorilla/Dict），自研缺 TSM File 时序块封装。
- 自研 `engine/src/db/storage/ts/ts_continuous_agg.c` ↔ TDengine `source/dnode/vnode/src/sma/smaRollup.c`：连续聚合思路一致（订阅源表增量 + 实时维护 Rollup 表），自研缺「Offset 管理 + 多订阅者支持」。
- 自研 `engine/src/db/storage/ts/ts_retention.c` ↔ QuestDB `io.questdb.cairo.PartitionPurgeJob`：整 partition 物理删除 + 后台扫描过期 partition 思路一致，自研缺「WAL 多 Shard TTL + 企业版 Replication」。

---

## 7. 补齐项下月计划

> 每条按「动作 / 投入 / 学习资源（wiki 链接）/ 验收标准 / 优先级」给出。
> 至少 3 条 P0/P1，本节给出 5 条。

### 7.1 P0 — 启动 TSM1 引擎家族（Cache + WAL + Compactor）（约 4 周）

- **动作**：在 `engine/src/db/storage/ts/` 下新建 `tsm_cache.{c,h}`（in-memory skip-list + ring buffer）+ `tsm_wal.{c,h}`（mmap append-only + CRC + Length + Type + Data 格式）+ `tsm_compactor.{c,h}`（Full Compaction + Level Compaction）+ `tsm_file.{c,h}`（不可变文件 + IndexBlock 末尾 + 时序块 + Gorilla/Simple8b）；扩展 `engine/src/db/storage/ts/ts_engine.c` 调度 TSM1 写入路径。
- **投入**：约 3000-6000 行 C，1 人 3 周开发 + 1 周测试。
- **学习资源**：[influxdb.md §7](../db-wiki/time-series/influxdb.md)（TSM1 三件套）+ [influxdb.md §7.1-7.2](../db-wiki/time-series/influxdb.md)（写入流程 + 时序块）。
- **验收标准**：
  - `engine/test/db/storage/ts/tsm_cache_test.cpp` ≥ 6 用例（put/get/flush/snapshot）通过；
  - `engine/test/db/storage/ts/tsm_wal_test.cpp` ≥ 4 用例（append/recovery/crash-safe）通过；
  - `engine/test/db/storage/ts/tsm_compactor_test.cpp` ≥ 4 用例（full/level/cross-shard）通过；
  - 1000 万次 insert 后 parts 数 < 100（后台合并有效）；
  - `engine/src/db/storage/ts/CMakeLists.txt` 通配新 .c 文件加入 `storage_ts` 静态库。
- **优先级**：**P0**（决定 §4.1 与 InfluxDB 引擎家族对标）。

### 7.2 P0 — 启动 InfluxDB Line Protocol + PostgreSQL Wire Protocol 双协议入口（约 3 周）

- **动作**：在 `engine/src/db/storage/ts/` 下新建 `protocol/line_protocol.{c,h}`（Line Protocol 解析：measurement + tag set + fields + ts）+ `protocol/pg_wire.{c,h}`（PostgreSQL Wire Protocol 完整兼容 psql/JDBC）+ `protocol/http_rest.{c,h}`（HTTP REST API /write + /query）；默认监听 8086（InfluxDB 风格）+ 8812（QuestDB PG 风格）双端口。
- **投入**：约 3000-5000 行 C（协议层）+ 1000-2000 行（认证/会话），1 人 2 周开发 + 1 周联调。
- **学习资源**：[influxdb.md §14](../db-wiki/time-series/influxdb.md)（监控 + 备份恢复 + 客户端 SDK）+ [questdb.md §11.2](../db-wiki/time-series/questdb.md)（PG Wire Protocol + InfluxDB Line Protocol）+ [questdb.md §14](../db-wiki/time-series/questdb.md)（运维与生态）。
- **验收标准**：
  - `curl -X POST 'http://localhost:8086/write?db=mydb' --data-binary 'cpu,host=server01 value=0.64 1465839830100400200'` 成功写入；
  - `psql -h 127.0.0.1 -p 8812 -U admin mydb -c "SELECT * FROM cpu WHERE time > now() - 1h"` 端到端可用；
  - InfluxDB Client SDK（`influxdb-client-python`）+ Telegraf + Prometheus Remote Write 兼容；
  - 多并发连接（≥32）无 dead-lock、无内存泄漏。
- **优先级**：**P0**（决定自研能否从内部引擎升级为外部可用产品）。

### 7.3 P0 — 启动 SAMPLE BY / LATEST ON / ASOF JOIN 时序 SQL 扩展（约 3 周）

- **动作**：在 `engine/src/db/storage/ts/ts_sql_functions.c` 中扩展实现 **SAMPLE BY**（自动时间桶 + FILL）+ **LATEST ON**（元数据定位最后一行，扩展 `ts_engine_get_latest()`）+ **ASOF JOIN**（排序键 + 双指针，扩展 `engine/src/db/executor/operators/`）；与 `engine/src/db/optimizer/` 联动根据 SAMPLE BY 自动选择预聚合表。
- **投入**：约 3000-6000 行 C，1 人 2 周开发 + 1 周测试。
- **学习资源**：[questdb.md §11.1](../db-wiki/time-series/questdb.md)（SQL Griffin 时序扩展）+ [questdb.md §16.2](../db-wiki/time-series/questdb.md)（Griffin SQL 引擎源码）+ [tdengine.md §11](../db-wiki/time-series/tdengine.md)（SQL 时序扩展 + INTERVAL/FILL/LAST_ROW）。
- **验收标准**：
  - `SELECT time_bucket('5m', ts) AS bucket, AVG(value) FROM cpu GROUP BY bucket` 端到端可用；
  - `SELECT LAST_ROW(value) FROM cpu GROUP BY host` 亚毫秒级响应（依赖 Last Cache 跳表优化）；
  - `SELECT * FROM orders ASOF JOIN trades ON orders.symbol = trades.symbol` 跨订单语义的非等值连接可用；
  - `engine/test/db/storage/ts/ts_sql_extensions_test.cpp` ≥ 8 用例（SAMPLE BY/LATEST ON/ASOF JOIN 各 ≥ 2 + FILL + 边界）通过。
- **优先级**：**P0**（决定 §4.3 与 QuestDB/TimescaleDB SQL 表达力对标）。

### 7.4 P1 — 启动多级存储（Level 0/1/2 自动迁移）+ Last Cache 跳表（约 3 周）

- **动作**：在 `engine/src/db/storage/ts/` 下新建 `ts_last_cache.{c,h}`（每子表 Skip List + 最新值缓存）+ `ts_multi_level.{c,h}`（多级存储策略 KEEP + STORAGE_LEVEL + DataMover 后台迁移）；扩展 `engine/src/db/storage/ts/ts_engine.c` 实现查询路由（按时间区间选 Level 0/1/2）。
- **投入**：约 3000-5000 行 C，1 人 2 周开发 + 1 周测试。
- **学习资源**：[tdengine.md §7.1](../db-wiki/time-series/tdengine.md)（Last Cache 跳表）+ [tdengine.md §9](../db-wiki/time-series/tdengine.md)（多级存储）+ [tdengine.md §16.2](../db-wiki/time-series/tdengine.md)（TSDB 源码必读）。
- **验收标准**：
  - `ALTER DATABASE mydb KEEP 90,365,3650; STORAGE_LEVEL 0,1,2;` 配置端到端可用；
  - `SELECT LAST_ROW(value) FROM cpu` 亚毫秒级响应（Last Cache 命中）；
  - Level 0 → Level 1 → Level 2 自动迁移（后台 DataMover 每 5 分钟检查）；
  - 查询按时间区间自动选 Level 0（最近 90 天）/Level 1（90-365 天）/Level 2（> 365 天）；
  - `engine/test/db/storage/ts/multi_level_test.cpp` ≥ 6 用例（migrate/query/cleanup）通过。
- **优先级**：**P1**（决定 §4.4 与 TDengine IoT 大数据场景对标）。

### 7.5 P2 — 启动乱序写入容忍协议 + SMA 流式聚合（可推到下季度，约 3 周）

- **动作**：在 `engine/src/db/storage/ts/` 下新建 `ts_out_of_order.{c,h}`（`max-out-of-order-write-delay` 配置 + Block 内排序 + 数据搬迁）+ `ts_sma.{c,h}`（流式聚合订阅源表增量 + 实时 Rollup + Offset 管理）；扩展 `engine/src/db/storage/ts/ts_continuous_agg.c` 复用连续聚合思路。
- **投入**：约 3000-5000 行 C，1 人 2 周开发 + 1 周测试。
- **学习资源**：[influxdb.md §十二](../db-wiki/time-series/influxdb.md)（事务与乱序写入）+ [tdengine.md §10.2](../db-wiki/time-series/tdengine.md)（乱序写入处理）+ [tdengine.md §8.1](../db-wiki/time-series/tdengine.md)（SMA 流式聚合）。
- **验收标准**：
  - `SET PARAM max_out_of_order_delay = '15m';` 配置端到端可用；
  - 延迟窗口内乱序数据自动落盘并参与合并；
  - 超过窗口的乱序数据**拒绝写入**并返回明确错误；
  - `CREATE SMA TABLE rollup_5m AS SELECT _wstart AS ts, AVG(value) FROM cpu INTERVAL(5m);` 端到端可用；
  - `engine/test/db/storage/ts/out_of_order_test.cpp` ≥ 6 用例（窗口内/窗口外/迁移）+ `engine/test/db/storage/ts/sma_test.cpp` ≥ 4 用例通过。
- **优先级**：**P2**（决定 §4.5 与 IoT 多边缘节点场景上限，可推到下季度）。

---

## 8. 附录 A — 自研与 InfluxDB / TDengine / QuestDB 路径对照表

| 自研路径 | InfluxDB 对照路径 | TDengine 对照路径 | QuestDB 对照路径 | 简述 |
|----------|-------------------|-------------------|------------------|------|
| `engine/src/db/storage/ts/ts_engine.c` | `tsdb/engine/tsm1/engine.go:WritePoints` | `source/dnode/vnode/src/tsdb/tsdbCommit.c/tsdbCommit2.c` | `io.questdb.cairo.CairoEngine` | 时序引擎主入口 |
| `engine/src/db/storage/ts/ts_compress.c` | `tsdb/engine/tsm1/encoding.go`（Gorilla + Simple8b + RLE） | `source/dnode/vnode/src/tsdb/tsdbDataFileRW.c`（Delta-of-Delta + Gorilla） | `io.questdb.cairo.Compact*`（Delta-of-Delta + Simple8b + Gorilla） | 压缩算法（5 种） |
| `engine/src/db/storage/ts/ts_segment.c` | `tsdb/engine/tsm1/file_store.go`（TSM File） | `source/dnode/vnode/src/tsdb/tsdbMemTable.c`（MemTable + Data File） | `io.questdb.cairo.TableWriter` | 段/块抽象 |
| `engine/src/db/storage/ts/ts_partition.c` | `tsdb/engine/tsm1/engine.go`（Shard Group） | `source/dnode/vnode/src/tsdb/tsdbCommit.c`（VNode partition） | `io.questdb.cairo.PartitionPurgeJob` | 分区管理 |
| `engine/src/db/storage/ts/ts_retention.c` | `tsdb/engine.go`（Retention Policy）+ `store.go:DeleteShard` | `source/dnode/vnode/src/meta/metaTtl.c`（TTL Job） | `io.questdb.cairo.PartitionPurgeJob` | 保留策略 |
| `engine/src/db/storage/ts/ts_continuous_agg.c` | `services/continuous_querier`（CQ 1.x）+ `task/`（Tasks 2.x） | `source/dnode/vnode/src/sma/smaRollup.c` | `io.questdb.griffin.engine.groupby.MatViewRefreshJob` | 连续聚合 |
| `engine/src/db/storage/ts/ts_mview.c` | N/A（依赖 task） | N/A（SMA 已涵盖） | `io.questdb.griffin.engine.groupby.MatViewRefreshJob` | 物化视图 |
| `engine/src/db/storage/ts/ts_label_index.c` | `tsdb/index/tsi1/log_file.go`（LogFile）+ `index_file.go`（IndexFile） | N/A（Tag 即 Schema） | `io.questdb.cairo.SymbolMap`（Symbol 列） | 标签/Symbol 索引 |
| `engine/src/db/storage/ts/ts_tag_index.c` | `tsdb/index/tsi1/series_file.go`（SeriesFile） | N/A | `io.questdb.cairo.BitmapIndexWriter`（Bitmap） | Tag 倒排索引 |
| `engine/src/db/storage/ts/ts_sql_functions.c` | `influxql/parser.go` + `storage/reads/`（InfluxQL）+ `flux/runtime/`（Flux） | `source/libs/parser/` + `source/libs/executor/` | `io.questdb.griffin.SqlParser` + `SqlOptimiser` + `SqlCodeGenerator` | 时序 SQL 扩展 |
| `engine/src/db/storage/ts/ts_columnar.c` | N/A（TSM File 内部已有列存） | `source/dnode/vnode/src/tsdb/tsdbDataFileRW.c`（3 列存储） | `io.questdb.cairo.TableWriter`（Native Columnar） | 时序列存特化 |
| `engine/include/db/ts_engine.h` | N/A（daemon API 嵌入式） | N/A（taosc 客户端 API） | N/A（Cairo 嵌入式 API） | 公共头 |
| `engine/src/db/storage/page/page.c` | `tsdb/engine/tsm1/file_store.go`（page/block） | `source/dnode/vnode/src/tsdb/tsdbMemTable.c`（MemTable page） | `io.questdb.cairo.PageFrame`（PageFrame 4096） | 页结构（多模型复用） |
| `engine/src/db/storage/buffer/bufmgr.c` | N/A（依赖 mmap） | N/A（依赖 mmap） | N/A（依赖 mmap） | 缓冲池（多模型复用） |
| `engine/src/db/storage/wal/wal.c` | `tsdb/engine/tsm1/wal.go`（mmap WAL） | `source/dnode/vnode/src/tq/tq.c`（TQ WAL） | `io.questdb.cairo.wal.seq`（WAL 序列） | WAL（多模型复用） |
| `engine/src/db/storage/catalog/catalog.c` | `tsdb/index/tsi1/index_file.go`（measurement index） | `source/dnode/mnode/sdb/`（Meta + RAFT） | `io.questdb.cairo.CairoEngine`（TableRegistry） | Catalog（多模型复用） |

## 9. 附录 B — 章节与 24 维度映射

| 报告章节 | 对应维度 |
|----------|----------|
| §1 自研现状 | 23、24 |
| §2.1 InfluxDB | 1-3、5、7-11、13-17、19-22 |
| §2.2 TDengine | 1-3、5、7-8、10、13-15、17、19-22 |
| §2.3 QuestDB | 1-3、5、7-10、12-15、17、19-22 |
| §3 矩阵 | 全部 24 维度 |
| §4.1 TSM1 引擎家族 | 7、8 |
| §4.2 协议层 | 6、15、18、21 |
| §4.3 时序 SQL 扩展 | 10、11 |
| §4.4 多级存储 | 7、8 |
| §4.5 乱序写入 + 复制 HA | 13、17、14 |
| §5.1 压缩齐全 | 11 |
| §5.2 连续聚合 + MV | 12 |
| §5.3 多模型复用 + 时序列存 | 18、23 |
| §6 学习路径 | 22 |
| §7.1 TSM1 | 7 |
| §7.2 协议层 | 15、18 |
| §7.3 时序 SQL | 10 |
| §7.4 多级存储 | 7、8 |
| §7.5 乱序 + SMA | 13、17 |
| §8 路径对照 | 23、24 |
| §9 本附录 | 全部维度 |

## 10. 元信息

- 报告作者：subagent-driven-development（gap-analysis Task 7 implementer）
- 关联文档：
  - `archive/.superpowers/sdd/gap-task-1-inventory.md` §5 time-series（行 42 索引 + 附录）
  - `docs/process/plans/2026-10-16-gap-analysis-implementation.md` Task 7
  - `docs/analysis/2026-08-29-multimodal-db-gap-analysis.md`（旧版顶层分析，本篇不替代，仅纵向对照）
  - `docs/analysis/relational.md`（姊妹篇，relational 子报告）
  - `docs/analysis/key-value.md`（姊妹篇，key-value 子报告）
  - `docs/analysis/document.md`（姊妹篇，document 子报告）
  - `docs/analysis/columnar.md`（姊妹篇，columnar 子报告）
  - `docs/analysis/time-series.md`（本报告，time-series 子报告）
- 行数目标：300-500（本篇实测见 §11 自检）
- 版本：v1（首稿，无迭代历史）

## 11. 自检（提交前 5 条验收）

- [ ] **24 维度对照矩阵完整**：YES（§3 已列 24 行，含自研/InfluxDB/TDengine/QuestDB/等级五列）
- [ ] **Top-5 差距每条有具体代码路径/wiki 引用**：YES（§4 五条均含 `engine/src/db/storage/ts/...` 自研路径 + InfluxDB/TDengine/QuestDB wiki 路径）
- [ ] **补齐项下月计划至少 3 条 P0/P1**：YES（§7.1 P0、§7.2 P0、§7.3 P0、§7.4 P1、§7.5 P2，共 4 条 P0/P1 + 1 条 P2）
- [ ] **至少 5 处 engine/ 真实路径引用**：YES（§1 列 12 条路径、§4 列 5 条、§7 引用 14 条、§8 表格 16 条，**总计 ≥ 47 处真实 engineering 路径引用**）
- [ ] **至少 2 个 wiki 链接引用**：YES（§6 列出 6 个 wiki 章节链接，§2 与 §7 多处内联 wiki 相对路径，**总计 ≥ 14 处 wiki 链接引用**）
- [ ] **目标行数 300-500**：实测见下表

行数实测（章节）：
- §1 自研现状：~55 行
- §2 业界主流：~75 行
- §3 24 维度对照矩阵：~32 行
- §4 Top-5 差距：~75 行
- §5 优势 Top-3：~25 行
- §6 学习路径：~30 行
- §7 补齐项下月计划：~80 行
- §8-§11 附录+元信息+自检：~70 行
- **合计：~440 行 markdown**（在 300-500 行目标区间内）

---

> 注意：本报告是基于 2026-10-16 自研现状的静态差距分析；后续 MM3/M5（mini-vecdb 端到端 plan）或 Phase 9 分布式演进完成时需重读并刷新 §1、§3、§4 与 §7。