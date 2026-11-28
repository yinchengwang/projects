---
project: haystack
feature: rankers
title: 重排器
page_type: feature
schema_version: 1
refs:
  - path: haystack/components/rankers/llm_ranker.py
    symbols: [LLMRanker]
  - path: haystack/components/rankers/meta_field.py
    symbols: [MetaFieldRanker]
  - path: haystack/components/rankers/lost_in_the_middle.py
    symbols: [LostInTheMiddleRanker]
  - path: haystack/components/rankers/max_sim.py
    symbols: [MaxSimRanker]
---

# 重排器

## 一句话职责

四个重排器，各自换一种「相关度」的定义：`LLMRanker` 让模型直接输出相关文档的序号，`MetaFieldRanker` 把元数据字段的排序与上游检索分数**融合**，`LostInTheMiddleRanker` 不动相关度只动**位置**（把最相关的放首尾），`MaxSimRanker` 用**多向量**（ColBERT 式）逐词比对。

## 机制是什么

| 类 | 相关度的来源 | 是否用 query | 是否改 `score` | 是否需要模型 |
|---|---|---|---|---|
| `LLMRanker` | 模型判断「哪些文档相关」 | **需要**（空则跳过重排） | 不改（只重排） | 是（chat generator） |
| `MetaFieldRanker` | 元数据字段值 | 不需要 | **改写**（融合后新分数） | 否 |
| `LostInTheMiddleRanker` | **不产生相关度**，只按输入的相对顺序重排 | 不需要 | 不改 | 否 |
| `MaxSimRanker` | 查询多向量与文档多向量的逐词最大内积之和 | **需要**（多向量） | **改写** | 否（只做矩阵运算） |

**四种「换一种定义」之间最重要的差别是「谁提供相关性判断」**：模型、元数据、上游的既有顺序、还是向量本身。这决定了它们各自能修什么问题。

## 怎么实现的

### 一、`LLMRanker`：让模型输出序号，而失败时降级为「不重排」

**提示模板把输出格式钉死成 JSON，且规则里有一条很容易被忽略的语义。**

```
Return valid JSON only, with this structure:
{
  "documents": [
    {"index": 1}
  ]
}

Rules:
- Rank documents from most relevant to least relevant for answering the query.
- Only include documents that are relevant to the query.
- Do not return or rank documents that are not relevant.
- If none are relevant, return {"documents": []}.
- Use only document indices from the provided documents.
- Do not repeat document indices.
- Do not include explanations or any text outside the JSON object.
```

**「只输出相关文档」这条让这个重排器同时承担了过滤职责** —— 一次调用既排了序又裁掉了不相关项。这就是它返回「至多 `top_k` 个」而不是「恰好 `top_k` 个」的原因。

**默认的 chat generator 直接绑定了 JSON Schema。**

```
return OpenAIResponsesChatGenerator(
    model="gpt-4.1-mini",
    generation_kwargs={"temperature": 0.0, "store": False, "text": {"format": {"type": "json_schema", "name": "document_ranking", "schema": {...}}}},
)
```

三处设计：`temperature=0.0`（重排要稳定）、`store=False`（不把请求留在服务端）、以及**结构化输出 schema**——`documents` 是对象数组、每项只有 `index: integer`、`additionalProperties: False`。

**用法说明里还写了 `store=False` 的理由是隐私**，而 schema 的作用是把「输出格式」从提示里的请求变成服务端的约束。

**提示变量的校验是一条精确的检查。**

```
self._prompt_builder = PromptBuilder(template=self.prompt, required_variables=["documents", "query"])
if set(self._prompt_builder.variables) != {"documents", "query"}:
    raise ValueError("prompt must include exactly the variables 'documents' and 'query'.")
```

**用 `set(...) != {...}` 而不是「包含」** —— 于是自定义提示里多一个变量也会报错。这比「允许多余的变量」严格，理由是模板里出现未提供的变量会导致渲染失败（而 `required_variables` 只保证这两个必有值）。

**`PromptBuilder` 的 `documents` 变量会被渲染成带编号的列表**，而编号是 **1 起**的（代码注释引用 Jinja 的 `loop.index` 是 1-based）。这解释了后面解析里的那个减一。

**`run` 的两级降级是这一页最值得记的部分。**

```
try:
    with _trace_chat_generator_run(self._chat_generator, {"messages": messages}) as span:
        result = self._chat_generator.run(messages=messages)
        span.set_content_tag("haystack.component.output", result)
except Exception as exc:
    if self.raise_on_failure: raise
    logger.warning("LLMRanker failed during chat generation. Returning the deduplicated input documents unranked. Error: {error}", error=exc)
    return {"documents": fallback_documents}

try:
    reply_text = self._get_reply_text(result)
    ranked_documents = self._rank_documents_from_reply(reply_text=reply_text, documents=deduplicated_documents)
except (TypeError, ValueError) as exc:
    if self.raise_on_failure: raise
    logger.warning("LLMRanker failed while processing the chat response. Returning the deduplicated input documents unranked. Error: {error}", error=exc)
    return {"documents": fallback_documents}
```

**两个独立的 `try`**，各自对应一个失败面：**生成失败**（网络、限流、模型拒答）与**响应处理失败**（没有 `replies`、JSON 坏了、没有有效索引）。

而两处降级都返回 `fallback_documents` —— 也就是**去重后的输入，原顺序，且不截断**。文档里明确写了「Not applied when the ranker falls back」：**降级时不应用 `top_k`**。

**这个选择是对的**：若降级时还截断，一个失败的模型调用会让下游拿到「前 k 个但没排序」的结果，看起来像正常输出。而不截断 + 原顺序，至少能让下游看见「全都在，只是没排」。

**`raise_on_failure` 默认 `False`** —— 也就是说**默认容忍失败**。这个默认值在重排场景是合理的：重排是「优化」而非「必要」，失败时不排序仍能出答案。而需要严格保证的调用方可以打开它。

**解析函数用一串「跳过而非报错」来容纳模型的格式偏差。**

```
for entry in ranked_entries:
    if not isinstance(entry, dict): raise TypeError("Expected each ranked document entry to be a JSON object.")
    document_index = entry.get("index")
    if not (type(document_index) is int or type(document_index) is str or (isinstance(document_index, float) and document_index.is_integer())):
        continue
    try:
        document_index = int(document_index)      # LLMs can return numeric indices as strings even when asked for integers.
    except ValueError:
        continue
    if document_index < 1 or document_index > len(documents):
        continue
    document = documents[document_index - 1]
    ranked_documents.append(document)
if not ranked_documents: raise ValueError("Ranking response did not contain any valid document indices.")
return ranked_documents
```

四种容错与一条硬校验：

1. **接受 `int` / `str` / 「整数值的 `float`」三种形态** —— 注释写明「即使要求整数，模型也可能返回字符串」。而 `type(x) is int` 而不是 `isinstance` 是为了排除 `bool`（`True` 是 `int` 的子类）。
2. **`int(document_index)` 失败就跳过**（比如 `"first"`）。
3. **越界索引跳过**（1-based，范围 `1..len`）。
4. **一个有效索引都没有时抛 `ValueError`** —— 这与「跳过个体」形成对比：**个体的格式偏差容忍，整体的无效不容忍**。

**而重复索引没有被剔除** —— 提示里要求「不要重复」，但代码不检查。于是模型若重复给出同一索引，那个文档会在结果里出现两次。这是一个**依赖提示而非代码保证**的不变量。

### 二、`MetaFieldRanker`：把两种排序融合，而 `weight` 是关键

**它的定位与其他三个不同：它不「重新判断相关性」，而是把「元数据字段的排序」与「上游检索的排序」按权重融合。**

```
weight=1.0  → 只看元数据排序
weight=0.5  → 两者等权
weight=0    → 完全禁用元数据排序，直接返回去重后的输入
```

而 `weight=0` 是**提前返回**，连校验都只做了参数检查：

```
if weight == 0: return {"documents": deduplicated_documents[:top_k]}
```

**两种融合模式，而它们的适用条件不同。**

```
ranking_mode: Literal["reciprocal_rank_fusion", "linear_score"] = "reciprocal_rank_fusion"
```

- **`reciprocal_rank_fusion`（默认）**：只用**排名**，不看分数。`_calculate_rrf(rank) * (1 - weight)` 给上游排序，`_calculate_rrf(rank) * weight` 给元数据排序。
- **`linear_score`**：用**分数**。文档里写明「只在上游返回 [0,1] 范围内的分数时使用」。

**`_calculate_rrf` 里那个 `k=61` 的注释很具体。**

```
The constant K is set to 61 (60 was suggested by the original paper, plus 1 as python lists are 0-based and the paper used 1-based ranking).
```

也就是说这里是**为了抵消 0-based 与 1-based 的差**而把论文里的 60 调成 61。这是一个「移植论文公式时必须处理的偏置」，而作者把它写在了文档注释里。

**`_calc_linear_score` 是一个线性归一。**

```
return (amount - rank) / amount
```

文档说明它的两个用途：降低离群值影响、产生落在 [0,1] 里且分布有意义的分数（与检索器/重排器的分数可比）。

**而 `linear_score` 模式对上游分数做了两重防御。**

```
score = float(0)
if document.score is None: logger.warning("The score wasn't provided; defaulting to 0.")
elif not 0 <= document.score <= 1: logger.warning("The score {score} for Document {document_id} is outside the [0,1] range; defaulting to 0", ...)
else: score = document.score
scores_map[document.id] += score * (1 - weight)
```

**越界的分数被当成 0 而不是被截断到边界。** 这是一个保守的选择：一个 5.0 的分数若被截成 1.0，会让那个文档拿到满分权重；当成 0 则相当于「这条记录不可信，忽略它的分数贡献」。而两次警告让问题可见。

**融合后所有文档的分数都被重写。**

```
scored_docs = [replace(doc, score=scores_map[doc.id]) for doc in documents]
return sorted(scored_docs, key=lambda doc: doc.score if doc.score else -1, reverse=True)
```

`replace` 返回新 `Document`（不改输入），除了 `score` 之外一切照旧——**包括 `id`**（于是 `id` 不变，去重键仍然有效）。

而排序键用 `doc.score if doc.score else -1` —— **`0` 与 `None` 都被当成 `-1`**。这与其它组件的 `-inf` 不同：这里 0 是一个可能的合法融合分数（元数据权重为 0 且上游分数为 0），把它降到 `-1` 会让它排到最后。**这是一个与 `e01`/`f01` 里 `-inf` 用法不一致的细节**。

**`docs_missing_meta_field` 的三条策略是这个组件最实用的部分。**

```
docs_with_meta_field = [doc for doc in deduplicated_documents if doc.meta.get(self.meta_field) is not None]
docs_missing_meta_field = [doc for doc in deduplicated_documents if doc.meta.get(self.meta_field) is None]
```

- `drop`：直接丢弃，且**只在有元数据的那批上做融合**（`_merge_rankings(docs_with_meta_field, sorted_documents, ...)`）——于是输出里没有缺元数据的文档。
- `top`：`docs_missing_meta_field + sorted_by_meta` 作为「元数据排序」那一侧，再与**全部**去重文档融合。
- `bottom`：`sorted_by_meta + docs_missing_meta_field`。

**注意 `drop` 分支与另两个分支传给 `_merge_rankings` 的第一个参数不同**（`docs_with_meta_field` vs `deduplicated_documents`）。这不是笔误：`drop` 时那批文档已不在候选里，用全集会让 `scores_map` 包含不存在的 id（虽然 `scored_docs` 只遍历第一个参数，所以结果是安全的，但这个不对称值得注意）。

**而「`None` 值算缺失」被显式注释了。**

```
# A None value can't be sorted against real values, so it is handled like a missing field
```

于是 `meta={"rating": None}` 与「没有 `rating` 键」走同一条路。这是必需的处理——否则 `sorted` 会在 `None` 与数值之间抛 `TypeError`。

**`meta_value_type` 支持把字符串解析成可比较的类型，而它的失败处理是「放弃解析」而不是报错。**

```
if not all(isinstance(doc.meta[self.meta_field], str) for doc in docs_with_meta_field):
    logger.warning("... not all of meta values ... are strings. Skipping parsing of the meta values. ...")
    return [d.meta[self.meta_field] for d in docs_with_meta_field]
...
try: meta_values = [parse_fn(d.meta[self.meta_field]) for d in docs_with_meta_field]
except ValueError as error: logger.warning("Tried to parse the meta values ... Skipping parsing of the meta values. ..."); meta_values = [...]
```

两处降级：**不是全部字符串**（混合类型无法统一解析）与**解析抛 `ValueError`**（比如 `"abc"` 转 `float`）。两处都退回未解析的值——于是 `sorted` 可能因类型混合再抛 `TypeError`，而那个错误由下面那条守卫接住。

**排序失败也有一条降级，而它按 `missing_meta` 决定拼装顺序。**

```
try:
    tuple_sorted_by_meta = sorted(tuple_parsed_meta_and_docs, key=lambda x: x[0], reverse=reverse)
except TypeError as error:
    logger.warning("Tried to sort Documents ... Returning the <top_k> Documents in their original order since meta field ranking is not possible.", ...)
    if missing_meta == "top": fallback_documents = docs_missing_meta_field + docs_with_meta_field
    elif missing_meta == "bottom": fallback_documents = docs_with_meta_field + docs_missing_meta_field
    else: fallback_documents = docs_with_meta_field
    return {"documents": fallback_documents[:top_k]}
```

**四层降级链值得整体记一遍**：类型混合/解析失败 → 退回未解析值 → 排序仍失败 → 退回原顺序（但仍按 `missing_meta` 策略拼装）。**每一层都保留了上一层的成果**，而不是一路退到底。

**「所有文档都缺元数据」也有一条路径。**

```
if len(docs_with_meta_field) == 0:
    logger.warning("... none of the provided Documents ... have this meta key. ... Applying the configured <missing_meta> policy instead of ranking.", ...)
    if missing_meta == "drop": return {"documents": []}
    return {"documents": deduplicated_documents[:top_k]}
```

**这条路径的警告文案把「策略仍然生效」说清楚了** —— 于是 `drop` 会得到空列表而不是「什么都不做」。

**最后那个 `_merge_rankings(add(docs_with_meta_field, sorted_documents, ...))` 调用让 `weight` 与 `top_k` 各自独立生效**：融合后排序，再截断。

### 三、`LostInTheMiddleRanker`：只改位置，不改分数

**它的插入算法只有三行，而结果值得推演。**

```
lost_in_the_middle_indices = [0]
for doc_idx in document_index[1:]:
    insertion_index = len(lost_in_the_middle_indices) // 2 + len(lost_in_the_middle_indices) % 2
    lost_in_the_middle_indices.insert(insertion_index, doc_idx)
```

**`n // 2 + n % 2` 就是 `ceil(n/2)`** —— 于是每次都插到「当前列表的中间偏后」。推演 5 个文档：

- `[0]`
- 插 1 到 `ceil(1/2)=1` → `[0, 1]`
- 插 2 到 `ceil(2/2)=1` → `[0, 2, 1]`
- 插 3 到 `ceil(3/2)=2` → `[0, 2, 3, 1]`
- 插 4 到 `ceil(4/2)=2` → `[0, 2, 4, 3, 1]`

最终顺序是 **0, 2, 4, 3, 1**。把它翻译回「位置 → 原始序号」：

| 位置 | 0 | 1 | 2 | 3 | 4 |
|---|---|---|---|---|---|
| 原始序号 | 0 | 2 | **4** | 3 | 1 |
| 相关性排名 | **第 1** | 第 3 | **第 5（最差）** | 第 4 | **第 2** |

也就是说：**最相关的（0）固定在最前、第二相关的（1）固定在最后、最不相关的（4）落在正中**；中间两格被「中段相关」的填充。3 个文档时结果同样是 `[第1, 第3, 第2]` —— 最差的正中。

**这个排布与论文描述的意图一致**：注意力在长上下文的首尾最强，于是「首」给最相关的、「尾」也留给强相关的，而把最弱的塞进中段。**注意它不是「对称分布高分」** ——最相关那一篇只有一份，只能放在一端；这端选了开头，因为它同时是最容易被读到的位置。

**输入顺序是前提，而它靠文档写清。**

```
LostInTheMiddleRanker assumes that some prior component in the pipeline has already ranked documents by relevance and requires no query as input but only documents. It is typically used as the last component before building a prompt for an LLM.
```

**它假设上游已经排好序**——所以它不排序，只重排位置。也正因为不需要 query，它能放在链路最末尾（在 prompt 构造之前）。

**`word_count_threshold` 是一个「按词数预算截断」的旋钮，而它的语义有几处需要读清。**

```
:param word_count_threshold: The cumulative word count threshold ... The document that causes the cumulative word count to reach or exceed the threshold is included in full, so the final word count may exceed the threshold. All subsequent documents are excluded. Words are counted using `str.split()`, not model tokens.
```

三个要点：

1. **触发越界的那一篇被完整包含** —— 于是最终词数**可能超过阈值**（不会在中途截断文档）。
2. **之后的全部排除。**
3. **用 `str.split()` 数，不是模型 token** —— 这是一个明确的能力边界声明。

**而它的实现顺序是先判第一篇、再在循环里判后续。**

```
if word_count_threshold and documents_to_reorder[0].content:
    word_count = len(documents_to_reorder[0].content.split())
    if word_count >= word_count_threshold: return {"documents": [documents_to_reorder[0]]}
for doc_idx in document_index[1:]:
    insertion_index = ...
    lost_in_the_middle_indices.insert(insertion_index, doc_idx)
    if word_count_threshold and documents_to_reorder[doc_idx].content:
        word_count += len(documents_to_reorder[doc_idx].content.split())
        if word_count >= word_count_threshold: break
```

**注意顺序：先插入再累加再判断。** 于是越界的那篇已经进了 `lost_in_the_middle_indices`，`break` 只是停掉后续——这与文档说明一致。

**而「第一篇就超阈值」有一条专门短路** —— 直接返回单元素列表，跳过整个重排（因为没有别的位置可放）。

**它对非文本文档直接报错。**

```
if any(doc.content is None for doc in documents):
    raise ValueError("Some provided documents are not textual; LostInTheMiddleRanker can process only text.")
```

理由很实际：`word_count_threshold` 要数字数，而多模态文档没有可数的 `content`。**报错而不是跳过**，因为「跳过」会让结果里少文档而调用方不知原因。

**`top_k` 与 `len(documents_to_reorder) == 1` 各有短路。**

```
documents_to_reorder = deduplicated_documents[:top_k] if top_k else deduplicated_documents
if len(documents_to_reorder) == 1: return {"documents": documents_to_reorder}
```

第二行是必需的——重排一个元素的列表没有意义，而循环里 `document_index[1:]` 为空，也能得到相同结果。这个短路只是省了对象构造。

### 四、`MaxSimRanker`：多向量的逐词比对

**它的打分公式是三行，而这是 ColBERT 的核心。**

```
similarities = query_vectors @ document_vectors.T
score = float(similarities.max(axis=1).sum())
```

`query_vectors` 形状是 `(q, d)`、`document_vectors` 是 `(p, d)`，于是 `query @ document.T` 得到 `(q, p)` 的相似度矩阵。

**`max(axis=1)` 沿「文档向量」方向取最大** —— 于是每个查询向量得到一个「它在文档里最匹配的那个词」的分数。再 `.sum()` 把 q 个查询向量的最佳匹配加起来。

**这个整体就是 MaxSim 的定义**：对每个查询向量，找出文档里最相似的那一项，把这些最大值求和。它比「单个向量的余弦」细得多——**一个多义词的不同义项可以分别匹配文档的不同部分**。

**维度校验是逐个文档做的，而错误消息很具体。**

```
if document_vectors.shape[1] != query_vectors.shape[1]:
    raise ValueError(f"multivector_embedding of document {document.id!r} has dimension {document_vectors.shape[1]}, but query_multivector_embedding has dimension {query_vectors.shape[1]}.")
```

**`{document.id!r}` 用了 `repr`** —— 于是 id 在消息里带引号，边界清楚（一个 id 若含空格也能看出范围）。

**`_prepare_vectors` 用三个条件一起判定矩阵合法性。**

```
matrix = np.asarray(multivector_embedding, dtype=np.float64)
if matrix.ndim != 2 or matrix.size == 0 or not np.isfinite(matrix).all():
    raise ValueError(f"{input_name} must be a non-empty matrix of finite numbers.")
```

- **`ndim != 2`** —— 一维数组（单个向量）与三维都会被拒；多向量必须是矩阵。
- **`size == 0`** —— 空矩阵被拒（否则 `max` 会作用在空轴上报错）。
- **`np.isfinite(matrix).all()`** —— **拒绝 `NaN` 与 `inf`**。

**`isfinite` 那条最值得记**：`NaN` 在比较里会让所有比较返回 `False`，于是 `max` 与 `sum` 会静默产出 `NaN` 分数，那个文档的排序位置完全不可预测。而 `inf` 会让它永远排第一。**在这里拒绝，比让污染传播到下游好。**

**而错误消息用 `{input_name}` 区分「查询侧」与「文档侧」**，调用方传的是 `f"multivector_embedding of document {document.id!r}"` —— 于是同一个函数的两处调用产出可分辨的消息。

**缺多向量嵌入的文档被跳过并汇总报告。**

```
for document in documents:
    if document.multivector_embedding is None: document_ids_without_embeddings.append(document.id)
    else: documents_with_embeddings.append(document)
if document_ids_without_embeddings:
    logger.warning("Skipping documents without multivector embeddings: {document_ids}", document_ids=document_ids_without_embeddings)
if not documents_with_embeddings: return {"documents": []}
```

**一次警告列出全部被跳过的 id**，而不是每个文档一条——这是日志的正确粒度（否则 1000 个缺嵌入的文档会刷 1000 行）。

**而「全部没有嵌入」时返回空列表而不是报错** —— 这是一个合理的选择：空结果是「没有可排的东西」，不是错误。

**它最后也重写分数。**

```
return {"documents": [replace(document, score=score) for document, score in scored_documents[:top_k]]}
```

`replace` 保留原 `id`，只换 `score`。于是下游看到的文档与输入是同一批（同 id、同内容），只是分数变成 MaxSim 值。

**它的典型用法是两阶段检索的第二阶段，而文档说明写清了这一分工。**

> 两个 embedder 分别准备稠密向量与多向量表示并存进 document store；查询时对应的 embedder 生成两种表示；**稠密检索器快速选出候选**，然后这个重排器用多向量做更细的比对。

**为什么需要两阶段**：多向量比对的成本是 `q × p` 次内积（每个文档），对全库做不现实；而稠密向量可以走 ANN 索引。于是「先粗筛再细排」是唯一可行的组织方式。

### 五、与 `MetaFieldGroupingRanker` 的分工

本页的 refs 只覆盖 `MetaFieldRanker`，但同目录里还有一个 `MetaFieldGroupingRanker`，而两者的差别正好说明「重排」有两种含义：

| | `MetaFieldRanker` | `MetaFieldGroupingRanker` |
|---|---|---|
| 目标 | **按元数据字段排序** | **按元数据字段分组** |
| 输出 | 单一按分数排序的列表 | 按 `group_by`、`subgroup_by` 分组的扁平列表 |
| 与其他排序的关系 | **融合**（`weight` 控制与上游排序的权重） | 不融合，只重排 |
| 缺字段的文档 | 有 `drop`/`top`/`bottom` 三策略 | **放在列表末尾** |

**「分组」是另一种重排需求**：把同源的块排在一起（比如同一个文档的 `split_id` 升序），让模型读到的是连贯上下文而不是打散的片段。而 `MetaFieldRanker` 的排序反而会把同源块打散（因为它按字段值排，同源块的字段值可能相同而顺序不定）。

**这个对照给出一条实用的判断**：若目的是「让模型读到连贯上下文」，用分组；若目的是「按业务权重（评分、日期、优先级）排」，用 `MetaFieldRanker`。

## 关键取舍

**源码事实：** `LLMRanker` 的提示要求「只输出相关文档」，于是一次调用同时完成排序与过滤——这就是它返回「至多 `top_k` 个」的原因。

**源码事实：** 它有两级独立降级（生成失败、响应处理失败），两处都返回**去重后的输入、原顺序、且不应用 `top_k`**；`raise_on_failure` 默认 `False`。

**源码事实：** 索引解析接受 `int` / `str` / 整数值 `float`（注释说明模型可能把整数返回成字符串），用 `type(x) is int` 排除 `bool`；个体偏差跳过、整体无有效索引才抛 `ValueError`。

**源码事实：** 提示要求「不要重复索引」，但**代码不校验重复**——重复的索引会让同一文档出现两次。

**源码事实：** `MetaFieldRanker` 的 `weight` 三档语义（1 只看元数据、0.5 等权、0 禁用），`weight=0` 提前返回去重后的输入。

**源码事实：** `_calculate_rrf` 的 `k=61` 而非论文的 60，注释说明这是为抵消 Python 0-based 与论文 1-based 的差。

**源码事实：** `linear_score` 模式对上游分数做两重防御（`None` 与越界都当成 0 并警告），**越界不截断到边界**——因为截断会让那条文档拿到虚高的权重。

**源码事实：** `None` 元数据值被当作缺字段处理（否则 `sorted` 会在 `None` 与实数间抛 `TypeError`）；`missing_meta` 的三策略里 `drop` 分支传给融合函数的第一个参数与另两个不同。

**源码事实：** `MetaFieldRanker` 有**四层降级链**：解析失败退回未解析值 → 排序 `TypeError` 退回原顺序 → 仍按 `missing_meta` 拼装 → 截断。每层都保留上层成果。

**源码事实：** 融合后的排序键是 `doc.score if doc.score else -1`——于是 `0` 与 `None` 都被降到 `-1`，与本仓其它组件的 `-inf` 用法不一致。

**源码事实：** `LostInTheMiddleRanker` 的插入位置是 `ceil(n/2)`，产出「最相关在最前、次相关在最后、最不相关在正中」；它**假设上游已排序**且不需要 query，因此能放在 prompt 构造之前。

**源码事实：** 它的 `word_count_threshold` 把触发越界的那篇**完整包含**（最终词数可能超阈值），且明说用 `str.split()` 而非模型 token 计数；非文本文档直接抛 `ValueError`。

**源码事实：** `MaxSimRanker` 的分数是 `max(axis=1).sum()`——每个查询向量取其在文档里的最佳匹配再求和，即 ColBERT 的 MaxSim。

**源码事实：** `_prepare_vectors` 用三个条件拒绝非法矩阵，其中 `np.isfinite` 那条挡住 `NaN`/`inf`——否则它们会静默污染排序（`NaN` 让比较全假、`inf` 让文档永远第一）。

**源码事实：** 缺多向量嵌入的文档被**汇总成一条警告**跳过（不是每文档一条），全都没有时返回空列表而非报错。

**架构推论：** 四个重排器揭示了「重排」这个词下的四种不同动作：**换相关性判据**（`LLMRanker`）、**融合两种判据**（`MetaFieldRanker`）、**只换位置**（`LostInTheMiddleRanker`）、**换相似度度量**（`MaxSimRanker`）。

而这个分类的价值在于**它们可以叠用**：`MaxSimRanker` 换度量 → `MetaFieldRanker` 融合业务权重 → `LostInTheMiddleRanker` 调位置。三步各自独立，因为前两步改分数、第三步只改顺序——**顺序敏感的步骤必须在最后**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/components/rankers/llm_ranker.py` | `LLMRanker` | 模型输出相关文档序号，兼做过滤 |
| `haystack/components/rankers/llm_ranker.py` | `DEFAULT_PROMPT_TEMPLATE` | 钉死 JSON 结构与「只输出相关项」规则 |
| `haystack/components/rankers/llm_ranker.py` | `_default_openai_chat_generator` | 绑定 JSON Schema 的默认 generator |
| `haystack/components/rankers/llm_ranker.py` | `run` | 两级降级，降级时不应用 `top_k` |
| `haystack/components/rankers/llm_ranker.py` | `_rank_documents_from_reply` | 三种数字形态容错、越界跳过、全无则抛错 |
| `haystack/components/rankers/meta_field.py` | `MetaFieldRanker` | 元数据排序与上游排序按 `weight` 融合 |
| `haystack/components/rankers/meta_field.py` | `_calculate_rrf` / `_calc_linear_score` | 两种融合模式的基础分数 |
| `haystack/components/rankers/meta_field.py` | `_merge_rankings` | 按权重累加两侧分数并重写 `score` |
| `haystack/components/rankers/meta_field.py` | `_parse_meta` | `meta_value_type` 解析，两处失败降级 |
| `haystack/components/rankers/meta_field.py` | `run` 的排序容错 | 四层降级链，按 `missing_meta` 拼装 |
| `haystack/components/rankers/lost_in_the_middle.py` | `LostInTheMiddleRanker` | 只改位置，`ceil(n/2)` 插入 |
| `haystack/components/rankers/lost_in_the_middle.py` | `word_count_threshold` | 按词数预算截断，越界那篇完整包含 |
| `haystack/components/rankers/max_sim.py` | `MaxSimRanker` | 多向量逐词最大内积求和 |
| `haystack/components/rankers/max_sim.py` | `_prepare_vectors` | 三条件校验矩阵，拒绝 `NaN`/`inf` |

## 参考

- [haystack 源码：rankers/llm_ranker.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/rankers/llm_ranker.py)
- [haystack 源码：rankers/meta_field.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/rankers/meta_field.py)
- [论文：ColBERT 与 MaxSim](https://arxiv.org/abs/2004.12832)
- [论文：Lost in the Middle](https://arxiv.org/abs/2307.03172)
- [论文：Reciprocal Rank Fusion](https://plg.uwaterloo.ca/~gvcormac/cormacksigir09-rrf.pdf)

## 与主核 / AI 的关系

重排与主核里的**二级索引回表后再过滤**同构：先用廉价索引选出候选，再用更贵的手段精确判定。而 `MaxSimRanker` 那种「稠密筛 + 多向量排」的两阶段，正是数据库里「索引扫描 + 精确的条件复核」的翻版。

**`MetaFieldRanker` 的 RRF 融合在主核里没有直接对应物**，因为数据库的 `ORDER BY` 是确定性的、不需要融合两个不可比的排序。RRF 存在的前提是「两个排序都有意义但分数不可比」——这是「用人/模型产出的相关性」才有的问题。

对 agent 工程的直接价值有三条。其一，**「重排失败时该怎么办」决定了整条链的健壮性**。`LLMRanker` 默认降级为「原顺序、不截断」，让下游能看出「全在但没排」；若降级时也截断，失败就伪装成正常输出。其二，**不可比的分数不能直接相加，要用排名**。`MetaFieldRanker` 的 `linear_score` 模式明确要求上游分数在 [0,1] 内，而默认的 RRF 模式不需要——这个差别是「要不要信任上游分数的量纲」。其三，**位置重排必须放在最后**。`LostInTheMiddleRanker` 只改顺序不改集合，任何在它之后的排序都会推翻它的成果；而它又需要「上游已排好序」这个前提，所以它的位置是被双向夹定的。
