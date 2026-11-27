---
project: mcp-python-sdk
feature: jsonrpc-dispatcher
title: JSON-RPC 分发器与在途请求表
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/shared/jsonrpc_dispatcher.py
    symbols: [JSONRPCDispatcher, _Pending, _InFlight]
  - path: src/mcp/shared/jsonrpc_dispatcher.py
    symbols: [handler_exception_to_error_data, _plan_outbound]
---

# JSON-RPC 分发器与在途请求表

## 一句话职责

`JSONRPCDispatcher` 把一对 `SessionMessage` 流变成一份真正的 `Dispatcher`：它用两张表（`_pending` 出站等待、`_in_flight` 入站执行）维护双向关联，用一个接收循环 + 一个 anyio task group 做并发与隔离，并且是**唯一的异常到线上错误边界**。

## 机制是什么

先看两张表的分工，这是理解整个类的钥匙：

- **`_pending: dict[RequestId, _Pending]`** — 我发出去、还在等回复的请求。每条 `_Pending` 带一对内存对象流（`send` / `receive`，`buffer=1`）和一个可选的 `on_progress`。回复到达时往 `send` 里塞结果，等待方在 `receive` 上醒。
- **`_in_flight: dict[RequestId, _InFlight]`** — 别人发进来、我正在处理的请求。每条 `_InFlight` 带一个 `anyio.CancelScope` 和一个分发上下文。对端发来 `notifications/cancelled` 时，就是靠这张表找到要取消的作用域。

两表的键都过 `coerce_request_id` 归一（`"7"` 与 `7` 视为同一个 id），所以一个用字符串回显 id 的对端仍然能对上关联——这正是 `coerce_request_id` 的 docstring 说的「关联域，不是线缆域」。

`_CLOSED_OUTCOME` 是第三个隐性参与者：一个模块级的 `ErrorData(code=CONNECTION_CLOSED, message="Connection closed")` 常量，注释强调「按身份比较，因为对端也能发出同样的码」。凡是「连接关了所以别等了」的场合，塞进等待者的都是**这一个对象**，于是接收方用 `is` 就能区分「我自己关的」与「对端真的报了这个码」。

## 怎么实现的

**出站：`send_raw_request` 的九个阶段。**

1. 关闭检查：`if self._closed: raise MCPError(code=CONNECTION_CLOSED, ...)`——注释说这是为了给关闭后的发送与在途等待者**同一个契约**。
2. 未启动检查：`if not self._running: raise RuntimeError(...)`。这个 `RuntimeError` 的文案直接写明了原因（`called before run()`）。
3. 分配 id：调用方给了 `opts["request_id"]` 就用它（撞在途的 `_pending` 键则……见下），否则 `_allocate_id()` 并**循环跳过已在 `_pending` 的号**。
4. 处理 `_meta`：`out_meta = dict(out_params.get("_meta") or {})`；若有 `on_progress`，就把 `out_meta["progressToken"] = request_id`——注释写明「请求 id 兼作进度令牌，于是 `_pending[token]` 能直接找到 `on_progress`」。这一步省掉了一张独立的令牌表。
5. 建 `_pending` 项：`anyio.create_memory_object_stream[...](1)`，注释解释 `buffer=1` 的必要性：「关闭信号可能先于等待者挂到 `receive()` 上；之后出现 `WouldBlock` 只说明等待者已经拿到它那一个结果了」。
6. `_plan_outbound(related_request_id, opts)` 定出出站元数据与「放弃时要不要礼貌取消」。
7. 开 OTel span（span 名带 `mcp.method.name` 与 `jsonrpc.request.id` 属性），注入 W3C 追踪上下文到 `_meta`（注释标 SEP-414）。span 名对 `tools/call` 之类会拼上目标名：`f"MCP send {method}{f' {target}' if isinstance(target, str) else ''}"`，目标从 `out_params.get("name")` 取。
8. **先 `checkpoint_if_cancelled()` 再置 `request_write_started = True` 再写**。顺序是有讲究的：注释说「在请求可证明尚未开始时把既有取消暴露出来；过了这一点，被中断的写也算已发出」。理由是规范 MUST：只有**此前已发出过**的请求才能被取消；而一次被取消打断的写其实可能已经把项交给了接收端，所以「已开始写」等同于「已发出」。
9. `with request_clock(opts.get("timeout")) as clock:` 写完之后才 `clock.start()` 并置 `timeout_armed = True`。

`params` 的序列化有一处规范对齐：`params` 为空时**不用** `params={}` 也不用 `params=None`，而是构造不带 `params` 键的 `JSONRPCRequest`。注释：「用 `exclude_unset=True` 时显式 `None` 会序列化成 `"params": null`，JSON-RPC 2.0 不允许」。同样的注释在 `notify()` 里也出现了一次。

**超时与取消的两条分支，各有一处反直觉的细节。**

超时分支先判断 `if not timeout_armed: raise`——注释解释：时钟是在写完之后才启动的，所以此处的 `TimeoutError` 只能是**传输自己的受限 `send()`** 失败，属于传输错误而非 `opts["timeout"]` 到期，应当原样抛出（v1 也是这个行为）。只有真的超时才走「礼貌取消」：

```
if plan.cancel_on_abandon:
    await self._final_write(partial(self._cancel_outbound, request_id, f"timed out after {opts.get('timeout')}s", _related_request_id),
                            shield=False, timeout=_ABANDON_WRITE_TIMEOUT, ...)
raise MCPError(code=REQUEST_TIMEOUT, ...)
```

注意 `shield=False`：超时路径上的取消写**不屏蔽**，好让外层的调用方取消仍然能打断它。

调用方取消分支则相反，`shield=True`，并且多一个前置条件 `request_write_started`：

```
except anyio.get_cancelled_exc_class():
    if plan.cancel_on_abandon and request_write_started:
        await self._final_write(..., shield=True, timeout=_ABANDON_WRITE_TIMEOUT, ...)
    raise
```

注释点明：在取消路径上裸 `await` 会立刻重抛，所以必须用屏蔽过的辅助函数，才能让礼貌取消真的发出去。

`finally` 里三件事一件不少：`self._pending.pop(pending_key, None)`（注释：「每条路径都移除等待者，好让迟到的响应被丢弃而不是泄漏」）、`send.close()`、`receive.close()`。

**`_plan_outbound` 是一个 25 行的优先级裁定。**

它的返回值是 `_OutboundPlan(metadata, cancel_on_abandon)`。裁定顺序：

1. `cancel_on_abandon` 先取 `opts.get("cancel_on_abandon", True)`；
2. 若有 `related_request_id`（即这条出站请求是从某个入站请求的处理器里发的），**它优先于续传提示**，把 `token` / `on_token` 丢掉并记一条 debug 日志（「`related_request_id %r` 在元数据上优先」），返回 `ServerMessageMetadata(related_request_id=...)`；
3. 否则若有续传提示，返回 `ClientMessageMetadata(resumption_token=..., on_resumption_token_update=..., headers=...)`，且 `cancel_on_abandon=False`；
4. 否则只有 `headers` 就返回 `ClientMessageMetadata(headers=...)`；
5. 全无则 `_OutboundPlan(None, cancel_on_abandon)`。

第 3 步那个 `cancel_on_abandon=False` 配着一句注释：「只有真的到达传输的续传提示才抑制礼貌取消——一个既不续传也不取消的请求会泄漏对端的工作」。也就是说：有续传能力时，对端的工作由续传机制兜住，不该再发取消；否则必须发。

**接收循环 `run()` 的嵌套结构。**

```
async with self._write_stream:           # 写流最后关
    async with anyio.create_task_group() as tg:
        self._tg = tg; self._running = True; task_status.started()
        try:
            async with self._read_stream:
                try:
                    async for item in self._read_stream:
                        sender_ctx = getattr(self._read_stream, "last_context", None)
                        await self._dispatch(item, on_request, on_notify, sender_ctx)
                except anyio.ClosedResourceError:
                    logger.debug("read stream closed by transport; treating as EOF")
            self._running = False; self._closed = True; self._fan_out_closed()
        finally:
            tg.cancel_scope.cancel()
```

三处设计值得单独讲：

- **LIFO 退出顺序**：注释写「写流最后关，于是拆除期间的写仍然落得下去」。任务组先 join，再关写流。
- **`ClosedResourceError` 视为 EOF**：注释点名「无状态 streamable HTTP 的拆除」，即接收端被传输侧关掉与读到流结束等价。
- **`finally: tg.cancel_scope.cancel()`**：注释解释「否则任务组 join 会等那些调用方早已消失的处理器」。这句是整套并发模型的关键——接收循环一结束，所有在跑的处理器都必须被取消，否则 `run()` 永远返回不了。

最外层 `finally` 还做了一次幂等的收尾（`_running=False` / `_closed=True` / `_tg=None` / `_fan_out_closed()` / `await resync_tracer()`），注释说这是「覆盖 cancel/崩溃等跳过内联 fan-out 的路径」。

**`_dispatch` 的四路分派只用一次 `match`。**

```
match msg:
    case JSONRPCRequest():        await self._dispatch_request(msg, metadata, on_request, sender_ctx)
    case JSONRPCNotification():   self._dispatch_notification(msg, metadata, on_notify, sender_ctx)
    case JSONRPCResponse():       self._resolve_pending(msg.id, msg.result)
    case JSONRPCError():          self._resolve_pending(msg.id, msg.error)
```

`match` 覆盖了 `JSONRPCMessage` 联合的所有成员，所以最后一个 case 上标了 `# pragma: no branch`，注释写「对 `JSONRPCMessage` 穷尽，所以不匹配的分支不可达」。这是把「联合新增成员会编译不过/静态检查报错」变成真正的保护。同一函数开头还处理了 `item` 是 `Exception` 的情形（传输把异常当消息交付）：无观察者则 debug 日志后丢弃，有观察者则 `await` 且**吞掉观察者自身的异常**。

`_dispatch` 的 docstring 有一条极简但重要的纪律：「只有 `inline_methods` 请求与 `on_stream_exception` 观察者被 await；任何其它 await 都会队头阻塞接收循环」。

**`_spawn` 是上下文变量传播的接线点。**

```
if sender_ctx is not None:
    sender_ctx.run(self._tg.start_soon, run)
else:
    self._tg.start_soon(run)
```

docstring 解释：ASGI 中间件（认证、OTel）是在**写这条消息的那个 task** 上设的 contextvars，`Context.run` 让派生出来的处理器继承那份上下文。这让「HTTP 请求的身份信息」能穿过内存流抵达处理器，而不需要显式传参。

`_dispatch_request` 里还实现了一个 `inline_methods` 的巧思：`initialize` 必需按序，但又要用 `sender_ctx` 运行，于是先 `_spawn` 再 `await done.wait()`：

```
done = anyio.Event()
async def _run_inline():
    try: await self._handle_request(req, dctx, scope, on_request)
    finally: done.set()
self._spawn(_run_inline, sender_ctx=sender_ctx)
await done.wait()
```

注释：「Spawn 是为了让 `sender_ctx` 生效，但把接收循环停在这里直到处理器返回——这才是 inline 的顺序保证」。代价写在构造函数的 docstring 里：「inline 处理器若 await 对端会造成死锁（接收循环被停住了）」。

**`_handle_request` 是那个「唯一的异常边界」。**

它的结构逐层剥开都能对应一条协议要求：

```
with scope:                                  # 可被对端取消的作用域
    try:
        result = await on_request(dctx, req.method, req.params)
    finally:
        dctx.close()
        if (entry := self._in_flight.get(key)) is not None and entry.dctx is dctx:
            del self._in_flight[key]
    if not dctx.cancel_requested.is_set():
        answer_write_started = True
        await self._write_result(req.id, result)
```

`finally` 里那句身份守卫的注释是：「不要驱逐重复 id 的更新条目」。因为有「重复 id 直接盲覆盖」（带 `TODO` 承认是 v1/TS 对齐）这条现状，`_in_flight[key]` 可能已属于后来的另一条请求。

`answer_write_started` 这个标志的用途在取消分支：

```
except anyio.get_cancelled_exc_class():
    if not answer_write_started and not dctx.cancel_requested.is_set():
        await self._final_write(partial(self._write_error, req.id, ErrorData(code=CONNECTION_CLOSED, ...)),
                                shield=True, timeout=_SHUTDOWN_WRITE_TIMEOUT, ...)
    raise
```

注释把判断依据写全了：「连接关闭时要应答请求，免得对端干等——除非应答写已经开始（它可能已到达传输，宁可答案是零份而不是两份），或者对端已经取消它并停止等待了」。`_SHUTDOWN_WRITE_TIMEOUT = 1` 比放弃路径的 `5` 更紧，注释说「防止卡死的传输把会话关闭也拖住」。

普通异常分支则回落 `handler_exception_to_error_data`；返回 `None`（无法映射）时打 `logger.exception` 并**钉 `code=0`**，旁边挂 `TODO(L58)` 承认「JSON-RPC 说要 `INTERNAL_ERROR`，`code=0` 是为了对齐既有服务端、按兼容性差异条目待改」。

最后一段是「一条被取消的请求在哪里收场」：

```
if not answer_write_started:
    await self._settle_unanswered(dctx)
if handler_failure is not None:
    raise handler_failure
```

注释：「处理器跑完（无论哪种模式）且没写出任何东西——对端打断式取消在 `scope` 退出时被吸收，也落到这里」。`_settle_unanswered` 调传输的 `on_request_unanswered` 回调（如果元数据里给了），并把这个回调自身的异常就地contain住。

**`handler_exception_to_error_data` 是两级阶梯。**

```
if isinstance(exc, MCPError):            return exc.error
if isinstance(exc, ValidationError):     return ErrorData(code=INVALID_PARAMS, message="Invalid request parameters", data="")
return None
```

docstring 把「为什么返回 `None` 而不是自己兜底」写清了：两个调用方需要不同的兜底——`JSONRPCDispatcher` 现在钉 `code=0` 做 v1 兼容，现代 HTTP 入口用 `INTERNAL_ERROR`。把它做成两级阶梯、兜底留给调用方，是「共享能共享的，不共享的显式分开」。

`ValidationError` 那档也写了理由：`data=""` 是「不把 pydantic 的文本放到线上」。

**关闭时的唤醒：`_fan_out_closed`。**

```
for pending in self._pending.values():
    try: pending.send.send_nowait(_CLOSED_OUTCOME)
    except (anyio.WouldBlock, anyio.BrokenResourceError, anyio.ClosedResourceError): pass
self._pending.clear()
```

docstring 两条：「同步——调用方可能正处于被取消的作用域里」「幂等」。`send_nowait` 而非 `await send` 是必须的：EOF 之后接收循环已经结束，若这里还要等，阻塞的等待者就永远等不到。

`_resolve_pending` 是反向操作，也全用 `send_nowait`，并且对「未知/迟到的 id」只记 debug 日志——这是「迟到的响应合法但无意义」的正确处置。

## 关键取舍

**源码事实：** 出站等待与入站执行分成 `_pending` / `_in_flight` 两张表，键统一过 `coerce_request_id`，因此字符串回显的 id 也能关联上。

**源码事实：** 「连接关了」用模块级常量 `_CLOSED_OUTCOME` 按**身份**传递，使接收方能区分「本端关闭」与「对端恰好发了同码错误」。

**源码事实：** 进度令牌复用请求 id（写进 `_meta.progressToken`），省掉独立令牌表。

**源码事实：** 写的时机被精确建模：先 `checkpoint_if_cancelled()`，再置 `request_write_started`，再写。理由是被打断的写可能已交付，规范只允许取消已发过的请求。

**源码事实：** 超时分支里「未 arm 的 `TimeoutError`」原样重抛（那是传输的 `send` 失败），只有真超时才发礼貌取消，且 `shield=False`；调用方取消路径则 `shield=True` 并要求 `request_write_started`。

**源码事实：** 有续传提示时 `cancel_on_abandon=False`——续传机制兜住对端的工作，不再重复发取消。

**源码事实：** `run()` 的退出顺序是「先 join 任务组、再关写流」，且 `finally` 里无条件 `tg.cancel_scope.cancel()`。

**源码事实：** `_handle_request` 用 `answer_write_started` 决定「关闭时要不要补一条错误应答」，理由是「宁可零份答案，也不要两份」。

**源码事实：** 无法映射的处理器异常钉 `code=0`（带 `TODO` 承认偏离规范），而共享的 `handler_exception_to_error_data` 返回 `None` 把兜底留给调用方。

**架构推论：** 整个类可以看成「一次请求从写到收的完整生命周期 + 每个可能中断点的处置」。它的复杂度不在算法，而在于每个中断点（未启动、传输坏、超时、调用方取消、对端取消、EOF、关闭）都有一套互不相同的正确行为，而这些行为大半只能从注释里读到。

**架构推论：** `inline_methods` 用「spawn 后等 event」在并发模型里造出一个顺序点，是「既要在同一上下文里跑、又要在接收循环上阻塞」的唯一解；代价是 inline 处理器里 await 对端会死锁，这一点只能靠文档约束。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/shared/jsonrpc_dispatcher.py` | `JSONRPCDispatcher` | 生产实现：双表关联、接收循环、并发隔离、异常边界 |
| `src/mcp/shared/jsonrpc_dispatcher.py` | `JSONRPCDispatcher.send_raw_request` | 九阶段出站：id 分配、进度令牌复用、写时机、超时与取消分支 |
| `src/mcp/shared/jsonrpc_dispatcher.py` | `JSONRPCDispatcher.run` | 循环与外层拆除，LIFO 关流 + 无条件取消任务组 |
| `src/mcp/shared/jsonrpc_dispatcher.py` | `JSONRPCDispatcher._handle_request` | 唯一的异常到线上错误边界，含「零份优于两份」判定 |
| `src/mcp/shared/jsonrpc_dispatcher.py` | `_Pending` | 出站等待者：结果流对 + 可选进度回调 |
| `src/mcp/shared/jsonrpc_dispatcher.py` | `_InFlight` | 入站执行者：可取消作用域 + 分发上下文 |
| `src/mcp/shared/jsonrpc_dispatcher.py` | `_plan_outbound` | 出站元数据与「放弃时是否礼貌取消」的优先级裁定 |
| `src/mcp/shared/jsonrpc_dispatcher.py` | `handler_exception_to_error_data` | 两级异常阶梯，兜底交给调用方 |
| `src/mcp/shared/jsonrpc_dispatcher.py` | `_fan_out_closed` | 同步、幂等地把「连接已关」推给所有等待者 |

## 参考

- [mcp-python-sdk 源码：shared/jsonrpc_dispatcher.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/shared/jsonrpc_dispatcher.py)
- [JSON-RPC 2.0 规范](https://www.jsonrpc.org/specification)
- [MCP 规范：取消与进度](https://modelcontextprotocol.io/specification/2025-06-18/basic/utilities/cancellation)
- [anyio 文档：内存对象流与取消作用域](https://anyio.readthedocs.io/en/stable/streams.html)

## 与主核 / AI 的关系

`_pending` / `_in_flight` 这一对表，与主核里「锁表 + 活跃事务表」是同一种结构：一张是「我在等别人」，一张是「别人在等我」，两张表的键域必须一致，否则关联错乱。主核处理这张表时最怕的是「条目泄漏」——一个事务结束时忘记摘除，后续请求就会撞上一个僵尸条目。这里用 `finally` 三重清理（pop 等待者、关两条流、条件式摘除在途项）并在 pop 上加了身份守卫，正是同一类防御。

对 AI 工程的实际意义集中在两点。第一是**「写的时机决定了取消是否合法」**这条建模：规范说「只能取消已发出的请求」，而「已发出」在多路复用里没有明确的判定点，这里把「开始写」当作判定点并写出理由（被中断的写可能已交付），这种把模糊规范落成可执行判定的做法，比任何抽象都更有价值。第二是**「宁可零份答案，也不要两份」**这条判定——在 shutdown 时补错误应答之前，先检查应答写是否已经开始。多路复用协议里「重复应答」是致命错误（对端会当成两个请求的结果），而「缺少应答」通常只是对端超时。用一句注释把这个偏好写下来，等于给后来改这段代码的人定了方向。
