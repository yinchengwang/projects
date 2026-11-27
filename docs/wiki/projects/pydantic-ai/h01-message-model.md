---
project: pydantic-ai
feature: message-model
title: 消息部件与多模态内容
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/messages.py
    symbols: [SystemPromptPart, TextContent, ImageUrl]
---

# 消息部件与多模态内容

## 一句话职责

把「一次模型交互的历史」表示成两层结构：两条消息（`ModelRequest` / `ModelResponse`）各自持有有序的部件列表，每个部件带 `kind` 判别字段；多模态内容与用户文本共享同一套「内容项」类型，并各自携带自己的来源语义。

## 机制是什么

消息只有两种，用 `kind` 判别：

- `ModelRequest`：应用发给模型的消息，`parts` 是 `ModelRequestPart` 序列，另有 `timestamp`、`instructions`、`run_id`。
- `ModelResponse`：模型返回的消息，`parts` 是 `ModelResponsePart` 序列，另有 `usage`（单个请求的 `RequestUsage`）、`model_name`、`provider_name`、`provider_url`、`state`。

请求侧的部件：`SystemPromptPart`、`UserPromptPart`、`InstructionPart`、`ToolReturnPart`、`RetryPromptPart` 等。响应侧的部件：`TextPart`、`ThinkingPart`、`ToolCallPart`、`NativeToolCallPart` 等。

用户内容（`UserContent`）是另一层联合：`str | StructuredContent | TextContent | MultiModalContent | CachePoint`。也就是说用户提示里的每一项既可以是裸字符串，也可以是带元数据的 `TextContent`，或者多模态内容，或者一个**缓存断点标记**。

`ImageUrl` 属于 `FileUrl` 家族（`ImageUrl` / `AudioUrl` / `VideoUrl` / `DocumentUrl`），它们共同持有 `url`、`force_download`、`vendor_metadata` 与可选的媒体类型。

## 怎么实现的

**`SystemPromptPart` 的四个行为开关都写在字段文档里，值得逐条记住。**

- `timestamp`：每次出现都带时间。
- `dynamic_ref`：动态系统提示函数的 ref，仅当系统提示是动态生成时才有值。
- `context_id`：这条提示以哪个 `<pyai:context>` 元素的 id 发给模型，`None` 表示内容原样发送。由注册时声明 `on_change='append'` 的动态系统提示设置，形如 `'system_prompt:<name>'`。**模型能看到这个 id**：对话里第一个带该 id 的部件开启元素，后续同 id 部件作为它的「替换」发送，空内容即撤回；模型还会被告知一次「后面同 id 的元素会替换前面的」。
- `scope`：`'conversation'`（默认）从它所在的历史位置起对后续整个对话可见；`'request'` 只在它所属的那一次模型请求里可见——下一次请求（哪怕同一运行的下一步）就不再显示。文档给出使用边界：**变化的值（例如剩余预算）用 `'request'`；固定的文本属于 instructions；模型在下一请求之后仍必须遵守的注记必须用默认的 `'conversation'`**（比如「用户看不到工具输出」这类说明）。

`scope='request'` 的部件仍以普通部件形式留在历史里，只是渲染时被排除。这解释了为什么「历史里能看到但模型没看到」是一种正常状态。

**`TextContent` 是「给程序看的字符串」。** 它比裸 `str` 多一个 `metadata` 字段，文档点明两点：`metadata` 不发给模型；`ModelMessagesTypeAdapter` 会保留它，但作为应用侧数据它不保证在 UI 适配器往返后仍存在；`sanitize_messages` 会**从用户提示里丢掉它**——理由是「能设置它的客户端就能把自己的文本伪装成应用添加的内容」。这条安全考量把 `metadata` 的信任边界钉在了「应用自己写的」范围内。

**`UserPromptPart.content` 的构造期校验防的是一类静默事故。** `__post_init__` 做了两层检查：不是 `str` 也不是 `Sequence`（并显式排除 `bytes`/`bytearray`，因为它们是 `int` 的序列）就 `ValueError`；逐个检查元素是否属于 `_USER_CONTENT_TYPES`。注释把理由写得很具体：**每个模型的映射器都会遍历 content 并对每项做穷尽 match，不属于 `UserContent` 的东西最终表现为一个「本该不可达」的 `AssertionError`；而可迭代但不是序列的东西（尤其是 `dict`）会被当成键序列遍历，从而把键名当提示静默发给模型**。放在构造期校验，比在 N 个映射器里各校验一次成本低。异常类型选 `ValueError` 而非 `UserError` 也有原因：`__post_init__` 在反序列化历史时也会跑，那里 `ValueError` 会被转成带位置信息的 `ValidationError`。

错误消息里还带一句可操作的提示：自己先序列化，用 Pydantic 或 `format_as_xml()`。这说明「随手传一个 dict」是最常见的误用。

**`FileUrl.force_download` 是一个三态安全旋钮，而不是布尔。** 文档把三种值讲清了：`False` 时 URL 直接发给支持直链的供应商，不支持的供应商才走带 SSRF 保护的下载；`True` 时**总是**下载并启用 SSRF 保护（拦私网与云元数据）；`'allow-local'` 时总是下载，但允许私网地址、仍拦云元数据。三态的存在说明「是否允许本机/内网访问」与「是否强制本地下载」是两个独立决定，不能用同一个布尔表达。

**每个 URL 类型自己推断媒体类型。** `ImageUrl._infer_media_type` 从 URL 猜，猜不出就抛 `ValueError` 并要求显式给 `media_type`。`format` 属性返回 `ImageFormat`，注释说明可选集合是按 Bedrock Converse API 定的——即「供应商的接受面」反过来定义了框架的类型枚举。

**`CachePoint` 作为用户内容的一员出现。** 它不携带内容，只是标记「这里是一个可缓存断点」，让应用在用户提示内部指定缓存边界。它出现在 `UserContent` 而不是请求侧独立字段，是因为断点的位置由内容序列决定。

**消息构建会「提升」部件类型。** `ModelRequest.parts` 与 `ModelResponse.parts` 的文档都写：构建消息时会把每个 `tool_kind` 有对应类型化子类的基础工具部件，替换成那个子类。同时特别说明：传入的列表是**原地更新而不是复制**，消息继续持有那个列表本身，所以之后改这个列表也就改了消息。这两条并存意味着「构造即定型」且「引用即共享」。

**`usage` 字段的类型边界。** `ModelResponse.usage` 的类型是 `RequestUsage`（单次请求），文档明确**不应该**把 `RunUsage` 赋给它——运行级聚合在 `result.usage` 上。它还有默认值，原因也写了：方便测试，以及支持加载缺这个字段的旧历史。

**`Authority` 是三值的来源分级。** 定义在消息层：`'operator'`（应用自己的，例如它的 instructions 或它控制的文件）、`'user'`（用户的，例如用户保存的笔记或上传物）、`'none'`（数据，例如工具输出、别的模型的文本、别人写的文件）。它不决定内容格式，只决定**这段内容以谁的口气到达模型**——应用语气走 system 消息、用户语气进用户轮并标注为应用代附、数据则被框成数据。`SystemPromptPart` 与 `CapabilityPart` 都读这个分级。

## 关键取舍

**源码事实：** 消息只有两种类型，所有差异都在部件层。这让「历史是一个消息序列」这个模型极其简单，代价是部件类型数量大（请求侧与响应侧各十几个），且每种都要在**每个**模型映射器里处理一遍。

**源码事实：** `SystemPromptPart.scope='request'` 的部件仍在历史里但下一请求不渲染。历史与应用可见性的这种错位是刻意设计，目的在让「每次都发送、每次都变」的值（剩余预算之类）不必进缓存前缀。

**源码事实：** `Sanitize` 会从用户提示里丢掉 `TextContent.metadata`，理由是客户端可设置它即可伪造应用内容。这是把「应用侧数据」与「用户可控数据」分开的边界处置。

**源码事实：** `UserPromptPart` 在构造期就校验内容类型，且用 `ValueError`（以便反序列化时变成带位置的 `ValidationError`）。防的是 `dict` 被当键序列遍历后静默当提示发给模型这类事故。

**源码事实：** `FileUrl.force_download` 是三态而非布尔。这承认了「是否下载」与「是否允许内网」是两个独立决定。

**源码事实：** 构建消息时部件类型会被提升，且传入列表被原地持有。任何依赖「我传进去的列表不会被改」或「我后面改消息 parts 不会生效」的假设都不成立。

**架构推论：** 这套消息模型的强项是**可穷尽匹配**：每个映射器都能对部件类型做 `match` 并 `assert_never`，新增部件类型会在类型检查与运行期都暴露漏处理的适配器。代价是这个面很大——加一个部件类型意味着要动所有供应商映射器、所有 UI 适配器、所有序列化路径。因此「要不要新增部件类型」在这个框架里是一个重决策，而多模态内容被压进 `UserContent` 联合而不是新增用户部件类型，正是为了控制这个面的增长。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/messages.py` | `SystemPromptPart` | 系统提示部件，含 `context_id` 与 `scope` 两个行为开关 |
| `pydantic_ai_slim/pydantic_ai/messages.py` | `TextContent` | 带应用侧元数据的文本，`metadata` 不进模型且会被 `sanitize` 丢弃 |
| `pydantic_ai_slim/pydantic_ai/messages.py` | `ImageUrl` | 图片 URL，媒体类型可从 URL 推断，格式枚举按 Bedrock 定 |
| `pydantic_ai_slim/pydantic_ai/messages.py` | `FileUrl` | URL 类文件基类，`force_download` 三态控制下载与 SSRF 策略 |
| `pydantic_ai_slim/pydantic_ai/messages.py` | `BinaryContent` | 内联二进制内容，`.base64` 提供编码形式 |
| `pydantic_ai_slim/pydantic_ai/messages.py` | `CachePoint` | 用户内容里的缓存断点标记 |
| `pydantic_ai_slim/pydantic_ai/messages.py` | `UserContent` | 用户内容联合：str、带元数据文本、多模态、缓存断点 |
| `pydantic_ai_slim/pydantic_ai/messages.py` | `UserPromptPart.__post_init__` | 内容类型构造期校验，防 `dict` 被当键序列静默发送 |
| `pydantic_ai_slim/pydantic_ai/messages.py` | `ModelRequest` | 请求消息，持有 `ModelRequestPart` 序列 |
| `pydantic_ai_slim/pydantic_ai/messages.py` | `ModelResponse` | 响应消息，多条运行期元数据，`usage` 只放 `RequestUsage` |
| `pydantic_ai_slim/pydantic_ai/messages.py` | `Authority` | 三值来源分级：operator、user、none |
| `pydantic_ai_slim/pydantic_ai/messages.py` | `ModelRequestAttempt` | 未产出响应的尝试记录，回退与重试共用同一清单 |

## 参考

- [pydantic-ai 源码：messages.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/messages.py)
- [Pydantic AI 文档：消息历史](https://ai.pydantic.dev/message-history/)
- [Pydantic AI 文档：输入与多模态内容](https://ai.pydantic.dev/input/)

## 与主核 / AI 的关系

消息层与主核的「行格式 + 列编码」有一处结构呼应：消息是容器，部件是载体，判别字段决定解释方式。主核的行格式也靠一个类型标签决定后续字节怎么读，多模态内容则类似「同一列可以有多种编码」的情形。

对 agent 工程的直接价值有三条。其一，**多模态内容的 URL 下载策略是有安全含义的**。`force_download` 的第三态 `'allow-local'` 让应用能明确表达「这个文件来自内网是预期的」，而不是把它和「随便抓取」混为一谈。其二，**用户提示里的 `metadata` 不可信**，框架在 `sanitize` 时会丢掉它，任何依赖它做权限或标注判断的实现都会在净化后失效。其三，**给用户提示传结构化对象前必须自己序列化**。把 `dict` 传给 `content` 会被当作键序列遍历，闹出的不是报错而是「模型收到了几个字段名当提示」这种极难定位的问题。
