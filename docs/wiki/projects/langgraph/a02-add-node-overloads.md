---
project: langgraph
feature: add-node-overloads
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/graph/state.py
    symbols: [add_node]
  - path: libs/langgraph/langgraph/graph/_node.py
    symbols: [_Node, _NodeWithConfig, _NodeWithWriterStore]
---

# add_node 的重载与节点包装

## 一句话职责

add_node 把用户给的「函数或 Runnable」连同各种可选策略，包装成一条 `StateNodeSpec` 记录，图上从此只认这条记录，不再关心原始函数的签名差异。

## 机制是什么

用户写节点时有十几种合法签名：只收 state；加 config；加 `writer` 关键字；加 `store` 关键字；三者任意组合；或者直接给一个带 `runtime` 参数的函数；或者干脆传个 `Runnable`。`add_node` 用五个 `@overload` 把这些形式都收进同一个方法名下，再在唯一的实现体里归一成 `StateNodeSpec`。

类型层面用 `_Node`、`_NodeWithConfig`、`_NodeWithWriter`、`_NodeWithStore`、`_NodeWithConfigWriterStore` 等一组 `Protocol` 描述每种签名，最后合成 `StateNode` 这个联合类型。所以「支持哪些签名」在类型检查期就是确定的，不是运行期 try/except 试出来的。

## 怎么实现的

**节点命名。** `add_node` 有两种调用形态：只给一个参数时，该参数同时充当节点和名字，名字由 `_get_node_name` 取 `__name__`，取不到就退到类名；给两个参数时，第一个是显式名字，第二个是节点本体。后一种形态是为了让两个 lambda 或两个同名函数能共存——`add_sequence` 里就用这条规则给出明确报错文案。

**输入 schema。** 每个节点可以带自己的 `input_schema`。缺省时节点读到的是图级 state schema 的全部通道；给了就会在 `attach_node` 里按该 schema 的通道集合重新算一份读通道列表。这条设计使得「图上不同节点从不同切片读状态」不需要在函数体里手写裁剪逻辑。

**策略挂载。** `add_node` 接受的 `retry_policy`、`cache_policy`、`timeout`、`error_handler`、`trace_policy`、`metadata` 全部原样存进 `StateNodeSpec`，在 `compile()` 期间与 builder 级默认值合并，逐节点值优先。其中 `timeout` 会先经 `coerce_timeout_policy` 归一，让「数字秒数」「timedelta」「TimeoutPolicy 实例」三种写法变成同一形态。

**defer 与 destinations。** `defer=True` 表示该节点延后到运行即将结束时才执行，它的下游触发通道要用 `LastValueAfterFinish` 而不是 `EphemeralValue`——这个差异落实在 `attach_node` 里。`destinations` 只用于图渲染，源码注释明确写了「不影响执行」，因为真正的跳转由节点返回的 `Command` 决定。

**错误处理器节点。** 带 `error_handler` 的节点在 `compile()` 期间会额外生成一个错误处理器节点，其名字由固定前缀加节点名构成；若用户自己恰好用了这个名字，会直接抛 `ValueError` 而不是静默覆盖。

## 关键取舍

**源码事实：** 节点签名用 `Protocol` 联合表达，而不是运行期反射探测。好处是类型检查器能提前发现「函数参数名写错」；代价是新增一种注入参数就必须同步补 `Protocol` 与 `StateNode` 联合，属于显式扩展。

**源码事实：** 单参数形态自动取函数名做节点名，双参数形态强制显式名字。这使最常见的写法最短，同时给「同名节点」留了逃生口。

**源码事实：** `defer` 改变的是下游触发通道类型，不是节点自身的执行方式。也就是说「延后」这个语义被编码在通道可见性上，而不是在调度器里加一个特判分支。

**架构推论：** 把策略全部存进 `StateNodeSpec` 而不到节点函数上，使得同一个可调用对象能被两个策略不同的节点复用；这也是 `_node_defaults` 能够只做「逐节点值优先」合并的前提。

**架构推论：** `destinations` 明确不参与执行，意味着图的「可视拓扑」与「实际跳转」可以是两套：用 `Command` 动态跳转时，渲染图只能靠 `destinations` 提示，无法保证与运行期一致。这是有意的取舍，让动态路由不被静态拓扑限制。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/graph/state.py` | `add_node` | 五个重载与唯一实现体，归一成节点记录 |
| `libs/langgraph/langgraph/graph/state.py` | `_get_node_name` | 从可调用对象推导节点名 |
| `libs/langgraph/langgraph/graph/_node.py` | `_Node` | 最简签名：只收 state |
| `libs/langgraph/langgraph/graph/_node.py` | `_NodeWithConfig` | 收 state 与 config |
| `libs/langgraph/langgraph/graph/_node.py` | `_NodeWithWriterStore` | 同时收 writer 与 store 的签名 |
| `libs/langgraph/langgraph/graph/_node.py` | `StateNodeSpec` | 节点记录：runnable、策略、ends、defer |

## 参考

- [LangGraph 源码：graph/state.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/graph/state.py)
- [LangGraph 源码：graph/_node.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/graph/_node.py)
- [LangGraph 文档：节点与边](https://langchain-ai.github.io/langgraph/concepts/low_level/#nodes)

## 与主核 / AI 的关系

节点签名的多重形态与主核的「同一操作多种入口」是同一种工程问题：都希望用户在最短写法下拿到常用能力，又不因此让内核多出运行期分支。这里选择在类型层展开、在实现体归一，与主核用统一入口函数吞掉多个调用形式、再分派到内部路径的思路一致。

对 agent 而言，`writer` 与 `store` 的注入边界值得注意：流式写入与外部存储都是横切能力，把它们做成节点参数而不是全局单例，使同一进程里不同图、不同 tenant 的数据不会串。这一点在把 agent 嵌入多用户服务时是必须的。
