---
project: autogen
feature: function-introspection
title: 函数签名到工具 schema
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/_function_utils.py
    symbols: [get_function_schema, get_typed_signature, normalize_annotated_type]
---

# 函数签名到工具 schema

## 一句话职责

把 Python 函数的类型注解翻译成模型能读懂的 JSON Schema（OpenAI `tools` 格式），并顺手造出「参数校验用的 pydantic 模型」，使普通函数可以直接被当成工具交给 LLM。

## 机制是什么

模型不会读 Python 函数，只认 JSON Schema。所以工具化的第一步是内省：取签名、解析注解、把每个参数的注解交给 pydantic 生成 JSON Schema 片段，最后组装成 `{"type": "function", "function": {"name", "description", "parameters"}}` 三段结构（`ToolFunction` / `Function` / `Parameters` 三个 pydantic 模型）。

内省的核心手段是 `typing.get_type_hints(..., include_extras=True)`，而不是直接读 `inspect.signature` 的 `annotation`。原因是字符串注解（`from __future__ import annotations` 或手写前置引用）在运行时只是 `str`，只有 `get_type_hints` 会拿 `__globals__` 把前向引用求值成真类型。`get_typed_signature` 因此重新拼装了一份 `inspect.Signature`：参数名、kind、default 沿用原签名，`annotation` 换成求值后的类型。

参数描述来自 `Annotated` 的元数据：`Annotated[str, "Date in YYYY/MM/DD"]` 的第一项元数据被当成 description；没有 `Annotated` 时**参数名本身**当 description。这是给模型看的文案，不是文档。

## 怎么实现的

**签名求值。** `get_typed_signature` 先 `inspect.signature(call)`，再取 `call.__globals__`（若存在）作为 `globalns`；遇到 `functools.partial` 时用 `call.func` 取注解，因为 partial 对象本身没有 `__globals__` 也没有注解。返回注解用 `type_hints.get("return", inspect.Signature.empty)`——缺失就退化为空，而非 `None`，这样才能与「显式注解了 `None`」区分开。

**必填集与默认值集。** `get_required_params` 收集 `default == inspect.Signature.empty` 的参数名；`get_default_values` 收集有默认值的参数及其值。两个集合互斥且合并即全集，这是「必填」与「可选」的唯一判据——不看注解是否 `Optional`。

**注解缺失的分级。** `get_missing_annotations` 把缺注解的参数切成两半：落在必填集里的 `missing`，以及有默认值但没注解的 `unannotated_with_default`。前者 `raise TypeError`（文案写「所有无默认值的参数必须有注解」），后者只 `logger.warning`。返回注解缺失也只是一条 warning，提示「函数应返回字符串或 `BaseModel` 子类」。于是同一份 schema 生成逻辑里，**三种缺失对应三种强度**：致命、警告、静默。

**单参数 schema。** `get_parameter_json_schema` 调 `TypeAdapter(v).json_schema()`，若该参数在 `default_values` 里则把默认值写进 `schema["default"]`，最后强制覆盖 `schema["description"] = type2description(k, v)`。注意顺序：description 是**赋值**不是补空，所以 pydantic 从 `Field(description=...)` 推出的描述会被覆盖掉。`type2description` 对 `Annotated` 取 `__metadata__[0]`，非 `str` 直接 `ValueError`。

**组装。** `get_parameters` 遍历 `param_annotations`，跳过 `v is inspect.Signature.empty` 的项（虽理论上已被 `get_param_annotations` 过滤过），把 properties 与 required 装进 `Parameters`。`get_function_schema` 最后 `ToolFunction(...).model_dump()` 得到纯字典。

**另一条路：直接造 pydantic 模型。** `args_base_model_from_signature` 用 `pydantic.create_model` 动态建类，每个字段是 `(归一化后的类型, Field(default=..., description=...))`。它有一处特例：参数名恰为 `cancellation_token` 时**整条跳过**——取消令牌是运行期注入的，不该出现在模型参数里。注解缺失在这里是硬失败（`ValueError("No annotation")`），因为 `create_model` 无法为无注解字段建列。

**Annotated 归一。** `normalize_annotated_type` 只做一件事：`get_origin` 是 `Annotated` 时取 `get_args()[0]`。`FunctionTool` 用它把返回值的 `Annotated[T, ...]` 剥成 `T`，`args_base_model_from_signature` 用它把参数注解剥成裸类型再建字段——元数据已经被 `type2description` 单独取走，留在字段类型里只会让 pydantic 困惑。

**可选类型与默认值。** `create_model` 传 `PydanticUndefined` 表示「无默认」，从而在生成的模型上成为必填字段。这条路径让 `run_json` 能用统一的 `model_validate(args)` 做参数校验，而无需再读一遍签名。

## 关键取舍

**源码事实：** `get_parameter_json_schema` 里 description 是覆盖赋值，所以 `Annotated[T, "描述"]` 与 `Field(description=...)` 同时存在时，前者胜出。而参数名兜底意味着**每个参数在 schema 里一定有 description**，不存在空描述。

**源码事实：** 必填只看「有没有默认值」，不看是否 `Optional`。所以 `x: int | None` 而无默认值会被判为必填——模型必须显式给出 `null`。这是 JSON Schema 的语义，不是 Python 的。

**源码事实：** `get_function_schema` 的 docstring 示例里参数 schema 的 `type` 写作 `'str'`、`'int'`，而现实现走 `TypeAdapter.json_schema()`，产出的是 `'string'`、`'integer'`。文档示例是旧实现残留，读源码时以代码为准。

**源码事实：** `args_base_model_from_signature` 跳过 `cancellation_token` 参数，而 `FunctionTool` 用 `"cancellation_token" in signature.parameters` 来标记 `has_cancellation_support`。这两个地方对同一个名字的判断口径一致，靠的是**约定而非类型**：只要参数叫这个名字，就自动被当作运行期注入。

**架构推论：** 内省结果的正确性完全依赖注解质量。没有注解的必填参数直接拒绝，等于把「工具必须自带使用说明」变成硬要求；有默认值的参数可以省略注解，因为模型能从默认值推断类型——这是把成本压到「只对模型真正需要的信息收费」。

**架构推论：** 这里有两套并行的类型→校验转换：一套产 JSON Schema 给模型看，一套产 pydantic 模型给运行期校验用。它们必须保持一致，否则会出现「模型按 schema 给出了合法参数，运行期校验却拒绝」的分裂。共享的是同一份 `Annotated` 元数据与同一个签名对象，一致性靠这个共同来源维持，没有任何自动对账机制。

**架构推论：** 签名内省发生在工具构造期（`FunctionTool.__init__`），而不是每次调用。所以运行期改注解、热替换函数都不会生效——工具是不可变的 schema 快照。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/_function_utils.py` | `get_typed_signature` | 用 `get_type_hints` 求值前向引用后重拼签名 |
| `python/packages/autogen-core/src/autogen_core/_function_utils.py` | `get_typed_return_annotation` | 返回注解求值，缺失返回 `None` |
| `python/packages/autogen-core/src/autogen_core/_function_utils.py` | `get_param_annotations` | 收集非空参数注解 |
| `python/packages/autogen-core/src/autogen_core/_function_utils.py` | `get_required_params` | 无默认值即必填 |
| `python/packages/autogen-core/src/autogen_core/_function_utils.py` | `get_default_values` | 有默认值的参数及其值 |
| `python/packages/autogen-core/src/autogen_core/_function_utils.py` | `get_missing_annotations` | 缺注解按必填与否分级 |
| `python/packages/autogen-core/src/autogen_core/_function_utils.py` | `get_parameter_json_schema` | 单参数 JSON Schema，写入 default 并覆盖 description |
| `python/packages/autogen-core/src/autogen_core/_function_utils.py` | `type2description` | `Annotated` 元数据取描述，否则用参数名 |
| `python/packages/autogen-core/src/autogen_core/_function_utils.py` | `get_function_schema` | 组装 OpenAI `tools` 格式的完整 schema |
| `python/packages/autogen-core/src/autogen_core/_function_utils.py` | `normalize_annotated_type` | 剥掉 `Annotated` 只留内层类型 |
| `python/packages/autogen-core/src/autogen_core/_function_utils.py` | `args_base_model_from_signature` | 用 `create_model` 生成参数校验模型，跳过取消令牌 |

## 参考

- [AutoGen 源码：_function_utils.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_function_utils.py)
- [AutoGen 文档：Tools](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/components/tools.html)
- [Pydantic 文档：JSON Schema 生成](https://docs.pydantic.dev/latest/concepts/json_schema/)

## 与主核 / AI 的关系

主核同样在运行期解释「外部给的描述」，但那是 SQL 的类型系统与目录：类型一旦注册就固定，SQL 文本在解析期被翻译成执行计划，任何类型错误都在编译期被拒。这里的内省是**把宿主语言类型系统的一部分投影成外部协议**，投影是单向且有损的：Python 的 `kind`（`POSITIONAL_ONLY`、`KEYWORD_ONLY`）、默认值的可变性、注解里的任意元数据，全都没有对应的 JSON Schema 表达。损失被有意忽略，因为接收方是模型，能读懂的只有 JSON。

对 AI 工程的直接教训是：**工具 schema 就是 API 契约，而 description 是契约里唯一的自然语言字段**。参数名兜底成 description 看起来很贴心，实际会让模型读到 `{"ticker": {"description": "ticker"}}` 这种零信息文本，白占 token 又不提供线索。写好 `Annotated` 描述是使用 `FunctionTool` 的必要工作，不是可选项。

另一条边界值得记住：`get_function_schema` 与 `args_base_model_from_signature` 都只处理**能被 `get_type_hints` 求值**的注解。在交互式环境（注解所在的名字不在 `__globals__` 里）或用 `eval` 拼出来的函数上，求值会失败并把注解退化成字符串——此时 `TypeAdapter(str)` 只会产出 `{"type": "string"}`，schema 静默失真而不报错。
