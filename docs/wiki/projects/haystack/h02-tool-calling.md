---
project: haystack
feature: tool-calling
title: 工具调用执行
page_type: feature
schema_version: 1
refs:
  - path: haystack/components/agents/tool_calling.py
    symbols: [ToolNotFoundException, _validate_and_prepare_tools, _merge_tool_outputs_into_state]
  - path: haystack/components/agents/tool_calling.py
    symbols: [_process_tool_output, _build_tool_result_message, _create_tool_result_streaming_chunk]
---

# 工具调用执行

## 一句话职责

把「模型说它要调三个工具」这件事变成「真的调了、结果按调用顺序返回、并且工具写进共享状态的那些键会被合回 State」——而为了正确性它做了一件超出预期的事：**按每个调用的状态读写集把调用排成批次，保证「读某个键的工具」不会和「写那个键的工具」并发**。

## 机制是什么

职责可以拆成四段，`_run_tool` 的函数体正是这四段的顺序：

1. **准备**：`_validate_and_prepare_tools` 扁平化、查重、建「名字 → Tool」索引。
2. **解析**：`_resolve_tool_calls` 把每个 `ToolCall` 映射到它的 `Tool`；找不到就产生一条错误消息（或按配置抛错）。
3. **调度与执行**：`_schedule_tool_calls` 分批，每批并发执行。
4. **合并**：`_finalize_tool_result` 把原始结果经 `_build_tool_result_message` 变成 `ChatMessage`，同时 `_merge_tool_outputs_into_state` 把工具想写进 State 的键写进去。

而「工具能返回状态更新」是这一层表达力的来源：一个工具不只是「返回一段文本给模型看」，它还可以声明 `outputs_to_state` 把自己的返回值写进 State 的某些键——于是下一个工具、或者下一轮的钩子，就能读到它。

## 怎么实现的

**`_validate_and_prepare_tools` 的三步各挡一类错误。** 实现只有五行：

```python
if not tools:
    raise ValueError("Tool execution requires at least one tool.")
available_tools = flatten_tools_or_toolsets(tools)
_check_duplicate_tool_names(available_tools)
tool_names = [tool.name for tool in available_tools]
return dict(zip(tool_names, available_tools, strict=True))
```

第一步挡「空工具集」；第二步扁平化（`Toolset` 可能嵌套）；第三步查重名；最后 `zip(..., strict=True)`。

`strict=True` 这个参数值得单独指出：它让「名字数量与工具数量不等」直接抛错而不是静默截断。这两者在这里必然相等（`tool_names` 就是从 `available_tools` 派生的），所以 `strict=True` 的作用是**把不变量写成断言**——将来若有人改了 `tool_names` 的来源，不会静默出错。

**`_schedule_tool_calls` 是一个「按状态依赖分层」的拓扑排序，而它的必要性来自 State 的可写性。** 完整逻辑：

```python
io_list = [_state_io_for_call(tool, tc.arguments) for tc, tool in zip(tool_calls, tools, strict=True)]
deps = [set() for _ in range(n)]
for j in range(n):
    reads_j, _ = io_list[j]
    for i in range(n):
        if i == j: continue
        _, writes_i = io_list[i]
        if _keys_intersect(reads_j, writes_i):
            deps[j].add(i)

scheduled = [False] * n
done = set()
batches = []
while len(done) < n:
    ready = [k for k in range(n) if not scheduled[k] and deps[k] <= done]
    if not ready:
        ready = [next(k for k in range(n) if not scheduled[k])]
    for k in ready:
        scheduled[k] = True
    done.update(ready)
    batches.append(ready)
```

四点值得逐一读：

1. **依赖是单向的：只按「读后写」建边。** `deps[j]` 收集「j 读的键被别人写的那些 i」。所以「两个工具都写同一个键」**不构成依赖**——docstring 明写「仅写写重叠不产生依赖」。理由是写写冲突不需要排序，只需要一个确定的合并顺序，而这个顺序由「批内按调用顺序合并」保证。

2. **`deps[k] <= done` 是「依赖集是已完成集的子集」。** 这是标准的 Kahn 算法判定，但写法更紧凑：不用入度数组，直接查集合包含。

3. **无 `ready` 时打破环的方式是「取最小索引单独跑一个」。** 注释解释：「剩下一个依赖环时，按确定的方式打破它——让索引最小的调用接下来单独跑」。所以环不会死循环，而是被降级成串行。什么情况会成环？docstring 举例：「一个既读又写同一个键的工具，被请求多次」——那两次调用互为依赖。

4. **`batches` 里的顺序就是执行顺序**，而 `deps` 建边时用的是**调用索引**，所以「模型请求的顺序」只在两个工具无依赖时保留。

`_state_io_for_call` 是这个调度的输入，它的解析规则**刻意与 `_inject_state_args` 对齐**：

```python
func_params = _get_func_params(tool)
if any(_is_state_annotation(param_type) for param_type in func_params.values()):
    return _ALL_STATE_KEYS, _ALL_STATE_KEYS
param_mappings = tool.inputs_from_state or {}
reads = {state_key for state_key, param_name in param_mappings.items() if param_name not in llm_args}
writes = set((tool.outputs_to_state or {}).keys())
return reads, writes
```

三条规则：带 `State` 注解的参数 → 读写都是通配符；否则读集是 `inputs_from_state` 里**模型没提供的**那些参数（因为「LLM 参数优先，会短路状态查找」）；写集是 `outputs_to_state` 的键。

`reads` 那个 `if param_name not in llm_args` 是一个精确的优化：若模型已经提供了这个参数，工具就不会去读状态，所以不该产生依赖。这让「模型明确给了值」与「需要从状态取」两种情况有不同的调度结果。

而通配符用哨兵 `_AllStateKeys` 而不是「全键集合」：

```python
_ALL_STATE_KEYS = _AllStateKeys()
_StateKeys = set[str] | _AllStateKeys
```

`_keys_intersect` 处理三种组合：

```python
if a is _ALL_STATE_KEYS:
    return b is _ALL_STATE_KEYS or bool(b)
if b is _ALL_STATE_KEYS:
    return bool(a)
return bool(a & b)
```

第一个分支的 `b is _ALL_STATE_KEYS or bool(b)` 有一句注释：「通配符 `a` 覆盖每个键，所以只要 `b` 碰到任何键就重叠」。而 `bool(b)` 对空集返回 `False` ——所以「一个不读不写的工具」与「一个读写一切的钩子式工具」**不构成依赖**。这个细节很实在：若把空集当「读一切」，那所有工具都会串行。

**参数准备被刻意推迟到每批开始时，理由写在 `_resolve_tool_calls` 的 docstring 里。** 原文：「参数准备在这里**故意不做**：参数按执行批次准备（见 `_schedule_tool_calls`），这样一个从 State 读的工具能观察到同一步里更早跑的工具所做的写入」。

所以 `_resolve_tool_calls` 只返回「调用 + 对应的 Tool」，不碰参数。而 `_run_tool` 的循环里：

```python
for batch in batches:
    futures = {}
    for idx in batch:
        args = _prepare_tool_args(tool=..., tool_call_arguments=tool_calls[idx].arguments, state=state, ...)
        futures[idx] = executor.submit(...)
```

`args` 在**每批的循环开头**算，用的 `state` 是「前面批次已经合并完的结果」。这就是「读后写」能被真正观察到的机制——调度保证了顺序，参数准备的时机保证了看到新值。两者缺一不可。

**`_inject_state_args` 的两条注入路径，优先级不同。** 实现：

```python
final_args = dict(llm_args)
for state_key, param_name in (tool.inputs_from_state or {}).items():
    if param_name not in final_args and state.has(state_key):
        final_args[param_name] = state.get(state_key)
for param_name, param_type in func_params.items():
    if _is_state_annotation(param_type):
        final_args[param_name] = state
```

第一条：按 `inputs_from_state` 显式映射注入，**且只在模型没提供时才注入**（`param_name not in final_args`）。第二条：任何标注为 `State` 的参数**无条件注入整个 State 对象**。

第二条的「无条件」很重要：即使模型在参数里编造了一个 `state` 值，它也会被真实 State 覆盖。所以 `State` 注解的参数是模型无法伪造的——这既是安全边界（模型不能注入任意状态）也是正确性保证（工具拿到的永远是真状态）。

docstring 的第一句是「LLM 参数优先」，但严格说只有第一条路径是「LLM 优先」；第二条是「State 强制」。两种意图被写在同一个小节里，读的时候要分开。

**`_process_tool_output` 的三层配置与一条容错规则。** 它接收一个 `config` dict，可能含 `source` / `handler` / `raw_result`：

```python
source_key = config.get("source")
value = result.get(source_key) if source_key is not None and isinstance(result, dict) else result
handler = config.get("handler")
raw_result = config.get("raw_result", False)
if handler is None:
    if raw_result: return value
    return _result_to_string(value)
try:
    return handler(value)
except Exception as e:
    if raise_on_failure: raise
    logger.warning(...)
    return _result_to_string(value)
```

三个决定：

1. **`source` 只在 `result` 是 dict 时才生效** ——否则整个 result 就用。所以 `source` 是一个「可选解包」而不是「必须匹配的键」。
2. **`raw_result=True` 且无 handler 时直接返回原值**，字符串化被跳过。注释说明了用途：「raw result 主要用于让 `ImageContent` 或 `TextContent` 块能被直接返回并由 `ChatMessage.from_tool` 消费」——**图像不能被 JSON 序列化成字符串**，所以必须留一条原值直通的路径。
3. **handler 抛异常时按 `raise_on_failure` 决定重抛还是降级**。降级路径是「记警告 + 退回默认字符串化」，且消息里带上 handler 名与工具名。

第 3 条的设计值得注意：它与「工具本身失败」是两个不同的失败点。工具跑成功了但格式化结果失败——这种情况下丢掉整个结果太浪费，降级成字符串至少保住了信息。

**`_build_tool_result_message` 区分单输出与多输出两种配置，且判断方式很务实。** 先判：

```python
outputs_config = tool.outputs_to_string or {}
if not outputs_config or _is_single_output_config(outputs_config):
    tool_result = _process_tool_output(outputs_config, result, tool_call, raise_on_failure=raise_on_failure)
    return ChatMessage.from_tool(tool_result=tool_result, origin=tool_call)
tool_result_dict = {output_key: _process_tool_output({**cfg, "raw_result": False}, result, tool_call, ...) for output_key, cfg in outputs_config.items()}
return ChatMessage.from_tool(tool_result=_result_to_string(tool_result_dict), origin=tool_call)
```

`_is_single_output_config` 的判据是：

```python
return any(key in outputs_to_string and not isinstance(outputs_to_string[key], dict) for key in ("source", "handler", "raw_result"))
```

所以「顶层出现 `source`/`handler`/`raw_result` 且它不是字典」→ 单输出格式。这个判据的巧处在于**不靠显式标记**：两种格式的区别体现在「这些键的值是标量还是字典」上。而 `not isinstance(..., dict)` 是必需的——多输出格式下 `handler` 可能是一个以 `handler` 为名的输出键。

多输出分支里有一行 `{**cfg, "raw_result": False}` ——**强制关掉 `raw_result`**。这与 Tool 的 docstring 一致：「注意 `raw_result` 在多输出格式里不被支持」。所以这里用「静默忽略」而不是报错，是一个轻微的宽容选择。

多输出最后把整个字典再 `_result_to_string` 一次，所以「每个值先字符串化，整个字典再字符串化」。

**`_merge_tool_outputs_into_state` 是「把工具声明想写的键写回共享状态」这一步，而它的失败处理是有意的。** 实现：

```python
if not isinstance(result, dict):
    return
for state_key, config in (tool.outputs_to_state or {}).items():
    source_key = config.get("source", None)
    if source_key and source_key not in result:
        continue
    output_value = result.get(source_key) if source_key else result
    try:
        state.set(state_key, output_value, handler_override=config.get("handler"))
    except Exception as e:
        raise RuntimeError(f"Tool '{tool.name}': failed to merge outputs into state. {e}") from e
```

四处值得读：

1. **不是 dict 的结果直接返回，什么都不写。** 所以「一个返回字符串的工具」即使声明了 `outputs_to_state` 也不会写任何东西——静默。这是一条容易踩的隐含约束：`outputs_to_state` 只对返回字典的工具有效。

2. **`source_key not in result` 时 `continue`** ——缺这个键就跳过这个 state 键，不报错。所以一个工具可以根据输入不同而产出不同的键集。

3. **`source_key` 为空时 `output_value = result`（整个字典）** ——所以「不给 `source`」意味着「把整个返回值写进去」。

4. **`state.set` 的异常被包成 `RuntimeError` 并带上工具名。** 这一点与本文件其他地方的容错方向**相反**：`_process_tool_output` 的 handler 失败会降级，而这里的状态写入失败会直接抛。理由很清楚——状态写失败意味着「后续工具会看到错误的输入」，那是正确性问题，不能降级。

`handler_override` 这个参数是 State 侧的一个能力：`state.set(key, value, handler=...)` 默认会按 schema 声明的 handler 合并，而 `handler_override` 让工具临时指定自己的合并方式。所以工具可以决定「我这个值要覆盖还是要追加」。

**并发执行是「每批一次，批内用线程池」。** 同步版：

```python
with ThreadPoolExecutor(max_workers=max_workers) as executor:
    for batch in batches:
        futures = {}
        for idx in batch:
            args = _prepare_tool_args(...)
            futures[idx] = executor.submit(_make_context_bound_invoke(...))
        for idx in batch:
            message = _finalize_tool_result(outcome=futures[idx].result(), ...)
```

`executor` 在**所有批次外面**建一次，所以线程被复用。而结果的收集在 `for idx in batch` 里**按调用顺序**做（不是按完成顺序），注释写明：「批内按调用顺序合并结果，让写写合并保持确定」。所以「并发执行、顺序合并」——两个不冲突的优化被同时满足。

**异步版的并发控制换成了信号量，且 `max_workers` 语义变了。**

```python
semaphore = asyncio.Semaphore(max_workers)
...
batch_results = await asyncio.gather(*tasks.values(), return_exceptions=True)
```

注释解释：「`max_workers` 加信号量为同步与异步工具调用统一限制并发——异步工具被直接 await，同步工具在 `Tool.invoke_async` 内部被派发到工作线程」。

所以同步版靠线程池大小限并发，异步版靠信号量 ——因为异步工具本就不占线程，只用线程池大小限不住它们。

`return_exceptions=True` 带一句注释：「即使一个抛错也要等这一批全部完成，这样这一步失败后没有工具还在跑」。这是「失败时也不要留下后台任务」的显式选择。然后按顺序重抛：

```python
if isinstance(outcome, BaseException) and not isinstance(outcome, ToolInvocationError):
    raise outcome
```

**注意这个 `not isinstance(outcome, ToolInvocationError)`** ——`ToolInvocationError` 被排除在外，因为它是「可预期的工具失败」，由 `_finalize_tool_result` 按 `raise_on_failure` 决定如何处理（抛或转成错误消息）。而其他异常（例如格式化 handler 崩了）在这里立刻重抛。

这条区分贯穿整个文件：`_make_context_bound_invoke` 的返回类型标注就是 `ToolInvocationError | tuple[Any, ChatMessage]` ——它把 `ToolInvocationError` **当作返回值**而不是异常抛出，就是为了「并行执行时收集失败而不中断整批」。

**`_create_tool_span` 每个工具调用一个 span，而父 span 是显式传的。** docstring 给了两条理由：

第一条是标准：「追踪 agent 的既成标准是每个工具调用一个 span，所以每次调用有自己的 span 而不是把一步的所有调用归到一起。OpenTelemetry GenAI 语义约定用一个专门的『execute tool』span 把这点编成规范，Langfuse 等追踪后端也遵循同一模型」。

第二条是实现上的：「父级是传进来的而不是在这里从 tracer 读取的：一步的调用并发执行，所以一个全局追踪当前 span 的 tracer 会把一个兄弟工具 span 报成当前 span，于是这些 span 会变成链式而不是并列」。

第二条是这一页最有价值的技术点：**并发场景下不能靠「当前 span」隐式继承**。若每个工具在自己的线程里读 `tracer.current_span()`，它可能读到另一个工具刚开的 span，于是追踪树变成一条链而不是扇形。显式传父级解决这个问题。

而 `parent_span = tracing.tracer.current_span()` 在**任何工具运行之前**取一次 —— 所以这一步的所有工具 span 都是同一个父级的兄弟。

**`_make_context_bound_invoke` 用 `contextvars.copy_context()` 保住追踪与上下文。** 实现：

```python
ctx = contextvars.copy_context()
def _invoke() -> ...:
    with _create_tool_span(tool, tool_call, parent_span) as span: ...
def _runner() -> Any:
    return ctx.run(_invoke)
return _runner
```

`ThreadPoolExecutor` 的线程不继承调用方的 contextvars，所以工具内部若用 `tracing.tracer.trace(...)` 开 span，会挂到错误的父级或丢失上下文。`ctx.run(_invoke)` 把执行放进一份 contextvars 快照里。

而异步版**不需要**这一步，docstring 解释了为什么：「ContextVars 天然继承进子任务（原生异步分支），而 `asyncio.to_thread` 会为回退分支传播它们」。

**`_create_tool_result_streaming_chunk` 让工具结果能流式吐出。**

```python
return StreamingChunk(
    content="",
    index=index,
    tool_call_result=tool_message.tool_call_results[0],
    start=True,
    meta={"tool_result": tool_message.tool_call_results[0].result, "tool_call": tool_call},
)
```

`content=""` ——工具结果不进 `content`，而是走 `tool_call_result` 字段与 `meta`。所以「流式」在这里的含义是「每完成一个工具就吐一个结构化片段」，而不是「文本逐字流出」。

`_run_tool` 里还有一个收尾：

```python
if tool_messages and streaming_callback is not None:
    streaming_callback(StreamingChunk(content="", finish_reason="tool_call_results", meta={"finish_reason": "tool_call_results"}))
```

所以流以一个 `finish_reason="tool_call_results"` 的空块结束。这与 LLM 流式的 `finish_reason` 约定同构——下游只要看 `finish_reason` 就能知道「这批工具结果说完了」。

**`ToolNotFoundException` 的消息带上可用工具列表。**

```python
message = f"Tool '{tool_name}' not found. Available tools: {', '.join(available_tools)}"
```

而 `_resolve_tool_calls` 按 `raise_on_failure` 决定抛它还是把它转成一条 `error=True` 的工具消息。转成消息时：

```python
error_messages.append(ChatMessage.from_tool(tool_result=str(error), origin=tool_call, error=True))
```

所以「模型调了一个不存在的工具」在宽容模式下不是异常，而是**一条错误工具结果**——模型在下一轮就能读到自己搞错了。这比抛错中断整个 agent 运行合适得多，因为模型往往能自我纠正。

而 `error_messages` 被拼在结果列表**前面**：`tool_messages = error_messages + [m for m in results if m is not None]`。所以顺序上「所有未找到的错误」先于「所有成功结果」，与调用顺序不一致——这是一个小的顺序偏差，在排查时值得注意。

## 关键取舍

**源码事实：** `_validate_and_prepare_tools` 用 `zip(..., strict=True)` 把「名字数与工具数相等」写成断言。

**源码事实：** 调度只按「读后写」建依赖边；「写写重叠」不构成依赖，靠批内按调用顺序合并来保证确定性。

**源码事实：** 依赖环用「取最小索引单独跑」打破，不会死循环。

**源码事实：** `_keys_intersect` 里「空读集」与「通配符」不构成重叠，所以不读不写的工具不会被迫串行。

**源码事实：** 参数准备被推到每批开始时做，而不是解析阶段——这样「读状态」的工具能看到前面批次的写入。

**源码事实：** `State` 注解的参数**无条件**注入真实 State，模型无法伪造；`inputs_from_state` 的注入才是「LLM 参数优先」。

**源码事实：** `raw_result=True` 提供一条不过字符串化的直通路径，供图像/文本内容块使用。

**源码事实：** 输出格式化 handler 失败会按 `raise_on_failure` 降级为字符串化，而 `state.set` 失败一律抛 `RuntimeError`（因为会影响后续工具的正确性）。

**源码事实：** `outputs_to_state` 只对返回 dict 的工具生效，非 dict 结果静默不写。

**源码事实：** 单输出与多输出格式的判别不靠显式标记，而靠「`source`/`handler`/`raw_result` 的值是不是字典」。

**源码事实：** 父 span 在任何工具运行前取一次并显式传入，因为并发下「当前 span」会被兄弟调用污染。

**源码事实：** 同步版用线程池大小限并发，异步版额外加 `asyncio.Semaphore`；`ToolInvocationError` 被当作返回值而不是异常，以支持并发收集失败。

**架构推论：** 「按状态读写集排批次」这件事说明 State 不是单纯的旁路存储，而是**并发正确性的一部分**。若所有工具都并发跑，那么「工具 A 写 `documents`、工具 B 读 `documents`」这两个调用就会产生竞态，而模型完全可能在一次回复里同时请求它们。所以调度器必须理解状态访问——这也是为什么 `_state_io_for_call` 要精确复刻 `_inject_state_args` 的解析规则：**调度时的判断与执行时的行为必须是同一套规则**，否则会漏掉依赖。

**架构推论：** 「`ToolInvocationError` 当返回值」是一个为了并发而做的类型选择。若它作为异常抛出，`gather` 就会中断整批；而要「收集所有失败再统一处理」，就必须让失败成为一个可以放在结果槽里的值。这与 Result 类型在并发场景里的价值是同一件事。

**架构推论：** `raw_result` 那条直通路径揭示了一个结构性限制：**「把结果变成字符串」这个动作对多模态内容是有损的甚至不可行的**。所以这一层必须有一条「不转换」的逃生舱，代价是「字符串化」不再是全局不变量，而是「默认行为」。任何「统一转成 X」的设计都会在遇到非 X 的原生类型时需要一个这样的口子。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/components/agents/tool_calling.py` | `ToolNotFoundException` | 带可用工具列表的未找到错误 |
| `haystack/components/agents/tool_calling.py` | `_validate_and_prepare_tools` | 扁平化、查重、建名字索引 |
| `haystack/components/agents/tool_calling.py` | `_merge_tool_outputs_into_state` | 按 `outputs_to_state` 写回 State，失败抛错 |
| `haystack/components/agents/tool_calling.py` | `_process_tool_output` | 单条输出的解包、handler 调用与降级 |
| `haystack/components/agents/tool_calling.py` | `_build_tool_result_message` | 单/多输出两种配置转成工具消息 |
| `haystack/components/agents/tool_calling.py` | `_create_tool_result_streaming_chunk` | 每个工具结果一个结构化流式块 |
| `haystack/components/agents/tool_calling.py` | `_schedule_tool_calls` | 按读后写依赖分层，环则按索引打破 |
| `haystack/components/agents/tool_calling.py` | `_state_io_for_call` | 复刻注入规则算出读写集 |
| `haystack/components/agents/tool_calling.py` | `_keys_intersect` | 通配符与空集的相交判定 |
| `haystack/components/agents/tool_calling.py` | `_inject_state_args` | 状态注入，`State` 注解无条件覆盖 |
| `haystack/components/agents/tool_calling.py` | `_prepare_tool_args` | 每批开始时准备参数并注入流式回调 |
| `haystack/components/agents/tool_calling.py` | `_resolve_tool_calls` | 解析调用但故意不准备参数 |
| `haystack/components/agents/tool_calling.py` | `_get_func_params` | 从组件 socket 或函数签名取参数注解 |
| `haystack/components/agents/tool_calling.py` | `_create_tool_span` | 每调用一个 span，父 span 显式传入 |
| `haystack/components/agents/tool_calling.py` | `_make_context_bound_invoke` | 线程池里保住 contextvars |
| `haystack/components/agents/tool_calling.py` | `_make_bounded_invoke_async` | 信号量限并发，异步分支无需搬上下文 |
| `haystack/components/agents/tool_calling.py` | `_finalize_tool_result` | 合并结果并返回消息，错误按配置处理 |
| `haystack/components/agents/tool_calling.py` | `_run_tool` | 分批并发执行，按调用顺序合并 |
| `haystack/components/agents/tool_calling.py` | `_ALL_STATE_KEYS` | 「所有状态键」的通配符哨兵 |

## 参考

- [haystack 源码：components/agents/tool_calling.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/agents/tool_calling.py)
- [haystack 文档：工具](https://docs.haystack.deepset.ai/docs/tools)
- [OpenTelemetry GenAI 语义约定：execute tool span](https://github.com/open-telemetry/semantic-conventions-genai/blob/main/docs/gen-ai/gen-ai-agent-spans.md)

## 与主核 / AI 的关系

主核里最接近的是「查询计划里算子的并行调度」：一批算子能否并发，取决于它们之间的读写依赖。这一页的 `_schedule_tool_calls` 就是那个依赖分析的对应物，连「读后写建边、写写不建边」这条规则都一样——数据库中写写冲突靠锁或 MVCC 解决，不靠调度排序。

差别在于**依赖的来源**。主核的依赖由表/列级别的静态分析得出，编译期就固定了；这里的依赖是**运行时从工具声明里推出来的**，而且带有通配符（`State` 注解 = 读写一切）。所以它必须有一个「无法排序时打破环」的兜底，而数据库的调度器面对环会直接判定计划非法。

与 h01（智能体组件与状态）的分工：那一页管循环与退出，这一页管一次工具执行。`_run_step` 里 `tool_messages, exe_context.state = _run_tool(**tool_execution_inputs)` 就是分界线——而注意 State 是**被返回的**，不是就地改的（签名 `-> tuple[list[ChatMessage], State]`），虽然实际上 `_merge_tool_outputs_into_state` 就地改了它。

与 h03（工具定义）的关系是直接的下游：`Tool` 的 `inputs_from_state` / `outputs_to_state` / `outputs_to_string` 三个字段的全部语义都在这一页被消费。所以 h03 定义契约，这一页实现它。

对 AI 工程最实用的一条是「模型一次回复里请求多个工具时，必须按状态依赖调度而不是全并发」。真实工具经常共享状态：一个工具写 `documents`、另一个读它做摘要。若全并发，读的那个会拿到空值或旧值，而症状是「偶尔摘要为空」——极难归因。做法是精确算出每个工具的读写集，只在无冲突时并发。

另一条是「工具失败要成为值而不是异常」。模型调错工具名、或工具内部报错，都是**高频且可恢复**的。把它们变成带 `error=True` 的工具消息，模型下一轮就能看到并纠正；抛异常中断则浪费已花的 token 且无法自愈。判据是：**这个失败能被上游的决策者（模型）利用吗？** 能，就该作为数据返回。

最后一条是「并发下追踪父级必须显式传」。任何「多个子任务并发、每个开自己的 span」的实现都不能依赖「当前 span」——那会让兄弟关系变成父子链。正确做法是在并发开始前取一次父级，然后显式传进每个子任务。
