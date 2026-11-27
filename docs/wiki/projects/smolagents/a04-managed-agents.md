---
project: smolagents
feature: managed-agents
title: 被管理的子智能体
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/agents.py
    symbols: [_setup_managed_agents, _validate_tools_and_managed_agents]
  - path: src/smolagents/agents.py
    symbols: [ManagedAgentPromptTemplate]
---

# 被管理的子智能体

## 一句话职责

把一个完整的智能体实例包装成「父智能体可调用的一个工具」：父智能体只看到名字、描述与一个 `task` 参数，子智能体在自己的循环里跑完后返回一段格式化的报告文本。

## 机制是什么

多智能体在这里没有专门的编排语法。它的实现只有三步改造：

| 改造点 | 做法 |
|---|---|
| 形状 | 构造期把子智能体的 `inputs` 强制改成 `{task, additional_args}`、`output_type` 改成 `"string"` |
| 可调用 | 给智能体实现 `__call__(task)`：加任务提示 → `run` → 包报告 |
| 可发现 | 名字与描述进系统提示词；`CodeAgent` 还把 `__call__` 注入解释器命名空间 |

所以「委派」在机制上等于「调用一个返回字符串的工具」。父智能体不需要知道子智能体是智能体，也不需要维护子任务的会话——它只拿到一份文本报告。

## 怎么实现的

**`_setup_managed_agents` 做三件事，其中第三件是隐式契约的核心。** 先断言每个子智能体都有 `name` 与 `description`（缺了直接 `assert` 失败，文案是「All managed agents need both a name and a description!」），然后建成 `{agent.name: agent}` 的字典，最后**改写子智能体自身的字段**：`agent.inputs = {"task": ..., "additional_args": {...}}`、`agent.output_type = "string"`。

这一步是在改别人的对象。意味着同一个智能体实例被两个父智能体托管时，第二次托管会覆盖第一次写的 `inputs`；也意味着一个智能体一旦被托管，它自己的 `inputs` 就不再由它决定。这是「把智能体当工具」必须付的代价——工具的 schema 是父级视角的。

两个参数里 `task` 是必填字符串，`additional_args` 是可空对象（`nullable: True`），描述里举例说明可传图片、DataFrame 等上下文数据。

**名字必须在三个维度上唯一，其中包括父智能体自己。** `_validate_tools_and_managed_agents` 把「工具名 + 托管智能体名 + 自身 `name`」拼成一个列表查重，重复就把重复项列出来报 `ValueError`。把父智能体自己的名字也算进去，是为了挡住「子智能体与父同名」这种在递归托管或可视化树里会造成歧义的情况。

**名字还得是合法的 Python 标识符。** `_validate_name` 用 `is_valid_name`（`str.isidentifier()` 且不是关键字）校验 `name`，不合法则 `ValueError`，文案写明「must be a valid Python identifier and not a reserved keyword」。这条校验发生在每个智能体自己的 `__init__` 里，因此托管智能体的名字在它被构造时就已被检查过一次。真正的约束动因来自 `CodeAgent`：它把 `{**tools, **managed_agents}` 交给执行器当 `static_tools`，名字必须是能写进 Python 代码的标识符；`ToolCallingAgent` 只把名字塞进 JSON schema，本没有这个限制，但两个子类共用基类校验，约束强度由更严的那一侧决定。

**`__call__` 是「加壳」而不是「转发」。** 它对入参做两次模板渲染：

1. `managed_agent.task` 模板：把 `{{name}}` 与 `{{task}}` 填进去，模板正文要求子智能体「不要给一行答案」「final_answer 必须包含简短版、详细版、附加上下文三部分」「即使任务失败也要返回尽可能多的上下文，好让上级能据此行动」；
2. 跑 `self.run(full_task, **kwargs)`；若返回 `RunResult` 就取 `.output`，否则直接用作 `report`；
3. `managed_agent.report` 模板：把 `{{name}}` 与 `{{final_answer}}` 包成「Here is the final answer from your managed agent '…':」这段文本。

所以父智能体收到的**永远是一段自然语言**，哪怕子智能体的 `output` 是结构化对象——它在第 3 步被塞进模板字符串了。这是「工具返回字符串」这条契约的实现位置。

**`provide_run_summary` 会让报告变长很多。** 若开着，在报告后面追加 `<summary_of_work> ... </summary_of_work>` 块，内容是把 `write_memory_to_messages(summary_mode=True)` 的每条消息 `truncate_content` 后逐条拼接（用 `\n---` 分隔）。默认关闭。`summary_mode=True` 的意义是让 memory 序列化成摘要形态而非完整形态，避免把子智能体的全部中间消息灌进父智能体的上下文。

**执行入口在父智能体这一侧只多一个布尔。** `execute_tool_call` 先 `available_tools = {**self.tools, **self.managed_agents}` 合并查名字，再 `is_managed_agent = tool_name in self.managed_agents`，然后分叉调用：

- 托管智能体：`tool(**arguments)`；
- 普通工具：`tool(**arguments, sanitize_inputs_outputs=True)`。

托管智能体不被传 `sanitize_inputs_outputs`，因为它的调用对象是 `MultiStepAgent.__call__`，签名是 `(task, **kwargs)`，这个额外关键字会被 `**kwargs` 吞掉并继续传给 `self.run(...)`——那会变成给 `run` 传一个它不认识的参数。所以「少传一个参数」不是疏忽，而是两条调用链签名不兼容的直接后果。

**错误文案区分「工具」与「团队成员」。** 两者的失败都包成 `AgentToolExecutionError`，但文案不同：托管智能体的失败是「Error executing request to team member '…' … Please try again or request to another team member」；普通工具是「Error executing tool '…' … Please try again or use another tool」。这条差异直接影响模型下一步的行为——是「换个同伴问」还是「换个工具试」。

**`ToolCallingAgent` 与 `CodeAgent` 的暴露方式不同，但都指向同一个 `__call__`。** 前者通过 `tools_and_managed_agents` 属性把合并列表交给 provider 的 function calling；后者在 `run` 开始时调 `self.python_executor.send_tools({**self.tools, **self.managed_agents})`，于是子智能体在解释器里就是一个可以直接调用的函数名。两条路都靠「对象可调用」这一点，不需要额外的适配层。

**远程执行器与托管智能体互斥，并且是硬报错。** `create_python_executor` 里 `if self.managed_agents: raise Exception("Managed agents are not yet supported with remote code execution.")`。原因在机制层面很清楚：远程执行器在别的进程/机器上跑代码，父进程里的 Python 对象没法被注入成那边的可调用名字。这个限制只影响 `CodeAgent`，`ToolCallingAgent` 不受影响（它不需要把对象送进解释器）。

**递归托管可以，序列化也是递归的。** `save` 会为每个子智能体建 `managed_agents/<name>/` 子目录并递归调用 `agent.save(...)`，`relative_path` 用于生成嵌套的 import 路径（`managed_agents.<name>.app`）；`to_dict` 里 `managed_agents` 是子字典列表（递归），`from_dict` 用 `AGENT_REGISTRY` 反查类名后递归重建。`AGENT_REGISTRY` 只登记了 `ToolCallingAgent` 与 `CodeAgent` 两个类，注释写明这层白名单的目的是「防止通过 importlib 动态加载实现任意代码执行」。

**可视化也按树递归。** `visualize_agent_tree` 里 `build_agent_tree` 先画当前智能体的工具表，若存在 `managed_agents` 就加一个分支，逐个画子智能体节点（`CodeAgent` 会额外显示授权导入），然后递归。所以托管关系是这棵树里唯一的分支来源——工具是叶子，子智能体是子树。

**托管提示词模板的键只有两个。** `ManagedAgentPromptTemplate` 是 TypedDict，字段就是 `task` 与 `report`，挂在 `PromptTemplates["managed_agent"]` 下。构造期 `__init__` 会用 `EMPTY_PROMPT_TEMPLATES` 逐键校验自定义模板是否缺键（含嵌套），缺了就断言失败。也就是说这套「加壳」的文案是**可替换的配置**，不是硬编码字符串——换掉 `task` 模板就能改变子智能体的行为约束（比如强制它只输出 JSON）。

## 关键取舍

**源码事实：** 托管智能体的 `inputs` 与 `output_type` 是被父智能体在构造期改写的（直接赋值到子对象上）。共享同一子智能体的多个父级会互相覆盖这两个字段。

**源码事实：** `name` 必须通过 `is_valid_name`（`isidentifier()` 且非关键字），否则 `ValueError`。校验发生在每个智能体自己的 `__init__` 里，因此托管智能体的名字在它被构造时已被检查过一次。

**架构推论：** 这条约束的真正下界由 `CodeAgent` 决定——它把 `{**tools, **managed_agents}` 交给执行器当 `static_tools`，于是名字必须是能写进 Python 代码的标识符。`ToolCallingAgent` 只把名字塞进 JSON schema，本来没有这个限制；两个子类共存于同一套基类校验之下，约束强度由更严的那一侧决定。

**源码事实：** 父级与子级之间传递的只有一段自然语言报告。子智能体的结构化输出在 `report` 模板里被字符串化，父级拿不到原始对象。

**源码事实：** `provide_run_summary` 默认关闭。打开后会把子智能体的 memory 摘要（逐条截断）拼进报告，是「看得更清」与「上下文膨胀」之间的显式开关。

**源码事实：** 托管智能体与远程代码执行不兼容，源码是 `raise Exception` 而非降级。所以「多智能体 + 远程沙箱」在当前版本里无法组合。

**源码事实：** 序列化反序列化都走白名单注册表（`AGENT_REGISTRY` 两个类、`MODEL_REGISTRY`），未登记的类名报 `ValueError` 并列出支持项——这是从「配置文件反序列化」到「任意代码执行」这条经典路径上的显式封堵。

**架构推论：** 「子智能体即工具」这个设计把多智能体降维成了工具调用，好处是父级的循环、护栏、步数上限、追踪全部不需要改；代价是**层级间没有共享状态**：子智能体有自己的 memory、自己的步数预算、自己的最终答案校验。父级既看不到中间过程（除非开 `provide_run_summary`），也无法在子任务跑偏时中途纠正。

**架构推论：** 因为报告是文本，父级必须靠提示词约束来获得结构化结果（源码模板里那三条 `### 1./2./3.` 就是硬编码的格式要求）。这属于「用自然语言做接口契约」，稳定性完全依赖模型遵守——这也是这类框架在多智能体协作上最常见的脆弱点。

**架构推论：** 递归托管（子智能体自己也有托管智能体）在实现上没有阻碍，但没有任何跨层预算协调：每层都有自己的 `max_steps`，总步数是乘积量级。要控成本必须在构造时逐层显式设小预算，框架不会替你算总账。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/agents.py` | `_setup_managed_agents` | 断言 name/description、建名字字典、改写 `inputs` 与 `output_type` |
| `src/smolagents/agents.py` | `_validate_tools_and_managed_agents` | 工具名、托管名、自身名三者查重 |
| `src/smolagents/agents.py` | `_validate_name` | 强制名字是合法 Python 标识符且非关键字 |
| `src/smolagents/agents.py` | `ManagedAgentPromptTemplate` | `task` 与 `report` 两段模板的类型定义 |
| `src/smolagents/agents.py` | `MultiStepAgent.__call__` | 加任务壳 → 跑子运行 → 包报告壳，可选附 memory 摘要 |
| `src/smolagents/agents.py` | `MultiStepAgent.write_memory_to_messages` | 摘要模式下为报告提供内容 |
| `src/smolagents/agents.py` | `ToolCallingAgent.execute_tool_call` | 按 `is_managed_agent` 分叉调用并区分错误文案 |
| `src/smolagents/agents.py` | `ToolCallingAgent.tools_and_managed_agents` | 交给 provider 的可调用合并列表 |
| `src/smolagents/agents.py` | `CodeAgent.create_python_executor` | 拒绝托管智能体与远程执行器组合 |
| `src/smolagents/agents.py` | `CodeAgent._step_stream` 前的 `send_tools` | 把托管智能体注入解释器命名空间 |
| `src/smolagents/agents.py` | `MultiStepAgent.to_dict` / `from_dict` | 子智能体的递归序列化与白名单反序列化 |
| `src/smolagents/agents.py` | `MultiStepAgent.save` | 递归写出 `managed_agents/<name>/` 目录 |
| `src/smolagents/agents.py` | `AGENT_REGISTRY` | 反序列化允许的智能体类白名单 |
| `src/smolagents/monitoring.py` | `visualize_agent_tree` | 递归绘制托管树 |

## 参考

- [smolagents 源码：agents.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/agents.py)
- [smolagents 文档：多智能体示例](https://huggingface.co/docs/smolagents/examples/multi-agents)
- [smolagents 文档：智能体参考](https://huggingface.co/docs/smolagents/reference/agents)

## 与主核 / AI 的关系

「子智能体即工具」在主核里对应的是「一个查询里调用另一个查询」——同样是层级调用，同样只传参数与结果、不共享内部状态。差别在契约的严格性：主核的子查询有类型化的返回结构与名字解析规则，这里的子任务只返回自然语言，格式靠提示词里的 `### 1./2./3.` 约定。前者可静态校验，后者只能事后检查。

三条对 agent 工程有价值的结论。第一，**层级边界要显式**：这类设计里上下级唯一的共享是那段报告文本，任何「让子智能体直接改父状态」的想法都会破坏这层隔离，也破坏可追踪性。第二，**名字是契约**：要求合法标识符这一条看似形式，实际是「能否注入代码执行环境」的前提；任何要跨「代码」与「JSON」两种动作形态的名字，都必须满足更严的那一侧。第三，**预算不会自动汇总**：逐层 `max_steps` 相乘就是总调用量，框架不做总账，做成本估算时必须自己乘。

还有一条与安全直接相关：`save`/`from_dict` 的类名走白名单注册表。任何「从配置重建对象」的设计（工具、模型、智能体、执行器）都应该这样白名单化，否则配置文件就是代码注入入口。这里的实现把 `AGENT_REGISTRY` 与 `MODEL_REGISTRY` 分开登记、未登记即报错并列出支持项，是一个可直接照搬的形态。

选型判断：如果需求是「多个角色协作、需要互相看见过程、需要中途干预」，托管智能体的文本边界会成为主要障碍，得改用显式的编排层（图、消息总线）。如果需求只是「把子任务外包出去、只要结果」，这个包装方式成本最低——加两个字段、实现一个 `__call__` 就完成了。
