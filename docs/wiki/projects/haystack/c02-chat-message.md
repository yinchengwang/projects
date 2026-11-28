---
project: haystack
feature: chat-message
title: 聊天消息与内容块
page_type: feature
schema_version: 1
refs:
  - path: haystack/dataclasses/chat_message.py
    symbols: [ChatMessage, ChatRole, TextContent, ToolCall]
  - path: haystack/dataclasses/chat_message.py
    symbols: [_haystack_content_part_to_openai_dict, _openai_dict_to_haystack_content_part, _parse_openai_base64_data_url]
---

# 聊天消息与内容块

## 一句话职责

`ChatMessage` 用「角色 + 有序内容块列表」表达一条消息，六种块类型覆盖文本、工具调用、工具结果、图像、文件与推理内容；同时提供到 OpenAI Chat Completions 字典的**双向转换**，而这组转换是本页真正的主体——它是多模态内容在两种表示之间往来的唯一通道。

## 机制是什么

一条聊天消息有多种建模方式，而 haystack 选了最扁平的一种：

```python
@dataclass
class ChatMessage:
    _role: ChatRole
    _content: Sequence[ChatMessageContentT]
    _name: str | None = None
    _meta: dict[str, Any] = Field(default_factory=dict, hash=False)
```

四个字段，其中 `_content` 是**六种类型的联合**：

```python
ChatMessageContentT = TextContent | ToolCall | ToolCallResult | ImageContent | ReasoningContent | FileContent
```

这个扁平结构带来两个后果：

1. **块之间有顺序**。所以「先说话再调工具」与「先调工具再说话」是两条不同的消息——而 OpenAI 的表示里 `content` 与 `tool_calls` 是**两个独立字段**，顺序信息因此丢失。
2. **块之间没有关联约束**。一个 `TOOL` 角色的消息里可以放 `TextContent`（虽然语义上不该），框架不阻止。

而 `ChatRole` 有四个值，每个值隐式规定了「允许哪些块」：

| 角色 | 允许的块 | 强制在哪 |
|---|---|---|
| `SYSTEM` | 只有 `TextContent` | `from_system` 只接受 `text` |
| `USER` | `TextContent` / `ImageContent` / `FileContent` | `from_user` 的 `content_parts` 类型检查 |
| `ASSISTANT` | `TextContent` / `ToolCall` / `ReasoningContent` | `from_assistant` 的参数 |
| `TOOL` | `ToolCallResult` | `from_tool` 只构造这一种 |

所以「角色约束块类型」这条规则不是由类型系统表达的，而是**由四个工厂方法分别实现**的。因此绕过工厂方法直接构造（`ChatMessage(_role=..., _content=[...])`）不受约束——而 `to_openai_dict_format` 里有几处显式检查来兜住这种情形（见下）。

## 怎么实现的

**`ChatMessage` 的字段全带下划线，而访问靠 12 个只读属性。**

```python
_role: ChatRole
_content: Sequence[ChatMessageContentT]
_name: str | None = None
_meta: dict[str, Any] = field(default_factory=dict, hash=False)
```

配合的模式是「列表访问器 + 单值访问器」成对出现：

| 列表 | 单值 | 语义 |
|---|---|---|
| `texts` | `text` | 第一段文本或 `None` |
| `tool_calls` | `tool_call` | 第一个工具调用或 `None` |
| `tool_call_results` | `tool_call_result` | 同上 |
| `images` | `image` | 同上 |
| `files` | `file` | 同上 |
| `reasonings` | `reasoning` | 同上 |

六个列表访问器都是同一个形状：

```python
@property
def tool_calls(self) -> list[ToolCall]:
    return [content for content in self._content if isinstance(content, ToolCall)]
```

```python
@property
def tool_call(self) -> ToolCall | None:
    if tool_calls := self.tool_calls:
        return tool_calls[0]
    return None
```

三处：

1. **`isinstance` 过滤**——因为 `_content` 是联合类型的序列，取特定类型只能靠 `isinstance`。所以每次访问都是 **O(n) 遍历**，而 `text` 访问器内部又调 `texts`（再遍历一次）。

2. **单值访问器是「第一个或 `None`」**，不检查「是否恰好一个」。所以一条消息有两个 `ToolCall` 时 `tool_call` 静默返回第一个——这与 d01（llama-index）里 `MultiSelection.ind` 在非单值时**抛错**的处理相反。两个设计各有道理：这里把「多值」当正常状态（一条助手消息可以有多个工具调用），所以单值访问器只是便捷读法。

3. **`_meta` 的 `hash=False`**——参与 `__eq__` 的字段比较但不参与哈希（若按 dataclass 默认会因 dict 不可哈希而报错）。但注意类被 `_warn_on_inplace_mutation` 装饰，而 `ChatMessage` 没有 `frozen=True`，所以它**可变且不可哈希**。

**`_serialize_content_part` 的序列化键是一个类到字符串的映射。**

```python
_CONTENT_PART_CLASSES_TO_SERIALIZATION_KEYS: dict[type[ChatMessageContentT], str] = {
    TextContent: "text",
    ToolCall: "tool_call",
    ToolCallResult: "tool_call_result",
    ImageContent: "image",
    ReasoningContent: "reasoning",
    FileContent: "file",
}

def _serialize_content_part(part: ChatMessageContentT, *, for_trace: bool = False) -> dict[str, Any]:
    serialization_key = _CONTENT_PART_CLASSES_TO_SERIALIZATION_KEYS.get(type(part))
    if serialization_key is None:
        raise TypeError(f"Unsupported type in ChatMessage content: `{type(part).__name__}` for `{part}`.")
    data = part._to_trace_dict() if for_trace and hasattr(part, "_to_trace_dict") else part.to_dict()
    return data if isinstance(part, TextContent) else {serialization_key: data}
```

四处：

1. **用 `type(part)` 精确查表**，不是 `isinstance`。所以子类（若有）查不到 → 抛 `TypeError`。这个严格性是必要的：反序列化要按键还原出确切的类，所以「键 ↔ 类」必须是双射。

2. **`TextContent` 是唯一不嵌套的块**：序列化成 `{"text": "..."}` 而不是 `{"text": {"text": "..."}}`。注释说明了这一点：「Text is serialized as `{"text": "..."}`; other parts are nested」。这个特例让最常用的块保持最简形式——代价是 `_deserialize_content_part` 里必须**先**处理扁平形态。

3. **`for_trace` 时优先用 `_to_trace_dict`**（存在则用），所以大载荷会在追踪时被替换。但 `TextContent` 没有 `_to_trace_dict`（文本不大），所以走 `to_dict`。

4. **`hasattr(part, "_to_trace_dict")`** 而不是列出类型——所以将来新增块类型只要实现这个方法就自动获得追踪支持。这是一处好的设计：用「有没有这个方法」代替「是不是这几个类」。

**反序列化必须先处理扁平文本，然后按六种键找，最后兜底尝试扁平结构。**

```python
def _deserialize_content_part(serialized_part: dict[str, Any]) -> ChatMessageContentT:
    if "text" in serialized_part:
        return TextContent.from_dict(serialized_part)

    for cls, serialization_key in _CONTENT_PART_CLASSES_TO_SERIALIZATION_KEYS.items():
        if serialization_key in serialized_part:
            return cls.from_dict(serialized_part[serialization_key])

    # Support for Pydantic's model_dump() output, which produces a flat dictionary without wrapping keys.
    if "tool_name" in serialized_part and "arguments" in serialized_part:
        return ToolCall.from_dict(serialized_part)
    if "result" in serialized_part and "origin" in serialized_part:
        return ToolCallResult.from_dict(serialized_part)
    if "reasoning_text" in serialized_part:
        return ReasoningContent.from_dict(serialized_part)
    if "base64_image" in serialized_part:
        return ImageContent.from_dict(serialized_part)
    if "base64_data" in serialized_part:
        return FileContent.from_dict(serialized_part)
    ...
```

四处：

1. **`"text" in serialized_part` 必须在最前**，因为扁平文本形态与嵌套形态不冲突（其他块的外层键是 `image`/`file` 等）。而**顺序仍然重要**：一个嵌套的 `{"text": {"text": "..."}}` 会被误判成 `TextContent.from_dict({"text": {...}})`——传进去的 `text` 是个字典，构造出一个 `text` 为字典的 `TextContent`。这是一个真实的形状错误通过点。

2. **按字典顺序遍历**六种键。所以若一个字典同时含 `text` 与 `image`（不该发生），会走 `TextContent`。

3. **Pydantic `model_dump()` 的扁平形态**是第二套兼容路径。注释解释了来源：「Pydantic's `model_dump()` output, which produces a flat dictionary without wrapping keys」。

   **五个分支各自用「特征字段组合」判断**：
   - `tool_name` + `arguments` → `ToolCall`
   - `result` + `origin` → `ToolCallResult`
   - `reasoning_text` → `ReasoningContent`
   - `base64_image` → `ImageContent`
   - `base64_data` → `FileContent`

   注意 `base64_data` 与 `base64_image` 的区分是必要的（前者是 `FileContent`，后者是 `ImageContent`），而 `"base64_data" in ...` 不会误命中 `base64_image`（子串不同）。

4. **错误消息异常详细**，注释说明了原因：「this verbose error message provides guidance to LLMs when creating invalid messages during agent runs」。消息里列出了六种键与每个键对应的完整示例形状。这是一处**把错误消息当成文档**的做法——因为读者可能是 LLM。

**`ChatMessage.from_dict` 有两代兼容路径。**

```python
# Pydantic dataclass dumps and pre-2.12 messages use underscore-prefixed fields.
if "content" not in data and "_content" in data:
    fields = ("role", "content", "name", "meta")
    data = {key: data[f"_{key}"] for key in fields if f"_{key}" in data}
```

三处：

1. **下划线字段的重映射**：`_content` → `content`、`_role` → `role` 等。所以「Pydantic dataclass dump」与「2.12 之前的消息」两种形态都能读。

2. **`data = {...}` 重建整个字典**，所以原有的其他键被丢弃。这在这里没问题（只有四个字段）。

3. **条件 `"content" not in data and "_content" in data`** —— 只在缺失新字段时才重映射。所以两种都有时优先新的。

而内容的三态处理：

```python
serialized_content = data["content"]
if isinstance(serialized_content, str):
    content = [TextContent(text=serialized_content)]  # pre-2.9 format
elif isinstance(serialized_content, Sequence):
    content = [_deserialize_content_part(p) for p in serialized_content]
else:
    raise TypeError(f"Unsupported content type in serialized ChatMessage: `{serialized_content}`")
```

**`str` 是 2.9 之前的格式**（那时 `content` 就是一段文本）。所以这是第三代兼容。

**`to_openai_dict_format` 开头有四道前置校验，每道对应一条 OpenAI 的约束。**

```python
has_content = bool(self.texts or self.tool_calls or self.tool_call_results or self.images or self.files)
if not has_content and not self.is_from(ChatRole.ASSISTANT):
    raise ValueError(f"A `ChatMessage` from `{self._role.value}` must contain at least one ... Only assistant messages can be empty.")
if len(self.tool_call_results) > 0 and len(self._content) > 1:
    raise ValueError("For OpenAI compatibility, a `ChatMessage` with a `ToolCallResult` cannot contain any other content.")
if self.tool_calls and not self.is_from(ChatRole.ASSISTANT):
    raise ValueError("Tool calls are only supported in assistant messages.")
```

四处：

1. **`has_content` 用五个列表的或**，注意**`reasonings` 不在其中**——所以一条只有 `ReasoningContent` 的消息被判为「无内容」。而 `ReasoningContent` 确实无法送到 OpenAI（见下），所以这个判断是对的。

2. **只有 assistant 可以是空的**。理由是「We convert an assistant message with no content part into a message with empty content, which the API accepts」——而其他角色空的会被 API 拒。

3. **`tool_call_results` 必须独占**（`len(self._content) > 1` 就拒）。注释直说「For OpenAI compatibility」——因为 tool 消息在 OpenAI 的表示里只有 `content` 与 `tool_call_id`，没有放其他内容的位置。

4. **`tool_calls` 必须在 assistant 里**。

这三道校验都是「OpenAI 的约束泄漏到通用模型里」。而它们出现在 `to_openai_dict_format` 而不是构造器里，位置是对的——通用的 `ChatMessage` 不限制这些，只有转成 OpenAI 格式时才有这些约束。

**分派按「角色 + 有无 tool_call_results」四路。**

```python
if self.is_from(ChatRole.USER):
    openai_msg.update(self._build_openai_user_message_fields())
elif self.tool_call_results:
    openai_msg.update(self._build_openai_tool_result_message_fields(require_tool_call_ids=require_tool_call_ids))
elif self.is_from(ChatRole.ASSISTANT):
    openai_msg.update(self._build_openai_assistant_message_fields(require_tool_call_ids=require_tool_call_ids))
else:
    openai_msg["content"] = self.text or ""
```

注意**第二支判的是「有无 tool_call_results」而不是角色**。所以一条 `USER` 角色的消息若含 `ToolCallResult`，会走第二支而不是第一支——它被当成 tool 消息。这与「角色决定内容」的设计有张力，但实际不会发生（`from_tool` 造的是 `TOOL` 角色）。

第四支（`SYSTEM` 与其他）只有一行：`{"content": self.text or ""}`——**丢掉了所有非文本块**。

**`_build_openai_user_message_fields` 有一个「单块简化」的分支。**

```python
def _build_openai_user_message_fields(self) -> dict[str, Any]:
    if len(self._content) == 1 and isinstance(self._content[0], TextContent):
        return {"content": self.text}
    return {
        "content": [
            _haystack_content_part_to_openai_dict(haystack_part)
            for haystack_part in self._content
            if isinstance(haystack_part, (TextContent, ImageContent, FileContent))
        ]
    }
```

两处：

1. **只有一个 `TextContent` 时用字符串形式**（`"content": "hello"`），否则用内容块数组。这个简化是有意义的：OpenAI 对纯文本消息接受字符串形式，而那是更常见也更省 token 的形状。

2. **多块时过滤掉不支持的块**（`ReasoningContent`/`ToolCall`/`ToolCallResult` 在 `isinstance` 里不通过）。所以**静默丢弃**——一条含推理内容的 user 消息，推理内容会消失且无警告。

**`_haystack_content_part_to_openai_dict` 是三模态的转换，而三处默认值各有理由。**

```python
if isinstance(haystack_part, TextContent):
    return {"type": "text", "text": haystack_part.text}
if isinstance(haystack_part, ImageContent):
    image_url = {"url": f"data:{haystack_part.mime_type or 'image/jpeg'};base64,{haystack_part.base64_image}"}
    if haystack_part.detail:
        image_url["detail"] = haystack_part.detail
    return {"type": "image_url", "image_url": image_url}
return {
    "type": "file",
    "file": {
        "file_data": f"data:{haystack_part.mime_type or 'application/pdf'};base64,{haystack_part.base64_data}",
        "filename": haystack_part.filename or "filename",
    },
}
```

四处：

1. **MIME 缺失时兜底**：图像用 `image/jpeg`，文件用 `application/pdf`。注释对图像给了理由：「OpenAI API appears to tolerate MIME type mismatches」。

   注意这与 c01 里 `ImageContent` 的校验有关：`ImageContent` 允许 `mime_type=None`（前面那个 `if self.mime_type and ...` 的漏洞），所以这里必须给兜底。**兜底的存在本身就是那个漏洞的症状**——一个可信的白名单检查会让 `None` 不可能到达这里。

2. **`detail` 只在非空时加**。所以 `detail=None` 时 OpenAI 收到的是不带这个键，而它的默认行为是 `auto`。这与显式传 `None` 不同（后者 OpenAI 可能拒绝）。

3. **`filename` 兜底成字面量 `"filename"`**。注释解释了：「Filename is optional but if not provided, OpenAI expects a `file_id` of a previous file upload. We use a dummy filename」。所以这个假名字是为了**避免走 `file_id` 分支**——一个纯粹的协议适配 hack。

4. **最后 `return` 而非 `if isinstance(haystack_part, FileContent)`**。所以这个函数的类型契约靠调用方的 `isinstance` 过滤保证——传一个 `ToolCall` 进来会造出一个 `"type": "file"` 的畸形块（然后 `haystack_part.base64_data` 会 `AttributeError`）。这是一处依赖调用方正确的写法。

**`from_openai_dict_format` 是反方向，而它的角色处理与正方向不对称。**

正方向：四个角色（`USER`/`SYSTEM`/`ASSISTANT`/`TOOL`）。反方向：**五个**——多了 `developer`：

```python
if role not in ["assistant", "user", "system", "developer", "tool"]:
    raise ValueError(f"Unsupported role: {role}")
```

而 `developer` 落到最后的 `from_system`（与 `system` 同路）。所以**往返一次后 `developer` 变成 `system`**——角色信息丢失。这是一个真实的有损点：OpenAI 新增的 `developer` 角色在 haystack 的 `ChatRole` 里没有对应值。

反方向的 assistant 分支是本页最复杂的一段：

```python
if role == "assistant":
    refusal = message.get("refusal")
    openai_tool_calls = message.get("tool_calls")
    if refusal is not None and not isinstance(refusal, str):
        raise ValueError(...)
    # An empty string is valid content for an assistant message
    if (openai_content is None or openai_content == []) and not openai_tool_calls and not refusal:
        raise ValueError("For assistant messages, either `content`, `tool_calls`, or `refusal` must be present.")
    parsed_content = _parse_openai_text_and_refusal_content(openai_content, role=role)
    refusals = [refusal] if refusal else []
    if "refusal" in parsed_content:
        refusals.append(parsed_content["refusal"])
    meta = {"refusal": "\n".join(dict.fromkeys(refusals))} if refusals else None
    return cls.from_assistant(
        text=parsed_content.get("text"), name=name, meta=meta,
        tool_calls=[_openai_dict_to_haystack_tool_call(tc) for tc in openai_tool_calls or []],
    )
```

四处：

1. **空字符串内容合法** —— 注释：「An empty string is valid content for an assistant message: that is how a reply with nothing to send is serialized」。所以判据是 `is None or == []`，**不含 `""`**。这与正方向「assistant 可以为空」对应。

2. **`dict.fromkeys(refusals)` 去重**。注释：「Avoid duplicating a refusal present in both the field and the content parts」。这是一个简洁的去重手法（`dict.fromkeys` 保序去重）。

3. **两个来源的 refusal 都可能存在**：顶层 `refusal` 字段与 `content` 里的 `{"type": "refusal", ...}` 块。它们被合并成一个以 `\n` 连接、存在 `meta["refusal"]` 的字符串。

   所以 **refusal 在 haystack 侧不是一个独立的块类型，而是 `meta` 里的一个键**。这与正方向对应：`to_openai_dict_format` 从 `self._meta.get("refusal")` 读它。

4. **`meta=meta` 传 `None` 时不带 refusal**，而 `from_assistant` 会把 `meta or {}` 归一成 `{}`。

**正方向的 refusal 处理有三条分支，而它揭示了「content 数组只能装一种东西」。**

```python
refusal = self._meta.get("refusal")
if isinstance(refusal, str) and refusal:
    if tool_calls:
        logger.warning("Dropping the refusal of an assistant message that also has tool calls, because OpenAI ignores "
                       "the tool calls of a message with a refusal and then rejects the tool results that follow.")
    elif text is not None:
        openai_fields["refusal"] = refusal
    else:
        openai_fields["content"] = [{"type": "refusal", "refusal": refusal}]
```

四处：

1. **有 tool_calls 时丢弃 refusal 并 warning**。理由写在消息里：「OpenAI ignores the tool calls of a message with a refusal and then rejects the tool results that follow」。即：不是「OpenAI 拒绝这个请求」，而是「它会静默忽略 tool_calls，然后下一步的 tool 结果校验失败」。这是一个**跨轮次的失败**，所以提前丢弃比让它在下一轮炸掉更好。

2. **有 text 时放顶层 `refusal` 字段**。注释：「OpenAI's content array holds either text parts or exactly one refusal part, so a refusal next to text goes into the `refusal` field」。所以**`content` 数组不能同时装文本与 refusal**——这是协议约束，而 haystack 用「顶层字段」绕过。

3. **无 text 时 refusal 当内容块**（`{"type": "refusal", "refusal": ...}`）。

4. **`isinstance(refusal, str) and refusal`** —— 非字符串或空串都跳过。所以 `meta["refusal"] = ""` 与不存在等价。

最后还有一句兜底：

```python
if "content" not in openai_fields and "tool_calls" not in openai_fields:
    openai_fields["content"] = ""
```

注释：「The API rejects a message carrying neither content nor tool calls, so send empty content for it」。所以一条「只有 refusal 且被丢弃」的 assistant 消息会得到 `{"content": ""}`——**空消息而不是错误**。

**`_parse_openai_base64_data_url` 是一个极简的 data URL 解析器。**

```python
def _parse_openai_base64_data_url(data_url: Any) -> tuple[str | None, str]:
    if not isinstance(data_url, str) or not data_url.startswith("data:") or ";base64," not in data_url:
        raise ValueError(
            f"Unsupported URL: {_CONTENT_PART_REPR.repr(data_url)}. Only base64 data URLs in the format "
            "`data:<mime_type>;base64,<base64_data>` are supported."
        )
    header, base64_data = data_url.split(";base64,", 1)
    return header[len("data:"):] or None, base64_data
```

五处：

1. **三道前置检查合在一个 `if`**：是字符串、以 `data:` 开头、含 `;base64,`。**不检查 URL 是否 http**——所以 http 图片 URL 会在这里被拒（错误消息说的是「Only base64 data URLs」）。

   这意味着**从 OpenAI 格式读回来的图像必须是内联 base64**。OpenAI 本身也接受 http URL 形式的 `image_url`，而 haystack 不支持——因为它要在本地持有字节，而一个 URL 引用的内容随时可能失效。

2. **`split(";base64,", 1)`** — 用 `maxsplit=1`，所以 base64 数据里若出现 `;base64,` 不会被误切（base64 字母表里没有 `;` 和 `,`，所以实际不会）。

3. **`header[len("data:"):] or None`** —— 剥离 `data:` 前缀，且**空 MIME → `None`**。所以 `data:;base64,xxx`（无 MIME）得到 `(None, "xxx")`。

4. **不校验 base64 是否合法**——解出来的字符串直接交给 `ImageContent`/`FileContent` 的构造器，由那里的 `b64decode(validate=True)` 校验（c01）。所以校验责任在数据类，不在解析器。

5. **不解析 data URL 的其他参数**（如 `;charset=utf-8;base64,`）。因为 `";base64," in data_url` 通过，而 `split(";base64,", 1)` 得到的 `header` 是 `"data:image/png;charset=utf-8"`——于是 MIME 变成 `"image/png;charset=utf-8"`，而那不在 `IMAGE_MIME_TYPES` 里 → `ImageContent` 报错。所以**带额外参数的 data URL 会被拒**，尽管格式上合法。

**`_parse_openai_tool_call_arguments` 的三态处理。**

```python
def _parse_openai_tool_call_arguments(raw_arguments: Any) -> dict[str, Any]:
    if isinstance(raw_arguments, dict):
        return raw_arguments
    return json.loads(raw_arguments) if raw_arguments else {}
```

两处：

1. **已经是 dict 就原样返回**——因为某些 OpenAI 兼容 API 直接返回对象而不返回 JSON 字符串。
2. **`raw_arguments` 为假值（`None`/`""`）时返回 `{}`**。所以「没有参数」被归一成空字典。

**错误信息里不包 `json.loads` 的 `JSONDecodeError`** —— 文档只说「raises `json.JSONDecodeError`」。所以一个畸形的 `arguments` 字符串会让 `JSONDecodeError` 冒到调用方，而调用方（`from_openai_dict_format`）没有捕获它。**这是一个真实的崩溃点**：模型输出的 `arguments` 常常是不完整的 JSON（流式截断、长度上限）。

**`_parse_openai_text_and_refusal_content` 把「按类型分组 + 换行连接」。**

```python
allowed_types = ("text", "refusal") if role == "assistant" else ("text",)
texts_by_type: dict[str, list[str]] = {}
for openai_part in openai_content or []:
    if (not isinstance(openai_part, dict)
        or openai_part.get("type") not in allowed_types
        or not isinstance(openai_part.get(openai_part["type"]), str)):
        ...
        raise ValueError(...)
    texts_by_type.setdefault(openai_part["type"], []).append(openai_part[openai_part["type"]])
return {part_type: "\n".join(part_texts) for part_type, part_texts in texts_by_type.items()}
```

四处：

1. **`openai_part.get(openai_part["type"])`** —— 用 `type` 的值当字段名。所以 `{"type": "text", "text": "..."}` 检查 `part["text"]` 是字符串。这个同名字段约定让代码省了一行映射表。

2. **多段文本用 `\n` 连接成一段**。注释解释了理由：「Text parts are joined into a single text because `to_openai_dict_format` only sends the first text of these messages」。即正方向只发 `self.text`（第一段），所以反方向把多段合成一段以保持往返一致——但**这是有损的**：三段文本往返一次变成一段。

3. **`refusal` 与 `text` 分组后分别连接**，所以 refusal 永远不会混进 text。注释直说了：「so a refusal never ends up in the text」。

4. **`role` 只用来决定 `allowed_types`**。所以一个 `system` 消息里带 `refusal` 块会报错——这与正方向「只有 assistant 有 refusal」一致。

## 关键取舍

**源码事实：** `ChatMessage` 是四个字段的扁平 dataclass（`_role`/`_content`/`_name`/`_meta`），`_content` 是六种块的联合，具有顺序；「角色约束块类型」由四个工厂方法分别实现，绕过工厂直接构造不受约束。

**源码事实：** 六个 `isinstance` 过滤的列表访问器 + 六个「第一个或 None」的单值访问器成对存在；单值访问器**不检查是否恰好一个**（与 llama-index 的 `MultiSelection.ind` 相反）。

**源码事实：** `_serialize_content_part` 用 `type(part)` 精确查表（子类会被拒）；`TextContent` 是唯一不嵌套的块（`{"text": "..."}` 而非 `{"text": {...}}`）；`for_trace` 时用 `hasattr(part, "_to_trace_dict")` 判断而非列类型。

**源码事实：** `_deserialize_content_part` 先判 `"text" in serialized_part`（否则嵌套的 `{"text": {...}}` 会被误当扁平文本），再按六种键遍历，最后有五条「Pydantic `model_dump()` 扁平形态」的兜底分支（靠特征字段组合识别）；错误消息列出了六种键与完整示例形状，注释说明这是为了引导 LLM。

**源码事实：** `ChatMessage.from_dict` 有两代兼容：下划线字段重映射（Pydantic dump / pre-2.12）、`content` 为 `str`（pre-2.9）。

**源码事实：** `to_openai_dict_format` 前置三道校验（非 assistant 不可空、`tool_call_results` 必须独占、`tool_calls` 必须在 assistant），都是一条条 OpenAI 约束的显式化；`has_content` 的五个列表里**不含 `reasonings`**。

**源码事实：** 分派四支，第二支按「有无 `tool_call_results`」而非角色判断；`SYSTEM` 与其他角色的分支只有一行 `{"content": self.text or ""}`，**丢弃所有非文本块**。

**源码事实：** `_build_openai_user_message_fields` 在「单块且是 `TextContent`」时用字符串形式，多块时过滤掉 `ToolCall`/`ToolCallResult`/`ReasoningContent`（静默丢弃）。

**源码事实：** `_haystack_content_part_to_openai_dict` 对图像与文件的 MIME 各有兜底（`image/jpeg` / `application/pdf`），`filename` 兜底成字面量 `"filename"`（注释：避免 OpenAI 走 `file_id` 分支），`detail` 只在非空时加；最后一支用无条件 `return` 处理文件，依赖调用方的 `isinstance` 过滤。

**源码事实：** `from_openai_dict_format` 接受五个角色（多一个 `developer`），而 `developer` 落到 `from_system`——**往返一次 `developer` 变成 `system`**。

**源码事实：** assistant 分支里 `(openai_content is None or openai_content == [])` 判空（**`""` 合法**）；refusal 的两个来源（顶层字段 + content 块）用 `dict.fromkeys` 去重后以 `\n` 连接存进 `meta["refusal"]`——所以 refusal 在 haystack 侧**不是块类型而是 meta 键**。

**源码事实：** 正方向的 refusal 三支：有 `tool_calls` 时**丢弃并 warning**（理由：OpenAI 会忽略带 refusal 消息的 tool_calls，然后拒绝后续 tool 结果——是跨轮次失败）；有 `text` 时放顶层 `refusal` 字段（因为 content 数组不能同时装文本与 refusal）；否则当内容块。最后若两者都没有则补 `{"content": ""}`。

**源码事实：** `_parse_openai_base64_data_url` 只接受 base64 data URL（**拒绝 http URL**），不校验 base64 合法性（交给 `ImageContent`/`FileContent` 的 `b64decode(validate=True)`），空 MIME 归一成 `None`；带额外参数（如 `;charset=`）的 data URL 会被拒（因为 MIME 会带上参数而不在 `IMAGE_MIME_TYPES` 里）。

**源码事实：** `_parse_openai_tool_call_arguments` 三态（dict 原样 / 假值给 `{}` / 其余 `json.loads`），且 `JSONDecodeError` 未被捕获——畸形 `arguments` 会直接冒到调用方。

**源码事实：** `_parse_openai_text_and_refusal_content` 用 `openai_part.get(openai_part["type"])` 借同名字段约定省掉映射表；多段文本以 `\n` 连接成一段（因为正方向只发第一段），refusal 分组合并所以不会混进 text。

**架构推论：** 「角色 + 有序块列表」这个扁平模型的最大优势是**表达力不受协议限制**：一条 assistant 消息可以同时有推理、文本、三个工具调用，而 OpenAI 的表示里只有 `content` 与 `tool_calls` 两个槽。所以这个模型能承载「比任何单个供应商 API 更丰富」的语义——代价是所有转换都是有损的，且损失的方向不可逆（推理内容在 OpenAI 格式里根本没有位置）。

**架构推论：** 双向转换里有损点集中在三处：**块顺序**（正方向拆进 `content`/`tool_calls` 两个字段）、**多段文本**（反方向合成一段）、**`developer` 角色**（反方向降级成 `system`）。这三处都是协议本身无法表达的东西，而不是实现疏漏。判断标准应当是「这个信息在目标表示里有没有位置」——没有位置的，丢弃是对的；有位置但丢了的，才是 bug。

**架构推论：** refusal 被建模成 `meta["refusal"]` 而不是一个块类型，这是个有意思的取舍。它的好处是不必为一个只出现在 assistant 消息里的字段新增类型；坏处是它**绕过了 content 的顺序与类型检查**——`meta` 是自由的字典，所以「refusal 必须是字符串」这条约束只能在使用处（`isinstance(refusal, str) and refusal`）检查。这与 c01 里「meta 承载一切剩余信息」是同一个模式的代价。

**架构推论：** 两套扁平形态的兼容（Pydantic `model_dump()` 输出 + 下划线字段）说明这个类的序列化格式已经变过至少三代。而兜底分支靠「特征字段组合」识别形态，这本质上是一种**形状嗅探**——它对合法输入有效，但无法区分「一个恰好有 `tool_name` 与 `arguments` 键的畸形输入」与「一个 `ToolCall`」。判据是「畸形输入本来就该报错」，所以嗅探失败时会走到那条详细错误消息，而不是产生错误的对象。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/dataclasses/chat_message.py` | `ChatRole` | 四角色枚举（`str` 子类，显式值） |
| `haystack/dataclasses/chat_message.py` | `ChatRole.from_str` | 字符串转角色，失败时列出支持值 |
| `haystack/dataclasses/chat_message.py` | `TextContent` / `ToolCall` / `ToolCallResult` / `ReasoningContent` | 四个自有块类型 |
| `haystack/dataclasses/chat_message.py` | `ChatMessageContentT` | 六种块的联合 |
| `haystack/dataclasses/chat_message.py` | `_CONTENT_PART_CLASSES_TO_SERIALIZATION_KEYS` | 类 ↔ 序列化键的双射表 |
| `haystack/dataclasses/chat_message.py` | `_serialize_content_part` | 按精确类型序列化，`TextContent` 不嵌套 |
| `haystack/dataclasses/chat_message.py` | `_deserialize_content_part` | 扁平文本优先 + 六键 + 五条形状嗅探兜底 |
| `haystack/dataclasses/chat_message.py` | `ChatMessage` | 四字段消息 + 六个列表/单值访问器对 |
| `haystack/dataclasses/chat_message.py` | `ChatMessage.from_user` / `from_system` / `from_assistant` / `from_tool` | 四个工厂，各自隐含角色-块的约束 |
| `haystack/dataclasses/chat_message.py` | `ChatMessage.to_dict` / `_to_dict` / `_to_trace_dict` | 序列化与追踪脱敏 |
| `haystack/dataclasses/chat_message.py` | `ChatMessage.from_dict` | 下划线字段重映射 + `str` content 兼容 |
| `haystack/dataclasses/chat_message.py` | `ChatMessage.to_openai_dict_format` | 三道前置校验 + 四支分派 |
| `haystack/dataclasses/chat_message.py` | `_build_openai_user_message_fields` | 单文本简化 / 多块过滤 |
| `haystack/dataclasses/chat_message.py` | `_build_openai_tool_result_message_fields` | 只支持字符串或纯文本结果 |
| `haystack/dataclasses/chat_message.py` | `_build_openai_assistant_message_fields` | text/tool_calls/refusal 三者的取舍 |
| `haystack/dataclasses/chat_message.py` | `ChatMessage.from_openai_dict_format` | 五角色 + assistant/tool/user/system 分支 |
| `haystack/dataclasses/chat_message.py` | `_haystack_content_part_to_openai_dict` | 三模态转换 + MIME/filename 兜底 |
| `haystack/dataclasses/chat_message.py` | `_openai_dict_to_haystack_content_part` | 反向转换，拒绝 `file_id` 与 http URL |
| `haystack/dataclasses/chat_message.py` | `_parse_openai_base64_data_url` | data URL 拆 MIME 与载荷 |
| `haystack/dataclasses/chat_message.py` | `_parse_openai_tool_call_arguments` | 三态解析，`JSONDecodeError` 未捕获 |
| `haystack/dataclasses/chat_message.py` | `_parse_openai_text_and_refusal_content` | 按类型分组合并，refusal 不混入 text |

## 参考

- [haystack 源码：dataclasses/chat_message.py](https://github.com/deepset-ai/haystack/blob/main/haystack/dataclasses/chat_message.py)
- [haystack 文档：ChatMessage](https://docs.haystack.deepset.ai/docs/data-classes)
- [OpenAI 文档：Chat Completions 消息格式](https://platform.openai.com/docs/api-reference/chat/create)
- [RFC 2397：data URL](https://datatracker.ietf.org/doc/html/rfc2397)

## 与主核 / AI 的关系

`ChatMessage` 的「角色 + 有序块」结构与主核（多模态数据库内核）的**行 + 有序列**在形态上相似，但语义完全不同：主核的列顺序是 schema 固定的，而块顺序是**语义的**（「先说后调工具」与「先调后说」不同）。所以这里的顺序不是布局细节，而是内容的一部分。

真正值得对照的是**双向转换的有损性**，它与主核的**字符集转换**（`SET client_encoding`）同构。主核在 `LATIN1 → UTF8` 转换时，某些字节序列在目标字符集里没有对应，于是只能替换或报错。haystack 在 `ChatMessage → OpenAI` 时，推理内容没有对应槽位，于是丢弃。两者的共同点是**目标表示的表达力更弱**，差别是主核可以报错（字符编码错误是硬错误）而 haystack 选择静默丢弃（因为「丢推理内容」不影响主流程）。

第二处对照是 **`detail` / `filename` 的兜底值**。这属于「协议适配层的默认值」，主核里对应的是 `DateStyle`、`IntervalStyle` 这类输出格式设置——同一个内部值在不同输出格式下需要一个默认的外部表示。判据是「这个默认值会不会掩盖真实信息」：`filename="filename"` 显然会（用户不知道真实文件名丢了），而 `image/jpeg` 兜底相对安全（OpenAI 本来就容忍）。

第三处是**形状嗅探**（`_deserialize_content_part` 的兜底五分支）。主核里对应的是 **`pg_dump` 的版本兼容格式解析**——同一个 SQL 输出在不同版本间形状会变，读取方必须靠特征判断版本。这类代码的共同弱点是「无法区分畸形输入与合法的旧版本输入」，只能靠「畸形输入报错」来兜底。

对 AI 工程的实际意义在于：聊天消息是所有 LLM 调用的输入载体，而多模态内容让它的形状迅速复杂化。这份实现给出的三条经验可直接用：**内容块用联合 + 顺序列表**（比 `content` + `tool_calls` 两个槽更能表达真实语义）、**转换的有损点要能数清**（块顺序、多段文本、`developer` 角色是本页的三处）、以及**跨轮次失败要提前裁剪**（refusal 与 tool_calls 冲突那条 warning 就是在防下一轮炸掉）。
