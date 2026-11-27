---
project: autogen
feature: single-threaded-runtime
title: 单线程 actor 运行时与消息队列
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py
    symbols: [SingleThreadedAgentRuntime, _process_send, _process_publish]
  - path: python/packages/autogen-core/src/autogen_core/_queue.py
    symbols: [Queue, QueueShutDown]
---

# 单线程 actor 运行时与消息队列

## 一句话职责

把所有智能体的消息处理压进一条 asyncio 任务与一个队列里串行推进，使「谁在什么时候收到什么消息」成为可判定的事实，而不是并发时序的副产物。

## 机制是什么

AutoGen 的运行时不是「多个智能体并行跑」，而是一个 actor 模型的最小实现：所有消息先进同一个队列，运行循环逐条取出并分派。消息分三种信封——发给指定智能体的点对点请求、发到主题的广播、以及请求的回复。

三种信封对应三条处理路径：`_process_send` 处理点对点请求并把结果包成回复信封重新入队；`_process_publish` 处理广播，按订阅解析出接收者后并发调用它们的处理函数；`_process_response` 把回复塞回等待中的 future。整个模型里**只有 publish 的多个接收者是并发执行的**，其余都是串行。

## 怎么实现的

**队列。** Python 3.13 起直接用 `asyncio.Queue`；更低版本用仓内 `_queue.py` 的自实现。自实现补上了上游缺失的 `shutdown()`：置一个关闭标记、唤醒所有等待者、后续 `put` 抛 `QueueShutDown`。这个能力是运行时的停止机制依赖的——`RunContext.stop()` 就是先关队列再等主任务退出。

**点对点请求。** `_process_send` 先检查目标智能体类型是否注册过，未注册直接抛 `LookupError`。然后构造 `MessageContext`（`is_rpc=True`、带 `message_id`），在 `MessageHandlerContext.populate_context` 里调 `agent.on_message`，把返回值包成 `ResponseMessageEnvelope` 再入队。

异常路径分两支：`CancelledError` 只设置 future 的异常（若 future 已取消则跳过）并调 `task_done()`；`BaseException` 设置异常后同样 `task_done()`。两支都**不重新抛出**——错误通过 future 传给调用方，运行循环本身继续跑。这是 actor 模型的关键选择：一个智能体失败不该让整个运行时崩溃。

**广播。** `_process_publish` 先由订阅管理器算出接收者集合，然后跳过发送者自己（源码注释写明「避免把消息发回给自己」）。对每个接收者构造 `is_rpc=False` 的上下文，并把调用包成协程塞进列表，最后 `asyncio.gather` 一次性等待。

这里有一个不对称：`gather` 抛出的异常**不**写回任何 future（广播没有等待方），而是记进 `_background_exception`，并受 `_ignore_unhandled_handler_exceptions` 开关控制。随后 `_process_next` 在下一次循环开头检查这个字段，若有就立刻关闭队列并抛出——也就是说广播里未处理的异常会**终止整个运行**，而不是被吞掉。

**回复。** `_process_response` 只做一件事：若 future 尚未取消，就把消息设成结果。它不检查接收者是否还存在，因为回复的目标就是发起请求时那些 future。

**收尾计数。** `task_done()` 的调用点分布在三条路径上，且 `_process_publish` 把它放在 `finally` 里。这是 `stop_when_idle` 能工作的前提：它靠队列的 `join()` 等到「所有取出的消息都 `task_done`」。

## 关键取舍

**源码事实：** 点对点请求的异常只设置 future、不重抛；广播的异常则记进 `_background_exception` 并在下一轮抛出、导致运行终止。两者处理方式刻意不同，因为前者有明确的等待方可以直接收到错误，后者没有。

**源码事实：** 广播时跳过发送者自己。这让「发布到主题」不会被「自己也订阅了该主题」放大成自回环。

**源码事实：** 广播的多个接收者用 `asyncio.gather` 并发执行，但整体仍在同一个事件循环与同一条主任务里。因此并发是「交错」而非「并行」，单个处理函数里若有 CPU 密集代码会阻塞所有人。

**源码事实：** `save_state` 只保存已实例化智能体的状态，源码注释明确说明**不保存订阅状态**。这意味着从状态恢复后，订阅必须靠重新注册或重新实例化才成立。

**架构推论：** 单线程队列使消息顺序完全确定，这让「日志重放」「按序调试」成为可能。代价是所有智能体的吞吐被一条队列限制，任何慢处理都会形成队头阻塞。

**架构推论：** 把「消息」与「调用」区分成信封类型，而不是直接 `await` 处理函数，使运行时可以在入队前插入拦截、序列化、追踪等横切动作。点对点路径返回 future 而不是直接返回结果，正是为了保持这条「一切经队列」的不变式。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py` | `SingleThreadedAgentRuntime` | 单线程 actor 运行时本体 |
| `python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py` | `_process_send` | 点对点请求处理，异常只写回 future |
| `python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py` | `_process_publish` | 广播处理，并发 gather 并跳过发送者 |
| `python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py` | `_process_next` | 主循环单步：先检查后台异常再取消息 |
| `python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py` | `PublishMessageEnvelope` | 广播信封：主题、发送者、消息 id |
| `python/packages/autogen-core/src/autogen_core/_queue.py` | `Queue.shutdown` | 上游缺失的关闭能力，停止机制依赖它 |

## 参考

- [AutoGen 源码：_single_threaded_agent_runtime.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py)
- [AutoGen 文档：Agent Runtime](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/core-concepts/architecture.html)
- [AutoGen 文档：消息与通信](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/framework/message-and-communication.html)

## 与主核 / AI 的关系

这套「单队列 + 串行分派 + 少数并发」的结构，与主核的写路径有直接可比性：主核把并发写收敛到锁与事务边界，这里把并发收敛到一条队列。差别在于主核必须处理真正的多线程竞争与崩溃恢复，而这里只需在单个事件循环内保持顺序。

对 AI 工程最重要的推论是：**agent 的「并行」在这个模型里只是协程交错**。若一个智能体在做长时间的网络调用（例如一次慢 LLM 请求），同一个运行时的其他智能体仍能推进，因为 `await` 会让出控制权；但若它做同步阻塞调用，整个运行时停摆。理解这条边界，才能判断该用单线程运行时还是换成分片/多进程的部署形态。

对「消息即状态变更」这一点也值得注意：广播没有等待方，异常只能通过 `_background_exception` 传导。这意味着主题订阅式编排里，错误处理必须显式设计，不能指望调用方收到异常。
