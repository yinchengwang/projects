---
project: mcp-python-sdk
feature: resources-and-prompts
title: 资源、模板与提示
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/server/mcpserver/resources/resource_manager.py
    symbols: [ResourceManager]
  - path: src/mcp/server/mcpserver/prompts/manager.py
    symbols: [PromptManager]
  - path: src/mcp/server/mcpserver/resources/templates.py
    symbols: [ResourceTemplate]
---

# 资源、模板与提示

## 一句话职责

MCP 服务端有三类可对外暴露的东西：工具（有参数、有返回值）、资源（按 URI 读）、提示（有参数、渲染成消息）。这一页讲后两类的管理器与资源模板——它们的共同点是**都不产生结构化输出**，却不约而同地在「匹配」和「渲染」之间塞进了一层能被安全策略拦截的关口。

## 机制是什么

三个管理器的形态差异很大，反映的是三者对「键」的定义不同：

| 管理器 | 键 | 查找语义 |
|---|---|---|
| `ToolManager` | 工具名 | 精确匹配，命中即调用 |
| `ResourceManager` | URI 字符串 | **先精确匹配具体资源，再逐个试模板** |
| `PromptManager` | 提示名 | 精确匹配，参数在渲染期校验 |

`ResourceManager` 维护两张表：`_resources`（URI → `Resource`）与 `_templates`（模板串 → `ResourceTemplate`）。`PromptManager` 只有一张表，`arguments` 声明与必填校验都落在 `Prompt` 自己身上。

三者共享同一个注入点与同一套错误包装风格——`find_context_parameter` 找出 `Context` 参数、`inject_context` 在调用前塞进去；失败一律包成该域自己的错误类型（`ResourceError` / `UnexpectedResourceError` / `MCPError(INVALID_PARAMS)`）。

## 怎么实现的

**`ResourceManager.get_resource` 的两段查找，顺序就是安全语义。** 先查 `_resources`，命中直接返回；否则**按注册顺序**遍历所有模板，逐个 `template.matches(uri_str)`，第一个返回非 `None` 的就用它创建资源。若都不匹配，抛 `ResourceNotFoundError(f"Unknown resource: {uri}")`。

这里有一处关键设计：`matches` 在安全校验失败时**抛 `ResourceSecurityError` 而不是返回 `None`**，而 `get_resource` 把那个异常转成 `ResourceNotFoundError`（`from e` 保留因果）。`ResourceSecurityError` 的 docstring 解释了为什么必须抛：如果返回 `None`，管理器会继续试下一个模板，而下一个模板可能更宽松，于是「被更严格模板拒绝的 URI」会被更宽松的模板接走。**抛错 = 停止迭代**，这是把「第一个匹配的模板说了算」这条语义贯彻到安全判断上。

`get_resource` 的 docstring 还主动交代了一个陷阱：Pydantic 的 `AnyUrl` 在校验时会规范化百分号编码并解析 `..`，所以 `AnyUrl("file:///a/%2E%2E/b")` 到这里时已经是 `file:///b`。也就是说**安全校验看到的可能是已被规范化的形式**。它明确区分了两种情况：JSON-RPC 协议层传的是原始 `str`（不受影响），而内部调用方若自己包了 `AnyUrl` 就要当心。

**`add_resource` 与 `add_template` 的重复策略不同。** `add_resource` 按 `str(resource.uri)` 查重，重名时警告并**返回已存在的那个**（不覆盖）。`add_template` 则直接 `self._templates[template.uri_template] = template`——**静默覆盖**。这是一处不一致：资源不覆盖、模板覆盖。后果是后注册的模板会顶掉先注册的同模板串模板。

**`ResourceTemplate` 是这一页最重的部分。** 它把「URI 模板 + 处理函数」编译成一份可在运行期反复匹配的对象，字段分两类：

- 对外的：`uri_template`、`name`、`title`、`description`、`mime_type`、`icons`、`annotations`、`meta`、`parameters`（函数的参数 JSON schema）。
- 本地的（`exclude=True`）：`fn`、`parsed_template`（解析好的 RFC 6570 模板）、`security`（路径安全策略）、`context_kwarg`。

`from_function` 的流程：`UriTemplate.parse` 解析模板串（格式错误抛 `InvalidUriTemplate`）→ `find_context_parameter` 找注入参数 → `func_metadata(fn, skip_names=[context_kwarg], structured_output=False)` 只取参数模型（**资源没有输出 schema**，注释点明了这一点）→ 用 `validate_call(fn)` 把函数包一层以获得运行期参数类型转换。

注意它**不把 `parameters` 的 `title` 去掉**（工具那边去掉了），也不做 `x-mcp-header` 校验——资源参数不走工具那套注解扩展。

**`matches` 是安全策略的唯一落点。** 它先 `parsed_template.match(uri)`（RFC 6570 提取），`None` 就直接返回 `None`；提取成功后再 `self.security.validate(params)`，返回失败的参数名就抛 `ResourceSecurityError(self.uri_template, failed)`。注释说明安全校验在**提取并解码之后**执行，因此无论值在 URI 里是字面量、`%2F`、`%5C` 还是 `%2E%2E`，看到的都是解码后的形态。

`ResourceSecurity` 是 `frozen dataclass`，四个开关全默认开启：`reject_path_traversal`、`reject_absolute_paths`、`reject_null_bytes`，外加 `exempt_params` 白名单。`validate` 对每个非豁免参数逐个值检查（列表值逐元素，因为 RFC 6570 的 explode 变量会产出列表），返回**第一个**失败参数名。null 字节那条的注释写得很具体：它能让字符串比较失效（`"..\x00" != ".."`），并可能在 C 扩展或子进程调用处造成截断。

`DEFAULT_RESOURCE_SECURITY` 是一个全开实例，且 docstring 自称「secure-by-default」。

**`create_resource` 的调用与错误包装。** 它先 `inject_context`，再按 `is_async_callable` 决定 `await fn(...)` 还是 `anyio.to_thread.run_sync(partial(...))`。返回值若是 `InputRequiredResult` 就原样透传（2026-07-28 的多轮流程），否则包成 `FunctionResource`——注意 `fn=lambda: result`，把已经算出的结果用闭包捕获，这样列表资源时不会重复执行函数。异常处理只有两档：`(ResourceError, MCPError)` 原样抛出，其他一律 `UnexpectedResourceError(f"Error creating resource from template {uri}")`，**不带原始文本**。这与工具那边「该不该给模型看」的判据一致。

**`PromptManager` 几乎是透明的。** 它只有增删查与 `render_prompt`；`render_prompt` 找不到名字就抛 `MCPError(code=INVALID_PARAMS, message=f"Unknown prompt: {name}")`，其余全部委托给 `prompt.render(...)`。用 `INVALID_PARAMS` 而不是自定义错误，是因为提示名来自客户端请求参数，协议层已有对应的错误码。

**`Prompt.render` 承担了所有实质工作。** 分四步：

1. **必填校验前置**。从 `self.arguments` 里取 `required` 的集合，与 `arguments or {}` 的键集合求差，有缺就抛 `MCPError(INVALID_PARAMS)`。注意这一步发生在调用函数之前，所以缺参数不会进用户代码。
2. **注入与调用**。同资源：`inject_context` + 异步/线程分流。
3. **`InputRequiredResult` 透传**。
4. **结果归一成消息列表**。不是 `list`/`tuple` 就包成单元素列表，然后逐个转换：`Message` 原样；`dict` 交给 `message_validator`；裸的 `str`/`ContentBlock`/`Image`/`Audio` 包成 `UserMessage`；其他类型（带 `pragma: no cover`）用 `pydantic_core.to_json(..., fallback=str)` 序列化成文本再当用户消息。

`message_validator` 有一个值得注意的细节：它是 `TypeAdapter(Annotated[UserMessage | AssistantMessage, Field(union_mode="left_to_right")])`。注释解释了为什么必须 `left_to_right`：两个类都接受任意 role，所以第一个分支总能匹配；若让 Pydantic 试两个分支，会对内容**验证两次**，而 path-backed 的 `Image`/`Audio` 在转换时会**读文件**——验证两遍就读两遍。

`Message.__init__` 里那三种转换（`str` → `TextContent`、`Image` → `ImageContent`、`Audio` → `AudioContent`）是「宽进」的入口：作者可以只写 `UserMessage("hello")`，不必手搓内容块。

## 关键取舍

**源码事实：** `ResourceManager.get_resource` 先精确匹配、后按注册顺序试模板，第一个匹配者胜。安全校验失败时抛异常以**阻止**回退到更宽松的模板。

**源码事实：** 模板重名静默覆盖，资源重名警告且保留旧的。两个管理器在同一件事上策略相反。

**源码事实：** `ResourceSecurity` 默认全开（穿越、绝对路径、null 字节），并允许按参数名豁免；检查在 RFC 6570 提取并解码之后执行。

**源码事实：** 资源与提示都没有输出 schema——`func_metadata` 一律传 `structured_output=False`。

**源码事实：** 资源创建与提示渲染的异常都只分两档：本域的显式错误原样抛，其他一律包成泛化错误且不携带原始文本。

**源码事实：** `message_validator` 用 `left_to_right` 联合模式，避免对内容验证两次（以及避免读两次文件）。

**架构推论：** 「先精确、后模板」这个查找顺序把「具体资源优先于模式匹配」写进了语义。反过来（模板优先）会让一个宽泛的模板遮蔽掉精确注册的资源，这类遮蔽在配置复杂时极难排查。

**架构推论：** 安全校验放在「匹配成功之后、创建资源之前」而不是「读取文件之前」，是一个经过权衡的位置：放得更早（在模板解析前）就无从知道参数是什么；放得更晚（在用户函数里）就等于把责任推给每个作者。这个位置也解释了为什么它必须抛错而非返回 `None`——它是在一个迭代过程中间，唯一的「停止」手段就是异常。

**架构推论：** 提示的必填校验在调用函数之前做，而参数的**类型**校验靠 `validate_call(fn)` 在调用时做。两层校验来自两个不同的机制，说明这套代码没有为提示单独实现一套参数系统，而是复用了 `func_metadata` 与 `validate_call` 两个既有工具。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/server/mcpserver/resources/resource_manager.py` | `ResourceManager` | 资源与模板两张表，两段查找与错误转换 |
| `src/mcp/server/mcpserver/resources/resource_manager.py` | `ResourceManager.get_resource` | 先精确后模板；`ResourceSecurityError` → `ResourceNotFoundError` |
| `src/mcp/server/mcpserver/resources/templates.py` | `ResourceTemplate` | URI 模板编译结果，承载 `matches` 与 `create_resource` |
| `src/mcp/server/mcpserver/resources/templates.py` | `ResourceTemplate.matches` | RFC 6570 提取 + 安全策略，失败抛错以停止迭代 |
| `src/mcp/server/mcpserver/resources/templates.py` | `ResourceSecurity` | 四个开关 + 参数豁免的路径安全策略 |
| `src/mcp/server/mcpserver/resources/templates.py` | `ResourceSecurityError` | 区分「不匹配」与「因安全被拒」的异常 |
| `src/mcp/server/mcpserver/prompts/manager.py` | `PromptManager` | 提示注册表与 `render_prompt` 转发 |
| `src/mcp/server/mcpserver/prompts/base.py` | `Prompt.render` | 必填校验、注入、调用、结果归一成消息列表 |
| `src/mcp/server/mcpserver/prompts/base.py` | `message_validator` | `left_to_right` 联合模式，避免重复验证与重复读文件 |

## 参考

- [mcp-python-sdk 源码：resources/resource_manager.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/mcpserver/resources/resource_manager.py)
- [mcp-python-sdk 源码：resources/templates.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/mcpserver/resources/templates.py)
- [MCP 规范：资源](https://modelcontextprotocol.io/specification/2025-06-18/server/resources)
- [MCP 规范：提示](https://modelcontextprotocol.io/specification/2025-06-18/server/prompts)
- [RFC 6570：URI 模板](https://www.rfc-editor.org/rfc/rfc6570)

## 与主核 / AI 的关系

「先精确、后模板」这套两级查找，与主核里**视图与表名的解析**是同一种结构：先找与名字完全匹配的关系，再考虑通过规则（例如搜索路径中的模式）解析。两边都把「具体优先于泛化」当作不可让渡的语义，因为反过来会产生遮蔽——而遮蔽类 bug 的特征是「配置里明明有，却用到了另一个」。

安全校验的位置更值得对照。主核里同类决定是「行级安全策略在什么时候求值」：不能太早（此时还不知道行内容），不能太晚（此时数据已经流出）。这里是「匹配之后、创建之前」，理由完全一样。而「抛异常而不是返回失败」也有对应物——主核的策略过滤器不会「返回 false 然后继续试下一个策略」，因为那会让更宽松的策略接管。

`message_validator` 那条注释是这页最实用的细节之一：联合类型的分支若都有副作用（这里副作用是读文件），试两遍就会做两遍。主核里有类似陷阱——类型转换函数被调用两次。这类问题只在「验证/转换有副作用」时显形，而 `Image`/`Audio` 的 path-backed 形态恰好把验证变成了 IO。

对 AI 工程的实际意义在于：资源与提示常被当作「比工具简单的附属物」，但这份实现显示它们的复杂度集中在两处不容简化的地方——**URI 模板的匹配与安全**、以及**渲染结果的归一化**。前者因为输入完全由客户端控制，必须假设恶意；后者因为作者返回什么都有可能（字符串、内容块、模型对象、字典、序列），必须有一个明确的转换阶梯，且那个阶梯里的每一步都要能说清「这个值最终会变成什么样的消息」。
