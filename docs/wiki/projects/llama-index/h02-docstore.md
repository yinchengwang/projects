---
project: llama-index
feature: docstore
title: 文档存储与引用信息
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/storage/docstore/types.py
    symbols: [BaseDocumentStore, RefDocInfo]
  - path: llama-index-core/llama_index/core/storage/docstore/keyval_docstore.py
    symbols: [KVDocumentStore]
---

# 文档存储与引用信息

## 一句话职责

把「节点内容」和「某个原始文档产出了哪些节点」这两件事分开存：前者按节点 id 一个键，后者用 `RefDocInfo` 做一张**文档 id → 节点 id 列表**的反向索引——正因为有这张表，删除一整个文档时才能精确清掉它派生的所有节点。

## 机制是什么

`BaseDocumentStore` 的抽象面可以分成五组：

1. **节点本体**：`docs` 属性、`add_documents` / `async_add_documents`、`get_document` / `aget_document`、`delete_document` / `adelete_document`、`document_exists`。
2. **哈希**：`set_document_hash` / `get_document_hash` / `get_all_document_hashes`（各带异步版，哈希还有批量设置版）。
3. **引用文档**：`get_ref_doc_info` / `get_all_ref_doc_info`（各带异步版）、`delete_ref_doc` / `adelete_ref_doc`。
4. **便利方法**（非抽象）：`get_nodes` / `aget_nodes`、`get_node` / `aget_node`、`get_node_dict` / `aget_node_dict`。
5. **`persist`**：**有方法体但是空实现**——签名在，函数体只有 docstring，没有任何 `@abstractmethod`。

`RefDocInfo` 是一个 `@dataclass` + `DataClassJsonMixin`，两个字段：

```python
node_ids: List = field(default_factory=list)
metadata: Dict[str, Any] = field(default_factory=dict)
```

所以它就是那张反向索引：**一个原始文档 id 对应它切出的所有节点 id**，外加这份文档自己的元数据。

## 怎么实现的

**`KVDocumentStore` 用三个 collection 实现这三类数据。** 三个后缀常量把这件事写得极清楚：

```python
DEFAULT_COLLECTION_DATA_SUFFIX = "/data"          # 每个节点的内容 + 节点级元数据
DEFAULT_REF_DOC_COLLECTION_SUFFIX = "/ref_doc_info"  # 文档 → 节点 id 列表的反向索引
DEFAULT_METADATA_COLLECTION_SUFFIX = "/metadata"     # 节点 → 它属于哪个文档
```

而 `DEFAULT_NAMESPACE = "docstore"`，最终三个 collection 名是拼接出来的：`f"{namespace}{suffix}"`。所以「换一个 namespace」就能在同一份 KV 后端上放多套 docstore 而不冲突。

三个 collection 的职责**不重叠**：`/data` 存节点全文（可选，见 `store_text`），`/ref_doc_info` 存反向索引，`/metadata` 存每个节点的 `doc_hash` 与 `ref_doc_id`。这就是三层结构的最小实现——**每个方向的关系各占一个键空间**，而不是把关系塞进节点对象里。

**`_get_kv_pairs_for_insert` 一次算出三个键值对的「可能」存在。** 签名返回三个 `Optional[Tuple[str, dict]]`，实现分四条：

```python
node_key = node.node_id
data = doc_to_json(node)
if store_text:
    node_kv_pair = (node_key, data)
```

所以 `store_text=False` 时**节点内容不进 `/data`**——但元数据与反向索引照旧写。这让「只想检索、不想让节点文本占空间」成为一种可行配置（代价是之后拿不到节点内容）。

```python
metadata = {"doc_hash": node.hash}
if ref_doc_info is not None and node.ref_doc_id:
    if node.node_id not in ref_doc_info.node_ids:
        ref_doc_info.node_ids.append(node.node_id)
    if not ref_doc_info.metadata:
        ref_doc_info.metadata = node.metadata or {}
    metadata["ref_doc_id"] = node.ref_doc_id
    metadata_kv_pair = (node_key, metadata)
    ref_doc_kv_pair = (node.ref_doc_id, ref_doc_info.to_dict())
else:
    metadata_kv_pair = (node_key, metadata)
```

两个条件决定分支：`ref_doc_info is not None`（说明这个节点有 `source_node`）**且** `node.ref_doc_id` 非空。

进入分支时三件事：**幂等地**把 `node_id` 加进列表（`not in` 检查避免重复）；若 `RefDocInfo` 的 `metadata` 还是空就填上节点的 metadata（`if not ref_doc_info.metadata` —— 所以**第一个节点的元数据赢**，后续节点不会覆盖）；最后把 `ref_doc_id` 也写进节点元数据。

不进分支时**仍然写 `metadata_kv_pair`**——所以每个节点的 `/metadata` 记录都存在（至少带 `doc_hash`），只是可能没有 `ref_doc_id`。

**「多个节点指向同一个 `ref_doc_id`」必须合并。** `_prepare_kv_pairs` 里 `ref_doc_kv_pairs` 是 `Dict[str, List[Tuple[str, dict]]]` ——先按 `ref_doc_id` 分组收集，然后 `_merge_ref_doc_kv_pairs` 合并：

```python
for key, kv_pairs in ref_doc_kv_pairs.items():
    merged_node_ids = []
    metadata = {}
    for kv_pair in kv_pairs:
        nodes = kv_pair[1].get("node_ids", [])
        new_nodes = set(nodes).difference(set(merged_node_ids))
        merged_node_ids.extend([node for node in nodes if node in new_nodes])
        metadata.update(kv_pair[1].get("metadata", {}))
    merged_ref_doc_kv_pairs.append((key, {"node_ids": merged_node_ids, "metadata": metadata}))
```

这段代码的意义是：**如果不合并，同一批里 10 个节点会产生 10 条对同一个 `ref_doc_id` 的 `put`，最后一条赢——于是只有最后一个节点的 id 被记住，其余 9 条关系丢失。**

合并时的顺序保留值得看：`set(nodes) - set(merged_node_ids)` 得到「本批新增的」，然后 `[node for node in nodes if node in new_nodes]` 按**原列表顺序**过滤。所以既去重又保序。而 `metadata.update(...)` 是按批顺序覆盖——与 `_get_kv_pairs_for_insert` 里「第一个赢」相反，这里是**最后一个赢**。两处优先级相反，是一个值得记的不一致。

**`allow_update=False` 是「一个节点的重复」而不是「一个文档的重复」。** 检查写在循环里：`if not allow_update and self.document_exists(node.node_id)`。而 `document_exists` 查的是 `/data` collection —— 所以若 `store_text=False`，这个检查**永远为假**（`/data` 里没有键），`allow_update=False` 就失效了。

这是 `store_text` 与 `allow_update` 之间一个真实的耦合缺陷：两个参数各自独立地看起来合理，组合起来却让「禁止覆盖」这个保护静默失效。

**同步写是三次 `put_all` 串行，异步写是 `asyncio.gather` 并发。** 这个差别不只是风格：

```python
# sync
self._kvstore.put_all(node_kv_pairs, collection=self._node_collection, batch_size=batch_size)
self._kvstore.put_all(metadata_kv_pairs, collection=self._metadata_collection, batch_size=batch_size)
self._kvstore.put_all(ref_doc_kv_pairs, collection=self._ref_doc_collection, batch_size=batch_size)
```

三次写没有事务包裹。所以若第二次失败，`/data` 已经写进去了而 `/metadata` 没有 —— 留下「有节点内容但没有元数据」的半成品。异步版的 `gather` 更激进：三个并发，任一个失败时其他两个可能已经完成也可能没有（`gather` 默认 `return_exceptions=False`，第一个异常立即上抛，其余 task 不被取消而是继续跑）。

所以这一层**没有原子性保证**。它的「一致性」靠的是「三个 collection 各自按 key 独立」，以及 `allow_update` 那个（可能被绕过的）检查。任何需要「要么全成功要么全失败」的场景，必须靠外层的重试或幂等重放。

**删除是「先反查、再逐键删」的三段式，而且有明确的兜底动作。** `_remove_from_ref_doc_node`：

```python
ref_doc_id = self._get_ref_doc_id(doc_id)          # 从 /metadata 查到它属于哪个文档
if ref_doc_id is None: return
ref_doc_info = self.get_ref_doc_info(ref_doc_id)   # 从 /ref_doc_info 拿到反向索引
if ref_doc_info is None: return
if doc_id in ref_doc_info.node_ids:                # sanity check
    ref_doc_info.node_ids.remove(doc_id)
if len(ref_doc_info.node_ids) > 0:
    self._kvstore.put(ref_doc_id, ref_doc_info.to_dict(), collection=self._ref_doc_collection)
else:
    self._kvstore.delete(ref_doc_id, collection=self._metadata_collection)
    self._kvstore.delete(ref_doc_id, collection=self._node_collection)
    self._kvstore.delete(ref_doc_id, collection=self._ref_doc_collection)
```

两个 `if ... None: return` 是宽容的早退——所以「删一个从未被索引的节点」不会报错，只是什么都不做（而 `delete_document` 之后会用 `delete_success` 决定是否报「not found」）。

`doc_id in ref_doc_info.node_ids` 那句注释写着 `# sanity check` —— 若不在，就**跳过移除但仍继续后续逻辑**。于是「`node_ids` 里没有这个 id」时，若列表非空会原样写回，若列表为空会走 else 分支删掉整条引用记录——后者是错的（本该什么都没删到，却把文档记录清了）。

`else` 分支的语义是「这个文档的最后一个节点被删了，所以文档记录也该消失」。它删了三个键：`ref_doc_id` 在 `/metadata`、`/data`、`/ref_doc_info` 三个 collection 各一次。注意它删的是 **`ref_doc_id`**（文档 id）而不是 `doc_id`（节点 id）——所以这是一次「顺便清理文档级残留」的动作，与「删节点」是两个不同的事，被合并在一处。

`delete_ref_doc` 则是「按反向索引逐个删节点，再无条件补三刀」：

```python
original_node_ids = ref_doc_info.node_ids.copy()  # copy to avoid mutation during iteration
for doc_id in original_node_ids:
    self.delete_document(doc_id, raise_error=False)
# Deleting all the nodes should already delete the ref_doc, but just to be sure
self._kvstore.delete(ref_doc_id, collection=self._ref_doc_collection)
self._kvstore.delete(ref_doc_id, collection=self._metadata_collection)
self._kvstore.delete(ref_doc_id, collection=self._node_collection)
```

`.copy()` 那行注释解释了必要性：循环里 `delete_document` 会调 `_remove_from_ref_doc_node` 去改 `ref_doc_info.node_ids` 这个列表，而那个 `ref_doc_info` 正是从 KV 读出来的**新对象**（不是当前循环手上这个）—— 但实际上 `_remove_from_ref_doc_node` 每次都重新读一次，所以循环变量不会被它改。

那句「删掉所有节点应该已经删掉了 ref_doc，但为了确保」是这一层最诚实的注释：它承认**前一步的副作用不该被依赖**。补三刀的成本是三次 `delete`（幂等，删不存在的键返回 `False` 而不抛错——见 h01 的 `MutableMappingKVStore.delete`），换来的是「不论前面某步是否漏了，最终状态都是干净的」。

这是一处值得学的取舍：**收尾时做一次幂等的全覆盖清理**，比精心推导「哪一步已经做过了」更可靠。后者一旦有一步改了行为，所有推导都失效。

**`RefDocInfo` 有一条遗留格式的兼容路径。** `_remove_legacy_info`：

```python
if "doc_ids" in ref_doc_info_dict:
    ref_doc_info_dict["node_ids"] = ref_doc_info_dict.get("doc_ids", [])
    ref_doc_info_dict.pop("doc_ids")
    ref_doc_info_dict["metadata"] = ref_doc_info_dict.get("extra_info", {})
    ref_doc_info_dict.pop("extra_info")
```

所以旧版本把字段叫 `doc_ids` 与 `extra_info`，新版本叫 `node_ids` 与 `metadata`。三个读取方法（`get_ref_doc_info` / `aget_ref_doc_info` / `get_all_ref_doc_info`）都调它，且每处都挂着 `# TODO: deprecated legacy support`。

这个兼容是**读时迁移**而不是写时迁移：读到旧字段就改名成新名，但**不回写**。所以一个老库每读一次都要转一次，直到有人重新 `add_documents` 覆盖它。这是「不做破坏性迁移」的温和做法，代价是这段代码要永久保留。

**`get_all_document_hashes` 返回的方向与 `set_document_hashes` 相反。** 写侧：

```python
for doc_id, doc_hash in doc_hashes.items():
    metadata_kv_pairs.append((doc_id, {"doc_hash": doc_hash}))
```

所以输入是 `{doc_id: doc_hash}`。而读侧的推导：

```python
return {
    doc_hash: doc_id
    for doc_id, doc in (...aget_all(metadata collection)...).items()
    if (doc_hash := doc.get("doc_hash"))
}
```

返回 `{doc_hash: doc_id}` ——**反过来了**。命名上两侧都叫「document hashes」，但一个是 id→hash、一个是 hash→id。

这个颠倒大概率不是故意的（变量名 `doc_hash` 与 `doc_id` 在推导式里的位置正好对调，看起来像顺手写反了），而它会造成「按 hash 查 id」的语义——恰好是去重场景想要的方向。但因为类型签名两边都是 `Dict[str, str]`，类型检查发现不了。

这段还用了海象运算符：`if (doc_hash := doc.get("doc_hash"))` —— 既取值又判真值，所以**空字符串与缺失都跳过**。

**`get_node` 是 `get_document` 之上的一层类型守卫。** 实现：

```python
doc = self.get_document(node_id, raise_error=raise_error)
if doc is None:
    # The doc store should have raised an error if the node_id is not found, but it didn't
    # so we raise an error here
    if raise_error: raise ValueError(f"Node {node_id} not found")
    return None
if not isinstance(doc, BaseNode):
    raise ValueError(f"Document {node_id} is not a Node.")
return doc
```

第一段注释点明了一个契约假设：「docstore 本应在找不到时抛错，但它没有，所以我们在这里抛」。所以这是**双保险**—— 一层在 `get_document`，一层在 `get_node`。

第二段 `isinstance` 检查的注释写着「文档应当总是 `BaseNode`，但我们检查以求稳妥」。这一条不是多余：`docs` 与 `get_document` 的类型标注是 `BaseNode`，但实际存储里若混进了一个旧格式或被误写的值，这里能挡住，而不是让一个错误的类型流到下游。

`get_node` 与 `aget_node` 各有两份 `@overload`：`raise_error: Literal[True]` → 返回 `BaseNode`；`Literal[False]` → 返回 `Optional[BaseNode]`。所以「要不要抛错」这个布尔参数在**类型层面**也被表达了——调用方传字面量 `False` 时类型检查器知道结果可能是 `None`，不必再 assert。

而 `get_nodes` 在 `raise_error=False` 时用海象过滤：

```python
if node := self.get_node(node_id=node_id, raise_error=False):
    nodes.append(node)
```

所以批量取时「跳过得那些不存在的」而不是留 `None` 占位—— 返回列表可能比输入短，且**不保证顺序对应**。这与 langchain 那边 `DocumentIndex.get` 的约定一致（见本项目之外的那套契约）。

## 关键取舍

**源码事实：** `BaseDocumentStore.persist` 有签名但函数体空—— 不是 `@abstractmethod`，也不做事。所以「持久化」在基类层面是「可选的、什么都不做」。

**源码事实：** `KVDocumentStore` 用三个 collection（`/data`、`/ref_doc_info`、`/metadata`）分别存节点内容、反向索引、节点级元数据。

**源码事实：** `store_text=False` 时节点内容不进 `/data`，但元数据与反向索引照旧写；此时 `allow_update=False` 的检查**永远为假**（因为它查的就是 `/data`）。

**源码事实：** 同批多节点指向同一 `ref_doc_id` 时必须合并（否则只有最后一条关系被记住）；合并去重且保序。

**源码事实：** `RefDocInfo.metadata` 由「第一个节点」填，之后不再覆盖；而合并阶段 `metadata.update` 是「最后一个赢」—— 两处优先级相反。

**源码事实：** 同步 `add_documents` 三次 `put_all` 串行无事务；异步版用 `asyncio.gather` 并发。

**源码事实：** `delete_ref_doc` 逐节点删完之后**无条件再删三个 collection 里该 `ref_doc_id`**，注释自述「为了确保」。

**源码事实：** `_remove_legacy_info` 把旧字段 `doc_ids` / `extra_info` 读时改名为 `node_ids` / `metadata`，不回写。

**源码事实：** `set_document_hashes` 收 `{doc_id: doc_hash}`，而 `get_all_document_hashes` 返回 `{doc_hash: doc_id}` —— 方向相反。

**架构推论：** 「节点内容」与「反向索引」分开存，是让删除成为可能的前提。若只存节点、不存反向索引，删一个文档就必须「扫描全部节点、判断哪个属于它」—— 在外部 KV 上等于全表扫。`RefDocInfo` 把这件事从 O(N) 变成 O(该文档的节点数)。这正是任何「一对多派生关系」的通用解法：**在写入时维护反向指针，把删除时的搜索换成查表**。

**架构推论：** 三个 collection 而不是一个大键空间，让「每类关系的写入可以独立批处理」。注意 `put_all` 的 `batch_size` 是分开传的—— 若某一类数据特征不同（例如节点内容大、元数据小），理论上可以给不同的批量。而实际实现里都传同一个 `batch_size`，所以这个潜力没被用上，但结构上留了口子。

**架构推论：** 「收尾时幂等全覆盖清理」这条思路值得推广。任何多步删除/清理，最后一步都该是「把该消失的东西按名字直接删一遍」而不是「依赖前面每步都做对了」。前者在改代码时保持正确，后者不会。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/storage/docstore/types.py` | `BaseDocumentStore` | 五组抽象：节点、哈希、引用文档、便利方法、空的 `persist` |
| `llama-index-core/llama_index/core/storage/docstore/types.py` | `RefDocInfo` | 文档 id → 节点 id 列表的反向索引 |
| `llama-index-core/llama_index/core/storage/docstore/types.py` | `BaseDocumentStore.get_node` | 类型守卫加双保险抛错，`@overload` 表达 `raise_error` |
| `llama-index-core/llama_index/core/storage/docstore/types.py` | `BaseDocumentStore.get_nodes` | 批量取，跳过缺失项 |
| `llama-index-core/llama_index/core/storage/docstore/types.py` | `DEFAULT_PERSIST_FNAME` | `docstore.json` |
| `llama-index-core/llama_index/core/storage/docstore/keyval_docstore.py` | `KVDocumentStore` | 三个 collection 的 KV 实现 |
| `llama-index-core/llama_index/core/storage/docstore/keyval_docstore.py` | `DEFAULT_COLLECTION_DATA_SUFFIX` | `/data`，节点内容 |
| `llama-index-core/llama_index/core/storage/docstore/keyval_docstore.py` | `DEFAULT_REF_DOC_COLLECTION_SUFFIX` | `/ref_doc_info`，反向索引 |
| `llama-index-core/llama_index/core/storage/docstore/keyval_docstore.py` | `DEFAULT_METADATA_COLLECTION_SUFFIX` | `/metadata`，节点级哈希与归属 |
| `llama-index-core/llama_index/core/storage/docstore/keyval_docstore.py` | `_get_kv_pairs_for_insert` | 一次算出三对键值，`store_text` 决定是否写内容 |
| `llama-index-core/llama_index/core/storage/docstore/keyval_docstore.py` | `_merge_ref_doc_kv_pairs` | 同批合并反向索引，去重且保序 |
| `llama-index-core/llama_index/core/storage/docstore/keyval_docstore.py` | `_prepare_kv_pairs` | `allow_update` 检查与三路收集 |
| `llama-index-core/llama_index/core/storage/docstore/keyval_docstore.py` | `KVDocumentStore.add_documents` | 三次串行 `put_all`，无事务 |
| `llama-index-core/llama_index/core/storage/docstore/keyval_docstore.py` | `KVDocumentStore.async_add_documents` | 三次并发 `aput_all` |
| `llama-index-core/llama_index/core/storage/docstore/keyval_docstore.py` | `_remove_from_ref_doc_node` | 从反向索引摘除节点，空了就清文档记录 |
| `llama-index-core/llama_index/core/storage/docstore/keyval_docstore.py` | `KVDocumentStore.delete_ref_doc` | 逐节点删加收尾三刀全覆盖 |
| `llama-index-core/llama_index/core/storage/docstore/keyval_docstore.py` | `_remove_legacy_info` | 旧字段读时改名，不回写 |
| `llama-index-core/llama_index/core/storage/docstore/keyval_docstore.py` | `KVDocumentStore.get_all_document_hashes` | 返回 hash → id，与写入方向相反 |

## 参考

- [llama-index 源码：storage/docstore/keyval_docstore.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/storage/docstore/keyval_docstore.py)
- [llama-index 源码：storage/docstore/types.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/storage/docstore/types.py)
- [llama-index 文档：文档管理](https://docs.llamaindex.ai/en/stable/module_guides/storing/docstores/)

## 与主核 / AI 的关系

主核里最接近的是「二级索引 / 反向指针」：主表按主键存行，另建一张索引把「某个值」映射到主键列表。删一行时靠索引找到所有相关项。`RefDocInfo` 就是这张反向索引表的对应物，连「写入时维护、删除时查表」这个结构都一样。

差别在于**主核的索引与数据在同一个事务里**。这里三个 collection 的三次写是三个独立操作，中间失败就留下不一致状态。所以主核的二级索引能保证「不存在指向已删行的索引条目」，而这里的 `/ref_doc_info` 可能指向已不存在的节点 —— 这正是 `delete_ref_doc` 要补那三刀的原因：用幂等清理对冲缺失的事务。

与 h01（存储上下文）的关系是直接的下层：`StorageContext.docstore` 字段的类型就是 `BaseDocumentStore`，而 `KVDocumentStore` 又建立在 h01 的 `BaseKVStore` 上。所以这三层是：上下文 → 文档存储 → 键值存储。

与 langchain 项目的 g02（记录管理器）对照是一个很好的对比点：那边用「内容哈希 → 分组 + 时间戳」做簿记，目的是判断「哪些该删」；这里用「文档 id → 节点 id 列表」做反向索引，目的是**知道删的时候删哪些**。前者服务于「增量判定」，后者服务于「关系遍历」。两者都是「在数据之外维护一份关系表」，但关系表的形状由使用方式决定。

对 AI 工程最实用的一条是「一对多派生关系必须有反向索引」。RAG 里最典型的形态就是「一份原始文档 → 几十个切块节点」。如果没有反向索引，删除一份文档就得遍历全部节点判断归属，而这在外部向量库/KV 上几乎等价于全表扫。做法很简单：写节点时顺手把它加进文档的记录里。

另一条是「同一批里对同一个 key 的多次写必须先合并」。`_merge_ref_doc_kv_pairs` 那段代码只做了一件事，但不做它的后果很严重：10 个节点同属一份文档，10 次 `put` 之后只有最后一个节点的 id 被记住，另外 9 个成了孤儿—— 它们在 `/data` 里在，但没有任何反向索引指向它们，于是永远删不掉。这类「同一个键在一批里出现多次」的情况在任何「先分组后写」的流程里都要显式处理。

最后一条是 `delete_ref_doc` 那句「删掉所有节点应该已经删掉了 ref_doc，但为了确保」。这是工程上很成熟的一个判断：**不要依赖前序步骤的副作用**。前面每一步都对，收尾就不用做；但若哪一步将来改了行为（例如某天 `_remove_from_ref_doc_node` 不再清文档记录），收尾那三刀仍然能兜住。代价是三次多余的 `delete`。
