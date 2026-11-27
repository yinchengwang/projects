---
project: pydantic-ai
feature: node-contract
title: 节点契约与运行上下文
page_type: feature
schema_version: 1
refs:
  - path: pydantic_graph/pydantic_graph/basenode.py
    symbols: [BaseNode, GraphRunContext, End, Edge]
  - path: pydantic_graph/pydantic_graph/node.py
    symbols: [StartNode, EndNode, Fork]
---

# 节点契约与运行上下文

## 一句话职责

`BaseNode` 用一句 `async def run(ctx) -> BaseNode | End` 定义了「图里一个节点是什么」，`GraphRunContext` 只带 `state` 与 `deps` 两样东西，`End` 是唯一的终止信号；`StartNode` / `EndNode` / `Fork` 则是三种**没有 `run` 方法的合成节点**，由运行器特判。

## 机制是什么

契约的全部内容不超过 40 行代码，但它承担了三件事：

一是**执行签名**。节点的 `run` 是 async、唯一参数是 `GraphRunContext`、返回值是「另一个节点」或 `End`。没有「返回 None 表示结束」这种省略——终止必须显式。

二是**声明控制流**。`run` 的**返回类型注解**在运行时被读取（docstring 里专门加了一条 note 强调这点），用来推导出「这个节点之后能走哪些节点」，并在运行图时被强制。所以返回注解不是文档，是数据。

三是**隔离状态与依赖**。`GraphRunContext` 只有 `state`（可变的图状态）与 `deps`（只读依赖）两个字段，`kw_only=True`。它被逐节点传下去，节点能改 `ctx.state` 但不能替换它；状态共享因此是显式的。

`Edge` 则完全是另一类东西：一个 `frozen` 的 dataclass，只有一个 `label: str | None`，docstring 说它是「给图中一条边贴标签的注解」。它不参与执行，只参与渲染与调试。

## 怎么实现的

**`BaseNode` 的最小面。** 两个成员：抽象方法 `run`，以及类方法 `get_node_id()`：

```
@classmethod
@cache
def get_node_id(cls) -> str:
    return cls.__name__
```

`@cache` 加在类方法上，等于「每个节点类只算一次类名」。这个 ID 是节点在图里的键——`Graph.nodes` 用 `NodeID` 索引，而 `NodeStep(node_type)` 默认 `id=NodeID(node_type.get_node_id())`，所以**类名就是节点身份**。这解释了为什么两个同名但不同模块的节点类会冲突（`GraphBuilder.add_reachable` 的 docstring 里就列出了这条会抛 `GraphBuildingError`）。

`run` 的契约里有个容易被忽略的细节：返回类型用 `BaseNode[StateT, DepsT, Any] | End[NodeRunEndT]`，其中 `Any` 与 `NodeRunEndT` 的位置是有意义的——后继节点可以是任意 run-end 类型，而只有 `End` 携带本节点的 `NodeRunEndT`。这个设计让「终态类型」沿 `End` 单线传播，不被中间节点的类型参数污染。

**四个 TypeVar 的默认值与变型。** `basenode.py` 顶部：

- `StateT = TypeVar('StateT', default=object)`——不写状态类型时默认 `object`；
- `RunEndT = TypeVar('RunEndT', covariant=True, default=object)`——图的返回值类型，协变；
- `NodeRunEndT = TypeVar('NodeRunEndT', covariant=True, default=Never)`——**单个节点**的终态类型，默认 `Never`；
- `DepsT = TypeVar('DepsT', default=object, contravariant=True)`——依赖，逆变。

`NodeRunEndT` 默认 `Never` 是个关键的巧思：不声明终态的节点无法返回 `End`（因为 `End[Never]` 没有可用的 `data`），于是「随手写个节点却不说明它怎么结束」在类型层就被拦住了。

**`GraphRunContext` 的克制。** 字段只有 `state: StateT` 与 `deps: DepsT`。没有 event bus、没有 run id、没有取消 token——全部不在这里。这不是遗漏：图层的语义应当只有「状态 + 依赖」，把运行期设施（事件、取消、追踪）留在上层，图才能被独立测试与复用。

**`End` 与 `Edge` 的形态差异。** `End` 是 `@dataclass`（可变）且只有一个 `data: RunEndT`；`Edge` 是 `@dataclass(frozen=True)` 且只有 `label`。两者都是「载荷容器」，但 `Edge` 被冻结，因为它只作为注解挂在边上，不该在运行期被改动。

**三种合成节点。** `node.py` 里三个类都不继承 `BaseNode`、也都没有 `run`：

- `StartNode(Generic[OutputT])`：只有一个类属性 `id = NodeID('__start__')`。它在图里充当「接收图输入的那一端」，`GraphBuilder.__init__` 造一个实例、`_build_agent_graph` 里 `g.edge_from(g.start_node).to(UserPromptNode[Any, Any])`。
- `EndNode(Generic[InputT])`：`id = NodeID('__end__')`，同样无字段。它接收最终值，`_run_task` 里 `isinstance(node, EndNode)` 就直接 `return EndMarker(inputs)`——**进 End 就等于出结果**。
- `@dataclass class Fork(Generic[InputT, OutputT])`：带三个字段 `id: ForkID`、`is_map: bool`、`downstream_join_id: JoinID | None`。它是唯一带数据的合成节点，因为分叉需要知道自己是不是 map、以及空映射时要跳到哪个 join。

`EndNode` 与 `Fork` 都写了一个 `_force_variance` 方法，docstring 直说「只为类型检查存在，永远不该被调用」，方法体是 `raise RuntimeError`，且标了 `# pragma: no cover`。这是 Python 泛型变型推断的一个真实妥协：为了让 `InputT` / `OutputT` 出现在方法签名里以获得正确的变型，只能写一个谁都不会调用的方法。

**两种节点如何被运行器区分。** `_run_task` 的分派是 `isinstance` 链：

```
if isinstance(node, StartNode | Fork):   return self._handle_edges(node, inputs, fork_stack)
elif isinstance(node, Step):             ...
elif isinstance(node, Join):             return JoinItem(node_id, inputs, fork_stack)
elif isinstance(node, Decision):         return self._handle_decision(node, inputs, fork_stack)
elif isinstance(node, EndNode):          return EndMarker(inputs)
else:                                    assert_never(node)
```

所以 `BaseNode` 的实例**永远不会直接进 `_run_task`**——它总是被 `NodeStep` 包一层。`_handle_node` 里那一步是接合点：

```
elif isinstance(next_node, BaseNode):
    node_step = NodeStep(next_node.__class__)
    return [GraphTask(node_step.id, next_node, fork_stack, self.get_next_task_id())]
```

即「`run()` 返回了一个 `BaseNode` 实例」被翻译成「下一个任务是这个类的 `NodeStep`，输入是那个实例本身」。`NodeStep._call_node` 再校验 `isinstance(node, self.node_type)` 后 `await node.run(GraphRunContext(state=ctx.state, deps=ctx.deps))`——**每一跳都新建一个 `GraphRunContext`**，所以节点拿到的是新容器、但 `state` 与 `deps` 是同一对象。

**这套契约怎么支撑 agent 循环。** `_agent_graph.py` 里 `AgentNode` 就是一层薄基类：

```
class AgentNode(BaseNode[GraphAgentState, GraphAgentDeps[DepsT, Any], result.FinalResult[NodeRunEndT]]):
    """The base class for all agent nodes.

    Using subclass of `BaseNode` for all nodes reduces the amount of boilerplate of generics everywhere
    """
```

它把三个类型参数预先绑定，于是具体节点只需要写 `class ModelRequestNode(AgentNode[DepsT, NodeRunEndT])`。四个具体节点的 `run` 返回注解就是它们的边声明：

- `UserPromptNode.run` → `ModelRequestNode | CallToolsNode`
- `ModelRequestNode.run` → `CallToolsNode | ModelRequestNode | SetFinalResult`
- `CallToolsNode.run` → `ModelRequestNode | End[FinalResult[...]]`
- `SetFinalResult.run` → `End[FinalResult[...]]`（返回方才 `End(self.final_result)`）

图构建时 `g.node(ModelRequestNode[Any, Any])` 逐个把注解变成边，`End` 自动连到 `end_node`。这就是为什么 agent 循环**没有一处显式的 `add_edge`**——它的控制流完全写在四个类的返回类型里。而 `SetFinalResult` 存在的唯一理由就是「有一个节点明确只返回 `End`」，让「本步结束整个 run」在类型上可见。

## 关键取舍

**源码事实：** 节点的 `run` 返回注解在**运行时**被读取并用于推导边。类型注解因此既是静态契约又是运行时数据，两套语义必须保持一致。

**源码事实：** `get_node_id()` 返回 `cls.__name__` 并带 `@cache`。类名即节点 ID，所以「两个不同模块的同名节点类」会撞 ID。

**源码事实：** `NodeRunEndT` 默认是 `Never`，于是未声明终态的节点类型上无法返回 `End`。这是把「你必须说明怎么结束」写进类型系统。

**源码事实：** `GraphRunContext` 只有 `state` 与 `deps`。事件、取消、追踪一概不在图这一层。

**源码事实：** `Edge` 被 `frozen=True` 且只有 `label`，不参与执行。它的用途是渲染（Mermaid 图）与调试标签。

**源码事实：** `StartNode` / `EndNode` / `Fork` 都**不是** `BaseNode` 子类，由 `_run_task` 的 `isinstance` 链特判。`Fork` 是三者中唯一带字段的，因为 map 与空映射需要额外信息。

**源码事实：** `EndNode` 与 `Fork` 各自有一个永远只 `raise RuntimeError` 的 `_force_variance` 方法，只为让泛型参数出现在签名里以得到正确变型。

**架构推论：** 「返回类型即边」让图的控制流可被静态阅读与部分静态验证，代价是**动态返回必须走 `Decision`**，以及注解写错（例如把 `StepNode` 的返回写成裸 `End`）会在构建期才暴露。

**架构推论：** 把 `state` 与 `deps` 收进一个两字段的上下文，等于给所有节点统一了「可写状态 + 只读依赖」的权限模型。这让「哪些东西可被节点修改」在签名层面就是明确的，也让节点之间无法通过上下文偷偷传私有变量（要么进 state，要么进 deps）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_graph/pydantic_graph/basenode.py` | `BaseNode` | 节点契约：异步 `run` + 类名即 ID |
| `pydantic_graph/pydantic_graph/basenode.py` | `BaseNode.run` | 唯一抽象方法，返回注解在运行期定义边 |
| `pydantic_graph/pydantic_graph/basenode.py` | `GraphRunContext` | 两字段上下文：可变状态 + 只读依赖，`kw_only` |
| `pydantic_graph/pydantic_graph/basenode.py` | `End` | 唯一终止信号，携带图的返回数据 |
| `pydantic_graph/pydantic_graph/basenode.py` | `Edge` | 冻结的边标签，仅用于渲染与调试 |
| `pydantic_graph/pydantic_graph/node.py` | `StartNode` | `__start__` 锚点，接收图输入 |
| `pydantic_graph/pydantic_graph/node.py` | `EndNode` | `__end__` 锚点，进入即产出结果 |
| `pydantic_graph/pydantic_graph/node.py` | `Fork` | 唯一带字段的合成节点：map 标志与下游 join |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `_handle_node` | `run()` 返回 `BaseNode` 实例 → 下一个 `NodeStep` 任务的翻译点 |
| `pydantic_ai_slim/pydantic_ai/_agent_graph.py` | `AgentNode` | 预绑定三个类型参数的基类；四个子类的返回注解就是 agent 循环的边 |

## 参考

- [pydantic-graph 源码：basenode.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_graph/pydantic_graph/basenode.py)
- [pydantic-graph 源码：node.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_graph/pydantic_graph/node.py)
- [Pydantic Graph 文档：节点](https://ai.pydantic.dev/graph/#nodes)
- [Pydantic Graph 文档：状态与依赖](https://ai.pydantic.dev/graph/#graph-state)

## 与主核 / AI 的关系

「执行单元 = 一个带异步入口的类，入口的返回类型决定下一步」这个契约，与主核里「算子 = 一个带 `next` 指针的执行函数」是同一思路的两种实现。主核靠显式指针（`next`、`Plan` 里的索引）表达控制流，这里靠类型注解——后者的好处是可静态检查、可自动渲染成图，坏处是**运行时必须重新解析类型注解**，于是注解写错不再是「文档不准」而是「图构建失败或边缺失」。

对 AI 工程的实际意义在于：`GraphRunContext` 只有 `state` 与 `deps` 这一克制，直接决定了 agent 循环能被做成「纯状态迁移」。`_agent_graph` 里的 `GraphAgentState`（消息历史、用量、待决工具调用、规划状态）可以整体序列化，这正是耐久执行（Temporal / DBOS / Prefect）能够重放 agent run 的前提——**如果上下文里塞了活的连接或事件总线，这套重放就不成立**。所以这条看似保守的接口约束，实际上是整个可持久化能力的地基。
