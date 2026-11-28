---
project: lightrag
page_type: overview
display_name: LightRAG
repo: https://github.com/HKUDS/LightRAG
license: MIT
language: Python
stars: 40808
stars_as_of: 2026-11-22
refs:
  - path: lightrag/base.py
    symbols: [StorageNameSpace, BaseKVStorage, BaseGraphStorage]
  - path: lightrag/namespace.py
    symbols: [NameSpace]
  - path: lightrag/operate.py
    symbols: [extract_entities, kg_query]
  - path: lightrag/chunker/registry.py
    symbols: [ChunkerSpec, register_chunker]
  - path: lightrag/lightrag.py
    symbols: [LightRAG, ainsert]
---
# LightRAG

## 一句话职责

把文档变成知识图谱再基于图谱回答问题的 RAG 框架：它从文本块里抽出实体与关系、合并去重成一张带权图，查询时先用关键词命中图上的实体与关系、再顺着图取回原文块，最后拼成上下文交给模型。

## 机制是什么

这个项目与 2C 前三个框架的区别在**重心**上，这一点决定了该看什么。

langchain、llama-index、haystack 都是「通用编排骨架 + 大量可替换组件」，机制分散在组件契约上。**lightrag 是单一产品的完整实现**：它把「文档进 → 图出来 → 答案出去」这一整条链写透，包括图谱抽取与合并、实体关系去重、上下文预算裁剪、四个存储后端的一致性处理、多模态 sidecar、配置分片与启动校验。

因此它的体量集中得反常：`lightrag.py` 9052 行、`utils.py` 7836 行、`pipeline.py` 7777 行、`operate.py` 7199 行。机制密度远高于组件数量。

**它最值得读的一条主线是「跨四个存储的一致性靠契约而不是事务」。** 一份文档拆成记录写进四个互相独立的存储（文本块进键值、嵌入进向量、图结构进图、进度进文档状态），四处写入之间没有任何原子性。整套契约因此要承担两件平时由事务承担的事：让残留在已知位置、让「不知道」与「没有」可区分。这条主线贯穿 A、F、G 三组，也是它比其他 RAG 框架的存储抽象厚得多的原因。

本文库按「数据从进来到出去」的顺序组织：**10 个机制领域、共 42 篇机制页**。

## 怎么实现的

下表的 10 个领域按下列顺序排列：A 抽象与存储契约 → B 解析与路由 → C 分块 → D 图谱抽取与合并 → E 查询与上下文 → F 流水线调度 → G 存储实现 → H 多模态与表格 → I 配置与安全 → J 评测与重排。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：`lightrag/` 包本体（196 个 py 文件）。`lightrag_webui/`（React 前端）、`tests/`、`examples/`、`reproduce/`、`scripts/` 不建页。存储后端（milvus / mongo / neo4j / faiss / nano-vectordb / networkx / memgraph / postgres / qdrant / redis / opensearch）属于同一契约的多个实现，归并成两页而不是每个后端一页。

## 机制导航（42 篇 / 10 组）

### A-抽象与存储契约（3 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-storage-contracts.md`](a01-storage-contracts.md) | 四类存储契约与命名空间 |
| [`a02-doc-status.md`](a02-doc-status.md) | 文档状态与游标 |
| [`a03-query-param.md`](a03-query-param.md) | 查询参数与基础类型 |

### B-解析与路由（3 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-parser-base.md`](b01-parser-base.md) | 解析器契约与原生解析基类 |
| [`b02-parser-registry.md`](b02-parser-registry.md) | 解析引擎注册与插件 |
| [`b03-parser-routing.md`](b03-parser-routing.md) | 解析路由与选项校验 |

### C-分块（5 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-chunker-registry.md`](c01-chunker-registry.md) | 分块器注册与上下文传递 |
| [`c02-recursive-character.md`](c02-recursive-character.md) | 递归字符分块与分隔符归一 |
| [`c03-token-window.md`](c03-token-window.md) | token 窗口分块 |
| [`c04-semantic-chunking.md`](c04-semantic-chunking.md) | 语义分块 |
| [`c05-chunk-schema.md`](c05-chunk-schema.md) | 块结构与标题上下文 |

### D-图谱抽取与合并（5 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-entity-extraction.md`](d01-entity-extraction.md) | 实体关系抽取 |
| [`d02-node-edge-merge.md`](d02-node-edge-merge.md) | 节点与边的合并去重 |
| [`d03-description-summary.md`](d03-description-summary.md) | 描述摘要与合并策略 |
| [`d04-graph-name-rules.md`](d04-graph-name-rules.md) | 图谱名称归一与拒绝规则 |
| [`d05-kg-rebuild.md`](d05-kg-rebuild.md) | 图谱重建与抽取缓存 |

### E-查询与上下文（5 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-kg-query.md`](e01-kg-query.md) | 图谱查询主流程 |
| [`e02-keyword-extraction.md`](e02-keyword-extraction.md) | 查询关键词抽取 |
| [`e03-context-budget.md`](e03-context-budget.md) | 上下文预算与截断 |
| [`e04-context-assembly.md`](e04-context-assembly.md) | 上下文组装与块合并 |
| [`e05-query-validation.md`](e05-query-validation.md) | 查询校验 |

### F-流水线调度（4 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-pipeline-steps.md`](f01-pipeline-steps.md) | 流水线步骤与决策 |
| [`f02-insert-flow.md`](f02-insert-flow.md) | 文档插入流程 |
| [`f03-custom-chunks-kg.md`](f03-custom-chunks-kg.md) | 自定义块与手工图谱写入 |
| [`f04-storage-lifecycle.md`](f04-storage-lifecycle.md) | 存储初始化与收尾 |

### G-存储实现（4 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-json-kv-store.md`](g01-json-kv-store.md) | JSON 键值存储与文档状态实现 |
| [`g02-graph-storage-impl.md`](g02-graph-storage-impl.md) | 图存储实现（内存与外部） |
| [`g03-vector-storage-impl.md`](g03-vector-storage-impl.md) | 向量存储实现 |
| [`g04-embedding-baseline.md`](g04-embedding-baseline.md) | 嵌入空间校验与存储迁移 |

### H-多模态与表格（3 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-multimodal-context.md`](h01-multimodal-context.md) | 多模态上下文与原子化 |
| [`h02-table-markup.md`](h02-table-markup.md) | 表格标记与 HTML 行拆分 |
| [`h03-sidecar.md`](h03-sidecar.md) | sidecar 写入与占位符 |

### I-配置与安全（7 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-config-store.md`](i01-config-store.md) | 配置存储与作用域 |
| [`i02-config-shards.md`](i02-config-shards.md) | 配置分片与工作区发现 |
| [`i03-storage-anchor.md`](i03-storage-anchor.md) | 存储锚点与排他锁 |
| [`i04-private-file.md`](i04-private-file.md) | 私有文件写入与原子替换 |
| [`i05-api-security.md`](i05-api-security.md) | 接口鉴权与准入控制 |
| [`i06-exceptions-and-roles.md`](i06-exceptions-and-roles.md) | 异常体系与多角色模型 |
| [`i07-llm-bindings.md`](i07-llm-bindings.md) | 模型绑定选项与环境解析 |

### J-评测与重排（3 篇）

| 页文件 | 标题 |
|---|---|
| [`j01-rerank.md`](j01-rerank.md) | 重排与分数聚合 |
| [`j02-evaluation.md`](j02-evaluation.md) | RAG 质量评测与离线检索检查 |
| [`j03-metrics-and-prompts.md`](j03-metrics-and-prompts.md) | 指标与提示模板管理 |

## 关键取舍

**源码事实：** 四类存储无跨存储事务。基类提供 `drop_pending_index_ops` / `drop_pending_upserts` / `has_pending_index_ops` 三个「丢弃前必须回答」的方法；`index_done_callback` 要求异常携带「是否丢了持久引用」的分类信息。

**源码事实：** 大量读取有严格版与宽松版（`get_docs_by_statuses(strict=...)`、`get_by_id` 与 `get_by_id_strict`）。严格版把「结果完整」写进契约，因为调度面一旦消费了唤醒信号就不会重读。

**源码事实：** `NameSpace` 的 12 个常量是物理容器名，注释标注「不应改动」；配置存储用独立的固定容器 `_lightrag_config`，注释明确「它不是工作区」。

**源码事实：** 向量存储的容器名编码嵌入模型（`_collection_suffix_for`），并且启动时有探针校验（抽样向量算余弦判断是否同一嵌入空间）。

**源码事实：** 同一个实体被多个文档提到时按描述合并，关系权重有契约 `weight >= 不同真实来源 id 数`；名称归一化只允许合并原本不同的名字，不允许拆分原本相同的名字（否则重建会丢数据）。

**源码事实：** 图谱存储（`NetworkXStorage`）是唯一声明 `requires_single_writer` 的后端，所有管理面写操作因此要经过 `_admin_write_gate`。

**架构推论：** 没有事务时，契约的职责从「保证正确」变成「让错误可定位、可自愈」。这解释了为什么基类里那么多方法在讨论「丢弃之后会怎样」——它们不是在管理数据，是在管理不确定性。

**架构推论：** 「严格读 vs 宽松读」成对出现，是因为控制面与展示面对完整性的要求相反：展示面宁可少显示一条也不能整页报错，调度面宁可报错也不能漏一条。**同一份数据的两个消费者，可靠性方向相反。**

**架构推论：** 用命名做隔离（容器名编码模型、锚点文件声明目录归属）的代价是清理责任转移给调用方，收益是把「向量空间错配」这类难发现的语义错误转成「容器不存在」这类立刻发现的命名错误。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `lightrag/base.py` | `StorageNameSpace` | 四类存储的公共契约 |
| `lightrag/base.py` | `DocStatusStorage` | 文档状态机与严格读契约 |
| `lightrag/namespace.py` | `NameSpace` | 物理命名空间常量 |
| `lightrag/parser/routing.py` | `sanitize_process_options` | 解析引擎与选项解析 |
| `lightrag/chunker/registry.py` | `register_chunker` | 分块器注册与能力探测 |
| `lightrag/chunk_schema.py` | `format_heading_context` | 标题上下文拼装 |
| `lightrag/operate.py` | `extract_entities` | 实体关系抽取（含 gleaning） |
| `lightrag/operate.py` | `merge_nodes_and_edges` | 节点与边合并去重 |
| `lightrag/operate.py` | `kg_query` | 图谱增强查询主流程 |
| `lightrag/pipeline.py` | `PipelineNextDecision` | 流水线步骤决策 |
| `lightrag/lightrag.py` | `LightRAG` | 主类与插入编排 |
| `lightrag/vector_space_gate.py` | `_probe_same_embedding_space` | 嵌入空间探针校验 |
| `lightrag/config_anchor.py` | `StorageAnchor` | 存储锚点与归属判定 |
| `lightrag/kg/networkx_impl.py` | `NetworkXStorage` | 内存图存储实现 |

## 参考

- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)
- [lightrag 论文（arXiv）](https://arxiv.org/abs/2410.05779)
- [lightrag 文档](https://github.com/HKUDS/LightRAG/blob/main/README.md)
- [lightrag 源码：base.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/base.py)
- [lightrag 源码：operate.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/operate.py)

## 与主核 / AI 的关系

lightrag 与主核（多模态数据库内核）的交集比前面三个 RAG 框架都深，因为它**真的在管理持久化状态**，而不只是编排调用。

**无事务的存储抽象 vs WAL 与事务。** 主核有 WAL 与检查点，所以存储接口可以很简单——「写成功」就是成功了，不需要 `has_pending_index_ops` 这种问题。lightrag 没有事务，于是契约必须把「可能没写成功」显式建模，接口因此厚了三四倍。**接口复杂度是可靠性保证的替代品而不是补充品**：保证越弱，契约必须越啰嗦。

**四存储一致性 vs 多表事务。** 数据库用事务让「多表写入要么全成要么全败」；lightrag 用「让残留在已知位置 + 下次运行自愈」。前者是原子性保证，后者是**收敛性保证**——不保证任何时刻一致，只保证没有永久损坏。这在一个「可以重跑整条流水线」的 AI 应用里是可接受的；在一个「用户刚存了钱」的账本系统里不可接受。**这个取舍的分界线是：操作是否幂等可重放。**

**实体名归一化只能合不能分**这条约束，与数据库的索引维护是同类问题：都要求「规则变更不能破坏既有数据的可寻址性」。数据库里对应的是排序规则的修改会导致索引失效；lightrag 里是归一化规则的修改会让存量对象的记录对不上。两者都用同一条解法：**新规则必须是旧规则的粗化（只合并），不能是细化（会拆分）**。

**图谱查询与数据库的图遍历。** lightrag 的 `local` / `global` / `hybrid` 模式与图数据库的邻居扩展、社区发现同构，但它没有查询优化器也没有代价模型——关键词命中哪些实体完全由 LLM 决定。这是「语义驱动检索」与「索引驱动检索」的根本差异：前者不需要统计信息，但结果不可复现；后者可复现，但无法处理「意思相近而字面不同」。

**配置分片与锚点 vs 数据库的实例注册。** `StorageAnchor` 声明「这个目录属于哪个 storage uuid」，与 PostgreSQL 的 `pg_control` 文件声明数据目录身份是同一手法：**让「误用」变成「启动即失败」而不是「运行到一半才出错」**。
