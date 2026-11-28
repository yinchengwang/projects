---
project: ragas
feature: faithfulness
title: 忠实度：陈述级判据
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/metrics/collections/faithfulness/metric.py
    symbols: [Faithfulness]
  - path: src/ragas/metrics/_faithfulness.py
    symbols: [Faithfulness, StatementGeneratorPrompt, NLIStatementPrompt]
---

# 忠实度：陈述级判据

## 一句话职责

把「答案有没有编」这个模糊判断拆成三步可执行的计算：先把答案切成原子陈述、再逐条让模型判定「这条能否被给定上下文支持」、最后算被支持的比例。

## 机制是什么

这是 ragas 里最重要的一条指标，也是理解整个库设计思路的入口。

直接问模型「这个答案忠实吗」是不可用的：不同调用给出的评分不稳定、无法解释、也无法定位到具体哪句话编了。**ragas 的解法是把整体判断拆成「分解 → 逐项判定 → 聚合」**，于是：

- 评分变成一个**比例**，取值天然落在 0 到 1，有明确含义（多少比例的陈述有据可依）；
- **每一项都可定位**：哪条陈述被判为无支持，读者能直接看到问题在哪；
- 判定任务变**简单**了：模型不需要同时处理「理解答案、理解上下文、权衡整体」三件事，只需判断「这一句能否被这段文字支持」——单项判定比整体判断可靠得多。

这条套路在 ragas 里反复出现（上下文精确率、事实正确性、主题一致性都是它的变体），所以把 faithfulness 读透，其余指标大半可以猜到形状。

## 怎么实现的

`Faithfulness` 只有三个私有步骤，分别对应上面三步。**代码量很小，但每一步的边界条件都写得很细。**

**第一步 `_create_statements`**：把 `(question, answer)` 塞进 `StatementGeneratorPrompt`，让模型输出 `StatementGeneratorOutput`，取 `result.statements`。用问题一起给，是因为「哪些句子算独立陈述」依赖问题——同一个答案在有不同问题的语境下该切成的粒度不同。

**第二步 `_create_verdicts`**：把**所有上下文合并成一整段**（`"\n".join(retrieved_contexts)`），连同陈述列表交给 `NLIStatementPrompt`，输出 `NLIStatementOutput`。

这里有一个值得注意的设计：**上下文是拼接而不是逐块判定**。这样做的后果是「无法区分『哪一块上下文支持了它』」，换来的是「LLM 调用次数固定为 1 次」——如果按块判定，调用次数会变成「陈述数 × 块数」。对忠实度这个指标，知道「有没有被支持」就够了，不需要知道「被哪块支持」（那是引用对齐指标 `QuotedSpansAlignment` 的职责）。**这是指标边界划分影响实现成本的例子。**

**第三步 `_compute_score`**：`sum(1 if statement.verdict else 0) / len(statements)`。注意它用 `if statement.verdict else 0` 而不是真值累加——`verdict` 是布尔，这样写把「明确判为支持」才算数。如果 `verdict` 可能是 `None`（模型没给出判定），`None` 会被算成 0，即**默认判为不忠实**。这是保守方向：宁可把可支持的判成不支持，也不反过来。

**空输入的处理是不对称的，这一点最值得看。** 三处：

1. `ascore` 开头对三个入参**逐个显式校验**并抛 `ValueError`，且错误消息带出「请把这个字段加到测试样本里」的指引——比 `KeyError` 有用得多。
2. **陈述生成结果为空时返回 `NaN` 而不是 0**：注释写了理由「return NaN like legacy」（与旧实现保持一致）。`NaN` 与 `0` 的区别是本质的——`0` 表示「判过了，全都不忠实」，`NaN` 表示「没有东西可判」。**把「无数据」混成「差评」是评分系统最常见的错误**，这里避开了。
3. `_compute_score` 里再做一次 `if not verdicts.statements: return float("nan")` 兜底，因为 NLI 那一步也可能回空。

第 2、3 点的存在说明**空值是这条链路的常态而非异常**：模型确实会吐出空陈述列表（答案太短、或模型判定「没有可陈述内容」）。

**新式实现拒绝旧式包装器。** 类的 docstring 明确写了 "Only supports modern components - legacy wrappers are rejected with clear error messages"。类型注解是 `InstructorBaseRagasLLM`，而 `ascore` 里调的是 `self.llm.agenerate(prompt_str, OutputModel)` ——**这个签名要求模型支持「给定 pydantic 模型的结构化输出」**，旧式 `BaseRagasLLM` 没有这个能力。这就是 A 组要讲的两代分野的具体落点。

## 关键取舍

**源码事实：** `Faithfulness.ascore` 分三步：`_create_statements`（问题+答案 → 陈述列表）、`_create_verdicts`（所有上下文合并 + 陈述 → 逐条判定）、`_compute_score`（支持数 / 总数）。

**源码事实：** 上下文被 `"\n".join(retrieved_contexts)` 合并成一整段后一次性判定，而不是逐块判定——因此无法知道「被哪块支持」，换来固定的单次 NLI 调用。

**源码事实：** 陈述为空返回 `float("nan")`，注释理由「return NaN like legacy」；`_compute_score` 里再兜底一次。而三个入参缺失则抛 `ValueError` 且错误消息带修复指引。

**源码事实：** `verdict` 用 `if statement.verdict else 0` 累加，`None` 会被算成不忠实（保守方向）。

**源码事实：** 新式实现只接受 `InstructorBaseRagasLLM`，旧式包装器被明确拒绝。

**架构推论：** 「分解 → 逐项判定 → 聚合」这套三段式是**把主观判断变成可复现计算**的通用手法。它的价值不在准确度（单项判定仍由模型做，仍不确定），而在**可解释与可定位**：分数低时能指出是哪几条陈述出的问题。这也是为什么 ragas 每个指标都自带一套输出模型——**结构化的中间结果本身就是产品**。

**架构推论：** 「空值返回 NaN 而不是 0」这条一致性规则看似小事，实则是评分系统的分水岭。一旦把「没数据」算成 0，所有聚合统计（平均值、通过率）都会被污染，而且**污染方向永远偏向差评**——于是「没配置好」的问题会被误诊为「系统质量差」。这类错误在评测工具里极其常见。

**架构推论：** 上下文合并 vs 逐块判定的取舍，说明**指标的语义边界应该驱动实现形态**。「忠实度」只问「有没有据」，所以合并是对的；「引用对齐」要问「哪一块」，所以才需要单独的指标。如果一开始就把两个需求塞进一个指标，实现会变成「每块都判一遍」，成本翻几十倍而大多数调用方并不需要那个精度。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/metrics/collections/faithfulness/metric.py` | `Faithfulness` | 新式实现（三步链路） |
| `src/ragas/metrics/collections/faithfulness/metric.py` | `_create_statements` | 答案 → 原子陈述 |
| `src/ragas/metrics/collections/faithfulness/metric.py` | `_create_verdicts` | 合并上下文 + 陈述 → 逐条判定 |
| `src/ragas/metrics/collections/faithfulness/metric.py` | `_compute_score` | 支持比例与 NaN 兜底 |
| `src/ragas/metrics/collections/faithfulness/util.py` | `StatementGeneratorPrompt` | 陈述生成提示与输出模型 |
| `src/ragas/metrics/collections/faithfulness/util.py` | `NLIStatementPrompt` | 逐条判定提示与输出模型 |
| `src/ragas/metrics/_faithfulness.py` | `Faithfulness` | 旧式实现（对照用） |

## 参考

- [ragas 源码仓库](https://github.com/explodinggradients/ragas)
- [ragas 文档：忠实度指标](https://docs.ragas.io/en/stable/concepts/metrics/available_metrics/faithfulness/)
- [RAGAS 论文（Es et al., 2023）](https://arxiv.org/abs/2309.15217)
- [ragas 源码：faithfulness/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/faithfulness/metric.py)

## 与主核 / AI 的关系

忠实度指标与主核（多模态数据库内核）的交集在**「把连续质量变成可聚合的离散判定」**这个共同问题上，而且两者对空值的处理给出一致的答案。

**陈述级判定 vs 数据库的行级约束。** 数据库的 `CHECK` 约束逐行判定、任一行失败即报错；忠实度逐条陈述判定、然后算比例。差别是**失败的处理方向相反**：约束要求「全部通过」，指标要的是「通过多少」。前者是二值门禁（一行不合规整行写入被拒），后者是比例度量（一条不忠实不会让整个答案作废）。**同一个分解动作，因为用途不同而导出相反的语义**——这是理解「评测」与「约束」分野的关键。

**NaN vs NULL。** 忠实度用 `float("nan")` 表示「无数据可判」，数据库用 `NULL` 表示「未知」，两者在语义上完全对应，连踩的坑都一样：`NaN` 参与任何算术都得 `NaN`，`NULL` 参与任何比较都是「未知」。这正是想要的传播行为——**「未知」污染聚合结果是对的**，因为用未知数据算出的平均值没有意义。反过来说，如果实现成「未知识别成 0」，聚合就在说谎。数据库用三值逻辑统一处理这件事，ragas 靠每个指标自己记住，后者的风险是**新写的指标容易忘**。

**三步链路 vs 存储过程。** 「分解 → 判定 → 聚合」与数据库里「扫描 → 逐行校验 → 聚合计数」的执行形态同构，都是**把一次难以直接回答的整体问题拆成可并行、可解释的单元**。差别是数据库的每一步都是确定的，而 ragas 的判定步由模型执行——所以它必须额外做「结构化输出约束 + 解析失败修复 + 空值兜底」这三件数据库不需要做的事。**不确定性越高，管道必须越笨重。**

对 agent 工程最直接的价值有三条。其一：**「是否忠实」必须拆成陈述级判定**，整体问模型既不稳定也不可定位。其二：**空值绝不能算成 0 分**，否则所有聚合统计偏向差评，把配置问题误诊成质量问题。其三：**指标的语义边界要驱动实现形态**——只问「有没有据」就合并上下文，要问「哪一块」就单开指标，把两者塞进一个指标会让成本翻几十倍。
