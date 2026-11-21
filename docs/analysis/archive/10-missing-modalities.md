> ⚠️ **归档说明**：本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考。
> 当前主核是 `engine/db/`（自带 Makefile，不在 CMake 图内），见 [设计文档](../../superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md)。

# 缺失模态调研（5 类）

> 审查日期：2026-08-27 ｜ 审查方式：公开资料 + `reference/` 子模块源码佐证 + 现有 `docs/multimodal-db-comparison-2026.md` 引用
> 范围：自研多模态数据库目前**完全没有**的 5 类数据模型方向，作为扩展建议

## 1. 全文搜索引擎（Dedicated Search Engine）

### 1.1 业界格局

| 产品 | 定位 | 核心能力 | 规模指标 |
|------|------|---------|---------|
| **Elasticsearch**（Elastic NV） | 分布式搜索 + 可观测 | 倒排 + BM25 + 向量（knn）+ RRF 混合 + ES|QL | 集群 PB 级、百万级文档 QPS |
| **Apache Lucene**（基础库） | 单机嵌入式搜索 | 倒排、列存、Block K-means 向量 | - |
| **OpenSearch**（AWS 分支） | ES 兼容 + 安全增强 | 同 ES + OpenSearch Dashboards | 同 ES |
| **Meilisearch** | 轻量搜索 | 错别字容忍、拼音、中文分词 | 单机 ~10K QPS |
| **Typesense** | 即时搜索 | 拼写纠正、faceting | 单机 ~5K QPS |
| **Quickwit** | 日志/对象存储 + 搜索 | S3 兼容 + Tantivy 引擎 | 对象存储级 |
| **Tantivy**（Rust 库） | Lucene 替代 | BM25 + faceting | 同量级 |

来源：`reference/search/elasticsearch` 可读源码（仓库已有 submodule）；公开 benchmark 与产品文档。

### 1.2 典型场景

- 应用内全文搜索（电商商品、文章、文档检索）需要 sub-100ms 响应
- 多语言检索（中文 IK、英文 Snowball、日文 Kuromoji 等）
- 实时增量索引（写入立即可搜）
- 复杂打分（BM25 + 字段加权 + function score + learning-to-rank）

### 1.3 与自研架构契合度

**重叠**：Document 模态已实现 BM25（`storage/doc/bm25.c`）+ 倒排索引（`doc_inverted.c`）+ standard/whitespace/keyword tokenizer（`doc_fts.c`）+ 同义词（DocSynonyms, `:97-175`）——基础工具齐全

**差异**：完整搜索引擎需要
- **分析器生态**：IK/Jieba/MMSeg（中文）、Snowball/Kuromoji（外文）——自研缺
- **打分机制可扩展**：BM25 + function score + 自定义脚本（Lucene `Similarity`/`Script`）
- **分布与一致性**：集群分片、副本、自动重平衡（ES 一等公民）
- **聚合与高亮**：高亮（Highlighter）、nested/parent-child 聚合
- **可观测性**：慢查询日志、JVM 监控

**接入点**：Document 模态（`doc_engine.c` 954 行）——在其上增加
1. 分析器插件（`DocTokenizer` 接口已有）
2. 分布式 Coordinator + Shard（新增 worker pool）
3. `mlockall`/内存映射优化

### 1.4 补入建议

- **建议：集成 Apache Lucene（或 Tantivy）作为 Document 模态的底层引擎**，而非自研。原因：
  - Lucene 20+ 年优化，分词器生态成熟
  - 已有倒排/BM25 代码可作为轻量查询路径，Lucene 作为完整搜索引擎
  - Java 进程集成需 JNI 或 HTTP gateway（Tantivy 是 Rust，可 FFI）
- 工作量：S（PoC）+ M（生产化）
- ROI：高——应用层全文搜索是常见需求，缺它意味着用户必须独立部署 ES

## 2. 宽表 / Wide-Column（Wide-Column Store）

### 2.1 业界格局

| 产品 | 定位 | 核心能力 | 规模指标 |
|------|------|---------|---------|
| **Apache Cassandra** | 分布式最终一致 | LSM-tree + 时间窗口 GC + 多 DC | 10K+ 节点、PB 级 |
| **Apache HBase** | Hadoop 生态强一致 | HDFS + WAL + 协处理器 | 千亿行级 |
| **ScyllaDB**（C++ 重写） | Cassandra 兼容 + 性能 | Seastar async + shard-per-core | 百万 IOPS/节点 |
| **Google Bigtable** | 云原生托管 | SSTable + Chubby 协调 | PB 级自动分片 |
| **TiKV**（Rust） | 分布式 KV + 事务 | Raft + RocksDB | TiDB 底层 |

### 2.2 典型场景

- 时序 + 标签 + 高基数：IoT 设备元数据（每设备数百万属性）
- 写多读少 + 时间窗口：消息元数据、日志索引
- 多维稀疏：每行 schema 不同（user profile、feature store）

### 2.3 与自研架构契合度

**已有部分**：
- KV 模态的页式存储（kv.c 732 行 + buffer pool）
- Column Family（P3-4 完成）

**缺的部分**：
- LSM-tree（自研是页式 B-Tree-like，LSM 写放大/空间放大权衡不同）
- MemTable + SSTable 分层
- 时间窗口 compaction（TWCS）
- 范围分区（partition key + clustering key）
- 分布式 gossip + consistent hashing

**架构差异**：宽表核心是"按 key 排序存储多版本 cell"——与 KV 的"key → 单 value"模型有 1:N 扩展关系。KV + CF 基础上引入"row key → (column1=v1, column2=v2, ...)"是自然的语义扩展。

### 2.4 补入建议

- **建议：分阶段**
  1. **第一阶段（短期）**：在 KV + CF 之上构建 row-column 抽象（cql 兼容层）——工作量 M
  2. **第二阶段（中期）**：引入 LSM-tree 存储引擎（参考 RocksDB 实现；RocksDB 已 vendor 经验）——工作量 L
  3. **第三阶段（远期）**：分布式分片 + 最终一致性协议——工作量 XL
- ROI：中——Kafka/Pulsar 元数据存储场景、Feature Store 场景真实需求；不是所有团队都需要
- 替代方案：直接集成 RocksDB 或 TiKV 作为宽表存储后端（写放大/compaction 优化免费获得）

## 3. 对象 / Blob 存储

### 3.1 业界格局

| 产品 | 定位 | 核心能力 | 规模指标 |
|------|------|---------|---------|
| **AWS S3** | 云对象存储标杆 | 11 个 9 持久性 + 版本 + lifecycle | EB 级、亿级 QPS |
| **MinIO** | 自托管 S3 兼容 | Erasure coding + 单命名空间 | PB 级单集群 |
| **Ceph RGW** | 分布式统一存储 | CRUSH 算法 + 多协议网关 | EB 级 |
| **Azure Blob** | 云对象 | 冷热分层 + 不可变存储 | EB 级 |
| **JuiceFS** | POSIX + 对象后端 | 元数据 RocksDB + 数据对象 | PB 级 |

### 3.2 典型场景

- 大文件（图像/视频/模型权重/PDF/日志归档）
- 跨可用区复制
- 内容寻址（hash-as-key）
- 与数据库集成（BLOB 列外存到对象存储）

### 3.3 与自研架构契合度

**完全空缺**：KV Value 上限 1MB、Vector/Relational/Document 页面 8-16KB——**没有任何引擎支持大对象**

**缺的部分**：
- Range GET（部分下载）
- Multipart upload（大文件分片）
- Erasure coding（vs 副本）
- 强一致性 / 读写一致性模型
- 数据局部性优化（placement group）

**架构差异**：对象存储的关键不是"存"，而是"分布式协议 + 数据保护 + 访问效率"。本质是分布式系统问题而非存储引擎问题。

### 3.4 补入建议

- **建议：不自研**——集成 MinIO（Apache 2.0，Go）或对接 S3 协议
- 在 mm_storage 之上提供 `BLOB` 类型外键引用，存储真实数据到外部对象存储
- 工作量：S（接口）+ M（缓存层）
- ROI：极高——图像/音视频/模型权重场景刚需；但**不应从零实现对象存储**（投入产出比极差）

## 4. 可观测 / 日志分析（Observability / Log Analytics）

### 4.1 业界格局

| 产品 | 定位 | 核心能力 | 规模指标 |
|------|------|---------|---------|
| **Grafana Loki** | 日志聚合（标签 + 流） | LogQL + Cortex 风格分片 | PB 级日志/天 |
| **Elasticsearch** | 日志 + 指标 + Trace | ILM + 数据流 + APM | 同搜索 |
| **ClickHouse** | 列式 OLAP | 极致压缩 + 向量化 | PB 级，单节点秒级聚合 |
| **VictoriaLogs** | 单二进制替代 Loki/ES | 全文索引 + 列式 | 同 Loki |
| **Apache Doris** | 实时 OLAP | MPP + 倒排索引 | PB 级，秒级响应 |
| **Quickwit** | 对象 + 全文索引 | S3 + Tantivy | 同 Loki |
| **SigNoz** | OpenTelemetry 后端 | Trace + Metric + Log | 小到中规模 |

### 4.2 典型场景

- 应用日志聚合（按 service/env/level 标签切片）
- 安全审计日志（合规保留 ≥ 1 年）
- 业务指标（QPS、延迟、错误率）
- Trace/Meter 关联
- 全文检索 + 复杂聚合（如 "error logs in last 5min from service X mentioning database"）

### 4.3 与自研架构契合度

**重叠**：Document 模态的 BM25、Vector 模态的语义检索、Timeseries 的指标存储、Relational 的聚合查询

**缺的部分**：
- **标签索引（tag-based indexing）**：高基数标签快速过滤（Loki/cortex 核心）
- **流式摄取**：Kafka/Pulsar/OTLP 直接消费（无 ETL）
- **压缩后的列式 OLAP**：ClickHouse 级别压缩比 + 聚合性能
- **日志专属查询语言**（LogQL、Loki）

**架构差异**：可观测性核心是"高写入吞吐 + 低成本长期保留 + 快速按标签切片"——这不是任何现有模态的强项（Timeseries 偏窄，Document 偏检索）。

### 4.4 补入建议

- **建议：在 ClickHouse 列式存储 + Timeseries 引擎组合上构建 LogQL 子集**
  - 短期：复用 ClickHouse 社区列式引擎（Apache 2.0，集成而非自研）
  - 中期：实现 Loki 风格的标签索引 + 流式摄取
  - 长期：OpenTelemetry Trace 标准支持
- 工作量：M（LogQL 子集）+ L（Trace/Meter）
- ROI：高——可观测性是任何生产系统的刚需
- 替代方案：直接部署 Loki/ClickHouse + 联邦查询——观察/告警查询分层

## 5. 多模态 AI 原生存储（Multimodal AI-Native Storage）

### 5.1 业界格局

| 产品 | 定位 | 核心能力 |
|------|------|---------|
| **LanceDB**（Rust） | 向量 + 多模态列存 | Lance 列式 + embedding 原生 + 二级索引 |
| **Chroma** | 嵌入式 AI 数据库 | 集合 + embedding + metadata filter |
| **Qdrant**（已实现稀疏 + 命名向量） | 命名向量多模 | 每个点可有多个 named vector |
| **Milvus 2.4+** | 多向量混合 | Hybrid Sparse + Dense + RRF |
| **Weaviate** | 多模态知识图谱 | 模块化向量 + RAG pipeline |
| **Marqo**（Elasticsearch 基础） | 端到端多模 | 预训练模型自动 embedding |
| **Vespa** | 多模态搜索引擎 | 张量字段 + 表达式评分 |

### 5.2 典型场景

- 图文检索：图像 embedding + 文本 metadata + 标签 filter
- 视频理解：关键帧 embedding + 转录文本 + 时间戳
- 跨模态检索（text-to-image, image-to-image）
- RAG 流水线（多模态向量 + metadata filter + reranker）
- 多 embedding 模型并存（CLIP + BLIP-2 + SigLIP）

### 5.3 与自研架构契合度

**已有部分**：
- Vector 模态（21+ 索引）+ Hybrid Retrieval（`storage/sparse/hybrid_retrieval.c`）
- RAG 系统（`engine/rag/`，含 graphrag_*.c、rag_pipeline.c、generate_op.c 等）
- BM25 + embedding hybrid（sparse + dense）

**缺的部分**：
- **多向量 per 对象**（named vector）：一个对象多个 embedding（不同模型/不同模态）
- **原生 blob + embedding 绑定**：图像/视频直接存数据库，metadata 关联
- **跨模态检索算子**：text-to-image 距离、image-text 对比学习
- **GPU 加速的多模态编码**：embedding 生成 pipeline
- **RAG 编排深度**：与 LLM/agent 编排（LangChain/LlamaIndex）

**架构差异**：当前 Vector 模态是"单向量单 ID"模型，多模态原生需要"对象可携带多向量 + blob + 元数据 + 跨向量查询"。这是 schema 级别的扩展。

### 5.4 补入建议

- **建议：在 Vector + Hybrid Retrieval 之上构建"多模态对象"抽象**
  - 短期：实现 NamedVector schema（一对象多 embedding）——工作量 M
  - 中期：对象内嵌 blob 字段（外存到 S3/MinIO，见 §3）——工作量 M
  - 中期：跨模态混合检索算子（CLIP-style image-text）——工作量 L
  - 长期：GPU 编码 pipeline（与 P2-1 GPU 加速呼应）——工作量 L
- ROI：**极高**——LLM/RAG 是当下最热需求，自研已有 RAG 基础但缺原生多模态
- 直接效益：现有 RAG 系统（`engine/rag/`）可从"text-only RAG"升级到"多模态 RAG"，与已规划的"多态 RAG 架构文档"（commit efcd239f8）形成闭环

## 6. 5 个模态调研汇总

| 模态 | 业界代表 | 契合度(0-10) | 建议 |
|------|---------|-------------|------|
| 全文搜索引擎 | Elasticsearch / Tantivy | 7 | **集成 Lucene/Tantivy 增强 Document 模态** |
| 宽表 / Wide-Column | Cassandra / HBase / TiKV | 5 | 阶段化（CF → LSM → 分布式），或集成 TiKV |
| 对象 Blob 存储 | S3 / MinIO / JuiceFS | 8 | **不自研，集成 MinIO，mm_storage 暴露 BLOB 类型** |
| 可观测日志 | Loki / ClickHouse / VictoriaLogs | 6 | **集成 ClickHouse + Loki 风格标签索引** |
| 多模态 AI 原生 | LanceDB / Qdrant / Vespa | **9** | **优先投入**，与现有 RAG/Vector 互补，最契合 |

## 7. 关键观察与建议

1. **不自研的边界已清晰**：对象存储（集成 MinIO 即可）、全文搜索（集成 Lucene）、OLAP（集成 ClickHouse）——自研从零开始投入产出比极差
2. **优先投入的是多模态 AI 原生存储**（契合度 9）：现有 Vector + RAG 基础扎实，扩展为多模态是顺势而为
3. **宽表与可观测的契合度中等**（5-6）：可作为通用 SQL 平台的拓展场景，但不是当前关键路径
4. **集成而非重写是 2026 数据库主流**（参考 Snowflake 与 Iceberg 集成、Databricks 与 Delta Lake 集成）——自研多模态数据库的最优策略是"内核自研 + 外围集成"
