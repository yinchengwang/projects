---
project: mcp-python-sdk
feature: transport-context
title: 传输上下文与请求元数据
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/shared/transport_context.py
    symbols: [TransportContext]
  - path: src/mcp/shared/metadata_utils.py
    symbols: [get_display_name]
---

# 传输上下文与请求元数据

## 一句话职责

`TransportContext` 是挂在每条入站消息上的**不可变传输身份证**：它说自己来自哪种传输、这条消息的通道能不能承载服务端发起的请求、以及传输层带来的请求头；`get_display_name` 则是客户端侧一个极小的显示名取值器，把「给人看的标题」与「给程序看的名字」的优先级固定下来。

## 机制是什么

`TransportContext` 只有三个字段，且类是 `@dataclass(kw_only=True, frozen=True)`——**不可变**，且必须全用关键字构造。三个字段分别是：

- `kind: str`：传输的短标识（`"stdio"`、`"streamable-http"`、`"jsonrpc"`、`"direct"`）；
- `can_send_request: bool`：这条消息的请求级通道能不能投递服务端发起的请求；
- `headers: Mapping[str, str] | None`：本消息携带的请求头，HTTP 传输有、stdio 为 `None`。

模块 docstring 定义了分层边界：`TransportContext` 是基类，每种传输「定义自己的子类，带上合理的字段（HTTP 请求 id、ASGI scope、stdio 进程句柄等）」；**分发器只是把不透明地透传**，只有分发器之上的层（`ServerRunner`、`Context`、用户处理器）才读它的具体字段。

一个值得注意的事实：仓库里目前**没有** `TransportContext` 的子类。所有构造点都直接用基类，靠 `kind` 区分来源：`jsonrpc_dispatcher` 的默认构造器给 `kind="jsonrpc"`；streamable HTTP 的两处给 `kind="streamable-http"` 加 `headers=request.headers`；`direct_dispatcher` 给 `kind="direct"`。也就是说「子类扩展」是**留在设计里的余地，而非当下的实现**。

`get_display_name` 属于另一件事，但被同一页收拢，因为它处理的是另一类元数据——对象自带的展示信息。它的规则是两条优先级链：工具是 `title > annotations.title > name`，其他对象（资源、提示、资源模板、实现信息）是 `title > name`。

## 怎么实现的

**不可变的三个理由都在源码里能对上。**

`frozen=True` 让 `TransportContext` 可以安全地被持有、被 `replace` 出变体、被当作值比较；`kw_only=True` 让「这是三字段以上的不可变记录」在调用点一眼可读，也防止后续加字段时参数位置错乱。

`can_send_request` 的 docstring 是全文件最长的一段，它把「为什么可能为假」穷举成三类：

1. **响应里没有余量**：JSON 响应模式的 streamable HTTP，以及 2026-07-28 的单次交换入口，都只回一个 JSON-RPC 应答；
2. **客户端的回复无处落地**：无状态 HTTP，没有会话；
3. **协议禁止服务端发起请求**：任何 2026-07-28 连接，其分发会把这个标志掩掉。

反过来为真的情形也列清了：普通双工管道（stdio、SSE）与 2025-11-25 及更早的有状态 streamable HTTP（SSE 响应）。

最要紧的是最后一句：**当它为假时，`DispatchContext.send_raw_request` 会抛 `NoBackChannelError`，而不是挂一个永远等不到回复的等待者**。这句把「元数据字段」直接连到「可观察的行为」上——一个布尔字段不只是描述，它是错误路径的开关。

docstring 还加了一条范围限定：`can_send_request` **不**表达连接的独立通道（standalone channel）状态，「那条通道会单独拒绝」。这个区分很关键：streamable HTTP 上「请求级流」与「那条常驻 GET 流」是两条不同的通道，一条能回问不代表另一条能。

**`headers` 的可空是有意的。** docstring 明确「由 HTTP 类传输填充；stdio 上为 `None`。处理器使用前应做 None 判断」。用 `None` 而不是空 `Mapping`，让「这个传输根本没有头这个概念」与「有这个概念但这次没带」区分开——尽管实践中后者并不出现。

**`kind` 是纯字符串而不是枚举。** 没有任何 `Literal` 或 `Enum` 约束它，所以拼写错误不会被类型检查捕获，只能靠人工一致。这与 `can_send_request` 那样带完整语义说明的字段形成对比：`kind` 显然只被当作日志/调试标签，没有被用作行为分支的判据。

**`get_display_name` 的两处实现细节。**

第一处：

```
if isinstance(obj, Tool):
    if hasattr(obj, "title") and obj.title is not None:
        return obj.title
    if obj.annotations and hasattr(obj.annotations, "title") and obj.annotations.title is not None:
        return obj.annotations.title
    return obj.name
```

对 `Tool` 专门开了三档而不是两档，因为 `Tool` 除了顶层 `title` 还有 `annotations.title`（规范里的「注释块内的标题」）。非工具对象只有两档 `title > name`。

第二处是 `hasattr(obj, "title")` 这种防御性写法。签名已经限定了五种类型，`title` 按理说都在，所以 `hasattr` 是冗余的——它是「类型可能来自不同 schema 年代、某些对象缺这个字段」的保险，而不是风格选择。这里可以说是保守过头了：既然是联合类型，`getattr(obj, "title", None)` 一行就够了。

`Tool` 之外的四个类型共用同一分支，说明它们的 `title` 语义一致：只是「更好的名字」，没有嵌套的次选来源。

**`get_display_name` 的定位：纯客户端。** docstring 写得很清楚：「设计为客户端工具函数，帮 MCP 客户端在界面里显示人可读的名字。当服务端提供了 `title` 字段，应当优先于程序化的 `name` 字段用于展示。」参数名 `obj`、返回 `str`（**永远不会返回 None**）——最后这一点是它作为工具函数的价值：调用方不必再写 `x.title or x.name`。

仓库里的实际使用者是 `examples/snippets/clients/display_utilities.py`，它在遍历 `list_tools` / `list_resources` / `list_resource_templates` 的结果时逐个取显示名。测试则明确钉住了优先级：`test_title.py` 里 `test_get_display_name_utility` 依次验证「只有 name」「有 title」「有 annotations.title」「两者都有」四种情形。

**这两者为什么同页。** 因为它们代表 MCP 里「元数据」的两个层次：`TransportContext` 是**运行时元数据**（这次请求怎么来的），`title` / `name` 是**内容元数据**（这个对象该显示成什么）。两者都在 `shared/` 下、都不参与协议语义、都由上层按需读取，因此被归进同一组页面。

## 关键取舍

**源码事实：** `TransportContext` 是 `frozen=True` + `kw_only=True` 的三字段 dataclass，模块 docstring 给子类留了扩展位，但仓库里当前**只有基类在用**，靠 `kind` 字符串区分传输。

**源码事实：** `can_send_request` 的 docstring 穷举了三种为假的原因与两种为真的情形，并明确它为假时 `send_raw_request` 抛 `NoBackChannelError` 而非挂等待者。

**源码事实：** `can_send_request` **不**表达独立通道的状态，docstring 声明那条通道单独拒绝。请求级通道与连接级通道是两个不同的判断对象。

**源码事实：** `headers` 用 `None` 而非空映射表示缺失，且 docstring 要求调用方做 None 判断。

**源码事实：** `kind` 是裸 `str`，无 `Literal`/`Enum` 约束，实际上只作标签使用，不参与行为分支。

**源码事实：** `get_display_name` 对工具是三档（`title > annotations.title > name`），其他对象两档（`title > name`），且保证返回非空字符串。

**源码事实：** `get_display_name` 里的 `hasattr(obj, "title")` 在已限定联合类型的前提下是冗余的防御，反映的是「不同 schema 年代对象字段可能不齐」的顾虑。

**架构推论：** 分发器「不透明透传」传输上下文，使分发层完全不需要知道自己跑在哪种传输上。「不支持的服务端请求」因此不是分发器里的分支，而是**由传输在元数据上声明、由上下文的访问器统一拒绝**。这把「哪种传输有什么限制」的知识集中到了构造点。

**架构推论：** 把「够不够宽」这条限制做成一个布尔字段而不是分散的能力查询，让「不能回问」这件事有了单一判断点。代价是它只能表达「能不能」，表达不了「能但要点对点路由」这类更细的差异——后者被推到了 `related_request_id` 那一侧。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/shared/transport_context.py` | `TransportContext` | 不可变传输身份证：kind / 能否回问 / 请求头 |
| `src/mcp/shared/metadata_utils.py` | `get_display_name` | 显示名取值：工具三档、其他两档，保证非空 |
| `src/mcp/shared/context.py` | `BaseContext.transport` | 把传输元数据透出给用户处理器 |
| `src/mcp/shared/jsonrpc_dispatcher.py` | `_JSONRPCDispatchContext.can_send_request` | 传输声明且上下文未关闭才允许回问 |
| `src/mcp/server/runner.py` | `_NoServerRequestsDispatchContext.transport` | 用 `replace` 把传输标志掩成 False，使元数据与拒绝行为一致 |
| `src/mcp/shared/direct_dispatcher.py` | `_DirectDispatchContext.can_send_request` | 进程内直连的同一判断点 |

## 参考

- [mcp-python-sdk 源码：transport_context.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/shared/transport_context.py)
- [mcp-python-sdk 源码：metadata_utils.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/shared/metadata_utils.py)
- [MCP 规范：传输层](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports)
- [MCP 规范：工具（含 title 字段）](https://modelcontextprotocol.io/specification/2025-06-18/server/tools)

## 与主核 / AI 的关系

「传输上下文」与主核里的「连接元数据」（客户端地址、协议版本、SSL 状态）是同一类东西：都是**运行时才知道、且要在处理器里问得到**的信息。主核的做法通常是把它塞进会话结构，处理器通过会话句柄访问；这里把它做成一个独立的不可变值，跟着消息走——因为同一条连接上并发的多条消息可能来自不同传输阶段（例如 streamable HTTP 的一次 POST 与一次 SSE、一条一次性的单次交换请求）。

对 AI 工程的实际意义在于 `can_send_request` 这个字段的设计：它把「这条通道能不能反向请求」从一个**隐式假设**变成了**显式可查的事实**，并且把为假时的行为钉死为「立刻抛 `NoBackChannelError`」。很多实现会在这种地方挂一个等待者然后超时，表现为请求卡住几十秒才失败，且日志里看不出原因。这里的做法把「不可能成功」变成「立刻拒绝」，依据是构造点已经知道答案——**能静态判定的失败，不该留到运行期超时**。

`get_display_name` 则是一个小但典型的「展示层与语义层分离」样例：`name` 是程序标识（稳定、唯一、可被引用），`title` 是给人看的（可变、不必唯一、可本地化）。把两者的优先级写成一个函数，比在每个 UI 调用点写 `obj.title or obj.name` 更不容易漂移——尤其当同一份代码要处理 `Tool` 与 `Resource` 这两种优先级规则不同的对象时。
