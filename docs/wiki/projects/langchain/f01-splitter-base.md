---
project: langchain
feature: splitter-base
title: 分块器基类与长度度量
page_type: feature
schema_version: 1
refs:
  - path: libs/text-splitters/langchain_text_splitters/base.py
    symbols: [TextSplitter, TokenTextSplitter, Tokenizer, Language]
  - path: libs/core/langchain_core/documents/base.py
    symbols: [Document, Blob, BaseMedia]
---

# 分块器基类与长度度量

## 一句话职责

把 `split_text` / `create_documents` / `split_documents` 与 `merge_splits` 组合成一个完整的分块骨架，并明确——**「长度」不是字符数而是一个可注入的度量函数**（`length_function`），因为分块的真实约束是模型的 token 预算而不是字节数。

## 机制是什么

`TextSplitter` 只是一个抽象基类，它把分块拆成两层：

| 层 | 方法 | 谁实现 |
|---|---|---|
| 切（怎么把文本切开） | `split_text(text) -> list[str]` | **子类必须实现**（抽象方法） |
| 合（怎么把小片段合成不超过上限的块） | `_merge_splits` | 基类实现，含重叠逻辑 |
| 装（怎么把块变成 `Document`） | `create_documents` / `split_documents` | 基类实现 |

也就是说 **子类只负责「用什么分隔符切」**，而「切完的碎片怎么合并、重叠怎么算、元数据怎么带」由基类统一处理。

## 怎么实现的

### 一、构造参数：五个字段与三条校验

**五个参数各管一件事。**

| 参数 | 默认 | 作用 |
|---|---|---|
| `chunk_size` | 4000 | 块的大小上限，**单位由 `length_function` 决定** |
| `chunk_overlap` | 200 | 相邻块的重叠量，同上单位 |
| `length_function` | `len` | 长度度量，默认按字符数 |
| `keep_separator` | `False` | 保留分隔符及其位置（`True` 等价于 `"start"`） |
| `add_start_index` | `False` | 在元数据里记块的起始字符下标 |
| `strip_whitespace` | `True` | 去掉每块首尾空白 |

**三条构造校验的第三条是最有意思的一条。**

```
chunk_size <= 0            → ValueError("chunk_size must be > 0")
chunk_overlap < 0          → ValueError("chunk_overlap must be >= 0")
chunk_overlap > chunk_size → ValueError("Got a larger chunk overlap ... than chunk size ... should be smaller.")
```

允许 `chunk_overlap == chunk_size` 但拒绝 `chunk_overlap > chunk_size`。前两条是边界，第三条有其实际意义：**重叠大于块大小时，合并逻辑的「回退多少」会超过「前进多少」，切片过程可能不终止**。

这个校验放在 `__init__` 而不是合并时，是「早失败」的选择——配置错误不该等到处理一万份文档时才在第一个长文本上暴露。

**`length_function` 默认 `len`，但它决定了 `chunk_size` 的语义。** 若传入 tiktoken 的编码计数，`chunk_size=4000` 就是 4000 个 token；若传 `len`，就是 4000 个字符。**同一个参数名在不同配置下量纲不同**，这是这个基类最重要的一个设计选择，也是它 527 行里最需要读文档的一处。

**`keep_separator` 的三态值是 `bool | Literal["start", "end"]`**，文档写着 `(True='start')`。第三态「放在块尾」的存在是因为某些分隔符（比如句号）在语义上属于前一句，放在块尾读起来更自然。

### 二、`_merge_splits`：重叠的本质是一次「回退」

**这是基类里唯一真正有算法的部分，22 行却值得逐句读。**

```
separator_len = self._length_function(separator)
docs = []; current_doc = []; total = 0
for d in splits:
    len_ = self._length_function(d)
    if total + len_ + (separator_len if len(current_doc) > 0 else 0) > self._chunk_size:
        if total > self._chunk_size: logger.warning("Created a chunk of size %d, which is longer than the specified %d", total, self._chunk_size)
        if len(current_doc) > 0:
            doc = self._join_docs(current_doc, separator)
            if doc is not None: docs.append(doc)
            while total > self._chunk_overlap or (total + len_ + (sep) > self._chunk_size and total > 0):
                total -= self._length_function(current_doc[0]) + (separator_len if len(current_doc) > 1 else 0)
                current_doc = current_doc[1:]
    current_doc.append(d)
    total += len_ + (separator_len if len(current_doc) > 1 else 0)
doc = self._join_docs(current_doc, separator)
if doc is not None: docs.append(doc)
return docs
```

三个机制：

1. **`separator_len` 也被计入总长。** 因为块是 `separator.join(docs)` 出来的，分隔符会真实占据位置。若不算它，最终块会超限。而且注意 `len(current_doc) > 0` / `> 1` 的判断——**分隔符只在「已有内容」或「将有两项以上」时才计**，恰好对应 `join` 的实际行为。

2. **超限时先产出当前块，再「回退」到只剩重叠量。** 那个 `while` 是重叠的实现：不断丢掉最前面的片段，直到 `total <= chunk_overlap`。所以**重叠不是「复制尾部」，而是「保留尾部片段作为下一块的头部」**——这是唯一能保证「按片段为单位」的实现方式，也解释了为什么重叠量是近似值而非精确值。

3. **`while` 的第二个条件是一个不常见的组合。** `total > chunk_overlap or (total + len_ + sep > chunk_size and total > 0)`。第一个条件是常规回退；第二个条件的意思是：**即使已经退到重叠量以内，如果新片段还是装不下，就继续退**。

第二条若不存在会发生什么：一个超大的片段在 `total` 已经很小的情况下仍然放不进当前块，但循环已因 `total <= chunk_overlap` 退出——于是下一轮 `current_doc` 里带上它，再次溢出，再产出一块……结果可能产出大量几乎相同的块。这一步是为了让「单个片段本身就超块大小」的情形至少能前进一格。

**那个 `logger.warning` 只在 `total > chunk_size` 时触发，且是在「准备产出一块」的时刻。** 也就是说它报的是「**刚才这块做出来就超限了**」。这个情形只在「单个片段加上重叠量仍超块大小」时出现——而基类不会试图把单片段再切细，那是子类（递归切分器）的职责。

### 三、`create_documents` 与 `split_documents`：差在「输入形态」

**`split_documents` 的实现只有五句，且它把工作全交给 `create_documents`。**

```
texts, metadatas = [], []
for doc in documents:
    texts.append(doc.page_content); metadatas.append(doc.metadata)
return self.create_documents(texts, metadatas=metadatas)
```

**所以两者的差别只有一条：`create_documents` 收「文本 + 元数据列表」，`split_documents` 收「`Document` 对象列表」。** 前者适合「手上只有字符串」的场景，后者适合「已经有一批 `Document`（比如刚 load 完）要切」。

**元数据的传递是「每个输入文档一份，深拷贝给它的每一块」。**

```
metadatas_ = metadatas or [{}] * len(texts)
...
metadata = copy.deepcopy(metadatas_[i])
```

`copy.deepcopy` 是必需的：若浅拷贝，同一份元数据字典会被同一文档的所有块共享，任何一块被下游改写就污染全部。而 `[{}] * len(texts)` 这个写法本身有个陷阱——它创建的是**同一个 `{}` 的引用**；但因为随后每块都 `deepcopy`，所以不会出问题。这是一个「靠下游的防复制来补上游的共享」的细节。

**`add_start_index` 的算偏移方式值得单独拆。**

```
index = 0; previous_chunk_len = 0
for chunk in self.split_text(text):
    ...
    offset = index + previous_chunk_len - self._chunk_overlap
    index = text.find(chunk, max(0, offset))
    metadata["start_index"] = index
    previous_chunk_len = len(chunk)
```

这里有三层修正：

1. **`max(0, offset)`** 保证不出现负数起点（第一块时 `previous_chunk_len` 与 `chunk_overlap` 会让 offset 变负）。
2. **用 `text.find(chunk, offset)` 而不是直接算位置**——也就是说**下标是「在原文里真实找到的位置」**，不是累加出来的。这比累加可靠，因为切分器可能重排或去掉空白（`strip_whitespace`），累加会漂移。
3. **`previous_chunk_len` 记的是上一块的**字符长度，而不是它的起始位置。

而 `text.find` 若失败会返回 `-1`，此时 `start_index` 就是 `-1`。这是一个未处理的边界——但实际极少发生（块是原文的子串，除非 `strip_whitespace` 把它改得不像原文）。

`offset` 的算式值得再想一遍：`index` 是上一块的起点，`previous_chunk_len` 是上一块的长度，减去 `chunk_overlap` 就是「重叠区的起点」。也就是说**搜索从重叠区开始，而不是从上块末尾开始**——因为这一块的起始内容就在重叠区里。

**`_join_docs` 的返回值是 `str | None`，用 `text or None` 把空串变成 `None`。**

```
text = separator.join(docs)
if self._strip_whitespace: text = text.strip()
return text or None
```

于是「全是空白」的块被丢弃而不是作为一个空 `Document` 进入结果。这个方向是对的：一个只含空白的块进向量库既浪费又可能干扰检索。而 `or None` 这个写法也把「空串」与「`None`」合并了——对调用方来说这两者都是「没有内容」。

### 四、`TokenTextSplitter` 与 `Tokenizer`：按 token 切的独立路径

**`TokenTextSplitter` 不继承上面的「按分隔符切 + 合并」逻辑，而是换了一条完全不同的路径。**

它用 `TokenTextSplitter` 的 `__init__` 里拿 tiktoken 编码器，把 `length_function` 设成该编码器的计数函数，然后 `split_text` 走的是：

```
def split_text(self, text):
    return split_text_on_tokens(text=text, tokenizer=self._tokenizer)
```

**`split_text_on_tokens` 是一个模块级函数，它的算法是纯滑动窗口。**

```
input_ids = tokenizer.encode(text)
if tokenizer.tokens_per_chunk <= tokenizer.chunk_overlap: raise ValueError("tokens_per_chunk must be greater than chunk_overlap")
start_idx = 0
while start_idx < len(input_ids):
    cur_idx = min(start_idx + tokens_per_chunk, len(input_ids))
    chunk_ids = input_ids[start_idx:cur_idx]
    if not chunk_ids: break
    decoded = tokenizer.decode(chunk_ids)
    if decoded: splits.append(decoded)
    if cur_idx == len(input_ids): break
    start_idx += tokens_per_chunk - chunk_overlap
```

**它先编码整个文本，再按 token 索引切片，最后把每片解码回字符串。** 这是一个和「按字符分隔切」完全异质的做法——它的重叠是**精确的 token 数**，而不是「近似按片段」。

代价也很清楚：**逐片 `decode` 会破坏原文在边界处的连续性**（一个 token 跨在边界上时，两侧解出来的文本可能拼接不回原文），且整个文本必须先完整编码进内存。

**它自己有一份与基类重叠的校验**：`tokens_per_chunk <= chunk_overlap → ValueError("tokens_per_chunk must be greater than chunk_overlap")`。而这个校验在基类层面也会被 `chunk_overlap > chunk_size` 捕获——**两处都在查**，说明这个不变量的重要性，也留下了「两处校验可能漂移」的隐患。

**`Tokenizer` 是一个 `@dataclass`，恰好三个字段加两个函数。**

```
chunk_overlap: int         # 重叠 token 数
tokens_per_chunk: int      # 每块 token 上限
decode: Callable[[list[int]], str]
encode: Callable[[str], list[int]]
```

这个「把 encode/decode 作为字段传入」的设计使 `split_text_on_tokens` 完全不依赖 tiktoken——任何有编码器的东西（HuggingFace、自定义词表）都能用。**这是整个分块器体系里唯一的「不依赖具体分词器」的接口。**

### 五、`Language` 与语言预设

**`Language` 是一个 `StrEnum`，二十**八**个值。**

```
CPP / GO / JAVA / KOTLIN / JS / TS / PHP / PROTO / PYTHON / R / RST /
RUBY / RUST / SCALA / SWIFT / MARKDOWN / LATEX / HTML / SOL / CSHARP /
COBOL / C / LUA / PERL / HASKELL / ELIXIR / POWERSHELL / VISUALBASIC6
```

它本身在 base.py 里，而**用它的是字符切分器**（下一节）。这个位置的选择说明：语言预设被认为是「切分器体系的公共概念」，而不是某个具体切分器的私有配置。

**`from_tiktoken_encoder` 是 `cls.__init__` 的一个包装，且它支持 `model_name` 覆盖 `encoding_name`。** 传入 `model_name` 时走 `tiktoken.encoding_for_model`，否则走 `get_encoding`。这个双路径是必要的：**同一个编码（比如 cl100k_base）被多个模型共用，而按模型名取更贴合实际**。

而 `allowed_special` / `disallowed_special` 两个参数控制特殊 token 的处理。默认 `allowed_special=None`（当作空集）、`disallowed_special="all"`——也就是**默认拒绝所有特殊 token**。这是一个安全向的默认：特殊 token 在编码时可能被当成控制字符，文本里出现它们是可疑的。

**`from_huggingface_tokenizer` 做了一次 `isinstance` 校验并抛 `ValueError` 而非 `TypeError`。** 代码里有 `# noqa: TRY004` 注释——也就是说 linter 建议这里该用 `TypeError`，而作者有意用 `ValueError` 并显式抑制了那条规则。理由是这是「参数值不合法」而非「类型不匹配」，与其它参数校验的错误类型保持一致。

**`transform_documents` 只是 `split_documents(list(documents))` 的转发。** 它存在是为了满足 `BaseDocumentTransformer` 的接口——于是分块器可以放进「文档变换流水线」里，与其它变换器一视同仁。

### 六、`Document` / `Blob` / `BaseMedia`

**refs 里带上了 `documents/base.py`，因为分块的产物是 `Document`，而它的元数据语义来自这里。**

`Document` 是「正文 + 元数据 + 可选 id」的三件套，而 `Blob` 是「原始字节 + 元数据」，`BaseMedia` 是它们的共同基类。这个分层说明 langchain 区分了两种数据形态：

- **`Blob`**：还没被解析的原始材料（一份 PDF 的字节）。
- **`Document`**：已解析、可切分、可嵌入的文本单元。

**分块器只处理后者。** 也就是说「把 PDF 变成文本」发生在分块之前，而分块之后得到的是更多、更小的 `Document`。这也解释了为什么 `split_documents` 要保留元数据——**元数据是從上游（loader）一路带到下游（检索过滤）的唯一线索**，而分块是这条链条上唯一会「一变多」的环节。

## 关键取舍

**源码事实：** `chunk_size` 的单位由 `length_function` 决定（默认 `len` 即字符数）；`chunk_overlap > chunk_size` 在构造时就被拒绝，因为那会让回退量超过前进量。

**源码事实：** `_merge_splits` 把分隔符长度也计入总量，且只在「已有内容」时才计（与 `join` 的实际行为对齐）。

**源码事实：** 重叠是「保留尾部片段作为下一块头部」而不是复制文本尾部，所以重叠量是近似值而非精确值。

**源码事实：** 回退循环有第二个条件（「新片段仍装不下就继续退」），避免超大片段导致反复产出几乎相同的块。

**源码事实：** `add_start_index` 用 `text.find(chunk, offset)` 得到真实位置而不是累加，因此对 `strip_whitespace` 这类改写免疫；`max(0, offset)` 防负起点。

**源码事实：** `_join_docs` 用 `text or None` 把空串/纯空白块丢弃，不让它们变成空 `Document`。

**源码事实：** 元数据对每块做 `deepcopy`，避免同一文档的多块共享一份字典。

**源码事实：** `TokenTextSplitter` 走独立的 `split_text_on_tokens` 滑动窗口路径（先编码整段再按 token 索引切片再解码），与「按分隔符切 + 合并」完全异质；重叠是精确 token 数。

**源码事实：** `Tokenizer` 把 `encode` / `decode` 作为字段传入，使滑动窗口算法不依赖具体分词器。

**源码事实：** `tokens_per_chunk <= chunk_overlap` 在两处被校验（基类与 `split_text_on_tokens`），理论上可能漂移。

**源码事实：** `from_huggingface_tokenizer` 用 `ValueError` 并显式抑制 `TRY004`（linter 建议 `TypeError`）。

**架构推论：** 这个基类最值得记的是**它把「长度」外置了**。`chunk_size` 与 `chunk_overlap` 的数值在两种配置下含义完全不同（字符 vs token），而基类对此不作任何假设——所有关于长度的判断都走 `_length_function`。

这个外置带来一个隐蔽的后果：**`_merge_splits` 里所有算术都是「长度函数的输出可加」这个假设**。若一个长度函数不满足可加性（例如「按句子计数」），分隔符长度加上片段长度的和就没有意义，重叠计算也会错。基类没有（也无法）校验这条性质——这是调用方的责任。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/text-splitters/langchain_text_splitters/base.py` | `TextSplitter` | 分块骨架：抽象 `split_text` + 基类合并与封装 |
| `libs/text-splitters/langchain_text_splitters/base.py` | `TextSplitter.__init__` | 六个参数与三条校验（含 `chunk_overlap > chunk_size` 拒绝） |
| `libs/text-splitters/langchain_text_splitters/base.py` | `_merge_splits` | 合并与重叠：分隔符计入、回退到重叠量、超大片段仍前进 |
| `libs/text-splitters/langchain_text_splitters/base.py` | `_join_docs` | 拼接 + 可选去空白，空串转 `None` |
| `libs/text-splitters/langchain_text_splitters/base.py` | `create_documents` | 文本 + 元数据 → `Document`，含 `start_index` 计算 |
| `libs/text-splitters/langchain_text_splitters/base.py` | `split_documents` | 与上者的唯一差别是收 `Document` 列表 |
| `libs/text-splitters/langchain_text_splitters/base.py` | `TokenTextSplitter` | 按 token 切，走独立路径 |
| `libs/text-splitters/langchain_text_splitters/base.py` | `Tokenizer` / `split_text_on_tokens` | 滑动窗口 + 可注入编解码 |
| `libs/text-splitters/langchain_text_splitters/base.py` | `Language` | 二十八种语言的枚举，供字符切分器取预设 |
| `libs/text-splitters/langchain_text_splitters/base.py` | `from_tiktoken_encoder` | 按模型名或编码名构造长度函数 |
| `libs/core/langchain_core/documents/base.py` | `Document` / `Blob` / `BaseMedia` | 分块产物的形态与元数据载体 |

## 参考

- [langchain 源码：text_splitters/base.py](https://github.com/langchain-ai/langchain/blob/master/libs/text-splitters/langchain_text_splitters/base.py)
- [langchain 文档：文本分割器](https://python.langchain.com/docs/concepts/text_splitters/)
- [tiktoken 仓库（编码器与 token 计数）](https://github.com/openai/tiktoken)
- [langchain 文档：Document 与元数据](https://python.langchain.com/docs/concepts/document_loaders/)

## 与主核 / AI 的关系

这套「尺寸上限 + 重叠窗口 + 按片段合并」与主核里的**缓冲区与页管理**是同一类问题的两个尺度：数据库要把一个大的数据流切成固定大小的页，同时不能让一条记录跨页丢失语义；分块器要把一份长文本切成固定 token 预算的块，同时不能让一个论证被切断。

两者都用「上限 + 重叠」来缓解边界效应，但差别在于**重叠的精确度**：数据库的页边界是精确的（页就是那么多字节），而这里的分块重叠是**近似**的——因为按片段合并无法保证精确等于 `chunk_overlap`。这个近似是有意接受的，代价是块的实际大小会在 `chunk_size` 附近浮动。

对 agent 工程的直接价值有三条。其一，**把「长度」做成可注入的函数，而不是假设字符数**。真实约束是模型的 token 预算，而字符数到 token 数的比例随语言剧烈变化（中文约 1:1.5、英文约 4:1）。其二，**上限与重叠必须有构造期校验**。`chunk_overlap > chunk_size` 会让切片不终止，这类「参数组合导致算法失效」的错误应当早失败。其三，**重叠的实现方式决定了它的精确度**。按片段保留尾部只能得到近似重叠；要精确重叠就必须像 `split_text_on_tokens` 那样按 token 索引切——两者不能兼得，而选用哪种取决于「块边界是否可以落在词中间」。
