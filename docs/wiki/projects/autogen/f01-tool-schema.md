---
project: autogen
feature: tool-schema
title: 工具 schema 与状态化调用
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/tools/_base.py
    symbols: [Tool, ToolSchema, BaseTool]
---

# 工具 schema 与状态化调用

## 一句话职责

用「参数 pydantic 模型 + 返回类型 + 名字 + 描述」四件套描述一个工具，再把参数模型自动投影成 `ToolSchema` 交给模型、把模型给的字典参数自动校验回模型实例交给实现——把「模型说的」和「代码要的」这两套类型系统接上。

## 机制是什么

`Tool` 是 `@runtime_checkable` 的 `Protocol`，九个成员：`name`、`description`、`schema` 三个属性，`args_type`、`return_type`、`state_type` 三个类型查询，`return_value_as_string`、`run_json`、以及 `save_state_json`/`load_state_json` 两组序列化口。`StreamTool` 在其上加 `run_json_stream`。

`ToolSchema` 是 `TypedDict` 而非 pydantic 模型，字段是 `parameters`（另一个 `TypedDict`，含 `type`、`properties`、可选 `required`、可选 `additionalProperties`）、`name`、可选 `description`、可选 `strict`。之所以用 `TypedDict`，是因为这份结构要原样喂给模型 API，而 API 认的是纯字典——`TypedDict` 在运行期就是 `dict`，类型检查与序列化两头都不损失。

`BaseTool[ArgsT, ReturnT]` 是 `ABC`，同时实现 `Tool` 并继承 `ComponentBase[BaseModel]`。它的构造函数接收 `args_type`、`return_type`、`name`、`description`、`strict`，把返回类型过一遍 `normalize_annotated_type`（剥掉 `Annotated`），然后 `schema` 属性在每次访问时**从 `args_type.model_json_schema()` 现场推导**。

三个具体基类分工：`BaseTool` 只有 `run`；`BaseToolWithState` 加 `save_state`/`load_state` 两个抽象方法与 JSON 包装；`BaseStreamTool` 加 `run_stream` 并在 `run_json_stream` 里校验流末尾的元素类型。

## 怎么实现的

**schema 的推导。** `self._args_type.model_json_schema()` 得到 JSON Schema 后，若含 `$defs`（即参数里引用了嵌套模型），用 `jsonref.replace_refs(obj=model_schema, proxies=False)` 把 `$ref` 就地展开，再 `del model_schema["$defs"]`。展开而非保留 `$ref`，是因为多数模型 API 不接受 `$ref`/`$defs` 这种组合式 schema。随后装成 `ParametersSchema(type="object", properties=..., required=model_schema.get("required", []), additionalProperties=model_schema.get("additionalProperties", False))`。

**strict 的两条硬约束。** 若 `strict=True`，则：required 集合必须等于 properties 的全部键，否则 `ValueError("…Default arguments are not allowed in strict mode.")`；`additionalProperties` 必须为假，否则 `ValueError`。两条都在 `schema` 属性里做 `assert` 后再判断——`assert "required" in parameters` 与 `assert "additionalProperties" in parameters` 是因为 TypedDict 的构造保证这两个键存在。strict 的用途是结构化输出的严格模式，那种模式下模型不会自动补默认值，所以带默认值的参数无法工作。

**`run_json` 的三段。** 用 `trace_tool_span(tool_name=..., tool_description=..., tool_call_id=call_id)` 包住真正的执行；执行体只有一行 `await self.run(self._args_type.model_validate(args), cancellation_token)`——**参数校验由 pydantic 完成，任何不合法都在这里抛 `ValidationError`**。span 结束后构造 `ToolCallEvent(tool_name, arguments=dict(args), result=self.return_value_as_string(return_value))` 写进事件 logger。

注意 span 的位置：它在 `run` 之前开始、在 `run` 之后结束，而事件日志写在 span 之外。所以「工具执行耗时」被 span 记录，而「调用事件」独立成一条日志。

**`return_value_as_string`。** 若返回值是 `BaseModel`，`model_dump()` 后 `json.dumps`；否则 `str(value)`。这个函数是工具结果进入 `FunctionExecutionResult.content`（一个字符串字段）的唯一通道，因此**非字符串返回值必然被序列化成文本**，多模态结果无法通过这条路传回（这正是 `Workbench` 引入 `ToolResult` 的动机）。

**状态化工具。** `BaseToolWithState.save_state_json` 是 `self.save_state().model_dump()`，`load_state_json` 是 `self.load_state(self._state_type.model_validate(state))`。默认 `state_type()` 返回 `None`，而 `BaseTool` 的 `save_state_json` 返回 `{}`、`load_state_json` 什么都不做——所以无状态工具零成本，有状态工具必须显式实现两个抽象方法。

**流式。** `run_json_stream` 边 `yield` 边记 `return_value = result`，流结束后 `assert return_value is not None`（文案说「流末尾必须产出最终返回值」），再 `isinstance(return_value, self._return_type)` 校验，不符抛 `TypeError`。然后才写事件日志。也就是说**流的最后一个元素同时是「最终结果」**，中间元素类型不受约束。

## 关键取舍

**源码事实：** `schema` 是 property，每次访问都重新调 `model_json_schema()` 并做 `jsonref` 展开。没有缓存。同一个工具被多个智能体反复列 schema 时会重复计算，换来的是「改了 `args_type` 立刻生效」——虽然 `args_type` 在构造期就固定了，所以这份灵活性实际用不上。

**源码事实：** `strict` 的两条校验只判 schema 形状，不判运行时行为。`additionalProperties=False` 会阻止模型传多余参数，但 `run_json` 的 `model_validate` 是否拒绝多余键取决于 pydantic 模型的配置（默认忽略），两处判据并不联动。

**源码事实：** `run_json` 里 `arguments=dict(args)` 用的是**原始字典**而不是校验后的模型。所以日志记的是「模型实际说了什么」，包含被 pydantic 忽略或转换掉的多余字段——这对排查「模型为什么传错参数」有用，但也意味着日志与执行输入可能不一致。

**源码事实：** `state_type()` 在 `BaseTool` 上返回 `None`（不是抽象方法），而 `BaseToolWithState` 才要求状态类型。所以「有没有状态」是**类层次**上的区分，不是运行期可查询的能力位。`Tool` 协议里 `state_type` 的返回类型 `Type[BaseModel] | None` 正是为这两类共存准备的。

**源码事实：** `Tool` 是 `@runtime_checkable` 的 `Protocol`，因此 `isinstance(x, Tool)` 可用。但 `@runtime_checkable` 只检查成员是否存在，不检查签名——`isinstance` 通过不代表能正确调用。

**架构推论：** `$defs` 展开用 `jsonref` 而非手写递归，代价是引入一个第三方依赖；收益是嵌套模型（`List[BaseModel]`、`Optional[Model]`）能被正确内联。若不展开，模型侧会拿到它读不懂的引用结构，工具调用质量直接下降。

**架构推论：** `trace_tool_span` 在 `BaseTool` 里而不是在 `ToolAgent` 里，意味着**任何**工具的调用都会被追踪，不依赖调用方是否记得包 span。这是把可观测性下沉到基类的典型做法，也让事件的 `call_id` 能贯穿到追踪系统。但 `run_json_stream` 的 span 覆盖整个流的生命周期（`with` 包住 `async for`），而 `yield` 在 `with` 内部——所以被取消的流会让 span 以异常结束。

**架构推论：** 三个基类都给 `component_type = "tool"`，包括 `BaseToolWithState` 与 `BaseStreamTool`。所以从组件配置看，有状态工具、流式工具、普通工具都是同一个类型，加载方无法从 `ComponentModel.component_type` 判断它是否支持 `run_json_stream`——能力差异只能靠 `isinstance(x, StreamTool)` 在运行期探。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/tools/_base.py` | `Tool` | 工具协议，九个成员，运行期可 isinstance |
| `python/packages/autogen-core/src/autogen_core/tools/_base.py` | `StreamTool` | 在 `Tool` 上加流式调用 |
| `python/packages/autogen-core/src/autogen_core/tools/_base.py` | `ToolSchema` | 给模型看的工具描述，`TypedDict` 保持纯字典 |
| `python/packages/autogen-core/src/autogen_core/tools/_base.py` | `ParametersSchema` | 参数 schema：type、properties、required、additionalProperties |
| `python/packages/autogen-core/src/autogen_core/tools/_base.py` | `ToolOverride` | 名字与描述的覆盖配置 |
| `python/packages/autogen-core/src/autogen_core/tools/_base.py` | `BaseTool` | 具体工具基类，schema 推导与 `run_json` 校验 |
| `python/packages/autogen-core/src/autogen_core/tools/_base.py` | `BaseTool.schema` | 展开 `$defs` 并组装 `ToolSchema`，strict 校验 |
| `python/packages/autogen-core/src/autogen_core/tools/_base.py` | `BaseTool.run_json` | span 包裹、pydantic 校验、事件日志 |
| `python/packages/autogen-core/src/autogen_core/tools/_base.py` | `BaseTool.return_value_as_string` | 模型结果转文本，`BaseModel` 走 JSON |
| `python/packages/autogen-core/src/autogen_core/tools/_base.py` | `BaseToolWithState` | 加状态存取，`state_type` 非空 |
| `python/packages/autogen-core/src/autogen_core/tools/_base.py` | `BaseStreamTool.run_json_stream` | 流转发并在末尾校验返回类型 |

## 参考

- [AutoGen 源码：tools/_base.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/tools/_base.py)
- [AutoGen 文档：Tools](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/components/tools.html)
- [OpenAI 文档：Function calling](https://platform.openai.com/docs/guides/function-calling)

## 与主核 / AI 的关系

主核的表访问方法（`am`）也是这样一层「把外部描述翻译成内部调用」的抽象：上层只说「扫这张表、给我这些列」，具体的页布局、元组格式、并行策略由实现决定。工具基类做的是同一件事的镜像版：把**内部函数的类型约束**翻译成**外部能理解的描述**，再在调用时把外部给的松散字典折回内部类型。方向相反，但都靠「一个中间描述 + 一次校验」维持两端一致。

对 AI 工程最实际的一点是 `$defs` 展开那一步。工具参数一旦涉及嵌套模型，pydantic 默认产出的 schema 会带 `$ref`，而不同厂商的 API 对 `$ref` 的支持差异很大。AutoGen 选择无条件展开成内联结构，牺牲 schema 体积换兼容性——这是「面向最差实现编程」的正确取舍，因为工具 schema 送给的是不可控的第三方服务。

第二点是 `return_value_as_string` 暴露的结构性限制：工具结果必须折成字符串才能进 `FunctionExecutionResultMessage`。这与模型的文本协议一致，但把多模态、结构化结果全部压扁了。`Workbench` 的 `ToolResult`（带 `TextResultContent` 与 `ImageResultContent` 两种判别类型）就是对这条限制的回应——先用富类型在本地保留，需要进模型协议时再 `to_text()`。

最后，strict 模式的约束值得记成口诀：**开了 strict 就不能有默认值，也不能有多余参数**。它换来的是模型侧严格按 schema 生成，前提是参数模型上每个字段都必填、且 `model_config` 不容许多余键。用它之前先把参数模型改成「全必填」形态，否则会在 `schema` 属性访问时直接抛错。
