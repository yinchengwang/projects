---
project: crewai
feature: delegation
title: 委派工具注入与层级协作
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/crew.py
    symbols: [_prepare_tools, _add_delegation_tools, _update_manager_tools]
  - path: lib/crewai/src/crewai/crew.py
    symbols: [_create_manager_agent, _get_agent_to_use]
---

# 委派工具注入与层级协作

## 一句话职责

「委派」不是智能体之间的直接调用，而是**给某个智能体的工具列表里塞进两个工具**（`Delegate work to coworker` 与 `Ask question to coworker`），让它自己决定要不要把活转出去；`_prepare_tools` 负责在每次任务执行前决定塞给谁、塞哪些同事。

## 机制是什么

三个概念必须分清：

**谁是执行者**，由 `_get_agent_to_use(task)` 决定：层级流程一律返回 `self.manager_agent`（管理智能体代跑所有任务），顺序流程返回 `task.agent`。

**谁拿到委派工具**，由 `_prepare_tools` 的三个分支决定：层级流程走 `_update_manager_tools`，非层级且 `allow_delegation` 为真走 `_add_delegation_tools`，否则不注入。

**委派给谁**，由工具实例里的 `agents` 列表决定，而这个列表在两种流程下取的是不同的集合——顺序流程是「除自己以外的所有智能体」，层级流程在正常情况是「团队全体」，但当 `task.agent` 已经被上一轮运行指派成别的智能体时，会退化成只有它自己。

这套机制的关键性质是：**委派是工具调用，配置在工具列表里，而不是在调度器里**。框架层面没有「委派表」，只有「这个智能体这一轮能看到哪些同事」。

## 怎么实现的

**`_prepare_tools` 是一个按能力逐项追加的管道。** 签名是 `(agent, task, tools)`，返回新的工具列表。它按固定顺序检查这些能力并逐个 `_merge_tools`：

1. 委派（`allow_delegation`）：层级流程 → `_update_manager_tools(task, tools)`，且若 `manager_agent` 为空则抛「Manager agent is required for hierarchical process.」；非层级 → `_add_delegation_tools(task, tools)`。
2. 代码执行（`allow_code_execution`）→ `_add_code_execution_tools`。
3. 多模态（`multimodal` 且模型**不支持**多模态）→ `_add_multimodal_tools`。注意这个否定条件：模型原生支持多模态时不需要额外的读图工具。
4. 平台应用（`apps` 非空）→ `_add_platform_tools`。
5. MCP（`mcps` 非空）→ `_add_mcp_tools`。
6. 记忆：`resolved_memory = getattr(agent, "memory", None) or self._memory`，非空就 `_add_memory_tools`——智能体级记忆优先于团队级。
7. artifact：`self.artifacts or getattr(agent, "artifacts", False)` 时从 `crewai.artifacts` **函数内**导入 `SaveArtifactTool` 并 `SaveArtifactTool(agent=agent, task=task)`。
8. 输入文件：`get_all_files(self.id, task.id)` 拿到本任务的文件字典，再按模型支持的 `content_type` 前缀分流——**模型能原生读的（`is_auto_injected`）不挂工具**，只有读不了的那部分才 `_add_file_tools`（内部造一个 `ReadFileTool` 并 `set_files(files)`）。

这一段说明了为什么委派逻辑要写在团队层而不是智能体层：工具集的最终形态取决于团队配置（流程类型、记忆、artifact 开关）与智能体配置的**交集**，单看智能体自己是算不出来的。

**`_add_delegation_tools` 的三重条件。** 实现只有几行：

```
agents_for_delegation = [agent for agent in self.agents if agent != task.agent]
if len(self.agents) > 1 and len(agents_for_delegation) > 0 and task.agent:
    if not tools:
        tools = []
    tools = self._inject_delegation_tools(tools, task.agent, agents_for_delegation)
return tools
```

三个条件缺一不可：团队里不止一个智能体、去掉自己后还有剩余、且任务确实绑了智能体。第一个条件在第二个成立时基本冗余，但它点明了意图——**一个独苗智能体没有可委派的对象**。最后 `_inject_delegation_tools` 调 `task_agent.get_delegation_tools(agents)`，在 `Agent` 上的实现是 `AgentTools(agents=agents).tools()`。

`AgentTools.tools()` 造两个工具：`DelegateWorkTool` 与 `AskQuestionTool`，两者的描述里都嵌了 `", ".join(agent.role for agent in agents)`——**同事名单是写进工具描述文本里的**。这意味着模型是通过自然语言看到候选同事的，而不是从某个结构化参数枚举里选。两个工具的 `args_schema` 分别是 `DelegateWorkToolSchema(task, context, coworker)` 与问询版本的对应 schema，`coworker` 是自由字符串。

**`_merge_tools` 按名字去重。** 它用 `sanitize_tool_name(tool.name)` 建 `new_tool_map`，先把已有列表里**同名**的条目全部过滤掉，再 `extend` 新工具。所以后注入的覆盖先注入的，且同名工具不会出现两份。这解释了为什么同一个工具被反复注入是安全的。

**`_update_manager_tools` 的两条路。** 实现是：

```
if self.manager_agent:
    if task.agent and task.agent is not self.manager_agent:
        tools = self._inject_delegation_tools(tools, task.agent, [task.agent])
    else:
        tools = self._inject_delegation_tools(tools, self.manager_agent, self.agents)
```

第一支带一句注释：「A previous run leaves the manager assigned to the task; that is not a worker to delegate to.」——这是对**复用状态**的处理。`prepare_task_execution` 里调用的是 `_get_agent_to_use(task)`，层级流程返回管理者，而管理者随后被写进了 `task.agent`（`_execute_core` 里回写 `self.agent = agent`）。因此第二次执行同一批任务时，`task.agent` 可能已经是管理者自己。此时若仍按「团队全体」注入，委派列表里就会出现管理者本人；代码的处理是：当 `task.agent` 存在且不是管理者时，把委派工具挂到 `task.agent` 上、名单只含它自己（这在语义上很怪，但因为执行者是管理者，`task.agent` 作为委派目标的场景本就边缘）；否则把委派工具挂到管理者上、名单为团队全体。

第二支是正常路径：管理者的工具列表里加两个委派工具，候选是 `self.agents`（**包含当前任务原本的执行者**，因为管理者需要能把活派给任何人）。

**`_create_manager_agent` 与委派的接合点。** 这个方法的细节见 a02，但其中一句与委派直接相关：造管理者时 `tools=AgentTools(agents=self.agents).tools()`——**管理者一开始就带着委派工具**，而不是等到 `_prepare_tools` 才注入。所以管理者有两处来源的委派工具，靠 `_merge_tools` 的同名去重合并成一份。用户自带管理者时则有一条硬约束：若有非 `BaseAgentTool` 的工具，记 warning、清空、并抛「Manager agent should not have tools」——管理者的职责是委派，不该自己动手。

**委派执行时的实际行为。** 工具的真身是 `BaseAgentTool._execute(agent_name, task, context)`：

1. `sanitize_agent_name`：`" ".join(name.split())` 归一空白、去掉双引号、`casefold()` 转小写。源码注释解释了去引号的理由——「less-powerful LLM 容易产出被截断的非法 JSON」，于是名字里可能带引号残片。
2. 拿 `self.agents` 里的每个 `role` 同样归一，比较后取**第一个**匹配项。
3. 匹配不到就返回国际化文案 `agent_tool_unexisting_coworker`，把「可用同事清单 + 具体错误」拼进去——即**失败被当成工具返回值**而不是异常，模型能看到并纠正。
4. 匹配到就**现造一个 Task**：`Task(description=task, agent=selected_agent, expected_output=I18N_DEFAULT.slice("manager_request"))`，然后 `selected_agent.execute_task(task_with_assigned_agent, context)`。

这一步很关键：**委派出来的是一个全新的、临时的 `Task` 对象**，它不属于团队的 `tasks` 列表，也不进 `task_outputs`、不落库、不参与重放。所以委派的产出对流水线是「隐形的」——它只作为工具结果回到委派者的消息里。

`_get_coworker` 还处理了一个现实问题：`coworker` 参数可能不存在，于是回落到 `kwargs` 里的 `co_worker` 或 `coworker`；若值形如 `"[a, b]"`（模型把列表塞进了字符串），取方括号内按逗号切的第一项。这些都是对模型输出不规范的兜底。

**层级流程里 `task.agent` 的语义被改写了。** 顺序流程下 `task.agent` 是执行者；层级流程下真正的执行者是管理者，`task.agent` 退化成「原始负责人」这一提示信息。这个语义漂移由 `_get_agent_to_use` 一处集中处理，也正是 `_update_manager_tools` 里那个 `task.agent is not self.manager_agent` 判断存在的原因。

## 关键取舍

**源码事实：** 委派能力通过「注入工具」实现，而不是「调度器分支」。因此委派与否完全由模型决定，框架不做任何保证——即使配了 `allow_delegation=True`，模型也可以一次都不委派。

**源码事实：** 同事名单是写进工具描述的自然语言文本，选择靠模型输出一个自由字符串 `coworker`，框架再用归一化比较去匹配角色名。这让接入成本极低，但也意味着角色名相似时会匹配到错误的同事（取第一个匹配）。

**源码事实：** 委派失败（找不到同事）返回错误文案而不是抛异常。这是把「失败」降级成「观察」，让模型自我纠正，与工具调用的一般约定一致。

**源码事实：** 委派产出的是一个临时 `Task`，不进入团队的 `tasks`、`task_outputs` 与存储。所以从执行日志里看不出委派发生过几次——只有 `Task.increment_delegations` 这类计数可能被触碰。

**源码事实：** `_update_manager_tools` 对「`task.agent` 已是管理者」这一残留状态做了特殊处理，而不是简单清空。这是复用 `Task` 对象带来的状态泄漏在代码里留下的疤痕。

**源码事实：** 用户自带的管理者若携带普通工具会被**强制清空并抛异常**，而不是只警告。委派工具豁免（`isinstance(tool, BaseAgentTool)` 判断）。

**架构推论：** 把「谁能派活给谁」表达成「工具的候选名单」而不是「权限矩阵」，使委派关系可以按任务动态变化（不同任务的执行者看到不同同事），代价是这种关系不可静态审计。

**架构推论：** 委派被限制在「一层」——被委派的智能体执行的是一个新 Task，该 Task 的 `tools` 由 `check_tools` 从智能体自身工具复制，因此它也可能带委派工具并继续往下派。源码没有深度限制，真正的上限只能是模型的收敛行为与 `max_iter` 的执行预算。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/crew.py` | `_prepare_tools` | 按能力逐项追加工具的管道，委派是第一项 |
| `lib/crewai/src/crewai/crew.py` | `_add_delegation_tools` | 顺序流程：按「除自己以外」的名单注入委派工具 |
| `lib/crewai/src/crewai/crew.py` | `_update_manager_tools` | 层级流程：管理者或残留 `task.agent` 两种注入路径 |
| `lib/crewai/src/crewai/crew.py` | `_create_manager_agent` | 管理者的两种来源与「不许带普通工具」的硬约束 |
| `lib/crewai/src/crewai/crew.py` | `_get_agent_to_use` | 层级流程固定返回管理者，顺序流程返回任务绑定者 |
| `lib/crewai/src/crewai/crew.py` | `_merge_tools` | 按 `sanitize_tool_name` 去重合并，后注入覆盖同名 |
| `lib/crewai/src/crewai/crew.py` | `_inject_delegation_tools` | 调 `task_agent.get_delegation_tools(agents)` 并合并 |
| `lib/crewai/src/crewai/tools/agent_tools/agent_tools.py` | `AgentTools` | 造出委派与提问两个工具，同事名单写进描述 |
| `lib/crewai/src/crewai/tools/agent_tools/base_agent_tools.py` | `BaseAgentTool` | 归一化角色名、匹配同事、造临时 Task 并执行 |

## 参考

- [CrewAI 源码：crew.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/crew.py)
- [CrewAI 源码：base_agent_tools.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/tools/agent_tools/base_agent_tools.py)
- [CrewAI 文档：协作与委派](https://docs.crewai.com/en/concepts/collaboration)

## 与主核 / AI 的关系

「把能力做成工具、把关系做成工具的候选名单」这套做法，与主核里「把访问权限做成执行计划里的一层算子」有相似之处：都不是在调度器里写死关系，而是让关系随上下文变化。但主核的关系是静态可验证的（权限检查在编译或加锁时就能确认），这里的关系是**运行时靠自然语言协商**的——框架只能保证工具存在，不能保证委派发生，也不能保证委派到对的人。

对 AI 工程的实际意义在于：委派是这套框架里最容易「看起来在工作」的机制。工具被注入了、名单被印在描述里了、模型也确实调用了，但被委派的智能体跑的是一次性 Task，它的产出既不入库也不参与重放。因此当流水线结果不对时，光看团队的 `task_outputs` 是查不出委派链的，必须依赖事件总线与追踪。这也解释了为什么这套机制被同时提供两条注入路径（顺序流程的自主委派、层级流程的管理者委派）——它们是同一种工具注入在不同调度模型下的两种用法，而不是两套独立系统。
