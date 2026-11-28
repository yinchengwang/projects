---
project: goose
feature: model-fallback
title: 模型配置补全与目录回落
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/model_catalog.rs
    symbols: [initialize, CATALOG_URL]
  - path: crates/goose/src/model_config.rs
    symbols: [ModelConfig]
---

# 模型配置补全与目录回落

## 一句话职责

把「用户只给了一个 provider 名与模型名」逐步补全成一个可用的模型配置：先用本地缓存与远端目录填上能力位（上下文上限、视觉支持、推理默认），再把环境变量与全局配置叠上去，最终得到一个每个字段都有明确来源的配置对象。

## 机制是什么

两件事并行：**目录侧**负责「这个模型有什么能力」，**配置侧**负责「这次运行要用什么参数」。

目录侧的 `model_catalog.rs` 极短，只有三件事：常量 `CATALOG_URL = "https://models.dev/api.json"`、`initialize()`、`wait_for_download()`。它的做法是把远端目录整份拉下来，落进本地缓存目录（`Paths::in_data_dir("model_catalog")`）。

配置侧的 `ModelConfig` 是来自 `oi-kit-providers` 的共享类型，本仓的 `model_config.rs` 只做「怎么从用户输入构造它」。

## 怎么实现的

**目录初始化的三段结构。** `initialize()` 先同步调 `load_cached_catalog(&cache_dir)`，失败只记一条警告（不让坏缓存阻断启动），然后 `tokio::spawn` 一个后台任务去 `refresh(cache_dir)`，最后把 spawn 得到的句柄转成一个 `OnceLock<Shared<BoxFuture<'static, ()>>>`。

这个 `Shared` 是关键：`futures::future::Shared` 让一个 future 可被多次 clone 并共同 await 同一份结果。于是任意多处都能 `wait_for_download()` 等**同一次**下载完成，而不是各自触发一次下载。

`OnceLock` 保证只初始化一次。所以「第一次调 `initialize()` 决定下载目标，后续调用装不进去也不报错」（`let _ = DOWNLOAD.set(...)`）。

**为什么只有解析走 blocking 池。** `refresh` 里有一句注释值得完整对待：解析目录慢到足以卡住运行时的定时器，所以只有 `catalog.install(&cache_dir)` 这一步放进 `spawn_blocking`，而下载本身留在异步路径上。

注释还说明了另一个原因：这样「关掉运行时时仍能取消下载而不是等它完成」。如果把整段放进 blocking 池，shutdown 就必须等那个线程跑完（因为 blocking 线程不可取消）；只把 CPU 密集的解析放进去，网络部分仍可被取消。

这是一个关于「哪些工作放 blocking 池」的精确判断：不是「所有慢活」，而是「不可取消的 CPU 工作」。

**`fetch_remote_catalog` 返回 `Option`。** `refresh` 里写成 `let Some(catalog) = fetch_remote_catalog(...).await? else { return Ok(()) };`——所以「远端不可用」不是错误，只是「本次没有新目录」。这与缓存加载的对待方式一致：目录是**增强**而非必需。

**配置补全是一条串行的 builder 链。** `model_config_from_user_config` 只是两步：`base_model_config_from_user_config` 建一个带模型名与若干全局默认的空配置，然后 `materialize_model_config` 补全并施加目录限制。

`materialize_model_config_inner` 里逐项补默认值，每一项都遵循同一个模式——**「若未设置则用全局配置」**：

- `temperature` 未设 → 读 `GOOSE_TEMPERATURE`（且若为负数直接报错）。
- 若开了 `toolshim` 但没设 `toolshim_model` → 读 `GOOSE_TOOLSHIM_OLLAMA_MODEL`（若设为空串则报错）。
- `max_tokens` 未设 → `with_default_max_tokens(config.get_goose_max_tokens()?)`（注意这个方法自身也判 `is_none`，所以是双重保护）。
- 可选地设 `thinking_effort`（`include_default_thinking_effort` 参数决定是否设，两个调用点分别是 `true` 与 `false`）。
- `cache_ttl` 未设 → 读 `GOOSE_CACHE_TTL`（只接受 `5m` 或 `1h`，其他值报错）。

最后若 provider 是 OpenAI，再 `apply_openai_request_params`——把 `GOOSE_OPENAI_STORE` 之类的开关并进 `request_params`。

**「目录限制」在最后。** `apply_canonical_limits` 做两件事：`with_canonical_limits(provider_name)` 与 `with_canonical_vision_support(provider_name)`。从 `oi-kit-providers` 的实现看，前者在三个字段未设置时用目录值填：`max_tokens`（取目录的输出上限，但**要求它小于上下文上限**）、`reasoning`、`supports_vision`（看目录的输入模态是否含 Image）。

「输出上限小于上下文上限」这个过滤很实际：目录里偶尔会把输出上限写得等于或大于上下文窗口，那样设置给 API 会被拒。加一层 `<` 的比较就把这类脏数据挡住了。

`apply_canonical_limits` 对 Azure Foundry 走**短路跳过**。这是唯一一个被排除在目录补全之外的 provider——因为它的模型名空间与上游目录不对应（同一模型在 Azure 上由不同 publisher 提供，能力集不同）。

**两个视图的差异。** `materialize_model_config`（给完整配置用）与 `materialize_model_config_inner(..., false)`（在 `_with_session_settings` 路径用）的区别是**是否设默认 thinking effort**。

带会话设置的版本（`model_config_from_user_config_with_session_settings`）先调 inner（`false`，不设默认 effort），再 `.with_inherited_session_settings_from(previous, request_params)`，再 `.with_default_thinking_effort(config.get_goose_thinking_effort())`。

顺序有意义：先从上一个配置继承会话相关的参数（只继承 `INHERITED_SESSION_PARAM_KEYS` 白名单里的键，且用 `or_insert` 不覆盖已有值），再把自己的 `request_params` 合并进来，最后才设默认 effort。所以「用户显式选过的 effort 不会被默认值覆盖」——`with_default_thinking_effort` 的实现注释也印证了这点：它判断的是**原始参数是否存在**而不是能否解析，因为持久化进来的 `"default"` 这种值也代表用户显式选择。

**`cache_ttl` 的特殊处理。** `with_rederived_cache_ttl` 先 `without_cache_ttl` 再按当前配置重设。docstring 写清了理由：TTL 是配置状态而非会话状态，恢复的会话应当反映用户**当前**的选择，而不是早些时候（可能已被 clamp 过的）某次运行存下的值。

配套还有 `with_cache_ttl_clamped`（在 `oi-kit-providers` 里）：把非 `5m` 的 TTL 压回 `5m`，注释说明是为「单次爆发型工作负载」准备的——headless 运行、子智能体、定时任务都不会空闲下来，用户级 1h 选择在这些场景里只会白付 2 倍的缓存写入溢价。

**`_context_limit` 参数被忽略。** `model_config_from_user_config_with_session_settings` 有个 `_context_limit: Option<usize>` 参数，下划线前缀即「刻意未用」。这是签名预留——调用方可能已经传了，但实现目前不用它。

## 关键取舍

**源码事实：** `initialize()` 对缓存加载失败与下载失败都只记警告，不传播错误。目录是增强，不是必需。

**源码事实：** 只有 `catalog.install` 走 `spawn_blocking`，下载留在异步路径。注释明确说是为了「shutdown 时仍能取消下载」。

**源码事实：** `DOWNLOAD` 是 `OnceLock<Shared<BoxFuture>>`，所以下载只发起一次、结果可被多处共享 await。

**源码事实：** Azure Foundry 被排除在 `apply_canonical_limits` 之外。

**源码事实：** 目录给出的 `max_tokens` 只有在「小于上下文上限」时才被采用。

**源码事实：** `with_default_thinking_effort` 判的是「原始参数存在与否」而非「能否解析」，所以无法解析的值也算显式选择。

**架构推论：** 「目录是增强」这条定位决定了整个错误处理策略。如果目录是必需的，加载失败就该报错让用户修；既然它只是让配置更准的辅助，任何失败都应该降级到「用内置默认值」。这也解释了为什么远端拉取返回 `Option` 而不是 `Result`——「没拉到」与「拉到了空的」在这个定位下是同一件事。

**架构推论：** 那串「未设则读全局」的链条实际上定义了一份**优先级表**：显式配置 > 全局配置 > 目录推断 > 类型内置默认。目录排在全局配置之后而不是之前，是因为全局配置是用户意图的直接表达，而目录是第三方数据。`apply_canonical_limits` 放在最后，正好对应这个次序。

**架构推论：** 把「哪些工作放 blocking 池」判断得这么细，是因为 Rust 异步运行时对阻塞很敏感——一个 CPU 密集的解析会让整个运行时的定时器停摆。但这个代价只在「运行时需要保持响应」时才是问题，所以注释里那条「shutdown 能取消下载」才是真正的决策依据。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/model_catalog.rs` | `initialize` | 加载缓存并后台刷新远端目录 |
| `crates/goose/src/model_catalog.rs` | `CATALOG_URL` | 远端目录地址常量 |
| `crates/goose/src/model_catalog.rs` | `wait_for_download` | 共享同一次下载的完成事件 |
| `crates/goose/src/model_config.rs` | `ModelConfig` | 共享的模型配置类型 |
| `crates/goose/src/model_config.rs` | `materialize_model_config_inner` | 逐项补默认值的串行链 |
| `crates/goose/src/model_config.rs` | `apply_canonical_limits` | 用目录填上限与视觉支持，Azure 除外 |
| `crates/goose/src/model_config.rs` | `model_config_from_user_config_with_session_settings` | 继承会话参数后再设默认 effort |
| `crates/goose/src/model_config.rs` | `with_rederived_cache_ttl` | 丢弃已存 TTL 并按当前配置重算 |
| `crates/oi-kit-provider-types/src/model.rs` | `with_canonical_limits` | 目录值填充三字段，含输出上限过滤 |

## 参考

- [goose 源码：model_catalog.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/model_catalog.rs)
- [goose 源码：model_config.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/model_config.rs)
- [models.dev 模型目录](https://models.dev/)

## 与主核 / AI 的关系

主核里最接近的是「代价模型与统计信息的加载」：优化器要估计行数、页数、索引可用性，而这些数据来自统计表——若统计缺失，优化器退回启发式默认值继续执行，而不是报错。这里目录的定位完全一致：它让模型配置更准（知道真实上下文上限与是否支持视觉），但缺失时用内置默认也能跑。

与 smolagents 的模型层相比，两者的「配置补全」思路正好相反。smolagents 的 `Model` 基类把参数合并做成三维优先级（`self.kwargs` > 显式 kwargs > 具体参数），全部来自**调用方**；goose 这边则大量从**全局配置与环境变量**取默认值，并用远端目录做最后的能力推断。差别在于使用场景：smolagents 是库，调用方自己知道自己要什么；goose 是长驻服务，同一份代码要服务多个用户与会话，所以必须有「用户没说的从哪儿补」的完整链条。

对 AI 工程最实用的一条是 `Shared<BoxFuture>` + `OnceLock` 的组合用法。任何「只该做一次、但多处都要等它完成」的异步初始化（目录下载、凭据发现、配置迁移）都可以用这个模式：`OnceLock` 保证发起一次，`Shared` 保证多处共同 await，而不是每处都写「检查是否已完成、没有就去触发」。两个原语各自解决一个问题，组合起来正好覆盖这类需求。

另一条是「TTL 是配置状态不是会话状态」这个判断。会话持久化最容易犯的错误是把「当前生效的参数」也存进去，然后在恢复时把它当成会话状态沿用。区分方法很简单：问一句「这个值会不会因为用户改了设置而变化」——会，就是配置状态，恢复时必须重新读；不会（例如这次对话里的中间结果），才是会话状态。`with_rederived_cache_ttl` 就是这条原则的具体实现。
