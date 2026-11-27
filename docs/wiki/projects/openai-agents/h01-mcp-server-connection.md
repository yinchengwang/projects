---
project: openai-agents
feature: mcp-server-connection
title: MCP 服务器连接与凭据安全
page_type: feature
schema_version: 1
refs:
  - path: src/agents/mcp/server.py
    symbols: [RequireApprovalToolList, _snapshot_tools]
  - path: src/agents/mcp/server.py
    symbols: [_first_unsafe_transport_error, _transport_error_urls_are_safe]
---

# MCP 服务器连接与凭据安全

## 一句话职责

定义一个 MCP 服务器的抽象生命周期（连接、列工具、调工具、清理）、把工具清单的缓存与审批策略规范化，并把传输层异常里的 URL 凭据剥掉——保证连接失败时既报得出原因，又不把令牌写进日志与异常链。

## 机制是什么

`MCPServer` 是抽象基类，抽象方法有四组：`connect` / `cleanup`（生命周期）、`name`（可读名字，抽象属性）、`list_tools` / `call_tool`（工具面）、`list_prompts` / `get_prompt`（提示面）。`list_resources`、`list_resource_templates`、`read_resource` 有默认实现，直接 `NotImplementedError` 并在文案里带上服务器名——所以「不支持资源」是一个可预期的能力缺失，而不是抽象方法未实现。

构造参数里有五个横切配置：`use_structured_content`（是否用 `tool_result.structured_content`）、`require_approval`（审批策略，六种输入形态）、`failure_error_function`（工具失败如何变成模型可见的错误文案；显式传 `None` 表示「直接抛」）、`tool_meta_resolver`（产出 MCP 请求的 `_meta`）、`custom_data_extractor`，以及工具级护栏两组。

`use_structured_content` 的注释解释了一个具体坑：默认 `False` 是为了向后兼容，因为多数 MCP 服务器会把结构化内容**同时**放进 `tool_result.content`，默认用它会导致内容重复。

## 怎么实现的

**审批策略的六种输入归一。** `RequireApprovalSetting` 是个联合类型：`"always"`/`"never"`、`dict[str, 策略]`、`{always: {tool_names}, never: {tool_names}}` 对象、同步或异步 callable、`bool`、`None`。`_normalize_needs_approval` 把它压成三种内部形态之一：`bool`、`dict[str, bool]`、或 callable。

`RequireApprovalToolList` 就是那个 `{"tool_names": [...]}` 的 TypedDict，`total=False` 所以键可缺省。校验分两层：`_to_bool` 只接受两个字符串字面量，其他值连同位置一起报错；`_validate_tool_names` 要求是列表且每项是字符串，报错时带下标（`require_approval.always.tool_names[2]`）——这种精确到下标与 JSON 路径的错误位置，是给配置文件调试用的。

`_is_tool_list_schema` 是个形状判别：只要字典里有 `always` 或 `never` 键，且对应值是含 `tool_names` 的字典，就认定它是对象形态而不是「工具名到策略的映射」。这条判别让两种 dict 输入不会混淆。

**最终形态的取值。** `_get_needs_approval_for_tool` 按内部形态分三支：callable 时包成 `async def _needs_approval(run_context, _args, _call_id)`（内部 await 若结果是 awaitable），**且 agent 为 None 时返回 `True`**——注释说这是「保留历史上的 fail-closed 行为」，即旧调用方走 `MCPUtil.to_function_tool()` 直接转换、拿不到 agent 时，一律要求审批。dict 形态按工具名查，查不到算 `False`。bool 形态直接用。

**工具清单的快照语义。** `_snapshot_tools` 一行实现：`[tool.model_copy(deep=True) for tool in tools]`。为什么必须深拷贝，`list_tools` 里那段注释写得很清楚：即使过滤器构造了新列表，`Tool` 对象与嵌套的 input schema 仍与缓存共享，调用方修改会破坏必填参数校验。所以**每次返回前都深拷贝**，而不只是在缓存命中时拷。

**连接的两条协议路径。** `_MCPServerWithClientSession.connect` 按 `MCP_V2` 分叉。v1 路径是标准的 `AsyncExitStack` 三连：进 transport（`create_streams`）、从 transport 元组取读写流（第二、三项，若第三项可调用则记成 session id 回调）、进 `ClientSession`、`await session.initialize()`。

v2 路径不同：`create_v2_client` 返回一个异步上下文管理器，内部在**独立 task** 里运行（`run_client`），把建好的 session 通过 `session_ready` future 交出来，然后用一个 `close_client` Event 控制存活到调用方 yield 结束。这段代码还处理了取消：若等 `session_ready` 时被取消，就取消 client task 并吞掉它的异常再重抛——保证取消不会留下一个还在跑的客户端任务。

**连接失败的处理顺序。** `connect` 的 except 分支先把异常分成三类：非 `Exception` 的（`BaseException`）原样留着；`Exception` 但抽不出 HTTP 错误也原样留着；抽得出 HTTP 错误的则挑第一个「不可保留」的错误，取出它的安全 cause，再判断这个错误是否属于可映射的三类（状态错误、连接错误、超时错误）。只有可映射且 cause 安全，才翻译成 `UserError`；否则保留原异常。

**清理放在异常处理之外。** 源码注释说明了这个结构的原因：「在离开连接异常处理器之后再跑清理，这样清理抛出的 `UserError` 不会把待处理的连接失败留作它的隐式 context」。所以 `if not connection_succeeded:` 那段 `await self.cleanup()` 在 except 块之后，且清理异常被单独记进 `cleanup_failure`。最后按优先级抛：清理失败优先（此时把连接错误全部清空），其次原始 `connection_exception`，最后是映射后的 `connection_error`。

**清理时容忍 cancel scope 错误。** 清理的 except 里专门识别 `RuntimeError` 且文案含 `"cancel scope"` 的情况，只记 debug。注释说这是 MCP 库 async generator 清理的已知问题，「不应该掩盖原始错误」。

**凭据安全的两级判定。** `_transport_error_urls_are_safe(http_error)` 收集三类 URL：异常自身 request 的 URL；若是状态错误，还收集重定向历史里每个响应的 request URL，以及 `location` 头拼出的目标 URL（拼不出就判不安全）。判定标准不是「看起来像 URL」，而是 `get_mcp_server_log_name(url) == url`——**即「日志用的脱敏函数对这个 URL 是恒等」**。只有恒等才认为 URL 里没有可剥除的凭据。

`_first_unsafe_transport_error(errors)` 在错误列表里找第一个「是 HTTP 传输错误且 URL 不安全」的。注意它的用途是「不可保留」——一旦找到，就说明这批异常只能整体脱敏，不能用原始对象。

`_safe_transport_cause` 的判定更严：不是 HTTP 传输错误就原样返回；是的话要求 URL 安全**且** `__cause__`/`__context__`/`__notes__` 三者全为空——因为异常链本身也可能带凭据。三者任一存在就返回 `None`，放弃保留原始对象。

**批量异常的安全化。** `_credential_safe_exception_group` 与 `_credential_safe_exception_leaf` 负责把含不安全 URL 的异常组重建：用固定文案（`"MCP request failed with additional errors."` 与 `"An additional error occurred during the MCP request."`）替代原始消息，同时保留异常类型结构。这两条固定文案是模块级常量，注释里的定位很明确——它们是**脱敏后的替身**，不是给用户看的原始错误。

## 关键取舍

**源码事实：** `use_structured_content` 默认 `False`。这不是「默认少用点功能」，而是「默认避免内容重复」的兼容决定——多数服务器的 `content` 里已经带了结构化数据。

**源码事实：** callable 审批策略在拿不到 agent 时返回 `True`（要求审批）。这是 fail-closed：宁可多弹一次审批，不可在无法判断时放行。

**源码事实：** `_transport_error_urls_are_safe` 用「脱敏函数是否恒等」判断安全性，而不是正则匹配 token 样式。好处是判定与脱敏逻辑同源，不会出现「判定说安全但脱敏改变了内容」的不一致。

**源码事实：** `_snapshot_tools` 无条件深拷贝，所以每次 `list_tools` 都付出拷贝成本。注释给出的理由是调用方可能改坏缓存的 schema——在没有冻结机制的前提下，拷贝是唯一可靠的隔离手段。

**源码事实：** 连接失败时的异常优先级是「清理失败 > 原始异常 > 映射后的传输错误」。清理失败会清空其它错误信息（`connection_exception = None` 等），因为「清理没成功」意味着资源可能仍在泄漏，这个信息比「连接为什么失败」更紧急。

**架构推论：** 清理代码刻意放在连接异常处理器之外，是一个易被忽略但很实际的细节：Python 在 `except` 块内抛出的新异常会自动挂上原异常作为 `__context__`，而清理异常里可能带连接 URL（含凭据）。`raise ... from None` 只抑制显示，不阻止数据在 `__context__` 里保留——把清理移出 except 块才是真正的切断。

**架构推论：** 审批策略做成六种输入形态的归一，是为了吸收三种使用群体的不同写法：配置文件用字符串、按工具清单配置用映射、需要运行时判断用 callable。归一到三种内部形态，让消费侧只需判三次而不是六次。

**架构推论：** v2 连接用「独立 task + future + Event」而不是简单的 `async with`，是为了给取消提供可收拾的边界。直接 `async with` 在取消时，客户端内部可能停在任意位置；显式持有 task 句柄才能保证取消后一定 join 它。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/mcp/server.py` | `MCPServer` | 服务器抽象：生命周期、工具面、提示面 |
| `src/agents/mcp/server.py` | `MCPServer._normalize_needs_approval` | 六种审批输入归一为三种内部形态 |
| `src/agents/mcp/server.py` | `MCPServer._get_needs_approval_for_tool` | 按工具取审批要求，拿不到 agent 时 fail-closed |
| `src/agents/mcp/server.py` | `MCPServer._get_failure_error_function` | 未设置时回落到 agent 级失败处理器 |
| `src/agents/mcp/server.py` | `RequireApprovalToolList` | `{tool_names: [...]}` 的审批清单形状 |
| `src/agents/mcp/server.py` | `_snapshot_tools` | 深拷贝工具清单以隔离缓存 |
| `src/agents/mcp/server.py` | `_transport_error_urls_are_safe` | 用脱敏函数恒等判定 URL 是否含凭据 |
| `src/agents/mcp/server.py` | `_safe_transport_cause` | 要求 URL 安全且异常链为空才保留原始对象 |
| `src/agents/mcp/server.py` | `_first_unsafe_transport_error` | 找出第一个需要脱敏的传输错误 |
| `src/agents/mcp/server.py` | `_credential_safe_exception_group` | 用固定文案重建异常组以删除原始信息 |
| `src/agents/mcp/server.py` | `_MCPServerWithClientSession.connect` | 按协议版本分叉的连接，清理在异常处理之外 |
| `src/agents/mcp/server.py` | `_MCPServerWithClientSession._client_session_context` | v2 独立 task 的会话上下文与取消收拾 |

## 参考

- [openai-agents-python 源码：mcp/server.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/mcp/server.py)
- [Model Context Protocol 规范](https://modelcontextprotocol.io/introduction)
- [Agents SDK 文档：MCP](https://openai.github.io/openai-agents-python/mcp/)

## 与主核 / AI 的关系

主核里有结构相似的「客户端连接对象」：连接、执行、清理三段，失败时按错误码分类，并且**错误消息里绝不能带连接串里的密码**。这一页的凭据安全部分与主核的日志脱敏是同一职责，差异在于这里的判定条件更精确——不是靠正则猜哪些字段是密钥，而是用「脱敏函数对 URL 是否恒等」判定。

与 CrewAI 的 MCP 客户端相比，两边都把「传输层成功不等于业务层成功」当作核心问题，但处置方向相反：CrewAI 在工具层把 `isError` 转成结构化失败对象；这里在服务器层把 HTTP 异常的 URL 凭据剥掉、把错误映射成可读的 `UserError`。前者关心「模型能否看见失败」，后者关心「日志能否安全地记录失败」。

对 AI 工程最实用的一条是清理必须放在异常处理之外。把 `await self.cleanup()` 写进 `except` 块是很自然的写法，但它会让清理异常自动继承原异常为 `__context__`，而清理路径往往要处理含凭据的 URL 或配置——于是敏感数据经由 `__context__` 泄漏到最终的错误报告里。`raise ... from None` 只能改善显示，不能切断引用；真正的做法是让清理发生在 except 块之后。

另一条是「脱敏判定与脱敏实现同源」。若判定用一套规则（正则匹配 `sk-` 前缀）、脱敏用另一套（替换 URL 的 userinfo），两者一定会出现缝隙。这里把判定直接定义为「脱敏函数是否改变输入」，逻辑上不可能不一致——这个技巧值得直接借鉴。
