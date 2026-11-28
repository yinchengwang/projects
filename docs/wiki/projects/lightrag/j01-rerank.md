---
project: lightrag
feature: rerank
title: 重排与分块聚合
page_type: feature
schema_version: 1
refs:
  - path: lightrag/rerank.py
    symbols: [chunk_documents_for_rerank, aggregate_chunk_scores, generic_rerank_api]
  - path: lightrag/rerank.py
    symbols: [cohere_rerank, jina_rerank, ali_rerank]
---

# 重排与分块聚合

## 一句话职责

重排模型（reranker）对「查询-文档」对打分，而它的输入有一个硬性的 token 上限；超出上限的文档必须**切成块分别打分再聚合回一个文档分**——而这一层的全部复杂度都在「切」与「聚」这两步上：切要保证可逆（能映射回原文档）且不切碎多字节字符，聚要保证 `top_n` 的语义仍然是「前 N 个文档」而不是「前 N 个块」。

## 机制是什么

三件事构成这条管线：

1. **切**（`chunk_documents_for_rerank`）：把超长文档按 token 窗口切成重叠块，同时返回 `doc_indices` —— 每个块属于哪篇原文档。
2. **打**（`generic_rerank_api`）：把块列表发给 provider，拿回每块的分数。
3. **聚**（`aggregate_chunk_scores`）：按 `doc_indices` 把块分归到文档，用某种策略合成一个分，然后排序。

而「聚」这一步的必要性不是性能优化而是**语义正确性**：若不聚合，返回的是一堆「块」按分数排序，而上层要的是「文档」按分数排序。两者的区别在 `top_n` 上最明显——`top_n=3` 在块空间里意味着「前 3 个块」，可能是同一篇文档的三段。

所以 `generic_rerank_api` 里有一个必须的做法：

```python
if enable_chunking:
    documents, doc_indices = await achunk_documents_for_rerank(documents, max_tokens=max_tokens_per_doc)
    ...
    # When chunking is enabled, disable top_n at API level to get all chunk scores
    # This ensures proper document-level coverage after aggregation
    # We'll apply top_n to aggregated document results instead
    if top_n is not None:
        logger.debug(f"Chunking enabled: disabled API-level top_n={top_n} to ensure complete document coverage")
        top_n = None
```

**在块空间里必须请求全部块**，因为「前 3 个块」可能只覆盖 1 篇文档，而那篇文档的真实排名无法判断。所以 `top_n` 被推迟到聚合之后再应用：

```python
if original_top_n is not None and len(standardized_results) > original_top_n:
    standardized_results = standardized_results[:original_top_n]
```

注释说明了这个顺序的语义：「这保持了文档级语义：`top_n` 限制文档，不是块」。

**这是这一页最重要的一条**：在「一变多」的变换之后，所有「前 N 个」的语义都必须重新定义，而正确的定义通常是「在原始空间里取前 N 个」。

## 怎么实现的

**`chunk_documents_for_rerank` 的四道前置校验，每一道防一个具体的死循环或静默错误。**

```python
if max_tokens < 1:
    # max_tokens=0 makes the chunk window zero-width and the loop never advances
    raise ValueError(f"max_tokens must be >= 1, got {max_tokens}")
```

第一道：`max_tokens = 0` 让窗口宽度为零，而 `start = end - overlap` 中的 `end` 永远等于 `start`，于是循环永不前进。这是**无限循环**，不是性能问题。

```python
if overlap_tokens < 0:
    # Checked up front, before the tokenizer/fallback branching below, so a negative value is always
    # rejected -- including for a short document that never enters either windowing loop and would
    # otherwise let the invalid value pass through unnoticed.
    raise ValueError(f"overlap_tokens must be non-negative, got {overlap_tokens}")
```

第二道先于分支检查，理由是「一个从不进入任何窗口循环的短文档否则会让这个非法值静默通过」。这是一个很实际的观察：若校验放在循环内部，那么「所有文档都短」这个常见情形下错误配置永远不会被发现——直到某天来了一篇长文档。

```python
if overlap_tokens >= max_tokens:
    original_overlap = overlap_tokens
    overlap_tokens = max_tokens // 2
    logger.warning(f"overlap_tokens ({original_overlap}) must be less than max_tokens ({max_tokens}). Clamping to {overlap_tokens} to prevent infinite loop.")
```

第三道是**夹紧而不是拒绝**，而夹紧到的值选 `max_tokens // 2` 而不是 `max_tokens - 1`，理由值得抄下来：

> 后者（`max_tokens - 1`）让每步只前进一个 token，把一篇文档炸成 O(tokens) 个块。折半让前进量约为半个窗口（`max_tokens == 1` 时重叠为零）。

所以两个选择都避免了死循环，但差一个数量级的块数量。而这里选「夹紧 + 警告」而不是「拒绝」，与前面两道不同——因为 `overlap >= max_tokens` 是一个**可修复**的配置（有明显的正确含义：重叠应当小于窗口），而 `max_tokens = 0` 没有。

**双路径：tokenizer 优先，字符近似兜底。**

```python
try:
    from .utils import TiktokenTokenizer
    tokenizer = TiktokenTokenizer(model_name=tokenizer_model)
except Exception as e:
    logger.warning(f"Failed to initialize tokenizer: {e}. Using character-based approximation.")
    max_chars = max_tokens * 4
    overlap_chars = overlap_tokens * 4
```

字符兜底用 `1 token ≈ 4 characters` 这个经验比例。而兜底路径的分块逻辑与 token 路径**不同**：

```python
start = 0
while start < len(doc):
    end = min(start + max_chars, len(doc))
    chunk = doc[start:end]
    chunked_docs.append(chunk); doc_indices.append(idx)
    if end >= len(doc): break
    start = end - overlap_chars
```

注意 `if end >= len(doc): break` 在 `start` 更新**之前**，所以最后一块不会产生一个零长度的尾部块。

而 token 路径**不用**这个循环，而是用 `tokenizer.split_by_token_limit`：

```python
# Token windows can cut through a Unicode code point. Use source spans so each chunk preserves text and fits after re-encoding.
for span in tokenizer.split_by_token_limit(doc, max_tokens, overlap_tokens):
    chunked_docs.append(doc[span.start : span.end])
    doc_indices.append(idx)
```

注释点出了一个关键问题：**token 窗口可以切穿一个 Unicode 码点**。因为 tokenizer 的 token 边界与字符边界不对齐——一个 token 可能对应半个 emoji（在 byte-level BPE 里）、或者多个 token 对应一个字。

而 `split_by_token_limit` 返回的是**源字符串的 span**（`span.start` / `span.end` 是字符偏移），所以 `doc[span.start:span.end]` 永远是**逐字子串**，不会产生替换字符（U+FFFD）。

这与 h01 那页的「原子化」是同一个问题的另一个形态：

| 场景 | 结构单位 | 处理 |
|---|---|---|
| h01：多模态上下文 | 多模态标签 | `_atomize` 避免切半标签 |
| 这里：重排分块 | Unicode 码点 | 用源 span 避免切半码点 |

两处的手段不同（atom 化 vs span 映射），但目标一样：**截断不能落在结构单位中间**。

而 docstring 里那句 `Raises: TokenBudgetError: If one complete Unicode code point exceeds max_tokens when using the tokenizer; lossy replacement characters are not emitted` —— 说明了一个极端情况：若单个码点就超过预算，那**无法**在不产生损失的前提下切分，所以抛错而不是发出替换字符。

这是一个很重要的立场：**宁可不切，也不发出被损坏的文本**。因为损坏的文本会被送进模型并影响排序，而那是一种无法归因的错误。

**`aggregate_chunk_scores` 用「按原文档索引分组」而不是「边扫边累加」。**

```python
doc_scores: Dict[int, List[float]] = {i: [] for i in range(num_original_docs)}
for result in chunk_results:
    normalized_result, _ = normalize_rerank_result(result, len(doc_indices))
    if normalized_result is None: continue
    chunk_idx = normalized_result["index"]
    score = normalized_result["relevance_score"]
    original_doc_idx = doc_indices[chunk_idx]
    if isinstance(original_doc_idx, int) and 0 <= original_doc_idx < num_original_docs:
        doc_scores[original_doc_idx].append(score)
```

三点：

1. **预置全部键**（`{i: [] for i in range(num_original_docs)}`）——这样后面遍历 `doc_scores.items()` 时顺序是 0..N-1，与文档顺序一致（虽然最终会排序，但确定性排序在分数相同时有用）。
2. **`normalize_rerank_result` 的第二个参数是 `len(doc_indices)`**（块数）——因为它要校验 `index` 在合法范围内。
3. **`isinstance(..., int) and 0 <= ... < num_original_docs`** 双检查——防 `doc_indices` 里的脏值（它由上游构造，但这里仍然校验）。

而聚合策略有三档：

```python
if aggregation == "max": final_score = max(scores)
elif aggregation == "mean": final_score = sum(scores) / len(scores)
elif aggregation == "first": final_score = scores[0]
else:
    logger.warning(f"Unknown aggregation strategy: {aggregation}, using max")
    final_score = max(scores)
```

`"first"` 这一档有点特别——它取第一个块的分数，而「第一个块」是文档开头那一段。它的用途是「只看开头是否相关」，而对长文档来说这是一个便宜的启发式。

而未知策略**回退到 max 并警告**，而不是抛错。这与前面 `chunk_documents_for_rerank` 的「拒绝非法参数」形成对比：那里拒绝，这里回退。差别在于「max」是一个合理的默认（重排的本质是「这篇文档里有没有相关内容」，所以最大值最贴切），而「max_tokens = 0」没有合理解释。

**`generic_rerank_api` 的装饰器有三个参数，而第三个是这一层最重要的。**

```python
@retry(
    stop=stop_after_attempt(3),
    wait=wait_exponential(multiplier=1, min=4, max=60),
    retry=retry_if_exception(is_transient_aiohttp_error),
    # Surface the last attempt's own exception rather than tenacity.RetryError, so the "Error during reranking" log names the provider's message.
    reraise=True,
)
```

| 参数 | 值 | 理由 |
|---|---|---|
| `stop_after_attempt(3)` | 3 次 | — |
| `wait_exponential(1, 4, 60)` | 4→60 秒 | — |
| `retry_if_exception(is_transient_aiohttp_error)` | **只重试暂时性错误** | 永久性错误（400、鉴权失败）重试三次纯浪费 |
| `reraise=True` | 抛原始异常 | 「让 `Error during reranking` 日志指明 provider 的消息」 |

第 4 项很实在：`tenacity.RetryError` 的错误消息是「RetryError[<Future at 0x...>]」，完全不包含 provider 说了什么。所以 `reraise=True` 让最后一轮的原始异常浮出来——而那是唯一包含「为什么失败」的信息。

而第 3 项的分工（`is_transient_aiohttp_error` 在另一个模块）说明「什么算暂时性」这个判断被抽出来了，而 docstring 里也验证了这一点：

> `aiohttp.ClientResponseError`: 如果 rerank API 返回非 200 响应。**连接失败与 408 / 409 / 429 / 5xx 响应先重试最多三次**；最后一轮的异常原样抛出，绝不包装进 `tenacity.RetryError`。

所以「可重试集合」被精确列举：连接失败、408（超时）、409（冲突）、429（限流）、5xx。而 4xx 里的其他（400 参数错、401 鉴权、403 权限、404 路径错）不重试——重试它们只会把「配置错误」变成「三次配置错误」。

**`top_n` 的两道校验体现了「负数是静默错误」这个事实。**

```python
if top_n is not None and top_n < 0:
    # A negative slice stop would silently drop the last |top_n| results.
    raise ValueError(f"top_n must be non-negative, got {top_n}")
```

「一个负的切片终点会静默地丢掉最后 `|top_n|` 条结果」——这是一个 Python 特有的陷阱：`results[:-3]` 是合法的，它返回「除最后 3 条之外的全部」。所以 `top_n = -3` 不会报错，而是**多返回**了 `len-3` 条。

而 `base_url` 的空检查单独一句：

```python
if not base_url:
    raise ValueError("Base URL is required")
```

这里的顺序是先查 `base_url` 再查 `top_n`，而 `headers` 的构造在两者之后——所以一个缺 `base_url` 的调用不会先花时间建 headers。

**`Authorization` 头只在 `api_key is not None` 时加上。**

```python
headers = {"Content-Type": "application/json"}
if api_key is not None:
    headers["Authorization"] = f"Bearer {api_key}"
```

用 `is not None` 而不是真值判断——所以 `api_key=""` 会加上一个 `Bearer ` 头。这看起来奇怪但正确：空字符串与 `None` 是**不同的意图**（前者「我有密钥但它是空的」，后者「我没有密钥」），而某些自托管服务确实接受空 Bearer。而若是 `if api_key:`，那空字符串会被静默当成「没配密钥」，于是服务端收到的是「未认证」而不是「空认证」——两者对服务端的行为可能不同。

**两种请求/响应格式的处理被拆成 `request_format` 与 `response_format` 两个独立参数。**

```python
if request_format == "aliyun":
    payload = {"model": model, "input": {"query": query, "documents": documents}, "parameters": {}}
    if top_n is not None: payload["parameters"]["top_n"] = top_n
    ...
```

而响应侧：

```python
if response_format == "aliyun":
    results = response_json.get("output", {}).get("results", [])
elif response_format == "standard":
    results = response_json.get("results", [])
else:
    raise ValueError(f"Unsupported response format: {response_format}")
```

四个参数而不是两个的原因很实际：阿里云的格式差异在**请求体结构**（嵌套 `input`/`parameters`）与**响应体结构**（`output.results`）两处，而这两处**可能独立**——一个 provider 可能接受标准请求但返回嵌套响应，或反之。所以两个维度分开。

而 `results` 不是 list 时的处理是**警告并置空**，而不是抛：

```python
if not isinstance(results, list):
    logger.warning(f"Expected 'output.results' to be list, got {type(results)}: {results}")
    results = []
```

然后 `if not results: return []`。所以「响应形状不对」退化成「没有结果」——上层会得到空列表，于是保序或退化成向量相似度顺序。这与前面「拒绝非法输入参数」形成对比：**输入参数错误要抛（那是调用方的 bug），输出形状错误要降级（那是 provider 的 bug）。**

**畸形结果的统计用 `Counter` 汇总成一行，而不是逐条记日志。**

```python
invalid_results = Counter()
standardized_results = []
for result in results:
    normalized_result, invalid_reason = normalize_rerank_result(result, len(documents))
    if normalized_result is None:
        invalid_results[invalid_reason] += 1
        continue
    standardized_results.append(normalized_result)

if invalid_results:
    invalid_summary = ", ".join(f"{reason}={count}" for reason, count in sorted(invalid_results.items()))
    logger.warning("Discarded %s malformed rerank result(s): %s", sum(invalid_results.values()), invalid_summary)
```

注释：「把畸形条目的报告为一条有界的汇总，而不是让整个用户查询失败」。

三点：

1. **`normalize_rerank_result` 返回 `(result, reason)` 二元组** ——所以「为什么畸形」被分类了（`invalid_reason`），而不只是「畸形」。
2. **按 reason 计数并排序** ——所以汇总行是确定性的（同样的输入产生同样的日志）。
3. **不抛** ——单个畸形条目被丢弃，其余结果继续用。

这是「部分成功」的正确处理：一个 provider 返回 100 个结果其中 3 个格式不对，正确做法是「用那 97 个并报告 3 个坏」，而不是「整个查询失败」。

而紧接着的兜底：

```python
if not standardized_results:
    logger.warning("Rerank API returned no usable results")
    return []
```

所以「全坏」与「全空」得到同一个结果（空列表），而日志分别说明了原因。

**三个 provider 包装器（`cohere_rerank` / `jina_rerank` / `ali_rerank`）各自只做参数映射。**

从签名看它们都是 `(query, documents, top_n, api_key, ...)` 而内部调 `generic_rerank_api`，差别在：

- **`cohere_rerank`**：标准格式，`return_documents` 有意义。
- **`jina_rerank`**：标准格式，`return_documents` 有意义（docstring 说「Jina only」）。
- **`ali_rerank`**：`request_format="aliyun"` + `response_format="aliyun"`。

所以「provider 差异」被压缩成参数映射，而逻辑只有一份。这与 i07 那页里「绑定选项派生」是同一个思路：**差异是声明，逻辑是共享的**。

`DEFAULT_RERANK_MAX_TOKENS_PER_DOC = 4096` 的注释说明它不是通用最优值：

> 4096 匹配 Cohere rerank-v3.5；更小的模型（例如 ColBERT 的 512 token 上限）应该覆盖它，以便在硬上限之下留出余量。

所以这个默认值是「跟随最强模型」而不是「取所有模型的交集」——而理由是：若默认取 512，那用 Cohere 的用户会白白多切很多块。而用 4096 的用户若配了小模型，后果是**请求被拒**（provider 报超限）而不是静默降质——所以这个方向的错误更可见。

而 `MIN_PRACTICAL_RERANK_MAX_TOKENS = 64` 的注释说明了「为什么不是硬限制」：

> 一个重排块窗口的实用下界。低于这个值循环仍然终止（由 `max_tokens >= 1` 守卫与重叠夹紧保证），但每篇文档会炸成许多微小的低信号块——膨胀请求载荷与要聚合的分数数量。**配置的值低于这个会被接受但在启动时警告，而不是静默降级查询延迟。**

所以这里的原则是：**不阻止，但让代价可见**。而「膨胀请求载荷」是一个可算的代价：一篇 4000 token 的文档在 `max_tokens=32` 下会变成约 125 个块，所以请求体涨 125 倍。

## 关键取舍

**源码事实：** 重排的分块/聚合三步：切（返回 `doc_indices` 映射块→原文档）、打、聚；聚合不是性能优化而是语义正确性——`top_n=3` 在块空间意味着「前 3 个块」，可能是同一篇文档的三段。

**源码事实：** 启用分块时**必须在 API 层禁用 `top_n`**（改为请求全部块），否则「前 3 个块」只覆盖 1 篇文档、真实排名无法判断；`top_n` 改为聚合后应用，因为「`top_n` 限制文档，不是块」。

**源码事实：** `max_tokens < 1` 拒绝（窗口宽零 → 循环永不前进），`overlap_tokens < 0` 拒绝（且**先于任何分支检查**，「一个从不进入窗口循环的短文档否则会让非法值静默通过」）。

**源码事实：** `overlap_tokens >= max_tokens` 被**夹紧到 `max_tokens // 2`** 而不是拒绝，因为夹紧到 `max_tokens - 1` 会「让每步只前进一个 token，把文档炸成 O(tokens) 个块」；折半让前进量为半个窗口。

**源码事实：** token 路径用 `split_by_token_limit` 返回的**源字符 span**，因为「token 窗口可以切穿一个 Unicode 码点」；单码点超预算时抛 `TokenBudgetError`，**绝不发出替换字符**。

**源码事实：** 字符兜底路径按 `1 token ≈ 4 characters` 近似，且 `if end >= len(doc): break` 在更新 `start` 之前，避免产生尾部零长度块。

**源码事实：** `aggregate_chunk_scores` 预置全部文档键（顺序确定）、用 `isinstance(int) and 0 <= i < N` 双检查脏索引；未知聚合策略**回退到 `max` 并警告**（重排本质是「这篇文档里有没有相关内容」）。

**源码事实：** `generic_rerank_api` 的 `@retry` 用 `reraise=True`，因为「让 `Error during reranking` 日志指明 provider 的消息」——`tenacity.RetryError` 的消息不含任何 provider 信息。

**源码事实：** 可重试集合被精确列举：连接失败 + 408 / 409 / 429 / 5xx；其余 4xx 不重试，所以「配置错误」不会变成「三次配置错误」。

**源码事实：** 负 `top_n` 被拒，因为「一个负的切片终点会静默丢掉最后 `|top_n|` 条结果」——`results[:-3]` 是合法的 Python。

**源码事实：** `api_key=""` 与 `None` 被区别对待（`is not None` 判断），所以空字符串会发出 `Authorization: Bearer ` 头——「空认证」与「未认证」对某些自托管服务是不同意图。

**源码事实：** `request_format` 与 `response_format` 是**两个独立参数**，因为请求体结构与响应体结构可能独立变化。

**源码事实：** 响应形状不对时**警告并置空**（降级），而输入参数非法时**抛错**——「输入参数错误是调用方的 bug，输出形状错误是 provider 的 bug」。

**源码事实：** 畸形结果按 reason 计数并排序成一行汇总日志，「而不是让整个用户查询失败」；全坏与全空都返空列表但日志分别说明原因。

**源码事实：** `DEFAULT_RERANK_MAX_TOKENS_PER_DOC = 4096` 跟随 Cohere rerank-v3.5 而非取所有模型交集——因为配小模型的代价是「请求被拒」（可见），而默认取 512 会让用大模型的用户白切很多块。

**源码事实：** `MIN_PRACTICAL_RERANK_MAX_TOKENS = 64` 只**警告不阻止**，因为「降低它会膨胀请求载荷与要聚合的分数数量」是可算的代价，应让运维看见而不是静默降级。

**架构推论：** 「一变多」的变换必须同时返回**逆映射**。`chunk_documents_for_rerank` 返回 `(chunks, doc_indices)` 而不是只返回 chunks，这让聚合成为可能。任何「拆分后还要合并」的设计都必须遵守这一条——只返回拆分结果的 API 会让调用方无从合并，于是只能退化成「在拆分空间里返回」，而那通常语义错误。

**架构推论：** 这一层在两个地方拒绝「静默降级」，方向相反：`top_n` 负数**拒绝**（因为静默行为是「多返回」）、`MIN_PRACTICAL_RERANK_MAX_TOKENS` 只警告（因为静默行为是「慢但正确」）。判据是：**降级之后的结果是错的还是只是慢的？** 错的必须拒绝，慢的可以让代价可见后继续。

**架构推论：** 「输入严格、输出宽容」这条分工在一层之内出现两次（参数校验 vs 响应解析、`chunk_documents_for_rerank` 的参数 vs `aggregate_chunk_scores` 的未知策略）。这可以概括成：**你能控制的严格处理，你不能控制的需要容错**。而容错的方式是把「坏了」变成「少了」（丢弃畸形条目）而不是变成「失败」——因为一次 provider 的格式变化不该让整个查询不可用。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/rerank.py` | `chunk_documents_for_rerank` | 按 token 窗口切块并返回块→原文档映射 |
| `lightrag/rerank.py` | `achunk_documents_for_rerank` | 上面的异步包装（跑在 tokenizer executor） |
| `lightrag/rerank.py` | `aggregate_chunk_scores` | 按 `doc_indices` 分归并用 max/mean/first 合成 |
| `lightrag/rerank.py` | `generic_rerank_api` | 唯一的实现，含分块、双格式、聚合、重试 |
| `lightrag/rerank.py` | `cohere_rerank` | Cohere 标准格式的参数映射 |
| `lightrag/rerank.py` | `jina_rerank` | Jina 标准格式，`return_documents` 有意义 |
| `lightrag/rerank.py` | `ali_rerank` | 阿里云嵌套格式的请求与响应 |
| `lightrag/rerank.py` | `DEFAULT_RERANK_MAX_TOKENS_PER_DOC` | 4096，跟随 Cohere rerank-v3.5 |
| `lightrag/rerank.py` | `MIN_PRACTICAL_RERANK_MAX_TOKENS` | 64，只警告不阻止 |
| `lightrag/rerank.py` | `is_transient_aiohttp_error` | 可重试集合的唯一定义（在 `llm/_aiohttp_retry`） |
| `lightrag/rerank.py` | `normalize_rerank_result` | 返回 `(结果, 畸形原因)` 二元组（在 `utils`） |

## 参考

- [lightrag 源码：rerank.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/rerank.py)
- [Cohere Rerank 文档](https://docs.cohere.com/docs/rerank-overview)
- [Jina Reranker 文档](https://jina.ai/reranker/)
- [tenacity 文档](https://tenacity.readthedocs.io/)

## 与主核 / AI 的关系

主核里最接近的是**分批排序（external merge sort）**：一个排序操作的工作集超过内存时，数据被切成多份分别排序再归并。而重排的分块聚合是同一结构的一个变体——只是「归并」用的不是比较而是**聚合函数**（max/mean/first）。

**差别在于「归并是否可逆」。** 外部排序的归并保持全序，所以「前 N 个」的语义不变；而这里的聚合并**不**保持全序——两篇文档各自的块分不同，聚合后可能反转。所以「`top_n` 必须推迟到聚合之后」不是实现细节，而是**因为聚合不保序**。这个分析值得记住：任何在聚合前做截断的地方都要先问「聚合保不保序」。

与 h01（多模态上下文）的关系：两处都在处理「截断不能切穿结构单位」。h01 的结构单位是多模态标签（所以先 atom 化再切），这里是 Unicode 码点（所以用源 span 而不是 token 序列）。而 h01 的 token 预算与这里的 token 窗口是同一个 tokenizer 的两种用法。

与 e03（上下文预算）的关系是「同一个 token 预算的不同用途」：那边决定「哪些检索结果进上下文」，这边决定「一篇文档切成几块」。

对 AI 工程最实用的一条是**「拆分-处理-合并」必须显式设计合并语义**。任何把长文档切块送进模型（重排、分类、抽取、摘要）的管线都会遇到：块分数怎么变成文档分数？`max` 适合「有没有相关内容」、`mean` 适合「整体质量」、`first` 适合「开头是否切题」——而选错策略不会报错，只会让排序看起来「有点奇怪」。所以策略应当显式配置，而默认值要能说清理由。

另一条是 `reraise=True`。任何用 tenacity 包装的重试都必须设它，否则最终错误是 `RetryError[<Future at 0x7f...>]`，里面的 provider 消息完全丢失。这一条在排障时的价值极高——因为「重试三次都失败」这个事实本身没信息量，有用的是第三次为什么失败。

最后一条是「负 `top_n` 会静默多返回」。Python 的切片语义让负数索引合法，于是任何接受 `top_n` / `limit` / `k` 的 API 都在往切片里传之前必须校验非负。这个坑的隐蔽性在于：它在多数测试里都不会触发（因为测试通常传正数），而在生产里一旦触发就是「返回了比预期更多的东西」——而那通常不会被当成错误。
