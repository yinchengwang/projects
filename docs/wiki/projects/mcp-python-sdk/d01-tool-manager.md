---
project: mcp-python-sdk
feature: tool-manager
title: 工具注册与调用参数校验
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/server/mcpserver/tools/tool_manager.py
    symbols: [ToolManager]
  - path: src/mcp/server/mcpserver/tools/base.py
    symbols: [Tool]
  - path: src/mcp/server/mcpserver/utilities/func_metadata.py
    symbols: [FuncMetadata, func_metadata]
---

# 工具注册与调用参数校验

## 一句话职责

`ToolManager` 只是一张名字到 `Tool` 的表，真正的工作在两端：注册时 `Tool.from_function` 把函数签名编译成 Pydantic 参数模型与 JSON schema，调用时 `Tool.run` 按「谁的错误」把异常分成两类——模型能读懂的参数错误，与只应留在服务端的崩溃。

## 机制是什么

三层的职责划分很清楚：

| 层 | 位置 | 职责 |
|---|---|---|
| 注册表 | `ToolManager` | 名字 → `Tool` 字典，重复名只警告不覆盖 |
| 编译 | `Tool.from_function` + `func_metadata` | 签名 → Pydantic 参数模型 + 上线的 JSON schema |
| 执行 | `Tool.run` + `FuncMetadata` | 校验、注入 context/解析器结果、调用、转换结果、分类错误 |

`Tool` 是 `pydantic.BaseModel`，但它的字段几乎全是注册期算好的派生数据：`parameters`（上线 schema）、`fn_metadata`（参数模型 + 输出 schema）、`is_async`、`context_kwarg`、`resolved_params`、`resolver_plans`。其中 `fn`、`resolved_params`、`resolver_plans` 标了 `exclude=True`——它们是本地执行用的，不该出现在对外序列化里。

一个值得注意的取向：**注册期尽量把能在注册期发现的问题挑出来**。`func_metadata` 的注释写明「`FuncMetadata` 在构造时就建校验器与 schema，所以不支持的返回类型在注册时暴露，而不是等到第一次调用」。

## 怎么实现的

**注册表本身几乎不做事。** `ToolManager.add_tool` 先 `Tool.from_function(...)`，再查重名：已有同名工具时打一条 `logger.warning`（若开关打开）并**返回已存在的那个**，不覆盖。构造函数里对初始 `tools` 列表的处理更简单——直接赋值覆盖，只在重名时警告。这两条路径行为不一致，但都是在「注册」这个宽松时刻刻意选择不抛错。

`remove_tool` 则是严格的：名字不存在就抛 `ToolError(f"Unknown tool: {name}")`。`call_tool` 同样，名字不存在抛 `ToolError`，否则转给 `tool.run(...)`。

**`from_function` 的六道注册期检查。** 按代码顺序：

1. `validate_and_warn_tool_name(func_name)` —— 工具名合法性（协议对名字有字符集要求，此处只警告）。
2. 名字为 `<lambda>` 时直接报错，要求显式给 `name`——lambda 没有可用名字。
3. 描述取 `description` 参数，回落到 `fn.__doc__`，再回落到空串。
4. `find_context_parameter(fn)` 找带 `Context` 注解的参数（详见 d03）。
5. `find_resolved_parameters(fn)` 找 `Annotated[_, Resolve(...)]` 参数；若同时又 `returns_input_required(fn)`，抛 `InvalidSignature`。理由写得很具体：一次调用只有一条 `input_required` 通道，多轮流程要么由解析器驱动、要么由工具体驱动，不能两者并用。
6. `func_metadata(fn, skip_names=[context_kwarg, *resolved_params], structured_output=...)` —— 注意被跳过的正是第 4、5 步找到的参数。它们**不出现在对外的参数 schema 里**，因为客户端不该为它们传值。

拿到参数模型后还有两步：`model_json_schema(by_alias=True, schema_generator=UntitledJsonSchema)` 生成 schema，并 `parameters.pop("title")`——注释说参数模型是动态生成的，它的类名标题没有信息量；然后 `find_invalid_x_mcp_header(parameters)` 检查 `x-mcp-header` 注解是否合法（这是 MCP 对参数标注的扩展，写错了在注册期报 `InvalidSignature`）。

最后 `build_resolver_plans(resolved_params, tool_arg_names)` 静态分析解析器 DAG（详见 d04）。这里有个细节：`tool_arg_names` 取的是 `field.alias or name`，注释解释了为什么必须对齐别名——`model_dump_one_level` 按别名输出，解析器「按名取工具参数」时用的键必须与那一刻真实存在的键一致。

**`func_metadata` 里几处不显然的处理。**

- 参数名以 `_` 开头直接抛 `InvalidSignature`（Pydantic 私有字段语义冲突）。
- 参数名与 `BaseModel` 的可调用属性冲突（如 `dict`、`json`、`copy`）时，字段真名改成 `field_{name}` 并设 `alias=原名`。这是为了绕开 Pydantic 对父类属性遮蔽的警告，同时保证对外 schema 里的属性名仍是用户写的那个。
- 无注解参数不是报错，而是注入 `WithJsonSchema({"type": "string"})`，即当作字符串。这是一个宽松默认——目的是让没写注解的函数也能注册。
- 参数模型的基类是 `ArgModelBase`，它提供一个 `model_dump_one_level()`：只展开一层，子模型保持为 Pydantic 实例而非 dict。这是为了让工具函数收到真正的模型对象。

**`structured_output` 的三态与返回类型推导。** `func_metadata` 的后半段几乎全在处理返回类型：

- `False` → 立刻返回只有参数模型的 `FuncMetadata`，不碰返回值。
- `True` 但返回值无注解 → `InvalidSignature`。
- 返回注解只有 `InputRequiredResult` → 无结构化输出（它只表达控制流）。
- 返回注解是含 `InputRequiredResult` 的联合 → **把那一支剥掉**，用剩余分支推导 schema。注释写明理由：`InputRequiredResult` 是控制流信号而非数据，`convert_result` 会在输出校验之前短路它，所以 schema 只会看到数据分支。
- 返回注解是 `CallToolResult`：若带 `Annotated` 元数据，第一个元数据被当作真实返回类型，其余保留；不带则视为「不做校验」，直接返回纯参数模型。
- `structured_output is None`（自动探测）且返回类型是裸 `list`/`tuple` 或内容块（`TextContent`、`EmbeddedResource`、`Image`、`Audio` 等）时，也退回非结构化。注释给的理由很实在：这些是「模型读的东西」而不是「应用要的数据」，推导出的 schema 会把内容块自己的模型当成 outputSchema 公布出去。
- 其余情况走 `_create_output_model`，成功则构造带 `output_model` 的 `FuncMetadata`（schema 在此刻经 `StrictJsonSchema` 生成，遇到不可序列化类型**抛错而非警告**）。捕获的异常类型列了七种，注释逐个解释了每种来自哪里；捕获后若 `structured_output is True` 就抛 `InvalidSignature`，否则安静退回非结构化。

**`Tool.run` 的两段式错误分类是这一页的核心。** 它把「校验阶段」与「执行阶段」分开，各用一套分类：

校验阶段：
- `ValidationError` → `ToolError(f"Error executing tool {name}: {exc}")`。注释写明意图：这是**模型读得懂并会去改正**的输入不匹配，所以按「故意的工具错误」上报，把校验信息带给模型。
- `MCPError` → 原样抛出（协议层错误）。
- 其他任何异常 → `UnexpectedToolError(f"Error executing tool {name}")`，**不带原始文本**。理由是自定义校验器或 `default_factory` 抛错属于崩溃。

执行阶段（顺序很重要）：
- `MCPError` → 原样抛出。docstring 解释了为什么这不是 `isError=True` 的结果：它携带 JSON-RPC `ErrorData(code, message, data)`，语义是「回一个协议错误」，应由内核当作顶层 JSON-RPC 错误上报。
- `UnexpectedToolError` / `UnexpectedResourceError` → 包成新的 `UnexpectedToolError` 并**带上内层消息**。注释给了理由：嵌套调用崩溃时它自己的消息已经是泛化过的，所以可以安全转发。
- `ToolError` / `ResourceError` → 包成 `ToolError` 并带上内层消息（这些是作者故意抛的，值得让模型看到）。
- 其他任何异常 → `UnexpectedToolError`，**丢弃原始文本**。「崩溃的异常自己的文本留在服务端」。

这套分类的判据不是「异常类型好不好看」，而是**这段文字该不该给模型看**。作者故意抛的（`ToolError`/`ResourceError`）可以看，参数校验失败的可以看（模型要据此改正），其他一律不给。

**执行路径上的注入顺序。** `pass_directly` 先放 context（若有 `context_kwarg`）；解析器结果在有 `resolved_params` 时算出并合并进去。注释强调解析器看到的是**与工具体完全相同的已校验参数**——这样 `default_factory` 或有状态校验器不会给解析器和工具体交出两个不同的值。

解析器返回 `InputRequiredResult` 时，工具体**不执行**，直接把批量问题作为本轮的返回（可选经 `convert_result`）。另有一条运行期兜底：若工具有 `resolved_params` 而工具体自己返回了 `InputRequiredResult`，抛 `RuntimeError`——这是作者错误，按崩溃处理而不是 `ToolError`。

**结果转换。** `convert_result` 同时产出两样东西：`content`（非结构化，由 `_convert_to_content` 从返回值生成）与 `structured_content`（仅当有 `output_model`）。它的 docstring 交代了一处刻意的重复：低层服务端已提供结构化的通用序列化，但 `MCPServer` 需要保留自己那套「从返回值构造非结构化内容」的临时逻辑以维持向后兼容。

还有一层兼容现实：`pre_parse_json`。它在正式 `model_validate` 之前，把「本该是列表/对象却被塞进 JSON 字符串」的值先解析出来。注释点名了来源：Claude Desktop 会把 `["a","b","c"]` 当字符串传，子模型也常以 JSON 字符串形式传对象。三类值被排除在预解析之外——解析失败/被拒的、以及解析后是标量的（因为 `"hello"` 解析成 JSON 会变成 `hello`，反而错）。

## 关键取舍

**源码事实：** `ToolManager` 只是字典封装，重复注册只警告并返回已存在的工具，不覆盖。`remove_tool` / `call_tool` 遇到未知名字才抛 `ToolError`。

**源码事实：** context 参数与 `Resolve(...)` 参数在注册期就被算出来并从对外参数 schema 里排除，客户端无法也不该为它们传值。

**源码事实：** `Resolve(...)` 参数与「工具体自己返回 `InputRequiredResult`」互斥，注册期抛 `InvalidSignature`，运行期还有一条兜底 `RuntimeError`。

**源码事实：** 错误分类的依据是「该不该让模型看见这段文本」：`ToolError`/`ResourceError` 与参数校验失败带文本，其余崩溃一律只给泛化消息，原始异常留在服务端。

**源码事实：** 无注解的参数不会报错，而是被当作 `string` 处理。

**源码事实：** 结构化输出在注册期就建校验器与 schema，不可序列化的返回类型在注册时暴露；自动探测模式下遇到裸 `list`/`tuple` 或内容块会刻意退回非结构化。

**架构推论：** 把「编译」放在注册期、把「校验+调用」放在调用期，是这类框架的必然选择：注册期可以付出较高的一次性成本（一次 `inspect.signature`、一次 Pydantic 模型构造），调用期必须是廉价的（一次 `model_validate` + 一次函数调用）。`FuncMetadata` 的 `_adapter` 私有属性缓存就是这条原则的体现——输出校验器只建一次，字段被重新赋值时才重建。

**架构推论：** 「错误文本给不给模型」这条判据，实际上是这个框架对 **prompt 注入面**的一次收窄。异常消息会直接进入下一轮模型输入，若把内部堆栈或文件路径带进去，等于把服务端实现细节泄给了模型（并可能被它复述给用户）。`UnexpectedToolError` 不携带原文，是为了让「模型可见的错误面」只剩作者明确表达过的那一小块。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/server/mcpserver/tools/tool_manager.py` | `ToolManager` | 名字到 `Tool` 的注册表，含增删查与 `call_tool` 转发 |
| `src/mcp/server/mcpserver/tools/base.py` | `Tool` | 注册期编译结果载体 + `run` 的两段式错误分类 |
| `src/mcp/server/mcpserver/tools/base.py` | `Tool.from_function` | 六道注册期检查、参数 schema 生成、解析器计划构建 |
| `src/mcp/server/mcpserver/tools/base.py` | `Tool.run` | 校验 → 注入 → 调用 → 转换，四类异常分派 |
| `src/mcp/server/mcpserver/utilities/func_metadata.py` | `FuncMetadata` | 参数模型 + 输出 schema/模型，含 `validate_arguments`、`call_fn`、`convert_result` |
| `src/mcp/server/mcpserver/utilities/func_metadata.py` | `func_metadata` | 签名 → 动态 Pydantic 模型；`structured_output` 三态与返回类型推导 |
| `src/mcp/server/mcpserver/utilities/func_metadata.py` | `ArgModelBase` | 只展开一层的 `model_dump_one_level`，别名映射的依据 |
| `src/mcp/server/mcpserver/utilities/func_metadata.py` | `func_metadata.pre_parse_json` | 应对客户端把列表/对象塞进 JSON 字符串的兼容层 |

## 参考

- [mcp-python-sdk 源码：tool_manager.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/mcpserver/tools/tool_manager.py)
- [mcp-python-sdk 源码：tools/base.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/mcpserver/tools/base.py)
- [MCP 规范：工具](https://modelcontextprotocol.io/specification/2025-06-18/server/tools)
- [Pydantic 文档：动态模型创建](https://docs.pydantic.dev/latest/concepts/models/#dynamic-model-creation)

## 与主核 / AI 的关系

「注册期编译、调用期校验」这条分工与主核的**预处理语句**是同构的：`PREPARE` 时解析、绑定类型、生成计划（一次性的重活），`EXECUTE` 时只做参数绑定与执行（廉价的轻活）。两边都把「能提前发现的问题提前」当作硬要求，因为调用期出错意味着要么中断、要么把错误交给某个上层去猜。

更值得对照的是错误分类那条判据。主核里也有一条类似的线：**哪些错误消息可以返给客户端**。数据库会把语法错误、约束冲突的具体内容告诉客户端（用户要据此改写 SQL），但不会把内部断言失败、段错误的细节返回——那些只进服务端日志。`Tool.run` 用的是同一个原则，只是「客户端」换成了「模型」，而模型比人类客户端更需要精确的纠正线索（它没有别的信息来源），所以参数校验失败的消息必须带上。

`pre_parse_json` 这类兼容层在主核里也有对应物：接受 `'2024-01-01'` 这样的字符串字面量并隐式转换成日期。两者都是「容忍客户端常见的错误形状」而不是拒绝，代价是校验逻辑变复杂、且真实类型错误可能被掩盖。这里的三条排除规则（解析失败的、解析成标量的）说明作者清楚这条路的边界。

对 AI 工程的实际意义在于：工具层的质量差异几乎全部体现在这两处——**注册期检查是否充分**、**错误文本是否分得清该给谁看**。前者决定「作者什么时候发现自己写错了函数」，后者决定「模型收到错误后能不能改对」。这份实现还给出一个具体做法：把「模型可见的错误」与「只进日志的崩溃」用两个不同的异常类型（`ToolError` vs `UnexpectedToolError`）在类型层面分开，而不是靠调用点约定。
