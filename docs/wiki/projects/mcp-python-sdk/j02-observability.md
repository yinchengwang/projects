---
project: mcp-python-sdk
feature: observability
title: 链路追踪与日志级别
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/server/_otel.py
    symbols: [_otel]
  - path: src/mcp/server/connection.py
    symbols: [allowed_log_levels]
---

# 链路追踪与日志级别

## 一句话职责

给每条入站请求包一个 OTel `SERVER` span（并尝试从 `_meta` 里接上调用方的 W3C 追迹），同时把「这条请求允许发出哪些级别的日志」算成一个明确的集合——两者都在版本时代上有硬切换点。

## 机制是什么

两件互不相关但都被「按协议版本行事」这条线索串起来的事：

- **追踪**：`server/_otel.py` 里的 `OpenTelemetryMiddleware` 是一个 `ServerMiddleware`，包住每条入站请求。它用 `shared/_otel.py` 的两个助手：`otel_span`（开 span）与 `extract_trace_context`（从 `_meta` 解出父上下文）。
- **日志级别**：`server/connection.py` 里的 `allowed_log_levels(protocol_version, meta)` 返回「这条请求可发出的 `notifications/message` 级别集合」。

前者的关键点是 span 的**命名与属性按方法名分流**、错误的**三种分类**；后者的关键点是「2026 线格式把日志变成逐请求 opt-in」。

## 怎么实现的

**追踪中间件默认就装着。** `Server.__init__` 的注释写明：`OpenTelemetryMiddleware` **默认随服务器一起启用**，于是每个服务端对每条消息都发一个 `SERVER` span；**在没装 exporter 之前它是 no-op**。要关掉就从 `middleware` 列表里把它移除。这条默认值的选择值得注意——它意味着「可观测」被当成默认行为而不是可选插件，代价是每条消息多一层 async 包装（无 exporter 时几乎无成本）。

**span 的命名把方法名与目标拼在一起。** `name=f"{ctx.method}{f' {target}' if target else ''}"`，其中 `target` 取自 `ctx.params.get('name')`（**只接受字符串**）。于是 `tools/call` 的 span 名会带上工具名，`prompts/get` 带上 prompt 名。这让追踪列表在筛选前就可读。

**属性按方法分三档。** 公共的两个：`mcp.method.name` 与 `mcp.protocol.version`；有 `request_id` 时再加 `jsonrpc.request.id`（转成字符串）。然后是方法特化：`tools/call` 加 `gen_ai.operation.name = "execute_tool"` 与 `gen_ai.tool.name`；`prompts/get` 加 `gen_ai.prompt.name`。这里同时用了 `mcp.*`（MCP 自己的命名空间）与 `gen_ai.*`（OpenTelemetry 语义约定），让同一份数据既能被 MCP 专用看板查，也能被通用 GenAI 看板查。

**父上下文提取有三个「返回 `None`」的条件，理由写得很清楚。** `extract_trace_context(meta)`：

- `meta` 为空 → `None`；
- `extract(meta)` 抛 `ValueError` / `TypeError` → `None`；
- 解出的上下文里当前 span 的 span context **不合法** → `None`。

docstring 解释了第三条为何必须是 `None` 而不是空 `Context`：**返回一个显式的空上下文会把 span 变成孤儿，而不是嵌套进当前 span**。也就是说「没有有效父节点」与「父节点是空的」在 OTel 语义里不是一回事，返回错的那个会让追踪断开。

**入站追踪上下文靠 `_meta` 传播。** `extract_trace_context(ctx.meta)` 说明 MCP 把 W3C 的 `traceparent`/`tracestate` 放在请求的 `_meta` 里。反方向 `inject_trace_context(meta)` 在 `jsonrpc_dispatcher` 里被调用（出站消息时把当前上下文注入 `_meta`）。这两个助手都在 `shared/_otel.py`，即客户端与服务端共用——追踪能跨两侧连起来的机制就在这里。

**错误处理分三类，每类的属性策略不同。** `OpenTelemetryMiddleware` 捕获顺序是：

| 异常 | 记录的属性 | span 状态消息 |
|---|---|---|
| `MCPError` | `error.type` 与 `rpc.response.status_code` 都设为协议错误码（字符串） | 用 `e.error.message` |
| `ValidationError` | 同上，码固定为 `INVALID_PARAMS` | 固定文案「Invalid request parameters」 |
| 其他 `Exception` | `error.type` 设为 `type(e).__qualname__` | 用 `str(e)`，并且 `record_exception(e)` |

`ValidationError` 那一支的注释说明了原因：**镜像线上那条脱敏后的响应，因为 pydantic 的错误消息里带着客户端输入**。这是本页最关键的一条安全细节——如果把 pydantic 的原始消息写进 span，客户端可控的输入就会进入追踪后端，而线上响应恰恰是为了避免这一点才脱敏的。

`record_exception=False` / `set_status_on_exception=False` 在 `otel_span` 调用处显式传入，于是异常记录完全由中间件自己控制：只有「非预期异常」才调 `record_exception`，前两类只设属性与状态。这让 span 上的异常事件只出现真正的意外，而不是把每次参数校验失败都记成异常事件。

**工具错误需要额外判定，因为它不是异常。** `tools/call` 之后有一段 `match`：若结果是 `CallToolResult(is_error=True)` 或原始 dict `{"isError": True}`，就把 `error.type` 设为 `"tool_error"` 并 `set_status(ERROR)`。注释把这段判定的边界讲得很细：

- **工具错误在序列化之前就被检测**，所以只算「会以错误形态到达线上的形状」；
- 模型形态走 `CallToolResult`，**原始 dict 形态走 camelCase 别名 `isError`**——因为别名式线校验会丢掉 `is_error`；
- dict 里的 `isError` **只按字面布尔匹配**：`1` 或 `"true"` 这类可强制转换的值会被序列化成错误，但作者判断这种情况罕见，因此不检测。

最后这句是一个诚实的取舍说明：追踪的完整性在这里让位于实现简洁，且**代价被明确写出来了**。

**日志级别在 2026 线格式上从「连接级」变成「请求级」。** `allowed_log_levels` 的 docstring 把两代语义都写清了：

- **2026-07-28 及以后**：日志投递是**逐请求 opt-in**。客户端在 `_meta` 里设保留键 `io.modelcontextprotocol/logLevel`；**缺失意味着不投递任何级别**（服务端「必须不发」）；存在意味着**该级别及更高级别**。未识别的值按缺失处理。
- **握手时代（更早）**：保留 `logging/setLevel` 语义，**所有级别都可以发**，过滤是应用自己的 `setLevel` 处理器的责任。

实现只有三行：非现代版本直接返回全集；现代版本从 `meta` 取保留键，不在合法级别里就返回空集；否则返回「该级别到末尾」的切片。

**「该级别及更高级别」是靠字面量声明顺序实现的。** `_LOG_LEVELS = get_args(LoggingLevel)`，注释说明它是**严重度递增**的（按 RFC 5424 尺度），并且**那个 Literal 的声明顺序是排序的唯一来源**。于是「该级别及以上」就是 `_LOG_LEVELS[_LOG_LEVELS.index(requested):]`——不需要任何映射表，改协议字面量的顺序会直接改变语义（这也意味着那个顺序是协议契约的一部分，不能当实现细节动）。

**连接级发射器在现代版本上什么都不发，且这是靠同一个函数实现的。** `allowed_log_levels(protocol_version, None)`——注意第二个参数是 `None`——在现代版本上必然返回空集，于是 `Connection.log` 里的 `if level not in allowed_log_levels(...)` 恒真，走 debug 日志并丢弃。方法的 docstring 把这个后果写明了：**在 2026-07-28+ 连接上它永远不发**，因为日志投递是逐请求的选择加入、搭在发起请求的那条流上（`ctx.log`、`ctx.session.send_log_message`），而独立流被禁止携带 `notifications/message`。

用一个纯函数同时表达两代语义，好处是「哪里能发日志」只有一个判定点；代价是 `meta=None` 这个调用约定承载了语义（「没有逐请求的 opt-in 信息」），读代码时必须知道它代表什么。

**`log` 方法本身被标记为已废弃。** `@deprecated(..., category=MCPDeprecationWarning)`，消息说明日志能力在 2026-07-28 被 SEP-2577 废弃。这与 `LOG_LEVEL_META_KEY` 的注释形成有趣的对照：**那个 `_meta` 键是「引入它的同一版规范里被废弃的」**。也就是说协议在引入逐请求日志的同时宣布整个日志能力要退场——保留它只是为了在过渡期让旧客户端仍能收到日志。

**`MCPServer` 的日志配置也顺带说明了「日志是给运维的」。** 构造 `MCPServer` 时会 `logging.basicConfig` 一个指向 stderr 的 handler（级别取 `log_level=`，默认 `"INFO"`），且 `basicConfig` 不会替换已有 handler——应用自己配过就应用赢。这条与追踪层的边界一致：两者都是**给操作者看的**，模型看不到。

## 关键取舍

**源码事实：** OTel 中间件默认启用，无 exporter 时是 no-op。可观测是默认行为而非插件。

**源码事实：** 父上下文提取在没有有效 `traceparent` 时返回 `None` 而不是空 `Context`，因为空上下文会让 span 变成孤儿而不是嵌进当前 span。

**源码事实：** 三类异常的处理不同：`MCPError` 用协议错误码、`ValidationError` 用固定码与固定文案（**因为 pydantic 的消息带客户端输入**）、其他异常才 `record_exception`。

**源码事实：** `otel_span` 调用处显式关闭自动的异常记录与自动状态设置，异常处理完全由中间件控制。

**源码事实：** 工具错误不是异常，靠匹配 `CallToolResult(is_error=True)` 或 dict `{"isError": True}` 检测；dict 形态只认字面布尔 `True`，`1`/`"true"` 这类被承认会漏检。

**源码事实：** 日志级别在现代版本上必须由请求 `_meta` 的 `io.modelcontextprotocol/logLevel` 显式指定，缺失即不投递；旧版本退回 `logging/setLevel` 时代语义（所有级别可发，过滤由应用负责）。

**源码事实：** 「该级别及以上」直接由 `LoggingLevel` 字面量的声明顺序切片得出，该顺序是唯一的排序来源。

**源码事实：** 连接级日志发射器在现代版本上恒为不发，用的就是同一个判定函数（`meta=None`）。

**架构推论：** 这两件事被放在同一个页面里，是因为它们共享同一条约束：**可观测数据也受协议版本与安全边界管辖**。追踪受版本影响是因为父上下文从 `_meta` 来（不同时代的 `_meta` 语义不同）；日志受版本影响是因为「谁能看日志」从连接级变成了请求级。而两者都受安全约束：追踪不能把客户端输入写进 span，日志不能在没有明确 opt-in 的情况下发给客户端。把可观测当成「无脑多记」的做法在这两条上都会翻车。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/server/_otel.py` | `OpenTelemetryMiddleware` | 包住每条入站请求的 `SERVER` span |
| `src/mcp/server/_otel.py` | `OpenTelemetryMiddleware.__call__` | span 命名、按方法分流的属性、三类异常处置 |
| `src/mcp/shared/_otel.py` | `otel_span` | 开 span 的上下文管理器，端口化 kind/context/异常开关 |
| `src/mcp/shared/_otel.py` | `extract_trace_context` | 从 `_meta` 解 W3C 父上下文，无效时返回 `None` |
| `src/mcp/shared/_otel.py` | `inject_trace_context` | 出站时把当前上下文注入 `_meta`，跨两侧连追踪 |
| `src/mcp/server/connection.py` | `allowed_log_levels` | 一条请求可发的日志级别集合，按协议时代硬切换 |
| `src/mcp/server/connection.py` | `_LOG_LEVELS` | 严重度递增的级别序，Literal 声明顺序即排序真值源 |
| `src/mcp/server/connection.py` | `Connection.log` | 已废弃的连接级发射器，现代版本上恒不发 |
| `src/mcp/server/lowlevel/server.py` | `Server.middleware` 默认值 | 默认装上 OTel 中间件，移除即退出追踪 |

## 参考

- [mcp-python-sdk 源码：server/_otel.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/_otel.py)
- [mcp-python-sdk 源码：shared/_otel.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/shared/_otel.py)
- [mcp-python-sdk 文档：日志](https://github.com/modelcontextprotocol/python-sdk/blob/main/docs/handlers/logging.md)
- [OpenTelemetry 语义约定：GenAI](https://opentelemetry.io/docs/specs/semconv/attributes-registry/gen-ai/)
- [W3C Trace Context](https://www.w3.org/TR/trace-context/)

## 与主核 / AI 的关系

这一层与主核的等待事件/执行统计面对同一组问题：**采样什么、在哪一层采样、以及敏感数据怎么处理**。数据库的追踪里同样有「这条语句的文本要不要记」的问题——文本里可能含字面量常量，也就是用户数据。这里的对应物是 `ValidationError` 的消息，处理手法一致：**记脱敏后的等价物，不记原始消息**。

对 agent 工程的直接价值有三条。其一，**异常的自动记录要关掉，改成分类处置**。默认的 `record_exception=True` 会把每次参数校验失败都记成 span 异常事件，让真正的问题淹没在噪音里；而且更重要的是，异常消息本身可能是客户端可控的。其二，**「没有有效父上下文」必须返回 `None` 而不是空上下文**。这是一个很容易写错的 OTel 细节，写错的症状是追踪莫名其妙地断开——而且只在跨进程传播时才出现。其三，**日志的可达性会随协议演进而变**。同一次 `log` 调用在旧连接上真的发出去、在新连接上被静默丢弃，这类差异不会报错，只会表现为「某些客户端收不到日志」。判定必须集中在一个函数里，否则调用点会各写一套猜测。
