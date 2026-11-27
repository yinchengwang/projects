---
project: langgraph
feature: serde-allowlist
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/_internal/_serde.py
    symbols: [build_serde_allowlist, collect_allowlist_from_schemas, apply_checkpointer_allowlist]
---

# 序列化允许清单构建

## 一句话职责

从「图的 schema 与通道类型」出发，把检查点里可能出现的全部类型名递归收成一张 `(module, name)` 白名单，再挂到检查点保存器的 msgpack 反序列化闸门上。

## 机制是什么

问题背景：检查点里的值是 msgpack 编码的任意 Python 对象，反序列化时要按 `(module, name)` 去 import 并构造类。若允许任意符号，谁能在检查点库里写数据，谁就能让服务端在下次读取时执行任意构造逻辑。

`STRICT_MSGPACK_ENABLED` 是这套机制的开关，由环境变量 `LANGGRAPH_STRICT_MSGPACK` 控制，默认**关闭**。打开后，序列化器的默认 `allowed_msgpack_modules` 变成 `None`，即只认内置的 `SAFE_MSGPACK_TYPES`（datetime、uuid、decimal、pathlib、zoneinfo、langchain-core 消息、`Send`/`Command`/`Interrupt`/`Overwrite` 等）。

但只认内置白名单会把用户自定义的状态类型全挡在外面。于是 `build_serde_allowlist` 负责从图定义里**推导**出还需要哪些类型，把它合并进去——这就是「允许清单构建」这件事的全部意义。

## 怎么实现的

**两条来源。** `build_serde_allowlist` 是 `curated_core_allowlist()` 与 `collect_allowlist_from_schemas(...)` 的并集。前者是硬编码的 langchain-core 消息类清单，逐个 `getattr` 取类的 `__module__` 与 `__name__`，取不到就跳过（不报错，因为不同版本的 langchain-core 消息集不完全一致）。后者从 schema 与通道推导。

**通道不只给两个类型。** `collect_allowlist_from_schemas` 遍历 `channels` 时，对每个通道取 `ValueType` 与 `UpdateType` 两个属性并各递归一次。这一点很关键：一个通道读出来的类型与它能接受的写入类型可以不同（比如增量通道的 update 是片段、value 是聚合结果），两个都可能进检查点。

**递归的类型树遍历。** `_collect_from_type` 是一个大分派，按顺序处理：

- `Any` / `None` / 裸 `Literal` / `Literal[...]` → 直接返回，不加任何符号。
- `types.UnionType`（`X | Y` 语法）与 `typing.Union` → 展开 `__args__` 或 `get_args` 逐个递归。两条分支分别处理是因为它们不是同一个类型对象。
- `Annotated`、`Required`、`NotRequired` → 取第一个参数继续递归，剥掉这层包装。
- `list` / `set` / `tuple` / `dict` / `deque` / `frozenset` → 递归所有类型参数。
- 有 `__supertype__` → 走一层（`NewType` 的定义）。
- `is_typeddict` → **自身不入表**，只递归它的字段类型。TypedDict 在运行期就是 dict，不存在「构造 TypedDict 类实例」这回事，所以不需要白名单条目。
- pydantic 模型 → 入表，再递归字段类型。字段类型优先用 `get_type_hints(include_extras=True)`，失败时退回 `model_fields` / `__fields__` 的注解，兼顾 v1 与 v2。
- dataclass → 入表，再递归字段类型；字段类型取不到 hints 时退回 `__dataclass_fields__` 的 `type`。
- `Enum` 子类 → 入表，不再递归（枚举成员不需要构造）。

**去重靠两层结构。** `_already_seen` 先试 `typ in seen`（`set`），碰到不可哈希的类型对象就退到 `id(typ)` 的集合。之所以需要两层：某些类型对象（含未 hash 的 `Annotated` 组合）不可哈希，会让 `in` 抛 `TypeError`。递归必须防环，否则一个自引用的 pydantic 模型会栈溢出。

**hints 失败的兜底策略是静默。** `_safe_get_type_hints` 吞掉所有异常返回 `{}`。这意味着一个前向引用解析不了的模型，它会入表但字段类型不会——不报错，只是白名单窄了一点。取舍明确：宁可让某个字段在反序列化时被拒，也不让 `compile()` 因为一个注解问题而失败。

**挂到保存器上。** `apply_checkpointer_allowlist` 先做三重短路：保存器为空、白名单为 `None`、或保存器是 `True`/`False` 这种标志值，都直接原样返回。然后检查保存器有没有 `with_allowlist` 方法（`_SUPPORTS_ALLOWLIST` 在模块导入时探好）；没有就只用**一次** `logger.warning` 提示「严格 msgpack 白名单会被跳过」——用模块级标志位保证不会每次编译都刷日志。有的话调 `checkpointer.with_allowlist(allowlist)`。

**with_allowlist 是非破坏性克隆。** 它造一个浅拷贝，把 serde 换成合并了额外条目的新序列化器。合并时若基础白名单已经是 `True`/`False`（即全放开或全关闭的布尔态）就直接返回原对象——布尔态没有「再合并」的语义。

**调用点只有两处。** `StateGraph.compile` 与 functional API 的 `entrypoint`，两处都先判 `_serde.STRICT_MSGPACK_ENABLED`。`StateGraph` 那边收集的 schema 相当全：state、input、output、context 四份，加上每个节点的 `input_schema`、每个分支的 `input_schema`。它拿到的通道字典同时作为 `channels` 参数传入，于是通道的 value/update 类型也进了遍历范围。

## 关键取舍

**源码事实：** 整条路径默认不启用。不开 `LANGGRAPH_STRICT_MSGPACK` 时，序列化器是「全放开 + 警告」模式，`build_serde_allowlist` 根本不会被调用。所以这是一个**可选加固**，不是默认安全边界。

**源码事实：** 白名单条目不支持前缀匹配。`JsonPlusSerializer` 的报错文案明确写着「Prefix allowlists are intentionally unsupported; prefer exact symbols」。每次都要精确到 `(module, name)`，避免「允许了一个包就允许了包里的所有东西」。

**源码事实：** 遍历会穿透 TypedDict 但不为它建条目。这看起来像遗漏，其实是对 msgpack 编码现实的正确建模：能出现在检查点里的符号是 pydantic 模型、dataclass、Enum 这些真正有构造函数的类。

**架构推论：** 「从类型推导而非手写配置」让加固能随图自动扩展：新增一个状态类型，白名单自动变宽，用户不需要改配置。代价是白名单的准确度受制于类型注解的完整度——注解写不全（用 `Any` 兜底）的地方，运行时就会撞上 `InvalidModuleError`。

**架构推论：** 保存器的 `with_allowlist` 返回克隆而不是原地改。这样同一个保存器实例可以被两张图复用，各带各的白名单——白名单是「这张图需要反序列化什么」，天然是图级而非进程级属性。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/_internal/_serde.py` | `build_serde_allowlist` | 内置消息清单与类型推导结果的并集 |
| `libs/langgraph/langgraph/_internal/_serde.py` | `collect_allowlist_from_schemas` | 遍历 schema 与通道的 ValueType/UpdateType |
| `libs/langgraph/langgraph/_internal/_serde.py` | `_collect_from_type` | 类型树递归分派，决定哪些符号入表 |
| `libs/langgraph/langgraph/_internal/_serde.py` | `_already_seen` | 防环去重，兼容不可哈希类型 |
| `libs/langgraph/langgraph/_internal/_serde.py` | `apply_checkpointer_allowlist` | 把白名单挂到保存器，含能力探测与一次性告警 |
| `libs/langgraph/langgraph/_internal/_serde.py` | `curated_core_allowlist` | langchain-core 消息类的硬编码白名单 |
| `libs/langgraph/langgraph/graph/state.py` | `StateGraph.compile` | 收集四份 schema 与全部节点/分支输入 schema 后构建白名单 |
| `libs/langgraph/langgraph/func/__init__.py` | `entrypoint` | functional API 侧的同构调用点 |
| `libs/checkpoint/langgraph/checkpoint/base/__init__.py` | `BaseCheckpointSaver.with_allowlist` | 克隆保存器并重建带合并白名单的 serde |
| `libs/checkpoint/langgraph/checkpoint/serde/_msgpack.py` | `SAFE_MSGPACK_TYPES` | 内置安全类型集合与严格模式开关 |

## 参考

- [LangGraph 源码：_internal/_serde.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/_internal/_serde.py)
- [LangGraph 源码：checkpoint serde/jsonplus.py](https://github.com/langchain-ai/langgraph/blob/main/libs/checkpoint/langgraph/checkpoint/serde/jsonplus.py)
- [LangChain 文档：检查点与持久化](https://langchain-ai.github.io/langgraph/concepts/persistence/)

## 与主核 / AI 的关系

这就是主核里「反序列化不信任输入」那条铁律的 Python 版本。主核的 WAL 回放会校验页头、校验 LSN、拒绝未知的记录类型，而不是照着日志里的字节去构造任意东西；这里做的是同一件事，只是「记录类型」变成了「Python 类名」，而拒绝的后果从数据损坏变成远程代码执行。

对 AI 侧的现实意义：部署到共享环境时应该打开 `LANGGRAPH_STRICT_MSGPACK`，然后把 `InvalidModuleError` 当作配置问题而不是 bug——它的报错文案会直接给出「把这一个符号加进白名单」的写法。反过来，若图里大量用 `Any` 或未注解的字段，严格模式上线后会连续炸出一串符号，这本身也是类型注解质量的体检。
