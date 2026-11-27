---
project: langgraph
feature: channel-read
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/_read.py
    symbols: [ChannelRead, do_read]
---

# ChannelRead 读通道节点

## 一句话职责

`ChannelRead` 把「从当前状态里取一组通道的值」包装成一个 Runnable，使条件边既能作为普通 runnable 被调度、又能被以静态函数的形式直接调用。

## 机制是什么

业务代码想读状态，但状态并不在它手里：通道映射由执行循环持有，节点函数拿到的是快照后的 dict。中间需要一层间接——配置里注入了一个读函数 `CONFIG_KEY_READ`，谁拿着 config 就能读。`ChannelRead` 就是这层间接的可复用封装：它自己不带状态，只保存「读哪些通道」「是否要看到本步写入」「读完用什么 mapper 转换」三件事，真正读的动作发生时从 config 里取出读函数。

关键在于 `fresh`。条件边是在某节点执行完之后、写入尚未合并进通道之前被调用的。默认读的是合并前的通道状态，看不到刚刚那个节点的输出；`fresh=True` 才能看到。这个差异由注入的读函数实现（`local_read` 会按需复制通道、把当前任务的写入先应用上去），`ChannelRead` 只负责把标志透传下去。

## 怎么实现的

**构造与命名。** `__init__` 把 `_read`/`_aread` 两个同步/异步体挂进 `RunnableCallable`，并显式传 `name=None` 与 `trace=False`——名字交给 `get_name` 覆写，追踪默认关闭，因为这些读操作不该在 trace 里各占一个 run。`get_name` 按通道名生成 `ChannelRead<...>`，多通道时用逗号连接；外部若给了显式名字则优先（源码里是一个空 `if name: pass` 分支的写法，意图就是「有就用，不要覆盖」）。

**同步与异步同体。** `_read` 与 `_aread` 的实现逐字相同，都只是转发到静态方法。因为读是纯内存访问，没有真正的 IO，异步版本无需额外逻辑；两者都保留只是为了满足 `RunnableCallable` 要求同时提供 sync/async 实现的契约。

**`do_read` 的三步。** 一是从 `config[CONF][CONFIG_KEY_READ]` 取读函数，取不到抛 `RuntimeError`，错误文案指向「不在 Pregel 进程上下文里」。这个 `except KeyError` 而不是 `get` 是有意的：把「拼错了 config 结构」和「压根没有读函数」区分开——键存在但值为 None 时会在调用处炸，带的是另一条栈。

二是调用 `read(select, fresh)`，把选择集与新鲜度一起交出去。

三是有 mapper 就先 `mapper(read(...))`，无 mapper 直接返回。`mapper` 的典型用途是把通道 dict 强制转成用户声明的 schema 类（Pydantic 模型或 dataclass），使条件边函数拿到的是带属性的对象而不是裸字典。

**静态调用的意义。** `do_read` 是 `@staticmethod`，因此 `graph/state.py` 里的 `attach_branch` 可以 `partial(ChannelRead.do_read, select=..., fresh=True, mapper=...)` 造一个裸 callable 直接塞给分支，而不必构造 `ChannelRead` 实例。这让「分支读取」这条热路径少一层对象，也让 `ChannelRead` 自身退化成「可被调度的读节点」这一个用途。

## 关键取舍

**源码事实：** 读函数藏在 config 的 configurable 里，而不是作为构造参数直接持有。这样 `ChannelRead` 实例可以被复制、被多个任务共享、被序列化进图定义，而实际读的总是「当前这一步」的状态。

**源码事实：** `fresh` 只是一个布尔标志，具体语义完全由注入的读函数解释。`ChannelRead` 不知道也不检查「fresh 读是否需要合并当前任务写入」，这层知识留在 `local_read` 里——因为只有它同时持有通道映射与任务写入。

**源码事实：** 名字里包含被读通道列表，因此 trace 视图上能直接看出「这次读涉及哪些通道」；代价是通道集合不同的两个读节点必然不同名，无法用固定名字去定位。

**架构推论：** `trace=False` 是必要的：条件边每次求值都要读一次，若各自产生一个 trace run，图的事件流会被读操作淹没。代价是排查「分支为什么选了这条路」时看不到读到了什么，只能靠分支自身的返回值。

**架构推论：** 因为 `do_read` 同时服务实例路径与 `partial` 路径，它必须对 `select` 的两种形态（单字符串、字符串列表）都成立——单字符串在通道侧是「直接取值」，列表是「取 dict 子集」。这个双形态约定是读写两侧共同遵守的基础，`map_input` 与 `map_output_values` 都一样。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/_read.py` | `ChannelRead` | 读通道的 Runnable 封装，持 `fresh` 与 `mapper` |
| `libs/langgraph/langgraph/pregel/_read.py` | `do_read` | 从 config 取读函数并调用，支持静态调用 |
| `libs/langgraph/langgraph/pregel/_algo.py` | `local_read` | 真正的读实现，按 `fresh` 决定是否并入当前任务写入 |
| `libs/langgraph/langgraph/graph/state.py` | `attach_branch` | 用 `partial` 把 `do_read` 配成分支的读取器 |
| `libs/langgraph/langgraph/_internal/_constants.py` | `CONFIG_KEY_READ` | 读函数的注入键 |

## 参考

- [LangGraph 源码：pregel/_read.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_read.py)
- [LangGraph 源码：pregel/_algo.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_algo.py)
- [LangGraph 文档：条件边](https://langchain-ai.github.io/langgraph/concepts/low_level/#conditional-edges)

## 与主核 / AI 的关系

「调用方不持有数据，只持有句柄，实际读取时向当前上下文要」这一模式与主核里算子不直接抓页、而是通过执行器提供的读接口取元组是同构的：都是为了在并发执行下让读取始终绑定当前事务/当前步的一致视图。这里的一致性单位是超级步，主核是事务。

对 AI 侧而言，`fresh` 的取舍是最容易踩的坑：条件边默认看不到节点本步的输出，因此「节点返回了新的标记位、边却按旧状态选路」是常见误判。要按新状态路由，必须走 `fresh` 读取路径，而这条路径拿到的是一份复制通道上的临时视图，不是真的把写入合并进了全局状态。
