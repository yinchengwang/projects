---
project: crewai
feature: flow-runtime
title: Flow 运行时与监听器执行
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/flow/runtime/__init__.py
    symbols: [Flow, kickoff, _execute_listeners]
  - path: lib/crewai/src/crewai/flow/runtime/__init__.py
    symbols: [_execute_start_method, _find_triggered_methods, _execute_racing_listeners]
---

# Flow 运行时与监听器执行

## 一句话职责

`Flow` 是流程的执行引擎：它读定义、建状态、跑入口方法，并在每个方法完成后重新解算「谁被触发了」，按「路由串行、监听器并行、竞态组先完成者胜」的规则把执行推进下去，直到没有新的触发为止。

## 机制是什么

运行时把流程当作**事件传播**来处理，而不是当作图遍历。触发单位的粒度是「方法完成」这个事件：事件的名字默认是方法名，路由方法的返回值也是事件名。一次 kickoff 的骨架是：

1. 恢复或新建状态；
2. 决定跑哪些入口方法；
3. 每个方法执行完后，把由它触发的东西全跑掉；
4. 取最后一个方法输出作为流程结果。

第 3 步是递归的：监听器自己完成后又会触发下一批监听器。终止条件不是「走完图」，而是「这一轮解算不出任何被触发的方法」。

三份运行期状态决定了「谁被触发」：

- `_pending_events`：键是订阅标识（监听器方法名，或入口条件的 `start:<方法名>`），值是「已见到的触发事件集合」。这是 `and_` 条件的累积器。
- `_fired_or_listeners`：已经触发过的多事件 `or_` 监听器集合。这是「`or_(a, b)` 只能被第一个到的事件触发一次」的记录器。
- `_completed_methods`：已完成方法集合。它同时承担「重放时跳过」与「循环流程里允许重跑」两种相反职责。

## 怎么实现的

**类构造：元类先兜住方法。** `Flow` 是 Pydantic 模型，但它的方法字段必须能当普通属性用。`FlowMeta` 在类创建时改写注解表：基类里的非 Pydantic 注解统一标成 `ClassVar`；本类里既没注解又不在父类字段里的可调用对象与流程包装器一律跳过（不变成字段）；与方法同名的 Pydantic 字段则采纳方法对象作默认值并以 `exclude=True` 挂上。最后一句源码注释明确说：静态定义是**惰性**构建的，不在类定义期跑 AST 解析与诊断日志。

**实例初始化：定义 + 方法绑定 + 自动记忆。** `_flow_post_init` 只在第一次执行：取类定义（或 `from_declaration` 传来的定义）→ 若实例给了不同的 `name` 就复制一份定义改名 → 让运行期扩展补全定义 → 绑定方法。方法绑定分两路：声明式流程用 `_action_bound_methods`（把每个 `do` 动作编译成可调用对象，任一动作编译失败就整体抛错而不是留个空壳）；类式流程用 `_class_bound_methods`（`getattr` 取方法，缺失则整体报错并列出所有缺失名）。随后处理持久化后端、建初始状态、装追踪监听器、发 `FlowCreatedEvent`。最后一条容易被忽略：若没显式给 `memory`，会按流程名建一个 `root_scope=/flow/<名>` 的 Memory，`_skip_auto_memory` 的内部流程（RecallFlow / EncodingFlow）例外。

**kickoff 的入口选择。** 这是最反直觉的一段。先取全部入口方法，分成「无条件的」与「有条件的」两组。规则是：

- 有无条件入口时，只跑无条件的——条件入口等自己的条件被满足；
- **一个无条件入口都没有时，把所有入口都跑**。

后一条是为了让「整个流程只由条件入口组成」的写法仍能启动：否则永远没有人发出第一个事件，流程会卡死。选定后再问运行期扩展（`_order_start_methods_for_kickoff`）是否需要改成串行——返回值第二项为真时逐个 `await`，否则 `asyncio.gather` 并发跑。

**同步/异步的双层桥。** `kickoff` 本体不是执行，它是桥：先 `apply_checkpoint` 试恢复（恢复成功就直接把这次调用转交给恢复出的实例，不再走后面）；`stream=True` 就转 `stream_events`；否则在事件总线的 runtime scope 内跑 `kickoff_async`。这里有一个务实的细节：若当前已有运行中的事件循环（说明调用方本身在 async 里却调了同步 API），就把执行搬到单线程池里用独立循环跑，避免 `asyncio.run` 嵌套报错。

**`kickoff_async` 的准备动作。** 依次是：重置工具动作的使用配额（否则同一实例第二次 kickoff 会带着上次耗尽的配额）、把 CEL 的 `now()` 冻结成本次运行的单一时刻（供 d04 的表达式读取）、把 inputs 与 input_files 挂进 OpenTelemetry baggage、在有条件的情况下设置 flow id / 名字 / request id 等 contextvar、进入 runtime scope、装一次性的用量聚合监听器。然后才进钩子链：`EXECUTION_START` → `INPUT`，钩子可以改输入（读回 `payload` 再继续），也可以抛中止异常——中止路径会先把状态 id 打上、把 flow scope 打开再重新抛，以保证失败事件能与开启事件配对。

**状态的三条来源。** 进主体后先决定这次是「新跑」还是「续跑」：给了 `id` 且有持久化、或刚从检查点恢复，就算续跑；新跑要清空已完成方法、输出、待定事件、已触发的 `or_` 监听器、方法调用计数。接着是 `restore_from_state_id` 的分支（从别的流程的快照补水，并换一个新 id 以免污染源流程的持久化历史），再是「给了 id 就从存储加载状态」。最后才用过滤掉 `id` 的其余 inputs 初始化状态。这三条来源的优先级写得很明确：fork 成功就跳过按 id 加载，`_skip_persistence_restore` 为真也跳过（活适配器自己已经恢复过）。

**执行方法：从调用到持久化的一条直线。** `_execute_method` 做这些事：发 `MethodExecutionStartedEvent`；`PRE_STEP` 钩子（允许改参数，位置参数用 `_0`/`_1` 这种键传递，钩子返回后再反向还原）；把方法名塞进 contextvar（供 `ask()` 读取）；异步方法直接 `await`，同步方法丢进线程池并用 `copy_context()` 带上上下文——源码注释说明这是为了让 `Agent.kickoff()` 能在同步流程方法里正常跑；同步方法返回协程时自动 `await`；跑人工反馈步骤（若定义了）；`POST_STEP` 钩子；把 `{"method", "output"}` 追加到 `_method_outputs`；累加执行计数与已完成集合；在线程里持久化；最后发 `MethodExecutionFinishedEvent` 并返回它的 event_id（用于因果链）。

失败分支也在此处：`HumanFeedbackPending` 被识别为「暂停」而不是「失败」，会发 `MethodExecutionPausedEvent`，其他异常发 `MethodExecutionFailedEvent` 后重新抛出。

**监听器解算：先路由后监听。** `_execute_listeners` 是引擎的心脏，分两段。

第一段是**路由循环**：反复问「当前触发事件能激活哪些路由方法」，有就依次执行；路由返回值（枚举取 `.value`）变成新的触发事件，继续循环。这个 while 没有显式上界，靠「路由链有限」+ 全局方法调用计数兜底。若路由返回 `None`，触发事件被置成空串，循环自然结束。

第二段是**监听器并行**：把「原触发事件 + 所有路由产出的事件」组成一个待处理队列，逐个事件解算监听器列表，然后**并发**执行。这里有两个例外：

- 若这批监听器构成一个**竞态组**，走 `_execute_racing_listeners`：组内并发起跑，第一个完成者胜，其余任务立即 `cancel()`；同批的其他非竞态监听器照常并发并 `gather(return_exceptions=True)`。
- 若当前事件是路由产出的事件，还要额外检查「条件入口」是否被它满足，满足就跑那个入口方法——这打通了「条件入口」与「路由标签」之间的连接。

**竞态组的构造规则**（`_build_racing_groups`）比看上去严格，源码注释逐条解释了理由：只有**多事件 or_** 的监听器参与竞态；只有**专属于该监听器**的备选事件才进组（若某事件还被 `and_` 监听器消费，取消它会让那个 AND 永远不可满足）；挂在 `and_` 分支下的 or 子项不是「备选」，不参与。结果缓存在 `_racing_groups_cache`，每次调用按「与当前监听器集合的交集是否大于 1」来判断是否命中。

**条件判定是有状态的。** `_condition_met` 先按订阅键把本次触发事件并入已见集合，再用 `_condition_satisfied` 递归求值（`and` 用 `all`、`or` 用 `any`），满足则**删除该订阅键**——条件用掉即清空，所以同一组 `and_` 条件在下一次通过时需要重新累积齐全部事件。

**`or_` 的重置与重臂。** 多事件 `or_` 监听器一旦触发就进 `_fired_or_listeners`，此后 `_find_triggered_methods` 会跳过它。为了让循环流程能再触发，引擎在两处重置：路由发出新信号时，`_rearm_or_listeners_for_trigger` 把「条件里引用了这个触发事件」的已触发监听器移出集合（若传了 `rearmable` 集合则只在其范围内重置，避免越界复活）；方法重跑时（从 `_completed_methods` 里移除）会 `_clear_or_listeners()` 清空全部。源码注释解释了为什么不能只丢掉那一个监听器：下游的 `or_`（例如监听 `or_(handler_a, handler_b)`）会跨轮次持续被抑制。

**重放与循环的耦合。** `_execute_start_method` 与 `_execute_single_listener` 都先看 `_completed_methods`：

- 若在集合里且 `_is_execution_resuming` 为真，跳过执行但**继续跑它的下游监听器**——这是恢复场景，已完成的方法不该重跑，但它的产出仍要推进后续；
- 若在集合里且不是恢复，就把方法从集合移除并清空 `or_` 记录——这是循环场景，同一方法被允许再跑一轮。

区别全靠一个布尔标志，所以恢复期间的「循环重入」需要临时把标志翻假（条件入口重跑那段就是这么做再翻回来）。

**无限循环的兜底。** `_execute_single_listener` 开头累加 `_method_call_counts`，超过 `max_method_calls`（契约默认 100）抛 `RecursionError`，报错文案直接点名最常见成因：「`@listen` 的标签等于方法自己的名字」。

**结果与收尾。** 全部入口跑完后清掉恢复标志，取 `_method_outputs` 最后一项作为流程结果，过 `OUTPUT` 与 `EXECUTION_END` 钩子（后者在成功路径上先置「已派发」标记再派发，避免钩子抛中止时又触发一次失败派发），排空后台记忆写入与事件总线，最后发 `FlowFinishedEvent`。`_should_defer_trace_finalization` 为真时既不发出也不收尾——留给多轮会话在结束时统一收口。

## 关键取舍

**源码事实：** 入口选择规则里「没有任何无条件入口就跑全部入口」是一条纯实用规则，不是语义推导。它把「流程只有一个条件入口」这种写法从死锁里救出来，代价是这种流程的启动语义与「有条件入口 + 无条件入口」的流程不同。

**源码事实：** 路由串行、监听器并行。路由的返回值决定下一跳，若并发跑两个路由就会出现「两个下一跳」的歧义；监听器之间没有顺序依赖，所以并发。这个不对称是有意的。

**源码事实：** `and_` 条件的累积是有状态的、且满足后即清空。这意味着「同一组 `and_` 条件不能同时服务两个下游」——因为订阅键是监听器名，一个监听器一个累积器。

**源码事实：** 竞态组只对「专属备选事件」生效。这是一条为了不破坏 `and_` 语义而加的约束，代价是「`or_` 与 `and_` 共用事件」时没有竞态优化，可能起跑多个最终只有一个有用的分支。

**源码事实：** 同步方法一律进线程池执行，并复制 contextvars。这让同步 SDK 与异步编排能混用，代价是每个同步方法有一次线程切换，且线程池里的代码不能依赖事件循环上的东西。

**架构推论：** 引擎的终止条件是「解算不出新触发」，而不是「所有方法已执行」。因此流程的图结构只是触发关系的载体——悬空的方法（没人触发、自己也不是入口）永远不会跑，而不报错。这与主核「不可达的计划节点被优化掉」形成对照：那边有规划器主动剔除，这里是被动地永不触发。

**架构推论：** 把「已完成方法」同时用于重放跳过与循环允许重跑，靠一个恢复标志区分，是一种用最少状态覆盖两种相反语义的写法。代价是任何新增的重入路径都必须正确维护这个标志，源码里已经出现两处「临时翻转再翻回」的补丁。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/flow/runtime/__init__.py` | `Flow` | 引擎主体：定义绑定、状态、钩子、执行、持久化接入 |
| `lib/crewai/src/crewai/flow/runtime/__init__.py` | `kickoff` | 同步桥：检查点恢复、流式分流、事件循环兼容 |
| `lib/crewai/src/crewai/flow/runtime/__init__.py` | `_execute_start_method` | 执行入口方法并推进其下游；重放跳过与循环重跑的分支 |
| `lib/crewai/src/crewai/flow/runtime/__init__.py` | `_execute_listeners` | 先串行解算路由链，再并行执行监听器，含竞态组分支 |
| `lib/crewai/src/crewai/flow/runtime/__init__.py` | `_find_triggered_methods` | 按 router 标志筛选并按条件判定，维护已触发 or 集合 |
| `lib/crewai/src/crewai/flow/runtime/__init__.py` | `_execute_racing_listeners` | 竞态组先完成者胜，其余取消 |
| `lib/crewai/src/crewai/flow/runtime/__init__.py` | `_condition_met` | 带累积与用后清空的条件判定 |
| `lib/crewai/src/crewai/flow/runtime/__init__.py` | `_build_racing_groups` | 计算竞态组：仅限多事件 or 的专属备选 |
| `lib/crewai/src/crewai/flow/runtime/__init__.py` | `_execute_method` | 单方法执行：钩子、线程隔离、事件、持久化 |

## 参考

- [CrewAI 源码：flow/runtime](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/flow/runtime/__init__.py)
- [CrewAI 文档：Flows 概念](https://docs.crewai.com/en/concepts/flows)
- [Python 文档：contextvars](https://docs.python.org/3/library/contextvars.html)

## 与主核 / AI 的关系

这套「事件传播」的推进方式与主核的迭代式执行器是同构的：都有一个「取下一批可执行单元」的循环，都在每次完成后重新解算，而不是预先排好完整序列。差别在可预测性——主核的下一批由计划树与谓词确定，同一输入必然同一序列；这里的下一批取决于方法返回值（可能是 LLM 产出），因此同一输入两次运行可能走不同分支。这也解释了为什么整套运行时旁边必须配事件记录与检查点：没有它们，一次异常的分支根本无法复盘。

竞态组的设计值得单独对照主核里的「多路索引扫描取第一个结果」：两者都是「并发起跑、先到者胜、其余取消」，都有资源浪费的可能，都靠「只让专属于本组的候选参与」来限制浪费范围。区别在取消语义——主核取消的是扫描任务，取消后无副作用；这里取消的是流程方法，若方法已经产生部分副作用（写了状态、发了事件），取消不会回滚。所以源码里竞态组被刻意限制在「多事件 or 的专属备选」这种小范围。

对 AI 工程的实际意义在于：把「编排」写成定义、把「推进」写成事件传播，能得到很大的灵活性（动态分支、循环、人工反馈插入），但会失去静态可预测性。运行时的这些兜底——入口选择的实用规则、`max_method_calls` 上限、有状态的 `and_` 累积、`or_` 的按需重臂——本质都是在用局部规则给动态图补上可终止性与确定性，每一条都对应一类现实中踩过的坑。
