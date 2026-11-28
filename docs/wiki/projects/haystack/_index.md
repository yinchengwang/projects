---
project: haystack
page_type: overview
display_name: Haystack
repo: https://github.com/deepset-ai/haystack
license: Apache-2.0
language: Python
stars: 27082
stars_as_of: 2026-11-23
refs:
  - path: haystack/core/pipeline/base.py
    symbols: [PipelineBase, ComponentPriority]
  - path: haystack/core/component/component.py
    symbols: [Component, ComponentMeta]
  - path: haystack/core/serialization_security.py
    symbols: [allow_deserialization_module]
  - path: haystack/document_stores/types/protocol.py
    symbols: [DocumentStore]
  - path: haystack/components/agents/agent.py
    symbols: [Agent]
---
# Haystack

## 一句话职责

面向生产 RAG 与 Agent 应用的编排框架：它把组件定义成**带类型的输入/输出 socket**，管道是一张有向图，运行前做静态校验、运行期按 socket 就绪状态调度，并支持管道快照与断点续跑。

## 机制是什么

haystack 与同阶段另外两个框架的差别在**组织范式**上，这一点决定了对它该看什么。

**langchain 的核心是「可运行对象」**：一切靠 `|` 组合成表达式管道，类型信息稀薄。
**llama-index 的核心是「节点 + 索引」**：按 RAG 阶段纵向分层，每层有独立机制。
**haystack 的核心是「有类型的组件 DAG」**：组件显式声明输入输出 socket 的类型与是否必填，管道在**运行前**就能校验「这条边连得通吗」，在**运行期**按「哪些 socket 已就绪」决定下一个跑谁。

这个范式带来三样另外两个框架没有的东西：

1. **类型兼容与自动转换**。socket 连不上时不是直接报错，先看能不能按策略自动转换（例如把 `ChatMessage` 降级成字符串）。
2. **管道快照与断点续跑**。因为所有中间值都要能过序列化，管道可以在任意组件之间停下来存盘、之后恢复。这也解释了为什么连「上游没产出值」这种状态都要有一个可序列化的哨兵类。
3. **反序列化的模块白名单**。从 YAML 还原管道意味着要按字符串 import 类，这是明显的安全面，所以它有独立的 `serialization_security` 模块与一套 allowlist 机制。

本文库按这套骨架与组件类别组织：**9 个机制领域、共 35 篇机制页**。

## 怎么实现的

下表的 9 个领域按下面的顺序排列：A 组件与管道骨架 → B 类型与序列化 → C 数据模型 → D 文档存储与过滤 → E 预处理与转换 → F 检索与重排 → G 路由 → H Agent 与工具 → I 评测与可观测。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：`haystack/` 包本体（299 个 py 文件）。组件层有 18 个类别、135 个文件，其中大量是不同后端的适配（OpenAI / Azure / Mock），机制重复度高，按类别归并成机制页而不是按文件建页。`test/`、`e2e/`、`examples/`、`docs-website/` 不建页。

## 机制导航（35 篇 / 9 组）

### A-组件与管道骨架（6 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-component-contract.md`](a01-component-contract.md) | 组件契约与元类校验 |
| [`a02-pipeline-run.md`](a02-pipeline-run.md) | 管道调度与流式运行 |
| [`a03-component-checks.md`](a03-component-checks.md) | 组件就绪判定 |
| [`a04-sockets.md`](a04-sockets.md) | 输入输出 socket |
| [`a05-super-component.md`](a05-super-component.md) | 超级组件与映射包装 |
| [`a06-breakpoints.md`](a06-breakpoints.md) | 断点与管道快照 |

### B-类型与序列化（4 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-type-compatibility.md`](b01-type-compatibility.md) | 类型兼容与自动转换 |
| [`b02-serialization.md`](b02-serialization.md) | 组件序列化 |
| [`b03-deserialization-security.md`](b03-deserialization-security.md) | 反序列化的模块白名单 |
| [`b04-marshal.md`](b04-marshal.md) | YAML 编组与可调用对象序列化 |

### C-数据模型（3 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-document.md`](c01-document.md) | 文档模型与字段约束 |
| [`c02-chat-message.md`](c02-chat-message.md) | 聊天消息与内容块 |
| [`c03-streaming-and-answer.md`](c03-streaming-and-answer.md) | 流式块与回答构建 |

### D-文档存储与过滤（3 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-document-store-contract.md`](d01-document-store-contract.md) | 文档存储契约与写入策略 |
| [`d02-filter-policy.md`](d02-filter-policy.md) | 过滤表达式与策略合并 |
| [`d03-in-memory-store.md`](d03-in-memory-store.md) | 内存文档存储与 BM25 |

### E-预处理与转换（4 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-document-splitter.md`](e01-document-splitter.md) | 文档切分器 |
| [`e02-structured-splitters.md`](e02-structured-splitters.md) | 结构化与层级切分 |
| [`e03-semantic-splitter.md`](e03-semantic-splitter.md) | 基于嵌入的语义切分 |
| [`e04-converters.md`](e04-converters.md) | 格式转换器 |

### F-检索与重排（4 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-embedding-retrievers.md`](f01-embedding-retrievers.md) | 嵌入检索器 |
| [`f02-hierarchical-retrievers.md`](f02-hierarchical-retrievers.md) | 自动合并与句子窗口检索 |
| [`f03-rankers.md`](f03-rankers.md) | 重排器 |
| [`f04-joiners.md`](f04-joiners.md) | 结果合并与分支汇合 |

### G-路由（3 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-conditional-router.md`](g01-conditional-router.md) | 条件路由 |
| [`g02-metadata-routers.md`](g02-metadata-routers.md) | 元数据与文件类型路由 |
| [`g03-llm-routers.md`](g03-llm-routers.md) | LLM 决策路由 |

### H-Agent 与工具（3 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-agent.md`](h01-agent.md) | 智能体组件与状态 |
| [`h02-tool-calling.md`](h02-tool-calling.md) | 工具调用执行 |
| [`h03-tools.md`](h03-tools.md) | 工具定义与工具集 |

### I-评测与可观测（5 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-llm-evaluators.md`](i01-llm-evaluators.md) | LLM 评测器 |
| [`i02-retrieval-metrics.md`](i02-retrieval-metrics.md) | 检索指标 |
| [`i03-hooks.md`](i03-hooks.md) | 钩子 |
| [`i04-tracing-telemetry.md`](i04-tracing-telemetry.md) | 追踪与遥测 |
| [`i05-token-counters.md`](i05-token-counters.md) | token 计数与评测结果聚合 |

## 关键取舍

**源码事实：** 组件用元类（`ComponentMeta`）在**类创建期**强制契约——比对 `run` 与 `run_async` 的签名一致性、注入 pre-init 钩子。契约靠元类而不是文档。

**源码事实：** 就绪判定被拆成「输入齐」与「有触发」两个入口，由 `can_component_run` 取与。「上游明确无产出」用哨兵类 `_NoOutputProduced` 表示，注释要求用 `isinstance` 而非 `==`（被检查值可能是 DataFrame 这类 `__eq__` 不返回布尔的类型），且它必须可序列化——因为它会进管道快照。

**源码事实：** socket 类型不匹配时先尝试自动转换（`ConversionStrategy`），能转就转、不能转才报错。有严格与宽松两种兼容判定。

**源码事实：** 反序列化有独立的模块白名单：注册、import 前后的双重检查（防路径穿越与间接导入）、属性访问的点号穿越禁令、以及一个环境开关。

**源码事实：** 追踪默认是空对象（`NullTracer`/`NullSpan`），启用时才替换成真实现。

**架构推论：** 「有类型的 socket + 运行前校验」把大量错误从运行期提前到构图期。代价是组件作者必须准确声明类型，且**类型声明的表达能力成了框架的表达能力上限**（这也是它必须有自动转换策略的原因——声明不可能覆盖所有实际情况）。

**架构推论：** 支持快照/断点续跑是一条**强约束**：它要求所有中间值可序列化，于是连「没有产出」这种状态都必须有可序列化的表示。这条约束的好处是管道可以长跑、可以人工介入、可以失败重试；坏处是任何不可序列化的类型（数据库连接、文件句柄）都不能成为 socket 值。

**架构推论：** 把「从字符串 import 类」当成安全面来对待，是 haystack 与其他两个框架最实质的差别。llama-index 与 langchain 也有反序列化，但 haystack 是唯一把 allowlist 做成独立模块并写进贡献指南的（「不要在库代码里放宽 allowlist」）。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `haystack/core/component/component.py` | `Component` | 组件契约与元类校验 |
| `haystack/core/pipeline/base.py` | `PipelineBase` | 管道图与调度循环 |
| `haystack/core/pipeline/component_checks.py` | `can_component_run` | 就绪判定 |
| `haystack/core/component/types.py` | `InputSocket` | socket 类型与必填/可变标记 |
| `haystack/core/type_utils.py` | `ConversionStrategy` | 类型兼容与自动转换 |
| `haystack/core/serialization.py` | `component_to_dict` | 组件序列化约定 |
| `haystack/core/serialization_security.py` | `allow_deserialization_module` | 反序列化模块白名单 |
| `haystack/dataclasses/breakpoints.py` | `PipelineSnapshot` | 管道快照结构 |
| `haystack/dataclasses/chat_message.py` | `ChatMessage` | 聊天消息与内容块 |
| `haystack/document_stores/types/filter_policy.py` | `FilterPolicy` | 过滤器合并规则 |
| `haystack/components/retrievers/auto_merging_retriever.py` | `AutoMergingRetriever` | 小块检索、大块返回 |
| `haystack/components/agents/agent.py` | `Agent` | 有状态的 Agent 组件 |

## 参考

- [haystack 源码仓库](https://github.com/deepset-ai/haystack)
- [haystack 官方文档](https://docs.haystack.deepset.ai/)
- [haystack 文档：管道与组件](https://docs.haystack.deepset.ai/docs/pipelines)
- [haystack 文档：Agent](https://docs.haystack.deepset.ai/docs/agents)
- [haystack 文档：文档存储](https://docs.haystack.deepset.ai/docs/document-store)

## 与主核 / AI 的关系

haystack 的骨架与主核（多模态数据库内核）的**查询执行器**是同一类系统的两种实现，对照最有价值。

**推送式 vs 拉取式调度。** 主核走拉取式：从根算子向下要数据，算子在被要时才执行，没有显式的「就绪判定」。haystack 走推送式：上游算完把值推进下游 socket，然后判断下游是否就绪。所以 haystack 必须写一整套 `component_checks`，而数据库不需要——**调度方向决定了要不要显式的就绪语义**。

**「无产出」的表示。** 数据库里对应的是 NULL 与「列不存在」的区分（SQL 三值逻辑）。haystack 用哨兵对象表达「上游没给这个 socket」，动机完全一致：必须能区分「没有值」与「值是空」。差别是 SQL 做进了类型系统，haystack 用普通类加 `isinstance` 约定。

**管道快照 vs 事务与检查点。** 数据库用 WAL + checkpoint 让系统可以在崩溃后恢复；haystack 用管道快照让一张图可以在任意组件之间暂停与恢复。两者都在解决「长跑任务如何中断重来」，但一致性要求不同：数据库要保证事务原子性与持久性，haystack 只要求中间值可序列化。**这条约束的强弱差异，正是「通用编排框架」与「数据库」的分野。**

**反序列化白名单 vs 类型注册表。** 主核从磁盘读元组时不需要担心「类名被篡改」，因为它的类型系统是封闭的；haystack 从 YAML 还原任意组件，类名来自外部输入，所以必须做 allowlist。这条对照说明：**当配置本身是不可信输入时，反序列化就从「读数据」变成了「执行代码」**，防护要求随之改变。

**类型兼容与自动转换**在主核里对应的是隐式类型转换（如 `int` 与 `numeric` 比较时的提升规则）。两者都在处理「声明类型与实际值不完全匹配」，但数据库的转换规则由 SQL 标准固定，haystack 的转换策略由框架自定义——因为 agent 应用的类型边界本就模糊（`ChatMessage` 与 `str` 都能表示一段文本）。
