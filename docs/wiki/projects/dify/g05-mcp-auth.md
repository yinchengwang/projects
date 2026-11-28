---
project: dify
feature: mcp-auth
title: MCP 授权流程
page_type: feature
schema_version: 1
refs:
  - path: api/core/mcp/auth/auth_flow.py
    symbols: [generate_pkce_challenge, discover_protected_resource_metadata]
  - path: api/core/mcp/auth/auth_flow.py
    symbols: [discover_oauth_authorization_server_metadata]
---

# MCP 授权流程

## 一句话职责

把 MCP 服务器的 OAuth 授权跑通：先从服务器发现「我是谁、谁能给我发令牌」，再按发现的元数据走三条路之一（授权码 + PKCE、客户端凭据、刷新令牌），中间用 Redis 存一次性 state——**并且把「写数据库」这件事全部交回给调用方**。

## 机制是什么

这是一份**几乎完整的 OAuth 客户端实现**，745 行里没有一行业务逻辑，全是协议动作。它的结构可以按「发现 → 决策 → 执行」三段落来看：

| 阶段 | 函数 | 干什么 |
|---|---|---|
| 发现 | `discover_protected_resource_metadata` | RFC 9470：拿到「这个资源需要什么、授权的权威在哪」 |
| 发现 | `discover_oauth_authorization_server_metadata` | RFC 8414：拿到 `authorization_endpoint`、`token_endpoint`、`registration_endpoint`、支持的 grant type 与 scope |
| 发现 | `get_effective_scope` | 四级优先级选出实际要申请的 scope |
| 决策 | `auth` | 按「有没有 client 信息 / 有没有授权码 / 有没有 refresh token」选路径 |
| 执行 | `start_authorization` / `exchange_authorization` / `refresh_authorization` / `client_credentials_flow` / `register_client` | 五条具体的协议动作 |

**最值得注意的是它的返回值不是「结果」而是「动作清单」。** `auth()` 返回 `AuthResult(actions=[...], response={...})`，`actions` 里装的是 `AuthAction(action_type=SAVE_CLIENT_INFO | SAVE_TOKENS | SAVE_CODE_VERIFIER, data=..., provider_id=..., tenant_id=...)`。

docstring 写得很清楚：「This function performs only network operations and returns actions that need to be performed by the caller (such as saving data to database).」

**所以这个模块是纯网络 + 纯协议，持久化被完全外部化。** 这是一个很强的边界选择：它让整个授权流程可以用「mock 掉 `ssrf_proxy`」来测，不需要数据库。

## 怎么实现的

### PKCE：手写而不是用库

```
code_verifier = base64.urlsafe_b64encode(os.urandom(40)).decode("utf-8")
code_verifier = code_verifier.replace("=", "").replace("+", "-").replace("/", "_")
code_challenge_hash = hashlib.sha256(code_verifier.encode("utf-8")).digest()
code_challenge = base64.urlsafe_b64encode(code_challenge_hash).decode("utf-8")
code_challenge = code_challenge.replace("=", "").replace("+", "-").replace("/", "_")
```

`os.urandom(40)` 给 320 位熵，base64 后约 53 字符，落在 RFC 7636 要求的 43–128 区间内。两次 `replace` 是**手工做 base64url 编码**：`base64.urlsafe_b64encode` 已经换了 `+/`，但仍保留 `=` 填充，所以还要去掉填充。

`code_challenge_method = "S256"` 写死在 `start_authorization` 里，与这里用 SHA256 一致。**没有 `plain` 方法的支持**——这是正确的：`plain` 在移动/公开客户端场景下几乎等于没有 PKCE。

### 发现：URL 候选列表本身就是协议知识

两个 `build_*_discovery_urls` 函数是这一页信息密度最高的部分，因为它们把 RFC 里的「路径插入 / 路径附加」规则翻译成了具体 URL。

**Protected Resource Metadata（RFC 9728 §5.1）三个候选，优先级明确：**

1. 来自 `WWW-Authenticate` 头的 URL（若给了）——并且要对它做**归一化**；
2. `https://host/.well-known/oauth-protected-resource{path}`（路径插入）；
3. `https://host/.well-known/oauth-protected-resource`（根）。

归一化那段值得单看，它处理了 `WWW-Authenticate` 里可能出现的三种写法：

- 有 scheme 有 netloc（绝对 URL）→ 直接用；
- 无 scheme 有 netloc（`//host/path` 协议相对）→ 拼上原 URL 的 scheme；
- 无 scheme 无 netloc 但 path 以 `/` 开头（相对路径）→ 用 `urljoin` 拼到 base 上，**但有一道过滤**：第一段必须是 `.well-known` 或**第一段里不含点**。

最后那道过滤是防 SSRF 的细节：若 `WWW-Authenticate` 给的是 `evil.example.com/path` 这种「看起来像相对路径其实是主机名」的写法，`urljoin` 会把它拼成 `https://原主机/evil.example.com/path`——而过滤条件让「第一段含点」的情况被拒绝（因为那更像域名而不是路径段）。**这是「按形状猜意图」的启发式，不是严格的校验，但它挡住了最常见的一类注入。**

**Authorization Server Metadata（RFC 8414 + OIDC）五个候选：**

1. `{base}/.well-known/oauth-authorization-server`（根，RFC 8414）
2. `{base}/.well-known/openid-configuration`（根，OIDC）
3. `{base}/.well-known/openid-configuration{path}`（OIDC 路径插入）
4. `{base}{path}/.well-known/openid-configuration`（OIDC 路径附加）
5. `{base}/.well-known/oauth-authorization-server{path}`（RFC 8414 路径插入）

**两种规范的路径规则不同**（RFC 8414 是插入，OIDC 两种都允许），所以候选列表必须同时覆盖——这也是为什么是 5 个而不是 2 个。函数 docstring 把每条规则与对应 URL 并列写出，**注释即协议索引**。

两个 `discover_*` 函数的循环逻辑完全一样：

```
for url in urls:
    try:
        response = ssrf_proxy.get(url, headers=headers)
        if response.status_code == 200:
            return Model.model_validate(response.json())
        elif response.status_code == 404:
            continue
    except (RequestError, ValidationError, json.JSONDecodeError):
        continue
return None
```

**注意：只有 200 才算命中，404 与异常都只是「试下一个」。而其它状态码（401、403、500）既不会 return 也不会 continue——它们会走出 `if/elif` 的末尾，同样进入下一轮循环。** 也就是说这个循环对「非 200 非 404」的状态码没有任何显式处理，效果与 404 相同。这在发现场景下是合理的（发现本来就是尽力而为），但代价是**服务端返回 500 时会被静默当作「这里没有元数据」**。

请求头带 `"User-Agent": "Dify"` 与 `MCP-Protocol-Version`——后者是 MCP 规范要求的版本声明。

### Scope 的四级优先

```
if scope_from_www_auth: return scope_from_www_auth
if prm and prm.scopes_supported: return " ".join(prm.scopes_supported)
if asm and asm.scopes_supported: return " ".join(asm.scopes_supported)
return client_scope
```

**服务器的显式要求 > 资源元数据 > 授权服务器元数据 > 客户端配置。** 这个顺序是对的：越靠近「服务器明确告诉我需要什么」的越优先。`WWW-Authenticate` 里的 scope 是服务器在 401 响应里直接点名要的，所以排第一。

### state：Redis 一次性键

`start_authorization` 不在 URL 里塞状态，而是：

1. `secrets.token_urlsafe(32)` 生成随机键；
2. 把整个 `OAuthCallbackState`（含 `provider_id`、`tenant_id`、`server_url`、`metadata`、`client_information`、`code_verifier`、`redirect_uri`）序列化后 `setex` 到 Redis，TTL 300 秒；
3. 把**这个随机键**当作 OAuth 的 `state` 参数放进授权 URL。

回调时 `_retrieve_redis_state` **先取值再立刻 `delete`**，注释是「Delete the state data from Redis immediately after retrieval to prevent reuse.」——**这是 state 参数防重放的核心：一次性使用。**

这也解释了为什么 `code_verifier` 要存 Redis：它不能出现在授权 URL 里（那会让 PKCE 失效），必须由服务端暂存到换 token 时再用。**把「不能给浏览器的秘密」放进服务端存储，是这个设计的要点。**

TTL 300 秒是硬编码常量。**它同时是「用户完成授权的时间上限」**——超过五分钟的授权流程会因 state 过期而失败。

### 三条流程路径

`auth()` 的分叉是按「手上已有什么」判断的，而且判断顺序有讲究：

1. **没有 client 信息** 且 `authorization_code is not None` → 报错「Existing OAuth client information is required when exchanging an authorization code」（换码必须有 client 身份）；
2. **没有 client 信息** 且 grant type 是 `client_credentials` → 报错「Client credentials flow requires client_id and client_secret to be provided」（客户端凭据流不能靠动态注册获得身份，必须用户填）；
3. **没有 client 信息** 其余情况 → `register_client` 动态注册，并把 `SAVE_CLIENT_INFO` 动作加进清单，然后继续；
4. **grant type 是 `client_credentials`** → 直接走 `client_credentials_flow`，成功后 `SAVE_TOKENS` 返回；
5. **有授权码** → 必须有 `state_param`，从 Redis 取回 `code_verifier` 与 `redirect_uri`，`exchange_authorization` 换 token；
6. **有 refresh token** → `refresh_authorization` 续期；
7. **以上都没有** → `start_authorization` 生成授权 URL，返回 `response={"authorization_url": ...}` 并加一条 `SAVE_CODE_VERIFIER` 动作。

第 3 步与第 7 步的分工值得注意：**`start_authorization` 内部自己也生成了一次 `code_verifier` 并把它写进 Redis 的 state 里**，同时又在返回值和 `SAVE_CODE_VERIFIER` 动作里暴露出来。所以 `code_verifier` 被存了两处（Redis state 与调用方数据库）。**这是冗余的，但冗余在这里是有意的：Redis 那份给回调换码用，数据库那份给「Redis 丢失后重试」用**——不过实际上第 5 步只从 Redis 取，所以数据库那份目前是死数据。

### 错误处理的一致性

几乎每个网络动作都有同样的模式：

```
try:
    ...network...
except (RequestError, ValueError, KeyError) as e:
    raise ValueError(f"... failed: {e}") from e
```

三处注释说明了为什么捕这三个：`RequestError` 是 HTTP 失败、`ValueError` 是响应数据非法、`KeyError` 是响应缺字段。**统一翻成 `ValueError` 是因为「调用方只把 `RequestError`/`ValueError` 翻成面向用户的错误」**——`register_client` 里那句注释写得最直接：

```
# Callers only translate RequestError/ValueError into a user-facing error,
# so a raise_for_status() here would escape as an opaque 500.
raise ValueError(f"Client registration failed: HTTP {response.status_code}, Response: {response.text}")
```

**这是一个「错误类型是对外契约」的实证**：如果这里用 `raise_for_status()`，抛出的 `httpx.HTTPStatusError` 不在调用方能翻译的范围内，就会变成 500。所以必须主动包成 `ValueError`。

### 令牌响应兼容两种格式

`_parse_token_response` 按 `content-type` 三分支：JSON（RFC 6749 标准）、`application/x-www-form-urlencoded`（老式实现）、以及未知时先试 JSON 再退表单。

注释举了例子：「some legacy OAuth providers (e.g., early GitHub OAuth Apps) return application/x-www-form-urlencoded format for backwards compatibility」。**这是一个真实存在的兼容性坑**，而且「未知 content-type 也两种都试」是必要的——有些服务端根本不发 content-type。

## 关键取舍

**源码事实：** PKCE 手写实现：`os.urandom(40)` → base64url（手工去 `=`、换 `+_/`）→ SHA256 → 再 base64url；`code_challenge_method` 固定 `S256`，不支持 `plain`。

**源码事实：** 两个发现函数各返回 URL 候选列表并逐个尝试，只有 200 命中，404 与异常都继续；其它状态码落空后同样进入下一轮，最终返回 `None`。

**源码事实：** `WWW-Authenticate` 提供的元数据 URL 会被归一化三种形态，相对路径形态带一道过滤（第一段必须是 `.well-known` 或不含点）。

**源码事实：** scope 四级优先：`WWW-Authenticate` → PRM → ASM → 客户端配置。

**源码事实：** state 是 Redis 里的一次性键，TTL 300 秒，取回后立刻 `delete`；`code_verifier` 与 `redirect_uri` 存在 state 数据里。

**源码事实：** 所有网络动作把 `(RequestError, ValueError, KeyError)` 统一翻成 `ValueError`，原因是调用方只翻译这两种类型；`register_client` 的注释明确说 `raise_for_status()` 会逃成 500。

**源码事实：** `_parse_token_response` 兼容 JSON、form-urlencoded 与未知类型（先 JSON 后表单）。

**源码事实：** `auth()` 只做网络与协议，把「保存 client 信息 / 保存令牌 / 保存 code_verifier」作为 `AuthAction` 清单返回。

**架构推论：** 「返回动作清单而不是直接写库」是一个很有价值的边界。它让授权流程成为**纯函数式的协议状态机**：输入是「服务器元数据 + 已存凭据 + 回调参数」，输出是「网络副作用 + 待落库动作」。测试只需 mock HTTP，用例可以覆盖全部分支。代价是调用方必须忠实执行 `actions`，漏做一步就会在下一次调用时表现成「状态缺失」。

**架构推论：** URL 候选列表是「把 RFC 的路径规则写成代码」。这类代码没法抽象——每种规范的插入/附加规则不同，只能逐个列。**但可以照抄这个形状：列表 + 优先级注释 + 逐个尝试 + 全部失败返回 None。** 比起自己发明一套发现顺序，照规范列全候选更不容易漏。

**架构推论：** 「错误类型是对外契约」这一点在跨层设计里经常被忽略。这个模块里所有网络错误都被压成 `ValueError`，不是因为它「更好」，而是因为**上游只认这两种**。正确做法要么是统一错误类型（这里就是），要么是上游改用宽类型（那会失去区分度）。**最坏的是每层各自抛原生异常，让用户看到 500。**

**架构推论：** `code_verifier` 存两处（Redis state + 数据库动作）而只用一处，是「多次演进后的残留」。这类冗余不会造成错误，但会让读者误以为两条路都通。**鉴别方法：看消费端实际读哪一份。** 这里回调只读 Redis。

**架构推论：** state 用「服务端随机键」而不是「签名后的自包含 token」，换来的是**可以存任意大小与任意敏感度的东西**（这里存了整个 `OAuthMetadata` 与 `client_information`），代价是**必须有可用的 Redis**。自包含方案（JWT 签名）不需要存储但会把内容暴露给浏览器。两者取舍取决于「暂存内容是否敏感」——`client_secret` 与 `code_verifier` 都属于敏感，所以这里选存储是对的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/mcp/auth/auth_flow.py` | `generate_pkce_challenge` | 手写 PKCE：verifier + S256 challenge |
| `api/core/mcp/auth/auth_flow.py` | `build_protected_resource_metadata_discovery_urls` | RFC 9728 的三级 URL 候选与归一化 |
| `api/core/mcp/auth/auth_flow.py` | `build_oauth_authorization_server_metadata_discovery_urls` | RFC 8414 + OIDC 的五级 URL 候选 |
| `api/core/mcp/auth/auth_flow.py` | `discover_protected_resource_metadata` | 逐个尝试 PRM 候选，全部失败返回 `None` |
| `api/core/mcp/auth/auth_flow.py` | `discover_oauth_authorization_server_metadata` | 逐个尝试 ASM 候选 |
| `api/core/mcp/auth/auth_flow.py` | `get_effective_scope` | 四级 scope 优先 |
| `api/core/mcp/auth/auth_flow.py` | `_create_secure_redis_state` | 生成随机 state 键并把状态写入 Redis |
| `api/core/mcp/auth/auth_flow.py` | `_retrieve_redis_state` | 取回并立刻删除（防重放） |
| `api/core/mcp/auth/auth_flow.py` | `handle_callback` | 回调入口：取 state + 换 token |
| `api/core/mcp/auth/auth_flow.py` | `check_support_resource_discovery` | 探测资源发现支持（兼容单复数两种字段名） |
| `api/core/mcp/auth/auth_flow.py` | `discover_oauth_metadata` | 串起 PRM 与 ASM 两步发现 |
| `api/core/mcp/auth/auth_flow.py` | `start_authorization` | 生成授权 URL 与 PKCE 参数 |
| `api/core/mcp/auth/auth_flow.py` | `_parse_token_response` | 三种响应格式的令牌解析 |
| `api/core/mcp/auth/auth_flow.py` | `exchange_authorization` | 授权码换令牌 |
| `api/core/mcp/auth/auth_flow.py` | `refresh_authorization` | 刷新令牌 |
| `api/core/mcp/auth/auth_flow.py` | `client_credentials_flow` | 客户端凭据流 |
| `api/core/mcp/auth/auth_flow.py` | `register_client` | 动态客户端注册 |
| `api/core/mcp/auth/auth_flow.py` | `auth` | 总编排：发现 → 选 grant type → 选路径 → 返回动作清单 |
| `api/core/mcp/entities.py` | `AuthActionType` | 三类待执行动作 |
| `api/core/mcp/entities.py` | `OAuthCallbackState` | Redis 里暂存的回调态 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/mcp/auth/auth_flow.py](https://github.com/langgenius/dify/blob/main/api/core/mcp/auth/auth_flow.py)
- [RFC 7636：PKCE](https://datatracker.ietf.org/doc/html/rfc7636)
- [RFC 8414：授权服务器元数据](https://datatracker.ietf.org/doc/html/rfc8414)
- [RFC 9728：受保护资源元数据](https://datatracker.ietf.org/doc/html/rfc9728)

## 与主核 / AI 的关系

这套授权流程与主核里的**外部认证/凭据管理**是同一层问题：如何在不把凭据给第三方的前提下，让第三方代表用户访问受保护资源。数据库侧对应的是 `pg_hba.conf` 的认证方法、外部表（FDW）的连接凭据、以及 `dblink` 的密码处理。**核心约束完全一样：凭据不能进 URI、不能进日志、不能给不该看到的一方。**

**state 一次性 vs 数据库的 nonce / 事务 id。** PostgreSQL 的 GSSAPI 认证里也有类似的「服务端生成一次性令牌」机制；所有防重放协议都需要一个**服务端持有、使用即销毁**的标记。`_retrieve_redis_state` 的「取完就 `delete`」与数据库「事务 id 用过即作废」是同一种纪律。

**返回动作清单 vs 数据库的两阶段命令。** 数据库的 `PREPARE` / `EXECUTE` 把「规划」与「执行」分开，让调用方在两阶段之间有控制权。这里把「网络动作」与「持久化动作」分开，让调用方决定何时、以何种事务边界落库。**把副作用外置，是让协议代码可测试、可重放的通用手法。**

**URL 候选列表 vs 数据库的 `search_path`。** 数据库解析一个表名会按 `search_path` 逐个 schema 尝试，找到即止；这里解析一个元数据文档会按候选 URL 逐个尝试，200 即止。**「按优先级列表逐个尝试、全失败才报错」是同一个模式**，区别只是数据库最终会报「表不存在」，而这里返回 `None` 让上层决定。

对 agent 工程四条结论。其一：**协议代码要把持久化外置**，返回「动作清单」而不是直接写库——这让全部分支可测，也让调用方能自己决定事务边界。其二：**错误类型就是对外契约**，若上游只翻译 `ValueError`，那所有网络失败都必须主动包成它，否则会逃成 500。其三：**OAuth 的 state 必须一次性**，取完即删；用服务端存储换「可以暂存敏感内容」。其四：**发现类请求要按规范列全候选 URL 并带优先级注释**，自己发明顺序必然漏协议分支。

最后一条关于实现质量：`code_verifier` 存两处而只用一处的冗余，说明这套代码经过多轮演进。**读这类代码时最有价值的动作是「找出实际被消费的那一份」**，否则会在调试时对着死数据浪费时间。
