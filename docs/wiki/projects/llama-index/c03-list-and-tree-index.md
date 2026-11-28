---
project: llama-index
feature: list-and-tree-index
title: 列表索引与树索引
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/indices/list/base.py
    symbols: [SummaryIndex, ListRetrieverMode]
  - path: llama-index-core/llama_index/core/indices/tree/base.py
    symbols: [TreeIndex, TreeRetrieverMode]
---

# 列表索引与树索引

## 一句话职责

`SummaryIndex` 是**最简的索引**：把节点 id 按顺序排成一个列表，检索时要么全给、要么靠嵌入/LLM 挑几个；`TreeIndex` 是**自底向上建摘要树**：每 `num_children` 个节点合成一个父节点（内容由 LLM 摘要），一直收到根节点，检索时从根往下走。

## 机制是什么

这两个索引是同一谱系的两端，而它们的存在恰好说明了「索引」这个词在 RAG 里可以有多轻：

| | `SummaryIndex` | `TreeIndex` |
|---|---|---|
| 索引结构 | `IndexList`（一个 id 列表） | `IndexGraph`（id → 序号 + 父子映射） |
| 构建成本 | **零 LLM 调用零嵌入** | 每层每组合一次摘要调用 |
| 检索形态 | 三种模式（全给 / 嵌入 / LLM） | 四种模式（选叶 / 嵌入选叶 / 全叶 / 仅根） |
| 能否删除 | 支持 | **抛 `NotImplementedError`** |

`SummaryIndex` 的构建是纯粹的「收集 id」——`_build_index_from_nodes` 里只有一个 for 循环调 `add_node`。所以它的价值不是「索引」而是**「把这批节点打包成一个可检索的对象」**：它让下游的 query engine 不必知道节点从哪来。

`TreeIndex` 则必须花 LLM 调用，因为父节点的内容是**摘要**而不是拼接。这也是它不能删除的原因（后文会讲到）。

## 怎么实现的

**`ListRetrieverMode` 只有三个值，而它们对应三种完全不同的检索代价。**

```python
class ListRetrieverMode(str, Enum):
    DEFAULT = "default"
    EMBEDDING = "embedding"
    LLM = "llm"
```

而 `as_retriever` 按模式分派到三个检索器：

```python
if retriever_mode == ListRetrieverMode.DEFAULT:
    return SummaryIndexRetriever(self, object_map=self._object_map, **kwargs)
elif retriever_mode == ListRetrieverMode.EMBEDDING:
    embed_model = embed_model or Settings.embed_model
    return SummaryIndexEmbeddingRetriever(self, object_map=self._object_map, embed_model=embed_model, **kwargs)
elif retriever_mode == ListRetrieverMode.LLM:
    llm = llm or Settings.llm
    return SummaryIndexLLMRetriever(self, object_map=self._object_map, llm=llm, **kwargs)
else:
    raise ValueError(f"Unknown retriever mode: {retriever_mode}")
```

三处：

1. **`DEFAULT` 不需要任何模型**——它大概是「把整个列表原样返回」。所以零成本的模式是默认值，这很合理。
2. **`EMBEDDING` 与 `LLM` 各自从 `Settings` 取默认模型**，而参数只在用户显式给时才用。所以一个没配 `Settings.embed_model` 的环境在下 `EMBEDDING` 模式时会拿到 `None` 并在检索器内部失败。
3. **`else` 分支理论上不可达**（三个枚举值都被覆盖），但保留了 `raise ValueError`。它的实际作用是拦住**传字符串但拼错**的情形——`retriever_mode` 声明为 `Union[str, ListRetrieverMode]`，所以传 `"dafault"` 会走到这里。

注意 `retriever_mode` 参数同时接受 `str` 与枚举，而比较用的是枚举值。`"default" == ListRetrieverMode.DEFAULT` 在 `str` 枚举下成立（因为 `ListRetrieverMode` 继承 `str`），所以字符串能匹配上。这是 b01 里「`str` 枚举 + 显式字面量」写法的实际收益——**字符串与枚举可以直接比较**（对比 `auto()` 那种数字串的情形）。

**`SummaryIndex._build_index_from_nodes` 是这套设计里最短的实现。**

```python
index_struct = IndexList()
nodes_with_progress = get_tqdm_iterable(nodes, show_progress, "Processing nodes")
for n in nodes_with_progress:
    index_struct.add_node(n)
return index_struct
```

两处：

- **`get_tqdm_iterable` 包一层进度条**。注意这里的进度条是**真的按节点计数的**（每个节点一次迭代），而 c02 的向量索引是按批次。
- **`IndexList.add_node` 只 `append(node.node_id)`**，所以索引结构就是一组 id，没有别的信息。

**`_delete_node` 的做法值得逐行读，因为它把「删除」实现在了 docstore 之上。**

```python
cur_node_ids = self._index_struct.nodes
cur_nodes = self._docstore.get_nodes(cur_node_ids)
nodes_to_keep = [n for n in cur_nodes if n.node_id != node_id]
self._index_struct.nodes = [n.node_id for n in nodes_to_keep]
```

四处：

1. **先从 docstore 把全部节点读出来**（`get_nodes(cur_node_ids)`）。所以删除一个节点要付出读全部节点的代价——**O(n) 的读取**。
2. **过滤掉目标节点，再重建整个列表**。注意这是「重建」而不是「原地删除」：`self._index_struct.nodes` 被整体替换。
3. **`get_nodes` 返回的顺序是否与 `cur_node_ids` 一致？** 这里**依赖它一致**。若 docstore 的实现（比如某个 KV 后端）返回顺序不同，重建后的列表就会**乱序**——而列表索引的顺序语义（「节点按顺序存放」）就被破坏了。源码没有校验。
4. **节点不在索引里时也不报错**——只是重建出一个内容不变的列表。所以删除是幂等的。这与 c02 的 `IndexDict.delete`（`del` 不幂等）形成对比。

为什么删除需要读 docstore 而不能只在 id 列表上操作？因为**要按 `node_id` 过滤，而 id 列表里存的就是 `node_id`**——理论上完全可以只在列表上做。所以这里读 docstore 是**不必要的**。这大概是从「删除需要知道节点其它信息」的早期实现延续下来的。

**`ref_doc_info` 在列表索引与向量索引里是同一段代码。**

```python
node_doc_ids = self._index_struct.nodes
nodes = self.docstore.get_nodes(node_doc_ids)
all_ref_doc_info = {}
for node in nodes:
    ref_node = node.source_node
    if not ref_node:
        continue
    ref_doc_info = self.docstore.get_ref_doc_info(ref_node.node_id)
    if not ref_doc_info:
        continue
    all_ref_doc_info[ref_node.node_id] = ref_doc_info
return all_ref_doc_info
```

与 c02 的 `ref_doc_info` 相比，**只有第一行的取 id 方式不同**（那里是 `nodes_dict.values()`，这里是 `nodes`）。其余完全相同。所以「按 SOURCE 关系回溯源文档」是这套索引族共有的逻辑，却**在每个索引里各写了一遍**。这是「基类没抽这个方法」的直接后果。

**`TreeRetrieverMode` 四个值，而其中三个要求树真的建了。**

```python
class TreeRetrieverMode(str, Enum):
    SELECT_LEAF = "select_leaf"
    SELECT_LEAF_EMBEDDING = "select_leaf_embedding"
    ALL_LEAF = "all_leaf"
    ROOT = "root"

REQUIRE_TREE_MODES = {
    TreeRetrieverMode.SELECT_LEAF,
    TreeRetrieverMode.SELECT_LEAF_EMBEDDING,
    TreeRetrieverMode.ROOT,
}
```

四个模式的含义：

| 模式 | 怎么检索 | 需要树吗 |
|---|---|---|
| `SELECT_LEAF` | 从根开始用 LLM 逐层选子节点 | 是 |
| `SELECT_LEAF_EMBEDDING` | 从根开始用嵌入选子节点 | 是 |
| `ALL_LEAF` | 取所有叶子节点 | **否** |
| `ROOT` | 只用根节点 | 是 |

**`ALL_LEAF` 不需要树**，因为全部叶子节点本来就存在（它们是原始节点）。其余三个都要沿树走，所以必须真的建了树。这个区分落在 `REQUIRE_TREE_MODES` 这个集合上，由 `_validate_build_tree_required` 消费：

```python
def _validate_build_tree_required(self, retriever_mode: TreeRetrieverMode) -> None:
    if retriever_mode in REQUIRE_TREE_MODES and not self.build_tree:
        raise ValueError("Index was constructed without building trees, "
                         f"but retriever mode {retriever_mode} requires trees.")
```

即**`build_tree=False` 时只能用 `ALL_LEAF`**。这个选项存在是因为建树很贵（每次摘要一次 LLM 调用），用户可以先把节点存进去、之后再决定要不要建树。

注意 `as_retriever` 里有个容易漏的细节：

```python
self._validate_build_tree_required(TreeRetrieverMode(retriever_mode))
```

**它先把 `retriever_mode` 转成枚举再校验**（因为参数可能是字符串）。而**后面的分派仍然用原始值**（`if retriever_mode == TreeRetrieverMode.SELECT_LEAF:`）——依赖 `str` 枚举的相等性。所以字符串 `"root"` 能通过校验（转成枚举）也能匹配分派（字符串比较）。两条路径都对，但用了两种机制。

**`TreeIndex` 的构建是「委托给 builder」，而 `_insert` 是「委托给 inserter」。**

```python
def _build_index_from_nodes(self, nodes, **build_kwargs) -> IndexGraph:
    index_builder = GPTTreeIndexBuilder(
        self.num_children, self.summary_template,
        llm=self._llm, use_async=self._use_async,
        show_progress=self._show_progress, docstore=self._docstore,
    )
    return index_builder.build_from_nodes(nodes, build_tree=self.build_tree)
```

注意**它把 `self._docstore` 传给 builder**。这很关键：摘要出来的父节点也要存进 docstore（否则检索走到父节点时拿不到内容）。所以**建树过程会写 docstore**，这与 c02 里「docstore 写入在 `_add_nodes_to_index` 内部」是同一种做法（下推到具体实现），也与 c01 里「父类统一写 docstore」相反。

而 `_insert` 用 `TreeIndexInserter`，并带一句注释：

```python
# TODO: allow to customize insert prompt
inserter = TreeIndexInserter(...)
inserter.insert(nodes)
```

`insert_prompt` 在构造函数里可以被设置（`self.insert_prompt = insert_prompt or DEFAULT_INSERT_PROMPT`），而这条 TODO 说的是「允许自定义插入提示」——但构造参数已经存在了。所以这条 TODO 大概是「`_insert` 应该接受每次插入时传入的提示」而不是「构造时配置的提示」。这是一个**注释与代码状态不一致**的实例。

**`TreeIndex._delete_node` 直接抛错，而理由可以从索引结构推断出来。**

```python
def _delete_node(self, node_id: str, **delete_kwargs: Any) -> None:
    raise NotImplementedError("Delete not implemented for tree index.")
```

为什么不支持？因为树节点的内容是**摘要**，而摘要是「对一组子节点的概括」。删掉一个叶子节点之后：
- 它的父节点摘要**就不再准确**（少了那一部分内容）；
- 而要修好它必须**重新调用 LLM 摘要**——这是一次有成本的、可能产生不同结果的生成操作。

所以删除在这里不是「移除一条记录」而是「重新生成一条记录」。选择抛错而不是「静默不修摘要」是对的：后者会让索引里的摘要**逐渐与叶子脱节**，而检索恰恰依赖摘要来选择路径。

对照：`SummaryIndex._delete_node` 能实现，是因为它的索引结构里**没有任何派生内容**——只有 id 列表。

**`IndexGraph` 的三个字段构成了树结构的最小表示。**

```python
all_nodes: Dict[int, str] = field(default_factory=dict)          # 序号 → node id
root_nodes: Dict[int, str] = field(default_factory=dict)         # 序号 → 根 node id
node_id_to_children_ids: Dict[str, List[str]] = field(default_factory=dict)
```

三处：

1. **`all_nodes` 的键是整数序号**而不是 node id。这与 `IndexDict`（键是向量库 id）不同。序号的意义是「节点在构建时的插入顺序」，它让树有了稳定的遍历顺序。
2. **`root_nodes` 单独存**。所以「根」不是靠「没有父节点的节点」推断出来的，而是显式记录的。这避免了「遍历全图找无父者」这类 O(n) 操作。
3. **`node_id_to_children_ids` 只存向下的边**。所以从子到父的方向**没有存储**——要从子找父必须遍历整个字典。这与 b01 的 `NodeRelationship.PARENT` 是两条独立的机制：`IndexGraph` 自己维护父子关系，同时节点上的 `relationships` 也可能有。

`node_id_to_index` 是一个派生属性：

```python
@property
def node_id_to_index(self) -> Dict[str, int]:
    return {node_id: index for index, node_id in self.all_nodes.items()}
```

**每次访问都重建整个反查字典**。所以 `get_index(node)` 与 `get_children(parent)` 都是 O(n)。树检索是逐层往下走的，每层都要调 `get_children`——所以**一次检索会做「层数 × O(n)」的工作**。这是一个真实的性能问题，而修法很直接（把这个映射存成字段）。这里没修，大概因为树的规模通常不大（叶子数 × `num_children` 的层数）。

`insert_under_parent` 与 `insert` 两个方法做的事情几乎相同，但**一个建边、一个不建**：

```python
def insert(self, node, index=None, children_nodes=None) -> None:
    index = index or self.size
    node_id = node.node_id
    self.all_nodes[index] = node_id
    if children_nodes is None:
        children_nodes = []
    children_ids = [n.node_id for n in children_nodes]
    self.node_id_to_children_ids[node_id] = children_ids
```

```python
def insert_under_parent(self, node, parent_node, new_index=None) -> None:
    new_index = new_index or self.size
    if parent_node is None:
        self.root_nodes[new_index] = node.node_id
        self.node_id_to_children_ids[node.node_id] = []
    else:
        if parent_node.node_id not in self.node_id_to_children_ids:
            self.node_id_to_children_ids[parent_node.node_id] = []
        self.node_id_to_children_ids[parent_node.node_id].append(node.node_id)
    self.all_nodes[new_index] = node.node_id
```

两处：

- **`insert` 把 `node` 的子节点记在自己名下**（自底向上建树时用：先有子、再为它们造父）。
- **`insert_under_parent` 把 `node` 记到父节点名下**（增量插入时用：新节点要挂到已有树上），并且 `parent_node is None` 时记进 `root_nodes`。

所以两个方法服务于**两个方向**的构建。注意 `insert` 里 `index = index or self.size` 用真值判断——**传 `index=0` 会被替换成 `self.size`**。这与「序号从 0 开始」冲突：第一个节点无法显式指定序号 0。`insert_under_parent` 里的 `new_index or self.size` 有同样的问题。

**`IndexGraph.get_children` 有个未处理的缺键。**

```python
def get_children(self, parent_node: Optional[BaseNode]) -> Dict[int, str]:
    if parent_node is None:
        return self.root_nodes
    else:
        parent_id = parent_node.node_id
        children_ids = self.node_id_to_children_ids[parent_id]
        return {self.node_id_to_index[child_id]: child_id for child_id in children_ids}
```

**`self.node_id_to_children_ids[parent_id]` 用方括号**——父节点的键不存在就 `KeyError`。而 `insert` 会给每个插入的节点建一个（可能为空的）子列表项，所以正常情况下键都在。但若索引是从旧存档加载的（旧格式没有这个字段），就会缺键。

`get_children(None)` 返回 `self.root_nodes`——即「树的入口」由 `None` 表示。这是一个把「根」编码成「父为 None」的约定，与 `root_nodes` 这个显式字段配合。

**`SummaryIndex` 的构建与 `TreeIndex` 的构建有一个根本差别。**

`SummaryIndex._build_index_from_nodes` **不接受 `docstore`**，因为它不产生新节点。而 `TreeIndex` 必须传 `docstore`，因为它**会产生大量新的摘要节点**——这些节点在构造时由 builder 直接写进 docstore。

所以「构建索引会不会写 docstore」这件事在两个子类之间是不同的：

| | 写 docstore 吗 | 何时 |
|---|---|---|
| `SummaryIndex` | 否 | — |
| `TreeIndex` | 是 | builder 建树时写入摘要节点 |
| `VectorStoreIndex` | 视 `stores_text` | `_add_nodes_to_index` 内部 |

而 `BaseIndex.build_index_from_nodes` **无条件**先写一次 `self._docstore.add_documents(nodes, allow_update=True)`。所以：
- 对 `SummaryIndex`：父类写一次（原始节点），子类不写。正确。
- 对 `TreeIndex`：父类写一次（原始节点），子类再写（摘要节点）。正确。
- 对 `VectorStoreIndex`：**父类的 `build_index_from_nodes` 被覆写了**，所以绕过了那一次无条件写入——正是为了避免重复。

这解释了 c02 为什么必须覆写 `build_index_from_nodes`（不只是为了过滤空节点）。

## 关键取舍

**源码事实：** `SummaryIndex` 的构建不花任何模型调用，索引结构只有一个 id 列表（`IndexList.nodes`）；`TreeIndex` 的构建每层每组做一次 LLM 摘要，且生成的新节点由 builder 直接写进 docstore。

**源码事实：** `ListRetrieverMode` 与 `TreeRetrieverMode` 都是显式字符串值的 `str` 枚举（与 b01 里 `MetadataMode` 一致），因此字符串参数与枚举可直接比较；`as_retriever` 的 `else` 分支拦住拼错的字符串。

**源码事实：** `REQUIRE_TREE_MODES` 含 `SELECT_LEAF`/`SELECT_LEAF_EMBEDDING`/`ROOT`，不含 `ALL_LEAF`；`build_tree=False` 时只能用 `ALL_LEAF`。`as_retriever` 先 `TreeRetrieverMode(retriever_mode)` 转枚举校验，分派本身仍用原值比较。

**源码事实：** `SummaryIndex._delete_node` 先 `docstore.get_nodes` 读出全部节点再重建 id 列表，**依赖 `get_nodes` 返回顺序与输入 id 顺序一致**，且读 docstore 在功能上并不必要（id 列表本身就够过滤）。

**源码事实：** `TreeIndex._delete_node` 抛 `NotImplementedError`——因为摘要节点是派生内容，删叶子后父摘要不再准确，修它需要重新调用 LLM。

**源码事实：** `IndexGraph` 显式存 `root_nodes`（而非靠「无父者」推断），只存向下的边（`node_id_to_children_ids`），从子找父需遍历全表；`node_id_to_index` 是每次访问重建的派生属性，故 `get_children` 与 `get_index` 都是 O(n)。

**源码事实：** `IndexGraph.insert`（自底向上，记子节点到自己名下）与 `insert_under_parent`（增量，把自己记到父节点名下）服务两个方向的构建；两者都用 `index or self.size` 的真值判断，因此**无法显式指定序号 0**。

**源码事实：** 三个索引是否写 docstore 各不相同：`SummaryIndex` 不写、`TreeIndex` 由 builder 写摘要节点、`VectorStoreIndex` 在 `_add_nodes_to_index` 分支里写；这与父类 `build_index_from_nodes` 的无条件写入叠加，正是 c02 必须覆写该方法的原因。

**架构推论：** 「索引需要建多少次 LLM 调用」决定了它的能力边界。`SummaryIndex` 零调用，所以增删改都能精确维护；`TreeIndex` 每次构建都产生不可重建的派生内容，所以删除必须抛错。这条规律可以推广：**凡是索引里存了「生成出来的内容」，它的维护就不再是数据操作而是生成操作**——而生成操作不可逆、不可预期、不能在一次删除里被悄悄触发。

**架构推论：** `SummaryIndex` 的「索引」其实只是「把节点打包」——它没有加速检索，只是提供了一个统一的检索入口。这说明在 RAG 里「索引」至少有三种用途：**加速定位**（向量索引）、**组织上下文**（列表索引：全给或挑几个）、**提供更高层的表示**（树索引：摘要是比原文更短的表示）。三者的构建成本与维护能力完全不同，混在一个 `BaseIndex` 接口下是合理的抽象，但使用者必须知道自己在用哪一种。

**架构推论：** `IndexGraph.node_id_to_index` 每次重建这件事，是「把派生数据做成属性而非字段」的典型代价。好处是不必担心同步（反查表永远与 `all_nodes` 一致），坏处是每次访问 O(n)。判据是**访问频率与数据规模的乘积**：树的层数 × 节点数在多数场景下可接受，所以这个偷懒是划算的。同样的手法在 c02 的 `ref_doc_info` 里也出现（每次遍历全部节点）。

**架构推论：** `ref_doc_info` 的实现在两个索引里逐字重复（只差第一行取 id 的方式），说明这条逻辑本该在 `BaseIndex` 里做成模板方法。它没被抽上去，可能是因为 `BaseIndex` 需要同时照顾「id 从哪来」的差异（`IndexDict.nodes_dict.values()` / `IndexList.nodes` / `IndexGraph.all_nodes.values()`）——这是一个可以用 `node_ids` 属性统一的小差异。三份重复的代价是「改一处忘两处」的风险。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/indices/list/base.py` | `ListRetrieverMode` | 三种检索模式：默认（零模型成本）/ 嵌入 / LLM |
| `llama-index-core/llama_index/core/indices/list/base.py` | `SummaryIndex` | 列表索引主体，构建只收集 id |
| `llama-index-core/llama_index/core/indices/list/base.py` | `SummaryIndex.as_retriever` | 按模式分派到三个检索器，`else` 拦拼错字符串 |
| `llama-index-core/llama_index/core/indices/list/base.py` | `SummaryIndex._build_index_from_nodes` | 逐节点 `add_node`，带 tqdm 迭代 |
| `llama-index-core/llama_index/core/indices/list/base.py` | `SummaryIndex._delete_node` | 读 docstore 全量后重建 id 列表（多一次无谓的读取） |
| `llama-index-core/llama_index/core/indices/list/base.py` | `SummaryIndex.ref_doc_info` | 按 SOURCE 回溯源文档（与向量索引逐字重复） |
| `llama-index-core/llama_index/core/indices/tree/base.py` | `TreeRetrieverMode` | 四种模式：选叶 / 嵌入选叶 / 全叶 / 仅根 |
| `llama-index-core/llama_index/core/indices/tree/base.py` | `REQUIRE_TREE_MODES` | 三个需要真树的模式（`ALL_LEAF` 除外） |
| `llama-index-core/llama_index/core/indices/tree/base.py` | `TreeIndex` | 树索引主体，构建委托 builder、插入委托 inserter |
| `llama-index-core/llama_index/core/indices/tree/base.py` | `TreeIndex._validate_build_tree_required` | `build_tree=False` 时拒绝需树模式 |
| `llama-index-core/llama_index/core/indices/tree/base.py` | `TreeIndex._delete_node` | 抛 `NotImplementedError`（摘要无法增量修复） |
| `llama-index-core/llama_index/core/indices/tree/base.py` | `TreeIndex.ref_doc_info` | 从 `all_nodes` 取 id，其余与列表索引相同 |
| `llama-index-core/llama_index/core/data_structs/data_structs.py` | `IndexList` | 单个 id 列表 |
| `llama-index-core/llama_index/core/data_structs/data_structs.py` | `IndexGraph` | 序号→id、根集合、向下的边；派生反查表为属性 |
| `llama-index-core/llama_index/core/data_structs/data_structs.py` | `IndexGraph.insert_under_parent` | 增量插入：挂到父节点或记为根 |
| `llama-index-core/llama_index/core/data_structs/data_structs.py` | `IndexGraph.get_children` | `parent_node=None` 表示取根；缺键抛 `KeyError` |

## 参考

- [llama-index 源码：indices/list/base.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/indices/list/base.py)
- [llama-index 源码：indices/tree/base.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/indices/tree/base.py)
- [llama-index 文档：索引概览](https://docs.llamaindex.ai/en/stable/module_guides/indexing/)
- [llama-index 文档：树索引](https://docs.llamaindex.ai/en/stable/module_guides/indexing/index_guide/)

## 与主核 / AI 的关系

`SummaryIndex` 与主核（多模态数据库内核）的**顺序扫描**同构：不加任何辅助结构，遍历全部数据。主核里 `seq scan` 是「没有索引时的默认计划」，代价是 O(n) 读；`SummaryIndex` 的 `DEFAULT` 模式也是「全部节点都给下游」，代价是 O(n) 的 token。两者的共同点是**不花钱建结构，每次查询都付全额**。

`TreeIndex` 则对应主核的**多级索引**（B-tree 的每层页面）。主核的中间层页面里存的是**键的边界值**（用于决定往哪个子树走），而树索引的中间节点存的是**子节点内容的摘要**（用于让 LLM 决定往哪个子树走）。这个差别很有意思：主核的导航信息是**精确的**（键边界比较是确定的），而树的导航信息是**生成的**（摘要是对内容的概括，可能误导）。所以树检索本质上是「用一次 LLM 判断换掉一次全量输入」，而这次判断可能出错——这就是为什么它保留了 `ALL_LEAF` 这个「不导航、直接全给」的模式。

`TreeIndex._delete_node` 抛错这条，与主核的**派生数据重建**有一处相同的判据。主核里物化视图（materialized view）的维护也需要重新计算——但因为 SQL 的定义是确定性的，它可以增量或全量重算而得到相同结果。树索引的摘要**不是确定性的**（LLM 每次可能给不同摘要），所以重算会改变索引内容。**这是生成式索引与确定性索引的根本区别**：前者无法「保持一致」，只能「重新生成」。

对 AI 工程的实际意义在于：索引选型的成本模型必须显式。这份实现给出的三条经验可直接用：**把「零模型调用的索引」当成默认起点**（`SummaryIndex` 的构建完全不花钱，适合先跑通链路）、**凡索引里含生成内容就要放弃增删改能力**（树的删除只能抛错）、以及**派生的反查表做成属性而非字段是可接受的偷懒**（前提是规模不大，否则每次 O(n) 会累计）。
