---
project: crewai
feature: context-summarization
title: 上下文压缩与空回复重试
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/utilities/agent_utils.py
    symbols: [_summarize_context_before_llm_call, _proactively_summarize_context, _call_llm_retrying_empty]
---

# 上下文压缩与空回复重试

## 一句话职责

每次调用模型之前先算一次「这次请求会不会超窗」，会超就把历史消息压成摘要再发；模型若返回空内容则重试一次，还是空就抛 `EmptyLLMResponseError`。两件事都在 `agent_utils` 里，都是「发请求之前/之后」的薄封装。

## 机制是什么

这是两个独立的保护，共用同一个调用点。

**压缩**是**主动预检**而不是被动接错：`get_llm_response` 在真正调用模型之前，若执行器标了 `respect_context_window`，就调 `_proactively_summarize_context`——先用 `llm.is_context_window_overflowing_threshold(...)` 估算，超阈值才真正压缩。这样避免了「先发失败、再捕获超窗异常、再压缩、再重发」的一轮浪费。

**空回复重试**是**一次重试**：`_call_llm_retrying_empty` 的逻辑是「试一次，空就再试一次，还空就抛」。两次尝试之间会重新过一遍 RPM 限流（`enforce_rpm_limit`），因为限流器是按请求计数的，重试也是一次请求。

## 怎么实现的

**阈值的两层折算。** 判断「会不会超窗」用的是 `is_context_window_overflowing_threshold(messages, tools, response_model)`，实现是 `input_tokens > get_context_window_size()`。而 `get_context_window_size` 返回的是**可用窗口**而非模型广告窗口：`resolve_context_window_size` 把配置值或目录值乘以 `CONTEXT_WINDOW_USAGE_RATIO = 0.85`。所以判定实际是「请求超过模型窗口的 85% 就压缩」，docstring 里明确写了这一点。目录查不到时用 `DEFAULT_CONTEXT_WINDOW_SIZE = 8192`，同样乘 0.85，并对每个模型只警告一次（`_fallback_warned_models` 集合去重）。

token 估算走 `get_input_tokens_count(messages, tools, response_model)`。基类的默认实现是**每四个字符一 token**（`(character_count + 3) // 4`），并对每次调用都记一条 warning 说明「用的是近似值」。文本长度由 `message_content_text(message)` 逐条取——这个辅助函数处理了内容是多模态 parts 列表的情况，对 `None` 返回空串、对无可用文本的列表返回占位文本。

**`_proactively_summarize_context` 的异常策略是这一页最关键的取舍。** 它整个包在 try 里，三种结果：

- 若压缩过程中抛 `HookAborted` 或 `LLMCallBlockedError`，**原样重新抛出**。
- 其他任何异常只记 `logger.debug(..., exc_info=True)`，然后**继续调用模型**。
- 正常返回则什么都不做。

也就是说：**压缩是尽力而为的优化，不是必经的关卡**。压缩失败不该阻断一次本来可能成功的请求；但钩子拒绝是用户的明确意图，必须穿透。这个区分很重要——它把「基础设施故障」与「策略性拒绝」分开对待。

**`_summarize_context_before_llm_call` 的前置判断。** 它先剔除所有 `role == "system"` 的消息，得到 `non_system_messages`，然后：

```
if not allow_single_user_compaction and len(non_system_messages) == 1 and non_system_messages[0].get("role") == "user":
    return
```

即「只剩一条用户消息时压缩没有意义」——没东西可压。但 `allow_single_user_compaction` 为真时例外，docstring 说明理由：会话型调用方（router）把整个上下文序列化进**一条**用户消息里，所以对它那一条也必须压。这是一处「按调用方语义放宽」的参数化。

接着它发 `ContextWindowSummarizationStartedEvent`（带 `from_task`、`from_agent`、`model`），verbose 时打印一个面板显示压缩前的 token 数；调 `llm.summarize_messages(messages, callbacks, verbose)`；失败则发 `ContextWindowSummarizationFailedEvent` 并重新抛出；成功则发 `ContextWindowSummarizationCompletedEvent`，verbose 时再算一次压缩后 token 数并打印「X → Y (Z% reduction)」。

**事件的三态是刻意的。** Started / Completed / Failed 三个事件配对，保证追踪里这段操作总有明确结局。与 b03 里 `process_guardrail` 的处理思路一致——有开始事件就必须有终止事件，否则追踪的 span 会变成孤儿。

**压缩本体在 `llms/context_window.py`。** `summarize_messages(llm, messages, callbacks, verbose)` 的步骤：

1. 把消息分成 `system_messages` 与 `work_messages`；`work_messages` 为空则直接返回（没什么可压）。
2. `preserved_files = collect_user_message_files(messages)`——记下用户消息里附带的文件引用。
3. `max_tokens = llm.get_context_window_size()`，`chunks = _chunk_messages(llm, work_messages, max_tokens)`。
4. `summaries = _summarize_chunks(llm, chunks, callbacks)`。
5. 用国际化模板 `summary` 把各段摘要合并成一条消息，若有 `preserved_files` 就挂到这条消息的 `files` 字段上。
6. **原地清空并重写** `messages`：先放回所有 system 消息，再追加那一条摘要消息。

关键点：**system 消息原样保留、非 system 消息全部被一条摘要替代、文件引用被搬到摘要消息上**。历史从 N 条变成 1 条，压缩比极高，代价是不可逆——原始对话在 `messages` 里彻底消失。

**分块是「按请求预算贪心装填」。** `_chunk_messages` 先检查一个空请求的 token 数是否已超 `max_tokens`（超了就抛「Summarization prompt exceeds the context window before conversation history is added」）。然后逐条消息试装：`_summary_request_tokens(llm, [*current, message]) <= max_tokens` 就加入当前块；装不下就把当前块封存、开新块；单条消息自己就装不下时走 `_split_message_to_fit` 切分，切出的片段若仍装不进空块则抛「Summarization message could not be split to fit the context window」。

`_summary_request_tokens` 精确地算「把这段消息喂给摘要提示词模板后」的 token 数（`llm.get_input_tokens_count(build_summary_prompt(messages))`），而不是消息本身的 token 数——因为摘要请求里还有模板开销。

`_split_message_to_fit` 按 75% 预算切文本：`MESSAGE_SPLIT_BUDGET_RATIO = 0.75`，`part_budget = max(1, int(available_tokens * 0.75))`，再按 `len(text) * part_budget / content_tokens` 折算成字符长度切片。`content_tokens` 或 `available_tokens` 非正时抛错。切出来的片段是 `{**message, "content": text[start:start+part_length]}`——**保留原消息的其他字段，只换 content**，但多模态的 parts 列表在这里已被 `message_content_text` 拍平成字符串，所以切分后丢失了 parts 结构。

**并发摘要有事件循环的两种应对。** `_summarize_chunks` 用 `asyncio.Semaphore(4)` 限制并发，`asyncio.gather` 收集，每段 `await llm.acall(build_summary_prompt(chunk), callbacks=callbacks)`，然后从响应里用正则 `<summary>(.*?)</summary>` 抠出摘要正文（匹配不到就用整段响应 strip）。由于这是同步函数，它必须自己起 loop：

```
coroutine = gather_summaries()
try: asyncio.get_running_loop()
except RuntimeError:
    return asyncio.run(coroutine)   # 没有运行中的 loop
context = contextvars.copy_context()
with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
    return pool.submit(context.run, asyncio.run, coroutine).result()
```

有运行中的 loop 时（例如在 Flow 里），不能直接 `asyncio.run`（会抛「cannot be called from a running event loop」），于是复制 contextvars、丢进单线程池、在线程里 `asyncio.run`。这与 `LLMGuardrail._run_coroutine_sync` 是同一套手法，说明这类「同步 API 里要跑异步逻辑」的需求在代码里反复出现。

**`_call_llm_retrying_empty` 的精确语义。** 实现只有几行：

```
with contextlib.suppress(EmptyLLMResponseError):
    if answer := call():
        return answer
_report_empty_llm_answer(printer, verbose, retrying=True)
enforce_rpm_limit(request_within_rpm_limit)
with contextlib.suppress(EmptyLLMResponseError):
    if answer := call():
        return answer
_report_empty_llm_answer(printer, verbose, retrying=False)
raise EmptyLLMResponseError(llm.model)
```

要点：

- 「空」的判据是**返回值为假**（`None`、`""`、空列表——空列表正是「没有工具调用」的情形），**或者**调用本身抛了 `EmptyLLMResponseError`（docstring 说这是 LiteLLM 流式场景的形态）。`contextlib.suppress` 把后者也吞成「空」。
- 第一次尝试的 RPM 由**调用方**负责放行（`get_llm_response` 里在 `_prepare_llm_call` 之外的地方并没有显式调 `enforce_rpm_limit`，但 `_call_llm_retrying_empty` 内部对**第二次**尝试显式调用了）。这是 docstring 里明确写的分工：「调用方放行第一次请求，重试在这里放行」。
- verbose 时两次提示不同：第一次说「Received an empty response from the LLM. Retrying once.」（黄色），第二次说「Received None or empty response from LLM call.」（红色）。
- 最终抛出 `EmptyLLMResponseError(llm.model)`，把模型名带进异常。

**异步版本 `_acall_llm_retrying_empty` 完全同构**，只把 `call()` 换成 `await call()`。`aget_llm_response` 里还有一处差异：它的上下文预检用 `await asyncio.to_thread(_proactively_summarize_context, ...)` 包了一层——因为压缩内部要跑自己的 `asyncio.run`，直接在事件循环里跑会冲突，所以挪到线程。

## 关键取舍

**源码事实：** 压缩是预检式的（先估算再压），不是「失败重试式」。省掉了一次注定被拒绝的请求，但也意味着**依赖 token 估算的准确性**——估算偏小就不会触发压缩，然后请求真的超窗并抛 `LLMContextLengthExceededError`。

**源码事实：** 判定阈值是模型窗口的 85%（`CONTEXT_WINDOW_USAGE_RATIO = 0.85`），不是 100%。预留 15% 给输出与估算误差。

**源码事实：** 压缩会把所有非 system 消息替换成**一条**摘要消息，原始内容不可恢复。system 消息与用户消息附带的文件引用被保留。

**源码事实：** 压缩失败（非钩子类异常）被 `logger.debug` 吞掉并继续调用模型。压缩是优化而非关卡；唯一穿透的是 `HookAborted` 与 `LLMCallBlockedError`。

**源码事实：** 单条用户消息默认不压缩（没内容可压），除 `allow_single_user_compaction=True` 时例外——因为会话型调用方把整个上下文塞在一条消息里。

**源码事实：** 空回复的重试只有一次，且第二次也要过 RPM 限流。两次都空就抛异常，不再尝试。

**源码事实：** token 估算的基类实现是「四字符一 token」，且每次调用都警告。这是一个刻意保守的近似（对英文偏大、对中文偏小），忠实实现需要各供应商覆盖。

**架构推论：** 把「压缩」与「重试」都做成调用前的装饰而非调用逻辑的一部分，使得 `get_llm_response` 的主线保持「拼参数 → 发请求 → 跑 after 钩子」三段清晰，保护逻辑可以独立修改。

**架构推论：** 摘要本身由**同一个模型**完成（`llm.acall`），没有独立的摘要模型。这让配置简单，但也意味着压缩动作与业务调用共享同一份 token 预算与同一个供应商的可用性——模型不可用时，压缩与调用会一起失败。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/utilities/agent_utils.py` | `_proactively_summarize_context` | 预检装饰：超阈值才压缩，非钩子异常吞掉继续 |
| `lib/crewai/src/crewai/utilities/agent_utils.py` | `_summarize_context_before_llm_call` | 压缩动作本体与三态事件、verbose 面板 |
| `lib/crewai/src/crewai/utilities/agent_utils.py` | `_call_llm_retrying_empty` | 空回复重试一次，重试再过 RPM，仍空则抛 |
| `lib/crewai/src/crewai/utilities/agent_utils.py` | `_acall_llm_retrying_empty` | 异步同构版本 |
| `lib/crewai/src/crewai/utilities/agent_utils.py` | `_report_empty_llm_answer` | 两次空回复的不同提示（黄 / 红） |
| `lib/crewai/src/crewai/llms/context_window.py` | `summarize_messages` | 压缩本体：保留 system、合并非 system 为一条摘要、搬移文件引用 |
| `lib/crewai/src/crewai/llms/context_window.py` | `_chunk_messages` | 按摘要请求预算贪心装填并切分超长消息 |
| `lib/crewai/src/crewai/llms/context_window.py` | `_split_message_to_fit` | 按 75% 预算把单条消息切成多片 |
| `lib/crewai/src/crewai/llms/context_window.py` | `resolve_context_window_size` | 可用窗口折算：配置 > 目录 > 默认 8192，统一乘 0.85 |
| `lib/crewai/src/crewai/llms/base_llm.py` | `is_context_window_overflowing_threshold` | 预检判据：估算 token > 可用窗口 |
| `lib/crewai/src/crewai/llms/base_llm.py` | `get_input_tokens_count` | 基类近似估算：四字符一 token |

## 参考

- [CrewAI 源码：agent_utils.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/utilities/agent_utils.py)
- [CrewAI 源码：context_window.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/llms/context_window.py)
- [CrewAI 文档：上下文窗口处理](https://docs.crewai.com/en/concepts/agents#context-window-management)

## 与主核 / AI 的关系

压缩与主核里的「缓冲区换出」在动机上同源：资源有限，必须把放不下的部分搬走。但搬的方式相反——主核换出到磁盘，数据**完整保留**、换回来还能继续用；这里的压缩是**有损且不可逆**的，原文彻底丢失，只留摘要。所以主核的换出是安全性措施（不丢失正确性），这里的压缩是可用性措施（用信息损失换取请求能发出去）。

对 AI 工程的实际意义在于：上下文压缩是把「状态管理」问题转成了「信息取舍」问题，而取舍没有正确答案。这段代码的几处选择都值得记住：保留 system 消息（因为那是行为约束）、保留文件引用（因为那是输入数据）、把其余全部压成一条摘要（因为对话历史的价值随时间衰减）。同时它承认了自己的脆弱——压缩失败就继续发，让服务端去拒绝；估算不准就不触发，让 `LLMContextLengthExceededError` 暴露出来。整个设计的底色是「尽力而为，不保证」，这与主核里「要么成功要么明确失败」的取向几乎是镜像相反的。
