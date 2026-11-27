---
project: pydantic-ai
feature: model-abstract
title: 模型抽象与请求参数
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/models/_abstract.py
    symbols: [AbstractModel]
  - path: pydantic_ai_slim/pydantic_ai/models/__init__.py
    symbols: [ModelRequestParameters, ModelRequestContext]
---

# 模型抽象与请求参数

## 一句话职责

用两层抽象把「模型是什么」与「一次请求要带什么」分开：`AbstractModel` 只提供身份（名字、provider、显示名），`ModelRequestParameters` 承载一次请求的全部结构化配置（工具、输出、指令、思考、缓存），两者都不含任何供应商专有的线格式。

## 机制是什么

`AbstractModel` 是一个极小的 ABC，只有两个抽象属性 `model_name` 与 `system`（后者即 provider 名，用于填充 OpenTelemetry 的 `gen_ai.system`），外加几个有默认实现的派生属性：`base_url`（默认 `None`）、`model_id`（`f'{system}:{model_name}'`）、`context_window`（默认 `None`）、`label`（把模型名转成人可读的显示名）。

它单独放在 `models/_abstract.py` 而不是 `models/__init__.py`，原因写在模块 docstring 里：实时模型也是模型，但它不属于「请求-响应模型」，两者都需要同一份身份——一份名字、一个 provider、一个显示标签，用于模型解析与 OTel 属性。

`ModelRequestParameters` 是另一层：它是一个 `kw_only` dataclass，字段按用途分成六组——工具、输出、指令、思考、缓存、遥测。它不带任何供应商细节，因此同一个实例可以交给任意 `Model` 子类，由子类按自己的线格式解释。

`ModelRequestContext` 是把「模型 + 消息 + 设置 + 参数」打包的上下文对象，供 `before_model_request` 之类的钩子使用。

## 怎么实现的

**`label` 的三条命名规则。** 它的实现全是字符串处理，但规则值得记住：OpenRouter 风格的 `meta-llama/llama-3-70b` 先取最后一段；`gpt` 作为第一段时全大写；数字段若前一段也是数字就合并成点分版本号（`4`-`5` → `4.5`），否则独立成段；其余段首字母大写。结果是 `claude-sonnet-4-5` → `Claude Sonnet 4.5`、`gpt-5` → `GPT 5`。这是纯展示用途，但注释把它写得像规范，因为它出现在 UI 与追踪里，改动会影响可读性。

**`model_id` 是拼接而非映射。** `f'{system}:{model_name}'`——这意味着 `model_id` 的正确性完全依赖 `system` 与 `model_name` 本身正确。而 `system` 的文档提醒它用于 OTel 语义约定，应使用已知值，这暗示 `system` 不是任意字符串而是有限集合。

**`context_window` 的三种来源被写进文档。** 有 profile 的模型从 profile 的 `context_window` 读（缺失时由 genai-prices 填）；包装模型报被包装者的值；`FallbackModel` 报候选里**最小**的那个。最后这条是本页最实用的一条事实：回退链的上下文窗口取交集，因为一次请求可能落在任何一个候选上。

**`ModelRequestParameters` 的工具分组。** `function_tools` 与 `native_tools` 两类分开存；`tool_visibility` 记录每个函数工具的解析后可见性（`'visible'` / `'withheld'` / `'via_history'`），`revealed_tool_names` 记录「历史已经揭示过哪些工具」，`introduced_tool_names` 记录「首轮之后才加入工具集的立即可用工具」。这三者共同回答一个问题：这个工具此刻该不该出现在请求里，以及以哪种方式出现。

`visibility_of` 是一个读方法而非字段：取 `tool_visibility` 里的值，没有则退回按 `defer_loading` 猜（`defer_loading` 为真的默认 `'withheld'`，否则 `'visible'`）。文档明确这个兜底只适用于「直接构造而非经 `prepare_request` 解析」的参数。

**「已发现」与「已揭示」的区分是本组的核心。** `revealed_tool_names` 的文档写得最清楚：**discovered 是历史证明模型见过，revealed 是这次请求的线上状态真的表示了它**。这两个概念分开，是因为一个被揭示过的工具在后续请求里仍要保留在历史中它出现的位置，而不能因为「这次不需要」就再次隐藏——否则模型会看到自相矛盾的历史。`introduced_tool_names` 处理另一件事：运行中途新增的工具不能进 `tools`（那是第一个缓存段），要像被揭示的延迟工具那样通过 `ToolAvailabilityDeltaPart` 走上「工具新增」通道。

**指令分组用 `InstructionPart` 列表而非单个字符串。** 每个 part 带 `dynamic` 标记（静态来自字面量、动态来自函数或模板），支持细粒度缓存的模型据此在静态/动态边界放缓存断点。`instruction_visibility` 是配套的可见性映射：某个 part 不在前缀里时，记录它是怎么到达模型的（`'via_history'`）。

**`thinking` 与 `cache` 是「已解析的统一值」。** 两个字段的文档都强调它们由基类的 `prepare_request` 从 `ModelSettings` 里的同名统一字段解出，且都先检查 profile 支持：模型不支持思考就不设 `thinking`，不支持缓存配置就不设 `cache`（并把保留期吸附到受支持的档位）。同时都留了一句「供应商专有设置优先，且不改变这里」——统一值与专有值并存，专有值赢。

**`tool_defs` 与 `declared_tool_defs` 是两个不同的缓存字典。** 前者索引 `function_tools + output_tools`，后者索引 `declared_function_tools + output_tools`，其中 `declared_function_tools` 过滤掉了 `withheld` 与 `via_history` 的工具。这个区分对应「模型能调什么」与「这次请求的 `tools` 数组里写什么」两件事，二者不必相同。

**`ModelRequestContext` 的字段全部可 `replace`。** 它的 docstring 记录了一个真实的坑：`model_id` 与 `streaming` 这两个字段由 agent 图在调用钩子前设置，如果声明成 `init=False`，任何用 `replace()` 复制上下文的钩子都会把它们重置成默认值——后果是一次流式运行丢失 `streaming` 标记、一次持久执行丢失用于重新解析别名模型的选择令牌。所以它们必须是普通可写字段。

**消息列表是「独立列表但不是独立对象图」。** `ModelRequestContext.messages` 的文档分三层说清楚：顶层 list 是本次请求独有的，`append`/重排/赋新序列只影响这次请求；但里面的消息与 part 可能与持久历史是同一批对象，就地修改 part 会影响历史。要隔离必须逐层构造替换。这个精确表述避免了「我改了但没生效」和「我只想改这次却污染了历史」两种相反的事故。

## 关键取舍

**源码事实：** `AbstractModel` 只有身份，没有任何请求能力。请求能力在 `Model`（请求-响应）与 `RealtimeModel`（实时）两个分支上分别定义。这样拆是为了让实时模型也拥有 `model_id`、`label`、`context_window` 这些身份属性，共享 OTel 与模型解析逻辑。

**源码事实：** `context_window` 在回退链上报「最小的候选」。这是安全侧取值：按最大候选规划上下文会导致部分候选必然超限。

**源码事实：** 「已发现」与「已揭示」是不同的集合。前者由历史推出，后者是本次请求的线上状态。混用二者会导致历史与请求声明矛盾。

**源码事实：** 运行中途新增的工具不能进 `tools` 段，因为 `tools` 是第一个缓存段，中途变化会破坏整个前缀缓存。它们改走「工具可用性增量」通道。这是一个**缓存成本驱动的协议设计**，不是纯粹的功能选择。

**源码事实：** `thinking` 与 `cache` 在参数里是统一值，供应商专有设置另存于 `ModelSettings` 并优先。这种「统一 + 专有并存」的模式让模型适配层可以逐步从专有迁移到统一，而不破坏已用专有的代码。

**源码事实：** `ModelRequestContext` 的字段刻意保持全部可 `replace`，理由是一段具体的故障史。这是一条「抽象签名要为未来的字段预留安全性」的教训。

**架构推论：** 这一层的价值在于**把供应商差异压缩到适配器内部**。上层（agent 图、能力、钩子）只处理 `ModelRequestParameters` 这些归一化字段，不需要知道 Anthropic 的 thinking 块或 OpenAI 的 response_format 长什么样。代价是归一化字段的数量会长（当前已覆盖工具可见性、指令可见性、思考、缓存四类），且每加一类都要想清楚「统一值 + 专有逃生口」怎么并存。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/models/_abstract.py` | `AbstractModel` | 模型身份基类：`model_name`、`system`、`model_id`、`context_window`、`label` |
| `pydantic_ai_slim/pydantic_ai/models/__init__.py` | `ModelRequestParameters` | 一次请求的全部归一化配置 |
| `pydantic_ai_slim/pydantic_ai/models/__init__.py` | `ModelRequestParameters.visibility_of` | 工具可见性读取与兜底规则 |
| `pydantic_ai_slim/pydantic_ai/models/__init__.py` | `ModelRequestParameters.declared_tool_defs` | 实际写进 `tools` 数组的定义集合 |
| `pydantic_ai_slim/pydantic_ai/models/__init__.py` | `ModelRequestParameters.with_default_output_mode` | `'auto'` 输出模式落定并同步 `allow_text_output` |
| `pydantic_ai_slim/pydantic_ai/models/__init__.py` | `ModelRequestContext` | 请求钩子上下文，字段全部可 `replace` |
| `pydantic_ai_slim/pydantic_ai/models/__init__.py` | `Model.prepare_request` | 设置合并、profile 判定、思考/缓存解析、输出模式落定 |
| `pydantic_ai_slim/pydantic_ai/models/__init__.py` | `Model.accepts_stateless_artifacts` | 无状态加密产物能否回传：按 canonical system 判定 |
| `pydantic_ai_slim/pydantic_ai/models/__init__.py` | `Model.accepts_server_state` | 服务端状态 ID 能否回传：按 endpoint 判定 |
| `pydantic_ai_slim/pydantic_ai/models/__init__.py` | `Model._default_structured_output_mode` | profile 默认通道与「强制工具调用关闭思考」特例 |

## 参考

- [pydantic-ai 源码：models/_abstract.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/models/_abstract.py)
- [pydantic-ai 源码：models/__init__.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/models/__init__.py)
- [Pydantic AI 文档：模型概览](https://ai.pydantic.dev/models/overview/)

## 与主核 / AI 的关系

这套分层与主核的「存储引擎接口 + 执行计划参数」类似：接口只声明身份与能力位，执行细节交给具体引擎；计划参数是归一化的（表、谓词、投影），不含任何存储引擎的私有选项。主核要做到「换引擎不改上层」，这里的 `ModelRequestParameters` 起到同样的隔离作用。

对 agent 工程的直接价值有三点。其一，**回退链的上下文窗口是最小值**，规划上下文预算时必须按最保守的候选算，否则回退到小窗口模型时会静默截断。其二，**工具与指令的可见性有「历史」这一维**——一个工具一旦在历史里出现过，就不能在后续请求里假装它不存在；任何自己做工具动态加载的方案都要遵守这条，否则模型会面对自相矛盾的历史。其三，**缓存前缀决定了协议的形状**：`tools` 是第一段、中途加入的工具必须走增量通道，这类约束看起来是实现细节，实际上限定了「运行中还能改什么」。
