---
project: openai-agents
feature: model-retry
title: 模型调用重试与裁决合并
page_type: feature
schema_version: 1
refs:
  - path: src/agents/retry.py
    symbols: [ModelRetrySettings, RetryDecision, ModelRetryAdvice]
  - path: src/agents/retry.py
    symbols: [_merge_positive_retry_decisions, RetryPolicyContext]
  - path: src/agents/run_internal/model_retry.py
    symbols: [_normalize_retry_error, apply_retry_attempt_usage]
---

# 模型调用重试与裁决合并

## 一句话职责

把「这次失败该不该重试」拆成三层可组合的裁决——provider 建议、运行期策略、运行器自己的重放安全否决——再用一套显式的合并语义把它们拼成一个决定，最后按决定退避并记账用量。

## 机制是什么

三层裁决的输入统一成 `RetryPolicyContext`：

| 输入 | 来源 | 作用 |
|---|---|---|
| `normalized` | `_normalize_retry_error` 从异常链提取 | `status_code`、`error_code`、`message`、`request_id`、`retry_after`、`is_abort`、`is_network_error`、`is_timeout` |
| `provider_advice` | 模型的 `get_retry_advice` 回调 | `suggested`（建议重试/不重试/无意见）、`retry_after`、`replay_safety`（`safe`/`unsafe`/其他）、`response_started`、可覆盖 normalized 的若干字段 |
| 运行参数 | 调用点传入 | `attempt`、`max_retries`、`stream`、`previous_response_id`、`conversation_id` |

策略的返回值是 `bool | RetryDecision`。`RetryDecision` 不只是「是/否」，还有 `delay`、`reason`、`approve_unsafe_replay`，以及三个 `init=False` 的私有标记 `_hard_veto`、`_delegable_replay_veto`、`_approves_replay`。这些私有标记只能由 `retry.py` 里的三个内部函数设置，构成了一套「否决的强弱分级」。

`RetryPolicyContext` 上还有三个派生属性把判断简化：`response_started`（provider 是否已开始输出）、`replay_safety`（三态，非 `safe`/`unsafe` 一律归 `unknown`）、`stateful_request`（是否带 `previous_response_id` 或 `conversation_id`）。

## 怎么实现的

**归一化会把异常链翻一遍。** `_normalize_retry_error` 用 `_get_status_code`/`_get_error_code`/`_get_request_id`/`_get_retry_after` 从异常里取结构化信息，`message=str(error)`；`is_abort` 由 `_is_abort_like_error` 判断；`is_network_error` 由 `_is_network_like_error` 判断（类型白名单是 `httpx` 的 `ConnectError`/`ReadError`/`RemoteProtocolError`/`TimeoutException`/`WriteError`，外加对旧版 httpx 与若干错误文案的兼容）；`is_timeout` 则遍历整条异常链看有没有 `APITimeoutError`/`TimeoutError`/`ModelTimeoutError`。

provider 的 `normalized` 覆盖是**逐字段按显式性**合并的：`ModelRetryNormalizedError` 重写了 `__init__`，用 `_UNSET` 哨兵记录哪些字段被显式传过，存进 `_explicit_fields`；只有显式传过的字段才会覆盖。`is_abort` 是唯一有方向性的：provider 可以补充 abort 证据，但**不能清除**从原始异常推断出的 abort（`normalized.is_abort or override.is_abort`）。

**裁决顺序是「先绝对否决，再问策略，最后过三道重放闸」。** `_evaluate_retry` 的顺序值得逐条记住：

1. `attempt > max_retries` 直接不重试；
2. `normalized.is_abort` 或「本轮已输出过不可重试的流事件」→ 绝对否决，返回 `retry=False`（注释：中止、以及已经吐出用户可见输出的失败，任何应用决定都无法让重放变安全）；
3. provider 标记 `replay_safety == "unsafe"` 且（是流式 或 请求本身带本地副作用标记）→ 不重试，且**在策略运行之前**就挡住；
4. 没有策略 → 不重试；
5. 调策略，若策略也说不重试，直接采纳；
6. 三道重放闸（注释明确说「刻意不合并成一个」）：
   - 请求级重放否决（如程序化工具调用）只能被 provider 自己的 safe 标记豁免，应用级 `approve_unsafe_replay` 不够；
   - 有状态请求（带 `previous_response_id`/`conversation_id`）默认 fail closed，只有 provider safe、或「provider unsafe + 应用显式批准」才放行；重放安全为 `unknown` 时一律挡住；
   - provider 标记 unsafe 时，必须要么 provider safe、要么 `approve_unsafe_replay=True`；
7. 通过后组装最终决定，延迟优先级是「策略给的 delay → normalized 的 `retry_after` → 默认退避」。

**默认退避是指数 + 抖动，且带上下界。** `_default_retry_delay` 取 `initial_delay`（默认 0.25）、`max_delay`（默认 2.0）、`multiplier`（默认 2.0）、`jitter`（默认开），算出 `min(initial * multiplier ** (attempt-1), max_delay)`，抖动时再乘 `0.875 + rand*0.25`（即 ±12.5%）并夹到 `max_delay` 内。

**策略组合有四个工厂，语义各不相同。** `retry_policies` 是一个实例，暴露：`never()`、`provider_suggested()`、`network_error()`、`retry_after()`、`http_status(statuses)`、`all(*policies)`、`any(*policies)`。

`any` 的合并规则最复杂：遇到硬否决立刻返回（但「可委托的重放否决」会先被记下来继续看别的策略）；多个正决定用 `_merge_positive_retry_decisions` 合并——它保留先到的 delay/reason，后到的若非 `None` 则覆盖，`approve_unsafe_replay` 取或，`_approves_replay` 任一为真就置位；最后若存在「可委托否决」且已有正决定，就调 `_resolve_delegable_replay_veto`——只有正决定明确带 `approve_unsafe_replay=True` 才能把否决翻成正决定。

`all` 的规则相反：任一硬否决立即返回；任一非否决的负决定立即返回；全部为正才合成一个正决定。

**策略带能力标记，供运行器判断能否关掉 provider 自己的重试。** `_mark_retry_capabilities` 给策略函数挂两个属性：`retries_safe_transport_errors`、`retries_all_transient_errors`。`all` 用「全部为真」聚合，`any` 用「任一为真」聚合。`network_error()` 与 `provider_suggested()` 标 `retries_safe_transport_errors=True`，`never()` 与 `http_status()` 都是 False。这个标记被 `_should_disable_provider_managed_retries` 与 `_should_disable_websocket_pre_event_retry` 使用：当运行器自己会重试安全传输错误时，就要关掉 provider 隐蔽的重试，避免两层重试叠乘。

**provider 管理的重试有独立的开关上下文。** 循环里用 `provider_managed_retries_disabled(...)` 与 `websocket_pre_event_retries_disabled(...)` 两个上下文管理器包裹每一次模型尝试。决策规则写在 `_should_disable_provider_managed_retries`：`replay_unsafe_request` 一律关；`max_retries <= 0` 一律关（注释：显式零预算应当连隐藏的 provider 重试一起关掉）；`attempt > 1` 时有状态请求关、无策略则不关（没有策略时运行器从不重试，provider 重试是唯一的恢复路径）、有策略则关；首轮有状态且 `max_retries > 0` 且有策略时关。

**流式重试要先解决「已经吐出去的事件」。** `stream_response_with_retry` 每收到一个事件就用 `_stream_event_blocks_retry` 判断：事件类型不在 `{"response.created", "response.in_progress"}` 里就置 `emitted_retry_unsafe_event = True`。此后任何失败都走第 2 条绝对否决。这解释了为什么流式重试只能在「刚开始、还没吐内容」的窗口里发生。

**带超时的流式尝试被隔离到一个任务里。** 若 `timeout` 不为 `None`，代码不是直接 `await stream.__anext__()`，而是建一个 `_run_stream_attempt_in_one_task` 任务，用两个 `asyncio.Queue` 做「请求下一次拉取 / 回传事件或异常」的握手，主循环按 deadline 算剩余时间调 `_await_model_attempt`。超时后 `_cancel_and_drain_model_attempt_task` 取消并排空该任务，然后**把 `stream`、`stream_owner`、`event`、`result_value` 等局部变量逐个置 `None`** 再抛——注释说明理由是「超时 traceback 帧不应保留被取消的 provider 任务与其载荷」。同一个理由也出现在 `_await_model_attempt` 里，那里是 `del awaitable, task, done, pending` 后 `raise timeout_error from None`。

**重试的成功路径要补记账。** `apply_retry_attempt_usage(usage, failed_attempts, reported_usage)` 做的是：失败次数为 0 原样返回；否则把成功那次的条目先摘出来，逐个失败尝试补齐 `requests` 与 `request_usage_entries`（有 provider 上报的用量就 `add`，没有再补一个全零 `RequestUsage`），最后把成功条目接回去。配套的 `record_failed_attempt_usage_on_error` 是一个上下文管理器：若重试循环最终失败，把 `reported_usage` 里所有已上报的用量补进运行总用量再重抛——避免「重试三次都失败」时用量统计为零。

**「会话被锁」有一条为兼容保留的独立重试路径。** `_is_conversation_locked_error` 命中且 `_should_preserve_conversation_locked_compatibility` 为真（默认真，除非显式 `max_retries=0`）时，走固定 3 次、延迟 `1.0 * 2**(n-1)` 的重试，**不经过策略**。源码注释说明了为什么保留：历史上这条路径存在，调用方可能只为别的失败启用了策略，不该因此失去这层兼容。

## 关键取舍

**源码事实：** `RetryDecision` 的否决分三档，且**普通 `retry=True` 绝不绕过重放保护**。`approve_unsafe_replay` 是与 `retry` 并列的独立字段，docstring 明写「只应设置在你接受 provider 侧可能已发生的工作被重复执行的工作负载上」。这是把「重试」与「允许不安全重放」在类型上分开，而不是让调用方用一个布尔同时表达两件事。

**源码事实：** 「provider 说 unsafe + 流式」的否决发生在策略**之前**。应用无法通过任何策略推翻「已经开始输出的流失败后重放」——因为那不是策略问题，是已经吐给用户了。

**源码事实：** 有状态请求默认 fail closed。带 `previous_response_id`/`conversation_id` 的请求依赖服务端状态，重放可能让服务端的对话增量与本地视图错位；因此只有在 provider 明确 safe、或 provider 明确 unsafe 且应用显式批准时才放行。重放安全 `unknown` 时永远挡住——注释的原话是「没有 provider-unsafe 的失败可供这个批准去针对」。

**源码事实：** `all` / `any` 的能力标记聚合规则不同（全体与任一），因为它们表达的「这组策略是否覆盖了安全传输错误的重试」在逻辑上确实不同：全部策略都覆盖才算全覆盖，任一覆盖就算有兜底。

**源码事实：** 会话锁兼容重试不经过策略，且不受 `attempt` 计数影响（单独用 `compatibility_retries_taken`）。它会在 `max_retries=0` 时才关闭。这是一条纯粹为向后兼容存在的旁路。

**架构推论：** 把重试的「语义」与「执行」分在两个文件（`retry.py` 定义策略与裁决，`run_internal/model_retry.py` 执行循环与用量记账），使策略可以是纯函数、可单测、可组合，而循环负责超时、取消、流式、记账这些脏活。这与主核里「优化器规则与执行器」的分层是同一手法。

**架构推论：** 重放安全的三态（`safe`/`unsafe`/`unknown`）而不是两态，是关键设计。两态会迫使「provider 没说」被归到某一边，而两边都错：归 safe 会放任不安全重放，归 unsafe 会让所有未知 provider 完全失去重试能力。三态加「unknown 时只在无状态请求上放行」才留出了可用空间。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/retry.py` | `ModelRetrySettings` | 开关式重试设置：`max_retries`、`backoff`、运行期 `policy` |
| `src/agents/retry.py` | `RetryDecision` | 决定对象，含延迟、原因、三档否决与不安全重放批准 |
| `src/agents/retry.py` | `ModelRetryAdvice` | provider 侧建议：是否重试、`retry_after`、重放安全性、是否已开始输出 |
| `src/agents/retry.py` | `RetryPolicyContext` | 策略入参，含归一化错误、provider 建议、三态重放安全与派生属性 |
| `src/agents/retry.py` | `ModelRetryNormalizedError` | 归一化错误事实，用哨兵记录哪些字段被显式覆盖 |
| `src/agents/retry.py` | `_merge_positive_retry_decisions` | 两个正决定的合并：延迟/原因后者覆盖，批准取或 |
| `src/agents/retry.py` | `_resolve_delegable_replay_veto` | 可委托否决在应用显式批准时翻转为正决定 |
| `src/agents/retry.py` | `retry_policies.all` / `any` | 策略组合，含能力标记的与/或聚合 |
| `src/agents/retry.py` | `retry_policies.provider_suggested` | 采纳 provider 建议，并按 unsafe 分档否决 |
| `src/agents/run_internal/model_retry.py` | `_normalize_retry_error` | 异常链 → 归一化错误，含 provider 逐字段覆盖 |
| `src/agents/run_internal/model_retry.py` | `_evaluate_retry` | 裁决总入口：绝对否决 → 策略 → 三道重放闸 |
| `src/agents/run_internal/model_retry.py` | `_default_retry_delay` | 指数退避 + ±12.5% 抖动，带上下界 |
| `src/agents/run_internal/model_retry.py` | `get_response_with_retry` | 非流式重试循环，含兼容会话锁旁路 |
| `src/agents/run_internal/model_retry.py` | `stream_response_with_retry` | 流式重试循环，跟踪「已吐事件」与超时任务隔离 |
| `src/agents/run_internal/model_retry.py` | `_should_disable_provider_managed_retries` | 何时关掉 provider 隐蔽重试，避免两层叠乘 |
| `src/agents/run_internal/model_retry.py` | `apply_retry_attempt_usage` | 把失败尝试的用量补进成功响应的用量 |
| `src/agents/run_internal/model_retry.py` | `record_failed_attempt_usage_on_error` | 重试全败时把已上报用量补进运行总量 |
| `src/agents/run_internal/model_retry.py` | `_stream_event_blocks_retry` | 只有 created/in_progress 允许重试 |

## 参考

- [openai-agents-python 源码：retry.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/retry.py)
- [openai-agents-python 源码：model_retry.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/run_internal/model_retry.py)
- [Agents SDK API：retry 模块](https://openai.github.io/openai-agents-python/ref/retry/)
- [Agents SDK 文档：运行智能体](https://openai.github.io/openai-agents-python/running_agents/)

## 与主核 / AI 的关系

重试这件事在主核里对应的是「事务重试」：也要判断错误是否可重试、要不要退避、副作用是否已发生。差别在于主核能靠事务日志判断「这个操作是否已提交」，这里的判断来自 provider 的语义声明与应用的实现约定。所以这里的核心机制不是「怎么重试」，而是**怎么在信息不全时保守地不重试**——三态重放安全 + 三道独立闸门就是这个态度的具体形态。

对 AI 工程有四条高价值结论。第一，**流式失败的重试窗口极窄**：只有 `response.created` 与 `response.in_progress` 两类事件到达前可重试，一旦吐出正文就永久放弃。任何「流式 + 自动重试」的设计都必须先确认这条。第二，**有状态请求不能随便重放**：带 `previous_response_id`/`conversation_id` 的调用依赖服务端状态，盲目重试会让服务端与本地视图分叉。第三，**两层重试要互斥**：SDK 自己的重试与 provider 的隐蔽重试同时生效会把失败次数乘起来，账单和延迟都会爆；`retries_safe_transport_errors` 这类能力标记就是为互斥服务的。第四，**失败的用量也要记账**：重试三次全失败、账单却显示零 token，是这类系统最常见的对账漏洞，`record_failed_attempt_usage_on_error` 正是补这个口子。

最后一条更普遍：把「重试」与「允许不安全重放」拆成两个字段，而不是让一个布尔同时表达。任何有副作用的调用在重试时都面临同一问题，能显式区分「再来一次」与「我知道可能重复也再来一次」的 API，才配得上放在生产路径上。
