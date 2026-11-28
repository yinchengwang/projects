---
project: ragas
feature: rubrics
title: 评分规则类指标
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/metrics/collections/domain_specific_rubrics/metric.py
    symbols: [DomainSpecificRubrics]
  - path: src/ragas/metrics/collections/instance_specific_rubrics/metric.py
    symbols: [InstanceSpecificRubrics]
  - path: src/ragas/metrics/_simple_criteria.py
    symbols: [SimpleCriteriaScore, SingleTurnSimpleCriteriaPrompt]
---

# 评分规则类指标

## 一句话职责

三类评分规则按「判据放在哪一层」区分：领域级固定规则一次配好全数据集共用、样本级规则每条自带、一句判据则由调用方在构造指标时给出——三者最终都是「给模型一段判据，让它打一个分」。

## 机制是什么

前面所有 B 组指标都在算「比例」或「二值」，因为它们都想把主观判断压成客观量。评分规则类指标走的是相反方向：**承认判断是主观的，但要求判断依据事先固定下来**。

三档的区别可以压成一张表：

| 形态 | 判据来源 | 判据的作用范围 | 分数尺度 |
|---|---|---|---|
| 领域固定（`DomainSpecificRubrics`） | 构造时给，或使用内置五档模板 | 整个指标实例，所有样本共用 | 1–5 |
| 每样本自带（`InstanceSpecificRubrics`） | 每次 `ascore` 的 `rubrics` 参数 | 单条样本 | 1–5 |
| 一句判据（`SimpleCriteriaScore`） | 构造时给 `definition` 字符串 | 整个指标实例 | 整数（默认二值） |

三者共用一个结构：判据变成提示文本的一部分，模型按判据给分并附理由，分数作为结果值、理由作为 `reason`。

选择哪一档，取决于**判据能不能被统一**。整份数据集评的是同一种能力（比如「客服回复是否专业」），用领域固定档；不同题目需要不同标准（比如「给十岁小孩解释」与「给专家解释」），用样本级档；判据短到一句话且不需要分五档，用一句判据档。

## 怎么实现的

**领域固定档：判据在构造期烧进提示。** `__init__` 先决定 `self.rubrics`：调用方给了就用给的，没给则按 `with_reference` 选内置模板——**两套模板不同**，无参考版评「是否回答了用户输入」，带参考版评「是否与参考一致」。这个「有没有参考」的开关不改变提示的其他部分，只换模板措辞。

接着是最关键的一步：把判据格式化成一串「键: 说明」的行，**直接追加到共享提示实例的 `instruction` 字段上**：

```
self.scoring_prompt.instruction = f"{self.scoring_prompt.instruction}\n\nScoring Rubrics:\n{rubrics_text}\n"
```

这样判据不进 `RubricScoreInput`，而是进了系统指令。代价是这个提示对象被就地改写——若两个指标实例共享同一个提示类，第二个实例会覆盖第一个的 instruction（实际不会发生，因为每个实例都 `RubricScorePrompt()` 新建）。

分数上下界通过 `super().__init__(allowed_values=(1.0, 5.0))` 注册给基类的数值校验器。注意只声明了范围，**不校验模型实际返回的是不是 1 到 5 之间的整数**——`RubricScoreOutput.score` 的类型是裸 `int`，模型回 7 也会被当成 7。

带参考/无参考两个便捷子类 `RubricsScoreWithReference` / `RubricsScoreWithoutReference` 只是把开关固化、改了默认指标名，行为完全继承。

**样本自带档：判据进输入模型。** `InstanceSpecificRubrics` 的构造期非常薄，只存 llm 与提示。判据作为 `ascore` 的**第一个必填参数** `rubrics` 传入，随后塞进 `InstanceRubricScoreInput`——注意这个输入模型的 `rubrics` 字段是 `Field(...)`（必填），而其他五个字段都是可选的。

因此有一个显式的早退：`if not rubrics: raise ValueError(...)`。这条校验挡的是「传空字典」，因为空字典能通过 Pydantic 的必填约束（字段存在即算给了），但会让提示里的判据段为空。

两档的提示 instruction 措辞差异很值得对照：领域档说「基于**评分标准**给分」（标准在系统指令里），样本档说「基于**输入中传入的**评分标准给分」。同一件事在两个提示里被明确区分了归属，避免模型混淆判据来源。

**一句判据档：判据替换整段 instruction。** `SimpleCriteriaScore` 是旧式框架（继承 `MetricWithLLM` + `SingleTurnMetric` + `MultiTurnMetric`），与前两档不在同一个抽象层。它的构造参数是 `name` 与 `definition`，后者是**一句话判据**。

判据处理方式与前两档又不同：它不是追加到提示的 instruction 后，而是**把 instruction 整体替换**成固定句式：

```
Evaluate the input based on the criteria defined.\nCriteria Definition: {definition}
```

`definition` 还做成了可写属性，setter 会把新判据重新写进单轮与多轮两个提示的 instruction——改判据不需要重建指标。

**两种输入形态。** 单轮侧把样本 dump 成 dict 后取四个字段构 `SingleTurnSimpleCriteriaInput`；多轮侧则用样本自己的 `pretty_repr()` 把整段对话压成字符串，构 `MultiTurnSimpleCriteriaInput`（只有 `user_input` 与 `reference` 两个字段）。所以这个指标同时支持「问答对」与「整段轨迹」两种输入。

**严格度与多数投票。** `SimpleCriteriaScore` 有 `strictness` 参数，语义是「自一致性检查的次数」。构造时强制转成奇数（偶数就加一），注释写明是为了避免多数投票打平。但实际计分函数 `_compute_score` 拿到的是**一个元素的列表**：单轮与多轮路径都只调一次提示、只包一层列表。也就是说 `strictness > 1` 时那条 `Counter(...).most_common(1)` 的分支在当前代码里永远走不到——多数投票的机制保留了，但采样次数没有跟上。

## 关键取舍

**源码事实：** 判据的挂载位置在三档里各不相同：领域档烧进提示的 instruction（构造期一次），样本档进输入模型（每次调用），一句判据档替换 instruction（构造期，且可重设）。这不是风格差异，而是判据作用域差异的直接后果。

**源码事实：** 内置领域模板有**两套**，按是否有参考切换，而不是用一套模板加一段条件说明。这让无参考评估不必提到「参考」，避免模型在没有参考时去寻找不存在的对照物。

**源码事实：** 三档都只声明分数范围（`allowed_values`）而不校验取值。模型返回越界分数会被原样记录为结果值，聚合时靠 `safe_nanmean` 之类的外层处理。

**源码事实：** `SimpleCriteriaScore` 的 `strictness` 强制奇数，但计分处只处理单次结果。多轮投票逻辑存在但未被采样次数驱动，属于留下未接线的能力。

**架构推论：** 评分规则类指标是「把人类评分表原样搬进评测」的最短路径。它的价值不在客观性——判据本身仍是人写的、模型仍可能不理解——而在**可复用与可对比**：同一套判据跑两版系统，分数差异至少是在同一把尺子上量的。这也是它与 B 组其他指标的根本区别：其他指标试图消掉主观性，这一类试图**固定**主观性。

**架构推论：** 领域固定档让整份数据集共用一把尺子，这在「评同一种能力」时是对的，但在「数据里混杂多种任务」时会失真。样本级档正是为后者准备的，代价是判据要随数据一起维护——判据不再是指标配置，而是数据集的一列。这也解释了为什么它在输入模型里是必填字段：没有判据这条样本就无法评。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/metrics/collections/domain_specific_rubrics/metric.py` | `DomainSpecificRubrics` | 领域固定档：构造期把判据烧进提示指令 |
| `src/ragas/metrics/collections/domain_specific_rubrics/util.py` | `DEFAULT_REFERENCE_FREE_RUBRICS` | 无参考的五档默认模板 |
| `src/ragas/metrics/collections/domain_specific_rubrics/util.py` | `DEFAULT_WITH_REFERENCE_RUBRICS` | 带参考的五档默认模板 |
| `src/ragas/metrics/collections/domain_specific_rubrics/util.py` | `format_rubrics` | 判据字典扁平成「键: 说明」行 |
| `src/ragas/metrics/collections/instance_specific_rubrics/metric.py` | `InstanceSpecificRubrics` | 样本自带档：判据作为每次调用的必填参数 |
| `src/ragas/metrics/collections/instance_specific_rubrics/util.py` | `InstanceRubricScoreInput` | 判据进输入模型，其余字段全可选 |
| `src/ragas/metrics/_simple_criteria.py` | `SimpleCriteriaScore` | 一句判据档，同时支持单轮与多轮 |
| `src/ragas/metrics/_simple_criteria.py` | `SingleTurnSimpleCriteriaPrompt` | 单轮提示骨架，instruction 由判据整体替换 |
| `src/ragas/metrics/_simple_criteria.py` | `_compute_score` | 多数投票分支，当前只接收单次结果 |

## 参考

- [ragas 源码：domain_specific_rubrics/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/domain_specific_rubrics/metric.py)
- [ragas 源码：instance_specific_rubrics/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/instance_specific_rubrics/metric.py)
- [ragas 文档：基于评分的指标](https://docs.ragas.io/en/stable/concepts/metrics/available_metrics/general_purpose/)
- [G-Eval 论文：用评分表让模型打分](https://arxiv.org/abs/2303.16634)

## 与主核 / AI 的关系

「把判断依据从代码里挪到配置里」这件事，在主核里对应的是把硬编码的阈值搬进参数表。差别在于主核的参数是数值、可比较；这里的判据是自然语言、只能被模型解释。于是产生了一个主核不会遇到的问题：**判据本身也可能被误解**。

**判据的作用域是一个架构选择。** 领域固定等价于「全局配置」，样本自带等价于「行级配置」。主核里既有表级默认值也有列级覆盖，逻辑完全同构：越靠近数据的配置越灵活、越难维护。ragas 把这三档做成了三个类而不是一个类的三个开关，这是有意的——它们连输入契约都不一样（判据在构造期、在调用期、还是在构造期但可重设）。

**分数范围声明与取值校验必须分开。** 三档都只注册了 1–5 的允许范围却没有校验。主核里约束是入库前校验、指标是出库后统计，两者职责清楚；这里把两件事混在一起「声明了但没执行」，实际是把校验责任推给了聚合层。这类「声明了约束但没人执行」的状态在评测库里很常见，因为校验会把模型的偶发越界变成硬失败，而评测通常希望「尽量把分算出来」。

对 agent 工程最实用的三条：其一，**判据能不能统一，决定该用哪一档**——能统一就用领域档（一次配置、全量可比），不能统一就必须走样本级，否则分数没有可比性。其二，**样本级档的判据要随数据一起版本管理**，它已经是数据的一部分，不在指标里。其三，**不要指望评分规则类指标给出可定位的失败点**：它只给一个分和一段理由，不像忠实度那样能指出「第几条陈述无据」。
