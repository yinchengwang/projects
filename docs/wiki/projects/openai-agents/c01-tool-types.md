---
project: openai-agents
feature: tool-types
title: 工具类型体系与输出形态
page_type: feature
schema_version: 1
refs:
  - path: src/agents/tool.py
    symbols: [ToolOutputText, ToolOutputImage, FunctionToolCustomDataContext]
  - path: src/agents/tool.py
    symbols: [CustomToolCustomDataContext, ComputerToolCustomDataContext, ApplyPatchToolCustomDataContext]
---

# 工具类型体系与输出形态

## 一句话职责

用一组 dataclass 区分「谁在执行工具」（本地函数、本地进程、服务端托管），再用一组 pydantic 模型统一「工具把什么交还给模型」（文本、图片、文件），使运行时不必为每种工具各写一条输出处理路径。

## 机制是什么

`Tool` 是一个联合类型，把十三个具体类绑在一起：`FunctionTool`、`CustomTool`、`ShellTool`、`ApplyPatchTool`、`LocalShellTool`、`ComputerTool`、`FileSearchTool`、`WebSearchTool`、`HostedMCPTool`、`ImageGenerationTool`、`CodeInterpreterTool`、`ToolSearchTool`、`ProgrammaticToolCallingTool`。

这十三个类实际只分两族：

- **本地执行族**：有 `on_invoke_tool` 或 `executor`/`editor`/`computer` 之类的回调，SDK 自己把工具跑起来，再把结果写回对话。`FunctionTool`、`CustomTool`、`ShellTool`（local 环境）、`ApplyPatchTool`、`ComputerTool`、`LocalShellTool` 都属于这一族。
- **服务端托管族**：只带参数（例如 `FileSearchTool` 的 `vector_store_ids`、`WebSearchTool` 的 `search_context_size`），没有本地执行入口。模型在服务端完成工具调用，SDK 只负责把定义送上去、把返回的调用项写进历史。`FileSearchTool`、`WebSearchTool`、`HostedMCPTool`、`ImageGenerationTool`、`CodeInterpreterTool`、`ToolSearchTool`、`ProgrammaticToolCallingTool` 属于这一族。

工具输出有独立的一层类型：`ToolOutputText`、`ToolOutputImage`、`ToolOutputFileContent` 三个 pydantic 模型，以及各自的 `TypedDict` 变体；三者组成 `ValidToolOutputPydanticModels` 联合，并用一个 `TypeAdapter` 兜住运行时的解析。

## 怎么实现的

**三态输出模型的字段约束。** `ToolOutputText` 只有 `type="text"` 与 `text`。`ToolOutputImage` 允许 `image_url`（URL 或 data URL）与 `file_id`（已上传内容的 id）二选一，外加可选的 `detail`（`low`/`high`/`auto`）；构造后由 `model_validator(mode="after")` 检查两者至少给一个，都不给直接 `ValueError`。`ToolOutputFileContent` 同理但有三选一：`file_data`（base64）、`file_url`、`file_id`，还可带 `filename` 作为提示。三个类都带 `type` 字面量字段，因此判别式联合无需额外映射表。

**为什么同时存在 pydantic 与 TypedDict 两套。** pydantic 版本用于应用侧构造（有校验、有默认值），`*Dict` 版本用于 wire 层与序列化（`type` 与字段全为 `NotRequired`，允许部分快照）。这不是冗余：`ItemHelpers.tool_call_output_item` 这类转换点必须能接受「结构上像但还没校验过」的字典。

**函数工具的可选字段分三段。** `FunctionTool` 的公开字段按语义分三段，且注释明确说明字段顺序是为保持位置构造兼容性而排的：

- 基础：`name`、`description`、`params_json_schema`、`on_invoke_tool`、`strict_json_schema`、`is_enabled`。
- 工具级护栏：`tool_input_guardrails`、`tool_output_guardrails`。源码注释写明「把护栏字段放在 `needs_approval` 之前，以保持 v0.7.0 的位置构造兼容」。
- 审批与超时：`needs_approval`、`timeout_seconds`、`timeout_behavior`、`timeout_error_function`。

此后是 `defer_loading`（让 Responses API 暂时隐藏定义，等 tool search 加载）、`allowed_callers`（哪些调用方可调用，`direct` / `programmatic`）、`output_json_schema`（给程序化调用方的输出 schema）。

再往下是一批 `kw_only` 私有字段，它们不参与公开契约但驱动运行时行为：`_output_type_adapter`（输出校验用的 TypeAdapter）、`_failure_error_function` 与 `_use_default_failure_error_function`（失败文案策略）、`_is_agent_tool` 与 `_agent_instance`（agent-as-tool 标记）、`_is_codex_tool`、`_tool_namespace` 与 `_tool_namespace_description`、`_mcp_title`、`_tool_origin` 与 `_emit_tool_origin`、`_mcp_tool_binding`、`_mcp_annotations`。

**构造期的一次性规范化集中在 `__post_init__`。** 它按顺序做五件事：规范化 `allowed_callers`（校验只含允许值、去重、复制）；规范化 `output_json_schema`；若 `on_invoke_tool` 带 `__agents_bind_function_tool__` 就重新绑定到自身（这是复制语义的关键）；`strict_json_schema` 为真时对参数 schema 的**副本**跑 `ensure_strict_json_schema`；最后校验超时配置是否自洽。把规范化塞进 `__post_init__` 而不是构造器，是为了让手写的 `FunctionTool(...)` 与装饰器产出的工具走同一套校验。

**`__copy__` 的两个陷阱。** 复制不复用构造器（否则会重跑子类自己的 `__init__` 签名），而是 `object.__new__` + 复制 `__dict__`。但若子类把自身的绑定方法当作 `on_invoke_tool`，直接复制会让副本仍调用原实例的方法——所以复制后专门检查并重绑 `invoker.__func__.__get__(copied_tool, ...)`。然后再调一次 `FunctionTool.__post_init__`，让副本重新完成规范化。

**四类自定义数据上下文。** `custom_data_extractor` 是「只给 SDK、不给模型」的旁路：`FunctionToolCustomDataContext`（tool_context、tool、output、raw_item）、`CustomToolCustomDataContext`（多一个原始 input）、`ComputerToolCustomDataContext`（run_context、tool、tool_call、截图 data URL、raw_item）、`ApplyPatchToolCustomDataContext`（run_context、tool、operations、output、status、raw_item）。四者都是 `frozen` dataclass，且都携带 `raw_item`——即「将要回放给模型的那条原始项」，让提取器能对齐原始载荷而不用重新拼装。

**来源标记 `ToolOrigin`。** `ToolOriginType` 只有三个成员：`FUNCTION`、`MCP`、`AGENT_AS_TOOL`。`ToolOrigin` 是 frozen dataclass，除 `type` 外还有三个可选字段 `mcp_server_name`、`agent_name`、`agent_tool_name`，并提供 `to_json_dict` 与 `from_json_dict`。`from_json_dict` 对非法输入不抛异常而是返回 `None`——因为这段数据来自持久化的历史，容忍坏数据比中断恢复更重要。`get_function_tool_origin` 的语义是：`_emit_tool_origin` 为假时返回 `None`（完全不上报）；否则返回 `_tool_origin`，没有显式设置就退回 `ToolOrigin(type=FUNCTION)`。

**`mcp_annotations` 是深拷贝属性。** 它返回捕获到的 MCP 注解的拷贝，源码注释明确两点：一是省略的提示保持省略，调用方必须自己套 MCP 默认值；二是这些是服务端给的**提示**，不是「有副作用」「执行成功」「可安全重试」的证明，信任判断归应用。

## 关键取舍

**源码事实：** 输出形态只覆盖三种：文本、图片、文件。没有「结构化对象输出」这个输出类型——需要结构化输出的工具走的是 `output_json_schema` / `output_type`（声明式），而不是 `ToolOutput*` 系列的返回形态。这两条路互不相干：前者约束「输出必须匹配声明的对象 schema」，后者描述「输出该以什么模态交给模型」。

**源码事实：** 托管族工具没有本地执行入口。`FileSearchTool`、`WebSearchTool` 这类类只有参数与 `name` 属性，`__post_init__` 里最多做参数归一化。这意味着「工具是否真的执行了」在 SDK 侧无从验证，只能等模型返回调用项。

**源码事实：** `ShellTool` 与 `ApplyPatchTool` 同时支持本地与托管，靠 `environment` 的类型区分：`environment` 为 `local` 时必须有 `executor`；为托管时反过来禁止 `executor`，并且**强制**把 `needs_approval` 置回 `False`、`on_approval` 置 `None`（若应用显式设了就抛 `UserError`）。这等于说「托管执行不能走本地审批」，审批权在服务端。

**源码事实：** `allowed_callers` 与 `is_enabled` 是两件事。前者是「这个工具允许被谁调用」（直接调用还是程序化调用），后者是「此刻它是否出现在模型视野里」。前者在构造期校验一次，后者每次装配工具列表时求值。

**源码事实：** `custom_data_extractor` 的四类上下文各自绑死一种工具，没有统一基类。代价是新增工具类型要新增一个上下文类；收益是每个提取器拿到的字段恰好是它能用的，不需要在一堆 `Any` 里挑。

**源码事实：** `ToolOrigin.from_json_dict` 遇到非法 `type` 返回 `None` 而不是抛错，`to_json_dict` 则只写非 `None` 字段。这一对使来源标记在持久化往返中是「尽力保留」，而不是「要么完整要么失败」。

**架构推论：** 把「谁执行」与「输出是什么形态」拆成两个正交维度，是这个工具体系的骨架。前者靠类继承表达（有没有 `on_invoke_tool`），后者靠返回值表达（返回 `ToolOutputText` 还是裸字符串）。运行时只需要处理输出形态这一维，所以在 `FunctionTool`、本地 `ShellTool`、`ApplyPatchTool` 三条完全不同的执行路径上可以复用同一段「写回对话」的代码。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/tool.py` | `ToolOutputText` | 文本输出形态，唯一必填字段是 `text` |
| `src/agents/tool.py` | `ToolOutputImage` | 图片输出，`image_url` 与 `file_id` 至少给一个 |
| `src/agents/tool.py` | `ToolOutputFileContent` | 文件输出，`file_data`/`file_url`/`file_id` 三选一 |
| `src/agents/tool.py` | `FunctionTool` | 本地函数工具：基础字段 + 护栏 + 审批 + 超时 + 私有运行时字段 |
| `src/agents/tool.py` | `__post_init__` | 构造期规范化：callers、输出 schema、invoker 重绑、strict schema、超时 |
| `src/agents/tool.py` | `FunctionToolCustomDataContext` | 函数工具的旁路数据上下文，不进模型 |
| `src/agents/tool.py` | `CustomToolCustomDataContext` | 原始字符串输入工具的旁路数据上下文 |
| `src/agents/tool.py` | `ComputerToolCustomDataContext` | 电脑工具的旁路数据上下文，含截图 data URL |
| `src/agents/tool.py` | `ApplyPatchToolCustomDataContext` | 补丁工具的旁路数据上下文，含状态字段 |
| `src/agents/tool.py` | `ToolOrigin` / `ToolOriginType` | 来源标记：函数、MCP、agent-as-tool |
| `src/agents/tool.py` | `ShellTool` | 同族双形态：本地需 executor，托管禁审批 |
| `src/agents/tool.py` | `Tool` | 十三个具体类的联合类型 |

## 参考

- [openai-agents-python 源码：tool.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/tool.py)
- [Agents SDK 文档：Tools](https://openai.github.io/openai-agents-python/tools/)
- [OpenAI 文档：工具与函数调用](https://platform.openai.com/docs/guides/function-calling)

## 与主核 / AI 的关系

这套「执行者一族 + 输出形态一维」的划分，与主核里「存储引擎实现各异、但对外只有统一的页/记录接口」是一个思路：把差异关在实现里，把共性提到接口上。差别是主核的接口是方法签名，这里的接口是返回值的类型。

对 agent 工程最实用的三条推论。其一，**输出形态决定了模型怎么看结果**：返回 `ToolOutputImage` 会把图片作为图片送进上下文，返回裸字符串则只是一段文本（base64 也在里面）。同一个工具换一种返回形态，模型的推理质量可能明显不同，而调用代码看起来只差一行。其二，**托管工具的执行不可验证**：SDK 只能看到模型声称发生的调用，无法确认服务端真的执行了、执行成功没有。审计与重试策略必须按这个前提设计。其三，**旁路数据是排障的主入口**：`custom_data_extractor` 拿到的 `raw_item` 与 `output` 是模型与运行时之间真实的交界，把护栏判定、原始参数、改写后的输出都存在这里，才能在不污染上下文的前提下留下可复盘证据。
