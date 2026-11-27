---
project: openai-agents
feature: result-objects
title: 运行结果与项模型
page_type: feature
schema_version: 1
refs:
  - path: src/agents/result.py
    symbols: [RunResult, RunResultStreaming, RunResultBase]
  - path: src/agents/items.py
    symbols: [RunItemBase, MessageOutputItem, ToolCallItem]
---

# 运行结果与项模型

## 一句话职责

把一次运行的产出拆成两层：`RunItem` 是「模型产生或工具产出的一条带类型的项」，`RunResult` 是「这次运行所有项的集合加最终输出加各类护栏结果」，并把「能转回 RunState 继续跑」所需的隐藏状态一并挂上。

## 机制是什么

`RunItemBase` 是所有项的共同基类，只有两个真正的数据字段：`agent`（哪个 agent 的运行产生了它）与 `raw_item`（Responses 协议里的原始项）。子类各自固定一个 `type` 字面量与更精确的 `raw_item` 类型。`to_input_item()` 把输出项转成可重放进请求的输入项。

现有子类覆盖 Responses 的全部输出形态：`InputItem`（恢复运行时被接纳的输入）、`MessageOutputItem`（助手消息）、`ToolCallItem`（普通工具调用）、`ToolCallOutputItem`（工具执行结果）、`HandoffCallItem` / `HandoffOutputItem`（交接的调用与结果）、`ReasoningItem`（推理项）、`ToolSearchCallItem` / `ToolSearchOutputItem`（工具搜索）、`MCPListToolsItem` / `MCPApprovalRequestItem` / `MCPApprovalResponseItem`（MCP 相关）、`CompactionItem`（压缩记账）、`ToolApprovalItem`（待审批）。

`RunResultBase` 有七个公开字段：`input`、`new_items`、`raw_responses`、`final_output`、四类护栏结果（输入、输出、工具输入、工具输出）、`context_wrapper`。`RunResult` 在此基础上补运行期状态（最后执行的 agent、最后处理过的响应、工具使用快照、当前轮次、会话与 previous id 等）；`RunResultStreaming` 补流式状态（当前 agent、当前轮次、事件队列、运行任务、已存异常等）。

## 怎么实现的

**agent 引用的弱引用化。** `RunItemBase.__post_init__` 存一份 `weakref.ref(self.agent)`；`__getattribute__` 对 `agent` 这个属性做了拦截，走 `_get_agent_via_weakref`。逻辑是：先从实例 `__dict__` 取，取不到（哨兵）就回落到正常的属性查找；取到非 `None` 就返回；取到 `None` 说明 `release_agent()` 已经把强引用放掉了，这时从弱引用解析，解析不到就返回 `None`。

`release_agent()` 做的是「把强引用换成弱引用」：存 `weakref.ref`，然后把 `__dict__["agent"] = None` 而不是 `del`——注释说「设为 None 而不是删除，让 dataclass 的 repr 与 asdict 继续工作」。

这套机制解决的问题是：一个结果对象可能持有几十个项，每个项都强引用着 agent，而 agent 又引用着工具、模型、记忆等一大片对象。长驻进程里只要结果对象没被释放，整个 agent 图都活着。显式 `release_agent()` 让调用方可以在拿到需要的字段后切断这条链。

**`RunResult.last_agent` 的同样处理。** `RunResult.__post_init__` 存 `_last_agent_ref`，而 `last_agent` 属性先从 `__dict__` 取，取不到再从弱引用取，都失败才抛 `AgentsException("Last agent reference is no longer available.")`。`_release_last_agent_reference()` 同理把强引用转弱引用并置 `None`。

注意这里多了一层：`RunItemBase` 解析失败返回 `None`，`RunResult` 解析失败抛异常。差别在于「项所属的 agent」是可选的元信息，「最后执行的 agent」是结果的必要部分——后者拿不到应该让调用方知道。

**输出项转输入项的清洗。** `to_input_item()` 走到 `_output_item_to_input_item`。后者先按 `type` 判断：`tool_search_call` / `tool_search_output` 走专门函数；其余则把 dict 浅拷贝或把 pydantic 模型 `model_dump(exclude_unset=True, by_alias=True)`，然后 `payload.pop("created_by", None)`。

`created_by` 是服务端给的输出侧元信息，不在 Responses 输入项 schema 里，重放回去会被拒。注释列出了携带它的类型：「apply_patch/shell 调用与工具调用输出」。

`shell_call_output` 还有一层：它的 `output` 是内容块列表，每个块自带 `created_by`。因为前面只做了浅拷贝，所以这里重建列表、为每个块创建新字典以剔除嵌套字段——注释说这「镜像了运行器在轮次结算里已经做的两级剥离」。

**`ModelResponse` 的 pydantic dataclass。** 它带四个字段：`output`（输出项列表）、`usage`、`response_id`（可为空，注释说明「不是所有 provider 都支持」）、`request_id`，以及 `raw_usage`——注释说它是「provider 用量载荷的 JSON 兼容快照，仅在保留开启时捕获」，捕获时机是「在 SDK 归一化缺失字段之前」。这个 `raw_usage` 的存在说明「归一化会丢信息」是已知问题：归一后 `input_tokens_details` 之类可能变成填充值，原始载荷才保留了「字段是否真的存在」。

`to_input_items()` 把输出列表逐个转过，注释解释为什么不能直接 `model_dump`：「几类项（工具搜索、apply_patch/shell 调用、工具调用输出）带有 `created_by` 这类只在输出侧存在的元数据，不属于输入 schema，所以要过重放清洗器」。

**`ToolCallItem` 的解析型访问器。** 它的 `raw_item` 类型宽（`Any`），因此提供两个 property：`tool_name` 与 `call_id`。两者都先看有没有 SDK 解析出的值（`_resolved_tool_name`），再看 `raw_item` 是 dict 还是对象，分别用 `.get(...)` 或 `getattr`。`call_id` 还做了双键兜底：`call_id` 取不到就用 `id`。

这两个 property 的存在说明原始载荷的形状不统一——有些 provider 回的调用项以 dict 形式给出、有的给出 typed 对象、有的缺 `name` 只给 `id`。用解析型访问器吸收这层差异，而不是让每个消费点各自处理。

**`RunResult.to_state()`。** 把结果转回 `RunState` 以便续跑。它需要搬的东西包括：当前轮次、最后处理过的响应（注释：「续跑中断时需要」）、已持久化到 session 的项数、工具使用快照、`_model_input_items`（筛选后用于构造模型输入的项）、原始输入、会话 id 与 previous response id、reasoning item id 策略，以及中断列表。

`RunResultStreaming` 的隐藏字段更多：`_event_queue`（`StreamEvent | QueueCompleteSentinel`）、`_input_guardrail_queue`、三类 task（运行循环、输入护栏、输出护栏）、`_stored_exception`、`_tool_error_selected`、`_cancel_mode`、`_waiting_on_event_queue`、`_active_stream_consumers`、`_stream_consumers_stopped`（`asyncio.Event`）。这些不是给人看的，而是「流式运行的可变状态机」。

**pydantic 兼容钩子。** `RunResultBase.__get_pydantic_core_schema__` 存在，注释说「RunResult 是运行期值，schema 生成应把它们当作实例而不是递归遍历内部 dataclass 字段」。所以结果对象被嵌进某个 pydantic 模型时，不会尝试展开它那一堆隐藏字段。

## 关键取舍

**源码事实：** `RunItemBase.agent` 通过 `__getattribute__` 拦截，任何对 `.agent` 的读取都走弱引用解析路径。这让「释放后仍能读」与「释放后读到 None」都成为正常状态——调用方必须能处理 `None`。

**源码事实：** 类型上 `RunItemBase.agent` 声明为非可选，但 `release_agent()` 之后实际会返回 `None`。类型标注与运行时行为在这一点上不一致，是为兼容既有测试与使用方而接受的妥协（注释里明确提到 `dataclass` 的 repr/asdict 需要字段存在）。

**源码事实：** `created_by` 的剥离是两级手写的（顶层 + `shell_call_output` 的嵌套块），注释说是镜像运行器里已有的同类逻辑。所以同一规则在两处出现，靠注释维持同步而非抽公共函数。

**源码事实：** `raw_usage` 只在「保留开启」时捕获，且捕获时机在归一化之前。所以它是一个「上游原始载荷」的可选副本，缺失不代表没有用量。

**架构推论：** 「项」这层抽象的核心价值是**把协议形状与业务语义分开**。`raw_item` 保留 Responses 的原始结构（可直接重放），子类与 `type` 提供业务语义（可直接渲染与分派）。没有这层，调用方要么处理一堆 `type` 字符串分支，要么丢掉可重放性。

**架构推论：** 弱引用机制的引入时机说明了一个长驻进程的现实：运行结果常被缓存、排队、在后台处理，其生命周期远超运行本身。若每个项都强引用 agent，则「保留一批历史结果」等于「保留所有 agent 及其工具、模型、连接」。显式释放把控制权交给调用方，代价是释放后部分字段不可用。

**架构推论：** `RunResult` 与 `RunResultStreaming` 共用 `RunResultBase` 但隐藏字段差异巨大，反映了「同步完成的结果是数据，流式进行中的结果是状态机」。把两者塞进一个类会让同步路径背上一堆流式字段；拆成两个则在基类处共享「已经产生了什么」的稳定部分。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/items.py` | `RunItemBase` | 项基类：所属 agent、原始项、转输入项 |
| `src/agents/items.py` | `RunItemBase.release_agent` | 把 agent 强引用换成弱引用 |
| `src/agents/items.py` | `MessageOutputItem` | 模型输出的一条消息 |
| `src/agents/items.py` | `ToolCallItem` | 工具调用，含解析型 `tool_name` 与 `call_id` |
| `src/agents/items.py` | `ToolCallOutputItem` | 工具执行结果，含解析后的 `output` |
| `src/agents/items.py` | `_output_item_to_input_item` | 重放前剥离输出侧 `created_by` 元数据 |
| `src/agents/items.py` | `ModelResponse` | 一次模型响应：输出项、用量、响应 id、原始用量 |
| `src/agents/result.py` | `RunResultBase` | 结果公共部分：输入、新项、原始响应、最终输出、四类护栏 |
| `src/agents/result.py` | `RunResult` | 非流式结果，含续跑所需隐藏状态 |
| `src/agents/result.py` | `RunResult.last_agent` | 弱引用解析，失败时抛异常 |
| `src/agents/result.py` | `RunResult.to_state` | 转成 `RunState` 以继续执行 |
| `src/agents/result.py` | `RunResultStreaming` | 流式结果：事件队列、task 句柄、消费者计数 |
| `src/agents/result.py` | `RunResultBase.__get_pydantic_core_schema__` | 声明为运行期值，阻止嵌套 schema 展开 |

## 参考

- [openai-agents-python 源码：result.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/result.py)
- [openai-agents-python 源码：items.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/items.py)
- [Agents SDK 文档：Results](https://openai.github.io/openai-agents-python/results/)

## 与主核 / AI 的关系

主核里对应的是「执行计划与执行结果」两套结构：计划节点带类型与参数，结果带实际产出与统计。这里的 `RunItem` 与之同构，但多了一层约束——它必须能**双向转换**：既要能从协议输出构造，也要能转回可重放的输入。这层双向性来自 LLM 应用的本质：历史必须完整回放，而回放要求形状与当初请求时一致。

与 AutoGen 的对照在「项」的粒度上。AutoGen 的消息是 `LLMMessage`（user/assistant/system 加内容），工具调用作为 assistant 消息的一个字段附带；这里是每个工具调用单独成项。后果是这里能对「一次并行多工具调用」逐项流式通知、逐项审批、逐项记录结果，而 AutoGen 需要拆消息。代价是项列表比消息列表长得多，且需要额外映射才能还原成 Chat Completions 的消息形状（f03 页里那 1000 多行的转换器正是做这件事）。

对 AI 工程最实用的一条是那个 `created_by` 剥离。它说明「服务端返回的项」与「客户端能发回去的项」不是同一个 schema，中间差着若干只在输出侧存在的字段。任何做「历史回放」的系统都会撞上这个问题，而症状通常是一句模糊的 400。把剥离规则显式写在 `to_input_item()` 里，并注释清楚哪些类型受影响，比在上层反复 try/except 便宜得多。

另一条是弱引用释放的取舍。长期持有运行结果时，若结果对象间接钉住了整个 agent 图，内存会随缓存条数线性增长而无法察觉。提供一个显式的 `release_agent()` 让调用方决定何时断开，比让运行时猜测何时安全更可控——代价是释放后字段读到的可能是 `None`，调用方必须为此写防御。
