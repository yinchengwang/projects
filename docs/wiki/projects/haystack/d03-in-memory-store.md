---
project: haystack
feature: in-memory-store
title: 内存文档存储与 BM25
page_type: feature
schema_version: 1
refs:
  - path: haystack/document_stores/in_memory/document_store.py
    symbols: [InMemoryDocumentStore, BM25DocumentStats]
  - path: haystack/document_stores/in_memory/document_store.py
    symbols: [_synchronized]
---

# 内存文档存储与 BM25

## 一句话职责

`InMemoryDocumentStore` 是 d01 契约的内存实现：用四个字典存文档与 BM25 的增量统计量，用一把 per-index 的 `RLock` 保证线程安全，用 `_synchronized` 装饰器把「取锁」这件事从每个方法体里挪走；BM25 支持 Okapi / L / Plus 三种变体，而统计量的增减全是增量维护的。

## 机制是什么

内存后端要解决的核心问题不是「存」，而是**「存 + 算检索分」的一致性**。BM25 的打分需要四个全局量：

| 量 | 含义 | 维护方式 |
|---|---|---|
| 每文档的 `freq_token` + `doc_len` | 词频与长度 | 写入时算一次、存起来 |
| `n_corpus` | 有内容的文档数 | `len(self._bm25_attr)` |
| `_avg_doc_len` | 平均文档长度 | **增量递推** |
| `_freq_vocab_for_idf` | 每个词出现在多少个文档里 | **增量增减** |

后两个不能每次重算（那是 O(全库)），所以必须增量维护。而增量维护的代价是**每一步都必须精确可逆**——删一个文档要把它对平均长度与词表频次的贡献完整减去。这就是为什么 `write_documents` 在覆盖写之前要先 `delete_documents`：

```python
# Since the statistics are updated in an incremental manner,
# we need to explicitly remove the existing document to revert
# the statistics before updating them with the new document.
if document.id in self.storage.keys():
    self.delete_documents([document.id])
```

注释把这条约束说清楚了：**统计量是增量的，所以覆盖必须先回滚**。

第二个问题是线程安全。用一把 `RLock`（可重入锁）而不是普通 `Lock`，因为覆盖写会在持锁状态下调 `delete_documents`——而那个方法自己也带 `_synchronized`，所以要能重入。

## 怎么实现的

**`_synchronized` 是一个参数保持签名的装饰器，而它的实现依赖 `ParamSpec`。**

```python
_P = ParamSpec("_P")
_T = TypeVar("_T")

def _synchronized(
    method: Callable[Concatenate["InMemoryDocumentStore", _P], _T],
) -> Callable[Concatenate["InMemoryDocumentStore", _P], _T]:
    @wraps(method)
    def locked(self: "InMemoryDocumentStore", *args: _P.args, **kwargs: _P.kwargs) -> _T:
        # Overwrites call delete_documents while holding the same index lock.
        with self._lock:
            return method(self, *args, **kwargs)

    return locked
```

三处：

1. **`Concatenate[Store, _P]`** —— 这个类型表达的是「第一个参数固定是 `Store`，其余是 `_P`」。所以装饰后的方法签名在静态类型检查下完全保留。`*args: _P.args, **kwargs: _P.kwargs` 是它的运行期对应写法。

2. **注释解释了为什么必须是 `RLock`**：「Overwrites call `delete_documents` while holding the same index lock」。若用普通 `Lock`，`write_documents` 内部调 `delete_documents` 会死锁。

3. **`@wraps(method)`** 保留 `__name__`/`__doc__`。所以 `help()` 显示的是原方法的文档字符串——这对一个「契约式」的类很重要（d01 的方法文档要能读到）。

注意**并非所有方法都带这个装饰器**：`count_documents` 没有。

```python
def count_documents(self) -> int:
    return len(self.storage.keys())
```

**不加锁读 `len()` 是有意的**：`dict` 的 `len()` 在 CPython 里是原子操作，且「长度」本身是一个近似值就够用的指标。所以这里接受了「可能读到写入中途的长度」这个弱一致。

而 `filter_documents` 加了锁，因为它要遍历 —— 遍历中途被改会在 CPython 抛 `RuntimeError: dictionary changed size during iteration`。所以**锁的范围是按「操作是否可原子完成」决定的**，而不是一律加。

**存储的两种形态：进程级共享 vs 实例局部，而 getter 在两者间切换。**

```python
_STORAGES: dict[str, dict[str, Document]] = {}
_BM25_STATS_STORAGES: dict[str, dict[str, BM25DocumentStats]] = {}
_AVERAGE_DOC_LEN_STORAGES: dict[str, float] = {}
_FREQ_VOCAB_FOR_IDF_STORAGES: dict[str, Counter] = {}
_STORAGE_LOCKS: dict[str, RLock] = {}
```

四个数据字典 + 一个锁字典，全部**按 `index` 键**。所以「共享」的实现是「同一 index 的实例看同一份全局字典」。

```python
@property
def storage(self) -> dict[str, Document]:
    return _STORAGES[self.index] if self._shared else self._local_storage
```

四处：

1. **`storage` 是公开属性**（不是 `_storage`），文档说「Utility property that returns the storage used by this instance」。所以它既是内部实现，也是给测试/调试用的接口。

2. **非共享形态用 `_local_*` 四个实例属性**。所以 `__init__` 里有一对分支：

```python
if self._shared:
    with self._lock:
        if self.index not in _STORAGES:
            _STORAGES[self.index] = {}
            _BM25_STATS_STORAGES[self.index] = {}
            _AVERAGE_DOC_LEN_STORAGES[self.index] = 0.0
            _FREQ_VOCAB_FOR_IDF_STORAGES[self.index] = Counter()
else:
    self._local_storage: dict[str, Document] = {}
    self._local_bm25_attr: dict[str, BM25DocumentStats] = {}
    self._local_avg_doc_len: float = 0.0
    self._local_freq_vocab_for_idf: Counter = Counter()
```

3. **共享形态的初始化在锁内**（`with self._lock:`），且用 `if self.index not in _STORAGES` 做「只在第一次创建」。所以两个实例同时用同一个 index 初始化时，只有一方真正创建字典。

   注意 `_STORAGE_LOCKS` 的初始化**不在锁内**（因为锁本身要先建）：

```python
self._lock = _STORAGE_LOCKS.setdefault(self.index, RLock()) if shared else RLock()
```

   `dict.setdefault` 在 CPython 里是原子的（GIL 保护单字节码），所以这个「先有锁」的鸡生蛋问题被这样绕过。但严格说这不是跨实现保证的原子操作——在 free-threaded Python 上会有竞态。

4. **`_avg_doc_len` 有一个 setter**，因为增量维护需要写它：

```python
@property
def _avg_doc_len(self) -> float:
    return _AVERAGE_DOC_LEN_STORAGES[self.index] if self._shared else self._local_avg_doc_len

@_avg_doc_len.setter
def _avg_doc_len(self, value: float) -> None:
    if self._shared:
        _AVERAGE_DOC_LEN_STORAGES[self.index] = value
    else:
        self._local_avg_doc_len = value
```

   而 `_bm25_attr` 与 `_freq_vocab_for_idf` **只有 getter**——它们的修改通过字典/`Counter` 本身的方法（`pop`/`update`/`del`）完成，不需要整体替换。

   这是一处**微妙的不对称**：`_avg_doc_len` 是标量所以需要 setter，其余是可变容器所以不需要。而 `delete_all_documents` 需要**整体替换**所有四个——那里它直接操作全局字典而不是通过属性：

```python
if self._shared:
    _STORAGES[self.index] = {}
    _BM25_STATS_STORAGES[self.index] = {}
    _AVERAGE_DOC_LEN_STORAGES[self.index] = 0.0
    _FREQ_VOCAB_FOR_IDF_STORAGES[self.index] = Counter()
else:
    self._local_storage = {}
    ...
```

   注释解释了为什么不 `clear()`：「Clearing the documents replaces the data dictionaries, but must preserve the lock shared by existing instances」（在 `_STORAGE_LOCKS` 上方）。即**锁不能换**——否则一个持有旧锁的线程与新实例用的新锁互斥失效。

**`shared=True` 是默认值，而文档明确警告它的内存后果。**

```
:param shared: Whether the documents live in process-global storage shared across instances using the same
    index (True, the default), or are kept instance-local and freed when this instance is garbage collected
    (False). Shared storage persists for the lifetime of the process, so prefer `shared=False` for stores
    that are created frequently (for example per request) to avoid unbounded memory growth.
```

**这是本页最实用的一条文档**：默认值 `True` 会让「每次请求建一个 store」的用法**无限累积内存**（全局字典永不释放）。而默认 `True` 的理由可以从 `index` 参数推断（「When `shared` is True, instances using the same index share the same documents」）——即默认行为是「同 index 共享」，这对「多处声明同一个 store」的场景更方便。

但默认值与最常见的危险用法（每请求新建）冲突。所以文档在这里承担了「警告默认值」的角色。

**`write_documents` 的五步，其中统计量更新是增量递推。**

```python
if (not isinstance(documents, Iterable) or isinstance(documents, str)
    or any(not isinstance(doc, Document) for doc in documents)):
    raise ValueError("Please provide a list of Documents.")

if policy == DuplicatePolicy.NONE:
    policy = DuplicatePolicy.FAIL

written_documents = len(documents)
for document in documents:
    if policy != DuplicatePolicy.OVERWRITE and document.id in self.storage.keys():
        if policy == DuplicatePolicy.FAIL:
            raise DuplicateDocumentError(f"ID '{document.id}' already exists.")
        if policy == DuplicatePolicy.SKIP:
            logger.warning("ID '{document_id}' already exists", document_id=document.id)
            written_documents -= 1
            continue

    if document.id in self.storage.keys():
        self.delete_documents([document.id])

    self.storage[document.id] = document

    if document.content is None:
        continue

    tokens = self._tokenize_bm25(document.content)
    self._bm25_attr[document.id] = BM25DocumentStats(Counter(tokens), len(tokens))
    self._freq_vocab_for_idf.update(set(tokens))
    n_docs = len(self._bm25_attr)
    self._avg_doc_len = (len(tokens) + self._avg_doc_len * (n_docs - 1)) / n_docs
return written_documents
```

六处：

1. **类型检查用 `Iterable` + 排除 `str`**。因为 `str` 也是 `Iterable`，所以要显式排除（否则传一个字符串会被逐字符检查然后因为不是 `Document` 而报错——报错方式一样，但先排除让意图清楚）。

   **注意它接受任何 `Iterable` 而不是 `list`**，所以生成器也能传。但后面 `len(documents)` 会失败——生成器没有 `len`。所以实际只支持有 `len` 的 iterable（`list`/`tuple`）。这是一个「检查与使用不匹配」的小缺口。

2. **`policy == NONE` 解析成 `FAIL`** —— 与 d01 里那条「behaviour depends on the Document Store」对应，这里做出了选择。

3. **`policy != OVERWRITE` 才检查重复**。所以 `OVERWRITE` 时跳过检查、直接进后面的删旧+写新。

4. **`written_documents = len(documents)` 先设成输入数，`SKIP` 时递减**。这符合 d01 的返回值语义（`OVERWRITE` 恒等于输入数、`SKIP` 可能更小）。

   注意 **`FAIL` 抛错时 `written_documents` 已经在循环里累减过**（若之前有 SKIP）——但抛错让返回值无意义，所以不影响。

5. **覆盖时先 `delete_documents` 再写** —— 这是统计量可逆性要求的直接体现（前面引过注释）。

6. **`if document.content is None: continue`** —— 无内容文档不进 BM25 统计。注释在 `bm25_retrieval` 里解释了理由：「BM25 retrieval only searches documents with content, so only they count toward N and avgdl」。

   而平均长度的递推公式是：

```python
n_docs = len(self._bm25_attr)
self._avg_doc_len = (len(tokens) + self._avg_doc_len * (n_docs - 1)) / n_docs
```

   即「新文档长度 + 旧平均 × (旧数量)」除以新数量。注意 `self._bm25_attr[document.id]` **已经在上一行赋值了**，所以 `len(self._bm25_attr)` **已经包含新文档**。于是 `n_docs - 1` 就是旧数量——公式正确。

   这是一个**顺序敏感的**写法：若把赋值放到后面，公式就错了。源码没有注释说明这一点。

**`delete_documents` 的统计量回滚是本页最细的一段。**

```python
for doc_id in document_ids:
    if doc_id not in self.storage.keys():
        continue
    del self.storage[doc_id]

    doc_stats = self._bm25_attr.pop(doc_id, None)
    if doc_stats is None:
        continue
    freq = doc_stats.freq_token
    doc_len = doc_stats.doc_len

    self._freq_vocab_for_idf.subtract(Counter(freq.keys()))
    for token in freq:
        if self._freq_vocab_for_idf[token] <= 0:
            del self._freq_vocab_for_idf[token]
    try:
        self._avg_doc_len = (self._avg_doc_len * (len(self._bm25_attr) + 1) - doc_len) / len(self._bm25_attr)
    except ZeroDivisionError:
        self._avg_doc_len = 0
```

五处：

1. **`if doc_id not in self.storage: continue`** —— 缺失的 id 被**跳过**，不抛错。这与 d01 里「`delete_documents` 声明任何一个 id 不存在就抛 `MissingDocumentError`」**不一致**——内存后端没有遵守契约。

   这个不一致是实质性的：客户端代码若依赖契约（捕获 `MissingDocumentError`），在内存后端上永远等不到。反过来若依赖「幂等」（重复删不报错），换到别的后端可能炸。

2. **`self._bm25_attr.pop(doc_id, None)` 后判 `None` 就 `continue`** —— 所以无内容文档（没进统计量）在这里也走这条早退。

3. **`_freq_vocab_for_idf.subtract(Counter(freq.keys()))`** —— 注意 `subtract` 接受 `Counter`，而这里构造的是「键集合 → 计数 1」。所以每个出现在文档里的词，全局文档频次减 1。

   **用 `freq.keys()` 而不是 `freq`** 是关键：`freq` 是 `{token: 词频}`，而 IDF 需要的是「有多少个文档包含这个词」。所以每个词只减 1 次——用 `keys()` 去重。

   而写入时对应的是 `self._freq_vocab_for_idf.update(set(tokens))`——也是「每个词一次」。两处对称。

4. **`if self._freq_vocab_for_idf[token] <= 0: del ...`** —— 减到 0 的词被**从字典里删掉**而不是留成 0。这一步是必要的：`Counter` 的 `subtract` 会留下 0 与负数项，而 `Counter` 的 `__getitem__` 对缺失键返回 0，所以「留 0」功能上等价——它删除是为了**不让字典无限增长**（一个词被删光后再也不出现，那个键会永久留着）。

   注意条件写的是 `<= 0` 而不是 `== 0`——防止浮点/逻辑异常导致负数残留。

5. **平均长度的回滚公式**：

```python
self._avg_doc_len = (self._avg_doc_len * (len(self._bm25_attr) + 1) - doc_len) / len(self._bm25_attr)
```

   这里 `self._bm25_attr.pop()` **已经执行**，所以 `len(self._bm25_attr)` 是**删除后的数量**。于是 `+1` 还原出删除前的数量。公式是「旧平均 × 旧数量 - 被删长度」除以新数量。

   **`ZeroDivisionError` 被用来判断「删掉最后一个有内容的文档」**：新数量为 0 → 除零 → 置 0。用异常做控制流，但它是这里最紧凑的写法（先判 `len == 0` 再算也可以，但要多一行）。

   **这个异常捕获的范围值得注意**：`try` 只包了赋值那一行，所以只可能捕获除零。若公式里的某一项是 `None`（不可能，`_avg_doc_len` 总是 float），会抛 `TypeError` 而不会被误吞。

**`_validate_filters` 是一个极浅的检查，而它的形状暴露了与 d01 契约的落差。**

```python
@staticmethod
def _validate_filters(filters: dict[str, Any] | None) -> None:
    if filters and "operator" not in filters and "conditions" not in filters:
        raise ValueError("Invalid filter syntax. See https://docs.haystack.deepset.ai/docs/metadata-filtering for details.")
```

**它只检查「既没有 `operator` 也没有 `conditions`」** —— 即只拦住「看起来不像逻辑过滤器又不像比较过滤器」的输入。

但注意：一个合法的比较过滤器 `{"field": "meta.x", "operator": "==", "value": 1}` **有 `operator`**，所以通过。一个**缺少 `operator` 的比较过滤器**（`{"field": "meta.x", "value": 1}`）会被这里拦住并报「Invalid filter syntax」——而这个检查实际上是在拦「畸形比较过滤器」，措辞说的却是「filter syntax」。

真正深入的类型/运算符校验在 `document_matches_filter`（d02）里做，它抛 `FilterError` 而非 `ValueError`。所以**同一个方法的两个失败点抛不同异常类型**——调用方若只捕 `ValueError` 会漏掉 `FilterError`。

**`filter_documents` 的三处细节。**

```python
if filters:
    InMemoryDocumentStore._validate_filters(filters)
    docs = [doc for doc in self.storage.values()
            if document_matches_filter(filters=filters, document=doc, strict_datetime_comparison=...)]
else:
    docs = list(self.storage.values())

if not self.return_embedding:
    docs = [replace(doc, embedding=None) for doc in docs]
return docs
```

三处：

1. **`if filters:` 真值判断** —— 所以 `{}` 与 `None` 都走「返回全部」。这与「空过滤器 = 无条件」的语义一致。
2. **`self.return_embedding=False` 时用 `replace(doc, embedding=None)` 造副本**。所以**返回的文档是副本**，不改存储里的（注意 `replace` 会重算 `id`——但原 `doc.id` 非空所以保留，c01 讲过）。

   **`return_embedding=True` 时直接返回存储里的对象引用**——不复制。所以调用方修改返回的文档会影响存储。这是一个真实的共享可变状态问题，源码没有说明。

3. **`strict_datetime_comparison` 从实例透传给过滤器**（d02 的那个参数）。

**`bm25_retrieval` 有四处「为 Null 与 0 做特殊处理」。**

```python
content_type_filter = {"field": "content", "operator": "!=", "value": None}
if filters:
    if "operator" not in filters:
        raise ValueError("Invalid filter syntax. ...")
    filters = {"operator": "AND", "conditions": [content_type_filter, filters]}
else:
    filters = content_type_filter
```

四处：

1. **无条件追加「content 不为 None」的过滤**。因为无内容文档不进 BM25 统计，所以也不该被检索。而这里的 `!=None` 会走 d02 的 `_not_equal` → `not _equal(...)`。

   注意**这里的过滤器形态检查是内联的**（只查 `"operator"`），而不是调 `_validate_filters`（那个查两个键）。所以两处检查不一致。

2. **用户过滤器被包成 `AND(content!=None, user_filter)`** —— 所以固定条件与用户条件用 `AND` 组合。这与 d02 里 `combine_init_comparison_and_runtime_logical_filters` 的做法等价，但这里是**直接构造**而不走 `apply_filter_policy`。

   而它**要求用户过滤器必须有 `operator` 键**（否则报错）——因为包成 `AND` 的 `conditions` 里必须都是逻辑过滤器？不，`conditions` 里可以是比较过滤器（d02 的 `_and` 用 `_comparison_condition` 递归，它能处理比较字典）。所以这个检查**过严**：一个合法的比较过滤器（有 `operator`）能过，一个「缺 operator」的比较字典被拒——但后者本来也是畸形的。所以实际上这个检查是有效的。

   更值得注意的是：**一个 `field` 是 `content` 的用户过滤器和这里自动加的条件会同时存在**，两者都是 `!=None` 或更严——语义上不冲突。

3. **tokenless 语料的保护**：

```python
if self._avg_doc_len == 0:
    scored_documents = [(doc, 0.0) for doc in all_documents]
else:
    scored_documents = self.bm25_algorithm_inst(query, all_documents)
```

   注释解释了：「would make all three BM25 algorithms divide by zero during scoring」。所以平均长度为 0（全库无内容）时直接给全部候选 0 分，绕过算法。

4. **非正分的丢弃按算法区分**：

```python
drop_non_positive = self.bm25_algorithm != "BM25Okapi"
...
if drop_non_positive and score <= 0.0:
    continue
```

   注释解释了两件事：BM25Okapi **可能返回有意义的负分**（所以不能丢），而 BM25L/BM25Plus 的 0 分恰好表示「查询词一个都不在文档里」（所以该丢）。还引了 PR 编号 6889 作为依据。

   第三件事：**过滤在缩放之前**（「Filter on the raw score, because scaling maps 0 to 0.5」）。这是一个真实的顺序陷阱：`expit(0/8) = 0.5`，所以若先缩放再判 `<= 0`，所有分数都大于 0，过滤失效。

**分数缩放用 `expit`（sigmoid），而两个方法的缩放公式不同。**

BM25：
```python
if scale_score:
    score = expit(score / BM25_SCALING_FACTOR)
```

嵌入检索（在 `_compute_query_embedding_similarity_scores` 里）：
```python
if scale_score:
    if self.embedding_similarity_function == "dot_product":
        scores = [expit(float(score / DOT_PRODUCT_SCALING_FACTOR)) for score in scores]
    elif self.embedding_similarity_function == "cosine":
        scores = [(score + 1) / 2 for score in scores]
```

三处：

1. **BM25 与 dot_product 都用 `expit(x / factor)`**，但因子不同（8 vs 100）。注释解释了因子的选择：「Larger scaling factor decreases scaled scores... The defaults were chosen empirically. Increase the default if most unscaled scores are larger than expected (>30)」。

2. **cosine 用 `(score + 1) / 2`** —— 因为余弦值域是 `[-1, 1]`，线性映射到 `[0, 1]`。这是一个**精确**的映射，不需要 sigmoid。

   而 dot_product 值域无界，所以必须用 sigmoid 压扁。

3. **两个分支用不同的数学**说明「scale_score 的目标是 `[0,1]`」而不是「统一的变换」。所以缩放后的分数在不同相似度函数间**不可比**——它们都落在 `[0,1]`，但意义不同。

**`_compute_query_embedding_similarity_scores` 有三处错误翻译。**

```python
try:
    document_embeddings = np.array([doc.embedding for doc in documents])
except ValueError as e:
    if "inhomogeneous shape" in str(e):
        raise DocumentStoreError("The embedding size of all Documents should be the same. ...") from e
    raise e
```

```python
try:
    scores = np.dot(a=query_embedding, b=document_embeddings.T)[0].tolist()
except ValueError as e:
    if "shapes" in str(e) and "not aligned" in str(e):
        raise DocumentStoreError("The embedding size of the query should be the same as the embedding size of the Documents. ...") from e
    raise e
```

三处：

1. **两个 `except ValueError` 都靠匹配 numpy 的错误消息文本**（`"inhomogeneous shape"`、`"shapes"` + `"not aligned"`）来分类。这是一个脆弱的手法：numpy 改措辞就失效。但失败方向是安全的（匹配不上就 `raise e` 原样抛）。

2. **翻译成的消息很有用**：告诉用户「用同一个模型嵌入」。这是把「numpy 的数组形状错误」翻译成「用户能理解的原因」。

3. **`if query_embedding.ndim == 1: np.expand_dims(...)`** —— 处理「单个向量 vs 批量向量」的歧义。同样 `document_embeddings.ndim == 1` 也展开。所以一个空的文档列表会得到 `shape (0,)` 的数组，展开成 `(1, 0)`——然后 dot 会失败。

   注意**空文档列表的情形没有专门处理**。`np.array([])` 的 `ndim` 是 1，`expand_dims` 后是 `(1,0)`，与 `query_embedding` 的 `(1,d)` 做 dot 会因形状不匹配抛 `ValueError`——然后被翻译成「embedding size 不一致」。所以「没有文档」被报成「维度不一致」，错误消息误导。

   不过 `embedding_retrieval` 里在调用之前有 `if not documents: return []` 之类的检查吗？从 `bm25_retrieval` 看有 `if len(all_documents) == 0: return []`，而 `embedding_retrieval` 的分支我看过一半（`if filters:` 收集 `all_documents`），大概也有类似检查。所以这个缺口可能被上游挡住了。

**余弦相似度有零范数保护。**

```python
query_norm = np.linalg.norm(x=query_embedding, axis=1, keepdims=True)
document_norms = np.linalg.norm(x=document_embeddings, axis=1, keepdims=True)
query_embedding /= np.where(query_norm == 0.0, 1.0, query_norm)
document_embeddings /= np.where(document_norms == 0.0, 1.0, document_norms)
```

三处：

1. **`np.where(norm == 0.0, 1.0, norm)`** —— 零范数时用 1.0 代替，避免除零得 NaN。所以零向量的余弦相似度变成 0（点积为 0）而不是 NaN。

   注释说明了理由：「guard against zero-norm vectors (e.g. a zero embedding) which would otherwise divide by zero and yield NaN scores」。

2. **`keepdims=True`** 让结果形状是 `(n, 1)` 而不是 `(n,)`，这样广播除法成立。

3. **`document_embeddings /= ...` 就地修改**。而 `document_embeddings` 是 `np.array([...])` 新建的数组，所以安全。但 `query_embedding` 也是 `np.array(embedding)` 新建的（从 list 复制），所以也安全——**注意 `np.array` 对 list 会复制，对 ndarray 默认也会复制**。所以这里不会改到调用方传进来的 list/数组。

**异步方法全是「丢进单线程执行器」。**

```python
async def filter_documents_async(self, filters=None) -> list[Document]:
    return await asyncio.get_running_loop().run_in_executor(
        self.executor, lambda: self.filter_documents(filters=filters)
    )
```

三处：

1. **`run_in_executor(self.executor, ...)`** —— 而 `self.executor` 默认是 `max_workers=1` 的 `ThreadPoolExecutor`：

```python
self.executor = (ThreadPoolExecutor(thread_name_prefix=f"async-inmemory-docstore-executor-{id(self)}", max_workers=1)
                 if async_executor is None else async_executor)
```

   **单线程**。所以所有异步调用被串行化。对一个内存 store 这大概够用（操作本来快），但它意味着「并发读」也不会并行。

2. **`_owns_executor` 与清理**：

```python
self._owns_executor = async_executor is None

def __del__(self) -> None:
    if hasattr(self, "_owns_executor") and self._owns_executor and hasattr(self, "executor"):
        self.executor.shutdown(wait=True)

def shutdown(self) -> None:
    if self._owns_executor:
        self.executor.shutdown(wait=True)
```

   两处细节：
   - **`__del__` 里用 `hasattr` 双重检查** —— 因为 `__del__` 可能在 `__init__` 失败后被调用（此时属性不存在）。这是一个正确的防御。
   - **只关自己创建的 executor**（`_owns_executor`）—— 外部传的由外部负责。这是标准的所有权约定。

3. **`get_running_loop()` 而不是 `get_event_loop()`** —— 前者在没有运行中的循环时抛 `RuntimeError`，后者会创建/返回一个可能已废弃的循环。用前者是更严的选择。

**`save_to_disk` / `load_from_disk` 是契约之外的一对方法，而它们的顺序有讲究。**

```python
@_synchronized
def save_to_disk(self, path: str) -> None:
    data: dict[str, Any] = self.to_dict()
    data["documents"] = [doc.to_dict(flatten=False) for doc in self.storage.values()]
    with open(path, "w") as f:
        json.dump(data, f)
```

三处：

1. **`to_dict()` 只存配置**（d01 的两个序列化方法之一），文档数据被单独加在 `documents` 键下。
2. **`doc.to_dict(flatten=False)`** —— 与 c01 的 `__eq__` 一致选择（保留 meta 结构）。
3. **`json.dump(data, f)` 没有 `ensure_ascii=False`** —— 所以非 ASCII 字符会被转义成 `\uXXXX`。功能上正确（能读回来），但文件不可读。

`load_from_disk`：

```python
# Parse the documents first so an invalid snapshot leaves the index untouched
documents = [Document.from_dict(doc) for doc in data.pop("documents")]
cls_object = default_from_dict(cls, data)
# Hold the index lock so synchronized readers never see a partially restored index
with cls_object._lock:
    cls_object.delete_all_documents()
    cls_object.write_documents(documents=documents, policy=DuplicatePolicy.OVERWRITE)
```

四处：

1. **先解析文档再构造实例**，注释说明理由：「so an invalid snapshot leaves the index untouched」。所以畸形存档不会破坏已有数据。
2. **`data.pop("documents")` 移除那个键**，剩下的才传给 `default_from_dict`（否则未知参数会被拒绝——b02 讲过的校验）。
3. **显式取 `cls_object._lock` 再用 `_synchronized` 方法** —— 这是 RLock 的重入。注释：「so synchronized readers never see a partially restored index」。所以「清空 + 重写」这个两步组合被一个外层锁包成了原子操作。

   **这是一个正确的用法**：`delete_all_documents` 与 `write_documents` 各自加锁不够，因为它们之间有窗口。外层再包一层才让整个恢复过程原子。

4. **`with open(path)` 没有指定编码** —— 依赖平台默认编码。在 Windows 上默认是 cp936（中文环境）或 cp1252，而 `json.dump` 写的是 UTF-8 安全的内容（因为 `ensure_ascii=True` 转义了非 ASCII），所以读回来不会乱码。但若用户手工编辑过那个文件（加了中文），读的时候会按平台编码解码 → 可能 `UnicodeDecodeError`。

   **这是一个真实的跨平台缺陷**：写的是纯 ASCII 所以没问题，但读的时候没有指定 `encoding="utf-8"`。

**`update_by_filter` 是契约之外的能力，而它用的是「整条替换」。**

```python
for doc in matching:
    self.storage[doc.id] = replace(doc, meta={**doc.meta, **meta})
return len(matching)
```

两处：

1. **`replace(doc, meta=...)` 造新文档** —— `meta` 做浅合并（新值覆盖旧值）。所以「部分更新」在这里是**对 meta 的部分更新**，其他字段不变。
2. **它不更新 BM25 统计量** —— 因为 `content` 没变，统计量只依赖 `content`。所以这是安全的。

   **但若 `meta` 里包含影响过滤的键，过滤结果会立刻变**（因为过滤是每次现算的）。所以这里没有「索引需要重建」的问题——内存后端没有为 meta 建索引。

**`count_documents` 不加锁而 `filter_documents` 加锁，这个区别在两处 `*_async` 里也被继承。**

```python
async def count_documents_async(self) -> int:
    return len(self.storage.keys())
```

**异步版也是直接读、不走执行器**。所以它有同样的弱一致语义——且它**在事件循环线程里同步执行**（没有 `run_in_executor`）。对一个 `len()` 调用这没问题。

**`BM25DocumentStats` 是一个两字段 dataclass，但它的类型标注与使用不一致。**

```python
@dataclass
class BM25DocumentStats:
    freq_token: dict[str, int]
    doc_len: int
```

而构造时传的是 `Counter(tokens)`：

```python
self._bm25_attr[document.id] = BM25DocumentStats(Counter(tokens), len(tokens))
```

**`Counter` 是 `dict` 的子类**，所以类型兼容。但声明成 `dict[str, int]` 让读者以为用 `freq.get(token, 0)` 就够了——实际用到了 `Counter` 的 `keys()`/`subtract` 语义（虽然是在外面构造 `Counter(freq.keys())`）。

所以这是一个「类型标注宽于实际需要」的情形：字段声明成 `dict` 更通用，但代价是丢失了「它是 Counter」这个信息。

## 关键取舍

**源码事实：** BM25 的四个统计量（每文档 `freq_token`/`doc_len`、`n_corpus`、`_avg_doc_len`、`_freq_vocab_for_idf`）全部增量维护，因此**覆盖写必须先 `delete_documents` 回滚统计量**，注释明确写了这条约束。

**源码事实：** `_synchronized` 用 `Concatenate[Store, _P]` + `ParamSpec` 保留签名，`@wraps` 保留文档；必须是 `RLock` 因为覆盖写会在持锁时调 `delete_documents`。

**源码事实：** 锁的范围按「操作能否原子完成」决定：`count_documents` 不加锁（`len()` 原子），`filter_documents` 加锁（遍历会因并发修改抛 `RuntimeError`）。

**源码事实：** `shared=True`（默认）时数据在四个按 `index` 键的进程级字典里，`shared=False` 时在实例属性里；`_STORAGE_LOCKS.setdefault(index, RLock())` 在锁外创建（用 `setdefault` 规避鸡生蛋），`delete_all_documents` **替换字典而不 `clear()`** 以保住锁的同一性。

**源码事实：** 文档明确警告 `shared=True` 的默认值会让「频繁创建 store」的用法无限累积内存，建议这类场景用 `shared=False`。

**源码事实：** `write_documents` 接受任何 `Iterable`（排除 `str`）但随后用 `len(documents)`，所以实际只支持有长度的 iterable；`policy == NONE` 解析成 `FAIL`；平均长度的递推公式依赖「已先赋值 `_bm25_attr[document.id]`」这一顺序。

**源码事实：** `delete_documents` 对缺失 id **跳过而不抛错**——与 d01 契约「任何一个 id 不存在就抛 `MissingDocumentError`」不一致；`_freq_vocab_for_idf.subtract(Counter(freq.keys()))` 用 `keys()` 保证每词只减 1（与写入侧的 `update(set(tokens))` 对称），减到 `<= 0` 的词被删除以防字典无限增长；平均长度回滚用 `ZeroDivisionError` 判断「删掉最后一个有内容文档」。

**源码事实：** `_validate_filters` 只查「既无 `operator` 又无 `conditions`，抛 `ValueError`」，而深层校验在 `document_matches_filter` 抛 `FilterError`——同一方法的两个失败点异常类型不同。

**源码事实：** `filter_documents` 在 `return_embedding=False` 时用 `replace(doc, embedding=None)` 返回副本，`True` 时返回**存储里的对象引用**（可被调用方修改）。

**源码事实：** `bm25_retrieval` 无条件追加 `content != None` 的过滤（并把它与用户过滤器 `AND` 组合）；平均长度为 0 时直接给全 0 分以绕过除零；非正分丢弃按算法区分（BM25Okapi 保留负分、L/Plus 丢弃 ≤0，且**在缩放之前**判断，因为 `expit(0)=0.5`）；引了 PR 6889 作为依据。

**源码事实：** `scale_score` 对 BM25 与 dot_product 用 `expit(score/factor)`（因子 8 与 100，注释说明是经验值），对 cosine 用 `(score+1)/2`（精确线性映射）——所以缩放后的分数跨相似度函数不可比。

**源码事实：** `_compute_query_embedding_similarity_scores` 靠匹配 numpy 的错误消息文本（`"inhomogeneous shape"`、`"shapes"`+`"not aligned"`）翻译成两条「请用同一个模型嵌入」的提示；余弦有零范数保护（`np.where(norm == 0.0, 1.0, norm)`）；查询向量与文档向量都是 `np.array(...)` 复制，不会改调用方数据。

**源码事实：** 异步方法全部通过 `run_in_executor(self.executor, ...)` 走一个默认 `max_workers=1` 的执行器（串行化）；`__del__` 用 `hasattr` 双重检查后只关自己创建的 executor；用 `get_running_loop()` 而非 `get_event_loop()`。`count_documents_async` 例外——直接同步读、不进执行器。

**源码事实：** `save_to_disk` 用 `to_dict()`（只配置）+ 额外 `documents` 键，`json.dump` 无 `ensure_ascii=False`、`open(path, "w")` **无 `encoding`**；`load_from_disk` 先解析文档再建实例（畸形存档不破坏已有数据），`data.pop("documents")` 后才交给 `default_from_dict`，并**显式取锁把「清空 + 重写」包成原子操作**。

**架构推论：** 「增量维护统计量」这个决定的代价集中在**每一个写入路径都必须可逆**。`write_documents` 里那句「先删再写」不是冗余，而是这个约束的直接后果；`delete_documents` 里那段减法也不是可选优化，而是唯一的正确做法。这类设计的判据很明确：**统计量的更新成本 O(1) vs 重算成本 O(n)**，n 是文档数——只要 n 可能大，就必须增量；而一旦增量，任何「绕过删除直接改」的路径都是 bug。

**架构推论：** 内存后端的真实职责不是「生产用」，而是「**契约的参考实现**」。所以它写入了很多生产后端不需要的细节（BM25 三种变体、零范数保护、空语料保护），这些细节的价值在于**定义语义**：当别的后端行为不一致时，可以拿它当参照。反过来说，它**不遵守契约的那一处**（`delete_documents` 静默跳过缺失 id）就特别值得警惕——因为参考实现的不一致会被下游模仿。d01 声明「缺任何一个 id 都抛错」，这里 `continue`，两者必须择一。

**架构推论：** 「锁的范围按操作能否原子完成决定」是一条比「一律加锁」更好的判据。它接受 `count_documents` 的弱一致（读到中间值），换来零锁开销；同时保证遍历类操作的强一致（否则 CPython 会抛 `RuntimeError`）。这个区分的正确性依赖「`len()` 在 CPython 里是原子」这个实现细节——在 free-threaded Python 上需要重新审视。

**架构推论：** 「靠匹配 numpy 错误消息文本」来分类失败是脆弱的，但在这一层是可接受的：因为它只是把错误**翻译得更可读**，匹配不上时 `raise e` 保持原错误。所以失败方向安全。真正危险的版本是「匹配成功就吞掉异常」——那样一次 numpy 措辞改动会让所有形状错误变成静默失败。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/document_stores/in_memory/document_store.py` | `BM25_SCALING_FACTOR` / `DOT_PRODUCT_SCALING_FACTOR` | 经验缩放因子（8 / 100） |
| `haystack/document_stores/in_memory/document_store.py` | `_CJK_CHAR_CLASS` / `_COMBINING_MARK_CHAR_CLASS` | 分词正则的字符类（CJK 逐字、组合符归词） |
| `haystack/document_stores/in_memory/document_store.py` | `BM25DocumentStats` | 每文档的词频与长度 |
| `haystack/document_stores/in_memory/document_store.py` | `_STORAGES` 等五个模块字典 | 按 `index` 键的进程级共享存储与锁 |
| `haystack/document_stores/in_memory/document_store.py` | `_synchronized` | `ParamSpec` 保签名的 RLock 装饰器 |
| `haystack/document_stores/in_memory/document_store.py` | `InMemoryDocumentStore.__init__` | 共享/局部双形态 + executor 所有权 |
| `haystack/document_stores/in_memory/document_store.py` | `storage` / `_bm25_attr` / `_avg_doc_len` / `_freq_vocab_for_idf` | 四个存取器，只有标量有 setter |
| `haystack/document_stores/in_memory/document_store.py` | `_dispatch_bm25` | 按名字查表选算法，非法名报错 |
| `haystack/document_stores/in_memory/document_store.py` | `_tokenize_bm25` | NFC 归一 + 小写 + 正则 findall |
| `haystack/document_stores/in_memory/document_store.py` | `_score_bm25l` / `_score_bm25okapi` / `_score_bm25plus` | 三种 BM25 变体 |
| `haystack/document_stores/in_memory/document_store.py` | `to_dict` / `from_dict` | 只序列化配置（不包含文档） |
| `haystack/document_stores/in_memory/document_store.py` | `save_to_disk` / `load_from_disk` | JSON 落盘与原子恢复 |
| `haystack/document_stores/in_memory/document_store.py` | `count_documents` | 不加锁的 `len()` |
| `haystack/document_stores/in_memory/document_store.py` | `filter_documents` | 加锁遍历 + `return_embedding` 分支 |
| `haystack/document_stores/in_memory/document_store.py` | `write_documents` | 类型检查、`NONE→FAIL`、覆盖前删旧、统计量增量递推 |
| `haystack/document_stores/in_memory/document_store.py` | `delete_documents` | 统计量回滚（词表减 1、平均长度逆推、零除兜底） |
| `haystack/document_stores/in_memory/document_store.py` | `_validate_filters` | 极浅检查，抛 `ValueError` |
| `haystack/document_stores/in_memory/document_store.py` | `delete_all_documents` | 替换字典而非 `clear()`，保住锁同一性 |
| `haystack/document_stores/in_memory/document_store.py` | `update_by_filter` | meta 浅合并 + `replace`，不动 BM25 统计 |
| `haystack/document_stores/in_memory/document_store.py` | `bm25_retrieval` | 追加 content 过滤、空语料保护、按算法丢非正分、缩放前过滤 |
| `haystack/document_stores/in_memory/document_store.py` | `embedding_retrieval` | 嵌入检索 + 形状翻译 |
| `haystack/document_stores/in_memory/document_store.py` | `_compute_query_embedding_similarity_scores` | 点积/余弦 + 零范数保护 + 两种缩放 |
| `haystack/document_stores/in_memory/document_store.py` | `count_documents_async` / `filter_documents_async` | 全部走单线程执行器（`count_documents_async` 例外） |

## 参考

- [haystack 源码：in_memory/document_store.py](https://github.com/deepset-ai/haystack/blob/main/haystack/document_stores/in_memory/document_store.py)
- [haystack 文档：InMemoryDocumentStore](https://docs.haystack.deepset.ai/docs/inmemorydocumentstore)
- [rank_bm25：BM25 变体实现](https://github.com/dorianbrown/rank_bm25)
- [Python 文档：threading.RLock 与 ParamSpec](https://docs.python.org/3/library/threading.html#rlock-objects)

## 与主核 / AI 的关系

内存文档存储与主核（多模态数据库内核）的**堆表 + 全表扫描**是同构的：`storage` 是堆文件（按 id 哈希而非物理顺序），`filter_documents` 是顺序扫描（逐条求值过滤器），`bm25_retrieval` 是「扫描 + 打分 + top-k」。

最有价值的对照是**增量统计量与主核的 `pg_statistic`**。主核的统计信息（直方图、MCV）是**采样后定期重建**的，而这里每文档的词频是**精确且增量**的。差别来自规模：主核的表可能上亿行，只能采样；内存 store 的文档数在几万量级，可以精确维护。而主核的 `ANALYZE` 之所以能「不够精确也没关系」，是因为它只影响**计划选择**（选错计划就慢一点），不影响**查询结果**。而这里的词频直接影响 BM25 分数——**算错就是结果错**。所以「增量还是采样」的判据是**这份统计信息会不会进入结果**。

第二处对照是**锁的粒度**。主核用 buffer manager 的 per-buffer 锁 + 表级锁的多级方案；这里只有一把 per-index 的 `RLock`。所以「两个线程读同一个 store」也会互斥（`filter_documents` 都加锁）。对一个纯内存、操作极快的实现，这没问题；但它意味着**内存后端不支持真正的并发读**。

第三处是 `ZeroDivisionError` 当控制流这件事，与主核里「除零必须显式判断」的纪律对照。主核的表达式求值遇到除零会报错（`division_by_zero`）而不是吞掉，因为「零除」在 SQL 里是数据错误。而这里的零除是**结构性**的（「没有文档了」），用异常捕获是紧凑的写法。判据是**这个零除是否意味着数据错误**：意味着就报错，不意味着（是边界状态）就用异常或显式分支。

对 AI 工程的实际意义在于：内存后端是最常用的开发期后端，它的行为会成为用户对「文档存储」的认知基准。这份实现给出的三条经验可直接用：**增量维护的统计量必须让每一条写入路径都可逆**（覆盖写先删再写）、**锁的范围按「操作能否原子完成」决定**（`len()` 不加锁、遍历加锁）、以及**用异常做边界控制流要限定 `try` 的范围**（只包那一行，避免吞掉真正的类型错误）。
