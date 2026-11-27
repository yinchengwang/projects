---
project: crewai
feature: tool-failure
title: 工具失败分类与策略
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/tools/tool_failure.py
    symbols: [ToolFailureReason, detect_tool_failure, resolve_tool_failure_policy]
---

# 工具失败分类与策略

## 一句话职责

把「工具跑了但没成功」变成可判定的数据：工具自己声明失败、框架给失败分七类、再按「工具 → 任务 → 智能体 → 团队」四级回退决定这一失败是忽略、警告还是中止。

## 机制是什么

这一层解决的是一个非常具体的问题，源码模块的注释开头就写了：Slack 用 HTTP 200 返回 `{"ok": false}`，MCP 服务器用 `isError: true` 标记工具失败——**调用「成功了」，所以错误过去是以普通字符串的形式抵达 agent，整次运行被记成成功**。

方案是「声明式检测」：工具返回一个 `ToolFailure` 对象，框架才知道这次调用失败了。模块注释明确写「这里没有任何东西去猜某段字符串『看起来像』错误」。于是检测函数 `detect_tool_failure` 只有两行：`isinstance` 判断，是就返回，不是就返回 `None`。

失败被分成七个理由（`ToolFailureReason`）：工具自己上报、抛异常、MCP 报错、配额用尽、工具不存在、参数非法、重复调用。前四个是工具/框架侧的，后三个更接近「调用方用错了」。

策略（`ToolFailurePolicy`）只有三档：`IGNORE`（1.16 之前的行为，不记录不发事件不动作）、`WARN`（默认，记录加发事件继续跑）、`RAISE`（记录加发事件后抛 `ToolExecutionFailedError`）。

## 怎么实现的

**`ToolFailure` 的五个字段。** `message`（人读与模型读的说明）、`reason`（分类，默认 `TOOL_REPORTED`）、`code`（失败系统的机器可读标识，例如 `channel_not_found`）、`retryable`（重试是否可能成功）、`details`（工具想保留的额外结构化上下文）。模型配置是 `frozen=True`——失败一旦产生就不可变，避免被下游改写。

`as_agent_message()` 是给模型看的渲染：有 `code` 就输出 `"{message} (code: {code})"`，否则只输出 `message`。所以 `code` 既给机器看，也会被模型看到。

**`ToolFailureRecord` 是「失败 + 现场」。** 除了 `failure` 本身，还带 `tool_name`、`tool_args`、`agent_role`、`task_name`、`task_id`。它落到 `TaskOutput.tool_failures` 和事件总线上，注释说「消费者不必解析字符串就能知道某步失败了」。`summary()` 生成一行日志：`Tool '<name>' failed during '<task>': <as_agent_message>`。

**策略解析的顺序。** `resolve_tool_failure_policy` 按 `(tool, original_tool, task, agent, crew)` 顺序遍历，第一个给出非 `None` 的 `tool_failure_policy` 就赢，全都没有则 `WARN`。这里有两个细节：`original_tool` 是结构化工具上的反向指针，因为「工具级策略在每条原生函数调用路径上都不能被忽略」——注释专门指出了这一点；解析时用 `ToolFailurePolicy(policy)` 做一次枚举转换，`ValueError` 时**只记 warning 并继续**，因为「一个畸形的策略不该让一次工具调用崩掉」。

**一节 ContextVar 收集器。** `_active_failures` 是个 `ContextVar[list | None]`，`tool_failure_collector()` 是上下文管理器：进入时设一个新列表并返回，退出时 reset。`_record_failure` 同时往收集器与 `agent._tool_failures` 追加。注释说明为什么不能只积累在 agent 上：**同一个 agent 可能被并发运行的任务共享**，那样一次执行会擦掉或继承另一次执行的记录。收集器是 ContextVar，asyncio 任务与线程会复制它，所以每次执行只看到自己的；嵌套也安全（护栏重试可以在外层里再开一个）。

`collect_tool_failures(agent)` 优先读活动收集器，没有才回落 agent 上的 `_tool_failures`，并且只挑出 `ToolFailureRecord` 类型的元素、对没有该属性的 agent 也容忍——理由是「读遥测绝不该抛异常」。

**`reportable_failure` 专门服务事件。** 它只做一件事：在 `IGNORE` 策略下把失败变成 `None`。注释解释了必要性：IGNORE 必须真正做到「什么都不浮现」——不记录、不发事件、**也不在 finished 事件上挂一个追踪 UI 会渲染成失败的标记**。

**`handle_tool_failure` 是完整流程。** 算出策略 → `IGNORE` 直接返回 `None` → 否则组 `ToolFailureRecord`（含 agent role、task 名与 id）→ `_record_failure` 落账 → 发 `ToolFailureDetectedEvent` → 若是 `RAISE` 则抛 `ToolExecutionFailedError(record)` → 否则返回记录。

事件构造里有两条注释值得注意。一是「本地 import `crewai.events` 以避免循环依赖」——工具类型与事件类型互引，模块级 import 会成环。二是 `agent_id` **显式传入**而不是走 `from_agent`，因为后者会同时覆盖 `agent_role`，丢掉 `_original_role` 的优先级，而配对的 `ToolUsage` 事件用的正是那个优先级。

**`merge_tool_failures` 的幂等合并。** 按 `(tool_name, failure.message, failure.code, task_id, str(tool_args))` 五元组去重后拼接。注释说明场景：护栏重试会从相互重叠的来源重建输出，所以「只靠对象身份不足以防重复」。

**`failure_from_exception` 把异常转成失败对象。** `message` 取 `str(error)`，空则退化为类名；`reason` 是 `EXCEPTION`；`code` 是异常类名；`retryable` 由调用方给。

## 关键取舍

**源码事实：** 检测严格声明式——返回字符串 "Error: ..." 的工具**不会**被判为失败。这是刻意的，注释明说「工具合法地返回一段关于错误的文本，绝不能被误读成它失败了」。代价是所有现存的、靠返回错误字符串表示失败的工具，升级后仍然会被当成成功，必须改成返回 `ToolFailure` 才生效。

**源码事实：** `RAISE` 抛的 `ToolExecutionFailedError` 携带完整的 `ToolFailureRecord`，所以调用方拿到的不只是一句话，还有 task_id、agent_role、tool_args。这比包一层字符串更能支持上层重试决策。

**源码事实：** 畸形的 `tool_failure_policy` 只警告不报错，且**不会回落到更外层**——循环里 `except ValueError` 之后 `continue` 到下一个来源，所以畸形的工具级策略会被跳过，实际生效的是任务级或更外层。这个行为在「工具上写错了想让它继承」的场景下恰好符合预期，但在「工具上写错其实是想要 RAISE」的场景下会静默降级成 WARN。

**源码事实：** 收集器是 ContextVar 但 `agent._tool_failures` 的追加没有加锁。并发任务下 agent 上的那份列表会被多个执行交错写入——注释也说明了这一点，并把 agent 副本定位为「只为 `last_tool_failures` 服务，best-effort」。

**架构推论：** 七个失败理由中，`USAGE_LIMIT`、`UNKNOWN_TOOL`、`INVALID_INPUT`、`REPEATED_CALL` 都不是工具「执行」产生的，而是调度层产生的。把它们与 `TOOL_REPORTED`、`EXCEPTION`、`MCP_ERROR` 放在同一个枚举里，意味着上层可以用一套策略处理「工具自己坏」与「调用方式不对」两大类，而不必分辨来源。

**架构推论：** 三级策略（IGNORE/WARN/RAISE）与四级作用域（tool/task/agent/crew）构成一张 4×3 的矩阵，但实现是「先解析出唯一的策略，再按三档动作」。这种「先归一再分派」的写法让新增作用域只需改遍历元组，新增动作只需加一个分支——比在每层各写一遍三档判断干净得多。

**架构推论：** `IGNORE` 的存在本身是兼容性设计：它明确标注为「1.16 之前的行为」。也就是说这套机制引入时不能改变老代码的可观测行为，否则所有依赖「工具失败看起来像成功」的上层逻辑都会突然收到异常。把旧行为保留成一个可选值，是新旧共存的常见手法，代价是这个值会长期存在并需要文档解释。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/tools/tool_failure.py` | `ToolFailureReason` | 七类失败理由的枚举 |
| `lib/crewai/src/crewai/tools/tool_failure.py` | `ToolFailurePolicy` | 三档反应策略，默认 `WARN` |
| `lib/crewai/src/crewai/tools/tool_failure.py` | `ToolFailure` | 工具自报的失败，含分类、code、可重试标记 |
| `lib/crewai/src/crewai/tools/tool_failure.py` | `ToolFailure.as_agent_message` | 渲染给模型看的文本，附带 code |
| `lib/crewai/src/crewai/tools/tool_failure.py` | `ToolFailureRecord` | 失败加调用现场，落 `TaskOutput` 与事件 |
| `lib/crewai/src/crewai/tools/tool_failure.py` | `ToolFailureRecord.summary` | 一行日志文本 |
| `lib/crewai/src/crewai/tools/tool_failure.py` | `detect_tool_failure` | 严格 isinstance 检测，不做字符串猜测 |
| `lib/crewai/src/crewai/tools/tool_failure.py` | `resolve_tool_failure_policy` | 四级回退解析，含原始工具与畸形值容错 |
| `lib/crewai/src/crewai/tools/tool_failure.py` | `handle_tool_failure` | 落账、发事件、按 `RAISE` 抛出 |
| `lib/crewai/src/crewai/tools/tool_failure.py` | `reportable_failure` | 让 `IGNORE` 在事件上也不留痕 |
| `lib/crewai/src/crewai/tools/tool_failure.py` | `tool_failure_collector` | ContextVar 级执行隔离的收集器 |
| `lib/crewai/src/crewai/tools/tool_failure.py` | `merge_tool_failures` | 按五元组去重的幂等合并 |
| `lib/crewai/src/crewai/tools/tool_failure.py` | `failure_from_exception` | 异常转失败对象，类名作 code |

## 参考

- [CrewAI 源码：tools/tool_failure.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/tools/tool_failure.py)
- [CrewAI 文档：Tools](https://docs.crewai.com/en/concepts/tools)
- [Model Context Protocol 规范](https://modelcontextprotocol.io/introduction)

## 与主核 / AI 的关系

主核里最直接的类比是错误码体系：SQLSTATE 把「语法错误」「唯一约束冲突」「死锁」分门别类，上层据此决定重试、回滚还是报给用户。这里的七类理由承担同一职能，但覆盖面不同——主核的错误码描述的是**引擎内部**发生了什么，这里的理由还包含「调用方用错了」（未知名、参数非法、重复调用），因为 LLM 就是那个可能用错的调用方。

与 AutoGen 相比差距很明显：AutoGen 的工具失败靠**异常**表达，`ToolAgent` 把 `ToolNotFoundException`、`InvalidToolArgumentsException`、`ToolExecutionException` 三个 `BaseException` 子类转成 `is_error=True` 的 `FunctionExecutionResult`，失败只有「哪一类异常」这一个维度，且没有策略层——一律转成数据交给模型。CrewAI 这边把失败提升成带 `reason`/`code`/`retryable` 的对象，并加了四级作用域的策略解析，还能把「跑完了但没成功」这种情况表达出来（AutoGen 结构上无法表达，因为没抛异常就是成功）。

对 AI 工程最要紧的是那条「不猜字符串」。绝大多数真实工具（HTTP API、MCP 服务、CLI 包装）在失败时返回的是**成功的传输层结果加失败的业务载荷**，如果框架靠关键词匹配判断错误，就会同时犯两种错：把含 "error" 的正常文本判成失败，把 `{"ok": false}` 判成成功。声明式的代价是每个工具都要显式返回失败对象，收益是判定完全可解释。

另一条是 `reportable_failure` 这个看起来多余的小函数。它存在的理由是「IGNORE 要真的什么都不留」——包括不在追踪 UI 里留下一个会被渲染成失败的字段。做可观测性时这类「半留痕」最容易漏：事件发了但内容被清空，前端仍然画一个红点。把「要不要上报」与「要不要执行」拆成两个判断，是干净的处理方式。
