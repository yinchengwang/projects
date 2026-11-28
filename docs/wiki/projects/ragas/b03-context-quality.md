---
project: ragas
feature: context-quality
title: 上下文精确率与召回率
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/metrics/collections/context_precision/metric.py
    symbols: [ContextPrecisionWithReference, ContextPrecisionWithoutReference, ContextUtilization]
  - path: src/ragas/metrics/collections/context_recall/metric.py
    symbols: [ContextRecall]
  - path: src/ragas/metrics/collections/context_relevance/metric.py
    symbols: [ContextRelevance]
---

# 上下文精确率与召回率

## 一句话职责

这三条指标把「检索出来的材料好不好」拆成三个可分别回答的问题：**精确率**问「检出来的每一块真的有用吗」，**召回率**问「参考答案里的每一条都能在检出的材料里找到依据吗」，**上下文相关性**问「这些材料跟问题本身有关系吗」——前者需要列表顺序，后两者不需要。

## 机制是什么

分野的核心是「有没有参考答案」，以及「判定对象是什么」：

| 指标 | 有无参考 | 逐项判定的是什么 | 聚合方式 |
|---|---|---|---|
| `ContextPrecisionWithReference` | 有 | 每块上下文对「能否得出参考答案」是否有用 | 平均精确率（AP） |
| `ContextPrecisionWithoutReference` | 无（用生成的回答） | 每块上下文对「能否得出这段回答」是否有用 | 同上 |
| `ContextRecall` | 有 | 参考答案的每条陈述「能否归因于」拼接后的所有上下文 | 归因比例 |
| `ContextRelevance` | 无 | 整段上下文的切题程度（0/1/2 三档） | 双裁判取平均 |

三条里只有**精确率**对「顺序」敏感：它算的是平均精确率（average precision），也就是把每块上下文当作一个排序位置，累加「在前 i 块里命中了多少」的比值。这意味着**把有用的材料排在前面会得高分**，同一批材料换个顺序分数就变。另外两条对顺序不敏感：召回率是比例，相关性是整体打分。

`ContextUtilization` 与 `ContextPrecision` 是 `ContextPrecisionWithoutReference` 与 `ContextPrecisionWithReference` 的**重命名包装**——同一个 `ascore` 实现，只是构造时把 `name` 换成旧指标名。所以这三个类名背后只有两套打分逻辑。

## 怎么实现的

**精确率的两条路只差「拿什么当答案」。** `ContextPrecisionWithReference.ascore(user_input, reference, retrieved_contexts)` 与 `ContextPrecisionWithoutReference.ascore(user_input, response, retrieved_contexts)` 的循环体**逐行相同**：对 `retrieved_contexts` 里每一块，构造 `ContextPrecisionInput(question=user_input, context=context, answer=<reference 或 response>)`，渲染提示词，调 `llm.agenerate(prompt_str, ContextPrecisionOutput)`，取 `result.verdict` 追加到 `verdicts`。

因此差别只在语义：有参考时判「这块能否支撑标准答案」，无参考时判「这块能否支撑模型自己的回答」。**后者其实测的是「回答有没有用好上下文」**，所以它在 `ContextPrecisionWithoutReference` 别名下同时被叫做 `ContextUtilization`——名字比原名更贴切。

每次调用是**逐块一次**，所以 LLM 调用次数等于上下文块数。这与忠实度把上下文拼成一段一次判完相反：精确率必须逐块，因为要区分「哪一块有用」。

**平均精确率的算法。** `_calculate_average_precision(verdicts)` 用一个手写的累加：

- `cumsum` 累计到当前位置为止的命中数；
- 只有 `v` 为真时才 `numerator += cumsum / (i + 1)`；
- 分母是 `cumsum + 1e-10`（防除零的极小量）。

这正是标准 AP 的定义：对每个命中的位置 i，取其精确率并平均。注意分母用的是 `cumsum` 而不是「verdict 总数」——所以**全部不命中时得分 0**（numerator 为 0），而一个命中都没有命中时 `cumsum` 为 0，`0 / 1e-10` 仍是 0。

`np.isnan(score)` 的判断保留着，并打一条「格式错误」的 warning——但实际上 `score` 不可能为 NaN（分子分母都是有限数），这段是**从旧实现平移过来的死代码**。两个精确率类各自复制了一份完全相同的 `_calculate_average_precision`，没有抽成公共函数。

**召回率是「归因比例」，且上下文先拼接。** `ContextRecall.ascore` 先用 `"\n".join(retrieved_contexts)` 把所有块合成一段，再 `ContextRecallInput(question, context, answer=reference)` 一次性交给模型，拿回 `ContextRecallOutput` 里的 `classifications`（每项含 `attributed`）。

然后：分类列表为空 → `MetricResult(value=np.nan)`；否则 `sum(attributions) / len(attributions)`。

注意它返回的是 **`np.nan` 而不是 `float("nan")`**——两者在 Python 里比较行为不同（`np.nan` 是 `np.float64`），而 `MetricResult` 的算术守卫只放行 `int` / `float`，`np.float64` 恰好是 `float` 的子类所以能通过。空分类返回 NaN 的取向与忠实度一致（无数据 ≠ 差评）。

拼接上下文是必然的：召回率关心「参考答案被覆盖了多少」，不需要知道「哪一块覆盖了它」。这与精确率精确相反。

**上下文相关性用「双裁判」抵抗单次判定的抖动。** `ContextRelevance.ascore(user_input, retrieved_contexts)` 先做四道边角处理：

1. `user_input` 为空 → `ValueError`（消息带「请把 user_input 加进测试样本」的指引）。
2. `retrieved_contexts` 为空 → `ValueError`（同上）。
3. 拼接后整段为空，或 `user_input` 全空白 → 返回 `MetricResult(value=0.0)`。
4. **两个退化情形各返回 0.0**：`user_input` 与 context 完全相同，或 context 是 `user_input` 的子串。源码注释称其为「edge case」——但这两种情形其实是「问题就是把这段材料原样贴过来」，判成 0 分是一个可疑的定义（它更像是「不构成问答关系」而非「不相关」）。

主流程：两个不同提示词（`ContextRelevanceJudge1Prompt` 与 `Judge2Prompt`）各打一次分，每次走 `_get_judge_rating`。该函数带重试：最多 `max_retries` 次（默认 5），每次调 `llm.agenerate` 拿 `rating`；**只有 `rating in [0, 1, 2]` 才接受**，其他值继续重试，重试耗尽返回 `float("nan")`；异常同样重试，耗尽返回 NaN。

拿到两个 0/1/2 评分后各除以 2 归一，再用 `_average_scores` 合并：两值都有效取平均，只有一个有效就用那个，都无效返回 NaN。因此**单裁判失败不会让分数变成 NaN**，这是双裁判的实际收益——不只是「降低偏差」，还有「提高可用性」。

`rating in [0, 1, 2]` 这个判断有个细节：它是**值相等**判断而非类型判断，所以 `0.0`（浮点）因为 `0.0 == 0` 也会通过，随后 `float(rating)` 得 0.0。而 `True` 因为 `True == 1` 同样通过，会被当作 1 分。

**三个类的入参校验风格不统一。** 精确率只校验「非空」，错误消息是裸的 `"user_input cannot be empty"`；召回率同样三句裸文案；相关性则带修复指引。同一批指标里三种风格，说明它们是分批实现的。

## 关键取舍

**源码事实：** 精确率逐块调用 LLM（次数等于块数），召回率把上下文拼接后一次调用。前者为了定位「哪块有用」，后者不需要定位。

**源码事实：** 精确率算的是平均精确率，因此对列表顺序敏感；召回率与相关性对顺序不敏感。

**源码事实：** `ContextPrecision` 与 `ContextUtilization` 只是重命名包装，底层分别是 `WithReference` / `WithoutReference` 的实现。所以「三个类名、两套逻辑」。

**源码事实：** `_calculate_average_precision` 里的 `np.isnan` 分支不可能触发（分子分母皆有限），是平移过来的死代码，且在两个类里各复制一份。

**源码事实：** 相关性用双裁判，且单裁判 NaN 时用另一个裁判的值——所以只有当两个裁判都失败时才得到 NaN。

**源码事实：** 相关性对「问题与材料完全相同」或「材料是问题的子串」直接返回 0.0，属于定义层面的特殊处理。

**架构推论：** 「有参考 vs 无参考」这条轴决定了指标能不能在**没有标注数据**的场景使用。精确率的两个版本恰好给出比较的可能：同一批上下文，用参考答案判 vs 用模型自己的回答判，差值反映「模型答对了但材料没支撑」或「材料够用但模型没用上」。这是排查 RAG 故障的实用手段。

**架构推论：** 逐块判定与拼接判定的成本差异是数量级的：一块上下文一次调用，若有 10 块就是 10 次；拼接后固定 1 次。所以「精确率 + 召回率」一起跑时，成本主要来自精确率。这解释了为什么工程上常见「跑召回率做粗筛、只在关键样本上跑精确率」。

**架构推论：** 双裁判的设计把「判定抖动」当成常态而不是异常——重试、校验取值域、按有效性合并，三层都在为「模型偶尔不按格式答」做准备。这类冗余在高不确定性管道里是必要的（正如忠实度页所说「不确定性越高，管道必须越笨重」）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/metrics/collections/context_precision/metric.py` | `ContextPrecisionWithReference` | 有参考的逐块精确率 |
| `src/ragas/metrics/collections/context_precision/metric.py` | `ContextPrecisionWithoutReference` | 以模型回答为答案的逐块精确率 |
| `src/ragas/metrics/collections/context_precision/metric.py` | `ContextUtilization` | `WithoutReference` 的重命名包装 |
| `src/ragas/metrics/collections/context_precision/metric.py` | `_calculate_average_precision` | 手写平均精确率，含不可能触发的 NaN 分支 |
| `src/ragas/metrics/collections/context_recall/metric.py` | `ContextRecall` | 拼接上下文 + 归因比例 |
| `src/ragas/metrics/collections/context_recall/metric.py` | `ContextRecall.ascore` | 空分类返回 NaN、否则算比例 |
| `src/ragas/metrics/collections/context_relevance/metric.py` | `ContextRelevance` | 双裁判相关性，0/1/2 三档 |
| `src/ragas/metrics/collections/context_relevance/metric.py` | `_get_judge_rating` | 取值域校验 + 重试，耗尽返回 NaN |
| `src/ragas/metrics/collections/context_relevance/metric.py` | `_average_scores` | 单边有效即采用，两边都失败才 NaN |
| `src/ragas/metrics/collections/context_precision/util.py` | `ContextPrecisionPrompt` | 判定「这块对得出答案有用吗」 |

## 参考

- [ragas 源码：collections/context_precision/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/context_precision/metric.py)
- [ragas 源码：collections/context_recall/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/context_recall/metric.py)
- [ragas 文档：上下文精确率](https://docs.ragas.io/en/stable/concepts/metrics/available_metrics/context_precision/)
- [ragas 文档：上下文召回率](https://docs.ragas.io/en/stable/concepts/metrics/available_metrics/context_recall/)

## 与主核 / AI 的关系

精确率与召回率这两个词直接借自检索系统，含义与主核里「索引扫描的候选命中率」可以对照：主核的索引返回一批候选块，再用谓词精确过滤；候选里有多少是真正符合谓词的（精确率），以及符合谓词的块有多少被索引找到了（召回率）。差别是主核的两者都是**确定可算**的——有真实的行集合可对照；ragas 的两者都靠模型判定「有用与否 / 能否归因」，所以它们是估计而非事实。

顺序敏感性也值得对照。主核的索引扫描顺序不影响最终结果（过滤是幂等的），而精确率的 AP 计算把顺序直接算进分数。这意味着**ragas 的「精确率」实质上是排序质量指标**，它同时衡量「检出的对」与「对的排在前面」——这是它比朴素精确率更有用的地方，也是容易误读的地方。

对 agent 工程的三条实际结论：其一，想把「材料够不够」与「材料用没用上」分开，就跑 `WithReference` 与 `WithoutReference` 两个版本做对比，单看一个分不出问题在检索还是在生成。其二，精确率的成本随块数线性增长，块数多时应先降 top-k 而不是提高并发。其三，相关性的 0.0 退化规则会让「问题本身就是材料」这类样本被误判为不相关，评测集里若有这类样本，分数会系统性偏低。
