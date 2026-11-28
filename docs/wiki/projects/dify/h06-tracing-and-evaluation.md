---
project: dify
feature: tracing-and-evaluation
title: 追踪与评测
page_type: feature
schema_version: 1
refs:
  - path: api/services/tracing/canonical.py
    symbols: [CanonicalTraceBuilder]
  - path: api/core/evaluation/evaluation_manager.py
    symbols: [EvaluationManager]
  - path: api/core/evaluation/frameworks/ragas/ragas_evaluator.py
    symbols: [RagasEvaluator]
  - path: api/core/evaluation/frameworks/deepeval/deepeval_evaluator.py
    symbols: [DeepEvalEvaluator]
---

# 追踪与评测

## 一句话职责

`CanonicalTraceBuilder` 把四种内部追踪记录（工作流、消息、审核、建议问题……）**拍平成同一种「trace + span 树」**再交给外部可观测后端；`EvaluationManager` + `RagasEvaluator` / `DeepEvalEvaluator` 则把「评什么、怎么评」从两个第三方评测框架里抽出统一接口，让上层只认「指标名 + 类别」。

## 机制是什么

这两个子系统解决的是同一个形状的问题：**外部世界有两套词汇，本仓需要一套自己的。**

**追踪侧**：本仓内部有 `MessageTraceInfo` / `WorkflowTraceInfo` / `ModerationTraceInfo` / `SuggestedQuestionTraceInfo` / `DatasetRetrievalTraceInfo` / `ToolTraceInfo` 等多种记录（每种对应产品里一种可观测对象）。而外部可观测后端（Langfuse、LangSmith 之类）只认一种模型：**trace 是一棵 span 树**，每个 span 有 `kind`（`llm` / `retriever` / `tool` / `agent` / `chain`）、时间区间、输入输出、状态。

于是 `CanonicalTraceBuilder` 是一个**投影函数**：把任意一种内部记录变成「一棵 trace 树」。它的 docstring 一句话说明契约：

> Convert records into canonical trace units; ``None`` means nothing to send.

**「`None` 表示什么都不用发」** ——这是关键：`build` 的返回类型是 `Trace | None`，而**返回 `None` 不是一个错误，是一个合法结果**。什么时候返回 `None`？当这条记录不携带足够信息构造一个合法 span（下面具体讲）。

**评测侧**：`EvaluationManager` 面对两个第三方框架（RAGAS、DeepEval），而两者对「同一个指标」的命名、构造参数、同步/异步方式都不同。上层接口是统一的四件事：`get_supported_metrics` / `metric_requires_embeddings` / `metric_requires_expected_output` / `evaluate_*`。

## 怎么实现的

**`EvaluationManager` 的骨架只有两个静态方法和一个注册表类。**

```python
class EvaluationFrameworkConfigMap:
    """Registry mapping framework enum -> {config_class, evaluator_class}."""

    def __getitem__(self, key: EvaluationFrameworkEnum) -> dict[str, Any]:
        match key:
            case EvaluationFrameworkEnum.RAGAS:
                from core.evaluation.entities.config_entity import RagasConfig
                from core.evaluation.frameworks.ragas.ragas_evaluator import RagasEvaluator

                return {
                    "config_class": RagasConfig,
                    "evaluator_class": RagasEvaluator,
                }
            case EvaluationFrameworkEnum.DEEPEVAL:
                ...
            case _:
                raise ValueError(f"Unknown evaluation framework: {key}")
```

**五处：**

1. **它用 `__getitem__` 而不是 `get(key)` 方法** ——于是调用方写 `config_map[framework]`。这让「查表失败要抛错」成为语法上的自然结果：`[]` 本来就该抛 `KeyError`，这里抛的是 `ValueError`。
2. **import 写在 `case` 分支内部** ——这是**延迟导入**：不装 RAGAS 的部署不会因为 import 这个模块而失败。**这与 h02 的「常驻子进程按需启动」是同一类取舍：把「依赖是否存在」的检查推迟到真正用它的时刻。**
3. **返回一个裸 `dict[str, Any]` 而不是 dataclass** ——于是键名 `"config_class"` / `"evaluator_class"` 是字符串约定，**拼错不会报错**（下面 `get_evaluation_instance` 里用 `config_map["evaluator_class"]` 是直接下标，拼错会 `KeyError`，但类型检查器看不出来）。
4. **`case _: raise ValueError`** ——`match` 的兜底分支。若 `EvaluationFrameworkEnum` 新增一个成员而忘了加分支，这里会抛错（好）。
5. **模块级实例化 `evaluation_framework_config_map = EvaluationFrameworkConfigMap()`** ——一个无状态的单例对象。它存在的唯一理由是让 `[]` 语法可用。

**`get_evaluation_instance` 把「默认值」与「枚举转换」压在一行。**

```python
@staticmethod
def get_evaluation_instance(
    framework: str | EvaluationFrameworkEnum | None = None,
) -> BaseEvaluationInstance | None:
    """Create an evaluator for the selected framework; legacy callers default to Ragas."""
    selected_framework = EvaluationFrameworkEnum.RAGAS if framework is None else EvaluationFrameworkEnum(framework)
    if selected_framework == EvaluationFrameworkEnum.NONE:
        return None

    config_map = evaluation_framework_config_map[selected_framework]
    evaluator_class = config_map["evaluator_class"]
    config_class = config_map["config_class"]
    return evaluator_class(config_class())
```

**四处：**

1. **`framework is None` → 默认 RAGAS**，docstring 说这是为「legacy callers」。**这是一个向后兼容的默认值**：新代码应当显式传框架，而老代码没传时行为不变。
2. **`EvaluationFrameworkEnum(framework)` 同时在处理 `str` 与枚举两种输入** ——传枚举成员时这个转换是幂等的，传字符串时它做校验。**一个参数两种类型，靠枚举构造器统一**。
3. **`NONE` 是一个枚举成员，返回 `None` 而不是抛错** ——「显式关闭评测」是一个合法选择，而不是配置错误。
4. **每次调用都新建 `config_class()` 与 evaluator** ——**没有缓存**。对一个「每次评测都跑几十条数据」的场景，这点开销可以忽略；但它意味着两次调用得到两个互不共享状态的实例。**这正是 RAGAS/DeepEval 需要的**（两个评测器的指标对象是有状态的，见下）。

**`get_supported_metrics` 是一个「通过实例转发」的薄方法。**

```python
@staticmethod
def get_supported_metrics(category: EvaluationCategory, framework: str | EvaluationFrameworkEnum | None = None) -> list[str]:
    """Return supported metrics for the selected framework and category."""
    instance = EvaluationManager.get_evaluation_instance(framework)
    if instance is None:
        return []
    return instance.get_supported_metrics(category)
```

**`instance is None` 时返回空列表** ——与 h04 里 `builtin_permissions` 对非法角色返回空权限同向（fail-soft）。**于是「框架被关闭」与「这个类别没有任何指标」在返回值上不可区分。**

**`RagasEvaluator` 用四张表把「指标名」映射到「RAGAS 的类/字段/依赖」。**

```python
_RAGAS_METRIC_MAP: dict[EvaluationMetricName, str] = {
    EvaluationMetricName.FAITHFULNESS: "Faithfulness",
    EvaluationMetricName.ANSWER_RELEVANCY: "AnswerRelevancy",
    EvaluationMetricName.ANSWER_CORRECTNESS: "AnswerCorrectness",
    EvaluationMetricName.SEMANTIC_SIMILARITY: "SemanticSimilarity",
    EvaluationMetricName.CONTEXT_PRECISION: "ContextPrecisionWithReference",
    EvaluationMetricName.CONTEXT_RECALL: "ContextRecall",
    EvaluationMetricName.CONTEXT_RELEVANCE: "ContextRelevance",
    EvaluationMetricName.TOOL_CALL_ACCURACY: "ToolCallAccuracy",
}

_RAGAS_EMBEDDING_METRICS = {ANSWER_RELEVANCY.value, ANSWER_CORRECTNESS.value, SEMANTIC_SIMILARITY.value}
_RAGAS_REFERENCE_METRICS = {ANSWER_CORRECTNESS.value, SEMANTIC_SIMILARITY.value, CONTEXT_PRECISION.value, CONTEXT_RECALL.value}

_RAGAS_METRIC_FIELDS: dict[str, tuple[str, ...]] = {
    EvaluationMetricName.FAITHFULNESS: ("user_input", "response", "retrieved_contexts"),
    EvaluationMetricName.ANSWER_RELEVANCY: ("user_input", "response"),
    ...
}
```

**四处设计点：**

1. **`_RAGAS_METRIC_MAP` 的值是**字符串类名**而不是类本身** ——因为类要延迟导入（`_import_ragas_metrics_module()`）。**于是这张表只负责「本仓名 → RAGAS 名」，真类在 `_build_ragas_metrics` 里按需从模块上取。**
2. **`_RAGAS_METRIC_MAP` 的键是枚举，而 `_RAGAS_EMBEDDING_METRICS` / `_RAGAS_REFERENCE_METRICS` 的成员是 `.value` 字符串** ——**同一个枚举一半用成员一半用值**。这导致 `get_supported_metrics` 里那句双重检查：

```python
return [m.value for m in candidates if m in _RAGAS_METRIC_MAP or m.value in _NATIVE_AGENT_METRICS]
```

`m in _RAGAS_METRIC_MAP`（枚举成员查找）`or m.value in _NATIVE_AGENT_METRICS`（字符串查找）——**两种查找方式的并集**，因为有两套不同的原生指标集合。**这是「指标来源有两个」造成的真实复杂度**：一部分指标由 RAGAS 提供，一部分由本仓原生实现（`NATIVE_AGENT_METRIC_NAMES`）。

3. **`_RAGAS_METRIC_FIELDS` 是「这个指标需要哪些输入字段」的声明** ——它是 `_score_ragas_items` 里做「按指标取子集」的依据：

```python
fields = _RAGAS_METRIC_FIELDS[metric.name]
...
inputs = {
    "user_input": _format_input(item.inputs, category),
    "response": item.output,
    "reference": item.expected_output or "",
    "retrieved_contexts": item.context or [],
}
kwargs = {field: inputs[field] for field in fields}
```

**注意 `_RAGAS_METRIC_FIELDS[metric.name]` 是直接下标** ——注释说这是前置条件：「Precondition: each requested metric has a declared Ragas field mapping」。**若某个指标进了 `ragas_metrics` 却没进这张表，会 `KeyError`。** 而 `_build_ragas_metrics` 只从 `ragas_class_map`（七个指标）里建对象，`TOOL_CALL_ACCURACY` 不在这七个里——所以它不会走到这里。**这个安全性是靠两张表的成员集合一致来维持的，而代码里没有断言。**

4. **`_RAGAS_REFERENCE_METRICS` 与 `metric_requires_expected_output` 一一对应**：

```python
@override
def metric_requires_expected_output(self, metric_name: str) -> bool:
    return metric_name in _RAGAS_REFERENCE_METRICS
```

**这个「声明」比「用的时候才发现字段为空」重要得多**：上层可以据此在**发起评测前**提示用户「这个指标需要标准答案」。**这是把运行期的隐含需求暴露成接口的一部分** ——与 h04 里 `from_other_member=True` 标注例外是同一类做法。

**`_score_ragas_metric` 的重试只针对三类异常，而理由写在注释里。**

```python
# Retry only provider failures known to be transient. Client-side validation
# failures such as PluginInvokeError must surface immediately.
_RAGAS_RETRYABLE_EXCEPTIONS = (
    InvokeConnectionError,
    InvokeRateLimitError,
    InvokeServerUnavailableError,
)

async def _score_ragas_metric(metric: Any, kwargs: dict[str, Any]) -> Any:
    """Bound each score attempt and retry only transient provider failures."""
    for attempt in range(_RAGAS_METRIC_RETRIES):
        try:
            return await asyncio.wait_for(metric.ascore(**kwargs), timeout=_RAGAS_METRIC_TIMEOUT_SECONDS)
        except _RAGAS_RETRYABLE_EXCEPTIONS:
            if attempt == _RAGAS_METRIC_RETRIES - 1:
                raise
            await asyncio.sleep(min(2**attempt, 60))
    raise AssertionError("RAGAS metric retry loop ended without a result")
```

**五处：**

1. **只重试三类**（连接、限流、服务不可用）——**注释说明了排除项**：「Client-side validation failures such as `PluginInvokeError` must surface immediately」。**这是「可重试」判据的正确形状：按异常类型白名单，而不是按「除了这些之外都重试」。**
2. **`asyncio.wait_for(..., timeout=180)`** ——每次尝试单独限时（`_RAGAS_METRIC_TIMEOUT_SECONDS = 180`）。于是**总耗时上界 = 10 × 180 秒 = 30 分钟**，这是一个很宽松的上界（下面还会提）。
3. **`min(2**attempt, 60)`** ——指数退避封顶 60 秒。第 6 次之后每次等 60 秒。
4. **循环结束后 `raise AssertionError`** ——**这行在正确性上是不可达的**（`range(10)` 里最后一次要么 `return` 要么 `raise`）。但它保护了「未来有人把最后一次的 `raise` 改掉」这种情况，代价是给类型检查器/读者一个「这里可能正常退出」的假象。
5. **`_RAGAS_METRIC_RETRIES = 10`** ——十次重试对一个 LLM 评测指标是很高的（意味着最多 11 次调用）。

**`_score_ragas_items` 的并发结构是「全笛卡尔积 + 信号量 + 结果按原序装配」。**

```python
async def _score_ragas_items(items, metrics, category) -> list[tuple[list[EvaluationMetric], list[str]]]:
    """Score rows with bounded concurrency while preserving input and metric order.

    Precondition: each requested metric has a declared Ragas field mapping.
    Postcondition: a missing text context fails only metrics requiring it;
    other row metrics still score, and no provider request uses empty evidence.
    """
    semaphore = asyncio.Semaphore(_RAGAS_MAX_WORKERS)

    async def score_one(item_index: int, metric: Any) -> tuple[int, EvaluationMetric | None, str | None]:
        item = items[item_index]
        fields = _RAGAS_METRIC_FIELDS[metric.name]
        if "retrieved_contexts" in fields and not any(context.strip() for context in item.context or []):
            failure = new_public_evaluation_error(EvaluationErrorCode.CONTEXT_MISSING)
            ...
            return item_index, None, failure.text
        ...
        async with semaphore:
            try:
                with use_plugin_daemon_request_timeout(_RAGAS_METRIC_TIMEOUT_SECONDS):
                    result = await _score_ragas_metric(metric, kwargs)
                score = float(result.value)
                if not math.isfinite(score):
                    raise ValueError(f"RAGAS returned a non-finite score: {score}")
                return item_index, EvaluationMetric(name=metric.name, value=score), None
            except Exception:
                failure = new_public_evaluation_error(EvaluationErrorCode.METRIC_FAILED)
                logger.exception(...)
                return item_index, None, failure.text

    outcomes = await asyncio.gather(*(score_one(index, metric) for index in range(len(items)) for metric in metrics))
    scored: list[tuple[list[EvaluationMetric], list[str]]] = [([], []) for _ in items]
    for index, metric, error in outcomes:
        if metric is not None:
            scored[index][0].append(metric)
        if error is not None:
            scored[index][1].append(error)
    return scored
```

**七处，每一处都在处理「部分失败」这个主题：**

1. **`asyncio.gather` 同时发起 `len(items) × len(metrics)` 个协程** ——于是**全部任务同时创建**，靠 `Semaphore(16)` 限流。**注意信号量在 `_score_ragas_metric` 外面**，所以「重试」整体占一个许可（正确：一次逻辑评分只占一个槽）。
2. **`CONTEXT_MISSING` 的检查在 `async with semaphore` 之前** ——即「缺上下文」这一判断**不占并发额度**。这是一个正确的小优化：它不需要网络。
3. **`if "retrieved_contexts" in fields and not any(context.strip() ...)`** ——**只对「需要上下文」的指标报缺失**（对应 docstring 的 postcondition 第 1 条：「a missing text context fails only metrics requiring it」）。于是同一行数据，`Faithfulness` 会报 `CONTEXT_MISSING` 而 `AnswerRelevancy` 正常评分。
4. **`not any(context.strip() for context in item.context or [])`** ——**用 `.strip()` 判断「有没有可用文本」**，所以「全是空白字符的上下文」等同缺失。对应 postcondition 第 2 条：「no provider request uses empty evidence」。**这个检查值钱的地方在于：它避免了「把空上下文发给 LLM 并得到一个看似合法的低分」。**
5. **`except Exception` 包住整个评分块** ——任何失败（含非有限分数）都变成一个 `METRIC_FAILED` 的公开错误，而**原始异常只进日志**。`logger.exception(... error_id=failure.error_id)` 把两者用 `error_id` 关联起来。
6. **`math.isfinite(score)` 检查** ——`NaN` / `inf` 被升格成异常再被接住。**这是一个真实的坑**：`NaN` 会污染平均值且「看起来是个数字」。
7. **结果装配是「先建空表再按 index 填」** ——因为 `gather` 的返回顺序与提交顺序一致，但**每个 index 有多个指标**，所以要用 `scored[index][0].append(...)` 逐条累积。**这保证了「输入顺序与指标顺序」都被保留**（postcondition 第 0 条）。

**`_format_input` 用 `match` 按类别取不同的输入字段。**

```python
def _format_input(inputs: dict[str, Any], category: EvaluationCategory) -> str:
    """Extract the user-facing input string from the inputs dict."""
    match category:
        case EvaluationCategory.LLM | EvaluationCategory.WORKFLOW:
            return str(inputs.get("prompt", ""))
        case EvaluationCategory.RETRIEVAL:
            return str(inputs.get("query", ""))
        case EvaluationCategory.AGENT:
            return str(inputs.get("task", ""))
        case _:
            return str(next(iter(inputs.values()), "")) if inputs else ""
```

**三处：**

1. **四种类别四种键名**（`prompt` / `query` / `task`）——这是「同一个概念在不同入口叫不同名字」的直接后果，与 h01 的 `DISPLAY_STATUS_FILTERS` 同源。
2. **兜底分支取第一个值**（`next(iter(inputs.values()), "")`）——**任意的**。这是一个「宁可给个非空字符串」的选择：RAGAS 的指标若收到空输入可能报错或给无意义分数。
3. **两处 `.get(key, "")`** ——**缺字段时给空串而不是报错**。于是「类别与输入不匹配」（比如给 LLM 类别传了 `query`）会静默变成一个空 prompt。**这是一个真实的静默失败点。**

**`DeepEvalEvaluator` 与 `RagasEvaluator` 的结构对应，但三处策略不同。**

**第一处：缺上下文的处理位置不同。** RAGAS 在 `score_one` 里按指标判断；DeepEval 在主循环里先算一次全局布尔：

```python
has_text_context = any(context.strip() for context in item.context or [])
for canonical_name, metric in _build_deepeval_metrics(supported_metrics, model, components):
    if canonical_name in _DEEPEVAL_CONTEXT_METRICS and not has_text_context:
        failure = new_public_evaluation_error(EvaluationErrorCode.CONTEXT_MISSING)
        ...
```

**`has_text_context` 提到循环外算一次** ——因为 DeepEval 是逐条串行评测的（`async_mode=False`），没有并发的必要性。**而 `_DEEPEVAL_CONTEXT_METRICS` 是 `frozenset(...)`，用「集合成员」代替 RAGAS 的「字段列表包含 `retrieved_contexts`」** ——**同一个语义，两种判定方式。**

**第二处：失败与不支持的处理粒度不同。** DeepEval 里「所有请求指标都不支持」会**抛异常**：

```python
if not supported_metrics:
    raise ValueError(f"DeepEval does not support any requested metrics: {requested_metrics}")
```

而 RAGAS 里同类情况只是返回原样结果：

```python
if not ragas_metrics:
    logger.warning("No valid RAGAS metrics found for: %s", requested_metrics)
    return [EvaluationItemResult(index=item.index, actual_output=item.output) for item in items]
```

**同一个「一个指标都用不了」的场景，一个抛错一个返回空结果。** 而从用户的视角，「我要求的指标这个框架都不支持」应当是错误（否则用户拿到一堆没有分数的行，不知道为什么）。

**第三处：Agent 的特殊处理只在 DeepEval 侧。**

```python
if category == EvaluationCategory.AGENT and isinstance(item, AgentEvaluationItem) and not item.tool_calls:
    results.append(
        EvaluationItemResult(
            index=item.index,
            actual_output=item.output,
            metrics=[
                EvaluationMetric(
                    name=EvaluationMetricName.ARGUMENT_CORRECTNESS.value,
                    value=None,
                    details={
                        "status": "not_applicable",
                        "reason": "The Agent did not call any tools for this dataset row.",
                    },
                )
            ],
            error="; ".join(errors) if errors else None,
        )
    )
    continue
```

**「Agent 没调用任何工具」被表达成 `value=None` + `status: "not_applicable"` + 一句人话原因** ——**而不是 0 分或跳过该行**。这个区分极重要：**「不该评分」与「得了 0 分」在统计上必须分开**，否则「工具调用准确率」的分母会包含不该计的行，指标被系统性拉低。

**RAGAS 侧对同一情况没有这个分支** ——它会把「无 tool_calls」的样本发进 `TOOL_CALL_ACCURACY`。**这是两个框架在语义完备性上的真实差距**，而它藏在「DeepEval 比 RAGAS 多 20 行」这样的细节里。

**`deepdeval` 侧两个 `_evaluate` 的 docstring 写成了前置/后置条件，而这是本仓 AGENTS.md 的要求。**

```python
"""Score rows with supported DeepEval metrics and public failures.

Precondition: the selected category and metrics are from validated configuration.
Postcondition: each row has metric scores or approved public errors;
metrics requiring text context never score without usable evidence,
and raw provider exceptions remain in server logs.
"""
```

**三句话各自可验证：**

1. **precondition 说「类别与指标来自已校验的配置」** ——即本函数不负责校验配置合法性（那是上游的事）。
2. **postcondition 第 1 条说「每行要么有分数要么有公开错误」** ——这是可以逐行断言的。
3. **postcondition 第 2 条说「原始 provider 异常留在服务端日志」** ——对应 `logger.exception` + 只把 `failure.text` 放进结果。

**这是「契约式文档」的正确用法：把「我能保证什么」写成可检查的句子。** 而对照 RAGAS 侧同位置的 docstring（「Score rows with Ragas collections metrics through Dify-owned orchestration」），**信息量差距明显**——前者说了保证，后者只说了它做什么。

**`_build_deepeval_metrics` 的 docstring 揭示了一个必须遵守的约束。**

```python
def _build_deepeval_metrics(...) -> list[tuple[str, Any]]:
    """Build fresh stateful metric instances for one dataset item."""
```

**「fresh stateful」** ——三个词都很重要：

- **`stateful`**：DeepEval 的 metric 对象在 `measure()` 之后持有 `score` / `reason`；
- **`fresh`**：所以每行数据必须重建指标对象，否则上一行的分数会残留；
- **`for one dataset item`**：说明它是**逐条调用**的，而调用点在每条数据的循环里。

**这就是为什么 `_evaluate` 里 `model` 只在第一次 `is None` 时创建、而 metric 每条都重建**：

```python
if model is None:
    model = DifyDeepEvalModel(model_provider, model_name, tenant_id)
metrics: list[EvaluationMetric] = []
test_case = _build_test_case(item, category, components)
for canonical_name, metric in _build_deepeval_metrics(supported_metrics, model, components):
```

**model 可共享（无状态），metric 不可共享（有状态）。** 这个区分没有类型系统保证，只靠 docstring 与调用位置。

**`threshold=None` 与 `async_mode=False` 在所有指标构造里重复出现。**

```python
metric = metric_class(
    model=model,
    threshold=None,
    include_reason=True,
    async_mode=False,
    verbose_mode=False,
)
```

**三处：**

1. **`threshold=None`** ——DeepEval 原生会把分数与阈值比出「通过/不通过」，而这里不需要那个判定（本仓只取分数）。**`None` 表示「不判定」，是一个刻意的关闭。**
2. **`async_mode=False`** ——**关掉框架内部的并发**，于是「一行的多个指标」串行评测。理由是：本仓已经在外层做了并发（按行），框架内再并发会让「限流」变得不可控。**这是一个「并发控制权归谁」的决定。**
3. **`verbose_mode=False`** ——关掉框架往 stdout 打印进度。**这与 `CONFIG` 里其他地方对日志格式的要求一致**（本仓用自己的 logger）。

**而 `_ANSWER_CORRECTNESS_STEPS` 是为 `GEval` 写的一份「评分步骤」提示。**

```python
metric = components.GEval(
    name="Answer Correctness",
    evaluation_params=[
        components.SingleTurnParams.INPUT,
        components.SingleTurnParams.ACTUAL_OUTPUT,
        components.SingleTurnParams.EXPECTED_OUTPUT,
    ],
    evaluation_steps=_ANSWER_CORRECTNESS_STEPS,
    model=model,
    threshold=None,
    async_mode=False,
    verbose_mode=False,
    _include_g_eval_suffix=False,
)
```

**两处细节：**

1. **`GEval` 不是标准指标类，而是「用 LLM 按步骤打分」的通用构造器** ——所以它要显式给 `evaluation_params`（参与评分的字段）与 `evaluation_steps`（打分步骤）。**这等于把「Answer Correctness 怎么算」写成了自然语言提示**，而它被放在模块级常量里，说明作者认为它需要可读、可调整。
2. **`_include_g_eval_suffix=False`** ——**注意这个参数以单下划线开头**：它是框架的私有参数。**依赖第三方库的私有参数是一个脆弱点**（版本一升就可能改名），而这行代码不得不这么做——因为不改的话指标名会被加上 `[GEval]` 后缀，与 `_DEEPEVAL_METRICS` 里的规范化名字对不上。

**`CanonicalTraceBuilder.build` 用结构化模式匹配做记录类型分派。**

```python
def build(self, record: BaseTraceInfo) -> Trace | None:
    match record:
        case WorkflowTraceInfo():
            return self._workflow(record)
        case MessageTraceInfo():
            return self._message(record)
        case ModerationTraceInfo():
            return self._auxiliary(
                record,
                name="moderation",
                kind=SpanKind.TOOL,
                inputs=record.inputs,
                outputs={"action": record.action, "flagged": record.flagged},
                error=None,
            )
        ...
```

**四处：**

1. **`case WorkflowTraceInfo():` 是「类型匹配」** ——Python 的 `match` 支持按类匹配，于是这个分派等价于 `isinstance` 链但更紧凑。
2. **`WorkflowTraceInfo` 与 `MessageTraceInfo` 有专门的构造方法**（`_workflow` / `_message`），因为它们会产生**多个 span**；其余六种都走同一个 `_auxiliary`（**单 span**）。**这个二分是这套模型的核心**：有的记录本身就是一棵树，有的只是一个叶子。
3. **`_auxiliary` 的 `kind` 参数逐个指定**：`moderation` 是 `SpanKind.TOOL`、`suggested_question` 是 `TOOL`、`dataset_retrieval` 是 `RETRIEVER`。**这些映射是硬编码在调用点的**，而 `_NODE_KIND` 那张表只覆盖节点类型。
4. **`error=None` 硬编码给 `moderation`** ——因为审核记录没有 error 字段。**这是一个「不同记录字段集不同」导致的逐项适配。**

**`_workflow` 是整页最长的方法，而它的核心是「两种根节点的二选一」。**

```python
if record.message_id:
    # A chatflow run is shown under its message, which is the trace root.
    root_logical_id = record.message_id
    root_span_id = span_id_for(record.message_id)
    # Units attached to this message (tool calls, retrieval, ...) name its creation as the trace start.
    message_start = record.message_started_at or workflow_start
    spans.append(Span(span_id=root_span_id, parent_span_id=None, name="message", kind=SpanKind.CHAIN, ...))
    workflow_parent: str | None = root_span_id
else:
    root_logical_id = record.workflow_run_id
    workflow_parent = None
```

**「chatflow 的一次运行挂在它的消息下面，消息是 trace 的根」** ——**这是一个产品语义的判断**：对话型工作流的一次执行是「一条消息的一部分」，而独立工作流运行才是根。

**代价是两条路径的 span 树形状不同**：有消息时是 `message → workflow → nodes`，无消息时是 `workflow → nodes`。**而下游的可观测后端看到的是「根 span 的 kind 可能是 `chain` 也可能就是 `workflow`」**，任何依赖「根一定是 workflow」的分析都会在 chatflow 上失效。

**`_parent_first` 是「保证 span 按父先序输出」的排序，而它的失败分支说明了一处不该发生的错误。**

```python
@staticmethod
def _parent_first(spans: Mapping[str, Span], emitted_ids: set[str]) -> list[Span]:
    ordered: list[Span] = []
    pending = sorted(spans.values(), key=lambda span: (span.start_time, span.span_id))
    while pending:
        ready = [span for span in pending if span.parent_span_id in emitted_ids]
        if not ready:
            # The hierarchy only names existing executions or wrappers as parents.
            raise ValueError("workflow span hierarchy is not a tree")
        for span in ready:
            ordered.append(span)
            emitted_ids.add(span.span_id)
        pending = [span for span in pending if span.span_id not in emitted_ids]
    return ordered
```

**五处：**

1. **`sorted(..., key=(start_time, span_id))`** ——**先按时间、再按 id**。第二个键是为了「同一时刻的多个 span 顺序确定」，否则输出顺序不稳定（而 span 树的输出顺序对可观测后端是有意义的）。
2. **`parent_span_id in emitted_ids`** ——父已输出的才算就绪。**初始的 `emitted_ids` 是 `{span.span_id for span in spans}`（已有的 message 与 workflow span）**，于是「以 workflow 为父的节点」第一轮就绪。
3. **`if not ready: raise`** ——**这是「环」或「指向不存在父」的检测**。注释断言「hierarchy 只会把已存在的 execution 或 wrapper 当父」，所以这种情况不该发生；真发生了就是 bug。
4. **这是一个 O(n²) 的算法**（每轮扫一遍 pending）——对一个工作流几十个节点完全够用。
5. **它不修改传入的 `spans`，只修改 `emitted_ids`** ——调用方传的是一个临时 set：

```python
spans.extend(self._parent_first(unordered, {span.span_id for span in spans}))
```

**这个内联的 set 构造让「已输出集合」的初值是「前面已经加进去的那些 span」** ——包含 message（可能）与 workflow。**若没有这一步，所有以 workflow 为父的节点都会被判成「父不存在」。**

**`_node_span` 里有一个「按 span 种类决定字段是否填充」的密集三元表达式段。**

```python
inputs=json_safe(process_data.get("prompts", []) if kind is SpanKind.LLM else dict(execution.inputs or {})),
outputs=json_safe(outputs),
model_name=process_data.get("model_name") if kind is SpanKind.LLM else None,
model_provider=process_data.get("model_provider") if kind is SpanKind.LLM else None,
usage=usage if kind is SpanKind.LLM else None,
cost=_cost(metadata.get("total_price"), metadata.get("currency")) if kind is SpanKind.LLM else None,
time_to_first_token=_seconds(time_to_first_token) if kind is SpanKind.LLM else None,
```

**五处：**

1. **五个字段都只在 LLM span 上填充** ——`model_name` / `model_provider` / `usage` / `cost` / `time_to_first_token` 对非 LLM 节点无意义。
2. **而 `kind` 是从 `_NODE_KIND.get(node_type, SpanKind.CHAIN)` 来的** ——只有四类节点（llm / knowledge-retrieval / tool / agent）有专属 kind，其余都是 `chain`。**所以「一个自定义节点若内部调了 LLM，它的 token 与成本不会出现在追踪里」** ——因为它的 kind 是 `chain`。
3. **`inputs` 的选择也按 kind**：LLM 取 `process_data["prompts"]`，其它取 `execution.inputs`。**同一个概念（输入）在两个来源，而选择依据是节点类型。**
4. **`_usage(...)` 在 `isinstance(usage_source, Mapping)` 检查后才调用** ——因为 `process_data.get("usage") or outputs.get("usage") or {}` 可能是任何类型。
5. **`usage` 与 `cost` 都做了「非有限值 → None」的过滤**（`_cost` 里 `math.isfinite` 与 `>= 0`，`_usage` 里 `>= 0` 且排除布尔）。**这一层防御是必要的：可观测后端对 `NaN` 的处理通常是「整条记录丢弃」或「污染聚合」。**

**`_auxiliary` 的 `None` 返回条件有两条，都在「信息不足」这个主题上。**

```python
message = record.message_data
operation_id = record.metadata.get("trace_event_id")
if not isinstance(operation_id, str):
    return None
started_at = record.start_time or (message.created_at if message is not None else None)
if started_at is None:
    return None
```

**两处：**

1. **没有 `trace_event_id` 就不发** ——因为**这个 id 是 span 身份的唯一来源**（`span_id_for(f"event:{operation_id}")`）。没有它就无法给 span 一个稳定 id，重复上报会导致后端出现重复 span。
2. **没有开始时间就不发** ——因为时间区间是 span 的必需属性，而「补一个当前时间」会造出错误的时间线。

**这两条合起来体现了 `build` 的契约**：**「信息不足时宁可不发，也不发一个残缺的 span」**。这与「可观测性数据宁可少不可错」的直觉一致——错误的 span 会污染调用链分析，缺失的只是少一条。

**而 `parent` 与 `parent_span_id` 的双写是一个冗余设计。**

```python
if message is None:
    trace_id = trace_id_for(record.trace_id or f"event:{operation_id}")
    parent = None
    parent_span_id = None
else:
    trace_id = message_trace_id(record.trace_id, message.id)
    parent = SpanRef(span_id=span_id_for(message.id), start_time=message.created_at)
    parent_span_id = parent.span_id
```

**`Span.parent_span_id` 是「我只知道父的 id」，而 `Trace.parent` 是「我知道父的 id 与开始时间」。** 后者用于「父 span 不在本次上报里」的情况——后端需要一个 `SpanRef` 才能在正确的 trace 里挂上这个 span（Otlp 的 `SpanRef` 语义）。

**于是同一个父被表达两次**，而它们必须一致。**这是一个「局部信息」与「跨 trace 引用」两种需求的并存**，代价是两处同步。

## 关键取舍

**源码事实：** `CanonicalTraceBuilder.build` 用结构化模式匹配分派六种记录；`WorkflowTraceInfo` / `MessageTraceInfo` 有专门的构造方法（会产生多 span），其余四种走 `_auxiliary`（单 span）。返回 `Trace | None`，`None` 表示「信息不足，不发」。

**源码事实：** `_workflow` 在 `record.message_id` 存在时把 trace 根设为 `message` span（chatflow 运行挂在消息下），否则根就是 `workflow` span；两条路径产生不同的树形状。

**源码事实：** `_parent_first` 用「父已输出」做拓扑排序，键为 `(start_time, span_id)`（第二键保证同刻顺序确定），初始已输出集合由调用方内联构造为「已有的 message + workflow span」；无法推进时抛 `ValueError("workflow span hierarchy is not a tree")`（O(n²) 算法，`n` 为节点数）。

**源码事实：** `_node_span` 中 `model_name` / `model_provider` / `usage` / `cost` / `time_to_first_token` 五个字段只在 `kind is SpanKind.LLM` 时填充；`kind` 来自 `_NODE_KIND`（仅 llm / knowledge-retrieval / tool / agent 四类，其余为 `chain`），因此自定义节点内部调 LLM 的 token 与成本不会出现在追踪里。

**源码事实：** `_cost` 要求 `amount` 非 `None` 且 `currency` 非空，并过滤非有限值与负数；`_usage` 的 `count` 排除 `bool`、要求 `>= 0`，`total` 缺失时用 prompt+completion 求和。`_seconds` 同样排除 `bool` 并要求非负有限。

**源码事实：** `_auxiliary` 在「`trace_event_id` 不是 str」或「没有开始时间」时返回 `None`；`parent`（`SpanRef`，带时间）与 `Span.parent_span_id` 双写表示同一个父。

**源码事实：** `EvaluationManager.get_evaluation_instance` 在 `framework is None` 时默认 RAGAS（docstring 说明为 legacy callers），`EvaluationFrameworkEnum.NONE` 时返回 `None`；每次调用新建 config 与 evaluator，不缓存。`get_supported_metrics` 在实例为 `None` 时返回空列表。

**源码事实：** `EvaluationFrameworkConfigMap` 用 `__getitem__` + `match` 做注册表，import 在分支内（延迟导入），`case _` 抛 `ValueError`；返回裸 `dict[str, Any]`，键名为字符串约定。

**源码事实：** `RagasEvaluator` 用四张表：`_RAGAS_METRIC_MAP`（枚举 → RAGAS 类名）、`_RAGAS_EMBEDDING_METRICS` / `_RAGAS_REFERENCE_METRICS`（`.value` 字符串集合）、`_RAGAS_METRIC_FIELDS`（指标 → 需要的输入字段）；`get_supported_metrics` 用「枚举成员查找 or 字符串查找」的并集来覆盖两套原生指标来源。

**源码事实：** `_RAGAS_METRIC_FIELDS[metric.name]` 是直接下标，安全性依赖「`_build_ragas_metrics` 只构造有字段声明的指标」，代码里没有断言；`_RAGAS_METRIC_MAP` 含 `ToolCallAccuracy` 而 `_RAGAS_METRIC_FIELDS` 不含它。

**源码事实：** `_score_ragas_metric` 只重试 `InvokeConnectionError` / `InvokeRateLimitError` / `InvokeServerUnavailableError`（注释说明客户端校验类错误如 `PluginInvokeError` 必须立即上抛），每次尝试 `asyncio.wait_for(timeout=180)`，10 次重试、退避 `min(2**attempt, 60)`；循环末尾 `raise AssertionError`（正常不可达）。

**源码事实：** `_score_ragas_items` 对「每条数据 × 每个指标」全量 `gather`，用 `Semaphore(16)` 限流；缺上下文的判定放在信号量**之外**，且只影响「字段列表含 `retrieved_contexts`」的指标；用 `.strip()` 判「有无可用文本」；非有限分数升格成失败；每条数据的错误用 `; ` 拼接，原始异常只进日志并用 `error_id` 关联。

**源码事实：** `DeepEvalEvaluator` 在「所有请求指标都不支持」时抛 `ValueError`，而 `RagasEvaluator` 同类情况只打 warning 并返回无分数的结果行；DeepEval 对「Agent 未调用任何工具」的行给 `value=None` + `status: "not_applicable"` + 人话原因，RAGAS 侧无对应分支。

**源码事实：** `_build_deepeval_metrics` 的 docstring 写明「Build fresh stateful metric instances for one dataset item」；所有指标构造都传 `threshold=None`（不做通过/不通过判定）、`async_mode=False`（禁用框架内部并发）、`verbose_mode=False`；`Answer Correctness` 用 `GEval` + `_ANSWER_CORRECTNESS_STEPS` 自然语言步骤，并传第三方库的私有参数 `_include_g_eval_suffix=False` 以避免指标名被加后缀。

**源码事实：** `_evaluate`（DeepEval）与 `_score_ragas_items`（RAGAS）的 docstring 都写成前置/后置条件，其中 DeepEval 那条明确「raw provider exceptions remain in server logs」。

**源码事实：** `_format_input` 按类别取 `prompt` / `query` / `task`，兜底取 `inputs` 的第一个值；缺字段时给空串（不报错）。

**架构推论：** 「把多种内部记录拍平成一种外部模型」是追踪层的本质工作，而它的质量判据是**「信息不足时拒绝发送」而不是「尽量发送」**。`_auxiliary` 的两条 `None` 条件正是这个判据的实例：没有稳定 id 就无法去重、没有开始时间就会造出错误时间线。主核里做「内部事件导出到外部监控」时同理——**一条错误的观测数据比一条缺失的更难排查，因为它会把人的注意力引到不存在的现象上。**

**架构推论：** 「同一个概念在不同入口有不同名字」（`prompt` / `query` / `task`）必须有一个集中映射，否则每加一种入口就要在所有消费点补一个分支。`_format_input` 是这个映射的集中点，但它的兜底分支（取第一个值）把这个集中化的收益打了折扣——**兜底越宽松，越难发现「新入口没登记」**。

**架构推论：** 「不该评分」与「得了 0 分」必须在数据形状上分开。DeepEval 侧的 `value=None` + `status: "not_applicable"` 是对的；RAGAS 侧缺这个分支意味着同类样本会被真的打分。主核里做「某些指标不适用于某些对象」的统计时同理：**分母的选择比分子的计算更容易出错，而它通常以「缺失被当成零」的形式出错。**

**架构推论：** 「每行重建有状态对象」这类约束只能靠注释与调用位置表达。DeepEval 的 metric 持有上次的 `score`/`reason`，若跨行复用就会串数据——而这类错误的表现是「某些行的分数看起来像上一行的」，极难定位。**当第三方对象有隐藏状态时，正确做法是把「必须新建」写进构造函数的 docstring 并把构造点收敛到一个函数**（这里 `_build_deepeval_metrics` 做到了）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/services/tracing/canonical.py` | `CanonicalTraceBuilder` | 内部追踪记录 → 规范 trace/span 树 |
| `api/services/tracing/canonical.py` | `CanonicalTraceBuilder.build` | 按记录类型分派；`None` 表示无需上报 |
| `api/services/tracing/canonical.py` | `CanonicalTraceBuilder._workflow` | 工作流树构造（chatflow 挂消息下，独立运行挂 workflow） |
| `api/services/tracing/canonical.py` | `CanonicalTraceBuilder._node_span` | 节点 span（LLM 专属字段只在该 kind 填充） |
| `api/services/tracing/canonical.py` | `CanonicalTraceBuilder._parent_first` | 按父先序拓扑排序（失败即「不是树」） |
| `api/services/tracing/canonical.py` | `CanonicalTraceBuilder._auxiliary` | 单 span 记录（缺 id 或缺开始时间时返回 `None`） |
| `api/services/tracing/canonical.py` | `_status` / `_cost` / `_usage` / `_seconds` | 数值与状态的收敛（过滤非有限、负数、布尔） |
| `api/services/tracing/canonical.py` | `_retry_metadata` / `_session_id` / `_user_id` | 重试摘要、会话 id 回退链、用户标识优先级 |
| `api/services/tracing/canonical.py` | `_NODE_KIND` | 节点类型 → span kind（只覆盖四种） |
| `api/core/evaluation/evaluation_manager.py` | `EvaluationManager` | 评测框架工厂（默认 RAGAS，`NONE` 返回 `None`） |
| `api/core/evaluation/evaluation_manager.py` | `EvaluationFrameworkConfigMap` | 框架枚举 → {config_class, evaluator_class} 的注册表（延迟导入） |
| `api/core/evaluation/frameworks/ragas/ragas_evaluator.py` | `RagasEvaluator` | RAGAS 适配器（四张映射表 + 指标能力声明） |
| `api/core/evaluation/frameworks/ragas/ragas_evaluator.py` | `_RAGAS_METRIC_FIELDS` | 指标 → 需要的输入字段（缺上下文判定的依据） |
| `api/core/evaluation/frameworks/ragas/ragas_evaluator.py` | `_score_ragas_metric` | 单指标评分（三类异常重试、每次 180 秒上限） |
| `api/core/evaluation/frameworks/ragas/ragas_evaluator.py` | `_score_ragas_items` | 全笛卡尔积并发评分 + 按原序装配 + 部分失败隔离 |
| `api/core/evaluation/frameworks/ragas/ragas_evaluator.py` | `_build_ragas_metrics` | 延迟导入 RAGAS 并按需注入 llm / embeddings |
| `api/core/evaluation/frameworks/ragas/ragas_evaluator.py` | `_format_input` | 按类别取输入字符串（四种键名 + 兜底） |
| `api/core/evaluation/frameworks/deepeval/deepeval_evaluator.py` | `DeepEvalEvaluator` | DeepEval 适配器（逐行串行、前置/后置条件式 docstring） |
| `api/core/evaluation/frameworks/deepeval/deepeval_evaluator.py` | `DeepEvalEvaluator._evaluate` | 逐条评测；Agent 无工具调用记 `not_applicable` |
| `api/core/evaluation/frameworks/deepeval/deepeval_evaluator.py` | `_build_deepeval_metrics` | 每条数据重建有状态指标（`threshold=None`、`async_mode=False`） |
| `api/core/evaluation/frameworks/deepeval/deepeval_evaluator.py` | `_build_test_case` | 构造框架的 test case（Agent 类别要求 `AgentEvaluationItem`） |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [RAGAS 文档：可用指标](https://docs.ragas.io/en/stable/concepts/metrics/)
- [DeepEval 文档：指标与 GEval](https://docs.confident-ai.com/docs/metrics-introduction)
- [OpenTelemetry 规范：Traces 与 Span](https://opentelemetry.io/docs/concepts/signals/traces/)

## 与主核 / AI 的关系

追踪与评测在主核（多模态数据库内核）里分别对应**运行时观测**与**质量度量**，两者的共同点是「本仓的语义要投影到外部词汇」。四条经验。

**第一，可观测性数据的取舍是「宁缺勿错」。** `_auxiliary` 在缺少稳定 id 或开始时间时返回 `None`，而不是补一个当前时间凑一条。主核里导出指标/事件到外部监控时同理：**一条时间线错误的 span 会让人去排查一个不存在的慢查询**，而缺失的那条只是少一个点。这个判据应当写进导出层的文档，因为「尽量多报」的直觉会诱使人补默认值。

**第二，「不该评分」必须与「得了 0 分」在数据形状上分开。** DeepEval 侧用 `value=None` + `status: "not_applicable"` 表达「Agent 没调工具所以不评」，而 RAGAS 侧没有对应分支。主核里做性能/质量统计时同理：**分母的选择比分子的计算更容易出错**——把「不适用」算进分母会系统性拉低指标，而且看起来完全正常。

**第三，重试策略要按异常类型白名单，而不是排除法。** 这里只重试三类 provider 异常，并明确注释「客户端校验错误必须立即上抛」。主核里做「页读取失败重试」「外部调用重试」时同理：**对不可重试的错误重试，会把一个立即失败变成三十秒后失败**（这里 10 次 × 180 秒的上界意味着最坏半小时），而错误信息在重试中被淹没。

**第四，「有隐藏状态的第三方对象必须逐次重建」这条约束要写进构造函数文档并收在单一位置。** `_build_deepeval_metrics` 的 docstring 三个词（fresh / stateful / one item）把这条说清了。主核里若使用带内部状态的第三方组件（解析器、压缩器、连接池），同样要把「能不能复用」写成明确结论——**这类错误的表现通常是「偶发地串了上一次的数据」，定位成本极高。**
