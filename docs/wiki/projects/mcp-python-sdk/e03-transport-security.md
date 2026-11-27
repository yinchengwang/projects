---
project: mcp-python-sdk
feature: transport-security
title: 传输安全设置与请求体限制
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/server/transport_security.py
    symbols: [TransportSecuritySettings, TransportSecurityMiddleware, RequestBodyLimitMiddleware]
---

# 传输安全设置与请求体限制

## 一句话职责

HTTP 传输的两个请求前置检查：一个按 Host / Origin 白名单挡 DNS 重绑定，一个在把请求交给应用之前挡掉超大请求体；两个都返回「响应或 None」，把拒绝变成普通的 HTTP 状态码而不是异常。

## 机制是什么

MCP 的 HTTP 服务端通常被挂在 `localhost` 上给本机客户端用。这类部署有一个经典攻击面：**DNS 重绑定**。攻击者控制的域名先解析到自己的服务器（页面被浏览器加载），再重新解析到 `127.0.0.1`；于是页面里的脚本就能以同源身份访问本机服务端。

浏览器在这里帮不上忙——同源策略按域名算，重绑定后域名没变。可用的信号只剩 `Host` 与 `Origin` 两个头，所以这两项白名单就是防线。第三个检查（`Content-Type`）与 DNS 重绑定无关，它保证 POST 请求体确实是 JSON，避免把任意内容喂给 JSON-RPC 解析器。

另一个中间件处理的是**资源消耗**而不是攻击者身份：请求体大小上限。它的难点不在判上限，而在**读完整个 body 判断完之后还要把请求原样交给下游**——ASGI 的 `receive` 是一次性的。

## 怎么实现的

**Host 校验：精确匹配 + 通配端口。** `_validate_host` 先判空（`None` 时记一条 warning 并返回假），再查 `host in allowed_hosts` 精确命中。其次是 `:*` 通配：对以 `:*` 结尾的条目，截掉后两个字符得到基名，然后判 `host.startswith(base_host + ":")`。

这条通配的实现值得细看：它是**字符串前缀匹配**而不是解析出端口再比较。于是 `example.com:*` 会匹配 `example.com:8080`，但也会匹配 `example.com:abc`（非数字端口）。实际影响有限——Host 头本身要被下游或客户端理解——但它说明了这条规则不是严格的端口语法校验，而是一个宽松的前缀捷径。同理，`allowed_hosts` 里不带端口时只匹配不带端口的 Host。

**Origin 校验：缺席即通过。** `_validate_origin` 在 `origin` 为空时**直接返回真**，注释给的理由是「同源请求可能不带 Origin 头」。这是整套校验里最关键的一条放宽：如果这里也严格，那么所有非浏览器客户端（curl、SDK、服务间调用）都会被拒。也正因为这条放宽，Origin 白名单只能防浏览器发起的跨源请求，挡不住任何能自己写头的东西。

其余逻辑与 Host 对称：精确匹配、`:*` 前缀、失败时 warning。

**`validate_request` 的顺序与状态码是刻意配对的。**

1. **先查 Content-Type**（仅 POST），不通过返回 **400**。这一步**不看** `enable_dns_rebinding_protection`，注释写「Always validate Content-Type for POST requests」——因为它与重绑定无关，属于基本的协议合规。
2. 若重绑定保护关闭，**直接返回 `None`**（通过）。这是默认行为。
3. Host 不通过返回 **421**（Misdirected Request）。
4. Origin 不通过返回 **403**。

三个状态码的选择有讲究：`400` 是「你的请求格式不对」，`421` 是「请求被定向到了不该处理的服务器」——这正是 Host 不匹配的语义，比 400 精确；`403` 是「服务器理解了请求但拒绝授权」，符合 Origin 白名单的语义。用状态码区分三种失败原因，让运维与客户端都能据此判断问题在哪。

**默认关闭重绑定保护，是一处显式记录的向后兼容妥协。** 构造函数写的是：`settings or TransportSecuritySettings(enable_dns_rebinding_protection=False)`，注释是「若未指定，为向后兼容默认关闭」。同时 `TransportSecuritySettings` 的字段默认值是 `True`，两个默认值**相反**。这不是笔误，而是「模型层给安全默认、构造层给兼容默认」的分裂：直接构造 `TransportSecuritySettings()` 得到的是开启，而 `TransportSecurityMiddleware()` 不传参数得到的是关闭。

源码里还留着两条 TODO，语气很直白：一条说「我们应该把这些设置扁平化。说实话我认为我们甚至不该有这个中间件」，另一条说「这应该是一个正经的 ASGI 中间件。看到这个我很悲哀」。这说明作者对当前形态不满——`TransportSecurityMiddleware` 并不是真正的 ASGI 中间件，而是一个被传输实现手动调用的普通对象。

**`RequestBodyLimitMiddleware` 是真 ASGI 中间件，难点在重放。** 它是标准的 `(scope, receive, send)` 三元组，`scope["type"] != "http"` 时直接透传（不干扰 websocket/lifespan）。

流程分两层：

- **先看声明的 `Content-Length`**：解析成 int（`ValueError` 时静默跳过，让实际累计去判），超限直接返回 **413**，**连一个字节都不读**。这是最便宜的拒绝路径。
- 否则**一边读一边累计**，每读一块就判 `len(received_body) + len(body) > max_body_size`，超限同样 413 并**立即返回**（不再读完剩余）。所以在声明的长度撒谎或缺失时，真实的防护在累计判这里。

读完（或决定不读）之后是这套实现的核心：**把已经消费的消息重放给下游**。它用 `deque` 缓存：

- 若读过 `http.request` 消息，把累计的 body 重新拼成**一条** `{"type": "http.request", "body": ..., "more_body": not body_complete}` 塞进队列。
- 若循环是因为遇到非 `http.request` 消息（例如 `http.disconnect`）而退出，把那条消息也缓存进去。

然后 `replay()` 先 `popleft()` 缓存，空了才转调原始 `receive`。这样下游看到的序列与原始序列**在语义上等价**（body 一样、终止标记一样），只是被合并成了一条消息。

几个细节：

- `body_complete` 只在整个 body 读完（`more_body` 为假）时置真。若因遇到非 `http.request` 消息而提前退出，`body_complete` 仍是假，所以重放的第一条消息带 `more_body=True`——这正确地表达了「body 尚未结束」。
- `received_request` 标记是否至少要发一条 `http.request`。没有读任何请求时（例如第一个消息就是 `http.disconnect`）不伪造一条。
- 上限在**累计值**上判而不是在单条消息上判，所以分块传输（chunked）无法绕过。
- 默认上限 `DEFAULT_MAX_REQUEST_BODY_SIZE` 是 4 MiB，定义为模块级 `Final`。

## 关键取舍

**源码事实：** Host 与 Origin 的 `:*` 通配是**字符串前缀**匹配（`host.startswith(base + ":")`），不是端口语法解析。

**源码事实：** Origin 头缺席时**通过**，理由是「同源请求可能不带 Origin」。这让校验只对会发 Origin 的浏览器生效。

**源码事实：** Content-Type 校验对 POST **无条件执行**，不受 `enable_dns_rebinding_protection` 影响。

**源码事实：** 三种失败用三个不同状态码：400（Content-Type）、421（Host）、403（Origin）。

**源码事实：** `TransportSecuritySettings` 字段默认 `enable_dns_rebinding_protection=True`，而 `TransportSecurityMiddleware` 不传参时构造的是 `False`——两处默认值相反，注释写的是「向后兼容」。

**源码事实：** 请求体上限先看 `Content-Length` 快速拒，再在累计值上判；拒绝后不回读剩余 body，直接返回 413。

**源码事实：** `RequestBodyLimitMiddleware` 读完后用 `deque` 重放消费过的消息给下游，把多块 body 合并成一条。

**架构推论：** 这两个检查的分工是「身份/来源」与「资源」的分离。前者回答「这个请求该不该被这个服务端处理」，后者回答「这个请求要花多少资源才能处理完」。混在一起会让拒绝时机难以推理——例如把大小检查放进 Host 校验之后，一个超大的恶意请求会先触发重绑定路径。

**架构推论：** 「读到一半就拒绝」这条能力是有代价的：一旦读了 body，就必须负责把它交还给下游。这就是为什么这个中间件的核心不是判限而是 `replay`。若换成先读完再判，实现简单但内存占用与延迟都变成「必须吃完才知道要不要吃」——对上限 4 MiB 来说可以接受，但设计者选择了更早拒绝。

**架构推论：** 默认关闭重绑定保护是典型的「安全默认 vs 兼容默认」冲突。这里的选择（模型层开、中间件层关）实际上把决定权留给了传输实现——它显式构造 settings 时才带上合适的默认。这个模式能工作，但它要求每个调用点都知道自己在做什么，而 `TransportSecuritySettings()` 与 `TransportSecurityMiddleware()` 两个相反的默认值很容易让人读错。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/server/transport_security.py` | `TransportSecuritySettings` | 重绑定保护开关与 Host/Origin 白名单配置 |
| `src/mcp/server/transport_security.py` | `TransportSecurityMiddleware` | 非 ASGI 的手动调用校验器：Content-Type、Host、Origin |
| `src/mcp/server/transport_security.py` | `TransportSecurityMiddleware._validate_host` | 精确匹配 + `:*` 前缀通配 |
| `src/mcp/server/transport_security.py` | `TransportSecurityMiddleware._validate_origin` | 缺席通过、精确匹配、通配 |
| `src/mcp/server/transport_security.py` | `RequestBodyLimitMiddleware` | 真 ASGI 中间件：声明长度快速拒 + 累计判 + 消息重放 |
| `src/mcp/server/transport_security.py` | `DEFAULT_MAX_REQUEST_BODY_SIZE` | 默认 4 MiB 上限 |

## 参考

- [mcp-python-sdk 源码：transport_security.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/transport_security.py)
- [MCP 规范：HTTP 传输安全](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports#security-warning)
- [OWASP：DNS 重绑定](https://owasp.org/www-community/attacks/Server_Side_Request_Forgery)
- [ASGI 文档：HTTP 请求的消息格式](https://asgi.readthedocs.io/en/latest/specs/www.html#request)

## 与主核 / AI 的关系

Host 白名单挡 DNS 重绑定，与主核里「监听地址与 `pg_hba.conf` 的客户端地址校验」处在相似的生态位：都是「在协议解析之前，先用请求自带的元信息决定要不要继续」。差别在可信度——数据库用的源 IP 由内核提供，难以伪造；HTTP 的 Host 头是客户端随便写的。所以数据库的地址校验能作为认证的一部分，而这里的 Host 校验只能防**浏览器**（因为浏览器强制填 Host）。这条差异解释了为什么 Origin 缺席时必须放行：非浏览器客户端不发 Origin，若强行要求就等于拒绝所有程序化访问。

请求体上限那套「读一半拒掉再重放」的做法，主核里对应的是**传输层分片限制**：同样要在「读完才知道多大」与「不能无限内存」之间取舍。数据库的解法是流式处理 + 上限，从不缓存整个请求；这里上限只有 4 MiB，所以选择了缓存 + 重放。

`replay` 这段代码值得作为 ASGI 中间件的通用范例记住：任何需要「窥视」请求体的中间件都必须解决重放问题，而重放的正确性依赖对 `more_body` 与终止消息的精确处理。写错的表现是下游永远等不到 body 结束（挂起）或者把 `http.disconnect` 吞掉（无法感知客户端断开）。

对 AI 工程的实际意义在于：本机部署的 MCP 服务端是这类框架最常见的形态，而它的威胁模型与公网服务**不同**——不是「陌生人攻击」，而是「你打开的网页攻击你本机的服务」。这类攻击不需要任何凭证，所以必须靠 Host/Origin 白名单挡。实践中最容易犯的错是把保护默认关掉（这里就是为了兼容而默认关），因此部署清单里应当明确要求：生产必须显式开启并给出白名单，否则本地服务端对任何网页都是敞开的。
