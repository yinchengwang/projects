---
project: pydantic-ai
feature: model-fallback
title: 模型回退链
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/models/fallback.py
    symbols: [FallbackModel]
  - path: pydantic_ai_slim/pydantic_ai/_fallback.py
    symbols: [FallbackChain, ResponseRejected, FallbackPredicates]
---

# 模型回退链

## 一句话职责

在一次模型请求内部按顺序试一串候选模型：`fallback_on` 决定「什么样的失败算失败」，第一个通过判定的响应胜出，全部失败则抛一个携带全部失败原因与全部尝试记录的异常组。

## 机制是什么

`FallbackModel(default_model, *fallback_models, fallback_on=(ModelAPIError,))` 是一个 `Model` 子类，它在**单次请求内**走完整个链条，因此脱离 agent 也能用（`direct.model_request` 或任何直接持有 `Model` 的场合）。

`fallback_on` 接受四种形态，可混合成序列：

| 形态 | 例子 | 判定对象 |
|---|---|---|
| 异常类型 | `ModelRateLimitError` | 抛出的异常 |
| 异常处理器 | `lambda exc: isinstance(exc, MyError)` | 抛出的异常 |
| 响应处理器 | `def check(r: ModelResponse) -> bool` | 返回的响应 |
| 序列混合 | `[ModelAPIError, exc_handler, response_handler]` | 逐项 |

处理器类型的判别靠**首参数类型注解**：注解正好是 `ModelResponse` 才算响应处理器，其余（含无注解、含 lambda）一律当异常处理器。

`FallbackPredicates` 是解析结果，把谓词分成 `exception_handlers`、`response_handlers` 两组，外加两个布尔位 `content_filter` 与 `output_token_limit`。

`FallbackChain` 承载一次请求的「下行过程」：谓词、owner 名、已收集的异常、已拒绝的响应、以及 `AttemptLog`。`ResponseRejected` 是当所有候选都被响应处理器拒绝时，塞进异常组的那条特殊异常。

## 怎么实现的

**两个布尔位不是性能优化，是语义修正。** `content_filter` 与 `output_token_limit` 这两个位在谓词里出现 `ContentFilterError` / `OutputTokenLimitExceeded` 时被置真。文档解释了原因：agent 只在响应**被接受之后**才抛这两个异常，那时已经太晚，无法回退或重试。所以这两个异常类型实际上代表「任何 `finish_reason` 为 `content_filter` / `length` 的响应」。判定时直接看 `finish_reason`，而不是等异常。这条设计的边界也写清了：流式响应不套用（流的 finish reason 只有流完才知道，所以流式下的拒答被接受），且不告警——与响应处理器不同。

**`OutputTokenLimitExceeded` 有一个排除项。** 只有它不是 `IncompleteToolCall` 的子类时才置 `output_token_limit`。原因写得很具体：`IncompleteToolCall` 只命名「被截断的工具调用」，不覆盖所有 `finish_reason='length'` 的响应，把它的语义扩大化会误伤。而 `length` 的响应可能是「有部分可用输出」，甚至「完整输出但恰好触顶」，所以这一位覆盖的是一个比异常名更宽的范围。

**处理器类型靠注解判别，且规则很严。** `_add_handler` 只在 `get_first_param_type(handler) is ModelResponse` 时认作响应处理器——必须是**正好** `ModelResponse`，不能是 `Optional[ModelResponse]`、不能是子类。其余全部当异常处理器。这条严格规则的后果是：一个想检查响应的处理器若忘了写注解，会被静默当成异常处理器，于是永远不匹配。

**空谓词直接报错。** `FallbackPredicates.parse` 在两组处理器都为空时抛 `UserError`，消息里点明「所有异常都会传播、所有响应都会被接受」并建议用默认值。这是一个**拒绝静默失效**的设计：空 `fallback_on` 会让 `FallbackModel` 退化成「只用第一个模型」，与其静默退化不如报错。

**`FallbackChain.falls_back` 兼职记录。** 它先问谓词是否匹配，匹配则把失败（异常或响应）分别记进 `exceptions` 或 `rejected_responses`，返回真。这个「判定即记录」的写法保证了异常组里的失败清单与实际跳过的候选严格对应，不会有漏记或重复记。

**`exhausted()` 组装异常组。** 它把 `exceptions` 全部塞进 `FallbackExceptionGroup`；若有被拒响应，再追加一条 `ResponseRejected(count)`——注意这里只是一个计数，不是把响应本身放进去（响应可能很大且含敏感内容）。异常组的消息是 `All models from {owner} failed`，`owner` 让「是 `FallbackModel` 还是 `ModelFallback` 能力」在报错里可区分。最后把 `attempts` 挂到组对象上。

**`AttemptLog` 的三条合并规则。** `record` 面对 `ModelResponse` 失败时，会把该响应自己携带的 `failed_attempts` 展开合并（因为嵌套的 `FallbackModel` 已经记过它内部的尝试，且那些也被计费了）；面对 `FallbackExceptionGroup` 时展开 `group.attempts`；面对普通异常时用 `carried_attempts` 从异常上取。合并完才追加本次尝试。这套规则解决的是**嵌套回退链的记账去重**：一个 `FallbackModel` 包里套另一个时，内层失败的尝试不能丢，也不能重复计。

**span 只在非重放时开。** 记录每次尝试的 ERROR 子 span，条件里有一条 `not start.replayed()`——重放模式下不重复产出 span。这暗示 `AttemptStart` 知道自己是回放还是首次执行，这一点与持久执行引擎的确定性要求对得上。

**流式请求的两条特殊规则。** 一是「打开流的第一个事件之前出错」仍可回退：`request_stream_until_first_event` 就是为了让「还没吐出任何事件」阶段的失败仍算可选。二是响应处理器对流式请求不生效，只发一次警告（`warn_response_handlers_not_applied`），消息里列出了会导致流式的各种入口（`run_stream`、`run_stream_events`、`node.stream()`、`event_stream_handler`、事件监听器等）。这个警告文案之详细，说明「为什么我的响应处理器没生效」是个高频困惑。

**挂起的响应会把链固定住。** 若上一个响应 `state='suspended'`（供应商那边还在算），这次请求**绕过整条链**直接打给当初发起它的模型。若那个模型在续算时失败、或它的响应被拒，则回卷历史（剥掉挂起的响应与尾部的续算请求）、最佳努力取消服务端的那个作业、然后才落回正常链条。取消这一步注释写得很直白：不取消的话，一个 OpenAI 后台作业会继续跑并继续计费，而链条已经发出了重复请求。

**回卷之后要打「替换」标记。** 因为回卷后链条产出的第一条成功响应是真正的全新生成，但它可能与那条被放弃的挂起响应有相同的 `model_name`（只有 `provider_response_id` 不同）。若不加 `replace_previous_response` 标记，合并逻辑会把它判成「累加」（与 Anthropic 的 `pause_turn` 无法区分），从而把被放弃的挂起部分重复排在新轮之前。这个标记是瞬时的，被消费后即弹出，不会落进历史。

## 关键取舍

**源码事实：** 回退发生在**单次请求内**，不是跨 agent 步骤。因此一次回退消耗的是一个 `ModelResponse`，`RunUsage.requests` 只加一——但这一个响应背后可能有多次计费的尝试，它们的 token 与成本仍会累加到 usage 上，尝试细节记在 `failed_attempts` 里。

**源码事实：** 响应处理器对流式请求不生效，且只会警告不会报错。这让流式与非流式在「可否按响应内容回退」上行为不同——同一份配置在两种入口下不等价。

**源码事实：** `ContentFilterError` 与 `OutputTokenLimitExceeded` 被特殊处理成「代表一类响应而不是一个异常」。这是对「异常抛得太晚」的补偿，也让这两个名字的实际语义比字面更宽。

**源码事实：** 处理器类型判别依赖首参数注解且要求精确等于 `ModelResponse`。无注解的处理器会被当成异常处理器，静默不生效。

**源码事实：** 空 `fallback_on` 直接报错而不是退化成「无回退」。这是刻意避免静默失效。

**源码事实：** 嵌套回退链的尝试记录会合并，且被放弃的挂起作业会被最佳努力取消。两条都是为了「不重复计费、不重复请求」——在按用量计费的场景里，这是钱的问题而不是整洁问题。

**架构推论：** 回退链的本质是**在单点失败上做局部重试，而不是重做整个任务**。这决定了它的适用边界：适合「同一个请求换个模型再问一次」，不适合「模型 A 产出的中间结果模型 B 接不上」这类语义不兼容。`FallbackModel` 与 `ModelFallback` 能力共享同一套核心（谓词、链、尝试记录、挂起响应固定），差别只在走法——前者在一次请求内走完，后者通过 agent 的重试循环一次一个 `RetryModelRequest`。共享核心的价值是「修一次两边都对」，代价是核心要同时满足「单请求内」与「跨步」两种节奏。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/models/fallback.py` | `FallbackModel` | 单请求内走完回退链的 `Model` 实现 |
| `pydantic_ai_slim/pydantic_ai/models/fallback.py` | `FallbackModel.request` | 挂起响应固定、失败回卷、取消被弃作业、落回链条 |
| `pydantic_ai_slim/pydantic_ai/_fallback.py` | `FallbackPredicates` | 解析 `fallback_on`，含两个语义修正布尔位 |
| `pydantic_ai_slim/pydantic_ai/_fallback.py` | `FallbackPredicates.matches` | 按 `finish_reason` 或处理器判定 |
| `pydantic_ai_slim/pydantic_ai/_fallback.py` | `FallbackChain` | 一次请求的下行过程与失败收集 |
| `pydantic_ai_slim/pydantic_ai/_fallback.py` | `FallbackChain.falls_back` | 判定即记录，保证异常组与跳过的候选对应 |
| `pydantic_ai_slim/pydantic_ai/_fallback.py` | `FallbackChain.exhausted` | 组装 `FallbackExceptionGroup`，挂上尝试记录 |
| `pydantic_ai_slim/pydantic_ai/_fallback.py` | `ResponseRejected` | 全部候选被响应处理器拒绝时的占位异常 |
| `pydantic_ai_slim/pydantic_ai/_fallback.py` | `AttemptLog.record` | 嵌套尝试合并，避免重复记账 |
| `pydantic_ai_slim/pydantic_ai/_fallback.py` | `warn_response_handlers_not_applied` | 流式下响应处理器不生效的警告 |
| `pydantic_ai_slim/pydantic_ai/_fallback.py` | `_rewind_messages` | 剥掉挂起响应，让链条看到干净的尾部 |
| `pydantic_ai_slim/pydantic_ai/models/fallback.py` | `_stamp_replace_previous` | 回卷后首条成功响应打「替换」标记，防重复累加 |

## 参考

- [pydantic-ai 源码：models/fallback.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/models/fallback.py)
- [pydantic-ai 源码：_fallback.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_fallback.py)
- [Pydantic AI 文档：模型回退](https://ai.pydantic.dev/capabilities/model-fallback/)

## 与主核 / AI 的关系

回退链与主核的「副本重试」有共同骨架：按优先级试一组目标，失败记一笔，全失败则返回一份包含所有失败原因的汇总。差别在语义层次——主核的重试目标是同一个副本集合（同构），回退链的候选是**不同模型**（异构），因此必须回答「什么算可接受的失败」这个额外问题，这也是 `fallback_on` 存在的原因。

对 agent 工程的直接价值有三条。其一，**回退会放大成本而不是节省**。一次回退的响应在 usage 上仍算一个请求，但被拒绝的响应、被放弃的挂起作业、被取消的服务端任务都已经计费。把回退当作「省钱手段」是误解，它的价值是「提高单次请求成功率」。其二，**流式与响应的判定能力不对等**，同一份配置在 `run` 与 `run_stream` 下行为不同，切换入口时要重新验证回退是否还按预期触发。其三，**嵌套回退链必须能合并尝试记录**，否则排查时看到的失败清单会缺项或重复——这也是为什么这套核心被两个走法共享，而非各写一份。
