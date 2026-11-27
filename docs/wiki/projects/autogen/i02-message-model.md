---
project: autogen
feature: message-model
title: 消息类型与事件模型
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/messages.py
    symbols: [BaseChatMessage, TextMessage, to_model_message]
---

# 消息类型与事件模型

## 一句话职责

用一棵 pydantic 类树把「智能体之间说的话」与「智能体做过的事」彻底分开：前者可被转换成语模型消息并参与对话，后者只用于观察和判定，永不进入其他智能体的上下文。

## 机制是什么

根是 `BaseMessage`（`BaseModel` + `ABC`），它只声明一件事：`to_text()`——渲染成给人看的字符串；另外提供基于 `model_dump(mode="json")` / `model_validate` 的 `dump` / `load` 用于 JSON 往返。

往下分成两支：

- `BaseChatMessage`：智能体之间的对话消息。公共字段是 `id`（默认 UUID）、`source`（发送者名字）、`models_usage`（可选 `RequestUsage`）、`metadata`（字符串字典）、`created_at`（UTC 时间）。抽象方法是 `to_model_text()`（转成纯文本片段）与 `to_model_message()`（转换成 `UserMessage`）。它有一个中间子类 `BaseTextChatMessage`，内容类型固定为 `str`，把三个转换方法都实现好了。
- `BaseAgentEvent`：发给用户与应用观察的事件。字段与上面同形（`id`、`source`、`models_usage`、`metadata`、`created_at`），但**没有** `to_model_message`，也没有任何转成模型消息的路径。

`BaseChatMessage` 的具体子类有五个：`TextMessage`、`MultiModalMessage`（内容为 `str | Image` 的列表）、`StopMessage`、`HandoffMessage`（多一个 `target` 与 `context`）、`ToolCallSummaryMessage`（多 `tool_calls` 与 `results`）。此外还有一个泛型 `StructuredMessage[T]`，内容为任意 pydantic 模型。

`BaseAgentEvent` 的具体子类有九个：`ToolCallRequestEvent`、`ToolCallExecutionEvent`、`CodeGenerationEvent`、`CodeExecutionEvent`、`MemoryQueryEvent`、`UserInputRequestedEvent`、`ModelClientStreamingChunkEvent`、`ThoughtEvent`、`SelectSpeakerEvent`。另有一个 `SelectorEvent` 也是 `BaseAgentEvent` 的子类，但它不在 `AgentEvent` 联合里、也未登记进 `MessageFactory`——选人过程中的流式摘要走的是直接塞队列而非消息通道。

## 怎么实现的

**类型标签与判别式联合。** 每个具体类都带一个 `type` 字段（`Literal["TextMessage"]` 之类），`StructuredMessage` 则用 `@computed_field` 返回类名。两个 `Annotated` 联合类型把这些标签绑成判别式联合：`ChatMessage` 与 `AgentEvent`，用 `Field(discriminator="type")`。源码注释明确写了 `ChatMessage` **不包含** `StructuredMessage` 类型——因为后者的标签是动态类名，无法静态枚举。

**`TextMessage` 的三行实现。** 它继承 `BaseTextChatMessage`，后者把 `to_text`、`to_model_text` 都写成 `return self.content`，`to_model_message` 写成 `UserMessage(content=self.content, source=self.source)`。所以「一条文本消息」在模型侧的身份永远是**用户消息**——模型的回复由 `AssistantMessage` 表示，而那是由智能体在写上下文时自己构造的，不来自任何聊天消息。

**`HandoffMessage` 是唯一带结构化载荷的聊天消息。** `context: List[LLMMessage]` 让交接可以把「前任的工具调用与结果」原样带过去。它的 `to_model_message` 继承自 `BaseTextChatMessage`，只把 `content` 转成 `UserMessage`；`context` 里的消息**不会**被 `to_model_message` 带出，必须由接手的智能体在 `_add_messages_to_context` 里显式展开。这是两条不同的路径，混淆会导致上下文缺失。

**`MultiModalMessage` 的三套渲染。** `to_model_text(image_placeholder="[image]")` 把图片换成占位符（传 `None` 则换成 base64）；`to_text(iterm=False)` 把图片渲染成 `<image>`（`iterm=True` 时用 iTerm2 的内联图片转义序列）；`to_model_message` 则保留原始 `List[str | Image]` 交给模型客户端。三处口径不同，是有意为之：给模型看结构、给人看占位、给终端看图片。

**`StructuredMessage` 的格式化。** 它带一个可选的 `format_string`；有则 `to_text` 与 `to_model_text` 都用 `format_string.format(**content.model_dump())`，没有则用 `content.model_dump_json()`。`to_model_message` 无论有没有 `format_string` 都发 `model_dump_json()`——即**模型看到的永远是 JSON，格式化只影响人看到的**。

**`StructuredMessageFactory` 解决动态类型。** 因为 `StructuredMessage[X]` 的类型参数在反序列化时无法从标签恢复，工厂把 `ContentModel` 的 JSON schema 存进配置（`StructureMessageConfig` 含 `json_schema`、`format_string`、`content_model_name`），恢复时用 `schema_to_pydantic_model` 动态重建模型类，再 `StructuredMessage[self.ContentModel]` 生成具体类。这使结构化消息能进出组件配置系统，代价是重建出的类与原类是两个不同的 Python 类型（名字相同）。

**`MessageFactory` 的登记表。** 构造时把十四个内置类型按类名塞进字典；`register` 校验「未注册过」且「是 `BaseChatMessage` 或 `BaseAgentEvent` 的子类」；`create(data)` 从 `data["type"]` 查类名再调 `message_class.load(data)`。查不到就 `ValueError`。这是团队与容器在恢复状态时重建消息的唯一入口，因此**自定义消息类型必须显式注册**，否则恢复会失败。

**`ToolCallSummaryMessage` 携带原始结构。** 它既是文本（`content` 是拼接后的摘要），又带 `tool_calls` 与 `results` 两个列表。这让下游既能读自然语言摘要，也能拿到结构化调用记录，不必解析文本。

**事件类里有两个特殊的。** `ModelClientStreamingChunkEvent` 带 `full_message_id`，用于把流式碎片与最终消息关联（`AssistantAgent` 生成 `message_id` 并传入）；`UserInputRequestedEvent` 的 `content` 被固定为 `Literal[""]`，注释写明是为了兼容「期待有 content 字段的消费者」。

## 关键取舍

**源码事实：** 「聊天消息 vs 事件」的区分是靠**类型继承**而不是靠字段标记。这带来一个强约束：事件无法被转换成模型消息，因为根本没有这个方法。于是「事件永不进入模型上下文」不是靠调用方自觉，而是类型系统层面做不到。

**源码事实：** 所有 `BaseChatMessage` 转模型消息都只能产出 `UserMessage`。模型自己的历史回复必须由智能体手工构造 `AssistantMessage` 写入上下文（`AssistantAgent` 就是这么做的）。这个不对称设计的后果是：聊天消息是无「角色」的，角色由谁在什么位置写进上下文决定。

**源码事实：** `metadata` 在所有类上的默认值是可变对象 `{}`。pydantic 在模型字段上会做拷贝，所以不会跨实例共享，但源码写法本身值得注意。

**源码事实：** `StructuredMessage` 被排除在 `ChatMessage` 联合类型之外。源码注释直说原因：它的标签是动态类名，无法用 `Literal` 判别。因此任何依赖 `ChatMessage` 联合做静态校验的地方（例如团队任务的类型检查）都只能用基类 `BaseChatMessage`。

**源码事实：** `MessageFactory.create` 里有一句冗余校验：先判断 `message_type not in self._message_types`，之后才判断 `not isinstance(message_type, str)`。顺序上如果 `type` 不是字符串，前一个判断会因为不可哈希类型而抛 `TypeError` 而不是预期的 `ValueError`（字典成员检查需要可哈希）。

**架构推论：** 把「过程」与「对话」分成两棵树，等价于给系统装了两条独立的信息通道：一条进模型上下文（昂贵、有 token 成本、会影响后续推理），一条只进日志与终止条件判定（廉价、可丢弃）。所有关于「可观测性」的设计都建立在这个分叉上。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-agentchat/src/autogen_agentchat/messages.py` | `BaseMessage` | 共同根：`to_text` 与 JSON 往返 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/messages.py` | `BaseChatMessage` | 对话消息契约：来源、用量、转模型消息 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/messages.py` | `BaseAgentEvent` | 事件契约：无转模型消息的路径 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/messages.py` | `TextMessage` | 最常用消息，纯字符串内容 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/messages.py` | `to_model_message` | 统一转换为 `UserMessage`，角色由调用方补 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/messages.py` | `HandoffMessage` | 带 `target` 与 `context` 的交接消息 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/messages.py` | `StructuredMessage` | 泛型结构化消息，格式化只影响人类可读输出 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/messages.py` | `StructuredMessageFactory` | 用 JSON schema 动态重建内容模型，支撑配置往返 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/messages.py` | `MessageFactory` | 按 `type` 字段重建消息，状态恢复的唯一入口 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/messages.py` | `ChatMessage` / `AgentEvent` | 判别式联合类型，不含结构化消息 |

## 参考

- [AutoGen 源码：messages.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-agentchat/src/autogen_agentchat/messages.py)
- [AutoGen 文档：消息与事件](https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/messages.html)
- [AutoGen 文档：Core 消息类型](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/framework/message-and-communication.html)

## 与主核 / AI 的关系

消息与事件的分野，对应主核里「数据」与「日志」的分野：数据参与后续计算，日志只用于审计与恢复。主核的日志还能参与恢复重放，这里的事件则完全不参与推理——它只被 UI 展示、被终止条件扫描、被容器转发。

对 AI 工程最实用的三条推论。其一，**「模型看到什么」由 `to_model_message` 与上下文写入逻辑共同决定，不由消息本身决定**。改一个消息类的内容渲染，未必会改变模型看到的东西；反之，也可能改变人看到的东西。其二，**`StructuredMessage` 的 `to_model_message` 固定发 JSON**，所以给它配 `format_string` 只影响展示，模型仍然读 JSON——若希望模型读自然语言，得让智能体在写上下文时手工转换。其三，**跨进程/跨重启恢复消息依赖 `MessageFactory` 的登记表**：自定义消息类型若没在团队构造时通过 `custom_message_types` 注册，`load_state` 会在重建阶段直接报错，而不是降级成原始字典。
