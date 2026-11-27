---
project: pydantic-ai
feature: mcp-integration
title: MCP 接入与能力映射
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/mcp.py
    symbols: [MCPError, Resource, ResourceTemplate]
  - path: pydantic_ai_slim/pydantic_ai/_mcp.py
    symbols: [map_from_mcp_params, map_from_pai_messages]
---

# MCP 接入与能力映射

## 一句话职责

把 MCP 服务器接成一个普通工具集，并在两个协议之间做双向翻译：MCP 的 `sampling/createMessage` 请求要变成 pydantic-ai 的消息历史，pydantic-ai 的历史要变成 MCP 的 sampling 消息；同时把服务器的资源、模板、错误、能力声明映射成框架自己的类型。

## 机制是什么

`mcp.py` 提供两类东西：

- **接入层**：`MCPToolset` 是一个 `AbstractToolset`，把 FastMCP `Client` 包成工具集；另有若干协议对象的本框架版本——`MCPError`（服务器错误响应）、`Resource` / `ResourceTemplate`（资源与模板）、`Prompt` / `PromptArgument`、`PromptResult` / `PromptMessage`、`EmbeddedResource`、`ResourceLink`、`Icon`、`ResourceAnnotations`，以及 `ServerCapabilities`（服务器声明支持什么）。
- **配置层**：`load_mcp_toolsets(config_path)` 从 JSON 配置批量构建工具集，支持 `${VAR}` 与 `${VAR:-default}` 环境变量展开。

`_mcp.py` 是纯映射模块，两个方向的入口：`map_from_mcp_params(params)` → `list[ModelMessage]`，`map_from_pai_messages(messages)` → `(system_prompt, list[SamplingMessage])`。

## 怎么实现的

**`MCPError` 保留协议的三元组。** 它有 `message`、`code`、`data` 三个字段，文档说明它「按 MCP 规范的 `ErrorData` schema 包装服务器错误响应」。保留 `code` 与 `data` 而不是只留消息，是为了让应用能按协议错误码分支，而不是靠解析字符串。

**`Resource` 与 `ResourceTemplate` 是服务器资源的两面。** `Resource` 有具体 `uri` 与可选 `size`（原始字节数，base64 编码前）；`ResourceTemplate` 有 `uri_template`（RFC 6570）。两者都从 MCP SDK 类型转换（`from_mcp_sdk`），且都携带 `annotations` 与 `icons`。转换时对可选字段用 `mcp_optional_field` 而不是直接取属性——因为 MCP 有两个 SDK 世代，字段名与存在性会变，显式取字段比鸭子取属性更稳。

**`ServerCapabilities` 把 `None` 转成布尔。** 转换逻辑的核心是把「字段存在与否」当能力开关：`logging = mcp_capabilities.logging is not None`、`prompts = prompts_cap is not None`、`tools = tools_cap is not None`。而 `*_list_changed` 则要看子字段：`prompts_cap.list_changed`。这个「存在即支持」的约定来自 MCP 规范本身——客户端判断服务器能力靠字段有没有出现。

**工具结果的映射优先级。** `_map_mcp_call_tool_result` 先看 `structured_content`：若有且（显式偏好 或 所有 content 块都是文本），就用结构化内容。理由写在注释里：按文档，文本块里装的是结构化内容的 JSON 编码（为了向后兼容），所以当全部是文本时优先取结构化版本能得到原值。还有一处剥壳：MCP SDK 会把基本类型与 `list` 这类泛型包在 `result` 键下，而框架想要工具函数原样返回的值，所以「字典只有一个键且叫 `result`」时拆开。

**多内容块的两条收敛规则。** `_map_mcp_tool_results` 把每个块映射后，单块直接返回、多块返回列表。单块的例外处理避免了「一个文本结果变成 `["text"]`」这种无意义的包装。

**二进制内容的不对称被显式记录下来。** `_map_mcp_tool_result` 的注释点明：工具结果**不**把 MCP 注解与 `_meta` 保留到 `BinaryContent.vendor_metadata`，只有 `_map_mcp_prompt_part` 会（经 `_map_mcp_binary_content`）。注释还说这个不对称是刻意为之——工具返回的内容流向模型，提示内容流向应用。

**反方向：pydantic-ai → MCP sampling。** `map_from_pai_messages` 返回 `(system_prompt, sampling_msgs)`：所有指令与系统提示被**收集成一个字符串**（用双换行连接），因为 MCP sampling 协议只要一个 `system_prompt` 字符串。请求里的 `InstructionBaselinePart` / `InstructionDeltaPart` 直接抛错误（`_unprojected_instruction_delta_error`），因为这类结构化指令增量还没有对应的 MCP 表示，硬塞会静默失真。

**工具结果在 MCP 里必须聚合成一条消息。** `_map_request_history` 先把所有 `ToolReturnPart` 收集起来，最后统一 `insert(0, ...)` 成一条 `role='user'` 的 sampling 消息。注释写明原因：**MCP 要求一轮的所有结果放在同一条消息里，且排在普通用户内容之前**。这是一个协议级约束，与本框架「工具结果可按位置分散」的自由度不同。

**不支持的东西会明确报错而不是丢弃。** `_map_user_prompt` 对非 `str`、非图片 `BinaryContent` 抛 `NotImplementedError`（含一条 TODO 说音频未支持）；`_map_tool_return` 对带 `files` 的结果抛 `NotImplementedError('Multimodal tool results in MCP sampling are not yet supported')`；`_map_response_history` 遇到 `NativeToolCallPart` / `NativeToolReturnPart` / `FilePart` / `SpeechPart` / `CompactionPart` 抛 `UnexpectedModelBehavior`。这三种错误的分类不同：`NotImplementedError` 表示「协议侧还没有对应表示」，`UnexpectedModelBehavior` 表示「这个部件根本不该出现在这条路径上」。

**`ThinkingPart` 在响应历史里被跳过。** 它既不能映射成 MCP 内容，也不该报错——思考内容本来就不该到达 MCP 客户端。

**`_map_sampling_tool_result` 要求调用 ID 可对上。** 反向映射时用一个 `tool_names` 字典（`tool_call_id` → 工具名）还原工具名；若结果的 `tool_use_id` 在字典里找不到，抛 `UnexpectedModelBehavior('MCP tool result has no matching tool call')`。这条校验保证「每个结果都有一条调用」，与框架自己历史的不变式一致。

**`outcome` 从 `is_error` 推出。** `ToolReturnPart.outcome` 取 `'failed'` 或 `'success'`，依据是 MCP 的 `is_error` 字段。反向映射时 `isError` 由 `part.outcome in ERROR_OUTCOMES` 推出。

**`include_instructions` 是显式的信任开关。** 字段文档写得很直白：服务器写的这段文本会以**你自己写的指令同等的权威**到达模型，所以只对信任的服务器开启。它还说明了指令变化时的两种行为：有 `id` 的工具集用 `on_change='append'`（`toolset:<id>`）把新文本追加进对话而不是重写缓存前缀；无 `id` 的 stdio/进程内服务器没东西可 key，变化会重写前缀。

**`cache_tools` 的失效条件很精确。** 缓存默认开启，在两种情况下失效：服务器发 `notifications/tools/list_changed`，或工具集完全退出（最后一次 `__aexit__` 对应第一次 `__aenter__`）。文档还点出协议版本差异：在 `2026-07-28` 协议（FastMCP 4 默认）上，服务器只能在自己处理请求期间发这个通知，所以它在请求之间做的改动不会让缓存失效。最后一条例外：传预构建的 FastMCP `Client` 时缓存失效的 message handler 不会被安装，因此只能靠会话关闭来失效。

**`id` 缺省从 URL 主机名派生。** 文档说明：没有显式 `id` 时，URL 型工具集会用它主机名；而持久执行环境（Temporal、DBOS）**要求显式 id**，因为它用来识别该工具集在 workflow 里的 activities/steps。

**`prefer_tasks` 是一个客户端侧概念。** 它表示「对可选支持 task 增强执行的工具偏好用它」；必需 task 的工具总用它，禁止的永不用。文档同时指出这是 FastMCP 3 的概念——FastMCP 4 服务器自己决定 task 创建（SEP-2663），这个偏好在那里无效。

**`log_level` 有个版本陷阱。** FastMCP 3 支持；FastMCP 4 在旧协议会话上支持；**现代会话会告警并跳过**，因为该方法是握手时代的，规范期望客户端在 `log_handler` 里自己过滤。

**`tool_error_behavior` 三选项各有用途。** `'retry'`（默认）抛 `ModelRetry` 让模型自我纠正；`'error'` 把底层 `ToolError` 传给调用方；`'failed'` 抛 `ToolFailed` 让模型看到错误但不重试。三者对应「让模型改」「让应用处理」「让模型知道但不重试」。

## 关键取舍

**源码事实：** `include_instructions` 默认关闭，且文档明说原因：服务器写的指令会以应用同等的权威到达模型。这是一个显式的信任边界，默认取安全侧。

**源码事实：** MCP sampling 反方向映射会**收集**所有系统提示为一个字符串，因为协议只要一个。这意味着多段分层的指令在跨进 MCP 时会丢失层次。

**源码事实：** MCP 要求一轮的工具结果聚合成一条消息并排在普通用户内容之前，这与本框架的分散布局不同，映射层必须重排。

**源码事实：** 不支持的内容类型一律抛错（`NotImplementedError` 或 `UnexpectedModelBehavior`），不静默丢弃。这两种异常的选择本身是有信息的：前者是协议缺表示，后者是不该出现的部件。

**源码事实：** 工具结果不保留 MCP 注解到 `vendor_metadata`，提示内容才保留。这个不对称是刻意的，理由是两条流向不同。

**源码事实：** `cache_tools` 的失效依赖协议版本。同一份配置在 FastMCP 3 与 4 上行为不同，且「传预构建 Client」这个入口会让缓存失效机制整体退化。

**源码事实：** `id` 在持久执行环境下是必需的。缺省派生（URL 主机名）在普通运行够用，但在需要按 id 识别 durable unit 的场景不够。

**架构推论：** MCP 接入的本质是**协议翻译 + 生命周期托管**。翻译层的自由度受两边约束：本框架的历史是分部件、可任意排布的；MCP 的消息是少而整的。因此翻译必然是有损的——分层指令变一段字符串、分散的工具结果变一条消息、结构化指令增量直接不支持。理解这些损失点，比记住 API 参数更重要，因为它们决定「哪些 agent 设计过不了 MCP 这道桥」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/mcp.py` | `MCPError` | 服务器错误响应，保留 `code` 与 `data` |
| `pydantic_ai_slim/pydantic_ai/mcp.py` | `Resource` | 具体资源：URI、大小、注解、图标 |
| `pydantic_ai_slim/pydantic_ai/mcp.py` | `ResourceTemplate` | 参数化资源的 RFC 6570 模板 |
| `pydantic_ai_slim/pydantic_ai/mcp.py` | `ServerCapabilities` | 把「字段存在与否」转成能力布尔的声明表 |
| `pydantic_ai_slim/pydantic_ai/mcp.py` | `MCPToolset` | 把 FastMCP Client 包成工具集，托管连接与缓存 |
| `pydantic_ai_slim/pydantic_ai/mcp.py` | `MCPToolset.include_instructions` | 服务器指令的信任开关，默认关闭 |
| `pydantic_ai_slim/pydantic_ai/mcp.py` | `MCPToolset.cache_tools` | 工具列表缓存与「何时失效」的协议依赖 |
| `pydantic_ai_slim/pydantic_ai/mcp.py` | `_map_mcp_call_tool_result` | 优先结构化内容并剥掉 SDK 的 `result` 外壳 |
| `pydantic_ai_slim/pydantic_ai/mcp.py` | `_is_visible_to` | MCP Apps 的可见性：工具对模型或应用可见 |
| `pydantic_ai_slim/pydantic_ai/mcp.py` | `load_mcp_toolsets` | 多服务器 JSON 配置加载，含环境变量展开 |
| `pydantic_ai_slim/pydantic_ai/_mcp.py` | `map_from_mcp_params` | MCP sampling 请求 → 本框架消息列表 |
| `pydantic_ai_slim/pydantic_ai/_mcp.py` | `map_from_pai_messages` | 本框架历史 → 单个系统提示 + sampling 消息 |
| `pydantic_ai_slim/pydantic_ai/_mcp.py` | `_map_request_history` | 工具结果聚合成一条且排在最前 |

## 参考

- [pydantic-ai 源码：mcp.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/mcp.py)
- [pydantic-ai 源码：_mcp.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_mcp.py)
- [Pydantic AI 文档：MCP 概览](https://ai.pydantic.dev/mcp/overview/)
- [MCP 规范：服务端资源](https://modelcontextprotocol.io/specification/2025-11-25/server/resources)

## 与主核 / AI 的关系

MCP 接入与主核的「外部存储适配器」同型：一个外部系统有自己的协议与能力声明，适配器的职责是把它降级成本系统的统一接口，并明确记录哪些能力是**没有对应表示**的（而不是假装支持）。主核对外部引擎也是这么做的：能下推就下推，不能就明确报错。

对 agent 工程的直接价值有三条。其一，**`include_instructions` 是提权开关**。服务器提供的「指令」会以应用口气到达模型，等于把一部分系统提示的控制权交给远端。默认关闭是对的，开启前应先看那段文本写了什么。其二，**跨 MCP 的指令会丢失层次**。本框架里 `InstructionPart` 的静态/动态分层、可见性标记，在变成单个 `system_prompt` 字符串后全部消失。其三，**工具列表缓存与协议版本耦合**。在 FastMCP 4 上，服务器在请求之间改工具不会让缓存失效；如果你的服务端是动态工具集，要么关缓存，要么确保改动发生在处理请求期间。
