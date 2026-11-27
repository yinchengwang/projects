---
project: mcp-python-sdk
feature: request-state
title: 请求状态编码与边界校验
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/server/request_state.py
    symbols: [RequestStateCodec, AESGCMRequestStateCodec, RequestStateSecurity]
  - path: src/mcp/server/request_state.py
    symbols: [RequestStateBoundary, authenticated_principal]
---

# 请求状态编码与边界校验

## 一句话职责

多轮往返（multi-round-trip）要让服务端在两个 HTTP 请求之间记住状态，而状态必须经过客户端——于是规范要求服务端把客户端回显的 `requestState` 当作**攻击者可控**：这条链把每个出站值封成 AES-GCM 密文，并在每个入站回显上重验一遍**帧外声明**（版本、时限、方法、目标、参数摘要、受众、主体），只有完全对上才交出当初铸的明文。

## 机制是什么

三层分工：

| 层 | 类型 | 负责 |
|---|---|---|
| 加密 | `RequestStateCodec` / `AESGCMRequestStateCodec` | 只提供完整性与（理想情况下）机密性：`seal(payload) -> token`、`unseal(token) -> payload` |
| 策略 | `RequestStateSecurity` | 密钥环或自定义 codec、`ttl`、主体绑定回调、受众 |
| 边界 | `RequestStateBoundary` | 中间件：入站验封、出站封装；所有 claim 校验都在这一层 |

关键的分界写在 `RequestStateCodec` 的 docstring 里：**框架自己盖戳并重验每一个信封声明（过期、请求绑定、主体），codec 只负责完整性与机密性**。也就是说自定义 codec（比如 KMS 封装的）不需要理解 TTL 或主体，但必须满足 `unseal(seal(payload))` 往返、对任何非原样令牌抛 `InvalidRequestState`、令牌不自述算法（版本前缀要在认证标签下绑定，RFC 8725）、比较要常量时间、两个方法都同步（所以要缓存密钥材料而不是每个令牌都调一次 KMS）。

`RequestStateBoundary` 只作用于「多轮往返载体」——`tools/call`、`prompts/get`、`resources/read`，这个集合从 `INPUT_REQUIRED_METHODS` 派生。其他方法原样透传。

## 怎么实现的

**信封有七个声明，每个都对一类攻击。** 封装时 `_seal` 构造的 dict 是：

| 键 | 含义 | 挡什么 |
|---|---|---|
| `v` | 信封版本（1） | 格式演进后的降级/串用 |
| `iat` | 签发时刻 | 未来时间戳（含 60 秒松弛） |
| `exp` | 到期时刻 = `iat + ttl` | 无限期重放 |
| `m` | 方法名 | 把 `tools/call` 的状态塞进 `resources/read` |
| `t` | 目标（工具名 / prompt 名 / 资源 URI） | 换一个目标复用同一份状态 |
| `a` | 参数摘要（前 16 字节 SHA-256 的 base64url） | 改参数但沿用旧状态 |
| `aud` / `p` | 受众、主体指纹 | 跨服务复用、跨用户重放 |

验封时逐条检查，**每条失败都走同一个 `_reject`**：日志记真实原因，线上只回固定的 `-32602`「Invalid or expired requestState」。这条设计的目的是不给出区分信号——如果「过期」与「受众不符」返回不同错误，攻击者就能用它来探测密钥环与受众配置。

**`_request_identity` 用允许列表而不是拒绝列表。** docstring 写得很直白：**per-method allowlist, never a denylist: a future wire field cannot silently join the digest**。实现只取两个字段：`resources/read` 取 `uri`，其余取 `name` 与 `arguments`。若用「除 X 以外所有字段」的写法，将来协议新增一个字段就会悄悄进入摘要，从而改变已有令牌的绑定语义。

**参数摘要排序后取 SHA-256 前 16 字节。** `compact_json(args, sort_keys=True)` 保证同样的参数不同键序得到同一摘要；截断到 16 字节是为了令牌体积，而它足够抗碰撞（这是完整性检查而非抗碰撞性证明）。

**主体绑定用的是「不可逆指纹」而不是原值。** `_principal_claim` 生成 `salt(8) + sha256(_PRINCIPAL_LABEL + salt + principal)[:16]`，`_principal_matches` 用 `hmac.compare_digest` 比对。这样即使 codec 只签不加密（docstring 承认签而不密的 codec 会让载荷对客户端可读），主体标识也不会以明文出现在令牌里。而 `_principal_bytes` 用 `utf-8` + `surrogatepass`，注释说明理由：摘要输入是单向的、永不还原，所以编码要**全域**（total），不能让一个代理对让它抛异常。

**默认主体是三元组，且与会话归属共用同一个真值源。** `authenticated_principal` 调 `principal_components(token)` 得到 `(client_id, issuer, subject)` 再 `compact_json` 成字符串。docstring 说明了两个后果：同一 OAuth 客户端的两个用户，只要令牌验证器提供了 subject 就是不同主体；不提供时**降级为客户端身份**。在无认证的传输上返回 `None`，此时状态不做主体绑定。

**「主体漂移」被当成拒绝条件。** 验封里的这段判断值得单独看：

```
if (claim is None) != (principal is None):
    _reject(ctx.method, "principal drift")
```

它要求「令牌有主体 claim」与「当前上下文能算出主体」**同时成立或同时不成立**。这挡住的是一个边界情形：一个在无认证传输上铸出的令牌被搬到有认证的上下文中回显（或反之），两边都「没有不匹配」，但语义已经变了。

**自定义回调的失败必须 fail closed，且两个方向共用一处实现。** `_bound_principal` 做三件事：包住 `security.bind_principal(ctx)` 的调用并捕获**任何**异常（注释：deny-on-error，一个会抛的主体绑定必须失败关闭）、检查返回值类型（声明是 `str | None`，但用户回调可以无视类型）、并通过传入的 `fail` 把失败转换成**当前方向的线上形状**——验封方向是固定拒绝，封装方向是脱敏的 internal error。把两个方向统一在一处，避免其中一侧漏写。

**旋转密钥环有两个细节值得记住。** `AESGCMRequestStateCodec` 用 HKDF-SHA256 把任意格式的操作者密钥「拉伸」成 AES-256 密钥；密钥长度下限 32 字节，错误消息里直接给出生成命令（`python -c "import secrets; print(secrets.token_hex(32))"`）。它还显式拒绝对非 `bytes | bytearray | str` 做强制转换，注释给了理由：`bytes(32)` 会静默造出一个全零密钥。

**每个令牌带 4 字节密钥指纹做 O(1) 环查找，且指纹被绑进关联数据。** `seal` 用 `_TOKEN_PREFIX.encode() + kid` 作为 GCM 的 AAD，`unseal` 用同样的值解密。docstring 说明这样「令牌不能被重放进另一个格式版本或环槽位」。`_KID_INFO` 前缀让指纹只从派生后的密钥算出，而不是从原始 secret 算——旋转时只需知道环里有哪几把派生密钥。

**base64url 解码是严格双向的。** `_b64u_decode` 解完后**重新编码并与原串比较**，不等就抛「non-canonical base64url」。这条挡的是「同一份字节有多个合法编码」的重放变体：若接受非规范编码，同一个令牌可以有无数种线形态，任何按字符串去重的缓存或审计都会被绕过。

**入站校验的顺序说明了优先级。** `_unseal` 的顺序是：非字符串直接拒 → codec 解封（另捕 `InvalidRequestState` 与「codec 自己抛」两类）→ JSON 解析与必需键 → 版本与内层类型 → `iat`（含未来松弛）→ `exp` → 请求绑定（方法/目标/参数）→ 受众 → 主体。时间检查在绑定检查之前，意味着一个过期令牌不会泄露「它的绑定是否匹配」这类信息。

**「可接受条件用肯定式表述」是一条刻意的编码风格。** 代码里有一句注释：`Accept-conditions are stated positively so a NaN claim fails the comparison and rejects.` 也就是说判断写成 `if not (now < exp)` 而不是 `if now >= exp`——因为 `NaN` 与任何值比较都是 `False`，肯定式表述能让 NaN 落进拒绝分支。这类细节在处理「来自 JSON 的数字」时是必需的，因为 JSON 解析器可能产出 `NaN`。

**`requestState: null` 等于「没给」。** `__call__` 里的判断是 `ctx.params.get("requestState") is not None`，注释说明：**显式的 JSON null 算作缺失，因为把字段删掉本来就在任何客户端的能力范围内**。这是一个正确的模型——「省略」与「显式 null」在语义上不可区分，所以不该给它们不同待遇。

**封装方向有两个入口，分辨的是结果的形状。** `_seal_result` 注释说明：走规范路径的结果是 wire mapping，而一个短路的中间件可能返回模型对象。于是它先 `is_input_required(result)`（该函数同时认「类型化的 `InputRequiredResult`」与「`resultType == 'input_required'` 的 dict」），再按形状分别处理——dict 走 `{**result, ...}`，模型走 `model_copy(update=...)`。对 dict 还有一个防线：非字符串的 `requestState` 直接原样放行，注释说明「只有短路的中间件能放进来，没什么可封的」。

**`_seal` 里预先算好的绑定会被复用。** 验封返回的 `_RoundBinding` 是 `(target, args_digest, principal)` 三元组，`__call__` 把它一路传给 `_seal_result`。于是**同一轮里验封与封装用同一个主体**，不会出现「验的时候算一次、封的时候再算一次而结果不同」的漂移。只有在没有入站状态（首轮）时才重新算。

**受众必须显式声明。** `RequestStateBoundary.__init__` 的 `default_audience` 是必填（可为 `None`）参数，docstring 说明理由：**它是阻止「另一个共用同一批密钥的服务铸出的状态」的服务身份**。`MCPServer` 默认用自己的服务器名传入；lowlevel `Server` 的用户要自己 append 并传名字，或显式传 `None` 表示「故意不绑受众」。

**`RequestStateSecurity` 的两种形态各自适合什么，docstring 写清了。** `keys=[...]` 是内置 AES-256-GCM（`keys[0]` 封装、所有密钥都能解封，于是零停机轮换有明确的三阶段：`[old, new]` → `[new, old]` → 等一个 TTL 后 `[new]`）；`codec=...` 是自带加密；`ephemeral()` 是进程内随机密钥。最后一者的文档点明适用边界：**适合单进程部署（stdio、单个 HTTP worker），重启前或别的 worker 铸出的状态会被拒；多实例部署必须用 `keys=[...]` 共享密钥**。它也是 `MCPServer` 在未指定 `request_state_security=` 时的默认策略。

**TTL 必须是有界的正数。** 构造时检查 `math.isfinite(ttl) and ttl > 0`。`inf` 会让 `exp` 变成 `inf`、于是永不过期；`NaN` 会让所有比较失真——这类参数没有任何「宽容一点」的理由。

## 关键取舍

**源码事实：** 信封声明由框架校验，codec 只管完整性/机密性。自定义 codec 不必（也不该）理解 TTL、主体或请求绑定。

**源码事实：** 所有校验失败走同一个固定响应，真实原因只进服务端日志。不给攻击者区分信号。

**源码事实：** `_request_identity` 是 per-method 允许列表，防止未来新增的线字段悄悄进入参数摘要。

**源码事实：** 主体以「盐 + 截断摘要」形式进 claim，用 `compare_digest` 比较。即使 codec 只签不密，主体也不以明文出现。

**源码事实：** 「主体漂移」（claim 有/无与当前上下文有/无不一致）单独拒。这挡住跨认证上下文的搬运。

**源码事实：** 自定义 `bind_principal` 的异常与类型错误都 fail closed，且两个方向共用一个实现，只把失败翻译成各自方向的线上形状。

**源码事实：** 令牌带 4 字节密钥指纹做 O(1) 环查找，指纹被绑进 GCM 关联数据，于是不能重放进另一个版本或环槽位。

**源码事实：** base64url 解码后重新编码比对，拒绝非规范编码。否则同一令牌有无限多线形态。

**源码事实：** 可接受条件用肯定式（`not (now < exp)`）书写，使 `NaN` 落进拒绝分支。

**源码事实：** `requestState: null` 视同缺失；首轮封装时重新算绑定，有入站状态时复用验封算出的绑定。

**源码事实：** `audience` 必填参数（可显式 `None`），因为它是「同密钥多服务」之间唯一的隔离。

**源码事实：** `ephemeral()` 只适合单进程；多实例必须共享密钥。TTL 必须是有限正数。

**架构推论：** 这套设计的核心判断是「客户端回显的任何东西都不可信」，而推论是「服务端必须能独立验证自己铸过它」。于是状态不能只是一个标识（标识需要服务端自己存），也不能只是签名（签名不保密），而是**加密 + 帧外声明**。帧外声明的价值在于：加密只证明「这是服务端铸的」，声明才证明「这是给这次请求、这个主体、这个受众、这个时间窗用的」。少了任何一条，一个合法令牌就能被搬到不该去的场景——这正是多轮往返协议最容易漏掉的一层。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/server/request_state.py` | `RequestStateCodec` | codec 协议：只提供完整性/机密性，同步、常量时间、不自述算法 |
| `src/mcp/server/request_state.py` | `AESGCMRequestStateCodec` | 内置实现：HKDF 派生、密钥环、指纹绑进关联数据 |
| `src/mcp/server/request_state.py` | `AESGCMRequestStateCodec.seal` / `unseal` | 封装与解封，含严格 canoncial base64url 检查 |
| `src/mcp/server/request_state.py` | `RequestStateSecurity` | 策略：keys 或 codec 二选一、ttl、主体回调、受众 |
| `src/mcp/server/request_state.py` | `RequestStateSecurity.ephemeral` | 进程内随机密钥策略，`MCPServer` 的默认 |
| `src/mcp/server/request_state.py` | `RequestStateBoundary` | 中间件：入站验封换明文、出站封装 |
| `src/mcp/server/request_state.py` | `RequestStateBoundary._unseal` | 七项声明的完整校验链与固定拒绝 |
| `src/mcp/server/request_state.py` | `RequestStateBoundary._seal` | 构造信封、复用本轮绑定、codec 失败脱敏 |
| `src/mcp/server/request_state.py` | `_request_identity` | per-method 允许列表的（目标, 参数摘要） |
| `src/mcp/server/request_state.py` | `_principal_claim` / `_principal_matches` | 主体指纹的生成与常量时间比对 |
| `src/mcp/server/request_state.py` | `_bound_principal` | 主体回调的 deny-on-error 包装，两方向共用 |
| `src/mcp/server/request_state.py` | `authenticated_principal` | 默认主体绑定：(client_id, issuer, subject) |
| `src/mcp/server/request_state.py` | `_reject` | 固定线上错误、真实原因入日志 |
| `src/mcp/server/request_state.py` | `compact_json` | 一切摘要与封装使用的规范化 JSON |
| `src/mcp/server/request_state.py` | `InvalidRequestState` | codec 层失败，只在日志里出现 |

## 参考

- [mcp-python-sdk 源码：server/request_state.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/request_state.py)
- [MCP 规范：多轮往返请求](https://modelcontextprotocol.io/specification/2025-06-18/basic/index)
- [RFC 8725：JSON Web Token 最佳实践](https://datatracker.ietf.org/doc/html/rfc8725)
- [cryptography 文档：AES-GCM](https://cryptography.io/en/latest/hazmat/primitives/aead/)

## 与主核 / AI 的关系

这条链与主核里「客户端持有的游标（cursor）」完全同类：服务端把状态交给客户端保管，下一轮拿回来必须能验证它确实是自己发的、给这个查询的、没过期。数据库的游标通常只是服务端侧的句柄，一旦要跨无状态连接就必须变成可验证的令牌——`requestState` 就是这个令牌。

对 agent 工程的直接价值有三条。其一，**加密只解决一半问题**。把状态封成密文能防篡改与窥探，但防不了「把合法令牌搬到别的用途」；必须同时绑定方法、目标、参数、主体、受众。少绑一项就是一个可被利用的横向移动。其二，**参数摘要要用允许列表**。按「除了某些字段都参与」的写法，协议演进一次就会静默改变绑定语义，让存量令牌失效或让新字段不受保护。其三，**失败响应不能有区分度**。过期、受众不符、主体不符都回同一个错误，真实原因只进日志——否则错误码本身就是攻击者的探测工具。
