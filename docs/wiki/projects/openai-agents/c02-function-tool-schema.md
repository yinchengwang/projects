---
project: openai-agents
feature: function-tool-schema
title: 函数工具与 schema 生成
page_type: feature
schema_version: 1
refs:
  - path: src/agents/function_schema.py
    symbols: [function_schema, generate_func_documentation, FuncSchema]
  - path: src/agents/strict_schema.py
    symbols: [ensure_strict_json_schema, _ensure_strict_root]
---

# 函数工具与 schema 生成

## 一句话职责

把一个普通 Python 函数的签名、类型注解与文档字符串，翻译成模型能读的（名字、描述、参数 JSON Schema）三元组，并把这份 schema 规整成模型严格模式要求的封闭形状。

## 机制是什么

`function_schema(func, ...)` 返回一个 `FuncSchema`，六个关键字段：`name`、`description`、`params_pydantic_model`（动态生成的参数模型类）、`params_json_schema`（由该模型导出的 schema）、`signature`（原函数签名）、`takes_context`（是否吃上下文参数）、`strict_json_schema`（是否已规整）、`return_annotation`（含 `Annotated` 元数据的返回注解）。

`FuncSchema.to_call_args(data)` 是反向的一步：把校验过的 pydantic 实例还原成 `(args, kwargs)`，供真正调用原函数使用。

两道 strict 规整的入口在 `strict_schema.py`：`ensure_strict_json_schema` 做递归改写，`_ensure_strict_root` 在顶层加一道额外约束。

## 怎么实现的

**文档字符串的三种风格。** `generate_func_documentation` 先用 `_detect_docstring_style` 打分：sphinx 看 `:param`/`:type`/`:return:`/`:rtype:` 四类行首标记；numpy 看 `Parameters`/`Returns`/`Yields` 后跟三行以上的横线；google 看 `Args:`/`Arguments:`/`Returns:`/`Raises:` 这类段落头。全零分退回 `google`，同分时优先级是 sphinx > numpy > google。检测完之后交给 griffe 的 `Docstring` 解析，取第一个 text 段落作描述、取 parameters 段落的逐参数描述。参数名会做 `lstrip("*")`——因为 google/numpy 风格里变参写成 `*args:` / `**kwargs:`，griffe 原样返回带星号的名字，而签名里的名字不带星号。

**google 风格的一个 griffe 陷阱。** griffe 在「段落头上一行非空、下一行缩进」时会静默跳过这个段落头，只记一条 "Missing blank line above section"，后果是所有参数描述丢失、`Args:` 原文漏进描述里。`_ensure_blank_line_before_google_sections` 专门修补这一点：只对参数段落头（`Args:` 及其别名）在需要时插入一个空行；不需要插入时直接返回原对象，保证良构文档串逐字节不变。源码注释还说明了为什么只处理参数段落：`generate_func_documentation` 只消费参数段落与第一个 text 段落，其它段落故意不动。

**上下文的识别规则。** 第一参数的注解若是 `RunContextWrapper` 或 `ToolContext`（比较的是 `get_origin(ann) or ann`，因此泛型参数化形式也能识别），就置 `takes_context=True` 并从参数列表里剔除。后续任何位置出现这两个类型直接 `UserError`。这条规则与 `tool.py` 里 `_get_function_tool_invoke_context` 配合，决定调用时要不要塞 `ctx`。

**Annotated 的双重用途。** 类型注解先经 `_strip_annotated` 剥出底层类型与元数据元组；元数据里第一个字符串被当作参数描述（`_extract_description_from_metadata`）。描述优先级是 `setdefault` 语义：文档串里的描述先写入，`Annotated` 的描述只在文档串没给该参数时补位。

**变参的两种处理。** `*args: int` 转成 `list[int]`，默认 `default_factory=list`；若注解本身是 `tuple[T, ...]`，则取 `T` 作为逐元素类型再包 `list`，而固定长度 `tuple[A, B]` 直接 `UserError`——因为变参注解描述的是每一个位置参数，固定元组无法表达。`**kwargs: X` 转成 `dict[str, X]`，但**在 strict 模式下直接 `UserError`**：strict schema 不允许开放对象，报错信息给出的出路由是「关掉 strict_mode 改用嵌套对象」或「改用显式参数」。非 strict 时给模型加 `extra="forbid"`，避免扁平键被静默丢弃。

**Field 信息的合并优先级。** 普通参数分三条路：注解本身带 `Annotated[..., Field(...)]` 时用 `FieldInfo.merge_field_infos` 合并（描述取文档串或注解自带的），若默认值是 `FieldInfo` 则走 `from_annotated_attribute` 再合并；无默认值则是必填 `Field(...)`；有默认值则 `Field(default=..., description=...)`。这段逻辑的复杂度来自一个目标：让「注解里的约束」「文档串里的描述」「签名里的默认值」三处来源都能保留，且不至于互相覆盖成错。

**`to_call_args` 的读法。** 它优先读实例的 `__dict__` 而不是 `getattr`，注释写明原因是避免 pydantic 的 `model_extra` / `model_fields_set` 属性遮蔽同名的工具参数；也不用 `model_dump()`，因为它会把嵌套模型转成字典。参数按签名顺序遍历：`VAR_POSITIONAL` 展开进位置参数并置标记；`VAR_KEYWORD` 更新关键字参数；位置或位置/关键字参数在「未见过 `*args`」时进位置参数，见过之后改走关键字；仅关键字参数一律进关键字。

**`**kwargs` 与具名参数冲撞的处理。** `_raise_on_var_keyword_collisions` 收集「既可作为位置/关键字绑定、又是仅关键字」的具名参数集合，与 `**kwargs` 的键取交，非空就 `ModelBehaviorError`。理由写得很清楚：`**kwargs` 最后展开，撞名要么覆盖掉模型给的值（那个值是 pydantic 校验过的），要么让调用报「got multiple values for argument」，两者都与 schema 的承诺不符。位置专用参数与 `*args` 故意不列入保留名——对 `def f(a, /, **kw)`，`f(1, a=2)` 合法且 `a=2` 进 `kw`。

**strict 规整的七个动作。** `_ensure_strict_json_schema` 递归处理：

1. **类型补全**：没有 `type` 但有 `properties` 的直接补成 `object`；没有 `type` 且 `additionalProperties` 不是 `False` 的抛错。
2. **封闭对象**：对象类型缺 `additionalProperties` 就补 `False`；已有但不是 `False` 就报错——这里特意用 `is not False` 而不是真值判断，因为 OpenAPI/MCP 常用 `additionalProperties: {}`（空 schema 意为「任意允许」），它在 Python 里是假值，用真值判断会静默放过一个非 strict schema。
3. **全程必填**：对 `properties` 里的所有键写入 `required`。
4. **联合改写**：`anyOf` 逐项递归；`oneOf` 被合并进 `anyOf` 再删掉原键，因为 OpenAI 的结构化输出在嵌套上下文不支持 `oneOf`。
5. **单元素 `allOf` 展开**：只含一项时内联合并（`_merge_single_all_of`），若父层与该项在约束键上冲突则报错；显式关过 `additionalProperties` 的父层要保留这个设置。
6. **去掉 `None` 默认值**：`default` 为 `None` 时删除，schema 仍是可空，模型自然会给 `None`。
7. **`$ref` 内联**：带额外兄弟键的 `$ref` 无法同时保留两者，于是把引用解开、用 `{**resolved, **json_schema}` 合并（本地键优先）后重新递归。

**两道安全阀。** `_MAX_SCHEMA_DEPTH`（100）在每一次递归入口检查；`_MAX_SCHEMA_NODES`（100_000）由 `_NodeBudget` 计数，`$ref` 每展开一次也扣一次额度。注释点名这是防「恶意 `$ref` 扇出」造成指数级展开的拒绝服务面——典型来源是第三方 MCP 服务器广告的工具 schema。嵌套资源（带 `$id` 的子 schema）里的 `$ref` 一律拒绝，因为无法相对文档根解析。

**顶层的额外约束。** `_ensure_strict_root` 做两件事：根节点不得使用 `anyOf`；根节点 `type` 若是列表且包含 `object`，必须恰好是 `["object"]`，否则 `UserError`——严格模式要求根是非空对象。

**空 schema 的特例。** 传入 `{}` 时直接返回 `_EMPTY_SCHEMA`（`type: object`、`properties: {}`、`required: []`、`additionalProperties: false`）。这对应无参工具的常见情形，避免走完整递归。

## 关键取舍

**源码事实：** strict 模式对 `**kwargs` 是硬拒绝而不是降级处理。代价是「用字典收零散参数」这种 Python 惯例在工具里不可用；收益是 schema 永远封闭，模型不可能给出 schema 外的键。

**源码事实：** `required` 被写成「所有 properties 键」。这意味着在 strict 模式下**参数没有可选这一说**——Python 里带默认值的参数，在 schema 里也是必填，模型必须显式给出。可选性只能靠「值本身可空」或者「让模型自己按默认值填」来表达。

**源码事实：** `oneOf` 被改写为 `anyOf` 并丢弃原键。语义上 `oneOf` 要求恰好匹配一支、`anyOf` 允许匹配多支，因此这是一个**放宽**而不是等价变换。注释承认「对判别联合提供等价功能」，实际成立的前提是各分支由判别字段互斥。

**源码事实：** 单元素 `allOf` 合并会检查冲突并在冲突时报错，而不是静默选一边。冲突判定用 `_json_values_equal`，它对 `bool` 与 `int` 做了类型区分（`True == 1` 在 Python 成立，但在 JSON 语义里不应等价）。

**源码事实：** schema 深度与节点数都有上限，且 `$ref` 展开也扣节点额度。这让「schema 多大」成为一个可预测的成本，而不是由上游 schema 的结构决定。

**架构推论：** 这一层做的事本质上是「把 Python 的开放签名压缩成 JSON 的封闭对象」。压缩必然会丢东西：可选性丢了、变参丢了、`**kwargs` 丢了、`oneOf` 的精确性丢了。所以理解「哪些 Python 特性在工具定义里会变形」比理解实现细节更重要——它决定了哪些签名能安全地当工具用。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/function_schema.py` | `function_schema` | 主入口：文档串 + 注解 + 签名 → `FuncSchema` |
| `src/agents/function_schema.py` | `FuncSchema` | 生成的参数模型、JSON schema、签名、上下文标记 |
| `src/agents/function_schema.py` | `FuncSchema.to_call_args` | 参数模型 → `(args, kwargs)`，含变参与 `**kwargs` 冲撞检查 |
| `src/agents/function_schema.py` | `generate_func_documentation` | 文档串解析：风格检测 + griffe + 参数描述提取 |
| `src/agents/function_schema.py` | `_detect_docstring_style` | sphinx/numpy/google 三种风格打分与优先级 |
| `src/agents/function_schema.py` | `_ensure_blank_line_before_google_sections` | 修补 griffe 对紧凑 `Args:` 段落的静默丢弃 |
| `src/agents/strict_schema.py` | `ensure_strict_json_schema` | strict 规整的唯一入口，带深度与节点预算 |
| `src/agents/strict_schema.py` | `_ensure_strict_json_schema` | 递归改写：封闭对象、全必填、联合归一、`$ref` 内联 |
| `src/agents/strict_schema.py` | `_ensure_strict_root` | 根节点禁止 `anyOf`、必须是纯对象 |
| `src/agents/strict_schema.py` | `_NodeBudget` | `$ref` 扇出防护的节点计数器 |
| `src/agents/strict_schema.py` | `_merge_single_all_of` | 单元素 `allOf` 的内联合并与冲突检测 |

## 参考

- [openai-agents-python 源码：function_schema.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/function_schema.py)
- [openai-agents-python 源码：strict_schema.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/strict_schema.py)
- [OpenAI 文档：结构化输出支持的 schema](https://platform.openai.com/docs/guides/structured-outputs)

## 与主核 / AI 的关系

这一层与编译器前端里的「语法树降级到目标 IR」同类：源语言的表达力（Python 签名）比目标格式（strict JSON Schema）强，降级时必须显式决定每一项丢掉什么、以及丢不掉时是否报错。这里的选择是「丢不掉就报错」——`**kwargs`、固定长度元组、带约束兄弟键的 `$ref` 都会让构造直接失败，而不是悄悄产出弱化的 schema。

对 agent 工程最实用的三条推论。其一，**工具签名要写成「严格模式友好」的形状**：全部具名参数、不用 `**kwargs`、不用固定元组、用 `Field` 显式表达约束，编译期就能过。其二，**strict 模式下参数全部必填**，想表达可选就让类型可空并给 `None` 一个明确语义；否则模型被迫编造一个值。其三，**schema 的大小有硬上限**：把大对象（例如整个配置树）当作工具参数会让节点数爆炸；需要时改成「传 id、由工具自己去取」，把体积从 schema 移到运行时。
