---
project: crewai
feature: base-llm
title: 模型基类与调用包装
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/llms/base_llm.py
    symbols: [BaseLLM, _wrap_call_method, to_config_dict]
---

# 模型基类与调用包装

## 一句话职责

把模型客户端定义成一个 pydantic 模型，并用 `__init_subclass__` 在子类定义时**自动给它的 `call`/`acall` 套上限流重试包装**——让每个 provider 实现只写裸调用，不必各自记得加重试。

## 机制是什么

`BaseLLM(BaseModel, ABC)` 声明了所有 provider 共享的字段：`model`（必填）、`temperature`、`top_p`、`max_tokens`、`stream`、`seed`、`frequency_penalty`、`presence_penalty`、`n`、`api_key`、`base_url`、`provider`（默认 `"openai"`）、`prefer_upload`、`is_litellm`、`stop`、`additional_params`、`context_window_size`。抽象方法只有一个：`call`。`acall` 有默认实现直接 `NotImplementedError`。

最特别的是包装机制：`__init_subclass__` 对子类调两次 `_wrap_call_method("call")` 与 `_wrap_call_method("acall")`。`_wrap_call_method` 用 `cls.__dict__.get(method_name)` 取**子类自己定义的**方法——继承来的不算——然后替换成调 `run_with_rate_limit_retry`（同步）或 `arun_with_rate_limit_retry`（异步）的版本，并在包装器上打 `_crewai_rate_limit_wrapped = True` 标记以便幂等。

这意味着「限流重试」不是继承来的行为，而是**在类定义那一刻注入的**。任何覆盖 `call` 的子类自动获得重试，无需调用 `super()`。

## 怎么实现的

**构造函数前的字段归一。** `_validate_init_fields` 是 `mode="before"` 的模型校验器，四件事：

1. `model` 为空直接 `ValueError("Model name is required and cannot be empty")`。
2. `stop_sequences` 从 data 里 `pop` 出来，优先用作 `stop`；`None` 归一成 `[]`、字符串归一成单元素列表、其他可迭代转 list。
3. `provider` 为空时用 `_extract_provider(model)` 从模型名推（模型名带 `/` 取前缀，否则 `"openai"`）。
4. **未知字段全部搬进 `additional_params`**：算出差集、从 data 里删掉、再合并进已有的 `additional_params`。所以 provider 私有参数（`aws_region`、`safety_settings` 之类）不必在基类声明字段也能传进来，代价是它们以字典形式存在、失去独立校验。

**`stop` 的双名与就地兼容。** `stop` 字段带 `validation_alias=AliasChoices("stop", "stop_sequences")`，所以两种名字都能构造。`__setattr__` 覆写又做了一层归一：名字是 `stop`/`stop_sequences` 时统一写成 `stop` 字段，且把 `None`/字符串/可迭代都归一成 list。异常处理也有讲究：`super().__setattr__` 抛 `ValueError` 时，若名字在 `model_fields` 里就重抛（这是真的校验失败），否则退回 `object.__setattr__`（兼容 mock 打补丁这类非声明字段）。

**方案上下文的覆盖。** `stop_sequences` 是 property 而非字段：先查 `_call_stop_override_var` 里按 `id(self)` 记的覆盖值，没有才返回 `self.stop`。`_effective_stream()` 同理查 `_call_stream_override_var`。这两个 ContextVar 让「某一次调用临时改 stop 或 stream」成为可能，而不污染实例字段——`stream_events` 里的 `call_stream_override(self, True)` 正是这么用的。

**上下文窗口的 85% 折减。** `get_context_window_size()` 在 `context_window_size` 已设时返回 `int(context_window_size * CONTEXT_WINDOW_USAGE_RATIO)`，否则返回默认值。注释写「85%」，所以预检认为「超过 85% 就算溢出」——把余量留给输出 token。

`context_window_size` 的校验器处理一个历史包袱：`0` 视为「未设置」返回 `None`（注释说 1.15.32 及以前用 0 表示未设置，检查点里带着它，2.0 才移除），负数报错。

**token 估算的兜底。** `get_input_tokens_count` 默认按「每 4 个字符算 1 token」向上取整，并对多模态消息用 `message_content_text` 压平后计数。它会打一条 warning 说明「因为原生计数不可用所以用近似」。`is_context_window_overflowing_threshold` 与 `is_tool_result_overflowing_threshold` 都建在这个估算上——后者把「工具结果超过可用窗口的 25%」作为延迟输出的判据。

**配置导出。** `to_config_dict()` 只导出基类拥有的核心字段：把 `model` 拼成 `f"{provider}/{model}"`（若 model 里没有 `/`），再按非空条件加 `temperature`、`base_url`、`stop`、`context_window_size`。docstring 明确要求子类覆盖并调 `super()`，用于补自己的字段。

**限流重试的注入点。** `_wrap_call_method` 对 `call` 与 `acall` 分别构造包装器：`wrapped_call` 把原方法包成 lambda 交给 `run_with_rate_limit_retry`，`wrapped_acall` 交给 `arun_with_rate_limit_retry`。两者都用 `@wraps(method)` 保留元信息，打标记防重复包装，最后 `setattr(cls, method_name, wrapped)` 写回类。若方法名不在两者之内（理论上不可能）则 `ValueError`。

**hook 入口在基类。** `_invoke_before_llm_call_hooks` 与 `_invoke_after_llm_call_hooks` 是给**原生 provider 实现**调用的：它们构造 `LLMCallHookContext`（`executor=None`，只带 messages、llm、agent）并 `dispatch` 到 `PRE_MODEL_CALL` / `POST_MODEL_CALL`。开头都检查 `model_call_hooks_already_dispatched()`——若外层执行器已经分派过就直接返回，避免同一调用跑两遍钩子。同步方法还会把 `LegacyHookBlocked` 翻译成 `LLMCallBlockedError`。

**可观测性分散在若干 `_emit_*` 方法。** `_emit_call_started_event`、`_emit_call_completed_event`、`_emit_call_failed_event`、`_emit_call_denied_event`、`_emit_stream_chunk_event`、`_emit_thinking_chunk_event` 各发一种事件，provider 实现按需调用。所以「发事件」是 provider 的责任而不是基类强制的——这既是灵活性来源，也是不一致来源。

**token 用量累积。** `_token_usage` 是 `PrivateAttr` 字典，初值七个键全零：`total_tokens`、`prompt_tokens`、`completion_tokens`、`successful_requests`、`cached_prompt_tokens`、`reasoning_tokens`、`cache_creation_tokens`。`_track_token_usage_internal` 增量更新，`get_token_usage_summary` 返回 `UsageMetrics`。私有不进序列化，所以检查点不保存用量。

## 关键取舍

**源码事实：** 包装只作用于子类**自己定义**的方法。若某子类完全不定义 `call`（纯继承），它不会获得包装——但因为 `call` 是抽象方法，不定义就无法实例化，所以这条在正常形态下不可达。若子类调 `super().call()` 转发给父类实现，重试会叠两层。

**源码事实：** 把未知字段搬进 `additional_params` 意味着**拼错的字段名不会报错**。写 `temprature=0.5` 不会失败，只会静默进入 `additional_params` 并被 provider 忽略。这是「宽松构造」的直接代价。

**源码事实：** `to_config_dict` 只导出非空字段，且 `stop` 为空列表时不导出。所以「显式设成空列表」与「从未设置」在导出的配置里无法区分——往返后两者都变成默认值。

**源码事实：** 默认 token 估算按字符数除 4，与真实分词器无关。注释只说这是保守估计，但中英文混排时误差可能很大（中文往往 1–2 字符一个 token）。所有依赖它做预检的逻辑都继承了这个误差。

**源码事实：** `get_context_window_size()` 任何时候都返回 85% 折减值，包括调用方只是想问「模型窗口多大」。所以任何拿这个数去和真实 token 上限比较的外部代码都会低估模型能力。

**架构推论：** 「类定义时注入重试」解决了一个组织问题：provider 实现多、由不同人写，靠约定「记得加重试」必然漏。把注入放进 `__init_subclass__`，让「实现即得」成为结构保证。代价是调试时看到的 `call` 不是源码里写的那个函数（被 `@wraps` 保留元信息掩盖），异常栈里会多一层。

**架构推论：** 字段开放 + 未知字段收敛到 `additional_params`，是「多 provider 复用一个基类」的必然结果：基类不可能预知所有 provider 的参数，而每个 provider 都声明自己的子类字段又太啰嗦。这条路的另一头就是「配置错误静默通过」，所以配套通常需要 provider 侧的严格校验。

**架构推论：** `stop_sequences` 做成 property + ContextVar 覆盖，而不是普通字段，是为了支持「一次调用级别的临时覆盖」而不动实例。这与全局共享同一个 LLM 实例的使用方式有关——同一个客户端被多个 agent 共用时，逐调用的差异不能写回实例。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/llms/base_llm.py` | `BaseLLM` | 模型基类：字段、抽象 `call`、共享能力 |
| `lib/crewai/src/crewai/llms/base_llm.py` | `BaseLLM._wrap_call_method` | 子类定义时注入限流重试包装 |
| `lib/crewai/src/crewai/llms/base_llm.py` | `BaseLLM.__init_subclass__` | 自动对 `call`/`acall` 调包装 |
| `lib/crewai/src/crewai/llms/base_llm.py` | `BaseLLM._validate_init_fields` | 模型名必填、stop 归一、provider 推导、未知字段收敛 |
| `lib/crewai/src/crewai/llms/base_llm.py` | `BaseLLM.__setattr__` | `stop` 双名归一，非声明字段回退直接赋值 |
| `lib/crewai/src/crewai/llms/base_llm.py` | `BaseLLM.stop_sequences` | 支持调用级 ContextVar 覆盖的 stop 列表 |
| `lib/crewai/src/crewai/llms/base_llm.py` | `BaseLLM.get_context_window_size` | 返回 85% 折减后的可用窗口 |
| `lib/crewai/src/crewai/llms/base_llm.py` | `BaseLLM.get_input_tokens_count` | 四字符一 token 的兜底估算 |
| `lib/crewai/src/crewai/llms/base_llm.py` | `BaseLLM.to_config_dict` | 导出可重建的配置字典，子类可扩展 |
| `lib/crewai/src/crewai/llms/base_llm.py` | `BaseLLM._invoke_before_llm_call_hooks` | 模型前钩子入口，带双重分派防护 |
| `lib/crewai/src/crewai/llms/base_llm.py` | `BaseLLM._apply_stop_words` | 客户端侧 stop word 截断兜底 |
| `lib/crewai/src/crewai/llms/base_llm.py` | `BaseLLM.is_context_window_overflowing_threshold` | 请求前预检，触发压缩 |

## 参考

- [CrewAI 源码：llms/base_llm.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/llms/base_llm.py)
- [CrewAI 文档：Agents 与模型配置](https://docs.crewai.com/en/concepts/agents)
- [Pydantic 文档：模型校验与别名](https://docs.pydantic.dev/latest/concepts/alias/)

## 与主核 / AI 的关系

主核里最接近的是「存储/访问方法基类」：它定义一组接口与共享参数，具体实现由各引擎后端补。这里多出来的是「类定义时改造子类」这一步——主核几乎没有这种做法，因为它依赖编译期分派，而 Python 靠 `__init_subclass__` 能在运行时改类。这条能力让「所有实现自动获得某个横切行为」变成可能，但也让代码的静态可读性下降。

与 AutoGen 的 `ChatCompletionClient` 相比差别很具体：AutoGen 那边抽象方法很多（`create`、`create_stream`、`close`、`actual_usage`、`total_usage`、`count_tokens`、`remaining_tokens`、`capabilities`），每个实现都要写全；重试、缓存、限流都留给外层包装（例如缓存是另一个客户端实现，重试由 provider SDK 自带）。CrewAI 这边抽象方法只有 `call`，其余都有默认实现，并把限流重试做成自动注入。方向相反：AutoGen 要求实现者显式拼装能力，CrewAI 要求实现者只写裸调用。

对 AI 工程最实用的一条是「未知字段收敛」。写多 provider 的适配层时，基类字段永远不够；把未知字段收进一个 `additional_params` 字典，能让新参数不需要改基类就能透传。代价是错误也被透传——拼错的参数名不会报错。要拿到严格性，就得在 provider 子类里显式声明字段，接受「每加一个参数改一次代码」。

另一条是 85% 折减。把「可用窗口」定义为模型窗口的 85%，意味着所有基于它做预算的逻辑（压缩、延迟工具输出、预检）都自动留出了输出余量。这比让每个调用点自己乘系数可靠——但要注意别把这个数当成模型真实上限。

最后是 token 估算的兜底。任何「按字符数除常数」的估算都会在中文与代码上偏得厉害，所以它只该用于「粗略预警」而不该用于「精确裁剪」。这页提到的 `is_tool_result_overflowing_threshold` 用 25% 这样宽松的阈值，正是为了让估算误差不影响判定——阈值离边界越远，估算越不需要准。
