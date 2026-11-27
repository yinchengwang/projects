---
project: pydantic-ai
feature: output-modes
title: 四种输出模式
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/output.py
    symbols: [TextOutput, ToolOutput, NativeOutput, PromptedOutput]
---

# 四种输出模式

## 一句话职责

用四个标记类把「模型怎样把结构化结果交回来」这件事显式化：走工具调用、走供应商原生 schema、走提示词约束的纯文本，还是走纯文本后由应用函数加工——选哪一种不改变输出类型，只改变传输通道。

## 机制是什么

四个标记类的字段极少，都是「把一个或多个输出类型包起来，外加少量通道级开关」：

| 标记 | 通道 | 独有字段 |
|---|---|---|
| `ToolOutput(type_)` | 把输出变成一个工具，模型调它来提交结果 | `name`、`description`、`max_retries`、`strict`、`sequential` |
| `NativeOutput(outputs)` | 请求里带 `response_format` 之类的原生 schema 参数 | `name`、`description`、`template` |
| `PromptedOutput(outputs)` | 完全靠提示词里的 JSON schema 约束，模型直接写字 | `name`、`description`、`template` |
| `TextOutput(output_function)` | 模型只写文本，文本交给这个函数加工 | 无（只有 `output_function`） |

除了这四个，还有两个非标记的输出声明可以混在 `output_type` 里：裸 `None`（允许无输出）与 `PendingModelResponse`（允许运行以「供应商还在算」结束）。它们不决定通道，只扩展可接受的结局。

`OutputMode` 字面量把实际通道枚举为 `'text'`、`'tool'`、`'native'`、`'prompted'`、`'image'`、`'auto'`，其中 `'auto'` 是「让模型的 profile 决定」，`'tool_or_text'` 是保留但已废弃的历史值。

## 怎么实现的

**`ToolOutput` 的两个真实取舍。** `max_retries` 是**每个工具**的重试预算，会覆盖 agent 级重试预算里属于输出侧的那一份；不设则沿用 agent 值。`sequential=True` 只在 `end_strategy='exhaustive'` 下有意义——那种模式下工具默认并行，而「必须独自运行、等前面模型发出的函数工具先完成」的输出工具需要这个标记才能成为屏障。其余策略下输出工具本来就是串行的，设为 `True` 没有效果。

`ToolOutput` 的 `name` 缺省行为分两种：只有一个输出时用 `final_result`；多个输出时把输出类型的名字拼上去。拼之前会先过 `TOOL_NAME_SANITIZER` 去掉 `[`、`]` 这类泛型类名里的非法字符，再由 `_unique_name` 处理重名（撞名就追加 `_2`、`_3`）。

**`NativeOutput` 与 `PromptedOutput` 的构造差异。** `NativeOutput` 的 `outputs` 既可以是单个类型也可以是序列；`PromptedOutput` 的 `template` 除了字符串还能是 `False`，含义是「完全不要 schema 提示」。`False` 的存在是因为某些小模型在提示里塞 JSON schema 反而更差，让应用可以只靠原生参数约束（对 `PromptedOutput` 而言是只靠模型自觉）。两者都保留裸 `None` 项，让 `None` 作为联合成员参与。

**`TextOutput` 只接受函数。** 它的字段叫 `output_function` 而不是「类型」，类型由函数的返回注解推断。原因是它在语义上就是「文本后处理」，没有 schema 可声明。文档明确提醒：流式场景下 `stream_text()` 不会调用这个函数，要拿到加工后的值得用 `stream_output()`——因为逐 token 的文本拿不到一个可调用的完整字符串。

**四个标记在 schema 构建期的优先级。** `OutputSchema.build` 先剥掉 `None`、`DeferredToolRequests`、`BinaryImage` 这三种特殊成员，再按顺序找标记：找到 `NativeOutput` 就直接建 `NativeOutputSchema`；否则找 `PromptedOutput` 建 `PromptedOutputSchema`；两者都没有才走「无标记」路径。也就是说**同一份 `output_type` 里不能混用两个标记**——先出现的那类赢，另一类被当成普通输出类型继续处理。

**无标记路径的四个分支。** 剥完特殊成员后按「文本输出」「工具输出」「其它」三分类：纯文本 → `TextOutputSchema`；有工具输出 → `ToolOutputSchema`（若同时有文本处理器则带上，形成「工具为主、文本兜底」的混合模式）；剩下既非文本也非工具的 → `AutoOutputSchema`；只有图片 → `ImageOutputSchema`。这条路径决定了「什么都不写」时框架默认怎么理解你的 `output_type`。

**`None` 为什么会变成一个工具。** 无标记路径里有一条特判：允许 `None` 且要建输出工具时，会把 `NoneType` 当成一个额外输出类型塞进工具集。这样模型可以在工具调用层面「选择不回答」，而不是只能在文本层面空着——如果没有这条，`output_type=[Foo, None]` 走工具模式时就无法表达「我选 None」。

**schema 与 mode 的两个弱耦合点。** 其一，一个 schema 可以同时有工具集与文本处理器（`ToolOutputSchema` 的 `text_processor` 非空），这时 `mode` 仍报 `'tool'`，`allows_text` 为真——判断「这次真的走了哪条路」要看响应里有没有 `tool_call`，不能只看 `mode`。其二，`native_output_requires_schema_in_instructions` 这个 profile 开关会让原生模式也塞提示词模板，此时 `mode` 是 `'native'` 但提示词里有 schema。

**`auto` 的解析时机在模型侧。** `'auto'` 不是 agent 决定的，而是 `Model.prepare_request` 里通过 `with_default_output_mode(self._default_structured_output_mode(...))` 落定的。默认值来自 profile 的 `default_structured_output_mode`（缺省 `'tool'`），并有一条特例：若默认是 `tool`、有输出工具、模型支持原生 schema 输出，且**强制工具调用会让模型无法思考**，就改用 `'native'`。这条特例说明「工具输出」这个默认并非在所有模型上都等价——对必须保持思考通道的模型，强制 `tool_choice` 会关掉推理。

## 关键取舍

**源码事实：** 四个标记里只有 `TextOutput` 拿的是函数，其余三个拿的是类型（或类型的序列）。这不是设计不一致，而是语义差别：前三个是「让模型产出符合 schema 的东西」，`TextOutput` 是「让模型自由产出，我来解释」。

**源码事实：** `ToolOutput` 是默认承载结构化输出的方式（profile 默认 `default_structured_output_mode='tool'`），它的错误反馈路径最完整——工具调用校验失败可以带 `max_retries` 让模型重试，而原生/提示模式的重试要重新生成整段文本。

**源码事实：** `PromptedOutput(template=False)` 是一个真实的逃生口，不是占位参数。它让应用可以确认「提示里不要再出现 schema」，把约束完全交给模型的原生 JSON 模式或者干脆不约束。

**源码事实：** 同一份输出声明里出现两个标记时，`NativeOutput` 优先于 `PromptedOutput`，且另一类的成员会被当成普通类型并入。源码没有为此报错，因此「混用」是一个静默行为。

**源码事实：** 输出工具名默认是 `final_result`，多输出时会拼接类型名并对泛型类名做字符清洗。这意味着**类名参与模型可见的协议**，重命名一个输出类会改变模型看到的工具名。

**源码事实：** `'auto'` 的判断依赖模型的 profile 与一条「强制工具调用是否关闭思考」的特例。所以同一个 agent 声明在换模型后可能选中不同通道——这是设计意图，不是 bug，但也意味着输出行为不能假定与模型无关。

**架构推论：** 把「输出类型」与「输出通道」拆成两组概念，是这个框架输出层最关键的抽象。类型决定校验与语义，通道决定模型怎么提交。好处是同一份类型声明可以换通道试（工具失败就换提示），代价是「输出到底会以什么形状出现在历史里」必须结合通道才说得清——历史里可能是工具调用，也可能是一条 assistant 文本。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/output.py` | `ToolOutput` | 工具通道标记，带 `max_retries` 与 `sequential` |
| `pydantic_ai_slim/pydantic_ai/output.py` | `NativeOutput` | 原生 schema 通道标记 |
| `pydantic_ai_slim/pydantic_ai/output.py` | `PromptedOutput` | 提示词约束通道标记，`template=False` 可关掉 schema 提示 |
| `pydantic_ai_slim/pydantic_ai/output.py` | `TextOutput` | 文本后处理标记，字段是函数而非类型 |
| `pydantic_ai_slim/pydantic_ai/output.py` | `OutputMode` | 实际通道枚举，含 `'auto'` 与废弃的 `'tool_or_text'` |
| `pydantic_ai_slim/pydantic_ai/output.py` | `PendingModelResponse` | 允许运行以「供应商仍在计算」结束的额外结局 |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `OutputSchema.build` | 标记优先级的唯一入口：先原生、再提示、最后无标记 |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `OutputSchema._build_unmarked` | 无标记时的四分支：文本、工具、自动、图片 |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `OutputToolset.build` | 输出工具集构建：命名、去重、按输出派生工具定义 |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `output_tool_name` | 默认 `final_result` 与多输出时的类型名拼接 |
| `pydantic_ai_slim/pydantic_ai/models/__init__.py` | `with_default_output_mode` | `'auto'` 落定为具体通道，并同步 `allow_text_output` |
| `pydantic_ai_slim/pydantic_ai/models/__init__.py` | `_default_structured_output_mode` | profile 默认值与「强制工具调用关闭思考」特例 |

## 参考

- [pydantic-ai 源码：output.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/output.py)
- [pydantic-ai 源码：_output.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_output.py)
- [Pydantic AI 文档：输出与结构化结果](https://ai.pydantic.dev/output/)

## 与主核 / AI 的关系

四通道设计与主核里「同一份逻辑计划可以有多种物理执行方式」同构：逻辑层声明要什么（输出类型），物理层决定怎么拿到（工具 / 原生 / 提示 / 文本）。主核的物理计划由优化器选，这里的物理通道由标记类显式指定或由 profile 默认决定。

对 agent 工程的直接价值在于：**通道选择是可独立调整的性能与可靠性旋钮**。工具通道最稳（结构化校验 + 可重试），但每轮多一次工具往返；原生通道省一次往返，但依赖供应商实现且失败后只能整段重生成；提示通道兼容性最好，但在小模型上经常产出不合 schema 的 JSON；文本通道最自由，代价是校验完全落在应用侧。遇到「结构化输出偶发解析失败」时，先看当前通道是什么、`max_retries` 给了几次，再考虑换通道——这个顺序比盲目调提示词更有效。
