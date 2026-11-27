---
project: langgraph
feature: runtime-object
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/runtime.py
    symbols: [Runtime, ExecutionInfo, RunControl]
---

# Runtime 与执行信息

## 一句话职责

Runtime 把「这次运行能碰到的一切外部依赖」收成一个冻结的 dataclass，随配置下发到每个任务；节点用参数注入取它，不必自己去翻 `config["configurable"]`。

## 机制是什么

`Runtime` 有七个字段：`context`（调用方给的静态依赖，比如 `user_id`、`db_conn`）、`store`（跨 thread 的长期存储）、`stream_writer`（自定义流写入函数）、`heartbeat`（空闲超时的手动进展信号）、`previous`（functional API 里上一轮的返回值）、`execution_info`（本次执行的位置信息）、`server_info`（LangGraph Server 注入的元数据）、`control`（协作式 drain 的控制面）。

它是冻结的（`frozen=True, slots=True`），所以「改运行时」只能通过 `override()` 造新实例或 `merge()` 合并两个实例——不存在原地修改，也就没有跨任务的可见性竞争。

`ExecutionInfo` 是另一个冻结 dataclass，回答「我现在在哪」：`checkpoint_id`、`checkpoint_ns`、`task_id`、`thread_id`、`run_id`、`node_attempt`（第几次尝试，1 起）、`node_first_attempt_time`。`RunControl` 是唯一一个可变的：里面只有一个 `_drain_reason` 字符串。

## 怎么实现的

**组装点。** 一次 run 开始时（同步与异步各一处），`Pregel` 先取父 runtime（`_coerce_parent_runtime` 会把一个只带 `context`/`store` 的外部对象降级成 `Runtime(context=...)`），用它算出 `server_info`，然后新建一个 Runtime：`context` 来自 `_coerce_context`，`store` 来自参数，`stream_writer` 来自本次流配置，`control` 按「显式传入 → 父 runtime 的 → 新建一个」的顺序取。最后 `parent_runtime.merge(runtime)` 把两层叠起来，写回 `config[CONF][CONFIG_KEY_RUNTIME]`。

`merge` 的规则是「other 有值就盖，没值就留 self」，但两个哨兵函数例外：`stream_writer` 与 `heartbeat` 必须比对是不是那个 no-op 默认值，因为函数没法用真假判断。这是 `merge` 里唯一需要特殊逻辑的地方。

**逐任务填充。** `prepare_next_tasks` 在构造每个任务时，把 runtime 再 `override` 一次：`previous` 取自检查点的 PREVIOUS 通道，`store` 取自本次运行的 store，`execution_info` 用 `checkpoint["id"]`、任务自己的检查点命名空间、`task_id`、thread id、run id 现造一个。这个 runtime 放进任务的 `configurable`，节点被调用时从那里取。

**重试时的补齐与递增。** 重试层进来先 `_ensure_execution_info`：分布式运行时里任务由服务端准备、在执行器里反序列化，绕过了 `_algo` 里那段创建 `ExecutionInfo` 的代码，所以这里要从配置键兜底补一个。之后每轮循环用 `patch_execution_info(node_attempt=attempts + 1)` 递增尝试序号——1 是首次，重试后递增。

**`node_first_attempt_time` 为什么单独记。** 它只在进入重试循环之前记一次，之后每轮都保留。这样错误处理器与观测端能算出「这个节点总共纠缠了多久」，而不是只看到最后一次尝试的耗时。

**`RunControl` 的线程安全性。** 注释直接说明：drain 请求由单次属性写入表示，所以不需要锁。同时也预先声明了约束——若将来往这个类里加可变状态，就必须补同步。

**取值入口。** `get_runtime` 就是一行：从 `get_config()[CONF]` 里取 `CONFIG_KEY_RUNTIME`。它没有独立的上下文管理器，源码里的 TODO 也承认「理想情况下 runtime 应该有独立于 config 的上下文管理」，但现在还挂在配置上。

## 关键取舍

**源码事实：** Runtime 不含 `config`。docstring 明确写着：要拿 `RunnableConfig` 就在节点签名里直接声明 `config` 参数，或用 `get_config()`。这让 Runtime 保持「依赖容器」的定位，而不是变成配置的万能入口。

**源码事实：** `context` 按「other 有值就盖」合并，而 `store` 用 `or`。两者区别在于 `context` 可能是任何对象而 `store` 可能是 `None`——`or` 对 `None` 的语义恰好就是想要的。这种逐字段手写合并而不是自动反射，说明每个字段的合并语义都是被单独考虑过的。

**源码事实：** `previous` 的合并规则和其它字段相反：`self.previous if other.previous is None else other.previous`。原因同上但顺序不同，读代码时值得留意。

**架构推论：** 冻结 + `override` 造新实例的组合，使 runtime 变更是「沿调用链替换」而不是「全局状态修改」。因此一个子图可以有自己的 store 或 media writer，而不会污染父图；反过来父图的 context 也能通过 `merge` 继承下去。

**架构推论：** `execution_info` 与 `control` 分开：前者是只读快照（每次尝试新造一个），后者是可变控制面（整个 run 共享一个）。这个划分决定了「重试」只需重建前者，而 drain 请求天然对所有尝试都可见。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/runtime.py` | `Runtime` | 运行期依赖容器，冻结 dataclass |
| `libs/langgraph/langgraph/runtime.py` | `Runtime.merge` | 两层 runtime 的字段级合并，含哨兵函数比对 |
| `libs/langgraph/langgraph/runtime.py` | `Runtime.override` | 用 `replace` 造新实例 |
| `libs/langgraph/langgraph/runtime.py` | `ExecutionInfo` | 本次执行的位置信息快照 |
| `libs/langgraph/langgraph/runtime.py` | `ExecutionInfo.patch` | 复制并替换选定字段，供重试递增尝试号 |
| `libs/langgraph/langgraph/runtime.py` | `RunControl` | 单标志的协作式 drain 控制面 |
| `libs/langgraph/langgraph/runtime.py` | `get_runtime` | 从配置里取当前 runtime |
| `libs/langgraph/langgraph/pregel/_algo.py` | `prepare_next_tasks` | 为每个任务 override 出带 execution_info 的 runtime |
| `libs/langgraph/langgraph/pregel/_retry.py` | `_ensure_execution_info` | 分布式路径下从配置键兜底补 execution_info |
| `libs/langgraph/langgraph/pregel/main.py` | `_coerce_parent_runtime` | 把外部塞进来的半成品对象归一成 Runtime |

## 参考

- [LangGraph 源码：runtime.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/runtime.py)
- [LangGraph 源码：pregel/_algo.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_algo.py)
- [LangGraph 文档：运行时上下文](https://langchain-ai.github.io/langgraph/concepts/runtime/)

## 与主核 / AI 的关系

Runtime 与主核里「执行上下文结构体」是同一种东西：把 session、事务、缓冲池这些横切依赖打成一个只读包，随调用往下传，而不是让每个函数自己去全局取。主核用显式指针传，这里用 dataclass，但约束一致——执行体只读它，不写它。

对 AI 侧的实际意义有两层。一是嵌套图不会串数据：子图拿到自己的 `store` 与 `context`，靠 `merge` 从父图继承，越界就读不到。二是可观测性：`execution_info` 里有 `checkpoint_ns` 与 `task_id`，把这三层 id 一起记下来，线上出问题时才能把一条 LLM 日志对回具体的图节点与尝试次数。
