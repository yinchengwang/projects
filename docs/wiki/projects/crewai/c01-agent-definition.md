---
project: crewai
feature: agent-definition
title: Agent 定义与属性校验
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/agent/core.py
    symbols: [Agent, validate_from_repository, post_init_setup]
  - path: lib/crewai/src/crewai/agents/agent_builder/base_agent.py
    symbols: [BaseAgent, execute_task]
---

# Agent 定义与属性校验

## 一句话职责

`Agent` 是 `BaseAgent` 的具体实现：`BaseAgent` 定下「角色 / 目标 / 背景 + 一组工具 + 一个执行器」的抽象契约与一批校验器，`Agent` 则在 `before` 校验器里合并远端仓库配置、在 `after` 校验器 `post_init_setup` 里把模型、执行器、技能、规划配置一次性装配好。

## 机制是什么

`BaseAgent` 是 `BaseModel` + `ABC` + `AgentMeta` 三者的组合：`BaseModel` 给字段校验，`ABC` 给 `@abstractmethod` 的执行契约，`AgentMeta` 是一个自定义元类，专门在类创建时把 `post_init_setup` 包一层。

抽象契约只有五个抽象方法：`execute_task`、`aexecute_task`、`create_agent_executor`、`get_delegation_tools`、`get_platform_tools`、`get_mcp_tools`（后三个是工具来源的扩展点）。也就是说「一个智能体」在框架眼里的最小定义是：**能被要求执行任务、能自建执行器、能提供委派 / 平台 / MCP 三类工具**。其余字段都只是配置。

必填字段只有三个：`role`、`goal`、`backstory`，都是 `Field(description=...)` 且无默认值，Pydantic 会强制要求。`validate_and_set_attributes` 再补一道 `getattr(self, field) is None` 检查——因为在 `config` 路径里这些字段可能被显式传 `None` 绕开 Pydantic 的必填判定。缺失时报「{field} must be provided either directly or through config」。

字段分几组，理解分组比记住每个字段重要：

- **身份**：`id`（`UUID4`、`frozen`、`_deny_user_set_id` 禁止用户设置，除非 `from_checkpoint`）、`role` / `goal` / `backstory`（`key` 由这三者的**原始值**算 md5）。
- **模型**：`llm`（`str | BaseLLM | None`，由 `_validate_llm_ref` 前置校验）、`function_calling_llm`（已标 deprecated）、`max_tokens`。
- **执行控制**：`max_iter`（默认 25）、`max_rpm`、`max_retry_limit`（默认 2）、`max_execution_time`、`respect_context_window`（默认 `True`）、`use_better_compaction`（默认 `True`）、`verbose`、`allow_delegation`（默认 `False`）。
- **工具与能力**：`tools`（`default_factory=list`）、`apps`、`mcps`、`skills`、`knowledge_sources`、`knowledge_storage`、`memory`、`artifacts`、`allow_code_execution`（已废弃）。
- **输出与安全**：`guardrail` / `guardrail_max_retries`（默认 3）、`security_config`、`callbacks`、`step_callback`。
- **持久化**：`checkpoint`、`execution_context`、`checkpoint_kickoff_event_id`。

## 怎么实现的

**`validate_from_repository` 是 `before` 校验器，顺序很讲究。** 它先 `v = process_config(dict(v), cls)`——把 `agents.yaml` 里的 `config` 展平，**然后**才看 `v.get("from_repository")`。源码注释直说了原因：如果先看 `from_repository`，写在 `yaml` 配置里的它就永远看不到。拿到名字后 `return load_agent_from_repository(from_repository) | v`——用字典的 `|` 合并，**仓库给的值是底、调用方给的值覆盖在上面**。`dict(v)` 那次拷贝是为了不破坏调用方的映射（注释写明「Copy so the caller's mapping keeps its config key」）。

`load_agent_from_repository` 住在 `utilities/agent_utils.py`，走的是远端仓库：取 auth token、建 `PlusAPI`、`client.get_agent(name)`；404 抛 `AgentRepositoryError` 并提示用 `crewai org switch` 检查组织；非 200 抛带 `response.text` 的错误。取回 JSON 后逐字段转成属性字典：`skill_versions` 单独跳过（会被折进 `skills` 引用里）、`skills` 走 `_pin_skill_refs`、`tools` 则按 `{"module": ..., "name": ..., "init_params": ...}` 三元组动态 `importlib.import_module` + `getattr` + 实例化，且**允许一个工具项产出列表**（`isinstance(tool_value, list)` 时 `extend`）。`None` 值一律跳过。

**`post_init_setup` 是整个装配过程的核心，也是唯一带「重入保护」的校验器。** 开头第一段就是：

```
if self._overlay_read:
    self.llm = create_llm(self.llm)
else:
    declared = create_llm(self.llm)
    self._declared_llm = declared
    self.llm = self._resolved_overlay(declared)
    self._overlay_read = True
```

为什么需要这个分支？注释解释得很清楚：事件总线在智能体**第一次发事件**时会把它注册进 `RuntimeState`，而那一步会**再次**跑一遍这个 `after` 校验器（同一个对象）。如果没有 `_overlay_read` 这道锁，第二次进入时会重新解析 `llm_overlay`，把已经跑着的 `llm` 换掉，还会丢掉挂在它上面的运行时状态（例如 `stream` 标志）。这是一处真实的「重复校验」陷阱。

`llm_overlay` 的解析逻辑分两层：`_resolved_overlay(declared)` 调 `overlay_llm_for(self.role, declared)`——**角色键优先于模型键，都没有就回落到的 `declared`**；解析成果若不是原来那个对象，就记进 `_overlay_built`。`_declared_now()` 则负责「此刻声明的模型是什么」：如果 `llm` 为空或不是 overlay 造出来的，就 `create_llm(self.llm)` 重新解析并更新 `_declared_llm`——这样调用方后来手动赋的 `llm` 会被认成新的声明值。

接着依次：`function_calling_llm` 若是字符串就 `create_llm`；`if not self.agent_executor: self._setup_agent_executor()`；`allow_code_execution` 为真时发废弃警告（提示改用 E2B / Modal 这类沙箱服务）；`set_skills()`；最后是规划配置的两条归一化——`reasoning` 为真且 `planning_config` 为空时发废弃警告并把 `max_reasoning_attempts` 搬进 `PlanningConfig(max_attempts=...)`；`planning` 为真且 `planning_config` 为空时造 `PlanningConfig(reasoning_effort="low", max_attempts=1)`（注释的理由是「裸 `planning=True` 应该是有界的，且默认不产生每步的 PlannerObserver 模型调用」）。

`planning_enabled` 属性把两者合一：`planning_config is not None or self.planning`。

**`AgentMeta` 做什么。** 它在 `__new__` 里取出类命名空间中的 `post_init_setup`，包成 `post_init_setup_with_extensions`——先调原函数，再检查 `getattr(self, "a2a", None)`，非空就建扩展注册表并 `wrap_agent_with_a2a_instance(self, ...)`。包装时用了 `warnings.catch_warnings()` 过滤掉「overrides an existing Pydantic...」告警，因为重复挂 `model_validator(mode="after")` 会触发它。所以 `a2a` 字段不是一个普通配置项，它是一处**元类级别的扩展点**：只要子类的 `post_init_setup` 存在，`a2a` 就会被自动接上，不需要在 `post_init_setup` 里写代码。

**`BaseAgent` 里两处内容相同的校验器。** `validate_and_set_attributes` 与 `set_private_attrs` 的代码几乎逐行相同：都做 `role`/`goal`/`backstory` 的空检查（后者没有）、`self._logger = Logger(verbose=self.verbose)`、`if self.max_rpm and not self._rpm_controller: self._rpm_controller = RPMController(...)`、`if not self._token_process: self._token_process = TokenProcess()`。前者多一个 `security_config is None` 时补 `SecurityConfig()`。这是源码里真实存在的重复，靠「已设置则不再覆盖」的判空使重复执行无害。

**`_setup_agent_executor` 的缓存判定。** 它先记一个 `_constructor_cache_opt_in`：`bool(self.cache and (self.cache_handler is not None or "cache" in self.model_fields_set))`。然后 `opted_in = self.cache_handler is not None or ("cache" in self.model_fields_set and self.cache)`。两个表达式的差别只有前半段，前者多要求 `cache` 为真。注释说明了用途——记录的是「构造期主动开启」，以便 `copy()` 能把它与「运行时被团队塞进来的 handler」区分开：后者不该让副本也变成缓存使用者。开启时若没有 handler 就新建一个再 `set_cache_handler`。

**`copy()` 的排除集合。** 排除 `id`、一组私有运行时对象（`_logger`、`_rpm_controller`、`_token_process`、`tools_handler`、`cache_handler`、`agent_executor`）、以及 `tools` / `llm` / `knowledge*` / `apps` / `mcps` / `actions`——这些要么是共享引用、要么要单独处理。然后 `{k: v for k, v in copied_data.items() if v is not None}` 丢掉空值，最后 `type(self)(**copied_data, llm=existing_llm, tools=self.tools, knowledge_sources=..., ...)`——**模型与工具是共享的**而不是深拷贝。有段注释专门解释 `cache` 的处理：若 `cache` 不在 `model_fields_set` 里就把它从 dump 里删掉，但如果 `_constructor_cache_opt_in` 为真则补 `cache=True`，让副本自己建 handler。

**`Agent.execute_task` 的编排。** 它是 `BaseAgent` 抽象方法的实现，外形是「准备 → 取知识 → 定工具 → 跑 → 收尾」：

```
spreadsheets = read_spreadsheet_inputs(task_input_text(task))
scope = enter_spreadsheet_scope(spreadsheets)
try: return self._execute_task(task, context, tools, spreadsheets)
finally: exit_spreadsheet_scope(scope)
```

`_prepare_task_execution` 依次做：`get_env_context()`、`reset_tool_failures()`、`tools_handler.last_used_tool = None`、`task.prompt()`、`build_task_prompt_with_schema(task, ...)`、`format_task_with_context(task_prompt, context)`、`_retrieve_memory_context(task, task_prompt)`。其中记忆检索只在 `self.memory` 或 `crew._memory` 存在时才跑，`recall(task.description, limit=5)` 并把命中的格式化结果前置成「Relevant memories:」。

`_finalize_task_prompt` 只做三件事：`prepare_tools(self, tools, task)`（解析最终工具集并绑到执行器）、`_emit_skill_usage(task)`（对披露等级 ≥ INSTRUCTIONS 的技能逐个发 `SkillUsedEvent`）、`apply_training_data(self, task_prompt)`。

**超时的实现有一处刻意的非常规写法。** `_execute_with_timeout` 用 `concurrent.futures.ThreadPoolExecutor` 提交到 `ctx.run`（复制过 contextvars），`future.result(timeout=...)` 超时就 `cancel()` 并抛 `TimeoutError`（消息里带任务描述与建议调大 `max_execution_time`）。`_passthrough_exceptions` 被单独 `except` 后原样抛出——注释说，把「故意的停止」包成 `RuntimeError` 会藏起来，反而触发重试循环。`finally` 里写的是 `executor.shutdown(wait=False, cancel_futures=True)` 而不是 `with` 语法，理由也在注释里：上下文管理器会 `shutdown(wait=True)`，把已经超时的工作等到结束，于是超时时间形同虚设。

**同步入口的护栏。** `_execute_without_timeout` 调 `self.agent_executor.invoke({...})`，然后：

```
if inspect.isawaitable(invoke_result):
    invoke_result.close()
    raise RuntimeError("... Use `agent.kickoff_async()` / `crew.kickoff_async()` ...")
```

因为执行器内部有「在事件循环里就返回 coroutine」的逻辑（`is_inside_event_loop()`），同步入口拿到 coroutine 时必须明确报错而不是硬跑，否则会在运行中的 loop 上嵌套调用。`invoke_result.close()` 是为了避免 coroutine 未被 await 的告警。

**重试次数在异常路径上计数。** `_check_execution_error(e, task)` 先发 `AgentExecutionErrorEvent`（注释说每次重试都会重新进入 `execute_task`、开出新的事件作用域，所以每个失败的尝试都必须关掉自己那个）；litellm 异常直接透传；否则 `self._times_executed += 1`，超过 `max_retry_limit` 才抛。`_times_executed` 是 `PrivateAttr`，因此**计数跨任务累积**而不是每次任务清零。

## 关键取舍

**源码事实：** `post_init_setup` 用 `_overlay_read` 布尔量做重入保护，因为同一个实例会被 Pydantic 重复校验（事件总线注册时触发）。这是「`after` 校验器带副作用」的必然代价。

**源码事实：** `validate_from_repository` 先展平 `config` 再读 `from_repository`，且用 `仓库 | 调用方` 的合并方向。这使 `agents.yaml` 里既能写 `from_repository`，又能局部覆盖仓库里的任意字段。

**源码事实：** 仓库里的 `tools` 用「模块名 + 类名 + 初始化参数」三元组动态导入，一个条目可以展开成多个工具。这让远端配置能引用任意 Python 类，也意味着配置的可信边界等于代码执行边界。

**源码事实：** `_setup_agent_executor` 用 `model_fields_set` 区分「用户显式写了 `cache=True`」与「用了默认值」。默认值不该被当成开启意图，这是 Pydantic 字段语义里最容易被忽略的一处。

**源码事实：** `copy()` 共享 `llm` 与 `tools` 引用而不是深拷贝，所以副本与原件跑在同一个模型实例上。要独立需要调用方自己换。

**源码事实：** `_times_executed` 只在异常路径递增。所以 `max_retry_limit` 控制的是「连续失败次数」，成功的任务不会重置它——多个任务连续失败会累积到同一个计数上。

**架构推论：** 把必填字段（`role`/`goal`/`backstory`）与「绝大多数字段都有默认值」并列，使 `Agent(role=..., goal=..., backstory=...)` 就能跑起来。这是有意的「低门槛 + 可深调」设计。

**架构推论：** 抽象方法把「工具来源」拆成委派 / 平台 / MCP 三个独立入口，而不是一个统一的 `get_tools()`。这让每种来源可以有各自的失败语义（委派工具永远可造，平台工具可能因未授权而不可用，MCP 需要连接），代价是 `_prepare_tools` 里要按来源逐个分支。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/agent/core.py` | `Agent` | 具体智能体：字段定义、执行入口、执行器参数更新 |
| `lib/crewai/src/crewai/agent/core.py` | `validate_from_repository` | `before` 校验：展平 config 后合并远端仓库配置，调用方优先 |
| `lib/crewai/src/crewai/agent/core.py` | `post_init_setup` | `after` 装配：模型解析与 overlay、执行器、技能、规划配置，含重入保护 |
| `lib/crewai/src/crewai/agent/core.py` | `_setup_agent_executor` | 构造期缓存开关判定，记录「构造期主动开启」标记 |
| `lib/crewai/src/crewai/agent/core.py` | `execute_task` | 同步执行入口：读表格输入、开作用域、转内部执行 |
| `lib/crewai/src/crewai/agent/core.py` | `_execute_with_timeout` | 超时实现：线程池 + `cancel_futures`，不用 `with` 以免等待 |
| `lib/crewai/src/crewai/agents/agent_builder/base_agent.py` | `BaseAgent` | 抽象基类：必填字段、校验器、`copy()`、`key` |
| `lib/crewai/src/crewai/agents/agent_builder/base_agent.py` | `execute_task` | 抽象方法，定义同步执行契约 |
| `lib/crewai/src/crewai/agents/agent_builder/base_agent.py` | `validate_and_set_attributes` | 角色三元组空检查与私有状态补建 |
| `lib/crewai/src/crewai/agent/internal/meta.py` | `AgentMeta` | 元类：把 `post_init_setup` 包一层以自动接上 `a2a` 扩展 |
| `lib/crewai/src/crewai/utilities/agent_utils.py` | `load_agent_from_repository` | 远端仓库取智能体配置，动态导入工具类 |

## 参考

- [CrewAI 源码：agent/core.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/agent/core.py)
- [CrewAI 源码：base_agent.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/agents/agent_builder/base_agent.py)
- [CrewAI 文档：智能体概念](https://docs.crewai.com/en/concepts/agents)

## 与主核 / AI 的关系

`BaseAgent` + `Agent` 的两层结构与主核里「接口定义 + 具体实现」的分层一致，但这里多了一层**元类注入**：`AgentMeta` 让子类的 `post_init_setup` 自动获得扩展能力，而不需要子类知道扩展的存在。主核里类似的需求通常靠显式的初始化顺序或注册表，不会用元类改写校验器——元类注入在存储引擎这类需要极致可预测性的代码里是不可接受的隐式行为，而在这里它是可接受的，因为智能体的构造本身就是「一次性的复杂装配」。

对 AI 工程的实际意义在于：智能体的构造期比运行期更容易出错且更难排查。`post_init_setup` 里那些看似繁琐的分支（重入保护、overlay 回落、规划配置归一化、废弃字段警告）都在做同一件事——把五花八门的输入形态收敛成一种确定的运行状态。特别是 `_overlay_read` 那处保护，它说明了一个普遍规律：一旦校验器里放了有副作用的操作，就必须假设它会被调用不止一次。
