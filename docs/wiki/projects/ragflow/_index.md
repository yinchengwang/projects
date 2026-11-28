---
project: ragflow
page_type: overview
display_name: RAGFlow
repo: https://github.com/infiniflow/ragflow
license: Apache-2.0
language: Go
stars: 94522
stars_as_of: 2026-11-22
refs:
  - path: internal/entity/dataset.go
    symbols: [Knowledgebase, ParserType]
  - path: internal/service/chat_pipeline.go
    symbols: [ChatPipelineService, AsyncChat]
  - path: internal/ingestion/task/pipeline_executor.go
    symbols: [PipelineExecutor]
  - path: internal/engine/engine.go
    symbols: [DocEngine, EngineType]
  - path: internal/harness/graph/pregel/engine.go
    symbols: [Engine, NewEngine]
---
# RAGFlow

## 一句话职责

一个完整的 Go 服务端 RAG 产品：从 PDF/Office 文档的深度版面解析开始，经摄取流水线抽成分块与实体关系，落到可切换的块存储引擎（Elasticsearch / OpenSearch / Infinity），再以对话、Agent 画布与 agentic 检索三种形态对外提供问答。

## 机制是什么

ragflow 与前五个项目的差别**不是类型上的，而是量纲上的**，这一点决定了该看什么。

langchain / llama-index / haystack 是「可替换组件的编排框架」；lightrag 是「单一产品的 Python 实现」；graphrag 是「离线建索引的 monorepo」。**ragflow 是一个完整的 Go 服务端产品**：它同时拥有

- **文档解析**，包括 PDF 版面分析的 CGO 原生实现（文本框阅读顺序、去连字符、分栏归并、表格重建）与外部解析器桥接（MinerU / Docling / PaddleOCR / SoMark 等）；
- **摄取流水线**，从解析、抽取、分块、分词嵌入到知识编译（wiki 页面 / 结构树 / claims）；
- **文档引擎**，三个后端（Elasticsearch / OpenSearch / Infinity）同一份块存储契约；
- **检索引擎与 Agentic RAG**，含知识图谱多跳探索与精确串检索两条补充通道；
- **Agent 平台**，含一个 langgraph 风格的图执行内核 `harness/`（Pregel 超步、通道、检查点、中断、中间件、预制 Agent）；
- **接入与运维面**：MCP 服务端、外部数据源同步器、即时通讯渠道、资源权限模型、CLI。

规模因此不能按「框架」的密度估：`internal/service` 8.1 万行、`internal/ingestion` 6.2 万行、`internal/agent` 5.8 万行、`internal/entity` 5.2 万行、`internal/harness` 4.1 万行，`internal/` 下共 1581 个非测试 `.go` 文件、54.7 万行。

**它最值得单独读的一条主线是「一个 RAG 产品到底要写多少东西」**：真正让 RAG 可用的不是检索算法，而是版面解析、分块策略、块存储引擎抽象、增量同步、权限、失败重试这些「非算法」部分。本文库的组划分就是按这条主线切的。

本文库按「一次请求会穿过哪些层」组织：**8 个机制领域、共 49 篇机制页**。

## 怎么实现的

下表的 8 个领域按下列顺序排列：A 实体与服务层 → B 摄取流水线 → C 文档解析 → D 文档引擎与存储 → E 图执行内核 → F Agent 平台 → G Agentic RAG → H 同步接入与运维面。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：`internal/` 下 1581 个非测试 `.go` 文件。`web/`（React 前端）、`test/`、`docs/`、`sdk/`（多语言客户端）、`tools/`、`cmd/` 不建页。

**一次摄取的一句话概览**（细节见 B、C、D 组）：上传文档 → 判定文件族并装配解析器 → 解析成条目（文本/表格/图片）→ 分块（token / 标题 / 通用 / 分组四种组件）→ 分词与嵌入 → 抽取实体关系与标签 → 写块存储并记录解析基线锚点 → 知识编译产出 wiki 与结构树。

## 机制导航（49 篇 / 8 组）

### A-实体与服务层（5 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-entity-model.md`](a01-entity-model.md) | 实体模型与状态枚举 |
| [`a02-chat-pipeline.md`](a02-chat-pipeline.md) | 对话编排流水线 |
| [`a03-model-provider.md`](a03-model-provider.md) | 模型提供方与模型工厂 |
| [`a04-metadata-filter.md`](a04-metadata-filter.md) | 元数据过滤条件生成 |
| [`a05-dataset-artifact.md`](a05-dataset-artifact.md) | 知识库产物与技能检索 |

### B-摄取流水线（6 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-pipeline-executor.md`](b01-pipeline-executor.md) | 摄取任务执行器 |
| [`b02-parser-component.md`](b02-parser-component.md) | 解析组件与文件族分派 |
| [`b03-extractor.md`](b03-extractor.md) | 实体关系抽取组件 |
| [`b04-chunker-components.md`](b04-chunker-components.md) | 四种分块组件 |
| [`b05-tokenizer-embedder.md`](b05-tokenizer-embedder.md) | 分词与嵌入组件 |
| [`b06-knowledge-compile.md`](b06-knowledge-compile.md) | 知识编译：排产、消费与落盘 |

### C-文档解析（7 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-doc-item.md`](c01-doc-item.md) | 文档条目与解析结果 |
| [`c02-parser-registry.md`](c02-parser-registry.md) | 解析器注册与按类型分派 |
| [`c03-chunk-operator.md`](c03-chunk-operator.md) | 分块算子链 |
| [`c04-chapter-plan.md`](c04-chapter-plan.md) | 章节规划与 PDF 切片 |
| [`c05-pdf-layout.md`](c05-pdf-layout.md) | PDF 版面重建 |
| [`c06-pdf-table.md`](c06-pdf-table.md) | PDF 表格重建 |
| [`c07-media-budget.md`](c07-media-budget.md) | 媒体预算与落盘 |

### D-文档引擎与存储（4 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-doc-engine.md`](d01-doc-engine.md) | 文档引擎抽象 |
| [`d02-chunk-store-es.md`](d02-chunk-store-es.md) | ES 与 OpenSearch 块存储 |
| [`d03-infinity-engine.md`](d03-infinity-engine.md) | Infinity 引擎与块分页排序 |
| [`d04-blob-storage.md`](d04-blob-storage.md) | 对象存储后端 |

### E-图执行内核（10 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-graph-api.md`](e01-graph-api.md) | 状态图装配接口 |
| [`e02-graph-channels.md`](e02-graph-channels.md) | 通道与屏障汇合 |
| [`e03-checkpoint.md`](e03-checkpoint.md) | 检查点与待写记录 |
| [`e04-pregel-engine.md`](e04-pregel-engine.md) | Pregel 执行引擎 |
| [`e05-pregel-stream.md`](e05-pregel-stream.md) | 流式事件与消息聚合 |
| [`e06-interrupt.md`](e06-interrupt.md) | 中断与恢复 |
| [`e07-react-agent.md`](e07-react-agent.md) | ReAct Agent 与图形态 |
| [`e08-tools-node.md`](e08-tools-node.md) | 工具节点与工具注册表 |
| [`e09-middlewares.md`](e09-middlewares.md) | 中间件体系 |
| [`e10-prebuilt-agents.md`](e10-prebuilt-agents.md) | 预制 Agent 形态 |

### F-Agent 平台（6 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-canvas.md`](f01-canvas.md) | 画布与组件工厂 |
| [`f02-dsl.md`](f02-dsl.md) | DSL 抽取与归一 |
| [`f03-agent-runtime.md`](f03-agent-runtime.md) | Agent 运行时 |
| [`f04-tool-registry.md`](f04-tool-registry.md) | 平台工具注册与检索工具 |
| [`f05-sandbox.md`](f05-sandbox.md) | 代码执行沙箱 |
| [`f06-workflowx.md`](f06-workflowx.md) | 循环与并行结构 |

### G-Agentic RAG（4 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-agentic-run.md`](g01-agentic-run.md) | Agentic 检索主循环 |
| [`g02-kg-service.md`](g02-kg-service.md) | 知识图谱服务与探索 |
| [`g03-grep-service.md`](g03-grep-service.md) | 精确串检索服务 |
| [`g04-structure-and-search-tools.md`](g04-structure-and-search-tools.md) | 结构导航与 agentic 检索工具 |

### H-同步接入与运维面（7 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-syncer.md`](h01-syncer.md) | 同步器：调度与任务执行 |
| [`h02-syncer-lock.md`](h02-syncer-lock.md) | 同步锁与检查点 |
| [`h03-chat-channels.md`](h03-chat-channels.md) | 渠道接入与出站 |
| [`h04-mcp.md`](h04-mcp.md) | MCP 服务端与连接器 |
| [`h05-cli.md`](h05-cli.md) | 命令行与管理命令 |
| [`h06-permission.md`](h06-permission.md) | 资源权限模型 |
| [`h07-runtime-common.md`](h07-runtime-common.md) | 公共常量、Agent 事件与分词器 |

## 关键取舍

**源码事实：** Go 单体服务端，`internal/` 下 54.7 万行非测试代码；`harness/graph` 是 LangGraph 的 Go 移植（`StateGraph`、`Checkpointer`、`Interrupt`、`Command`、`Send`、`StreamMode`、`Durability` 术语与语义对齐）。

**源码事实：** 块存储有三个后端（Elasticsearch / OpenSearch / Infinity）共用同一份契约；对象存储有四个（内存 / MinIO / OSS / GCS）。

**源码事实：** PDF 解析是 CGO 原生实现（`deepdoc/parser/pdf/`），含版面阅读顺序重建、去连字符、分栏归并、表格构造，并有 `_cgo` / `_nocgo` 成对文件让无 CGO 环境下退化为桩。

**源码事实：** 摄取流水线的失败恢复靠「解析基线锚点」（`parseBaselineAnchor` / `recordParseBaseline`）记录块写入前后的基线，而不是事务。

**源码事实：** 图执行内核默认 `recursionLimit = 25`；任务选择有两种语义（`AnyPredecessor` 支持环、`AllPredecessor` 不支持环）；检查点持久化三档（Sync / Async / Exit）。

**源码事实：** 元数据过滤由 LLM 生成过滤条件（`GenMetaFilter`）再收窄检索范围，而不是纯结构化查询语言。

**架构推论：** 一个 RAG 产品里「检索算法」的代码占比极小，绝大部分是**让检索可用的周边**：版面解析精度、分块策略、引擎可替换、增量同步、失败恢复、权限。这也是为什么它的体量是 langchain 的十倍以上——框架只需要把组件接起来，产品要把每一环都做对。

**架构推论：** 三个块存储后端 + 四个对象存储后端的选择，是「客户已有基础设施」这个现实约束的产物。RAG 产品的部署环境差异极大（有的客户只有 ES，有的只有 S3 兼容存储），**支持多后端不是设计洁癖，是部署前提**。代价是每加一个后端都要写一遍完整的契约实现，且跨后端行为一致性只能靠测试保证。

**架构推论：** 用「基线锚点」而不是事务做失败恢复，是因为跨数据库（MySQL 元数据 + ES 块存储 + 对象存储）根本无法拿到一个事务域。这跟 lightrag 的结论完全一致，只是 ragflow 的跨度更大（不止四个存储，还有外部搜索引擎）。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `internal/entity/dataset.go` | `Knowledgebase` | 知识库实体与状态枚举 |
| `internal/service/chat_pipeline.go` | `ChatPipelineService` | 对话编排 |
| `internal/ingestion/task/pipeline_executor.go` | `PipelineExecutor` | 摄取任务执行 |
| `internal/ingestion/component/parser.go` | `ParserComponent` | 摄取侧解析组件 |
| `internal/parser/parser/doc_item.go` | `DocItem` | 解析产物条目模型 |
| `internal/deepdoc/parser/pdf/layout/layout.go` | `TextMerge` | PDF 版面阅读顺序 |
| `internal/deepdoc/parser/pdf/table/table_construct.go` | `ConstructTable` | PDF 表格重建 |
| `internal/engine/engine.go` | `DocEngine` | 块存储引擎抽象 |
| `internal/storage/memory.go` | `MemoryStorage` | 对象存储后端之一 |
| `internal/harness/graph/pregel/engine.go` | `Engine` | Pregel 超步执行 |
| `internal/harness/core/react_agent.go` | `ReActAgent` | ReAct Agent |
| `internal/agent/canvas/canvas.go` | `Canvas` | Agent 画布 |
| `internal/agentic_rag/agent.go` | `Run` | agentic 检索主循环 |
| `internal/syncer/syncer.go` | `Syncer` | 外部数据源同步 |
| `internal/permission/policy.go` | `CheckResource` | 资源权限判定 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 官方文档](https://ragflow.io/docs/)
- [Pregel 论文（Malewicz et al., 2010）](https://dl.acm.org/doi/10.1145/1807167.1807184)
- [LangGraph 文档：图执行](https://langchain-ai.github.io/langgraph/concepts/low_level/)
- [ragflow 源码：internal/harness/graph/pregel/engine.go](https://github.com/infiniflow/ragflow/blob/main/internal/harness/graph/pregel/engine.go)

## 与主核 / AI 的关系

ragflow 是本文库目前**与主核（多模态数据库内核）最可类比的项目**：它真的有存储层、有引擎抽象、有多后端、有失败恢复。对比价值比前五个 RAG 框架都大。

**块存储引擎抽象 vs 主核的存储层。** 两者都在「一份逻辑数据 + 多个物理后端」上做抽象，但契约厚度完全不同：主核只有一套自研内核，接口可以很薄（写成功就是成功，因为有 WAL 与事务）；ragflow 要支撑三个外部搜索引擎，**每个后端的失败语义都不一样**（有的分段提交、有的异步索引），于是只能靠「基线锚点 + 事后核对」做恢复。**再次印证同一条规律：可靠性保证越弱，契约必须越啰嗦。**

**基线锚点 vs WAL。** 主核用 WAL 记录操作、崩溃后重放；ragflow 记录「块写入前的基线」、失败后靠它判断该回到哪个状态。前者是**前向恢复**（重放完成的操作），后者是**后向核对**（比对基线找出不一致）。前向恢复需要「操作可重放」，后向核对只需要「状态可比较」——在跨异构存储的场景下，后者门槛低得多。

**Pregel 超步 vs 事务隔离。** 同一问题的两种答案已在 e04 展开：数据库愿意为吞吐放松隔离级别，Pregel 不愿为吞吐放松确定性。对 Agent 系统来说确定性优先，因为模型本身已经在引入不确定性。

**三后端一致性 vs 数据库的多存储引擎。** MySQL 的 InnoDB/MyISAM 差别在事务与锁；ragflow 的 ES/OpenSearch/Infinity 差别在检索能力（向量、全文、混合）与索引延迟。两者都要回答同一个问题：**换了引擎，语义变了吗？** 数据库靠「SQL 层统一语义」回答，ragflow 靠「契约 + 跨后端测试」回答——前者有标准可依，后者只能自己定义标准。

**权限模型 vs 数据库的 RBAC。** `internal/permission` 的 `ResourceKind` / `ResourceRef` / `CheckResource` 与数据库的对象级权限同构：都是「资源引用 + 操作 + 主体」三要素判定。差别是数据库的权限在连接/语句层生效（拒绝即报错），ragflow 的权限在服务层生效（拒绝可以表现为「返回空结果」）——**后者更难审计**，因为「没有权限」与「没有数据」在结果上不可区分。

对 agent 工程最直接的价值的四条。其一：**版面解析是 RAG 质量的实际上限**——检索再准，解析错了也白搭，而这部分恰恰是纯算法代码写不出来的（需要 CGO 与大量启发式）。其二：**Agent 平台需要图执行内核**，把 ReAct 循环、工具节点、中间件、中断恢复做成可组合的层，比手写循环可靠得多。其三：**agentic 检索是「让模型自己决定查什么」的实现**，它需要台账（读过哪些块、服务过哪些文档）才能避免重复劳动，这是纯向量检索没有的问题。其四：**跨异构存储的一致性只能靠基线核对**，任何预计算或多后端方案都要面对这一点。
