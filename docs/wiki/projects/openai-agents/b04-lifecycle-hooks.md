---
project: openai-agents
feature: lifecycle-hooks
title: 生命周期钩子
page_type: feature
schema_version: 1
refs:
  - path: src/agents/lifecycle.py
    symbols: [RunHooksBase, AgentHooksBase]
---

# 生命周期钩子

## 一句话职责

提供两个平行的钩子基类——运行级 `RunHooksBase` 与单智能体级 `AgentHooksBase`——每个都覆盖同样的七个时点，全部是「空实现的可覆写异步方法」，且运行级与智能体级在同一时点并发触发。

## 机制是什么

两个基类的方法集合几乎一一对应，只有命名与参数语义不同：

| 时点 | `RunHooksBase` | `AgentHooksBase` | 参数差异 |
|---|---|---|---|
| LLM 前 | `on_llm_start` | `on_llm_start` | 都是 `(context, agent, system_prompt, input_items)` |
| LLM 后 | `on_llm_end` | `on_llm_end` | 都是 `(context, agent, response)` |
| 智能体前 | `on_agent_start` | `on_start` | 都用 `AgentHookContext` |
| 智能体后 | `on_agent_end` | `on_end` | 都带 `output` |
| 交接 | `on_handoff` | `on_handoff` | 运行级用 `from_agent`/`to_agent`；智能体级用 `agent`/`source`，语义是「我要被交接进来了，来源是谁」 |
| 工具前 | `on_tool_start` | `on_tool_start` | 都是 `(context, agent, tool)` |
| 工具后 | `on_tool_end` | `on_tool_end` | 都是 `(context, agent, tool, result)` |

全部方法体都是 `pass`，没有 `@abstractmethod`。这意味着「覆写你需要的」不是建议而是唯一用法——漏覆写不会报错，只会什么都不发生。

挂钩位置决定了两者的分工：`RunHooks` 是整次运行的观察者（每次运行传一次，`Runner.run(hooks=...)`），`AgentHooks` 挂在智能体上（`Agent(hooks=...)`），随交接自动跟随当前智能体。两个类型别名 `RunHooks`/`AgentHooks` 都特化到 `Agent`，即不支持 `RealtimeAgent` 之外的变体。

## 怎么实现的

**同一点的两个钩子并发触发，不排序。** 每个触发点都写成 `await gather_with_cancel(hooks.on_X(...), agent.hooks.on_X(...) or noop)`。`gather_with_cancel` 用 `ensure_future` + `gather` 起两边，任一方抛错就取消另一方并排空。所以：

- 无法保证 `RunHooks` 先于 `AgentHooks` 执行；
- 一方抛错会连带取消另一方（可能让另一方只执行了一半）；
- 钩子的耗时直接叠加进该阶段。

**智能体级钩子只在「当前智能体」上触发。** 交接时是先切 `current_agent` 再继续循环，`should_run_agent_start_hooks` 被置真，因此下一个 `on_start` 打在新智能体上。`on_handoff` 则是运行级拿 `from_agent`/`to_agent` 两个都看得到，智能体级只有被交接进来的那一个（`source` 是来源）。

**`AgentHookContext` 是「同一时刻的快照」，不是共享的那个 wrapper。** `run_final_output_hooks` 与两处 turn 入口都新建 `AgentHookContext(context=..., usage=..., turn_input=..., tool_input=...)`，然后 `context_wrapper._share_tool_state_with(agent_hook_context)`。所以钩子拿到的是一个新的 wrapper 对象：应用对象与用量按值传入（用量引用同一份），审批与调用记录按引用共享。这意味着在钩子里 `approve_tool` 会作用于真实运行状态，而在钩子里替换 `context` 字段不会影响运行。

**`on_tool_start` 在输入护栏之后触发。** 函数工具的路径是：`_execute_tool_input_guardrails` 返回拒绝消息则直接返回（工具不执行、钩子不触发），只有通过后才 `gather_with_cancel(on_tool_start ...)`，然后才真正 `create_task` 调用工具。也就是说 **护栏拦下的工具调用完全不会出现在 `on_tool_start` 里**，审计日志若只依赖钩子会漏掉被拒的调用。

**`on_tool_end` 在输出落定之后触发。** 函数工具路径先算出 `final_result`、算好 `custom_data` 并挂到输出项上，然后才 `on_tool_end`。计算机/本地 shell/自定义/apply_patch 各条路径的写法一致：`finalize_output()` 里先提交输出项（`tool_output_committer`），再触发 `on_tool_end`。因此钩子里看到的 `result` 就是模型将要看到的那份。

**`on_llm_end` 会被推迟到「响应处理完成之后」。** `run_single_turn` 调 `get_new_response(..., defer_llm_end_hooks=True)`，把触发交给 `after_invocation_validation` 回调；该回调在响应处理的两个出口各被调一次：正常路径（拿到 `ProcessedResponse` 之后）与 `ModelBehaviorError` 路径（重新抛出之前）。两处都无条件跑 `on_llm_end`，而 `on_response_accepted`/`on_response_hooks_started` 只在 `response_accepted` 为真时才跑。所以钩子与「模型返回了一个可解析的响应」对齐，而不是与「HTTP 请求成功」对齐；模型调用本身失败（网络、超时、provider 错误）不会触发它，响应解析失败会触发。

**`on_llm_start` 拿的是过滤后的输入。** 触发点在 `maybe_filter_model_input` 之后，参数是 `filtered.instructions` 与 `filtered.input`。所以 `RunConfig.call_model_input_filter` 的改写对钩子可见——钩子看到的和模型收到的完全一致，而不是原始输入。

**`validate_run_hooks` 会拒绝错类型的钩子。** 传 `None` 返回一个空 `RunHooks`；传 `AgentHooksBase` 实例则抛 `TypeError`，报错文案直接给出修法（attach AgentHooks to an Agent via `Agent(..., hooks=...)`）；传其它非 `RunHooksBase` 也抛。这条校验把「运行级钩子」与「智能体级钩子」的误用变成启动期错误。

**钩子能改工具清单，但不能并发改。** 两个 `on_start` 的 docstring 里用大段文字警告：共享同一个 `Agent` 跑并发运行时，替换 `agent.tools` 会抛 `UserError`（并且是在新解析出的工具到达模型**之前**抛）；原地修改共享列表不被隔离；改共享工具对象（`FunctionTool.is_enabled`、`needs_approval`）既不被隔离也不被检测。给出的替代方案是 `agent.clone(tools=[*agent.tools])` 或用基于上下文的启用/审批回调。实现这一警告的是 `guard_agent_tool_configuration`：它在 `on_agent_start` 两侧各 `snapshot_agent_tools` 一次，中间若有冲突就抛错。

**恢复已序列化状态时钩子不重放。** 同样的 docstring 里还有一条：恢复 `RunState` 时要在加载状态**之前**在传入的智能体上重建由钩子创建的工具，因为序列化状态不含 Python 工具定义，而且「在处理待审批调用之前，start 钩子不会被重放」。这解释了一个容易踩的坑：靠 `on_start` 动态装配工具的智能体，恢复后若没手动重建，审批恢复会失败。

**四个钩子点接收 `ToolContext`，三个接收普通 wrapper。** 工具前后的四个点里，函数工具传的是 `ToolContext`（带 `tool_call_id`、`tool_name`、`tool_arguments`），其它工具族可能只传普通 `RunContextWrapper`——docstring 明写这一点。所以靠钩子做审计时要按 `isinstance(context, ToolContext)` 分支，不能假定总能拿到 call id。而 `on_agent_start`/`on_agent_end`/`on_handoff` 这三个点传的是 `AgentHookContext`，它是空子类，字段与 wrapper 相同。

## 关键取舍

**源码事实：** 两个基类都没有抽象方法。这使「只覆写需要的」成为零成本用法，代价是「方法名写错」不会报错——在 `AgentHooksBase` 子类里定义 `on_agent_end`（正确名是 `on_end`）只是多了一个谁也不会调用的方法，运行照常、钩子静默失效。两个基类的命名不一致（`on_agent_start`/`on_start`）放大了这个风险。

**源码事实：** 七个时点覆盖的是「模型调用前后、智能体切换前后、交接、工具调用前后」，**没有**「本轮开始/结束」「运行开始/结束」「护栏触发」的专用钩子。护栏结果要靠返回值或异常获取，轮次边界只能从 `on_agent_start` 与 `on_llm_start` 的调用次数推断。

**源码事实：** 两个层级并发而非串行，且用 `gather_with_cancel` 而非 `asyncio.gather`。后者在兄弟异常时会留下未完成的兄弟任务；前者会取消并排空。取舍是「钩子失败会导致另一侧钩子被中途取消」，因此钩子里不能有必须完成的副作用。

**源码事实：** `on_llm_end` 的推迟是刻意的：非流式路径下它只在响应被接受后触发。这让「模型调用成功但输出校验失败」这一类失败不会污染「LLM 已完成」的统计——但如果你的监控把 `on_llm_start`/`on_llm_end` 成对计数，就会看到不配对的次数。

**架构推论：** 把钩子设计成「全部可选覆写」而不是「少量必需实现」，使钩子成为纯粹的观测/增强手段而非控制流参与者。真正能改变行为的是工具、交接、护栏、输入过滤器；钩子只能观察，或通过共享上下文（审批、用量）产生有限影响。

**架构推论：** 钩子拿 `AgentHookContext` 而非直接拿 `RunContextWrapper`，是在类型层面留了一个未来扩展点：将来若要给钩子加专属字段（如本轮起始时间），可以只加在子类上而不污染其它用途的 wrapper。目前它是空的，等于「现在没有专属信息，但接口上留了位置」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/lifecycle.py` | `RunHooksBase` | 运行级七个钩子，全部空实现 |
| `src/agents/lifecycle.py` | `RunHooksBase.on_agent_start` | 智能体切换时触发，含共享工具清单的告警 |
| `src/agents/lifecycle.py` | `RunHooksBase.on_handoff` | 交接时触发，带来源与目标 |
| `src/agents/lifecycle.py` | `RunHooksBase.on_tool_start` | 工具前触发，`context` 可能是 `ToolContext` |
| `src/agents/lifecycle.py` | `AgentHooksBase` | 智能体级七个钩子，挂在 `Agent.hooks` |
| `src/agents/lifecycle.py` | `AgentHooksBase.on_start` | 名字与运行级不同（不是 `on_agent_start`） |
| `src/agents/lifecycle.py` | `RunHooks` / `AgentHooks` | 特化到 `Agent` 的类型别名 |
| `src/agents/run_internal/turn_preparation.py` | `validate_run_hooks` | 拒绝把智能体级钩子当运行级传入 |
| `src/agents/run_internal/run_loop.py` | `run_single_turn` 的钩子点 | agent start、llm start、defer 的 llm end |
| `src/agents/run_internal/turn_resolution.py` | `run_final_output_hooks` | 构造 `AgentHookContext` 并并发触发 agent end |
| `src/agents/run_internal/tool_execution.py` | 函数工具的钩子点 | 护栏之后 on_tool_start，输出落定后 on_tool_end |
| `src/agents/run_internal/tool_actions.py` | 原生工具族的钩子点 | 计算机/shell/自定义/apply_patch 各自的 start/end |
| `src/agents/run_internal/agent_tool_configuration.py` | `guard_agent_tool_configuration` | 在 start 钩子两侧快照工具，检测并发改写 |

## 参考

- [openai-agents-python 源码：lifecycle.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/lifecycle.py)
- [Agents SDK API：lifecycle 模块](https://openai.github.io/openai-agents-python/ref/lifecycle/)
- [Agents SDK 文档：运行智能体](https://openai.github.io/openai-agents-python/running_agents/)

## 与主核 / AI 的关系

这套钩子与主核里的「统计回调 / 探针」是同一层：不改变执行路径，只在固定点上报。主核的探针通常要求极低开销且不能抛错（因为在锁内或热点路径上），这里的钩子恰恰相反——可以 `await` 任意慢操作，代价是直接拉长该阶段，并且抛错会连带取消并行的另一侧。判断一个钩子能用在哪，先看它是否在关键路径上。

对 AI 工程有四条可直接用的结论。第一，**钩子不覆盖全部事件**：被输入护栏拦下的工具不会触发 `on_tool_start`，非流式路径下未被接受的模型响应不会触发 `on_llm_end`。要完整审计必须同时读护栏结果与错误处理器。第二，**两个层级并发**，不要指望运行级钩子先跑；需要顺序就合并到一个对象里。第三，**钩子会延长延迟**，把它当同步 RPC 用（比如在钩子里写远程库）会直接放大每次模型调用的 P99。第四，**钩子能改工具但有并发保护**，共享智能体时改工具会抛错，正确做法是克隆或改用上下文回调。

最值得记住的是那条 docstring 警告：序列化状态不含工具定义，恢复前必须手动重建钩子创建的工具。任何「靠钩子动态装配能力」的设计，都要为「恢复」这条路径单独准备一次重放——因为恢复不会自动重跑 start 钩子。
