---
project: pydantic-ai
feature: capability-hooks
title: 能力钩子与超时
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/capabilities/hooks.py
    symbols: [HookTimeoutError, _HookEntry, _filter_tool_entries]
---

# 能力钩子与超时

## 一句话职责

`Hooks` 把「写一个能力子类」降级成「注册几个普通函数」：一个注册表 + 三种条目（普通、带工具过滤、带事件过滤），每个条目可带超时，派发时按注册顺序串成链。

## 机制是什么

`Hooks` 本身就是一个能力，只是它不靠子类覆写方法，而是持有一张注册表：`dict[钩子名, list[条目]]`。同一个钩子可以注册任意多个函数，派发时依次调用，语义按钩子族不同：

- `before_*` / `after_*` / `prepare_*`：**值透传链**，上一个的返回值是下一个的入参。
- `wrap_*`：**中间件链**，第一个注册的在最外层。
- `on_*_error`：**接力链**，前一个抛出的新错误成为后一个看到的错误。
- `on_event`：观察器，可返回替换事件（已废弃，有告警）。

三种条目类型决定了过滤能力：

| 条目 | 额外字段 | 用于 |
|---|---|---|
| `_HookEntry` | 只有 `func` + `timeout` | 运行、节点、模型请求、输出阶段 |
| `_ToolHookEntry` | 再加 `tools: frozenset[str] \| None` | 工具校验、工具执行阶段 |
| `_EventHookEntry` | 再加 `event_types: tuple[type, ...]` | `on_event` |

## 怎么实现的

**两种注册入口，同一张表。** 构造函数接受全部钩子名作为关键字参数（内部先映射成方法的真实名字：`run` → `wrap_run`、`model_request` → `wrap_model_request`、`tool_execute` → `wrap_tool_execute`、`run_error` → `on_run_error` 等），把非 `None` 的包成条目塞进表。另一个入口是 `hooks.on` 这个装饰器命名空间（`_HookRegistration`，`cached_property` 惰性构造），每个钩子一个方法。

装饰器方法统一走两个辅助函数，它们解决的是「裸用」与「带参数用」的歧义：

- `_bare_or_parameterized(registry, key, func, *, timeout)`：`func` 非空说明是 `@hooks.on.before_run` 这种裸用，直接入表并返回原函数；否则返回一个装饰器，供 `@hooks.on.before_run(timeout=5)` 用。
- `_tool_bare_or_parameterized(...)`：同上，但多一个 `tools` 参数并冻结成 `frozenset`（`None` 保持 `None`，表示不过滤）。

**超时的实现是 `anyio.fail_after` 加线程放弃。** `_call_entry` 是所有派发的唯一出口：有 `timeout` 时用 `with anyio.fail_after(entry.timeout), _utils.abandon_threads_on_cancel():` 包住调用，捕获 `TimeoutError` 后抛 `HookTimeoutError`（`from None`，即不保留原始 traceback 链）。`HookTimeoutError` 同时继承 `AgentRunError` 与 `TimeoutError`——前者让它被当作运行期错误，后者让现有的 `except TimeoutError` 也能接住它。它带三个字段：`hook_name`、`func_name`（取不到 `__name__` 时退到 `repr`）、`timeout`。

`abandon_threads_on_cancel` 的存在说明了一个真实约束：同步钩子跑在 executor 线程里，`fail_after` 只能取消 await，取消不了那个线程。所以「超时」的真实语义是「不再等它」，而不是「终止它」——一个死循环的同步钩子会继续占着线程。

**同步钩子在 executor 里跑，但仍要处理返回协程的情况。** `_call_func` 先判 `is_async_callable`，是就直接 await；否则 `run_in_executor` 后还要过一层 `await_maybe`。注释点明原因：`run_in_executor` 会丢弃一个 `def` 返回的 awaitable，若不处理就静默不执行。

**工具级过滤只看调用名。** `_filter_tool_entries(entries, *, call)` 只排除「是 `_ToolHookEntry` 且 `tools` 非 `None` 且 `call.tool_name` 不在其中」的条目。注意它用的是 `call.tool_name`，即模型实际调用的名字（可能已被 `PrefixedToolset` 加过前缀），而不是 ToolsetTool 的内部键。这对写过滤名单的人是个真实的坑：前缀包装之后，名单要写暴露名。

**wrap 链的构造方向。** 以 `wrap_tool_execute` 为例：先过滤条目，空则直接 `await handler(args)`；否则从空链开始 `for entry in reversed(entries)` 逐个 `_make_wrap_link(...)`。`reversed` 是关键——从最后一个开始包，最终第一个注册的成为最外层。`_make_wrap_link` 生成一个只接收「被包住的那个值」的闭包，把 `handler=inner_handler` 与静态 kwargs（如 `call`、`tool_def`）合进调用。`handler_arg` 为 `None` 的钩子（如 `wrap_run`、`wrap_model_request`）走另一分支，生成无参闭包。

**`on_event` 的替换语义有三个层次。** `Hooks.on_event` 的实现：

1. 先查 `ctx._event_stream_replacements` 里这个事件对象是否已有更早的替换，有就从那个开始——注释解释了为什么：这条链**跨能力**，每个能力都该看见前一个能力已经做出的替换，否则最后跑的那个能力会静默丢掉所有先前替换。
2. 逐条目按 `_event_entry_matches` 过滤后调用；返回非 `None` 且不是自身就当作替换，同时发一条 `PydanticAIDeprecationWarning`，提示改用 `hooks.on.run_event_stream`。
3. 替换只在「原事件不是即时派发的 `CapabilityEvent`」时才记进 map——即时派发的事件没有可替换的流位置。
4. 最后 `await super().on_event(...)`，把子类上用 `@on_event` 标记的方法交给基类派发。

`_event_entry_matches` 被注释特别说明为「派发与 `listens_to` 共用」，所以「这个回调会不会跑」只有一个答案来源；它还兼容了「过滤器出现之前注册的条目」——非 `_EventHookEntry` 一律接收所有事件。

**`@on_event` 是描述符，不是普通装饰器。** `_OnEventMethod` 保存函数与事件类型元组，`__set_name__` 里直接拒绝方法名为 `on_event`（那是派发器本身）。`__get__` 在实例访问时返回 `MethodType` 绑定。`collect_on_event_methods` 按 MRO 逆序收集，用字典按方法名覆盖，所以子类同名方法会替换父类的；结果 `lru_cache` 缓存。`marked_listens_to` 用同一份收集结果判断「这个类是否监听某事件」，从而让派发可以直接跳过整个能力——这也是 docstring 建议「知道类型就写出类型」的原因。

**错误链的接力规则。** 以 `on_tool_execute_error` 为例：按注册顺序遍历，调第一个；若它抛出新异常，把这个新异常当成 `error` 继续给下一个看；全部都没能解决就 `raise error`。`on_model_request_error` 多一条特例：`RetryModelRequest` 立即 `raise`，不当作替换错误——它是控制流（要求换模型再试），某个能力提出它就代表整条链作答了。

`on_tool_validate_error` 的捕获类型限定为 `(ValidationError, ModelRetry)`，而 `on_output_validate_error` 同样。这与 `on_tool_execute_error` 的 `except Exception` 形成对比：校验阶段只把「可重试的校验失败」当接力对象，其他异常直接上抛。

**`handle_deferred_tool_calls` 有累积语义。** 它不像其他钩子那样简单传递：维护一个 `DeferredToolResults` 累加器与一个「剩余请求」视图，每个条目拿到的都是尚未被解决的部分；返回非空就把结果 `update` 进累加器，再用 `remaining.remaining(result)` 收缩剩余请求，全解决就 `break`。一个都没处理过就返回 `None`（而不是空结果），让上层按「无人处理」处理。

**`_has_*` 属性要同时看注册表与子类。** 例如 `_has_wrap_model_request` 返回 `type(self).wrap_model_request is not Hooks.wrap_model_request or bool(self._get('wrap_model_request'))`。注释写明了理由：这些探测不能掩盖子类的覆写，也不能掩盖标记的监听器——注册的函数是「本类自己的贡献」，叠加在基类能力表面之上。同理 `has_on_event` 要额外看 `collect_on_event_methods`。

## 关键取舍

**源码事实：** 三个装饰器族（裸用 / 带 timeout / 带 tools）共用两个辅助函数，靠「第一个位置参数是不是函数」区分。这让两种写法共用同一份注册逻辑，代价是类型签名必须写两套 `overload`。

**源码事实：** 超时只能停止等待，不能终止同步钩子。`abandon_threads_on_cancel` 是明确承认这一点后的处理方式。

**源码事实：** `HookTimeoutError` 同时是 `AgentRunError` 与 `TimeoutError`。前者让它进入运行期错误体系，后者保留与既有 `except TimeoutError` 的兼容。

**源码事实：** 工具过滤用模型侧的调用名。前缀/重命名包装之后，名单必须写暴露名而不是内部名。

**源码事实：** `on_event` 的返回值替换已被标为废弃并告警，替代方案是 `run_event_stream`。但替换机制仍然保留（跨能力传递替换结果），因为历史与序列化格式依赖它。

**源码事实：** 错误钩子的接力语义里，`RetryModelRequest` 是唯一被特判的控制流异常。其他异常都会被当成「可被下一个能力替换的错误」。

**架构推论：** 用注册表而不是子类覆写，把「写能力」的心智负担从「继承 + 覆写 + 记住调 super」降到「写个函数」。代价是失去了类型系统对「你实现的是不是完整契约」的帮助——`Hooks` 只能靠 `overload` 与运行时校验，而子类方式至少还有基类方法的签名可对齐。

**架构推论：** 同一个钩子允许多个函数，于是「顺序」再次成为语义：`before_*` 按注册顺序、`wrap_*` 第一个注册的在最外层。这与 c02 的能力排序是同一件事在两个粒度上的表现——能力之间的顺序由 `get_ordering` 决定，能力内部的顺序由注册顺序决定。

**架构推论：** `_has_*` 用「是否覆写基类方法」判断能力是否实现了某钩子，这条机制对 `Hooks` 必须额外叠加注册表判断。这类「元能力」的代价是每加一个钩子就要同步两处（基类的 `_has_` 属性 + `Hooks` 的覆写）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/capabilities/hooks.py` | `HookTimeoutError` | 钩子超时异常，兼作运行期错误与 `TimeoutError` |
| `pydantic_ai_slim/pydantic_ai/capabilities/hooks.py` | `_HookEntry` | 注册条目基类：函数 + 可选超时 |
| `pydantic_ai_slim/pydantic_ai/capabilities/hooks.py` | `_ToolHookEntry` / `_EventHookEntry` | 带工具名单过滤 / 带事件类型过滤的条目 |
| `pydantic_ai_slim/pydantic_ai/capabilities/hooks.py` | `_filter_tool_entries` | 按模型侧工具名过滤条目 |
| `pydantic_ai_slim/pydantic_ai/capabilities/hooks.py` | `_call_entry` / `_call_func` | 唯一派发出口：超时包装、同步转线程、协程兜底 |
| `pydantic_ai_slim/pydantic_ai/capabilities/hooks.py` | `_bare_or_parameterized` / `_tool_bare_or_parameterized` | 裸用与参数化装饰器的统一注册 |
| `pydantic_ai_slim/pydantic_ai/capabilities/hooks.py` | `_make_wrap_link` | 构造中间件链的一节，决定包住方向 |
| `pydantic_ai_slim/pydantic_ai/capabilities/hooks.py` | `Hooks.handle_deferred_tool_calls` | 累积式延迟调用处理，按剩余请求收缩 |
| `pydantic_ai_slim/pydantic_ai/capabilities/_on_event.py` | `_OnEventMethod` / `collect_on_event_methods` | 标记式事件监听器的描述符与收集，供跳过派发 |

## 参考

- [pydantic-ai 源码：capabilities/hooks.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/capabilities/hooks.py)
- [pydantic-ai 源码：capabilities/_on_event.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/capabilities/_on_event.py)
- [Pydantic AI 文档：钩子](https://ai.pydantic.dev/hooks/)
- [anyio 文档：取消与超时](https://anyio.readthedocs.io/en/stable/cancellation.html)

## 与主核 / AI 的关系

「一张注册表 + 按类型分发的条目 + 每个条目自带超时」这个结构，与主核里的插件/探针注册是同一类设计：都要求多个可选回调在同一挂载点上共存且顺序可控。差别在错误语义——主核的探针一般不允许改变控制流，这里的钩子可以（`wrap_*` 能短路、`on_*_error` 能替换错误、`handle_deferred_tool_calls` 能解决调用）。能力越强，越需要明确的接力规则，这也是为什么每个错误钩子族的捕获类型都被逐条写死。

超时那一节最值得对照主核里的语句超时。主核能在执行器里检查截止时间并主动中止；这里只能停止等待——同步钩子跑在线程里，Python 没有安全的线程终止手段。所以 `abandon_threads_on_cancel` 是「承认做不到」的显式记录。这类妥协在实际系统里很常见，重要的是它被写下而不是被忽略。

对 AI 工程的实际意义在于：钩子是最容易被滥用的扩展点。`Hooks` 的这份实现给出了一条可借鉴的分界线——把「注册」做得很轻（一个函数即可），但把「语义」写死在派发器里（顺序、透传、接力、过滤、超时）。用户不需要理解这五条规则就能用，但一旦排查问题，答案只可能在这五条里。
