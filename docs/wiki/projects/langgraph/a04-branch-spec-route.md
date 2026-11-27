---
project: langgraph
feature: branch-spec-route
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/graph/_branch.py
    symbols: [BranchSpec, _route, _aroute]
---

# 分支规格与路由求值

## 一句话职责

BranchSpec 把「一个返回目标名的可调用对象」包装成能在图里跑的路由节点：读输入、求值、把目标名翻译成通道写入。

## 机制是什么

条件边在 builder 里只是登记，运行期真正干活的是 `BranchSpec.run` 产出的那个 `RunnableCallable`。它做三段：先从读通道取出分支函数该看到的输入（必要时与节点的输入合流），再调用用户的 `path` 得到目标，最后把目标名经 `writer` 翻译成写入包。

`BranchSpec` 是 `NamedTuple`，只存三样东西：`path`（路由可调用对象）、`ends`（目标名到节点名的映射，可为 `None`）、`input_schema`（分支自己声明的输入 schema）。三样都是构造期确定的，运行期不再变形。

## 怎么实现的

**目标集合的三种来源。** `from_path` 负责把用户在 `add_conditional_edges` 里给的 `path_map` 归一成字典。给字典直接复制；给列表变成「名字映射到自身」；都不给则尝试从 `path` 的返回类型注解里抠 `Literal` 的字面量。第三条走的是 `get_type_hints` 加 `get_origin`/`get_args`，一旦 `path` 是包了多层的 `RunnableLambda`，还要先剥出真正的函数对象再取注解。整段被 `try/except Exception` 包住——推断失败是允许的，此时 `ends` 保持 `None`。

**输入 schema 的推断。** `_get_branch_path_input_schema` 只在显式要求推断时调用。它的做法是：剥出可调用对象、取 `get_type_hints`、拿签名的第一个参数名、用该名字查注解；只有「注解本身是 type 且它自己也有类型提示」才算有效 schema。这条限制很实际——若第一个参数注解是普通 `dict`，`get_type_hints(dict)` 为空，就不会被误认成 schema。

**读输入。** `_route` 与 `_aroute` 是对称的同步/异步实现。给了 `reader` 就从读通道取值；取值结果若是字典、节点输入也是字典、且分支没声明自己的 `input_schema`，就把两者合并——这样分支函数既能看到节点返回的字段，也能看到它在读通道上订阅的字段。没给 `reader` 就直接用节点输入。

**求值并翻译。** 路由结果被 `_finish` 归一成列表（单值也包成单元素列表）。有 `ends` 时按映射把结果翻成节点名，结果元素本身是 `Send` 就保留；没有 `ends` 就把结果原样当节点名用。随后做两处校验：目标不能是 `None` 或 `START`（这两者都意味着分支没给出合法去向），`Send` 的目标不能是 `END`。最后交给 `writer` 生成写入条目。

**顺带复用 `ChannelWrite`。** 若写入条目里有依赖当前节点输入的透传值，`_finish` 返回一个 `ChannelWrite` 节点让框架继续跑下去；否则直接调用 `ChannelWrite.do_write` 落写入，并原样返回输入。这个分支让「分支只是选路」这个常见情形少一次调度。

## 关键取舍

**源码事实：** 目标集合可以从 `Literal` 注解推断，且推断失败被静默吞掉。好处是窄场景零配置，坏处是注解写错时不会报错，只在渲染图时表现为「可能去任何节点」。

**源码事实：** 读输入时把节点输入与读通道值合并，仅当分支没声明 `input_schema`。一旦显式声明 schema，就严格只用 schema 的通道，不再合流——这是「显式优先」的一贯做法。

**源码事实：** 分支求值后对目标做了两次前置校验（非 None/START、Send 不到 END），但**不校验目标节点是否存在**。存在性检查在编译期由 `validate()` 完成，运行期只拦「不可能合法」的形态。

**架构推论：** `BranchSpec` 用 `NamedTuple` 而非 dataclass，说明它是纯数据、不参与继承与扩展；分支的可变行为全在 `run` 生成的那个闭包里，符合「规格是数据、执行是闭包」的分工。

**架构推论：** 尽量用 `ChannelWrite.do_write` 直接落写入而不是返回新节点，是为了让「只选路不产数据」的分支不额外占一轮调度——但一旦需要透传当前输入，就必须回到节点形态，因为写入要依赖本轮输入。这条分界线决定了分支实现的复杂度。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/graph/_branch.py` | `BranchSpec` | 分支规格：path、ends、input_schema |
| `libs/langgraph/langgraph/graph/_branch.py` | `BranchSpec.from_path` | 归一 path_map，推断输入 schema |
| `libs/langgraph/langgraph/graph/_branch.py` | `BranchSpec.run` | 生成带读写闭包的路由节点 |
| `libs/langgraph/langgraph/graph/_branch.py` | `_route` | 同步求值并翻译目标 |
| `libs/langgraph/langgraph/graph/_branch.py` | `_aroute` | 异步求值路径，与同步实现对称 |
| `libs/langgraph/langgraph/graph/_branch.py` | `_get_branch_path_input_schema` | 从首个参数注解里推断分支输入 schema |

## 参考

- [LangGraph 源码：graph/_branch.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/graph/_branch.py)
- [LangGraph 文档：条件边](https://langchain-ai.github.io/langgraph/concepts/low_level/#conditional-edges)
- [LangGraph 文档：Command 与动态路由](https://langchain-ai.github.io/langgraph/concepts/low_level/#command)

## 与主核 / AI 的关系

「按运行期算出的名字去选下一个执行单元」在主核里对应的是查询计划的动态裁剪：计划算子要依据统计信息决定走哪条路径。差别在于主核在编译期就把可选路径穷举成算子枚举，而这里允许名字完全由用户函数决定，代价是运行期无法静态保证目标合法。

对 agent 最重要的是 `Send` 分支：它让一次路由能派发出多个并行子任务，且每个子任务可以携带独立输入。这是 map-reduce 式 agent 编排的基础；也正因为如此，`Send` 到 `END` 被显式禁止——派发到终止点没有语义。
