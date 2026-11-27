---
project: pydantic-ai
feature: decision-routing
title: 决策节点与分支路由
page_type: feature
schema_version: 1
refs:
  - path: pydantic_graph/pydantic_graph/decision.py
    symbols: [Decision, DecisionBranch, DecisionBranchBuilder]
---

# 决策节点与分支路由

## 一句话职责

`Decision` 是一个**按输入值类型分派**的路由节点：它持有一串有序分支，每条分支声明「我接什么类型的输入」以及「接到之后走哪条路径」，运行期按顺序试匹配，用**第一个命中的分支**。

## 机制是什么

`Decision` 的形态与常规 switch 不同：它的分支键不是常量而是**类型**（`source: TypeOrTypeExpression[SourceT]`），并且当没给 `matches` 谓词时，有一套明确的默认匹配规则：

- `source` 是 `Any` 或 `object` → 永远命中（兜底分支）；
- `source` 是 `Literal[...]` → 值与之一相等即命中，但对 `bool` / `int` 有**专门的消歧**；
- 其他类型 → `isinstance` 判定。

匹配是**顺序的**，第一个命中的分支胜出；全都不命中则运行期抛 `RuntimeError`（不是静默丢弃）。

这套机制的意图从类型参数名就能看出来：`Decision[StateT, DepsT, HandledT]` 的第三个参数 `HandledT` 累积「已被分支覆盖的输入类型」。`_force_handled_contravariant` 那个永远 `raise` 的方法只为让 `HandledT` 呈**逆变**，从而让类型检查器能验「所有可能的输入类型都被覆盖了」——这是本页最值得单独讲的取舍。

## 怎么实现的

**`Decision` 本体极简。** 三个字段：`id: NodeID`、`branches: list[DecisionBranch[Any]]`、`note: str | None`。`branch(branch)` 返回**新的** `Decision`（`branches=self.branches + [branch]`），因此构建过程是纯函数式的，`Decision` 自身不可变（尽管它是 `@dataclass` 而非 frozen）。`id` 在构建期通常是占位符——`GraphBuilder.decision()` 用 `generate_placeholder_node_id('decision')`，`build()` 时再由 `_replace_placeholder_node_ids` 换成稳定 ID。

`_force_handled_contravariant` 的实现是：

```
def _force_handled_contravariant(self, inputs: HandledT) -> Never:
    raise RuntimeError('This method should never be called, it is just defined for typing purposes.')
```

这与 `node.py` 里 `EndNode._force_variance` / `Fork._force_variance` 是同一手法：把一个类型参数放进方法签名以获得变型，方法体永远不执行。

**`DecisionBranch` 的四个字段都是必要的。**

- `source`：期望的输入类型，注释直说它「对穷尽性检查是必需的」。
- `matches`：可选谓词。它的 docstring 把默认匹配规则完整写出来了，包括那条 `bool` / `int` 消歧：**「`bool` 值只匹配 `bool` 字面量，`int` 值只匹配非 `bool` 的」**。
- `path`：命中后要走的路径（可含 transform / map / broadcast / label）。
- `destinations`：路径里 `DestinationMarker` 可能引用到的目的地节点列表。

类 docstring 里有一段很少见的自我说明：**「在当前设计下，`DecisionBranch` 在 `SourceT` 上不变（invariant）是_关键_的」**，并指出测试 `test_decision_no_matching_branch` 里有 `# type: ignore` 作为例子。这解释了为什么这个类没有标 `infer_variance=True`——变型在这里不是风格选择，而是穷尽性检查能不能成立的前提。

**`DecisionBranchBuilder` 的流式接口。** 它持四个私有字段 `_decision` / `_source` / `_matches` / `_path_builder`，并提供与 `PathBuilder` 几乎同名的四个方法：

- `to(destination, *extra, fork_id=None)`：完成分支。它先把 `get_origin(d) or d` 归一（注释解释：`type[BaseNode[...]]` 实际可能是 `pydantic_ai._agent_graph.UserPromptNode[~DepsT, ~OutputT]` 这种泛型别名，要取 origin 才是真类），再把类包成 `NodeStep(d)`、实例原样保留，最后 `self._path_builder.to(*destinations, fork_id=fork_id)`。
- `broadcast(get_forks, fork_id=None)`：调用 `get_forks(self)` 拿到一批分支，把它们的 `path` 汇成 `self._path_builder.broadcast(new_paths, fork_id=fork_id)`；**空结果直接抛 `GraphBuildingError`**（「必须至少返回一条分支」）。
- `transform(func)`：加一个 `TransformMarker`，同时把 `OutputT` 换成新类型。
- `map(...)`：只在该输出可迭代时可用（类型上限定在 `PathBuilder[..., Iterable[T]] | PathBuilder[..., AsyncIterable[T]]`）。
- `label(label)`：仅用于 Mermaid 图。

注意这五个方法各自返回**新的** builder（`DecisionBranchBuilder(decision=self._decision, ...)`），所以链式调用不会改动原对象；而 `_decision` 始终是同一个引用——分支是在最终 `to` / `broadcast` 时由 `GraphBuilder` 收集并挂到 `Decision` 上的。

**运行时匹配的精确实现。** `_handle_decision` 是这段逻辑唯一的地方：

```
for branch in decision.branches:
    match_tester = branch.matches
    if match_tester is not None:
        inputs_match = match_tester(inputs)
    else:
        branch_source = unpack_type_expression(branch.source)
        if branch_source in {Any, object}:
            inputs_match = True
        elif get_origin(branch_source) is Literal:
            literal_values = get_args(branch_source)
            if isinstance(inputs, int):
                literal_values = tuple(
                    v for v in literal_values
                    if not isinstance(v, int) or isinstance(v, bool) == isinstance(inputs, bool)
                )
            inputs_match = inputs in literal_values
        else:
            try:
                inputs_match = isinstance(inputs, branch_source)
            except TypeError as e:
                raise RuntimeError(f'Decision branch source {branch_source} is not a valid type.') from e
    if inputs_match:
        return self._handle_path(branch.path, inputs, fork_stack)
raise RuntimeError(f'No branch matched inputs {inputs} for decision node {decision}.')
```

三个细节值得指出：

第一，`branch_source in {Any, object}` 用的是**集合比较**而不是 `is`——这在 `Any` 与 `object` 是类/特殊对象时能工作，但它同时意味着「任何与 `Any` 相等的东西」都会兜底。

第二，`Literal` 的 `bool`/`int` 消歧是**单向过滤**：只过滤 `literal_values`，不过滤 `inputs`。当 `inputs` 是 `int` 时，保留「非 int 的」或「与 inputs 的 bool 属性一致的」。于是 `inputs=True` 会保留 `Literal[True]` 但剔除 `Literal[1]`；`inputs=1` 会保留 `Literal[1]` 与所有非 `int` 值，剔除 `Literal[True]`。源码注释写得很直白：`True == 1`，但它们是两种不同的选择。

第三，`isinstance` 抛 `TypeError` 时会转成 `RuntimeError` 并指明「分支的 source 不是合法类型」。这让「用了个不能当类型用的对象当分支键」在运行期有清晰报错。

`unpack_type_expression` 这层间接是必需的：`source` 的类型是 `TypeOrTypeExpression[SourceT]`，允许写成字符串形式（前向引用），所以运行期得先解包。

**在 agent 循环里，`Decision` 几乎不出现——但有一处出现得很关键。** `GraphBuilder._edge_from_return_hint` 遇到「返回联合类型里有多个目的地」时，会造一个 `Decision` 并给每个目的地配一条 `self.match(NoneType).to(destination)`：

```
decision = self.decision()
for destination in destinations:
    # We don't actually use this decision mechanism, but we need to build the edges for parent-fork finding
    decision = decision.branch(self.match(NoneType).to(destination))
return edge.to(decision)
```

注释坦白：**这个 `Decision` 的匹配逻辑根本不会被用到**，它存在的唯一目的是「让边图上出现一个分叉点，好让父分叉分析有东西可依附」。这是一个值得警惕的信号——`UserPromptNode.run` 返回 `ModelRequestNode | CallToolsNode`，本该是「按运行期状态二选一」，但实际控制流由 `run()` 内部 `return` 决定，图结构上那个 `Decision` 只是占位。

也就是说：`Decision` 在这套代码里有两种用法——**真用**（用户显式 `decision()` + `match()` 做类型分派）与**占位**（边推导为了图分析而造一个不被执行的 `Decision`）。两者共用同一套类，靠 `match(NoneType)` 这种「永不命中」的分支区分开。

## 关键取舍

**源码事实：** 分支键是类型而不是常量，默认匹配规则是 `Any`/`object` 兜底、`Literal` 相等、其他 `isinstance`。匹配顺序即声明顺序，第一个命中者胜出，无命中抛 `RuntimeError`。

**源码事实：** `Literal` 分支对 `bool` 与 `int` 做了显式消歧（`True == 1` 但语义不同）。

**源码事实：** `DecisionBranch` 在 `SourceT` 上**必须**不变，这是穷尽性类型检查成立的前提，docstring 与测试里的 `# type: ignore` 相互印证。

**源码事实：** `HandledT` 通过一个永不执行的 `_force_handled_contravariant` 方法被强制成逆变，从而让「已覆盖的输入类型」可以累积。

**源码事实：** `broadcast()` 在 `get_forks` 返回空序列时抛 `GraphBuildingError`，而不是造一个空分叉。

**源码事实：** 边推导会为一个多返回类型的节点造 `Decision`，但**不执行它的匹配**（分支用 `match(NoneType)`）。它的存在只为父分叉分析。

**架构推论：** 「类型即路由键」让决策节点的可用性完全取决于类型系统的表达力：能表达 `Any` / `Literal` / 类，就能路由；表达不了（例如「按某字段的取值区间」）就只能写 `matches` 谓词，而谓词一旦出现，穷尽性检查就退化为人工责任。

**架构推论：** 同一个 `Decision` 类同时承担「真路由」与「图分析占位」两种职责，说明这套图模型里「分叉点」这个概念是**结构性**的而非语义性的——分析算法需要图上有分叉，至于分叉是否真的在运行期被求值，是另一回事。这处混用在读代码时最容易误解，因为 `_handle_decision` 的注释与行为只对「真路由」有效。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_graph/pydantic_graph/decision.py` | `Decision` | 路由节点：有序分支列表 + `branch()` 纯函数式追加 |
| `pydantic_graph/pydantic_graph/decision.py` | `Decision._force_handled_contravariant` | 让 `HandledT` 呈逆变，服务于穷尽性检查 |
| `pydantic_graph/pydantic_graph/decision.py` | `DecisionBranch` | 分支：类型键 + 可选谓词 + 路径 + 目的地；必须对 `SourceT` 不变 |
| `pydantic_graph/pydantic_graph/decision.py` | `DecisionBranchBuilder` | 流式构建：`to` / `broadcast` / `transform` / `map` / `label` |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `_handle_decision` | 运行期匹配：谓词优先，否则 `Any`/`object` → `Literal` → `isinstance` |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `GraphBuilder.decision` | 造一个无分支的 `Decision`，ID 为占位符 |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `GraphBuilder.match` | 造分支构建器，`source` + 可选 `matches` |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `GraphBuilder.match_node` | 快捷方式：直接为 `BaseNode` 子类造一条分支 |
| `pydantic_graph/pydantic_graph/graph_builder.py` | `_edge_from_return_hint` | 多返回类型时造占位 `Decision`，仅服务父分叉分析 |

## 参考

- [pydantic-graph 源码：decision.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_graph/pydantic_graph/decision.py)
- [Pydantic Graph 文档：条件分支](https://ai.pydantic.dev/graph/#decisions)
- [Pydantic Graph 文档：图与节点](https://ai.pydantic.dev/graph/)
- [pydantic-ai 源码：\_agent_graph.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_agent_graph.py)

## 与主核 / AI 的关系

「按输入类型分派」与主核里的「按表达式类型选择算子实现」是同一类路由，但主核通常靠**多态分发或显式类型标签**（tagged union），而不是 `isinstance` 链。这里选 `isinstance` 的收益是零配置（类型就是键），代价是分派成本随分支数线性增长，且「子类是否该匹配父类分支」这类细节只能靠顺序约定。`Literal` 的 `bool`/`int` 消歧正是这套代价的直接体现：`isinstance(True, int)` 为真，若不特判，`Literal[1]` 分支会吃掉所有布尔输入。

对 AI 工程的实际意义在于：agent 的控制流很少能用「值类型」表达——真正需要决定的是「本轮有了工具调用就继续、有了最终输出就结束」，这取决于模型返回的**内容结构**而不是类型。所以这套 `Decision` 在 agent 循环里主要作为图分析的占位物存在，真正的分流写在 `run()` 的 `return` 里。这个事实提醒一件事：**当框架同时支持「图上的显式分支」与「代码里的隐式返回」时，读懂控制流不能只看图，必须读节点的 `run`**。
