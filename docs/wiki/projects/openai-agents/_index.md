---
project: openai-agents
page_type: overview
display_name: OpenAI Agents SDK
repo: https://github.com/openai/openai-agents-python
license: MIT
language: Python
stars: 30825
stars_as_of: 2026-11-23
refs:
  - path: src/agents/run.py
    symbols: [Runner, AgentRunner]
  - path: src/agents/agent.py
    symbols: [Agent, AgentBase]
  - path: src/agents/run_state.py
    symbols: [RunState]
  - path: src/agents/tool.py
    symbols: [FunctionTool, ToolOutputText]
  - path: src/agents/guardrail.py
    symbols: [InputGuardrail, OutputGuardrail]
  - path: src/agents/handoffs/__init__.py
    symbols: [Handoff]
---
# OpenAI Agents SDK

## 一句话职责

OpenAI 官方的 Agents SDK：`Agent` 是纯数据定义，`Runner` 是唯一执行入口，围绕「一次模型调用 + 工具/交接执行」的循环，配护栏、会话、MCP、追踪四套横切能力。

## 机制是什么

这套 SDK 的结构异常清晰：**没有智能体自己的控制流**。`Agent` 只是一个数据类（名字、指令、工具、交接、输出类型、护栏），它不持有任何执行状态。所有执行都由 `Runner` 发起，运行状态集中在 `RunState`，运行结果集中在 `RunResult`。

单轮循环的逻辑是固定的：调用模型 → 若产生最终输出则结束 → 若有交接则换智能体继续 → 否则执行工具调用并再来一轮。这个循环不接受自定义（不像 langgraph 可以画任意图），它的可扩展性来自四个挂载点：**工具**（模型可以调用的函数或远程服务）、**交接**（把控制权交给另一个智能体）、**护栏**（在输入、输出、工具三个层面拦截）、**钩子**（生命周期回调）。

四套横切能力各司其职：**会话**（`Session` 把对话历史持久化，使多轮不必自己拼输入）、**MCP**（把外部 MCP 服务器的工具接进来）、**追踪**（跨度树记录每一次模型调用与工具执行）、**流式**（把循环过程中的事件逐条吐出）。

本文库按经审阅的机制单元组织：**10 个机制领域、共 34 篇机制页**，每个机制页对应一个可独立理解、可独立验证的机制单元，而不是一个源文件。

## 怎么实现的

下表的 10 个领域按下面的顺序排列：A Agent 定义与运行入口 → B 单轮执行循环 → C 工具系统 → D 护栏 → E 交接 → F 模型层 → G 会话与记忆 → H MCP 集成 → I 追踪与可观测 → J 流式与结果。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：`src/agents/` 主包。`realtime/`（实时语音）、`voice/`（语音流水线）、`sandbox/`（远程沙箱）三块各自是独立子系统且体量大，本轮不纳入，留作后续增量。

## 机制导航（34 篇 / 10 组）

### A-Agent 定义与运行入口（4 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-agent-definition.md`](a01-agent-definition.md) | Agent 定义与配置合并 |
| [`a02-runner-entry.md`](a02-runner-entry.md) | Runner 唯一入口与默认运行器 |
| [`a03-run-context.md`](a03-run-context.md) | 运行上下文与用途约束 |
| [`a04-run-state.md`](a04-run-state.md) | 运行状态与序列化恢复 |

### B-单轮执行循环（4 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-turn-loop.md`](b01-turn-loop.md) | 单轮循环与最大轮次 |
| [`b02-run-config.md`](b02-run-config.md) | 运行配置与敏感数据开关 |
| [`b03-model-retry.md`](b03-model-retry.md) | 模型调用重试与裁决合并 |
| [`b04-lifecycle-hooks.md`](b04-lifecycle-hooks.md) | 生命周期钩子 |

### C-工具系统（4 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-tool-types.md`](c01-tool-types.md) | 工具类型体系与输出形态 |
| [`c02-function-tool-schema.md`](c02-function-tool-schema.md) | 函数工具与 schema 生成 |
| [`c03-tool-invocation.md`](c03-tool-invocation.md) | 工具调用与参数解析 |
| [`c04-tool-approvals.md`](c04-tool-approvals.md) | 工具审批与拒绝路径 |

### D-护栏（3 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-input-output-guardrails.md`](d01-input-output-guardrails.md) | 输入输出护栏与触发语义 |
| [`d02-tool-guardrails.md`](d02-tool-guardrails.md) | 工具级护栏与行为策略 |
| [`d03-guardrail-gating.md`](d03-guardrail-gating.md) | 护栏门控与并行任务取消 |

### E-交接（4 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-handoff-model.md`](e01-handoff-model.md) | 交接对象与输入改造 |
| [`e02-handoff-execution.md`](e02-handoff-execution.md) | 交接在循环中的识别与切换 |
| [`e03-handoff-filters.md`](e03-handoff-filters.md) | 交接过滤器与提示扩展 |
| [`e04-agent-as-tool.md`](e04-agent-as-tool.md) | 智能体作为工具 |

### F-模型层（4 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-model-interface.md`](f01-model-interface.md) | 模型接口与追踪模式 |
| [`f02-responses-model.md`](f02-responses-model.md) | Responses 模型实现 |
| [`f03-chatcmpl-model.md`](f03-chatcmpl-model.md) | Chat Completions 实现与转换 |
| [`f04-multi-provider.md`](f04-multi-provider.md) | 多提供方映射 |

### G-会话与记忆（3 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-session-interface.md`](g01-session-interface.md) | 会话接口与压缩感知 |
| [`g02-sqlite-session.md`](g02-sqlite-session.md) | SQLite 会话实现 |
| [`g03-server-conversation-tracking.md`](g03-server-conversation-tracking.md) | 服务端会话追踪 |

### H-MCP 集成（2 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-mcp-server-connection.md`](h01-mcp-server-connection.md) | MCP 服务器连接与工具快照 |
| [`h02-mcp-manager.md`](h02-mcp-manager.md) | 多服务器管理与工作进程 |

### I-追踪与可观测（3 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-tracing-spans.md`](i01-tracing-spans.md) | 追踪跨度与错误记录 |
| [`i02-tracing-processors.md`](i02-tracing-processors.md) | 追踪处理器与导出 |
| [`i03-tracing-scope.md`](i03-tracing-scope.md) | 追踪作用域与上下文传播 |

### J-流式与结果（3 篇）

| 页文件 | 标题 |
|---|---|
| [`j01-streaming-events.md`](j01-streaming-events.md) | 流式事件与三层事件源 |
| [`j02-result-objects.md`](j02-result-objects.md) | 结果对象与项模型 |
| [`j03-structured-output.md`](j03-structured-output.md) | 结构化输出与校验 |

## 关键取舍

**源码事实：** `Runner` 的三个类方法参数列表逐字相同，且都是逐个参数显式转发给模块级单例运行器，不用 `**kwargs` 透传。新增参数要改 4 处，但换来签名级类型检查。

**源码事实：** 输入接受三种形态：字符串、响应输入项列表、`RunState`。「从上次中断继续」和「从零开始」共用同一个入口，不是两套 API。

**源码事实：** 脱敏异常在离开 `Runner.run` 前会把作用域内 12 个局部变量逐个置 `None`，并摘掉 traceback 帧。理由是异常对象被外传时，traceback 帧的 `f_locals` 仍持有原始数据引用。

**源码事实：** 护栏分三层：输入/输出护栏拦「模型说了什么」，工具级护栏拦「工具收到什么参数、返回什么」，护栏门控负责在输入护栏判定期间挡住工具并行执行并在触发时取消并发任务。

**架构推论：** 固定循环 + 四个挂载点，是这个 SDK 与其他 agent 框架最大的分野。langgraph 让用户画图，autogen 让用户组团队，crewai 让用户定义 SOP，而这里不给用户改控制流的机会——代价是复杂编排做不了，收益是行为可预测、可测试、可追踪。

**架构推论：** 把执行状态集中到 `RunState` 且可完整序列化，使「暂停后跨进程恢复」成为一等能力。这也解释了为什么它的会话层（`Session`）与运行状态层（`RunState`）是两个独立概念：前者管对话历史，后者管一次运行的全部中间态。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `src/agents/run.py` | `Runner` | 唯一执行入口：异步、同步、流式三个类方法 |
| `src/agents/agent.py` | `Agent` | 智能体定义：指令、工具、交接、输出类型、护栏 |
| `src/agents/run_state.py` | `RunState` | 一次运行的可续跑状态与序列化 |
| `src/agents/run_context.py` | `RunContextWrapper` | 运行上下文与用量统计 |
| `src/agents/tool.py` | `FunctionTool` | 函数工具定义与输出类型 |
| `src/agents/guardrail.py` | `InputGuardrail` | 输入输出护栏定义 |
| `src/agents/handoffs/__init__.py` | `Handoff` | 交接对象与输入改造 |
| `src/agents/models/interface.py` | `Model` | 模型抽象接口 |
| `src/agents/memory/session.py` | `Session` | 会话持久化契约 |
| `src/agents/tracing/spans.py` | `Span` | 追踪跨度 |

## 参考

- [openai-agents-python 源码仓库](https://github.com/openai/openai-agents-python)
- [Agents SDK 文档：运行智能体](https://openai.github.io/openai-agents-python/running_agents/)
- [Agents SDK 文档：工具](https://openai.github.io/openai-agents-python/tools/)
- [Agents SDK 文档：护栏](https://openai.github.io/openai-agents-python/guardrails/)
- [Agents SDK 文档：追踪](https://openai.github.io/openai-agents-python/tracing/)

## 与主核 / AI 的关系

这个 SDK 与主核（多模态数据库内核）在「固定执行结构 + 可插拔挂载点」这一点上高度同构。主核的查询执行是固定流水线（解析 → 计划 → 执行 → 返回），可插拔的是索引、算子、存储；这里的循环固定，可插拔的是工具、交接、护栏、钩子。两者都通过「不让用户改主干」换取行为可分析。

**运行状态与 WAL 的对照。** `RunState` 记录一次运行的完整中间态并可序列化恢复，WAL 记录页面的变更意图并可重放。差别在于 `RunState` 是快照式的（存当前状态），WAL 是增量式的（存变更序列）。快照简单但体积大，增量省空间但要保证可重放——这与主核里「检查点 + 增量日志」的组合是同一种权衡的两个方向。

**三层护栏与访问控制。** 输入护栏对应「请求准入」、工具护栏对应「资源访问控制」、护栏门控对应「并发协调」，正好是主核里权限检查、存储层校验、锁管理三层的类比。差别在于这里的判定由 LLM 参与，因此「误判」是常态而非异常，框架必须提供「拒绝后如何继续」的路径（拒绝输出、抛异常、放行三种行为）。

对 agent 工程的判断价值：这个 SDK 是「约束换确定性」的典型。它不适合需要复杂动态编排的场景，但适合「一个智能体加一堆工具、要求可追踪可审计可恢复」的生产场景。选它之前先确认你的业务能不能塞进那个固定循环里。
