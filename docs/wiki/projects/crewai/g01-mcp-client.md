---
project: crewai
feature: mcp-client
title: MCP 客户端与连接生命周期
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/mcp/client.py
    symbols: [MCPClient, connect, call_tool]
---

# MCP 客户端与连接生命周期

## 一句话职责

把 MCP 服务器封装成「可连接、可列举、可调用、可断开」的会话对象，并在连接与调用的每个转折点上把失败翻译成分类明确的异常与事件，而不是让一组裸的传输层错误冒到上层。

## 机制是什么

`MCPClient` 持有一个 `BaseTransport`（stdio / http / sse 三种实现）和四个超时/重试参数：`connect_timeout`、`execution_timeout`、`discovery_timeout`、`max_retries`，默认值都是 30 秒与 3 次。连接状态由两个布尔共同决定：`connected` 属性是 `transport.connected and self._initialized`——传输层连上还不够，MCP 协议层的 `session.initialize()` 也必须成功。

资源管理用一个 `AsyncExitStack`：transport 与 session 都通过 `enter_async_context` 压进同一个栈。源码注释解释了原因——两者必须在同一个 async 作用域里进入，否则会出现 anyio 的 cancel scope 报错。这是这套实现里最容易踩的坑，也是它不用「分别 with 两层」的原因。

调用侧有两个方法：`call_tool` 返回内容，`call_tool_result` 返回 `(content, is_error)`。前者是后者的薄封装，丢掉 `isError` 标志。这个二选一的存在本身说明了 MCP 协议的一个特性：服务器用**成功的 JSON-RPC 响应 + `isError: true`**表示工具失败，只拿 content 的调用方无法分辨。

## 怎么实现的

**连接的顺序与失败分支。** `connect` 先 `if self.connected: return self`（幂等），取服务器信息后发 `MCPConnectionStartedEvent`（带 `is_reconnect`，来自 `_was_connected`）。然后：

1. `from mcp import ClientSession`——放在 try 内部，`ImportError` 有专门分支，转成「请 pip install mcp」的提示并发 `import_error` 类型事件。
2. `await self._exit_stack.enter_async_context(self.transport)` 进传输层。
3. 用 transport 的读写流构造 `ClientSession`，再压进同一个栈。
4. `asyncio.wait_for(self._session.initialize(), timeout=connect_timeout)`——MCP 协议要求 initialize 必须先于任何其他请求。
5. 置 `_initialized` 与 `_was_connected`，发 `MCPConnectionCompletedEvent`，带耗时。

失败分支有四条，各有明确语义：

- `ImportError`：清理 + 事件 + 重抛原类型。
- `asyncio.TimeoutError`：先 `_cleanup_on_error()`，再用 `find_http_status(e, cleanup_error)` 从两个异常里找 HTTP 状态码——**找到就往「服务器拒绝」的方向解释**，找不到才当作真超时。
- `asyncio.CancelledError`：源码注释写得很直白，「失败的传输会取消这个协程，所以取消本身什么也没说明；把传输解开才能看到原因」。所以它先清理、再找状态码、再找 `find_transport_failure`，两者都没有才认定这是真取消并原样重抛。
- 其他 `BaseExceptionGroup` 或 `Exception`：同样先清理再分类。显式捕获 `BaseExceptionGroup` 是因为「只装 `BaseException` 的异常组不是 `Exception`，但仍可能携带服务器的拒绝」。

**`_cleanup_on_error` 返回异常而不是抛出。** 它 `aclose()` 那个栈，把清理过程中抛出的异常**作为返回值**交回调用方，并在 `finally` 里重置 `_session`、`_initialized` 与一个新的 `AsyncExitStack`。这个「清理异常作为数据返回」的形状是整段分类逻辑的基础：HTTP 状态码往往只出现在解传输时的那次异常里，不在等待中的协程里。

**`_report_connection_failure` 统一收口。** 三种输入（状态码、已是 `MCPConnectionError` 的异常、普通异常）都被翻译成「一个 `MCPConnectionError` + 一个 `MCPConnectionFailedEvent`」，并返回该异常供调用方 `raise`。所以每条失败路径最终都同时得到「可抛的对象」与「已发出的事件」，不会出现只发事件不抛、或抛了却没事件的缺口。

**调用的三层。** `call_tool_result` 先确保已连接（未连接就 `await self.connect()`），清洗参数（`_clean_tool_arguments` 去掉 `None` 值并修格式），发 `MCPToolExecutionStartedEvent`；然后 `_retry_operation` 包住 `_call_tool_impl`，超时用 `execution_timeout`；拿到结果后按 `tool_result.is_error` 分别发 `MCPToolExecutionFailedEvent`（`error_type="tool_error"`）或 `MCPToolExecutionCompletedEvent`（带耗时与结果）。异常路径再发一次 failed 事件，`error_type` 由 `tool_execution_error_type(e)` 决定。

**重试策略。** `_retry_operation` 最多 `max_retries` 次，退避是 `2**attempt` 秒（1、2、4…）。分类规则：

- `MCPConnectionError` 直接重抛（不重试）。
- 有 HTTP 状态码且 `error_type_for_status` 判为 `authentication`，立刻转成对应错误抛出（不重试）。
- 错误文本含 `authentication` 或 `unauthorized` → `ConnectionError`。
- 含 `not found` → `ValueError`（资源不存在，重试无意义）。
- 其余可重试，用尽后 `ConnectionError(f"Operation failed after {max_retries} attempts: ...")`。

**工具清单缓存。** `list_tools(use_cache)` 在启用缓存时用 `_get_cache_key("tools")` 查模块级 `_mcp_schema_cache`，TTL 300 秒。缓存键由传输类型与地址拼成：stdio 用 `command:args...`，http/sse 用 `url`，统一前缀 `mcp:`，尾部接资源类型。这是**进程级共享缓存**，所以两个连到同一服务器不同配置的客户端会互相命中。

**断开的清理。** `disconnect` 若未连接直接返回；否则 `aclose()` 栈，`CancelledError` 与 `MCPConnectionError` 原样重抛，其余异常包成 `RuntimeError`，`finally` 里重置三个字段。所以断开既不会吞掉取消，也不会把服务器拒绝说成「断开失败」。

**上下文管理器。** `__aenter__` 就是 `connect`，`__aexit__` 就是 `disconnect`，没有额外语义。

## 关键取舍

**源码事实：** `connected` 要求 transport 与协议初始化都成立。所以「传输层连上了但 initialize 超时」的客户端在 `connected` 上是假，重连会重新走完整流程——这避免了「半初始化的会话看起来可用」这类隐蔽故障。

**源码事实：** 失败分类把「清理时抛出的异常」当成主要证据来源。这不是绕路，而是 MCP 传输实现的实际行为：服务器拒绝连接时，拒绝信息常常在解传输时才冒出来，而不是在等待 initialize 的协程里。

**源码事实：** `call_tool` 丢弃 `isError`，`call_tool_result` 保留。两个方法并存意味着 API 层面「可以放弃失败信息」，这要求调用方自觉——本仓的 `MCPNativeTool` 用的正是保留版。

**源码事实：** 缓存是模块级字典、键里不含进程身份或凭据。同一台服务器若有两个不同租户的客户端，工具清单可能互相污染（清单通常不因人而异，但过滤器不同就会）。

**源码事实：** 重试用固定指数退避，没有抖动。多客户端同时重试会同步撞击服务器，这是「简单优先」的代价。

**架构推论：** 这套实现的复杂度几乎全部在「失败如何分类」。连接阶段区分了导入缺失、超时、取消、服务器拒绝四类；调用阶段区分了连接错误、鉴权失败、资源不存在、可重试四类。分类的价值在于**决定要不要重试、以及该不该把错误升级成用户可懂的话**：鉴权失败重试一万次也不会成功，而网络抖动重试三次往往就通了。

**架构推论：** 事件在每次状态迁移上都发一对（started/completed 或 started/failed），使外部观测者能只靠事件重建连接与调用的时间线。这对 MCP 特别重要，因为 MCP 服务器的行为常常不受本进程控制，出问题时第一手证据就是这条时间线。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/mcp/client.py` | `MCPClient` | MCP 会话封装：连接、列举、调用、断开 |
| `lib/crewai/src/crewai/mcp/client.py` | `MCPClient.connect` | 四类失败的连接流程，栈式资源管理 |
| `lib/crewai/src/crewai/mcp/client.py` | `MCPClient.disconnect` | 关闭退出栈并重置会话状态 |
| `lib/crewai/src/crewai/mcp/client.py` | `MCPClient.connected` | 传输层与协议初始化的联合判据 |
| `lib/crewai/src/crewai/mcp/client.py` | `MCPClient._cleanup_on_error` | 解传输并把清理异常作为返回值交回 |
| `lib/crewai/src/crewai/mcp/client.py` | `MCPClient._report_connection_failure` | 统一成 `MCPConnectionError` 加事件 |
| `lib/crewai/src/crewai/mcp/client.py` | `MCPClient.call_tool` | 只返回内容，丢弃 `isError` |
| `lib/crewai/src/crewai/mcp/client.py` | `MCPClient.call_tool_result` | 返回内容与失败标志，按标志发不同事件 |
| `lib/crewai/src/crewai/mcp/client.py` | `MCPClient.list_tools` | 带 TTL 缓存的工具发现 |
| `lib/crewai/src/crewai/mcp/client.py` | `MCPClient._retry_operation` | 指数退避重试与不可重试分类 |
| `lib/crewai/src/crewai/mcp/client.py` | `MCPClient._get_cache_key` | 由传输类型与地址拼缓存键 |
| `lib/crewai/src/crewai/mcp/client.py` | `_MCPToolResult` | 内容加 `is_error` 的内部结果类型 |

## 参考

- [CrewAI 源码：mcp/client.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/mcp/client.py)
- [Model Context Protocol 规范](https://modelcontextprotocol.io/introduction)
- [Python 文档：contextlib.AsyncExitStack](https://docs.python.org/3/library/contextlib.html#contextlib.AsyncExitStack)

## 与主核 / AI 的关系

主核里与「连接」对应的是客户端连接的建立与认证：连接被拒、认证失败、网络超时、对端重启，每一类都对应不同的错误码与不同的重试策略。这里的分类颗粒度与动机完全一致，但失败来源更杂——MCP 是跨协议层的（JSON-RPC over stdio/HTTP/SSE），拒绝信息可能出现在传输层、协议层或业务层，所以必须「解开栈再找证据」而非只看最外层的异常。

与 AutoGen 相比，AutoGen 那边根本没有等价的 MCP 客户端：它的 MCP 支持在 `autogen-ext` 里以 `McpWorkbench` 的形式出现，把 MCP 会话整体包装成一个 `Workbench`，工具清单每轮通过 `list_tools` 重新取。CrewAI 的选择是把 MCP 拆成两层——底层是这里的 `MCPClient`（可复用、可独立测试、带重试与事件），上层是每个工具一个 `MCPNativeTool`（每次调用新建客户端）。这个拆分让「连接问题」与「工具问题」可以在两个层次分别诊断。

对 AI 工程最实际的一条是 `isError` 的存在感。MCP 服务器报工具失败用的是一条**传输成功的响应**，任何只处理异常的客户端都会把失败当成功。这与 f05 那页讲的 Slack `{"ok": false}` 是同一类问题在不同协议上的重演：**传输层成功不代表业务层成功**。凡是接外部工具，都要先确认它的失败信号落在哪一层。

另一条是 `AsyncExitStack` 的必要性。把 transport 与 session 压进同一个栈，意味着它们的生命周期严格嵌套、退出顺序由栈决定。分开用两层 `async with` 也能工作，但在 anyio 的 cancel scope 语义下跨作用域会炸。选一个统一的退出栈，是把「顺序正确」变成结构约束而不是靠人记。
