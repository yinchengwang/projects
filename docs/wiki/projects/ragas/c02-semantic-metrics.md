---
project: ragas
feature: semantic-metrics
title: 语义相似与结构化比对
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/metrics/collections/_semantic_similarity.py
    symbols: [SemanticSimilarity]
  - path: src/ragas/metrics/collections/datacompy_score/metric.py
    symbols: [DataCompyScore]
  - path: src/ragas/metrics/collections/sql_semantic_equivalence/metric.py
    symbols: [SQLSemanticEquivalence]
---

# 语义相似与结构化比对

## 一句话职责

三种「不靠自然语言生成」的判定：嵌入向量的余弦相似度、CSV 表格的行/列比对、以及 SQL 查询的语义等价判断——前两者各自在自己的表示空间里算距离，第三者虽然用模型，但只让它解释两条查询再表态是否等价。

## 机制是什么

这三个指标的共同点是**绕开了「让模型自由评判一段自然语言」**，各自换了一种表示：

| 指标 | 表示 | 判据 | 输出 |
|---|---|---|---|
| `SemanticSimilarity` | 嵌入向量 | 余弦相似度（可选阈值二值化） | 连续 / 二值 |
| `DataCompyScore` | 关系表格 | 行或列级精确匹配，可组精确率/召回率/F1 | 连续 |
| `SQLSemanticEquivalence` | 查询意图 | 让模型分别解释两条 SQL 再判是否等价 | 二值 |

三者都只关心「reference 与 response 是否一致」这一个问题，但一致的**定义**完全不同：嵌入版的一致是「意思相近」，表格版的一致是「单元格值相同」，SQL 版的一致是「在同一数据库上执行会返回相同结果」。

选择哪一种取决于输出形态。响应是自由文本就用嵌入；响应是结构化表格就用表格比对；响应是 SQL 就用语义等价。用嵌入相似度评 SQL 是错的——两条完全不同的查询可以字面高度重合。

## 怎么实现的

**嵌入相似度：一步余弦，两个细节。** `SemanticSimilarity` 持有 `embeddings` 字段（有该字段就会触发基类的嵌入校验，只接受新式 `BaseRagasEmbedding`）。`ascore` 的流程是：

1. `reference = reference or " "` / `response = response or " "`——**空串替换成单个空格**，因为多数嵌入接口拒绝空串输入。
2. 两次 `embed_text` 得到两个向量，各自 L2 归一化（`np.linalg.norm(..., keepdims=True)` 后除）。
3. 矩阵乘 `v1 @ v2.T` 得余弦相似度，`flatten()` 后取标量。
4. 若设了 `threshold`，把连续值变成 `score >= threshold` 的布尔。
5. `float(score.item())` 返回。

归一化用 `keepdims=True` 是必要的：不保留维度的话广播会对整条向量求范数再逐元素除，结果错得离谱。这一步写在代码里但没写注释，属于「靠早期测试兜住」的实现细节。

`threshold` 的默认值是 `None`，即连续输出。设了阈值后分数变成 0/1，但**布尔值怎么转成 float** 依赖下游 `float(True) == 1.0`；这里直接 `float(score.item())`，`numpy.bool_` 也能转。所以二值模式下返回的仍是浮点 0.0 / 1.0，与其他二值指标一致。

**表格比对：两种模式，各自算精确率与召回率。** `DataCompyScore` 构造期做三件重活：动态导入 pandas 与 datacompy（**做了双路径兼容**——先试 `datacompy.core.Compare`（0.14 以上），失败再试 `datacompy.Compare`），把两个类存成 `self._pd` / `self._Compare`；然后校验 `mode` 与 `metric` 的取值。

`ascore` 里先 `pd.read_csv(StringIO(...))` 把两段 CSV 字符串读成 DataFrame。**解析异常不抛**，而是返回 `value=float(nan)` 加一条 reason——这是本组唯一返回 NaN 的地方，语义是「无法比较」。

然后构造 `Compare(reference_df, response_df, on_index=True)`。「以索引为准」这个选项决定了匹配方式：行级匹配靠索引对齐而不是内容对齐。

`rows` 模式下 `count_matching_rows()` 给出完全相等的行数，然后分别除以参考行数（召回率）与响应行数（精确率）。`columns` 模式下遍历 `column_stats`，把 `unequal_cnt == 0` 的列数作分子，同样除以两侧列数。

三种 `metric` 取值分别取精确率、召回率或调和均值；**调和均值前先判 `precision + recall == 0`**，为零则直接给 0.0 而不是除零。这个边界处理是必要的——两侧都空时两个分母都是 0，两个比率都是 0.0。

**SQL 语义等价：让模型先解释再表态。** `SQLSemanticEquivalence` 的输出模型有三个字段：`response_explanation`、`reference_explanation`、`equivalent`。提示指令明确要求「先解释每条查询做什么，再判断是否语义等价」，并列举了四种应视为等价的情形（`1` 与 `true`、SELECT 列顺序、别名差异、空白格式）。

「先解释再判断」不是装饰：它把判定拆成两个可检查的子任务，等价于让模型在给出结论前必须先落地自己的理解。这与 B 组「分解 → 逐项判定 → 聚合」的思路同源，只是这里只分两级而不是多级。

`database_schema` 是可选输入：`reference_contexts` 给了就换行拼成一段 schema 文本，没给就是空串。**schema 为空时等价判断会退化**——模型只能凭常识猜列语义，`COUNT` 与 `SUM` 这类差异还能看出，而「两列同名但含义不同」完全无从判断。

分数是 `1.0 if result.equivalent else 0.0`，reason 把两段解释拼起来。所以这个指标的 reason 是**全组最有信息量的**：它同时给出「我认为 A 做了什么」与「我认为 B 做了什么」，判错时能直接看出模型误解在哪一侧。

## 关键取舍

**源码事实：** 嵌入版把空串替换成单个空格而不是报错。这是为了适配拒绝空输入的嵌入接口，代价是「空」与「只有一个空格」在语义上被抹平。

**源码事实：** 表格版的 CSV 解析失败返回 NaN，而不是 0。这与 b01 里「空陈述返回 NaN」的取向一致：解析失败是「无法比较」，不是「比较结果是零差异」。

**源码事实：** 表格版的精确率/召回率方向被显式绑定：召回率除参考规模、精确率除响应规模。所以召回率衡量「参考有多少被覆盖」，精确率衡量「响应里有多少是参考中的」。

**源码事实：** SQL 版把两段解释都放进 reason。这让它成为本组唯一「判错也能诊断」的指标；嵌入版与表格版在中等分数上都只说「差不多」或给出两个比率，无法定位具体差异。

**架构推论：** 三者的「一致」定义不同，所以它们**不可互相替代也不可混合平均**。把嵌入相似度与 SQL 等价分数平均，得到的是一个既不是语言相似也不是执行等价的数字，没有任何含义。一份评测里若针对同一响应同时用多个指标，必须按「这个响应是什么形态」选一个，而不是全上。

**架构推论：** 表格版依赖 `on_index=True`，意味着**行顺序与索引必须对齐**才能正确匹配。若响应把行重排，即使数据完全一致，`count_matching_rows` 也会大幅下降。这不是缺陷而是定义：以索引为准就是要求「同一位置的值相同」。要求「集合相同」的场景应当先把两侧排序再比。

**架构推论：** SQL 版虽然用模型，但没有走「结构化分解 → 逐项判定」，只走「两段解释 + 一个判定」。这是有意的成本控制：SQL 等价的判定维度（返回集、过滤条件、聚合方式、空值处理）太多，逐项拆会带来爆炸的调用次数，而整体判定在这个任务上已经足够可靠——因为有数据库 schema 作为强约束。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/metrics/collections/_semantic_similarity.py` | `SemanticSimilarity` | 余弦相似度，含空串替换与可选阈值 |
| `src/ragas/metrics/collections/_semantic_similarity.py` | `ascore` | 归一化 → 矩阵乘 → 可选二值化 |
| `src/ragas/metrics/collections/datacompy_score/metric.py` | `DataCompyScore` | CSV 行/列级比对，三种聚合方式 |
| `src/ragas/metrics/collections/datacompy_score/metric.py` | `__init__` | 动态导入 pandas 与 datacompy，双路径兼容 |
| `src/ragas/metrics/collections/sql_semantic_equivalence/metric.py` | `SQLSemanticEquivalence` | 先解释再判定的二值等价 |
| `src/ragas/metrics/collections/sql_semantic_equivalence/util.py` | `SQLEquivalenceOutput` | 两段解释加布尔判定 |
| `src/ragas/metrics/collections/sql_semantic_equivalence/util.py` | `SQLEquivalencePrompt` | 列举四类应判等价的情形 |
| `src/ragas/embeddings/base.py` | `BaseRagasEmbedding` | 嵌入版依赖的新式嵌入契约 |

## 参考

- [ragas 源码：collections/_semantic_similarity.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/_semantic_similarity.py)
- [ragas 源码：datacompy_score/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/datacompy_score/metric.py)
- [ragas 源码：sql_semantic_equivalence/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/sql_semantic_equivalence/metric.py)
- [SAS 论文：用嵌入相似度评答案相似](https://arxiv.org/abs/2108.06130)

## 与主核 / AI 的关系

**「换一种表示」是绕开模型判断的通用手法。** 文字自由评述不可靠，于是换成向量、换成表格、换成 SQL 意图——这与主核里把「复杂的谓词表达式」换写成「可索引的访问路径」是同一个动作：**为问题找一个计算代价更低、结果更确定的表示**。关键在于新表示要保留判定所需的信息，丢掉不相关的部分。

**精确率与召回率的方向绑定，与主核选择率估算的分歧同构。** 表格版的召回率除参考规模、精确率除响应规模，两边的分母是两个不同的实体。任何信息检索或数据库场景都要显式回答「分母是谁」——分母搞错时，所有指标都变成另一个意思，且不会报错。

**NaN 与 0 的再次出现。** CSV 解析失败给 NaN 而不是 0，理由与忠实度处理空陈述时一致：**「无法比较」不能伪装成「比较结果为最差」**。这条原则在 b01 已经写过，这里换了个场景再次出现，说明它不是某一个指标的偶然选择，而是贯穿全库的约定。

对 AI 工程最实用的四条：其一，**按响应形态选指标**，自由文本用嵌入、结构化输出用表格比对、查询用语义等价，混用会得到无意义数字。其二，**表格比对要确认行序**，`on_index=True` 下重排数据会被判为不匹配。其三，**SQL 等价必须给 schema**，否则列语义无从判断，等价结论失去依据。其四，**嵌入相似度需要阈值才能当门禁**，连续相似度适合观察趋势，要判「通过/不通过」必须自己定阈值并把指标调到二值模式。
