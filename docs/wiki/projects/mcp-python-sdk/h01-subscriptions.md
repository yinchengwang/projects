---
project: mcp-python-sdk
feature: subscriptions
title: 订阅总线与监听
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/server/subscriptions.py
    symbols: [SubscriptionBus, InMemorySubscriptionBus, ListenHandler]
  - path: src/mcp/shared/subscriptions.py
    symbols: [event_to_notification, event_from_wire, ResourceUpdated]
---

# 订阅总线与监听

## 一句话职责

在 `2026-07-28` 线格式上实现 `subscriptions/listen`：一次请求换一条事件流，服务端用一个可插拔的 fan-out 总线把类型化事件分发给所有打开的流，并用一套客户端与服务端共享的准入谓词保证「只投递被 ack 过的东西」。

## 机制是什么

这条线格式取消了常驻 GET 流：客户端要收服务端事件，就发一个 `subscriptions/listen` 请求，**它的响应本身就是那条流**。因此三个组件各司其职：

| 组件 | 所在 | 职责 |
|---|---|---|
| `ServerEvent` 词表 | `shared/subscriptions.py` | 四个类型化事件，服务端与客户端共用 |
| `SubscriptionBus` | `server/subscriptions.py` | fan-out 接缝：只搬类型化事件，**从不接触 JSON-RPC** |
| `ListenHandler` | `server/subscriptions.py` | 服务一个 listen 流：先 ack、逐帧打戳、按已 ack 的过滤器投递 |

事件词表只有四种：`ToolsListChanged`、`PromptsListChanged`、`ResourcesListChanged`（三个无字段）、`ResourceUpdated(uri)`（带一个 URI）。共享模块的 docstring 给了一条贯穿两侧的设计原则：**每个事件都是电平触发（「这东西变了，在意的话就去重取」），所以两侧都可以靠去重来约束缓冲**。

`SUBSCRIPTION_ID_META_KEY = 'io.modelcontextprotocol/subscriptionId'`，值就是那条 listen 请求的 JSON-RPC id。

## 怎么实现的

**「电平触发」是整套协议能收敛的前提。** 事件表达的是状态（某个列表变了），不是增量（第 3 项被删了）。这让投递可以丢——丢了以后客户端重新 listen 再重取，拿到的是最新状态。模块 docstring 因此直接给出结论：投递是**发后不管且无重放**的，一条被丢掉的流不可恢复，客户端重新 listen 并重取即可。`ListenHandler` 关于 `max_buffered_events` 的文档把这句话说得更狠：一个客户端停止读取的流会在上限处**被结束**，因为「无重放，所以结束这条流不会丢失积压本来就已在丢的东西」。

**总线刻意不理解协议。** `SubscriptionBus` 的 docstring 写明：总线搬运的是类型化 `ServerEvent`，**订阅 id 打戳与按流过滤都由 listen 处理器负责**，所以一个自定义总线（例如用 Redis pub/sub 支持多副本）永远看不到 JSON-RPC。这条分层让「跨副本 fan-out」变成一个纯事件总线问题，不必让每个副本都懂线格式。

**接缝的同步/异步分工是显式约定的。** `publish` 是 `async`（后端要能发网络 I/O），`subscribe` 是**同步的本地注册**，监听器本身也是同步的、不允许抛异常、并在服务端的事件循环上被调用。这三条不是风格选择：订阅是本地状态操作，如果做成 async 就会在 `ListenHandler` 的 ack 之前引入一个可被取消的等待点。

**`InMemorySubscriptionBus` 用「每次订阅一个 token」作键。** 注释给了真实原因：**绑定方法（bound method）比较相等**，所以用一个可调用对象作字典键时，同一个方法注册两次会互相覆盖。用 `object()` 当 token 就消除了这个问题，也让 `subscribe` 返回的 `unsubscribe` 可以幂等（`pop(token, None)`）。

**`publish` 的循环里有两个易漏的细节。** 其一，逐个 listener 用 `try/except Exception` 包住并记日志，注释写明「一个坏 listener 不该饿死其他 listener、也不该让发布方的处理器失败」——这是 fan-out 边界的标准隔离。其二，循环之后 `await anyio.lowlevel.checkpoint()`，注释解释得非常具体：**一个任务连续发布一批事件时，这个 checkpoint 让 listen 流有机会在事件之间排空，而不是把它们的缓冲未读地撑爆**。

**`_safe_unsubscribe` 把同样的隔离用到反方向。** 它包住总线返回的 `unsubscribe`，理由写在 docstring 里：这个可调用对象来自自定义总线，一个会抛的实现不该阻止流自己的清理**释放订阅槽位**。也就是说清理路径上的两条动作（退订、释放槽位）必须彼此独立。

**准入谓词是双侧共享的，这是本页最关键的一条设计。** `event_matches(honored, uris, event)` 的 docstring 用一句话定义了它：**服务端投递与客户端收流都只承认被 ack 过的东西**。它的实现是逐类型判断：三个 `*_list_changed` 事件查对应布尔位是否**恰好为 `True`**，`ResourceUpdated` 查 URI 是否在 honor 集合里。注意用的是 `is True` 而不是真值判断——`SubscriptionFilter` 的字段是 `bool | None`，`None` 表示「没请求」，必须与 `False` 区分开。

**ack 回的是「honored 子集」，且会丢掉假值。** `_honored_subset` 把每个请求的开关归一成 `True if requested.x else None`，URI 列表非空才保留。它的 docstring 解释了为什么「请求什么都答应」：**某个事件类型会不会真的触发取决于服务端发布了什么，这就像订阅一个不存在的资源 URI 也会被答应、只是永远不会触发**。所以 ack 不是「我能给你这些」，而是「我按这个过滤器给你投递」。丢掉假值而不是回显 `false`，让 ack 的形状与「客户端请求了什么」对齐。

**ack 必须是第一帧，而实现用「先订阅、后发 ack」同时满足了两件事。** 代码里的注释解释了这处精妙排序：先 `subscribe` 再 `send_notification(ack)`，于是**在 ack 写入被挂起期间发布的事件会被缓冲而不是丢失**；而 ack 仍然是第一帧，因为**只有这一个任务在写这条流，而它要等 ack 发完才开始排空缓冲**。如果只做其中一件（先 ack 后订阅会丢事件，先订阅但并发写会乱序），都会出问题。

**每个流的缓冲是有界的，且到顶就直接结束。** `anyio.create_memory_object_stream(max_buffered_events)`。`deliver` 在 `send_nowait` 抛 `WouldBlock` 时做的三件事值得逐条看：记 warning、**立刻从 `self._streams` 里摘掉**、`send.close()`。注释给了立刻摘的理由：处理器自己的清理可能**卡在一次 transport 写入上**，而「关闭这个缓冲」唤不醒它（客户端已经不读了）——所以必须在这一刻就把订阅槽位释放掉，否则一个死掉的客户端会永久占用一个 `max_subscriptions` 名额。

**并发流上限在 ack 之前就拒。** `len(self._streams) >= max_subscriptions` 时抛 `INTERNAL_ERROR`，文档注明是「在 ack 之前」。这与「ack 是流的第一帧」契约一致：被拒的请求不能先给一个 ack 再断。

**两种关流方式语义不同，都被明确区分。** 客户端断开会取消处理器，于是 `async for` 结束、`finally` 清理，**流就这样结束**——这是规范里的 abrupt-close 契约。而 `close()` 是优雅关闭：它只对每个流的 `send` 端 `close()`，于是各流的 `async for` 自然结束、**先把缓冲排空再发出 `SubscriptionsListenResult` 作为最后一帧**，告诉客户端「这是有意结束的，不是掉了」。`close()` 的文档还注明了它**只发起、不等待**刷新完成——因为每个流的收尾由它自己的处理器任务做。

**序列化方向是两套函数，因为两侧拿到的形状不同。** `event_to_notification(event, meta)` 是服务端方向：把类型化事件转成带 `_meta` 戳的 `ServerNotification`。`event_from_wire(method, params)` 是客户端方向，它**接受原始 wire dict 而不是类型化通知**——docstring 明确说这是因为**客户端要在类型化解析之前做解复用**。所以客户端先用方法名分流（判断这帧属不属于 listen 流），再从原始参数里取 `uri`。

**`LISTEN_STREAM_METHODS` 声明了这条流的词表边界。** 它是一个 frozenset：三个 list_changed 方法与 `notifications/resources/updated`。docstring 补了个括号说明——这几个方法「在这条线格式上只走 listen 流，别处不走」。这句让「客户端该从哪里收这些通知」有了确定答案。

**每个请求 id 都是流身份。** `subscription_id = ctx.request_id`，缺失时抛 `INVALID_REQUEST`。同一个 id 同时用于：`_meta` 戳的值、`send_notification` 的 `related_request_id`（把通知关联回那条请求）、以及 `close()` 时的收尾戳。用一个值贯穿三处，避免了「流有多个等价标识且可能不一致」这类问题。

## 关键取舍

**源码事实：** 事件是电平触发而非增量，因此投递可以丢、无重放。客户端丢流的恢复动作是「重新 listen + 重取」。

**源码事实：** 缓冲有界，到顶即结束该流，并在那一刻主动释放订阅槽位——因为处理器清理可能卡在唤醒不了的 transport 写入上。

**源码事实：** 先订阅后 ack，使 ack 写入期间发布的事件被缓冲；ack 仍为第一帧，因为只有该处理器任务写流且排空在 ack 之后开始。

**源码事实：** 准入谓词双侧共享，只用 `is True` 判断布尔位，`None`（未请求）与 `False` 严格区分。

**源码事实：** ack 回的是清洗过的 honored 子集，非真值与空 URI 列表被丢掉而不是回显假值；服务端答应一切请求，包括永远不会触发的（不存在的资源 URI）。

**源码事实：** 总线只搬类型化事件，不接触 JSON-RPC；`publish` 异步、`subscribe` 同步、listener 同步且不许抛。

**源码事实：** `InMemorySubscriptionBus` 用 token 而非可调用对象作键，因为绑定方法比较相等。

**源码事实：** `publish` 在一个循环里逐个隔离 listener，循环末尾加 checkpoint 让流有机会排空。

**源码事实：** 优雅关闭与 abrupt close 语义不同：前者先排空缓冲再发终结结果帧，后者流直接结束。

**架构推论：** 这套设计把「多副本 fan-out」与「单流协议细节」彻底切开：总线是可替换的一维（内存 / Redis / NATS），而打戳、过滤、缓冲、关闭语义全在 `ListenHandler` 一处理完。代价是处理器承担了所有边界正确性——排序（ack 第一帧）、背压（有界缓冲 + 到顶即断）、清理顺序（退订与释放槽位互不依赖）、两种关闭流程。这些细节单条都不难，但同时做对才有「客户端断线不会拖垮服务端」这个性质。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/server/subscriptions.py` | `SubscriptionBus` | fan-out 接缝协议：只搬类型化事件，异步发布、同步订阅 |
| `src/mcp/server/subscriptions.py` | `InMemorySubscriptionBus` | 进程内实现，token 作键、逐 listener 隔离、末尾 checkpoint |
| `src/mcp/server/subscriptions.py` | `ListenHandler` | 服一条 listen 流：ack、打戳、过滤、有界缓冲、两种关闭 |
| `src/mcp/server/subscriptions.py` | `ListenHandler.__call__` | 先订阅后 ack 的排序，以及 `finally` 中的清理顺序 |
| `src/mcp/server/subscriptions.py` | `ListenHandler.close` | 优雅关闭：排空缓冲后以结果帧收尾 |
| `src/mcp/server/subscriptions.py` | `_honored_subset` | ack 的过滤器归一化，丢弃非真值与空列表 |
| `src/mcp/server/subscriptions.py` | `_safe_unsubscribe` | 隔离总线的退订异常，不阻断槽位释放 |
| `src/mcp/shared/subscriptions.py` | `event_to_notification` | 服务端方向：类型化事件 → 带 `_meta` 戳的通知 |
| `src/mcp/shared/subscriptions.py` | `event_from_wire` | 客户端方向：原始 wire dict → 事件，供解复用前使用 |
| `src/mcp/shared/subscriptions.py` | `event_matches` | 双侧共享的准入谓词：只承认已 ack 的 |
| `src/mcp/shared/subscriptions.py` | `ResourceUpdated` | 唯一带载荷的事件：变更的资源 URI |
| `src/mcp/shared/subscriptions.py` | `LISTEN_STREAM_METHODS` | 走 listen 流的方法词表边界 |
| `src/mcp/shared/subscriptions.py` | `SUBSCRIPTION_ID_META_KEY` | 每帧的订阅标识 `_meta` 键 |

## 参考

- [mcp-python-sdk 源码：server/subscriptions.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/subscriptions.py)
- [mcp-python-sdk 源码：shared/subscriptions.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/shared/subscriptions.py)
- [MCP 规范：变更通知与订阅](https://modelcontextprotocol.io/specification/2025-06-18/server/resources#subscriptions)
- [anyio 文档：内存对象流](https://anyio.readthedocs.io/en/stable/streams.html)

## 与主核 / AI 的关系

订阅总线与主核的「LISTEN/NOTIFY」是同一类机制的两代形态。Postgres 的通知也是**电平式**的（「这个频道有事」），不保证送达、不重放、接收方自己去查当前状态。`SubscriptionBus` 相当于频道注册表，`ListenHandler` 相当于把通知搬进某条连接的那段逻辑。

对 agent 工程的直接价值有三条。其一，**把「事件变了」与「事件增量」分清**。一旦事件携带增量，就必须保证送达与顺序，进而必须做重放与去重；而电平事件允许丢弃，于是背压处理可以简单到「到顶就断」。这个选择决定了整套实现的复杂度。其二，**ack 之前先订阅**。任何「先确认再开始收」的协议都有这个窗口，正确做法是先把接收方就位、再发确认，并靠「只有一个任务写流」来保证确认仍是第一帧。其三，**清理路径上的两个动作不能互相依赖**。退订与释放槽位都必须能单独成功，否则一个异常的第三方退订实现就能把连接名额永久泄漏。
