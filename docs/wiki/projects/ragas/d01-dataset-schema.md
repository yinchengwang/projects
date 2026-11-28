---
project: ragas
feature: dataset-schema
title: 样本与数据集结构
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/dataset_schema.py
    symbols: [BaseSample, SingleTurnSample, MultiTurnSample]
  - path: src/ragas/dataset_schema.py
    symbols: [RagasDataset, EvaluationDataset, EvaluationResult]
---

# 样本与数据集结构

## 一句话职责

把「一条评测数据」定义成一个字段全可选的样本对象，把「一批样本」定义成一个只允许单一类型的序列，把「一次评测的产出」定义成按指标名可索引的聚合结果——这三层是全库所有指标与执行器共同的输入输出契约。

## 机制是什么

三层结构，各自解决一个问题：

**第一层，样本。** 两条互斥的形态：`SingleTurnSample` 表示一问一答，`MultiTurnSample` 表示一段消息序列。它们都继承 `BaseSample`，后者只有三个方法，核心是 `to_dict()`——用 `model_dump(exclude_none=True)` 把样本变成字典，**并且把所有取值为 `None` 的字段整体丢掉**。

这一步是整份契约的关键。样本的字段全部可选，所以「这条样本提供了什么」不是由类定义决定，而是由**哪些字段被赋了值**决定。指标要跑，就必须能在样本里找到它需要的字段；找不到就报错。于是「字段集合」成了指标与数据之间的握手协议。

**第二层，数据集。** `RagasDataset` 是抽象基类，做两件事：构造时校验**所有样本类型必须相同**（拿第一条的类型逐个比对，不一致就抛 `ValueError` 并指明第几条）；提供一整套格式转换（HF Dataset、pandas、CSV、JSONL、dict/list）。`EvaluationDataset` 是具体实现，额外带 `backend` 与 `name` 两个字段以及切片支持。

**第三层，结果。** `EvaluationResult` 是一个 dataclass，核心动作在 `__post_init__`：把「每行一个字典」的分数列表**转置**成「每个指标一个列表」，同时对非二值指标算均值。转置是必要的，因为下游要做的是「按指标看分布」，而不是「按样本看分数」。

## 怎么实现的

**单轮样本的字段清单。** 十二个字段，全部 `Optional` 且默认为 `None`，可以分成四组：

| 组 | 字段 |
|---|---|
| 问题与答案 | `user_input`、`response`、`multi_responses` |
| 检索到的东西 | `retrieved_contexts`、`retrieved_context_ids` |
| 参考标准 | `reference_contexts`、`reference_context_ids`、`reference` |
| 评测附加 | `rubrics`、`persona_name`、`query_style`、`query_length` |

后四个不参与任何指标计算，是测试集合成流程留下的痕迹（合成器会记下这条问题由哪个角色、用哪种风格、按什么长度生成），以及评分规则指标的判据载体。

这里有一处文档与代码的偏差：字段名是 `rubrics`（复数），而类文档字符串写的是 `rubric`。照文档写代码会直接抛校验错误。

**特征集合只来自第一条样本。** `RagasDataset.features()` 的实现是 `self.samples[0].get_features()`——**只取第一条样本的非 `None` 字段**，不是全体字段的并集。这与 `exclude_none=True` 组合后产生一个真实的边界条件：

若第一条样本恰好没填 `reference`（其余样本都填了），`features()` 里就不会有 `reference`，而 `validate_required_columns` 正是拿 `features()` 与指标的必需列做子集判断——于是整批数据被判定为「缺列」并报错，尽管其余样本都有。这是「用首样本代表全体」的代价，只影响单轮样本，因为多轮样本的字段几乎都有值。

反过来，若第一条填了某字段而后续样本没填，`features()` 会包含它，校验通过；但执行到后续样本时该字段是 `None`，指标里对缺失字段的显式校验会抛错。**校验通过不等于执行通过**，这两道门在数据不一致时会给出不同结论。

**多轮样本的对话校验。** `MultiTurnSample.user_input` 是唯一必填字段（`t.List[...]`，没有默认值），挂了一个校验器，除了逐条检查消息类型外，还检查工具消息的**位置合法性**，共三条规则：

1. 任何 `ToolMessage` 之前，整段对话里必须已经出现过 `AIMessage`；
2. `ToolMessage` 前面紧跟的必须是 `AIMessage` 或另一个 `ToolMessage`；
3. 若紧跟的是 `AIMessage`，那这条 AI 消息必须真的带 `tool_calls`。

三条规则串起来的效果是：工具输出只能出现在「AI 刚刚调了工具」之后，且可以连续多条（并行调用多个工具）。任何「AI 没调工具却出现工具输出」或「工具输出直接跟在人类消息后」的数据都会在构造期被拒。

**类型自动推断。** `from_list` / `from_dict` 判断多轮还是单轮的方式是同一个条件：**所有条目都含 `user_input` 且第一条的 `user_input` 是 list**。所以只要样本的第一条 `user_input` 是列表，整批按多轮解析；否则整批按单轮。这条推断是「要么全单轮、要么全多轮」的另一种表达——而 `RagasDataset` 的类型一致性校验又把这个约束重复了一遍。两处冗余是有意的：一处保证解析方向，一处保证构造后不会被混入异类。

**结果转置与均值。** `EvaluationResult.__post_init__` 先把分数转置成字典，再逐指标算 `safe_nanmean`，结果存两份：`_repr_dict`（全部指标，用于 `__repr__`）与 `values`（排除二值列，且每个值加 `1e-10`）。加 `1e-10` 的作用是避免某些下游在做「全零判定」时把合法零值当成缺失——这在 `scores` 里保留原值，只影响内部那个未被使用的 `values` 列表。

`__repr__` 的输出是一个花括号包裹的「指标名: 四位小数」列表，刻意模仿字典的观感，但它**不是字典**：`__getitem__` 接受指标名返回整列分数（列表），而不是单值。

二值列之所以被排除在均值计算之外，是因为这列分数的语义是「通过/不通过」，平均值没有意义——不过这个排除只作用于内部的 `values`，`_repr_dict` 里仍然会算出二值列的均值并打印。所以打印出来的报告里，二值指标的均值仍会显示，只是它不对应任何有意义的统计量。

**评测轨迹的解析时机。** 结果对象的 `traces` 字段在 `__post_init__` 里由 `parse_run_traces(self.ragas_traces, run_id)` 现算，而 `ragas_traces` 本身带 `repr=False`（打印时不展示）。所以「原始轨迹」与「可读轨迹」是两个字段，前者来自 tracer，后者是解析产物。

**训练期标注容器。** 同一文件里还有四个标注类，服务于「人工标注提示输出」这条线：`PromptAnnotation` 记录一次提示的输入、模型输出与人工修正；`SampleAnnotation` 记录样本级的指标输入、指标输出、各提示的标注与是否采纳；`MetricAnnotation` 以指标名为键收集；`SingleMetricAnnotation` 是单指标的样本集合，并提供 `sample` / `batch` / `stratified_batches` / `select` / `filter` 等抽样方法。

`SingleMetricAnnotation.train_test_split` 是 `raise NotImplementedError`——留了签名没实现。而 `sample` 的分层抽样在「按比例分配后总数不足 n」时会用随机补足，直到凑满或没有剩余样本。

## 关键取舍

**源码事实：** 样本字段全可选，`to_dict` 用 `exclude_none=True`。「提供了什么」由运行时值决定，不由类型声明决定。这让同一份数据集能吃不同所需字段的指标，代价是缺字段只能在执行期暴露。

**源码事实：** `features()` 只看第一条样本。校验与执行因此在数据不一致时会给出不同结论：校验可能误判缺列（首样本缺、其余有），也可能放过（首样本有、其余缺）。

**源码事实：** 多轮样本的校验器只检查「工具消息的位置」，不检查「对话是否以人类消息开头」，也不检查 AI 消息是否token合法。校验是必要非充分。

**源码事实：** `EvaluationResult` 转置后按指标名索引，返回整列。二值列被排除在内部均值计算之外，但 `__repr__` 仍打印它们的均值。

**源码事实：** `SingleMetricAnnotation.train_test_split` 抛 `NotImplementedError`；分层抽样在凑不满时会随机补足。

**架构推论：** 「样本 = 可选字段的集合」这个模型让 ragas 能同时容纳 RAG 评测、多轮对话评测、Agent 评测与结构化输出评测，而不需要为每种场景定义新类型。代价是**输入契约变成了隐式契约**：哪些字段必须存在，只写在每个指标的 `required_columns` 里，不在数据集类型上。

**架构推论：** 把「训练期标注」与「评测期样本」放在同一个文件但分成两套类型，说明这两条线共用「样本」这个容器概念但用途不同：前者要记录人机协商过程（含人工修正与采纳标记），后者要记录待评数据。它们不复用同一个类，因为标注需要的字段（`is_accepted`、`edited_output`、`target`）对评测毫无意义。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/dataset_schema.py` | `BaseSample` | 样本基类，`to_dict` 丢弃 `None` 字段 |
| `src/ragas/dataset_schema.py` | `SingleTurnSample` | 单轮样本的十二个可选字段 |
| `src/ragas/dataset_schema.py` | `MultiTurnSample` | 多轮样本，含三条工具消息位置规则 |
| `src/ragas/dataset_schema.py` | `RagasDataset` | 抽象数据集：类型一致性校验与格式转换 |
| `src/ragas/dataset_schema.py` | `RagasDataset.features` | 特征集合，仅取自第一条样本 |
| `src/ragas/dataset_schema.py` | `EvaluationDataset` | 具体数据集，带 backend/name 与切片 |
| `src/ragas/dataset_schema.py` | `EvaluationResult` | 结果聚合：转置、均值、轨迹解析 |
| `src/ragas/dataset_schema.py` | `SingleMetricAnnotation` | 训练期标注容器与抽样方法 |

## 参考

- [ragas 源码：dataset_schema.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/dataset_schema.py)
- [ragas 文档：评测数据集与样本](https://docs.ragas.io/en/stable/concepts/datasets/)
- [ragas 文档：单轮与多轮样本](https://docs.ragas.io/en/stable/concepts/test_data_generation/)

## 与主核 / AI 的关系

**「全可选字段 + 丢弃 None」等价于主核里的稀疏列。** 数据库的稀疏列只存有值的行，`exclude_none=True` 做的是同一件事——把一个宽表压成「只含实际有值字段的稀疏记录」。两者的后果也一致：读取方必须先确认字段存在，不能按 schema 假定。

**「用首样本代表全体」是主核里无统计信息的经典错误。** 主核若用第一行去推断表结构，遇到稀疏或脏数据第一行就会全错。正确做法永远是扫全量或维护统计信息。这里的 `features()` 选了最省的做法，代价是校验结论可能既误报又漏报。

**类型一致性校验与主核的行格式一致。** `RagasDataset` 拒绝混合类型的样本，与主核一张表只允许一套行格式是同一个约束。它让下游可以做「一次判断、全程适用」，代价是异构数据要分表而不是混存。

对 AI 工程最实用的四条：其一，**别在数据里混填/漏填同一字段**，首样本的特征集合会决定整批判定；全量一致最安全。其二，**多轮数据必须满足工具消息顺序**，AI 没调工具却给工具输出会被构造期拒绝。其三，**`rubrics` 是复数**，照文档写 `rubric` 会失败。其四，**`result['metric']` 拿到的是整列**，想要单值必须自己取下标，这是最容易误用的接口点。
