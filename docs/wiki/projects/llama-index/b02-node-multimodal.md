---
project: llama-index
feature: node-multimodal
title: 多模态节点与媒体资源
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/schema.py
    symbols: [ImageNode, IndexNode, MediaResource, Modality]
  - path: llama-index-core/llama_index/core/schema.py
    symbols: [NodeWithScore]
---

# 多模态节点与媒体资源

## 一句话职责

`MediaResource` 是一个**装任何媒体的容器**（原始字节 / 本地路径 / URL / 纯文本 / 向量），`Node` 用它四个副本表达「一条内容可以同时有文本与图像」；`ImageNode`、`IndexNode`、`NodeWithScore` 则是这条思路在兼容层、内容寻址与检索结果三个方向上的延伸。

## 机制是什么

多模态在数据模型上要回答一个看似简单的问题：**一张图该怎么存**。至少有四种正当答案，而它们各自成立：

| 存法 | 适用 | 代价 |
|---|---|---|
| base64 字节 | 内容自足，可直接发给模型 | 体积膨胀 33%，落库会很大 |
| 本地路径 | 最省，且能复用文件系统 | 节点不再自足，路径失效即数据丢失 |
| URL | 适合远端资源 | 取用时要发网络请求，引入 SSRF 与超时问题 |
| 向量 | 支持按图像相似度检索 | 只是检索索引，不是内容本身 |

`MediaResource` 的选择是**四种全要**，并用同一套字段表达它们——不只是「支持四种输入」，而是「一个容器能同时装下文本表示、原始数据、路径、URL 与向量」。

这带来一个必须解决的矛盾：**这些来源彼此可以冲突**。同一个 resource 可能既有 `data` 又有 `path` 与 `url`。源码没有强制互斥，而是让 `hash` 把**所有非 `None` 的字段都算进去**——于是「同一张图通过不同渠道」得到不同 hash。这是一个刻意的选择，代价是**内容同一性无法跨渠道识别**。

## 怎么实现的

**`MediaResource` 的六个字段，加上一条必须注意的序列化排除。**

```python
class MediaResource(BaseModel):
    embeddings: dict[EmbeddingKind, list[float]] | None = None
    data: bytes | None = Field(default=None, exclude=True, description="base64 binary representation of this resource.")
    text: str | None = None
    path: Path | None = None
    url: AnyUrl | None = None
    mimetype: str | None = None

    model_config = {"validate_default": True}
```

四处：

1. **`data` 带 `exclude=True`**——即**序列化时永远不会被写出去**。这是防止 docstore 被 base64 图像撑爆的关键一步。但它同时意味着**落库之后原始字节就没了**：只能靠 `path` 或 `url` 重新取回。若两者都没有，那个 resource 序列化后只剩 `text` 与 `mimetype`——内容不可恢复。这是一个安静的信息丢失点。

2. **`embeddings` 是 `dict[EmbeddingKind, list[float]]`，而 `EmbeddingKind = Literal["sparse", "dense"]`**。即一个资源可以**同时带稀疏与稠密两种向量**。这个设计与 b01 的 `BaseNode.embedding`（单个 `List[float]`）并存，是「新模型 vs 兼容字段」的又一处重叠。

3. **`validate_default: True`** 让默认值（`None`）也走校验器。注释写「This ensures validation runs even for None values」。必要性在于两个 `field_validator` 都依赖「另一个字段已经处理过」——若默认值不校验，`mimetype` 的校验器就看不到 `data` 是否被编码过。

4. **`url: AnyUrl | None`** 而不是 `str`。所以 URL 会被 pydantic 规范化（如补末尾斜杠、百分号编码）。这对 `hash` 有影响：同一个 URL 的两种写法会被规范化成同一个，这反而是好事。

**`data` 的校验器做了一件反直觉的事：它把「原始字节」编码成 base64，而不是解码。**

```python
@field_validator("data", mode="after")
@classmethod
def validate_data(cls, v: bytes | None, info: ValidationInfo) -> bytes | None:
    if v is None:
        return v
    try:
        decoded = base64.b64decode(v, validate=True)
    except BinasciiError:
        return base64.b64encode(v)
    return v
```

逻辑是：**若传入的字节串本身已经是合法 base64，就原样保留；否则把它当成原始二进制、编码成 base64 存入**。所以字段名说「data 是 base64 binary representation」，即**内部统一存 base64 形式**。

三处：

- **`validate=True` 的理由写在注释里**：「`b64decode()` can succeed on random binary data, so we pass `validate=True` to make sure it's not a false positive」。这一点很关键——不带 `validate` 的 `b64decode` 会**静默丢弃非 base64 字符**，于是任意二进制都可能「解码成功」。加上它之后误判基本消除。
- **但依然有误判空间**：一段恰好由 base64 字母表组成的短二进制（例如 4 字节的随机数据）会被当成合法 base64 而原样保留。此时它不会被编码，于是**内部存的是那 4 字节原始数据而不是 base64**。所以「data 一定是 base64」这个不变量并不严格成立。
- **`decoded` 这个变量算出后没被使用**。它只为「验证可否解码」服务。这是为 `validate=True` 服务的必要写法——不解一次就无法知道它是否合法。

**`mimetype` 的校验器是「两级猜测」，且顺序依赖字段校验次序。**

```python
@field_validator("mimetype", mode="after")
@classmethod
def validate_mimetype(cls, v: str | None, info: ValidationInfo) -> str | None:
    if v is not None:
        return v
    b64_data = info.data.get("data")
    if b64_data:
        decoded_data = base64.b64decode(b64_data)
        if guess := filetype.guess(decoded_data):
            return guess.mime
    rpath: str | None = info.data["path"]
    if rpath:
        extension = Path(rpath).suffix.replace(".", "")
        if ftype := filetype.get_type(ext=extension):
            return ftype.mime
    return v
```

四处：

1. **显式给了就用**，不猜。猜测只是兜底。
2. **先从内容猜**（`filetype.guess` 读魔数），**再从路径扩展名猜**。前者更可靠（扩展名可以撒谎），所以顺序合理。
3. **`info.data.get("data")` 依赖 `data` 字段已被校验**。注释明确说了「Since this field validator runs after the one for `data` then the contents of `data` should be encoded already」。**所以这两个字段的声明顺序是有语义的**：`data` 必须声明在 `mimetype` 之前。把字段顺序从「按重要性排」改成「按字母排」就会破坏它。
4. **`info.data["path"]` 用方括号而不是 `.get()`**。若 `path` 未通过校验（例如被省略且无默认），这里会 `KeyError` 而不是返回 `None`。由于 `path` 有默认 `None`，实际不会发生；但这是一个潜在的脆弱点。

还有一处：校验器里 `base64.b64decode(b64_data)` **没有带 `validate=True`**。所以若 `data` 是「误判为 base64 的原始二进制」，这里解码会得到与原始数据不同的字节，`filetype.guess` 就猜错了。两处的 `validate` 用法不一致。

**`path` 有一个专门的序列化器，把它转成字符串。**

```python
@field_serializer("path")
def serialize_path(self, path: Optional[Path], _info: ValidationInfo) -> Optional[str]:
    if path is None:
        return path
    return str(path)
```

`Path` 对象 pydantic 能序列化成字符串，这个序列化器大概是为了**确保 Windows 路径不被特殊处理**——但源码没注释。实际效果是 `str(Path)`，与默认行为一致。

`__init__` 里这段 `rpath: str | None = info.data["path"]` 之后又 `Path(rpath)`——即**在校验阶段 `path` 可能已经是 `str` 而非 `Path`**。这与字段声明矛盾，说明 `validate_default` 或别名处理让类型在这里不严格。这段代码的可移植性值得质疑，但它确实在工作（因为 `Path(str)` 与 `Path(Path)` 都合法）。

**`hash` 把空字符串与「没有」分开，这是一处显式设计。**

```python
@property
def hash(self) -> str:
    bits: list[str] = []
    if self.text is not None:
        if self.text == "":
            bits.append("<empty_string>")
        else:
            bits.append(self.text)
    if self.data is not None:
        bits.append(str(sha256(self.data).hexdigest()))
    if self.path is not None:
        bits.append(str(sha256(str(self.path).encode("utf-8")).hexdigest()))
    if self.url is not None:
        bits.append(str(sha256(str(self.url).encode("utf-8")).hexdigest()))
    if not bits:
        return ""
    doc_identity = "".join(bits)
    return str(sha256(doc_identity.encode("utf-8")).hexdigest())
```

四处：

1. **`text=""` 用标记 `<empty_string>` 区分于 `text=None`**，文档也明说了：「An empty string for text (`text=""`) is treated as valid content and will produce a different hash than `text=None`」。理由很实在——空文本与没有文本是不同状态，前者意味着「这里确实是一段空内容」。
2. **四个来源都参与，`text` 不哈希而其余三个哈希**。`text` 直接用原文（它已经是可比较的字符串），`data`/`path`/`url` 各自先 sha256。这个不对称的理由是效率：path/url 可能很长，先哈希能压缩。
3. **`data` 的哈希是对**已编码的 base64**算的**。所以同一个图像，一次以原始字节传入（被编码）与一次以 base64 传入（原样保留），只要最终编码结果相同，hash 就一致。这保证了跨输入形式的稳定性。
4. **全空返回空字符串 `""`**，而不是对空串哈希。所以「什么都没有」的 resource 的 hash 是 `""`，而**它在 `Node.hash` 里会被当成有效的一节拼进去**（`Node.hash` 用 `-` 连接四个 resource hash，不过滤空串）。这会让一个没有内容的 resource 也影响节点 hash。

**`Node` 用四个 `MediaResource` 字段而不是一个 `list`，这是一个关键取舍。**

```python
class Node(BaseNode):
    text_resource: MediaResource | None = None
    image_resource: MediaResource | None = None
    audio_resource: MediaResource | None = None
    video_resource: MediaResource | None = None
    text_template: str = DEFAULT_TEXT_NODE_TMPL
```

即**每种模态最多一个**。所以一个 `Node` 不能同时带两张图，也不能带两段音频。

这个限制与 `Modality` 枚举的四个值（TEXT/IMAGE/AUDIO/VIDEO）对齐——枚举表达的正是这四个槽位。代价是**多图场景必须切成多个节点**（或退化成用 `list` 的老 `TextNode` 多模态块路径）。收益是结构简单可枚举：任何消费方都能确定「有这个字段就有这个模态」。

`Modality` 这个枚举本身值得单记：它在 `schema.py` 里被声明，但**在整个 `llama-index-core` 里没有任何使用**（除定义外零引用）。所以它是一个**为外部（集成包、用户代码）准备的公开类型**，而核心内部仍然靠「四个字段名」而不是「模态枚举」来分派。这是一个「公开 API 面比内部实现面更整齐」的现象——对外你可以说「按 `Modality` 遍历」，内部却是四个 if。

**`get_content_blocks` 是这套资源模型的出口，四个模态各自映射到不同的 LLM 块类型。**

```python
def get_content_blocks(self, metadata_mode=MetadataMode.NONE) -> list[BaseContentBlock]:
    blocks: list[BaseContentBlock] = []
    blocks.extend(self.get_metadata_content_blocks(metadata_mode))
    if self.text_resource:
        blocks.append(TextBlock(text=self.text_resource.text or ""))
    if self.image_resource:
        blocks.append(ImageBlock(image=..., url=..., path=..., image_mimetype=...))
    if self.audio_resource:
        guess = filetype.get_type(mime=self.audio_resource.mimetype)
        blocks.append(AudioBlock(audio=..., url=..., path=..., format=guess.extension if guess else None))
    if self.video_resource:
        blocks.append(VideoBlock(video=..., url=..., path=..., video_mimetype=...))
    return blocks
```

三处：

1. **元数据块在最前**（`get_metadata_content_blocks` 产出一个 `TextBlock`），顺序与 `get_content` 的 `"{metadata_str}\n\n{content}"` 一致——元数据先定位，内容后读。
2. **`AudioBlock` 需要 `format`（扩展名）而不是 mimetype**，所以要把 mimetype 反查成扩展名：`filetype.get_type(mime=...)` 再取 `.extension`。这是四个模态里唯一需要**双向映射**的（其余三个直接用 mimetype）。
3. **`ImageBlock`/`VideoBlock` 的字段是 `image`/`video` 而 `AudioBlock` 的字段是 `audio`**——三个块类型的字段名不一致，这来自 `base/llms/types.py` 的定义（`ImageBlock.image`、`VideoBlock.video`、`AudioBlock.audio`）。所以这里的构造代码必须逐个查字段名，无法泛化。

而 `TextBlock` 的构造要小心：`self.text_resource.text or ""` —— 若 `text_resource` 存在但 `text` 为 `None`（只有一个 `path` 指向的 txt 文件的 resource），会产出一个**空文本块**。这是「有 resource 就产出块」这个判据的副作用。

**`ImageNode` 是「平字段 + 资源字段」的两套接口并存。**

```python
class ImageNode(TextNode):
    image: Optional[str] = None         # base64
    image_path: Optional[str] = None
    image_url: Optional[str] = None
    image_mimetype: Optional[str] = None
    text_embedding: Optional[List[float]] = None
```

五个平字段，与 `Node.image_resource` 一一对应但**形状不同**。构造函数负责把新写法的 `image_resource` 拆成平字段：

```python
def __init__(self, *args: Any, **kwargs: Any) -> None:
    if "image_resource" in kwargs:
        ir = kwargs.pop("image_resource")
        if isinstance(ir, MediaResource):
            kwargs["image_path"] = ir.path.as_posix() if ir.path else None
            kwargs["image_url"] = ir.url
            kwargs["image_mimetype"] = ir.mimetype
        else:
            kwargs["image_path"] = ir.get("path", None)
            kwargs["image_url"] = ir.get("url", None)
            kwargs["image_mimetype"] = ir.get("mimetype", None)
    mimetype = kwargs.get("image_mimetype")
    if not mimetype and kwargs.get("image_path") is not None:
        extension = Path(kwargs["image_path"]).suffix.replace(".", "")
        if ftype := filetype.get_type(ext=extension):
            kwargs["image_mimetype"] = ftype.mime
    super().__init__(*args, **kwargs)
```

四处：

1. **只搬 `path`/`url`/`mimetype` 三个，`data` 被丢弃**。所以 `ImageNode(image_resource=MediaResource(data=...))` 会让**图像数据消失**——`image` 平字段仍是 `None`。这是一个真实的转换损失：新模型的 `data` 无法表达在 `ImageNode` 的五个平字段里（`image` 字段期望的是 base64 **字符串**，而 `data` 是已编码的 bytes）。

   从平字段到 resource 的方向也不存在自动转换——`ImageNode` 没有把 `image` 输出成 `image_resource.data` 的逻辑。所以**两个方向都是单向且有损**。

2. **`ir.path.as_posix()`** 把路径强制成 POSIX 形式。所以在 Windows 上创建、在 Linux 上读取的节点会得到 `/`-分隔的路径。这对跨平台可复现有利，但会让 Windows 上的 `Path("C:/x/y")` 之后无法直接 `open` 成功（实际上可以，`Path` 接受正斜杠）。

3. **`kwargs["image_url"] = ir.url` 保持 `AnyUrl` 而不是 `str`**。而字段声明是 `Optional[str]`，所以 pydantic 会转成字符串。若 `ir.url` 是 `None` 则 `kwargs["image_url"]=None`——**显式传 None 会覆盖用户可能传的 `image_url`**。即「同时传 `image_resource` 与 `image_url`」时，`image_resource` 的空 url 会把用户的 url 抹掉。

4. **mimetype 兜底猜测在 `super().__init__` 之前**，所以它只对 `image_path` 有效；对 `image`（base64）不猜——因为那段代码只检查 `image_path`。

**`resolve_image` 的返回类型是一个联合，且三个分支的返回值语义不同。**

```python
def resolve_image(self) -> ImageType:
    if self.image is not None:
        return BytesIO(base64.b64decode(self.image))
    elif self.image_path is not None:
        return self.image_path
    elif self.image_url is not None:
        response = requests.get(self.image_url, timeout=(60, 60))
        return BytesIO(response.content)
    else:
        raise ValueError("No image found in node.")
```

`ImageType = Union[str, BytesIO]`。三个分支：

- base64 → 解码后包 `BytesIO`（**内存中**的完整字节）
- 路径 → **直接返回路径字符串**（不读文件！让调用方（如 PIL）自己打开）
- URL → **同步 HTTP 请求**，把响应内容包 `BytesIO`

四处问题值得记：

1. **返回值语义不统一**：两个分支给内存字节，一个分支给路径。所以调用方必须 `isinstance` 判断（`get_content_blocks` 就是这么做的）。
2. **路径分支不验证文件存在**。`resolve_image` 返回路径后，真正的失败发生在下游读文件时——错误位置离成因很远。这与 `ImageDocument` 的构造器形成对照（那里用 `is_image_pil` 提前校验）。
3. **URL 分支是同步阻塞 + 无 SSRF 防护**。`requests.get` 用 `timeout=(60, 60)`（连接 60 秒、读取 60 秒），**没有域白名单、没有大小上限、没有重定向限制**。一个恶意文档里的 `image_url` 可以让服务端去请求内部地址（内网探测），或拉取超大文件打爆内存。这是一个**真实的攻击面**，而它在 `get_content_blocks` 这个「构造送给模型的块」的常规路径上——即只要把节点喂给多模态模型就会触发。
4. **异常信息是「No image found in node.」**——但这是 `ImageNode`，一个图像节点没有图像是构造错误而不是运行时状态。用 `ValueError` 而非专门异常类型，调用方无法区分「没有图」与「图取不到」。

**`IndexNode` 是「指向任意对象的节点」，而它的序列化是单向有损的。**

```python
class IndexNode(TextNode):
    index_id: str
    obj: Any = None
```

它的用途写在类文档里：「Node with reference to any object. This can include other indices, query engines, retrievers.」即内容不是文本而是**一个指针**——这让「索引里再放索引」成为可能。

序列化：

```python
def _serialize_obj(self) -> Any:
    try:
        if self.obj is None:
            return None
        elif isinstance(self.obj, BaseNode):
            return doc_to_json(self.obj)
        elif isinstance(self.obj, BaseModel):
            return self.obj.model_dump()
        else:
            return json.dumps(self.obj)
    except Exception:
        raise ValueError("IndexNode obj is not serializable: " + str(self.obj))
```

四支：`None` 原样、节点走 `doc_to_json`、pydantic 模型走 `model_dump`、其余尝试 `json.dumps`。全包在 `try` 里，失败重抛成带内容的 `ValueError`。

反序列化：

```python
@classmethod
def from_dict(cls, data, **kwargs):
    output = super().from_dict(data, **kwargs)
    obj = data.get("obj")
    parsed_obj = None
    if isinstance(obj, str):
        parsed_obj = TextNode(text=obj)
    elif isinstance(obj, dict):
        try:
            parsed_obj = json_to_doc(obj)
        except Exception:
            parsed_obj = TextNode(text=str(obj))
    output.obj = parsed_obj
    return output
```

三处关键：

1. **`str` 被当成文本内容包成 `TextNode`**。所以序列化时 `json.dumps("hello")` 得到 `'"hello"'`，反序列化时它是个 `str`，被包成 `TextNode(text="hello")`。**原来那个字符串直接丢了**。
2. **`dict` 先试 `json_to_doc`**（即假设它是个节点），失败则 `TextNode(text=str(obj))`——**一个字典被压成字符串**。
3. **因此 `IndexNode` 的 `obj` 永远无法还原成原对象**。`obj` 可能是查询引擎、检索器、任意 Python 对象，它们都不可序列化。所以这个字段的实际语义是「序列化后是一个可读的替身」，而**引用本身在往返中丢失**。

这个设计有一个可辩的合理性：`IndexNode` 的主要用途是「索引嵌套」，而嵌套关系中真正需要持久化的是 `index_id`（它指向另一个索引结构），`obj` 更像「本地缓存」。但源码没写这条，而 `from_dict` 的实现让人可能误以为能往返。

**`NodeWithScore` 是检索结果的标准载体，而它有两处「静默降级」。**

```python
class NodeWithScore(BaseComponent):
    node: SerializeAsAny[BaseNode]
    score: Optional[float] = None
```

`SerializeAsAny[BaseNode]` 是一个必要的 pydantic 注解：没有它，序列化只会输出 `BaseNode` 声明的字段，**子类的 `text`、`image` 等字段会全部丢失**。加上它让 pydantic 序列化**实际类型**的字段。

三处静默降级：

1. **`get_score(raise_error=False)` 在 `score is None` 时返回 `0.0`**。所以「没有分数」与「分数是 0」在默认调用下无法区分。要区分必须传 `raise_error=True` 并捕获异常。这与 b03（langchain 的 `usage_metadata=None`）那篇的对比很有意思：那里用 `None` 严格区分，这里默认折成 0。

2. **`text` 与 `get_text` 在节点不是 `TextNode` 时抛 `ValueError("Node must be a TextNode to get text."`)**。所以一个 `ImageNode` 会被拒——尽管 `ImageNode` **继承自 `TextNode`**，所以实际上能过。真正会被拒的是裸 `Node`（多模态基类）。而这个错误消息把「不是文本」说成「不是 `TextNode`」，措辞不准确。

3. **`text` 属性返回 `self.node.text`（原始正文），而 `get_text()` 返回 `self.node.get_text()`（`get_content(MetadataMode.NONE)`）**。对 `TextNode` 两者相同；但这些是两条不同的路径，若某个子类覆写了其中之一就会分叉。

`__str__` 的分数格式是 `f"{self.score: 0.3f}"` 或 `"None"`——三位小数固定格式。这对齐了「分数用于显示」的用途，但也意味着**内部精度高于显示精度**（`0.4999` 显示成 `0.500`）。

**`Document` 与 `ImageDocument` 的对照揭示了「哪个类做校验」的不一致。**

`ImageDocument` 是 `Document` 的图像版本，五个平字段全部变成**属性**，各自读写 `image_resource`：

```python
@property
def image(self) -> str | None:
    if self.image_resource and self.image_resource.data:
        return self.image_resource.data.decode("utf-8")
    return None

@image.setter
def image(self, image: str) -> None:
    self.image_resource = MediaResource(data=image.encode("utf-8"))
```

四处：

1. **`image` 属性把 `data` 用 utf-8 解码**。因为 `data` 存的是 base64（纯 ASCII），解码得到 base64 字符串。所以 `document.image` 是 base64 文本，与 `ImageNode.image` 的语义一致。
2. **setter 把字符串编码成 utf-8 再交给 `MediaResource`**，而 `MediaResource.validate_data` 会检查它是否已是合法 base64——因为是，所以原样保留。**这条链自洽**，且能正确往返。
3. **`image_path` 的 setter 会整体替换 `image_resource`**，丢掉原有的 `image`/`image_url`。所以同时设 `image` 与 `image_path` 只会留下后者。
4. **`image_mimetype` 的 setter 是唯一保留 resource 的**——它只改 `mimetype` 字段。

而 `ImageDocument.__init__` 与 `ImageNode.__init__` 的**校验强度不同**：

```python
elif image_path:
    if not is_image_pil(image_path):
        raise ValueError("The specified file path is not an accessible image")
    kwargs["image_resource"] = MediaResource(path=image_path, mimetype=image_mimetype)
elif image_url:
    if not is_image_url_pil(image_url):
        raise ValueError("The specified URL is not an accessible image")
```

即 `ImageDocument` **构造时就验证路径/URL 真的是可打开的图片**（`is_image_url_pil` 会发请求），而 `ImageNode` 什么都不验（只在 `resolve_image` 里发请求，且那里的失败是 `requests` 抛的）。

这个不一致有两面：`ImageDocument` 的早起校验把错误挡在近处（好），但它**在构造阶段就发网络请求**（坏——构造一个对象不该有远程副作用，且 `is_image_url_pil` 会阻塞）。而 `ImageNode` 的延迟校验把副作用推到了使用阶段。

`resolve_image(as_base64=False)` 在 `ImageDocument` 上多一个参数，语义是「要不要把结果重新 base64 化」：

```python
if self.image_resource.data is not None:
    if as_base64:
        return BytesIO(self.image_resource.data)   # data 已是 base64
    return BytesIO(base64.b64decode(self.image_resource.data))
elif self.image_resource.path is not None:
    img_bytes = self.image_resource.path.read_bytes()
    if as_base64:
        return BytesIO(base64.b64encode(img_bytes))
    return BytesIO(img_bytes)
```

所以两个分支的 base64 语义是**相反的**：`data` 分支在 `as_base64=True` 时**直接返回已编码的**，`path` 分支在 `as_base64=True` 时才**现编码**。逻辑自洽（两者都输出 base64），但这要求读者记住「data 存的是 base64、path 存的是原始」。

而 `ImageDocument.resolve_image` 与 `ImageNode.resolve_image` 的返回类型也不同：前者**永远返回 `BytesIO`**（路径分支会 `read_bytes()`），后者可能返回**路径字符串**。同名方法、同名参数（不含 `as_base64`）、不同语义——这是一个真实的接口不一致。

## 关键取舍

**源码事实：** `MediaResource` 用六个字段同时表达「原始字节 / base64 / 路径 / URL / 文本 / 向量」，不强制互斥，`hash` 把全部非 `None` 来源都算进去。

**源码事实：** `data` 带 `exclude=True`，序列化后原始字节消失；若 resource 既无 `path` 也无 `url`，落库后内容不可恢复。

**源码事实：** `data` 校验器的行为是「已是合法 base64 就保留，否则把输入的字节当原始数据编码成 base64」，用 `validate=True` 避免 `b64decode` 对随机二进制的误判；但短二进制仍可能误判，故「data 一定是 base64」不是严格不变量。

**源码事实：** `mimetype` 校验器先从 `data` 内容猜（`filetype.guess` 读魔数）、再从 `path` 扩展名猜；它读 `info.data`，因此**依赖 `data` 字段在类里声明在 `mimetype` 之前**。两处 `b64decode` 的 `validate` 用法不一致。

**源码事实：** `MediaResource.hash` 用 `<empty_string>` 标记把 `text=""` 与 `text=None` 区分开，全空时返回空字符串（而 `Node.hash` 不过滤空串）。

**源码事实：** `Node` 用四个固定字段（`text_`/`image_`/`audio_`/`video_resource`）而非列表，故每模态最多一个；`Modality` 枚举在 core 内除定义外零引用。

**源码事实：** `Node.get_content_blocks` 把四个 resource 映射到 `TextBlock`/`ImageBlock`/`AudioBlock`/`VideoBlock`，其中音频需要把 mimetype 反查成扩展名（`AudioBlock` 的字段是 `format`），且 `text_resource` 存在但 `text` 为 `None` 时会产出空文本块。

**源码事实：** `ImageNode.__init__` 把 `image_resource` 拆成平字段时**只搬 path/url/mimetype，丢弃 data**；且显式赋 `None` 会覆盖用户传的 `image_url`。`ImageNode.resolve_image` 三分支返回值语义不一致（两个给 `BytesIO`、一个给路径字符串），路径分支不校验存在性，URL 分支是同步请求 + 60 秒超时 + 无 SSRF 防护。

**源码事实：** `IndexNode.obj` 的序列化是单向有损的：`str` 反序列化成 `TextNode(text=...)`、`dict` 反序列化成节点或压成字符串，原对象永不还原。

**源码事实：** `NodeWithScore.node` 用 `SerializeAsAny[BaseNode]` 才让子类字段被序列化；`get_score()` 默认把 `None` 折成 `0.0`；`text` 在节点非 `TextNode` 时抛 `ValueError`。

**源码事实：** `ImageDocument` 构造时用 `is_image_pil`/`is_image_url_pil` **提前校验**（且对 URL 会发请求），而 `ImageNode` 完全不校验；两者的 `resolve_image` 返回类型也不同（`ImageDocument` 恒为 `BytesIO`，`ImageNode` 可能是路径字符串）。

**架构推论：** 「一个容器装所有来源」的好处是调用方不必按来源写分支，坏处是**来源之间的冲突无法在类型层面表达**。`MediaResource` 里可以同时有 `data` 与 `url`，而这两者指向的可能是不同内容。`hash` 把它们都算进去算是**承认冲突存在**而不是消除它——所以同一张图通过两条渠道进来会被当成两个不同资源。更严的做法是用判别位联合（如 `data`/`path`/`url` 三选一），但那会破坏「同时带文本表示与图像数据」这个真实需求。

**架构推论：** `data` 的 `exclude=True` 是本页最重的一处取舍。它换来「docstore 不会被 base64 撑爆」，代价是**节点在落库后不再自足**。于是「持久化一个多模态节点」这件事必须同时持久化 `path` 或 `url`，否则重启后图像字段就是空的——而且这个损失是**静默的**（反序列化不会报错，只是 `data` 字段不存在）。这类「字段存在但序列化时消失」的设计，要求每一处持久化代码都记得「图像要靠外部来源」这条约定。

**架构推论：** 两个 `resolve_image` 的语义分叉（一个是取字节、一个是给路径/取字节）说明了**同名方法在不同类里可以承担不同契约**。而更值得注意的是：`ImageNode` 的路径分支不读文件是为了「让 PIL 自己打开、省内存」，`ImageDocument` 却读进内存。这个差异大概来自它们在不同年代实现——一个面向单图、一个面向批量文档。这类差异一旦形成就会固化，因为改任何一侧都影响既有调用方。

**架构推论：** `IndexNode` 单向有损的序列化，是「引用语义无法持久化」的一般性问题。`obj` 可能是任意 Python 对象，而持久化格式只能是 JSON——所以必然降级。源码的选择是「降级成最接近的可读形式」（`TextNode`），而不是报错拒绝序列化。这保住了「索引结构能在重启后加载」这个能力；代价是加载后 `obj` 已不是原来的东西。**判据是「加载后能否继续工作」而不是「能否精确还原」**——对「`index_id` 才是真正指针」的设计来说，前者成立。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/schema.py` | `MediaResource` | 六字段媒体容器，`data` 序列化时排除 |
| `llama-index-core/llama_index/core/schema.py` | `MediaResource.validate_data` | 「已是 base64 则保留，否则编码」用 `validate=True` 抗误判 |
| `llama-index-core/llama_index/core/schema.py` | `MediaResource.validate_mimetype` | 先读魔数再按扩展名猜，依赖字段声明顺序 |
| `llama-index-core/llama_index/core/schema.py` | `MediaResource.hash` | 四个来源全参与，空串用 `<empty_string>` 标记 |
| `llama-index-core/llama_index/core/schema.py` | `EmbeddingKind` | `"dense"` / `"sparse"` 的多向量键 |
| `llama-index-core/llama_index/core/schema.py` | `Modality` | 四模态枚举，core 内除定义外零引用 |
| `llama-index-core/llama_index/core/schema.py` | `Node` / `Node.get_content_blocks` | 四资源槽位与到 LLM 块类型的映射 |
| `llama-index-core/llama_index/core/schema.py` | `ImageNode` | 平字段兼容层，`image_resource` 拆解丢 data |
| `llama-index-core/llama_index/core/schema.py` | `ImageNode.resolve_image` | 三分支返回语义不一致，URL 分支同步请求无防护 |
| `llama-index-core/llama_index/core/schema.py` | `IndexNode` | 指向任意对象，`obj` 序列化单向有损 |
| `llama-index-core/llama_index/core/schema.py` | `ImageDocument` | 图像文档：属性全部转发到 `image_resource`，构造即校验 |
| `llama-index-core/llama_index/core/schema.py` | `NodeWithScore` | 检索结果载体：`SerializeAsAny` + 分数默认折零 |

## 参考

- [llama-index 源码：schema.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/schema.py)
- [llama-index 文档：多模态检索](https://docs.llamaindex.ai/en/stable/module_guides/models/multi_modal/)
- [Python 文档：base64.b64decode 的 validate 参数](https://docs.python.org/3/library/base64.html#base64.b64decode)
- [Pydantic 文档：SerializeAsAny](https://docs.pydantic.dev/latest/api/base_model/#pydantic.SerializeAsAny)

## 与主核 / AI 的关系

`MediaResource` 与主核（多模态数据库内核）的**大对象存储**是同一类问题的两种解法。主核的典型做法是：小的值内联在元组里，大的值走溢出页或外部存储（`TOAST` 式），元组里只留一个指针。`MediaResource` 做的是同一件事的两端——`data` 内联（但序列化时被排除）、`path`/`url` 当指针。差别在于**主核的「内联 vs 外置」由大小阈值自动决定**，而 `MediaResource` 让调用方自己选。

本页最值得对照主核的一处是 `data` 的 `exclude=True`。主核里对应的是「溢出页在备份时不进主文件」这类策略——两者都在处理「一份数据不该在两个地方同时存两份完整副本」。主核的处理是**位置上分离**（主元组只留指针，数据在溢出页，两者都在库内），而 `MediaResource` 的处理是**序列化时丢弃**（数据只在内存里存在）。后者的代价明显更大：一旦序列化，数据就没地方找了。这也解释了为什么 `MediaResource` 必须同时支持 `path`/`url`——**它们不是「另一种输入」，而是序列化后唯一的存活方式**。

`ImageNode.resolve_image` 的 URL 分支值得与主核的**外部表访问**对比。主核读外部表时要经过一层「可访问性 + 凭证 + 限额」的约束；而这里直接 `requests.get(url, timeout=(60,60))`，没有任何约束。两者都承认「必须访问外部资源」，但只有主核把这种访问当成需要治理的行为。这是一个真实的差距，也是这类框架在服务端部署时的典型风险点。

`IndexNode` 的引用有损序列化，与主核的**游标**同构。`DECLARE CURSOR` 之后游标可跨语句持有状态，但它不能跨连接、跨重启——因为游标的本质是「服务端的一个句柄」，序列化它没有意义。`IndexNode.obj` 同理：它指向进程内的对象，落盘时只能存一个替身。**判据都是「这个引用的生命周期是否可能超出它所指向的东西」**。

对 AI 工程的实际意义在于：多模态 RAG 的可行性完全取决于「图像怎么在节点与存储之间往返」这条链是否闭合。这份实现给出的三条经验可直接用：**内容自足与存储体积是对立的，必须显式选一个**（`exclude=True` 就是选了前者）、**媒体容器不要假装互斥**（同时装 `data` 与 `url` 时 hash 要诚实地反映冲突）、以及**取用外部资源必须当受治理的行为**（超时只是最低要求，域白名单与大小上限才是关键）。
