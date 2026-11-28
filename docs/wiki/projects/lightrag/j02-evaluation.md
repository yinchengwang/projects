---
project: lightrag
feature: evaluation
title: 评估与离线检索检查
page_type: feature
schema_version: 1
refs:
  - path: lightrag/evaluation/eval_rag_quality.py
    symbols: [RAGEvaluator, _is_nan]
  - path: lightrag/evaluation/offline_retrieval_check.py
    symbols: [score_query, inverse_document_frequency, audit_samples]
---

# 评估与离线检索检查

## 一句话职责

两个不同层次的验证工具：`eval_rag_quality.py` 是**端到端**评估（起真实 API、拿真实答案与真实检索块、交给 RAGAS 打四个指标），`offline_retrieval_check.py` 是**零依赖**的自检（不启动 LightRAG、不调 API、不嵌向量，用一个小型确定性词法排序器验证「随附的样例数据本身是自洽的」）——而两者共同的难点不是打分，是**NaN 与「没有结果」的区分**。

## 机制是什么

两者面向的问题完全不同，而放置在一起是因为它们构成一条「从便宜到贵」的验证梯度：

| | `offline_retrieval_check` | `eval_rag_quality` |
|---|---|---|
| 依赖 | 只用标准库 | httpx、ragas、langchain、datasets、tqdm |
| 需要服务 | 否 | 是（LightRAG API + LLM + 嵌入模型） |
| 耗时 | 毫秒 | 分钟到小时（每个用例两次 LLM 调用） |
| 验证什么 | 样例数据集与 oracle 自洽 | 真实端到端质量 |
| 失败说明 | 样例数据坏了 | 系统质量下降或配置错了 |

`offline_retrieval_check` 的模块 docstring 用一句话把它的边界画清了：

> 这个检查使用一个小型确定性词法排序器。它**不启动 LightRAG、不调 API 服务器、不算嵌入、不调 LLM/RAGAS 服务**。

所以它的角色是**回归基线**：如果样例数据被改动导致某个问题不再能检索到正确文档，它在毫秒内就能发现，而不需要花一小时跑端到端评估去定位「是数据坏了还是系统坏了」。

## 怎么实现的

**`offline_retrieval_check` 的分词器是三件事的组合：NFKC 规范化、停用词过滤、CJK 双字索引。**

```python
def tokenize(text: str) -> list[str]:
    tokens: list[str] = []
    normalized = unicodedata.normalize("NFKC", text).lower()
    for run in _TOKEN_PATTERN.findall(normalized):
        if run.isascii():
            if run not in STOPWORDS and len(run) > 1:
                tokens.append(run)
        elif len(run) == 1:
            tokens.append(run)
        else:
            tokens.extend(run[i : i + 2] for i in range(len(run) - 1))
    return tokens
```

四个决定，每个都有理由：

1. **NFKC 规范化 + `lower()`** ——docstring：「文本先做 NFKC 规范化，所以全角 ASCII（`ＲＡＧ`）能匹配它的 ASCII 形式，半角片假名能匹配全角片假名」。

2. **ASCII 词过滤停用词与单字符** ——「ASCII 词保留，除非它们是停用词或单个字符」。单字符被丢因为「a」「I」这类没有区分度。

3. **CJK 单字保留、多字转双字** ——「一串 CJK 字符变成它的重叠双字；一个孤立的 CJK 字符原样保留，因为一个表意字常常就是一整个词」。

4. **重叠双字而不是不重叠** ——`run[i:i+2] for i in range(len(run)-1)` 产生 `n-1` 个双字，每对相邻字符都被覆盖。理由与 Lucene 的 CJK analyzer 一致：不重叠会漏掉「AG」这样的跨界组合。

而 `_CJK_CHARS` 的字符范围定义里有两处细致的排除：

```python
_CJK_CHARS = ("\u3005" "\u3041-\u309f" "\u30a1-\u30fa\u30fc-\u30ff" "\u3400-\u4dbf\u4e00-\u9fff" "\uac00-\ud7af" "\uf900-\ufaff" "\U00020000-\U0003ffff")
```

注释说明了 `\u30a1-\u30fa\u30fc-\u30ff` 这个**断开**的区间：

> 假名区间略过 U+30A0 与片假名中点 U+30FB，它们是标点，所以 `データ・ベース` 不会产生一个跨越那个点的双字。迭代符 U+3005 被保留因为它是 `人々` 这类词的一部分。平面 2 与 3 只含 CJK 表意字（Extension B 起）。

所以「双字」的生成必须**在标点处断开**，否则 `データ・ベース` 会产出 `タ・` 与 `・ベ` 这样的垃圾双字——而它们会污染 IDF 与匹配。

而 `\U00020000-\U0003ffff` 覆盖 SJK 扩展 B 及以后——这需要 Python 的宽字符支持（PEP 393 之后所有 Python 3 都是）。

**`inverse_document_frequency` 用平滑的 IDF，而公式的选择有实际后果。**

```python
return {token: math.log((doc_count + 1) / (frequency + 1)) + 1 for token, frequency in document_frequency.items()}
```

这是标准的平滑 IDF（`+1` 在分子分母，`+1` 在结果上）。三点：

1. **分子分母都加 1** ——避免 `log(N/0)` 的除零，且让「出现在所有文档里」的词得到 `log(1)+1 = 1` 而不是 0。所以**没有任何词的 IDF 是零**，于是「只由高频词组成的查询」仍能产生非零分数。
2. **结果加 1** ——保证 IDF 恒为正。若不加，一个出现在所有文档里的词 IDF = 0，它对分数的贡献为零——那在数学上合理，但会让「只匹配到这种词」的文档得分恰好 0，从而被 `if score > 0` 过滤掉。
3. **`math.log` 是自然对数** ——底数的选择对排序无影响（只是全局缩放），所以用自然对数是惯例。

第 1、2 点合起来的效果是：**匹配总是产生正分数**，所以 `audit_samples` 里那个 `if score > 0` 的过滤等价于「至少匹配到一个 token」，而不是「IDF 权重非零」。

**`score_query` 是 TF-IDF 的标准形态，但有一处值得注意。**

```python
def score_query(query_tokens, document, idf):
    score = 0.0
    for token in query_tokens:
        if token in document.tokens:
            score += (1 + math.log(document.tokens[token])) * idf.get(token, 0.0)
    return score
```

`1 + log(tf)` 是「对数词频」的标准写法——它让「出现 1 次」与「出现 10 次」的差距是 3.3 倍而不是 10 倍。而 `if token in document.tokens` 保证了 `log(0)` 不会出现。

而 `idf.get(token, 0.0)` ——查询里可能有文档集中从未出现的 token，而 `inverse_document_frequency` 只对**文档里出现过的** token 建表。所以缺键回退到 0：一个从未在任何文档里出现的 token 不贡献分数（合理，因为它不可能帮助区分）。

**`audit_samples` 的排序用「分数降序 + 文件名升序」的双键，所以结果是确定的。**

```python
ranked = [document for score, document in sorted(scored_documents, key=lambda item: (-item[0], item[1].name)) if score > 0]
```

`key=lambda item: (-item[0], item[1].name)` ——分数取负实现降序，文件名作为并列时的次序。所以**同样的输入永远产生同样的 rank 列表**，这让「recall@k 从 0.8 掉到 0.7」成为一个可复现的信号而不是噪声。

而 `if score > 0` 在列表推导的**末尾**——它过滤的是 `(score, document)` 元组（因为 `sorted` 的输出是元组序列，而推导变量解包成 `score, document`）。所以零分文档不被排入结果。

**`QueryResult` 的两个指标各有一处「除零」风险，而处理方式不同。**

```python
def recall_at(self, top_k: int) -> float:
    expected = set(self.expected)
    hits = expected & set(self.ranked[:top_k])
    return len(hits) / len(expected)

def reciprocal_rank(self) -> float:
    for rank, document in enumerate(self.ranked, start=1):
        if document in self.expected:
            return 1 / rank
    return 0.0
```

`recall_at` 除以 `len(expected)` ——若 `expected` 为空就是除零。但 `load_oracle` 里已经挡了这一步：

```python
if not question or not isinstance(expected, list) or not expected:
    raise ValueError("Each oracle entry needs question and expected_documents")
```

`not expected` 拒绝了空列表。所以「oracle 里没有期望文档」是一个**加载期错误**而不是运行期除零。

`reciprocal_rank` 无命中返回 0.0（而不是 `1/rank` 的某个值），这是 MRR 的定义：未命中贡献 0。

**`summarize` 里有两个「全命中 / 全未命中」的计数，而它们的用途是区分两种失败。**

```python
return {
    "queries": len(results),
    "top_k": top_k,
    "average_recall_at_k": sum(recalls) / len(recalls),
    "mean_reciprocal_rank": sum(reciprocal_ranks) / len(reciprocal_ranks),
    "full_recall_queries": sum(recall == 1.0 for recall in recalls),
    "no_hit_queries": sum(recall == 0.0 for recall in recalls),
}
```

平均 recall 与 MRR 是汇总，而 `full_recall_queries` / `no_hit_queries` 是两个端点的计数。为什么需要后者？因为平均值会掩盖分布：10 个查询里 5 个满分 5 个零分，与 10 个都是 0.5，平均 recall 都是 0.5——但前者说明「一半的查询完全失效」，后者说明「普遍排名偏低」。这两个问题的修法完全不同。

而 `recall == 1.0` 与 `recall == 0.0` 的**精确相等**比较是安全的，因为 `recall_at` 返回的是 `len(hits) / len(expected)`——整数除法产生的浮点在 0 与 1 这两个端点是精确的（`0/n == 0.0`、`n/n == 1.0`）。中间值不精确，但这里不比较它们。

**`main` 的退出码有三档，而 `--strict` 把「不完美」变成失败。**

```python
if args.top_k <= 0:
    print("--top-k must be positive", file=sys.stderr)
    return 2
...
except (OSError, ValueError, json.JSONDecodeError) as exc:
    print(f"Sample retrieval check failed: {exc}", file=sys.stderr)
    return 2
...
if args.strict and summary["full_recall_queries"] != summary["queries"]:
    return 1
return 0
```

三档：`2` = 检查本身跑不了（参数错、文件读不了、JSON 坏、oracle 缺项）；`1` = 跑了但没全对（且 `--strict`）；`0` = 正常。

这个分工对 CI 很重要：`2` 说明「测试基础设施坏了」，`1` 说明「被测的东西退化了」。两者需要不同的响应。

而 `except` 捕获的三个类型都是「输入坏了」：`OSError`（文件读不了）、`ValueError`（`load_cases` / `load_oracle` 的显式校验）、`json.JSONDecodeError`（`json.loads` 失败）。三者都归到 `2`。

`--strict` 的 `help` 说明了它的用途：「除非每个样例查询都有满 recall@k，否则以非零退出」。所以默认模式是「跑一下看看」，而 `--strict` 是 CI 模式。

**现在转到端到端评估，而它的第一个设计决定是「导入失败要保留原因」。**

```python
_RAGAS_IMPORT_ERROR: ImportError | None = None
try:
    from datasets import Dataset
    from ragas import evaluate
    ...
    RAGAS_AVAILABLE = True
except ImportError as exc:
    RAGAS_AVAILABLE = False
    _RAGAS_IMPORT_ERROR = exc
    Dataset = None
    evaluate = None
    LangchainLLMWrapper = None
```

注释说明了为什么要保存异常对象：

> 这里的 `ImportError` 意味着「没装」**或**「装了但坏了」（例如 ragas/langchain 版本不匹配）；**保留异常好让 `RAGEvaluator` 报告真正的原因**，而不是一句泛泛的安装提示。

这个区分在排障时很实在：「没装」的修法是 `pip install ragas`，「装了但版本冲突」的修法是 `pip install ragas==0.2.x`——而若统一报「请安装 ragas」，后者会被反复重装而无效。

而三处 `Dataset = None` 之类的赋值是为了让**模块级名字仍然存在**——否则若有人在无 ragas 环境下引用它们，会得到 `NameError` 而不是更有意义的错误。

**RAGAS 顶层指标的导入被包在 `warnings.catch_warnings()` 里，而注释解释了为什么不能换成新 API。**

```python
with warnings.catch_warnings():
    warnings.filterwarnings("ignore", message=r"Importing \w+ from 'ragas\.metrics' is deprecated", category=DeprecationWarning)
    from ragas.metrics import (AnswerRelevancy, ContextPrecision, ContextRecall, Faithfulness)
```

注释：

> ragas 0.4 把这些顶层导出废弃了，改为 `ragas.metrics.collections`，但 **collections 的指标不被 `ragas.evaluate()` 接受**且拒绝 `LangchainLLMWrapper`，所以它们不是可替代的。只静音这条导入的通知，范围局限在此。

所以「用废弃 API」是一个**有意的选择**，因为新 API 不完整。而抑制警告的范围被限制在 `with` 块内——所以其他废弃警告仍然可见。这与模块级那两处 `warnings.filterwarnings` 形成对比：那两处是全局的（针对 LangchainLLMWrapper 废弃与 token usage 警告），因为它们会在**运行期**反复触发。

**`CONNECT_TIMEOUT_SECONDS = 180.0` 与 `READ_TIMEOUT_SECONDS = 300.0` 这个量级说明评估的调用是慢的。**

连接 180 秒、读取 300 秒——这些不是普通 HTTP API 的超时，而是「一次 mix 模式查询要跑三轮检索 + 一次 LLM 生成」的量级。而 `TOTAL_TIMEOUT_SECONDS = 180.0` 用在别处（可能是单次评估的总预算）。

**`_is_nan` 是一个两行函数，而它的存在理由是「RAGAS 会返回 NaN」。**

```python
def _is_nan(value: Any) -> bool:
    """Return True when value is a float NaN."""
    return isinstance(value, float) and math.isnan(value)
```

为什么不能直接用 `math.isnan`？因为它对非 float 抛 `TypeError`。而 RAGAS 的指标字典里可能有 `None`、字符串、或缺失键（`metrics.get("faithfulness", 0)` 会给出 `0`）。

所以 `_is_nan` 的定义是「它是 float 且是 NaN」——`isinstance` 前置让它对任何类型都安全。

而这个函数在 `_calculate_benchmark_stats` 里被用了**六次**，每次都在做同一件事：把一个指标值累加进 `sum` 并递增 `count`，除非它是 NaN。

**`_calculate_benchmark_stats` 的 NaN 处理方式是「按指标独立计数」。**

```python
metrics_data = {name: {"sum": 0.0, "count": 0} for name in _METRIC_NAMES}
for result in valid_results:
    metrics = result.get("metrics", {})
    faithfulness = metrics.get("faithfulness", 0)
    if not _is_nan(faithfulness):
        metrics_data["faithfulness"]["sum"] += faithfulness
        metrics_data["faithfulness"]["count"] += 1
    ...
```

所以每个指标有自己的 `count`，而不是共用一个。这解决了什么问题？

RAGAS 的四个指标里，`context_recall` 与 `context_precision` 需要有 ground truth 上下文；若某次评估缺了 ground truth，这两个指标会是 NaN 而 `faithfulness` / `answer_relevance` 仍有值。若共用一个 `count = len(valid_results)`，那 NaN 会被当作 0 计入平均——**把「无法评估」变成「得分为零」**。

这是一个很重要的区分：**NaN 意味着「这个指标对这个样本不适用」，而不是「得分为零」**。前者应从平均中排除，后者应计入。

而平均值的计算：

```python
for metric_name, data in metrics_data.items():
    if data["count"] > 0:
        avg_val = data["sum"] / data["count"]
        avg_metrics[metric_name] = round(avg_val, 4) if not _is_nan(avg_val) else 0.0
    else:
        avg_metrics[metric_name] = 0.0
```

两道 NaN 检查：累加前的（排除 NaN 样本）与平均后的（理论上不该再是 NaN，但防御性检查）。而 `count == 0`（所有样本都是 NaN）时给 0.0——所以「一个指标完全无法评估」在输出里显示为 0.0，而那与「得分为零」在数值上不可区分。

这是一个残留的模糊性，而它被另两个字段补偿：`successful_tests` 与 `failed_tests`。所以读报告的人能看到「20 个测试里 3 个失败」，从而知道平均值不是基于全部 20 个。

**「全部失败」的分支返回**零值而不是**省略字段**，而理由是可组合性。**

```python
if not valid_results:
    # Every case failed (e.g. an all-NaN RAGAS result, or every RAG call raised). Keep the same keys
    # the success path returns -- zeroed rather than omitted -- so callers like run() can read
    # average_metrics / min_ragas_score / max_ragas_score unconditionally instead of branching on
    # whether anything succeeded.
    return {
        "total_tests": total_tests, "successful_tests": 0, "failed_tests": failed_tests,
        "success_rate": 0.0, "average_metrics": dict.fromkeys(_METRIC_NAMES, 0.0),
        "min_ragas_score": 0, "max_ragas_score": 0,
    }
```

「保留成功路径返回的同一组键——置零而不是省略——好让 `run()` 这样的调用方**无条件读取**而不是先分支判断有没有成功过」。

而 `_METRIC_NAMES` 被提取成一个模块常量，注释说：「在基准统计里平均的指标名，共用**好让 `_calculate_benchmark_stats` 的全失败分支与有成功分支就键达成一致**」。

这与 h03 里「占位符预留与展开共用同一个正则」、i07 里「三个布尔解析器共用词表」是同一个手法：**两个必须一致的地方共用一个常量**。而这里的风险是具体的——若两个分支的键集不同，`run()` 在全失败时会 `KeyError`，而那是最不该崩的时刻。

**`generate_rag_response` 的关键是「一次调用拿回答案与真实检索块」。**

```python
payload = {
    "query": question, "mode": "mix", "include_references": True,
    "include_chunk_content": True,  # NEW: Request chunk content in references
    "response_type": "Multiple Paragraphs",
    "top_k": int(os.getenv("EVAL_QUERY_TOP_K", "10")),
}
```

而注释里那句 `# NEW:` 说明这是一个**重要的修正**，而下面 `evaluate_single_case` 里有一行更明确：

```python
# *** CRITICAL FIX: Use actual retrieved contexts, NOT ground_truth ***
```

所以要点是：评估 `context_recall` / `context_precision` 时，用来打分的「上下文」必须是**系统实际检索到的块**，而不是数据集的 `ground_truth`。

若误用 `ground_truth` 当作 contexts，那么 `context_recall` 会恒为 1.0（因为「期望的答案就在期望的上下文里」），于是这个指标**完全失去意义**——它测的是「数据集自洽性」而不是「检索质量」。而由于数值看起来很好，这个 bug 极难被发现。

而 `include_chunk_content: True` 让 API 在 references 里返回块正文，于是不需要第二次请求。docstring 称之为「单次优化的 API 调用——同时拿到答案与块内容」。

**而 contexts 的提取处理了「一个引用包含多个块」这个情形。**

```python
contexts = []
for ref in references:
    content = ref.get("content", [])
    if isinstance(content, list):
        contexts.extend(content)          # 每个块成为一个独立 context
    elif isinstance(content, str):
        contexts.append(content)          # 向后兼容
```

注释说明了演化：「`content` 现在是一个引用下的块列表（一个文件可能有多个块）」。所以一个文件检索到 3 段，就产生 3 个 context 而不是 1 个——这对 `context_precision` 很重要，因为它评估的是「检索到的上下文里有多少是相关的」，而粒度过粗（整个文件当一个 context）会让这个指标失真。

`elif isinstance(content, str)` 那段标了「向后兼容：如果 `content` 还是字符串（不该发生）」——一个残留分支，说明 API 曾经返回过字符串。

**四个异常分支各给一条可执行的错误消息，而这是这一层最实用的部分。**

```python
except httpx.ConnectError as e:
    raise Exception(f"Cannot connect to LightRAG API at {self.rag_api_url}\n   Make sure LightRAG server is running:\n   python -m lightrag.api.lightrag_server\n   Error: {str(e)}")

except httpx.HTTPStatusError as e:
    raise Exception(f"LightRAG API error {e.response.status_code}: {e.response.text}")

except httpx.ReadTimeout as e:
    raise Exception(f"Request timeout after waiting for response\n   Question: {question[:100]}...\n   Error: {str(e)}")

except Exception as e:
    raise Exception(f"Error calling LightRAG API: {type(e).__name__}: {str(e)}")
```

四条各自不同：

1. **连接失败** → 给出启动命令。所以运维不需要查文档就知道下一步。
2. **HTTP 状态错** → 带上**状态码与响应体**。这是唯一包含服务端解释的分支。
3. **读超时** → 带上**问题前 100 字符**。因为超时通常与具体查询相关（某个查询特别慢），而不是普遍故障。
4. **兜底** → 带上异常**类型名**与消息。`type(e).__name__` 在这里很关键，因为 `str(e)` 对某些异常是空的。

第 3 条的「问题前 100 字符」是一个很好的诊断细节：超时是最难定位的一类失败，而「哪个查询超时了」是第一手信息。截断到 100 字符避免日志被长查询淹没。

**而 `generate_rag_response` 的返回契约让上游可以只关心两个字段。**

```python
return {"answer": answer, "contexts": contexts}
```

两个字段正好是 RAGAS 需要的：`answer` 用于 `faithfulness` 与 `answer_relevance`，`contexts` 用于 `context_recall` 与 `context_precision`。

而 `answer` 的兜底是 `result.get("response", "No response generated")`——所以「API 返回了但没有 response 字段」会产生一个字符串而不是 `None`。这看起来是好事，但对 `faithfulness` 而言，`"No response generated"` 会被当成一个真实答案去评估，它的得分会很低但不为零。

这是一个可以商量的地方：更严格的做法是「没有 response 就当作失败」。而当前的选择让评估能继续跑完，代价是一个语义上可疑的分数。不过因为 `_METRIC_NAMES` 的统计有 `failed_tests` 字段，这种「假成功」不会被完全掩盖。

**`evaluate_single_case` 用两级信号量，而它的注释说明了为什么需要两级。**

```python
# rag_semaphore controls the entire evaluation process to prevent
# all RAG responses from being generated at once when eval is slow
async with rag_semaphore:
    ...
    # Stage 1: Generate RAG response
    # Stage 2: RAGAS evaluation (under eval_semaphore)
```

所以 `rag_semaphore` 覆盖整个函数（两阶段），而 `eval_semaphore` 只覆盖第二阶段。理由是：「防止在评估慢的时候所有 RAG 响应被一次性生成」。

这是一个**两阶段管线的背压**问题：若只有一个信号量覆盖全局，那么当 RAGAS 评估（第二阶段的 LLM 调用）变慢时，所有 worker 都会卡在第二阶段，而第一阶段的并发度也降到零。用两级信号量让「第一阶段的并发」与「第二阶段的并发」可以独立控制。

而 `position_pool: asyncio.Queue` 与 `pbar_creation_lock: asyncio.Lock` 是为 `tqdm` 多行进度条服务的：每个用例需要一个固定的行号，而池子提供可复用的行号，锁保护「取行号 + 创建 pbar」这个序列不被并发打乱。

第 1 阶段的失败处理把错误记录进结果而不抛：

```python
except Exception as e:
    logger.error("Error generating response for test %s: %s", idx, str(e))
    progress_counter["completed"] += 1
    return {
        "test_number": idx, "question": question, "error": str(e),
        "metrics": {}, "ragas_score": 0, "timestamp": datetime.now().isoformat(),
    }
```

所以单个用例失败不中断整批评估。而 `"metrics": {}` 让它在 `_calculate_benchmark_stats` 里被归为 `failed_tests`（因为 `valid_results = [r for r in results if r.get("metrics")]` 会过滤掉空 dict）。

这里有一个隐含的一致性：**失败结果必须带空的 `metrics` 才能被正确分类**。若失败结果不写 `metrics` 键，那 `r.get("metrics")` 返回 `None`（falsy），同样被过滤——所以两种写法等价。但显式写 `{}` 更好，因为它让结果的结构统一。

## 关键取舍

**源码事实：** `offline_retrieval_check` 零依赖、不启动 LightRAG、不算嵌入、不调 LLM——它是一个毫秒级回归基线，用于区分「样例数据坏了」与「系统退化了」。

**源码事实：** 分词先做 NFKC 规范化再 `lower()`，所以全角 ASCII 匹配 ASCII 形式、半角片假名匹配全角；ASCII 词丢停用词与单字符；CJK 单字保留（「一个表意字常常就是一整个词」）、多字转**重叠**双字（与 Lucene 的 CJK analyzer 一致）。

**源码事实：** `_CJK_CHARS` 在假名区间**断开**略过 U+30A0 与片假名中点 U+30FB，因为「它们是标点，所以 `データ・ベース` 不会产生一个跨越那个点的双字」；保留 U+3005 迭代符因为它是 `人々` 这类词的一部分。

**源码事实：** IDF 用平滑式 `log((N+1)/(df+1)) + 1`，分子分母的 `+1` 避免除零，结果的 `+1` 保证「恒为正」——所以「出现在所有文档里的词」IDF 为 1 而不是 0，于是匹配总是产生正分数，`if score > 0` 等价于「至少匹配一个 token」。

**源码事实：** `audit_samples` 排序用 `(-score, document.name)` 双键，所以同样的输入永远产生同样的 rank 列表——这让「recall 从 0.8 掉到 0.7」成为可复现信号。

**源码事实：** `recall_at` 的除零风险在 `load_oracle` 处被挡（`not expected` 拒绝空列表），所以「oracle 缺期望文档」是加载期错误而不是运行期除零。

**源码事实：** `summarize` 除了平均 recall 与 MRR，还输出 `full_recall_queries` 与 `no_hit_queries` 两个端点计数，因为平均值掩盖分布：「5 个满分 + 5 个零分」与「10 个 0.5」平均都是 0.5，但问题完全不同。

**源码事实：** `main` 的退出码三档：`2` = 检查本身跑不了（参数错 / 文件读不了 / JSON 坏 / oracle 缺项），`1` = 跑了但 `--strict` 下没全对，`0` = 正常——「测试基础设施坏了」与「被测的东西退化了」需要不同的响应。

**源码事实：** RAGAS 的 `ImportError` 被保存成 `_RAGAS_IMPORT_ERROR`，因为「没装」与「装了但版本冲突」的修法不同，报「请安装 ragas」会让后者被反复重装而无效。

**源码事实：** `ragas.metrics` 的顶层指标导入被包在 `warnings.catch_warnings()` 里，因为「collections 的指标不被 `ragas.evaluate()` 接受且拒绝 `LangchainLLMWrapper`，所以它们不是可替代的」——用废弃 API 是有意的。

**源码事实：** `_is_nan` 用 `isinstance(value, float) and math.isnan(value)`，因为 `math.isnan` 对非 float 抛 `TypeError`，而指标字典里可能有 `None`、字符串或缺失键。

**源码事实：** `_calculate_benchmark_stats` 让**每个指标有自己的 `count`**，因为 NaN 意味着「这个指标对这个样本不适用」（例如缺 ground truth 时 `context_recall` 是 NaN）而不是「得分为零」；共用一个 count 会把「无法评估」变成「零分」。

**源码事实：** 「全部失败」分支返回**同样的键、置零**而不是省略，因为「`run()` 这样的调用方可以无条件读取 `average_metrics` / `min_ragas_score` / `max_ragas_score` 而不必先分支」；`_METRIC_NAMES` 是共用常量，「好让两个分支就键达成一致」。

**源码事实：** `include_chunk_content: True` 让一次调用同时拿回答案与块正文，避免第二次请求；而 `evaluate_single_case` 里标着 `*** CRITICAL FIX ***`：「用实际检索到的上下文，**不是** `ground_truth`」——若误用 ground truth，`context_recall` 会恒为 1.0 而该指标完全失去意义。

**源码事实：** contexts 提取处理「一个引用含多个块」：`isinstance(content, list)` 时 `extend`（每块一个 context），`str` 时 `append`（向后兼容）；粒度对 `context_precision` 很重要，整个文件当一个 context 会让该指标失真。

**源码事实：** 四个 HTTP 异常分支各给可执行信息：连接失败给启动命令、状态错给状态码与响应体、读超时给问题前 100 字符（「哪个查询超时了」是第一手信息）、兜底给异常类型名（因为 `str(e)` 对某些异常为空）。

**源码事实：** `evaluate_single_case` 用两级信号量：`rag_semaphore` 覆盖整个函数、`eval_semaphore` 只覆盖 RAGAS 阶段——理由是「防止在评估慢的时候所有 RAG 响应被一次性生成」。

**架构推论：** 两个工具的分工是「便宜的先跑」。`offline_retrieval_check` 毫秒级且零依赖，所以它可以在每次提交时跑；`eval_rag_quality` 分钟级且需要整套服务，所以它只能在发布前跑。而这个梯度让「样例数据被改坏」这类问题在几十秒内被发现，而不是等到一小时后的端到端评估里以一个无法归因的低分浮现。

**架构推论：** 「NaN ≠ 零」这条区分是评估工具里最容易做错的一处。零是一个有意义的观测（模型确实给出了无关答案），NaN 是一个缺失的观测（这个指标对这个样本不适用）。把两者混在一起会让一个「一半样本无法评估」的报告看起来像「质量下降了一半」。正确做法是分别计数并在输出里同时呈现——而这一层用「每指标独立 count + 失败计数」做到了。

**架构推论：** 「错误消息要给出下一步动作」在这两个文件里反复出现：连接失败给启动命令、超时给问题前 100 字符、`--strict` 的 help 说明它的用途、`not expected` 的错误说「每个 oracle 条目需要 question 和 expected_documents」。这类消息的价值不在「说明发生了什么」，而在「避免读者还需再查一次文档」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/evaluation/offline_retrieval_check.py` | `tokenize` | NFKC + 停用词 + CJK 重叠双字 |
| `lightrag/evaluation/offline_retrieval_check.py` | `inverse_document_frequency` | 平滑 IDF，保证恒正 |
| `lightrag/evaluation/offline_retrieval_check.py` | `score_query` | `1 + log(tf)` 乘 IDF 的 TF-IDF |
| `lightrag/evaluation/offline_retrieval_check.py` | `audit_samples` | 确定性排序（分数降序 + 文件名升序） |
| `lightrag/evaluation/offline_retrieval_check.py` | `QueryResult` | `recall_at` 与 `reciprocal_rank` |
| `lightrag/evaluation/offline_retrieval_check.py` | `summarize` | 平均 + 两个端点计数 |
| `lightrag/evaluation/offline_retrieval_check.py` | `main` | 三档退出码，`--strict` 启用 CI 模式 |
| `lightrag/evaluation/offline_retrieval_check.py` | `_CJK_CHARS` | 假名区间在标点处断开 |
| `lightrag/evaluation/eval_rag_quality.py` | `RAGEvaluator` | 端到端评估器，含数据集加载与结果导出 |
| `lightrag/evaluation/eval_rag_quality.py` | `_is_nan` | 对非 float 安全的 NaN 判据 |
| `lightrag/evaluation/eval_rag_quality.py` | `_calculate_benchmark_stats` | 每指标独立 count，全失败分支返回同构键 |
| `lightrag/evaluation/eval_rag_quality.py` | `generate_rag_response` | 一次调用拿回答案与真实块，四个错误分支 |
| `lightrag/evaluation/eval_rag_quality.py` | `evaluate_single_case` | 两级信号量，单用例失败不中断整批 |
| `lightrag/evaluation/eval_rag_quality.py` | `_METRIC_NAMES` | 两个分支共用的键集常量 |
| `lightrag/evaluation/eval_rag_quality.py` | `_RAGAS_IMPORT_ERROR` | 保留导入失败原因以区分「没装」与「坏了」 |

## 参考

- [lightrag 源码：evaluation/eval_rag_quality.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/evaluation/eval_rag_quality.py)
- [lightrag 源码：evaluation/offline_retrieval_check.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/evaluation/offline_retrieval_check.py)
- [RAGAS 文档](https://docs.ragas.io/)
- [Unicode 规范化形式](https://www.unicode.org/reports/tr15/)

## 与主核 / AI 的关系

主核里最接近的是**回归测试套件与基准测试的分层**：一套小的、每次提交都跑的正确性测试，加上一套大的、发布前跑的基准。这里的两个工具是同一分层在 RAG 系统上的实例——`offline_retrieval_check` 是 smoke test，`eval_rag_quality` 是 benchmark。

**差别在于「被测量的是什么」。** 主核的测试测量确定性的行为（给定输入得到给定输出），所以可以「通过/失败」。而 RAG 质量是一个连续量，所以这里的工具输出的是**数字**而不是通过/失败——而 `--strict` 那个开关正好是「把数字变成判定」的边界，它只在「样例数据必须完全自洽」这个确定性子集上合理。

与 g04（嵌入基线）的关系：那里用余弦相似度做判据，与这里的 RAGAS 指标一样是「连续量 + 阈值」的判定形态。而两处都需要「不确定」这一档——g04 用三值（采纳/拒绝/不确定），这里用「NaN 不计入平均」。

与 j01（重排）的关系：`EVAL_QUERY_TOP_K` 控制评估时的检索数量，而重排是检索之后的一个可选阶段——所以评估结果会受重排配置影响。

对 AI 工程最实用的一条是 **`context_recall` 必须用实际检索到的上下文**。这个 bug 的形状值得记住：用 `ground_truth` 当 contexts 会让 `context_recall` 恒为 1.0，而报告看起来「召回完美」——于是团队不会去改检索，而真正的检索问题被一个假指标掩盖。判据是：**这个指标的输入是否来自被评估的系统本身？** 若不是，那它测的不是系统。

另一条是「NaN 与零必须分开」。任何聚合多指标的评估报告都会遇到某个指标在某些样本上不适用。把 NaN 当 0 会计入平均，于是「无法评估」变成「得了零分」，而报告会显示「质量下降」。正确做法是每指标独立计数，并在报告里同时给出「多少个样本成功」。

最后一条是离线检查的确定性排序。`(-score, name)` 双键让「同样的输入产生同样的输出」——而没有这一点，一个「分数相同但顺序不同」的运行会让 recall@1 抖动，于是无法判断「这次改动是让效果变差了还是只是换了顺序」。任何以「前 k 个」为核心指标的测试都需要一个确定的次级排序键。
