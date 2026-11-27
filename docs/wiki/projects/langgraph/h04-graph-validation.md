---
project: langgraph
feature: graph-validation
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/_validate.py
    symbols: [validate_graph, validate_keys]
---

# 图结构与键校验

## 一句话职责

`validate_graph` 在编译产物成形之后、真正运行之前，把「节点读的通道、订阅的触发通道、输入输出键、断点节点」四类引用逐一对账，任何一处指向不存在的东西就当场报错。

## 机制是什么

校验分两层，职责不同。**builder 层的 `StateGraph.validate` 看拓扑**：所有边的起点与终点是否都是已知节点、有没有从 START 出发的入口、条件边的 `ends` 是否合法。**Pregel 层的 `validate_graph` 看引用**：节点声明要读的通道是否存在、节点订阅的触发通道是否存在、输入通道是否至少被某个节点订阅、输出与流式通道是否存在、断点节点是否真实存在。

这一页说的是第二层。它拿到的输入是已经装配好的 `nodes`（`PregelNode` 映射）、`channels`（通道实例）、`managed`（托管值规格）、以及输入/输出/流式/断点四组键。

`validate_keys` 是它的小号版本：只做「这些键是否都在 channels 里」这一件事，供每次运行前校验 `output_keys` 用。

## 怎么实现的

**先查保留名。** 三类名字各查一遍 `RESERVED`：通道名、托管值名、节点名。保留集合里装的是框架内部写入键（`__interrupt__`、`__resume__`、`__error__`、`__no_writes__` 等）、内部配置键，以及 `langsmith:hidden` 这个标签。用户若用这些名字建节点或建通道，框架的内部写入协议就会被顶掉，所以宁可编译期直接失败。

**遍历节点做三件事。** 对每个 `name, node`：名字查保留；若 `node` 不是 `PregelNode` 就抛 `TypeError`（注释写着「expected PregelNode or NodeBuilder」——说明这里也接住了一个本该更早失败的装配错误）；累加 `node.triggers` 到 `subscribed_channels`，并逐个检查 `node.channels` 里的通道存在。

**channels 与 managed 的判定是分开的。** 节点读的通道可以在 `channels` 里，也可以在 `managed` 里——托管值不占通道槽位，但仍可作为节点输入。所以循环里两个集合都要查。而 `node.channels` 是 `str` 时只查 `channels`（单通道形态下不可能是托管值）。

`main.py` 传参的方式揭示了两者的实际关系：它把 `self.channels` 里**不是** `BaseChannel` 实例的项切成第二份传进来当 `managed`。也就是说托管值在装配阶段确实混在 `channels` 字典里，靠类型分辨。

**订阅集合要复查。** 节点声明的 triggers 未必都在 `channels` 里——特别是触发通道由编译期生成时容易漏。所以先累加到集合，最后统一对 `channels` 复查一遍。这一步不能省：它也是把「所有触发通道」与「所有通道」做交集校验的唯一位置。

**输入通道的两段检查。** 若 `input_channels` 是单个字符串：它必须存在，**并且**必须被某个节点订阅。若是一组：每个都要存在，且**至少一个**被订阅。后一条是关键设计——允许多通道输入里只有部分被订阅，但不能全部落空，否则喂进去的数据永远没人读。

**输出与流式通道合并检查。** 先把 `output_channels` 与 `stream_channels`（可能是 str 或序列，也可能为 `None`）合进一个集合，再统一查存在性。合并的理由是两者的校验规则完全一样，没必要分两遍。

**断点节点。** `interrupt_after_nodes` / `interrupt_before_nodes` 若等于 `"*"` 就整段跳过（那是「所有节点」的简写，不是名字）；否则逐个查是否在 `nodes` 里。

**错误文案带上下文。** 几条主要错误都把已知通道名的 `repr(sorted(channels))` 截断到 100 字符塞进消息。这不是为了排版，而是让「拼错一个键名」在报错里能直接看到正确的候选。

## 关键取舍

**源码事实：** 校验只查「引用是否存在」，不查「引用是否合理」。例如「某节点订阅了通道 A 但从不写 A」这种悬空依赖不会被拦——那是运行期行为问题，静态判不了。

**源码事实：** 校验发生在 `Pregel.validate()` 里，而且它除了校验还顺带算出 `trigger_to_nodes`。校验与派生索引绑在同一次遍历上，省一遍扫描，也保证两者看到的是同一份节点表。

**源码事实：** `StateGraph.validate` 与 `validate_graph` 的错误类型不同：拓扑错误抛 `ValueError` 带具体节点名，类型错误抛 `TypeError`。分成两层意味着用户可能先撞到拓扑错误、修完才撞到通道引用错误——报错是逐步暴露的，不是一次性汇总。

**架构推论：** 把校验放在编译期而非运行期，代价是编译要遍历全部节点与通道，收益是运行期完全不需要再判断「这个通道存在吗」。这与 Pregel 执行循环「只认通道、不做类型判断」的设计互为前提。

**架构推论：** 输入通道「至少一个被订阅」这条规则，实际上是在静态层面检测「入口数据被吞掉」这一整类 bug。它挡不住逻辑错误，但能挡住最常见的装配失误。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/_validate.py` | `validate_graph` | 通道/节点/输入输出/断点的全量引用校验 |
| `libs/langgraph/langgraph/pregel/_validate.py` | `validate_keys` | 单组键的存在性校验，供每次运行前使用 |
| `libs/langgraph/langgraph/pregel/main.py` | `Pregel.validate` | 调用校验并把通道字典按类型切成 channels 与 managed |
| `libs/langgraph/langgraph/pregel/main.py` | `_trigger_to_nodes` | 校验之后顺带派生的触发索引 |
| `libs/langgraph/langgraph/_internal/_constants.py` | `RESERVED` | 保留名集合：内部写入键与配置键 |
| `libs/langgraph/langgraph/graph/state.py` | `StateGraph.validate` | builder 层的拓扑校验，先于 Pregel 层执行 |

## 参考

- [LangGraph 源码：pregel/_validate.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_validate.py)
- [LangGraph 源码：pregel/main.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/main.py)
- [LangGraph 文档：低层概念](https://langchain-ai.github.io/langgraph/concepts/low_level/)

## 与主核 / AI 的关系

「编译期全量对账、运行期零检查」与主核把 SQL 里的表名与列名在分析阶段就绑定到 OID 的做法完全一致：运行时拿到的都是已经解析好的对象，不需要再查字典。这里绑定的对象是通道名，索引是 `trigger_to_nodes`。

对 AI 侧的实际价值是错误定位速度。图是声明式拼出来的，一个键名拼错不会在拼的时候报错，只会在跑到那个节点时莫名拿到空输入。`validate_graph` 把这类问题提前到 `compile()`，并在消息里列出已知通道名——这是写多节点 agent 时最常踩的一类坑，报错文案里那份候选列表通常比堆栈更有用。
