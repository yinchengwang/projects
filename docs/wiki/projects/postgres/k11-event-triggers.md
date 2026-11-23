---
project: postgres
feature: event-triggers
page_type: feature
schema_version: 1
refs:
  - path: src/backend/commands/event_trigger.c
    symbols: [CreateEventTrigger, EventTriggerDDLCommandStart, EventTriggerInvoke, filter_event_trigger]
---

# 事件触发器

## 一句话职责

事件触发器在 DDL 语句前后按事件名与命令标签筛选已注册的触发函数，并在独立内存上下文中依次调用它们。

## 机制是什么

与挂在单张表上的 DML 触发器不同，事件触发器注册在数据库级，按“事件”（`ddl_command_start`、`ddl_command_end`、`sql_drop`、`table_rewrite`、`login`）以及可选的命令标签集合来匹配。注册信息存在 `pg_event_trigger`：名字、事件、所有者、函数 OID、启用状态与标签数组。执行前由事件缓存给出该事件的候选条目，再按会话复制角色与标签集过滤，最后形成待调用函数 OID 列表。

本机制负责注册校验、事件捕获点的套接字式派发、按标签与复制角色过滤、函数调用以及事件之间的命令计数器推进。它不负责 DDL 语句本身的解析与执行、不负责对象删除列表的收集（那在依赖与删除路径中完成，本机制只消费结果）、也不负责触发函数内部的语言级语义。事件触发器也明确不支持针对事件触发器自身以及若干全局对象类型，避免自我递归与跨库生效问题。

## 怎么实现的

1. `CreateEventTrigger` 先做权限与命名校验：仅超级用户可创建（源码注释说明放开给库所有者会带来明显的提权风险）；`eventname` 必须是五个已知事件之一；`whenclause` 只接受 `tag` 一个过滤变量且不得重复。
2. 标签校验按事件分流：`ddl_command_start`/`ddl_command_end`/`sql_drop` 走 `validate_ddl_tags`，逐个标签用 `GetCommandTagEnum` 解析并要求 `command_tag_event_trigger_ok`；`table_rewrite` 走 `validate_table_rewrite_tags`；`login` 事件则直接报“不支持标签过滤”。
3. 名字重复与函数契约随后校验：`SearchSysCache1(EVENTTRIGGERNAME, ...)` 命中即报对象已存在；`LookupFuncName` 找到函数后必须确认返回类型是 `EVENT_TRIGGEROID`。通过后由 `insert_event_trigger_tuple` 写入 `pg_event_trigger`，默认启用状态为 `TRIGGER_FIRES_ON_ORIGIN`，无标签时把 `evttags` 置 NULL。
4. 执行期先经过两道总开关：`EventTriggerDDLCommandStart` 在 `!IsUnderPostmaster` 或 `event_triggers` GUC 关闭时直接返回。源码注释给出两个理由：足够糟糕的事件触发器可能让数据库不可用并且无法自行关闭，此时单用户模式是逃生口；事件缓存构建依赖有序的系统表扫描，索引损坏时需要一种“事件触发器不参与”的模式来恢复。
5. `EventTriggerCommonSetup` 从 `EventCacheLookup(event)` 取候选列表，为空立即返回；再用 `EventTriggerGetTag` 得出命令标签（`login` 事件固定为 `CMDTAG_LOGIN`，其余由 `CreateCommandTag` 从语法树推断）。在断言构建里还会校验“这里见到的标签必须恰好是 `CREATE EVENT TRIGGER` 接受的那批”，注释说明这是为了防止两个控制点漂移。
6. 过滤通过 `filter_event_trigger` 完成：先按 `SessionReplicationRole` 判断，复制角色为 replica 时丢弃 `TRIGGER_FIRES_ON_ORIGIN` 的条目，否则丢弃 `TRIGGER_FIRES_ON_REPLICA` 的条目（源码注明到达这里时已不会看到 disabled 条目）；再按标签集判断，`tagset` 非空且不含当前标签即丢弃。通过者把 `fnoid` 追加进 `runlist`。
7. 形成 `runlist` 的过程被刻意放在“接触任何 catalog 之前”：源码注释说明一旦开始运行触发器或做任何触碰 catalog 的操作，缓存失效可能让 `cachelist` 指向垃圾，所以必须先复制出 OID 列表。`runlist` 为空则不再构造 `EventTriggerData`。
8. `EventTriggerInvoke` 建立名为 `event trigger context` 的独立内存上下文，逐个 `fmgr_info` 并 `FunctionCallInvoke`，把 `trigdata` 作为上下文指针传入且不传参数。第一个之后的每次调用前先 `CommandCounterIncrement`，使后一个触发器能看到前一个的效果；每次调用后 `MemoryContextReset`，最后切换回原上下文并删除临时上下文。入口处 `check_stack_depth` 防止递归事件触发器打爆栈。
9. 各事件在调用前后安排命令计数器：`ddl_command_start` 在调用后 `CommandCounterIncrement` 让触发器所做的改动对主命令可见；`ddl_command_end` 则在调用前先推进，让主命令的改动对触发器可见。`ddl_command_end` 与 `sql_drop` 还额外检查 `currentEventTriggerState`：若本次 DDL 开始时没有任何相关事件触发器，状态就不会建立，此时即便中途新建了触发器也不执行，否则触发器函数调用 `pg_event_trigger_ddl_commands` 会失败。

## 关键取舍

- 源码事实：`CreateEventTrigger` 硬性要求超级用户，并在注释中写明“放开给库所有者或普通用户会带来必须先堵住的提权风险”。放弃更细粒度的创建权限换取不引入新的提权面；代价是运维必须用高权限账号管理事件触发器。
- 源码事实：`filter_event_trigger` 先按 `SessionReplicationRole` 与启用状态过滤，再按标签集过滤，且注释说明 disabled 条目根本不会进入到这一层。放弃在缓存层就完成全部过滤使缓存条目保持中立、可被不同会话状态复用；代价是每次事件派发都要重复一遍角色判断。
- 源码事实：`EventTriggerCommonSetup` 在扫描缓存后立刻把命中的 `fnoid` 复制成 OID 列表，注释明确理由是“缓存失效可能让 cachelist 指向垃圾”。放弃直接持有缓存条目指针边遍历边调用换取不发生悬空引用；代价是多一次 OID 列表分配与释放。
- 源码事实：`EventTriggerDDLCommandStart` 在单用户模式或 GUC 关闭时完全不派发。放弃“任何模式下都保持一致行为”换取一个能从坏触发器里脱身的恢复通道；代价是同类 DDL 在单用户模式下不会触发事件，排障时容易被误判为触发器失效。
- 源码事实：`ddl_command_end`/`sql_drop` 依赖 `currentEventTriggerState` 是否存在来决定是否执行，注释给出的理由是防止“命令中途新建的触发器”被调用后访问不到 `pg_event_trigger_ddl_commands` 所需状态。放弃“按最新注册集合执行”换取触发器函数所依赖的查询接口始终有数据可读；代价是同一语句内新建的事件触发器不会在本语句生效。
- 源码事实：`EventTriggerInvoke` 为每次调用重置独立上下文并在触发器之间推进命令计数器。放弃“所有触发器共享一个上下文且不推进计数”换来内存不随触发器数量累积、且后序触发器可见前序效果；代价是每次调用后丢失前一触发器留在上下文中的中间状态，触发器不能依赖跨调用共享的临时对象。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/commands/event_trigger.c` | `CreateEventTrigger` | 校验权限、事件名、过滤变量与函数返回类型并写入 `pg_event_trigger` |
| `src/backend/commands/event_trigger.c` | `EventTriggerDDLCommandStart` | DDL 命令开始时的总开关判断、候选构造、调用与命令计数器推进 |
| `src/backend/commands/event_trigger.c` | `EventTriggerInvoke` | 在独立上下文中逐个调用触发函数，调用间推进命令计数器并重置内存 |
| `src/backend/commands/event_trigger.c` | `filter_event_trigger` | 按会话复制角色与启用状态、再按标签集决定某条目是否参触发 |

## 参考

- [PostgreSQL 文档：事件触发器](https://www.postgresql.org/docs/current/event-triggers.html) —— 支撑事件名、标签过滤与触发函数接口的整体说明。
- [PostgreSQL 文档：CREATE EVENT TRIGGER](https://www.postgresql.org/docs/current/sql-createeventtrigger.html) —— 支撑 `WHEN TAG` 过滤语法与创建时的校验要求。
- [PostgreSQL 文档：事件触发器函数](https://www.postgresql.org/docs/current/functions-event-triggers.html) —— 支撑 `pg_event_trigger_ddl_commands` 等函数对查询期状态的依赖。
- [PostgreSQL 官方源码：事件触发器实现](https://github.com/postgres/postgres/blob/master/src/backend/commands/event_trigger.c) —— 支撑注册校验、缓存过滤与调用循环细节。

## 与主核 / AI 的关系

暂无直接对应。主核若要引入 DDL 钩子，可借鉴“注册时严校验、派发前先复制待调用列表、单用户模式保留逃生开关、触发器之间推进可见性”四点；但事件语义、标签枚举与函数返回类型契约需按主核自身的扩展模型定义。
