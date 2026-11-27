---
project: crewai
feature: execution-context
title: 执行上下文与追踪标识
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/context.py
    symbols: [ExecutionContext, capture_execution_context, apply_execution_context]
  - path: lib/crewai/src/crewai/execution.py
    symbols: [ExecutionTrace, begin_execution, end_execution]
---

# 执行上下文与追踪标识

## 一句话职责

把「当前在哪」压缩成一个可序列化的快照（任务、flow 方法、事件栈、序列号、平台令牌），并把「这一次运行的追踪身份」绑到一个只在最外层生成的 uuid 上——让嵌套调用共享身份，让检查点能复原位置。

## 机制是什么

这一页有两个对象，都在 `ContextVar` 上工作。

`ExecutionContext` 是个 pydantic 模型，十个字段：`current_task_id`、`flow_request_id`、`flow_id`、`flow_method_name`、`event_id_stack`（元组的元组）、`last_event_id`、`triggering_event_id`、`emission_sequence`、`feedback_callback_info`、`platform_token`。`capture_execution_context()` 从当前上下文的各个 ContextVar 里读出这十项组成快照；`apply_execution_context(ctx)` 反向写回。

`ExecutionTrace` 是个 dataclass，三个字段：`session`（追踪会话）、`cleanup`（退出栈）、`closed`（幂等标记）。它由 `begin_execution` 创建、由 `end_execution` 关闭。运行身份是一个单独的 ContextVar `_current_execution_uuid`。

两者解决的问题不同：`ExecutionContext` 是**空间位置**（我在哪个任务、哪个方法、哪个事件里），`execution_uuid` 是**运行身份**（我属于哪一次运行）。

## 怎么实现的

**上下文快照的十个来源。** `capture_execution_context` 直接读十个 ContextVar：三个来自本模块（当前任务 id、平台令牌）、三个来自 `crewai.flow.flow_context`（flow 请求 id、flow id、flow 方法名）、四个来自 `crewai.events.event_context` 与 `base_events`（事件栈、最后事件 id、触发事件 id、序列号）。它接受一个 `feedback_callback_info` 参数并原样放进快照——这是唯一不从 ContextVar 读的字段，说明它的来源是调用时显式传入。

**上下文恢复的顺序无关性。** `apply_execution_context` 接受 `ExecutionContext` 或字典（后者先 `model_validate`），然后逐个 `set`：任务 id、三个 flow 字段、事件栈、最后事件 id、触发事件 id、序列号（走 `set_emission_counter`，会从给定值之后继续计数）、平台令牌。因为每一项都是独立 ContextVar，写入顺序不影响结果。

**平台令牌的双来源。** `get_platform_integration_token()` 先查 ContextVar，查不到再读环境变量 `CREWAI_ENTERPRISE_ACTION_AUTH_TOKEN`。`platform_context(token)` 是上下文管理器：设置后在退出时 `reset`。所以令牌既可在运行期显式注入，也能从环境继承，且注入优先。

**运行身份的诞生规则。** `begin_execution` 的三条规则写得很清楚：

1. `if _current_execution_uuid.get() is not None: return None`——已有一个活动身份就直接返回 `None`（表示「不是我创建的，我不负责清理」）。这就是嵌套继承：最外层 kickoff 造 uuid，内层（crew 套 flow、AgentExecutor、子 flow）沿用。
2. 若传了 `trace_session` 且追踪被禁用（`_tracing_disabled`），把这个会话以 `GeneratorExit` 关闭并置空——注释说「追踪在两次延迟对话轮次之间被关掉时，要丢弃匿名 span 而不弹同意提示」。
3. 否则若传了有效 `trace_session` 就用它的 `kickoff_id` 作 uuid；否则 `execution_uuid or str(uuid4())`。

最后 `set_execution_uuid` 拿 token，尝试启动或激活追踪；任何异常都先 `clear_execution_uuid(token)` 再重抛——保证失败不留半初始化状态。

**为什么 uuid 在 kickoff 路径上生成而不是在事件总线里。** 模块 docstring 给了理由：**总线处理器跑在 worker 线程上，没法把 ContextVar 发布回用户线程**。所以身份的诞生必须发生在用户调用的那条线程上。

**追踪的两条支路。** `_start_tracing` 里：若已有活动追踪会话或追踪被禁用就直接返回。否则 `should_enable_tracing(override=tracing)` 判定是否启用；未启用但 `should_auto_collect_first_time_traces()` 为真时仍继续（首次运行自动采集）。之后分两支：能解析出凭据（`resolve_tracing_credential()`）就走认证路径——`TraceGrantClient` 申请 grant、`GrantSpanExporter` 导出、`TraceSession` 组装，并在退出栈上挂一个 `finish_authenticated_trace` 回调；解析不出凭据就走 `ephemeral_tracing(uuid, first_time=...)` 匿名路径。

**凭据被拒时的降级。** 认证路径的 grant 请求若抛 `TraceGrantError`，代码**不重抛**，而是记一条 warning 并 `stack.close()` 返回——注释把理由写得很完整：「trace 是运行的记录，不是运行的条件；过期登录或被撤销的令牌不能把运行一起带走。运行继续但不被追踪，并且明确说出来，绝不退回到对已登录用户运行做匿名上传。」

`_untraced_because(error, source)` 生成那条 warning，按状态码与凭据来源给出具体修复建议：401/403 时从 `_CREDENTIAL_FIX` 里查该凭据该叫什么、该怎么修（`pat` → 换一个有效的个人访问令牌、`integration` → 检查环境给的集成令牌、`login` → 重新 `crewai login`）。其他状态码给通用文案。

**还有一条安全判断。** `_evaluation_trusts_this_amp()`：若这次运行是 `crewai eval` 发起的，且本次要追踪到的 AMP 不是 eval 当时批准的那个，则拒绝使用已保存的登录（退回匿名）。注释解释了时序问题：eval 在项目的 `.env` 被加载**之前**就决定了哪个 AMP 可以收到登录，所以 `.env` 后来选的 AMP 只能拿匿名凭据。这是防凭据外泄的一道闸。

**追踪的挂载与卸载。** `_activate_tracing` 建一个 `ExitStack` 进入会话的激活上下文，然后用 `sys.exc_info()` 取当前环境异常一并存进 `_execution_tracing`。注释说明为什么：kickoff 可能本身就被调用在一个 `except` 块里（包括同步 Flow 包装器的事件循环探测），那个环境异常不是这次运行的失败。

`end_execution(token, defer=False)` 只在 `token is not None` 时工作（即「我创建的」）。它取出追踪四元组，用 `sys.exc_info()` 判当前异常；若与记录的环境异常是同一对象就把三元组清空——即「这不是本次运行的错误」。然后 `if defer and error is None and not closed: return lifetime`（把会话交给调用方，用于延迟的对话轮次之间保留追踪），否则 `lifetime.finish(...)`。`finally` 里关激活栈、清 ContextVar、`clear_execution_uuid(token)`。

**`ExecutionTrace.finish` 的幂等。** `if self.closed: return`，然后置 `closed=True`，在 `session.activate()` 上下文里 `cleanup.__exit__(...)`。所以重复调用安全，且清理一定发生在会话激活状态下。

## 关键取舍

**源码事实：** `begin_execution` 在已有身份时返回 `None`，调用方必须能处理 `None`——`end_execution` 就是这么设计的：`token` 为 `None` 时什么都不做。所以「嵌套不清理」是靠 token 的有无表达的。

**源码事实：** `end_execution` 用 `sys.exc_info()` 与启动时记录的环境异常做**对象身份**比较。这依赖 CPython 在同一个 `except` 块内重复调用 `sys.exc_info()` 返回相同对象——这是实现细节，但在 CPython 上成立。

**源码事实：** 凭据被拒时追踪静默降级为不追踪，只留一条 warning。所以「为什么这次没有 trace」可能只能从日志里找到。文案本身写得足够具体（哪个凭据、怎么修），算是把可诊断性补回来了。

**源码事实：** `apply_execution_context` 会用 `set_emission_counter(ctx.emission_sequence)` 恢复序列号，而从检查点恢复的序列号是「上次发射时的值」，所以恢复后第一条新事件的序列号是它的 +1——不会与历史冲突。

**源码事实：** `ExecutionContext` 里带着 `platform_token`，而它会被挂到每个实体上随检查点落盘（`RuntimeState._prepare_entities` 里 `entity.execution_context = capture_execution_context()`）。所以**平台令牌会进检查点文件**。这是一个需要知情的安全边界。

**架构推论：** 把「位置」与「身份」分成两个机制，是因为它们的生命周期粒度不同：位置随每个任务/方法变化，身份整次运行不变。合成一个对象会让「只想知道 uuid」的调用方不得不处理一堆栈状态。

**架构推论：** 「最外层生成、内层继承」用 `ContextVar` 加 `return None` 实现，本质上是把运行身份绑定到**调用栈深度**而不是显式传参。好处是任何深层代码都能读到同一个 uuid，不用层层透传；坏处是跨线程不自动生效——所以那些工具里 `contextvars.copy_context()` 的处理（g02 页）是必需的。

**架构推论：** 追踪失败降级为不追踪、绝不匿名上传已登录用户的运行，这条决策比它看起来重要。匿名上传意味着数据去了一个不一定被授权的地方，而「记录缺失」只是可观测性损失。把两类损失排序之后选了前者，是很清醒的取舍。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/context.py` | `ExecutionContext` | 十个字段的执行位置快照 |
| `lib/crewai/src/crewai/context.py` | `capture_execution_context` | 从十个 ContextVar 读出快照 |
| `lib/crewai/src/crewai/context.py` | `apply_execution_context` | 把快照写回各 ContextVar |
| `lib/crewai/src/crewai/context.py` | `set_current_task_id` | 当前任务 id 的设置与 token |
| `lib/crewai/src/crewai/context.py` | `get_platform_integration_token` | 上下文优先、环境变量兜底的令牌读取 |
| `lib/crewai/src/crewai/context.py` | `platform_context` | 令牌的作用域注入 |
| `lib/crewai/src/crewai/execution.py` | `ExecutionTrace` | 追踪生命周期，可延迟关闭 |
| `lib/crewai/src/crewai/execution.py` | `ExecutionTrace.finish` | 幂等的会话关闭与清理 |
| `lib/crewai/src/crewai/execution.py` | `begin_execution` | 最外层生成 uuid，内层返回 None |
| `lib/crewai/src/crewai/execution.py` | `end_execution` | 只有创建者清理，区分环境异常与本次失败 |
| `lib/crewai/src/crewai/execution.py` | `_start_tracing` | 认证与匿名两条追踪支路 |
| `lib/crewai/src/crewai/execution.py` | `_tracing_disabled` | 显式关闭或环境变量关闭的判定 |
| `lib/crewai/src/crewai/execution.py` | `_untraced_because` | 按凭据来源给出具体修复建议的警告文案 |
| `lib/crewai/src/crewai/execution.py` | `_evaluation_trusts_this_amp` | 防止 eval 场景把登录送到未授权的 AMP |

## 参考

- [CrewAI 源码：context.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/context.py)
- [CrewAI 源码：execution.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/execution.py)
- [Python 文档：contextvars 与线程/任务传播](https://docs.python.org/3/library/contextvars.html)

## 与主核 / AI 的关系

主核里有强对应：一次会话/一个后台进程的「当前事务、当前快照、当前资源属主」就是一整套 ContextVar 类似物，只不过在 C 里它们是显式的全局状态加栈。主核的处理方式是**在每条可能阻塞或跨进程的边界上显式保存与恢复**这些状态；这里靠 `ContextVar` 自动随任务与线程复制。后者省掉了大量样板，代价是「跨线程时不自动生效」这个隐形约束。

与 AutoGen 相比，AutoGen 没有执行上下文快照这一类东西：它的 `MessageContext` 携带 `sender`、`topic_id`、`is_rpc`、`cancellation_token`，作用域是**单条消息**，消息处理完就丢。CrewAI 的 `ExecutionContext` 跨越多条消息、能落盘、能恢复，因为它要支持检查点。代价是它必须知道所有相关 ContextVar 的位置——`capture_execution_context` 里那十个来源横跨三个模块，任何新增的上下文变量都要改这两处函数。

对 AI 工程最实用的一条是「位置与身份分离」。任何长时间运行的 agent 系统都会遇到这两个需求：追踪要看「这次运行整体」（一个 id 串到底），恢复要看「我当时在哪一步」（完整位置快照）。混成一个对象会导致追踪系统被上下文噪声淹没，或者检查点被迫携带运行级元信息。

另一条是 `begin_execution` 的返回 `None` 语义。用「我是否创建了它」决定「我是否负责清理」，比让每层都调一次 `end` 再靠幂等去重更精确——因为嵌套层的 `end` 若真的清理了，外层还在用的追踪会话就断了。这种「创建者负责」的模式在任何资源生命周期管理里都适用。

最后是追踪降级那条决策，以及 `_evaluation_trusts_this_amp` 这道闸。它们共同说明一件事：**可观测性数据的流向本身是一个安全面**。把「记录不到」当成可接受的损失，把「记到不该去的地方」当成不可接受，这个排序值得在自建追踪系统时抄回去。
