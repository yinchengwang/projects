---
project: openai-agents
feature: tool-invocation
title: 工具调用与参数解析
page_type: feature
schema_version: 1
refs:
  - path: src/agents/tool.py
    symbols: [with_function_tool_failure_error_handler, get_function_tool_origin]
  - path: src/agents/util/_approvals.py
    symbols: [parse_function_tool_arguments]
---

# 工具调用与参数解析

## 一句话职责

把模型给出的一串 JSON 字符串变成一次真正的 Python 调用，并在中间插入参数校验、上下文注入、超时、失败兜底与旁路元数据——整个过程对失败的默认态度是「变成给模型看的文本」，而不是让整个运行崩掉。

## 机制是什么

调用链的入口是 `FunctionTool.on_invoke_tool(ctx, input)`，签名固定为「工具上下文 + JSON 字符串」。真实实现由 `function_tool` 装饰器生成，包在一个 `_FailureHandlingFunctionToolInvoker` 里；后者负责「异常 → 交给失败文案函数 → 若它返回文本则作为工具输出，否则原样重抛」。

调用前需要的参数有三条来源，按优先级：上下文里预置的 `PreparedFunctionArguments`（审批阶段已经准备好的那份）、或现场用 `_prepare_arguments` 解析。参数解析本身是 `_parse_function_tool_json_input`（JSON 解码 + 必须是对象）+ pydantic 模型校验 + `FuncSchema.to_call_args`（还原成 `(args, kwargs)`）。

超时由 `FunctionTool.timeout_seconds` 控制，行为由 `timeout_behavior` 二选一：`error_as_result`（默认，超时变成一条模型可见的文案）或 `raise_exception`（抛 `ToolTimeoutError` 让运行失败）。

审批策略看到的参数走另一条更保守的解析路径：`parse_function_tool_arguments`。

## 怎么实现的

**「准备好的参数」的复用与校验。** `_on_invoke_tool_impl` 开头从 `ctx._function_tool_arguments` 取出预置参数并立刻置 `None`（一次性消费）。若有预置值，它必须满足两个条件：`owner` 是同一个 `_prepare_arguments` 闭包、`arguments` 字符串与本次 `input` 完全一致；任一不满足就 `UserError("Prepared function arguments do not match this invocation.")`。没有预置值时才走正常的 `_prepare_arguments`。

这条路径的存在意义是：审批判定已经解析并校验过一次参数，如果放行后重新解析，模型给的内容有机会在两次之间变化（或者解析带有副作用）。复用同一份解析结果，使「审批时看到的就是执行时用的」。

**审批阶段为何可能没有预置参数。** `_prepare_for_approval` 先用 `can_prepare_without_user_code(schema.params_pydantic_model)` 判断这个参数模型是否「纯数据校验」：校验器必须正好是 pydantic 的 `SchemaValidator`（被插件包过就不算），core schema 里不能有 `custom_init`/`post_init`，且逐层检查每个字段的 schema 类型——`str` 带 `pattern` 的不算（回溯正则是可执行代码）、`default` 的工厂只允许 `list`/`dict` 这两个内建、模型字段不能带 `extras_schema`。不满足就返回一个空壳 `PreparedFunctionArguments`（`args=[]`、`kwargs={}`），让执行阶段自己重新解析；`_prepare_arguments` 抛异常时也退回空壳。

`arguments_unchanged` 是这套判断的最后一道：把校验后的实例字段按别名投影成纯 JSON 值，与原始 JSON 逐字段比较；浮点比较用 `float.hex()`，`root=True` 时忽略字典键序、嵌套字典则必须保持键序（因为那关系到 Python 调用时的绑定顺序）。`unchanged=False` 时审批逻辑会放弃「用原始参数做策略判定」，改为要求人工审批。

**JSON 解析的两种失败模式。** `_parse_function_tool_json_input` 在 `json.loads` 失败时抛 `ModelBehaviorError`。关键细节是：若开启了工具数据脱敏（`_debug.DONT_LOG_TOOL_DATA`），它**在 `except` 块之外**重新抛错。注释解释了原因——`JSONDecodeError` 的 `.doc` 字段里带着原始载荷，如果在 `except` 块里抛新异常，原异常会作为 `__context__` 挂上去，脱敏异常外传时载荷就又可达了。未脱敏时才把 `input_json` 拼进错误消息。解析成功但不是 `dict`（例如是数组或标量）也报错。

**同步函数的线程化。** 若工具函数不是协程函数，调用走 `asyncio.to_thread(the_func, ...)`，避免阻塞事件循环。`is_sync_function_tool` 在装饰器阶段判定一次，并作为 `sync_invoker=True` 传给内部构造函数（在 invoker 上打一个标记属性）。带不带上下文由 `schema.takes_context` 决定，两条分支各自再分同步/异步，一共四条调用形态。

**超时的两种收场。** `_invoke_function_tool_with_metadata` 在 `timeout_seconds` 为空时直接 await。有超时则把调用包成 task 再用 `asyncio.wait_for`。`asyncio.TimeoutError` 分支里先做一件事：检查 task 是否**其实已经完成**（`done()` 且未 `cancelled()`），若是就把它的结果或异常取出来——这是为了处理「刚好在超时边界完成」的竞态，避免把一个成功的结果当超时丢掉。仍未完成才构造 `ToolTimeoutError`：`raise_exception` 行为直接抛；`error_as_result` 行为下，若有 `timeout_error_function` 就调它生成文案，否则用默认的 `Tool '<name>' timed out after N seconds.`，并把结果标记为「SDK 生成的错误」（`is_sdk_generated_error=True`）。这个标记后续会让输出 schema 校验被跳过——因为这条文案不是工具真正的返回值。

**失败兜底的包装层。** `_FailureHandlingFunctionToolInvoker.__call__` 捕获 `Exception` 后交给 `maybe_invoke_function_tool_failure_error_function`：它按「工具自己配的 formatter → 运行级 formatter → `default_tool_error_function`」的优先级取一个文案生成器；返回 `None` 表示「不要兜底，把异常抛出去」，于是原样 `raise`。返回文本时先调 `on_handled_error`（记日志、给 span 打错误）再把文本当输出返回。

这段包装必须能感知「自己属于哪个工具实例」，否则复制出来的 `FunctionTool` 会用原件实例的失败配置。承担这个绑定的是 `__agents_bind_function_tool__`：`__post_init__` 发现 invoker 带这个方法就调用它，得到的返回值若 `_function_tool` 不同就新建一个绑定到当前实例的 invoker，并把同步工具标记一并带过去。`with_function_tool_failure_error_handler` 是这条链上的适配层——它把一个「三参数 `on_handled_error`」（工具、异常、输入 JSON）转成内部使用的四参数版本，再交给 `_with_context_function_tool_failure_error_handler` 构造 invoker；`_build_wrapped_function_tool` 则直接用四参数版本，两处最终落到同一个 invoker 类。

**默认文案刻意不含细节。** `default_tool_error_function` 恒返回 `"An error occurred while running the tool."`。源码注释点明原因：把异常内容透给模型等于把内部信息灌进上下文；需要细节的应用应显式提供自己的 formatter 并自行承担脱敏责任。默认的超时文案则包含工具名与秒数——因为这两个值是应用配置，不是运行时数据。

**输出类型校验。** 返回值若配了 `output_type_adapter`，会在返回前 `validate_python`；失败抛 `UserError`。脱敏模式下这里也做了同样的处理：把携带载荷的局部变量 `result` 置 `None` 再在 `except` 块外抛，避免 `ValidationError` 的 repr 通过 traceback locals 泄漏原始输出。

**来源元数据的取用。** `get_function_tool_origin` 是给运行项打标签的统一入口：`_emit_tool_origin` 为假返回 `None`（完全不记录）；否则返回显式设置的 `_tool_origin`，未设置则给 `ToolOrigin(type=FUNCTION)`。调用方（例如审批项构造）用它把「这次调用从哪来」写进 `ToolApprovalItem` 与 `ToolCallOutputItem`，而这份元数据不进模型上下文。

**审批策略的参数视图。** `parse_function_tool_arguments` 比执行路径更保守：空串或 `None` 直接返回 `None`；`json.loads` 时挂一个 `parse_constant` 把 `NaN`/`Infinity` 这类非标准常量变成错误；任何解析失败返回 `None`；解析结果不是 `dict` 也返回 `None`。返回 `None` 的语义是「策略无法检查这份参数」，于是 `evaluate_function_tool_approval` 直接返回 `require_approval`——**看不明白就要求人工审批**，而不是默认放行。

## 关键取舍

**源码事实：** 失败默认变成工具输出而不是异常。这意味着模型会看到「工具失败了」这条文本，然后有机会换个参数重试；代价是运行不会因为工具 bug 立刻暴露。需要严格时配 `failure_error_function=None`，让异常穿透。

**源码事实：** 预置参数与本次 `input` 必须先字符串相等才复用。这挡住了「审批的是 A、执行的是 B」这类错位，但也意味着审批之后任何对参数的重新序列化都会导致复用失败、退回重新解析。

**源码事实：** 超时只管「等待」不管「停止」。`asyncio.wait_for` 超时后 task 会被取消，但同步工具跑在 `to_thread` 里，线程本身无法被取消——它会在后台继续跑到自然结束。所以超时是「模型视角的超时」，不是「执行保证被中断」。

**源码事实：** 默认错误文案不含任何异常信息，而默认超时文案含工具名与秒数。区别在于前者可能泄露运行时数据，后者只是配置常量。

**源码事实：** 输出校验失败抛的是 `UserError`（配置/实现错误），而参数校验失败抛 `ModelBehaviorError`（模型行为错误）。两者的处置路径不同：前者通常意味着工具实现与声明不符，后者可以靠模型重试。

**源码事实：** 审批路径的解析与执行路径的解析是**两套独立实现**——一个用 pydantic 模型的完整校验（含默认值、别名、校验器），另一个只做 JSON 解码并把非标准常量拒掉。这是刻意的：审批阶段绝不能执行用户代码（自定义校验器、默认工厂都可能带副作用），所以只能用最保守的纯 JSON 视图；宁可多要求人工审批，也不在审批前跑任何应用逻辑。

**架构推论：** 这条调用链把「不可信输入 → 校验 → 执行 → 兜底 → 输出」分成明确的几段，每段都有各自的失败策略：解析失败 -> `ModelBehaviorError`（可重试）、业务失败 -> 文案（模型可见）、超时 -> 二选一、输出不符声明 -> `UserError`（不可重试）。把失败分类这件事本身，比任何单个校验都更影响系统的稳定性——因为不同类别的正确应对方式完全不同。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/tool.py` | `_on_invoke_tool_impl` | 装饰器生成的调用体：复用预置参数、解析、注入上下文、线程化调用、校验输出 |
| `src/agents/tool.py` | `_parse_function_tool_json_input` | JSON 解码 + 对象检查，脱敏模式下把重抛移出 `except` |
| `src/agents/tool.py` | `_FailureHandlingFunctionToolInvoker` | 失败包装层：捕获异常 → 文案生成器 → 返回文本或重抛 |
| `src/agents/tool.py` | `__agents_bind_function_tool__` | 把 invoker 重新绑定到复制出来的工具实例 |
| `src/agents/tool.py` | `with_function_tool_failure_error_handler` | 三参数回调到四参数内部签名的适配层 |
| `src/agents/tool.py` | `_invoke_function_tool_with_metadata` | 超时控制与「SDK 生成错误」标记 |
| `src/agents/tool.py` | `default_tool_error_function` | 不含任何细节的默认失败文案 |
| `src/agents/tool.py` | `_prepare_for_approval` | 审批前准备：纯数据校验或退回空壳 |
| `src/agents/tool.py` | `_validate_function_tool_output` | 输出类型校验，脱敏模式下清理携带载荷的局部变量 |
| `src/agents/tool.py` | `get_function_tool_origin` | 来源元数据统一出口，供运行项打标签 |
| `src/agents/util/_approvals.py` | `parse_function_tool_arguments` | 审批策略的保守参数视图：拒绝非标准 JSON 常量，非对象返回 `None` |
| `src/agents/util/_approvals.py` | `evaluate_needs_approval_setting` | 布尔或可调用的审批开关求值 |

## 参考

- [openai-agents-python 源码：tool.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/tool.py)
- [openai-agents-python 源码：util/_approvals.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/util/_approvals.py)
- [Agents SDK 文档：函数工具](https://openai.github.io/openai-agents-python/tools/#function-tools)

## 与主核 / AI 的关系

这条链路与主核的「请求解析 → 计划 → 执行」有直接可比性：解析层严格拒绝畸形输入（这里是 `ModelBehaviorError`，主核是语法/语义错误），执行层的失败按可恢复性分流（这里超时二选一，主核是死锁重试 vs 立即报错），最后都要把结果编码回一个外部可见的形态（这里是 JSON 输出项，主核是结果集）。

对 agent 工程最实用的三条推论。其一，**让工具失败变成文本，而不是让运行崩溃**是这套 SDK 的默认取向。它提高了长任务的完成率，但会掩盖实现层的 bug；关键工具应当显式关掉兜底，让错误尽早暴露。其二，**审批前的参数视图必须是无副作用的**。任何在审批阶段执行的应用代码（pydantic 校验器、默认工厂、正则匹配）都可能被一次恶意参数触发出乎意料的行为，这里用 `can_prepare_without_user_code` 逐层白名单把这个面收到最小。其三，**默认文案不泄漏异常内容**这条设计值得照抄：模型上下文是会被记录、被回放、被第三方查看的，错误消息一旦进去就等于永久留存。
