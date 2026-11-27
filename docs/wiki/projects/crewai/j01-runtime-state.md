---
project: crewai
feature: runtime-state
title: 运行时状态与序列化
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/state/runtime.py
    symbols: [RuntimeState, _serialize, _deserialize]
---

# 运行时状态与序列化

## 一句话职责

用一个 `RootModel` 把「当前活着的所有实体」和「执行事件图」打成一份自包含快照，并按版本做迁移、按分支链记录血缘，使整次运行能被完整落盘再复原。

## 机制是什么

`RuntimeState(RootModel)` 的 `root` 是一个 `list[Entity]`——即 crew、flow、agent、task 这些实体的实例列表。除了 root，它有三个私有属性承载「快照之外」的元信息：`_provider`（写检查点用哪个后端，默认 `JsonProvider`）、`_event_record`（事件图）、`_checkpoint_id` / `_parent_id` / `_branch`（血缘三件套）。

序列化不是 pydantic 的默认行为，而是自定义的 `@model_serializer(mode="plain")`：产出的字典有五个键——`crewai_version`、`parent_id`、`branch`、`entities`（每个实体 `model_dump(mode="json")`）、`event_record`（事件图 `model_dump(mode="json")`）。所以一份快照同时包含「对象树」与「怎么走到这一步的事件图」。

反序列化用 `@model_validator(mode="wrap")`：若输入是含 `entities` 的字典，先跑 `_migrate`，然后 `handler(data["entities"])` 让 pydantic 正常构建 root，最后补回 `_event_record`、`_parent_id`、`_branch`。不是这种形状就原样交给 handler。

这里最关键的一点：`Entity` 这个类型**在导入时无法解析**（实体类会反向 import state 模块），所以模块 docstring 明确说「`Entity` 在 `crewai/__init__.py` 里通过 `RuntimeState.model_rebuild()` 解析」。也就是说这个模型的可用性依赖包初始化的顺序。

## 怎么实现的

**版本迁移。** `_migrate(data)` 从 `data["crewai_version"]` 读版本、与 `get_crewai_version()` 比。`None` 时记 warning 并当成 `0.0.0`；版本不同记 debug 日志。低于 `1.14.6` 时对每个实体跑 `_backfill_discriminators`。迁移块按版本顺序排列，注释说明这样「迁移可以组合」。

三处回填各对应一个历史字段：

- `_backfill_memory_kind`：旧的内存字典没有 `memory_kind` 判别字段，按结构推断——有 `scopes` 键就是 `slice`、有 `root_path` 就是 `scope`、否则 `memory`。
- `_backfill_source_type`：旧的知识来源没有 `source_type`。只有 `StringKnowledgeSource` 能可靠推断（`content` 是纯字符串）；文件类来源的 `content` 是字典/列表，所以**故意不标**，让下游校验报出清晰的错误。推断不出来时抛的 `ValueError` 直接建议「升级到 1.14.6+ 后重新存一次检查点」。
- `_backfill_sources_on` / `_backfill_discriminators`：把上面两个回填分别应用到实体本身、它的 `knowledge`、它的每个 agent，以及每个 agent 的 `knowledge`。

**写检查点的准备。** `checkpoint(location)` 流程：`_begin_checkpoint` 发 `CheckpointStartedEvent` 并返回不变式上下文（provider 名、parent 快照、branch 快照、起始时间）；`_prepare_entities(self.root)`；`self._provider.checkpoint(self.model_dump_json(), location, parent_id=..., branch=...)`；`_chain_lineage` 更新 `_checkpoint_id` 与 `_parent_id`；成功发 `CheckpointCompletedEvent`（带耗时与 provider 提取的 id）；异常发 `CheckpointFailedEvent` 后重抛。`acheckpoint` 是它的异步版，结构完全对称。

**`_prepare_entities` 的两件事。** 对每个实体：`entity.execution_context = capture_execution_context()`（把当前 ContextVar 里的执行上下文快照挂上去），再 `_sync_checkpoint_fields(entity)`。

后者是把**私有运行期属性搬到公开检查点字段**上，因为 pydantic 只序列化公开字段。三类实体的搬运内容不同：`BaseAgent` 搬 `_kickoff_event_id`；`Flow` 搬已完成的 method 集合、方法输出列表、方法执行计数（键转成 `str`）、以及 `_copy_and_serialize_state()` 序列化后的状态；`Crew` 搬 inputs、train、`_kickoff_event_id`，并对每个 task 搬原始描述、原始期望输出、输入描述。

task 那一项有个细节值得看：`checkpoint_input_description` 取 `task.description if task._input_description is None else task._input_description`，注释写「永远不为 None，这样 None 就标志着一份字段出现之前的旧检查点」。用一个哨兵值区分「没存过」与「存的是空」，是版本兼容的标准手法。

**读检查点。** `from_checkpoint(config)` 要求 `config.restore_from` 非空（否则 `ValueError`），发 `CheckpointRestoreStartedEvent`，用 `detect_provider(location)` 猜后端，`provider.from_checkpoint(location)` 取原始 JSON，`cls.model_validate_json(raw)` 构建，然后把这个实例的 `_provider`、`_checkpoint_id`、`_parent_id` 都设成恢复来源。失败发 `CheckpointRestoreFailedEvent` 并重抛；成功发 `CheckpointRestoreCompletedEvent`（带 checkpoint_id、branch、parent_id、耗时）。

**分支。** `fork(branch=None)` 生成新分支名：给了就用；没给但有 `_checkpoint_id` 就用 `f"fork/{_checkpoint_id}_{uuid4().hex[:6]}"`；两者都没有就用 `f"fork/{uuid4().hex[:8]}"`。注释说「始终唯一，多次调用不会碰撞」。然后发 `CheckpointForkStartedEvent`、置 `_branch`、发 `CheckpointForkCompletedEvent`。

注意 `fork` **只改分支标签**，不写检查点。docstring 说「若这个状态是从检查点恢复的，会在新分支上写一份初始检查点」，但实现里没有任何 provider 调用——这是文档与实现的偏差。

**血缘链。** `_chain_lineage(provider, location)` 把 `_checkpoint_id` 设成 provider 从 location 提取的 id，`_parent_id` 也设成同一个值。所以「下一次写的检查点的 parent 就是这一次的 id」，形成链。从检查点恢复时 `_parent_id` 同样设成恢复来源的 id——于是接着写会落在原链上。

## 关键取舍

**源码事实：** 序列化把 `_provider` **排除在外**。所以一份检查点里不含「写它的后端是哪个」，恢复时靠 `detect_provider(location)` 从 location 形态反推。这让检查点更可移植（同一个位置可以由不同后端读），但也意味着「换后端」不会因为检查点内容而受限。

**源码事实：** 迁移只处理一个版本分界（`1.14.6`）。版本更高（未来版本倒灌）时不做任何检查，直接按当前 schema 校验——老代码读新检查点会得到 pydantic 校验错误，而不是「拒绝加载」这种明确提示。

**源码事实：** `_migrate` 里对 `crewai_version` 缺失只 warning，然后按 `0.0.0` 处理，于是会跑一遍 1.14.6 的回填。这是宽松兼容：读得了就尽量读，读不了靠下游校验暴露。

**源码事实：** `checkpoint` 与 `acheckpoint` 是两份几乎一样的代码，只有 provider 调用那一行是 `await`。它们没有共享实现，所以任何新增逻辑（例如新增一个事件）都要改两处。

**源码事实：** `_prepare_entities` 会**修改实体对象本身**（写 `execution_context` 与 `checkpoint_*` 字段）。所以「存检查点」这个动作有副作用，不是纯读取。多次存会反复覆盖这些字段——其中 `Flow` 的 `checkpoint_state` 每次都要重新序列化整个状态。

**架构推论：** 把「执行上下文快照」挂到每个实体上再序列化，是让检查点自足的关键：恢复后每个实体都知道「我当时处在哪个任务、哪个 flow 方法、事件栈到哪一层」。没有这一步，恢复出来的对象树是「静态结构」而不是「某个时刻的运行态」。

**架构推论：** 用 `RootModel` 而不是普通 BaseModel 包一层 `entities` 字段，使「状态就是一个实体列表」这件事在类型上直白。代价是所有附加信息只能放私有属性，而私有属性必须手写序列化——这正是 `_serialize` / `_deserialize` 存在的原因，也是「漏掉某个私有属性」这类 bug 的温床。

**架构推论：** 血缘用「parent_id 指向上一次写的检查点」建模，是典型的**单链表**。它足以表达「从哪个点分叉」，但不能表达「合并」。对重放与分叉实验足够了；若要支持「两个分支汇合」，这个结构得换成 DAG。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/state/runtime.py` | `RuntimeState` | 实体列表加事件图的整体快照模型 |
| `lib/crewai/src/crewai/state/runtime.py` | `RuntimeState._serialize` | 自定义序列化：版本、血缘、实体、事件图 |
| `lib/crewai/src/crewai/state/runtime.py` | `RuntimeState._deserialize` | 先迁移再构建 root，再补回私有元信息 |
| `lib/crewai/src/crewai/state/runtime.py` | `RuntimeState.checkpoint` | 同步写检查点，含事件与血缘更新 |
| `lib/crewai/src/crewai/state/runtime.py` | `RuntimeState.acheckpoint` | 异步写检查点的对称实现 |
| `lib/crewai/src/crewai/state/runtime.py` | `RuntimeState.from_checkpoint` | 探测后端、恢复、设置血缘 |
| `lib/crewai/src/crewai/state/runtime.py` | `RuntimeState.fork` | 生成唯一分支标签并切换分支 |
| `lib/crewai/src/crewai/state/runtime.py` | `RuntimeState._chain_lineage` | 把本次写出的 id 记成下一次的 parent |
| `lib/crewai/src/crewai/state/runtime.py` | `_prepare_entities` | 挂执行上下文快照，同步检查点字段 |
| `lib/crewai/src/crewai/state/runtime.py` | `_sync_checkpoint_fields` | 私有运行期属性搬到公开检查点字段 |
| `lib/crewai/src/crewai/state/runtime.py` | `_migrate` | 按版本顺序应用迁移块 |
| `lib/crewai/src/crewai/state/runtime.py` | `_backfill_memory_kind` | 按结构推断旧版内存判别字段 |
| `lib/crewai/src/crewai/state/runtime.py` | `_backfill_source_type` | 只能可靠推断字符串来源，其余报错 |

## 参考

- [CrewAI 源码：state/runtime.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/state/runtime.py)
- [CrewAI 文档：Crews 与状态](https://docs.crewai.com/en/concepts/crews)
- [Pydantic 文档：RootModel](https://docs.pydantic.dev/latest/concepts/models/#rootmodel-and-custom-root-types)

## 与主核 / AI 的关系

主核里有直接对应的东西：检查点（checkpoint）就是「把整个共享内存状态刷到磁盘」，并在启动时从检查点恢复。两者的共同问题是「私有/运行期状态如何进快照」——主核靠显式的 `T_*` 数据结构与各自的序列化例程，这里靠 `_sync_checkpoint_fields` 手工把私有属性搬到公开字段。差别在于主核的检查点要保证崩溃一致性（部分写入必须可辨识），这里的检查点只是一份 JSON，靠 provider 实现保证原子性。

与 AutoGen 相比，AutoGen 的状态是**分散的**：每个 agent 有自己的 `save_state`/`load_state`，团队有 `TeamState`，但没有任何组件负责「把所有东西打成一个包」。CrewAI 的 `RuntimeState` 是单一入口的全局快照，因此天然支持「一次落盘恢复全部」。代价是这个模型必须知道所有实体类型（见 `_sync_checkpoint_fields` 里对 `BaseAgent`、`Flow`、`Crew` 的 isinstance 分支），新增实体类型就要改这里——这是一处明确的扩展点瓶颈。

对 AI 工程最实用的一条是「用哨兵区分没存过与存的是空」。`checkpoint_input_description` 那段注释写得很清楚：字段值永不为 `None`，于是 `None` 可以用来标记「这份检查点早于该字段出现」。做任何带版本演进的状态格式时，这个手法都能省掉一个额外的 `schema_version` 判断。

另一条是 `execution_context` 的挂载时机。它是在**写检查点那一刻**从 ContextVar 读出来挂到实体上的，所以快照记录的是「存的那一刻」的执行位置。这意味着如果检查点是在错误的上下文里写的（例如从后台线程、或事件处理器里），快照里的执行位置会是错的。写检查点的入口必须保证自己跑在业务执行的上下文里——这也是为什么事件总线里的检查点处理器要额外处理上下文复制。

最后是那个 `fork` 的文档与实现偏差。它提醒我们：一个方法名听起来像「写初始检查点」，实际只改标签，会让调用方以为落盘了而实际没有。这类偏差在恢复链上代价很高——用户以为有了分叉点，实际下次写的检查点 parent 仍指向分叉前的 id。
