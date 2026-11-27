---
project: smolagents
feature: api-models
title: API 模型实现与限流重试
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/models.py
    symbols: [_prepare_completion_kwargs, generate]
---

# API 模型实现与限流重试

## 一句话职责

给所有走网络的模型提供共同的骨架：一套限流加指数退避重试、一个按需创建客户端的钩子、一组共享的请求参数拼装，子类只需实现 `create_client` 与 `generate` 两件事。

## 机制是什么

`ApiModel` 继承 `Model`，构造时做三件事：`super().__init__(model_id=...)` 把模型 id 交给基类、`self.client = client or self.create_client()`（惰性工厂，允许外部注入已配置的客户端）、以及装配两个控制器。

`RateLimiter(requests_per_minute)` 是一个最小限流器：`requests_per_minute` 为 `None` 时整个功能关闭（`_enabled=False`，`throttle()` 直接返回）；启用时算出 `_interval = 60.0 / requests_per_minute`，每次 `throttle()` 检查距上次调用的间隔，不足就 `time.sleep` 补齐，然后更新 `_last_call`。

`Retrying(...)` 是手写的重试器（docstring 说「受 tenacity 启发的简版」），参数为：最大尝试次数、初始等待、指数基数、是否抖动、重试谓词、是否重抛、以及两个日志钩子。在 `ApiModel` 里它的配置是：`max_attempts = RETRY_MAX_ATTEMPTS if retry else 1`、`retry_predicate=is_rate_limit_error`、`reraise=True`、两个 logger 都是 INFO 级。

`is_rate_limit_error` 的判定是**字符串匹配**：异常文本转小写后，含 `"429"`、`"rate limit"`、`"too many requests"`、`"rate_limit"` 任一即为真。

## 怎么实现的

**重试器的调用形状。** `Retrying.__call__(fn, *args, **kwargs)` 把任意可调用体包起来重试，所以调用点写成 `self.retryer(self.client.completion, **completion_kwargs)`——传的是方法与参数，而不是「先调再捕」。这个形状的代价是重试器必须自己记录开始时间、自己打印每次尝试的日志，而不能像装饰器那样依赖调用方的上下文。

循环体：`for attempt_number in range(1, max_attempts + 1)`，每次都先试 `fn(*args, **kwargs)`。成功时若已重试过（`attempt_number > 1`）就打一条 after 日志（含总耗时与「第几次尝试/共几次」），然后返回。

失败时先查 `should_retry = self.retry_predicate(e) if self.retry_predicate else False`——没给谓词就一律不重试。然后 `if not should_retry or attempt_number >= self.max_attempts: raise`。注意两个分支都 `raise`，区别只在 `reraise` 的真假——而这在当前实现里没有行为差异。这是「保留参数但暂未区分」的痕迹。

**退避计算。** `delay *= self.exponential_base * (1 + self.jitter * random.random())`。注释给了出处（OpenAI 的限流处理手册）。这里的乘法结构值得注意：`delay` 是**累乘**的，所以第 n 次的等待大致是 `wait_seconds * (base * (1 + jitter*rand))^n`。抖动的实现是「乘以 1 到 (1+jitter) 之间的随机因子」，只往上抖不往下抖；默认 `jitter=True` 时因子在 1 到 2 之间。

**日志的两个位置。** after logger 在两处被调用：成功且重试过时、以及失败且还要重试时。before sleep logger 只在失败且即将重试时调用。所以日志序列能拼出「第一次失败 → 等 X 秒 → 第二次失败 → 等 Y 秒 → 成功」的完整时间线。

`fn_name = getattr(fn, "__name__", repr(fn))` —— 传入的是 `self.client.completion` 这类绑定方法，`__name__` 通常存在；拿不到就退回 repr，避免日志里因为取不到名字而崩。

**`_prepare_completion_kwargs` 的覆盖在子类。** 基类版本已经能用，但 `LiteLLMModel.generate` 调它时传了额外参数：`model=self.model_id`、`**self._get_credentials_kwargs()`、`convert_images_to_image_urls=True`、`custom_role_conversions=self.custom_role_conversions`。所以子类负责把「本 provider 需要的东西」注入同一套合并逻辑。

`convert_images_to_image_urls=True` 在 API 路径上是必须的——HTTP API 不接受本地 PIL 对象，必须编成 `data:` URL。而本地推理模型（见 e03）走 `flatten_messages_as_text`，直接不带图。

**`generate` 的标准三段。** 以 `LiteLLMModel.generate` 为例：调 `_prepare_completion_kwargs` 拼参数 → `self._apply_rate_limit()` 限流 → `self.retryer(self.client.completion, **completion_kwargs)` 带重试调用。然后处理响应：

- `if not response.choices` 抛 `RuntimeError`，文案里带上 `response.model_dump()` 全文。注释说「可能表示 API 或上游问题」——这是把「HTTP 200 但载荷是错误对象」这类情况显式化。
- 取 `response.choices[0].message.content`；若 `stop_sequences` 有值且模型不支持 stop 参数，客户端截断。
- 组装 `ChatMessage`，`role` 直接用响应的 role，`tool_calls` 直接沿用，`raw=response` 保留原始响应，`token_usage` 从 `response.usage` 取（**用 `getattr(..., None) is not None` 判断而非直接访问**，因为有些兼容端点不回 usage）。

**流式路径的用量注入。** `generate_stream` 用 `stream=True, stream_options={"include_usage": True}` 请求流。注释没有解释这两个参数，但它们是配套的：OpenAI 兼容协议默认不在流里带 usage，必须显式要。循环里先判 `if getattr(event, "usage", None)` 并 yield 一个 `content=""` 只带用量的 delta，再处理 `event.choices`。

漏掉 usage 事件时的处理是**抛错**：`if not getattr(choice, "finish_reason", None): raise ValueError(f"No content or tool calls in event: {event}")`。所以「既没 delta 又没 finish_reason」被当作协议异常，而不是静默跳过。

**Tiny 细节：`role` 的原样传递。** 非流式路径里 `role=response.choices[0].message.role`——直接把厂商给的 role 字符串塞进 `MessageRole` 枚举字段。这依赖该字符串恰好是五值之一；若厂商返回别的东西，会在 dataclass 构造时失败。

## 关键取舍

**源码事实：** `is_rate_limit_error` 用字符串匹配而非异常类型。这覆盖了「不同 SDK 各有自己的 RateLimitError 类」这个现实，但也意味着任何文本里出现「429」的异常都会被当作限流——包括「429 是本次请求的 id」这类误判。

**源码事实：** `Retrying` 的 `reraise` 参数当前不产生行为差异（两个分支都 `raise`）。这是一个预留位而非有效配置。

**源码事实：** 限流是「同步 sleep」，不是排队或令牌桶。多线程共用同一个 `ApiModel` 时，`_last_call` 的读改写没有加锁，实际限流会略松于设定值。

**源码事实：** 退避的抖动只向上抖动（因子 1 到 2），所以实际等待总是 ≥ 理论值。这在「避免同时重试撞服务端」的目的上是对的，代价是重试慢于名义延迟。

**源码事实：** 非流式路径对空 `choices` 抛 `RuntimeError` 并把整个 `model_dump()` 拼进消息。这让错误自解释，但如果响应体很大（含长文本），异常消息会很长。

**架构推论：** 「限流 + 重试」放在 API 模型的基类而不是 agent 主循环，是因为它们只对网络调用有意义。本地推理没有限流概念（e03 的两个实现根本不构造这两个控制器），代码执行器也不需要。把这层放在最贴近网络的那一层，避免了在不需要的地方引入等待。

**架构推论：** 重试只针对限流错误（`retry_predicate` 只判 429 类），不针对超时或连接错误。这是一个有意的窄口径：限流是「稍后必然成功」的，而连接错误重试可能只是把同一个失败重复几次。代价是网络抖动导致的失败会直接冒到 agent 层，由 agent 的错误处理路径消化。

**架构推论：** 客户端由 `create_client` 惰性创建且允许外部注入，使测试与自定义端点配置都无需改代码。这比「构造函数里硬建客户端」灵活，也让「同一个客户端被多个模型实例共享」成为可能。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/models.py` | `ApiModel` | API 模型基类：客户端、限流器、重试器 |
| `src/smolagents/models.py` | `ApiModel.create_client` | 子类实现的客户端工厂钩子 |
| `src/smolagents/models.py` | `ApiModel._apply_rate_limit` | 调用前应用限流 |
| `src/smolagents/models.py` | `is_rate_limit_error` | 按异常文本判定限流错误 |
| `src/smolagents/models.py` | `Model._prepare_completion_kwargs` | 三层优先级参数合并 |
| `src/smolagents/models.py` | `LiteLLMModel.generate` | 标准三段流程与非流式响应解析 |
| `src/smolagents/models.py` | `LiteLLMModel.generate_stream` | 流式调用与用量事件注入 |
| `src/smolagents/utils.py` | `RateLimiter` | 最小间隔限流器，`None` 即关闭 |
| `src/smolagents/utils.py` | `Retrying` | 指数退避加抖动的重试器 |

## 参考

- [smolagents 源码：models.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/models.py)
- [smolagents 源码：utils.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/utils.py)
- [OpenAI 文档：限流处理与退避](https://platform.openai.com/docs/guides/rate-limits)

## 与主核 / AI 的关系

主核里最接近的是「客户端连接池的失败重试」：连接被拒、认证失败、超时各有不同处置，而「服务端过载」的处置是退避重试。这里把同一套逻辑放在模型层，是因为限流错误（HTTP 429）在 LLM 场景里出现频率远高于传统数据库——按 token 计费的接口在高峰期被限流是常态而非异常。

与 CrewAI 的 `BaseLLM` 相比，差别在「重试怎么装」：CrewAI 用 `__init_subclass__` 把 `run_with_rate_limit_retry` 包装**注入到子类的 call 方法上**，子类实现者不需要知道重试存在；smolagents 让子类在自己的 `generate` 里显式写 `self.retryer(...)`。前者的好处是不可能漏，后者的好处是调用点可见、能对不同的调用用不同的重试配置。两种选择都合理，但前者在「实现者可能忘记」这件事上更安全。

对 AI 工程最实用的一条是那两个日志钩子的位置。做重试时最常见的可观测性缺陷是「只记录最后一次失败」——中间的尝试与等待时间全部丢失，于是无法判断重试是否有用、延迟花在哪里。这里在「失败即将重试」和「重试成功后返回」两个点都记录耗时与次数，天然能拼出完整时间线。任何自研重试器都应该照这个结构留两个钩子。

另一条是流式请求里 `stream_options={"include_usage": True}` 的必要性。默认不开时，流式响应全程不带 token 计数，于是用量统计只能靠估算。把它显式打开，并单独处理「只带 usage 不带内容」的那种 delta，是让流式路径也能准确记账的关键——否则整条链路的成本统计会在流式模式下静默失真。
