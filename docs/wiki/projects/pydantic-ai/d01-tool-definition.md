---
project: pydantic-ai
feature: tool-definition
title: 工具定义与 schema
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/tools.py
    symbols: [Tool, ToolDefinition, GenerateToolJsonSchema]
  - path: pydantic_ai_slim/pydantic_ai/_function_schema.py
    symbols: [function_schema, FunctionSchema, takes_ctx]
---

# 工具定义与 schema

## 一句话职责

`Tool` 把「一个 Python 函数」编译成三样东西：给模型看的 JSON schema、给运行时用的 Pydantic 校验器、以及一份可被包装/改名/过滤的 `ToolDefinition`；三者一次构建、处处复用。

## 机制是什么

一个工具在框架里有两个面孔：

- **`ToolDefinition`**：纯数据——名字、描述、参数 JSON schema、返回 schema、`kind`、`sequential`、`timeout`、`defer_loading`、`ui_visibility`、`capability_id` 等。它是模型看到的东西，也是包装层改写的东西。
- **`Tool`**：实现——持有原函数、`FunctionSchema`（校验器 + 调用适配）、`prepare` 回调、自定义 `args_validator`、以及那些原样落进 `ToolDefinition` 的配置。

`FunctionSchema` 是两者的中介：它在构造期一次性把函数签名、类型注解、docstring 变成「JSON schema + 校验器 + 调用约定」三件套，此后运行期只做校验与调用，不再反射。

## 怎么实现的

**schema 构建的全流程。** `function_schema` 是唯一入口，顺序是：取签名与类型注解（`get_type_hints(include_extras=True)`，所以要读懂 `Annotated` 里的 pydantic 元数据）→ 解析 docstring 得到描述与逐参数说明 → 逐参数登记进一个上下文字段表 → 生成 TypedDict 核心 schema → `clean_schema` → 用清理后的 schema 建 `SchemaValidator` → 同一份 schema 生成 JSON schema。

几个可选行为在第一步就分叉：

- `takes_ctx`：为 `None` 时自动探测——只看第一个参数，注解是 `RunContext`（含参数化形式）就置真。探测用的 `is_call_ctx` 只比较类或 `get_origin`，不追字符串注解。
- `require_parameter_descriptions`：为真时，缺描述的参数名累进 `errors`，最后拼成一条 `UserError` 抛出。
- `attribute_docstrings`：写进 `ConfigDict(use_attribute_docstrings=...)`，影响「字段下方的字符串是否成为描述」。

错误不是逐条抛，而是**收集后一次抛出**。`function_schema` 里 `errors` 列表同时接收：签名获取失败、缺参数描述、以及 `validate_schema_signature` 的两种拒绝——`RunContext` 不许作为 `*args`、除 ctx 外每个参数都必须有类型注解。最后统一成「Error generating schema for <qualname>:」加缩进列表。

**单参数的特殊处理。** 若函数恰好只有一个值参数且没有 `**kwargs`，`_build_schema` 会做一件特殊事：如果这个参数是「模型式」（`metadata['is_model_like']` 为真，即单个 Pydantic 模型/dict 之类），JSON schema 直接就是那个模型自己的 schema——模型在顶层生成字段，而不是塞进一层冗余的包装对象。但校验器输出仍被包成 `{参数名: 值}`，因为下游契约（钩子、`call_tool`）一律假设「校验后的参数是一个按参数名索引的 dict」。

这层包装由 `_validate_single_arg` 的 `no_info_wrap_validator_function` 实现，它要区分三种输入形态：`_Revalidated`（来自 `revalidate`，明确是已校验值）、真正的未包装输入、以及 `{name: ...}` 包装信封。判定规则写得非常谨慎：

- 若 `name` 不是该模型可接受的键（`accepted_keys`），`{name: ...}` 只可能是包装信封，直接解包。
- 若 `name` 恰好也是模型的一个字段或别名，`{name: ...}` 通常是真实的未包装输入，先按未包装校验，失败才退到解包。
- 两种读法都可接受时（例如字段是 `Any`），优先未包装——注释明确承认这种情况下「重复校验不幂等」，需要确定性的调用方应当用 `FunctionSchema.revalidate`。

`revalidate` 就是那条确定性通道：它存在的原因是参数可能以 JSON 跨过一次边界（序列化 → 反序列化）后需要重新校验。它分两条：单模型参数走 `_Revalidated` 包一层再校验；多参数则先按别名读（`validator` 的默认读法），失败才用 `revalidator`（专门按 Python 名读的一份 schema）。为什么不直接用 `revalidator`？注释说得很清楚：当某参数的别名恰好是另一参数的名字时，按名读会改变原本按别名读能接受的参数。

**参数到调用的映射。** 校验后是 dict，调用要还原成位置的/关键字的实参。`_call_args` 处理三种参数形态：位置专用参数从 dict 按序 `pop` 出来放前面；`*args` 同理展开；其余作为 kwargs。这里有一个刻意的设计选择：**只要存在位置专用参数或 `*args`，就先复制一份 dict 再 pop**。理由是「同一个校验后 dict 之后还要交给工具执行钩子（例如 `after_tool_execute`），钩子必须还能看到全部参数」。

还有一个对称的细节：前置的 `POSITIONAL_OR_KEYWORD` 参数在存在 `*args` 时必须按位置传，否则会与解包进 `*args` 的值双绑定；而不存在 `*args` 时保持历史行为（按关键字传）。这条判断写在 `_FunctionSchemaContext.has_var_positional` 里，是 `function_schema` 在建 schema 时就定下的。

**返回值也要 schema。** `extract_return_schema_type` 把返回注解归一到「总能生成一个 schema」的形态：无注解或 `-> Any` 生成 `{}`；`-> None` 生成 `{"type": "null"}`；`ToolReturn[T]` 取 `T`，裸 `ToolReturn` 退成 `Any`；`Self` 在已绑定的方法上解析为所属类。生成用 `mode='serialization'`，失败则告警并降级为空 schema。注释给了一条实现理由：返回 schema 要在 `function_schema` 里**提前**算好，因为 Temporal 沙箱里 `TypeAdapter` 太慢。

**`GenerateToolJsonSchema` 的两处改写。** 它继承 Pydantic 的默认生成器，只覆写两个方法：

- `enum_schema`：当枚举类混入 `UseEnumMemberDocstrings` 时，把 `enum` 列表改写成 `anyOf` + `const` + `description`，让模型能区分枚举选项。这里有两个约束值得记：一是不能用外层模型的 `use_attribute_docstrings` 配置控制，因为那份配置在**核心 schema** 构建时压栈，生成 JSON schema 时已经不在栈上；二是若枚举里有 `None` 成员，则放弃改写——`{'const': None}` 会被读成「没有 const」，反而丢失约束。别名（`urgent = 'high'`）的 docstring 通过 `__members__` 归到规范名，`setdefault` 保证规范名自己的 docstring 优先。
- `_named_required_fields_schema`：顺手删掉每个属性上的 `title`，因为对工具参数而言这些标题基本无用，且会占 token。

**`ToolDefinition` 的字段纪律。** 它是一条带默认值的 dataclass，几乎每个字段都写了「发给模型 / 不发给模型」。这条区分很重要：

- **发给模型**：`name`、`description`、`parameters_json_schema`、`strict`、`sequential`、`return_schema`（仅在 `include_return_schema` 为真时保留）。
- **不发给模型**：`metadata`（供过滤与行为定制，MCP 工具会把 `meta` 与 `annotations` 放进来）、`durable`、`ui_visibility`、`toolset_id`、`capability_id`、`timeout`。

`__post_init__` 只做一件事：把 `tool_kind` 与 `unless_tool_kind` 从「kind 对象或其 id」归一成字符串 id。`defer` 是个只读便利属性，判据是 `kind in ('external', 'unapproved')`——即「这个调用会不会被延迟」。

**`Tool.tool_def` 是三态开关的汇合点。** 它是 property 而不是缓存字段，每次访问重建，因为其中两个字段受一个 `ContextVar` 影响：`_without_toolset_sequential_defer_loading`。置真时，`sequential` 与 `defer_loading` 里「由工具集施加的那一份」会被撤掉（`_sequential_from_toolset` / `_defer_loading_from_toolset` 这两个非 dataclass 属性记录来源）。注释解释了用途：Temporal 与 DBOS 为「在 `FunctionToolset` 施加这两个值之前启动的工作流」列工具时，要让 `prepare` 看到当时看到的样子。

**`prepare` 是唯一的逐次改写口。** `prepare_tool_def` 先取 `tool_def`，有 `prepare` 就调（同步/异步皆可），返回值可以是 `None`——表示这次的运行不注册这个工具。这是「工具动态出现/消失」的唯一官方途径，其余一切（前缀、改名、过滤、加元数据）都通过包装型工具集实现。

**`Tool.from_schema` 是反方向的构造。** 它接受现成的 JSON schema 与一个函数，构造一个 `validator` 为「接受任何值」的 `FunctionSchema`——schema 校验被跳过，但自定义 `args_validator` 仍会运行。这条路径是给「工具来自外部定义（例如 MCP）」准备的：外部 schema 不适合再走 Pydantic 的核心 schema 生成。

**命名与废弃兼容。** `Tool.__init__` 里 `self.name = name or function.__name__`；`description` 为空时从 docstring 取（若函数只有一个模型式参数且没写描述，还会把该参数的 description 提上来当工具描述）。模块末尾的 `__getattr__` 只处理 `ToolKind` 这一个名字：它已被重命名为 `ToolDefinitionKind`，访问旧名会发一条 `PydanticAIDeprecationWarning` 再返回新类型——`v3` 才删除。

## 关键取舍

**源码事实：** schema 构建是「一次到底」的：签名、注解、docstring 全部在构造期消费，运行期不再反射。代价是装饰后修改 `__annotations__` 不会生效，好处是每个工具只在启动期付一次反射成本。

**源码事实：** 校验器输出的 dict 键是 **Python 参数名**，而 JSON schema 的属性可能是**别名**。`FunctionSchema.parameter_names` 专门提供这层映射，且在「别名与另一参数同名」时不重映射也不拒绝，理由是要保持已有的成功运行不被改变。

**源码事实：** 单模型参数的 JSON schema 被摊平（模型字段在顶层），校验器输出却被打包。这是一处不对称，目的只是让模型少写一层无意义的包装对象。

**源码事实：** 位置专用参数与 `*args` 的存在会让 `_call_args` 复制 dict，因为同一份 dict 之后还要给执行钩子看。

**源码事实：** 返回 schema 在构造期就生成，不是懒加载。注释点明原因是 Temporal 沙箱里 `TypeAdapter` 太慢。

**源码事实：** 枚举成员 docstring 的启用方式是一个混入基类（`UseEnumMemberDocstrings`），不是配置项。理由很具体：枚举的类属性只要带注解就会变成成员，所以「能携带的标记」只能是基类。

**架构推论：** 把「模型看到的定义」与「运行时的实现」拆成 `ToolDefinition` 与 `Tool`，使包装层只处理纯数据。于是「前缀」「改名」「过滤」「加元数据」都可以在不触碰原函数的前提下实现——这也解释了为什么工具集会有一整族 `WrapperToolset`。

**架构推论：** `tool_def` 做成每次重建的 property 并读取一个 `ContextVar`，是为了兼容「同一份代码在两种运行引擎里必须看到不同的工具列表」这个现实。这类兼容位点通常会被写进注释而不是文档，因为它描述的是历史包袱而非设计意图。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/tools.py` | `Tool` | 工具实现：schema、prepare、args_validator、配置项 |
| `pydantic_ai_slim/pydantic_ai/tools.py` | `Tool.tool_def` | 生成 `ToolDefinition`，受 ContextVar 影响撤掉工具集施加的标记 |
| `pydantic_ai_slim/pydantic_ai/tools.py` | `Tool.prepare_tool_def` | 唯一逐次改写/隐藏工具的入口 |
| `pydantic_ai_slim/pydantic_ai/tools.py` | `Tool.from_schema` | 从现成 JSON schema 构造，跳过 schema 校验 |
| `pydantic_ai_slim/pydantic_ai/tools.py` | `ToolDefinition` | 模型可见的纯数据定义，逐字段标注是否发给模型 |
| `pydantic_ai_slim/pydantic_ai/tools.py` | `GenerateToolJsonSchema` | 枚举 docstring 改写、去掉参数 title |
| `pydantic_ai_slim/pydantic_ai/_function_schema.py` | `function_schema` | 一次构建校验器 + JSON schema + 返回 schema，错误收集后统一抛出 |
| `pydantic_ai_slim/pydantic_ai/_function_schema.py` | `FunctionSchema` | 校验、重校验、调用适配（位置参数与 ctx 注入） |
| `pydantic_ai_slim/pydantic_ai/_function_schema.py` | `takes_ctx` / `is_call_ctx` | 首参是否为 `RunContext` 的探测 |
| `pydantic_ai_slim/pydantic_ai/_function_schema.py` | `extract_return_schema_type` | 返回注解归一化，保证总能生成 schema |

## 参考

- [pydantic-ai 源码：tools.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/tools.py)
- [pydantic-ai 源码：_function_schema.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_function_schema.py)
- [Pydantic AI 文档：工具](https://ai.pydantic.dev/tools/)
- [Pydantic 文档：JSON Schema 生成](https://docs.pydantic.dev/latest/concepts/json_schema/)

## 与主核 / AI 的关系

「一份定义同时供外部消费者与内部执行器使用」这个结构，与主核里「系统目录 + 执行器」的关系是同构的：主核的表定义既是给优化器看的元数据，也是给存取层用的接口描述；这里的 `ToolDefinition` 既发给模型（作为函数的 JSON schema），又供校验与调用使用。两边都要处理同一个麻烦——外部形态与内部形态不完全一致。主核里是 SQL 类型与存储格式的差异，这里是 JSON 属性名（别名）与 Python 参数名的差异。两边的解法都是「保留一层显式映射，而不是强行让两边统一」。

单模型参数摊平那段尤其值得对照主核里的「行类型与列投影」：为了让消费方少写一层嵌套，内部表示与外部表示被刻意做成了不同形状，再用一个校验器桥接。这类不对称是必要的，但它必须被写下来——否则下一个维护者会把它当成 bug 修掉，从而破坏已上线模型的行为。

对 AI 工程的实际意义在于：工具的 schema 质量直接决定模型能不能正确调用。这份实现里几处「看起来琐碎」的改动都指向这个目标：摊平单参数、枚举选项带描述、删掉无用的 title、返回类型也给出 schema。它们的共同点是「减少模型需要猜的东西」，而代价是构造期的复杂度与几处必须小心维持的兼容性。
