---
project: mcp-python-sdk
feature: server-session
title: 服务端会话与出站通道
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/server/session.py
    symbols: [ServerSession]
  - path: src/mcp/server/connection.py
    symbols: [Connection, NotifyOnlyOutbound]
---

# 服务端会话与出站通道

## 一句话职责

`Connection` 是**每条客户端连接的持久状态**（对端能力、协议版本、会话 id、待清理资源栈、一条连接级出站通道）；`ServerSession` 是**每个入站请求的临时代理**，把「回问对端」这件事分派到两条通道上——请求作用域的那条，或连接级的那条。

## 机制是什么

两层的区别按生命周期划：

- `Connection` 一个客户端一个，从连接建立起活到连接结束。它持有 `outbound`（连接级通道）、`client_params` / `client_capabilities` / `protocol_version`、`session_id`、`initialized` 事件、`state` 字典、`exit_stack`。
- `ServerSession` 一个入站请求一个，由 `ServerRunner._make_context` 现造。它持有两条通道的引用，并按调用方给的 `related_request_id` 选择用哪条。

这个划分对应一个真实约束：streamable HTTP 上「请求级响应流」与「那条常驻 GET 流」是两条不同的通道。响应一条请求时产生的通知应当回到**该请求自己的流**上（否则客户端那边分不清是哪次调用的旁路信息），而连接级的主动推送只能走 GET 流。所以选择依据不是「谁在发」，而是「这条消息关联哪个请求」。

`NotifyOnlyOutbound` 是第三种通道：它转发现有通知、但 `send_raw_request` 按构造就拒绝。它给 2026-07-28 的双工连接用——管道是真的，通知能走，但那个年代的协议禁止服务端发起 JSON-RPC 请求。

## 怎么实现的

**`Connection` 的字段与两个工厂。**

字段有九个，每个都带 docstring。几个值得细看的：

- `outbound: Outbound` — 连接级通道。真通道与「无通道哨兵」`_NO_CHANNEL` 靠**身份**区分，`has_standalone_channel` 属性的实现就是 `return self.outbound is not _NO_CHANNEL`。
- `client_capabilities` — docstring 写明了它和 `client_params` 的耦合关系：「由 setter 与 `client_params` 保持同步，但也能独立设置——现代信封里能力是必需的、客户端信息是可选的（规范 PR #3002），能力检查不能依赖对端是否自我标识」。
- `initialized: anyio.Event` — 「`notifications/initialized` 到达时置位（对齐 TS 的 `oninitialized`）；从这一点起规范允许服务端在 ping/logging 之外发起请求。`from_envelope` 建的连接**预先置位**」。
- `exit_stack: AsyncExitStack` — 「连接关闭时按 LIFO 屏蔽地展开。处理器或中间件可以往里压清理逻辑；异常记日志并吞掉」。

`client_params` 的 setter 是同步点：

```
@client_params.setter
def client_params(self, value):
    self._client_params = value
    if value is not None:
        self.client_capabilities = value.capabilities
```

注释说明两件事：赋值即同步（记录完整客户端参数就等于记录了能力事实，两者永不错位）；但**清空成 `None` 时不动 `client_capabilities`**——因为现代信封只声明能力、不声明客户端信息。

两个工厂的语义差异就是「出生时状态如何」：

```
@classmethod
def from_envelope(cls, protocol_version, client_info, client_capabilities, *, outbound=_NO_CHANNEL) -> Connection:
```

它的 docstring 极详细：`protocol_version` 必须是**已验证过**的字符串（拒绝非法值归入站分类阶梯管）；`client_info` / `client_capabilities` 是信封里的原始值，由这个构造器负责把它们变成连接身份，「在每个现代入口上都一致，于是形状不对的值降级成『未提供』而不是让请求失败」。行为是：`initialized` 置位、形状良好的能力记成 `client_capabilities`、两者都有时额外合成完整 `client_params`。`outbound` 默认是有哨兵——单次交换 HTTP 路径没有回问通道；双工现代传输（如 stdio）传一个 `NotifyOnlyOutbound` 包住分发器。

```
@classmethod
def for_loop(cls, outbound, *, session_id=None, protocol_version_hint=None) -> Connection:
```

「不是生来就绪：`initialized` 稍后由内核在 `notifications/initialized` 到达时置位。`protocol_version` 用传输提示（或 `LATEST_HANDSHAKE_VERSION`）播种，于是永不为 `None`；握手协商后会覆盖它」。

`for_loop` 那句注释很重要：`protocol_version` 从构造起就有一个**合理的默认值**，而不是空待协商再填。这让内核「把 `protocol_version` 当既成事实读」（模块 docstring 的原话）成为可能。

`initialize_accepted` 是个派生属性而非存储状态：

```
@property
def initialize_accepted(self) -> bool:
    return self.client_params is not None or self.initialized.is_set()
```

docstring 说「一旦入站请求闸门打开即为真：`initialize` 记录了对方信息，或握手已整体完成（生来就绪，或收到裸的 `notifications/initialized`）。**派生的，从不存储**」。用派生而非存储，避免了两个状态位不同步。

**三种出站通道的层次。**

最底层是 `_NoChannelOutbound`：`send_raw_request` 直接抛 `NoBackChannelError(method)`，`notify` 记 debug 日志「dropped %s: no standalone channel」。模块级常量 `_NO_CHANNEL = _NoChannelOutbound()` 让它能用身份比较。

`NotifyOnlyOutbound` 继承它，只覆写 `notify`：

```
def __init__(self, outbound: Outbound) -> None:
    self._outbound = outbound

async def notify(self, method, params, opts=None) -> None:
    if method in LISTEN_STREAM_METHODS:
        logger.debug("dropped %s: delivered via subscriptions/listen at this era", method)
        return
    await self._outbound.notify(method, params, opts)
```

docstring 给出的理由分两层：类层面「安装给现代（2026-07-28+）双工连接：管道是真的，所以服务端通知能走，但现代协议禁止服务端发起的 JSON-RPC 请求，所以 `send_raw_request`（继承来的）按构造就拒绝」；`notify` 层面「变更通知（`notifications/*/list_changed`、`notifications/resources/updated`）带 debug 日志丢弃：这个年代它们只能通过客户端自己打开的 `subscriptions/listen` 流到达客户端，所以共享通道上一份裸拷贝会是**未被请求的通知**。请改用服务端的 `SubscriptionBus` 发布」。

这段注释解释了一个容易搞错的地方：即使在同一个协议年代、同一条连接上，「能发通知」也不是一刀切——某些具体方法的通知在该年代必须走订阅流。用方法名集合（`LISTEN_STREAM_METHODS`）表达这个约束，并直接丢弃而非报错，因为通知本来就不可靠。

**`Connection.send_raw_request` 是转发加文档。**

```
async def send_raw_request(self, method, params, opts=None) -> dict[str, Any]:
    """... Raises: MCPError / NoBackChannelError ..."""
    return await self.outbound.send_raw_request(method, params, opts)
```

docstring 写「低层 `Outbound` 通道。优先用类型化的 `send_request` 或下面的便捷方法；只在发规范外消息时直接用它」。再往下是四个 `@overload` 的 `send_request`（`CreateMessageRequest` → `CreateMessageResult`、`ElicitRequest` → `ElicitResult`、`ListRootsRequest` → `ListRootsResult`、还有一个 `PingRequest`），把「哪个请求对应哪个结果类型」编进重载，于是调用方不需要指定 `result_type`。

**`allowed_log_levels` —— 一个跨层的门禁函数。**

虽然它在 `connection.py` 里，但服务于 `ServerSession` 的日志发送。它的 docstring 把三个年代的行为写清了：

- 2026-07-28+ 把日志投递变成**逐请求 opt-in**：客户端在保留的 `io.modelcontextprotocol/logLevel` 这个 `_meta` 键里指定；**缺席即无级别——服务端 MUST NOT 发送**；存在即「该级别及以上」。无法识别的值按缺席处理。
- 规范方法在 surface 校验阶段就已拒掉畸形值（在任何处理器运行之前），所以「无法识别」那一支只服务自定义方法，那里丢弃是安全方向。
- 连接级发送者传 `meta=None`：2026 的 `logging/setLevel` 已不存在、日志投递只按请求作用域，所以它们什么都不发。
- 握手版本保持 `logging/setLevel` 时代的语义：所有级别都可发，过滤是应用的 `logging/setLevel` 处理器的职责。

`_LOG_LEVELS` 的构造也值得一提：`get_args(LoggingLevel)`，注释写「按严重性升序，取自 `LoggingLevel` 字面量的声明顺序（RFC 5424 标度）——字面量是顺序的唯一来源」。即顺序不是硬编码的列表，而是从类型定义里读出来的。

**`ServerSession` 的构造只做两件事。**

```
def __init__(self, request_outbound, connection, *, request_meta=None) -> None:
    self._request_outbound = request_outbound
    self._connection = connection
    self._log_is_request_scoped = connection.protocol_version in MODERN_PROTOCOL_VERSIONS
    self._allowed_log_levels = allowed_log_levels(connection.protocol_version, request_meta)
```

两条「在构造时定死」的判断：日志是否请求作用域（按协议年代）、以及这个请求允许哪些日志级别（按 `_meta` opt-in）。注释解释了为什么定死：「在 2026-07-28+ 上，入站请求的 `_meta` 日志级别 opt-in 决定这个请求可以发哪些 `notifications/message` 级别（且它们只搭这个请求的流）；在握手版本上所有级别都可发」。

**`send_request` 的通道选择是这一页的核心逻辑。**

```
related = metadata.related_request_id if metadata is not None else None
channel = self._request_outbound if related is not None else self._connection.outbound
data = request.model_dump(by_alias=True, mode="json", exclude_none=True)
opts: CallOptions = {}
if request_read_timeout_seconds is not None: opts["timeout"] = request_read_timeout_seconds
if progress_callback is not None: opts["on_progress"] = progress_callback
result = await channel.send_raw_request(data["method"], data.get("params"), opts or None)
try:
    _methods.validate_client_result(request.method, self.protocol_version, result)
except KeyError:
    pass
return result_type.model_validate(result, by_name=False)
```

四个要点：

1. **`related_request_id` 是唯一选择依据**：有就用请求级通道，没有就用连接级。
2. 序列化用 `by_alias=True, mode="json", exclude_none=True`——别名保证线上字段名，`exclude_none` 保证不出现 JSON-RPC 不允许的 `null`。
3. **`KeyError` 被显式吞掉**：`validate_client_result` 用 `(method, version)` 做 surface 查表，若该方法在无版本映射里（客户端自定义方法）会抛 `KeyError`，表示「不校验」而非「失败」。
4. 先用 surface 校验线上形状，再用 `result_type` 解析成类型化模型——与 `methods.py` 的两段式完全同构。

`send_notification` 与私有的 `_notify` 是同一逻辑的简化版：

```
async def send_notification(self, notification, related_request_id=None) -> None:
    await self._notify(notification, request_scoped=related_request_id is not None)

async def _notify(self, notification, *, request_scoped: bool) -> None:
    channel = self._request_outbound if request_scoped else self._connection.outbound
    data = notification.model_dump(by_alias=True, mode="json", exclude_none=True)
    await channel.notify(data["method"], data.get("params"))
```

注意参数从 `related_request_id` 退化成了布尔 `request_scoped`——因为通知不需要知道具体是哪个请求，只需要知道走哪条通道。这是「先归约再传递」的一个小例子。

**`send_log_message` 的守卫是这一页最有信息量的地方。**

```
if level not in self._allowed_log_levels:
    _logger.debug("dropped notifications/message at %r: not opted in at that level on this request", level)
    return
await self._notify(
    types.LoggingMessageNotification(params=...),
    request_scoped=self._log_is_request_scoped or related_request_id is not None,
)
```

两个判断叠在一起：

1. 级别不在允许集合内就**静默丢弃**（debug 日志），不报错——日志是装饰性通道。
2. **通道选择不再只看 `related_request_id`**，而是 `self._log_is_request_scoped or related_request_id is not None`。docstring 给了理由：「在 2026-07-28+ 上投递是逐请求 opt-in：除非这个请求的 `_meta` 带了保留的日志级别键，否则什么都不发，且低于请求级别的条目被丢弃（debug 日志）。发出去的内容**无论 `related_request_id` 如何都搭这个请求的流**——规范禁止 `notifications/message` 出现在除承载响应之外任何流上。握手版本则像以前一样在 `related_request_id` 选定的通道上无条件发送」。

这是本页最值得记的一条：**同一个「通道选择」问题，日志通知的答案比普通通知更严格**。普通通知可以走连接级通道，但日志消息在该年代必须跟响应同流。所以 `_log_is_request_scoped` 这个预计算字段不是优化，而是语义需要。

模块顶部还有一个 `_logger` 别名的注释：「`send_log_message` 的 `logger` 参数（公开 API，规范的 logger 名字段）在该方法内遮蔽了模块 logger；这个别名让模块 logger 在那里仍可达」。`context.py` 与 `connection.py` 里各有同一处处理——这是一个「公开 API 的名字与内部约定撞车」的重复模式，三次出现说明它是真实困扰。

## 关键取舍

**源码事实：** `Connection` 是每连接持久状态，`ServerSession` 是每请求临时代理；后者由 `ServerRunner._make_context` 现造，只持有两条通道的引用。

**源码事实：** 通道选择只看 `related_request_id` 是否存在：有则请求级通道，无则连接级通道。日志消息是唯一的例外（见下）。

**源码事实：** `client_params` 的 setter 是同步点：设置非 `None` 值时同步更新 `client_capabilities`；清空成 `None` 时**不动**能力（现代信封只声明能力、不声明客户端信息）。

**源码事实：** `initialize_accepted` 是派生属性（`client_params is not None or initialized.is_set()`），从不存储，避免双位不同步。

**源码事实：** `__init__` 里把「日志是否请求作用域」与「允许的日志级别」两条判断一次算好。日志投递的通道选择是 `_log_is_request_scoped or related_request_id is not None`，因为规范禁止 `notifications/message` 出现在承载响应之外的流上。

**源码事实：** `NotifyOnlyOutbound` 转发普通通知，但把 `LISTEN_STREAM_METHODS` 里的方法名带 debug 日志丢弃——那些变更通知在该年代只能经 `subscriptions/listen` 流到达。

**源码事实：** `send_request` 里 `validate_client_result` 抛的 `KeyError` 被显式吞掉，表示「这个方法不在已知映射里，不校验」而不是失败。

**架构推论：** 把「连接级」与「请求级」两条通道都挂在会话对象上、由调用方用 `related_request_id` 选择，使同一个 `send_request` API 能适配 stdio（只有一条通道）与 streamable HTTP（两条），而不需要在 API 上加传输分支。代价是调用方必须理解 `related_request_id` 的语义——它不是一个调试字段，而是路由开关。

**架构推论：** `Connection` 把所有「对端告诉我们的事实」（能力、版本、会话 id）与「我们自己的资源」（`exit_stack`、`state`）混在一个类里，让处理器只需拿一个 `Connection` 就能读到全部上下文。代价是这个类承担了两种生命周期概念，且它必须被谨慎地深拷贝/序列化（`outbound` 是活的，不可序列化）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/server/connection.py` | `Connection` | 每连接持久状态：通道、身份、初始化事件、清理栈 |
| `src/mcp/server/connection.py` | `Connection.from_envelope` | 现代单次交换：生来就绪、默认无回问通道 |
| `src/mcp/server/connection.py` | `Connection.for_loop` | 握手驱动：版本用提示或最新握手版播种，`initialized` 待置 |
| `src/mcp/server/connection.py` | `Connection.initialize_accepted` | 派生属性，表达入站闸门是否已开 |
| `src/mcp/server/connection.py` | `NotifyOnlyOutbound` | 转发通知但按构造拒绝服务端发起请求 |
| `src/mcp/server/connection.py` | `allowed_log_levels` | 按协议年代与 `_meta` opt-in 算可发送的日志级别 |
| `src/mcp/server/session.py` | `ServerSession` | 每请求代理：双通道 + 类型化回问/通知 |
| `src/mcp/server/session.py` | `ServerSession.send_request` | 按 `related_request_id` 选通道，两段式校验后解析 |
| `src/mcp/server/session.py` | `ServerSession.send_log_message` | 级别门禁 + 「日志必跟响应流」的通道强制 |

## 参考

- [mcp-python-sdk 源码：server/connection.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/connection.py)
- [mcp-python-sdk 源码：server/session.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/session.py)
- [MCP 规范：采样（服务端向客户端请求）](https://modelcontextprotocol.io/specification/2025-06-18/client/sampling)
- [MCP 规范：日志](https://modelcontextprotocol.io/specification/2025-06-18/server/utilities/logging)

## 与主核 / AI 的关系

「连接态 + 请求态」两层与主核里的「会话 + 语句执行」是同构的：会话持有事务默认值、临时表、权限，语句执行持有本次的游标与参数。主核通常把这两层强行分开（会话不允许携带语句级数据），这里也一样——`ServerSession` 不缓存任何跨请求状态，`Connection` 不记录任何单请求数据。

对 AI 工程的实际意义在于**「同一条连接上的两条通道」这件事必须有明确的选路规则**。streamable HTTP 把「响应这条请求」与「主动推送」拆成两条流之后，任何出站消息都必须回答「走哪条」。这里给出的规则极简——`related_request_id` 有就用请求级——但日志消息是例外，因为规范对它更严：它必须搭响应流，否则客户端会收到一条无法归属的日志。**当一条简单规则出现例外时，把例外写成独立字段（`_log_is_request_scoped`）并在构造时算好**，比在每次调用时重新推导要可靠得多。

另一个值得借鉴的是 `client_params` setter 的不对称：设置时同步能力，清空时不同步。这类「赋值有副作用」的设计通常危险，但这里它解决了一个真实的双状态问题（能力必需、客户端信息可选）。关键在于把不对称性写进注释，说明「为什么清空时不跟着清」——否则后来的人一定会把它「修正」成对称的，然后破坏现代信封路径。
