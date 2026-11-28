---
project: ragflow
feature: pregel-engine
title: Pregel 执行引擎
page_type: feature
schema_version: 1
refs:
  - path: internal/harness/graph/pregel/engine.go
    symbols: [Engine, NewEngine, ExecuteResult]
  - path: internal/harness/graph/pregel/optimized.go
    symbols: [PregelOptimizedEngine, PriorityTaskQueue]
---

# Pregel 执行引擎

## 一句话职责

把状态图跑成一个**超步循环**：每一轮确定要执行的节点、并发执行它们、把结果写回通道、按持久化策略存检查点，再进入下一轮，直到没有节点可执行或触到递归上限。

## 机制是什么

Pregel（BSP，整体同步并行）模型的核心是**一道栅栏**：同一超步内的节点全部并发执行，但它们的输出要到下一个超步才对别人可见。这样做的代价是**失去细粒度并行**（本来可以边算边传的，现在必须等齐），收益是**确定性**——同一份输入、同一个超步顺序，就得到同一个结果，跟节点实际完成先后无关。

对 Agent 图来说这个收益是必须的。Agent 的行为依赖上下文状态，如果节点 A 写完状态后节点 B 立刻读到、而另一次运行里顺序反过来，同一个图的两次运行就可能给出不同答案。**在一个用 LLM 的系统里，不确定性已经来自模型，图执行层再引入一份不确定性会让调试完全不可能。**

ragflow 的 `harness/graph` 是 LangGraph 的 Go 移植（术语、选项名、语义都对得上：`StateGraph`、`Checkpointer`、`Interrupt`、`Command`、`Send`、`StreamMode`、`Durability`）。这一点值得先说明，因为读代码时会不断遇到「这和 LangGraph 的某个概念对应」——这是**有意的对齐**，不是巧合。

## 怎么实现的

**`Engine` 是一个可复用的执行器，不是一次运行的句柄。** `NewEngine(g, opts...)` 绑定一张图与一组选项（检查点器、中断点、递归上限、并发上限、重试策略、缓存、回调、后台执行器），而 `Run` 每次调用都**自己建一套运行期状态**。这从 `Run` 开头那段重置看得很清楚：

```go
// Reset per-execution engine state.
// Without this, reusing the same Engine across multiple RunSync calls
// causes checkpoint maps and channel versions to accumulate indefinitely,
// leading to unbounded memory growth (soak tests exposed this).
e.currentCheckpoint = nil
e.channelVersions = make(map[string]int)
e.versionsSeen = make(map[string]map[string]int)
e.deferredCheckpoints = nil
```

注释写明了不重置的后果：**跨多次运行累积、内存无界增长**，而且是**浸泡测试（soak test）发现的**——这类缺陷单次运行永远看不到，只有反复跑才暴露。这是一个很实在的工程细节：把「可复用对象持有运行期状态」这个常见陷阱用注释锁住。

**`Run` 的并发结构是三个 goroutine 加一道排空。** 主 goroutine 跑超步循环、一个转发 goroutine 把流事件搬进 `outputCh`、还有一个由 `NewAsyncPipeline` 起的任务执行池。收尾顺序写在 `defer` 里，注释解释了为什么这么排：先读 `errCh` 拿到退出错误（此时 `errCh` 还开着）→ 派发 `RunEnd` 回调 → 把错误**放回** `errCh` 给调用方 → 关流管理器 → **等转发 goroutine 退出** → 最后才 `close(outputCh)`。

那条 `fwWg.Wait()` 是必需的：不等它就直接 `close(outputCh)`，会与「转发 goroutine 正在往 `outputCh` 发」构成数据竞争（close 与 send 并发）。注释写了这层理由。**Go 里「关闭通道」是最容易写错的一步**，因为它必须由发送方在确认没有其他发送者之后才做。

`Run` 的文档还写了一条调用方义务：「**必须读到 outputCh 被关闭为止，否则会泄漏 goroutine**」。这是 channel-based API 的固有契约——接收方提前退出，发送方就永久阻塞在 send 上。它把这条写在 API 文档而不是靠调用方自觉，是对的。

**超步循环本体在 521 行那个 `for {}` 里，四件事按固定顺序做：**

1. **每个超步先查取消与递归上限。** `if step >= e.recursionLimit` 抛 `GraphRecursionError`。默认上限是 **25**（`NewEngine` 里 `recursionLimit: 25`）。这个数字对 Agent 图是个关键参数：太小会让正常的「多轮工具调用」被误判为死循环，太大则让真正的死循环烧完全部预算才停。25 是个偏保守的选择。
2. **派发 `StepStart` 回调并发射检查点事件**，然后**确定本轮要跑哪些任务**（`prepareNextTasks`）。
3. **`len(tasks) == 0` 即终止。** 这是唯一的正常退出条件——「没有节点可执行」而不是「某个标志被设置」。Pregel 模型的自然终止形态。
4. **执行前先查中断点。** 命中中断就先存检查点、发射中断事件、返回——所以**中断不是异常路径，是带持久化的一等退出**。

**两种任务选择模式是这套实现与「教科书 Pregel」最大的差异。** `prepareNextTasksWithMode` 的注释写清了语义：

> In `AllPredecessor` (DAG) mode, a node is triggered only when ALL of its incoming edges' source nodes have completed. In `AnyPredecessor` (Pregel/BSP) mode (default), a node is triggered when any predecessor completes. `AllPredecessor` does not support cycles.

这是**两种图语义塞进一个引擎**：默认的 `AnyPredecessor` 是 Pregel 语义（任一前驱完成就触发，支持环，因而支持 Agent 的自循环），`AllPredecessor` 是 DAG 语义（所有前驱完成才触发，**不支持环**）。DAG 模式的实现是 O(n) 全扫描（`prepareNextTasksDAG` 扫所有节点看哪些的前驱都完成了），注释承认「O(n) per call but correct for fan-in patterns」——**为正确性接受复杂度**，因为 fan-in（多路汇合）恰恰是 `AnyPredecessor` 语义下容易出错的地方。

**持久化有三种档位，用 switch 分派而不是统一处理。** `types.DurabilitySync` 阻塞保存、`DurabilityAsync` 起 goroutine 保存（失败只记日志不影响执行）、`DurabilityExit` **只把检查点累积在内存里，退出时统一存**（`e.deferCheckpoint`）。三种档位的语义差别是「能容忍多大的崩溃损失」：

- `Sync`：每个超步都落盘，崩溃最多丢一个超步；
- `Async`：不阻塞下一步，但「保存中崩溃」会让检查点缺失；
- `Exit`：只有正常结束才落盘，中途崩溃**全部丢失**——省的是 I/O，赌的是「不会崩」。

`Async` 分支的注释「Log async error but don't fail execution」是个明确的取舍：**保存失败降级成日志**。这与 `Sync` 分支「保存失败就终止执行」相反。理由能理解（异步保存的意义就是不阻塞主流程），但它意味着**用 Async 档位时检查点可能静默缺失**。

**`versionsSeen` 与 `channelVersions` 是两套版本账。** 前者是「每个超步看到的各通道版本」，后者是「各通道当前版本」。这是 Pregel 里做**增量触发判断**的标准手段：只有版本变了的通道才需要被消费。没有它，每轮都要重算所有节点是否该触发，复杂度会变成「节点数 × 边数」每超步。

**`resumeValuesForTask` 与 `withResumeMap` 把恢复值塞进 `context` 而不是函数参数。** 恢复值（用户对中断点的回答）要沿「引擎 → 任务 → 节点」这条链传下去，而节点签名是固定的。选择放在 context 里是 Go 的常见做法（`context.Value`），代价是**类型不安全且不可见**——读代码时看不出某个节点依赖了恢复值，只能靠它主动调 `interrupt.GetResumeValues`。

## 关键取舍

**源码事实：** `Engine` 可复用，但 `Run` 每次开头**必须重置** `currentCheckpoint` / `channelVersions` / `versionsSeen` / `deferredCheckpoints`，否则跨运行累积导致内存无界增长；注释注明由浸泡测试发现。

**源码事实：** `Run` 的收尾 `defer` 必须先 `fwWg.Wait()` 再 `close(outputCh)`，注释说明这是为了避免「close 与 send」的数据竞争。

**源码事实：** `Run` 文档要求调用方读到 `outputCh` 关闭为止，否则泄漏 goroutine。

**源码事实：** 默认 `recursionLimit = 25`；超限抛 `GraphRecursionError`。

**源码事实：** 终止条件是 `len(tasks) == 0`，不是任何标志位。

**源码事实：** 两种任务选择模式：`AnyPredecessor`（默认，任一前驱完成触发，支持环）与 `AllPredecessor`（所有前驱完成触发，不支持环）；后者的实现是 O(n) 全扫描，注释承认「为 fan-in 正确性接受复杂度」。

**源码事实：** 三种持久化档位：Sync（阻塞保存、失败终止）、Async（goroutine 保存、失败只记日志）、Exit（内存累积、退出时保存）。

**架构推论：** BSP 模型用「失去细粒度并行」换「确定性」，这在 LLM 系统里是必要的——模型本身已经在引入不确定性，执行层再引入一份会让调试不可能。代价是图的形状被限制：能并行的地方必须真的能等齐，否则只能用 `AnyPredecessor` 的弱保证。

**架构推论：** 「中断是带持久化的一等退出」这个设计是 Agent 系统的分水岭。它意味着 Agent 可以**停在需要人批准确的地方**、序列化全部状态、等人几小时后回来再续。没有这个能力，「Human-in-the-loop」就只能实现成「整轮重跑」——成本与体验完全不同。

**架构推论：** `Async` 档位把保存失败降级成日志，这是唯一一个「失败不停机」的分支。它把「可观测性缺口」变成了一种资源：用检查点的完整性换主流程不被 I/O 阻塞。这个取舍只在「丢了能重跑」的场景成立——如果检查点是唯一的状态副本，Async 就是不安全的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/harness/graph/pregel/engine.go` | `Engine` | 执行器状态（检查点器、中断点、并发上限、版本账） |
| `internal/harness/graph/pregel/engine.go` | `NewEngine` | 绑定图与选项，默认递归上限 25 |
| `internal/harness/graph/pregel/engine.go` | `WithMaxConcurrency` | 节点并发上限 |
| `internal/harness/graph/pregel/engine.go` | `WithRetryPolicy` | 节点重试策略（nil 时用默认） |
| `internal/harness/graph/pregel/engine.go` | `ExecuteResult` | 执行结果结构 |
| `internal/harness/graph/pregel/engine.go` | `prepareNextTasksWithMode` | 两种触发语义的分派点 |
| `internal/harness/graph/pregel/engine.go` | `prepareNextTasksDAG` | AllPredecessor 的 O(n) 扫描实现 |
| `internal/harness/graph/pregel/optimized.go` | `PregelOptimizedEngine` | 带优先级的执行变体 |
| `internal/harness/graph/pregel/optimized.go` | `PriorityTaskQueue` | 优先级任务队列 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [Pregel 论文（Malewicz et al., 2010）](https://dl.acm.org/doi/10.1145/1807167.1807184)
- [LangGraph 文档：图执行](https://langchain-ai.github.io/langgraph/concepts/low_level/)
- [ragflow 源码：pregel/engine.go](https://github.com/infiniflow/ragflow/blob/main/internal/harness/graph/pregel/engine.go)

## 与主核 / AI 的关系

图执行内核与主核（多模态数据库内核）的交集在**确定性执行与状态持久化**上，而且两者给出了同一个问题的两种答案。

**超步栅栏 vs 事务隔离级别。** 数据库用隔离级别统一说明「并发事务之间能看到什么」；Pregel 用超步栅栏给出一个固定的、最强的答案：**同一超步内不可见，跨超步全部可见**。这相当于「每超步一次全屏障的串行化」。简单、可证明，但代价是并行度受限——数据库愿意为吞吐放松隔离，Pregel 不愿意为吞吐放松确定性。

**检查点 vs WAL。** 两者都是「把中间状态落盘以便崩溃后恢复」，但粒度与语义相反：WAL 记录**操作**（重放得到状态），检查点记录**状态快照**（直接读回）。WAL 的好处是记录小、可细粒度；检查点的好处是恢复快、不需重放。三种 `Durability` 档位本质上是**在 WAL 与检查点之间滑动**：Sync ≈ 每个超步一次检查点，Exit ≈ 只在结束时一次。

**递归上限 vs 查询超时。** 两者都是「防止无限消耗的保护」，但性质不同：数据库的 `statement_timeout` 按**时间**算，`GraphRecursionError` 按**超步数**算。按超步算的好处是确定（同一图同一输入必然在同一超步触发），按时间算的好处是能防住「单步巨慢」。两者都需要，ragflow 这里只有前者——一个需要长时间等待的单步节点不会被这个上限拦住。

**`AllPredecessor` 的 O(n) 扫描 vs 数据库的 fan-in 处理。** 多路汇合在数据库里对应「多个外键指向同一行」的约束检查，实现上是「逐个子句检查」而不是全表扫描——因为数据库有索引。图执行器没有索引可用（节点触发条件依赖动态完成的集合），于是选了全扫描换正确性。**这是「缺少索引结构时用简单算法保证正确」的标准取舍**，在数据量小（Agent 图的节点数通常是个位数到几十）时完全合理。

对 agent 工程最直接的价值有三条。其一：**BSP 的确定性是调试的前提**——Agent 出错时如果连「同样输入是否同样输出」都保证不了，就无法复现、无法定位。其二：**中断+检查点让 Human-in-the-loop 成为可能**，这是 Agent 从「演示」走向「生产」的必要条件。其三：**两种触发语义必须显式选**——`AnyPredecessor` 支持环但汇合语义弱，`AllPredecessor` 汇合语义强但不支持环，一个图到底用哪种不能靠猜。
