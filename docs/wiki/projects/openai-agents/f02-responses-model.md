---
project: openai-agents
feature: responses-model
title: Responses 模型的工具与流
page_type: feature
schema_version: 1
refs:
  - path: src/agents/models/openai_responses.py
    symbols: [OpenAIResponsesModel, _materialize_responses_tool_params, _usage_from_response]
  - path: src/agents/models/openai_responses.py
    symbols: [OpenAIResponsesWebSocketOptions, _construct_response_stream_event_from_payload]
---

# Responses 模型的工具与流

## 一句话职责

把 SDK 内部的工具模型、输出 schema、交接列表翻译成 Responses API 的 `tools` 参数，再通过 HTTP 或持久 WebSocket 发出请求，把响应与流事件原样交回接口层。

## 机制是什么

`OpenAIResponsesModel` 是 `Model` 的原生实现，只持有三样东西：`model`（字符串或 SDK 的 `ChatModel`）、`AsyncOpenAI` 客户端、以及一个 `model_is_explicit` 标记。它不缓存消息、不管历史、不做重试——那些在运行器层。

三件机制是这一页的重点：

**工具翻译。** `convert_tools` 把 `Tool` 与 `Handoff` 列表转成 `ResponsesToolParam` 列表，同时返回一个 `includes` 列表（告诉服务端「请把加密推理内容一并返回」之类的附加请求）。函数工具、托管工具（web search、file search、computer、code interpreter）、MCP 工具各走不同分支；带 namespace 的函数工具会被归并成 namespace 分组，同组内描述必须一致否则报错。

**两种传输。** 默认走 HTTP；若客户端配置了 `use_responses_websocket`，则走一条**持久 WebSocket 连接**，请求以 `{"type": "response.create", "stream": true}` 帧发出。WebSocket 路径自己维护连接身份（URL 加排序后的头）、请求锁、close 代数、以及断线后的强制中止回退。

**输出 schema 的位置。** `get_response_format` 产出 `{"format": {"type": "json_schema", "name": "final_output", "schema": ..., "strict": ...}}`，纯文本时返回 `omit`。名字固定为 `final_output`，这与 Chat Completions 路径完全一致——两条路径给模型的输出契约名相同。

## 怎么实现的

**工具分类与拒绝。** `convert_tools` 里第一件事是收集所有 `ComputerTool`，多于一个直接 `UserError`（「只能提供一个 computer 工具」）。随后对每个工具判断：带 namespace 的 `FunctionTool` 走分组逻辑（首次出现时在列表里占一个 `None` 占位，记下索引与描述，后续同 namespace 的工具追加进同一个 `FunctionToolParam` 列表）；其余按类型分派。

**namespace 的占位技巧。** 因为 namespace 分组要一次性产出单个工具条目，而遍历是逐个进行的，所以代码先用 `None` 占位并在最后回填。这个「先占位后回填」的写法在需要按组聚合的遍历里很常见，代价是中间态列表里存在 `None`，任何提前返回都必须保证回填完成。

**托管工具的形状。** `computer_use_preview` 与 `computer` 是两个不同取值：源码注释说明「`computer_use` 是兼容别名，但 GA 的内建工具表面是 `computer`」。所以是否用 preview 由 `_should_use_preview_computer_tool(model, tool_choice)` 决定，取决于模型与 tool_choice 的组合——这是把「模型代际差异」编码进工具形状的典型。

**tool_search 的规则。** `validate_responses_tool_search_configuration` 在转换前先校验配置，参数里带 `allow_opaque_search_surface`。说明 tool search 有两种合法形态：一种是公开的、可枚举的工具定义；另一种是「不透明表面」——服务端自己决定有哪些工具，客户端只发一个搜索入口。后者的开关是显式的，因为它放松了「工具清单必须可知」这条前提。

**材料化。** `_materialize_responses_tool_params` 做两件事：`_to_dump_compatible` 把所有工具对象转成纯字典（处理 pydantic 模型与 TypedDict 的混合），再逐个过 `_require_responses_tool_param` 做类型断言。传的是一个 `list`，所以异常信息里能指出「材料化后的载荷必须是列表」——这类断言在 pydantic 宽松模式下是必要的护栏。

**用量的兜底。** `_usage_from_response` 分两支：`response.usage` 非空就用 `_response_usage_to_usage` 正常转换；为空则 `Usage(requests=_requests_for_response_without_usage(response))`。配合 `_mark_transport_request_without_usage`——后者在 `response.usage is None` 时给响应打一个「本次请求没有用量」的标记。于是「provider 没给 usage」与「这次请求根本没发生」被区分开：前者计 1 次请求但 token 为 0。

**流的中介。** `_ResponseStreamWithRequestId` 包住 SDK 的流，做四件事：把 `request_id` 挂到每个事件携带的 response 上（`_attach_request_id`，失败静默）、记录是否已产出终止事件（`_yielded_terminal_event`）、在 `response.completed` 时调 `_mark_transport_request_without_usage`、以及处理关闭。终止事件集合是五个字符串：`response.completed`、`response.failed`、`response.incomplete`、`error`、`response.error`。

**关闭的两种语义。** `_close_stream_allowing_background_completion` 把 `aclose()` 包成 task 再 `asyncio.shield`：取消到来时不杀这个 task，而是 detach 出去让它自己跑完。注释解释了原因——取消可能正好在 `aclose()` 已经等 provider 时到来，shield 让它在后台完成而不是半途丢弃，同时避免第二次关闭（重复关 provider 流不保证安全或幂等）。

`_schedule_async_iterator_close` 则是「完全不等待」的版本：起 task 后直接 detach。所以同步调用路径（由运行器驱动）用一个，异步取消路径用另一个。

**WebSocket 的连接身份与复用。** `_ensure_websocket_connection` 用 `(ws_url, sorted(headers))` 作为连接身份；身份相同、且连接属于当前事件循环（`_ws_connection_loop_ref() is running_loop`）、且连接仍可复用（`_is_websocket_connection_reusable` 依次探 `state.name == "OPEN"`、`closed`、`open`、`close_code`）就直接复用，否则先 drop 旧的再建新的。

`close()` 的处理分了三条：递增 close 代数（让正在进行中的请求在下一检查点自行失效）、若请求锁被持有则**强制中止**连接（`_force_abort_websocket_connection` 走 transport 的 `abort`/`close`，因为跨事件循环 await 关闭不可靠）、否则正常 drop。这个「锁被持有就强杀」的分支只在跨循环清理时走到，注释称之为「best-effort fallback」。

**事件解析的方式。** `_construct_response_stream_event_from_payload` 用 OpenAI SDK 的**内部函数** `openai._models.construct_type` 把 websocket 收到的 JSON 载荷解析成 `ResponseStreamEvent`。`ImportError` 时给出可操作的错误：「升级这两个 SDK 版本对，或把 Responses 传输切回 HTTP」。这是一处对私有 API 的依赖，被显式承认并给了退路。

**对 prompt 的所有权。** `_prompt_owns_model(prompt)` 是 `prompt is not None and not self._model_is_explicit`。也就是说：当 prompt 存在且模型不是用户显式指定时，**模型名由 prompt 决定**，从而 reasoning 与 verbosity 的默认值也不该由本地推断——`_get_request_settings` 用这两个布尔去 `_omit_model_defaults`。这是把「谁拥有这次请求的模型选择」变成可判定的条件。

**WebSocket 的超时选项。** `OpenAIResponsesWebSocketOptions` 有三个键：`ping_interval`、`ping_timeout`、`max_size`。注释指出了各自的实际后果：`websockets` 库默认 20 秒心跳，设 `None` 关掉；`ping_timeout=None` 保留心跳但禁用超时（应对大延迟抖动）；`max_size` 默认无限制，**设置为显式上限用于约束长驻进程的内存**。这三条都是对「长驻 agent 进程」场景的直接回应。

## 关键取舍

**源码事实：** WebSocket 是持久连接，所以模型实例持有跨请求状态（连接、身份、锁、代数）。这意味着同一个实例被并发用于多个运行时会互相影响，除非每次都走 `close()`——`close()` 里的代数递增就是为了处理这种「关闭时还有请求在飞」的情况。

**源码事实：** `_construct_response_stream_event_from_payload` 依赖 `openai._models.construct_type` 这个私有函数，并在不可用时抛 `RuntimeError` 要求用户升级版本对或切回 HTTP。这是少见的「主动承认对私有 API 的依赖并给出降级路径」的写法。

**源码事实：** 终止事件用字符串集合判定，而不是 `isinstance` 检查具体的 SDK 事件类。原因是 websocket 路径的事件是从 JSON 载荷构造的，可能落到不同的类上；按 `type` 字符串判断更稳。

**源码事实：** `_usage_from_response` 在缺 usage 时仍然产出一个「1 次请求、0 token」的 `Usage`，而不是 `None` 或全零。这让「请求发生了但没记账」与「请求没发生」在后续统计里可区分。

**源码事实：** 关闭做了两套：一套 shield 让关闭在后台完成，一套起 task 直接 detach。两者都在解决「取消不要留下半关的连接」，差别是前者还保证调用方在成功路径上等到关闭结束。

**架构推论：** 工具翻译的复杂度几乎全部来自「一个 API 里混了多种工具形态」。函数工具是纯数据、托管工具是带类型的服务端能力、MCP 工具是远端目录的映射、namespace 分组还要聚合。把这些塞进一个 `convert_tools` 而不用多层抽象，是因为输出形状由外部 API 规定——多一层抽象也只是换个地方写同一堆分支。

**架构推论：** WebSocket 路径的存在说明「高并发 agent 场景下 HTTP 握手成本变得可观」。它的代价是把一堆 HTTP 里不存在的问题引进来：连接身份、跨事件循环、连接半开、关闭不可重入。代码里那些 close 代数与强制中止回退，全是这条路上的必要成本。

**架构推论：** `_prompt_owns_model` 这个判断很有意思——它把「模型名是谁决定的」变成影响**参数默认值**的条件。这是服务端模板能力带来的新耦合：模板一旦拥有模型，客户端就不该再按模型名推断 reasoning 支持情况，否则会发出与模板矛盾的通知。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/models/openai_responses.py` | `OpenAIResponsesModel` | Responses API 的 `Model` 实现 |
| `src/agents/models/openai_responses.py` | `OpenAIResponsesModel.convert_tools` | 工具与交接转成 Responses 工具参数 |
| `src/agents/models/openai_responses.py` | `OpenAIResponsesModel.get_response_format` | 输出 schema 包装成 `final_output` 格式 |
| `src/agents/models/openai_responses.py` | `OpenAIResponsesModel._get_request_settings` | 按 prompt 所有权修剪推断设置 |
| `src/agents/models/openai_responses.py` | `OpenAIResponsesModel._ensure_websocket_connection` | 连接身份比对与复用判定 |
| `src/agents/models/openai_responses.py` | `OpenAIResponsesModel._is_websocket_connection_reusable` | 多路探测连接是否仍可用 |
| `src/agents/models/openai_responses.py` | `OpenAIResponsesModel.close` | 递增关闭代数并按锁状态选择强杀或正常关闭 |
| `src/agents/models/openai_responses.py` | `_materialize_responses_tool_params` | 工具参数转纯字典并逐个类型断言 |
| `src/agents/models/openai_responses.py` | `_usage_from_response` | 用量转换，缺 usage 时计一次请求 |
| `src/agents/models/openai_responses.py` | `_ResponseStreamWithRequestId` | 流包装：挂 request_id、标记终止、识别缺用量 |
| `src/agents/models/openai_responses.py` | `_construct_response_stream_event_from_payload` | 用 SDK 私有构造器解析 websocket 载荷 |
| `src/agents/models/openai_responses.py` | `OpenAIResponsesWebSocketOptions` | 心跳、心跳超时、最大消息尺寸三个选项 |

## 参考

- [openai-agents-python 源码：models/openai_responses.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/models/openai_responses.py)
- [OpenAI 文档：Responses API](https://platform.openai.com/docs/api-reference/responses)
- [Agents SDK 文档：Models](https://openai.github.io/openai-agents-python/models/)

## 与主核 / AI 的关系

主核里最接近的是「网络协议适配层」——把内部请求编码成线上格式，再解回来的那段代码。那里的复杂度同样集中在「同一语义在协议里有多种表达」上（比如不同的行格式、不同的空值表示）。Responses 的工具翻译是同一类问题：`computer_use` 与 `computer` 是同一能力的两个取值，`tool_search` 有公开与不透明两种形态，namespace 是分组而非独立实体。这类多样性无法靠抽象消除，只能靠集中到一处并逐条注释说明。

与 Chat Completions 实现相比，这一页的模型几乎没有「降级」逻辑：Responses API 是原生的、能力齐全的，所以它不需要警告「这个功能不支持」。反过来 Chat Completions 实现里塞满了 `_handle_unsupported_*`。这解释了为什么同一个接口下两个实现的体积差得远——一个在表达「我能做什么」，另一个在表达「我不能做什么」。

对 AI 工程最实用的一条是那个「缺 usage 也计一次请求」的处理。做成本统计时最怕的是「请求发出去了但没记账」，它会让预算控制失效。显式区分「没有用量数据」与「没有请求」需要 provider 适配器在拿不到 usage 时主动标记，否则上层只能看到全零，无法判断是限流还是真的没调。

另一条是 WebSocket 关闭的那套处理。它揭示了一个通用陷阱：**在取消路径上做清理，清理本身也可能被取消**。shield 加 detach 的组合是标准解法——让清理在后台跑完，调用方立即得到取消信号。任何在 `finally` 里 await 网络操作的地方都会遇到同一问题。
