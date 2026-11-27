---
project: crewai
feature: knowledge
title: 知识库与来源管理
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/knowledge/knowledge.py
    symbols: [Knowledge, query, add_sources]
---

# 知识库与来源管理

## 一句话职责

`Knowledge` 把一个向量集合与若干「来源」绑在一起：来源负责把自己的内容切块并写进集合，`Knowledge` 负责集合的创建、检索与重置——也就是「知识入库」与「知识检索」两条腿的接合点。

## 机制是什么

`Knowledge` 有三个可替换部件：

- `sources`：来源列表，每个来源是一个 `BaseKnowledgeSource` 子类实例，自带「怎么读内容」与「怎么切块」的能力；
- `storage`：存储后端，默认 `KnowledgeStorage`（一层薄封装，把检索与写入转发给 RAG 客户端）；
- `embedder`：嵌入器，可以是 provider 配置字典、`BaseEmbeddingsProvider` 实例、或 `None`（表示用当前 RAG 配置里已经装好的嵌入器）。

它们的关系是**单向**的：来源写进存储，存储不做内容理解；检索只问存储，不回头问来源。`Knowledge` 自己不存向量、不做切块、不算相似度。

来源类型由一张固定表登记，目前七种：`string`、`docling`、`csv`、`excel`、`json`、`pdf`、`text_file`。这张表的作用是让「字典形态的来源声明」能被还原成有类型的对象——用于配置与检查点。

## 怎么实现的

**构造期的两件大事。** `Knowledge.__init__` 的签名要求 `collection_name` 与 `sources` 必填（尽管字段上都有默认值，构造器把它们变成必需参数，这是一处刻意的收紧）。然后：

1. 定存储：用户给了 `storage` 就用它；否则先问登记过的存储工厂（`resolve_knowledge_storage(embedder, collection_name)`），工厂返回 `None` 就退回内置 `KnowledgeStorage(embedder=..., collection_name=...)`。这条链与记忆系统的存储解析是同一套「工厂优先、内置兜底」的模式。
2. 定来源：把 `sources` 赋给字段。但赋值会给字段的 `BeforeValidator` 再跑一遍——这就是字典来源被还原成类型化来源的时机。

**来源的还原。** `_resolve_knowledge_sources` 遍历列表：非字典项原样保留（实例、mock 都通过）；字典项看 `source_type` 键，缺失就原样保留（交给 Pydantic 后续判断），有值就查那张七项表，查不到抛错并列出全部合法值，找到就 `cls.model_validate(item)`，校验失败也包一层错误、带上索引与 `source_type`。这个函数的价值在于错误定位：一份含 10 个来源的配置里第 7 个写错，报错会直接说是第 7 个、是哪个类型。

**集合名的约定藏在存储层。** `KnowledgeStorage` 里的集合名规则是：有 `collection_name` 就拼成 `knowledge_<名字>`，没有就是 `knowledge`。所以「多个知识库共用一个 RAG 客户端」靠的就是不同集合名，而 `Knowledge` 只负责把名字传下去。这条前缀约定是必要的：知识库与记忆、与用户自己的集合都共用同一个后端时，前缀是唯一的隔离手段。

**`add_sources` 的两步舞。** 它的实现短得出奇：遍历每个来源，把 `storage` 赋给来源，然后调 `source.add()`。为什么必须先赋值？因为来源对象不知道自己该往哪写——`storage` 字段默认是 `None`，来源的 `_save_documents()` 在它为 `None` 时直接抛 `ValueError("No storage found to save documents.")`。所以「把存储注入来源」是这一步的全部意义，真正的切块与写入发生在 `source.add()` 里。

来源内部的动作（以字符串来源与文件来源为例）：`_chunk_text` 按 `chunk_size`（默认 4000）切、相邻块重叠 `chunk_overlap`（默认 200），切法的终止条件写成 `max(len(text) - overlap, 1)`，源码注释解释了原因——避免最后一块只是前一块的尾巴；文件类来源在构造后立即处理路径（把字符串路径拼到知识目录下、展开 `file_path` 这个已弃用字段、校验文件存在）并 `load_content()` 把内容读成 `{路径: 文本}`。也就是说，来源在**构造时就已经读过文件**了，`add()` 只是切块与写库。

`aadd_sources` 是同一逻辑的异步版，用 `await source.aadd()`。

**`query` 只有转发。** `query(queries, results_limit=5, score_threshold=0.6)` 检查 `storage` 非空后调 `storage.search(...)`。注意参数名是复数 `query`，因为可以传多个查询串；存储层把多个查询串**用空格拼成一个**再检索（不是分别检索再合并）——这是一个容易误解的细节：多查询字符串在这里等价于一条更长的查询，而不是多路召回。

`aquery` 与 `areset` / `reset` 分别对应异步检索、异步重置、同步重置。重置的语义是**删集合**而非清空记录。

**嵌入器的序列化被特别处理。** `embedder` 字段带一个 `PlainSerializer`：`None` 直通、字典直通、实例 `model_dump(mode="json")`；传**类**（而不是实例）会抛 `TypeError`，报错文案解释「`build_embedder` 需要实例或配置字典，不能是类」，并提示先实例化；其他类型也抛 `TypeError`。这条限制很实在：传类看起来像是「用这个类型的默认配置」，实际会到建嵌入器时才炸，所以在序列化边界提前拦下。

还有一处细节：模块导入时就把 `TOKENIZERS_PARALLELISM` 环境变量设为 `"false"`。这是为了避免某些嵌入/分词库在 fork 多进程时因为并行分词而死锁——一个全局副作用，写在模块顶层，任何导入 `Knowledge` 的进程都会受影响。

## 关键取舍

**源码事实：** `Knowledge` 不理解内容。切块、读取、格式解析全在来源里；检索、写入全在存储里。它只做「把两者接起来 + 管理集合名 + 提供重置」。这是一个刻意做薄的类。

**源码事实：** 存储注入是显式的两步（先赋 `storage`，再 `add()`）。这暴露了一条隐含契约：来源必须被「配置」过才能工作。好处是来源可以被复用、可以被单独测试；代价是任何绕过 `add_sources` 直接调 `source.add()` 的代码都会因为 `storage=None` 而失败。

**源码事实：** 多个查询串在存储层被拼成一个。所以 `query(["a", "b"])` 不等于「a 的结果 + b 的结果」，而等于「搜 `"a b"`」。这是一处与直觉不同的行为，写调用方时要注意。

**源码事实：** 嵌入器传类会立刻报错，且报错文案指向真实原因。这是把一类「延迟到运行期才炸」的配置错误提前到赋值/序列化期。

**源码事实：** 模块级设置 `TOKENIZERS_PARALLELISM=false`。这是为并发场景做的全局妥协，代价是所有分词都串行——对导入这个模块的整个进程生效。

**架构推论：** 把「来源类型」做成一张显式登记表（而不是按模块路径动态导入），使配置里的 `source_type` 是封闭集合、错误能立即报出。代价是新增来源类型必须改这张表，第三方来源无法通过配置注册（只能以实例形式传入）。

**架构推论：** 重置做的是「删集合」而不是「删记录」。这让重置在大集合上很快，也顺带清掉了嵌入维度等集合级元数据——这正是处理「换嵌入模型导致维度不匹配」的正确手段，存储层在写入失败时给出的报错文案就建议用户重置集合。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/knowledge/knowledge.py` | `Knowledge` | 知识库门面：存储与来源的接合、集合名传递、重置 |
| `lib/crewai/src/crewai/knowledge/knowledge.py` | `query` | 转发到存储检索；多查询串在存储层合并为一条 |
| `lib/crewai/src/crewai/knowledge/knowledge.py` | `add_sources` | 把存储注入每个来源，再让来源切块写库 |
| `lib/crewai/src/crewai/knowledge/knowledge.py` | `_resolve_knowledge_sources` | 把字典来源按 `source_type` 还原成类型化对象，错误带索引 |
| `lib/crewai/src/crewai/knowledge/knowledge.py` | `_serialize_embedder_spec` | 嵌入器序列化约束：实例或配置字典，禁止传类 |
| `lib/crewai/src/crewai/knowledge/storage/knowledge_storage.py` | `KnowledgeStorage` | 存储实现：`knowledge_<名>` 集合约定、检索与写入转发 |
| `lib/crewai/src/crewai/knowledge/source/base_knowledge_source.py` | `BaseKnowledgeSource` | 来源基类：切块规则与「无存储则报错」的保存入口 |

## 参考

- [CrewAI 源码：knowledge.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/knowledge/knowledge.py)
- [CrewAI 文档：Knowledge 概念](https://docs.crewai.com/en/concepts/knowledge)
- [CrewAI 源码：knowledge 存储层](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/knowledge/storage/knowledge_storage.py)

## 与主核 / AI 的关系

这套「切块-入库-检索」的结构，对应主核里的「表扫描 + 二级索引」被替换成了「来源解析 + 向量索引」。两者都把「数据从哪来」与「怎么查」分开：主核的装载器把外部数据写进堆表，查询走索引；这里来源把外部文件变成块写进集合，查询走向量检索。`Knowledge` 与主核里的表对象地位相仿——都知道自己用哪个索引结构、都知道集合名（表名），但都不理解内容。

一个值得对照的差别是**更新语义**。主核的更新是行级的、可事务化的；这里只有「追加块」与「删整个集合」两种操作，没有「修改某个块」。所以一条知识点变了，正确做法是重置集合重新灌，而不是增量改。这个粗粒度不是疏忽，而是向量索引的固有代价：改一块意味着重算嵌入、可能改变集合维度元数据，事务化提交的成本远高于收益。`chunk_size` / `chunk_overlap` 作为切块参数也印证了这一点——切块策略一旦定下，改它就得重新灌库。

对 AI 工程的实际意义在于：知识库的工程难点几乎全在边界上，而不是检索算法。这份实现的四个边界各占一段代码：来源类型必须封闭可校验（错误的来源在构造期就报错）、存储必须注入后才可用（避免来源悄悄写不进去）、嵌入器不能传类（避免延迟爆炸）、重置用删集合（顺带解决维度漂移）。这些都是「用起来才踩到」的坑，写进代码后的形态都很朴素。
