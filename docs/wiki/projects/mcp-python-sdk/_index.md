---
project: mcp-python-sdk
page_type: overview
display_name: MCP Python SDK
repo: https://github.com/modelcontextprotocol/python-sdk
license: MIT
language: Python
stars: 24929
stars_as_of: 2026-11-22
refs:
  - path: src/mcp/server/mcpserver/server.py
    symbols: [MCPServer]
  - path: src/mcp/client/session.py
    symbols: [ClientSession]
  - path: src/mcp/shared/jsonrpc_dispatcher.py
    symbols: [JSONRPCDispatcher]
  - path: src/mcp/shared/path_security.py
    symbols: [safe_join]
  - path: src/mcp/server/streamable_http.py
    symbols: [StreamableHTTPServerTransport]
  - path: src/mcp-types/mcp_types/version.py
    symbols: [SUPPORTED_PROTOCOL_VERSIONS]
---
# MCP Python SDK

## 一句话职责

Model Context Protocol 的官方 Python 实现：它不是 agent 框架，而是「模型与外部能力之间的协议栈」——定义消息格式、传输方式、服务端与客户端两侧的会话，以及授权、路径安全、请求状态等横切机制。

## 机制是什么

MCP 解决的问题很具体：**让一个模型客户端（宿主程序）能用统一协议发现并调用任意外部能力（服务器）**。协议本身规定消息形态与生命周期，SDK 提供两端的实现。

代码结构是**三层对称**：

- **shared/**：两端共用的协议机制——JSON-RPC 信封、消息分发器、传输上下文、路径安全、URI 模板、请求状态编码。
- **server/**：服务端——低层 `Server`（按方法名登记处理器）、高层 `MCPServer`（装饰器登记工具/资源/提示）、三种传输（stdio、SSE、Streamable HTTP），以及传输安全设置。
- **client/**：客户端——`ClientSession`（握手与回调）、`Client`（连接管理）、多会话组、响应缓存。

协议按版本演进：`mcp_types` 是独立分发的基础类型包（`src/mcp/types/*.py` 只是把它的命名空间通配再导出），版本常量集中在 `version.py`，且区分「握手版本」与「现代版本」两套。

**2.x 的一处重大改名**：`FastMCP` 已改名为 `MCPServer`。`src/mcp/server/fastmcp.py` 现在是刻意的空模块——import 就抛 `ModuleNotFoundError` 并给出迁移指引，存在的唯一目的是让旧代码拿到比「模块不存在」更有用的报错。

本文库按经审阅的机制单元组织：**10 个机制领域、共 29 篇机制页**，每个机制页对应一个可独立理解、可独立验证的机制单元，而不是一个源文件。

## 怎么实现的

下表的 10 个领域按下面的顺序排列：A 协议类型与消息 → B 分发与请求生命周期 → C 服务端骨架 → D 工具、资源与提示 → E 服务端传输与安全 → F 客户端与传输 → G 授权与路径安全 → H 订阅、诱导与请求状态 → I 扩展与技能 → J 缓存与可观测。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：`src/mcp/` 与 `src/mcp-types/` 两个包。`server/fastmcp.py` 因是刻意的空模块而无独立页面。

## 机制导航（29 篇 / 10 组）

### A-协议类型与消息（3 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-jsonrpc-envelope.md`](a01-jsonrpc-envelope.md) | JSON-RPC 信封与错误约定 |
| [`a02-method-registry.md`](a02-method-registry.md) | 方法名注册与能力协商 |
| [`a03-transport-context.md`](a03-transport-context.md) | 传输上下文与请求元数据 |

### B-分发与请求生命周期（3 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-dispatcher-contract.md`](b01-dispatcher-contract.md) | 分发器契约与调度选项 |
| [`b02-jsonrpc-dispatcher.md`](b02-jsonrpc-dispatcher.md) | JSON-RPC 分发器与在途请求表 |
| [`b03-progress-and-cancel.md`](b03-progress-and-cancel.md) | 进度通知与取消 |

### C-服务端骨架（4 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-lowlevel-server.md`](c01-lowlevel-server.md) | 低层服务端与处理器登记 |
| [`c02-mcpserver-facade.md`](c02-mcpserver-facade.md) | MCPServer 高层门面 |
| [`c03-server-session.md`](c03-server-session.md) | 服务端会话与出站通道 |
| [`c04-server-runner.md`](c04-server-runner.md) | 服务端运行器与中间件 |

### D-工具、资源与提示（4 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-tool-manager.md`](d01-tool-manager.md) | 工具注册与调用参数校验 |
| [`d02-resources-and-prompts.md`](d02-resources-and-prompts.md) | 资源、模板与提示 |
| [`d03-context-injection.md`](d03-context-injection.md) | 上下文参数注入 |
| [`d04-dependency-resolution.md`](d04-dependency-resolution.md) | 请求期依赖解析 |

### E-服务端传输与安全（3 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-streamable-http-server.md`](e01-streamable-http-server.md) | Streamable HTTP 服务端传输 |
| [`e02-stdio-server.md`](e02-stdio-server.md) | stdio 服务端与文件描述符占用 |
| [`e03-transport-security.md`](e03-transport-security.md) | 传输安全设置与请求体限制 |

### F-客户端与传输（3 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-client-session.md`](f01-client-session.md) | 客户端会话与握手 |
| [`f02-client-and-transports.md`](f02-client-and-transports.md) | 客户端连接与三种传输 |
| [`f03-session-group.md`](f03-session-group.md) | 多会话组与命名空间 |

### G-授权与路径安全（3 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-oauth-models.md`](g01-oauth-models.md) | OAuth 授权模型 |
| [`g02-path-security.md`](g02-path-security.md) | 路径穿越防护 |
| [`g03-uri-template.md`](g03-uri-template.md) | URI 模板展开与匹配 |

### H-订阅、诱导与请求状态（3 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-subscriptions.md`](h01-subscriptions.md) | 订阅总线与监听 |
| [`h02-elicitation.md`](h02-elicitation.md) | 诱导请求与结果形态 |
| [`h03-request-state.md`](h03-request-state.md) | 请求状态编码与边界校验 |

### I-扩展与技能（2 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-extension-framework.md`](i01-extension-framework.md) | 扩展点与绑定 |
| [`i02-skills.md`](i02-skills.md) | 技能包与资源校验 |

### J-缓存与可观测（2 篇）

| 页文件 | 标题 |
|---|---|
| [`j01-client-response-cache.md`](j01-client-response-cache.md) | 客户端响应缓存 |
| [`j02-observability.md`](j02-observability.md) | 链路追踪与日志级别 |

## 关键取舍

**源码事实：** 基础类型拆成独立的 `mcp-types` 分发，`src/mcp/types/*.py` 只是 `from mcp_types.xxx import *` 的镜像。这样「只想用类型不想装整套 SDK」的调用方能只依赖类型包。

**源码事实：** 协议版本分成「握手版本」与「现代版本」两组常量（`HANDSHAKE_PROTOCOL_VERSIONS` 与 `MODERN_PROTOCOL_VERSIONS`），`SUPPORTED_PROTOCOL_VERSIONS` 是两者拼接。这说明协议在演进中保留了两条兼容通道。

**源码事实：** 路径安全给了三个强度递减的工具：不依赖根目录的字符串级检测、跨平台绝对路径识别、以及解析后验容器的安全拼接。`safe_join` 的文档主动交代了 TOCTOU 窗口，并建议用 `O_NOFOLLOW` 缓解。

**源码事实：** `src/mcp/server/fastmcp.py` 是一个「会抛异常的模块」：import 即 `ModuleNotFoundError`，消息里带迁移链接。这是把「升级报错」当作产品设计的一部分。

**架构推论：** MCP 与 agent 框架的分工很清晰：协议栈负责「能力如何被发现与调用」，agent 框架负责「什么时候调用、调用哪个」。crewai、openai-agents、smolagents、pydantic-ai 都以它为依赖接 MCP 服务器，正是这条边界成立。

**架构推论：** 三层对称（shared / server / client）意味着安全机制可以只写一份。路径安全、URI 模板、请求状态编码都放在 shared 下，说明它们不是「服务端的事」而是「协议两侧都要遵守的规则」。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `src/mcp/server/mcpserver/server.py` | `MCPServer` | 高层服务端门面（原 FastMCP） |
| `src/mcp/server/lowlevel/server.py` | `Server` | 低层服务端：按方法名登记处理器 |
| `src/mcp/client/session.py` | `ClientSession` | 客户端会话与握手 |
| `src/mcp/shared/jsonrpc_dispatcher.py` | `JSONRPCDispatcher` | JSON-RPC 分发与在途请求表 |
| `src/mcp/server/streamable_http.py` | `StreamableHTTPServerTransport` | Streamable HTTP 传输 |
| `src/mcp/server/stdio.py` | `_claim_fd` | stdio 传输的描述符接管 |
| `src/mcp/shared/path_security.py` | `safe_join` | 路径穿越防护 |
| `src/mcp/shared/uri_template.py` | `UriTemplate` | RFC 6570 风格模板 |
| `src/mcp/server/request_state.py` | `AESGCMRequestStateCodec` | 请求状态加密编码 |
| `src/mcp-types/mcp_types/version.py` | `SUPPORTED_PROTOCOL_VERSIONS` | 协议版本常量 |

## 参考

- [mcp-python-sdk 源码仓库](https://github.com/modelcontextprotocol/python-sdk)
- [MCP 规范](https://modelcontextprotocol.io/specification/2025-06-18)
- [MCP 文档：Python SDK](https://modelcontextprotocol.io/docs/sdk/python)
- [MCP 规范：传输](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports)

## 与主核 / AI 的关系

MCP 与主核（多模态数据库内核）的交集在「协议栈该怎么分层」这一问题上有直接可对照之处：

**shared / server / client 三层 vs 主核的公共层与前后端。** MCP 把「两侧都要遵守的规则」放在 shared（路径安全、URI 模板、请求状态编码、消息分发），主核把跨层规则放在 common 层。两者都靠「同一份实现」避免两侧行为漂移——这是协议类系统的核心纪律。

**路径安全 vs 主核的路径约束。** `safe_join` 的做法（解析后验容器）与主核里把表空间路径解析成规范形式再比较前缀完全同构。两者都必须承认 TOCTOU 窗口存在，并用「带标志的打开原语」而非「再检查一次」来处理。

**请求状态编码 vs 无状态服务。** MCP 的服务端可以把状态加密进令牌再交给客户端保管（`RequestStateCodec`），主核里对应的是「把会话状态放客户端」这类设计取舍。共同的好处是服务端可以水平扩展，共同的代价是令牌体积与加密开销。

在 2B 波次里，mcp-python-sdk 是唯一一个「协议栈」而非「agent 实现」的项目。它的价值在于约束：一旦 agent 框架通过它接外部能力，工具发现、调用、授权、进度、取消这些语义就有了统一答案，不必每个框架自己发明一遍。看这份 wiki 的顺序建议是：先 A（消息）、再 B（分发）、然后 C 与 F（两端骨架），最后按需读横切机制。
