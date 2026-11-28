---
project: ragas
feature: answer-relevance
title: 答案相关性与正确性
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/metrics/collections/answer_relevancy/metric.py
    symbols: [AnswerRelevancy]
  - path: src/ragas/metrics/collections/answer_correctness/metric.py
    symbols: [AnswerCorrectness]
  - path: src/ragas/metrics/_answer_relevance.py
    symbols: [ResponseRelevancePrompt, ResponseRelevancy]
---

# 答案相关性与正确性

## 一句话职责

两条衡量「答案好不好」的指标：**相关性**只问「这段回答是不是在答那个问题」，做法是让模型从回答反向生成问题、再与原问题比相似度；**正确性**问「回答对不对」，做法是把回答与参考答案都拆成陈述、按 TP/FP/FN 算 F-beta，再与语义相似度加权合并。

## 机制是什么

这两条指标回答的是不同的问题，所以拆法完全不同：

| 指标 | 核心问题 | 中间产物 | 聚合方式 |
|---|---|---|---|
| `AnswerRelevancy` | 回答切题吗 | 若干个「能引出这段回答的问题」+ 每份是否敷衍 | 余弦相似度均值 × 非敷衍标志 |
| `AnswerCorrectness` | 回答正确吗 | 回答与参考答案的陈述列表 + TP/FP/FN 分类 | F-beta 与相似度加权平均 |

**反向提问是相关性指标的关键手法。** 直接问模型「这段回答切题吗」会得到主观且不稳定的判断。改成「假设你看到的只有这段回答，你会问什么问题」——模型产出的问题就代表了「这段回答实际在回答什么」。再把这个生成问题与用户的真实问题比相似度，等于在度量「回答的实际主题」与「用户想知道的主题」是否一致。这个反向操作的另一个好处是：它是**无须参考答案**的，因此能用在没有标准答案的场景。

`AnswerRelevancy` 还顺带产出一个 `noncommittal` 标记：回答若属于「我不知道」「要看情况」这类敷衍内容，模型把它标为 1。

`AnswerCorrectness` 走的是有参考答案的路线，与忠实度共用「陈述分解 + NLI 判定」的骨架，但判定对象不同：忠实度判「陈述能否被上下文支持」，正确性判「回答的陈述属于 TP、FP 还是 FN」。

## 怎么实现的

**新式相关性的流程（`collections/answer_relevancy/metric.py`）。** `ascore(user_input, response)` 六步：

1. 入参校验：`user_input` 与 `response` 都非空，否则 `ValueError`。
2. 循环 `strictness` 次（默认 3），每次用 `AnswerRelevancePrompt` 渲染 `AnswerRelevanceInput(response=...)`，调 `llm.agenerate(prompt_str, AnswerRelevanceOutput)` 得到 `{question, noncommittal}`。这一步是**独立重复调用**而不是一次要 3 个——因为提示词只要求生成「一个问题」。
3. 只有 `result.question` 非空的结果才被收下（空问题被静默丢弃），同时收集对应的 `noncommittal` 标志。
4. 若一个有效问题都没生成，直接返回 `MetricResult(value=0.0)`——**这里是 0 而不是 NaN**，与忠实度那一页遇到的空值处理取向相反。
5. 嵌入：原问题用 `aembed_text`（返回单条向量后 `reshape(1, -1)`），生成的问题用 `aembed_texts`（`reshape(n, -1)`），各自转成 numpy 数组。
6. 余弦相似度：`np.dot(gen_vec, q_vec.T).reshape(-1,) / (‖gen‖ * ‖q‖)`，再 `mean()`；最后乘 `int(not all_noncommittal)`——**只有当所有生成结果都被标为敷衍时整分为 0**，只要有一份是实质性的就保留相似度均值。

最后一步是二值开关式的惩罚，不是比例惩罚：3 份里有 2 份敷衍，分数完全不受影响。

**旧式相关性实现（`metrics/_answer_relevance.py`）的对照价值。** `ResponseRelevancy` 是 `MetricWithLLM + MetricWithEmbeddings + SingleTurnMetric` 的组合，即「单轮 + 需要 LLM + 需要嵌入」三个维度都占。类上声明了 `_required_columns = {SINGLE_TURN: {"user_input", "response"}}` 与 `output_type = CONTINUOUS`。

它的提示词类 `ResponseRelevancePrompt` 是 `PydanticPrompt` 子类，带 `instruction` 与**两条示例**（爱因斯坦出生地 → 问「出生在哪」、不知道 2023 手机特性 → 问那个特性）。新式的 `AnswerRelevancePrompt` 结构相同但给了三条示例，多一条法国首都的例子。

打分路径分两步：`generate_multiple(data, llm, callbacks, n=strictness)` 一次拿 n 份输出（`n` 是 LLM 的多候选参数），然后 `_calculate_score` 里：若所有生成问题都是空串，**打 warning 并 `score = np.nan`**；否则算余弦均值并乘非敷衍标志。

于是两代在同一个边界上给了相反答案：旧式空问题 → `nan`，新式空问题 → `0.0`。这不是笔误，而是两代对「无数据」的语义理解不同；但从「答案相关性」的定义看，`0.0` 会把「模型没能生成问题」判成「完全不切题」，属于把配置问题混成质量问题的典型。

**新式正确性的流程（`collections/answer_correctness/metric.py`）。** 构造期先做四道校验，都是硬失败：

- `weights` 必须是长度为 2 的列表；
- 两个权重不能全为 0；
- 权重必须非负；
- `weights[1] > 0` 时 `embeddings` 必须提供（否则抛错并提示「要么给嵌入，要么把相似度权重设成 0」）。

此外还校验 `beta` 必须是 `float`（`int` 会被拒绝）。注意默认 `beta=1.0` 是浮点、默认 `weights=[0.75, 0.25]`，因此默认配置合法。

`weights` 用一个**可变列表**做默认值（`weights: List[float] = [0.75, 0.25]`），在 dataclass 里这是被允许的但属于可变默认值，多个实例共享同一个列表对象。

`ascore(user_input, response, reference)` 四步：

1. 分别对 `response` 与 `reference` 生成陈述（`StatementGeneratorPrompt`，输入是 `(question, answer)`）。
2. 两边都有陈述时，用 `CorrectnessClassifierPrompt` 一次性分类出 `ClassificationWithReason`（含 `TP` / `FP` / `FN` 三个列表），再由 `_compute_f1_score` 算 F-beta。**若任一边陈述为空，`factuality_score` 直接记 1.0**——注释写的是「assume perfect match」。这与「空值中性」相反，是往满分方向兜底。
3. 相似度：`weights[1] == 0` 时跳过并记 0.0（此时该项权重为 0，不影响结果）；否则把 response 与 reference 各自嵌入、算余弦；任一向量范数为 0 时返回 0.0（避免除零）。
4. `np.average([factuality, similarity], weights=self.weights)` 得最终分。

**F-beta 的边界处理值得单看。** `_compute_f1_score` 里：

- `tp + fp == 0` 时，`fn == 0` 给 precision 1.0，否则 0.0；
- `tp + fn == 0` 时，`fp == 0` 给 recall 1.0，否则 0.0；
- `precision + recall == 0` 直接返回 0.0；
- 否则 `(1+β²)·p·r / (β²·p + r)`。

也就是说「两边都没有可判陈述」被当成完美（precision 与 recall 都是 1.0），而不是 0 或 NaN。`metrics/utils.py` 里的公共 `fbeta_score` 在同样情形下给出的是 0（它把空区间时的 precision/recall 设为 0 并直接返回 0.0）。**两处同名语义的实现对空集给出相反结果**，这是选择实现细节影响可比性的实例。

**类级校验的覆写。** `AnswerCorrectness` 覆写了 `_validate_embeddings` 为一个空的 `pass`，理由写在注释里：嵌由 `__init__` 按权重决定是否必需，基类的无条件校验会误报。这是新旧校验层次冲突的典型补丁——基类的 `hasattr` 门控在此失效，只能靠覆写成空函数绕开。

## 关键取舍

**源码事实：** 相关性用「反向生成问题 + 余弦相似度」绕开主观整体判断，因此**不需要参考答案**。

**源码事实：** 敷衍惩罚是二值开关（`int(not all_noncommittal)`），只在全部候选都敷衍时才归零，不按比例扣分。

**源码事实：** 两代相关性对「生成问题为空」的处理相反：旧式 `nan`，新式 `0.0`。

**源码事实：** 正确性在「陈述为空」时把事实性判成 1.0（完美），而公共 `fbeta_score` 在同类空集上返回 0.0。

**源码事实：** 正确性的 `weights` 默认值是一个共享的可变列表；`beta` 的类型校验只接受 `float`，传 `1` 会被拒绝。

**源码事实：** `AnswerCorrectness` 覆写 `_validate_embeddings` 为空实现，以绕开基类的无条件嵌入校验。

**架构推论：** 两条指标的食物链位置不同。相关性只看「问与答的主题是否对齐」，正确性看「答与参考答案的事实是否重合」，因此相关性是**检索质量的下游信号**（回答跑题往往意味着检索没给对材料），正确性是**端到端质量信号**。它们不能被一条指标替代，也不该互相替代。

**架构推论：** 两代实现在空值上给出相反答案，说明「迁移半路」的代价不止于代码重复：**同一指标名下的历史数据不可直接比较**。跨版本做基线对比时，必须确认用的是哪一代实现，否则「分数下降」可能只是空值语义变了。

**架构推论：** 正确性把事实性与相似度加权合并，本质是承认「要点对上」与「说法相近」是两件事。默认 0.75 / 0.25 的权重偏向事实性，说明作者认为「陈述级重合」比「整体语义相近」更能反映正确性；但 `beta` 与 `weights` 两个旋钮叠加后，同一个回答的分数可以在很大区间内变动——**这类多参数指标的分数只有配上参数才有意义**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/metrics/collections/answer_relevancy/metric.py` | `AnswerRelevancy` | 新式相关性：反向提问 + 余弦 + 敷衍开关 |
| `src/ragas/metrics/collections/answer_relevancy/metric.py` | `AnswerRelevancy.ascore` | 严格度循环、空问题丢弃、二值惩罚 |
| `src/ragas/metrics/collections/answer_relevancy/util.py` | `AnswerRelevancePrompt` | 新式提示词：生成问题 + 标敷衍，三条示例 |
| `src/ragas/metrics/collections/answer_correctness/metric.py` | `AnswerCorrectness` | 新式正确性：陈述分解 + TP/FP/FN + 加权 |
| `src/ragas/metrics/collections/answer_correctness/metric.py` | `AnswerCorrectness.__init__` | 四道权重与嵌入校验 |
| `src/ragas/metrics/collections/answer_correctness/metric.py` | `_compute_f1_score` | F-beta，空精度/召回按完美兜底 |
| `src/ragas/metrics/collections/answer_correctness/metric.py` | `_calculate_similarity` | 余弦相似度，零范数返回 0 |
| `src/ragas/metrics/_answer_relevance.py` | `ResponseRelevancy` | 旧式相关性：`generate_multiple` + 空问题给 NaN |
| `src/ragas/metrics/_answer_relevance.py` | `ResponseRelevancePrompt` | 旧式提示词，两条示例 |
| `src/ragas/metrics/_answer_relevance.py` | `_calculate_score` | 空问题打 warning 并返回 NaN |
| `src/ragas/metrics/utils.py` | `fbeta_score` | 公共 F-beta，空集返回 0 |

## 参考

- [ragas 源码：collections/answer_relevancy/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/answer_relevancy/metric.py)
- [ragas 源码：collections/answer_correctness/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/answer_correctness/metric.py)
- [ragas 源码：metrics/_answer_relevance.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/_answer_relevance.py)
- [ragas 文档：答案相关性](https://docs.ragas.io/en/stable/concepts/metrics/available_metrics/answer_relevance/)

## 与主核 / AI 的关系

「反向生成再比对」与主核里「把查询改写成索引可用的形式再匹配」是同一种迂回手法：两者都承认原始形态难以直接度量，于是构造一个中间表示让比较变得确定。主核把模糊的自然语言查询变成结构化谓词，ragas 把模糊的「切题与否」变成「两个问题之间的余弦距离」。**中间表示的选择，决定了度量能有多细。**

空值语义的分歧在这里格外明显。数据库用三值逻辑统一了「未知」，所以相似的语义只有一个答案；ragas 没有统一层，于是同一语义在不同实现里各自决定——忠实度给 NaN、相关性新式给 0、正确性给 1.0。**同一个「没有东西可评」的情况，产生了三个不同的数值**。这对聚合统计是灾难：一条 NaN 会污染整个均值，一条 0 会把平均拉低，一条 1.0 会把它拉高。

对 agent 工程的三条实际建议：其一，相关性指标只能反映「答得切不切题」，它对检索错误与回答错误的区分毫无帮助，不要用它替代正确性。其二，敷衍惩罚是开关式的，`strictness` 越大越容易被判定「有实质性内容」——因为只要有一份不敷衍就保留分数，所以提高严格度实际会**放宽**惩罚。其三，正确性的分数必须连同 `weights` 与 `beta` 一起记录，否则跨实验对比毫无意义。
