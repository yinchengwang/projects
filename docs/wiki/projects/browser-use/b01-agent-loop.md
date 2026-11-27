---
project: browser-use
feature: agent-loop
title: 循环骨架与 python 工具
page_type: feature
schema_version: 1
refs:
  - path: browser_use/agent/loop.py
    symbols: [BrowserLoop, python, run]
  - path: browser_use/agent/loop.py
    symbols: [build_agent, model_kind]
---

# 循环骨架与 python 工具

## 一句话职责

`loop.py` 用一个 `Agent[Session, Any]` 承担整个 agent 循环，而它只注册了一个真正的工具——`python`；`BrowserLoop` 作为能力（capability）挂在这个 agent 上，负责在每次模型请求前后做预算判定、图像裁剪、错误重试与流式改造。

## 机制是什么

先把 1.0 与 0.x 的差别说清，因为它决定了整页的结构。0.x 的循环是「每步把一份精简 DOM 喂给模型，模型返回一个结构化动作（`click(index=5)`），再由框架执行」。1.0 里这两步都消失了：

- **观测**：模型自己写 `print(...)` 来决定看什么。
- **动作**：模型自己写 Python，在常驻 REPL 里直接调 `browser-harness` 的辅助函数或裸 CDP。

于是「工具」只剩一个 `python(code: str)`，而原来那些能力（点击、输入、截图、读取页面）全部下移成了「REPL 里可用的 Python 函数」。这不是把工具改了个名字，而是**把动作空间从受限的 JSON schema 换成了完整的 Python**。

循环因此退化成两件正交的事：

1. **模型请求循环**——由 Pydantic AI 提供，`BrowserLoop` 通过能力钩子往里插预算判定、裁剪、重试。
2. **单元格执行**——由 `python` 工具负责，它把代码交给常驻 REPL 并等结果。

`run()` 是这两者的外部包装，负责把「Pydantic AI 抛出的各种结局」翻译成统一的 `AgentHistoryList`（`completed` / `timeout` / `max_steps` / `error`），并且**永不因 agent 或 provider 的失败而抛异常**。

`Session` 是整轮运行的依赖容器（Pydantic AI 的 deps）：预算、消息、检查点、统计、各类计数器都在里面。

## 怎么实现的

**`Session` 的关键字段决定循环的形状。**

- 预算：`timeout_s`（默认 1700）、`max_steps`（默认 1000）、`cell_timeout_s`（120）、`started`。
- 交付状态：`finalizing`（是否进入只投递阶段）、`checked_delivery`（投递校验只触发一次）、`split_delivery_step`、`success`、`finish_via`、`budget_hit`。
- 检查点：`checkpoint`、`covered`、`usage_after`、`overflowed_step`。
- 其它：`provider_retried`（每轮只重试一次 provider 错误）、`unforced`（强制工具选择被模型拒绝过）、`request_params`（最后一次请求的工具定义，判分时要复用以便命中提示缓存）、`summaries`（摘要响应，计入用量与花费）。

两个方法把「预算」写成了时间的函数：

```
def delivery_at(self) -> float:
    return self.timeout_s - max(0.1 * self.timeout_s, min(30.0, 0.5 * self.timeout_s))

def time_left(self) -> float:
    return (self.timeout_s if self.finalizing else self.delivery_at()) - (time.monotonic() - self.started)
```

`delivery_at()` 的 docstring 解释了这个不显然的公式：「只投递的那一轮在预算的 90% 处开始，或者在短预算时更早开始，以便投递请求有最多 30 秒（`timeout=25` 只留了 2.5 秒：run 空手结束）」。也就是取「10% 预算」与「最多 30 秒」的较大者作为保留，且不超过预算的一半。`time_left()` 则区分两个阶段：未进入投递时算到投递点，进入投递后算到真正结束。

`partial()` 从工作区的 `partial.json` 读回检查点值并脱敏，且把所有失败都吃掉：「恢复数据永不该把一次超时变成一次崩溃」。

**`python` 工具是这一页的核心。**

```
async def python(ctx: RunContext[Session], code: str) -> list[Any]:
    """Run Python in the persistent REPL (browser-harness helpers and raw cdp() pre-imported).

    Browser helpers are synchronous (do not await them); variables survive calls; the last expression is printed.
    Cell deadline applies; checkpoint small batches before it. screenshot() returns native images.
    """
```

它的实现有七个动作，每个都对应急需处理的一种边界：

1. **预算检查前置**：`left = ctx.deps.delivery_at() - (started - ctx.deps.started)`，`if left <= 0: return [NO_TIME]`。注释：「同一个响应里的后续调用，在先前那次用光时间之后」——即一个响应里可以有多个 python 调用，后面的会看到时间已尽，于是返回一句提示文本而不是执行。
2. **计时与统计**：`ctx.deps.rec.cell_start()`、`starts = getattr(repl, 'start_ns', 0)`（注释：「测试替身没有这个」）。
3. **超时的双重上限**：`min(ctx.deps.cell_timeout_s + grace, max(left, 1.0))`。`grace = 10 if mode == 'ultrafast' else 0`，注释解释：「ultrafast 在单元格代码之后打印页面状态，所以它的单元格多给 10 秒，从剩余时间里扣——加在剩余时间之上，会让一次短 `run(timeout=...)` 丢掉投递轮（status timeout，没有答案）」。
4. **执行并记账**：`r = await repl.call('exec', code, ...)`，然后 `stats.add_cell`、`rec.phase('repl_start', ...)`、`rec.cell(...)`。
5. **浏览器丢失的判定**：`if r.get('browser_gone'): ctx.deps.budget_hit = 'browser_gone'`。注释：「一个没有浏览器的模型离开后自己去找另一个浏览器（用户自己的 Chrome）」——这把一种「跑偏」提升为一个预算事件，从而触发投递轮。
6. **输出整形**：文本与错误拼接（`f'{text}\nError: {r["error"]}'.strip()`）；日志行用代码的第一条非空行（脱敏后截 120 字符），错误行取最后一行（截 200）。
7. **图像处理**：`vision=False` 时返回一句「截图未显示」，否则把 base64 解成 `BinaryContent` 一起返回。

返回值是 `list[Any]`：第一项是文本（空输出也填 `'(no output)'`），后面是可选的图片。也就是说**模型看到的是一个混合内容列表**，文本与图像平级——这是 1.0 的一个实质变化，0.x 的「截图作为单独字段」不见了。

有一处安全细节藏在日志里：

```
# the page controls the error text (a JS exception's message), the model the code: no terminal escapes from either
logger.info(printable(f'step {ctx.run_step}: {line[:120]}' + (f' -> {error[-1][:200]}' if error else '')))
```

即：错误文本由页面控制、代码由模型控制，两者都不可信，所以过一遍 `printable()` 去掉终端转义序列。否则一段恶意 JS 的报错就能向日志终端注入控制字符。

**`run()` 把「各种失败」收敛成四种状态。**

```
status, output, error = 'completed', None, None
(session.repl.state / 'partial.json').unlink(missing_ok=True)   # 上一次的 partial 不属于这次
budget = asyncio.timeout(session.timeout_s)
task = redact(task, session.repl.secrets)
recording = CURRENT.set(session.rec)
try:
    async with budget:
        result = await agent.run(task, deps=session, message_history=message_history,
                                 usage_limits=UsageLimits(request_limit=session.max_steps + 1))
    output, messages = result.output, result.all_messages()
except TimeoutError as e:
    if budget.expired(): status, messages = 'timeout', session.messages
    else: status, messages = 'error', session.messages; error = ...
except Exception as e:
    name = session.error_type = type(e).__name__
    status = 'max_steps' if name == 'UsageLimitExceeded' else 'error'
    ...
```

三处值得单说。

**第一，`TimeoutError` 有两种，靠 `budget.expired()` 区分。** 注释：「一个工具自己的 socket/HTTP 超时是 error，不是 run 预算」。即外层 `asyncio.timeout` 到点才算 `timeout`，工具内部的超时算 `error`。如果只按异常类型分，两种都会被误判成 `timeout`，而它们对用户的含义完全不同（一个是「时间不够」，一个是「某个请求挂了」）。

**第二，`request_limit=max_steps + 1`。** 多出来的那一，就是「只投递的一轮」——`max_steps` 计的是模型请求数，而投递轮是额外的。

**第三，`CURRENT.set(session.rec)` 与 `CURRENT.reset(recording)`。** 这是一个 ContextVar，让 `StreamedRequest`（在 b04 页）能拿到本轮 recorder；注释：「这次任务的心跳：`StreamedRequest` 通过它够到 recorder」。

**部分结果与兜底答案的优先级。**

```
partial = session.partial()
new = messages[len(message_history or []) :]
if output is None and status != 'completed':
    output = json.dumps(partial, ensure_ascii=False) if partial is not None else redact(last_text(new), session.repl.secrets)
```

三档优先级：**已投递的 output** > **检查点里的 partial** > **最后一段模型文本**。注释点明了两个细节：`new` 只取本次新增的消息（「上一轮任务的文本永不做这一轮的兜底」）；兜底文本要脱敏（「模型可能从截图里读到了密钥并引用出来」）。

而 `if output is None and status != 'completed'` 这个条件也有讲究：注释说「一次 completed 的 run 已经投递了它：一个可空 schema 返回的 None 就是答案」。即不能用 `output is None` 判断「没投递」——`None` 可能是合法结果。

**用量与花费的分账。**

```
usage, cost = usage_and_cost(new + session.summaries, model_ref)
if search:
    session.rec.cost_search = sum(search.charges[searched:])
if session.summaries:
    session.rec.cost_compaction = usage_and_cost(list(session.summaries), model_ref)[1]
if cost is not None and search:
    cost += sum(search.charges[searched:])
```

三层：主循环的用量与花费（含摘要响应）、搜索的单独记账、摘要的单独记账（用于遥测分项），最后把搜索花费并进总花费。注意 `usage_and_cost` 收的是 `new + summaries`——摘要请求也是这个 run 花的钱。

**最后一步：脱敏是在返回前对整份消息做的。**

```
forms = secret_forms(session.repl.secrets)
if forms:
    messages = [replace(m, parts=[redact_part(p, forms) for p in m.parts]) for m in messages]
return AgentHistoryList(status, text, output, steps, duration, usage, cost, partial, error, messages, success)
```

注释解释了为什么连历史也要改：「模型文本、思考与工具调用参数可能引用从截图上读到的密钥，而 browser-use 会把它们回引到请求里：投递修复轮引用了那段回复，schema 错误会回显被拒的输入」。但注意顺序：`session.messages = messages` 在脱敏**之前**赋值——所以续跑用的原文是未脱敏的（「提供方会拒绝被改过的签名思考块」），而对外的 `AgentHistoryList` 用脱敏版。

**`build_agent` 按「模型种类」走三条分支。**

```
kind = model_kind(model)   # 'responses' | 'chat' | 'other'
```

`model_kind` 的判定同时接受实例与字符串：

```
if isinstance(model, OpenAIResponsesModel) or (isinstance(model, str) and model.startswith(('openai:', 'openai-responses:'))):
    return 'responses'
if isinstance(model, OpenAIChatModel) or (isinstance(model, str) and model.startswith('openai-chat:')):
    return 'chat'
return 'other'
```

三条分支的差异不是风格，而是「哪些设置会被对方 400」：

- **`responses`**：全套 OpenAI 设置（`openai_store=False`、`openai_reasoning_effort`、`openai_reasoning_summary='auto'`、`openai_reasoning_context='auto'`、`max_tokens`），能力链是 `[BrowserLoop(), LazyCompaction(...), StreamResponses(), Compaction(overflow_only=True), Recording()]`。`openai_store=False` 的理由注释得很细：「一次存储的响应回来晚了 170–320 毫秒（首字节时间，39k–83k tokens）。没有任何东西会读回它」。
- **`chat`**：只有必要的一个设置（OpenAI 自家推理模型在 chat 上要 `reasoning_effort='none'`，否则「400: use /v1/responses or set reasoning_effort to 'none'」），能力链是 `[BrowserLoop(), Compaction(...), Recording()]`——**没有 `StreamResponses`**，因为那个只对 Responses 模型有意义。
- **`other`**：`thinking` 统一设置、Google 补 `max_retries=2`（注释：「google-genai 不主动重试；OpenAI 与 Anthropic 的 SDK 各自重试两次。Gemini 在一轮里第二次 503 'overloaded' 就结束了运行」）、Claude 要 `anthropic_thinking={'type': 'adaptive', 'display': 'summarized'}`（注释：「不开的话每个 thinking 块都空着回来（Haiku 上是 124/124），历史缺少 bu-pi 计入上下文的推理，于是 Haiku 从不压缩」）。

能力链的顺序是有意义的，`Recording` 的 docstring 直接说明了为什么它排最后：「最后一次 `prepare` 钩子在压缩之后跑（分开计时），而它的错误钩子最先看到每次失败，就像提供方抛出的那样」。

**`Tool(python, max_retries=TOOL_RETRIES, sequential=True)` 里的 `sequential`。**

注释解释了为什么必须串行：「同一个响应里的并行 python 调用会一个接一个跑，每个从自己的开始计时（bu-pi 的 `executionMode 'sequential'`）。在调用处计时的话，一个排队的单元格会拿着自己过期的预算等 REPL，于是跑过了投递点」——后面直接给了事故编号。

**`retries` 的两个预算各有事故记录。**

```
retries={'tools': TOOL_RETRIES, 'output': TOOL_RETRIES},
```

注释里 `'tools'` 那条：「Pydantic AI 给不存在的工具名一个预算，而那个名字的计数永不复位：两次这样的调用就在第 140 步结束了 DeepSeek 的一次运行。bu-pi 回答『Tool X not found』然后继续（`max_steps` 会兜住）」。`'output'` 那条：「六次失败的投递分散在一次长 run 里（一次表达式错误、一次 schema 偏差、一次只有思考的回复、一次被拒的分裂投递算两次）就以『Exceeded maximum output retries (5)』结束了。bu-pi 没有这个上限；`max_steps` 与超时才是边界」。

这两条注释的价值在于它们说清了「为什么这个数字是 `TOOL_RETRIES` 而不是 Pydantic AI 的默认」——默认值在长 run 里会把「偶发的失败」累积成「致命」。

**`@agent.instructions` 动态追加工作区说明。**

```
@agent.instructions
def workspace(ctx: RunContext[Session]) -> str:
    text = (f'Workspace directory (JSON string): {json.dumps(str(ctx.deps.repl.workspace.resolve()))}. ...')
    return f'{text}\n\n{secrets_prompt(ctx.deps.repl.secrets)}' if ctx.deps.repl.secrets else text
```

注释给了来源（bu-pi 的 `agent.ts` 工作区指令）与动机：「V2 的交付物跑到了工作区外面」。指令里三句话都是行为约束：工作区路径用 JSON 字符串（避免 Windows 反斜杠被当成转义）、REPL 的 cwd 从这里开始、交付物必须放在这个目录内（「外面的文件不会被交付」）、用相对路径或精确的工作区值而不是猜一个父目录。

`json.dumps` 包路径这个细节值得注意：路径里的反斜杠如果直接拼进提示，模型可能在字符串字面量里写 `C:\Users\...` 而不是 `C:\\Users\\...`。

## 关键取舍

**源码事实：** 1.0 只注册一个工具 `python`；点击/输入/截图等都下移成 REPL 里的 Python 函数。观测由模型自己 `print`，动作由模型自己写代码。

**源码事实：** `python` 返回 `list[Any]`（文本 + 可选图片），不是结构化动作；`vision=False` 时图片被替换成一句说明。

**源码事实：** `python` 的超时取 `min(cell_timeout_s + grace, max(left, 1.0))`，ultrafast 多给 10 秒且**从剩余时间里扣**，否则短预算会丢掉投递轮。

**源码事实：** `run()` 用 `budget.expired()` 区分「run 预算到点」与「工具自身超时」，前者是 `timeout`，后者是 `error`。

**源码事实：** `UsageLimits(request_limit=max_steps + 1)`——多出的一就是投递轮。

**源码事实：** 兜底答案的优先级是「已投递的 output > 检查点 partial > 最后一段模型文本」，且判据用 `status != 'completed'` 而非 `output is None`。

**源码事实：** `retries={'tools': ..., 'output': ...}` 覆盖 Pydantic AI 默认，两处注释各自记录了被默认值坑掉的事故。

**源码事实：** `Tool(python, sequential=True)` 是必需的，否则排队的单元格会拿着过期预算跑过投递点。

**架构推论：** 把「动作空间」从 JSON schema 换成 Python，收益是表达力（模型能循环、能解析、能组合），代价是**放弃了静态可校验的动作集合**。`d01` 那层（工具注册）因此从「定义动作」退化为「定义额外能力」，而安全检查必须靠环境与回流限制（c03）而不是类型。

**架构推论：** 三条模型分支的存在说明「一次 agent 循环的可移植性」不是靠抽象层，而是靠**逐 provider 的例外表**。`build_agent` 里每一条 `if` 都对应一个具体的 400 或一次具体的退化，注释里带着事故编号——这类代码不可能靠设计消除，只能靠记录维护。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/agent/loop.py` | `Session` | 单轮运行的依赖容器：预算、检查点、交付状态、统计 |
| `browser_use/agent/loop.py` | `Session.delivery_at` | 投递轮的起始时刻（90% 预算，保留至多 30 秒） |
| `browser_use/agent/loop.py` | `python` | 唯一的真工具：预算检查、单元格执行、输出与图像整形 |
| `browser_use/agent/loop.py` | `BrowserLoop` | 能力：请求前预算判定、图像裁剪、错误重试、流式改造 |
| `browser_use/agent/loop.py` | `run` | 把各种结局收敛成四种状态并组装 `AgentHistoryList` |
| `browser_use/agent/loop.py` | `build_agent` | 按模型种类装配设置、能力链、重试预算与工具 |
| `browser_use/agent/loop.py` | `model_kind` | 模型三分法：responses / chat / other |

## 参考

- [browser-use 源码：agent/loop.py](https://github.com/browser-use/browser-use/blob/main/browser_use/agent/loop.py)
- [browser-use 文档：架构说明](https://docs.browser-use.com/)
- [Pydantic AI 文档：能力（capabilities）](https://ai.pydantic.dev/capabilities/)
- [Pydantic AI 文档：工具与输出](https://ai.pydantic.dev/tools/)

## 与主核 / AI 的关系

「一个工具跑任意代码」与主核里「一个执行入口跑任意计划」是同一种收敛：把动作空间从「枚举」变成「语言」，于是框架不需要为每个新动作加代码。主核早就这么做（SQL 是语言，不是动作枚举），差别在于主核的执行语言是**受限的**（DDL/DML 是封闭语法），而这里是完整的 Python。

对 AI 工程的实际意义有三条。第一，**预算必须作用在「同一个响应里的多次调用」上**。`python` 里那句 `if left <= 0: return [NO_TIME]` 处理的正是这个场景：模型可以在一次响应里连发多个 python 调用，如果每个都从头算时间，整轮就会超出预算。多智能体/多工具系统里同样的问题普遍存在，而处理方式通常是「先算总预算，再在每次调用前复查」。

第二，**「超时」与「错误」必须分开**。`budget.expired()` 这个判断看起来多余，但它决定了用户拿到的状态是 `timeout`（可重试、加大预算）还是 `error`（该修代码）。用一个异常类型承载两种语义，然后靠一个布尔去分辨，是因为上游（`asyncio.timeout`）与下游（工具的 socket 超时）抛的是同一个类。

第三，**默认重试预算在长任务里是危险的**。`'output': TOOL_RETRIES` 那段注释列了六次「合法的、偶发的」投递失败如何耗尽 Pydantic AI 默认的 5 次上限。任何「每个 run 全局共享一个重试预算」的设计，都要问一句：这个预算是为短任务定的，还是为长任务定的？
