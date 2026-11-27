---
project: langgraph
feature: managed-values
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/managed/base.py
    symbols: [ManagedValue, is_managed_value]
  - path: libs/langgraph/langgraph/managed/is_last_step.py
    symbols: [IsLastStepManager, RemainingStepsManager]
---

# 托管值与剩余步数

## 一句话职责

托管值是「看起来像状态字段、实际上不存任何数据」的一类键：它们的值每次读取时由框架用当前步数现场算出，因此永远与执行进度同步，也不会写进检查点。

## 机制是什么

普通状态键背后是通道（`BaseChannel`），有值、有版本、被写进检查点。托管值不是通道，它是一个类——`ManagedValue` 的子类，只提供一个静态方法 `get(scratchpad) -> V`。

判定方式极其简单：`is_managed_value(value)` 检查「是不是类」且「是不是 `ManagedValue` 的子类」。这个判定在两处被用上——建图时（`_get_channels` 判断某个注解是不是托管值）和编译尾声（把 `channels` 字典里不是 `BaseChannel` 的项切成 `managed` 映射传给 `validate_graph`）。

所以「托管值是混在通道表里的非通道项」是理解这套机制的关键：它们在装配阶段同处一个字典，靠类型分辨，在运行期走完全不同的取数路径。

## 怎么实现的

**声明方式。** 内置两个都是通过 `Annotated` 挂上去的：

- `IsLastStep = Annotated[bool, IsLastStepManager]`
- `RemainingSteps = Annotated[int, RemainingStepsManager]`

`Annotated` 的最后一个元数据槽里放的是**类本身**而不是实例——`ManagedValue` 的 `get` 是 `@staticmethod`，所以不需要实例状态。

**从注解到托管映射。** `_is_field_managed_value` 先看 `__metadata__` 的最后一层，取 `get_origin(meta[-1]) or meta[-1]` 得到装饰对象，再交给 `is_managed_value` 判。若外层还有包装（`Required`、`NotRequired` 这类），它会取出第一个类型参数递归下去——所以 `NotRequired[IsLastStep]` 也能被认出来。

**值的来源是 scratchpad。** `get(scratchpad)` 只读 `PregelScratchpad` 的两个字段：`step` 与 `stop`。两个内置实现因此都是单行算术：

- `IsLastStepManager.get` 返回 `scratchpad.step == scratchpad.stop - 1`
- `RemainingStepsManager.get` 返回 `scratchpad.stop - scratchpad.step`

**`stop` 的来处。** `PregelLoop.__enter__` 里 `self.step = checkpoint_metadata["step"] + 1`，然后 `self.stop = self.step + config["recursion_limit"] + 1`。注意两点：`step` 是从检查点续上的，所以恢复运行时剩余步数正确地变少；`stop` 多加了一个 1，因为 `tick()` 的退出条件是 `self.step > self.stop` 而不是 `>=`。

**每次读取都重算。** `local_read` 把 `select` 拆成两半：在 `managed` 里的键单独收集成 `managed_keys`，剩下的才当通道读；最后 `values.update({k: managed[k].get(scratchpad) for k in managed_keys})`。`_proc_input` 更直接——节点输入的每个键若不在 `channels` 里，就走 `managed[chan].get(scratchpad)`。

这意味着：同一个节点在一次执行里两次读 `remaining_steps` 会得到同一个值（scratchpad 是任务构造时固定的），但跨步就不同。值不存在于任何地方，只存在于「此刻」的算式里。

**为什么不进检查点。** 因为它们根本没有值可以存。`apply_writes` 处理的是通道写入，托管值不在写入目标里；检查点的 `channel_values` 里也不会出现这些键。恢复运行时它们从新算出的 `step`/`stop` 重新得出，天然正确——若存下来反而会与续跑的步数矛盾。

**input/output schema 里禁止出现。** `_add_schema` 对 input 与 output schema 用 `allow_managed=False` 调用，一旦发现托管值直接抛 `ValueError`。理由：托管值是运行期状态，从外部输入塞进来毫无意义，向外部输出里承诺它也毫无意义。state schema 才允许。

**与图结构的交互。** 编译期算 `output_channels` 与 `stream_channels` 时都显式过滤掉托管值（`if not is_managed_value(val)`）。所以「把这些键当成输出字段」这种误用会在源头被排除——它们既不会被流出来，也不会被当成节点返回值目标。

## 关键取舍

**源码事实：** 托管值只给两个内置项。这是一个封闭集合，没有「用户自定义托管值」的公开路径（`ManagedValue` 是 ABC，技术上可继承，但没有任何注册机制把它接进 `managed` 映射）。所以它更像「框架内部的两个派生视图」，而不是通用扩展点。

**源码事实：** 值是**任务级快照**而非实时值。scratchpad 在任务构造时定下 `step`/`stop`，一次任务执行期间不变。这避免了「同一节点的不同语句看到不同步数」的诡异行为。

**源码事实：** 两个内置项都基于 `stop - step` 的差值。因此它们表达的是「还剩几个超步」，不是「还剩几次节点调用」——一个超步可以并行跑多个节点，全部算一步。

**架构推论：** 把「派生值」实现成 `ManagedValue.get(scratchpad)` 而不是通道 reducer，避免了「谁负责写它」这个无解问题。通道必须有写者，而步数没有写者——它由调度器自然推进。用拉取式求值绕开了这一整类设计难题。

**架构推论：** 与通道共用一个字典、靠类型分辨，使节点读状态时不需要区分「这是存储字段还是派生字段」——两者在节点入参上都是一样的 `state["key"]`。代价是任何遍历通道表的地方都必须记得过滤，`validate_graph`、`output_channels`、`stream_channels` 三处都各写了一遍这个过滤。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/managed/base.py` | `ManagedValue` | 托管值契约：子类实现静态 `get(scratchpad)` |
| `libs/langgraph/langgraph/managed/base.py` | `is_managed_value` | 判定某个对象是不是托管值规格（`TypeGuard`） |
| `libs/langgraph/langgraph/managed/is_last_step.py` | `IsLastStepManager` | `step == stop - 1`，即是否最后一步 |
| `libs/langgraph/langgraph/managed/is_last_step.py` | `RemainingStepsManager` | `stop - step`，即剩余超步数 |
| `libs/langgraph/langgraph/pregel/_algo.py` | `local_read` | 读状态时把托管键从 select 里拆出并现场求值 |
| `libs/langgraph/langgraph/pregel/_algo.py` | `_proc_input` | 节点入参构造：通道取值，非通道走托管求值 |
| `libs/langgraph/langgraph/pregel/_loop.py` | `PregelLoop.__enter__` | 从检查点续上 `step` 并据此算出 `stop` |
| `libs/langgraph/langgraph/graph/state.py` | `_is_field_managed_value` | 从 `Annotated` 元数据识别托管值，穿透 `Required` 等包装 |
| `libs/langgraph/langgraph/_internal/_scratchpad.py` | `PregelScratchpad` | 承载 `step`/`stop` 的冻结记录 |

## 参考

- [LangGraph 源码：managed/base.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/managed/base.py)
- [LangGraph 源码：managed/is_last_step.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/managed/is_last_step.py)
- [LangGraph 文档：递归上限与步数控制](https://langchain-ai.github.io/langgraph/concepts/low_level/#recursion-limit)

## 与主核 / AI 的关系

托管值与主核里「不做物化、查询时计算的派生列」是同一类设计：视图里能算出答案的字段不落盘，避免更新时机与一致性维护。差别在于主核的派生列可以自由定义表达式，这里的派生值只有两个且写死在框架里——因为它们依赖的是框架自己的步进状态，不是用户数据。

对 AI 侧最实用的地方是 `remaining_steps`：agent 循环里用它决定「还要不要继续找工具」。典型用法是判断剩余步数低于某个阈值时强制收尾，输出一个保底答案，而不是等 `GraphRecursionError` 抛出来。要注意它是按**超步**计数的：一个超步里并行的三个工具调用只消耗一步，所以按它估算「还能跑几次工具」会偏保守。
