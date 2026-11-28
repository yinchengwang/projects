---
project: graphrag
feature: input-and-chunking
title: 输入读取与分块器
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag-input/graphrag_input/csv.py
    symbols: [CSVFileReader]
  - path: packages/graphrag-chunking/graphrag_chunking/chunker.py
    symbols: [Chunker]
---

# 输入读取与分块器

## 一句话职责

两个各只有 24–45 行的接口，划出索引链路最前端的两个可替换环节：`InputReader` 把存储里的文件读成 `TextDocument` 列表，`Chunker` 把一段文本切成 `TextChunk` 列表——**两者的共同产出都是「带字符偏移的文本」，而偏移的准确性决定了后续所有溯源信息能不能对齐**。

## 机制是什么

这两个接口的分工沿着一条清晰的线：

| | `InputReader` | `Chunker` |
|---|---|---|
| 输入 | 一个文件（通过 `Storage` 抽象，见 f02 那页） | 一段字符串 |
| 输出 | `list[TextDocument]`（每行一份） | `list[TextChunk]`（每块一份） |
| 一条输入产出 | **多份文档** | **多份块** |
| 是否异步 | 是（`async def read_file`） | **否**（同步 `def chunk`） |

**分块的同步性是刻意的**：它是对内存里字符串的纯计算，没有 I/O。**而读取必须异步**，因为文件可能来自云存储。

**四个环节共享同一个「偏移」概念：**

| 环节 | 偏移字段 | 语义 |
|---|---|---|
| `TextChunk` | `start_char` / `end_char` | 块在**源文档**里的字符区间 |
| `TextChunk` | `index` | 块在**这份文档**里的序号（0 起） |
| `TextChunk` | `original` / `text` | 变换**前**与变换**后**的文本 |
| `TextDocument` | `raw_data` | 原始行（供 `get(field)` 取任意列） |

**`original` 与 `text` 分开存是这套设计的核心。** 因为分块后常要对文本做归一化（`transform`），而偏移必须对应**原始文本**——否则下游按偏移定位会错。

## 怎么实现的

### 一、`TextChunk`：五个必需字段 + 一个可选

```
@dataclass
class TextChunk:
    original: str      # Raw original text chunk before any transformation.
    text: str          # The final text content of this chunk.
    index: int         # Zero-based index of this chunk within the source document.
    start_char: int    # Character index where the raw chunk text begins in the source document.
    end_char: int      # Character index where the raw chunk text ends in the source document.
    token_count: int | None = None
```

**`end_char` 的语义是「最后一个字符的下标」而不是「之后一个位置」** —— 这一点从 `create_chunk_results` 的实现可以确证：

```
end_char = start_char + len(chunk) - 1  # 0-based indices
...
start_char = end_char + 1
```

**即 `end_char` 是**闭区间**的右端。** 这与 Python 的切片惯例（`s[a:b]` 中 `b` 是开区间）相反，是一个**容易踩的边界**——用 `text[start:end]` 会丢掉最后一个字符。

**而注释 `# 0-based indices` 就写在那一行**，说明作者意识到这个约定需要显式说明。

**`token_count` 是可选的原因是「算它要花钱」。** `create_chunk_results` 只在传了 `encode` 时才算：

```
if encode:
    result.token_count = len(encode(result.text))
```

**而算的是 `text`（变换后）而不是 `original`** —— 因为下游关心的是「发给模型的那份要多少 token」。

### 二、`create_chunk_results`：把字符串列表变成带偏移的块

**七个分块器实现里有三个共用它（`TokenChunker` / `SentenceChunker` / 其它），而它的全部逻辑是累加。**

```
def create_chunk_results(chunks, transform=None, encode=None) -> list[TextChunk]:
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

**而它的 docstring 里有一句极重要的限定：**

> The index assignments are 0-based and **assume chunks were not stripped relative to the source text**.

**「假设块没有被 strip 过」是这套偏移计算的唯一前提。** 若某个分块器在产出块之前做了 `chunk.strip()`，那么 `len(chunk)` 就小于它在原文里占据的长度，**后续所有块的偏移都会往前偏**。

**这正是 `SentenceChunker` 必须做偏移修正的原因**（见下）。

### 三、`TokenChunker`：一个容易读错的循环

**分块主循环只有十行，而它的边界条件值得逐步核对。**

```
input_tokens = encode(text)

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

**四个要点：**

1. **步长是 `chunk_size - chunk_overlap`** —— 经典滑动窗口。`overlap=0` 时步长等于块大小，块之间不重叠。
2. **`cur_idx` 用 `min(..., len(input_tokens))` 截断** —— 于是最后一块可能短于 `chunk_size`。
3. **`if cur_idx == len(input_tokens): break`** —— **这是防止最后产出一个纯重叠块的关键**。若不 break，`start_idx` 会继续增加而 `cur_idx` 已到末尾，下一轮 `chunk_tokens` 就只剩重叠部分，产出一个「全是上一块尾巴」的块。
4. **`decode(list(chunk_tokens))`** —— 把 list 复制一份再传，可能因为某些 tokenizer 的 `decode` 会修改输入（防御性拷贝）。

**这个循环有一个可改进点：`chunk_tokens` 在循环末尾被重新赋值，但第一次进循环前也赋了一次** —— 于是「初始化」与「推进」两处代码重复。可以用 `for` + 切片替代，但当前写法让「块是 token 切片」这件事更直白。

**而它的缺陷在于偏移：`decode(encode(x)[a:b])` 的结果长度不等于 `b - a`。** 一个 token 可能解码成零个或多个字符，而 `create_chunk_results` 假设 `len(chunk)` 就是它在原文里占的长度。

**于是 token 分块的 `start_char` / `end_char` 是**不准确的**。** 这是「用 token 边界分块却用字符偏移记录位置」的固有矛盾——除非像某些实现那样用 `decode_with_offsets` 建立 token→字符的映射表（lightrag 的 `DocumentSplitter` 就是这么做的，见那边 e01 那页）。

**这个接口没有暴露那个能力**，所以 `TokenChunker` 的偏移是尽力而为的。

### 四、`SentenceChunker`：**唯一真正修偏移的实现**

**它的核心是 nltk 分句 + 一段修正循环。**

```
sentences = nltk.sent_tokenize(text.strip())
results = create_chunk_results(sentences, transform=transform, encode=self._encode)
# nltk sentence tokenizer may trim whitespace, so we need to adjust start/end chars
for index, result in enumerate(results):
    txt = result.text
    start = result.start_char
    actual_start = text.find(txt, start)
    delta = actual_start - start
    if delta > 0:
        result.start_char += delta
        result.end_char += delta
        # bump the next to keep the start check from falling too far behind
        if index < len(results) - 1:
            results[index + 1].start_char += delta
            results[index + 1].end_char += delta
```

**注释直接说明了问题：`nltk` 的分句器会剥掉空白**，于是 `create_chunk_results` 的累加假设（「块没被 strip」）不成立。

**而修法是「用 `text.find` 搜索实际位置」。** 四个细节：

1. **`text.find(txt, start)`** —— 从计算出的 `start` 开始找（而不是从头找），于是相同的句子重复出现时也能定位到正确的那一次。
2. **`delta = actual_start - start`** —— 正向偏差（实际位置更靠后，因为前面被剥掉了空白）。
3. **`if delta > 0`** —— **只处理正向偏差**。若 `delta < 0`（`find` 找不到时返回 `-1`，于是 `delta = -1 - start` 是个负数），则不改。**这是一个保护：找不到就不动，而不是把偏移改成负数。**
4. **`results[index + 1]` 也加 `delta`** —— 注释说「让下一个的 start 检查不至于落后太远」。**因为 `create_chunk_results` 已经把下一块的 `start_char` 算成 `本块 end_char + 1`（基于未修正的长度），而本块实际占得更长。** 于是修正要传播一块。

**但这个修正有两个漏洞：**

**漏洞一：`result.text` 可能是被 `transform` 改过的。** 修正循环用的是 `result.text`（变换后），而 `text.find(txt, start)` 在**原文**里找**变换后**的字符串——若 `transform` 改了内容（比如归一化空白），`find` 就找不到（返回 `-1`），偏移不修正。

**而正确的做法应该用 `result.original`** —— 那正是 `TextChunk` 保留 `original` 的用途。**这是一处真实的 bug**（在 `transform` 非 `None` 时触发）。

**漏洞二：`delta` 只传播一块，而不是累计传播。** 若连续两句都被剥掉空白，第二句的修正会基于第一句已修正的 `start_char`（因为 `results[index+1].start_char` 被加了 `delta`）——这恰好是对的。但 `end_char` 也一起加了 `delta`，而 `end_char` 的累加依赖于「前一块的长度」，**这个长度在修正里没有被更新**。**于是多块连续修正时会累积误差。**

**`bootstrap()` 在构造时被调用。**

```
def __init__(self, encode=None, **kwargs):
    self._encode = encode
    bootstrap()
```

**它是「确保 nltk 的分句模型已下载」的副作用** —— 一个构造期做网络/磁盘操作的调用。**这解释了为什么 `SentenceChunker` 的 `encode` 是可选的**（分句不需要 tokenizer），而 `TokenChunker` 的 `encode` / `decode` 是必需参数。

### 五、`InputReader`：模板方法模式与「跳过坏文件」

**基类的 `__init__` 是具体的，而 `read_file` 是抽象的。**

```
def __init__(self, storage: Storage, file_pattern: str, encoding: str = "utf-8", **kwargs):
    self._storage = storage
    self._encoding = encoding
    self._file_pattern = file_pattern
```

**它持有 `Storage` 而非路径** —— 于是输入也可以来自云存储（见 f02 那页）。

**而 `_iterate_files` 是一个模板方法。**

```
files = list(self._storage.find(re.compile(self._file_pattern)))
if len(files) == 0:
    msg = f"No {self._file_pattern} matches found in storage"
    logger.warning(msg)
    return

for file in files:
    try:
        for doc in await self.read_file(file):
            doc_count += 1
            yield doc
    except Exception as e:  # noqa: BLE001
        logger.warning("Warning! Error loading file %s. Skipping...", file)
        logger.warning("Error: %s", e)
```

**四个决定：**

1. **`find` 是同步的**（见 f02 那页的接口不一致），所以这里 `list(...)` 把它整个物化。**于是所有文件名先被列进内存**——对大规模输入是一个可扩展性问题。
2. **空匹配只 `warning` 不抛错** —— 于是「目录里没有 CSV」被当成「零文档」而不是错误。
3. **单个文件失败被捕获并跳过**（`except Exception` + `logger.warning`）—— **这与「零匹配只警告」一致：输入阶段的失败是数据问题，不该让整个索引崩溃。**
4. **`__aiter__` 让它可以直接 `async for doc in reader`** —— 而 `read_files()` 只是 `[doc async for doc in self]` 的物化版本。**于是「流式处理」与「一次取全」是两个入口，共一份实现。**

**这个「跳过坏文件」的取向与 `graphrag-llm` 的「检索失败必须传播」相反**，而判据同样是「失败后还能不能诚实地继续」：一个坏文件只是若干份文档里的一份（跳过它，其余照常索引，而 warning 留痕）；而一次检索失败意味着这一步的结果不完整。

### 六、`CSVFileReader`：45 行里有一个全局副作用

**它的实现只有四行有效代码。**

```
class CSVFileReader(StructuredFileReader):
    def __init__(self, file_pattern=None, **kwargs):
        super().__init__(file_pattern=file_pattern if file_pattern is not None else ".*\\.csv$", **kwargs)

    async def read_file(self, path: str) -> list[TextDocument]:
        file = await self._storage.get(path, encoding=self._encoding)
        reader = csv.DictReader(io.StringIO(file))
        rows = list(reader)
        return await self.process_data_columns(rows, path)
```

**默认 `file_pattern` 是 `".*\\.csv$"`** —— 注意它用的是 `$(行尾)`，而 `Storage.find` 是在**绝对路径字符串**上做 `search`（见 f02 那页），所以这个模式能匹配任何以 `.csv` 结尾的路径。

**而模块顶部有一个全局副作用：**

```
try:
    csv.field_size_limit(sys.maxsize)
except OverflowError:
    csv.field_size_limit(100 * 1024 * 1024)
```

**`csv.field_size_limit` 是模块级全局设置，默认上限是 131072 字节（128 KiB）** —— 一个长文本单元格很容易超。**先试 `sys.maxsize`，溢出则退回 100 MiB。**

**为什么会有 `OverflowError`**：在 Windows 上 `sys.maxsize` 是 2^63-1，而 `csv.field_size_limit` 的底层接受 C long（32 位），于是传 2^63-1 会溢出。**这个 try/except 是跨平台兼容的补丁。**

**而它是**导入时就执行**的** —— 于是「导入 `graphrag_input.csv`」会改变**整个进程**里所有 CSV 读取的限制。**这是一个作用域远大于本模块的副作用**（与 `FileTypeRouter` 的 `mimetypes.add_type` 同类，见 haystack 那轮的 g02）。

**`process_data_columns` 在基类里，它做三件事。**

```
for index, row in enumerate(rows):
    text = get_property(row, self._text_column)
    id = get_property(row, self._id_column) if self._id_column else gen_sha512_hash({"text": text}, ["text"])
    num = f" ({index})" if len(rows) > 1 else ""
    title = get_property(row, self._title_column) if self._title_column else f"{path}{num}"
    creation_date = await self._storage.get_creation_date(path)
    documents.append(TextDocument(id=id, title=title, text=text, creation_date=creation_date, raw_data=row))
```

**三个决定：**

1. **`text` 是必需的**（`get_property(row, self._text_column)` 没有默认值），而 `id` 与 `title` 可选。
2. **`id` 的默认值是「只对 text 做 sha512」** —— 于是**两行内容相同会得到同一个 id**。这是一个真实的冲突源：CSV 里两条完全相同的文本会被视为同一份文档（下游去重会丢掉一条）。
3. **`title` 的默认值是「路径 + (index)」，而 `(index)` 只在一行文件有多行时才加** —— 单行文件的 title 就是路径，多行文件的 title 是 `path (0)`、`path (1)`……

**注意 `creation_date` 在循环内被**重复获取**（每行一次），而它对同一个文件是不变的。** 这是一个可以提出循环外的调用，若 `get_creation_date` 在云后端上是一次网络请求，则 N 行就 N 次请求。

**`raw_data=row` 保留整行**，于是 `TextDocument.get("任意列名")` 能取到它 —— 这是「输入列可以直接在配置里引用」的实现基础（如抽取时用 `title` 或某个自定义列作为实体名来源）。

### 七、两个工厂：同一套骨架的第五、第六个实例

**`create_chunker` 与 `create_input_reader` 与另外五个包的工厂逐行同构**（懒注册 + `match` + 兜底列出已注册类型）。

**而 `create_chunker` 有一个独有的处理：把 `encode` / `decode` 注入配置字典。**

```
config_model = config.model_dump()
if encode is not None:
    config_model["encode"] = encode
if decode is not None:
    config_model["decode"] = decode
chunker_strategy = config.type
...
return chunker_factory.create(chunker_strategy, init_args=config_model)
```

**为什么需要单独注入**：`encode` / `decode` 是**函数**，不是可序列化的配置项——它们来自 tokenizer 实例，无法写进 YAML。**于是工厂在 `model_dump()` 之后手工塞进去。**

**这与 `create_retry` 的 `init_args = retry_config.model_dump()` 是同一个手法的两个面**：那边假设「构造参数名与配置字段名逐字一致」，这边必须额外处理那些「不可能来自配置」的参数。

**而 `SentenceChunker` 没有 `decode`** —— 它只接受 `encode`（且可选）。**于是工厂无条件传两个键时，`SentenceChunker.__init__` 会用 `**kwargs` 吞掉多余的 `decode`。** 这是一个「统一注入 + 各自容忍」的设计。

## 关键取舍

**源码事实：** `TextChunk.end_char` 是**闭区间右端**（`start + len - 1`），与 Python 切片惯例相反；`create_chunk_results` 里那行写着 `# 0-based indices`。

**源码事实：** `create_chunk_results` 的 docstring 声明前提：「假设块相对源文本没有被 strip 过」——这是偏移累加能成立的唯一条件。

**源码事实：** `TextChunk` 同时存 `original` 与 `text`，因为 `transform` 会改内容而偏移必须对应原文。

**源码事实：** `token_count` 只在传入 `encode` 时计算，且算的是 `text`（变换后）而非 `original`。

**源码事实：** `TokenChunker` 的步长是 `chunk_size - chunk_overlap`，`if cur_idx == len(input_tokens): break` 是为了防止最后产出一个纯重叠块。

**源码事实：** `decode(encode(x)[a:b])` 的字符长度不等于 token 区间长度，**于是 `TokenChunker` 的字符偏移是尽力而为、并不准确**——除非用 token→字符偏移表（lightrag 的切分器就这么做），而这个接口没有暴露该能力。

**源码事实：** `SentenceChunker` 是唯一真正修偏移的实现，因为 nltk 分句会剥空白；它用 `text.find(txt, start)` 搜索实际位置、只处理 `delta > 0`、并把 `delta` 也加到下一块上。

**源码事实（缺陷）：** 修正循环用的是 `result.text`（变换后）去原文里 `find`——`transform` 改动内容时 `find` 返回 `-1`，偏移不修正；正确应使用 `result.original`。此外 `end_char` 的累加依赖前块长度而修正里没更新该长度，连续多块修正会累积误差。

**源码事实：** `SentenceChunker.__init__` 调用 `bootstrap()`（确保 nltk 模型已下载），这是构造期的网络/磁盘副作用。

**源码事实：** `InputReader._iterate_files` 是模板方法：`find` 全量物化文件名、零匹配只 `warning`、单文件失败被 `except Exception` 捕获并跳过。`__aiter__` 与 `read_files()` 是同一实现的流式与物化两个入口。

**源码事实：** `CSVFileReader` 模块顶部 `csv.field_size_limit(sys.maxsize)` 带 `OverflowError` 回退到 100 MiB——因为 Windows 上 C long 装不下 2^63-1。该调用是**模块级全局副作用**，改变整个进程的 CSV 单元格上限。

**源码事实：** `CSVFileReader` 默认 `file_pattern = ".*\\.csv$"`；`process_data_columns` 里 `id` 默认是对 text 做 sha512（**两行内容相同会得到同一 id，下游去重会丢一条**）、`title` 默认是 `path (index)`（仅多行文件加索引）、`creation_date` **在循环内每行获取一次**（同一文件不变，是可提出循环外的调用）。

**源码事实：** `create_chunker` 在 `config.model_dump()` 之后手工注入 `encode` / `decode`——因为它们是函数，无法来自配置文件；`SentenceChunker` 用 `**kwargs` 吞掉多余的 `decode`。

**架构推论：** 这两个接口加起来不到七十行，但**偏移的准确性贯穿整条链路**。分块的 `start_char`/`end_char` 决定下游能否把实体溯源回原文位置；一旦偏移偏了，症状是「引用对不上」而不是「报错」，而这类错误在质量评估里几乎不可见。**所以这个接口最重要的一条约束（`end_char` 的开闭、`original` 与 `text` 的分离）必须写在 docstring 里，因为类型系统表达不了它。**

**架构推论：** 「块是从原文切片还是重建」是一个根本性的分岔。`TokenChunker` 重建（`decode` 出字符串），于是偏移只能靠长度累加猜；而若从原文切片（用 token 区间换算出字符区间），偏移天然准确。**lightrag 的递归切分器两种都实现过**（word 版切片、char/token 版重建），而它的注释明确说切片版保留了空白与分页符。**这是一个被两边代码都确认过的取舍：重建容易写，切片偏移准。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag-chunking/graphrag_chunking/chunker.py` | `Chunker` | 抽象：同步 `chunk(text, transform)` |
| `packages/graphrag-chunking/graphrag_chunking/text_chunk.py` | `TextChunk` | `original`/`text`/`index`/`start_char`/`end_char`/`token_count` |
| `packages/graphrag-chunking/graphrag_chunking/create_chunk_results.py` | `create_chunk_results` | 长度累加算偏移（闭区间右端） |
| `packages/graphrag-chunking/graphrag_chunking/token_chunker.py` | `TokenChunker` / `split_text_on_tokens` | 滑动窗口 + 防纯重叠块 |
| `packages/graphrag-chunking/graphrag_chunking/sentence_chunker.py` | `SentenceChunker` | nltk 分句 + `find` 修偏移（有 transform 时的缺陷） |
| `packages/graphrag-chunking/graphrag_chunking/bootstrap_nltk.py` | `bootstrap` | 构造期确保 nltk 模型就位 |
| `packages/graphrag-chunking/graphrag_chunking/chunker_factory.py` | `create_chunker` | 注入 `encode`/`decode` 后按类型装配 |
| `packages/graphrag-input/graphrag_input/input_reader.py` | `InputReader` | 模板方法 + 跳过坏文件 + 异步迭代 |
| `packages/graphrag-input/graphrag_input/structured_file_reader.py` | `StructuredFileReader.process_data_columns` | text 必需、id/title 有默认值 |
| `packages/graphrag-input/graphrag_input/csv.py` | `CSVFileReader` | 模块级 `field_size_limit` 副作用 |
| `packages/graphrag-input/graphrag_input/text_document.py` | `TextDocument` | `get(field)` 支持点号嵌套 + `raw_data` 兜底 |
| `packages/graphrag-input/graphrag_input/input_reader_factory.py` | `create_input_reader` | 六种输入类型的懒注册 |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 源码：graphrag_chunking](https://github.com/microsoft/graphrag/tree/main/packages/graphrag-chunking/graphrag_chunking)
- [NLTK 文档：sent_tokenize](https://www.nltk.org/api/nltk.tokenize.html)
- [Python 文档：csv 模块](https://docs.python.org/3/library/csv.html)

## 与主核 / AI 的关系

这一页的两个接口与主核的**记录解析层**（元组→记录的 `heap_getattr`、以及类型输入函数）地位相当：都是「把外部字节变成内部结构」的第一道转换。而差别在「多样性」。

**主核只有一种磁盘格式。** 而 graphrag 有六种输入格式（csv / text / json / jsonl / markitdown / parquet）与两种分块策略——**因为它的输入来自用户的无结构数据，而不是自己写出的页。**

**这个差别决定了错误处理的取向。** 主核遇到损坏的页要做崩溃恢复（因为页是它自己写的，损坏意味着介质或代码有问题）；graphrag 遇到坏文件只是跳过并警告——**因为输入本来就可能不合法，一个坏 CSV 不该让整个索引起不来。**

**`TextChunk` 的 `original`/`text` 分离与数据库的「原始元组 vs 索引后的值」是同构的。** 数据库索引里存的是转换后的值（大小写折叠、规范化），而原始值留在堆里。**两者都必须保留原始形态——因为转换是不可逆的，而某些操作（重建索引、显示原文）需要原文。**

**`end_char` 用闭区间而 Python 用开区间，与数据库里「页内偏移 vs 长度」的表示选择同类。** 两种都能工作，但**混用会差一**。数据库的 PDP 里反复出现「`pd_lower` 是下一个空闲字节的偏移」这类定义——**每个都显式写明是开还是闭**，因为这正是最容易出错的地方。

对 agent 工程的直接价值有三条。其一，**偏移的表示（开/闭）必须写进文档**。`end_char = start + len - 1` 这个闭区间约定与 Python 切片相反，而 `text[start:end]` 会丢最后一个字符——这类错误不会报错，只是结果悄悄少一个字。其二，**`original` 与 `text` 必须都留**。任何对文本的变换都会破坏「按偏移定位」的能力，而保留原文是唯一的补救。其三，**用 token 边界分块却记录字符偏移时，偏移是不准的**。要么用 token→字符的偏移表，要么接受这个不精确并让它不成为溯源的依据——两者之外没有第三条路。