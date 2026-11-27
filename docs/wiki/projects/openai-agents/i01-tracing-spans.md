---
project: openai-agents
feature: tracing-spans
title: Span 与 Trace 的抽象与实现
page_type: feature
schema_version: 1
refs:
  - path: src/agents/tracing/spans.py
    symbols: [Span, SpanImpl, NoOpSpan, SpanError]
---

# Span 与 Trace 的抽象与实现

## 一句话职责

把一次可追踪操作定义成「开始/结束 + 类型化数据 + 错误 + 导出载荷」的抽象，并提供三个实现：真记录（`SpanImpl`）、无操作占位（`NoOpSpan`）、以及为恢复场景复用 id 而不重发事件的重建版本。

## 机制是什么

`Span[TSpanData]` 是抽象基类，泛型参数是 span 数据的具体类型。抽象成员分三组：身份（`trace_id`、`span_id`、`parent_id`）、生命周期（`start`、`finish`、`__enter__`、`__exit__`）、载荷（`span_data`、`set_error`、`error`、`export`、`started_at`、`ended_at`、`tracing_api_key`）。另有 `trace_metadata` 有默认实现返回 `None`。

`SpanImpl` 是真实现，用 `__slots__` 固定十个字段：三个 id、两个时间戳、一个错误、一个「前一个 current span 的 token」、一个 processor、一份 span 数据、一个 tracing api key、一份 trace 元数据。

`NoOpSpan` 是占位：id 全是 `"no-op"`，`export()` 返回 `None`，`set_error` 什么也不做。它仍然维护 ContextVar（`start(mark_as_current=True)` 会设 current span），因为「追踪关闭」不该改变调用方的上下文行为。

`SpanError` 是个 TypedDict，只有两个键：`message`（人读描述）与 `data`（可选的结构化上下文）。

## 怎么实现的

**start 的幂等与顺序。** `SpanImpl.start` 先判 `started_at is not None`——已启动就打一条 warning 并直接返回，不重复记录时间也不重复通知 processor。正常路径是：写 `started_at`（ISO 字符串）、调 `processor.on_span_start(self)`、若 `mark_as_current` 则把 span 设为当前并保存返回的 token。

注意 processor 通知在设 current 之前。所以 processor 在 `on_span_start` 里读 `Scope.get_current_span()` 拿到的是**父** span，不是自己。这个顺序让「父节点」在处理器侧天然可得。

**finish 的对称性。** 同理先判 `ended_at`，写时间、`on_span_end(self)`、若 `reset_current` 且有 token 则重置并清掉 token。

**上下文管理器与 GeneratorExit 特例。** `__enter__` 就是 `start(mark_as_current=True)`；`__exit__` 分两支：`exc_type is GeneratorExit` 时走 `_finish_on_generator_exit(self)`，否则 `finish(reset_current=True)`。

`_finish_on_generator_exit` 的 docstring 值得细读——它解释了一个无法完全解决、只能有限容忍的场景：被抛弃的异步生成器由**哪个 task 拿到 `aclose`** 是不确定的，于是 with 块在从未设置 token 的上下文里恢复，`ContextVar.reset` 抛 `ValueError`。Token 只在创建它的 Context 里有效，所以正在收尾的 task 无法改写调用方的上下文，调用方会继续把这个已结束的 span 看作 current 直到自己的作用域结束。抛异常只会在那之上再加一次崩溃，所以选择容忍。

容忍范围被刻意收窄：只在这条路径、且只在 reset 本身。所以显式从错误上下文调 `finish` 仍会抛（那是「上下文所有权违规」而非「无法避免」），processor 在 `finish` 里失败仍然照常暴露，不会被误判成外来 token。

`_finish_on_generator_exit` 自身还有一层：`finish(reset_current=False)` 放在 `try` 里，reset 放在 `finally` 里。这样即使 `finish` 失败（processor 抛错），作用域仍会释放，不会把一个已结束的 span 留作 current。

**export 的字段选择。** `SpanImpl.export` 产出七个基础键：`object`（恒为 `"trace.span"`）、`id`、`trace_id`、`parent_id`、`started_at`、`ended_at`、`span_data`（调 `span_data.export()`）、`error`。

元数据是分开合并的，且顺序有意义：先从 trace 级元数据里**只挑路由键**（`_SPAN_METADATA_ROUTING_KEYS` 只有一个值 `agent_harness_id`），再把 span 数据自带的 `metadata` 字典补进去（跳过已存在的键）。最后只有非空才写进载荷。所以 trace 级元数据不会整份复制到每个 span——只复制与路由相关的那一个键，避免下游把整份 workflow 元数据重复 N 次。

**Trace 侧的对称结构。** `Trace` 抽象与 `Span` 高度相似，但多了一个 `to_json(include_tracing_api_key=False)`：默认不把 api key 写进序列化结果，注释说「避免无意持久化密钥」。`TraceState` 是它的持久化形态，包含 `trace_id`、`workflow_name`、`group_id`、`metadata`、`tracing_api_key`、`tracing_api_key_hash`、`object_type`、`extra`。

**key 的指纹化。** `_hash_tracing_api_key` 用 SHA-256 算指纹，注释说明用途：「只持久化指纹，让恢复的运行能校验用的是同一个显式追踪密钥，而不存密钥本身」。`TraceState.from_json` 里有个细节：即使原始 key 被安全快照剥掉，也保留已存的指纹用于恢复时匹配。

**恢复不重发事件。** `ReattachedTrace` 是第三个 Trace 实现，注释写明「从持久化状态重建 trace 上下文而不重新发出 trace 开始事件」。它的 `start` 只做两件事：标记已启动、把 trace id 记进「已启动集合」。**它不持有 processor、不发 `on_trace_start`**。这是「恢复后续跑」的关键：同一个 trace_id 若重发开始事件，追踪后端会看到两条同 id 的开始记录。

**已启动 id 的环形缓冲。** `_mark_trace_id_started` 把一个有界 `OrderedDict`（上限 4096）当作「最近启动的 trace id」集合，命中时 `move_to_end` 刷新顺序，超限时 `popitem(last=False)` 淘汰最旧的。`_trace_id_was_started` 查询它。这样判断「这个 id 是否真的开始过」是 O(1) 且有内存上界。

**NO_OP_TRACE 是模块级单例。** `NoOpTrace` 被实例化成 `NO_OP_TRACE` 常量复用，因为它无状态（只记录 started 与 token）。

## 关键取舍

**源码事实：** span 的 `__slots__` 固定十项，所以 `SpanImpl` 不能动态加属性。想附加信息只能塞进 `span_data`，而 `span_data` 的具体类型由各自的 `SpanData` 子类定义。

**源码事实：** `SpanImpl.start` 里 processor 通知**早于**设 current，所以处理器看到的 current 是父节点。这个顺序无法靠处理器侧自行推断替代——因为父 span 一旦被设为 current 就再也读不到「之前是谁」。

**源码事实：** `GeneratorExit` 分支的容忍被限制在两条：只在 GeneratorExit 路径、只容忍 reset 的 `ValueError`。这种「窄豁免」比全局吞异常安全得多，因为其他路径上的同类错误仍会暴露真实问题。

**源码事实：** `export()` 里 trace 元数据只复制 `agent_harness_id` 一个键。这说明 trace 级元数据的主要用途不是「广播到每个 span」，而是「给恢复/路由提供上下文」。

**源码事实：** `NO_OP_TRACE` 是共享单例，所以多个「关闭追踪」的 `with trace(...)` 块共用同一个对象。因为 `NoOpTrace` 的 `_started` 与 token 会被后续进入覆盖，嵌套使用会互相干扰——但既然它不记录任何东西，实际影响仅限于 current trace 的指向。

**架构推论：** 三个实现的划分不是「真假」二元，而是按**记录与否**（Impl 与 NoOp）与**是否重发事件**（Impl 与 Reattached）两维交叉。这个划分来自一个真实约束：恢复运行需要正确的 trace id 与上下文，但不能让后端看到重复的开始事件。

**架构推论：** 把「设 current」和「通知 processor」拆成两个可独立控制的动作（`mark_as_current`、`reset_current` 参数），是为了支持手动管理模式：调用方可以先 `start()` 再自行决定何时把 span 设为 current。代价是两处状态（时间戳与 token）必须各自维护幂等。

**架构推论：** `_finish_on_generator_exit` 那段长 docstring 是这套代码里最有信息量的一处注释。它把「异步生成器被抛弃时清理发生在哪个 task 不可控」这个 Python 语义细节，与「ContextVar Token 有上下文归属」这条约束连起来，解释了为什么这里的处理既不完美也不该完美。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/tracing/spans.py` | `Span` | span 抽象：身份、生命周期、载荷、导出 |
| `src/agents/tracing/spans.py` | `SpanImpl` | 真实现，`__slots__` 固定字段并通知 processor |
| `src/agents/tracing/spans.py` | `SpanImpl.start` | 记录起始时间并通知 processor，再设 current |
| `src/agents/tracing/spans.py` | `SpanImpl.finish` | 记录结束时间、通知 processor、按需重置 current |
| `src/agents/tracing/spans.py` | `SpanImpl.export` | 导出七个基础键并按路由键合并元数据 |
| `src/agents/tracing/spans.py` | `NoOpSpan` | 关闭追踪时的占位实现，仍维护上下文 |
| `src/agents/tracing/spans.py` | `SpanError` | 错误消息加可选结构化上下文 |
| `src/agents/tracing/spans.py` | `_finish_on_generator_exit` | 只在 GeneratorExit 路径容忍外来上下文的 reset |
| `src/agents/tracing/traces.py` | `Trace` | trace 抽象，含默认不导出密钥的 `to_json` |
| `src/agents/tracing/traces.py` | `TraceImpl` | 真实现，持 processor 与上下文 token |
| `src/agents/tracing/traces.py` | `ReattachedTrace` | 复用已持久化 id 而不重发开始事件 |
| `src/agents/tracing/traces.py` | `TraceState` | trace 的可持久化形态，含密钥指纹 |
| `src/agents/tracing/traces.py` | `_mark_trace_id_started` | 有界 LRU 记录已启动的 trace id |

## 参考

- [openai-agents-python 源码：tracing/spans.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/tracing/spans.py)
- [openai-agents-python 源码：tracing/traces.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/tracing/traces.py)
- [OpenTelemetry 文档：Span 与父子关系](https://opentelemetry.io/docs/concepts/signals/traces/)

## 与主核 / AI 的关系

主核里的性能追踪也分「记录」与「不记录」两套实现：关闭时的埋点应当是廉价空操作，不能因为没人看就让每次调用背上字符串拼接与哈希表插入。这里的 `NoOpSpan` 与 `SpanImpl` 是同一取舍，但多了一个第三形态——`ReattachedTrace`——解决的是「恢复」问题，主核那边对应的是「从旧日志继续写同一个 trace」，同样要求不重发开始记录。

与 OpenTelemetry 的 Span 相比，这里少了属性和事件（attributes/events/links），只有一个强类型的 `span_data` 与一个 `SpanError`。这是有意的收敛：span 的种类由 SDK 自己定义（agent、generation、function、handoff、guardrail……），每种的载荷字段在 `span_data` 子类里声明，比通用属性字典更可校验。代价是加一个新 span 类型要加一个 data 类。

对 AI 工程最实用的两条：

第一条是 `start` 里「先通知再设 current」的顺序。任何追踪系统都需要处理器能知道「我是谁的孩子」，而这个信息只有在自己成为 current 之前才能读到。把顺序写反的后果是所有 span 都被挂成自己的子节点——一种只在层级报表里才看得出来的静默错误。

第二条是 `_finish_on_generator_exit` 的窄豁免。异步代码里「清理发生在意外的 task」是常态，最省事的做法是在 reset 外面套一个 `except Exception: pass`。这里没有这么做，而是只容忍 `ValueError` 且只在这一条路径上——因为「Token 属于别的 Context」是这个路径的**预期**状况，而其他 reset 失败是真 bug。把容忍范围精确到预期错误，而不是错误类型大类，是让这类兼容代码不吞噬问题的关键。
