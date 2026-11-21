> ⚠️ **归档说明**：本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考。
> 当前主核是 `engine/db/`（自带 Makefile，不在 CMake 图内），见 [设计文档](../../superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md)。

# 未注册引擎核对（9 个）

> 审查日期：2026-08-27 ｜ 审查方式：静态代码审查（轻量核对）
> 范围：`engine/src/db/` 下存在但未注册进 `DataModel` 枚举（`include/db/storage_engine.h`）的 9 个引擎

## 1. 整体盘点

| # | 引擎 | 代码位置 | 行数 | 状态 |
|---|------|---------|------|------|
| 1 | RDF/SPARQL | `storage/rdf/` | rdf_engine.c + rdf_index.c + sparql_parser.c（6 文件） | 完成度 7 |
| 2 | 稀疏向量+BM25 | `storage/sparse/` | sparse_vector.c + bm25_index.c + hybrid_retrieval.c（4 文件） | 完成度 7 |
| 3 | 流引擎 | `storage/stream/stream_engine.c` | ~300 | 完成度 5 |
| 4 | 列存引擎 | `storage/columnar/columnar_engine.c` | ~700 | 完成度 6 |
| 5 | Column Family | `src/db/cf/` | cf_engine.c + cf_row.c + cf_column.c | 完成度 8 |
| 6 | 时空引擎（st） | `storage/st/st_engine.c` | - | 完成度 4 |
| 7 | 物化视图（mmview） | `storage/mmview/mview.c` | - | 完成度 4 |
| 8 | 账本（ledger） | `src/db/ledger/ledger.c` | - | 完成度 3 |
| 9 | CDC 订阅 | `src/db/subscription/` | cdc.c + cdc_wal.c | 完成度 5 |

## 2. 各引擎核对

### 2.1 RDF/SPARQL（MODEL_RDF 候选）

**定位**：语义三元组存储与 SPARQL 1.1 查询——知识图谱、GraphRAG、与 Graph 模态的语义层。

**完成度**：parser（sparql_parser.c）+ 索引（rdf_index.c）+ 引擎（rdf_engine.c）三件齐全；与同 Graph 模态 CSR 存储互补（CSR 用于属性图遍历，RDF 用于三元组）。

**业界差距**：
- Jena / Virtuoso / Blazegraph / Stardog：均支持完整 SPARQL 1.1（含 UPDATE/FEDERATED/Property Path），部分支持 OWL 推理
- 与 Graph 模态功能重叠（属性图 ≈ RDF + 命名图）；建议打通两层共享节点存储

**注册建议**：建议注册 `MODEL_RDF = 10`，与 Graph 共享存储后端

### 2.2 稀疏向量 + BM25 混合检索

**定位**：稀疏 embedding（SPLADE/BGE-M3）+ BM25 词项权重——Hybrid Search（密集+稀疏混合检索），Milvus 2.4+ 标配。

**完成度**：hybrid_retrieval.c 提供 RRF/DBSF 等混合策略；sparse_vector.c 处理稀疏向量编码。

**业界差距**：Milvus Hybrid Search、Pinecone sparse-dense、Qdrant Sparse Vector——实现密度已接近业界

**注册建议**：建议注册 `MODEL_SPARSE = 11`，与 Vector 同源

### 2.3 流引擎（stream_engine）

**定位**：流式数据管理（窗口聚合、事件序列）；近似 Kafka Streams / Flink SQL。

**完成度**：单文件 ~300 行，API 表面窄；窗口与持久化深度未核全文

**业界差距**：Kafka Streams / Flink / Materialize：成熟流处理提供 Exactly-Once 语义、watermark、状态后端（RocksDB）、CDC 源/汇。自研极薄

**注册建议**：建议作为 `MODEL_STREAM = 8`（枚举已存在）下的二级服务或子系统，不强行独立注册——避免与 CDC（已存在）和已注册的 STREAM 重复定义

### 2.4 列存引擎（columnar_engine）

**定位**：列式存储——OLAP 查询、向量检索 Scan 优化、压缩带宽优势

**完成度**：~700 行 columnar_engine.c + internal.h；具体算子（projection/filter/agg pushdown）覆盖度未核

**业界差距**：DuckDB 列式（向量化执行 + Pushdown）、ClickHouse MergeTree、Parquet + DuckDB。自研规模小，能力面有限

**注册建议**：建议作为底层存储格式（与 mm_storage 接口并行），不独立 `MODEL_COLUMNAR = 9`（枚举已存在）枚举值，由 vector/relational/time-series 共用

### 2.5 Column Family（cf）

**定位**：KV 多命名空间隔离——RocksDB Column Family 等价物

**完成度**：cf_engine.c + cf_row.c + cf_column.c 三层；P3-4 commit bb2c77fbb 已完成

**业界差距**：RocksDB CF（最成熟）/ Cassandra keyspace / FoundationDB Directory；自研规模小但 API 形态对齐

**注册建议**：作为 KV 模态扩展，**不独立注册枚举**——通过 `kv_open_cf(db, name)` 子 API 暴露

### 2.6 时空引擎（st）

**定位**：**Spatio-Temporal**——轨迹查询、时空范围、移动对象（st_object_t 含 position/speed/heading）

**完成度**：复用 R-Tree（`#include "db/storage/spatial/rtree.h"`）；st_object_t 结构（id + position + speed + heading）完整；轨迹/移动对象基础齐

**业界差距**：PostGIS + TimescaleDB / Azure Time Series Insights / 移动对象数据库（MOD）；自研单文件覆盖有限

**注册建议**：建议 `MODEL_SPATIOTEMPORAL = 11`，与 Spatial 模态互补——`MODEL_SPATIAL` 静态空间，`MODEL_SPATIOTEMPORAL` 动态对象

### 2.7 物化视图（mmview）

**定位**：关系模态的物化视图——pre-computed JOIN/AGG 结果集

**完成度**：mview.c（`storage/mmview/mview.c`），独立于 `sql/materialized_view.c`（918 行）——可能是底层增量维护模块

**业界差距**：PostgreSQL MView（已实现 REFRESH CONCURRENTLY/FULL/ON COMMIT）、ClickHouse 物化视图（增量更新）；自研独立模块状态待核

**注册建议**：作为 Relational 模态子系统，不独立注册

### 2.8 账本（ledger）

**定位**：不可变审计日志（append-only）——区块链风格的账本数据

**完成度**：ledger.c（`src/db/ledger/`）单文件；append-only 语义实现完整度待核

**业界差距**：Hyperledger Fabric（许可链）/ QLDB（中心化不可变账本）/ S3 Object Lock；通用领域用 PostgreSQL `INSERT-only` 表 + 触发器即足够

**注册建议**：不建议独立注册——可作为 KV/Relational 之上的语义层，通过 WAL 重定向实现

### 2.9 CDC 订阅

**定位**：Change Data Capture——订阅数据库变更事件（INSERT/UPDATE/DELETE）

**完成度**：cdc.c + cdc_wal.c 双文件；与 WAL 紧密关联（`cdc_wal.c`），订阅流基于共享 WAL（KV 已接入）

**业界差距**：Debezium（最成熟，基于 binlog/WAL 包装）/ Maxwell / Postgres logical replication / MySQL binlog；自研仅 ~2 文件

**注册建议**：建议作为 SQL 协议的扩展层（pgwire + CDC 流），**不独立注册枚举**

## 3. 汇总

| 引擎 | 定位 | 完成度(0-10) | 注册建议 |
|------|------|--------------|---------|
| RDF/SPARQL | 语义图谱 | 7 | **MODEL_RDF = 10**，与 Graph 共享存储 |
| 稀疏向量+BM25 | 混合检索 | 7 | **MODEL_SPARSE = 11**，与 Vector 同源 |
| 流引擎 | 流处理 | 5 | 复用 MODEL_STREAM（已存在） |
| 列存引擎 | 列式 OLAP | 6 | 不独立注册，作为存储格式 |
| Column Family | KV 多命名空间 | 8 | 不独立注册，KV 扩展 |
| 时空引擎 | 移动对象 | 4 | **MODEL_SPATIOTEMPORAL = 12** |
| 物化视图 | 预计算 | 4 | 不独立注册，Relational 子系统 |
| 账本 | 不可变审计 | 3 | 不注册，语义层 |
| CDC | 变更捕获 | 5 | 不独立注册，WAL 扩展 |

## 4. 关键观察

1. **9 个未注册引擎中至少 3 个（RDF/稀疏/st）建议独立注册进 DataModel 枚举**，其余 6 个属于底层模块不应独占枚举值
2. **完成度两极分化**：CF（8）、RDF/稀疏（7）已接近可用；st/ledger/mview（3-4）功能面窄需大幅投入
3. **与已注册模态功能重叠**：Stream、Columnar、mmview、CDC 都与已有枚举的"实现细节"绑定而非独立数据模型——这是健康的架构选择
4. **未注册的独立模态实质只有 3 个**：RDF/稀疏/st——其中 st 是 Spatial 的动态扩展，RDF 是 Graph 的语义扩展，稀疏是 Vector 的检索扩展
