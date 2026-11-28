---
project: graphrag
page_type: overview
display_name: GraphRAG
repo: https://github.com/microsoft/graphrag
license: MIT
language: Python
stars: 36717
stars_as_of: 2026-11-22
refs:
  - path: packages/graphrag/graphrag/config/models/graph_rag_config.py
    symbols: [GraphRagConfig]
  - path: packages/graphrag/graphrag/index/run/run_pipeline.py
    symbols: [run_pipeline]
  - path: packages/graphrag/graphrag/graphs/hierarchical_leiden.py
    symbols: [hierarchical_leiden]
  - path: packages/graphrag/graphrag/query/structured_search/global_search/search.py
    symbols: [GlobalSearch]
  - path: packages/graphrag-llm/graphrag_llm/middleware/with_middleware_pipeline.py
    symbols: [with_middleware_pipeline]
---
# GraphRAG

## 一句话职责

微软研究院的图谱增强 RAG 实现：它先离线把文档抽成实体关系图、用层次 Leiden 算法划分社区、给每个社区生成一份摘要报告；查询时检索的是**社区报告**而不是原始文本块，因此能回答「这个数据集的主要主题是什么」这类全局性问题。

## 机制是什么

graphrag 与同阶段其他四个框架的差别在**工作重心**上，这一点决定了该看什么。

langchain、llama-index、haystack 的中心是「编排调用」：把可替换的组件拼成管道，图是可选的加分项。lightrag 的中心是「图即索引」：从文档抽实体关系图，查询时沿图扩展。**graphrag 的中心是「离线建索引」**：它的绝大部分代码和绝大部分成本都在索引阶段，查询阶段反而相对薄。

这个差别带来一个直接后果：**查询时检索的对象不是文本块，而是社区报告**。社区是图上的一簇实体，报告是「这一簇实体的共同主题、关键发现、评分」。于是「全局性问题」从「检索若干文本块让模型总结」（必然片面，因为没有单个文本块包含全局视角）变成「检索已经总结好的社区报告」。

代价是显式的：**索引成本高**（每个社区一次 LLM 调用）、**增量更新困难**（新文档会改变图结构，社区的划分可能整体变化）。收益是「全局性问题」第一次有了可用的答案。

**另一条值得注意的主线是它的工程化程度**：graphrag 被拆成 8 个独立包（`graphrag-llm`、`graphrag-cache`、`graphrag-storage`、`graphrag-vectors`、`graphrag-chunking`、`graphrag-input`、`graphrag-common`、主包 `graphrag`）。其中 `graphrag-llm` 把「补全」做成了可以被中间件层层包裹的抽象，这套设计比同阶段其他框架的模型层都更彻底。

本文库按「配置 → 索引 → 图算法 → 查询 → 支撑层」的顺序组织：**7 个机制领域、共 31 篇机制页**。

## 怎么实现的

下表的 7 个领域按下列顺序排列：A 配置与数据模型 → B 索引工作流 → C 图算法与社区 → D 查询 → E LLM 层 → F 独立子包 → G 调用面与回调。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：`packages/` 下的 8 个包共 423 个 py 文件。`tests/`、`unified-search-app/`（一个独立的 Web 检索前端）、`docs/`、`scripts/` 不建页。

**索引流水线的一句话概览**（细节见 B 组）：载入文档 → 切块成文本单元 → LLM 抽取实体与关系（含 gleaning 多轮追加）→ 合并成图并算度数/边权 → 层次 Leiden 划分社区 → 每个社区生成报告 → 生成嵌入 → 拼装最终产物表。

## 机制导航（31 篇 / 7 组）

### A-配置与数据模型（4 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-graph-rag-config.md`](a01-graph-rag-config.md) | 总配置结构 |
| [`a02-config-enums.md`](a02-config-enums.md) | 枚举与索引方法 |
| [`a03-data-model.md`](a03-data-model.md) | 索引产物数据模型 |
| [`a04-row-transformers.md`](a04-row-transformers.md) | DataFrame 行转换与强类型视图 |

### B-索引工作流（6 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-chunking-workflow.md`](b01-chunking-workflow.md) | 文档分块工作流 |
| [`b02-graph-extraction.md`](b02-graph-extraction.md) | 实体关系抽取工作流 |
| [`b03-graph-finalize.md`](b03-graph-finalize.md) | 图收尾与度数计算 |
| [`b04-communities-workflow.md`](b04-communities-workflow.md) | 社区划分与报告生成 |
| [`b05-embeddings-and-final.md`](b05-embeddings-and-final.md) | 文本嵌入与最终产物 |
| [`b06-pipeline-run.md`](b06-pipeline-run.md) | 流水线执行与增量更新 |

### C-图算法与社区（3 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-hierarchical-leiden.md`](c01-hierarchical-leiden.md) | 层次 Leiden 社区划分 |
| [`c02-modularity.md`](c02-modularity.md) | 模块度计算 |
| [`c03-graph-pruning.md`](c03-graph-pruning.md) | 图剪枝与稳定大组件 |

### D-查询（7 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-query-base.md`](d01-query-base.md) | 查询引擎契约与适配 |
| [`d02-local-search.md`](d02-local-search.md) | 局部检索 |
| [`d03-global-search.md`](d03-global-search.md) | 全局检索与社区报告 |
| [`d04-drift-search.md`](d04-drift-search.md) | DRIFT 检索 |
| [`d05-basic-search.md`](d05-basic-search.md) | 朴素向量检索 |
| [`d06-context-building.md`](d06-context-building.md) | 上下文构建与动态社区选择 |
| [`d07-conversation-history.md`](d07-conversation-history.md) | 对话历史与问题生成 |

### E-LLM 层（5 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-llm-completion.md`](e01-llm-completion.md) | 补全抽象与工厂 |
| [`e02-middleware.md`](e02-middleware.md) | 中间件管道 |
| [`e03-rate-limit-and-retry.md`](e03-rate-limit-and-retry.md) | 限流与重试 |
| [`e04-cache-key-and-templating.md`](e04-cache-key-and-templating.md) | 缓存键与模板引擎 |
| [`e05-threading-and-metrics.md`](e05-threading-and-metrics.md) | 线程化执行与指标聚合 |

### F-独立子包（4 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-cache-storage.md`](f01-cache-storage.md) | 缓存实现与键 |
| [`f02-storage-abstraction.md`](f02-storage-abstraction.md) | 存储抽象与表提供方 |
| [`f03-vector-store.md`](f03-vector-store.md) | 向量存储抽象 |
| [`f04-input-and-chunking.md`](f04-input-and-chunking.md) | 输入读取与分块器 |

### G-调用面与回调（2 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-api-and-cli.md`](g01-api-and-cli.md) | API 与命令行调用面 |
| [`g02-callbacks-and-profiling.md`](g02-callbacks-and-profiling.md) | 回调与性能剖析 |

## 关键取舍

**源码事实：** 索引与查询彻底分离。索引产出六类产物表（文档、文本单元、实体、关系、社区、社区报告），查询只读这些表。

**源码事实：** 层次 Leiden 的 `max_cluster_size`（默认 10）是**软上限**——超过就继续分裂一层，这是「层次」的来源，不是跑多次不同分辨率。

**源码事实：** 边在进入聚类前被规范化成无向形式（两端排序）并 `drop_duplicates(keep="last")`，注释说明这是为复刻 NetworkX 保留最后一条边属性的语义；边列表还被 `sorted` 过，因为 Leiden 对输入顺序敏感。

**源码事实：** `seed` 默认 `0xDEADBEEF`，写死在函数签名里而非配置里——可复现是默认要求。

**源码事实：** 图剪枝按度数分布的「均值 + N 倍标准差」剪掉高度数节点，理由是枢纽节点会让聚类把所有东西归到一个社区。

**源码事实：** `graphrag-llm` 用中间件装饰器组合（缓存、限流、重试、指标、日志、请求计数）包裹补全，而不是责任链类。

**架构推论：** 「检索社区报告而不是文本块」是 graphrag 的核心贡献，它把「全局性问题」从不可解变成可解。代价是**索引成本与增量更新难度的双重上升**：每个社区一次 LLM 调用，且新文档可能改变整个图结构。这个取舍的分界线是**问题类型**——问具体事实用局部检索更便宜，问全局主题才值得付这个成本。

**架构推论：** 用「软上限触发分裂」构造层次而不是多次独立运行，关键好处是**嵌套一致性**：上层簇天然是下层簇的并集。多次独立运行会产生互不嵌套的划分，跨层引用就失去意义。

**架构推论：** 排序 + 固定 seed 说明作者把「同一份文档重跑应得到同一张图」当成硬要求。这在知识图谱构建里不是洁癖：否则增量更新会在旧图上叠加不一致的划分。代价是放弃并行化与随机优化的收益。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `packages/graphrag/graphrag/config/models/graph_rag_config.py` | `GraphRagConfig` | 顶层配置结构 |
| `packages/graphrag/graphrag/data_model/entity.py` | `Entity` | 实体产物模型 |
| `packages/graphrag/graphrag/index/workflows/create_base_text_units.py` | `create_base_text_units` | 文档切块 |
| `packages/graphrag/graphrag/index/workflows/extract_graph.py` | `extract_graph` | 实体关系抽取 |
| `packages/graphrag/graphrag/index/workflows/create_community_reports.py` | `create_community_reports` | 社区报告生成 |
| `packages/graphrag/graphrag/graphs/hierarchical_leiden.py` | `hierarchical_leiden` | 层次社区划分 |
| `packages/graphrag/graphrag/index/run/run_pipeline.py` | `run_pipeline` | 流水线执行 |
| `packages/graphrag/graphrag/query/structured_search/local_search/search.py` | `LocalSearch` | 局部检索 |
| `packages/graphrag/graphrag/query/structured_search/global_search/search.py` | `GlobalSearch` | 全局检索 |
| `packages/graphrag/graphrag/query/structured_search/drift_search/search.py` | `DRIFTSearch` | DRIFT 检索 |
| `packages/graphrag-llm/graphrag_llm/completion/completion.py` | `LLMCompletion` | 补全抽象 |
| `packages/graphrag-cache/graphrag_cache/cache.py` | `Cache` | 缓存抽象 |
| `packages/graphrag/graphrag/api/index.py` | `build_index` | 建索引入口 |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 官方文档](https://microsoft.github.io/graphrag/)
- [GraphRAG 论文（Edge et al., 2024）](https://arxiv.org/abs/2404.16130)
- [Leiden 算法论文（Traag et al., 2019）](https://www.nature.com/articles/s41598-019-41695-z)
- [graphrag 文档：查询模式](https://microsoft.github.io/graphrag/query/overview/)

## 与主核 / AI 的关系

graphrag 与主核（多模态数据库内核）的关系是「**图算法 + 物化视图**」这一对概念的组合。

**社区报告本质上是一张物化视图。** 它把「从实体关系图里总结主题」这个昂贵的计算**预先算好并存下来**，查询时直接读。这与数据库的物化视图完全同构，包括同样的问题：**基表变化后视图就过期了**。数据库用 `REFRESH MATERIALIZED VIEW` 或者增量维护来解决；graphrag 用「全量重跑」或者「增量更新链路」（B 组的 `update_*` 工作流）来解决。**「增量维护物化视图」是数据库里出了名的难题**，graphrag 啃的是同一个硬骨头。

**层次社区 vs B-tree 的多级。** 两者都在给数据建跳表式的抽象层，但依据不同：B-tree 按键的全序切（有客观答案），社区按连接强度切（同一张图在不同分辨率下都算「正确划分」）。**这是确定性索引与非确定性索引的分野**——数据库索引可证明等价，社区划分只能靠质量指标（模块度）评价。

**四个层级的模块度 vs 数据库的统计信息。** graphrag 分别计算根层、叶层、整图、最大连通分量的模块度，用途是**评价层次划分的质量**。数据库的 `ANALYZE` 收集列统计信息，用途是**让优化器估代价**。两者都是「为上层决策提供量化依据的元数据」，差别是一个评价结果质量、一个预测执行成本。

**排序 + 固定 seed 保证可复现，与数据库的确定性重放同构。** 差别是门槛：数据库的确定性要针对任意并发交错，graphrag 只要针对单次批处理——所以一个 `sorted()` 就够。

对 agent 工程的直接价值有三条。其一：**「全局性问题」在纯 RAG 里无解**，因为没有任何文本块包含全局视角；先聚类再总结是唯一可行路径。其二：**层次结构让查询可以选择抽象层级**，这与人类回答「具体哪家公司」和「这个行业怎么样」用不同粒度的知识是同一件事。其三：**物化视图的老问题会以「增量更新」的形式重新出现**——任何预计算方案都要面对「基数据变了怎么办」，这是架构层面无法回避的成本。
