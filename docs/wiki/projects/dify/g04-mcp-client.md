---
project: dify
feature: mcp-client
title: MCP 客户端与传输
page_type: feature
schema_version: 1
refs:
  - path: api/core/mcp/mcp_client.py
    symbols: [MCPClient]
  - path: api/core/mcp/client/streamable_client.py
    symbols: [StreamableHTTPTransport, StreamableHTTPError]
  - path: api/core/mcp/session/client_session.py
    symbols: [ClientSession]
---

# MCP 客户端与传输

## 一句话职责

把「连上一个 MCP 服务器并调它的工具」拆成三层：`MCPClient` 选传输并管生命周期、`StreamableHTTPTransport` 把 JSON-RPC 消息变成 HTTP 请求与 SSE 流、`ClientSession` 在其上跑 MCP 的会话协议（初始化握手、请求/响应配对、分页）。

## 机制是什么

三层的分工非常清楚，每层只解决一类问题：

| 层 | 载体 | 解决的问题 |
|---|---|---|
| 门面 | `MCPClient` | 选哪种传输、连接失败怎么回退、`ExitStack` 管所有上下文、工具发现的上限 |
| 传输 | `StreamableHTTPTransport` | 把「发一条 JSON-RPC 消息 / 收一批回包」映射到「POST + 读 SSE」；会话 id、断点续传、终止 |
| 会话 | `ClientSession` | MCP 协议语义：`initialize` 握手、`tools/list` 分页、`tools/call`、服务端反向请求 |

**关键点是中间那一层用的是「双队列 + 后台线程」模型，不是「请求-响应函数」。**

`streamablehttp_client` 起一个 `ThreadPoolExecutor(max_workers=2)`：一个线程跑 `post_writer`（从 `client_to_server_queue` 取消息发出去），一个线程跑 `handle_get_stream`（维持服务端推送的 GET SSE 流）。对上层只暴露两个 `queue.Queue` 与一个取 session id 的回调。

**所以 `ClientSession` 看到的不是 HTTP，而是两个队列**——它对传输层一无所知。这也是为什么同一个 `ClientSession` 能同时配 SSE 传输与 StreamableHTTP 传输：**只要对方提供一对队列，它就是「流」**。

## 怎么实现的

### 门面：`MCPClient`

**传输选择靠 URL 路径的最后一段，且默认值不是首选。**

```
connection_methods = {"mcp": streamablehttp_client, "sse": sse_client}
path = urlparse(self.server_url).path or ""
method_name = path.rstrip("/").split("/")[-1] if path else ""
if method_name in connection_methods:
    client_factory = connection_methods[method_name]
    self.connect_server(client_factory, method_name)
else:
    try:
        self.connect_server(sse_client, "sse")     # 先试 sse
    except (MCPConnectionError, ValueError):
        self._exit_stack.close()                   # 关掉半开的连接
        self._exit_stack = ExitStack()
        self.session = None
        self.connect_server(streamablehttp_client, "mcp")   # 再退到 mcp
```

路径以 `/mcp` 结尾就走 StreamableHTTP、以 `/sse` 结尾就走 SSE，其余一律**先试 SSE、失败再试 StreamableHTTP**。这与「默认用 StreamableHTTP」的直觉相反，是历史原因（SSE 是较早的传输）留下的顺序。

**回退前必须清掉半开的连接**，注释写得很清楚：「The SSE transport and `ClientSession` can already be entered when the handshake fails. Close that partial connection before opening the fallback so both transports are never live at once.」——**握手失败时上下文已经进入，不关掉就会出现两个传输同时活着**，连接与线程都会泄漏。

这段清理之后是 `connect_server(streamablehttp_client, "mcp")`，但如果这一步也失败，异常会直接冒出去（没有再包一层），所以最终的失败类型是 `MCPConnectionError` 或 `ValueError`。

**请求头支持从入站请求里做占位符替换。**

```
pattern = re.compile(r"\{\{\s*request\.headers?\.(.+?)\s*\}\}", re.IGNORECASE)
```

形如 `{{request.header.authorization}}` 的占位符会被替换成**当前 HTTP 请求**里对应头的值。这让「MCP 服务器的凭据用调用方的 token」成为可能，而不必在配置里存死凭据。

三个限制要看清：

1. 只在 `has_request_context()` 为真时生效——**后台任务/Celery 里没有请求上下文，占位符不会被替换**，会原样发出去；
2. 只处理 `str` 类型的头值；
3. 取不到的头返回**空字符串**而不是保留占位符——所以配置写错头名会静默变成空凭据。

**工具发现有三道上限，都是为了「服务器不可信」这个前提。**

```
_MAX_DISCOVERED_TOOLS = 10_000
_MAX_TOOL_DISCOVERY_REQUESTS = 100
```

循环里的检查依次是：

1. 累计工具数超过 10000 → 抛错；
2. **恰好等于 10000 但还有下一页** → 抛错（「reached the limit with more pages」）；
3. `nextCursor` 为 `None` → 正常返回；
4. **`nextCursor` 与已见游标重复** → 抛错（防服务器无限返回同一个游标）；
5. **请求次数达到 100 但还有下一页** → 抛错。

第 4 条（重复游标检测）是必需的：**分页协议里最经典的坑就是服务器返回一个循环的 cursor**，客户端若无检测就永远转下去。第 2 条也很细——它区分了「刚好一万个且收完」与「刚好一万个还有更多」，后者必须拒绝而不是静默截断。

### 传输：`StreamableHTTPTransport`

**它把「一条消息的往返」拆成五种响应处置，这是这一页最值得逐条对照协议的部分。**

`_handle_post_request` 先 `stream("POST", ...)`，然后按状态码分派：

- `202 Accepted` → 直接返回（通知类消息，服务端不回应）；
- `204 No Content` → 直接返回；
- `404 Not Found` → **构造一个 JSON-RPC 错误塞进接收队列**，而且文案分两种：初始化请求时是「MCP server URL returned 404 Not Found: ... — verify the server URL is correct and the server is running」，非初始化时是「Session terminated by server」；
- 其它 → `raise_for_status()`。

**404 的两种文案是有意区分的**：初始化阶段 404 说明 URL 配错了（用户问题）；之后 404 说明会话被服务端终结了（协议行为）。**同一个状态码，语义完全不同，所以要分支。**

成功后的处理遵循协议规定：

```
if isinstance(message.root, JSONRPCRequest):
    content_type = response.headers.get(CONTENT_TYPE, "").lower()
    if content_type.startswith(JSON):   self._handle_json_response(...)
    elif content_type.startswith(SSE):  self._handle_sse_response(...)
    else:                               self._handle_unexpected_content_type(...)
```

**只有 request 才等回包，notification 不等**——注释引了规范：「The server MUST NOT send a response to notifications.」服务端可以回 JSON（一次性）也可以回 SSE（流式），两种都要支持，所以 `Accept` 头是 `application/json, text/event-stream` 两者都声明。

**SSE 读取是可中断的，靠 `stop_event` + 主动关响应。**

```
event_source = EventSource(response)
try:
    for sse in event_source.iter_sse():
        if self.stop_event.is_set(): break
        ...
finally:
    self._unregister_response(response)
```

而 `close_active_responses` 从另一侧把正在读的 `httpx.Response` 关掉，注释是「Close all active SSE connections to unblock threads.」——**因为 `iter_sse` 会阻塞在网络上，光设 `stop_event` 不够，必须把连接关掉才能真正唤醒线程**。

这是「线程模型里做取消」的通用水法：**信号 + 资源关闭双管齐下**。缺前者会在两条事件之间多读一次；缺后者会永远阻塞。

**`client_to_server_queue.put(None)` 是线程终止哨兵。** `streamablehttp_client` 的 `finally` 里先清空队列再放 `None`，然后给两个队列各放一个 `None`，最后 `executor.shutdown(wait=False)`。**用 `None` 而不是 `queue` 的关闭语义**，因为 `queue.Queue` 没有「关闭」概念，消费者只能靠哨兵值退出。

**`terminate_on_close=True` 会在关闭时主动终止服务端会话**（`transport.terminate_session(client)`）——MCP 协议有显式的会话终止语义，不等服务端超时。

### 会话：`ClientSession`

**`initialize` 里有一处「按需声明能力」的判断，是这一层最讲究的地方。**

```
sampling = types.SamplingCapability() if self._sampling_callback is not _default_sampling_callback else None
roots = types.RootsCapability(listChanged=True) if self._list_roots_callback is not _default_list_roots_callback else None
```

注释写的是：「Only set capabilities if non-default callbacks are provided. This prevents servers from attempting callbacks when we don't actually support them.」

**这是「能力声明必须与实际实现一致」的正确做法。** 若无论有没有回调都声明 `sampling` 能力，服务端就会真的发采样请求过来，而默认回调只是返回一个空结果——**看起来能跑，实际是假的**。用「回调是否被自定义」来判断是否声明，让「能力」与「实现」严格对应。

`listChanged=True` 只在自定义 roots 回调时才带上，同样理由：默认实现没人监听变化通知。

**握手后要校验协议版本：**

```
if result.protocolVersion not in SUPPORTED_PROTOCOL_VERSIONS:
    raise RuntimeError(f"Unsupported protocol version from the server: {result.protocolVersion}")
```

然后才发 `notifications/initialized`——**顺序不能反**：初始化通知必须在版本校验通过之后发，否则等于承认了一个自己不支持的会话。

**`call_tool` 与 `list_tools` 都是「带类型期望的 `send_request`」：**

```
def list_tools(self, cursor: str | None = None) -> types.ListToolsResult:
```

`cursor` 是可选参数，**由调用方（`MCPClient.list_tools`）负责循环**。这是有意的分层：会话层只做「一次带游标的请求」，分页策略与上限留给门面层。**于是「分页该不该限流」这件事不需要会话层知道。**

## 关键取舍

**源码事实：** 三层分工为门面（选传输/生命周期/发现上限）→ 传输（POST + SSE、双队列 + 两线程）→ 会话（MCP 协议语义）。

**源码事实：** 传输按 URL 路径最后一段选（`/mcp`、`/sse`），都不匹配时**先试 SSE 再退 StreamableHTTP**，且回退前必须 `_exit_stack.close()` + 重建 + 清空 session，避免两个传输同时存活。

**源码事实：** 请求头支持 `{{request.header.x}}` 占位符替换，仅在 `has_request_context()` 为真时生效，取不到的头替换成空字符串。

**源码事实：** 工具发现上限为 10000 个工具与 100 次请求；检测重复 `nextCursor`；`tool_count == 10000` 且仍有下一页时报错而不是截断。

**源码事实：** `_handle_post_request` 对 202/204/404/其它四种状态分别处理，404 的错误文案在初始化与非初始化两种情况不同。

**源码事实：** SSE 读取靠 `stop_event` 检查 + `close_active_responses` 主动关响应两者配合才能中断阻塞的线程。

**源码事实：** `ClientSession.initialize` 只在回调被自定义时才声明 `sampling`/`roots` 能力；协议版本校验在发 `initialized` 通知之前。

**源码事实：** `list_tools` 的 `cursor` 参数由门面层循环驱动，会话层不管分页策略。

**架构推论：** 「双队列 + 后台线程」把异步的流式传输包装成同步的阻塞接口，代价是**取消需要两套机制**（信号 + 关资源），收益是上层可以用普通阻塞代码写协议逻辑。这是把「异步 IO」适配成「同步语义」的经典权衡，与 f01 里 `apply_nest_asyncio` 解决的是同一类问题的反面（那个是把「已有事件循环」适配成「可同步调用」）。

**架构推论：** 分页上限放在门面层而不是会话层，是正确的职责划分：会话层只表达协议（「带 cursor 请求一次」），策略（限多少、要不要报错）属于产品决定。**把策略上提，让协议层保持可复用**。

**架构推论：** 「只在真正支持时才声明能力」是能力协商协议里最容易被忽视的一条。反向例子随处可见：声明支持 gzip 却不实现解压、声明支持 `SELECT FOR UPDATE` 却只是忽略。**声明的能力等于契约，虚报的代价是对端发起注定失败的请求。**

**架构推论：** 404 两种文案说明「同一个错误码在不同阶段有不同含义」。这一条对任何长连接/会话协议都成立：**「连不上」与「被断开」必须可区分**，否则用户会去查错误的配置。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/mcp/mcp_client.py` | `MCPClient` | 门面：传输选择、回退、生命周期、发现上限 |
| `api/core/mcp/mcp_client.py` | `MCPClient._initialize` | 按路径选传输，失败回退并清理半开连接 |
| `api/core/mcp/mcp_client.py` | `MCPClient.connect_server` | 用 `ExitStack` 托管传输与会话上下文 |
| `api/core/mcp/mcp_client.py` | `MCPClient.list_tools` | 分页循环与三道上限（数量、请求数、重复游标） |
| `api/core/mcp/mcp_client.py` | `MCPClient.invoke_tool` | 转发到会话层 `call_tool` |
| `api/core/mcp/mcp_client.py` | `MCPClient.cleanup` | 关闭 `ExitStack` 并复位状态 |
| `api/core/mcp/client/streamable_client.py` | `StreamableHTTPTransport` | POST/SSE 传输实现 |
| `api/core/mcp/client/streamable_client.py` | `StreamableHTTPTransport._handle_post_request` | 202/204/404 与 content-type 四面处置 |
| `api/core/mcp/client/streamable_client.py` | `StreamableHTTPTransport._handle_sse_response` | SSE 边读边投递，可被 `stop_event` 打断 |
| `api/core/mcp/client/streamable_client.py` | `StreamableHTTPTransport.close_active_responses` | 主动关活动响应以唤醒阻塞线程 |
| `api/core/mcp/client/streamable_client.py` | `StreamableHTTPTransport._maybe_extract_session_id_from_response` | 从初始化响应里取会话 id |
| `api/core/mcp/client/streamable_client.py` | `StreamableHTTPTransport.terminate_session` | 关闭时主动终止服务端会话 |
| `api/core/mcp/client/streamable_client.py` | `StreamableHTTPError` | 传输层错误基类 |
| `api/core/mcp/client/streamable_client.py` | `streamablehttp_client` | 造双队列 + 两线程并暴露给上层 |
| `api/core/mcp/session/client_session.py` | `ClientSession` | MCP 会话协议：握手、请求配对、分页请求 |
| `api/core/mcp/session/client_session.py` | `ClientSession.initialize` | 按需声明能力、握手、版本校验、发 initialized |
| `api/core/mcp/session/client_session.py` | `ClientSession.list_tools` | 单次带游标的工具列表请求 |
| `api/core/mcp/session/client_session.py` | `ClientSession.call_tool` | 工具调用 |
| `api/core/mcp/session/client_session.py` | `ClientSession._received_request` | 处理服务端发起的反向请求 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/mcp/client/streamable_client.py](https://github.com/langgenius/dify/blob/main/api/core/mcp/client/streamable_client.py)
- [MCP 规范：Streamable HTTP 传输](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports)
- [MCP 协议总览](https://modelcontextprotocol.io/docs/concepts/architecture)

## 与主核 / AI 的关系

这套三层结构与主核里「客户端连接数据库」的分层完全同构：连接门面（选 unix socket / TCP / TLS）→ 报文传输（把查询打成协议帧、读回结果流）→ 会话（认证、参数状态、结果集游标）。**MCP 的 `ClientSession` 对应数据库的协议层，`StreamableHTTPTransport` 对应网络传输层**，两者的边界几乎一模一样。

**「双队列 + 线程」vs 数据库的同步 socket。** 数据库驱动的同步客户端直接阻塞在 socket 上读，取消靠发送取消请求 + 关 socket。MCP 客户端因为是 HTTP 上的流式协议，必须用线程把推送流「抽」出来 —— 但**取消手段和数据库驱动是一样的：关资源**。`close_active_responses` 与「关 socket 让阻塞的 read 返回」是同一招。

**分页上限 vs 数据库的游标数量限制。** PostgreSQL 有 `max_open_cursors`，防止客户端无限开游标耗尽服务端资源。MCP 客户端的 10000 工具 / 100 请求上限方向相反（**是客户端防服务端**），但动机相同：**对方是不可信的，必须自己设边界**。重复游标检测尤其值得对照——数据库游标不会循环，但任何「由对端生成续页标记」的协议都要防这个。

**「只在支持时声明能力」vs 数据库的 `pg_settings` 能力协商。** 数据库握手时会协商协议版本与参数（`client_encoding`、`DateStyle`），并且**只协商双方都支持的**。MCP 的 `capabilities` 字段是同一件事的现代形态。dify 这段代码特意用「回调是否被自定义」作为判据，避免虚报——**这是一条比「写死声明」可靠得多的实现原则。**

对 agent 工程四条结论。其一：**把异步流适配成同步接口，取消必须靠「信号 + 关资源」双管**，只设标志位会永久阻塞。其二：**由对端生成的续页标记必须检测循环**，并同时限制「次数」与「总量」两个维度——只限一个都会留下无限转的缺口。其三：**能力声明要与实现严格一致**，虚报会让对端做出注定失败的调用。其四：**同一个状态码在不同阶段含义不同**（404 在握手前是配置错、握手后是会话终结），分支文案要分开，否则用户查错方向。
