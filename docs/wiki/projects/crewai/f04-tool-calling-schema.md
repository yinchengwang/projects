---
project: crewai
feature: tool-calling-schema
title: 函数调用式工具协议
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/tools/tool_calling.py
    symbols: [ToolCalling, InstructorToolCalling]
  - path: lib/crewai/src/crewai/utilities/agent_utils.py
    symbols: [convert_tools_to_openai_schema, render_text_description_and_args]
---

# 函数调用式工具协议

## 一句话职责

定两种「模型说它要调什么工具」的最小数据形状，并提供两条把 CrewAI 工具送进模型视野的通道：一条是把 schema 转成 OpenAI 原生 `tools` 格式，另一条是把工具渲染成纯文本塞进提示。

## 机制是什么

`ToolCalling` 与 `InstructorToolCalling` 是两个只有两个字段的 pydantic 模型：`tool_name`（字符串）与 `arguments`（字典或 `None`）。两个类**内容完全一样**，区别只有基类来源与字段构造方式的写法差异——`ToolCalling` 用 `Field`，`InstructorToolCalling` 用 `PydanticField` 别名并继承 `PydanticBaseModel` 别名。这个重复是刻意的：一条路径给「模型原生支持函数调用」时用（`instructor` 约束解码），另一条给「用普通 LLM 把文本转成结构化」时用。

为什么要两个类而不是一个？因为 `_function_calling` 里按能力二选一：

- `self.function_calling_llm.supports_function_calling()` 为真 → 用 `InstructorToolCalling`
- 为假 → 用 `ToolCalling`

两者都塞进同一个 `Converter` 做结构化输出，但落到不同的类上。这套设计的实质是：**同一个语义契约，两个由不同解码机制保证的实现类**。下游 `use` / `ause` 的签名是 `ToolCalling | InstructorToolCalling`，两边都能接。

送进模型的两条通道也在这一页：`convert_tools_to_openai_schema` 产出 OpenAI 原生格式，`render_text_description_and_args` 产出纯文本清单。

## 怎么实现的

**`convert_tools_to_openai_schema` 的三返回。** 返回一个三元组：`openai_tools`（schema 字典列表）、`available_functions`（消毒工具名 → `tool.run`）、`tool_name_mapping`（消毒工具名 → 原工具对象）。后两个让调用方拿到模型回复后能按名字找回可调用体与原对象。

**参数提取。** 对每个工具，若有 `args_schema` 就调 `generate_model_description(tool.args_schema, strip_null_types=False)`，取 `json_schema.schema` 作为 `parameters`，然后 `pop` 掉顶层的 `title` 与 `description`——OpenAI 的 `parameters` 不接受这两个键。取 schema 全程包在 `try` 里，异常时 `parameters = {}`，即退化成「无参数」而不是整条工具失败。

**描述的清洗。** `description = strip_composite_description_prefix(tool.description)`——与 f02 同一套幂等清洗，防止历史遗留的三段式合成块被当成描述再送一遍给模型。

**名字去重。** 消毒名若已存在于 `available_functions`，从 `_2` 开始递增找一个没被占用的后缀（`f"{name}_{counter}"` 再消毒）。所以两个不同来源的同名工具会分别变成 `foo` 与 `foo_2`，模型侧不会混淆。

**schema 形状固定。** 每条是 `{"type": "function", "function": {"name", "description", "parameters", "strict": True}}`。注意 `strict` 被**硬编码为 True**：所有工具都按严格模式声明，无论工具自身是否开了 strict。这是一个显式的取舍——用统一形状换实现简单，代价是「参数有空默认值」的工具在严格模式下可能被模型侧拒绝。

**`render_text_description_and_args` 的宽容。** 对每个工具取 `formatted_description`，判断它是不是 `str`；不是就回退到 `tool.description`。这个 `isinstance` 检查是为了兼容「鸭子类型的工具或测试 mock」——它们没有 `formatted_description` 属性，`getattr` 会返回 `None`。最后用换行连接。

## 关键取舍

**源码事实：** 两个调用模型类字段完全相同，没有任何行为差异。它们的区分价值在外层：`Converter` 走哪条解码路径、以及下游按类型判断时能否区分来源。这是「用类型做标记」而不是「用类型做能力扩展」。

**源码事实：** `arguments` 字段的类型是 `dict[str, Any] | None` 但**必填**（`Field(...)`）。所以模型必须给出这个键，哪怕值是 `None`。这与「参数可能为空对象」的直觉不同——空参数要写成 `{}`。

**源码事实：** `convert_tools_to_openai_schema` 里 `strict: True` 是无条件的。若某个工具的 schema 里有可选字段（有默认值的参数），严格模式下模型必须补齐所有字段，这与工具侧的 `strict=False` 语义不一致。

**源码事实：** 描述取 schema 失败时静默降级为 `{}`，而不是跳过该工具或报错。所以一个 schema 坏掉的工具仍然会出现在模型视野里，只是看起来不需要参数——这会诱导模型给出空参数调用。

**架构推论：** 「两条通道」的分工其实对应模型能力的两个世代。原生支持函数调用的模型读 `tools` 数组，靠服务端约束生成合法 JSON；不支持的老模型只能读提示里的文本清单，再由一个辅助 LLM 把文本解析回结构。同一套工具因此需要两种呈现形态，而 `ToolCalling` 与 `InstructorToolCalling` 就是这两条路的落点。

**架构推论：** 名字去重会把 `foo_2` 这样的名字写进模型看到的 schema，但 `tool_name_mapping` 里记的是原对象。所以「模型说调 `foo_2`」与「实际调 `foo`」之间的映射只在映射表里，任何绕过映射表按名字直查的路径都会找不到工具。这是隐藏耦合，也是这类「改名不改实体」设计的通病。

**架构推论：** 纯文本渲染走的是 `formatted_description` 而不是 `description`，意味着提示里天然带参数 JSON。这解释了为什么文本通道对老模型的可用性还不错：模型能从 `Tool Arguments:` 那一行读到参数结构，虽然它读的是 JSON 而不是 schema。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/tools/tool_calling.py` | `ToolCalling` | 通用调用形状：工具名加参数字典 |
| `lib/crewai/src/crewai/tools/tool_calling.py` | `InstructorToolCalling` | 同形状的变体，供约束解码路径区分 |
| `lib/crewai/src/crewai/utilities/agent_utils.py` | `convert_tools_to_openai_schema` | 转 OpenAI `tools` 格式，去重名字，返回三件套 |
| `lib/crewai/src/crewai/utilities/agent_utils.py` | `render_text_description_and_args` | 渲染纯文本工具清单，兼容鸭子类型工具 |
| `lib/crewai/src/crewai/utilities/agent_utils.py` | `get_tool_names` | 拼接消毒后的工具名清单 |

## 参考

- [CrewAI 源码：tools/tool_calling.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/tools/tool_calling.py)
- [CrewAI 源码：utilities/agent_utils.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/utilities/agent_utils.py)
- [OpenAI 文档：Function calling](https://platform.openai.com/docs/guides/function-calling)

## 与主核 / AI 的关系

主核里最接近的是「客户端协议的两套编码」：同一条查询可以被文本协议解析，也可以被二进制协议解析，两边落到同一棵语法树上。这里的两条通道也归一到同一个语义（工具名 + 参数），差别在于主核的两条协议都由同一方实现、可以共享解析器，而这里的光谱两端——原生 function calling 与文本提示——由**不同的模型、不同的厂商**实现，无法共享约束。

与 AutoGen 相比，AutoGen 的做法是把 `ToolSchema` 作为 `TypedDict` 直接交出去，由各家客户端自己转成厂商格式，核心包不参与「转 OpenAI 格式」这件事。CrewAI 则把转换函数留在本仓的 `agent_utils` 里，并硬编码 `strict: True` 与名字去重——也就是说它承担了「把自家工具模型适配到某个具体厂商协议」的责任。这个差别决定了扩展性边界：AutoGen 换厂商只需换客户端，CrewAI 换协议要改这个转换函数。

对 AI 工程最实用的一条：**两个内容相同的调用模型类是必要的**。当同一份语义由两种不同解码机制产生时，用不同类标记来源，比在字段上加 `source` 字段更可靠——因为类型能参与静态检查与 `isinstance` 分派，而字符串标记不能。代价是下游签名必须写联合类型，任何只处理其中一支的代码都会漏。

另一条是那条 `isinstance(formatted := getattr(tool, "formatted_description", None), str)`。用海象运算符在同一次表达式里完成「取值 + 判类型 + 回退」，是处理鸭子类型接口的简洁写法：既不会因为属性不存在而炸，也不会把 `None` 当字符串拼进提示。写适配层时可以直接借这个模式。
