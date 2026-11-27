---
project: smolagents
feature: argument-validation
title: 工具参数校验
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/tools.py
    symbols: [validate_tool_arguments, validate_arguments]
---

# 工具参数校验

## 一句话职责

参数校验在 smolagents 里发生两次、在两个时刻、查两件事：`validate_arguments` 在**实例化时**查「工具自己声明得对不对」（属性、名字、签名、可空性一致），`validate_tool_arguments` 在**每次调用前**查「这次传进来的值合不合声明」。

## 机制是什么

两个同名意近的方法容易混淆，先分清：

| 方法 | 何时跑 | 校验对象 | 失败后果 |
|---|---|---|---|
| `Tool.validate_arguments` | 工具实例构造时（由 `validate_after_init` 包装 `__init__` 触发） | 工具**自己**：类属性、`name` 合法性、`forward` 签名与 `inputs` 是否对齐 | `TypeError` / `AssertionError` / `ValueError`，工具造不出来 |
| `validate_tool_arguments` | 每次工具调用前（`ToolCallingAgent.execute_tool_call` 里） | 调用**参数**：键是否在声明里、必填是否齐、值的 JSON 类型是否匹配 | `ValueError`（键不认/缺必填）或 `TypeError`（类型不符） |

前者的产物是「一个可信的工具定义」，后者的输入就是那份定义。所以这两次校验不是重复，而是**上下游**：先保证声明内部一致，再用一致了的声明去查实际值。

校验用的类型体系是 `AUTHORIZED_TYPES` 那十个字符串（`string` / `boolean` / `integer` / `number` / `image` / `audio` / `array` / `object` / `any` / `null`），而非 Pydantic 或完整 JSON Schema。值侧的真实类型由 `_get_json_schema_type(type(value))["type"]` 现场映射出来再比对。

## 怎么实现的

**声明侧：`validate_arguments` 的五道检查。** 顺序不是随意的，从粗到细：

1. **四个必需属性的存在性与类型**：`description` / `name` 必须 `str`，`inputs` 必须 `dict`，`output_type` 必须 `str`；缺失报「You must set an attribute {attr}」。`output_schema` 可选，但给了就必须是 `dict`。
2. **`name` 必须是合法 Python 标识符且非保留字**（`is_valid_name`）。报错文案是 `Exception` 而非更精确的类型。
3. **`inputs` 的形状**：每个条目的值必须是 dict；必须同时有 `type` 与 `description`（缺任一就断言失败并把已有的键列出来）；`type` 只能是字符串或字符串列表（列表里全须是字符串，否则 `TypeError`）；展开后每个类型都必须落在 `AUTHORIZED_TYPES` 内（否则 `ValueError` 并列出非法项）。
4. **`forward` 签名与 `inputs` 键集合必须完全相等**（`actual_keys != expected_keys`）。错误文案明确要求「`self` 之后依次对齐 `inputs` 的键」，并把两边的集合都印出来。
5. **签名 JSON schema 与 `inputs` 的双向对齐**：先用 `_convert_type_hints_to_json_schema(self.forward, error_on_missing_type_hints=False)` 拿到 `properties`（注意注释特意说明这个函数不像 `get_json_schema` 那样要求 docstring，否则这里会因为缺文档而失败），然后对每个 `inputs` 的键：必须出现在 `properties` 里；`nullable` 必须**两边同时有或同时无**——只在 `inputs` 写、或只在类型注解写 `Optional`，都报错。

第 4、5 道可以被类属性 `skip_forward_signature_validation = True` 整体跳过。跳过者是 `PipelineTool`、`SpaceToolWrapper`、`LangChainToolWrapper`（源码注释点名），它们的 `forward` 是 `*args/**kwargs` 或被赋成外部方法。

**调用侧：`validate_tool_arguments` 的分支结构。** 它只做两件事，但都要处理「dict 与单值」两种入参形态。

先看 `arguments` 是不是 dict：

- **是 dict**：遍历每个键值对。键不在 `tool.inputs` 里 → `ValueError("Argument {key} is not in the tool's input schema")`。然后算 `actual_type = _get_json_schema_type(type(value))["type"]`、`expected_type = tool.inputs[key]["type"]`、`expected_type_is_nullable = tool.inputs[key].get("nullable", False)`。
  判定「类型合格」的条件是三选一：实际类型等于期望（期望若是列表则用 `in`）、期望是 `"any"`、或实际是 `"null"` 且期望可空。三者都不成立时还有一条**单向宽容**：`actual_type == "integer"` 而 `expected_type == "number"` 时放行（int 是 number 的子集）。
  遍历完再反向查一遍必填：`inputs` 里每个键，若不在 `arguments` 且不可空 → `ValueError("Argument {key} is required")`。
- **不是 dict**：当成单参数工具的裸值。取 `list(tool.inputs.values())[0]["type"]` 作为期望类型，同样比对，`"any"` 放行。注意这条路径**不查**「这个工具是不是真的只有一个参数」——它直接取第一个。

**错误在智能体层被翻译成两类。** `ToolCallingAgent.execute_tool_call` 的调用点结构很明确：

```python
try:
    validate_tool_arguments(tool, arguments)
except (ValueError, TypeError) as e:
    raise AgentToolCallError(str(e), self.logger) from e
except Exception as e:
    raise AgentToolExecutionError(error_msg, self.logger) from e
```

即「参数问题」与「其他问题」分开：前者是模型可以自己修的（换个参数、补个必填），后者是运行环境问题。执行阶段的异常也走 `AgentToolExecutionError`，且文案分工具与团队成员两种（后者提示「请重试或请求另一位团队成员」）。

还有一处前置：`validate_tool_arguments` 之前会先做 `_substitute_state_variables(arguments)`，把引用状态变量的字符串替换成真实值——所以校验的是**替换后**的值。这个顺序是必须的，否则类型检查会看到字符串而误判。

**空值与可空的双重门。** 这里有个容易踩的细节：`nullable` 在声明侧被强制要求「`inputs` 与类型注解同时声明」，所以调用侧可以放心只读 `inputs` 里的标记。但 `arguments[key]` 的值是 `None` 时，`_get_json_schema_type(type(None))` 会返回 `"null"`，于是「实际 null + 声明 nullable」这条分支接住它。若声明不可空，则报类型错误而不是「缺必填」——两者都是拒绝，但错误文案不同，模型据此得到的修正线索也不同。

## 关键取舍

**源码事实：** 校验分两次，发生在两个时刻，检查两个不同对象（声明的一致性 / 实参的合规性）。二者共用一套十个字符串的受限类型体系。

**源码事实：** `nullable` 必须两边同时声明，否则工具构造失败。这把「文档与实现不一致」变成了硬错误。

**源码事实：** 类型相容是**单向**的：`integer` 可当 `number`，反之不行。没有其他隐式转换（例如字符串数字不会被接受为 `number`）。

**源码事实：** 非 dict 入参走「取第一个 input 的 type」这条捷径，不校验工具是否真的只有一个参数。

**源码事实：** `skip_forward_signature_validation` 能整体跳过第 4、5 两道声明侧检查，使用者恰好是三类包装型工具。

**源码事实：** 参数校验在状态变量替换之后执行，所以校验看到的是实际要传给工具的值。

**架构推论：** 用十个字符串而不是完整 JSON Schema，换来的是「提示词文本可以原样复用这份类型名」——`to_code_prompt` 里就写成 `arg: string`。代价是表达力：联合类型只能写成字符串列表、`Literal` 类枚举要靠 `(choices: ...)` 约定（见 d03）、`anyOf` 这种结构要靠 `inputs` 里手写 `anyOf` 键再由 `get_tool_json_schema` 转换（见 d04）。也就是说这套类型体系是一个**自定义的中间方言**，两头（Python 类型 / 供应商 JSON Schema）各有一层转译。

**架构推论：** 把「声明侧」的检查挂在 `__init_subclass__` 上而不是等首次调用，等于要求**每个工具类都在实例化时自证其合法性**。这让错误出现在构造栈里（容易定位到具体是哪个工具类），代价是不能有「半成品工具」——例如需要运行时才能确定 `inputs` 的工具，只能靠 `skip_forward_signature_validation` 或干脆不从 `Tool` 继承。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/tools.py` | `Tool.validate_arguments` | 声明侧五道检查：属性、名字、inputs 形状、签名对齐、nullable 一致 |
| `src/smolagents/tools.py` | `validate_tool_arguments` | 调用侧校验：键合法、必填齐全、JSON 类型相容 |
| `src/smolagents/tools.py` | `validate_after_init` | 把声明侧检查挂到每个子类 `__init__` 的包装器 |
| `src/smolagents/tools.py` | `AUTHORIZED_TYPES` | 十个字符串组成的受限类型体系 |
| `src/smolagents/_function_type_hints_utils.py` | `_get_json_schema_type` | 运行时值/类型 → 字符串类型的映射，调用侧比对的依据 |
| `src/smolagents/_function_type_hints_utils.py` | `_convert_type_hints_to_json_schema` | 声明侧第 5 道检查用的签名 schema（不要求 docstring） |
| `src/smolagents/agents.py` | `ToolCallingAgent.execute_tool_call` | 校验调用点，把 ValueError/TypeError 翻成 `AgentToolCallError` |

## 参考

- [smolagents 源码：tools.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/tools.py)
- [smolagents 文档：工具输入](https://huggingface.co/docs/smolagents/tutorials/tools#how-to-create-a-tool)
- [smolagents 文档：工具参考](https://huggingface.co/docs/smolagents/reference/tools)
- [JSON Schema 文档：type 关键字](https://json-schema.org/understanding-json-schema/reference/type)

## 与主核 / AI 的关系

「声明侧一致性与实参合规性分两次校验」与主核里的**解析期 vs 执行期绑定**是同一套分工：主核在解析阶段确认 SQL 里的列名与类型存在且相容（错了就在编译期失败），在执行阶段确认实际参数/行数据符合类型（错了在运行期失败）。两边的收益相同——**尽早失败**，且在两个阶段给出不同粒度的诊断。

类型相容的单向规则（`integer` → `number`）也值得对照：主核的隐式类型转换同样是有向的（int 提升为 numeric 可以，反向不行），因为反向意味着精度丢失。这里没有更宽的隐式转换，实际上是把「模型可能传错类型」当成常态，宁可拒绝也不猜——因为**模型能从错误信息里学会修正参数**，让它猜反而掩盖了问题。

`nullable` 双边一致检查最值得借鉴。主核里有大量同类的一致性检查（索引列必须存在于表、分区键必须在主键内），共同点是：**一份语义被写在两个地方时，必须有一个机器检查它们相等**，否则「改了这边忘了那边」会成为永久性的静默缺陷。

对 AI 工程的实际意义在于：工具参数校验写得好不好，直接决定模型能不能自我纠错。这套实现里两条细节特别有效：错误文案带上具体的键名与期望类型（`Argument query has type 'integer' but should be 'string'`），以及把参数错误与其他错误分成两类异常——前者暗示「改参数重试」，后者暗示「换个工具」。相比之下，一个笼统的「Invalid arguments」会让模型反复做同样的错误尝试，白白消耗步数。
