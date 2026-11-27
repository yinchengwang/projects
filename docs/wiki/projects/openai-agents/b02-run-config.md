---
project: openai-agents
feature: run-config
title: 运行配置与敏感数据开关
page_type: feature
schema_version: 1
refs:
  - path: src/agents/run_config.py
    symbols: [ModelInputData, CallModelData, ToolExecutionConfig]
  - path: src/agents/run_config.py
    symbols: [_default_trace_include_sensitive_data, _coerce_run_config]
---

# 运行配置与敏感数据开关

## 一句话职责

`RunConfig` 是一次运行的全局横切配置：模型与设置的全局覆盖、输入输出的可插拔过滤器、追踪开关与敏感数据策略、以及若干「本该更严格但为兼容保留了旧默认」的行为开关；它同时接受 dataclass 与字典两种形态。

## 机制是什么

`RunConfig` 的字段可以分成五组：

| 组 | 字段 | 说明 |
|---|---|---|
| 模型 | `model`、`model_provider`、`model_settings` | 运行级覆盖：`model` 有值就压过所有 Agent 的 `model` |
| 交接 | `handoff_input_filter`、`nest_handoff_history`、`handoff_history_mapper` | 全局交接输入过滤；历史嵌套（beta，默认关） |
| 护栏 | `input_guardrails`、`output_guardrails` | 运行级护栏，与 Agent 级并列执行 |
| 追踪 | `tracing_disabled`、`tracing`、`trace_include_sensitive_data`、`workflow_name`、`trace_id`、`group_id`、`trace_metadata` | 是否追踪、是否带敏感数据、trace 的组织与命名 |
| 行为 | `session_input_callback`、`call_model_input_filter`、`tool_error_formatter`、`session_settings`、`reasoning_item_id_policy`、`tool_execution`、`tool_not_found_behavior`、`tool_name_collision_policy`、`output_guardrail_blocked_message` | 各阶段的策略开关 |

三个伴生数据类把「回调的入参」定型，它们都是纯数据：`ModelInputData`（`input` 项列表 + `instructions`）、`CallModelData`（`model_data` + `agent` + `context`）、`ToolExecutionConfig`（`max_function_tool_concurrency` + `pre_approval_tool_input_guardrails`）。

**敏感数据开关的默认值来自环境变量。** `trace_include_sensitive_data` 的 `default_factory` 是 `_default_trace_include_sensitive_data`，它读 `OPENAI_AGENTS_TRACE_INCLUDE_SENSITIVE_DATA`，缺省 `"true"`，只在值落在 `("1","true","yes","on")` 之一时为真。也就是说**默认会导出敏感数据**，要关必须显式设置环境变量或传字段。

## 怎么实现的

**两个入口形态：dataclass 与字典。** `_coerce_run_config` 把 `RunConfig | dict` 归一成 `RunConfig`，委托给 `coerce_dataclass_config`：若已经是实例就原样返回；若不是 dict 就抛 `TypeError`；若是 dict，先按 `fields(config_type)` 里的 **`init=True` 字段名**过滤未知键并报错（`Unknown run_config settings: ...`），再 `config_type(**value)`。

这条「未知键必须报错」是刻意的：它挡住 `temprature` 这类拼写错误被静默忽略。同一个 coercion 工具被复用给 `ModelSettings`、`SessionSettings`、`SandboxRunConfig`、`ToolExecutionConfig` 以及它们的嵌套字段，形成了一套统一的「SDK 拥有的配置对象」入口规范。

**`__post_init__` 做四类事。** 一是枚举校验（`tool_name_collision_policy` 必须是 `warn` 或 `error`）。二是 `output_guardrail_blocked_message` 的三态校验：`str` 必须非空、其他 `str` 子类要拒绝（区分「内建字符串」与「字符串子类」）、不是 str 且不是 None 就必须可调用、并且**必须是同步函数**——`inspect.iscoroutinefunction` 为真直接抛 `TypeError`。源码在旁边留了一段注释解释原因：这个格式化器在「终态工具输出被拒之后、所有重放与持久化归属者被重建之前」执行，在那个边界上 `await` 应用代码会让被拒输出通过取消的 traceback 局部变量或半清洗状态重新可达；要支持 async 得重新设计脱敏边界，而不是简单加个 `await`。三是把 `model_settings`、`session_settings`、`sandbox`、`tool_execution` 四个字段递归 coerce 成对应类型。四是用 `_declared_dataclass_type` 读取「子类重声明过的字段注解」，让 `RunConfig` 的子类可以指定自己的 `SandboxRunConfig` 子类而不被父类默认类型覆盖。

**`ToolExecutionConfig` 的两个开关语义都很窄。** `max_function_tool_concurrency` 为 `None` 表示保留默认行为（一轮里发出的所有函数调用全部并发启动），设成正整数则限制并发；字段 docstring 特别声明「这不改变 provider 侧的 `parallel_tool_calls` 行为」——即它限的是本地执行并发，不是让模型少发调用。`pre_approval_tool_input_guardrails` 为真时，在「发出待审批中断之前」先跑一次函数工具输入护栏，且审批通过后执行前仍会再跑一次。两者都在 `__post_init__` 里校验范围（并发数 ≥ 1，布尔必须是布尔）。

**模型设置是「覆盖」而不是「替换」。** `RunConfig.model_settings` 的语义在 docstring 里写的是「任何非空值将覆盖 Agent 特定的设置」。实现落在 `ModelSettings.resolve(override)`：只取 override 里非 `None` 的字段做 `replace`；`extra_args` 与 `retry` 两个字段特殊处理——前者把两个 dict 合并，后者走 `_merge_retry_settings`（含 `backoff` 的逐字段合并）。所以「运行级只设 `temperature`」不会把 Agent 的 `reasoning` 抹掉。

**模型解析是四段优先级。** `get_model` 的顺序是：`RunConfig.model` 是 `Model` 实例就用它；是字符串就用 `RunConfig.model_provider.get_model(名字)`；否则看 `Agent.model`，是 `Model` 实例就用它；否则 `provider.get_model(agent.model)`。而模型设置还要再叠一层：`get_model_settings` 先取 Agent 的设置，若 Agent 处于「隐式默认」状态则按**解析后的模型名**重新推导默认值（见 a01），然后把 `prepared_overrides` 与 `run_config.model_settings` 依次 `resolve` 上去，并把被覆盖过的隐式字段从「可再推导」集合里剔除，最后交给 `model._get_request_settings(...)` 处理剩下的隐式字段。

**追踪开关的默认值与生效点。** `trace_include_sensitive_data` 决定的是「span 上写不写原文」。`get_trace_error(trace_include_sensitive_data, error_message)` 是统一的取值点：真就返回原文，假就返回固定的 `REDACTED_TRACE_ERROR_MESSAGE`。更细的一处是 `_model_error_text`：它只在真的要导出时才 `str(error)`，注释说明理由——某些 provider 异常的 `__str__` 有副作用，不能为了「随后丢弃」而调用它。`tracing_disabled` 与 `tracing`（`TracingConfig`，含 `api_key` 与 `include_task_and_turn_spans`）是另外两个开关，其中 `include_task_and_turn_spans` 在缺省时视为 `True`。

**`call_model_input_filter` 是同步或异步皆可的回调。** `maybe_filter_model_input` 先复制一份 `input` 造 `ModelInputData`，再用 `CallModelData(model_data, agent, context_wrapper.context)` 调用它；返回值若可等待就 `await`，然后强制要求是 `ModelInputData` 实例，否则 `UserError`。异常路径会向当前 span 挂一条错误（文案受 `trace_include_sensitive_data` 控制）。

**同步/异步混用是这套配置的常态。** `tool_error_formatter` 返回 `MaybeAwaitable[str | None]`（可为 None 表示回退默认消息），`session_input_callback`、`handoff_input_filter` 也都是 maybe-awaitable；只有 `output_guardrail_blocked_message` 的格式化器被明确限制为同步。

## 关键取舍

**源码事实：** 敏感数据默认导出。`OPENAI_AGENTS_TRACE_INCLUDE_SENSITIVE_DATA` 缺省视为 `true`。这是一个面向开发体验的默认：追踪默认就好用，但生产环境必须显式关掉。判断「这个 SDK 是否会被合规团队卡住」时，这一条比任何其他字段都关键。

**源码事实：** `output_guardrail_blocked_message` 必须是同步的，原因写在源码注释里而非 docstring 里。这属于典型的「限制来自执行时序而非接口美学」——同一个边界上，异步回调会延长被拒数据的可达窗口。

**源码事实：** `max_function_tool_concurrency` 只影响本地并发，不改变 `parallel_tool_calls`。想「让模型少发并行调用」必须改模型设置，改这个不会有效果。

**源码事实：** `RunConfig` 的字典形态会拒绝未知键。这与许多「宽松接受配置」的框架相反；好处是拼写错误立刻暴露，坏处是跨版本时旧配置项被移除会直接失败而不是被忽略。

**架构推论：** `RunConfig` 承载了从「模型选谁」到「敏感数据能不能出机器」的一切，跨度极大。这带来一个实际后果：一个运行的可审计性取决于调用方是否显式设置了这些开关——默认值里既有「导出敏感数据为真」，也有「交接历史嵌套为假」这类偏保守的项，混合默认值意味着「不配置」不等于「安全」。

**架构推论：** 把 `call_model_input_filter` 放在「输入组装完成后、模型调用前」这一个位置，使它成为唯一能改写模型输入的用户钩子。想要「按 token 预算裁剪历史」，只能在这里做——而不是在 `Session` 或 `RunState` 上做，因为那两处管的是历史的存取，不是本次请求的形态。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/run_config.py` | `RunConfig` | 运行级全局配置的全部字段 |
| `src/agents/run_config.py` | `RunConfig.__post_init__` | 枚举校验、同步格式化器约束、嵌套配置 coerce |
| `src/agents/run_config.py` | `ModelInputData` | 模型输入的容器：项列表 + 指令 |
| `src/agents/run_config.py` | `CallModelData` | 输入过滤器的入参：模型数据 + 智能体 + 应用上下文 |
| `src/agents/run_config.py` | `ToolExecutionConfig` | 本地函数工具的并发与审批前护栏开关 |
| `src/agents/run_config.py` | `ToolErrorFormatterArgs` | 工具错误格式化器的入参，含 error kind 与 tool type |
| `src/agents/run_config.py` | `_default_trace_include_sensitive_data` | 从环境变量取敏感数据默认值，缺省为真 |
| `src/agents/run_config.py` | `_coerce_run_config` | 字典形态归一，未知键报错 |
| `src/agents/_config_coercion.py` | `coerce_dataclass_config` | 通用 dataclass 配置归一与未知键检查 |
| `src/agents/_config_coercion.py` | `_declared_dataclass_type` | 读取子类重声明的字段类型，支持配置子类化 |
| `src/agents/model_settings.py` | `ModelSettings.resolve` | 只覆盖非空字段，`extra_args`/`retry` 单独合并 |
| `src/agents/run_internal/turn_preparation.py` | `get_model` | 模型解析的四段优先级 |
| `src/agents/run_internal/turn_preparation.py` | `get_model_settings` | 隐式默认重推导 + 两层覆盖 |
| `src/agents/run_internal/turn_preparation.py` | `maybe_filter_model_input` | 输入过滤器唯一执行点与返回值校验 |
| `src/agents/util/_error_tracing.py` | `get_trace_error` | 敏感数据开关的统一取值点 |
| `src/agents/util/_error_tracing.py` | `_model_error_text` | 仅在要导出时才字符串化异常 |

## 参考

- [openai-agents-python 源码：run_config.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/run_config.py)
- [Agents SDK 文档：配置](https://openai.github.io/openai-agents-python/config/)
- [Agents SDK API：run_config 模块](https://openai.github.io/openai-agents-python/ref/run_config/)

## 与主核 / AI 的关系

`RunConfig` 与主核里的「会话级 GUC / 运行时参数」是同一层：一次执行期间生效、可覆盖更下层的默认、且混有「性能项」与「安全项」。主核的做法是把安全相关参数单独成组并要求显式开启，这里的默认值取向相反——追踪默认带敏感数据。这不是对错问题，而是受众差异：SDK 面向本地开发，数据库面向生产。

对 AI 工程有三条可直接用的判断。第一，**看清默认值**：`trace_include_sensitive_data` 默认真、`nest_handoff_history` 默认假、`tool_not_found_behavior` 默认抛错。混合默认意味着「不配置」既可能泄漏也可能过度严格，必须逐项确认而不是整体信任。第二，**区分「限本地并发」与「限模型行为」**：`max_function_tool_concurrency` 改不了 `parallel_tool_calls`，这类「名字像但语义不同」的开关在任何框架里都存在，只看字段名会误判。第三，**配置入口要拒绝未知键**：字典形态的配置最容易在跨版本时静默失效，`coerce_dataclass_config` 的严格检查是值得抄的做法。

`output_guardrail_blocked_message` 必须同步这一条，是「异步不是免费的」的好例子：在被拒数据的生命周期边界上，任何能让出控制权的操作都会扩大可达窗口。做脱敏相关设计时，这条约束比性能考量更优先。
