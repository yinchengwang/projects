---
project: llama-index
feature: hierarchical-parser
title: 层级切分与父子关系
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/node_parser/relational/hierarchical.py
    symbols: [HierarchicalNodeParser, get_leaf_nodes, get_root_nodes]
  - path: llama-index-core/llama_index/core/node_parser/relational/hierarchical.py
    symbols: [get_child_nodes, get_deeper_nodes]
---

# 层级切分与父子关系

## 一句话职责

`HierarchicalNodeParser` 用同一份文档同时切出**多套不同粒度的节点**（默认 2048 / 512 / 128 三层），把小粒度的节点挂在大粒度节点下面形成父子关系，最后把各层节点**平铺成一个列表**返回；配套的四个纯函数（`get_root_nodes` / `get_leaf_nodes` / `get_child_nodes` / `get_deeper_nodes`）则在这个平铺列表上按关系游走。

## 机制是什么

这套机制的用途是 RAG 里最实用的一个技巧：**用小块检索，用大块生成**。检索命中一个 128 token 的叶子节点（匹配精确、噪声少），然后把它的父节点（512 或 2048 token）取出来喂给模型（上下文完整）。这与 a02 的句子窗口解决的是同一个张力，区别在于：

| | 句子窗口 | 层级切分 |
|---|---|---|
| 小单位与大单位的关系 | 滑动的「前后 N 句」窗口，**不落库** | 真实的父子节点，**全部落库** |
| 大单位怎么得到 | 由后处理器在检索后算出（`MetadataReplacementPostProcessor` 换 text） | 直接是另一个节点，靠 `PARENT` 关系取回 |
| 内容冗余 | 窗口内容只在元数据里存一份 | **父与子的文本重复存储** |
| 混合粒度 | 只有两级（句 / 窗口） | 任意多层 |

最后一行是它的独特价值：**层数可配**（`chunk_sizes=[2048, 512, 128]` 或任意列表），而句子窗口只有两级。代价是冗余——一份文档切成三层，文本总量大约变成 3 倍（父节点包含子节点的全部内容，且相邻父节点还有重叠）。

机制的关键设计决定是「**平铺返回**」。所有层级的节点混在一个 `List[BaseNode]` 里，靠节点自身的 `PARENT` / `CHILD` 关系维系结构。这样做的好处是：它就是一个普通的节点列表，可以直接塞进任何索引、任何存储；坏处是**「遍历」变成对列表的线性扫描**，而不是沿指针走（见后文 `get_child_nodes` 的实现）。

## 怎么实现的

**三层级的构造在 `from_defaults` 里，是一次「按尺寸生成解析器」的循环。**

```
if chunk_sizes is None:
    chunk_sizes = [2048, 512, 128]

node_parser_ids = [f"chunk_size_{chunk_size}" for chunk_size in chunk_sizes]
node_parser_map = {}
for chunk_size, node_parser_id in zip(chunk_sizes, node_parser_ids):
    node_parser_map[node_parser_id] = SentenceSplitter(
        chunk_size=chunk_size,
        callback_manager=callback_manager,
        chunk_overlap=chunk_overlap,
        include_metadata=include_metadata,
        include_prev_next_rel=include_prev_next_rel,
    )
```

四个要点：

1. **每一层是一个独立的 `SentenceSplitter`，id 由尺寸拼出**（`chunk_size_2048`）。于是「层」这个概念在数据结构上就是「一个解析器 + 一个 id」，而 `node_parser_ids` 的顺序就是层的顺序（第 0 项用于最外层）。
2. **`node_parser_map` 的键是那个拼出来的 id**，不是尺寸、不是序号。这意味着**同名尺寸不能重复**（`[2048, 2048]` 会让 map 里两个 id 撞成一个），也意味着用户可以完全自定义（走 `node_parser_ids` + `node_parser_map` 那条路，用自己实现的解析器）。
3. **`chunk_overlap` 是单一值，所有层共用**（默认 20）。这是一个值得记的不对称：20 token 的重叠对 2048 的块只占 1%，对 128 的块却占 15.6%。**同一份绝对重叠量在不同尺度上意义完全不同**——小块的相对重叠被放大了 16 倍，于是叶子层的有效信息密度明显更低。
4. **回调管理器共用**，于是三层的切分事件都发到同一个 manager 上；`include_metadata` / `include_prev_next_rel` 也逐层下传，所以每一层内部各自有前后关系（下次再看这一点）。

`from_defaults` 的两条互斥校验很干脆：

```
if chunk_sizes is not None:
    raise ValueError("Cannot specify both node_parser_ids and chunk_sizes.")
if node_parser_map is None:
    raise ValueError("Must specify node_parser_map if using node_parser_ids.")
```

**「给了 ids 就不能给 sizes」** 的理由是：`chunk_sizes` 的唯一用途就是「帮我生成 ids 和 map」，既然你已经自己给了那两样，尺寸就没用了。这个校验避免了「两个来源同时指定层数」导致的不一致。

**递归切分是这一页的主体，它的形态是「先切本层，再递归下一层」。**

```
if level >= len(self.node_parser_ids):
    raise ValueError(f"Level {level} is greater than number of text splitters ({len(self.node_parser_ids)}).")

sub_nodes = []
for node in nodes:
    cur_sub_nodes = self.node_parser_map[self.node_parser_ids[level]].get_nodes_from_documents([node])
    if level > 0:
        for sub_node in cur_sub_nodes:
            _add_parent_child_relationship(parent_node=node, child_node=sub_node)
    sub_nodes.extend(cur_sub_nodes)

if level < len(self.node_parser_ids) - 1:
    sub_sub_nodes = self._recursively_get_nodes_from_nodes(sub_nodes, level + 1, show_progress=show_progress)
else:
    sub_sub_nodes = []

return sub_nodes + sub_sub_nodes
```

四处值得逐个看：

**第一，`if level > 0` 这个条件是全篇最关键的一行。**

它意味着**第 0 层切出的节点不加父关系**——因为第 0 层的「父」是原始 `Document`，而 `Document` 不会出现在返回列表里。若加了，每个最外层节点都会有一个指向文档的 `PARENT`，于是 `get_root_nodes` 返回空列表，整个游走体系就塌了（`get_deeper_nodes` 会直接 `raise ValueError("There is no root nodes in given nodes!")`）。

注释写得很清楚：

> Only add relationships if level > 0, since we don't want to add relationships for the top-level document objects that we are splitting

所以「第 0 层节点的父是文档」这个事实**被刻意丢弃**，换来的是「最外层节点就是根」。这是一个为可用性牺牲完整性的取舍——代价是**你无法从任何节点回到原始文档节点本身**（只能靠 `SOURCE` 关系，而那是 a01 父类在每层切分时各自建立的，指向的是那一层的输入节点）。

**第二，`get_nodes_from_documents([node])` 是「把节点当文档再切一次」。** 单元素列表、每次一个。而 `get_nodes_from_documents` 是 a01 里那个带后处理的入口，于是**每一层都完整跑一遍父类后处理**：字符偏移会在这一层的输入节点内重新算、元数据会再合并一次、前后关系会在这一层内部重建。

叠加起来的效果很具体：**同一个偏移字段在不同层级上意义不同**。叶子节点的 `start_char_idx` 是「在它的父节点文本中的位置」，不是「在原始文档中的位置」。下游若把叶子节点的偏移当作文档偏移使用，就会指到错误的字符位置。这个语义漂移没有任何注释说明，是层级切分最容易踩的坑。

**第三，`sub_nodes + sub_sub_nodes` 决定了返回列表的顺序。**

递归的返回值结构是「本层全部 +（下层的全部 + 更下层的全部）」，于是最终列表是 **`[L0 全部, L1 全部, L2 全部]`** ——按层分块，层内按文档顺序。这个顺序有一个实际好处：想取某一层，用切片或 `get_deeper_nodes` 都很快；想让「同层节点相邻」的代码（比如按层做 embedding 批量）也方便。

**第四，递归用「层号 + 1」推进，而终止条件是 `level < len(node_parser_ids) - 1`。** 于是「层数」完全由 `node_parser_ids` 的长度决定，没有任何额外字段。而那个 `level >= len(...)` 的 `ValueError` 是**永远不会被触发**的防御（因为递归在 `len-1` 就停了）——它只在有人直接调 `_recursively_get_nodes_from_nodes` 传错层号时才有用。

**`_add_parent_child_relationship` 是双向写入，且两个方向的数据形状不同。**

```
def _add_parent_child_relationship(parent_node, child_node):
    child_list = parent_node.child_nodes or []
    child_list.append(child_node.as_related_node_info())
    parent_node.relationships[NodeRelationship.CHILD] = child_list

    child_node.relationships[NodeRelationship.PARENT] = parent_node.as_related_node_info()
```

三个要点：

1. **`PARENT` 是单值，`CHILD` 是列表**。这符合树的形状：一个节点只有一个父，但可以有多个子。`NodeRelationship` 这个枚举本身不区分单值/列表，形状由使用方约定——而 `parent_node.child_nodes` 这个属性（在 `schema.py` 里）就是「读 `CHILD` 关系并当成列表」的封装。
2. **`child_list = parent_node.child_nodes or []`** ——`or` 让它同时处理「没有该关系」（`None`）与「列表为空」两种情况。**空列表也是假值**，所以第一次调用时会新建一个列表而非复用那个空列表——无害，但如果别处持有了那个空列表的引用，就会看到「关系没写进去」。
3. **`as_related_node_info()` 每次调用都会重算节点哈希**（a01 里那处优化的对象）。这里每个父子链接调两次（一次为子、一次为父），于是一个有 N 个子节点的父节点要算 2N 次哈希，每次都对**全文 + 元数据**算 sha256。对 2048 token 的父节点 × 大量子节点，这是实打实的开销，且完全重复——同一个父节点被算了 N 次。

**`get_nodes_from_documents` 被覆盖，它绕过了 a01 的后处理。**

```
with self.callback_manager.event(CBEventType.NODE_PARSING, payload={EventPayload.DOCUMENTS: documents}) as event:
    all_nodes: List[BaseNode] = []
    documents_with_progress = get_tqdm_iterable(documents, show_progress, "Parsing documents into nodes")

    # TODO: a bit of a hack rn for tqdm
    for doc in documents_with_progress:
        nodes_from_doc = self._recursively_get_nodes_from_nodes([doc], 0)
        all_nodes.extend(nodes_from_doc)

    event.on_end(payload={EventPayload.NODES: all_nodes})

return all_nodes
```

**它没有调 `super().get_nodes_from_documents(...)`，所以 `_postprocess_parsed_nodes` 在这个外层完全不跑。** 这一点必须明确：跨文档的 PREVIOUS/NEXT **不会**在这里建立（层内的前后关系是由各层自己的 `SentenceSplitter` 建立的，各层 `include_prev_next_rel` 默认 `True`，所以层内是有的）。

那个 `# TODO: a bit of a hack rn for tqdm` 说明作者自己知道「逐文档循环 + 每文档一次递归」不是理想形态——理想形态是一次递归处理整批文档。但那样 tqdm 的进度条就无法逐文档推进。

而 `_parse_nodes` 是一个**什么都不做的占位**：

```
# Unused abstract method
def _parse_nodes(self, nodes, show_progress=False, **kwargs):
    return list(nodes)
```

它是为了满足 `NodeParser` 的抽象方法要求而存在的。也就是说：**这个类违反了契约的语义**——契约说「子类实现 `_parse_nodes`，父类的 `get_nodes_from_documents` 会调它」，这里却是「覆盖 `get_nodes_from_documents`，让 `_parse_nodes` 变成死代码」。

这不是没想清楚，而是「后处理（a01 的 `_postprocess_parsed_nodes`）在这套递归里不适用」逼出来的绕行：偏移跨层无意义（前面讲过）、跨层建前后关系无意义（不同层级的邻居不该连）。绕过之后，`_parse_nodes` 就只剩一个形式上的实现。

**四个遍历函数都是纯函数，只依赖节点里嵌的关系。**

**`get_root_nodes` 与 `get_leaf_nodes` 是相反的两个判据：**

```
def get_leaf_nodes(nodes):
    return [node for node in nodes if NodeRelationship.CHILD not in node.relationships]

def get_root_nodes(nodes):
    return [node for node in nodes if NodeRelationship.PARENT not in node.relationships]
```

**「没有 CHILD」= 叶子，「没有 PARENT」= 根。** 判据都在 `relationships` 字典里查键，而不是读 `child_nodes` / `parent_node` 属性。这个写法有一个细微的好处：**不依赖属性的类型转换**（`child_nodes` 属性在「关系缺失」与「关系存在但值非法」两种情况下行为不同，而查键不会）。但这也意味着**关系的形状完全靠约定**——若有人写了 `CHILD` 但值不是列表，`get_leaf_nodes` 仍会认为它不是叶子（它只看键是否存在）。

这两个函数对「单层输入」的行为值得注意：一份只切一层（`chunk_sizes=[512]`）的节点列表，每个节点既没有 CHILD 也没有 PARENT → **同时是根也是叶子**。这不是 bug，而是这套判据的必然结果。

**`get_child_nodes` 是本页最值得细看的一处，因为它必须在列表里做一次查找。**

```
children_ids = []
for node in nodes:
    if NodeRelationship.CHILD not in node.relationships:
        continue
    children_ids.extend([r.node_id for r in (node.child_nodes or [])])

child_nodes = []
for candidate_node in all_nodes:
    if candidate_node.node_id not in children_ids:
        continue
    child_nodes.append(candidate_node)
return child_nodes
```

**为什么要 `all_nodes` 参数？** 因为 `RelatedNodeInfo` 只带 `node_id` / `node_type` / `metadata` / `hash`——**没有正文、没有子节点列表**。也就是说关系是一条「指针」，要拿到真实节点必须回到全量列表里查。这就是 a01 里 `build_nodes_from_splits` 那条注释的另一面：「`as_related_node_info()` 开销大但也只带这些字段」，所以「遍历必须靠列表」。

两处效率问题：**`children_ids` 是 `list` 而成员判断用的是 `not in children_ids`**——0 到 N 个 id 的线性查找；**外层每个候选节点都做一次成员判断**，于是整体是 O(N·M)。用一个 `set` 就能降到 O(N+M)。对一个 10 万节点的文档，这个差别是「秒级」与「分钟级」。

另一处语义问题：**返回节点的顺序是 `all_nodes` 的顺序，不是父子关系的顺序。** 于是「按 `CHILD` 列表里的 id 顺序」这个信息丢了——树的孩子顺序变成了「在平铺列表里的出现顺序」。而平铺列表是「按层」排的，层内是文档顺序，所以对同一父节点的孩子通常恰好也是文档顺序——**它成立，但靠的是巧合而非保证**。

**`get_deeper_nodes` 是「从根往下走 depth 步」。**

```
if depth < 0:
    raise ValueError("Depth cannot be a negative number!")
root_nodes = get_root_nodes(nodes)
if not root_nodes:
    raise ValueError("There is no root nodes in given nodes!")

deeper_nodes = root_nodes
for _ in range(depth):
    deeper_nodes = get_child_nodes(deeper_nodes, nodes)
return deeper_nodes
```

三点：

1. **`depth=0` 返回根**，`depth=1` 返回根的直接子节点。默认值是 `1`，也就是说「不传参数」得到的是第二层——一个不直观的默认（多数人会觉得默认是 0 或「全部」）。
2. **`if not root_nodes: raise`** 这条是前面提到的那处依赖的直接体现：因为第 0 层节点刻意不带 `PARENT`，「没有根」正常情况下不会发生；但若输入是**某一层的子集**（例如从存储里只取回了叶子节点），根就为空，于是 `get_deeper_nodes` 直接抛错而不是返回空列表。**「返回空」与「报错」的区分在这里是「输入不完整」的判定**——作者认为传进来一批无根节点是调用方的错误。
3. **循环里每次都重算一次「从 `nodes` 里按 id 查」**，于是 depth 层的复杂度是 `depth × O(N·M)`。深层次遍历会明显变慢。

值得再强调一次这组函数的定位：**它们是模块级函数而不是 `Graph` 对象的方法**，也不需要构造任何遍历器。整个层级结构就是「一个平铺列表 + 每个节点里的关系字段」，没有任何额外的索引。这让它极易被序列化与传输（就是一个节点数组），也让遍历必须做线性扫描——**一个明确的「结构简单 vs 查询快」的取舍**。

**还有一件必须点明的事实：父子关系里的 `PARENT` 是单值，而 `CHILD` 是列表，但两者的「值形状」并不对称地反映在类型上。**

`schema.py` 里 `NodeRelationship` 的定义是一组枚举（`SOURCE` / `PREVIOUS` / `NEXT` / `PARENT` / `CHILD`），`BaseNode.relationships` 是 `Dict[NodeRelationship, RelatedNodeInfo | List[RelatedNodeInfo]]`——**一个联合类型**。于是「`PARENT` 是单值、`CHILD` 是列表」这条约定**没有类型层面的保证**，只能靠使用方的自觉（`parent_node.child_nodes` 属性会断言它是列表，`source_node` 属性会断言它是单值）。这是这套关系模型最薄弱的地方：所有关系的读写都经过属性访问器做隐式转换，而转换失败的表现是运行时异常或静默错值。

## 关键取舍

**源码事实：** `from_defaults` 默认 `chunk_sizes=[2048, 512, 128]`，为每个尺寸生成一个 id（`chunk_size_<n>`）并构造一个独立的 `SentenceSplitter`；`chunk_overlap` 是单一值、所有层共用（默认 20）。

**源码事实：** `from_defaults` 拒绝「同时给 `node_parser_ids` 与 `chunk_sizes`」，且在给 `node_parser_ids` 时要求同时给 `node_parser_map`。

**源码事实：** 递归切分只在 `level > 0` 时建立父子关系，于是第 0 层节点的父（原始 `Document`）被刻意丢弃，最外层节点直接成为「根」。

**源码事实：** 每一层通过 `get_nodes_from_documents([node])` 切分，因此**每层都完整跑一遍 a01 的后处理**——字符偏移是「相对该层父节点」的，不是相对原始文档的。

**源码事实：** 返回列表的顺序是「按层分组、层内按文档顺序」（`sub_nodes + sub_sub_nodes`），层数完全由 `node_parser_ids` 长度决定；`level >= len(...)` 的 `ValueError` 在正常调用路径上不可达。

**源码事实：** `_add_parent_child_relationship` 写双向关系，`PARENT` 单值、`CHILD` 列表；每次调用 `as_related_node_info()` 都会重算节点哈希，故一个 N 子节点的父节点被算 N 次。

**源码事实：** `get_nodes_from_documents` 被覆盖，**绕过 `_postprocess_parsed_nodes`**，于是跨文档的 PREVIOUS/NEXT 在外层不建立；`_parse_nodes` 只剩一个 `return list(nodes)` 的占位（源码注释自称 "Unused abstract method"）。

**源码事实：** `get_root_nodes` / `get_leaf_nodes` 的判据是「关系字典里有没有那个键」（`PARENT` / `CHILD`），单层输入时同一批节点同时满足两者。

**源码事实：** `get_child_nodes` 必须传入 `all_nodes` 才能在列表里解析 id（`RelatedNodeInfo` 不带正文）；`children_ids` 是 `list` 且成员判断用 `not in`，整体复杂度 O(N·M)；返回顺序是 `all_nodes` 的顺序而非 `CHILD` 列表的顺序。

**源码事实：** `get_deeper_nodes` 默认 `depth=1`（返回根的**直接子节点**）；`depth < 0` 报错；根为空时报 `ValueError` 而非返回空列表。

**架构推论：** 「平铺列表 + 内嵌关系」是「可传输性 vs 可遍历性」的取舍。这个结构就是一组普通节点，能直接塞进任何索引与任何存储，代价是每次游走都要线性扫描并靠 id 反查。若换成真正的图对象（带 id→node 的哈希索引），遍历会快得多，但「能存进任意存储」这个最重要的性质就丢了——而它才是这个设计能被广泛使用的原因。

**架构推论：** 第 0 层刻意不加父关系，把「根」定义成「最外层节点」而非「文档」。这让四个遍历函数有了明确的起点，代价是层级结构丢失了「这些节点来自同一份文档」这个显式链接（只能靠 `SOURCE` 关系间接知道）。任何「为了遍历方便而裁剪信息」的设计都有同一笔账。

**架构推论：** 「小块检索、大块生成」的代价是文本重复存储。三层的文档会让存储量变成约 3 倍，且这个倍数随层数线性增长。之所以值得，是因为它把「检索精度」与「生成上下文」这两个互相冲突的目标从**运行期算法**（句子窗口那种后处理）变成了**存储期的结构**——于是检索时零成本。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/node_parser/relational/hierarchical.py` | `HierarchicalNodeParser` | 多层级切分主体：按尺寸生成解析器、递归切分、平铺返回 |
| `llama-index-core/llama_index/core/node_parser/relational/hierarchical.py` | `_recursively_get_nodes_from_nodes` | 逐层切分；只在 `level > 0` 时建父子关系；返回「本层 + 更深层」 |
| `llama-index-core/llama_index/core/node_parser/relational/hierarchical.py` | `_add_parent_child_relationship` | 双向写关系：父的 `CHILD` 是列表、子的 `PARENT` 是单值 |
| `llama-index-core/llama_index/core/node_parser/relational/hierarchical.py` | `get_root_nodes` / `get_leaf_nodes` | 「没有 `PARENT`」= 根；「没有 `CHILD`」= 叶子 |
| `llama-index-core/llama_index/core/node_parser/relational/hierarchical.py` | `get_child_nodes` | 必须靠 `all_nodes` 反查 id（关系不携带正文），O(N·M) |
| `llama-index-core/llama_index/core/node_parser/relational/hierarchical.py` | `get_deeper_nodes` | 从根往下走 `depth` 步；根为空时报错 |
| `llama-index-core/llama_index/core/node_parser/text/sentence.py` | `SentenceSplitter` | 每一层的实际切分器，按各自的 `chunk_size` 与共用的 `chunk_overlap` |
| `llama-index-core/llama_index/core/schema.py` | `NodeRelationship` / `RelatedNodeInfo` / `child_nodes` | 关系枚举与「只带 id / 类型 / 元数据 / 哈希」的指针值类型 |

## 参考

- [llama-index 源码：node_parser/relational/hierarchical.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/node_parser/relational/hierarchical.py)
- [llama-index 文档：节点与切分器](https://docs.llamaindex.ai/en/stable/module_guides/loading/node_parsers/)
- [llama-index 文档：AutoMergingRetriever（层级节点的典型用法）](https://docs.llamaindex.ai/en/stable/examples/retrievers/auto_merging_retriever/)
- [llama-index 文档：SentenceSplitter](https://docs.llamaindex.ai/en/stable/module_guides/loading/node_parsers/modules/)

## 与主核 / AI 的关系

「同一份数据在不同粒度上都要可查」在主核里对应的是 **B-tree 的多层索引结构**：叶子页存实际数据，内层页存指向孩子的指针，遍历从根开始，按层下降。这里的层级节点几乎是同一张结构图的另一种实现，四条经验。

**第一，「平铺存储 + 内嵌关系」与「图结构 + 遍历器」的取舍要看使用方式。** 这里选了平铺，因为它必须能塞进任何索引/存储；代价是遍历 O(N·M)。主核的 B-tree 选了相反的方向（页内有 slot 数组、按页号跳转），因为它的遍历频率极高。**判断标准是「数据更多地被遍历，还是更多地被搬运」。** 节点要被存进各种向量库、被序列化传输，所以搬运优先。

**第二，层级数据必须明确「偏移相对谁」。** 这里叶子节点的 `start_char_idx` 相对父节点而非原始文档，而这个语义漂移没有注释。主核里做「多粒度副本」时同样会遇到：同一个位置字段在不同层指向不同坐标系。正确做法是给每一层带一个「坐标系标识」（例如文本总长 + 层号），否则下游的错误引用会静默发生。

**第三，「重叠量是相对量而非绝对量」。** `chunk_overlap=20` 对 2048 的块是 1%、对 128 的块是 15.6%。同一参数在跨尺度使用时意义会差一个数量级。主核里凡是把「固定尺寸参数」复用到不同粒度的场景（页大小、批大小、超时），都要问一句「这个绝对量在新尺度上还算合理吗」。

**第四，关系的单值/列表形状必须有类型保证。** 这里 `PARENT` 单值、`CHILD` 列表，但 `relationships` 的类型是「单值或列表」的联合，靠属性访问器隐式转换。主核里若用类似的通用关系表（一个 `relationships: Map<Key, Value|List<Value>>`），同样会失去形状保证，而形状错误的表现通常是「取到列表却当单值用」这类运行时错。要么给每个关系键固定类型，要么像这里一样把所有访问都收归到少数几个属性里——但后者要求**没有任何代码直接读写原始字段**，而这里的 `_add_parent_child_relationship` 恰恰是直接写原始字段的。

还有一条更实用的：**「小块检索、大块生成」把冲突从算法挪到了存储。** 句子窗口用后处理器在检索时拼上下文（运行期成本），层级切分在写入时就把大块准备好（存储成本）。哪种更好取决于「检索次数 vs 写入次数」——检索远多于写入时，用存储换运行期是对的。
