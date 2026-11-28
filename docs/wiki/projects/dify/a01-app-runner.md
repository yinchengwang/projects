---
project: dify
feature: app-runner
title: 应用执行器骨架
page_type: feature
schema_version: 1
refs:
  - path: api/core/app/apps/base_app_runner.py
    symbols: [AppRunner]
  - path: api/core/app/apps/execution_coordinator.py
    symbols: [AppExecutionCoordinator, AppExecutionState]
  - path: api/core/app/apps/base_app_queue_manager.py
    symbols: [AppQueueManager]
---

# 应用执行器骨架

## 一句话职责

把「一次应用调用」拆成三段可复用的公共部件：`AppRunner` 负责准备阶段（提示词装配、输入审核、标注回复、多模态输出落盘），`AppQueueManager` 负责「执行侧往队列里发事件、响应侧从队列里取事件」的解耦通道，`AppExecutionCoordinator` 负责这一次尝试的超时与中止策略。

## 机制是什么

一次应用调用在 dify 里是**跨进程**的：API 进程接到请求，起一个执行（可能在 Celery 里跑），执行过程不断产生事件，HTTP 响应侧把事件转成 SSE 推给客户端。这三段因此被切成三个角色：

| 部件 | 在哪一侧 | 管什么 |
|---|---|---|
| `AppRunner` | 执行侧 | 准备输入与提示词、产出事件 |
| `AppQueueManager` | 桥 | 内存队列 + Redis 取消信号 + 心跳 |
| `AppExecutionCoordinator` | 执行侧 | 一次尝试的超时与中止状态机 |

**七个应用形态共用这三个部件，但不共用执行流程。** 这是本页最重要的一点：`AppRunner` 里没有 `run()` 方法，它只有一组**能力**（`organize_prompt_messages`、`recalc_llm_max_tokens`、`direct_output`、`moderation_for_inputs`、`query_app_annotations_to_reply`、`fill_in_inputs_from_external_data_tools`），各形态的 runner 继承它并自己实现执行循环。抽象层只沉淀「怎么把提示词拼好」「怎么把模型输出变成事件」这类与形态无关的事。

**取消是双通道的，且被刻意保留了两条。** 一条是新式的图引擎命令通道（Redis 列表 + `AbortCommand`），一条是旧的停止标志（一个 Redis 键）。两个都发，是因为执行体可能跑在新引擎也可能跑在旧路径上；`send_abort_command` 的异常处理注释写明「Redis 不可用时旧停止标志仍作兜底」。

## 怎么实现的

### AppRunner：准备阶段的能力集合

`recalc_llm_max_tokens` 解决一个具体问题：用户设的 `max_tokens` 加上提示词本身的 token 数可能超出模型上下文窗口。做法是：

1. 从 `model_schema.parameter_rules` 里找 `name == "max_tokens"` 或 `use_template == "max_tokens"` 的规则，取出用户设的值。
2. `model_context_tokens is None`（模型没声明上下文长度）时返回 `-1`，**不调整**。
3. 否则算提示词 token 数，若两者之和超限，就把 `max_tokens` 改成 `max(剩余空间, 16)` 并**直接写回 `model_config.parameters`**。

第 3 步的「直接写回」是一个副作用式的设计：函数签名返回 `None`（或 `-1`），真正的输出是对入参对象的原地修改。下限 16 是防负数/零的兜底——剩余空间可能因为提示词本身超长而为负。

`organize_prompt_messages` 按 `prompt_type` 分流到两套转换器：

- `SIMPLE` → `SimplePromptTransform().get_prompt(...)`，把模式（`AppMode`）、输入、查询、文件、上下文、记忆、模型配置一起传进去。
- 否则（高级）→ `AdvancedPromptTransform`。这里**再按模型类型分流一次**：`ModelMode.COMPLETION` 要求 `advanced_completion_prompt_template` 存在，否则抛 `InvokeBadRequestError("Advanced completion prompt template is required.")`，然后取它的 `prompt` 组成补全模板，若有 `role_prefix` 则一并拷进一个**关闭了窗口记忆**的 `MemoryConfig`；`ModelMode.CHAT` 要求 `advanced_chat_prompt_template` 存在，否则同样抛 `InvokeBadRequestError`，把每条消息转成 `ChatModelMessage`。`stop` 序列取自 `model_config.stop`。

注意高级分支显式构造 `MemoryConfig(window=WindowConfig(enabled=False))`——**高级提示词模板自己管上下文，不吃记忆窗口**。这是两条路的语义差异，不是遗漏。

`direct_output` 是「不走模型、直接把固定文本吐出去」的出口，用于标注回复这类场景。`stream=True` 时它**逐字符**发 `QueueLLMChunkEvent` 并 `time.sleep(0.01)`：这是刻意伪造打字机效果。最后总发一个 `QueueMessageEndEvent` 收尾，`usage` 缺省用 `LLMUsage.empty_usage()`。

`_handle_invoke_result` 是一个 `match` 分发：非流式的 `LLMResult` 直接包成结束事件；流式的 `Generator` 走 `_handle_invoke_result_stream`；其他类型抛 `NotImplementedError`。

流式处理有四处细节值得看：

1. **文本累积与多模态分流并行。** 每个 chunk 的 `delta.message.content` 可能是 `str` 也可能是 `list`；列表里 `TextPromptMessageContent` 拼接文本，`ImagePromptMessageContent` 走落盘分支。
2. **多模态落盘需要三个 ID 同时齐备**（`message_id` / `user_id` / `tenant_id`），缺任一就只打 warning 而**不落盘**——图片会出现在流里但不会进 `MessageFile`。
3. **落盘用独立 session。** `_handle_multimodal_image_content` 里注释写明「用独立 session 以免提交或关闭调用方请求级 session」，且 `session_factory.create_session()` 后显式 `commit()`。
4. **`GenerateTaskStoppedError` 时显式 `invoke_result.close()`。** 注释说明目的是「尽快关掉 provider 流以停止在途 token 生成」——不 close 的话底层 HTTP 流会继续被读。

`model` 与 `prompt_messages` 都取**第一次出现的值**（`if not model: ...`），`usage` 取**最后一次非空值**。这个不对称是有意的：模型名与提示词在整条流里不变，取首即权威；usage 只在最后一个 chunk 上给全量。

`_check_for_sqlalchemy_models` 在 `publish` 里对事件做深度遍历，发现 SQLAlchemy 模型实例就抛 `TypeError`，消息写着「会导致线程安全问题」。这是一道**跨线程边界的防护**：事件会被另一个线程消费，若带着 ORM 实例过去，它的 session 绑定关系会失效。递归只处理 `dict` 与 `list`，其他叶子节点检查 `_sa_instance_state`。

### AppQueueManager：队列、心跳与取消

构造期做三件事：校验 `user_id` 非空（否则 `ValueError("user is required")`）、把「这个 task 属于谁」写进 Redis（键 `generate_task_belong:<task_id>`，TTL 1800，值为 `account-<uid>` 或 `end-user-<uid>`）、建一个无界 `queue.Queue` 与一个 `AppExecutionCoordinator`。

**归属前缀由 `invoke_from` 决定**：`runs_as_account()` 为真时是 `account`，否则 `end-user`。这个前缀是「谁能停这个任务」的判据。

`listen` 是一个生成器，四个职责挤在一个循环里：

- `self._q.get(timeout=1)` 取事件，取到 `None` 就退出；超时则 `continue`。
- **`finally` 块里每轮都做两件事**：查停止标志（命中且有 coordinator 接受就发 `QueueStopEvent`），以及按「每 10 秒一次」的节奏发 `QueuePingEvent` 保活。
- 停止事件注释写着「发两条以确保客户端能收到停止信号并在处理完后停止监听」。
- 退出时 `finally` 里调 `listener_closed(segment_completed=...)` 并把 `_graph_runtime_state` 置空——**释放对图运行时状态的引用**，让执行结束后能被 GC。

`_is_stopped` 用 `cachedmethod` + `TTLCache(maxsize=1, ttl=1)` 缓存 1 秒。这个缓存不是性能优化，而是**把每轮都查 Redis 变成每秒查一次**。它的异常策略很讲究：`BrokenPipeError` 或 `RedisError` 里能追到 broken pipe 的返回 `False`（注释：「断连无法证明用户请求了停止，所以 fail open 以免打断工作流生成器」），**其他 Redis 错误原样抛出**。

`_is_broken_pipe_error` 沿 `__cause__` / `__context__` 链向上找，因为异常常被包了两三层。

`set_stop_flag` 是**带归属校验的停止**：先读归属键，不存在返回 `False`；值不等于当前用户的 `account-/end-user-` 前缀组合返回 `False`；全通过才写停止键（TTL 600）。所以「只有发起者能停自己的任务」。

`set_stop_flag_no_user_check` 则跳过校验，直接调协调器的 `set_app_task_stop_flag`——用于没有用户上下文的内部停止。

`stop_listen` 只接受 `PAUSED` 与 `TERMINAL` 两个状态（其他抛 `ValueError`），然后**先推进 coordinator 状态、再置事件、清归属缓存、往队列塞 `None`**。塞 `None` 是让 `listen` 的生成器退出。这个顺序保证了「状态已经确定，监听才宣告结束」。

### AppExecutionCoordinator：一次尝试的状态机

四个状态：`RUNNING` / `PAUSED` / `ABORTING` / `TERMINAL`。转移规则写在各方法里：

- `start_watchdog`：只在 `RUNNING` 且未启动过时生效。超时 `<= 0` 时**立刻同步调 `_handle_timeout`**，否则起一个 daemon `threading.Timer`。
- `mark_paused`：只在 `RUNNING` 时生效。
- `mark_terminal`：无条件生效。
- `request_abort`：只在 `RUNNING` 且尚未发过时生效（`_abort_sent` 守卫），成功后转 `ABORTING`。
- `_handle_timeout`：调 `request_abort`，**只有它返回真才回调 `on_timeout`**——避免重复上报。

**每个状态转移都在锁内完成，但定时器的 `cancel()` 都在锁外。** 这是必要的：`cancel()` 可能触发回调链，持锁调用会与回调里想拿同一把锁的路径互等。实现手法统一成「锁内摘下 `_watchdog` 并置 `None`，锁外 cancel」。

`attempt_id` 每次构造生成一个 UUID。docstring 明确说：「恢复的工作流会复用稳定的 task ID，但会新建一个 coordinator」——**`attempt_id` 才是「这一次尝试」的身份，`task_id` 是跨尝试的业务键**。这与中断恢复场景的需求直接相关。

**`listener_closed` 只记日志，不做任何取消动作。** docstring 写得很清楚：「响应侧的脱离不是执行取消信号：流式工作流执行可能仍在另一个进程继续，并为后来的订阅者发布持久事件。」——这是把「客户端断开」与「任务取消」彻底分开的设计。

`_abort_execution` 双发：先 `set_app_task_stop_flag`（失败只记日志），再 `send_abort_command`（内部自己 try）。**旧通道失败不阻止新通道**。

`clear_app_task_cancellation_signals` 是一个容易被忽略但很关键的辅助函数。docstring 解释了原因：两个取消通道都按 task ID 建键且**活得比发起它们的尝试更久**（停止标志 600 秒、排在队列里的 `AbortCommand` 一小时），而没有引擎在跑时没人消费它们。恢复的工作流**故意复用暂停那次运行的 task ID**，所以不清就会「一启动就自我中止」。函数末尾的 admonition 是：「只在新尝试准备要跑时调用，绝不在执行中途调用」。

## 关键取舍

**源码事实：** `AppRunner` 不含 `run()`，只有准备与事件产出的能力方法；各形态自己实现执行循环。

**源码事实：** `recalc_llm_max_tokens` 以原地修改 `model_config.parameters` 的方式生效，返回 `-1` 表示模型未声明上下文长度因而不调整，下限 16。

**源码事实：** 高级提示词分支显式关掉记忆窗口（`WindowConfig(enabled=False)`），而简单分支会把 `memory` 传下去。

**源码事实：** `direct_output` 逐字符发 chunk 并 `sleep(0.01)` 伪造打字机效果。

**源码事实：** 流式处理里 `model` 与 `prompt_messages` 取首次值、`usage` 取最后一次非空值；`GenerateTaskStoppedError` 时显式 `close()` 底层流。

**源码事实：** 多模态图片落盘需要三个 ID 齐备，且用独立 session 提交。

**源码事实：** `publish` 会深度检查事件里的 SQLAlchemy 模型实例并抛 `TypeError`。

**源码事实：** `_is_stopped` 有 1 秒 TTL 缓存；broken pipe 类错误 fail open 返回 `False`，其他 Redis 错误向上抛。

**源码事实：** 停止有归属校验（`set_stop_flag`）与无校验（`set_stop_flag_no_user_check`）两个入口。

**源码事实：** `listener_closed` 不做取消；`clear_app_task_cancellation_signals` 明确要求只在新尝试开始时调用。

**架构推论：** 「抽象只沉淀准备能力、不沉淀执行流程」是这份设计最值得学的判断。七种形态的交互模型本质不同（工作流是无状态图、对话是有状态的、Agent 是循环的），强行统一 `run()` 会得到一个满是 `if mode == ...` 的巨型函数。**抽象的边界应该划在「共享什么」上，而不是「看起来像什么」。**

**架构推论：** 双取消通道的存在意味着取消是「尽力而为」的：两条都发是因为不知道谁在消费。`clear_app_task_cancellation_signals` 的存在则揭示了这种设计的代价——**按长生命周期键发的信号会在下一次尝试里变成幽灵**，必须显式清。用 `attempt_id` 与 `task_id` 分开是治本的方向，但键仍然按 `task_id` 建，所以清理无法省。

**架构推论：** `listen` 用「1 秒超时轮询 + 每轮查停止标志 + 每 10 秒 ping」把三件事混在一个循环里，全部依赖 `finally` 保证每轮都执行。这类写法的好处是只有一个循环、状态面小；代价是**每个职责的节奏被绑死在一起**（想改 ping 频率要动同一个 `finally`）。

**架构推论：** `_is_stopped` 的 fail-open 是一个明确的取舍：断连时「不停止」比「误停止」好。因为误停止会中断一个正在正常产出的回答，而不停止最多让用户多点一次按钮。**在这个场景下，可用的坏结果优于不可用的好结果。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/app/apps/base_app_runner.py` | `AppRunner` | 准备阶段能力集合（无执行循环） |
| `api/core/app/apps/base_app_runner.py` | `AppRunner.recalc_llm_max_tokens` | 按上下文窗口回调 max_tokens，原地改配置 |
| `api/core/app/apps/base_app_runner.py` | `AppRunner.organize_prompt_messages` | 简单/高级两种提示词装配 |
| `api/core/app/apps/base_app_runner.py` | `AppRunner.direct_output` | 不走模型，逐字符伪流式输出固定文本 |
| `api/core/app/apps/base_app_runner.py` | `_handle_invoke_result_stream` | 流式 chunk 转事件 + 多模态落盘 |
| `api/core/app/apps/base_app_runner.py` | `_handle_multimodal_image_content` | 图片落 ToolFile 与 MessageFile，独立 session |
| `api/core/app/apps/base_app_runner.py` | `moderation_for_inputs` | 输入审核，按平台路由决定 OpenAI 检查 |
| `api/core/app/apps/execution_coordinator.py` | `AppExecutionCoordinator` | 单次尝试的超时与中止状态机 |
| `api/core/app/apps/execution_coordinator.py` | `AppExecutionState` | 四态：运行/暂停/中止中/终态 |
| `api/core/app/apps/execution_coordinator.py` | `request_abort` | 幂等中止，只在运行时生效 |
| `api/core/app/apps/execution_coordinator.py` | `clear_app_task_cancellation_signals` | 清理复用 task ID 带来的幽灵取消信号 |
| `api/core/app/apps/execution_coordinator.py` | `send_abort_command` | 图引擎命令通道，异常不回抛 |
| `api/core/app/apps/base_app_queue_manager.py` | `AppQueueManager` | 内存队列 + 归属缓存 + 停止标志检查 |
| `api/core/app/apps/base_app_queue_manager.py` | `AppQueueManager.listen` | 取事件、查停止、发心跳的单一循环 |
| `api/core/app/apps/base_app_queue_manager.py` | `AppQueueManager.set_stop_flag` | 带归属校验的停止 |
| `api/core/app/apps/base_app_queue_manager.py` | `_is_stopped` | 1 秒缓存，broken pipe fail open |
| `api/core/app/apps/base_app_queue_manager.py` | `_check_for_sqlalchemy_models` | 阻止 ORM 实例跨越线程边界 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/app/apps/base_app_runner.py](https://github.com/langgenius/dify/blob/main/api/core/app/apps/base_app_runner.py)
- [dify 源码：core/app/apps/execution_coordinator.py](https://github.com/langgenius/dify/blob/main/api/core/app/apps/execution_coordinator.py)
- [dify 文档：应用类型与 API](https://docs.dify.ai/guides/application-publishing/developing-with-apis)

## 与主核 / AI 的关系

「执行侧发事件、响应侧收事件、中间隔着队列」与主核里「后台写进程刷脏页、前台读请求看可见性」是同一种解耦：生产者与消费者的速率不必匹配。差别是主核的队列是持久化的 WAL，消费者崩溃可以重放；这里的内存 `queue.Queue` 一崩就没了——**所以「事件」在这个系统里不是事实来源，数据库里的 `Message` 才是**。响应侧断线后重新订阅，靠的是重读数据库而不是重放队列。

取消的双通道与主核的「查询取消」也有可比性。数据库的取消是一个信号（PG 的 `pg_cancel_backend`），执行体在检查点主动退出；这里也是同样的形态（停止标志 + 命令通道），但**多了「按 task ID 的键会比尝试活得久」这个麻烦**，于是必须显式清理。数据库没有这个问题，因为它的信号不发到持久键上。

`_check_for_sqlalchemy_models` 这道防护与主核里「不允许把页缓冲区的指针传出引擎」是同一类边界守卫：**跨边界传的不是数据而是引用，就会在另一侧以不可预测的方式失效**。ORM 实例带 session 绑定，页指针带 latch 状态，都不适合跨线程。

对 agent / RAG 工程的实际含义有四条。其一，`max_tokens` 会被静默下调（下限 16），**看到输出比预期短时该先怀疑提示词太长而不是模型不听话**。其二，客户端断开不等于任务取消，长任务会继续跑完并落库，重新连上能看到结果——这是有意的，不要按「断开即中止」设计前端。其三，恢复暂停的工作流会复用 task ID，若自己实现恢复逻辑，必须先清掉遗留的取消信号，否则一启动就自停。其四，只有发起者能停自己的任务（`set_stop_flag` 的归属校验），管理后台要停别人得走 `set_stop_flag_no_user_check`。
