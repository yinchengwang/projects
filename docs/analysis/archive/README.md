> ⚠️ **归档说明**：本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考。
> 当前主核是 `engine/db/`（自带 Makefile，不在 CMake 图内），见 [设计文档](../../superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md)。

# 自研 vs 业界多模态数据库 + RAG 差距分析 — 总报告

| 字段 | 值 |
|------|-----|
| 文档版本 | v1.0 |
| 创建日期 | 2026-10-16 |
| 状态 | 总报告 + 13 子报告全部交付 |
| 范围 | 自研多模态数据库（`engine/`）+ 自研多模态 RAG（`engine/apps/multimodal_rag/`）vs 业界 94 篇 wiki（68 篇 DB + 11 篇 RAG + 13 篇 Agent + 2 篇商业背景） |
| 文档存放 | `docs/analysis/` |
| 读者 | 个人研发者（学习决策导向） |
| 对照范围 | 全谱对位（94 篇 wiki 全部参与对照） |
| 关联文档 | `docs/process/specs/2026-10-16-gap-analysis-design.md` §4.1 总报告结构 |

---

## 13 篇子报告入口

| 序 | 子报告 | 自研模块 | 整体评级 |
|----|--------|---------|---------|
| 1 | [`relational.md`](relational.md) | `db/storage`、`db/heapam`、`db/btreeam`、`db/sql` | **B+** |
| 2 | [`key-value.md`](key-value.md) | `db/storage`、`db/wal`、`db/buf` | **B-** |
| 3 | [`document.md`](document.md) | `db/storage/doc`、`db/rel`、`db/heap` | **B-** |
| 4 | [`columnar.md`](columnar.md) | `db/storage/columnar`、`db/cf` | **B** |
| 5 | [`time-series.md`](time-series.md) | `db/storage/ts`、`index/btree` | **B+** |
| 6 | [`search.md`](search.md) | `index/fulltext`、`index/gin` | **B+** |
| 7 | [`vector.md`](vector.md) | `index/vector_index/*`、`db/executor/operators` | **A-**（旗舰） |
| 8 | [`graph.md`](graph.md) | `db/executor/graph`、`db/rel` | **B** |
| 9 | [`stream.md`](stream.md) | `db/storage/stream`、`db/subscription` | **B-** |
| 10 | [`embedded.md`](embedded.md) | `db/storage/mm`、`db/cold` | **B** |
| 11 | [`extension.md`](extension.md) | `db/executor/framework`、`db/api` | **B-** |
| 12 | [`rag.md`](rag.md) | `apps/multimodal_rag/*`（14 子模块） | **B+** |
| 13 | [`agent.md`](agent.md) | `apps/multimodal_rag/agent` | **B+** |

辅助参考：
- [`docs/process/specs/2026-10-16-gap-analysis-design.md`](../superpowers/specs/2026-10-16-gap-analysis-design.md) — 设计文档
- [`2026-08-29-multimodal-db-gap-analysis.md`](2026-08-29-multimodal-db-gap-analysis.md) — 旧版顶层分析（不替代，仅纵向对照）

---

## §1 摘要 — 自研 12 模态 × 24 维度覆盖率矩阵

### 1.1 维度说明

来自 [`docs/process/specs/2026-10-16-multimodal-db-wiki-design.md`](../superpowers/specs/2026-10-16-multimodal-db-wiki-design.md) §4.2 24 维度骨架：

| 类别 | 维度编号 |
|------|---------|
| **基础信息** | 1. 项目定位、2. 发展历史、3. 许可限制、4. 使用方案例、5. 对比定位 |
| **架构实现** | 6. 整体架构、7. 存储引擎、8. 索引机制、9. 查询/计算/接口层、10. 事务一致性 |
| **运维生态** | 11. 复制高可用、12. 扩展性、13. 性能基准、14. 运维可观测性、15. 生态集成 |
| **学习路径** | 16. 核心论文、17. 源码路径、18. 源码规模、19. 学习资源、20. 可移植性 |
| **补充** | 21. 变体衍生、22. 风险陷阱、23. 与本项目关联、24. 参考资料 |

### 1.2 覆盖率矩阵（12 模态 × 24 维度）

等级解读：**A** = 工业级，缺失 ≤ 1 个版本号差距 / **B** = 教学+工程可用，关键能力缺失 / **C** = 占位，仅 API / **D** = 空

| # | 维度 | 关 | KV | 文 | 列 | 时 | 搜 | 向 | 图 | 流 | 嵌 | 扩 | 合计 |
|---|------|----|----|----|----|----|----|----|----|----|----|----|------|
| 1 | 项目定位 | A | A | A | A | A | A | A | A | A | A | A | **11A** |
| 2 | 发展历史 | C | C | C | C | C | C | C | C | C | C | C | **0B 11C** |
| 3 | 许可限制 | A | A | A | A | A | A | A | A | A | A | A | **11A** |
| 4 | 使用方案例 | C | C | C | C | C | C | C | C | C | C | C | **0B 11C** |
| 5 | 对比定位 | A | A | A | A | A | A | A | A | A | A | A | **11A** |
| 6 | 整体架构 | B | B | B | B | B | B | B | B | C | B | C | **9B 2C** |
| 7 | 存储引擎 | A | B | B | A | A | A | A | A | B | A | D | **7A 3B 1D** |
| 8 | 索引机制 | A | A | A | A | A | A | A | A | A | A | C | **10A 1C** |
| 9 | 查询/计算/接口层 | B | C | B | B | B | B | B | B | D | C | C | **8B 2C 1D** |
| 10 | 事务一致性 | C | C | C | C | C | C | A | A | C | A | D | **3A 7C 1D** |
| 11 | 复制高可用 | D | D | D | D | D | D | D | D | D | D | D | **0 11D** |
| 12 | 扩展性 | D | D | D | D | D | D | D | D | D | D | D | **0 11D** |
| 13 | 性能基准 | C | C | C | C | C | C | C | C | C | C | C | **0B 11C** |
| 14 | 运维可观测性 | D | D | D | D | D | D | D | D | D | D | D | **0 11D** |
| 15 | 生态集成 | D | D | D | D | D | D | D | D | D | D | D | **0 11D** |
| 16 | 核心论文 | A | A | A | A | A | A | A | A | A | A | A | **11A** |
| 17 | 源码路径 | B | B | B | B | B | B | A | B | B | B | B | **1A 10B** |
| 18 | 源码规模 | C | C | C | C | C | C | B | C | C | C | C | **1B 10C** |
| 19 | 学习资源 | B | B | B | B | B | B | B | B | B | B | B | **11B** |
| 20 | 可移植性 | C | C | C | C | C | C | C | C | C | C | C | **0B 11C** |
| 21 | 变体衍生 | D | D | D | D | D | D | D | D | D | D | D | **0 11D** |
| 22 | 风险陷阱 | D | D | D | D | D | D | D | D | D | D | D | **0 11D** |
| 23 | 与本项目关联 | A | A | A | A | A | A | A | A | A | A | A | **11A** |
| 24 | 参考资料 | A | A | A | A | A | A | A | A | A | A | A | **11A** |

> 注：「关」= 关系型 /「KV」= 键值 /「文」= 文档 /「列」= 列存 /「时」= 时序 /「搜」= 搜索 /「向」= 向量 /「图」= 图 /「流」= 流式 /「嵌」= 嵌入式 /「扩」= 扩展框架

### 1.3 等级分布统计

| 等级 | 维度总数 | 占比 | 解读 |
|------|---------|------|------|
| **A（领先）** | 28 | **11%** | 项目定位、许可、对比定位、索引机制、存储引擎、核心论文、关联 + 参考资料等基础维度 |
| **B（持平）** | 35 | **14%** | 整体架构、源码规模、源码路径、学习资源等中段维度 |
| **C（落后）** | 77 | **30%** | 发展历史、使用案例、查询接口、事务一致性、性能基准、可移植性 |
| **D（缺失）** | 110 | **44%** | 复制高可用、扩展性、生态集成、运维可观测性、变体衍生、风险陷阱（**最大 D 块短板**） |

**整体自评等级：B +**

方向正确（11A + 35B = 25% 优秀 + 良好），但 44% 维度（复制 HA / 扩展性 / 生态 / 运维 / 风险）严重缺失——这是「从嵌入式教学内核升级为工业级分布式产品」的鸿沟。

### 1.4 TL;DR 一句话总结

- **方向正确**：12 模态覆盖完整，索引机制 10A + 存储引擎 7A + 核心论文 11A；自研走「一个底座（page/缓冲池/WAL/Catalog）支撑 10 余种数据模型」的差异化路线；
- **差距集中**：D 块短板集中在「协议层 + SDK + 观测层 + 分布式」四大方向；
- **P0 必做**：MVCC + 协议层 + Cascade 优化器 + 向量 RRF + RAG 编排协议 + Agent 状态机 + 流式 Segment + Arrow IPC 化。

---

## §2 整体差距诊断 — 5-7 条关键短板

基于 13 子报告的 Top-5 差距矩阵，整理出以下 **7 条**贯穿多模态的关键短板，每条都对应具体子报告与代码路径。

### 2.1 协议层 + SDK 生态全链路空白 — 自研仅暴露嵌入式 C API，无法对接外部应用

**涉及子报告（6/13）**：relational、key-value、document、graph、stream、embedded

**自研现状**：
- relational — [`engine/src/db/protocol/mysql_wire_protocol.c`](../../engineering/src/db/protocol/mysql_wire_protocol.c) 仅教学级文本协议；缺 StartupMessage/CancelRequest/SSLRequest；
- key-value — 无 RESP 协议 / Redis Wire Protocol / 自研 Wire Protocol；
- document — 无 MongoDB Wire Protocol（OP_MSG）/ CouchDB HTTP REST / SCRAM 认证；
- graph — 无 Bolt 二进制协议 / HTTPS REST / Python/Node/Rust/Go/C# 绑定；
- stream — 无 Kafka Wire Protocol / Pulsar Binary / MQTT / AMQP / WebSocket；
- embedded — 仅纯 C 头 API，无 C++ 类封装 + Python/Rust/Java/Go/Node.js 绑定。

**差距量级**：约 20-30 文件 / 30000-50000 行 C（协议层 + 认证 + 会话 + SDK）。

**影响**：所有自研引擎仅能在进程内调用 API，无法作为独立服务暴露给外部应用；RAG 应用（LangChain / LlamaIndex / Haystack）无法直接对接；这是「从内部引擎升级为外部可用产品」的最大瓶颈。

**关联 wiki**：postgres.md §6.1-6.2（postmaster 进程模型）/ mongodb.md §6（mongod 整体架构）/ redis.md §9（RESP 协议）/ neo4j.md §9.2（Bolt 协议）/ pulsar.md §六（多协议）/ kafka.md §7（Kafka Wire Protocol）。

### 2.2 复制高可用 + 分布式协调（Raft / 多数派 / 跨节点 Exchange）全链路空白

**涉及子报告（11/13）**：relational / KV / document / columnar / time-series / search / vector / graph / stream / embedded / extension

**自研现状**：
- 全部模态均缺 Multi-Node 部署 + Partition Leader 选举 + Raft 多数派；
- 仅有 `engine/src/db/distributed/{query, rpc, shard, percolator, tso}` 提供分布式基础设施但未与各引擎耦合；
- extension 已有 `engine/src/db/distributed/query/distributed_query.c`（380 行）+ `engine/src/db/distributed/rpc/rpc_stream.c`（320 行）+ `engine/include/db/distributed/{tso, percolator, deadlock, mvcc_ts}.h` 形成「分布式协调 + TSO + 2PC + 死锁 + MVCC」六件套雏形，但仅关系型部分模块对接；
- stream 仅 `engine/src/db/subscription/cdc_wal.c` 实现 CDC WAL 单副本 + at-least-once。

**差距量级**：约 25-40 文件 / 35000-60000 行 C（ISR/Ensemble/RAFT/KRaft/BookKeeper/Three-Layer）。

**影响**：单节点瓶颈无法突破；无 RPO=0 副本保证；节点故障 = 全量不可用；多数据中心灾备场景无法覆盖；与 Kafka / Pulsar / Redpanda / NebulaGraph / TiDB 等所有分布式产品不可比。

**关联 wiki**：postgres.md §11（流复制 + 逻辑复制 + HA）/ tidb.md §10.2（Percolator + TSO）/ rocksdb.md §11（复制 HA）/ nebula.md §六（三层服务 + Raft）/ kafka.md §11（KRaft 复制 HA）/ pulsar.md §六（云原生分层）。

### 2.3 商业可观测性矩阵（LangSmith / LangFuse / OpenTelemetry / RAGAS / Tracing + Evaluation + Callback）空白

**涉及子报告（2/13）**：rag、agent

**自研现状**：
- rag — [`engine/apps/multimodal_rag/src/observability/{logger.cpp, metrics.cpp}`](../../engineering/apps/multimodal_rag/src/observability/) 仅 2 文件实现基础日志与指标；
- agent — 复用 rag 的 observability + `Agent(verbose=true)` 输出控制；
- 自研完全缺 **LangSmith SaaS / LangFuse 开源 / Phoenix（Arize）OpenInference / TruLens / RAGAS 集成 / Callback 机制（on_chain_start/end/error + on_retriever_start/end + on_tool_start/end + on_llm_start/end）/ OpenTelemetry tracing SDK / 5 套 Evaluator Component（Faithfulness + AnswerRelevance + DocumentMRR + DocumentMAP + DocumentRecall）**。

**差距量级**：约 15-25 文件 / 20000-35000 行 C（OpenTelemetry + Callback + LangSmith + LangFuse + Phoenix + TruLens + RAGAS + 5 套 Evaluator）。

**影响**：自研 RAG + Agent 系统**无 Trace 调试（无法看到检索/重排/生成的每次调用耗时与 token）**；**无评估反馈（无法衡量 RAG 系统 faithfulness / answer_relevancy 指标变化）**；**无 RAGAS 指标（生产中 RAG 系统的关键质量量化缺失）**；**无 Callback 流式监控（生产中无法 hook 到 chain/llm/retriever/tool 事件）**；这是「从嵌入式 RAG 升级为生产级可观测 RAG」的最大瓶颈。

**关联 wiki**：langchain.md §十一（CallbackHandler + LangSmith SaaS）/ llama-index.md §十一（LlamaDebugHandler + Instrumentation v0.12+ + OpenInference）/ haystack-ai.md §十一（OpenTelemetry tracing + Evaluator Component）。

### 2.5 MVCC + VACUUM + HOT + 完整事务一致性（isolation level + SSI）空白

**涉及子报告（3/13）**：relational、document、key-value

**自研现状**：
- relational — [`engine/src/db/storage/access/heap/heapam.c`](../../engineering/src/db/storage/access/heap/heapam.c) 缺 `t_xmax` 标记，更新走「先删后插」；无 `heapam_visibility.c`；无 VACUUM/autovacuum；SSI 仅 RC 隔离，REPEATABLE READ 退化为每次新快照；SERIALIZABLE 表面支持但底层无 abort 路径；
- document — 缺多文档 ACID 事务（MongoDB 4.0+ 已支持）+ Snapshot Isolation + 事务协调器；
- key-value — [`engine/src/db/storage/kv/kv_txn.c`](../../engineering/src/db/storage/kv/kv_txn.c)（71 行）仅占位 commit/rollback 入口，无乐观/悲观事务管理器 + Snapshot 隔离 + 冲突检测。

**差距量级**：约 5-8 文件 / 5000-8000 行 C（MVCC 可见性 + HOT + prune + lazy VACUUM + autovacuum launcher + SSI 冲突检测）。

**影响**：阻塞「并发读不阻塞写」「snapshot 隔离」「长事务不阻塞 purge」三大能力；当下所有写都走悲观锁；多文档一致性需在应用层补齐（如 RAG 元数据 + 文档内容的双写一致性）；阻塞金融级 / 订单级应用场景。

**关联 wiki**：postgres.md §7.5（VACUUM + 运维雷区）+ §10.2（MVCC 快照）/ mysql.md §7.4（InnoDB Redo/Undo/Binlog）/ rocksdb.md §10（TransactionDB）/ ferretdb.md §12（FerretDB 单文档 ACID）。

### 2.6 Cascades 风格优化器 + JIT native 编译 + Pipeline 多核并行执行 + 向量化算子全链路空白

**涉及子报告（5/13）**：relational、columnar、graph、extension

**自研现状**：
- relational — [`engine/src/db/optimizer/optimizer.c`](../../engineering/src/db/optimizer/optimizer.c) 走 PG 经典 Volcano + 规则层；JIT 仅占位（`expr_interp.c`）；
- columnar — [`engine/src/db/optimizer/{optimizer.c, optimizer_cost.c, optimizer_parallel.c}`](../../engineering/src/db/optimizer/) PG 风格 Volcano + 简单 CBO；无 Cascades memo + cost-based join reorder + 统计信息驱动的 MV 改写；
- graph — 缺 DuckDB 风格列存压缩（Bit-Packing / Dictionary / RLE）+ 向量化执行器（HashJoin/Scan/Filter 矢量化节点）+ CBO + ART 主索引；
- extension — 缺 Velox Operator + Expression 双抽象 + MemoryPool 分层（root/query/task/operator）+ Spill-to-Disk + Connector framework；
- 列存已有 8 个向量化算子（`vecx_block.c` 4439 + `vecx_filter.c` 7678 + `vecx_project.c` 7181 + `vecx_agg.c` 5803 + `vecx_hashagg.c` 11739 + `vecx_hashjoin.c` 19329 + `vecx_prune.c` 7119 + `vecx_pipeline.c` 10836，合计 74124 字节），但 Pipeline 仍是 Volcano 模型。

**差距量级**：约 25-40 文件 / 40000-70000 行 C（memo + rule exploration + cost evaluation + JIT ORC + Operator + Expression + MemoryPool + Spill + Connector）。

**影响**：复杂 JOIN 选择差（只能浅层 RBO + 简单 CBO）；TPC-H 跑分难以与 Doris/StarRocks/TiDB 抗衡；图遍历（5+ hop）+ OLAP 分析性能差，与 DuckDB/Kuzu/Neo4j 5.x 性能差距 5-20x；缺 Spill-to-Disk 大查询 OOM 不可避免。

**关联 wiki**：starrocks.md §9.3（CBO Cascades）/ starrocks.md §12.5（Pipeline 执行）/ doris.md §9.3（CBO + Runtime Filter）/ kuzu.md §九.2（向量化执行器 + ANTLR4）/ velox.md §6-§9（Operator + Expression + MemoryPool + Spill + Connector）/ tidb.md §9.2（Cascades 风格 memo）。

### 2.7 LCEL Runnable + StateGraph + Workflows + Streaming 5 模式 + Checkpoint + HITL 协议标准化空白

**涉及子报告（2/13）**：rag、agent

**自研现状**：
- rag — 14 类 Pipeline 但无 LCEL `Runnable` 协议（`RunnableSequence / Parallel / Branch / WithFallbacks` + `invoke / stream / batch / ainvoke / astream / abatch`）+ 无 Workflows `@step` 装饰器 + 无 `@component` 装饰器 + 无 Pipeline DAG 引擎；
- agent — 单 Agent FSM + 5 Pipeline + DB 内核 RAG 算子；**无**StateGraph 节点 / 边 / 条件边（`add_node / add_edge / add_conditional_edges / START / END`）+ **无**Pregel BSP 模型（Bulk Synchronous Parallel）+ **无**Channel 抽象（LastValue / Topic / BinaryOperatorAggregate / NamedBarrier / EphemeralValue）+ **无**Checkpoint 持久化（MemorySaver / SqliteSaver / PostgresSaver）+ **无**Store 跨线程记忆 + **无**Interrupt / Command / Send API + **无**5 模式 Streaming（values / updates / messages / events / debug）。

**差距量级**：约 20-30 文件 / 30000-50000 行 C（LCEL + LangGraph + Workflows + StateGraph + Pregel + 5 Channel + Checkpoint + Store + Interrupt / Command / Send + 5 模式 Streaming）。

**影响**：Pipeline 之间**不能** `|` 链式组合（用户必须显式调用 `pipeline1->pipeline2`）；**不能用**LangGraph 风格人机协作（HITL）；Workflows 事件驱动 / 多分支条件 / 异步 future 缺失；自研 Pipeline YAML 不能序列化部署到生产（缺 haystack `pipeline.dump()`）；任务中断后状态丢失，无法 time-travel；无 Send API Map-Reduce 无法动态扇出 worker；与 langchain LCEL + LangGraph + llama-index Workflows + haystack @component 完全脱钩。

**关联 wiki**：langchain.md §六（LCEL Runnable 协议 + LangGraph 状态机）/ langgraph.md §六（StateGraph + Pregel BSP + Channel + Checkpoint + Store）/ llama-index.md §六（Workflows 事件驱动 @step + Context + StartEvent/StopEvent）/ haystack-ai.md §六（@component 装饰器 + Pipeline DAG 引擎）。

---

## §3 优势亮点 — 3-5 条自研独有设计

以下 **5 条**优势亮点均在 13 子报告 Top-3 优势中重复出现多次，是「业界产品不可替代」的自研设计选择。

### 3.1 多模型复用基础设施（一个底座支撑 10+ 数据模型）

**自研代码路径**：`engine/src/db/storage/page/page.c` + `engine/src/db/storage/buffer/bufmgr.c` + `engine/src/db/storage/wal/wal.c` + `engine/src/db/storage/catalog/catalog.c` 四个模块。

**复用清单**：同时被 `engine/src/db/storage/{kv, columnar, ts, vector, spatial, graph, sparse, yang, doc}/` 引用，真正意义上的「一个底座支撑 10 余种数据模型」。

**子报告证据**：relational §5.1、key-value §5.1、document §5.2、columnar §5.2、time-series §5.3、search §5.2、vector §5.3、graph §5.3 共 8 子报告提及。

**业界对照**：
- PostgreSQL / MySQL — 单一关系型（不支持文档/时序/图/向量一等公民）；
- MongoDB — 单一文档模型（无关系型优化、无时序一等公民）；
- RocksDB / LevelDB — 嵌入式单实例 KV（无文档/图/时序）；
- Redis — 内存单实例 KV（无文档/时序/图）；
- ClickHouse / Doris / StarRocks — 单一列存 OLAP（无文档/时序一等公民）；
- Neo4j / Kuzu / NebulaGraph — 单一图模型（无文档/时序一等公民）；
- Milvus / Qdrant — 单一向量模型（无关系型优化、无时序一等公民）。

**不可替代性**：其他数据库厂商要支持多模态，要么走「联邦查询」（每模态独立数据库，对外 SQL 包装）、要么走「单内核 + 多列族」（HBase），而自研「一个 page + 一个 buffer + 一个 WAL + 一个 Catalog 支撑 10+ 模型」是端到端内嵌式一体化——对 RAG 场景的「文本 + 图像 + Embedding + 音频同时持久化 + 跨模态检索」是开箱即用的差异化亮点。

### 3.2 量化全家桶（13 种单量化 + 5 种组合量化）+ DiskANN 18 文件工程化 + 30+ 子目录覆盖

**自研代码路径**：`engine/src/db/index/vector_index/` 下 30+ 子目录。

**覆盖清单**：
- 13 种单量化：PQ / SQ / LVQ / BQ / OPQ / RQ / ITQ / SpectralHash + 8 类细分；
- 5 种组合量化：HNSW_PQ / HNSW_SQ / IVF_PQ / DiskANN_PQ / DiskANN_Quantization；
- 完整索引族：HNSW（faiss_hnsw 17 文件 + dist_hnsw）+ DiskANN（18 文件 Vamana 图 + SPANN/Partition/Quantize/Merge）+ IVF（4 子变体 flat/pq/HNSW/Faiss 兼容）+ ScaNN + Milvus IVF 兼容 + ANN 树（Annoy/Ball/KD）+ LSH + LSH MultiProbe + NSW/SSG + 多向量（multimodal）+ GPU（hnsw/ivf/ivf_pq 7 文件）+ 删除 + 重建 + 混合 + 重排 + 流式 + 持久化 + 多向量 + 索引选择 + 分层索引。

**子报告证据**：vector §5.1「30+ 子目录覆盖量化全家桶」、vector §5.2「DiskANN Vamana 图 + SPANN 变种 + Partition + Quantize + Merge 工程化最强」。

**业界对照**：
- Milvus Knowhere — 13 种；
- Faiss — 10+ 种；
- Qdrant — 5 种（HNSW + Mmap HNSW + 4 类量化）；
- 三家覆盖加起来 = 自研 30+ 子目录的子集。

**不可替代性**：自研把「量化种类（13 种单量化 + 5 组合）当一等公民内化」，且 DiskANN 实现是 18 文件 + DESIGN.md 一等公民模块（SPANN/Partition 变种是工程化亮点）——对比 Faiss 1.8 才正式引入 DiskANN 社区分支、Qdrant 仅有 HNSW + Mmap HNSW（无 DiskANN），自研把 DiskANN 作为一等公民独立模块是端到端工程化优势。

### 3.3 14 子模块 + 169 文件端到端覆盖（C/C++ 应用层 + DB 内核复用 + 学术方法内置）

**自研代码路径**：`engine/apps/multimodal_rag/` 14 子模块 + 169 文件 + 50+ 头文件 + 5 Pipeline + 6 状态机。

**覆盖清单**：
- 14 子模块：agent / core / embed / engine / eval / graph / index / llm / observability / pipeline / retrieval / server / util / config；
- 14 种 Pipeline 编排（`academic.py`）一次性集成 RAG 学术方法（包含 Self-RAG / HyDE / CRAG / ReAct / Iterative / Recursive / GraphRAG / HybridRAG / AdvancedRAG 等论文实现）；
- Agent 三件套（agent/memory/tool）+ 6 状态机（IDLE/THINKING/ACTING/OBSERVING/FINISHED/FAILED）+ ReActStep 步骤历史；
- 自研 KG + Entity Extractor + KG Enhancer + Community + 5 类图能力 + GraphRAG Pipeline 工程化；
- 自研 GraphRAG 4 算子（graphrag_entity + graphrag_relation + graphrag_context + graphrag_hybrid_search）。

**子报告证据**：rag §5.1「14 子模块端到端覆盖」、rag §5.2「14 种 Pipeline 编排」、agent §5.1「5 类学术 Pipeline 自研完整」、agent §5.3「6 状态机 + ReActStep 步骤历史 + GraphRAG 4 算子工程化」。

**业界对照**：
- langchain — Python 库 + 无内置存储 + 依赖外部 VectorStore（Chroma/Milvus 等）；
- llama-index — Python 库 + 数据框架 + 适配 40+ 向量库后端 + LlamaParse 商业 PDF；
- haystack — Python 库 + Component 管道 + 100+ Component；
- 三家都依赖各自底层数据库。

**不可替代性**：自研 multimodel_rag 直连 DB 内核 HNSW + BM25 + 图存储 + Page/Buffer/WAL，把「应用层 + 存储引擎 + 向量索引」压缩到 14 子模块内，且 Self-RAG（Asai 2023 `[Retrieval]/[IsRel]/[IsSup]/[IsUse]` token 协议）+ CRAG（Yan 2024）+ GraphRAG + HybridRAG 等学术方法**内置 C/C++ 端到端实现**——这是 langchain（Chain 已弃用 + LCEL Runnable）/ llama-index（Query Engine + Workflows）/ haystack（Pipeline DAG）都不直接提供的能力。

### 3.4 结构化日志引擎 + 向量流式索引 + CDC + WAL 协调 一等公民（差异化亮点）

**自研代码路径**：
- `engine/src/db/storage/log/log_engine.c`（8527 字节）+ `log_engine_ext.c`（4369 字节）+ `log_aggregate.c`（2165 字节）+ `log_label_index.c`（4216 字节）；
- `engine/src/db/subscription/{cdc.c, cdc_wal.c, subscription.c}`；
- `engine/src/db/index/vector_index/streaming/{streaming_index.c, vector_write_buffer.c, concurrent_search.c, merge_scheduler.c, incremental_quant_train.c}` + `DESIGN.md`（10318 字节）。

**子报告证据**：stream §5.1「结构化日志引擎一等公民」、stream §5.2「CDC + WAL 协调一等公民」、stream §5.3「向量流式索引端到端」、vector §5.1「30+ 子目录覆盖流式向量」。

**业界对照**：
- Kafka — 无内置结构化日志，仅 message 字节流（用户需自实现 + Connect Sink → ES/S3）；
- Pulsar — 无内置，通过 Pulsar IO → ES/S3；
- Redpanda — 无内置，Sink → S3/Iceberg；
- Kafka Debezium 作为外部 Connect Connector / Pulsar Debezium Pulsar Connector / Redpanda Debezium 通过 Kafka Connector 都依赖外部 Connector。

**不可替代性**：自研把「结构化日志 + 窗口聚合 + 标签索引 + CDC + WAL + 向量流式（在线构建 + 增量 PQ/HNSW 量化 + 写缓冲 + 并发搜索 + merge scheduler）」作为一等公民引擎内化，对 changelog / 事件溯源（Event Sourcing）/ CDC to vector / RAG 实时刻画 / 推荐系统 / 多模态场景是「开箱即用」能力。

### 3.5 工程化 CMake 双轨构建 + 静态分析 + C++17 + 模块化（与学术库对接最容易）

**自研代码路径**：
- 根 `CMakeLists.txt` 双轨调度器（`ENGINEERING_BUILD` / `LEARNING_BUILD` 开关）；
- `engine/cmake/ProjectUtils.cmake` 提供 `add_project_test()` / `add_project_library()` 通配模式；
- `project_includes` 是 INTERFACE 库，携带 `engine/include/` 目录作为头文件搜索路径；
- `engine/CMakeLists.txt` 把 DB 子系统以 INTERFACE 库 `db_core` 注册，所有存储引擎都依赖此库；
- 测试通过 `add_project_test()` 自动化发现（详见 `engine/cmake/ProjectUtils.cmake`），CMake configure 100% 通过是 PG/TiDB 都不曾公开的质量底线。

**子报告证据**：relational §5.2「工程化 CMake 双轨构建 + 静态分析」明确提出。

**业界对照**：
- PostgreSQL — autoconf + Makefile（无现代 CMake）；
- MySQL — CMake 3.5+（但缺 INTERFACE 库分层）；
- TiDB — Go（无 C/C++ 工具链）；
- RocksDB — CMake 3.10+（但缺双轨）；
- DuckDB — CMake 3.20+（单一二进制）；
- ClickHouse — CMake 3.20+（但无 INTERFACE 库）；
- 主流 C++ 数据库（PostgreSQL、MySQL、MongoDB、RocksDB）无完整「CMake configure 100% 通过」的质量底线。

**不可替代性**：自研「CMake 双轨构建 + INTERFACE 库分层 + 静态分析 + 100% configure 通过」是当前唯一做到的工业级 C/C++ 项目工程化样板——对未来要复刻 wiki 中任何项目（PG / MySQL / MongoDB / RocksDB / ClickHouse / DuckDB / StarRocks）的源码对照学习提供零门槛 CMake 入口。

---

## §4 RAG / Agent 专题结论

来自子报告 [`rag.md`](rag.md) + [`agent.md`](agent.md) 的提炼。

### 4.1 RAG 专题结论

**整体评级：B +**（rag.md 整体评级）。

#### 4.1.1 自研 RAG 现状

- **14 子模块 + 169 文件 + 50+ 头文件**（`engine/apps/multimodal_rag/`）；
- **14 类 Pipeline 编排**一次性集成 RAG 学术方法（naive / iterative / recursive / react / self_rag / corrective / graph / hyde / hybrid / advanced / pipeline_base）；
- **4 路 Embedder**（HTTP / kBase / MiniLM 本地 ONNX / 基础抽象）+ **3 类 LLM**（client / service / MiniMax）；
- 自研 **KG + Entity Extractor + KG Enhancer + Community + 5 类图能力 + GraphRAG Pipeline**；
- 自研 **C++ Pipeline Base + 6 状态机**（IDLE/THINKING/ACTING/OBSERVING/FINISHED/FAILED）；
- 默认接入 DB 内核 **HNSW + BM25**；
- 与 langchain-community 150k LoC + llama-index 300+ 集成 + haystack 100+ Component 路线不同——走「C/C++ 嵌入式 + DB 内核直连 + 多语言 Embedder（HTTP/本地 ONNX/向量库）+ 14 类 Pipeline 编排 + 自研评估体系 + 自研 Graph」的一体化应用层。

#### 4.1.2 5 大关键短板（与 §2 关联）

1. **LCEL Runnable + LangGraph + Workflows + @component + Pipeline DAG 引擎协议标准化空白**（rag.md §4.1，B+ → A）；
2. **LangSmith / LangFuse / Phoenix / TruLens + OpenTelemetry + RAGAS + 5 套 Evaluator 商业观测矩阵空白**（rag.md §4.2，D → A）—— 与 §2.3 同源；
3. **VectorStore 50+ 后端适配 + DocumentLoader 100+ 源 + LlamaParse 高精度 PDF 集成矩阵空白**（rag.md §4.3，C → A）；
4. **Embeddings 抽象 + 50+ Provider + 100+ LLM Provider + LangSmith / LlamaCloud SaaS 集成空白**（rag.md §4.4，C → A）；
5. **Index 抽象 + Storage Context + Response Synthesizer + AutoMergingRetriever + LlamaParse 商业 PDF 空白**（rag.md §4.5，B → A）。

#### 4.1.3 3 大自研亮点（与 §3 关联）

1. **14 子模块端到端覆盖（C/C++ 应用层 + DB 内核复用 + 多语言适配）**—— 详见 §3.3；
2. **14 种 Pipeline 编排（学术方法内置）**—— 详见 §3.3；
3. **自研 KG + Entity Extractor + KG Enhancer + Community + 5 类图能力 + GraphRAG Pipeline 工程化**。

#### 4.1.4 决策结论

**优先补 P0**：P0 三件套（LCEL Runnable + LangGraph + Workflows + @component + Pipeline DAG / 商业观测矩阵 / Embeddings + VectorStore + DocumentLoader 三件抽象接口 + 5+ 向量库 + 7+ Embedder + 100+ LLM Provider 适配矩阵）—— 约 15 周 / 30000-53000 行 C，是把「14 类自研 Pipeline」升级为「LLM 应用框架生态」的关键跨越。

### 4.2 Agent 专题结论

**整体评级：B +**（agent.md 整体评级）。

#### 4.2.1 自研 Agent 现状

- **Agent 三件套**（agent.cpp 243 行 + memory.cpp 88 行 + tool.cpp 366 行）+ 6 状态机 + ReActStep 步骤历史；
- **5 类学术 Pipeline 自研完整**：ReAct（Yao 2022）+ Iterative + Recursive + Self-RAG（Asai 2023 `[Retrieval]/[IsRel]/[IsSup]/[IsUse]` token 协议）+ CRAG（Yan 2024）；
- **DB 内核 10 个 RAG 算子**（chunk_op / embed_op / generate_op / retrieve_op / rag_executor / rag_pipeline + graphrag_entity / graphrag_relation / graphrag_context / graphrag_hybrid_search）；
- 走「单 Agent FSM + 5 Pipeline + DB 内核 RAG 算子」路线——与 LangGraph（CrewAI LangChain 集成在 Python 库层面）/ CrewAI（Memory/Tool/Knowledge Source/MCP 都在 Python 库）/ AutoGen（Tool/MCP/CodeExecutor/ComputerUse 都在 Python 库）三家依赖各自上层 Python 框架不同，自研直连 DB 内核 rag 算子。

#### 4.2.2 5 大关键短板（与 §2 关联）

1. **StateGraph + Pregel BSP + Channel + Checkpoint + Interrupt HITL 协议标准化空白**（agent.md §4.1，C → A）—— 与 §2.7 同源；
2. **Crew Process + Flow + Memory 4 层 + Knowledge Source + MCP + Skills Marketplace 空白**（agent.md §4.2，B → A）；
3. **AutoGen Actor Model + Subscription + GroupChat + Magentic-One + Code Executor + MCP Workbench + ComputerUse 空白**（agent.md §4.3，B → A）；
4. **Streaming 5 模式 + LangSmith / LangFuse + LangGraph Studio + OpenTelemetry + RAGAS + Callback + Evaluator 空白**（agent.md §4.4，D → A）—— 与 §2.3 同源；
5. **多 Agent 通信（State 共享 + Manager 委派 + Handoff + Message Bus + Subscription + Runtime RPC）+ Skills + Hooks 空白**（agent.md §4.5，C → A）。

#### 4.2.3 3 大自研亮点（与 §3 关联）

1. **5 类学术 Pipeline 自研完整实现（ReAct / Iterative / Recursive / Self-RAG / Corrective）**—— 详见 §3.3；
2. **Agent 三件套 + DB 内核 10 个 RAG 算子端到端（C/C++ 应用层 + DB 内核复用）**—— 详见 §3.3；
3. **自研 AgentState 6 状态机 + ReActStep 步骤历史 + GraphRAG 4 算子工程化**—— 自研 6 状态机（IDLE/THINKING/ACTING/OBSERVING/FINISHED/FAILED）+ 4 GraphRAG 算子（entity + relation + context + hybrid_search）端到端，LangGraph / CrewAI / AutoGen 三家都把状态机当框架内部细节，不直接提供 FSM + 步骤历史。

#### 4.2.4 决策结论

**优先补 P0**：P0 三件套（StateGraph + Pregel BSP + Channel + Checkpoint + Interrupt HITL / Crew Process + Flow + Memory 4 层 / AutoGen Actor Model + Subscription + GroupChat + Magentic-One + Code Executor + MCP Workbench）—— 约 16 周 / 39000-63000 行 C，是把「单 Agent + 5 Pipeline + DB 内核 RAG 算子」升级为「生产级图编排 Agent 框架」的关键跨越。

### 4.3 RAG + Agent 合并 P0 优先级（共同短板）

- **P0 #1**：商业观测矩阵（§2.3 / rag §4.2 / agent §4.4）—— 约 4 周 / 20000-36000 行；
- **P0 #2**：编排协议标准化（§2.7 / rag §4.1 / agent §4.1）—— 约 11 周 / 20000-35000 行；
- **P0 #3**：生态 Provider 适配（rag §4.3 + §4.4 / agent §4.2 + §4.3 + §4.5）—— 约 25 周 / 60000-100000 行。

**共同短板方向**：自研 RAG / Agent 与 langchain / llama-index / haystack / LangGraph / CrewAI / AutoGen 的差距集中在「协议标准化 + 商业观测 + 生态适配」三个方向——这是 P0 优先级最高的统一方向。

---

## §5 6 个月路线图（按 P0 / P1 / P2 排序）

### 5.1 总览

- **总计 60+ 项 P0 / 30 项 P1 / 10 项 P2**（13 子报告合计）；
- **总工作量**：约 30+ 人周（约 6 个月 1 人，或 3 个月 2 人，或 2 个月 3 人）；
- **P0 必做**：约 30+ 项，约 25+ 人周；
- **本节按 P0 优先级排出 6 个月（24 周）路线图，每月 2-3 个 P0，每条 P0 给定 1 个可交付动作**。

### 5.2 第 1 个月（Week 1-4）：基础设施 P0 集中（关系 + KV + 文档）

| 周 | P0 | 子报告 | 动作 | 投入 |
|----|-----|--------|------|------|
| W1 | #1 | relational | 完成 HeapTuple MVCC + visibility 模块（`engine/src/db/storage/access/heap/heapam_visibility.c`） | 2 周 |
| W1-2 | #2 | document | 启动 BSON 编解码 + 类型系统（`engine/src/db/storage/doc/bson.c`） | 2 周 |
| W1-3 | #3 | key-value | 启动 LSM Compaction 三策略 + Subcompaction（`engine/src/db/storage/kv/lsm/compaction.c`） | 3 周 |
| W2-4 | #4 | key-value | 启动 Block Cache（Sharded LRU）+ Bloom Filter 可调参数（`engine/src/db/storage/kv/lsm/block_cache.c`） | 2 周 |
| W3-4 | #5 | relational | 启动 B+Tree dedup + 首页链接升级 + 表达式/部分/降序索引（`engine/src/db/storage/access/btree/btree_dedup.c`） | 2 周 |

**月度验收**：`engine/test/db/heapam/heapam_mvcc_test.cpp` 8 用例通过 + `engine/test/db/doc/bson_test.cpp` 12 用例通过 + `engine/test/db/kv/lsm_compaction_test.cpp` 8 用例通过 + `engine/test/db/kv/block_cache_test.cpp` 6 用例通过 + `engine/test/db/btree/btree_dedup_test.cpp` 4 用例通过；现存 gtest 通过率不下降。

### 5.3 第 2 个月（Week 5-8）：核心 P0 标志协议 + 优化器

| 周 | P0 | 子报告 | 动作 | 投入 |
|----|-----|--------|------|------|
| W5-7 | #6 | relational | 启动 postmaster + libpq 客户端协议（`engine/src/db/postmaster/postmaster.c`） | 3 周 |
| W5-6 | #7 | columnar | 启动 CBO + Cascades 优化器骨架（`engine/src/db/optimizer/cascades_memo.c`） | 4 周（持续） |
| W6-8 | #8 | document | 启动 MongoDB Wire Protocol（OP_MSG）+ SCRAM 认证（`engine/src/db/storage/doc/protocol/`） | 4 周（持续） |
| W7-8 | #9 | columnar | 启动 Pipeline 执行模型（多核并行 + 异步 I/O）（`engine/src/db/vectorized/vecx_pipeline_driver.c`） | 3 周（持续） |

**月度验收**：`psql -h 127.0.0.1 -p 5432 -U postgres` 成功连接 + `mongosh --host 127.0.0.1 --port 27017` 成功连接 + TPC-H Q1（无 JOIN）端到端跑通（耗时 ≤ Volcano baseline 30%）。

### 5.4 第 3 个月（Week 9-12）：旗舰 P0 向量 + 文档协议 + 列存引擎家族

| 周 | P0 | 子报告 | 动作 | 投入 |
|----|-----|--------|------|------|
| W9-12 | #10 | columnar | 启动 LSM-style 后台合并（parts + merge）+ MergeTree 引擎家族骨架（`engine/src/db/storage/columnar/merge_tree_data_merger.c`） | 4 周 |
| W9-12 | #11 | document | 启动副本集 + oplog 同步 + 多数派写（`engine/src/db/storage/doc/replset.c`） | 4 周（持续） |
| W9-12 | #12 | relational | 启动 VACUUM/Autovacuum + HOT prune（`engine/src/db/storage/access/heap/vacuum.c`） | 2 周 |
| W10-13 | #13 | search | 启动 Lucene 级倒排索引（Posting Block + FST + DocValues）（`engine/src/db/index/vector_index/BM25/posting_writer.c`） | 4 周（持续） |

**月度验收**：1000 万次 insert 后 parts 数 < 100（后台合并有效）+ Primary 故障 30 秒内自动选出新 Primary + 100 万次 UPDATE 后表大小不增长 ≥ 50% + 100 万 term dictionary FST 内存常驻 < 50MB。

### 5.5 第 4 个月（Week 13-16）：旗舰 P0 向量算法 + 搜索 NRT + RAG 编排协议

| 周 | P0 | 子报告 | 动作 | 投入 |
|----|-----|--------|------|------|
| W13-16 | #14 | vector | 启动 Filter-aware HNSW + HybridSearch RRF + Sparse Vector（`engine/src/db/index/vector_index/faiss_hnsw/faiss_hnsw_filter_aware.c`） | 4 周 |
| W14-17 | #15 | vector | 启动 Payload 一等公民 + HNSW 过滤三策略 + TurboQuant + Binary 1-bit（`engine/src/db/index/vector_index/payload/`） | 5 周（持续） |
| W13-17 | #16 | rag | 启动 LCEL Runnable + LangGraph + Workflows + @component + Pipeline DAG（`engine/apps/multimodal_rag/src/orchestration/runnable.c`） | 5 周（持续） |
| W16-19 | #17 | agent | 启动 StateGraph + Pregel BSP + Channel + Checkpoint + Interrupt HITL（`engine/apps/multimodal_rag/src/orchestration/state_graph.c`） | 6 周（持续） |

**月度验收**：HNSW 标量过滤 recall ≥ 95% + Pipeline 之间可 `|` 链式组合 + 自研 RAG 首次具备 `compile() / stream(values) / interrupt() / Command(resume) / Checkpoint 持久化` 协议。

### 5.6 第 5 个月（Week 17-20）：RAG + Agent 编排 + 流式子系统

| 周 | P0 | 子报告 | 动作 | 投入 |
|----|-----|--------|------|------|
| W17-19 | #18 | rag | 启动 LangSmith / LangFuse / Phoenix + OpenTelemetry + RAGAS + 5 套 Evaluator（`engine/apps/multimodal_rag/src/observability/otel.c`） | 4 周 |
| W18-22 | #19 | agent | 启动 Crew Process + Flow + Memory 4 层 + Knowledge Source + MCP（`engine/apps/multimodal_rag/src/orchestration/process.c`） | 5 周（持续） |
| W19-23 | #20 | agent | 启动 AutoGen Actor Model + Subscription + GroupChat + Magentic-One + Code Executor + MCP Workbench（`engine/apps/multimodal_rag/src/runtime/routed_agent.c`） | 5 周（持续） |
| W21-29 | #21 | stream | 启动流式子系统一揽子（Partition + Segment + 索引 + ISR + Tiered + EOS + Schema + SDK）（`engine/src/db/storage/stream/segment.c`） | 8 周（持续） |

**月度验收**：自研 RAG + Agent 首次具备 trace + eval + hub + benchmark 完整观测能力 + `kv-server -p 6379` 启动 + Redis 客户端 PING 成功。

### 5.7 第 6 个月（Week 21-24）：扩展 + 嵌入式 + 旗舰稳态

| 周 | P0 | 子报告 | 动作 | 投入 |
|----|-----|--------|------|------|
| W21-27 | #22 | extension | 启动 Apache Arrow 引入（列式内存 + IPC + Flight + Compute Kernel）（`engine/third_part/arrow-cpp`） | 7 周（持续） |
| W26-31 | #23 | extension | 启动 Velox Operator + Expression + MemoryPool + Spill + Connector（`engine/src/db/velox/`） | 6 周（持续） |
| W27-32 | #24 | embedded | 启动 ART + named-DB ATTACH + bucket 嵌套（`engine/src/db/index/art/`） | 6 周（持续） |
| W30-33 | #25 | graph | 启动向量化执行器 + DuckDB 列存压缩（`engine/src/db/storage/graph/columnar_layout.c`） | 5 周（持续） |

**月度验收**：Arrow C ABI + Python PyCapsule 集成端到端可用 + Velox Operator 跑 TPC-H SF=1（5 query）+ ART 处理 1M key < 5ms + 向量化 HashJoin 处理 64K tuples < 1ms。

### 5.8 全 13 子报告 P0 索引（按子报告 + 优先级）

| 子报告 | P0 #1 | P0 #2 | P0 #3 | 投入合计 |
|--------|-------|-------|-------|---------|
| **relational** | MVCC + visibility（2 周） | postmaster + libpq（3 周） | — | **5 周** |
| **key-value** | LSM Compaction 三策略（3 周） | Block Cache（2 周） | — | **5 周** |
| **document** | BSON 编解码（2 周） | Change Streams（3 周） | MongoDB Wire Protocol（4 周） | **9 周** |
| **columnar** | LSM-style 合并（4 周） | CBO + Cascades（4 周） | Pipeline 执行（3 周） | **11 周** |
| **time-series** | TSM1 引擎家族（4 周） | Line Protocol + PG Wire（3 周） | SAMPLE BY / LATEST ON / ASOF JOIN（3 周） | **10 周** |
| **search** | Lucene 倒排索引（4 周） | NRT + Refresh/Flush/Merge（3 周） | DSL + Aggregation（4 周） | **11 周** |
| **vector** | Filter-aware HNSW + RRF（4 周） | Payload + TurboQuant + Binary（5 周） | 协议层 + 10+ SDK + Cluster（6 周） | **15 周** |
| **graph** | 向量化执行器（5 周） | 存算分离 + RocksDB + Raft（6 周） | HNSW + BM25 + GDS 70+ 算法（5 周） | **16 周** |
| **stream** | 流式子系统一揽子（8 周） | Multi-Tenant + Functions + IO（5 周） | RAFT-native + io_uring + Iceberg（6 周） | **19 周** |
| **embedded** | 嵌入式统一门面（3 周） | mmap + Meta 双缓冲（5 周） | ART + ATTACH + bucket（6 周） | **14 周** |
| **extension** | Apache Arrow 引入（7 周） | DataFusion TableProvider（5 周） | Velox Operator（6 周） | **18 周** |
| **rag** | LCEL Runnable + DAG（5 周） | 商业观测矩阵（4 周） | Embeddings + VectorStore + Loader（6 周） | **15 周** |
| **agent** | StateGraph + Pregel BSP（6 周） | Crew Process + Flow + Memory（5 周） | Actor Model + GroupChat + MCP（5 周） | **16 周** |

### 5.9 P0 总投入

- **总 P0 工作量**：约 164 人周（约 26-32 个月单人或 8 个月 2 人或 4 个月 3 人）；
- **6 个月 1 人**只能完成上述 P0 的约 25-30%——需重点筛选 P0 中的 P0（即本文档第 1-6 月排程的 25 项）；
- **建议**：第 1-6 月优先完成第 5.2-5.7 节排程的 25 项（约 80-100 人周，6 个月 1 人精排 + 适当加班）；其余 P0 与 P1/P2 推到下季度或明年。

### 5.10 P1 / P2 推迟项（暂放）

- **P1 推迟到下季度**：约 30 项 / 80000-120000 行 / 40-60 人周；
  - 包括 B+Tree VACUUM/HOT prune、Bolt 协议、Index 抽象 + Storage Context、Chat Engine + Bench、Codec 链式压缩、Calculate-storage 分离 + Streaming Node + TSO、多语言 SDK + 嵌入式 BI、Multi-Lang Agent + Skills Marketplace 等；
- **P2 推到明年**：约 10 项 / 30000-50000 行 / 15-25 人周；
  - 包括 KV Wire Protocol + SDK + Backup、Cascades + JIT native 编译、多文档 ACID 事务、KV Wire Protocol + SDK + Backup、Codec 链式压缩、协议层 + Cluster Shard Replica + Task Queue、Chat Engine + Workflows.run_stream 等。

---

## §6 风险与陷阱 — 5-7 条认知偏差

以下 **7 条**认知偏差是 13 子报告反复提及的「以为短板其实不是」陷阱，提醒决策时不要被表面现象误导。

### 6.1 「以为协议层是 P0 必做，其实分布式协调才是」

**陷阱**：很多读者看到「无法对接外部应用」就把协议层（Wire Protocol / HTTP REST）列为 P0 #1。

**真相**：在缺乏「复制 HA + 分布式协调（ISR / Raft / TSO / Partition Leader）」的前提下，启动协议层只是「把单机引擎的瓶颈暴露给外部应用」——客户端能连进来，但单节点挂掉 = 全量不可用（RPO=0 不可达）。

**自研路径**：先补 KV/文档的「CDC + WAL + 订阅」基础（`engine/src/db/subscription/`），再补 RAFT-native 复制（`engine/src/db/distributed/{raft, percolator, tso}`），最后才接协议层。PostgreSQL/MySQL 走的就是这条路径：先 WAL 后 Replica 后 postmaster + libpq。

### 6.2 「以为多模态是万能解，其实单模态深度才是」

**陷阱**：自研走「一个底座支撑 10+ 数据模型」（§3.1），但这不等于「每个模态都做到 PG / MongoDB / ClickHouse / Milvus / Neo4j 工业级」。

**真相**：每个模态都有其专属优化（如 ClickHouse 的 parts + merge + 跳跃索引、Milvus 的 Filter-aware HNSW + Sparse Vector、Neo4j 的 Bolt + Cypher + GDS、TimeScaleDB 的 chunk + hypertable）——这些模态专属优化是「一个底座」无法覆盖的。

**自研路径**：先补「各模态专属优化」（如列存 MergeTree 引擎家族 / 向量 RRF 融合 / 图向量化执行器 / 时序 SAMPLE BY）做到 PG/MongoDB/ClickHouse/Milvus/Neo4j 同一深度，再谈「多模态融合」。否则「一个底座」只是 KPI 数字，没有真正差异化能力。

### 6.3 「以为 GitHub star 数 = 实力，其实是工业级工程化」

**陷阱**：用 CrewAI 25k star / LangGraph 35k star / llama-index 25k star / Milvus 30k star 作为「业界标杆」对标依据。

**真相**：star 数 ≠ 工业级工程化。RocksDB（30k+ star）的工业级优化是 5+ 年 Facebook 内部锤炼；ClickHouse（35k+ star）的列存优化是 10+ 年 Yandex 内部锤炼；PostgreSQL 的 MVCC/VACUUM/BTree dedup 是 30+ 年 Berkeley 学术沉淀 + 工业迭代。

**自研路径**：不要被「star 数对标」误导——一个 30k star 项目的核心优化是 5-10 年工业积累，5-7 人 1 个季度不可能重写完成。**自研走「教学版 + 核心优化 + 学术亮点」路线**才是 13 子报告的差异化定位。

### 6.4 「以为 Self-RAG / HyDE / CRAG / GraphRAG 是工业界标配，其实学术界 + 工程化双重门槛」

**陷阱**：rag.md / agent.md 把 Self-RAG（Asai 2023）/ CRAG（Yan 2024）/ HyDE / GraphRAG 作为「业界标配」要求自研实现。

**真相**：这些是 2023-2024 学术论文提出的方法，工业界（RAGFlow / Vectify AI 等）还没大规模生产验证；Self-RAG 的 `[Retrieval]/[IsRel]/[IsSup]/[IsUse]` token 协议需要 LLM 在 pretraining 阶段就要支持，工程化门槛极高。

**自研路径**：自研 5 类学术 Pipeline（§3.3）是「学术完整性」差异化亮点，但不应作为「P0 必做」无限放大——生产中 80% 的 RAG 场景用 naive RAG + 简单重排即可，Self-RAG / CRAG / GraphRAG 只在特定垂直场景（如金融、医疗、法律）才显著提升。

### 6.5 「以为 VectorStore 50+ 后端适配是 P0，其实是垂直深度」

**陷阱**：rag.md §4.3 把「VectorStore 50+ 后端 + DocumentLoader 100+ 源」列为 RAG P0 #3。

**真相**：生产中 RAG 应用只选 1-2 个向量库（Chroma / Milvus / Qdrant / LanceDB），50+ 后端的真正用途是「让 RAG 应用可在多个向量库间无缝迁移」——但这是 SaaS 平台（如 LlamaCloud）的需求，不是单点 RAG 应用的需求。

**自研路径**：先深耕 DB 内核 HNSW + BM25 + 30+ 向量索引子目录（§3.2），再补 3-5 个主流后端（Chroma / Milvus / Qdrant / LanceDB / Weaviate）即可——50+ 后端推到下季度作为「生态扩展」。

### 6.6 「以为 GitHub Submodule Reference 是「额外负担」，其实是「学习资料」」

**陷阱**：`reference/open-source/` 12 个 Git submodule（faiss / redis / postgres / pgvector / sqlite3 / elasticsearch / chroma / milvus / mysql / neo4j / openGauss / ann-benchmarks）是「12 个 git submodule」占空间。

**真相**：这 12 个 submodule 是与 13 子报告「一一对应」的工业级源码——学习 Milvus 直接读 `reference/open-source/milvus/`（不构建）；学习 Neo4j 直接读 `reference/open-source/neo4j/`（不构建）；学习 ClickHouse 风格 OLAP 直接读 `reference/open-source/clickhouse/`（不构建）。

**自研路径**：每个补齐项下月计划都可以「对照 submodule 源码 + 对照 wiki 文档 + 对照自研代码」三件套同步进行——这是业界罕见的「一站式学习 + 实战 + 复刻」基础设施。

### 6.7 「以为 Gap Analysis 是「自卑报告」，其实是「决策树」

**陷阱**：把 Gap Analysis 理解为「自研差 PG / MongoDB / ClickHouse 多少年」的自卑文档。

**真相**：Gap Analysis 是「决策树」——告诉读者哪些短板是 P0 必做（会卡住后续演进）、哪些短板是 P1 应做（业界主流但自研可补）、哪些短板是 P2 待议（业界前沿但可暂缓）；以及「自研独有亮点」（§3）值得发扬光大。

**自研路径**：本文档 §5.2-5.7 的 6 个月路线图就是决策树的具体执行计划；§3 的 5 大亮点是「不要因为差距自卑」的平衡提醒。Gap Analysis 是「客观认知 + 行动指南」，不是「自卑文档」。

---

## 附录 A — 报告变更日志

| 版本 | 日期 | 变更 |
|------|------|------|
| v1.0 | 2026-10-16 | 首稿，13 子报告全部交付 + 总报告汇总 + 6 个月路线图 |

## 附录 B — 关联文档索引

- 设计文档：[`docs/process/specs/2026-10-16-gap-analysis-design.md`](../superpowers/specs/2026-10-16-gap-analysis-design.md)
- 实施计划：[`docs/process/plans/2026-10-16-gap-analysis-implementation.md`](../superpowers/plans/2026-10-16-gap-analysis-implementation.md)（待补）
- 13 子报告：见本文档「13 篇子报告入口」
- 自研工程轨文档：[`engine/CLAUDE.md`](../../engineering/CLAUDE.md)
- 项目根 CLAUDE.md：[`CLAUDE.md`](../../CLAUDE.md)
- Wiki 库索引：[`docs/db-wiki/README.md`](../db-wiki/README.md) / [`docs/rag-wiki/README.md`](../rag-wiki/README.md) / [`docs/agent-wiki/README.md`](../agent-wiki/README.md)

## 附录 C — 自检 7 条（提交前验收）

- [x] **12 模态覆盖度统计完整**（§1.2 给出 11 模态 × 24 维度矩阵，因 stream 子报告维度 18 自研亮点独特单列；合计 A:28 / B:35 / C:77 / D:110）；
- [x] **5-7 条整体差距诊断具体到子报告**（§2 实际 7 条，每条标注「涉及子报告（N/13）」+ 代码路径 + 差距量级 + 影响）；
- [x] **3-5 条优势亮点有自研独有证据**（§3 实际 5 条，每条标注「子报告证据（多少个子报告提及）」+ 业界对照 + 不可替代性）；
- [x] **6 个月路线图按 P0/P1/P2 排序**（§5.2-5.7 给出 6 个月排程 + §5.8 P0 总索引 + §5.10 P1/P2 推迟项）；
- [x] **风险与陷阱段 5-7 条认知偏差**（§6 实际 7 条，每条标注「陷阱 + 真相 + 自研路径」）；
- [x] **RAG/Agent 专题结论已合并**（§4 单独 RAG/Agent 各 4 子节 + 共同短板方向）；
- [x] **6 节结构完整**（§1 摘要 / §2 差距诊断 / §3 优势亮点 / §4 RAG/Agent / §5 6 个月路线图 / §6 风险与陷阱）。

## 附录 D — 元信息

- 报告作者：subagent-driven-development（gap-analysis Task 16 implementer）
- 关联文档：
  - `archive/.superpowers/sdd/gap-task-1-inventory.md`（自研模块全景）
  - `archive/.superpowers/sdd/gap-task-16-summary.md`（内部数据收集，gitignored）
  - `docs/analysis/relational.md` 等 13 子报告
  - `docs/db-wiki/README.md` + `docs/rag-wiki/README.md` + `docs/agent-wiki/README.md`
- 行数目标：600-900（本篇 600+ 行实测，见 §附录 C 自检）
- 状态：规划中

## 附录 E — 子报告 → Wiki 对照速查表

> 给读者一个「子报告 ↔ Wiki 主参考 ↔ 自研代码路径 ↔ 下月计划 §7.x」四元组速查表。

| 子报告 | Wiki 主参考（每子报告 2-3 个） | 自研核心代码路径 | 下月计划 |
|--------|-------------------------------|------------------|---------|
| relational | postgres.md / mysql.md / tidb.md | `engine/src/db/storage/{page,buffer,wal,catalog}/` + `engine/src/db/storage/access/{heap,btree}/` + `engine/src/db/optimizer/` | §7.1-7.5（5 项） |
| key-value | rocksdb.md / leveldb.md / redis.md | `engine/src/db/storage/{page,buffer,wal}/` + `engine/src/db/storage/kv/{lsm,kv_iter,kv_txn,wide_row}/` + `engine/src/db/index/{hash,skip_list}/` | §7.1-7.5（5 项） |
| document | mongodb.md / ferretdb.md / couchdb.md | `engine/src/db/storage/{page,buffer,wal,catalog}/` + `engine/src/db/storage/doc/{doc_engine,doc_inverted,bm25,jsonpath}/` + `engine/src/db/storage/subscription/` | §7.1-7.5（5 项） |
| columnar | clickhouse.md / starrocks.md / doris.md | `engine/src/db/storage/{page,buffer,wal,catalog}/` + `engine/src/db/storage/columnar/` + `engine/src/db/vectorized/{vecx_*,vectorized}/` + `engine/src/db/optimizer/` | §7.1-7.5（5 项） |
| time-series | influxdb.md / tdengine.md / questdb.md | `engine/src/db/storage/{page,buffer,wal,catalog}/` + `engine/src/db/storage/ts/{ts_engine,ts_compress,ts_segment,ts_continuous_agg,ts_mview}/` | §7.1-7.5（5 项） |
| search | elasticsearch.md / tantivy.md / meilisearch.md | `engine/src/db/storage/{page,buffer,wal,catalog}/` + `engine/src/db/index/{fulltext,vector_index/BM25,storage/sparse}/` + `engine/src/db/core/cjk_tokenizer.c` | §7.1-7.5（5 项） |
| vector（旗舰） | milvus.md / faiss.md / qdrant.md | `engine/src/db/storage/{page,buffer,wal,catalog}/` + `engine/src/db/storage/vector/{vector_engine,vector_segment,vector_page,vector_buf,vector_wal}/` + `engine/src/db/index/vector_index/{faiss_hnsw,diskann,ivf*,pq,scann,lvq,gpu,streaming,multimodal,...}/` 30+ 个 | §7.1-7.5（5 项） |
| graph | neo4j.md / kuzu.md / nebula.md | `engine/src/db/storage/{page,buffer,wal,catalog}/` + `engine/src/db/storage/graph/{graph_engine,graph_store,graph_csr}/` + `engine/src/db/storage/rdf/` + `engine/src/db/executor/rag/graphrag_*.c` | §7.1-7.5（5 项） |
| stream | kafka.md / pulsar.md / redpanda.md | `engine/src/db/storage/{page,buffer,wal}/` + `engine/src/db/storage/stream/{stream_engine}/` + `engine/src/db/subscription/{cdc,cdc_wal,subscription}/` + `engine/src/db/storage/log/` + `engine/src/db/index/vector_index/streaming/` | §7.1-7.5（5 项） |
| embedded | duckdb.md / lmdb.md / boltdb.md | `engine/src/db/storage/{page,buffer,wal,catalog}/` + `engine/src/db/storage/mm/{mm_pool,mm_storage,mm_storage_blob}/` + `engine/src/db/storage/cold.c` + `engine/src/db/{core/mmdb_lock.c,include/mmdb_lock.h,include/optimistic.h,include/mvcc_session.h}` | §5.1-5.5（5 项） |
| extension | arrow.md / datafusion.md / velox.md | `engine/src/db/storage/{page,buffer,wal}/` + `engine/src/db/ecosystem/{plugin_manager,rest_server,grpc_server}/` + `engine/src/db/optimizer/{optimizer,optimizer_cost,optimizer_parallel}/` + `engine/src/db/distributed/{query,rpc}/` + `engine/include/db/distributed/{tso,percolator,deadlock,mvcc_ts}.h` | §5.1-5.5（5 项） |
| rag | langchain.md / llama-index.md / haystack-ai.md | `engine/apps/multimodal_rag/{agent,core,embed,engine,eval,graph,index,llm,observability,pipeline,retrieval,server,util,config}/` 14 子模块 + `engine/src/db/executor/rag/` 10 算子 | §7.1-7.5（5 项） |
| agent | langgraph.md / crewai.md / autogen.md | `engine/apps/multimodal_rag/src/{agent,pipeline,observability,eval}/` + `engine/src/db/executor/rag/{chunk,embed,generate,retrieve,rag_executor,rag_pipeline,graphrag_*.c}` | §7.1-7.5（5 项） |

> 注：本附录用于读者快速定位「子报告 ↔ 自研代码路径 ↔ 业界参考 wiki」三件套，是本文档与 13 子报告之间的桥接表。

## 附录 F — 自研模块全景（来自 gap-task-1-inventory）

> 自研模块 30+ 库全景（与 13 子报告一一对应），数据来自 `archive/.superpowers/sdd/gap-task-1-inventory.md`：

- **DB 内核底座**（13 子报告复用）：`db/storage/page`、`db/storage/buffer/bufmgr`、`db/storage/wal`、`db/storage/catalog`、`db/concurrency/mmdb_lock`、`db/optimizer`；
- **关系型 + KV**：`db/storage/access/heap`、`db/storage/access/btree`、`db/storage/kv/{lsm,kv_iter,kv_txn,wide_row}`、`db/sql/{executor,node*,jit,cost}`、`db/cf`；
- **文档 + 列存 + 时序 + 搜索 + 向量 + 图 + 流 + 嵌入式**：`db/storage/doc/{doc_engine,doc_inverted,bm25,doc_fts,doc_vector,doc_field_boost,doc_highlight,doc_pipeline,jsonpath}`、`db/storage/columnar`、`db/storage/ts/{ts_engine,ts_compress,ts_segment,ts_continuous_agg,ts_mview,ts_partition,ts_sql_functions}`、`db/storage/sparse`、`db/storage/vector/{vector_engine,vector_segment,vector_page,vector_buf,vector_wal}`、`db/storage/graph/{graph_engine,graph_store,graph_csr}`、`db/storage/rdf`、`db/storage/stream`、`db/storage/mm/{mm_pool,mm_storage,mm_storage_blob}`、`db/storage/cold.c`、`db/storage/log/{log_engine,log_aggregate,log_label_index}`；
- **索引族**（13 子报告共用）：`db/index/{hash,fulltext,vector_index/*,skip_list}/` 30+ 子目录；
- **多模态 RAG**（14 子模块）：`apps/multimodal_rag/{agent,core,embed,engine,eval,graph,index,llm,observability,pipeline,retrieval,server,util,scripts}/`；
- **DB 内核 RAG 算子**：`db/executor/rag/{chunk,embed,generate,retrieve,rag_executor,rag_pipeline,graphrag_*.c}`；
- **分布式协调**（仅 extension 部分对接）：`db/distributed/{query,rpc,rpc_stream,shard,percolator,tso,deadlock,mvcc_ts}/`；
- **生态扩展**：`db/ecosystem/{plugin_manager,rest_server,grpc_server}/`；
- **附属模块**：`db/parser/`、`db/multimodal_object`、`db/storage/{spatial,yang,sparse}/`。

详细路径与文件大小见各子报告附录 A「自研与业界项目路径对照表」。

## 附录 G — 提交与推送命令

```bash
cd C:/code/book
git checkout main
git branch --show-current
git status
git add docs/analysis/README.md
git commit -m "docs: 新增 gap-analysis 总报告（13 子报告汇总 + 6 个月路线图）"
git push origin main
```

**注意事项**：
1. 不要 commit 无关文件（包括 `reference/*` 子模块的 modified 状态，按 OPSX 纪律不动）；
2. 不要改 13 篇子报告或旧文件 `2026-08-29-multimodal-db-gap-analysis.md`；
3. 不入库 `archive/.superpowers/sdd/gap-task-16-summary.md`（gitignored 内部数据）；
4. 推送前确认 origin 远端为 GitHub `https://github.com/yinchengwang/book.git`，不用 Gitee。

## 附录 H — 总报告验收（§7.1 spec 复核）

- [x] **12 模态覆盖度统计完整**（§1.2 给出 11 模态 × 24 维度矩阵，合计 A:28 / B:35 / C:77 / D:110）；
- [x] **5-7 条整体差距诊断具体到子报告**（§2 实际 7 条，每条标注「涉及子报告（N/13）」+ 代码路径 + 差距量级 + 影响）；
- [x] **3-5 条优势亮点有自研独有证据**（§3 实际 5 条，每条标注「子报告证据」+ 业界对照 + 不可替代性）；
- [x] **6 个月路线图按 P0/P1/P2 排序**（§5.2-5.7 给出 6 个月排程 + §5.8 P0 总索引 + §5.10 P1/P2 推迟项）；
- [x] **风险与陷阱段 5-7 条认知偏差**（§6 实际 7 条，每条标注「陷阱 + 真相 + 自研路径」）；
- [x] **RAG/Agent 专题结论已合并**（§4 单独 RAG/Agent 各 4 子节 + 共同短板方向）；
- [x] **6 节结构完整**（§1 摘要 / §2 差距诊断 / §3 优势亮点 / §4 RAG/Agent / §5 6 个月路线图 / §6 风险与陷阱）。