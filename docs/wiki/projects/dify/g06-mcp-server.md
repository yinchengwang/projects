---
project: dify
feature: mcp-server
title: MCP 服务端
page_type: feature
schema_version: 1
refs:
  - path: api/core/mcp/server/streamable_http.py
    symbols: [handle_mcp_request, handle_call_tool, build_parameter_schema]
  - path: api/core/mcp/types.py
    symbols: [Request, Notification, Result]
---

# MCP 服务端

## 一句话职责

把「一个 Dify 应用」暴露成一个 MCP 工具：客户端发 JSON-RPC 请求，服务端把它翻译成应用调用（`AppGenerateService.generate`），再把结果翻译回 MCP 的 `CallToolResult`——**中间最难的一步是「Dify 应用的可变输入表单」到「静态 JSON Schema」的投影**。

## 机制是什么

服务端的职责只有四件事，对四个 JSON-RPC 方法：

| 方法 | 处理函数 | 干什么 |
|---|---|---|
| `initialize` | `handle_initialize` | 协商协议版本、声明能力、回报 serverInfo |
| `tools/list` | `handle_list_tools` | 把应用的表单投影成工具的 `inputSchema` |
| `tools/call` | `handle_call_tool` | 参数翻译 → 调应用 → 结果翻译回 `CallToolResult` |
| `ping` | `handle_ping` | 返回空结果 |

**四条消息模型撑起协议：`Request` / `Notification` / `Result` 与 `JSONRPCError`。** `types.py` 的 1353 行几乎全是数据类定义，没有逻辑——它是一个**手写的协议类型库**，不是从 schema 生成的。

`Request` 与 `Notification` 都用 PEP 695 的泛型参数语法声明两个类型参数：

```
class Request[RequestParamsT: RequestParams | dict[str, Any] | None, MethodT: str](BaseModel):
    method: MethodT
    params: RequestParamsT
    model_config = ConfigDict(extra="allow")
```

**`MethodT: str` 这个约束是关键**：每个具体请求类型都用 `Literal["initialize"]`、`Literal["tools/list"]` 之类把 `method` 钉成一个字面量。于是**判别联合（discriminated union）不需要显式的判别字段**——pydantic 靠 `method` 的字面量就能选对模型。这比手写 `type: "xxx"` 判别标签更省，且与 JSON-RPC 的线格式天然一致（JSON-RPC 本来就有 `method`）。

`JSONRPCMessage` 是一个 `RootModel`，成员是四个候选的联合：

```
class JSONRPCMessage(RootModel[JSONRPCRequest | JSONRPCNotification | JSONRPCResponse | JSONRPCError]):
```

**用 `RootModel` 装联合，是 pydantic v2 里表达「JSON 顶层可以是多种形状之一」的标准做法**——普通 `BaseModel` 没法直接 `model_validate_json` 一个裸的联合类型。模块 docstring 里也把这条写成了规范：「Union types should be represented with a Pydantic `RootModel`」。

**三个版本常量并存，这是理解整个服务端行为的关键。**

```
LATEST_PROTOCOL_VERSION = "2025-11-25"              # 客户端向上游协商用的
SERVER_LATEST_PROTOCOL_VERSION = "2025-06-18"       # 服务端对外宣称的最新版
SERVER_SUPPORTED_PROTOCOL_VERSIONS = {"2024-11-05", "2025-03-26", "2025-06-18"}
DEFAULT_NEGOTIATED_VERSION = "2025-03-26"           # 客户端不发头时的假定版本
```

**客户端比服务端能说的版本更新**（11-25 vs 06-18）。这不是笔误，而是同一份 `types.py` 同时服务两个方向：dify 作为客户端要跟上上游最新版，作为服务端只承诺到 06-18。

`DEFAULT_NEGOTIATED_VERSION` 取了三个支持版本里的**中间那个**，而不是最新的 06-18。源码注释的说法是「Version assumed when a client omits the MCP-Protocol-Version header on post-initialize requests」——**对「不声明的客户端」假定一个较保守的版本，避免用它们没协商过的新特性**。

## 怎么实现的

### 版本协商：两处逻辑，规则不同

`negotiate_protocol_version(header_value, is_initialize)` 明确区分两种情形：

- **初始化请求**：永远返回 `DEFAULT_NEGOTIATED_VERSION`，**不校验也不拒绝头**。理由是初始化阶段的版本协商走消息体（`params.protocolVersion`），头还没有意义；
- **初始化之后**：头缺失或为空 → 用默认版本；头在支持集合里 → 用它；头不在集合里 → **返回 `None`**。

`None` 的语义由 docstring 交代：「the caller should reply with a JSON-RPC INVALID_REQUEST error」。**用返回值而非抛异常来表达「无效」，是为了让调用方能统一走 JSON-RPC 错误响应**——抛异常在协议层是不合适的，因为所有失败都必须是合法的 JSON-RPC 消息。

`handle_initialize` 里是另一套规则：

```
negotiated_version = SERVER_LATEST_PROTOCOL_VERSION
if isinstance(requested_version, str) and requested_version in SERVER_SUPPORTED_PROTOCOL_VERSIONS:
    negotiated_version = requested_version
```

**客户端要的版本若在支持集合里就照给，否则回服务端最新版**——这是 MCP 生命周期规范要求的「回退到自己的最新版」，而不是报错。所以两处协商的方向相反：一处「客户端版本不对就拒」，一处「不对就退回自己的」。**前者管的是「后续请求的行为开关」，后者管的是「握手时能不能配合」。**

能力声明只有一项：

```
capabilities = ServerCapabilities(tools=ToolsCapability(listChanged=False))
```

**`listChanged=False` 是诚实的**：工具列表由应用的输入表单决定，运行时不会变，所以不声明变更通知能力。这与 g04 里客户端「只在真正支持时才声明能力」是同一条纪律。

### 结构化输出：按协商版本开关

`_supports_structured_output(protocol_version)` 是一行：

```
return protocol_version >= STRUCTURED_OUTPUT_MIN_VERSION
```

注释解释了为什么字符串比较可行：「MCP protocol versions are YYYY-MM-DD strings, so lexical comparison equals chronological.」——**前提是日期格式严格固定**。这在 MCP 里成立，但作为通用做法很脆：任何带 `v` 前缀或补零差异的版本串都会破坏它。

这个开关影响两处，且**都是「新版本多给字段、老版本保持原样」**：

`tools/list` 里：

```
tool = Tool(
    name=app_name,
    title=app_name if supports_structured else None,
    description=description,
    inputSchema=parameter_schema,
    outputSchema={"type": "object"} if supports_structured else None,
)
```

注释写明：「Both stay None (and are stripped by exclude_none serialization) for older clients, so their tool definition is unchanged.」——**老客户端看到的工具定义与以前逐字节相同**。这是版本兼容的正确做法：不是「改字段」，而是「加字段且默认省略」。

`tools/call` 里：

```
structured_content = None
if _supports_structured_output(protocol_version):
    structured_content = extract_structured_output(app, response, answer)
```

`extract_structured_output` 按应用模式分流：

- `WORKFLOW` → 从 `response["data"]["outputs"]` 取原始输出映射（且用 `isinstance(..., Mapping)` 三重守卫），注释说明「structuredContent must be a JSON object」，所以最后 `dict(outputs)` 强制成具体 dict；
- `ADVANCED_CHAT | CHAT | AGENT_CHAT | COMPLETION` → 返回 `{"answer": answer}`；
- 其它 → `None`。

### 参数 schema：把可变表单投影成静态 schema

这是整个服务端最值得细看的一段，因为**输入表单是动态的、JSON Schema 是静态的，两者必须对齐**。

`build_parameter_schema` 按应用模式分两种形状：

- `COMPLETION` 与 `WORKFLOW` → **直接用表单参数**，`required` 就是表单里的必填项；
- 其它（聊天类）→ **额外加一个必填的 `query`** 字段，`required` 变成 `["query", *required]`。

`convert_input_form_to_parameters` 做具体投影，三个要点：

1. **跳过三种类型**：`FILE`、`FILE_LIST`、`EXTERNAL_DATA_TOOL` —— **文件与外部数据工具不进 MCP schema**。原因是 MCP 的工具参数是 JSON 值，而文件需要先上传到某个地方；这类参数在 MCP 通道上无法表达，只能不支持。
2. **描述从 `parameters_dict` 取，缺了就给空串**，且注释解释了为什么不报错：「if the workflow republished, the parameters not changed / we should not raise error here」——**工作流重新发布但参数没变时，描述字典可能还没有这个键**。这是「先发布表单、后填描述」这个流程顺序造成的可见中间态。
3. **类型映射是白名单 `if/elif` 链**：`TEXT_INPUT`/`PARAGRAPH`/`SELECT` → `string`（`SELECT` 还带 `enum`）、`NUMBER` → `number`、`CHECKBOX` → `boolean`、`JSON_OBJECT` → `object`（并把 `json_schema` 里的 `properties`/`required`/`additionalProperties` 三个键透传）。

**没有 `else` 分支**——遇到未列出的类型，参数会被创建成**只有 `description`、没有 `type`** 的 schema 项。这不是崩溃，而是产出 `{"description": "..."}`。**JSON Schema 里缺 `type` 是合法的（表示任意类型），所以这是「宽松降级」而不是错误**，但模型看到这样的参数会不知道填什么。

### 调用：三处翻译

`handle_call_tool` 只有四步，但每步都在做翻译：

1. `prepare_tool_arguments(app, arguments)` 按模式重排参数：

```
case AppMode.WORKFLOW:   return {"inputs": arguments}
case AppMode.COMPLETION: return {"query": "", "inputs": arguments}
case _:                  # 聊天类
    args_copy = arguments.copy()
    query = args_copy.pop("query", "")
    return {"query": query, "inputs": args_copy}
```

**MCP 的扁平参数被重新组织成 Dify 的 `{query, inputs}` 双层结构**。工作流没有 `query` 概念（所以不传），补全类没有独立对话输入（所以 `query` 给空串），聊天类才把 `query` 抽出来。注释特意说「create a copy to avoid modifying original dict」——**避免改调用方的字典**。

2. `AppGenerateService.generate(...)`，`invoke_from=InvokeFrom.SERVICE_API`、`streaming=(app.mode == AppMode.AGENT_CHAT)`。**只有 Agent 模式走流式**，其余一次性返回。
3. `extract_answer_from_response(app, response)` 把两种响应形态统一成字符串：
   - `RateLimitGenerator`（流式）→ 逐行扫 `data: ` 前缀的 SSE，只累加 `event == "agent_thought"` 的 `thought` 字段；
   - `Mapping`（一次性）→ 按模式取 `response["answer"]`，或工作上取 `json.dumps(response["data"]["outputs"])`。
4. 组装 `CallToolResult(content=[TextContent(text=answer)], structuredContent=...)`。

**第 3 步的流式解析只取 `agent_thought` 事件**，这是个有意的取舍：MCP 的工具返回是一个字符串，而 Agent 的流里有思考、有工具调用、有最终答案。它选了 `agent_thought`（agent 思考/输出），意味着**返回给 MCP 客户端的是 Agent 的推理文本拼接**，而不是结构化的事件序列。这符合「工具返回一段文本」的契约，但会丢掉「哪部分是最终答案」的信息。

`process_streaming_response` 里的 `json.JSONDecodeError` 只 `continue` 不报错——**单行解析失败不影响整条流**。

### 错误映射：三类异常到三个 JSON-RPC 错误码

```
except TriggerWorkflowServiceModeUnavailableError as e:
    return create_error_response(INVALID_REQUEST, str(e), data={"code": e.error_code})
except ValueError as e:
    logger.exception("Invalid params")
    return create_error_response(INVALID_PARAMS, str(e))
except Exception as e:
    logger.exception("Internal server error")
    return create_error_response(INTERNAL_ERROR, "Internal server error: " + str(e))
```

**三类的划分依据是「谁的错」**：业务模式不可用属于请求本身不适用（`INVALID_REQUEST`，还带业务错误码）、参数错误属于调用方（`INVALID_PARAMS`）、其余属于服务端（`INTERNAL_ERROR`）。

而**所有异常都被转成 JSON-RPC 错误消息，没有一个逃出去**——这是协议服务器的硬要求：任何失败都必须是合法的 JSON-RPC 响应，否则客户端无法解析。这与 HTTP 层「抛异常由框架转 500」的惯例相反。

**未知方法也走错误响应而不是报错**：

```
case _:
    return create_error_response(METHOD_NOT_FOUND, f"Method not found: {request_type.__name__}")
```

注意错误消息里给的是**Python 类名**（`request_type.__name__`）而不是 `method` 字符串。对调试者有用（能看出是哪个模型类没被处理），但客户端收到的是一个 Python 类名，略怪。

## 关键取舍

**源码事实：** 服务端只处理四个方法（`initialize`/`tools/list`/`tools/call`/`ping`），其余一律 `METHOD_NOT_FOUND`。

**源码事实：** 三个版本常量并存：客户端最新 `2025-11-25`、服务端最新 `2025-06-18`、默认协商 `2025-03-26`，服务端支持集合为 `{2024-11-05, 2025-03-26, 2025-06-18}`。

**源码事实：** `negotiate_protocol_version` 对初始化请求不校验头、对后续请求返回 `None` 表示「应回 INVALID_REQUEST」；`handle_initialize` 则「客户端版本不支持就退回服务端最新版」。

**源码事实：** 结构化输出按 `protocol_version >= "2025-06-18"` 用**字符串字典序**判定，注释声明 MCP 版本是 `YYYY-MM-DD` 所以可行。

**源码事实：** 老客户端不获得 `title` 与 `outputSchema` 字段（值为 `None` 并被 `exclude_none` 剥离），工具定义保持逐字节不变。

**源码事实：** `convert_input_form_to_parameters` 跳过 `FILE`/`FILE_LIST`/`EXTERNAL_DATA_TOOL`；描述缺失时给空串而不报错（工作流重发布的中间态）；类型映射是没有 `else` 的白名单链，未知类型只产出 `description`。

**源码事实：** `prepare_tool_arguments` 把 MCP 的扁平参数重排成 `{query, inputs}`，并对聊天类的字典做 copy 后 `pop("query")`。

**源码事实：** 流式响应的解析只累加 `event == "agent_thought"` 的 `thought`；单行 JSON 错误只 `continue`。

**源码事实：** 三类异常映射到三个 JSON-RPC 错误码，且全部转成错误响应而不是抛出。

**架构推论：** 「服务端只支持四个方法」是**把 MCP 当薄适配层**而不是当完整实现。MCP 规范里还有 resources、prompts、sampling、roots、logging 等一整套，这里一个都没实现——`ServerCapabilities` 里也只声明了 `tools`。**声明什么就只实现什么，是能力协商协议的正确姿势**；反之若声明了 resources 却在 `resources/list` 上返 METHOD_NOT_FOUND，客户端会反复重试。

**架构推论：** 三个版本常量并存说明**同一份类型库要同时服务「客户端」与「服务端」两个角色**，而两个角色的版本策略不同。把常量集中在一处（`types.py` 顶部）并逐条注释用途，是这个问题的正确解法；散落在各处则会互相覆盖。

**架构推论：** 「新版本只加字段、且默认省略」是版本兼容里最稳的手法。它让老客户端看到的字节流完全不变，所以兼容性可以被逐字节验证。相比之下「改字段语义」或「把可选字段变成必填」都会让老客户端静默出错。

**架构推论：** 「输入表单 → JSON Schema」这一步必然有损：文件类参数无法表达、未知类型退化成无类型、动态表单的具体值域无法静态化。**这是跨协议桥接的固有代价，不是实现缺陷**。值得做的是**把损失显式化**（如跳过时记日志），而不是假装无损。

**架构推论：** 把所有异常转成 JSON-RPC 错误是协议服务器的铁律，但**它同时也吞掉了堆栈的可观测性**：客户端只拿到一句 `Internal server error: ...`，真正的堆栈只在本进程日志里。所以这里每个 `except` 都先 `logger.exception(...)`——**在「不破坏协议」与「保留可诊断性」之间，用日志补上后者**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/mcp/server/streamable_http.py` | `handle_mcp_request` | 总入口：四方法分派与统一错误响应 |
| `api/core/mcp/server/streamable_http.py` | `handle_initialize` | 版本协商、能力声明、serverInfo |
| `api/core/mcp/server/streamable_http.py` | `handle_list_tools` | 把表单投影成工具定义，按版本加字段 |
| `api/core/mcp/server/streamable_http.py` | `handle_call_tool` | 参数重排 → 调应用 → 结果翻译 |
| `api/core/mcp/server/streamable_http.py` | `build_parameter_schema` | 按模式构造 `inputSchema`（聊天类含 `query`） |
| `api/core/mcp/server/streamable_http.py` | `convert_input_form_to_parameters` | 表单 → JSON Schema 的白名单投影 |
| `api/core/mcp/server/streamable_http.py` | `prepare_tool_arguments` | 扁平参数 → `{query, inputs}` |
| `api/core/mcp/server/streamable_http.py` | `extract_structured_output` | 按模式产出 `structuredContent` |
| `api/core/mcp/server/streamable_http.py` | `extract_answer_from_response` | 两种响应形态统一成字符串 |
| `api/core/mcp/server/streamable_http.py` | `process_streaming_response` | 流式响应里抽 `agent_thought` |
| `api/core/mcp/server/streamable_http.py` | `process_mapping_response` | 一次性响应按模式取答案 |
| `api/core/mcp/server/streamable_http.py` | `negotiate_protocol_version` | 后续请求的版本门禁 |
| `api/core/mcp/server/streamable_http.py` | `_supports_structured_output` | 字符串字典序的版本判定 |
| `api/core/mcp/types.py` | `Request` | 泛型请求基类（`method` 由字面量钉死） |
| `api/core/mcp/types.py` | `Notification` | 泛型通知基类 |
| `api/core/mcp/types.py` | `Result` | 泛型结果基类（带 `_meta` 别名） |
| `api/core/mcp/types.py` | `JSONRPCMessage` | 四候选联合的 `RootModel` |
| `api/core/mcp/types.py` | `SERVER_SUPPORTED_PROTOCOL_VERSIONS` | 服务端可协商的版本集合 |
| `api/core/mcp/types.py` | `CallToolResult` | 工具调用结果（含 `structuredContent`） |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/mcp/server/streamable_http.py](https://github.com/langgenius/dify/blob/main/api/core/mcp/server/streamable_http.py)
- [MCP 规范：生命周期与版本协商](https://modelcontextprotocol.io/specification/2025-06-18/basic/lifecycle)
- [MCP 规范：工具](https://modelcontextprotocol.io/specification/2025-06-18/server/tools)

## 与主核 / AI 的关系

这个服务端与主核里的**协议前端（frontend/backend protocol）**是同一层：把内部的执行引擎包装成一个对外稳定的线协议。数据库后端要处理「客户端版本协商 → 参数解析 → 执行 → 结果序列化 → 错误码映射」，与这里的四步一一对应。

**版本协商的两种方向 vs 数据库的 `protocol_version` 处理。** PostgreSQL 的启动包里客户端声明 `protocol 3.0`，服务端若不支持就断开（**拒**）。这里的 `handle_initialize` 则是「客户端要的不支持就退回自己的最新版」（**顺从**）。两者的差异在于协议是否允许降级：MCP 明确允许（生命周期规范要求回退），PG 不允许。**同一种「版本不匹配」在不同协议里必须有不同处置，不能照搬。**

**`RootModel` 装联合 vs 数据库的报文类型分派。** PG 的报文靠第一个字节区分类型（`Q` = Query、`P` = Parse…），MCP 的 JSON-RPC 靠 `method` 字段。两者都是「先用一个判别字段选分支，再解析剩余部分」。pydantic 的字面量联合把这个过程自动化了——**判别字段来自协议本身（`method`）时，不需要再发明一个 `type`**。

**「新版本加字段」vs 数据库的 `RowDescription` 扩展。** PG 加新字段描述时也是「追加、老客户端按已知字段数读、多余忽略」。**向后兼容的本质是「老客户端能安全忽略新内容」**，所以兼容性设计的第一原则是：新增内容必须能被忽略。

**「输入表单 → JSON Schema」vs 数据库的外部表（FDW）映射。** FDW 要把外部数据源的任意结构映射成 PG 的行类型，同样面临「有些东西表达不了」的问题（不支持的类型只能当 text）。**跨协议的桥接必然有损**，关键是把损失显式化而不是静默吞掉。

对 agent 工程四条结论。其一：**只声明你真正实现的能力**，`ServerCapabilities` 里写 `tools` 就别去管 `resources`。其二：**新版本只加字段并默认省略**，兼容性才能逐字节验证。其三：**协议服务器要把所有异常翻成协议层错误**，同时用日志保留堆栈——两者缺一都会出问题。其四：**桥接层的损失要显式化**：文件类参数不支持、未知类型无 `type`，这些都该有日志或文档，否则使用者会以为参数被正常传递了。
