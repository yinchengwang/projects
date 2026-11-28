---
project: dify
feature: dataset-retrieval
title: 知识库检索编排
page_type: feature
schema_version: 1
refs:
  - path: api/core/rag/retrieval/dataset_retrieval.py
    symbols: [DatasetRetrieval]
  - path: api/core/rag/retrieval/retrieval_methods.py
    symbols: [RetrievalMethod]
---

# 知识库检索编排

## 一句话职责

`DatasetRetrieval` 决定「这次提问该去哪个知识库找、用哪种检索方式、要不要先做元数据过滤」——它是检索侧唯一的编排者，把多知识库、单知识库、外部知识库、附件检索这几条路径收敛到同一份产物上。

## 机制是什么

`RetrievalMethod` 是四个取值的枚举，但它的巧思在**两个静态判定函数**：

| 取值 | 语义检索 | 全文检索 |
|---|---|---|
| `SEMANTIC_SEARCH` | 是 | 否 |
| `FULL_TEXT_SEARCH` | 否 | 是 |
| `HYBRID_SEARCH` | 是 | 是 |
| `KEYWORD_SEARCH` | 否 | 否 |

`is_support_semantic_search` 与 `is_support_fulltext_search` 把「四种方法」映射成「两个能力位」。**混合检索在这个模型里不是第三种算法，而是"两个能力都开"**——所以下游可以把「要不要做向量检索」「要不要做全文检索」当成两个独立布尔量来分支，而不必对四种取值各写一遍。`KEYWORD_SEARCH` 两个能力都是假，因为它是经济模式的关键词倒排表，走的是另一条完全不同的实现。

`DatasetRetrieval` 有两级入口：

- **`retrieve`**：老链路，给一份 `DatasetEntity` 配置，走单库或多库策略，返回拼好的上下文字符串与图片列表。
- **`knowledge_retrieval`**：新链路，收一个结构化的 `KnowledgeRetrievalRequest`，返回 `list[Source]`。

两个入口都汇到同一批底层方法（`single_retrieve` / `multiple_retrieve`），说明这是**一次入口重构进行到中途**的状态——新接口换了输入输出的形状，但内核没变。

## 怎么实现的

**元数据过滤是两条链路的共同前置，且它发生在检索之前。** `get_metadata_filter_condition` 返回两个东西：`{dataset_id: [document_id]}` 的映射，以及一个 `MetadataFilteringCondition`。三种模式：

- `disabled` → 返回 `(None, None)`，完全不筛。
- `automatic` → 让 LLM 从「查询 + 可用元数据列表」里推出过滤条件，再转成 SQL 条件。
- `manual`（其他值）→ 直接用配置里给的 `metadata_filtering_conditions`。

返回的 `document_id` 映射会被当**白名单**用：某数据集若在映射里没有条目，或条目是空列表，那个数据集直接被跳过（`continue`）。所以过滤不是「检索后筛选」而是「检索前缩小范围」——这对性能影响很大，因为向量检索的候选集根本不会包含被排除的文档。

**单库检索的本质是「让模型选库」。** `single_retrieve` 先把每个数据集包装成一个 `PromptMessageTool`——**工具名就是数据集 id**，描述取数据集描述（空则用「useful for when you want to answer queries about the 名字」），参数 schema 是空对象。然后按模型能力选路由策略：

- 模型支持 `TOOL_CALL` 或 `MULTI_TOOL_CALL` → `FunctionCallMultiDatasetRouter`
- 否则 → `ReactMultiDatasetRouter`（走 ReAct 文本推理）

这个分流发生在 `retrieve` 里对 `model_schema.features` 的判断上。**它是「工具调用能力」驱动「编排方式」的直接例子**：同一个功能在支持函数调用的模型上走结构化调用，在不支持的模型上靠提示词让模型输出数据集名。

选出的 `dataset_id` 还要**再过一遍库校验**：`next(...)` 找到对应数据集对象，再用 `session.scalar(select(Dataset).where(Dataset.id == ..., Dataset.tenant_id == tenant_id))` 查一次。这一步是防「模型返回了一个我们不认识的 id」——**模型输出被当成不可信输入处理**。

**多库检索是「一个数据集一个线程」，并且明确区分了容错级别。** `multiple_retrieve` 先做两组一致性校验：

1. 所有知识库的 `indexing_technique` 必须一致；不一致时**必须有重排模型**，否则抛 `ValueError("...please set reranking model.")`。
2. 高质量模式下若用 `weighted_score` 重排，所有库的嵌入模型与 provider 必须一致，否则同样抛错并要求改用重排模型。

第 2 条的根因很实际：`weighted_score` 会把向量分与关键词分加权求和，而不同嵌入模型的向量分数**不在同一量纲上**，加权毫无意义。所以代码还把第一个库的嵌入模型名与 provider 写回 `weights["vector_setting"]`，让后续重排知道用谁的模型。

线程层面有一层套一层的设计：`multiple_retrieve` 起一个「查询线程」加若干「附件线程」，每个线程内部 `_multiple_retrieve_thread` 又为**每个数据集**起一个线程。所以并发度是「查询/附件数 × 数据集数」。

**两种线程用不同的安全包装，容错语义相反。** `_run_retriever_thread_safely` 与 `_multiple_retrieve_thread_safely` 都存在，但内部那个（按数据集）传 `skip_on_error=True`：

```python
if skip_on_error:
    logger.warning("Skipping dataset retrieval because retriever failed, dataset_id=%s, ...")
    span.add_event("dataset_retrieval.skipped", attributes={...})
    return
```

**单个数据集失败被降级成「跳过 + 记录 span 事件」，不中断整次检索**。这对外层传 `skip_on_error=False` 的路径（附件线程）则相反：失败会 `cancel_event.set()` 并把异常塞进共享列表，最终由主线程 `raise thread_exceptions[0]`。

所以同一次多库检索里，**查询线程的失败是致命的、单个数据集检索的失败是可跳过的**。这个分级是合理的：某个库挂了不该让整个问答失败，但附件通道挂了说明请求本身有问题。

**失败快速传播靠「短超时轮询」。** 外层用 `while any(t.is_alive() ...)` 每 0.1 秒 `join` 一次，一旦 `thread_exceptions` 非空就 `cancel_event.set()` 并 `break`。这样不用等所有线程跑完就能抛错。内层同理，只是响应的是 `cancel_event`。

`cancel_event` 的存在让「一个数据集出错 → 通知其它数据集线程提前退出」成为可能，减少无谓的检索开销。

**重排只做一次，且在合并后做。** `_multiple_retrieve_thread` 拿到本通道的所有文档后分三种情况处理：

- `reranking_enable` 且 `dataset_count > 1` → 走 `DataPostProcessor`（真正重排）；
- 经济模式 → `calculate_keyword_score`（本地 TF-IDF）；
- 高质量模式 → `calculate_vector_score`（阈值过滤 + 排序）。

**注释点明「只有一库时跳过二次重排」**——因为单库检索内部已经重排过一次，再排一遍是重复计费。这条优化藏在 `dataset_count > 1` 这个条件里。

**`calculate_keyword_score` 是自己实现的一套 TF-IDF。** 步骤是：用 jieba 抽查询关键词与每个文档的关键词 → 用整个候选集算每个词的 IDF（`log((1+N)/(1+df)) + 1`，加一平滑）→ 查询与文档各自算 TF-IDF 向量 → 余弦相似度 → 按分数排序取 top_k。文档关键词会被写回 `metadata["keywords"]`，这个字段后续可能被别处用到。

这是一段**完全本地、不调模型、不用向量库**的打分，为经济模式（无向量索引）提供排序能力。它的 IDF 只在本次候选集内统计，所以**同一文档在不同查询下拿到的分数不可比**。

**`calculate_vector_score` 只做阈值过滤与排序**，分数直接来自向量库返回的 `metadata["score"]`。它有一处细节：`score_threshold is None` 时**不过滤**，但传 `0.0` 时会过滤掉所有负分——**`None` 与 `0` 在这里语义不同**，这与配置里 `score_threshold_enabled` 开关有关。

**`_on_retrieval_end` 带数据库死锁重试。** 它负责回写命中次数（`hit_count`）等统计，装饰器指定「只对 Postgres 死锁错误重试」，退避是 0.05 到 0.1 秒的指数增长。这说明**回写统计会与并发检索事务撞车**，而这类冲突可以靠短暂重试解决。

**`knowledge_retrieval` 入口把模型校验做得更细。** 它按 `ModelStatus` 分四种情况抛不同异常：不存在、凭据未初始化、无权限、配额超限。而 `retrieve` 老入口只用 `get_model_schema` 是否为空来判断。**新入口的错误颗粒度明显更细**——这是新老接口差异的又一处证据。

## 关键取舍

**源码事实：** `RetrievalMethod` 的四个取值可用两个静态方法映射成「语义检索能力」与「全文检索能力」两个布尔位，`HYBRID_SEARCH` 是两者都为真。

**源码事实：** 元数据过滤在检索之前执行，产出 `{dataset_id: [document_id]}` 白名单；映射里没有该数据集或列表为空即跳过该库。

**源码事实：** 单库检索把每个知识库包装成一个工具（工具名 = 数据集 id），按模型是否支持 `TOOL_CALL`/`MULTI_TOOL_CALL` 选函数调用路由或 ReAct 路由；选出的 id 会再被数据库校验一次。

**源码事实：** 多库检索要求所有库的索引技术一致；不一致或嵌入模型不一致时必须启用重排模型，否则抛 `ValueError`。`weighted_score` 时会把第一个库的嵌入模型写回权重配置。

**源码事实：** 单个数据集检索失败在 `skip_on_error=True` 时降级为「跳过并记录 span 事件」，不中断整体；附件通道的失败则会取消全部并上抛。

**源码事实：** 重排在 `dataset_count > 1` 时才做第二遍，单库跳过。

**源码事实：** `_on_retrieval_end` 对 Postgres 死锁做有限次指数退避重试。

**架构推论：** 「用一个布尔对而不是四个枚举值来分支」是个小但有效的设计。四种检索方法里，语义与全文是两个正交能力，混合就是并集，关键词是两者都不走。把它表达成能力查询后，下游不必 `if method in (SEMANTIC, HYBRID)` 这样逐个列举，也不会漏掉将来新增的组合。**枚举给用户看，能力位给代码用**，这是枚举设计的一个好范式。

**架构推论：** 「过滤在检索前」与「过滤在检索后」的差别不只是性能。检索前过滤意味着被排除的文档**根本不参与相似度竞争**，所以 top_k 的候选质量更高；检索后过滤则会让被排除文档占掉候选名额，然后在最后被丢掉，等效于降低了有效候选数。代码选了前者，代价是它必须依赖 `document_id` 白名单这种较粗的粒度——无法表达「按任意元数据字段在向量库内部过滤」这类后端能力。

**架构推论：** 「模型选库」这条路径把 LLM 输出的 `dataset_id` 当不可信输入，再查库校验一次。这是正确的边界处理：模型可能幻觉出不存在的 id，也可能给出别的租户的 id。校验里的 `Dataset.tenant_id == tenant_id` 是关键——**没有它就是一个越权读取漏洞**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/rag/retrieval/retrieval_methods.py` | `RetrievalMethod` | 四种检索方法，附两个能力判定 |
| `api/core/rag/retrieval/retrieval_methods.py` | `RetrievalMethod.is_support_semantic_search` | 语义能力位（语义 + 混合） |
| `api/core/rag/retrieval/retrieval_methods.py` | `RetrievalMethod.is_support_fulltext_search` | 全文能力位（全文 + 混合） |
| `api/core/rag/retrieval/dataset_retrieval.py` | `DatasetRetrieval` | 检索编排主体 |
| `api/core/rag/retrieval/dataset_retrieval.py` | `DatasetRetrieval.retrieve` | 老入口：配置驱动，返回上下文与图片 |
| `api/core/rag/retrieval/dataset_retrieval.py` | `DatasetRetrieval.knowledge_retrieval` | 新入口：请求对象驱动，返回 `Source` 列表 |
| `api/core/rag/retrieval/dataset_retrieval.py` | `DatasetRetrieval.single_retrieve` | 模型选库 + 单库检索 |
| `api/core/rag/retrieval/dataset_retrieval.py` | `DatasetRetrieval.multiple_retrieve` | 多库并发 + 一致性校验 + 合并重排 |
| `api/core/rag/retrieval/dataset_retrieval.py` | `DatasetRetrieval._multiple_retrieve_thread` | 逐数据集起线程、按索引技术选打分方式 |
| `api/core/rag/retrieval/dataset_retrieval.py` | `DatasetRetrieval._run_retriever_thread_safely` | 数据集级容错：跳过或上抛 |
| `api/core/rag/retrieval/dataset_retrieval.py` | `DatasetRetrieval.calculate_keyword_score` | 本地 TF-IDF 打分（经济模式） |
| `api/core/rag/retrieval/dataset_retrieval.py` | `DatasetRetrieval.calculate_vector_score` | 阈值过滤 + 排序（高质量模式） |
| `api/core/rag/retrieval/dataset_retrieval.py` | `DatasetRetrieval.get_metadata_filter_condition` | 三种元数据过滤模式的前置计算 |
| `api/core/rag/retrieval/dataset_retrieval.py` | `DatasetRetrieval._on_retrieval_end` | 命中统计回写，带死锁重试 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/rag/retrieval/dataset_retrieval.py](https://github.com/langgenius/dify/blob/main/api/core/rag/retrieval/dataset_retrieval.py)
- [dify 文档：知识库检索设置](https://docs.dify.ai/guides/knowledge-base/create-knowledge-and-upload-documents)
- [论文：混合检索与重排（RRF 与交叉编码器）](https://arxiv.org/abs/2210.11934)

## 与主核 / AI 的关系

「按能力位组合检索策略」与主核里「按访问方法支持的操作选择执行计划」是同一种规划思路：先问「这个执行者支持什么」，再决定「怎么组合它」。主核的代价模型算 I/O 成本，这里的校验算语义一致性（嵌入模型是否同一量纲）。

对 RAG 工程最实用的四条。其一：**多库检索要求索引技术一致**，混用经济库与高质量库必须配重排模型，否则直接抛错——这是有意的强约束，因为两种库的分数不可比。其二：**`weighted_score` 重排要求所有库用同一嵌入模型**，否则加权毫无意义；代码会把第一个库的模型写进权重配置，所以「不一致」的情况实际会在校验阶段被拦。其三：**单库检索是「模型选库」**，模型返回的 id 会被再校验一次（含租户校验）；自己实现类似功能时这一步不能省。其四：**元数据过滤发生在检索前**，配了过滤条件后某些库可能整库被跳过——如果发现"某个库明明有内容却检索不到"，先检查元数据过滤是否把它排除了。
