---
project: langgraph
feature: timeout-scope
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/_retry.py
    symbols: [_resolve_timeout, _TimedAttemptScope, wait_for_idle_timeout]
---

# 超时解析与尝试范围

## 一句话职责

超时被拆成「硬墙钟上限」与「无进展上限」两种，各自由一个看门狗任务盯着，而「什么算进展」由一个可包装的配置窗口决定。

## 机制是什么

`TimeoutPolicy` 有两个互不替代的字段：`run_timeout` 是单次尝试的绝对墙钟上限，永不被刷新；`idle_timeout` 是「多久没有可观测进展」的上限。第三个字段 `refresh_on` 决定哪些信号能刷新空闲时钟：`"auto"` 认标准图进展信号加显式心跳，`"heartbeat"` 只认显式 `runtime.heartbeat()`。

一次节点执行（一次 attempt，不是一次节点调用——重试会重新计时）对应一个 `_TimedAttemptScope`。这个 scope 的工作是把节点拿到的配置**改写一遍**，让「写入、流式输出、子任务调度、回调事件」这些动作都顺带记一次进展时间戳。超时判定本身则是一个独立的 asyncio 任务：`wait_for_idle_timeout` 反复算「上次进展 + 空闲上限 − 现在」，剩下多少就睡多少，睡醒发现已过期就抛 `asyncio.TimeoutError`。

## 怎么实现的

**归一化。** `_resolve_timeout` 先把 `TimeoutPolicy` 的 `timedelta | float` 两种写法统一成秒（`_timeout_secs`），并且只在 `idle_timeout` 存在时保留 `refresh_on`——没有空闲超时，刷新策略就没有意义。上游 `TimeoutPolicy.coerce` 已经做过一次校验（必须为正数、两个字段不能都为空），`_resolve_timeout` 只负责转单位。

**配置窗口。** `_TimedAttemptScope.wrap_config` 逐个检查四个配置键是否在 `configurable` 里，命中才打补丁：`CONFIG_KEY_SEND` 的写入被包成「持锁检查 active 再写，若 `refresh_on="auto"` 则顺带 touch」；`CONFIG_KEY_STREAM` 换成记录进展的版本；`CONFIG_KEY_CALL` 在 scope 关闭后直接抛 `asyncio.CancelledError`，阻止再起子任务；`CONFIG_KEY_RUNTIME` 用 `runtime.override` 换掉 `stream_writer`，并在需要时同时换上 `heartbeat=self.touch`。最后按 `refresh_on == "auto"` 决定是否再 `merge_configs` 一个 `_IdleProgressCallbackHandler`。

**锁的取舍是逐个决定的。** 写入路径持锁，因为要保证「scope 关闭后不可能再有写入落盘」与 `close()` 严格互斥；流式输出与子任务调度不持锁，理由写在注释里——它们只从事件循环线程触发，active 检查与写入在两次 await 之间是原子的；而回调不能在锁内跑，因为回调会执行任意的用户/运行时代码。

**进展信号的三个来源。** 一是 `_IdleProgressCallbackHandler`，它把二十来个 LangChain 回调钩子全挂到 `_touch` 上，靠 `run_inline = True` 保证按回调发射顺序记录而不被线程池重排；它通过弱引用持 scope，防止比 attempt 活得更久的管理器把 scope 拖住。二是 `_guard_stream` 与 `_guard_stream_writer`，即「产出即进展」。三是 `runtime.heartbeat()`，即用户显式调用。

**为什么弱引用。** 管理器可能在节点结束后仍然存活（比如作为一个长时间运行的父 run 的 run_manager），若强引用 scope，就会让一个早该释放的超时窗口跟着活下来。

**看门狗的执行。** `_arun_with_timeout` 用 `asyncio.wait(..., return_when=FIRST_COMPLETED)` 同时等节点任务与最多两个看门狗。节点先完成就取消并 await 看门狗（`suppress` 掉 `CancelledError` 与 `TimeoutError`——FIRST_COMPLETED 可能两边同时返回，看门狗已经抛了）。看门狗先完成则只有一种情况会转成 `NodeTimeoutError`：它抛的确实是 `asyncio.TimeoutError`。注释特意点明「`task.proc` 自己抛的 `TimeoutError` 原样透传」，不会被误判成框架超时。

**超时发生时的清理顺序。** `scope.close()` → `task.writes.clear()` → 取消后台任务并挂 `_drain_cancelled` 回调。清 writes 是为了让这次尝试的产物不被当成有效结果；drain 是为了不让被丢弃任务的异常在事件循环里打日志。

**进度事件的限流。** 若配了观测回调且有空闲超时，`on_progress` 会被接上，同时把 `progress_min_interval` 设成空闲窗口的四分之一（约每个窗口最多四次事件），避免 token 级的回调把观测端淹掉。限流是「尽力而为」的，注释承认高并发下可能重复发——观测端本来就必须容忍回调从任意线程乱序到达。

**同步节点不支持。** `run_with_retry` 一进去就检查 `task.timeout`，有值就抛 `sync_timeout_unsupported`。理由在错误文案里：同步 Python 执行无法在进程内被安全取消。编译期 `validate_timeout_supported` 本该先拦住，运行期这道是给绕过编译校验的路径兜底。

## 关键取舍

**源码事实：** 超时只在异步车道生效。这不是尚未实现，而是刻意的限制：没有协作式取消点，超时只能靠杀线程，而杀线程会留下持锁的僵尸——比不超时更危险。

**源码事实：** 两类超时共用一个看门狗池但没有优先级。哪个先到就哪个生效，`NodeTimeoutError` 用 `kind` 字段告诉调用方是哪种。

**源码事实：** `refresh_on` 在 `idle_timeout` 为 `None` 时被丢弃。所以「只设 run_timeout 还指定 refresh_on」是无意义配置，框架静默忽略而不是报错。

**架构推论：** 把「进展」定义成「经过被改写的配置的动作」，使超时判定不需要侵入节点代码——节点写状态、发流、调 LLM 都自动算进展。代价是判定标准隐式：一个纯 CPU 计算的节点在 `refresh_on="auto"` 下可能被误判空闲，除非它主动 `heartbeat()`。

**架构推论：** 关闭 scope 与写入的互斥保证了「超时之后不可能再有写入落盘」。这条不变量是中断/恢复正确性的前提：否则一个被判定超时的任务可能在后台继续写检查点，与重试产生的写入打架。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/_retry.py` | `_resolve_timeout` | 把 `TimeoutPolicy` 归一成秒，并决定是否保留 `refresh_on` |
| `libs/langgraph/langgraph/pregel/_retry.py` | `_TimedAttemptScope` | 改写节点配置，把关键动作转成进展信号 |
| `libs/langgraph/langgraph/pregel/_retry.py` | `_TimedAttemptScope.wrap_config` | 四类配置键的条件打补丁与回调注入 |
| `libs/langgraph/langgraph/pregel/_retry.py` | `_TimedAttemptScope.wait_for_idle_timeout` | 空闲看门狗：算剩余时间、睡、过期抛 `TimeoutError` |
| `libs/langgraph/langgraph/pregel/_retry.py` | `_IdleProgressCallbackHandler` | 任意 LangChain 回调即视为进展 |
| `libs/langgraph/langgraph/pregel/_retry.py` | `_arun_with_timeout` | 节点与看门狗 FIRST_COMPLETED 竞速 |
| `libs/langgraph/langgraph/pregel/_retry.py` | `arun_with_retry` | 每次 attempt 建 scope 与进度观测上下文 |
| `libs/langgraph/langgraph/types.py` | `TimeoutPolicy` | 两个超时字段与刷新策略的定义与校验 |
| `libs/langgraph/langgraph/_internal/_timeout.py` | `sync_timeout_unsupported` | 同步节点的统一拒绝错误 |

## 参考

- [LangGraph 源码：pregel/_retry.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_retry.py)
- [LangGraph 源码：types.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/types.py)
- [LangGraph 文档：节点超时与重试](https://langchain-ai.github.io/langgraph/how-tos/node-retries/)

## 与主核 / AI 的关系

「硬上限 + 空闲上限」这套二分在主核里对应的是「语句超时」与「锁等待超时」：前者不看你在做什么，到点就杀；后者只在你没有任何进展时才杀。区别在于主核有真正的中断机制（信号处理 + 检查点），这里只有 asyncio 取消，所以能力被限制在异步车道。

对 AI 侧的实用含义：LLM 调用的长尾延迟用 `run_timeout` 卡住，工具里的长轮询用 `idle_timeout` 配合 `runtime.heartbeat()`——把心跳放在轮询循环里，就能区分「在等」与「卡死」。若不发心跳就让 `refresh_on="auto"` 兜底，但要记得一个不产生任何流式输出、不写状态、不打日志的纯计算函数在框架眼里就是「没进展」。
