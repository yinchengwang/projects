---
project: pydantic-ai
feature: graph-builder
title: 图构建与运行入口
page_type: feature
schema_version: 1
refs:
  - path: pydantic_graph/pydantic_graph/graph_builder.py
    symbols: [Graph, GraphRun, GraphTask]
---

# 图构建与运行入口

## 一句话职责

`GraphBuilder` 把「节点 + 边 + 分叉/汇合」攒成一张**不可变可执行的图**（`Graph`），`Graph` 提供 `run` / `run_sync` / `iter` 三个入口，`iter` 产出的 `GraphRun` 是一次运行的驱动器——它以 `GraphTask` 为单位把节点丢进 anyio task group 并发执行，并用 `next()` 把控制权交还给调用方。

## 机制是什么

这一层的核心抽象只有三个词：

**节点是一张 ID 表。** `Graph` 持有 `nodes: dict[NodeID, AnyNode]` 与 `edges_by_source: dict[NodeID, list[Path]]`。节点不是被「连起来」的，而是「每个 ID 有一份出边路径列表」。运行时按 ID 查表，再把路径展开成任务。

**任务是执行单位。** `GraphTaskRequest` 只带 `node_id`、`inputs`、`fork_stack` 三样；`GraphTask` 在它之上加一个 `task_id`。**分叉上下文（`fork_stack`）跟着任务走**，而不是存在运行器的某个全局变量里——这是让并发分支彼此不干扰的关键。

**运行是一个协程间的握手。** `GraphRun` 内部跑一个 `_GraphIterator` 的异步生成器；调用方每次 `next()`，生成器向前推进一格，产出「下一批任务」或「结束标记」。所有节点真正执行在 anyio task group 里，结果通过一条内存对象流汇聚回生成器。

这个「生成器 + 任务组 + 流」的三件套，比直接 `while` 循环多做了一件事：**它允许调用方在任意两次 yield 之间介入**（改下一批任务、或直接 `EndMarker` 提前结束）。`override_next` 与节点级钩子都靠这个口子实现。

## 怎么实现的

**`GraphBuilder` 的累积状态。** 字段是 `_nodes`、`_edges_by_source`（`defaultdict(list)`）、`_decision_index`、`_start_node` / `_end_node` 两个固定实例。`start_node` / `end_node` 是属性，返回固定实例——`StartNode.id` 是 `'__start__'`、`EndNode.id` 是 `'__end__'`，这两个固定 ID 是整张图的两个锚点（结构校验里会明确检查「有出边」「有入边」）。

builder 上更值得注意的是**边不是手写的，多数是推导出来的**。`_infer_step_edges` 对每个 `Step` 目的地做：

1. `get_type_hints(destination.call, localns=parent_namespace, include_extras=True)` 取 `return` 注解；
2. `_edge_from_return_hint` 把注解展开成目的地集合。

`_edge_from_return_hint` 的处理分支本身就说明了「返回类型即控制流」这条设计：

- `End` → 连到 `self.end_node`；
- 裸 `BaseNode`（不带具体子类）→ 抛 `GraphSetupError`，消息直接说明「边推导要求逐个列出可能返回的 `BaseNode` 子类」；
- `StepNode` → 从 `Annotated[...]` 里找 `Step` 注解，找不到就报错并给出正确写法 `Annotated[StepNode[StateT, DepsT], my_step]`；
- `JoinNode` → 同理找 `Join` 注解；
- `BaseNode` 子类 → `NodeStep(return_type)`。

最后一步很有意思：若推导出的目的地数量**少于**联合类型成员数量（说明有返回类型不是节点），就直接 `return None`——**放弃推导而不是报错**，把这条边留给用户显式声明。若多于一个目的地，它造一个 `Decision` 并给每个目的地加一条 `match(NoneType).to(destination)` 的分支，注释坦白说「我们其实不使用这个决策机制，只是需要为找父分叉建边」。

**`build()` 的四个变换 + 两次分析。** `build` 不是简单包装，它按固定顺序改造：

1. `_replace_placeholder_node_ids`——`match()` / `decision()` / map / broadcast 造出来的 ID 都是占位符（`generate_placeholder_node_id('decision')` 之类），这一步换成稳定 ID。
2. `_flatten_paths`——把路径摊平。
3. `_normalize_forks`——规范化分叉。
4. `_validate_graph_structure`（可关）——五条结构检查。
5. `_collect_dominating_forks`——为每个 join 找出「支配它的分叉」，产出 `parent_forks`。
6. `_compute_intermediate_join_nodes`——为每个 join 算出「它到父分叉之间的其他 join」集合。

第 5、6 步的产物直接变成 `Graph` 的字段 `parent_forks` 与 `intermediate_join_nodes`，而 `intermediate_join_nodes` 的注释说明了用途：判定哪些 join 是「final」（中间没有别的 join），以及哪些 join 向下游推进时要**保留 fork stack**。

**结构校验的五条**（`_validate_graph_structure`）：有从 start 出去的边、有进 end 的边、除 end 外没有死端节点、end 从 start 可达、所有节点从 start 可达。docstring 自己承认前几条与后几条冗余，但保留它们是为了将来支持「更细粒度校验」；并且每条错误消息都附带同一句提示：想故意违反就传 `validate_graph_structure=False`。判定「死端」时对 `Decision` 有特例——`has_edges = has_edges or len(node.branches) > 0`。

**`Graph.run` 的循环写法。** 它没用 `async for`，而是显式 `next(event)` 循环：

```
event: Any = None
while True:
    try:
        event = await graph_run.next(event)
    except StopAsyncIteration:
        assert isinstance(event, EndMarker)
        return cast(EndMarker[OutputT], event).value
```

注释解释了为什么：这样能把 `next` 方法也放到关键路径上被测试，避免它出问题却没人发现。这个选择有代价——`run()` 的返回依赖「`StopAsyncIteration` 抛出时 `event` 已经是 `EndMarker`」这个断言。

`iter()` 是 `@asynccontextmanager`：推断名字（`depth=3`，因为 `asynccontextmanager` 多了一层栈帧）、按 `auto_instrument` 决定是否开 span（span 名**预格式化成** `f'run graph {self.name}'`，注释指向 issue 说明原因：非 Logfire 的 OTel 后端会显示字面模板）、取 traceparent，然后 `async with GraphRun(...)`。

**`GraphRun` 的构造与首任务。** 构造函数建立了运行的初始姿态：`_active_reducers`（活跃归约器表，键是 `(JoinID, NodeRunID)`）、`_next`（待处理项）、两个 `itertools.count()` 造的 ID 生成器、**初始 fork stack**（`(ForkStackItem(StartNode.id, node_run_id, 0),)` 一项）、`_first_task`、task group、以及 `_GraphIterator` 实例与其生成器。

注释里有一处防环说明：ID 生成器用 `lambda` 而不是绑定方法，因为 `_GraphIterator` 持有它们，而绑定方法会反向引用 `GraphRun` 形成环。

`next()` 的实现有一处对生成器协议的照护：

```
if self._next is None:
    await anext(self)   # 防止向刚启动的生成器 send 非 None
if value is not None:
    self._set_next(value)
return await anext(self)
```

即：首次调用必须先推进一格，之后才允许 send。`_set_next` 把 `EndMarker` 直接存，把任务请求序列转成 `GraphTask`（复用已有任务的 id，见 `GraphTask.from_request` 的注释「已经是任务了就别再铸一个 id」）。

`__anext__` 遇到 `ErrorMarker` **抛出原始错误**，但 `_next` 里留着它——所以调用方可以在下一次迭代前用 `override_next` 改道，否则错误会被重新抛出。`next_task` 属性在 `_next` 为空时返回 `[self._first_task]`，这就是「还没开始跑时能看一眼第一个任务」的来源。

**`_GraphIterator.iter_graph` 是真正的引擎。** 它的骨架是「发首任务 → 循环消费结果流」：

```
self.active_tasks[first_task.task_id] = first_task
self._handle_execution_request([first_task])
async with self.iter_stream_receiver:
    while self.active_tasks or self.active_reducers:
        async for task_result in self.iter_stream_receiver:
            ...
```

`_handle_execution_request` 做两件事：把任务登记进 `active_tasks`，然后对每个任务 `task_group.start_soon(self._run_tracked_task, task)`——**节点并发执行**就是这么来的。

结果流里的每个 `_GraphTaskResult` 按类型分流：

- 有 `error`：`yield ErrorMarker(...)` 而不是直接抛，注释说这是为了让调用方通过 `on_node_run_error` 恢复；**如果调用方把 ErrorMarker 原样 echo 回来，才真正 raise**。
- `JoinItem`：不 yield，直接进归约逻辑。
- 其他（`EndMarker` 或任务序列）：yield 给调用方，等它回送。

回送值的三分支决定了整个控制流：`EndMarker` → 收尾（`_finish_task`，若还有活跃任务就 `task_group.cancel_scope.cancel()`，然后 `return`）；`JoinItem` → 走归约；序列 → 登记新任务。

归约那段（`JoinItem` 分支）是分叉/汇合的核心，值得逐句读：

```
parent_fork_id = self.graph.get_parent_fork(result.join_id).fork_id
fork_run_id, downstream_fork_stack = self._resolve_join_fork_run(result.join_id, result.fork_stack)
join_node = self.graph.nodes[result.join_id]
join_state = self.active_reducers.get((result.join_id, fork_run_id))
if join_state is None:
    current = join_node.initial_factory()
    join_state = self.active_reducers[(result.join_id, fork_run_id)] = JoinState(current, downstream_fork_stack)
context = ReducerContext(state=self.state, deps=self.deps, join_state=join_state)
join_state.current = join_node.reduce(context, join_state.current, result.inputs)
if join_state.cancelled_sibling_tasks:
    await self._cancel_sibling_tasks(parent_fork_id, fork_run_id)
```

即：**同一个 join 的同一个 fork run 共用一个 `JoinState`**（首次到达时用 `initial_factory()` 起手），每次到达都 `reduce` 累加；若归约器要求早停（`cancelled_sibling_tasks`），立刻取消同分叉的兄弟任务。

之后 `_get_completed_fork_runs` 判定哪些归约器可以收尾——条件是「该 fork run 不再有活跃任务卡在父分叉的中间节点或 join 自身」（`_is_fork_run_completed`）。收尾时 `pop` 掉归约器，用 `_handle_non_fork_edges(join_node, join_state.current, join_state.downstream_fork_stack)` 造后续任务。

**循环末尾的双重保险。** `if not self.active_tasks: break` 跳出内层；随后 `if self.active_reducers:` 做一次「无待决任务时的归约器终结」。这段终结逻辑相当长，原因是**中间 join 的顺序**：若 join J2 的中间 join J1 共享同一个父 fork run，必须**先终结 J1**（它可能产出喂给 J2 的项）。实现方式是比对两边 `downstream_fork_stack` 里父分叉的 `node_run_id` 是否相同，相同就 `should_skip`。这段代码的注释密度明显高于周围，说明它是真实踩过的坑。

若循环正常退出但没产出结果，抛 `RuntimeError('Graph run completed, but no result was produced...')`，文案直接承认「这是图或运行器的 bug」。

**任务的执行与错误隔离。** `_run_tracked_task` 用 `CancelScope()` 记录每个任务的取消作用域（`cancel_scopes[task_id]`），并且**把错误塞进流而不是让它们冒进 task group**——注释解释了为什么：冒进去会被 anyio 改造成 `CancelledError` 或 `ExceptionGroup`，丢失原始异常类型。它还处理了「流的发送端已被拆掉」的两种情况（`BrokenResourceError` / `ClosedResourceError`），注释说明这是 run 拆除时另一个任务正在 `send` 的竞态，属于良性。

`_run_task` 才是按节点类型分派的地方：

- `StartNode | Fork` → `_handle_edges`（不消耗输入，直接按边展开）；
- `Step` → 开 span、造 `StepContext(state, deps, inputs)`、`await node.call(step_context)`；若是 `NodeStep` 就 `_handle_node(output, ...)`（输出本身是个 `BaseNode` 或 `End`），否则 `_handle_edges(node, output, ...)`（普通步的返回值直接喂给边）；
- `Join` → 返回 `JoinItem(node_id, inputs, fork_stack)`（**不执行，只登记**）；
- `Decision` → `_handle_decision`；
- `EndNode` → `EndMarker(inputs)`。

**取消的路径。** 两条：`_finish_task` 会 `cancel_scopes.pop(...).cancel()`，`_cancel_sibling_tasks` 遍历 `active_tasks` 找出 fork stack 里含同一 `(fork_id, node_run_id)` 的任务并逐个 `_finish_task`。`GeneratorExit` 被单独 `except` 后取消整个 task group 并 `return`——调用方提前关掉生成器不会留下孤儿任务。

## 关键取舍

**源码事实：** 节点的出边存在 `edges_by_source` 里，绝大多数由**返回类型注解推导**（`_infer_step_edges` + `_edge_from_return_hint`）；推导不全时返回 `None` 静默放弃，只有「裸 `BaseNode`」「`StepNode` 缺 `Step` 注解」这类无法处理的形态才报错。

**源码事实：** 多返回类型的边推导会造一个 `Decision` 但**不走它的匹配逻辑**，只为了让「找父分叉」有边可依。这是「为一个内部算法服务而存在的结构」。

**源码事实：** `build()` 里 `_replace_placeholder_node_ids` → `_flatten_paths` → `_normalize_forks` 三步是**必须按序**的规范化，之后才做结构校验与两次分叉分析。顺序错了校验会看到中间态。

**源码事实：** 运行时用「异步生成器 + anyio task group + 内存对象流」而不是简单循环，为的是把控制权在每次 yield 处交还调用方，从而实现 `override_next` 与节点级钩子的介入。

**源码事实：** 首任务用一个固定的 `StartNode` fork stack 项起手，此后每个分叉都往栈上**追加**一项；`fork_stack` 随任务走，使并发分支的归约归属可以精确判定。

**源码事实：** 归约器按 `(JoinID, NodeRunID)` 分配，因此同一个 join 在不同 fork run 上是**独立的累加器**；同一 run 内多次到达才共享。

**源码事实：** 结构校验的每条错误都带「想故意违反请传 `validate_graph_structure=False`」。这是把「严格默认 + 明确逃生口」写进错误消息。

**架构推论：** 「返回注解即控制流」让图的定义接近纯类型声明——节点不需要显式 `add_edge`，编译器能看到的返回类型就是运行时会走的边。代价是动态返回（例如根据数据决定下一个节点）必须兜到 `Decision` 或手写边，而推导失败又只静默放弃，容易变成「图能跑但边少了一条」的隐性问题（由结构校验兜住）。

**架构推论：** 把 run 做成可被外部逐步驱动的迭代器，而不是一次跑完的函数，是为了让上层（`AgentRun`）能在节点前后插入能力钩子、替换节点、甚至提前结束。这解释了为什么 `GraphRun` 要暴露 `override_next`、`next_task`、`output` 这些小面——它们不是给最终用户用的，是给**包一层的编排器**用的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_graph/pydantic_graph/graph_builder.py` | `Graph` | 可执行图：节点表、出边表、父分叉与中间 join 分析结果 |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `Graph.run` | 显式 `next()` 循环驱动，以 `EndMarker` 作返回值 |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `Graph.iter` | 开 span、取 traceparent，产出 `GraphRun` |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `GraphBuilder._edge_from_return_hint` | 从返回注解推导目的地，含 `StepNode`/`JoinNode` 的注解约定 |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `GraphBuilder.build` | 三步规范化 + 结构校验 + 父分叉与中间 join 分析 |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `GraphRun` | 单次运行驱动器：首任务、ID 生成、`next`/`override_next` |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `GraphTask` | 执行单位：节点 ID + 输入 + fork stack + 任务 ID |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `_GraphIterator.iter_graph` | 引擎主体：任务并发、结果流消费、归约收尾顺序 |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `_run_tracked_task` | 错误经流传递而非冒进 task group，保留原始异常类型 |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `_handle_fork_edges` | map/broadcast 展开成带 fork stack 的并行任务 |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `_is_fork_run_completed` | 判定某个 fork run 的归约是否可以收尾 |

## 参考

- [pydantic-graph 源码：graph_builder.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_graph/pydantic_graph/graph_builder.py)
- [Pydantic Graph 文档：图的概念](https://ai.pydantic.dev/graph/)
- [Pydantic AI 文档：Agent 与图的关系](https://ai.pydantic.dev/agent/)
- [pydantic-ai 源码：\_agent_graph.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_agent_graph.py)

## 与主核 / AI 的关系

这张图与主核里的「执行计划」是同构的：都是一张节点 + 边的有向图，节点是可执行单元，边决定下一步。差别在**边的来源**——主核的计划通常由优化器**构造**出来（成本估算决定顺序），而这里的边优先从**类型注解推导**，等于让静态类型承担了图的声明职责。这带来一个主核里没有的好处：图的形状可以被类型检查器部分验证（返回 `End` 的节点一定终结、返回 `BaseNode` 子类的节点一定接得上），也让重构（重命名节点类）能被类型检查捕获。

对 AI 工程的实际意义在于：agent 的执行循环本身就是一张图，而这张图的**运行时**必须支持「中途改道」。`override_next` 这个接口看起来不起眼，但它是所有节点级钩子（`before_node_run` 替换节点、`on_node_run_error` 恢复错误、`after_node_run` 把 `End` 变回节点）能工作的物理基础——因为这些钩子都在 `AgentRun.next(node)` 里、在图推进**之外**执行，产物需要写回图运行器的 `_next`。理解了这一点，就能理解为什么 `pydantic_graph` 的 `GraphRun` 要把 `_set_next` 暴露成 `override_next`，而不只是内部实现细节。
