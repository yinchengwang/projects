---
project: graphrag
feature: chunking-workflow
title: 文档分块工作流
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/index/workflows/create_base_text_units.py
    symbols: [create_base_text_units, chunk_document]
  - path: packages/graphrag/graphrag/index/text_splitting/text_splitting.py
    symbols: [TokenTextSplitter, split_single_text_on_tokens]
  - path: packages/graphrag/graphrag/index/workflows/load_input_documents.py
    symbols: [load_input_documents]
---

# 文档分块工作流

## 一句话职责

`load_input_documents` 把文件读成 `documents` 表（分配 `human_readable_id`），`create_base_text_units` 把每条文档的 `text` 按 token 切成块并**逐块写进 `text_units` 表**（id 是 `text` 的 sha512），`chunk_document` 负责可选地把源文档元数据前置到每块开头；而 `text_splitting.py` 里的 `TokenTextSplitter` 是**上一代实现，全仓没有任何调用点**。

## 机制是什么

分块这一步在 graphrag 里的定位与「普通 RAG 的切块」有一点根本差别：**它切出来的东西会成为全图的引用锚点。**

文本单元（`text_unit`）是实体、关系、社区三者共同指向的东西——一个实体之所以可信，是因为它出现在某个文本单元里；一条关系之所以可信，是因为它出现在某个文本单元里。于是「切多碎」这个参数同时决定了三件事：

1. **抽取质量**：块越大，LLM 一次能看到的上下文越多，跨句关系越容易被抽到；但块太大会撞上模型上下文上限，也会让单次抽取的注意力被稀释。
2. **引用精度**：检索到实体后回指原文时，给用户看的是整块文本。块越小，指得越准，但越可能断章取义。
3. **成本**：块数 × 每块一次 LLM 调用（抽取），块数直接乘在 token 花费上。

默认值 `size=1200` / `overlap=100` 就是这三者的折中：1200 token 大致是一篇短文的长度，100 的重叠约 8%，足以让跨块边界的一句话在两个块里都完整出现。

而这一步的**实现形态**是这一页真正的重点：它被重写成「流式读一行、切、写若干行」，而不是「读全表、切、写全表」。理由写在 `create_base_text_units` 的 docstring 里——「avoiding loading all data into memory」。

## 怎么实现的

**`load_input_documents` 只做三件事，其中一件是给一个整数 id。**

```
async def load_input_documents(
    input_reader: InputReader, documents_table: Table, sample_size: int = 5
) -> tuple[pd.DataFrame, int]:
    """Load and parse input documents into a standard format."""
    sample: list[dict] = []
    idx = 0

    async for doc in input_reader:
        row = asdict(doc)
        row["human_readable_id"] = idx
        if "raw_data" not in row:
            row["raw_data"] = None
        await documents_table.write(row)
        if len(sample) < sample_size:
            sample.append(row)
        idx += 1

    return pd.DataFrame(sample), idx
```

四点：

1. **`human_readable_id` 是读入顺序的序号**。它不是内容哈希、不是文件名——就是 `0, 1, 2, ...`。这意味着**换一个文件顺序重跑，同一份文档的 `human_readable_id` 就变了**。它是给提示词与报表用的短名（a03 讲过 `short_id` 的用途），而不是稳定标识。
2. **`raw_data` 补 `None`**。这是为了让所有文档行有相同的列集合——表写入是按第一行的键确定表头的，缺键会导致某些格式下丢列。
3. **返回值只有前 5 行样本 + 总数**。`sample` 用来在日志/回调里展示「读到了什么」，不是给下游的数据通路——真正的数据已经写进表了。这是这套流水线的**统一约定**：workflow 的 `result` 是样本与统计，真实产物在存储里。
4. **零文档直接抛错**：

```
if total_count == 0:
    msg = "Error reading documents, please see logs."
    logger.error(msg)
    raise ValueError(msg)
```

**错误消息里没有告诉用户「去哪看什么」**——只说「请看日志」。这是这条链路上唯一一处「诊断信息不足」的地方，但它的位置合理：真正的原因（目录空了、pattern 不匹配、编码错）在 `InputReader` 里，这里拿不到。

**`create_base_text_units` 的骨架是「打开两张表 → 流式读 → 逐块写」。**

```
async def run_workflow(config, context) -> WorkflowFunctionOutput:
    tokenizer = get_tokenizer(encoding_model=config.chunking.encoding_model)
    chunker = create_chunker(config.chunking, tokenizer.encode, tokenizer.decode)

    async with (
        context.output_table_provider.open("documents") as documents_table,
        context.output_table_provider.open("text_units") as text_units_table,
    ):
        total_rows = await documents_table.length()
        sample_rows = await create_base_text_units(
            documents_table, text_units_table, total_rows,
            context.callbacks,
            tokenizer=tokenizer, chunker=chunker,
            prepend_metadata=config.chunking.prepend_metadata,
        )
    return WorkflowFunctionOutput(result=sample_rows)
}
```

**注意 `create_chunker` 的签名把 `encode` / `decode` 作为单独参数传入，而不是让 chunker 自己去拿 tokenizer。**

```
def create_chunker(
    config: ChunkingConfig,
    encode: Callable[[str], list[int]] | None = None,
    decode: Callable[[list[int]], str] | None = None,
) -> Chunker:
    config_model = config.model_dump()
    if encode is not None:
        config_model["encode"] = encode
    if decode is not None:
        config_model["decode"] = decode
    chunker_strategy = config.type

    if chunker_strategy not in chunker_factory:
        match chunker_strategy:
            case ChunkerType.Tokens:
                from graphrag_chunking.token_chunker import TokenChunker
                register_chunker(ChunkerType.Tokens, TokenChunker)
            case ChunkerType.Sentence:
                from graphrag_chunking.sentence_chunker import SentenceChunker
                register_chunker(ChunkerType.Sentence, SentenceChunker)
            case _:
                msg = f"ChunkingConfig.strategy '{chunker_strategy}' is not registered in the ChunkerFactory. Registered types: {', '.join(chunker_factory.keys())}."
                raise ValueError(msg)

    return chunker_factory.create(chunker_strategy, init_args=config_model)
```

三处值得记：

**第一，`pydantic` 配置被 `model_dump()` 摊平成字典，再把可调用对象塞进同一个字典传下去。** 于是「配置字段」与「运行时依赖（encode/decode）」走同一条构造通道。这样做的直接好处是注册的自定义 chunker 只需要一个接受任意 kwargs 的构造器；代价是**类型信息在这一步丢了**——`chunker_factory.create(..., init_args=dict)` 无法在编译期保证参数对得上。

**第二，内建 chunker 是「用到才注册」。** `if chunker_strategy not in chunker_factory` 之后才 import 并注册。这让「TokenChunker 依赖 tiktoken」这件事只在真用 token 分块时才付费。而 `case _` 的兜底错误消息列出了**已注册的类型**——不过在首次使用时这个列表是空的（因为内建的一个都还没注册），于是消息会变成「Registered types: 。」这种信息量很低的形态。

**第三，`ChunkingConfig` 声明了 `extra="allow"`。**

```
class ChunkingConfig(BaseModel):
    model_config = ConfigDict(extra="allow")
    """Allow extra fields to support custom cache implementations."""
```

于是 yaml 里给自定义 chunker 的参数不会被 pydantic 拒绝，而是原样保留进 `model_dump()` 再传给构造器。**这条是「可插拔」能成立的关键**：没有它，自定义 chunker 的任何参数都会被校验器挡掉。而注释里写的「custom cache implementations」是笔误（应为 chunker），一处小瑕疵。

**`create_base_text_units` 的核心循环只有十几行。**

```
tick = progress_ticker(callbacks.progress, total_rows)
doc_index = 0
sample_rows: list[dict[str, Any]] = []
sample_size = 5

async for doc in documents_table:
    chunks = chunk_document(doc, chunker, prepend_metadata)
    for chunk_text in chunks:
        if chunk_text is None:
            continue
        row = {
            "id": "",
            "document_id": doc["id"],
            "text": chunk_text,
            "n_tokens": len(tokenizer.encode(chunk_text)),
        }
        row["id"] = gen_sha512_hash(row, ["text"])
        await text_units_table.write(row)
        if len(sample_rows) < sample_size:
            sample_rows.append(row)
    doc_index += 1
    tick()
```

四处：

1. **`id` 先写空串再覆盖**——因为 `gen_sha512_hash(row, ["text"])` 要拿整个 row 当输入。空串占位保证「哈希前的 row 形状」与「写入的 row 形状」一致（键集合相同）。这是「按内容哈希」的必然写法：**哈希的输入必须是最终行的子集**，否则以后加一个字段就会让所有 id 变化。
2. **`gen_sha512_hash(row, ["text"])` 只对 `text` 取哈希**，参数 `["text"]` 是「用哪些列算」的白名单。于是**同一段文本出现在两份不同文档里会得到同一个 text_unit id**。这不是 bug——它让去重自动发生（表写入按 id 覆盖），也意味着 `document_id` 那一列在重复文本时只保留最后一次写入的值。
3. **`n_tokens` 是重新 `encode` 一遍算的**，而 chunker 内部其实已经算过（`create_chunk_results` 的 `token_count`）。这次重复编码是个可观测的浪费——但因为 chunker 的返回类型是字符串列表（`chunk_document` 里 `[chunk.text for chunk in chunker.chunk(...)]`），`token_count` 在那一步就被丢掉了。
4. **`if chunk_text is None: continue`** ——chunker 的契约是返回 `list[str]`，这里的 `None` 检查是防御性的：它暗示某处可能产出 `None`（例如 sentence chunker 对空文本）。没有注释说明，属于「不确定但防御一下」的代码。

**`chunk_document` 的元数据前置是一个可选的、按字段名收集的动作。**

```
def chunk_document(doc, chunker, prepend_metadata=None) -> list[str]:
    transformer = None
    if prepend_metadata:
        document = TextDocument(
            id=doc["id"],
            title=doc.get("title", ""),
            text=doc["text"],
            creation_date=doc.get("creation_date", ""),
            raw_data=doc.get("raw_data"),
        )
        metadata = document.collect(prepend_metadata)
        transformer = add_metadata(metadata=metadata, line_delimiter=".\n")

    return [chunk.text for chunk in chunker.chunk(doc["text"], transform=transformer)]
```

三点：

1. **`prepend_metadata` 是一个字段名列表**（配置里 `list[str] | None`，默认 `None`）。它指定「从源文档里取哪几个字段前置到每块开头」。docstring 说用途是「Optional metadata fields to prepend to each chunk」——典型用法是把 `title` 前置，让每一块都带着「这段来自哪篇文档」的信息，**因为检索命中的是块，而块本身不知道自己属于谁**。
2. **`TextDocument(...)` 是把 dict 行重新包成一个类型化对象**，只为了调 `collect(字段名列表)`。也就是说元数据收集逻辑挂在 `TextDocument` 上，而这里只有 `doc` 这个 dict。
3. **`add_metadata(..., line_delimiter=".\n")`** 决定前置文本与正文之间的分隔形态：一个句点加换行。选 `".\n"` 而不是单纯换行，是因为抽取提示词是按「句子/段落」理解文本的，加一个句点能让前置行成为一个独立句子而不是被当作标题接续。

**返回类型被压平成 `list[str]`，`TextChunk` 的其余字段全被丢掉。**

`chunker.chunk()` 的返回是 `list[TextChunk]`，每个 `TextChunk` 有五个字段：

```
original: str     # 变换前的原文
text: str         # 变换后的最终文本
index: int        # 块在文档内的序号
start_char: int   # 在源文档中的起始字符位置
end_char: int     # 结束字符位置
token_count: int | None
```

而 `chunk_document` 只取 `chunk.text`。于是 `index` / `start_char` / `end_char` / `original` **在写表这一步就永久丢了**——`text_units` 表的列里没有它们（`TEXT_UNITS_FINAL_COLUMNS` 是 `id / human_readable_id / text / n_tokens / document_id / entity_ids / relationship_ids / covariate_ids`）。

**这是一个真实的取舍**：保留字符偏移可以让「回指原文」精确到字符，代价是表多三列且偏移在拼接元数据后语义会变（前置的元数据文本不属于原文）。graphrag 选了不保留——回指原文靠 `document_id` 定位整篇，而不是靠字符位置。

**`create_chunk_results` 的字符偏移计算有一个前提，而那个前提在有 overlap 时不成立。**

```
def create_chunk_results(chunks, transform=None, encode=None) -> list[TextChunk]:
    """Create chunk results from a list of text chunks. The index assignments are 0-based and assume chunks were not stripped relative to the source text."""
    results = []
    start_char = 0
    for index, chunk in enumerate(chunks):
        end_char = start_char + len(chunk) - 1  # 0-based indices
        result = TextChunk(
            original=chunk,
            text=transform(chunk) if transform else chunk,
            index=index,
            start_char=start_char,
            end_char=end_char,
        )
        if encode:
            result.token_count = len(encode(result.text))
        results.append(result)
        start_char = end_char + 1
    return results
```

**它把相邻块当作「首尾相接、无重叠」。** `start_char` 永远是上一块的 `end_char + 1`。而默认配置 `overlap=100`，相邻块在源文本里重叠 100 个 token——于是从第二块起，`start_char` / `end_char` 全都偏小（少算了累计的重叠量）。

docstring 只承认了「assume chunks were not stripped relative to the source text」（字符数与源文本对齐），没有承认「chunks 之间无重叠」。所以**在默认配置下这些偏移就是错的**。它没有造成可观测问题，正是因为 `chunk_document` 把偏移丢掉了——**一个算错的字段被一个丢弃字段的调用点掩盖了**。

**`transform` 只影响 `text`，不影响 `original`，也不影响偏移。**

```
original=chunk,
text=transform(chunk) if transform else chunk,
```

于是「原始文本」与「最终文本」都被保留在 `TextChunk` 里，而偏移是按 `original`（= `chunk`）算的。这是正确的：位置是相对于源文档的，而源文档里有的就是原文。但这也意味着**前置了元数据的块，其 `text` 长度与 `end_char - start_char` 不相等**——任何按 `text` 长度推位置的下游都会错。

**`TokenTextSplitter` 是上一代实现，全仓零调用点。**

```
class TokenTextSplitter(ABC):
    """Token text splitter class definition."""

    _chunk_size: int
    _chunk_overlap: int
    _length_function: LengthFn
    _keep_separator: bool
    _add_start_index: bool
    _strip_whitespace: bool

    def __init__(
        self,
        # based on OpenAI embedding chunk size limits
        # https://devblogs.microsoft.com/azure-sql/embedding-models-and-dimensions-optimizing-the-performance-resource-usage-ratio/
        chunk_size: int = 8191,
        chunk_overlap: int = 100,
        length_function: LengthFn = len,
        keep_separator: bool = False,
        add_start_index: bool = False,
        strip_whitespace: bool = True,
        tokenizer: Tokenizer | None = None,
    ):
```

**检索整仓可以确认：`TokenTextSplitter` 与 `split_single_text_on_tokens` 除了定义处之外没有任何引用**，真正在用的是 `graphrag_chunking` 包里的 `TokenChunker` + `split_text_on_tokens`。两者是逐行相同的算法：

```
# text_splitting.py 的 split_single_text_on_tokens
start_idx = 0
cur_idx = min(start_idx + tokens_per_chunk, len(input_ids))
chunk_ids = input_ids[start_idx:cur_idx]
while start_idx < len(input_ids):
    chunk_text = decode(list(chunk_ids))
    result.append(chunk_text)
    if cur_idx == len(input_ids):
        break
    start_idx += tokens_per_chunk - chunk_overlap
    cur_idx = min(start_idx + tokens_per_chunk, len(input_ids))
    chunk_ids = input_ids[start_idx:cur_idx]
```

```
# token_chunker.py 的 split_text_on_tokens
start_idx = 0
cur_idx = min(start_idx + chunk_size, len(input_tokens))
chunk_tokens = input_tokens[start_idx:cur_idx]
while start_idx < len(input_tokens):
    chunk_text = decode(list(chunk_tokens))
    result.append(chunk_text)
    if cur_idx == len(input_tokens):
        break
    start_idx += chunk_size - chunk_overlap
    cur_idx = min(start_idx + chunk_size, len(input_tokens))
    chunk_tokens = input_tokens[start_idx:cur_idx]
```

**唯一的差别是变量名（`input_ids`/`chunk_ids` 与 `input_tokens`/`chunk_tokens`）。** 这个文件是 `graphrag_chunking` 包抽出去之前的老版本，抽包之后没有删除。

而它自己还有几个「老版本残留」的特征：

- **`__init__` 的默认值 `chunk_size=8191`**，注释说是「based on OpenAI embedding chunk size limits」——**这是嵌入模型的上限，不是抽取模型的上限**。当前配置的默认值是 `1200`（`ChunkingDefaults.size`），差了近七倍。这两个默认值服务的目标不同：8191 是「不要超过嵌入模型能收的长度」，1200 是「抽取时给 LLM 一段合适的上下文」。
- **六个实例属性里有四个没有被任何代码读取**：`_length_function` / `_keep_separator` / `_add_start_index` / `_strip_whitespace` 全都在 `__init__` 里赋了值，而 `split_text` 只用 `_chunk_overlap` / `_chunk_size` / `_tokenizer` / `_encode` / `_decode`。也就是说**构造参数里有四个是纯装饰**——它们的存在暗示这个类曾有过更复杂的实现（LangChain 的 `TextSplitter` 那套），被简化后没有清理接口。
- **`split_text` 里的类型检查顺序有点怪**：

```
if isinstance(text, list):
    text = " ".join(text)
elif cast("bool", pd.isna(text)) or text == "":
    return []
if not isinstance(text, str):
    msg = f"Attempting to split a non-string value, actual is {type(text)}"
    raise TypeError(msg)
```

`pd.isna(text)` 对 `None` 返回 `True`（于是返回 `[]`），对 `""` 也返回 `False` 所以要看 `or text == ""`；而如果 `text` 是 `numpy.ndarray`，`pd.isna` 返回数组，`cast` 只是骗类型检查器——`or` 对数组求值会抛「truth value is ambiguous」。这个 `cast("bool", ...)` 正是知道自己绕过了类型系统却没真正安全的标志。

**`TokenChunker` 的 `**kwargs` 吞掉了全部有多余配置。**

```
class TokenChunker(Chunker):
    def __init__(self, size, overlap, encode, decode, **kwargs: Any) -> None:
        self._size = size
        self._overlap = overlap
        self._encode = encode
        self._decode = decode
```

`create_chunker` 把 `config.model_dump()`（含 `type` / `encoding_model` / `prepend_metadata` 以及 `extra="allow"` 带进来的任意自定义字段）整个作为 `init_args` 传进来，而 `TokenChunker` 只取出 `size` / `overlap` / `encode` / `decode`，其余全被 `**kwargs` 丢掉。**这让「配置里多写一个字段」不会导致构造失败**，但也意味着**拼错的字段名会被静默忽略**（`sise: 800` 会安静地不生效）。

## 关键取舍

**源码事实：** `create_base_text_units` 是流式的：`async for doc in documents_table` → 切 → `await text_units_table.write(row)`，逐行写；docstring 说明目的是「avoiding loading all data into memory」。workflow 的返回值只是前 5 行样本，真实产物在表里。

**源码事实：** text unit 的 id 是 `gen_sha512_hash(row, ["text"])`——只对 `text` 一列取哈希，且先把 `id` 置空串再覆盖（保证哈希输入与最终行键集合一致）。因此相同文本在不同文档中会得到同一个 id，写入时按 id 覆盖。

**源码事实：** `n_tokens` 在 `create_base_text_units` 里重新 `len(tokenizer.encode(chunk_text))` 算一遍，而 chunker 内部的 `TextChunk.token_count` 已经算过——因为 `chunk_document` 用 `[chunk.text for chunk in ...]` 把整个 `TextChunk` 压平成字符串，其余字段（含 `token_count`）被丢弃。

**源码事实：** `chunk_document` 只取 `chunk.text`，于是 `TextChunk` 的 `index` / `start_char` / `end_char` / `original` 在写表前就丢了；`text_units` 表也没有这些列。回指原文只能靠 `document_id` 定位整篇。

**源码事实：** `create_chunk_results` 把相邻块当作首尾相接（`start_char = end_char + 1`），docstring 只承认「assume chunks were not stripped relative to the source text」，未承认「块之间无重叠」；默认 `overlap=100` 时从第二块起偏移累计偏小——这个错误之所以无后果，是因为调用点丢弃了这些字段。

**源码事实：** `chunk_document` 在 `prepend_metadata` 非空时把 dict 行重新包成 `TextDocument` 以调用 `collect(字段名列表)`，再用 `add_metadata(metadata, line_delimiter=".\n")` 生成 transformer；`transform` 只改 `text`，不改 `original`，因此前置元数据后 `len(text)` 与 `end_char - start_char` 不再相等。

**源码事实：** `create_chunker` 用 `config.model_dump()` 摊平配置、把 `encode`/`decode` 塞进同一个 dict 当 `init_args`；`ChunkingConfig` 声明 `extra="allow"` 以让自定义 chunker 的参数通过校验；内建 chunker 是「首次使用时才 import + 注册」。

**源码事实：** `TokenChunker.__init__` 用 `**kwargs: Any` 吞掉多余配置，于是拼错的配置字段名被静默忽略。

**源码事实：** `index/text_splitting/text_splitting.py` 的 `TokenTextSplitter` 与 `split_single_text_on_tokens` 全仓**零调用点**，算法与 `graphrag_chunking` 包的 `TokenChunker` / `split_text_on_tokens` 逐行相同（仅变量名不同）；其默认 `chunk_size=8191`（注释称来自嵌入模型上限）与当前配置默认 `1200` 相差近七倍；六个实例属性中四个（`_length_function` / `_keep_separator` / `_add_start_index` / `_strip_whitespace`）从未被读取。

**源码事实：** `load_input_documents` 用读入顺序给 `human_readable_id` 编号（不是稳定标识），给缺失的 `raw_data` 补 `None`，零文档时抛 `ValueError`（消息只说「please see logs」）。

**架构推论：** 「按内容哈希当 id」这个选择把去重变成了 O(1) 的写覆盖，但代价是**同一段文本只保留最后一次写入的 `document_id`**。对「一段文字被两篇文档引用」这种真实情况，`text_units` 表会丢掉其中一个来源。若引用完整性重要，需要的是「id 基于 text + document_id」的复合哈希——但那会让一模一样的两段话变成两个块，抽取成本翻倍。

**架构推论：** `create_chunk_results` 那处偏移错误揭示了一个通用模式：**一个字段被算错却无人发现，通常不是因为它没用，而是因为它唯一的消费点也把它丢了**。这类「双重失效」在重构时特别危险——一旦有人恢复使用 `start_char`（比如为了实现精确高亮），就会拿到从第二块起全错的偏移，而且看不出是哪里错的。

**架构推论：** 老实现留在仓库里且零调用，是重构（抽出 `graphrag_chunking` 包）的典型残渣。它的危害不只是占空间：它带着**误导性的默认值**（8191）与**有误导性的名字**（`TokenTextSplitter` 听起来还是当前实现），任何按名字搜索的人会先撞到它。判断准则：**抽出独立包之后，旧文件应当删除；若要保留作为参考，就移到明确的 archive 位置。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/index/workflows/load_input_documents.py` | `load_input_documents` | 流式读入文档、分配 `human_readable_id`、补 `raw_data`、零文档抛错 |
| `packages/graphrag/graphrag/index/workflows/create_base_text_units.py` | `create_base_text_units` | 流式切块并逐行写 `text_units`，id = `text` 的 sha512 |
| `packages/graphrag/graphrag/index/workflows/create_base_text_units.py` | `chunk_document` | 可选元数据前置 + 把 `TextChunk` 压平成字符串列表 |
| `packages/graphrag/graphrag/index/workflows/create_base_text_units.py` | `run_workflow` | 建 tokenizer 与 chunker、打开两张表、组装输出样本 |
| `packages/graphrag/graphrag/index/text_splitting/text_splitting.py` | `TokenTextSplitter` | 上一代分块器（零调用点），四个构造参数从未被读取 |
| `packages/graphrag/graphrag/index/text_splitting/text_splitting.py` | `split_single_text_on_tokens` | 与 `TokenChunker` 逐行相同的滑窗切分算法 |
| `packages/graphrag-chunking/graphrag_chunking/token_chunker.py` | `TokenChunker` / `split_text_on_tokens` | 当前实现：token 滑窗 + `create_chunk_results` |
| `packages/graphrag-chunking/graphrag_chunking/create_chunk_results.py` | `create_chunk_results` | 把字符串块包装成 `TextChunk`（含字符偏移，假定无重叠） |
| `packages/graphrag-chunking/graphrag_chunking/text_chunk.py` | `TextChunk` | 块结果：`original` / `text` / `index` / `start_char` / `end_char` / `token_count` |
| `packages/graphrag-chunking/graphrag_chunking/chunker_factory.py` | `create_chunker` / `register_chunker` | 配置 → chunker 实例；内建实现按需注册 |
| `packages/graphrag-chunking/graphrag_chunking/chunking_config.py` | `ChunkingConfig` | 分块配置（`extra="allow"` 支持自定义参数） |

## 参考

- [graphrag 源码：create_base_text_units.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/index/workflows/create_base_text_units.py)
- [graphrag 源码：text_splitting.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/index/text_splitting/text_splitting.py)
- [graphrag 文档：索引——文本单元](https://microsoft.github.io/graphrag/index/outputs/)
- [tiktoken 文档（`o200k_base` 编码）](https://github.com/openai/tiktoken)

## 与主核 / AI 的关系

分块在主核（多模态数据库内核）里对应的是**页分裂与记录打包**：都是「把不定长的逻辑单位切成定长的物理单位，同时保留回指原位置的能力」。四条经验。

**第一，按内容哈希当主键会丢掉「来源」这一个维度。** 这里 `id = sha512(text)` 让相同文本自动去重，代价是 `document_id` 只剩最后一个写入者。主核做内容寻址存储（CAS）时会遇到同一个问题：同一份数据块被多处引用时，「它从哪来」与「它是什么」是两件事，用一个哈希只能记后者。**若需要来源可追溯，就必须在哈希之外维护一张「内容 → 引用者集合」的表。**

**第二，切分单元的偏移量在「有重叠」时不能靠累加算。** `create_chunk_results` 用 `start_char = 上一块 end_char + 1` 推位置，而块之间重叠 100 token——从第二块起全错。主核里记录页内偏移、WAL 记录位置时同理：**任何「由前一项推后一项位置」的逻辑，都必须先确认两项之间没有重叠或间隙**。有重叠就必须用显式位置（切分器本来就有起始 token 下标，只是没传下来）。

**第三，重构抽包之后旧实现必须删或归档。** 这里的 `TokenTextSplitter` 与当前实现逐行相同、零调用，却带着一个差了七倍的默认值（8191 对 1200）和一个模糊了新旧的名字。主核里凡是「旧模块留下、新模块接管」的迁移，都应当把旧文件移进明确的 archive 路径或直接删除——**留着它等于在代码库里放了一个会误导搜索的诱饵**。

**第四，`**kwargs` 吞参数让拼写错误变成静默失效。** `TokenChunker.__init__(..., **kwargs)` 让「配置里多写/写错字段」既不报错也不生效。主核解析配置时同理：**可选参数应当显式声明，未知键应当告警或报错**。若确实要支持自定义实现的未知参数，至少应保留一份「本次被忽略的键」清单并打日志——否则用户改了配置发现没反应，只能靠读源码才知道为什么。
