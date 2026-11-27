---
project: mcp-python-sdk
feature: oauth-models
title: OAuth 授权模型
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/shared/auth.py
    symbols: [OAuthToken, OAuthClientMetadata, InvalidScopeError]
  - path: src/mcp/shared/auth_utils.py
    symbols: [check_resource_allowed, calculate_token_expiry]
---

# OAuth 授权模型

## 一句话职责

把 MCP 客户端与服务端要交换的那几份 OAuth 文档（令牌、动态注册元数据、注册结果、授权服务器元数据、受保护资源元数据）声明成 Pydantic 模型，并把「哪些值必须严格、哪些值必须宽容」这条分界写进模型的字段类型与校验器里。

## 机制是什么

`shared/auth.py` 提供五组模型，各自对应一份 IETF 文档：

| 模型 | 对应规范 | 角色 |
|---|---|---|
| `OAuthToken` | RFC 6749 §5.1 | 令牌端点返回的访问令牌 |
| `OAuthClientMetadata` | RFC 7591 §2 | **客户端发出的**动态注册请求 |
| `OAuthClientInformationFull` | RFC 7591 §3.2.1 | **授权服务器回显的**注册记录 |
| `OAuthMetadata` | RFC 8414 §2 | 授权服务器元数据（发现） |
| `ProtectedResourceMetadata` | RFC 9728 §2 | 受保护资源元数据 |

前两个元数据模型共享基类 `OAuthClientMetadataBase`，基类只放「两边取值一致」的字段；取值可能不同的字段（`redirect_uris`、`token_endpoint_auth_method`、`grant_types`、`application_type`）各自声明在子模型上。

`shared/auth_utils.py` 提供三个纯函数：`resource_url_from_server_url`（RFC 8707 资源标识规范化）、`check_resource_allowed`（层级匹配）、`calculate_token_expiry`（过期时刻计算）。

## 怎么实现的

**请求模型与回显模型必须分开，这是在类型层面表达「谁有话语权」。** 这是本页最重要的一条设计。两个模型的 docstring 把理由写透了：`OAuthClientMetadata` 用来声明「本 SDK 会放到线上的值」，所以 `token_endpoint_auth_method` 的类型是收窄的 `TokenEndpointAuthMethod`（`none` / `client_secret_post` / `client_secret_basic` / `private_key_jwt`），`grant_types` 是带字面量的列表，`application_type` 是 `Literal['web', 'native']`。

而 `OAuthClientInformationFull` 的对应字段是 **`str | None` 与 `list[str]`**。原因是 RFC 7591 允许服务器「拒绝或替换客户端请求的任何元数据值」，所以服务器可能回显本 SDK 不认识的认证方法、别的 `application_type` 字符串、或者干脆把这些字段省略。文档写了一句关键的话：**「替换后的值是否可用，在使用它的地方决定，而不是在解析时决定」**。

**占位值被当成「缺省」。** 两个校验器在做同一件事：把 `null` 与 `""` 读成「这个键没出现」，于是字段的默认值生效，而不是解析失败。

- `_placeholder_members_read_as_omitted` 是一个 `model_validator(mode='before')`，把整份 dict 里值为 `None` 或 `""` 的成员直接剔除。它的注释说明了意图：服务器会把未设置的成员 dump 成 `null`、或回显成 `""`，任一占位值都会让列表字段解析失败，从而**丢掉一个已经完成供给的注册**。
- 对 URL 字段另有一个 `field_validator(mode='before')` 调 `_empty_str_to_none`，因为 `""` 过不了 `AnyHttpUrl`。

这两条合起来说明：解析一份第三方产出的文档时，「宽容」不是可选项，而是能否正常工作的前提。

**`redirect_uris` 是唯一仍被严格解析的字段。** 尽管整个回显模型都在放宽，`redirect_uris: list[AnyUrl] | None` 保留 URL 类型。文档给了理由：授权服务器要拿它和客户端的 `redirect_uri` 比对。这是「宽容原则要按后果分级」的例子——放宽类型是为了不丢数据，但不能放宽到失去可比性。

**两个业务校验方法把策略放在模型上。** `validate_scope(requested_scope)` 把请求的作用域按空格切开，逐个检查是否在已注册的 `scope` 里，不在就抛 `InvalidScopeError`。`validate_redirect_uri(redirect_uri)` 走三条分支：给了就必须在已注册列表里；没给且恰好注册了一个则用那个；否则抛 `InvalidRedirectUriError`（含第三方 OAuth 的常见陷阱：多个注册 URI 时必须显式指定）。

注意 `InvalidScopeError` 的形态：它只有 `message` 字段而**不继承** `ValueError`。这是一个刻意的选择——它不是「模型数据非法」，而是「业务请求越界」，两者应当被调用方分别捕获。

**`OAuthToken` 归一化 `token_type` 的大小写。** `field_validator('token_type', mode='before')` 做 `v.title()`，注释给了两处规范引用：RFC 6749 写的是标题形式的 `Bearer`，RFC 6750 §4 定义了它的用法。实际服务器会返回 `bearer` 或 `BEARER`，若不归一化，`Literal['Bearer']` 会拒掉合法令牌。这是一个很小但会让整条授权链失败的点。

**`calculate_token_expiry` 防御了「字符串形式的 `expires_in`」。** 它接受 `int | str | None`，内部 `int(expires_in)`。注释直白地写了原因：某些服务器会把它当字符串返回。返回 `None` 表示服务器没给过期时间——这个 `None` 与「过期于 0」必须区分，前者意味着令牌不会自动刷新。

**`check_resource_allowed` 做的是层级前缀匹配，不是相等匹配。** 规则是：scheme 与 netloc（小写后）必须相同，然后**给两边的 path 都补一个尾斜杠再比前缀**。补斜杠这一步是关键，注释给了反例：不补的话 `/api123/` 会匹配上 `/api/`。这就是 RFC 8707 允许「父资源的令牌用于子资源」的实现，同时挡掉了「前缀相同但不是路径层级」的伪造。

**`resource_url_from_server_url` 做规范化：小写 scheme 与 netloc、去掉 fragment。** fragment 必须去掉是 RFC 8707 §2 的明文要求（资源 URI「不得包含 fragment 组件」）。小写化则是为了让同一资源的不同书写得到同一个标识——否则同一个资源会因为主机名大小写而拿到不同令牌。

**`ProtectedResourceMetadata` 的 `bearer_methods_supported` 默认是 `['header']`，注释写明「MCP 只支持 header 方式」。** 这条注释本身就是规范约束的落地：模型允许服务器声明其他方式，但框架的默认值与实现只认 header。

**`ProtectedResourceMetadata.authorization_servers` 有 `min_length=1`。** 一个受保护资源若不指明授权服务器，客户端就无从开始发现流程，所以这是结构上的必需项而非可选项。

**`OAuthClientMetadataBase` 里有一批「存而不用」的字段。** 注释写明 `client_name`、`client_uri`、`logo_uri`、`contacts`、`tos_uri`、`policy_uri`、`jwks_uri`、`jwks`、`software_id`、`software_version` 目前不参与逻辑，但被支持并存储以备将来使用。这是一个「协议字段先落地、语义后实现」的取舍：不解析它们会让注册请求不完整，解析但不使用则无成本。

**`OAuthMetadata` 把支持的扩展点声明成可选的列表。** 里面既有 RFC 8414 的标准项（`code_challenge_methods_supported`、`revocation_endpoint`、`introspection_endpoint`），也有 `client_id_metadata_document_supported`、`authorization_response_iss_parameter_supported`，以及注释标为「SEP-990」的 `authorization_grant_profiles_supported`。把这些都声明成可选字段，让客户端能「按服务器声明的能力决定用哪条流程」，而不是硬编码一套假设。

**`OAuthClientInformationFull.issuer` 不是 RFC 7591 字段。** 注释说明了它的用途：由 SDK 自己记录「这批凭据是在哪个 issuer 注册的」，用来**检测授权服务器迁移、避免跨 AS 复用凭据**。这是把安全需求（凭据不能跨主体重用）编码成一个附加字段的做法，比靠调用方自觉更可靠。

## 关键取舍

**源码事实：** 客户端发出的注册请求（`OAuthClientMetadata`）用收窄类型，服务器回显的注册记录（`OAuthClientInformationFull`）用宽松类型。分界依据是 RFC 7591 赋予服务器的替换权，以及「可用性在使用处判断」这条原则。

**源码事实：** `null` 与 `""` 在回显模型里等价于「键缺失」，于是默认值生效。这样不会因为一个占位符丢掉一次已完成的注册。

**源码事实：** `redirect_uris` 即使在宽松模型里也保持 URL 类型，因为它要参与比对。宽容的边界是「不能宽容到失去语义」。

**源码事实：** `token_type` 在解析时归一化为标题形式。不归一化会让 `bearer` 这种常见返回被 `Literal['Bearer']` 拒掉。

**源码事实：** `InvalidScopeError` 与 `InvalidRedirectUriError` 不继承 `ValueError`，与模型解析失败区分开。

**源码事实：** `check_resource_allowed` 先给两边 path 补尾斜杠再比前缀，避免 `/api123/` 匹配 `/api/`。

**源码事实：** `calculate_token_expiry` 接受字符串形式的 `expires_in`，并把「无过期时间」表示为 `None` 而不是 0。

**源码事实：** `issuer` 字段由 SDK 记录而非协议提供，目的是阻止跨授权服务器的凭据复用。

**架构推论：** 这一层几乎不做决策，只做**形状与宽容度的声明**。真正的决策（这个 scope 能不能给、这个 redirect_uri 收不收、这个资源标识能不能用父令牌）放在方法或调用方。这样分层的好处是模型可以被任意一方的代码复用（客户端解析服务器响应、服务器解析客户端请求），代价是「什么算合法」散落在多处，读代码时必须同时看模型的类型约束与业务方法。

**架构推论：** 整组模型都围绕一个事实展开：**OAuth 的两个端点由不同主体实现**。因此「我发出去的」与「我收回来的」必须有不同强度的契约，且收回来的那份必须能容忍实现差异。任何只写一份「客户端元数据」模型的设计，都会在接入第三方授权服务器时被现实打回。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/shared/auth.py` | `OAuthToken` | RFC 6749 令牌，`token_type` 归一化为标题形式 |
| `src/mcp/shared/auth.py` | `OAuthClientMetadata` | 客户端发出的注册请求，字段类型收窄 |
| `src/mcp/shared/auth.py` | `OAuthClientInformationFull` | 服务器回显的注册记录，字段类型放宽并容忍占位值 |
| `src/mcp/shared/auth.py` | `OAuthClientInformationFull.validate_scope` | 请求作用域必须已在注册时授予 |
| `src/mcp/shared/auth.py` | `OAuthClientInformationFull.validate_redirect_uri` | 三态校验，含「唯一注册 URI 可省略」 |
| `src/mcp/shared/auth.py` | `OAuthMetadata` | RFC 8414 授权服务器元数据与能力声明 |
| `src/mcp/shared/auth.py` | `ProtectedResourceMetadata` | RFC 9728 受保护资源元数据 |
| `src/mcp/shared/auth.py` | `InvalidScopeError` | 作用域越界，不继承 `ValueError` |
| `src/mcp/shared/auth_utils.py` | `check_resource_allowed` | 先补尾斜杠再比前缀的层级匹配 |
| `src/mcp/shared/auth_utils.py` | `resource_url_from_server_url` | RFC 8707 规范化：小写 origin、去 fragment |
| `src/mcp/shared/auth_utils.py` | `calculate_token_expiry` | 容忍字符串 `expires_in`，无过期返回 `None` |
| `src/mcp/shared/auth.py` | `DEFAULT_GRANT_TYPES` | RFC 7591 §2 的默认 `grant_types` |

## 参考

- [mcp-python-sdk 源码：shared/auth.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/shared/auth.py)
- [mcp-python-sdk 源码：shared/auth_utils.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/shared/auth_utils.py)
- [RFC 7591：OAuth 2.0 动态客户端注册](https://datatracker.ietf.org/doc/html/rfc7591)
- [RFC 8707：OAuth 2.0 资源指示符](https://datatracker.ietf.org/doc/html/rfc8707)
- [MCP 规范：授权](https://modelcontextprotocol.io/specification/2025-06-18/basic/authorization)

## 与主核 / AI 的关系

这一层与主核里「解析外部格式」的工作同类：读别人产出的数据时，宽容度必须按「这份数据能不能被拒」来定。主核解析外部表定义、COPY 文本格式、pg_dump 输出时都遵循同一条：**能恢复的就恢复，能当缺省的就当缺省，但参与比对的字段必须严格**。`OAuthClientInformationFull` 与 `redirect_uris` 的关系就是这句话的实例。

对 agent 工程的直接价值有三条。其一，**别用一份模型同时表达「我要发的」与「我收回的」**。前者该收窄（好让错误早暴露），后者该放宽（好让第三方实现能被接受）；合成一份不可避免地要在两难里选一边。其二，**占位值要当缺省处理**。第三方服务器把未设置字段写成 `null` 或 `""` 是常态，把它们当非法值会在最不该失败的时候失败——一次已经完成的注册。其三，**资源标识的层级匹配要补斜杠**。`/api` 与 `/api123` 的前缀关系在字符串层面成立、在路径层面不成立，这类错误只在生产环境被利用时才暴露。
