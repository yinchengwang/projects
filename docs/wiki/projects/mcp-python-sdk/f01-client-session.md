---
project: mcp-python-sdk
feature: client-session
title: 客户端会话与握手
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/client/session.py
    symbols: [ClientSession, ClientRequestContext]
---

# 客户端会话与握手

## 一句话职责

`ClientSession` 是客户端的一半：它跑在一个分发器之上，负责把「一次握手」变成一份协商结果（两代协议各有各的握手方式）、把每个出站请求按协商结果打上正确的信封、再把入站的服务端请求分派给用户提供的回调。

## 机制是什么

这一层要处理的复杂性几乎全部来自**协议有两个时代**：

| 时代 | 握手方法 | 版本从哪来 | 信封形态 |
|---|---|---|---|
| 旧（handshake） | `initialize` + `notifications/initialized` | 响应体的 `protocolVersion` | 只加 `Mcp-Protocol-Version` 头 |
| 新（modern） | `server/discover` | 响应体的 `supported_versions` 与本地取交集 | 每个请求的 `params._meta` 里带版本/客户端信息/能力 |

`ClientSession` 用**一个可变的 `_stamp` 函数**把这个差异收敛掉：构造函数里初始化为 `_preconnect_stamp`（几乎什么都不做），`initialize()` 成功后换成 `_make_handshake_stamp(version)`，`discover()` 成功后换成 `_make_modern_stamp(...)`。此后每个 `send_request` / `send_notification` 都先调 `self._stamp(data, opts)`——于是「怎么打信封」只有一处决定，调用点完全不必关心时代。

第二处复杂性是**能力广告**。`_build_capabilities(version)` 根据「用户到底给了哪些回调」决定广告什么：`sampling`、`elicitation`、`roots` 三者各自对应一个回调，回调还是默认空实现就**不广告**。这条逻辑的理由很直接：广告一个自己不会处理的能力，等于邀请服务端发来自己答不了（或答错）的请求。

第三处是**版本相关的收窄**。同一个方法名在不同协议版本里可能有不同的结果形状，所以 `send_request` 在拿到原始 dict 之后要做两步：先用 `_methods.validate_server_result(method, version, raw)` 按协商版本校验；再把「更晚版本才有的字段」剔掉。

## 怎么实现的

**构造与生命周期。** 构造可以走两条路：给 `read_stream`/`write_stream`（内部建 `JSONRPCDispatcher`），或给一个现成的 `dispatcher=`。两者同时给会 `ValueError`——注释写的是「pass read_stream/write_stream or dispatcher, not both」。

若传入的是 `JSONRPCDispatcher` 且它自己没有 `on_stream_exception`，构造函数会装上 `self._on_stream_exception`。注释说明了两个顾虑：不要覆盖调用方给的钩子；以及留了一条 TODO 承认这个接缝有内存问题（会话退出后分发器上仍留着一个绑定方法引用，且同一个分发器被二次包装时会跳过安装）。

`__aenter__` 里有个顺序很重要：**先建通知队列，再启动分发器**。注释写「Queues must exist before the dispatcher starts: `_on_notify` enqueues into this dict」——否则第一条通知就会 KeyError。随后 `task_group.start(self._dispatcher.run, self._on_request, self._on_notify, self._intercept_notification)` 把三个回调交给分发器，再为每个通知绑定起一个消费者任务。

`__aenter__` 的异常路径写得比正常路径更仔细：一旦启动过程中抛异常（例如外层的 `move_on_after` 让连接超时），必须**先把已进入的 task group 退出来**再传播。注释的解释是 anyio 的具体错误信息——放弃一个已进入的 group 会导致后续「exited non-innermost cancel scope」。它还额外做了两步：把自己的取消范围 shield 起来（「a new one would break LIFO exit」），并在 `finally` 里关队列。这是这一页里最容易被忽略但最容易在超时场景下炸的一段。

`__aexit__` 的原则是「退出不能阻塞」：先 `cancel_scope.cancel()`，再退出 group，然后在 `finally` 里关绑定队列并把所有 listen 路由结算成 `lost`（用 `CONNECTION_CLOSED` 错误），最后 `resync_tracer()`。

**`send_request` 的五步。** 这是所有强类型方法的公共路径：

1. `request.model_dump(by_alias=True, mode="json", exclude_none=True)` —— 转成线上形状。
2. `self._stamp(data, opts)` —— 打信封。注释强调**顺序**：「The stamp runs first, so its NAME_BEARING_METHODS rows win; a missing name fails loud」。意思是新协议的 stamp 自己会填 `Mcp-Name` 头，而紧随其后的兜底逻辑只在 stamp 没填时才动手。
3. **`Mcp-Name` 兜底**：若请求类的 `name_param` 非空且头里还没有，就从 `params[name_param]` 取出名字；取不到（不是字符串）就 `ValueError`。这保证「需要名字的方法」不会静默缺少这个头。
4. 组装 `CallOptions`：超时（请求级覆盖会话级）、`on_progress`、`resumption_token` 与 `on_resumption_token`。
5. `send_raw_request` → `_clamp_inbound_ttl(raw)` → 按版本校验 → 剔除更晚版本的字段 → 用 `result_type` 校验成强类型对象。

第 5 步里三处细节值得单说：

- **`_clamp_inbound_ttl`**：注释在 `send_discover` 里给了原因——「Un-floored, a negative ttl fails the mode='auto' probe's validation and silently downgrades the handshake」。即服务端返回负数 ttl 会让自动协商静默降级到旧协议，所以这里把它夹到合法范围。
- **版本回落的字面量**：`version = self._negotiated_version or "2025-11-25"`。注释解释为什么写死而不是用 `LATEST_PROTOCOL_VERSION`：这个回落覆盖「握手前」与「无状态」两种情况，其含义是固定的，不随 `LATEST` 升级而变。同样的字面量在 `_on_request` / `_on_notify` 里各出现一次，注释也各写了一遍。
- **剔除更晚版本的字段**：`_later_revision_fields(method, version)` 算出该版本不该有的键名，非空就从原始 dict 里删掉。注释写得很清楚：「they are outside the negotiated contract, and the version-free result type would otherwise apply that revision's constraints to them」——结果类型是不分版本的，若不剔除，旧会话上收到未来版本的字段会被按新版本的约束校验，可能直接失败。

**两条握手的差异。**

`initialize()` 若已有结果直接返回（幂等）。否则发 `InitializeRequest`，参数里的 `protocol_version` 用 `LATEST_HANDSHAKE_VERSION`，能力用 `_build_capabilities(LATEST_HANDSHAKE_VERSION)`——注释点明「The handshake negotiates only legacy versions, where no claim is active」。拿到结果先校验版本在 `HANDSHAKE_PROTOCOL_VERSIONS` 里，否则 `RuntimeError`；然后 `adopt(result)`；最后发 `InitializedNotification()`。**顺序是刻意的**：先 adopt 再发通知，这样通知被打上的是协商后的版本信封。

`discover()` 若已有结果直接返回。否则 `send_discover(LATEST_MODERN_VERSION)`，专门捕获 `UNSUPPORTED_PROTOCOL_VERSION`（-32022）：解析错误体里的 `supported` 列表，与 `MODERN_PROTOCOL_VERSIONS` 求交集，有交集就用最高的那个**重试一次**；没有交集或重试再失败就原样抛出。docstring 明确划清了职责边界：其他错误（包括 `METHOD_NOT_FOUND` 与 `REQUEST_TIMEOUT`）一律传播，「legacy `initialize()` fallback is the caller's policy」——即「探不到新协议就退回旧协议」这个决定不在这里做。

`send_discover` 是更底层的一个方法：不带重试、不 `adopt`，把 `_meta` 信封与 `Mcp-Protocol-Version` 头都按传入的版本打上，让服务端的「时代路由」看到一次自洽的探测。它用 `cancel_on_abandon=False` 且超时 `DISCOVER_TIMEOUT_SECONDS = 10.0`。

**`adopt()` 是状态收敛的唯一入口。** 它接受 `InitializeResult` 或 `DiscoverResult`，做四件事：

- 确定 `version`。`DiscoverResult` 走「按 `MODERN_PROTOCOL_VERSIONS` 顺序（旧→新）取交集的最后一个」，没有交集就 `RuntimeError`。
- 换 `_stamp`。现代分支构造 `_make_modern_stamp(version, client_info, capabilities, self._resolve_param_headers, log_level=...)`。
- **清空对侧槽位**：`initialize_result` 与 `discover_result` 至多一个非 `None`。docstring 点明了这条不变量。
- 重算版本相关状态：`_active_claims`、`_call_tool_adapter`（按活跃 claim 重建 tools/call 的结果解析器），并检查通知绑定是否有「永远不会触发」的情况。

最后那条检查值得看一眼：若某个绑定方法的 `(method, version)` 落在 `SERVER_NOTIFICATIONS` 里，就打一条 warning 说「这个绑定永远不会触发，因为核心协议定义了这个方法」。绑定的设计意图是**只观察核心协议不认识的方法**，所以这条 warning 是在提示作者他的绑定被核心表遮蔽了。

**能力广告的取舍。** `_build_capabilities` 三个分支的判据都是「回调是不是默认实现」：

```
sampling = (self._sampling_capabilities or SamplingCapability()) if self._sampling_callback is not _default_sampling_callback else None
```

注意这里用的是 `is not` 身份比较而不是「有没有传」——因为构造函数已经把 `None` 换成了默认实现，所以「用户是否提供」只能靠身份区分。`roots` 分支还带一句 TODO，承认「该基于我们将要发通知还是仅基于支持」是个未决问题。`extensions` 分支的逻辑是：带 claim 的扩展标识符若在**该版本下所有 claim 都不活跃**就整个丢掉，无 claim 的总是广告——docstring 写的是「the client never advertises result shapes it would reject」。

**入站请求的分派。** `_on_request` 处理服务端发起的请求（`sampling/createMessage`、`elicitation/create`、`roots/list` 等）：

1. `_methods.parse_server_request(method, version, params)`，`KeyError` → `MCPError(METHOD_NOT_FOUND)`。
2. `PingRequest` 特例：直接回 `EmptyResult`，**不构造 `ClientRequestContext`**。注释说「ping has no callback that would need one」。
3. 其余构造 `ClientRequestContext(session=self, request_id=..., meta=request.params.meta)`，交给 `dispatch_input_request`。
4. 回调结果先 `ClientResponse.validate_python(response)`；若是 `ErrorData` 就抛成 `MCPError`（即**回调返回错误数据等于拒绝这次请求**）。
5. 再 `_methods.validate_client_result(method, version, dumped)`：回调返回了不符合该版本的形状时，记 `logger.exception` 后抛 `MCPError(INTERNAL_ERROR, "Client callback returned an invalid result")`——不把内部细节递给服务端。

`dispatch_input_request` 是一个 `match` 分派表，注释说明它被两处共用：旧的服务端→客户端 RPC 路径（`_on_request`），以及 2026-07-28 的多轮往返驱动（`InputRequiredResult.input_requests` 里的内嵌请求）。**复用同一个回调表**是这套设计的关键——否则用户要为同一个能力实现两套回调。

**入站通知的两级路由。** `_on_notify` 先尝试按协商版本解析；`KeyError`（该版本核心表不认识这个方法）才去查 `_notification_bindings`，都没有就打一条 debug 丢掉。

绑定路径上有三处细节：

- 参数校验失败只 warning 后返回（`ValidationError`）。
- 投递用 `send_nowait`，注释写「Must not await: DirectDispatcher calls `_on_notify` inline; blocking deadlocks in-process servers」。这是 in-process 场景的一条硬约束：同一调用栈上不能阻塞等自己。
- 队列满（`WouldBlock`）时**丢掉最老的一条再塞新的**。注释解释为什么这里安全：「no checkpoint since the failed send, so the buffer is still full and the retry cannot block」——`receive_nowait` 之后立刻 `send_nowait` 必然成功，因为中间没有 await 点。

解析成功的通知里，`CancelledNotification` 被**直接吞掉**（docstring 说与 v1 一致：分发器已经应用了它）。其余先按需调 `logging_callback`，再 tee 给 `message_handler`，异常被这一层捕获——注释解释了为什么不能留给分发器：「DirectDispatcher awaits this handler inline in the peer's `notify()` call, so a raising callback would otherwise fail the peer's send」。

**`_intercept_notification` 是一个同步的、运行在接收路径上的旁路。** 它只处理 `subscriptions/listen` 的解复用：按 `_meta` 里的订阅 id 找到路由，把事件投进去；若帧是 `notifications/subscriptions/acknowledged` 则记录 ack 并返回 `True`（消费掉这一帧，不上抛）。docstring 给了为什么必须同步：「Bookkeeping must advance in receive order with the listen result (resolved on this same path); the spawned `_on_notify` path would race it and drop events」。也就是说这一小段不是为了性能，而是为了**顺序正确**。

**传输层异常的最后一段路。** `_on_stream_exception` 不直接调 `message_handler`，而是 `start_soon`——注释说「Running the handler inline would park the dispatcher's read loop and deadlock handlers that await session I/O」。`_deliver_stream_exception` 再捕获 handler 自身抛的异常并记日志。

## 关键取舍

**源码事实：** 信封由 `_stamp` 单一决定，两代协议各装一个实现，调用点不关心时代。构造期是「几乎空」的 `_preconnect_stamp`。

**源码事实：** 握手前与无状态场景用字面量 `"2025-11-25"` 作为版本回落，注释说明刻意不用 `LATEST_PROTOCOL_VERSION`。

**源码事实：** `send_request` 在按协商版本校验之后，会剔除「更晚版本才有的字段」，防止版本无关的结果类型套用未来版本的约束。

**源码事实：** 能力广告按「回调是否为默认实现」判定（身份比较），不广告未实现的能力；带 claim 的扩展在该版本无活跃 claim 时被整个丢掉。

**源码事实：** `initialize()` 先 `adopt` 再发 `initialized` 通知，所以通知带的是协商后的信封。

**源码事实：** `discover()` 只在 `UNSUPPORTED_PROTOCOL_VERSION` 时重试一次，其他错误一律传播；退回旧协议不是这里的职责。

**源码事实：** `adopt()` 保证 `initialize_result` 与 `discover_result` 至多一个非 `None`。

**源码事实：** `PingRequest` 不构造 `ClientRequestContext`；回调返回 `ErrorData` 等于拒绝；回调返回形状不符该版本时抛 `INTERNAL_ERROR` 而不是把细节给服务端。

**源码事实：** 通知绑定的队列满时丢最老一条，且这次替换中间没有 await 点，因此第二次 `send_nowait` 必然成功。

**架构推论：** 「一个可变的 stamp 函数」是把「协议版本演进」这个横切关注点收敛到一处的经典手法。它避免了在每个 API 方法里写 `if version >= ...`，代价是所有信封相关的规则都集中在一个闭包里，读代码时必须跳到 `_make_modern_stamp` 才知道一个请求到底被加了什么。

**架构推论：** 「剔除更晚版本的字段」这条规则揭示了一个真实的不对称：请求可以按版本构造（因为客户端知道自己说什么），但**结果类型无法按版本分叉**（那会要求每个方法都有 N 个结果类）。于是只能在接收侧做一次「事后裁剪」。这是一个务实的妥协，它把版本兼容的成本从类型定义转移到了运行时过滤。

**架构推论：** `_on_notify` 与 `_intercept_notification` 的分工（异步 vs 同步、普通通知 vs listen 解复用）说明了一个判断：**顺序敏感的书必须发生在接收路径上**。这和把日志、统计这类「可以晚一点」的旁路放在 spawned 任务里形成对比——两者的区别不是性能，而是「能不能容忍乱序」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/client/session.py` | `ClientSession` | 客户端会话：握手、信封、请求/通知收发、入站分派 |
| `src/mcp/client/session.py` | `ClientRequestContext` | 服务端请求回调的上下文（会话 + 请求 id + `_meta`） |
| `src/mcp/client/session.py` | `ClientSession.__aenter__` | 建队列 → 启动分发器 → 起绑定消费者，含取消安全的异常路径 |
| `src/mcp/client/session.py` | `ClientSession.send_request` | 五步公共路径：序列化、打戳、名字头兜底、选项、按版本校验与裁剪 |
| `src/mcp/client/session.py` | `ClientSession._build_capabilities` | 按回调身份决定广告哪些能力，按 claim 活跃度过滤扩展 |
| `src/mcp/client/session.py` | `ClientSession.initialize` | 旧协议握手：`initialize` + `initialized`，adopt 在前 |
| `src/mcp/client/session.py` | `ClientSession.discover` | 新协议探测：-32022 时按交集重试一次 |
| `src/mcp/client/session.py` | `ClientSession.adopt` | 协商结果收敛：换 stamp、清对侧槽位、重算 claim |
| `src/mcp/client/session.py` | `_make_handshake_stamp` / `_make_modern_stamp` | 两代协议的信封构造 |
| `src/mcp/client/session.py` | `ClientSession._on_request` | 服务端请求分派、ping 特例、回调结果校验 |
| `src/mcp/client/session.py` | `ClientSession.dispatch_input_request` | 回调表，旧 RPC 与多轮往返共用 |
| `src/mcp/client/session.py` | `ClientSession._on_notify` | 通知解析、绑定回退、丢弃 `notifications/cancelled` |
| `src/mcp/client/session.py` | `_intercept_notification` | 同步的 listen 解复用，保证与结果同序 |
| `src/mcp/client/session.py` | `_later_revision_fields` | 算出当前协商版本不该出现的结果字段 |

## 参考

- [mcp-python-sdk 源码：client/session.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/client/session.py)
- [MCP 规范：生命周期与初始化](https://modelcontextprotocol.io/specification/2025-06-18/basic/lifecycle)
- [MCP 规范：客户端能力](https://modelcontextprotocol.io/specification/2025-06-18/client)
- [anyio 文档：任务组与取消范围](https://anyio.readthedocs.io/en/stable/tasks.html)

## 与主核 / AI 的关系

「握手协商出一份能力清单，之后每个请求按它打信封」这套流程，与主核的**连接启动协商**是同构的：客户端连上来先报自己的协议版本与能力，服务端回一份自己支持的，双方按交集工作；之后的每条命令都带上协商出的版本，因为同一个命令名在不同版本里语义可能不同。差别在主核通常只有「版本号」一个维度，而这里还有「能力」（sampling / elicitation / roots）与「扩展 claim」两个维度。

「剔除更晚版本的字段」这条做法，主核里对应的是**按版本解析报文**：不同协议版本的报文里可能有新字段，旧服务端必须能安全忽略它们而不是报错。两边都是「向前兼容」的实现，但路径不同——主核在解析阶段按版本选择不同的解码逻辑（因为它需要能区分「缺失」与「不理解」），这里是在拿到通用 dict 之后按名字过滤。

`adopt` 里「两个结果槽至多一个非 `None`」这条不变量，值得作为状态机设计的范例记下：当有两代协议各自产生一份「协商结果」时，用一个明确的互斥不变量避免「两份状态不一致」这种最难查的 bug。

对 AI 工程的实际意义在于：MCP 客户端会话最容易被低估的是**回调表的复用**。`dispatch_input_request` 同时服务「服务端发起的 RPC」与「多轮往返里内嵌的请求」，这让用户只需实现一套 `sampling` / `elicitation` / `roots` 回调就能覆盖两种触发方式。另一个值得抄的实践是「回调返回 `ErrorData` 即拒绝」——把「拒绝」表达成一个正常返回值而不是异常，避免用户在该返回错误时被迫抛异常破坏控制流。最后，`__aenter__` 里那段取消安全的回滚是异步 Python 的通用教训：**进了 task group 就必须保证退出**，否则错误会以完全无关的形式在别处出现。
