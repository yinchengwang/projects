---
project: crewai
feature: recall-flow
title: 记忆检索流程
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/memory/recall_flow.py
    symbols: [RecallFlow, analyze_query_step, recursive_exploration]
---

# 记忆检索流程

## 一句话职责

`RecallFlow` 是一个内部流程，把「一次记忆检索」写成六个步骤加两个路由判定：蒸馏查询 → 选作用域 → 并行搜 → 看置信度 → 可选深探一轮 → 合成结果，用流程引擎的分支能力换到「按需加深」的自适应检索。

## 机制是什么

检索被拆成一张有环的小图：

- `analyze_query_step`（入口）把查询变成若干条子查询并批量嵌入；
- `filter_and_chunk` 选候选作用域；
- `search_chunks` 在「子查询 × 作用域」的笛卡尔积上并行搜索；
- `decide_depth`（路由）按置信度、复杂度与剩余预算决定去 `explore_deeper` 还是 `synthesize`；
- `recursive_exploration` 把每一批命中的前几条内容喂回 LLM，提取「与本查询相关的部分」，并记录「缺什么」；
- `re_search` 重新搜索以更新置信度，再由 `re_decide_depth` 复判（逻辑与 `decide_depth` 相同）；
- `synthesize_results` 去重、复合打分、排序、截断，把证据缺口挂到第一条结果上。

流程状态 `RecallState` 是这条流水线的全部上下文：查询与过滤条件（scope / categories / limit / source / include_private）、时间下界、查询嵌入列表、分析结果、候选作用域、逐作用域命中、证据缺口、置信度、最终结果、剩余探索预算。

## 怎么实现的

**构造即配置。** 流程在 `__init__` 里收后端、LLM、嵌入器与 `MemoryConfig`，然后调 `super().__init__(suppress_flow_events=True)`——它是内部流程，不往外发流程事件（事件由外层 `Memory` 统一发，见 e01）。同时声明 `is_crewai_internal = True` 与 `_skip_auto_memory = True`：前者让钩子与执行边界判定把它当内部机器，后者避免「检索流程自己又建一个记忆」的递归浪费。

**第一步：短查询跳过 LLM。** `analyze_query_step` 先看查询长度是否低于 `query_analysis_threshold`。低于就直接构造一个「简单查询」的分析结果——关键词空、建议作用域空、复杂度 simple、子查询就是原查询本身。这是明确的性能取舍，源码注释给了量级：省 1~3 秒。高于阈值才真调 LLM，并且把当前作用下可用的作用域列表（以及当前作用域的信息）一起给它做参考。LLM 返回的 `time_filter` 若存在，用 `datetime.fromisoformat` 解析，解析失败就静默丢弃——不因为时间提示格式不对而失败整次检索。

**嵌入是批量的，且有兜底。** 子查询取前 3 条（多于 3 条没有额外收益，只是多花嵌入成本），用 `embed_texts` 一次性嵌入。若全部嵌入结果为空（例如嵌入器对空文本返回空向量），就退回用原始查询再嵌一次；仍为空则 `query_embeddings` 为空，后续搜索会因任务数为零而得到零结果——流程不会崩，只是返回空列表。

**第二步：作用域选择。** `filter_and_chunk` 优先用 LLM 建议的作用域；没有建议就调后端 `list_scopes` 取当前前缀下的子树。两者都失败或为空时退回「用当前前缀自己」。最后截断到 20 个。截断是必要的：搜索成本是子查询数 × 作用域数，作用域一多就会打爆。

**第三步：并行搜索。** `_do_search` 把「(子查询嵌入, 作用域)」的每个组合做成一个搜索任务。只有一个任务时串行跑；多个任务时开线程池（最多 4 个 worker），每个任务用 `contextvars.copy_context()` 包一层再提交——这保证了上下文变量（例如追踪、事件作用域）在搜索线程里可见。单个任务失败只记 warning 并跳过，不影响其他作用域。

每个任务的搜索参数是：`limit = 请求数 × 过采样倍数`（默认 2 倍）、`min_score=0.0`（先都要，后面统一打分）。搜到结果后再过两道过滤：时间下界（`created_at >= time_cutoff`）与私密过滤。私密过滤的条件与浅检索保持一字不差（见 e01），源码注释特意指出这一点：两处必须在「`source` 为 `None` 的私密记录不得泄漏给默认调用方」上一致。

置信度取所有任务里**最高**的复合分（没有命中就是 0）。注意这里的 `top_score` 用的是复合分而不是原始语义相似度，所以置信度天然含了时间与重要度的贡献。

**第四步：路由判定的三步优先序。** `decide_depth` 的顺序值得照抄：

1. 若 LLM 判为 `complex` 且置信度低于 `complex_query_threshold`（默认 0.7），且预算还有——深探；
2. 否则若置信度已达 `confidence_threshold_high`（0.8）——直接合成；
3. 否则若预算还有且置信度低于 `confidence_threshold_low`（0.5）——深探；
4. 其余情况——合成。

也就是「复杂查询的阈值」比「普通查询的低阈值」更严，且复杂查询先看，避免被「已经够高了」的短路径短路。第 2 步穿插的位置很关键：如果某个复杂查询碰巧拿到 0.9 的置信度，会走合成而不是深探。

**第五步：深探。** `recursive_exploration` 先**扣预算**（这是唯一的终止保证），然后对每个作用域的命中取前 5 条内容拼成文本，连同原查询一起交给 LLM，要求「提取与查询最相关的信息，若缺什么用一行说明缺什么」。若回复里含 `missing`，把前 200 字符记进 `evidence_gaps`。LLM 调用失败时该作用域仍保留命中，只是 `extraction` 为空——不让一次 LLM 失败毁掉已有结果。这里显式放行 `HookAborted`（钩子中止必须上抛）。

深探后 `re_search` 重新搜一遍，目的不是改变查询，而是**用新的（更丰富的）上下文刷新置信度**——注意 `_do_search` 本身不消费 `extraction`，所以这一轮搜索的结果与上一轮是同一批。这个设计有点反直觉：深探的产出（`extraction`）只影响证据缺口与可观测输出，对最终排序没有直接贡献。真正参与排序的是每轮搜索结果里的 (记录, 原始分数)。

**第六步：合成。** `synthesize_results` 遍历全部作用域的命中，逐条从 `(记录, 原始分)` 里取出记录、按 id 去重、用复合分打分，再整体降序排、截断到 `limit`，把 `evidence_gaps` 挂到第一条结果上（`MemoryMatch.evidence_gaps` 字段）。遍历时对形态做了防御：`finding` 必须是字典、`results` 必须是列表、每一项必须是长度 ≥ 2 的序列、记录必须是 `MemoryRecord`——任何不合形态的项静默跳过。这套防御是为了兼容「深探前后的 findings 形态不同」这个事实。

**同一个阈值在两处默认值不同。** `query_analysis_threshold` 在 `Memory` 字段上默认 200，在 `MemoryConfig` 上默认 250。`analyze_query_step` 读的是 `self._config`，而 `Memory` 在初始化时会用自己字段的值构造 `MemoryConfig` 传进来——所以经 `Memory.recall` 走时实际生效的是 200；只有直接构造 `RecallFlow(... config=None)` 时才会落到 `MemoryConfig()` 的 250。这类「字段默认值与内部配置默认值各写一遍」的细节，是读这套代码时最容易误判实际行为的地方。

## 关键取舍

**源码事实：** 终止靠预算递减，不是靠置信度收敛。`recursive_exploration` 无条件扣预算，`decide_depth` 只在预算 > 0 时才路由到深探，所以最多深探 `exploration_budget` 轮（默认 1）。

**源码事实：** 短查询完全跳过 LLM。这条路径下 `time_filter` 永远不会被提取，也就是说「上周」这类时间词在短查询里不起作用——除非查询本身超过阈值。

**源码事实：** 深探的 `re_search` 是同一批查询、同一批作用域的重复搜索。它更像「为路由判定刷新一次读数」，而不是「用新线索重搜」。源码没有把它写成「基于 extraction 生成新查询」，这是一个保守选择。

**源码事实：** 置信度是「最高复合分」而不是「命中数量」或「分数分布」。单条高置信命中就能让流程走合成短路。

**源码事实：** 单个作用域搜索失败只告警跳过。这使「某个作用域的后端查询异常」不会让整次检索失败，但也会让结果静默变少——没有任何标记告诉调用方「有空缺」。

**架构推论：** 用流程引擎实现检索，直接拿到了三样东西：路由（条件分支）、事件抑制（内部流程不外发事件）、以及每个步骤的独立可测试性。代价是「一次检索」变成了一次 `kickoff`，包含状态初始化、方法绑定与事件作用域管理的开销，对高频检索是可见的成本。

**架构推论：** 作用域截断到 20、子查询截断到 3、每作用域取前 5 条做深探——三个魔法数都在「够用」和「别爆炸」之间选了保守值。它们没有被提到配置层，说明这被视为实现细节而非调优旋钮。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/memory/recall_flow.py` | `RecallFlow` | 检索流程主体：六个步骤 + 两个路由判定 |
| `lib/crewai/src/crewai/memory/recall_flow.py` | `RecallState` | 检索全程的状态载体 |
| `lib/crewai/src/crewai/memory/recall_flow.py` | `analyze_query_step` | 入口：短查询跳 LLM、蒸馏子查询、批量嵌入、提取时间下界 |
| `lib/crewai/src/crewai/memory/recall_flow.py` | `_do_search` | 子查询 × 作用域的线程池并行搜索，含时间与私密过滤 |
| `lib/crewai/src/crewai/memory/recall_flow.py` | `decide_depth` | 四步优先序的深度路由 |
| `lib/crewai/src/crewai/memory/recall_flow.py` | `recursive_exploration` | 扣预算、LLM 提取、记录证据缺口 |
| `lib/crewai/src/crewai/memory/recall_flow.py` | `synthesize_results` | 去重、复合打分、排序截断、挂证据缺口 |

## 参考

- [CrewAI 源码：recall_flow.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/memory/recall_flow.py)
- [CrewAI 文档：Memory 概念](https://docs.crewai.com/en/concepts/memory)
- [CrewAI 源码：memory/types.py（复合打分与配置项）](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/memory/types.py)

## 与主核 / AI 的关系

这是「自适应查询」这个想法在一个极小的规模上的实现。它与主核里的代价估算器是同一种角色：都是先花一点代价估计「当前计划够不够」，不够就换更贵的计划。差别在主核的估算是静态的（基于统计信息），这里的估算要真的跑一次检索才知道置信度——所以它是「先执行再重规划」，也就是自适应查询处理的经典形态。

与主核的另一处对照是作用域的并行扫描：`_do_search` 对每个作用域起一个任务并发查，失败单个不影响其余，这与分区表并行扫描的容错策略一致。区别在主核会记录「某分区扫描失败」并让计划可见，这里只发一条 warning 日志，调用方看不到。

值得警惕的是 `re_search` 的语义：它在深探之后原样重搜，除了刷新置信度没有别的产出。这个设计使得「深探轮」的收益完全取决于 LLM 提取是否影响置信度——而置信度只看搜索分数，提取文本不参与。所以在默认预算 1 的情况下，深探主要产出的是 `evidence_gaps` 这个诊断信息，而不是更好的排序。理解这一点对调参很重要：想通过调大预算改善结果质量，预期收益可能低于预期。

对 AI 工程的实际意义在于：检索质量与延迟的权衡不能靠一个固定 `top_k` 解决。这套实现的思路是把「要不要更努力」做成显式可观测的决策（置信度、复杂度、预算），并在结果上附带「缺什么」的证据缺口——后者对下游的 agent 尤其有用，它让「查不到」变成一条可消费的信息，而不是空列表。
