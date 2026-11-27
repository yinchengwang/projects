---
project: mcp-python-sdk
feature: jsonrpc-envelope
title: JSON-RPC 信封与错误约定
page_type: feature
schema_version: 1
refs:
  - path: src/mcp-types/mcp_types/jsonrpc.py
    symbols: [JSONRPCRequest, JSONRPCResponse, JSONRPCError]
  - path: src/mcp/shared/message.py
    symbols: [SessionMessage, ClientMessageMetadata, ServerMessageMetadata]
---

# JSON-RPC 信封与错误约定

## 一句话职责

`mcp_types.jsonrpc` 定义线上的四种信封（请求 / 通知 / 结果 / 错误）与一套**带宽分配过的错误码表**；`mcp.shared.message.SessionMessage` 把任一信封与传输层元数据（可恢复令牌、相关请求 id、SSE 关闭回调）打包成「进出一个传输的最小单位」。

## 机制是什么

两层，职责严格分离：

- **信封层**（`mcp_types`，可独立分发的 `mcp-types` 包）只描述 JSON-RPC 2.0 的形状：谁有 `id`、谁允许 `result`、谁允许 `error`。它不知道 MCP 的方法名。
- **会话消息层**（`mcp.shared`）不碰形状，只做一件事：给信封**挂传输侧附件**，让「这条消息从哪个连接来、要不要回复到哪个流上、断了能不能续」这些信息不污染信封本体。

`src/mcp/types/jsonrpc.py` 是纯转口——整个文件就是 `from mcp_types.jsonrpc import *` 加一行 `__all__` 镜像。看类型定义必须到 `src/mcp-types/` 下，这一点在仓库里读代码时最容易踩空。

## 怎么实现的

**四种信封的差异全部体现在字段的有无上。**

`JSONRPCRequest` 有 `jsonrpc` / `id` / `method` / `params`（后者可空）；`JSONRPCNotification` 与之同形但**没有 `id`**——这是规范里「不需要回复」的唯一表达方式，因此「是不是请求」不需要额外标志位，看有无 `id` 即可。`JSONRPCResponse` 的 `result` 声明为 `dict[str, Any]`（既非可选也不是 `Any`），意味着成功结果必须是一个对象，不能是标量或数组。`JSONRPCError` 则把 `id` 声明成 `RequestId | None`，docstring 写明理由：JSON-RPC 2.0 要求这个成员存在但可空，`None` 编码的是 `"id": null`，表示「连 id 都没能确定」。

四个类合成 `JSONRPCMessage` 联合，并配一个模块级 `jsonrpc_message_adapter = TypeAdapter(JSONRPCMessage)`。分发器接收入站消息时用的就是这类判别联合——所以「信封是哪种」是解析结果，不是发送方的声明。

**`RequestId` 的类型是个刻意的窄口。**

```
RequestId = Annotated[int, Field(strict=True)] | str
```

`strict=True` 只挂在 int 分支上：字符串 `"7"` 走 str 分支原样保留，数值 `7` 必须真的是 int（`True` 不会被 pydantic 宽松地当成 1）。这条约束的意义在关联域——后面分发器要拿请求 id 去查在途表，如果 `True` 能被当成 1，一个布尔参数就能冒充某个请求的 id。分发器侧还有对应的二次防御（`as_request_id` 显式拒 `bool`）。

**错误码不是随手列举，而是一张带区间的分配表。**

源码用注释写清了 `-32000..-32099` 这个 JSON-RPC 服务器错误区间内部的再分配：

- `-32000..-32019` 是实现方自定义区间；
- `-32020..-32099` 留给规范定义，从 `-32020` 开始**顺序分配**；
- `-32002`、`-32042` 标记为「保留但永不复用」（早期协议版本退役后腾出的号）。

按这张表排下来的结果是：`HEADER_MISMATCH = -32020`、`MISSING_REQUIRED_CLIENT_CAPABILITY = -32021`、`UNSUPPORTED_PROTOCOL_VERSION = -32022`、`URL_ELICITATION_REQUIRED = -32042`。SDK 自己占用了实现自定义区间的两个号：`CONNECTION_CLOSED = -32000`、`REQUEST_TIMEOUT = -32001`，并且 `CONNECTION_CLOSED` 的 docstring 明确写着「SDK 专用，永不发到线上」——它是内部等待者被唤醒时拿到的终止信号。

再往下才是标准码：`PARSE_ERROR`、`INVALID_REQUEST`、`METHOD_NOT_FOUND`、`INVALID_PARAMS`、`INTERNAL_ERROR`（依次 `-32700` / `-32600` / `-32601` / `-32602` / `-32603`）。

`INTERNAL_ERROR` 的 docstring 里有一条容易被忽略的事实：**SDK 只会构造通用的 `ErrorData` 信封，规范为每个错误码单独定义的包装类型一个都没用**。这让错误路径只有一个数据形状，代价是丢了每码特有的结构化字段。

**`ErrorData` 的三字段是宽松的。**

`code: int`（注意是裸 `int`，不是 `Literal` 或枚举）、`message: str`、`data: Any = None`。`message` 的 docstring 引用了规范原文「SHOULD 限制为简洁的单句」，`data` 则完全交给发送方定义。这是有意的：错误数据是**唯一允许承载任意结构**的信封部分，把它收窄会让扩展方法无法传递自己的错误详情。

**`SessionMessage` 才是传输层的真实单位。**

```
@dataclass
class SessionMessage:
    message: JSONRPCMessage
    metadata: MessageMetadata = None
```

只有两个字段，但 `metadata` 是个三选一的联合：`ClientMessageMetadata | ServerMessageMetadata | None`。两侧的元数据字段完全不重叠，因为客户端和服务端关心的事情本就不同。

`ClientMessageMetadata` 三项：`resumption_token`（断线续传令牌）、`on_resumption_token_update`（传输发新令牌时回调）、`headers`（每条消息要带的 HTTP 头，例如 `MCP-Protocol-Version`）。注释里点名这些头是「传输应该设置的」，即元数据在这里充当**发件人给传输的指令**。

`ServerMessageMetadata` 六项，是整个出站路径的接线板：

- `related_request_id`：本消息应答的是哪个入站请求——streamable HTTP 靠它把响应路由回原始 POST 的流；
- `request_context`：传输特有的请求对象（HTTP 是 starlette 的 `Request`，stdio 是 `None`）。类型是 `Any`，注释直说因为服务端层要与传输无关；
- `close_sse_stream` / `close_standalone_sse_stream`：两个「关流但不终止」的回调，一个针对当前请求的流，一个针对那条独立的 GET 流（用于主动推送通知）；
- `on_request_unanswered`：请求**没有响应也要收场**时（例如被对端取消）由分发器调用，供「线上必须结束这次请求」的传输收尾；
- `can_send_request: bool = True`：传输对「这条消息的请求级通道能不能承载服务端发起的请求」的判断，缺省为真。

最后一项值得单独看：它的默认值让「普通双工管道」不需要任何配置就有正确行为，只有**没有余量的传输**（JSON 响应模式的 streamable HTTP）才需要显式把它置假。这就是把「例外显式化，正例默认化」写进字段默认值。

## 关键取舍

**源码事实：** `src/mcp/types/jsonrpc.py` 与 `src/mcp/types/methods.py` 都是通配再导出的镜像模块，真正的定义在独立分发的 `mcp_types` 包里。这既让只用类型的人不必装整个 SDK，也让「读了哪个文件」这件事变得容易搞错。

**源码事实：** `RequestId` 的 int 分支带 `strict=True`，且分发器另有 `as_request_id` 再拒一次 `bool`。`True == 1` 因此不会在关联域里制造碰撞。

**源码事实：** 错误码有明确的区间与顺序分配规则，并标注了两个「保留永不复用」的号。这不是注释习惯，而是给未来新增码留出可预测的位置。

**源码事实：** `CONNECTION_CLOSED` 属 SDK 自定义区间，且永不写到线上——它是内部信号被复用成了错误码。

**源码事实：** `JSONRPCResponse.result` 是非可选的 `dict`，`JSONRPCError.id` 是必填但可空。两条都直接来自规范，且都能在类型层面强制。

**源码事实：** `INTERNAL_ERROR` 只走通用 `ErrorData`，规范为每码定义的包装类型全部弃用，等于用「结构统一」换「每码特有字段」。

**架构推论：** 把信封与传输附件分成两层，让同一份信封能在 stdio、SSE、streamable HTTP、进程内直连之间复用，而每种传输只需在元数据里塞自己的字段。代价是服务端要读具体字段就得先做 `isinstance` 判别（runner 里就是这么做的）。

**架构推论：** `can_send_request` 用「默认真、例外置假」的方式表达能力，使绝大多数传输零配置，也让「这条通道不能回问」这件事在 `DispatchContext` 上有一个统一的判断点，而不是散落到各个传输里。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp-types/mcp_types/jsonrpc.py` | `JSONRPCRequest` | 有 id 的信封，参数可空 |
| `src/mcp-types/mcp_types/jsonrpc.py` | `JSONRPCResponse` | 成功结果，`result` 必须为对象 |
| `src/mcp-types/mcp_types/jsonrpc.py` | `JSONRPCError` | 错误信封，`id` 必填但可空，承载 `ErrorData` |
| `src/mcp-types/mcp_types/jsonrpc.py` | `ErrorData` | 唯一允许任意结构的错误载荷 |
| `src/mcp-types/mcp_types/jsonrpc.py` | `jsonrpc_message_adapter` | 四类信封的判别联合解析器 |
| `src/mcp/shared/message.py` | `SessionMessage` | 信封 + 元数据的传输单位 |
| `src/mcp/shared/message.py` | `ClientMessageMetadata` | 客户端侧：续传令牌与每消息 HTTP 头 |
| `src/mcp/shared/message.py` | `ServerMessageMetadata` | 服务端侧：相关请求 id、SSE 回调、未响应收尾、回问许可 |

## 参考

- [mcp-python-sdk 源码：mcp_types/jsonrpc.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp-types/mcp_types/jsonrpc.py)
- [mcp-python-sdk 源码：shared/message.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/shared/message.py)
- [JSON-RPC 2.0 规范](https://www.jsonrpc.org/specification)
- [MCP 规范：基础协议](https://modelcontextprotocol.io/specification/2025-06-18/basic)

## 与主核 / AI 的关系

「信封 + 附件」两分法与主核里的「报文头 + 会话状态」是同一路数：网络层只认字节，语义层只认字段，中间那层负责挂上下文。主核的附加信息通常挂在连接的会话结构上，这里挂到**每条消息**上——因为 MCP 的一条连接上可能并发多个请求，而「这条响应该回到哪个流」是逐消息决定的，不是逐连接的。

对 AI 工程的实际意义在于错误码的分区纪律。很多人给 JSON-RPC 服务加错误码时随手挑号，结果是不同来源的码在同一区间里打架，客户端只能靠比对字符串判断。这里把 `-32000..-32099` 再切成「实现自定义 / 规范顺序分配 / 保留不复用」三段，并且把 SDK 自己的内部信号（`CONNECTION_CLOSED`）显式标注为「永不上线」，等于把「哪些码是协议事实、哪些是本实现的私有约定」写清了。这一条比任何抽象设计都更能防止长期演化中的语义漂移。
