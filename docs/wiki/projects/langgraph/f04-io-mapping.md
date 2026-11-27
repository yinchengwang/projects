---
project: langgraph
feature: io-mapping
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/_io.py
    symbols: [map_input, map_output_values, map_command]
---

# 输入输出与命令映射

## 一句话职责

三个映射函数把三种对外形态——图输入、通道读值、`Command`——翻译成内部统一的 `(通道, 值)` 写入序列或直接输出块，是图边界上唯一的形态转换层。

## 机制是什么

外部只认「输入对象」与「输出块」，内部只认通道。这中间的转换需要方向相反的三个函数：

- `map_input` 把外部输入变成待应用的写入序列，是「进」；
- `map_command` 把 `Command` 的三部分（`goto`、`resume`、`update`）拆成带任务标识的写入三元组，是「控制流进」；
- `map_output_values` 反向：给定输出通道集合与本次写入，决定要不要从通道里读出一块输出，是「出」。

三者共享一条约定：通道选择集有两种形态。传单个字符串表示「把整个值当成一个对象」，传序列表示「按名取子集构成 dict」。这条双形态贯穿读写两侧，不是三个函数各自发明的。

## 怎么实现的

**`map_input`。** 输入为 `None` 时直接 `return`（生成器，等于不产任何写入）——这条分支让「恢复执行、无新输入」的调用体面地通过。单字符串选择集时产出 `(该通道, 整个输入)`，这意味着整个 chunk 被塞进一个通道，对应 schema 只有 `ROOT` 的场景。序列选择集时先要求 chunk 是 dict，否则抛 `TypeError`；然后逐键判断，命中选择集就产出写入，未命中的键**只警告不报错**，警告文案会带上该键与选择集——这是为了让上游多传字段时仍能跑通，同时留下可诊断痕迹。

**`map_command`。** 先挡掉 `graph == Command.PARENT`：在最外层图收到父命令说明调用层次错了，抛 `InvalidUpdateError`。然后按三部分展开，全部用 `NULL_TASK_ID` 作为任务标识——这个全零 UUID 的含义是「属于检查点自身而不是某个任务」，读侧据此把写入当作检查点自己的状态。

`goto` 可以是单值或序列。`Send` 包产出 `(NULL_TASK_ID, TASKS, send)`；字符串目标产出 `(NULL_TASK_ID, "branch:to:<目标>", START)`——**值写 `START` 而不是 `None`**，因为目标节点的分支通道是 `EphemeralValue(guard=False)`，写任何非空值即可让它可用并触发订阅者。其他类型抛 `TypeError`。`resume` 非 `None` 时产出一条 `RESUME` 写入；`update` 非空时经 `_update_as_tuples()` 展开成普通通道写入。

注意这三个部分不是互斥的：一个 `Command` 可以同时带 `goto` 与 `update`，映射会把它们全部产出，顺序是 `goto` → `resume` → `update`。

**`map_output_values`。** 参数 `pending_writes` 有一个特殊值 `True`，语义是「无条件输出」。否则就要判断本次写入是否触及输出选择集：单字符串形态下检查 `any(chan == output_channels ...)`，序列形态下先构造命中集合再判真。真才 `read_channel` / `read_channels` 读出来产出。

这里有个容易忽略的细节：判断依据是**写入集合**而不是通道版本。这样「本步没有写任何输出通道」的超级步不会产出重复的输出块，避免下游收到内容相同的两次 values 事件。

**读侧的容错。** `read_channel` 捕获 `EmptyChannelError` 并在 `catch=True` 时返回 `None`；`read_channels` 用 `skip_empty` 反向控制：默认 `True` 时空通道不进结果 dict，`False` 时把异常透出去。所以「输出 schema 里有某个键、但该通道当前为空」默认表现为「该键缺失」，而不是「该键为 None」。

## 关键取舍

**源码事实：** 输入侧的未知键只警告不报错。这与「输出侧未知通道」的处理不同：`apply_writes` 遇到节点写未知通道也是警告加忽略。两处都选择宽容，取向是「运行时不让结构性笔误炸掉整个执行」，代价是错误可能被延迟发现。

**源码事实：** `Command` 的三类写入共用 `NULL_TASK_ID` 作为归属。这使得「图输入」「`update_state`」「`Command` 更新」在存储层看起来都是同一类写入——它们不属于任何具体任务。DeltaChannel 的重放逻辑正是依赖这条约定把检查点自身的写入当作它的初始状态。

**源码事实：** `goto` 字符串目标写的是 `START` 常量作为值，而不是空值或布尔。因为触发只判「通道是否可用 + 版本是否更新」，写什么值不重要；但这意味着这些分支通道里存的值没有业务含义，读它毫无信息。

**架构推论：** 输出判定用「写入是否触及输出通道」而非「通道是否变化」，把「本步有没有产出」这个语义交给了写入集合。好处是确定性强、与通道实现无关；代价是若某个通道因为 reducer 语义在不被显式写入时也发生变化（例如 `apply_writes` 里的空更新推进），这种变化不会触发输出。

**架构推论：** 三个函数都是生成器，调用方可以惰性消费，也可以像执行循环那样先 `deque(...)` 求值一趟再复用。这让「先看有没有输入再决定是不是空输入错误」这类逻辑不必构造两次。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/_io.py` | `map_input` | 外部输入 → 通道写入序列，含未知键告警 |
| `libs/langgraph/langgraph/pregel/_io.py` | `map_output_values` | 按输出通道集合决定是否读出 values 块 |
| `libs/langgraph/langgraph/pregel/_io.py` | `map_command` | `Command` 的 `goto`/`resume`/`update` 拆成写入三元组 |
| `libs/langgraph/langgraph/pregel/_io.py` | `read_channel` / `read_channels` | 带 `EmptyChannelError` 容错的读 |
| `libs/langgraph/langgraph/pregel/_io.py` | `map_output_updates` | 按任务聚合写入，产出 updates 块 |

## 参考

- [LangGraph 源码：pregel/_io.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_io.py)
- [LangGraph 源码：pregel/_loop.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_loop.py)
- [LangGraph 文档：流式输出模式](https://langchain-ai.github.io/langgraph/concepts/streaming/)

## 与主核 / AI 的关系

「边界层只做形态转换、不持有状态、不做业务判断」与主核把文件格式解析与元组格式解耦是同一分工：解析器产出统一的元组流，执行器只认元组。这里的三个函数就是解析器，`apply_writes` 与通道是执行器。

对 AI 侧而言，最实用的一条是输入键的宽容语义：给图传了 state schema 里不存在的字段，不会报错、只在日志里留一条警告，因此「我传了参数但节点读不到」的排查应该先去看警告而不是去翻节点代码。另一条是 `Command(update=...)` 会绕过节点的写入器直接写通道，所以它不做 `mapper` 转换，也不会经过节点的输出 schema 过滤。
