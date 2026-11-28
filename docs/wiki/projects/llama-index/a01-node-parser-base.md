---
project: llama-index
feature: node-parser-base
title: 节点解析器契约与 id 推导
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/node_parser/interface.py
    symbols: [NodeParser, TextSplitter, MetadataAwareTextSplitter]
  - path: llama-index-core/llama_index/core/node_parser/node_utils.py
    symbols: [default_id_func, build_nodes_from_splits, IdFuncCallable]
---

# 节点解析器契约与 id 推导

## 一句话职责

`NodeParser` 用一个抽象方法 `_parse_nodes` 加一份公共后处理把「文档 → 节点」这件事定死：**子类只管切，父类负责补全**——补 `SOURCE` 关系、算字符偏移、合并父文档元数据、必要时建前后关系；`node_utils.build_nodes_from_splits` 则是所有文本切分器共用的「片段 → 节点」工厂，节点 id 由可替换的 `id_func` 生成，默认实现是随机 uuid。

## 机制是什么

这套契约的形状是「一个抽象点 + 一堆公共后处理」，而这个分配非常明确：

| 由子类负责 | 由父类负责 |
|---|---|
| `_parse_nodes`：怎么把内容切碎 | 回调事件（`NODE_PARSING`）的开启与收尾 |
| 产生什么类型的节点 | 每个节点的 `SOURCE` 关系 |
| 切出几段 | 每段的 `start_char_idx` / `end_char_idx` |
| | 父文档元数据合并（`include_metadata`） |
| | 前后节点关系（`include_prev_next_rel`） |

**所有子类都不必自己去填 `SOURCE`、不必自己算偏移**——这两件事在实践中极易漏，而且漏了不会立刻报错（只会在检索、引用高亮、上下文扩展时表现异常）。把「易漏但必要」的字段收归父类，是这套契约最重要的设计决定。

四个字段描述了契约的可配置面：

```
include_metadata: bool = True        # 是否把父文档元数据并进子节点
include_prev_next_rel: bool = True   # 是否建立前后关系
callback_manager: CallbackManager    # 事件总线
id_func: IdFuncCallable = default_id_func   # 节点 id 生成器
```

`id_func` 的签名是 `(i: int, doc: BaseNode) -> str`——**拿到的是「第几段」与「整份文档」，而不是段文本**。这个参数选择决定了「做确定性 id」这件事能做到什么程度：可以做「文档 id + 序号」的拼接，但不能做「内容哈希」。这一点后面会展开，它是这一页最有意思的取舍。

## 怎么实现的

**`_parse_nodes` 是唯一的抽象点，异步版本默认同步转发。**

```
@abstractmethod
def _parse_nodes(self, nodes, show_progress=False, **kwargs) -> List[BaseNode]: ...

async def _aparse_nodes(self, nodes, show_progress=False, **kwargs) -> List[BaseNode]:
    return self._parse_nodes(nodes, show_progress=show_progress, **kwargs)
```

于是「异步支持」不是另一份实现，而是「默认把同步版本当异步用」——一个子类只写一份逻辑就同时具备两种入口。需要真异步（例如切分要调远端服务）的子类才覆盖 `_aparse_nodes`。这与 langchain 的 Runnable 契约是同一种思路（同步优先、异步降级），只是这里降级得更彻底：连线程池都不用，直接在同一线程里跑。

对外的两个入口（`get_nodes_from_documents` / `aget_nodes_from_documents`）都是「开事件 → 调 `_parse_nodes` → 跑后处理 → 关事件」：

```
doc_id_to_document = {doc.id_: doc for doc in documents}
with self.callback_manager.event(CBEventType.NODE_PARSING, payload={EventPayload.DOCUMENTS: documents}) as event:
    nodes = self._parse_nodes(documents, show_progress=show_progress, **kwargs)
    nodes = self._postprocess_parsed_nodes(nodes, doc_id_to_document)
    event.on_end({EventPayload.NODES: nodes})
return nodes
```

**注意后处理拿到的是 `doc_id_to_document` 这个映射**，而不是节点自己携带的文档引用。这是刻意的：后处理需要**原始文档的全文**来算字符偏移，而节点上只有 `SOURCE` 关系（含 id、类型、metadata、hash，但**不含正文**）。用映射查回来的做法让「后处理必须能看见原文」成为硬前提——也正是它让 `source_node.text` 这条路径不被依赖。

**后处理的四个动作按顺序执行，顺序有讲究。**

```
doc_search_positions: Dict[str, int] = {}
for i, node in enumerate(nodes):
    parent_doc = parent_doc_map.get(node.ref_doc_id or "", None)
    parent_node = node.source_node
    ...
```

**第一件：补 `SOURCE` 关系。**

```
if parent_doc is not None:
    if parent_doc.source_node is not None:
        node.relationships.update({NodeRelationship.SOURCE: parent_doc.source_node})
```

只有父文档**自己也有 `source_node`** 时才覆写。也就是说「文档是被某个东西产生的」（例如从网页或其他文档派生）时，这个更上游的关系会被继承到子节点上。若父文档没有来源，则保留 `build_nodes_from_splits` 建的那条「指向父文档本身」的关系。

**第二件：算字符偏移，这是全段最值得读的一处。**

```
doc_id = node.ref_doc_id or ""
search_start = doc_search_positions.get(doc_id, 0)
node_content = node.get_content(metadata_mode=MetadataMode.NONE)
start_char_idx = parent_doc.text.find(node_content, search_start)
if start_char_idx >= 0 and isinstance(node, TextNode):
    node.start_char_idx = start_char_idx
    node.end_char_idx = start_char_idx + len(node_content)
    doc_search_positions[doc_id] = start_char_idx + 1
```

四点：

1. **用 `find` 而不是记账**。切分器本来知道每段在原文的位置，但它把 `List[str]` 交出去就丢了偏移。于是父类用「在原文里从上次位置往后找这段文本」来重建偏移。这是一次**有损信息的逆向重建**。
2. **`doc_search_positions` 每文档一份，且推进 `start_char_idx + 1`（不是 `+ len`）**。注释把理由写得很直接：「我们追踪 START 位置而不是 end 位置，以便正确支持重叠块」。若推进到 `start + len`，一个与下一块重叠的块（其起点落在上一块内部）就会被 `find` 跳过，偏移算错。`+1` 是「至少往后挪一格以免死循环」与「仍能落在重叠区域内」的折中。
3. **取的是 `MetadataMode.NONE` 的内容**。因为原文里没有元数据字符串，要定位必须用纯净正文。若用 `MetadataMode.ALL`，`find` 永远失败（−1），偏移就永远不写。
4. **`find` 返回 −1 时静默跳过**。切分器若对文本做了任何改写（normalize、strip、大小写变化），重建就会失败，而这个失败**不可见**——节点只是没有偏移。这是这一层的最大脆弱点。

**第三件：合并父文档与内层节点的元数据，方向是「子节点优先」。**

```
if self.include_metadata:
    node.metadata = {**parent_doc.metadata, **node.metadata}
```

先展开父文档元数据、再展开节点自己的，于是**同名键取节点自身值**。这个方向的理由很实在：节点级的元数据是切分器专门为这一段设置的（例如 Markdown 切分器写入的 `header_path`），它比文档级的通用字段更具体、更该胜出。

紧接着还有一层「父节点元数据」的合并，但写法有点绕：

```
if parent_node is not None:
    if self.include_metadata:
        parent_metadata = parent_node.metadata
        combined_metadata = {**parent_metadata, **node.metadata}
        node.metadata.update(combined_metadata)
```

`node.metadata.update(combined_metadata)` 里 `combined_metadata` 已经含了 `node.metadata` 的全部键，所以**这次 update 的实际效果只有「把 `parent_metadata` 里节点没有的键补进来」**。虽然展开顺序有两层，净效果仍是「不覆盖节点已有的值」。写法冗余（可以写成 `{**parent_metadata, **node.metadata}` 直接赋值），但语义正确。

**第四件：建前后关系，判据是「同一个来源」。**

```
if self.include_prev_next_rel:
    if (i > 0 and node.source_node and nodes[i - 1].source_node
        and nodes[i - 1].source_node.node_id == node.source_node.node_id):
        node.relationships[NodeRelationship.PREVIOUS] = nodes[i - 1].as_related_node_info()
    if (i < len(nodes) - 1 and node.source_node and nodes[i + 1].source_node
        and nodes[i + 1].source_node.node_id == node.source_node.node_id):
        node.relationships[NodeRelationship.NEXT] = nodes[i + 1].as_related_node_info()
```

那个四重条件里最关键的一条是**「相邻两个节点的 `source_node` id 相同」**。若不做这个检查，一批文档切成节点后，文档 A 的最后一段就会把文档 B 的第一段当成「下一段」——跨文档的假关系。这个检查让关系只在「同一个父文档内部」成立。

但它有一个未覆盖的边界：**同一批里传入两份内容完全相同的文档**时，两份文档的节点在列表里相邻且 `source_node` 不同，所以不会建错关系。这个设计是对的。

`as_related_node_info()` 每次调用都会读 `self.hash`：

```
def as_related_node_info(self) -> RelatedNodeInfo:
    return RelatedNodeInfo(node_id=self.node_id, node_type=self.get_type(), metadata=self.metadata, hash=self.hash)
```

而 `hash` 是「对文本与元数据算 sha256」的惰性属性。于是每建一条关系都会触发一次哈希计算——除非已被缓存。这一点在 `build_nodes_from_splits` 里被专门优化掉了（见下）。

**`TextSplitter` 把「切」抽象成一个方法，其余全包。**

```
class TextSplitter(NodeParser):
    @abstractmethod
    def split_text(self, text: str) -> List[str]: ...

    def split_texts(self, texts: List[str]) -> List[str]:
        nested_texts = [self.split_text(text) for text in texts]
        return [item for sublist in nested_texts for item in sublist]

    def _parse_nodes(self, nodes, show_progress=False, **kwargs):
        all_nodes = []
        nodes_with_progress = get_tqdm_iterable(nodes, show_progress, "Parsing nodes")
        for node in nodes_with_progress:
            splits = self.split_text(node.get_content())
            all_nodes.extend(build_nodes_from_splits(splits, node, id_func=self.id_func))
        return all_nodes
```

**子类只要实现 `split_text(text) -> List[str]`，就自动获得「批量切文本」「遍历文档切节点」两件事。** 这是整套结构里最省力的地方：`TokenTextSplitter` / `SentenceSplitter` / `CodeSplitter` 全都只写 `split_text`（再加上一些预算相关的辅助方法）。

注意 `node.get_content()` **实际用的是 `MetadataMode.NONE`**：`BaseNode.get_content` 是抽象方法、其默认值是 `ALL`，但 `Node` / `TextNode` 的覆盖实现把默认值改成了 `NONE`，于是 `TextSplitter` 拿到的永远是**不含元数据渲染的纯净正文**。这也解释了 `MetadataAwareTextSplitter` 与它的真实差别：不是「换了个模式取正文」（两处都取 `NONE`），而是**多拿到一份 `metadata_str` 并据此扣减预算**——它会显式传入 `metadata_mode=MetadataMode.NONE`，含义是「明确声明」而非「换一种取法」。

`get_tqdm_iterable` 是进度条的注入点，于是「显示进度」这件事对每个文本切分器都是白送的。

**`MetadataAwareTextSplitter` 走的是「先算元数据占用、再扣预算切正文」这条路。**

```
def _parse_nodes(self, nodes, show_progress=False, **kwargs):
    for node in nodes_with_progress:
        metadata_str = self._get_metadata_str(node)
        splits = self.split_text_metadata_aware(
            node.get_content(metadata_mode=MetadataMode.NONE), metadata_str=metadata_str
        )
        all_nodes.extend(build_nodes_from_splits(splits, node, id_func=self.id_func))
```

与父类的实质差别是一处：**它多算出一份 `metadata_str` 并把它交给切分器**。正文本身两边都是纯净的（`NONE`），这里的显式传参只是把「要纯净正文」这件事写死，不依赖具体子类的默认值。于是切分器知道了「待切正文」与「将来会随块一起被渲染进去的元数据」，可以只按正文长度做切分、同时从预算里扣掉元数据占用的那一份——**切分边界不受元数据污染，但块的实际 token 数仍然在预算内**。这正是 `TokenTextSplitter.split_text_metadata_aware` 里 `chunk_size - metadata_len` 的来源。

`_get_metadata_str` 取的是**两种模式下较长的那一份**：

```
embed_metadata_str = node.get_metadata_str(mode=MetadataMode.EMBED)
llm_metadata_str = node.get_metadata_str(mode=MetadataMode.LLM)
metadata_str = embed_metadata_str if len(embed_metadata_str) > len(llm_metadata_str) else llm_metadata_str
```

注释说「用最长的元数据字符串做切分」。理由是**预算必须按最坏情况扣**：元数据在「嵌入模式」与「LLM 模式」下渲染的键集合不同（`excluded_embed_metadata_keys` 与 `excluded_llm_metadata_keys` 各排除一批），若按较短的那份扣预算，另一种模式下的实际块会超限。取最长的等于「两种模式的预算都够用」。

**`__init_subclass__` 那段代码是为了修一个文档问题。**

```
@classmethod? no — 是普通方法
def __init_subclass__(cls, *args, **kwargs):
    super().__init_subclass__(*args, **kwargs)
    user_init = getattr(cls, "__pydantic_init__", None)
    if user_init is None: return
    if getattr(user_init, "__module__", "").startswith("pydantic"): return
    doc = getattr(user_init, "__doc__", None)
    if not doc: return
    try: cls.__init__.__doc__ = doc
    except (AttributeError, TypeError): pass
```

它做的是「把子类手写的 `__init__` 文档字符串搬到 pydantic 生成的 `__init__` 上」，于是 `help(SomeSplitter)` 能看到用户写的参数说明，而不是 pydantic 生成的空 docstring。三处防御（没有 `__pydantic_init__`、docstring 来自 pydantic 自己、没有 docstring）都是在避免把自动生成的东西再搬一遍。这是一个**纯粹为了人类可读性**的钩子，不影响任何行为。

**`build_nodes_from_splits` 是节点工厂，它的第一步是一处性能优化。**

```
ref_doc = ref_doc or document
id_func = id_func or default_id_func
nodes: List[TextNode] = []
"""Calling as_related_node_info() on a document recomputes the hash for the whole text and metadata"""
"""It is not that bad, when creating relationships between the nodes, but is terrible when adding a relationship"""
"""between the node and a document, hence we create the relationship only once here and pass it to the nodes"""
relationships = {NodeRelationship.SOURCE: ref_doc.as_related_node_info()}
```

**那段三行注释是整份文件里最有价值的注释**：`as_related_node_info()` 会重算整份文档的 sha256，若对每个节点各调一次，一份 100 段的文档就要算 100 次全文哈希——O(N·L)。所以在循环外建一次 `RelatedNodeInfo` 再共享，复杂度变成 O(L)。

这个优化之所以能成立，靠的是**所有节点共享同一个父文档**这个事实。若每个节点来自不同文档，就必须各建一份。所以「批量切同一份文档」这个用法假设被写进了实现里。

**三支分支各造一种节点类型。**

```
if isinstance(document, ImageDocument):
    image_node = ImageNode(id_=id_func(i, document), text=text_chunk, embedding=document.embedding,
        image=document.image, image_path=document.image_path, image_url=document.image_url, ..., relationships=relationships)
elif isinstance(document, Document):      # → TextNode
elif isinstance(document, TextNode):      # → TextNode
else:
    raise ValueError(f"Unknown document type: {type(document)}")
```

三处值得注意：

1. **`ImageDocument` 切成 `ImageNode`**，且**每个分块都带上完整的图像引用**（`image` / `image_path` / `image_url`）。这意味着一张图被切成 N 段文本时，N 个节点都指向同一张图——检索命中任一段，都能取回原图。这是多模态检索的基础约定。
2. **`Document` 与 `TextNode` 两支产出的都是 `TextNode`，且代码几乎逐行相同**。两者只在语义上不同（前者是「文档级」，后者是「已被切过一次的节点再切」）。这个重复是真实的代码冗余，但它让 `isinstance` 链路保持显式。
3. **未知类型抛 `ValueError`**，而不是猜测或跳过。这是「不要在数据形状上做猜测」的正确态度。

**所有分支都把文档的「渲染配置」复制到子节点**（`excluded_embed_metadata_keys`、`excluded_llm_metadata_keys`、`metadata_seperator`、`metadata_template`、`text_template`）。这一组字段决定了「元数据怎么渲染进节点文本」，若子节点用默认值而不继承，父文档上设的排除规则就会失效——那些被排除的键会突然出现在块的渲染结果里。**继承这五项是必要的，而不是顺手。**

**id 的推导是这一页的收尾，也是全篇最需要想清楚的地方。**

```
@runtime_checkable
class IdFuncCallable(Protocol):
    def __call__(self, i: int, doc: BaseNode) -> str: ...

def default_id_func(i: int, doc: BaseNode) -> str:
    return str(uuid.uuid4())
```

**默认实现完全忽略两个参数，返回随机 uuid。** 这个选择有三个直接后果：

1. **同一份文档切两次得到不同的 id**。于是「重复摄取同一份文档」不会因为 id 相同而被去重——去重必须靠别的手段（内容哈希或 docstore 的比对），这解释了 llama-index 为什么需要专门的一套 ingestion 去重机制。
2. **id 无法从内容推出**，因此也不具备「跨进程可预测性」：两个独立进程对同一份文档切出的节点 id 完全不同。
3. **完全不冲突**，这是它唯一的优点，而且是压倒性的优点——不需要任何去重、加密、命名空间逻辑。

`id_func` 的类型是 `Annotated[Callable, Field(), BeforeValidator(_validate_id_func), WithJsonSchema(...), PlainSerializer(_serialize_id_func)]`。三件附加物各自解决一个问题：

- **`BeforeValidator(_validate_id_func)`**：传 `None` 时替换成 `default_id_func`。于是「不写 `id_func`」与「写 `id_func=None`」等价，不必在构造器里再判一次。
- **`WithJsonSchema({"type": "string"}, mode=...)`** 双份（validation 与 serialization 各一份）：让 pydantic 这个**函数类型字段**在 JSON Schema 里显示成字符串。没有它，函数字段无法被序列化成 schema，任何 `model_json_schema()` 都会失败。
- **`PlainSerializer(_serialize_id_func)`**：序列化时只写函数名。

```
def _serialize_id_func(f: Callable) -> Any:
    return {"id_func_name": f"{f.__name__}", "title": "id_func"}
```

**这个序列化是单向的**——把函数写成 `{"id_func_name": "...", "title": "id_func"}`，但没有反序列化器。也就是说一个含自定义 `id_func` 的切分器**无法从 JSON 还原**。这是"可序列化能力"在这里的一个明确缺口：能导出（用于展示配置）但不能重放。对一个「切分器配置」而言这个缺口可以接受，但对工作流编排就不行。

而 `(i, doc)` 这个签名是**唯一能表达「确定性 id」的地方**：用户可以传一个 `lambda i, doc: f"{doc.id_}_{i}"` 得到「文档 id + 序号」的稳定 id。但**用户拿不到片段文本**，所以做不到 `sha256(chunk_text)` 这种「内容即身份」的 id。若确实要做内容哈希 id，必须自己在切分之后改写节点 id。

这个限制很值得记：**参数签名决定了下游能做到什么**。这里的签名支持的正是「按位置编号」这一种确定性策略，而它恰好也是最常用于「重复摄取去重」的策略——用 `doc_id + i` 做 id，重跑同一份文档会得到同样的 id，于是 upsert 天然幂等。

## 关键取舍

**源码事实：** `NodeParser` 只有一个抽象方法 `_parse_nodes`，`_aparse_nodes` 默认同步转发；四字段 `include_metadata` / `include_prev_next_rel` / `callback_manager` / `id_func` 都有默认值。

**源码事实：** 字符偏移靠「在原文里从上次位置往后 `find` 这段文本」重建，每文档维护一个游标且推进到 `start + 1`（而非 `start + len`），以便重叠块也能被找到；取的是 `MetadataMode.NONE` 的正文；`find` 失败时静默跳过、不写偏移。

**源码事实：** 元数据合并是两层、方向都是「节点自身优先」：先 `{**parent_doc.metadata, **node.metadata}`，再把父节点的键补进来。`include_metadata` 为假时两处都不做。

**源码事实：** 前后关系的建立要求相邻两个节点的 `source_node.node_id` 相同，因此不会跨文档误连。

**源码事实：** `TextSplitter` 把切分抽象成 `split_text(text) -> List[str]`，并自动提供 `split_texts` 与遍历文档的 `_parse_nodes`；它传给 `split_text` 的是**纯净正文**——`BaseNode.get_content` 的抽象签名默认 `MetadataMode.ALL`，但 `Node` / `TextNode` 的覆盖实现默认 `MetadataMode.NONE`，实际调用走的是后者。

**源码事实：** `MetadataAwareTextSplitter` 的 `_parse_nodes` 比父类多做的一件事是**算出 `metadata_str` 并交给切分器**（正文本就都是 `NONE`）；`_get_metadata_str` 取 EMBED 与 LLM 两种渲染里**较长**的那一份，以便按最坏情况扣预算。

**源码事实：** `build_nodes_from_splits` 在循环外只建一次 `SOURCE` 关系并共享给所有节点，注释明确说明理由是 `as_related_node_info()` 会重算整份文档的哈希（逐节点调用是 O(N·L)）。

**源码事实：** 工厂按输入类型分三支：`ImageDocument` → `ImageNode`（每段都带完整图像引用），`Document` / `TextNode` → `TextNode`（两支代码近乎重复），其余抛 `ValueError`；所有分支都复制父文档的五个渲染/排除配置字段。

**源码事实：** `default_id_func` 忽略两个参数并返回 `str(uuid.uuid4())`；`IdFuncCallable` 签名是 `(i, doc) -> str`，因此只能做「按位置编号」类的确定性 id，做不到内容哈希 id。

**源码事实：** `IdFuncCallable` 用 `Annotated` 挂了三件附加物：`BeforeValidator` 把 `None` 归一成默认函数、两份 `WithJsonSchema` 让函数字段在 schema 里显示为字符串、`PlainSerializer` 只写函数名——**没有对应的反序列化器**，故含自定义 `id_func` 的切分器无法从 JSON 还原。

**架构推论：** 「子类只管切、父类补全」这个分工把「易漏但必要」的字段收归一处。字符偏移与 `SOURCE` 关系都是「漏了不会立刻报错」的东西——它们只在检索、引用高亮、上下文扩展时才显形。把这类字段放在父类的公共后处理里，是新切分器能被正确写出来的前提。

**架构推论：** 「切分器返回 `List[str]`」这个接口选择，代价是丢失了偏移信息，于是父类必须靠 `find` 逆向重建。这个重建要求「切分器不改写文本」——任何 strip 或 normalize 都会让偏移静默失败。若接口返回 `List[(text, start, end)]`，偏移就是免费且精确的；但那样每个切分器都要自己维护位置，实现成本上升。这是一个用「父类脆弱的重建」换「子类简单的接口」的取舍。

**架构推论：** 默认 id 是随机 uuid，等于把「去重」的责任完全推给下游。这在库的定位上是对的——切分器不该假设自己的输出会被存两次；但它也让「同一份文档重跑一次摄取」在默认配置下必然产生重复节点。确定性 id 只能由调用方通过 `id_func` 提供。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/node_parser/interface.py` | `NodeParser` | 契约主体：抽象 `_parse_nodes` + 公共后处理（SOURCE / 偏移 / 元数据 / 前后关系） |
| `llama-index-core/llama_index/core/node_parser/interface.py` | `TextSplitter` | 把切分抽象成 `split_text`，自动提供批量切与遍历文档切节点 |
| `llama-index-core/llama_index/core/node_parser/interface.py` | `MetadataAwareTextSplitter` | 用纯净正文 + 元数据扣预算；`_get_metadata_str` 取两种渲染里较长的一份 |
| `llama-index-core/llama_index/core/node_parser/interface.py` | `_validate_id_func` / `_serialize_id_func` | `None` 归一成默认函数；序列化时只写函数名 |
| `llama-index-core/llama_index/core/node_parser/node_utils.py` | `IdFuncCallable` | `(i, doc) -> str` 的协议，决定确定性 id 能做到什么粒度 |
| `llama-index-core/llama_index/core/node_parser/node_utils.py` | `default_id_func` | 忽略参数、返回随机 uuid —— 语义上的「不做去重」 |
| `llama-index-core/llama_index/core/node_parser/node_utils.py` | `build_nodes_from_splits` | 片段 → 节点的工厂，循环外只建一次 SOURCE 关系以避开重复哈希 |
| `llama-index-core/llama_index/core/schema.py` | `as_related_node_info` / `hash` | 前者读后者；后者对文本与元数据算 sha256，是上面那处优化的对象 |

## 参考

- [llama-index 源码：node_parser/interface.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/node_parser/interface.py)
- [llama-index 源码：node_parser/node_utils.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/node_parser/node_utils.py)
- [llama-index 文档：节点与切分器](https://docs.llamaindex.ai/en/stable/module_guides/loading/node_parsers/)
- [llama-index 文档：自定义节点解析器](https://docs.llamaindex.ai/en/stable/module_guides/loading/node_parsers/modules/)

## 与主核 / AI 的关系

「一个整体变成多个带元数据的片段」在主核里对应的是**记录投影与派生列**：源行变成多行结果时，每行都要带上来源标识、行号、以及父级属性。这里的经验有五条。

**第一，易漏的公共字段必须收归父类。** `SOURCE` 关系与字符偏移都是「漏了不报错」的东西。主核里同理：派生行若不带来源标识，后续的行级血缘追踪就会断，而断点不会在写入时报错，只会在审计查询时才发现。把这类字段放进统一的后处理，比在文档里写「记得带」可靠得多。

**第二，「逆向重建位置」是接口设计的代价，不是必然。** 因为切分器只返回字符串，偏移只能靠 `find` 重建，且一旦文本被 normalize 就静默失败。主核里若要做「原文位置的定位」，更稳的做法是让产出方直接带上偏移（例如返回 `(文本, 起, 止)`），而不是让消费方去原文里找。

**第三，`+1` 而不是 `+len` 的游标推进，是「允许重叠」这条需求逼出来的。** 任何「从上次结束处往后找」的重建逻辑都要处理重叠，而处理方式就是「把游标推到能重新命中重叠区域的位置」。这个细节在主核里对应「增量扫描的可变窗口」——推进量必须小于实际消费量，否则会跳过重叠部分。

**第四，`as_related_node_info()` 会重算全文哈希这件事，说明「惰性属性可能很贵」。** 循环外建一次的做法把 O(N·L) 降成 O(L)。主核里凡是「每次访问都重算摘要/统计」的属性都有同样的风险，而它与「逐行为它调用一次」的循环组合起来是灾难性的。

**第五，id 的生成策略决定了去重能力。** 默认随机 uuid 意味着「框架不去重」，确定性 id 交给调用方。主核里对应的是**主键由谁定**：让用户声明主键（可以自然幂等），还是由系统生成（必然产生重复）。这个选择不该被默认值悄悄做出——这里的默认值实际上替用户做了「不去重」这个决定。
