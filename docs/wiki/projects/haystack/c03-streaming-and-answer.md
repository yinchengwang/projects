---
project: haystack
feature: streaming-and-answer
title: 流式块与回答构建
page_type: feature
schema_version: 1
refs:
  - path: haystack/dataclasses/streaming_chunk.py
    symbols: [StreamingChunk, ToolCallDelta, select_streaming_callback]
  - path: haystack/dataclasses/answer.py
    symbols: [GeneratedAnswer, ExtractedAnswer]
  - path: haystack/components/builders/answer_builder.py
    symbols: [AnswerBuilder]
---

# 流式块与回答构建

## 一句话职责

`StreamingChunk` 是流式输出的最小单位（一条内容 + 归属的组件 + 是否块首 + 结束原因），并用 `index` 把碎片归到同一个内容块；`AnswerBuilder` 则把生成器的文本回复用正则抽成 `GeneratedAnswer`（答案串 + 引用到的文档 + 元数据），两者分别是「生成的中间态」与「生成的最终态」。

## 机制是什么

流式输出对数据结构提了三个硬要求，而它们互相牵制：

1. **碎片必须能拼回去**。模型一次吐几个 token，拼接责任落在消费方。要拼就得知道「这属于哪个块」——所以需要 `index`。
2. **一条流里可能同时有多种内容**。文本、工具调用、推理内容可以交错出现（甚至同一次流里先推理后说话），所以要能区分「这块碎片是什么」。
3. **结束原因只在最后一个碎片上**。所以 `finish_reason` 是可选的，且只有一个碎片带它。

`StreamingChunk` 用九个字段覆盖这三条，其中 `content` 是唯一必填的（非 `None` 的字符串，但允许空串）。

而 `AnswerBuilder` 面对的是相反的问题：**已经完整的文本**，但需要从中分离出「答案」与「引用」。它用两套正则：`pattern` 抽答案，`reference_pattern` 抽引用编号。这个「用正则约定输出格式」的做法很原始，但它把「提示词怎么写」与「答案怎么解析」解耦成了两个独立可配的东西。

## 怎么实现的

**`StreamingChunk.__post_init__` 有两条校验，而第二条的豁免值得逐字读。**

```python
def __post_init__(self) -> None:
    fields_set = sum(bool(x) for x in (self.content, self.tool_calls, self.tool_call_result, self.reasoning))
    if fields_set > 1:
        raise ValueError(
            "Only one of `content`, `tool_call`, `tool_call_result` or `reasoning` may be set in a StreamingChunk. "
            f"Got content: '{self.content}', tool_call: '{self.tool_calls}', "
            f"tool_call_result: '{self.tool_call_result}', reasoning: '{self.reasoning}'."
        )

    # NOTE: We don't enforce this for self.content otherwise it would be a breaking change
    if (self.tool_calls or self.tool_call_result or self.reasoning) and self.index is None:
        raise ValueError("If `tool_call`, `tool_call_result` or `reasoning` is set, `index` must also be set.")
```

四处：

1. **`sum(bool(x) for x in ...)` 统计「几个字段有值」**。用真值判断，所以 `content=""` 算「没设」。这与「`content` 是必填字段」形成微妙关系：一个 `content=""` 且没有其他字段的碎片，`fields_set` 是 0，校验通过——所以**空内容碎片是合法的**。

2. **互斥检查在四个字段之间**（`content`/`tool_calls`/`tool_call_result`/`reasoning`）。注意 `tool_calls` 是列表，`tool_call_result` 是单个对象，两者同时设也会被拒（`fields_set` 为 2）。

3. **`index` 只对三种非文本内容强制**。注释说明了豁免 `content` 的理由：「otherwise it would be a breaking change」——即早期版本的文本碎片不带 `index`，若现在强制会让所有既有实现报错。

   所以这是一处**兼容性优先于一致性**的取舍：理论上文本碎片也该带 `index`（同一次流里可能有多段文本），但历史原因不允许。

4. **错误消息里 `tool_call` 单数、字段名 `tool_calls` 复数**。消息说的是 "Only one of `content`, `tool_call`, ..." 而变量名是 `self.tool_calls`。这是一个小的措辞不一致。

**`to_dict`/`from_dict` 是手写的，而不是 `asdict`。**

```python
def to_dict(self) -> dict[str, Any]:
    return {
        "content": self.content,
        "meta": self.meta,
        "component_info": self.component_info.to_dict() if self.component_info else None,
        "index": self.index,
        "tool_calls": [tc.to_dict() for tc in self.tool_calls] if self.tool_calls else None,
        "tool_call_result": self.tool_call_result.to_dict() if self.tool_call_result else None,
        "start": self.start,
        "finish_reason": self.finish_reason,
        "reasoning": self.reasoning.to_dict() if self.reasoning else None,
    }
```

三处：

1. **必须手写**，因为 `component_info`/`tool_calls`/`tool_call_result`/`reasoning` 都是嵌套对象，`asdict` 会递归但不会调它们的 `to_dict`——而 `Document`（在 `ToolCallResult` 里）需要 `to_dict(flatten=False)` 这种定制行为。

2. **`if self.tool_calls else None`** —— 空列表也输出 `None`。所以 `tool_calls=[]` 与 `tool_calls=None` 序列化后相同。

3. **九个字段全部输出**（即使为 `None`），所以序列化结果是**定形的**——这与 `Document.to_dict(flatten=False)` 输出全部字段是一致风格。

`from_dict` 反向：

```python
if "content" not in data:
    raise ValueError("Missing required field `content` in StreamingChunk deserialization.")
```

只校验 `content` 存在（因为它是唯一必填）。其余用 `data.get(...)` 加默认值。注意 `finish_reason=data.get("finish_reason")` 没有类型校验——一个非法字符串（如 `"done"`）会被赋值给声明为 `FinishReason | None` 的字段，而 dataclass 不做运行时校验。

**`FinishReason` 的五个值里有四个来自 OpenAI，一个是 haystack 自造的。**

```python
FinishReason = Literal["stop", "length", "tool_calls", "content_filter", "tool_call_results"]
```

注释直说了：「Type alias for standard finish_reason values following OpenAI's convention plus Haystack-specific value ("tool_call_results")」。

最后一个 `"tool_call_results"` 是复数形式的自造值。而 OpenAI 的 `"tool_calls"` 也在列表里——所以两者语义不同：`"tool_calls"` 表示「模型要调工具」（流结束，等外部执行），`"tool_call_results"` 表示「这次流的内容是工具结果」（组件产出的不是模型输出）。

这是一个**借用外部协议词汇表并扩展**的做法。它让消费方能用一套 `match` 处理，但也意味着「`tool_calls` 与 `tool_call_results` 只差一个 s」这种易混点。

**`ToolCallDelta` 与 `ToolCall` 的差别是「参数是字符串还是对象」。**

```python
@dataclass
class ToolCallDelta:
    index: int
    tool_name: str | None = None
    arguments: str | None = None      # 字符串！
    id: str | None = None
    extra: dict[str, Any] | None = None
```

对照 c02 的 `ToolCall`：`tool_name: str`（必填）、`arguments: dict[str, Any]`（必填）、`id: str | None`。四处差别：

1. **`index` 是 `ToolCallDelta` 的必填首字段，`ToolCall` 没有**。因为流式时必须知道「这个碎片属于第几个工具调用」——一次流里可以有多个并行工具调用。
2. **`tool_name` 可选**。因为名字可能是分片到达的（第一个碎片带名字，后续只带参数）。
3. **`arguments` 是 `str`**——半截 JSON 无法解析成 dict。这是流式的本质约束。
4. **`delta` 后缀**表明它是增量。

所以「拼回 `ToolCall`」这件事需要：按 `index` 分组 → 拼接 `arguments` 字符串 → `json.loads` → 组装。而**这个拼接逻辑不在本页的源码里**（在 chat 生成器组件里）。

**`ComponentInfo` 用 `__component_name__` 这个约定属性标识组件。**

```python
@classmethod
def from_component(cls, component: Component) -> "ComponentInfo":
    component_type = f"{component.__class__.__module__}.{component.__class__.__name__}"
    component_name = getattr(component, "__component_name__", None)
    return cls(type=component_type, name=component_name)
```

两处：

1. **`type` 用 `module + __name__`**，与 b02 的 `generate_qualified_class_name` 完全一致。所以「全限定类名」这个约定复用了。
2. **`name` 从 `__component_name__` 取**（管道在 `add_component` 时注入的实例属性），取不到就是 `None`。所以「组件在管道里的名字」与「类的类型名」是两个独立信息——`StreamingChunk` 同时带两者，让消费者能定位到「是哪个组件的哪一行」。

**`select_streaming_callback` 是同步/异步兼容的裁决点，而它的规则是「一侧警告、另一侧报错」。**

```python
if init_callback is not None and runtime_callback is None:
    if requires_async and not _is_callable_async_compatible(init_callback):
        logger.warning("A sync streaming callback was provided at initialization for use in an async context. "
                       "It will run synchronously on the event loop and may block it.")
    if not requires_async and _is_callable_async_compatible(init_callback):
        raise ValueError("The init callback cannot be a coroutine.")

if runtime_callback is not None:
    ... 同结构，消息里说 "at runtime" ...
```

四处：

1. **优先检查哪个回调**：注意两个 `if` 的守卫不同——第一个只在 `runtime_callback is None` 时检查 init；第二个检查 runtime。所以**只检查「会被选中」的那个回调**。文档明说了：「Only the selected callback is checked for compatibility with the execution context」。

   这个设计是对的：一个不兼容的 init 回调若被 runtime 回调覆盖，就不必报错——它根本不会被调用。

2. **`requires_async=True` + 同步回调 → 警告**（不报错）。理由：「It will run synchronously on the event loop and may block it」。所以同步回调在异步上下文里是**可用的但危险**。

3. **`requires_async=False` + 异步回调 → 报错**。理由清楚：同步上下文里没有事件循环，无法 `await` 一个协程。所以这是硬错误。

4. **不对称的方向是对的**：能降级（异步环境跑同步代码）就降级并警告，不能降级（同步环境跑异步代码）就报错。这比「两个方向都报错」实用，也比「两个方向都警告」安全。

**`_is_callable_async_compatible` 只看 `iscoroutinefunction`。**

```python
def _is_callable_async_compatible(func: Callable) -> bool:
    return inspect.iscoroutinefunction(func)
```

**这是一个不完整的判据**：一个同步函数返回 `Awaitable`（例如 `functools.partial` 包了一个协程，或一个返回 coroutine 的普通函数）不是 `coroutinefunction`，但它是可 await 的。而 `_invoke_streaming_callback` 里用了更宽的判据：

```python
async def _invoke_streaming_callback(callback: StreamingCallbackT, chunk: StreamingChunk) -> None:
    result = callback(chunk)
    if inspect.isawaitable(result):
        await result
```

即**调用后看返回值是不是 awaitable**。所以两处用了不同判据：

| 位置 | 判据 | 覆盖 |
|---|---|---|
| `select_streaming_callback` | `iscoroutinefunction` | 只认 `async def` |
| `_invoke_streaming_callback` | `isawaitable(返回值)` | 认任何可等待返回 |

后果：一个「同步函数返回协程」的回调在 `requires_async=False` 时**不会被拒绝**（因为不是 `coroutinefunction`），但它返回的协程永远不会被 await → **协程泄漏**（Python 会警告 "coroutine was never awaited"）。

反过来，在 `requires_async=True` 时它不会被警告（不是 `coroutinefunction`），但实际上 `_invoke_streaming_callback` 会正确地 await 它——所以那条路径反而是对的。

**`GeneratedAnswer` 的 `to_dict` 要处理 `meta` 里可能混着 `ChatMessage` 对象这件事。**

```python
def to_dict(self) -> dict[str, Any]:
    meta = self.meta
    all_messages = meta.get("all_messages")
    if all_messages and isinstance(all_messages[0], ChatMessage):
        meta = {**meta, "all_messages": [msg.to_dict() for msg in all_messages]}
    return {
        "data": self.data,
        "query": self.query,
        "documents": [doc.to_dict(flatten=False) for doc in self.documents],
        "meta": meta,
    }
```

四处：

1. **`meta` 里可以放 `list[ChatMessage]`**，键名是 `"all_messages"`。所以 `meta` 不是「纯 JSON 元数据」——它可以携带非平凡对象。而 `to_dict` 负责在序列化时把它们转掉。

   注释说「`all_messages` is either a list of `ChatMessage` objects or a list of strings」——所以两种形态并存。后者来自早期版本（那时只有字符串）。

2. **`isinstance(all_messages[0], ChatMessage)`** —— 只看**第一个元素**。所以一个混合列表（前几个是 `ChatMessage`，后面是字符串）会被当作「全是 `ChatMessage`」处理，然后 `msg.to_dict()` 在字符串上 `AttributeError`。

3. **`meta = {**meta, "all_messages": ...}` 复制一份**，不改原 `self.meta`。这是必要的（否则序列化会改变对象状态）。

4. **`documents` 用 `to_dict(flatten=False)`** —— 与 c01 的 `Document.__eq__` 一致选择，保证 meta 结构不被摊平。

`from_dict` 里对称地转回来：

```python
# Copy `meta` before converting `all_messages` so the caller's input dict is left untouched.
meta = dict(data.get("meta", {}))
if (all_messages := meta.get("all_messages")) and isinstance(all_messages[0], dict):
    meta["all_messages"] = [ChatMessage.from_dict(m) for m in all_messages]
```

注释明确说了复制理由。这里判据是「第一个元素是 `dict`」——与 `to_dict` 侧的「是 `ChatMessage`」对称。所以一个 `all_messages` 是字符串列表的旧数据会因为 `isinstance("...", dict)` 为假而原样保留。

**两个 Answer 类都支持「旧格式的 `init_parameters` 信封」。**

```python
# Backward compatibility: the old format wrapped the fields in an `init_parameters` envelope.
if "init_parameters" in data:
    data = data["init_parameters"]
```

两个类的 `from_dict` 都有这四行（`ExtractedAnswer` 与 `GeneratedAnswer`）。所以早期序列化格式是 b02 的组件信封（`{"type":..., "init_parameters": {...}}`），而现在是裸字典。

**注意 `to_dict` 不产出那个信封**——所以这是**单向兼容**：能读旧的，只能写新的。这是合理的（不需要维持旧格式），但它意味着「用旧版本加载新存档」不可能。

**`ExtractedAnswer` 里有一个嵌套类 `Span`，它被重复装饰并用自己的名字引用。**

```python
@_warn_on_inplace_mutation
@dataclass
class ExtractedAnswer:
    ...
    @_warn_on_inplace_mutation
    @dataclass
    class Span:
        start: int
        end: int
```

两处：

1. **`Span` 是嵌套定义在 `ExtractedAnswer` 里的**，且自己也被 `_warn_on_inplace_mutation` 装饰。所以 `Span` 也不可原地改。

   但注意：嵌套类**无法通过 `generate_qualified_class_name` 往返**（b02 讲过用 `__name__` 会丢掉外层名）。而这里 `Span` 不参与组件序列化（它只在 `to_dict` 里用 `asdict`），所以不受影响。

2. **`document_offset: Optional["Span"]` 用字符串注解** —— 因为类体内引用自身命名空间里的嵌套类需要延迟解析。这是 Python 里嵌套类自引用的标准手法。

**`to_dict` 对 `Span` 用 `asdict`，对 `Document` 用 `to_dict(flatten=False)`。**

```python
"document": self.document.to_dict(flatten=False) if self.document is not None else None,
"document_offset": asdict(self.document_offset) if self.document_offset is not None else None,
```

两处不同：

1. **`Document` 走 `to_dict(flatten=False)`** —— 因为它需要定制序列化（`blob`/`sparse_embedding` 的转换，c01）。
2. **`Span` 走 `asdict`** —— 因为它是纯数据（两个 int），`asdict` 足够。

所以这里的选择标准是「嵌套类型有没有定制序列化需求」。

**`AnswerBuilder` 是「正则约定」的完整实现，而它的每个设计点都在处理「模型输出不可靠」。**

构造时校验正则的捕获组数量：

```python
@staticmethod
def _check_num_groups_in_regex(pattern: str) -> None:
    num_groups = re.compile(pattern).groups
    if num_groups > 1:
        raise ValueError(f"Pattern '{pattern}' contains multiple capture groups. "
                         "Please specify a pattern with at most one capture group.")
```

**为什么最多一个捕获组？** 因为 `_extract_answer_string` 的取值逻辑只能处理三种情况：

```python
if match := re.search(pattern, reply):
    if not match.lastindex:
        return match.group(0)   # 无捕获组 → 整个匹配
    return match.group(1)       # 一个捕获组 → 组 1
return ""
```

三处：

1. **无捕获组用 `group(0)`**（整个匹配），有捕获组用 `group(1)`。所以「`Answer: (.*)`」抽组 1，「`[^\n]+$`」抽整个匹配。

2. **`match.lastindex` 判断有无捕获组** —— 注意这个属性是「最后一个匹配的组号」，若捕获组**没有参与匹配**（可选组），它是 `None`。所以 `(?:...)` 这种非捕获结构不算，而未匹配的可选组会让 `lastindex` 为 `None` → 走 `group(0)` 分支。

   这是一个细微的行为：`pattern = "(foo)?(bar)"` 在输入 `"bar"` 上，`lastindex` 是 2（第二个组匹配了），所以返回 `group(1)` 而它是 `None`——于是 `_extract_answer_string` 返回 `None` 而不是 `str`。**类型契约被破坏**。

   而 `_check_num_groups_in_regex` 会拦住 `(foo)?(bar)`（两个组 → 报错）。所以这个漏洞被前置校验挡住了。但如果用户用 `"(foo)?"`（一个可选组且未匹配），`lastindex` 为 `None` → `group(0)` → 返回空串。这是安全的。

3. **没匹配就返回 `""`**（空串），不是原文本也不是 `None`。所以「正则没匹配」被归一成空答案——静默。

**`_extract_reference_idxs` 是本页最复杂的一段，而它有三层防护。**

```python
matches = re.findall(reference_pattern, reply)
idxs: set[int] = set()
for match in matches:
    try:
        if expand_ranges:
            for part in match.split(","):
                part = part.strip()
                if not part:
                    continue
                if "-" in part:
                    start_str, end_str = part.split("-", 1)
                    start, end = int(start_str), int(end_str)
                    if start > end:
                        continue
                    if num_documents is not None:
                        end = min(end, num_documents)
                    idxs.update(range(start - 1, end))
                else:
                    idxs.add(int(part) - 1)
        elif match:
            idxs.add(int(match) - 1)
    except ValueError as e:
        raise ValueError(
            f"Reference pattern '{reference_pattern}' matched '{match}', which is not a document index. ..."
        ) from e
```

四处：

1. **`re.findall` 的返回类型依赖捕获组数量**：零个组返回 `list[str]`，一个组返回 `list[str]`（组内容），多个组返回 `list[tuple]`。而 `_check_num_groups_in_regex` 保证最多一个组，所以这里总是 `list[str]`。

2. **`elif match:` 处理空字符串** —— 注释：「An optional capture group that did not participate in the match yields an empty string」。所以空串被跳过而不是 `int("")` 报错。

3. **区间展开时 `end = min(end, num_documents)`** —— 注释：「Clamp the range end to the number of documents to avoid materializing a huge set from an out-of-range citation like `[1-999999999]`」。这是一个**真实的 DoS 防护**：模型输出一个巨大的区间会让 `range()` 撑爆内存。

   注意 clamp 的是 `end` 而不是 `start`。所以 `[5-3]`（start > end）被 `if start > end: continue` 跳过，而 `[1-999999999]` 的 `end` 被压到 `num_documents`。

   注意 `idxs.update(range(start - 1, end))` —— `range` 的第二个参数是**开区间**，而 `end` 是 1-based 的文档号。所以 `[1-3]` 展开成 `range(0, 3)` = `{0, 1, 2}`，即文档 1、2、3（0-based）。正确。

4. **`except ValueError` 给出可操作的消息**：说明匹配到了什么、期望什么。而 `int(match)` 在 `match = "abc"` 时抛 `ValueError`，被接住。

   注意 `int("-5")` 不抛错，得到 `-5`，于是 `idxs` 里有 `-6`。而下面的循环有边界检查（见下）。

**`run` 里有三处「静默归一」与一处「显式边界检查」。**

```python
if not meta:
    meta = [{}] * len(replies)
elif len(replies) != len(meta):
    raise ValueError(f"Number of replies ({len(replies)}), and metadata ({len(meta)}) must match.")
```

`[{}] * len(replies)` —— **乘法复制出同一个字典对象的引用**。所以所有 reply 共享一个 `{}`。由于后面只读不改（`{**extracted_metadata, **given_metadata}` 生成新字典），这没有问题——但它是一个容易出错的手法。

```python
replies_to_iterate = replies[-1:] if self.last_message_only and replies else replies
meta_to_iterate = meta[-1:] if self.last_message_only and meta else meta
```

**`replies[-1:]` 而不是 `[replies[-1]]`** —— 用切片可以安全处理空列表（得到 `[]` 而不是 `IndexError`）。这是一个正确的细节。而 `and replies` 是多余的（`replies[-1:]` 在空列表上也返回 `[]`），但它让意图更明确。

```python
for reply, given_metadata in zip(replies_to_iterate, meta_to_iterate, strict=True):
```

**`strict=True`**（Python 3.10+）—— 长度不等时抛 `ValueError` 而不是静默截断。这是必要的：前面已经校验过长度相等，但 `last_message_only` 的切片可能产生不一致（若 `replies` 为空而 `meta` 非空，两者切片都为空，所以一致）。所以 `strict=True` 是防御性的。

`extracted_metadata` 的三步：

```python
extracted_metadata = reply.meta if isinstance(reply, ChatMessage) else {}
extracted_metadata = {**extracted_metadata, **given_metadata}
extracted_metadata["all_messages"] = replies
```

三处：

1. **`ChatMessage` 的 `meta` 被并入**——所以生成器的元数据（如 token 用量）会进 `GeneratedAnswer`。
2. **`given_metadata` 覆盖 `reply.meta`**（后者在前）。所以显式传的 meta 优先。
3. **`extracted_metadata["all_messages"] = replies`** —— 存的是**整个 replies 列表**，不是当前这个。所以每个 `GeneratedAnswer` 都带着完整的多轮消息，而不只是自己那一轮。这正是 c02 里 `GeneratedAnswer.to_dict` 要处理 `all_messages` 的原因。

   注意这里**存的是原列表的引用**，而 `replies` 可能是 `list[ChatMessage]`。所以 `GeneratedAnswer.meta` 里有活对象——序列化时才转换。

**文档引用的边界检查值得单独看，因为它防的是一个 Python 特有的陷阱。**

```python
for idx in sorted(doc_idxs):
    # An explicit bounds check is needed because references are 1-based: a reference like [0]
    # yields idx = -1, which Python would otherwise silently resolve to the last document.
    if not 0 <= idx < len(documents):
        logger.warning("Document index '{index}' referenced in Generator output is out of range. ", index=idx + 1)
        continue
    doc = documents[idx]
```

三处：

1. **注释指出的陷阱是真实的**：引用从 1 开始，所以 `[0]` 得到 `idx = -1`，而 Python 的 `documents[-1]` 是**最后一个文档**——不会报错。这是一个「错误输入被静默解释成合法值」的经典案例。

2. **检查用 `not 0 <= idx < len(documents)`**（链式比较），覆盖负数与超上界两种情况。

3. **警告里用 `idx + 1`** 还原成用户看到的引用编号。所以日志说「Document index '0'」而不是「-1」——面向用户。

`doc_meta` 的构造：

```python
doc_meta: dict[str, Any] = dict(doc.meta or {})
doc_meta["source_index"] = idx + 1
if reference_pattern:
    doc_meta["referenced"] = idx in referenced_idxs
referenced_docs.append(replace(doc, meta=doc_meta))
```

四处：

1. **`dict(doc.meta or {})` 复制**——所以不改输入文档。
2. **`source_index` 是 1-based**，与引用编号一致。
3. **`referenced` 只在有 `reference_pattern` 时加**。所以「没有引用模式」时文档元数据里没有这个键——文档说明了这个区别。
4. **`replace(doc, meta=doc_meta)`** —— 用 `dataclasses.replace` 造副本。这与 c01 里 `_warn_on_inplace_mutation` 的建议一致（不要原地改，用 `replace`）。

   注意 `replace` 会走一遍 `Document.__post_init__`——所以**id 会被重新计算**（若 `doc.id` 为空）。但 `doc` 已有 id，所以 `self.id or self._create_id()` 保留原 id。于是「加了 `source_index` 的副本与原文档同 id 但 meta 不同」——这破坏了「同 id 即同内容」的不变量。

## 关键取舍

**源码事实：** `StreamingChunk` 九字段，只有 `content` 必填；`__post_init__` 用 `sum(bool(x) for x in ...)` 统计四个内容字段并禁止多于一个，因此 `content=""` 算「未设」。

**源码事实：** `index` 只对 `tool_calls`/`tool_call_result`/`reasoning` 强制，注释说明对 `content` 豁免是因为「otherwise it would be a breaking change」。

**源码事实：** `StreamingChunk.to_dict`/`from_dict` 是手写的（不能用 `asdict`，因为嵌套的 `ToolCallResult`/`ReasoningContent` 需要各自的 `to_dict`）；空 `tool_calls` 序列化成 `None`；`from_dict` 只校验 `content` 存在，`finish_reason` 无类型校验。

**源码事实：** `FinishReason` 四值取自 OpenAI 约定，加一个 haystack 自造的 `"tool_call_results"`——与 OpenAI 的 `"tool_calls"` 只差一个 s，语义不同（前者是「流的内容是工具结果」，后者是「模型要调工具」）。

**源码事实：** `ToolCallDelta` 与 `ToolCall` 的差别是：`index` 必填、`tool_name` 可选、`arguments` 是 `str`（半截 JSON 无法解析）。

**源码事实：** `ComponentInfo.type` 用 `module + __name__`（与 b02 的 `generate_qualified_class_name` 同一约定），`name` 从 `__component_name__` 取。

**源码事实：** `select_streaming_callback` 只检查「会被选中的那个」回调（init 只在 runtime 为 `None` 时检查）；`requires_async=True` + 同步回调只**警告**（会阻塞事件循环），`requires_async=False` + 异步回调**报错**（无法 await）。

**源码事实：** 两处对「异步」用了不同判据：`select_streaming_callback` 用 `inspect.iscoroutinefunction`，`_invoke_streaming_callback` 用 `inspect.isawaitable(返回值)`。因此「同步函数返回协程」的回调在同步上下文里不会被拒绝，但它的协程永远不会被 await（协程泄漏）。

**源码事实：** `GeneratedAnswer.to_dict` 把 `meta["all_messages"]` 里的 `ChatMessage` 逐个序列化，判据只看**第一个元素**（混合列表会 `AttributeError`）；`from_dict` 对称地按「第一个元素是 `dict`」转回，并先复制 `meta` 以免改调用方数据。

**源码事实：** 两个 Answer 类的 `from_dict` 都兼容旧格式的 `init_parameters` 信封，而 `to_dict` **不产出**该信封——单向兼容。

**源码事实：** `ExtractedAnswer.Span` 是嵌套类，用字符串注解 `Optional["Span"]` 自引用；`document_offset`/`context_offset` 用 `asdict` 序列化，而 `document` 用 `to_dict(flatten=False)`。

**源码事实：** `AnswerBuilder` 构造时校验两个正则各自最多一个捕获组——因为取值逻辑只有「无组用 `group(0)`、一个组用 `group(1)`」两条路；没匹配时返回空串（静默）。

**源码事实：** `_extract_reference_idxs` 三条防护：空匹配跳过（可选组未参与匹配会得到空串）、区间展开时把 `end` 压到 `num_documents`（防 `[1-999999999]` 撑爆内存）、`int()` 失败时给出可操作错误。

**源码事实：** `run` 里 `meta` 缺省用 `[{}] * len(replies)`（共享同一字典对象，但后续只读）；`last_message_only` 用切片 `replies[-1:]` 而非下标（空列表安全）；`zip(..., strict=True)` 防长度不匹配。

**源码事实：** `extracted_metadata["all_messages"] = replies` 存的是**整个列表**的引用（不是当前元素），所以每个 `GeneratedAnswer` 都带完整多轮消息——这正是 `to_dict` 要处理它的原因。

**源码事实：** 文档引用的边界检查注释指出 `[0]` 会得到 `idx = -1`，而 Python 的 `documents[-1]` 静默取最后一个；警告里用 `idx + 1` 还原成用户看到的编号。

**源码事实：** 文档副本用 `dict(doc.meta or {})` + `replace(doc, meta=...)` 构造，`source_index` 为 1-based；因原文档已有 id，`replace` 后 id 不变——于是「同 id 但 meta 不同」的两个文档并存。

**架构推论：** `StreamingChunk` 的四字段互斥（内容/工具调用/工具结果/推理）配合 `index`，实际上定义了「流里的一条消息由若干个**块**组成，每个块由若干**碎片**组成」这个双层结构。而 `index` 只标块、不标块内位置——所以块的顺序靠**首次出现的顺序**推断，碎片顺序靠**到达顺序**。这个设计对「单次流只产一个块」的场景足够，但对「文本与工具调用交错」需要消费方自己维护 `index → 缓冲` 的映射。

**架构推论：** 同步/异步回调的不对称处理（一侧警告一侧报错）是一条正确的判据：**能降级就降级并告知，不能降级就拒绝**。「异步环境跑同步代码」能跑（阻塞但有结果），「同步环境跑异步代码」跑不了（没有事件循环）。把这个方向搞反（两个都报错或都警告）会分别导致「过度严格」与「静默失败」。

**架构推论：** `AnswerBuilder` 的「正则约定」暴露了一个 RAG 里常见的结构问题：**生成器的输出格式是一个契约，但它由提示词决定，而提示词与解析器是两个独立配置项**。所以「改提示词忘了改正则」是必然会发生的一类错误，而框架无法检测（因为两者都是自由字符串）。这个问题的正确解法是让提示词与解析器绑定（像 c02 的选择器那样「提示必须带 `output_parser`」），而 `AnswerBuilder` 没有这么做。

**架构推论：** `replace(doc, meta=...)` 造成「同 id 不同内容」这一点，是「id 由内容算出」与「需要给副本加元数据」之间的冲突。c01 讲过的 id 计算包含 `meta`，所以按理论 `replace` 后 `__post_init__` 会重算 id——但代码是 `self.id or self._create_id()`，而 `doc.id` 已经非空，所以**保留旧 id**。于是这个副本的身份是错的（它的 meta 变了但 id 没变）。正确做法是复制时清空 id，让它重算——但那会让「引用后的文档无法用原 id 找到」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/dataclasses/streaming_chunk.py` | `FinishReason` | 四值取自 OpenAI + 一个自造值 |
| `haystack/dataclasses/streaming_chunk.py` | `ToolCallDelta` | 流式工具调用碎片（`arguments` 是字符串、`index` 必填） |
| `haystack/dataclasses/streaming_chunk.py` | `ComponentInfo` | 组件类型 + 管道内名字 |
| `haystack/dataclasses/streaming_chunk.py` | `StreamingChunk` | 九字段碎片 + 四字段互斥校验 |
| `haystack/dataclasses/streaming_chunk.py` | `StreamingChunk.__post_init__` | 互斥检查 + `index` 的三种强制/一种豁免 |
| `haystack/dataclasses/streaming_chunk.py` | `StreamingChunk.to_dict` / `from_dict` | 手写序列化（嵌套对象各自转换） |
| `haystack/dataclasses/streaming_chunk.py` | `select_streaming_callback` | 只检查选中的回调；同步降级警告、异步硬报错 |
| `haystack/dataclasses/streaming_chunk.py` | `_is_callable_async_compatible` | `iscoroutinefunction` 判据 |
| `haystack/dataclasses/streaming_chunk.py` | `_invoke_streaming_callback` | `isawaitable(返回值)` 判据（与上一个不一致） |
| `haystack/dataclasses/answer.py` | `Answer` | `runtime_checkable` 的 `Protocol` |
| `haystack/dataclasses/answer.py` | `ExtractedAnswer` | 抽取式答案：分数 + 文档 + 上下文 + 两个 `Span` |
| `haystack/dataclasses/answer.py` | `ExtractedAnswer.Span` | 嵌套类，字符串注解自引用 |
| `haystack/dataclasses/answer.py` | `GeneratedAnswer` | 生成式答案：文本 + 引用文档 + meta（可含 `all_messages`） |
| `haystack/dataclasses/answer.py` | 两个 `from_dict` | 兼容旧 `init_parameters` 信封（单向） |
| `haystack/components/builders/answer_builder.py` | `AnswerBuilder` | 正则抽答案 + 抽引用 + 文档副本 |
| `haystack/components/builders/answer_builder.py` | `_extract_answer_string` | 无组用 `group(0)`、一组用 `group(1)`、不匹配返回空串 |
| `haystack/components/builders/answer_builder.py` | `_extract_reference_idxs` | 区间展开 + clamp + 空匹配跳过 |
| `haystack/components/builders/answer_builder.py` | `_check_num_groups_in_regex` | 最多一个捕获组的前置校验 |
| `haystack/components/builders/answer_builder.py` | `_resolve_reference_pattern` | `expand_reference_ranges` 时换成更宽的模式 |

## 参考

- [haystack 源码：dataclasses/streaming_chunk.py](https://github.com/deepset-ai/haystack/blob/main/haystack/dataclasses/streaming_chunk.py)
- [haystack 源码：dataclasses/answer.py](https://github.com/deepset-ai/haystack/blob/main/haystack/dataclasses/answer.py)
- [haystack 文档：流式输出](https://docs.haystack.deepset.ai/docs/streaming)
- [Python 文档：inspect.iscoroutinefunction 与 isawaitable](https://docs.python.org/3/library/inspect.html#inspect.isawaitable)

## 与主核 / AI 的关系

流式块与主核（多模态数据库内核）的**游标分批取行**在形态上相似：都是「一个完整结果被切成若干批交付」。但方向相反：主核的批是**服务端切好、客户端按需拉取**（`FETCH 100`），流式是**生产端主动推、消费端被动收**。这个差别决定了错误处理：主核的分批可以随时中断（客户端不发下一个 `FETCH`），而流式一旦开始推送，中途失败的语义必须由协议定义（haystack 用 `finish_reason` 承载，但它没有错误值——只有 `content_filter` 这个近似的）。

第二处对照是 `index` 这个字段与主核的**列号**。流式碎片需要 `index` 才能归组，而主核的每个值天然有列号。差别在于列的集合是 schema 固定的，而块的集合是运行时决定的——所以 `index` 只是一个序号，没有「第 0 号块必须是文本」这类约束。这正是 c02 里 `ChatMessage` 的块顺序语义在流式侧的对应：**顺序承载信息**。

第三处是 `AnswerBuilder` 的正则约定与主核的**`COPY` 格式选项**（`FORMAT csv, DELIMITER ',', QUOTE '"'`）。两者都是「用一个可配的格式描述来解析文本」，且都需要「写入方与读取方用同一套描述」。主核的解法是把这个描述放在**同一句 SQL 里**（`COPY ... TO` 与 `COPY ... FROM` 各写一次，容易写错但至少在一起）；`AnswerBuilder` 则把它拆到「提示词」与「组件参数」两处，更容易漂移。

对 AI 工程的实际意义在于：流式与最终态是两个必须分别设计的表示，而它们的接合点是最容易出错的。这份实现给出的三条经验可直接用：**流式碎片要能归组**（`index` 是必需的，对文本豁免是历史债）、**同步异步的兼容判据要一致**（`iscoroutinefunction` 与 `isawaitable` 混用会漏掉「同步函数返回协程」）、以及**1-based 引用必须显式边界检查**（`[0]` 在 Python 里会静默变成最后一个元素）。
