---
project: crewai
feature: event-record-graph
title: 事件记录图与因果重建
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/state/event_record.py
    symbols: [EventRecord, EventNode, descendants]
---

# 事件记录图与因果重建

## 一句话职责

把顺序发生的事件流按关系字段组装成一张有向图：每个事件是一个节点、每条关系是一条双向边，于是「某任务下面发生过什么」「这次失败是被谁引起的」这类问题变成图上的一次遍历。

## 机制是什么

`EventRecord` 只持一个字段：`nodes: dict[str, EventNode]`——事件 id 到节点的映射，因此按 id 查是 O(1)。另有私有 `RWLock` 保护所有读写。

`EventNode` 也简单：一个 `event` 加一个 `edges: dict[EdgeType, list[str]]`。`EdgeType` 是八个值的字面量：`parent`、`child`、`trigger`、`triggered_by`、`next`、`previous`、`started`、`completed_by`。四对互逆关系，所以每条关系都在两端各记一次。

关键在 `add(event)`：它不是简单地插入节点，而是**按事件自带的四个关系字段自动接线**。而那几个字段（`parent_event_id`、`triggered_by_event_id`、`previous_event_id`、`started_event_id`）正是事件总线在 `_prepare_event` 里补齐的（见 h02）。

## 怎么实现的

**`add` 的四段接线。** 每条都先判「关系字段非空**且**目标节点已在图里」——这个前置条件很重要，因为事件可能先于它的父到达（父关系指向一个尚未记录的事件，或者指向一个被裁剪掉的早期事件）。判断通过才双向连边：

- `parent_event_id` → 本节点加 `parent` 边，目标节点加 `child` 边。
- `triggered_by_event_id` → 本节点加 `triggered_by`，目标加 `trigger`。
- `previous_event_id` → 本节点加 `previous`，目标加 `next`。
- `started_event_id` → 本节点加 `started`，目标加 `completed_by`。

所以「开始事件」与「结束事件」在图上是双向可达的：从开始能走到结束（`completed_by`），从结束能走回开始（`started`）。这比单向指针强，恢复时可以任选入口。

**遍历只有一种：`descendants(event_id)`。** 广度优先，从给定 id 出发、只看 `child` 边、用 `visited` 集合防重（因为图可能有交叉边），结果按 BFS 顺序返回**子孙节点列表，不含起点本身**。队列用 `list.pop(0)`，即简单的 FIFO。

注意 `visited` 的用法有个细节：起点先入队，出队时才加入 `visited` 并查节点；子节点是**在入队前**就检查 `child_id not in visited`、然后把 `child_node` 加进结果并 `append` 到队列。所以一个节点只会在结果里出现一次，但「已访问」的登记晚于「已入结果」。

**其余读操作。** `get(event_id)` 读锁下取节点；`roots()` 返回所有**没有 `parent` 边**的节点（即顶层事件）；`all_nodes()` 返回节点的快照列表（注释说这样调用方可以在不持锁的情况下迭代）；`clear()` 写锁下清空；`__len__` 与 `__contains__` 都在读锁下。

**事件类型的多态恢复。** `EventNode.event` 的类型注解是 `Annotated[BaseEvent, BeforeValidator(_resolve_event), PlainSerializer(...)]`。`_resolve_event` 处理反序列化：已是 `BaseEvent` 实例直接返回；不是字典就用 `BaseEvent.model_validate`；是字典则先按 `_event_type_map` 把 `type` 字符串映射成具体类再校验，映射失败或校验异常都回落成基础 `BaseEvent`。

`_build_event_type_map` 递归遍历 `BaseEvent.__subclasses__()`，取每个子类 `model_fields["type"].default` 作为键。所以映射是在**首次需要时**才构建（`_resolve_event` 里 `if not _event_type_map` 触发），因为导入顺序保证不了所有事件类都已定义。

`PlainSerializer` 则是按序列化模式分派：JSON 模式 `model_dump(mode="json")`，否则普通 `model_dump()`。

**并发保护。** 所有公开方法都套 `with self._lock.r_locked()` 或 `w_locked()`，用的是仓内自己的 `RWLock`。`add` 在写锁里同时插入节点与改邻居的边——这必须原子，否则「半张图」会被别的读者看到。

**与运行状态的接合。** `RuntimeState._event_record` 是私有属性，默认 `EventRecord()`，公开为 `event_record` 属性。事件总线在 `_prepare_event` 末尾调 `self._record_event(event)`，后者若运行状态存在就 `self._runtime_state.event_record.add(event)`。序列化时 `event_record` 一起进 JSON，反序列化时 `EventRecord.model_validate(record_data)` 还原。所以**事件图随检查点一起落盘**——这是「恢复后还能重建因果关系」的前提。

## 关键取舍

**源码事实：** 只有当目标节点已在图里时才连边。所以事件到达顺序会影响图的完整性：若子事件先被 `add`，它的 `parent` 边不会建立（目标节点还不存在），事后父事件到达也不会回头补这条边。实际执行顺序通常保证父先于子（父的 `add` 发生在子事件发射之前），但**重放与恢复时会打破这个顺序**。

**源码事实：** `edges` 是 `dict[EdgeType, list[str]]`，允许重复。`add_edge` 用 `setdefault(...).append(...)`，不去重。所以同一对节点之间若有两次同样的关系（理论上不该发生），会出现重复边。

**源码事实：** 没有任何删除单节点的能力，只有 `clear()` 全清。图只增不减，所以长运行会持续占用内存——事件数量与运行时长成正比。

**源码事实：** `roots()` 的判据是「没有 `parent` 边」，而不是「`parent_event_id` 为空」。这两者在「父节点尚未记录」时会分叉：一个事件带 `parent_event_id` 但父不在图里，它的 `parent` 边是空的，于是会被 `roots()` 当成根。这是上面那条顺序依赖的另一个表现。

**源码事实：** `descendants` 只走 `child` 边，不跟 `triggered_by`。所以「因果链」与「包含树」在这个 API 里是两条独立路径，没有任何方法同时遍历两者。

**架构推论：** 「关系分散在事件字段里、图只是投影」这个设计让事件本身仍然是自包含的（可单独序列化、可单独传输），而图是它们的索引。代价是图必须按正确顺序构建，否则边会缺失——本质上是把「图的一致性」交给「事件的到达顺序」来保证。

**架构推论：** 双向边（每对关系存两次）用空间换查询方向自由。对「给我某节点的子节点」与「给我某节点的父节点」两个高频方向，都不需要反向索引。八种边类型成四对，正是这套设计的最小完备集。

**架构推论：** 事件类型在反序列化时需要一张 `type` 字符串到类的映射，而这张表是**运行期**从子类递归收集的。这意味着事件图的可移植性依赖「恢复时有同样的类集合」：一个插件定义的事件类型在另一环境里不存在，就会退化为基础 `BaseEvent` 并丢掉它的专属字段。这是所有「按类名字符串反序列化」方案的共同弱点。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/state/event_record.py` | `EventRecord` | 事件图：id 到节点的字典加读写锁 |
| `lib/crewai/src/crewai/state/event_record.py` | `EventRecord.add` | 插入节点并按四个关系字段双向接线 |
| `lib/crewai/src/crewai/state/event_record.py` | `EventRecord.descendants` | 沿 `child` 边广度优先取子孙 |
| `lib/crewai/src/crewai/state/event_record.py` | `EventRecord.roots` | 无父边的节点即顶层事件 |
| `lib/crewai/src/crewai/state/event_record.py` | `EventRecord.all_nodes` | 读锁下取节点快照 |
| `lib/crewai/src/crewai/state/event_record.py` | `EventNode` | 一个事件加八类邻接列表 |
| `lib/crewai/src/crewai/state/event_record.py` | `EventNode.add_edge` | 追加邻接 id，不去重 |
| `lib/crewai/src/crewai/state/event_record.py` | `EdgeType` | 四对互逆关系的字面量类型 |
| `lib/crewai/src/crewai/state/event_record.py` | `_resolve_event` | 反序列化时按 `type` 找具体事件类，失败回落基类 |
| `lib/crewai/src/crewai/state/event_record.py` | `_build_event_type_map` | 懒构建的类型名到类映射 |

## 参考

- [CrewAI 源码：state/event_record.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/state/event_record.py)
- [CrewAI 文档：Event Listeners](https://docs.crewai.com/en/concepts/event-listener)
- [Python 文档：collections.deque 与广度优先遍历](https://docs.python.org/3/library/collections.html#collections.deque)

## 与主核 / AI 的关系

主核里有结构更严格的同类物：事务的父子关系用 `PGPROC` 链与子事务 id 范围表达，可见性判断可以只看 id 不需要遍历图。这里的图是**显式且通用的**——任何两个事件只要有关系就能连边，不限于固定的父子链。通用性带来表达力，代价是查询必须遍历。

与 AutoGen 相比，AutoGen 没有事件图。它的事件只是一条条日志记录，加上 OpenTelemetry 的 span 嵌套（那是追踪系统的树，不是业务事件的图）。CrewAI 把图建在业务事件上，好处是可以回答「这个 task 下发生过多少次 LLM 调用、各自耗时多少」这类业务问题；坏处是图需要自己维护一致性，而 OTel 的 span 树由 SDK 保证。

对 AI 工程最实用的一条是「关系字段在事件上、图只是投影」。这样事件可以单独流转（发给外部 sink、落到日志），而图可以在需要时重建。若把关系只存在图里，事件就失去了自解释性。

另一条是那个顺序依赖的坑。`add` 只在目标已存在时连边，所以「先记子后记父」会永久丢边。任何要重放事件去重建图的系统，都必须**按原始顺序重放**；若按别的顺序（例如按事件类型分组），图就会残缺。这解释了为什么事件总线要维护 `emission_sequence`——它是重放顺序的唯一依据。

最后是「图只增不减」。对短生命周期的运行没问题，但对长驻进程（例如持续接收请求的 agent 服务）会持续增长。设计上要么接受内存换完整性，要么引入按时间或按根的裁剪，而裁剪会带来「父节点消失导致 `roots()` 判断失真」的连带问题——指数的代价。
