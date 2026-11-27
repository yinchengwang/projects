---
project: smolagents
feature: json-schema-from-signature
title: 从签名与文档生成 JSON schema
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/_function_type_hints_utils.py
    symbols: [get_json_schema, _parse_type_hint, _parse_google_format_docstring]
  - path: src/smolagents/_function_type_hints_utils.py
    symbols: [_convert_type_hints_to_json_schema, _parse_union_type]
---

# 从签名与文档生成 JSON schema

## 一句话职责

这个模块把「一个普通 Python 函数」变成「一份工具定义」：类型注解给出参数与返回的类型，Google 风格 docstring 给出描述与枚举候选，两者合成为 JSON schema——`@tool` 装饰器就是靠它一次拿到 `name` / `description` / `inputs` / `output_type`。

## 机制是什么

它接受的输入是**函数**，不是类。这决定了它在 smolagents 里的位置：它服务于 `@tool` 装饰器路径（把函数变工具），而子类化 `Tool` 的路径则是人手工写 `inputs`。两条路径最终产出的东西相同（四个类属性），但前者需要这层推导。

层叠结构是：

1. `get_json_schema(func)` → 返回 `{"type": "function", "function": {...}}`，即 OpenAI tool calling 的标准外形。这一步负责 docstring。
2. `_convert_type_hints_to_json_schema(func)` → 返回 `{"type": "object", "properties": {...}, "required": [...]}`，只负责类型。源码注释特意说明它**不要求 docstring**，所以它能被 `Tool.validate_arguments` 单独用来做声明侧校验（见 d01）。
3. `_parse_type_hint(hint)` → 单个类型注解到 JSON schema 片段的递归转换。
4. `_parse_union_type(args)` → 联合类型的合并规则。
5. `_get_json_schema_type(type)` → 基础类型查表，也是运行期值类型映射的同一张表（`validate_tool_arguments` 用它）。

## 怎么实现的

**docstring 解析用四条正则，而不是 `docstring_parser` 库。** 模块里定义了 `description_re`、`args_re`、`args_split_re`、`returns_re` 四个编译好的正则，`_parse_google_format_docstring` 依次 `search` 并取第一个捕获组：

- `description_re` 从开头取到第一个 `Args:` / `Returns:` / `Raises:` 之前（`DOTALL`，所以能跨行）。
- `args_re` 取 `Args:` 与下一个段标题之间的整块。
- `args_split_re` 是这里最讲究的一条：它以「行首的空格 + 标识符 + 可选括号类型 + 冒号 + 空格」为参数起点（`VERBOSE` 模式下每个片段都有注释说明用途），参数描述可以跨行，靠**下一个参数的起点或块尾**作为终止前瞻。
- 返回块会先跳过可能的类型标注（`(?:[^)]*?:\s*)?`）。

解析后的参数块会先删空行再 `re.sub(r"\s*\n+\s*", " ", ...)` 把跨行描述压成单行。这个「压平」动作是必要的——后面的 `(choices: ...)` 正则要求候选块在**行尾**。

**枚举是靠 docstring 里的约定，不是靠 `Literal`。** `get_json_schema` 里对每个参数描述做 `re.search(r"\(choices:\s*(.*?)\)\s*$", desc, flags=re.IGNORECASE)`，命中就把捕获组当 JSON 解析成列表塞进 `schema["enum"]`，并从描述里删掉这一段。docstring 明确写了这条约定必须放在行尾，否则解析不到。注意这里用的是 `json.loads`，所以候选值必须是合法 JSON（字符串要带引号）。

顺带一提，`_parse_type_hint` 对真正的 `Literal[...]` 也有处理（取所有值类型的并集再补 `enum`），所以**两条路都能产生 enum**：类型注解里的 `Literal` 与 docstring 里的 `(choices: ...)`。

**必填与可空的判定分离在两层。** `_convert_type_hints_to_json_schema` 里：

- `properties` 由 `get_type_hints(func)` 逐项 `_parse_type_hint` 得到——注意它遍历的是**类型注解**，所以有注解但没在签名里的名字不会出现。
- `required` 由 `inspect.signature` 遍历参数得到：`param.default == inspect.Parameter.empty` 就进 `required`，否则给该参数的 schema 打 `nullable = True`。
- 还有一个补漏：若某参数在签名里但不在 `properties` 里（注释场景是没注解），先补一个空 dict，避免后面访问 KeyError。但如果 `error_on_missing_type_hints` 为真，没注解会先抛 `TypeHintParsingException`。
- 返回值的特殊处理：若 `properties["return"]["type"]` 不是字符串（即多类型联合），**整体降级为 `"any"`**。注释只写了「multi-type union -> treat as any」，没有解释——大概是 return 的联合类型对模型意义不大。

`nullable` 的语义要小心：它由**是否有默认值**决定，而不是由类型里有没有 `None`。但 `_parse_union_type` 在遇到 `Optional[X]` 时也会加 `nullable: True`。两条路都能置位，说明 `nullable` 是个「可能有多个来源」的标记。

**`_parse_type_hint` 是一条按 `get_origin` 分派的递归。** 分支覆盖：

- `origin is None`：基础类型，查 `_BASE_TYPE_MAPPING`（`int`→integer、`float`→number、`str`→string、`bool`→boolean、`list`→array、`dict`→object、`Any`→any、`NoneType`→null）。查不到就再走 `_get_json_schema_type` 的补充逻辑（`Image`→image、`Tensor`→audio），最后兜底 `{"type": "object"}`。注意兜底不是抛错——自定义类会被当成 object，这与 `origin is None` 分支里 `KeyError` 后抛 `TypeHintParsingException` 的行为**不一致**（实际上 `_get_json_schema_type` 从不会抛 `KeyError`，所以那句 except 是死代码）。
- `Union` / `types.UnionType`：交给 `_parse_union_type`。
- `list[X]` → `{"type": "array", "items": ...}`；裸 `list` → `{"type": "array"}`。
- `tuple`：单元素元组**直接抛错**并建议改用 `List[]`；含 `...` 的变长元组也抛错；其余用 `prefixItems` 表达定长元组。
- `dict[K, V]` → `{"type": "object", "additionalProperties": ...}`（只有两个参数时才填值类型，键类型被丢弃——JSON 的对象键只能是字符串）。
- `Literal[...]` → 先取所有值的类型求并集，再补上 `enum`（排除 `None`）。
- 其他 origin 抛 `TypeHintParsingException`。

**`_parse_union_type` 的三档合并。** 先剔除 `NoneType` 再逐个递归：

- 只剩一个非空类型 → 直接用它的 schema（`Optional[str]` 化简为 `{"type": "string", "nullable": true}`）。
- 全部是基础字符串类型 → 用**排序后**的类型名列表（`int | str` → `{"type": ["integer", "string"]}`）。排序是为了稳定输出。
- 否则用 `anyOf` 装各子 schema。

最后若原 args 里含 `NoneType`，给结果加 `nullable: True`。

**`_get_json_schema_type` 是「类型 → 字符串」的唯一映射表，且被三个地方复用**：`_parse_type_hint` 的基础类型分支、声明侧 `validate_arguments` 的签名核对、调用侧 `validate_tool_arguments` 的值类型比对。这是整个类型体系的实际锚点——`AUTHORIZED_TYPES` 那十个字符串就是这张表的像。其中 `Image` 与 `Tensor` 的判断用 `str(param_type)` 先做字符串比较再 import 真类型，注释没写理由，实际效果是避免为了判断类型而强制 import PIL/torch。

## 关键取舍

**源码事实：** docstring 解析用手写正则而非成熟库。四条正则覆盖「描述 / 参数块 / 参数切分 / 返回块」，`args_split_re` 靠「下一参数起点」做终止前瞻。

**源码事实：** 枚举有两条产生路径：类型注解里的 `Literal[...]`，与 docstring 参数描述末尾的 `(choices: ["a", "b"])`（必须用 `json.loads` 能解析、必须在行尾）。

**源码事实：** `required` 由「有没有默认值」决定，`nullable` 既可能来自默认值也可能来自 `Optional[...]`，所以同一个 `nullable` 标记有两种来源。

**源码事实：** 返回类型的多类型联合会整体降级为 `"any"`；单元素 `tuple` 与含 `...` 的 `tuple` 直接抛错而非近似转换。

**源码事实：** `dict[K, V]` 的键类型被丢弃，只保留值类型进 `additionalProperties`。

**源码事实：** `_parse_type_hint` 的 `except KeyError` 分支是死代码——`_get_json_schema_type` 始终有兜底返回，不会抛 `KeyError`。

**架构推论：** 这套 schema 生成器的目标不是「完整表达 Python 类型系统」，而是「产生模型能读懂的、供应商能接受的 schema」。这解释了两处看起来「信息丢失」的选择：返回联合降级为 any（模型对返回类型本来就不敏感）、dict 键类型丢弃（JSON 对象键只能是字符串）。牺牲表达力换的是**可移植性**——生成的 schema 只用到 JSON Schema 的最小子集。

**架构推论：** 用 docstring 的 `(choices: ...)` 约定表达枚举，而不是强迫用户写 `Literal`，是一个以「提示词可读性」为先的取舍。`Literal["tea", "coffee"]` 对读者不如「The beverage to drink (choices: ["tea", "coffee"])」直观，且后者不依赖类型系统。代价是这条约定只能放在行尾、且必须是合法 JSON——两个都容易写错，且写错了不会报错，只会静默丢掉 enum。

**架构推论：** 手写正则而非用 `docstring_parser`，把「支持哪种 docstring 风格」变成一份可以精确控制的实现。`_parse_google_format_docstring` 只认 Google 风格是显式选择：`Args:` / `Returns:` / `Raises:` 三个标题写死在正则里。好处是零依赖且行为可预期，坏处是换风格就要改正则。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/_function_type_hints_utils.py` | `get_json_schema` | 函数 → 完整 tool schema，合并类型与 docstring，处理 enum 约定 |
| `src/smolagents/_function_type_hints_utils.py` | `_parse_google_format_docstring` | 用四条正则切出描述、参数说明、返回说明 |
| `src/smolagents/_function_type_hints_utils.py` | `_convert_type_hints_to_json_schema` | 仅从类型注解生成 properties/required，不要求 docstring |
| `src/smolagents/_function_type_hints_utils.py` | `_parse_type_hint` | 按 `get_origin` 递归转换，含 tuple/dict/Literal 的特殊规则 |
| `src/smolagents/_function_type_hints_utils.py` | `_parse_union_type` | 联合类型的三档合并（单类型 / 基础类型列表 / anyOf） |
| `src/smolagents/_function_type_hints_utils.py` | `_get_json_schema_type` | 类型 → 字符串类型的唯一映射表，三处复用 |
| `src/smolagents/_function_type_hints_utils.py` | `TypeHintParsingException` / `DocstringParsingException` | 两类解析失败，区分「类型问题」与「文档问题」 |
| `src/smolagents/tools.py` | `tool` | `@tool` 装饰器，直接消费 `get_json_schema(...)["function"]` 的四个字段 |

## 参考

- [smolagents 源码：_function_type_hints_utils.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/_function_type_hints_utils.py)
- [smolagents 文档：工具与 schema](https://huggingface.co/docs/smolagents/tutorials/tools)
- [Google Python 风格指南：docstring](https://google.github.io/styleguide/pyguide.html#38-comments-and-docstrings)
- [JSON Schema 文档：type 关键字](https://json-schema.org/understanding-json-schema/reference/type)

## 与主核 / AI 的关系

「从声明推导出机器可消费的元数据」这件事，主核里对应的是**目录自省**：从 `CREATE TABLE` 的声明生成列定义、类型、约束，供规划器与协议层使用。两边都是从人类书写的声明里提取结构化信息，都需要处理「声明不完整」（缺类型、缺描述）与「声明不可表达」（自定义类型）两类缺口。主核遇到不可表达的类型会报错（宁可拒绝）；这里对自定义类兜底成 `object`（宁可放过）——因为目标是让模型**有东西可看**，而不是保证严格。

`nullable` 的双来源（默认值 / `Optional`）是一个值得警惕的设计。主核里同类信息通常只有一个来源（`NOT NULL` 约束），因为多来源意味着「两个地方可能不一致」。这里靠着 `Tool.validate_arguments` 的双边一致检查把风险压住了（见 d01），但它仍然是一个需要人力维持的不变式。

对 AI 工程的实际意义在于：这份实现说明了「工具 schema 生成」的三条真实约束。第一，**必须处理 docstring**，因为参数语义只能从那里来，而模型对参数语义的依赖远大于对类型的依赖。第二，**必须容忍不完整声明**，否则大量现成函数无法直接变工具。第三，**必须输出最小子集**，因为不同供应商支持的关键字不同——`anyOf`、`prefixItems` 这些只在必要时才用，`$ref` 完全没有，正是这个原因。最后那条 `(choices: ...)` 约定值得单独评价：它用自然语言里的括号表达枚举，牺牲了可校验性（写错静默失效）换取了提示词可读性，是一个典型的「面向模型的 API 设计」取舍。
