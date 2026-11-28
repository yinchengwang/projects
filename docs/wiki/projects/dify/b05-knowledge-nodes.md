---
project: dify
feature: knowledge-nodes
title: 知识检索与知识索引节点
page_type: feature
schema_version: 1
refs:
  - path: api/core/workflow/nodes/knowledge_retrieval/knowledge_retrieval_node.py
    symbols: [KnowledgeRetrievalNode]
  - path: api/core/workflow/nodes/knowledge_retrieval/retrieval.py
    symbols: [KnowledgeRetrievalRequest, RAGRetrievalProtocol]
  - path: api/core/workflow/nodes/knowledge_index/knowledge_index_node.py
    symbols: [KnowledgeIndexNode]
---

# 知识检索与知识索引节点

## 一句话职责

两个方向相反的 RAG 节点：检索节点把「查询 + 附件 + 元数据过滤」整理成一个请求对象交给检索服务；索引节点把「分块 → 索引 → 摘要向量化」串起来，并在调试模式下走一条只会生成预览、不落库的分支。

## 机制是什么

知识库里两个动作的方向正好相反：**写进去**（索引）与**读出来**（检索）。它们被做成两个独立节点，不是同一个节点的两种模式，因为触发时机完全不同——索引发生在数据摄取流水线里，检索发生在问答流水线里。

两个节点共享一个特征：**它们本身没有 RAG 逻辑，只有参数整理与结果整形**。真正的检索在 `DatasetRetrieval`，真正的索引在 `IndexProcessor`，摘要向量化在 `SummaryIndex`。节点做的三件事是：从变量池取值、按 DSL 配置拼装请求/参数、把结果包成引擎认识的输出段类型。

这个分工的原因在 b01 讲过：本仓不做执行内核，节点就该薄。**当节点开始理解业务细节时，说明职责边界滑了。**

两个节点各有一处「模式分叉」，而这两个分叉是这一篇真正的主题：检索节点按 `retrieval_mode`（单库/多库）分出两套参数装配；索引节点按 `invoke_from`（调试器/其它）分出「只生成预览」与「真落库 + 触发摘要」两条执行路径。

## 怎么实现的

**检索节点：先短路，再取值，再按模式分派。**

`_run` 的第一件事是一个早退：若「查询变量 selector」与「查询附件 selector」都为空，**直接返回一个全空的成功结果**，而不是报错。注释没解释理由，但从行为看这是有意的：一个没配查询的检索节点被视为「无事可做」，而不是「配置错误」。这个选择的代价是——工作流里留了一个没接线的检索节点，跑起来毫无提示。

取值有两个来源，各自带严格类型校验：查询必须是 `StringSegment`（否则失败，错误信息是「Query variable is not string type.」）；附件必须是 `ArrayFileSegment` 或 `FileSegment`（否则失败）。单文件会被**包成单元素列表**，于是下游只处理一种形态。

**模式分派的两套参数差异值得对照。**

单库模式（`SINGLE`）需要模型配置，它把 `single_retrieval_config.model` 的四个字段（provider、name、mode、completion_params）拆平塞进请求——这个模型是给「自动元数据过滤」用的，也就是让 LLM 从查询里推断过滤条件。

单库模式还隐含一个条件：**必须有 query**。代码里写的是 `SINGLE and query`，所以单库模式 + 只有附件时**什么也不做**，`retrieval_resource_list` 保持空列表，节点仍然返回成功且输出空数组。这是第二个静默分支。

多库模式（`MULTIPLE`）参数多得多：`top_k`、`score_threshold`（`None` 时补 0.0）、重排模式、重排模型或权重、`reranking_enable`、附件 ID 列表。重排配置用 `match` 分三支：`reranking_model` 模式要求重排模型存在（不存在则给 `None`）、`weighted_score` 模式要求权重存在（不存在直接抛 `ValueError`）、其它值一律退化成「都是 None」。

注意这两支的**失败处理不对称**：`reranking_model` 模式下模型缺失是允许的（`None`），`weighted_score` 模式下权重缺失是致命的（抛错）。前者可以被理解成「关了重排」，后者无法理解成任何有效配置。

**附件到检索的转换只取 record_id。** 多库模式里有一段推导式：对每个附件调 `parse_file_reference(attachment.reference)`，解析成功就取 `record_id`，解析失败（返回 `None`）的附件被静默丢弃。所以**引用形态不合规的附件不会报错，只会不参与检索**。这与同文件其它地方的严格类型校验形成对比——那里的错会变成节点失败，这里的错会变成「少了一个附件」。

**沿用老代码的位置有一句诚实的注释。** 最后一个 `except Exception` 上方写着「Temporary handle all exceptions from DatasetRetrieval class here.」——`Temporary` 这个词已经留在这里，而它旁边的分支（`RateLimitExceededError`）已经有了自己的专门处理。所以「临时」的兜底有了永久化的迹象：限流被单独拎出来之后，兜底分支并没有被删掉。

**元数据过滤条件是「模板解析 + 标量归一」两段。** `_resolve_metadata_filtering_conditions` 遍历每个条件，看它的 `value`：

- 字符串 → 用 `convert_template` 在变量池里解析，若解析出一个段就取它的对象值并归一成标量，若解析出多个段就**退化成整串文本**；
- 字符串序列 → 逐个按同样规则解析，结果仍是列表；
- 其它 → 原样保留。

「一个段就取对象、多个段就取文本」这条规则很关键：只有一个变量时能保留类型（数字还是数字），一旦是拼接串就只能是字符串。**这是模板系统对类型信息的一次不可避免的丢失**，代码选择在这里显式处理，而不是把字符串硬塞给下游。

标量归一函数只保留 `str`、`float`、非布尔 `int` 与 `None`，其余一律 `str()`。**布尔被专门排除**在整数之外——Python 里 `True` 是 `int` 的实例，不排除的话过滤器会收到 `1`。

**输出整形用 `ArrayObjectSegment`。** 检索结果是一组 `Source` 模型，节点把它们逐个 `model_dump(by_alias=True)` 后塞进对象数组段。`by_alias=True` 是为序列化别名服务的：`SourceMetadata` 里 `source` 字段的序列化别名是 `_source`，加下划线前缀是为了避开前端/用户元数据里的同名键。

`Source` 与 `SourceMetadata` 的设计也值得看一眼：`SourceMetadata` 的字段几乎都有默认值，包括 `retriever_from` 默认 `"workflow"`、`segment_hit_count` 默认 0、`position` 默认 0。**这些默认值让「检索结果」成了一个宽容的结构**——不同检索后端给的字段不一样，缺失的用默认填。代价是「缺失」与「真的是 0」不可区分。

**`RAGRetrievalProtocol` 是一份与实现脱节的协议。** 它声明了三个成员：`llm_usage` 属性、`set_request_metadata`、以及 `knowledge_retrieval(request)`。但全仓**没有任何地方引用它**，而且它的 `knowledge_retrieval` 签名只有 `request` 一个参数，与实际的 `DatasetRetrieval.knowledge_retrieval(session, request)` 不一致——真实现需要一个数据库会话，协议里没有。

所以这个 Protocol 既没被用于类型检查，也不符合真实签名。它是一个**曾经打算做接口、后来实现走偏、也没有清理的残留**。节点里 `self._rag_retrieval` 的类型是直接靠构造函数赋值推断的，不走任何协议。

**索引节点：两种模式的执行路径差异。**

`execution_type = NodeExecutionType.RESPONSE` 在类层面声明了它的角色——它位于响应路径上（索引通常出现在摄取流水线的末端）。

`_run` 先取三样系统变量：`DATASET_ID`（必须有，否则抛错）、`DOCUMENT_ID`（可空）、以及 `INVOKE_FROM`。然后用 `invoke_from == "debugger"` 判断是不是预览模式。

**预览模式的分支极短**：开一个会话，调 `index_processor.get_preview_output(...)`，把返回模型 `model_dump(exclude_none=True)` 后原样作为输出，返回。**它不写数据库、不生成摘要、不需要 batch、不需要原始文档 ID。** 这正是「画布上点一下预览」该有的行为。

**正式模式要多两样系统变量**：`ORIGINAL_DOCUMENT_ID`（可空）与 `BATCH`（必须有，否则抛错），然后调 `_invoke_knowledge_index`。

`_invoke_knowledge_index` 里有一步容易被忽略但很重要：**`index_and_clean` 之后立即 `session.commit()`**，然后才调摘要生成。注释写明理由：「Summary generation opens independent sessions and must see the indexed rows.」——摘要生成自己开会话，如果不先提交，它看不到刚索引的行。

这是一处**跨会话可见性**的隐式依赖。它不出现在任何接口签名里，只体现在这一行 commit 与那句注释里。如果哪天有人把摘要在同一会话内联，这一行 commit 就会变成多余；反之如果摘要在事务外被当成「读到旧数据」而改成延迟执行，行为就会静默变化。

同一段还包了一层 `use_credit_usage_metadata(request_metadata)` 上下文，把 app_type 传下去供计费使用。这说明索引过程可能消耗模型调用（摘要生成要调模型），需要归到正确的应用类型上。

**摘要设置从节点数据取，不吃数据集配置。** 注释在预览分支里解释了：`summary_index_setting` 来自 `node_data`（工作流图配置），取不到才回退到数据集。这个优先级是对的——工作流节点显式配置的参数应当赢过数据集级默认，否则「在这个节点上关掉摘要」就做不到。

**输出形态是「原样透传」。** 正式模式把 `index_and_clean` 的返回值直接当 `outputs`，不做任何整形；预览模式则 `model_dump` 一个模型。两者都是「返回底层给什么」，与检索节点把结果包成段类型的做法不同。这个不一致反映了两者下游消费者的差异：索引的输出通常不再被后续节点按变量引用，只是记录；检索的输出要进变量池供下游节点选择。

## 关键取舍

**源码事实：** 检索节点在「查询与附件 selector 都为空」时返回空成功结果，不报错。

**源码事实：** 单库模式的条件是 `retrieval_mode == "single" and query`，因此单库 + 仅附件时不执行检索，返回空结果。

**源码事实：** 多库模式下 `reranking_model` 缺失允许（`None`），`weighted_score` 模式下权重缺失直接抛 `ValueError`。

**源码事实：** 附件转 `attachment_ids` 时解析失败的引用被静默跳过，不报错。

**源码事实：** 元数据过滤条件的模板解析遵循「单段取对象值、多段退化取文本」；标量归一排除布尔类型。

**源码事实：** 检索输出被包成 `ArrayObjectSegment`，`Source` 用 `by_alias=True` 序列化以产出 `_source` 键。

**源码事实：** `SourceMetadata` 的多数字段有默认值（`retriever_from` 默认 `"workflow"`、计数字段默认 0），使结构对缺失字段宽容。

**源码事实：** `RAGRetrievalProtocol` 全仓无引用，且其 `knowledge_retrieval` 签名与实际实现的 `(session, request)` 不一致。

**源码事实：** 索引节点用 `INVOKE_FROM == "debugger"` 区分预览与正式模式；预览不需要 batch 与原始文档 ID。

**源码事实：** 正式模式在 `index_and_clean` 之后立即 `session.commit()`，因为摘要生成要开独立会话才能看到新索引的行。

**架构推论：** 「空输入 → 空成功」与「空输入 → 失败」的选择，本质是**在问「这个节点是可选的还是必需的」**。检索节点允许无查询，是因为它在画布上可能先被拖出来还没接线；索引节点要求 chunks 非空，因为它在一个必然有数据的流水线里。同一份代码里两种取向并存，说明这取决于节点在流水线中的位置，而**位置信息只存在于产品设计里，不在代码里**。

**架构推论：** 静默丢弃（附件引用解析失败）与严格校验（查询类型不符就失败）并存，暴露了「容错粒度」的判断：**影响主链路的关键参数必须严，可选的补充数据可以宽**。这个判断是对的，但代价是排查困难——「为什么检索结果里没有这个附件」不会有任何日志。

**架构推论：** 索引节点的 `commit` 是一处**隐式契约**。它保证了后续组件能看到数据，但没有类型或接口层面的表达。这类「靠注释维持的时序依赖」在多组件协作的系统里是最常见的故障源：改动者不知道这一行的作用，删掉后错在别处。

**架构推论：** 未使用的 Protocol 是一份「设计意图的化石」。它记录了作者曾想把检索服务抽象成接口，而现实是需要 session 参数、抽象失败了。**留下一个不符合实现且无人使用的协议，比删掉它更糟**——它会误导后续读代码的人以为这层抽象存在。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/workflow/nodes/knowledge_retrieval/knowledge_retrieval_node.py` | `KnowledgeRetrievalNode` | 检索节点主体，含两处静默空分支 |
| `api/core/workflow/nodes/knowledge_retrieval/knowledge_retrieval_node.py` | `KnowledgeRetrievalNode._run` | 取值、类型校验、模式分派、异常分类 |
| `api/core/workflow/nodes/knowledge_retrieval/knowledge_retrieval_node.py` | `KnowledgeRetrievalNode._fetch_dataset_retriever` | 单库/多库两套请求装配 |
| `api/core/workflow/nodes/knowledge_retrieval/knowledge_retrieval_node.py` | `KnowledgeRetrievalNode._resolve_metadata_filtering_conditions` | 模板解析与标量归一 |
| `api/core/workflow/nodes/knowledge_retrieval/retrieval.py` | `KnowledgeRetrievalRequest` | 检索请求的扁平参数对象 |
| `api/core/workflow/nodes/knowledge_retrieval/retrieval.py` | `Source` | 检索结果条目，含元数据与内容 |
| `api/core/workflow/nodes/knowledge_retrieval/retrieval.py` | `SourceMetadata` | 结果元数据，带 `_source` 序列化别名 |
| `api/core/workflow/nodes/knowledge_retrieval/retrieval.py` | `RAGRetrievalProtocol` | 无引用且签名与实现不符的残留协议 |
| `api/core/workflow/nodes/knowledge_index/knowledge_index_node.py` | `KnowledgeIndexNode` | 索引节点主体，响应型节点 |
| `api/core/workflow/nodes/knowledge_index/knowledge_index_node.py` | `KnowledgeIndexNode._run` | 预览与正式两条路径的分叉 |
| `api/core/workflow/nodes/knowledge_index/knowledge_index_node.py` | `KnowledgeIndexNode._invoke_knowledge_index` | 索引、提交、摘要向量化的时序 |
| `api/core/rag/retrieval/dataset_retrieval.py` | `DatasetRetrieval.knowledge_retrieval` | 真实检索实现，需要数据库会话 |
| `api/core/rag/index_processor/index_processor.py` | `IndexProcessor` | 索引与预览输出 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：nodes/knowledge_retrieval](https://github.com/langgenius/dify/blob/main/api/core/workflow/nodes/knowledge_retrieval/knowledge_retrieval_node.py)
- [dify 文档：知识检索节点](https://docs.dify.ai/guides/workflow/node/knowledge-retrieval)
- [dify 文档：知识库元数据](https://docs.dify.ai/guides/knowledge-base/metadata)

## 与主核 / AI 的关系

**「节点只做参数整理」与主核的算子层分工一致。** 数据库的执行器算子只负责「按给定的参数调用访问方法」，不判断业务语义；dify 的检索节点只负责「把 DSL 配置拼成请求」，不实现检索。**判据是「这段逻辑是否需要同时理解上下两层」**——需要就下沉到服务层，不需要才留在编排层。

**「单段取类型、多段退文本」与主核的隐式类型转换同一困境。** 拼接字符串天然是文本，但模板里只有一个变量时能保住原类型。数据库在多态表达式上也有同样问题：`int + numeric` 能定类型，`varchar || int` 就只能强制成文本。**类型信息在「组合」这个动作里丢失，是所有模板/表达式系统的共同代价。**

**「先 commit 再跨会话可见」与主核的事务可见性同源。** 摘要生成看不到未提交的行，等价于数据库里另一个事务读不到未提交数据。dify 用一行 commit 手工解决，数据库用隔离级别统一解决。**手工解决的危险在于它依赖调用顺序，而顺序不是接口的一部分。**

**未使用且签名不符的 Protocol，与主核里废弃的系统目录项同类。** 数据库的系统表若留下已废弃但未删除的条目，查询者会把它当成有效契约。**契约必须与实现一致，否则宁可不写。**

对 agent 工程最实用的五条。其一：**检索节点在两个条件下静默返回空结果**（无 selector、单库无 query），排查「为什么没检索到」时先确认这两条。其二：**附件引用格式不合规会被静默丢弃**，不会报错，也不会进 `attachment_ids`。其三：**重排配置里 `weighted_score` 缺权重是硬错误，`reranking_model` 缺模型不是**。其四：**索引节点在调试模式下不落库**，想验证真实写入必须走正式触发。其五：**不要在索引与摘要之间插入延迟或异步**，那行 commit 删掉后摘要会静默生成不全。
