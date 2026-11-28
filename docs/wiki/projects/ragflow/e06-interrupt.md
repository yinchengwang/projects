---
project: ragflow
feature: interrupt
title: 中断与恢复
page_type: feature
schema_version: 1
refs:
  - path: internal/harness/graph/interrupt/interrupt.go
    symbols: [WithInterruptContext, GetResumeValues, Interrupt]
  - path: internal/harness/graph/types/types.go
    symbols: [NodeTriggerMode, StreamMode, Command, Send]
---

# 中断与恢复

## 一句话职责

让图能在某个节点内部**停下来等人**：节点调用 `Interrupt` 抛出一个可序列化的中断，图把当前状态存成检查点后退出，等人把答复写进来后从该节点开头重跑，`Interrupt` 这次就把答复直接返回。

## 机制是什么

Agent 系统里「人要在中间做决定」是刚需：发邮件前要确认、删文件前要批准、花钱前要授权。

不用中断机制的实现方式是「整轮重跑」——把决策点做成一个独立的 API 调用，用户答复后再从头跑一次。代价很直观：**重跑意味着前面所有 LLM 调用白烧**。一个五步 Agent 在第四步等人批准，重跑就是四次白烧。

中断机制要解决的正是这件事：**把「图的状态」和「控制流的位置」一起持久化**，等人回来时接着跑。这在实现上有个绕不开的难点：Go 的函数栈没法序列化，你不可能把「停在某个函数第 37 行」这件事存进数据库。**所以唯一的解法是「从节点开头重跑，但把已经用过的答复按顺序回放」**。

这套做法对节点有两项硬要求：节点必须是**幂等**的（重跑前半段不能有副作用），以及节点内的 `Interrupt` 调用顺序必须**稳定**（因为答复是按顺序匹配的）。这两条都是这个机制的价格，而代价藏在调用方而不是实现里。

## 怎么实现的

**中断上下文挂在 `context.Context` 上。** `WithInterruptContext` 往 ctx 里塞一个 `*interruptContext`，里面三样东西：一把 `sync.Mutex`、一个 `resumeValues []interface{}`、一个 `index int`。

**`Interrupt(ctx, value)` 的逻辑只有两条分支，而顺序是关键：**

```go
if v, ok := ic.consumeNextResumeValue(); ok {
    return v, nil
}
return nil, &errors.GraphInterrupt{...}
```

**先尝试消费答复，没有才抛中断。** 这个顺序让同一段节点代码在两次执行里表现相反：首跑时 `resumeValues` 为空 → 抛中断；重跑时里面有值 → 直接返回。**节点代码完全不用感知自己是在首跑还是重跑**——这是这套 API 最优雅的地方。

**`consumeNextResumeValue` 在同一个锁内读值并推进 index。** 注释写明理由：「avoids TOCTOU races between separate getResumeValues/getInterruptIndex calls」。如果拆成「先读值、再加索引」两步，两次加锁之间可能被另一个 goroutine 插入。这里把「读 + 推进」合成一个原子动作，与数据库里的 `SELECT FOR UPDATE` 是同一类解法。

**中断 ID 是「值哈希 + 进程级原子计数器」的组合。** `generateInterruptID` 用 `sha256` 算 value 的指纹，再用 `atomic.AddInt64` 取一个递增序号。注释解释了为什么两个都要：

> two interrupts with the same value (e.g. "Please provide input") are still distinguishable

只用哈希不行（同一句提示语会撞），只用计数器也不行（跨进程重启后计数从 0 开始，历史检查点里的 ID 会与新 ID 重复）。**两者结合才既唯一又可复现**。

**`SubGraphStateCtxKey` 单独定义在 interrupt 包里，注释点明了原因**：「Defined here so that both the engine (pregel) and the sub-graph node (graph/loop.go) can access it without import cycles」。Go 的包循环导入是编译期错误，所以共享的 context key 必须放在一个双方都能依赖的公共位置——**这是一个被语言约束逼出来的设计，不是架构选择**。

**`AppendResumeValue` / `GetResumeValues` / `Reset` 三个导出函数都做了 nil 安全**：`GetInterruptContext` 返回 nil 时，`getResumeValues`、`getInterruptIndex`、`appendResumeValue`、`reset` 都是 nil 接收者方法并直接返回。这让「在没有中断上下文的普通代码里调这些函数」不会 panic——**但 `Interrupt` 本身不是**：拿不到上下文时它返回 `ErrMissingInterruptContext` 而不是静默降级。这个不对称是有意的：查询类操作降级成空值无害，而「你以为自己中断了其实没有」会让图跑过头。

**`Command` 与 `Send` 是中断之外的另一半。** 中断解决「停下来」，这两个解决「跳过去」：

`Send` 只有两个字段（`Node` 目标节点名、`Arg` 传给它的输入），语义是「以这个输入运行那个节点」——**它是 `Command.Goto` 的元素类型**。

`Command` 有四个字段，注释逐条写死了每个的取值域：`Graph`（`nil`/空表示当前图，`"__parent__"` 表示最近的父图，常量 `Parent` 就是这串）、`Update`（要合并进状态的增量）、`Resume`（恢复值）、`Goto`（节点名 / 节点名序列 / `Send` / `Send` 序列，**四种形态**）。

`Goto` 用 `interface{}` 承载四种形态是个明确的取舍：**Go 没有联合类型**，要么用四种子类型加类型断言，要么用 `interface{}` 加类型断言。选后者省了类型定义，代价是**编译期不知道传错**，只能在运行期断言失败。`Command` 的 `WithGoto` / `WithGraph` 等链式方法让调用点至少可读。

**`NodeTriggerMode` 与 `StreamMode` 也在同一个文件里**，它们是 e04 讲过的「两种图语义」与「七种流式粒度」的枚举声明处：

- `NodeTriggerAnyPredecessor`（`"any"`，任一前驱完成即触发，支持环）与 `NodeTriggerAllPredecessor`（`"all"`，所有前驱完成才触发，不支持环）；
- 七种 `StreamMode`：`values`（每步全量）、`updates`（仅增量）、`custom`（节点内自写）、`messages`（LLM token 级）、`checkpoints`（每次存检查点）、`tasks`（任务起止）、`debug`（后两者的合集）。

**这七种粒度的存在说明流式输出不是「要或不要」而是「要多细」**：`values` 适合调试（每步都能看到完整状态），`updates` 适合前端渲染（只传变化），`messages` 适合打字机效果。

## 关键取舍

**源码事实：** `Interrupt` 先尝试 `consumeNextResumeValue()`，成功则直接返回答复，否则抛 `GraphInterrupt`——因此节点代码无需区分首跑与重跑。

**源码事实：** `consumeNextResumeValue` 在同一把锁内读值并推进 `index`，注释说明这是为消除 `getResumeValues` / `getInterruptIndex` 分离调用的 TOCTOU 竞态。

**源码事实：** 中断 ID = `sha256(值)` + 进程级 `atomic` 计数器，注释说明「同值的两个中断仍需可区分」。

**源码事实：** `SubGraphStateCtxKey` 定义在 interrupt 包里以避开 `pregel` 与 `graph/loop` 的循环导入。

**源码事实：** 查询/追加/重置类函数都是 nil 接收者安全的，但 `Interrupt` 拿不到上下文时返回 `ErrMissingInterruptContext` 而不是静默返回 nil。

**源码事实：** `Command.Goto` 是 `interface{}`，可承载节点名、节点名序列、`Send`、`Send` 序列四种形态。

**源码事实：** 恢复的语义是「从节点开头重跑、按序回放答复」，docstring 明写「The graph resumes from the start of the node, re-executing all logic」。

**架构推论：** 「从节点开头重跑」是所有「无法序列化函数栈」的语言做中断的唯一选项。它把实现成本压到最低（不需要续延捕获），代价转移成**对节点的幂等性要求**。这个成本转移经常被低估：一个节点如果在 `Interrupt` 之前发了邮件或写了数据库，重跑就会重复。**API 无法强制幂等，只能靠文档警告**——这是这类机制固有的边界。

**架构推论：** 「按顺序匹配答复」让节点内的中断顺序变成隐式契约。在节点里加一个 `Interrupt` 插到中间，会让后面所有中断的答复错位。这与 lightrag 的 `NameSpace` 常量（改名等于丢数据）是同一类问题：**隐式的位置依赖比显式的命名依赖更脆弱**，因为没有任何东西会在顺序变化时报警。

**架构推论：** 「ID = 值哈希 + 计数器」这个组合反映了一个通用模式：**内容哈希保证同输入同 ID（可复现），计数器保证同输入不同次仍可区分（可追溯）**。单用任一个都不行——这与会话日志里同时记录 `trace_id`（业务键）与自增 `seq`（序号）是同一手法。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/harness/graph/interrupt/interrupt.go` | `Interrupt` | 节点内中断入口（先消费答复后抛中断） |
| `internal/harness/graph/interrupt/interrupt.go` | `WithInterruptContext` | 把中断上下文挂到 ctx |
| `internal/harness/graph/interrupt/interrupt.go` | `GetResumeValues` | 读取当前答复列表（拷贝） |
| `internal/harness/graph/interrupt/interrupt.go` | `consumeNextResumeValue` | 锁内读值 + 推进索引 |
| `internal/harness/graph/interrupt/interrupt.go` | `generateInterruptID` | 值哈希 + 原子计数器 |
| `internal/harness/graph/interrupt/interrupt.go` | `SubGraphStateCtxKey` | 规避循环导入的共享 context key |
| `internal/harness/graph/types/types.go` | `Command` | 状态增量 + 恢复值 + 跳转指令 |
| `internal/harness/graph/types/types.go` | `Send` | 定向发送（Goto 的元素类型） |
| `internal/harness/graph/types/types.go` | `NodeTriggerMode` | 两种触发语义的枚举 |
| `internal/harness/graph/types/types.go` | `StreamMode` | 七种流式粒度 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [LangGraph 文档：人类介入（interrupt）](https://langchain-ai.github.io/langgraph/concepts/human_in_the_loop/)
- [Pregel 论文（Malewicz et al., 2010）](https://dl.acm.org/doi/10.1145/1807167.1807184)
- [ragflow 源码：graph/interrupt/interrupt.go](https://github.com/infiniflow/ragflow/blob/main/internal/harness/graph/interrupt/interrupt.go)

## 与主核 / AI 的关系

中断机制与主核（多模态数据库内核）的交集在**「把执行位置持久化」这个不可能任务上，两者给出了相似的妥协**。

**从节点开头重跑 vs 数据库的语句重试。** 数据库在死锁时回滚整个事务再重跑，与中断的「重跑节点」完全同构：都是**放弃中间状态、从头再来**。差别是数据库有日志可以做精细回滚（只撤销已执行的部分），而中断只能整节点重来——因为节点内的中间状态根本不可见。**能回滚的粒度取决于日志的粒度**，中断这里没有节点内日志，所以粒度必然是整节点。

**按顺序匹配答复 vs 隐式游标。** 中断答复靠 `index` 顺序匹配，不靠名字。这与 SQL 的 `ORDER BY` 省略时的行为是同一类风险：**依赖物理顺序的代码在数据变动时静默出错**。数据库行业为此把「显式排序」写进规范，中断机制没有——因为在节点里给每个 `Interrupt` 起名成本太高，而顺序依赖在单节点内通常稳定。

**幂等性要求 vs 事务的原子性。** 中断把幂等责任推给节点作者；数据库用事务把这个责任收进系统（要么全成要么全败，重试安全）。**这是「机制保证」与「约定保证」的分野**：系统能提供多强的保证，取决于它能观察到多少信息。数据库能看到每一条写操作，所以能保证；中断机制看不到节点内部，所以只能约定。

**`Command.Goto` 用 `interface{}` vs 联合类型。** Go 缺失联合类型导致的类型不安全，与 C 里用 `void*` 传递多态数据是同一个问题。数据库的列类型约束、图执行器的 `Goto` 断言，都是**把类型检查从编译期推到运行期**——代价是错误发现得晚，收益是不用为每种组合定义类型。

**`StreamMode` 的七种粒度 vs 数据库的隔离级别。** 两者都是「同一个操作，不同消费者需要不同视角」的实例。数据库用隔离级别统一描述可见性，图执行器用 `StreamMode` 描述输出粒度。差别是前者是**正确性维度**（读到什么），后者是**呈现维度**（输出什么）——所以前者影响结果，后者只影响性能与体验。

对 agent 工程最直接的价值有四条。其一：**中断是 Agent 从演示走向生产的门槛**——没有它，所有需要人批准的动作都只能靠「整轮重跑」，成本与体验都不可接受。其二：**节点必须幂等**，任何在 `Interrupt` 之前的副作用都会在恢复时重复。其三：**节点内的中断顺序是隐式契约**，插入一个中断会让后续答复错位。其四：**「值哈希 + 计数器」是生成可追溯 ID 的通用手法**，同时满足可复现与可区分。
