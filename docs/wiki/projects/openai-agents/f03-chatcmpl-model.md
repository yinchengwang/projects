---
project: openai-agents
feature: chatcmpl-model
title: Chat Completions 转换与降级
page_type: feature
schema_version: 1
refs:
  - path: src/agents/models/openai_chatcompletions.py
    symbols: [OpenAIChatCompletionsModel]
  - path: src/agents/models/chatcmpl_converter.py
    symbols: [Converter, items_to_messages]
---

# Chat Completions 转换与降级

## 一句话职责

用同一个 `Model` 接口跑旧协议：把 Responses 形态的输入项逐条翻译成 Chat Completions 消息，把服务端独有的能力（可复用提示、服务端会话、部分推理设置）显式降级为警告或错误。

## 机制是什么

`OpenAIChatCompletionsModel` 与 Responses 实现共享接口，但它面对的是一条**能力更窄**的协议。于是它多出三类 Responses 实现里没有的东西：

**一个转换器。** `Converter` 静态类承担全部格式翻译，核心是 `items_to_messages`——把 `TResponseInputItem` 序列折成 `ChatCompletionMessageParam` 列表。这条翻译不是一对一映射：输入项里可能有「助手消息」「工具调用」「工具输出」「推理项」四类，而 Chat Completions 的助手消息是**一条消息里聚合多个 tool_calls**。所以翻译过程要维护「当前正在构造的助手消息」并决定何时 flush。

**一套降级策略。** 三个 `_handle_unsupported_*` 方法分别处理 prompt、服务端会话、推理设置。每个都遵循同一模式：`strict_feature_validation` 为真就抛 `UserError`，否则只在**首次**遇到时 warning 一次（用工个布尔标记抑制重复）。

**一个内容类型白名单。** `_validate_official_openai_input_content_types` 只对官方 OpenAI 端点生效，检查 user 消息的每个内容块类型是否在 `{"input_text", "input_image", "input_audio", "input_file"}` 里，不在就 `UserError` 并带出原始块。这是「同一协议不同实现支持不同内容类型」的显式护栏。

## 怎么实现的

**助手消息的聚合与 flush。** `items_to_messages` 用一个闭包变量 `current_assistant_msg` 保存正在构造的助手消息，`ensure_assistant_message()` 在需要时创建它（并初始化空 `tool_calls` 列表）。工具调用一旦到达就追加进当前的 `tool_calls`；遇到用户消息或其他类型时调 `flush_assistant_message()` 把它推进结果列表。

flush 里有个必要修补：**若 `tool_calls` 为空就删掉这个键**。源码注释写「API 不支持空的 tool_calls 数组」——所以「只是文本回复」的助手消息不能带 `tool_calls: []`，否则请求被拒。

**推理内容的三条来源。** 转换器同时处理三种 provider 的推理表示：`reasoning_content`（DeepSeek 风格，落在 assistant 消息的一个额外字段）、`reasoning`（另一种文本形式）、`thinking_blocks`（Anthropic 风格，带签名与 `redacted_thinking`）。三者的优先级由 `message_to_output_items` 里的判断决定：若已有结构化的 `reasoning` 或 `thinking_blocks`，就不再采用 `reasoning_content`。

这解释了 `pending_thinking_blocks` 与 `pending_reasoning_content` 为什么都是「待应用」状态：推理项在流或消息里出现的位置与它应归属的助手消息之间可能隔了若干项，所以先暂存、到 flush 时再一次性贴上去。flush 里还专门清空这些暂存值，注释说「推理块属于产生它的那一轮助手消息，未被直接跟随的推理项不能把签名块泄漏到后面那一轮」。

**thinking blocks 的两种落地方式。** `apply_pending_thinking_blocks` 分两支：若块是 provider 原生的（`pending_thinking_blocks_are_native`），直接写成消息的 `thinking_blocks` 字段——注释说这是 LiteLLM 的字段，能保留完整序列包括空 thinking 与 redacted 块；否则（历史里存的是归一化后的推理项）把块作为内容前缀插进 `content` 数组，保持旧版本的「内联重建」行为。

**tool_choice 的转换。** `convert_tool_choice` 先处理四个简单值（`None` → `omit`、`MCPToolChoice` → `UserError`、三个字符串原样），其余当作工具名转成 `{"type": "function", "function": {"name": ...}}`，但在那之前调 `ensure_tool_choice_supports_backend`——把「某些后端不支持按名字强制调用」这条约束集中到一处。

**输出 schema 的包装层级不同。** Responses 侧是 `{"format": {...}}`，Chat Completions 侧是 `{"type": "json_schema", "json_schema": {...}}`。两者名字都是 `final_output`、都带 `strict`，但嵌套结构不同——这是同一个语义在两条协议里的不同落点，转换器里各写一份。

**request_id 的继承。** Chat Completions 的流对象上挂着 HTTP 响应，所以 `_attach_stream_request_id` 从 `stream.response.headers` 里取 `x-request-id` 并写到 `response._request_id`。注释解释了两个细节：终止响应是 `model_copy()` 出来的副本，私有属性会被保留；自定义客户端或测试替身可能给一个没有 HTTP 响应的裸迭代器，所以要容忍取不到。

**官方端点的输入校验。** `_validate_official_openai_input_content_types` 只在 `ChatCmplHelpers.is_openai(self._client)` 为真时跑。它先把每个输入项尝试按 `EasyInputMessage` 或 `InputMessage` 解析，只检查 `role == "user"` 的消息，用 `_normalize_input_content_part_alias` 归一别名后再比对白名单。不在白名单里就抛错并带上完整的 `part`——报错信息足够定位到具体块。

**请求设置的修剪。** `_get_request_settings` 与 Responses 侧同构，但判断条件不同：`reasoning=self._tools_require_non_reasoning_mode(has_tools) or "reasoning_effort" in extra_args`。`_tools_require_non_reasoning_mode` 的实现是 `has_tools and self.model in {"gpt-6-sol", "gpt-6-luna"}`——把「某些模型带工具时不能开推理」这条经验固化成集合常量。

**响应为空的分支。** `get_response` 里 `if not response.choices` 时先取 `getattr(response, "error", None)`，拼成 `ModelBehaviorError`，文案是「响应没有 choices（可能是 provider 的错误载荷）」。这是兼容那些「HTTP 200 但主体是错误对象」的兼容端点。

## 关键取舍

**源码事实：** 降级是**一次性警告**而不是每次警告。三个 `_has_warned_*` 布尔保证每个不支持项只提一次。这对长驻进程是必要的——否则每个 turn 都刷一条同样的 warning。

**源码事实：** 默认是「警告并忽略」，只有开了 `strict_feature_validation` 才抛错。所以从 Responses 切到 Chat Completions 时，用了 prompt 的代码**不会失败**，只是 prompt 悄悄不生效。

**源码事实：** `_validate_official_openai_input_content_types` 只在官方端点检查。走自定义 `base_url` 的兼容端点不做这个检查，因为那些端点可能支持 Responses 的内容类型。这是「按端点身份决定校验强度」的做法。

**源码事实：** `MCPToolChoice` 在 Chat Completions 里直接 `UserError`，不做降级。所以这一个不支持项是硬错，不是因为「无法等价表达」，而是因为按 MCP 工具名强制调用在这个协议里没有对应形状。

**源码事实：** thinking blocks 的两条落地路径对应两代存储格式：字段形式（完整、含 redacted）与内容内联形式（归一化、丢信息）。代码同时支持，是因为历史消息里两种都存在。

**架构推论：** 转换器的复杂度主要来自「聚合」而非「映射」。单条输入项转成单条消息是简单的；但 Chat Completions 要求助手消息内的多个 tool_calls 聚合在一条消息里，而 Responses 把每个调用当独立项。这个粒度差异决定了必须维护一个「正在构造」的中间状态，并小心 flush 时机——flush 早了会拆散一次并行的多工具调用，flush 晚了会把不同轮次混在一条消息里。

**架构推论：** 三个 `_handle_unsupported_*` 的模式（严格抛错、宽松警告一次）是把「协议能力差异」变成**可配置的失败模式**。这对迁移路径很重要：先在宽松模式下跑通、观察哪些功能被忽略，确认无影响后再开严格模式，比一次性硬切安全得多。

**架构推论：** `items_to_messages` 的 docstring 把规则列成了六条映射（EasyInputMessage 按角色分流、assistant 消息起或 flush、tool call 附加到当前助手消息、tool output 单独成消息）。这种「把 if-else 摊成规则清单」的文档习惯，在转换逻辑这种天然分支密集的代码里比注释每个分支更有用。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/models/openai_chatcompletions.py` | `OpenAIChatCompletionsModel` | Chat Completions 的 `Model` 实现 |
| `src/agents/models/openai_chatcompletions.py` | `OpenAIChatCompletionsModel._handle_unsupported_prompt` | 可复用提示的降级：严格抛错否则警告一次 |
| `src/agents/models/openai_chatcompletions.py` | `OpenAIChatCompletionsModel._handle_unsupported_reasoning_settings` | 推理 mode/context 的降级 |
| `src/agents/models/openai_chatcompletions.py` | `OpenAIChatCompletionsModel._validate_official_openai_input_content_types` | 官方端点的内容类型白名单检查 |
| `src/agents/models/openai_chatcompletions.py` | `OpenAIChatCompletionsModel._attach_stream_request_id` | 从响应头取 `x-request-id` 挂到结果上 |
| `src/agents/models/openai_chatcompletions.py` | `_tools_require_non_reasoning_mode` | 特定模型带工具时禁用推理 |
| `src/agents/models/chatcmpl_converter.py` | `Converter` | 全部格式翻译的静态方法集合 |
| `src/agents/models/chatcmpl_converter.py` | `Converter.items_to_messages` | 输入项折成消息列表，含助手消息聚合与 flush |
| `src/agents/models/chatcmpl_converter.py` | `Converter.message_to_output_items` | 助手消息转输出项，含三种推理表示的优先级 |
| `src/agents/models/chatcmpl_converter.py` | `Converter.convert_tool_choice` | 工具选择转换，含后端支持性校验 |
| `src/agents/models/chatcmpl_converter.py` | `Converter.convert_response_format` | 输出 schema 包装成 Chat Completions 的嵌套形状 |
| `src/agents/models/chatcmpl_converter.py` | `Converter._normalize_input_content_part_alias` | 内容块类型别名归一 |

## 参考

- [openai-agents-python 源码：models/chatcmpl_converter.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/models/chatcmpl_converter.py)
- [OpenAI 文档：Chat Completions API](https://platform.openai.com/docs/api-reference/chat)
- [Agents SDK 文档：Models](https://openai.github.io/openai-agents-python/models/)

## 与主核 / AI 的关系

主核里也有同类工作：同一个查询要能被不同版本的协议理解，于是一个编码器要处理「能力更弱的对端」——该省的字段省掉、该合并的结构合并。Chat Completions 路径做的正是这件事，而且它比主核更麻烦的地方在于：**对端是模型的输入接口，字段的增删会直接改变模型行为**，而不只是改变传输。

与 Responses 实现对比最能看出收敛代价。同一个接口下，Responses 侧实现主要在「表达能力」（工具分组、托管工具、websocket），Chat Completions 侧主要在「承认缺失」（三个 unsupported 处理加一个内容白名单）。所以选哪个模型实现不只是性能问题，还决定了你要处理多少降级路径——想要 prompt 模板与服务端会话，就必须用 Responses。

对 AI 工程最实用的一条是那个「警告一次」的降级模式。做跨 provider 的框架时，无法支撑全部能力的 provider 一定会出现；默认硬失败会让用户跑不起来，默认静默会让用户以为功能生效了。折中是「默认警告、可选严格」，并保证同一警告只出现一次。这套模式值得直接抄。

另一条是助手消息聚合里的那个 `del tool_calls` 修补。它说明「空集合」在协议层常常与「字段不存在」不等价。写任何协议适配层时，遇到列表字段都要问一句「空列表合法吗」——合法就不动，不合法就必须删键，而不是发一个空数组赌对端宽容。
