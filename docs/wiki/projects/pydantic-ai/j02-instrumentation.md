---
project: pydantic-ai
feature: instrumentation
title: 插桩与敏感内容策略
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/_instrumentation.py
    symbols: [ContentPolicy, InstrumentationNames, span_include_content]
---

# 插桩与敏感内容策略

## 一句话职责

为每次模型请求开一个 OTel `chat` span，并回答一个必须闭合的问题：**这个 span 到底允不允许把提示词与响应内容写进去**——答案靠「策略绑定到具体 span」实现，任何不确定的情形都按不允许处理。

## 机制是什么

`ContentPolicy` 是一个只有三个字段的 dataclass：`span`（策略是为哪个 span 设的）、`include_content`（允许不允许）、`tracer`（这个 span 是用哪个 tracer 开的）。

它被放在 `include_content_ctx` 这个 `ContextVar[ContentPolicy | None]` 里，语义是「当前打开着的插桩请求的策略」。`None` 表示当前没有插桩请求在跑。

`span_include_content(span)` 与 `open_request_policy()` 是两个读法，回答两个不同的问题：

- `span_include_content(span)`：**这一个** span 自己是不是开了内容捕获。
- `open_request_policy()`：在调用者所在的请求范围内，应该按哪份策略渲染——用于在请求内部再开子 span 的场合。

`InstrumentationNames` 是命名配置：span 名（`agent_run_span_name`、`tool_span_name`、`output_tool_span_name`）、属性名（`agent_name_attr`、`tool_arguments_attr`、`tool_result_attr`），外加一组类变量常量（工具延迟、失败阶段、重试原因/次数/上限、输出重试原因/次数/上限）。`for_version(version)` 按版本返回两套名字。

## 怎么实现的

**策略绑定 span 的理由是一段具体的教训，写在 docstring 里。** 恢复用的写法是**普通 `set` 而不是 `reset`**，因为一个被中断的流式运行会在**另一个 `Context`** 里结束上下文管理器，那里 `reset` 会抛异常；而 `set` 只在执行它的那个 `Context` 里生效，于是「打开请求的那个 `Context`」可能一直持有一个已完成请求的值。这就是为什么策略必须自带 span 身份：**读取者只能承认「为眼前这个 span 设的」策略，其余一律 fail closed**。

这条设计值得单独记住：它不是洁癖，而是「contextvar 在被复制的 `Context` 之间不保证配平」这一事实的直接后果。任何依赖 `reset` 来「恢复干净状态」的插桩都会在流式取消/中断路径上泄漏。

**`span_include_content` 的判据是 span_id 相等。** 它检查三件事：策略非空、策略的 span_id 等于传入 span 的 span_id、且策略允许内容。任一条不满足返回 `False`。文档的原话是「对每一个不是『这个 span 自己的请求要内容』的答案都 fail closed」——包括「没有请求打开」与「策略属于别的 span」两种情况。

**`open_request_policy` 换了两条更宽的判据。** 因为「在请求内部开的 span」（例如决策模型的 `decide`、或一次失败的回退尝试）**不是**那个请求的 span，甚至可能不是它的子 span——一个持久执行引擎的 step/task/activity span 可以夹在中间。于是它检查：策略非空、策略所指的 span **还在录制中**（`is_recording()`，已结束的请求其 span 就不再录制）、且当前 span 与策略 span 在**同一条 trace** 里。满足就返回策略，否则 `None`（表示「这里没有打开的插桩请求，什么都不该发出」）。

**`current_model_request_span` 又更严。** 它要求当前 span 与策略 span **完全相同**（整个 span context 相等），供模型往请求 span 上写属性用。这是一个「只有正主能写」的限制。

**`include_content` 的取值只来自设置，不来自内容。** `open_model_request_span` 在 `with` 块**内部**才 `include_content_ctx.set(ContentPolicy(span, settings.include_content, settings.tracer))`，注释点明「在 with 内部，因为策略要指名它为之发言的那个 span」。这个顺序也保证了异常路径上策略不会被设成指向一个没开的 span。

**tracer 也被带进策略里。** 文档解释：这样「在请求内部开的 span」（决策模型的 `decide`、失败的回退尝试）会发往与请求 span **同一个 tracer provider**，即使那个不是全局的。若无此项，请求走 Logfire 而内部尝试走全局 provider，追踪就会被拆成两半。

**请求属性在「模型调用边界」才定稿。** `defer_request_attributes=True` 时 span 立即打开，但请求属性由 agent 图在请求钩子跑完之后、真正调模型时才填充（`set_request_attributes`）。这解决了「钩子可能改写请求、甚至把请求转到另一个模型」（例如 `FallbackModel`）的问题：属性描述的是**最终**发给谁，而不是最初打算发给谁。代码里 `span.update_name(f'{operation} {...}')` 会把 span 名改成最终模型名。

**`finish` 是闭包而非内联，为了两件事。** 一是结果属性必须在 `with span:` **内部**设置（这样才挂到 span 上）；二是 `record_metrics` 要在 span 关闭**之后**的 `finally` 里执行——注释说明理由是「按 span 属性聚合指标的后端不会重复计数」。这是一条很容易被违反的细节：先在 span 里记指标、又在 span 属性里带同样的数据，聚合端就会双计。

**成本计算在 `is_recording()` 判断之前。** 注释写明：这样即使 span 被采样丢弃，`_record_metrics` 仍会发出成本数据。也就是说「追踪可以抽样，但计费不能」。

**`message_json_cache` 是一个跨请求复用的序列化缓存。** 文档说 agent 流会传一个，一次性请求传 `None`——因为输入历史每轮都在增长，不缓存就会每轮把整段历史重新序列化一遍。

**二进制内容有两套处理，且理由不同。** `_convert_binary_to_otel_part` 处理**消息内容**里的二进制，按 `include_binary_content` 决定带不带；而 `redact_binary_content` 处理「工具或输出函数产出的任意值」这类属性，它的 docstring 说明了为什么不能走同一条路：那些属性序列化任意值，而 `BinaryContent` 自己的序列化是与消息历史共享的**公共契约**，让它的行为依赖插桩设置会改变它在别处的 dump 行为。所以它选择**提前把值脱敏**：保留 `media_type`、`vendor_metadata`、`kind`、`identifier`，只丢数据本身。

**脱敏函数的失败策略是「不打断运行」。** 它在异常时返回一段固定文案（只带异常类型名），理由是：插桩不该让一次本来成功的运行失败；而**不能把原值交回去**，因为调用方会 `str(value)` 兜底，而 `BinaryContent` 的 repr 恰好会打印被那个开关排除的数据。异常消息也不带，因为它是用户可控的文本、本身可能内嵌 `BinaryContent`。

**遍历有环检测但允许重复。** `_redact_binary_content` 用 `active: set[int]`（`id`）避免无限递归；注释特别说明「同一个 `BinaryContent` 并排出现两次会被脱敏两次，而不是把第二次当成环」——因为 `active` 在退出时移除，只防真正的循环引用。

**`InstrumentationNames.for_version` 的差异是语义约定迁移的落点。** `version == 2` 给旧名（`agent run`、`running tool`、属性 `agent_name` / `tool_arguments` / `tool_response`）；否则给语义约定名（`invoke_agent`、`execute_tool`、`gen_ai.agent.name`、`gen_ai.tool.call.arguments`、`gen_ai.tool.call.result`）。两套并存让升级插桩格式不破坏已有的看板与查询。

**请求参数属性的内容可被脱敏。** `model_request_parameters_attributes(..., include_content=True)` 为假时会先 `_redact_model_request_parameters(serialized)`；若脱敏后为 `None` 则整个属性都不加，而不是加个 `null`。

**MCP 的遥测身份要单独收集并遮蔽。** `mcp_telemetry_url_ids` 从能力与工具集中收集「由 URL 派生出来的 MCP 身份」，用于在遥测里遮蔽。这条存在的原因是：MCP 服务器 URL 可能内含凭据或内部主机名，直接进 trace 就是泄漏。

## 关键取舍

**源码事实：** 内容捕获策略绑定到具体 span，且用 `set` 而非 `reset` 恢复。任何「属于别的 span」或「没有打开请求」的情形一律按不允许处理。

**源码事实：** 读取策略有两个入口、三套判据：`span_include_content` 要 span_id 相等；`open_request_policy` 要策略 span 仍在录制且同 trace；`current_model_request_span` 要 span context 完全相等。宽严不同是因为用途不同（渲染/开子 span/写属性）。

**源码事实：** 请求属性在模型调用边界定稿，span 名也会随之更新为最终模型名。这让追踪反映「实际发给谁」，而不是「计划发给谁」。

**源码事实：** 指标在 span 关闭之后记录，成本在采样判断之前计算。两条都是为了不与「按 span 属性聚合」的后端重复计数，同时保住计费数据。

**源码事实：** 二进制内容有两条不同的处理路径，因为 `BinaryContent` 的序列化是公共契约、不能依赖插桩设置。

**源码事实：** 脱敏失败时既不返回原值也不带异常消息。这是「宁可少一条属性，不可泄漏一次内容」的取向。

**源码事实：** span 名与属性名有 v2 与语义约定两套，按版本切换。

**架构推论：** 插桩层最难的从来不是「怎么记」，而是「记什么、什么时候可以记、记不上时怎么办」。这套实现把三件事都显式化了：内容捕获是**按 span 授权的**；属性定稿时机对齐**真实调用边界**；脱敏失败有**降级而非抛出**的兜底。代价是策略状态跨 `Context` 传递时会有残留，于是必须靠「自带 span 身份」来抵消——这也是任何在异步取消场景下做插桩的通用教训。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/_instrumentation.py` | `ContentPolicy` | 绑定到具体 span 的内容捕获策略，含 tracer |
| `pydantic_ai_slim/pydantic_ai/_instrumentation.py` | `span_include_content` | 单个 span 是否开了内容捕获，其余情形 fail closed |
| `pydantic_ai_slim/pydantic_ai/_instrumentation.py` | `open_request_policy` | 请求范围内应遵循的策略，供内部子 span 使用 |
| `pydantic_ai_slim/pydantic_ai/_instrumentation.py` | `current_model_request_span` | 只有正主当前 span 才能写属性的入口 |
| `pydantic_ai_slim/pydantic_ai/_instrumentation.py` | `open_model_request_span` | 开 `chat` span、定稿请求属性、`finish` 与延迟指标 |
| `pydantic_ai_slim/pydantic_ai/_instrumentation.py` | `InstrumentationNames` | span 名与属性名的集中声明 |
| `pydantic_ai_slim/pydantic_ai/_instrumentation.py` | `InstrumentationNames.for_version` | v2 旧名与语义约定名的双版本切换 |
| `pydantic_ai_slim/pydantic_ai/_instrumentation.py` | `redact_binary_content` | 值级二进制脱敏，失败时降级不抛出 |
| `pydantic_ai_slim/pydantic_ai/_instrumentation.py` | `model_attributes` | 模型身份属性：provider、base_url、请求模型名 |
| `pydantic_ai_slim/pydantic_ai/_instrumentation.py` | `model_request_parameters_attributes` | 请求参数属性，可按需脱敏或整条省略 |
| `pydantic_ai_slim/pydantic_ai/_instrumentation.py` | `mcp_telemetry_url_ids` | 收集由 URL 派生的 MCP 身份以在遥测中遮蔽 |
| `pydantic_ai_slim/pydantic_ai/_instrumentation.py` | `capture_model_response_span_context` | 在钩子可能拒绝响应之前先捕获它 |

## 参考

- [pydantic-ai 源码：_instrumentation.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_instrumentation.py)
- [Pydantic AI 文档：Logfire 与插桩](https://ai.pydantic.dev/logfire/)
- [Pydantic AI 文档：插桩能力](https://ai.pydantic.dev/capabilities/instrumentation/)
- [OpenTelemetry 语义约定：GenAI](https://opentelemetry.io/docs/specs/semconv/attributes-registry/gen-ai/)

## 与主核 / AI 的关系

插桩层与主核的性能计数器/等待事件统计同源：都是「在不改执行路径的前提下采集可观测量」。但这里多了一层主核里通常没有的复杂度——**记录内容本身是敏感操作**。模型请求里带着用户数据、凭据、内部提示词，往 trace 里写一份等于复制一份到第三方后端。

对 agent 工程的直接价值有三条。其一，**内容捕获必须按 span 授权，且默认拒绝**。`span_include_content` 的 fail-closed 语义应当被当成模板：不确定就不写。其二，**注意异步取消下的 contextvar 泄漏**。用 `reset` 恢复插桩状态在流式取消路径上会抛异常，用 `set` 又会残留——解法不是选一种写法，而是让状态自带身份、让读者自己校验。其三，**指标与属性不要双记**。把同一份数据既放进 span 属性又单独记成 metric，任何「从属性聚合」的后端都会双计；正确做法是属性供查询、指标在 span 关闭后单独发。
