---
project: pydantic-ai
feature: output-schema
title: 输出 schema 与自动推断
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/_output.py
    symbols: [OutputSchema, AutoOutputSchema, TextOutputSchema]
  - path: pydantic_ai_slim/pydantic_ai/output.py
    symbols: [OutputObjectDefinition, OutputContext]
---

# 输出 schema 与自动推断

## 一句话职责

把用户随手写的 `output_type`（一个类型、一个函数、一个混合列表、或者带标记的对象）编译成一个 `OutputSchema` 对象：它同时回答「模型该怎么提交」「提交后怎么校验」「校验后怎么加工」三个问题，并在支持多个输出时生成一份带路由信息的联合体。

## 机制是什么

`OutputSchema` 是一族 dataclass 的抽象基类，五个字段构成公共契约：

- `allows_none`：是否接受「不回答」。
- `text_processor`：文本通道的处理器，非空即表示该 schema 接受纯文字提交。
- `toolset`：工具通道的输出工具集，非空即表示接受工具调用提交。
- `object_def`：`OutputObjectDefinition`，携带 `json_schema`、`name`、`description`、`strict`。
- `allows_deferred_tools` / `allows_image` / `allows_pending_model_response`：三种额外结局的许可位。

它的 `mode` 属性是多态的：`AutoOutputSchema` 报 `'auto'`、`TextOutputSchema` 报 `'text'`、`NativeOutputSchema` 报 `'native'`、`PromptedOutputSchema` 报 `'prompted'`、`ToolOutputSchema` 报 `'tool'`、`ImageOutputSchema` 报 `'image'`。

`OutputContext` 是另一件事：它是**输出钩子拿到的上下文**，不是 schema 本身。字段包括 `mode`、`output_type`、`object_def`、`has_function`、`function_name`、`tool_call`、`tool_def`、`allows_text`、`allows_image`、`allows_deferred_tools`。

## 怎么实现的

**`build` 与 `_build` 的两段式。** 公共类方法 `build` 先处理两个不参与 schema 形状的特殊输出：`PendingModelResponse`（把它从输出列表里摘出来，只置一个布尔位；若摘完什么都不剩则报错）与后续的 `None`/`DeferredToolRequests`/`BinaryImage`。真正的通道判定在 `_build` 与 `_build_unmarked` 里。这种拆分的原因是 `allows_pending_model_response` 不能影响「用什么通道」——一个还在等的响应不是一个输出格式。

**`AutoOutputSchema` 为什么要带 toolset。** 它同时持有 `processor` 与 `toolset`。源码注释解释了原因：agent 构造器里要检查各工具集的命名冲突，而那一刻可能还不知道最终会用哪种输出模式（模型可能到 `run` 时才定），所以先按「可能走工具模式」预留一个工具集，避免届时才发现名字冲突。这是一个防御性设计——代价是它多持有一个对象，收益是构造期的冲突检查不会漏。

**`TextOutputSchema` 只有处理器没有 schema。** 它的构造器不接受 `object_def`，因为它对应的就是「模型写自由文本」。它的 `text_processor` 是 `TextOutputProcessor`（或 `TextFunctionOutputProcessor`），负责把字符串原样返回或交给用户的输出函数。

**`StructuredTextOutputSchema` 是原生与提示模式的共同父类。** 它比 `TextOutputSchema` 多了 `template` 字段与一个 `build_instructions` 类方法：把 `object_def` 拷一份，把 `name` 写成 `title`、`description` 写上，再用 `json.dumps` 塞进模板的 `{schema}` 占位符。若模板里没有 `{schema}`，会自动在末尾补一段。这个方法的存在意味着「提示词里的 schema」与「工具参数里的 schema」是同一份 JSON，只是包装不同。

**`object_def` 的来源取决于输出类型。** 对 pydantic 模型/`dataclass`/`TypedDict`，schema 由 `TypeAdapter` 生成；对普通函数，取的是**参数**类型而不是返回类型（`ObjectOutputProcessor.output_type` 的文档明确了这点：函数场景下「模型产出的是函数的入参」）。对 `Choices` 这类特殊集合，schema 是一份枚举表。

**联合体的两种走法。** 单输出走 `ObjectOutputProcessor`，多输出走 `UnionOutputProcessor`。联合体不是把多个 schema 简单 `anyOf` 拼起来，而是包一层带判别字段的结构：`UnionOutputModel` 里每个成员带一个 `kind` 标签，`UnionOutputResult` 是这个结构的根。这样校验时能先按 `kind` 定位分支，再对分支做精确校验，而不是让 pydantic 在多个 `anyOf` 分支间猜。`ObjectOutputProcessor._choice_values` 则是 `Choices` 的对应机制：记住每个键代表什么值，执行阶段据此解析。

**`OutputContext` 的三个易错点，源码都写明了。** 其一，`mode` 反映的是**配置的 schema**，不是这次响应的实际格式——一个带 `text_processor` 的 `ToolOutputSchema` 仍报 `'tool'`，即便模型这次返回了文本，判断真实路径要看 `tool_call` 是否为空。其二，`output_type` 对输出函数而言是**入参类型**。其三，`tool_call` 与 `tool_def` 在非工具路径上都是 `None`，输出钩子必须能处理这种空。

**`allows_text` 是个派生属性。** 它没有对应的存储字段，就是 `text_processor is not None`。因此「是否接受文本」与「文本处理器是谁」永远一致，不可能出现「说接受但没有处理器」的坏状态。

**`_output_type_name` 处理 `None` 这个坑。** 输出类型是 `None`（或 `NoneType`）时，模型可见的名字必须写 `'None'` 而不是 `NoneType`——否则模型会看到 `final_result_NoneType`，需要知道 Python 实现细节才能理解为「不回答」。同理 `Annotated[X, ...]` 要按 `X` 命名，因为 `Annotated` 的 `__name__` 是所有被注解成员共享的，直接拼会撞名。

**额外结局的许可位是逐条剥出来的。** `DeferredToolRequests` 在输出列表里出现时会被摘下并置 `allows_deferred_tools=True`，然后继续用剩下的类型建 schema；若剩下为空则报错。`BinaryImage` 同理只是置 `allows_image`。二者都是「不改变通道，只扩展可接受结局」的成员。

## 关键取舍

**源码事实：** `OutputSchema` 把「通道」与「校验/加工」混在一个对象里（既有 `toolset` 又有 `text_processor`）。好处是判定「这次响应能不能接受」只需查一个对象；代价是存在混合态（工具为主 + 文本兜底），此时 `mode` 不再等价于实际路径。

**源码事实：** `AutoOutputSchema` 自称 `'auto'` 但已经持有一个构建好的 `processor` 与 `toolset`。也就是说「自动」只体现在**报告的模式名**上，实际处理能力在构造期就都准备好了。真正的模式选择发生在模型侧 `prepare_request`。

**源码事实：** 联合体用 `kind` 判别字段而非 `anyOf`。这让错误定位更准（能指到具体分支），但也使模型看到的 schema 多一层嵌套——`output_type=[A, B]` 在工具模式下的参数 schema 不是一个干净的 `oneOf`。

**源码事实：** `OutputContext.mode` 的文档明确区分「配置的模式」与「本次响应的格式」。这是一个被反复踩的坑，所以文档用两个例子把它钉死。

**源码事实：** `allows_text` 是纯派生属性而非独立字段，从结构上排除了「声明接受文本但没有处理器」的不一致状态。

**源码事实：** `None` 在命名上被特判成 `'None'`，`Annotated` 被剥到内层命名。这两条都是为了「模型可见的名字必须精确且不撞名」，说明**输出类型的名字是协议的一部分**，不是内部细节。

**架构推论：** 这一层的真正职责是「把开发者随手写的声明归一化成一个运行时可判定的对象」。归一化过程中必然会做判断（哪个标记优先、哪个成员被剥出去当许可位、联合体怎么包），这些判断大多静默进行。这对使用者意味着：写 `output_type` 时理解归一化规则，比理解校验实现更重要——因为前者决定你到底声明了什么。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/_output.py` | `OutputSchema` | 输出 schema 抽象基类：通道、处理器、许可位 |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `OutputSchema.build` | 摘出 `PendingModelResponse` 等特殊成员后交给 `_build` |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `OutputSchema._build_unmarked` | 无标记时的通道判定与分支构建 |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `AutoOutputSchema` | 报 `'auto'` 但预置 processor 与 toolset，用于构造期冲突检查 |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `TextOutputSchema` | 纯文本通道，只有处理器没有 object def |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `StructuredTextOutputSchema` | 原生/提示的共同父类，含 `build_instructions` 模板填充 |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `ObjectOutputProcessor` | 单输出处理器，持有 `object_def`、`validator`、可选输出函数 |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `UnionOutputProcessor` | 多输出的判别式联合处理 |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `_output_type_name` | `None` 与 `Annotated` 的模型可见命名规则 |
| `pydantic_ai_slim/pydantic_ai/output.py` | `OutputObjectDefinition` | schema 的四元组：JSON schema、名字、描述、strict |
| `pydantic_ai_slim/pydantic_ai/output.py` | `OutputContext` | 输出钩子上下文，区分配置模式与实际格式 |
| `pydantic_ai_slim/pydantic_ai/output.py` | `StructuredDict` | 用裸 JSON schema 构造输出类型的入口 |

## 参考

- [pydantic-ai 源码：_output.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_output.py)
- [pydantic-ai 源码：output.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/output.py)
- [Pydantic AI 文档：结构化输出](https://ai.pydantic.dev/output/#structured-output)

## 与主核 / AI 的关系

`OutputSchema` 的角色类似主核里的「执行计划描述符」：它不是执行本身，而是把「做什么、怎么做、什么算成功」编码成一个可被多个下游组件判定的对象。主核用计划描述符统一了执行器、优化器与统计组件；这里的 `OutputSchema` 统一了工具管理器、输出校验器与输出钩子。

对 agent 工程的直接价值在三点。其一，**输出声明会被静默归一化**，因此「我声明了什么」和「框架理解成什么」可能不同——排查结构化输出问题时，先打印实际 schema 的 `mode`、`allows_text`、`object_def` 比读自己的代码更快。其二，**输出类型的名字会进模型视野**（`final_result`、联合体的 `kind` 标签），重命名类不只是代码重构。其三，**联合体多一层嵌套是可靠的代价**：判别字段让错误更可定位，但也让模型面对更复杂的 schema，输出类型超过三四个时值得考虑拆成多次运行而不是一个大联合体。
