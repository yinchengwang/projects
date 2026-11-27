---
project: pydantic-ai
feature: builtin-capabilities
title: 内置能力清单
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/capabilities/caching.py
    symbols: [Caching]
  - path: pydantic_ai_slim/pydantic_ai/capabilities/thinking.py
    symbols: [Thinking]
  - path: pydantic_ai_slim/pydantic_ai/capabilities/model_fallback.py
    symbols: [ModelFallback]
  - path: pydantic_ai_slim/pydantic_ai/capabilities/prepare_tools.py
    symbols: [PrepareTools, PrepareOutputTools]
---

# 内置能力清单

## 一句话职责

内置能力是同一份契约的几种具体填法：有的只贡献一个模型设置，有的接管「这一次请求用哪个模型」，有的在工具准备阶段过滤工具定义——每一个都能指出它改写了哪个阶段的哪个钩子。

## 机制是什么

按介入的深度排，内置能力大致分三档：

- **只贡献静态配置**：`Caching` 与 `Thinking`。它们只实现 `get_model_settings()`，交给运行时的设置合并链，不参与任何生命周期钩子。
- **改写模型选择**：`ModelFallback`。它既贡献模型（当 agent 没有自己的模型时），又在 `prepare_model_request` / `after_model_request` / `on_model_request_error` 三个钩子上接管「换一个模型再试」。
- **改写工具清单**：`PrepareTools` 与 `PrepareOutputTools`。它们只实现 `prepare_tools` / `prepare_output_tools`，把一个 `ToolsPrepareFunc` 包成能力。

这三档恰好覆盖了能力的三种典型用途：配置、控制流、内容加工。读它们的实现，比读契约本身更能看清「能力到底能改什么」。

## 怎么实现的

**`Caching`：把两个布尔/枚举旋钮映射成一个 `cache` 设置。** 它有两个输入：`retention`（`True` / `False` / `'5m'` / `'30m'` / `'1h'` / `'12h'`）与 `messages`（是否连增长的会话一起缓存）。`get_model_settings` 的三条分支写得很细：

- `messages=True` 或 `retention=False`：直接 `ModelSettings(cache=self.retention)`——后者之所以走这条，是因为「关掉缓存」这个意图不需要再区分缓存范围。
- `retention=True` 且 `messages=False`：`CacheConfig(messages=False)`，即用供应商默认保留期但只缓存稳定前缀。
- 其他（具体保留期 + 不缓存会话）：`CacheConfig(retention=..., messages=False)`。

它默认 `id='caching'`，docstring 里写明了这个 id 的语义：「一个 agent 只有一份缓存配置，所以 id 由类固定下来；两个实例通过 `combine` 变成一，保留后者」。这正是 c02 里「同层同 id 合并」的实例——`Caching` 在类体里声明了默认 id，于是重复出现被视为「同一配置说了两遍」。

还有一条边界写在类注释里：走的是统一的 `cache` 设置以便跨供应商可移植；在隐式缓存提示词的模型上它没有效果；而供应商专用设置（如 `anthropic_cache`、`bedrock_cache_instructions`）在两处都设置时优先。也就是说能力负责「通用旋钮」，供应商专用字段仍归供应商。

**`Thinking`：与 `Caching` 同构，只有一个旋钮。** `effort` 取 `True` / `False` 或六个等级字符串，`get_model_settings` 直接返回 `ModelSettings(thinking=self.effort)`。默认 `id='thinking'`，理由与上一条完全一样。文档注释里的边界也一致：供应商专用设置（`anthropic_thinking`、`openai_reasoning_effort`）优先。

这两个能力加起来只有几十行，却是「能力系统为什么值得存在」的最好例子：如果不用能力，这类配置就得变成 `Agent` 的构造参数，于是每加一个这样的开关都要改一次 `Agent` 的签名与文档。

**`ModelFallback`：一个能力改了三个阶段。** 它的实现最值得细看，因为它同时在四处出手：

1. **模型贡献**（构造期 / `for_agent`）。`for_agent` 只做一件事：判断 `agent.model is None`，据此设置 `_supplies_model`。注释解释了为什么必须这么判：能力贡献的模型会压过 agent 自己的默认，如果无条件贡献，`Agent(model, capabilities=[ModelFallback(...)])` 会被静默重定向到第一个候选模型，而不是「回退到」它。只有当 agent 完全没有模型时才补位。`get_model()` 返回的是**未解析**的候选，使 `resolve_model_id` 之类的能力仍有发言权，与用户直接传给 `Agent` 的模型同等对待。
2. **排序位置**（`get_ordering`）。它声明 `position='innermost'`。理由在注释里：链在那一层按「最内层先跑」执行，且一 `raise` 就短路，所以外层钩子（含埋点）只看得见最终被接受的那个响应。若它排在中间，一次被拒的响应会先被外层的 `after_model_request` 看到（例如被记录成正常响应）。
3. **换模型前的记账**（`prepare_model_request`）。它把「本次尝试用哪个模型」记进 `_attempted_models` / `_attempted_ids`，并按 `ctx.run_step` 决定是否重置链——注意键是 `run_step` 而不是 `request_context.attempt`，这样即使另一个能力要求了第一个额外尝试，新的一步仍会从链头开始。它还处理两种「钉住」情形：续接一个 provider 端挂起的响应时必须回到启动它的模型；`sticky=True` 时整个会话留在上次作答的模型上（为了持续命中供应商的缓存，也避免每一步都先问那个坏模型）。钉住通过抛 `RetryModelRequest(candidate)` 实现——**在还没发出任何东西之前**就把这一步改道。
4. **判定与推进**（`after_model_request` / `on_model_request_error`）。错误路径先排除 `UsageLimitExceeded`（这是本次运行自己的额度拦下的，换模型不会变），再问 `_chain.falls_back(error)`；响应路径先跳过流式（流式响应已经送到消费者手上，无法反悔，只在有响应处理器时告警），再问谓词。任一成立就 `raise self._advance()`。

`_advance` 是链推进的核心：跳过本次已尝试过的候选（`_already_attempted` 用 `_is_or_wraps` 逐层剥包装，因为尝试过的模型此刻可能已被其他能力包了一层），找到第一个未尝试的就返回 `RetryModelRequest(candidate)`；全部试完则 `_chain.exhausted()` 汇总抛出。

`after_model_request` 里有一句注释值得单独记：被拒的响应在进入下一次尝试之前就把 token 与成本记进了 `RunUsage`，所以**被拒的生成照样计费**，即使它从未进入历史。

**`PrepareTools` / `PrepareOutputTools`：把回调包成能力。** 两者结构完全对称，区别只在于挂 `prepare_tools` 还是 `prepare_output_tools`。三个细节：

- `get_serialization_name()` 返回 `None`，注释是「不可 spec 序列化（接受一个可调用对象）」。所以它们只能出现在 Python 代码里，无法写进声明式规格。
- 共用 `_call_prepare_func`：先调，再 `inspect.isawaitable` 判断是否要 await（同一个函数可同步可异步），最后过 `_utils.check_tools_prepare_func_result`。校验的存在理由写在注释里：`PreparedToolset.get_tools` 会验证结果既没新增也没重命名工具，而这些能力的钩子正是经由那里派发的。
- `PrepareOutputTools` 的文档点明它对齐的是**输出**重试预算：`ctx.retry` / `ctx.max_retries` 反映 `max_output_retries`，与输出钩子的生命周期一致。

## 关键取舍

**源码事实：** `Caching` 与 `Thinking` 完全不实现生命周期钩子，只贡献模型设置。这使它们的组合完全由设置合并链解决，不存在顺序敏感的包裹——两个缓存配置合并就是「后者赢」，没有更复杂的语义。

**源码事实：** `ModelFallback` 必须落在 `innermost`，否则「被拒的响应」会被外层 `after_model_request` 误当成正常响应。这条约束是顺序即语义的直接体现。

**源码事实：** `ModelFallback` 只在 agent 没有模型时才贡献模型。这是为了避免能力贡献压过用户显式的 `Agent(model=...)`。

**源码事实：** `ModelFallback` 用 `RetryModelRequest` 表达「换模型」，而不是自己重发请求。好处是重试统一由模型请求阶段的尝试循环处理，坏处是必须理解 `RetryModelRequest` 的特殊控制流地位（见 c02 里那条「不要在错误链里接住它」的规则）。

**源码事实：** `PrepareTools` 系列不可 spec 序列化，因为它们持有可调用对象。同一族的 `Capability` / `Hooks` 之类可以序列化，这种差异决定了「哪些行为能写进 YAML」。

**架构推论：** 内置能力的分档反映了框架对「横切改动」的三个容忍级别：改设置最安全（纯数据），改工具清单次之（影响模型看到什么），改模型选择最危险（影响成本与可复现性）。把最危险的那个钉在最内层并要求显式排序声明，是把风险限制在可预测位置的工程手段。

**架构推论：** `Caching` / `Thinking` 用固定的类级 `id` 表达「一个 agent 只有一份」，而 `PrepareTools` 不声明默认 id（它接受任意回调，两个不同的 `PrepareTools` 显然是两件事）。这条区别正是 c02 里 `_declares_default_id` 那条检查所依据的语义：类体里的默认 id 是类在声明「唯一性」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/capabilities/caching.py` | `Caching` | 只贡献 `cache` 设置，固定 id，三个分支覆盖范围与保留期组合 |
| `pydantic_ai_slim/pydantic_ai/capabilities/thinking.py` | `Thinking` | 只贡献 `thinking` 设置，固定 id |
| `pydantic_ai_slim/pydantic_ai/capabilities/model_fallback.py` | `ModelFallback` | 模型贡献、innermost 排序、按步记账、钉住与推进 |
| `pydantic_ai_slim/pydantic_ai/capabilities/model_fallback.py` | `ModelFallback.for_agent` | 仅在 agent 无模型时补位，避免压过显式模型 |
| `pydantic_ai_slim/pydantic_ai/capabilities/model_fallback.py` | `ModelFallback.prepare_model_request` | 每步重置链、记录尝试、按 needs 改道 |
| `pydantic_ai_slim/pydantic_ai/capabilities/model_fallback.py` | `ModelFallback._advance` | 跳到下一个未尝试候选，耗尽则汇总失败 |
| `pydantic_ai_slim/pydantic_ai/capabilities/prepare_tools.py` | `PrepareTools` / `PrepareOutputTools` | 把回调包成工具准备钩子，校验结果不得增删改名 |

## 参考

- [pydantic-ai 源码：capabilities/caching.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/capabilities/caching.py)
- [pydantic-ai 源码：capabilities/model_fallback.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/capabilities/model_fallback.py)
- [Pydantic AI 文档：能力一览](https://ai.pydantic.dev/capabilities/)
- [Pydantic AI 文档：模型回退](https://ai.pydantic.dev/models/fallback/)

## 与主核 / AI 的关系

`Caching` 与 `Thinking` 这类「只贡献一个设置项」的能力，与主核里的会话级 GUC 参数地位相仿：都是一个统一的配置注入点，取值最终由合并顺序决定，不参与执行流程。差别在主核的参数有明确的生效层级（会话 → 事务 → 语句），而这里的合并在同一层内是「后者赢」，没有更细的作用域。

`ModelFallback` 对应的是主核里「主备切换」类机制：一条候选链 + 一个失败判定 + 一个推进器。两边都把「哪些失败该切换」做成可配置谓词（这里是 `fallback_on` / 响应处理器，主核那边是错误码分类），也都要处理「已经试过的不重试」。真正不同的是成本可见性——`ModelFallback` 明确记录「被拒的生成也计费」，这在实际工程里是必须知道的：回退不是免费的，一次失败的生成已经花掉了 token。

对 AI 工程的实际意义在于：这份清单说明了能力的真实边界。它能改设置、能改工具清单、能换模型，但它**不能改控制流结构**——没有哪个内置能力引入了新的循环或分支节点。这与 c01 的结论一致：能力系统让横切行为可组合，代价是它无法改变图本身。
