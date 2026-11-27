---
project: crewai
feature: flow-persistence
title: Flow 持久化与恢复
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/flow/persistence/decorators.py
    symbols: [persist]
  - path: lib/crewai/src/crewai/flow/persistence/sqlite.py
    symbols: [SQLiteFlowPersistence]
  - path: lib/crewai/src/crewai/flow/runtime/__init__.py
    symbols: [install_persistence, reload, _persist_method_completion]
---

# Flow 持久化与恢复

## 一句话职责

`@persist` 声明「这个方法的完成要落盘」，`FlowPersistence` 定义落盘接口，运行时在每个被声明的方法完成后调用后端存一份状态快照——于是流程可以按 id 续跑、可以从历史快照补水，也可以被重放。

## 机制是什么

持久化的单位是**方法完成**，不是流程完成。每存一条记录就是一次「这个流程在这个方法结束时状态长这样」的快照，同一流程的多次快照按插入顺序追加。读取默认取最新一条，另有按记录 id 取历史快照的接口。

三处开关决定「要不要存」：

- **装饰器**：`@persist(...)` 可加在类上（类级持久化，所有方法完成都存）或方法上（只存这个方法）。
- **定义**：持久化配置被投影进 `FlowPersistenceDefinition`，有 `enabled`、`verbose`、`persistence`（后端）三个字段，方法级与流程级各有一份，方法级优先。
- **实例**：`Flow(persistence=...)` 或 `install_persistence(...)` 装的后端会**覆盖**定义里的后端。

装饰器与运行时都遵循同一条默认规则：没显式给后端就用 `default_flow_persistence()`——若登记过工厂就用工厂的产物，否则退回内置 SQLite。

## 怎么实现的

**装饰器仍是盖章。** `persist` 不包装方法，它把 `SimpleNamespace(persistence=后端, verbose=布尔)` 写到目标的 `__flow_persistence_config__` 上就返回原对象。类级与方法级共用同一个属性名，由定义构建阶段分别读取：类上读到的是 `FlowDefinition.persist`，方法上读到的是 `FlowMethodDefinition.persist`。注意装饰器在**装饰期**就把默认后端实例化好了（`persistence if persistence is not None else default_flow_persistence()`）——所以这个默认后端在导入时就被创建，而不是等到第一次 kickoff。

**运行时只在一个地方判断。** `_persist_method_completion` 是唯一调用点，逻辑很短：

1. 取方法定义上的 persist，若没有就取流程级 persist；
2. 两者都没有或 `enabled` 为假——直接返回；
3. 选后端：若 `_instance_persistence` 为真且实例上有后端，用实例后端；否则走 `_persist_backend_for(定义)` 按定义解析。

第 3 步的注释解释了这个优先级的原因：实例给的后端是用户明确的意志，必须赢；而引擎从流程级定义**推导**出来的后端不能遮蔽方法级的 persist 配置。

后端按「定义对象的 `id()`」缓存（`_persist_backends`），所以同一份定义对象只解析一次，不会每个方法完成都重建一个后端。

**解析链条。** `_resolve_persist_backend` 先看定义里的 `persistence` 字段：为空就退回默认工厂；不为空就调模块级 `_resolve_persistence`——已是 `FlowPersistence` 实例的直接用；是字典则按 `persistence_type` 去 `_persistence_registry` 里找类并 `model_validate`。解析结果若不是 `FlowPersistence` 子类实例就抛错，报错文案带上流程名以便定位。

`_persistence_registry` 是 `FlowPersistence.__init_subclass__` 自动维护的：任何没有遗留抽象方法的子类都会被登记到以类名为键的字典里。这是「按名字从声明重建后端」的基础，代价是类名成了协议的一部分，重名会互相覆盖。

**存取动作本身很薄。** `PersistenceDecorator.persist_state` 做三件事：拿流程状态、取出 `id` 作为流程 uuid、调后端 `save_state`。取 id 的写法兼容四种状态形态——dict 直接取键、有 `_unwrap()` 的（某些包装状态）先解包、Pydantic 模型或任意有 `id` 属性的对象取属性。没有状态或没有 id 都抛 `ValueError`，后端写入失败包装成 `RuntimeError`。`verbose` 只影响是否往控制台打印，不影响是否写。

**内置后端：SQLite 两张表。**

- `flow_states`：自增主键 `id`、`flow_uuid`、`method_name`、`timestamp`、`state_json`，`flow_uuid` 上有索引。插入即追加，所以历史天然保留。
- `pending_feedback`：`flow_uuid` 唯一、`context_json`、`state_json`、`created_at`。这是给人工反馈暂停用的旁路表（见 d05），用 `INSERT OR REPLACE` 保证一个流程只有一条待反馈记录。

初始化在 Pydantic 的 `mode="after"` 校验器里做（`_setup` 里算锁名并调 `init_db`），所以「构造后端」与「建表」是同一步。`init_db` 对「数据库被锁」有专门的容忍：在 30 秒预算内以 50 毫秒间隔重试建表，只有超过预算或错误不是 locked 才上抛——并发启动多个进程时不会因为建表竞争直接失败。

每次写入都开一条新连接（`PRAGMA journal_mode=WAL`），并在 `store_lock("sqlite:<realpath>")` 里执行；读连接不开锁。锁名用 `realpath` 归一化，使同一文件的不同写法共享同一把锁。状态序列化前先 `_to_state_dict`：Pydantic 模型先试 `model_dump(mode="json")`，失败退到 `mode="python"`，再交给 `json.dumps` 的兜底序列化器（模型 dump、集合转列表、日期转 ISO、其余 `str()`）。

**读取与续跑。** `load_state` 按 `ORDER BY id DESC LIMIT 1` 取最新一条；`load_state_from_history` 按 `id + flow_uuid` 精确取一条历史快照；两者都只接受反序列化结果是 dict 的记录，其他一律当「没有」。`kickoff(inputs={"id": ...})` 时若配了持久化，运行时就会 `load_state` 并把结果灌回状态（见 d03 的三条状态来源）。

**`install_persistence` 与 `reload` 是两条不同的恢复路径。** `install_persistence` 只做两件事：设实例后端、把 `_instance_persistence` 置真——它不解状态，只决定「以后往哪写」。`reload` 吃的是 `FlowExecutionData`：恢复 id、从 `completed_methods` 重建已完成集合，再从 `execution_methods` 里按 `started_at` 排序、倒序找第一个带 `final_state` 的方法取状态（找不到就取最后一个方法的 `initial_state`），最后把除最后一个之外的所有方法加进已完成集合。

这个 `reload` 的状态选取规则值得单独指出：它不信任某个「权威状态字段」，而是从**方法级执行记录**里回溯最近一次真实的状态快照。源码最后那行「把除最后一个之外的方法都标为已完成」的用意是：最后一个方法的状态可能只是它开始时的样子（即没有 `final_state`），所以它不能被当成已完成。这套语义与检查点机制（见 j 组）是两套并行的恢复途径，`kickoff` 里那句「不能同时给 `from_checkpoint` 与 `restore_from_state_id`」的报错就是在划清这条界线。

## 关键取舍

**源码事实：** 持久化粒度是方法完成，不是事务边界。一个方法内部的多次状态改动只会留下最终快照，方法执行到一半崩溃则这次改动完全丢失。这是刻意的简化：要让状态演进全程可恢复，就得要求方法内部也配合标记，成本远高于收益。

**源码事实：** 默认后端在装饰期就实例化。好处是路径错误能在导入阶段暴露；代价是一个模块被 import 就会建一次 SQLite 连接并建表，即使这个流程一次都不会跑。

**源码事实：** 后端的三种来源有一条明确的优先级：实例 > 方法级定义 > 流程级定义 > 默认工厂。源码用 `_instance_persistence` 这个布尔记住「后端是人给的还是引擎推的」，就是为实现这条优先级。

**源码事实：** 状态里必须有 `id` 字段才能持久化。这条要求由 `_create_initial_state` 保证——它给所有状态（包括纯 dict 状态）补一个 uuid。所以「状态的 id」在 CrewAI 里不是可选装饰，而是持久化的主键。

**源码事实：** SQLite 后端的错误处理分两级：结构性错误（无状态、无 id）抛 `ValueError`，写入失败抛 `RuntimeError` 并保留原因链。`verbose` 只控制可见性，不控制严格性——也就是说不可见的失败仍然是失败。

**架构推论：** 追加式快照表 + 索引在 `flow_uuid`，使得「按流程查最新」与「按流程翻历史」都很便宜，代价是存储随方法调用次数线性增长，源码没有提供裁剪或压缩机制。这也是为什么另有检查点机制（按事件触发、按会话管理）而不是把一切都压在 `@persist` 上。

**架构推论：** `FlowPersistence` 是抽象基类 + Pydantic 模型，`__init_subclass__` 自动登记。这套写法让「自定义后端」只需要继承并实现三个抽象方法就能被声明式定义按类名解析。代价是登记表以类名为键，类的重命名会破坏现存声明。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/flow/persistence/decorators.py` | `persist` | 类级/方法级盖章，装饰期确定后端（含默认工厂回退） |
| `lib/crewai/src/crewai/flow/persistence/decorators.py` | `PersistenceDecorator.persist_state` | 从状态取 id 并调后端写入，统一错误与日志 |
| `lib/crewai/src/crewai/flow/persistence/sqlite.py` | `SQLiteFlowPersistence` | 内置后端：两张表、WAL、锁内写入、正反向读取 |
| `lib/crewai/src/crewai/flow/persistence/base.py` | `FlowPersistence` | 抽象接口、待反馈三件可选方法、子类自动登记 |
| `lib/crewai/src/crewai/flow/persistence/factory.py` | `default_flow_persistence` | 默认后端解析：登记工厂优先，否则 SQLite |
| `lib/crewai/src/crewai/flow/runtime/__init__.py` | `_persist_method_completion` | 唯一的持久化调用点，实现四级后端优先级 |
| `lib/crewai/src/crewai/flow/runtime/__init__.py` | `install_persistence` | 装实例后端并把「人给的」标记置真 |
| `lib/crewai/src/crewai/flow/runtime/__init__.py` | `reload` | 从执行数据重建 id、已完成集合与最近快照状态 |

## 参考

- [CrewAI 源码：persistence 包](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/flow/persistence/sqlite.py)
- [CrewAI 文档：Flows 概念](https://docs.crewai.com/en/concepts/flows)
- [SQLite 文档：WAL 模式](https://www.sqlite.org/wal.html)

## 与主核 / AI 的关系

这套机制与主核里的检查点（checkpoint）是同源问题：如何在不确定何时崩溃的前提下，用尽量低的代价换到「可恢复到某个一致性点」。主核的做法是周期性把脏页刷盘并记检查点记录，恢复时从最近检查点重放 WAL；这里的做法是**把应用层的一致性点显式写在代码里**——每个被 `@persist` 标记的方法完成就是一个一致性点，恢复时直接拿快照，不需要重放。

两者的代价结构恰好相反。主核的重放要求操作幂等且日志完备，好处是恢复点密集（不必等业务代码配合）；这里放弃重放，只保留快照，好处是恢复逻辑极简（load 一份 dict 就行），代价是快照之间的所有进度会丢失，且快照是否成立完全取决于方法作者把 `@persist` 放在哪里。

`reload` 里那段「倒序找 `final_state`、找不到就取最后一个的 `initial_state`、除最后一个外都标已完成」的逻辑，与主核里「从 WAL 找最后一个完整提交记录」是同一类保守回溯：都不假设记录一定完整，而是从后往前找一个确实可用的点。

对 AI 工程的实际意义在于：agent 流程的单步成本很高（一次方法可能跑几分钟、花掉不少 token），所以「方法级快照」这个粒度是合适的——比它更细会引入大量序列化开销，比它更粗（流程级）则一崩就前功尽弃。真正需要警惕的是快照本身的不完整性：默认只存状态，不存方法内部的中间产物，也不存待处理的事件集合。后一点在循环流程里会有影响——从快照恢复后 `or_` 监听器的已触发记录是空的，可能重跑某些分支。
