---
project: browser-use
feature: delivery-guards
title: 投递校验与预算
page_type: feature
schema_version: 1
refs:
  - path: browser_use/agent/loop.py
    symbols: [Session, check_delivery, check_split_delivery]
  - path: browser_use/agent/loop.py
    symbols: [delivery_tool_choice, _sendable, last_text]
---

# 投递校验与预算

## 一句话职责

把「模型说完了」这件事变成一个**有验证的投递动作**：`finish` / `finish_from_repl` 两个输出工具之外，还有三道闸——一次性的投递内容校验（占位符、指向不存在文件的链接）、一次性的「分裂投递」拒绝（同一响应里在投递之后还有别的调用）、以及预算耗尽时把请求的工具选择强行收窄到只能投递。

## 机制是什么

1.0 里模型的主要工作是写 Python，而「结束」需要一次明确的投递。这带来三种失败模式，每一道闸对应一种：

| 失败模式 | 表现 | 闸门 |
|---|---|---|
| 交了空答案 | `finish("...")`、`finish({"authors": []})`，而真数据在 REPL 里 | `check_delivery` |
| 交了假证据 | 答案里链接到没写出来的文件 | `check_delivery` |
| 交了半句话 | 同一响应里先 `finish(success=False)` 打底，后面又是真的 `finish_from_repl` | `check_split_delivery` |
| 到点还没交 | 步数/时间耗尽，第二轮还去点浏览器 | `delivery_tool_choice` + 只投递轮 |

这三件事的共同点是：**它们都是「模型以为自己完成了」的情况**，而框架能观察到的事实（REPL 里有数据、工作区里没有那个文件、同一个响应里还有别的调用）足以判定它没完成。于是每次判定都以 `ModelRetry` 的形式把结果退回给模型，并附带一句具体的修复指引。

关键的节制是「一次性」：`Session` 里 `checked_delivery` 与 `split_delivery_step` 两个字段保证每轮只拒一次。否则一个总是交空答案的模型会被无限退回，把 `max_steps` 全烧在校验上。

## 怎么实现的

**`check_delivery` 检查两件事，都靠文本。**

```
s, text = ctx.deps, as_text(value)
if s.checked_delivery: return
problem = None
if not re.search(r'[^\W_]', text) or (not isinstance(value, str) and _holds_nothing(json.loads(text))):
    problem = (f'The result {text[:20]!r} is a placeholder, not the answer.', 'delivery_placeholder')
else:
    missing = sorted({ t for t in LINK.findall(text) + FILE.findall(text) if not re.match(r'^([a-z][\w+.-]*:|#|//)', t, re.I)
                       and not any((d / t.split('#')[0]).exists() for d in (s.repl.workspace, s.repl.workspace / 'downloads')) })
    if missing:
        problem = (f'These linked files do not exist in the workspace: {", ".join(missing[:10])}.', 'delivery_missing_files')
if problem is None: return
```

`as_text` 先把结果统一成文本（字符串原样，模型/数据类/list 走 `to_json`），然后做两件判断。

**第一件：占位符判定。** `re.search(r'[^\W_]', text)` 的含义是「有没有任何字母或数字」。`\W` 是非单词字符，`_` 是下划线（属单词字符但要排除），所以 `[^\W_]` = 「单词字符但不是下划线」= 字母/数字/其它 Unicode 字母。于是 `'...'`、`''`、空白、纯标点全部命中「没有值」；而注释特意点明「一个字符的答案像 `'7'` 是合法的」——一个数字就够。

结构化结果另有一条：`_holds_nothing(json.loads(text))`。`_holds_nothing` 是递归的，注释写着判据：「JSON 里没有值：空 list、空 object、空字符串、null。一个数字或布尔**是**值」。于是 `{"authors": []}` 被判空，而 `{"count": 0}` 不算空。注释给了这条例子的来源：「Sonnet 5.5 交了 `finish({"authors": []}, success=False)`，而 34 个作者都在 REPL 里」。

**第二件：链接判存。** 两个正则分工：`LINK` 抓 markdown 链接目标（`](...)` 形式，允许尖括号），`FILE` 抓反引号包起来的带扩展名的文件名（扩展名白名单是 `md|csv|json|jsonl|txt|tsv|xlsx|html|zip|pdf|png|jpe?g`）。

排除三类：带 scheme 的（`^[a-z][\w+.-]*:`，如 `https:`、`file:`）、锚点（`#`）、协议相对（`//`）。剩下的在**两个目录**里找工作区根与 `workspace/downloads`——注释：「下载落在 `workspace/downloads`；答案用文件名指它们」。查存时用 `t.split('#')[0]` 去掉片段。

**「最后一次请求」的特例是这一处最微妙的判断。**

```
what, cause = problem
if ctx.run_step > s.max_steps:
    logger.warning(printable(f'Delivered as it is on the last request: {redact(what, s.repl.secrets)}'))
    return
s.checked_delivery = True
s.rec.retry(cause)
raise ModelRetry(...)
```

docstring 说明了为什么：“不在 run 的最后一次请求上（投递轮之后的修复轮）：后面没有东西能接，被拒的结果就丢了（`status max_steps`、空字符串）。在那里结果照交，问题记日志。”`ctx.run_step > s.max_steps` 正是「已经越过 `max_steps`，这是额外的修复轮」。于是策略是：**能给修复机会时拒，不能给时收下并记录**。这比「无论如何都拒」务实得多——后者会让用户拿到空结果。

`ModelRetry` 的消息也是一份操作指引：

> Deliver the actual result: `finish_from_repl` with the existing records, or `finish` with the full answer inline. Do not repeat browser actions.

三句话：用哪个工具、怎么用、不要重新操作浏览器（避免修复轮变成又一轮探索）。

**`check_split_delivery` 处理的是「先打底再补真」这个具体行为。**

```
s = ctx.deps
if s.finalizing or s.split_delivery_step not in (None, ctx.run_step):
    return
response = next((m for m in reversed(ctx.messages) if isinstance(m, ModelResponse)), None)
calls = [p for p in response.parts if isinstance(p, ToolCallPart)] if response else []
at = next((i for i, p in enumerate(calls) if p.tool_call_id == ctx.tool_call_id), None)
if at is None: return
others = [p.tool_name for i, p in enumerate(calls) if i > at or (i < at and p.tool_name in DELIVERY_TOOLS)]
if not others: return
s.split_delivery_step = ctx.run_step
s.rec.retry('delivery_split')
raise ModelRetry(...)
```

判定逻辑值得逐条读。

`others` 的构成有两部分：**位于本次调用之后的所有调用**（`i > at`，不管是什么工具），加上**位于本次调用之前的投递类调用**（`i < at and p.tool_name in DELIVERY_TOOLS`）。第二部分的含义是「我前面已经交过一次了」——正是「先 `finish(success=False)` 打底」的情形。

而位于本次调用**之前**的非投递调用不算问题——注释：「一次投递之前的 python 调用仍然在同一个响应里投递」。也就是说 `python(...)` → `finish_from_repl(...)` 这个正常顺序是被允许的。

为什么会走到被拒？因为 Pydantic AI 的语义是「输出工具被调用即结束这一轮」。于是 `finish` 在前，后面那个真的 `finish_from_repl` 就不会被执行，run 带着桩结果结束。docstring 直说：「Sonnet 5.5 先发了一个桩 `finish(success=False)`，然后是真的 `finish_from_repl` 或一次 python 调用（客户任务里 3 次有 2 次）：run 带着桩完成了」。

**三个短路条件各有含义：**

- `s.finalizing`：在只投递轮上不检查——那一轮除了投递什么都不会跑，不存在「之后的调用」。
- `s.split_delivery_step not in (None, ctx.run_step)`：每轮只拒一次。用「步号」而不是布尔，是为了**同一轮里重试之后不再拒**（重试会在同一步内再来一次），但下一轮可以再拒。
- `at is None`：当前调用的 id 不在最后那条响应里（比如它在更早的响应里），放弃判定。

**`_sendable` 是投递之外的另一个「不崩」保证。**

```
def _sendable(result: Any) -> Any:
    try: tool_return_ta.dump_json(result)
    except (PydanticSerializationError, ValueError):
        try: return to_jsonable_python(result, fallback=str, bytes_mode='base64')
        except ValueError: return str(result)
    return result
```

三级降级：能直接序列化就原样返回；不能则转成可 JSON 化的形态（未知类型走 `str()`，字节走 base64）；连转换都失败（值包含自己，例如带父指针的对象）则退化成 `str(result)`。

注释给的理由：「一个它无法序列化的对象（SDK 响应、client 对象）曾以 `PydanticSerializationError` 结束 run；MIGRATING 承诺模型能看到任何值：它的 `str()`」。也就是说这是一个契约：**模型写的工具返回什么，都得能回到模型那里**，实在不行就给 `str()`。

注意 `ValueError` 被单独列出，注释：「值包含自己（一个父链接）」——`to_jsonable_python` 对循环引用抛的是 `ValueError` 而不是 `PydanticSerializationError`。

**`delivery_tool_choice` 决定「怎么强制模型只调投递工具」，而这件事在不同模型上做法不同。**

```
p = model.profile
effort = settings.get('openai_reasoning_effort')
thinks = (
    settings.get('thinking') not in (None, False)
    or (settings.get('anthropic_thinking') or {}).get('type') in ('enabled', 'adaptive')
    or effort not in (None, 'none')
    or p.get('thinking_always_enabled', False)
    or (p.get('thinking_enabled_by_default', False) and settings.get('thinking') is not False)
)
forcing = p.get('supports_forced_tool_choice', True) and not (
    thinks and (not p.get('supports_forced_tool_choice_with_thinking', True) or p.get('forced_tool_choice_disables_thinking', False))
)
return DELIVERY_TOOLS if forcing else UNFORCED
```

两个产物的形态不同：`DELIVERY_TOOLS` 是一个**工具名列表**（`['finish_from_repl', 'finish']`），而 `UNFORCED` 是 `ToolOrOutput(function_tools=[])`——「把 function tools 清空，让模型自己在投递工具里选」。

docstring 交代了缘由：「只在模型在**这一次请求**上接受强制工具选择时才强制（`allowed_tools` 会保留缓存的工具列表）；否则把 python 收起来、让选择保持 auto。Claude 在扩展思考下拒绝强制，并且会回答一次无思考的强制调用；Claude Opus 5.5 直接拒绝强制」。

`thinks` 那五个条件覆盖了不同 provider 表达「在思考」的方式：统一的 `thinking`、Anthropic 的 `anthropic_thinking.type`、OpenAI 的 `openai_reasoning_effort`、以及 profile 上的两个布尔（`thinking_always_enabled` / `thinking_enabled_by_default`）。这就是为什么它必须先探测「这个模型此刻是否在思考」，再决定能不能强制——强制与思考在同一请求上互斥。

而 `UNFORCED` 这条退路本身也有单独处理的理由，`on_model_request_error` 里：

```
if s.finalizing and not s.unforced and isinstance(error, ModelRequestRejectedError) and _rejects_tool_choice(error):
    s.unforced = True
    s.rec.retry('delivery_unforced')
    logger.warning(f'{error.model_name} rejected the forced delivery tool choice; asking again without forcing it')
    raise RetryModelRequest()
```

注释：「DeepSeek 与 Qwen 在思考下、gpt-oss 与 vLLM 会拒绝一个它们的 profile 没有标记的强制工具选择：run 曾经以错误结束且没有交付。用不强制的方式再问一次，python 收起来」。`_rejects_tool_choice` 只看错误文本里有没有 `tool_choice` / `tool choice`——一个粗糙但有效的识别，因为各家的报错措辞都包含这个参数名。

于是「profile 说不支持」与「profile 说支持但对方实际拒绝」两条路都通向 `UNFORCED`。`s.unforced` 是 sticky 的：一旦被拒过，本轮余下的投递请求都不再强制。

**`last_text` 是兜底答案的最后一环。**

```
def last_text(messages: list[ModelMessage]) -> str:
    for m in reversed(messages):
        if isinstance(m, ModelResponse):
            text = '\n'.join(p.content for p in m.parts if isinstance(p, TextPart)).strip()
            if text: return text
    return ''
```

它从后往前找**第一条有文本内容的响应**，把该响应里所有 `TextPart` 拼起来。空文本的响应被跳过而不是直接返回空串——这点很重要，因为一次「只有工具调用没有文本」的响应很常见，不跳过就会把兜底答案变成空。

`run()` 里调用它时还额外做了两件事：只取本次新增的消息（`new`），并且过一遍脱敏：

```
output = json.dumps(partial, ...) if partial is not None else redact(last_text(new), session.repl.secrets)
```

顺序也说明了优先级：检查点里的 `partial` 比「最后一段模型文本」更可信，因为它来自 `checkpoint(..., partial=True)` 的显式记录。

**`Session` 里与投递相关的字段构成一个完整的状态机。**

- `finalizing`：是否已进入只投递阶段。
- `checked_delivery`：投递内容校验是否已用掉。
- `split_delivery_step`：分裂投递被拒的步号。
- `success` / `finish_via`：模型给的判定与投递途径（`finish` / `finish_from_repl` / `text`）。
- `budget_hit`：`max_steps` / `timeout` / `browser_gone`——是什么逼出了只投递轮。
- `unforced`：强制工具选择被拒过。
- `finish_via` 的三值与 `budget_hit` 的三值，正好是「怎么交的」与「为什么只剩交付」两个维度的完整枚举。

## 关键取舍

**源码事实：** 投递校验与分裂投递拒绝都是**每轮一次**：`checked_delivery` 是布尔，`split_delivery_step` 用步号比较而非布尔，于是同一轮内的重试可以再交，下一轮可以再拒。

**源码事实：** 占位符判定用「有没有字母或数字」（`[^\W_]`），因此 `'7'` 合法、`'...'` 与 `''` 非法；结构化结果另有 `_holds_nothing` 递归判空，`{"authors": []}` 判空而 `{"count": 0}` 不判空。

**源码事实：** 文件存在性检查只认工作区根与 `workspace/downloads` 两处，且跳过带 scheme、锚点、协议相对的引用。

**源码事实：** 在 `ctx.run_step > s.max_steps`（即投递轮之后的修复轮）时不拒，只记 warning 并交付——「后面没有东西能接，被拒的结果就丢了」。

**源码事实：** 分裂投递的判定把「本次调用之后的任何调用」与「本次调用之前的投递类调用」都算作问题，但允许「之前的 python 调用」。

**源码事实：** `_sendable` 三级降级（原样 → 可 JSON 化 → `str()`），把「模型看到任何值」当成契约。

**源码事实：** `delivery_tool_choice` 先探测模型此刻是否在思考，再按 profile 决定是强制工具列表还是清空 function tools；profile 说谎时还有 `on_model_request_error` 的 `UNFORCED` 退路，且 `unforced` 一旦置位本轮不再回退。

**架构推论：** 三道闸的共同形态是「把框架能观察到的事实转成一条 `ModelRetry`」——不修改模型的结果，而是让模型自己修。这比框架代修更安全（框架不知道正确数据在哪），代价是每次拒绝都花掉一次模型请求。

**架构推论：** 每轮只拒一次的节制，是这类「校验 + 让模型重试」设计必须有的。没有它，一个系统性错误的模型会把全部步数浪费在同一条校验上；有了它，最坏情况是浪费一步，而正确的情况仍能得到修复。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/agent/loop.py` | `Session` | 投递状态机：`finalizing` / `checked_delivery` / `split_delivery_step` / `budget_hit` |
| `browser_use/agent/loop.py` | `check_delivery` | 占位符与悬空链接的一次性校验，最后一轮放行 |
| `browser_use/agent/loop.py` | `check_split_delivery` | 拒绝「投递之后还有调用」的分裂交付 |
| `browser_use/agent/loop.py` | `delivery_tool_choice` | 按模型是否在思考决定强制投递还是清空函数工具 |
| `browser_use/agent/loop.py` | `_sendable` | 工具结果的三级可序列化降级 |
| `browser_use/agent/loop.py` | `last_text` | 兜底答案：最近一条有文本的响应 |
| `browser_use/agent/loop.py` | `as_text` / `_holds_nothing` | 结果文本化与结构化「空值」判定 |

## 参考

- [browser-use 源码：agent/loop.py](https://github.com/browser-use/browser-use/blob/main/browser_use/agent/loop.py)
- [Pydantic AI 文档：输出与 ModelRetry](https://ai.pydantic.dev/output/)
- [Pydantic 文档：JSON 与严格模式](https://docs.pydantic.dev/latest/concepts/json/)
- [browser-use 仓库：MIGRATING.md](https://github.com/browser-use/browser-use/blob/main/MIGRATING.md)

## 与主核 / AI 的关系

「让调用方自己修」与主核里「把约束违反反射成一次具体错误而不是静默纠正」是同一种姿态。主核遇到一条违反约束的 INSERT 时不会替你改值，而是报一个带列名与约束名的错；这里的 `ModelRetry` 消息结构也一样：先说哪里错了（占位符 / 哪些文件不存在 / 同响应里还有什么），再说怎么改（用哪个工具），最后禁止一件事（不要重新操作浏览器）。

对 AI 工程的实际意义有三条。第一，**「每轮只拒一次」是这类机制能上线的必要条件**。任何一个「校验失败就让模型重试」的设计都必须回答「它会不会把预算吃光」，而这里的答案是用两个字段把它限制成每轮一次。

第二，**「不能修复时收下并记录」比「严格拒绝」更正确**。`ctx.run_step > s.max_steps` 那条分支是整个校验里最少见也最重要的一个：在最后一次请求上拒绝，等于把用户唯一能拿到的东西也丢了。把「拒绝」看成一种「有修复机会时才可用的手段」，而不是一条绝对规则，这一步判断决定了系统在压力下的表现。

第三，**「模型能看到的任何值」应该被当作契约维护**。`_sendable` 的三级降级看起来像防御性编程，实际是在兑现承诺：模型写 `tools` 返回一个 SDK 对象，那是合理的用法，框架不能因此让整个 run 崩掉。凡是把模型写的代码的返回值再喂回模型的地方，都需要这一层。
