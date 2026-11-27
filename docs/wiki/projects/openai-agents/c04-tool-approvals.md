---
project: openai-agents
feature: tool-approvals
title: 工具审批与拒绝路径
page_type: feature
schema_version: 1
refs:
  - path: src/agents/run_internal/approvals.py
    symbols: [filter_tool_approvals, approvals_from_step, append_approval_error_output]
---

# 工具审批与拒绝路径

## 一句话职责

把「这个工具调用要不要先问人」做成一次可恢复的中断：需要审批的调用不进执行，而是变成待决项让整个运行停下并存盘；批准就继续执行，拒绝就把一段文本当作工具输出写回对话。

## 机制是什么

审批横跨四个对象：

- `FunctionTool.needs_approval`：`bool` 或 `Callable[[RunContextWrapper, dict, str], Awaitable[bool]]`，决定这一次调用是否需要审批。
- `ToolApprovalItem`：待决项本身，携带原始调用、工具名、命名空间、来源元数据与规范查找键。
- `RunContextWrapper._approvals`：决定记录表，键是规范化的「工具身份」，值是批准/拒绝记录，支持「仅这一次」与「以后都这样」两级。
- `NextStepInterruption`：单步结果的一种，让运行循环停在「模型已返回、工具未执行」的位置。

`run_internal/approvals.py` 是这个机制里「中断载荷」一层的工具函数：`filter_tool_approvals` 从混杂的中断列表里挑出审批项，`approvals_from_step` 从可能没有 `interruptions` 属性的步骤里安全地取审批项，`append_approval_error_output` 生成一条合成的工具输出，让「审批流程本身出错」这件事对模型可见。

## 怎么实现的

**中断载荷是混合的。** `interruptions` 列表里不只有审批项，还可能有别的需要人介入的东西。`filter_tool_approvals` 的做法是最朴素的 `isinstance(item, ToolApprovalItem)` 过滤；`approvals_from_step` 则先用 `getattr(step, "interruptions", None)` 探测——属性不存在或为 `None` 时返回空列表。后者存在的原因很实际：恢复路径上拿到的「步骤」可能是不同版本的状态对象，不能假设它一定有 `interruptions`。

**合成错误输出。** `append_approval_error_output` 用于「审批没能完成」的场景：它先把传进来的 `tool_call` 规范化成 `ResponseFunctionToolCall`，再构造 `ToolCallOutputItem` 追加到 `generated_items`。规范化的做法是：已经是目标类型就直接用；否则从字典或对象上尽力取出 `namespace`，再拼一个 `type="function_call"`、`name`、`call_id`（缺省填 `"unknown"`）、`status="completed"`、`arguments="{}"` 的调用对象。注释写明目的：**让用户看到审批为什么失败**——因为如果只发一个错误而不产出工具输出，对话历史里会留下一个「有调用无输出」的悬空引用，下一轮模型输入就不合法。

**策略求值的两条路径。** `evaluate_function_tool_approval` 是「要不要审批」的主判定，它的返回值 `FunctionToolApproval` 带 `outcome`（`invoke` 或 `require_approval`）、工具、invoker、参数字符串，以及可选的 `prepared`。关键分支：`needs_approval` 是布尔就直接用；是可调用才去解析参数给策略看。解析走 `parse_function_tool_arguments`——解析不出对象就**直接返回 `require_approval`**，不做第二次尝试。解析成功时还要检查能否「准备好参数」：`prepare_arguments` 返回 `None` 或 `unchanged=False` 时同样退回 `require_approval`。

**为什么 `unchanged=False` 意味着必须人工审批。** 参数准备会经过 pydantic 校验，而校验可能补默认值、做类型转换、跑自定义校验器。如果「准备后的值」与「模型原始给的 JSON」不一致，那么「策略看到什么」与「工具真正收到什么」就可能不同——一个只检查 `path` 字段的策略可能放过一个实际会写到别处的调用。所以 `unchanged` 为假时不信任自动判定。

**决定记录的三层键。** `get_approval_status` 是决定查询入口，它的候选键有三类来源：显式传入的命名空间与查找键、待决项自身携带的键、以及裸工具名。查询顺序是先具体后笼统，注释与文档明确说明「以 agent 为归属的函数工具决定」必须带调用方信息才生效，裸名字查询要用 `is_tool_approved`。这带来一个重要推论：**「永远批准」是随 agent 生效的**，不是全局的——同一个工具在两个 agent 里被授过的权限互不共享。

**拒绝消息的三级来源。** `resolve_approval_rejection_message` 按顺序取值：应用在上下文中设置的显式拒绝消息 → 运行级 `tool_error_formatter` → 常量 `REJECTION_MESSAGE`（即 `Tool execution was not approved.`）。取到之后还会调 `_mark_tool_invocation_executed`，避免同一次调用在后续判定里被当成「未执行」。

**拒绝的落地形态。** `function_rejection_item` 构造的是 `ToolCallOutputItem`：`output` 是拒绝消息本身（给应用与 UI 看），`raw_item` 由 `function_tool_error_output` 组装（给模型看）。若该函数工具声明了 `output_json_schema`，拒绝消息会被塞进符合该 schema 的错误结构里——否则模型下一轮会看到一个「不符合输出声明的工具结果」，反而制造新的错误。构造前还会 `drop_agent_tool_run_result`，清掉 agent-as-tool 缓存的嵌套运行结果，防止被拒的调用复用一个已存在的嵌套结果。

**拒绝后运行怎么走。** 拒绝不是抛异常，而是往 `new_step_items` 里塞一条工具输出，然后**继续下一轮**：模型会看到「该工具未获批准」这条文本，并有机会改走别的路。这就是为什么拒绝消息必须写成模型可理解的句子，而不是错误码。

**恢复路径。** 中断时整个 `RunState` 被保存，恢复时 `Runner` 接受 `RunState` 作为输入。`resolve_interrupted_turn` 负责继续：它检查当前步骤是否是「已接受响应但还没跑完 LLM 结束钩子」的状态，若是则补跑钩子；然后从状态里取出待决项，对每个待决项查决定记录，批准的执行、拒绝的产出拒绝项。路径里还有一个「已恢复的输出」重扫：从生成项与公开会话项两个切片中找回那些「已被接受但因回调失败没进公开历史」的工具输出，通过 `tool_output_identity` 去重，保证恢复后不重复也不丢。

**嵌套审批。** agent-as-tool 场景下，内层 run 的中断要冒泡到外层。`RunState.approve` / `reject` 会先调 `_find_nested_approval_state`：找到就转交给内层状态的同名方法，找不到才作用于自己的上下文。这使外层应用可以直接批准一个内层工具的调用，而不需要自己拆解层级。

## 关键取舍

**源码事实：** 审批是**中断 + 存盘 + 重入**，不是「原地等待」。运行不是挂起在某个 `await` 上等人，而是干净地结束并返回一个可序列化状态。这条选择决定了审批可以跨进程、跨服务、跨天完成，但也意味着「等待期间的内存状态」全部丢失，恢复完全依赖 `RunState` 里存了什么。

**源码事实：** 策略看不明白就要求审批。`parse_function_tool_arguments` 遇到非对象 JSON、非标准常量（`NaN`/`Infinity`）、空串都返回 `None`，`evaluate_function_tool_approval` 据此直接给 `require_approval`。这是一个明确的「失败即保守」选择：宁可多问一次人，也不让策略在信息不全时放行。

**源码事实：** 「永远批准」的作用域是单个 agent。文档在 `approve_tool` / `reject_tool` / `get_approval_status` 三处重复强调「permanent function-tool decisions apply only to the approval item's agent」。多 agent 编排里，这意味着给一个 agent 授权的工具在切到另一个 agent 后需要重新授权。

**源码事实：** 拒绝要产出符合输出 schema 的载荷。若工具声明了 `output_json_schema`，拒绝消息会被包装成该 schema 的错误形状。这避免了「拒绝一次就污染后续所有轮次」的连锁错误——否则模型下一轮读到的工具结果通不过它自己的输出约定。

**源码事实：** `append_approval_error_output` 会为一个可能不存在的调用造出合成调用对象（`call_id` 填 `"unknown"`）。这是一个兜底：宁可产生一条字段不精确但结构完整的输出，也不要让历史里出现悬空的调用引用。

**源码事实：** 拒绝路径不抛异常。对应用来说，「拒绝」是一次正常完成（带拒绝输出），只有「审批流程本身出错」才走错误路径。这让应用侧的分支更简单：读到拒绝文本继续跑就行。

**架构推论：** 这套设计的核心不变式是「一次工具调用在历史里必须成对出现」：有 `function_call` 就必有 `function_call_output`。审批、拒绝、超时、解析失败、执行失败全都遵守这条，因为它们都以「写一条输出」收尾。反过来说，任何绕过这条不变式的错误处理（例如在工具执行前直接抛异常退出）都会留下不合法历史，使恢复或下一轮调用失败。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/run_internal/approvals.py` | `filter_tool_approvals` | 从混合中断列表中挑出审批项 |
| `src/agents/run_internal/approvals.py` | `approvals_from_step` | 从无 `interruptions` 属性的步骤安全取审批项 |
| `src/agents/run_internal/approvals.py` | `append_approval_error_output` | 生成合成工具输出，让审批失败对模型可见 |
| `src/agents/run_internal/approvals.py` | `_build_function_tool_call_for_approval_error` | 把任意原始载荷规范化为 `ResponseFunctionToolCall` |
| `src/agents/util/_approvals.py` | `evaluate_function_tool_approval` | 策略主判定：布尔直判，可调用时先备参数再判 |
| `src/agents/util/_approvals.py` | `parse_function_tool_arguments` | 策略可见的保守参数视图 |
| `src/agents/run_internal/items.py` | `function_rejection_item` | 拒绝的落地形态：输出项 + 符合输出 schema 的载荷 |
| `src/agents/run_internal/tool_execution.py` | `resolve_approval_rejection_message` | 拒绝文案三级取值：上下文显式 → 运行级 formatter → 常量 |
| `src/agents/run_internal/tool_execution.py` | `_maybe_execute_tool_approval` | 中断前判定：查已决记录、决定是否产出待决项 |
| `src/agents/run_internal/turn_resolution.py` | `resolve_interrupted_turn` | 恢复路径：补跑钩子、逐项查决定、执行或产出拒绝项 |
| `src/agents/run_state.py` | `RunState.approve` / `RunState.reject` | 对外的决定入口，含嵌套审批转发 |

## 参考

- [openai-agents-python 源码：run_internal/approvals.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/run_internal/approvals.py)
- [openai-agents-python 源码：util/_approvals.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/util/_approvals.py)
- [Agents SDK 文档：Human-in-the-loop](https://openai.github.io/openai-agents-python/human_in_the_loop/)

## 与主核 / AI 的关系

审批在这里扮演的角色，与主核里「两阶段提交」的 prepare 阶段相似：先把动作固定成一个可审阅、可拒绝、可持久化的中间态，再决定是否真的执行。差别是主核的 prepare 由系统自动裁决，这里的裁决权交给应用或人，并且允许「拒绝」作为一种正常结果继续流程。

对 agent 工程最实用的三条推论。其一，**审批成本取决于中断粒度**。中断意味着这次运行结束、状态存盘、下次重新进模型循环；若把审批放在每次工具调用上，一次多步任务会被切成很多次运行，延迟与一致性成本都要重新评估。适合审批的是「少量、高风险、不可逆」的动作，而不是所有工具。其二，**拒绝消息是模型可见的提示**，写在里面的措辞会直接影响模型下一步行为。说「未获批准」模型可能再试一次；说「该操作被策略永久禁止，请改用查询」它才会换路。其三，**`always_approve` 的 agent 级作用域是个容易踩的坑**：多 agent 系统里在一个 agent 下授予的「永远批准」，切到另一个 agent 就不生效，表现为「同一个工具反复要求审批」。
