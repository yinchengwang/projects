---
project: lightrag
feature: recursive-character
title: 递归字符分块与分隔符归一
page_type: feature
schema_version: 1
refs:
  - path: lightrag/chunker/recursive_character.py
    symbols: [chunking_by_recursive_character, _split_text_with_spans, _merge_splits_with_spans]
  - path: lightrag/chunker/recursive_character.py
    symbols: [normalize_r_separators, RSeparatorNormalization, inspect_r_separators]
---

# 递归字符分块与分隔符归一

## 一句话职责

`"R"` 策略把 LangChain 的 `RecursiveCharacterTextSplitter` 从「只给文本」改成「文本 + 原文区间」——切分与合并的每一步都算出 `(start, end)`，因为下游要把块映射回原文位置；同时把用户给的分隔符级联在有界化之后才使用，且把「做了修正」这件事记成结构体而非日志。

## 机制是什么

这个分块器有三个不显然的需求同时存在：

**需求一：每个块要能映射回原文。** 块要从原文里裁剪出来，但下游还需要知道它在原文的哪一段（用于 sidecar 回填、高亮、增量重切）。所以「区间」必须与「文本」一路同行，而不是事后用 `content.find()` 反查。

源码的模块文档把反查为什么不行说清楚了——它选择**不用** LangChain 的现成能力：

> We deliberately do *not* request LangChain's `add_start_index`. That offset is computed with a character-vs-token unit mismatch when a token-based `length_function` is in play, and text-search recovery is ambiguous for repeated blocks.

两个理由都不容回避：**单位错配**（LangChain 的 `add_start_index` 按字符数推进，而这里 `chunk_size` 是 token 数）与**重复片段歧义**（同一段文本出现两次，`find()` 只能找到第一处）。

**需求二：`chunk_size` 必须以 token 计。** 但递归切分是在**字符层面**做分隔（`"\n\n"`、`"\n"`、`" "`），只有长度测量进了 token 空间。所以这套实现里存在两个坐标：字符位置（`start`/`end`）与 token 长度（`length_function`）。区间用前者，容量判断用后者。混用就会出错——见需求一的第一个理由。

**需求三：分隔符级联是攻击面。** 一个用户可控的 `separators` 列表可以做两件事：塞进超长分隔符（正则编译开销），或塞进成千上万个分隔符（每一级都要对全文做一次 `re.search`）。所以必须有上界。而上界化会改变行为，所以「改了」这件事必须能被上游报告一次——但不能在热路径上每次都报。

## 怎么实现的

**`_SpanPiece` 就是一个三元组，而整个文件的复杂度都来自给它算准确的第三项。**

```python
_SpanPiece = tuple[str, int, int]
```

`(text, start, end)`，`end` 是**开区间**。文件里所有函数都吃它、吐它。

**`_split_text_with_regex_spans` 逐字镜像 LangChain 的正则切分，只多了偏移量。**

```python
if not separator_pattern:
    return [(char, base_offset + index, base_offset + index + 1)
            for index, char in enumerate(text) if char]

matches = list(re.finditer(separator_pattern, text))
if not matches:
    return [(text, base_offset, base_offset + len(text))] if text else []
```

两处：

1. **空 pattern 是字符级兜底**：逐字符成片。这是级联最后一级（空字符串分隔符）的语义。`if char` 挡掉空字符串（不可能出现，但防御）。

2. **`if not matches` 返回整段**。注意这里**没有**用 `keep_separator` 的逻辑——因为无匹配时无论 keep 与否结果都一样。这是一个正确的早退。

`keep_separator` 有三个值，而三个分支的差别全在「分隔符归谁」：

```python
if keep_separator:
    if keep_separator == "end":
        # 分隔符归到前一片的末尾
        cursor = 0
        for match in matches:
            if match.end() > cursor:
                pieces.append((text[cursor:match.end()], base_offset+cursor, base_offset+match.end()))
            cursor = match.end()
        if cursor < len(text):
            pieces.append((text[cursor:], base_offset+cursor, base_offset+len(text)))
    else:
        # 分隔符归到后一片的开头
        first = matches[0]
        if first.start() > 0:
            pieces.append((text[:first.start()], base_offset, base_offset+first.start()))
        for index, match in enumerate(matches):
            end = matches[index+1].start() if index+1 < len(matches) else len(text)
            if end > match.start():
                pieces.append((text[match.start():end], base_offset+match.start(), base_offset+end))
else:
    # 分隔符丢弃
    cursor = 0
    for match in matches:
        if match.start() > cursor:
            pieces.append((text[cursor:match.start()], base_offset+cursor, base_offset+match.start()))
        cursor = match.end()
    if cursor < len(text):
        pieces.append((text[cursor:], base_offset+cursor, base_offset+len(text)))
```

四处：

1. **`if not separator_pattern` 用真值判断**，所以 `keep_separator == ""` 会走「丢弃」分支（因为 `""` 是假值）。这与 LangChain 的语义一致。

2. **`"end"` 与其他真值分开** —— 因为 LangChain 的 `keep_separator` 类型是 `bool | Literal["start", "end"]`，而 `"start"` 与 `True` 走同一分支（都是「归到后一片开头」）。

3. **每个分支末尾都有 `if cursor < len(text)`**（`"end"`/`False` 两支）或 `end = len(text)`（`"start"` 支）——保证最后一段不丢。

4. **最后 `[piece for piece in pieces if piece[0]]`** 过滤空片。三处 `if ... > cursor` / `if ... > match.start()` 已经防了空片，这一句是双保险。

**`_join_span_pieces` 是「拼回去的同时算出裁剪后的区间」，而它的 `char_offsets` 是关键数据结构。**

```python
chars: list[str] = []
char_offsets: list[int] = []
for index, (fragment, start, end) in enumerate(pieces):
    if index > 0 and separator:
        previous_end = pieces[index - 1][2]
        for sep_index, sep_char in enumerate(separator):
            chars.append(sep_char)
            char_offsets.append(previous_end + sep_index)
    chars.extend(fragment)
    char_offsets.extend(range(start, end))
```

四处：

1. **`chars` 与 `char_offsets` 平行增长，一一对应**。所以拼出来的第 `i` 个字符在原文的位置是 `char_offsets[i]`。这是「拼接不破坏位置信息」的实现方式——**每个字符都记得自己从哪来**。

2. **插入的分隔符的偏移用 `previous_end + sep_index`** —— 即假装分隔符紧跟在前一片末尾。但**分隔符本身在原文里的真实位置可能离得很远**（因为它是被丢弃/归到别处的那一份）。

   这是一个**刻意的近似**：拼接出来的文本在「分隔符混进了块里」这个意义上是**原文里不存在的字符串**。所以 `char_offsets` 里那几项并不是「这个字符的真实来源」，而是「如果它在原文里，应该在哪」。于是拼出来的块 `text` **未必是 `content[start:end]` 的子串**。

   这一点很重要：`_source_span` 给出的区间是「覆盖范围」，不是「精确切片」。下游若用它做 `content[start:end]` 取原文，得到的内容可能与块的 `content` 不同（多了或少了分隔符）。

3. **`range(start, end)` 用 `extend`** —— 所以是 O(end-start) 的内存。一个 1200 token 的块约几千个 int，可接受；但这说明这个实现的内存占用与文本长度线性相关（而不是 O(块数)）。

4. **`if index > 0 and separator`** —— 第一片前不加分隔符，且 `separator` 为空时不加。

然后裁剪：

```python
text = "".join(chars)
if strip_whitespace:
    left = 0
    right = len(text)
    while left < right and text[left].isspace():
        left += 1
    while right > left and text[right - 1].isspace():
        right -= 1
else:
    left, right = 0, len(text)

if left >= right:
    return None
return text[left:right], char_offsets[left], char_offsets[right - 1] + 1
```

三处：

1. **`text[left]` 与 `text[right-1]` 而不是 `chars`** —— 因为已经 `join` 了。注意这里对**拼接后**的文本裁剪，所以裁掉的可能是一个插入的分隔符。

2. **`left >= right` 返回 `None`** —— 全空白片被丢弃。所以调用方必须处理 `None`（`_merge_splits_with_spans` 里两处都判了）。

3. **`char_offsets[right - 1] + 1`** —— 末尾用最后一个**被保留**字符的偏移加一。所以区间正好覆盖保留部分。

   注意左边用 `char_offsets[left]` 而右边是 `char_offsets[right-1]+1`。这个不对称是必须的：`char_offsets` 给出的是每个字符的起始位置，所以末字符的**结束位置**是它的起始加一。当插入的分隔符占用连续偏移时（`previous_end + sep_index`），`+1` 恰好接上。

**`_merge_splits_with_spans` 镜像 LangChain 的 `_merge_splits`，而重叠回退的循环条件是一个嵌套的复合条件。**

```python
for split in splits:
    split_len = length_function(split[0])
    if total + split_len + (separator_len if len(current_doc) > 0 else 0) > chunk_size:
        if total > chunk_size:
            logger.warning("Created a chunk of size %d, which is longer than the specified %d", total, chunk_size)
        if len(current_doc) > 0:
            doc = _join_span_pieces(current_doc, separator, strip_whitespace=strip_whitespace)
            if doc is not None:
                docs.append(doc)
            while total > chunk_overlap or (
                total + split_len + (separator_len if len(current_doc) > 0 else 0) > chunk_size
                and total > 0
            ):
                total -= length_function(current_doc[0][0]) + (separator_len if len(current_doc) > 1 else 0)
                current_doc = current_doc[1:]
    current_doc.append(split)
    total += split_len + (separator_len if len(current_doc) > 1 else 0)
```

五处：

1. **`separator_len = length_function(separator)` 在循环外算一次**。所以分隔符的长度也按 token 计——一个 `"\n\n"` 可能是 1 token 也可能更多，取决于 tokenizer。

2. **`total > chunk_size` 的警告在「当前累积已超」时触发**，不是「加下一个才超」时。所以它报的是「已经出了一个超长块」——这只能发生在单个 split 本身超过 `chunk_size` 时（那它单独成块）。

   这也是**输出大小上限不由本函数保证**的体现：模块文档明说「Output cap is *not* enforced internally: oversized segments are produced when no separator can break them, and `enforce_chunk_token_limit_before_embedding` does the final hard split before embedding」。

3. **回退循环的 `or` 两边做不同的事**：
   - `total > chunk_overlap` → 还要继续退（重叠量超了）
   - 或者「退了之后仍然放不下新 split 且还有东西可退」（`... > chunk_size and total > 0`）

   第二个条件里的 `and total > 0` 是**防死循环**：若 `total` 已归零但从 `current_doc[0][0]` 减出来非零（理论上不会，因为 `total` 就是各项之和），会一直循环。这个 `total > 0` 是多余的保险，但它保证终止。

   注意第二个条件里的 `len(current_doc) > 0` **在循环内不会变假**（循环体要么减到空、要么 `total` 到 0）——实际可能变假：`current_doc = current_doc[1:]` 可以让它变空，此时 `current_doc[0]` 会 `IndexError`。

   **所以这个循环有一个真实的越界风险**：若 `total > chunk_overlap` 成立且 `current_doc` 只剩一项，循环体会取 `current_doc[0]`、减完、切片成空列表，然后条件重新求值：`total > chunk_overlap` 此时 `total` 是 0（或负），通常为假；第二支 `len(current_doc) > 0` 为假 → `and` 链短路为假。所以退出。**不会越界，但依赖 `total` 在减到空时必然 ≤ overlap**。若 `chunk_overlap` 是负的（本函数允许，虽然 `chunking_by_recursive_character` 会把负值归零再传），`total > chunk_overlap` 会一直为真 → `IndexError`。这是一个**依赖上游把重叠归零**的脆弱点。

4. **`len(current_doc) > 0` 与 `len(current_doc) > 1` 的混用**：条件判断用 `> 0`（「已有内容则要加分隔符」），`total` 增减用 `> 1`（「加完这一片后是否已有两片」）。这两个数在 `total +=` 那行求值时机不同——那时 `current_doc` 刚 append 过，所以 `> 1` 意味着「不是第一片」。逻辑对，但读起来需要小心。

5. **`if len(current_doc) > 0:` 这个内层守卫是多余的** —— 外层条件已经是「加新 split 会超」，而此时 `current_doc` 可能为空（第一个 split 就超大）。所以这个守卫是必要的，不是多余。它保证「第一个 split 本身超大时」直接落到 `current_doc.append(split)`，然后下一轮才处理。

**`inspect_r_separators` 的有界化，而它最细的一段是「截断时必须保住哨兵」。**

```python
if separators is None:
    return RSeparatorNormalization(None)

bounded: list[str] = []
dropped = 0
for candidate in separators:
    if len(candidate) > MAX_R_SEPARATOR_CHARS:
        dropped += 1
        continue
    bounded.append(candidate)
```

两处：

1. **`None` 原样透传**，而文档特别强调这不是个小细节：

> `None` passes straight through: it is the documented way to ask `chunking_by_recursive_character` for LangChain's own default cascade (`["\n\n", "\n", " ", ""]`), which is **NOT** `DEFAULT_R_SEPARATORS` — turning one into the other here would change how a direct SDK caller's CJK text splits.

   即项目级的默认级联与 LangChain 的默认级联**不同**，且差异影响 CJK 切分。所以「`None` 保持 `None`」是为了让直接调用者的行为不变。

2. **超长分隔符被丢弃而不是截断**。因为截断一个分隔符会造出一个**从没被配置过的分隔符**——那比丢弃更糟。

截断那段是核心：

```python
if len(bounded) > MAX_R_SEPARATORS:
    # Truncation must preserve a terminating sentinel. The empty string is
    # the char-level fallback: ``_split_text_with_spans`` takes the
    # ``if not candidate`` branch on it and splits anything. Dropping it
    # exhausts ``new_separators`` early, so a segment that has no ordinary
    # separator in it is emitted whole instead of being split — a data-quality
    # regression introduced by a security fix. Only re-append a sentinel the
    # caller actually had: ``load_chunk_separators`` strips it on purpose.
    sentinel = "" if any(not candidate for candidate in bounded) else None
    keep = MAX_R_SEPARATORS - (1 if sentinel is not None else 0)
    truncated = [candidate for candidate in bounded[:keep] if candidate]
    if sentinel is not None:
        truncated.append(sentinel)
    truncated_from = len(bounded)
    bounded = truncated
```

五处，这段注释值一整段分析：

1. **「截断必须保住终止哨兵」** —— 空字符串分隔符是字符级兜底：`_split_text_with_spans` 在它上面走 `if not candidate` 分支并切任何东西。若截断把它砍掉，`new_separators` 会提前耗尽，于是一个「内部没有普通分隔符」的段会**整段输出而不被切**。

2. **注释直说了这是「安全修复引入的质量回归」** —— 「a data-quality regression introduced by a security fix」。即：加 `MAX_R_SEPARATORS` 是为了防攻击，但它顺带砍掉了哨兵，导致正常文本切不开。这是一处**安全加固伤到功能**的实例，而修法是把两件事拆开（截断内容、保留哨兵）。

3. **`keep = MAX_R_SEPARATORS - (1 if sentinel is not None else 0)`** —— 先为哨兵留一个位置，再取前 `keep` 个。所以最终长度恰为 `MAX_R_SEPARATORS`。

4. **`if candidate` 过滤掉空串**（因为 `bounded[:keep]` 里可能有空串，而哨兵要单独加在末尾）。所以空串被移到末尾去了——**这改变了级联顺序**。

   这是一个真实的语义变化：若用户写 `["\n\n", "", "\n"]`，截断后变成 `["\n\n", "\n", ""]`。空串从中间挪到末尾，于是 `"\n"` 这一级会被使用，而原配置下空串会先命中（因为空串在 `"\n"` 之前）。

   从「级联是从强到弱」的语义看，空串本就该在末尾（它最弱），所以这个重排是**修正**而非破坏。但它没有被注释提到。

5. **「Only re-append a sentinel the caller actually had: `load_chunk_separators` strips it on purpose」** —— 所以若调用方原本没有空串（即它故意依赖「不切到字符级」的行为），这里也不加。这是一个**尊重调用方意图**的细节：安全机制不擅自改变语义，只保住已有的语义。

`RSeparatorNormalization` 的三个字段与 `changed` 属性：

```python
@dataclass(frozen=True)
class RSeparatorNormalization:
    separators: list[str] | None
    dropped: int = 0
    truncated_from: int | None = None

    @property
    def changed(self) -> bool:
        return self.dropped > 0 or self.truncated_from is not None
```

两处：

1. **`frozen=True`** —— 但注意它含一个 `list` 字段，所以它**实际可变**（`frozen` 只阻止字段重新赋值，不阻止 `separators.append`）。这是一处「frozen 给了错误的保证」。

2. **`changed` 是派生属性而不是第四个字段** —— 所以「有没有修正」永远与两个计数一致，不会不同步。

**两个入口的分工是本页的设计要点：`normalize_r_separators` 静默，`log_r_separator_normalization` 才说话。**

```python
def normalize_r_separators(separators):
    """Return a bounded separator cascade without logging.
    This is the runtime backstop shared by direct SDK calls and per-document snapshots.
    Warnings belong to the configuration ingress points ..."""
    return inspect_r_separators(separators).separators

```

而 `RSeparatorNormalization` 的类文档解释了为什么不在这里告警：

> The normalizer is deliberately silent: it is also the final safety boundary for direct SDK calls and old persisted document snapshots, where a warning per document would turn one historic bad configuration into a log storm.

**这是一处很实在的运维判断**：一个 10 万文档的库，若每篇都告警一次同样的配置问题，日志会被淹掉，而「真正的新问题」也被埋掉。所以告警被移到**配置入口**（只发生一次），而**运行期边界静默**。

而 `log_r_separator_normalization` 的文档进一步要求：「Callers must invoke this only at a configuration ingress boundary」。

**`_split_text_with_spans` 是镜像 LangChain `_split_text` 的实现，而它唯一的偏离是为了修一个 DoS。**

模块文档与函数文档把这件事写得很完整：

> One departure from a literal mirror, for cost only: a separator that matches but does not actually divide the text is skipped in place instead of via a recursive call. With `keep_separator` truthy, a separator occurring exactly once at offset 0 yields a single piece byte-identical to the input, so the parent would call `length_function` on it — a whole-text token encode — and recurse with the same text. A cascade of such separators therefore costs one full encode per level while splitting nothing, which is what made `O(len(separators) x len(text))` reachable from a single request (GHSA-26pm-px5v-8c4w).

三处：

1. **「匹配但不切」的分隔符**（出现一次且在偏移 0）会让 `keep_separator` 为真时产出**与输入完全相同的一片**。于是父层会：对整个文本做一次 token encode → 递归 → 进同样的局面。级联 N 个这样的分隔符就是 N 次全文 encode。

2. **修法不是限制级联长度，而是「就地跳过」**：`remaining = new_separators; continue` —— 不递归，直接在循环里换下一级。状态等价，但省掉 N 次 encode。

3. **这个修复有前提**，注释专门说了：

> That holds only because the skip first checks the piece's length: LangChain sends a piece below `chunk_size` to the merge step instead of recursing, so a short text whose first separator is a no-op must stop here rather than fall through to a weaker separator (down to `""`), which would fragment it.

   即「跳过」只在**文本超长时**才对；若文本本来就不超，LangChain 会把它交给合并而不是继续切，所以这里必须 `break` 而不是 `continue`。这个区分就落在两行：

```python
if unsplit_length is None:
    unsplit_length = length_function(text)
if unsplit_length < chunk_size:
    break
remaining = new_separators
continue
```

**「短则停、长则跳」** —— 这个二分是「等价于递归」的充分条件，也是这段代码最容易被误改的地方。

而 `unsplit_length` 的缓存（`known_length` 参数）是第二层优化：

```python
def _split_text_with_spans(..., known_length: int | None = None):
    ...
    unsplit_length: int | None = known_length
```

注释：「The length is computed at most once per call, and not at all when the caller passes `known_length` (the parent has already measured the piece it recurses into)」。

所以递归调用时把父层已算过的长度传下去，省一次全文 encode。

**主函数 `chunking_by_recursive_character` 的六段处理，每段都对应一类真实误用。**

```python
if not _LANGCHAIN_TEXT_SPLITTERS_AVAILABLE:
    raise ImportError("langchain-text-splitters is required for the 'R' chunking strategy; install with `pip install langchain-text-splitters>=0.3`.")

if not content or not content.strip():
    return []
```

两处：

1. **可选依赖缺失时报可操作的错**（给出 pip 命令与版本）。`try/import` 在模块顶层，所以这是「模块能 import 但功能不可用」的情形——比「模块 import 失败」好得多，因为后者会让整个 `lightrag.chunker` 包不可用。

2. **`not content or not content.strip()`** —— 空白文本返回空列表而不是一个空块。这个判断有两层：`not content` 挡空串，`not content.strip()` 挡全空白。

然后是有界化与空级联的兜底：

```python
separators = normalize_r_separators(separators)
if separators is not None and not separators:
    separators = None
```

**注意 `is not None and not separators`** —— 区分「原本是 `None`」与「原本有内容但全被丢弃」。后者退回到 `None`（LangChain 默认级联），注释解释了为什么不退到 `DEFAULT_R_SEPARATORS`：

> Falling back to this function's own default (LangChain's cascade) rather than to `DEFAULT_R_SEPARATORS` keeps the behaviour identical to `separators=None`; substituting the repo-wide cascade here would silently change how CJK text splits for a caller who never asked for it.

而且**强调空列表比退回 `None` 更糟**：「`_split_text_with_spans` reads `separators[-1]` on its first line」——空列表会 `IndexError`。

接下来两个参数校验：

```python
resolved_chunk_size = int(chunk_token_size)
if resolved_chunk_size < 1:
    # Never floor a non-positive size to 1: that turns a misconfiguration
    # into one chunk per token and multiplies the extraction LLM calls.
    raise ValueError(f"chunk_token_size must be >= 1, got {chunk_token_size!r}")
resolved_chunk_overlap = max(int(chunk_overlap_token_size), 0)
if resolved_chunk_overlap >= resolved_chunk_size:
    raise ValueError(f"chunk_overlap_token_size ({resolved_chunk_overlap}) must be < chunk_token_size ({resolved_chunk_size})")
```

三处：

1. **`chunk_size < 1` 直接抛，不归一到 1**。注释的理由是**成本**而非正确性：「that turns a misconfiguration into one chunk per token and multiplies the extraction LLM calls」。一个配错成 0 的 `chunk_token_size` 若被归一到 1，会让一篇文档产出几十万个块，每块都可能触发一次抽取 LLM 调用。**所以这是「宁可启动失败也不让账单爆炸」的判断。**

2. **重叠的负值归零，其余不归**。所以 `-100` 变成 `0`（宽容），而 `>= size` 报错（严格）。这个不对称是对的：负重叠没有合理语义，归零即可；而重叠不小于块大小是**配置矛盾**，必须让用户知道。

3. **`>=` 而不是 `>`** —— 重叠等于块大小会退化成「每块前进 0 字符」，无限循环。所以必须排除等号。

   注释说明了为什么在这里重复校验一次：「addon_params, direct SDK calls, and persisted per-doc snapshots all bypass that validator and reach this function directly, so without this check an out-of-range overlap would instead surface as LangChain's own uncontrolled `ValueError` from the splitter constructor below (or, for overlap == size exactly, would not be rejected at all)」。

   **最后那句是本页最值得记的一条**：LangChain 自己的校验对 `overlap == size` **不报错**，所以只依赖上游校验会漏掉这一种。

然后是调用与结果组装：

```python
splitter_kwargs: dict[str, Any] = {
    "chunk_size": resolved_chunk_size,
    "chunk_overlap": resolved_chunk_overlap,
    "length_function": length_function,
    "strip_whitespace": True,
}
if separators is not None:
    splitter_kwargs["separators"] = list(separators)

splitter = RecursiveCharacterTextSplitter(**splitter_kwargs)
```

**构造 `splitter` 只是为了读它的私有属性**：

```python
pieces = _split_text_with_spans(
    content,
    base_offset=0,
    separators=list(splitter._separators),
    chunk_size=int(splitter._chunk_size),
    chunk_overlap=int(splitter._chunk_overlap),
    length_function=length_function,
    keep_separator=splitter._keep_separator,
    is_separator_regex=bool(splitter._is_separator_regex),
    strip_whitespace=bool(splitter._strip_whitespace),
)
```

四处：

1. **五个私有属性**（`_separators`/`_chunk_size`/`_chunk_overlap`/`_keep_separator`/`_is_separator_regex`/`_strip_whitespace`——六个）。所以这个实现与 LangChain 的内部结构**强耦合**：升级 LangChain 改私有名就会坏。

   为什么要这么做？因为要复用 LangChain 的**默认值解析**（`separators=None` 时它填什么、`keep_separator` 默认什么）。自己硬编码这些默认值就会与 LangChain 漂移。所以这是一个「用耦合换一致性」的选择——**耦合的是私有属性，换来的是「行为与 LangChain 一致」这个更强的保证**。

2. **`int()` / `bool()` 强转** —— 防 LangChain 内部把值存成别类型（比如 numpy 整数）。

3. **`base_offset=0`** —— 顶层从 0 开始，递归调用时传 `split[1]`。

4. **`length_function` 是本函数内的闭包**：

```python
def length_function(text: str) -> int:
    return len(tokenizer.encode(text))
```

   每次调用都重新 encode，**没有缓存**。所以 `_merge_splits_with_spans` 里对同一个片反复 `length_function(current_doc[0][0])` 会重复 encode（回退循环里每退一片就 encode 一次）。这是一个真实的性能点——但因为回退次数受 `chunk_overlap` 限制，量级可控。

结果组装里有两处值得看：

```python
for raw_body, start_index, end_index in pieces:
    left = 0
    right = len(raw_body)
    while left < right and raw_body[left].isspace(): left += 1
    while right > left and raw_body[right - 1].isspace(): right -= 1
    body = raw_body[left:right]
    if not body:
        continue
    start_index += left
    end_index -= len(raw_body) - right
```

三处：

1. **这里的裁剪与 `_join_span_pieces` 的裁剪重复**。对于走了合并路径的片（`_merge_splits_with_spans` 产出的），`_join_span_pieces` 已经 `strip_whitespace=True` 裁过，所以 `left = 0`、`right = len(raw_body)`，这段是空转。但**对于「无法再切」而直接 `final_chunks.append(split)` 的片**（那就是 `_split_text_with_regex_spans` 的原始片，带首尾空白），这段是必需的。

   所以一段代码要同时服务「已裁」与「未裁」两种输入——它必须自己再裁一次，即使多数情形是空转。

2. **区间随裁剪调整**：`start_index += left`、`end_index -= len(raw_body) - right`。所以 `_source_span` 指向的是**保留部分**，不是原始片。

3. **`if not body: continue`** —— 全空白片被跳过。所以块数可能少于 `pieces` 数。

最后是防御性兜底：

```python
if not results:
    logger.warning("[recursive_character] splitter produced no non-empty chunks for %d-char input; emitting single fallback chunk.", len(content))
    body = content.strip()
    if body:
        start = content.find(body)
        results.append({
            "tokens": len(tokenizer.encode(body)),
            "content": body,
            "chunk_order_index": 0,
            **({"_source_span": {"start": start, "end": start + len(body)}} if start >= 0 else {}),
        })
```

四处：

1. **只在「输入非空但输出空」时触发** —— 即全空白被切碎的情形。这时返回一个块而不是空列表，保证「非空输入至少有一个块」。

2. **`content.find(body)`** —— 这里是**唯一**用查找反推位置的地方。它可行是因为 `body` 是 `content.strip()`，只去掉了首尾空白，所以 `find` 一定能找到（`start >= 0` 恒成立）。

   但 `start >= 0` 的判断还是留着（`**({} if start >= 0 else {})`），所以「找不到就不给 span」——这是一个正确的降级：宁可没有区间，也不要给错区间。

3. **`chunk_order_index: 0`** 而正常路径是 `len(results)`。所以顺序索引始终从 0 开始连续。

4. **缺 `_source_span` 时该键完全不存在**（而不是 `None`）——所以下游必须用 `.get()` 而不是 `[]`。这与正常路径「总是有该键」不一致。**这是一个真实的接口不一致**：下游若写 `chunk["_source_span"]["start"]`，在这个兜底路径上会 `KeyError`。

## 关键取舍

**源码事实：** `_SpanPiece = tuple[str, int, int]`（`end` 开区间），切分与合并全程携带原文偏移；故意**不用** LangChain 的 `add_start_index`，理由是「token-based `length_function` 下单位错配」+「重复块用文本查找有歧义」。

**源码事实：** 存在两个坐标系：字符位置（span）与 token 长度（`length_function`）。区间用字符、容量判断用 token。

**源码事实：** `_split_text_with_regex_spans` 三支 `keep_separator`（`False` 丢弃 / `"end"` 归前片 / 其他真值归后片），末段都有 `if cursor < len(text)` 兜底，最后过滤空片。

**源码事实：** `_join_span_pieces` 用平行的 `chars` + `char_offsets` 记录「每个字符的原文位置」；**插入的分隔符用 `previous_end + sep_index` 伪造偏移**，所以拼出的 `text` 未必是 `content[start:end]` 的子串，`_source_span` 是「覆盖范围」而非「精确切片」；`left >= right` 返回 `None`。

**源码事实：** `_merge_splits_with_spans` 的 `separator_len = length_function(separator)`（分隔符也按 token 计）；`total > chunk_size` 的警告报的是「已产出超长块」；回退循环的 `and total > 0` 防死循环——但**若 `chunk_overlap` 为负则该循环会 `IndexError`**，实际安全依赖调用方已把负值归零。

**源码事实：** **输出大小不保证**：模块文档明说超长段会产出，最终硬切由 `enforce_chunk_token_limit_before_embedding` 在嵌入前做。

**源码事实：** `inspect_r_separators` 丢弃超长分隔符（`> MAX_R_SEPARATOR_CHARS`，不截断——因为截断会造出没被配置过的分隔符）；级联超 `MAX_R_SEPARATORS` 时**必须保住空串哨兵**，注释直说丢了哨兵是「a data-quality regression introduced by a security fix」；`keep = MAX - (1 if sentinel)` 先给哨兵留位；且**只重加调用方原本就有的哨兵**（`load_chunk_separators` 会故意剥掉它）。

**源码事实：** 截断时空串会被移到级联末尾（`[c for c in bounded[:keep] if c]` 再 `append(sentinel)`），即**改变级联顺序**——从强到弱的语义看这是修正，但未被注释提及。

**源码事实：** `RSeparatorNormalization` 是 `frozen=True` 但含 `list` 字段，所以只阻止字段重赋值、不阻止列表修改；`changed` 是派生属性，与两个计数不会不同步。

**源码事实：** 归一化静默、告警只在配置入口（`log_r_separator_normalization`），理由是「a warning per document would turn one historic bad configuration into a log storm」。

**源码事实：** `None` 必须原样透传——LangChain 默认级联与 `DEFAULT_R_SEPARATORS` **不同**，互换会改变 CJK 切分行为。

**源码事实：** `_split_text_with_spans` 唯一的偏离（就地跳过而非递归）是为修 GHSA-26pm-px5v-8c4w：匹配但不切的分隔符会造成「每级一次全文 encode」。修复成立的前提是**「短则 `break`、长则 `continue`」**——短文本 LangChain 会交给合并而不是继续切，若落到更弱的分隔符（直到空串）会把它打碎。`known_length` 让父层复用已算的长度。

**源码事实：** `chunk_token_size < 1` 直接抛而不归一到 1，注释理由是成本（「turns a misconfiguration into one chunk per token and multiplies the extraction LLM calls」）；负重叠归零、`overlap >= size` 报错——注释指出 LangChain 自己对 `overlap == size` **不报错**，所以只靠上游校验会漏。

**源码事实：** 构造 `RecursiveCharacterTextSplitter` 只为读六个私有属性（`_separators`/`_chunk_size`/`_chunk_overlap`/`_keep_separator`/`_is_separator_regex`/`_strip_whitespace`）——用「耦合私有属性」换「与 LangChain 默认值一致」。`length_function` 每次调用都重新 encode，无缓存。

**源码事实：** 结果组装对**已裁**（合并路径）与**未裁**（无法再切的原始片）都再裁一次，并相应调整 span；全空白片被跳过。零块时的兜底块用 `content.find(body)` 反查位置（此处可行因为只去了首尾空白），`start < 0` 时**完全省略 `_source_span` 键**——所以下游必须用 `.get()`，直接下标会 `KeyError`。

**架构推论：** 「span 全程伴随」这个要求的真实成本是「每个字符都要记住自己的来源」。`char_offsets` 是那个记账本，而它暴露了一处无法回避的近似：**拼接时插入的分隔符在原文里没有位置**，只能伪造。所以「块有区间」与「块等于区间切片」是两件事，把前者当后者用会在含分隔符合并的块上出错。这个限制是拼接式分块的固有属性，不是实现缺陷——除非改成「块始终是原文的连续切片」，那就必须放弃分隔符插入（LangChain 的 `keep_separator=False` 路径）。

**架构推论：** 「安全修复引入质量回归，再单独修回来」这个模式在 `inspect_r_separators` 的哨兵处理里非常清楚。值得记的是它修得很克制：**只保住调用方原本有的哨兵**。若顺手「总是补一个空串」，那些故意不切到字符级的配置就会被静默改变——安全加固的边界应该止于「消除攻击面」，不该顺手规范用户的行为。

**架构推论：** 「静默归一 + 入口告警」这个分工是运维友好性的正确形态。判据是**告警的重复次数**：一个配置错误在一次进程生命周期里只该告警一次，而运行期边界会被每个文档碰到。把告警放在「配置被缓存的那一刻」正好满足这个要求，而且它天然知道「这是第一次」。代价是任何绕过配置入口的路径（直接 SDK 调用）不会被告警——所以函数里留了 `inspect_r_separators` 让愿意查的人能查。

**架构推论：** 「宁可报错也不归一到安全值」这条与常见做法相反。`chunk_token_size=0` 归一到 1 看起来「友好」，但它把「配置错」变成了「成本爆炸」——而成本爆炸的表现形式（LLM 账单）与原因（一个配置项）相隔很远。所以这里的判据是**错误的可见性**：让它在启动时就炸，而不是在账单上炸。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/chunker/recursive_character.py` | `_SpanPiece` | `(text, start, end)` 三元组 |
| `lightrag/chunker/recursive_character.py` | `RSeparatorNormalization` | 有界化结果与修正诊断（`frozen`） |
| `lightrag/chunker/recursive_character.py` | `_split_text_with_regex_spans` | 镜像 LangChain 正则切分并带偏移 |
| `lightrag/chunker/recursive_character.py` | `_join_span_pieces` | 拼接 + 记 `char_offsets` + 裁剪后算区间 |
| `lightrag/chunker/recursive_character.py` | `_merge_splits_with_spans` | 镜像 `_merge_splits`，含重叠回退循环 |
| `lightrag/chunker/recursive_character.py` | `inspect_r_separators` | 丢弃超长、截断并保住空串哨兵 |
| `lightrag/chunker/recursive_character.py` | `log_r_separator_normalization` | 仅在配置入口调用的两条告警 |
| `lightrag/chunker/recursive_character.py` | `normalize_r_separators` | 静默的运行时兜底 |
| `lightrag/chunker/recursive_character.py` | `_split_text_with_spans` | 镜像 `_split_text` + 跳过空操作分隔符的 DoS 修复 |
| `lightrag/chunker/recursive_character.py` | `chunking_by_recursive_character` | 六段处理：依赖、空文本、有界化、参数校验、调用、组装 |

## 参考

- [lightrag 源码：chunker/recursive_character.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/chunker/recursive_character.py)
- [LangChain 文档：RecursiveCharacterTextSplitter](https://python.langchain.com/docs/how_to/recursive_text_splitter/)
- [lightrag 安全公告 GHSA-26pm-px5v-8c4w](https://github.com/HKUDS/LightRAG/security/advisories)
- [RFC 3986：URI 通用语法（原文定位相关）](https://datatracker.ietf.org/doc/html/rfc3986)

## 与主核 / AI 的关系

「块要能映射回原文」与主核（多模态数据库内核）的**元组 `ctid`（物理位置）** 是同一种需求的两层实现。主核的每个元组自带页号 + 槽号，所以「这条数据在哪」是精确的、免费的；这里的块没有物理位置，只能靠切分过程**顺带算出**偏移。差别在于主核的位置由存储层维护、这里的位置由算法维护——所以主核不可能算错，而这里需要 `char_offsets` 那样一整套记账。

**而这里暴露的那个近似（插入的分隔符没有真实位置）在主核里不存在**，因为主核的元组永远是页里的一段连续字节。「块可能不等于区间切片」是**派生数据**固有的问题：块是被算法**构造**出来的，不是被**切下来**的。判据是「这块内容是否已存在于原文」——存在则可以给精确区间，不存在（被拼接/加了分隔符/加了标题）就只能给覆盖范围。

第二处对照是 `inspect_r_separators` 的「安全上界伤到功能再修回来」，与主核的 **`max_stack_depth`** 一类上限设置同理。任何为了防 DoS 而加的上限都会在边界上改变行为，正确做法是「上限只砍攻击面，不改变合法输入的语义」——哨兵的单独保留就是这条原则的具体形态。主核里对应的纪律是「错误码要区分『超限』与『非法』」，因为调用方对两者的处理不同。

第三处是「静默归一 + 入口告警」与主核的 **`log_min_messages`** 思路。主核把「同一个问题重复报」交给日志系统去聚合（`log_min_messages` + 相同消息的抑制），而这里在代码层就把它消掉了。两者都承认「重复告警等于没有告警」。

对 AI 工程的实际意义在于：分块是所有 RAG 的第一个环节，而它的错误会一路放大。这份实现给出的三条经验可直接用：**区间要在切分时算，不能事后反查**（重复片段会让 `find` 找错）、**安全上限不能顺手改语义**（保住哨兵）、以及**配置错误要让它炸在启动而不是炸在账单上**（`chunk_token_size < 1` 直接抛）。
