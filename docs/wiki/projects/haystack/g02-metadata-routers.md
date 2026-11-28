---
project: haystack
feature: metadata-routers
title: 元数据与文件类型路由
page_type: feature
schema_version: 1
refs:
  - path: haystack/components/routers/metadata_router.py
    symbols: [MetadataRouter]
  - path: haystack/components/routers/file_type_router.py
    symbols: [FileTypeRouter]
  - path: haystack/components/routers/document_length_router.py
    symbols: [DocumentLengthRouter]
  - path: haystack/components/routers/document_type_router.py
    symbols: [DocumentTypeRouter]
---

# 元数据与文件类型路由

## 一句话职责

四个**完全确定性**的路由器，各用一种「已经写在数据上的事实」分派：`MetadataRouter` 用元数据过滤表达式、`FileTypeRouter` 用文件/字节流的 MIME 类型、`DocumentLengthRouter` 用 `content` 的字符数、`DocumentTypeRouter` 用文档元数据里的 MIME 类型或文件路径推断。

## 机制是什么

四个组件的共同点是**判据完全来自输入自身的字段，不需要外部知识**。而它们的分野在于「字段从哪来、判什么」：

| 组件 | 输入类型 | 判据来源 | 输出桶 |
|---|---|---|---|
| `MetadataRouter` | `Document` **或** `ByteStream` | 任意元数据字段的过滤表达式 | 用户命名的路由 + `unmatched` |
| `FileTypeRouter` | `Path` / `str` / `ByteStream` | MIME（路径推断或 ByteStream 自带） | 用户声明的 MIME 模式 + `unclassified` / `failed` |
| `DocumentLengthRouter` | `Document` | `len(content)` | 固定两个：`short_documents` / `long_documents` |
| `DocumentTypeRouter` | `Document` | 元数据里的 MIME 或元数据里的文件路径 | 用户声明的 MIME 模式 + `unclassified` |

**三处成对的设计值得先记：**

- `FileTypeRouter` 与 `DocumentTypeRouter` 判据几乎相同（都是 MIME + 正则），差别是**输入类型**：前者吃文件路径与字节流，后者吃 `Document`。**文件类型判断发生在解析之前，所以要处理"还没有 Document"的形态**——这正是 `e04` 那页转换器的上游。
- `MetadataRouter` 与 `d02`（过滤策略页）用的是**同一套过滤表达式语法**，但用途相反：过滤器从集合里**筛出**满足条件的，路由器把集合**分到多个桶**里。
- `DocumentLengthRouter` 是唯一**输出桶固定**的，也是唯一不靠元数据的（直接看内容长度）。

## 怎么实现的

### 一、`MetadataRouter`：一条表达式、允许命中多个桶

**它的规则是「桶名 → 过滤表达式」，而过滤复用文档存储那套语义。**

```
rules: dict[str, dict]   # {"en": {"field": "meta.language", "operator": "==", "value": "en"}}
...
if document_matches_filter(filters=rule, document=doc_or_bytestream, strict_datetime_comparison=self.strict_datetime_comparison):
    output[edge].append(doc_or_bytestream)
    current_obj_matched = True
```

**注意没有 `break`。** 于是**一个对象可以同时命中多个桶** —— 这与 `ConditionalRouter`（首个为真者胜出）根本不同，也与 `DocumentTypeRouter` / `FileTypeRouter`（命中首个模式即 `break`）不同。

**这个差别的后果很实际**：若两条规则都写 `meta.language == "en"`（一个叫 `en`、一个叫 `english`），那个文档会**同时出现在两个输出里**。于是下游会处理它两次。

**这不是缺陷，而是这类路由的语义**：规则之间是**独立的判定**而不是**互斥的分支**。类比 SQL：`MetadataRouter` 像多个独立的 `SELECT ... WHERE`，而 `ConditionalRouter` 像 `CASE WHEN`。

**`current_obj_matched` 那面旗帜只决定是否进 `unmatched`。**

```
unmatched: list[Document] | list[ByteStream] = []
output: dict[str, list[Document] | list[ByteStream]] = {edge: [] for edge in self.rules}
for doc_or_bytestream in documents:
    current_obj_matched = False
    for edge, rule in self.rules.items():
        if document_matches_filter(...):
            output[edge].append(doc_or_bytestream)
            current_obj_matched = True
    if not current_obj_matched:
        unmatched.append(doc_or_bytestream)
output["unmatched"] = unmatched
```

**`unmatched` 在循环之后才被写进 `output`** —— 而 `output` 初始化时只有用户声明的桶。于是「一个都没配」的路由器会返回 `{"unmatched": [...]}`，且**不会因为某个规则恰好叫 `unmatched` 而冲突**——因为构造期就拦了：

```
if "unmatched" in rules:
    raise ValueError("The rule name 'unmatched' is reserved for documents that do not match any rule.")
```

**`output_type` 是构造参数而且是类型对象。**

```
def __init__(self, rules: dict[str, dict], output_type: type = list[Document], *, strict_datetime_comparison: bool = False) -> None:
...
component.set_output_types(self, unmatched=self.output_type, **dict.fromkeys(rules, self.output_type))
```

**默认 `list[Document]`，但它能改成 `list[ByteStream]`** —— 因为 `document_matches_filter` 底层接受 `Union[Document, ByteStream]`（代码里那两处 `# type: ignore[arg-type]` 就是在说明这件事）。

**于是同一个组件既能路由「已解析的文档」也能路由「还没解析的字节流」** —— 判据都是元数据，而元数据在两种形态上都存在。这是本页四个组件里唯一一个同时吃两种输入的。

**`strict_datetime_comparison` 被一路透传给过滤器。**

```
:param strict_datetime_comparison: If `True`, timezone-naive and timezone-aware datetimes never match each other. If `False` (the default), the timezone from the aware datetime is copied to the naive one before comparing.
```

**默认「宽松」：把一个带时区的时间的时区抄给不带时区的那个，再比较。** 这个默认值把「时区混用」这个常见数据问题变成「可比较」，代价是可能把 `2023-01-01T00:00:00` 与 `2023-01-01T00:00:00+08:00` 判成相等——而实际上它们相差 8 小时。**要严格就把这个开关打开。**

**构造期校验只有一处：每条规则必须有 `operator`。**

```
for rule in self.rules.values():
    if "operator" not in rule:
        raise ValueError("Invalid filter syntax. See https://docs.haystack.deepset.ai/docs/metadata-filtering for details.")
```

**只查 `operator` 存在**，不查值是否合法（那留给运行期的过滤函数）。这与注释一致：它只挡住「传了一个明显不是过滤表达式的 dict」（比如把 `{field, operator, value}` 少写了 `operator`），并给出文档链接让人自查。

**序列化要单独处理 `output_type`。**

```
return default_to_dict(self, rules=self.rules, output_type=serialize_type(self.output_type), strict_datetime_comparison=self.strict_datetime_comparison)
...
init_params = dict(data.get("init_parameters", {}))
if "output_type" in init_params:
    init_params["output_type"] = deserialize_type(init_params["output_type"])
return default_from_dict(cls, {**data, "init_parameters": init_params})
```

**类型对象不能直接进 YAML，所以要 `serialize_type` / `deserialize_type`。** 而 `from_dict` 里那个 `dict(...)` 浅拷贝与 `g01` 的 `BranchJoiner` 是同一个考虑（不污染调用方数据、保证可重入）。

### 二、`FileTypeRouter`：为什么它吃 `ByteStream` 而不是 `Document`

**它的输入签名说明了它的位置。**

```
def run(self, sources: list[str | Path | ByteStream], meta: dict[str, Any] | list[dict[str, Any]] | None = None) -> dict[str, list[ByteStream | Path]]:
```

**上游是「文件」，不是「文档」** —— 因为它要做的事发生在**解析之前**：`Path` 与 `ByteStream` 还没有内容、没有元数据，只有「它是什么类型的文件」这件事。而这件事恰好决定该用哪个转换器（`e04` 那页的 `MarkdownToDocument` / `HTMLToDocument` / `DOCXToDocument`）。

**于是它的典型拓扑是「`FileTypeRouter` → 各自的转换器 → `DocumentJoiner`」**，也就是说它承担了「转换器选择」这个职责。

**MIME 的来源按类型分两支。**

```
if isinstance(source, str): source = Path(source)
if isinstance(source, Path):
    if not source.exists():
        if self._raise_on_failure: raise FileNotFoundError(f"File not found: {source}")
        logger.warning("File not found: {source}. Skipping it.", source=source)
        mime_types["failed"].append(source)
        continue
    mime_type = _guess_mime_type(source)
elif isinstance(source, ByteStream):
    mime_type = source.mime_type
else:
    raise TypeError(f"Unsupported data source type: {type(source).__name__}")
```

**`Path` 走扩展名推断，`ByteStream` 用它自带的 `mime_type`。** 而 `str` 先被转成 `Path` —— 于是调用方不必关心传哪种。

**注意 `FileNotFoundError` 与 `failed` 桶的两种模式**：`raise_on_failure=True` 时抛错，`False`（默认）则**把这个路径放进 `failed` 桶并继续**。这与 `e04` 转换器「跳过坏文件」的取向一致，但这里多了一个**显式的桶**——让下游能看见「有哪些文件没处理」。

**两个保留名各有明确语义，而它们被分开是有意义的。**

```
reserved = {"unclassified", "failed"}
collisions = reserved.intersection(mime_types)
if collisions:
    names = ", ".join(repr(name) for name in sorted(collisions))
    raise ValueError(f"MIME type(s) {names} are reserved output names in FileTypeRouter ('unclassified' for unmatched sources, 'failed' for unreadable files). Rename the MIME type to something else.")
```

- **`unclassified`**：文件能读，但 MIME 不匹配任何声明。
- **`failed`**：文件**读不到**（不存在，或带 `meta` 时读字节失败）。

**区分这两者让「出现问题」的两种原因不混淆**：一个是「我的规则没覆盖这个类型」（需要加规则），另一个是「这个文件本身有问题」（需要修数据）。

**而 `failed` 桶里放的是 `Path`（原始对象）而不是转换后的 `ByteStream`** —— 因为失败就发生在转换之前（文件不存在），或转换本身（此时没有新对象）。

**精确匹配优先于正则，这是这一页最值得记的一处。**

```
matched = False
if mime_type:
    # Try exact equality first so MIMEs containing regex metacharacters (e.g. the `+` in
    # `image/svg+xml`) match themselves before the regex fallback gets a chance to misread them.
    for bucket_key, pattern in zip(self.mime_types, self.mime_type_patterns, strict=True):
        if mime_type == bucket_key or pattern.fullmatch(mime_type):
            mime_types[bucket_key].append(source)
            matched = True
            break
if not matched:
    mime_types["unclassified"].append(source)
```

**注释给出的例子非常具体：`image/svg+xml` 里的 `+` 是正则量词。** 若只用 `fullmatch`，`image/svg+xml` 这个**模式**会要求 `image/svgx` 后面跟一个或多个 `m`——于是它**匹配不到自己**。

而 `mime_type == bucket_key or pattern.fullmatch(mime_type)` 这个 `or` 解决了它：**字面相同的先命中，正则只在字面不等时才尝试**。

**注意不是「先全部精确匹配一轮、再全部正则一轮」，而是逐个模式「先精确后正则」。** 差别在于**顺序**：`zip` 保留声明顺序，于是「排在前面的模式优先」。若改成两轮，则「所有精确匹配优先于所有正则」——那会让一个排在第 5 位的精确 MIME 抢在排第 1 位的正则之前。当前写法是**以声明的顺序为唯一优先级**，这是更可预期的语义。

**`fullmatch` 而不是 `match`** —— `match` 只从开头匹配，于是 `audio/.*` 不会误中 `audio/x-wav-extra`……实际上 `.*` 会覆盖到末尾，但一个模式 `audio` 用 `match` 会中 `audio/x-wav`，用 `fullmatch` 不会。**`fullmatch` 让「模式」必须覆盖整个 MIME 串**，这是正确的语义。

**`additional_mimetypes` 会污染全局注册表。**

```
if additional_mimetypes:
    for mime, ext in additional_mimetypes.items():
        mimetypes.add_type(mime, ext)
```

**`mimetypes.add_type` 是模块级全局函数** —— 于是这个构造调用会影响**同一进程里所有**用 `mimetypes` 的地方。这是一个有意的取舍（让扩展名推断覆盖非常见类型），但它的作用域是进程级的，不是组件级的。

**而 `meta` 参数会就地构造一份新的 `ByteStream`。**

```
if meta_dict:
    try: source = get_bytestream_from_source(source)
    except Exception as e:
        if self._raise_on_failure: raise e
        logger.warning("Could not read {source}. Skipping it. Error: {error}", source=source, error=e)
        mime_types["failed"].append(source)
        continue
    # `get_bytestream_from_source` hands back a ByteStream source unchanged, so writing the
    # metadata onto it would modify the object the caller still holds. Build a copy instead.
    source = replace(source, meta={**source.meta, **meta_dict}, mime_type=mime_type)
```

**注释解释了这个 `replace` 的必要性**：`get_bytestream_from_source` 对一个 `ByteStream` 输入会**原样返回同一个对象**，于是在它上面写 `meta` 就是改调用方持有的对象。`replace(...)` 造一个副本。

**而 `mime_type=mime_type` 这个显式重设也有原因**：`get_bytestream_from_source` 从路径构造 `ByteStream` 时可能给出不同的 MIME（或 `None`），而路由用的 MIME 是之前推断出来的那个——重设保证「路由判据」与「下游看到的 MIME」一致。

**它还保留了一个向后兼容的再导出。**

```
# We import CUSTOM_MIMETYPES here to prevent breaking change from moving to haystack.utils.misc
from haystack.utils.misc import CUSTOM_MIMETYPES  # noqa: F401
```

**`noqa: F401`（未使用导入）与注释配合**：这个导入不为使用，只为让老的 `from ...file_type_router import CUSTOM_MIMETYPES` 继续可用。这是一个「迁移而不破坏」的手法。

**输出类型有一处刻意的「不精确」。**

```
# the actual output type is list[Union[Path, ByteStream]],
# but this would cause PipelineConnectError with Converters
component.set_output_types(self, unclassified=list[str | Path | ByteStream], failed=list[str | Path | ByteStream], **dict.fromkeys(mime_types, list[str | Path | ByteStream]))
```

**注释说明了原因**：真实类型是 `list[Path | ByteStream]`，但那样写会导致与转换器连接时 `PipelineConnectError`。于是放宽成 `list[str | Path | ByteStream]`。

**这是「类型系统里的必要妥协」**：管道的连线校验要求类型兼容，而过窄的声明会让合法的连接被判为不兼容。**放宽类型是一处用类型精度换取可组合性的交易**，而注释把它标出来了——这比让后来的人猜测为什么不用精确类型好得多。

### 三、`DocumentTypeRouter`：与 `FileTypeRouter` 同一判据、不同输入

**它的 MIME 来源有两条路，而顺序是「先元数据、后路径」。**

```
mime_type = doc.meta.get(self.mime_type_meta_field) if self.mime_type_meta_field else None
file_path = doc.meta.get(self.file_path_meta_field) if self.file_path_meta_field else None
if mime_type is None and file_path:
    # if mime_type is not provided, try to guess it from the file path
    mime_type = _guess_mime_type(Path(file_path))
```

**`mime_type is None and file_path` 这两个条件**：元数据里没有 MIME **且**有路径时才推断。于是「元数据显式给了 MIME」总是优先——这一条很重要，因为文件扩展名可能骗人（一个 `.txt` 里装着 JSON），而元数据里的 MIME 是转换器或上游给出的、更可信的判断。

**而两个字段都可选，但至少要给一个。**

```
if mime_type_meta_field is None and file_path_meta_field is None:
    raise ValueError("At least one of 'mime_type_meta_field' or 'file_path_meta_field' must be provided to determine MIME types.")
```

**这条校验的必要性在于**：两个都不给时，所有文档的 `mime_type` 都是 `None`，于是**全部进 `unclassified`**——一个静默失效的配置。构造期报错把它变成显式失败。

**匹配循环与 `FileTypeRouter` 逐字相同（连注释都一致）：先精确、后正则、`break`、`fullmatch`。**

```
for bucket_key, pattern in zip(self.mime_types, self._mime_type_patterns, strict=True):
    # Match an exact MIME type first, so literal types containing regex
    # metacharacters (e.g. the '+' in 'image/svg+xml') are not misread as
    # a regex; fall back to regex matching for patterns like 'audio/.*'.
    if mime_type == bucket_key or pattern.fullmatch(mime_type):
        mime_types[bucket_key].append(doc)
        matched = True
        break
```

**这里是 `break`，而 `MetadataRouter` 没有。** 两者的语义差别是本页最容易混的一点：

| | 命中多个桶？ | 为什么 |
|---|---|---|
| `MetadataRouter` | **是** | 规则之间是独立判定（每条是一个 WHERE） |
| `FileTypeRouter` / `DocumentTypeRouter` | **否** | 桶是互斥分类（一个文件只有一种 MIME） |

**「一个文件只有一种 MIME 类型」这个事实决定了后者必须 `break`** —— 否则一个 `image/svg+xml` 的文件会同时落进 `"image/.*"` 和 `"image/svg+xml"` 两个桶，下游会对同一份内容处理两次。

**而 `DocumentTypeRouter` 没有 `failed` 桶**，只有 `unclassified`：

```
component.set_output_types(self, unclassified=list[Document], **dict.fromkeys(mime_types, list[Document]))
```

**原因很直接**：它接受的是已经在内存里的 `Document`，没有「文件读不到」这个失败面。**`failed` 桶是 `FileTypeRouter` 独有的**，因为只有它碰文件系统。

### 四、`DocumentLengthRouter`：最简单的那个，而它的阈值有两个边界行为

**全部逻辑只有五行。**

```
for doc in documents:
    if doc.content is None or len(doc.content) <= self.threshold:
        short_documents.append(doc)
    else:
        long_documents.append(doc)
return {"short_documents": short_documents, "long_documents": long_documents}
```

**两个边界行为值得记：**

**第一，`content is None` 被归到「短」而不是报错。** 因为 `None` 无法 `len()`，而「没有内容」在语义上确实是「短」。这与 `e04` 那页 `HTMLToDocument` 对 `text is None` 的处理形成对照：那里**跳过**（因为要产出合法文档），这里**归入短桶**（因为文档已经存在，只是内容为空）。

**第二，判据是 `<=` 而不是 `<`，且 `threshold` 可以是负数。**

```
:param threshold: ... Documents where `content` is None or whose character count is less than or equal to the threshold will be routed to the `short_documents` output. ... To route only documents with None content to `short_documents`, set the threshold to a negative number.
```

**负数阈值是一个被文档明确支持的用法**：`-1` 时 `len(content) <= -1` 永假，于是**只有 `content is None` 的进短桶**。这把「筛出无内容的文档」表达成了一个纯阈值设定，不必加额外参数。

**而「小于等于而不是小于」的选择也有实际含义**：`threshold=10` 时 10 字符的文档算短。文档示例里 `Document(content="Short")` 是 5 字符，`"Long document "*20` 是 280 字符——两个都远离边界，所以示例没有暴露这个细节。

**它的典型用途在文档里写得很具体：**

> 一个常见用途是处理来自 PDF 的、包含非文本内容（扫描页或图像）的文档。这个组件能检测空或低内容文档并把它们路由到执行 OCR、生成标题、或计算图像嵌入的组件。

**也就是说它的判据是「这段文本太短，说明它其实不是文本」** —— 一个用长度当代理指标的启发式。这与 `e03`（语义切分）用「距离百分位」当代理是同一类手法：**判决要的信息拿不到时，用一个相关的可测量量代替。**

## 关键取舍

**源码事实：** `MetadataRouter` 的规则循环**没有 `break`** —— 一个对象可同时进多个桶；`ConditionalRouter` 是首个为真者胜出，`FileTypeRouter` / `DocumentTypeRouter` 是首个匹配模式即停。三种语义并存。

**源码事实：** `MetadataRouter` 同时接受 `Document` 与 `ByteStream`（底层 `document_matches_filter` 接受 `Union`），通过 `output_type` 参数选择；它是四者里唯一能路由未解析字节流的。

**源码事实：** `MetadataRouter` 的 `strict_datetime_comparison` 默认 `False`：把带时区时间的时区抄给不带时区的再比较。代价是相差 8 小时的两个时间可能被判相等。

**源码事实：** `FileTypeRouter` 吃 `Path`/`str`/`ByteStream` 而非 `Document`——文件类型判断发生在解析之前，所以它承担「选择转换器」的职责，典型拓扑是「它 → 各转换器 → `DocumentJoiner`」。

**源码事实：** `FileTypeRouter` 有两个保留桶：`unclassified`（能读但 MIME 不匹配）与 `failed`（读不到）。分开是为了让「规则没覆盖」与「文件有问题」不混淆。`failed` 桶是它独有的，因为只有它碰文件系统。

**源码事实：** 两个 MIME 路由器都用「先字面相等、再正则 `fullmatch`」的逐模式匹配——`image/svg+xml` 里的 `+` 是正则量词，纯正则会匹配不到自己。而逐模式匹配（而非两轮全扫）意味着**声明顺序是唯一优先级**。

**源码事实：** `additional_mimetypes` 调用模块级的 `mimetypes.add_type`，会污染整个进程的 MIME 注册表，作用域不是组件级的。

**源码事实：** `FileTypeRouter` 在 `meta` 非空时用 `replace` 造新 `ByteStream`，因为 `get_bytestream_from_source` 对 `ByteStream` 输入会原样返回同一对象，就地写 `meta` 会改到调用方持有的对象；同时显式重设 `mime_type` 保证路由判据与下游看到的一致。

**源码事实：** `FileTypeRouter` 的输出类型声明放宽成 `list[str | Path | ByteStream]`，注释说明真实类型是 `list[Path | ByteStream]` 但那样会与转换器连接时报 `PipelineConnectError`。

**源码事实：** `DocumentTypeRouter` 的 MIME 来源优先级是「元数据显式值 > 从文件路径推断」，两个字段至少给一个（否则全部静默进 `unclassified`）。

**源码事实：** `DocumentLengthRouter` 把 `content is None` 归入「短」桶而不报错；判据是 `<=`；且**负数阈值被文档明确支持**——`-1` 时只有 `None` 内容进短桶，用阈值本身表达「只筛无内容文档」。

**架构推论：** 这四个路由器的存在本身就是一条设计宣言：**能在数据上读出来的判断，绝不该交给模型或手写表达式**。它们的判据（元数据、MIME、长度）都是已在字段里的事实，因此完全确定、可复现、零成本。

**而这个「确定」是有前提的**：字段必须真的在。`DocumentLengthRouter` 靠 `content` 长度当「是不是文本」的代理、`DocumentTypeRouter` 靠扩展名当「是什么格式」的代理——**代理失真时判断就失真**，而这些组件不会告诉你代理失效了（它们只按数字比）。这正是 `g03`（LLM 决策路由）存在的理由：当代理不可靠而判断又必须做时，只能上模型。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/components/routers/metadata_router.py` | `MetadataRouter` | 按任意元数据过滤表达式分派，允许一对象多桶 |
| `haystack/components/routers/metadata_router.py` | `output_type` | 在 `list[Document]` 与 `list[ByteStream]` 间切换 |
| `haystack/components/routers/metadata_router.py` | `unmatched` 保留名 | 构造期拒绝用户占用 |
| `haystack/components/routers/metadata_router.py` | `strict_datetime_comparison` | 是否允许跨时区比较 |
| `haystack/components/routers/file_type_router.py` | `FileTypeRouter` | 按 MIME 分派 `Path`/`ByteStream`，带 `failed` 桶 |
| `haystack/components/routers/file_type_router.py` | 精确优先匹配 | `mime_type == bucket_key or pattern.fullmatch(...)` |
| `haystack/components/routers/file_type_router.py` | `_guess_mime_type` 分支 | 路径走扩展名推断、`ByteStream` 用自带 MIME |
| `haystack/components/routers/file_type_router.py` | `replace` | 造 `ByteStream` 副本，避免改调用方对象 |
| `haystack/components/routers/file_type_router.py` | 输出类型放宽 | 为与转换器可连接而用更宽的类型声明 |
| `haystack/components/routers/file_type_router.py` | `CUSTOM_MIMETYPES` 再导出 | 迁移后保持向后兼容 |
| `haystack/components/routers/document_length_router.py` | `DocumentLengthRouter` | 按 `len(content)` 二分，`None` 归短、支持负阈值 |
| `haystack/components/routers/document_type_router.py` | `DocumentTypeRouter` | 按元数据 MIME 或文件路径推断 MIME 分派 |
| `haystack/components/routers/document_type_router.py` | MIME 来源优先级 | 显式元数据 > 路径推断，两字段至少给一个 |

## 参考

- [haystack 源码：routers/metadata_router.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/routers/metadata_router.py)
- [haystack 源码：routers/file_type_router.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/routers/file_type_router.py)
- [haystack 文档：元数据过滤](https://docs.haystack.deepset.ai/docs/metadata-filtering)
- [Python 文档：mimetypes](https://docs.python.org/3/library/mimetypes.html)
- [haystack 文档：路由](https://docs.haystack.deepset.ai/docs/routers)

## 与主核 / AI 的关系

这四个路由器与主核里的**分区裁剪**是同一件事：给定一条数据与一组「桶」的定义，决定它落到哪个桶。数据库的分区裁剪也完全确定——按分区的范围/列表/哈希定义判断，不需要任何统计信息或外部知识。

**`MetadataRouter` 的过滤表达式与 `d02`（过滤策略）用的是同一套语法，这正是「同一机制、两种用法」的例子**：过滤是「从一个集合里取出满足条件的」，路由是「把集合按条件切成多个桶」。数据库里对应的是 `WHERE` 与 `PARTITION BY`——同一组谓词，一个用来筛、一个用来分。

**而 `FileTypeRouter` 的「精确优先于正则」有一条数据库级的对应**：索引选择时**等值谓词优先于范围谓词**，因为等值能用更精确的访问路径。这里的动机相同但更朴素——`image/svg+xml` 里的 `+` 会被正则误读，所以字面相等要先试。

对 agent 工程的直接价值有三条。其一，**「一对象多桶」与「一对象一桶」必须按语义选**。`MetadataRouter` 的规则是独立判定（可以叠加，比如「是英文」且「是 PDF」两个桶），而 MIME 路由是互斥分类（一个文件只有一种类型）。选错的表现是「同一份内容被下游处理两次」。其二，**失败要分桶而不是混在一起**。`unclassified`（规则没覆盖）与 `failed`（文件读不到）的区分，让排查时一眼知道该改规则还是改数据。其三，**用长度/扩展名当代理指标时要知道它会失真**。`DocumentLengthRouter` 判「是不是文本」、`DocumentTypeRouter` 判「是什么格式」都只是代理——代理失效时这些组件会给出错误但确定的答案，而它们不会报警。
