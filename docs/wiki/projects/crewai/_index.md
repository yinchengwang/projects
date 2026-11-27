---
project: crewai
page_type: overview
display_name: CrewAI
repo: https://github.com/crewAIInc/crewAI
license: MIT
language: Python
stars: 61187
stars_as_of: 2026-11-23
refs:
  - path: lib/crewai/src/crewai/crew.py
    symbols: [Crew, kickoff]
  - path: lib/crewai/src/crewai/task.py
    symbols: [Task, execute_sync]
  - path: lib/crewai/src/crewai/agent/core.py
    symbols: [Agent]
  - path: lib/crewai/src/crewai/flow/runtime/__init__.py
    symbols: [Flow]
  - path: lib/crewai/src/crewai/tools/tool_usage.py
    symbols: [ToolUsage]
---
# CrewAI

## 一句话职责

CrewAI 用「团队（Crew）+ 任务（Task）+ 智能体（Agent）」三层组织多智能体协作，并用一套独立的 Flow 引擎承载确定性编排。

## 机制是什么

CrewAI 有两条并行的执行模型。**团队模型**是角色分工式的：一组智能体各带角色、目标、背景，任务按顺序（或由管理者委派）依次执行，任务之间靠上下文传递产出。**Flow 模型**是事件驱动式的：方法用装饰器声明触发条件，运行时按事件推进，状态在方法间流动，可持久化、可暂停等人工反馈。

两者的定位不同：团队模型适合「让模型自己分工」，Flow 模型适合「让开发者精确控制每一步」。同一个项目可以只用其一，也可以让 Flow 的某个步骤调用一支团队。

在此之上还有三套横切机制，它们是 CrewAI 区别于其他 agent 框架的地方：**工具系统**带使用次数配额、重复调用检测与失败分类策略；**事件与钩子系统**带因果上下文栈和可查询的事件记录图；**状态与检查点机制**把运行时状态按事件触发条件落盘，支持重放与恢复。

本文库按经审阅的机制单元组织：**10 个机制领域、共 41 篇机制页**，每个机制页对应一个可独立理解、可独立验证的机制单元，而不是一个源文件。

## 怎么实现的

下表的 10 个领域按下面的顺序排列：A 团队与流程 → B 任务与条件执行 → C 智能体执行循环 → D Flow 编排 → E 记忆系统 → F 工具系统 → G MCP 与技能 → H 事件与钩子 → I 模型层与安全 → J 运行时状态与检查点。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：`lib/crewai/src/crewai/` 主包。同仓的 `crewai-core`（认证提供方）、`crewai-tools`（上百个第三方工具适配）、`crewai-files`（文件上传与格式化）、`cli`（脚手架与部署）各有独立包边界，属外部集成与工具链层，不在本清单内。

## 机制导航（41 篇 / 10 组）

### A-团队与流程（4 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-crew-object.md`](a01-crew-object.md) | Crew 对象与配置校验 |
| [`a02-sequential-and-hierarchical.md`](a02-sequential-and-hierarchical.md) | 顺序与层级两种流程 |
| [`a03-task-loop-and-async.md`](a03-task-loop-and-async.md) | 任务循环与异步任务排队 |
| [`a04-replay-and-checkpoint.md`](a04-replay-and-checkpoint.md) | 重放与执行起点定位 |

### B-任务与条件执行（4 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-task-execution-core.md`](b01-task-execution-core.md) | Task 执行内核 |
| [`b02-conditional-task.md`](b02-conditional-task.md) | 条件任务的跳过判定 |
| [`b03-guardrail.md`](b03-guardrail.md) | 护栏与重试上限 |
| [`b04-task-output-handling.md`](b04-task-output-handling.md) | 任务输出与结构化导出 |

### C-智能体执行循环（4 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-agent-definition.md`](c01-agent-definition.md) | Agent 定义与属性校验 |
| [`c02-delegation.md`](c02-delegation.md) | 委派工具注入与层级协作 |
| [`c03-agent-executor-loop.md`](c03-agent-executor-loop.md) | 执行器循环与工具调用 |
| [`c04-context-summarization.md`](c04-context-summarization.md) | 上下文压缩与空回复重试 |

### D-Flow 编排（6 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-flow-decorators.md`](d01-flow-decorators.md) | Flow 装饰器与触发条件 |
| [`d02-flow-definition.md`](d02-flow-definition.md) | Flow 定义契约与状态模型 |
| [`d03-flow-runtime.md`](d03-flow-runtime.md) | Flow 运行时与监听器执行 |
| [`d04-flow-expressions.md`](d04-flow-expressions.md) | Flow 表达式与模板渲染 |
| [`d05-human-feedback.md`](d05-human-feedback.md) | 人工反馈与恢复 |
| [`d06-flow-persistence.md`](d06-flow-persistence.md) | Flow 持久化与恢复 |

### E-记忆系统（6 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-unified-memory.md`](e01-unified-memory.md) | 统一记忆入口 |
| [`e02-memory-scope.md`](e02-memory-scope.md) | 记忆作用域与隔离 |
| [`e03-recall-flow.md`](e03-recall-flow.md) | 记忆检索流程 |
| [`e04-encoding-flow.md`](e04-encoding-flow.md) | 记忆编码与去重 |
| [`e05-knowledge.md`](e05-knowledge.md) | 知识库与来源管理 |
| [`e06-rag-factory.md`](e06-rag-factory.md) | RAG 存储工厂 |

### F-工具系统（5 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-base-tool.md`](f01-base-tool.md) | 工具基类与参数模式 |
| [`f02-structured-tool.md`](f02-structured-tool.md) | 结构化工具与描述生成 |
| [`f03-tool-usage.md`](f03-tool-usage.md) | 工具调用编排与重复检测 |
| [`f04-tool-calling-schema.md`](f04-tool-calling-schema.md) | 函数调用式工具协议 |
| [`f05-tool-failure.md`](f05-tool-failure.md) | 工具失败分类与策略 |

### G-MCP 与技能（3 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-mcp-client.md`](g01-mcp-client.md) | MCP 客户端与连接生命周期 |
| [`g02-mcp-native-tool.md`](g02-mcp-native-tool.md) | MCP 工具的原生暴露 |
| [`g03-skills.md`](g03-skills.md) | 技能发现与加载 |

### H-事件与钩子（3 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-event-bus.md`](h01-event-bus.md) | 事件总线与处理器登记 |
| [`h02-event-context.md`](h02-event-context.md) | 事件上下文与因果链 |
| [`h03-hooks.md`](h03-hooks.md) | 工具与模型调用钩子 |

### I-模型层与安全（3 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-base-llm.md`](i01-base-llm.md) | 模型基类与调用包装 |
| [`i02-llm-factory.md`](i02-llm-factory.md) | 模型创建与环境回退 |
| [`i03-security-fingerprint.md`](i03-security-fingerprint.md) | 安全配置与指纹 |

### J-运行时状态与检查点（4 篇）

| 页文件 | 标题 |
|---|---|
| [`j01-runtime-state.md`](j01-runtime-state.md) | 运行时状态与序列化 |
| [`j02-checkpoint-triggers.md`](j02-checkpoint-triggers.md) | 检查点配置与触发 |
| [`j03-event-record-graph.md`](j03-event-record-graph.md) | 事件记录图与因果重建 |
| [`j04-execution-context.md`](j04-execution-context.md) | 执行上下文与追踪标识 |

## 关键取舍

**源码事实：** 团队模型与 Flow 模型是两套独立执行引擎，不共享调度器。团队用任务循环推进，Flow 用事件触发推进；两者都能调智能体，但状态模型、持久化方式、暂停语义各不相同。

**源码事实：** 顺序流程与层级流程共用同一个任务循环，层级性只体现为「循环前多造一个管理智能体」。因此流程类型不是两套代码，而是一个配置差异。

**源码事实：** 工具调用有使用次数配额与重复调用检测，且失败被分成原因类别并配策略。这是把「工具滥用」当成一等问题来治理，而不是靠提示词约束。

**源码事实：** 事件带因果上下文（父事件、触发来源、作用域栈），并组织成可查询的记录图。这让「一次失败到底由哪条链路引起」可以被代码回答，而不是靠日志拼凑。

**架构推论：** 两套执行引擎意味着两套心智模型。选团队模型就是接受「顺序不可完全预测」（层级流程由模型决定分工），选 Flow 模型就是接受「要自己写控制流」。CrewAI 没有把它们合并，因为合并会同时失去两者各自的确定性优势。

**架构推论：** 工具配额、失败策略、检查点触发条件这些机制都要求「跨调用的状态」，所以 CrewAI 必须自带运行时状态、事件总线和持久化层。这解释了为什么它的包体量明显大于纯编排框架：它试图把生产环境的可运维性做进框架内部。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `lib/crewai/src/crewai/crew.py` | `Crew` | 团队对象：配置校验、流程分派、任务循环 |
| `lib/crewai/src/crewai/crew.py` | `kickoff` | 唯一执行入口：检查点恢复、流式包装、追踪 |
| `lib/crewai/src/crewai/task.py` | `Task` | 任务执行内核：输入插值、护栏、输出归一 |
| `lib/crewai/src/crewai/agent/core.py` | `Agent` | 智能体定义与执行器装配 |
| `lib/crewai/src/crewai/utilities/agent_utils.py` | `get_llm_response` | 执行器循环的模型调用入口 |
| `lib/crewai/src/crewai/flow/runtime/__init__.py` | `Flow` | Flow 运行时：事件推进、监听器执行、状态传递 |
| `lib/crewai/src/crewai/memory/unified_memory.py` | `Memory` | 记忆门面：记住、回忆、忘记、更新 |
| `lib/crewai/src/crewai/tools/tool_usage.py` | `ToolUsage` | 工具调用编排：配额、重复检测、结果格式化 |
| `lib/crewai/src/crewai/events/event_bus.py` | `CrewAIEventsBus` | 事件总线：登记、分发、运行时作用域 |
| `lib/crewai/src/crewai/state/runtime.py` | `RuntimeState` | 运行时状态：序列化、谱系、检查点 |

## 参考

- [CrewAI 源码仓库](https://github.com/crewAIInc/crewAI)
- [CrewAI 文档：核心概念](https://docs.crewai.com/en/concepts/agents)
- [CrewAI 文档：流程与编排](https://docs.crewai.com/en/concepts/processes)
- [CrewAI 文档：Flow](https://docs.crewai.com/en/concepts/flows)

## 与主核 / AI 的关系

CrewAI 与主核（多模态数据库内核）在三个地方可以对照：

**两套执行引擎 vs 两套存储引擎。** CrewAI 同时维护任务循环与 Flow 事件引擎，主核同时维护按行存储与多模态索引链路。共同点是「不同负载用不同执行模型」，共同的风险是「两套状态模型之间的边界最难维护」——CrewAI 用运行时状态与事件记录把边界显式化，主核用分层依赖门禁强制边界。

**工具配额 vs 资源配额。** 工具使用次数上限与主核的连接数/内存配额是同一类治理：把「滥用」变成可计数的资源，而不是靠约定。区别在于主核的配额失效表现为拒绝服务，这里的配额失效表现为智能体拿到一个错误结果需要自己纠偏。

**检查点触发 vs WAL 写入时机。** 检查点按事件条件落盘，WAL 按事务提交落盘。两者都要回答「多久持久化一次」这个成本问题：太频繁则开销大，太稀疏则丢失的工作量大。

对 AI 工程的直接结论是：CrewAI 的价值不在「让多智能体跑起来」，而在它承认了生产环境需要的那三套横切能力——工具治理、因果可观测、可恢复状态。评估一个 agent 框架时，先看这三项是否具备，再看编排写法是否好看。
