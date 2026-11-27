---
project: mcp-python-sdk
feature: progress-and-cancel
title: 进度通知与取消
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/shared/jsonrpc_dispatcher.py
    symbols: [progress_token_from_params, cancelled_request_id_from_params, _shielded_progress]
---

# 进度通知与取消

## 一句话职责

两种通知——`notifications/progress` 与 `notifications/cancelled`——都是**无 id 的 JSON-RPC 通知**，所以它们的关联责任落在分发器上：进度靠 `progressToken` 找回等待者的回调，取消靠 `requestId` 找回在途请求的取消作用域；三者中两个是纯参数解析器，一个是把用户回调与分发器生命周期隔开的屏蔽包装。

## 机制是什么

先明确一件事：**这两个通知不携带请求 id**（通知本就没有 id 字段），它们各自用一个 `params` 里的字段做关联：

- 进度用 `params.progressToken`——由**发起方自己**在请求的 `_meta` 里设置；
- 取消用 `params.requestId`——指向**要被取消的那个请求**的 id。

于是关联方向恰好相反：进度令牌映射回「我发出去的那个请求」（`_pending`），请求 id 映射回「别人发进来、我正在跑的那个请求」（`_in_flight`）。同一个分发器上两张表都因为这两个通知被读到。

三个符号分工：

- `progress_token_from_params(params)` —— 从通知参数里取进度令牌，用结构化模式匹配，并且**显式拒 bool**。
- `cancelled_request_id_from_params(params)` —— 从通知参数里取要取消的请求 id，复用 `as_request_id` 的形状规则（同样拒 bool）。
- `_shielded_progress(fn)` —— 把用户提供的进度回调包一层异常吸收，使一个抛异常的回调**不能取消分发器的任务组**。

进度令牌的生成端在 `send_raw_request` 里：若调用方给了 `on_progress`，分发器就把**请求自己的 id** 写进 `_meta.progressToken`（注释：「请求 id 兼作进度令牌，于是 `_pending[token]` 能直接找到 `on_progress`」）。这省掉了一张独立的「令牌 → 回调」表，代价是令牌的取值域与请求 id 的取值域被绑在一起。

## 怎么实现的

**`progress_token_from_params` 用模式匹配加守卫。**

```
match params:
    case {"_meta": {"progressToken": str() | int() as token}} if not isinstance(token, bool):
        return token
    case _:
        return None
```

三重含义叠在一行里：令牌必须嵌在 `_meta` 下（不是顶层 `params.progressToken`）；只接受 `str` 或 `int`（其余类型一律视为没有）；且 `int` 分支要再排掉 `bool`。docstring 直接给出理由：「bool 是 int 的子类，`True` 会等于 1」。

这个守卫不是孤例——同一个文件里 `as_request_id` 也拒 bool（注释：「`True` 会别名到请求 id 1」），进度通知的处理处也准备了两道 `not isinstance(..., bool)` 的守卫（令牌与进度值各一）。

**取消通知的解析是两行。**

```
def cancelled_request_id_from_params(params):
    return as_request_id((params or {}).get("requestId"))
```

`as_request_id` 本身是：

```
if isinstance(value, str | int) and not isinstance(value, bool):
    return value
return None
```

所以「缺字段」「类型不对」「是 bool」三种情况统一成一个 `None`，调用方只需要判断 `is not None`。

**拦截与分发在同一处完成，且顺序是刻意的。**

`_dispatch_notification` 的 docstring 写清了它做两件互相独立的事：

> `notifications/cancelled` 与 `notifications/progress` 在这里被拦截（它们要对本层拥有的 `_in_flight`/`_pending` 表做关联），之后**仍然**转发给 `on_notify`。调用方的 `on_notify_intercept` 随后按接收顺序运行；只有未被消费的通知才到达 spawn 出来的 `on_notify`。

注意「仍然转发」这四个字：被分发器拦截**不等于**被消费。一个用户想在 `notifications/cancelled` 上做审计日志，仍然收得到——这正是 `_in_flight` 那层的拦截与 `on_notify_intercept` 那层的消费是两件事。

**取消的两个动作，由 `peer_cancel_mode` 选择。**

```
if msg.method == "notifications/cancelled":
    rid = cancelled_request_id_from_params(msg.params)
    if rid is not None and (in_flight := self._in_flight.get(coerce_request_id(rid))) is not None:
        in_flight.dctx.cancel_requested.set()
        if self._peer_cancel_mode == "interrupt":
            in_flight.scope.cancel()
```

三件事：

1. `coerce_request_id(rid)` 归一后再查表——注释在 `_in_flight` 赋值处专门提过「键做了归一，好让字符串化的 `notifications/cancelled` id 仍能对上」。
2. **无条件**置 `dctx.cancel_requested` 事件。这是给处理器看的信号，任何模式下都设。
3. 只有在 `"interrupt"` 模式下才真的 `scope.cancel()`。

`PeerCancelMode` 的 docstring 把两种模式讲全了：

> `"interrupt"`（默认）取消处理器的作用域；`"signal"` 只置 `ctx.cancel_requested` 让处理器跑到自然结束。

而两者的共同点是关键的一句：**无论哪种模式，被取消的请求永远不会被应答**——处理器最终的结果或错误被丢弃，不写出。

这条「永不应答」在 `_handle_request` 里的落地是：

```
if not dctx.cancel_requested.is_set():
    answer_write_started = True
    await self._write_result(req.id, result)
```

即使处理器在 `"signal"` 模式下跑完了，只要取消事件已置位，结果就不写。而结尾的 `if not answer_write_started: await self._settle_unanswered(dctx)` 负责告诉传输「这次请求无应答收场」——那是 2025-era streamable HTTP 必须的收尾，因为它的线上需要一个明确的结束。

**被取消时为什么会「静默」而不报错。**

`interrupt` 模式下 `scope.cancel()` 会让处理器内部的裸 `await` 抛 `CancelledError`。这段异常在 `_handle_request` 里被两层吸收：`with scope:` 的上下文管理器在 `__exit__` 时吞掉本作用域自己发出的取消；外层那个 `except anyio.get_cancelled_exc_class()` 分支则只在「这是整个 run 的关闭」时才补错误应答，而它带有 `not dctx.cancel_requested.is_set()` 的前置条件，所以对端取消不会触发补应答。

这就是「取消是终局且静默」的实现：没有日志、没有错误响应，只有 `answer_write_started` 保持 `False`。

**进度通知的处理是五重守卫的模式匹配。**

```
match msg.params:
    case {"progressToken": str() | int() as token, "progress": int() | float() as progress} if (
        not isinstance(token, bool)
        and not isinstance(progress, bool)
        and (pending := self._pending.get(coerce_request_id(token))) is not None
        and pending.on_progress is not None
    ):
        total = msg.params.get("total")
        message = msg.params.get("message")
        self._spawn(
            _shielded_progress(pending.on_progress),
            float(progress),
            float(total) if isinstance(total, int | float) else None,
            message if isinstance(message, str) else None,
            sender_ctx=sender_ctx,
        )
    case _:
        pass
```

`case _: pass` 的存在说明「参数不对就静默丢弃」是有意的：进度是尽力而为的，一个畸形的进度通知不该产生错误或日志噪音。四道守卫中各有一处值得记住：

- 令牌与进度值都要排 bool（两个独立的守卫，说明作者对 `True == 1` 这条陷阱有系统性防范）；
- 查表用 `coerce_request_id(token)`，与 `_pending` 的键域一致；
- `pending.on_progress is not None` 在**模式匹配的守卫里**就查了，于是「没登记回调」的进度通知根本不会进入分支体。

`total` 与 `message` 都是可选的，且各自做了类型窄化：`total` 不是 `int | float` 就传 `None`，`message` 不是 `str` 就传 `None`。`float(total)` 的转换也有意——协议允许整数，回调签名统一收 `float`。

**`_shielded_progress` 只有七行，但它是这一层的安全阀。**

```
def _shielded_progress(fn: ProgressFnT) -> ProgressFnT:
    """Wrap a user progress callback so an exception can't cancel the dispatcher's task group."""
    async def _wrapped(progress, total, message):
        try:
            await fn(progress, total, message)
        except Exception:
            logger.exception("progress callback raised")
    return _wrapped
```

docstring 说的正是要点：用户在回调里抛异常，若这个回调是 `_spawn` 到分发器任务组里的，未捕获异常会让整个任务组取消——一次杂散的异常就会拆掉整条连接。包装后异常只影响这一次进度更新。

同一文件里还有 `_contained_notify`，注释写「与 `_shielded_progress` 同一道边界」，包装的是 `on_notify`。两者是**同一个模式的两次应用**：任何「用户代码被调度到分发器任务组里」的位置都需要它。

**取消的另一个方向：出站礼貌取消。**

前面讲的是「对端取消我们的请求」。反向的「我们放弃自己的出站请求」也走 `notifications/cancelled`，由 `_cancel_outbound` 发出：

```
await self.notify("notifications/cancelled",
                  {"requestId": request_id, "reason": reason},
                  _related_request_id=related_request_id)
```

注释说明为什么要把 `_related_request_id` 透传：「让 streamable HTTP 把取消路由到请求自己的 SSE 流上，而不是那条可能不存在的 GET 流」。`notify` 自身吞掉连接状态错误，所以这里不加守卫。

三个触发点各带不同的 `reason` 文案：超时是 `f"timed out after {opts.get('timeout')}s"`，调用方取消是 `"caller cancelled"`。两者的 `shield` 取值相反（超时 `False`、调用方取消 `True`），这一点在前一页已展开。

**长度与超时预算也被显式定了值。**

```
_ABANDON_WRITE_TIMEOUT: float = 5
_SHUTDOWN_WRITE_TIMEOUT: float = 1
```

docstring 给出的理由不对称：放弃路径（超时/调用方取消）给 5 秒，因为「调用方取消那一支屏蔽了它的写，一个卡死的传输否则会让它不可取消地挂住」；关闭路径只给 1 秒，因为「更紧的界，好让卡死的传输不能拖住会话关闭」。两者都落在 `_final_write` 的 `anyio.move_on_after(timeout, shield=shield)` 里，超时后记一条 warning（`"%s gave up: transport write blocked"`）而不是抛错。

## 关键取舍

**源码事实：** 进度令牌由发起方在 `_meta.progressToken` 里设置，分发器复用**请求自己的 id** 作为令牌，于是不需要独立的「令牌 → 回调」映射表。

**源码事实：** 两个解析器都显式拒 `bool`，且守卫是分层的：`as_request_id` 一处、`progress_token_from_params` 一处、进度通知匹配处再两处（令牌与进度值）。

**源码事实：** 分发器拦截 `notifications/cancelled` 与 `notifications/progress` **之后仍然转发**给 `on_notify`；「拦截」与「消费」是两件事，消费由 `on_notify_intercept` 决定。

**源码事实：** `peer_cancel_mode` 有 `"interrupt"`（默认）与 `"signal"` 两种；前者取消作用域，后者只置事件让处理器自然结束。**两种模式下被取消的请求都不写应答。**

**源码事实：** 取消导致的静默收场靠 `answer_write_started` 保持 `False` 表达，并由 `_settle_unanswered` 调传输的 `on_request_unanswered` 收尾。

**源码事实：** 进度通知的参数校验失败一律走 `case _: pass` 静默丢弃，不打日志、不报错。

**源码事实：** 用户回调（进度与通知处理）都被包装成「异常只影响这一次调用」，因为抛进分发器任务组会取消整条连接。

**源码事实：** 放弃路径与关闭路径的写超时不同（5 秒 / 1 秒），且超时后只记 warning 不抛错。

**架构推论：** 把「取消」实现成「取消作用域 + 一个事件」，让同一条通知能支持两种语义（硬中断 / 软信号），而「永不应答」这条协议要求由 `answer_write_started` 一个布尔统一兜住。这比给两种模式各写一套逻辑要省得多，也让 `signal` 模式天然复用 `interrupt` 路径的收尾。

**架构推论：** 进度是「尽力而为」的通道——没有回调、参数畸形、回调抛异常，全都被静默或记日志地吸收。这符合它的本质：进度通知是给人看的装饰，任何情况下都不该影响请求本身的成败。这条定位使得进度的所有路径都不需要错误处理分支，也让 `_shielded_progress` 这种七行包装成为充分的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/shared/jsonrpc_dispatcher.py` | `progress_token_from_params` | 从 `_meta.progressToken` 取令牌，拒 bool、只收 str/int |
| `src/mcp/shared/jsonrpc_dispatcher.py` | `cancelled_request_id_from_params` | 从 `requestId` 取目标请求 id，复用 `as_request_id` 形状规则 |
| `src/mcp/shared/jsonrpc_dispatcher.py` | `_shielded_progress` | 把用户进度回调的异常关在本调用内，避免取消任务组 |
| `src/mcp/shared/jsonrpc_dispatcher.py` | `_dispatch_notification` | 拦截取消与进度并仍然转发；拦截 ≠ 消费 |
| `src/mcp/shared/jsonrpc_dispatcher.py` | `PeerCancelMode` | `interrupt` / `signal` 两种取消语义，均不应答 |
| `src/mcp/shared/jsonrpc_dispatcher.py` | `_cancel_outbound` | 反向：放弃自己的出站请求时发 `notifications/cancelled` |
| `src/mcp/shared/jsonrpc_dispatcher.py` | `_final_write` | 带界与屏蔽选项的写，超时只记 warning |
| `src/mcp/shared/dispatcher.py` | `ProgressFnT` | 进度回调签名：`(progress, total, message)` |
| `src/mcp/shared/dispatcher.py` | `DispatchContext.progress` | 无令牌时为空操作的进度上报入口 |

## 参考

- [mcp-python-sdk 源码：shared/jsonrpc_dispatcher.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/shared/jsonrpc_dispatcher.py)
- [MCP 规范：进度](https://modelcontextprotocol.io/specification/2025-06-18/basic/utilities/progress)
- [MCP 规范：取消](https://modelcontextprotocol.io/specification/2025-06-18/basic/utilities/cancellation)
- [JSON-RPC 2.0 规范](https://www.jsonrpc.org/specification)

## 与主核 / AI 的关系

「取消 = 取消作用域 + 一个信号事件」这套双轨，与主核里的「强制中断查询」与「设置取消标志让算子自行检查」是同一对比：强中断能立刻停住，但可能留下未清理状态；标志位则让算子在自己的安全点上收尾。主核通常只实现其中一种（多数实现选强中断），而这里两种都要，因为 MCP 的处理器是用户代码——有的能安全中断，有的必须跑完才能保持一致性。

对 AI 工程的实际意义有三条。第一，**「被取消的请求永不应答」是一条必须显式实现的协议要求**，而不是自然结果：处理器在 `signal` 模式下会正常返回一个结果，若不检查 `cancel_requested` 就写出去，对端会收到一个它已声明放弃的请求的结果，行为上等于协议违规。这里用一个 `is_set()` 检查把它按住了，又用 `_settle_unanswered` 让传输能正确收场。

第二，**用户回调必须被包装才安全**。`_shielded_progress` 只有七行，但它防的是一类极难排查的故障：某个进度回调里一个 `KeyError`，导致整条连接被 anyio 任务组取消，症状表现为「连接随机断开」而日志里只有一条与断开无关的异常。凡是「把用户代码调度进框架的任务组」的地方，都该有这一层。

第三，**`True == 1` 这条 Python 陷阱在这份代码里被系统性防范了**：请求 id、进度令牌、进度值三处各设守卫。当一个字段既可以是 `int` 也可以是 `str`，而 `int` 又被用来做关联键时，`bool` 就是一个必须排除的输入——否则一个布尔参数就能冒充某个请求的 id，在关联域里制造静默的错配。
