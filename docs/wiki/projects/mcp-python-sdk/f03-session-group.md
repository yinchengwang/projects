---
project: mcp-python-sdk
feature: session-group
title: 多会话组与命名空间
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/client/session_group.py
    symbols: [ClientSessionGroup, ClientSessionParameters, StreamableHttpParameters]
---

# 多会话组与命名空间

## 一句话职责

`ClientSessionGroup` 把多个 MCP 服务端的工具、资源、提示聚合成**一套扁平命名空间**，同时记住每个组件属于哪个会话——于是上层的运维助手可以像对待一个服务端那样调用几十个服务端的能力，而冲突与拆除都由它负责。

## 机制是什么

一个「组」要解决三个问题，分别对应三张表：

| 问题 | 表 |
|---|---|
| 现在有哪些组件？ | `_tools` / `_resources` / `_prompts`（名字 → 组件） |
| 这个工具该发给谁？ | `_tool_to_session`（工具名 → 会话） |
| 删掉这个会话要连带删什么？ | `_sessions`（会话 → `_ComponentNames` 反查索引） |

第三张表是关键。前两张都是「正向」的，用于查找与调用；`_sessions` 里存的 `_ComponentNames` 记录了这个会话贡献了哪些名字，断开时逐个删掉。没有它，断开一个服务端就只能靠遍历比较，无法区分同名组件。

聚合的前提是**名字不冲突**。协议本身不保证不同服务端不会重名，所以组提供了 `component_name_hook`：一个 `(组件名, 服务端信息) => 新名字` 的函数。docstring 的示例直接给出了最常用的写法 `f"{server_info.name}_{name}"`。不给钩子时名字原样保留——也就是说**默认不解决冲突**，冲突会在聚合期抛错。

## 怎么实现的

**两种连接入口，一个聚合动作。**

- `connect_to_server(server_params, session_params)` 是完整路径：`_establish_session` 建传输、建会话、握手，然后 `connect_with_session` 聚合。
- `connect_with_session(server_info, session)` 是**已有会话**的入口，只做聚合。这让你可以自己建会话（例如复用 `Client` 的连接逻辑）再交给组管理。

`_establish_session` 按 `ServerParameters` 的三态分派。注意第三态（`StreamableHttpParameters`）比其他两个多做一步：**先自己建 `httpx` 客户端**（`create_mcp_http_client`，把 `timeout` 与 `sse_read_timeout` 映射成 `httpx2.Timeout(timeout, read=...)`），进退出栈，再把它传给 `streamable_http_client(http_client=...)`。前两态（stdio、SSE）都是直接进传输。

`session_params` 默认是 `ClientSessionParameters()`，它把 `read_timeout_seconds` 与五个回调原样递给 `mcp.ClientSession(read, write, ...)`。注意这里用的是**流**而不是分发器——即 `ClientSession` 会自己建 `JSONRPCDispatcher`。

`ClientSessionParameters` 的注释值得单记一句：「Use dataclass instead of Pydantic BaseModel because Pydantic BaseModel cannot handle Protocol fields」。五个回调的类型都是 `Protocol`（`SamplingFnT` 等），而 Pydantic 模型对任意 `Protocol` 字段会尝试校验，于是只能退成 `dataclass`。这条约束在 `StreamableHttpParameters` 那边不存在，所以它仍是 `BaseModel`。

**双层退出栈是这一页最值得看的机制。** 每个会话有**自己的** `AsyncExitStack`（`session_stack`），并且在成功后把自己也注册到组的 `_exit_stack` 里：

```python
self._session_exit_stacks[session] = session_stack
await self._exit_stack.enter_async_context(session_stack)
```

这么做换来两件事：

- **可以单独断开一个会话**：`disconnect_from_server` 取出那一个栈并 `aclose()`，不影响其他会话。
- **组退出时自动全断**：主栈展开会依 LIFO 顺序关掉所有会话栈。

`_establish_session` 的失败路径也围绕这个栈写：整段在 `try` 里，任何一步失败就 `await session_stack.aclose()` 再 `raise`。注释只说「If anything during this setup fails, ensure the session-specific stack is closed」，但重点是**此时还没有把它注册到主栈**——所以必须在这里手动关，否则资源泄漏。

`__ainit__` 还有一个所有权标记：`_owns_exit_stack`。若调用方传了 `exit_stack`，组就不在 `__aenter__` 里进入它、也不在 `__aexit__` 里关闭它（因为那是调用方的栈）。`__aexit__` 里关会话栈用的是 `anyio.create_task_group()` + `tg.start_soon(exit_stack.aclose)`，即**并发关闭**所有会话栈。

**聚合的「先算后合」策略。** `_aggregate_components` 先把要加的东西算进四个临时字典（`prompts_temp` / `resources_temp` / `tools_temp` / `tool_to_session_temp`），最后才一次性 `update` 到正式表里。注释写明动机：「We do not want to modify the aggregate lists in case of an intermediate failure」。

四个临时结构对应三类组件 + 一张归属映射。每类的流程一样：调 `session.list_xxx()`，逐个用 `_component_name` 改名，塞进临时 dict，并把名字加进 `component_names` 的对应集合。三类查询各自被 `try/except MCPError` 包住，失败只 `logging.warning`——**一个服务端不支持 prompts 不算连接失败**。

这里有个细节：若三类都拿不到东西，代码会 `del self._session_exit_stacks[session]`，注释是「Clean up exit stack for session if we couldn't retrieve anything from the server」。注意它只从表里删掉引用，**并没有关栈**；栈仍然在组的主栈里被管理。所以这是一个「不参与单独断开」的标记，而不是资源清理。

**冲突检测在三类上分别做，且都是硬失败。** 用集合交集 `prompts_temp.keys() & self._prompts.keys()` 找重名，非空就抛 `MCPError(code=INVALID_PARAMS, message=f"{matching} already exist in group prompts.")`。三类各一条，文案分别写 `group prompts` / `group resources` / `group tools`。

这条策略的含义很明确：**聚合器不做自动消歧**。要么用 `component_name_hook` 事前避免，要么连接失败。这与 `ToolManager` 那种「重名只警告、保留旧的」形成对照——组不允许静默覆盖，因为覆盖意味着某个服务端的能力凭空消失。

**`disconnect_from_server` 的两个「已知」判据。** 它分别检查会话是否在 `_sessions`（组件索引）与 `_session_exit_stacks`（栈表）里：

```python
session_known_for_components = session in self._sessions
session_known_for_stack = session in self._session_exit_stacks
if not session_known_for_components and not session_known_for_stack:
    raise MCPError(code=INVALID_PARAMS, message="Provided session is not managed or already disconnected.")
```

两者分开判断是有原因的：前面提到「什么都取不到」的会话只从栈表里删掉了，所以它可能「在组件表里不在栈表里」，也可能反过来。两个判据都假才拒绝，正好覆盖这些中间态。

清理顺序是：先 `pop` 出 `_ComponentNames`，按三类逐名删除，工具额外还要删 `_tool_to_session` 里的归属；最后若有栈就 `pop` 并 `aclose()`。

**`call_tool` 的两级查找。** 这是组作为「门面」的落点：

```python
session = self._tool_to_session[name]
session_tool_name = self.tools[name].name
```

第一行按聚合名找到会话；第二行取**组件自己声明的名字**——因为聚合名可能被 `component_name_hook` 改过，真正发给服务端的必须是原名。这一步如果写成 `session.call_tool(name, ...)` 就会在用了钩子后全部失败。这是整页最容易忽略的一行。

`call_tool` 还带两个 `@overload`，用来区分 `allow_input_required=False`（返回 `CallToolResult`）与 `True`（返回 `CallToolResult | InputRequiredResult`）。docstring 说明 `False` 时若服务端返回 `InputRequiredResult` 会 `RuntimeError`。

**`_component_name` 是钩子的唯一调用点**，三行：有钩子就用，否则原名返回。它被三类组件共用，所以钩子看到的第一个参数是「任意组件名」，不含类型信息——若想让工具与资源用不同前缀，钩子只能自己判断（或用同一前缀，如示例那样）。

**三个传输参数模型的默认值差异反映现实。** `SseServerParameters.timeout` 默认 **5.0** 秒，而 `StreamableHttpParameters.timeout` 默认 **30.0** 秒；两者的 `sse_read_timeout` 都是 300 秒。前者是旧传输（长连接一次建立、之后一直读流），后者每次请求都要走一次 HTTP，所以给更宽的超时。`StreamableHttpParameters` 还有两个 SSE 时代没有的字段：`terminate_on_close`（默认真，传输关闭时关会话）与 `max_sse_event_size`（`gt=0` 校验）。

## 关键取舍

**源码事实：** 组维护三张正向表（组件名 → 组件、工具名 → 会话）与一张反查表（会话 → 组件名集合）。反查表是断开时精确清理的唯一依据。

**源码事实：** 每个会话有自己的退出栈，且该栈本身被注册进组的主栈；于是既能单独断开，又能整体退出。`_establish_session` 失败时手动关这个栈（因为尚未注册）。

**源码事实：** 传入外部 `exit_stack` 时组不拥有它，`__aenter__` 不进入、`__aexit__` 不关闭。

**源码事实：** 聚合先写临时字典、全部算完后一次性合并，避免中途失败污染聚合结果。

**源码事实：** 三类组件查询各自容错（`MCPError` 只 warning），但**重名一律硬失败**（`INVALID_PARAMS`）。

**源码事实：** `call_tool` 用聚合名定位会话，用组件自己声明的名字发给服务端——两级别名。

**源码事实：** `ClientSessionParameters` 是 `dataclass` 而非 `BaseModel`，因为回调字段是 `Protocol` 类型，Pydantic 处理不了。

**架构推论：** 「默认不消歧、冲突即失败」是一个保守但可解释的选择：静默覆盖在多方聚合场景里几乎总是错的，因为被覆盖方的能力会无声消失，而上层无从判断少的是什么。提供钩子而不提供默认前缀，把命名策略的决定权交给使用者——因为只有使用者知道自己的服务端名字是否稳定。

**架构推论：** 双级别名（聚合名 vs 原名）是命名空间映射的必然产物，也是这类聚合器最容易出错的地方。一旦允许改名，就存在「对外名字」与「对内名字」两套，所有面向服务端的调用都必须用后者，而所有面向用户的查询都用前者。这份实现把两者分别放在 `_tools` 的键与 `Tool.name` 里，靠一行 `session_tool_name = self.tools[name].name` 接上。

**架构推论：** 用一个专门的 `_ComponentNames` 模型而不是三个集合属性，让「一个会话贡献了什么」成为一份可整体存取的数据。这也让「部分清理」变得不可能——要么整个会话被移除，要么它完整存在。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/client/session_group.py` | `ClientSessionGroup` | 多服务端聚合：三张表、连接、聚合、断开、调用 |
| `src/mcp/client/session_group.py` | `ClientSessionGroup._establish_session` | 三态传输分派、建会话、握手、双层退出栈注册 |
| `src/mcp/client/session_group.py` | `ClientSessionGroup._aggregate_components` | 先临时后合并的聚合与三类冲突检测 |
| `src/mcp/client/session_group.py` | `ClientSessionGroup.disconnect_from_server` | 按反查索引精确清理，含两个「已知」判据 |
| `src/mcp/client/session_group.py` | `ClientSessionGroup.call_tool` | 两级名字查找：聚合名定位会话、原名发给服务端 |
| `src/mcp/client/session_group.py` | `ClientSessionGroup._component_name` | `component_name_hook` 的唯一调用点 |
| `src/mcp/client/session_group.py` | `ClientSessionGroup._ComponentNames` | 会话 → 组件名集合的反查索引 |
| `src/mcp/client/session_group.py` | `ClientSessionParameters` | 会话参数（dataclass，因回调字段是 `Protocol`） |
| `src/mcp/client/session_group.py` | `StreamableHttpParameters` | HTTP 传输参数：超时、`terminate_on_close`、事件大小上限 |

## 参考

- [mcp-python-sdk 源码：client/session_group.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/client/session_group.py)
- [mcp-python-sdk 源码：client/client.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/client/client.py)
- [MCP 规范：服务端特性](https://modelcontextprotocol.io/specification/2025-06-18/server)
- [Python 文档：AsyncExitStack](https://docs.python.org/3/library/contextlib.html#contextlib.AsyncExitStack)

## 与主核 / AI 的关系

「把多个来源的组件聚合成一套命名空间，并记住每个组件的归属」这件事，主核里对应的是 **schema 搜索路径与外部表**：多个 schema 各有同名表时，靠搜索路径决定解析到哪一个；外部表（foreign table）则把远端的数据源映射成本地一个名字，删除映射时要连带清理本地元数据。两边的核心都是「一份反查索引 + 一套冲突规则」。

冲突策略的差别值得对照：主核的搜索路径是**有序的**，冲突时按顺序取第一个，且允许用完全限定名绕过；这里没有顺序概念，冲突即失败，靠钩子改名规避。原因是用途不同——数据库的查询是人工/程序写定的，能理解「前面的 schema 优先」；聚合器的组件名要交给模型去挑，模型无法推理「同名时谁优先」，所以必须让名字唯一。

对 AI 工程的实际意义在于：多 MCP 服务端是现实里最常见的部署形态（文件系统一个、GitHub 一个、数据库一个），而它们的工具名极容易撞车（`search`、`list`、`read`）。这份实现给出的三条经验可直接用：**用钩子加服务端前缀**（示例里就是这个）、**冲突时硬失败而不是覆盖**、以及**维护反查索引以便精确拆除**。最后一条在长跑的 agent 会话里尤其重要——服务端会掉线重连，如果没有反查索引，反复连接断开后聚合表里会残留幽灵组件，模型调用它们只会得到「未知工具」。

另有一处值得抄的工程细节：`call_tool` 里那行 `session_tool_name = self.tools[name].name`。任何做了名字映射的系统都要回答「发出去的时候用哪个名字」，而这里用一行代码把两套名字接上，比让调用方自己记映射要可靠得多。
