---
project: pydantic-ai
feature: step-and-stream
title: 步节点与流式节点
page_type: feature
schema_version: 1
refs:
  - path: pydantic_graph/pydantic_graph/step.py
    symbols: [Step, StepNode, StepContext, NodeStep]
---

# 步节点与流式节点

## 一句话职责

`Step` 把「一个异步函数」包装成图里可执行的一个单元；`StepContext` 是它拿到的三样东西（状态、依赖、输入）；`NodeStep` 是反向的一层包装——让 `BaseNode` 子类也能作为步参与图；`StepNode` 则是「从 `BaseNode.run` 里指回某个步」的返回值信号，本身永远不该被执行。

## 机制是什么

这一层解决的是「**函数型节点与类型节点如何共存于同一张图**」这个接缝问题，因此有四种角色：

- **`StepFunction` / `StreamFunction` 两个协议**，分别描述「异步返回一个值」与「异步返回一个异步迭代器」的函数。
- **`Step`** 是前者的包装物：它有 `id`、`label`，以及一个 `_call`。
- **`NodeStep`** 是 `Step` 的**子类**，但它的 `_call` 不是用户函数，而是内部的 `_call_node`——于是「跑一个 `BaseNode` 实例」被伪装成「跑一个 step」。
- **`StepNode`** 反过来：它是 `BaseNode` 的子类，包着「某个 Step + 已绑定的 inputs」，`run` 永远 `raise NotImplementedError`。它的用途不是执行，而是**当返回值**：`BaseNode.run` 返回它，运行器就翻译成「下一个任务是那个 step」。

用一句话概括这套设计：**`NodeStep` 让类伪装成函数，`StepNode` 让指针伪装成节点。**

`StepContext` 还带来一个语义约束：步函数能看到的只有 `state` / `deps` / `inputs`，与 `GraphRunContext` 是同一份信息的两种封装——后者给 `BaseNode.run` 用，前者给步函数用。两者都**不含**事件总线、取消令牌、追踪句柄。

## 怎么实现的

**`StepContext` 的手写 `__init__`。** 类标 `@dataclass(init=False)`，字段是 `_state` / `_deps` / `_inputs`，然后手写：

```
def __init__(self, *, state: StateT, deps: DepsT, inputs: InputT):
    self._state = state
    self._deps = deps
    self._inputs = inputs
```

三个属性 `state` / `deps` / `inputs` 各自返回对应私有字段。`inputs` 的 docstring 直接给出理由：**「必须是属性，才能获得正确的变型行为」**。这与 `ReducerContext` 完全同一手法——把类型参数放进属性返回注解，是让 Pyright/mypy 推断出期望变型的唯一途径，字段直读拿不到。

注意它是 `kw_only`：三个参数都得用关键字传，调用点因此必须是 `StepContext(state=..., deps=..., inputs=...)`。

**两个协议与一个别名。** `StepFunction` 的 `__call__` 返回 `Awaitable[OutputT]`；`StreamFunction` 的 `__call__` 返回 `AsyncIterator[OutputT]`，方法体是 `raise NotImplementedError` 后跟一个裸 `yield`——那个 `yield` 是必需的，否则函数体不是生成器，`AsyncIterator` 的协议也无法在类型层面成立。`AnyStepFunction = StepFunction[Any, Any, Any, Any]` 只是省去到处写四个 `Any`。

**`Step` 的三字段与两个刻意的形态。** 字段是 `id: NodeID`、`_call`、`label: str | None`；`__init__` 是手写的、**关键字专用**（`*, id, call, label=None`）。`call` 是属性而不是字段，注释同样写着「需要是属性才能正确推断变型」。

`as_node(inputs=None)` 是两个重载 + 一个实现：

```
@overload
def as_node(self, inputs: None = None) -> StepNode[StateT, DepsT]: ...
@overload
def as_node(self, inputs: InputT) -> StepNode[StateT, DepsT]: ...
def as_node(self, inputs: InputT | None = None) -> StepNode[StateT, DepsT]:
    return StepNode(self, inputs)
```

它返回的 `StepNode` 只保留 `StateT` / `DepsT` 两个类型参数——因为 inputs 的具体类型已经被值本身确定，不需要再靠泛型表达。

**`StepNode` 为什么存在。** 它是一个 `@dataclass` 形式的 `BaseNode` 子类，字段 `step: Step[...]` 与 `inputs: Any`，`run` 实体只有一句 `raise NotImplementedError('...it is meant to be returned from a BaseNode subclass to indicate a transition to a builder step.')`。

这个「永远不执行」的节点之所以必要，是因为图运行器的分派需要区分两种「指针」：

```
if isinstance(next_node, StepNode):
    return [GraphTask(next_node.step.id, next_node.inputs, fork_stack, self.get_next_task_id())]
elif isinstance(next_node, JoinNode):
    return JoinItem(next_node.join.id, next_node.inputs, fork_stack)
elif isinstance(next_node, BaseNode):
    node_step = NodeStep(next_node.__class__)
    return [GraphTask(node_step.id, next_node, fork_stack, self.get_next_task_id())]
elif isinstance(next_node, End):
    return EndMarker(next_node.data)
```

也就是说 `BaseNode.run` 的返回值有四种可能形状，而「去哪个 step」与「去哪个类节点」用的是**不同的 id 与不同的 inputs**：前者是 `step.id` + `next_node.inputs`（inputs 是值），后者是 `NodeStep(类).id` + `next_node`（inputs 是节点实例本身）。`StepNode` 与 `JoinNode` 都是在类型层面把「我要去哪」表达清楚，好让运行器不必猜。

**`NodeStep` 是 `Step` 的子类，而不是另一种东西。** 它继承 `Step`，并在 `__init__` 里调 `super().__init__(id=id or NodeID(node_type.get_node_id()), call=self._call_node, label=label)`。三个要点：

1. **id 默认取节点类的 `get_node_id()`**，也就是类名。所以「同一个 `BaseNode` 类在图里只有一个 `NodeStep`」，`g.node(SomeNode)` 与 `_handle_node` 里现造的 `NodeStep(SomeNode)` 会得到同一个 id——这正是同一个类节点在循环里反复执行却能复用同一张图节点的原因。
2. **`_call_node` 是 `StepFunction` 协议的一个实现**：它接收 `StepContext`，从 `ctx.inputs` 取出节点实例，`isinstance(node, self.node_type)` 校验（不匹配就 `ValueError`，且标了 `# pragma: no cover`），然后 `await node.run(GraphRunContext(state=ctx.state, deps=ctx.deps))`——**每次调用都新建一个 `GraphRunContext`**，因此节点拿到新容器、共享同一 `state` 与 `deps`。
3. **`node_type` 做了 `get_origin` 归一**：`self.node_type = get_origin(node_type) or node_type`，注释说明参数可能是个 `typing._GenericAlias`（例如 `UserPromptNode[~DepsT, ~OutputT]`），必须取 origin 才是真类。这与 `DecisionBranchBuilder.to`、`EdgePathBuilder.to` 里的同类处理呼应。

类头上还有一句自承性注释：本可以做成 `frozen` dataclass，但**当前不行**，因为那会在 Python 3.13 上破坏变型推断（`__replace__` 相关），并挂着 mypy 的 issue 编号。

**`stream()` 其实不是一种新节点。** `GraphBuilder.stream` 的实现只有一层包装：

```
async def wrapper(ctx: StepContext[StateT, DepsT, InputT]):
    return call(ctx)

node_id = node_id or get_callable_name(call)
return self.step(call=wrapper, node_id=node_id, label=label)
```

因为 `StreamFunction` 本身是同步调用（`def __call__` 返回 `AsyncIterator`），而 `Step` 要求 `StepFunction`（`def __call__` 返回 `Awaitable`）。所以把一个 async generator 函数包进 `async def wrapper` 后，`await wrapper(ctx)` 得到的正是那个异步迭代器。类型上，`stream` 返回 `Step[StateT, DepsT, InputT, AsyncIterable[OutputT]]`——**它的「输出」是一个异步可迭代对象**，因此可以继续 `.map()`（`PathBuilder.map` 同时接受 `Iterable` 与 `AsyncIterable`）。

这也解释了为什么 `MapMarker` 侧要同时处理同步与异步可迭代：`_handle_fork_edges` 里 `_is_any_iterable(inputs)` 走同步展开、`_is_any_async_iterable(inputs)` 则返回一个 `_GraphTaskAsyncIterable`，让任务按需逐个产出。流式步的输出因此可以被 `map` 成「边流边分叉」。

**运行时的实际调用路径。** `_run_task` 遇到 `Step` 时的完整逻辑：

```
with ExitStack() as stack:
    if self.graph.auto_instrument:
        span_name = f'run node {node.id}'   # 预格式化，非 Logfire 后端才显示成文本
        stack.enter_context(logfire_span(span_name, node_id=node.id, node=node))
    step_context = StepContext[StateT, DepsT, Any](state=state, deps=deps, inputs=inputs)
    output = await node.call(step_context)

if isinstance(node, NodeStep):
    return self._handle_node(output, fork_stack)
else:
    return self._handle_edges(node, output, fork_stack)
```

两处分派是这段代码的全部价值所在：**`NodeStep` 的输出是「下一个节点」**（因此交给 `_handle_node`，那里面再区分 `StepNode` / `JoinNode` / `BaseNode` / `End`）；**普通 `Step` 的输出是「数据」**（因此交给 `_handle_edges`，由路径上的 transform 与目的地决定去哪）。同一句 `await node.call(...)` 在此产生了两种语义，靠 `isinstance` 分开。

`StartNode | Fork` 走的是 `_handle_edges` 而根本不调 `call`（它们没有 `_call`），所以「步」在语义上只覆盖 `Step` 这一支。

**在 agent 循环里，`Step` 几乎不出现——但 `NodeStep` 无处不在。** `_build_agent_graph` 的写法是：

```
g.add(
    g.edge_from(g.start_node).to(UserPromptNode[Any, Any]),
    g.node(UserPromptNode[Any, Any]),
    g.node(ModelRequestNode[Any, Any]),
    g.node(CallToolsNode[Any, Any]),
    g.node(SetFinalResult[Any, Any]),
)
return g.build(validate_graph_structure=False)
```

`g.node(X)` 内部就是 `_node(X, parent_namespace)`：造 `NodeStep(X)`、插进图、再由 `_edge_from_return_hint` 解析 `run` 的返回注解造边。也就是说**四个 agent 节点全部以 `NodeStep` 的形式存在于图里**，`Step`（也就是用户函数式步）在这张图里一个都没用。

这套接缝在「`BaseNode.run` 想调用一个用户步」时才会用到：返回值写成 `Annotated[StepNode[StateT, DepsT], my_step]`，`_edge_from_return_hint` 从注解里取出那个 `Step` 作为目的地；缺少这层 `Annotated` 时它直接抛 `GraphSetupError` 并给出正确写法——因为仅凭 `StepNode` 这个类型无法知道是哪一个步。

## 关键取舍

**源码事实：** `StepContext` / `Step` 都把字段做成私有 + 属性，并在 docstring 里写明「属性是为了正确的变型推断」。这是类型系统约束反过来塑造 API 形态的一处。

**源码事实：** `StepNode.run` 永远抛 `NotImplementedError`，`JoinNode.run` 同样。它们不是节点，是「下一跳」的类型化信号。

**源码事实：** `NodeStep` 是 `Step` 的子类而非平行类型，靠把 `_call` 换成 `_call_node` 让「执行节点实例」有与「执行函数」一致的调用形状。

**源码事实：** `NodeStep` 的 id 默认取被包类的 `get_node_id()`（即类名），因此同一个类在图里只有一个步节点。

**源码事实：** `_call_node` 每次调用都新建 `GraphRunContext`，并做 `isinstance` 校验后才 `await node.run(...)`。

**源码事实：** `stream()` 只是把 `StreamFunction` 包进一个 `async def` 再交给 `step()`；流式在图层不是新节点类型，而是「输出类型为 `AsyncIterable` 的普通步」。

**源码事实：** `NodeStep` 本可 `frozen`，但因 Python 3.13 的 `__replace__` 会破坏变型推断而放弃，源码留有注释与 issue 编号。

**架构推论：** 函数型节点与类型节点共用 `Step` 这一个执行单元，使得运行器的 `_run_task` 只有一支「step」分支；代价是「`NodeStep` 的输出是节点、普通 `Step` 的输出是数据」这条不对称必须靠 `isinstance(node, NodeStep)` 在运行期区分，且没有任何类型层面的帮助。

**架构推论：** 把「跨类型跳转」表达成返回值（`StepNode` / `JoinNode`）而不是图上的边，让 `BaseNode.run` 能凭运行期状态决定下一步，而图结构仍然保持静态可分析。代价是每次这种跳转都要带一层 `Annotated` 注解，否则构建期无法解析到具体的 step 或 join。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_graph/pydantic_graph/step.py` | `StepContext` | 步函数的三元上下文，属性暴露以获得变型 |
| `pydantic_graph/pydantic_graph/step.py` | `StepFunction` / `StreamFunction` | 两种步函数协议：返回 awaitable / 返回异步迭代器 |
| `pydantic_graph/pydantic_graph/step.py` | `Step` | 步的包装物：id、`_call`、label，`call` 为属性 |
| `pydantic_graph/pydantic_graph/step.py` | `Step.as_node` | 把步与已绑定 inputs 包成 `StepNode` 作返回值 |
| `pydantic_graph/pydantic_graph/step.py` | `StepNode` | `BaseNode` 子类，`run` 永不执行，仅作跨类型跳转信号 |
| `pydantic_graph/pydantic_graph/step.py` | `NodeStep` | `Step` 子类，`_call` 换成 `_call_node`，id 默认取类名 |
| `pydantic_graph/pydantic_graph/step.py` | `NodeStep._call_node` | 校验实例类型后新建 `GraphRunContext` 调 `node.run` |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `GraphBuilder.step` | 装饰器/直调两用，造 `Step`；id 默认取函数名 |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `GraphBuilder.stream` | 包一层 `async def` 后复用 `step`，输出类型为 `AsyncIterable` |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `_run_task` | `await node.call(...)` 后按 `NodeStep` 与否分派两条语义 |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `_handle_node` | `StepNode` / `JoinNode` / `BaseNode` / `End` 四种返回形状的翻译点 |

## 参考

- [pydantic-graph 源码：step.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_graph/pydantic_graph/step.py)
- [Pydantic Graph 文档：步函数](https://ai.pydantic.dev/graph/builder/steps/)
- [Pydantic Graph 文档：图与节点](https://ai.pydantic.dev/graph/)
- [pydantic-ai 源码：\_agent_graph.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_agent_graph.py)

## 与主核 / AI 的关系

「用一层包装让两种执行单元看起来相同」这套手法，与主核里「让算子与回调都满足同一个执行签名」是同一种抽象技巧。主核常见的做法是给两种东西都实现同一个 `invoke` 接口，运行器只认接口；这里更进一步——**包装物本身是另一个包装物的子类**（`NodeStep` 继承 `Step`），于是「跑一个节点」在运行器眼里就是「跑一个步」，连分派都不用多写一支。代价是两者输出的语义并不相同，那条不对称只能留到 `await` 之后用 `isinstance` 判断。

对 AI 工程的实际意义在于：agent 循环的价值几乎全部来自 `BaseNode` 这一侧（`UserPromptNode` / `ModelRequestNode` / `CallToolsNode` / `SetFinalResult`），而 `Step` 这一侧在 agent 图里完全没被使用——它服务的是「用户自己画图」的场景。这个事实说明了框架的分层意图：**类节点承载确定性的、需要类型推演的控制流；步函数承载用户自定义的、临时的计算**。当两者需要互通时，`StepNode` / `NodeStep` 这一对反向包装提供了双向通道，但两个方向都要靠 `Annotated` 注解来消歧，这也是这类「静态图 + 动态跳转」设计无法避免的摩擦点。
