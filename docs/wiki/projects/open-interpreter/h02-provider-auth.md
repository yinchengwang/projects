---
project: open-interpreter
feature: provider-auth
title: 提供方认证与工作区路由
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/model-provider/src/auth.rs
    symbols: [ResolvedProviderAuth, ProviderAuthScope, AuthManagerAuthProvider]
  - path: codex-rs/model-provider/src/workspace_routing.rs
    symbols: [WorkspaceRoutingContext, ACCOUNT_ROUTING_HEADER, apply_workspace_routing]
---

# 提供方认证与工作区路由

## 一句话职责

按「环境变量 / 实验性 token → 密钥库 OAuth → agent 身份 → auth manager 快照」的顺序解析出一份可用的认证，并把「账号可能被路由到另一个后端」这个事实做成请求头加 origin 替换——且替换前后都能证明请求没跑到别的地方去。

## 机制是什么

`ResolvedProviderAuth` 是解析结果，两字段：`auth: SharedAuthProvider`（真正会往请求头里加东西的对象）与 `agent_identity_telemetry: Option<AgentIdentityTelemetry>`（仅当走了 agent 身份路线时才有）。它 `#[derive(Clone)]` 但不含 `Debug`。

`ProviderAuthScope` 三字段，全部是「这次请求在什么语境下」：`agent_identity_policy`、`session_source`、`agent_identity_session_fallback`。所以它是一个**语境包**，而不是凭据包——凭据在 `ResolvedProviderAuth` 里。

`AuthManagerAuthProvider` 是一个 `AuthProvider` 实现，两字段：`auth_manager: Arc<AuthManager>` 与 `expected_auth: CodexAuth`。第二字段上有条注释解释了它的地位：「启动时的认证只是账号作用域的身份锚点。请求头**总是**来自下面的当前 AuthManager 快照」。

`ACCOUNT_ROUTING_HEADER = "x-openai-account-routing-override"`——这是工作区路由的请求头名。

`WorkspaceRoutingContext` 三字段：`chatgpt_base_url: String`、`previously_routed: Mutex<bool>`、`session: Option<Arc<WorkspaceRoutingSession>>`。doc 讲清了语义：「一个会话的 provider 的路由作用域。一旦被路由，它不能仅因为账号所有者的引导配置变化就变得独立。查找与第一次成功的路由切换按会话串行化」。

## 怎么实现的

**认证解析是一条有优先级的链，每一级都有具体的失败形态。** `resolve_provider_auth(auth, provider, codex_home)` 的顺序是：

1. `bearer_auth_for_provider(provider)`——尝试从 provider 配置里拿 bearer（环境变量或 `experimental_bearer_token`）。
2. `Ok(Some(auth))` → 直接返回。
3. `Ok(None)` → 落到下一级。
4. `Err(err)` → **不是立刻失败**，而是先试 Kimi Code 的 OAuth：

```rust
return match kimi_code_bearer_token(provider, codex_home).await? {
    Some(token) => Ok(Arc::new(BearerAuthProvider::new(token))),
    None => Err(err),
};
```

注释解释了这条兜底：「Kimi Code provider 在没有 API key 可用时，可以用存储的 OAuth 凭据认证」。

所以「配置里找不到 bearer」这个错误被当作「可能只是供应商不同」而不是「认证不可用」。这是一个很具体的宽容——只对 Kimi Code 生效，其他 provider 仍然直接返回原错误。

`kimi_code_bearer_token` 本身的三层早退很干净：不是 Kimi Code → `Ok(None)`；没有 codex home → `Ok(None)`；`MissingCredentials` → `Ok(None)`；其他错误 → `Fatal`。所以「没配」与「配了但坏了」被严格分开：前者回落到上一级的错误，后者直接 Fatal。

**`resolve_provider_auth_without_bearer` 的三个判定。** 走到这里说明没有 bearer，那么：

```rust
if !provider.requires_openai_auth && provider.auth.is_none() {
    return Ok(unauthenticated_auth_provider());
}
```

第一判定：不需要 OpenAI 认证且没配 `auth` → 返回一个什么都不做的 `UnauthenticatedAuthProvider`。那个结构的注释说明了它的存在理由：「有些 provider 是故意不发认证头的。例如本地 OSS provider 与 `requires_openai_auth = false` 的自定义测试 provider」。

第二判定：Bedrock 的两种 API key 形态直接 `UnsupportedOperation`——因为「Bedrock API key 认证只被 Amazon Bedrock model provider 支持」。

第三判定：有 `auth` 就 `auth_provider_from_auth(auth)`，否则仍然 `unauthenticated`。

所以这条路径的最终默认值是「无认证头」而不是「报错」。这是一个安全方向上的选择吗？不完全——「静默不发认证头」若用在一个需要认证的后端上，会得到一个 401，而 401 会在上层触发恢复流程。所以错误依然会显现，只是延后一层。

**`resolve_provider_auth_for_scope` 是带语境的版本，多了 agent 身份这一级。** 它的顺序：

1. Bedrock API key 检查（同上）。
2. `bearer_auth_for_provider`——`Some` 就用，`Err` 直接返回（**没有 Kimi 兜底**）。
3. 若 `agent_identity_session_fallback` 尚未触发过：先看传入的 `auth` 是不是 `AgentIdentity`，是就直接用；否则问 `auth_manager.agent_identity_auth(policy, session_source)`。
4. 拿到 `Some(auth)` 就用 `ResolvedProviderAuth::for_agent_identity(auth)`。
5. 拿到 `Err(err)`：**若策略是 `ChatGptAuth` 且 fallback 的 `engage()` 返回真**（第一次调用时才真），就退回 `resolve_provider_auth` 走普通路径；否则 `Fatal`。
6. 都没成就走 `resolve_provider_auth`。

第 5 步那个 `engage()` 是关键：它的实现是 `!self.engaged.swap(true, Ordering::Relaxed)`——第一次调用返回 `true` 并置位，之后再调用返回 `false`。

所以「agent 身份认证失败就退回 ChatGPT 认证」这件事**只会发生一次**。这个限制的必要性很清楚：如果每次失败都退回，那么每次请求都会先白跑一次 agent 身份流程；而且「已经确定 agent 身份不可用」这个结论应当被记住。用 `AtomicBool` 的 swap 做「只发生一次」，与 g01、i01 里那些一次性声明是同一种手法。

注释里那句「Startup auth is only the account-scoped identity anchor」解释了为什么第 3 步要先看传入的 `auth`：启动时拿到的 `CodexAuth` 只用来做身份锚，但请求头**永远**从当前的 auth manager 快照取。所以两者不能混用——旧锚点只能锚定「是同一个账号」，不能用它的 token。

**`is_expected_auth` 是三项比对，不是一项。** `AuthManagerAuthProvider::is_expected_auth` 要求：

- `auth.uses_codex_backend()`
- `get_account_id()` 相同
- `get_chatgpt_user_id()` 相同
- `is_workspace_account()` 相同

四项全真才算「还是我以为的那个账号」。这个比较的动机是：auth manager 是全局的，可能在运行中被别的流程刷新成**另一个账号**的凭据。若不校验，一个 provider 实例可能把 A 账号的 provider 配上 B 账号的 token——这在多账号场景里是真实会发生的事。用一个启动时的锚点加四项比对，就不需要给每个 provider 单独复制一份凭据。

`add_auth_headers` 与 `resolve_auth_headers` 的差异值得看：

- `add_auth_headers`（同步）用 `auth_cached()`，取不到就**直接返回不加头**。
- `resolve_auth_headers`（异步）用 `.auth().await`，取不到返回 `AuthError::Transient("managed authentication is unavailable")`。

所以同步路径是「尽力而为」，异步路径是「要么给头要么报错」。这个双路径反映了调用方的两种处境：有些路径（例如构造调试日志）不关心是否有头，有些（真正发请求）必须在缺认证时明确失败。

**工作区路由的校验是「把 origin 拆开逐字段验」。** `apply_workspace_routing(provider, routing)` 先把 `routing.backend_origin` 解析成 URL，然后要求它满足七个条件：

- scheme 必须是 `"https"`
- 必须有 host
- 不得有 username
- 不得有 password
- `path()` 必须是 `"/"`
- 不得有 query
- 不得有 fragment

七条里最值得注意的是「path 必须是 `/`」与「不得有 userinfo」。一个带路径的 origin（`https://api.example.com/v1`）意味着「替换后 base_url 的路径段会被覆盖掉一部分」，那会让请求落到意料之外的端点；而带 `user@` 的 URL 会把凭据写进 URL，绕过请求头。

然后处理 routing override：

```rust
match routing.account_routing_override.as_str() {
    "NO_CONSTRAINT" => None,
    "us" | "us_cr" => Some(HeaderValue::from_str(...)?),
    _ => return Err("invalid workspace routing override"),
}
```

三支：`NO_CONSTRAINT` 不加头、两个白名单值加头、其他报错。所以 routing override 是一个**闭合枚举的字面表达**，不做「任意值透传」。若透传，一个被篡改的 routing 响应就能让客户端往服务端声明任意 routing 值。

随后只做三件事：`url.set_scheme` / `set_host` / `set_port` 换成 backend 的，写回 `provider.base_url`；`headers.remove(ACCOUNT_ROUTING_HEADER)`（先删！）；有头就插。

先删再插这一步很重要：即使本次 override 是 `NO_CONSTRAINT`（不加头），也必须删掉可能残留的旧值。否则「上一次路由带来的头」会跟着这一次请求发出去，让服务端认为这次也有 override 约束。

`url.set_scheme` 的返回类型是 `Result<(), ()>`（`set_scheme` 在 scheme 非法时返回 `Err(())`），所以 `map_err(|()| io::Error::other("invalid backend scheme"))`——注意捕获的是**单元类型**而不是错误值，这个写法只在 `set_scheme` 这种 API 上出现。

**为何 `redirect_policy` 必须跟着变成 `Reject`。** `ResolvedResponsesProvider` 的 doc 说：「工作区路由即使其路由覆盖没有提供头，也会拒绝重定向」。实现里 `apply_workspace_routing` 成功后立刻 `redirect_policy = ClientRedirectPolicy::Reject`。

理由是安全与正确性叠在一起：路由的意义是「把这次请求定向到某个后端」。若之后跟随 302，那个重定向可以指向任何主机——请求会带着 `ACCOUNT_ROUTING_HEADER` 与认证头跑掉。所以「一次路由生效 = 从此不跟随重定向」是路由本身的前提条件，而不是附加的保守措施。

注意这里不区分「有没有加 override 头」：只要发生了路由就 Reject。因为 `NO_CONSTRAINT` 的情况下 base_url 也已经被替换过了，同样不能被重定向带偏。

**`ResponsesConnectionKey` 决定「能不能复用 socket」。** 三个字段：`base_url`（仅字符串）、`routing_header`（可选头值）、`auth_revision: Option<u64>`。

`new` 从 `provider.base_url` 与 `provider.headers.get(ACCOUNT_ROUTING_HEADER)` 构造。所以复用的判据是「目标地址 + 路由约束 + 认证版本」三项全同。

`base_url` 用 `String` 而 `routing_header` 用 `HeaderValue`——前者因为它是配置值，后者因为它需要与 header map 里的值做精确比较。`auth_revision` 是 `Option<u64>` 而不是 `CodexAuth` 本体：比一个数字便宜得多，且认证一换就必然换号。

**`previously_routed` 是会话级的一次性标记。** `WorkspaceRoutingContext` 里它是 `Mutex<bool>`，`responses_api_provider` 在 `.await` 期间持有它（并为此写了 `#[expect]` 的理由）。它的作用是把「查找路由」与「本会话第一次成功的路由切换」串行化——所以两个并发的首个请求不会各自做一次路由、各自得出不同结论。

`WorkspaceRoutingContext` 的 doc 还补了一条语义：「一旦被路由，它不能仅因为账号所有者的引导配置变化就变得独立」。意思是路由结论对会话是粘性的：即便之后 bootstrap 配置变了，这个会话也不回到未路由状态。这与「粘性路由」在 provider 层的实现是同一个思路——一次决议，长期沿用。

## 关键取舍

**源码事实：** bearer 解析失败时会先试 Kimi Code 的存储 OAuth，只在 `None` 时才返回原错误。

**源码事实：** agent 身份的失败回退只用一次（`AtomicBool` swap），之后永久走普通路径或 Fatal。

**源码事实：** `AuthManagerAuthProvider` 用四项比对（后端、账号 id、用户 id、工作区标记）确认「还是同一个账号」。

**源码事实：** 同步 `add_auth_headers` 取不到快照就不加头；异步 `resolve_auth_headers` 取不到就报 `Transient` 错误。

**源码事实：** 工作区后端 origin 必须 https、无 userinfo、path 恰为 `/`、无 query、无 fragment；routing override 只接受 `NO_CONSTRAINT` / `us` / `us_cr`。

**源码事实：** 写头前先 `remove` 再 `insert`，所以 `NO_CONSTRAINT` 也会清掉旧值。

**源码事实：** 任何生效的路由都使 `redirect_policy` 变成 `Reject`，与是否加了 override 头无关。

**架构推论：** 「启动锚点 + 每请求快照」这个组合解决了「凭据会变而实例会活很久」的矛盾。provider 实例在启动时创建，但凭据可能在运行中刷新甚至换账号。用启动时的 `CodexAuth` 做身份锚，用当前快照取实际头，就同时得到「实例不必重建」与「不会串号」。

**架构推论：** 把「路由」实现成「替换 base_url 的三个部分」而不是「替换整个 URL」，是一个精确性上的选择。替换整串 URL 会让路径、query 一起被覆盖；只替换 scheme/host/port 则保留了原 base_url 的路径前缀，同时把 origin 换成新的。而先校验 backend origin 的七条性质，保证被替换进来的那部分本身是干净的。

**架构推论：** 「粘性」这个词在三个地方出现（`WorkspaceRoutingContext` 的 doc、`previously_routed`、`redirect_policy`），且都指向同一件事：**一旦某次决议生效，后续请求必须沿用同一个世界**。这类「决策一旦做出就不可逆」的设计在多租户/多后端系统里是必需项，否则请求会在不同后端之间跳来跳去，表现为间歇性的认证失败与数据不一致。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/model-provider/src/auth.rs` | `ResolvedProviderAuth` | 解析出的认证加可选的 agent 身份遥测 |
| `codex-rs/model-provider/src/auth.rs` | `ProviderAuthScope` | 认证解析的语境包：策略、来源、回退位 |
| `codex-rs/model-provider/src/auth.rs` | `AgentIdentitySessionFallback` | 只发生一次的会话级回退开关 |
| `codex-rs/model-provider/src/auth.rs` | `AuthManagerAuthProvider` | 从当前快照取头，用四项比对防串号 |
| `codex-rs/model-provider/src/auth.rs` | `UnauthenticatedAuthProvider` | 故意不发认证头的 provider 实现 |
| `codex-rs/model-provider/src/auth.rs` | `resolve_provider_auth` | bearer 优先，Kimi Code 兜底 |
| `codex-rs/model-provider/src/auth.rs` | `resolve_provider_auth_for_scope` | 加 agent 身份一级与一次性回退 |
| `codex-rs/model-provider/src/auth.rs` | `kimi_code_bearer_token` | 三层早退的 Kimi OAuth 解析 |
| `codex-rs/model-provider/src/auth.rs` | `auth_manager_for_provider` | 有 provider 配置就建 external bearer 管理器 |
| `codex-rs/model-provider/src/workspace_routing.rs` | `ACCOUNT_ROUTING_HEADER` | 账号路由覆盖的请求头名 |
| `codex-rs/model-provider/src/workspace_routing.rs` | `WorkspaceRoutingContext` | 会话级粘性路由作用域 |
| `codex-rs/model-provider/src/workspace_routing.rs` | `apply_workspace_routing` | 七条 origin 校验、override 白名单、换 origin |
| `codex-rs/model-provider/src/workspace_routing.rs` | `ResolvedResponsesProvider` | 路由后的 provider 与重定向策略 |
| `codex-rs/model-provider/src/workspace_routing.rs` | `ResponsesConnectionKey` | socket 复用的三元判据 |

## 参考

- [open-interpreter 源码：model-provider/src/auth.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/model-provider/src/auth.rs)
- [open-interpreter 源码：model-provider/src/workspace_routing.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/model-provider/src/workspace_routing.rs)
- [RFC 6750：Bearer 令牌的使用](https://datatracker.ietf.org/doc/html/rfc6750)

## 与主核 / AI 的关系

主核里最接近的是「连接认证与租户路由」：一条连接进来说明自己是谁，服务端据此决定它该访问哪个租户的数据目录，且这个决定一旦做出就在会话内固定。这一页的 `WorkspaceRoutingContext` 与那个「会话级租户绑定」是同一职责，连「粘性」这个要求都一样。

与 goose 的 g04（声明式提供方的密钥管理）对照：那边解决的是「密钥存在哪儿」（配置文件只存键名，密钥进密钥库）；这边解决的是「密钥以什么顺序被找到、找到之后怎么保证不会串到别的账号」。两者是同一件事的两半，而这里更强调**解析顺序**与**身份校验**。

与 smolagents 的 `ApiModel` 相比，那边的认证就是「一个环境变量或一个构造参数」，没有多级回退、没有身份锚点、没有路由。差别来自使用场景：smolagents 面向「开发者本机跑自己的模型」，这里面向「一个客户端连十几个后端、还要处理多账号与工作区路由」。

对 AI 工程最实用的一条是「启动锚点 + 每请求快照」。任何长驻进程里持有凭据的组件都会遇到同一个问题：凭据会过期、会刷新、可能被换。若组件在创建时把凭据复制一份，过期后就永久失效；若每次都从全局取，又可能出现「取到的是别的账号」的串号。用「启动时的身份指纹作为锚 + 每次从当前快照取实际凭据 + 取之前校验指纹」三步，可以同时避免这两种失败。这里用四项比对做指纹，是一个足够便宜且足够可靠的实现。

另一条是「路由必须与禁止重定向联动」。这条规则的重要性容易被低估：重定向是传输层的自动行为，而路由是应用层的意图。两者若不联动，一个 302 就能把「我已定向到这个后端」的保证抹掉，而且连认证头一起带走。任何做过「请求定向」的代码都应该检查一句：这条路径上还能不能自动跟随 3xx？

最后一条是「override 值用闭合白名单」。`account_routing_override` 只接受三个已知值，其他一律报错。若改成「透传字符串」，那么路由响应被篡改（或服务端出错）时，客户端会把任意值当作合法约束发给服务端——这等于把服务端的输入校验责任挪到了客户端，而客户端并没有能力判断哪些值合法。白名单让非法值在**本地**就被拦下，且错误信息明确。
