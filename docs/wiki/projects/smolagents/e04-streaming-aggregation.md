---
project: smolagents
feature: streaming-aggregation
title: 流式增量与聚合
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/models.py
    symbols: [ChatMessageStreamDelta, agglomerate_stream_deltas, ChatMessageToolCallStreamDelta]
---

# 流式增量与聚合

## 一句话职责

定义流式响应的增量形状（文本、工具调用片段、用量），并给出一个把「一串增量」还原成「一条完整 `ChatMessage`」的聚合器——重点是工具调用参数跨块拼接这条最难处理的部分。

## 机制是什么

两个 dataclass 描述增量：

`ChatMessageStreamDelta` 有 `content`（文本增量，可空）、`tool_calls`（`ChatMessageToolCallStreamDelta` 列表，可空）、`token_usage`（可空）。

`ChatMessageToolCallStreamDelta` 有 `index`、`id`、`type`、`function`。`index` 是关键字段——它标识「这属于第几个工具调用」，因为一次响应可能同时要求多个工具调用，而它们的增量片段会交错出现在流里。

聚合函数 `agglomerate_stream_deltas(stream_deltas, role=ASSISTANT)` 把这些增量折成一个 `ChatMessage`。

## 怎么实现的

**三个累加器并行推进。** 函数维护三样状态：`accumulated_content`（字符串）、`accumulated_tool_calls`（`dict[int, ChatMessageToolCallStreamDelta]`，按 index 索引）、以及两个整数 token 计数。

遍历每个 delta：有用量就累加到计数；有内容就拼接到字符串；有工具调用就处理。

**index 是必须的。** 工具调用分支里第一件事是 `if tool_call_delta.index is not None:`，否则直接 `raise ValueError(f"Tool call index is not provided in tool delta: {tool_call_delta}")`。

这个强制要求不是苛刻，而是必要的：没有 index 就无法知道「这个参数片段续的是哪个调用」。代码注释在这里还留了一句 `# ?ormally there should be only one call at a time`——拼写有误但保留了下来，内容大致是「通常一次只有一个调用」，说明了为什么 index 在多数情况下都是 0 却仍然必填。

**首次出现的初始化。** 若 index 不在字典里，先放一个空壳：`ChatMessageToolCallStreamDelta(id=delta.id, type=delta.type, function=ChatMessageToolCallFunction(name="", arguments=""))`。注意 name 与 arguments 初始化为**空字符串**而不是 `None`——因为后面要往里追加。

**三个字段的更新方式各不相同。** 这是聚合逻辑最值得细看的地方：

- `id` 与 `type`：**直接覆盖**（`if tool_call_delta.id: tool_call.id = tool_call_delta.id`）。因为它们只在第一块出现一次，后续块通常不再携带。用「有值才覆盖」避免后续的空值把已有值冲掉。
- `name`：**有值才覆盖**（`if ... and len(...) > 0`）。工具名可能分块到达，也可能一次给全；取「最后非空」的策略能容忍两种。
- `arguments`：**追加拼接**（`tool_call.function.arguments += ...`）。参数是 JSON 字符串，会被任意切分在多个块里，所以必须拼接而不是覆盖。

三种不同的更新策略对应三种不同的数据特性——这正是这个聚合器不能用「简单字段合并」糊过去的原因。

**输出侧的重建。** 聚合完成后构造 `ChatMessage`：`content=accumulated_content`（可能是空字符串）、`tool_calls` 列表由字典值转换而来、每个调用固定 `type="function"`（**丢掉原始 type，写死**）、`id` 用 `tool_call_stream_delta.id or ""`（缺则空串）、`token_usage` 由两个累计值构造。

列表推导里有个 `if tool_call_stream_delta.function` 的过滤：没有 function 的增量不会进最终列表。所以「只给了 id 没给 function」的碎片不会变成空工具调用。

**用量的语义。** 聚合出的 `TokenUsage` 是**所有增量用量的总和**。这在流式场景里是个潜在陷阱：多数 provider 只在最后一个块给出**总计**用量，此时累加恰好等于总数；但若 provider 在每块都报「累计到目前的用量」，累加就会严重偏大。代码没有区分这两种语义，采用累加——这意味着它假定的是「每块报增量」。

## 关键取舍

**源码事实：** `index` 为 `None` 时抛异常而不是假定 0。这避免了「多个工具调用的参数被混拼到同一个」这种灾难性错配——宁可报错也不猜。

**源码事实：** `arguments` 用字符串拼接，所以聚合结果是**未解析的 JSON 字符串**。调用方（例如 `parse_tool_calls`）需要再做一次 `parse_json_if_needed` 才能拿到 dict。聚合器不负责解析，只负责拼装。

**源码事实：** 最终 `type` 固定成 `"function"`，忽略增量里的 `type`。所以若未来出现非函数类型的工具调用（例如服务端内置工具），这个聚合器会丢掉类型信息。

**源码事实：** 用量按增量累加，不区分「增量语义」与「累计语义」。这是一个未被处理的语义歧义。

**架构推论：** 流式聚合的复杂度全部来自「同一逻辑对象的多个片段交错到达」。文本只需顺序拼接，因为流里文本本身有序；工具调用则需要按 index 归组，因为多调用的片段会交错。这个「按 index 归组」是流式协议解析里最常见的坑：只按到达顺序拼，会把两个工具的 JSON 参数混成一段非法字符串。

**架构推论：** 聚合结果仍是「未解析的参数字符串」，把解析留给下游，是分层上的正确选择：聚合器只负责「还原结构」，解析负责「理解语义」。混在一起会让聚合器必须处理 JSON 语法错误，而那个错误的归因应该落在解析层。

**架构推论：** 三种字段三种更新策略（覆盖、有值才覆盖、拼接）看似零散，实际上是被数据形状逼出来的：id 与 type 是标量且只出现一次、name 可能分片也可能整块、arguments 必然分片。写这类聚合器时，正确做法是先弄清每个字段在协议里的分片行为，而不是统一用同一套「合并」规则。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/models.py` | `ChatMessageStreamDelta` | 单个流式增量：文本、工具调用片段、用量 |
| `src/smolagents/models.py` | `ChatMessageToolCallStreamDelta` | 单个工具调用的增量片段，带 index |
| `src/smolagents/models.py` | `agglomerate_stream_deltas` | 按 index 归组并拼接，折成完整消息 |
| `src/smolagents/models.py` | `ChatMessageToolCallFunction` | 工具调用的函数名与参数载体 |
| `src/smolagents/models.py` | `parse_json_if_needed` | 把参数字符串按需解析成字典 |

## 参考

- [smolagents 源码：models.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/models.py)
- [OpenAI 文档：Chat Completions 流式响应](https://platform.openai.com/docs/api-reference/chat/streaming)
- [smolagents 文档：Streaming](https://huggingface.co/docs/smolagents/main/en/reference/models)

## 与主核 / AI 的关系

主核里有大量同类问题：网络传输把一条逻辑记录切分成多个分片，接收端要按「流标识 + 序号」归组再拼装。`index` 在这里的角色与分片序号完全一致——它是让交错的多路数据能各自归位的唯一依据。差别在于主核的分片由协议强制编号，这里的 `index` 由 provider 给出且「通常都是 0」，所以代码不得不显式校验它的存在性。

与 OpenAI Agents SDK 的流式处理相比（见 openai-agents 的 j01 页），那边的三层事件模型里「原始事件」是直接透传给使用者的，聚合发生在「把输出项转成运行时项」这一步；smolagents 这边聚合是显式的一步函数调用，且只针对文本与工具调用。前者面向「渲染流式 UI」，后者面向「拿到完整消息以驱动下一步」。

对 AI 工程最实用的一条就是那个「按 index 归组」。任何流式工具调用解析，只要不按 index 归组，就一定会在「模型同时要求调两个工具」时出错，而错误的形态是参数 JSON 变成非法字符串——报错点在下游解析，看不出根因在聚合。把 index 做成必填并显式抛错，是让这个错误在正确的位置暴露。

另一条是「聚合不解析」的分层。聚合器输出字符串参数，解析交给 `parse_json_if_needed`，这个分工让聚合器可以对任何 provider 的片段格式保持中立，也让「JSON 不完整」这类问题归因到解析层。若在聚合器里顺手解析，就必须处理「还没收完但已经能部分解析」这种状态，复杂度会陡增。
