---
project: crewai
feature: replay-and-checkpoint
title: 重放与执行起点定位
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/crew.py
    symbols: [replay, _get_execution_start_index, _validate_replay_tasks]
---

# 重放与执行起点定位

## 一句话职责

`replay(task_id)` 从某次已落库的执行里定位一个任务的起点，把它之前的输出原样灌回任务对象当上下文，再让循环从该下标继续跑；`_get_execution_start_index` 则负责另一条完全不同的起点计算——检查点续跑。

## 机制是什么

这里有两套起点机制，来源与判据都不同，容易混淆：

**重放（replay）** 的数据源是 `TaskOutputStorageHandler` 落下的 SQLite 记录（表 `latest_kickoff_task_outputs`），它按 `task_id` 找到目标任务的下标，把 `[0, start_index)` 区间的历史输出重新灌进 `tasks[i].output`，然后调 `_execute_tasks(tasks, start_index, was_replayed=True)`。循环靠 `prepare_task_execution` 的跳过分支把已完成任务「只贡献上下文、不重新执行」。

**检查点续跑** 的数据源是恢复出来的 `Crew` 对象自身的任务状态：`_get_execution_start_index` 只看 `checkpoint_kickoff_event_id` 是否非空，非空就从任务列表里找第一个 `output is None` 的下标。它不读写数据库，只看内存里的任务对象。

两者的共同点是都最终落到同一个 `start_index` 参数上；不同点是重放显式传入并允许用户干预（换 `inputs`），续跑则由框架在循环内部静默覆盖。

## 怎么实现的

**`replay` 的六步。**

第一步取数据：`stored_outputs = self._task_output_handler.load()`，空列表直接抛 `ValueError`（提示任务未找到）。

第二步定位：`_find_task_index(task_id, stored_outputs)` 是个 `next()` 生成器，按 `d["task_id"] == str(task_id)` 匹配返回下标，找不到返回 `None`，`replay` 据此抛错。注意它比的是字符串化的 id。

第三步校验：`_validate_replay_tasks(stored_outputs, start_index)`。这是整段里最值得细看的部分，它要解决的问题是「**历史记录对应的还是当前这套任务吗**」。判据分两级：

- 先比 `task_key`。若历史前缀里所有记录都带 `task_key`（该列曾作为后加字段迁移过，见下），就比较 `stored_task_keys != current_task_keys`；不等即抛「current crew does not match the stored task outputs」。同时要求两侧 key 集合去重后数量不变，否则抛「stored task identities are ambiguous」——这是防「两个任务 key 相同导致前缀无法唯一对应」。
- 若无 `task_key`（老库），退化到用 `(output.description, expected_output)` 二元组做身份：先检查两侧该二元组无重复（重复同样抛 ambiguous），再逐条比对 `task.description` 与 `task.expected_output`。

另外它先检查 `len(self.tasks) <= start_index`——当前任务数不足以容纳历史下标时直接抛错，这一条挡住了「历史记录比现在的任务列表长」这种最常见的配置漂移。

`task_key` 的定义是 `description` 与 `expected_output` 的 md5，用的是 `_original_description` / `_original_expected_output`（插值前的原文）而不是插值后的当前值。这一处选择很关键：重放时输入可能变了，插值后的描述会变，但任务身份不该因为这个改变。

第四步取输入：`inputs` 传了就用自己的，没传就取 `stored_outputs[start_index]["inputs"]`（即当初那次运行用的输入）。随后 `self._inputs = replay_inputs` 并 `_interpolate_inputs(replay_inputs)`——重新插值任务描述。

第五步开事件作用域。源码注释解释得很清楚：重放的执行尾段会发 `CrewKickoffCompletedEvent(started_event_id=self._kickoff_event_id)`，所以必须先发一个 `CrewKickoffStartedEvent` 来配对，否则事件总线会警告「未配对的结束事件」，而且完成事件会指向原本那次 kickoff 的 start id。这里还同步 `future.result()` 一次，异常被吞掉。

第六步灌历史并跑：

```
if self.process == Process.hierarchical:
    self._create_manager_agent()
for i in range(start_index):
    stored_output = stored_outputs[i]["output"]
    task_output = TaskOutput(description=..., agent=..., raw=..., pydantic=..., json_dict=..., output_format=..., messages=stored_output.get("messages", []))
    self.tasks[i].output = task_output
self._logging_color = "bold_blue"
return self._execute_tasks(self.tasks, start_index, True)
```

层级流程在这里要**重新造管理智能体**，因为重放不走 `_run_hierarchical_process`，那条路径上的准备被跳过了。异常分支里补发 `CrewKickoffFailedEvent`，注释说明理由：上面的 start 事件开启了作用域，失败的这次重放必须自己关掉，否则泄漏的事件帧会成为后续所有事件的父节点。

**`_get_execution_start_index` 的三行逻辑。** 无 `checkpoint_kickoff_event_id` 返回 `None`（调用方保留默认 `0`）；有则线性扫第一个 `output is None` 的下标；全非空则返回 `len(tasks)`（空列表则是 `None`）。返回 `len(tasks)` 意味着循环体一次都不执行，直接走到 `_create_crew_output` —— 但那里对空 `task_outputs` 会抛 `ValueError`，所以「全部任务都已完成时续跑」实际上会以 `No task outputs available to create crew output.` 收场。这是源码里的一处真实边界。

**存储层的行为决定了重放的可用性。** `TaskOutputStorageHandler.update` 做了一次分支：`log["was_replayed"]` 为真时走 `storage.update(task_index, ...)`（按 `task_index` UPDATE 已有行，字段只含 `task_id`、`task_key`、`expected_output`、`output`、`was_replayed`、`inputs`），否则走 `storage.add(...)`（`INSERT OR REPLACE`，主键是 `task_id`）。也就是说：**正常执行是插入，重放是原地更新，且更新按 `task_index` 定位**。如果重放时的任务顺序与历史不一致（尽管有校验）就会更新错行。

底层 `KickoffTaskOutputsSQLiteStorage` 建表时的列是 `task_id`（主键）、`task_key`、`expected_output`、`output`（JSON）、`task_index`、`inputs`（JSON）、`was_replayed`、`timestamp`；`_initialize_db` 里显式 `PRAGMA table_info` 检查后 `ALTER TABLE ADD COLUMN task_key`——这就是上面「老库无 key 时退化比对」的由来。所有写操作都在 `store_lock("sqlite:<realpath>")` 里，并开 `PRAGMA journal_mode=WAL`，`load()` 则是唯一不加锁的读。

**重放时条件任务与异步任务的连带影响。** 因为 `was_replayed=True` 被一路传到 `check_conditional_skip` 与 `_store_execution_log`：条件任务被跳过时不写日志（`if not was_replayed`），而正常执行的任务在重放路径上也会写日志（`_store_execution_log(..., was_replayed)`），只是字段走 update 分支。

## 关键取舍

**源码事实：** 重放的起点由用户按 `task_id` 指定，续跑的起点由框架按「第一个无输出的任务」推导。前者允许从任意历史点重来，后者只能从头补齐。

**源码事实：** 任务身份用 `task_key`（由**插值前**的描述与期望输出算出的 md5）而不是插值后的文本，因此「换了输入但任务定义没变」的重放能通过校验。

**源码事实：** 兼容老库时退化到 `(description, expected_output)` 二元组比对，并要求两侧都无重复。这是「宁可拒绝重放，也不要把历史输出灌错任务」的取舍。

**源码事实：** 全部任务都有输出时 `_get_execution_start_index` 返回 `len(tasks)`，而 `_create_crew_output` 拒绝空输出列表，于是续跑会报错而不是返回上一次的结果。这是一个刻意不做特判的边界——「没有新输出」不算一次成功的执行。

**源码事实：** 丢弃 `was_replayed` 的历史记录在 `update` 里只更新六个字段，`timestamp` 由表默认值接管（UPDATE 不改它，因此保留首次写入时间）。

**架构推论：** 重放不重建执行环境（不重新插值工具、不重置智能体），它只在任务对象上补 `output`，所以重放的可信度取决于「任务对象仍是同一批」。`_validate_replay_tasks` 正是这条假设的唯一守卫，删掉它重放就会变成静默的数据污染源。

**架构推论：** 把重放的起点语义做成「灌历史 + 从下标继续」而不是「重跑整个流程并跳过已完成」的另一条实现，好处是循环只有一份；代价是重放路径绕过了 `_run_hierarchical_process` 等准备逻辑，必须逐处手工补齐（例如管理智能体创建、事件配对）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/crew.py` | `replay` | 重放入口：定位、校验、取输入、开事件、灌历史、从下标续跑 |
| `lib/crewai/src/crewai/crew.py` | `_get_execution_start_index` | 检查点续跑的起点：第一个无输出的任务下标 |
| `lib/crewai/src/crewai/crew.py` | `_validate_replay_tasks` | 校验历史输出与当前任务身份一致，含 `task_key` 与二元组两级判据 |
| `lib/crewai/src/crewai/crew.py` | `_find_task_index` | 按 `task_id` 在历史记录里定位下标 |
| `lib/crewai/src/crewai/crew.py` | `_store_execution_log` | 组装并写入任务输出的落库记录 |
| `lib/crewai/src/crewai/utilities/task_output_storage_handler.py` | `TaskOutputStorageHandler` | 存储门面：正常走 add，重放走 update |
| `lib/crewai/src/crewai/memory/storage/kickoff_task_outputs_storage.py` | `KickoffTaskOutputsSQLiteStorage` | SQLite 实装：建表、`task_key` 迁移、加锁写、无锁读 |

## 参考

- [CrewAI 源码：crew.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/crew.py)
- [CrewAI 源码：kickoff_task_outputs_storage.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/memory/storage/kickoff_task_outputs_storage.py)
- [CrewAI 文档：任务重放](https://docs.crewai.com/en/concepts/tasks#task-replay)

## 与主核 / AI 的关系

重放与检查点续跑，本质上都是「**用持久化的中间状态换确定性**」。这与主核的 WAL 重做是同构的：主核把已提交的日志按 LSN 顺序重放，跳过已经落盘的部分，只补未完成的部分；这里把已完成任务的输出按 `task_index` 顺序灌回，只补未完成的任务。差别在一致性保证的强度——主核的重放有 LSN 与校验和，能证明「这段日志确实属于这个页」；这里的重放只有 `task_key` 与描述文本比对，属于**启发式校验**，挡得住配置漂移但挡不住语义漂移（描述一字不差但期望的输出含义变了）。

对 AI 工程的实际意义在于：LLM 流水线的重跑成本很高，所以「从中间某步重来」是刚需。但这套机制有个根本前提——任务的定义必须稳定。一旦改了任务描述，历史输出就与新的任务不再是同一件事，此时正确的做法不是重放而是从头跑。`_validate_replay_tasks` 那些听起来过度的重复性检查（ambiguous 判断），正是在替人把这个判断自动化：它能识别「我自己都不确定这两条记录谁是谁」，而不是硬灌下去。
