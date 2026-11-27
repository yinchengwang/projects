---
project: pydantic-ai
feature: output-validation
title: 输出校验与不完整输出处理
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/_output.py
    symbols: [OutputValidator, is_incomplete_json, truncated_output_message]
---

# 输出校验与不完整输出处理

## 一句话职责

在模型已经交出结果之后，再插两道独立的检查：一道是应用自己写的 `OutputValidator`（能拒绝并让模型重来），一道是框架对「输出被 token 上限截断」的专门识别——两者都不负责解析，只负责判定「这份输出能不能算数」。

## 机制是什么

`OutputValidator` 是一个极简 dataclass，只有一个真正需要用户提供的字段 `function`，外加两个在 `__post_init__` 里推出来的标志：`_takes_ctx`（函数是否接受上下文）与 `_is_async`（是否协程）。

它的 `validate(result, run_context)` 签名的关键在于：**原样传播用户抛出的 `ModelRetry`**，不包装。文档写明调用方（输出处理钩子、`stream_text` 等）自行决定是把它包成 `ToolRetryError` 走重试，还是直接上抛。这意味着校验器本身不需要知道重试机制存在。

`is_incomplete_json` 是另一类判定：它只回答「这个 `ValidationError` 是不是因为 JSON 在完整之前就断了」。判断方式是检查错误详情里有没有 `type == 'json_invalid'`。

`truncated_output_message` 与 `incomplete_tool_call_message` 是两条错误文案生成器，都把「token 上限是多少」写进消息，并给出两个可操作的出路：加大 `max_tokens`，或者简化提示词。

## 怎么实现的

**同步校验器也不会阻塞事件循环。** `validate` 分两条路：若 `_is_async` 为真直接 `await`；否则先 `run_in_executor` 再 `await_maybe`。`await_maybe` 的存在是必要的：一个普通 `def` 也可能返回 awaitable，如果只 `run_in_executor` 就拿到一个没被 await 的协程对象，结果会静默变成一个「永远为真」的返回值。这行代码防的是一类很难查的 bug。

**`_takes_ctx` 用参数个数判定。** `len(inspect.signature(self.function).parameters) > 1`——大于一个就认为第一个是上下文。这个规则很粗，但足够用：校验器要么签一个参数（结果），要么签两个（上下文、结果）。它没有做类型注解检查，因此带默认值的第二参数也算「接受上下文」，这属于约定而非校验。

**不完整 JSON 与「校验失败」被刻意区分。** `is_incomplete_json` 的文档说明写得很清楚：**schema 或输出校验器拒绝了一份能解析的 JSON，那个错误保留它的重试机会**。只有 `json_invalid` 才被认定为截断。这条区分的实际后果是：如果模型的输出本来就是错的结构，会走正常的重试/报错路径；只有当输出被 token 上限砍断时，才走「提高 max_tokens」这条专门的死路提示。

**工具调用参数被截断有独立判定。** `are_tool_call_args_cut_off` 检查一个 `ToolCallPart`：参数为空 → 截断（并注明 Anthropic 在截断的工具调用上会发 `{}`，流式时干脆不发输入）；参数是字符串且 `from_json` 失败 → 截断；参数能解析 → **不算截断**，仍保留重试。这里用的是 `pydantic_core.from_json` 而不是 `json.loads`，与框架其余部分的 JSON 解析实现保持一致。

**两条文案把「怎么办」写进错误。** `truncated_output_message` 与 `incomplete_tool_call_message` 都不只陈述事实，而是给出两个具体动作（增 `max_tokens`、简化提示）。文案里的上限值取 `max_tokens or "provider default"`——把「没配」和「配了」区分开，避免用户看到 `0` 或 `None` 这种无意义值。

**重试结果必须与真实返回一致。** `build_retried_tool_return` 的文档点明它的用途：这是 agent 循环处理失败时模型**真正收到**的那条结果，因此任何其它要展示同一失败的通道（比如插桩 span）都必须用同一方式构造。它把 `ValidationError` 的详情序列化成结构化内容，序列化方式与 `RetryFeedbackPart` 一致，且明确 `include_context=False` 丢掉 `ctx`、`input` 每个不同值只保留一次——这两个削减都是为了不让错误反馈把整份输入再灌回上下文。

**`cause` 是显式区分的。** `ToolReturnPart` 上的 `retry.cause` 取 `'validation_error'` 或 `'model_retry'`，由错误类型推出。工具管理器在工具真正运行之前就拒绝一个调用时，会传自己的 cause。这个字段让下游能区分「参数不合法」与「工具逻辑主动要求重来」，而这两者的正确处置方式不同。

**`outcome='retried'` 是一个专门的结果状态。** 失败的结果不是「失败」，而是「需要重试」。文档说明一个非成功返回永远不会被提升成该 kind 的类型化子类——即一条重试结果不会伪装成正常结果。

**输出函数的重试与校验器重试共用同一套反馈通道。** `make_retry_signal` 把 `ValidationError` 或 `ModelRetry` 统一转成 `ToolRetryError`，并接受 `run_context`。这条函数是「输出侧的错误」通往「工具侧的反馈机制」的桥：输出校验失败最终会以一条工具返回的形式回到模型，走与工具参数校验失败完全相同的重试预算。

## 关键取舍

**源码事实：** 截断与校验失败是两条不同的失败分类。截断的出路是「加大预算」，校验失败的出路是「模型重来」。把它们混为一谈会导致一个典型的坏体验：模型输出被砍断，错误却提示「schema 不匹配」，模型于是不断重试同样的长度直到预算耗尽。

**源码事实：** `OutputValidator.validate` 不包装 `ModelRetry`。这让校验器保持纯粹（只表达「不行，原因如下」），但把「包成重试还是直接失败」的决定推给每个调用点，因此每个调用点都必须自行处理这个异常——否则 `ModelRetry` 会漏到用户层。

**源码事实：** `build_retried_tool_return` 会刻意丢弃 `ctx` 并去重 `input`。错误反馈本身是上下文污染的主要来源之一，这里用两条具体削减来控制它的体积。

**源码事实：** 同步校验器通过 `run_in_executor` 执行，且额外 `await_maybe` 兜住「普通函数返回协程」的情形。这属于在语言层面的不严谨上做防御。

**源码事实：** 判断「是否接受上下文」只看参数个数，不做注解校验。约定优于校验，代价是签名写错时不报错、只是行为不对。

**架构推论：** 输出校验与输出解析是两件事，但在这个框架里它们被安排成串联的两道：先解析（把文本/工具参数变成对象），再校验（应用判断这个对象对不对），校验失败则借工具重试机制回灌反馈。这条链路的价值在于**应用层的业务规则可以参与重试循环**，而不需要自己实现「失败后重新请求模型」。代价是每次校验失败都消耗一个重试预算，因此校验器应当廉价、确定、不联网。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/_output.py` | `OutputValidator` | 应用输出校验器，原样传播 `ModelRetry` |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `OutputValidator.validate` | 同步/异步统一执行，`await_maybe` 兜住返回协程的普通函数 |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `is_incomplete_json` | 只认 `json_invalid` 为截断，其余解析失败保留重试 |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `are_tool_call_args_cut_off` | 工具调用参数的截断判定，空参数也算 |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `truncated_output_message` | 结构化输出截断的文案，含两条可行出路 |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `incomplete_tool_call_message` | 工具调用参数截断的文案 |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `build_retried_tool_return` | 重试结果项构造，模型真正收到的那条 |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `make_retry_signal` | 把校验/重试错误统一成工具重试信号 |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `OutputSubmission` | 工具主动提交输出的结果状态：待决、接受、拒绝 |
| `pydantic_ai_slim/pydantic_ai/_output.py` | `BaseOutputProcessor.hook_validate` | 校验阶段与执行阶段的分离点，语义值 + 不透明状态 |

## 参考

- [pydantic-ai 源码：_output.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_output.py)
- [pydantic-ai 源码：exceptions.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/exceptions.py)
- [Pydantic AI 文档：输出校验与重试](https://ai.pydantic.dev/output/#output-validators)

## 与主核 / AI 的关系

这一层与主核的「结果集校验」思路相通：先按声明的形状解析，再由应用施加业务约束，不满足则拒绝而不是默默接受。差别在于主核的校验失败会终止查询，这里的校验失败会借助重试预算把反馈交回给模型——也就是说它把「错误」当成了一次协商机会。

对 agent 工程的直接价值有三条。其一，**区分截断与不合规是必要的第一道分诊**。两者都表现为「解析不出来」，但正确处理方向相反；不做这个区分，重试预算会浪费在注定失败的方向上。其二，**校验器要廉价且确定**。它跑在重试循环里，任何联网或慢操作都会把一次失败放大成多次昂贵的模型调用。其三，**错误反馈就是新的上下文**。`ctx` 丢弃、`input` 去重这类削减看起来是细节，实际上决定了重试是「有帮助的提示」还是「把问题重新灌一遍」。
