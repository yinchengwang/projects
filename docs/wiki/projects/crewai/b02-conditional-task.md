---
project: crewai
feature: conditional-task
title: 条件任务的跳过判定
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/crew.py
    symbols: [_handle_conditional_task, _ahandle_conditional_task]
---

# 条件任务的跳过判定

## 一句话职责

条件任务在执行前先被问一句「上一个任务的输出满足条件吗」，不满足就带着一个空输出被跳过；判定逻辑由 `ConditionalTask.should_execute` 提供，触发时机与屏障顺序由 `_handle_conditional_task` 负责。

## 机制是什么

这不是「调度器决定跳过」，而是「任务列表里预留了位置、运行时可能填一个空结果」。被跳过的条件任务并不会从 `task_outputs` 里消失——它会追加一个 `raw=""` 的 `TaskOutput`，`output_format` 固定为 RAW。

这个设计有一个直接后果：**任务下标与输出下标始终对齐**。第 i 个任务无论执行与否，都在 `task_outputs[i]` 上留下痕迹。正因此，重放的 `start_index`、`_store_execution_log` 的 `task_index`、以及后续任务的上下文拼接都能继续工作——否则中间少一项，后面全部错位。

判据的来源是「上一项输出」：`previous_output = task_outputs[-1] if task_outputs else None`。所以条件任务**只看它前面那一项**，不是看全部历史。这条规则与团队校验里的 `validate_first_task`（首任务不能是条件任务）互为表里——首任务前面没有输出，判定无从谈起。

## 怎么实现的

**同步路径的完整脉络。** `_execute_tasks` 的循环里，条件任务被单独识别：`isinstance(task, ConditionalTask)` 为真就转 `_handle_conditional_task(task, task_outputs, futures, task_index, was_replayed)`。该方法第一步是**回收所有待决 future**（`if futures: task_outputs.extend(self._process_async_tasks(futures, was_replayed)); futures.clear()`），第二步 `return check_conditional_skip(self, task, task_outputs, task_index, was_replayed)`。异步版本 `_ahandle_conditional_task` 完全对应，只是回收换成 `await self._aprocess_async_tasks(...)`，判定函数相同。

**为什么要先回收 future。** 判定的输入是 `task_outputs[-1]`。若前一个任务是异步的，它的输出此刻还在 `Future` 里没被追加，`task_outputs[-1]` 会退化成更早的同步结论——一个完全错误的判定依据。所以「条件判定必须发生在同步屏障之后」是由代码顺序保证的硬约束，而不是文档里的建议。

这也解释了团队校验里的那条：`validate_async_tasks_not_async` 禁止条件任务本身带 `async_execution`。反过来「条件任务前可以有异步任务」是允许的，靠的就是这次强制回收。两条规则合起来的意思是：**异步可以在条件任务之前，但不能是条件任务本身，也不能隔着零个同步任务引用另一个异步任务的输出**。

**`check_conditional_skip` 的四步。** 它住在 `crews/utils.py`，与 `prepare_task_execution` 相邻：

1. 取 `previous_output`，为空则返回 `None`（不跳过，正常执行）。
2. `task.should_execute(previous_output)` 为假才继续，否则返回 `None`。
3. 记一条 debug 日志 `Skipping conditional task: <描述>`（颜色 yellow）。
4. `skipped_task_output = task.get_skipped_task_output()`；`if not was_replayed: crew._store_execution_log(task, skipped_task_output, task_index)`；返回该输出。

注意第 4 步的条件：**重放路径不写跳过日志**。理由是重放时历史记录已经在库里，再写一次会与已有行冲突（`update` 按 `task_index` 定位，会把一条真实的历史输出覆盖成空输出）。这一处是「跳过也是执行结果」与「重放不篡改历史」两个原则的取舍点。

**`should_execute` 只做一件事。** `ConditionalTask.should_execute(context)` 的实现是 `if self.condition is None: raise ValueError("No condition function set for conditional task"); return bool(self.condition(context))`。也就是说条件函数收到的是整个 `TaskOutput` 对象而不是字符串——用户可以读 `.raw`、`.pydantic`、`.json_dict`、`.agent` 等任意字段，也可以按 `output_format` 分支处理。返回值被 `bool()` 强制转换，因此返回 `None`、非空字符串、非空 list 都能安全落到布尔语义。

`condition` 字段的类型是 `SerializableCallable | None`，且有一个看起来很奇怪的 `__init__` 重写：先 `super().__init__(**kwargs)` 再 `self.condition = condition`。原因在于 Pydantic 模型字段构造与位置参数的时序——把 `condition` 从关键字参数里提出来单独赋值，避免它与 Pydantic 的字段初始化顺序打架。

**被跳过输出的形状。** `get_skipped_task_output` 返回 `TaskOutput(description=self.description, raw="", agent=self.agent.role if self.agent else "", output_format=OutputFormat.RAW)`。三个字段值得注意：`raw` 是空串（而不是 `None` 或某个占位文案），所以后续任务拼上下文时它会贡献一个**空片段**——连同上一条的分隔符，上下文里会出现一个「空段」，但不会引入误导性文本。`agent` 取角色名，没有智能体时退化为空串。`output_format` 固定 RAW，即使任务配了 `output_json`——因为确实没有 JSON 可给。

这里还有一个连锁效应：`_create_crew_output` 会用 `valid_outputs = [t for t in task_outputs if t.raw]` 过滤掉空 `raw` 的输出，再取最后一个当最终结果。因此**如果流水线最后一项是被跳过的条件任务，最终输出会回退到最近一个真正有内容的任务**，而不是空串。

**与重放的交互。** `_validate_replay_tasks` 比对任务身份时用的是 `task.description` 与 `task.key`，条件任务与其他任务没有区别对待。而重放灌历史时（`replay` 里的 `for i in range(start_index)`）对跳过任务的空 `raw` 也会照原样重建：`TaskOutput(description=..., agent=..., raw=..., ...)`，由于存库时存的就是空串，恢复后依然是空串。语义一致。

**团队层的三条校验是这套机制的前置约束**（细节见 a01）：首任务不能是条件任务、必须至少有一个非条件任务、条件任务不能异步。第三条最关键——如果条件任务被异步化，它的判定会发生在无人回收的时机，`task_outputs[-1]` 的语义就崩了。

## 关键取舍

**源码事实：** 跳过不产生异常、不产生缺项，而是产生一个 `raw=""`、格式为 RAW 的占位输出。这让下游的下标对齐与上下文拼接零成本，代价是下游必须知道「空 raw 意味着跳过」而不是「任务产出了空内容」——两者在数据结构上无法区分。

**源码事实：** 判定输入只有「紧邻的上一个输出」，不是全部历史。要基于更早的输出做判断，只能通过任务自身的 `context` 去取（但那属于上下文构造，不参与跳过判定）。

**源码事实：** 回收 future 的动作在判定之前无条件执行，哪怕条件任务的 `condition` 最终返回真、任务还是要正常跑。这是「宁可多一次屏障，也不让判定读错数据」的选择。

**源码事实：** 重放时跳过的条件任务不写日志（`if not was_replayed`）。跳过是「本可以发生但没发生」的结果，重放不该把它当成新的历史事实。

**架构推论：** 条件判定被实现为「客户端提供的可调用对象」而不是声明式表达式，因此判定可以任意复杂（读 JSON、查外部状态、调模型）。代价是判定逻辑不可序列化、不可审计——源码里 `condition` 用 `SerializableCallable` 包装，但真正跨进程恢复时它只能退化或丢失。

**架构推论：** 「跳过也留位」这个决定让条件任务成为**流水线里的可选槽位**，而不是分支跳转。它无法表达「A 或 B 二选一」，只能表达「B 可能不发生」。要表达分支就得用 Flow 的路由机制（另一个子系统），而不是任务列表。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/crew.py` | `_handle_conditional_task` | 同步路径：先回收 future 再判定，返回跳过输出或 `None` |
| `lib/crewai/src/crewai/crew.py` | `_ahandle_conditional_task` | 异步路径的同构实现，回收改 `await` |
| `lib/crewai/src/crewai/crew.py` | `_execute_tasks` | 条件任务分支的调用点，按返回是否真值决定追加跳过输出并 `continue` |
| `lib/crewai/src/crewai/crews/utils.py` | `check_conditional_skip` | 真正的判定：取上一输出、调 `should_execute`、造跳过输出、按重放与否决定写日志 |
| `lib/crewai/src/crewai/tasks/conditional_task.py` | `ConditionalTask` | 条件任务类型，`condition` 字段与 `should_execute` 的实现 |
| `lib/crewai/src/crewai/tasks/conditional_task.py` | `get_skipped_task_output` | 跳过时的占位输出：空 `raw`、RAW 格式 |

## 参考

- [CrewAI 源码：conditional_task.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/tasks/conditional_task.py)
- [CrewAI 源码：crews/utils.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/crews/utils.py)
- [CrewAI 文档：条件任务](https://docs.crewai.com/en/concepts/tasks#conditional-tasks)

## 与主核 / AI 的关系

「预留槽位 + 可能填空」这个做法，在主核里对应的不是条件分支而是**稀疏数据结构**：定长数组里某些位置允许为空，遍历时长度不变。主核处理「这一步可能不执行」的常规手段是显式的分支跳转或状态机，因为存储层的槽位一旦预留就要占空间；而在任务流水线里，槽位是内存里的列表元素，占位成本几乎为零。

对 AI 工程的实际意义在于：条件任务把「是否执行」这个不确定性收窄成了「执行 or 空输出」两种形状，从而让下游仍然可以无脑地按序号取上下文。这是「用结构吸收不确定性」的典型手法——代价是下游无法从数据结构上区分「跳过」与「真的产出了空内容」，只能在语义约定上依赖空串这个标记。这也提醒一件事：一旦有人真的让某个任务合法地产出空 `raw`，跳过判定与「无输出」就会混淆，团队最后结果也可能被静默回退到更早的任务上。
