---
project: openai-agents
feature: agent-definition
title: Agent 定义与配置合并
page_type: feature
schema_version: 1
refs:
  - path: src/agents/agent.py
    symbols: [AgentBase, Agent, ToolsToFinalOutputResult]
  - path: src/agents/agent.py
    symbols: [StopAtTools, MCPConfig, _initial_model_settings_for_model]
---

# Agent 定义与配置合并

## 一句话职责

`Agent` 是一个不可执行的纯数据类：它只描述「用哪个模型、什么指令、能调哪些工具、能交给谁」，把「怎么跑」完全留给运行器；唯一的逻辑集中在构造期校验、克隆时的浅拷贝语义，以及模型默认参数的隐式/显式区分。

## 机制是什么

两层定义：`AgentBase` 放 `Agent` 与 `RealtimeAgent` 共用的字段（名字、交接描述、工具、MCP 服务器与配置），`Agent` 追加模型相关与流程相关的字段。

`Agent` 自己的字段可分成四组：

| 组 | 字段 | 语义 |
|---|---|---|
| 提示 | `instructions`、`prompt` | 前者是字符串或 `(context, agent) -> str` 函数，后者是服务端 Prompt 对象 |
| 模型 | `model`、`model_settings` | 字符串（由 provider 解析）、`Model` 实例、或 `None`；设置见下节的隐式默认 |
| 编排 | `handoffs`、`tool_use_behavior`、`reset_tool_choice` | 交接目标；工具结果如何变成最终输出；调用工具后是否重置 tool_choice |
| 拦截 | `input_guardrails`、`output_guardrails`、`output_type`、`hooks` | 护栏、输出 schema、本智能体的生命周期钩子 |

三个 TypedDict/数据类承担了「Agent 只是数据」的具体形态：`StopAtTools`（只有一个 `stop_at_tool_names` 列表）、`MCPConfig`（三个可选项：转 strict schema、失败转错误消息、工具名带服务器前缀）、`ToolsToFinalOutputResult`（`is_final_output` 与 `final_output` 两个字段）。它们都不是行为，只是配置的形状。

`tool_use_behavior` 是唯一以联合类型表达的「策略字段」，四种取值：字符串 `"run_llm_again"`（默认，工具结果回喂模型）、字符串 `"stop_on_first_tool"`、`StopAtTools` 字典、以及一个函数 `(context, tool_results) -> ToolsToFinalOutputResult`。

## 怎么实现的

**构造期只做类型校验，不做语义装配。** `__post_init__` 逐字段 `isinstance` 检查，且刻意拒绝「可隐式转换的近似值」：`name` 必须是 `str`，`instructions` 必须是 `str`/可调用/`None`，`model` 必须是 `str`/`Model`/`None`，`tool_use_behavior` 必须是两个合法字符串之一、`dict` 或可调用。这些检查不会触发网络或文件访问，也不解析任何相对关系。

**模型默认值的隐式/显式区分。** 这是本类唯一的非平凡状态机。`model_settings` 的 `default_factory` 是 `_default_agent_model_settings`，它包了一层 `get_default_model_settings` 并在返回对象的 `__dict__` 里打上 `_agent_default_settings = True` 标记。`__post_init__` 检查这个标记：

- 若用户没传（标记仍在），则调用 `_initial_model_settings_for_model(self.model)` 按「解析后的模型名」重新取默认值（`None` → 全局默认模型的设置；字符串 → 该名字的默认设置；`Model` 实例 → 空 `ModelSettings()`），并把 `_agent_assigned_fields` 置为空集合；
- 若用户传了，`_implicit_model_settings` 保持 `None`，即「一切以用户给的为准」。

配套的两个方法把这条区分变成可查询的事实：`_uses_implicit_model_settings()` 判断「当前设置仍是当初那套默认值且没有被任何赋值改动过」，`_implicit_request_fields()` 返回 `reasoning`/`verbosity` 这两个字段中「仍是默认值且未被显式赋值」的部分。`ModelSettings.__setattr__` 在每次赋值时把字段名记进 `_agent_assigned_fields`，所以哪怕是「赋成与默认相同的值」也会被记成显式赋值。

这套机制的用处是：模型名可以在运行期被 `RunConfig.model` 覆盖，此时运行器需要知道哪些设置可以按新模型重新推导（隐式默认），哪些必须原样保留（用户显式指定）。没有这层区分，「Agent 用默认设置 + RunConfig 换模型」会导致旧模型的 reasoning 默认值被带到新模型上。

**`clone` 的浅拷贝语义写在 docstring 里。** 实现是 `dataclasses.replace`，`docstring` 用大段文字说明后果：不传的列表属性（`tools`、`handoffs`、`mcp_servers`、两个护栏列表）在克隆体里就是原对象的那个列表本身，通过任一智能体 `append` 都会改到另一个；传了 `tools=agent.tools` 仍然共享，只有 `tools=[*agent.tools, extra]` 才是新列表（元素对象仍共享）。`clone` 还处理了隐式设置：若原智能体处于隐式默认状态且调用方改了 `model`，就用新模型的默认设置替换；若完全不传 `model_settings`，克隆体继承原对象的 `_implicit_model_settings` 元组。

**工具赋值被拦截。** `AgentBase.__setattr__` 对 `tools` 单独分流，交给 `assign_agent_tools`；该函数会在锁内检查「同一个 Agent 是否正被并发运行使用」，若在而且赋的是新列表，就把所有活跃运行标记为冲突，随后 `snapshot_agent_tools` 会抛 `UserError` 提示「用 `agent.clone(tools=[...])` 或改为上下文启用的方式」。这是把「Agent 是共享的可变数据」这个隐患变成显式的运行期错误。

**工具清单是「快照 + 拼装」，不是存储的原样。** `get_all_tools` 先 `snapshot_agent_tools(self)` 取一份冻结的工具元组，再取 MCP 工具，然后并发跑每个 `FunctionTool.is_enabled`（布尔或 `(context, agent) -> bool` 回调），过滤后与 MCP 工具合并，再 `prune_orphaned_tool_search_tools` 修剪悬空的工具搜索项，最后做 Codex 工具重名校验。

**提示词与 Prompt 走两条取法。** `get_system_prompt` 支持字符串与函数两种 `instructions`；函数分支用 `inspect.signature` 强制**恰好两个参数**，并且不靠 `iscoroutinefunction` 判断是否需要 `await`，而是先调用再看返回值是否可等待——注释说明这是为了覆盖「实例的 `__call__` 是 async」这种不是协程函数的写法。`get_prompt` 则把 `prompt` 交给 `PromptUtil.to_model_input` 转成 Responses API 的 prompt 参数。

## 关键取舍

**源码事实：** `Agent` 没有任何执行方法。它不持有运行器、不持有状态、不发起调用，只提供 `get_system_prompt`/`get_prompt`/`get_all_tools` 三个「把自己描述转成运行期输入」的查询方法。这正是它与 langgraph 的 `StateGraph`（构造期对象、编译后执行）、autogen 的 `BaseAgent`（有 `on_message` 入口、自己参与执行）最根本的差异：这里的智能体没有消息入口。

**源码事实：** `tool_use_behavior` 的四种取值不在 Agent 里分派，而在 `check_for_final_output_from_tools` 里判：`"stop_on_first_tool"` 取第一条工具结果作最终输出；`StopAtTools` 按名字或限定名匹配（`qualified_name` 也参与比较，以支持带命名空间的工具）；可调用对象必须返回 `ToolsToFinalOutputResult`，否则抛 `UserError`。Agent 侧只负责「这个值类型合法」。

**源码事实：** `model_settings` 的隐式默认标记写在对象 `__dict__` 里（`_agent_default_settings`、`_agent_assigned_fields`），不是 dataclass 字段。这样它不会进入序列化、不会出现在 `fields()` 里，也不会影响相等比较——但它确实是可变状态，随 `ModelSettings` 实例一起被 `clone` 或共享。

**源码事实：** `__post_init__` 的校验粒度是「类型对不对」，不校验「组合对不对」。例如同时传 `instructions` 与 `prompt` 不会报错；`output_type` 是「类型、`AgentOutputSchemaBase` 实例、或带泛型参数的对象」三选一，校验里用 `get_origin(...) is not None` 放过泛型形态。

**架构推论：** 把「默认值推导」放在构造期而不是调用期，使 `Agent` 一旦建好就是确定的：同一份 `Agent` 被多条运行共享时，设置不会随运行变化。代价是「换模型」这个动作必须显式走 `RunConfig.model` 或 `clone(model=...)`，不能让运行器就地改 Agent 的设置。

**架构推论：** 因为 Agent 不可执行，「测试一个智能体」在这个 SDK 里意味着「用脚本化的模型跑一次 Runner」，而不是「直接调 Agent 的方法」。这与 autogen 里可以 `await agent.on_message(...)` 的单测写法形成对比，也让 Agent 天然可以跨进程、跨序列化边界传递（`RunState` 只存对 agent 的引用与身份键，不存 Agent 本身）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/agent.py` | `AgentBase` | 与 RealtimeAgent 共用的字段容器，含工具赋值拦截与 MCP 工具拼装 |
| `src/agents/agent.py` | `AgentBase.__setattr__` | 对 `tools` 分流到 `assign_agent_tools`，检测并发运行的配置冲突 |
| `src/agents/agent.py` | `AgentBase.get_all_tools` | 快照 + MCP + 启用过滤 + 悬空修剪 + 重名校验 |
| `src/agents/agent.py` | `Agent` | 智能体定义主体：提示、模型、编排、拦截四组字段 |
| `src/agents/agent.py` | `Agent.__post_init__` | 逐字段类型校验，并处理模型设置的隐式默认 |
| `src/agents/agent.py` | `Agent.clone` | `dataclasses.replace` 浅拷贝，含隐式设置继承规则 |
| `src/agents/agent.py` | `Agent._uses_implicit_model_settings` | 判断设置是否仍是未被改动的默认值 |
| `src/agents/agent.py` | `Agent._implicit_request_fields` | 返回可随模型名重新推导的字段集合 |
| `src/agents/agent.py` | `Agent.get_system_prompt` | 字符串或双参函数两种 `instructions` |
| `src/agents/agent.py` | `ToolsToFinalOutputResult` | 工具结果转最终输出的判定结果 |
| `src/agents/agent.py` | `StopAtTools` | 按工具名提前终止的配置形状 |
| `src/agents/agent.py` | `MCPConfig` | MCP 工具接入的三个开关 |
| `src/agents/agent.py` | `_initial_model_settings_for_model` | 按模型形态取初始默认设置 |

## 参考

- [openai-agents-python 源码：agent.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/agent.py)
- [Agents SDK 文档：Agents](https://openai.github.io/openai-agents-python/agents/)
- [Agents SDK API：agent 模块](https://openai.github.io/openai-agents-python/ref/agent/)

## 与主核 / AI 的关系

`Agent` 与主核里的「表定义 / 索引定义」是同一类东西：一份声明式描述，构造期校验形状，执行期由别的东西（这里是 Runner，那里是查询执行器）读取它来驱动行为。区别在于主核的定义会编译成物理结构并长期驻留，这里的 `Agent` 每次运行都被重新读取一遍工具清单与提示词——所以它必须保持廉价可读。

「隐式默认」那套标记值得在大模型工程里单独记住：模型默认参数（reasoning effort、verbosity）与模型名是绑定的，而 Agent 与 RunConfig 分别可以指定模型。若不记录「这个值来自默认还是来自用户」，配置合并必然会串味。任何做多层配置覆盖的系统都要回答同一个问题：**空值表示「没设」还是表示「显式设为空」**，这里的答案是用一个不可序列化的旁挂标记来区分。

对选型的判断价值：看一个 agent 框架的 `Agent` 类里有没有执行入口（消息处理方法、`run` 方法）。有，说明这个框架把控制流交给智能体自身，编排自由度更高但行为更难预测；没有，说明控制流在框架手里，可预测、可追踪，但能做的东西受固定循环限制。
