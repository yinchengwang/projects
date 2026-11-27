---
project: mcp-python-sdk
feature: context-injection
title: 上下文参数注入
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/server/mcpserver/utilities/context_injection.py
    symbols: [find_context_parameter, inject_context]
  - path: src/mcp/server/mcpserver/context.py
    symbols: [Context]
---

# 上下文参数注入

## 一句话职责

处理函数（工具、资源、提示）只要在签名里写一个注解为 `Context` 的参数，框架就会在调用时把当前请求的上下文对象塞进去——不经过客户端、不出现在参数 schema 里，作者因此能在不改变「函数就是一个普通 Python 函数」这件事的前提下拿到会话、进度上报、日志、注入请求等能力。

## 机制是什么

注入分两步，两处都在 `context_injection.py` 里，各只有十几行：

- `find_context_parameter(fn)`：**注册期**在签名里找出那个参数的名字。
- `inject_context(fn, kwargs, context, context_kwarg)`：**调用期**把 context 塞进 kwargs。

找到的名字被保存在各个注册对象上（`Tool.context_kwarg`、`ResourceTemplate.context_kwarg`、`Prompt.context_kwarg`），并在 `func_metadata(..., skip_names=[context_kwarg])` 中被排除出参数模型——所以它既不在对外的 `parameters` schema 里，也不参与参数校验，客户端根本无法为它传值。

`Context` 本身是一个 Pydantic 模型，但它的**实质内容是四个私有属性**：`_request_context`、`_mcp_server`、`_input_params`、`_subscriptions`。对外暴露的全是属性与方法——这是一层刻意做薄的包装：既让作者不必理解 `ServerRequestContext` 的结构，又保留了「拿不到就报错」的显式失败。

## 怎么实现的

**识别靠类型注解的递归扫描。** `find_context_parameter` 先 `typing.get_type_hints(fn)` 解析注解（这一步能处理字符串注解）。解析失败时**返回 `None` 而不抛错**（带 `pragma: lax no cover`），注释写明理由：解析不出来就找不到 context 参数，静默降级比拒绝注册更有用。

然后逐参数判断两种形态：

- 直接就是 `Context` 子类（`inspect.isclass(annotation) and issubclass(annotation, Context)`）。
- 泛型形态（`get_origin` 非空）里任一类型参数是 `Context` 子类——覆盖 `Optional[Context]` 这类包装。

只扫描参数、跳过 `return`。找到第一个就返回，所以**一个函数里最多注入一个 context 参数**。

注意这里用的是 `typing.get_type_hints` 而非 `inspect.signature` 的注解——因为要解析字符串注解（`from __future__ import annotations` 之后注解全是字符串）。

**注入是纯字典合并。** `inject_context` 只有两个条件：`context_kwarg` 非 `None` **且** `context` 非 `None`，两者都满足才 `{**kwargs, context_kwarg: context}`。`context` 为 `None` 时不注入——于是函数会因为缺参数而报错，这是一个显式的失败而不是传入一个坏值。

**`Context` 的四个私有字段决定它能做什么。** 构造函数全是关键字参数，四个都可选：

- `_request_context`：`ServerRequestContext`，提供 session、request_id、`protocol_version`、`close_sse_stream` 回调等。访问器 `request_context` 在它为 `None` 时抛 `ValueError("Context is not available outside of a request")`。
- `_mcp_server`：`MCPServer` 实例，供 `ctx.read_resource` 走嵌套调用。
- `_input_params`：`InputResponseRequestParams`，承载 `input_responses`（客户端对上一轮 `InputRequiredResult.input_requests` 的回答）与 `request_state`（客户端回传的不透明状态）。两者在首轮都是 `None`。
- `_subscriptions`：`SubscriptionBus`，供 `notify_*` 系列发布事件。

这套设计有一个明显后果：**同一个 `Context` 类型在请求内与请求外是两种东西**。框架在请求内构造时填满四个字段；作者若自己 `Context()` 造一个，所有属性访问都会抛 `ValueError`。这个「半可用对象」的形态是有意的——它让 `Context` 可以在签名里安全地当作类型标注（不需要 Optional），同时保证误用在第一处访问就暴露。

**一个容易踩的 Pydantic 细节：`Context[T]` 在运行期是另一个类。** `_keep_instance` 是一个 `mode="wrap"` 的 `model_validator`，它的实现只有一行：值已经是 `Context` 实例就原样返回，否则交给默认校验。注释解释了为什么必须有它：`Context[T]` 与 `Context` 在运行期是不同的类，若不拦截，Pydantic 会把传入的 `Context[T]` 实例**重新构造成一个普通 `Context`**，丢掉全部请求状态。这是「泛型 + Pydantic 模型」组合的经典陷阱，也是这一页最值得记的实现细节。

**`_nested_invocation` 是「同一个请求里的第二次调用」的正确做法。** 当工具体里调 `ctx.read_resource(uri)` 时，它需要一个新的 `Context` 去调资源处理器，但**不能**继承当前的 `_input_params`。理由写在 docstring 里：`input_responses` 与 `request_state` 的键是外层处理器自己铸造的，对嵌套调用没有意义，所以嵌套调用总是从第一轮开始。它构造新 `Context` 时只传 `_request_context`、`_mcp_server`、`_subscriptions` 三个，刻意不传 `_input_params`。

`read_resource` 因此有一条明确的运行时约束：若模板返回 `InputRequiredResult`，它**抛 `RuntimeError`** 并指向替代方案（`MCPServer.read_resource(uri, context)`）。docstring 还补了一句更细的警告：即使换成后者，「不要从使用了 `Resolve(...)` 的工具里调用」——因为解析器已经占用了那次调用的 `request_state` 通道，转发的结果会把它的状态冲掉。

**能力面按「是否依赖请求」分成三类。**

- **必须有请求**：`report_progress`（转 `session.report_progress`）、`read_resource`、`elicit`、`elicit_url`、`log` 及 `debug`/`info`/`warning`/`error`、`close_sse_stream`、`close_standalone_sse_stream`、`notify_*` 四个。它们的属性访问器在缺失时抛 `ValueError`。
- **可空**：`headers`（HTTP 传输才有，stdio 下为 `None`）、`input_responses`、`request_state`（首轮为 `None`）、`protocol_version`（请求外为 `None`）、`client_capabilities`（客户端未声明时为 `None`）。
- **必然有**：`request_id`（转 `str(request_context.request_id)`）、`mcp_server`、`session`。

`headers` 的 docstring 有一句安全提示：**头是客户端提供的输入，绝不能当作身份断言**。这类提示很少出现在 API 文档里，但它点出的是 MCP 里一个真实的误区——HTTP 头看起来像认证信息，而实际上任何客户端都能写。

**弃用是批量标注的。** `log` 与四个便捷级别都挂 `@deprecated(..., category=MCPDeprecationWarning)`，理由是「logging 能力自 2026-07-28 起弃用（SEP-2577）」。这类协议级弃用有个现实困难：客户端可能还在用旧版本，所以只能标注而不能删除，且调用处要 `# pyright: ignore[reportDeprecated]` 一路穿过。

## 关键取舍

**源码事实：** 注入通过「注册期找参数名 + 调用期塞字典」完成，参数名被存进注册对象并从对外 schema 中排除。一个函数最多注入一个 context 参数。

**源码事实：** 类型注解解析失败时静默返回 `None`，不阻止注册。

**源码事实：** `Context` 的四个构造字段都可选，缺失时属性访问抛 `ValueError("Context is not available outside of a request")`。这使 `Context` 能在签名里安全地当类型标注用。

**源码事实：** `_keep_instance` 阻止 Pydantic 把 `Context[T]` 实例重建为普通 `Context`（那会丢掉请求状态）。

**源码事实：** `_nested_invocation` 刻意不传 `_input_params`，所以嵌套调用总是第一轮；`ctx.read_resource` 遇到 `InputRequiredResult` 抛 `RuntimeError` 并指向 `MCPServer.read_resource(uri, context)`。

**源码事实：** logging 能力自 2026-07-28 起弃用，`log` 与四个级别方法都挂了弃用标注。

**架构推论：** 用「注解识别 + 名字注入」而不是「显式注册参数」，把作者的心智负担压到最小——写个普通函数，加一个带类型的参数，就拿到了上下文。代价是**隐式**：读签名看不出这个参数会被框架填，也只能靠类型注解表达意图（换个同形状的类就不认）。这也是为什么 `Context` 必须做成一个「半可用对象」：它出现在签名里时必须看起来像一个普通的可选依赖。

**架构推论：** `_nested_invocation` 那段是这套设计里最有信息量的地方。它揭示了一个容易被忽略的事实：**上下文不是一个对象，而是「一次往返的状态」**。同一个请求里再发起一次处理器调用时，请求元数据（session、元信息、生命周期）应该共享，而往返状态（客户端回答、不透明状态）必须重置。把这两类东西放在同一个 `Context` 里是便利，区分它们才是正确性。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/server/mcpserver/utilities/context_injection.py` | `find_context_parameter` | 注册期扫描注解找 context 参数，解析失败静默返回 `None` |
| `src/mcp/server/mcpserver/utilities/context_injection.py` | `inject_context` | 调用期纯字典合并注入 |
| `src/mcp/server/mcpserver/context.py` | `Context` | 四私有字段的薄包装，属性缺失即 `ValueError` |
| `src/mcp/server/mcpserver/context.py` | `Context._keep_instance` | `wrap` 校验器，防止 `Context[T]` 被重建丢了请求状态 |
| `src/mcp/server/mcpserver/context.py` | `Context._nested_invocation` | 嵌套调用用的 `Context`，刻意不带 `_input_params` |
| `src/mcp/server/mcpserver/context.py` | `Context.read_resource` | 内容读取入口，遇 `InputRequiredResult` 抛 `RuntimeError` |
| `src/mcp/server/mcpserver/context.py` | `Context.headers` | 传输层头，文档明确不可当作身份断言 |

## 参考

- [mcp-python-sdk 源码：context_injection.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/mcpserver/utilities/context_injection.py)
- [mcp-python-sdk 源码：mcpserver/context.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/mcpserver/context.py)
- [MCP 规范：客户端特性](https://modelcontextprotocol.io/specification/2025-06-18/client)
- [Pydantic 文档：模型校验器](https://docs.pydantic.dev/latest/concepts/validators/)

## 与主核 / AI 的关系

「按类型注解隐式注入一个上下文对象」这套做法，在主核里对应的概念是**会话状态**：执行期间有一份属于本次会话的上下文（当前事务、GUC 设置、当前用户），函数可以读取但不能由调用方直接传入。两边的共同点是「这份状态由系统提供而非由调用者传入」，因此它必须挂在某个隐含的位置上——主核挂在后端进程的全局状态里，这里挂在函数签名的一个参数上。

差别在于显式程度。主核的会话状态是全局可寻址的（`current_setting('x')`），任何函数都能读，不必声明；这里必须**声明一个参数**才能拿到。后者的好处是可测试：给函数传一个 `Context()` 就能单测（虽然属性访问会抛错，但至少能测不需要上下文的路径）；前者无法在单测里替换。

`_nested_invocation` 那条约束值得对照主核里的**嵌套事务 / 子查询**：外层有状态，内层不能继承那些「只对本次往返有意义」的部分。主核里对应的问题是「子查询不能继承外层的游标位置」。两者都在回答同一个问题：**哪些状态是可共享的、哪些必须重置**。

对 AI 工程的实际意义在于：上下文注入让处理函数能主动与客户端交互（提问、采样、上报进度），这是 MCP 相对「纯函数式工具」的核心增量。而这份实现的三个细节都值得抄：`Context` 做成「签名里安全、访问即失败」的半可用对象；嵌套调用用一个显式的构造函数而不是靠复制；以及 `headers` 上那句「不要当作身份断言」——最后这条是踩过坑才会写的注释。
