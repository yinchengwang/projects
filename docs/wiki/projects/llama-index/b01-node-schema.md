---
project: llama-index
feature: node-schema
title: 节点类型与关系
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/schema.py
    symbols: [BaseNode, Node, TextNode, Document]
  - path: llama-index-core/llama_index/core/schema.py
    symbols: [NodeRelationship, RelatedNodeInfo, MetadataMode]
---

# 节点类型与关系

## 一句话职责

`BaseNode` 是 llama-index 的最小可检索单位：它把「一段内容」「一份元数据」「一组到其它节点的关系」压在一个类型里，并用 `MetadataMode` 把同一份元数据拆成「给模型看的」与「给嵌入模型看的」两个视图。

## 机制是什么

这套数据模型要同时喂三种下游，而它们的需求互相冲突：

| 下游 | 需要什么 | 冲突点 |
|---|---|---|
| 嵌入模型 | 参与向量化的文本 | 元数据全带上会稀释语义 |
| LLM | 作为上下文的一段文本 | 元数据全带上会挤占 token |
| 向量库 | 可过滤的元数据键值 | 序列化后必须能被后端索引 |

如果只有一个 `metadata` 字典，这三件事只能靠调用方自己拼字符串。llama-index 的做法是**给元数据加一个「模式」参数**：`MetadataMode` 的四个取值分别决定「哪些键被渲染进去」。于是同一份元数据能产出两个不同的文本视图，而字典本身保持原样供过滤使用。

第二个冲突是**层级关系**。chunk 之间不是平铺的：一个 document 切出多个 chunk，chunk 之间还有前后顺序，句子级节点还要回指父块。这套关系必须能在序列化后完整保留，否则「检索命中小块、生成用大块」（a02 的句子窗口）就无从实现。

## 怎么实现的

**`BaseNode` 的六个字段，每一个都在解决一个具体问题。**

```python
id_: str = Field(default_factory=lambda: str(uuid.uuid4()))
embedding: Optional[List[float]] = Field(default=None)
metadata: Dict[str, Any] = Field(default_factory=dict, alias="extra_info")
excluded_embed_metadata_keys: List[str] = Field(default_factory=list)
excluded_llm_metadata_keys: List[str] = Field(default_factory=list)
relationships: Dict[Annotated[NodeRelationship, EnumNameSerializer], RelatedNodeType] = Field(default_factory=dict)
metadata_template: str = Field(default=DEFAULT_METADATA_TMPL)
metadata_separator: str = Field(default="\n", alias="metadata_seperator")
```

三处值得看：

- **`metadata` 的别名是 `extra_info`**，而 `extra_info` 本身也已废弃（属性上有 `@deprecated(version="0.12.2")`，读写都转发到 `metadata`）。即这个字段经历了 `extra_info` → `metadata` 的更名，两条路都还在。
- **`metadata_separator` 的别名拼错了**：`metadata_seperator`（缺一个 `a`）。这不是笔误遗留的噪音——它是**必须保留的兼容路径**，因为早期版本的序列化产物里就写着这个拼错的键。这类「把拼写错误固化进别名」的做法在长期维护的库里很常见。
- **`embedding` 是**节点上的字段**而不是外部表里的字段**。这让节点自带向量，可以直接落进 docstore 而不必维护一张「id → 向量」的旁表。代价是同一份向量可能被存两次（docstore 一份、向量库一份）。

`BaseNode` 是抽象类，五个抽象成员：`get_type`、`get_content`、`get_content_blocks`、`set_content`、`hash`。注意**`get_content` 与 `get_content_blocks` 并列存在**——一个是「拼成一段字符串」，一个是「保持结构化块列表」。这是 b02 的多模态路径与文本路径的分界，在基类就已经分开。

**`MetadataMode` 是整页最值得精确理解的一处。**

```python
class MetadataMode(str, Enum):
    ALL = "all"
    EMBED = "embed"
    LLM = "llm"
    NONE = "none"
```

四个值是**显式字符串**（不是 `auto()`）。这与同文件里其它枚举的写法不同——那些用 `auto()`，这四个用字面量。原因大概是这四个值会出现在对外接口上（用户直接传 `MetadataMode.LLM` 或字符串 `"llm"`），而 `auto()` 在 `str` 子类下生成的是 `"1"`、`"2"`…（见下文），不适合人读。

渲染逻辑：

```python
def get_metadata_str(self, mode: MetadataMode = MetadataMode.ALL) -> str:
    if mode == MetadataMode.NONE:
        return ""

    usable_metadata_keys = set(self.metadata.keys())
    if mode == MetadataMode.LLM:
        for key in self.excluded_llm_metadata_keys:
            if key in usable_metadata_keys:
                usable_metadata_keys.remove(key)
    elif mode == MetadataMode.EMBED:
        for key in self.excluded_embed_metadata_keys:
            if key in usable_metadata_keys:
                usable_metadata_keys.remove(key)

    return self.metadata_separator.join(
        [
            self.metadata_template.format(key=key, value=str(value))
            for key, value in self.metadata.items()
            if key in usable_metadata_keys
        ]
    )
```

五处：

1. **`NONE` 早退返回空串**，不做任何计算。
2. **`usable_metadata_keys` 是「元数据键全集减去排除集」**。所以排除是**减法**而不是白名单——默认全部可见，只有显式排除的才不可见。这个方向决定了「新加一个元数据键会自动出现在两个视图里」，除非你记得同步加排除项。
3. **遍历的是 `self.metadata.items()` 而不是 `usable_metadata_keys`**。`usable_metadata_keys` 是 `set`，若遍历它输出顺序就不确定了；遍历原字典再按集合过滤，**保留了元数据的插入顺序**。这是一个容易被忽略但影响可复现性的细节——同一份元数据必须每次渲染出同一个字符串，否则嵌入结果不稳定。
4. **`value=str(value)`**：所有值都转成字符串再塞进模板。所以嵌套字典会被 `str()` 成一个单行 Python 字面量，而不是结构化展开。
5. **`metadata_template` 默认 `"{key}: {value}"`**，`metadata_separator` 默认 `"\n"`。所以默认渲染是每行一条 `key: value`。

`get_content` 把元数据与正文拼起来，而拼法由 `text_template` 决定：

```python
def get_content(self, metadata_mode: MetadataMode = MetadataMode.NONE) -> str:
    metadata_str = self.get_metadata_str(mode=metadata_mode).strip()
    if metadata_mode == MetadataMode.NONE or not metadata_str:
        return self.text
    return self.text_template.format(content=self.text, metadata_str=metadata_str).strip()
```

`DEFAULT_TEXT_NODE_TMPL = "{metadata_str}\n\n{content}"`，即**元数据在前、正文在后**。这个顺序不是随意的：元数据（如文件名、章节标题）先出现，给模型一个「这段是什么」的定位，再读正文。

注意**三个实现类的默认参数不一致**。基类签名写 `MetadataMode.ALL`，而 `TextNode.get_content`、`Node.get_content`、`Document.text` 都默认 `NONE`。所以「调用 `node.get_content()` 而什么都不传」时，得到的是**纯正文**——元数据不会自动带上。要带元数据必须显式传 `MetadataMode.LLM`。

这个默认值的取舍很关键：它让「默认行为无副作用」（不悄悄把元数据塞进正文），代价是**最常见的调用方式拿不到元数据**，而很多调用方（如 `VectorStoreIndex` 构造嵌入文本时）需要自己记得传。

**`NodeRelationship` 的五个值决定了层级检索能表达什么。**

```python
class NodeRelationship(str, Enum):
    SOURCE = auto()
    PREVIOUS = auto()
    NEXT = auto()
    PARENT = auto()
    CHILD = auto()
```

五个关系各自对应一种结构：

| 关系 | 基数 | 用途 |
|---|---|---|
| `SOURCE` | 单 | 指向源文档（chunk → document） |
| `PREVIOUS` / `NEXT` | 单 | 线性相邻（块序） |
| `PARENT` | 单 | 向上（句 → 块） |
| `CHILD` | **列表** | 向下（块 → 句/子索引） |

所以这套枚举**同时支持三种结构**：线性链（PREVIOUS/NEXT）、两层的树（PARENT/CHILD）、以及一个「归属」指针（SOURCE）。a02 的句子窗口正是靠 `SOURCE` + `PREVIOUS`/`NEXT` 实现的：命中句子级节点后，沿前后指针取窗口。

注意 `SOURCE` 与 `PARENT` 在语义上有重叠——都指「上一级」。它们的区别是**用途**：`SOURCE` 回答「这条内容来自哪个文档」（用于回溯与删除），`PARENT` 回答「这条内容属于哪个更大的块」（用于上下文扩展）。同一个节点可以两者都有。

**`auto()` 在这个 `str` 子类里生成的是数字字符串，而不是名字——这是一个真实且容易误判的坑。**

`NodeRelationship(str, Enum)` 的每个成员用 `auto()`，而类里**没有定义 `_generate_next_value_`**。在 Python 的 `Enum` 里，`auto()` 会调用 `_generate_next_value_(name, start, count, last_values)`；默认实现对 `str` 混合类返回的是**递增整数的字符串形式**。所以实测结果是：

```
NodeRelationship.SOURCE.value == "1"
NodeRelationship.PREVIOUS.value == "2"
...
NodeRelationship.SOURCE == "source"   → False
str(NodeRelationship.SOURCE)          → "NodeRelationship.SOURCE"
```

这意味着**关系字典序列化后的键是 `"1"`…`"5"`**，而不是 `"source"`、`"parent"` 这样可读的名字。序列化由一个显式指定的序列化器完成：

```python
EnumNameSerializer = PlainSerializer(lambda e: e.value, return_type="str", when_used="always")
```

即「取 `.value`」。于是 `relationships` 落进 docstore 时是 `{"1": {"node_id": "..."}, ...}`。

这个设计有一个可验证的后果：**序列化产物只能被这个 schema 读懂**。`"1"` 这个键本身不含任何语义，任何外部系统想直接解析 docstore 的 JSON 都必须知道这个枚举的声明顺序。而顺序一旦被改动（例如在中间插入一个新关系），旧数据的键就会指向错误的关系——这是一个**隐藏在枚举成员顺序里的向后兼容约束**。

**`RelatedNodeType` 用一个联合表达「单值或列表」，于是每个读取器都得做类型检查。**

```python
RelatedNodeType = Union[RelatedNodeInfo, List[RelatedNodeInfo]]
```

而字段声明是 `Dict[NodeRelationship, RelatedNodeType]`——即**同一个字典的值可能是单个对象也可能是列表**，取决于键。五个 getter 各自负责自己那一类的判定：

```python
@property
def source_node(self) -> Optional[RelatedNodeInfo]:
    if NodeRelationship.SOURCE not in self.relationships:
        return None
    relation = self.relationships[NodeRelationship.SOURCE]
    if isinstance(relation, list):
        raise ValueError("Source object must be a single RelatedNodeInfo object")
    return relation
```

```python
@property
def child_nodes(self) -> Optional[List[RelatedNodeInfo]]:
    if NodeRelationship.CHILD not in self.relationships:
        return None
    relation = self.relationships[NodeRelationship.CHILD]
    if not isinstance(relation, list):
        raise ValueError("Child objects must be a list of RelatedNodeInfo objects.")
    return relation
```

三处：

1. **缺失返回 `None`，类型错则抛 `ValueError`**。所以「没有关系」与「关系形状错」被严格区分：前者是正常状态，后者是数据损坏。这与「字段缺失当空值」的宽松做法相反——写错了形状必须暴露。
2. **`child_nodes` 是唯一返回列表的 getter**，其余四个都是单值。
3. **每个 getter 都重复了一遍 `if ... not in self.relationships` 的检查**，没有抽公共函数。代价是重复，收益是每个 getter 的语义（单值/列表）与它的错误消息能写在一起。

这个联合类型的设计是否值得可以讨论。更紧的写法是给 `SOURCE`/`PREVIOUS`/`NEXT`/`PARENT` 用单值类型、只给 `CHILD` 用列表类型——但 pydantic 的字典值类型无法按键区分，所以只能退化成联合 + 运行时检查。**即「类型系统表达不了的约束，被下推到了每个 getter 的运行时检查」。**

**`RelatedNodeInfo` 是关系的载体，而不是关系本身。**

```python
class RelatedNodeInfo(BaseComponent):
    node_id: str
    node_type: Annotated[ObjectType, EnumNameSerializer] | str | None = None
    metadata: Dict[str, Any] = Field(default_factory=dict)
    hash: Optional[str] = None
```

四个字段的意义：

- **`node_id`** 是必需的对端标识。
- **`node_type`** 是 `ObjectType`（TEXT/IMAGE/INDEX/DOCUMENT/MULTIMODAL），但类型声明允许 `str`——所以既可以是枚举也可以是任意字符串。这给「第三方自定义节点类型」留了空间。
- **`metadata`** 让关系**带上对端的一部分元数据副本**。这是一个空间换时间的取舍：存副本意味着「沿关系跳过去」不一定需要再查一次 docstore，但也意味着副本可能与源节点不一致。
- **`hash`** 是构造关系时记录的**内容指纹**。它的用途是检测对端内容是否变过——若 `RelatedNodeInfo.hash` 与对端节点当前的 `hash` 不一致，说明关系记录已经过期。这是「关系是快照而不是活指针」的证据。

`as_related_node_info()` 是产出一个 `RelatedNodeInfo` 的唯一正路：

```python
def as_related_node_info(self) -> RelatedNodeInfo:
    return RelatedNodeInfo(
        node_id=self.node_id,
        node_type=self.get_type(),
        metadata=self.metadata,
        hash=self.hash,
    )
```

注意 `metadata=self.metadata` 是**直接引用同一个字典对象**，不是复制。于是「关系里的元数据副本」与「节点自己的元数据」在内存中是同一份——修改其中一处会影响另一处。序列化后才分离。

**`hash` 是抽象属性，四个实现类的算法完全不同——这决定「内容没变」的判据。**

| 类 | hash 组成 |
|---|---|
| `TextNode` | `str(text) + str(metadata)` |
| `Node` | `metadata_str` + 四个 `MediaResource.hash`，用 `-` 连接 |
| `ImageNode` | `image`/`image_path`/`image_url`/`text` 四项用 `-` 连接 |
| `Document` | 继承 `Node`（`Document(Node)`） |

三处：

- **`TextNode` 的 hash 把 `str(metadata)` 混进去**——即**元数据变化会让内容被判为「变了」**。这与 g01 那种「元数据算进身份」的做法是同一条路，后果也一样：改一个只用于过滤的键（如抓取时间）会让整份内容被视为新内容。
- **`Node` 的 hash 用 `get_metadata_str(mode=MetadataMode.ALL)`**，即渲染后的元数据字符串，而不是 `str(dict)`。所以键的插入顺序会影响 hash——`{"a":1,"b":2}` 与 `{"b":2,"a":1}` 得到不同 hash。这是一个真实的敏感点。
- **`ImageNode` 的 hash 用 `self.image or "None"`**：空字符串与 `None` 会被折成同一个 `"None"`。所以「image 是空串」与「image 没设」得到相同 hash——而它们可能是不同的状态。

所有 hash 都用 `encode("utf-8", "surrogatepass")`。`surrogatepass` 是必需的：某些 Unicode 字符（如孤立的代理项）用默认编码会抛 `UnicodeEncodeError`。加上它让 hash 计算对任意文本都不崩——代价是「非法字符串也能被 hash」，所以 hash 不构成输入合法性检查。

**`Node` 与 `TextNode` 的关系是「新模型 vs 兼容层」，而兼容是靠构造函数搬运实现的。**

`Node` 是新的多模态基类（`get_type` 返回 `ObjectType.MULTIMODAL`），持有 `text_resource`/`image_resource`/`audio_resource`/`video_resource` 四个 `MediaResource`。而 `TextNode` 保留着一个裸的 `text: str` 字段——它的文档只有一句「Provided for backward compatibility」。

两者之间靠构造函数的参数搬运打通：

```python
class TextNode(BaseNode):
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        if "text_resource" in kwargs:
            tr = kwargs.pop("text_resource")
            if isinstance(tr, MediaResource):
                kwargs["text"] = tr.text
            else:
                kwargs["text"] = tr["text"]
        super().__init__(*args, **kwargs)
```

即**用 `Node` 的写法构造 `TextNode` 也能成功**——`text_resource` 会被拆出它的 `text` 塞进 `text` 字段，其余字段正常处理。反过来 `Node.get_content` 也标着「Provided for backward compatibility, use `self.text_resource` directly instead」。

这个双写局面带来一个真实的不一致：`TextNode.text` 与 `Node.text_resource.text` 是**两个不同的存储位置**。所以 `TextNode` 一旦经过 `Node` 的路径（例如某个函数返回 `Node` 而实际是 `TextNode`），写入可能落在另一处。`Node.set_content` 会**新建**一个 `MediaResource(text=value)` 覆盖整个 resource——若原来的 resource 还有 `path` 或 `url`，它们会丢失。

**`Document` 是四层兼容的集中体现，而且它的序列化会凭空多回一个字段。**

```python
def __init__(self, **data: Any) -> None:
    if "doc_id" in data:
        value = data.pop("doc_id")
        if "id_" in data:
            logging.warning("'doc_id' is deprecated and 'id_' will be used instead")
        else:
            data["id_"] = value
    if "extra_info" in data:
        ...
    if data.get("text"):
        text = data.pop("text")
        if "text_resource" in data:
            ...  # 与已有 text_resource 的 text strip 后比较，不同则警告
        else:
            data["text_resource"] = MediaResource(text=text)
    super().__init__(**data)
```

四处：

1. **三个旧参数各自有「新参数已存在就用新的、只警告」的策略**：`doc_id` 与 `id_` 并存时用 `id_`；`extra_info` 与 `metadata` 并存时用 `metadata`。这是**「新版优先 + 警告」**，比「旧版优先」更合理（否则新代码的设置会被旧参数悄悄覆盖）。
2. **`text` 的处理更细**：与已有 `text_resource.text` 做 `strip()` 后比较，**不同才警告**。所以「传了 `text` 且与 `text_resource` 内容一致」不产生噪音。这是三处里唯一做值比较的——因为内容一致的情况下两个参数表达同一件事，不算冲突。
3. **`if data.get("text")` 是真值判断**，所以 `text=""` 不会触发这段逻辑（空串是 falsy）。即「显式传空文本」不会建立 `text_resource`。这是一个边界：空文档的 `text_resource` 保持 `None`。
4. **警告用 `logging.warning` 而不是 `warnings.warn`**。这与 a02 里用 `print` 的做法又不同——同一个项目里三种提示方式（`logging.warning`、`warnings.warn`、`print`）并存，说明「给出提示」这件事没有统一约定。

序列化侧还有一处反向的兼容：

```python
@model_serializer(mode="wrap")
def custom_model_dump(self, handler, info):
    data = super().custom_model_dump(handler, info)
    exclude_set = set(info.exclude or [])
    if "text" not in exclude_set:
        data["text"] = self.text
    return data
```

即**每次 dump 都把 `text` 加回输出**（`text` 是只读属性，从 `get_content()` 算出来）。注释说「For full backward compatibility with the text field」。

这个做法的代价值得指出：`text` 被加回来时用的是 `get_content()` 的默认参数，即 `MetadataMode.NONE`——所以序列化产物里的 `text` 是**不带元数据的纯正文**，而 `text_resource` 里也只有正文。两个字段内容重复，**同一份文本在 JSON 里出现两次**。对大文档这是实打实的体积浪费，而收益只是让旧读法能继续工作。

**`Document` 还带跨框架转换，两行代码暴露了三个生态的字段命名差异。**

```python
def to_langchain_format(self) -> LCDocument:
    return LCDocument(page_content=self.text, metadata=self.metadata, id=self.id_)

def to_haystack_format(self) -> HaystackDocument:
    return HaystackDocument(content=self.text, meta=self.metadata, embedding=self.embedding, id=self.id_)
```

| 概念 | llama-index | langchain | haystack |
|---|---|---|---|
| 正文 | `text` | `page_content` | `content` |
| 元数据 | `metadata` | `metadata` | `meta` |
| 向量 | `embedding` | — | `embedding` |

即三个生态对同一组概念用了三套名字（正文三套、元数据两套）。转换函数的全部价值就是这张映射表——它是「框架间互操作」的最小实现：不共享数据结构，只做字段搬运。

注意 `from_langchain_format` 保留了 `doc.id`（有则传 `id_`、无则不传），而 `to_langchain_format` 总是传 `id=self.id_`（因为 llama-index 的 `id_` 永远有值，默认是 UUID4）。所以**往返一次会让 `id` 从 `None` 变成 UUID**——这是一次不可逆的填充。

## 关键取舍

**源码事实：** `BaseNode` 有六个字段，其中 `metadata` 的别名是 `extra_info`（已废弃但保留），`metadata_separator` 的别名拼错成 `metadata_seperator`（为兼容旧序列化产物而保留）。

**源码事实：** `MetadataMode` 的四个值是显式字符串（`"all"`/`"embed"`/`"llm"`/`"none"`），而同文件其它枚举用 `auto()`；排除机制是「全集减排除集」，渲染时遍历原字典以保留插入顺序。

**源码事实：** `NodeRelationship` 与 `ObjectType` 用 `auto()` 且**未定义 `_generate_next_value_`**，因此在 `str` 子类下取值为 `"1"`…`"5"`（数字字符串），序列化键也是这些数字。`NodeRelationship.SOURCE == "source"` 为假。

**源码事实：** `RelatedNodeType` 是 `Union[RelatedNodeInfo, List[RelatedNodeInfo]]`，同一个字典的值可能是单值或列表；五个 getter 各自 `isinstance` 检查并在形状错时抛 `ValueError`（缺失则返回 `None`）。

**源码事实：** `RelatedNodeInfo` 携带对端 `metadata` 与 `hash`；`as_related_node_info` 直接引用同一份 `metadata` 字典对象（非复制）。

**源码事实：** `hash` 在四个类里算法不同：`TextNode` 混入 `str(metadata)`；`Node` 用渲染后的元数据串并连接四个 resource hash；`ImageNode` 把空串与 `None` 折成 `"None"`。全部用 `surrogatepass` 编码。

**源码事实：** `Document.custom_model_dump` 每次 dump 都把 `text`（来自 `get_content()`，默认纯正文）加回输出，导致同一份文本在 JSON 中与 `text_resource` 重复。

**源码事实：** `Document.__init__` 对三个旧参数采取「新版优先 + 警告」，且 `text` 与 `text_resource.text` 相同则不警告；`if data.get("text")` 是真值判断，故 `text=""` 不建 resource。

**架构推论：** 「元数据的模式化渲染」解决的是**同一份数据面向多个消费者**的问题，而不是数据存储问题。它的价值在 `MetadataMode.EMBED` 与 `MetadataMode.LLM` 可以塞不同的排除集——例如把 `source_path` 留给 LLM（它能帮助定位）但从嵌入里排除（它对语义相似度无贡献）。代价是**同一份元数据的两个视图会漂移**，而漂移是静默的：没人会发现嵌入里少了一个键。

**架构推论：** `auto()` 生成数字串这件事，是一个**类型声明与实际语义脱节的实例**。代码写的是 `SOURCE = auto()`，读到的是「源码里这个关系叫 SOURCE」；而落盘的是 `"1"`。这个脱节之所以没被修，大概因为改枚举顺序会破坏既有 docstore；而它之所以危险，正因为**成员顺序变成了隐式的持久化格式**。更稳的做法是像 `MetadataMode` 那样写死字面量——牺牲一点书写简洁，换来「顺序可以自由调整」。

**架构推论：** `RelatedNodeType` 的联合类型是「类型系统表达不了键相关的约束」的典型后果：约束下沉到五个 getter 的运行时检查。这类设计的成本不在运行时（每个 getter 只多一次 `isinstance`），而在**认知**——读代码的人必须先知道「哪个关系是列表」才能正确解释 `relationships`。把这条知识写在每个 getter 的错误消息里是对的补救。

**架构推论：** `Document` 的兼容层揭示了一个长期项目的真实形态：**同一个概念在序列化产物里可能有多个名字、在内存里可能有多个存储位置**。`text` 与 `text_resource` 并存就是如此。这类重复一旦引入就极难消除（旧数据、旧 API、旧测试都依赖它），而维护成本集中在「每一处写入都必须同时照顾两边」——`Node.set_content` 会丢掉 resource 的 `path`/`url` 就是这类成本的实例。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/schema.py` | `BaseNode` | 六个字段 + 五个抽象成员的可检索单位基类 |
| `llama-index-core/llama_index/core/schema.py` | `BaseNode.get_metadata_str` | 按 `MetadataMode` 渲染元数据，保留键插入顺序 |
| `llama-index-core/llama_index/core/schema.py` | `BaseNode.child_nodes` | 唯一返回列表的关系读取器，形状错时抛 `ValueError` |
| `llama-index-core/llama_index/core/schema.py` | `BaseNode.as_related_node_info` | 产出带 `hash` 的关系快照（元数据为同引用） |
| `llama-index-core/llama_index/core/schema.py` | `NodeRelationship` | 五个关系：SOURCE / PREVIOUS / NEXT / PARENT / CHILD |
| `llama-index-core/llama_index/core/schema.py` | `ObjectType` | TEXT / IMAGE / INDEX / DOCUMENT / MULTIMODAL |
| `llama-index-core/llama_index/core/schema.py` | `RelatedNodeInfo` | 关系载体：id + 类型 + 元数据副本 + 内容指纹 |
| `llama-index-core/llama_index/core/schema.py` | `RelatedNodeType` | 单值或列表的联合，把约束推到运行时 |
| `llama-index-core/llama_index/core/schema.py` | `MetadataMode` | 元数据的四个视图（显式字符串值） |
| `llama-index-core/llama_index/core/schema.py` | `EnumNameSerializer` | 枚举序列化成 `.value` 的 `PlainSerializer` |
| `llama-index-core/llama_index/core/schema.py` | `Node` | 多模态基类，持有四个 `MediaResource` |
| `llama-index-core/llama_index/core/schema.py` | `TextNode` | 兼容层：裸 `text` 字段 + `text_resource` 构造搬运 |
| `llama-index-core/llama_index/core/schema.py` | `Document` | 四层兼容：三个旧参数 + 序列化回填 `text` + 跨框架转换 |
| `llama-index-core/llama_index/core/schema.py` | `QueryBundle` | 查询侧对应物：原始查询 + 自定义嵌入串 + 图像路径 |

## 参考

- [llama-index 源码：schema.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/schema.py)
- [llama-index 文档：Node 与 Document](https://docs.llamaindex.ai/en/stable/module_guides/loading/documents_and_nodes/)
- [Python 文档：enum.auto 与 _generate_next_value_](https://docs.python.org/3/library/enum.html#enum.auto)
- [Pydantic 文档：PlainSerializer 与字段别名](https://docs.pydantic.dev/latest/concepts/serialization/#custom-serializers)

## 与主核 / AI 的关系

`Node` 与主核（多模态数据库内核）的**元组 + 行标识**结构有一对一的对应关系：

| 概念 | llama-index | 主核 |
|---|---|---|
| 身份 | `id_`（默认 UUID4） | 行标识（页内偏移 + 页号） |
| 内容 | `text` / `text_resource` | 列值 |
| 元数据 | `metadata`（可过滤） | 扩展属性 / 索引列 |
| 关系 | `relationships` | 外键、页指针链 |
| 指纹 | `hash` | 行的 `xmin` / 校验和 |

最有价值的一处对照是**「关系的单值/列表联合」对应主核的「外键基数」**。主核有 `REFERENCES` 声明的基数（一对一、一对多），而这个基数是在 schema 里声明的、由引擎强制执行。llama-index 没有 schema，所以基数只能靠「哪个枚举成员 + 运行时 `isinstance`」表达。这解释了为什么 `CHILD` 是列表而其它是单值——**它是一条写在代码里的隐式 schema**。

第二处是**元数据的双视图**对应主核的**列的可空性与存储形态**。主核里同一列可以在行存里是 `int8`、在列存里被编码成字典编码的短码——同一份数据两种物理表示，由引擎保证等价。llama-index 的 `MetadataMode` 做的是类似的事，但它**不保证等价**：两个视图按配置排除不同的键，是有意的差异。这个差别来自目的——主核要对同一查询给出同一结果，RAG 要让同一份数据服务不同消费者。

第三处是 `hash` 把元数据算进指纹这一点，与主核的**唯一索引不能只覆盖部分列**同理。主核里若唯一索引只覆盖部分列，改非索引列不会换身份；llama-index 把元数据算进 hash，改任何元数据都会换身份。g01 那篇已经讨论过这个取舍的两个方向，这里的结论相同：**身份的定义范围决定了「什么算新内容」**。

对 AI 工程的实际意义在于：节点模型是 RAG 的地基，它的字段划分决定了后续能表达多少结构。这份实现给出的三条经验可直接用：**元数据要带模式而不是只有一份字典**（否则调用方各拼各的字符串）、**关系要能表达「线性 + 树 + 归属」三种结构**（否则句子窗口这类机制无处落地）、以及**兼容层要显式标出哪些字段是历史包袱**（否则新代码会把 `text` 与 `text_resource` 当成等价的两个入口）。
