---
project: crewai
feature: task-execution-core
title: Task 执行内核
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/task.py
    symbols: [Task, execute_sync, _execute_core]
  - path: lib/crewai/src/crewai/task.py
    symbols: [execute_async, aexecute_sync, _aexecute_core]
---

# Task 执行内核

## 一句话职责

`execute_sync` / `aexecute_sync` 是对外的两个薄壳，各自只做「记开始时间 + 转交」，真正的执行内核 `_execute_core` / `_aexecute_core` 负责一次任务运行的完整边界：设上下文、发事件、跑智能体、造 `TaskOutput`、过护栏、落文件、跑回调。

## 机制是什么

Task 的执行被刻意分成**壳与核**两层，而且核有两份（同步与原生异步并行实现，不是互相包装）。这种「写两遍」的选择换来的是一致的语义：两条路径都经历同样的七个阶段，只是等待方式不同。

一条任务运行的生命周期，在 `_execute_core` 里是这样一个固定序列：

1. 装配上下文（`set_current_task_id`、`_store_input_files`、把 `agent` 参数或 `self.agent` 定为执行者、`prompt_context = context`）。
2. 发出 `TaskStartedEvent`——但如果智能体执行器处于恢复状态并且 `resume_task_scope` 命中，就跳过，避免重复开事件作用域。
3. `PRE_STEP` 拦截点（钩子可以改写 `context`）。
4. 在 `tool_failure_collector()` 里调 `agent.execute_task(task=..., context=..., tools=...)`。
5. 把原始返回值规范化成 `raw` / `pydantic_output` / `json_output` 三件套，造出 `TaskOutput`。
6. 跑护栏链（`_guardrails` 列表逐条、然后单条 `_guardrail`），护栏可能替换输出或抛异常。
7. `POST_STEP` 拦截点、写 `output_file`、存 artifact、跑 `self.callback` 与 `crew.task_callback`、发 `TaskCompletedEvent`。

异常路径统一为「补 `end_time` + 发 `TaskFailedEvent` + 原样抛出」，`finally` 里清掉任务级文件并复位 `current_task_id`。

## 怎么实现的

**壳层为什么这么薄。** `execute_sync` 只有两行：`self.start_time = datetime.datetime.now()` 然后 `return self._execute_core(agent, context, tools)`。`aexecute_sync` 同理但转给 `_aexecute_core`。计时起点放在壳里，意味着**所有非任务自身的耗时（准备提示词、取上下文、工具注入）都会被算进任务时长**，而 `execution_duration` 用 `end_time - start_time` 直接取。

**智能体的空值边界。** 核内第一件事是 `agent = agent or self.agent`，然后 `self.agent = agent`——注意这里**回写**了 `self.agent`，所以一次无参执行会永久把执行者绑到任务上。紧接着若无 agent 就抛异常，文案明确说明「应通过支持此行为的流程（如层级流程）在 Crew 中执行」。这是「任务可以不带智能体，但只在能被指派的地方」的分界点。

**工具列表的解析顺序。** `tools = tools or self.tools or []`。也就是说：调用方传的优先（`prepare_task_execution` 传来的已经做过委派与记忆工具注入），其次任务自身的 `tools`，最后空列表。这条链与 `check_tools` 校验器互补：构造期若 `tools` 为空且智能体有工具，就把智能体的工具复制到任务上，因此靠默认配置也能走通。

**原始结果到三件套。** 这段分支逻辑是本页最需要精确描述的部分：

- 若 `result` 是 `BaseModel`：`raw = result.model_dump_json()`；配了 `output_pydantic` 就把这个模型直接当 `pydantic_output`、`json_output` 为 `None`；否则配了 `output_json` 就 `result.model_dump(mode="json")` 当 `json_output`、`pydantic_output` 为 `None`；两者都没配则两者皆 `None`。
- 若 `result` 不是 `BaseModel` **且没有任何护栏**：`raw = result`，再调 `_export_output(result)` 做结构化导出（内部用 `convert_to_model` + `_unpack_model_output`）。
- 若 `result` 不是 `BaseModel` **但有护栏**：`raw = result`，`pydantic_output` 与 `json_output` 一律置 `None`。

第三条分支是刻意留白：**护栏还没跑，此时导出的结构化结果可能被护栏改掉**，所以延后。`_apply_guardrail_result` 里有一句注释直接承认了这个设计——「Outputs are left unexported while guardrails run, so one passed straight through may still lack the configured structured field」，并在护栏通过后又补一次 `_export_output`。

**`TaskOutput` 的构造字段。** `name=self.name or self.description`（没名字就用描述当名字），`description`、`expected_output` 原样带上，`raw`、`pydantic`、`json_dict` 如上，`agent=agent.role`（存角色字符串而不是对象，为序列化友好），`output_format=self._get_output_format()`（`output_json` 优先 → JSON，`output_pydantic` → PYDANTIC，否则 RAW），`messages=agent.last_messages`（智能体最近一轮对话，便于审计），`tool_failures=list(execution_failures)`（从收集器里取出本任务期间所有工具失败记录）。

`TaskOutput` 自身还有一个 `after` 校验器 `set_summary`，把 `description` 前 10 个词拼成 `"<摘要>..."` 当 `summary`——纯截断，不做摘要模型调用。

**护栏的调用位置。** 护栏在 `TaskOutput` 造好之后、`POST_STEP` 之前跑。列表 `_guardrails` 逐条调用 `_invoke_guardrail_function(..., guardrail_index=idx)`，每次返回的 `task_output` 成为下一条的输入；然后单条 `_guardrail` 再跑一次。注意 `_invoke_guardrail_function` 内部会**重新执行智能体**（护栏失败时），用的是原始 `agent.execute_task`，所以护栏的每一次重试都是一整次任务重跑，而不是只重跑模型。

**落盘与产物。** `if self.output_file: self._save_file(self._output_content(task_output))`——内容优先级是 `json_dict` > `pydantic.model_dump_json()` > `raw`。`_save_file` 用 `Path(...).expanduser().resolve()` 解析，`create_directory` 为真时自动建父目录，否则目录不存在就抛 `RuntimeError`；写文件时 dict 走 `json.dump(ensure_ascii=False, indent=2)`，其余 `str()`。OSError 被包成 `RuntimeError` 并在消息里附上「用 crewai_tools 的 FileWriterTool」的建议。`artifact` 为真时 `_save_artifact` 把内容交给 `save_output_artifact`，失败只记 warning 并返回 `None`——artifact 是增强能力，不该让任务失败。

**两条回调。** 先 `self.callback`，再判断团队级回调：`crew = self.agent.crew`，条件写成 `crew and not isinstance(crew, str) and crew.task_callback and crew.task_callback != self.callback`——两道防御很有信息量：`crew` 字段允许是字符串（序列化引用），所以必须排除字符串；再排除与任务回调相同的可调用对象，避免同一次执行回调两次。同步路径用 `inspect.iscoroutine(cb_result)` + `asyncio.run`，异步路径用 `inspect.isawaitable` + `await`。

**异步核与同步核的四处差异**（都不是语义差异，是实现手段差异）：`result = await agent.aexecute_task(...)`；结构化导出用 `_aexport_output`（内部 `await async_convert_to_model`，注释说明是为了不阻塞事件循环）；artifact 保存用 `asyncio.to_thread(self._save_artifact, ...)`（`_save_artifact` 里有同步 IO 与 `json.dumps`）；回调判定用 `isawaitable` 以兼容返回 coroutine 或 future 的情况。

**`execute_async` 是第三套路径。** 它不用 `asyncio`，而是「`Future` + 守护线程 + `contextvars.copy_context()`」，线程的 target 是 `_execute_task_async`，后者只做「计时 → `_execute_core` → `future.set_result` / `set_exception`」。所以 `execute_async` 实际上是「在另一个线程里同步执行」，与 `aexecute_sync` 的原生异步不是同一件事。这一点在任务循环里被明确体现：同步流程用 `execute_async` + `Future`，异步流程用 `aexecute_sync` + `create_task`。

**`finally` 的两件事。** `clear_task_files(self.id)` 清掉任务级输入文件缓存，`reset_current_task_id(task_id_token)` 复位 contextvar。这两步保证即使任务失败，文件与上下文标识也不会泄漏到下一个任务。

## 关键取舍

**源码事实：** 有护栏时不预先导出结构化输出（`pydantic`/`json_dict` 留空），等护栏通过后再补导。代价是未配护栏与配了护栏的路径产出时机不同，好处是护栏的改写结果不会被旧的导出结果覆盖。

**源码事实：** 同步核与异步核是两份独立代码而非「异步壳套同步核」。两者逻辑完全对齐但需要同步维护，换来的是原生异步不阻塞事件循环。

**源码事实：** `execute_async` 走线程，`aexecute_sync` 走协程。同一个 Task 上有三条执行路径（`_execute_core`、`_aexecute_core`、线程包装的 `_execute_task_async`），第三条只是为了复用同步核。

**源码事实：** `_execute_core` 会把 `agent` 回写到 `self.agent`。这让「一次运行之后任务就绑定了执行者」，也让同一个 `Task` 对象跨团队复用时会带着上一个团队的智能体引用。

**源码事实：** `TaskStartedEvent` 在恢复场景下被故意跳过（`executor._resuming and resume_task_scope(...)`）。检查点恢复时事件作用域已经存在，重发会让同一次执行出现两个 start。

**源码事实：** `artifact` 保存失败只记 warning 返回 `None`；而 `output_file` 写入失败会抛 `RuntimeError`。两者的严格程度不同，因为文件写入是用户显式声明的主路径，artifact 是可选的附加产物。

**架构推论：** 把「护栏重试」实现为「重跑整个任务」而不是「只重跑模型调用」，使护栏可以依赖工具调用的副作用与完整上下文；代价是一次失败的重试成本等于一次完整任务。

**架构推论：** 任务不持有执行循环（循环在 Crew 侧），它只保证「一次运行」的原子边界。这个切分让层级流程能让管理智能体指派任意任务，而不必把调度逻辑塞进任务。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/task.py` | `Task` | 任务模型：描述、期望输出、输出格式、工具、护栏、回调、`output` |
| `lib/crewai/src/crewai/task.py` | `execute_sync` | 同步壳：计时后转交内核 |
| `lib/crewai/src/crewai/task.py` | `_execute_core` | 同步内核：上下文、事件、钩子、智能体调用、输出构造、护栏、落盘、回调 |
| `lib/crewai/src/crewai/task.py` | `execute_async` | 线程 + `Future` 外壳，复制 contextvars 后在线程内跑同步核 |
| `lib/crewai/src/crewai/task.py` | `aexecute_sync` | 原生异步壳：计时后转交异步内核 |
| `lib/crewai/src/crewai/task.py` | `_aexecute_core` | 异步内核：与同步核阶段一一对应，用 `await` 版智能体与导出 |
| `lib/crewai/src/crewai/task.py` | `_apply_guardrail_result` | 护栏通过后把结果套回输出并按需补导出 |
| `lib/crewai/src/crewai/tasks/task_output.py` | `TaskOutput` | 任务结果载体，含 `raw`/`pydantic`/`json_dict` 三件套与 `tool_failures` |

## 参考

- [CrewAI 源码：task.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/task.py)
- [CrewAI 文档：任务概念](https://docs.crewai.com/en/concepts/tasks)
- [CrewAI 文档：任务输出与结构化输出](https://docs.crewai.com/en/concepts/tasks#task-output)

## 与主核 / AI 的关系

`_execute_core` 的形态与主核里「一次请求处理」的骨架是同构的：入口极薄、内核承担全部生命周期、结尾统一收口（这里收的是 `TaskOutput`，主核收的是响应或错误码）、清理放 `finally`。差别在于主核的「处理」是确定性的数据变换，这里的「处理」是一次不可预测的模型会话，因此所有能确定化的部分（输出格式判定、护栏、落盘、回调）都被显式摆在内核里，而不可预测的部分被压在 `agent.execute_task` 这一个调用后面。

对 AI 工程的实际意义在于：把结构化输出与校验的时序排对，是这类框架最容易出错的地方。CrewAI 的解法是「先跑完模型 → 造原始输出 → 护栏改写 → 再无护栏才导出」，即**校验在导出之前**。这个顺序保证了护栏看到的永远是模型的原始产物而不是被 schema 转换过的产物，也保证了最终落到 `pydantic` 字段上的东西是护栏认可的那一份。
