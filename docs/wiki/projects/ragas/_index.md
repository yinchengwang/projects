---
project: ragas
page_type: overview
display_name: Ragas
repo: https://github.com/vibrantlabsai/ragas
license: Apache-2.0
language: Python
stars: 16398
stars_as_of: 2026-11-22
refs:
  - path: src/ragas/metrics/base.py
    symbols: [Metric, MetricWithLLM]
  - path: src/ragas/metrics/collections/base.py
    symbols: [BaseMetric]
  - path: src/ragas/metrics/collections/faithfulness/metric.py
    symbols: [Faithfulness]
  - path: src/ragas/evaluation.py
    symbols: [evaluate, aevaluate]
  - path: src/ragas/dataset_schema.py
    symbols: [SingleTurnSample, EvaluationDataset]
---
# Ragas

## 一句话职责

RAG 系统的评测库：它把「这套 RAG 好不好」这个模糊问题拆成一组可执行、可解释、可聚合的指标——忠实度、答案相关性、上下文精确率与召回率、事实正确性等——并附带测试集合成与提示优化能力。

## 机制是什么

ragas 与前六个项目的差别是**它卖的不是管道，是尺子**。这一点决定了该看什么。

langchain / llama-index / haystack 解决「怎么把 RAG 搭起来」；lightrag / graphrag / ragflow 解决「怎么把 RAG 做成一个产品」；**ragas 解决「搭起来的 RAG 到底好不好」**。

因此它的机制核心不是「组件如何连接」，而是「**怎么把一个主观的质量问题变成一个可复现的计算**」。看它的每个指标都是一次同样的套路：

1. **分解**：把难以直接判断的整体问题拆成可判定的单元（答案 → 原子陈述；上下文 → 逐块是否有用）；
2. **逐项判定**：让模型对每个单元给出结构化判定（输出被 pydantic 模型约束，不是自由文本）；
3. **聚合**：把判定折算成比例或加权分。

这套三段式的价值不在准确度（判定仍由模型做，仍不确定），而在**可解释与可定位**——分数低时能指出具体是哪条陈述、哪块上下文出的问题。这也是为什么 ragas 每个指标都自带一套输出模型：**结构化的中间结果本身就是产品**。

它还有一条值得单独读的弧线：**同一个指标存在两代实现**。`metrics/_*.py` 是旧式 `Metric` 子类，`metrics/collections/*/` 是新式 `BaseMetric` 组件。两代并存不是为了兼容，而是迁移在半路——新式实现明确拒绝旧式包装器（因为需要「给定 pydantic 模型的结构化输出」这个能力），旧式实现仍被大量文档与集成引用。**理解两代的差别才能理解这个库当前的形态。**

本文库按「抽象 → 指标 → 执行 → 支撑」组织：**9 个机制领域、共 35 篇机制页**。

## 怎么实现的

下表的 9 个领域按下列顺序排列：A 指标抽象 → B LLM 判定类指标 → C 非 LLM 指标 → D 数据集与执行 → E 提示层 → F 模型接入 → G 测试集合成 → H 优化器 → I 运行时支撑。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：`src/ragas/` 下 238 个 py 文件，`metrics` 占 13k 行（含 `collections` 6.2k）。`tests/`、`docs/`、`examples/` 不建页；`integrations/`（langchain / llama-index / haystack / langgraph / ag_ui 等十来个适配器）只作为 F 组的旁注，不单独建页——它们是**接入胶水**而不是本库的机制。

**一次评测的一句话概览**（细节见 D 组）：准备 `EvaluationDataset`（单轮或多轮样本）→ 选一组指标 → `evaluate` 按并发把每个样本 × 每个指标跑一遍 → 汇总成 `EvaluationResult` 表。每个指标内部大多是「提示 → 结构化输出 → 解析 → 打分」的固定链路。

## 机制导航（35 篇 / 9 组）

### A-指标抽象（4 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-legacy-metric.md`](a01-legacy-metric.md) | 旧式指标契约 |
| [`a02-modern-metric.md`](a02-modern-metric.md) | 新式组件指标 |
| [`a03-metric-decorator.md`](a03-metric-decorator.md) | 装饰器式自定义指标 |
| [`a04-metric-result.md`](a04-metric-result.md) | 结果结构与输出校验 |

### B-LLM 判定类指标（9 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-faithfulness.md`](b01-faithfulness.md) | 忠实度：陈述级判据 |
| [`b02-answer-relevance.md`](b02-answer-relevance.md) | 答案相关性与正确性 |
| [`b03-context-quality.md`](b03-context-quality.md) | 上下文精确率与召回率 |
| [`b04-factual-correctness.md`](b04-factual-correctness.md) | 事实正确性与声索分解 |
| [`b05-topic-and-summary.md`](b05-topic-and-summary.md) | 主题一致性与摘要质量 |
| [`b06-agent-metrics.md`](b06-agent-metrics.md) | Agent 与会话指标 |
| [`b07-rubrics.md`](b07-rubrics.md) | 评分规则类指标 |
| [`b08-multimodal-metrics.md`](b08-multimodal-metrics.md) | 多模态指标 |
| [`b09-noise-sensitive.md`](b09-noise-sensitive.md) | 噪声敏感度与引用片段 |

### C-非 LLM 指标（2 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-string-metrics.md`](c01-string-metrics.md) | 字符串类指标 |
| [`c02-semantic-metrics.md`](c02-semantic-metrics.md) | 语义相似与结构化比对 |

### D-数据集与执行（4 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-dataset-schema.md`](d01-dataset-schema.md) | 样本与数据集结构 |
| [`d02-evaluation-run.md`](d02-evaluation-run.md) | 评测执行与并发 |
| [`d03-run-config.md`](d03-run-config.md) | 运行配置与重试 |
| [`d04-experiment.md`](d04-experiment.md) | 实验与表格容器 |

### E-提示层（3 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-prompt-base.md`](e01-prompt-base.md) | 提示契约 |
| [`e02-pydantic-prompt.md`](e02-pydantic-prompt.md) | 结构化输出提示 |
| [`e03-dynamic-few-shot.md`](e03-dynamic-few-shot.md) | 动态少样本 |

### F-模型接入（2 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-llm-wrappers.md`](f01-llm-wrappers.md) | 模型包装与工厂 |
| [`f02-embedding-wrappers.md`](f02-embedding-wrappers.md) | 嵌入包装与工厂 |

### G-测试集合成（4 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-knowledge-graph.md`](g01-knowledge-graph.md) | 文档知识图谱 |
| [`g02-persona.md`](g02-persona.md) | 问题角色生成 |
| [`g03-synthesizer-base.md`](g03-synthesizer-base.md) | 合成器契约与场景 |
| [`g04-testset-generator.md`](g04-testset-generator.md) | 测试集总装 |

### H-优化器（2 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-genetic-optimizer.md`](h01-genetic-optimizer.md) | 提示遗传优化 |
| [`h02-dspy-optimizer.md`](h02-dspy-optimizer.md) | DSPy 编译优化 |

### I-运行时支撑（5 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-cache.md`](i01-cache.md) | 结果缓存 |
| [`i02-cost.md`](i02-cost.md) | token 用量与成本回调 |
| [`i03-tokenizers.md`](i03-tokenizers.md) | 分词器适配 |
| [`i04-async-utils.md`](i04-async-utils.md) | 异步工具与追踪 |
| [`i05-backends.md`](i05-backends.md) | 数据集后端 |

## 关键取舍

**源码事实：** 两代指标实现并存：`metrics/_*.py` 的旧式 `Metric` 子类，与 `metrics/collections/*/metric.py` 的新式 `BaseMetric` 组件。新式实现只接受支持结构化输出的 `InstructorBaseRagasLLM`，旧式包装器被明确拒绝。

**源码事实：** 每个指标的判定步骤都把输出约束成 pydantic 模型，解析失败时有修复与重试路径（`RagasOutputParser` / `FixOutputFormat`）。

**源码事实：** 空陈述列表返回 `float("nan")` 而不是 0（注释「return NaN like legacy」），并在聚合处再兜底一次。

**源码事实：** 忠实度把**所有上下文合并成一整段**再一次性判定，不逐块判定；因此不知道「被哪块支持」——那由单独的 `QuotedSpansAlignment` 指标负责。

**源码事实：** 指标分三类输出形态：数值（`metric` + `NumericMetric`）、离散（`discrete_metric`）、排序（`ranking_metric`），各自有配套的 `Validator`。

**源码事实：** 种子测试集生成基于「文档知识图谱 + 角色画像 + 单跳/多跳合成器」，而不是直接从文档随机出题。

**架构推论：** 「分解 → 逐项判定 → 聚合」是**把主观判断变成可复现计算**的通用手法。代价是 LLM 调用次数上升（一个指标往往要两三次），收益是**可解释、可定位、可聚合**。这是评测库与业务代码的根本分野：业务代码要「一个分数」，评测库要「分数 + 为什么」。

**架构推论：** 「空值返回 NaN 而不是 0」这条一致性规则是评分系统的分水岭。一旦把「没数据」算成 0，所有聚合统计（平均、通过率）都会被污染，而且**污染方向永远偏向差评**——于是「配置问题」会被误诊成「系统质量差」。这类错误在评测工具里极其常见，而且极难发现。

**架构推论：** 指标的语义边界应该驱动实现形态。「忠实度」只问「有没有据」→ 合并上下文是对的；「引用对齐」要问「哪一块」→ 必须单开指标。若把两个需求塞进一个指标，实现会变成「每块都判一遍」，成本翻几十倍而多数调用方并不需要那个精度。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `src/ragas/metrics/base.py` | `Metric` | 旧式指标契约 |
| `src/ragas/metrics/collections/base.py` | `BaseMetric` | 新式组件指标契约 |
| `src/ragas/metrics/collections/faithfulness/metric.py` | `Faithfulness` | 忠实度（陈述级判定） |
| `src/ragas/metrics/collections/context_precision/metric.py` | `ContextPrecisionWithReference` | 上下文精确率 |
| `src/ragas/metrics/collections/factual_correctness/metric.py` | `FactualCorrectness` | 事实正确性 |
| `src/ragas/metrics/decorator.py` | `create_metric_decorator` | 装饰器式自定义指标 |
| `src/ragas/metrics/result.py` | `MetricResult` | 结果承载结构 |
| `src/ragas/evaluation.py` | `evaluate` | 评测编排 |
| `src/ragas/executor.py` | `Executor` | 并发执行器 |
| `src/ragas/dataset_schema.py` | `EvaluationDataset` | 数据集容器 |
| `src/ragas/prompt/pydantic_prompt.py` | `PydanticPrompt` | 结构化输出提示 |
| `src/ragas/llms/base.py` | `llm_factory` | 模型工厂 |
| `src/ragas/testset/synthesizers/generate.py` | `TestsetGenerator` | 测试集总装 |
| `src/ragas/optimizers/genetic.py` | `GeneticOptimizer` | 提示遗传优化 |
| `src/ragas/cache.py` | `DiskCacheBackend` | 结果磁盘缓存 |

## 参考

- [ragas 源码仓库](https://github.com/explodinggradients/ragas)
- [ragas 官方文档](https://docs.ragas.io/)
- [RAGAS 论文（Es et al., 2023）](https://arxiv.org/abs/2309.15217)
- [ragas 文档：可用指标](https://docs.ragas.io/en/stable/concepts/metrics/available_metrics/)
- [ragas 源码：metrics/collections/faithfulness/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/faithfulness/metric.py)

## 与主核 / AI 的关系

ragas 与主核（多模态数据库内核）的关系是「**评价器与约束器**」这一对概念的对照：ragas 给数据算分，数据库给数据立规矩。

**指标的三段式 vs 数据库的行级约束。** 数据库的 `CHECK` 约束逐行判定、任一行失败即拒绝；ragas 逐条陈述判定、然后算比例。**同一个分解动作，因为用途不同导出相反的语义**：约束要求「全部通过」（二值门禁），指标要的是「通过多少」（比例度量）。一条不忠实不会让整个答案作废，正如一行不合规不会让整张表报废——除非你把两件事混为一谈。

**NaN vs NULL。** ragas 用 `float("nan")` 表示「无数据可判」，数据库用 `NULL` 表示「未知」，语义完全对应，连踩的坑都一样：两者参与算术都得「未知」。这正是想要的传播行为——**用未知数据算出的平均值没有意义**。数据库用三值逻辑统一处理，ragas 靠每个指标自己记住；后者的风险是**新写的指标容易忘**。

**评测确定性 vs 事务确定性。** 数据库的事务保证「要么全成要么全败」，这是可证明的；ragas 的指标只保证「同样的输入按同样的步骤算」，中间判定由模型给出，**不保证两次运行同分**。这是评测工具的本质限制——它能做到的是**把不确定性显式化**（哪条陈述被判倒、分数怎么来的），而不是消除它。

**上下文合并 vs 逐块判定，与数据库的聚合下推。** 数据库会为了减少 I/O 把过滤下推到存储层；ragas 为了减少 LLM 调用把多块上下文合并成一次判定。两者都是**在「精度」与「成本」之间选成本**，判据也一样：下游到底需不需要那个精度。「忠实度」不需要知道哪一块，所以合并；「引用对齐」需要，所以单开指标。**先看清需求再决定精度，比先实现最高精度再砍成本要正确得多。**

**测试集合成 vs 数据库的测试数据生成。** ragas 从知识图谱 + 角色 + 多跳合成器造题，与数据库的「按约束随机生成合法数据」是同一类工具：**都要保证生成的东西「有意义」而不是「合法但无信息」**。数据库的难点是保持外键与业务不变量，ragas 的难点是造出的问题真的能从语料里答出来——两者都在「生成器必须理解数据语义」这一条上卡住。

对 agent 工程最直接的价值有四条。其一：**评测必须可定位**，只给一个分数没有工程价值——出问题时要知道是哪条陈述、哪块上下文。其二：**空值绝不能算成 0 分**，否则配置问题被误诊为质量问题。其三：**指标的语义边界决定实现成本**，把两个需求塞进一个指标会让 LLM 调用次数翻几十倍。其四：**测试集要能自动生成**，否则评测覆盖率会长期停在「手工写的二十个问题」。
