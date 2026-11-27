---
project: autogen
feature: chat-completion-context
title: 模型上下文基类与状态存取
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/model_context/_chat_completion_context.py
    symbols: [ChatCompletionContext, add_message, get_messages]
---

# 模型上下文基类与状态存取

## 一句话职责

把「喂给模型的那一串消息」从智能体里剥出来做成可替换的组件：基类只负责存与清，`get_messages` 留给子类实现，于是「记得多少」成为可配置的策略而不是硬编码的逻辑。

## 机制是什么

模型上下文是一个很窄的抽象，窄到只有四个动作：`add_message` 追加、`get_messages` 取视图、`clear` 清空、以及 `save_state`/`load_state` 存取快照。其中只有 `get_messages` 是抽象方法——因为它是**唯一的策略位**：存什么、留多少、按什么顺序返回，全在这个方法里决定。

基类刻意只做「无策略的存储」：内部维护一个 `_messages` 列表，`add_message` 就是 `append`，`clear` 就是换一个空列表。没有裁剪、没有去重、没有排序。所有窗口策略（最近 n 条、头尾各留、按 token 上限、完全不裁）都是子类覆写 `get_messages` 得到的。

它同时是组件：`ChatCompletionContext(ABC, ComponentBase[BaseModel])`，`component_type = "chat_completion_context"`。所以上下文策略可以被 dump 成配置、随智能体定义一起序列化——这是它必须做成类而不是函数的关键理由。

## 怎么实现的

**两个列表。** `__init__(initial_messages=None)` 里维护了**两份**引用：`self._messages` 是当前全部消息（`initial_messages` 的元素被逐条 `extend` 进来），`self._initial_messages` 保留构造时传入的那个列表本身。这不是冗余——`_messages` 会随对话一直长，而 `initial_messages` 用于 `_to_config` 时 dump 出「构造这份上下文需要什么」。子类 `_to_config` 里 dump 的都是 `self._initial_messages` 而非 `self._messages`，正是这个分工的直接体现。

**追加与清空。** `add_message` 是 `async` 但不 `await` 任何东西，纯 `append`——保持异步签名是为了让子类可以覆写成真的异步（例如写盘或写远端）。`clear` 是 `self._messages = []`，**换对象而非 `clear()` 就地清空**：任何早前拿到的列表引用不会被清空，只是与上下文脱钩。

**状态存取。** `save_state` 把消息列表装进 `ChatCompletionContextState` 再 `model_dump()`；`load_state` 走 `model_validate(state).messages` 并**整体替换** `_messages`。没有合并、没有增量。`ChatCompletionContextState` 只有 `messages: List[LLMMessage]` 一个字段，`default_factory=list` 保证空状态可解析。`LLMMessage` 是带 `type` 判别器的联合类型，所以状态是一份**可读、可 diff 的 JSON**，而不是 pickle。

**序列化边界。** 状态里的消息走 pydantic 的判别联合序列化，因此 `save_state` 的产物能跨进程、跨版本被读回；但这也意味着**消息类本身必须能被 pydantic 序列化**。`UserMessage.content` 允许 `List[Union[str, Image]]`，而 `Image` 自定义了 `__get_pydantic_core_schema__`（`to_base64()` 存成 `{"data": ...}`），所以图文混排的消息同样能进状态——这条链路是隐式的，不看 `Image` 的实现就不知道状态里其实塞了 base64。

**继承链。** 四个具体实现各自 `Component[<Config>]`：`UnboundedChatCompletionContext`、`BufferedChatCompletionContext`、`HeadAndTailChatCompletionContext`、`TokenLimitedChatCompletionContext`。基类只提供 `ComponentBase`，因此它可被当作接口使用（`ChatCompletionContext.load_component(...)`），具体类补上 `Component` 才具备 `_from_config`。

## 关键取舍

**源码事实：** 只有 `get_messages` 是 `@abstractmethod`。这意味着默认策略是「调用方自己实现」，没有「无策略」的默认实现——想不裁剪就必须显式继承 `UnboundedChatCompletionContext`。这个选择让「忘了裁剪」不会静默发生。

**源码事实：** `get_messages` 的返回类型是 `List[LLMMessage]` 且基类不做防御性拷贝。`UnboundedChatCompletionContext` 直接返回 `self._messages` 本体，于是调用方 `append`/`pop` 会直接改动上下文；而 `BufferedChatCompletionContext` 返回切片（新列表）。**同一条契约下两种可变性语义**，子类各自决定。

**源码事实：** `load_state` 整体替换而非追加。恢复语义是「回到那个时刻」，不是「把历史接上」。若在同一实例上先 `load_state` 再 `add_message`，得到的是「快照 + 新消息」，符合直觉；但若先 `add_message` 再 `load_state`，之前追加的消息会被丢掉。

**源码事实：** `_initial_messages` 保存的是**引用**而非拷贝。若调用方在构造后修改了自己传进来的那个列表，`dump_component` 的结果会随之改变——一份「已经 dump 过的配置」不再是快照。

**架构推论：** 把策略收敛到一个方法上，使上下文成为可替换的**横切关注点**：同一个智能体换上下文类就换了记忆能力，业务代码一行不改。代价是每个实现都要自己处理「工具调用结果必须成对出现」这类约束（后续页的三种实现都在 `get_messages` 里手工剥掉孤立的 `FunctionExecutionResultMessage`，没有任何共享的辅助函数）。

**架构推论：** 因为上下文是组件，它的配置里可以嵌别的组件配置——`TokenLimitedChatCompletionContext` 就把 `model_client` dump 成嵌套 `ComponentModel`。这让「上下文依赖模型」这件事在配置层显式可见，也带来嵌套加载的失败传播：外层 `_from_config` 里调 `ChatCompletionClient.load_component`，内层白名单校验失败会直接冒泡。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/model_context/_chat_completion_context.py` | `ChatCompletionContext` | 上下文基类：存、清、状态存取，策略留白 |
| `python/packages/autogen-core/src/autogen_core/model_context/_chat_completion_context.py` | `ChatCompletionContext.add_message` | 追加消息，异步签名留给子类扩展 |
| `python/packages/autogen-core/src/autogen_core/model_context/_chat_completion_context.py` | `ChatCompletionContext.get_messages` | 唯一抽象方法，全部窗口策略的实现位 |
| `python/packages/autogen-core/src/autogen_core/model_context/_chat_completion_context.py` | `ChatCompletionContext.clear` | 换空列表，不与旧引用共享 |
| `python/packages/autogen-core/src/autogen_core/model_context/_chat_completion_context.py` | `ChatCompletionContext.save_state` | 消息列表打包成可序列化状态 |
| `python/packages/autogen-core/src/autogen_core/model_context/_chat_completion_context.py` | `ChatCompletionContext.load_state` | 整体替换消息列表 |
| `python/packages/autogen-core/src/autogen_core/model_context/_chat_completion_context.py` | `ChatCompletionContextState` | 状态模型，单字段 `messages` |
| `python/packages/autogen-core/src/autogen_core/model_context/_chat_completion_context.py` | `_initial_messages` | 保存构造入参，供 `_to_config` 复原配置 |

## 参考

- [AutoGen 源码：_chat_completion_context.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/model_context/_chat_completion_context.py)
- [AutoGen 文档：Model Context](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/components/model-context.html)
- [Pydantic 文档：判别联合与序列化](https://docs.pydantic.dev/latest/concepts/unions/)

## 与主核 / AI 的关系

主核的 buffer pool 也区分「保留什么」与「怎么保留」：页面在内存里全都在，取用时的可见性由锁与快照决定；而这里 `_messages` 也全都留着，「可见性」只在 `get_messages` 那一刻被决定。两者的共同点是把**全量存储**与**受控视图**分离——存储层不承担语义裁剪，语义只在读取路径上施加。差别在于主核的裁剪必须保证事务正确性（不能读到未提交版本），这里的裁剪只需要让模型不超限。

对 AI 工程最实际的一条：**上下文是有状态的，而状态是可序列化的**。`save_state` 产出的是 pydantic JSON，意味着一次对话的全部输入可以落盘、可 diff、可回放到测试里。这比「打印日志」强得多，因为它是精确的模型输入。反过来说，任何不进状态的东西（例如 `_initial_messages` 之外的构造参数）在恢复后都会丢失，恢复出来的上下文不会自动重建智能体的其余部分。

还有一条边界值得提前知道：`add_message` 追加的是调用方传进来的那个对象本身，不是拷贝。若上层在 `add_message` 之后改了这个消息实例的字段，已进入上下文的内容会一起变——消息在语义上是不可变的，但类型系统没拦。

最后，`get_messages` 的「返回视图还是返回本体」没有统一约定，这是接入自定义上下文实现时最容易踩的坑：调用方若习惯性地对返回值做原地修改，在 `UnboundedChatCompletionContext` 上会污染历史，在 `BufferedChatCompletionContext` 上则毫无效果。
