---
project: pydantic-ai
feature: agent-definition
title: Agent 定义与抽象基类
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/agent/__init__.py
    symbols: [Agent]
  - path: pydantic_ai_slim/pydantic_ai/agent/abstract.py
    symbols: [AbstractAgent, AgentRetries]
---

# Agent 定义与抽象基类

## 一句话职责

`AbstractAgent` 是一份「谁想冒充 Agent 就必须提供什么」的契约（几个只读属性 + 一组 run / run_sync / run_stream 入口），`Agent` 是它的具体实现：构造期把散装参数收成能力树、工具集、输出 schema 与一批 ContextVar 形式的 override 覆盖点。

## 机制是什么

两层结构：`AbstractAgent` 只声明**能力面**，`Agent` 负责**装配**。

抽象面只有七样东西：`model`、`name`（可读写）、`description`（可读写）、`deps_type`、`output_type`、`event_stream_handler`、`toolsets`；外加一个**非抽象但有实际含义**的 `root_capability`（基类实现直接 `raise NotImplementedError`，`Agent` 覆盖）。方法面上，`run` / `run_sync` / `run_stream` / `run_stream_sync` / `run_stream_events` / `iter` 全部定义在基类上，且**都靠 `iter()` 兜底**：`run()` 内部 `async with self.iter(...) as agent_run`，再反复 `next(event)` 直到 `StopAsyncIteration`。

这个「一切经由 iter」的设计是整页的关键。子类只要实现 `iter()`（以及那几个属性）就能获得全部运行入口，因为 run / run_sync / run_stream 都只是在 iter 之上加一层驱动或包装。装饰器层则完全是 `Agent` 自己的事——`tool`、`system_prompt`、`instructions`、`output_validator`、`on_event` 等都需要往具体字段里写东西。

`AgentRetries` 是个 `TypedDict(total=False)`，两个键 `tools` 与 `output`。它存在的意义是把「一张整数」的简写展开成两类预算：`Agent(retries=N)` 相当于 `{'tools': N, 'output': N}`。两类预算的语义在 docstring 里已经分明：`tools` 管工具调用重试，`output` 在文本路径上是整轮共享的预算、在工具路径上是每个输出工具的默认 `max_retries`。

## 怎么实现的

**`AbstractAgent` 的抽象成员。** 逐个都是 `raise NotImplementedError` 的空壳，且大多带 `@property`：

- `model` / `deps_type` / `output_type` / `event_stream_handler` / `toolsets` 只读；
- `name` 与 `description` 成对出现（getter 与 setter 都得实现），因为 `_infer_name` 会从调用栈推断名字并回写，`description` 也允许运行前被改写；
- `root_capability` 写 `raise NotImplementedError` 但**没标 `@abstractmethod`**——这处不一致值得指出：它不是语法错误（继承者没有义务实现），但它意味着「用 isinstance 检查是否实现了 `root_capability` 并不可靠」，只有运行到才会暴露。

`_get_validation_context()` 是唯一有默认实现的非抽象方法，返回 `None`；`system_prompt_parts(...)` 也是「基类返回空列表，具体子类覆盖」的形态，它的用途是在不跑一次真实 run 的前提下重建 system prompt 部件（例如 UI 适配器或历史压缩后重建 message_history）。

**`run()` 与 `iter()` 的关系。** `run()` 的实现结构是：

```
agent_run = node = _stream_step = None
try:
    async with self.iter(...全部参数...) as agent_run:
        # 有 event_stream_handler 时构造 _stream_and_advance，否则用普通推进
        ...
```

它**显式通过 `agent_run.next()` 驱动**而不是 `async for`，源码注释给了理由：`next()` 让能力钩子对每个节点都触发；需要把节点事件交给 `run()` 级 `event_stream_handler` 时，还得自定义 step 函数。注释还点明了顺序约束：流式发生在 `before_node_run` **之后**（该钩子可能替换节点）、`wrap_node_run` **之内**。

**`Agent` 的字段与私有状态的分层。** 类体上声明的字段几乎全用下划线前缀（`_model`、`_name`、`_description`、`_output_type`、`_deps_type`、`_output_schema`、`_output_validators`、`_instructions`、`_system_prompts`、`_system_prompt_functions`、`_system_prompt_dynamic_functions`、`_function_toolset`、`_output_toolset`、`_user_toolsets`、`_max_output_retries`、`_max_tool_retries`、`_tool_timeout`、`_validation_context`、`_event_stream_handler`、`_concurrency_limiter`、`_root_capability`、`_cap_toolsets`、`_cap_instructions`、`_cap_native_tools`、`_cap_model_settings`），只有少数是公开的（`model_settings`、`end_strategy`）。这是有意的：公开面通过 `@property` 暴露（`model`、`name`、`description`、`deps_type`、`output_type`、`event_stream_handler`、`validation_context`、`instrument`），写入也要过 setter，于是「谁改了 Agent 的配置」在类型层面是可追踪的。

**构造期的顺序是有语义的。** `__init__` 大致分这些阶段：

1. `capabilities = wrap_capability_funcs(capabilities)`——把 `CapabilityFunc`（接收 `RunContext` 返回能力的函数）包装成能力对象。
2. `_inject_auto_capabilities(capabilities)`——按需补上框架自带能力。
3. `self._event_hooks = Hooks()`——**单独存一份**，注释说明了原因：`@agent.on_event` 注册的监听器要能在 `override(root_capability=...)` 之后存活，跟 `@agent.tool` 的工具能活过 `override(toolsets=...)` 是同一个诉求；而且它不贡献 instructions / tools / model settings，没监听器时 `listens_to()` 为假、派发闸门直接跳过。
4. `self._root_capability = CombinedCapability(capabilities)`，接着 `_validate_capability_ids(...)`，再 `_combine_duplicate_capabilities(self._root_capability, [capabilities])`（注释解释了「同一个 agent 上两个同 id 能力在读取任何贡献之前就已经合起来」，否则 native tool 校验会把同 id 的两个 `WebSearch` 判成冲突），再 `_validate_instruction_source_ids(...)`。
5. `self._model = model`——**故意不在这里 `infer_model`**，注释写明理由：能力可能自己解释模型 ID，提前推断会构造错供应商并触发认证副作用，而耐久能力需要在 `for_agent` 之后重建模型。
6. `self._output_schema = _output.OutputSchema[OutputDataT].build(output_type)`；`_output_validators = []`。
7. instructions 归一化：`_instructions.normalize_instructions(instructions)` 逐个包成 `sourced_instruction(...)`，其中字面量 instructions 的 `name_source` 是 `AgentInstructionSource()`——这让每条字面量指令在历史重写时是可寻址的；函数式指令只有 `@agent.instructions(name=...)` 给了名字才可寻址。
8. `retry_overrides = _normalize_agent_retry_overrides(retries)` → `resolved_retries = _normalize_agent_retries(...)` → 拆成 `_max_tool_retries` / `_max_output_retries`。`tool_timeout` 必须为正，否则 `UserError`。
9. 输出工具集：`self._output_toolset = self._output_schema.toolset`，若它存在且 `max_retries is None` 就填成 `_max_output_retries`。
10. **工具集故意留 `max_retries=None`**：源码注释明确这是为了让 agent 级预算在运行时经 `ToolManager.default_max_retries` → `RunContext.max_retries` 解析，从而能被 `run`/`iter`/`override` 覆盖，而不是烘焙进每个工具。显式的 per-tool / per-toolset 预算仍按 `tool.max_retries → toolset.max_retries → ctx.max_retries` 的链优先。
11. 用户工具集拆成两类：非 `AbstractToolset` 的包成 `DynamicToolset(toolset_func=...)` 记入 `_dynamic_toolsets`（并记录构造期数量 `_constructor_dynamic_toolset_count`），是 `AbstractToolset` 的直接进 `_user_toolsets`。
12. 一长串 `ContextVar`：`_override_name`、`_override_deps`、`_override_model`、`_override_toolsets`、`_override_tools`、`_override_native_tools`、`_override_instructions`、`_override_metadata`、`_override_model_settings`、`_override_output_retries`、`_override_tool_retries`、`_override_workspace`、`_override_root_capability`。**`override()` 就是靠 set/reset 这些 ContextVar 实现的**，所以同一个 Agent 在并发 run 之间不会串味，且嵌套 override 自然是栈。
13. 能力绑定**分两轮**：`bind_capabilities_tier(..., innermost=False)` 先绑外层能力，`get_toolset()` 抽成 `_cap_toolsets`（因此进入 `toolsets`），然后 `bind_capabilities_tier(..., innermost=True)` 绑 `innermost` 那一档（耐久能力）。注释解释了这条不对称：`innermost` 能力包裹所有工具集，所以必须等其他能力都绑完并把贡献的工具集抽出来之后才绑；代价是 `innermost` 能力**不能贡献自己的工具集**。
14. 若 `model` 非空、`defer_model_check` 为假、且根能力没有 `has_resolve_model_id`，才 `self._model = models.infer_model(model)`。
15. 用 `apply(static_capabilities.append)` 重新收集静态能力，做**第二次** id 校验——因为 `for_agent` 可能替换掉某个能力，替换出来的也要过同样的严格检查。
16. 抽取能力贡献：`_cap_instructions`、`_cap_native_tools`（再 `_validate_native_tool_ids`）、`_cap_model_settings`。
17. 最后 `CombinedToolset(self.toolsets)`——注释说这一步在构造期就把工具集的稳定 id 校验一遍，避免等到 run 里铸指令 id 或派发工具时才炸。

**`from_spec` / `from_file` / `override` / `iter` 四个入口。** `from_spec` 把「标量字段当默认值、能力列表合并」这条规则写得很直白：`effective_model = model or validated_spec.model`；`name=name or validated_spec.name`；`end_strategy=end_strategy if end_strategy is not None else validated_spec.end_strategy`（这里用 `is not None` 而不是 `or`，因为 `'graceful'` / `'early'` 都是真值但语义要区分）；instructions 是 `spec` 与参数的**拼接**而不是二选一；`retries` 走 `_merge_retries_with_spec`；`capabilities` 是 `spec 的 + 参数的`。

`_resolve_spec` 是运行期版本（`run`/`override` 里用），返回 `_ResolvedSpec`，与构造期有一个重要差别：它会把 spec 里**运行期不支持的字段**逐条告警并忽略。读取方式特意用 `validated_spec.__dict__[field_name]` 而不是 `getattr`——注释说这是为了**避免触发 Pydantic 对已废弃字段的 deprecation warning**。

`override` 用一个 `@contextmanager` 包住「设置 ContextVar → yield → 复原」；`iter` 与 `run` 的关系见上。

**`run` 的 docstring 里有几处被真实故障塑造的约束：**`conversation_id` 可继承自 `message_history`、`run_id` **绝不继承**（空串或重复值都 `UserError`，因为两者都会破坏 `new_messages()`）；`timeout` 在嵌套 run 里只能缩短不能延长。

**`AgentRunEvents` 为什么不是 async generator。** 它是手写的迭代器类，docstring 明确给了理由：generator 的清理是在 finalization 期间向挂起帧扔 `GeneratorExit`，在 Python 3.11 上可能在不同 `Context` 下恢复帧，从而触发 `pydantic_ai.current_run_context` 的 token 错误。手写类把清理走 `aclose()`，停留在调用方的 task 与 context 里。它的懒启动也是刻意的：进入上下文管理器但不迭代，就不会启动 run。

## 关键取舍

**源码事实：** 所有运行入口都由 `iter()` 派生。子类实现 `iter` 就自动获得 `run` / `run_sync` / `run_stream` / `run_stream_events`，这是「一个真入口 + 若干包装」的复用，代价是 `run()` 的实现要显式驱动 `next()`，比 `async for` 更难读。

**源码事实：** `root_capability` 在基类里 `raise NotImplementedError` 却没有 `@abstractmethod`。这处不一致让「是否实现」无法在构造期强制。

**源码事实：** 构造期故意不提前推断模型名（`self._model = model` 原样保留），因为能力可能自己解释模型 ID。只有确认根能力没有 `has_resolve_model_id` 时，才在最后一步 `infer_model`。

**源码事实：** `_function_toolset` 的 `max_retries` 留 `None`，让 agent 级预算在运行期解析，从而可以被 `run` / `iter` / `override` 覆盖。把预算烘焙进工具会让「按次覆盖」失效。

**源码事实：** 能力绑定分 `innermost` / 非 innermost 两轮，位置换来的是「耐久能力能包裹所有工具集」，代价是 `innermost` 能力不能贡献自己的工具集。

**源码事实：** `override()` 用 ContextVar 而不是改字段。这让同一个 Agent 能在并发/嵌套场景下正确隔离覆盖，且复原自动。

**源码事实：** `_resolve_spec` 读废弃字段用 `__dict__` 绕过 Pydantic 的丢弃告警；这是「只在需要告警时才告警」的取舍。

**架构推论：** 把抽象面收窄到「七个属性 + iter」，让 `WrapperAgent`、耐久包装、以及用户自定义 Agent 的实现成本极低，也让所有入口行为天然一致；代价是**能力面窄到无法表达「我只有 run 没有 stream」这类差异**，于是「模型不支持流」被推到了运行期的 `UserError`。

**架构推论：** 构造期顺序如此之长且互相依赖（能力先绑、工具集后抽、id 校验两次），说明「把配置收成一个可执行对象」的复杂度已经超过单函数可读的上限。这解释了为什么后面还需要 `AgentSpec` 这一层把配置外置成数据。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/agent/abstract.py` | `AbstractAgent` | 抽象契约：七个属性 + iter 派生的全部运行入口 |
| `pydantic_ai_slim/pydantic_ai/agent/abstract.py` | `AgentRetries` | 两类重试预算的 TypedDict，整数简写展开成两类 |
| `pydantic_ai_slim/pydantic_ai/agent/abstract.py` | `AbstractAgent.run` | 通过 `iter()` + `next()` 驱动，保证能力钩子逐节点触发 |
| `pydantic_ai_slim/pydantic_ai/agent/abstract.py` | `AgentRunEvents` | 手写异步迭代器，用 `aclose()` 而非 `GeneratorExit` 清理 |
| `pydantic_ai_slim/pydantic_ai/agent/__init__.py` | `Agent` | 具体实现：字段分层、构造期装配、装饰器层 |
| `pydantic_ai_slim/pydantic_ai/agent/__init__.py` | `Agent.__init__` | 装配总序列：能力树、输出 schema、重试预算、工具集、override ContextVar、两轮绑定 |
| `pydantic_ai_slim/pydantic_ai/agent/__init__.py` | `Agent.from_spec` | 声明式构造：标量 spec 当默认、能力列表合并 |
| `pydantic_ai_slim/pydantic_ai/agent/__init__.py` | `Agent._resolve_spec` | 运行期 spec 解析，忽略不支持字段并告警 |
| `pydantic_ai_slim/pydantic_ai/agent/__init__.py` | `Agent.override` | 基于 ContextVar 的临时覆盖，天然支持嵌套与并发 |

## 参考

- [pydantic-ai 源码：agent/abstract.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/agent/abstract.py)
- [pydantic-ai 源码：agent/\_\_init\_\_.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/agent/__init__.py)
- [Pydantic AI 文档：Agent 概念](https://ai.pydantic.dev/agent/)
- [Pydantic AI 文档：依赖注入](https://ai.pydantic.dev/dependencies/)

## 与主核 / AI 的关系

「抽象基类只留一个真入口、其余全部派生」这个形态，与主核里「驱动层只有一个执行函数、其余都是参数适配」是同一种压缩手段。差别在于主核压缩的是调用路径，这里压缩的是**扩展面**：只要实现 `iter()`，包装 Agent（耐久执行、Temporal、测试替身）就免费获得一整套入口，且行为与内建 Agent 完全一致——这正是 `WrapperAgent` 与各种耐久包装能低成本存在的原因。

对 AI 工程的实际意义在于：Agent 的复杂度必须被收拢到一个确定的装配序列里，否则「为什么这次 run 用了那个模型/那个预算/那个工具」将无法回答。这段代码的两个手段值得借鉴：一是**把 override 放进 ContextVar**，让临时覆盖不污染对象本身、也天然支持并发与嵌套；二是**把预算延迟解析**（工具集留 `max_retries=None`），让「按次数覆盖」这种最常被需要的灵活性不被构造期烘焙死。反过来，构造期那串越来越长的阶段（能力绑定 → 工具集抽取 → id 二次校验）也是一个信号：当一个对象的装配步骤超过十步且顺序敏感时，把它外化成 `AgentSpec` 那样的数据契约，通常比继续往 `__init__` 里加更有出路。
