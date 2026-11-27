---
project: crewai
feature: structured-tool
title: 结构化工具与描述生成
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/tools/structured_tool.py
    symbols: [CrewStructuredTool, from_function, format_description_for_llm]
---

# 结构化工具与描述生成

## 一句话职责

给出工具的第二种形态——一个可以自己 `invoke`、能校验参数、能计配额、还能把结果按 `result_schema` 规范化成 JSON 的对象，同时负责把「名字 + 参数 JSON + 作者描述」拼成模型真正读到的那段提示文本。

## 机制是什么

`BaseTool` 是抽象基类（子类必须实现 `_run`），`CrewStructuredTool` 则是**具体的**：它把可调用对象存在 `func` 字段里，自己实现 `invoke` / `ainvoke`。于是同一套工具体系里有两种可执行形态——声明式的子类，与包函数的实例。

`CrewStructuredTool` 的字段里有两个刻意 `exclude=True`：`func` 与 `cache_function`。它们不参与序列化，因为函数对象没法安全落盘；其余字段（`name`、`description`、`args_schema`、`result_schema`、`result_as_answer`、`max_usage_count`、`current_usage_count`、`tool_failure_policy`）都进 JSON。`args_schema` 与 `result_schema` 都套了 `Annotated`：入方向用 `BeforeValidator(_deserialize_schema)` 把字典还原成模型类，出方向用 `PlainSerializer(_serialize_schema)` 转回 JSON Schema。

这类最重要的一件事是**描述的两层分离**：`description` 字段只放作者写的原文，模型看到的那段三段式文本由 `format_description_for_llm` 按需合成，通过 `formatted_description` 属性暴露。历史上这两层是混在一起的（构造时把合成结果写回 `description`），现在为了往返幂等专门做了一次拆解。

## 怎么实现的

**合成格式。** `format_description_for_llm(name, args_schema, description)` 产出三行：`Tool Name: <消毒后的名字>`、`Tool Arguments: <缩进 2 的 JSON>`、`Tool Description: <作者原文>`。参数部分取自 `generate_model_description(args_schema)` 的 `json_schema.schema` 再 `json.dumps(indent=2)`；没有 schema 时写 `{}`。

**幂等的入口清洗。** 函数开头先调 `strip_composite_description_prefix`：若传入的 `description` 本身就符合「三段式」形状，只保留最后一个 `Tool Description:` 之后的正文。识别用一条**锚定整段形状**的正则（`^Tool Name:.*\nTool Arguments:.*\nTool Description:`），注释里解释了为什么不宽松匹配——作者写的散文里恰好提到 `Tool Description:` 时不能被误判成合成块。这个清洗让「从旧版本检查点反序列化出来的工具」与「把合成块烘进 `description` 的适配器」都能得到正确结果，而不是套娃成三段嵌套。

**`from_function` 的推导。** 名字默认取 `func.__name__`，描述默认取 `inspect.getdoc(func)` 并 `dedent` + `strip`；两者都拿不到描述时抛 `ValueError`。参数模式三选一：显式 `args_schema`、`infer_schema=True` 时由 `_create_schema_from_function` 建、否则报错要求至少给一个。`_create_schema_from_function` 用 `inspect.signature` 加 `get_type_hints` 遍历参数，跳过 `self`/`cls`，每个字段写成 `(annotation, Field(default=...))`，模型名是 `f"{name.title()}Schema"`。

**签名与模式的交叉校验。** `_validate_func` 是 `mode="after"` 的模型校验器，只在 `func` 非空时调 `_validate_function_signature`。后者只检查一个方向：**函数里无默认值的参数必须在 `args_schema` 里有对应字段**，缺了就报错。反方向（schema 里有而函数里没有）不检查——多出来的字段在调用时会被 `func(**parsed)` 拒掉，这是运行期的事。

**参数解析。** `_parse_args` 接受 `str` 或 `dict`：字符串先 `json.loads`，`JSONDecodeError` 包成 `ValueError`；无 `args_schema` 时直接返回 dict 或空字典；有 schema 时 `model_validate` 再 `model_dump()` 回字典，失败时同样拼上 `build_schema_hint` 的期望参数提示。

**执行与配额。** `invoke` 与 `ainvoke` 的骨架一致：解析参数 → `has_reached_max_usage_count()` 为真则抛 `ToolUsageLimitExceededError` → `_increment_usage_count()` → 执行。差别在执行方式：`invoke` 对协程函数 `asyncio.run`，对返回协程的同步函数也 `asyncio.run`；`ainvoke` 对协程函数直接 `await`，对同步函数走 `asyncio.to_thread`。

同步函数走线程这一步有专门的注释：`to_thread` 与 `run_in_executor` 的区别在于前者**在调用者的上下文中运行**，于是 ContextVar（例如本次运行的集成令牌）能传到工具里。这一句解释了为什么不用更常见的 `run_in_executor`。

**计数器同步。** `_increment_usage_count` 除了自增自己，还会把值写回 `self._original_tool.current_usage_count`。这条反向同步是为了让「从 `BaseTool` 转来的结构化工具」与原始工具共用同一个配额计数——否则同一个工具经两条路径调用的计数会分叉。

**结果规范化。** `format_output_for_agent` 走模块级 `_format_tool_output_for_agent`，三分支：结果本身是 `ToolFailure` 就直接 `as_agent_message()`；有 `_original_tool` 就转发给原始工具（避免双重转换）；否则若有 `result_schema` 就 `model_validate` 再 `model_dump_json`，校验失败发 `RuntimeWarning` 并回落 `str(raw_result)`。

**`_serialize_schema` / `_deserialize_schema` 的对称性。** 前者 `v.model_json_schema() if v else None`；后者对 `None` 与已有类原样返回，对字典调 `create_model_from_schema`，其他类型返回 `None`。所以「模式字段」在 JSON 形态里是一段 JSON Schema，在 Python 形态里是一个 pydantic 类，两者靠这一对函数互换。

## 关键取舍

**源码事实：** `ToolUsageLimitExceededError` 在 `invoke`/`ainvoke` 里是**抛异常**，而 `BaseTool._claim_usage` 是**返回 `ToolFailure`**。同一个语义（配额用尽）在两条路径上行为不同：直接调用结构化工具会炸，经 agent 路径调用会拿到一个失败对象。

**源码事实：** `invoke` 与 `ainvoke` 的配额检查是「先判后加」两步，没有 `BaseTool` 里那种锁保护。并发调用同一个结构化工具时存在竞态，可能双双通过检查。

**源码事实：** `func` 与 `cache_function` 都被 `exclude=True`，所以结构化工具**无法从 JSON 完整复原**——能复原的只有名字、描述、两个 schema 和几个开关。真正的可调用体必须由外部重新注入。这是「工具可序列化」承诺的边界。

**源码事实：** `_format_tool_output_for_agent` 优先转发给 `_original_tool`，这意味着「从 `BaseTool` 转来的结构化工具」用的是原始工具的结果格式化逻辑，而只有「直接 `from_function` 造出来的」才走 `result_schema` 校验路线。

**架构推论：** 描述的两层分离本质上是在修一个**幂等性 vs 兼容性**的矛盾。旧做法把合成块写进 `description`，于是「dump 再 load」会得到嵌套的合成块；新做法保留原文、按需合成，代价是所有读取描述的调用方都必须改用 `formatted_description`，否则会拿到没有参数说明的纯文本。`render_text_description_and_args` 里的回退分支正是为「不提供 `formatted_description` 的鸭子类型工具」准备的。

**架构推论：** `to_thread` 的选择揭示了这套代码对 ContextVar 传播的敏感度。凡是「工具在别的线程里跑」的地方，都必须考虑本次运行的上下文（追踪、集成令牌、事件上下文栈）会不会丢。用 `to_thread` 而不是自己起 executor，是把复制上下文的责任交给标准库。

**架构推论：** `args_schema` 的单向校验（函数必填参数必须在 schema 里）是刻意的宽松：它只防「schema 漏了参数导致调用必然 TypeError」这一类硬错，不试图保证两侧完全一致，因为 schema 可以合法地带额外字段（比如给模型的提示字段）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/tools/structured_tool.py` | `CrewStructuredTool` | 包函数的具体工具，自带 invoke/配额/结果规范化 |
| `lib/crewai/src/crewai/tools/structured_tool.py` | `CrewStructuredTool.from_function` | 从函数建工具，名字与描述有默认来源 |
| `lib/crewai/src/crewai/tools/structured_tool.py` | `CrewStructuredTool.invoke` | 同步执行：解析、查配额、自增、执行 |
| `lib/crewai/src/crewai/tools/structured_tool.py` | `CrewStructuredTool.ainvoke` | 异步执行，同步函数走 `asyncio.to_thread` 保留上下文 |
| `lib/crewai/src/crewai/tools/structured_tool.py` | `CrewStructuredTool._parse_args` | 字符串或字典参数归一，失败时附 schema 提示 |
| `lib/crewai/src/crewai/tools/structured_tool.py` | `CrewStructuredTool._validate_function_signature` | 单向校验：函数必填参数必须存在于 args_schema |
| `lib/crewai/src/crewai/tools/structured_tool.py` | `CrewStructuredTool._increment_usage_count` | 自增并同步回原始工具的计数器 |
| `lib/crewai/src/crewai/tools/structured_tool.py` | `format_description_for_llm` | 合成三段式描述，入口先做幂等清洗 |
| `lib/crewai/src/crewai/tools/structured_tool.py` | `strip_composite_description_prefix` | 从预合成块里取回作者原文 |
| `lib/crewai/src/crewai/tools/structured_tool.py` | `_format_tool_output_for_agent` | 结果格式化：失败对象、原始工具转发、result_schema、回落 |
| `lib/crewai/src/crewai/tools/structured_tool.py` | `build_schema_hint` | 从 JSON Schema 生成期望参数与必填项提示 |
| `lib/crewai/src/crewai/tools/structured_tool.py` | `_serialize_schema` / `_deserialize_schema` | 模式字段在类与 JSON Schema 之间往返 |

## 参考

- [CrewAI 源码：tools/structured_tool.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/tools/structured_tool.py)
- [CrewAI 文档：Tools](https://docs.crewai.com/en/concepts/tools)
- [Python 文档：asyncio.to_thread 与上下文传播](https://docs.python.org/3/library/asyncio-task.html#asyncio.to_thread)

## 与主核 / AI 的关系

主核里也有「同一份逻辑的两种入口」：一个是编译进执行器的原生算子，一个是运行时注册的 UDF。原生算子的参数布局在系统目录里，UDF 的签名在运行期才解析。`CrewStructuredTool` 的 `_original_tool` 反向指针就像 UDF 到原生实现的那层适配——两边都能调，但要共用同一份计数与策略，就必须显式同步状态，而不能指望两个对象各自独立。

与 AutoGen 相比，这里多了两件 AutoGen 工具层没有的东西。第一件是**结果模式**（`result_schema`）与配套的结果规范化：工具可以声明自己返回什么结构，agent 拿到的是序列化后的 JSON 而不是 `str(obj)`。第二件是**描述的两层结构**：`description` 是给人和给配置看的原文，`formatted_description` 是给模型看的带参数说明的三段式，二者由一条锚定正则保证往返幂等。

对 AI 工程最要紧的一条是那个幂等清洗。任何把工具配置落盘再读回的系统，只要曾经把「给模型看的最终文本」写回过描述字段，就会遇到同一份配置越存越长的套娃问题。这里的解法是把「合成」变成读取时的纯函数，并在入口剥掉历史遗留的前缀——这个模式可以直接搬到任何「提示模板被持久化」的场景里。

另一条是 `asyncio.to_thread` 与 `run_in_executor` 的区别。工具里若需要读本次运行的令牌、追踪 id 或事件上下文，走 executor 会静默丢失这些 ContextVar，表现为「工具里拿不到当前运行信息」。选 `to_thread` 是为这条正确性付的代价，换来的是工具内部与调用方看到同一个上下文。
