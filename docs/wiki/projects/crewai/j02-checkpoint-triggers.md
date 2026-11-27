---
project: crewai
feature: checkpoint-triggers
title: 检查点配置与触发
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/state/checkpoint_config.py
    symbols: [CheckpointConfig, trigger_events, apply_checkpoint]
  - path: lib/crewai/src/crewai/state/checkpoint_listener.py
    symbols: [_on_any_event, _should_checkpoint, _do_checkpoint]
---

# 检查点配置与触发

## 一句话职责

把「什么事件触发落盘、落到哪儿、用哪个后端、留几份」做成一份配置，再挂一个通配事件监听器，让检查点在指定事件发生时自动写出，全程不需要调用方插手。

## 机制是什么

`CheckpointConfig` 有五个字段：`location`（默认 `./.checkpoints`）、`on_events`（默认 `["task_completed"]`）、`provider`（带判别字段的 `JsonProvider | SqliteProvider` 联合，默认 JSON）、`max_checkpoints`（默认 `None` 表示全留）、`restore_from`（恢复来源）。另有两条属性：`trigger_all`（`"*" in on_events`）与 `trigger_events`（把列表转集合）。

`CheckpointEventType` 是一个巨型 `Literal`，列了约 130 个合法事件名：任务、crew、agent、flow 方法、护栏、工具、记忆、知识、MCP、技能、A2A、信号（`SIGTERM`/`SIGINT`/`SIGHUP`/`SIGTSTP`/`SIGCONT`）、以及几个环境标记（`cc_env`、`codex_env`、`cursor_env`、`default_env`）。把信号也当成事件类型，意味着「进程收到信号时落盘」被表达成「订阅该信号事件」，不需要另起一套机制。

触发逻辑在 `checkpoint_listener.py`：它给每个有 `type` 字段的 `BaseEvent` 子类注册**同一个同步处理器** `_on_any_event`，处理器里判三件事——是否在重放、是否本身是检查点事件、以及源的配置是否要求在这个事件上落盘。

## 怎么实现的

**配置的两处副作用。** `CheckpointConfig` 有两个地方会触发处理器注册。一是 `_coerce_checkpoint`（`BeforeValidator`）——它是 Crew/Flow/Agent 上 `checkpoint` 字段的校验器：`True` 转成 `CheckpointConfig()`，然后只要值是 `CheckpointConfig` 就调 `_ensure_handlers_registered()`。二是 `_register_handlers`（`mode="after"` 的模型校验器），同样调它，并且顺手做一件事：若 provider 是 `SqliteProvider` 而 `location` 没有后缀，自动补 `.db`。

所以「谁能触发注册」有两个入口：`checkpoint=True` 的布尔简写，与显式构造的 `CheckpointConfig`。而且注册是**惰性且一次性的**——`_ensure_handlers_registered` 用模块级布尔加锁做双检。

**处理器注册的规模。** `_register_all_handlers` 递归遍历 `BaseEvent.__subclasses__()`，对每个有 `type` 字段且默认值不是 `"base_event"` 的子类注册 `_on_any_event`。所以是**通配监听**：所有具体事件类型都挂上这个处理器，再由处理器内部判断要不要落盘。源码注释解释了「只注册同步处理器」的理由：同步处理器跑在线程池里，阻塞 I/O 是安全的，而且能避免同步与异步两条分派都写一遍导致重复检查点。

**配置解析的四级回退。** `_find_checkpoint(source)` 按源的类型分派：`Flow` 与 `Crew` 直接读自己的 `checkpoint` 字段；`BaseAgent` 走 `_resolve_from_agent`；`Task` 取它的 `agent` 再走 `_resolve_from_agent`。

`_resolve_from_agent` 先看 agent 自己的配置：是 `CheckpointConfig` 就用；是 `_SENTINEL`（即值为 `False`）就返回 `None` 并**停止向上走**；是 `None` 则继续看 agent 的 `crew` 的配置。

`_resolve` 的三值语义是整套机制的关键：`CheckpointConfig` 表示「用这个」、`_SENTINEL` 表示「显式退出、不再往上找」、`None` 表示「没配置、继续往上找」。`False` 与 `None` 在 pydantic 字段上都可能是「没设」，所以代码用 `_SENTINEL` 这个模块级哨兵对象来区分。

**触发判定。** `_should_checkpoint(source, event)`：先 `_find_checkpoint`，无配置返回 `None`；再 `if not cfg.trigger_all and event.type not in cfg.trigger_events: return None`。所以 `"*"` 走全触发，否则按名单匹配。

**`_on_any_event` 的三道门。** 第一道 `is_replaying()`——重放历史事件时不写检查点，否则恢复过程会不断产生新检查点。第二道 `isinstance(event, (CheckpointBaseEvent, CheckpointForkBaseEvent, CheckpointRestoreBaseEvent))`——检查点自身的事件不再触发检查点，避免递归。第三道 `_should_checkpoint`。全通过才 `_do_checkpoint`，且**整个调用包在 `try/except` 里**，失败只记 warning 不抛——自动检查点失败不该打断运行。

**`_do_checkpoint` 的完整流程。** 先抓三样现场：provider 名、触发事件类型（`trigger`）、上下文四元组（task_id、task_name、agent_id、agent_role，全部从事件上取）。然后快照 `state._parent_id` 与 `state._branch`（因为写完之后这两个会被更新，而事件里要记的是写之前的值）。发 `CheckpointStartedEvent`。`_prepare_entities(state.root)`、`state.model_dump(mode="json")`、若有关联事件就往 payload 里塞一个 `trigger` 字段、`json.dumps`、`cfg.provider.checkpoint(...)`、`state._chain_lineage(...)`、`provider.extract_id(location)` 得到 checkpoint_id。

失败时发 `CheckpointFailedEvent`（带全部上下文）后**重抛**——注意这里与 `_on_any_event` 的 try 层次不同：`_do_checkpoint` 内部选择重抛，由外层 `_on_any_event` 吞掉并 warn。

成功后记一条 info 日志，文案是**可操作的提示**：`f"Checkpoint saved. Resume with: crewai checkpoint resume {checkpoint_id}"`。然后发 `CheckpointCompletedEvent`（带耗时与 id）。最后若 `max_checkpoints` 非空，调 `provider.prune(location, max_checkpoints, branch=branch_snapshot)` 并单独发 `CheckpointPrunedEvent`（带移除数量与上限）。prune 失败只记 warning 并 `return`，不再发事件。

**`apply_checkpoint` 的三分支。** 这是 kickoff 入口用的辅助函数。`from_checkpoint` 为 `None` 直接返回 `None`（正常走）。有 `restore_from` 则 `type(instance).from_checkpoint(from_checkpoint)` 造出恢复实例，并把它的 `checkpoint` 设成一份 `restore_from` 已清空的副本（这样恢复之后的新检查点不会又指回原文件），返回这个实例。有配置但没 `restore_from`，就把配置挂到 `instance.checkpoint` 上并返回 `None`（正常走，顺便开启自动落盘）。

## 关键取舍

**源码事实：** 处理器是**通配注册**，即每个具体事件类型各挂一份引用。事件类型有上百个，所以「注册」这个动作本身有可观的开销；但因为它惰性且一次性，只在第一个 `CheckpointConfig` 出现时才发生。

**源码事实：** `False` 与 `None` 在解析语义上完全不同。`False` 是「显式退出、不再向上找」，`None` 是「没配、继续向上」。这让「crew 开了自动检查点，但某个 agent 明确不要」成为可表达的状态。

**源码事实：** `_on_any_event` 吞掉所有异常，所以**自动检查点失败不会让运行失败**。日志是唯一的痕迹（`logger.warning` 带 `exc_info=True`）。这意味着如果磁盘满或路径不可写，运行会一路跑完而没有任何检查点，用户可能直到需要恢复时才发现。

**源码事实：** 信号事件也在触发名单里（`SIGTERM`、`SIGINT` 等）。所以「进程被终止时落盘」是通过把信号发成事件、再让检查点监听它来实现的——这要求有一个地方真的把信号转成事件，否则这些类型名只是枚举里的空位。

**源码事实：** prune 失败只记 warning 且**直接 return**，不再发 `CheckpointPrunedEvent`。所以「检查点目录无限增长」这件事可能完全静默。

**架构推论：** 把「什么时候落盘」做成按事件名的黑白名单，而不是写死几个时机，让用户能精确表达意图：只在任务完成时（默认）、每个事件都存（`"*"`）、或某个具体里程碑。代价是配置里会出现一长串事件名字符串，拼错不会被 `Literal` 之外的检查拦住——除非用了类型检查。

**架构推论：** 惰性注册是个重要的性能决定。检查点处理器要遍历所有事件子类并逐个注册，若在 import 时就做，所有不使用检查点的程序都要付这份开销。改为「第一个 `CheckpointConfig` 出现时才注册」，把成本绑定到真正使用的人身上。

**架构推论：** `_do_checkpoint` 里对 `parent_id` 与 `branch` 的**先快照后更新**是关键顺序：事件里记的必须是「这次写入基于哪个父」，而 `_chain_lineage` 会把 `_parent_id` 更新成这次自己的 id。若顺序反了，事件里的 parent 会指向自己。

**架构推论：** 「只注册同步处理器」这一句注释藏着一条正确性论证：同步与异步两条分派都会调用处理器，若两条都注册就会写两次检查点。所以这个设计把「检查点是阻塞 I/O」与「不能重复」两个约束一起解掉了。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/state/checkpoint_config.py` | `CheckpointConfig` | 五个字段的落盘配置 |
| `lib/crewai/src/crewai/state/checkpoint_config.py` | `CheckpointConfig.trigger_events` | 把事件名单转集合 |
| `lib/crewai/src/crewai/state/checkpoint_config.py` | `CheckpointConfig.trigger_all` | `"*"` 表示全触发 |
| `lib/crewai/src/crewai/state/checkpoint_config.py` | `_coerce_checkpoint` | 布尔简写转配置并触发处理器注册 |
| `lib/crewai/src/crewai/state/checkpoint_config.py` | `CheckpointEventType` | 约一百三十个合法触发事件名 |
| `lib/crewai/src/crewai/state/checkpoint_config.py` | `apply_checkpoint` | kickoff 入口的三分支：恢复、开启、不动 |
| `lib/crewai/src/crewai/state/checkpoint_listener.py` | `_register_all_handlers` | 给每个具体事件类型注册同步处理器 |
| `lib/crewai/src/crewai/state/checkpoint_listener.py` | `_on_any_event` | 三道门后落盘，异常只警告 |
| `lib/crewai/src/crewai/state/checkpoint_listener.py` | `_should_checkpoint` | 找配置并按名单或 `"*"` 判定 |
| `lib/crewai/src/crewai/state/checkpoint_listener.py` | `_find_checkpoint` | 按源类型分派的四级配置查找 |
| `lib/crewai/src/crewai/state/checkpoint_listener.py` | `_resolve` | 三值语义：配置、显式退出哨兵、未配置 |
| `lib/crewai/src/crewai/state/checkpoint_listener.py` | `_do_checkpoint` | 发事件、写盘、更新血缘、按需裁剪 |
| `lib/crewai/src/crewai/state/checkpoint_listener.py` | `_ensure_handlers_registered` | 双检加锁的惰性一次性注册 |

## 参考

- [CrewAI 源码：state/checkpoint_config.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/state/checkpoint_config.py)
- [CrewAI 源码：state/checkpoint_listener.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/state/checkpoint_listener.py)
- [CrewAI 文档：Crews](https://docs.crewai.com/en/concepts/crews)

## 与主核 / AI 的关系

主核里有直接对应物：检查点进程按固定周期与固定触发条件（检查点记录写入、手动切换）刷盘，触发时机是**编译进代码的策略**，不接受每个用户自定义。CrewAI 把它做成配置，是因为它的使用场景差异极大——短脚本可能只想在任务完成时存一次，长流程可能想每个事件都存。把策略外置成数据，代价是触发判定要在运行期做上百次比较。

与 AutoGen 相比，AutoGen 根本没有自动检查点：`save_state`/`load_state` 是手动调用的，什么时候存由用户代码决定。CrewAI 的这层把「存」从显式调用变成事件驱动，好处是与现有事件体系完全复用（不用发明新的钩子），坏处是检查点行为变成了「隐式发生在事件处理里」——排查时得先知道有这么一个监听器。

对 AI 工程最实用的一条是 `False` 与 `None` 的三值区分。配置继承系统里最常见的问题就是「没设置」与「显式关掉」无法区分，于是子对象一旦被显式设为「不启用」就会被父对象的设置盖回去。用一个模块级哨兵对象来承载「显式退出」，是解决这个问题的干净办法，成本只是一个额外的类型判断。

另一条是 `_do_checkpoint` 里的「先快照后更新」。任何「更新链指针」的操作都有这个顺序陷阱：事件/日志里要记的是更新前的值，而更新本身会覆盖它。把快照放在更新之前，比事后从别处反推便宜且可靠。

最后是那条自动检查点失败只警告的选择。它把「可观测性设施失败」与「业务失败」彻底分开：磁盘写不进去不该让一次正常的 LLM 对话崩掉。但这意味着必须有另外的监控去发现「检查点其实一直没写成功」——否则等到需要恢复的那天才知道它是空的。做这类 fail-open 的设施，配套的「心跳式」健康检查几乎是必需的。
