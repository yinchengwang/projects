---
project: metagpt
feature: simple-engine
title: SimpleEngine 与检索器能力校验
page_type: feature
schema_version: 1
refs:
  - path: metagpt/rag/engines/simple.py
    symbols: [SimpleEngine, from_docs, retrieve]
  - path: metagpt/rag/engines/simple.py
    symbols: [_ensure_retriever_queryable, add_docs, persist]
---

# SimpleEngine 与检索器能力校验

## 一句话职责

`SimpleEngine` 把「文档 → 节点 → 索引 → 检索器 → 重排器」这条链路收进一个入口类里，并在调用增删改查之前先断言「当前检索器是否支持这个操作」，把能力不足从运行期异常提前成调用点的明确报错。

## 机制是什么

它继承 llama-index 的 `RetrieverQueryEngine`，因此天然具备 `retrieve`/`aretrieve`/`query`/`aquery` 全套检索与合成能力。自己加的只有三样东西：

1. **四个构造入口**：`from_docs`（从目录或文件列表）、`from_objs`（从实现了 `rag_key` 的对象列表）、`from_index`（从已持久化的索引）、以及默认构造。四条路最终都汇到「拿到 retriever 与 rankers，构造引擎本体」。
2. **一组能力断言**：`add_docs`/`add_objs`/`persist`/`count`/`clear` 在动手前分别要求检索器支持「可修改」「可持久化」「可查询」「可清空」。断言不是检查某个布尔，而是检查**类型**——`rag.retrievers.base` 里为每种能力定义了一个带 `__subclasshook__` 的协议类，只要目标类有对应方法就算满足。
3. **对象往返**：`add_objs` 把对象转成 `ObjectNode` 入库时把原始对象序列化进 metadata，`retrieve` 之后再从 metadata 反序列化回对象，挂在 `node.metadata["obj"]` 上。

## 怎么实现的

**四条构造入口的分工。** `from_docs` 负责「文件 → 文档」：用 `SimpleDirectoryReader` 读目录或文件列表，PDF 会走 `OmniParse`（仅当配置了 `omniparse.base_url`），然后 `_fix_document_metadata` 把 `file_path` 加进「不参与 embedding 的元数据键」，避免路径污染向量。`from_objs` 负责「对象 → 节点」：调 `get_obj_nodes` 把每个对象转成 `ObjectNode`，文本用 `obj.rag_key()`，元数据用 `ObjectNode.get_obj_metadata(obj)`。`from_index` 负责「已持久化索引 → 引擎」，通过索引工厂按 `index_config` 的类型加载。三条路都在节点就绪后汇到 `_from_nodes` 或 `_from_index`。

`from_objs` 有一条显式校验：对象列表为空且检索器配置里含 `BM25RetrieverConfig` 时直接 `ValueError`。理由是 BM25 需要非空语料才能建索引——这条约束在经验池那边是用「插一条占位经验」绕过的，这里选择直接拒绝。

**`_from_nodes` 的三件事。** 解析 embedding 模型（`_resolve_embed_model`）、取 llm（缺省 `get_rag_llm()`）、用检索器工厂拿 retriever（`get_retriever(configs, nodes, embed_model)`）、用重排工厂拿 rankers（缺省空列表）、最后构造引擎并把 rankers 作为 `node_postprocessors` 传入、`get_response_synthesizer(llm)` 作为合成器。

`_resolve_embed_model` 有一条特殊分支：若所有检索器配置都实现了 `NoEmbedding` 协议（典型是 BM25，不需要向量），就返回 `MockEmbedding(embed_dim=1)` 而不是真实 embedding。这样「只用关键词检索」的部署不会因为缺 embedding 依赖而失败，代价是向量维度是 1 这个假值——任何后续试图用它做向量检索都会静默出错。

**能力断言的实现路径。** 每个公开操作对应一个 `_ensure_retriever_xxx`，它们都转调 `_ensure_retriever_of_type(required_type)`。后者先判「检索器是不是 `SimpleHybridRetriever`」：是则要求其 `retrievers` 列表里至少有一个满足能力；随后**无条件**再执行一次 `isinstance(self.retriever, required_type)` 检查。

这两步的组合有一个必须知道的结果：混合检索器本身只是 `RAGRetriever` 的子类，不会同时是 `ModifiableRAGRetriever` 等能力子类，因此第二步必然失败。也就是说混合检索器上的 `add_docs`/`persist`/`count`/`clear` 一定会抛 `TypeError`，而那半段「至少有一个子检索器满足」的检查在抛错之前就已经做完了，只是结论被后面的无条件判断覆盖。

**`retrieve` 的对象还原。** `SimpleEngine.retrieve` 接受字符串或 `QueryBundle`，字符串会先包成 `QueryBundle`。调用父类拿到节点后统一走 `_try_reconstruct_obj`：对每个 `metadata["is_obj"]` 为真的节点，用 `obj_mod_name` + `obj_cls_name` 动态 import 类，用 `obj_json` 反序列化，结果塞回 `metadata["obj"]`。因此调用方拿到的是「节点 + 可用的原始对象」，可以直接读字段而不必自己解析 JSON。

`ObjectNode` 在构造时把自身元数据键全部加入 `excluded_llm_metadata_keys` 与 `excluded_embed_metadata_keys`——这些字段是给程序用的，不该出现在提示词里，也不该参与相似度计算。

**`add_docs` 与 `add_objs` 共用存储路径。** 两者都先 `_ensure_retriever_modifiable()`，然后各自把输入转成节点，最后都走 `_save_nodes` → `retriever.add_nodes(nodes)`。`add_docs` 用的是引擎记住的 `_transformations`（构造时传入或默认 `[SentenceSplitter()]`），所以「首次建库用的切分方式」会延续到后续追加，不会因为追加而改变切分策略。

**`persist` 是纯委托。** 断言可持久化之后直接 `self._persist(persist_dir)` → `retriever.persist(persist_dir)`。引擎自己不持有存储上下文，落盘完全由检索器决定。这一点与 `count`/`clear` 一致：引擎是编排层，不是状态所有者。

**`delete_docs` 绕过了能力断言。** 它直接翻 `self.retriever._index.ref_doc_info` 找 `metadata["file_path"]` 匹配的 doc_id，再调 `delete_ref_doc`。也就是说它假定检索器一定有 `_index` 这个私有属性，没有走 `DeletableRAGRetriever` 的协议。这与同文件其他操作的做法不一致。

**`asearch` 是给别人用的适配器。** 它只是把参数转给 `aquery`，注释写着 `Inplement tools.SearchInterface`——即为了让引擎能当作「有 `asearch` 方法的搜索工具」被工具层调用。拼写错误留在源码里，但不影响行为。

## 关键取舍

**源码事实：** 能力断言是**类型断言**而非能力探测。`ModifiableRAGRetriever` 等协议类用 `__subclasshook__` + `check_methods` 判「有没有这个方法」，所以判定发生在 `isinstance` 那一刻，不需要实例属性。代价是它只认方法名存在，不认语义。

**源码事实：** 混合检索器的断言逻辑自相矛盾：先检查子检索器是否满足，再无条件做一次整体类型检查，后者一定会失败。因此混合模式下四个能力操作全部不可用。

**源码事实：** `BM25` 的零向量方案是 `MockEmbedding(embed_dim=1)`，一个假 embedding。它让「无向量依赖」的检索器能装进同一套构造流程，代价是维度信息是错的。

**源码事实：** `_from_nodes` 在这个文件里被定义了两次，内容完全相同，后定义覆盖前定义。这是残留的重复代码，不影响行为，但会让「改一处」的直觉失效。

**架构推论：** 把「能力」拆成四个独立协议而不是一个大接口，使得每个检索器只需实现自己实际支持的方法，未实现的能力在调用点报错而不是在构造时报错。这让新后端容易接入，代价是错误发生在使用路径深处而不是装配路径上。

**架构推论：** 引擎不持有存储状态（不持有 `StorageContext`），全部委托给检索器。因此「持久化目录」「集合名」这些概念只出现在检索器配置里，引擎只负责把它们传下去。这也解释了 `persist` 为什么必须由调用方显式给目录——引擎不知道也不关心落哪儿。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/rag/engines/simple.py` | `SimpleEngine` | 编排层：入口、能力断言、对象还原 |
| `metagpt/rag/engines/simple.py` | `from_docs` | 从目录或文件列表建库，含 PDF 解析与元数据清洗 |
| `metagpt/rag/engines/simple.py` | `from_objs` | 从对象列表建库，空对象 + BM25 直接报错 |
| `metagpt/rag/engines/simple.py` | `retrieve` | 检索后按 metadata 还原原始对象 |
| `metagpt/rag/engines/simple.py` | `add_docs` | 断言可修改后用既有切分方式追加文档 |
| `metagpt/rag/engines/simple.py` | `persist` | 断言可持久化后委托检索器落盘 |
| `metagpt/rag/engines/simple.py` | `_ensure_retriever_queryable` | 查询总量前的能力断言入口 |
| `metagpt/rag/engines/simple.py` | `_ensure_retriever_of_type` | 混合检索器的子检索器检查 + 整体类型检查 |
| `metagpt/rag/engines/simple.py` | `_try_reconstruct_obj` | 由 metadata 动态 import 并重建对象 |
| `metagpt/rag/engines/simple.py` | `get_obj_nodes` | 对象转 `ObjectNode`，文本取 `rag_key` |
| `metagpt/rag/retrievers/base.py` | `QueryableRAGRetriever` | 用 `__subclasshook__` 判「有 query_total_count」 |

## 参考

- [MetaGPT 源码：rag/engines/simple.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/rag/engines/simple.py)
- [MetaGPT 源码：rag/retrievers/base.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/rag/retrievers/base.py)
- [LlamaIndex 文档：RetrieverQueryEngine](https://docs.llamaindex.ai/en/stable/module_guides/deploying/query_engine/)

## 与主核 / AI 的关系

「把能力拆成多个小协议、在使用点断言」与主核里「按需探测后端是否支持某特性（如是否支持稀疏文件、是否支持直接 I/O）」是同一种结构：都不要求后端实现完整接口，只要求它声明自己能做什么。差别在判定时机——主核多在打开时探测一次并缓存结论，这里每次操作都重新 `isinstance` 一次，成本可忽略但语义更弱（只验方法名，不验行为）。

对 agent 工程而言，这一页说明了两件会实际踩到的事。其一，混用多种检索器（混合检索）时，增量写入与持久化能力不可用，因为断言会失败——要增量维护就选单一后端。其二，只用 BM25 时系统会塞一个维度为 1 的假 embedding，若之后改成向量检索而忘了改配置，检索结果会是「看起来在跑但没有意义」，因为它比较的是被 mock 掉的向量。

