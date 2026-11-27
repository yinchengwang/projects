---
project: autogen
feature: model-client-contract
title: 模型客户端与能力声明
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/models/_model_client.py
    symbols: [ChatCompletionClient, ModelFamily, ModelInfo]
---

# 模型客户端与能力声明

## 一句话职责

用一份抽象基类固定「怎么问模型、怎么数 token、怎么报用量」，再用一个 capability 字典把「这个模型能做什么」变成运行期可查的事实，使上层编排不必按厂商写分支。

## 机制是什么

`ChatCompletionClient` 定义了两组契约：**调用**（`create`、`create_stream`、`close`）与**计量/能力**（`actual_usage`、`total_usage`、`count_tokens`、`remaining_tokens`、`model_info`）。前者是所有客户端必须实现的形状，后者是上层决策的依据。

参数表是整个契约的核心，值得逐个看：`messages` 是判别联合的 `LLMMessage` 列表；`tools` 接受 `Tool` 或已序列化的 `ToolSchema`；`tool_choice` 允许 `"auto"`、`"required"`、`"none"` 三个字符串，也允许直接传一个 `Tool` 实例以强制模型调用它；`json_output` 是三态——`None` 不用、`True` 走普通 JSON 模式、传一个 pydantic 模型类型则走结构化输出；`extra_create_args` 是给底层客户端的逃生口；`cancellation_token` 让调用可被取消。

返回值 `CreateResult` 是固定结构：`finish_reason`（受限于 `FinishReasons` 字面量联合）、`content`（`str` 或 `FunctionCall` 列表）、`usage`、`cached`、`logprobs`、`thought`。**`content` 的两分是整套工具调用机制的基础**：模型要么给出文本，要么给出一批函数调用，没有第三种形态。

能力声明侧，`ModelInfo` 是 `total=False` 的 `TypedDict`，但用 `Required` 标出五个必填字段：`vision`、`function_calling`、`json_output`、`family`、`structured_output`；另有可选字段 `multiple_system_messages`。`family` 取 `ModelFamily` 的常量或任意字符串。`ModelFamily` 是个**禁止实例化的命名空间类**（`__new__` 直接 `TypeError`），只有类常量与四个 `is_*` 判定函数。

`validate_model_info` 表明显式校验路径：四个字段缺失抛 `ValueError`（文案点明 v0.4.7 起强制），`structured_output` 缺失只发 `UserWarning`（预告未来会强制）。

## 怎么实现的

**抽象方法清单。** `create` 与 `create_stream` 都是 `@abstractmethod`；`create_stream` 是**普通方法**（返回 `AsyncGenerator`）而不是 `async def`——它返回生成器，调用方 `async for`。`close`、`actual_usage`、`total_usage`、`count_tokens`、`remaining_tokens` 全部 `@abstractmethod`。`capabilities` 是 `@property` 且 `@abstractmethod`，但类注释直接写「Deprecated」。

**`model_info` 的兜底实现。** 这一个方法**不是抽象方法**，而是带默认实现的 `@property`：先 `warnings.warn` 说「客户端没实现 model_info，回落到 capabilities，后者已弃用」，然后把 `capabilities` 的字典塞进 `base_info` 并补 `family = ModelFamily.UNKNOWN`，返回。这让老客户端不必立刻升级，代价是每次访问 `model_info` 都发一次警告。

**能力判定的形状。** `ModelFamily` 的五个 `is_*` 静态方法（`is_claude`、`is_gemini`、`is_openai`、`is_llama`、`is_mistral`）都是「成员是否落在硬编码元组里」。没有前缀匹配、没有版本泛化：`"gpt-4o-mini"` 不在 `is_openai` 的元组里，因此会被判为非 OpenAI 家族——除非客户端的 `model_info["family"]` 明确给出 `gpt-4o` 这类常量。这正是「family 是客户端声明的，不是从模型名猜的」的设计后果。

**类型别名与字面量。** `ModelFamily.ANY` 是一个 `TypeAlias` 形式的 `Literal[...]`，把全部常量列举一遍，用于 `ModelInfo["family"]` 的类型约束。它支持 `| str` 的宽化，所以类型检查允许任意字符串，同时保留常量提示。

**弃用的双轨。** `ModelCapabilities` 被 `@deprecated("Use the ModelInfo class instead ModelCapabilities.")` 装饰，但它仍是 `@property capabilities` 的类型。所以客户端要么实现新口（`model_info`）、要么实现旧口（`capabilities`），基类用一段会告警的适配代码把旧口翻译成新口。

**构造即缓存。** 类注释里有一句关键说明：**缓存必须在客户端内部处理**，因为缓存键取决于构造期存下的那些参数。所以基类不提供缓存钩子——缓存被设计成「包一层的另一个客户端」，而不是本层的功能。

## 关键取舍

**源码事实：** `model_info` 有默认实现而 `capabilities` 是抽象方法。也就是说新客户端**可以完全不实现 `model_info`**，继承来的兜底会读 `capabilities`——但 `capabilities` 又是抽象方法，所以无论如何都得实现一个。这套双轨的净效果是「过渡期两个都必须有」。

**源码事实：** 默认参数用可变对象（`tools: Sequence[...] = []`、`extra_create_args: Mapping[str, Any] = {}`）。因为类型是 `Sequence`/`Mapping` 而非 `list`/`dict`，实现方约定不得原地修改，所以共享默认值在实践上不构成问题——但这条约束是**约定而非强制**。

**源码事实：** `tool_choice` 的类型是 `Tool | Literal[...]`。允许传 `Tool` 实例意味着强制某次调用使用特定工具；这把「选择工具」的权利从模型手里部分收回给了调用方，且不需要额外的策略层。

**源码事实：** `CreateResult.usage` 类型是 `RequestUsage`，而 `RequestUsage` 是普通 `dataclass`（不是 pydantic 模型）。于是它不能直接进 pydantic 状态序列化，只能被包在其他模型里或单独处理。

**源码事实：** `validate_model_info` 只在**显式调用时**生效，基类不做自动校验。所以客户端可以返回一个缺字段的 `ModelInfo`，直到上层某处主动校验才暴露。这个函数属于「约定检查工具」，不属于拦截。

**架构推论：** `remaining_tokens` 与 `count_tokens` 被放在客户端而不是上下文里，意味着 token 计数规则**绑定在客户端实现上**。这对 `TokenLimitedChatCompletionContext` 是必要的前提（它在裁剪时反复问客户端），但也意味着同一批消息在不同客户端下有不同预算含义，跨客户端迁移上下文策略时必须重测。

**架构推论：** `finish_reason` 是受限字面量（`"stop"`、`"length"`、`"function_calls"`、`"content_filter"`、`"unknown"`）。上层若想区分「模型主动结束」与「被长度截断」，只需看这一个字段，不必解析内容。这是把厂商差异收敛到一处的最便宜做法。

**架构推论：** `cached` 字段在 `CreateResult` 上。这说明缓存命中是**结果的一个属性**而不是外层包装的行为——上层拿到结果后能判断这次是否真的花了钱，从而正确记账。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/models/_model_client.py` | `ChatCompletionClient` | 客户端抽象基类，固定调用与计量两组契约 |
| `python/packages/autogen-core/src/autogen_core/models/_model_client.py` | `ChatCompletionClient.create` | 单次调用，参数表定义工具、JSON、结构化输出三态 |
| `python/packages/autogen-core/src/autogen_core/models/_model_client.py` | `ChatCompletionClient.create_stream` | 流式调用，产出字符串块并以 `CreateResult` 收尾 |
| `python/packages/autogen-core/src/autogen_core/models/_model_client.py` | `ChatCompletionClient.count_tokens` | 消息加工具的 token 计数，裁剪策略依赖它 |
| `python/packages/autogen-core/src/autogen_core/models/_model_client.py` | `ChatCompletionClient.model_info` | 能力声明入口，默认回落到弃用的 `capabilities` 并告警 |
| `python/packages/autogen-core/src/autogen_core/models/_model_client.py` | `ModelFamily` | 模型家族常量命名空间，禁止实例化 |
| `python/packages/autogen-core/src/autogen_core/models/_model_client.py` | `ModelFamily.is_openai` | 家族判定，按硬编码元组成员匹配 |
| `python/packages/autogen-core/src/autogen_core/models/_model_client.py` | `ModelInfo` | 能力字典，五个必填字段 |
| `python/packages/autogen-core/src/autogen_core/models/_model_client.py` | `validate_model_info` | 显式校验，缺失字段按新旧分级 |
| `python/packages/autogen-core/src/autogen_core/models/_model_client.py` | `ModelCapabilities` | 已弃用的旧能力字典，仅作兼容 |
| `python/packages/autogen-core/src/autogen_core/models/_types.py` | `CreateResult` | 调用结果：结束原因、内容、用量、缓存标记 |

## 参考

- [AutoGen 源码：_model_client.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/models/_model_client.py)
- [AutoGen 文档：Model Clients](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/components/model-clients.html)
- [OpenAI 文档：Chat Completions API](https://platform.openai.com/docs/api-reference/chat)

## 与主核 / AI 的关系

主核的存储引擎也有一套「能力声明」：某个访问方法支持不支持并发、是不是有序、能不能回滚，全靠 `am` 接口约定与标志位。那里的能力是编译期固定的，一个不满足约定的实现会被上层逻辑直接踩坏。这里的 `ModelInfo` 是运行期声明的，而且**真假由客户端实现者自负**：`validate_model_info` 只查字段在不在，不查声明与实况是否一致。结果是 AI 工程里最常见的一类 bug——客户端声明 `function_calling: True` 却在实际调用时丢了工具定义，上层看到的是模型「不肯用工具」。

对编排层来说，`tool_choice` 与 `json_output` 两个参数是真正决定行为的分水岭。前者可以把「必须调用这个工具」写死在调用点；后者能把输出直接约束成 pydantic 模型，省掉一轮解析与重试。但这些能力都受 `ModelInfo` 里的对应布尔位限制——**如果编排层不检查这些位，就会把不支持的工具调用发给不支持的模型，得到一段模型自说自话的文本**。

需要留意的还有 `thought` 字段。它在 `CreateResult` 与 `AssistantMessage` 上各有一份，是给推理模型用的额外文本。基类不规定怎么用，只在上下文示例里演示「把 `thought` 置空以省 token」——这是推理模型时代新增的一类裁剪维度，与条数裁剪、token 裁剪并列但更语义化。

最后，`create_stream` 的收尾约定很值得记：流里前面的元素是 `str` 片段，**最后一个元素一定是完整的 `CreateResult`**。因此流式调用既能拿到增量体验，也能拿到完整结果用于落库——不需要调用方自己拼装。
