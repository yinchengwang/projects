---
project: langgraph
feature: interrupts-and-errors
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/errors.py
    symbols: [GraphInterrupt, ParentCommand, NodeInterrupt]
---

# 中断、父命令与图错误

## 一句话职责

`GraphBubbleUp` 的三个子类共同构成「不是失败、但要穿透执行栈」的信号通道：中断要穿到根图并落检查点，父命令要穿到父图并改写目标命名空间。

## 机制是什么

执行栈是「Pregel 循环 → PregelRunner → 重试层 → 节点」。节点抛出的异常如果只是普通异常，就该被错误处理与重试机制消费；但有两类信号必须**原样穿到更外层**：

- **中断**：某个 `interrupt()` 调用需要把问题交给人类，节点不能继续。信号必须穿过重试层（不重试）、穿过并发执行器（不取消兄弟任务视为失败）、穿到根图（子图的中断由根图统一呈现）。
- **父命令**：节点返回 `Command(graph=Command.PARENT)`，意图是操作父图。子图的执行栈不认识那个目标节点，必须把命令原样交给调用它的父图。

`GraphBubbleUp` 是一个空基类，只用作「这条异常别当失败处理」的标记。`GraphInterrupt` 与 `ParentCommand` 继承它，`NodeInterrupt` 又继承 `GraphInterrupt`。

## 怎么实现的

**中断的产生。** `interrupt()` 从配置里取 scratchpad，用 `interrupt_counter()` 拿本次调用的序号，然后按三种情况走：已有本地 resume 列表且序号在范围内 → 直接返回对应值（并回写一个截断到 `idx+1` 的 RESUME 写入）；有全局 resume 值 → 校验后追加进列表并返回；两者都没有 → 抛 `GraphInterrupt`，携带一个 `Interrupt` 对象，其 `id` 由检查点命名空间与「已回答数量」共同哈希得出。

这个 id 的构成方式解释了为什么「同一个节点里的多次 interrupt」能被分辨：命名空间区分任务，计数区分任务内的调用次序。

**中断的保存。** `PregelRunner.commit` 对 `GraphInterrupt` 有专门分支：只有当 `exception.args[0]` 非空时才写检查点，写入的是 `(INTERRUPT, payload)` 以及该任务已产生的 `RESUME` 写入。空负载的 `GraphInterrupt`（例如静态断点 `interrupt_before` 抛出的）不写中断负载——那种暂停不需要人类回答。

**中断的合并。** 同一超步里多个任务同时中断时，`_panic_or_proceed` 把每个 `GraphInterrupt` 的 args 展开收集，最后合成一个 `GraphInterrupt`，负载是全部 `Interrupt` 的元组。于是外部只看到一次中断，里面装着若干问题。

**中断的呈现。** `PregelLoop._suppress_interrupt` 判定「这是非嵌套图的中断」后做三件事：推一条 `GraphInterruptEvent` 生命周期事件；若当前任务有待应用的写入，先 `apply_writes` 再补发一次 values 事件（让调用方看到中断那一刻的状态）；然后吞掉异常。吞掉是有意的——中断不是错误，不该以异常形式抵达调用方。

**父命令的产生。** `_control_branch` 在编译期生成的分支函数里检查 `command.graph == Command.PARENT`，命中就直接 `raise ParentCommand(command)`。静态边做不到这件事，因为父命令只能由图执行到一半时才决定。

**父命令的转发。** 重试层捕获 `ParentCommand` 后分三种情况：目标就是本图（`cmd.graph` 等于当前命名空间、其规范化形式、或任务名）→ 把命令写给本任务的 writers，然后 `break` 结束重试循环；目标是 `Command.PARENT` → 用 `_checkpoint_ns_for_parent_command` 剥掉当前层命名空间后替换 `cmd.graph`，再上抛；其它情况 → 直接上抛。`_checkpoint_ns_for_parent_command` 还要处理纯数字段——那是运行时为区分并发任务插入的，不是真实的层级。

**取消与用户取消的区分。** `NodeCancelledError` 不是 `GraphBubbleUp`，它是普通 `Exception`：节点体自己抛 `asyncio.CancelledError` 时，重试层用 `_is_user_raised_cancelled`（检查 `asyncio.Task.cancelling() == 0`）判断这不是框架发起的取消，于是转换成 `NodeCancelledError` 让它走正常错误路径。否则该任务会被当成「被兄弟任务失败连带取消」而静默消失，整个 run 报成功。

**超时。** `NodeTimeoutError` 刻意不继承内置 `TimeoutError`（后者是 `OSError` 子类），这样默认的 `RetryPolicy` 会把它当作可重试异常。

## 关键取舍

**源码事实：** `GraphInterrupt` 的 docstring 明确写着「永远不会被直接抛出，也不会暴露给用户」。它只是运行期间的搬运工具，最终形态是检查点里的 INTERRUPT 写入与 stream 上的 `__interrupt__`。

**源码事实：** `NodeInterrupt` 已被标记废弃，`__init__` 里直接 `warn` 并转成携带 `Interrupt` 的 `GraphInterrupt`。保留它只为兼容旧代码，新代码应调 `interrupt()`。

**源码事实：** 空负载中断不写检查点负载。这让「静态断点暂停」与「需要人回答的暂停」在持久层就能区分开，而不需要额外的标志位。

**架构推论：** 用异常做控制流换来了「零侵入穿透」：节点不需要知道自己在第几层子图里，只要抛就行，中间每一层都有对应的 catch 分支决定「继续抛还是就地处理」。代价是这些异常必须被每一层显式识别，漏一处就会变成真正的失败。

**架构推论：** 中断在根图被吞掉、父命令在目标图被消化，两者最终都不以异常形式出现。因此「图以异常结束」与「图暂停了」在调用方是两种完全不同的观测，不会混淆。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/errors.py` | `GraphBubbleUp` | 「非失败、需穿透」的标记基类 |
| `libs/langgraph/langgraph/errors.py` | `GraphInterrupt` | 中断信号，携带 `Interrupt` 序列 |
| `libs/langgraph/langgraph/errors.py` | `ParentCommand` | 父命令信号，携带要交给父图的 `Command` |
| `libs/langgraph/langgraph/errors.py` | `NodeInterrupt` | 已废弃的节点级中断入口 |
| `libs/langgraph/langgraph/errors.py` | `NodeCancelledError` | 把用户自抛的 `CancelledError` 转成正常失败 |
| `libs/langgraph/langgraph/types.py` | `interrupt` | 按 scratchpad 序号决定返回值还是抛中断 |
| `libs/langgraph/langgraph/pregel/_runner.py` | `PregelRunner.commit` | 中断与错误分别写入检查点 |
| `libs/langgraph/langgraph/pregel/_runner.py` | `_panic_or_proceed` | 合并同超步的多个中断 |
| `libs/langgraph/langgraph/pregel/_loop.py` | `_suppress_interrupt` | 根图吞掉中断，补发最后一次 values |
| `libs/langgraph/langgraph/pregel/_retry.py` | `run_with_retry` | 父命令的本图消化与跨层转发 |
| `libs/langgraph/langgraph/graph/state.py` | `_control_branch` | 分支函数里把 PARENT 目标转成 `ParentCommand` |

## 参考

- [LangGraph 源码：errors.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/errors.py)
- [LangGraph 源码：pregel/_runner.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_runner.py)
- [LangGraph 文档：中断与 human-in-the-loop](https://langchain-ai.github.io/langgraph/concepts/human_in_the_loop/)

## 与主核 / AI 的关系

用异常做跨层信号，与主核里「用错误码逐层上传以免污染正常路径」是相反的策略。主核倾向返回值；这里选异常，是因为执行栈里套着用户代码、LLM 调用、并发执行器三种不可控边界，只有异常能保证信号不被中间的任意代码吞掉——中间层想吞必须显式写 `except GraphBubbleUp`。

对 AI 侧最关键的一条是取消语义：agent 工具里随手写 `raise asyncio.CancelledError()` 会让整个 run 报成功。框架为此专门加了 `NodeCancelledError` 与 `cancelling()` 判定，这说明「静默成功」比「报错」危险得多。做 agent 编排时若发现某步明明失败却显示成功，先查这一条。
