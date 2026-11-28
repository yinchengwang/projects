---
project: lightrag
feature: semantic-chunking
title: 语义分块
page_type: feature
schema_version: 1
refs:
  - path: lightrag/chunker/semantic_vector.py
    symbols: [chunking_by_semantic_vector, _semantic_groups_with_spans]
  - path: lightrag/chunker/paragraph_semantic.py
    symbols: [chunking_by_paragraph_semantic, _block_to_paragraphs]
  - path: lightrag/chunker/paragraph_semantic.py
    symbols: [_normalize_references_tail_n, _apply_part_suffixes]
---

# 语义分块

## 一句话职责

两种「语义」策略：`"V"` 用句子嵌入的距离突变找边界（语义向量分块），`"P"` 用文档结构（标题层级、表格行、参考文献尾部）找边界（段落语义分块）。前者把 LangChain 的 `SemanticChunker` 镜像了一遍只为拿回原文区间，后者是仓内最大的分块文件——因为它要处理真实文档的标题树、表格切片与参考文献。

## 机制是什么

两者都被同一个问题卡住：**上游给出的「语义边界」不带位置信息**。

- `SemanticChunker.split_text` 返回的是 `" ".join(sentences)` 拼接出来的字符串——**不是原文切片**，所以位置全丢。
- `blocks.jsonl` 给的是「一行一个 content block」，但一个 block 里的段落、表格、公式需要再切，而切完的碎片要能追回原 block（用于文档删除时清缓存）。

所以两者的第一件事都是**重建位置对应**，只是方式不同：`"V"` 靠 `text.find(sentence, cursor)` 逐句定位，`"P"` 靠「一行一个 block，块内按 `\n` 拆」这个 sidecar 约定。

第二个共同问题是**都要兜底到 `"R"`**。`"V"` 的语义完全依赖嵌入函数，没有嵌入函数时它没有任何优势；`"P"` 依赖 sidecar，没有 sidecar 时它无从下手。两者的兜底都写明「宁可退化也不要静默丢文档」，但退化时都保留自己的语义特点——`"V"` 传 `chunk_overlap_token_size=0`（语义边界本就不该重叠）。

第三个是 `"P"` 独有的**阶段化**。它的处理是四段流水线，每段只解决一件事：

| 阶段 | 做什么 | 输出不变量 |
|---|---|---|
| TableRowSplit | 超大的表格按行重切，重复表头 | 表格切片不超 `table_max` |
| AnchorSplit | 超长的普通块按段落重切 | 每块不超 `target_max` |
| HeadingGlue | 没有正文的标题块粘到**更深**的子块上 | 标题不孤立 |
| LevelMerge | 自底向上合并过小的兄弟块 | 块大小接近 `target_ideal` |

顺序不能乱，而 `HeadingGlue` 必须在 `LevelMerge` 之前——注释说明了理由：否则一个 bare heading（如 `## 2.4` 后面紧跟 `### 2.4.1`）会被 `LevelMerge` 粘到**同级邻居**（如 `## 2.3`）上，从而错挂到无关主题。

## 怎么实现的

### `"V"`：语义向量分块

**`_semantic_groups_with_spans` 是一个带警告的镜像实现。**

```python
def _semantic_groups_with_spans(splitter, text):
    """Mirror SemanticChunker grouping while keeping original source spans.

    .. warning::
        This re-implements the body of ``SemanticChunker.split_text`` so each group
        carries its exact source span (``text[start:end]``) instead of the upstream
        ``" ".join(sentences)`` reflow. It relies on **private** members
        (``sentence_split_regex``, ``breakpoint_threshold_type``, ``min_chunk_size``,
        ``number_of_chunks``, ``_calculate_sentence_distances``,
        ``_threshold_from_clusters``, ``_calculate_breakpoint_threshold``). Verified
        byte-for-byte against ``langchain-experimental`` 0.3.2–0.4.x ... If that pin
        is widened, re-verify against the new upstream ``split_text`` —
        ``tests/chunker/test_chunker_semantic_vector.py`` has a drift guard that
        compares this mirror's grouping to the live ``splitter.split_text`` output.
    """
```

四处：

1. **依赖七个上游私有成员**（四个属性 + 三个方法）。所以这是一个**与 LangChain 内部结构强耦合**的实现——和 c02 读 `RecursiveCharacterTextSplitter` 的六个私有属性是同一手法。

2. **警告里给了版本范围**（`0.3.2–0.4.x`）与依据（`pyproject.toml` 的 pin）。所以「什么情况下会坏」是可判定的：放宽 pin 就要重新验证。

3. **它有一个 drift guard 测试**——把镜像的分组与真 `splitter.split_text` 的输出对比。**这是这处耦合唯一可接受的形态**：耦合 + 自动检测漂移。若没有这个测试，一次上游升级会让分块行为静默改变。

4. **镜像的目的是去掉 `" ".join(sentences)` 的重排**。上游把句子用空格拼起来，于是「原文里是换行的地方」变成空格，位置也无从恢复。这里改成「按首尾句的 span 取原文切片」，所以位置精确、格式保持。

**句子定位靠 `_sentence_spans`，而它有三层降级。**

```python
def _sentence_spans(text, sentences):
    spans = []
    cursor = 0
    for sentence in sentences:
        if not sentence:
            spans.append((cursor, cursor))
            continue
        start = text.find(sentence, cursor)
        if start < 0:
            start = text.find(sentence)
        if start < 0:
            start = cursor
        end = start + len(sentence)
        spans.append((start, end))
        cursor = end
    return spans
```

五处：

1. **`find(sentence, cursor)` 从上一个结束处往后找** —— 这解决了「同一句出现两次」的歧义（总是找下一处）。这正是 c02 里「文本查找有歧义」那个理由的**正确解法**：带游标的顺序查找，而不是全局 `find`。

2. **找不到时退回 `text.find(sentence)`（从 0 开始）** —— 说明句子列表与原文不一致（上游正则可能改写了文本）。

3. **再找不到就用 `cursor`** —— 给出一个零宽区间。所以最坏情况下位置是错的，但**不会崩**。

4. **`if not sentence` 给零宽区间** —— 正则切分可能产出空串（如分组边界），它们不占位置。

5. **`cursor = end`** —— 单调推进。所以「第一层降级失败」（从 0 找到更早的位置）会让 `cursor` **后退**，于是后续句子可能重复映射。这是一处真实的退化路径，但没有更好的选择。

**分组逻辑与上游一致，但有两处提前返回与一处「跳过但不推进」的细节。**

```python
if len(single_sentences_list) == 1:
    group = _group(0, 0)
    return [group] if group else []
if splitter.breakpoint_threshold_type == "gradient" and len(single_sentences_list) == 2:
    return [g for i in range(2) if (g := _group(i, i)) is not None]
```

两处：

1. **单句直接返回一组** —— 没有距离可算（`_calculate_sentence_distances` 需要一个相邻对）。
2. **`gradient` + 两句时也提前返回** —— 这是上游的一个特例（gradient 阈值在只有两句时无法定义）。注释没解释，但它是镜像的一部分。

然后是主循环里那个**关键的 `continue`**：

```python
for index in indices_above_thresh:
    end_index = index
    group_sentences = sentences[start_index:end_index+1]
    combined_text = " ".join([d["sentence"] for d in group_sentences])
    if splitter.min_chunk_size is not None and len(combined_text) < splitter.min_chunk_size:
        continue
    group = _group(start_index, end_index)
    if group is not None:
        chunks.append(group)
    start_index = index + 1
```

**注意 `start_index = index + 1` 在 `continue` 之后**，所以「因 `min_chunk_size` 而跳过的那段」**不会**被并入下一组——它的内容直接丢失。

**这是一处真实的内容丢失点**：一个短于 `min_chunk_size` 的段被丢弃，而 `start_index` 仍然跳过它。这与上游行为一致（镜像就是这么写的），但它意味着 `min_chunk_size` 不是「最小块」而是「短于此就丢掉」。

而末尾那段兜住了剩下的：

```python
if start_index < len(sentences):
    group = _group(start_index, len(sentences) - 1)
    if group is not None:
        chunks.append(group)
```

所以只有「中间的短段」会被丢，最后一段总是被保留。

**`chunking_by_semantic_vector` 的嵌入适配器与三道预算。**

`_AsyncEmbeddingFuncAdapter` 把 LightRAG 的 `EmbeddingFunc` 包装成 LangChain 的 `Embeddings` 接口（因为 `SemanticChunker` 要求后者）。而它做了三件事：

1. **批量化**（`DEFAULT_EMBEDDING_BATCH_NUM`）。
2. **并发上限**（`embedding_max_async`，非正值直接 `ValueError` 而不是挂住）。
3. **截断**（`_truncate`，按 token 预算）。

第三点对应文档里那段关于「0 视为未设置」的说明：

> The per-window token budget is `embedding_func.max_token_size`, or the effective `chunk_token_size` when that is `None` **or** `0`. Treating `0` as "unset" is a deliberate divergence from `lightrag.llm.openai.openai_embed`, where `max_token_size=0` documents "disable truncation": V needs a budget of its own, and the server path cannot even produce `0` here ... so only a direct `EmbeddingFunc(max_token_size=0)` SDK caller reaches this branch.

**这是同一语义（`0`）在两个模块里含义相反的例子**：在 `openai_embed` 里 `0` = 「不截断」，在这里 `0` = 「用 `chunk_token_size` 兜底」。理由说得很实在——「V 需要自己的预算」，而 `0` 若解释成「不截断」就意味着「句子窗口可以无限长」，那 `SemanticChunker` 的嵌入调用会因超长失败。所以这里选了「更安全但更意外」的语义，并用 `budget_source` 字段把这个分歧记录下来传给日志。

**取消处理是本页最细的一段。**

```python
try:
    pieces = await asyncio.to_thread(_semantic_groups_with_spans, splitter, content)
except asyncio.CancelledError:
    # ``to_thread`` returns on cancellation but the worker thread keeps
    # running.  This both cancels the bounced call it is blocked on (so
    # the loop-side worker pool is torn down instead of running to
    # completion with its results discarded) and makes the refusal
    # sticky, which is what covers the thread not having reached the
    # adapter yet — see ``cancel_inflight``.
    adapter.cancel_inflight()
    raise
```

三处：

1. **`asyncio.to_thread` 在取消时并不停止线程** —— 协程返回了，但工作线程还在跑，还会继续调嵌入函数、烧 token。

2. **`cancel_inflight()` 做两件事**：取消已经在飞的调用（让服务端的 worker pool 被拆掉而不是白跑完），以及**把拒绝变成粘性的**（"makes the refusal sticky"）——因为线程可能还没到适配器，所以「此刻没有在飞的调用」不等于「它不会发起」。

3. **它必须 `raise` 重抛** —— 否则取消被吞掉，上层以为分块成功了。

**这是「异步包装同步调用」的通用陷阱**：`to_thread`/`run_in_executor` 都无法真正中断线程，所以必须给被调用的代码一个「停止」信号。

**分段后的尺寸处理是两跳，而注释解释了为什么要两跳。**

```python
def _size_and_resplit() -> list[dict[str, Any]]:
    """...
    ``await asyncio.to_thread`` above covers the embedding-driven grouping
    and nothing else: this loop encodes each piece and, for anything over
    budget, runs a whole R pass on it. On the event loop that is comparable
    in cost to the grouping it follows, which is why the whole loop — not
    each piece — moves off it in one hop.

    ``chunking_by_recursive_character`` is called DIRECTLY here: this
    function already runs inside the chunking executor, and a nested
    submission into a single-worker pool whose permit it holds would
    deadlock.
    """
    ...
return await run_in_chunking_executor(_size_and_resplit)
```

四处：

1. **「分组」与「重切」分两跳卸载** —— 因为重切的开销与分组相当，所以整段一起卸载，而不是每块卸一次（后者会有 N 次提交开销）。判据是**开销可比则合并卸载**。

2. **`chunking_by_recursive_character` 必须直接调，不能再提交** —— 因为 `run_in_chunking_executor` 背后是单 worker 池 + 信号量，**已经持有许可的线程再申请同一许可会死锁**。

   **这是一处很具体的死锁来源**：任何 `executor_safe` 的分块器内部若调用另一个 `executor_safe` 的分块器（走 `ChunkerBinding.__call__`），就会死锁。这里靠「直接调裸函数」规避。

3. **`SemanticChunker` 无内建大小上限**（文档：「SemanticChunker does NOT enforce a maximum natively」），所以超限的语义段要显式用 `"R"` 重切，否则只能靠嵌入时那次「按 `embedding_token_limit` 而不是 `chunk_token_size`」的硬切——那就不是用户配的尺寸了。

4. **`_source_span` 的偏移换算**：

```python
sub_span = sub.get("_source_span")
source_span = None
if isinstance(sub_span, dict):
    try:
        source_span = {"start": source_start + int(sub_span["start"]),
                       "end": source_start + int(sub_span["end"])}
    except (KeyError, TypeError, ValueError):
        source_span = None
```

   即重切出的子块的区间是**段内偏移加段起点**。而 `except` 捕三种异常后置 `None`——所以一个畸形的 `_source_span` 会让整块失去区间，但块本身保留。

   注意 **c02 里讲过（`recursive_character` 的兜底路径可能完全没有 `_source_span` 键），这里已经处理了**（`isinstance(sub_span, dict)` 为假 → `source_span = None` → 该键不出现）。所以两页的接口在这里是打通的。

**`"V"` 的 `content` 兜底与 `number_of_chunks` 的相互作用**值得一提：`number_of_chunks` 非 `None` 时用 `_threshold_from_clusters`，此时 `breakpoint_array = distances`（直接把距离当断点数组）；否则用 `_calculate_breakpoint_threshold` 返回两个值。所以两条路径的 `breakpoint_array` 来源不同，而后续 `indices_above_thresh` 的判据一致。

### `"P"`：段落语义分块

**`_block_to_paragraphs` 是一句 `split("\n")`，而它成立依赖一个 sidecar 约定。**

```python
def _block_to_paragraphs(content: str) -> list[dict[str, Any]]:
    """Recover the per-paragraph view of a rewritten block.

    The sidecar writer joins paragraphs with ``\\n`` and emits
    tables/equations/drawings as single-line tags with no internal newlines,
    so ``split("\\n")`` faithfully recovers paragraph boundaries.
    """
    paragraphs = []
    for line in content.split("\n"):
        if not line.strip():
            continue
        paragraphs.append({"text": line, "is_table": _is_table_paragraph(line)})
    return paragraphs
```

三处：

1. **「无损」的论证是：表格/公式/图形被渲染成单行标签**（`<table ...>...</table>`、`<equation>`），内部没有换行。所以按 `\n` 拆不会把表格拆坏。

2. **`_is_table_paragraph` 判 `startswith("<table ")` 且 `endswith("</table>")`** —— 注意 `<table ` 带空格（因为标签带属性如 `format="json"`）。

3. **空行被跳过**（`if not line.strip(): continue`），所以段落数 ≤ 行数。

**这是一处脆弱的约定**：若将来 sidecar 改成「表格多行渲染」，这个 `split("\n")` 就会把表格拆成几段，而 `_is_table_paragraph` 会对每一段都返回假（因为它们既不以 `<table ` 开头也不以 `</table>` 结尾）。**整个 TableRowSplit 阶段会静默失效**。

**`_normalize_references_tail_n` 与两个 env 读取函数体现了「运行期读 vs 快照」的分界。**

```python
def _references_tail_n() -> int:
    """Content-block window for reference detection (live from env).
    Read at chunk time (NOT snapshotted) so changing
    ``CHUNK_P_REFERENCES_TAIL_N`` takes effect on re-runs of already-enqueued
    documents.  ``0`` scans all content blocks; a positive value restricts
    detection to the last N blocks. Invalid or negative values fall back to
    DEFAULT_P_REFERENCES_TAIL_N.
    """
    raw = os.getenv("CHUNK_P_REFERENCES_TAIL_N")
    if raw is None:
        return DEFAULT_P_REFERENCES_TAIL_N
    return _normalize_references_tail_n(raw, setting="CHUNK_P_REFERENCES_TAIL_N")
```

四处：

1. **「运行期读，不快照」** —— 这是与 `drop_references` 的刻意对比。文档在 `drop_references` 那里说它是「the only reference knob that is snapshotted into `chunk_options`」并记进 `doc_status.metadata['chunk_opts']`。

   **所以三个相关旋钮里只有一个快照**：`drop_references`（因为它可能来自 per-file hint，必须与那次处理绑定），而 `tail_n` 与 `headings` 是「全库策略」（改了应该对重跑生效）。

   这是一个很实用的区分：**与文档绑定的参数要快照，与策略绑定的参数不要**。

2. **`_normalize_references_tail_n` 三态**：可转 `int` 且非负 → 用；不可转 → warning + 默认；负数 → warning + 默认。所以畸形的 env 不会让分块崩。

3. **`0` 的语义是「全扫」**（不是「不扫」）。所以 `0` 与「禁用」相反——禁用要用别的方式（`drop_references=False`）。

4. **`raw is None` 与 `raw == ""` 不同**：前者用默认，后者走 `int("")` → `ValueError` → warning + 默认。所以空字符串会多一条 warning。

**`_is_reference_heading` 的 ASCII/非 ASCII 双规则是本页最细的语言学判断。**

```python
for prefix in prefixes:
    pl = pref.casefold()
    if not low.startswith(pl):
        continue
    if pref.isascii():
        rest = low[len(pl):]
        if rest and rest[0].isalnum():
            continue  # e.g. "Referenced" — not a standalone word
    return True
```

四处：

1. **ASCII 前缀要求词边界**（`rest[0].isalnum()` 为假才匹配）。所以 `"References"` 匹配，而 `"Referenced work"` **不**匹配——因为 `"Referenced"` 是另一个词。

2. **非 ASCII 前缀按纯前缀匹配**。注释：「CJK has no word boundary, so ``参考文献列表`` still matches」。

   **这是「同一规则在两种书写系统里需要两种实现」的实例**：`isascii()` 是一个廉价的语言族判据，而它恰好对应「有词边界 / 无词边界」这个真实的语言差异。

3. **`casefold()` 而不是 `lower()`** —— 对德语 `ß` 这类字符，`casefold` 才能正确比对。

4. **`pref.isascii()` 判的是前缀而非待匹配文本** —— 这是对的，因为规则取决于「这个前缀属于哪种书写系统」。

**`_apply_part_suffixes` 给被切开的块加 `[part n]`，而它有三层保护。**

```python
def _apply_part_suffixes(blocks):
    """Tag split fragments from one original block as ``[part n]``."""
    if len(blocks) <= 1:
        return blocks
    for idx, block in enumerate(blocks, start=1):
        block["heading"] = _append_part_suffix(block.get("heading", ""), idx)
    return blocks
```

三处：

1. **只有被切成多块时才加后缀**（`len(blocks) <= 1` 直接返回）。所以未切的块保持原标题——避免给每个块都挂一个 `[part 1]`。

2. **`_append_part_suffix` 先剥旧后缀**：

```python
def _append_part_suffix(heading: str, part_number: int) -> str:
    base = _strip_generated_heading_suffixes(heading)
    suffix = f"[part {part_number}]"
    return f"{base} {suffix}" if base else suffix
```

   而 `_strip_generated_heading_suffixes` 是个**循环剥到不动点**的实现：

```python
cleaned = (heading or "").rstrip()
while True:
    next_cleaned = _PART_SUFFIX_RE.sub("", cleaned).rstrip()
    next_cleaned = _LEGACY_TABLE_CHUNK_SUFFIX_RE.sub("", next_cleaned).rstrip()
    if next_cleaned == cleaned:
        return cleaned
    cleaned = next_cleaned
```

   **为什么要循环？** 因为历史上有两种后缀（新的 `[part n]` 与旧的表格分块后缀），而一份存档可能被处理过多次，于是叠了多层。循环剥到不动点保证无论叠了几层都剥干。这是一个「不知道数据有多脏时的稳妥写法」。

3. **`f"{base} {suffix}" if base else suffix`** —— 空标题只留后缀，不留前导空格。

   所以后缀落在 `heading["heading"]` 上而不是 `parent_headings`（文档明确：「``parent_headings`` are untouched」）。这是对的：`parent_headings` 参与层级比较，若带上 `[part n]` 会让 `_same_parent_path` 判不相等。

**`_same_parent_path` 与 `_is_descendant` 是 LevelMerge 的两道防「跨主题污染」闸。**

```python
def _same_parent_path(a, b) -> bool:
    return list(a.get("parent_headings") or []) == list(b.get("parent_headings") or [])

def _is_descendant(shallow, deep) -> bool:
    head = _strip_generated_heading_suffixes(shallow.get("heading") or "")
    shallow_full = list(shallow.get("parent_headings") or []) + ([head] if head else [])
    deep_parents = list(deep.get("parent_headings") or [])
    return deep_parents[: len(shallow_full)] == shallow_full
```

四处：

1. **`_same_parent_path` 用于同级合并**（Phase A 尾部吸收）。所以「同在 level 3 但父链不同」的两个块（如 `2.4.1` 与 `2.5.1`）**不合并**。注释说这是「the documented anti-cross-topic-pollution guarantee」。

2. **`_is_descendant` 用于跨级吸收**（Phase B）。判据是「浅块的完整标题路径是深块父链的前缀」。

3. **`_strip_generated_heading_suffixes` 在比较前被调用** —— 因为 `parent_headings` 从不带 `[part n]`，而 `heading` 可能带。所以必须剥掉才能比较。

   **这是一处必须记得的对称性**：`_apply_part_suffixes` 只改 `heading`，而 `_is_descendant` 把 `heading` 拼进路径——两者必须配合，否则层级判断全错。

4. **「无标题的块」（preamble）路径为空，前缀任何东西** —— 所以它不受限制地可以吸收。注释说：「they have no hierarchy to violate」。这是一个「宽松」的选择，但合理（前言确实没有层级）。

**`_can_merge_forward` / `_can_merge_backward` 只允许 `role == "none"`。**

```python
def _can_merge_forward(role: str) -> bool:
    # A split-table slice (first/middle/last) is frozen against LevelMerge: its
    # repeating header is already injected into the slice at TableRowSplit time,
    # so re-merging two slices of one table would duplicate the header mid-body.
    return role == "none"
```

**理由很具体**：表格切片在 `TableRowSplit` 阶段已经被注入了重复表头，若两块切片再合并，表头会出现在正文中间。

这是一处「先做的处理让后做的处理不安全」的例子——`TableRowSplit` 为了「每片可独立理解」而注入表头，代价是这些片**永久不可再合并**。所以 `role` 这个字段的作用是**冻结标记**。

**参考文献丢弃的三重条件。**

```python
if row.get("session_type") == "references" or (
    idx >= start and _is_reference_heading(row.get("heading", "") or "", prefixes)
):
    dropped_headings.append((row.get("heading") or "").strip())
else:
    kept.append(row)
```

三处：

1. **`session_type == "references"` 时无条件丢**（不看窗口）——因为这是解析器给的明确标记（Word 的参考文献控件），比标题匹配可靠。

2. **否则要同时满足「在尾部窗口内」与「标题匹配前缀」**。窗口由 `start = 0 if tail_n == 0 else max(0, len(rows) - tail_n)` 算出。

3. **`start = 0` 表示全扫**（`tail_n == 0`）。

然后是那个**保命判断**：

```python
if dropped_headings and any((row.get("content") or "").strip() for row in kept):
    logger.info(...)
    rows = kept
elif dropped_headings:
    logger.warning("[paragraph_semantic] dropping reference block(s) %s would leave no content; keeping them ...")
```

**「若丢掉参考文献后一个字都不剩，就不丢」** —— 而且注释说明了判据为什么基于「还有内容的行」：

> Protect against an empty document: base the guard on rows that still carry content (the loop below skips blank-content rows), so a pathological sidecar whose only kept rows are empty does not silently produce zero chunks.

即：`kept` 非空不代表有内容（可能是空行），所以要 `any(...strip())`。这是一个**精确的「空文档」判据**。

**阶段化流水线的主循环，以及 `[part n]` 的施加时机。**

```python
for blk in initial:
    block_after_b = _expand_block_with_table_splits(blk, ..., table_headers=table_headers)
    block_after_c = []
    for split_blk in block_after_b:
        block_after_c.extend(_split_long_block(split_blk["paragraphs"], ...))
    after_c.extend(_apply_part_suffixes(block_after_c))
```

四处：

1. **两个 split 阶段都**在同一个原始 block 内**，所以 `[part n]` 可以「按原始 block 的局部计数器」编号**（注释：「so split fragments can be labelled with `[part n]` using a row-local counter」）。若两个阶段跨 block，编号就没有意义了。

2. **`_apply_part_suffixes` 在两级 split 都完成之后** —— 所以它看到的是「一个原始 block 的所有最终碎片」，编号连续。

3. **`table_headers` 只在有 `blocks_path` 时加载**（`_load_table_headers(blocks_path) if blocks_path else {}`），且「absent / unreadable tables.json degrades to an empty map, so HeaderRecovery silently no-ops」。所以缺 sidecar 时表格仍能切，只是不补表头。

   注释还说了一个重要的顺序理由：「Fed into TableRowSplit so the header's tokens are budgeted **before** splitting (the slice + header never exceeds `target_max`), rather than backfilled after the cap is set」。**预算必须把表头算进去**，否则补表头会超上限。

4. **`_glue_heading_only_blocks` 在 `[part n]` 之后运行**，注释说明：「heading-only rows are never split, so no part suffix」——即无正文的标题块不需要编号。

**六个比例常量决定整个算法，而它们都由 `chunk_token_size` 派生。**

```python
target_max = max(int(chunk_token_size), 1)
target_ideal = max(int(target_max * _IDEAL_RATIO), 1)
table_max = max(int(target_max * _TABLE_MAX_RATIO), 1)
table_ideal = max(int(target_max * _TABLE_IDEAL_RATIO), 1)
table_min_last = max(int(table_max * _TABLE_MIN_LAST_RATIO), 1)
small_tail_threshold = max(int(target_max * _SMALL_TAIL_RATIO), 1)
overlap = _bounded_overlap(target_max, chunk_overlap_token_size)
```

三处：

1. **六个派生量，全部 `max(..., 1)` 兜底** —— 所以极小的 `chunk_token_size`（如 1）不会让某个比例算出 0（那会让「上限 0」变成「什么都超」）。

2. **`target_ideal` 是 `75%`**（文档：「The ideal target is set at 75 % of this value (mirroring the audit-mode 6000/8000 ratio)」）。所以「上限」与「目标」是两个值：块不超过 `target_max`，但合并时尽量接近 `target_ideal`（留余量）。

3. **`overlap` 用 `_bounded_overlap`**：

```python
def _bounded_overlap(target_max: int, chunk_overlap_token_size: int) -> int:
    overlap = max(int(chunk_overlap_token_size), 0)
    if target_max <= 1:
        return 0
    return min(overlap, target_max - 1)
```

   即「负归零、且不超过 `target_max - 1`」。这是把 c03 里那条「重叠必须小于块大小」的不变量在这里**主动夹紧**而不是抛错——因为 `"P"` 的重叠只在兜底路径与表格桥接上用，夹紧比失败合理。

**输出 schema 与 `sidecar` 的构造。**

```python
chunk_dict = {
    "tokens": blk["tokens"],
    "content": body,
    "chunk_order_index": idx,
    "heading": {"level": int(blk.get("level") or 0),
                "heading": str(blk.get("heading") or ""),
                "parent_headings": list(blk.get("parent_headings") or [])},
}
blockids = blk.get("blockids") or []
if blockids:
    chunk_dict["sidecar"] = {"type": "block", "id": blockids[0],
                             "refs": [{"type": "block", "id": bid} for bid in blockids]}
```

四处：

1. **`heading` 是嵌套字典**（`level`/`heading`/`parent_headings` 都在里面，不在顶层）。文档明确强调这一点：「``level`` / ``parent_headings`` live inside it, not at the top level」。

2. **`chunk_order_index: idx`** 来自 `enumerate(final)` —— 但 **`if not body: continue` 会跳过一些块**，所以顺序索引**可能有跳号**（因为 `idx` 是 `final` 的下标，而跳过的块占了号）。

   **这是一个真实的不一致**：c02/c03 的 `chunk_order_index` 都是 `len(results)`（连续无跳号），而这里用 `enumerate` 的下标。所以 `"P"` 的输出顺序索引可以跳号。下游若假设「`chunk_order_index` 连续」就会出问题。

3. **`sidecar` 只在 `blockids` 非空时出现**。所以无 `blockid` 的块没有 sidecar。文档说：「``sidecar`` is present only when the source row carried a ``blockid``」。

4. **`id` 是 `blockids[0]` 而 `refs` 是全部** —— 所以 `id` 表示「主 block」（第一个），`refs` 表示「所有来源」。块被合并后会有多个 blockid，此时 `id` 只是代表。

## 关键取舍

**源码事实：** `"V"` 的 `_semantic_groups_with_spans` 是 `SemanticChunker.split_text` 的**手工镜像**，依赖上游七个私有成员，注释给出验证过的版本范围（`langchain-experimental` 0.3.2–0.4.x）并说明有 drift guard 测试对比镜像与真实现的分组。

**源码事实：** 镜像的目的是去掉上游 `" ".join(sentences)` 的重排，好让每组携带精确 `text[start:end]`。

**源码事实：** `_sentence_spans` 三层降级：从 `cursor` 处 `find` → 从 0 处 `find` → 用 `cursor` 给零宽区间。空句子给零宽区间。第二层降级会让 `cursor` 后退，从而可能重复映射。

**源码事实：** 分组循环里 `min_chunk_size` 不足时 `continue`，而 `start_index = index + 1` 在其后——**所以被跳过的短段内容直接丢失**（与上游一致）。

**源码事实：** `"V"` 的嵌入预算：`embedding_func.max_token_size`，为 `None` **或 `0`** 时用 `chunk_token_size`。文档明确这是与 `openai_embed`（那里 `0` = 不截断）的**刻意分歧**，理由是「V needs a budget of its own」，且服务端路径根本产生不了 `0`。`budget_source` 字段把分歧来源传给日志。

**源码事实：** `"V"` 取消时调 `adapter.cancel_inflight()` 再 `raise`——因为 `to_thread` 取消后工作线程仍在跑；`cancel_inflight` 既取消在飞调用（拆掉服务端 worker pool）又**把拒绝变成粘性的**（覆盖「线程还没到适配器」）。

**源码事实：** `"V"` 的尺寸处理分两跳卸载（分组一跳、重切一跳），理由是重切开销与分组相当所以整段一起卸；`chunking_by_recursive_character` 在 `_size_and_resplit` 里**直接调用**，因为再提交进单 worker 池会**死锁**（已持有许可）。

**源码事实：** `SemanticChunker` 无内建上限，所以超 `chunk_token_size` 的语义段用 `"R"` 重切（否则只能靠嵌入时按 `embedding_token_limit` 而非用户配置的尺寸硬切）；子块区间用「段内偏移 + 段起点」换算，`_source_span` 畸形时置 `None` 但保留块。

**源码事实：** `"P"` 是四段流水线：TableRowSplit → AnchorSplit → HeadingGlue → LevelMerge，**顺序不可换**（HeadingGlue 必须在 LevelMerge 前，否则 bare heading 会粘到同级无关邻居）。

**源码事实：** `_block_to_paragraphs` 是 `split("\n")`，其无损性依赖 sidecar 把表格/公式/图形渲染成**单行标签**；`_is_table_paragraph` 判 `startswith("<table ")` 且 `endswith("</table>")`。若 sidecar 改成多行渲染，整个 TableRowSplit 会**静默失效**。

**源码事实：** 三个参考文献旋钮里**只有 `drop_references` 被快照**（它可能来自 per-file hint，记进 `doc_status.metadata['chunk_opts']`），而 `references_tail_n` 与 `references_headings` **运行期读 env 不快照**，所以改 env 对重跑已入队文档生效。

**源码事实：** `references_tail_n = 0` 表示**全扫**（不是禁用）；`_normalize_references_tail_n` 对不可转/负值都 warning + 回默认；`raw is None` 与 `raw == ""` 行为不同（后者多一条 warning）。

**源码事实：** `_is_reference_heading` 双规则：ASCII 前缀须满足词边界（`"Referenced work"` 不匹配），非 ASCII 前缀（`参考文献`）纯前缀匹配（`参考文献列表` 匹配）——因为 CJK 无词边界；用 `casefold()` 而非 `lower()`；判 `isascii()` 的是**前缀**而非待匹配文本。

**源码事实：** 丢参考文献前有保命判断：若丢弃后**没有任何带内容的行**，则保留并 warning；判据用 `any((row.get("content") or "").strip() for row in kept)` 而非 `kept` 非空，因为空行不算内容。

**源码事实：** 六个派生量（`target_max`/`target_ideal`/`table_max`/`table_ideal`/`table_min_last`/`small_tail_threshold`）全部 `max(..., 1)` 兜底；`target_ideal` 是上限的 75%（文档说镜像审计模式的 6000/8000）；`overlap` 用 `_bounded_overlap` **主动夹紧**（负归零、且 ≤ `target_max - 1`）而非抛错。

**源码事实：** `_apply_part_suffixes` 只对切成多块的组加 `[part n]`，且 `_strip_generated_heading_suffixes` **循环剥到不动点**（因为存档可能叠了多层新/旧两种后缀）；后缀只落在 `heading["heading"]`，`parent_headings` 不碰（否则 `_same_parent_path` 会判不等）。

**源码事实：** `_same_parent_path` 用于同级合并（防 `2.4.1` 与 `2.5.1` 被合并），`_is_descendant` 用于跨级吸收（浅块完整标题路径须是深块父链前缀，且比较前要剥 `[part n]`）；无标题的 preamble 路径为空因而可吸收任何块。

**源码事实：** `_can_merge_forward`/`_can_merge_backward` 只允许 `role == "none"`，理由具体：表格切片在 TableRowSplit 时已注入重复表头，再合并会让表头出现在正文中间——所以 `role` 实际上是**冻结标记**。

**源码事实：** `table_headers` 在 TableRowSplit **之前**注入，注释理由是把表头 token 计入预算（「the slice + header never exceeds `target_max`」）而不是切完再补；缺 `tables.json` 时降级为空 map 使 HeaderRecovery 静默 no-op。

**源码事实：** 输出 `chunk_order_index` 用 `enumerate(final)` 的下标，而循环里有 `if not body: continue`——所以**索引可能跳号**；这与 `"R"`/`"F"` 用 `len(results)` 的连续编号不一致。`sidecar` 仅在源行有 `blockid` 时出现，`id` = `blockids[0]`，`refs` = 全部 blockid。

**架构推论：** 两种「语义分块」的分界是**语义信号来自哪里**：`"V"` 来自模型（嵌入距离），`"P"` 来自文档结构（标题树）。前者通用但昂贵且不可解释，后者精确但要求 sidecar 且依赖解析质量。而两者都被同一个问题卡住——**上游的语义边界不带位置**——说明这不是某一方的缺陷，而是「语义」这件事本身不携带坐标：语义是模型/结构给出的判断，坐标是原文的属性，两者必须显式对齐。

**架构推论：** `"V"` 的镜像实现是最值得警醒的一处。它依赖 7 个上游私有成员，唯一的保障是一个 drift guard 测试。取舍很清楚：不镜像就只能拿 `" ".join(sentences)`（丢位置、丢格式），镜像就得跟着上游变。**判据是「这份能力的价值是否超过维护成本」**——对 `"V"` 而言位置是必需的（下游要 sidecar 回填），所以只能镜像。而把「验证过的版本范围」写进 docstring 是这种耦合的最低要求。

**架构推论：** `"P"` 的阶段化设计里有一条通用教训：**任何阶段只要「为了让每一片可独立理解」而注入共享内容（如表头），就必须同时把这一片标记为「不可再合并」**。否则下一个合并阶段会重复注入。`role` 字段就是这个标记。这类「增强破坏后续优化」的耦合在流水线设计里很常见，正确做法不是调整顺序，而是让增强阶段留下不可逆标记。

**架构推论：** 「与文档绑定的参数要快照，与策略绑定的参数不要」是一条清晰的配置设计原则。`drop_references` 可能是 per-file hint（与这份文档的证据绑定），所以进 `chunk_opts`；`tail_n`/`headings` 是全库策略，改了希望重跑生效，所以运行期读 env。这条区分的实际价值是：**避免「改配置对已入队文档无效」这种难查的行为**，同时保住「per-file 设置不被后来的全局改动冲掉」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/chunker/semantic_vector.py` | `_AsyncEmbeddingFuncAdapter` | 把 `EmbeddingFunc` 包成 LangChain `Embeddings`，含批量/并发/截断 |
| `lightrag/chunker/semantic_vector.py` | `_AsyncEmbeddingFuncAdapter._truncate` | 按 token 预算截断句子窗口 |
| `lightrag/chunker/semantic_vector.py` | `_AsyncEmbeddingFuncAdapter.cancel_inflight` | 取消在飞调用 + 粘性拒绝 |
| `lightrag/chunker/semantic_vector.py` | `_sentence_spans` | 句子 → 原文区间（三层降级） |
| `lightrag/chunker/semantic_vector.py` | `_trim_span` | 裁空白并夹紧区间 |
| `lightrag/chunker/semantic_vector.py` | `_semantic_groups_with_spans` | 镜像 `SemanticChunker.split_text` 但带 span |
| `lightrag/chunker/semantic_vector.py` | `chunking_by_semantic_vector` | `"V"` 入口：兜底、预算、适配器、两跳卸载 |
| `lightrag/chunker/semantic_vector.py` | `_size_and_resplit` | 编码每块、超限用 R 重切（直接调用避免死锁） |
| `lightrag/chunker/paragraph_semantic.py` | `_block_to_paragraphs` | `split("\n")` 恢复段落视图 |
| `lightrag/chunker/paragraph_semantic.py` | `_is_table_paragraph` | 单行 `<table ...>...</table>` 判定 |
| `lightrag/chunker/paragraph_semantic.py` | `_load_blocks_from_jsonl` | 读 sidecar；损坏即抛 `BlocksSidecarCorruptError` 不降级 |
| `lightrag/chunker/paragraph_semantic.py` | `_normalize_references_tail_n` | 三态归一（可转非负 / 不可转 / 负） |
| `lightrag/chunker/paragraph_semantic.py` | `_references_tail_n` / `_references_headings` | 运行期读 env（不快照） |
| `lightrag/chunker/paragraph_semantic.py` | `_is_reference_heading` | ASCII 词边界 / 非 ASCII 纯前缀双规则 |
| `lightrag/chunker/paragraph_semantic.py` | `_format_dropped_headings` | 日志行长度有界（截断 + `(+N more)`） |
| `lightrag/chunker/paragraph_semantic.py` | `_load_table_headers` | 读 `.tables.json`，缺失降级为空 map |
| `lightrag/chunker/paragraph_semantic.py` | `_expand_block_with_table_splits` | TableRowSplit：按行重切 + 重复表头 |
| `lightrag/chunker/paragraph_semantic.py` | `_split_long_block` | AnchorSplit：超长块按段落重切 |
| `lightrag/chunker/paragraph_semantic.py` | `_strip_generated_heading_suffixes` / `_append_part_suffix` | 循环剥旧后缀再加 `[part n]` |
| `lightrag/chunker/paragraph_semantic.py` | `_apply_part_suffixes` | 仅对切成多块的组加后缀 |
| `lightrag/chunker/paragraph_semantic.py` | `_is_heading_only` / `_glue_heading_only_blocks` | HeadingGlue：无正文标题粘到更深子块 |
| `lightrag/chunker/paragraph_semantic.py` | `_can_merge_forward` / `_can_merge_backward` | 只允许 `role == "none"` 参与合并 |
| `lightrag/chunker/paragraph_semantic.py` | `_same_parent_path` / `_is_descendant` | 同级同父 / 跨级祖先 两道防污染闸 |
| `lightrag/chunker/paragraph_semantic.py` | `_merge_small_blocks` | LevelMerge 主体 |
| `lightrag/chunker/paragraph_semantic.py` | `_bounded_overlap` | 重叠主动夹紧到 `< target_max` |
| `lightrag/chunker/paragraph_semantic.py` | `chunking_by_paragraph_semantic` | `"P"` 入口：兜底、参考文献丢弃、四段流水线、输出 schema |

## 参考

- [lightrag 源码：chunker/semantic_vector.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/chunker/semantic_vector.py)
- [lightrag 源码：chunker/paragraph_semantic.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/chunker/paragraph_semantic.py)
- [LangChain 文档：SemanticChunker](https://python.langchain.com/docs/how_to/semantic-chunker/)
- [lightrag 文档：文件处理流水线](https://github.com/HKUDS/LightRAG/blob/main/docs/FileProcessingPipeline.md)

## 与主核 / AI 的关系

两种语义分块对应主核（多模态数据库内核）里**两种聚簇决策**：`"V"` 像「按数据相似度聚簇」（如 K-means 分区），`"P"` 像「按 schema 结构分区」（如按范围分区）。前者对数据分布自适应但结果不可预测、边界不可解释；后者边界完全由元数据决定、可预测但要求元数据准确。主核的 `PARTITION BY RANGE` 与 `PARTITION BY HASH` 就是这一对——而主核的做法是**让用户选**并且**把分区键写进 schema**，从不偷偷决定。这里同理：`"V"` 与 `"P"` 是两个显式选项，不能互相替代。

第二处对照是 `"V"` 的镜像实现与主核的**扩展 API 版本耦合**。主核的扩展通过 `PG_MODULE_MAGIC` 声明兼容的 ABI 版本，版本不匹配直接拒绝加载。这里的做法是「写死版本范围 + drift guard 测试」——更弱（运行期不会拒绝）但更灵活（小版本升级通常仍可用）。判据是**上游变更的频率与破坏性**：LangChain 的 `SemanticChunker` 内部实现会变，但没有 ABI 承诺，所以只能靠测试守。

第三处是 `"P"` 的「注入表头即冻结不可合并」，与主核的 **`VACUUM FULL` 与索引重建**关系同理：一个操作做了「为了局部完整性」的冗余（表头重复 / 索引重建），就必须留下标记让后续操作知道「这部分不能按常规处理」。主核用 `pg_index.indisvalid` 这类标志位表达；这里用 `role` 字段。

对 AI 工程的实际意义在于：语义分块看起来比固定窗口「更聪明」，但它的成本与风险都更高。这份实现给出的三条经验可直接用：**语义边界必须与原文位置显式对齐**（否则下游 traceback 全断）、**镜像上游私有实现必须配 drift 测试**（否则升级静默改变行为）、以及**为局部完整性注入的内容必须冻结该片段**（否则下一个阶段会重复注入）。
