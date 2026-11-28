---
project: lightrag
feature: token-window
title: token 窗口分块
page_type: feature
schema_version: 1
refs:
  - path: lightrag/chunker/token_size.py
    symbols: [chunking_by_token_size, chunking_by_fixed_token]
  - path: lightrag/chunker/token_size.py
    symbols: [_token_windows, _window_step, _make_chunk]
---

# token 窗口分块

## 一句话职责

`"F"` 策略是滑动窗口分块：把文本切成「最多 `chunk_token_size` 个 token、相邻重叠约 `chunk_overlap_token_size` 个 token」的窗口，而每个窗口都是**在字符边界上切的字符切片**，绝不是解码出来的 token 窗口——所以永远不会把一个多字节字符切两半。

## 机制是什么

这个分块器只有 240 行，但它处理了三个容易出错的地方，而每个都对应一类真实失败：

1. **切点必须落在字符边界。** 直觉做法是「把 token 序列切成窗口再 decode 回去」，但那会让一个多字节字符跨越窗口边界时被切成两半——`decode` 会产生替换字符，或者干脆抛错。所以这里反过来：**在字符空间切**，只用 tokenizer 测量长度（`split_by_token_limit` 返回的是字符区间）。
2. **`chunk_overlap >= chunk_token_size` 会让步长 ≤ 0。** 而步长为 0 会让 `range()` 抛 `range() arg 3 must not be zero`，为负则静默产出空序列（整段被丢弃）。两种失败都很糟，所以必须显式拒绝。
3. **`content.strip()` 之后 token 数会变。** 这不是可以忽略的细节：BPE 会把前导空格粘到词上，`" bridging"` 是 1 token 而 `"bridging"` 是 2 token。所以「窗口的 token 数」与「存下来的块的 token 数」可以不同，且**去空白后的可能更多**。

第三条是这一页最有信息量的地方——它让「块的 token 数不超过 `chunk_token_size`」这个看似显然的性质变成**假的**。

## 怎么实现的

**两个入口，同一个算法，差别只在签名。**

模块文档把两者的分工写得很清楚：

> - `chunking_by_token_size` — the **legacy 6-arg signature** used as the default value for `LightRAG.chunking_func`. Kept for backward compatibility so externally-supplied chunking functions can continue to drop in unchanged.
> - `chunking_by_fixed_token` — the same algorithm exposed under the **new file-chunker contract** (standard prefix `(tokenizer, content, chunk_token_size)` plus keyword-only knobs).

所以 `chunking_by_fixed_token` 是一个**纯转发**：

```python
def chunking_by_fixed_token(tokenizer, content, chunk_token_size=1200, *,
                            chunk_overlap_token_size=100,
                            split_by_character=None,
                            split_by_character_only=False,
                            _emit_source_span=False):
    return chunking_by_token_size(
        tokenizer, content,
        split_by_character=split_by_character,
        split_by_character_only=split_by_character_only,
        chunk_overlap_token_size=chunk_overlap_token_size,
        chunk_token_size=chunk_token_size,
        _emit_source_span=_emit_source_span,
    )
```

**关键是参数的顺序与位置**：legacy 版的第三个位置参数是 `split_by_character`（因为是六参数老契约的一部分），而新契约的第三个位置参数是 `chunk_token_size`。两者的位置语义冲突，所以必须两个函数——一个函数无法同时满足两种调用约定。

而新契约的签名与 c01 里 `ChunkerBinding.__call__` 的形态一致（`(tokenizer, content, chunk_token_size, *, ...)`），所以 `invoke_chunker` 能用统一的六参数 + `context=` 分派。

**`_window_step` 只做一件事，但它被单独抽出来的理由是那个错误消息。**

```python
def _window_step(chunk_token_size: int, chunk_overlap_token_size: int) -> int:
    if chunk_overlap_token_size >= chunk_token_size:
        raise ValueError(
            f"chunk_overlap_token_size ({chunk_overlap_token_size}) must be < "
            f"chunk_token_size ({chunk_token_size})"
        )
    return chunk_token_size - chunk_overlap_token_size
```

文档把这处单抽的理由写透了：

> When overlap >= size the stride is <= 0, which makes `range()` yield an empty sequence (dropping the whole segment silently) or raise the opaque `range() arg 3 must not be zero`. Fail closed with the same invariant the API-boundary validators enforce.

三处：

1. **两种失败模式都很糟**：负步长静默产出空结果（数据丢失且无告警），零步长抛一个与业务无关的 `range()` 错误（用户看不出根因）。
2. **`>=` 而不是 `>`** —— 等号也要拒，因为步长为 0。
3. **返回 `chunk_token_size - chunk_overlap_token_size` 这个值本身**，而调用方（`_token_windows`）并不用它——它只用这个函数做校验：

```python
def _token_windows(tokenizer, text, chunk_token_size, chunk_overlap_token_size):
    _window_step(chunk_token_size, chunk_overlap_token_size)
    try:
        spans = tokenizer.split_by_token_limit(text, chunk_token_size, overlap_tokens=chunk_overlap_token_size)
    ...
```

   **`_window_step` 的返回值被丢弃**。所以它实际上是一个「带返回值的断言」。这是误导性的写法（读者会以为步长被使用），但它让「校验」与「步长计算」共用一处定义。

   实际步长由 `split_by_token_limit` 内部决定——因为重叠是「尽力而为」的，字符空间里没有固定步长（见下）。

**`_token_windows` 把 `TokenBudgetError` 翻译成带预览的领域异常。**

```python
try:
    spans = tokenizer.split_by_token_limit(text, chunk_token_size, overlap_tokens=chunk_overlap_token_size)
except TokenBudgetError as exc:
    raise ChunkTokenLimitExceededError(
        chunk_tokens=exc.code_point_token_count,
        chunk_token_limit=chunk_token_size,
        chunk_preview=exc.preview,
    ) from exc
return [(span.start, span.end, span.token_count) for span in spans]
```

三处：

1. **「连一个字符都放不下」** —— 注释说明这「Unreachable through configuration, which requires `MIN_CHUNK_TOKEN_SIZE`」，但直接调用可以传一个比单字符 token 数还小的值。所以这是一处**只有绕过配置入口才会到达**的分支。

2. **翻译成 `ChunkTokenLimitExceededError`**，带上三个字段：实际 token 数、上限、内容预览。这是把「底层 tokenizer 的预算错」变成「用户能理解的分块错」。

3. **`[(span.start, span.end, span.token_count) for span in spans]`** —— 只取三个字段，丢掉 `TokenSpan` 的其他内容。所以三元的 `(start, end, token_count)` 是本模块的内部契约。

**`_token_windows` 文档里关于重叠精度的两句话必须记住。**

> Windows cover `text` without gaps, overlap by about `chunk_overlap_token_size` tokens and end at character boundaries. The overlap is **exact** on `TiktokenTokenizer`'s offset path and a **best-effort estimate** on the generic path (see `Tokenizer.split_by_token_limit`). `token_count` is the count of the window's own text.

**这是「同一接口两种精度」的实例**：tiktoken 有 token→字符的偏移映射，所以重叠可以精确；通用 tokenizer 没有，只能按「全局字符/token 比率」估算并逐步回退。而 `split_by_token_limit` 的文档进一步说明了为什么无法精确：

> `overlap_tokens` is a best-effort target, not a guarantee of the mathematically closest boundary — BPE token count is not monotonic in character length, so this generic implementation only guarantees a safe, progress-making boundary near the target, retreating the target itself (by the same exponential steps) whenever the estimated overlap would fail to make progress.

**「BPE token 数在字符长度上不单调」** —— 这是根因。加一个字符可能让 token 数不变、+1、甚至变少（因为合并规则改变）。所以「精确的重叠 N 个 token」在字符空间里不可解，只能保证「安全且前进」。

而 `split_by_token_limit` 里那行 `chars_per_token = len(content) / total_tokens`（一次全局估算，不持久化到 `self`）是这条估算的具体实现。

**`_make_chunk` 是本页最值得细读的函数——它让「token 数」有歧义，而文档坦白了这一点。**

```python
def _make_chunk(*, tokenizer, content, tokens, order, source_span, emit_source_span):
    """Build one chunk dict whose ``tokens`` counts the stored content.

    ``tokens`` is the count of ``content`` as given; the chunk stores
    ``content.strip()``, so a stripped chunk is counted again. The recount
    may exceed the window's count, and so ``chunk_token_size``, by a token
    or two: BPE attaches a space to the word after it, and a word whose
    leading space is stripped can take more tokens on its own (cl100k:
    ``" bridging"`` is one token, ``"bridging"`` two). The figure reports
    the stored text; the hard limit before embedding recounts on its own.
    """
    stripped = content.strip()
    if stripped != content:
        tokens = len(tokenizer.encode(stripped))
    item: dict[str, Any] = {"tokens": tokens, "content": stripped, "chunk_order_index": order}
    if emit_source_span and source_span is not None:
        item["_source_span"] = source_span
    return item
```

五处：

1. **只在「确实去掉了空白」时才重算**（`if stripped != content`）。没有首尾空白时沿用窗口给的计数——省一次 encode。这是一个正确的优化：分块是热路径，每块一次 encode 在大库上是显著的。

2. **重算的结果可能**更大**。文档给了具体例子：`cl100k` 里 `" bridging"` 是 1 token，而 `"bridging"` 是 2 token。所以**去掉一个空格反而多一个 token**。

   这个反直觉事实的根源是 BPE：`" bridging"` 是词表里的一个合并 token，而 `"bridging"` 需要拆成两个（`" brid"` 不存在，`"brid"`+`"ging"`）。

3. **所以「块的 `tokens` ≤ `chunk_token_size`」是假的**。窗口保证了原始切片的 token 数 ≤ 上限，但 `strip` 后可能 +1 或 +2。这是一个**契约层面的诚实**：文档直说「The recount may exceed the window's count, and so `chunk_token_size`, by a token or two」。

4. **「The figure reports the stored text; the hard limit before embedding recounts on its own」** —— 即 `tokens` 字段描述的是**存下来的内容**，而嵌入前的硬性上限会独立重算。所以下游不能假设 `tokens <= chunk_token_size`。

   这是「两处计数各自负责」的分工：分块器报真实值（哪怕超），限流器自己数（不相信上报值）。

5. **`emit_source_span and source_span is not None`** —— 两个条件都要满足。所以 `_emit_source_span=False` 时完全不产出区间（兼容旧契约），而区间无效（全空白）时也不产出。

**`_trimmed_span` 与 `_source_span` 是两层：前者裁空白，后者还判空。**

```python
def _trimmed_span(content: str, start: int, end: int) -> tuple[int, int]:
    start = max(0, min(start, len(content)))
    end = max(start, min(end, len(content)))
    while start < end and content[start].isspace():
        start += 1
    while end > start and content[end - 1].isspace():
        end -= 1
    return start, end

def _source_span(content: str, start: int, end: int) -> dict[str, int] | None:
    start, end = _trimmed_span(content, start, end)
    if start >= end:
        return None
    return {"start": start, "end": end}
```

四处：

1. **`max(0, min(start, len(content)))` 是双向夹紧** —— 所以即使调用方传了越界值也不会 `IndexError`。`end` 的夹紧用 `max(start, ...)` 保证 `end >= start`。
2. **两个 while 循环对 `content` 而非切片做判断** —— 所以它算的是「在原文里的位置」，直接可用。
3. **`_source_span` 判 `start >= end` 返回 `None`** —— 全空白窗口没有有效区间。
4. **`_make_chunk` 的 `content` 已经是 `_token_windows` 切出的切片**，而 `_source_span` 又对原文重新裁一遍空白。所以**空白被裁了两次**（`_make_chunk` 的 `strip` 与 `_trimmed_span` 的循环）。

   两处必须一致，否则「块的 `content`」与「`_source_span` 覆盖的原文」会对不上。它们确实一致（都按 `.isspace()` 从两端裁），但这是一处**隐式耦合**：改一处的裁剪规则而忘了另一处，区间就错位。

**主循环的两个分支：有 `split_by_character` 与没有。**

没有分隔符时：

```python
for start, end, count in _token_windows(tokenizer, content, chunk_token_size, chunk_overlap_token_size):
    pieces.append((count, content[start:end], start))
```

**`pieces` 的元素是 `(token_count, text, start offset in content)`**，注释点明了不变量：

> (token_count, text, start offset in content): the text is always the slice `content[start : start + len(text)]`.

即三者一致，所以最后算区间时只需 `_source_span(content, start, start + len(text))`——不需要重新查找。

有分隔符时：

```python
cursor = 0
for segment in content.split(split_by_character):
    segment_start = cursor
    cursor += len(segment) + len(split_by_character)
    segment_tokens = len(tokenizer.encode(segment))
    if segment_tokens <= chunk_token_size:
        pieces.append((segment_tokens, segment, segment_start))
        continue
    if split_by_character_only:
        logger.warning("Chunk split_by_character exceeds token limit: len=%d limit=%d", segment_tokens, chunk_token_size)
        raise ChunkTokenLimitExceededError(chunk_tokens=segment_tokens, chunk_token_limit=chunk_token_size, chunk_preview=segment[:120])
    for start, end, count in _token_windows(tokenizer, segment, chunk_token_size, chunk_overlap_token_size):
        pieces.append((count, segment[start:end], segment_start + start))
```

五处：

1. **`cursor` 累加 `len(segment) + len(split_by_character)`** —— 这是 `str.split` 的位置反推。所以它假设分隔符**每次都是同一个字符串**且长度固定。

   **这是一个真实的 bug 面**：`split_by_character` 可以是多字符串（如 `"\n\n"`），长度固定所以加法对。但如果分隔符在原文里以不同形式出现（例如 `split_by_character="\n"` 而原文是 `"\r\n"`），`split` 后重新加起来的位置就会**偏移**。

   更根本的问题：`content.split(sep)` 会丢弃分隔符，所以「用 `len(segment) + len(sep)` 走一遍」只对「分隔符恰好出现 N-1 次且都是完整匹配」成立。这对 `str.split` 是正确的（它就是这么切的），所以加法对——只要 `split_by_character` 非空且没有重叠。

   但**若 `split_by_character` 是空串**，Python 的 `str.split("")` 会抛 `ValueError: empty separator`。所以这个分支实际上要求非空（`if split_by_character:` 已挡住空串，因为空串是假值）。

2. **短段直接成块**（`segment_tokens <= chunk_token_size`），不再切。所以 `split_by_character` 是一个「硬分段」：每段至少是一个块。

3. **`split_by_character_only=True` 时超长段直接抛 `ChunkTokenLimitExceededError`**（先 `logger.warning` 再抛）。所以「只按字符切、不按 token 切」是一个**严格模式**：它要求用户保证每段都不超。

   为什么 warning 与 raise 一起？因为异常向上冒泡时可能被上游捕获并降级处理，而 warning 保证「即使被降级也有日志」。

4. **`chunk_preview=segment[:120]` 截前 120 字符**。这是一个固定的魔数，没有走常量——与 `recursive_character.py` 里的 `[:100]` 又不一致。

5. **`segment_start + start`** —— 段内偏移加上段在全文的起点。所以区间仍是全文坐标。

**注意这里没有对「段之间」的空白做任何处理**：分隔符被丢弃了，所以相邻块之间少了分隔符——而 `split_by_character` 往往就是 `"\n"` 这类空白符。所以「块内容拼起来」不等于原文。

**最后的列表推导把区间计算与 `_make_chunk` 合在一起。**

```python
return [
    _make_chunk(
        tokenizer=tokenizer,
        content=text,
        tokens=count,
        order=index,
        source_span=(_source_span(content, start, start + len(text)) if _emit_source_span else None),
        emit_source_span=_emit_source_span,
    )
    for index, (count, text, start) in enumerate(pieces)
]
```

三处：

1. **`_emit_source_span` 被传了两次**：一次用于「要不要算」（三元表达式），一次用于「要不要写」（`_make_chunk` 内的判断）。**第一个是多余的**——因为 `_make_chunk` 内部已经判了。所以 `_emit_source_span=False` 时白算了一次 `_source_span`（虽然便宜，但要遍历空白）。

   这是一处明确的冗余，且它的存在说明「`_emit_source_span` 的语义在这两层没对齐」。

2. **`order=index`** —— 顺序索引来自 `enumerate`，所以连续从 0 开始。这与 c02 的 `len(results)` 等价。

3. **`start + len(text)`** —— `text` 是切出来的片段，所以它的长度就是区间长度。依赖前面那个「不变量」。

**关于 `_emit_source_span` 这个私有参数（下划线前缀），它说明区间能力是后加的。**

两个公开入口都把它写成 `_emit_source_span: bool = False`，且它是 keyword-only。所以：
- 缺省不产出 `_source_span`（保持旧契约，下游不用改）
- 需要时显式打 true（内部调度器这么做）

下划线前缀表示「不是给终端用户的 API」。但它在两个 public 函数的签名里，所以它是一个**内部约定而非强封装**——这也是为什么 c01 里 `ChunkerBinding` 要按 `accepts_context` 显式分派，而不能靠签名探测：这些内部参数的存在让签名探测更不可靠。

## 关键取舍

**源码事实：** `"F"` 策略在**字符空间**切窗口，只用 tokenizer 测量长度，所以永不在多字节字符中间切开；窗口由 `tokenizer.split_by_token_limit` 产出，返回 `(start, end, token_count)` 字符区间。

**源码事实：** 两个入口同算法不同签名：`chunking_by_token_size`（legacy 六参数，第三个位置参数是 `split_by_character`）与 `chunking_by_fixed_token`（新契约，第三个位置参数是 `chunk_token_size`）。**位置语义冲突**是必须两个函数的根本原因，后者是纯转发。

**源码事实：** `_window_step` 校验 `overlap >= size` 并用 `range()` 的两种失败模式作为理由（负步长静默丢数据、零步长抛 `range() arg 3 must not be zero`）；它的返回值被 `_token_windows` **丢弃**，实际只作「带返回值的断言」用——真实步长由 `split_by_token_limit` 决定（因为字符空间里没有固定步长）。

**源码事实：** 重叠精度分两档：tiktoken 有偏移映射所以精确，通用 tokenizer 只能按「全局字符/token 比率」估算——根因是**BPE token 数在字符长度上不单调**，所以「精确重叠 N token」在字符空间不可解。

**源码事实：** `_token_windows` 把 `TokenBudgetError`（连一个字符都放不下）翻译成 `ChunkTokenLimitExceededError`，后者带实际 token 数、上限、内容预览；该分支「通过配置不可达」（配置要求 `MIN_CHUNK_TOKEN_SIZE`），只有直接调用能触发。

**源码事实：** `_make_chunk` 只在「`strip` 确实改变了内容」时**重算 token 数**，而重算结果**可能更大**：`cl100k` 里 `" bridging"` 是 1 token、`"bridging"` 是 2 token（BPE 把前导空格粘进了词）。所以**「块的 `tokens` ≤ `chunk_token_size`」不成立**，文档明确承认「may exceed ... by a token or two」，并说明「The figure reports the stored text; the hard limit before embedding recounts on its own」。

**源码事实：** `_trimmed_span` 对 `start`/`end` 做双向夹紧（越界不抛错），并按 `isspace()` 从两端裁；`_source_span` 再判 `start >= end` 返回 `None`。空白被裁两次（`_make_chunk` 的 `strip` 与 `_trimmed_span` 的循环），两处规则必须一致——是一处隐式耦合。

**源码事实：** `pieces` 的不变量是「`text` 恒等于 `content[start : start + len(text)]`」，所以最后算区间不需要重新查找。

**源码事实：** `split_by_character` 分支用 `cursor += len(segment) + len(split_by_character)` 反推段位置（依赖 `str.split` 的语义与分隔符长度固定）；短段直接成块不再切（所以每段至少是一个块）；`split_by_character_only=True` 时超长段**先 `logger.warning` 再抛** `ChunkTokenLimitExceededError`（预览截 120 字符，与 c02 的 100 不一致）；段间分隔符被丢弃，所以块拼起来不等于原文。

**源码事实：** 空 `split_by_character` 被 `if split_by_character:` 挡住（它是假值），也就避开了 `str.split("")` 的 `ValueError`。

**源码事实：** 列表推导里 `_emit_source_span` 被传两次（三元表达式 + `_make_chunk` 内部），第一个是多余的，导致关闭时白算一次 `_source_span`。

**源码事实：** `_emit_source_span` 是下划线前缀的私有参数但出现在两个公开函数签名里——所以它是内部约定而非强封装。

**架构推论：** 「在字符空间切、只用 tokenizer 测量」这个选择把「多字节字符不被切开」变成**结构性保证**而不是检查项。若反过来做（切 token 再 decode），就必须每一处都验证「拼回去等于原文」，而 BPE 的 decode 不是 encode 的逆（有损字符会出现）。所以这里的判据是：**切分单位必须与「不可分割的最小单元」对齐**——字符是不可分割的（对文本语义），token 不是。

**架构推论：** `_make_chunk` 那处「token 数可能超上限」的坦白，反映的是**分块器的输出契约只能是近似的**。因为它要做两件彼此不兼容的事：按 token 容量切（窗口），又按字符语义裁（`strip`）。BPE 让这两件事的单位换算出错。框架的应对是把「硬上限」交给另一个组件（嵌入前的限流器），而让分块器只报「我实际存了什么」。**这是「不变量由最靠近它的组件负责」的应用**：分块器无法保证，就不假装保证。

**架构推论：** `overlap >= size` 的校验放在 `_window_step` 而负面重叠校验放在 `chunking_by_token_size` 的**最前面**，这个不对称是有理由的，注释写得很细：

> Checked up front, before any branching on `split_by_character`, so a negative `chunk_overlap_token_size` is always rejected -- including on the `split_by_character` path where every segment stays under `chunk_token_size` and `_window_step()`'s own (upper-bound only) check would otherwise never be reached. The upper-bound check stays lazy, inside `_window_step()`, since it's only meaningful once windowing is actually about to happen.

即：**负值在所有路径上都是错**（所以最前面查），而**上界只在真要切窗口时才错**（所以懒查）。这是一条很好的判据：**校验的位置应该由「这个错是否与路径无关」决定**。

**架构推论：** 两个入口的存在暴露了一个常见的 API 演进困境：位置参数的语义无法演进。`chunking_by_token_size(tokenizer, content, split_by_character, ...)` 与 `chunking_by_fixed_token(tokenizer, content, chunk_token_size, ...)` 都是「第三个位置参数」，但含义完全不同。若只保留一个函数并加 `*` 强制关键字，老调用方会全部坏掉。所以「新契约 = 新函数名」是唯一安全的路——代价是同一个算法有两份文档、两处需要保持同步。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/chunker/token_size.py` | `_trimmed_span` | 按 `isspace()` 裁空白并双向夹紧区间 |
| `lightrag/chunker/token_size.py` | `_source_span` | 裁后判空，无效返回 `None` |
| `lightrag/chunker/token_size.py` | `_make_chunk` | 组块；`strip` 后重算 token 数（可能变大） |
| `lightrag/chunker/token_size.py` | `_window_step` | 校验 `overlap < size`，返回值被丢弃 |
| `lightrag/chunker/token_size.py` | `_token_windows` | 调 `split_by_token_limit` 并翻译 `TokenBudgetError` |
| `lightrag/chunker/token_size.py` | `chunking_by_token_size` | legacy 六参数入口（负面重叠在最前校验） |
| `lightrag/chunker/token_size.py` | `chunking_by_fixed_token` | 新文件分块器契约的纯转发入口 |
| `lightrag/chunker/token_size.py` | `_emit_source_span` | 私有开关，控制是否产出 `_source_span` |
| `lightrag/utils.py` | `Tokenizer.split_by_token_limit` | 字符空间的窗口切分（tiktoken 精确 / 通用估算） |

## 参考

- [lightrag 源码：chunker/token_size.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/chunker/token_size.py)
- [tiktoken 仓库：BPE 与偏移映射](https://github.com/openai/tiktoken)
- [OpenAI 文档：tokenizer 与 token 计数](https://platform.openai.com/docs/advanced-usage/tokenizers)
- [Python 文档：str.split 与 str.strip](https://docs.python.org/3/library/stdtypes.html#str.split)

## 与主核 / AI 的关系

「按容量切分但切点必须对齐语义边界」与主核（多模态数据库内核）的**页内元组布局**是同一类约束。主核的页有固定大小（8KB），而一个元组不能跨页（除非 TOAST 或元组过大走溢出链）。所以「容量上限」与「不可分割单元」的冲突在主核里也存在，解法同样是**让不可分割单元优先，让容量成为软目标**——主核允许元组跨页（通过 continuation），正如这里允许块超一两个 token。

**但主核有一个这里没有的能力：它可以事后验证。** 主核写入元组后能读回来比对（校验和不匹配就报错），而这里的「块是否完整」无法验证——因为没有「原文的正确切法」这个参照物。所以这里的正确性只能靠**结构性保证**（在字符空间切）而不是事后检查。

第二处对照是 `chunk_overlap >= chunk_token_size` 与主核的**`fillfactor`**。两者都是「布局参数不合理导致退化」：`fillfactor=100` 会让页没有更新空间，重叠 ≥ 块大小会让窗口不前进。主核的做法是**允许设置但警告**，这里的做法是**直接拒绝**。差别在于后果可见性：`fillfactor` 设错的后果是「更新变慢」（性能问题，可观察），而重叠设错的后果是「整段被静默丢弃」（数据丢失，不可观察）。**判据是失败的可见性**——不可观察的失败必须提前拦。

第三处是「BPE token 数不单调」这件事，它与主核的**变长编码**（varlena）有相通之处：一个值的编码长度不随「看起来应该更短」的变化而单调变化。主核里 `varchar` 截断到 N 字符后字节数可能变多（多字节字符被截断到半字符时不会，但换编码会）。所以「按 A 维度限制，按 B 维度测量」这件事在所有变长编码系统里都不可靠。

对 AI 工程的实际意义在于：token 窗口分块是绝大多数 RAG 的默认策略，而它的坑几乎全在「token 与字符的换算」上。这份实现给出的三条经验可直接用：**切点对齐字符边界而不是 token 边界**（否则多字节字符会坏）、**重叠必须严格小于块大小**（否则静默丢数据）、以及**`strip` 会改变 token 数**（所以不要假设块 token 数不超过上限）。
