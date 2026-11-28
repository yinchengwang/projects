---
project: haystack
feature: structured-splitters
title: 结构化与层级切分
page_type: feature
schema_version: 1
refs:
  - path: haystack/components/preprocessors/markdown_header_splitter.py
    symbols: [MarkdownHeaderSplitter]
  - path: haystack/components/preprocessors/recursive_splitter.py
    symbols: [RecursiveDocumentSplitter]
  - path: haystack/components/preprocessors/hierarchical_document_splitter.py
    symbols: [HierarchicalDocumentSplitter]
---

# 结构化与层级切分

## 一句话职责

三个切分器各自利用文本里「已有的结构」：`MarkdownHeaderSplitter` 按 ATX 标题切并把标题层级写进元数据，`RecursiveDocumentSplitter` 按分隔符列表逐级降级切分，`HierarchicalDocumentSplitter` 把同一份文档切成多个粒度并**显式建立父子关系**——后者是自动合并检索能工作的前提。

## 机制是什么

| 类 | 利用的结构 | 产出的元数据 | 是否建立多层 |
|---|---|---|---|
| `MarkdownHeaderSplitter` | ATX 标题（`#` 到 `######`） | `header` / `parent_headers` | 否（扁平，靠元数据表达层级） |
| `RecursiveDocumentSplitter` | 分隔符（含 `sentence` 这个伪分隔符） | `split_idx_start` / `_split_overlap` / `page_number` | 否 |
| `HierarchicalDocumentSplitter` | `block_sizes` 集合 | `__parent_id` / `__children_ids` / `__level` / `__block_size` | **是（真正的树）** |

前两者是「一次切分」，后者是「多次切分并串起来」。而 `HierarchicalDocumentSplitter` 本身不改算法——它**复用 `DocumentSplitter` 按多个粒度各切一遍**。

## 怎么实现的

### 一、`MarkdownHeaderSplitter`：三处需要「字面还原」的细节

**它的正则表达式的不是简单匹配 `^#+ `，而是照 CommonMark 写出来的。**

```
self._header_pattern = re.compile(r"(?m)^ {0,3}(#{1,6})[ \t](.+)$")
```

` {0,3}` 允许最多三个前导空格（**四个空格就是缩进代码块，不是标题**）；`[ \t]` 要求标题标记后必须有一个空格或制表符；`(?m)` 让 `^$` 按行匹配。

**而闭合序列要单独剥掉。**

```
self._closing_sequence_pattern = re.compile(r"(?:^|[ \t])#+[ \t]*\r?$")
header_text = self._closing_sequence_pattern.sub("", match.group(2)).strip()
```

`## Install ##` 的末尾那串 `##` 是 ATX 闭合标记，不属于标题文本。注释写明了它「不属于标题文本，只允许空格/制表符围绕，还允许 Windows 换行的一个 `\r`」。

**代码块的识别是这一页最讲究的一段，因为它的注释把整个正则拆开解释了。**

```
self._code_block_pattern = re.compile(
    r"^[ ]{0,3}(?:(?P<backticks>`{3,})(?![^\n]*`)|(?P<tildes>~{3,}))[^\n]*(?:\n|\Z)"
    r".*?"
    r"(?:^[ ]{0,3}(?(backticks)(?P=backticks)`*|(?P=tildes)~*)[ \t]*\r?$|\Z)",
    re.MULTILINE | re.DOTALL,
)
```

**要解决的具体问题：代码块里的 `#` 注释会被误认为标题**，从而把文档结构撕碎。

五处设计各有原因：

1. **`^[ ]{0,3}`** —— 围栏最多缩进三个空格；四个以上是缩进代码块，这个切分器不跟踪。
2. **`(?P<backticks>`{3,})(?![^\n]*`)`** —— 三个以上反引号，**且开行剩余部分不能再有反引号**。于是以行内代码开头的行（`` ```foo``` ``）不会开启代码块。这个否定断言是本段最容易被忽略的一处。
3. **`(?P<tildes>~{3,})`** —— 波浪号围栏也算。
4. **条件闭合 `(?(backticks)(?P=backticks)`*|(?P=tildes)~*)`** —— **闭合必须用同种字符**，` ``` ` 不会被 `~~~` 闭合。这是正则的条件分支（`(?(name)yes|no)`），靠命名组判断用的是哪种围栏。
5. **`|\Z`** —— 永不闭合的块一直延伸到文本末尾（往里吞是安全的，因为最坏情况是把后续内容当代码）。

**而「标题与代码块两者按源码顺序合并」的实现值得单独看。**

```
code_blocks = self._code_block_pattern.finditer(text)
code_block = next(code_blocks, None)
matches = []
for match in self._header_pattern.finditer(text):
    if len(match.group(1)) not in self._header_split_levels_set: continue
    while code_block is not None and code_block.end() <= match.start(): code_block = next(code_blocks, None)
    if code_block is None or match.start() < code_block.start(): matches.append(match)
```

**两个 `finditer` 是并行推进的游标**（注释写着「两者都按源码顺序到达，每个围栏只需访问一次」）。对每个候选标题：先把所有「已经结束」的代码块跳过，然后检查当前标题是否落在下一个代码块开始之前——是则采纳，否则它在代码块内。

这个写法的关键是**两个迭代器都只前进不后退**，于是整体是线性的（而不是「对每个标题扫描全部代码块」的 O(n·m)）。

**前言的三种处理是本页最能说明「字节级分区」意图的一段。**

```
preamble = text[: matches[0].start()]
if preamble.strip():
    chunks.append({"content": preamble, "meta": {"header": "", "parent_headers": []}, ...})
    has_content = True
elif preamble and self.keep_headers:
    pending_start = 0
```

三种情形：

1. **前言里有非空白内容** → 单独成块，`header` 留空、`parent_headers` 空列表。注释说明它「不属于任何节，但它自己就是文档内容」（README 的开头段落、标题块、front matter）。
2. **前言只有空白且 `keep_headers=True`** → **缓冲起来**（`pending_start = 0`），等第一个「保留标题行」的块把它一起带上。这样得到的块仍然是输入的**字节精确分区**，不会多出一个只含空白的块。
3. **前言只有空白且 `keep_headers=False`** → 什么都不做。注释给了理由：**缓冲机制只被 `keep_headers` 分支消费**，而 `keep_headers=False` 时块内容本来就从标题行之后开始，没有东西可带。

**「缓冲」机制本身是为了处理连续的、没有正文的标题。**

```
if not content.strip():
    if self.keep_headers:
        if pending_start is None: pending_start = match.start()
        pending_header_text = header_text
        pending_level = level
    continue
```

`## A` 后面紧跟 `## B` 再跟正文时，`## A` 没有自己的正文——若直接丢掉它，`## A` 这一行就从输出里消失了。于是把它缓冲起来，等下一个有正文的标题出现时，**从 `pending_start` 开始切片**，把中间所有行原样带上。

**注意它只保留「第一个缓冲标题的偏移」和「最后一个缓冲标题的文本」**——中间的标题既不需要偏移（整段切片就够）也不需要文本（元数据只记最后一个）。这是一个够用的最小信息集。

**而末尾的 flush 是一个必需的补丁。**

```
if pending_start is not None:
    parent_headers = [h for h in header_stack[: pending_level - 1] if h is not None]
    chunks.append({"content": text[pending_start:], "meta": {"header": pending_header_text, ...}})
```

**文档末尾的一串空标题永远等不到「下一个有正文的标题」**，于是不 flush 就会被静默丢弃。注释明确写了这一点，并说明「切片原文保证重建精确」。

**`parent_headers` 的构造时机有一个容易写错的地方。**

```
parent_headers = [h for h in header_stack[: level - 1] if h is not None]
```

这一行取的是**当前标题之前、层级更浅的那些**。而它必须在**更新 `header_stack` 之后、但取 `[:level-1]` 所以不受当前层影响**——实际上顺序是：

```
header_stack[level - 1] = header_text
for j in range(level, 6): header_stack[j] = None
...
parent_headers = [h for h in header_stack[: level - 1] if h is not None]
```

先把自己的层级写进栈、再把更深的层级清空（因为那些更深的标题已经不在当前作用域了），然后取 `[:level-1]`。这个顺序保证「第一个有正文的子节也能拿到完整祖先链」。

**次级切分（`secondary_split`）把「一次切分」扩成两步，而它最麻烦的部分是偏移与页号的换算。**

```
secondary_splits = self.secondary_splitter.run(documents=[Document(content=content_for_splitting, meta=clean_meta)])["documents"]
...
section_relative_start_idx = secondary_content_start_idx + split.meta["split_idx_start"]
split.meta["split_idx_start"] = source_start_idx + section_relative_start_idx
```

三层偏移的嵌套：`source_start_idx`（块在原文的起点）+ `secondary_content_start_idx`（若剥掉了标题行，正文在块内的起点）+ `split.meta["split_idx_start"]`（次级块在正文内的起点）。三者相加才是原文偏移。

**而页号的换算有三处修正，每处都有注释。**

```
chunk_start_page = doc.meta.get("page_number", 1) - _leading_page_breaks(doc.content, self.page_break_character)
```

注释解释了这一减：`doc.meta["page_number"]` 记的是「块的**文本**起始页，已经算进了块开头的分页符」，而下面的 `page_break_ends` 会**再数一遍**那些分页符——所以要先把它们减掉。

```
page_break_ends = [match.end() for match in re.finditer(re.escape(self.page_break_character), doc.content)]
while page_break_count < len(page_break_ends) and page_break_ends[page_break_count] <= section_relative_start_idx:
    page_break_count += 1
split.meta["page_number"] = chunk_start_page + page_break_count + _leading_page_breaks(split.content or "", self.page_break_character)
```

也就是说最终页号 = 块的起始页 + 「块内、这个次级块之前的分页符数」+ 「次级块自己开头的分页符数」——**与 e01 那页的 `_leading_page_breaks` 语义完全一致的复用**，只是多加了一层块内偏移。

**`from_header_split` 这个标志有一个必须存在的理由。**

```
if not self.keep_headers and from_header_split:
    header_match = re.match(self._header_pattern, doc.content)
    if header_match:
        secondary_content_start_idx = header_match.end()
        content_for_splitting = doc.content[secondary_content_start_idx:]
```

注释把「为什么不能靠元数据推断」写得很清楚：**放在第一个标题之前的那一块也带标题元数据，而它的首行可能是一个不在切分层级里的标题**（比如 `header_split_levels=[1]` 而首行是 `## foo`）。若靠「有 header 元数据」来推断，就会把那一行剥掉并永久丢失。

所以这个布尔值必须从块构造处一路传下来——**它是「这块是否真的由标题切分产生」的不可重建信息**。

### 二、`RecursiveDocumentSplitter`：伪分隔符与两次重叠

**它的分隔符列表里有一个特殊值：`"sentence"`。**

```
self.separators = separators if separators else ["\n\n", "sentence", "\n", " "]
```

其余分隔符都是**正则**（注释写明「字符串分隔符会被当作正则处理，除非它是 `"sentence"`」）。于是 `_chunk_text` 里对它是特殊处理：

```
if curr_separator == "sentence":
    sentence_with_spans = self.nltk_tokenizer.split_sentences(text)
    split_bounds = [sentence["start"] for sentence in sentence_with_spans]
    if split_bounds:
        split_bounds.append(sentence_with_spans[-1]["end"])
        trailing_text = text[split_bounds[-1]:]
    splits = [text[start:end] for start, end in zip(split_bounds, split_bounds[1:], strict=False)]
```

**关键是「用下一句的 start 作为本句的 end」而不是用本句的 end。** 注释说明了理由：这样切出来的片段**保留了句间的空白**，即使分词器的 `keep_white_spaces=False`。若丢掉那些空白，句子会被粘在一起、分页符会丢失、并且**之后每个块的偏移都会错位**。

这是一个「为了让偏移可累加而多保留空白」的例子——而 `trailing_text` 单独处理最后一句之后的部分（否则那部分会落在所有片段之外）。

**普通分隔符的处理是「加捕获组再按两步合并」。**

```
escaped_separator = f"({re.escape(curr_separator)})"
splits = re.split(escaped_separator, text)
splits = ["".join([splits[i], splits[i + 1]]) if i < len(splits) - 1 else splits[i] for i in range(0, len(splits), 2)]
splits = splits[:-1] if splits[-1] == "" else splits
```

带捕获组的 `re.split` 把分隔符也返回，于是结果是「片段、分隔符、片段…」交替。配对取 `[i] + [i+1]`（从偶数下标起）得到「片段 + 后面的分隔符」——**分隔符贴在片段末尾**，与 e01 那页字符切分的做法一致，为了让拼接能还原原文。

**而递归的核心判据是「这层有没有切出东西」。**

```
if len(splits) == 1: continue
```

若某个分隔符在文本里不存在，`re.split` 会返回只有一个元素的列表——于是跳过它试下一个。这是「逐级降级」的实现方式：**降级依据不是「上层的块还太长」，而是「这层根本切不动」**。

**而「切出来的块还太长」则由两个不同分支处理。**

```
if current_length + self._chunk_length(split_text) > self.split_length:
    if current_chunk: chunks.append("".join(current_chunk)); current_chunk = []; current_length = 0
    if self._chunk_length(split_text) > self.split_length:
        if curr_separator == self.separators[-1]:
            chunks.extend(self._fall_back_to_fixed_chunking(split_text, self.split_units))
        else:
            chunks.extend(self._chunk_text(split_text))
    else:
        current_chunk.append(split_text); current_length += ...
```

三档：**装不下就先收块**、**单个片段本身超限则递归（或到最后一级时退到定长切）**、**否则继续攒**。

注意 `curr_separator == self.separators[-1]` 这个终止判定——**只看是不是最后一个分隔符**，而不是「还有没有更细的」。于是递归深度由分隔符列表长度决定。

**重叠被分成两套实现，而它们的位置很关键。**

`_run_one` 里注释写着：

> 重叠只在整个切分完成之后施加一次，这样在递归内层产生的块不会被施加第二次重叠。

```
if self.split_overlap > 0 and self.split_units == "word":
    text = "".join(chunks)
    spans = self._apply_word_overlap(chunks)
    chunks = [text[start:end] for start, end in spans]
    chunk_starts = [start for start, _ in spans]
else:
    if self.split_overlap > 0: chunks = self._apply_overlap(chunks)
    chunk_starts = [0]
    for chunk in chunks[:-1]:
        overlap = self._get_overlap([chunk])[0] if self.split_overlap > 0 else ""
        chunk_starts.append(chunk_starts[-1] + len(chunk) - len(overlap))
```

**这个位置选择是必需的**：若在递归内层加重叠，同一次重叠会被施加多次（因为内层的结果会再经过外层的 `_apply_overlap`），重叠量会指数增长。

**而 word 与 char/token 分两套的原因值得记：**

- **char/token 版的 `_apply_overlap` 是「重建」**：它把重叠文本拼到块前面（`_create_chunk_starting_with_overlap` 直接字符串相加或 token 拼接），于是块内容被改写，偏移必须靠「上一块长度减重叠长度」反推（那段 `chunk_starts` 循环）。
- **word 版的 `_apply_word_overlap` 返回 `(start, end)` 字符区间**，块**从原文切片**得到——注释写明「块是从文本里切出来的，而不是用词重建的，所以它们保留了原文的空白与分页符」。

**这是一个重要的差别**：重建会丢空白（因为 `" ".join(words)` 会把原文的多个空格、制表符、换行统一掉），切片则保留一切。而 word 版能做到切片，是因为它**先算出所有词的字符区间**（`re.finditer(r"[^ ]+", chunk)`），再在「词」这个粒度上决定块的边界。

**`_apply_word_overlap` 的循环结构值得逐句看。**

```
spans = [(0, chunk_ends[0])]
first_word, end_word = 0, chunk_word_ends[0]
for idx in range(1, len(chunk_word_ends)):
    while True:
        first_word = max(first_word, end_word - self.split_overlap)
        end_word = min(first_word + self.split_length, chunk_word_ends[idx])
        end = chunk_ends[idx] if end_word == chunk_word_ends[idx] else words[end_word - 1][1]
        spans.append((words[first_word][0], end))
        if idx < len(chunk_word_ends) - 1 or end_word == chunk_word_ends[idx]: break
```

外层遍历「块的词结束位置」，内层 `while` 处理「加了重叠之后又超限」的情形——**一个块可能需要吐出多个 span**。而内层的退出条件是「不是最后一块」或「已经追到本块词尾」。

**`end` 的算法也有讲究**：若 `end_word` 已到本块词尾，就用 `chunk_ends[idx]`（本块在拼接文本里的终点，**包含尾部空白**）；否则用「第 `end_word-1` 个词的结束位置」——**只在块被截断时才丢尾部空白**。

**页号在 `_run_one` 里靠「累加分页符数」而不是每次全量数。**

```
page_breaks_before = 0
for split_nr, (chunk, current_position) in enumerate(zip(chunks, chunk_starts, strict=True)):
    if split_nr > 0:
        page_breaks_before += content.count("\f", chunk_starts[split_nr - 1], current_position)
    new_doc.meta["page_number"] = 1 + page_breaks_before + _leading_page_breaks(chunk)
```

注释说明了这个优化的前提：**块起点永不后退**（`chunk_starts` 单调不减），所以只需数「上一块起点到这一块起点」之间的分页符。这是 O(n) 而不是 O(n²)。

### 三、`HierarchicalDocumentSplitter`：用多次切分建树

**它的参数只有一个集合：`block_sizes`。**

```
self.block_sizes = sorted(set(block_sizes), reverse=True)
self.splitters = {block_size: DocumentSplitter(split_length=block_size, split_overlap=..., split_by=..., tokenizer_encoding=...) for block_size in self.block_sizes}
```

**每个尺寸配一个 `DocumentSplitter`，且从大到小排序。** 于是「先按大粒度切，再对每个块按更小粒度切」这件事就是遍历这个有序列表。

**三条构造校验里第三条是这一页唯一有实际内容的。**

```
smallest_block_size = min(block_sizes)
if split_overlap >= smallest_block_size:
    raise ValueError(f"split_overlap ({split_overlap}) must be less than the smallest value in block_sizes ({smallest_block_size}). ...")
```

**用最小尺寸而不是最大**——因为在最小那一层，重叠必须小于块长才能前进。若只看最大尺寸，一个 `block_sizes={10, 3}` 配 `split_overlap=5` 会通过校验，然后在尺寸 3 那层挂死。

**建树的循环有一个「切不动就跳过」的分支。**

```
splitted_docs = self.splitters[block].run([doc])
child_docs = splitted_docs["documents"]
if len(child_docs) == 1:
    next_level_nodes.append(doc)
    continue
```

**若切完只得一个块，说明这个粒度对这份内容是无效的**——那就把原 `doc` 当作「这一层的节点」继续往下走，不产生新的子节点。这避免了在树里插入一个与父节点内容完全相同的中间层。

**而偏移与页号的换算有很具体的三行。**

```
is_root = doc.meta["__level"] == 0
page_offset = 0 if is_root else doc.meta["page_number"] - 1 - _leading_page_breaks(doc.content or "")
idx_offset = 0 if is_root else doc.meta["split_idx_start"]
for child_doc in child_docs:
    child_doc.meta["page_number"] += page_offset
    child_doc.meta["split_idx_start"] += idx_offset
```

**注释写明了原因：`DocumentSplitter` 是从 `doc` 的起点开始数页与偏移的，不是从原始文档。**

于是两层修正：

- **偏移修正**：父块在原文的位置（`split_idx_start`）加上子块在父块内的位置。
- **页号修正**：父块的起始页（`page_number` 减去它开头的分页符，得到「父块的第一个字符所在页」）加上子块在父块内的页偏移。

**而 `is_root` 的特判是必需的**——根节点没有 `split_idx_start`（它不被切出来），也没有 `page_number`（它是原始文档）。若不加这个特判，`doc.meta["split_idx_start"]` 会抛 `KeyError`。

**`_add_meta_data` 用 `replace` 而不是原地改。**

```
new_meta = {**document.meta, "__block_size": 0, "__parent_id": None, "__children_ids": [], "__level": 0}
return replace(document, meta=new_meta)
```

`replace` 是 dataclass 的替换（`Document` 是 dataclass 风格的），它**返回新对象且 `id` 会被重算**——于是每个新节点得到新 id，而根节点也得到一个新的（不是调用方那个 document 的 id）。

**根节点还额外做了一次元数据的深拷贝。**

```
# the root is the only node built from the caller's Document, so it is the only one that needs detaching
root = self._add_meta_data(replace(document, meta=deepcopy(document.meta)))
```

注释说明了理由：只有根节点是从调用方的对象派生的，所以要**断开引用**以免修改调用方的 `meta`。而子节点的 `meta` 是 `_add_meta_data` 里新建的字典（`{**document.meta, ...}`），已经是浅拷贝后的新字典——但注意**浅拷贝意味着嵌套的值仍共享**。

**四个元数据键的前缀 `__` 是一个刻意的命名。** `__parent_id` / `__children_ids` / `__level` / `__block_size`——双下划线让它们一眼可辨「这是框架写的内部结构」，与用户元数据区分开。这与 `g01`（摄取流水线）里 `__last_accessed__` 的思路一致。

**而 `__level=0` 与 `__block_size=0` 对根节点的设定，正是 `AutoMergingRetriever` 校验的依据。**

从 `auto_merging_retriever.py` 的注释可以读出这个契约：

```
# HierarchicalDocumentSplitter uses __level=0 and __block_size=0 for the root, so these ...
```

以及它的入口校验：

```
parentless_doc = next((doc for doc in matched_leaf_documents if not doc.meta.get("__parent_id")), None)
if parentless_doc is not None:
    raise ValueError(f"... do not have the required meta field '__parent_id': document '{parentless_doc.id}' has none. AutoMergingRetriever only accepts leaf documents created by HierarchicalDocumentSplitter. Write only the leaf documents to the document store ...")
```

也就是说**两个组件之间的契约完全靠这四个键**：只要元数据在，任何来源的文档都能被自动合并；元数据缺一个，就有一个明确的错误消息告诉你缺哪个。

**这正是 AGENTS.md 里「复用切分器写下的元数据键、为已有下游契约新增键」这条规定的由来**——切分器写下的键是下游组件的输入契约，不能随意改。

## 关键取舍

**源码事实：** `MarkdownHeaderSplitter` 的标题正则照 CommonMark 写：最多三个前导空格（四个就是缩进代码块）、ATX 标记后必须有空格或制表符；ATX 闭合序列（`## Install ##` 的尾部 `##`）单独剥掉。

**源码事实：** 代码块正则用**条件分支**（`(?(backticks)...|(?P=tildes)...)`）保证「闭合必须用同种围栏」，并用 `(?![^\n]*`)` 排除「以行内代码开头的行」，还允许永不闭合的块吞到文本末尾。

**源码事实：** 标题与代码块用**两个并行推进的 `finditer` 游标**合并，于是判断「标题是否在代码块内」是线性的，而不是每个标题扫一遍代码块。

**源码事实：** 前言分三种处理：有非空白内容则单独成块（`header` 为空）、只有空白且 `keep_headers=True` 则缓冲到第一个块、只有空白且 `keep_headers=False` 则丢弃（因为该分支没有缓冲消费方）。

**源码事实：** 连续的、没有正文的标题被「缓冲」，按第一个缓冲标题的偏移整段切片，等到下一个有正文的标题出现时一并带上；文档末尾的残余缓冲必须单独 flush，否则会被静默丢弃。

**源码事实：** `parent_headers` 在「把自己的层级写进栈、清空更深层级」之后取 `[:level-1]`，保证第一个有正文的子节也有完整祖先链。

**源码事实：** 次级切分的偏移是三层相加（块起点 + 剥掉标题行后的正文起点 + 次级块在正文内的起点）；页号有三处修正，其中「先减掉 `_leading_page_breaks`」是为了不让同一批分页符被数两遍。

**源码事实：** `from_header_split` 必须从块构造处传下来而不能靠元数据推断——因为「第一个标题之前的那一块」也带标题元数据，而它的首行可能是不在切分层级里的标题，靠推断会误剥。

**源码事实：** `RecursiveDocumentSplitter` 的分隔符除了 `"sentence"` 之外都被当作正则；`"sentence"` 模式的片段用「下一句 start 作本句 end」以保留句间空白，否则偏移会错位。

**源码事实：** 递归的降级判据是「这层切不动」（`len(splits) == 1` 则试下一个），而终止条件是「当前分隔符已是最后一个」（退到定长切）。

**源码事实：** 重叠**只在整个切分完成后施加一次**，因为递归内层加的重叠会被外层再施加一遍。

**源码事实：** word 版重叠返回字符区间、块从原文**切片**（保留空白与分页符）；char/token 版是**重建**（拼字符串），偏移只能靠「上一块长度减重叠长度」反推。

**源码事实：** 页号靠「累加上一块起点到本块起点之间的分页符数」，因为块起点单调不减。

**源码事实：** `HierarchicalDocumentSplitter` 的重叠校验用**最小**块尺寸而非最大——否则在小尺寸那层重叠会大于块长。

**源码事实：** 某一层切完只得一个块时，把原节点当作该层节点继续往下，不插入与父内容相同的中间层。

**源码事实：** 子节点的页号与偏移需要两层修正（父块在原文的位置 + 子块在父块内的位置），根节点靠 `is_root` 特判跳过（它没有这些键）。

**源码事实：** 根节点是唯一从调用方 `Document` 派生的节点，因此额外做了一次 `deepcopy(document.meta)` 断开引用。

**架构推论：** 这三个切分器的分工揭示了一个层次：**结构的「表达方式」决定了下游能用它做什么**。

`MarkdownHeaderSplitter` 把层级写进**元数据的字符串列表**（`parent_headers`）——扁平结构，下游只能按它过滤（「只在前两节里搜」）。`HierarchicalDocumentSplitter` 把层级写进**id 引用**（`__parent_id` / `__children_ids`）——真正的树，下游可以沿边遍历（走到父节点取更大上下文）。

后者能支撑「自动合并」这类需要向上遍历的能力，前者不能。**而两者的成本差别在小规模上不明显，在「下游需要什么」这个问题上却是决定性的。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/components/preprocessors/markdown_header_splitter.py` | `MarkdownHeaderSplitter` | 按 ATX 标题切，扁平层级写进元数据 |
| `haystack/components/preprocessors/markdown_header_splitter.py` | `_header_pattern` / `_closing_sequence_pattern` | CommonMark 风格的标题正则 + 闭合序列剥离 |
| `haystack/components/preprocessors/markdown_header_splitter.py` | `_code_block_pattern` | 条件分支保证同种围栏闭合 |
| `haystack/components/preprocessors/markdown_header_splitter.py` | `_split_text_by_markdown_headers` | 双游标合并、前言三分支、空标题缓冲与 flush |
| `haystack/components/preprocessors/markdown_header_splitter.py` | `_apply_secondary_splitting` | 三层偏移相加 + 页号三处修正 |
| `haystack/components/preprocessors/markdown_header_splitter.py` | `from_header_split` | 「这块是否真由标题切分产生」的不可重建信息 |
| `haystack/components/preprocessors/recursive_splitter.py` | `RecursiveDocumentSplitter` | 分隔符列表逐级降级切分 |
| `haystack/components/preprocessors/recursive_splitter.py` | `_chunk_text` | `"sentence"` 伪分隔符 + 递归降级 + 终止退到定长 |
| `haystack/components/preprocessors/recursive_splitter.py` | `_apply_overlap` / `_apply_word_overlap` | 重建式与切片式两套重叠 |
| `haystack/components/preprocessors/recursive_splitter.py` | `_run_one` | 重叠只施加一次；页号靠累加分页符 |
| `haystack/components/preprocessors/recursive_splitter.py` | `_fall_back_to_fixed_chunking` | 所有分隔符都无效时的定长兜底 |
| `haystack/components/preprocessors/hierarchical_document_splitter.py` | `HierarchicalDocumentSplitter` | 多个 `block_sizes` 各切一遍建树 |
| `haystack/components/preprocessors/hierarchical_document_splitter.py` | `build_hierarchy_from_doc` | 逐层下降、`is_root` 特判、两层偏移修正 |
| `haystack/components/preprocessors/hierarchical_document_splitter.py` | `__parent_id` / `__children_ids` / `__level` / `__block_size` | 与 `AutoMergingRetriever` 的元数据契约 |

## 参考

- [haystack 源码：markdown_header_splitter.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/preprocessors/markdown_header_splitter.py)
- [haystack 源码：recursive_splitter.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/preprocessors/recursive_splitter.py)
- [haystack 源码：hierarchical_document_splitter.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/preprocessors/hierarchical_document_splitter.py)
- [CommonMark 规范：fenced code blocks](https://spec.commonmark.org/0.31.2/#fenced-code-blocks)
- [haystack 文档：文档切分](https://docs.haystack.deepset.ai/docs/documentsplitter)

## 与主核 / AI 的关系

这三个切分器与主核里的**索引组织方式**有直接对照：主核的 B 树把「层级」表达为节点间的指针（真正的树），而倒排索引把「层级」表达为字段值（扁平）。两者都服务于「按层级过滤」，但只有前者能「向上走」。

`MarkdownHeaderSplitter` 像倒排索引（`parent_headers` 是一个可过滤的字段），`HierarchicalDocumentSplitter` 像 B 树（父子是 id 引用）。而 `AutoMergingRetriever` 那种「命中足够多的叶子就把父节点带回来」，本质上就是**B 树的区间提升**。

**「块必须是原文的字节精确分区」这个不变量在三个切分器里各有实现方式**，值得对照：`MarkdownHeaderSplitter` 靠「缓冲 + 切片原文」，`RecursiveDocumentSplitter` 的 word 版靠「算词区间再切片」，char/token 版则**放弃**了这个不变量（重建字符串会丢空白）。

对 agent 工程的直接价值有三条。其一，**层级要写成引用而不是文本**。`parent_headers` 是给人看的名字、`__parent_id` 是给程序走的边；后者才能支撑自动合并这类遍历能力。其二，**「这块是怎么来的」有时是不可重建的信息**。`from_header_split` 那个布尔值就是例子——从元数据推断会误判，只能显式传下来；类似的还有 `RecursiveDocumentSplitter` 里「重叠只施加一次」的位置选择。其三，**偏移与页号的换算每加一层就要多修一次**。次级切分要三层相加、层级切分要两层修正、页号要防重复计数——这些都是「切分器叠切分器」的必然代价。
