---
project: langgraph
feature: pregel-node
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/_read.py
    symbols: [PregelNode, flat_writers, invoke]
---

# PregelNode 的读写编排

## 一句话职责

`PregelNode` 是「一个节点在图上长什么样」的完整声明——读哪些通道、被哪些通道触发、主体是谁、之后往哪些通道写，并把它拼装成一个可直接执行的 runnable。

## 机制是什么

这是一个纯数据容器，但携带的字段决定了执行语义的一半。`channels` 是读集，可以是单个通道名（读到的是值本身）或名字列表（读到的是 dict 子集）；`triggers` 是触发集，任一被写就调度；`bound` 是节点主体（缺省是一个恒等函数）；`writers` 是一串在主体之后执行的 Runnable，负责把主体输出分派到通道；`mapper` 把读到的值转成用户 schema；再加 `retry_policy`、`cache_policy`、`timeout`、`tags`、`metadata`、`error_handler_node`、`subgraphs` 这些策略与元信息。

它自己不会被图当 runnable 直接调用——执行循环把它拆进 `PregelExecutableTask`。真正被调用的是它拼出来的 `node`：`bound` 与 `writers` 串成一条 `RunnableSeq`。

## 怎么实现的

**构造期的归一化。** `retry_policy` 允许传单个实例，构造时统一裹成元组，让下游不必再判类型。`timeout` 经 `coerce_timeout_policy` 归一，把「秒数」「timedelta」「策略对象」三种写法压成同一形态。`bound` 缺省时填 `DEFAULT_BOUND`——一个恒等 lambda 的 `RunnableCallable`，于是「只写不读」的节点（例如 `START`）也能构造出来。

**子图自动发现。** `subgraphs` 未显式给出、且 `bound` 不是缺省时，构造期会尝试 `find_subgraph_pregel(self.bound)` 探测 bound 内部是否嵌了一个 Pregel，探到就记一条。整个探测被 `try/except Exception` 包住且吞掉异常——子图发现只是给流式与追踪用的元信息，探不出来不该让建图失败。

**`flat_writers`。** 这是唯一一处优化：从尾部开始，只要最后两项都是 `ChannelWrite` 就合并——用两者 `writes` 列表拼接出一个新 `ChannelWrite` 替换掉这两项，循环直到无法再合并。合并只发生在**尾部连续**的 `ChannelWrite` 上，因为中间夹一个非 writer 的 runnable 就存在顺序依赖。原列表先 `copy()`，`ChannelWrite` 也是新建的，所以不污染声明。

**`node` 的四分支。** 依据「bound 是不是缺省」与「writers 有几条」组合出四条路径：两者皆空返回 `None`（这类节点不产生可执行任务）；只有一条 writer 时直接返回那一条，省掉序列包装；只有多条 writer 时把它们串成 `RunnableSeq`；有 bound 且有 writers 时串成 `bound` 加全部 writers。`trace_policy` 只在需要序列包装时才挂上。

**`copy`。** 用 `self.__dict__` 展开加更新构造新实例，但先剔除三个 `cached_property` 缓存项。若不剔除，旧缓存会绕过新字段——例如改了 `writers` 却仍复用旧的 `flat_writers`。

**`invoke` 系列。** 四个方法（`invoke`/`ainvoke`/`stream`/`astream`）都不调 `node` 而是直接调 `self.bound`，并把自己 `metadata`/`tags` 通过 `merge_configs` 挂到 config 上。也就是说这四个方法是「直接执行节点主体」的便捷入口，**不写通道**；写通道只在被拆成任务后经 `writers` 发生。这个区分很关键：误用 `invoke` 会得到一个没有落到状态里的结果。

**`input_cache_key`。** 由 `mapper` 与通道集合（列表则转成元组）组成的二元组，用作「同一节点多任务共享读输入」的缓存键。它只在通道集合与 mapper 都相同才命中，因此不会把不同读集的输入混起来。

## 关键取舍

**源码事实：** `PregelNode` 是容器，不实现 `Runnable` 接口。它同时要求 `channels` 与 `triggers`，但两者可以指向不同通道——「读到什么」与「被什么触发」被显式解耦，这是实现 `input_schema` 裁剪与延后节点（下游用不同通道类型触发）的机制基础。

**源码事实：** `flat_writers` 只在尾部合并，且用 `cached_property` 缓存。合并是为了减少 `RunnableSeq` 的层数（每层都带一次 config patch 与回调开销），缓存是因为每个任务构造时都要读它。

**源码事实：** `invoke` 只跑 `bound`，`node` 才跑 `bound` 加 writers。两个入口语义不同却相邻放置，属于容易误用的设计面；好处是调试时可以单独执行节点主体而不触发写入。

**架构推论：** `copy` 必须显式剔除缓存属性，说明「以 `__dict__` 展开构造」这种写法与 `cached_property` 天然冲突。这也是为什么这里选择手动列出三个键名而不是通用处理——`cached_property` 的名字在实例字典里就是名字本身，无法从类型信息反推。

**架构推论：** 子图探测放在构造期而非运行期，是因为流式输出与命名空间需要提前知道结构；代价是构造期要尝试执行一次探测逻辑，且探测失败被静默吞掉，子图信息可能缺失而无任何提示。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/_read.py` | `PregelNode` | 节点声明容器：读集、触发集、主体、写入器与策略 |
| `libs/langgraph/langgraph/pregel/_read.py` | `flat_writers` | 尾部连续 `ChannelWrite` 合并，带缓存 |
| `libs/langgraph/langgraph/pregel/_read.py` | `invoke` | 只执行 `bound`，不触发写入 |
| `libs/langgraph/langgraph/pregel/_read.py` | `node` | 拼装 `bound` 与 writers 的可执行 runnable |
| `libs/langgraph/langgraph/pregel/_algo.py` | `prepare_next_tasks` | 把 `PregelNode` 拆成可执行任务 |

## 参考

- [LangGraph 源码：pregel/_read.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_read.py)
- [LangGraph 源码：pregel/_algo.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_algo.py)
- [LangGraph 文档：节点与超级步](https://langchain-ai.github.io/langgraph/concepts/low_level/#nodes)

## 与主核 / AI 的关系

「声明一份执行描述、再在别处把它编译成可执行计划」与主核把查询树编译成执行器算子链是同一路数：`PregelNode` 对应算子描述，`node` 属性对应编译产物，`PregelExecutableTask` 对应带参数的执行实例。差异在于主核编译时确定数据流向，而这里读写集合在运行期还能被 `Command` 与 `Send` 改写。

对 AI 侧而言，这一页解释了两个常见困惑：为什么 `input_schema` 能给不同节点裁剪读集（因为 `channels` 与 `triggers` 是两个独立字段），以及为什么同一节点可以被并行调度多次却共享一份读输入缓存（`input_cache_key` 只看 mapper 与读集，不看任务身份）。后者在 `Send` 扇出时特别有用，但若 mapper 是有副作用的函数就会出问题。
