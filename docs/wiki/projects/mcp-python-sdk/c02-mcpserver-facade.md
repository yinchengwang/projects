---
project: mcp-python-sdk
feature: mcpserver-facade
title: MCPServer 高层门面
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/server/mcpserver/server.py
    symbols: [MCPServer, Settings, lifespan_wrapper]
---

# MCPServer 高层门面

## 一句话职责

`MCPServer` 是给人用的那一层：装饰器登记工具/资源/提示、一个 `run()` 选传输、可选的 OAuth 与扩展；它自己不实现协议语义，而是把三个管理器与一份 `Settings` 组装成一个低层 `Server`，并把每个 `on_*` 处理器接到对应的管理器上。

## 机制是什么

理解这个类的关键是**它只做三件事**：

1. **持有管理器**：`ToolManager`、`ResourceManager`、`PromptManager` 各一个，外加一个 `SubscriptionBus`（默认内存实现）。工具、资源、提示的实际存取与校验全在管理器里，不在门面里。
2. **接线低层 `Server`**：构造期一次性把 `on_list_tools` / `on_call_tool` / `on_read_resource` / `on_get_prompt` 等全部指向自己的 `_handle_*` 私有方法，那些方法再转给管理器。
3. **提供人机接口**：`@server.tool()` 等装饰器、`run(transport=...)`、`streamable_http_app()` / `sse_app()`。

于是「门面」的含义很具体：**协议层看到的是一个普通 `Server`，用户看到的是一个装饰器集合。** 中间那层转接全部写在 `__init__` 与 `_handle_*` 里。

`Settings` 是 `BaseModel`（不是 dataclass），把构造参数里「属于配置而不是行为」的部分收拢：`debug`、`log_level`、三个 `warn_on_duplicate_*`、`dependencies`、`lifespan`、`auth`。它被存成 `self.settings`，且 `dependencies` 还被单独复制到 `self.dependencies`。

`lifespan_wrapper` 是一个四行的适配器：把「收 `MCPServer` 的 lifespan」转成「收 `Server` 的 lifespan」。

## 怎么实现的

**构造期是一段有明确顺序的装配。**

管理器先建（顺序有依赖）：

```
self._tool_manager = ToolManager(tools=tools, warn_on_duplicate_tools=self.settings.warn_on_duplicate_tools)
self._resource_manager = ResourceManager(resources=resources, warn_on_duplicate_resources=...)
self._prompt_manager = PromptManager(warn_on_duplicate_prompts=...)
self._subscriptions = InMemorySubscriptionBus() if subscriptions is None or subscriptions is False else subscriptions
```

`subscriptions` 参数的三态值得单独看：`None` 与 `False` **都**建内存总线，区别在下一步——`False` 时 `on_subscriptions_listen=None`（不服务这个方法）；注释写得很清楚：「`False` 让 `subscriptions/listen` 不被服务；总线仍然留着，好让 `ctx.notify_*` 继续工作，只是没人在听」。也就是说「关掉监听」不等于「关掉发布」，因为发布路径还承载着 `ctx.notify_*` 的语义。

然后建低层服务端并一次性把全部处理器接上：

```
self._lowlevel_server = Server(
    name=name or "mcp-server",
    ...,
    get_tool_input_schema=self._tool_input_schema,
    on_list_tools=self._handle_list_tools,
    on_call_tool=self._handle_call_tool,
    on_list_resources=self._handle_list_resources,
    on_read_resource=self._handle_read_resource,
    on_list_resource_templates=self._handle_list_resource_templates,
    on_list_prompts=self._handle_list_prompts,
    on_get_prompt=self._handle_get_prompt,
    on_subscriptions_listen=None if subscriptions is False else ListenHandler(self._subscriptions),
    lifespan=(lifespan_wrapper(self, self.settings.lifespan) if self.settings.lifespan else default_lifespan),
)
```

`name` 的默认值是字符串 `"mcp-server"`——注意它不是 `None`，因为 `Server` 的 `name` 是必填的。旁边的 `TODO(Marcelo)` 承认 lifespan 的类型在 `MCPServer` 与 `Server` 之间不匹配（需要像 Starlette 那样做一个泛型化的 Lifespan 类型），所以那一行挂着 `# type: ignore`。

**`lifespan_wrapper` 的四行值得完整看。**

```
def lifespan_wrapper(app, lifespan):
    @asynccontextmanager
    async def wrap(_: Server[LifespanResultT]) -> AsyncIterator[LifespanResultT]:
        async with lifespan(app) as context:
            yield context
    return wrap
```

它把「低层传来的 `Server` 实例」丢掉、把闭包里的 `app`（即 `MCPServer`）传进去。原因是用户的 lifespan 写的是 `async def lifespan(server: MCPServer)`，而低层只会给 `Server`。这是一个**单向适配**：从「收 Server」到「收 MCPServer」不可逆，所以反向无法做。形状上和 Starlette 的 `lifespan` 约定一致。

**中间件的插入顺序是显式的。**

```
if request_state_security is None:
    security = RequestStateSecurity.ephemeral()
else:
    if not name and request_state_security.audience is None:
        raise ValueError(_MISSING_AUDIENCE)
    security = request_state_security
self._lowlevel_server.middleware.append(RequestStateBoundary(security, default_audience=self.name))
self._lowlevel_server.middleware.extend(middleware or ())
```

两步：先把自己的请求态边界 `append` 到低层已有的 `[OpenTelemetryMiddleware()]` 之后，再把用户中间件 `extend` 到末尾。注释写明「用户中间件跑在 SDK 内建件之内（先是 OpenTelemetry，然后是请求态边界），按给定顺序、最外层优先」。

`_MISSING_AUDIENCE` 那段错误消息是整个类里最长的一段，它解释了一个真实的安全要求：请求态里封着服务名作为 audience 声明，于是「另一个共享同一批密钥的服务」发出的状态会被拒；**未命名的服务会全部盖同一个占位符**，等于失去这层隔离，所以「外部给了安全策略但服务没名字」时必须报错。

`RequestStateSecurity.ephemeral()` 作为默认值则表达另一条：不显式配置时用进程内临时密钥，安全但不可跨进程复用。

**认证配置的校验是四路互斥。**

```
if self.settings.auth is not None:
    if auth_server_provider and token_verifier:  raise ValueError("Cannot specify both auth_server_provider and token_verifier")
    if not auth_server_provider and not token_verifier: raise ValueError("Must specify either auth_server_provider or token_verifier when auth is enabled")
elif auth_server_provider or token_verifier:
    raise ValueError("Cannot specify auth_server_provider or token_verifier without auth settings")
```

「两者都给」与「两者都不给」都报错，外加「有 provider/verifier 却没有 auth settings」也报错。再往下一段是向后兼容的桥接：

```
if auth_server_provider and not token_verifier:
    self._token_verifier = ProviderTokenVerifier(auth_server_provider)
```

即「只给了授权服务器 provider」时自动包一个 `ProviderTokenVerifier`，注释标着 backwards compatibility。

**扩展的应用是「加法，不许替换」。**

`_apply_extension` 的三段各有约束：

```
for tool in extension.tools():     self.add_tool(tool.fn, meta=tool.meta, **tool.kwargs)
for resource in extension.resources(): self.add_resource(resource.resource)
for method in extension.methods():
    if self._lowlevel_server.get_request_handler(method.method) is not None:
        raise ValueError(f"Extension {identifier!r} binds method {method.method!r}, which is already "
                         "registered; extension methods are additive and cannot replace another handler")
    handler = _version_gated(method) if method.protocol_versions is not None else method.handler
    self._lowlevel_server.add_request_handler(method.method, method.params_type, handler)
self._lowlevel_server.extensions[extension.identifier] = extension.settings()
```

工具与资源是直接加进管理器（重复由管理器的 `warn_on_duplicate_*` 处理）；而**方法**是要检查冲突并报错的，因为 `add_request_handler` 的语义是「替换」，扩展不能借它悄悄覆盖已有处理器。报错文案直说「扩展方法是加法，不能替换另一个处理器」。

注意 `_version_gated(method)` 这一分支：扩展方法可以声明 `protocol_versions`，此时处理器被包一层版本闸门。这是把「扩展只在某些协议版本可用」交给门面处理，而不是让扩展自己判断。

`self._extensions.append(extension)` 之前还有两道检查：`validate_extension_identifier(identifier, owner=type(extension).__name__)`（标识符合法性，`owner` 参数让错误消息能指明是哪个类），以及「同 identifier 已注册」的重复检查。

`_install_extension_interceptor` 之后被调用一次，且带条件：

```
if any(type(e).intercept_tool_call is not Extension.intercept_tool_call for e in self._extensions):
    self._lowlevel_server.add_request_handler("tools/call", CallToolRequestParams,
                                              compose_tool_call_handler(self._extensions, self._handle_call_tool))
```

条件写的是「类型上的方法**不是**基类的那个方法」，即只有真的重写了 `intercept_tool_call` 的扩展才触发重装。docstring 说明了两点：纯加法的扩展保持裸处理器；拦截链包在**处理器本身**上、位于 runner 的出站封装之下，因此「短路拦截器返回的结果会被与处理器结果完全一样地筛选与盖戳」。

**`_handle_call_tool` 是错误语义最讲究的一处。**

```
context = Context(request_context=ctx, mcp_server=self, input_params=params, subscriptions=self._subscriptions)
try:
    return await self.call_tool(params.name, params.arguments or {}, context)
except MCPError:
    raise
except Exception as exc:
    if isinstance(exc, ToolError) and not isinstance(exc, UnexpectedToolError):
        if isinstance(exc.__cause__, ValidationError):
            fields = sorted({".".join(str(part) for part in err["loc"]) for err in exc.__cause__.errors()})
            logger.info("Tool %r rejected arguments: %r", params.name, fields)
        else:
            logger.info("Tool %r failed: %r", params.name, str(exc))
    else:
        logger.exception("Tool %r raised an unexpected exception", params.name)
    return CallToolResult(content=[TextContent(type="text", text=str(exc))], is_error=True)
```

三档处理：

- `MCPError` 原样抛（走协议错误路径）；
- 「预期内的工具错误」（`ToolError` 但不是 `UnexpectedToolError`）记 **info** 级日志，且若是参数校验失败，只记**字段名**——注释：「只记字段名，被拒的值是调用方的数据」；
- 其他异常记 `logger.exception`（带堆栈），然后**统一**转成 `is_error=True` 的 `CallToolResult`。

也就是说：工具的错误**不是** JSON-RPC 错误，而是「成功的应答里带一个错误标志」——这符合 MCP 工具语义（模型要看到错误内容才能自我修正）。而日志级别的区分（info vs exception）正是「预期内 vs 意外」的分界。

`_handle_read_resource` 的对应逻辑不同：资源错误**转成 `MCPError`** 抛出去，且分级更细：

```
code = INVALID_PARAMS if isinstance(err, ResourceNotFoundError) else INTERNAL_ERROR
raise MCPError(code=code, message=str(err), data={"uri": str(params.uri)})
```

「找不到资源」是 `INVALID_PARAMS`（客户端给错了 URI），其他资源错误是 `INTERNAL_ERROR`。`data` 里带 `uri` 便于排查。这与工具路径相反——资源读失败是协议级失败，因为没有「模型该看到错误内容」这个需求。

**`run()` 是同步门面，三个传输各走一条异步实现。**

```
def run(self, transport: Literal["stdio", "sse", "streamable-http"] = "stdio", **kwargs: Any) -> None:
    ...
    match transport:
        case "stdio":            anyio.run(self.run_stdio_async)
        case "sse":              anyio.run(lambda: self.run_sse_async(**kwargs))
        case "streamable-http":  anyio.run(lambda: self.run_streamable_http_async(**kwargs))
```

docstring 特意点明「注意这是同步函数」。三条异步实现里 `stdio` 最短：

```
async with stdio_server() as (read_stream, write_stream):
    await self._lowlevel_server.run(read_stream, write_stream, self._lowlevel_server.create_initialization_options())
```

两条 HTTP 的则是 `import uvicorn` 在函数体内（`# pragma: no cover`），构造 `uvicorn.Config` 时把 `log_level` 小写传给 it。

`sse_app` 与 `streamable_http_app` 的分工不同：`streamable_http_app` 几乎只是转发到低层的同名方法，但额外把自己持有的认证三件套（`auth` / `token_verifier` / `auth_server_provider`）、自定义路由与 `debug` 一起传下去；`sse_app` 则在门面里自己搭 Starlette（含同样的「回环地址自动开 DNS 重绑定保护」逻辑）。

**`middleware` 属性暴露的是低层那同一个列表。**

```
@property
def middleware(self) -> list[ServerMiddleware[Any]]:
    """The same list as the low-level `Server.middleware`: append an `async (ctx, call_next)` callable..."""
    return self._lowlevel_server.middleware
```

docstring 明确「同一个列表」，所以 `server.middleware.append(...)` 在门面上是生效的，不需要 setter。同时它还诚实标了「Provision - 签名可能在 2.x 小版本改变」。

其余属性（`name` / `title` / `description` / `instructions` / `website_url` / `icons` / `version` / `session_manager`）全是向低层转发，没有本地副本。这是「门面不复制状态」的贯彻。

**`tool()` 装饰器有一处贴心的防御。**

```
if callable(name):
    raise TypeError("The @tool decorator was used incorrectly. Did you forget to call it? Use @tool() instead of @tool")
```

`name` 参数本应是 `str | None`，但用户极易写成 `@server.tool`（不加括号），此时 `name` 收到的是函数对象。这里显式检测并给出正确写法，而不是让它变成一个名字诡异的工具。`remove_tool` 的 docstring 也写明「工具不存在时抛 `ToolError`」——与「删不到就静默成功」相反。

**`read_resource` 里有一处运行期类型校验。**

```
content = cast(object, await resource.read())
if not isinstance(content, str | bytes):
    raise TypeError(f"Resource.read() must return str or bytes, not {type(content).__name__}")
```

注释说明为什么用运行期检查而不是靠类型注解：「`Resource` 子类可能不遵守注解」。`cast(object, ...)` 是为了让类型检查器允许之后做 `isinstance`——这是一个纯粹为了让运行期检查成立而写的 cast。

## 关键取舍

**源码事实：** `MCPServer` 不实现协议语义，只持有三个管理器 + 一个订阅总线，并在构造期把低层 `Server` 的全部 `on_*` 指向自己的 `_handle_*`。

**源码事实：** `Settings` 是 `BaseModel` 且被存成 `self.settings`，其余属性一律转发到低层，门面不留状态副本（`middleware` 也返回同一个列表对象）。

**源码事实：** `subscriptions=False` 只关掉「服务 `subscriptions/listen`」，内存总线仍然建立，以便 `ctx.notify_*` 继续工作。

**源码事实：** `lifespan_wrapper` 是单向适配（收 `Server` → 用闭包里的 `MCPServer`），因此无法反向。

**源码事实：** 中间件顺序是「OTel → 请求态边界 → 用户中间件」，最外层优先；构建期的 `_MISSING_AUDIENCE` 检查防止未命名服务在共享密钥场景下失去 audience 隔离。

**源码事实：** 扩展的方法绑定冲突时抛 `ValueError`，而工具与资源只是交给管理器（重复由 `warn_on_duplicate_*` 处理）——因为 `add_request_handler` 的语义是替换，必须拦住。

**源码事实：** 工具的异常被转成 `is_error=True` 的 `CallToolResult`（协议层成功），资源的异常被转成 `MCPError`（协议层失败），且「资源找不到」用 `INVALID_PARAMS` 而非 `INTERNAL_ERROR`。

**源码事实：** 工具参数校验失败时只记录**字段名**，不记录被拒的值（那是调用方的数据）。

**源码事实：** `@server.tool` 不加括号会抛 `TypeError` 并给出正确写法。

**架构推论：** 门面把「装饰器风格的人机接口」与「字典风格的低层注册表」之间的落差全放在构造期与 `_handle_*` 里，代价是构造期很长（管理器 + 低层服务端 + 中间件 + 认证 + 扩展），收益是运行期没有任何额外间接层——低层看到的处理器就是普通异步函数。

**架构推论：** 工具错误走「成功应答 + `is_error`」、资源错误走协议错误，这条不对称来自两种能力的消费者不同：工具的结果要给模型读（于是错误内容必须可读），资源的结果要给客户端程序用（于是失败就是失败）。把它做进门面而不是让用户自己决定，避免了同类服务端行为不一致。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/server/mcpserver/server.py` | `MCPServer` | 门面：装饰器接口 + 三个管理器 + 低层接线 |
| `src/mcp/server/mcpserver/server.py` | `Settings` | 配置项收拢：debug、日志级别、重复告警、依赖、lifespan、auth |
| `src/mcp/server/mcpserver/server.py` | `lifespan_wrapper` | 把「收 MCPServer 的 lifespan」适配成「收 Server 的 lifespan」 |
| `src/mcp/server/mcpserver/server.py` | `MCPServer._handle_call_tool` | 工具异常三档处置，错误以 `is_error` 结果返回 |
| `src/mcp/server/mcpserver/server.py` | `MCPServer._handle_read_resource` | 资源错误转协议错误，找不到资源用 INVALID_PARAMS |
| `src/mcp/server/mcpserver/server.py` | `MCPServer._apply_extension` | 扩展贡献工具/资源/方法，方法冲突即报错 |
| `src/mcp/server/mcpserver/server.py` | `MCPServer.run` | 同步门面，按传输分派到三个异步实现 |
| `src/mcp/server/mcpserver/server.py` | `MCPServer.streamable_http_app` | 转发低层并附上认证三件套与自定义路由 |

## 参考

- [mcp-python-sdk 源码：server/mcpserver/server.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/mcpserver/server.py)
- [MCP Python SDK 文档：MCPServer 快速上手](https://py.sdk.modelcontextprotocol.io/)
- [MCP 规范：工具](https://modelcontextprotocol.io/specification/2025-06-18/server/tools)
- [MCP 规范：资源](https://modelcontextprotocol.io/specification/2025-06-18/server/resources)

## 与主核 / AI 的关系

「门面 + 管理器 + 低层注册表」三层，与主核里「SQL 界面 + 目录/存储管理器 + 执行引擎」是同一个骨架。主核的门面（解析器与执行器入口）也很少持有状态，真正的状态在目录与缓冲池里；这里同样如此：`MCPServer` 只有一个 `Settings` 和四个管理器引用，全部身份属性都转发给低层。

对 AI 工程的实际意义集中在两处错误语义。第一，**工具错误不该是协议错误**。工具返回 `is_error=True` 的结果而不是抛异常，是因为工具的消费者是模型——它需要「看到」错误文本才能自我修正；如果抛成协议错误，模型什么也看不到。这个判断在 AI 工具框架里经常被做反。

第二，**资源错误分级**。`ResourceNotFoundError` 映射到 `INVALID_PARAMS` 而不是 `INTERNAL_ERROR`，因为它几乎总是客户端给了不存在的 URI。把一个「客户端错误」报成「服务端内部错误」会误导排查方向，也会污染监控指标里的错误率。这种「按原因归因而不是按发生位置归因」的分级，是接口层最容易被忽略却影响长期可维护性的细节。
