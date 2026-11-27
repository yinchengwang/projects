---
project: openai-agents
feature: structured-output
title: 结构化输出与严格模式校验
page_type: feature
schema_version: 1
refs:
  - path: src/agents/agent_output.py
    symbols: [AgentOutputSchema, AgentOutputSchemaBase]
---

# 结构化输出与严格模式校验

## 一句话职责

把「agent 的最终输出应该是什么类型」变成一份可发给模型的 JSON Schema 加一个校验器，并对「不能表示成 JSON 对象」的类型自动套一层 `response` 包装；开启严格模式时还要求 schema 满足一套更窄的子集。

## 机制是什么

`AgentOutputSchemaBase` 是抽象接口，五个方法：`is_plain_text()`、`name()`、`json_schema()`、`is_strict_json_schema()`、`validate_json(json_str)`。接口注释明确指出：`json_schema()` **只会在输出类型不是纯文本时被调用**——所以「纯文本」是这条契约里的第一类分支。

`AgentOutputSchema` 是唯一实现，用 `@dataclass(init=False)` 加手写 `__init__`，四个私有字段：`_type_adapter`（pydantic 的 `TypeAdapter`）、`_is_wrapped`（是否套了包装）、`_output_schema`（JSON Schema 字典）、`_strict_json_schema`（是否严格模式）。

它的构造做了四步：判纯文本、判是否要包装、建 TypeAdapter 与 schema、按需过严格化。

## 怎么实现的

**纯文本的早退。** `output_type` 是 `None` 或 `str` 时，`_is_wrapped = False`，`TypeAdapter(output_type)` 与 `json_schema()` 都建好就返回。注意此时**仍然建了 TypeAdapter**，所以 `validate_json` 是可用的（对 `str` 来说就是校验它是字符串）。但 `is_plain_text()` 为真，所以调用方根本不会去取 `json_schema()`——而且真取会抛 `UserError("Output type is plain text, so no JSON schema is available")`。

这个「接口说不会被调用，实现里仍然显式抛错」的搭配是防御性的：契约一旦被误用，立刻得到明确错误而不是一个无意义的空 schema。

**包装判定。** `_is_wrapped = not _is_subclass_of_base_model_or_dict(output_type)`。`_is_subclass_of_base_model_or_dict` 处理两种情况：泛型别名（`get_origin` 非空）看 origin 是否是 `BaseModel` 或 `dict` 的子类；普通类型直接 `issubclass`。

所以判定标准是「能否自然地表示成一个 JSON 对象」。`dict` 与 `BaseModel` 子类可以；`list[str]`、`int`、`Union[...]` 这些不行——它们要么是数组要么是标量，而 Responses 的结构化输出期望顶层是对象。

**包装的具体形状。** 需要包装时，动态建一个 `TypedDict("OutputType", {"response": output_type})`，再用它建 TypeAdapter 与 schema。所以发给模型的 schema 形状是 `{"type": "object", "properties": {"response": ...}, "required": ["response"]}`，而校验回来时要把 `["response"]` 取出来。

`_WRAPPER_DICT_KEY = "response"` 是模块常量。取这个键名而不是像 `value`/`data` 之类的，大约是因为它与「模型的响应」语义贴合。

**严格化。** 若 `_strict_json_schema`（默认 `True`）为真，调 `ensure_strict_json_schema(self._output_schema)`。失败时把 `UserError` 包成一条可操作的错误：文案告诉用户两条路——「让输出类型本身符合严格 schema」或「用 `AgentOutputSchema(YourType, strict_json_schema=False)` 包一下」。

这个错误文案的价值在于给出了**具体的构造写法**，而不只是说「你的类型不行」。所有「配置期就能发现的问题」都应该这样报错。

**校验与脱敏分支。** `validate_json(json_str)` 调 `_json.validate_json(json_str, self._type_adapter, partial=False, strict=True if self._strict_json_schema else None)`——注意 `partial=False`，即不接受部分 JSON。若抛 `ModelBehaviorError` 且该错误「数据已脱敏」（`_is_error_data_redacted`），则摘掉 traceback 帧、暂存错误、**把 `json_str` 换成 `"<redacted>"`**，再重抛。

这段代码里有一行 `self = cast(Any, None)`——它把局部变量 `self` 置空。目的与 a02 那页的运行入口一致：让当前栈帧的局部变量不再可达原始数据。虽然 `validated` 在那个分支里可能未定义，但既然马上要抛，就不追究。

**包装解包的两道检查。** 若 `_is_wrapped`，校验结果必须是 dict，否则附一个 span 错误并抛 `ModelBehaviorError`（带实际类型与原始 JSON）。dict 里必须有 `response` 键，否则同样附 span 错误并抛出。都通过才返回 `validated[_WRAPPER_DICT_KEY]`。

两个失败分支都调 `_error_tracing.attach_error_to_current_span(SpanError(...))`。所以「模型返回的形状不对」这类失败会记到当前 span 上，在追踪里能直接看到。

**`name()` 的字符串化。** `_type_to_str` 递归构造类型名：无 origin 用 `__name__`，有 args 就 `Origin[Arg1, Arg2]` 递归拼接。所以 `list[str]` 得到 `"list[str]"`，`dict[str, int]` 得到 `"dict[str, int]"`。这个名字会进 span 的 `output_type` 字段，也用于给模型看的提示。

## 关键取舍

**源码事实：** 严格模式默认开启（`strict_json_schema: bool = True`）。注释用加粗强调「强烈建议设为 True，因为它提高正确 JSON 输入的可能性」。所以默认值是「更严」而不是「更宽」。

**源码事实：** 包装是**自动**的，不需要用户声明 `list[int]` 要包一层。用户拿到的最终输出仍是列表（`validate_json` 会解包），所以从使用侧看不出包装存在——只有看 schema 时才会发现顶层是对象。

**源码事实：** 纯文本时 `json_schema()` 抛 `UserError`，而不是返回一个表示字符串的 schema。所以「不要给纯文本类型取 schema」是接口层面的强制约定，而非可选建议。

**源码事实：** `validate_json` 里 `partial=False`，即严格解析——不允许尾随内容或部分 JSON。这与 `strict` 参数是两件事：前者管「整串必须恰好是一个 JSON 值」，后者管「校验时是否启用 pydantic 的严格类型转换」。

**架构推论：** 自动包装解决的是一个协议限制与 Python 类型系统之间的错配。Responses 的 structured output 要求顶层是 JSON 对象（`json_schema` 格式），但用户自然会想「输出一个列表」或「输出一个整数」。包装把「受限的协议」与「自然的类型」解耦：协议侧永远是对象，用户侧永远是原类型。

**架构推论：** 严格模式的实现方式是**改写 schema**而不是改写校验器。`ensure_strict_json_schema` 把可选字段补进 `required`、给所有对象加 `additionalProperties: false`——这是 OpenAI 严格结构化输出格式的要求。所以「严格」有两层效果：让模型更难产出不合规的 JSON，以及让校验更严。这也是为什么关闭严格模式要显式写出来：它同时放松了两侧。

**架构推论：** 把「schema 生成」与「校验」放在同一个对象上，而不是分成两个独立的工具，是因为两者必须共享同一份「是否包装」与「是否严格」的决策。若分开，调用方有可能发出包装后的 schema 却按未包装的方式解析——这类不一致非常难调试。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/agent_output.py` | `AgentOutputSchemaBase` | 输出契约抽象：类型、schema、严格性、校验 |
| `src/agents/agent_output.py` | `AgentOutputSchema` | 唯一实现，含包装与严格化 |
| `src/agents/agent_output.py` | `AgentOutputSchema.__init__` | 判纯文本、判包装、建 schema、按需严格化 |
| `src/agents/agent_output.py` | `AgentOutputSchema.is_plain_text` | 输出类型是否为 `None` 或 `str` |
| `src/agents/agent_output.py` | `AgentOutputSchema.json_schema` | 纯文本时抛错，否则返回 schema |
| `src/agents/agent_output.py` | `AgentOutputSchema.validate_json` | 校验并解包，失败时附 span 错误 |
| `src/agents/agent_output.py` | `_is_subclass_of_base_model_or_dict` | 判断类型能否直接表示成 JSON 对象 |
| `src/agents/agent_output.py` | `_type_to_str` | 递归构造可读类型名，供 span 与提示使用 |
| `src/agents/agent_output.py` | `_WRAPPER_DICT_KEY` | 包装用的键名 `response` |

## 参考

- [openai-agents-python 源码：agent_output.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/agent_output.py)
- [OpenAI 文档：Structured Outputs 支持的 schema](https://platform.openai.com/docs/guides/structured-outputs)
- [Pydantic 文档：TypeAdapter](https://docs.pydantic.dev/latest/concepts/type_adapter/)

## 与主核 / AI 的关系

主核里最接近的是「结果集描述符」：查询返回的列类型由目录声明，客户端据此反序列化。差别在于主核的类型描述是**被动的**（只用于解释收到的字节），这里的 schema 是**主动的**（要发给模型，影响模型如何生成）。这种「描述即约束」的性质，是结构化输出与前结构化时代最大的区别。

与 CrewAI 的工具 schema 相比（见 crewai 那批 f 组页面），这里的严格模式是**默认开**的，而 CrewAI 的 `convert_tools_to_openai_schema` 把 `strict: True` 硬编码在工具 schema 上。两者都在追求「让模型产出可解析的 JSON」，但处置路径不同：这里允许用户显式关掉并承担风险，CrewAI 那边是统一强制。

对 AI 工程最实用的一条是那个自动包装。当协议要求顶层是对象而用户想返回列表时，有三个选择：拒绝用户的类型、让用户自己包、自动包。第一种太严苛，第二种把协议细节泄漏给用户，自动包是唯一不牺牲可用性的做法。代价是实现里必须记住「包过没有」，并在校验后解包——所以这个状态一定要和 schema 存在同一个对象里。

另一条是严格化的实现层次。很多人会想「在 pydantic 校验时用 `strict=True` 就够了」，但那只控制了输入的类型转换，控制不了模型会不会生成缺字段的对象。真正的严格要把 schema 里的可选字段全部提升为必填、并禁止额外属性——这是**改变协议**而不只是改变校验。理解这一点，才能解释为什么 `strict_json_schema=False` 是个需要用户显式选择的选项，而不是内部实现细节。
