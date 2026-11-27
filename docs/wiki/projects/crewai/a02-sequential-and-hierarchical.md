---
project: crewai
feature: sequential-and-hierarchical
title: 顺序与层级两种流程
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/crew.py
    symbols: [kickoff, _arun_sequential_process, _arun_hierarchical_process]
  - path: lib/crewai/src/crewai/process.py
    symbols: [Process]
---

# 顺序与层级两种流程

## 一句话职责

kickoff 是所有团队执行的唯一入口，它按流程类型分派到两条路径：顺序流程直接遍历任务，层级流程先造一个管理智能体再遍历。

## 机制是什么

`Process` 是个 `str` 枚举，目前只有两个值：`sequential` 与 `hierarchical`（源码里留了一行 `consensual` 的 TODO）。这个枚举不是装饰性的分类标签，它决定了执行时**由谁决定下一个动作**。

顺序流程里，任务序列本身就是执行顺序，没有中间决策者。层级流程则在遍历之前先插入一个管理智能体，由它通过委派工具决定某个任务该交给谁做。两条路径共用同一套任务循环（见 a03），差异只在于进入循环前的准备。

## 怎么实现的

**入口的四段准备。** `kickoff` 在分派前做了四件事：先 `apply_checkpoint` 尝试从检查点恢复（若恢复成功则直接转交给恢复出来的对象，不再走后面的流程）；再调 `get_env_context()` 读取环境；然后处理流式模式——`stream=True` 时会临时把 `self.stream` 置假并递归调用自身，把真正的执行塞进一个 chunk 生成器，使调用方拿到的是可迭代的流对象而不是结果；最后用 OpenTelemetry 的 baggage 挂上团队上下文，并 `begin_execution` 开一条追踪。

这个「流式递归」写法值得单独指出：它靠一个闭包改状态再复原，避免把执行逻辑写成两份。代价是 `self.stream` 在生成器存活期间是可变共享状态，同一实例并发 kickoff 会互相干扰。

**分派。** 准备完成后是一次朴素的两分支判断：`Process.sequential` 走 `_run_sequential_process`，`Process.hierarchical` 走 `_run_hierarchical_process`，其它值抛 `NotImplementedError`。异常路径上还接了一个针对追踪授权的专门捕获分支，说明追踪失败被当成需要区别对待的错误类型。

**顺序流程。** `_run_sequential_process` 只有一行：`return self._execute_tasks(self.tasks)`。异步版本 `_arun_sequential_process` 同理，转给 `_aexecute_tasks`。所以「顺序」这个语义**不在这个方法里**，它体现在任务循环没有管理智能体介入。

**层级流程。** `_run_hierarchical_process` 先调 `_create_manager_agent()` 再调同一个 `_execute_tasks`。也就是说两种流程的任务循环是同一份代码，层级性来自「多了一个管理智能体」这个事实。

**管理智能体的两种来源。** `_create_manager_agent` 分两支：用户提供了 `manager_agent` 就直接用，但强制把 `allow_delegation` 置真，并检查它是否带着非委派类工具——带了就记警告并抛异常清空，理由是管理者的职责是委派而不是自己干活。没提供就现造一个：从国际化文案取角色、目标、背景三个字段，工具集用 `AgentTools(agents=self.agents).tools()` 生成的委派工具，`allow_delegation=True`，模型用 `manager_llm`。

两支最后都会做同一件事：把 `manager.crew` 指向当前团队；若团队开了缓存，再把缓存处理器交给管理者。后一段源码注释解释了原因——管理者是在智能体校验循环之外创建的，不会在构造时拿到团队的缓存处理器，必须在返回前补上，否则 `cache=True` 的团队里管理者的工具调用不会被去重。

## 关键取舍

**源码事实：** 两种流程共用同一个任务循环，差异只在循环前是否创建管理智能体。这意味着「层级」不是一套独立的调度器，而是「多一个会委派的智能体」的配置差异。

**源码事实：** 用户自带的管理智能体若携带普通工具，会被强制清空并抛异常，而不是只警告。这是把「管理者不该自己执行工具」当成硬约束。

**源码事实：** 管理智能体的角色文案来自国际化资源（`get_i18n`），不是硬编码字符串。这让默认管理者的提示词可翻译，也让不同语言环境下行为一致。

**源码事实：** 流式模式通过在闭包里反复翻转 `self.stream` 实现，不是两条独立代码路径。省掉了重复实现，但引入了实例级可变状态。

**架构推论：** 把「流程类型」做成枚举而不是布尔开关，使新增流程（例如源码 TODO 里的共识流程）只需加一个分支，而不必改调用方签名。当前的 `NotImplementedError` 分支就是这个扩展点的显式占位。

**架构推论：** 层级流程把决策权交给一个 LLM 驱动的智能体，因此任务顺序不再可静态预测。这对可重放性有直接影响：顺序流程的重放只需按索引跳过已完成任务，层级流程的重放还依赖管理者当时的决策历史。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/crew.py` | `kickoff` | 唯一执行入口：检查点恢复、流式包装、追踪开启、按流程分派 |
| `lib/crewai/src/crewai/crew.py` | `_run_sequential_process` | 顺序流程，直接转给任务循环 |
| `lib/crewai/src/crewai/crew.py` | `_run_hierarchical_process` | 层级流程，先建管理智能体再进任务循环 |
| `lib/crewai/src/crewai/crew.py` | `_arun_sequential_process` | 顺序流程的异步版本 |
| `lib/crewai/src/crewai/crew.py` | `_create_manager_agent` | 管理智能体的两种来源与工具约束 |
| `lib/crewai/src/crewai/process.py` | `Process` | 流程类型枚举，含未实现的扩展占位 |

## 参考

- [CrewAI 源码：crew.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/crew.py)
- [CrewAI 文档：流程概念](https://docs.crewai.com/en/concepts/processes)
- [CrewAI 文档：层级流程与管理者](https://docs.crewai.com/en/concepts/processes#hierarchical-process)

## 与主核 / AI 的关系

「同一份执行循环 + 不同的决策者」这个结构，与主核里「同一个执行器 + 不同的查询计划」是同构的：执行器的代码不因计划而变，变的是它拿到的指令序列。差别在于主核的计划由优化器静态确定并可复现，这里的层级流程由模型在运行时决定，因此天然不可复现。

对 AI 工程的实际意义在于：选顺序还是层级，本质是选「可预测性」还是「灵活性」。顺序流程的任务与执行顺序完全静态，适合流水线式产出；层级流程允许动态分工，但代价是同一输入两次运行可能产生不同的任务顺序与不同的中间结果，排障时必须同时看管理者当时的决策轨迹。这也是为什么 CrewAI 在这套流程之上还单独做了事件记录与检查点——没有它们，层级流程的失败几乎无法复盘。
