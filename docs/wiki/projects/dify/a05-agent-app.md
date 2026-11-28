---
project: dify
feature: agent-app
title: Agent 应用形态
page_type: feature
schema_version: 1
refs:
  - path: api/core/app/apps/agent_app/app_runner.py
    symbols: [AgentAppRunner, _TextDeltaDebouncer]
  - path: api/core/app/apps/agent_app/app_generator.py
    symbols: [AgentAppGenerator]
---

# Agent 应用形态

## 一句话职责

把一次 Agent 应用对话交给**外部 Agent 后端**跑：`AgentAppGenerator` 负责解析 Agent 配置版本、建会话与消息记录、起工作线程；`AgentAppRunner` 负责调用后端、消费事件流、把过程事件落成 `MessageAgentThought` 行、并把最终答案折回既有聊天管道。

## 机制是什么

Agent 应用最特殊的一点：**执行不在本进程里。** 之前各形态的 runner 都是「在本进程调一次模型」或「在本进程跑一张图」，而这个 runner 的对手是 `dify-agent` 后端——它用 HTTP 起一次 run，然后一条事件流回来。

这带来三个与前几种形态不同的机制需求：

| 需求 | 手段 |
|---|---|
| 跨进程执行 | `AgentBackendRunClient.create_run` + `stream_events` |
| 过程可追溯 | `_AgentProcessRecorder` 把流事件逐条落成 `MessageAgentThought` |
| 与既有管道兼容 | 仍然只发 `QueueLLMChunkEvent` / `QueueMessageEndEvent`，复用聊天持久化与 SSE |

第三点是这份设计的核心取向：**新执行体不新开输出通道。** 后端的答案被重打包成 EasyUI 聊天管道认识的两种事件，于是「答案怎么写库、怎么推 SSE」完全不需要重写。`publish_text_answer` 的 docstring 直接写明：「让后端产出的答案与短路答案（审核/标注回复）共享完全相同的持久化 + SSE 路径」。

**阻塞模式被直接禁掉。** `AgentAppGenerator.generate` 第一句就是 `if not streaming: raise AgentAppGeneratorError("Agent App only supports streaming mode")`。因为这个形态的交互模型本身就是流式的：过程（思考、工具调用）要实时可见。

## 怎么实现的

### AgentAppGenerator：解析版本、建记录、起线程

`generate` 的顺序可以分成五段。

**第一段：解析 Agent 版本。** `_resolve_agent(...)` 返回四元组（`agent`、`agent_config_id`、`agent_config_version_kind`、`agent_soul`）。注释写明了关键规则：「新会话用当前 Agent 版本；已有会话用其 Binding 指向的**不可变版本**」。所以一个持续对话不会因为 Agent 被改版而中途换行为——这与会话记忆的语义一致。

随后 `_session_scope_config_version_id` 单独算一个 session scope 用的版本 ID。**它和 run 用的版本是两回事**：run 用哪版决定行为，session scope 用哪版决定「工作区状态按哪版隔离」。

**第二段：拼一份 EasyUI 形状的配置。** 这一步很有意思：`AgentAppConfigManager.get_app_config(app_model, agent_soul, annotation_reply, app_model_config, conversation)` 把 **Agent Soul** 转成 EasyUI 风格的 `app_config`，注释说明理由是「让聊天管道能持久化 usage」。也就是说，这条管道下游的持久化逻辑依赖 `app_config` 这个形状，所以哪怕真正执行的是外部后端，也要先伪造一份形状正确的配置。

`model_conf = ModelConfigConverter.convert(app_config)` 同理——聊天管道要写 token 数与价格，需要模型配置。

**第三段：文件与追踪。** `_bind_file_access_scope` 包住文件构造，`FileUploadConfigManager.convert(..., is_vision=True)` 与 `file_factory.build_from_mappings(...)` 把原始文件转成 `File`。注意 `is_vision=True` 是硬编码的：Agent 应用默认按视觉模式解析文件上传配置。

`create_trace_scope(...)` 带上 `tenant_id`、`app_id`、`app_tracing`、`user_id`（Account 用 `id`，EndUser 用 `session_id`）与 `extras`。

**第四段：组 `AgentAppGenerateEntity`。** 这里有几个值得注意的字段：

- `task_id=str(uuid.uuid4())` 每次新生成，不复用。
- `parent_message_id` 在 `SERVICE_API` / `OPENAPI` 下被强制设成 `UUID_NIL`——API 调用不支持指定父消息。
- `inputs` 走 `_prepare_user_inputs(...)`（即 a02 那页讲的按变量声明重建 + 类型转换）。
- `call_depth=0` 与 `agent_llm_gateway_enabled=True` 都是固定值。

**第五段：建记录、起线程、处理响应。** `_init_generate_records` 建 `Conversation` 与 `Message`，`MessageBasedAppQueueManager` 带上 `conversation_id` 与 `message_id`，然后 `contextvars.copy_context()` + `threading.Thread` 起工作线程，主线程走 `_handle_response`。

这套「一请求两线程」与工作流形态同构，但这里多一步 `flask_app=current_app._get_current_object()`——把 Flask 应用对象本身传进线程，因为线程里要用它的 context。

### AgentAppRunner：一次 run 的完整生命周期

`run(...)` 的参数有十七个。它的主干只有六步：

1. `_build_session_scope` 算会话作用域。
2. `_session_store.load_or_create(scope)` 取或建绑定（`StoredAgentAppSession`，含 `binding_id` 与 `backend_binding_ref`）。
3. `_build_runtime` 由 `AgentAppRuntimeRequestBuilder` 拼出请求体（带 `session_snapshot`、`idempotency_key=message_id`）。
4. `create_run(runtime.request)` 拿 `run_id`。
5. `_consume_stream(...)` 消费事件流，得到 `terminal` 与 `process_recorder`。
6. 按 `terminal` 类型分派收尾。

第 3 步的 `idempotency_key=message_id` 是一个重要保证：**同一个消息重复提交不会起两次 run**。用业务键（消息 ID）而不是新生成的 UUID 作幂等键，意味着重试是安全的。

第 6 步的分派逻辑值得细看：

- 三种终止事件（成功/失败/取消）都会 `_persist_message_usage(...)` 持久化用量。**取消也记用量**，因为 token 已经花了。
- 失败或取消时，若 `terminal.session_snapshot is not None` 就 `_save_session(...)`。注释：「`None` 表示没有产出退出后的快照，此时保留之前存储的快照不动」。
- 非成功时抛异常：`binding_lost`（`error_type is None` 且 `reason == "binding_lost"`）抛一句干净的 `AgentBackendError("The retained agent working environment is no longer available.")`；其他交给 `_agent_backend_failure_to_exception` 映射成具体的 `InvokeError` 子类。
- 成功时：`_terminal_output_to_answer` 归一答案 → `trim_answer_suffix` 修掉重复后缀 → `_publish_terminal_answer` → `_save_session` → 追踪钩子。

`_terminal_output_to_answer` 处理三种形态：`None` → 空串；`str` → 原样；`dict` → 若有 `text` 字段就用它，否则 `json.dumps`。所以**配置了结构化输出的 Agent 应用，聊天消息体是那段 JSON 文本**。

`trim_answer_suffix` 解决一个具体问题：流式过程中逐块累积进 `MessageAgentThought` 的答案，可能与后端最终给出的完整 answer 有重叠。`_suffix_prefix_overlap_length` 从最长开始向下找「已存答案的后缀 == 最终答案的前缀」的最长长度，把这段从已存答案里切掉，避免重复。若切完这行变成全空，就删掉整行。

### _consume_stream：四层异常与状态的交叉

这是全文件最复杂的函数，因为它要同时处理「事件流」「停止信号」「持久化失败」「取消竞态」四件事。

事件循环的骨架是：`stream_events(run_id, should_stop=queue_manager.is_stopped)` 逐条拿**外部事件**，记 `last_event_id`（用作取消时的 `after` 游标），用 `event_adapter.adapt` 转成**内部事件**列表；先扫一遍看有没有终止事件（有就记下来并 `break`），再扫一遍按类型处理。

**两遍扫描的顺序有讲究**：先找终止帧，再处理其他帧。这样终止帧之后的内容不会被处理——避免在一次已经结束的 run 上继续写行。

处理分支只有三类：`AgentBackendAgentMessageDeltaInternalEvent` 走防抖后 `persist_answer_text`；`AgentBackendStreamInternalEvent` 先 `flush_pending_agent_message_text()` 再交给 `process_recorder.handle_stream_event(...)`；其余跳过。**每个内部事件前都再查一次 `is_stopped()`**，这是让停止能在事件批内生效。

异常处理有四层：

1. `GenerateTaskStoppedError`：先 `flush` 剩余文本，再 `cancel_current_run()`，然后 re-raise。`flush` 外面套 `suppress(GenerateTaskStoppedError)`——**停止期间 flush 也可能再抛停止错误，那不该掩盖原始错误**。
2. 其他异常：同样 flush + cancel，然后**若此刻已停止就转成 `GenerateTaskStoppedError`，否则原样抛**。这一层把「业务失败」与「因停止导致的失败」区分开。
3. `terminal is not None` 时调 `process_recorder.finish(terminal)`——把没收尾的工具调用按终止事件定性。
4. 循环后再 `flush`，再查一次 `is_stopped()`：**如果终止帧已经到达但用户随后点了停止，仍然走 `_finish_stopped_terminal` 保留快照与用量，然后抛停止**。

第 4 层最能说明这份代码的取向：**停止不丢弃已经获得的结果**。终止帧里的 usage 与 session 快照都是真实发生过的，即使客户端不再要答案，也要落库。

`cancel_current_run` 用 `cancellation_attempted` 保证只取消一次——注释写着「即使停止与文本 flush 竞争，也只收尾一次」。`_cancel_run` 通过 `cancel_run_and_wait(run_id, after=last_event_id)` 通知后端，并把返回的终止事件也过一遍 recorder 与 `_finish_stopped_terminal`。

`_persist_message_usage` 里有一处并发考量：用 `db.session.get(Message, message_id, populate_existing=True, with_for_update=True)` 加行锁读最新行，注释解释「已停止的消费者可能通过另一个 session 提交了停止意图或部分答案，要在同一把短锁下与最新行合并，而不是用身份缓存快照覆盖」。**这是「两个线程都可能写同一行」时的正确处理：加锁重读合并，而不是覆盖。**

### 兼容层：三个 publish 函数

`publish_text_delta`、`publish_agent_message_delta`、`publish_message_end` 三个函数把后端文本重打包成队列事件。它们都把整段文本放进**单个 chunk**（`index=0`），而不是切成小块——因为没有真实的 token 边界可用。

`publish_text_answer` = `publish_text_delta` + `publish_message_end`。注释说明这个组合是为了让短路答案与后端答案走同一条路。

注意 `publish_text_delta` 与 `publish_agent_message_delta` 的差别只有一个事件类型（`QueueLLMChunkEvent` vs `QueueAgentMessageEvent`）——**前者进最终答案，后者进过程消息**。所以 `persist_answer_text` 里同时做了两件事：`process_recorder.append_answer_text`（落库到 thought 行）+ `publish_agent_message_delta`（推过程流）。而最终答案由 `_publish_terminal_answer` 单独推一次。

### _TextDeltaDebouncer：按时间窗合并增量

三字段实现：`_debounce_seconds`、`_parts`、`_first_pending_at`。

`push(delta)`：空串返回 `None`；`debounce_seconds <= 0` 时**立即返回原文**（关闭防抖）；否则若 `_parts` 为空记下 `_first_pending_at`，追加，然后**判断「距第一次待推送过去了多久」而不是「距上一次追加过去了多久」**——达到阈值就 `flush()` 并返回合并文本，否则返回 `None`（什么也不发）。

这个「以首次待推送为起点」的选择决定了行为：它是**固定窗口**而不是「静默期」语义。一串持续不断的 delta 会以固定间隔被切分输出，而不是一直等到安静下来。对 SSE 而言这是正确的——用户要看到持续输出，而不是等到段落结束才一次性出现。

`flush()` 合并所有 part 并清空两个状态。**没有 `flush` 的 `push` 返回值就是丢**，所以所有退出路径都必须记得 flush，这正是 `_consume_stream` 里 `flush_pending_agent_message_text()` 反复出现的原因。

### _AgentProcessRecorder：把流事件落成 thought 行

这是全文件最大的一块，也是唯一与「过程可见」直接相关的部分。

它维护六组索引：`_thinking_by_index`（流内 index → 思考行）、`_answer_thought_id`（答案行）、`_tool_by_index`（index → 工具行）、`_tool_by_call_id`（run:id → 工具行）、`_open_tool_by_name`（工具名 → 未完成的工具行集合）、`_activities`（行 ID → 结构化活动元数据），外加 `_finished_tools` 与 `_knowledge_receipts`。

**`_restore_tool_identities` 在构造期就把历史行读回来。** 它按 `position, id` 排序，从已有行恢复 `_next_position`、活动元数据、id 映射与未完成集合。这样**同一消息的续跑（重放）能复用已有行而不是新建**。

**`_bind_run` 保证一个 recorder 只服务一个 run**：`_run_id` 已设且不同就抛 `ValueError`。而 `_call_key` 把 `tool_call_id` 加上 `run_id` 前缀，注释说明「原生 id 按 run 隔离；直接调用旧辅助函数时保持旧键」。

**工具活动的元数据是「白名单构造」而不是「模型声称」**：`_activity` 的 docstring 写着「Build allowlisted metadata from a native call, never from model claims」。它只从 `part["metadata"]` 里取经过 `validated_call_limit` 校验的配额信息，并据 `execution_status == "not_executed"` 或 `part_kind == "retry-prompt"` / `outcome == "failure"` / `runtime_tool_failed(...)` 决定状态与错误文案。

**「关闭思考段」这个动作贯穿所有工具与答案路径。** `_close_thinking_segments()` 清空 `_thinking_by_index` 与 `_answer_thought_id`，于是**工具事件之后的新模型文本会开一行新行**，而不是继续往旧行追加。类 docstring 说明了理由：「Tool events close currently open text segments so later model text starts a fresh row instead of replaying content that was already streamed before the tool.」

**终态不回退**：`_finished_tools` 一旦加入某行，后续同标识的 call/return 事件会被直接忽略（`if thought_id in self._finished_tools: return`），只在 `_tool_by_index` 里保住位置。这就是 docstring 里「terminal states never regress」的实现。

**先提交再通知**：`_create_thought` 里 `db.session.commit()` 之后才 `_publish_thought(thought_id)`，而 `_publish_thought` 的 docstring 明写「Notify only after commit; a concurrent Stop cannot discard saved metadata」。若队列已停止就不发通知，且发布本身也 `suppress(GenerateTaskStoppedError)`。

**`_update_thought` 会判断「有没有实际变化」**：逐字段比较（`row.tool != tool`、`row.tool_input != tool_input` 等），`changed` 为假就直接返回，不提交也不通知。注释：「identical replay emits no additional update」。这让重放幂等。

**工具标签在构造期被复制并冻结**：`_tool_labels = {name: label.copy() for ...}`，`_labels_for_tool` 只对新名查表，找不到就退化成语义为「技术名即显示名」的 `{"en_US": tool, "zh_Hans": tool}`。docstring 说明「重构 recorder 不会给已有调用重新打标签」。

**知识引用有 200 条上限**：`_record_tool_return_part` 里 `len(self._knowledge_receipts) < 200` 才继续收，并立即 `publish(QueueRetrieverResourcesEvent(...))`。

`finish(terminal)` 处理「run 结束了但某些工具没有结果」：遍历所有 `started` / `running` 的活动，按终止事件类型定性——取消 → `cancelled`、失败 → 用 `terminal.error`、其余（成功）→ `failed` 配 `"Tool call did not return a result."`。并用 `unconfirmed_reservation(...)` 把配额状态标记为「未确认」而不是「未执行」或「成功」。docstring 强调：「A retained reservation with no tool result becomes unconfirmed, never successful or definitively unexecuted.」

## 关键取舍

**源码事实：** Agent 应用只支持流式，非流式请求直接抛 `AgentAppGeneratorError`。

**源码事实：** 已有会话固定使用其 Binding 指向的不可变 Agent 版本；新会话用当前版本。

**源码事实：** 为了复用聊天管道的持久化，需要先把 Agent Soul 转成 EasyUI 形状的 `app_config` 与 `model_conf`。

**源码事实：** `idempotency_key=message_id`，用业务键而非随机 UUID 做幂等。

**源码事实：** 三种终止事件都持久化用量；失败/取消时仅在 `session_snapshot` 非 `None` 时保存会话。

**源码事实：** 答案流式累积进 thought 行，再用「后缀 == 最终答案前缀」的最长重叠把重复段切掉。

**源码事实：** `_TextDeltaDebouncer` 按「距首次待推送」计窗口，首次为起点而非最后一次。

**源码事实：** recorder 的工具元数据只从原生调用的白名单字段构造，不采信模型声称；终态不因重放而回退；提交后才发队列通知。

**源码事实：** 未返回结果的工具在成功终止时记为 `failed`、在取消时记为 `cancelled`，配额标为「未确认」。

**架构推论：** 「新执行体不新开输出通道」是这次接入最关键的决策。它让 Agent 后端的答案与既有的审核回复、标注回复走完全相同的下游路径，也就自动获得了「消息落库、token 计费、SSE 帧格式」的全部既有行为。代价是要伪造 `app_config` —— **兼容既有管道的费用通常是一次形状转换。**

**架构推论：** `_TextDeltaDebouncer` 的固定窗口语义对 SSE 是对的，但对「写入 thought 行」也需要同样的切分粒度——两者共用一个 debouncer，说明这份代码把「推给客户端」与「落库」当成同一节奏的事件。若将来需要更细的落库粒度，这个共享会成为障碍。

**架构推论：** 「先提交再通知」的次序是在对抗一个具体的竞态：用户点停止会让队列管理器拒绝后续发布，如果先通知再提交，可能出现「通知成功但数据没落」。反过来的次序最坏情况是「数据落了但通知没发」，客户端看不到但数据在。**在可观测性与数据完整性冲突时，这里选数据完整性。**

**架构推论：** 幂等键用 `message_id` 意味着「同一消息只能跑一次 run」。这对重试友好，但也意味着**如果想对同一条消息重跑（比如换模型重试），必须先建新消息**。约束被推到业务侧。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/app/apps/agent_app/app_generator.py` | `AgentAppGenerator` | 解析 Agent 版本、建记录、起工作线程 |
| `api/core/app/apps/agent_app/app_generator.py` | `AgentAppGenerator.generate` | 只允许流式，拼 EasyUI 形状配置 |
| `api/core/app/apps/agent_app/app_runner.py` | `AgentAppRunner` | 调后端、消费事件流、收尾 |
| `api/core/app/apps/agent_app/app_runner.py` | `AgentAppRunner.run` | 六步生命周期与终止事件分派 |
| `api/core/app/apps/agent_app/app_runner.py` | `AgentAppRunner._consume_stream` | 四层异常与停止竞态的交叉处理 |
| `api/core/app/apps/agent_app/app_runner.py` | `_TextDeltaDebouncer` | 以首次待推送为起点的固定窗口合并 |
| `api/core/app/apps/agent_app/app_runner.py` | `_AgentProcessRecorder` | 把流事件落成 thought 行 |
| `api/core/app/apps/agent_app/app_runner.py` | `_AgentProcessRecorder._activity` | 从原生调用白名单构造工具活动元数据 |
| `api/core/app/apps/agent_app/app_runner.py` | `_AgentProcessRecorder.finish` | 未返回结果的工具按终止事件定性 |
| `api/core/app/apps/agent_app/app_runner.py` | `_AgentProcessRecorder._close_thinking_segments` | 工具事件后开新文本行 |
| `api/core/app/apps/agent_app/app_runner.py` | `trim_answer_suffix` | 用最长重叠切掉重复答案段 |
| `api/core/app/apps/agent_app/app_runner.py` | `publish_text_answer` | 后端答案重打包成聊天管道事件 |
| `api/core/app/apps/agent_app/app_runner.py` | `_persist_message_usage` | 加行锁重读后合并用量 |
| `api/core/app/apps/agent_app/app_runner.py` | `_terminal_output_to_answer` | 字符串/字典/空 三种输出的归一 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/app/apps/agent_app/app_runner.py](https://github.com/langgenius/dify/blob/main/api/core/app/apps/agent_app/app_runner.py)
- [dify 源码：core/app/apps/agent_app/app_generator.py](https://github.com/langgenius/dify/blob/main/api/core/app/apps/agent_app/app_generator.py)
- [dify 文档：Agent 应用](https://docs.dify.ai/guides/application-orchestrate/agent)

## 与主核 / AI 的关系

「外部执行 + 本地落账」与主核里「WAL 由后台进程写、前台只读可见性」是同一类分工：真正的重活交给别的执行体，本地只保留可查询的记录。差别是主核的记录是**事实来源**（能重放），这里的 `MessageAgentThought` 是**过程记录**（不能重放——重放需要重新调后端）。所以这类记录只能用于展示与审计，不能用于恢复。

「先提交再通知」与主核的「先写 WAL 再回 ACK」是同一个次序原则：**任何对外承诺都必须建立在持久化之后**。反过来的次序在主核里对应「先回 ACK 再落盘」，那是数据丢失的经典成因。

幂等键用业务键（`message_id`）与主核里「用事务 ID 而不是时间戳做去重键」同理：业务键天然唯一且可重放，随机键则要求调用方保存映射。**用业务键做幂等的代价是「同一业务对象不能并发跑两次」，这恰好是这里想要的约束。**

对 agent / RAG 工程的实际含义有四条。其一，Agent 应用不能阻塞调用，客户端必须支持 SSE。其二，改了 Agent 配置后，**老会话仍按旧版本行为**，想验证新版必须开新会话。其三，过程消息（thought 行）在工具调用后会开新行，前端按行渲染即可得到自然的分段；但工具行的「失败/取消/未确认」三种状态需要分别显示，不能一律标成错误。其四，用量在取消时也会落库，所以账单里会出现「用户说没看到答案但扣了 token」的情况，这不是 bug。
