---
project: mcp-python-sdk
feature: lowlevel-server
title: 低层服务端与处理器登记
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/server/lowlevel/server.py
    symbols: [Server, HandlerEntry, NotificationOptions]
---

# 低层服务端与处理器登记

## 一句话职责

`Server` 是**注册表本身**：它持有「方法名 → 处理器」的两张字典、一份服务身份（名字/版本/标题/描述/图标）与一个中间件列表，并负责把「已注册的处理器」反推成能力声明；它不跑循环、不管传输、不做协商——那些在 `ServerRunner` 与各个 driver 里。

## 机制是什么

`Server` 存在感最强的地方是它的构造函数：一个巨长的重载列表，几乎每个 MCP 规范方法都对应一个 `on_*` 关键字参数（`on_list_tools`、`on_call_tool`、`on_read_resource`、`on_subscriptions_listen`……）。这些参数**不是存成字段**，而是在构造期被摊成一张表：

```
_spec_requests = [
    ("ping", types.RequestParams, on_ping),
    ("server/discover", types.RequestParams, self._handle_discover),
    ("prompts/list", types.PaginatedRequestParams, on_list_prompts),
    ...
]
self._request_handlers.update({m: HandlerEntry(pt, h) for m, pt, h in _spec_requests if h is not None})
```

因此 `Server` 的核心状态只有两个字典：`_request_handlers` 与 `_notification_handlers`，值是 `HandlerEntry`。所有能力推导、查找、替换都基于这两张表。

`HandlerEntry` 是 `@dataclass(frozen=True, slots=True)` 的两字段记录：`params_type`（校验入参的模型）与 `handler`。它的 docstring 交代了一处**有意的类型擦除**：处理器的第二参数在存储里被抹成 `Any`，因为每个条目的具体参数类型都不同，而 `Callable` 的参数是逆变的——精确类型可通过 `params_type` 还原，两者的相关性由注册时的 `add_request_handler` 保证。

`NotificationOptions` 则是另一种东西：三个布尔（`prompts_changed` / `resources_changed` / `tools_changed`），用来表达「我这个服务端会不会发这三种 list_changed 通知」。它必须由调用方**显式传**，因为「有没有注册 `notifications/prompts/list_changed` 的处理器」推不出「我会不会发它」。

## 怎么实现的

**注册即替换，但 `initialize` 被保留。**

`add_request_handler` 的实现只有两行有效代码，前面挡了一个特例：

```
if method == "initialize":
    raise ValueError("'initialize' is handled by the server runner and cannot be overridden; "
                     "use Server.middleware to observe or wrap initialization")
self._request_handlers[method] = HandlerEntry(params_type, handler)
```

`ValueError` 的文案不只是拒绝，还给出了替代路径（用中间件观察/包裹初始化）。这是因为握手归 `ServerRunner` 所有，允许覆盖会让「协商结果」与「握手行为」两套事实分离。

docstring 里还钉了三条参数契约：

- `params_type` 应当继承 `RequestParams`，这样 `_meta` 能统一解析；
- **没有 `params` 成员的消息会拿 `{}` 去校验** `params_type`：有必填字段的模型会以 `INVALID_PARAMS` 拒绝，全可选的模型则以默认值到达处理器——**处理器永远不会收到 `None`**；
- 同方法重复注册是替换（`initialize` 除外）。

`add_notification_handler` 同样，另加一条语义：「`notifications/initialized` 的处理器在 runner 标记连接已初始化**之后**运行」。

**`get_capabilities` 是「从已注册处理器反推声明」的集中体现。**

五个能力块各自只由一个条件触发：

```
if "prompts/list" in self._request_handlers:   prompts_capability = types.PromptsCapability(list_changed=prompts_changed)
if "resources/list" in self._request_handlers: resources_capability = types.ResourcesCapability(subscribe=subscribe, list_changed=resources_changed)
if "tools/list" in self._request_handlers:     tools_capability = types.ToolsCapability(list_changed=tools_changed)
if "logging/setLevel" in self._request_handlers: logging_capability = types.LoggingCapability()
if "completion/complete" in self._request_handlers: completions_capability = types.CompletionsCapability()
```

注意 `logging` 与 `completion` 没有布尔字段——它们的能力声明就是一个「存在性标记」，所以注册了处理器就等于声明了能力。

更有意思的是 `protocol_version` 参数如何改变推导。docstring 说它「让订阅相关的位与协议年代一致」：

```
if protocol_version in MODERN_PROTOCOL_VERSIONS:
    listen_served = "subscriptions/listen" in self._request_handlers
    prompts_changed = tools_changed = resources_changed = subscribe = listen_served
else:
    prompts_changed = notification_options.prompts_changed
    tools_changed = notification_options.tools_changed
    resources_changed = notification_options.resources_changed
    subscribe = "resources/subscribe" in self._request_handlers
```

在 2026-07-28 及以后，变更通知只能通过 `subscriptions/listen` 流送达，所以三个 `list_changed` 位与 `resources.subscribe` 全部由「是否服务 `subscriptions/listen`」决定，**`notification_options` 与旧的 `resources/subscribe` 处理器被完全忽略**（因为现代线缆根本分发不了它们）。这是一个「同一个能力声明在两种协议年代下语义不同」的精确处理：不是加分支判断，而是换一套推导源。

`subscriptions/listen` 在现代线缆里也顺带承载了资源的「订阅」语义——所以 `subscribe` 与 `list_changed` 用的是同一个布尔。

**`create_initialization_options` 是把 `Server` 的身份与能力打包成握手应答。**

```
return InitializationOptions(
    server_name=self.name, server_version=self.version,
    title=self.title, description=self.description,
    capabilities=self.get_capabilities(notification_options or NotificationOptions(), experimental_capabilities, extensions),
    instructions=self.instructions, website_url=self.website_url, icons=self.icons,
)
```

它在 `run()` 之外还有第二处调用（`MCPServer` 用它，或者 `Server.run` 由调用方手工传）。方法名里的 `create_` 前缀是诚实的：每次调用都重新算一遍能力。

**`server_info` 与 `server_info_stamp` 是「算一次、深拷贝发出去」的一对。**

```
@property
def server_info(self) -> types.Implementation: ...   # 每次新建

@cached_property
def _server_info_stamp_source(self) -> dict[str, Any]:
    return self.server_info.model_dump(by_alias=True, mode="json", exclude_none=True)

@property
def server_info_stamp(self) -> dict[str, Any]:
    return copy.deepcopy(self._server_info_stamp_source)
```

`@cached_property` 上的注释说明为什么可以缓存：「身份在构造时就固定了，所以每个服务端只算一次 dump，而不是每个请求算一次」。而 `server_info_stamp` 的 docstring 说明为什么还要拷贝：「每次访问都物化那份一次性的 dump 的深拷贝，于是调用方修改被盖戳的响应，永远不会污染盖到后续响应里的身份」。这两句合起来是一条完整的所有权约定：**缓存是为了省，深拷贝是为了不共享**。

`server_info` 自己的 docstring 也有一句值得记的：「未声明版本的服务端报告空 `version`；SDK 永不替换成自己的版本」。即「空版本」是合法且被保留的状态，不被默认值悄悄填空。

**默认的 `server/discover` 处理器是从当前状态现算的。**

```
async def _handle_discover(self, ctx, params) -> types.DiscoverResult:
    return types.DiscoverResult(
        supported_versions=list(MODERN_PROTOCOL_VERSIONS),
        capabilities=self.get_capabilities(protocol_version=ctx.protocol_version),
        instructions=self.instructions,
    )
```

docstring 说得很清楚：「调用时从服务端状态自动推导，于是能力反映**当时**注册了什么（构造函数的 `on_*` 与后来 `add_request_handler` 都算）。运维可以通过 `add_request_handler("server/discover", ...)` 整体替换」。它还补了一句范围限定：「对老对端的可达性由边界（`types.methods`）决定，不在这里」。

这处设计值得单独表扬：一个默认处理器**保留了被替换的能力**，而不是把一个固定值塞进去。于是「先注册工具、再处理 discover」与「先处理 discover、再注册工具」结果都对。

**中间件列表默认带一个 OTel 中间件。**

```
self.middleware: list[ServerMiddleware[LifespanResultT]] = [OpenTelemetryMiddleware()]
```

注释给了三点：中间件是「上下文层」的，包裹每条入站请求（含 `initialize`、查找、校验、处理器）；`OpenTelemetryMiddleware` 默认开启，于是每个服务端每条消息都产出一个 SERVER span；**没有安装 OTel 导出器时它是空操作**，想去掉就从这个列表里删掉。旁边挂着 `TODO(L54)` 承认签名与语义在 2.x 小版本里可能改。

**`lifespan` 的默认实现什么都不做，但签名是 `Server[Any]`。**

```
@asynccontextmanager
async def lifespan(_: Server[Any]) -> AsyncIterator[dict[str, Any]]:
    yield {}
```

参数名是下划线——默认实现不用服务端。返回 `{}` 而非 `None`，于是 `ServerRequestContext.lifespan_context` 永远是个可下标的对象。

**`streamable_http_app()` 里有一处「本地自动加固」。**

```
if transport_security is None and host in ("127.0.0.1", "localhost", "::1"):
    transport_security = TransportSecuritySettings(
        enable_dns_rebinding_protection=True,
        allowed_hosts=["127.0.0.1:*", "localhost:*", "[::1]:*"],
        allowed_origins=["http://127.0.0.1:*", "http://localhost:*", "http://[::1]:*"],
    )
```

当宿主是回环地址且调用方没给安全设置时，自动开启 DNS 重绑定保护并把允许的 host / origin 收窄到回环。IPv4、IPv6 字面量与 `localhost` 三种写法都列了（IPv6 带方括号）。

**`run()` 只是一层薄包装。**

```
async with self.lifespan(self) as lifespan_context:
    await serve_dual_era_loop(self, read_stream, write_stream, lifespan_state=lifespan_context,
                              init_options=initialization_options, raise_exceptions=raise_exceptions)
```

docstring 交代了边界：「薄包装 `serve_dual_era_loop`：进入服务端 lifespan，然后驱动循环，同时服务旧握手年代与现代逐请求信封年代（客户端第一个请求决定是哪个）。**自带 lifespan 归属者的传输（streamable-HTTP 管理器）直接调 `serve_loop`**」。

也就是说 `Server.run` 不是唯一入口，它只是「没人替我管 lifespan」时的方便入口。`raise_exceptions` 的注释也留了历史说明：「False 时异常作为消息返回给客户端；True 时抛出，服务端会关掉，但让追踪更清楚——测试与进程内服务端用」。

**构造期的参数宽严是有层次的。**

`version` 默认空串（不是 `None`），`name` 是必填的位置参数。`cache_hints` 走 `validate_cache_hints`，注释说明「处理器返回之后由 `ServerRunner` 应用；处理器显式设置过的字段优先」。`get_tool_input_schema` 的 docstring 更细：「设了之后，2026-07-28 的 Streamable HTTP 路径上 `Mcp-Param-*` 头校验会调它，而不是去跑 `tools/list` 处理器；它在中间件之前被调用，所以不绑定调用者；如果它抛错，则记日志并让请求以未校验方式通过」。

最后这条是典型的「校验器失败不能比它所校验的东西更危险」：一个用来校验的钩子若抛错，选择**放行并记录**，而不是拒绝请求。

**三个已废弃参数的告警在构造函数里发出，而不是用装饰器。**

```
if on_set_logging_level is not None: warnings.warn("The logging capability is deprecated as of 2026-07-28 (SEP-2577).", ...)
if on_roots_list_changed is not None: warnings.warn("The roots capability is deprecated as of 2026-07-28 (SEP-2577).", ...)
if on_progress is not None: warnings.warn("Client-to-server progress is deprecated as of 2026-07-28.", ...)
```

用 `warnings.warn(..., stacklevel=2)` 而非 `@deprecated` 装饰器，因为要废弃的是**个别关键字参数**而不是整个函数。`stacklevel=2` 让告警指向调用方。

## 关键取舍

**源码事实：** `Server` 的核心状态只有 `_request_handlers` / `_notification_handlers` 两个字典，构造函数里那串 `on_*` 参数在构造期就被摊平成表，不保留为字段。

**源码事实：** `HandlerEntry` 故意把处理器的第二参数类型擦成 `Any`（因 `Callable` 参数逆变），准确性由 `params_type` 承担并由注册函数保证。

**源码事实：** `initialize` 不可注册，`add_request_handler` 直接抛 `ValueError` 并给出替代路径（用中间件）。

**源码事实：** 缺 `params` 的消息用 `{}` 校验，处理器永不收到 `None`；有必填字段的模型会以 `INVALID_PARAMS` 拒绝。

**源码事实：** 能力声明是从「方法是否注册」反推的，而 `notification_options` 必须由调用方显式传入——它推不出来。

**源码事实：** 在 2026-07-28+ 上，三个 `list_changed` 位与 `resources.subscribe` 全部改由「是否服务 `subscriptions/listen`」决定，`notification_options` 与旧 `subscribe` 处理器被忽略。

**源码事实：** 默认 `server/discover` 处理器在调用时现算能力，因此后注册的方法也会被反映；它同时可被整体替换。

**源码事实：** `server_info_stamp` 用 `@cached_property` 缓存一次 dump、再用 `deepcopy` 发给每个调用方。缓存省算，拷贝防共享。

**源码事实：** `streamable_http_app()` 在宿主为回环地址且未给安全设置时，自动开启 DNS 重绑定保护。

**架构推论：** 把「注册表」与「运行器」彻底分开（`Server` 只有表，`ServerRunner` 才有执行逻辑），让同一份注册表能被多种驱动复用：`Server.run`、streamable-HTTP 管理器、双年代循环各自走不同的 driver，但共享同一套处理器与能力推导。

**架构推论：** 「能力从注册反推」的好处是声明与实现不会漂移——不存在「声明了 tools 但没注册 `tools/list`」这种状态。代价是 `list_changed` 这类**行为**（会不会主动发通知）无法推出，只能靠 `NotificationOptions` 手工补，而它正是那个 `TODO(L53)` 提到的不一致来源。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/server/lowlevel/server.py` | `Server` | 处理器注册表 + 服务身份 + 能力推导，不含执行 |
| `src/mcp/server/lowlevel/server.py` | `HandlerEntry` | 冻结的两字段记录，承担有意的参数类型擦除 |
| `src/mcp/server/lowlevel/server.py` | `NotificationOptions` | 三个 list_changed 意图位，必须显式传入 |
| `src/mcp/server/lowlevel/server.py` | `Server.add_request_handler` | 注册即替换，`initialize` 保留并给出替代路径 |
| `src/mcp/server/lowlevel/server.py` | `Server.get_capabilities` | 从已注册方法反推能力，按协议年代切换推导源 |
| `src/mcp/server/lowlevel/server.py` | `Server._handle_discover` | 默认 discover 处理器：现算能力，可被替换 |
| `src/mcp/server/lowlevel/server.py` | `Server.server_info_stamp` | 缓存一次 dump、每调用方深拷贝 |
| `src/mcp/server/lowlevel/server.py` | `Server.run` | 进 lifespan 后交给 `serve_dual_era_loop` 的薄包装 |
| `src/mcp/server/lowlevel/server.py` | `Server.streamable_http_app` | 回环宿主自动开启 DNS 重绑定保护 |

## 参考

- [mcp-python-sdk 源码：server/lowlevel/server.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/lowlevel/server.py)
- [MCP 规范：服务端能力](https://modelcontextprotocol.io/specification/2025-06-18/server)
- [MCP 规范：生命周期与初始化](https://modelcontextprotocol.io/specification/2025-06-18/basic/lifecycle)
- [MCP Python SDK 文档：低层服务端](https://py.sdk.modelcontextprotocol.io/)

## 与主核 / AI 的关系

「注册表 + 运行器」的分离与主核里「系统目录 + 执行引擎」的分层互为镜像：目录记录「有什么」，引擎负责「怎么跑」，两者通过稳定的名字空间（这里是方法名，主核里是表名/函数名）耦合。主核的目录通常是持久化的，这里的表是内存里的字典，但职责相同——**声明与执行分开，才能让同一份声明被多种执行路径复用**。

对 AI 工程的实际意义有两条。第一是那处「默认 `server/discover` 处理器现算能力」。默认实现很容易写成「构造时拍一份快照塞进去」，那样一来「运行期新增处理器」就对客户端不可见了。这里让默认处理器读当前状态，于是「注册顺序」不再是隐含约束——**默认实现应该读状态而不是缓存状态**，除非确知状态不再变化。

第二是 `get_capabilities` 在两种协议年代换推导源的做法。它不是加 `if` 打补丁，而是整段换掉「哪些输入被采信」。这种处理方式比在原有推导上叠加例外要清晰得多，也解释了为什么同一个字段（`resources.subscribe`）在两个年代下含义不同却不需要改数据结构：**语义的差异被局限在一个函数的两个分支里**。
