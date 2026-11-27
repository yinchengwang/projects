---
project: pydantic-ai
feature: usage-accounting
title: 用量归属与成本
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/usage.py
    symbols: [RunUsage, RequestUsage, UsageLimits]
  - path: pydantic_ai_slim/pydantic_ai/_usage_attribution.py
    symbols: [accumulate, record_request, record_tool_call]
---

# 用量归属与成本

## 一句话职责

把「这次花了多少」拆成两个层次（单次请求的 `RequestUsage`、整次运行的 `RunUsage`），再用一个基于调用栈的归属机制回答一个更难的问题：**在并发多 agent 的场景里，这笔用量该记在谁头上**。

## 机制是什么

三个类型：

- `UsageBase` 是所有用量类型的共享基类，字段按「**包含式的父子桶**」组织，不是互斥的：`input_tokens` 包含 `cache_read_tokens`、`cache_write_tokens`、`input_audio_tokens`；`output_tokens` 包含 `output_reasoning_tokens`、`output_audio_tokens`；`cache_read_tokens` 又包含 `cache_audio_read_tokens`。
- `RequestUsage` 是**单次请求**的用量。它有个特殊属性 `requests` 恒为 `1`——这是为了让它能作为 genai-prices 的 `AbstractUsage` 被计价。
- `RunUsage` 是**运行级**累计，多了 `requests`、`tool_calls` 两个计数。

`UsageLimits` 是配额检查，`_usage_attribution.py` 是归属机制。

## 怎么实现的

**父子桶的约定必须靠归一化维持。** `input_tokens` 的文档明说：token 计数是包含式的父子桶，这个总数**包含**缓存 token 与音频 token；并且「用量提取会归一化那些分开上报的供应商（例如 Anthropic 与 Bedrock 的原始 `input_tokens` 不含缓存读写），使这条约定处处成立」。这条归一化是整个用量体系可信的前提——如果一个供应商不归一化，跨供应商的缓存命中率与成本就无法比较。

**`cost` 用 `None` 而不是 `0` 表示未知。** 文档解释：`None`（而非零）用于「模型或供应商无法定价」的情形，这样「未知」与「真的花了 0」保持可区分。它还给了两条实用信息：比你的安装版本更新的模型可以用 `update_in_background()` 补价格；经 Pydantic AI Gateway 的请求会使用网关上报的成本（当它上报时）。

**`audio_seconds` 存在的原因是一个具体缺口。** 文档说：有些实时模型（例如 xAI 的 Grok Voice）根本没有 token 价格，按音频秒数计费，所以它们的 token 计数会算出零成本；上报时长才能让这类调用可计价。它还解释了为什么这是一个字段而不是 `details` 里的一项：`details` 被刻意排除在计价之外，且类型是 `dict[str, int]` 而时长是小数。

**`unmeasured_requests` 让「下界」变成显式的。** 当响应没有携带用量信息时这个计数非零，此时 token 总量与任何已知成本都是**下界**。文档还限定：这个计数不暗示缺失了多少 token、成本或细节。在一个「缺失静默为零」的常见实现里，这条显式化是必要的。

**增量靠遍历 `__dict__` 而不是逐字段列举。** `_add_usage_tokens` 的做法是取两边 `__dict__.keys()` 的并集、排除 `requests`/`tool_calls`/`details`/`cost`，对两边都是 `int` 或 `float` 的字段做加减。这样**供应商特有的计数器**（`details` 之外、以属性形式存在的那些）也会被正确累加，不需要每加一个字段就改一次加法。`sign=-1` 是同一个函数的逆运算，被 `RunUsage.__sub__` 与「续算请求在挂起响应之外又花了多少」的计算使用。

**`RequestUsage.incr` 不碰 `requests`，`RunUsage.incr` 会。** 这个差异看似细节，实际是设计要点：把一个 `RequestUsage` 加进 `RunUsage` 不会增加 `RunUsage.requests`，因为后者计的是「agent 真正据此行动过的响应数」，不是「发出去的请求数」。文档在 `RunUsage.requests` 上把这条讲透了：被 `FallbackModel` 或 `ModelFallback` 跳过的尝试、完成挂起响应的续算请求、供应商 SDK 或 HTTP 传输层的重试**都不计入**；但被拒响应与续算请求的 **token 与成本仍会计入**。被跳过的尝试列在该响应的 `failed_attempts` 上。

**`requests` 的计数口径直接决定 `request_limit` 的语义。** `UsageLimits.request_limit` 的文档复述了同一件事，并补了一条：它在每一步开始时检查，**包括一次没有向模型发请求就被回答的步骤**。所以 `request_limit` 限的是「agent 循环的轮数」，不是「打到供应商的请求数」。这是一个非常容易被误读的参数。

**三种检查时机不同。** `check_before_request` 在发请求前检查 `request_limit`、`input_tokens_limit`、`total_tokens_limit`、`cost_limit`；`check_before_tool_call` 用「预测用量」检查 `tool_calls_limit`；`check_per_request_input_tokens` 检查单次请求的输入 token 数，且文档明说它看的是**单条请求**而非累计的 `RunUsage.input_tokens`。

**`per_request_input_tokens_limit` 是另一类防护。** 文档解释了它与累计限额的分工：累计限额防「跑飞」，单请求限额防「上下文过大」（损害模型表现、缓存未命中时成本高）。它还点明一个常见误解：`input_tokens`（因此这条限额）**包含缓存前缀的 token**，是归一化后的口径——一个几乎全从缓存服务的请求仍然把完整上下文计入这条限额。所以它「限的是上下文大小，不是缓存未命中成本」。

**`has_token_limits` 是一个性能开关而非语义开关。** 它返回是否有任何 token 限额。文档说明用途：有 token 限额时需要在收到每条流式消息后检查；没有限额就能在流式迭代器里跳过这段处理。

**归属机制的核心是「按调用栈而非按对象」记账。** `_usage_attribution.py` 的模块 docstring 把问题和解法都讲清了：`RunUsage` 本身无法说明是谁产生的用量——它是就地累加的，而多 agent 委派模式会把**同一个对象**交给并发委派方（`usage=ctx.usage`），所以无论是对象内容还是「结束减开始」的差值都分不清本次运行的请求与兄弟运行的请求（并发委派方会互相吸收）。

解法：**哪个运行在产出是调用栈的性质，所以就跟调用栈**。`_active` 是一个 `ContextVar[RunUsage | None]`；`accumulate(run_usage)` 在它的 span 打开期间把该运行的 `RunUsage` 设为「被记功」的那一个，退出时恢复外层的；`record_request` / `record_tool_call` / `record_usage` 只记给最内层那个。每个运行启动时以 `None` 进入，因此**没有自己 span 的运行不记给任何人**，而不是记给外层。

**asyncio 的任务上下文复制让并发委派天然隔离。** docstring 点明：asyncio 在创建任务时会复制上下文，所以并发委派方各自从父级开始、然后替换成自己的，永远看不到彼此的。

**这个模块独占就地修改用量字段的权利。** docstring 最后一句很强硬：本模块拥有对运行用量的**唯一**就地修改权；直接给 `RunUsage` 字段加一会让 token 数从该运行的 span 上消失，因此 `tests/test_usage_attribution.py` 会对未标 `# usage-attribution: ok` 并附理由的裸 `requests += `、`tool_calls += ` 或 `.incr(` 报错。

**归属的目的是让 trace 能直接相加。** docstring 开头说明了设计目标：一次 agent 运行的 span 上报**该运行自己发起的请求**的用量，而不是它嵌套运行的用量（后者自己上报）。这样把 trace 里的 agent-run span 相加就得到整个对话的总量，与它们下面那些 `chat` span 的和一致——这正是把 agent-run 用量放在独立属性命名空间的意义：后端可以相加而不重复计数。

**`RunUsage` 的属性可被序列化保留。** `UsageBase.__get_pydantic_core_schema__` 用一个自定义 validator/serializer 保留任意额外字段，`reserved_names` 排除掉字段名、类属性名与 `_LEGACY_USAGE_KEYS`（`requests`、`request_tokens`、`response_tokens`、`total_tokens`）。这让「供应商特有计数器」在存取后不丢，同时不会把旧键名当成动态字段收进来。

**旧键名有两条兼容路径。** `input_tokens` / `output_tokens` 用 `Field(validation_alias=AliasChoices('input_tokens', 'request_tokens'))` 之类接受旧名；`details` 用 `BeforeValidator(lambda d: d or {})` 接受曾经的 `None`。这些都在为「数据库里存着改名之前的模型响应」服务。

## 关键取舍

**源码事实：** token 计数是包含式父子桶，且靠各供应商适配器的归一化来维持。不归一化就无法跨供应商比较，成本也算不准。

**源码事实：** `cost=None` 与 `cost=0` 语义不同。未知不能被当成零。

**源码事实：** `unmeasured_requests` 让「数据缺失」显式化，并把总量标注为下界。

**源码事实：** `requests` 只计「agent 据此行动过的响应」，不计被回退跳过的尝试、续算请求、传输层重试——但那些的 token 与成本会计入。这条差异让「轮数」与「花费」两个口径不能互相推导。

**源码事实：** 发请求前的限额检查在每步开始时进行，包括不发请求那种步骤，因此 `request_limit` 限的是循环轮数。

**源码事实：** `per_request_input_tokens_limit` 限的是上下文大小而非成本：它包含缓存前缀，一个几乎全命缓存的请求也照样计入。

**源码事实：** 用量归属按调用栈（`ContextVar`）而不是对象，因为多 agent 委派会共享同一个 `RunUsage` 对象。没有 span 的运行不记给任何人。

**源码事实：** 就地修改 `RunUsage` 字段只允许发生在归属模块内，且被测试强制。这是「让统计数据不丢」的纪律化手段。

**架构推论：** 用量记账在 agent 系统里的困难不是加法，而是**归属**与**口径**。归属难因为对象会被共享、并发会交错、嵌套会重叠；口径难因为「一次请求」在不同层面意味着不同的东西（轮数 / 供应商请求数 / 计费单元）。这套实现的选择是：对象只做累加，归属交给上下文；口径用字段名精确区分（`requests` vs `failed_attempts` vs `unmeasured_requests`），而不是靠文档约定。代价是调用者必须理解这几个口径的差别，否则「限额为什么提前触发」或「为什么总数对不上」永远查不出来。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/usage.py` | `UsageBase` | 用量的公共字段与包含式父子桶约定 |
| `pydantic_ai_slim/pydantic_ai/usage.py` | `RunUsage` | 运行级累计：`requests`、`tool_calls` 与 token/成本 |
| `pydantic_ai_slim/pydantic_ai/usage.py` | `RequestUsage` | 单次请求用量，`requests` 恒为 1 以适配计价库 |
| `pydantic_ai_slim/pydantic_ai/usage.py` | `UsageLimits` | 配额检查：累计值、单请求值、工具调用数 |
| `pydantic_ai_slim/pydantic_ai/usage.py` | `UsageLimits.check_before_request` | 每步开始时的请求数与累计 token/成本检查 |
| `pydantic_ai_slim/pydantic_ai/usage.py` | `UsageLimits.check_per_request_input_tokens` | 单请求上下文大小检查，含缓存前缀 |
| `pydantic_ai_slim/pydantic_ai/usage.py` | `UsageLimits.has_token_limits` | 让流式路径在无限额时跳过检查 |
| `pydantic_ai_slim/pydantic_ai/usage.py` | `_add_usage_tokens` | 遍历 `__dict__` 的通用加减，支持供应商特有计数器 |
| `pydantic_ai_slim/pydantic_ai/usage.py` | `_incr_usage_cost` | 成本累加，保留「未知」语义 |
| `pydantic_ai_slim/pydantic_ai/usage.py` | `cache_hit_ratio` | 跨供应商可比的缓存命中率 |
| `pydantic_ai_slim/pydantic_ai/_usage_attribution.py` | `accumulate` | 在 span 期间把某运行的用量设为被记功者 |
| `pydantic_ai_slim/pydantic_ai/_usage_attribution.py` | `record_request` | 只记给最内层运行，不记给外层 |
| `pydantic_ai_slim/pydantic_ai/_usage_attribution.py` | `record_usage` | 累加一条响应的用量或一个跨边界的增量 |

## 参考

- [pydantic-ai 源码：usage.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/usage.py)
- [pydantic-ai 源码：_usage_attribution.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_usage_attribution.py)
- [Pydantic AI 文档：用量与限额](https://ai.pydantic.dev/api/usage/)
- [genai-prices：模型价格库](https://github.com/pydantic/genai-prices)

## 与主核 / AI 的关系

用量记账与主核的「执行统计 / 等待事件」在结构上同源：都是「就地累加的共享计数器 + 事后归属」。数据库里这类计数器通常靠会话/后台进程 id 归属，这里靠 contextvar 归属——因为 agent 的并发模型是任务而不是进程。

对 agent 工程的直接价值有三条。其一，**`request_limit` 不是请求数上限**。它数的是 agent 循环的轮数，被回退跳过的尝试与传输层重试都不计入，但它们的 token 与钱会计入成本。以为它等于「最多打几次 API」是常见误判。其二，**并发委派必须靠上下文而不是对象归属**。把同一个 `RunUsage` 交给多个子 agent 是官方模式，因此任何「按对象差值算」的统计都会把兄弟运行的量算到自己头上。其三，**`cost=None` 必须与 0 分开**。把未知成本当零会让预算告警在最需要它的场景（新模型、私有部署）静默失效。
