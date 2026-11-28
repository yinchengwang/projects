---
project: dify
feature: dify-agent-runtime
title: 外部 Agent 运行时
page_type: feature
schema_version: 1
refs:
  - path: dify-agent/src/dify_agent/runtime/runner.py
    symbols: [AgentRunRunner, AgentRunValidationError]
  - path: dify-agent/src/dify_agent/protocol/__init__.py
    symbols: [__all__]
---

# 外部 Agent 运行时

## 一句话职责

一个可以 `pip install` 的独立 Agent 运行时：它接收一份「运行请求 + 层组合」的 JSON，按组合把原生 Pydantic AI 的模型、工具集、能力装起来跑一轮，并**只把公开的运行事件写进 sink**——本仓的职责是「执行一次运行」，不是「管住一个工作区」。

## 机制是什么

这是 `dify-agent` 这个独立包的核心。它与 `api/core/agent/` 那套（g01）的根本区别是**边界画在哪**：

| 维度 | `api/core/agent/`（g01） | `dify-agent`（本页） |
|---|---|---|
| 部署形态 | 与 API 同进程 | 独立可安装包，`api` 用 `path = "../dify-agent"` 依赖 |
| 执行模型 | 每个执行器各写一个 while 循环 | 一轮 `pydantic_ai.Agent.run()`，循环在框架里 |
| 组合方式 | 从 `app_config` 读固定结构 | 从 `request.composition.layers` **按名字装配** |
| 状态归属 | 会话/消息/思考都在 `api` 的数据库 | 只持久化 `State`，工作区与绑定的退役由 API 拥有 |
| 事件输出 | 直接推 `queue_manager` | 只写 `RunEventSink`，由宿主再转发 |

模块 docstring 把契约写得很死：「Storage-agnostic execution of native Pydantic AI module runs. Validate all JSON data and references before I/O, create fresh native components, and persist only State after capability cleanup. Captured history is saved in `finally`, including interrupted tool responses; current instructions are never stored. Live clients belong to the server lifespan and leases to native `wrap_run` scopes. Workspace and Binding retirement remain API-owned.」

这句话里有五条可以逐条对照实现的约束：

1. **在 I/O 之前完成所有校验** —— 于是有一个专门的 `AgentRunValidationError`；
2. **每次运行创建全新的原生组件** —— 不复用上一轮的对象；
3. **能力清理之后才持久化 State** —— 否则会把已释放的资源写进快照；
4. **`finally` 里也要存历史，含被打断的工具响应** —— 中断不能丢上下文；
5. **当前指令从不存储** —— 只存历史，不存提示词。

## 怎么实现的

**两个类撑起全部：`AgentRunRunner` 与它的异常类型 `AgentRunValidationError`。**

`AgentRunValidationError(ValueError)` 的 docstring 是「Raised when a run request is valid JSON but cannot execute.」——**它专门标记「JSON 合法但跑不了」这个区间**。与之相对的是 JSON 非法（那是协议层的 pydantic 校验失败，在到这里之前就拒了）。这个区分很关键：前者是「请求格式对、组合引用了不存在的层」，属于业务可报错；后者是「报文坏了」，属于协议错误。

**`run()` 是个一次性守卫 + `finally` 收尾。**

```
if self._started:
    raise RuntimeError("a runner can execute its native run only once")
self._started = True
try:
    await self._run()
finally:
    self._terminal_tool_call_limits = self._tool_call_limit_store.seal()
```

**「一个 runner 只能跑一次」是显式的**，不是靠调用方自觉。而 `finally` 里的 `seal()` 保证**无论成功、失败还是取消，工具调用配额的最终快照都会被封存**——否则中断路径上配额就是笔糊涂账。

**事件序列是严格三段：started → stream events → 单一终止事件。**

`_run` 的结构值得逐层拆：

```
try:
    try:
        await emit_run_started(...)
        outcome = await self._run_agent()
    except RunStreamSealedError:
        return                                    # 别人已经封了流，不再补终止事件
    except BaseException as exc:
        failure = exc
        raise
    finally:
        await self._finalize_application_tracing(...)
except Exception as exc:
    if self.is_cancelled(): return
    ... emit_run_failed(...)
```

`RunStreamSealedError` 那条分支是新分布式语义的直接体现：「A terminal transition (typically a cancellation accepted by another process) sealed the stream first; stop without appending a second terminal event.」——**终止事件只允许有一个，而它可能由别的进程发**。所以本进程检测到「流已被封」时只能安静退出，不能再发一个。

`finally` 里那句 `_finalize_application_tracing` 用 `failure` 变量与 `is_cancelled()` 一起判定是否为取消：`cancelled=isinstance(failure, asyncio.CancelledError) or self.is_cancelled()`。**「异常是 CancelledError」与「外部标记已取消」是两条独立线索，任一为真就算取消**——因为原生取消可能被工具抑制，而外部标记又可能早于异常。

**`_run_agent` 是装配段，也是唯一会抛 `AgentRunValidationError` 的地方。**

它的最后是一个把全部校验异常归一的 `except`：

```
except (KeyError, TypeError, ValueError) as exc:
    raise AgentRunValidationError(str(exc)) from exc
```

注意 `ValueError` 也在里面——**pydantic 的校验错误本身就是 `ValueError` 的子类**，所以「层配置不符合该层的 Config 模型」会自动被归进「JSON 合法但跑不了」。

装配的次序是：`load_modules` → `create_modules` → 取 `llm` 模块 `build_model` → 取 `history` 模块 `load_messages` → 取 `output` 模块 `build_output_contract` → 收集 `AbstractToolset` 与 `AbstractCapability` → 造 compaction 能力 → `create_agent(...)`。

四处断言式校验很直白：

- `model_module` 必须是 `llm_module.Capability`，否则 `ValueError("The llm module must provide native model assembly")`；
- `history` 槽位若存在必须是 `history_module.Capability`；
- `output` 槽位若存在必须是 `output_module.Capability`；
- **用户内容为空且没有 eager 知识内容时抛 `EMPTY_USER_PROMPTS_ERROR`**。

**历史加载后会被「清洗」两次：**

```
if message_history:
    message_history = without_historical_knowledge_evidence(message_history)
    message_history = without_historical_read_media_content(message_history)
```

注释写的是「Also strip when knowledge was removed from the new Soul.」——**当这一轮的配置里去掉了知识库/读媒体能力时，历史里对应的证据必须剥掉**。否则会出现「模型看到上一轮的知识库检索结果，而这一轮根本没有那个工具」，行为不可解释。**这是能力变更时的一致性维护，属于很容易漏掉的一类。**

**输出类型由 output 模块提供，缺省是 `str`：**

```
output_type = output.build_output_contract(deps).output_type if isinstance(output, output_module.Capability) else str
```

**追踪插桩失败不会中断运行**，而是显式关闭插桩并把这次调用标为不完整：

```
except Exception:
    agent.instrument = False
    ...
    self.application_observability.exporter.mark_incomplete((run_id, invocation_id))
    logger.warning("Agent tracing instrumentation failed; ...")
```

**这一步的处理方式值得单记：把「可观测性缺失」显式记录成「这次调用不完整」，而不是静默降级。** 否则会有人拿一份缺数据的 trace 当完整数据用。

**超时与工具配额之间有一段很细的配合。** 事件处理函数里每发一个事件都检查：

```
if self.is_cancelled() or timeout.expired():
    raise asyncio.CancelledError
```

而超时的实施是多层保险：

1. `asyncio.timeout(self.run_timeout_seconds)` 包住 `agent.run(...)`；
2. 另注册一个 `loop.call_at(deadline, self.close_tool_admission)` —— 注释是「Close the same owner's admission at the timeout, before a tool can suppress cancellation and reserve another call during native cleanup」；
3. `agent.run` 结束后再查一次 `timeout.expired()`，**若已过期就显式抛 `TimeoutError`**，注释是「Native tools may suppress cancellation while cleaning up. They cannot turn an expired deadline into success.」；
4. 最后把 `TimeoutError` 翻译成 `UsageLimitExceeded`。

这四层合起来防的是一个具体场景：**原生工具在清理阶段会抑制取消信号，若不做额外措施，它就能在「已经超时」之后继续动作**。第 2 层先关掉配额准入（物理上不允许再预留新调用），第 3 层再把「已过期」从「可能被忽略的取消」升级成「明确失败」。

**历史保存在 `finally` 里，并带 `interrupted` 标记：**

```
with capture_run_messages() as messages:
    finished = False
    try:
        ...
        finished = True
    finally:
        if messages and isinstance(history, history_module.Capability):
            history.save_messages(deps, messages, interrupted=not finished)
```

**这与模块 docstring 那条「Captured history is saved in `finally`, including interrupted tool responses」完全对应**。`interrupted=not finished` 让历史层知道这批消息是否完整。

**用量有两个来源，优先「完整」的那个：**

```
complete_usage = model.accumulated_usage if isinstance(model, _HasAccumulatedUsage) else None
self._terminal_usage = _serialize_agent_usage(complete_usage if complete_usage is not None else result.usage)
```

`model` 若能报 `accumulated_usage`（来自 daemon 侧的精确统计），就用它；否则退到 pydantic_ai 的 `RunUsage`。`_serialize_agent_usage` 把两种形态统一成 `AgentRunUsage`——pydantic_ai 的 `RunUsage` 只有 token 数（`input_tokens`/`output_tokens`/`total_tokens`），而 `LLMUsage` 有价格字段，**所以在回退路径上价格信息会丢失**。

`_HasAccumulatedUsage` 是一个 `runtime_checkable` 的 Protocol，只声明一个属性——**用结构化子类型而不是继承来探测能力**，避免了为「能报累计用量」这件事在模型类层次里加一个基类。

**终态快照在 `finally` 里生成，且用量会二次覆盖：**

```
finally:
    self._terminal_session_snapshot = snapshot_modules(deps, registry=self.registry)
    if isinstance(model, _HasAccumulatedUsage):
        usage = _serialize_agent_usage(model.accumulated_usage)
        if usage is not None:
            self._terminal_usage = usage
```

**这段是在「原生 `wrap_run` 作用域已退出（含取消清理）」之后执行的**——注释写明了。所以快照拿到的是清理后的状态，而不是某个中间态。

**成功与失败的分支各自还做了一件额外的事：通知知识模块。**

成功时：

```
if finalization.applied:
    answer = outcome.output if isinstance(outcome.output, str) else json.dumps(outcome.output, ensure_ascii=False)
    for module in self._knowledge_modules:
        await module.publish_investigation(self.services, status="completed", answer=answer)
```

失败时是 `status="interrupted"`，且**只在 `finalization.applied` 为真时才做**。`applied` 表示「这次的终止事件真的被写进去了」——如果另一个进程已经封了流，本进程不该再发一次「已完成」的副作用。**把副作用挂在「事件是否成功落盘」上，是分布式下避免重复动作的正确做法。**

`_knowledge_modules` 是在装配段里筛出来的：`[m for m in modules.values() if isinstance(m, knowledge_fs_module.Capability)]`。

**协议包的 `__all__` 就是对外契约的全部。** `dify_agent/protocol/__init__.py` 的 101 行里，除了 import 就是那份显式清单，包含四组：

- **运行生命周期**：`CreateRunRequest`/`CreateRunResponse`、`RunEvent`（含 `RunStartedEvent`/`RunSucceededEvent`/`RunFailedEvent`/`RunCancelledEvent` 与对应 Data）、`RunStatus`/`RunStatusResponse`、`RunEventsResponse`；
- **组合描述**：`RunComposition`、`RunLayerSpec`、三个层 id 常量（`DIFY_AGENT_MODEL_LAYER_ID`/`DIFY_AGENT_HISTORY_LAYER_ID`/`DIFY_AGENT_OUTPUT_LAYER_ID`）；
- **状态与用量**：`SessionSnapshot`、`AgentRunUsage`、`RunFailureType`、`utc_now`；
- **绑定与文件**：`CreateExecutionBindingRequest/Response`、`DestroyExecutionBindingRequest`、`BindingFile*` 六个、`HomeSnapshot*` 三个。

模块 docstring 特意声明「Stub-specific protocol DTOs live under `dify_agent.agent_stub.protocol` so the run API package boundary stays explicit」——**协议被劈成「运行协议」与「桩协议」两包，是为了让 API 边界显式**。

**这份清单本身就是设计说明**：它暴露了「一次运行需要什么」（请求 + 组合 + 层规格）、「运行产生什么」（事件 + 快照 + 用量）、以及「运行之外还有什么」（执行绑定、绑定文件读写、home 快照）。**后三者不属于一次 run，但同一套协议要能表达它们**——这就是「运行时不拥有工作区，但需要访问工作区文件」的具体形态。

## 关键取舍

**源码事实：** `AgentRunRunner` 一次性（`_started` 守卫），`run()` 的 `finally` 封存工具配额快照。

**源码事实：** 事件序列为 started → 若干流事件 → 唯一终止事件；遇到 `RunStreamSealedError` 时静默返回，不追加第二个终止事件。

**源码事实：** 装配段的 `except (KeyError, TypeError, ValueError)` 统一抛 `AgentRunValidationError`，因此 pydantic 校验失败也被归入「合法 JSON 但不可执行」。

**源码事实：** 历史加载后强制剥掉「知识证据」与「读媒体内容」；当前指令不存储。

**源码事实：** 追踪插桩失败 → `agent.instrument = False` + `mark_incomplete` + warning，不中断运行。

**源码事实：** 超时四层保险：`asyncio.timeout` 包裹、`call_at(deadline, close_tool_admission)`、`timeout.expired()` 复查、把 `TimeoutError` 翻成 `UsageLimitExceeded`。

**源码事实：** 历史保存带 `interrupted=not finished`；用量优先 `model.accumulated_usage`，回退 `result.usage`（后者无价格字段）。

**源码事实：** 知识模块的 `publish_investigation` 只在 `finalization.applied` 为真时调用。

**架构推论：** 「一次运行只创建全新原生组件」这条约束，是把「运行」当成**纯函数式的一段执行**：不依赖上一轮对象的状态，因此可以重放、可以在任意进程上跑、可以把快照作为唯一状态载体。代价是每轮都要重新装配，装配成本进了关键路径。

**架构推论：** 「只持久化 State、只写公开事件」是**可托管运行时的最小接口面**。宿主（`api`）不需要知道 Pydantic AI 的任何类型，只需要读写 JSON 事件与快照。这让运行时的实现可以换（换框架、换语言），而契约不变。**这是把「框架内部类型」挡在协议边界外的正确做法。**

**架构推论：** 「终止事件只能有一个，且可能由别的进程发」意味着**终止是全局状态而不是本地决定**。这比「谁跑谁负责收尾」复杂得多，但它是支持「取消可以由任意副本接受」的前提。所有分布式任务系统最终都要走到这一步。

**架构推论：** 超时的四层保险揭示了一个容易被忽略的事实：**取消信号不是强制终止，而是协作信号**。任何可能抑制取消的组件（这里是原生工具的清理逻辑）都必须用「物理上禁止继续动作」的手段（关掉配额准入）来兜底，否则「已超时但仍在跑」是必然的。

**架构推论：** 把「可观测性缺失」显式标成 `mark_incomplete` 而不是静默降级，是一个值得推广的判断：**数据缺失必须可见，否则会被人当完整数据用**。这与 b01（ragas 忠实度）里「空值返回 NaN 而不是 0」是同一条原则的不同形态。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `dify-agent/src/dify_agent/runtime/runner.py` | `AgentRunRunner` | 一次运行的执行者：装配、跑、发事件、封存快照 |
| `dify-agent/src/dify_agent/runtime/runner.py` | `AgentRunRunner.run` | 一次性守卫与配额封存 |
| `dify-agent/src/dify_agent/runtime/runner.py` | `AgentRunRunner._run` | 事件序列与终止事件唯一性 |
| `dify-agent/src/dify_agent/runtime/runner.py` | `AgentRunRunner._run_agent` | 按层组合装配原生组件并执行 |
| `dify-agent/src/dify_agent/runtime/runner.py` | `AgentRunRunner._finalize_application_tracing` | 追踪收尾，含取消与失败标记 |
| `dify-agent/src/dify_agent/runtime/runner.py` | `AgentRunValidationError` | 「合法 JSON 但不可执行」的专用异常 |
| `dify-agent/src/dify_agent/runtime/runner.py` | `RunSuccessOutcome` | 成功结果的三元组（输出、快照、用量） |
| `dify-agent/src/dify_agent/runtime/runner.py` | `_run_failed_error_payload` | 异常 → （消息、失败类型、结构化原因） |
| `dify-agent/src/dify_agent/runtime/runner.py` | `_serialize_agent_usage` | 两种用量形态归一（回退路径丢价格） |
| `dify-agent/src/dify_agent/runtime/runner.py` | `_HasAccumulatedUsage` | 结构化探测「能报累计用量」 |
| `dify-agent/src/dify_agent/runtime/runner.py` | `close_tool_admission` | 超时时刻关闭配额准入 |
| `dify-agent/src/dify_agent/protocol/__init__.py` | `__all__` | 运行协议的全部对外符号 |
| `dify-agent/src/dify_agent/protocol/schemas.py` | `CreateRunRequest` | 运行请求（含 `composition`） |
| `dify-agent/src/dify_agent/protocol/schemas.py` | `RunComposition` | 层组合描述 |
| `dify-agent/src/dify_agent/protocol/schemas.py` | `SessionSnapshot` | 可持久化的状态载体 |
| `dify-agent/src/dify_agent/protocol/schemas.py` | `RunFailureType` | 失败类型枚举 |
| `dify-agent/src/dify_agent/runtime/modules.py` | `load_modules` | 按组合装配依赖 |
| `dify-agent/src/dify_agent/runtime/modules.py` | `create_modules` | 造出模块实例 |
| `dify-agent/src/dify_agent/runtime/modules.py` | `REGISTRY` | 层注册表 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：dify_agent/runtime/runner.py](https://github.com/langgenius/dify/blob/main/dify-agent/src/dify_agent/runtime/runner.py)
- [Pydantic AI 文档](https://ai.pydantic.dev/)
- [dify 文档：Agent 节点](https://docs.dify.ai/en/cloud/use-dify/nodes/agent)

## 与主核 / AI 的关系

这个运行时的结构与主核里的**可插拔存储引擎 + 统一执行接口**高度对应：引擎（这里是 `AgentRunRunner`）只认一份配置（这里是 `RunComposition`）与一组契约（`AbstractCapability`/`AbstractToolset`），具体实现由外部注册；执行结束后只落盘「状态」（这里是 `SessionSnapshot`），不落盘进程内对象。

**「每次创建全新组件」vs 数据库的每查询独立 plan。** 数据库不会复用上一次的查询计划对象去做下一次执行，而是每次重新构造（虽然计划本身可能被缓存）。dify-agent 同理：`load_modules` + `create_modules` 每轮重来。**这换来的是「同一份请求在任何进程上重放得到同样结果」**，代价是装配成本。

**终止事件唯一 vs 数据库的两阶段提交。** 分布式下「谁有权写最终状态」必须唯一，否则会出现两个终止事件互相覆盖。dify-agent 用「流已封」这个信号来实现，与数据库用「事务 id 已被别的进程终结」判重是同一种思路。**判据必须是一个共享的、原子的可观测事实**，不能是本地推断。

**超时四层保险 vs 数据库的 statement_timeout + 取消请求。** PostgreSQL 的 `statement_timeout` 也只是发取消信号，真正保证「到点停」还需要后端在安全点检查。dify-agent 的第 2 层（到点关闭配额准入）就是那个「安全点检查」的等价物——**它不是停止执行，而是让执行无法再产生新的副作用**。

对 agent 工程五条结论。其一：**运行时应把框架类型挡在协议外**，宿主只读事件与快照，才能换实现。其二：**终止事件必须全局唯一**，判据用共享状态（流封没封）而非本地标志。其三：**超时不能只靠取消信号**，必须叠加「物理上禁止继续动作」的手段。其四：**能力变更后要清洗历史**，否则模型会看到这一轮不存在的工具留下的证据。其五：**可观测性失败要显式标不完整**，不要静默降级。
