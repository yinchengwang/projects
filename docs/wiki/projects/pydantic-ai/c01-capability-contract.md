---
project: pydantic-ai
feature: capability-contract
title: 能力契约与生命周期挂载点
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/capabilities/capability.py
    symbols: [Capability]
  - path: pydantic_ai_slim/pydantic_ai/capabilities/abstract.py
    symbols: [AbstractCapability, CapabilityOrdering]
---

# 能力契约与生命周期挂载点

## 一句话职责

能力（Capability）是 pydantic-ai 表达「横切执行期增强」的唯一机制：它在一张固定的挂载点清单上插入自己的逻辑，而不需要改 Agent 的构造参数或继承 Agent 类。

## 机制是什么

这个框架里所有横切行为——缓存、思考链、模型回退、工具集挂载、工具预处理、内容过滤、持久执行——都不是 Agent 的字段，也不是子类重写，而是同一个 `AbstractCapability` 契约的不同实现。

契约的形态是**成对的钩子**：每个可拦截的阶段提供三个入口——`before_*`（前置副作用）、`after_*`（后置观察）、`wrap_*`（包裹，可决定要不要调下游）、外加 `on_*_error`（错误处理）。这套模式在九个阶段上重复：运行、节点、事件流、模型请求、工具校验、工具执行、输出校验、输出处理、工具准备。

`Capability` 是其中最简的一种：它不写任何钩子，只用来把「指令 + 工具 + 工具集」打包成一个有身份的整体。

## 怎么实现的

**基类的钩子全清单。** `AbstractCapability` 上定义的挂载点按阶段分：

| 阶段 | 钩子 |
|---|---|
| 运行 | `before_run` / `after_run` / `wrap_run` / `on_run_error` |
| 节点 | `before_node_run` / `after_node_run` / `wrap_node_run` / `on_node_run_error` |
| 事件流 | `on_event` / `wrap_run_event_stream` |
| 模型请求 | `before_model_request` / `prepare_model_request` / `after_model_request` / `wrap_model_request` / `on_model_request_error` |
| 工具校验 | `before_tool_validate` / `after_tool_validate` / `wrap_tool_validate` / `on_tool_validate_error` |
| 工具执行 | `before_tool_execute` / `after_tool_execute` / `wrap_tool_execute` / `on_tool_execute_error` |
| 输出校验 | `before_output_validate` / `after_output_validate` / `wrap_output_validate` / `on_output_validate_error` |
| 输出处理 | `before_output_process` / `after_output_process` / `wrap_output_process` / `on_output_process_error` |
| 工具准备 | `prepare_tools` / `prepare_output_tools` |

注意模型请求阶段有**两个**前置钩子，语义不同：`before_model_request` 每个步执行一次，在回退或重试可能换模型之前；`prepare_model_request` 每次尝试执行一次，拿到的是真正服务的那个模型。需要按模型能力做决策的逻辑（上下文压缩、窗口适配、供应商翻译）必须放后者，否则回退后写的逻辑对不上模型。

**能力不是方法集合，而是可组合对象。** 基类提供一组 `_has_*` 谓词（`_has_wrap_model_request`、`_has_prepare_model_request`、`has_on_event` 等）让运行期能判断「这个能力到底实现了哪些钩子」，从而跳过无谓的包装层。这是性能考量：如果把每个阶段都无条件包一层，即使能力什么都没做，也会有调用开销。

**身份与合并。** 每个能力可以声明一个 `id`；合并时同 `id` 的能力被视为「同一个能力的多次出现」，由该能力自己实现的 `combine` 决定怎么合并（默认合并集合并以后值优先，冲突无法调和时抛 `UserError`）。没有 `id` 的能力不允许重复出现——因为框架无法判断两个无身份实例是「同一个」还是「两个不同的」。

**`Capability` 的三处细节。** 它的 `get_toolset()` 在没有任何工具集时**返回那个空的函数工具集对象本身**而不是 `None`，源码注释给了理由：Agent 只会接线一次，返回 `None` 会让构造之后通过 `@tool` 注册的工具消失。它给用户传入的 `toolsets` 里的裸可调用对象生成 `id` 时用「该能力 id + 在参数里的位置」（`{id}_{index}`），而不是运行时组合顺序，这样后注册的工具不会移动前面工具的 id（持久执行按 id 识别叶子工具集）。

## 关键取舍

**源码事实：** 九组阶段 × 最多五个钩子，构成一张很大的契约面。这不是随意膨胀：每个阶段都需要「前置、后置、包裹、出错」四种介入方式，缺任何一种都会逼用户去写包裹钩子模拟它。

**源码事实：** `before_model_request` 与 `prepare_model_request` 是两个不同粒度的钩子，前者每步一次、后者每次尝试一次。两者并存是因为「按模型决策」必须在回退之后。

**源码事实：** 基类用 `_has_*` 谓词做能力探测，运行期据此跳过未实现的包装。这让「注册很多能力」不必然带来每阶段一层调用。

**源码事实：** `Capability.get_toolset()` 没工具时返回空工具集对象而非 `None`，是为支持构造后注册工具。这是「对象引用即接线」的直接后果。

**架构推论：** 用一套统一契约承载所有横切行为，换来的是「行为可枚举」——想知道一次运行里有哪些东西会介入，看能力列表即可，不必翻 Agent 的字段、也不必有子类。代价是契约本身很大，实现一个能力时容易只关注自己那一个钩子而忽略与其他阶段的交互（例如缓存与重试、回退与模型请求准备）。

**架构推论：** 能力可组合且顺序决定结果，意味着调试时**顺序是语义的一部分**。源码注释明确「两个不同能力贡献同一设置时不告警，因为顺序决定结果，而顺序是用户选的」。这把责任交给了调用方。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/capabilities/abstract.py` | `AbstractCapability` | 能力基类与全部生命周期钩子 |
| `pydantic_ai_slim/pydantic_ai/capabilities/abstract.py` | `CapabilityOrdering` | 能力排序声明 |
| `pydantic_ai_slim/pydantic_ai/capabilities/capability.py` | `Capability` | 最简能力：打包指令、工具、工具集 |
| `pydantic_ai_slim/pydantic_ai/capabilities/capability.py` | `Capability.get_toolset` | 空工具集返回对象而非 None 的理由 |
| `pydantic_ai_slim/pydantic_ai/capabilities/capability.py` | `Capability._toolset_id` | 按参数位置生成稳定工具集 id |

## 参考

- [pydantic-ai 源码：capabilities/abstract.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/capabilities/abstract.py)
- [Pydantic AI 文档：能力概览](https://ai.pydantic.dev/capabilities/)
- [Pydantic AI 文档：自定义能力](https://ai.pydantic.dev/capabilities/custom/)
- [Pydantic AI 文档：Agent 与依赖注入](https://ai.pydantic.dev/agent/)

## 与主核 / AI 的关系

能力系统与主核里的「查询钩子」是同一个问题的两种解法。主核要在查询执行的各个阶段插入统计、审计、限流，若用继承会形成子类爆炸，若用回调表则回调之间无法包裹；pydantic-ai 选择的是「一组成对的前置/后置/包裹/错误钩子 + 可组合对象」，这比裸回调表强的地方在于包裹钩子能决定是否调用下游，从而可以短路、重试、替换。

**与 langgraph 与 autogen 的分野**在这里最清楚：langgraph 让用户画图（控制流可编程），autogen 让用户组团队（消息可路由），pydantic-ai 让用户挂能力（执行期横切可组合）。三者对「可扩展性」的答案不同，代价也不同：能力系统不改控制流，所以行为最可预测，但也不能通过能力引入新的循环结构。

对工程的直接建议：评估一个 agent 框架时，看它「如何插入横切逻辑」比看它「支持多少模型」更有预测力。前者决定了你后面加审计、加配额、加脱敏要付多少成本。
