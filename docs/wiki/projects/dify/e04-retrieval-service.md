---
project: dify
feature: retrieval-service
title: 单库检索服务
page_type: feature
schema_version: 1
refs:
  - path: api/core/rag/datasource/retrieval_service.py
    symbols: [SegmentRecord, DefaultRetrievalModelDict]
  - path: api/core/rag/docstore/dataset_docstore.py
    symbols: [DatasetDocumentStore]
---

# 单库检索服务

## 一句话职责

`RetrievalService` 在**一个**知识库上按检索方法并发地发起一到三路查询并把结果合并，`DatasetDocumentStore` 则负责向量命中之后的回表——把「一个 `doc_id` 与一个分数」变成带原文、子块与附件的完整段落。

## 机制是什么

先看两个 TypedDict 的分工，它们恰好对应检索链的两端：

`DefaultRetrievalModelDict` 是**配置的形状**，描述一个知识库「怎么检索」：检索方法、是否重排、重排模型、重排模式、权重、分数阈值、`top_k`、阈值开关。它有模块级默认值 `default_retrieval_model`——语义检索、不重排、`top_k=4`、阈值关闭。所有可选键都标成 `NotRequired`，所以老配置缺字段也能通过类型检查。

`SegmentRecord` 是**中间结果的形状**，只保证有 `segment`（一个 `DocumentSegment` 行对象），其余三个键都是可选的：`score`、`child_chunks`、`files`。这个「只有 segment 必填」的设计很关键——它让「只命中父块（无子块）」「命中摘要（走另一条路回填）」这些情况共用同一个容器。

`DatasetDocumentStore` 是**写侧与读侧的对偶**：写侧 `add_documents` 把 `Document` 变成 `DocumentSegment` 行，读侧 `get_docs` / `get_document` 把它变回来。两侧共享同一个身份：`Document.metadata["doc_id"]` 与 `DocumentSegment.index_node_id` 是同一个值。

## 怎么实现的

**`retrieve` 是「一次请求最多三路并发」。** 它按 `RetrievalMethod` 的能力位（见上一页）决定提交哪些任务：

- `KEYWORD_SEARCH` 且 `query` 非空 → `keyword_search`
- 支持语义 → `query` 非空时提交 `embedding_search`（文本查询）；`attachment_ids` 非空时逐个提交 `embedding_search`（图片查询）
- 支持全文 → `query` 非空时提交 `full_text_index_search`

三个 worker 方法都是同一套骨架：`with flask_app.app_context()` 包一层、`try` 里干活、`except` 里 `logger.error` 并把**异常字符串 append 进共享的 `exceptions` 列表**。所以它们都**不抛异常**，失败信息靠共享列表传回主线程，最终 `raise ValueError(";\n".join(exceptions))`。

这个设计有一个实际后果：**多个 worker 同时失败时，错误消息是拼接的多条**，去掉一条就得改全部——因为没有任何 worker 会中断其它 worker。为了补偿，主线程用 `as_completed(timeout=3600)` 一旦发现异常就 `f.cancel()` 其余任务。

**混合检索的分数阈值必须延后执行。** `embedding_search` 里有一段注释解释得很清楚：

> Hybrid search merges keyword / full-text / vector hits and then reranks (weighted fusion or reranking model). Applying the user score threshold at vector retrieval time uses embedding similarity, which is not comparable to reranked or fused scores and incorrectly drops high-quality chunks.

所以代码在混合模式下把向量检索的阈值**强制改成 0.0**，真正的阈值等重排之后再应用。原始分数来自嵌入相似度，重排后的分数来自交叉编码器或加权融合，两者不在同一量纲——拿用户的阈值去卡原始分数会误杀优质片段。

**单项重排只在「纯语义」或「纯全文」时做。** `embedding_search` 只在 `retrieval_method == SEMANTIC_SEARCH` 时才调 `DataPostProcessor`，`full_text_index_search` 只在 `FULL_TEXT_SEARCH` 时调。混合模式两者都不重排——因为混合的重排发生在合并之后（`_retrieve` 尾部）。这是一条清晰的规则：**重排只在结果集最终确定的那一层做，不做两遍。**

**多模态重排有一条「模型不支持视觉就退回原结果」的分支。** 数据集是多模态时，`embedding_search` 会先查重排模型是否支持视觉（`check_model_support_vision`）；不支持就直接 `all_documents.extend(documents)`，注释写「not effective, return original documents」。也就是说**多模态数据用纯文本重排模型等于不重排**——直接跳过比拿到无意义的分数好。

**`_retrieve` 尾部的混合去重有一处特例。** 混合检索先 `_deduplicate_documents(all_documents_item)` 再去重排。但去重之前有一句：

```python
if attachment_id and reranking_mode == RerankMode.WEIGHTED_SCORE:
    all_documents.extend(all_documents_item)
```

即「图片查询 + 加权分数模式」时**先把未去重的结果直接放进最终结果**，然后去重后的版本再走重排并 `extend` 一次。于是最终结果里会有重复项。这个分支看起来是刻意保留图片查询的原始结果（加权模式下图片与文本的分数不可比），但代价是**同一张图可能出现两次**。

**去重规则是「有 `doc_id` 按 id、无 `doc_id` 按内容」，且两种规则的取舍不同。** `_deduplicate_documents` 的 docstring 写得很明确：

- 有 `doc_id`：键是 `(provider, doc_id)`，**保留分数最高的那个**；后出现的重复项若无分数则忽略。
- 无 `doc_id`：键是 `(provider, page_content)`，**保留第一个**，不比较分数。

两套规则并存的原因：向量库返回的文档一定有 `doc_id`（那是元数据），而全文检索或外部来源可能没有。全用内容哈希会把「同内容的两个不同片段」误合并；全用 id 则无法处理没有 id 的条目。

实现上是 O(n) 的：用一个 `chosen` 字典加一个 `order` 列表记录首次出现顺序，最后按 `order` 取——这保证输出顺序稳定（Python 3.7+ 的 dict 本身有序，但保留显式列表让意图更明确）。

**混合检索在「没有重排器」时还要补一次阈值过滤。**

```python
if not data_post_processor.rerank_runner and score_threshold:
    all_documents_item = self._filter_documents_by_vector_score_threshold(...)
```

因为前面把向量阈值改成 0 了，若 `DataPostProcessor` 内部也没有真正的重排器（`rerank_runner` 为空，例如只做加权融合），就没人执行阈值——于是这里手工补一遍。注释直接引用了 `calculate_vector_score` 的规则，说明这是两处必须保持一致的逻辑。

**`escape_query_for_search` 只做了一件事：双引号转义。** `query.replace('"', '\\"')`。这个方法给关键词检索与全文检索用，那些后端把查询串嵌进某种查询语法里，未转义的双引号会破坏语法。**它不做任何其他转义**，所以如果后端语法里还有别的元字符（例如某些库的 `+`、`-`、`*`），这里管不了。

**`format_retrieval_documents` 是最重的一段，本质是「多态回表」。** 输入是一批 `Document`（来自向量库/全文/关键词），输出是 `list[RetrievalSegments]`。它要处理四种命中形态：

1. **摘要命中**：文档元数据里 `is_summary` 为真。这类**不直接返回**，而是把 `original_chunk_id` 记进 `summary_segment_ids`、把摘要分数记进 `summary_score_map`（同一片段多次摘要命中取最高），然后 `continue`。后面按 `original_chunk_id` 查回原始段落，并把摘要内容一并带上。
2. **父子结构的子块命中**：先把 `doc_id` 分进 `child_index_node_ids`，回表查到 `ChildChunk` 行，再由 `segment_id` 找到父段落。父段落的分数取**所有子块分数的最大值**。
3. **图片命中**：分进 `image_doc_ids`，走附件绑定表查回所属段落。
4. **普通段落命中**：分进 `index_node_ids`，直接查 `DocumentSegment`。

`doc_to_document_map` 是贯穿全程的辅助结构：`{doc_id: 原始 Document}`，用来在最后算分时从原始命中记录里取 `score`。**没有它，子块与附件的分数就丢了**——数据库行里没有检索分数。

几个值得记住的实现细节：

- 所有批量查询都带 `enabled.is_(True)` 与 `status == "completed"` 双重过滤。所以**被禁用的段落永远检索不到**，即使向量库还留着它的向量。
- `assert` 被用来做非空断言（`assert i.index_node_id`、`assert index_node_segment.index_node_id`）。在生产代码里用 `assert` 意味着这些不变量靠「数据库列非空」保证，一旦违反会抛 `AssertionError` 而不是友好错误。
- 整个方法是 `try/except` 包住的，异常时先 `session.rollback()` 再重抛。这是唯一一处显式回滚——因为它是只读查询，回滚只是为了让 session 状态干净。
- 最后按分数降序排序，`score` 为 `None` 时当作 `0.0`。所以**只有分数没有位置信息**，同分时顺序取决于数据库返回顺序。

**`DatasetDocumentStore.add_documents` 是幂等覆盖而非追加。** 逐条查 `get_document_segment`，存在就更新（覆盖 `content`、`answer`、`index_node_hash`、`word_count`、`tokens`，并重建子块），不存在才新建。`allow_update=False` 时已存在就抛 `ValueError`。

新建段落时 `enabled=False`——**新段落默认禁用**，要等整个文档索引完成才由上层开启。这解释了为什么索引失败后重试是安全的：残留段落是禁用状态，检索不到。

`position` 用 `max(position) + 1` 递增，取的是该文档下的最大值，所以新段落总是追加在末尾。

`doc.metadata.pop("answer", "")` 值得单独注意：它把答案从元数据里**取走**（不是读后保留），所以同一次 `add_documents` 里同一份 `Document` 若被处理两次，第二次就没有 `answer` 了。这是就地修改入参。

`add_multimodel_documents_binding` 只在 `_document_id` 非 `None` 时生效——所以构造 store 时不传 `document_id` 就无法写附件绑定，是静默跳过而不是报错。

**`DatasetDocumentStore` 的 `docs` 属性是个刻意的陷阱。** 它被声明成 property 但直接 `raise ValueError("session is required; use get_docs(session)")`。这是「接口收紧」的显式处理：老代码通过 `.docs` 拿全量文档，现在强制要走带 session 的 `get_docs`。**用一个必定抛错的 property 而不是删掉属性，是为了让老调用点立刻失败并看到迁移指引**，而不是静默拿到空字典。

`to_dict` 只序列化 `dataset_id`——`user_id` 与 `document_id` 不在里面，所以 `from_dict` 反序列化出来的对象必须显式补这两个参数才能用于写入。**这意味着这个类不可完整往返序列化**，`from_dict` / `to_dict` 只覆盖了「读」的场景。

## 关键取舍

**源码事实：** `retrieve` 按能力位最多并发三路（关键词、向量、全文），worker 用共享 `exceptions` 列表汇报失败、不抛异常；主线程 `as_completed` 见异常即 `cancel` 其余任务。

**源码事实：** 混合检索时向量检索阈值被强制设为 `0.0`，阈值统一延后到重排之后；若重排器为空则再手工补一次阈值过滤。

**源码事实：** 单项重排只发生在纯语义或纯全文模式；混合模式的重排只在合并后做一次。

**源码事实：** 多模态数据集在重排模型不支持视觉时直接返回原结果，不做重排。

**源码事实：** 「图片查询 + 加权分数」分支把未去重结果也加入最终列表，与去重后的重排结果并存，因此可能重复。

**源码事实：** 去重规则分两套：有 `doc_id` 按 `(provider, doc_id)` 取胜者（保留最高分），无 `doc_id` 按 `(provider, page_content)` 取首现。

**源码事实：** 摘要命中的文档不直接返回，按 `original_chunk_id` 回填原始段落，分数取该片段所有摘要命中的最大值。

**源码事实：** 父块分数取所有子块分数的最大值；回填过程中所有查询都带 `enabled` 与 `status=completed` 过滤。

**源码事实：** 新建段落 `enabled=False`，`position` 取该文档当前最大值加一；`add_documents` 就地 `pop` 掉 `metadata["answer"]`。

**源码事实：** `DatasetDocumentStore.docs` 是必定抛错的 property，强制调用方改用 `get_docs(session)`；`to_dict` 只写 `dataset_id`。

**架构推论：** 「worker 不抛异常、错误走共享列表」是一种刻意的**汇合式错误处理**：并发场景下让所有分支都跑完再一次性汇报，比第一个失败就中断更容易看到全貌（可能三路都坏了）。代价是错误消息变成拼接串、且必须靠主线程的 `cancel` 才能止损。

**架构推论：** 「分数只在原始命中记录里、数据库行里没有分数」这一点决定了回表的形态。父子结构拿不到「哪个子块分数最高」以外的信息，摘要结构只能靠 `original_chunk_id` 反查——**所有跨层聚合都必须在内存里做，因为持久层不保存检索分数**。这是刻意的：检索分数是每次查询的临时产物，不该落库。

**架构推论：** `docs` 那个抛错的 property 是一个值得学的迁移手法。删掉属性会让老代码在 import 或运行时抛 `AttributeError`（信息量低、位置难定位）；保留属性但主动抛带指引的 `ValueError`，能让每个错误清晰指向「该怎么改」。**用运行时错误替代编译期/属性错误，是在弱类型语言里做接口迁移的实用办法。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/rag/datasource/retrieval_service.py` | `RetrievalService.retrieve` | 单库多路并发检索入口 |
| `api/core/rag/datasource/retrieval_service.py` | `RetrievalService._retrieve` | 按能力位分派三路 + 混合去重与重排 |
| `api/core/rag/datasource/retrieval_service.py` | `RetrievalService.keyword_search` | 关键词倒排检索（经济模式） |
| `api/core/rag/datasource/retrieval_service.py` | `RetrievalService.embedding_search` | 向量检索，混合模式延后阈值，单项重排 |
| `api/core/rag/datasource/retrieval_service.py` | `RetrievalService.full_text_index_search` | 全文检索，单项重排 |
| `api/core/rag/datasource/retrieval_service.py` | `RetrievalService._deduplicate_documents` | 双规则去重，稳定顺序 |
| `api/core/rag/datasource/retrieval_service.py` | `RetrievalService._filter_documents_by_vector_score_threshold` | 无重排器时补的阈值过滤 |
| `api/core/rag/datasource/retrieval_service.py` | `RetrievalService.format_retrieval_documents` | 四种命中形态的多态回表 |
| `api/core/rag/datasource/retrieval_service.py` | `RetrievalService.escape_query_for_search` | 仅转义双引号的查询清洗 |
| `api/core/rag/datasource/retrieval_service.py` | `SegmentRecord` | 回表中间结果：段落必填、其余可选 |
| `api/core/rag/datasource/retrieval_service.py` | `DefaultRetrievalModelDict` | 检索配置形状与默认值 |
| `api/core/rag/docstore/dataset_docstore.py` | `DatasetDocumentStore.add_documents` | 幂等覆盖写入，含子块重建 |
| `api/core/rag/docstore/dataset_docstore.py` | `DatasetDocumentStore.get_docs` | 全量读回为 `Document` 字典 |
| `api/core/rag/docstore/dataset_docstore.py` | `DatasetDocumentStore.get_document_segment` | 按 doc_id 定位段行，带租户/库/文档三重约束 |
| `api/core/rag/docstore/dataset_docstore.py` | `DatasetDocumentStore.docs` | 必定抛错的属性，强制迁移到 `get_docs` |
| `api/core/rag/docstore/dataset_docstore.py` | `DatasetDocumentStore.add_multimodel_documents_binding` | 写段落与附件的绑定 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/rag/datasource/retrieval_service.py](https://github.com/langgenius/dify/blob/main/api/core/rag/datasource/retrieval_service.py)
- [dify 源码：core/rag/docstore/dataset_docstore.py](https://github.com/langgenius/dify/blob/main/api/core/rag/docstore/dataset_docstore.py)
- [dify 文档：混合检索与重排](https://docs.dify.ai/guides/knowledge-base/retrieval-test-and-citation)

## 与主核 / AI 的关系

「检索只返回引用、回表才拿到内容」与主核里「索引扫描产出 TID、回表取行」是完全同构的两阶段读。主核的 `Index Only Scan` 之所以快，是因为索引里已经带了需要的列；这里的向量命中之所以必须回表，是因为**向量库里只有 `doc_id` 与分数，没有原文**。同一份语义在两边都是「先定位、再取数」。

对 RAG 工程最实用的四条。其一：**混合检索的分数阈值是延后生效的**，别指望它在向量召回阶段就过滤——调阈值实际影响的是重排后的结果。其二：**被禁用的段落永远检索不到**，即使向量还在库里；排查「明明有内容检索不到」时先查段落的 `enabled` 与 `status`。其三：**摘要命中会回填到原始段落**，所以返回的条数可能少于摘要命中数（多个摘要指向同一片段时合并，分数取最大）；这解释了为什么「摘要检索开了但结果条数没变多」。其四：**同一张图在「图片查询 + 加权分数」组合下可能出现两次**，去重不能省——把检索结果拼进 prompt 前先按 `doc_id` 自己去一次重。
