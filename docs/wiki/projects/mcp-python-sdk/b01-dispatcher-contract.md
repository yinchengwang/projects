---
project: mcp-python-sdk
feature: dispatcher-contract
title: 分发器契约与调度选项
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/shared/dispatcher.py
    symbols: [Dispatcher, CallOptions, DispatchContext]
  - path: src/mcp/shared/direct_dispatcher.py
    symbols: [DirectDispatcher]
---

# 分发器契约与调度选项

## 一句话职责

`Dispatcher` 是一份**刻意不认 MCP** 的协议：它把一个双工消息通道拆成「出站 API（`send_raw_request` / `notify`）+ 入站泵（`run(on_request, on_notify)`）」，方法名只是字符串、参数与结果只是 `dict[str, Any]`；`CallOptions` 是每次调用的调度旋钮，`DispatchContext` 是处理一条入站消息时能用的回问通道。

## 机制是什么

这层的价值在于**切一刀**：MCP 的类型层（请求/结果模型、能力协商、`Context`）在它之上，线缆编码（JSON-RPC、gRPC、进程内直调）在它之下。因此 `Dispatcher` 只承诺四件事：

1. **关联出站请求与入站结果**（谁在等哪个 id 的回复）；
2. **驱动接收循环**（`run` 直到通道关闭）；
3. **为每条入站请求提供隔离**（各自一个任务，一个出错不拖垮其他）；
4. **接好取消与进度**。

`DispatchContext` 是这一层的第二半：入站处理函数拿到的不是光秃秃的 `(method, params)`，而是「方法 + 参数 + 一条能反向发消息的通道 + 这条消息的传输元数据 + 一个取消事件 + 一个进度上报口」。于是「处理请求时需要回问客户端」（例如 sampling、elicitation）不需要额外传一堆参数，通道就在上下文里。

两个实现互为对照：`JSONRPCDispatcher` 是生产实现（JSON-RPC 帧 + 在途表 + 取消/进度接线），`DirectDispatcher` 是**零序列化、零流、零帧**的进程内实现。后者的存在本身是一个证明：如果协议抽象得对，换掉整条线缆层不该需要改动上面的任何代码。

`Dispatcher` 的 docstring 还自己标了一句「生命周期接口是暂定的，`run()` 可能在 2.x 小版本里改」。在对外契约里主动声明哪部分是临时的，比假装稳定更诚实。

## 怎么实现的

**`Dispatcher` 继承 `Outbound`，只加一个方法。**

```
class Dispatcher(Outbound, Protocol[TransportT_co]):
    async def run(self, on_request, on_notify, on_notify_intercept=None, *,
                  task_status: anyio.abc.TaskStatus[None] = anyio.TASK_STATUS_IGNORED) -> None: ...
```

`Outbound` 是更窄的协议：`send_raw_request(method, params, opts)` 与 `notify(method, params, opts)`。`Outbound` 的 docstring 明确「`Dispatcher`（顶层出站）与 `DispatchContext`（入站请求期间的回问）都扩展它」，也就是说**出站能力是这一层的公共底座**，装客户端与服务端两种角色都靠它。

`Dispatcher` 用的是 `Protocol[TransportT_co]`，`TransportT_co` 是 `bound=TransportContext` 且**协变**的类型变量。协变的原因很实在：`DispatchContext[TransportT_co].transport` 是只读属性，协变让一个 `DispatchContext[MyTransport]` 能当 `DispatchContext[TransportContext]` 用——这正是「分发器不透明透传，上层才读具体字段」这条设计的类型前提。

**`CallOptions` 是 `TypedDict(total=False)`，六个键全部可选。**

源码 docstring 直接写「所有键都可选，分发器忽略自己看不懂的键」——这是版本演进的关键约定：新加旋钮不需要改协议，老实现自然忽略。六个键分两类：

- **关联控制**：`request_id`（用调用方指定的 id 发，而不是分发器铸的）、`on_progress`（接收本轮请求的进度通知）、`timeout`（等结果的秒数，超时后抛错并发 `notifications/cancelled`）。
- **传输提示**：`resumption_token` / `on_resumption_token`（断线续传，客户端侧、仅 streamable HTTP）、`headers`（HTTP 传输合并到出站请求上，非 HTTP 忽略）。

`request_id` 的 docstring 是六个里最长的，因为它承载了一个不显然的语义：**对端看到的是原样值**（`"7"` 仍是字符串），而「与发送方自己的某个在途 id 撞车」会抛 `ValueError`。再往下还有一层：整数 id 与「能解析成整数的字符串」共享分发器自己的计数器域，所以一个已被用过的号在该请求结束后**可能被再次铸出**。若需要整个会话唯一的 id，得用「不解析成整数的字符串」（例如 `listen-1`、`listen-2`），源码说 `subscriptions/listen` 的驱动就是这么做的。

`cancel_on_abandon` 的 docstring 明确了默认值与例外：默认 `True`；协议不允许取消的请求（例如 `initialize`）置 `False`；此外「续传提示真的到达传输层」或「请求根本没写出去」时也会被抑制。这条判断在分发器里对应那句注释：「一个既不续传也不取消的请求会**泄漏对端的工作**」。

`resumption_token` 的 docstring 末尾带一条时间线信息：SSE 流续传在下一个协议修订里被移除。这类「能力有寿命」的说明应该出现在字段文档里，而不是 changelog 里。

**`DispatchContext` 是五个属性加两个动作。**

```
class DispatchContext(Outbound, Protocol[TransportT_co]):
```

五个只读属性：`transport`（传输元数据）、`can_send_request`（当前能不能回问，上下文关闭后为假）、`request_id`（入站请求的 id，通知为 `None`）、`message_metadata`（传输附加的原始元数据，注释带 `TODO` 说它绑在 `SessionMessage` 线格式上、等传输改造后就没了）、`cancel_requested`（`anyio.Event`，对端取消时置位）。

两个动作：`send_raw_request` 与 `notify`（来自 `Outbound`），外加 `progress(progress, total, message)`。

几个约束值得逐一记住：

- `can_send_request` 为假时 `send_raw_request` **恰好**抛 `NoBackChannelError`——docstring 用「exactly when」这种措辞，把它从一个描述提升为一条可依赖的契约。
- `progress` 在「对端没提供进度令牌」时是**空操作**，不报错。于是处理器可以无条件上报进度，不必先查有没有令牌。
- `message_metadata` 上挂着 `TODO(maxisbey): remove for context rework`，这是全套代码里多处出现的同一个迁移标记：一旦传输不再以 `SessionMessage` 交付消息，这个属性就该消失。

**三个回调类型别名，其中一个有硬性顺序要求。**

`OnRequest = Callable[[DispatchContext, str, Mapping | None], Awaitable[dict[str, Any]]]`——返回 `dict`，抛 `MCPError` 则以错误应答。

`OnNotify` 同形但返回 `None`。

`OnNotifyIntercept = Callable[[str, Mapping | None], bool]` 是**同步**的，且返回「是否已被消费」。它的 docstring 里有一句契约级的约束：

> MUST offer every inbound notification to `on_notify_intercept` synchronously in receive order, handing only unconsumed ones to `on_notify`.

理由也写了：「让关联状态按线序推进，与响应解析保持相对顺序（客户端 listen 的解复用依赖这一点）」，并且「不得阻塞接收路径」。这是全页最容易被违反也最难在事后发现的一条——异步化这个拦截器看起来无害，却会打乱顺序。

配套的 `run_notify_intercept` 负责把它包成「抛异常只影响这一条通知，不影响接收循环」：捕获后记日志并返回 `False`（即「没消费，继续往下传」）。

**`run()` 的 `task_status` 参数是一处精心的接口设计。**

签名里带 `task_status: anyio.abc.TaskStatus[None] = anyio.TASK_STATUS_IGNORED`，docstring 说明「`task_status.started()` 在分发器能接受 `send_request`/`notify` 调用时触发，于是调用方可以写 `await tg.start(dispatcher.run, on_request, on_notify)`」。这就是 anyio 的 task-group 启动协议：`tg.start()` 会等到 `started()` 被调用才返回，于是「分发器已就绪」这个事实不需要额外的 `Event` 或轮询来表达。

**`DirectDispatcher` 是契约的可实现性证明。**

模块 docstring 用列表写清了它存在的三个理由：证明 `Dispatcher` 协议可以在没有 JSON-RPC 的情况下实现；为分发器之上的层（`ServerRunner`、`Context`、`Connection`）提供无线上部件的快速测试基底；在 JSON-RPC 开销不必要时把服务端嵌进同一进程。

它的 `run()` 实现短得近乎滑稽：存下三个回调、`self._running = True`、`self._ready.set()`、`task_status.started()`，然后 `await self._close_event.wait()`。**没有接收循环**——因为对端直接调 `self._on_request(...)`，不需要经过任何通道。`finally` 里把 `_running` 置假、`_closed` 置真并 `_close_event.set()`，注释说明「`run()` 可能经由取消结束而从未调用 `close()`；置位事件让等待者看到已关闭状态而不是永远停着」。

`_dispatch_request` 里有几处与 `JSONRPCDispatcher` 精确对齐的行为：

- 整个流程包在 `anyio.fail_after(opts.get("timeout"))` 里，注释点明「在超时作用域内，所以配置的超时也约束『等对端 run() 启动』这件事」；
- 幂等性：调用方指定的 `request_id` 若已在途，抛 `ValueError(f"request id {request_id!r} is already in flight")`；而**自己铸号时会跳过**在途的号——注释解释「撞车错误留给真正选了那个号的调用方」；
- 异常映射与线上路径一致：`ValidationError` → `INVALID_PARAMS`（`data=""`，与 JSON-RPC 分发器写出的形状相同，注释说是为了让「runner over direct」的测试看到与「runner over JSON-RPC」一样的结果）；其他异常 → `INTERNAL_ERROR`，且 `raise_handler_exceptions=True` 时把原异常挂成 `__cause__`（便于进程内调试），`False` 时消毒成不透明消息（与线上路径的防泄漏一致）；
- 超时 → `REQUEST_TIMEOUT`，消息里带方法名。

`create_direct_dispatcher_pair()` 造一对互连实例，**两侧共用一个 `TransportContext` 实例**：

```
ctx = TransportContext(kind=DIRECT_TRANSPORT_KIND, can_send_request=can_send_request, headers=headers)
client = DirectDispatcher(ctx, raise_handler_exceptions=raise_handler_exceptions)
server = DirectDispatcher(ctx, raise_handler_exceptions=raise_handler_exceptions)
```

`can_send_request=False` 就得到一个「没有回问通道」的传输，用来测那条拒绝路径。docstring 还老实承认「接线是对称的，所以 client / server 两个角色只是约定」。

## 关键取舍

**源码事实：** `Dispatcher` 与 `DispatchContext` 都不认 MCP：方法名是 `str`、参数与结果是 `dict[str, Any]`。类型层完全在它们之上。

**源码事实：** `run()` 用 anyio 的 `task_status` 协议宣告就绪，而不是另设就绪事件。调用方 `tg.start(...)` 返回即意味着可以发消息。

**源码事实：** `OnNotifyIntercept` 是**同步**的，且必须按接收顺序对每条通知调用一次，未被消费的才转给 `on_notify`。理由是关联状态必须按线序推进（客户端 listen 的解复用依赖它）。

**源码事实：** `CallOptions` 是 `total=False` 的 TypedDict，契约里写明「分发器忽略自己不认识的键」。新旋钮因此可以向后兼容地加入。

**源码事实：** `request_id` 允许调用方自带 id；对端看到原样值，撞在途 id 抛 `ValueError`；整数型 id 与分发器自铸号共享计数器，可能在被释放后重用，需要会话级唯一时应使用非整数解析的字符串。

**源码事实：** `progress()` 在无令牌时是空操作，让处理器可以无条件上报进度。

**源码事实：** `DirectDispatcher` 的 `run()` 没有接收循环——对端直接调用回调；它的异常映射与超时语义与 `JSONRPCDispatcher` 逐条对齐，以便上层测试在两种实现下行为一致。

**源码事实：** `create_direct_dispatcher_pair()` 让两侧共享同一个 `TransportContext` 实例，连接的角色「只是约定」。

**架构推论：** 把「出站能力」抽成 `Outbound` 并让 `Dispatcher` 与 `DispatchContext` 都扩展它，使得「顶层发消息」与「处理请求时回问」在调用方看来是同一种东西——这正是 `BaseContext` 能只做一层转发就实现 `report_progress` 等能力的原因。

**架构推论：** 用 `TypedDict(total=False)` + 「忽略未知键」做前向兼容，代价是拼错的键会被静默忽略。这是把「可演进」优先于「可发现」的一次明确取舍，缓解手段是每个键都附完整 docstring。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/shared/dispatcher.py` | `Dispatcher` | 双工通道协议：出站 API + 入站泵 |
| `src/mcp/shared/dispatcher.py` | `Outbound` | 更窄的出站协议，被分发器与请求上下文共同实现 |
| `src/mcp/shared/dispatcher.py` | `CallOptions` | 每次调用的调度旋钮，全部可选且未知键被忽略 |
| `src/mcp/shared/dispatcher.py` | `DispatchContext` | 处理一条入站消息时的上下文与回问通道 |
| `src/mcp/shared/dispatcher.py` | `OnNotifyIntercept` | 同步、按接收顺序、可消费的通知拦截契约 |
| `src/mcp/shared/direct_dispatcher.py` | `DirectDispatcher` | 零线缆的进程内实现，证明协议可实现 |
| `src/mcp/shared/direct_dispatcher.py` | `create_direct_dispatcher_pair` | 造一对互连实例，两侧共享传输上下文 |

## 参考

- [mcp-python-sdk 源码：shared/dispatcher.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/shared/dispatcher.py)
- [mcp-python-sdk 源码：shared/direct_dispatcher.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/shared/direct_dispatcher.py)
- [MCP 规范：基础协议（请求/响应）](https://modelcontextprotocol.io/specification/2025-06-18/basic)
- [anyio 文档：任务组与 TaskStatus](https://anyio.readthedocs.io/en/stable/tasks.html)

## 与主核 / AI 的关系

「把线缆层抽成一个不认业务语义的协议」与主核里「存储引擎不认 SQL、只认页与记录」是同一种分层：下层的契约越窄，上层的可移植性越强。主核通常不会有「两个实现互为证明」的习惯，而这里 `DirectDispatcher` 的存在就是一份可执行的规格——如果哪天有人给 `Dispatcher` 加了只有 JSON-RPC 才满足得了的要求，`DirectDispatcher` 会立刻编不过或跑不通。

对 AI 工程的实际意义有三条。第一，**`OnNotifyIntercept` 必须同步且保持顺序**这条契约提醒我们：并发化的诱惑在「拦截器/观察者」这类位置最危险，因为顺序错了不会报错，只会让解复用偶尔错乱。第二，`CallOptions` 的「忽略未知键」是让协议能长期加参数的低成本手段，但它把「拼错的键静默无效」变成了已知代价——值得用类型检查或断言在开发期补偿。第三，`DirectDispatcher` 那种「整个异常映射与线上路径逐条对齐」的做法值得模仿：**内存实现最容易在错误语义上漂移**，而漂移之后依赖它写的测试就不再能证明线上行为。
