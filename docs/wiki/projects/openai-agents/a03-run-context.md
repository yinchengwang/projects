---
project: openai-agents
feature: run-context
title: 运行上下文与用途约束
page_type: feature
schema_version: 1
refs:
  - path: src/agents/run_context.py
    symbols: [RunContextWrapper, AgentHookContext]
  - path: src/agents/tool_context.py
    symbols: [ToolContext]
---

# 运行上下文与用途约束

## 一句话职责

`RunContextWrapper` 是「你的应用对象 + SDK 自己的工具状态」的合成体：应用对象永远不进模型，SDK 的工具审批与调用记录永远不进应用，两者靠一个 wrapper 绑在一起随调用链下传。

## 机制是什么

`RunContextWrapper` 有六个字段，前三个是公开语义，后三个是 SDK 私有：

| 字段 | 语义 |
|---|---|
| `context` | 调用方传给 `Runner.run` 的应用对象（任何类型），会原样交给工具函数、交接、护栏、钩子 |
| `usage` | 本次运行的累计用量，含 `request_usage_entries` 逐请求明细 |
| `turn_input` | 当前轮次交给模型的输入项列表 |
| `_approvals` | 审批决定表：键是工具命名空间/名字/调用 id 的组合，值是 `_ApprovalRecord` |
| `_tool_invocations` | 工具调用记录表：按 provider 的 call id 记执行与完成状态 |
| `tool_input` | 当本智能体是「作为工具被调用」时，它收到的结构化输入 |

**它明确不是消息载体。** 类文档写死一句：Contexts are not passed to the LLM。这句话划定了整个 SDK 的一条边界：应用对象可以带数据库连接、用户 id、缓存句柄，它们不会因为「上下文里有」就被序列化进模型请求。

三个子类各自负责一种用途：

- `ToolContext`：在 `RunContextWrapper` 之上加 `tool_name`、`tool_call_id`、`tool_arguments`、`tool_call`、`tool_namespace`、`agent`、`run_config`，是工具函数与工具钩子实际收到的类型；
- `AgentHookContext`：**空子类**，不加任何字段，只用于把「钩子上下文」与「工具上下文」在类型层面区分开；
- `RunState._context` 字段持有的仍然是一个 `RunContextWrapper`，恢复时会被 `resolve_resumed_context` 特殊处理（见下）。

## 怎么实现的

**`context` 与 SDK 状态同住一个对象，但生命周期完全不同。** `context` 是调用方给的，SDK 只读不写；`usage` 与两张状态表是 SDK 自己的，运行中反复原地修改。把两者放一起的理由是省掉「每次调用都传两个参数」，代价是「共享一个 wrapper」就等于「共享用量与审批状态」——这一点直接决定了下文的 fork/copy 两种传播方式。

**两种传播方式，语义相反。** `_share_tool_state_with(target)` 把 `_approvals`、`_tool_invocations`、两个恢复标记**按引用**赋给目标，用于「同一个运行的派生子上下文」；`_copy_for_run_state()` 则 `deepcopy` 用量与两张表、再给新 wrapper 生成一个全新的 `agent_tool_state_scope` uuid，用于「独立可续跑的检查点」。源码注释解释了为什么用量必须深拷贝：`Usage.add` 是原地累加并追加 `request_usage_entries`，共享实例会让一个检查点的 token 记到所有检查点与结果对象上。

`_fork_with_tool_input(tool_input)` 与 `_fork_without_tool_input()` 是共享式的 fork：新建 wrapper、共享 `usage` 与工具状态、复制 `turn_input`，区别只在是否设置 `tool_input`。

**`ToolContext` 用「必填参数没有默认值」实现强制。** 三个必填字段的 `default_factory` 分别指向 `_assert_must_pass_tool_name`、`_assert_must_pass_tool_call_id`、`_assert_must_pass_tool_arguments`——三个只做一件事的函数：抛 `ValueError("... must be passed to ToolContext")`。构造函数自己用 `_MISSING` 哨兵逐个判断，缺哪个就调对应的断言函数。这样「漏传」得到的是带字段名的错误，而不是 `TypeError: missing argument` 或静默的 `None`。构造函数保留 v0.7 的位置参数顺序（`context, usage, tool_name, tool_call_id, tool_arguments, tool_call`），新字段全部走 keyword-only，docstring 写明这是为了保住旧调用形态。

**`from_agent_context` 是「升格」而不是「转换」。** 它用 `fields(RunContextWrapper)` 遍历父类的 `init=True` 字段（排除 `_approvals`）逐个复制，然后 `context._share_tool_state_with(tool_context)` 把审批与调用记录**共享**过来，再把 `agent_tool_state_scope` 也复制过去。所以一个工具里的 `ToolContext` 看到的是与本次运行完全相同的审批状态——这正是「在工具里 `ctx.approve_tool(...)` 就等价于在运行里批准」的实现基础。

**恢复路径的 override 语义是反直觉的。** `resolve_resumed_context` 在用户给了一个新 context 覆盖时，不是新建 wrapper，而是把**恢复出来的那个 wrapper** 里的 `context` 字段换掉，然后继续用它。docstring 明说理由：审批、用量、turn input、tool input 属于运行所有，不能因为换应用对象而丢失。这条取舍意味着「恢复时换 context」只换应用数据，不动运行状态。

**审批表的键要绑定到具体智能体。** `_approvals` 的键类型是 `_FunctionToolApprovalKey`，一个 `str` 子类，额外带 `_agent_ref`（对智能体实例的弱引用）与 `tool_key`。它实现了 `__get_pydantic_core_schema__`，序列化时只写字符串，反序列化时得到的键 `_agent_ref` 为 `None`——也就是「悬空」。`_resolve_function_approval_owners` 在恢复时遍历智能体图，用 `_function_approval_identity` 里的 token 把悬空键重新绑到**同一个活着的智能体实例**上；绑不上的记录会被保留但保持休眠（注释：留待后续有 owner 的运行使用）。这挡住了「把审批状态搬到克隆出来的、同名的智能体上」这条路径。

**审批值是「布尔或调用 id 列表」。** `_ApprovalRecord.approved` / `rejected` 都是 `bool | list[str]`：`True`/`False` 表示对该工具的永久放行/拒绝，列表表示只针对特定 call id。`approve_tool(item, always_approve=True)` 就是把它写成永久决定。`is_tool_approved` 与 `get_approval_status` 都返回 `bool | None`，`None` 表示尚无决定。

**`AgentHookContext` 是纯标记。** 它继承 `RunContextWrapper` 且 `pass` 到底，唯一的差别是类型。这样 `RunHooks.on_agent_start(context: AgentHookContext, ...)` 与 `on_tool_start(context: RunContextWrapper, ...)` 在类型层面可区分，同时钩子拿到的字段与运行上下文完全一致。

## 关键取舍

**源码事实：** `context` 绝不进模型。这条约束使「把敏感数据放进 context」成为合法做法——它只会被你的 Python 代码读到，而不会被模型请求带出去。

**源码事实：** 共享 vs 深拷贝的区别是刻意的。派生上下文（工具、嵌套）按引用共享，所以工具里做的审批决定立刻对整条运行生效；检查点快照则深拷贝，所以多个检查点之间不会互相污染用量。一个类上并存两种语义，判断依据只有调用点用的是哪个方法。

**源码事实：** `ToolContext` 用哨兵 + 断言工厂而不是「必填位置参数」，是为了在保留 v0.7 位置参数兼容的同时给出更明确的错误。代价是 dataclass 的 `__init__` 被手写覆盖，字段默认值实际不可用。

**源码事实：** 恢复时覆盖 context 只替换应用对象，wrapper 本身保留。这使「恢复时换数据库连接、保留审批决定」这种真实需求得以成立；反过来，「恢复时想清空用量」没有对应开关。

**架构推论：** 把「应用数据」与「框架状态」塞进同一个对象，是这类 SDK 的常见做法（LangGraph 后来也把 `config` 与 `Runtime` 分开过）。代价是共享粒度只能整体共享或整体拷贝——没有「只共享审批、不共享用量」的组合。需要那种组合时只能手工构造 wrapper 再逐字段赋值，而这正是 `_fork_*` 系列在做的事。

**架构推论：** 审批键绑定到智能体实例（弱引用）而不是名字，意味着「把状态搬到同类新实例上」在框架层面被拒绝。这与 `RunState` 序列化里「同名智能体且无法用 hook 状态区分就拒绝对等化」是同一套思路：身份优先于名字。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/run_context.py` | `RunContextWrapper` | 应用对象 + SDK 工具状态的合成容器 |
| `src/agents/run_context.py` | `RunContextWrapper._share_tool_state_with` | 派生上下文的按引用共享 |
| `src/agents/run_context.py` | `RunContextWrapper._copy_for_run_state` | 检查点用的深拷贝 + 新作用域 id |
| `src/agents/run_context.py` | `RunContextWrapper._fork_with_tool_input` | 共享式 fork，并设置工具结构化输入 |
| `src/agents/run_context.py` | `RunContextWrapper.approve_tool` | 批准一条调用，可选永久生效 |
| `src/agents/run_context.py` | `RunContextWrapper.reject_tool` | 拒绝一条调用，可带拒绝消息 |
| `src/agents/run_context.py` | `RunContextWrapper.get_approval_status` | 按工具名/调用 id 查询决定，`None` 表未定 |
| `src/agents/run_context.py` | `RunContextWrapper.is_tool_approved` | 简化查询，兼容 hosted MCP 的查询记录 |
| `src/agents/run_context.py` | `_ApprovalRecord` | 决定表的值：布尔（永久）或 call id 列表（按次） |
| `src/agents/run_context.py` | `_FunctionToolApprovalKey` | 绑定到智能体实例的审批键，可序列化为字符串 |
| `src/agents/run_context.py` | `AgentHookContext` | 空子类，只用于类型区分 |
| `src/agents/tool_context.py` | `ToolContext` | 工具调用上下文的字段与强制构造 |
| `src/agents/tool_context.py` | `ToolContext.from_agent_context` | 从运行上下文升格为工具上下文并共享工具状态 |
| `src/agents/run_internal/agent_runner_helpers.py` | `resolve_resumed_context` | 恢复时只替换应用对象，保留运行状态 |
| `src/agents/run_internal/agent_runner_helpers.py` | `ensure_context_wrapper` | 把裸应用对象归一成 wrapper |

## 参考

- [openai-agents-python 源码：run_context.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/run_context.py)
- [openai-agents-python 源码：tool_context.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/tool_context.py)
- [Agents SDK 文档：Context](https://openai.github.io/openai-agents-python/context/)
- [Agents SDK API：run_context 模块](https://openai.github.io/openai-agents-python/ref/run_context/)

## 与主核 / AI 的关系

这份 wrapper 与主核里「执行上下文结构体」扮演同一个角色：把横切依赖打包下传，而不是让每个函数自己去取全局。差别在于主核的上下文通常是只读的，而这里的 wrapper 内部一半是只读（`context`）、一半是活状态（`usage`、审批表）。

「应用数据与框架状态共存但用途受约束」这条边界值得迁移到任何 agent 系统：判断一个框架能不能安全携带敏感依赖，看它有没有明确声明「context 不进模型请求」。声明了，才敢把连接池、凭据句柄放进去；没声明，就得假设整个上下文随时可能被序列化进提示词。

审批键绑定实例而非名字，是另一个可直接借用的模式：任何需要「决定可跨序列化边界传递、但只能作用于原始对象」的系统，都得在键里带上可验证的身份，并在恢复时做一次重新绑定，而不是按名字模糊匹配。
