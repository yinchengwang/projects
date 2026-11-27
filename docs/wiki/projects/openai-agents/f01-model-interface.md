---
project: openai-agents
feature: model-interface
title: 模型接口与追踪开关
page_type: feature
schema_version: 1
refs:
  - path: src/agents/models/interface.py
    symbols: [Model, ModelProvider, ModelTracing]
---

# 模型接口与追踪开关

## 一句话职责

定义 `Model` 与 `ModelProvider` 两个抽象：前者把「一次模型调用」的输入输出形状固定下来，后者把「按名字取模型」固定下来；同时用 `ModelTracing` 三态枚举把「这次调用能不能记录数据」变成调用参数而不是全局开关。

## 机制是什么

`Model` 只有两个抽象方法：`get_response` 与 `stream_response`。两者的参数表逐字相同，差别只在返回：前者返回完整的 `ModelResponse`，后者返回 `AsyncIterator` 的流事件。参数共九个——系统指令、输入项、模型设置、工具、输出 schema、交接列表、追踪配置，以及三个仅关键字参数 `previous_response_id`、`conversation_id`、`prompt`。

这套参数表的取舍值得看清：输入统一成 **Responses 格式的 `TResponseInputItem` 列表**，而不是各家自己的消息格式。所以接口层就做了归一——任何 provider 实现都必须能把 Responses 输入转成自己的协议，而不是让调用方按 provider 分支。

`ModelProvider` 只有一个抽象方法 `get_model(model_name) -> Model`。名字暗示它是工厂：拿到字符串，回一个能跑的模型。`aclose()` 有默认空实现，注释说「缓存了持久模型或网络连接的 provider 可以覆盖」，所以生命周期回收是**可选**的。

`ModelTracing` 是三个值的枚举：`DISABLED`、`ENABLED`、`ENABLED_WITHOUT_DATA`。两个辅助方法把它翻译成布尔：`is_disabled()` 判断是否关闭，`include_data()` 判断是否**只有** `ENABLED` 才为真。所以三态的语义是「关 / 开且带数据 / 开但不带数据」——中间那档让追踪能记录时间与结构，但不记录消息内容。

除了这两个抽象方法，`Model` 还有五个有默认实现的可选钩子：`get_default_model_settings`（按 `self.model` 字符串查默认设置表）、`_get_request_settings`（在运行器解析完显式覆盖后调整推断字段）、`_cleanup_on_run_end`（运行结束时释放运行期资源）、`close`、`get_retry_advice`（provider 特定的重试建议）。

## 怎么实现的

**追踪三态的使用方式。** 接口不规定实现怎么用 `ModelTracing`，但从两个实现的写法能反推契约：span 的 `disabled` 取自 `tracing.is_disabled()`，而敏感数据（请求体、响应体）写进 span 与否取自 `tracing.include_data()`。所以 `ENABLED_WITHOUT_DATA` 的效果是「有 span、有时间、有 token 用量，但没有内容」。

**三个仅关键字参数的分工。** `previous_response_id` 与 `conversation_id` 是**服务端会话**的两个句柄：前者指向上一次响应，后者指向服务端存的会话。接口注释写得很克制——「一般不由模型使用，除了 OpenAI Responses API」，意思是绝大多数 provider 会忽略这两个参数。`prompt` 是「可复用的提示模板」，同样主要服务 Responses。

把它们做成关键字参数而非位置参数，是有意的：三个都是「某一家 provider 的特有能力」，强制显式命名能让调用方意识到自己在用非通用功能。

**`get_default_model_settings` 的推导。** 默认实现取 `getattr(self, "model", None)`，若是字符串就查 `default_models` 里的表。注释指出了边界：「带命名空间标识的适配器可以用自己的规范模型名覆盖这个方法」——因为 `self.model` 可能是 `"litellm/openai/gpt-4.1"` 这种带前缀的串，直接查表查不到。

**`_get_request_settings` 的位置很关键。** 它在**运行器解析完显式覆盖之后**被调用，参数里带 `implicit_fields`（哪些字段是推断出来的，而非用户显式给的）。两个实现都用它做同一件事：把「推断出来的、但目标 API 不支持的字段」丢掉。这是把「哪些设置该发」的判断放在模型侧而不是运行器侧，因为只有模型知道自己的 API 支持什么。

**`_cleanup_on_run_end(owner)` 与 `close()` 的区别。** 前者在**每次运行结束**时调用，参数是持有模型的那个对象（运行器用它做归属检查）；后者是**进程级**的资源释放。所以持久连接（如 websocket）应该在 `close` 里关，而「这次运行申请的临时资源」在 `_cleanup_on_run_end` 里放。运行器在收尾时遍历所有用到的模型调前者。

**`get_retry_advice` 的形状。** 入参是 `ModelRetryAdviceRequest`，返回 `ModelRetryAdvice | None`。接口注释列的用途是「重放安全、retry-after 延迟、服务端显式重试指引」三类。这解释了为什么它挂在模型上而不是运行器上：这三类信息只有 provider 适配器读得到（HTTP 头、错误码语义、请求是否幂等）。

**docstring 里那条硬约束。** `Model` 的类注释不是介绍，而是一条协议要求：「每次工具调用必须分配非空的 call ID，它必须在整次运行及其序列化续跑谱系内标识一次规范调用，不得因工具身份或载荷变化而复用；运行期可以省略完全一致的已完成重放；工具输出必须保留 call ID 以便关联」。这条约束解释了整套去重与续跑机制（见 g03）为什么能成立——它依赖 provider 适配器给出稳定 id。

## 关键取舍

**源码事实：** 输入类型是 Responses 的 `TResponseInputItem`，而不是抽象的消息类型。这使接口直接依赖 OpenAI SDK 的类型，第三方 provider 必须做格式转换（Chat Completions 实现里那 1000 多行的 `items_to_messages` 就是代价）。

**源码事实：** `stream_response` 返回的是 SDK 的 `TResponseStreamEvent`，不是本项目自己的事件类型。所以「原始事件」在接口层就已经是 OpenAI 的协议形状，项目自己的 `RawResponsesStreamEvent` 只是给它套了一层（见 j01）。

**源码事实：** `get_response` 与 `stream_response` 都是 `@abc.abstractmethod`，但方法体是 `pass` 而非 `raise NotImplementedError`。所以子类若忘记实现，实例化时会因抽象方法未实现而报错——`pass` 只是占位。

**源码事实：** `ModelTracing` 用整数 0/1/2 作值而不是字符串。这让它适合按序比较的场景（「至少记录了结构」），但两个辅助方法都是等值判断，没有用上顺序。

**架构推论：** 把「追踪是否记录数据」做成**调用参数**而不是全局配置，是为了支持同一次运行里不同调用有不同策略。实际收益体现在敏感数据边界上：某个调用要发含 PII 的 prompt 时，可以只把这次降级成 `ENABLED_WITHOUT_DATA`，其余调用照常记录内容。

**架构推论：** 五个可选钩子里有三个（`get_default_model_settings`、`_get_request_settings`、`get_retry_advice`）都是「让 provider 声明自己的特性」，而不是「让调用方传更多参数」。这种设计让接口参数表保持稳定——新增 provider 能力不需要改接口签名，只需覆盖钩子。

**架构推论：** `previous_response_id` 与 `conversation_id` 出现在通用接口上，等于承认「服务端可能持有历史」。这与「客户端自己管历史」是两种模型，接口同时容纳两者：不支持的实现直接忽略这两个参数并抛或警告。Chat Completions 实现里那两个 `_handle_unsupported_*` 方法就是这条边界的显式落地——默认警告一次，开了严格校验则抛错。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/models/interface.py` | `Model` | 模型抽象：两个必须实现的调用方法加五个可选钩子 |
| `src/agents/models/interface.py` | `Model.get_response` | 非流式调用，九个参数的固定形状 |
| `src/agents/models/interface.py` | `Model.stream_response` | 流式调用，返回 SDK 流事件迭代器 |
| `src/agents/models/interface.py` | `Model.get_default_model_settings` | 按模型名查默认设置，供命名空间化标识覆盖 |
| `src/agents/models/interface.py` | `Model._get_request_settings` | 显式覆盖解析后修剪推断字段 |
| `src/agents/models/interface.py` | `Model._cleanup_on_run_end` | 单次运行结束时释放运行期资源 |
| `src/agents/models/interface.py` | `Model.get_retry_advice` | provider 特定重试建议 |
| `src/agents/models/interface.py` | `ModelProvider` | 按名字取模型的工厂抽象 |
| `src/agents/models/interface.py` | `ModelTracing` | 追踪三态枚举与两个判定方法 |

## 参考

- [openai-agents-python 源码：models/interface.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/models/interface.py)
- [Agents SDK 文档：Models](https://openai.github.io/openai-agents-python/models/)
- [OpenAI 文档：Responses API 的 input items](https://platform.openai.com/docs/api-reference/responses)

## 与主核 / AI 的关系

主核里「存储抽象」也是这个形状：接口固定一组方法，实现各自适配底层，能力差异用可选钩子表达（能不能回滚、能不能并行）。差别在于主核的接口参数是**位置化且数量稳定**的，模型接口却必须容纳「某一家独有的三个参数」。用仅关键字参数收留这类扩展，是比「加一个 `**kwargs` 逃生口」更克制的做法：调用方仍然看得见它们在签名里。

与 AutoGen 的 `ChatCompletionClient` 对比能看出两种不同的收敛点。AutoGen 把「数 token」「报用量」「声明能力」都做成必实现的方法，于是上层可以直接问「这个模型还剩多少 token」；这里的 `Model` 只负责调用，token 与能力放在 `Usage` 与 `ModelSettings` 里，数 token 的能力被下推给运行期需要的具体地方。前者的好处是上层的裁剪策略可以通用，后者的好处是接口窄、实现省事。

对 AI 工程最实用的一条是 `ModelTracing` 的三态设计。做合规敏感的产品时，常见需求是「要能统计调用耗时与错误率，但不能留消息内容」——如果追踪只有一个布尔开关，就只能二选一：要么放弃可观测性，要么冒险存内容。三态枚举把这条中间路线变成可表达的配置。

另一条是 `_get_request_settings` 的位置。它揭示了一个通用模式：**「哪些参数该发」应该由最了解目标协议的那一层决定**。放在调用方会导致每个调用点都要判断 provider，放在适配器里则只需实现一次。代价是适配器必须知道哪些字段是「用户显式给的」——这就是 `implicit_fields` 参数存在的原因。
