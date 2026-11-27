# catalog 扩容候选审阅矩阵（270 项目标）

> **当前阶段：候选集合已按“广义基础设施范围”冻结为 54 + 64 个唯一 canonical repo，仍不是正式 catalog additions。** 2026-11-27 对其中约 20 个候选做了 GitHub REST 抽查并记录事实；其余条目尚未逐个联网核验，不得把抽查结论外推到整批。写入 catalog 前必须逐条取得 stars / license / language / stars_as_of 真实值。
>
> **收录口径（用户 2026-11-27 确认）**：允许具备独立源码机制的数据库、存储、扩展、消息/流、搜索、时序、向量、benchmark、Agent 框架与 SDK、RAG 解析/检索/重排/评测/可观测、LLM runtime/serving/kernel/训练基础设施。排除纯数据集、纯展示产品、失效或错误指向仓库、同一 canonical repo 的重复语言 SDK、无可盘点机制条目。
>
> **本地基线（2026-11-26 复核）**：从当前 checkout `projects/docs/wiki/catalog-projects.yml` 用 `wiki_catalog.parse_catalog()` 读取，得到 152 项：数据库/数据基础设施 96、AI 三分类 56；目标分别为 150 和 120，净增 54 和 64，共 118。少数代理汇报的 121 是局部/错误读取，不能作为计数依据。
>
> **Tier 仅为审阅建议**：T1=领域核心或广泛采用；T2=重要代表实现；T3=窄领域/补充。现有 T1/T2/T3 的定义包含 stars 门槛，因此在 API metadata 回填前，表中“建议”只是暂定判断，不是最终分层。

---

## 1. 目标与数据库已完成项目别名

| 分类口径 | 当前 | 目标 | 净增 |
|---|---:|---:|---:|
| 数据库及数据基础设施（所有非 `ai/*` 分类） | 96 | 150 | 54 |
| AI（`ai/agent`、`ai/rag`、`ai/llm`） | 56 | 120 | 64 |
| **总计** | **152** | **270** | **118** |

已完成项目的目录名映射，别名不另计项目：`mysql` → `mysql-server`；`sqlite3` → `sqlite`。Postgres 和 Redis 目录名不变。

**新增计数按 distinct canonical GitHub repo，不按候选显示名。** 同仓的 Python / JS SDK、项目拆分包、组织迁移前后名字不能自动重复计数；别名需指向主记录。以下每项仍须人工检查 repo URL 和 canonical identity。

---

## 2. 数据库 / 数据基础设施候选：54 项

以下拟映射到 catalog 已有 13 个非 AI 分类。项目数量只为满足分类覆盖的初始 proposal，后续复核允许调配类别，但 DB 净新增必须保持 54。

| 分类键 | 目标新增 | 候选（名称 — Likely canonical repo — Tier 提议 — 纳入理由） |
|---|---:|---|
| relational | 3 | `derby` — `https://github.com/apache/derby` — T2 — Apache Derby 的 GitHub 镜像仓；API 明示 mirror，2026-11-27 API 返回未归档、Java、Apache-2.0、379 stars、最近 push 2026-05-15。若不接受只读镜像仓则需替补。<br>`libsql` — `https://github.com/tursodatabase/libsql` — T2 — SQLite 派生的 edge/replicated SQL；确认与 `sqlite3` 的 fork/项目边界以及目录 policy。<br>`ignite` — `https://github.com/apache/ignite` — T2 — 分布式内存计算、缓存与 SQL；需确认归 relational 还是 key-value。 |
| key-value | 4 | `aerospike` — `https://github.com/aerospike/aerospike-server` — T1 — 低延迟分布式键值/文档存储。<br>`garnet` — `https://github.com/microsoft/garnet` — T1 — Redis protocol 兼容但实现独立的内存数据存储。<br>`memcached` — `https://github.com/memcached/memcached` — T1 — 分布式内存 KV 缓存；2026-11-27 GitHub API 返回未归档、C、BSD-3-Clause、14314 stars、最近 push 2026-11-24，可补足 DB 净增缺口。<br>`tarantool` — `https://github.com/tarantool/tarantool` — T2 — 内存数据库与 Lua 应用运行时。<br>`infinispan` — `https://github.com/infinispan/infinispan` — T2 — Apache Infinispan data grid / scalable NoSQL store；API 已核验 active / Apache-2.0 / Java，可作为独立 key-value/data-grid 机制项目。 |
| columnar | 5 | `kudu` — `https://github.com/apache/kudu` — T2 — 支持快速更新的列式存储引擎。<br>`monetdb` — `https://github.com/MonetDB/MonetDB` — T2 — 列式 SQL 分析数据库。<br>`carbondata` — `https://github.com/apache/carbondata` — T1 — Apache 高性能数据存储与列式分析；2026-11-27 GitHub API 返回未归档、Scala、Apache-2.0、1452 stars、最近 push 2026-11-26，可替换已归档 HAWQ。<br>`hyrise` — `https://github.com/hyrise/hyrise` — T2 — 可运行的内存列存研究型数据库。<br>`apache-impala` — `https://github.com/apache/impala` — T1 — MPP SQL 查询引擎；需确认按本 taxonomy 放 columnar，而非 distributed-sql。 |
| document | 3 | `litedb` — `https://github.com/mbdavid/LiteDB` — T2 — .NET 嵌入式文档数据库。<br>`ejdb` — `https://github.com/Softmotions/ejdb` — T3 — 嵌入式 JSON 文档存储。<br>`pouchdb` — `https://github.com/pouchdb/pouchdb` — T2 — 本地优先文档数据库与同步模型；确认与数据库 wiki 范围相符。 |
| graph | 3 | `typedb` — `https://github.com/typedb/typedb` — T1 — 类型化知识图谱与查询模型。<br>`terminusdb` — `https://github.com/terminusdb/terminusdb` — T2 — 可版本化的图 / 文档数据库。<br>`orientdb` — `https://github.com/orientechnologies/orientdb` — T2 — 多模型图与文档数据库；核对仓库维护状态。 |
| search | 2 | `bleve` — `https://github.com/blevesearch/bleve` — T1 — Go text/numeric/geo/vector indexing library；2026-11-27 GitHub API 返回未归档、Go、Apache-2.0、11261 stars、最近 push 2026-11-25，可替换已归档 ZincSearch。<br>`sonic` — `https://github.com/valeriansaliou/sonic` — T2 — 轻量搜索索引 / 查询服务器；维护、license 和版本范围仍待核验。 |
| stream | 4 | `rabbitmq-server` — `https://github.com/rabbitmq/rabbitmq-server` — T1 — 成熟消息 broker，协议支持广。<br>`apache-artemis` — `https://github.com/apache/artemis` — T1 — Apache Artemis 多协议消息 broker；2026-11-27 GitHub API canonical repo 是 `apache/artemis`，Apache-2.0，未归档、Java，1073 stars、最近 push 2026-11-25。<br>`bookkeeper` — `https://github.com/apache/bookkeeper` — T2 — 分布式日志 / 存储层，为消息系统提供持久化；类别边界需审阅。<br>`zookeeper` — `https://github.com/apache/zookeeper` — T2 — 分布式协调与配置服务；仅在项目范围包含基础设施协调机制时纳入。 |
| time-series | 2 | `m3` — `https://github.com/m3db/m3` — T2 — 分布式指标存储与查询系统。<br>`openobserve` — `https://github.com/openobserve/openobserve` — T2 — 可观测性日志 / 指标 / trace 存储；确认按 time-series 还是 search 更合适。 |
| vector | 2 | `diskann` — `https://github.com/microsoft/DiskANN` — T1 — 大规模近似最近邻算法及磁盘索引实现。<br>`marqo` — `https://github.com/marqo-ai/marqo` — T2 — 多模态向量搜索系统；核验其与现有向量数据库类别不重复。 |
| distributed-sql | 4 | `vitess` — `https://github.com/vitessio/vitess` — T1 — MySQL 横向扩展与分片系统。<br>`shardingsphere` — `https://github.com/apache/shardingsphere` — T1 — 数据库分片 / 分布式 SQL 中间件。<br>`ydb` — `https://github.com/ydb-platform/ydb` — T1 — 分布式 SQL / KV 数据库。<br>`citus` — `https://github.com/citusdata/citus` — T1 — PostgreSQL 分布式查询扩展；确认其独立 repo 与 extension 分组的归属，避免和 extension 重复计数。 |
| embedded | 2 | `rqlite` — `https://github.com/rqlite/rqlite` — T2 — 基于 SQLite 的复制关系数据库。<br>`litefs` — `https://github.com/superfly/litefs` — T2 — SQLite 文件复制 / failover 层；属于数据库基础设施而非完整 DBMS，纳入边界须核准。 |
| extension | 12 | `postgis` — `https://github.com/postgis/postgis` — T1 — PostgreSQL 空间数据扩展。<br>`paradedb` — `https://github.com/paradedb/paradedb` — T1 — PostgreSQL 内全文 / BM25 搜索扩展。<br>`pg-partman` — `https://github.com/pgpartman/pg_partman` — T2 — PostgreSQL 分区生命周期管理。<br>`pg-cron` — `https://github.com/citusdata/pg_cron` — T2 — 数据库内调度扩展。<br>`pgmq` — `https://github.com/tembo-io/pgmq` — T2 — PostgreSQL 原生消息队列扩展。<br>`vectorchord` — `https://github.com/tensorchord/VectorChord` — T2 — PostgreSQL 向量检索扩展；与现有 pgvector 不同实现。<br>`pgaudit` — `https://github.com/pgaudit/pgaudit` — T2 — PostgreSQL 审计扩展。<br>`pgsodium` — `https://github.com/michelp/pgsodium` — T2 — PostgreSQL 加密扩展；核验维护与依赖边界。<br>`pg_ivm` — `https://github.com/sraoss/pg_ivm` — T2 — PostgreSQL 增量视图维护扩展。<br>`pg_duckdb` — `https://github.com/duckdb/pg_duckdb` — T2 — PostgreSQL / DuckDB 集成扩展；需明确双项目边界。<br>`pgroonga` — `https://github.com/pgroonga/pgroonga` — T2 — PostgreSQL 全文检索扩展。<br>`pg_tle` — `https://github.com/aws/pg_tle` — T2 — PostgreSQL Trusted Language Extensions，扩展开发与部署管理机制。<br>`pglogical` — `https://github.com/2ndQuadrant/pglogical` — T2 — PostgreSQL logical replication extension；repo canonical、当前归属和维护状态待 API/source 复核，暂作 extension proposal。 |
| benchmark | 8 | `ycsb` — `https://github.com/brianfrankcooper/YCSB` — T1 — 多种云端 / KV 存储 workload 基准。<br>`sysbench` — `https://github.com/akopytov/sysbench` — T1 — 可脚本化数据库与系统性能基准；2026-11-27 API 核验存在、未归档、C、GPL-2.0、6817 stars。<br>`benchbase` — `https://github.com/cmu-db/benchbase` — T2 — OLTP / OLAP 工作负载基准套件。<br>`hammerdb` — `https://github.com/TPC-Council/HammerDB` — T2 — 数据库 workload 与 benchmark 工具。<br>`oltpbench` — `https://github.com/oltpbenchmark/oltpbench` — T3 — 多 DBMS benchmark framework；2026-11-27 API 确认已归档（最后 push 2021-08-23）、415 stars、Java、license Other/NOASSERTION；作为历史机制候选，是否计入正式配额待裁定。<br>`clickbench` — `https://github.com/ClickHouse/ClickBench` — T2 — OLAP 查询基准；repo canonical 与独立 benchmark 定位待核验。 |
| **DB proposal subtotal** | **54** | **上表分类数相加为 54，表内 name 与 repo 唯一、与现有 catalog 零冲突；`oltpbench` 的归档资格与 GitHub mirror 仓是否接受仍待裁定。** |

### DB 分类数量的审阅规则

上述 54 项是按候选名/仓库草案分配的数量，尚不是已批准清单。已完成的处置：`apache-hawq` 归档 → 换 `carbondata`；`zincsearch` 归档 → 换 `bleve`；`hsqldb` 指向非官方 mirror 且无 SPDX license → 换 `apache/derby`（仍属 Apache mirror，若要纯源码仓需再换）；`dbt` 与 `go-ycsb` 已移出；`consul` 换成 `memcached`，保留 `zookeeper` 作为数据库周边基础设施。仍待裁定：`oltpbench` 已归档（2021-08-23 最后 push、license Other），是否允许作为历史机制项目计入；`derby` / `derby`、`MonetDB` 等属于 GitHub mirror，是否接受只读镜像仓。即使候选行数凑足 54，也不得跳过这些 include/exclude 裁定。
这份 54 项草案的表格需要精确按行计算；行内若标为“替补候选”，不得同时计入正式配额。特别是：

- 当前 catalog 已含 `h2`、`cassandra`、`hbase`、`solr`、`tantivy`、`trino`、`presto`、`manticoresearch` 等。相同 repo 的大小写/缩写、`-server` 变体、上游 org 变更都必须按 canonical URL 去重。
- 当前 catalog 已含 `meilisearch`、`typesense`、`quickwit`、`opensearch`、`qdrant`、`chroma`、`milvus`、`weaviate`、`vespa`、`vald`、`victoriametrics`、`greptimedb`、`tdengine`、`timescaledb`、`nats`、`pulsar`、`redpanda`、`etcd`、`firebird`、`mariadb`、`rethinkdb` 等，不能把展示名加前缀就当新增项目。
- 需优先审阅 genuine independent repo，而不是只看类别名补足。若某些分类实在没有足够高质量候选，报告候选不足并请用户裁定是否调配 DB 子分类，不可虚构项目。

---

## 3. AI / Agent / RAG / LLM 候选：64 项

维持现有三个类别键。已冻结 24 Agent + 20 RAG + 20 LLM = 64 个唯一 canonical repo；其中约 12 条已取得 GitHub API 事实（semantic-kernel、TypeChat、FlashInfer、DeepEP、LMCache、openrag、google/adk-js 等），其余待逐条核验。不同语言 SDK 不因名称不同自动计两个项目。

| 分类键 | 目标新增 | 候选（名称 — Likely repo — Tier 提议 — 纳入理由） |
|---|---:|---|
| ai/agent | 24 | `semantic-kernel` — `https://github.com/microsoft/semantic-kernel` — T1 — 多语言 AI 编排 SDK / 插件与 Agent 工作流。<br>`langroid` — `https://github.com/langroid/langroid` — T2 — message-passing 风格的多 Agent Python 框架。<br>`praisonai` — `https://github.com/MervinPraison/PraisonAI` — T2 — 多 Agent 编排与部署工具。<br>`agency-swarm` — `https://github.com/VRSEN/agency-swarm` — T2 — 可组合 Agent / tool / communication flow。<br>`swe-agent` — `https://github.com/SWE-agent/SWE-agent` — T1 — 软件工程 Agent 与 benchmark 体系。<br>`browsergym` — `https://github.com/ServiceNow/BrowserGym` — T1 — browser agent 执行环境与评测基准。<br>`stagehand` — `https://github.com/browserbase/stagehand` — T1 — AI browser automation 框架。<br>`skyvern` — `https://github.com/Skyvern-AI/skyvern` — T1 — 面向复杂网站流程的视觉 / 浏览器自动化。<br>`lavague` — `https://github.com/lavague-ai/LaVague` — T2 — Web agent 与浏览器交互框架。<br>`beeai-framework` — `https://github.com/i-am-bee/beeai-framework` — T2 — 可复用 Agent 构件与 multi-agent 编排。<br>`burr` — `https://github.com/apache/burr` — T2 — 带状态持久化与可观测性的 Agent / app 编排。<br>`google-adk` — `https://github.com/google/adk-python` — T1 — Google Python Agent Development Kit（只计 Python 主仓；JS SDK 是同产品族，不重复计）。<br>`langflow` — `https://github.com/langflow-ai/langflow` — T1 — 可视化 Agent / LLM flow builder；页面聚焦 flow/runtime 机制。<br>`flowise` — `https://github.com/FlowiseAI/Flowise` — T1 — 可视化 LLM 与 Agent workflow 平台。<br>`agentuniverse` — `https://github.com/antgroup/agentUniverse` — T2 — 企业多 Agent 协作框架；注意与 agentverse 名称相近但 repo 独立。<br>`agentlite` — `https://github.com/OSU-NLP-Group/AgentLite` — T3 — 轻量研究型 tool-using Agent。<br>`aider` — `https://github.com/Aider-AI/aider` — T1 — 终端 AI 编码 Agent。<br>`cline` — `https://github.com/cline/cline` — T1 — 开源 IDE 编码 Agent 与 tool execution。<br>`mem0` — `https://github.com/mem0ai/mem0` — T1 — 给 Agent / 应用提供持久化记忆层。<br>`typechat` — `https://github.com/microsoft/TypeChat` — T2 — TypeScript library for natural-language interfaces using types；2026-11-27 API 返回未归档、MIT、JavaScript、8695 stars。<br>`vercel-ai` — `https://github.com/vercel/ai` — T1 — TypeScript AI SDK 的 tool / stream / provider orchestration；2026-11-27 API 存在且未归档，license API 标 `NOASSERTION`，需人工确认 SPDX。<br>`ag2` — `https://github.com/ag2ai/ag2` — T2 — 多 Agent 编排框架；需核验与 AutoGen 的历史分叉/独立 repo 身份。<br>`mcp-go` — `https://github.com/mark3labs/mcp-go` — T2 — Go MCP implementation；协议工具/框架而非完整 Agent，分类边界中等，最终纳入需按 scope 审核。<br>`semantic-router` — `https://github.com/aurelio-labs/semantic-router` — T2 — 基于语义路由的模型/工具选择机制。 |
| ai/rag | 20 | `flashrag` — `https://github.com/RUC-NLPIR/FlashRAG` — T2 — RAG 方法研究与评估工具包。<br>`rag-anything` — `https://github.com/HKUDS/RAG-Anything` — T2 — 面向多模态资料的 RAG 框架。<br>`ragatouille` — `https://github.com/AnswerDotAI/RAGatouille` — T2 — ColBERT retrieval 工具链。<br>`infinity` — `https://github.com/michaelfeil/infinity` — T2 — embedding / reranking 高吞吐推理服务。<br>`openrag` — `https://github.com/langflow-ai/openrag` — T3 — 开放 RAG stack；GitHub API 2026-11-27 显示该 canonical repo 有效、未归档、Apache-2.0；机制页需聚焦 retrieval pipeline。<br>`kag` — `https://github.com/OpenSPG/KAG` — T2 — 基于结构化知识的知识增强生成。<br>`hipporag` — `https://github.com/OSU-NLP-Group/HippoRAG` — T2 — 图式检索研究框架。<br>`graphiti` — `https://github.com/getzep/graphiti` — T2 — 面向 Agent 的时序知识图记忆与检索。<br>`unstructured` — `https://github.com/Unstructured-IO/unstructured` — T1 — 多格式文件解析及 RAG ingestion；纳入前处理层。<br>`docling` — `https://github.com/docling-project/docling` — T1 — 文档解析 / 版面结构化，为 RAG 提供输入。<br>`marker` — `https://github.com/datalab-to/marker` — T2 — PDF / 文档转文本与结构抽取。<br>`mineru` — `https://github.com/opendatalab/MinerU` — T2 — 多格式文档解析与版面抽取。<br>`colbert` — `https://github.com/stanford-futuredata/ColBERT` — T1 — late-interaction 神经检索。<br>`pyserini` — `https://github.com/castorini/pyserini` — T2 — IR / sparse+dense retrieval 工具箱。<br>`flashrank` — `https://github.com/PrithivirajDamodaran/FlashRank` — T2 — 轻量 reranking 工具。<br>`vanna` — `https://github.com/vanna-ai/vanna` — T2 — RAG 驱动的 Text-to-SQL 框架。<br>`giskard` — `https://github.com/Giskard-AI/giskard` — T2 — LLM / RAG 应用评测和测试；按评测机制入选。<br>`ragchecker` — `https://github.com/amazon-science/RAGChecker` — T2 — RAG 系统评估与诊断；评测机制而非 runtime。<br>`phoenix` — `https://github.com/Arize-ai/phoenix` — T1 — LLM/RAG tracing 与评估平台；机制页聚焦 trace/evaluation。<br>`langfuse` — `https://github.com/langfuse/langfuse` — T1 — LLM observability 与评估平台；机制页聚焦 trace/score pipeline。 |
| ai/llm | 20 | `dspy` — `https://github.com/stanfordnlp/dspy` — T1 — 可声明式编程与优化 LM pipeline。<br>`instructor` — `https://github.com/instructor-ai/instructor` — T1 — schema / type 约束的 LLM 结构化输出。<br>`promptfoo` — `https://github.com/promptfoo/promptfoo` — T1 — LLM prompt/app 测试、评估与红队。<br>`deepeval` — `https://github.com/confident-ai/deepeval` — T1 — LLM / RAG regression testing 与 metrics。<br>`guidance` — `https://github.com/guidance-ai/guidance` — T2 — 受约束生成与结构化解码。<br>`outlines` — `https://github.com/dottxt-ai/outlines` — T1 — 结构化 generation / decoding。<br>`mlx` — `https://github.com/ml-explore/mlx` — T1 — Apple Silicon ML runtime 与本地模型生态。<br>`tensorrt-llm` — `https://github.com/NVIDIA/TensorRT-LLM` — T1 — GPU 推理优化库、kernel 与 runtime。<br>`mlc-llm` — `https://github.com/mlc-ai/mlc-llm` — T1 — 跨平台模型编译与部署。<br>`exllamav2` — `https://github.com/turboderp/exllamav2` — T2 — 量化模型本地推理 engine。<br>`sonar` — `https://github.com/dphnAI/sonar` — T2 — 大规模 LLM inference engine；GitHub API 将旧 Aphrodite URL 重定向至此 canonical repo，全表仅保留此单条记录，不重复计旧名。<br>`localai` — `https://github.com/mudler/LocalAI` — T1 — 多后端、本地 OpenAI-compatible inference server。<br>`xinference` — `https://github.com/xorbitsai/inference` — T1 — 多模型 open inference serving 平台。<br>`candle` — `https://github.com/huggingface/candle` — T1 — Rust ML framework，含 LLM 推理。<br>`onnxruntime-genai` — `https://github.com/microsoft/onnxruntime-genai` — T1 — generative AI runtime APIs 与优化。<br>`transformers` — `https://github.com/huggingface/transformers` — T1 — 模型加载、训练与推理基础库。<br>`peft` — `https://github.com/huggingface/peft` — T1 — 参数高效微调方法与工具。<br>`flashinfer` — `https://github.com/flashinfer-ai/flashinfer` — T1 — CUDA kernel library for LLM serving；2026-11-27 GitHub API 返回未归档、CUDA、Apache-2.0、7013 stars。<br>`deepep` — `https://github.com/deepseek-ai/DeepEP` — T1 — MoE expert-parallel communication library；2026-11-27 API 返回未归档、CUDA、MIT、10477 stars。<br>`lmcache` — `https://github.com/LMCache/LMCache` — T1 — LLM KV cache 复用与存储层；2026-11-27 GitHub API 返回未归档、Python、Apache-2.0、12491 stars。 |
| **合计** | **64** | **agent 24 + rag 20 + llm 20；表内 name 与规范化 repo URL 均唯一，与当前 catalog 零冲突。** |

---

## 3. 候选规范与风险

1. **没有 metadata 冒充已验证。** 本矩阵中 URL 仅为提案来源字符串，所有 stars / language / license / activity 待 Task 2 使用 GitHub API 核实。遇到 GitHub API 未认证速率限制，分轮缓存和记录，替换不可核实候选，不伪造值。
2. **重复识别采用两层。** 比对 catalog `name`（大小写归一），再比对规范化 GitHub repo URL；不能只看候选文字名称。组织迁移、fork、项目别名、多个语言 SDK 需人工判断 canonical identity。
3. **跨分类不重复计数。** Chroma/Qdrant/Milvus/Weaviate/Vespa/LanceDB/pgvector/sqlite-vec/FAISS/hnswlib 属现有 DB/vector/extension 范围，不能算成 AI 追加；向量检索对 AI 有用途不等于项目归属 AI。
4. **项目族和语言实现。** Google ADK 只保留 Python 主仓 `google-adk`，JS SDK 不单独计数；YCSB 与 go-ycsb 属同一项目族实现，不双计；`aphrodite-engine` 与 `sonar` 指向同一 canonical repo，只保留 `sonar`。
5. **类别数量可因审阅调整。** 它们是 proposal distribution，不是先验强制配额；项目若被跨类、排除或 repo 合并，应在 review 表中保留“旧候选 → 判定 → 替补”记录。DB 54 / AI 64 为 net additions 数，不是保留坏候选的理由。

---

## 4. 审阅结果与阶段 0A Task 1 出口

| 工作项 | 结果 |
|---|---|
| catalog / alias / repo 静态去重 | **通过（本地静态核对）** | DB 54、Agent 24、RAG 20、LLM 20，共 118；四个分区内部 name 与 repo URL 均唯一，与当前 152 项 catalog 的 name / repo slug / repo URL 零精确冲突。这不等于 URL 可访问、项目活跃或 metadata 真实。 |
| 候选范围 / 分类审阅 | **已按广义范围完成一轮** | 用户确认允许 benchmark、解析、评测、SDK、runtime 等机制项目；纯数据集（如 RAGTruth）已剔除，归档项目（HAWQ、ZincSearch、TaskWeaver）已标注或替换，重复语言实现（ADK JS、go-ycsb）不再双计，`aphrodite-engine` 归并为唯一 `sonar`。 |
| live metadata 抓取 | **部分完成** | 2026-11-27 抽查约 20 条：carbondata、bleve、apache/artemis、apache/activemq、apache/derby、sysbench、TypeChat、FlashInfer、DeepEP、LMCache、memcached、apache/ozone、infinispan、openrag、google/adk-js、微軟 semantic-kernel 等有 API 事实记录；其余尚未逐个核验。 |
| 正式入选 | **未完成** | 118 条仍是候选配额；待整批 metadata 与 final include/exclude 复核后才可写入 catalog。 |

> **重要：本次 Task 1 是候选提案，不是最终 include 决议。** 表中的 category 分布需要在后续 review 里逐项精确审阅；若最终无法提供 54 + 64 个高质量唯一 canonical repos，应明确短缺并请用户决定调整范围/配额，而不是用不实项目凑数。

### 明确下一步

**下一步**：候选集合已冻结为 118 个唯一 canonical repo（DB 54 / Agent 24 / RAG 20 / LLM 20），且与现有 catalog 零冲突。剩余工作：（1）逐条实取 GitHub metadata，未取到真实值的不写入；（2）对 `oltpbench`、GitHub mirror 仓等做 include/exclude 裁定；（3）通过 additions 测试后合并 catalog 到 270；（4）建立项目状态账本。Catalog 冻结的同时即可开始第一批项目源码 wiki 生产，不必等全部 metadata 完成。

阶段 0A 实施顺序与持续进度维护详见 [`2026-11-26-wiki-catalog-270-implementation.md`](../superpowers/plans/2026-11-26-wiki-catalog-270-implementation.md) 与总路线 [`project-roadmap.md`](project-roadmap.md)。
