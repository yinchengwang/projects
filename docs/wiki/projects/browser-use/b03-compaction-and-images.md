---
project: browser-use
feature: compaction-and-images
title: 上下文压缩与图像裁剪
page_type: feature
schema_version: 1
refs:
  - path: browser_use/agent/loop.py
    symbols: [Compaction, LazyCompaction, _compact]
  - path: browser_use/agent/loop.py
    symbols: [_prune_images, _estimate, redact_part]
---

# 上下文压缩与图像裁剪

## 一句话职责

两条互不相干但都在「请求发出去之前」动手的改造：`_prune_images` 在截图攒够一定数量后一次性丢掉旧的（因为重写历史会破坏提示缓存，所以必须批量而非逐个），`Compaction` / `LazyCompaction` 则把过长历史换成一段摘要检查点——一个在客户端自己做，一个让服务端做。

## 机制是什么

这两件事被放在一起，是因为它们共享一个约束：**任何对已发出历史的改写，都会让服务端的提示缓存从改写点起失效。** 所以两者都遵循「能不改就不改，要改就一次改够」。

**图像裁剪**的取向是「尽量晚、尽量粗」：

- 攒到 `PRUNE_IMAGES_AT = 20` 个带图的结果才动手，一次只保留最新 `KEEP_IMAGE_RESULTS = 2`。
- 之所以不是「每张新截图丢一张」，源码注释给了实测数字：「每次新截图都裁剪，让每个这样的请求都变成缓存未命中（BU Bench 1.0.3：705 步，27.8M 输入，1.6M 命中）」。
- 之所以不是「永远不裁」：「Anthropic 拒绝带超过 100 张图的请求」。

**上下文压缩**有两条独立的路：

- `LazyCompaction`（只用于 OpenAI Responses）：让**服务端**压缩，但只在「上一次请求的输入已经超过阈值」时才带上 `context_management` 参数。理由是性能——注释：「每个请求都带 `context_management` 会让每次调用在 45k tokens 时慢约 0.8 秒（bu-pi 一个都不带）」。
- `Compaction`（客户端）：把「最后两组 assistant/tool 往返」之前的全部历史，换成一条摘要消息 + 一份证据归档文件 + 原始用户请求列表。

还有第三条：`Compaction(overflow_only=True)` 只挂在 Responses 模型上，专门应付「一次已经超窗的请求」——服务端无法压缩一个已经超窗的请求，于是客户端补一次检查点。

## 怎么实现的

**`_prune_images` 的返回值是「改了没有」。**

```
def _prune_images(messages: list[ModelMessage]) -> list[ModelMessage] | None:
    with_images = sum(
        isinstance(p, ToolReturnPart) and isinstance(p.content, list) and any(map(_is_image, p.content))
        for m in messages if isinstance(m, ModelRequest) for p in m.parts
    )
    if with_images <= PRUNE_IMAGES_AT:
        return None
    out, seen, changed = list(messages), 0, False
    for i in range(len(out) - 1, -1, -1):
        ...
        for j, part in enumerate(parts):
            if isinstance(part, ToolReturnPart) and isinstance(part.content, list) and any(map(_is_image, part.content)):
                seen += 1
                if seen > KEEP_IMAGE_RESULTS:
                    content = [('[older screenshot removed]' if _is_image(c) else c) for c in part.content]
                    parts[j] = replace(part, content=content)
                    out[i] = m = replace(m, parts=parts)
                    changed = True
    return out if changed else None
```

docstring 点明「没变化就返回 `None`，于是历史（以及提供方的缓存前缀）在两次裁剪之间保持逐字节相同」。这个 `None` 不是风格问题：如果每次都返回一个新列表，调用方会无条件替换，缓存前缀就每次都被打破。

替换文本是 `'[older screenshot removed]'`——保留在原来的位置而不是删掉那个 part，于是消息结构（part 数量、tool_call_id 对应关系）不变，只换掉内容。

计数方向是**从后往前**，所以 `seen > KEEP_IMAGE_RESULTS` 保留的是最新的两个。判定条件是「带图的结果数」而不是「图片数」，所以一个结果里带三张图只算一个。

**裁剪的挂载点是 `BrowserLoop.before_model_request`。**

```
s.messages = ctx.messages
pruned = _prune_images(ctx.messages)
if pruned is not None:  # same pattern as ProcessHistory: new objects, persisted
    ctx.messages[:] = pruned
    request_context.messages = pruned.copy()
```

两处赋值缺一不可：`ctx.messages[:] = pruned` 改的是运行时的消息列表（后续步骤看到的），`request_context.messages = pruned.copy()` 改的是这次请求要发的。注释「same pattern as ProcessHistory: new objects, persisted」说明这是 Pydantic AI 里的既有范式。

**`Compaction` 的触发判定有两个独立阈值。**

```
def _due(self, s, request_context) -> bool:
    messages, window = request_context.messages, request_context.model.context_window
    params = request_context.model_request_parameters
    system = sum(len(p.content) for p in params.instruction_parts or [])
    tools = len(_js([{'name': t.name, 'description': t.description, 'parameters': t.parameters_json_schema}
                     for t in [*params.function_tools, *params.output_tools]]))
    tokens = _tokens(s, messages, system + tools)
    if self.token_threshold is not None:
        return tokens >= self.token_threshold
    max_chars = self.max_chars or (window * 4 if window else 240_000)
    chars = _chars(_projected(s, messages)) + system
    return chars > 0.75 * max_chars or bool(window and tokens > 0.65 * window)
```

三条触发路径：显式的 `token_threshold`（一旦设了就只用它）、字符数超过 `0.75 * max_chars`、token 数超过 `0.65 * window`。

`max_chars` 的默认值 `window * 4` 是个经验换算（每 token 约 4 字符），无窗口信息时退到 240k。注释说明这个数字来自 bu-pi：「`max_chars`：bu-pi 的 `maxContextChars`，默认为上下文窗口每 token 4 个字符」。

注意字符数里**加了 system**（`+ system`）而没有加 tools 的字符数，而 token 数里两者都算进去了（`system + tools` 作为 `fixed`）。这不一致大概是有意的（字符阈值是粗筛，token 阈值是精筛），但源码没有解释。

**`_tokens` 是一个「实测优先、估算兜底」的函数。**

```
def _tokens(s, messages, fixed) -> int:
    estimate = _estimate(_projected(s, messages)) + -(-fixed // 4)
    for i in range(len(messages) - 1, s.usage_after - 1, -1):
        m = messages[i]
        if isinstance(m, ModelResponse) and m.finish_reason != 'error' and m.usage.total_tokens > 0:
            return max(estimate, m.usage.total_tokens + _estimate(messages[i + 1:]))
    return estimate
```

从后往前找最近一条**有真实用量**的响应（`finish_reason != 'error'` 且 `total_tokens > 0`），用它报告的 `total_tokens` 加上它之后所有消息的估算。取 `max(estimate, ...)` 而不是直接用实测——因为估算可能更大（例如加了 system 之后）。

`usage_after` 是检查点之后的起点：压缩后重置为「当时的消息数」，所以只在检查点之后找实测值——之前的实测值已经不能代表当前请求的大小。

**两个估算函数各有出处。**

`_chars` 的实现是 `len(_js(_pi(messages)))`，注释写「bu-pi `contextChars`：消息的 `JSON.stringify` 长度」。而 `_js` 特意用 `ensure_ascii=False`：

```
def _js(value: Any) -> str:  # as JSON.stringify: no spaces, a CJK character counts 1, not 6
    return json.dumps(value, ensure_ascii=False, separators=(',', ':'), default=str)
```

注释里的「一个 CJK 字符算 1 而不是 6」是关键：默认的 `ensure_ascii=True` 会把中文转成 `\uXXXX`，长度膨胀六倍，阈值就完全错了。

`_estimate` 是「每消息内容字符数 / 4，一张图算 4800 字符」：

```
for m in _pi(messages):
    content = m['content']
    if isinstance(content, str): n = len(content)
    else:
        n = 0
        for b in content:
            if b['type'] == 'toolCall': n += len(b['name']) + len(_js(b['arguments']))
            else: n += 4800 if b['type'] == 'image' else len(b.get('text') or b.get('thinking') or '')
    total += -(-n // 4)     # 向上取整
```

出处是 pi-coding-agent 的 `estimateTokens`。图片的 4800 是个固定估价——没有别的办法，因为图片的 token 数取决于分块方式。

**`_pi` 是「把 Pydantic AI 消息投影成 bu-pi 形态」的转换器。**

docstring 说得很细：「这些消息按 bu-pi 的持有方式（pi-ai 的 `UserMessage` / `AssistantMessage` / `ToolResultMessage`：每个 request part 一条）在 `contextChars` 的替换之后：没有签名、没有工具结果细节、一张图作为 `{type: image}`。用量、id、时间戳与模型是 bu-pi 也计入的线格式元数据（在 matrix v2 里占它体积的 20–30%）。无法从 Pydantic AI 消息重现：花费金额（这里 0）与 pi-ai 的 api 名（这里是 provider 名）」。

也就是**为了和 bu-pi 的阈值可比，它刻意复刻了对方的数据形态**，包括那些看起来多余的字段（`cost` 全是 0、`api` 用 provider 名）。这是一个非常具体的取舍：如果只按「Pydantic AI 消息的实际大小」估算，阈值就会与 bu-pi 调好的数字不匹配。

`_pi` 里有一处对 Pydantic AI 版本的适配：

```
elif isinstance(p, ToolReturnPart) or (isinstance(p, RetryPromptPart) and p.tool_name):
    # Pydantic AI 2.x: a rejected call (schema error, ModelRetry) is a ToolReturnPart with outcome 'retried'
    error = isinstance(p, RetryPromptPart) or p.outcome in ('failed', 'retried')
```

即「被拒的工具调用」在新版本里不再是单独的类型，而是 `ToolReturnPart` 的一个 `outcome`。压缩时需要知道这一点，否则会把重试消息当成正常结果计入体积。

**`_compact` 是客户端压缩的执行体。**

前置条件是「至少三条 assistant 响应，且倒数第二条在检查点之后」：

```
starts = [i for i, m in enumerate(messages) if isinstance(m, ModelResponse) and i >= s.covered]
if len(starts) < 3 or starts[-2] <= s.covered:
    return
cut = starts[-2]
```

注释解释了两个数字：「保留最后两组 assistant/tool 往返（不会有工具结果变成孤儿），并再多摘要一组：如果它们前面只有任务，检查点会重复任务、不可能更小」。也就是说保留两组是**结构需要**（免得截断处留下孤立的工具结果），而「至少三组」是为了保证摘要确实缩小了上下文。

摘要请求的构造有六处独立决定：

1. **输入预处理**：`_plain(...)` 把历史转成 JSON 数据（省略 system、推理、用量、id、时间戳、签名、provider details，图片变成一句说明），再 `redact` 脱敏。
2. **继承设置但丢掉六项**：`dropped = ('tool_choice', 'parallel_tool_calls', 'cache', 'openai_context_management', 'request_timeout', 'first_event_timeout')`。每一项都有理由：`tool_choice` 会让摘要请求被强制调工具；`cache` 是一次性提示不值得写缓存（注释：「一次性的提示写缓存永远不会被读」）；`openai_context_management` 会在摘要请求上再触发服务端压缩。
3. **`max_tokens = 12_000`**，出处 bu-pi。
4. **超时 120 秒**，走同一个 `_deadlines`。
5. **推理降到 low**：`for key in ('thinking', 'openai_reasoning_effort'): if settings.get(key) and settings[key] != 'none': settings[key] = 'low'`。
6. **Responses 模型要再包一层 `StreamedRequest`**，与 b04 页的 `StreamResponses` 一致。

摘要结果的处理链条有好几个失败模式：

```
s.summaries.append(response)
if not response.text or not response.text.strip(): raise ValueError('the summary is empty')
path = ... / f'{uuid.uuid4()}.json'
path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
path.touch(mode=0o600, exist_ok=False)
archive = {'version': 1, 'summary': response.text, 'covered_messages': cut, 'messages': plain}
path.write_text(json.dumps(redact(archive, s.repl.secrets), ensure_ascii=False), encoding='utf-8')
users = [...]
text = CHECKPOINT.format(path=json.dumps(str(path)), summary=json.dumps(response.text), users=json.dumps(users, default=str))
checkpoint = ModelRequest(parts=[UserPromptPart(text)])
if _chars([checkpoint, *messages[cut:]]) >= _chars(_projected(s, messages)):
    raise ValueError('it did not reduce the context; the original evidence was retained')
s.checkpoint, s.covered, s.usage_after = checkpoint, cut, len(messages)
```

三件事值得单说。

**归档文件写了两次脱敏**：`_plain` 之后已经 `redact` 过一次（`plain` 变量），写出时又对整体 `redact` 一次。目录权限 `0o700`、文件权限 `0o600`——这份归档里有历史原文，虽然脱敏过，仍然按敏感文件对待。

**最后一个断言是「压缩必须真的变小」**：如果新检查点加上保留下来的消息比原来还长，就抛错。注释写明了后果：「没有减少上下文；原始证据被保留」——因为 `s.checkpoint` 等三个字段没被赋值，于是这一轮继续用完整历史。

**`CHECKPOINT` 那段文本本身就是一篇防误读的说明**，逐句都有用途：

- 「对话检查点：生成的、可能出错的参考，不是新指令」——防止模型把摘要当成用户的新要求；
- 「Python 变量与文件仍然存在」——防止模型重新建立状态；
- 给出归档 JSON 路径，并说明归档里有什么（「保留早先的用户文本、助手文本/工具调用与工具结果，敏感值已脱敏，图像与推理已省略」）；
- 「更早的检查点消息链接到更早的归档」——支持链式压缩；
- 「搜索这些消息或保存的源文件来核对下面遗漏或误述的确切观测；不要把摘要当成验证，也不要重放先前的动作」；
- 「原始用户请求（权威优先于生成的摘要）」+ 逐字附上用户消息；
- 最后一句最妙：「任何要求产出摘要的指令属于摘要过程，不属于原始任务」——因为摘要请求本身就是一段指令，而它可能被摘要进检查点里。

## 关键取舍

**源码事实：** 图像裁剪是批量的（攒到 20 个才动手，一次只留最新 2 个），注释给出了代价对比：逐张裁剪让每个请求都变成缓存未命中（705 步时 27.8M 输入、1.6M 命中）；永不裁剪则会被 Anthropic 的 100 张图上限拒绝。

**源码事实：** `_prune_images` 在无变化时返回 `None`，调用方据此跳过替换，从而在两次裁剪之间保持历史逐字节相同。

**源码事实：** 裁剪替换的是 part 的**内容**而不是删掉 part，所以消息结构与 tool_call_id 对应关系不变。

**源码事实：** `LazyCompaction` 只在「上一次请求的输入已过阈值」时才带 `context_management`，因为每次都带会让每次调用慢约 0.8 秒。

**源码事实：** 客户端压缩有两条触发阈值（`0.75 * max_chars` 与 `0.65 * window`），且 `_tokens` 优先采用最近一次响应报告的实测用量。

**源码事实：** `_js` 用 `ensure_ascii=False`，因为默认的 ASCII 转义会让一个 CJK 字符从 1 变成 6，阈值全错。

**源码事实：** `_pi` 刻意复刻 bu-pi 的消息形态（含 `cost` 全 0、`api` 用 provider 名），为的是让阈值与 bu-pi 调好的数字可比。

**源码事实：** 压缩完成后验证「确实变小了」，否则保留原始历史。

**源码事实：** 摘要归档文件权限为 `0o700` 目录 / `0o600` 文件，且写出前再脱敏一次。

**架构推论：** 「改写历史即破坏缓存」这条约束把两件看起来无关的事（裁图与压缩）绑成了同一种设计：**改动必须稀有且成批**。这也是为什么 `PRUNE_IMAGES_AT` 与 `KEEP_IMAGE_RESULTS` 是一对常数而不是自适应策略——自适应的代价是更多次改写。

**架构推论：** `Compaction` 把「服务端压缩」与「客户端摘要」都实现成同一种能力接口的不同实例（同一个基类 `OpenAICompaction`、同一个 `overflow_only` 开关），于是「这个模型由谁压缩」退化成一次能力链的组装决策，而不是运行期的分支。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/agent/loop.py` | `_prune_images` | 批量裁剪旧截图，无变化返回 `None` |
| `browser_use/agent/loop.py` | `Compaction` | 客户端压缩：阈值判定、摘要、归档、检查点替换 |
| `browser_use/agent/loop.py` | `LazyCompaction` | 服务端压缩：仅在超过阈值时请求 `context_management` |
| `browser_use/agent/loop.py` | `_compact` | 摘要请求的构造与检查点的落地 |
| `browser_use/agent/loop.py` | `_estimate` | 每消息字符数/4、图片按 4800 的 token 估算 |
| `browser_use/agent/loop.py` | `_pi` | 把消息投影成 bu-pi 形态，使阈值可比 |
| `browser_use/agent/loop.py` | `redact_part` | 对 part 的 `content` 与 `args` 做形态替换 |
| `browser_use/agent/loop.py` | `_projected` | 「检查点 + 检查点之后的消息」的投影视图 |

## 参考

- [browser-use 源码：agent/loop.py](https://github.com/browser-use/browser-use/blob/main/browser_use/agent/loop.py)
- [OpenAI 文档：上下文管理（context management）](https://platform.openai.com/docs/guides/reasoning)
- [Pydantic AI 文档：消息与历史](https://ai.pydantic.dev/message-history/)
- [Anthropic 文档：提示缓存](https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching)

## 与主核 / AI 的关系

「改写历史即破坏缓存」这条约束在主核里对应的是「改写共享页即破坏只读副本」。主核的做法是写时复制、按页失效；这里的做法是「尽量不改、改就成批、改完保持稳定」。两者的共同点是**把「不改」当作默认，把「改」当作需要理由的例外**。

对 AI 工程的实际意义有三条。第一，**上下文压缩的正确性判据是「变小了」**。`_compact` 末尾那个断言看起来像保险，其实是一条设计约束：如果摘要不比原文短，压缩就毫无意义，而更糟的是它会**丢掉原始证据**。用断言把这条不变量写成可执行的形式，比依赖实现者的判断可靠。

**第二，估算模型必须与目标对齐**。`_pi` 花那么多代码去复刻 bu-pi 的消息形态，包括那些「看起来没用」的字段（`cost` 全 0、`api` 用 provider 名），原因是阈值是从 bu-pi 的实测里调出来的——如果按另一种形态算字符数，同一个阈值就不再有原来的含义。这是「移植一个策略时必须移植它的度量口径」的典型例子。

**第三，检查点文本的前几句应该专门防误读**。「这是参考不是指令」「变量仍然存在」「不要把摘要当验证」——这些句子不解自明地占用上下文，但每一条都对应一种真实的失败：模型把摘要当成新任务、模型重新建立已存在的状态、模型以为摘要就等于核对过事实。压缩做得好不好，一半取决于摘要质量，另一半取决于这段说明写没写清。
