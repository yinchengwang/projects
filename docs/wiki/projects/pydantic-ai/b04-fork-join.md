---
project: pydantic-ai
feature: fork-join
title: 并行分叉与汇合
page_type: feature
schema_version: 1
refs:
  - path: pydantic_graph/pydantic_graph/paths.py
    symbols: [Path, PathBuilder, BroadcastMarker]
  - path: pydantic_graph/pydantic_graph/join.py
    symbols: [Join, JoinState, ReducerContext]
---

# 并行分叉与汇合

## 一句话职责

分叉与汇合被拆成「**路径上的标记**」与「**运行期的归约器**」两半：`BroadcastMarker` / `MapMarker` 在构建期声明「这里要并行」，`Join` 在运行期按 `(join_id, fork_run_id)` 分配一个 `JoinState` 把到达的输入逐个归约，直到该 fork run 的所有任务结束才向下游推进。

## 机制是什么

四个概念必须分清：

**路径（`Path`）是一串标记。** `Path.items` 里可能出现的标记有五种：`TransformMarker`（数据变换）、`MapMarker`（按可迭代拆分）、`BroadcastMarker`（同一份数据发往多条子路径）、`LabelMarker`（纯渲染）、`DestinationMarker`（终点）。`Path.last_fork` 与 `next_path` 两个属性只做两件小事：找最近的 fork、去掉第一项。

**`PathBuilder` 是纯函数式累积器。** 每个方法都返回**新的** builder（`working_items=[*self.working_items, next_item]`），因此可以链式写成 `edge_from(a).label('x').map().to(b)`。

**`Join` 在构建期声明一次，运行期按 fork run 多次实例化状态。** 它带的 `reducer` 与 `initial_factory` 是**模板**；真正被累加的是 `JoinState.current`，键是 `(JoinID, NodeRunID)`。

**`fork_stack` 是归属判据。** 每个任务带着自己进入过的分叉栈（`ForkStackItem(fork_id, node_run_id, thread_index)`）。汇合时靠比对栈里父分叉的 `node_run_id` 来判断「这次到达属于哪个 fork run」。

## 怎么实现的

**`Path` 的两个基本操作。** `last_fork` 反向扫描找出最近的 `BroadcastMarker | MapMarker`——它被用来判断路径里最后一个分叉是哪一类。`next_path` 返回 `Path(self.items[1:])`，`_handle_path` 就是靠递归剥掉首项来「执行」路径。

`_handle_path` 的分派只有三支（另两种标记在 `build()` 阶段就被消掉了，源码里用 `assert not isinstance(item, MapMarker | BroadcastMarker)` 明说「这些标记在图构建期间应当已从路径里移除」）：

- `DestinationMarker` → 造一个 `GraphTask`；
- `TransformMarker` → 用 `item.transform(StepContext(state, deps, inputs))` 变换输入，再对 `path.next_path` 递归；
- `LabelMarker` → 跳过，继续递归。

注意 `TransformMarker` 调用的是**同步**函数（`TransformFunction` 协议是 `def __call__(...) -> OutputT`，docstring 还特意说「与 StepFunction 很像，但必须是同步而非异步」），所以路径变换不能 `await`。

**`PathBuilder` 的五个方法里有两个制造分叉。**

- `to(destination, *extra_destinations, fork_id=None)`：有额外目的地时造一个 `BroadcastMarker(paths=[Path([DestinationMarker(d.id)]) for d in ...], fork_id=ForkID(NodeID(fork_id or generate_placeholder_node_id('broadcast'))))`。也就是说 `to(a, b, c)` 是 `broadcast` 的语法糖。
- `broadcast(forks, fork_id=None)`：直接包一层 `BroadcastMarker`。
- `map(fork_id=None, downstream_join_id=None)`：造 `MapMarker(fork_id=..., downstream_join_id=JoinID(...) if ... else None)`。类型上要求当前输出是 `Iterable[T]` 或 `AsyncIterable[T]`，返回 `PathBuilder[..., T]`——**类型层面就把「map 之后元素类型变成单个元素」表达出来了**。

`downstream_join_id` 这个参数的存在理由写在 docstring 与运行期代码里：**map 到空可迭代对象时，元素一个都不会到达，下游 join 就永远不触发**。预先声明这个 join 之后，`_handle_fork_edges` 会在展开 map 之前**急切地**为该 join 建一个 `JoinState`：

```
if (join_id := node.downstream_join_id) is not None:
    join_node = self.graph.nodes[join_id]
    child_fork_stack = fork_stack + (ForkStackItem(node.id, node_run_id, 0),)
    fork_run_id, downstream_fork_stack = self._resolve_join_fork_run(join_id, child_fork_stack)
    self.active_reducers.setdefault(
        (join_id, fork_run_id), JoinState(join_node.initial_factory(), downstream_fork_stack)
    )
```

于是空映射时该 join 仍然会以 `initial_factory()` 的值触发。这是一个精确的边界处理。

**`EdgePathBuilder` 的三处细节。** 这是 `PathBuilder` 的外层包装（多了 `sources`）：

- `to()` 也做 `get_origin(d) or d` 归一，理由同上页：`type[BaseNode[...]]` 可能是泛型别名。
- `broadcast(get_forks, ...)` 用回调形式而不是直接传序列——因为分叉的每条子路径都可能从一个**新的** builder 分出去，回调让调用方能对 builder 继续改造。
- `map()` 里有一条明确的 `NotImplementedError`：`if len(self.sources) > 1: raise NotImplementedError('Map is not currently supported with multiple source nodes...')`，注释全文如下：「当前实现会因此出错，因为你会从 MapMarker 到它的目的地得到每个源一份边，导致意外的多次执行。我怀疑这能在不做大重构的前提下修好，但理想行为是什么并不清楚。所以现在直接报错最简单。」这是一段极少见地把「不修的理由」写清楚的自承式注释。

**`Join` 的构造与调用约定。** 字段：

- `id: JoinID`
- `_reducer: ReducerFunction`
- `_initial_factory: Callable[[], OutputT]`
- `parent_fork_id: ForkID | None`
- `preferred_parent_fork: Literal['closest', 'farthest']`，**默认 `'farthest'`**

`initial` 与 `initial_factory` 两种形态在 `GraphBuilder.join()` 里被归一：`if initial_factory is UNSET: initial_factory = lambda: initial`。为什么要工厂而不是值？因为同一个 join 在不同 fork run 上是**独立的累加器**，共享一个可变的 list 初值会让两个 run 互相污染。`reduce_list_append` 这类归约器直接 `current.append(...)`，必须每次拿一个新的。

`reduce()` 靠**参数个数**分辨两种归约器：

```
n_parameters = len(inspect.signature(self.reducer).parameters)
if n_parameters == 2:
    return cast(PlainReducerFunction, self.reducer)(current, inputs)
else:
    return cast(ContextReducerFunction, self.reducer)(ctx, current, inputs)
```

这就是 `PlainReducerFunction`（`(current, inputs) -> OutputT`）与 `ContextReducerFunction`（`(ctx, current, inputs) -> OutputT`）两种签名共存的实现方式——**用 `inspect.signature` 在运行期数参数**。这在前面的 `Decision` 与后面的 `Step` 里也反复出现，是这套代码的一个统一手法。

内建归约器一共六个：`reduce_null`（丢弃）、`reduce_list_append`、`reduce_list_extend`、`reduce_dict_update`、`reduce_sum`（通过 `SupportsSum` 协议限定的 `NumericT`）、以及 `ReduceFirstValue`——最后一个不是函数而是一个 `@dataclass` 可调用对象，它 `ctx.cancel_sibling_tasks()` 然后返回 `inputs`，从而实现**早停**。

**`ReducerContext` 的三样东西与一个动作。** 三样是 `state`、`deps`、`_join_state`（都是属性），一个动作是 `cancel_sibling_tasks()`，实现只是 `self._join_state.cancelled_sibling_tasks = True`。它自己不做任何取消——**置位**，取消动作发生在运行器里（见下）。

`_state` / `_deps` / `_join_state` 用下划线字段 + 属性暴露，且 `__init__` 是手写的 `def __init__(self, *, state, deps, join_state)`——类上标了 `@dataclass(init=False)`。这是为了让 `state` / `deps` 的**变型**通过属性表达（与 `StepContext` 完全同一手法，那里注释说「`inputs` 必须是属性才能获得正确的变型行为」）。

**运行期的归约循环。** 前页已引过那段代码，这里只补与「汇合」直接相关的三点：

第一，**归约器按 `(join_id, fork_run_id)` 分配**，首次到达才 `initial_factory()`：

```
join_state = self.active_reducers.get((result.join_id, fork_run_id))
if join_state is None:
    current = join_node.initial_factory()
    join_state = self.active_reducers[(result.join_id, fork_run_id)] = JoinState(current, downstream_fork_stack)
```

第二，**早停是归约完成后立刻检查的**：

```
join_state.current = join_node.reduce(context, join_state.current, result.inputs)
if join_state.cancelled_sibling_tasks:
    await self._cancel_sibling_tasks(parent_fork_id, fork_run_id)
```

`_cancel_sibling_tasks` 遍历 `active_tasks`，凡 fork stack 里含同一 `(fork_id, node_run_id)` 的任务都 `_finish_task(task_id)`——而 `_finish_task` 会 `cancel_scopes.pop(...).cancel()` 并把它从 `active_tasks` 移出。所以早停是**真取消**，不只是「不再等待结果」。

第三，**`fork_run_id` 与 `downstream_fork_stack` 由 `_resolve_join_fork_run` 决定**，那段代码区分了 final join 与 non-final join：

```
for i, x in enumerate(fork_stack[::-1]):
    if x.fork_id == parent_fork_id:
        if self.graph.is_final_join(join_id):
            downstream_fork_stack = fork_stack[: len(fork_stack) - i]
        else:
            downstream_fork_stack = fork_stack
        return x.node_run_id, downstream_fork_stack
```

final join 会**截断**栈（丢掉「直接产出这一项的那个分叉及其更深的分叉」），non-final join 则**原样保留**——注释解释：non-final join 是别的 join 的中间节点，保留栈才能让下游 join 仍关联到同一个 fork run。

**收尾顺序的第二重保险。** 当没有待决任务但仍有活跃归约器时，循环末尾会逐个尝试终结，但有一个 `should_skip` 判断：若该 join 的某个中间 join 也是活跃归约器、且两者**共享同一个父 fork run**（通过比对 `downstream_fork_stack` 里父分叉的 `node_run_id`），就必须先让中间 join 走完。这段逻辑的注释密度远超周围代码，说明它是被真实失败逼出来的。

`Join` 与 `JoinNode` 的关系也值得一提：`JoinNode` 是一个 `BaseNode` 子类，包着 `join` 与 `inputs`，`run` 里永远 `raise NotImplementedError`。它的作用不是被执行，而是**作为返回值告诉运行器「接下来去哪个 join」**——`_handle_node` 里 `isinstance(next_node, JoinNode)` 就返回 `JoinItem(next_node.join.id, next_node.inputs, fork_stack)`。这与 `StepNode` 是同一套手法（b05 详述）。

## 关键取舍

**源码事实：** 分叉被建模成路径上的标记（`BroadcastMarker` / `MapMarker`），并在 `build()` 期间被转成 `Fork` 节点；运行期 `_handle_path` 断言「这些标记不该还留在路径里」。

**源码事实：** `to(a, b)` 是 `broadcast` 的语法糖，两者造出的 `BroadcastMarker` 结构完全相同。

**源码事实：** `initial` 被归一成 `initial_factory`，因为每个 fork run 需要**独立的可变累加器**；共享一个 list 初值会让并发分支互相污染。

**源码事实：** 归约器的两种签名靠 `inspect.signature` 数参数个数区分，而不是靠类型标注或协议检查。

**源码事实：** 早停（`ReduceFirstValue`）只置位 `cancelled_sibling_tasks`，真正的取消在运行器里执行，且取消范围严格限定在「同一父分叉的同一 fork run」。

**源码事实：** `map` 到空可迭代对象时，靠预先声明的 `downstream_join_id` 让下游 join 仍以初值触发。不声明就会静默不触发。

**源码事实：** `map` 不支持多源节点，源码给出 `NotImplementedError` 并诚实解释「出于什么理由不修」。

**源码事实：** final join 会截断 fork stack，non-final join 保留——这条区别决定了嵌套分叉时归约器能否正确关联。

**架构推论：** 把「并行」拆成「构建期声明标记」与「运行期归约状态」两半，使得同一个 join 定义可以在多个并发 run 上重复使用而互不干扰，也让「早停」「空映射」「嵌套分叉」这些边界能各自在独立位置处理。

**架构推论：** `fork_stack` 跟着任务走而不是存在运行器里，是整个并发模型能正确归约的根本。代价是每个任务都要携带一个可能很深的元组，且任何涉及「这个任务属于哪个 fork run」的判断都必须扫这个元组——`_is_fork_run_completed` 与 `_cancel_sibling_tasks` 都是这样的线性扫描。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_graph/pydantic_graph/paths.py` | `Path` | 标记序列 + `last_fork` / `next_path` 两个基本操作 |
| `pydantic_graph/pydantic_graph/paths.py` | `PathBuilder` | 纯函数式累积器：`to` / `broadcast` / `map` / `transform` / `label` |
| `pydantic_graph/pydantic_graph/paths.py` | `BroadcastMarker` | 「同一份数据发往多条子路径」，带 fork_id |
| `pydantic_graph/pydantic_graph/paths.py` | `MapMarker` | 「按可迭代拆分」，带 fork_id 与可选 downstream_join_id |
| `pydantic_graph/pydantic_graph/paths.py` | `EdgePathBuilder.map` | 多源节点下的 `NotImplementedError` 与自承式解释 |
| `pydantic_graph/pydantic_graph/join.py` | `Join` | 归约模板：reducer + initial_factory + parent fork 声明 |
| `pydantic_graph/pydantic_graph/join.py` | `Join.reduce` | 按参数个数分辨 plain / context 两种归约器 |
| `pydantic_graph/pydantic_graph/join.py` | `JoinState` | 单个 `(join, fork run)` 的累加状态与早停标志 |
| `pydantic_graph/pydantic_graph/join.py` | `ReducerContext` | 归约器可见的 state / deps / 早停入口 |
| `pydantic_graph/pydantic_graph/join.py` | `ReduceFirstValue` | 首个值胜出并置早停标志的内建归约器 |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `_handle_fork_edges` | 展开 map/broadcast，空映射时急切建 join 状态 |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `_resolve_join_fork_run` | final / non-final join 的 fork stack 处理差异 |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `_is_fork_run_completed` | 判定某 fork run 是否还有活跃任务卡在中间节点 |

## 参考

- [pydantic-graph 源码：paths.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_graph/pydantic_graph/paths.py)
- [pydantic-graph 源码：join.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_graph/pydantic_graph/join.py)
- [Pydantic Graph 文档：并行与汇合](https://ai.pydantic.dev/graph/#join)
- [Pydantic Graph 文档：map 与 broadcast](https://ai.pydantic.dev/graph/#map-and-broadcast)

## 与主核 / AI 的关系

分叉/汇合与主核里的「并行扫描 + 聚合并发结果」是同构的：主核把一次扫描拆成多个 worker 并行读页，再由上层把结果按顺序聚合。差别在于**归属判定**：主核的并行工作项通常带一个明确的序号或页号，聚合时按它归位；这里的 `fork_stack` 是一个**嵌套结构的路径**，因为它支持任意深度的嵌套分叉，而嵌套意味着「同一次归约可能来自不同层的分叉」，必须靠栈来区分。

对 AI 工程的实际意义在于：并行工具调用是 agent 里最需要并行的部分，但它的正确性难点不在并发本身，而在「什么时候算汇合完成」。`_is_fork_run_completed` 那句「只要还有活跃任务的 fork stack 里含这个 fork_run_id、且该任务停在父分叉的中间节点或 join 自身，就没完成」精确地表达了这条判据——它不是数「到达了几次」，而是看「还有没有人可能到达」。这个视角差别很关键：按次数计数会在「某个分支抛异常提前结束」时永不闭合，而按「是否还有人可能到达」则天然收敛。`ReduceFirstValue` 的早停则从另一侧补上：当已经不需要其他分支时，主动把兄弟任务取消掉，避免白跑。
