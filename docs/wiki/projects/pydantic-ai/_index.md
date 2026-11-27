---
project: pydantic-ai
page_type: overview
display_name: PydanticAI
repo: https://github.com/pydantic/pydantic-ai
license: MIT
language: Python
stars: 21331
stars_as_of: 2026-11-22
refs:
  - path: pydantic_ai_slim/pydantic_ai/agent/__init__.py
    symbols: [Agent]
  - path: pydantic_ai_slim/pydantic_ai/capabilities/abstract.py
    symbols: [AbstractCapability]
  - path: pydantic_ai_slim/pydantic_ai/tool_manager.py
    symbols: [ToolManager]
  - path: pydantic_ai_slim/pydantic_ai/models/_abstract.py
    symbols: [AbstractModel]
  - path: pydantic_ai_slim/pydantic_ai/run.py
    symbols: [AgentRun, AgentRunResult]
  - path: pydantic_graph/pydantic_graph/graph_builder.py
    symbols: [Graph, GraphRun]
---
# PydanticAI

## 一句话职责

Pydantic AI 是一个「以类型安全为核心、以能力系统为扩展机制」的 Python agent 框架：Agent 是声明式的，执行循环是一张用 pydantic-graph 搭的图，所有横切行为都通过可组合的 Capability 挂进生命周期。

## 机制是什么

这个框架有三层结构，理解它们的分工就理解了它：

**图引擎（pydantic-graph）** 是一个独立的、类型注解驱动的图库。节点是 `BaseNode` 子类，返回下一个节点或 `End`。它提供分叉、汇合、决策、步节点等原语。agent 循环本身**就是**用这套原语搭的：`UserPromptNode` → `ModelRequestNode` → `CallToolsNode` → `SetFinalResult`。

**能力系统（Capability）** 是这个框架区别于其他 agent SDK 的核心。所有横切行为——缓存、思考链、模型回退、工具集挂载、内容过滤、持久执行——都不是 Agent 的字段也不是子类重写，而是同一个 `AbstractCapability` 契约的不同实现，在一张固定的九组钩子清单上插入逻辑。

**Agent 与运行** 是面向用户的门面：`Agent` 声明模型、指令、工具、输出类型与能力列表；`Agent.run()` 把这次调用变成一次图运行，返回可迭代、可取消、可流式的 `AgentRun`。

此外还有一批独立机制：**工具集**用装饰器式组合（过滤、加前缀、重命名、延迟加载）而不是修改基类；**输出**有文本/工具/原生/提示词四种模式；**模型层**把「抽象接口 + 提供方 + 模型档案 + 包装器」分开；**持久执行**把运行拆成可重放的操作单元。

本文库按经审阅的机制单元组织：**10 个机制领域、共 38 篇机制页**，每个机制页对应一个可独立理解、可独立验证的机制单元，而不是一个源文件。

## 怎么实现的

下表的 10 个领域按下面的顺序排列：A Agent 定义与运行入口 → B 图执行引擎 → C 能力系统 → D 工具与工具集 → E 工具执行与延迟审批 → F 输出与结构化 → G 模型层 → H 消息与历史 → I MCP 与工作区 → J 持久执行与可观测。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：`pydantic_ai_slim/pydantic_ai/`（SDK 主体）与 `pydantic_graph/pydantic_graph/`（图引擎）。`pydantic_evals`（评估框架）面向测试与数据集而非运行时机制，本轮不纳入。

## 机制导航（38 篇 / 10 组）

### A-Agent 定义与运行入口（4 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-agent-definition.md`](a01-agent-definition.md) | Agent 定义与抽象基类 |
| [`a02-agent-spec.md`](a02-agent-spec.md) | Agent 规格与可序列化定义 |
| [`a03-run-and-result.md`](a03-run-and-result.md) | 运行对象与结果契约 |
| [`a04-run-context.md`](a04-run-context.md) | 运行上下文与依赖注入 |

### B-图执行引擎（5 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-graph-builder.md`](b01-graph-builder.md) | 图构建与运行入口 |
| [`b02-node-contract.md`](b02-node-contract.md) | 节点契约与运行上下文 |
| [`b03-decision-routing.md`](b03-decision-routing.md) | 决策节点与分支路由 |
| [`b04-fork-join.md`](b04-fork-join.md) | 并行分叉与汇合 |
| [`b05-step-and-stream.md`](b05-step-and-stream.md) | 步节点与流式节点 |

### C-能力系统（4 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-capability-contract.md`](c01-capability-contract.md) | 能力契约与生命周期挂载点 |
| [`c02-capability-composition.md`](c02-capability-composition.md) | 能力组合与排序 |
| [`c03-builtin-capabilities.md`](c03-builtin-capabilities.md) | 内置能力清单 |
| [`c04-capability-hooks.md`](c04-capability-hooks.md) | 能力钩子与超时 |

### D-工具与工具集（4 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-tool-definition.md`](d01-tool-definition.md) | 工具定义与 schema |
| [`d02-toolsets.md`](d02-toolsets.md) | 工具集抽象与装饰型工具集 |
| [`d03-tool-manager.md`](d03-tool-manager.md) | 工具管理器与工具裁决 |
| [`d04-tool-validation.md`](d04-tool-validation.md) | 工具参数校验与重试 |

### E-工具执行与延迟审批（3 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-tool-execution.md`](e01-tool-execution.md) | 工具执行与并发控制 |
| [`e02-deferred-tools.md`](e02-deferred-tools.md) | 延迟工具调用与审批 |
| [`e03-tool-search.md`](e03-tool-search.md) | 工具搜索与延迟发现 |

### F-输出与结构化（3 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-output-modes.md`](f01-output-modes.md) | 四种输出模式 |
| [`f02-output-schema.md`](f02-output-schema.md) | 输出 schema 与自动推断 |
| [`f03-output-validation.md`](f03-output-validation.md) | 输出校验与不完整输出处理 |

### G-模型层（4 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-model-abstract.md`](g01-model-abstract.md) | 模型抽象与请求参数 |
| [`g02-model-fallback.md`](g02-model-fallback.md) | 模型回退链 |
| [`g03-model-providers.md`](g03-model-providers.md) | 提供方与模型档案 |
| [`g04-model-wrappers.md`](g04-model-wrappers.md) | 模型包装：插桩与并发限流 |

### H-消息与历史（4 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-message-model.md`](h01-message-model.md) | 消息部件与多模态内容 |
| [`h02-history-processing.md`](h02-history-processing.md) | 历史处理与重写 |
| [`h03-framing.md`](h03-framing.md) | 内容帧包裹与提示注入防护 |
| [`h04-conversation-store.md`](h04-conversation-store.md) | 会话存储与冲突检测 |

### I-MCP 与工作区（3 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-mcp-integration.md`](i01-mcp-integration.md) | MCP 接入与能力映射 |
| [`i02-workspaces.md`](i02-workspaces.md) | 工作区后端与权限分层 |
| [`i03-ssrf-guard.md`](i03-ssrf-guard.md) | 网络抓取的地址校验 |

### J-持久执行与可观测（4 篇）

| 页文件 | 标题 |
|---|---|
| [`j01-durable-execution.md`](j01-durable-execution.md) | 持久执行与可重放单元 |
| [`j02-instrumentation.md`](j02-instrumentation.md) | 插桩与敏感内容策略 |
| [`j03-usage-accounting.md`](j03-usage-accounting.md) | 用量归属与成本 |
| [`j04-cancellation.md`](j04-cancellation.md) | 取消传播与截止时间 |

## 关键取舍

**源码事实：** agent 循环不是手写的 while 循环，而是用 pydantic-graph 搭的一张图（`UserPromptNode` → `ModelRequestNode` → `CallToolsNode` → `SetFinalResult`）。这让「循环」和「图编排」共用一套执行与追踪设施。

**源码事实：** 能力用九组「前置 / 后置 / 包裹 / 错误」四联钩子承载横切逻辑，其中模型请求阶段有 `before_model_request`（每步一次）与 `prepare_model_request`（每次尝试一次）两个前置钩子，因为按模型能力决策必须在回退之后。

**源码事实：** 基类用 `_has_*` 谓词探测能力实现了哪些钩子，据此跳过无谓的包装层。注册很多能力不必然带来每阶段一层调用。

**源码事实：** 框架给模型提示里的不可信内容加 `<pyai:...>` 帧标记，让模型能区分「数据」与「指令」。这是提示注入防护的一层，但它不阻止模型仍然听指令，只降低混淆概率。

**架构推论：** 「图引擎 + 能力系统」的组合让扩展有两个方向：改控制流（画图，但不改 agent 内部循环）和改横切（挂能力，但不改控制流）。两条路都不允许直接改 agent 循环本身，这是它与其他框架最根本的分野。

**架构推论：** 能力顺序是语义的一部分——源码注释明确「两个能力贡献同一设置时不告警，因为顺序决定结果，而顺序是用户选的」。这把「优先级」的责任显式交给了调用方，也意味着调试时顺序必须被记录。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/agent/__init__.py` | `Agent` | 面向用户的智能体门面 |
| `pydantic_ai_slim/pydantic_ai/capabilities/abstract.py` | `AbstractCapability` | 能力基类与九组生命周期钩子 |
| `pydantic_ai_slim/pydantic_ai/capabilities/combined.py` | `CombinedCapability` | 多能力编织与顺序 |
| `pydantic_ai_slim/pydantic_ai/tool_manager.py` | `ToolManager` | 请求前把多工具集裁成可用集合 |
| `pydantic_ai_slim/pydantic_ai/toolsets/abstract.py` | `AbstractToolset` | 工具集抽象 |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `OutputSchema` | 输出 schema 与校验 |
| `pydantic_ai_slim/pydantic_ai/models/_abstract.py` | `AbstractModel` | 模型抽象接口 |
| `pydantic_ai_slim/pydantic_ai/framing.py` | `wrap` | 不可信内容的帧标记 |
| `pydantic_ai_slim/pydantic_ai/durable_exec/_base.py` | `_RestrictedRunContext` | 持久执行的可重放单元 |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `Graph` | 图构建与运行 |

## 参考

- [Pydantic AI 源码仓库](https://github.com/pydantic/pydantic-ai)
- [Pydantic AI 文档：Agent](https://ai.pydantic.dev/agent/)
- [Pydantic AI 文档：能力](https://ai.pydantic.dev/capabilities/)
- [Pydantic AI 文档：图](https://ai.pydantic.dev/graph/)
- [Pydantic AI 文档：工具](https://ai.pydantic.dev/tools/)

## 与主核 / AI 的关系

Pydantic AI 与主核（多模态数据库内核）在「抽象层次划分」上可以对照得很具体：

**图引擎 vs 执行器。** pydantic-graph 提供的是「节点 + 边 + 状态」的最小执行骨架，主核的执行器提供的是「算子 + 数据流 + 元组」。两者都不关心业务语义，只保证推进顺序、分叉汇合与错误传播。差别是图引擎用类型注解在编译期校验节点返回类型，主核用 C 的类型系统和运行期断言。

**能力系统 vs 查询钩子。** 主核要在解析、计划、执行各阶段插入统计与审计；pydantic-ai 用「前置 / 后置 / 包裹 / 错误」四联钩子做同一件事。包裹钩子能决定是否调用下游，这是裸回调表做不到的——它使「短路、重试、替换」都能用同一种形态表达。

**帧标记 vs 输入校验。** 帧标记承认了一个前提：当指令和数据都进同一个文本通道，就无法靠语法区分。主核不存在这个问题，因为 SQL 与数据是结构性分开的。这条差异说明：LLM 应用的输入校验只能做到「降低混淆概率」，做不到「结构性隔离」。

在 2A 波次的六个 Agent 框架里，pydantic-ai 的定位最接近「库」而非「框架」：它不规定你必须怎么组织智能体，只给一套强原语和一张大而清晰的扩展面。选它的典型理由是「需要类型安全 + 需要精细的横切控制 + 不想被绑定某种编排风格」。
