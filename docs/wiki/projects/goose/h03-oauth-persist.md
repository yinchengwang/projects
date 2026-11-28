---
project: goose
feature: oauth-persist
title: OAuth 凭据持久化与形状兼容
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/oauth/persist.rs
    symbols: [PersistedCredentials, GooseCredentialStore]
---

# OAuth 凭据持久化与形状兼容

## 一句话职责

把上游 SDK 的凭据结构包一层、额外带上「这次请求过哪些 scope」，再整体交给配置系统的密钥库；用 `serde(flatten)` 让新旧两种存储形状互相可读。

## 机制是什么

`GooseCredentialStore` 只有一个字段 `name`，实现上游 `CredentialStore` trait 的三个方法：`load`、`save`、`clear`。

内部用的是 `PersistedCredentials`——一个把上游 `StoredCredentials` 加上 `requested_scopes` 的结构：

```rust
struct PersistedCredentials {
    #[serde(flatten)]
    credentials: StoredCredentials,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    requested_scopes: Option<Vec<String>>,
}
```

`flatten` 是这一页最关键的一行：它让 `credentials` 的字段**平铺**进外层对象，而不是嵌套在一个 `credentials` 键下。于是序列化结果既有上游原有的所有键，又多了一个可选的 `requested_scopes`。

密钥的键名由 `secret_key()` 生成：`format!("oauth_creds_{}", self.name)`。所以不同的 OAuth 目标（不同 MCP 服务器、不同服务）各自一份凭据。

## 怎么实现的

**为什么需要 `requested_scopes`。** 上游的 `StoredCredentials` 里有 `granted_scopes`（实际授予的）与 `client_id` 等，但没有「本次请求要求的是哪些」。这两者不同：请求 10 个 scope 但只授予 3 个是常见情况（用户在授权页取消了部分勾选）。

记录「请求过什么」是为了后续能判断「是否需要重新授权」——当功能需要新 scope 时，可以比较「现在的请求」与「上次请求的」，而不是只能看「实际授予的」（后者永远比请求少，无法区分「用户拒绝」与「根本没请求」）。

**`save` 会先读再写。** 这是这段代码里最微妙的一处：

```rust
async fn save(&self, credentials: StoredCredentials) -> Result<(), AuthError> {
    let requested_scopes = self.load_requested_scopes()?;
    self.save_with_requested_scopes(credentials, requested_scopes)
}
```

上游的 `save` 只给凭据，不给 scopes。而 `PersistedCredentials` 是一个整体，写入时会整体覆盖。若不先读回旧的 `requested_scopes`，每次 `save`（例如 token 刷新）都会把它抹成 `None`。

所以这里做的是「读-改-写」：读旧的 scopes，用新的凭据，写回。代价是每次刷新多一次密钥库读取——但这个操作频率极低（token 过期时才有），可以接受。

这也意味着**如果读失败（返回 `Ok(None)`），scopes 会丢失**。`load_persisted` 把所有 `Err` 都映射成 `Ok(None)`，所以「密钥库暂时不可用」与「凭据不存在」在这里不可区分，都会导致 scopes 被清空。这是一个有意的容错方向：宁可丢 scopes（功能降级为「下次重新授权」），也不要让刷新失败。

**`load_persisted` 吞掉所有错误。** `match config.get_secret::<PersistedCredentials>(&key) { Ok(c) => Ok(Some(c)), Err(_) => Ok(None) }`——注意错误分支连日志都不打。所以「密钥库里有这个键但反序列化失败」与「根本没这个键」在调用方看来完全一样。

这与 `save_persisted` 的对比很有意思：写失败会包装成 `AuthError::InternalError`（带具体消息），读失败则安静地当「没有」。这是「读路径宽容、写路径严格」的典型配置——读失败最多让用户重新授权，写失败必须让调用方知道。

**兼容旧形状靠 `flatten` 加 `default`。** 测试里两个用例专门验证这一点：

- `persisted_credentials_read_the_legacy_shape`：把纯 `StoredCredentials` 的 JSON 反序列化成 `PersistedCredentials`，断言 `requested_scopes` 为 `None`。这验证**旧数据能被新代码读**——因为 `flatten` 让旧字段仍在顶层位置，而 `#[serde(default)]` 让缺失的 `requested_scopes` 取默认值。
- `persisted_credentials_remain_readable_as_stored_credentials`：方向相反，把 `PersistedCredentials` 的 JSON 反序列化成纯 `StoredCredentials`，断言上游字段仍能读到。这验证**新数据能被旧代码读**。

两个方向都成立，意味着这份存储格式是**向前向后兼容**的：用旧版本写过、新版本读过再写回，两种客户端都能继续工作。

单靠 `flatten` 不够，还要 `default`——若没有 `default`，缺失 `requested_scopes` 时反序列化会失败。两个属性是配套的。

`skip_serializing_if = "Option::is_none"` 则是为了「没有 scopes 时不要写一个 null 键」——这让写出的 JSON 与旧版几乎一致，正是第二个测试成立的原因。

**`load_requested_scopes` 是公开的额外能力。** 它不属于 `CredentialStore` trait 的一部分，是 goose 自己加的：读回上次请求过哪些 scope。调用方可以在启动时比较「这个连接需要哪些 scope」与「上次请求过哪些」，据此决定是否提示用户重新授权。

**`clear` 的实现是删键。** `config.delete_secret(&key)`，失败包装成 `AuthError::InternalError`。所以退出登录就是把这个命名空间下的那条密钥删掉。

**命名空间的设计。** `oauth_creds_{name}` 这个前缀加名字的键名，让同一个密钥库里可以并存多份 OAuth 凭据（每个 `name` 一份），互相不覆盖。`GooseCredentialStore::new(name)` 让调用方为每个 OAuth 目标建一个实例。

## 关键取舍

**源码事实：** `PersistedCredentials` 用 `#[serde(flatten)]` 把上游凭据平铺，加一个 `#[serde(default)]` 的可选 `requested_scopes`。两个方向的反序列化测试都通过，所以新旧格式互读。

**源码事实：** `save` 先读旧的 `requested_scopes` 再写，否则每次 token 刷新都会把 scopes 抹掉。

**源码事实：** `load_persisted` 把任何错误都映射成 `Ok(None)`，且不记日志。所以「读失败」与「不存在」不可区分。

**源码事实：** 写失败会包装成 `AuthError::InternalError` 并带上原因。

**源码事实：** 密钥键名是 `oauth_creds_{name}`，所以多个 OAuth 目标各自独立。

**架构推论：** `flatten` 在这里的作用不只是「少一层嵌套」，而是**让新旧格式互相可读**。若改成嵌套（`{"credentials": {...}, "requested_scopes": [...]}`），旧代码反序列化 `StoredCredentials` 时会因为多了一个不认识的 `credentials` 键而失败（视 serde 配置而定），旧数据也无法被新代码读。平铺让两个版本共享同一个字段空间，新增字段只是在旁边多一个键。

**架构推论：** 「读-改-写」这个模式在 `save` 里的出现，揭示了 trait 设计的局限：`CredentialStore::save(&self, credentials)` 只接受凭据，没给「附带数据」留位置。于是任何想额外存点东西的实现，都必须在 `save` 里自己去读回。这是「适配第三方 trait」的常见代价——要么放弃额外数据，要么接受一次多余的读。

**架构推论：** 「读宽容、写严格」在这里的具体形态是「读失败当不存在、写失败报错」。这个方向的依据是角色的重要性不同：读失败时系统还能继续（大不了让用户重新授权），写失败则意味着「用户以为保存了但没保存」，必须立刻暴露。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/oauth/persist.rs` | `PersistedCredentials` | 平铺上游凭据并附带请求过的 scope |
| `crates/goose/src/oauth/persist.rs` | `GooseCredentialStore` | 基于配置密钥库的凭据存储实现 |
| `crates/goose/src/oauth/persist.rs` | `GooseCredentialStore.secret_key` | `oauth_creds_<name>` 的命名空间 |
| `crates/goose/src/oauth/persist.rs` | `GooseCredentialStore.load_persisted` | 读取，任何错误都归为「不存在」 |
| `crates/goose/src/oauth/persist.rs` | `GooseCredentialStore.save_persisted` | 写入，失败包装为内部错误 |
| `crates/goose/src/oauth/persist.rs` | `GooseCredentialStore.load_requested_scopes` | 读回上次请求过的 scope |
| `crates/goose/src/oauth/persist.rs` | `GooseCredentialStore.save_with_requested_scopes` | 凭据与 scope 一并写入 |
| `crates/goose/src/oauth/persist.rs` | `CredentialStore::save` | 先读旧 scope 再写，避免被抹掉 |

## 参考

- [goose 源码：oauth/persist.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/oauth/persist.rs)
- [serde 文档：flatten 与 default](https://serde.rs/field-attrs.html)
- [RFC 6749：OAuth 2.0 授权框架](https://datatracker.ietf.org/doc/html/rfc6749)

## 与主核 / AI 的关系

主核里最接近的是「外部认证凭据的持久化」：连接外部系统时的 token 要存下来，且要能在格式演进后仍读得懂旧文件。这里 `flatten` 加 `default` 解决的正是同一类问题——如何在一个既定的序列化形状上追加字段而不破坏兼容。

与 openai-agents 的 `SafeSerializer` 对照很有意思：那边是「白名单 + 显式标记 + 不可逆则拒绝」，这边是「struct 序列化 + 平铺 + 默认值」，宽松得多。差别在于数据来源：`SafeSerializer` 的输入可能来自沙箱（不完全可信），而这份凭据数据来自自己写的密钥库（可信）。对可信数据，serde 的 struct 反序列化已经足够安全——它不会像 pickle 那样实例化任意类。

与 smolagents 的 `SafeSerializer` 对比，同样是「可信度决定严格度」的例子。smolagents 的 dataclass 编码「能出去回不来」，因为它不 import 原类；这里的 `flatten` 反而保证了「能出去也能回来」——因为双方都是同一份 `StoredCredentials` 定义。

对 AI 工程最实用的一条是那个「读-改-写」的必要性。当你要给一个第三方 trait 的接口「加一点自己的数据」时，若该接口的写方法只接收部分数据，就必须在写前读回其余部分。这里的具体教训是：**token 刷新会频繁触发 `save`**，如果没有这段读回逻辑，用户的 scope 记录会在第一次刷新后就永久丢失，而症状是「授权页反复弹出」——一个很难归因到「序列化细节」的问题。

另一条是「双向兼容测试」。两个测试各验一个方向：旧数据新代码能读、新数据旧代码能读。做存储格式演进时，只测一个方向是不够的——你改了写路径但没改读路径时，「新写旧读」会失败而「旧写新读」会通过，只测一个方向就发现不了。这套「两个方向各一个测试」的做法值得直接借鉴。
