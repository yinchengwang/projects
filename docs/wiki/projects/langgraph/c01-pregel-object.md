---
project: langgraph
feature: pregel-object
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/main.py
    symbols: [Pregel, _resolve_checkpointer]
---

# Pregel 图对象与检查点解析

## 一句话职责

`Pregel` 是一张已编译图在运行期的唯一句柄：它把节点表、通道表、检查点器、存储与缓存归一成一个可反复执行的对象，并在每次运行开始时现场决定「这一次到底用哪个检查点器」。

## 机制是什么

`Pregel` 的构造参数全部 keyword-only，可以分成两类：一类描述图的结构（`nodes`、`channels`、`input_channels`、`output_channels`、`stream_channels`、`interrupt_before_nodes`、`interrupt_after_nodes`），一类是运行期资源（`checkpointer`、`store`、`cache`、`retry_policy`、`cache_policy`、`step_timeout`、`context_schema`）。构造期只做三件事：把 `NodeBuilder` 编译成 `PregelNode`、把资源字段落到实例属性上、按 `auto_validate` 决定要不要立刻跑一次 `validate()`。它不打开检查点、不读任何通道值。

`channels` 有一处强制改写，是唯一会被框架覆盖的通道：`TASKS` 必须且只能是 `Topic(Send, accumulate=False)`。调用方传了一个非 `Topic` 的 `TASKS` 会直接 `ValueError`，其余情况无条件写入这个 `Topic`。也就是说「待执行的 `Send` 队列」在框架侧被钉死，图不能自定义它的合并语义。

检查点不是构造期固定的，而是每次运行时解析。`_resolve_checkpointer` 按四级优先级决定：

1. `self.checkpointer is False` → 返回 `None`。这是显式的「本次运行不要检查点」，也是子图能在父图有检查点器时主动关掉自己的唯一口子。
2. config 的 `configurable` 里带 `CONFIG_KEY_CHECKPOINTER` → 用它。这一条对应子图被父图调用：子图自己可能没配置检查点器，检查点器是父图在 config 里借给它的。
3. `self.checkpointer is True` → 抛 `RuntimeError`。`True` 表示「我要一份自己的 thread 历史」，只有子图能这么说，根图没有父图可借，所以这是运行期错误而不是构造期错误。
4. 否则用实例自己的 `checkpointer`。若拿到的是 `BaseCheckpointSaver` 实例，再套一层 serde 允许清单。

## 怎么实现的

**构造期归一。** 传入的 `deprecated_kwargs` 里若还有 `config_type`，会先降级成 `context_schema` 并发出弃用警告。`ensure_valid_checkpointer` 把 `True`/`False`/`None`/实例四种写法归一。`nodes` 里每个 `NodeBuilder` 调 `.build()` 编译成 `PregelNode`，纯 `PregelNode` 原样放入。`debug` 缺省取 `get_debug()`（由环境变量决定）而不是硬编码 `False`。`retry_policy` 若给的是单个实例会被包成元组，后续代码只需要处理序列。

**校验与派生缓存。** `validate()` 分三步：逐节点检查 `timeout` 是否被该节点形态支持，交给 `validate_timeout_supported`；然后用 `validate_graph` 检查节点、通道、输入输出键与中断点之间的结构一致性；最后把 `trigger_to_nodes`（通道名 → 可能被该通道更新触发的节点名）算出来存到实例上。这个映射是纯派生数据，`PregelLoop` 在每步推导任务时用它把「全量扫节点」降成「只看受影响的节点」。

**serde 允许清单。** `_serde_allowlist` 初始为 `None`，是惰性构建的。`_apply_checkpointer_allowlist` 先看 `_serde.STRICT_MSGPACK_ENABLED`，关着就原样返回检查点器；开着才调用 `apply_checkpointer_allowlist`，由它判断检查点器是否实现了 `with_allowlist`，没实现只警告一次并跳过。因此这条链在旧检查点器上是静默降级的。

**只读 API 的检查点器。** `_state_checkpointer` 给 `get_state`、`get_state_history` 一类的只读调用用，它先 `ensure_config` 再解析，解析结果不是 `BaseCheckpointSaver` 就抛 `ValueError("No checkpointer set")`——只读 API 不接受「假装没有检查点」这种状态。

**命名空间处理。** `_own_checkpoint_config` 处理 `checkpointer=True` 的子图：它把 config 里的 checkpoint namespace 每一段里 `NS_END` 之后的部分剥掉，但保留数字段。源码注释明确解释了与 `recast_checkpoint_ns` 的差别：同一个子图被第二次调用时，那次调用的历史要落在不同的数字段下，否则两次调用会互相覆盖。`_subgraph_for_namespace` 则负责把带 namespace 的状态请求转发给对应子图，并把本图的检查点器借给它；`for_update=True`（写操作）时找不到匹配子图就直接报错，因为用本图的节点与通道去写子图的状态会写错地方。

**每次运行的参数解析。** `_defaults` 在 `stream`/`invoke` 入口处统一解析：`recursion_limit` 下限检查、`output_keys` 缺省退到 `stream_channels_asis`、中断点缺省退到实例上的 `interrupt_*_nodes`、`stream_mode` 与 `print_mode` 合成集合、检查点器走 `_resolve_checkpointer`、`store` 与 `cache` 优先取 config 里的、`durability` 缺省 `"async"`。这里有一条硬约束：有检查点器但 config 的 `configurable` 是空的，直接抛 `ValueError`，因为检查点必须靠 `thread_id` 之类的键定位，没有键就等于没有落点。

## 关键取舍

**源码事实：** 检查点器是四态而非「有/无」两态——`None`、`False`、`True`、实例。把「本次运行关掉检查点」和「我要自己一份 thread 历史」都编码成布尔值，省掉了一个参数，代价是 `_resolve_checkpointer` 必须在运行期区分根图与子图，且 `checkpointer=True` 用在根图上只能到运行时才报错。

**源码事实：** `TASKS` 通道被无条件覆盖。图可以定制任意通道的合并语义，唯独 PUSH 队列不行。这是把「`Send` 在下一超步执行」这条语义锁在框架内，不留给使用方改写。

**源码事实：** `trigger_to_nodes` 只在 `validate()` 里算一次并缓存。它不会随 `self.nodes` 的后续改动自动更新；绕过公开 API 直接改节点表，这个缓存就是过期的，任务推导会漏掉触发关系。

**架构推论：** `Pregel` 自身不持有任何运行期状态——没有 step、没有 checkpoint、没有通道值，全部装在每次运行新建的 `PregelLoop` 里。由此换来的是同一个实例可以并发跑多个 thread（会话），代价是所有运行期变量都必须显式穿过 loop 与 runner 传递。

**架构推论：** `auto_validate` 可以关。关掉后结构错误推迟到执行期才暴露，这给「先构造、后补节点」的用法留了口子，但也意味着 `trigger_to_nodes` 也不会被填充，任务推导退化到全量扫节点。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/main.py` | `Pregel` | 运行期图句柄：结构表、资源字段、校验入口 |
| `libs/langgraph/langgraph/pregel/main.py` | `Pregel.__init__` | 编译 NodeBuilder、归一键检查点器与策略、可选校验 |
| `libs/langgraph/langgraph/pregel/main.py` | `_resolve_checkpointer` | 检查点器的四级优先级解析 |
| `libs/langgraph/langgraph/pregel/main.py` | `_own_checkpoint_config` | `checkpointer=True` 子图的 namespace 归一 |
| `libs/langgraph/langgraph/pregel/main.py` | `_subgraph_for_namespace` | 状态方法按 namespace 转发到子图 |
| `libs/langgraph/langgraph/pregel/main.py` | `_defaults` | 每次运行开始时解析运行参数与资源 |

## 参考

- [LangGraph 源码：pregel/main.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/main.py)
- [LangGraph 文档：Pregel 与 BSP 执行模型](https://langchain-ai.github.io/langgraph/concepts/low_level/#pregel-algorithm)
- [LangGraph 文档：持久化与检查点](https://langchain-ai.github.io/langgraph/concepts/persistence/)

## 与主核 / AI 的关系

`Pregel` 这一层与主核的「实例句柄」是同一种角色：主核打开一个库时把路径、页大小、缓存预算归一成句柄，之后所有操作都通过句柄进行；`Pregel` 把节点表、通道表、检查点器归一成句柄，之后所有 `invoke`/`stream` 都通过它进行。差别在于主核的打开是一次性定型的，而这里的检查点器要等到每次运行才解析——因为父图可能临时借出一个不同的实现。

对 AI 侧而言，关键结论是：一个 `Pregel` 实例可以同时服务多个会话。`thread_id` 在 config 里而不在实例上，检查点器在运行期解析，所以把 agent 部署成多租户服务时不需要为每个用户复制一份图对象；需要隔离的只有检查点器后端的 key 空间。

