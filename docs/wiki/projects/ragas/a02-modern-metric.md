---
project: ragas
feature: modern-metric
title: 新式组件指标
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/metrics/collections/base.py
    symbols: [BaseMetric]
  - path: src/ragas/metrics/collections/__init__.py
    symbols: [Faithfulness, AnswerRelevancy]
  - path: src/ragas/metrics/collections/summary_score/metric.py
    symbols: [SummaryScore]
---

# 新式组件指标

## 一句话职责

`BaseMetric` 是 ragas 第二代指标的基类：它把「指标」从「有列声明、有四个调用入口的类」收窄成「只有一个 `ascore(**kwargs)` 的组件」，并要求依赖必须是最新式的 LLM / 嵌入包装器，旧式包装器在构造时就被明确拒绝。

## 机制是什么

两代指标体系的差别不是命名，而是**契约的形状**：

| 维度 | 旧式（`metrics/base.py`） | 新式（`collections/base.py`） |
|---|---|---|
| 输入 | 由 `_required_columns` 声明，样本被裁剪后传入 | 无声明，`ascore` 的自由参数直接收 |
| 入口 | 四个（单轮/多轮 × 同步/异步） | 一个 `ascore`，同步壳自动包一层 |
| 返回 | `float`（或 `nan`） | `MetricResult`（值 + 理由 + 轨迹） |
| 依赖校验 | `isinstance(llm, BaseRagasLLM)` 后调 `set_run_config` | 必须是 `InstructorBaseRagasLLM`，否则抛错 |
| 依赖来源 | 类字段 `llm` / `embeddings` | 同名字段，但由具体指标在 `__init__` 里自设 |

关键在最后两行。新式指标**不接受**旧式 `BaseRagasLLM`，也不接受旧式 `BaseRagasEmbeddings`——不是因为不兼容，而是因为新式实现依赖一个旧式没有的能力：**给定 pydantic 输出模型的结构化生成**（`agenerate(prompt, OutputModel)`）。旧式 LLM 只能返回文本，所以它无法支撑新式指标「每次调用都返回强类型对象」的写法。

`collections/__init__.py` 是这份契约的目录：它把约四十个名字从各自的子模块里再导出，`__all__` 分成「基类 / RAG 指标 / Agent 与工具指标 / 评分规则指标 / SQL 与数据指标」四组。读这份清单能直接看出迁移走到了哪里——`AnswerRelevancy`、`Faithfulness`、`ContextPrecision` 三代同名类都在里面，而旧式实现仍留在 `metrics/_*.py`。

## 怎么实现的

**`BaseMetric` 是三重继承的组合。** `class BaseMetric(SimpleBaseMetric, NumericValidator)`：

- `SimpleBaseMetric`（在 `metrics/base.py`）提供 `name` 与 `allowed_values` 两个字段、抽象 `score` / `ascore`、以及批量版 `batch_score` / `abatch_score`。
- `NumericValidator` 提供一个 `validate_result_value`，把结果值限定在 `allowed_values` 声明的数值区间。
- `BaseMetric` 自己只加两件事：构造期组件校验，与同步/异步壳。

因此新式指标的默认取值域是 `(0.0, 1.0)`——`__init__` 的签名直接写着 `allowed_values: Tuple[float, float] = (0.0, 1.0)`。

**构造期的校验是「有才校验」。** `__init__` 里两行判断：

- `if hasattr(self, "llm"): self._validate_llm()`
- `if hasattr(self, "embeddings"): self._validate_embeddings()`

也就是说基类不知道某个指标到底需不需要 LLM 或嵌入——它只是「如果子类声明了这个字段，就必须是新型包装器」。`_validate_llm` 用 `isinstance(llm, InstructorBaseRagasLLM)`，失败抛 `ValueError`，消息里既指出实际类型，也给出正确的构造方式（`llm_factory('gpt-4o-mini', client=openai_client)`）。`_validate_embeddings` 同理，要求 `BaseRagasEmbedding`，消息里给出 `embedding_factory(..., interface='modern')`。

这条「hasattr 才校验」的设计解释了新式指标的构造顺序：具体指标在 `super().__init__()` **之前**就把 `self.llm` / `self.embeddings` 赋值好（页内多处注释写着「Set attributes explicitly before calling super()」），否则基类看不到这些字段，校验会被跳过。这是一个隐含的时序契约，不是显式的参数传递。

**`score` 是 `ascore` 的同步适配壳，且拒绝在事件循环里被调用。** 实现用一个非常规的手法：先 `asyncio.get_running_loop()`，若成功就主动 `raise RuntimeError("Cannot call sync score() from an async context...")`；随后在 `except RuntimeError` 里靠检查错误消息里是否含 `"Use ascore() instead"` 来区分「这层是我们自己抛的」与「这层是 get_running_loop 抛的」。后者意味着没有运行中的事件循环，于是安全地 `asyncio.run(self.ascore(**kwargs))`。`batch_score` 与 `abatch_score` 是同一套手法，只换了错误文案。

这个写法的好处是不依赖第三方库就能同时支持「脚本里同步调用」与「Jupyter 里报出清晰错误」；代价是靠**字符串匹配**判断异常来源，属于脆弱的实现。

**`BaseMetric.ascore` 只是占位。** 它返回 `MetricResult(value=0.0, reason="Base metric placeholder - override ascore() in subclass")`——故意给一个可识别的固定理由，让「忘了实现」在结果里可见而不是静默通过。

**子类的标准形态：把提示词对象与组件在 `__init__` 里一起装配。** 以 `SummaryScore` 为例（`collections/summary_score/metric.py`）：

1. 显式赋 `self.llm`、业务参数（`length_penalty`、`coeff`）、以及三个提示词类实例（抽关键词 / 生成问题 / 生成回答）。
2. 校验 `coeff` 在 0 到 1 之间，越界抛 `ValueError`。
3. 再 `super().__init__(name=name, **kwargs)`——注意**不把 `llm` 塞进 kwargs**，因为 `llm` 已经在 `self` 上，基类靠 `hasattr` 找到它。

`ascore` 的签名是 `(self, reference_contexts: List[str], response: str) -> MetricResult`。它对两个入参做显式校验（上下文不能为空或全空白、回答不能为空或全空白），然后走五步：抽关键词 → 生成是非问句 → 用摘要作答 → 算问答得分 → 可选加简洁度惩罚。

**每一次模型调用都是「提示词对象 → 字符串 → 结构化输出」。** `_extract_keyphrases` 里先 `ExtractedKeyphrasesInput(text=text)` 构造输入模型，再 `self.extract_keyphrases_prompt.to_string(input_data)` 渲染，最后 `await self.llm.agenerate(prompt_str, ExtractedKeyphrases)` 拿到 pydantic 实例并直接读 `.keyphrases`。全程没有一次文本解析，也没有「模型没按格式输出怎么办」的分支——这一步被推给了 `InstructorBaseRagasLLM` 的实现（它内部负责重试与修复）。

**空结果是「记日志并继续」，不是抛错，也不是 NaN。** `SummaryScore` 两处都是：关键词为空或问题为空时 `logging.error(...)` 然后置成空列表继续往下。最终会走到 `_compute_qa_score`：`correct / len(answers)`——**回答为空时这里会抛 `ZeroDivisionError`**，源码注释明确写了「Will raise ZeroDivisionError if answers is empty (legacy behavior)」。这处是新式实现里少见的「不兜底」：它选择与旧实现行为一致，而不是改成 NaN。

**最后 `MetricResult(value=float(final_score))` 只带值，不带理由。** 新式实现普遍不给 `reason`——理由藏在中间的结构化输出里，但当前版本没有把它们汇总进结果。`MetricResult` 的三个字段（`value`、`reason`、`traces`）在 `SummaryScore` 里只用了第一个。

**目录是再导出清单，不是逻辑。** `collections/__init__.py` 里 `Faithfulness` 与 `AnswerRelevancy` 都来自各自的子包，`BaseMetric` 来自 `collections/base.py`。清单的存在让 `from ragas.metrics.collections import X` 成为统一入口，也意味着「新式实现有哪些」可以直接靠读这个文件数出来。

## 关键取舍

**源码事实：** `BaseMetric` 不声明「本指标需要哪些输入字段」。旧式的 `_required_columns` 白名单契约在这里消失了，`ascore` 直接收具名参数。代价是没有统一的样本裁剪与缺列检查——每个指标自己写 `if not user_input: raise ValueError(...)`，错误消息风格各不相同。

**源码事实：** 依赖校验靠 `hasattr` + 在 `super().__init__()` 之前赋值。这个时序契约没有写在签名里，只能从各子类的注释与顺序读出来。若某个子类先调 `super()` 再赋 `self.llm`，校验会被静默跳过。

**源码事实：** 旧式 LLM/嵌入被**明确拒绝**而不是「尽力兼容」。错误消息直接给出替代工厂函数的调用示例，把「怎么改」写进异常里。

**源码事实：** `score` / `batch_score` 用「抛出再捕获 RuntimeError 并匹配消息文本」来区分两种失败来源。这是一处可以更简洁但选择了省依赖的实现。

**架构推论：** 新式指标把「结构化输出」当作基础设施而不是指标自己的责任。于是每个指标的实现里都没有解析、重试、格式修复的代码——那些全部下沉到 LLM 包装器。这解释了为什么迁移是**分段**的：只要 `InstructorBaseRagasLLM` 这条链路还没铺到所有用户，旧式实现就不能删。

**架构推论：** 返回值从 `float` 换成 `MetricResult`，是把「分数」升级为「分数 + 可追溯信息」的容器。当前多数实现只填 `value`，所以收益尚未兑现；但接口已经预留了 `reason` 与 `traces` 两个位置，且 `MetricResult` 支持算术与比较运算，因此下游聚合代码不必改写就能继续用（详见 `a04-metric-result`）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/metrics/collections/base.py` | `BaseMetric` | 新式基类：组件校验 + 同步/异步壳 + 数值校验 |
| `src/ragas/metrics/collections/base.py` | `BaseMetric.__init__` | `hasattr` 门控的组件校验，默认区间 (0, 1) |
| `src/ragas/metrics/collections/base.py` | `BaseMetric._validate_llm` | 要求 `InstructorBaseRagasLLM`，拒绝旧式包装器 |
| `src/ragas/metrics/collections/base.py` | `BaseMetric._validate_embeddings` | 要求 `BaseRagasEmbedding` |
| `src/ragas/metrics/collections/base.py` | `BaseMetric.score` | 同步壳：无事件循环时 `asyncio.run`，否则报错 |
| `src/ragas/metrics/collections/__init__.py` | `Faithfulness` | 再导出：新式忠实度 |
| `src/ragas/metrics/collections/__init__.py` | `AnswerRelevancy` | 再导出：新式答案相关性 |
| `src/ragas/metrics/collections/summary_score/metric.py` | `SummaryScore` | 范例：五步链路 + 组件自装配 |
| `src/ragas/metrics/collections/summary_score/metric.py` | `SummaryScore.ascore` | 入参校验、分步调用、加权合并 |
| `src/ragas/metrics/collections/summary_score/metric.py` | `_compute_qa_score` | 问答得分，空列表时按旧行为抛除零 |
| `src/ragas/metrics/base.py` | `SimpleBaseMetric` | 新式的公共底：字段、抽象方法、批量实现 |
| `src/ragas/metrics/base.py` | `SimpleLLMMetric` | 从提示词 + LLM 生成 `MetricResult` 的通用实现 |

## 参考

- [ragas 源码：metrics/collections/base.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/base.py)
- [ragas 源码：metrics/collections/__init__.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/__init__.py)
- [ragas 源码：collections/summary_score/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/summary_score/metric.py)
- [ragas 文档：指标总览](https://docs.ragas.io/en/stable/concepts/metrics/)

## 与主核 / AI 的关系

新式指标与旧式指标的关系，很像主核里「老式按页读写接口」与「新式按对象/事务接口」的关系：后者不是前者的超集，而是**建立在更强能力之上**的新一层，因此不能双向兼容。主核可以选择保留两套接口让调用方慢慢迁；ragas 的选择更硬——新式直接拒绝旧式依赖，把兼容成本推给用户。

「结构化输出当作基础设施」这一点值得对照主核的约束校验。主核把「这个词法上合法但语义上违反约束」的判断放在写入路径，一次拒绝；ragas 的旧式指标把「模型输出要不要符合格式」的判断散落在每个指标里（有的用 `CodeParser` 抠 JSON，有的自己修补），而新式把它上移到 LLM 包装器，指标只读强类型结果。**判断上移的收益是每个指标都少一段脆弱代码，代价是整条链路必须一起升级。**

对 agent 工程的实际含义有两条。其一，选哪一代取决于依赖能否满足：拿不到 `InstructorBaseRagasLLM` 就只能用旧式指标，而这与指标质量无关。其二，`MetricResult` 目前多数只有 `value`，把 `reason` 当作可用信息会落空——想要可解释性，得自己把中间的结构化输出取出来（新式实现把提示词对象暴露在实例上，正是为此）。
