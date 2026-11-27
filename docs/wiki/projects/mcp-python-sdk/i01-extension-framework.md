---
project: mcp-python-sdk
feature: extension-framework
title: 扩展点与绑定
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/server/extension.py
    symbols: [Extension, ToolBinding, MethodBinding]
  - path: src/mcp/client/extension.py
    symbols: [ClientExtension, ClaimContext, ResultClaim]
---

# 扩展点与绑定

## 一句话职责

用一对镜像的窄基类（服务端 `Extension` / 客户端 `ClientExtension`）让扩展以**声明**的方式贡献能力：服务端声明「我新增哪些工具、资源、请求方法、以及一个 `tools/call` 拦截器」，客户端声明「我认得哪些非核心的结果形状、观察哪些通知」——两侧都**拿不到对方的本体**，只交出被消费的绑定。

## 机制是什么

两侧各有四种贡献，而且方向互补：

| 维度 | 服务端 `Extension` | 客户端 `ClientExtension` |
|---|---|---|
| 身份 | `identifier`（反向 DNS，如 `io.modelcontextprotocol/ui`） | 同 |
| 新增内容 | `ToolBinding` / `ResourceBinding` | —（内容由服务端给） |
| 新增线协议 | `MethodBinding`（新请求方法） | `ResultClaim`（既有动词上的新 `resultType`） |
| 包裹/观察核心行为 | `intercept_tool_call`（包 `tools/call`） | `NotificationBinding`（只观察通知） |
| 广告 | `settings()` → `ServerCapabilities.extensions[id]` | `settings()` → `ClientCapabilities.extensions[id]` |

绑定的字段都很小：`ToolBinding(fn, meta, kwargs)`、`ResourceBinding(resource)`、`MethodBinding(method, params_type, handler, protocol_versions)`；客户端 `ResultClaim(result_type, model, resolve, method, protocol_versions)`、`NotificationBinding(method, params_type, handler)`。

两侧的「运行期上下文」也各有一个：服务端拦截器拿 `ServerRequestContext` 与 `CallNext`，客户端 `ResultClaim.resolve` 拿 `ClaimContext(session, tool_name, read_timeout_seconds)`。

## 怎么实现的

**「扩展不拿到宿主」是两侧共同的第一原则。** 服务端模块的 docstring 写明：**服务器从不把自己交给扩展；扩展声明它增加什么，服务器来消费**。客户端模块用同一句话收尾：**接口是声明式的、在构造时固定、永不接收客户端**。这条约束的实际收益是扩展无法绕过中间件、参数校验与结果信封——它只能贡献被明确定义的那几类东西。

**贡献种类是封闭集合，这是刻意的。** 服务端只认四种（工具、资源、新方法、一个拦截器），客户端只认两种（claim、通知）。`_CLAIM_METHODS` 的注释甚至写了配套纪律：「这是 claim 可以依附的封闭动词集合，**要与 `method` 的 `Literal` 一起放宽**」——也就是说改一处必须改另一处，类型与运行期校验是成对的。

**两侧都用 `__init_subclass__` 在类定义时校验 `identifier`。** 规则来自 `shared/extension.py`：反向 DNS 前缀 + `/` + 名字，点分标签每个以字母开头、以字母或数字结尾（连字符可在内部），名字首尾必须是字母数字。两侧都留了同一种逃逸口：**若子类在 `__init__` 里赋 `identifier`（实例级 id），类定义时没有可检查的属性，于是改为在应用/消费时校验**。注释把这条写明了，避免维护者以为漏了。

**`Extension` 的每个方法都有默认实现，最小扩展只覆写 `identifier` 加一个贡献方法。** docstring 提到这个形状抄自 httpx2 的 `Transport`/`Auth`：窄基类 + 有意义的默认值，所以扩展只覆写它需要的。它还给出两种典型形态：**纯增量型（Apps）覆写 `tools`/`resources`；拦截型覆写 `methods`/`intercept_tool_call`**。

**模块层级的选择本身是一条设计说明。** 服务端 `extension.py` 放在 `mcp.server` 而不是 `mcp.server.mcpserver`，docstring 给的理由是：**基类不拖入消费它的组合层，于是扩展可以在不构造 `MCPServer` 的情况下被导入**。这对测试与库作者都重要——一个只想声明扩展的包不必依赖门面。

**`MethodBinding` 的两条构造期约束都对应真实失败模式。** 其一，`method` 不能是规范定义的请求方法（`tools/list`、`completion/complete` 等）——因为它属于服务器，扩展绑定一个会**静默遮蔽或被遮蔽**。其二，`protocol_versions` 不能是空集，因为「空集意味着永远不会被服务」；要放开所有版本应当传 `None`。第二条的错误消息直接把替代方案写出来了。

`protocol_versions` 的语义也写在 docstring 里：限制在那些线上版本，**在其他版本收到该方法即 `METHOD_NOT_FOUND`**，与规范的 `(method, version)` 边界表对齐。这比「忽略版本」正确得多——旧客户端不会拿到一个它无法解释的方法。

**docstring 还给了「想恢复被删的规范方法」的正确路径。** 2026 版删掉了 `logging/setLevel` 这类方法，若想给旧客户端继续提供，应当用 lowlevel `Server.add_request_handler` 而不是扩展——因为 **runner 的按版本表面门禁根本不会把这种方法路由到扩展处理器**。这句话避免了一个必然踩的坑。

**`intercept_tool_call` 的文档把三段语义分得很清。** 一是可以短路（不调 `call_next` 直接返回结果），也可以只观察。二是**拦截发生在处理器层**，所以它返回的东西会像普通处理器结果一样被序列化，包括 2026 期的 `serverInfo` `_meta` 戳。三是一条容易被误解的规则：**拦截器收到的 `params` 就是被包裹处理器收到的 `params`**——通过 `call_next` 传一个改写过的 context 只改变处理器在 `ctx` 上看到的东西，**不改变工具调用本身**。想做线级请求改写要用 `Server.middleware`（在参数校验之上）。

**`compose_tool_call_handler` 的折叠顺序与参数捕获都值得单独记。** 它把每个扩展的拦截器绕在真正的 `tools/call` 处理器外面，**第一个扩展在最外层**（`for extension in reversed(extensions)` 从内往外搭）。`params` 在外层函数入口就被闭包捕获，所以内层拿到的永远是原始参数——这正是上一条「改写 ctx 不改调用」的实现方式。注释还重申了它发生在 outbound 信封处理之下，因此短路结果同样会被过筛与打戳。

**客户端的 `ResultClaim` 是全篇约束最多的一处，因为它在核心校验之后才生效。** `__post_init__` 逐一拦六类问题，每类都对应一个具体后果：

1. `method` 必须在 `_CLAIM_METHODS` 里——目前只有 `tools/call`。
2. `result_type` 不能是核心协议词汇（`CORE_RESULT_TYPES`），否则会与核心结果形状冲突。
3. `model` 必须**确实**继承 `Result`。注释特别说明这是运行期守卫：`ClaimedT` 的上界只约束受检调用方，运行期仍要查。
4. `model` 不能是 `CallToolResult` / `InputRequiredResult` 的子类——不能假装核心结果。
5. `model` 的任何字段**不得别名到保留线键**。这条最精妙：`_RESERVED_WIRE_ALIASES` 是 `{requestState, inputRequests}`，注释解释**核心结果表面会预校验每个入站结果**，一个撞名的字段会在 claim 适配器运行之前就让核心校验失败。检查用的是 `_wire_keys`，它把 `alias`、`serialization_alias`、以及 `validation_alias` 的每个 choice（含 `AliasPath` 取首段）都算进「这个字段可能读写的线键」。
6. `result_type` 字段的注解必须**恰好**是 `Literal[<该 result_type>]`（用 `get_args(...) != (self.result_type,)` 判断），否则反序列化无法靠判别字段选中模型。

外加版本门：`protocol_versions` 不能为空集，且**必须是 `MODERN_PROTOCOL_VERSIONS` 的子集**。错误消息说清了原因——**claim 形状无法在旧线上投递**，`None` 表示所有现代版本。

**claim 的激活条件是两个条件的与。** docstring：**只有在声明它的扩展被构造进客户端、且协商出的协议版本允许时才生效**。工具名进 `ClaimContext` 是因为 `resolve` 往往需要知道自己在替哪个工具收拾结果；`read_timeout_seconds` 同理，因为 `resolve` 可能要继续发请求。

**「没选择加入却收到 claim 结果」有专门的异常。** `UnexpectedClaimedResult` 携带已解析的结果，消息给出两条出路（把所属扩展传给 `Client(extensions=[...])`，或调用时传 `allow_claimed=True` 自行处理），并且提醒一句关键事实：**服务端可能已经持有这份结果所引用的状态**。这解释了为什么不能简单丢弃——丢弃会留下需要清理的孤儿状态。

**`NotificationBinding` 是「只观察」，且明确不保证顺序。** docstring 三条：参数已校验、每个绑定一次一条、按分发顺序、通过**有界队列**传递、溢出处**丢弃最旧的一条并警告**。同时点明流式传输会独立分发每条通知，所以**近乎同时的通知可能以非线序到达**。还有一条边界：**协商版本的核心表已处理的方法永远不会投递给绑定**——因此绑定只能观察扩展方法。

**`advertise()` 是一个带警告的便利函数。** 它只为「仅需广告标识与设置」的场景生成一个 `_AdvertiseOnly`。docstring 的警告很直接：**广告一个你没有实现的扩展等于声称你不具备的线上支持；行为型扩展应当构造真正的扩展**。它还顺手复用了同一套 identifier 校验（`owner='advertise'`）。

**两侧的 `for_agent`/消费入口都做版本感知的广告。** 客户端 `settings()` 的文档补了一条：**带 claim 的扩展只在「至少一个 claim 处于激活状态」的版本上被广告**。这条避免了在旧版线上广告一个自己不会兑现的扩展。

## 关键取舍

**源码事实：** 两侧贡献种类都是封闭集合，且客户端的 `_CLAIM_METHODS` 与 `method` 的 `Literal` 被注释要求**同步放宽**——类型与运行期校验成对演进。

**源码事实：** 扩展永远不接收宿主（服务端不接收 server、客户端不接收 client），只交出声明式绑定，由宿主消费。这让扩展无法绕过中间件、参数校验与结果信封。

**源码事实：** 模块放在 `mcp.server` 而非 `mcp.server.mcpserver`，使扩展可在不构造门面的情况下被导入。

**源码事实：** `MethodBinding` 禁止绑定规范方法（会静默遮蔽/被遮蔽），且空 `protocol_versions` 被拒（永远服务不了）。想恢复被删的规范方法要用 lowlevel `Server.add_request_handler`，因为按版本表面门禁不会把这类方法路由给扩展。

**源码事实：** `intercept_tool_call` 的拦截发生在处理器层；改写 `ctx` 不影响工具调用本身，只有 `params` 决定调用。线级改写属于 `Server.middleware`。

**源码事实：** 拦截器折叠时第一个扩展在最外层，`params` 在入口闭包捕获，所以内层永远看到原始参数。

**源码事实：** `ResultClaim` 在构造期拦六类问题，其中「字段不得别名到 `requestState`/`inputRequests`」是因为核心结果表面会先预校验入站结果，撞名会在 claim 适配器之前失败。

**源码事实：** claim 只能依附 `tools/call`，且只支持现代协议版本；`None` 与空集语义不同（`None` = 所有现代版本，空集 = 被拒）。

**源码事实：** `UnexpectedClaimedResult` 特意携带结果并提醒服务端可能已持有状态，避免调用方随手丢弃。

**源码事实：** `NotificationBinding` 只观察、有界队列丢最旧、流式传输可能非线序、核心表处理过的方法永不投递。

**架构推论：** 这套框架的本质是**把「扩展能做什么」变成一份封闭的、可在两侧对照的表**。封闭的原因是可预测性：一旦允许扩展任意挂到任意层，就没法保证中间件不被绕过、结果信封不被破坏、版本门禁不被跳过。而两侧镜像的收益是**同一个扩展概念在两端有对称的表达**——服务端加一个方法，客户端能给它加一个结果形状；服务端加一个工具，客户端能给它的非核心结果加一个 claim。代价是每加一种贡献种类都要同时改两侧的封闭表、版本门与构造期校验，所以「支持一种新的扩展形态」在这个框架里是一次跨两端的设计动作，而不是加个钩子。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/server/extension.py` | `Extension` | 服务端扩展基类：四种贡献全有默认实现 |
| `src/mcp/server/extension.py` | `ToolBinding` | 贡献一个工具 + 要打的 `_meta` + 额外 kwargs |
| `src/mcp/server/extension.py` | `ResourceBinding` | 贡献一个预构建资源 |
| `src/mcp/server/extension.py` | `MethodBinding` | 贡献一个新请求方法，含 params 类型与版本门 |
| `src/mcp/server/extension.py` | `MethodBinding.__post_init__` | 拒规范方法、拒空版本集 |
| `src/mcp/server/extension.py` | `Extension.intercept_tool_call` | 包 `tools/call`：可观察也可短路，默认透传 |
| `src/mcp/server/extension.py` | `compose_tool_call_handler` | 折叠拦截器，第一个扩展在最外层，`params` 闭包捕获 |
| `src/mcp/client/extension.py` | `ClientExtension` | 客户端扩展基类：claim 与通知观察 |
| `src/mcp/client/extension.py` | `ResultClaim` | 既有动词上的新 `resultType` 及其解析器 |
| `src/mcp/client/extension.py` | `ResultClaim.__post_init__` | 六类构造期约束，含保留线键别名检查 |
| `src/mcp/client/extension.py` | `ClaimContext` | `resolve` 的宿主注入上下文：会话、工具名、读超时 |
| `src/mcp/client/extension.py` | `NotificationBinding` | 只观察的通知绑定，有界队列、可能非线序 |
| `src/mcp/client/extension.py` | `UnexpectedClaimedResult` | 未选择加入却收到 claim 结果，携带结果并提示状态清理 |
| `src/mcp/client/extension.py` | `advertise` | 只广告不实现，docstring 警告过度声明的后果 |
| `src/mcp/shared/extension.py` | `validate_extension_identifier` | 反向 DNS 标识语法，两侧共用 |

## 参考

- [mcp-python-sdk 源码：server/extension.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/extension.py)
- [mcp-python-sdk 源码：client/extension.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/client/extension.py)
- [MCP 规范：扩展与反向 DNS 标识](https://modelcontextprotocol.io/specification/2025-06-18/basic/index)
- [httpx 文档：Transport 与 Auth 接口](https://www.python-httpx.org/advanced/transports/)

## 与主核 / AI 的关系

扩展框架与主核的「可插拔访问方法 / 自定义扫描」是同一类问题的两种解法。数据库扩展点通常也要求显式注册、声明成本与能力、并被优化器当作黑盒对待——因为一旦允许绕过优化器的规划约束，性能与正确性都失去保证。这里用的是同一条思路：扩展只能贡献封闭集合里的东西，不能接管请求管线。

对 agent 工程的直接价值有三条。其一，**「不得拿到宿主」比「提供更多钩子」更有价值**。扩展一旦能拿到服务器对象，中间件顺序、参数校验、结果打戳都可以被绕过，而这些恰恰是安全与可观测性的支点。其二，**新增线能力要两侧同时设计**。服务端加一个方法，若客户端没有对应的结果形状声明，返回的东西就只能是不可解析的未知——所以 `MethodBinding` 与 `ResultClaim` 必须是成对的概念，而不是「服务端先加、客户端以后再说」。其三，**构造期校验要按后果分级**。`ResultClaim` 拦的六类里，最容易被忽略的是「字段别名撞上核心保留键」——它在运行期的表现是核心校验先失败、错误指向 claim 之外的地方，几乎无法从现象推出原因。
