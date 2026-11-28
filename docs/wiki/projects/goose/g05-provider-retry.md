---
project: goose
feature: provider-retry
title: 重试判定层次与退避曲线
page_type: feature
schema_version: 1
refs:
  - path: crates/oi-kit-provider-types/src/retry.rs
    symbols: [RetryConfig, ProviderRetry, should_retry]
  - path: crates/oi-kit-provider-types/src/retry.rs
    symbols: [is_permanent_request_failure, DEFAULT_MAX_RETRIES, DEFAULT_BACKOFF_MULTIPLIER]
---

# 重试判定层次与退避曲线

## 一句话职责

把「这次失败该不该再试一次」拆成三层判定：错误类型的可观性、可配置的瞬态策略、以及一组硬编码的「永不可能成功」特征串；并用一条带抖动的指数退避曲线决定等多久。

## 机制是什么

`RetryConfig` 有五个字段：`max_retries`、`initial_interval_ms`、`backoff_multiplier`、`max_interval_ms`、`transient_only`。四个常量给了默认值：最多 3 次、初始 1 秒、乘子 2.0、上限 30 秒。

`ProviderRetry` 是 trait，但**不是一个独立的、需要实现的 trait**：它有两个默认方法（`retry_config` 返回默认配置、`with_retry` 用默认配置调 `with_retry_config`），只有 `with_retry_config` 需要实现。而实现由一条 blanket impl 提供——`impl<P: Provider> ProviderRetry for P`。所以任何 `Provider` 自动获得重试能力，不需要写任何代码。

这个「trait + blanket impl」的组合是为一个具体目的：`Provider` 要保持 dyn 兼容（可放进 `Box<dyn Provider>`），而泛型方法会破坏对象安全。所以把泛型方法挪到另一个 trait 上，用 blanket impl 让所有 provider 都有它。

`should_retry(error, config)` 是纯函数式的判定入口，`is_permanent_request_failure(message)` 是它内部调用的「特征串匹配」子判定。`retry_operation(...)` 是独立的自由函数版本，`ProviderRetry::with_retry_config` 是带「凭据刷新」增强的方法版本。

## 怎么实现的

**三层判定的顺序。** `should_retry` 的 match 分支揭示了完整的优先级：

1. `RateLimitExceeded{..}`、`ServerError(_)`、`NetworkError(_)` → 无条件 `true`。
2. `RequestFailed(message)` 且 `is_permanent_request_failure(message)` → **无条件 `false`**。
3. `RequestFailed(_)` 其余 → `!config.transient_only`（默认 `transient_only=false`，所以默认会重试）。
4. 其他所有错误 → `false`。

第 1 层是「明确瞬态」——限流、服务端错、网络断。这些与配置无关，总是重试。

第 2 层是「明确永久」——这里最值得细看。`PERMANENT_REQUEST_FAILURE_MARKERS` 是三个字符串片段：Anthropic 的两条关于 `thinking`/`redacted_thinking` 块不可修改的消息（分别出现在「助手消息里的块不可被修改」与「必须保持原响应中的样子」两种措辞），以及 `"Reasoning is mandatory for this endpoint"`。此外还并入了两个模块级判据：`formats::anthropic::is_thinking_signature_error` 与 `is_unsupported_thinking_config_error`，以及 `formats::openai_responses::is_encrypted_reasoning_error`。

注释解释了为什么这些必须硬编码为「永久」：Anthropic 把签名过的 thinking 块视为不可变，一旦会话中途改了 thinking 模型的配置，**每次重试都会重建同一份载荷**，所以重试在数学上不可能成功。

这一层的位置很关键：它排在 `transient_only` 判断**之前**。所以即使把配置设成「重试所有 RequestFailed」，这些特征串仍然阻断重试。

第 3 层是「可配置的灰色地带」——4xx 客户端错误通常是「你请求写错了」，重试无益，但有些 provider 会返回可恢复的 4xx。`transient_only=true` 把整个这一层关掉，只留第 1 层。

**意义在于三层的语义不同**：第 1 层由错误类型决定（编译期已知），第 2 层由内容特征决定（运行期嗅探但结论是确定的），第 3 层由配置决定（策略选择）。

**退避曲线的构造。** `delay_for_attempt(attempt)`：

- `attempt == 0` → 零延时。这是「第一次尝试不等待」，不是一次重试。
- 否则 `exponent = attempt - 1`，`base = initial_interval_ms * backoff_multiplier^exponent`。
- `min(base, max_interval_ms)` 做封顶。
- 抖动因子 `0.8 + rand * 0.4`，即 **0.8 到 1.2 之间**，然后乘上去。

所以第 1、2、3 次重试的封装前延时分别是 `1s`、`2s`、`4s`（乘子 2.0、初始 1s），封顶 30 秒。加抖动后各在 ±20% 区间内。

**抖动是双向的**，这一点与很多实现不同：常见的抖动是「只往下抖」或「只往上抖」，这里是 `[0.8, 1.2)`。下行意味着某次重试可能比名义延时更早，这在「避免惊群」的目的下仍然有效（随机化就够），但让「至少等 X 秒」的保证失效。

**限流错误的延时被覆盖。** 两处重试循环（`retry_operation` 与 `with_retry_config`）都有同一段逻辑：若错误是 `RateLimitExceeded` 且带 `retry_delay: Some(d)`，就用 provider 给的延时**替换**配置算出的延时。

这比指数退避更准——provider 知道自己的限流窗口何时重置，它给的秒数是最优等待。配置的曲线只应在 provider 没给出建议时使用。

**`attempts` 的计数语义。** 循环里 `attempts` 从 0 开始，每次决定重试时 `attempts += 1`，然后 `delay_for_attempt(attempts)`。所以第一次重试前 `attempts=1`，延时 1 秒（初始值），符合「第一次重试等 initial_interval」。上限判断是 `attempts < config.max_retries`，所以 `max_retries=3` 意味着最多 3 次重试（总尝试 4 次）。

**凭据刷新是独立的第四条路径。** `with_retry_config` 比 `retry_operation` 多一段前置逻辑：若错误是 `ProviderError::Authentication(_)` 且尚未刷新过，就先 `refresh_credentials()`；成功则 `continue` 重试（**不消耗 `max_retries` 配额**），失败则记警告后落到正常判定。

注释说得很明确：「auth retry is separate from transient-error retries: we get at most 1 credential refresh, independent of max_retries」。所以即使 `max_retries=0`，也有一次凭据刷新机会。

`auth_retried` 布尔保证只做一次，避免刷新成功但凭据仍无效时无限循环。

**退避可被环境变量跳过。** `with_retry_config` 读 `GOOSE_PROVIDER_SKIP_BACKOFF`，`parse::<bool>()` 成功且为真时打一条 info 日志并**不 sleep**。这是一个测试与调试用的逃生口——让重试路径可以快速跑完而不必等真实秒数。

注意 `retry_operation`（自由函数版）**没有**这个开关，只有 trait 方法版有。

**对 `Provider` 的依赖是单向的。** blanket impl 的 `retry_config()` 直接转发到 `Provider::retry_config(self)`——所以「要不要重试」的配置来源是 provider 自己。`refresh_credentials()` 也是 provider 的方法（认证刷新属于 provider 的知识）。

## 关键取舍

**源码事实：** 三层判定的顺序决定了「永久特征串」优先于 `transient_only` 配置。所以这些 Anthropic thinking 错误在任何配置下都不重试。

**源码事实：** `should_retry` 对 `Authentication`、`ContextLengthExceeded`、`InvalidValue`、`UsageError`、`NotImplemented`、`EndpointNotFound`、`CreditsExhausted`、`Refusal`、`ExecutionError` 全部返回 `false`。其中 `CreditsExhausted`（余额耗尽）与 `Refusal`（模型拒答）属于「重试没意义」，`ContextLengthExceeded` 属于「同样的载荷必然同样超限」。

**源码事实：** 抖动区间是 `[0.8, 1.2)`，双向。所以「至少等 X 秒」不成立。

**源码事实：** provider 提供的 `retry_delay` 优先于配置曲线，两者不是相加也不是取大，是替换。

**源码事实：** 凭据刷新不消耗重试配额，且最多一次。`retry_operation` 没有这段逻辑。

**源码事实：** `GOOSE_PROVIDER_SKIP_BACKOFF` 只影响 trait 方法版，不影响自由函数版。

**架构推论：** 「trait + blanket impl」在这里不是抽象的炫耀，而是对象安全的实用解。`Provider` 要保持可装箱，就不能有泛型方法；把泛型重试方法放到 `ProviderRetry` 上，再用 blanket impl 让所有实现自动拥有，两边都满足。代价是 `ProviderRetry` 的 `with_retry_config` 必须被单独特化（无法用默认方法），于是它在 blanket impl 里实现了一次。

**架构推论：** 「永久失败特征串」是这类系统的必要妥协。理想情况下「是否可重试」应该是一个结构化字段，由错误构造者标注。但实际上游 provider 返回的是自然语言消息，SDK 无法从文本判断语义，只能靠字符串匹配。这解释了为什么这组常量长得像一串人工整理的句子，而不是枚举。

**架构推论：** 两条重试路径（自由函数与 trait 方法）的存在本身是一个信号：`retry_operation` 面向「没有 provider 上下文的场景」（如独立的 HTTP 调用），`with_retry_config` 面向「有 provider 可以刷新凭据的场景」。代价是两者会漂移——环境变量开关与凭据刷新都只加在了后者。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/oi-kit-provider-types/src/retry.rs` | `RetryConfig` | 重试参数与 `delay_for_attempt` 曲线 |
| `crates/oi-kit-provider-types/src/retry.rs` | `RetryConfig.delay_for_attempt` | 指数退避封顶加双向抖动 |
| `crates/oi-kit-provider-types/src/retry.rs` | `ProviderRetry` | 泛型重试 trait，保 `Provider` 对象安全 |
| `crates/oi-kit-provider-types/src/retry.rs` | `should_retry` | 三层判定入口 |
| `crates/oi-kit-provider-types/src/retry.rs` | `is_permanent_request_failure` | 特征串加模块判据的永久失败检测 |
| `crates/oi-kit-provider-types/src/retry.rs` | `retry_operation` | 无 provider 上下文的自由函数版重试 |
| `crates/oi-kit-provider-types/src/retry.rs` | `DEFAULT_MAX_RETRIES` | 最多 3 次 |
| `crates/oi-kit-provider-types/src/retry.rs` | `DEFAULT_BACKOFF_MULTIPLIER` | 指数乘子 2.0 |
| `crates/oi-kit-provider-types/src/errors.rs` | `ProviderError` | 错误分类，决定第 1、4 层判定 |

## 参考

- [goose 源码：oi-kit-provider-types/retry.rs](https://github.com/aaif-goose/goose/blob/main/crates/oi-kit-provider-types/src/retry.rs)
- [goose 源码：oi-kit-provider-types/errors.rs](https://github.com/aaif-goose/goose/blob/main/crates/oi-kit-provider-types/src/errors.rs)
- [AWS 架构博客：指数退避与抖动](https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/)

## 与主核 / AI 的关系

主核里最接近的是「存储层重试判定」：错误码决定是否可重试，死锁与临时不可用可以重试，语法错误与约束冲突不能。差别在于主核的判据是**枚举码**（SQLSTATE 的类别），goose 这里第一层也是枚举（`ProviderError` 变体），但**多出第二层**——用消息文本做特征匹配。这一层的存在是因为上游 LLM provider 不提供语义化的错误码，只说人话。

与 smolagents 的重试器（`Retrying` + `is_rate_limit_error`）对比，两者都在「只重试限流」上起步，但复杂度差一个量级。smolagents 的判定是一句字符串匹配（含 `"429"` 等四个片段），重试器是七个参数的手写循环，没有永久失败判定、没有凭据刷新、没有 provider 建议延时。smolagents 面向「本地工具链的单模型」，goose 面向「多 provider 的生产网关」——后者的错误空间大得多，才需要三层判定。

对 AI 工程最实用的一条是「永久失败特征串」这个机制的存在理由。注释里那个 Anthropic thinking 块的例子极为具体：一旦会话中途改了 thinking 配置，签名过的块就不再匹配，而客户端**每次重试都重建同一份载荷**，所以重试次数再多也只是把同一个失败重复 N 次，还多花了 N 个往返。这一类「确定性失败」在任何带签名的协议里都会出现（缓存键、幂等键、内容哈希），识别它们的唯一线索往往只有错误消息——所以硬编码特征串虽是权宜之计，却是可用的唯一办法。

另一条是「provider 建议延时覆盖配置曲线」。这条规则应该成为惯例：限流响应里的 `Retry-After`（或等价字段）永远比客户端猜的指数退避更准，因为只有服务端知道自己的窗口何时重置。把「有建议就用建议、没建议才用曲线」写成显式分支，比让两者相乘或相加都更正确。
