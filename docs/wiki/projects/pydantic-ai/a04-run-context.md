---
project: pydantic-ai
feature: run-context
title: 运行上下文与依赖注入
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/_run_context.py
    symbols: [RunContext, OutputSubmitter, EventStreamBuffer]
  - path: pydantic_ai_slim/pydantic_ai/_cancel.py
    symbols: [CancellationToken, RunCancellation]
---

# 运行上下文与依赖注入

## 一句话职责

`RunContext` 是「这一次运行此刻能看到的一切」的载体：依赖、模型、用量、消息历史、工具管理器、取消控制器、事件缓冲都挂在它上面，用 dataclass 做容器、用 `dataclasses.replace` 做逐阶段的窄化副本；`CancellationToken` 是跨线程的取消把手，`RunCancellation` 是单次 run 的取消状态机。

## 机制是什么

依赖注入在这套框架里不是「把 deps 传进函数」，而是「**让 deps 出现在所有需要它的地方**」。`RunContext` 就是那个「处处都在」的对象：工具函数第一参数、输出校验器参数、能力钩子的参数，都是它。它的泛型参数 `RunContextAgentDepsT` 是协变的，因此 Agent 声明 `deps_type=UserDeps` 之后，所有钩子与工具都拿得到类型化的 `ctx.deps`。

更重要的是它是**不可变容器 + 可变内容的混合体**：容器本身用 `dataclasses.replace` 逐层复制（每次钩子拿到自己的副本），但里面的 `messages` 列表、`usage` 对象、`pending_messages` 队列、事件缓冲都是**共享引用**。这就是为什么 `RunContext.messages` 的 docstring 要专门提醒「改这个列表等于改写 run 的历史」。

`CancellationToken` 与 `RunCancellation` 分工明确：前者是用户手里的**一次性把手**（可以给多个 run 共用，一按全灭），后者是每个 run 内部的**状态机**（记录任务、计数、是否已请求、是否超时）。

## 怎么实现的

**`RunContext` 的字段分层。** 按用途分四组：

- **身份与依赖**：`deps`、`model`、`agent`、`_model_id`（内部：解析 `model` 用的选择令牌）。
- **预算与观测**：`usage`、`usage_limits`、`deadline`、`tracer`、`trace_include_content`、`instrumentation_version`、`run_id`、`conversation_id`、`run_step`、`metadata`。
- **消息与注入**：`prompt`、`messages`、`pending_messages`、`_appended_messages`。
- **执行与工具**：`tool_manager`、`tools` / `output_tools` 属性、`retries` / `retry` / `max_retries`、`tool_call_id` / `tool_name`、`tool_call_approved` / `tool_call_metadata`、`partial_output`、`validation_context`。
- **私有运行时**：`_cancellation`、`_event_stream_buffer`、`_pending_immediate_dispatches`、`_event_stream_replacements`、`_output_submitter`、`_run_held_toolsets`、`_durable_operations`、`_run_capabilities_by_id`、`_mcp_tool_defs_cache`、`_capability`、`_dispatched_tool_name`、`_tool_name_path`、`_anchored_evidence`。

`usage_limits` 的 docstring 有一句很值得抄下来的话：运行期**总是**设了这个字段（没传就用默认 `UsageLimits()`，「例如 `request_limit=50`」），只有合成的 `RunContext` 才是 `None`；而且它「是 run 正在实际执行的那个活对象」——所以把它标成 read-only 是有实际意义的警告，改它真的会改到后续请求。

**`deadline` 与 `remaining_time()`。** `deadline` 是 UTC 时区感知的时刻，来自 `timeout=`；嵌套 run 继承外层的 deadline 且**只能变早**。`remaining_time()` 的实现是 `(self.deadline - current_run_clock()()).total_seconds()`——注意 `current_run_clock()` 这个间接层：在耐久工作流里时间必须从引擎的**可重放时钟**读（例如 Temporal 的 `workflow.now()`），否则重放会得到不同结果。

**`OutputSubmitter` 协议。** 只有四个成员：`tools` 属性、`submit(ctx, tool_name, args)`、`record_prepared_tool_defs(tool_defs)`、`discard_submission(tool_call_id)`。后两个方法解释了「提交输出」的完整生命周期：先记录本步 `prepare_output_tools` 钩子处理后的工具定义（否则提交时用的还是声明态），并在某个钩子跳过了该工具调用时**撤销**已提交的输出（`discard_submission`）。`RunContext.output_tools` 与 `submit_output()` 都通过 `_live_output_submitter` 拿到它——而那个属性用 `self.__dict__.get('_output_submitter')` 读，注释说明原因：`TemporalRunContext.__getattribute__` 对缺席字段会抛「请自行序列化」的 `UserError`，但这个提交器是图状态的活视图，永远过不了 activity 边界。

`submit_output` 的 docstring 是全页最长的一段，因为它的语义确实复杂：在工具调用内提交时，输出归属于那次工具调用，工具照常返回、结果照常进历史，而这次调用**被当成该响应里的最终输出工具调用**，随后按 `end_strategy` 决定后续；工具在提交后失败或要求重试则提交被**丢弃**。在工具调用**之外**提交（例如别的钩子）时，run 会在「下一次本会继续的位置」结束。两条路径的失败语义不同：工具内被拒抛 `ToolRetryError` 并**消耗一次输出重试**；工具外被拒抛 `ModelRetry` 且**不消耗输出重试**（因为没有工具调用需要被答复）。

**`EventStreamBuffer` 为什么继承 `list`。** 它的 docstring 给了直白理由：继承 `list` 让图状态的持久化能**透明地**把它序列化；而一个被复活成普通 `list` 的缓冲会**退化为「在流位置处排空」而不是唤醒阻塞的流合并器**。它靠 `__slots__ = ('waiters',)` 加一个 asyncio 事件列表，`append` 覆写为「先 `super().append`，再逐个 `set()`」。这是「数据结构形态必须在序列化前后保持一致」的一个精确案例。

`RealtimeEventBuffer` 是它的实时版：未绑定时攒着，绑定（`bind(sink)`）时把已攒的**按顺序**灌给 sink 并清空，之后每次 `append` 直接投递。注释点明语义：`before_run` 里发出的、在会话连上之前的事件会被保留并**领跑**该会话的流。

**`RunContext.cancel()` 与 `_cancellation` 的写法。** 同样用 `self.__dict__.get('_cancellation')` 读，同样是为了绕开 `TemporalRunContext` 的缺字段报错。取消的语义在 docstring 里写得很清楚：这是**请求**取消，函数正常返回，调用方继续跑到下一个 `await` 才被投递——于是它还能收尾，但返回值（例如工具结果）会被丢弃；而且**取消是终局**，能力钩子只能观察到并清理，不能把 run 恢复成成功；同步跑在工作线程里的代码**无法被强制停止**，可能继续产生副作用。

**`CancellationToken` 的实现。** 三个字段：`_cancelled`、`_registrations: set[RunCancellation]`、`_lock: threading.Lock`。`cancel()` 的形态值得注意：拿锁后若已取消直接返回，否则置位并**快照注册集合**（`tuple(self._registrations)`），出锁后**逐个尝试**取消，任何 `UserError`（事件循环不可达，例如 Temporal 工作流）都记录下来但**不中断循环**——注释说「一个不可达的 run 不该屏蔽其他 run」。若有错误，最后**把 `_cancelled` 复位成 `False`** 再抛：这样同一个 token 再从正确的地方按一次仍然能命中那个漏掉的 run，而被取消过的 run 会忽略重复请求。

`_register(cancellation)` 里有个边界：若 token 已被取消，注册时**立刻**取消该 run（`should_cancel` 分支），否则只是加入集合。

**`RunCancellation` 是状态机。** 字段包括 `_owner`（当前驱动任务）、`_loop`、`_issued: dict[Task, int]`（对每个任务发出了几次 `cancel()`）、`_requested`、`_timed_out`、`_finished`、`_tokens`。

`bind(task=None)` 在**每个步边界**被调用（`AgentRun.__anext__` 里就有），目的是「总是取消当前正在干活的那个任务」。它做三件事：记录 `_owner` 与 `_loop`；若该任务此前已被发出过取消，就用 `min(self._issued[task], task.cancelling())` **夹紧**计数（注释直说这是为了应对用户代码自己 `uncancel()` 的情况，并承认用计数而非身份追踪会留下问题——直接引用了 issue 编号）；若已请求取消且任务还没被发过取消，则**立即重新投递**。

`cancel()` / `expire()` 都进 `_request(timed_out=...)`。这里的桥接逻辑是关键：若「有 owner、有 loop、owner 未完成、且当前没有跑在同一个 loop 上」，就 `loop.call_soon_threadsafe(self._deliver)`；`call_soon_threadsafe` 抛 `NotImplementedError`（例如 Temporal 工作流那种只允许自身代码驱动的 loop）则转成 `UserError`，提示「请从那个 loop 上的代码里取消，比如 `@workflow.signal` 处理器」，并且**不记录请求**，以免下一个步边界又把它投递出去。

`resolve()` 是另一端：在 run 的外缘捕获 `CancelledError` 后，判断「这次取消是不是我们的」。它只 `uncancel()` 自己在 `_issued` 里记过的次数，最后 `return task.cancelling() == 0`——**残留计数就是外部取消，外部优先**。docstring 明确说这是「无基线」策略（不像 `asyncio.timeout()` 用进入作用域时的基线），并解释为什么：「保守方向是外部优先」；还诚实列出了残余窗口：用户若自己 uncancel 掉我们的取消、而外部取消恰好在下一个 `bind()` 之前到达且计数匹配，就会被误判成本方的。

`release_issued()` 是清理口：把从未被 resolve 掉的、以及发给已被取代的驱动任务的取消，逐个数 `uncancel()` 掉，避免污染 `asyncio.timeout()` 与 AnyIO 取消作用域的外层记账。

**`RunBinding` 与 deadline 的传播。** `RunBinding` 是个 dataclass，装 `cancellation`（自建 `RunCancellation`）、`agent_run`（后挂上）、`started_by_installed_package`。它存在的理由写在 docstring 里：`AgentRunEvents` 这个把手在「懒启动的后台 run」之前就已存在，所以由它持有取消控制器，`Agent.iter()` 之后接入活的 run 状态时**保留同一个控制器**。

传递用两个 ContextVar：`_current_run_binding`（`provide_run_binding` 设、`take_run_binding` **消费一次**——注释说消费是为了防止嵌套 run 继承外层把手的绑定）；`_current_run_deadline`（`provide_run_deadline` 设）。deadline 那个多了一个 `active` 标志，注释解释了原因：在 run 内部 spawn 的任务会复制上下文且可能**活得比 run 更久**，所以 run 退出时把 `active` 置假；`_active_run_deadline()` 只认 `active` 为真的那个。`current_run_clock()` 也在这里：继承来的 clock 优先，否则 `_utils.now_utc`。

## 关键取舍

**源码事实：** `RunContext` 是 dataclass，但容器复制 vs 内容共享并存。`messages`、`usage`、`pending_messages`、事件缓冲都是共享引用，因此「拿到的是自己的副本」并不意味「改它只影响自己」。

**源码事实：** `usage_limits` 运行期恒非空，且是 run **正在执行的活对象**。docstring 用「把它当成只读」来劝退修改。

**源码事实：** 取消的语义是「请求 + 在下个 `await` 投递」，不是同步中断。因此取消之后工具仍可能产生副作用，其返回值会被丢弃。

**源码事实：** `CancellationToken.cancel()` 遇到不可达的 run 会**继续取消其他 run**，然后把 token 复位成未取消并抛错。这是「部分成功也不留假状态」。

**源码事实：** `resolve()` 用「剩余 `cancelling()` 计数是否为零」判定归属，策略是**外部取消优先**，且 docstring 承认一个需要用户自己 uncancel 才能进入的误判窗口。

**源码事实：** `EventStreamBuffer` 继承 `list` 只为序列化透明；被复活成普通 list 时行为降级（不唤醒等待者），而不是报错。

**源码事实：** 私有字段访问大量使用 `self.__dict__.get(...)` 而不是 `getattr`，专为绕开 `TemporalRunContext` 对缺席字段抛 `UserError` 的行为。

**架构推论：** 把「一次运行的所有可见状态」压进一个 dataclass，让工具/钩子/校验器的签名统一（都收 `ctx`）。代价是这个类长到一千余行、字段分五六组、还有一批「私有但必须存在」的活对象——它已经不只是上下文，而是 run 的第二张状态表。

**架构推论：** 取消用「取消驱动任务」而非「设置标志位由各层自检」。好处是复用完整的拆除机制（关流、取消工具任务、尽力取消服务端作业、把已完成的工作写进历史）；代价是需要一套计数与 `uncancel()` 记账来区分「谁取消的」，这套记账天然脆弱（源码自己列了残余窗口与 issue 编号）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/_run_context.py` | `RunContext` | 运行期全量上下文：依赖、预算、消息、工具、私有运行时 |
| `pydantic_ai_slim/pydantic_ai/_run_context.py` | `RunContext.submit_output` | 两种提交路径的不同失败语义与重试记账 |
| `pydantic_ai_slim/pydantic_ai/_run_context.py` | `RunContext.cancel` | 经 `__dict__` 取取消控制器，避开 Temporal 缺字段报错 |
| `pydantic_ai_slim/pydantic_ai/_run_context.py` | `RunContext.remaining_time` | 用可重放时钟计算剩余时间 |
| `pydantic_ai_slim/pydantic_ai/_run_context.py` | `OutputSubmitter` | 输出提交协议：工具定义记录与提交撤销 |
| `pydantic_ai_slim/pydantic_ai/_run_context.py` | `EventStreamBuffer` | 继承 list 的序列化透明缓冲，`append` 唤醒等待者 |
| `pydantic_ai_slim/pydantic_ai/_cancel.py` | `CancellationToken` | 跨线程/跨 run 的一次性取消把手，含部分失败复位 |
| `pydantic_ai_slim/pydantic_ai/_cancel.py` | `RunCancellation` | 单 run 取消状态机：绑定、投递、归属判定、清理 |
| `pydantic_ai_slim/pydantic_ai/_cancel.py` | `RunCancellation.resolve` | 消费本方取消，剩余计数即外部取消（外部优先） |
| `pydantic_ai_slim/pydantic_ai/_cancel.py` | `RunBinding` | 把事件把手与懒启动的后台 run 桥接，共享取消控制器 |

## 参考

- [pydantic-ai 源码：\_run_context.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_run_context.py)
- [pydantic-ai 源码：\_cancel.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_cancel.py)
- [Pydantic AI 文档：依赖注入](https://ai.pydantic.dev/dependencies/)
- [Pydantic AI 文档：取消](https://ai.pydantic.dev/agent/#cancellation)

## 与主核 / AI 的关系

`RunContext` 与主核里的「连接上下文 / 事务上下文」是同一类东西：一个在生命周期内被到处传递的对象，装着资源句柄、状态与预算。主核的做法通常是**显式传递**（每层函数都收一个 `ctx*` 参数），这里的做法是**借助签名约定**（所有钩子与工具的第一个参数就是它）。差别在于主核的上下文往往更窄（只带连接与事务），而这里带的东西极多，因为 AI 工具确实需要「当前模型、当前用量、当前消息、当前取消控制器」这些横切信息。

对 AI 工程的实际意义在于：**取消的正确性比想象的难**。这段代码用「取消驱动任务 + 计数记账 + uncancel 归还」这套手法，好处是能复用完整拆除逻辑，坏处是必须自己维护「这次 `CancelledError` 是谁发出的」这一判定，而源码已经诚实列出它的残余窗口（用户自己 uncancel 之后遇到的匹配计数）。工程上的结论是：把取消做成「外部优先」并把本方取消严格记账，即使存在小概率误判，也比把外部取消误当成自己的、然后错误地把 `CancelledError` 翻译成业务异常要安全得多——后者会让上层误以为「run 正常结束了」。
