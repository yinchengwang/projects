---
project: langchain
feature: functions-and-tools-binding
title: 函数调用格式与工具绑定
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/utils/function_calling.py
    symbols: [convert_to_openai_tool, convert_to_openai_function, tool_example_to_messages]
  - path: libs/core/langchain_core/output_parsers/openai_tools.py
    symbols: [JsonOutputToolsParser, PydanticToolsParser]
---

# 函数调用格式与工具绑定

## 一句话职责

在两个方向上都做归一：**出站**把「字典 / Pydantic 模型 / TypedDict / BaseTool / 普通函数」五种形态压成一份 OpenAI 工具 schema，**入站**把模型吐回来的原始 tool_call 字典还原成 `ToolCall` 或 Pydantic 实例——中间还夹着一层「部分 JSON 能不能解析」的现实妥协。

## 机制是什么

出站侧两个函数：

- `convert_to_openai_function(function, *, strict=None) -> dict` —— 产出**函数体**（`name` / `description` / `parameters` / `strict`）。
- `convert_to_openai_tool(tool, *, strict=None) -> dict` —— 产出**工具**，通常是 `{"type": "function", "function": ...}`，但内置工具原样透传。

入站侧三个层次：

- `parse_tool_call(raw, *, partial, strict, return_id)` —— 单条解析，无效 JSON 抛 `OutputParserException`。
- `parse_tool_calls(raws, ...)` —— 批量解析，收集所有异常后一次性抛出。
- `JsonOutputToolsParser` / `PydanticToolsParser` —— 两级解析器，后者在前者基础上把字典实例化成 Pydantic 对象。

`tool_example_to_messages(input, tool_calls, tool_outputs=None, *, ai_response=None)` 是第三个方向：**把「一个示例」变成一串 few-shot 消息**。

## 怎么实现的

**`convert_to_openai_function` 是一个七分支的类型分派器。** 顺序本身就有信息：

1. `dict` 且含 `name` 与 `input_schema` → **Anthropic 格式**，映射成 `{"name": ..., "parameters": input_schema}`，`description` 有就带。
2. `dict` 且含 `toolSpec` → **Amazon Bedrock Converse 格式**，取 `toolSpec.name` 与 `toolSpec.inputSchema.json`。
3. `dict` 且含 `name` → 已是 OpenAI 格式，**白名单过滤**：`{k: v for k, v in function.items() if k in {"name", "description", "parameters", "strict"}}`。
4. `dict` 且含 `title` → **JSON Schema**，`name` 取 `title`，剩余部分进 `parameters`（条件是 `function_copy and "properties" in function_copy`）。
5. `type` 且是 Pydantic 子类 → `_convert_pydantic_to_openai_function`。
6. `is_typeddict` → `_convert_typed_dict_to_openai_function`。
7. `BaseTool` → `_format_tool_to_openai_function`；`callable` → `_convert_python_function_to_openai_function`。

第 1 与第 2 分支的存在说明「这个函数不只服务 OpenAI」——它把 Anthropic 与 Bedrock 的工具定义也收进同一个出口。所以这个转换层的实际职责是「把各家**输入侧**的声明格式统一」，而不只是「转成 OpenAI」。

第 3 分支的白名单是关键：一个已是 OpenAI 格式的 dict 里若混着别家字段（例如某后端特有的元数据），会被**丢掉**而不是透传。这把转换层变成一个有明确输出形状的函数——与 `convert_to_openai_tool` 里「内置工具原样透传」形成对比（那里透传是因为内置工具的 schema 只有 OpenAI 自己懂，不该被改写）。

**错误消息分两级，且第一级带诊断。** 最终 `else` 里：

```python
if isinstance(function, dict) and ("type" in function or "properties" in function):
    msg = f"Unsupported function\n\n{function}\n\nTo use a JSON schema as a function, it must have a top-level 'title' key to be used as the function name."
    raise ValueError(msg)
msg = f"Unsupported function\n\n{function}\n\nFunctions must be passed in as Dict, pydantic.BaseModel, or Callable. ..."
```

第一个分支专门用于「看起来像 schema 但缺 `title`」的情形——因为那是最常见的用户错误：直接传一个没有标题的 JSON Schema。错误消息直接点出缺什么、以及为什么需要它（「要用作函数名」）。这个细分很有意思：**同一个失败（不支持的输入），按「能不能给出具体补救建议」分成两条消息**。

**`strict` 的处理有两道检查。** 若 `strict` 不是 `None`：

```python
if "strict" in oai_function and oai_function["strict"] != strict:
    raise ValueError(...)
oai_function["strict"] = strict
if strict:
    oai_function["parameters"] = _recursive_set_additional_properties_false(oai_function["parameters"])
```

第一道是**冲突检测**：输入里已经有一个与之矛盾的 `strict` 值时，不静默覆盖而是报错，且错误消息里同时给出原值与新值。第二道是 OpenAI 的 strict 模式的**形状要求**：所有对象节点都要 `additionalProperties: false`，所以一个 `_recursive_set_additional_properties_false` 递归改写整份 schema。

注意只有 `strict=True` 才改写 schema，`strict=False` 只设键不改写。所以「严格模式」是一个**双向承诺**：既要求模型严格按 schema 输出，也要求你的 schema 自己先严格（禁止额外属性）。

而 `if strict and isinstance(function, dict): function = copy.deepcopy(function)` ——那个 deepcopy 只在 `strict` 为真且输入是 dict 时做。理由很清楚：`_recursive_set_additional_properties_false` 会就地改写 `parameters`，若不深拷贝就会污染调用方传入的字典。而其他分支（Pydantic / TypedDict / Tool / 函数）都是「现场生成」新字典，不需要拷贝。

这个条件写成「两个均为真」而不是「只要 strict」，是一个**精确的最小化**：只在真的会修改输入时才拷贝。

**`convert_to_openai_tool` 的三段与透传规则。** 实现：

```python
if isinstance(tool, dict):
    if tool.get("type") in _WellKnownOpenAITools: return tool
    if (tool.get("type") or "").startswith("web_search_preview"): return tool
if isinstance(tool, Tool) and (tool.metadata or {}).get("type") == "custom_tool":
    ... return {"type": "custom", "name": ..., "description": ..., ["format": ...]}
oai_function = convert_to_openai_function(tool, strict=strict)
return {"type": "function", "function": oai_function}
```

`_WellKnownOpenAITools` 是 **12 个字符串的元组**：`function`、`file_search`、`computer`、`computer_use_preview`、`code_interpreter`、`mcp`、`image_generation`、`web_search_preview`、`web_search`、`tool_search`、`apply_patch`、`namespace`。

注释解释了这张表的边界：「OpenAI 聊天模型或 responses API 支持的知名工具列表。**这些工具不预期被其他遵循 OpenAI 函数调用 API 的聊天模型 provider 支持**」。

所以「透传」的判据是「这个类型标识只有 OpenAI 懂」。一旦命中，整个 dict 原样返回——因为那些工具（如 `file_search`）的 schema 是 OpenAI 专有形状，任何通用化尝试都会破坏它。

第二个特例 `web_search_preview` 的 `startswith` 判断带注释：「截至 03.12.25 可能是 `web_search_preview` 或 `web_search_preview_2025_03_11`」。所以 OpenAI 会给工具类型追加日期后缀，透传判断必须按前缀匹配而不是精确匹配。

第三个分支是 `Tool` 子类且 `metadata["type"] == "custom_tool"`：产出一个 `{"type": "custom", "name", "description", ["format"]}`。注意它**不进** `convert_to_openai_function`——因为 custom tool 不是函数调用，它带的是自由格式输入与 `format`（通常是语法/文法描述）。

**`tool_example_to_messages` 造的是「四段式 few-shot」。** 实现六步：

1. `messages = [HumanMessage(content=input)]`.
2. 把每个 `tool_calls` 里的 Pydantic 实例转成 OpenAI 形状：`{"id": str(uuid.uuid4()), "type": "function", "function": {"name": tool_call.__class__.__name__, "arguments": tool_call.model_dump_json()}}`。
3. `messages.append(AIMessage(content="", additional_kwargs={"tool_calls": openai_tool_calls}))`。
4. 若 `tool_outputs` 长度不符则 `ValueError`，消息里同时给出两边数字。
5. `tool_outputs = tool_outputs or ["You have correctly called this tool."] * len(...)` ——默认占位文案。
6. 逐条 `ToolMessage(content=output, tool_call_id=tool_call_dict["id"])`；若 `ai_response` 有值再追加一条 `AIMessage`。

第 2 步那句注释值得记：「**函数名目前对应于 Pydantic 模型的名字。这在当前 API 里是隐式的，将来会改进**」。所以「模型类名 = 工具名」是一个隐式约定，而代码里唯一表达它的地方就是 `tool_call.__class__.__name__`。

第 3 步用 `additional_kwargs` 而不是 `AIMessage.tool_calls` 字段，是一个向后兼容的选择：这个函数早于 `tool_calls` 字段出现，所以沿用原始形状。

第 5 步的默认占位文案「你正确地调用了这个工具」值得细读。docstring 解释了这个设计：「`ToolMessage` 是必需的，因为**某些聊天模型对 agent 用途过度优化，而不是对抽取用途**」。所以这些占位消息的作用不是传达业务信息，而是**满足协议形状**——很多模型在看到「工具调用后面没有工具结果」时会行为异常（继续调工具、或拒绝输出）。补一条假的确认把对话结构补齐。

这也解释了 `tool_example_to_messages` 的典型用途：把「文本 → 结构化输出」这个抽取任务表达成「工具调用」任务，让模型用工具调用的形式产出结构化结果（这是 `with_structured_output` 的核心手法之一）。

第 6 步 `if ai_response:` 用的是真假判定而不是 `is not None`，所以空串不会追加消息。

**入站侧的第一层：`parse_tool_call` 的四条路径。** 实现是：

```python
if "function" not in raw_tool_call: return None
arguments = raw_tool_call["function"]["arguments"]
if partial:
    try: function_args = parse_partial_json(arguments, strict=strict)
    except (JSONDecodeError, TypeError): return None   # None args raise TypeError
elif not arguments:
    function_args = {}                                  # 无参数工具
else:
    try: function_args = json.loads(arguments, strict=strict)
    except JSONDecodeError as e: raise OutputParserException(...) from e
```

四条值得逐一说：

1. **缺 `function` 键就直接 `None`**（不报错）。所以「非函数调用的 tool_call」被静默跳过——对应 `convert_to_openai_tool` 里那些透传的内置工具（它们的原始形状里没有 `function`）。
2. **`partial=True` 时解析失败返回 `None`**，即「还没解析出来」，而不是错误。这是流式的必需妥协：JSON 在流到一半时天然不合法。
3. **`not arguments` → `{}`**，注释写「为无参数工具处理 `None` 或空字符串」。所以「模型没给参数」不会被当成解析失败。
4. **非 partial 时解析失败抛 `OutputParserException`**，且消息里包含函数名与原始参数串——足够定位问题。

`partial` 分支里那个 `TypeError` 捕获带注释：「`None` 参数抛 `TypeError`」。所以 `parse_partial_json(None)` 不返回错误而是抛 `TypeError`——这是被解析函数的行为细节，调用方用宽捕获把「不可解析」统一成 `None`。

末尾：

```python
parsed = {"name": raw_tool_call["function"]["name"] or "", "args": function_args or {}}
if return_id:
    parsed["id"] = raw_tool_call.get("id")
    parsed = create_tool_call(**parsed)
```

`name` 的 `or ""` 与 `args` 的 `or {}` 是对「键存在但值为 `None`」的兜底。而 `create_tool_call(**parsed)` 只在 `return_id` 为真时调用——所以「不带 id」时返回的是**普通 dict**，「带 id」时返回的是 `ToolCall` 类型（`TypedDict`）。这个差别会影响下游（`ToolCall` 有字段保证，dict 没有）。

**第二层：`parse_tool_calls` 是「收集所有错误再一次性抛出」。** 实现遍历每条、`try/except OutputParserException` 把 `str(e)` 收进 `exceptions` 并 `continue`，最后 `if exceptions: raise OutputParserException("\n\n".join(exceptions))`。

所以「三条都失败」给出一个含三条错误消息的异常，而不是只报第一条。这个选择对「有多个工具调用、其中几个格式坏了」的场景很实用——用户一次就能看到全部问题，不必改一个再跑一遍。

注意 `if parsed: final_tools.append(parsed)` ——`parse_tool_call` 返回 `None` 的两种情形（缺 `function`、partial 解析失败）都被**静默丢弃**，不算错误。所以「跳过」与「失败」是两件事。

**第三层：`JsonOutputToolsParser` 有两条数据来源。** `parse_result` 先取 `result[0]`，要求是 `ChatGeneration`（否则 `OutputParserException`），然后 `raise_if_refused(message)`——**拒答检查在解析之前**。所以模型说「我不能帮这个」时会抛 `ModelRefusalError`，而不是「找不到 tool_calls」。

然后分两条路径：

```python
if isinstance(message, AIMessage) and message.tool_calls:
    tool_calls = [dict(tc) for tc in message.tool_calls]
    for tool_call in tool_calls:
        if not self.return_id: tool_call.pop("id")
else:
    try: raw_tool_calls = copy.deepcopy(message.additional_kwargs["tool_calls"])
    except KeyError: return []
    tool_calls = parse_tool_calls(raw_tool_calls, partial=partial, strict=self.strict, return_id=self.return_id)
```

第一条路径：消息已有解析好的 `tool_calls`（新式字段），直接取，按需去掉 `id`。
第二条路径：从 `additional_kwargs["tool_calls"]`（旧式原始形状）里取，走 `parse_tool_calls` 重新解析。

所以这个解析器**同时兼容新旧两种表示**。`copy.deepcopy` 是因为要往下传（`parse_tool_calls` 内部可能改），避免污染消息对象。

`except KeyError: return []` ——没有 `tool_calls` 键就返回空列表（不是 `None`），因为 docstring 承诺「`first_tool_only=False` 时结果是列表或空列表」。

接着一句注释很重要：

```python
# for backwards compatibility
for tc in tool_calls:
    tc["type"] = tc.pop("name")
```

所以输出字典里**没有 `name` 键，只有 `type` 键**，值是工具名。这是历史形状：早期把这个字段叫 `type`（表示「哪种工具」），后来的 `ToolCall` 叫 `name`。所以这个解析器把 `name` 改名成 `type` 输出。

这个改写解释了 `PydanticToolsParser` 里那句 `name_dict[res["type"]]` ——它取的是 `type` 键，而那个值实际是工具名。

`first_tool_only` 的两条语义：真时 `tool_calls[0] if tool_calls else None`；假时返回列表（可能空）。注意两个分支的返回类型不同（`dict | None` vs `list`），这是 docstring 里明说的。

`parse(text)` 直接 `raise NotImplementedError` ——这个解析器只支持「整条 `Generation`」的输入，不能吃裸字符串。理由很实：裸字符串里没有 `tool_calls` 结构，无从解析。

**`PydanticToolsParser` 在字典上再加一层实例化，且有三处特殊处理。** `parse_result` 先调 `super().parse_result(...)` 拿 `json_results`，然后：

1. `if not json_results: return None if self.first_tool_only else []` ——保留 `first_tool_only` 的空值语义。
2. `json_results = [json_results] if self.first_tool_only else json_results` ——**把单个 dict 包成列表**，以便后面统一循环。
3. 建两张名字表并按 v1/v2 合并：

```python
name_dict_v2 = {tool.model_config.get("title") or tool.__name__: tool for tool in self.tools if issubclass(tool, BaseModel)}
name_dict_v1 = {tool.__name__: tool for tool in self.tools if issubclass(tool, BaseModelV1)}
name_dict = {**name_dict_v2, **name_dict_v1}
```

第 3 项值得细看：**v2 的键优先取 `model_config["title"]`，没有才退到 `__name__`**；v1 只能取 `__name__`。而合并时 `{**v2, **v1}` 意味着**同名键 v1 覆盖 v2**。

这解释得通：`title` 是显式声明的工具名（可能与类名不同），所以优先；v1 没有 `model_config` 机制，只能用类名。而 v1 覆盖 v2 是因为后写的字典赢——但这也意味着一个同名工具若同时有 v1 与 v2 版本，v1 版本会胜出。这是一处**隐式优先级**，文档没说，但代码里是确定的。

4. 循环里三道检查：

```python
if not isinstance(res["args"], dict):
    if partial: continue
    raise ValueError(f"Tool arguments must be specified as a dict, received: {res['args']}")
try: tool = name_dict[res["type"]]
except KeyError as e:
    available = ", ".join(name_dict.keys()) or "<no_tools>"
    raise OutputParserException(f"Unknown tool type: {res['type']!r}. Available tools: {available}") from e
try: pydantic_objects.append(tool(**res["args"]))
except (ValidationError, ValueError):
    if partial: continue
    has_max_tokens_stop_reason = any(generation.message.response_metadata.get("stop_reason") == "max_tokens" for generation in result if isinstance(generation, ChatGeneration))
    if has_max_tokens_stop_reason: logger.exception(_MAX_TOKENS_ERROR)
    raise
```

第一道：`args` 不是字典时，partial 下跳过、否则报错。partial 下跳过的理由与流式相关——参数还没流完时可能是字符串片段。

第二道：未知工具名报错时**列出所有可用名**（`", ".join(name_dict.keys()) or "<no_tools>"`）。空表时给 `<no_tools>` 而不是空字符串，让消息不残缺。这是「错误消息要可自救」的又一例。

第三道最细：实例化失败时，若 `partial` 就跳过（还没流完，字段不全很正常）；否则**先检查所有 generation 的 `stop_reason` 是否是 `max_tokens`**，是就打一条 `logger.exception(_MAX_TOKENS_ERROR)` 再 raise。

`_MAX_TOKENS_ERROR` 的文案（从文件末尾可见片段）：「输出很可能不完整——请增加 `max_tokens` 或缩短你的提示」。

所以这条检查是把两个同时发生的失败**关联起来**：模型输出被截断（`stop_reason == max_tokens`）时，JSON 自然不完整、Pydantic 校验自然失败。此时给出「可能是被截断」的提示，比让用户去读一条 Pydantic 校验错误有用得多。

用 `logger.exception` 而不是 `logger.error` 是因为在 `except` 块内——它会带上当前异常栈。所以日志里有原始的 `ValidationError`，而抛出的是同一个异常（`raise` 裸抛，保留原栈）。

## 关键取舍

**源码事实：** `convert_to_openai_function` 七分支分派，包含 Anthropic（`input_schema`）与 Bedrock Converse（`toolSpec`）两种外部格式。

**源码事实：** 已是 OpenAI 格式的 dict 走白名单过滤，只保留四个键。

**源码事实：** 输入是 dict 且 `strict` 为真时才 `deepcopy`，因为 `_recursive_set_additional_properties_false` 会就地改写。

**源码事实：** `strict` 冲突时报错并同时给出原值与新值；`strict=True` 才递归加 `additionalProperties: false`。

**源码事实：** `_WellKnownOpenAITools` 12 项透传，`web_search_preview` 按前缀匹配（因为有日期后缀变体）。

**源码事实：** `tool_example_to_messages` 默认给「你正确地调用了这个工具」的占位 `ToolMessage`，docstring 理由是「某些模型对 agent 用途过度优化」。

**源码事实：** `parse_tool_call` 缺 `function` 返 `None`、`partial` 解析失败返 `None`、空参数给 `{}`、非 partial 失败抛异常；`return_id` 决定返回 dict 还是 `ToolCall`。

**源码事实：** `parse_tool_calls` 收集所有错误后一次性抛出，`None` 结果被静默跳过而非算失败。

**源码事实：** `JsonOutputToolsParser` 同时支持 `message.tool_calls` 与 `additional_kwargs["tool_calls"]` 两条来源，输出把 `name` 改名成 `type`。

**源码事实：** `PydanticToolsParser` 的名字表 v2 优先取 `model_config["title"]`，合并时 v1 覆盖 v2。

**源码事实：** Pydantic 实例化失败时，若 `stop_reason == "max_tokens"` 会额外记录一条「输出可能被截断」的诊断。

**架构推论：** 「出站归一 + 入站归一」这对函数的存在，说明函数调用这件事的痛点不在调用，而在**格式的方言差异**。模型 provider 各有各的工具声明形状，而模型输出的 tool_call 又有新旧两种表示（`tool_calls` 字段 / `additional_kwargs` 里的原始 dict）。所以这一层的价值是「把方言收敛到一种中间形状」，让上层的 agent 逻辑只处理一种。

**架构推论：** `partial` 这个参数贯穿解析的每一层，且几乎每个失败点在 `partial=True` 时都变成「跳过」而不是「报错」。这不是宽松，而是**流式解析的本质**：JSON 在流到一半时必然不完整，此时任何严格校验都会失败。所以流式解析必须能区分「还没到」与「坏了」——而大多数情况下它无法区分，只能选择「等」。

**架构推论：** `_MAX_TOKENS_ERROR` 那条关联是本页最有工程价值的一处。它把「Pydantic 校验失败」与「模型输出被截断」两个独立现象连起来，并给出可操作的补救（加 `max_tokens`）。这类「把下游症状反向映射到上游原因」的诊断，成本极低（一次 `response_metadata` 检查），但能把一次排查从半小时变成十秒。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/utils/function_calling.py` | `convert_to_openai_function` | 七分支类型分派，产出函数体 schema |
| `libs/core/langchain_core/utils/function_calling.py` | `convert_to_openai_tool` | 产出工具 schema，内置工具透传 |
| `libs/core/langchain_core/utils/function_calling.py` | `tool_example_to_messages` | 把示例拼成 few-shot 消息序列 |
| `libs/core/langchain_core/utils/function_calling.py` | `_WellKnownOpenAITools` | 12 个 OpenAI 专有工具类型名 |
| `libs/core/langchain_core/utils/function_calling.py` | `_convert_pydantic_to_openai_function` | Pydantic 模型转 schema |
| `libs/core/langchain_core/utils/function_calling.py` | `_convert_typed_dict_to_openai_function` | TypedDict 转 schema |
| `libs/core/langchain_core/utils/function_calling.py` | `_convert_python_function_to_openai_function` | 从函数签名与文档字符串生成 schema |
| `libs/core/langchain_core/utils/function_calling.py` | `_recursive_set_additional_properties_false` | 严格模式要求的递归改写 |
| `libs/core/langchain_core/output_parsers/openai_tools.py` | `parse_tool_call` | 单条解析，四条路径与 partial 妥协 |
| `libs/core/langchain_core/output_parsers/openai_tools.py` | `parse_tool_calls` | 批量解析，收集全部错误后一次性抛出 |
| `libs/core/langchain_core/output_parsers/openai_tools.py` | `make_invalid_tool_call` | 无效调用保留原始参数与错误文案 |
| `libs/core/langchain_core/output_parsers/openai_tools.py` | `JsonOutputToolsParser` | 双来源取数，输出把 `name` 改名为 `type` |
| `libs/core/langchain_core/output_parsers/openai_tools.py` | `JsonOutputKeyToolsParser` | 按 `key_name` 取特定工具的结果 |
| `libs/core/langchain_core/output_parsers/openai_tools.py` | `PydanticToolsParser` | 名字表映射与实例化，含截断诊断 |

## 参考

- [langchain 源码：utils/function_calling.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/utils/function_calling.py)
- [langchain 源码：output_parsers/openai_tools.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/output_parsers/openai_tools.py)
- [OpenAI 文档：函数调用](https://platform.openai.com/docs/guides/function-calling)

## 与主核 / AI 的关系

主核里最接近的是「协议适配层」：把不同客户端的不同请求格式归一成内部统一的执行计划，再把结果归一成各方能读的响应格式。这一页的两个转换函数就是那个适配层的对应物，连「内置/专有格式透传而不改写」这条规矩都一样——主核也不会试图把某个后端的专有数据类型翻译成通用类型。

差别在于**失败的可容忍度**。主核的协议解析对格式错误一律拒绝（坏报文就是坏报文）；这里的解析在 `partial=True` 时把几乎一切失败都变成「跳过」，因为流式场景下「不完整」是常态。这是一条重要分野：**能否容忍不完整输入，取决于这个输入是不是正在生成中**。

与 h01（聊天模型契约）的连接：`convert_to_openai_tool` 的产物正是 `bind_tools` 要喂给 provider 的东西；而 `_llm_type` 那一类「模型标识」在追踪里的作用，与这里的「工具名映射」是同一类问题——都需要一个跨 provider 的稳定标识。

与 h02（流式投影）的连接更直接：`parse_tool_calls(partial=True)` 是流式工具调用的基础——`_push_tool_call_chunk` 那条路径与这里的 partial 解析处理的是同一个现实（工具调用的 JSON 参数是逐段到达的）。h02 累积片段、这页解析片段，两页合起来才构成「流式工具调用」。

与 g05（压缩与嵌入）的分工：那边是「文本到向量」的形状转换；这里是「调用声明到 schema」的形状转换。两者都属「格式协商」，但后者的失败模式更复杂，因为要处理模型输出的不合法。

对 AI 工程最实用的一条是「错误消息要能自救」。这一页有三处做得很好：缺 `title` 时指出「要用作函数名所以需要它」；未知工具名时列出所有可用名；`tool_outputs` 数量不符时同时给出两个数字。这类消息把「你错了」变成「你该怎么改」——成本只是几次字符串拼接，收益是使用者不必去读源码。

另一条是 `_MAX_TOKENS_ERROR` 那条诊断。任何「下游校验失败」的地方都值得问一句：上游有没有一个信号能解释这次失败？这里 `response_metadata["stop_reason"] == "max_tokens"` 就是那个信号。把两个现象关联起来并在日志里点明，比让用户自己发现「哦原来是截断了」高效得多。

最后一条是「v1 与 v2 的名字表合并顺序」。`{**name_dict_v2, **name_dict_v1}` 里 v1 覆盖 v2 是一个隐式决定，而它会影响「同名工具有两个版本时用哪个」。任何靠字典合并实现「多来源注册」的地方，都要显式想清楚同名冲突时谁赢，并把它写进注释或文档——否则这个优先级会变成一个没人知道的实现细节，而它恰恰决定了运行时行为。
