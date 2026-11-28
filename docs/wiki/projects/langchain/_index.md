---
project: langchain
page_type: overview
display_name: LangChain
repo: https://github.com/langchain-ai/langchain
license: MIT
language: Python
stars: 149988
stars_as_of: 2026-11-23
refs:
  - path: libs/core/langchain_core/runnables/base.py
    symbols: [Runnable, RunnableSequence]
  - path: libs/core/langchain_core/indexing/api.py
    symbols: [index, IndexingResult]
  - path: libs/core/langchain_core/messages/content.py
    symbols: [TextContentBlock, ToolCall]
  - path: libs/text-splitters/langchain_text_splitters/character.py
    symbols: [RecursiveCharacterTextSplitter]
  - path: libs/core/langchain_core/vectorstores/base.py
    symbols: [VectorStore]
  - path: libs/core/langchain_core/_security/_policy.py
    symbols: [SSRFPolicy]
---
# LangChain

## 一句话职责

LLM 应用的组合框架：把模型、提示、解析器、检索器、工具、向量库都抽象成同一种可组合的「可运行对象」，让一条 RAG 管道从「模型调用」到「文档落库」都在同一套表达式语言里表达。

## 机制是什么

这个项目最值得看的是**一层抽象**：`Runnable`。它把异质的组件统一成同一个接口——模型、提示模板、输出解析器、文档加载器、向量库检索、甚至一个普通函数，都实现 `invoke` / `batch` / `stream` 与对应的异步版本。于是「管道」就是 `|` 运算，中间不需要为每种组合写专门的适配代码。

这层抽象带来三个系统性后果：

1. **组合即表达**：`prompt | model | parser` 是一条 RunnableSequence，`chain.batch([...])` 是批量，`chain.stream(...)` 是流式。序列、并行、分支、回退、重试、逐项映射都是 `Runnable` 的不同子类，而非各写一套。
2. **配置沿链传播**：`RunnableConfig` 携带回调、标签、并发上限、递归上限等，随每次调用向下传递。这就是为什么一次调用能在所有层级被追踪。
3. **身份由内容决定**：文档索引去重不靠主键，靠内容与元数据的哈希；于是重跑幂等，但元数据变化会被当成新文档。

RAG 侧的机制集中在两处：`libs/core/langchain_core/` 的**索引与向量存储抽象**，以及 `libs/text-splitters/` 的**分块算法**。后者是 ingestion 的入口——分块方式直接决定检索质量，而它比看起来复杂得多（递归分隔符、结构感知、长度度量单位）。

本仓是 monorepo，`libs/partners/` 下是各家模型与向量库的具体适配，机制重复度高，不建页。

本文库按经审阅的机制单元组织：**9 个机制领域、共 37 篇机制页**。

## 怎么实现的

下表的 9 个领域按下面的顺序排列：A 表达式语言 → B 消息与内容 → C 提示与解析 → D 工具与函数调用 → E 回调与追踪 → F 文档分块 → G 索引与向量存储 → H 模型层 → I 序列化与安全。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：`libs/core/langchain_core/`（抽象与核心机制）与 `libs/text-splitters/langchain_text_splitters/`（分块算法）。`libs/langchain`、`libs/langchain_v1`、`libs/partners`、`libs/standard-tests` 属于实现层与适配层，不建页。

## 机制导航（37 篇 / 9 组）

### A-表达式语言（8 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-runnable-core.md`](a01-runnable-core.md) | Runnable 契约与序列组合 |
| [`a02-parallel-and-each.md`](a02-parallel-and-each.md) | 并行组合与逐项映射 |
| [`a03-branch-and-router.md`](a03-branch-and-router.md) | 分支、路由与回退 |
| [`a04-retry.md`](a04-retry.md) | 重试与退避 |
| [`a05-config-and-context.md`](a05-config-and-context.md) | 运行配置与上下文传播 |
| [`a06-passthrough-and-assign.md`](a06-passthrough-and-assign.md) | 透传、赋值与字段选取 |
| [`a07-message-history.md`](a07-message-history.md) | 会话历史绑定 |
| [`a08-runnable-graph.md`](a08-runnable-graph.md) | 组合图与可视化 |

### B-消息与内容（3 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-message-model.md`](b01-message-model.md) | 消息模型与角色 |
| [`b02-content-blocks.md`](b02-content-blocks.md) | 内容块与工具调用块 |
| [`b03-usage-metadata.md`](b03-usage-metadata.md) | 用量元数据 |

### C-提示与解析（3 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-prompt-template.md`](c01-prompt-template.md) | 提示模板与消息占位 |
| [`c02-output-parsing.md`](c02-output-parsing.md) | 输出解析 |
| [`c03-example-selector.md`](c03-example-selector.md) | 示例选择 |

### D-工具与函数调用（4 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-tool-contract.md`](d01-tool-contract.md) | 工具契约与参数注入 |
| [`d02-tool-conversion.md`](d02-tool-conversion.md) | 函数转工具与 schema 生成 |
| [`d03-structured-tool.md`](d03-structured-tool.md) | 结构化工具与结果 |
| [`d04-retriever-tool.md`](d04-retriever-tool.md) | 检索器与渲染 |

### E-回调与追踪（3 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-callback-manager.md`](e01-callback-manager.md) | 回调管理器与运行管理器 |
| [`e02-callback-handlers.md`](e02-callback-handlers.md) | 内置处理器与流式输出 |
| [`e03-tracers.md`](e03-tracers.md) | 追踪器与事件流 |

### F-文档分块（4 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-splitter-base.md`](f01-splitter-base.md) | 分块器基类与长度度量 |
| [`f02-character-splitter.md`](f02-character-splitter.md) | 字符与递归字符分块 |
| [`f03-markup-splitter.md`](f03-markup-splitter.md) | 标记语言与结构化文档分块 |
| [`f04-semantic-splitter.md`](f04-semantic-splitter.md) | 语义与句法分块 |

### G-索引与向量存储（6 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-indexing-api.md`](g01-indexing-api.md) | 文档索引与去重清理 |
| [`g02-record-manager.md`](g02-record-manager.md) | 记录管理器 |
| [`g03-vectorstore-contract.md`](g03-vectorstore-contract.md) | 向量存储契约与检索器 |
| [`g04-in-memory-stores.md`](g04-in-memory-stores.md) | 内存向量存储与检索实现 |
| [`g05-document-transformers.md`](g05-document-transformers.md) | 文档压缩与嵌入 |
| [`g06-memory-and-loaders.md`](g06-memory-and-loaders.md) | 会话内存与文档加载 |

### H-模型层（4 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-chat-model-contract.md`](h01-chat-model-contract.md) | 聊天模型契约 |
| [`h02-stream-projection.md`](h02-stream-projection.md) | 流式投影与增量合成 |
| [`h03-functions-and-tools-binding.md`](h03-functions-and-tools-binding.md) | 函数调用格式与工具绑定 |
| [`h04-model-profile.md`](h04-model-profile.md) | 模型档案与能力声明 |

### I-序列化与安全（2 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-serialization.md`](i01-serialization.md) | 可序列化基类与反序列化 |
| [`i02-ssrf-policy.md`](i02-ssrf-policy.md) | 出站请求的 SSRF 防护 |

## 关键取舍

**源码事实：** `Runnable` 是唯一抽象，序列/并行/分支/回退/重试/逐项映射都是它的子类（`runnables/base.py` 一个文件 5698 行）。这是「用一个接口吸收所有组合形态」的选择。

**源码事实：** 文档身份由「内容哈希 + 元数据哈希」决定（元数据用 `sort_keys=True` 序列化），默认 SHA-1 会发一次性警告提示不抗碰撞，换编码器时必须同时换索引。

**源码事实：** 索引的清理模式有四个取值（含 `None`），`incremental` 强制要求 `source_id_key`。「哪些文档该删」无法从当前批次推断，只能靠记录管理器保存的上一轮状态。

**源码事实：** 分块的核心文件在独立分发包 `langchain-text-splitters` 里，而不在 core。`RecursiveCharacterTextSplitter` 按分隔符优先级递归下降，`MarkdownHeaderTextSplitter` 会把标题层级写进 metadata。

**源码事实：** 出站 HTTP 请求有独立的安全模块（`_security/`），策略与传输层两级：策略判定 URL 与解析后的 IP，传输层包装 HTTP 客户端。

**架构推论：** 「一种接口吸收所有组合」的代价是类型信息稀薄。`Runnable[Input, Output]` 只有输入输出两个类型参数，中间步骤的形态（dict 还是消息列表）靠约定，因此链改一处经常要在别处跟着调整。收益是任何组件都能插进任何位置，不需要为组合写适配器。

**架构推论：** 「内容哈希做文档身份」把去重从「用户声明主键」变成「框架推导身份」。这让索引幂等，但也意味着**元数据是身份的一部分**——抓取时间戳这类字段会让每次重跑都产生新文档。

**架构推论：** 分块算法独立成包，说明它被当作**可独立替换的算法层**而不是框架内部实现。这也是 RAG 工程里最常被单独调优的环节。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `libs/core/langchain_core/runnables/base.py` | `Runnable` | 统一的可运行抽象 |
| `libs/core/langchain_core/runnables/base.py` | `RunnableSequence` | 序列组合 |
| `libs/core/langchain_core/runnables/config.py` | `RunnableConfig` | 沿链传播的调用配置 |
| `libs/core/langchain_core/runnables/graph.py` | `Graph` | 组合结构导出成图 |
| `libs/core/langchain_core/messages/content.py` | `ToolCall` | 多模态内容块与工具调用块 |
| `libs/core/langchain_core/indexing/api.py` | `index` | 索引主流程与清理模式 |
| `libs/core/langchain_core/indexing/base.py` | `RecordManager` | 上一轮文档记录的契约 |
| `libs/core/langchain_core/vectorstores/base.py` | `VectorStore` | 向量存储契约与检索器 |
| `libs/text-splitters/langchain_text_splitters/character.py` | `RecursiveCharacterTextSplitter` | 递归字符分块 |
| `libs/text-splitters/langchain_text_splitters/markdown.py` | `MarkdownHeaderTextSplitter` | 按结构分块并保留元数据 |
| `libs/core/langchain_core/_security/_policy.py` | `SSRFPolicy` | 出站请求策略 |
| `libs/core/langchain_core/load/serializable.py` | `Serializable` | 可序列化基类 |

## 参考

- [langchain 源码仓库](https://github.com/langchain-ai/langchain)
- [langchain 文档](https://python.langchain.com/)
- [langchain 文档：索引与去重](https://python.langchain.com/docs/how_to/indexing/)
- [langchain 文档：Runnable 接口](https://python.langchain.com/docs/concepts/runnables/)
- [langchain 文档：检索增强生成](https://python.langchain.com/docs/concepts/rag/)

## 与主核 / AI 的关系

langchain 与主核（多模态数据库内核）的交集集中在 RAG 的下半段：**文档如何落库、如何去重、如何分块**。这三件事在主核里都有对应物，但机制不同。

**去重：内容哈希 vs 主键。** 主核用显式主键表达身份，插入时按主键判冲突；langchain 用内容哈希推导身份。差别在「谁承担去重责任」——主核让用户声明，索引让框架推导。后果是：索引幂等重跑，但元数据变化等于新文档；主核改非主键列不换身份。这条对照说明**身份的来源决定了去重的语义**，没有普适答案。

**清理需要外部状态。** 「哪些文档该删」只能靠记录管理器保存的上一轮状态。主核里删除靠显式 `DELETE`，不需要历史。这揭示了增量索引的复杂度来源：它必须持久化一份框架层状态，而这个状态本身可能失效。主核的 MVCC 与索引维护在处理同类问题时，把状态放在数据库内部由事务保证一致性——这是「状态放哪里」的分野。

**分块是 RAG 独有的阶段。** 数据库没有「文档切块」这个概念，因为它的输入是结构化元组。RAG 必须切块，且切法直接决定检索质量：递归分隔符、结构感知（Markdown 标题进 metadata）、长度单位（字符还是 token）。这一层在主核里没有对应物，是 LLM 应用特有的工程问题。

**组合抽象 vs 查询计划。** `Runnable` 用一层接口吸收所有组合形态，与主核的「一切查询都编译成算子树」在目标上一致：让组合不需要专门的适配代码。差别是主核的算子树携带完整类型与代价信息（可优化），`Runnable` 的类型信息稀薄（不可优化）。这是「为执行而抽象」与「为表达而抽象」的分野。
