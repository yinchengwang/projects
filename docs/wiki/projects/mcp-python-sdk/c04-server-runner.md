---
project: mcp-python-sdk
feature: server-runner
title: 服务端运行器与中间件
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/server/runner.py
    symbols: [ServerRunner, _apply_middleware]
  - path: src/mcp/server/context.py
    symbols: [ServerMiddleware, ServerRequestContext]
---

# 服务端运行器与中间件

## 一句话职责

`ServerRunner` 是**每连接一个的处理器内核**：它把分发层的「`(ctx, method, params)` + 无类型字典」翻译成用户的「类型化 `ServerRequestContext` + 类型化参数」，串起中间件链，并把结果整形回线上字典；`ServerMiddleware` 则是插在这条链上的 `(ctx, call_next)` 钩子。

## 机制是什么

模块 docstring 用三句话定义了它：

> `ServerRunner` 桥接分发层（`on_request` / `on_notify`，无类型字典）与用户的处理器层（类型化 `Context`、类型化参数）。它是一个**纯内核**：持有一个已填充的 `Connection`，把 `connection.protocol_version` / `connection.outbound` 当作既成事实来读。驱动分发器循环与拆除连接住在自由函数 driver 里（`serve_connection`、`serve_loop`、`serve_dual_era_loop`、`serve_one`）；入口构造 `Connection`，driver 拆除它。

「纯内核」这个说法是有约束力的：`ServerRunner` 自己不建连接、不开循环、不做生命周期管理，甚至不认识传输。它由 `@dataclass` 定义，字段只有四个：

```
@dataclass
class ServerRunner(Generic[LifespanT]):
    server: Server[LifespanT]
    connection: Connection
    lifespan_state: LifespanT
    _: KW_ONLY
    init_options: InitializationOptions | None = None
```

`init_options` 的 docstring 说明「默认用 `server.create_initialization_options()`」，即「不传就现算」。

`on_request` / `on_notify` 是 `@cached_property`，返回内部的 `_on_request` / `_on_notify`。缓存的意义在于「同一个可调用对象反复交给 `dispatcher.run()`」，避免每连接重新绑定。

于是整个架构是三段：**driver 造 `Connection` → 内核处理单条消息 → driver 拆 `Connection`**。四个 driver 各对应一种入口形态：`serve_connection`（已有分发器）、`serve_loop`（握手单年代流）、`serve_dual_era_loop`（双年代双工流）、`serve_one`（单次交换）。

## 怎么实现的

**`_on_request` 是这一页的主体，它的结构是一条链。**

先造上下文，再定义内层处理器，再包中间件，最后做收尾（握手提交）：

```
meta = _extract_meta(params)
version = self.connection.protocol_version
ctx = self._make_context(dctx, method, params, meta, version)

async def _inner(ctx: ServerRequestContext[LifespanT, Any]) -> HandlerResult:
    method, params = ctx.method, ctx.params     # 从 ctx 读，让中间件重写的值生效
    ...

call = self._compose_server_middleware(_inner)
result = _dump_result(await call(ctx))
if method == "initialize":
    self.connection.client_params, self.connection.protocol_version = self._negotiate_initialize(params)
return result
```

三处细节值得单独说。

**第一，`_inner` 从 `ctx` 重读 `method` / `params`。** 注释写明了原因：「从 `ctx` 读方法/参数，好让用 `call_next(replace(ctx, ...))` 重写了它们的中间件能影响到查找与处理器」。也就是说中间件不是只能观察，它能改写这次请求的身份。而 `_inner` 的签名收 `ctx` 也正是为此——`_compose_server_middleware` 的 docstring 说「组合出来的可调用在调用时取 `ctx`，于是中间件能为链的余下部分重写它」。

**第二，`_dump_result` 在中间件链**之外**，而 `_serialize` 在链**之内**。这个安排有明确理由，写在 `_serialize` 的 docstring 里：「在中间件链内运行，好让 OpenTelemetry span 把失败的返回形状（不支持的类型、畸形的规范结果）观测成错误，而不是在一个客户端看到失败请求上正常关闭 span——也让一个不调 `call_next` 就短路的中间件拥有自己的结果，**连信封一起**」。

配套的那句注释更直接：「`_inner` 已经产出了线上字典；一个没调 `call_next` 就短路的中间件被信任会返回自己形状良好的结果——包括它自己的响应信封。流水线事后绝不修补它」。这条「短路的中间件自己负责信封」是有风险的约定，但它让短路成为真正的一等能力（比如一个缓存中间件直接返回上次的完整结果）。

**第三，握手的提交在链成功之后。**

```
if method == "initialize":
    # Commit only on chain success, so a middleware veto leaves no state.
    # Race-free: the read loop is parked until this call returns.
    self.connection.client_params, self.connection.protocol_version = self._negotiate_initialize(params)
```

注释给了两个保证：**只在链成功时提交**（中间件否决则不留状态）、**无竞态**（`initialize` 是 inline 方法，读循环停在这里直到返回）。

紧跟的 `TODO` 承认了一个已知缺陷：「这里重读了线上 `params`，所以一个重写了 `ctx.params`（或 `ctx.method`，或不调 `call_next` 就短路）的中间件会让 `connection.protocol_version` 与 `_inner` 产出的 `InitializeResult` 不同步。等 `initialize` 变成内建处理器时解决」。这处「读 `ctx` 与读 `params` 不一致」的矛盾正是「让中间件可改写」与「握手只发生一次」两个目标相撞的地方，而它被诚实地标出来而不是掩盖。

**`_inner` 内部的判定顺序是协议语义的精确表达。**

```
if method in _methods.SPEC_CLIENT_METHODS:
    try:
        _methods.validate_client_request(method, version, params)
    except KeyError:
        raise MCPError(code=METHOD_NOT_FOUND, message="Method not found", data=method) from None
if method == "initialize":
    return self._serialize(method, version, self._handle_initialize(params))
entry = self.server.get_request_handler(method)
if entry is None:
    raise MCPError(code=METHOD_NOT_FOUND, message="Method not found", data=method)
if not self.connection.initialize_accepted and method not in _INIT_EXEMPT:
    raise MCPError(code=INVALID_PARAMS, message="Invalid request parameters", data="")
typed_params = entry.params_type.model_validate({} if params is None else params, by_name=False)
result = await entry.handler(ctx, typed_params)
if isinstance(result, ErrorData):
    raise MCPError.from_error_data(result)
return self._serialize(method, version, result)
```

顺序的含义：

1. **规范方法先做 surface 校验**（注释：「规范方法在查找之前就被 surface 校验，所以畸形参数即使没有注册处理器也是 `INVALID_PARAMS`」），而 `KeyError`（该方法在这个版本不存在）直接变 `METHOD_NOT_FOUND`。
2. **`initialize` 优先于处理器查找**——它是内建路径，不走注册表（这也是 `add_request_handler("initialize", ...)` 被禁的原因）。
3. **没注册处理器一律 `METHOD_NOT_FOUND`，与初始化状态无关**。注释解释了为什么不先判初始化：「JSON-RPC 2.0 把 -32601 保留给『本服务器不提供』，而在握手之前探测服务器的客户端正是按那个码来判断的。所以下面的初始化闸门只对服务器真的提供的方法生效」。
4. **初始化闸门**：未接受初始化且方法不在 `_INIT_EXEMPT`（只有 `ping`）里，抛 `INVALID_PARAMS`。注释说「与联合校验产出的错误形状相同」——这是刻意的向后兼容。
5. **缺 `params` 时用 `{}` 校验**（注释：「有必填字段的仍会拒绝，全可选的处理器拿到带默认值的模型，**永不收到 `None`**」）。
6. **处理器返回 `ErrorData` 时转成异常抛出**，注释：「在链内抛出，好让中间件观测到失败」。这是一个反向的桥：处理器可以用「返回错误数据」而不是「抛异常」来表达失败，而中间件仍然能看见。

**`_on_notify` 与请求路径同构，但所有失败都降级为日志。**

```
if method in _methods.SPEC_CLIENT_NOTIFICATION_METHODS:
    try: _methods.validate_client_notification(method, version, params)
    except KeyError: logger.debug("dropped %r: not defined at %s", method, version); return
    except ValidationError: logger.warning("dropped %r: malformed params", method); return
if method == "notifications/initialized":
    self.connection.initialized.set()
elif not self.connection.initialize_accepted:
    logger.debug("dropped %s: received before initialization", method); return
entry = self.server.get_notification_handler(method)
if entry is None: logger.debug("no handler for notification %s", method); return
try: typed_params = entry.params_type.model_validate({} if params is None else params, by_name=False)
except ValidationError: logger.warning("dropped %r: malformed params", method); return
await entry.handler(ctx, typed_params)
```

三处与请求路径的差别：不存在的通知静默丢弃（不报错，因为通知本就没有应答通道）；`notifications/initialized` 先置位事件再**继续下传**（注释：「上面的 surface 校验已拒掉畸形正文，所以可以提交；继续往下以便注册的处理器观测到一个已初始化的连接」）；参数畸形也只记 warning。

而 `call` 外层还有一道 containment：

```
try: await call(ctx)
except Exception:
    logger.exception("notification handler for %r raised", method)
```

注释：「一个崩溃的处理器不得取消分发器的任务组；中间件先看到从 `call_next()` 抛出的异常」。这与分发器里 `_contained_notify` 是同一道防线的两个位置，刻意重复——因为中间件可能自己重抛。

**`_compose_server_middleware` 只有四行，但方向是承重的。**

```
call = inner
for middleware in reversed(self.server.middleware):
    call = partial(_apply_middleware, middleware, call)
return call
```

`reversed` 让列表中**第一个**中间件位于**最外层**，与 `Server.middleware` 的「outermost-first」约定一致。docstring 补充：「`_on_request` 与 `_on_notify` 共用它，于是同一条中间件链观测每条入站消息。组合出的可调用在调用时取 `ctx`，所以中间件能为链的余下部分重写它」。

`_apply_middleware` 那三行是纯适配：

```
def _apply_middleware(middleware, call_next, ctx) -> Awaitable[HandlerResult]:
    """Adapt one middleware to the `CallNext` shape: bind `call_next`, take
    `ctx` at call time so a rewritten context flows down the chain."""
    return middleware(ctx, call_next)
```

为什么要这个中间层？因为 `partial(_apply_middleware, middleware, call)` 得到的是「还缺 `ctx`」的可调用，而 `CallNext` 的类型是「收 `ctx` 返回 awaitable」。于是链的每一层都是 `CallNext`，同一条 `call(ctx)` 就能穿透任意层。

**`ServerMiddleware` 是一个单方法 Protocol，它的 docstring 解释了类型上的难处。**

```
async def __call__(self, ctx: ServerRequestContext[_MwLifespanT, Any], call_next: CallNext) -> HandlerResult: ...
```

docstring 里几条约束：

- 中间件跑在上下文层，包裹每条入站请求（含 `initialize`、查找、校验、处理器）。
- 「`Server[L].middleware` 持有 `ServerMiddleware[L]`，所以应用特定的中间件能看到 `ctx.lifespan_context: L`」。但「当上下文是可变 `ServerRequestContext` dataclass 时它对 `L` 是**不变**的，所以可复用的中间件应标成 `ServerMiddleware[Any]` 才能在任意 `Server[L]` 上注册」。
- 关于 `initialize`：「提交读的是线上 `params`，所以要否决握手必须在 `call_next()` **之前**抛」。

旁边还有一个 `TODO(maxisbey)`：「等 `_make_context` 重新返回（协变的）`Context[L]` 时，把 `_MwLifespanT` 恢复成 `contravariant=True` 并把 `ctx` 重标成 `Context[_MwLifespanT]`，这样可复用中间件可以是 `ServerMiddleware[object]` 而不是 `ServerMiddleware[Any]`」。这是一处「当前的类型表达被一个临时实现细节拖累」的完整记录。

**`ServerRequestContext` 是给处理器的那份上下文。**

```
@dataclass(kw_only=True)
class ServerRequestContext(Generic[LifespanContextT, RequestT]):
    session: ServerSession
    lifespan_context: LifespanContextT
    protocol_version: str
    method: str
    params: Mapping[str, Any] | None = None
    request_id: RequestId | None = None
    meta: RequestParamsMeta | None = None
    request: RequestT | None = None
    close_sse_stream: CloseSSEStreamCallback | None = None
    close_standalone_sse_stream: CloseSSEStreamCallback | None = None
```

docstring：「由 `ServerRunner._make_context` 为每条入站消息构造。带连接域 `ServerSession`（服务端到客户端请求与通知）、逐请求元数据，以及传输附加的逐消息数据（HTTP 请求、SSE 流关闭回调）」。

它是 `@dataclass(kw_only=True)` 而**不是** frozen——这是有意的：中间件通过 `replace(ctx, ...)` 造新副本，但由于它可变，`ServerMiddleware` 的类型参数只能不变（上一段已述）。`LifespanContextT` 的注释也点明理由：「不变——它参数化一个可变 dataclass 字段」。

`_make_context` 的装配过程里有一处最绕的判断：

```
md = dctx.message_metadata
if isinstance(md, ServerMessageMetadata):
    request = md.request_context
    close_sse_stream = md.close_sse_stream
    close_standalone_sse_stream = md.close_standalone_sse_stream
else:
    request = close_sse_stream = close_standalone_sse_stream = None
session = ServerSession(dctx, self.connection, request_meta=meta if dctx.request_id is not None else None)
```

`request_meta` 那行有条件：注释说「`meta` 带着一个请求的日志级别 opt-in，供会话的日志门禁使用。**通知没有可 opt-in 的请求**（也没有响应流承载日志条目），所以通知的 `_meta` 永不打开这个门」。用 `dctx.request_id is not None` 区分请求与通知，比另设标志更省。

**`_serialize` 是入站分类阶梯的出站对偶。**

docstring 说它「一趟走完整个响应信封，按顺序：缓存提示填上处理器留空的 `ttlMs`/`cacheScope`；核心词汇的规范方法结果经逐版本 surface 校验与筛选（扩展声明的 `resultType` 形状归扩展所有）；2026 年代的结果盖上 `serverInfo` 的 `_meta` 章（规范 #3002）」。

三个步骤各有一处边界：

- 缓存提示的填充顺序：「提示的键在前，于是处理器设过的线上键获胜，与 `apply_cache_hint` 的优先级一致」。
- `core_shape` 的判断是三选一或：非现代版本、或 `resultType` 不是字符串、或它在 `CORE_RESULT_TYPES` 里。只有核心形状才走 surface 筛选，注释说明「逐版本 surface 描述不了扩展形状」；而「旧连接筛选一切——声明的形状是 2026 年代词汇，旧线缆送不出去」。
- surface 校验失败的处理：「服务端 bug，不是客户端过错。细节留在服务端日志里：pydantic 的消息会回显结果正文」。于是抛 `MCPError(INTERNAL_ERROR, "Handler returned an invalid result")`，不带 pydantic 文本。
- 最后一处补丁很关键：「规范 2026-07-28 要求 `Result.resultType`，服务器 MUST 包含它（『缺席即完成』的桥只给老服务器的客户端用）。筛选保证核心方法有它；这一步覆盖其余全部：自定义方法、扩展方法、空结果」。

**`_stamp_server_info` 是「不改处理器对象」的示范。**

```
if version not in MODERN_PROTOCOL_VERSIONS: return result
raw_meta = result.get("_meta")
if raw_meta is None:
    result["_meta"] = {SERVER_INFO_META_KEY: self.server.server_info_stamp}
elif isinstance(raw_meta, dict):
    meta = cast("dict[str, Any]", raw_meta)
    if meta.get(SERVER_INFO_META_KEY) is None:
        result["_meta"] = {**meta, SERVER_INFO_META_KEY: self.server.server_info_stamp}
return result
```

docstring 把每一条都解释了：处理器自己写的值获胜；**显式 `null` 视为缺席并被覆盖**（对齐请求侧 `clientInfo` 的姿态：`null` 不是合法的 `Implementation`，存在即是有值）；非映射的 `_meta` 归处理器所有（不动）；握手年代永不盖章。而 `result` 是流水线所有的（`_dump_result` 拷贝字典、surface 筛选会重新 dump），但 `_meta` **可能仍是处理器的对象**，所以「**替换它而不是往里写**」——用 `{**meta, ...}` 造新字典。最后一句：「`server_info_stamp` 每次访问都是新字典，所以响应永不别名到服务端状态」。

**`_dump_result` 与 `_extract_meta` 是两个防御性小函数。**

```
def _dump_result(result: Any) -> dict[str, Any]:
    if isinstance(result, BaseModel):
        return result.model_dump(by_alias=True, mode="json", exclude_none=True)
    elif isinstance(result, dict):
        return dict(result)          # 拷贝，避免动到处理器的字典
    return {"result": result}
```

第三支是最有意思的：一个既不是模型也不是字典的结果被包成 `{"result": ...}`——这是最后的手段，而非正常路径。

`_extract_meta` 则「从原始参数里提升 `_meta`；缺席或畸形时返回 `None`，好让上下文构造独立于参数合法性」。这一点很重要：上下文是先造的（因为日志门禁要用 `meta`），而参数校验在 `_inner` 里才做——两件事被解耦。

**握手协商的两段。**

```
def _negotiate_initialize(params) -> tuple[InitializeRequestParams, str]:
    init = InitializeRequestParams.model_validate(params or {}, by_name=False)
    requested = init.protocol_version
    negotiated = requested if requested in HANDSHAKE_PROTOCOL_VERSIONS else LATEST_HANDSHAKE_VERSION
    return init, negotiated
```

协商规则只有一句：客户端请求的版本在握手版本集里就照办，否则回落到最新的握手版。没有区间协商、没有逐特性降级。

`_handle_initialize` 构造应答时用了 `self.init_options if self.init_options is not None else self.server.create_initialization_options()`——即「不传就现算」，与字段 docstring 呼应。

**`aclose_shielded` 是拆除侧的安全阀。**

```
with anyio.move_on_after(_EXIT_STACK_CLOSE_TIMEOUT, shield=True) as scope:
    try: await connection.exit_stack.aclose()
    except Exception: logger.exception("connection exit_stack cleanup raised")
if scope.cancelled_caught:
    logger.warning("connection exit_stack cleanup exceeded %s seconds; abandoning remaining callbacks", ...)
```

docstring 把三个设计点列清：「由 driver 的 `finally` 调用：屏蔽让每连接清理回调即使在 driver 自身被取消时也能跑；`_EXIT_STACK_CLOSE_TIMEOUT`（5 秒）界住一个卡住的回调、不让它卡死关闭；抛错回调记日志并吞掉，于是永不掩盖 driver 自己的异常」。三条合起来是一条原则：**拆除阶段的失败绝不能盖掉主路径的失败**。

**四个 driver 的分工就是「谁造连接、谁拆连接」。**

`serve_connection` 收一个已有的分发器与连接，建内核、跑循环、`finally` 里 `aclose_shielded`。docstring 强调「入口构造 `Connection`；这一步只消费它」。

`serve_loop` 造分发器（`inline_methods=frozenset({"initialize"})`，注释解释：「inline 处理 `initialize`，好让把它与下一个请求流水线化的客户端（规范是 SHOULD NOT 而非 MUST NOT）看到已初始化状态，而不是撞上初始化闸门」）与 `Connection.for_loop`，然后交给 `serve_connection`。

`serve_dual_era_loop` 先窥探第一个请求定年代，再走 `_serve_modern_stream` 或 `_serve_legacy_stream`；两侧的 `on_request` 各自加了自己的年代校验：旧路径对带现代信封（且非 `initialize`）的请求抛 `INVALID_REQUEST`；现代路径对 `initialize` 抛 `UNSUPPORTED_PROTOCOL_VERSION` 并在 `data` 里给出 `_initialize_after_modern_data(params)`（「客户端提议的版本可解析时给类型化的 -32022 载荷，否则只给支持列表——重点是说明我们服务什么」）。

现代路径还有两个组合件：`NotifyOnlyOutbound(dispatcher)` 作连接级通道（通知能走、请求拒绝），以及 `_NoServerRequestsDispatchContext(dctx)` 包住每条请求的分发上下文。后者的 `transport` 属性做了一件细致的事：

```
transport = self._inner.transport
return replace(transport, can_send_request=False) if transport.can_send_request else transport
```

注释：「掩掉逐消息标志，好让传输元数据与这个包装器的拒绝一致：现代 HTTP 入口构造上下文时 `can_send_request=False`，而循环的默认构造器说 `True`」。也就是说**元数据与行为必须说同一件事**，即使某个入口忘了设对，这一层也会补正。

`serve_one` 是单次交换：建内核、跑一次 `on_request`、`finally` 拆 `exit_stack`。docstring 明确「抛处理器链抛的一切（`MCPError` / `ValidationError` / 未映射）；异常到线上的映射归调用方」。

`modern_error_data` 在文件里占了一个位置，因为它定义了「现代请求在任何传输上失败都长一样」：`MCPError` 与 `ValidationError` 走共享阶梯，其他异常记服务端日志后统一 `INTERNAL_ERROR`（「于是处理器内部永不到达线上」）。

## 关键取舍

**源码事实：** `ServerRunner` 是纯内核：`@dataclass` 四个字段，不建连接、不开循环、不做生命周期；驱动与拆除全在四个自由函数 driver 里。

**源码事实：** `_inner` 从 `ctx` 重读 `method` / `params`，使中间件能用 `call_next(replace(ctx, ...))` 改写这次请求的身份（包括走哪个处理器）。

**源码事实：** `_serialize` 在中间件链**之内**执行（让 OTel span 观测到失败的返回形状），而 `_dump_result` 在链外；短路的中间件被信任返回自己的完整信封，流水线事后不修补。

**源码事实：** 中间件链用 `reversed()` 组装，列表第一个在最外层，与「outermost-first」约定一致；`_apply_middleware` 存在的唯一目的是让每层都成为「收 `ctx` 的 `CallNext`」。

**源码事实：** 握手的提交在链成功之后，且依赖「`initialize` 是 inline 方法、读循环被停住」来保证无竞态；同时有一处 `TODO` 承认「重读线上 `params`」与「中间件可能改写 `ctx`」不同步。

**源码事实：** 判定顺序是：规范方法 surface 校验 → `initialize` 内建路径 → 处理器查找 → 初始化闸门 → 参数校验。无处理器一律 `METHOD_NOT_FOUND`，**与初始化状态无关**，因为客户端在握手前正是靠这个码探测。

**源码事实：** 处理器返回 `ErrorData` 会被转成 `MCPError` 抛出，好让中间件观测到失败。

**源码事实：** 通知路径的所有异常都降级为日志（不存在的通知、畸形参数、处理器崩溃），因为通知没有应答通道。

**源码事实：** `_stamp_server_info` 用 `{**meta, ...}` **替换** `_meta` 而不是往里写，因为 `_meta` 可能仍是处理器的对象；显式 `null` 视为缺席并被覆盖。

**源码事实：** `aclose_shielded` 用「屏蔽 + 5 秒界 + 吞异常」三条，确保拆除失败不掩盖主路径失败。

**架构推论：** 「纯内核 + 多个 driver」让同一套处理器逻辑服务于四种入口（已有分发器、握手流、双年代流、单次交换），而内核不需要知道自己被谁驱动。代价是连接与 `dctx` 的归属在 docstring 之间必须反复澄清（「入口构造，这一步只消费」）。

**架构推论：** 允许中间件改写 `ctx` 并短路，换来极强的可扩展性（缓存、鉴权、限流、请求重写都能在同一层实现），代价是「重读 `ctx` 与重读 `params`」这类不一致必须逐个手工对齐，而这类问题只能在 `TODO` 里挂着。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/server/runner.py` | `ServerRunner` | 每连接处理器内核：上下文构造、中间件链、结果整形 |
| `src/mcp/server/runner.py` | `ServerRunner._on_request` | 请求主链：surface 校验 → initialize → 查找 → 初始化闸门 → 校验 → 处理器 |
| `src/mcp/server/runner.py` | `ServerRunner._serialize` | 出站信封：缓存提示、surface 筛选、`resultType` 补齐、身份盖章 |
| `src/mcp/server/runner.py` | `_apply_middleware` | 把中间件适配成收 `ctx` 的 `CallNext` |
| `src/mcp/server/runner.py` | `_compose_server_middleware` | 逆序组装，首个中间件最外层 |
| `src/mcp/server/runner.py` | `aclose_shielded` | 屏蔽 + 界时 + 吞异常的连接拆除 |
| `src/mcp/server/runner.py` | `serve_one` / `serve_loop` / `serve_connection` | 三种 driver：单次交换、握手流、已有分发器 |
| `src/mcp/server/context.py` | `ServerRequestContext` | 交给处理器的逐请求上下文（全字段可空者带默认） |
| `src/mcp/server/context.py` | `ServerMiddleware` | `(ctx, call_next)` 单方法 Protocol 与它的变型困境 |

## 参考

- [mcp-python-sdk 源码：server/runner.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/runner.py)
- [mcp-python-sdk 源码：server/context.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/context.py)
- [MCP 规范：生命周期与初始化](https://modelcontextprotocol.io/specification/2025-06-18/basic/lifecycle)
- [MCP 规范：结果类型与 resultType](https://modelcontextprotocol.io/specification/)

## 与主核 / AI 的关系

「纯内核 + driver」这套分法在主核里对应得很直接：内核是执行器（把一次请求变成结果），driver 是连接管理与协议适配（谁建会话、谁拆会话、什么时候读下一个包）。主核之所以普遍这样分，是因为同一套执行逻辑要服务多种入口（本地 socket、共享内存、复制流）；这里同样如此——`serve_one`、`serve_loop`、`serve_dual_era_loop` 是三个入口，内核一份。

对 AI 工程的实际意义有三条。第一，**判定顺序本身就是协议语义**。「无处理器一律 `METHOD_NOT_FOUND`，与初始化状态无关」这条如果写反（先判初始化再判存在性），握手前探测服务器的客户端会收到 `INVALID_PARAMS` 而不是 `METHOD_NOT_FOUND`，于是它们无法区分「服务器不提供这个能力」与「我还没握手」——一个码的顺序错，换来一类客户端的不兼容。

第二，**拆除不能掩盖主因**。`aclose_shielded` 的「屏蔽 + 界时 + 吞异常」三条是清理代码的通用纪律：清理过程中抛出的异常若逃出去，会把「为什么到这里」的真正原因盖掉，留下一条误导性的堆栈。给清理一个时间上界同样是必需的，否则一个卡住的清理回调会让整个服务端关不掉。

第三，**中间件链的方向与短路语义必须一次性定清**。`reversed()` 决定了「谁最外层」，而「短路的中间件自己负责信封」决定了短路者的责任边界。这两件事若只靠实现约定而不写明，加第二个中间件的人一定会搞错顺序，或以为流水线会替它补齐信封。
