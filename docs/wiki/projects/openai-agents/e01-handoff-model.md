---
project: openai-agents
feature: handoff-model
title: 交接对象与输入改造
page_type: feature
schema_version: 1
refs:
  - path: src/agents/handoffs/__init__.py
    symbols: [Handoff, HandoffInputData, handoff]
---

# 交接对象与输入改造

## 一句话职责

把「换一个 agent 接手」表达成一次函数工具调用，并把「接手方看到什么历史」做成可插拔的过滤器：交接本身只负责切换目标，历史改造由 `input_filter` 决定，两者互不耦合。

## 机制是什么

`Handoff` 是一个 dataclass，字段可分四组：

- 模型侧定义：`tool_name`、`tool_description`、`input_json_schema`、`strict_json_schema`。
- 执行入口：`on_invoke_handoff`（接收上下文与参数 JSON 字符串，返回目标 agent）。
- 名字与回调：`agent_name`、`is_enabled`（布尔或按上下文求值的可调用）。
- 历史改造：`input_filter`、`nest_handoff_history`、`failure_error_function`。

`HandoffInputData` 是过滤器的输入输出，五个字段：

| 字段 | 含义 |
|---|---|
| `input_history` | 调用 `Runner.run()` 之前的输入历史 |
| `pre_handoff_items` | 交接发生那一轮之前生成的项 |
| `new_items` | 当前轮新产生的项，含触发交接的调用与交接工具的输出 |
| `run_context` | 交接时的上下文（后加，可选，向后兼容） |
| `input_items` | 若设置，作为下一个 agent 的输入；`new_items` 仍保留给会话历史 |

`HandoffInputFilter` 是 `Callable[[HandoffInputData], MaybeAwaitable[HandoffInputData]]`——同步或异步都可以，返回值必须是 `HandoffInputData`。

`handoff(agent, ...)` 是构造辅助函数，把目标 agent 包成交接对象。

## 怎么实现的

**默认命名规则。** `default_tool_name` 是 `transfer_to_{agent.name}` 经过 `_transforms.transform_string_function_style` 规范化（把不适合函数名的字符替换掉，且 `warn_on_whitespace=False` 表示名字里有空格不告警）。`default_tool_description` 是 `Handoff to the {name} agent to handle the request. ` 拼上目标 agent 的 `handoff_description`（可为空）。两者都可在构造时被 `tool_name_override` / `tool_description_override` 覆盖。

**参数 schema 的两态。** 若给了 `input_type`，用 `TypeAdapter(input_type).json_schema()` 生成 schema，并强制校验 `on_handoff` 必须存在且正好接收两个参数（上下文、输入）；若没给 `input_type`，schema 是空字典，此时若提供了 `on_handoff` 则要求它正好接收一个参数（只有上下文）。两种情况的参数个数错误都在**构造期**抛 `UserError`，而不是等到运行期。

**schema 强制 strict。** 不论哪一态，最后都会跑 `ensure_strict_json_schema(input_json_schema)`。空字典会变成 `{type: object, properties: {}, required: [], additionalProperties: false}`。源码注释说明「以后可以做成可配置」，当前是硬编码。

**执行时的两条分支。** `_invoke_handoff_impl` 若 `input_type` 存在：先检查 `input_json` 不是 `None`（是则给 span 挂个错误并抛 `ModelBehaviorError`），再用 `_json.validate_json(..., partial=False, strict=True, contains_tool_data=True)` 校验，把结果传给双参数版 `on_handoff`。若只有 `on_handoff` 而没有 `input_type`：直接单参数调用。没提供 `on_handoff` 时，实现什么都不做，直接返回目标 agent。无论哪条路，返回值恒为构造时捕获的那个 `agent`——文档明确说明 `handoff()` 永远返回它捕获的 agent，`on_handoff` 用于副作用或记账，不做动态选目标。

**同步/异步回调统一处理。** 两处都写成「先调用，结果 `inspect.isawaitable` 才 await」。因此 `on_handoff` 可以是协程函数也可以是普通函数。

**脱敏异常的包装。** `on_invoke_handoff` 实际指向 `_invoke_handoff_with_redaction`，它在内部调 `_invoke_handoff_impl`。若捕获到 `ModelBehaviorError` 且该错误被标记为「数据已脱敏」，就摘掉 traceback 帧、把作用域里的 `invoke_handoff`、`ctx`、`input_json` 三个引用清空（`input_json` 置成 `"<redacted>"`），再重抛。这与 `Runner.run` 的处理一致：脱敏异常意味着异常本身携带着不该外泄的数据，而 traceback 帧的 locals 会让数据仍然可达。

**`is_enabled` 的归一化。** 传入可调用时，包一层 `_is_enabled`：断言 `is_enabled` 可调用、断言目标 agent 是 `Agent` 实例、调用后按可等待性处理、最终 `bool(...)`。这条包装让「动态启用的交接」在装配阶段（`get_handoffs`）就能被过滤掉，而不是到运行期才发现目标不可用。

**弱引用的目标保存。** 构造完 `Handoff` 后执行 `handoff_obj._agent_ref = weakref.ref(agent)`。这个 `_agent_ref` 是 `init=False` 的私有字段，只在执行交接时被用来「顺手把目标 agent 的工具配置注册进运行时」。用弱引用而不是强引用，是为了让交接对象不延长目标 agent 的生命周期——典型场景是动态构造的交接表。

**`input_filter` 的三条重要事实，都写在字段文档里。** 一是流式模式下过滤器不产生任何流式输出（该输出的项在过滤前已经流过）；二是服务端管理会话（`conversation_id` / `previous_response_id` / `auto_previous_response_id`）不支持交接输入过滤器；三是**嵌套历史不是脱敏机制**——`nest_handoff_history` 会从 `input_history`、`pre_handoff_items`、`new_items` 三处拼历史，并忽略 `input_items`，所以要脱敏必须改这三处源字段本身（或改返回的 `input_history`），只过滤 `input_items` 挡不住嵌套历史里的敏感内容。

**`input_items` 的存在理由。** 它是唯一能同时满足「模型输入被裁剪」与「会话历史完整」的字段：设置它之后，模型看到的是裁剪后的项，而 `new_items` 原封不动地留给会话持久化。这也解释了为什么文档反复强调「只过滤 `input_items` 不能脱敏嵌套历史」——因为嵌套历史是从 `new_items` 那侧读的。

**`failure_error_function` 的边界。** 它只处理「调用交接函数时抛的异常」，把它转成一条给调用方的工具输出（于是调用方可以重试或自己回答，不必真的转交）。文档列出三处**不**在它覆盖范围内的东西：取消与 formatter 自身的错误照常传播；移交钩子（on_handoff 之后的 hook）与输入过滤器只在调用成功后才运行，因此不受它保护；恢复过程不回滚回调已经产生的副作用。

**`clone` 的一个细节。** `HandoffInputData.clone(**kwargs)` 用 `dataclasses.replace`，但会额外把 `_nested_history_owned_items` 这个私有属性从原对象搬到克隆体上。这个属性记录「哪些项在嵌套成摘要历史时被搬进了摘要里」，用于后续在会话持久化时对齐。若无条件 `replace`，这个私有属性会丢失，嵌套历史的来源追踪就断了。

## 关键取舍

**源码事实：** 交接是一次**工具调用**。模型看到的不是「切换 agent」这种抽象动作，而是一个名为 `transfer_to_xxx` 的函数。因此交接的可发现性完全依赖工具名与描述——模型不知道有这条路，就永远不会转交。

**源码事实：** 目标 agent 是构造时捕获的，不能动态选。文档写明 `handoff()` 永远返回捕获的 agent，`on_handoff` 只做副作用。想做「按条件转到不同 agent」，得注册多个交接工具，让模型自己选。

**源码事实：** 参数 schema 强制 strict，且「有 `input_type` 就必须有 `on_handoff`」。反过来说，交接工具的参数只服务于 `on_handoff` 的入参，**不改变接手方的主输入**——字段文档明确写了「它只描述传给 `on_invoke_handoff` 的结构化载荷，不替换下一个 agent 的主输入」。

**源码事实：** 输入过滤器与服务端管理会话互斥。这不是性能取舍，而是能力缺失：服务端历史由服务端持有，本地过滤器改不了它。因此启用 `conversation_id` 之类的特性就等于放弃交接历史裁剪。

**源码事实：** `HandoffInputData` 里既有 `new_items` 又有 `input_items`，是为了分离「模型看到什么」与「会话存什么」。这个分离在后来的版本里才加入（`input_items` 的文档用「当设置时」的措辞），属于向后兼容的追加字段。

**源码事实：** `failure_error_function` 明确不覆盖「交接后才会跑的东西」（钩子、过滤器）。也就是说，一旦交接成功，后续任何失败都按正常运行错误处理，不会退回成「交接失败，请调用方重试」。

**架构推论：** 把交接定义为「工具 + 历史过滤器」两个正交件，使同一条转换路径能表达很多形态：不传过滤器就是「原样继承全部历史」，传 `remove_all_tools` 就是「只交接对话不交接工具痕迹」，开 `nest_handoff_history` 就是「把历史压成一段摘要」。这三种形态对运行时来说完全相同，差别只在过滤器的返回值。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/handoffs/__init__.py` | `Handoff` | 交接对象：模型侧定义 + 执行入口 + 历史改造 |
| `src/agents/handoffs/__init__.py` | `HandoffInputData` | 过滤器输入输出：历史、轮前项、轮内项、可选输入覆盖 |
| `src/agents/handoffs/__init__.py` | `HandoffInputData.clone` | 复制并保留嵌套历史来源追踪的私有属性 |
| `src/agents/handoffs/__init__.py` | `handoff` | 构造辅助：默认命名、参数 schema、`on_handoff` 签名校验 |
| `src/agents/handoffs/__init__.py` | `default_tool_name` / `default_tool_description` | `transfer_to_*` 命名与描述生成 |
| `src/agents/handoffs/__init__.py` | `_invoke_handoff_with_redaction` | 脱敏异常：摘帧、清引用、重抛 |
| `src/agents/handoffs/__init__.py` | `HandoffInputFilter` | 同步/异步过滤器类型别名 |
| `src/agents/handoffs/__init__.py` | `nest_handoff_history` | 历史摘要化的入口（由 `input_filter` 或运行级开关触发） |
| `src/agents/run_internal/turn_preparation.py` | `get_handoffs` | 把 `Agent` 参与项转成 `Handoff` 并按 `is_enabled` 过滤 |
| `src/agents/models/openai_responses.py` | `_convert_handoff_tool` | 交接在 Responses 请求里就是一个 strict 函数工具 |

## 参考

- [openai-agents-python 源码：handoffs/__init__.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/handoffs/__init__.py)
- [Agents SDK 文档：Handoffs](https://openai.github.io/openai-agents-python/handoffs/)
- [Agents SDK 文档：交接输入过滤器](https://openai.github.io/openai-agents-python/ref/handoffs/)

## 与主核 / AI 的关系

这套设计的骨架与主核里的「权限/上下文移交」同构：移交时既换执行者，也决定新执行者能看到哪些历史。主核的移交靠会话与角色做隔离，这里的隔离靠一个纯函数——输入是完整历史，输出是下一方可见的历史。

对 agent 工程最实用的三条推论。其一，**交接的可靠性取决于工具描述**。模型只根据 `tool_description` 与工具名决定要不要转交，所以描述里该写「什么情况下该转交」而不是「这个 agent 是什么」。其二，**历史改造必须显式设计**。默认行为是接手方看到全部历史，包括前一个 agent 的所有工具调用与结果；这在多 agent 协作里既浪费上下文又可能泄漏内部实现。`remove_all_tools` 这类过滤器是常规操作而不是优化项。其三，**`input_items` 是唯一同时满足「裁剪模型输入」与「保留完整会话历史」的通道**，但它对嵌套摘要历史无效——要脱敏必须改源字段。把「脱敏」和「裁剪」当成两件事分别处理，是这类系统里最容易出错的地方。
