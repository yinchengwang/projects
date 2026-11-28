---
project: llama-index
feature: token-splitter
title: 按 token 切分与代码切分
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/node_parser/text/token.py
    symbols: [TokenTextSplitter]
  - path: llama-index-core/llama_index/core/node_parser/text/code.py
    symbols: [CodeSplitter]
---

# 按 token 切分与代码切分

## 一句话职责

`TokenTextSplitter` 是「按预算切分」的最朴素形态：不认句子、不认段落，只按分隔符（空格、换行）与字符逐级降级，然后按 token 数合并成块；`CodeSplitter` 则把切分单位换成**语法节点**——先用 tree-sitter 解析出 AST，再按 AST 的子树边界合并成不超过预算的块，于是产物永远是语法完整的片段。

## 机制是什么

这两个切分器共享同一套骨架（`TextSplitter` 的 `_parse_nodes` → `split_text` → `build_nodes_from_splits`），但它们的「降级阶梯」完全不同：

| | `TokenTextSplitter` | `CodeSplitter` |
|---|---|---|
| 切口 | 分隔符 → 备份分隔符 → 字符 | AST 节点边界 |
| 预算单位 | token（可换 tokenizer） | 字符 **或** token（`count_mode`） |
| 切口是否语义化 | 否（不认句子） | 是（不切断语法结构） |
| 重叠 | token 数 | 无（靠 AST 子树回退） |
| 依赖 | 无 | `tree_sitter_language_pack` |

**两者的差别恰好是「什么算一个不可分割的单位」这个问题上的两种答案。** 对自然语言，最小单位是词/字符，句子只是「尽量保住的更好单位」；对代码，最小单位是语法节点，一个 `if` 块被切成两半就不再是合法代码。所以前者用「分隔符降级」，后者用「AST 递归」。

这也解释了为什么 `TokenTextSplitter` 在 llama-index 里的定位是**基线**：它是 `SentenceSplitter` 的退化版（少了句子那一级），代价是切出来的块经常在半句话处断开。它的价值在于（a）不依赖任何东西，（b）行为完全可预测——给定 `chunk_size`，产物长度就是确定的，不像句子切分那样受标点影响。

关于重叠：`TokenTextSplitter` 有 `chunk_overlap`，但 `CodeSplitter` 虽然保留了 `chunk_lines_overlap` 字段，**实际切分路径并不使用它**——这是个只在字段上存在的参数。这一点后面会展开，它是这一页最值得记的一处「文档与实现脱节」。

## 怎么实现的

**`TokenTextSplitter` 的预算单位是可换的 tokenizer，且构造期就固定了降级阶梯。**

```
_tokenizer: Callable = PrivateAttr()
_split_fns: List[Callable] = PrivateAttr()

self._tokenizer = tokenizer or get_tokenizer()
all_seps = [separator] + (backup_separators or [])
self._split_fns = [split_by_sep(sep) for sep in all_seps] + [split_by_char()]
```

**`_split_fns` 是一个「已经绑好参数的函数列表」**：主分隔符（默认空格）一个、每个备份分隔符（默认换行）各一个、最后兜底一个按字符切。这个列表在构造期一次性算出来，切分时只是遍历——于是「降级阶梯」是一个数据结构而不是一串 `if`。

`from_defaults` 与 `__init__` 的参数表刻意不同：`from_defaults` **不接受 `tokenizer`**（于是永远用 `get_tokenizer()` 的全局分词器），而 `__init__` 接受。这个差异说明「自定义 tokenizer」是一条进阶路径而不是默认路径。

构造期唯一的校验是重叠不能大于块：

```
if chunk_overlap > chunk_size:
    raise ValueError(f"Got a larger chunk overlap ({chunk_overlap}) than chunk size ({chunk_size}), should be smaller.")
```

注意它**允许 `chunk_overlap == chunk_size`**（只判 `>`）。等值在 `_merge` 里会退化成「每块都与前一块完全一样」的边界行为，但不会死循环——因为合并循环另外有「`cur_len + split_len > chunk_size`」这个推进条件。

**`_split` 是递归的降级切分，而它的递归停止条件与 a02 是同一个坑。**

```
if len(self._tokenizer(text)) <= chunk_size:
    return [text]

for split_fn in self._split_fns:
    splits = split_fn(text)
    if len(splits) > 1:
        break
```

「只要某个函数切出多于一段就停下」——**第一个生效者胜**，后面的更粗暴的函数不再尝试。这与 `SentenceSplitter` 的两组函数是同一手法，只是这里只有一组。

然后是那个注释写得很清楚的保护：

```
elif len(splits) == 1:
    # The text could not be broken down any further (e.g. a single
    # character whose token count already exceeds chunk_size, which
    # happens for multi-token CJK / emoji characters with a small
    # chunk_size). Recursing on the same text would loop forever and
    # raise RecursionError, so keep it as an oversized split here --
    # _merge() already tolerates splits larger than chunk_size.
    new_splits.append(split)
```

**判据是「切分函数只产出一段」而非「切分函数是 `split_by_char`」**。也就是说：即使已经降到了按字符切，一个单个 CJK 字符仍可能占多个 token（很多分词器把整段中文切成字节对），此时 `split_by_char` 对单字符返回一段，递归就会原地打转。用「产出段数」作为「还能不能切」的判据，比用「当前是哪一级」更普适——它自动覆盖了「字符级也切不动」这条死角。

而 `_merge` 对超长片段的容忍方式是**只警告不报错**：

```
if split_len > chunk_size:
    _logger.warning(f"Got a split of size {split_len}, ", f"larger than chunk size {chunk_size}.")
```

注意这里 `_logger.warning` 被传了**两个位置参数**，而消息里没有任何 `%s` 占位符。`logging` 的 `getMessage()` 会在「没有占位符但 `args` 非空」时执行 `msg % args`，于是抛 `TypeError: not all arguments converted during string formatting`。这个异常发生在 `emit()` 内部，被 `logging` 的 `handleError` 捕获——结果是：**这条警告一条都打不出来**（handler 收到的是空输出），只在 stderr 打一段 `--- Logging error ---` 的栈。也就是说「超长片段」这个必须被知道的事件，实际表现是「什么都看不见」。这是 a03 里唯一的实锤缺陷，且症状比「信息不全」更糟：它连前半句都丢了。

对比 a02（`SentenceSplitter`）的选择：那里对同一个情形抛 `ValueError("Single token exceeded chunk size")`。**两个切分器对同一个物理不可能满足的预算约束采取了不同策略**——一个报错、一个降级放行。差异的理由大概是定位不同：`SentenceSplitter` 假定用户配的预算合理，冲突说明配置错了；`TokenTextSplitter` 是基线工具，宁可产出超长块也不要中断整批处理。

**`_merge` 的循环形态比 a02 简单，但有一处关键的顺序差异。**

```
cur_chunk: List[str] = []
cur_len = 0
for split in splits:
    split_len = len(self._tokenizer(split))
    if cur_len + split_len > chunk_size:
        chunk = "".join(cur_chunk) if self.keep_whitespaces else "".join(cur_chunk).strip()
        if chunk:
            chunks.append(chunk)
        while cur_chunk and (cur_len > self.chunk_overlap or cur_len + split_len > chunk_size):
            first_chunk = cur_chunk.pop(0)
            cur_len -= len(self._tokenizer(first_chunk))
    cur_chunk.append(split)
    cur_len += split_len
```

三处：

1. **重叠是「弹出头部片段直到同时满足两个条件」**：`cur_len <= chunk_overlap` **且** `cur_len + split_len <= chunk_size`。第二个条件保证「留着做重叠的片段加上新片段不会直接超预算」——否则新块一开始就超限。这两个条件是 `and` 关系的「都要满足」，而 `while` 的语义是「任一不满足就继续弹」。

2. **重叠的实质是「复用尾部片段」，与 a02 的结论一致**——`pop(0)` 逐个移除头部，留下的是尾部片段。于是新块以旧块的末尾片段开头。

3. **`keep_whitespaces` 决定拼接后是否 `strip()`**。默认 `False`（strip），于是「`" ".join` 拼起来的文本会丢掉块首尾的空白」。这个字段是为中文这类「词之间本来没有空格」的语言准备的——若按空格切开再拼，不 strip 会累积多余空格。

`_merge` 的 `_logger.warning` 那处缺陷已在上一段提过，此处不再重复。

**`split_text_metadata_aware` 是「先扣预算再切」的窄实现。**

```
metadata_len = len(self._tokenizer(metadata_str)) + DEFAULT_METADATA_FORMAT_LEN
effective_chunk_size = self.chunk_size - metadata_len
if effective_chunk_size <= 0:
    raise ValueError(f"Metadata length ({metadata_len}) is longer than chunk size ({self.chunk_size}). ...")
elif effective_chunk_size < 50:
    print(f"Metadata length ({metadata_len}) is close to chunk size ({self.chunk_size}). Resulting chunks are less than 50 tokens. ...", flush=True)
return self._split_text(text, chunk_size=effective_chunk_size)
```

`DEFAULT_METADATA_FORMAT_LEN = 2` 是「元数据格式本身占的 token 预留」（模板里的分隔符之类）。两档处理：**预算 ≤ 0 报错**（元数据比块还长，无法产出任何正文）、**预算 < 50 时打一条 print 警告**。用 `print` 而不是 `logger` 是 a02 里也指出的同一件事——它直接打到 stdout，`flush=True` 保证在管道里也能看到。

**`_split_text` 的双阶段是「切 → 合并」，中间夹一个 `CHUNKING` 回调事件：**

```
if text == "":
    return [text]
with self.callback_manager.event(CBEventType.CHUNKING, payload={EventPayload.CHUNKS: [text]}) as event:
    splits = self._split(text, chunk_size)
    chunks = self._merge(splits, chunk_size)
    event.on_end(payload={EventPayload.CHUNKS: chunks})
return chunks
```

**空串返回 `[""]` 而不是 `[]`** ——于是「空文档仍会产出一个空节点」。这个选择与 a01 里 `build_nodes_from_splits` 的循环配合的结果是：一个空文档会得到一个 `text=""` 的节点，而不是零个节点。两种行为都说得通，但「产出空节点」会让下游必须处理空文本。

**`CodeSplitter` 的第一句文档就坦白它不是自己发明的。**

```
Split code using a AST parser.
Thank you to Kevin Lu / SweepAI for suggesting this elegant code splitting solution.
https://docs.sweep.dev/blogs/chunking-2m-files
```

它引用的那篇文章讲的就是「按 AST 边界切代码」。核心思路一句话：**遍历 AST 的子节点，把子节点的源代码文本累积进当前块，超预算就关闭当前块；子节点自己超预算就递归进去。**

**构造期有三道与依赖有关的检查，每一道对应一种具体的失败。**

```
if parser is None:
    try:
        import tree_sitter_language_pack
        parser = tree_sitter_language_pack.get_parser(language)
    except ImportError:
        raise ImportError("Please install tree_sitter_language_pack to use CodeSplitter. Or pass in a parser object.")
    except Exception:
        print(f"Could not get parser for language {language}. Check https://github.com/Goldziher/tree-sitter-language-pack?tab=readme-ov-file#available-languages for a list of valid languages.")
        raise

if not hasattr(parser, "parse"):
    raise ImportError("The installed version of tree-sitter-language-pack is not compatible. Please install a compatible version: pip install 'tree-sitter-language-pack<1.0'")
```

三道：

1. **包没装** → `ImportError` 并给出「或者自己传 `parser`」的替代方案。
2. **语言名不在支持列表** → 打印一条带官方支持列表 URL 的消息后原样 `raise`。这里用 `print` 而不是 `logger`，且**打印之后仍然抛出**，于是用户既看到指引也看到原始异常。
3. **`parser` 没有 `parse` 方法** → 专门提示「所装版本不兼容，请装 `<1.0`」。这条检查是针对**具体的版本断裂**写的：新版 `tree-sitter-language-pack` 改了接口，`get_parser` 不再返回带 `.parse` 的对象。这类「针对一次真实事故写的检查」是依赖管理里最有信息量的东西——它说明 1.0 之前与之后是两个不兼容的世界。

**`split_text` 中间那一个条件判断是「解析失败」的唯一判据。**

```
text_bytes = bytes(text, "utf-8")
tree = self._parser.parse(text_bytes)

if (not tree.root_node.children or tree.root_node.children[0].type != "ERROR"):
    chunks = [chunk.strip() for chunk in self._chunk_node(tree.root_node, text_bytes)]
    event.on_end(payload={EventPayload.CHUNKS: chunks})
    return chunks
else:
    raise ValueError(f"Could not parse code with language {self.language}.")
```

**tree-sitter 的容错解析不会抛异常**——它对语法错误仍会返回一棵树，错误处用 `ERROR` 节点占位。于是「解析成功」必须靠检查**根节点的第一个子节点是不是 `ERROR`** 来推断。

这个判据有两个值得注意的性质：

1. **只看第一个子节点**。如果代码前半段合法、后半段语法错，`ERROR` 节点可能不在首位，于是**这份代码会被当成解析成功并正常切分**（产物里含着残缺的片段）。这是一个宽松到有点危险的判据，但它避免了「一个语法错误就让整份文件无法切分」。
2. **空树也被当成成功**（`not tree.root_node.children` 走同一支）。空输入于是产出空列表，而不是报错。

**`_chunk_node` 是真正的切分逻辑，它的状态分两个变量。**

```
new_chunks = []
current_chunk = ""
max_size = self.max_chars if self.count_mode == "char" else self.max_tokens

for child in node.children:
    child_text = text_bytes[child.start_byte : child.end_byte].decode("utf-8")
    child_size = len(child_text) if self.count_mode == "char" else len(self._tokenizer(child_text))
    ...
    last_end = child.end_byte
```

**注意 `child_text` 只用来度量，不用来累积**——实际累积的是 `text_bytes[last_end : child.end_byte]`（从上一个子节点的结束字节到当前子节点的结束字节）。这个区别很重要：AST 的子节点字节范围之间**可能有间隙**（空白、注释、括号），而 `child_text` 不含间隙。用 `last_end` 起算可以把间隙也包含进来，于是**产物能完整拼回原文**（不丢字符）。

`last_end` 在每次循环末尾无条件更新为 `child.end_byte`，这也是为什么进 `if child_size > max_size` 分支时，累积时要用 `last_end` 而不是上一个已处理节点——两者在正常路径下等价，但在「上一轮是递归」的情况下 `last_end` 仍是上一个**已消费**位置的正确值。

三支处理：

**一、子节点自己超预算 → 先收尾，再递归进去，若它是叶子则硬切。**

```
if child_size > max_size:
    if len(current_chunk) > 0:
        new_chunks.append(current_chunk)
    current_chunk = ""
    if child.children:
        new_chunks.extend(self._chunk_node(child, text_bytes, last_end))
    else:
        leaf_text = text_bytes[last_end : child.end_byte].decode("utf-8")
        new_chunks.extend(self._split_oversized_leaf(leaf_text, max_size))
```

叶子节点的处理是这一版新增的关键逻辑（注释写明「避免静默丢弃内容」）：一个**没有子结构的超大节点**（长字符串字面量、长注释、大段数据字面量）在旧实现里会被直接丢掉——因为「超预算 → 递归 → 没有 children → 什么也没产出」。现在的做法是拿它的原文硬切成若干块。

**这里有一个真实的字节偏移隐患**：递归调用 `_chunk_node(child, text_bytes, last_end)` 时传的是**父层的 `last_end`**（即上一个兄弟节点的结束位置），而不是 `child.start_byte`。于是递归层累积的第一段文本是「从上一个兄弟结束到第一个孙节点结束」，把 `child` 开头到第一个孙节点之间的内容（可能包括 `child` 自己的关键字，如 `def`）都算进了第一个孙节点的块。这大概是有意的（保住 `def` 之类的语法前缀），但它意味着**块边界不严格对应 AST 节点边界**。

**二、子节点不超预算，但加入后会让当前块超预算 → 收尾并开启新块。**

```
new_chunk_text = current_chunk + text_bytes[last_end : child.end_byte].decode("utf-8")
new_chunk_size = len(new_chunk_text) if self.count_mode == "char" else len(self._tokenizer(new_chunk_text))
if new_chunk_size > max_size:
    if len(current_chunk) > 0:
        new_chunks.append(current_chunk)
    current_chunk = text_bytes[last_end : child.end_byte].decode("utf-8")
else:
    current_chunk += text_bytes[last_end : child.end_byte].decode("utf-8")
```

**这里是 `count_mode` 的落点**：字符模式下新块大小是 `len(拼接后的文本)`，token 模式下是 `len(self._tokenizer(拼接后的文本))`。注意 token 模式下**每加一个子节点都要把整段重新分词**——一个 N 个子节点的块会做 N 次全文分词，复杂度是 O(N²) 的字符处理（不涉及网络，但大文件下确实慢）。这是「精确控制」的代价。

**三、末尾收尾**：`if len(current_chunk) > 0: new_chunks.append(current_chunk)`。

**`_split_oversized_leaf` 提供两种硬切，token 模式下的做法很讲究。**

```
if self.count_mode == "char":
    return [text[i : i + max_size] for i in range(0, len(text), max_size)]

# Token mode: greedily accumulate characters while staying within the
# token budget. This keeps the split reversible (decoding token ids is
# not guaranteed for an arbitrary tokenizer).
chunks = []
current = ""
for char in text:
    if current and len(self._tokenizer(current + char)) > max_size:
        chunks.append(current)
        current = char
    else:
        current += char
if current:
    chunks.append(current)
```

字符模式是切片，一句话搞定。**token 模式则是「逐字符累加 + 每次重新分词」，而注释给的理由是全篇最有意思的一处判断**：

> 这样切分保持可逆（对任意 tokenizer 都不保证能解码 token id）。

也就是说：本来最精确的做法是「先分词 → 按 token 数切 token 序列 → 再解码回文本」。但**解码不保证有对应实现**（`get_tokenizer()` 只要求给一个 `Callable[[str], List]`，反向映射不存在）。于是退一步：在**字符**上做贪心累积，用 tokenizer 只作为「度量器」。产物因此永远是原始文本的连续子串，不会出现解码失真。

这个取舍的代价是复杂度：逐字符、每个字符都要把当前累积串重新分词 → O(L²·分词成本)。对一段超长注释或字符串字面量，这可能是显著的耗时。而它在收益上是正确的——**宁可慢也不引入不可逆变换**。

`split_text` 最后对每个块做 `chunk.strip()`：

```
chunks = [chunk.strip() for chunk in self._chunk_node(tree.root_node, text_bytes)]
```

**strip 发生在最后一步，于是块与原文的字节偏移不再对应**（a01 里 `find` 重建偏移也因此可能失败——前导空白被 strip 后，`find` 仍能找到内容，但找到的是「strip 后文本」在原文中的位置，通常仍正确；只是如果块全是空白，strip 后变空串，节点就变成空文本）。这一点与 a02 的 `_postprocess_chunks` 是同一个信息损失点。

**`chunk_lines` 与 `chunk_lines_overlap` 这两个字段在实际路径里没有被读取。**

```
chunk_lines: int = Field(default=DEFAULT_CHUNK_LINES, description="The number of lines to include in each chunk.", gt=0)
chunk_lines_overlap: int = Field(default=DEFAULT_LINES_OVERLAP, description="How many lines of code each chunk overlaps with.", gt=0)
```

搜遍 `_chunk_node` / `_split_oversized_leaf` / `split_text` 都没有用到它们。也就是说：**默认值 40 行 / 15 行重叠是「只存在于字段声明上」的参数**，`CodeSplitter` 的实际行为完全由 `max_chars` / `max_tokens` 决定，且**没有重叠**。

这是一个需要明确指出的实现现状：按字段名与文档描述，用户会以为「每块 40 行、相邻块重叠 15 行」；实际产物是按字符/token 预算的块，且块之间不重叠。`DEFAULT_CHUNK_LINES` 与 `DEFAULT_LINES_OVERLAP` 两个常量的存在则说明**曾经（或曾计划）有过按行切分的实现**——字段保留了，路径换了。

这类「保留字段但不用」的情况对使用者是最不友好的一种：它不报错、不警告，配置了也看不出差别（除非去比对产物行数）。

## 关键取舍

**源码事实：** `TokenTextSplitter` 在构造期把降级阶梯算成一个函数列表（主分隔符 + 备份分隔符 + 按字符），切分时遍历该列表、**第一个切出多于一段的函数即生效**。

**源码事实：** 递归降级用「切分函数只产出一段」作为「切不动」的判据（而非「已到字符级」），于是既能覆盖「字符级也切不动」（CJK/emoji 单字符多 token），也不会无限递归；超长片段被保留而非报错。

**源码事实：** `_merge` 里对超预算片段的 `_logger.warning` 传了两个位置参数，而消息中没有 `%s` 占位符——`getMessage()` 里的 `msg % args` 抛 `TypeError`，被 `logging.handleError` 吞掉，于是这条警告**完全打不出来**（只留一段 `--- Logging error ---` 栈到 stderr）。

**源码事实：** 重叠通过「从头部 `pop(0)` 直到 `cur_len <= chunk_overlap` 且 `cur_len + split_len <= chunk_size`」实现，实质仍是复用尾部片段；`keep_whitespaces` 控制拼接后是否 `strip()`。

**源码事实：** 元数据感知路径把 `metadata_str` 的 token 数加上预留 2 后从 `chunk_size` 里扣掉，预算 ≤ 0 抛错、< 50 用 `print` 警告。

**源码事实：** `CodeSplitter` 只在 `parser is None` 时才导入 `tree_sitter_language_pack`，并对三种失败分别给消息：包缺失、语言不支持（打印官方列表 URL 后重抛）、`parser` 无 `.parse`（提示装 `<1.0`）。

**源码事实：** `CodeSplitter` 只检查**根节点的第一个子节点**是否为 `ERROR` 来判定解析失败；解析器对语法错误不抛异常，因此部分错误的代码会被当成功切分。

**源码事实：** `_chunk_node` 用 `child_text` 只做度量、用 `text_bytes[last_end : child.end_byte]` 做累积，于是 AST 子节点之间的间隙（空白、注释、括号）被包含进产物，文本可拼回原文。

**源码事实：** 超出预算的叶子节点（长字符串字面量、长注释）被 `_split_oversized_leaf` 硬切而不是丢弃；token 模式下用「逐字符贪心累积 + 每次重新分词」，注释明确说理由是不保证能解码 token id。

**源码事实：** `chunk_lines` 与 `chunk_lines_overlap` 两个字段以及 `DEFAULT_CHUNK_LINES` / `DEFAULT_LINES_OVERLAP` 常量在切分路径中**未被使用**——`CodeSplitter` 实际按 `max_chars` / `max_tokens` 切分且**无重叠**。

**架构推论：** 同一个物理约束（单个不可分割单位已超预算）在 `SentenceSplitter` 里是 `ValueError`、在 `TokenTextSplitter` 里是「保留 + 警告」。差异反映了定位：前者假定预算合理、冲突即配置错误；后者是基线工具，宁可产出超长块也不让整批中断。**同一情形采取不同策略不是不一致，而是「谁承担错误」的分配不同。**

**架构推论：** 「用产出段数判据代替层级判据」是递归降级的正确写法。若按「当前是第几级」来判断是否还能切，就无法在不增加层级的情况下覆盖「字符级也切不动」；而用「切出几段」这个观察结果，任何死角都自动被识别。

**架构推论：** `CodeSplitter` 的 `strip()` 与 `TokenTextSplitter` 的 `strip()` 是同一个取舍的两处实现：**为了让块的文本好看而牺牲与原文的位置对齐**。凡下游需要精确位置（引用高亮、增量重建），strip 都是信息损失点。

**架构推论：** 「字段保留但不使用」比「移除字段」更有害。移除会让旧代码报错并发现；保留则让配置看起来生效而实际无效。`chunk_lines` 这一对就是实例——默认值 40/15 会让用户以为存在行级切分与重叠。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/node_parser/text/token.py` | `TokenTextSplitter` | 按 token 预算切分；构造期把降级阶梯算成函数列表 |
| `llama-index-core/llama_index/core/node_parser/text/token.py` | `_split` | 递归降级切分，以「只产出一段」为切不动的判据 |
| `llama-index-core/llama_index/core/node_parser/text/token.py` | `_merge` | 按预算合并 + 头部弹出式重叠；含那处因 `logging` 格式串不匹配而完全失效的 warning |
| `llama-index-core/llama_index/core/node_parser/text/token.py` | `DEFAULT_METADATA_FORMAT_LEN` | 元数据格式自身的 token 预留（2） |
| `llama-index-core/llama_index/core/node_parser/text/code.py` | `CodeSplitter` | 按 AST 边界切代码；三档依赖检查与 `count_mode` 双预算 |
| `llama-index-core/llama_index/core/node_parser/text/code.py` | `_chunk_node` | 递归累积子树、超预算收尾、叶子硬切；用 `last_end` 累积以含间隙 |
| `llama-index-core/llama_index/core/node_parser/text/code.py` | `_split_oversized_leaf` | 字符切片 / 逐字符贪心（token 模式，为避免不可逆解码） |
| `llama-index-core/llama_index/core/node_parser/interface.py` | `TextSplitter` | 两个切分器共同的父类：只要求 `split_text`，其余全包 |

## 参考

- [llama-index 源码：node_parser/text/token.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/node_parser/text/token.py)
- [llama-index 源码：node_parser/text/code.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/node_parser/text/code.py)
- [Sweep AI：文档所述的分块思路来源](https://docs.sweep.dev/blogs/chunking-2m-files)
- [tree-sitter 文档](https://tree-sitter.github.io/tree-sitter/)

## 与主核 / AI 的关系

这两个切分器对「什么是一个不可分割的单位」给出了两种答案，而主核（多模态数据库内核）在**记录边界**上也必须回答同一个问题。三条经验。

**第一，递归降级的停止判据应当是「观察结果」而非「层级计数」。** `TokenTextSplitter` 用「切分函数只产出一段」来判断「切不动了」，于是它能自动覆盖「字符级也切不动」这种死角。主核里做「按页切分」或「按块压缩」时同理：判断依据应当是「这一轮到底缩小了没有」，而不是「已经尝试过几级策略」。前者任何死角都安全，后者需要把每种死角都枚举进层级里。

**第二，「宁可慢也不引入不可逆变换」这条准则值得抄。** `CodeSplitter` 在 token 模式下逐字符累积、每次重新分词，而不是「按 token 序列切再解码」。它明说理由：不保证能解码回来。主核里凡是做「压缩/编码后再切/再排」的地方都有同一个风险——若变换不可逆，切点就找不到原文位置。判断准则：**任何进入索引的东西都应当保留回到原文的路径**。

**第三，`strip()` 是全局性的信息损失点，必须在设计阶段决定。** 这里两个切分器都在产物上 strip。主核里若要做「从索引回到原文的精确引用」，任何 normalize 都会破坏偏移。与其在后期补一套「模糊定位」，不如在写入时就不 strip——或者像 `_chunk_node` 那样，用「上一位置的结束字节」累积以保证不丢字符，把 strip 推迟到展示层。

还有一条更细的：**「字段保留但不使用」是配置陷阱**。`chunk_lines` / `chunk_lines_overlap` 让用户以为自己配置生效了。主核的配置文件里若留着已废弃的参数，危害相同——它不报错、只在行为上 silently 不生效。宁可让废弃参数在解析时报错，也不要让它静默。
