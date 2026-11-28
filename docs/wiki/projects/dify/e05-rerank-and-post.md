---
project: dify
feature: rerank-and-post
title: 重排与后处理
page_type: feature
schema_version: 1
refs:
  - path: api/core/rag/rerank/rerank_model.py
    symbols: [RerankModelRunner]
  - path: api/core/rag/summary_index/summary_index.py
    symbols: [SummaryIndex]
---

# 重排与后处理

## 一句话职责

`RerankModelRunner` 把「查询 + 一批候选」交给重排模型重打分并按阈值截断——它是检索链路里唯一能让「召回顺序」变成「相关顺序」的一步；`SummaryIndex` 是它的配套：为片段生成摘要并写进向量库，让摘要也参与检索。

## 机制是什么

重排在这一层不是「再算一次相似度」，而是**换一个模型重新打分**。原因是向量检索的分数来自「查询向量与片段向量的余弦相似度」——两个向量各自独立编码，从不互相看；而重排模型（交叉编码器）**同时看查询与文档**，能捕捉「这句话是否真的回答了这个问题」。

`RerankModelRunner` 继承 `BaseRerankRunner`，只暴露一个 `run(query, documents, score_threshold, top_n, query_type)`。它内部有两条实现路径，由「重排模型是否支持视觉」决定：纯文本走 `invoke_rerank`，多模态走 `invoke_multimodal_rerank`。

`SummaryIndex` 属于同一层的另一半：它不是重排，但它产出的摘要**会作为独立向量**进库，于是检索时摘要能命中、再回填到原始段落。这让「整段内容的语义」也能参与相似度竞争——而不只是片段开头的几百字。

## 怎么实现的

**视觉能力判定是每次 `run` 都做的，不是构造时做的。**

```python
is_support_vision = self._check_model_support_vision()
```

它从 `rerank_model_instance.get_model_schema()` 读 `features` 里有没有 `VISION`。放在 `run` 里而不是 `__init__` 里，意味着同一个 runner 实例可以跨模型使用（虽然当前每处都新建），但也意味着**每次重排都多一次 schema 查询**。

**不支持视觉时的行为在一个分支里不对称。**

```python
if not is_support_vision:
    if query_type == QueryType.TEXT_QUERY:
        rerank_result, unique_documents = self.fetch_text_rerank(...)
    else:
        return documents          # 图片查询：直接返回原顺序
else:
    rerank_result, unique_documents = self.fetch_multimodal_rerank(...)
```

**图片查询遇到不支持视觉的模型时直接返回原文档**，连阈值与 `top_n` 都不生效。这是个刻意的短路：纯文本模型无法为图片打分，与其拿 0 分或乱序，不如保留召回顺序。上一页提到的「数据集多模态但重排模型不支持视觉就跳过重排」就是这条决策在上层的镜像。

**去重发生在送去重排之前，且两个分支的判据不同。**

`fetch_text_rerank` 的过滤条件：

- `provider == "dify"` 且元数据里的 `doc_id` 未出现过，**且 `doc_type` 是空或 `TEXT`** → 收下；
- `provider == "external"` → 只要不在已收列表里就收下（用 `document not in unique_documents` 比较）。

第一条里的 `doc_type` 判断很关键：**文本重排会把图片文档排除掉**，因为把图片的 `page_content`（通常是文件名或占位符）送去打分毫无意义。这也意味着**多模态数据集用纯文本重排时，图片命中会被静默丢掉**——不是不重排，而是直接不进重排候选，`unique_documents` 里就没有它们，自然也不在最终结果里。

`fetch_multimodal_rerank` 的过滤条件相同，但构造输入不同：图片文档要读存储里的字节、base64 编码、`content_type` 标成 `IMAGE`；文本文档直接用 `page_content`。`doc_type` 缺失时统一按 `TEXT` 处理。

**`fetch_multimodal_rerank` 有个隐藏的重复工作。** 它先把文档转成 `MultimodalRerankInput` 列表（还给图片做了 base64 编码），然后：

```python
if query_type == QueryType.TEXT_QUERY:
    rerank_result, unique_documents = self.fetch_text_rerank(query, documents, ...)
```

**文本查询时把刚构造的 `docs` 整个丢弃**，重新走 `fetch_text_rerank`。所以视觉模型遇到文本查询时，是一次多余的 base64 编码 + 一次多余的去重遍历。功能上无害（`fetch_text_rerank` 会再过滤一遍），但这段代码的意图应该是「多模态模型也支持纯文本输入」，实现上却退化成了「重新走一遍文本路径」。

**结果重组靠下标回指原文档。** `rerank_result.docs` 里每条有 `index` 与 `score`，代码用 `unique_documents[result.index]` 取回元数据与 provider，但**正文用 `result.text` 而不是原文档的 `page_content`**。这个差别有实际意义：重排模型可能对文本做了归一化（截断、清理），返回的 `text` 与入库内容未必逐字相同。同时它**新建一个 `Document` 对象**，不修改入参——所以调用方拿到的重排结果是新对象，原来的 `documents` 里的分数不会变。

**阈值过滤与排序的顺序是「先过滤后排序再截断」。** 注意排序键是 `metadata.get("score", 0.0)`，而分数是刚从 `result.score` 写进去的，所以这里总能拿到值。`top_n` 只在非空时截断（`if top_n else rerank_documents`）——传 `0` 会被当成假值从而不截断。这与上一页 `calculate_vector_score` 的行为一致（那里也是 `[:top_k] if top_k else ...`），**是个容易踩的点：想「取零条」得用别的方式，传 0 等于不限制**。

**`run` 挂了两个装饰器：`@trace_span()` 与 `@with_credit_usage_created_by(KNOWLEDGE_RETRIEVAL)`。** 后者把重排消耗记到「知识检索」这个用途上——重排是按 token 计费的外部调用，必须记账。

**`SummaryIndex.generate_and_vectorize_summary` 是「预览同步、正式异步」的双通道。**

参数里的 `is_preview` 决定走哪边：

- `is_preview=True`：**同步**执行。先查数据集与文档、校验索引技术是高精度、摘要开关已开、文档不是 `qa_model`；再查该文档所有 `completed` 且 `enabled` 的段落；再查已有 `completed` 摘要的段落 id；两者相减得到**待处理的段落**。若待处理为空直接返回。然后起 `min(10, 待处理数)` 个线程，每个线程开**自己的** session，调 `SummaryIndexAdapter.generate_and_vectorize_summary`，异常只 `logger.exception` 不中断其余线程。
- `is_preview=False`：`generate_summary_index_task.delay(...)` 丢给 Celery。

这个「预览同步、正式异步」的分工很清晰：预览时用户在等结果，必须当场出来；正式索引走后台任务，可以慢。

**异步那一路的第三个参数是 `None`。** `generate_summary_index_task.delay(dataset_id, document_id, None)`——三参里第三个应该是 `summary_index_setting`，传 `None` 意味着让任务自己去读数据集的配置。所以正式路径**无法通过这个入口覆盖摘要配置**，只有预览路径能传。

**摘要在批量生成时「只补缺不重算」。** 预览路径先算 `pending_segment_ids = 已有 completed 摘要之外的段落`，只处理缺的那些。所以重复调用不会重算已有摘要——**摘要生成很贵，这条差距检查是必要的**，否则每次预览都会把整篇文档重算一遍。

**跳过 `qa_model` 文档是一条硬编码字符串比较。** `document.doc_form == "qa_model"`——用了字面量而不是枚举。结合问答索引那一页（问答的 `page_content` 是问题、答案在元数据里），跳过是对的：问答对本身就是精炼过的，再摘要没有意义。

**摘要写进向量库后靠两个元数据字段被识别。** 上一页讲过：`is_summary` 与 `original_chunk_id`。`SummaryIndexAdapter` 负责写这两项，`RetrievalService` 负责读它们并把摘要命中折叠回原始段落。所以**摘要在检索层不是独立结果，而是原始段落的另一个"入口"**。

## 关键取舍

**源码事实：** 视觉能力每次 `run` 都从模型 schema 里查一次，不在构造时缓存。

**源码事实：** 不支持视觉且 `query_type` 是图片查询时，直接返回原 `documents`，阈值与 `top_n` 都不生效。

**源码事实：** 文本重排会排除 `doc_type` 为 `IMAGE` 的 dify 文档，所以多模态数据集配纯文本重排时图片命中不参与重排、也不出现在结果里。

**源码事实：** 视觉模型 + 文本查询时，先构造的 `MultimodalRerankInput` 列表被丢弃，重新走一次 `fetch_text_rerank`。

**源码事实：** 重排结果的正文取 `result.text` 而非原 `page_content`，并新建 `Document` 对象（不修改入参）。

**源码事实：** `top_n` 为 0 等于不截断。

**源码事实：** `SummaryIndex` 预览同步、正式异步；预览只处理「缺 completed 摘要」的段落；跳过 `doc_form == "qa_model"`。

**源码事实：** 异步路径把摘要配置传 `None`，只能读数据集上的配置。

**架构推论：** 「不支持视觉就直接返回原文档」这个短路的方向值得商榷但可以理解。它保证了不发生「用错模型给图片打文本分」这种更糟的情况，代价是**用户以为开启了重排、实际没有**——而且没有任何日志或标记提示这次重排被跳过了。相比之下，「多模态数据集 + 纯文本重排」在上层有显式分支（`check_model_support_vision` 后打日志），这一层则完全静默。**同一个约束在不同层给出不同的可观测性，是这类多层降级最常见的问题。**

**架构推论：** 重排结果用 `result.text` 而不是原 `page_content`，实际上把「重排服务的文本规范化」引入了下游。这可能是刻意的（让送进 prompt 的内容与送去打分的完全一致，避免"用 A 打分、拿 B 生成"），也可能是无意的。无论哪种，它意味着**重排之后的正文可能不再与数据库里的段落逐字相同**，任何依赖精确匹配的逻辑（比如按内容去重）都要注意。

**架构推论：** 摘要「只补缺不重算」配合「摘要是原始段落的另一入口」形成一条很省的设计：摘要向量不是为了替代原文，而是为了在「整段语义」与「局部片段」之间多一个可匹配的粒度。代价是**向量库里多出一份数据**，且需要靠两个元数据字段维持与原段的关联——一旦 `original_chunk_id` 写错或缺失，那些摘要命中会变成"找不到原文的孤儿"而被静默丢弃。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/rag/rerank/rerank_model.py` | `RerankModelRunner` | 重排门面，文本与多模态两路 |
| `api/core/rag/rerank/rerank_model.py` | `RerankModelRunner.run` | 能力判定、阈值过滤、排序与截断 |
| `api/core/rag/rerank/rerank_model.py` | `RerankModelRunner._check_model_support_vision` | 从模型 schema 读视觉能力 |
| `api/core/rag/rerank/rerank_model.py` | `RerankModelRunner.fetch_text_rerank` | 文本去重（含排除图片）并调用重排 |
| `api/core/rag/rerank/rerank_model.py` | `RerankModelRunner.fetch_multimodal_rerank` | 图片转 base64 的多模态重排 |
| `api/core/rag/summary_index/summary_index.py` | `SummaryIndex` | 摘要生成与向量化的双通道入口 |
| `api/core/rag/summary_index/summary_index.py` | `SummaryIndex.generate_and_vectorize_summary` | 预览同步补齐、正式投递异步任务 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/rag/rerank/rerank_model.py](https://github.com/langgenius/dify/blob/main/api/core/rag/rerank/rerank_model.py)
- [dify 源码：core/rag/summary_index/summary_index.py](https://github.com/langgenius/dify/blob/main/api/core/rag/summary_index/summary_index.py)
- [论文：用交叉编码器做段落重排](https://arxiv.org/abs/1908.10084)

## 与主核 / AI 的关系

「召回用便宜模型、重排用贵模型」与主核里「先用索引粗筛、再用精确条件过滤」是同一个漏斗结构：**便宜的手段负责把候选压到可控规模，贵的手段只处理少量候选**。主核的精确过滤是确定性的，这里的重排是模型打分——所以必须多一层阈值与截断，否则贵的那一步会退化成"把所有候选都算一遍"。

对 RAG 工程最实用的四条。其一：**重排是唯一能真正改善排序的一步**，向量相似度只看「编码后是否接近」，交叉编码器才看「是否回答了问题」；在意答案质量时这一步的收益通常比换嵌入模型更大。其二：**多模态数据集要配支持视觉的重排模型**，否则图片命中会在文本重排路径上被静默排除，表现为"图片内容永远检索不到"。其三：**`top_n` 传 0 等于不限制**，想返回空结果得在调用方自己处理。其四：**摘要索引的收益依赖 `original_chunk_id` 的完整性**，这个字段一旦缺失或写错，摘要命中会变成无法回填的孤儿；批量建库后建议核对一次「摘要向量数 = 可回填数」。
