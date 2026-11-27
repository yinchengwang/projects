---
project: browser-use
feature: streaming-recording
title: 响应流改造与请求录制
page_type: feature
schema_version: 1
refs:
  - path: browser_use/agent/loop.py
    symbols: [StreamResponses, StreamedRequest, _cut_after_response]
  - path: browser_use/agent/loop.py
    symbols: [Recording, thinking_and_text, usage_and_cost]
---

# 响应流改造与请求录制

## 一句话职责

`StreamedRequest` 把每个 OpenAI Responses 请求偷偷改成流式、再把事件流装配回一个「非流式」的响应返回，`StreamResponses` 是决定给哪个模型套这层包装的能力；`Recording` 把每次模型请求尝试记进 `RunRecorder`，`usage_and_cost` 把消息换算成用量与美元——两者共同构成这一层的可观测面。

## 机制是什么

先看这层的核心矛盾：**循环想要非流式的语义，而传输必须是流式。**

- 语义上，循环只关心「一次请求 → 一个完整响应」：有 `parts`、有 `usage`、有 `finish_reason`。它不该处理「半个响应」。
- 传输上，Browser Use 网关会在**非流式 Responses 请求 300 秒处切断**，返回 `HTTP 200 status=failed request_timeout`。一次长的 bu-smart 轮次（常常正是投递那一轮）因此结束。

`StreamedRequest` 的解法是 `WrapperModel`：对外 `request()` 返回 `ModelResponse`，内部却跑 `request_stream()` 把事件流消费完。于是**上层看到的是一次普通请求**，而所有钩子（重试、录制、错误分类）都按非流式路径工作——这一点在 docstring 里被专门强调：「钩子仍然看到一个非流式请求，所以流里任何一点的错误都会被重试」。

三个部件分工：

| 部件 | 层 | 职责 |
|---|---|---|
| `StreamResponses` | 能力（capability） | 只在 `OpenAIResponsesModel` 上挂 `StreamedRequest` |
| `StreamedRequest` | 模型包装（`WrapperModel`） | 流式执行、装配响应、剥掉不可读的 compaction/reasoning item |
| `Recording` | 能力 | 每次尝试的开始/成功/失败记进 recorder |

`usage_and_cost` 与 `thinking_and_text` 是两个纯函数：前者把消息列表换算成用量与花费，后者把一条响应拆成「思考片段」与「文本片段」的序列。

## 怎么实现的

**`StreamResponses` 只有一个方法。**

```
class StreamResponses(AbstractCapability[Session]):
    def get_wrapper_model(self, model: Model) -> Model | None:
        return StreamedRequest(model) if isinstance(model, OpenAIResponsesModel) else None
```

能力层的职责就这么多——判断「要不要包」。docstring 把动机写全了：「每个 OpenAI Responses 请求（网关、api.openai.com）都以 `stream=true` 出去，并以装配好的响应回来，就像 bu-pi 对每个请求都流式。网关会在 300 秒处切断非流式请求：它回了 `HTTP 200 status=failed request_timeout`，一次长的 bu-smart 轮次（往往是投递）就此结束了运行。用量、网关的计费（`browser_use` 于 `response.completed`）、推理与压缩项都来自终态事件，与来自响应体一样。」

最后一句是这层能成立的技术前提：**终态事件携带的信息量与响应体一致**，包括用量、计费和 compaction item，所以「装配回来的响应」不是残次品。

**`StreamedRequest.request` 外层是一个针对 compaction item 的重试循环。**

```
guessed = False
while True:
    try:
        return await self._request(messages, model_settings, model_request_parameters)
    except ModelAPIError as e:
        if not (item := _unreadable_compaction(e, messages, guessed)):
            raise
        guessed = guessed or e.provider_error_code != 'invalid_encrypted_content'
        if rec := CURRENT.get():
            rec.retry('compaction_unreadable')
        what = 'reasoning item' if item.startswith('rs_') else 'compaction item'
        logger.warning(f'{self.model_name} could not read {what} {item}; sending the request without it')
        if item.startswith('rs_'):
            for m in messages:
                if isinstance(m, ModelResponse): m.parts = _without_part(m, item).parts
        else:
            messages = [_without_part(m, item) if isinstance(m, ModelResponse) else m for m in messages]
```

这解决的是「加密内容跨请求不可读」这一类故障。docstring 给的实例：「bub2-010（gpt-6-luna，5 次流式压缩里 1 次）：下一个请求在 compaction item 上得到 400 `invalid_encrypted_content`，运行结束。消息里仍然持有那个项所替换的历史，所以请求再发一次、不带它，服务端重新压缩那段历史；频率不超过此类项的个数，对网关的 400 则只试一次（它不说明是哪个项）。一个 OpenAI 这样命名的 reasoning item（线上，改了一个字符：运行结束）以同样方式被去掉：请求丢掉那一轮的推理，而不是整个运行。」

两种剥除方式的**副作用不同**，这是这段代码最精细的地方：

- **reasoning item（`rs_` 前缀）就地改**：`m.parts = _without_part(m, item).parts`——直接改运行时的历史对象，因为「每一次后续请求都会发送它」。
- **compaction item 只改本次请求的列表**：`messages = [...]`——因为历史要保留它，「下一个 compaction item 会取代它」。

而 `guessed` 变量处理的是网关的一个具体缺失：「网关把每个 provider 400 都答复成 `invalid_request`，既不给 code 也不给 message（cloud llmgateway `publicError`），所以在那里只能猜最新的那一项——而请求只携带它一项，且只猜一次」。`_unreadable_compaction` 里对应的两条分支：

```
if error.provider_error_code == 'invalid_encrypted_content':
    ids = re.findall(r'\b(?:cmp|rs)_\w+', error.body ...)
    thoughts = {p.id for m in messages if isinstance(m, ModelResponse) for p in m.parts if isinstance(p, ThinkingPart)}
    return next((i for i in ids if i in sent or i in thoughts), None)
if error.provider_error_code == 'invalid_request' and getattr(error, 'status_code', None) == 400 and sent and not guessed:
    return sent[-1]
return None
```

第一条从错误体里正则挖出 `cmp_*` / `rs_*` 形态的 id，并且要求它**要么在发出去的 compaction 项里、要么在历史里的思考项里**——一个凭空出现在错误消息里的 id 不会被采信。第二条就是那个「猜最新一项」的降级路径。

**`_request` 里对「首事件超时」的自我实现是必要的。**

```
wait = (model_settings or {}).get('first_event_timeout')
async with AsyncExitStack() as stack:
    first_event = asyncio.timeout(wait)
    try:
        async with first_event:
            stream = await stack.enter_async_context(
                self.wrapped.request_stream(messages, model_settings, model_request_parameters))
            if handle := getattr(stream, '_handle_terminal_response', None):
                setattr(stream, '_handle_terminal_response', lambda r: (terminal.append(r), handle(r))[1])
            events = _in_stream(stream)
            stack.push_async_callback(events.aclose)
            await anext(events, None)
            if rec := CURRENT.get(): rec.first_event()
    except TimeoutError:
        if not first_event.expired(): raise
        raise ModelRequestTimeout(self.model_name, cast(float, wait), setting='first_event_timeout') from None
    async for _ in events: pass
```

注释点明前提：「`first_event_timeout`（`_deadlines` 里设的）在这里强制执行：agent 看到的是一次非流式请求，所以 Pydantic AI 会跳过它」。也就是说这个超时本来该由框架管，但因为上层以为是非流式请求，框架不会管，只能自己来。

三个细节：`asyncio.timeout` 的上下文包住了**打开流**这一步（注释：「打开流也算，和 Pydantic AI 一致」）；`TimeoutError` 要判 `first_event.expired()`——不是这个作用域超时的 `TimeoutError` 原样重抛；`await anext(events, None)` 只拉第一个事件即退出作用域，并在那一刻 `rec.first_event()` 记下首字节延迟。

**终态事件的拦截是用属性替换做的。**

```
if handle := getattr(stream, '_handle_terminal_response', None):
    setattr(stream, '_handle_terminal_response', lambda r: (terminal.append(r), handle(r))[1])
```

即：如果底层流对象有 `_handle_terminal_response` 这个私有钩子，就把「先把终态响应存进 `terminal`、再调原钩子」的 lambda 换上去。`(x, y)[1]` 的写法是为了在一个 lambda 里先做副作用再返回原调用的结果。

这是一个**依赖私有属性**的做法，注释也承认了这一点（没有断言它一定存在，`getattr` 带默认值）。它之所以值得，是因为终态事件里的 `output` 才是 compaction item 的权威版本——见下一步。

**流结束后两道检查：一个是完整性，一个是修正。**

```
response = stream.get()
if response.state != 'complete':
    raise ModelConnectionError(self.model_name, 'The response stream ended before the response did.', in_stream=True)
return _sealed_as_completed(response, terminal[-1]) if terminal else response
```

第一道的理由写在注释里：「一个在终态事件（`response.completed`、`.incomplete`、`.failed`）之前就关闭的流，等同于一个提前结束的响应体：半个答案被当成结果、半个摘要被当成检查点、没有用量也没有计费」。三种后果都指向同一件事——**半个响应比一个错误更危险**，因为它看起来合法。

第二道 `_sealed_as_completed` 解决的是 compaction item 的双版本问题：

```
items = {o.id: o for o in getattr(final, 'output', None) or () if getattr(o, 'type', None) == 'compaction'}
if not items: return response
parts = [replace(p, provider_details=items.pop(p.id).model_dump()) if isinstance(p, CompactionPart) and p.id in items else p
         for p in response.parts]
missing = [CompactionPart(None, id=i.id, provider_name=response.provider_name, provider_details=i.model_dump()) for i in items.values()]
return replace(response, parts=[*missing, *parts])
```

docstring 说明了为什么必须替换：「compaction item 自己的事件可能携带另一个 `encrypted_content`（`output_item.added`：一个临时的 1016 字符 token，线上 gpt-6-luna），只有响应里的那个才是要回传的。事件里从未完成的那一项排在最前面，那里正是服务端放它的位置」。

于是做了两件事：已在响应里的 compaction part **换掉 `provider_details`**（用终态事件的版本）；终态里有但响应里没有的 **补到最前面**（位置对齐服务端）。`missing` 那个列表的名字是诚实的——它们是「流事件没走完、只出现在终态里」的项。

**`_in_stream` 只做一件事：给失败打上「发生在流内」的标记。**

```
async def _in_stream(stream: AsyncIterable[Any]) -> AsyncGenerator[Any]:
    try:
        async for event in stream: yield event
    except ModelAPIError as e:  # the SDK retries a request until its stream opens, never a stream that broke off
        e.in_stream = True
        raise
```

注释给了理由：「SDK 会重试一个请求直到它的流打开，但从不重试一个中途断掉的流」。这个标记在 `on_model_request_error` 里被用来判断「SDK 有没有替我们重试过」：

```
in_200 = error.in_stream or (failed and getattr(error, 'status_code', None) is None) or _cut_after_response(error, model)
sdk_retried = not in_200 and (retries or (model.system == 'xai' and code == 'UNAVAILABLE'))
```

即「流内的失败一律当作『SDK 没重试过』」，于是本层的一次性重试会接手。

**`_cut_after_response` 是最精细的一个判定。**

```
def _cut_after_response(error: ModelAPIError, model: Model) -> bool:
    cause = error.__cause__
    if not isinstance(error, ModelConnectionError) or not isinstance(cause, _HTTP_ERRORS):
        return False
    return not (model.system.startswith('google') and isinstance(cause, _HTTP_RETRIED))
```

docstring：「一个在读取响应时断掉的连接：没有 SDK 重试过它。OpenAI 与 Anthropic 的 SDK 把它们会重试的东西包在自己的 `APIConnectionError` 里；一个裸的 httpx 错误来自 SDK 已经返回的响应（Claude 的流：线上的『peer closed connection without sending complete message body』在第一次尝试就结束了运行）。google-genai 从不包装它们，且只重试连接错误与超时（一个被切断的 JSON body，线上，也是）」。

三句话分别对应三条不同的事实：

1. **异常链的形态**区分「SDK 会重试的」与「SDK 已放弃的」：前者被 SDK 包成自己的 `APIConnectionError`（于是 `__cause__` 不是 httpx 错误类型），后者是裸的 httpx 错误。
2. **Google 例外**：它不包装，所以裸 httpx 错误不能一律判成「没重试过」；但它只重试 `ConnectError` 与 `TimeoutException`，所以这两类要排除掉（`_HTTP_RETRIED`）。
3. 判定的用途是让本层接手重试——即「分清谁已经试过了」。

`_HTTPX = [httpx2, *([httpx] if httpx else [])]` 那几行也是同一个思路的延伸：Pydantic AI 的 provider 用 `httpx2`，而调用方自己的客户端可能是 `httpx`，两者的异常类型不同，所以两组都要认。

**`Recording` 是三个钩子加一句排序理由。**

```
class Recording(AbstractCapability[Session]):
    async def prepare_model_request(self, ctx, request_context):
        ctx.deps.rec.attempt_start()
        return request_context
    async def after_model_request(self, ctx, *, request_context, response):
        ctx.deps.rec.attempt_ok(response, ctx.deps.finalizing)
        return response
    async def on_model_request_error(self, ctx, *, request_context, error):
        ctx.deps.rec.attempt_failed(error)
        raise error
```

docstring 交代了它在能力列表里的位置为什么是最后：「最后一次 `prepare` 钩子在压缩之后跑（分开计时），而它的错误钩子最先看到每次失败，就像提供方抛出的那样」。

这句话包含两条推理：**`prepare` 钩子按注册顺序执行**，所以排在最后意味着它在 `Compaction` 的 `prepare` 之后——压缩耗时会单独记在 `Compaction` 的 `rec.compaction(...)` 里，不会被算成模型请求时间。而**错误钩子按相反顺序**（最先注册的最后看到），所以 `Recording` 排最后就是在最外层——它看到的是原始异常，而 `BrowserLoop` 的错误钩子会决定要不要改成重试。

`rec.attempt_ok(response, finalizing)` 带 `finalizing` 是因为 recorder 要在步骤上打标记（`EV_DELIVERY`），于是「哪些步骤属于只投递轮」在遥测里是可见的。`attempt_failed` 则记录「第几步、耗时、异常类型、provider 错误码」——注意记的是**类型**而不是消息，这与整个遥测的「不发送文本」原则一致。

**`usage_and_cost` 的三层优先级。**

```
def usage_and_cost(messages, model_ref) -> tuple[dict[str, int], float | None]:
    total = {'input': 0, 'output': 0, 'cache_read': 0, 'cache_write': 0, 'requests': 0}
    cost: float | None = 0.0
    provider = 'openai'
    if ':' in model_ref:
        provider, model_ref = model_ref.split(':', 1)
        provider = provider.removesuffix('-responses').removesuffix('-chat')
    for m in messages:
        if isinstance(m, ModelResponse):
            ...
            if (charge := gateway_charge(m)) is not None: cost += charge; continue
            if provider == 'browser-use': cost = None; continue
            served = m.model_name
            names = [served] if served and not served.startswith(model_ref) else [served, model_ref]
            for name in dict.fromkeys(filter(None, names)):
                try: cost += float(calc_price(u, model_ref=name, provider_id=provider).total_price); break
                except Exception: continue
            else: cost = None
    return total, cost
```

优先级是：**网关实际收取的金额** > **按实际服务的模型名查价** > **按请求的模型名查价** > **`None`**。

三处判断值得记住：

- `if provider == 'browser-use': cost = None`——网关模型没有 charge 时就**放弃**，注释：「绝不用零售估价代替收取金额」。这个方向很重要：少报比错报好，因为错报会让成本核算失真而无人察觉。
- 「按实际服务的模型名查价」那条注释给了实例：「请求了 `gemini-3.5-flash`，被服务并计费为 `gemini-3.6-flash`」——所以优先用 `m.model_name`，只有在它为空或者是请求名的快照（`gpt-4.1` → `gpt-4.1-2025-04-14`）时才用请求名。
- `cost is None` 一旦被置上就 `continue` 跳过后续所有计算（`if cost is None: continue`）——一个未知价格会让整体变成「未知」，而不是「其余部分的和」。这是一致的：混算一个半准确的数字比 `None` 更有害。

`total` 里 `requests` 也一并统计，所以「这次花了多少请求」与「花了多少钱」来自同一次遍历。

**`thinking_and_text` 是一个没有内部调用者的公开辅助。**

```
def thinking_and_text(m: ModelResponse) -> list[tuple[str, str]]:
    return [('thinking' if isinstance(p, ThinkingPart) else 'text', p.content)
            for p in m.parts if isinstance(p, (ThinkingPart, TextPart)) and p.content]
```

它把一条响应里所有非空的思考与文本片段按顺序抽成 `(kind, content)` 列表。在整个仓库里它**没有调用者**——它属于模块的对外表面（评测或外部消费者会用到这种「把响应拆成可展示片段」的能力）。这一点值得在页里说明：不能因为找不到调用点就断定它无用，但也不能编造一个内部使用场景。

**`_deadlines` 把两个超时按模型种类分开。**

```
def _deadlines(s, settings, model, wait_s):
    left = max(1.0, s.time_left())
    if isinstance(model, OpenAIResponsesModel):
        settings['first_event_timeout'] = min(settings.get('first_event_timeout') or wait_s, left)
        wait_s = STREAM_TIMEOUT_S
    settings['request_timeout'] = min(settings.get('request_timeout') or wait_s, left)
```

两条规则：非流式请求全程 `wait_s`（`MODEL_TIMEOUT_S = 300`）；流式请求「首个事件 `wait_s`，之后直到结束 `STREAM_TIMEOUT_S = 930`」。两个上界都与 `time_left()` 取 `min`，因为「两者都不让运行超过投递轮的起点」。

`STREAM_TIMEOUT_S = 930` 的来历写在常量注释里：「闸道自己的流限制是 15 分钟（`llmGatewayStreamTimeout`）加 30 秒，好让闸道自己的结束先到。总共 300 秒会切断仍在流式输出、还在继续要的 bu-smart 轮次；闸道把每次被切的尝试都全额计费了，而 `cost_usd` 一个都没算（staging：5 次切断、计费 $1.84、`cost_usd` $0.0074）」。

这条注释是一份完整的成本事故报告：超时设得太短 → 请求被切断 → 服务端仍然计费 → 而本地以为没拿到响应所以不计成本 → 账目与实际支付脱节。930 秒不是「够长」，而是「比对方的硬上限多 30 秒，让它先结束」。

## 关键取舍

**源码事实：** `StreamedRequest` 对外是 `ModelResponse`、对内跑 `request_stream`，所以上层钩子全部按非流式路径工作，注释明确说「流里任何一点的错误都会被重试」。

**源码事实：** `first_event_timeout` 必须在包装内自己强制，因为上层看到的是非流式请求、框架会跳过它。

**源码事实：** 流在终态事件之前关闭时抛 `ModelConnectionError(in_stream=True)`，理由是三样后果：半个答案当结果、半个摘要当检查点、没有用量与计费。

**源码事实：** `_sealed_as_completed` 用终态事件的 `output` 覆盖响应里的 compaction part 的 `provider_details`，并把只出现在终态里的项补到最前面。

**源码事实：** reasoning item 的剥除就地改历史（因为后续请求都会发它），compaction item 的剥除只改本次请求（因为下一个会取代它）。

**源码事实：** `_cut_after_response` 靠异常链形态区分「SDK 已重试过」与「SDK 已放弃」，并对 google-genai 单独排除它自己会重试的两类。

**源码事实：** `Recording` 排在能力链最后，于是它的 `prepare` 在压缩之后跑、错误钩子最先看到原始异常。

**源码事实：** `usage_and_cost` 的花费优先级是「网关实收 > 实际服务模型名定价 > 请求模型名定价 > None」，且一旦某响应未知就整体 `None`，绝不用零售估价替代实收。

**源码事实：** `usage_and_cost` 里 `cost_usd` 与 `requests` 来自同一次遍历。

**架构推论：** 「包装成非流式」这个手法让流式变成一个**实现细节**而不是接口变化：能力链、重试、录制、错误分类都不需要知道流的存在。代价是所有与流相关的约束（首事件超时、断流判定、终态完整性）都要在这一个包装里补齐。

**架构推论：** 抓终态事件用属性替换（`_handle_terminal_response`）而不是公开接口，是因为上游 SDK 把「终态响应」放在了流对象的私有字段上。这类做法在版本升级时容易断，所以代码用 `getattr(..., None)` 容忍它不存在——代价是「终态修正」这个行为会静默失效。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/agent/loop.py` | `StreamResponses` | 能力：只对 OpenAI Responses 模型套 `StreamedRequest` |
| `browser_use/agent/loop.py` | `StreamedRequest` | 模型包装：流式执行并装配成非流式响应 |
| `browser_use/agent/loop.py` | `StreamedRequest.request` | 剥掉不可读的 compaction / reasoning item 后重试 |
| `browser_use/agent/loop.py` | `StreamedRequest._request` | 首事件超时、终态拦截、完整性检查、终态修正 |
| `browser_use/agent/loop.py` | `_sealed_as_completed` | 用终态事件覆盖/补齐 compaction part |
| `browser_use/agent/loop.py` | `_cut_after_response` | 判定「响应读到一半断了且 SDK 未重试」 |
| `browser_use/agent/loop.py` | `_deadlines` | 非流式 300 秒 / 流式首事件 300 秒 + 全程 930 秒，均不越过投递点 |
| `browser_use/agent/loop.py` | `Recording` | 每次尝试的开始/成功/失败记入 recorder |
| `browser_use/agent/loop.py` | `usage_and_cost` | 用量与花费换算：网关实收优先，未知则整体 None |
| `browser_use/agent/loop.py` | `thinking_and_text` | 把响应拆成思考/文本片段序列（模块对外表面） |

## 参考

- [browser-use 源码：agent/loop.py](https://github.com/browser-use/browser-use/blob/main/browser_use/agent/loop.py)
- [browser-use 源码：telemetry/recorder.py](https://github.com/browser-use/browser-use/blob/main/browser_use/telemetry/recorder.py)
- [OpenAI 文档：流式响应（streaming responses）](https://platform.openai.com/docs/guides/streaming-responses)
- [Pydantic AI 文档：模型包装与流式](https://ai.pydantic.dev/models/overview/)

## 与主核 / AI 的关系

「在适配层把一种传输形态改造成另一种」与主核里的「把异步 I/O 包装成同步读」是同一种手法：上层拿到它熟悉的语义，下层用更高效或更受限的方式跑。主核里的典型例子是缓冲池把「随机寻道」隐藏成「按页读」；这里是把「事件流」隐藏成「完整响应」。共同点是**改造层必须自己补齐被隐藏那层的全部约束**——主核要自己处理短读与 EOF，这里要自己处理首事件超时、断流判定与终态完整性。

对 AI 工程的实际意义有三条。第一，**「谁已经重试过」必须能判定，否则会重复重试**。`_cut_after_response` 那段靠异常链形态推断「SDK 已放弃」，并给 google-genai 单独开了一个例外，原因就是「每个 SDK 的重试策略不同，而重试叠加会让一次故障变成三次请求」。任何在 SDK 之上加自己的重试层的系统，都必须回答这个问题。

**第二，成本计账要偏向「少报」而不是「错报」**。`usage_and_cost` 在找不到价格时返回 `None` 而不是用零售价估一个数——注释说得很直白：「绝不用零售估价代替收取金额」。原因在于成本数据的用途：它会被拿去做预算判断。一个偏低的准确数字会让人继续花钱；一个看起来合理但错误的数字会让人在不该停的时候停、该停的时候没停。

**第三，把「半个响应」当成最危险的结果**。`state != 'complete'` 就抛错的理由不是保守，而是因为半截响应会被上游当成合法结果处理，并污染比它本身更重要的东西（结果文本、检查点摘要、用量统计）。这一条在流式系统里普遍适用：**宁可报一个明确的连接错误，也不要交出一个看起来完整的残缺对象**。
