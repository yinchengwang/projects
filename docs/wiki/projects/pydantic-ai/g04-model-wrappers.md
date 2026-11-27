---
project: pydantic-ai
feature: model-wrappers
title: "模型包装：插桩与并发限流"
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/models/instrumented.py
    symbols: [InstrumentedModel, InstrumentationSettings, instrument_model]
  - path: pydantic_ai_slim/pydantic_ai/models/concurrency.py
    symbols: [ConcurrencyLimitedModel, limit_model_concurrency]
  - path: pydantic_ai_slim/pydantic_ai/models/wrapper.py
    symbols: [WrapperModel]
---

# 模型包装：插桩与并发限流

## 一句话职责

用一个「透传但可介入」的基类 `WrapperModel`，把「记录请求」和「限制并发」这两类横切关注点做成可叠加的模型装饰层，从而不必在每个供应商适配器里各写一遍。

## 机制是什么

`WrapperModel(Model)` 只做一件事：持有一个 `wrapped` 模型，并把它所有方法原样转发。它的文档写得很干脆——「自己什么都不做，作为基类使用」。转发的不只是 `request`、`request_stream`，还有 `batch_max_requests`、`batch_request_size`、`_batch_credential`、`_run_scope`、`_refresh_http_client` 等一批内部协作方法。

在它之上叠了两个具体包装：

- `InstrumentedModel(wrapped, options)`：在 `request` 外层开一个 OTel span，把响应的属性写进 span 后结束。`InstrumentationSettings` 承载开关（是否记录内容、span 命名版本等）。
- `ConcurrencyLimitedModel(wrapped, limiter)`：在每次请求前后进出并发限流器。`limiter` 可以是整数、`ConcurrencyLimit` 配置对象，或共享的 `AbstractConcurrencyLimiter`。

`instrument_model(model, instrument)` 与 `limit_model_concurrency(model, limiter)` 是两条便利函数：前者在 `instrument` 为真且模型还不是 `InstrumentedModel` 时包一层（幂等），后者在 limiter 归一化为 `None` 时直接返回原模型（不产生空包装）。

## 怎么实现的

**「借用」机制解决双重进入。** `WrapperModel.__aenter__` 会进入被包装模型，但如果被包装模型的生命周期由其所有者管理，就不该由包装层再进入一次。`_Borrowed` 这个 dataclass 加上 `_borrowed` 这个 ContextVar 实现了这个判断：只有在**正在进出包装层的那个任务**里、且**只有那一段时间内**，被包装模型才被视为「借用」。注释特别说明：包装层启动的子任务会复制上下文，因此那些子任务里必须照常进出模型——这条区分避免了「父任务借用」被错误地传播给子任务。

**插桩层刻意不复用准备结果。** `InstrumentedModel.request` 里开 span 时构造了一个 `ModelRequestContext`，但紧接着的注释说明：**这个上下文只用于生成 span 属性，被包装模型会自己再 prepare 一次**，因为 `prepare_request` 不是幂等的——第二次会重复追加提示输出模板、重复遍历已转换的 JSON schema。所以必须把原始参数交给被包装者。注释还点名另外两处做同样处理：`Instrumentation.wrap_model_request` 与 `FallbackModel.request`。这是一个跨三处的一致性约定。

**`instrument_model` 的幂等性有方向。** 它只在「尚未是 `InstrumentedModel`」时包装，因此重复调用不会叠出两层 span。但它不检查被包装者内部是否已插桩——一个已插桩的模型再被包进别的 wrapper 后，外层调用 `instrument_model` 会再包一层。这是可接受的，因为「包在 wrapper 外面的插桩」与「wrapper 内部的插桩」测的是不同层次。

**`limit_model_concurrency` 的 `None` 是显式的。** limiter 归一化为 `None` 时返回原模型（字符串则先 `infer_model`），而不是返回一个不做事的包装。这条看似小的选择避免了「无意义的一层转发」出现在调用栈里，也就避免了排查时的困惑。

**共享 limiter 是这套设计的关键用途。** `ConcurrencyLimitedModel` 的文档示例直接展示了两条模型共用同一个 `ConcurrencyLimiter`：

```
shared = ConcurrencyLimiter(max_running=10, name='openai-pool')
m1 = ConcurrencyLimitedModel('openai:gpt-4o', limiter=shared)
m2 = ConcurrencyLimitedModel('openai:gpt-4o-mini', limiter=shared)
```

这解释了三件事：为什么 limiter 参数接受「对象」而不只是整数（要能共享）、为什么需要 `name`（共享池在观测里要能区分）、为什么限流要做在模型层而不是 agent 层（池的作用域是 API 不是一个 agent）。

**spans 的命名是版本化的。** `InstrumentationNames.for_version(version)` 在 `version == 2` 时返回旧名（`agent run`、`running tool`、属性 `agent_name`、`tool_arguments`、`tool_response`），否则返回语义约定名（`invoke_agent`、`execute_tool`、`gen_ai.agent.name`、`gen_ai.tool.call.arguments`、`gen_ai.tool.call.result`）。这个双版本共存让升级插桩格式不破坏已有的看板与查询。

**并发限流的接入点是 `_active_model_limiter`。** 模块里有一个 `_active_model_limiter` 上下文管理器，把当前 limiter 放进上下文，供更深层需要感知限流的代码读取。这暗示限流不只在「请求进入」这一个点生效——模型内部若发起子请求，也能看到当前限流环境。

**包装层必须转发内部协作方法，不能只转发公开接口。** `WrapperModel` 转发 `_run_scope`、`_refresh_http_client`、`_batch_credential`、`_batch_unlisted_status` 等私有方法。原因是这些方法参与运行期资源管理（HTTP 客户端复用、批处理凭据），如果包装层不转发，被包装模型会退化成默认实现，表现为「加了插桩后批处理行为变了」这类难查的问题。

## 关键取舍

**源码事实：** `WrapperModel` 转发大量私有方法，而不只是 `request`。任何新增一条需要跨层的内部协议，都必须同步加到包装层的转发清单里，否则包装会静默改变行为。

**源码事实：** 插桩层**不复用** `prepare_request` 的结果，而是把原始参数传给被包装模型，让它自己 prepare。原因是 `prepare_request` 非幂等。这是「包装层必须理解被包装方的幂等性」的典型例子。

**源码事实：** `_Borrowed` 借用判定的作用域被双重限定：同一任务 + 活跃期。子任务复制上下文所以不受影响。这让「包装层进出模型」与「模型所有者进出模型」不会互相踩。

**源码事实：** `instrument_model` 幂等（不叠同名插桩），`limit_model_concurrency` 对 `None` 直接透传（不产生空包装）。两者都在避免「无意义层」。

**源码事实：** 限流器可共享，因此限流的作用域是**API 池**而不是单个 agent 或单次运行。这是把它做在模型层而非运行层的根本原因。

**源码事实：** span 名与属性名有 v2 与语义约定两套，按版本切换。插桩格式是外部契约（看板、告警、查询都依赖），不能无声改。

**架构推论：** 包装模式在这里的收益是**组合**：插桩 + 限流可以任意叠加，顺序不同效果不同但都能工作（先限流再插桩＝spans 包含等待时间；先插桩再限流＝spans 只含执行时间）。代价是每一层都必须忠实转发其余所有方法，这个「转发清单」会随模型接口增长而膨胀，且漏转发不会报错、只会让行为悄悄降级。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/models/wrapper.py` | `WrapperModel` | 透传基类，转发公开与内部协作方法 |
| `pydantic_ai_slim/pydantic_ai/models/wrapper.py` | `_Borrowed` | 借用判定：同一任务且在活跃期内才视为借用 |
| `pydantic_ai_slim/pydantic_ai/models/wrapper.py` | `serving_wrapper_scope` | 进入包装层时把被包装模型标记为借用 |
| `pydantic_ai_slim/pydantic_ai/models/instrumented.py` | `InstrumentedModel` | 请求前后的 OTel span 包装 |
| `pydantic_ai_slim/pydantic_ai/models/instrumented.py` | `InstrumentationSettings` | 插桩开关与内容记录策略 |
| `pydantic_ai_slim/pydantic_ai/models/instrumented.py` | `instrument_model` | 幂等包装函数 |
| `pydantic_ai_slim/pydantic_ai/models/concurrency.py` | `ConcurrencyLimitedModel` | 请求级并发限流包装 |
| `pydantic_ai_slim/pydantic_ai/models/concurrency.py` | `limit_model_concurrency` | 便利包装，`None` 时透传原模型 |
| `pydantic_ai_slim/pydantic_ai/models/concurrency.py` | `_active_model_limiter` | 把当前 limiter 放入上下文供深层读取 |
| `pydantic_ai_slim/pydantic_ai/_instrumentation.py` | `InstrumentationNames.for_version` | span 名与属性名的双版本切换 |
| `pydantic_ai_slim/pydantic_ai/_instrumentation.py` | `open_model_request_span` | 请求 span 的打开与结束协议 |

## 参考

- [pydantic-ai 源码：models/wrapper.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/models/wrapper.py)
- [pydantic-ai 源码：models/instrumented.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/models/instrumented.py)
- [pydantic-ai 源码：models/concurrency.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/models/concurrency.py)
- [Pydantic AI 文档：Logfire 集成](https://ai.pydantic.dev/logfire/)

## 与主核 / AI 的关系

模型包装层与主核里「存储引擎的装饰器层」（统计、限流、审计）是同一手法：不改被装饰者，只在其调用边界插入逻辑。主核的装饰器同样必须转发底层接口的全部方法，否则会丢能力——这是装饰器模式在任何系统里的共同代价。

对 agent 工程的直接价值有三条。其一，**限流放在模型层才能跨 agent 共享池**。如果你的配额是按 API key 算的，把限流做在 agent 或运行层会导致多个 agent 各自计数、合计超限。其二，**包装顺序有语义**：`InstrumentedModel(ConcurrencyLimitedModel(m))` 与反过来测到的是不同的东西（是否包含排队等待），选错会把「限流排队延迟」误认为「模型响应慢」。其三，**非幂等的方法不能被包装层重复调用**。`prepare_request` 就是一个例子：任何自己在包装层里「顺手先 prepare 一下」的实现都会悄悄多插一段提示模板。包装层应当只做观测与门控，不重做被包装方的准备工作。
