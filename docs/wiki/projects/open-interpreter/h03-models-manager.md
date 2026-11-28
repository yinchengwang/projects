---
project: open-interpreter
feature: models-manager
title: 模型目录与缓存
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/models-manager/src/manager.rs
    symbols: [ModelsManager, RefreshStrategy, DEFAULT_MODEL_CACHE_TTL]
  - path: codex-rs/models-manager/src/cache.rs
    symbols: [ModelsCache, FileModelsCache, ModelsCacheEntry]
---

# 模型目录与缓存

## 一句话职责

把「这台机器现在能用哪些模型」变成一个可缓存、可校验的答案：每次采纳远端目录前都先比一遍「凭据身份」，并把「新鲜度」与「身份」两个维度分开判——身份不对的缓存直接当未命中，而不是当过期。

## 机制是什么

三个常量定下全貌：`MODEL_CACHE_FILE = "models_cache.json"`（磁盘文件名）、`DEFAULT_MODEL_CACHE_TTL = 300 秒`、`DEFAULT_PROVIDER_CACHE_ID = "openai"`。

`RefreshStrategy` 三值，每个 doc 都短而明确：

- `Online` —— 「总是从网络拉取，忽略缓存」。
- `Offline` —— 「只用缓存，从不联网」。
- `OnlineIfUncached` —— 「缓存可用且新鲜就用，否则联网」。

`as_str()` 给出 `"online"` / `"offline"` / `"online_if_uncached"`，并实现了 `Display` —— 所以这三值是会被打到日志或指标标签里的。

`ModelsCacheEntry` 五字段：`fetched_at`（拉取或最近一次重新校验的时间）、`etag`、`client_version`、`identity`、`models`。后四个全是 `#[serde(default, skip_serializing_if = "Option::is_none")]`（`models` 除外，它有一个自定义 `deserialize_with`）。

`ModelsCache` 是缓存后端的 trait：`load(client_version)`、`store(entry)`、`refresh_ttl(client_version, identity, etag)`。`FileModelsCache` 是内置的文件实现，两字段：`cache_path` 与 `cache_ttl`。

而 `ModelsManager` 是更上层的抽象，有 `OpenAiModelsManager`（远端 + 缓存 + 内置）与 `StaticModelsManager`（纯内存）两个实现。

## 怎么实现的

**身份是三处校验的核心判据。** `ModelsEndpointClient::identity()` 的 doc 把这个契约写得极硬：

「当前 provider 与凭据的不透明身份，**不解析认证**就必须能给出。它必须在 account、email、plan、provider 或 API 凭据变化时改变。**恒等不可用时返回 `None`；此时缓存复用被禁用**」。

三句话对应三个设计决定：

1. 「不解析认证」——`identity()` 必须是同步可得的。若它需要 await 认证解析才能给出，那每次判缓存都要等一次网络。
2. 「必须在这些变化时改变」——所以 identity 是一个**指纹**，包含了够多的维度。
3. 「`None` 就禁用复用」——所以「无法确认自己是谁」时不信任任何缓存。

这条规则在 `get_remote_models` 里被执行：

```rust
if entry.identity.is_some() && entry.identity == self.endpoint_client.identity() {
    entry.models.clone()
} else {
    self.base_models.clone()
}
```

注意那个双条件：`entry.identity.is_some()` 与相等。若当前身份是 `None`，那么 `entry.identity == None` 可能为真（当 entry 也没身份时），所以必须显式排除「entry 没身份」这种情况。`ModelsCacheEntry.identity` 的 doc 正好补上另一半：「不透明 provider 与认证身份。**未带作用域的遗留条目是缓存未命中**」。

所以「老版本的缓存文件」与「当前身份未知」都会落到 `base_models`（内置目录），而不是拿一份不可信的数据。

`try_load_cache` 里的校验更密：

```rust
if cache_entry.identity.as_ref() != Some(&identity) || self.endpoint_client.identity().as_ref() != Some(&identity) {
    info!("models cache: provider or auth identity mismatch");
    return false;
}
```

它比了**两次**当前身份。第一次与缓存里的比，第二次与最开始取到的 `identity` 比。第二次的必要性是防「在 await cache.load 的这段时间里身份变了」——若变了，那么用旧身份做的第一次比较虽然通过了，但结果已不适用于新身份。

**`apply_remote_models` 是「发布前再验一次身份」。** 它的 doc 一句话概括：「只在请求身份仍然匹配时才发布，**包括异步存储之后**」。

实现里它先拿写锁，然后 `if entry.identity != self.endpoint_client.identity() { return false; }`——这里比较的是 `Option<String>` 与 `Option<String>`，所以 `Some == None` 为假、`None == None` 为真。后者允许「身份不可用的 provider」发布缓存（因为对它来说 `None` 就是它的真实身份）。

接着是一个分支，判「远端目录是否权威」：

```rust
let remote_only = entry.models.iter().any(|m| m.visibility == ModelVisibility::List)
    && (self.supports_api_key_discovery() || auth_manager.auth_mode().is_some_and(AuthMode::has_chatgpt_account));
```

`remote_only` 为真时**直接用远端目录替换**（连内置都不合）。为假时把远端模型与 `base_models` 合并——按 slug 定位，存在就 `merge_provider_catalog_model`，否则 push。最后若 `enrich_with_compatibility_catalog` 为真再跑一遍 `apply_compatibility_catalog`。

所以「权威与否」的判据是两个条件的与：远端目录里有对外可见的模型（`visibility == List`），且当前认证是「API key 发现」或「有 ChatGPT 账号」。这解释得通——只有这两类凭据的目录才是「用户实际能用的全部」；其他情况下远端目录只是补丁，需要与内置目录合并。

**`merge_provider_catalog_model` 是单向覆盖。** 实现：

```rust
if existing.visibility == List && remote.visibility != List {
    // 保留 existing，只从 remote 接 context_window 与 max_context_window
} else {
    remote  // 否则远端整条赢
}
```

所以「本地认为该列出、远端说不该列出」时，保留本地可见性但采纳远端的窗口大小。其余情况远端整条覆盖。这条规则保护的是「内置目录中标记为对外可见的模型不会被远端悄悄隐藏」。

**新鲜度判定的三道守卫。** `ModelsCacheEntry::is_fresh(ttl)`：

1. `if ttl.is_zero() { return false; }` —— 零 TTL 表示永不新鲜。
2. `chrono::Duration::from_std(ttl)` 失败（超出范围）也返回 `false`。
3. `age = Utc::now().signed_duration_since(fetched_at)`，`age <= ttl_duration`。

第 3 步用 `signed_duration_since` 而不是 `duration_since`，所以 `fetched_at` 在未来时会得到负数年龄——那会被判为新鲜。这是一个时钟回拨时的宽容方向。

`is_fresh` 是私有方法，说明它只在 `FileModelsCache` 内部用。所以「新鲜」这个判据只属于文件缓存后端——自定义后端可以自己定。这与 `ModelsCache::load(client_version)` 签名里没有 `ttl` 是一致的：TTL 是后端自己的配置。

**`refresh_ttl` 是一个只改时间的写操作。** `refresh_if_new_etag` 在「身份相同且 etag 相同」时走这条路：

```rust
cache.refresh_ttl(&client_version, &identity, &etag).await
```

所以「远端告诉我们 etag 没变」时不必重传目录，只把 `fetched_at` 推到现在。这是 ETag 机制的标准用法，但这里额外要求身份相同——否则给另一个账号的缓存刷 TTL 会让它一直新鲜下去。

**`should_refresh_models` 决定「能不能联网」，与策略无关。** 三条件之一成立即可：

- `uses_codex_backend()` —— 走 Codex 后端。
- `has_command_auth()` —— 有命令式认证。
- `supports_api_key_discovery()` —— 支持 API key 发现。

`supports_api_key_discovery` 又是三条件的与：`supports_api_key_models() && !has_command_auth() && uses_api_key_auth()`。

注意 `!has_command_auth()` 在两个地方都以否定形式出现。第一个地方在 `should_refresh_models` 是「或」的一项（命令式认证可以直接刷新），第二个在 `supports_api_key_discovery` 是「且」的一项（有命令式认证时不算 API key 发现）。所以命令式认证是一个**独立的**类别，不与 API key 发现叠加。

**`refresh_available_models` 的第一段是「API key 发现的门闸」。** 实现：

```rust
if self.uses_api_key_auth() && !self.endpoint_client.has_command_auth()
    && (!self.endpoint_client.supports_api_key_models() || !self.api_key_model_discovery_enabled.load(SeqCst))
{
    return Ok(());
}
```

注释解释了它：「API key 发现必须在复用远端目录之前被启用且被支持。否则即使是更早运行留下的匹配缓存也会绕过 bundled-only 行为。命令式认证的 provider 保持既有发现行为」。

所以这是一个**提前返回**，不只是「不刷新」——它连缓存都不加载。所以「API key 认证 + 未启用发现」时，系统完全停留在内置目录。这个行为的动机很明确：用 API key 的用户若没显式开启「发现远端模型」，就不该看到一串他从别处听来的模型名。

接着 `if !self.should_refresh_models().await` 时按策略决定是否 `try_load_cache()`：`Offline` 与 `OnlineIfUncached` 会加载缓存，`Online` 不加载（因为它要的是网络结果）。然后返回 `Ok(())`。

再往下才是策略分发：`Offline` → 只加载缓存；`OnlineIfUncached` → 缓存命中就返回，否则 `fetch_and_update_models`；`Online` → 直接 `fetch_and_update_models`。

**`fetch_and_update_models` 在写入前又验一次身份。** 它从端点拿到 `ModelsEndpointResponse`（含 `models`、`etag`、`identity`），然后：

```rust
if Some(&identity) != self.endpoint_client.identity().as_ref() { return Ok(()); }
```

即「端点报回来的身份」与「客户端现在的身份」不同就直接放弃——不写缓存也不发布。这条检查防的是「请求发起后认证被换掉了」：那种情况下拿到的目录属于旧身份，写进缓存会污染新身份的条目。

之后构造 `ModelsCacheEntry`（`fetched_at: Utc::now()`、`client_version: Some(...)`、`identity: Some(identity)`），写缓存（失败只记 error 不中断），再 `apply_remote_models(entry)`。

注意写缓存与发布的顺序：**先写盘、后发布**。所以崩溃在发布前会留下「盘上有、内存里没有」的状态，下次启动会通过 `try_load_cache` 补上。反过来若先发布后写盘，崩溃会留下「内存里有、盘上没有」，重启后丢失。

**`refresh_after_auth_change` 是「尽力而为 + 超时 + 三重早退」。** 它在 `tokio::time::timeout(5s, refresh)` 里跑，且失败只 warn：「模型目录在认证变化后的刷新失败或超时」。

内层 `refresh` 有三个早退：

1. `if !self.should_refresh_models().await { return Ok(()); }` —— 不能联网就不刷。
2. `let identity = self.endpoint_client.identity();` —— 拿当前身份。
3. `if identity.is_some() && self.remote_models.read().await.identity == identity { return Ok(()); }` —— 手头已有同一个身份的目录就不刷。

第 3 条的第一个条件是 `identity.is_some()`：身份不可用时**不能**用「两个都是 None」来判「已经有了」。这与前面 `get_remote_models` 的双条件同理。

注释还说明了为什么要先解析认证：「在比对目录身份之前先解析惰性命令凭据」。所以 `identity()` 虽然在契约上是同步的，实际可能要等「惰性命令凭据」被解析过才有值——这就是为什么在比对前先问一次。

**`list_models_from_cache_if_present` 是纯缓存的入口。** 它不做网络请求：`try_load_cache()` 成功就 `build_available_models(...)` 返回，失败就 `None`。所以它回答的是「如果你有缓存，给我用它算出的预设列表」。

**`try_load_cache` 的四道检查。** 除前面讲的两次身份比对，还有：

- `cache.load` 返回 `Ok(None)` → 记 info「没有可用的缓存条目」→ false。
- `Err` → 记 error → false。
- `client_version` 不匹配 → 记 info 带 `expected_version` 与 `cached_version` → false。

版本检查的理由写在 `ModelsCacheEntry.client_version` 的 doc：「模型管理器会拒绝该值缺失或与它当前版本不同的条目」。所以缓存按客户端版本严格分桶——不同版本的客户端可能有不同的模型元数据 schema，混用会读出错误的结构。

而 `models` 字段的 `deserialize_with = deserialize_model_infos_with_legacy_base` 是另一个兼容点：老版本写出的模型数组可能用旧的基础字段形态，这个自定义反序列化能读它。

## 关键取舍

**源码事实：** `identity()` 必须同步可得（契约明确「不解析认证」），且身份不可用时 `None` 表示禁用缓存复用。

**源码事实：** 判「缓存属于我」的所有地方都用了双条件（`is_some()` 且相等），因为 `None == None` 会误判。

**源码事实：** `try_load_cache` 比了两次身份，第二次防「await 期间身份变了」。

**源码事实：** 缓存条目按 `client_version` 严格分桶，版本不符直接当未命中。

**源码事实：** `refresh_ttl` 只推 `fetched_at`，不做数据传输，且要求身份与 etag 都相同。

**源码事实：** 写盘先于内存发布。

**源码事实：** API key 认证 + 未启用发现时，`refresh_available_models` 提前返回，连缓存都不加载。

**架构推论：** 「新鲜度」与「归属」被当成两个正交维度处理，是这一层的核心手法。多数缓存实现只判过期时间，把「这条缓存是谁的」隐含在「缓存文件只有一个」这个前提里。一旦有多个身份复用同一个缓存位置（多账号、多 provider、API key 与 OAuth 切换），这个前提就破了。把身份做成指纹并逐处校验，是让「多个身份共用一个存储」成立的必要条件。

**架构推论：** 在多个 `await` 点前后重复校验身份，看起来是过度防御，实际是这类「跨越异步边界的身份敏感操作」的标准做法。判据很简单：**任何在 await 前观察到的事实，在 await 后都可能已经过期**。这段代码在三个位置做了这件事（`try_load_cache` 的双比、`fetch_and_update_models` 的回报比、`apply_remote_models` 的发布比），每一处都对应一个真实的 await 边界。

**架构推论：** 「权威目录」与「补丁目录」的区分，本质上是「这组凭据能不能看到全部模型」。判据用 `visibility == List` 加上凭据类型，是一个务实的选择：它不需要知道后端的授权模型，只需要知道「这条目录里有没有对外可见的条目」以及「凭据属于哪一类」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/models-manager/src/manager.rs` | `ModelsManager` | 模型管理总契约，含缓存与刷新策略入口 |
| `codex-rs/models-manager/src/manager.rs` | `RefreshStrategy` | 在线 / 离线 / 缓存优先三策略 |
| `codex-rs/models-manager/src/manager.rs` | `DEFAULT_MODEL_CACHE_TTL` | 300 秒默认缓存有效期 |
| `codex-rs/models-manager/src/manager.rs` | `MODEL_CACHE_FILE` | `models_cache.json` 文件名 |
| `codex-rs/models-manager/src/manager.rs` | `ModelsEndpointClient` | 端点抽象，`identity()` 是缓存判据之源 |
| `codex-rs/models-manager/src/manager.rs` | `OpenAiModelsManager` | 内置目录加缓存加远端端点 |
| `codex-rs/models-manager/src/manager.rs` | `StaticModelsManager` | 纯内存的权威目录实现 |
| `codex-rs/models-manager/src/manager.rs` | `refresh_available_models` | API key 门闸与策略分发 |
| `codex-rs/models-manager/src/manager.rs` | `fetch_and_update_models` | 回报身份校验、先写盘后发布 |
| `codex-rs/models-manager/src/manager.rs` | `apply_remote_models` | 发布前再验身份，决定权威或合并 |
| `codex-rs/models-manager/src/manager.rs` | `try_load_cache` | 版本、身份、双身份的依次校验 |
| `codex-rs/models-manager/src/manager.rs` | `merge_provider_catalog_model` | 保留本地可见性、只采纳远端窗口 |
| `codex-rs/models-manager/src/manager.rs` | `refresh_after_auth_change` | 5 秒超时的尽力刷新与三重早退 |
| `codex-rs/models-manager/src/cache.rs` | `ModelsCache` | 缓存后端契约，含 `refresh_ttl` |
| `codex-rs/models-manager/src/cache.rs` | `FileModelsCache` | 文件后端的路径与 TTL |
| `codex-rs/models-manager/src/cache.rs` | `ModelsCacheEntry` | 目录本体加版本、身份、etag、时间 |
| `codex-rs/models-manager/src/cache.rs` | `ModelsCacheEntry.is_fresh` | 零 TTL 与超范围 TTL 都判不新鲜 |
| `codex-rs/models-manager/src/cache.rs` | `ModelsCacheError` | 不暴露后端错误类型的消息型错误 |

## 参考

- [open-interpreter 源码：models-manager/src/manager.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/models-manager/src/manager.rs)
- [open-interpreter 源码：models-manager/src/cache.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/models-manager/src/cache.rs)
- [RFC 9110：ETag 与条件请求](https://www.rfc-editor.org/rfc/rfc9110.html#name-etag)

## 与主核 / AI 的关系

主核里最接近的是「元数据缓存」：表结构、统计信息、索引可用性都从系统表读出后缓存在内存里，且必须在「这是同一份数据」的前提下才敢用。这一页把「同一份数据」做成了身份指纹，与主核用「事务 id / 代际号」判缓存有效性是同一职责。

与 goose 的 g02（模型目录回落）对照很有意思：那边用 `OnceLock<Shared<BoxFuture>>` 做「下载一次、多处共同 await」，且缓存加载失败只记警告；这边用「身份指纹 + 版本 + TTL」三重判据，且**任何不匹配都当未命中**而非降级。差别在于两边的缓存位置不同：goose 的目录是进程内单例（不存在多身份共存），这里要支持多账号共用同一个磁盘文件。

与 smolagents 的 `Model` 构造相比，那边根本没有模型目录这个概念——模型名是调用方给的字符串，能力靠试。这里必须维护一份目录，因为「用户该看到哪些模型可选用」是一个需要权威答案的产品问题。

对 AI 工程最实用的一条是「缓存归属比缓存新鲜度更容易出错」。几乎每个缓存实现都会判过期时间，但很少判「这条是我的吗」。一旦出现多身份共享（多账号、多环境、API key 与 OAuth 混合），身份判据的缺失就会表现为「登录另一个账号后看到上一个账号的模型列表」——一个很隐蔽且看起来像后端 bug 的现象。做法很直接：给缓存条目加一个不透明身份字段，**在不匹配时当未命中而不是当过期**。

另一条是「跨 await 边界重复校验」。这段代码在三个位置重复做了同一件事，每次都在一个 await 之后。如果只在一开始验一次，那么「请求发起后认证被换掉」这个窗口就会把旧身份的目录写进新身份的缓存。判据可以总结成一句话：若某个事实会影响「这份数据该归谁」，那么每次跨越 await 之后都要重新确认它。

最后一条是 `refresh_ttl` 这个「只改时间不改内容」的小接口。ETag 机制下，最常见的浪费是「远端说没变，客户端还是下了整个目录」。把「确认未变」与「确认已变」分成两条路径，前者只做一次元数据写入，是让缓存真正省钱的关键。
