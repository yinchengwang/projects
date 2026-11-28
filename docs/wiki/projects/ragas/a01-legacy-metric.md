---
project: ragas
feature: legacy-metric
title: 旧式指标契约
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/metrics/base.py
    symbols: [Metric, MetricType, MetricOutputType]
  - path: src/ragas/metrics/base.py
    symbols: [MetricWithLLM, MetricWithEmbeddings]
  - path: src/ragas/metrics/base.py
    symbols: [SingleTurnMetric, MultiTurnMetric]
---

# 旧式指标契约

## 一句话职责

`Metric` 是 ragas 第一代指标体系的根：它把「这个指标需要样本里的哪几列」声明成类属性，再用两个正交的混入（需要 LLM / 需要嵌入）与两个正交的子类（单轮 / 多轮）组合出所有具体指标，最终每个指标只需实现一个私有的异步打分函数。

## 机制是什么

一个指标要能被评测框架统一调用，框架必须先知道两件事：**它要什么输入**、**它怎么被调用**。`Metric` 用两条独立的轴来回答：

| 轴 | 取值 | 承载者 |
|---|---|---|
| 输入契约 | 单轮样本 / 多轮样本 | `SingleTurnMetric` / `MultiTurnMetric` |
| 外部依赖 | 需要 LLM / 需要嵌入 / 两者都不需要 | `MetricWithLLM` / `MetricWithEmbeddings` |

这两条轴**完全正交**：`ResponseRelevancy` 是「单轮 + 需要 LLM + 需要嵌入」，`FactualCorrectness` 是「单轮 + 只需要 LLM」，多轮指标则是「多轮 + 需要 LLM」。所以继承链是 `Metric` 的多重继承组合，而不是一棵树。读代码时必须两个维度一起看，否则会误判某个指标能接受什么输入。

第三条辅助轴是**输出形态**：`MetricOutputType` 用 `BINARY` / `DISCRETE` / `CONTINUOUS` / `RANKING` 四值标注，写在具体指标类上（例如 `output_type = MetricOutputType.CONTINUOUS`）。它不参与调用，只用于下游把分数当什么类型处理。

## 怎么实现的

**列声明有三层语义。** `Metric._required_columns` 的类型是 `Dict[MetricType, Set[str]]`——不是一份扁平集合，而是「按样本类型分别声明」。因为同一个指标可能同时支持单轮与多轮，两边的必需列不同。

列名上还带两个**后缀标记**：

- `:optional`——该列可选。`required_columns` 属性在读取时会把它过滤掉（所以外部看到的是「严格必需列」），而 `get_required_columns(with_optional=True)` 会去掉后缀并保留它。两套读法的差异直接决定「样本要提供哪些列」。
- `:ignored`——该列不参与计算，两种读法下都被丢弃。

`required_columns` 的 setter 做了一次校验：对每个列名先 `column.split(":")[0]` 取基名，必须在模块级白名单 `VALID_COLUMNS`（`user_input`、`retrieved_contexts`、`reference_contexts`、`response`、`reference`、`rubric` 六项）里，否则抛 `ValueError`。也就是说，**指标不能自造输入字段名**——这条约束把「所有指标都能被同一个样本结构喂」变成了编译期的硬保证。

**名称缺省是类名转蛇形。** `__post_init__` 里 `self.name = camel_to_snake(self.__class__.__name__)`。所以 `AnswerCorrectness` 自动成为 `answer_correctness`，与旧版对外暴露的指标名一致，用户不必显式传。

**`init(run_config)` 是唯一的初始化协议。** `Metric` 声明它为 `@abstractmethod`；两个混入各自实现：

- `MetricWithLLM.init` 校验 `llm` 非空（为空直接抛 `ValueError`，错误消息带指标名，提示「请给这个指标传一个 LLM」），然后**只在它是旧式 `BaseRagasLLM` 时**调 `set_run_config`。注释明确写了「只有 BaseRagasLLM 有这个方法，InstructorBaseRagasLLM 没有」——这就是两代并存的接缝之一。
- `MetricWithEmbeddings.init` 同样校验非空，再用 `hasattr(self.embeddings, "set_run_config")` 判断而不是类型判断。同一件事在两个混入里用了两种判据，前者靠 isinstance、后者靠鸭子类型。

**单轮与多轮的调用壳几乎逐行相同。** 四个公开方法（`single_turn_score` / `single_turn_ascore` / `multi_turn_score` / `multi_turn_ascore`）各做四件事：

1. `_only_required_columns_*`：把样本裁剪成只含必需列（取 `get_required_columns(with_optional=True)` 里对应该样本类型的列，用 `model_dump(include=...)` 重建样本）。必需列为空时不裁，原样返回。
2. `new_group(self.name, inputs=sample.to_dict(), metadata={"type": ChainType.METRIC})` 建一个回调组，把指标名挂在链路上。
3. 调私有 `_single_turn_ascore` / `_multi_turn_ascore`，异常时 `rm.on_chain_error`，正常时 `rm.on_chain_end({"output": score})`。两处都先判 `not group_cm.ended`，避免重复上报。
4. 记账：`_analytics_batcher.add_evaluation(EvaluationEvent(...))`。

同步版比异步版多两步：`apply_nest_asyncio()` 与 `run(_async_wrapper)`——同步入口本质是「把异步实现跑起来」的适配层，注释也提示在 Jupyter 环境里可能抛 `ImportError`。

**多轮版的记账写错了指标类型。** `multi_turn_score` 与 `multi_turn_ascore` 里 `evaluation_type` 传的是 `MetricType.SINGLE_TURN.name`，而单轮版也是同一个值。这处不一致只影响上报统计，不影响打分，但它是「两个壳复制粘贴」留下的痕迹。

**异步版才有超时。** `single_turn_ascore(sample, callbacks, timeout)` 用 `asyncio.wait_for(..., timeout=timeout)` 包住私有实现；同步版没有超时参数。也就是说**超时只在异步路径可控**，走同步入口时只能靠上层限时。

**两个私有抽象方法名不同构。** 单轮叫 `_single_turn_ascore(sample, callbacks)`，多轮叫 `_multi_turn_ascore(sample, callbacks)`，签名一致、返回 `float`。具体指标把它们当唯一实现点，因此「一个指标只能支持一种样本类型」——要两种都支持就得写两个类，这也是为什么 `AgentGoalAccuracyWithReference` / `WithoutReference` 这类变体被拆成独立类而不是一个类里的分支。

**文件后半段还有三种「非旧式」的辅助类型。** 它们同在 `base.py` 里，但形态与旧式契约无关，这里只作定位：

- `Ensember`（拼写如此）：把同一输入的多份模型输出合并成一份。`from_discrete` 用 `Counter(...).most_common()` 做多数投票，长度不一致或缺目标属性时打 warning 并**退回第一份输入**而不是报错。
- `ModeMetric`：一个 `runtime_checkable` 的 `Protocol`，只要求有 `name` 与 `mode` 两个字段——给「按模式切换打分的指标」（如 `FactualCorrectness` 的 precision/recall/f1）留的类型标注位。
- `SimpleBaseMetric` / `SimpleLLMMetric` 与 `create_auto_response_model`：新式路线的基座（`score` / `ascore` 返回 `MetricResult`），底座的说明在 `a02-modern-metric` 与 `a03-metric-decorator` 两页。

## 关键取舍

**源码事实：** 列名只允许 `VALID_COLUMNS` 里的六个基名，且支持 `:optional` / `:ignored` 两个后缀。这条白名单是「所有指标共用同一份样本结构」的强制手段，代价是指标无法声明额外输入（要新字段就必须改白名单）。

**源码事实：** 两条正交轴用多重继承表达，而不是一个「特性矩阵」字段。好处是每个具体指标只要挑两个父类；代价是继承图复杂，且同一语义（例如「需要 LLM」）在两代实现里用了不同判据（`isinstance` vs `hasattr`）。

**源码事实：** `init(run_config)` 里对 LLM 与 embeddings 的空值校验是硬失败（`ValueError`），不是延后到打分时报错。这把「忘了传 LLM」从运行中途失败提前到准备阶段。

**源码事实：** 同步入口没有超时，异步入口有；两者都做分析上报，且多轮版把上报类型写成了单轮。

**架构推论：** `Metric` 只声明「要什么列、怎么调用」，不声明「怎么算」。算的部分被压到单个私有异步方法，因此所有旧式指标的**唯一差异点只有一处**。这让新增指标极其便宜，也让框架能对全部指标统一做回调、裁剪、超时、上报——代价是每个指标都必须自己处理「输入为空怎么办」，框架不提供统一兜底（正如忠实度那页所记的 NaN 处理散落在各指标里）。

**架构推论：** 把「输入裁剪」放在公开壳里而不是指标实现里，意味着指标可以假定「我声明的列一定都在」。这是一个**契约前置**设计：错误从「跑一半发现缺字段」变成「准备阶段就缺字段」。代价是裁剪本身要重建 pydantic 样本对象，每个样本多一次 `model_dump` + 构造。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/metrics/base.py` | `Metric` | 旧式根：列契约、名称缺省、`init` 抽象 |
| `src/ragas/metrics/base.py` | `MetricType` | 单轮 / 多轮，列声明的分区键 |
| `src/ragas/metrics/base.py` | `MetricOutputType` | 二值 / 离散 / 连续 / 排序四种输出形态 |
| `src/ragas/metrics/base.py` | `MetricWithLLM` | 需要 LLM 的混入，`init` 校验并设置运行配置 |
| `src/ragas/metrics/base.py` | `MetricWithEmbeddings` | 需要嵌入的混入，用鸭子类型判断运行配置 |
| `src/ragas/metrics/base.py` | `SingleTurnMetric` | 单轮样本的裁剪与四个调用壳 |
| `src/ragas/metrics/base.py` | `MultiTurnMetric` | 多轮样本的同构实现 |
| `src/ragas/metrics/base.py` | `Metric.required_columns` | 过滤 `:optional` 与 `:ignored` 的只读视图 |
| `src/ragas/metrics/base.py` | `Metric.get_required_columns` | 可选列是否保留的两种读法 |
| `src/ragas/metrics/base.py` | `Ensember` | 多份模型输出的多数投票合并 |
| `src/ragas/metrics/base.py` | `ModeMetric` | 按模式切换的指标协议 |

## 参考

- [ragas 源码：metrics/base.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/base.py)
- [ragas 文档：指标总览](https://docs.ragas.io/en/stable/concepts/metrics/)
- [ragas 文档：自定义指标](https://docs.ragas.io/en/stable/concepts/metrics/custom_metrics/)

## 与主核 / AI 的关系

「先声明输入形状、再由框架统一裁剪与校验」与主核里「先有表定义、再按定义分配行槽」是同一种前置契约：主核的 DDL 决定一行有多少字段、每个字段什么类型，写入路径据此拒绝不合格的行；这里的 `_required_columns` 决定一个指标能吃到哪些字段，评测路径据此裁剪样本。两者都把「形状错误」从运行中途推到准备阶段。

差别在于**约束的紧度**：主核的字段集合由用户定义，天然可扩展；这里的列名被 `VALID_COLUMNS` 钉死为六个，扩展必须改框架代码。这个选择让「所有指标都能喂同一份数据集」成立，也让 ragas 永远无法评测「需要额外输入」的指标（例如需要工具调用轨迹的指标只能走多轮样本的另一条路径）。

对 agent 工程最实际的一条：旧式指标的**成本结构是透明的**。每个指标的公开壳只做裁剪与回调，真正花钱的 LLM 调用次数完全由私有实现决定——判断一个指标贵不贵，只需数它在私有实现里调了几次 `llm.agenerate`。这也是为什么同一语义的指标会有「合并上下文一次判完」与「逐块判」两种实现，而成本差几十倍。
