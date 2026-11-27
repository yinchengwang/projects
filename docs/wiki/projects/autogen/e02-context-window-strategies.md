---
project: autogen
feature: context-window-strategies
title: 上下文窗口的四种裁剪策略
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/model_context/_buffered_chat_completion_context.py
    symbols: [BufferedChatCompletionContext, get_messages]
  - path: python/packages/autogen-core/src/autogen_core/model_context/_head_and_tail_chat_completion_context.py
    symbols: [HeadAndTailChatCompletionContext]
  - path: python/packages/autogen-core/src/autogen_core/model_context/_token_limited_chat_completion_context.py
    symbols: [TokenLimitedChatCompletionContext]
---

# 上下文窗口的四种裁剪策略

## 一句话职责

在不改变「上下文」这一抽象的前提下，用四个 `get_messages` 实现覆盖从「全留」到「按 token 预算删中间」的全部取舍，让「模型能看到多少历史」成为一个可替换的决定。

## 机制是什么

四个实现共享同一份存储（基类的 `_messages` 全量保留），差别只在 `get_messages` 返回什么：

- `UnboundedChatCompletionContext` 全量返回，不做任何裁剪。
- `BufferedChatCompletionContext` 取尾部 `buffer_size` 条（MRU 语义）。
- `HeadAndTailChatCompletionContext` 取头部 `head_size` 条加尾部 `tail_size` 条，中间插一条占位消息说明跳过了多少条。
- `TokenLimitedChatCompletionContext` 反复删除**中间位置**的消息，直到满足 token 预算。

前三者按条数裁剪，第四者按 token 裁剪且必须依赖一个 `ChatCompletionClient` 来数 token。这是这套策略里唯一的「外部依赖」，也是它被标注为实验性的原因。

一个共同的补丁贯穿三个裁剪实现：**若裁剪后的首条消息是 `FunctionExecutionResultMessage`，就把它丢掉**。原因是 OpenAI 协议要求 `FunctionExecutionResultMessage` 必须紧跟在含 `FunctionCall` 的 `AssistantMessage` 之后；裁剪掉配对的前半后，孤立的执行结果会让整条请求被拒。这个修正没有任何共享函数，三处各写一遍，头部裁剪的镜像情况（尾部留下孤立 `AssistantMessage(FunctionCall)`）也在头部处理。

## 怎么实现的

**Unbounded。** `get_messages` 只有一行 `return self._messages`——返回本体，不是拷贝。所以它也是四个实现里唯一「返回可变引用」的。

**Buffered。** 构造期 `buffer_size <= 0` 抛 `ValueError`。`get_messages` 先 `self._messages[-buffer_size:]`。注意这是**切片**，即使 `buffer_size` 大于总数也安全（Python 切片的越界行为是截断），并且得到的是新列表。随后检查首元素类型，是 `FunctionExecutionResultMessage` 就 `messages[1:]`。

**HeadAndTail。** 构造期校验 `head_size` 与 `tail_size` 都大于 0。`get_messages` 分三步：
1. 取 `_messages[:head_size]`；若其**末条**是 `AssistantMessage` 且 `content` 是列表且全为 `FunctionCall`，丢掉末条（否则尾部会出现孤立函数调用）。
2. 取 `_messages[-tail_size:]`；若其首条是 `FunctionExecutionResultMessage`，丢掉首条。
3. 计算 `num_skipped = len(self._messages) - head_size - tail_size`；若 `<= 0` 说明总数不够填满头尾，直接**返回全量**（而不是返回拼接结果，那是为了避免重复）。否则插一条 `UserMessage(content=f"Skipped {num_skipped} messages.", source="System")`，拼成 `head + placeholder + tail`。

第三步的 `num_skipped` 用原始长度算，而不是用裁剪后的 `head_messages`/`tail_messages` 长度算——所以当头部或尾部被剥掉一条时，占位文案里的数字仍反映「原始列表里被跳过的条数」，与实际返回的条数之差不是恒等关系。

**TokenLimited。** 构造期校验 `token_limit`（若非 None）大于 0；保存 `model_client`、`tool_schema`（默认空列表）。`get_messages` 分两支：
- `token_limit is None`：循环 `remaining_tokens(messages, tools=...)`，小于 0 就删一条，直到非负或列表空。
- 有 `token_limit`：循环 `count_tokens(messages, tools=...)`，大于上限就删一条。

两支的删除点都是 `middle_index = len(messages) // 2` —— **从中间删**，而不是从头部删。这是刻意的：既保住开头（通常是 system 提示与任务设定），也保住结尾（最新的对话轮次），只牺牲中段。最后同样做孤立 `FunctionExecutionResultMessage` 的剥离。

**组件配置。** 四个实现都是 `Component[<Config>]`，各自有对应的 Config 类。`TokenLimitedChatCompletionContextConfig` 里 `model_client` 的类型是 `ComponentModel` 而非客户端实例——dump 时调 `self._model_client.dump_component()`，load 时调 `ChatCompletionClient.load_component(config.model_client)`。其余三个 Config 都是「尺寸参数 + `initial_messages`」的直白组合。

## 关键取舍

**源码事实：** `BufferedChatCompletionContext` 的组件文档只说取最后 n 条，代码里还额外剥掉首条孤立函数结果——文档与实现不同步，读行为必须看代码。

**源码事实：** `HeadAndTailChatCompletionContext` 在消息总量不足时返回 `self._messages`（原列表本体），足够时才返回新列表。所以它同样存在「返回可变引用」的路径，且这条路径只在短对话里命中。

**源码事实：** `TokenLimitedChatCompletionContext` 的删除策略是「从中间反复删」。这既不是最优（删一条大消息可能就够，删一堆小消息反而多余），也不是贪心（不看单条 token），而是**结构最简单的近似**。循环每轮都要完整重算一次 token 数，最坏复杂度是 O(n²) 次 tokenize。

**源码事实：** `token_limit is None` 分支用 `remaining_tokens`，即「不超过模型上限」；给了 `token_limit` 就用 `count_tokens` 即「不超过显式预算」。两个分支的判据不同，但循环体与删除点完全一致。

**源码事实：** 四处孤立消息修正都建立在「列表非空」与「首/末元素类型判断」上，没有统一的辅助函数。所以新增一个裁剪策略就得把这套规则再抄一遍，漏抄是典型故障。

**架构推论：** 三种条数策略都不看 token，因此在图文混排或工具结果很长时会严重低估占用：`buffer_size=10` 的十条消息可能远超上下文上限。反过来 token 策略不看结构，可能把「一问一答」削成只剩半轮。生产环境里通常需要两层：外层 token 预算兜底，内层结构修正。

**架构推论：** 裁剪只发生在读取时，`_messages` 永远保留全量。所以 `save_state` 保存的是完整历史而非裁剪结果——恢复后可以换一个更宽松的策略读出更多内容。这是把策略与存储解耦的实际收益。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/model_context/_unbounded_chat_completion_context.py` | `UnboundedChatCompletionContext` | 全量返回，返回原列表 |
| `python/packages/autogen-core/src/autogen_core/model_context/_buffered_chat_completion_context.py` | `BufferedChatCompletionContext` | 尾部 n 条，剥首条孤立执行结果 |
| `python/packages/autogen-core/src/autogen_core/model_context/_buffered_chat_completion_context.py` | `BufferedChatCompletionContext.get_messages` | 切片加孤立消息修正 |
| `python/packages/autogen-core/src/autogen_core/model_context/_head_and_tail_chat_completion_context.py` | `HeadAndTailChatCompletionContext` | 头尾各留，中间插跳过提示 |
| `python/packages/autogen-core/src/autogen_core/model_context/_head_and_tail_chat_completion_context.py` | `HeadAndTailChatCompletionContext.get_messages` | 三步裁剪加 `num_skipped` 占位 |
| `python/packages/autogen-core/src/autogen_core/model_context/_token_limited_chat_completion_context.py` | `TokenLimitedChatCompletionContext` | 按 token 预算从中间删消息 |
| `python/packages/autogen-core/src/autogen_core/model_context/_token_limited_chat_completion_context.py` | `TokenLimitedChatCompletionContext.get_messages` | 两种预算判据共用的删中间循环 |
| `python/packages/autogen-core/src/autogen_core/model_context/_token_limited_chat_completion_context.py` | `TokenLimitedChatCompletionContextConfig` | 嵌套 `ComponentModel` 形式的模型客户端配置 |

## 参考

- [AutoGen 源码：BufferedChatCompletionContext](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/model_context/_buffered_chat_completion_context.py)
- [AutoGen 源码：TokenLimitedChatCompletionContext](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/model_context/_token_limited_chat_completion_context.py)
- [AutoGen 文档：Model Context](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/components/model-context.html)

## 与主核 / AI 的关系

主核里最接近的类比是排序与物化时的内存上限：`tuplesort` 在内存预算内排序，超了就落盘归并，策略的选择直接决定性能与是否 OOM。这里的权衡更粗糙——没有溢出路径，超限就**丢弃**。删掉的中间消息不会落盘、不会被重新拉回，信息永久丢失。所以「预算」在这里不是性能旋钮，而是正确性的边界：设小了模型会忘记关键约束，设大了请求被 API 拒绝。

对 AI 工程最要紧的一条是那个反复出现的孤立消息修正。它揭示了工具调用在协议层是**成对结构**而非独立消息：`AssistantMessage(FunctionCall)` 与 `FunctionExecutionResultMessage` 必须相邻。任何按条数裁剪的策略都会偶发破坏这个配对，因此每个实现都得手工补。写自定义上下文时，这是第一件要检查的事——否则会出现「平时都正常，偶尔请求 400」。

另一条是 token 数的**可测性**。`count_tokens` 与 `remaining_tokens` 由客户端实现，不同客户端对同一批消息给出的数字不同（OpenAI 侧用 tiktoken 估，Ollama 侧另有算法）。因此 `TokenLimitedChatCompletionContext` 的结果天然依赖具体客户端，测试里必须为不同客户端设不同 `token_limit`——仓内测试正是这么做的，OpenAI 用例给 30、Ollama 用例给 20。

最后，从中间删是个值得借用的思路：它保住两头（任务设定与最新进展），牺牲可再获得的中段过程。相比「丢最早的」会丢掉 system 提示、「丢最新的」等于没记忆，从中间删在多数对话场景下信息损失最小。
