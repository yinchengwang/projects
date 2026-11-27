---
project: pydantic-ai
feature: tool-manager
title: 工具管理器与工具裁决
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/tool_manager.py
    symbols: [ToolManager, ValidatedToolCall]
  - path: pydantic_ai_slim/pydantic_ai/tool_manager.py
    symbols: [_decide_which_tools_stay, _first_on_a_cycle]
---

# 工具管理器与工具裁决

## 一句话职责

`ToolManager` 是「一步之内」的工具裁决者：它缓存本步的工具集快照、裁决同名替代品（内置工具取代本地工具）、把校验与执行拆成两个可独立观察的阶段，并持有本步的重试账本。

## 机制是什么

每进入一个 run step，就会新建一个 `ToolManager`（或在同一 step 内可用性变动时重建），它做四件事：

1. **取工具快照**。`toolset.for_run_step(ctx)` 之后 `get_tools(ctx)`，得到本步模型能看到的工具全集。
2. **裁决替代关系**。`_drop_superseded_tools` 剔除那些「已被另一个工具的 kind 取代」的工具——这就是「模型支持内置搜索时，本地同名工具不上线」的实现位置之一。
3. **分发校验与执行**。`validate_tool_call` 与 `execute_tool_call` 是两个独立方法，中间夹着能力钩子；`ValidatedToolCall` 是它们之间的载体，明确区分「校验失败」与「执行失败」。
4. **记重试账**。`failed_tools` / `succeeded_tools` / `retries` / `availability_refused` 四个集合驱动跨步的重试预算结转。

## 怎么实现的

**同一步内可能重建，判据是「可用性有没有变」。** `for_run_step` 先算 `frozenset(ctx._dispatch_active_capability_ids)` 与缓存的 `resolved_capability_ids` 比较：相等且 `run_step` 相同则直接返回自身。否则要么同步重建（此时**不**重复执行重试结转，注释说明否则一次失败会烧掉两格预算），要么跨步重建（结转规则是：成功过的工具清零，失败过的 +1）。

注释解释了为什么可用性也是解析的输入而不只是执行的门槛：「一个延迟能力只有在激活之后其 `prepare_tools` 才会被派发，所以在该能力未激活时解析出的工具集是不受它过滤的；只按 `run_step` 缓存会把这份未受治理的工具集交给一个此后已可用的派发——一个权限过滤器静默地没有运行」。

`_release_context` 在返回新管理器前把旧 ctx 的 `tool_manager` 置空，注释说这是为了在保留「工具或钩子持有的 ctx」的同时打断管理器与其 ctx 的循环引用。

同一步内重建与跨步重建还有一处行为差异：`AbstractToolset.for_run_step` 标记的是**步边界**而不是重新解析，所以跨步时调用它（`DynamicToolset` 会在那里退出再进入内层工具集、重跑工厂并丢弃已进入工具集的状态），而同一步内复用已经过渡过的工具集实例、只重跑 `get_tools`——那才是重跑 `prepare_tools` 的动作。另外 `failed_tools` / `succeeded_tools` 只在跨步时重置，同一步内沿用，注释说明丢掉它们会让下一步结转时少算。快照顺序是 `get_tools` → `_check_tool_kinds` → `_drop_superseded_tools`，即先验工具 kind 的形状，再裁决替代。

**同名替代的裁决是一套确定性算法。** `_drop_superseded_tools` 的关键点：

- 输入是**全部**已收集的工具，包括尚未揭示的延迟工具——这样「揭示一个工具不会在一轮中途改变答案」。
- `_suppliers_of_named_kinds` 建立「谁声明了 `unless_tool_kind`，以及该 kind 有哪些工具提供」。
- `_decide_which_tools_stay` 用不动点迭代：若某工具的某个提供者已确定留下，它就出局；若它的提供者全部确定出局，它就留下。
- 迭代卡住时说明存在环（每个未决工具都有未决的提供者），`_first_on_a_cycle` 找出「沿未决提供者能回到自己」的第一个工具并钉住它留下，同时把它命名的那些工具标记出局。
- 最后 `_restore_tools_without_a_replacement` 做一次反向修复：**一个出局的工具若没有任何提供者留下，就把它恢复**。注释说明了这条的价值：三者互相取代（A unless B, B unless C, C unless A）这种环里，两条规则无法同时满足，此时宁可让「工具与它命名的工具并存」（只是冗余），也不要「丢掉功能且没有替代品」。

结果对工具集顺序确定。一处细节：`_shape_mismatch` 带 `lru_cache(maxsize=256)`，因为它要对 JSON schema 做较重的结构比对。

**`ValidatedToolCall` 为什么要单独存在。** docstring 给了三条理由：知道校验是否通过再去执行、事件里能给出准确的 `args_valid`、能把校验失败与执行失败分开处理。它的字段设计体现了这个拆分：`args_valid`、`validated_args`、`validation_error`（可能是 `ToolRetryError` 或 `ToolFailedError`）、以及 `deferral`。

`deferral` 是一条容易误解的语义：当 `args_validator` 或校验钩子在**参数已经合法之后**抛 `ApprovalRequired` / `CallDeferred`，这属于控制流而非校验失败，所以 `args_valid` 仍为 `True`、`validated_args` 仍然带上；`execute_tool_call` 见到 `deferral` 就 `raise` 它而不执行工具，从而与「工具函数自己抛出」走到同一条延迟处理路径。

**校验阶段的钩子包裹有三层顺序。** `_run_validate_hooks` 用一个闭包 `lifecycle` 把「完整校验生命周期」交给 `wrap_tool_validate`：

- 外层是 `wrap_tool_validate`（能力在这里最能控制「要不要校验」）；
- 里面是 `_run_validation_lifecycle`：`before_tool_validate` → 真正的 `_validate_tool_args`（含 `on_tool_validate_error`）→ `after_tool_validate`。

三条延迟相关的守卫值得记：

- `before_tool_validate` 抛延迟异常 → 直接 `_validate_hook_deferral_error`，报 `UserError`。理由写在文案里：此时参数还没校验，不该宣告「参数合法」。
- 校验器自己的延迟异常被**暂存**（`_ValidationDeferral`），直到 `after_tool_validate` 也跑完才重新抛——因为「策略门」应当先看到校验后的参数。
- `on_tool_validate_error` 抛延迟异常同理报 `UserError`；`after_tool_validate` 抛延迟异常则合法（参数已校验）。

另外 `handler_validated_args` 这个变量只在 `lifecycle` 里被赋值，用来判定「`wrap_tool_validate` 抛的延迟异常是否发生在它自己调过 handler 之后」。这是唯一能区分「外层包裹者提前拒绝」与「内层校验完成后才拒绝」的依据。

还有一条直白的排除：输出工具（`kind == 'output'`）**不走**这些钩子，注释说与 `WrapperToolset` 及 `prepare_tools` 排除输出工具的做法一致。

**`_validate_tool_args` 的细节。** 它接受 `str` 或 dict 两种原始参数：字符串走 `validator.validate_json`（空串退成 `'{}'`），dict 走 `validate_python`；`allow_partial` 映射成 pydantic 的 `'trailing-strings'` 或 `'off'`。校验通过后跑 `args_validator_func`，并在捕获 `(CallDeferred, ApprovalRequired)` 时包装成 `_ValidationDeferral`。

**未知工具与不可用工具是两种不同的 `ModelRetry`。** `_UnknownTool` 的报错文案会列出「本轮模型真正能调的工具」（复用了 `ctx.is_tool_available` 这同一个门槛），并区分三种情况：有可用工具、有工具但都还没揭示（提示「先搜索你需要的工具」）、根本没有工具。`_ToolUnavailable` 是另一种：工具存在但当前不可调（例如延迟工具尚未被展示）。注释解释了为什么文案写「not available yet」而不是「unknown」——要让模型再去搜索/加载，而不是断定工具不存在；而这次搜索/加载的往返恰好会把「凭什么可以调它」的历史恢复出来，这对被压缩过的历史保持连贯是必要的。

**不可用工具的第一个拒绝免费。** `_make_validation_failure` 里有一段专门的逻辑：若错误是 `_ToolUnavailable` 且该工具还没在 `availability_refused` 里，则**不消耗重试预算**，只把它记进那个集合。理由写得很实在：这类拒绝说的是「运行状态」而不是「参数错了」，在默认预算 1 的情况下收费会让一次模型的不听话就直接终止运行，而且会让工具在真正被正确调用并真的失败时无预算可用。后续同一工具的拒绝才正常收费。`availability_refused` 跨整个运行而不按步重置，因为「纠正这个拒绝至少需要再走一步」。

`_check_max_retries` 用 `>=` 而不是 `==` 比较，注释说明：负预算必须立刻抛，而不是无限循环（计数从 0 开始只增不减，永远等不到负数）。

**执行阶段的钩子包裹同样三层。** `_run_execute_hooks` 把 `wrap_tool_execute` 里的 `do_execute` 包住 `_run_execution_lifecycle`（`before_tool_execute` → 实际调用 → `on_tool_execute_error`? → `after_tool_execute`）。`SkipToolExecution` 在这里被捕获：直接用钩子给的结果，并**撤销**该调用提交的输出（`submitter.discard_submission`），注释点明「只有真正跑过的调用才能让它提交的输出成为最终结果」。

`_raw_execute` 是不带钩子、不带追踪的裸执行；`_execute_tool_call_impl` 是带钩子的版本，两者都处理 `ModelRetry` / `ToolFailed` 的包装，但 `wrap_validation_errors=False` 时一律原样抛出、且**不动**重试账本——这条「原始模式」是给沙箱内嵌套派发准备的：嵌套调用不该消耗 agent 的重试预算，调用方要的就是原始异常。

**成功也在记账。** `succeeded_tools` 只在 `wrap_validation_errors` 为真时记录，理由与失败侧对称：原始模式的嵌套成功不得重置下一轮结转使用的计数。

**`handle_call` 是校验 + 执行 + 内联延迟的一站式入口。** 它把 `validate_tool_call` 与 `execute_tool_call` 串起来，并在遇到延迟异常时调用能力的 `handle_deferred_tool_calls` 尝试在运行内解决；两个内部回调 `on_deferred_requests` / `on_inline_deferred` 让上层能观察「有一个调用要被延迟了」与「它被内联解决了」。

## 关键取舍

**源码事实：** 工具集快照按 `(run_step, 能力可用性集合)` 缓存，后者也参与缓存键。这是为了不让「未受能力过滤的工具集」被交给已经可以派发该能力的调用。

**源码事实：** 同名替代裁决考虑全部工具（含未揭示的延迟工具），并且有一套明确处理环与「无替代品则恢复」的兜底，结果对工具集顺序确定。

**源码事实：** 校验与执行被拆成两次调用，中间隔着一个 `ValidatedToolCall`。这让事件能准确报告 `args_valid`，也让「校验失败」与「执行失败」走不同的重试记账路径。

**源码事实：** 延迟异常在校验阶段只有「参数已合法之后」才被允许，之前抛一律报 `UserError`。校验器自己抛的被暂存到 `after_tool_validate` 之后再抛。

**源码事实：** 不可用工具的首个拒绝免费、跨运行累积记录，与普通重试预算分账。

**架构推论：** 把重试账本放在「步级管理器」而不是「运行级对象」，并在跨步时用「成功清零 / 失败累加」来结转，是一种用最少状态表达「工具的连续失败次数」的写法。代价是同一步内的重建必须特别小心不要重复结转（源码为此专门加了 `same_step` 分支）。

**架构推论：** 所有这些裁决（替代、可用性、免费拒绝）都围绕一个目标：让模型看到的工具集**在这一步内稳定**，同时允许跨步变化。这与提示词缓存的要求一致——工具定义与顺序在步间保持稳定，否则前缀失效。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/tool_manager.py` | `ToolManager` | 步级工具裁决者：快照、重试账本、校验/执行分发 |
| `pydantic_ai_slim/pydantic_ai/tool_manager.py` | `ToolManager.for_run_step` | 按 (run_step, 可用性) 缓存或重建，跨步结转重试 |
| `pydantic_ai_slim/pydantic_ai/tool_manager.py` | `ValidatedToolCall` | 校验结果载体：合法性、参数、错误、延迟信号 |
| `pydantic_ai_slim/pydantic_ai/tool_manager.py` | `_drop_superseded_tools` | 同名替代裁决入口 |
| `pydantic_ai_slim/pydantic_ai/tool_manager.py` | `_decide_which_tools_stay` | 不动点迭代 + 钉住环 + 恢复无替代者 |
| `pydantic_ai_slim/pydantic_ai/tool_manager.py` | `_first_on_a_cycle` | 找出沿未决提供者成环的第一个工具 |
| `pydantic_ai_slim/pydantic_ai/tool_manager.py` | `_run_validate_hooks` / `_run_validation_lifecycle` | 校验阶段三层包裹与三种延迟守卫 |
| `pydantic_ai_slim/pydantic_ai/tool_manager.py` | `_make_validation_failure` | 免费拒绝、重试计数、两类错误包装 |
| `pydantic_ai_slim/pydantic_ai/tool_manager.py` | `handle_call` | 校验 + 执行 + 内联延迟处理的一站式入口 |

## 参考

- [pydantic-ai 源码：tool_manager.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/tool_manager.py)
- [Pydantic AI 文档：工具重试](https://ai.pydantic.dev/tools-toolsets/tools-advanced/#tool-retries)
- [Pydantic AI 文档：延迟工具](https://ai.pydantic.dev/tools-toolsets/deferred-tools/)
- [Pydantic AI 文档：工具集](https://ai.pydantic.dev/toolsets/)

## 与主核 / AI 的关系

`ToolManager` 在架构上对应主核里的**执行器状态**：它持有的东西（本步的元数据快照、重试计数、失败集合）都是「一次执行上下文里的可变账本」，而不是持久化数据结构。这与主核把执行状态与存储状态严格分开是同一条纪律——重试计数这类东西不该写进任何持久结构，它只属于这一次运行。

同名替代算法值得单独对照主核里的「索引选择」：都是「给定一组候选，按声明的关系决定谁上场」。但主核的裁决是基于代价与可用性的全局优化，可以有多个索引同时存在；这里的裁决是布尔式的淘汰，且带了一条明确的失败哲学——环冲突时宁可保留冗余也不丢功能。这条取舍在数据库里不可想象（空间与写放大会失控），在工具场景里却合理，因为「多一个工具」的成本只是几个 token。

对 AI 工程的实际意义在于：这套代码里最值钱的部分是那些「免费拒绝」「未知 vs 不可用」「原始模式不动账本」的细节。它们解决的都不是理论问题，而是实际运行中会遇到的失真：模型调了一个还没揭示的工具，如果按普通失败计数，默认预算 1 就让整轮结束了；模型调了个不存在的工具名，如果文案只说 unknown，它会重复猜；沙箱里的嵌套派发如果消耗外层预算，会让外层莫名其妙地失败。这些区分让错误信息与预算各归其位。
