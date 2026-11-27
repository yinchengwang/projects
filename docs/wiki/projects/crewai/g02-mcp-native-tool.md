---
project: crewai
feature: mcp-native-tool
title: MCP 工具的原生暴露
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/tools/mcp_native_tool.py
    symbols: [MCPNativeTool]
  - path: lib/crewai/src/crewai/crew.py
    symbols: [_add_mcp_tools, _inject_mcp_tools]
---

# MCP 工具的原生暴露

## 一句话职责

把 MCP 服务器上的每个工具变成 CrewAI 的一个 `BaseTool`，并且让**每次调用都新建一个 MCP 客户端**——用连接开销换取并发安全，避免并发调用共享同一份传输状态而触发 cancel scope 错误。

## 机制是什么

`MCPNativeTool` 继承 `BaseTool`，但它不持有客户端，只持有一个**工厂**：`client_factory` 是个零参可调用，每次 `_run_async` 都调一次，产出一个全新的 `MCPClient` 加传输层。模块头的 docstring 把理由写得很清楚：这保证并行调用——无论是同一个工具还是同一服务器上的不同工具——永不共享可变连接状态，而共享会导致 anyio 的 cancel scope 错误。

工具名是**服务器名 + 下划线 + 工具名**拼出来的：`f"{server_name}_{tool_name}"`。这样多个 MCP 服务器上同名的 `search` 工具在 CrewAI 侧互不冲突。同时它保留三个身份信息：`original_tool_name`（服务器上的真名，调用时用它）、`server_name`、`server_reference`（请求服务器时用的标识，目前是 AMP 的 slug，因为工具名是从 URL 推的，追溯不回 slug）。

参数模式由外部传进来：`tool_schema["args_schema"]` 有就作为 `args_schema` 传给 `BaseTool`，没有就让基类从 `_run` 签名推导（而 `_run` 只有 `**kwargs`，所以推导结果是空模式）。

## 怎么实现的

**同步外壳的三层。** `_run(**kwargs)` 要处理「调用方在事件循环里」与「不在事件循环里」两种情况：

1. `asyncio.get_running_loop()` 成功 → 说明**已经**在跑事件循环（例如 agent 的异步执行路径）。此时不能在当前线程 `asyncio.run`（会报「loop already running」），所以 `contextvars.copy_context()` 拿当前上下文，开一个 `ThreadPoolExecutor`，把 `ctx.run, asyncio.run, coro` 提交进去、`future.result()` 取结果。
2. `get_running_loop()` 抛 `RuntimeError` → 说明没有事件循环，直接 `asyncio.run(self._run_async(**kwargs))`。

复制上下文这一步是必须的：工具内部可能要读本次运行的集成令牌、事件上下文栈等 ContextVar，新线程默认拿不到，`copy_context` 把当前上下文带过去。

整段包在 `try/except Exception` 里，任何异常都被包成 `RuntimeError(f"Error executing MCP tool {original_tool_name}: ...")` 并 `from e` 保留原因链。

**异步体只有四步。** `_run_async` 里：建客户端 → `await client.connect()` → `try: tool_result = await client.call_tool_result(original_tool_name, kwargs)` → `finally: await client.disconnect()`。注意用的是 `call_tool_result` 而不是 `call_tool`——它需要 `isError` 标志。

**失败即返回，不抛。** 若 `tool_result.is_error` 为真，构造并返回一个 `ToolFailure`：`message` 是压平后的内容，`reason` 是 `ToolFailureReason.MCP_ERROR`，`details` 里塞 `{"server": ..., "tool": ...}` 便于定位。源码注释解释了这一步的必要性：`isError` 搭在一个本来成功的响应上，没有这层转换，agent 无法把它和真实结果区分开。

**内容压平。** `_extract_content` 三分支：结果本身是 `str` 就直接返回；有 `.content` 且非空就调 `content_blocks_to_text(result.content)`；否则 `str(result)`。

`content_blocks_to_text` 是 `mcp/content.py` 里的函数，按块类型分别处理：`TextContent` 取 `text`；`EmbeddedResource` 里若是 `TextResourceContents` 取文本，否则写成 `[<type>: <uri>]`；`ResourceLink` 写成 `[<type>: <uri>]`；`ImageContent` 与 `AudioContent` 写成 `[<type>: <mimeType>]`；其余 `str(block)`。所有块用换行连接。

关键在「不拷贝 base64」：图片、音频、blob 的 `data` 字段全部不进结果，只留一个带 MIME 类型的占位符。这是为了避免把几十 KB 的 base64 塞进模型上下文。

**装配到团队侧。** `Crew._inject_mcp_tools(tools, task_agent)`：取 `task_agent.mcps`；若 agent 实现了 `get_mcp_tools` 且 `mcps` 非空，调 `agent.get_mcp_tools(mcps=mcps)` 拿到 `MCPNativeTool` 列表，再用 `_merge_tools` 合并进当前工具集。`Crew._add_mcp_tools(task, tools)` 是它的外围包装：任务有 agent 才注入，最后 `return tools or []`。

`Agent.get_mcp_tools` 委托给 `MCPToolResolver.resolve(mcps)`，并在调用前先 `_cleanup_mcp_clients()` 清掉上一轮的解析器。解析器的 `_resolve_native` 做三件事：用一个**临时探测客户端**连上去 `list_tools` 拿清单（用完就断开，还 `sleep(0.1)` 缓冲）、按 `tool_filter` 过滤、为每个工具定义 `_client_factory()` 闭包并构造 `MCPNativeTool`。

所以最终形态是：**探测一次拿清单，之后每次工具调用各建一个连接**。清单可以缓存（`cache_tools_list`），连接不缓存。

## 关键取舍

**源码事实：** 每个工具每次调用都完整走一遍 connect/disconnect。这意味着 N 次调用有 N 次握手成本，包括 stdio 启动子进程或 HTTP 建连。设计上明确用这个代价换并发安全，注释里也点明了「否则会触发 cancel scope 错误」。

**源码事实：** 探测阶段结束后 `await asyncio.sleep(0.1)` 再继续，注释未解释原因，但从行为看是给传输层留出完全关闭的时间窗，避免紧接着创建新连接时撞上半关闭状态。

**源码事实：** `client_factory` 是闭包但不是每次调用都新建传输——它每次都 `self._create_transport(mcp_config)` 重新造传输层。所以「工厂」造的是完整的一套（传输 + 客户端），不是一个客户端复用一份传输。

**源码事实：** `args_schema` 缺失时 `BaseTool` 会从 `_run` 签名推导，而这里的 `_run` 是 `(**kwargs)`，推导出的模型没有任何字段。所以「JSON Schema 转 pydantic 失败」不会让工具消失，只会让它看起来不需要参数——工具注册仍在，但模型侧看不到应有的参数。

**源码事实：** 工具名加服务器前缀，但 `call_tool_result` 用的是 `original_tool_name`（不带前缀）。所以「展示名」与「调用名」是两个不同的字符串，只有这个类内部知道它们的关系。

**架构推论：** 用「每次调用新连接」而不是「连接池 + 锁」，是把并发问题从**同步**转成**隔离**。前者要处理重入、超时释放、半开连接；后者只需保证每个连接独占。代价是延迟与服务器侧的连接压力，而 MCP 服务器通常不是高频服务，这笔交易对本场景是划算的。

**架构推论：** 内容压平里丢弃 base64 是个有意的信息取舍：模型通过 MCP 拿到的图片只能看到「这里有一张 image/png」，看不到内容。要真正用图片，需要上层把占位符替换成多模态消息，而这层没有提供钩子。这是把「富内容进模型」的责任交给了更上层的组件。

**架构推论：** 三个身份字段（前缀名、原名、服务器引用）解决的是三种不同的寻址需求：给模型看的名字要全局唯一，给服务器发的名字必须原样，给运维看的引用要能追回配置来源。把它们全存在工具对象上，比在调用时从名字反推可靠得多。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/tools/mcp_native_tool.py` | `MCPNativeTool` | 每次调用新建客户端的 MCP 工具封装 |
| `lib/crewai/src/crewai/tools/mcp_native_tool.py` | `MCPNativeTool._run` | 同步外壳：在/不在事件循环两条路，含上下文复制 |
| `lib/crewai/src/crewai/tools/mcp_native_tool.py` | `MCPNativeTool._run_async` | 建连、调用、断开三步，失败转 `ToolFailure` |
| `lib/crewai/src/crewai/tools/mcp_native_tool.py` | `MCPNativeTool._extract_content` | 结果压平：字符串、内容块、兜底 str |
| `lib/crewai/src/crewai/tools/mcp_native_tool.py` | `MCPNativeTool.original_tool_name` | 服务器侧真名，调用时必须用它 |
| `lib/crewai/src/crewai/tools/mcp_native_tool.py` | `MCPNativeTool.server_reference` | 服务器请求标识，用于追溯配置来源 |
| `lib/crewai/src/crewai/mcp/content.py` | `content_blocks_to_text` | 块级压平，图像音频只留类型占位 |
| `lib/crewai/src/crewai/crew.py` | `_add_mcp_tools` | 任务级注入入口，无 agent 时不动工具集 |
| `lib/crewai/src/crewai/crew.py` | `_inject_mcp_tools` | 读 agent 的 mcps 并合并 MCP 工具 |

## 参考

- [CrewAI 源码：tools/mcp_native_tool.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/tools/mcp_native_tool.py)
- [Model Context Protocol 规范](https://modelcontextprotocol.io/introduction)
- [Python 文档：contextvars.copy_context](https://docs.python.org/3/library/contextvars.html#contextvars.copy_context)

## 与主核 / AI 的关系

主核里最接近的类比是「每个查询会话一个后端连接」而不是「全局共享一条连接」：共享连接需要协议层的同步与事务标识来隔离并发，而独立连接把这些复杂度推给操作系统的调度。这里做的正是同一件事——用连接隔离替代连接复用，把并发安全变成结构性保证。

与 AutoGen 相比，AutoGen 的途径是 `McpWorkbench`：一个工作台持有一条 MCP 会话，`list_tools` 与 `call_tool` 都走这条会话，工具是「工作台里的一堆名字」而不是「一堆独立的 Tool 对象」。CrewAI 反过来把 MCP 工具**展开成普通工具**，使得 agent 侧完全不需要知道「这个工具来自 MCP」——配额、失败策略、缓存、指纹 cfg 全部照常生效。代价是失去了「工作台级」的生命周期管理（`start`/`stop`/`reset`/`save_state`），只能靠每次调用自建自拆。

对 AI 工程最实用的一条是 `isError` 那条转换。MCP 服务器报失败时不会抛异常，只会给一个 `isError: true` 的正常响应；如果工具层不把它转成 `ToolFailure`，agent 会拿到一段看起来像结果的错误文案，然后基于它继续推理。这层转换看似只是包一层对象，实际决定了失败能不能被后续的策略层（见 f05）看见。

另一条是同步外壳里那套「在事件循环里就另起线程」的处理。这个模式在任何「同步 API 需要调用异步实现」的场合都会遇到：直接 `asyncio.run` 会炸，忽略上下文会丢 ContextVar。正确做法是两件一起做——检测现有循环 + 复制上下文到新线程。缺任何一半都会得到难以定位的故障：前者让调用直接异常，后者让工具里读到的运行上下文是空的。
