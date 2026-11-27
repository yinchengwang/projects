---
project: mcp-python-sdk
feature: streamable-http-server
title: Streamable HTTP 服务端传输
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/server/streamable_http.py
    symbols: [StreamableHTTPServerTransport, EventStore, EventMessage]
  - path: src/mcp/server/streamable_http_manager.py
    symbols: [StreamableHTTPSessionManager, StreamableHTTPASGIApp]
---

# Streamable HTTP 服务端传输

## 一句话职责

把「一次 HTTP 请求」和「一条长活的 JSON-RPC 会话」缝在一起：POST 进来一个请求，回一条 SSE 流或一个 JSON 体；GET 撑开一条独立的下行流；两边都靠一张「请求 id → 内存流」的路由表分发，靠可选的 `EventStore` 支持断线重放。

## 机制是什么

Streamable HTTP 要同时满足两个矛盾的诉求：

- **HTTP 是无状态的**：每个请求独立，随时可能换一台机器。
- **MCP 会话是有状态的**：初始化协商、服务端主动发请求、进度通知，都需要跨请求记住一个对端。

它的解法是三层：

| 层 | 职责 | 位置 |
|---|---|---|
| 会话管理 | 按 `mcp-session-id` 把请求路由到已存在的传输实例，或开新的 | `StreamableHTTPSessionManager` |
| 传输 | 处理单个会话内的 POST / GET / DELETE，维护请求 id 到内存流的映射 | `StreamableHTTPServerTransport` |
| 服务端循环 | 一条长跑的任务，从读流取消息、往写流投结果 | `connect()` 里 yield 的两条流 |

关键设计是 `connect()` 里那个 `message_router`：它从 `write_stream_reader` 不断取服务端要发出的消息，**按消息的归属决定投给哪条请求流**——若是一个带 id 的响应，就投给 `str(message.id)` 对应的流；若是带 `related_request_id` 的通知，就投给它；否则一律投给 `GET_STREAM_KEY`（独立下行流）。

于是同一台服务端可以：一个 POST 的响应走它自己的 SSE 流、一条进度通知走同一条流、一条无关的日志通知走 GET 流——三条路互不干扰。

## 怎么实现的

**三种 HTTP 方法的语义。**

- **POST** 携带一条 JSON-RPC 消息。若是请求（有 id），要么回一条 SSE 流（默认），要么回一个 JSON 体（`is_json_response_enabled`）；若是通知或响应（无 id），**立刻回 202 Accepted 然后把消息交给服务端处理**——注意注释强调的是「先发响应再处理」，因为通知没有回包，先应能降低客户端感知延迟。
- **GET** 撑开独立 SSE 流，让服务端能主动推送。一个会话只允许一条，重复开返回 **409 Conflict**。
- **DELETE** 终止会话。

**`check_accept_headers` 处理 RFC 7231 的通配。** 它返回 `(has_json, has_sse)`：`*/*` 两者都算有，`application/*` 算 json，`text/*` 算 sse。这避免了「客户端只写 `*/*` 就被拒」的常见坑。

Accept 的校验随模式不同：JSON 模式只要求 `application/json`；SSE 模式**同时要求 `application/json` 与 `text/event-stream`**。后者看起来奇怪但符合协议——客户端必须声明它两种都接受，因为服务端可能在流里先发一个事件再决定。

**Content-Type 的解析有一处细节。** `_check_content_type` 把头的第一个分号前的部分按逗号再切一次，逐个精确比对 `application/json`。所以 `application/json; charset=utf-8` 通过，而 `application/json-patch+json` **不**通过——这是精确匹配，不是前缀匹配。

**body 只读一次。** `body = await request.body()` 之后用 `pydantic_core.from_json` 解析，再 `jsonrpc_message_adapter.validate_python(..., by_name=False)` 得到具体消息类型。两个阶段的错误分得很清：JSON 语法错 → **400 + `PARSE_ERROR`**；JSON 合法但不符合消息形状 → **400 + `INVALID_PARAMS`**。这个区分对客户端很重要，前者说明「你的序列化坏了」，后者说明「字段不对」。

**initialize 请求有一条特殊路径。** 若消息是 `method == "initialize"`，代码**跳过**常规的 session 校验，改为只在「请求带了 session id 且与当前不符」时才 404。理由是不言而喻的：初始化请求本来就不该带（或带对的）会话 id，它是建立会话的那一步。

**SSE 模式的投递顺序是刻意安排的。** 关键代码段：

1. `priming_event = await self._mint_priming_event(request_id, protocol_version)` ——**在建立任何 per-request 状态之前**先向事件存储写一条「起始游标」。注释给了两个理由：`EventStore.store_event` 是用户代码、可能抛异常，此时外层直接回 500 且没有需要清理的东西；同时这一步严格早于派发，所以**存储顺序等于线上顺序**。
2. 建两条内存流：`sse_stream_writer/reader`（`SSEEvent`，缓冲 0）与 `_request_streams[request_id]`（`EventMessage`，缓冲 `REQUEST_STREAM_BUFFER_SIZE = 16`）。
3. 构造 `EventSourceResponse`，把 `_run_sse_writer` 作为 `data_sender_callable`。
4. 在一个 task group 里同时启动响应与「把消息送进读流」——注释写「先发响应建立 SSE 连接，再发消息给服务端处理」。

`_run_sse_writer` 的循环里有一个终止条件值得注意：发出一个 `JSONRPCResponse | JSONRPCError` 之后**就 `break`**。因为一次 POST 只期待一个响应，拿到就关流。通知可以在响应之前任意多条，所以流的内容是「若干通知 + 一条响应」然后结束。

**JSON 模式的处理路径完全不同且更窄。** 它在 `_request_streams` 里建流后只 `receive()` **一次**，拿到一个事件就序列化成 JSON 体返回。注释明确写道：`message_router` 只会把它自己的响应投进这条流，其他任何 scoped 到该请求的消息「在 JSON 模式下没有线上形式」。

这个模式带来一处必须显式关掉的通道：`_message_metadata` 是**唯一**给 `ServerMessageMetadata` 打 `can_send_request` 标记的地方，注释说这是为了保证「没有任何构造点会忘记它」——JSON 体只能承载响应，所以 `can_send_request=not self.is_json_response_enabled`。代价写在构造函数 docstring 里：JSON 模式下服务端主动发起的请求会抛 `NoBackChannelError`，相关通知被丢弃。

`message_router` 里也有一处对称的丢弃：JSON 模式下遇到带 `related_request_id` 的消息，直接 `continue` 并打日志「Dropped message related to request ... in JSON mode」。注释点名了 issue 号，说明这是一次修复——之前这类消息会被塞进流里然后没人读。

**可恢复性由 `EventStore` 提供，而它的门槛是版本。** `EventStore` 是抽象类，只有两个方法：`store_event(stream_id, message)` 返回事件 id，`replay_events_after(last_event_id, callback)` 重放并返回流 id。`message` 允许为 `None`，那是 priming 事件的专用写法。

两处都做了版本门：`_mint_priming_event` 与 `_create_session_message` 里的 close 回调，都要求 `is_version_at_least(protocol_version, "2025-11-25")`。注释解释了原因：**老客户端解析不了空 data 的 SSE 事件**（priming 事件正是 `{"id": ..., "data": ""}`），也收不到 priming 事件、因此无法在流被提前关闭后恢复。

`_replay_events` 处理 GET 请求带 `Last-Event-ID` 的情形：调 `event_store.replay_events_after` 把旧事件逐条发出去，拿到流 id 后**接着把实时消息也接上**——注释坦白了这里有一个「重放转实时」的时序窗口，是既存问题、单独跟踪。它还顺手做了两件事：把 `sse_stream_writer` 注册进 `_sse_stream_writers` 让 `close_sse_stream()` 能关它，以及重放结束后清理该流。

**`close_sse_stream` 是主动断流的入口。** 它按 request id 取出 SSE writer 并关闭，从而结束这一次 HTTP 响应——但事件仍在 `EventStore` 里，客户端带 `Last-Event-ID` 重连会重放。这被用来实现长操作的轮询：服务端定期断流让客户端重连，重连时把积压的事件带走。`retry_interval` 就是给这条路径用的（写在 priming 事件的 `retry` 字段里），而 docstring 说它**只在使用 `event_store` 时有效**。

**空闲超时用一个共享的 `CancelScope` 实现。** 构造函数的 `idle_timeout` 若给了正值，`connect()` 会建一个 `anyio.CancelScope` 存到 `self.idle_scope`。`handle_request` 的包装逻辑是：

- 没有 `idle_scope` 或没设超时 → 直接处理。
- `idle_scope.cancel_called`（空闲时间已耗尽、宿主正在结束会话）→ 先 `terminate()` 再处理请求，这样请求会得到「会话已终止」的 404 而不是走进一个即将消失的消息循环。
- 否则先把 `deadline` 设为 `math.inf`（**挂起倒计时**）、计数 `_requests_in_flight += 1`，处理完再减一；减到 0 时把 `deadline` 设成 `anyio.current_time() + idle_timeout`。

「有请求在飞就挂起倒计时」这条注释点明了一个容易被忽略的语义：**一条打开的 GET 流也算「在飞」**，所以只要客户端连着，会话就不会因为空闲被回收。

**会话管理器的两态。** `StreamableHTTPSessionManager` 支持 `stateless=True`，此时每个请求建一个全新传输、没有 session id、**也不接 `event_store`**——不可恢复。stateful 模式下所有会话存在 `_server_instances: dict[session_id, transport]`。

stateful 的请求分流（`_handle_stateful_request`）有四个分支，按顺序：

1. **带了 session id 且存在**：先校验归属——`requestor != _session_owners.get(session_id)` 时返回 **404**，且注释写明「一个会话只能被创建它的凭证使用。响应要表现得和会话不存在完全一样」。这是防止会话 id 泄漏被他人接管。通过后交给传输处理；若处理完 `transport.is_terminated`（说明是 DELETE），立即 `_discard_session`。
2. **没带 session id**：在 `_session_creation_lock` 里调用 `_admit_session` 做准入（会话数上限与注册）。注释解释了为什么请求本身在锁外服务：**一个慢发 opening request 的客户端不该拖住其他客户端**。超限返回 **503**。
3. **带了未知/过期 id**：404。源码留了一条 TODO 与 issue 链接，承认错误码待规范澄清。

`_admit_session` 用 `uuid4().hex` 生成 id，并在会话数达上限时返回 `None`。

**`_serve_opening_request` 处理「开新会话的那一次请求」这个尴尬时刻。** 它先用 `task_group.start(run_server)` 在后台起会话任务，再用 `_send_and_report_status` 让传输去回答这次请求，**并观察它回的状态码**；只有 `status is not None and status < 400` 才算会话真的建立，否则在 `finally` 里 `_discard_session`。

这条判据的背后是：没有 session id 时只有 initialize 能成功，若这次被拒/失败/取消，那就什么都没建立，不该留一个带后台任务的空会话。`_send_and_report_status` 的实现就是包一层 `send`，抓 `http.response.start` 里的 `status`。

`_discard_session` 有两条约束，注释都点了：**先**把会话从两张表里删掉（在任何 `await` 之前），这样它的 id 从调用那一刻起就答 404；然后**用 `anyio.CancelScope(shield=True)` 包住 `transport.terminate()`**，保证即使调用方正在被取消，终止也能跑完。

**`serve_loop` 而不是 `Server.run()`。** `run_server` 里注释说明：用 `serve_loop` 是为了复用管理器**已经进入的 lifespan**，而不是每个会话重新进入一次。所以 lifespan 是管理器级（`run()` 里一个 `lifespan` 上下文），会话级只有一个消息循环。

**`StreamableHTTPASGIApp` 只是一个薄的 ASGI 适配器**：`__call__` 直接转给 `manager.asgi_app`。它存在的意义是让管理器能作为 Starlette 的 ASGI 应用挂载。

## 关键取舍

**源码事实：** 三种 HTTP 方法分工固定：POST 传消息、GET 撑独立下行流（每会话限一条，重复 409）、DELETE 终止会话。

**源码事实：** 通知与响应（无 id）收到后**先回 202 再处理**。

**源码事实：** SSE 模式下，一次 POST 的流以「一条 `JSONRPCResponse`/`JSONRPCError` 发出后 `break`」结束。

**源码事实：** JSON 响应模式关掉了请求作用域的回信道：`can_send_request` 全部由 `_message_metadata` 一处统一置位，带 `related_request_id` 的消息在 `message_router` 里被显式丢弃。

**源码事实：** priming 事件在任何 per-request 状态建立**之前**写入事件存储，以保证存储顺序等于线上顺序，且 `EventStore` 抛错时不留下需要清理的状态。

**源码事实：** 可恢复能力被 `2025-11-25` 版本门槛限制：老客户端解析不了空 data 事件，也收不到 priming 事件。

**源码事实：** 会话只能被创建它的凭证使用，凭证不匹配时返回 404 且表现得像会话不存在。

**源码事实：** 会话准入在锁内、请求服务在锁外；开新会话的请求若没回 2xx/3xx 就立即丢弃会话。会话丢弃先删表项再屏蔽式终止传输。

**架构推论：** 「一张请求 id → 内存流的路由表」是这套传输的核心抽象。它把「一条 HTTP 响应」变成「一次订阅」：POST 建立订阅、消息路由把相关的消息投进订阅、响应发完就退订。这也解释了为什么 GET 流要用一个伪 id（`GET_STREAM_KEY`）——它就是「不属于任何请求的那条订阅」。

**架构推论：** 可恢复性只能靠外部 `EventStore` 实现，且必须是**用户提供**的。SDK 无法自己持久化事件，因为那需要跨进程存储；而一旦跨进程，就必须假定多实例部署，于是事件 id 与流 id 的生成规则也成了用户的契约。

**架构推论：** 用 `CancelScope.deadline` 实现空闲超时而不是起一个定时任务，节省了一条常驻任务；代价是所有「是否有请求在飞」的记账必须精确——漏加一次计数就永不超时，漏减一次就提前超时。这里的写法（`handle_request` 单一入口、`try/finally` 配对）是保证不漏的正确方式。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/server/streamable_http.py` | `StreamableHTTPServerTransport` | 单会话传输：三种方法分派、请求流表、空闲超时 |
| `src/mcp/server/streamable_http.py` | `StreamableHTTPServerTransport.handle_request` | 入口，包装空闲计数的挂起与重启 |
| `src/mcp/server/streamable_http.py` | `_handle_post_request` | 三阶段解析、initialize 特例、SSE/JSON 两种回法 |
| `src/mcp/server/streamable_http.py` | `_handle_get_request` | 独立下行流，含 `Last-Event-ID` 重放分派 |
| `src/mcp/server/streamable_http.py` | `connect` | 建流并启动 `message_router` |
| `src/mcp/server/streamable_http.py` | `message_router` | 按响应 id / `related_request_id` / 缺省 GET 流分流 |
| `src/mcp/server/streamable_http.py` | `EventStore` | 抽象可恢复存储：`store_event` + `replay_events_after` |
| `src/mcp/server/streamable_http.py` | `EventMessage` | 消息 + 可选事件 id |
| `src/mcp/server/streamable_http.py` | `_mint_priming_event` | 在派发前写起始游标，带版本门与 `retry` 字段 |
| `src/mcp/server/streamable_http.py` | `check_accept_headers` | RFC 7231 通配处理 |
| `src/mcp/server/streamable_http_manager.py` | `StreamableHTTPSessionManager` | 会话表、准入、stateless 分支、lifespan 复用 |
| `src/mcp/server/streamable_http_manager.py` | `_handle_stateful_request` | 四分支分流与凭证归属校验 |
| `src/mcp/server/streamable_http_manager.py` | `_serve_opening_request` | 起会话任务并按其状态码决定是否保留 |
| `src/mcp/server/streamable_http_manager.py` | `StreamableHTTPASGIApp` | 把管理器包成 ASGI 应用 |

## 参考

- [mcp-python-sdk 源码：streamable_http.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/streamable_http.py)
- [mcp-python-sdk 源码：streamable_http_manager.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/streamable_http_manager.py)
- [MCP 规范：Streamable HTTP 传输](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports#streamable-http)
- [RFC 7231：Accept 头与媒体类型通配](https://www.rfc-editor.org/rfc/rfc7231#section-5.3.2)

## 与主核 / AI 的关系

「一张句柄表把连接与请求关联起来」这套结构，主核里对应的是**后端进程的连接与会话管理**：客户端连上来，服务端记住这个连接上挂着一个会话、会话上有事务，后续每条命令都按连接找到对应的会话。两边都要回答「一个连接能承载多少并行的工作」——主核用后台进程 + 事务上下文，这里用「请求 id → 内存流」的路由表。

更直接的对应是**异步查询与游标**。一次长查询在主核里返回游标、后续 `FETCH` 拉结果；这里一次 POST 返回 SSE 流、后续事件往上追加。两边都必须在「客户端可能中途断掉」的前提下设计：主核靠游标超时回收，这里靠 `EventStore` 重放 + `idle_timeout` 回收。

`_serve_opening_request` 那条「按 HTTP 状态码判断会话是否真的建立」值得作为通用手法记住。它把「意图」与「结果」分开：起任务只是意图，状态码才是结果。主核里同类场景是「连接建立后立刻发认证包，失败就断开」——同样是**用第一轮的结果反向决定资源是否保留**。

对 AI 工程的实际意义在于：Streamable HTTP 是远程 MCP 部署的主流形态，而它的复杂度几乎全在「一次逻辑调用跨越多个 HTTP 请求」这一件事上。这份实现给出的三条可借鉴做法是：把响应做成订阅（请求 id → 流）、把可恢复性外置成接口（`EventStore` 由用户实现）、以及把每一种降级都在类型层面显式化（`can_send_request` 收在一处、老版本客户端不拿 priming 事件）。最后一条尤其重要——一个「有时能主动发请求、有时不能」的传输，若不在元数据里显式标注，调用方只能靠运行时异常去发现。
