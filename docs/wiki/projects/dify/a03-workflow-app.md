---
project: dify
feature: workflow-app
title: 工作流应用形态
page_type: feature
schema_version: 1
refs:
  - path: api/core/app/apps/workflow/app_runner.py
    symbols: [WorkflowAppRunner]
  - path: api/core/app/apps/workflow/app_generator.py
    symbols: [WorkflowAppGenerator]
  - path: api/core/app/apps/workflow/active_workflow_tasks.py
    symbols: [active_workflow_task]
---

# 工作流应用形态

## 一句话职责

把「用户点一次运行」变成「一个后台线程把图跑完、事件从队列流向响应流」：`WorkflowAppGenerator` 起线程并在主线程消费队列，`WorkflowAppRunner` 在后台线程里装配图执行需要的三样东西（变量池、运行时状态、命令通道），`active_workflow_task` 提供「同一个 task ID 不能被并发跑两次」的进程内互斥。

## 机制是什么

工作流形态的核心矛盾是：**执行要跑很久，而 HTTP 响应要立刻开始流式输出**。dify 的解法是**一个请求两个线程**：

| 线程 | 职责 | 生命周期终点 |
|---|---|---|
| 主线程（响应侧） | 从 `queue.Queue` 取事件、转 SSE 写给客户端 | 客户端断开或事件流结束 |
| 后台线程（执行侧） | 建变量池、装图、跑图、往队列发事件 | 图执行结束 |

两个线程通过 `AppQueueManager` 交接，且后台线程**不持有 Flask 上下文**——所以它必须把请求上下文与 `contextvars` 一起带过去（`preserve_flask_contexts`）。这是 Python 里「起线程跑请求级逻辑」的必备动作，否则线程里访问 `current_app` 会直接炸。

**`active_workflow_task` 是一个进程内注册表，不是分布式锁。** 它保证同一个 `task_id` 在本进程里不会同时被两个上下文持有，重复进入直接抛 `ValueError`。而进程之间靠 Redis 的取消信号协调——两者职责不同，不能互相替代。

## 怎么实现的

### 响应侧：起线程 + 消费队列

`_generate` 的顺序是精心排过的：

1. `_bind_file_access_scope` 绑定文件归属标记，并在**范围内部**先 `grant_file_default_access(variables)` 再 `restore_file_access()`。docstring 特别说明：「恢复时 HITL 提交必须已经提交，要在捕获 worker 与响应上下文之前恢复 EndUser 的文件授权」——**先恢复授权、再拷上下文**，否则后台线程拿到的是没有授权的上下文。
2. 建 `WorkflowAppQueueManager`（带 `app_mode`）。
3. `contextvars.copy_context()` 抓一份上下文快照。
4. **`db.session.close()` 主动释放数据库连接**。注释写明理由：「后续的新线程操作可能耗时很久」——主线程要立刻释放连接，否则一个长跑的工作流会把连接池占住。
5. 起 `threading.Thread`，把 `current_app._get_current_object()`、上下文快照、各 repository 一起作为参数传进去。
6. `_handle_response` 得到阻塞或流式响应，再 `WorkflowAppGenerateResponseConverter.convert(response, invoke_from)` 转成最终形状。
7. **收尾分两条路**：抛异常时先 `_join_worker_thread` 再 re-raise；拿到生成器时用 `_wrap_stream_with_worker_thread_join` 包一层（等流结束才 join）；其他情况直接 join。

第 7 步是这个小节最值得注意的设计：**线程收尾的时机取决于响应的形态**。阻塞响应必须在返回前 join，否则线程还活着而请求已经结束；流式响应则必须等到流被消费完（所以包装在生成器的 `finally` 里）。漏掉任一条，要么会提前返回、要么会泄漏线程。

### 执行侧：三个必需品

`WorkflowAppRunner.prepare` 有**三条互斥分支**，决定图怎么初始化：

**分支一：恢复。** `_resume_graph_runtime_state` 非空时直接复用它的 `variable_pool`，只重新 `_init_graph`。这是「暂停后恢复」的路径——变量池已经在暂停时持久化过，不能再造一遍。

**分支二：单节点调试。** `single_iteration_run` 或 `single_loop_run` 非空时调 `_prepare_single_node_execution`。注意**这一分支会先把 `invoke_from` 改写成 `DEBUGGER`**，即使请求来自网页端——单步调试本质上是调试行为。

**分支三：正常启动。** 这条最长，四步：

1. `build_system_variables` 造系统变量（`files`、`user_id`、`app_id`、时间戳、`workflow_id`、`workflow_execution_id`）。
2. 空 `VariablePool` + `add_variables_to_pool(build_bootstrap_variables(system_variables, environment_variables))`——把系统变量与环境变量先铺进池子。
3. `root_node_id = self._root_node_id or get_default_root_node_id(graph_dict)`，然后 `add_node_inputs_to_pool` 把**用户填的表单值**写到入口节点下，并带上 `get_compatible_start_aliases(...)` 算出的别名。
4. `RuntimeState(workflow_id, variable_pool, start_at=perf_counter())` 后 `_init_graph`。

第 3 步的别名是个兼容性细节：`get_compatible_start_aliases` 按工作流种类与入口节点 ID 生成别名列表，于是**同一个输入可以按多个名字被引用**。这是为了让改过入口节点 ID 的老工作流仍能读到输入。

**命令通道是三个通道的并集。** 构造 `CombinedCommandChannel` 时按顺序传：Redis 命令通道（`app_task_command_channel_key(task_id)`）、旧停止标志通道、Celery 信号通道。

三个通道的语义各不相同：

- `RedisChannel` 是**外部可写**的通道：任何进程都能往里塞 `AbortCommand`。
- `StopFlagCommandChannel` 把「Redis 里的停止标志」翻译成一次 `AbortCommand`，且用 `_abort_emitted` 保证**只发一次**。
- `CelerySignalCommandChannel` 把「进程内 Celery 温关机状态」翻译成一次中止，同样只发一次。

`CombinedCommandChannel.fetch_commands` **逐个通道 try**，某个通道抛异常只记日志继续——**一个通道坏掉不能阻断其他通道的取消能力**。而 `send_command` 只走第一个通道（Redis），注释写明「第一个是 runner 的主命令汇」。

顺序因此有含义：Redis 在第一位既是被 fetch 的第一个，也是 send 的唯一目标。

`attach_stop_aware_ready_queue(graph_runtime_state, task_id)` 把「停止感知的就绪队列」挂到运行时状态上——图在取下一个待执行节点时可以感知到停止，而不是等到当前节点跑完。

最后装配 `WorkflowEntry`（图 + 配置 + 调用深度 + 运行时状态 + 命令通道 + 响应流过滤器），包成 `PreparedWorkflowRun` 返回。**runner 不做执行**，它只 prepare——执行由 `WorkflowRunDriver` 完成。

### 后台线程的两段式异常处理

`_generate_worker` 显式分成两段，中间隔着一个 `try/except` 分界，位置很关键：

**第一段（准备）：** 开 session 查 `Workflow`，`_ensure_snippet_start_node_in_worker` 补代码片段起点，恢复运行则 `_restore_workflow_run_graph`，然后按 `user_from` 决定 `system_user_id`（`END_USER` 用 `end_user.session_id`，找不到就空串；否则用 `user_id`），最后建 `WorkflowAppRunner`。

这一段的 `except` 会 `publish_error` 然后 `return`。注释写明了理由：「调用方会阻塞在队列上直到这个 worker 发出事件，所以准备期失败必须像执行期失败一样被发布出去」。**这是「两个线程用队列同步」时最容易漏的一处**：如果准备期静默失败，主线程会永远等下去。

**第二段（执行）：** `with active_workflow_task(task_id):` 包住 `self._execution_driver(...)`，然后五个 `except` 分支依次处理：`GenerateTaskStoppedError`（只记 warning，不发布错误——用户主动停的不算错误）、`InvokeAuthorizationError`（重建成一句干净的「API key 不对」）、`ValidationError`、`ValueError`（只在 DEBUG 下打堆栈）、兜底 `Exception`。

`system_user_id` 的选择值得单独看：面向最终用户时用的是 `end_user.session_id` 而不是 `user_id`。**这是把「同一个浏览器会话」当成系统用户身份**，让同一会话内的多次调用共享同一个系统身份。

### 进程内互斥

`active_workflow_task` 是一个 38 行的上下文管理器：全局 `set` 加一把 `RLock`。进入时若 ID 已在集合里就抛 `ValueError(f"Workflow task already active for task_id=...")`，否则加入；退出时 `discard`。

它配了两个辅助：`get_active_workflow_task_count`（给指标用）与 `reset_active_workflow_tasks`（给 worker 初始化与测试用）。

**`discard` 而不是 `remove`**，所以退出路径重复调用不会抛 `KeyError`。而 `task_id` 为空时直接抛 `ValueError`——不允许注册无名任务。

## 关键取舍

**源码事实：** `_generate` 在主线程起后台线程前主动 `db.session.close()`，理由是新线程操作可能耗时很久。

**源码事实：** 线程收尾分三种情况：异常时先 join 再抛、流式时包在生成器 `finally` 里、阻塞时立即 join。

**源码事实：** `prepare` 有三条互斥分支（恢复 / 单节点调试 / 正常启动），且单节点调试会把 `invoke_from` 改写成 `DEBUGGER`。

**源码事实：** 正常启动时先铺系统变量与环境变量，再把用户输入写到入口节点并附上兼容别名。

**源码事实：** 命令通道是三通道并集，fetch 时逐个 try 互不影响，send 只走第一个（Redis）。

**源码事实：** `StopFlagCommandChannel` 与 `CelerySignalCommandChannel` 都用 `_abort_emitted` 保证只发一次。

**源码事实：** `_generate_worker` 的准备期异常必须 `publish_error` 后返回，否则主线程会一直阻塞等事件。

**源码事实：** `active_workflow_task` 是进程内 `set` + `RLock` 的互斥，重复 ID 直接抛错，退出用 `discard`。

**架构推论：** 「一个请求两个线程 + 队列」是这个形态的唯一可行解，因为它要同时满足「立即开始流式」与「执行可能超时」。代价是两个线程的失败必须显式桥接——准备期漏发事件就会挂住主线程。这类结构性风险无法靠类型系统防范，只能靠 `_generate_worker` 里那个位置正确的 `try`。

**架构推论：** 命令通道的「多源并集 + fetch 全部 try」是面向不确定性的设计：不知道取消信号会从哪条路来（外部 API、旧停止标志、进程关机），所以全收。代价是**取消的原因无法从通道本身看出来**——三个通道都会产出 `AbortCommand`，只带各自的 `reason` 字符串。

**架构推论：** `active_workflow_task` 是进程内的，所以它的保护强度等于「单实例」。多副本部署时，同一 `task_id` 在两个进程里都能注册成功。真正的跨进程一致性靠 Redis 命令通道与数据库状态，注册表只是提前在本地拦住明显重复。**不要把它当分布式锁用。**

**架构推论：** 「用户输入写到入口节点下 + 附别名」把「表单字段名」与「图内变量路径」解耦了。这让工作流可以被改结构而输入契约不变，代价是别名列表必须随结构变化同步维护，且别名冲突时行为取决于 `add_node_inputs_to_pool` 的写入顺序。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/app/apps/workflow/app_generator.py` | `WorkflowAppGenerator` | 起后台线程、消费队列、转换响应 |
| `api/core/app/apps/workflow/app_generator.py` | `WorkflowAppGenerator._generate` | 绑文件授权、释放连接、起线程、按形态收尾 |
| `api/core/app/apps/workflow/app_generator.py` | `_generate_worker` | 后台线程：准备期与执行期分段处理异常 |
| `api/core/app/apps/workflow/app_generator.py` | `_handle_response` | 从队列取事件组织响应 |
| `api/core/app/apps/workflow/app_runner.py` | `WorkflowAppRunner` | 装配变量池、运行时状态与命令通道 |
| `api/core/app/apps/workflow/app_runner.py` | `WorkflowAppRunner.prepare` | 三条互斥初始化分支 |
| `api/core/app/apps/workflow/active_workflow_tasks.py` | `active_workflow_task` | 进程内 task ID 互斥 |
| `api/core/app/apps/workflow/active_workflow_tasks.py` | `get_active_workflow_task_count` | 当前活跃任务数 |
| `api/core/app/apps/workflow/command_channels.py` | `CombinedCommandChannel` | 多源 fetch、单源 send |
| `api/core/app/apps/workflow/command_channels.py` | `StopFlagCommandChannel` | 旧停止标志翻译成一次中止 |
| `api/core/app/apps/workflow/command_channels.py` | `CelerySignalCommandChannel` | 温关机信号翻译成一次中止 |
| `api/core/app/apps/workflow/stop_aware_ready_queue.py` | `attach_stop_aware_ready_queue` | 让就绪队列感知停止 |
| `api/core/app/apps/workflow/app_queue_manager.py` | `WorkflowAppQueueManager` | 工作流形态的队列管理 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/app/apps/workflow/app_runner.py](https://github.com/langgenius/dify/blob/main/api/core/app/apps/workflow/app_runner.py)
- [dify 源码：core/app/apps/workflow/app_generator.py](https://github.com/langgenius/dify/blob/main/api/core/app/apps/workflow/app_generator.py)
- [Flask 文档：应用上下文与 contextvars](https://flask.palletsprojects.com/en/stable/appcontext/)

## 与主核 / AI 的关系

「主线程消费队列、后台线程跑执行」与主核里「前台接收请求、后台进程刷脏页」是同一种角色分离，但方向相反：主核的后台是**周期性**的，工作流的后台是**一次性**的。这决定了两者的失败处理不同——主核后台失败下一轮还能补，工作流后台失败必须当场把失败事件发出去，否则前台永久阻塞。

`active_workflow_task` 与主核的「单写者锁」形似而神不同：主核的锁由存储引擎持有并参与崩溃恢复（进程死了锁会被清），这里的注册表随进程消失。**所以它不是互斥保证，只是本地防重入。** 真正的互斥来自数据库状态与 Redis 信号。

「用户输入写到入口节点 + 别名」和主核的「视图把旧列名映射到新列」是同一手法：用一层名字映射吸收结构变更，让旧的引用方式继续有效。代价也一样——**别名表是隐式契约，改结构时容易忘记同步**，而忘了的后果是运行时「变量取不到」而不是编译期报错。

对 agent / RAG 工程的实际含义有四条。其一，长跑工作流会主动释放数据库连接，所以执行期间不要假设请求级 session 可用。其二，准备期失败会把错误发布成事件流里的一帧，排查「请求挂住不返回」时先看后台线程日志而不是等超时。其三，取消有三条来源（外部命令、旧停止标志、进程关机），日志里的 `reason` 字符串是判断来源的唯一线索。其四，单步调试会把调用来源强制改写成调试器，因此它走的是全量响应分支，与网页端正常运行的响应结构不同。
