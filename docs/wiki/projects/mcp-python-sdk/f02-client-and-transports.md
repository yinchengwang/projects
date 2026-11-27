---
project: mcp-python-sdk
feature: client-and-transports
title: 客户端连接与三种传输
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/client/client.py
    symbols: [Client, _connect_transport, _connect_inproc]
  - path: src/mcp/client/stdio.py
    symbols: [StdioServerParameters]
  - path: src/mcp/client/streamable_http.py
    symbols: [StreamableHTTPTransport, ResumptionError]
---

# 客户端连接与三种传输

## 一句话职责

`Client` 是一个看起来不需要配置的高层门面：它的 `server` 字段可以是一行 URL、一份子进程参数、一个现成的传输对象，甚至一个服务端实例，`__post_init__` 会据此**在构造期就决定**一条连接路径，`__aenter__` 再按 `mode` 完成握手。

## 机制是什么

这层的设计核心是「**把选择从运行期搬到构造期**」。`_Connector` 的类型签名说明了它的职责：

```
_Connector = Callable[[AsyncExitStack, ConnectMode, bool], Awaitable[Dispatcher[Any]]]
```

注释写得很明确：它在 `__post_init__` 里**仅凭 `server` 的形状**就解析出来，职责是「把需要的资源进入退出栈，然后交回一个分发器」；而 `mode` 与 `raise_exceptions` 留到调用时再传，这样它们被读取的时刻与 `__aenter__` 读取它们做握手是同一个时刻。

三种连接路径：

| `server` 的形状 | 连接器 | 底层 |
|---|---|---|
| `str`（URL） | `_connect_transport(streamable_http_client(url))` | 客户端 Streamable HTTP |
| `StdioServerParameters` | `_connect_transport(stdio_client(params))` | 子进程 + stdin/stdout |
| 任意 `Transport` 实例 | `_connect_transport(transport)` | 用户提供的流对 |
| `Server` / `MCPServer` 实例 | `_connect_inproc(server)` | 进程内直连 |

前三种的共同点是「拿到一对读写流，再包成 `JSONRPCDispatcher`」，所以走同一个连接器。第四种完全不走 JSON-RPC 框架层（`mode != "legacy"` 时），这是它与前三种的本质区别。

## 怎么实现的

**`__post_init__` 的三段工作。** 这是一个 `@dataclass`，所以 `__post_init__` 是配置校验与派生的唯一入口：

1. **校验 `mode`**。合法值是 `"legacy"`、`"auto"`、或 `MODERN_PROTOCOL_VERSIONS` 里的某个版本字符串。非法时抛 `ValueError`，且错误文案会**额外提示**：若传入的其实是握手时代的版本（`HANDSHAKE_PROTOCOL_VERSIONS` 里），就补一句「`{mode}` is a handshake-era version; use mode='legacy'」。这是一处体贴的诊断——用户最可能犯的错就是把旧版本号当 mode 传。
2. **折叠扩展**。`_fold_extensions(self.extensions)` 把用户给的 `ClientExtension` 序列折成 `ClientSession` 消费的四种形状：`ad`（能力广告）、`claims`（结果声明）、`bindings`（通知绑定）、`by_model`。折叠期做了三类冲突检测，且错误文案会点名**两个**归属者（「extensions 'a' and 'b' both claim resultType ...」）。它还显式拒绝 mapping 形式：`TypeError("extensions= takes a sequence of ClientExtension instances. The mapping form was replaced: use advertise(identifier, settings) for advertise-only entries")`——这是一次 API 迁移留下的守卫。
3. **解析连接器**。`MCPServer` 先取 `_lowlevel_server` 再判断（注释带 `pyright: ignore`，说明这是刻意的内部访问）。

**`_connect_inproc` 的两条路径是这一页最值得写的部分。**

```python
if mode == "legacy":
    transport = InMemoryTransport(server, raise_exceptions=raise_exceptions)
    read_stream, write_stream = await exit_stack.enter_async_context(transport)
    return JSONRPCDispatcher(read_stream, write_stream)
lifespan_state = await exit_stack.enter_async_context(server.lifespan(server))
client_disp, server_disp = create_direct_dispatcher_pair(raise_handler_exceptions=raise_exceptions)
```

- **legacy 模式**走 `InMemoryTransport`，仍是「内存流 + JSON-RPC 框架」，与远程连接在行为上等价（只是不经过网络）。这让测试能覆盖真实的序列化/反序列化路径。
- **其他模式**（`auto` 与现代版本）走 `DirectDispatcher` 对：**没有流、没有 JSON-RPC 框架、没有 initialize 握手**。注释把三点都点了出来。它把服务端 lifespan 手动进入退出栈，然后在后台任务里跑 `server_disp.run(modern_on_request(server, lifespan_state), _no_inbound_client_notifications)`。

`_no_inbound_client_notifications` 是一个空函数，docstring 解释了为什么可以空：「At 2026-07-28 the spec defines no client→server notifications: `initialized` and `roots/list_changed` are removed, and cancellation is structural (anyio scope cancel through the direct await, not a notify)」。这是新协议时代的一次真实简化——旧协议里那些「客户端要告诉服务端我初始化好了」的信封在新协议里不存在了。

`exit_stack.callback(server_disp.close)` 这一行也值得注意：用同步 callback 而不是 `enter_async_context`，因为 `close` 是同步方法。

**`_build_session` 的两处包装。**

- **消息处理器被缓存驱逐逻辑包了一层**：`_evicting_message_handler(self._response_cache, self.message_handler)`。它的实现是三件事按顺序：若是服务端通知，先 `await cache.evict_for_notification(message)`；再调用户处理器；用户没给处理器就 `await anyio.lowlevel.checkpoint()`（注释说这是「mirrors ClientSession's default handler」）。驱逐调用的异常被**单独捕获并记日志**，注释解释：「eviction reaches user store code; a cache fault must not block delivery」——缓存出问题不能让通知丢。
- **折叠好的扩展被展开成 `ClientSession` 的三个参数**：`extensions=self._folded_extensions.ad`、`result_claims=self._folded_extensions.claims`、`notification_bindings=self._folded_extensions.bindings`。

**`__aenter__` 的顺序保证了一条重要的不变量。** 它先在局部 `async with AsyncExitStack()` 里做完全部工作：建会话、进入会话、按 mode 握手、最后才把 exit_stack **弹出**（`pop_all()`）保存、把 session 赋给 `self._session`。所以：

- 握手失败时，局部退出栈会负责把传输拆掉，`self._session` 保持 `None`。
- `_session is not None` 因此蕴含「`protocol_version` 与 `server_capabilities` 都已就绪」。注释把这条写成了显式契约，还补充 `server_info` 仍然是可选的（2026 时代的服务端可能不报自己是谁）。

没有这个 `pop_all()` 技巧，中途失败会留下一个已进入但没人负责退出的栈。

**三种 mode 对应三步不同的握手。** 就在 `__aenter__` 里：

```python
if self.mode == "legacy":
    await session.initialize()
elif self.mode == "auto":
    await negotiate_auto(session)
else:
    session.adopt(self.prior_discover or _synthesize_discover(self.mode))
```

第三种（版本钉住）走 `_synthesize_discover`：造一个只有该版本、空能力、`ttl_ms=0`、`cache_scope="public"` 的最小 `DiscoverResult`。它不发任何网络请求——用户已经声明了用哪个版本。

`negotiate_auto` 在 `_probe.py` 里，是整个客户端最精巧的一段策略。它的模块 docstring 就把策略写清楚了：**denylist 而非 allowlist**——「Anything that is not positive evidence the peer is a modern MCP server falls back to the legacy `initialize` handshake」。于是：

- 任何 `MCPError` 都退回旧协议，只有一种例外：`-32022`（`UNSUPPORTED_PROTOCOL_VERSION`）且 `supported` 列表**不含任何握手时代版本**（即服务端只支持现代版本且与本客户端无交集）→ 这是真不兼容，抛错。
- 非 `MCPError` 的异常（网络、连接、anyio 取消）**一律传播**。docstring 的理由很到位：「an outage or in-process bug is never an era verdict」——服务端挂了不代表它不支持新协议。
- 成功拿到 `DiscoverResult` 但 `supported_versions` 里没有现代版本 → 当作**旧协议的明确广告**，退回 `initialize`。注释点名了实例：go-sdk 的 stateful streamable 默认就这样。
- 还有一个反直觉的分支：**退回的 `initialize` 本身可能被回 `-32022`**。原因是「a probe that timed out client-side but succeeded on a slow-starting server locked the connection modern before the pipelined `initialize` arrived」。这时那个错误码本身就是现代证据，于是用交集版本**再探一次**。所以循环写了两次尝试，注释解释第二次探会因为没有冷启动而很快返回。

**`stdio_client` 的一处顺序意识。** `_create_platform_compatible_process` 之后紧接着一句注释：「The spawn succeeded; no awaits until the task group is entered, or a cancellation delivered in the gap would leak the live process」。这是一个真实的泄漏窗口：进程已经起来了，但还没登记到任何会被取消清理的结构里，此时若被取消，子进程就成了孤儿。stdlib 的 `subprocess` 不做这个保证，所以只能靠代码顺序约束。

`StdioServerParameters` 的字段也值得一提：`command`、`args`、`env`、`cwd`、`encoding`、`encoding_error_handler`。`env` 的 docstring 说它「merged over `get_default_environment()`」——即默认继承，只覆盖给出的键。`encoding` 默认 `"utf-8"`，这与服务端 stdio 那边强制 UTF-8 相呼应。

**`StreamableHTTPTransport` 管理三类状态。**

- `session_id`：从响应头的 `mcp-session-id` 提取（`_maybe_extract_session_id_from_response`），此后每次请求都带上。
- `_protocol_version_header`：从每个 stamp 过的消息元数据里**同步地**（在 `post_writer` 循环里）捕获，「so the cache always reflects wire order（a POST task's scheduling is arbitrary）」。它被注释说明有三种用途：给不带 per-message 头的出站 HTTP（传输内部的 GET/DELETE、绕过了会话 stamp 的响应/错误 POST）复用版本；供 `_consume_modern_cancellation` 判断时代；以及在出队 `initialize` 消息时**清空**，防止探针期间的值泄漏到握手上。
- `_in_flight_posts`：把每个请求的 POST 包在一个 `CancelScope` 里，配合 `_InFlightPost.modern` 记录「这条请求发出时的时代」。注释解释了为什么要缓存时代：「a later cancel frame is interpreted under the era the request actually ran with, not whatever the cache says by then」。

**重连与恢复是两条不同的路。** 先说清区别：

- **POST 的响应流断了**：`_handle_reconnection` 带 `Last-Event-ID` 重连，最多 `MAX_RECONNECTION_ATTEMPTS = 2` 次。它的注释强调了一句：「Only requests reconnect: every caller arrives from a request's response stream」，所以它断言消息是 `JSONRPCRequest` 并取它的 id 用于最终应答。
- **恢复请求本身**：`_handle_resumption_request` 走 GET + `Last-Event-ID`（来自 `metadata.resumption_token`），缺 token 就抛 `ResumptionError("Resumption request requires a resumption token")`。
- **独立 GET 流**：另有一套循环，用 `retry_interval_ms` 或 `DEFAULT_RECONNECTION_DELAY_MS = 1000` 作为退避。

重连的实现里有两处细节值得记：

1. **重置尝试计数**：若重连成功建立流、又再次断开且仍无响应，递归调用时传 `0` 作为 attempt，注释是「reset attempt counter」。也就是说「反复断」不会被 2 次上限卡死——上限约束的是「连不上」，不是「连上了又断」。这是一条容易被写错的策略。
2. **必须给放弃一个明确的应答**：达到上限时调 `_resolve_abandoned_request`，注释的理由是「a request with no read timeout（a listen stream）would otherwise hang its caller forever」。它伪造一条带该请求 id 的 `JSONRPCError` 塞进读流，让等待方得到错误而不是永远挂着。

还有一个区分：`httpx2.SSEError` 若发生在**已经确认是 SSE 响应**之后，直接按失败结算（不再重连）；若还没确认（说明这次 GET 本身就没成功），才递增 attempt 重连。

**缓存身份的派生有一个安全细节。** `_strip_userinfo` 从 URL 的 authority 里剥掉 userinfo，注释写「Credentials must not enter cache-key material; any further normalization could merge distinct servers」。实现上刻意**不用 `urlsplit`**：注释说它会在解析前剥掉内嵌的 tab/CR/LF，从而让切片错位。所以这里用纯文本的 `find` 定位 `//`、再用 `/`、`?`、`#` 找 authority 的终点，最后 `rpartition("@")` 取最后一个 `@` 之后的部分。这类手工解析通常是为了避开库的隐式规范化——它宁可保持字节精确。

派生出的 `target_id` 再经 `hashlib.sha256(...).hexdigest()` 变成 `arm_id`，注释明确写「Only the hash below leaves this scope - the raw identity may carry credentials; never log or store it」。若没有可用身份（进程内服务端或自定义 `Transport`）且用户给了自定义 store，就抛 `ValueError` 并解释原因：「their entries in a shared store could never be served to another client」——随机身份写进共享存储永远命不中，所以这是配置错误而不是可容错情形。

## 关键取舍

**源码事实：** 连接路径在 `__post_init__` 里定下来，只依赖 `server` 的形状；`mode` 与 `raise_exceptions` 留到调用时才读。

**源码事实：** 进程内 `Server` 在 `legacy` 模式走 `InMemoryTransport`（有 JSON-RPC 框架），其他模式走 `DirectDispatcher` 对（无流、无框架、无 initialize 握手）。

**源码事实：** `__aenter__` 用局部 `AsyncExitStack` + `pop_all()`，使握手失败时传输被拆掉且 `_session` 保持 `None`。因此 `_session is not None` 蕴含版本与能力已就绪。

**源码事实：** `mode='auto'` 的探针策略是 denylist：只有「-32022 且 `supported` 只有现代版本且无交集」才抛错，其余 `MCPError` 全退回 `initialize`；非 `MCPError` 异常一律传播。

**源码事实：** 退回的 `initialize` 若被回 `-32022`，会用一个交集版本再探一次（因为该错误码本身是现代证据）。

**源码事实：** POST 响应流的重连上限是 2 次；但「重连成功后再次断开」会重置计数。达到上限时必须伪造一条错误应答，否则无超时的等待方会永久挂起。

**源码事实：** 缩略 URL 中的 userinfo 是为了让缓存键材料不含凭证，且刻意手写解析以避开 `urlsplit` 的隐式规范化。

**架构推论：** 「构造期决定连接方式」让 `Client` 的用户代码可以完全不知道传输细节：`Client(url)` 与 `Client(params)` 与 `Client(server_instance)` 用法一致，`async with` 之后拿到的是同一个 `ClientSession` 接口。代价是所有分支都在 `__post_init__` 里，读代码时要一直看到一个函数把四种形状摊平。

**架构推论：** 进程内现代路径跳过 JSON-RPC 框架，是一个**测试友好性换真实性**的选择。它让端到端测试快且确定性高（没有序列化、没有流），代价是这条路径不验证线上形状——所以 legacy 模式仍然保留，用来覆盖序列化路径。两种模式并存正是这个取舍的显式化。

**架构推论：** denylist 式的版本协商是一个「默认乐观」的选择：假定对方可能支持新协议，只有拿到正面反证才放弃。这比 allowlist（列出一堆错误码逐个判断）更稳健，因为未知错误码会被归入「不影响时代判断」而不是「未知所以失败」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/client/client.py` | `Client` | 高层门面：形状派发连接器、mode 校验、缓存与扩展折叠 |
| `src/mcp/client/client.py` | `_connect_transport` | 流式传输连接器：进传输、包 `JSONRPCDispatcher` |
| `src/mcp/client/client.py` | `_connect_inproc` | 进程内两条路径：`InMemoryTransport` 与 `DirectDispatcher` 对 |
| `src/mcp/client/client.py` | `Client.__aenter__` | 局部退出栈 + `pop_all()`，握手成功后才发布会话 |
| `src/mcp/client/client.py` | `_fold_extensions` | 折叠扩展贡献，检测标识符/结果标签/通知方法的重复 |
| `src/mcp/client/client.py` | `_evicting_message_handler` | 通知到达时驱逐缓存，缓存故障不阻断投递 |
| `src/mcp/client/client.py` | `_strip_userinfo` / `_synthesize_discover` | 缓存键材料去凭证 / 版本钉住时的最小 DiscoverResult |
| `src/mcp/client/_probe.py` | `negotiate_auto` | denylist 式时代协商，含握手回 -32022 的再探分支 |
| `src/mcp/client/stdio.py` | `StdioServerParameters` | 子进程传输的参数：命令、参数、环境、工作目录、编码 |
| `src/mcp/client/stdio.py` | `stdio_client` | 起子进程并接到流上，注意 spawn 后到任务组之间的取消窗口 |
| `src/mcp/client/streamable_http.py` | `StreamableHTTPTransport` | 客户端 HTTP 传输：会话 id、版本缓存、在飞请求表 |
| `src/mcp/client/streamable_http.py` | `_handle_reconnection` | `Last-Event-ID` 重连，上限与「连上又断」的计数重置 |
| `src/mcp/client/streamable_http.py` | `ResumptionError` | 恢复请求缺少 token 时的错误 |

## 参考

- [mcp-python-sdk 源码：client/client.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/client/client.py)
- [mcp-python-sdk 源码：client/streamable_http.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/client/streamable_http.py)
- [MCP 规范：客户端传输](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports)
- [anyio 文档：AsyncExitStack 与任务组](https://anyio.readthedocs.io/en/stable/)

## 与主核 / AI 的关系

「按连接串的形状选择驱动」这件事，主核里对应的是**连接字符串与后端的选择**：`host=... port=...`、`dbname=...` 或一个 unix socket 路径，客户端库据此走 TCP 或本地 socket。两边都把「怎么连」编码进了「连什么」的表示里，代价相同——新增一种连接方式就要改那个派发函数。

`__aenter__` 里 `pop_all()` 的用法值得作为资源管理的范式记住。它的本质是「**先在局部栈里做完所有可能失败的事，全部成功后才把责任转移出去**」。主核里对应的是「先建立并验证连接、成功后再生效到连接池」——两边的目标都是让「已发布的状态」与「已就绪的资源」严格对应。

对 AI 工程的实际意义在于三处可借鉴的判断。第一，**版本协商用 denylist**：只有正面反证才降级，网络错误不算证据——这条原则对任何「探测能力再决定走哪条路」的代码都适用。第二，**重连的终止必须给调用方一个应答**（这里伪造一条 JSONRPCError），否则无超时的等待方永久挂起；这比「抛出超时」更精确，因为调用方是按请求 id 等待的。第三，**缓存键材料要先去凭证**，且宁可手写解析也不引入会做隐式规范化的库——一旦 URL 被规范化，两个不同的服务端可能被合并成同一个缓存身份，这是很难发现的数据串号 bug。
