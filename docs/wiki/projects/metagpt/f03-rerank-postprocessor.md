---
project: metagpt
feature: rerank-postprocessor
title: 重排后处理与对象还原
page_type: feature
schema_version: 1
refs:
  - path: metagpt/rag/rankers/object_ranker.py
    symbols: [ObjectSortPostprocessor]
  - path: metagpt/rag/engines/simple.py
    symbols: [_try_reconstruct_obj, get_obj_nodes]
---

# 重排后处理与对象还原

## 一句话职责

重排后处理是检索链路的最后一环：检索器给出候选节点后，由一组 `node_postprocessors` 重新打分或重排、再裁到 `top_n`；其中 `ObjectSortPostprocessor` 是一种特殊的重排器——它不看向量分数，而是按节点里那个**原始对象的某个字段**排序。

## 机制是什么

llama-index 的 `RetrieverQueryEngine` 在检索之后、合成之前会顺序执行一组 `BaseNodePostprocessor`，每个都能拿到节点列表与查询并返回新列表。`SimpleEngine` 把秩器工厂产出的 rankers 直接当作这组后处理器传入，因此「重排」在本仓就是「后处理链」。

三类后处理器形态不同：

| 类型 | 依据 | 是否调模型 |
|---|---|---|
| `LLMRerank` | 模型对节点与查询的相关性判断 | 是 |
| `ColbertRerank` / `CohereRerank` / `FlagEmbeddingReranker` | 外部重排模型 | 是（外部服务） |
| `ObjectSortPostprocessor` | 节点对象某字段的可比较值 | 否 |

它们都由 `RankerFactory` 按配置类型创建，配置缺省时该链为空（`get_rankers` 直接返回 `[]`），也就是「不重排」。

`ObjectSortPostprocessor` 的意义在于：当库里存的是业务对象（例如一条经验、一条记忆）时，「相关性」往往不如「哪个更新、哪个分更高」这类确定性问题重要。用字段排序替代语义重排，结果可预测、零模型成本。

## 怎么实现的

**`ObjectSortPostprocessor` 的三个配置项。** `field_name` 必填，源码描述了它的使用前提：「字段的值必须可比较」。`order` 取 `desc` 或 `asc`，默认降序。`top_n` 默认 5。类还实现了 `class_name()` 返回固定字符串，这是 llama-index 对可序列化后处理器的要求。

**`_postprocess_nodes` 的四步。**

1. `query_bundle is None` 时抛 `ValueError`。这条看起来多余（排序不需要查询），但它把「被当作后处理器调用」与「被直接调用」区分开——llama-index 在 dry-run 或某些路径下可能不带查询。
2. 空列表直接返回 `[]`，避免后面取 `nodes[0]` 越界。
3. `_check_metadata(nodes[0].node)`——注意**只检查第一个节点**。它要求 `metadata["obj_json"]` 能被 `json.loads` 解析，且解析出的字典里含 `field_name`。后面所有节点都不再校验。
4. 用 `heapq.nlargest`（降序）或 `heapq.nsmallest`（升序）从节点列表里取 `top_n` 条，key 是 `lambda node: json.loads(node.node.metadata["obj_json"])[self.field_name]`。

**排序键每次都重新解析 JSON。** 每个被比较的节点都会被 `json.loads` 一次（`heapq` 内部按需调用 key 函数，最坏接近全部）。没有缓存、没有预计算。节点数不大时无所谓，但这是「按需解析」而非「批量预处理」的选择。

同样的 `json.loads` 在 `_check_metadata` 里对第一个节点做了一次，在 key 函数里对这个节点又做一次——第一个节点被解析了两遍。

**`_check_metadata` 的错误风格。** 两种失败都抛 `ValueError` 而不是跳过：`obj_json` 解析失败抛「invalid object json in metadata」，字段缺失抛「Field X not found in object」。因为它只查了第一个节点，若后续节点缺字段，会在 `json.loads(...)[field_name]` 处抛 `KeyError`——错误类型与前面不一致。

**`add_objs` 一侧如何保证元数据存在。** `SimpleEngine.get_obj_nodes` 是生产端：对每个对象构造 `ObjectNode(text=obj.rag_key(), metadata=ObjectNode.get_obj_metadata(obj))`。后者用 `ObjectNodeMetadata` 填四个字段：`is_obj=True`、`obj=None`、`obj_json=obj.model_dump_json()`、`obj_cls_name=obj.__class__.__name__`、`obj_mod_name=obj.__class__.__module__`。

`ObjectNode` 在 `__init__` 里把这四个键全部加入 `excluded_llm_metadata_keys` 与 `excluded_embed_metadata_keys`。因此这些「程序用的」元数据既不会进提示词、也不参与向量相似度——它们能在检索后存活，但在模型看来是隐形的。

**`_try_reconstruct_obj` 是消费端。** 检索管线的终点调用它：对每个 `is_obj` 为真的节点，用 `obj_mod_name` + `obj_cls_name` 动态 import 出类，用 `obj_json` 反序列化出实例，写进 `metadata["obj"]`。这填上了生产端留下的 `obj=None` 空位。

「import 谁」这件事由数据自己决定：`obj_mod_name` 是入库时记录的模块全路径。因此任何能写入 `ObjectNode` 的对象，在检索时都会被 import 其类——这既是对象的往返机制，也是这条链路上的安全边界（详见取舍）。

两个函数的配对关系值得记住：`get_obj_nodes` 负责「对象 → 带 obj_json 的节点」，`_try_reconstruct_obj` 负责「带 obj_json 的节点 → 对象 + obj」。`ObjectSortPostprocessor` 工作在两者之间，它读的是 `obj_json` 而不是 `obj`，所以即使检索管线上没有做还原，它也能工作。

## 关键取舍

**源码事实：** 字段排序只校验第一个节点的元数据。这是「只抽检一次」的性能取舍，代价是其余节点的问题报错风格不统一（`KeyError` 而非 `ValueError`）。

**源码事实：** 每比较一次就解析一次 JSON。这让后处理器无状态、无预处理要求，属于即插即用；代价是大节点集下的重复解析。

**源码事实：** `ObjectSortPostprocessor` 不改变节点的 `score`。它只做筛选与排序，节点的向量分数原样保留。因此若后面还有另一个按分数排序的后处理器，`top_n` 里的相对顺序可能被再次改写——后处理器链是顺序执行的，谁在最后谁决定最终顺序。

**源码事实：** `ObjectNode` 把程序元数据排除在 embedding 与提示词之外。这让「用 `rag_key()` 做检索、用元数据做还原」成为清晰的分工：检索只看业务文本，还原只看元数据。

**架构推论：** 「按字段排序」把业务语义带进了重排层，代价是重排器必须知道对象的结构（`field_name` 由调用方指定）。这使重排配置与业务模型耦合，但也让「取最近 N 条记忆」「取评分最高的 N 条经验」这类需求不必再写一遍自定义检索。

**架构推论：** 对象还原用「动态 import 由数据指定」的方式，使 RAG 层不需要预注册对象类型。这消除了注册表维护成本，同时意味着检索结果能触发任意模块的导入。任何把外部可控内容写进 `obj_mod_name` 的路径都会变成代码加载面——生产环境应确保写入端只有自己。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/rag/rankers/object_ranker.py` | `ObjectSortPostprocessor` | 按对象字段排序的后处理器 |
| `metagpt/rag/rankers/object_ranker.py` | `ObjectSortPostprocessor._postprocess_nodes` | 首个节点校验 + 堆排序取 top_n |
| `metagpt/rag/rankers/object_ranker.py` | `ObjectSortPostprocessor._check_metadata` | `obj_json` 可解析且含目标字段 |
| `metagpt/rag/rankers/object_ranker.py` | `ObjectSortPostprocessor._get_sort_func` | 降序用 nlargest、升序用 nsmallest |
| `metagpt/rag/engines/simple.py` | `get_obj_nodes` | 对象转节点，写入四段程序元数据 |
| `metagpt/rag/engines/simple.py` | `_try_reconstruct_obj` | 由元数据动态 import 并还原对象 |
| `metagpt/rag/factories/ranker.py` | `RankerFactory` | 按配置类型创建各类后处理器 |
| `metagpt/rag/schema.py` | `ObjectNode` / `ObjectNodeMetadata` | 节点与元数据的字段契约 |

## 参考

- [MetaGPT 源码：rag/rankers/object_ranker.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/rag/rankers/object_ranker.py)
- [MetaGPT 源码：rag/factories/ranker.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/rag/factories/ranker.py)
- [LlamaIndex 文档：Node Postprocessor 模块](https://docs.llamaindex.ai/en/stable/module_guides/querying/node_postprocessors/)

## 与主核 / AI 的关系

「检索给出候选集、后处理按业务规则收敛到 top_n」与主核里「访问路径给出候选块、过滤条件按谓词筛掉不符合的行」是同一种分层：检索负责召回，后处理负责精确化。差别在于主核的过滤在扫描时就地完成、有索引可依；这里的后处理在内存里对已召回的小集合操作，本身就是「先用近似手段缩小范围，再用精确规则定序」。

对 agent 工程而言，这一页回答了「为什么从向量库取回的东西能直接当对象用」。答案是两端的约定：写入时把对象序列化进元数据，读出时按元数据里的类路径重建。这也解释了它的成本结构——还原只对 `is_obj` 为真的节点做，普通文档节点（来自 `from_docs`）完全不走这条路径，因此「混存文档与对象」在同一个库里是可行的。

