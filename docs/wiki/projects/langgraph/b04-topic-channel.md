---
project: langgraph
feature: topic-channel
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/channels/topic.py
    symbols: [Topic, _flatten]
---

# Topic 累积通道与元组展平

## 一句话职责

Topic 把每次写入都累积成列表，并区分「整批写入」与「元组展平写入」两种意图。

## 机制是什么

Topic 是「只追加」通道。它的值是一个列表，每轮把本轮的更新序列拼到末尾。与 `BinaryOperatorAggregate` 用算子累积不同，Topic 不接受自定义算子——它就是追加语义，并且额外处理一件事：本轮更新若是一个元组，究竟该当「一个列表元素」还是「多个元素分别追加」。

`_flatten` 就是这条判定的实现，它决定了 Topic 的写入形态边界。

## 怎么实现的

**值的表达。** 通道内部持有 `values` 列表与一个「是否可写」标记。列表本身就是累积结果，不额外包装；读取时直接给出去——这里没有做拷贝，是「读快、写慢」的取向。

**空值判定。** `get()` 在列表为空且从未写过时抛 `EmptyChannelError`。它的判定同时看列表长度与写入标记，因为「写过但写的是空元组」与「从未写过」在语义上不同。

**追加逻辑。** `update` 遍历本轮更新序列，对每个值调 `_flatten` 决定展开成多少个元素，再逐一追加。一旦有元素被追加，就把可写标记置上并返回 `True`。

**展平的判定。** `_flatten` 的核心是一个 `isinstance` 检查：只有元组才可能被展开，其他类型（包括列表）一律当作单个元素。这个选择很关键——若列表也被展开，那么「往列表里追加一个列表」这个最自然的操作就退化了，用户无法表达嵌套。

**检查点。** `checkpoint()` 返回内部列表本身，`from_checkpoint` 恢复成同样的列表。因为 Topic 的值就是普通列表，序列化不需要特殊编码。

**相等性。** `__eq__` 只判类型，不比内容——与 `LastValue` 相同的宽松策略，服务于构建期「同键是否可以合并」的判定。

## 关键取舍

**源码事实：** 只有元组会被展开，列表不会。这让「追加一个列表作为元素」成为可行写法，同时保留「一次追加多个值」的简短表达。

**源码事实：** 读取返回内部列表本身，不做拷贝。这与基类 docstring 关于「复杂结构要复制」的要求在 `from_checkpoint` 里才得到满足——`copy()` 通过检查点往返实现真正隔离。

**源码事实：** 空通道判定同时看长度与写入标记，因此「曾写过空元组」不会被误判成「从未写过」。

**架构推论：** Topic 不做累加上限，也不做去重。这意味着长时间运行的图若持续向 Topic 追加，列表会无界增长；控制规模必须由用户在上层做，通道不替用户决策。

**架构推论：** 因为值是普通列表，Topic 天然可以被外部按列表语义消费，不需要专门的访问器。这让它适合承载「事件流」这类由下游自己筛选、自己截断的数据。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/channels/topic.py` | `Topic` | 只追加通道，值是列表 |
| `libs/langgraph/langgraph/channels/topic.py` | `Topic.update` | 遍历更新，按展平结果逐一追加 |
| `libs/langgraph/langgraph/channels/topic.py` | `Topic.get` | 空列表时抛 EmptyChannelError |
| `libs/langgraph/langgraph/channels/topic.py` | `_flatten` | 元组展开、其他类型当单元素的判定 |
| `libs/langgraph/langgraph/channels/topic.py` | `Topic.checkpoint` | 返回内部列表，供检查点使用 |

## 参考

- [LangGraph 源码：channels/topic.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/channels/topic.py)
- [LangGraph 文档：reducer 与累积状态](https://langchain-ai.github.io/langgraph/concepts/low_level/#reducers)
- [LangGraph 文档：并行分支写入](https://langchain-ai.github.io/langgraph/how-tos/branching/)

## 与主核 / AI 的关系

「只追加不覆盖」在主核里对应的是 WAL 与追加型日志：写入不可回改，消费方自行决定读到哪里。差别在于主核的追加记录有固定格式与回收机制，Topic 只是内存里的 Python 列表。

对 agent 而言，Topic 适合承载「多分支各自产生的事件」。但因为它无界增长，把它当消息历史用时必须在上层截断；真正需要「按消息语义合并、可去重、可删改」的场景应该用消息专用的 reducer，而不是 Topic 的裸追加。
