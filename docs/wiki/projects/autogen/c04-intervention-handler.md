---
project: autogen
feature: intervention-handler
title: 消息拦截与改写
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/_intervention.py
    symbols: [InterventionHandler, DropMessage, on_publish]
---

# 消息拦截与改写

## 一句话职责

在「信封出队」与「处理器执行」之间插一段可改写、可丢弃的钩子，把审批、脱敏、终止判定这类横切动作放到消息层，而不是塞进每个智能体。

## 机制是什么

`InterventionHandler` 是一个 `Protocol`，三个钩子分别对应三种信封：

| 钩子 | 触发时机 | 额外参数 |
|---|---|---|
| `on_send` | 点对点请求出队时 | `message_context`、`recipient` |
| `on_publish` | 广播出队时 | `message_context` |
| `on_response` | 回复出队时 | `sender`、`recipient` |

每个钩子的返回值有两种语义：返回一个对象表示「用它替换消息」，返回 `DropMessage` 表示「丢弃这条消息」。`DropMessage` 是 `@final` 的空类，源码判定写成 `temp_message is DropMessage or isinstance(temp_message, DropMessage)`，所以返回类本身或它的实例都算丢弃。

`DefaultInterventionHandler` 把三个钩子都实现成「原样返回消息」，方便只覆写需要的那一个。

拦截点位于 `SingleThreadedAgentRuntime._process_next` 的模式匹配分支里，也就是**消息刚从队列取出、分派任务尚未创建之前**。

## 怎么实现的

**三种信封装载同一个消息，但每条路径的处置不同。** 这是本机制最需要看清的地方：

| | 处理器抛异常 | 返回 `DropMessage` |
|---|---|---|
| `on_send` | `future.set_exception(e)` 后 `return` | `future.set_exception(MessageDroppedException())` 后 `return` |
| `on_publish` | 记 error 日志后 `return`，**不抛给发布方** | 记 `MessageDroppedEvent` 后 `return` |
| `on_response` | `future.set_exception(e)` 后 `return` | `future.set_exception(MessageDroppedException())` 后 `return` |

点对点与回复都有等待方（`future`），所以错误与丢弃能传回调用方；广播没有等待方，源码在 `on_publish` 的异常分支上留了注释 `# TODO: we should raise the intervention exception to the publisher.`，承认这是未解的问题——一次被拦截失败的广播就此消失，发布方无从得知。

**多处理器不是链式的。** 这是最容易误读的一处。源码事实：`for handler in self._intervention_handlers:` 循环体里调用的是 `handler.on_send(message, ...)`，而 `message` 是模式匹配时绑定的**原始**变量；循环末尾的 `message_envelope.message = temp_message` 只更新信封，不回写 `message`。因此当配置了两个以上处理器时，每个处理器看到的都是同一条原始消息，最终被投递的是**最后一个处理器**的返回值。要把它们串成管道，得让它们自己共享可变状态，或者只配一个处理器。

**返回 `None` 不是「无改动」。** 文档字符串写的是「返回 None 会告警并按无改动处理」，但实现里只有 `_warn_if_none` 发一条 `RuntimeWarning`，随后照样执行 `message_envelope.message = temp_message`——即把 `None` 当成新消息写回信封，消息类型随之变成 `NoneType`。上游 `RoutedAgent` 查 `type(None)` 必然落空，最后走 `on_unhandled_message`。所以在严格模式下，忘记 `return message` 的表现是「消息被静默地变成 None」。

**拦截发生在运行循环里，不是在分派任务里。** 三条 `_process_*` 路径都是 `asyncio.create_task(...)` 起的独立任务，而拦截在 `_process_next` 本体里 `await`。这意味着拦截期间整个运行循环停摆：一个慢的 `on_send`（比如等人审批）会阻塞同运行时其他所有消息的出队。若要做人工审批，代价是整个运行时冻结。

**只有单机运行时支持。** autogen-ext 的 grpc worker 运行时构造函数没有 `intervention_handlers` 参数，`InterventionHandler` 的文档也明写「目前只有 `SingleThreadedAgentRuntime` 支持」。所以它是单机调试/治理能力，不是分布式能力。

**回复路径没有追踪包裹。** 发送与广播分支都用 `trace_block("intercept", ...)` 把拦截过程纳入 OpenTelemetry，回复分支没有这层包裹。观测上会造成「回复拦截不可见」的不对称。

**未完成计数漏记。** 源码事实：`_process_next` 在拦截抛错或丢弃时都是直接 `return`，没有调用 `message_queue.task_done()`；而三条 `_process_*` 处理路径都会调（发送/回复在末尾，广播在 `finally` 里）。队列的未完成计数因此不会归零，`stop_when_idle` 的 `join()` 等不到那一刻。仓内两个涉及丢弃的测试都用 `stop()` 而不是 `stop_when_idle()`，与这一行为吻合。用拦截做丢弃时，必须用 `stop()` 收尾。

## 关键取舍

**源码事实：** 拦截作用在**信封**上，不是作用在智能体的处理器上。因此它对全部消息生效，包括智能体之间互发的消息与外部注入的消息，无需在每个智能体里各写一遍。这是把横切逻辑上移到消息层的直接收益。

**源码事实：** 三种信封的错误通道强度不同，完全由「有没有等待方」决定。点对点与回复能把拦截失败变成调用方可见的异常，广播不能。要让广播也能失败回传，得改信封结构，代价是所有发布路径都要带上 future。

**源码事实：** `DropMessage` 用 `@final` 空类 + 「返回类型本身也算」的判定，使「丢弃」在类型上可与「正常消息」区分，同时不必实例化。这是用类型标记而不是 `None`/`False` 来表达控制流的选择——`None` 已被「消息就是 None」这个合法值占用。

**源码事实：** 处理器列表在运行时构造时固定（`__init__` 直接把参数存进 `_intervention_handlers`），没有增删接口。要换处理器只能重建运行时。

**架构推论：** 把拦截放在出队点而不是入队点，使调用方 `await send_message` 拿到的异常路径统一：无论是处理器内部出错还是拦截拒绝，都通过同一个 future 传出。若拦截发生在入队点，调用方要么同步抛错、要么异步抛错，两种错误风格并存。

**架构推论：** 拦截器是「运行时的横切面」，因此它的粒度天然是整个消息，而不是某个智能体的一次处理。要做「只让某些智能体需要审批」，得靠 `message_context.sender` 与 `recipient` 自己判断，框架不替你筛。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/_intervention.py` | `InterventionHandler` | 三个钩子的协议定义 |
| `python/packages/autogen-core/src/autogen_core/_intervention.py` | `InterventionHandler.on_publish` | 广播钩子，失败无法回传给发布方 |
| `python/packages/autogen-core/src/autogen_core/_intervention.py` | `DropMessage` | `@final` 空类，作为丢弃标记的类型 |
| `python/packages/autogen-core/src/autogen_core/_intervention.py` | `DefaultInterventionHandler` | 三个钩子均原样返回的便利基类 |
| `python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py` | `_warn_if_none` | 返回值是 `None` 时发 `RuntimeWarning` |
| `python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py` | `_process_next` | 拦截的执行点，位于出队与分派之间 |
| `python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py` | `SingleThreadedAgentRuntime.__init__` | 接收处理器列表，之后不可变更 |

## 参考

- [AutoGen 源码：_intervention.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_intervention.py)
- [AutoGen 文档：用拦截器实现终止](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/cookbook/termination-with-intervention.html)
- [AutoGen 文档：工具执行的用户审批](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/cookbook/tool-use-with-intervention.html)
- [AutoGen 源码：_single_threaded_agent_runtime.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py)

## 与主核 / AI 的关系

拦截器与主核里「写入前的策略检查」位置相同：都在对象进入执行路径之后、真正落地之前，用一段可拒绝的钩子承担审计与准入。主核里这类检查通常要求可重入、不容许阻塞，因为它在锁内；这里则恰恰相反——拦截器可以 `await` 任意慢操作，代价是整条运行循环被冻住。

对 AI 工程有三条高价值的判断。第一，这是「人在回路」的落点：工具调用审批、危险操作拦截、终止判定都能在消息层做，不必改智能体代码；但必须清楚代价是运行时冻结，若同时还要继续处理别的消息，得把审批挪到智能体外部的队列。第二，丢弃用 `MessageDroppedException` 回传给等待方，所以调用方可以区分「处理器没处理」与「被策略拒绝」，这是做可观测性时必须分开的两类事件。第三，多处理器不链式这条限制意味着「脱敏 + 审批 + 审计」三件事不能用三个处理器叠加实现——每个都会拿到原始消息，最后一次的返回值才是真正投递的内容，前两个的改写会被覆盖掉。
