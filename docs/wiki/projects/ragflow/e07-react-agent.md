---
project: ragflow
feature: react-agent
title: ReAct Agent 与图形态
page_type: feature
schema_version: 1
refs:
  - path: internal/harness/core/react_agent.go
    symbols: [ReActAgent, ReActConfig, NewReActAgent]
  - path: internal/harness/core/react_graph.go
    symbols: [ReActGraph, NewReActGraph, ReActGraphState]
  - path: internal/harness/core/agent_loop.go
    symbols: [AgentLoop, NewAgentLoop]
---

# ReAct Agent 与图形态

## 一句话职责

同一个 `ReActAgent` 对象可以落到三种执行形态上——无工具的裸模型调用、纯 for 循环的 ReAct 循环、以及把每轮迭代摊成图节点的 Pregel 图；再加上外层可选的 `AgentLoop` 推式轮次循环，构成「一次 Run 到底走哪条路」的选择题。

## 机制是什么

`ReActAgent` 表面上是一个 Agent 对象，实际上它**只在第一次 Run 时决定自己用哪条路径，然后冻结**。这个选择发生在 `buildRunFunc` 里，靠 `sync.Once` 保证只选一次，判据只有两条：

| 条件 | 选中形态 |
|---|---|
| 没有任何工具（配置、ToolsConfig、中间件贡献三者皆空） | 无工具单次调用 |
| 有工具且 `GraphReAct` 为真 | 图形态（Pregel 超步） |
| 有工具且 `GraphReAct` 为假（默认） | for 循环形态 |

三者的差别不是实现细节，而是**能力边界**：

- **无工具形态**只有一次模型调用，没有循环、没有工具、没有迭代上限。它是「纯对话」的快路径，存在的理由是避免为一个不需要工具的 Agent 付循环与工具节点的开销。
- **for 循环形态**是完整的 ReAct：模型生成 → 抽工具调用 → 执行 → 结果回填 → 再来一轮，上限是 `MaxIterations`。它把全部状态放在一个 Go 结构体里，跑在同一个 goroutine 内。
- **图形态**把同一套逻辑拆成四个图节点，交给上一页（e04）讲过的超步循环。它换来的是**每个节点边界自动存检查点、工具执行前可中断、可从检查点恢复**——因为这些正是图引擎已有的能力，Agent 只要把自己表达成图就能白拿。

第四层是 `AgentLoop`，它**不是**第四种 ReAct 形态，而是包在 Agent 外面的轮次调度器：推式缓冲、抢占、停止、轮次级检查点。它和上面三者的关系是「驱动者与被驱动者」，而不是并列选项。

## 怎么实现的

**构造期的隐式补全。** `NewReActAgent` 除设默认名与描述外只做一件事：若 `ToolsConfig` 为空而 `Tools` 列表非空，就把 `Tools` 与 `ReturnDirectly` 打包成一个 `ToolsNodeConfig` 塞回去。这一步让后续代码只认 `ToolsConfig` 一个来源；代价是同一个字段集合有了两个入口，`prepareExecContext` 里必须显式处理「两者都有值时不要重复收集」——注释写明了这个陷阱，判断逻辑是「ToolsConfig 有工具就只从它取，否则退回 `Tools`」。

**执行上下文的收集只做一次。** `prepareExecContext` 在 `once.Do` 里被调用，收集三样东西：指令（空则用内置默认系统提示）、合并后的 `ReturnDirectly`（配置 + 中间件贡献）、以及工具元信息（配置工具 + 中间件贡献工具的 `ToolInfo`）。中间件贡献的工具由 `collectContributorTools` 一类函数从 `Middlewares` 里逐个问出来——这就是 e09 要讲的 `ContributeTools` 钩子的消费方。

**冻结机制。** 两个字段配合：`sync.Once` 管「只初始化一次」，`atomic` 的 `frozen` 标志管「对外宣告已冻结」。`Run` 与 `Resume` 都在执行体结束后调 `freeze()`，`IsFrozen` 供外部查询。注意冻结发生的时机是**执行结束**而不是执行开始——所以并发的两次 `Run` 都可能进入未冻结状态，冻结只是事后标记，不是互斥锁。

**for 循环形态的骨架。** 循环条件是 `state.RemainingIterations > 0`，每轮递减。每轮的顺序是固定的：跑 `BeforeModelRewrite` 链 → 可选 `StateModifier` → 构造模型输入 → 调模型 → 追加到消息列表 → 跑 `AfterModelRewrite` 链 → 抽工具调用 → 有调用就执行。**两个终止条件**：模型没吐工具调用（`completed = true` 并 break）、或工具执行返回了「直接退出」动作。循环耗尽后另发一条「超出最大迭代」的错误事件——注意这是事件而不是 error 返回，所以调用方只能从事件流里看到它。

**图形态的四个节点。** `prepare_input` 只做一次，跑 `BeforeAgent` 链并把指令写回状态；`model_generate` 递减迭代计数、**清空工具执行缓存**、跑前后两条模型改写链、调模型、记录「这轮有没有工具调用」；`execute_tools` 逐个执行待办工具并把结果写进缓存；`check_done` 跑 `AfterAgent` 链并写输出。

边结构是：`prepare_input` → `model_generate` →（条件边：无工具调用或迭代耗尽走 `check_done`，否则走 `execute_tools`）→ `execute_tools` 回到 `model_generate`。`check_done` 到终点。

这里的条件边判断读的是**状态里的布尔标记**而不是重新解析最后一条消息——`model_generate` 在返回前把 `HasToolCall` 置好，条件边只看它。这样图引擎不必理解消息结构，判断逻辑被压进节点内部。

**图形态的两个硬限制。** 其一，它只支持 `*schema.Message`；泛型参数是 `*schema.AgenticMessage` 时会**回退到 for 循环形态**，代码里先做类型断言，不匹配就调另一条 run 函数。其二，递归上限被设成 `MaxIterations * 2`——注释写明「每轮迭代是 2 个节点，所以放宽」，即把「轮数」翻译成「超步数」。

**工具执行缓存是图形态独有的续跑机制。** `ReActGraphState` 里有一个 `ToolExecutedCache`（工具调用 ID → 结果字符串）。生命周期是三段：`model_generate` 开头清空、`execute_tools` 执行成功一个就写一条、全部成功后置空。中断时不置空而是保留——于是这个 map 随检查点落盘，恢复时 `execute_tools` 先过滤掉缓存里已有的调用，只跑剩下的。**这是「一轮里多个工具，中断在中间」的唯一解**：不缓存就得重跑已完成的工具，而工具可能有副作用。

**恢复数据的作用面很窄。** `ReActAgentResumeData` 只有一个字段 `HistoryModifier`，语义是「恢复时改写消息历史」。它只在 `Resume` 且 `info.WasInterrupted` 与 `info.InterruptState` 类型都成立时才被取出；更窄的是，在 for 循环形态里它只在**消息列表首元素能被断言成 `Message`** 时才生效——代码先按 `*schema.Message` 分支判断，其他泛型形态直接跳过。所以这个钩子对 `AgenticMessage` 实际上是无效的。

**版本常量是预留位。** 文件里有 `CheckpointDataVersion` 类型与 `CheckpointDataV1` 常量，还有一个 `preprocessCheckpointData` 函数——后者是 `return data`，什么都不做。也就是说「检查点数据需要向前兼容迁移」这件事已经被识别并留了接口，但迁移逻辑尚未落地。

**`AgentLoop` 是另一层。** 它持有 `turnBuffer`（推式缓冲）、停止控制器、抢占控制器、以及一个 `checkPointRunnerBytes`（Agent 运行器的序列化快照）。`Run` 只启动 goroutine 就返回（非阻塞），`Wait` 阻塞取结果。`NewAgentLoop` 对两个必填依赖做了 `panic` 而不是返回错误：`GenInput` 与 `PrepareAgent` 为空直接 panic——这是一个「配置错误必须在启动前炸掉」的取向，与库内其他返回 error 的构造风格不一致。

**轮次级检查点的三档条件。** `shouldSaveCheckpoint` 的判断链是：没配 store 或没给检查点 ID 就不存；显式跳过就不存；**没有任何待存内容**（运行器字节与中断项都空）就不存。三者都通过后，才看该不该存：要么「停止已提交且退出确由停止引起」，要么「发生了业务中断」。正常跑完不存检查点——因为没什么可续的。

`cleanup` 随后把结果装进 `AgentLoopState`：退出原因、未处理项、被中断项、停止原因、检查点是否尝试过及其错误、以及一个**一次性取走迟到项**的闭包（内部用 `sync.Once` 保证只取一次）。若这次是「加载了旧检查点却没有新检查点要存」，则顺手把旧检查点删掉。

## 关键取舍

**源码事实：** 形态选择在首次 `Run` 时由 `sync.Once` 决定并缓存进 `a.run`，之后永不变更。判据是「有没有工具」与 `GraphReAct` 两个条件，三者互斥。

**源码事实：** 无工具形态只调一次模型，没有循环。`MaxIterations` 对它完全无效。

**源码事实：** 图形态只支持 `*schema.Message`，遇到 `*schema.AgenticMessage` 会回退到 for 循环形态，静默地失去检查点与中断能力。

**源码事实：** 图形态的递归上限是 `MaxIterations * 2`，把「轮」换算成「超步」。

**源码事实：** `ToolExecutedCache` 在每轮模型生成前清空、成功后在工具执行末尾置空、中断时保留并随检查点落盘，恢复时据此跳过已完成调用。

**源码事实：** `ReActAgentResumeData.HistoryModifier` 只在首消息可断言为 `Message` 时生效，对 `AgenticMessage` 无效。

**源码事实：** `CheckpointDataVersion` / `CheckpointDataV1` 已定义，但 `preprocessCheckpointData` 是恒等函数，迁移逻辑未实现。

**源码事实：** `NewAgentLoop` 对缺失的 `GenInput` / `PrepareAgent` 直接 panic，不返回 error。

**架构推论：** 三种形态共存是「同一语义、三档成本」的典型工程折中。无工具形态省掉循环开销，for 循环省掉图引擎开销，图形态付超步与序列化成本换可恢复性。**把选择做成一次性决策而不是每次运行再判断**，意味着一个 Agent 实例的能力在生命周期内是固定的——配置改了 `GraphReAct` 也不会让它换路，除非新建实例。这条约束容易被误用：改配置看着像生效了，实际跑的还是旧路径。

**架构推论：** 图形态与 for 循环形态是**两份独立实现**（各自维护一遍中间件调用顺序、模型输入构造、终止判定）。这带来一个真实的维护风险：任何中间件语义变更都必须改两处。它换来的是两条路径都能按自己的约束优化——for 循环能自由使用 goroutine 与共享状态，图节点必须是无状态函数。

**架构推论：** `ToolExecutedCache` 的存在暴露了「一轮内多工具 + 中断」这个组合的固有难点。中断的语义是「从节点开头重跑」（见 e06），而 `execute_tools` 节点里可能有多个工具调用，重跑就会重做已完成的工具。缓存是唯一不改变中断语义的解法：**把「已完成」这个事实也变成状态的一部分**，让它跟状态一起被持久化。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/harness/core/react_agent.go` | `ReActAgent` | Agent 对象，持有配置、once、冻结标志与缓存的 run 函数 |
| `internal/harness/core/react_agent.go` | `ReActConfig` | 全部配置项，含 `GraphReAct` 三兄弟 |
| `internal/harness/core/react_agent.go` | `NewReActAgent` | 构造并做 `Tools` 到 `ToolsConfig` 的隐式补全 |
| `internal/harness/core/react_agent.go` | `buildRunFunc` | 用 `sync.Once` 一次性选定执行形态 |
| `internal/harness/core/react_agent.go` | `prepareExecContext` | 收集指令、直接返回表与工具元信息 |
| `internal/harness/core/react_agent.go` | `buildNoToolsRunFunc` | 无工具形态：单次模型调用 |
| `internal/harness/core/react_agent.go` | `buildGraphReActRunFunc` | 图形态入口，含非 Message 类型的回退 |
| `internal/harness/core/react_agent.go` | `ReActAgentResumeData` | 恢复数据，仅一个历史改写钩子 |
| `internal/harness/core/react_agent.go` | `CheckpointDataVersion` | 检查点数据版本类型 |
| `internal/harness/core/react_agent.go` | `preprocessCheckpointData` | 版本迁移预留位，当前恒等返回 |
| `internal/harness/core/react_loop.go` | `buildReActRunFunc` | for 循环形态的完整循环体 |
| `internal/harness/core/react_graph.go` | `NewReActGraph` | 建图：四节点、条件边、编译选项 |
| `internal/harness/core/react_graph.go` | `ReActGraphState` | 图状态，含工具执行缓存 |
| `internal/harness/core/react_graph.go` | `ReActGraph.Invoke` | 同步跑图，input 为空即恢复路径 |
| `internal/harness/core/react_graph.go` | `ReActGraph.Resume` | 以空输入触发检查点恢复 |
| `internal/harness/core/agent_loop.go` | `AgentLoop` | 推式轮次循环，含停止与抢占控制器 |
| `internal/harness/core/agent_loop.go` | `NewAgentLoop` | 构造，缺必填依赖即 panic |
| `internal/harness/core/agent_loop.go` | `shouldSaveCheckpoint` | 轮次级检查点的三档判定 |
| `internal/harness/core/agent_loop.go` | `cleanup` | 装结果、存或删检查点、关闭通道 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：core/react_agent.go](https://github.com/infiniflow/ragflow/blob/main/internal/harness/core/react_agent.go)
- [ragflow 源码：core/react_graph.go](https://github.com/infiniflow/ragflow/blob/main/internal/harness/core/react_graph.go)
- [ReAct 论文（Yao et al., 2022）](https://arxiv.org/abs/2210.03629)

## 与主核 / AI 的关系

**「一次决策、终身生效」与主核里查询计划的编译时绑定同构。** 数据库把 SQL 编译成执行计划后，即使表结构变了也不会自动换计划，除非重新解析。这里把 Agent 编译成某个执行形态后也不再重判。两者的好处是运行期没有分支判断开销，坏处是**配置漂移不生效**——用户改了配置却发现行为不变，是这类设计最常见的困惑来源。

**两份实现 vs 一份抽象。** 图形态与 for 循环形态各写一遍相同的中间件序列，这与主核里「同一语义在 OLTP 路径与批量路径各实现一遍」是同一个取舍：**为各自的最优形态放弃代码复用**。判据应该是一样的——只有当两条路径的性能特征确实不同、且各自的优化无法统一时才值得。这里成立的：for 循环要在一个 goroutine 里积累状态，图节点必须纯函数化（因为会被重跑）。

**把「已完成」也当成状态。** `ToolExecutedCache` 是一个很小但很典型的设计：中断语义要求「重跑整节点」，而节点内有多个副作用操作，唯一的出路是**把进度外化成可持久化的状态**。主核在崩溃恢复时靠日志记录已提交的事务来实现同一件事——**恢复的正确性取决于「进度」被记录得多细**。这里粒度是「单个工具调用」，主核是「单个事务」，都受制于各自能观察到的最小单元。

对 agent 工程最实用的四条。其一：**改 `GraphReAct` 不生效是正常的**——形态在首次 Run 时固化，换形态必须新建 Agent 实例。其二：**图形态只对 `*schema.Message` 可用**，泛型换成 `AgenticMessage` 会静默回退到 for 循环，检查点与中断随之消失，且不会有任何提示。其三：**中断恢复的正确性依赖工具执行缓存**，自定义工具时不要让同一次调用在 `execute_tools` 之外的路径产生副作用，否则重跑仍会重复。其四：**`AgentLoop` 是外层驱动而非第四种形态**，不要把它和三种 ReAct 形态并列理解。
