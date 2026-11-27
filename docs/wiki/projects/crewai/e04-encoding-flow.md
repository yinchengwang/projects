---
project: crewai
feature: encoding-flow
title: 记忆编码与去重
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/memory/encoding_flow.py
    symbols: [EncodingFlow, batch_embed, intra_batch_dedup]
---

# 记忆编码与去重

## 一句话职责

`EncodingFlow` 是写入侧的五步批处理流水线：一次批量嵌入、批内近似去重、并发找相似旧记录、并发做字段推断与合并决策、最后批量执行删除/更新/插入——目标是把「N 条记忆入库」压缩成尽量少的嵌入调用、尽量少的 LLM 调用与一次原子写入。

## 机制是什么

流水线的每一步都有明确的省成本意图，源码开头的注释直接列了出来：一次嵌入调用、一个余弦矩阵、N 个并发存储搜索、N 个并发 LLM 调用、一次批量重嵌入 + 一次批量写。

状态分两层：

- `EncodingState`：批级——条目列表、四个计数器（插入/更新/删除/批内丢弃数）。
- `ItemState`：条目级——调用方给的字段（`scope` / `categories` / `metadata` / `importance` / `source` / `private` / `root_scope`），解析后的字段（`resolved_*`），嵌入向量、是否被丢弃、相似旧记录、最高相似度、合并计划、最终记录。

两层分离的意义在于：批级只做聚合与计数，条目级保留每个条目的独立命运——某条被丢弃不影响其他条目继续。

## 怎么实现的

**第一步：一次嵌入。** `batch_embed` 只做一件事：把所有条目内容一次性交给 `embed_texts`。`embed_texts` 本身会跳过空文本并保持索引对齐（空文本得到空向量），所以后续步骤可以放心假设「`item.embedding` 为空 = 这条没有可用向量」。

**第二步：批内去重是 O(n²) 的贪心。** `intra_batch_dedup` 对每对 (i, j)（j > i）算余弦相似度，超过 `batch_dedup_threshold`（默认 0.98）就把后者标为丢弃并跳出内层循环。两个细节：

- 只允许「后出现的被丢弃」，先出现的保留。这使结果对输入顺序敏感，但保证了确定性。
- 跳过已丢弃或没有向量的条目。所以「被丢弃的条目不会成为后续条目的比较对象」——去重是相对于「幸存者」做的。

阈值 0.98 定得很高，源码注释解释了原因：宁可留下近似重复，也不要丢掉「只是相似」的有用记忆。真的合并留给后面的 LLM 决策。

余弦相似度是手写的（点积除模长乘积，任一为零向量返回 0，长度不等返回 0），不走 numpy——这一批数据规模很小，避免为一个比较引入数组依赖。

**第三步：并发找相似。** `parallel_find_similar` 为每个存活条目构造搜索前缀：有 `root_scope` 就以它为边界，若再有显式 `scope` 就把后者拼进去；没有 root 就用显式 scope；两者都没有就传 `None`（全库搜）。搜索参数是 `limit=consolidation_limit`（默认 5）、`categories=None`（不做分类过滤）、`min_score=0.0`。

一个条目时串行，多个条目时线程池（最多 8 个 worker，每个带 `copy_context()`）。单个搜索失败只告警并把该条目当作「没有相似记录」——这直接影响后续分组：它会落进「新插入」而不是「合并」。

**第四步：并发分析，按四组分类。** `parallel_analyze` 是整条流水线最复杂的一步。它先判断「有没有条目缺字段」，若有就预取一次作用域列表与分类统计（都限制在 root 边界内），供所有字段推断调用共享——这两个列表是 LLM 的上下文提示，取一次比每条取一次便宜得多。

然后按两个布尔量把条目分成四组：

| 字段齐？ | 有相似旧记录？ | 组 | 动作 |
|---|---|---|---|
| 是 | 否 | A | 直接插入，零次 LLM |
| 是 | 是 | B | 一次合并决策 LLM |
| 否 | 否 | C | 一次字段推断 LLM |
| 否 | 是 | D | 两次 LLM 并发 |

所有 LLM 调用都提交到同一个线程池（最多 10 个 worker），**先全部提交再逐个等结果**——这是并发的关键：不是「对每个条目依次等」，而是「全部起飞后按索引收」。字段推断的结果先落地（因为它决定 `resolved_scope` / `resolved_categories` / `resolved_importance` / `resolved_metadata`），合并计划后落地。没有合并 future 的条目（A、C 组）补一个「空动作 + 插入」的计划。

`finally` 里 `pool.shutdown(wait=False)`：不等在途任务完成就返回。这是刻意的——所有 future 都已 `result()` 过，剩下的只可能是异常路径的残留。

**第五步：执行计划，先跨条目去重。** `execute_plans` 有一个不起眼但重要的前置动作：**跨条目去重动作**。多个条目的相似集合可能重叠，于是两个条目可能都想删/改同一条旧记录。代码用「先到先得」的集合/字典把动作压成一个：删除集合、更新字典（记录 id → 条目索引 + 新内容）。源码注释点明了原因——两个操作打同一条记录会让 LanceDB 提交冲突。

然后：批量重嵌入所有更新内容（一次 `embed_texts`）→ 组装待插入记录 → 执行删除 → 执行更新 → 批量插入。顺序是「删、改、插」，而不是「插、改、删」，这减少了「同一记录既被删又被插」的窗口。

更新时用旧记录做模板重建 `MemoryRecord`：保留 id、scope、categories、metadata、importance、created_at，更新 content、last_accessed（设为当前时间）、embedding（新嵌入为空则沿用旧的）。这条「重嵌入失败就留旧向量」的兜底避免了「内容更新了但检索失效」。

插入的 `MemoryRecord` 直接复用第二步算出的向量（`item.embedding if item.embedding else None`），不再重新嵌入。

最后给非插入条目补 `result_record`：优先用本次更新产出的记录，其次用相似记录里的第一条。这使 `remember()` 能对「合并进已有记录」的写入也返回一条记录，而不是 `None`。

**计数字段是唯一的批级观测。** `records_inserted` / `records_updated` / `records_deleted` / `items_dropped_dedup` 在流程状态上累加，但目前没有对外暴露的读取路径——它们只在流程内部可用（`Memory` 只从 `state.items` 里挑 `result_record`）。这是「先埋点、暂未消费」的一类代码。

## 关键取舍

**源码事实：** 批内去重阈值 0.98 极高，且只丢弃「后出现的」。这是把「宁可留重复」写进默认值，真正的合并交给 LLM 决策。代价是一批里若含大量近似重复，会有多条进入合并决策流程，LLM 调用数上去了。

**源码事实：** 四组分类的目标是「零 LLM 快路径」。调用方把字段都给齐且没有相似记录时，整条路径不产生任何 LLM 调用。这是给高频写入场景留的逃生口。

**源码事实：** 存储搜索失败被当作「没有相似记录」。这个降级会让条目走插入路径，可能产生重复；反过来若把它当失败上抛，则一条后端抖动会让整批写入失败。选择了「宁可重复」。

**源码事实：** 跨条目的动作去重是「先到先得」，不是「合并」。两个条目都想更新同一记录时，只有第一个生效，第二个的意图被丢弃（它仍会拿到那条记录作为 `result_record`）。

**源码事实：** 流程用 `suppress_flow_events=True` 且 `_skip_auto_memory=True`。内部机器不产生对外事件、不建自己的记忆。

**架构推论：** 用流程引擎表达批处理，好处是「步与步之间的依赖」由定义显式表达（`@listen` 串起五步），并且每步可以单独测。代价是这批货必须走一遍流程初始化，而流水线本身没有任何分支——`@listen` 链在语义上等价于一个顺序函数调用。这里的收益主要在于「与检索流程对称」和「复用引擎的并发能力」，不在灵活性。

**架构推论：** 所有 LLM 调用都走「先全提交、后按索引收」，而不是 `as_completed`。这让结果顺序确定（与条目顺序一致）、便于按索引回填状态，代价是「最慢的一个决定总耗时」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/memory/encoding_flow.py` | `EncodingFlow` | 五步流水线主体：批量嵌入、去重、找相似、分析、执行 |
| `lib/crewai/src/crewai/memory/encoding_flow.py` | `ItemState` | 单条目状态：输入字段、解析字段、向量、相似记录、计划、结果 |
| `lib/crewai/src/crewai/memory/encoding_flow.py` | `batch_embed` | 全批一次嵌入调用 |
| `lib/crewai/src/crewai/memory/encoding_flow.py` | `intra_batch_dedup` | 批内 O(n²) 贪心去重，高阈值只丢近似重复 |
| `lib/crewai/src/crewai/memory/encoding_flow.py` | `parallel_find_similar` | 并发搜索相似旧记录，失败降级为「无相似」 |
| `lib/crewai/src/crewai/memory/encoding_flow.py` | `parallel_analyze` | 四组分类、先全提交后按索引收、字段推断与合并决策 |
| `lib/crewai/src/crewai/memory/encoding_flow.py` | `execute_plans` | 跨条目动作去重、批量重嵌入、删/改/插顺序执行 |
| `lib/crewai/src/crewai/memory/encoding_flow.py` | `_apply_defaults` | 字段齐备时的快速填充（含 root 前缀拼接） |

## 参考

- [CrewAI 源码：encoding_flow.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/memory/encoding_flow.py)
- [CrewAI 源码：memory/analyze.py（推断与合并计划的 schema）](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/memory/analyze.py)
- [CrewAI 文档：Memory 概念](https://docs.crewai.com/en/concepts/memory)

## 与主核 / AI 的关系

这条流水线的形状与主核里的「批量写入 + 唯一约束冲突处理」非常相似：都先算键、再查冲突、再按冲突结果分派为「插入 / 更新 / 删除」，最后批量落盘。区别在冲突判定——主核用精确的键比较，这里用向量相似度加 LLM 判断。这个替换带来一个本质差异：主核的冲突处理是确定性的、可解释的；这里的合并决策由模型给出，同一批输入两次运行可能得到不同的计划。

第三步「单个作用域搜索失败当无相似」与主核里「索引损坏时的降级扫描」是同一种保守降级：宁可给出可能重复的结果，也不让整次操作失败。这类降级的共同风险是**静默**——调用方看不出「这次写入多了一条重复」。主核至少会记进错误日志或让执行计划可见，这里只有一条 warning。

对 AI 工程的实际意义在于：记忆写入的真实成本不是存储，而是嵌入调用与 LLM 判断。这套流水线的每一处省成本都是可复用的经验：批量嵌入而非逐条、用极高阈值做廉价去重、用「字段齐备 + 无相似」做零 LLM 快路径、跨条目合并动作以避免后端版本冲突、重嵌入失败时沿用旧向量。这些不是框架特性，而是踩过坑之后的工程细节。

值得注意的是流程在这里只是一个组织容器。五步之间是纯顺序依赖，没有分支、没有循环、没有事件。用 Flow 实现它的收益是与检索侧对称、可单步测试；成本是一层流程初始化开销。读这类代码时应该按「顺序流水线」理解，而不是按「event-driven 编排」理解。
