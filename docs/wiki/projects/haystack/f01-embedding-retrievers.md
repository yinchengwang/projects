---
project: haystack
feature: embedding-retrievers
title: 嵌入检索器
page_type: feature
schema_version: 1
refs:
  - path: haystack/components/retrievers/text_embedding_retriever.py
    symbols: [TextEmbeddingRetriever]
  - path: haystack/components/retrievers/multi_query_embedding_retriever.py
    symbols: [MultiQueryEmbeddingRetriever]
  - path: haystack/components/retrievers/multi_retriever.py
    symbols: [MultiRetriever]
---

# 嵌入检索器

## 一句话职责

把「文本查询 → 向量 → 文档」这条链包成一个组件：`TextEmbeddingRetriever` 是单个查询的最小封装，`MultiQueryEmbeddingRetriever` 让一批查询并行跑并合并去重，`MultiRetriever` 让**多个异构检索器**（BM25 + 向量）并行跑并按可配置的方式合并。

## 机制是什么

三层的差别是「并行的维度不同」，而它们都建立在同一个底座上：**真正的检索由 `EmbeddingRetriever` 协议的一个实现承担（比如 `InMemoryEmbeddingRetriever`），它只接受向量，不接受文本。**

| 类 | 并行的维度 | 输入 | 合并方式 |
|---|---|---|---|
| `TextEmbeddingRetriever` | 无 | 单个 `query: str` | 不需要（单次） |
| `MultiQueryEmbeddingRetriever` | **同一查询的多个改写** | `queries: list[str]` | 去重 + 按分数降序 |
| `MultiRetriever` | **多个不同的检索器** | 单个 `query: str` | RRF 或拼接，可截断 |

**关键的分层意图：** 前两者内部持有**一个** embedder 与**一个** retriever；后者持有一批 `TextRetriever`（于是每个都自带 embedder）。也就是说 `MultiRetriever` 并不自己做嵌入——它要 `TextEmbeddingRetriever` 先包好。

## 怎么实现的

### 一、`TextEmbeddingRetriever`：为什么需要这一层

**它本身几乎没有逻辑，只有三步。**

```
embedding_result = self.text_embedder.run(text=query)
result = self.retriever.run(query_embedding=embedding_result["embedding"], filters=filters, top_k=top_k)
docs = result["documents"]
docs.sort(key=lambda doc: doc.score if doc.score is not None else -inf, reverse=True)
return {"documents": docs}
```

**这三步各自解决一个「组件协议不匹配」的问题：**

1. **把文本变成向量** —— 底层的 `EmbeddingRetriever` 只吃 `query_embedding`，于是必须有东西做这一步嵌入。
2. **把 `filters`/`top_k` 透传** —— 否则调用方得自己知道底层检索器的参数名。
3. **排序** —— 底层检索器「理论上」已经按分数排好了，但这里仍然排一次。

**第 3 步为什么要重排？** 因为 `score is not None` 的判断：**它把「没有分数」的文档排到最后**。底层实现可能把无分数的文档混在中间（或者干脆不排），而这个组件对调用方承诺「按相关度降序」。

**`None` 用 `-inf` 而不是 0** —— 因为分数可能是负数（余弦距离、内积都可能为负），用 0 会让负分文档排到 `None` 之后。`-inf` 保证 `None` 严格在最后。

**它的 `warm_up` / `close` 是「对每个内部组件挨个转发」的固定形态。**

```
for inner in (self.text_embedder, self.retriever):
    if hasattr(inner, "warm_up"): inner.warm_up()
```

**用 `hasattr` 而不是要求接口** —— 因为 `EmbeddingRetriever` 协议里 `warm_up` 是可选的（一个内存向量库不需要预热）。这符合 `AGENTS.md` 里「`warm_up` 只用于真实初始化且必须幂等」的规定：不是所有组件都需要它。

**四个生命周期方法（`warm_up` / `warm_up_async` / `close` / `close_async`）的形态完全一致：优先用异步版，没有则回退同步版。**

```
async def warm_up_async(self) -> None:
    for inner in (self.text_embedder, self.retriever):
        if hasattr(inner, "warm_up_async"): await inner.warm_up_async()
        elif hasattr(inner, "warm_up"): inner.warm_up()
```

**这个「异步优先、同步回退」的两级判断在三个类里都出现**，而它对应一个实际约束：组件可能只实现了同步版，此时在异步路径里调用它会阻塞事件循环——但那是不可避免的，只能在有异步版时优先用它。

**`run` 里无条件调 `self.warm_up()`。**

```
def run(self, query, filters=None, top_k=None):
    self.warm_up()
    ...
```

**不是「首次调用时预热」，而是每次调用都调。** 这依赖 `warm_up` 的幂等性（`AGENTS.md` 明确要求幂等），于是这个写法的成本只是几次 `hasattr` 与空调用。

**收益是：无论调用方是否在 `Pipeline` 里（`Pipeline` 会自动调 `warm_up`），这个组件都能独立工作。** 这是一个「为自己的可嵌入性买单」的选择。

**`to_dict` / `from_dict` 把内部组件一起序列化。**

```
return default_to_dict(self, retriever=component_to_dict(obj=self.retriever, name="retriever"), text_embedder=component_to_dict(obj=self.text_embedder, name="text_embedder"))
```

**`component_to_dict` 与 `default_to_dict` 的区别是前者处理「组件」、后者处理「本组件」。** 于是 YAML 里会嵌套两棵完整的组件树——这意味着这个组件的序列化产物可以独立还原（不需要调用方另外提供内部的 embedder/retriever）。

### 二、`MultiQueryEmbeddingRetriever`：线程池 + 信号量 + 一个「有界」的对应

**同步版的并行是 `ThreadPoolExecutor` + `executor.map`。**

```
with ThreadPoolExecutor(max_workers=self.max_workers) as executor:
    queries_results = executor.map(lambda query: self._run_on_thread(query, retriever_kwargs), queries)
    for result in queries_results:
        if not result: continue
        docs.extend(result)
```

**`executor.map` 保持输入顺序** —— 于是结果的顺序与 `queries` 一致。这对「合并去重后排序」不是必需的，但让行为可复现。

**而 `if not result: continue` 同时过滤掉 `None` 与空列表** —— `_run_on_thread` 在没有结果时返回 `None`，在有结果时返回列表。这个 `if not` 把两种情况都接住了。

**异步版用一个信号量显式限制并发，而注释解释了原因。**

```
# Bound concurrency to max_workers, mirroring the ThreadPoolExecutor in the sync `run`.
semaphore = asyncio.Semaphore(self.max_workers)

async def _bounded_run_one(query: str) -> list[Document] | None:
    async with semaphore:
        return await self._run_one_async(query, retriever_kwargs)

tasks = [asyncio.create_task(_bounded_run_one(query)) for query in queries]
results = await _gather_tasks_with_cancel(tasks)
```

**「mirroring the ThreadPoolExecutor in the sync run」这句话说明了这个设计的目的：让同步与异步两条路径有相同的并发上限。** 若不限并发，`asyncio.gather` 会一次把 N 个查询全部发出——对远端 API 来说这是限流风险，且行为与同步版不一致。

**`asyncio.create_task` + `_gather_tasks_with_cancel` 而不是直接 `gather`** —— 后者的取消语义有已知问题（外层被取消时子任务可能继续跑）。先显式建 task 再统一 gather，能让取消传播到每个 task。

**`max_workers` 的校验是一条构造期检查。**

```
if max_workers <= 0: raise ValueError(f"max_workers must be greater than 0, but got {max_workers}")
```

**错误消息把实际值带上了** —— 这一页三个类的校验消息都是这个形态（"must be greater than 0, but got {value}"），而它们的共同点是：**参数一错，组件就不可能有意义地运行，所以在构造时就拦。**

**去重是共享的工具函数。**

```
from haystack.utils.misc import _deduplicate_documents
...
docs = _deduplicate_documents(docs)
docs.sort(key=lambda doc: doc.score if doc.score is not None else -inf, reverse=True)
```

**去重与排序分开两步，顺序不能反。** 若先排序再去重，去重函数保留哪一份是不确定的（取决于实现）；先按「批次顺序」去重再排序，被保留的是「先出现的那一份」——而 `queries` 的顺序是调用方给的，于是保留策略是可预期的。

**注意它**不去重跨查询的分数**：同一个文档若被两个查询命中，保留先命中的那份分数（丢弃后命中）。这不一定是最高分。`MultiRetriever` 用 RRF 解决了这个问题（见下），而这个类没有——**这是它与 `MultiRetriever` 的一个实质差别**。

### 三、`MultiRetriever`：RRF 与一个「设了 top_k 就强制 RRF」的规则

**这是三个类里最复杂的一个，而它的复杂度集中在一个决定上：合并方式取决于 `top_k` 是否设置。**

```
def _merge_results(self, document_lists, top_k=None):
    # When top_k is set we always use reciprocal rank fusion to merge the results, regardless of join_mode,
    # so that truncation is applied to a consistently ranked list.
    if top_k is not None or self.join_mode == "reciprocal_rank_fusion":
        documents = _reciprocal_rank_fusion(document_lists)
        merged = sorted(documents, key=lambda d: d.score if d.score is not None else -inf, reverse=True)
        return merged[:top_k] if top_k is not None else merged
    return _deduplicate_documents([doc for docs in document_lists for doc in docs])
```

**注释给出的理由很关键：截断必须作用在一个「有全局一致排序」的列表上。**

若用 `concatenate` 模式再截断前 k 个，结果会是「第一个检索器的前 k 个」——因为拼接后顺序就是各检索器结果的首尾相接。那等于**只用了第一个检索器**。

于是这里的规则是：**`top_k` 一设，`join_mode` 就被忽略。** 这个覆盖是必要的，但它让 `join_mode` 的语义变成「只在不需要截断时生效」——而文档里两个参数的说明分别提到了这点（`top_k` 那条写着 "the results are always merged using reciprocal rank fusion (regardless of join_mode)"）。

**这里有一个 API 设计上的张力**：`join_mode` 是构造参数、`top_k` 是运行参数，而运行参数**能推翻构造参数的行为**。从「配置可预测」的角度看这不太好；但从「截断必须有意义」的角度看它是必需的。**取舍选择了正确性而非配置的一致性。**

**`concatenate` 模式的实现是一行：**

```
return _deduplicate_documents([doc for docs in document_lists for doc in docs])
```

**只是拍平 + 去重，没有任何重打分。** 于是文档的 `score` 还是各自检索器的原始分数——而它们**不可比**（BM25 的分数与余弦相似度完全不同量纲）。这正是 RRF 存在的理由：它丢开原始分数，只用**排名**。

**`_resolve_top_k` 里有一个「0 是合法值」的说明。**

```
"""
Resolve runtime values against the init defaults and reject negatives.

A resolved value of 0 is valid and means no documents are returned.
"""
if resolved_top_k is not None and resolved_top_k < 0: raise ValueError(...)
```

**构造时拒绝 `<= 0`，运行时只拒绝 `< 0`。** 这个不对称是刻意的：**`top_k=0` 在运行期是一个有意义的参数**（明确表达「这次不要文档」，比如用于只做过滤场景），而构造时设 `top_k=0` 说明配置本身就错了。

于是 `run` 里有一句提前返回：

```
if resolved_top_k == 0 or resolved_top_k_per_retriever == 0: return {"documents": []}
```

**它放在「解析检索器」之后、真正执行之前** —— 于是即使 `top_k=0`，一个非法的 `active_retrievers` 名字仍然会报错（因为 `_resolve_retrievers` 在前面）。这个顺序让错误检查完整。

**并行执行用 `ThreadPoolExecutor` + `as_completed`（而不是 `map`）。**

```
future_to_name = {}
for name, retriever in retrievers_to_run.items():
    run_kwargs = {"query": query}
    if resolved_top_k_per_retriever is not None: run_kwargs["top_k"] = resolved_top_k_per_retriever
    if resolved_filters is not None: run_kwargs["filters"] = resolved_filters
    future_to_name[executor.submit(retriever.run, **run_kwargs)] = name

for future in as_completed(future_to_name):
    name = future_to_name[future]
    try: results_by_name[name] = future.result().get("documents", [])
    except Exception as e: raise RuntimeError(f"Retriever '{name}' failed: {e}") from e
```

三处设计值得记：

1. **用 `future_to_name` 反向映射而不是按提交顺序** —— 因为 `as_completed` 的完成顺序不确定，必须靠 future 找回名字。
2. **`run_kwargs` 只在参数非 `None` 时加键** —— 而不是传 `top_k=None`。这尊重了底层检索器的默认行为（若显式传 `None` 而某个实现不做 `None` 检查，会出错）。
3. **`document_lists` 用 `retrievers_to_run` 的顺序重建** —— `results_by_name[name] for name in retrievers_to_run`。于是**合并时的顺序是配置顺序而不是完成顺序**，这保证了结果可复现（RRF 对顺序不敏感，但 `concatenate` 敏感）。

**异常处理是「一个检索器失败就让整次调用失败」。**

```
except Exception as e: raise RuntimeError(f"Retriever '{name}' failed: {e}") from e
```

**这与转换器那页「跳过坏文件」的取向相反**，而理由不同：多检索器查询的语义是「综合这几个来源」，缺一个来源的结果是**不完整的答案**而不是「少一个文件」。而消息里带上检索器名字，让人知道是哪个坏了。

**`_resolve_retrievers` 的错误消息把可用名字列出来了。**

```
unknown = set(active_retrievers) - self.retrievers.keys()
if unknown:
    raise ValueError(f"Unknown retriever name(s): {sorted(unknown)}. Available retrievers: {sorted(self.retrievers.keys())}")
```

**两处 `sorted`** —— 让错误消息可复现（`set` 的迭代顺序不固定）。这是一个小但正确的细节：错误消息本身也应当确定性。

**而 `active_retrievers` 的顺序不影响结果**（因为它只用来选子集，然后按 `retrievers_to_run` 遍历）。实际上 `return {name: self.retrievers[name] for name in active_retrievers}` 保留了传入顺序 —— 于是 `concatenate` 模式下的结果顺序由 `active_retrievers` 决定，这是可预期的。

## 关键取舍

**源码事实：** `TextEmbeddingRetriever` 只做三件事（嵌入、透传参数、按分数重排），而重排的目的是**把 `score is None` 的文档排到最后**——用 `-inf` 而不是 0，因为分数可能为负。

**源码事实：** 三个类的 `warm_up` / `warm_up_async` / `close` / `close_async` 都是「对每个内部组件用 `hasattr` 判断后转发」，异步版优先、同步版回退。

**源码事实：** `TextEmbeddingRetriever.run` **无条件**调 `self.warm_up()`（不是首次），依赖 `warm_up` 幂等——好处是组件能在 `Pipeline` 之外独立工作。

**源码事实：** `MultiQueryEmbeddingRetriever` 的同步版用 `ThreadPoolExecutor` + `map`（保序），异步版用 **`asyncio.Semaphore(max_workers)`**，注释明确说这是为了「与同步版的 `ThreadPoolExecutor` 一致」。

**源码事实：** 异步版先 `asyncio.create_task` 再统一 gather（`_gather_tasks_with_cancel`），而不是直接 `gather`——为了让取消能传播到每个子任务。

**源码事实：** 去重先于排序（保留「先出现的那一份」），且**跨查询的重复文档只保留首次命中的分数，不是最高分**。

**源码事实：** `MultiRetriever` **一旦设置 `top_k` 就强制用 RRF**，忽略 `join_mode`——因为截断必须作用在有全局一致排序的列表上，否则 `concatenate` 模式下截断出的是「第一个检索器的前 k 个」。

**源码事实：** `concatenate` 模式只是拍平 + 去重、不重打分，于是文档分数仍是各检索器的原始分数——而它们不可比（BM25 vs 余弦）。这正是 RRF 存在的理由。

**源码事实：** `top_k` 在构造时拒绝 `<= 0`，在运行时只拒绝 `< 0`——`top_k=0` 在运行期是有意义的（明确要求不返回文档）。

**源码事实：** `MultiRetriever` 的并行用 `as_completed` + `future_to_name` 反向映射（完成顺序不确定），但**合并时按 `retrievers_to_run` 的顺序重建列表**以保可复现。

**源码事实：** 单个检索器失败会抛 `RuntimeError`（带检索器名）而非跳过——与转换器「跳过坏文件」相反，因为缺一个来源意味着答案不完整。

**源码事实：** `_resolve_retrievers` 的错误消息对两个集合都 `sorted`，让消息本身确定性。

**架构推论：** 三个类的分层揭示了一个「包装器的层次」原则：**每一层只解决一个不可比问题**。

- `TextEmbeddingRetriever` 解决「协议不匹配」（文本 vs 向量）。
- `MultiQueryEmbeddingRetriever` 解决「多查询的合并」（去重 + 排序）。
- `MultiRetriever` 解决「多检索器的合并」（**不可比分数的归一**）。

最后一层才引入 RRF，因为只有它面对「分数不可比」这个问题——前两层的分数都来自同一个 embedder，可比。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/components/retrievers/text_embedding_retriever.py` | `TextEmbeddingRetriever` | 文本 → 向量 → 文档，含降序重排 |
| `haystack/components/retrievers/text_embedding_retriever.py` | `run` / `run_async` | 三条语句 + `-inf` 兜底排序 |
| `haystack/components/retrievers/text_embedding_retriever.py` | `warm_up_async` / `close_async` | 异步优先、同步回退的两级转发 |
| `haystack/components/retrievers/multi_query_embedding_retriever.py` | `MultiQueryEmbeddingRetriever` | 多查询并行检索 |
| `haystack/components/retrievers/multi_query_embedding_retriever.py` | `run` | `ThreadPoolExecutor` + `map`，去重后排序 |
| `haystack/components/retrievers/multi_query_embedding_retriever.py` | `run_async` | `Semaphore(max_workers)` + `create_task` + 带取消的 gather |
| `haystack/components/retrievers/multi_query_embedding_retriever.py` | `_run_on_thread` / `_run_one_async` | 单查询执行，无结果返回 `None` |
| `haystack/components/retrievers/multi_retriever.py` | `MultiRetriever` | 多检索器并行，可切子集 |
| `haystack/components/retrievers/multi_retriever.py` | `_merge_results` | 设 `top_k` 即强制 RRF；否则按 `join_mode` |
| `haystack/components/retrievers/multi_retriever.py` | `_resolve_top_k` | 构造拒绝 `<=0`、运行只拒绝 `<0` |
| `haystack/components/retrievers/multi_retriever.py` | `_resolve_retrievers` | 子集选择 + 确定性错误消息 |
| `haystack/components/retrievers/multi_retriever.py` | `run` | `as_completed` + `future_to_name`，合并时按配置顺序 |

## 参考

- [haystack 源码：text_embedding_retriever.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/retrievers/text_embedding_retriever.py)
- [haystack 源码：multi_retriever.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/retrievers/multi_retriever.py)
- [论文：Reciprocal Rank Fusion](https://plg.uwaterloo.ca/~gvcormac/cormacksigir09-rrf.pdf)
- [haystack 文档：检索器](https://docs.haystack.deepset.ai/docs/retrievers)

## 与主核 / AI 的关系

「多个索引并行查询再合并」与主核里的**并行计划 + 结果归并**是同一类问题：数据库也会把一个查询拆给多个分区/索引并行执行，再归并结果。而两者的一个关键差别是**分数可否比较**：数据库的结果是行（没有「相关度」这个概念），而检索结果带一个分数，且不同检索器的分数不同量纲。

RRF 就是为这个问题存在的：**丢掉分数、只用排名**。这与主核里「不同来源的统计信息不可直接相加」是同一个道理——量纲不同就不能混算，只能先归一。

对 agent 工程的直接价值有三条。其一，**「设了 top_k 就必须用能产生全局排序的合并方式」这条规则要显式写出来**。截断作用在错误排序的列表上，结果会静默地只来自一个来源——这类错误不报异常，只是效果打折。其二，**跨查询去重要考虑保留哪一份分数**。`MultiQueryEmbeddingRetriever` 保留首次命中的分数（可预期但不是最高），`MultiRetriever` 用 RRF 重算（不可比但正确）——选哪个取决于下游是否依赖绝对分数。其三，**「一个来源失败」的处理取决于「缺了它是否还能回答」**。转换器可以跳过坏文件，而多检索器查询缺一个来源就得到不完整的答案，所以必须报错。
