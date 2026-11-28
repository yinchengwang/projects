---
project: langchain
feature: in-memory-stores
title: 内存向量存储与检索实现
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/vectorstores/in_memory.py
    symbols: [InMemoryVectorStore]
  - path: libs/core/langchain_core/indexing/in_memory.py
    symbols: [InMemoryDocumentIndex]
---

# 内存向量存储与检索实现

## 一句话职责

给两份抽象各配一个「字典 + 全量扫描」的参考实现：向量库那份用余弦相似度排序、支持元数据过滤器与 MMR；文档索引那份干脆不做向量，只数「查询串在正文里出现了几次」——它们存在的意义是让契约可测、可跑，而不是可上生产。

## 机制是什么

`InMemoryVectorStore` 继承 `VectorStore`，构造只收一个 `embedding: Embeddings`。真正的状态是一个裸 dict：

```python
self.store: dict[str, dict[str, Any]] = {}
```

每条记录是四个键的普通字典：`"id"`、`"vector"`、`"text"`、`"metadata"`。构造处留了一条 TODO：「希望能改成 `dict[str, Document]`，但那会是一个破坏性改动」——所以这个四键 dict 是历史包袱，公开文档字符串里的示例还直接教用户 `vector_store.store.items()` 遍历并取 `doc['text']`。

`InMemoryDocumentIndex` 继承 `DocumentIndex`（`beta` 抽象，见 g02），是一个 Pydantic 模型，两个字段：`store: dict[str, Document]`（`default_factory=dict`）与 `top_k: int = 4`。它完全不碰嵌入——检索靠 `page_content.count(query)`。

## 怎么实现的

**`add_documents` 的 id 三级回退。** 同步版与异步版是两份几乎逐行相同的代码（差别只有 `embed_documents` vs `await aembed_documents`），核心循环：

```python
id_iterator = iter(ids) if ids else iter(doc.id for doc in documents)
for doc, vector in zip(documents, vectors, strict=False):
    doc_id = next(id_iterator)
    doc_id_ = doc_id or str(uuid.uuid4())
    self.store[doc_id_] = {...}
```

所以 id 的来源是三层：显式传的 `ids` → 文档自带的 `doc.id` → 随机 UUID。第三层的 `or` 而不是 `if` 是因为 `doc.id` 可能是空串（空串是 falsy），此时也会落到 UUID。

`ids` 与 `documents` 的长度校验在嵌入**之后**做——顺序值得注意：代码先 `texts = [...]` 再 `vectors = embed_documents(texts)` 再校验 `len(ids)`。所以一个长度不匹配的调用会**先付掉一次嵌入调用的费用**才报错。这个顺序在内存实现里算不上问题（嵌入是唯一的真实成本），但它把「参数校验」放在了「最贵的那步」之后。

`zip(..., strict=False)` 说明「向量条数少于文档条数」会被静默截断。若某个嵌入实现返回了短列表，这里不会报错，只会少写几条。

**写入是覆盖而不是追加。** `self.store[doc_id_] = {...}` ——同一个 id 再写一次就是替换。所以它天然满足 `UpsertResponse` 里「`upsert` 应当更新已存在内容」的语义（见 g02），尽管这里根本没叫 upsert。而 `DocumentIndex.upsert` 的 docstring 要求的正是这条行为，`InMemoryDocumentIndex` 也照做了（`self.store[id_] = item_`）。

**`delete` 的实现比契约窄。** `InMemoryVectorStore.delete` 签名照抄基类 `(ids=None, **kwargs) -> None`，但函数体只在 `if ids:` 时才做删除——**`ids=None` 什么都不做**。而基类 docstring 明写「若 `None` 则删除全部」。

这是一个真实的行为偏差：`delete()` 传 `None` 时，契约说「删全部」，实现说「什么都不删」。返回类型也被收窄成 `None`（基类是 `bool | None`）。所以它既没实现「删全部」，也不能报告成功/失败。

`InMemoryDocumentIndex.delete` 则走另一条路：`if ids is None: raise ValueError("IDs must be provided for deletion")` ——**拒绝**而不是删除。这符合 `DocumentIndex` 那条「不带参数调用应当抛 ValueError」的约定。

所以同一对「内存实现」对同一件危险操作给出了两种答案：向量库静默不删、文档索引报错。后者的方向对使用者更友好（错误比静默安全），但它俩面对的是两套不同严格度的契约，所以各自都是「符合自己那份契约」的。

**检索路径是「全量取 → 映射过滤器 → 余弦 → argsort」。** `_similarity_search_with_score_by_vector` 五步：

1. `docs = list(self.store.values())` ——全量取出。注释写「以固定顺序取全部文档」，所以 dict 的插入序保证了遍历顺序稳定。
2. 若有 `filter`，先构造 `Document` 再调它：`if filter(Document(id=..., page_content=..., metadata=...))`。所以过滤器看到的是**完整的 Document**，不是一个 metadata dict——这让调用方可以做内容级过滤（代价是每条都要构造一次对象）。
3. 空就 `return []`。
4. `similarity = cosine_similarity([embedding], [doc["vector"] for doc in docs])[0]` ——一次矩阵运算算所有相似度，取第 0 行。
5. `top_k_idx = similarity.argsort()[::-1][:k]` ——升序 argsort 再反转，得降序前 k 个下标。

第 5 步用 `argsort` 而不是直接 `argpartition`，是「完整排序」而不是「选出前 k」。对 k=4 与上万条文档，这多花了一些排序时间，但换来结果严格有序。

返回是**三元组** `(Document, float, list[float])` ——第三项是向量本身。这个三元组的唯一用途是 MMR（它需要候选的向量），所以有一层私有的 `_similarity_search_with_score_by_vector` 返回三元组、公开的 `similarity_search_with_score_by_vector` 剥成两元组。

那个列表推导里有一段用 walrus：

```python
for idx in top_k_idx
# 用 walrus 运算符赋值以避免多次查找
if (doc_dict := docs[idx])
```

`docs[idx]` 一定是存在的（`idx` 来自 `similarity` 的下标，长度与 `docs` 一致），所以这个 `if` 恒真。它的功能纯粹是「顺手绑定变量」。注释说「避免多次查找」——确实省了两次 `docs[idx]`。

**MMR 是「先取 fetch_k 再重排」。** `max_marginal_relevance_search_by_vector`：

```python
prefetch_hits = self._similarity_search_with_score_by_vector(embedding=embedding, k=fetch_k, filter=filter)
...
mmr_chosen_indices = maximal_marginal_relevance(np.array(embedding, dtype=np.float32), [vector for _, _, vector in prefetch_hits], k=k, lambda_mult=lambda_mult)
return [prefetch_hits[idx][0] for idx in mmr_chosen_indices]
```

所以先把度量过滤（含用户 `filter`）与相似度排序做一遍取 `fetch_k` 条，再交给 MMR 用向量数据做多样性挑选。**过滤器只在预取阶段生效**——MMR 阶段拿到的是已过滤的候选集。这个顺序是对的：若在 MMR 之后再过滤，可能挑出的多样性集合被过滤掉大半，k 就填不满。

numpy 检查放在预取之后而不是函数开头，理由同样明显：预取不需要 numpy（`_cosine_similarity` 的 simsimd 分支就不需要它，虽然那个函数自己也要求 numpy……）。实际效果是「只有在真要算 MMR 时才检查 numpy」，所以错误消息能精确说「`numpy` 必须安装才能用 `max_marginal_relevance_search`」。

**异步面全是「同步实现 + await 嵌入」。** `asimilarity_search_with_score` 里 `embedding = await self.embedding.aembed_query(query)`，然后调**同步**的 `similarity_search_with_score_by_vector`。`asimilarity_search_by_vector` 更是直接 `return self.similarity_search_by_vector(...)`。

所以这层的异步只覆盖了「唯一真正会阻塞的那一步」（嵌入的网络调用）。相似度计算是 CPU 的，就留在同步路径上——这与基类 `run_in_executor` 的保守做法（把整个同步方法丢线程池）不同。内存实现知道自己慢在哪，所以精确到只 await 那一步。

`aadd_documents` 则把整份循环又抄了一遍（而不是调同步版），因为中间要 `await aembed_documents`。

**`get_by_ids` 是「按 id 顺序过滤掉缺失的」。** 实现遍历 `ids`，`if doc:` 才 append。所以返回顺序与输入顺序一致，但可能更短。基类 docstring 对这条的要求是「返回的文档不保证与输入顺序一致，用户应当依赖返回文档里的 ID 字段」——所以这个实现比契约更强（它保序了）。

`if doc:` 而不是 `if doc is not None:` 有一个理论缺口：若某条记录是空 dict（不可能，因为写入时总填四个键）会被跳过。实际无害。

**`from_texts` / `afrom_texts` 是「构造 + 调 add」。** `store = cls(embedding=embedding)` 然后 `store.add_texts(...)`。所以它走的是基类 `add_texts` → 因为子类覆盖了 `add_documents` → 被拼成 Document 列表 → 回到自己的 `add_documents`。这条路径绕了一圈（见 g03 的方法身份探测），但结果是正确的：`metadatas` 被拼进 `Document.metadata`，`ids` 被拼进 `Document.id`。

**`dump` / `load` 靠 `dumpd` / `load` 做序列化。** `dump` 是 `json.dump(dumpd(self.store), f, indent=2)`；`load` 是 `load(json.load(f), allowed_objects=[Document])` 然后 `vectorstore.store = store`。

所以这两条路径走的是 langchain 自己的序列化体系（见 i01），而不是裸 JSON。`allowed_objects=[Document]` 是那个体系的白名单参数——**加载时只允许还原 `Document` 一种类型**。这个参数是必要的：`load` 会按序列化数据里的类型标记去查映射表并构造对象，不加限制就等于「让文件决定实例化什么」。

注意 `load` 把反序列化结果直接赋给 `vectorstore.store`，且没有校验形状——若文件里的 `store` 不是四键 dict 的结构，后续检索会在取 `doc["vector"]` 时崩掉。

**`InMemoryDocumentIndex` 的检索只数出现次数。** `_get_relevant_documents`：

```python
counts_by_doc.append((document, document.page_content.count(query)))
counts_by_doc.sort(key=operator.itemgetter(1), reverse=True)
return [doc.model_copy() for doc, count in counts_by_doc[: self.top_k]]
```

`str.count` 是**子串计数**，不是词频。所以「查询串恰好是另一个词的子串」会导致误命中；空查询串会让每个文档的 count 都等于「正文长度 + 1」（Python 的实现细节，但语义上是「无限次命中」）。按 count 降序排，同分时保持原顺序（`sort` 是稳定的）。

`doc.model_copy()` ——返回副本而不是原对象。所以调用方改返回文档的 metadata 不会污染索引。这是这一层唯一一处对「共享可变状态」的防御。

`upsert` 对 `item.id is None` 的处理是**先 copy 再改 id**：

```python
id_ = str(uuid.uuid4())
item_ = item.model_copy()
item_.id = id_
```

而 id 已存在时 `item_ = item`（不 copy）。所以「自动生成 id 的路径」不修改调用方传进来的对象，「显式给 id 的路径」则把原对象直接存进 store。这个不对称意味着：用显式 id 写入的文档，之后调用方若改了那个 Document 对象，索引里的内容也会跟着变。`InMemoryDocumentIndex` 的 docstring 说它「把文档存在字典里」，没有承诺隔离。

`get` 是 `[self.store[id_] for id_ in ids if id_ in self.store]` ——注意这是**两次**查字典（`in` 一次、索引一次），且保序、跳过缺失。与向量库那份的 `get_by_ids` 写法一致。

**`delete` 返回的 `DeleteResponse` 四个字段全填。** `DeleteResponse(succeeded=ok_ids, num_deleted=len(ok_ids), num_failed=0, failed=[])`。所以它兑现了 g02 里那条「不存在不算失败」——不存在的 id 既不进 `succeeded` 也不进 `failed`。

## 关键取舍

**源码事实：** 向量库的内部结构是 `dict[str, dict[str, Any]]`，每条记录四键（id/vector/text/metadata）；构造处的 TODO 明确说改成 `dict[str, Document]` 会是破坏性改动。

**源码事实：** 文档 id 三级回退（显式 ids → `doc.id` → UUID），用 `or` 所以空串也落到 UUID。

**源码事实：** `ids` 与文档长度校验发生在嵌入调用**之后**。

**源码事实：** `delete(ids=None)` 在向量库实现里什么都不做（与基类「None 删全部」的文档相反），文档索引实现则抛 `ValueError`。

**源码事实：** 过滤器收到完整 `Document` 对象；MMR 的过滤器只在预取阶段生效。

**源码事实：** 私有检索方法返回三元组（含向量）供 MMR 使用，公开方法剥成二元组。

**源码事实：** 异步只 await 嵌入那一步，其余留在同步路径。

**源码事实：** `dump`/`load` 走 `dumpd`/`load` 且 `allowed_objects=[Document]`；`load` 对 `store` 的形状不校验。

**源码事实：** 文档索引的检索是 `page_content.count(query)` 的子串计数，不是词频。

**架构推论：** 这两个实现的价值在于「让抽象可测」。`VectorStore` 与 `DocumentIndex` 都是 ABC，若没有一份内存实现，任何测试都得起真实后端。而内存实现同时暴露了契约的**薄弱处**：向量库那份把异步做得比基类更精确，文档索引那份用子串计数——后者说明 `DocumentIndex` 抽象里其实没有「检索相关性」的语义要求，只有「能返回一些文档」。一个参考实现能做到什么程度，往往反过来暴露了契约本身的松紧。

**架构推论：** `delete(ids=None)` 的行为分歧是「契约文档与实际实现脱节」的典型样本。基类 doc 说「None 删全部」、内存实现什么都不做——两者都不会报错，所以调用方永远不会发现。这类偏差在没有契约测试的情况下几乎不可能被捕获，而它恰恰出现在最危险的操作（批量删除）上。

**架构推论：** 「全量 dict + 全量扫描」这个实现形态的作用应该被说清：它是**参照实现**，不是生产实现。`cosine_similarity([embedding], [所有向量])[0]` 每次检索都是 O(N) 的矩阵运算，`list(self.store.values())` 每次都要物化一遍。它适合「几千条、测试、demo」，不适合真实规模。这一层的存在理由是把「怎么接一个向量库」这件事从「必须懂某个后端的 API」降成「实现一个字典操作」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/vectorstores/in_memory.py` | `InMemoryVectorStore` | 字典加余弦相似度的参照向量库 |
| `libs/core/langchain_core/vectorstores/in_memory.py` | `InMemoryVectorStore.store` | 四键 dict 的内部结构，历史包袱 |
| `libs/core/langchain_core/vectorstores/in_memory.py` | `InMemoryVectorStore.add_documents` | id 三级回退、先嵌入后校验长度 |
| `libs/core/langchain_core/vectorstores/in_memory.py` | `InMemoryVectorStore.delete` | `ids=None` 不删，与基类文档不符 |
| `libs/core/langchain_core/vectorstores/in_memory.py` | `_similarity_search_with_score_by_vector` | 全量取、映射过滤、argsort 降序 |
| `libs/core/langchain_core/vectorstores/in_memory.py` | `InMemoryVectorStore.max_marginal_relevance_search_by_vector` | 先取 `fetch_k` 再交给 MMR 重排 |
| `libs/core/langchain_core/vectorstores/in_memory.py` | `InMemoryVectorStore.afrom_texts` | 构造后走异步 add，绕基类委托一圈 |
| `libs/core/langchain_core/vectorstores/in_memory.py` | `InMemoryVectorStore.dump` | 走 `dumpd` 而非裸 JSON |
| `libs/core/langchain_core/vectorstores/in_memory.py` | `InMemoryVectorStore.load` | `allowed_objects=[Document]` 的类型白名单 |
| `libs/core/langchain_core/indexing/in_memory.py` | `InMemoryDocumentIndex` | Pydantic 模型的文档索引参照实现 |
| `libs/core/langchain_core/indexing/in_memory.py` | `InMemoryDocumentIndex.upsert` | 自动 id 时 copy 后改，显式 id 时直存 |
| `libs/core/langchain_core/indexing/in_memory.py` | `InMemoryDocumentIndex.delete` | `ids=None` 抛 ValueError |
| `libs/core/langchain_core/indexing/in_memory.py` | `InMemoryDocumentIndex._get_relevant_documents` | 子串计数降序取 `top_k` |

## 参考

- [langchain 源码：vectorstores/in_memory.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/vectorstores/in_memory.py)
- [langchain 源码：indexing/in_memory.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/indexing/in_memory.py)
- [langchain API 参考：InMemoryVectorStore](https://python.langchain.com/api_reference/core/vectorstores/langchain_core.vectorstores.in_memory.InMemoryVectorStore.html)

## 与主核 / AI 的关系

主核里最接近的是「内存表 / 临时表的参考实现」：为了让新存储引擎的接口能在没有落盘之前被测，先实现一套全内存版本。两者的定位完全一致——参照实现负责验证契约，生产实现负责性能。

差别在于「参照实现是否被当作产品」。主核的内存表明确是临时对象；这里的 `InMemoryVectorStore` 被写进公开文档，示例里直接教用户在 demo 中用，并且有 `dump`/`load` 让它能持久化。于是「全量扫描」这个本该只出现在测试里的性能特征，就成了不少实际项目的起点。这是一个值得注意的边界问题：**便利性与规模上限之间的取舍，往往被文档的「上手示例」隐式做出**。

与 g03（向量存储契约）的关系是这一页的骨架：那边定义「必选只有 `similarity_search`、其余可报错」，这边是实现者视角——它把几乎所有方法都实现了，包括异步面。所以读这两篇能同时看到「契约要求什么」与「一个实现愿意做到什么程度」。

与 g02（记录管理器）的对照：`InMemoryDocumentIndex` 继承的是 `DocumentIndex`（g02 里的 beta 抽象），而 `InMemoryRecordManager` 也在 g02 的同一份文件里。两份内存实现的分工是：「记录管理器」记「谁被索引过」，「文档索引」是「被索引的东西放哪儿」。这一页只做后者。

对 AI 工程最实用的一条是「参考实现会暴露契约的松紧」。`InMemoryDocumentIndex` 用 `page_content.count(query)` 做检索，这件事本身就是一条信息：`DocumentIndex` 抽象只要求「能返回一些文档」，没要求「相关性有意义」。若把这份实现当成默认检索路径，会得到一个「拼写相近就命中」的召回器。判断一个抽象是否真的定义了语义，看它的参照实现有没有偷懒是一条捷径。

另一条是 `delete(ids=None)` 那个偏差的教训。基类文档写「None 删全部」，实现什么都不做——两者都不报错，所以永远不会被发现。对危险操作的接口，`None` 这类「有隐含意义」的默认值必须配一个契约测试；否则文档与实现会安静地漂开，而漂开的代价是「以为删了其实没删」或「以为没删其实删光了」。这里的方向恰好是后者更安全（没删），但它只是运气好，不是设计。

最后一条是 `load(..., allowed_objects=[Document])`。反序列化时给类型白名单，是让「文件不能决定实例化什么」的唯一做法。任何从磁盘/网络恢复对象的实现，都该显式列出允许还原的类型——这个成本只有一行参数，收益是消除一整类反序列化风险（详见 i01 那一页的机制）。
