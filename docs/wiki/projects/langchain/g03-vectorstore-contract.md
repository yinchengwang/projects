---
project: langchain
feature: vectorstore-contract
title: 向量存储契约与检索器
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/vectorstores/base.py
    symbols: [VectorStore, VectorStoreRetriever]
  - path: libs/core/langchain_core/vectorstores/utils.py
    symbols: [maximal_marginal_relevance]
---

# 向量存储契约与检索器

## 一句话职责

把「向量库」抽象成一份**极少必选、其余给默认实现**的契约：只强求相似度检索与一批元操作，其余（相关性归一、MMR 多样性、检索器包装）都由基类给出默认路径——代价是默认路径只能兼容「余弦风格的归一化」，每种距离度量都得自己声明。

## 机制是什么

`VectorStore(ABC)` 是那个抽象。必选只有一个：`similarity_search(query, k=4, **kwargs)` 标了 `@abstractmethod`。同一族的 `similarity_search_with_score`、`max_marginal_relevance_search` 等默认实现直接 `raise NotImplementedError`——所以它们不是「必须实现」，而是「不实现就报错」。

`VectorStoreRetriever(BaseRetriever)` 是那层包装：三字段 `vectorstore`、`search_type`（默认 `"similarity"`）、`search_kwargs`（默认空 dict），外加一个 `allowed_search_types: ClassVar` 三元组 `("similarity", "similarity_score_threshold", "mmr")` 与 `arbitrary_types_allowed=True`。

`maximal_marginal_relevance` 是纯函数，签名是 `(query_embedding, embedding_list, lambda_mult=0.5, k=4) -> list[int]`——它返回**下标**而不是向量，所以调用方保留了对原数据的控制权。

## 怎么实现的

**`add_texts` 的默认实现靠「子类有没有覆盖 `add_documents`」来判断能力。** 这是基类里最不寻常的一处：

```python
if type(self).add_documents != VectorStore.add_documents:
    # 子类提供了 upsert 实现，走这条路径
    ...
    return self.add_documents(docs, **kwargs)
msg = f"`add_texts` has not been implemented for {self.__class__.__name__} "
raise NotImplementedError(msg)
```

它比较的是**方法身份**（`type(self).add_documents != VectorStore.add_documents`），而不是某个能力标志。所以「基类能否提供 `add_texts`」完全取决于「子类是否覆盖了 `add_documents`」。

走默认路径时，它把 `texts` / `metadatas` / `ids` 三个平行序列拼成 `Document` 列表（`metadatas` 缺省用 `cycle([{}])` 无限重复空 dict、`ids` 缺省用 `cycle([None])`），并用 `zip(..., strict=False)` 截断到最短。若传了 `ids`，还会往 `kwargs` 里**再塞一份** `kwargs["ids"] = ids`，注释写的是「为了向后兼容」——所以 `ids` 既是显式参数又被重复传给子类的 `add_documents`。

这个判断方式的代价很直接：一个子类若为了别的目的覆盖了 `add_documents`（却没真正支持 add-by-document），基类会以为它支持 `add_texts`，于是调用它并得到一个错误。用方法身份做能力探测，本质上是用「实现痕迹」代替「声明」。收益是不必给每个子类加一个布尔字段，也不必维护一份能力清单。

**`add_documents` 与 `aadd_documents` 的相互委托有终止条件。** 同步版里：

```python
if type(self).add_texts != VectorStore.add_texts:
    return self.add_texts(texts, metadatas, **kwargs)
return await run_in_executor(None, self.add_documents, documents, **kwargs)  # 异步版
```

所以 `add_documents` 在「子类覆盖了 `add_texts`」时转调 `add_texts`；`aadd_documents` 在「子类没覆盖 `add_texts`」时把 `add_documents` 丢线程池。两个判断互补，正好避免无限互调：子类只实现 `add_texts` 时，`add_documents` → `add_texts`（真实现）；子类只实现 `add_documents` 时，`add_texts` → `add_documents`（真实现）。这是用「谁覆盖了谁」做双向路由的经典手法。

**相关性归一是「三个静态公式 + 一个必须覆盖的选择器」。** 三个 `@staticmethod`：

- `_euclidean_relevance_score_fn(distance)` → `1.0 - distance / math.sqrt(2)`
- `_cosine_relevance_score_fn(distance)` → `1.0 - distance`
- `_max_inner_product_relevance_score_fn(distance)` → `distance > 0 ? 1.0 - distance : -1.0 * distance`

欧氏那个的分母 `sqrt(2)` 是有讲究的，注释解释了：「这个函数把归一化嵌入的欧氏范数（0 最相似、`sqrt(2)` 最不相似）转成相似度（0 到 1）」。所以它假定嵌入是**单位范数**的——两个单位向量的欧氏距离最大是 `sqrt(2)`（正交时）。若嵌入没归一化，这个公式会把距离大于 `sqrt(2)` 的结果压成负数，得到无意义的相似度。

内积那个的符号分别处理是为了让两边都落在 `[0,1]`：内积为负时取 `-1.0 * distance`（负负得正），为正时取 `1.0 - distance`。注释没说这条的理由，但从形状看它是把 `[-1,1]` 的余弦相似度折成两段。

而 `_select_relevance_score_fn` 是 `raise NotImplementedError` 的——docstring 明写「**VectorStore 应当定义它们自己的**基于选择的（selection-based）相关性方法」。所以这套设计是「给你三个现成公式，但你必须自己声明用哪个」。基类拒绝猜，因为猜错的后果是「相似度分数没有意义但看起来正常」——比报错更难发现。

**`similarity_search_with_relevance_scores` 在基类就实现了阈值过滤。** 它先 `score_threshold = kwargs.pop("score_threshold", None)`（从 kwargs 里摘掉，不让它流到下层），调 `_similarity_search_with_relevance_scores` 得到 `(doc, score)` 列表，再按阈值筛。

而 `_similarity_search_with_relevance_scores` 的实现是两行：拿 `_select_relevance_score_fn()`，然后 `[(doc, fn(score)) for doc, score in docs_and_scores]`。所以「归一化」与「阈值过滤」是两个独立的层，前者可被子类整体覆盖，后者固定在基类。

注意 `_asimilarity_search_with_score` 的注释：「这是一个让相似度检索变成异步的临时变通。正确的做法是在向量库实现里把相似度检索做成异步的」。所以异步版本普遍是 `run_in_executor` 包同步——整个异步面是**兼容层**而不是原生实现。这句话在文件里出现了好几次（`asimilarity_search_with_score`、`amax_marginal_relevance_search`、`afrom_texts` 等），是这一层最诚实的自述。

**`search(query, search_type, **kwargs)` 是一个三选一分派器。** 三个分支：

- `"similarity"` → `similarity_search`
- `"similarity_score_threshold"` → `similarity_search_with_relevance_scores` 后**丢掉分数**（`[doc for doc, _ in docs_and_similarities]`）
- `"mmr"` → `max_marginal_relevance_search`

其余抛 `ValueError` 并列出三个合法值。这个分派器是给检索器用的——`VectorStoreRetriever` 的 `search_type` 就是从这里查。

注意 `similarity_score_threshold` 那个分支丢掉了分数：所以「用阈值过滤」这个能力是存在的，但通过 `search()` 走的时候只看结果不看分数。想要分数得直接调 `similarity_search_with_relevance_scores`。

**`as_retriever` 把整个检索配置塞进 kwargs。** 实现是一行：

```python
tags = kwargs.pop("tags", None) or [*self._get_retriever_tags()]
return VectorStoreRetriever(vectorstore=self, tags=tags, **kwargs)
```

所以 `search_type` 与 `search_kwargs` 都是通过 `**kwargs` 传进去的，不做显式校验。而 `_get_retriever_tags` 给出 `[类名]` 加上（若有嵌入）`[嵌入类名]`——所以一个 `VectorStoreRetriever` 的 tags 天然记录了「哪个向量库 + 哪个嵌入模型」，这对回调追踪很有用。

大段 docstring 里的四个例子实际构成了这份 API 的**教程**：`search_type="mmr"` 加 `lambda_mult` 调多样性、`fetch_k` 与 `k` 分开（先取多少给 MMR 考虑、再返回多少）、`score_threshold` 过滤、`filter` 按元数据过滤。

**`VectorStoreRetriever` 的校验是 `model_validator(mode="before")`。** 它在字段赋值前跑（`values: dict`），因为要检查「`search_type` 是否在允许集合里」以及「`score_threshold` 是否给了合法值」。放在 before 阶段的好处是：若校验失败，实例根本没被构造出来——不会出现「构造成功但配置无效」的对象。

`arbitrary_types_allowed=True` 是必需的：`vectorstore: VectorStore` 是任意自定义类，Pydantic 默认不接受。

`allowed_search_types` 是 `ClassVar[Collection[str]]` 三元组——所以子类可以通过改这个 tuple 扩展允许的检索类型，而不必改校验逻辑。这是一个「用类属性做配置点」的做法。

**MMR 的算法是「贪心 + 双重相似度」。** `maximal_marginal_relevance` 七步：

1. `if min(k, len(embedding_list)) <= 0: return []` —— 提前返回空。
2. `query_embedding.ndim == 1` 时 `expand_dims(axis=0)` 变成一行矩阵。
3. `similarity_to_query = _cosine_similarity(query_embedding, embedding_list)[0]` —— 取第 0 行，即每个候选与查询的余弦相似度。
4. `most_similar = argmax(...)`，放进 `idxs`，`selected` 初始化为那一条。
5. 循环直到 `len(idxs) == min(k, len(embedding_list))`：
   - `similarity_to_selected = _cosine_similarity(embedding_list, selected)` —— 每个候选与**已选集合**的相似度矩阵。
   - 对每个未选中的 `i`：`redundant_score = max(similarity_to_selected[i])`（与已选中最像的那一个的相似度）。
   - `equation_score = lambda_mult * query_score - (1 - lambda_mult) * redundant_score`
   - 取最大的那个加入。
6. `selected = np.append(selected, [embedding_list[idx_to_add]], axis=0)`。

所以公式正是经典的 MMR：`λ·sim(q,d) - (1-λ)·max_{s∈S} sim(d,s)`。`lambda_mult=1` 时退化成纯相似度检索（`1-λ=0`），`=0` 时变成纯多样性。

一个实现细节值得记：`redundant_score` 用 `max` 而不是「与质心的距离」——所以它惩罚的是「与**已选中的某一条**重复」，而不是「与已选集合整体重复」。这是定义上的选择（经典 MMR 就是这么定义的），后果是「一组都相似但每两条之间不那么像的」结果仍会被一起选中。

**`_cosine_similarity` 有三级实现与一处 NaN/Inf 处理。** 优先用 `simsimd`（`1 - cdist(metric="cosine")`），否则退回 `numpy`（`dot(x, y.T) / outer(norms)`）。

numpy 路径里有四步值得看：

- `np.any(np.isnan(...))` 与 `np.any(np.isinf(...))` 各打一条 `RuntimeWarning`——所以坏数据被**报告**而不是静默。两条警告的措辞都是「输入里发现 NaN/Inf，可能得到意外的返回」。
- 除法用 `with np.errstate(divide="ignore", invalid="ignore")` 关掉 numpy 自己的除零警告，因为下面要手工处理。
- `if np.isnan(similarity).all()` 则 `raise ValueError("NaN values found, please remove the NaN values and try again")` ——**全 NaN 才报错**，部分 NaN 被下一步清成 0。
- `similarity[np.isnan(similarity) | np.isinf(similarity)] = 0.0` ——零向量造成的 0/0 会被清成 0.0（即「不相似」）。

这个层次是有讲究的：零向量在真实数据里会出现（嵌入服务失败、空文档），把它当「不相似」比当「错误」更符合使用者的期待；但若整个结果都是 NaN，那说明所有向量都有问题，此时报错才能让人发现。`from None` 用在 `raise ... from None` 里，抑制原始异常链，让这条 ValueError 看起来就是根因。

**`_cosine_similarity` 提前返回空矩阵。** `if len(x) == 0 or len(y) == 0: return np.array([[]])` ——注意返回的形状是 `1×0` 而不是 `0×k`。所以调用方若对空集合做 `[0]` 索引会拿到 `array([])`，`argmax` 会失败。这个边界在 `maximal_marginal_relevance` 里被前面的 `min(k, len(embedding_list)) <= 0` 拦住了，但 `_cosine_similarity` 单独被调用时仍有这个形状不一致。

**`delete` 的三态返回。** `def delete(self, ids=None, **kwargs) -> bool | None`，docstring：「`True` 表示删除成功、`False` 否则、`None` 表示未实现」。基类实现是 `raise NotImplementedError("delete method must be implemented by subclass.")`。

这个 `bool | None` 的三态与 g01 里 `_delete` 的判定直接对应：那边只在显式 `False` 时判失败、`None` 视为「不知道但没报错」。所以这里的「未实现」也有明确的语义位置——它是「我不知道」，不是「失败」。

`delete(ids=None)` 的 docstring 说「若 `None` 则删除全部」。这与 `DocumentIndex.delete` 那条「不带参数调用应当抛 ValueError」正好相反。两套契约对同一个「删全部」的危险操作给出了不同答案，值得注意——`VectorStore` 更老、更宽松，`DocumentIndex` 是后来加的 beta 抽象，选择了更严格的方向。

## 关键取舍

**源码事实：** 必选抽象只有 `similarity_search`；`similarity_search_with_score`、`max_marginal_relevance_search`、`delete` 等在基类直接 `raise NotImplementedError`。

**源码事实：** `add_texts` 用「子类是否覆盖了 `add_documents`」判断能否走默认路径；`aadd_documents` 用「子类是否覆盖了 `add_texts`」判断，两者互补避免互调。

**源码事实：** `_select_relevance_score_fn` 是必须覆盖的，docstring 明写「VectorStore 应当定义自己的相关性方法」。

**源码事实：** 欧氏归一化假定单位范数嵌入（分母 `sqrt(2)`）；内积归一化按正负分两段。

**源码事实：** `_cosine_similarity` 优先 `simsimd`、退回 numpy；部分 NaN 清成 0、全 NaN 报错。

**源码事实：** MMR 返回下标而非向量；`redundant_score` 用与已选单条的最大相似度。

**源码事实：** `VectorStoreRetriever` 用 `model_validator(mode="before")` 校验 `search_type` 与 `score_threshold`。

**源码事实：** 异步面普遍是 `run_in_executor` 包同步，注释自述为「临时变通」。

**架构推论：** 「极少必选 + 其余报错而非缺失」这个契约形态，是在「广泛兼容第三方向量库」的前提下最务实的选择。若把十几个方法都标 `abstractmethod`，每个第三方库都得实现全套（哪怕只支持最基础的 add/search），适配成本会高到没人愿意接。现在只强求一个检索方法，其余按需覆盖——代价是「调用了未实现的方法」只能在**运行时**发现，而不是在实例化时。

**架构推论：** 「相关性归一必须由子类选择」这条拒绝默认，是这一层最有判断力的地方。任何相似度分数一旦被错误地归一化，它看起来仍然是一个 `[0,1]` 的数——从「明显错误」退化成「看起来正常但没意义」，这是最难排查的一类 bug。所以基类宁可提供一个必须覆盖的抽象方法，也不给一个「猜一个吧」的默认。

**架构推论：** MMR 因为要「两两比较」，在客户端做必然是全量拉取（`fetch_k`）后重排。这就是为什么 `fetch_k`（20）比 `k` 大——不先取一批候选，MMR 无从选择。而在服务端做 MMR 的向量库可以把这一步下推，但那样就失去了「客户端统一的多样性语义」。这个取舍在接口上体现为「基类提供了默认实现（会拉 `fetch_k` 条）但允许覆盖」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/vectorstores/base.py` | `VectorStore` | 向量库抽象，必选仅相似度检索 |
| `libs/core/langchain_core/vectorstores/base.py` | `VectorStore.add_texts` | 按子类是否覆盖 `add_documents` 决定默认路径 |
| `libs/core/langchain_core/vectorstores/base.py` | `VectorStore.add_documents` | 与 `add_texts` 互补的委托路由 |
| `libs/core/langchain_core/vectorstores/base.py` | `VectorStore._cosine_relevance_score_fn` | 余弦距离到相似度的归一 |
| `libs/core/langchain_core/vectorstores/base.py` | `VectorStore._euclidean_relevance_score_fn` | 单位范数假设下的欧氏归一 |
| `libs/core/langchain_core/vectorstores/base.py` | `VectorStore._select_relevance_score_fn` | 必须由子类指定的度量选择 |
| `libs/core/langchain_core/vectorstores/base.py` | `VectorStore.similarity_search_with_relevance_scores` | 基类固定阈值过滤层 |
| `libs/core/langchain_core/vectorstores/base.py` | `VectorStore.search` | 三种 search_type 的分派 |
| `libs/core/langchain_core/vectorstores/base.py` | `VectorStore.delete` | 返回 `bool` 或 `None` 的三态删除 |
| `libs/core/langchain_core/vectorstores/base.py` | `VectorStore.as_retriever` | 包装成检索器并带自动 tags |
| `libs/core/langchain_core/vectorstores/base.py` | `VectorStoreRetriever` | 检索器三字段与 before 阶段校验 |
| `libs/core/langchain_core/vectorstores/utils.py` | `maximal_marginal_relevance` | 贪心 MMR，返回下标 |
| `libs/core/langchain_core/vectorstores/utils.py` | `_cosine_similarity` | simsimd/numpy 双实现与 NaN 分级处理 |

## 参考

- [langchain 源码：vectorstores/base.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/vectorstores/base.py)
- [langchain API 参考：VectorStore](https://python.langchain.com/api_reference/core/vectorstores/langchain_core.vectorstores.VectorStore.html)
- [Carbonell & Goldstein：Maximal Marginal Relevance 原论文](https://dl.acm.org/doi/10.1145/290941.291025)

## 与主核 / AI 的关系

主核里最接近的是「索引访问方法（AM）的接口」：一个索引只需要实现少数必选操作（插入、按条件取、扫描），其他（排序、聚合、距离计算）由上层算子做。这一页的分层与那个模式一致：`VectorStore` 只强求相似度检索，其余都在基类给默认或要求子类声明。

差别在于「错误发现时机」。主核的 AM 接口是 C 函数指针表，缺一个就是构建时链接失败；这里的契约是 Python ABC，只对 `similarity_search` 强制，其余方法在基类里 `raise NotImplementedError` ——所以缺实现是**运行时**错误。这是 Python 生态的普遍取舍：为了允许「部分实现」的适配器，放弃了实例化时的完备性检查。

与 g01（索引 API）的分工：g01 消费的是 `VectorStore` 的 `add_documents` / `delete` / `get_by_ids` 三个方法；这一页定义它们。而 `RecordManager`（g02）是第三方簿记，不属于 `VectorStore` ——这正是 g02 doc 里那条「记录管理器与向量库分开实现」的接口体现。

与 `similarity_score_threshold` 的设计对照主核的「代价门限」。两者都在做「只接受足够好的结果」，但主核的门限基于代价估计（可以精确计算），这里的门限基于**归一化后的相似度**——而归一化本身是可能错的。这是「AI 检索」与「数据库查询」的一个根本区别：后者的排序键有明确的单位与意义，前者的「相似」是一个由嵌入模型定义、又被归一函数二次改写的量。

对 AI 工程最实用的一条是「相关性归一必须显式声明」。多向量库混合使用时，最常见的隐性 bug 是「A 库返回距离、B 库返回相似度，都被当成同一尺度比较」。基类强制子类实现 `_select_relevance_score_fn`，等于强制每种子类回答一次「你返回的数字是什么意思」。这条规矩值得照抄——任何要跨实现比较的分数都需要一个显式的「尺度声明」环节。

另一条是「用方法身份做能力探测」的教训。`add_texts` 那个 `type(self).add_documents != VectorStore.add_documents` 的判断省掉了一堆样板，但它把「实现痕迹」当成了「能力声明」，一旦有人为了别的目的覆盖 `add_documents`，探测就失效。当一个基类想「可选地提供默认实现」时，明确的布尔能力位或独立的 `supports_*` 方法，比比对方法身份更稳。

最后一条是 `_cosine_similarity` 的 NaN 分级：部分 NaN 清洗成 0、全 NaN 报错。这是处理「脏数据」的一个好范式——单条坏数据不该让整批失败（那会让使用者倾向于加个 `try/except` 把所有错误吞掉），但全部坏掉必须让使用者知道。判据是：这个坏值是「个别异常」还是「系统性故障」？
