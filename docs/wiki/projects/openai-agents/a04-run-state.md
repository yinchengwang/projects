---
project: openai-agents
feature: run-state
title: 运行状态与序列化恢复
page_type: feature
schema_version: 1
refs:
  - path: src/agents/run_state.py
    symbols: [RunState, _HeldCurrentResponse, _PendingSessionWrite]
---

# 运行状态与序列化恢复

## 一句话职责

`RunState` 是一次运行的全部可续跑中间态：它是这个 SDK 里唯一横跨「暂停—序列化—跨进程恢复」的载体，也是一份带版本号、前向不兼容、恢复时按白名单脱敏的持久化契约。

## 机制是什么

`RunState` 是约 30 个私有字段的 dataclass，按职责可分成六组：

| 组 | 字段 | 作用 |
|---|---|---|
| 位置 | `_current_turn`、`_current_agent`、`_starting_agent`、`_max_turns` | 现在跑到第几轮、谁在跑、从谁开始 |
| 历史 | `_original_input`、`_model_responses`、`_generated_items`、`_session_items`、`_pending_input` | 原始输入、模型响应、用于构建模型输入的项、用于会话历史的项、下一轮待准入的输入 |
| 上下文 | `_context`（一个 `RunContextWrapper`） | 应用对象 + 用量 + 审批 + 工具调用记录 |
| 守卫 | 四个护栏结果列表、`_current_step`、`_last_processed_response` | 护栏产物与「下一步是什么」 |
| 服务端会话 | `_conversation_id`、`_previous_response_id`、`_auto_previous_response_id`、`_generated_prompt_cache_key` | Responses API 的服务端状态指针 |
| 持久化簿记 | `_current_turn_persisted_item_count`、`_pending_session_write`、`_session_write_in_progress`、`_terminal_unrecoverable`、`_sandbox`、`_trace_state`、`_agent_tool_state_scope_id` | 会话写入进度、待结算写入、终态锁、沙箱与追踪快照 |

三个关键区分决定了它的复杂度：

- **`_generated_items` 与 `_session_items` 是两个列表**。前者是「发给模型的输入来源」（交接的输入过滤器可能改过它），后者是「会话历史的完整记录」。两者在正常轮次相同，在交接过滤或阻塞输出被替换后不同。
- **`_current_step` 是四态枚举**：`NextStepRunAgain`（还要再跑一轮）、`NextStepHandoff`（换智能体）、`NextStepFinalOutput`（结束）、`NextStepInterruption`（等审批），加 `None`（终态）。它既是「下一步做什么」，也是「能不能再接收输入」的判据。
- **`_terminal_unrecoverable` 是一个单标志的窗口锁**。docstring 写得很明确：一旦最终输出、它的护栏、它的终结钩子都完成，这段窗口里运行已经持有一个不会被任何恢复复现的结果，所以状态必须对续跑关闭；只有该轮完全持久化后才会被清除。

## 怎么实现的

**序列化是「显式白名单 + 逐项编码」，不是反射。** `to_json` 先做两个硬检查：没有 `_current_agent` 或没有 `_context` 直接抛 `UserError`。然后按字段手工拼一个字典，键包括 `$schemaVersion`、`current_turn`、`current_agent`（智能体引用，含用于区分的身份键）、`original_input`、`pending_input`、`model_responses`、`context`（内含 `usage`、`approvals`、`tool_invocations`、`context`、`context_meta`，以及条件性的 `tool_input`、`hosted_mcp_approvals`、`function_tool_approvals`）、`tool_use_tracker`、`max_turns`、`no_active_agent_run`、四类护栏结果、三个服务端会话指针、`reasoning_item_id_policy`、嵌套历史引用、`generated_session_item_indexes`、`generated_items`、`session_items`、`current_step`、`last_model_response`、`last_processed_response`、`current_turn_persisted_item_count`，再加条件性的 `pending_session_write`、`terminal_unrecoverable`、`trace`、`sandbox`。

`no_active_agent_run: True` 这个常量字段值得单独注意：它是快照的语义声明——这份状态来自一个**不在运行中**的运行。

**沙箱字段在序列化与反序列化两侧都要过安全检查。** 写出时调用 `sanitize_run_state_sandbox_mount_authority`，若 `_sandbox` 不是 Mapping 就把它清空并抛 `_raise_invalid_run_state_sandbox_envelope`；读入时同样先 sanitize 再交给构建函数。也就是说「沙箱挂载授权」不被信任地原样往返，而是在边界上被清洗。

**上下文的序列化有四条优先级，且会留下「你需要提供反序列化器」的元数据。** `_serialize_context_payload` 的顺序是：Mapping 直接 `dict()`；否则若 `strict_context` 为真且没给 `context_serializer` 就抛错；否则用 `context_serializer`（返回值必须是 Mapping）；否则若是 Pydantic `BaseModel` 就 `model_dump(mode="json")`（注释说明取当前默认值与就地修改，而不只是显式赋过的字段），若是别的 `model_dump` 就用 `exclude_unset=True`；都不行就写出但发警告。无论走哪条，都会生成 `context_meta`（`serialized_via` 与 `requires_deserializer`），让恢复侧能判断「这个 mapping 是不是等价于原始对象」。

**版本策略是「读旧不读新」，且写死在常量关系里。** `CURRENT_SCHEMA_VERSION = "1.22"`；`SCHEMA_VERSION_SUMMARIES` 是一张「版本 → 一句话说明」的表，注释要求每次升版必须加一行；`SUPPORTED_SCHEMA_VERSIONS = frozenset(SCHEMA_VERSION_SUMMARIES)`。模块导入时还有两条断言：当前版本必须在表里，且每个版本的说明不能为空。注释里写清四条策略：已发布版本必须可读；未发布的中间版本可以在发布前重编号或压缩；`to_json` 只写当前版本；**前向兼容是刻意 fail-fast**——旧 SDK 拒绝更新或未知的版本，而不是尽力而为。

除了总版本，还有一组「能力下限版本」常量：程序化工具调用要求 1.13、hosted MCP 审批作用域 1.14、当前响应所有权 1.17、待写压缩元数据 1.18、held 待写 1.19、工具输出护栏边界 1.20。`_build_run_state_from_json` 会把版本解析成 `(major, minor)` 元组逐条比较，低于下限且数据里确实含该能力时抛错。

**反序列化把「不可信输入」当成一等威胁。** `from_string` / `from_json` 的路径是：解析 JSON（失败时把 `state_string` 置为 `"<redacted>"` 再抛）；用 `_validate_run_state_json_value` **逐节点按精确内建类型**校验（注释：不调用调用方定义的协议）；校验版本；sanitize 沙箱；构建状态。构建过程中所有校验错误都通过一个 `validation_error_factory` 登记到 `trusted_validation_errors`，异常离开时用 `_trusted_run_state_validation_message` 判断「这条消息是否在 `_TRUSTED_RUN_STATE_ERROR_MESSAGES` 白名单里且确实是本次登记的」，只有在白名单里才原样保留消息，否则整体走 `_prepare_data_redacted_error`；最后把 `state_json`、`initial_agent`、`context_override` 等局部变量逐个置 `None`，再 `_raise_data_redacted_error`。

这套机制的目标很具体：快照里含工具参数与工具输出，任何「因为 JSON 畸形导致把一段快照内容拼进异常消息」的路径都等于泄漏。所以**错误消息必须白名单**，而不是「小心不要拼进去」。

**同名智能体是不可序列化的。** `to_json` 在 `_starting_agent` 存在且 `_has_ambiguous_hook_identities` 为真时直接拒绝，要求「在开始可持久化运行前使用唯一的智能体名」。恢复侧还有两条：schema < 1.21 且 `_has_legacy_hook_identity_ambiguity` 时拒绝旧快照；当前智能体图有歧义时一律拒绝。docstring 里那句「重命名智能体不能修复已存在的歧义检查点」是关键——身份键在写出时就固定了。

**待写会话是显式状态，不是一个隐式重试。** `_PendingSessionWrite` 是 TypedDict，字段包括 `session_id`、`items`、`before`（写前历史摘要）、`persisted_count`、`held`、`append_acknowledged`、`compaction_model_exchange`、`response_id`、`store`、`has_local_tool_outputs`、`reasoning_item_id_policy`、`current_response`。它的 docstring 说明了 `held` 的含义：这一批输出被「中断停靠」扣住了，因为输出护栏还没批准该轮；被扣住的批次从未提交给 Session，所以 `before` 保持 `None`，直到一个合法出口开始结算它。`_HeldCurrentResponse` 更小，只有 `turn` 与 `start` 两个整数，表示「当前逻辑轮在被扣住批次中的后缀起点」——注释强调过滤或重建当前响应不能改动更早的 held 历史。

**`_session_write_in_progress` 是进程内所有权的守卫。** docstring 一句话：独立序列化的副本需要调用方自己串行化。也就是说这个布尔只防同一份状态被并发使用，不防「同一 Session 被两个恢复快照并发写」——那件事在类文档里被写成约束条款。

**`_copy_for_result_checkpoint` 是「把状态挂到结果对象上」的那一步。** 它 `copy.copy` 后深拷贝待写会话、把 `_session_write_in_progress` 置假、用 `_copy_for_run_state()` 复制上下文、再逐个 `functions` 把嵌套 agent-tool 的 resume state 也复制一份。这解释了为什么同一个运行可以产出多个互不干扰的检查点。

**输入可以延后准入。** `add_input` 把输入放进 `_pending_input`，并在四种情况下拒绝：当前 step 是终态、轮次预算已用尽（`_current_turn >= _max_turns`）、中断中但模型响应已被接受（本地处理还没做完）、以及中断中且没有当前智能体。在中断态下还要看 `tool_use_behavior`：若 `stop_on_first_tool` 或命中了 `stop_at_tool_names`，或行为是可调用对象，就允许追加输入（因为工具结果本身可能成为最终输出）。

**中断项是「拷贝后再交出去」。** `get_interruptions` 只从 `Interruption` 步骤取值，对每个 `ToolApprovalItem` 的 `raw_item` 做一次安全拷贝再 `dataclasses.replace`。拷贝失败时走与反序列化同一套脱敏路径：标记错误已脱敏、把 `self` 也置 `None`、抛脱敏异常。审批项的匹配也做了同样的谨慎：`_approval_items_match` 返回 `bool | None`，`None` 表示「这个候选者无法安全区分」；`_find_current_approval_item` 只要看到任何一个不确定候选或匹配到多个，就抛错要求使用唯一 call id。

## 关键取舍

**源码事实：** 序列化是手写白名单而不是 `dataclasses.asdict`。这让「新增字段必须显式决定是否持久化」成为必然——漏掉就是明显的功能缺失，而不是悄悄泄漏或悄悄丢失。

**源码事实：** 前向兼容是 fail-fast，不是尽力解析。旧 SDK 看到 1.23 会直接拒绝。取舍是明确性换灵活性：宁可拒绝，也不猜一个未知字段的语义。升级路径因此是「升级 SDK」，而不是「旧 SDK 容忍新快照」。

**源码事实：** 版本号是字符串 `"1.22"` 而不是整数，且能力下限用 `(major, minor)` 元组比较。这让「1.9 与 1.10」这类顺序问题必须显式处理（代码里确实解析成整数元组），也让版本号可以承载「语义族」而不是单纯递增。

**源码事实：** 反序列化的错误消息白名单是 `frozenset` 常量，条目里包含所有可能的校验失败文案。任何新增校验路径若忘了把文案加进去，用户看到的就是被脱敏的通用错误而不是具体原因——这是一个故意的摩擦点，逼作者判断「这条消息会不会含快照内容」。

**源码事实：** `_terminal_unrecoverable` 只在「最终输出 + 护栏 + 终结钩子都完成」到「该轮完全持久化」这个窗口内为真。它挡的是「拿着一个已被接受但还没落盘的结果去序列化」——这种快照恢复后既不能重放（结果已接受）也不能重跑（副作用已发生）。

**架构推论：** 把「会话历史」与「模型输入来源」分成两个列表，使交接的输入过滤器与输出护栏的替换都能作用于其中一个而不破坏另一个。代价是每个持久化路径都要想清楚「我该写哪个」，源码里因此出现了大量「哪个列表的哪个后缀已经被写过」的簿记（`_generated_session_item_indexes`、`current_response_generated_item_ownership`、`_generated_items_last_processed_marker`）。

**架构推论：** `RunState` 与 `Session` 是两个独立概念：前者是一次运行的完整中间态（含审批、护栏、待写批次），后者只是对话历史。这个划分使「同一个 Session 上跑多次运行」与「一次运行跨多个 Session 后端」都成为可能，也解释了为什么待写会话需要独立的结算协议——它横跨两个概念。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/run_state.py` | `RunState` | 可续跑状态的全部字段与构造 |
| `src/agents/run_state.py` | `RunState.to_json` | 显式白名单序列化，含智能体引用与上下文元数据 |
| `src/agents/run_state.py` | `RunState.to_string` | `json.dumps(indent=2)` 包装 |
| `src/agents/run_state.py` | `RunState.from_json` | 反序列化主入口：类型校验、版本校验、脱敏重抛 |
| `src/agents/run_state.py` | `RunState.from_string` | 先解析 JSON，失败即把原文置 `<redacted>` |
| `src/agents/run_state.py` | `RunState.add_input` | 延迟准入输入，四种拒绝条件 |
| `src/agents/run_state.py` | `RunState.get_interruptions` | 返回中断项的安全拷贝，失败走脱敏 |
| `src/agents/run_state.py` | `RunState._serialize_context_payload` | 上下文四条序列化优先路径与元数据 |
| `src/agents/run_state.py` | `RunState._copy_for_result_checkpoint` | 挂到结果对象上的独立状态副本 |
| `src/agents/run_state.py` | `_PendingSessionWrite` | 待结算的会话写入批次及其全部归属信息 |
| `src/agents/run_state.py` | `_HeldCurrentResponse` | 当前轮在被扣住批次中的后缀边界 |
| `src/agents/run_state.py` | `_validate_run_state_schema_version` | 版本必须在受支持集合内 |
| `src/agents/run_state.py` | `_build_run_state_from_json` | 能力下限比较与智能体解析 |
| `src/agents/run_state.py` | `_validate_run_state_json_value` | 按精确内建类型校验 JSON 树，不触发调用方协议 |
| `src/agents/run_state.py` | `_trusted_run_state_validation_message` | 只放行白名单内的错误文案 |
| `src/agents/run_state.py` | `SCHEMA_VERSION_SUMMARIES` | 版本→一句话说明，兼作受支持版本集合 |

## 参考

- [openai-agents-python 源码：run_state.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/run_state.py)
- [Agents SDK 文档：人在回路](https://openai.github.io/openai-agents-python/human_in_the_loop/)
- [Agents SDK 文档：结果对象](https://openai.github.io/openai-agents-python/results/)
- [Agents SDK API：run_state 模块](https://openai.github.io/openai-agents-python/ref/run_state/)

## 与主核 / AI 的关系

`RunState` 与主核的「事务快照 + 检查点」几乎逐条对应：都有版本号与兼容策略、都把「写入进度」显式记录、都要在恢复时重建外部资源绑定。差别在信任模型——主核的检查点是自己的代码写自己读，这里的快照可能来自客户端，所以多了一整套「类型精确校验 + 错误消息白名单 + 变量清空」。主核若哪天要接受外部传入的备份文件，需要补的正是这一层。

对 agent 工程有三条可直接借用的结论。第一，**版本不兼容要显式拒绝**：AI 应用的快照里含工具参数与模型输出，静默降级解析比直接失败危险得多。第二，**错误消息是泄漏通道**：任何把快照内容拼进异常的做法都要被白名单挡住，`raise ... from None` 只抑制显示、清不掉对象里仍可达的数据。第三，**「结果已接受但未落盘」必须是一个独立状态**，否则恢复要么重复副作用，要么丢掉一个用户已经看到的结果；`_terminal_unrecoverable` 就是给这个窗口起的名。

对选型的判断价值：看一个框架的持久化对象里有没有「发送方/接收方视角分开的历史列表」与「待结算写入」。两者都没有的框架，通常在「输出被护栏拒绝」或「交接改了输入」这类场景下会出现历史与输入不一致——而那正是线上最难复现的一类问题。
