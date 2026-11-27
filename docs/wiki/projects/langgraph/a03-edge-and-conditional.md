---
project: langgraph
feature: edge-and-conditional
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/graph/state.py
    symbols: [add_edge, add_conditional_edges]
---

# 静态边与条件边登记

## 一句话职责

add_edge 与 add_conditional_edges 都只是往图上登记「谁触发谁」的意图，真正的触发链在编译期才被翻译成通道订阅与写入。

## 机制是什么

边有两种。**静态边**是一条确定的边：从某个节点（或多个节点）到某个目标。**条件边**是一条「由一个可调用对象在运行期决定目标」的边。两者在 builder 里落在不同的容器：静态边走 `edges`（单起点）或 `waiting_edges`（多起点），条件边走 `branches[source][name]`。

这两个容器都不直接参与执行。执行期的触发完全由通道决定：编译期会为每个节点分配一个触发通道，把上游节点的写入接到这个通道上。所以「边」在运行时其实是「某通道被谁写、被谁订阅」。

## 怎么实现的

**单起点静态边。** `add_edge` 先做三处校验：`END` 不能作为起点（否则图永远结束不了）、`START` 不能作为终点（入口是虚拟节点，不能当出口）；只有非 `StateGraph` 的图会额外检查「同一节点是否已有出边」，因为普通图没有多值通道来承接并发写。校验通过就把 `(start, end)` 加进 `edges` 集合。

**多起点静态边。** 起点给列表时走另一条分支：每个起点必须已存在，终点必须已存在或是 `END`，然后把 `(tuple(start_key), end_key)` 放进 `waiting_edges`。这条边表达的是「等所有起点都完成后才走」的会合语义，编译期会为它生成一个 `NamedBarrierValue` 通道——这就是为什么它不能简化成多条单起点边。

**条件边。** `add_conditional_edges` 先把 `path` 用 `coerce_to_runnable` 归一成 `Runnable`，名字取 `path.name`，缺省叫 `condition`。同名分支在同一源节点下重复登记会直接报错。随后构造 `BranchSpec`，若推断出了输入 schema 就顺带把该 schema 注册进图的通道表——因为分支函数读的字段也必须有通道才能被读到。

`path_map` 有三种形态并被 `BranchSpec.from_path` 归一成字典：给字典就复制；给列表就变成 `{name: name}`；都不给就尝试从 `path` 的返回类型注解里抠 `Literal` 的字面量集合。第三种是「零配置」路径，代价是依赖类型注解，注解缺失时 `ends` 为 `None`，运行期就不再校验目标合法性。

**START 与 END 的落地。** 编译期 `attach_edge` 处理单起点边时，`START` 节点本身没有上游，它靠 `triggers=[START]` 在图启动时被触发一次；而终点为 `END` 时**不写任何通道**——因为「结束」不是一次写入，而是「没有新的待跑任务」这个事实。多起点边如果没有终点通道可写，会创建一个形如拼接的会合通道名，让下游节点订阅它。

## 关键取舍

**源码事实：** 单起点边与多起点边落在两个不同容器，因为前者不需要额外通道、后者必须靠屏障通道才能表达「等齐」。把两者放进同一集合会让编译期无法区分要不要建屏障。

**源码事实：** 条件边的 `path_map` 可以是字典、列表或从 `Literal` 注解推断。推断失败不报错，只在渲染图时把所有节点都当作可能目标——`add_conditional_edges` 的 docstring 明确警告了这一点。

**源码事实：** `END` 作为终点时不产生写入。这意味着图终止是「没有可触发任务」的自然结果，而不是一次显式信号。

**架构推论：** 边只是声明、通道才是执行机制，带来一个直接后果：动态跳转（`Command`）可以绕过静态边，只要它写对了目标通道。这解释了为什么 `destinations` 参数只需要服务于图渲染。

**架构推论：** 多起点会合用屏障通道而不是「计数器」，使会合语义与通道的 `is_available` 判定统一起来——下游节点只要看通道是否可用，不需要感知自己有几个上游。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/graph/state.py` | `add_edge` | 登记单起点边或等待边，含 END/START 校验 |
| `libs/langgraph/langgraph/graph/state.py` | `add_conditional_edges` | 登记分支，构造 BranchSpec 并注册其输入 schema |
| `libs/langgraph/langgraph/graph/state.py` | `attach_edge` | 编译期把边翻译成通道订阅与写入，多起点建屏障 |
| `libs/langgraph/langgraph/graph/state.py` | `attach_branch` | 编译期把分支挂成写节点，含读通道与写映射 |
| `libs/langgraph/langgraph/graph/state.py` | `_all_edges` | 静态边与等待边的统一视图，供 validate 使用 |

## 参考

- [LangGraph 源码：graph/state.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/graph/state.py)
- [LangGraph 文档：边](https://langchain-ai.github.io/langgraph/concepts/low_level/#edges)
- [LangGraph 文档：多起点边与并行分支](https://langchain-ai.github.io/langgraph/how-tos/branching/)

## 与主核 / AI 的关系

「声明与实际触发分离」在主核里对应的是查询计划与执行算子的关系：计划只描述数据流向，真正跑起来靠算子之间的数据依赖。这里把同样的分层用在 agent 图上，使「图长什么样」和「这一轮跑哪些节点」可以分别推理。

对 AI 侧的实际影响是：多 agent 的会合点必须显式声明多起点边，否则会出现「某个分支还没写完下游就跑了」的早触发。这类 bug 在 agent 编排里很难复现，因为依赖 LLM 的响应时序；把会合语义固定在屏障通道上，至少让结构性问题能在建图期表达清楚。
