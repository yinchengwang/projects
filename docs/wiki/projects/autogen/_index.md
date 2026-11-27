---
project: autogen
page_type: overview
display_name: AutoGen
repo: https://github.com/microsoft/autogen
license: CC-BY-4.0
language: Python
stars: 61964
stars_as_of: 2026-11-23
refs:
  - path: python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py
    symbols: [SingleThreadedAgentRuntime, RunContext]
  - path: python/packages/autogen-core/src/autogen_core/_base_agent.py
    symbols: [BaseAgent]
  - path: python/packages/autogen-core/src/autogen_core/_routed_agent.py
    symbols: [RoutedAgent]
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/base/_chat_agent.py
    symbols: [ChatAgent]
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_base_group_chat.py
    symbols: [BaseGroupChat]
---
# AutoGen

## 一句话职责

AutoGen 是一套「以 actor 运行时为底座、以团队编排为上层」的多智能体框架：消息经统一队列串行分派，智能体按消息类型分派处理，团队层在此之上编排选人与终止。

## 机制是什么

AutoGen 的分层很清楚：**core** 只提供 actor 运行时、消息协议、订阅路由与组件模型；**agentchat** 在其上定义单智能体契约（ChatAgent）与团队编排（群聊、选人、终止、状态）；**ext** 提供与外部系统（模型供应商、代码沙箱、浏览器）的适配。

关键设计是「一切经队列」。点对点请求、主题广播、以及请求的回复都是信封，运行时逐条取出处理。这让消息顺序确定、可追踪、可拦截，也让「谁在什么时候收到什么」不再是并发时序的副产物。上层团队编排正是建立这条不变式之上：管理器只发「请这个参与者发言」，不直接调用参与者。

本文库按经审阅的机制单元组织：**9 个机制领域、共 39 篇机制页**，每个机制页对应一个可独立理解、可独立验证的机制单元，而不是一个源文件。

## 怎么实现的

下表的 9 个领域按下面的顺序排列：A actor 运行时 → B 消息路由与订阅 → C 智能体基类与消息处理 → D 组件与序列化 → E 模型上下文与记忆 → F 工具与代码执行 → G 单智能体实现 → H 团队编排 → I 终止条件和状态恢复。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：`python/packages/` 下的 `autogen-core` 与 `autogen-agentchat` 两个包。`autogen-ext` 主体是第三方集成（各模型供应商、各沙箱后端），按机制归类属于外部适配层，不在本清单内；`autogen-studio`、`pyautogen`、`agbench`、`dotnet/` 同样不在本清单内。

## 机制导航（39 篇 / 9 组）

### A-actor 运行时（4 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-single-threaded-runtime.md`](a01-single-threaded-runtime.md) | 单线程 actor 运行时与消息队列 |
| [`a02-runtime-interface.md`](a02-runtime-interface.md) | 运行时抽象与智能体注册 |
| [`a03-run-context.md`](a03-run-context.md) | 运行上下文与停止条件 |
| [`a04-agent-id-and-type.md`](a04-agent-id-and-type.md) | 智能体标识与类型约束 |

### B-消息路由与订阅（4 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-routed-agent.md`](b01-routed-agent.md) | 按消息类型分发的 RoutedAgent |
| [`b02-subscription-matching.md`](b02-subscription-matching.md) | 订阅模型与主题匹配 |
| [`b03-subscription-manager.md`](b03-subscription-manager.md) | 订阅登记与接收者解析 |
| [`b04-closure-agent.md`](b04-closure-agent.md) | 闭包智能体与轻量注册 |

### C-智能体基类与消息处理（4 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-base-agent-contract.md`](c01-base-agent-contract.md) | BaseAgent 契约与消息入口 |
| [`c02-agent-instantiation-context.md`](c02-agent-instantiation-context.md) | 实例化上下文与订阅延迟绑定 |
| [`c03-agent-proxy.md`](c03-agent-proxy.md) | 代理与跨智能体调用 |
| [`c04-intervention-handler.md`](c04-intervention-handler.md) | 消息拦截与改写 |

### D-组件与序列化（4 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-message-serialization.md`](d01-message-serialization.md) | 消息序列化注册表 |
| [`d02-component-config.md`](d02-component-config.md) | 组件声明与配置往返 |
| [`d03-function-introspection.md`](d03-function-introspection.md) | 函数签名到工具 schema |
| [`d04-cache-store.md`](d04-cache-store.md) | 缓存存储抽象与内存实现 |

### E-模型上下文与记忆（4 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-chat-completion-context.md`](e01-chat-completion-context.md) | 模型上下文基类与状态存取 |
| [`e02-context-window-strategies.md`](e02-context-window-strategies.md) | 上下文窗口的四种裁剪策略 |
| [`e03-model-client-contract.md`](e03-model-client-contract.md) | 模型客户端与能力声明 |
| [`e04-memory-abstraction.md`](e04-memory-abstraction.md) | 记忆抽象与列表实现 |

### F-工具与代码执行（5 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-tool-schema.md`](f01-tool-schema.md) | 工具 schema 与状态化调用 |
| [`f02-function-tool.md`](f02-function-tool.md) | 函数工具与参数校验 |
| [`f03-workbench.md`](f03-workbench.md) | 工作台与工具集合调度 |
| [`f04-tool-agent.md`](f04-tool-agent.md) | 工具智能体与调用循环 |
| [`f05-code-executor.md`](f05-code-executor.md) | 代码执行器与依赖声明 |

### G-单智能体实现（5 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-chat-agent-contract.md`](g01-chat-agent-contract.md) | ChatAgent 契约与响应对象 |
| [`g02-assistant-agent.md`](g02-assistant-agent.md) | AssistantAgent 的模型调用与工具循环 |
| [`g03-code-executor-agent.md`](g03-code-executor-agent.md) | 代码执行智能体与审批 |
| [`g04-user-proxy-and-filters.md`](g04-user-proxy-and-filters.md) | 用户代理与消息过滤智能体 |
| [`g05-society-of-mind.md`](g05-society-of-mind.md) | 心智社会智能体（团队作为单体） |

### H-团队编排（6 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-group-chat-base.md`](h01-group-chat-base.md) | 群聊团队基类与参与者装配 |
| [`h02-chat-agent-container.md`](h02-chat-agent-container.md) | 参与者容器与事件桥接 |
| [`h03-group-chat-manager.md`](h03-group-chat-manager.md) | 群聊管理器与轮次推进 |
| [`h04-round-robin-and-selector.md`](h04-round-robin-and-selector.md) | 轮询与模型选人两种策略 |
| [`h05-swarm-handoff.md`](h05-swarm-handoff.md) | Swarm 交接式选人 |
| [`h06-graph-flow.md`](h06-graph-flow.md) | 图式编排与激活条件 |

### I-终止条件和状态恢复（3 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-termination-conditions.md`](i01-termination-conditions.md) | 终止条件与布尔组合 |
| [`i02-message-model.md`](i02-message-model.md) | 消息类型与事件模型 |
| [`i03-team-and-agent-state.md`](i03-team-and-agent-state.md) | 状态对象与保存恢复 |

## 关键取舍

**源码事实：** 所有消息进一条队列，单条 asyncio 任务串行推进。只有主题广播的多个接收者用 `gather` 并发执行，其余路径严格串行。这让顺序确定，代价是全运行时吞吐受一条队列限制，任何同步阻塞调用都会造成队头阻塞。

**源码事实：** 点对点请求的异常只写回 future，不重抛；主题广播的异常记进 `_background_exception`，会在下一轮循环抛出并终止运行。两种错误传导方式刻意不同，因为前者有等待方、后者没有。

**源码事实：** `save_state` 只保存已实例化智能体的状态，源码注释明确写明**不保存订阅状态**。所以从状态恢复不等于恢复拓扑，订阅必须靠重新注册或重新实例化重建。

**源码事实：** 组件的配置往返与消息序列化是两套独立机制。前者管「对象如何由声明重建」，后者管「消息如何跨进程传输」。

**架构推论：** core 与 agentchat 的边界是「机制 vs 编排」。core 不知道什么是「团队」，只知道消息与订阅；团队编排完全用 core 的能力组合而成——参与者被包成运行时智能体，管理器通过发布请求来驱动它们。这条边界让新编排形态可以只加 agentchat 层，不动运行时。

**架构推论：** 单线程运行时的确定性是「可调试」与「可重放」的前提，但不是「高性能」的方案。要横向扩展，正确做法是分片多个运行时实例，而不是把一条队列变成多线程。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py` | `SingleThreadedAgentRuntime` | 单线程 actor 运行时：队列、分派、状态 |
| `python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py` | `RunContext` | 运行上下文：空闲即停、条件停止 |
| `python/packages/autogen-core/src/autogen_core/_base_agent.py` | `BaseAgent` | 智能体基类契约 |
| `python/packages/autogen-core/src/autogen_core/_routed_agent.py` | `RoutedAgent` | 按消息类型分派的智能体 |
| `python/packages/autogen-core/src/autogen_core/_subscription.py` | `Subscription` | 订阅定义与匹配 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/base/_chat_agent.py` | `ChatAgent` | 单智能体对外契约 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_base_group_chat.py` | `BaseGroupChat` | 群聊团队装配与运行 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_base_group_chat_manager.py` | `BaseGroupChatManager` | 轮次推进与终止判定 |

## 参考

- [AutoGen 源码仓库](https://github.com/microsoft/autogen)
- [AutoGen 文档：Core 架构概念](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/core-concepts/architecture.html)
- [AutoGen 文档：AgentChat 团队](https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/teams.html)
- [AutoGen 文档：状态与持久化](https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/state.html)

## 与主核 / AI 的关系

AutoGen 与主核（多模态数据库内核）有三处可直接对照：

**消息队列 vs WAL。** 两者都是「先记下意图、再按序执行」。差别在于主核的 WAL 要处理崩溃恢复与页面版本校验，而这里的队列是纯内存的，进程退出即丢失，恢复靠上层 `save_state`。

**订阅路由 vs 索引查找。** 订阅把「消息类型/主题」映成接收者集合，主核把「键值」映成元组位置。两者都是把「查询意图」解析成「具体目标」，都不承担目标本身的执行语义。

**智能体状态 vs 表状态。** 智能体的 `save_state` 与团队状态对象，等价于主核里「把某个对象的全部持久字段序列化」。区别是这里的状态是用户自定义的 dataclass/pydantic，没有 schema 校验，也就没有版本演进保护。

对 AI 工程最实际的结论是：AutoGen 把「多智能体协作」拆成可独立验证的三层——运行时负责顺序，路由负责可达性，团队层负责策略。要排查一个 agent 行为异常，先确认它是否收到消息（路由层）、再确认处理函数选了哪个（分派层）、最后才看策略逻辑。这个分层顺序比盯着 prompt 猜有效得多。
