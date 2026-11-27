---
project: langgraph
feature: stategraph-builder
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/graph/state.py
    symbols: [StateGraph, _add_schema, _all_edges]
---

# StateGraph 构建器与状态模式注册

## 一句话职责

StateGraph 把「若干个状态 schema + 节点 + 边」的描述，在构造期编译成一张通道表与节点表，图本身不执行任何逻辑。

## 机制是什么

`StateGraph` 是一个纯构造期对象。它在 `__init__` 里注册三份 schema——state、input、output，其中 input 与 output 缺省时退化成 state——并为每份 schema 建立一份「键 → 通道」映射。节点、边、条件分支分别落在 `nodes`、`edges`/`waiting_edges`、`branches` 三个容器里，等 `compile()` 时才真正装配成可执行的 Pregel。

关键点在于**通道不是用户显式声明的**，而是从 schema 的类型注解推导出来的：`_get_channels` 读 `get_type_hints(schema, include_extras=True)`，逐个注解调用 `_get_channel`，把注解翻译成对应的通道实例。所以「状态里有几个键」直接决定「图上有几个通道」。

## 怎么实现的

**schema 注册。** `_add_schema` 做了三件事：把 schema 记进 `self.schemas`；把推导出的通道按「先到先得、同键异类报错」的规则并进 `self.channels`；把托管值并进 `self.managed`。它对 input/output schema 用 `allow_managed=False` 调用，一旦发现托管值就直接抛 `ValueError`——因为托管值（剩余步数、是否最后一步）只能由框架在运行时注入，让用户从外部输入里塞进去毫无意义。

同键异类的处理有一处刻意的例外：若新通道是 `LastValue`，源码选择静默放过，而不是报错。这样 graph 用 `TypedDict` 声明 state、用普通类型声明 input 时不会因为「都是 LastValue 但类型参数不同」而失败。

**通道推导。** `_get_channel` 按优先级依次尝试三条路：先看注解是不是托管值（`_is_field_managed_value`）；再看 `Annotated` 元数据里有没有用户塞进去的 `BaseChannel` 实例；再看元数据里有没有 reducer 函数。三条都不命中，就退化成 `LastValue(annotation)`，即「覆盖写」语义。这个退化是默认行为，也是绝大多数状态键的实际通道类型。

`_get_channels` 里还有一处特殊分支：schema 若没有 `__annotations__`（例如直接传一个非类型对象），整份 schema 只产生一个以 `ROOT` 为键的通道。这让「把整个输入当成一个值」这种最简用法也能成立。

**边表。** `_all_edges` 是一个只读属性，把普通边（`edges`）与多起点等待边（`waiting_edges` 里的 `(starts, end)` 展开成若干个二元组）合并成一个统一的二元组集合。它被 `validate()` 用来一次性检查所有起点与终点是否都存在，避免在多处重复遍历两种边。

## 关键取舍

**源码事实：** 通道从注解推导，不是显式声明。好处是状态与通道不会脱节，坏处是「某个键用什么合并语义」藏在类型注解里，读代码时必须同时看注解。

**源码事实：** input/output schema 禁止出现托管值，state schema 允许。这个非对称是有意的：托管值代表运行期状态，不该出现在对外输入输出契约中。

**源码事实：** 同键异类通道默认报错，但 `LastValue` 例外放行。这个例外让「图内部用带类型的 state、对外用普通类型」成为可行写法。

**架构推论：** StateGraph 自身不持有任何执行状态，全部编译产物在 `compile()` 里生成。因此同一个 builder 可以被多次编译，也可以作为子图嵌进另一张图；代价是编译期的校验负担更重，`validate()` 必须在编译前把结构性错误全部拦住。

**架构推论：** 把 schema 到通道的翻译放在构建期而非运行期，使运行时只需按通道表读写，不必再做类型判断；这也是 Pregel 执行循环能保持「只认通道」这一简洁模型的前提。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/graph/state.py` | `StateGraph.__init__` | 初始化容器并注册三份 schema |
| `libs/langgraph/langgraph/graph/state.py` | `_add_schema` | 合并 schema 的通道与托管值，含冲突判定 |
| `libs/langgraph/langgraph/graph/state.py` | `_get_channels` | 从类型注解推导通道、托管值与类型提示 |
| `libs/langgraph/langgraph/graph/state.py` | `_get_channel` | 单键的通道推导优先级 |
| `libs/langgraph/langgraph/graph/state.py` | `_all_edges` | 普通边与等待边的统一视图 |

## 参考

- [LangGraph 源码：graph/state.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/graph/state.py)
- [LangGraph 文档：Graph API](https://langchain-ai.github.io/langgraph/concepts/low_level/)
- [LangGraph 文档：使用状态 schema](https://langchain-ai.github.io/langgraph/how-tos/state-model/)

## 与主核 / AI 的关系

`_get_channels` 这套「类型注解 → 运行时数据结构」的翻译，与主核把表定义翻译成物理布局是同构问题：都是把声明期的静态描述固化成一串运行期可索引的槽位。差别在于主核的槽位由定长元组构成、必须处理对齐与变长字段，这里的通道是按名索引的字典条目。

对 AI 侧而言，这一页解释了为什么 agent 的状态字段能「带合并语义」：合并语义不是运行时判断出来的，而是在建图时就被注解固定。若要给 agent 增加新的状态合并方式，正确做法是新增通道类型并让注解指向它，而不是在执行循环里加分支。
