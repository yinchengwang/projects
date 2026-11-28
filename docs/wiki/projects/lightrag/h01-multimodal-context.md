---
project: lightrag
feature: multimodal-context
title: 多模态上下文与原子化
page_type: feature
schema_version: 1
refs:
  - path: lightrag/multimodal_context.py
    symbols: [_atomize, _largest_fitting_row_prefix, _row_trim_table_leading]
  - path: lightrag/multimodal_context.py
    symbols: [find_target_span, _split_text_segment, _char_trim_trailing]
---

# 多模态上下文与原子化

## 一句话职责

给 `drawings.json` / `tables.json` / `equations.json` 里每个多模态条目补一段 `surrounding`（目标前后的正文），使 VLM 分析时能看见「这张图在文章里处于什么语境」——而这段文本必须在 token 预算内、以段落/句子/子句边界截断、**且不能把 `<table>…</table>` 这类标记切成半截**。

## 机制是什么

先要理解它补的是什么缺。一条多模态条目（一个图、一张表、一个公式）在 `blocks.jsonl` 的某一行里以标签形式出现，位置由 `blockid` 定位。而送去分析的内容只有条目本身——**上下文丢了**。所以这个模块做四步：

1. **定位**：在对应 `blockid` 的那一行内容里找到目标标记的 `(start, end)`。
2. **切分候选**：`block_content[:start]` 是 leading 源，`block_content[end:]` 是 trailing 源。
3. **清洗**：把解析器内部标识（`id` / `path` / `src` / `refid`）从候选文本里剥掉——**在截断之前**。
4. **预算内截断**：按段落/句子/子句递归切分，从目标一侧向外累积；单个最近片段仍超预算时退到字符级二分。

而「不能切半截标签」这件事决定了整个实现的核心结构：**先原子化，再切分**。`_atomize` 把字符串切成 `(kind, content)` 序列，`kind ∈ {"text", "drawing", "equation", "table"}`，而 docstring 承诺一条可检验的性质：**把所有 atom 的内容拼起来逐字复现原文**。

所以标签是一个**不可分割的单位**，切分器只能在 text atom 内部动刀。这解释了为什么代码里没有一处「在某个偏移处截断」——所有截断都作用在 atom 内部的纯文本上。

## 怎么实现的

**`_atomize` 只做一件事：按三个多模态标签的正则切段，并保证无损。**

```python
for match in _MM_TAG_RE.finditer(text):
    if match.start() > pos:
        atoms.append(("text", text[pos : match.start()]))
    tag_text = match.group(0)
    if tag_text.startswith("<drawing"):   kind = "drawing"
    elif tag_text.startswith("<table"):   kind = "table"
    else:                                  kind = "equation"
    atoms.append((kind, tag_text))
    pos = match.end()
if pos < len(text):
    atoms.append(("text", text[pos:]))
```

三个细节：

1. **kind 靠 `startswith` 判而不是靠正则分组名** —— 因为 `_MM_TAG_RE` 是三个分支的或，用组名会让正则更难读；而三个前缀互不重叠，`startswith` 是充分判据。
2. **空文本不产生 atom**（`if match.start() > pos`）—— 所以序列里不会有零长的 text atom，下游的 `if not atom_text: continue` 是双保险。
3. **尾部残余单独处理** —— 若最后一个标签之后还有文本，它不会被任何 `finditer` 匹配捕获。

而这条「拼接可复现」的性质是整条管线的地基：因为截断永远不会作用在标签上，所以「截断后的文本」一定还包含完整的标签（或完全没有该标签），不会有 `</tab` 这种半截产物。

**四个目标定位正则各自处理「属性顺序不定」这件事，而 table 的定位有两条规则。**

```python
def _drawing_pattern(item_id: str) -> re.Pattern[str]:
    esc = re.escape(item_id)
    return re.compile(rf'<drawing\b[^>]*?\bid\s*=\s*"{esc}"[^>]*?/>', re.DOTALL)
```

关键在 `[^>]*?\bid\s*=\s*"{esc}"` —— **`id` 前面允许任意其他属性**，所以 `<table id="T1" format="json">` 与 `<table format="json" id="T1">` 都能匹配。

`_table_cite_pattern` 用了两个前瞻而不是一个顺序正则：

```python
rf'<cite\b(?=[^>]*\btype\s*=\s*"table")'
rf'(?=[^>]*\brefid\s*=\s*"{esc}")[^>]*>.*?</cite>'
```

两个独立的 `(?=...)` 意味着**两个属性的顺序无所谓**——若写成 `type="table"[^>]*refid="T1"`，那属性反过来就匹配不上。这是一个很容易写错的地方，因为顺序正则在「自己造的测试数据」上通常能过。

`find_target_span` 的分派里，tables 是唯一有两条模式的：

```python
patterns = [_table_tag_pattern(item_id), _table_cite_pattern(item_id)]
```

docstring 说明了优先级：「`<table id="…">` 标签**优先于**指向同一 id 的 `<cite type="table" refid="…">`，无论各自在块里的位置；cite 只是『块里提到这张表但没持有它的标签』时的兜底（已移除的 content_list 写者发出的替代物）」——所以是**按模式顺序取第一个命中，而不是按文本位置**。

而它最后一句很诚实：「树内没有写者会把两者放进同一个块，所以优先级只在别处写的 sidecar 上有意义」。

**清洗必须在截断之前，而 docstring 给了两个理由——第二个是正确性理由。**

第一个是语义的：「这与 chunk 内容在实体抽取前的处理一致，确保多模态分析提示词永远不会看到那些内部标记」。

第二个是结构的：

> 在截断之前清洗也保证**截断点永远不会落进 `id="…"` 属性里并留下一个 strip 正则不再认识的畸形标签**。

所以顺序不是「顺手先做」，而是**让下游的不变量成立**：strip 正则假设标签是完整的；若先截断，可能产生 `<table id="tab`，而它不匹配任何 strip 规则，于是这个畸形成分会被原样送进提示词。

这也解释了为什么 `_build_leading` / `_build_trailing` 里的清洗发生在 `_atomize` **之前**：

```python
source = strip_internal_multimodal_markup_for_extraction(source, keep_cite_tag=True)
if not source:
    return ""
accumulated = ""
atoms = _atomize(source)
```

所以 atom 化作用在**清洗后的文本**上。若反过来（先 atom 再清洗），atom 内的标签会被清洗改写成不含 id 的形态——那不一定是错的，但会让「预算反映 LLM 实际看到的内容」这条性质更难维持。

**`keep_cite_tag=True` 是一个有区别的开关，而两种模态对它的选择相反。**

docstring 说得很具体：`<cite type="table">表 3</cite>` 里的 `refid` 会被剥掉但 **wrapper 保留**，因为「VLM/LLM 仍然能区分一个引用标签与行内散文（例如 `表 3` 会让它明显看出『表 3』指的是文档别处另一张表，而不是一个普通名词短语）」。

而 tables 的 surrounding **不用它**——`_build_leading` 里：

```python
if kind == "tables":
    source = remove_table_tags(source)
    if not source: return ""
source = strip_internal_multimodal_markup_for_extraction(source, keep_cite_tag=True)
```

tables 先走 `remove_table_tags`，而那个函数把 table 与 cite 一起删（模块 docstring 说「tables.json 的 surrounding 仍然通过 `remove_table_tags` 丢掉所有 cite 标签，因为目标表的分析不应被指向其他表的悬空引用带偏」）。

所以这里是**按目标类型倒推需要什么语境**：

- 分析一张图 → 想知道「图在文章的哪一段」，其他表格的存在是干扰，但「见表格3」这个指向本身是有效语境的。
- 分析一张表 → 想知道「表周围的正文说了什么」，其他表格（和指向它们的引用）全是干扰。

**「tables 先删标签再算预算」还有一个数值上的必要性，而模块 docstring 明说了。**

> 对 `tables.json` 条目——surrounding 应当描述目标表周围的文本而不把其他表拖进来——每个 `<table>…</table>` 都在**token 计数与分段之前**从候选文本里移除，**所以保存的 surrounding 字符串与为它预算的 token 保持同步**。

所以移除必须发生在预算之前。若在预算之后移除，那「我给了 2000 token，实际文本只有 800 token」——预算是按含标签的文本算的，而保存的是去掉标签后的文本。

**`_split_text_segment` 保留分隔符，而这是「拼接无损」的必要条件。**

```python
parts = text.split(sep)
assembled = []
for j, part in enumerate(parts):
    if j < len(parts) - 1:
        assembled.append(part + sep)
    else:
        if part:
            assembled.append(part)
if len(assembled) > 1:
    return assembled, idx
```

所以分隔符**附着在前一个片段尾部**，因此把片段接回去等于原文。而它返回 `(segments, sep_index)`，`sep_index` 让调用方知道「用了第几级分隔符」——这样递归时可以从更弱的下一级继续。

而「第一个产生 >1 片段的分隔符」这个判据意味着：**若某个分隔符在文本里不存在（`sep not in text`），跳过它继续找下一个更弱的分隔符**。这就是「递归分隔符级联」的实际含义——它不是先按段落分再对每段按句子分，而是「找一个能切的层级切下去」。

**`_char_trim_leading` 与 `_char_trim_trailing` 是一对，而它们的二分方向相反。**

```python
def _char_trim_leading(text, max_tokens, tokenizer):
    """Drop characters from the head until the token count fits. ... we want to keep the *tail*"""
    lo, hi = 0, len(text)
    while lo < hi:
        mid = (lo + hi) // 2
        if _count_tokens(tokenizer, text[mid:]) <= max_tokens: hi = mid
        else: lo = mid + 1
    return text[lo:]

def _char_trim_trailing(text, max_tokens, tokenizer):
    """Drop characters from the tail ... we keep the *head*"""
    lo, hi = 0, len(text)
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if _count_tokens(tokenizer, text[:mid]) <= max_tokens: lo = mid
        else: hi = mid - 1
    return text[:lo]
```

注意两个 `mid` 的算式不同：`(lo + hi) // 2` 与 `(lo + hi + 1) // 2`。这是二分「找左边界」与「找右边界」的标准写法差异——前者配 `hi = mid`，后者配 `lo = mid`。若两边用同一个算式，其中一侧会死循环。

而「leading 保留尾部、trailing 保留头部」这个选择是按语义定的：**leading 是目标之前的内容，越靠后越接近目标，所以要保住接近目标的那一端**。trailing 反之。两处 docstring 都把这一点写成了显式理由（"closest to the target"）。

**`_largest_fitting_row_prefix` 用二分而不是线性扫描，而理由是可算的复杂度。**

```python
lo, hi = 0, row_count - 1
best: str | None = None
while lo < hi:
    mid = (lo + hi + 1) // 2
    candidate = build(mid)
    if _count_tokens(tokenizer, candidate) <= max_tokens:
        lo, best = mid, candidate
    else:
        hi = mid - 1
return best
```

docstring：

> `build(k)` 渲染一个持有 k 行的表；它的 token 数随 k 增长，所以搜索用二分而不是探测每个 k：**每次探测都要重新序列化并重新 tokenize 整个候选，对一个大表做线性扫描是平方级的**。二分依赖那个增长是单调的，与 `_char_trim_trailing` 对字符所做的同一个 tokenizer 假设。

这段把「为什么能二分」讲清了：**单调性**。而它同时承认了这个假设是 tokenizer 级别的——一个 tokenizer 在行数增加时 token 数不增加是不可能的，但「严格单调」并不保证（可能出现加入一行而 token 数不变的情况）。二分在非严格单调下仍能返回**一个**可行解，而 `best` 变量保证返回的是「探测过的最大可行 k」。

而 `if lo == hi` 时循环退出——注意 `hi` 初值是 `row_count - 1`，所以 `k` 的范围是 **[1, row_count - 1]**，即「至少留一行出去」。docstring 明确写「`k` in `[1, row_count - 1]`」。

**`_row_trim_table_leading` 是「按行裁剪表格」的入口，而它把 JSON 与 HTML 两种表格分开处理。**

```python
match = TABLE_TAG_RE.match(tag_text.strip())
if not match: return None
attrs = match.group("attrs")
body = match.group("body")
fmt = detect_table_format(attrs, body)
if fmt == "json":
    parsed = parse_table_tag(tag_text)
```

所以流程是：先解析出 `attrs` 与 `body`、判格式（JSON 还是 HTML）、然后走对应的行切分。

而它存在的**语义**理由是模块 docstring 里那句：「超尺寸的 JSON 或 HTML 表被行裁剪（leading 取尾部行、trailing 取头部行），**所以 surrounding 保留物理上最接近目标的行**」。

这解释了为什么表格不能只做字符级截断：字符截断会把 `<tr>` 切成半个、或者留下一个「表头没了」的残表。而按行裁剪的产物是一个**仍然合法的表**（有完整的 `<table>` wrapper 与完整的行），代价是行数变少。

`_trim_content_inner` 里有一条相应的保护：

```python
trimmed = _row_trim_table_trailing(content, budget, tokenizer)
if trimmed is not None: return trimmed
# Budget too small for a valid <table> wrapper; trim body text only
# so we never emit a partial opening tag.
return _char_trim_trailing(match.group("body"), budget, tokenizer)
```

**注释那句「所以永远不会吐出半个开标签」**是这个模块反复出现的原则：预算不够时宁可丢掉整个 wrapper 只保留 body 文本，也不产出一个畸形标签。而注意它取的是 `match.group("body")`——已经解析出来的 body，所以 wrapper 被明确丢弃而不是被截断。

**`trim_content_to_budget` 里有一段「为截断标记预留预算」的处理，而它的失败路径很实在。**

```python
marker_probe = _CONTENT_TRUNCATION_MARKER.format(original=original_tokens, final=max_tokens)
marker_tokens = _count_tokens(tokenizer, marker_probe)
if marker_tokens >= max_tokens:
    return _trim_content_inner(content, kind=kind, budget=max_tokens, tokenizer=tokenizer), True
inner_budget = max(0, max_tokens - marker_tokens)
```

三步：先用一个**最坏情况的标记**（`final=max_tokens`，即最长的数字）测它的 token 数，若标记本身就占满预算则**放弃标记只保内容**，否则从预算里扣掉。

然后：

```python
result = trimmed_inner + marker
if _count_tokens(tokenizer, result) > max_tokens:
    return _trim_content_inner(content, kind=kind, budget=max_tokens, tokenizer=tokenizer), True
return result, True
```

最后这道检查的理由写在注释里：「**tokenizer 的非可加性**仍可能把它推过预算；那就算掉标记」。

这两处合起来说明一个具体事实：**「文本 A 的 token + 文本 B 的 token」不必然等于「A+B 的 token」**。BPE tokenizer 在拼接边界上会重新合并，所以「先算两半再加起来」是不可靠的。这个模块在两处都被它咬过（标记预留、`_accumulate_text_leading` 的每次整串重算），而应对方式都是**每次对整串重新计数**而不是累加。

**`_build_leading` 的 atom 遍历是从右往左，而三种 atom 的处理方式不同。**

```python
for atom_idx in range(len(atoms) - 1, -1, -1):
    atom_kind, atom_text = atoms[atom_idx]
    if not atom_text: continue
    if atom_kind in {"drawing", "equation"}:
        candidate = atom_text + accumulated
        if _count_tokens(tokenizer, candidate) <= max_tokens:
            accumulated = candidate
            continue
        break
    if atom_kind == "table":
        ...
        break
    addition = _accumulate_text_leading(...)
```

三个分支的差别：

- **drawing / equation**：整块加上，不进去切。放不下就**直接停**（`break`）——因为进去切一个 drawing 标签没有意义（标签本身是个原子单位，切它只会得到畸形结果）。
- **table**：整块放不下时，**按行裁一次**再放（`_row_trim_table_leading`），然后 `break`。
- **text**：交给 `_accumulate_text_leading` 做递归切分，且它返回 `None` 时停。

所以「原子性」在这里是分级的：drawing/equation 完全不可切；table 可切但只在行边界；text 可任意切（但仍优先语义边界）。

而 table 分支里那段注释点明了一个前置条件：「只对 drawings/equations 的 surrounding 会走到这里——table 标签对 `tables` 类型在上面已被预先剥掉」。所以同一个函数在两种 kind 下的行为不同，而这个不同由调用前的预处理保证。

**`_accumulate_text_leading` 是这一页最难的一段，而它的注释把两个失败分支区分得很清楚。**

```python
buf = ""
for i in range(len(segments) - 1, -1, -1):
    candidate = segments[i] + buf
    if _count_tokens(tokenizer, candidate + existing) <= max_tokens:
        buf = candidate
        continue
    if buf:
        return buf
    weaker = separators[sep_idx + 1 :] if sep_idx < len(separators) else []
    if weaker:
        return _accumulate_text_leading(segments[i], existing=existing, tokenizer=tokenizer, max_tokens=max_tokens, separators=weaker)
    remaining = max_tokens - _count_tokens(tokenizer, existing)
    if remaining <= 0: return None
    trimmed = _char_trim_leading(segments[i], remaining, tokenizer)
    return trimmed if trimmed else None
return buf if buf else None
```

关键在于「放不下当前片段」时的两个分支：

**分支 A：`if buf: return buf`** —— 已经加进了至少一个片段，那就**停下，不去字符截断一个更远的片段**。注释：「我们已经加进了至少一个片段——停在这里，不字符截断一个更远的片段」。

这是一个语义选择：字符截断会产生「半句话」，而完整片段是干净的边界。所以**宁可少一些完整句子，不要多一句半句**。

**分支 B：`buf` 为空** —— 最近的片段本身就超预算。这时不放弃，而是**递归到更弱的分隔符**（`separators[sep_idx + 1:]`）在**这个片段内部**再切。

注释：「递归进下一级分隔符，这样在退到字符之前先试一次更细的切分」。

**分支 C：没有更弱的分隔符** —— 才退到字符级，且只取「剩余预算内最长的后缀」。

三级降级：整片段 → 更细片段 → 字符。而每一级都只在上一级**完全无法产出**时才触发（分支 A 的存在保证了不会为了多塞一点而跳级）。

注意 `candidate + existing` 与 `segments[i] + buf` 的计数方式：它算的是「prepend 之后的整串」，不是各半之和。这就是前面说的「非可加性」应对方式。

**`_build_leading` 末尾的双重停止条件也值得一提。**

```python
accumulated = addition + accumulated
if _count_tokens(tokenizer, accumulated) >= max_tokens:
    break
```

所以除了 `addition is None`，还有一个「已经到预算」的提前退出。没有它的话，循环会继续往前遍历剩下的 atom，每个都调一次 `_accumulate_text_leading`（而它内部会做多次 tokenize）——纯粹浪费，因为结果一定是空。

## 关键取舍

**源码事实：** `_atomize` 把文本切成 `(kind, content)` atom 且**拼接可逐字复现原文**；`kind ∈ {"text", "drawing", "equation", "table"}`。切分器只能在 text atom 内部动刀，所以标签永远不被切半。

**源码事实：** 目标定位正则用 `[^>]*?\bid\s*=\s*"..."`，所以 `id` 与其他属性的顺序无关；table 的 cite 用**两个独立前瞻**同时约束 `type` 与 `refid`，同样与顺序无关。

**源码事实：** tables 的定位有两条模式且**按模式顺序**（而非文本位置）取第一个命中：`<table id="…">` 优先于 `<cite refid="…">`，后者只是「块里提到但没持有标签」的兜底。

**源码事实：** 内部标识的清洗发生在**截断之前**、也发生在 atom 化之前；理由之一是语义（提示词不该看到内部标记），之二是结构（截断点落进 `id="…"` 会产出 strip 正则不认识的畸形标签）。

**源码事实：** `keep_cite_tag=True` 只用于 drawings / equations；tables 先走 `remove_table_tags` 把所有 table 与 cite 一起删，因为「目标表的分析不应被指向其他表的悬空引用带偏」。

**源码事实：** tables 的标签移除在**token 计数与分段之前**，所以保存的字符串与为它预算的 token 保持同步。

**源码事实：** `_split_text_segment` 把分隔符**附着在前一片段尾部**，因此拼接无损；「第一个能切出 >1 片段的分隔符」即级联的实际含义。

**源码事实：** `_char_trim_leading` 保留尾部、`_char_trim_trailing` 保留头部，两处都按「越靠后越接近目标」定；两者的 `mid` 算式不同（`(lo+hi)//2` 配 `hi=mid`，`(lo+hi+1)//2` 配 `lo=mid`）以避免死循环。

**源码事实：** `_largest_fitting_row_prefix` 用二分，因为「每次探测都重序列化并重 tokenize 整个候选，对大表线性扫描是平方级」；二分依赖 token 数随行数单调，与字符级裁剪同一个假设。

**源码事实：** 表格在预算不够时按**行**裁剪（leading 取尾部行、trailing 取头部行），因为字符截断会切出半个 `<tr>` 或砍掉表头；预算是连 wrapper 都不够时，退到「只保留 body 文本」，**绝不吐出半个开标签**。

**源码事实：** `trim_content_to_budget` 为截断标记预留预算，且用最坏情况（`final=max_tokens`）测标记长度；标记本身超过预算时**放弃标记只保内容**；最后还有一道「tokenizer 非可加性可能把它推过预算」的复检。

**源码事实：** `_accumulate_text_leading` 三级降级：整片段 → 更细分隔符下递归 → 字符级。而「已加进至少一个片段」时**立即停**，不去字符截断更远的片段（宁可少整句，不要多半句）。

**源码事实：** drawing / equation atom 放不下就 `break`（不进去切，因为切标签只会得到畸形结果）；table atom 放不下时按行裁**一次**再 `break`；只有 text atom 会递归切分。

**架构推论：** 「原子化」这个结构是「无损截断」的唯一廉价解。若直接按字符裁剪，就必须在每一步判断「我是不是正落在标签中间」——那要么要求解析器的位置信息（成本高且易错），要么就接受偶发畸形。先原子化之后，截断的输入域被限制成纯文本，正确性变成结构性质而不是逐点检查。而代价是必须先做一次全串扫描与分类。

**架构推论：** 「清洗先于截断」这条顺序，是「让下游的不变量成立」而不是「顺手优化」。strip 正则假设标签完整，而截断能制造不完整标签——所以只要顺序反了，strip 就会静默失效。这类「两步都有正当事由但顺序有硬要求」的地方最适合写测试固定，因为颠倒顺序后代码仍能跑、只是产出略差。

**架构推论：** 三种 atom 三种处理方式（不可切 / 按行切 / 任意切）说明「原子性不是布尔量」。真正的设计问题是「在每个类型上，最小的合法可切单位是什么」：对标签是一个整体，对表格是一行，对文本是一个句子。搞错了不会崩，只会产出难读或畸形的结果——所以这个判断必须由语义而不由实现便利决定。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/multimodal_context.py` | `enrich_sidecars_with_surrounding` | 为启用的模态回填 `surrounding` 的总入口 |
| `lightrag/multimodal_context.py` | `build_surrounding` | 由 span 与预算算出 `{leading, trailing}` |
| `lightrag/multimodal_context.py` | `find_target_span` | 定位目标标记，table 有标签/引用两级 |
| `lightrag/multimodal_context.py` | `_atomize` | 按标签切 atom，拼接可复现原文 |
| `lightrag/multimodal_context.py` | `_MM_TAG_RE` | 三个多模态标签的或，`DOTALL` |
| `lightrag/multimodal_context.py` | `_split_text_segment` | 按级联分隔符切分并保留分隔符 |
| `lightrag/multimodal_context.py` | `_char_trim_leading` | 字符级保尾部，二分找左边界 |
| `lightrag/multimodal_context.py` | `_char_trim_trailing` | 字符级保头部，二分找右边界 |
| `lightrag/multimodal_context.py` | `_largest_fitting_row_prefix` | 二分找最大可容纳行数，避免平方级扫描 |
| `lightrag/multimodal_context.py` | `_row_trim_table_leading` | 表格按行裁尾部行，保 `<table>` wrapper 合法 |
| `lightrag/multimodal_context.py` | `_build_leading` | 从右向左累积 atom，三种类型三种处理 |
| `lightrag/multimodal_context.py` | `_accumulate_text_leading` | 整片段 → 更细分隔符 → 字符 三级降级 |
| `lightrag/multimodal_context.py` | `trim_content_to_budget` | 为分析提示词压缩 content，预留截断标记 |
| `lightrag/multimodal_context.py` | `remove_table_tags` | tables 语境里删掉所有 table 与 cite |

## 参考

- [lightrag 源码：multimodal_context.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/multimodal_context.py)
- [lightrag 源码：table_markup.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/table_markup.py)
- [Python 正则表达式文档](https://docs.python.org/3/library/re.html)
- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)

## 与主核 / AI 的关系

主核里最接近的是**变长字段的部分读（partial read）与截断显示**：一个超大文本字段要在有限缓冲里显示开头与结尾，而截断点不能落在字符编码的多字节序列中间——否则会产出无效 UTF-8。这里的问题是同一个：**截断点不能落在结构单位中间**。

差别在于结构单位的层级。主核只需关心字节/字符边界（编码层面），这里要关心三层：字符、句子（分隔符级联）、标签（atom）。层级越多，正确的截断点越稀疏，所以代码里出现了三级降级。

与 h02（表格标记）的连接最直接：`find_target_span` 调 `parse_table_tag` / `detect_table_format` / `serialize_html_rows` / `split_html_rows`，而 `_row_trim_table_leading` 依赖 `TABLE_TAG_RE` 的 `attrs` / `body` 分组。所以那一页定义表格的表示，这一页消费它。

与 e03（上下文预算）的关系是同一类问题的两个尺度：那边按 token 预算挑选哪些检索结果进上下文，这边按 token 预算截断单条文本。共同的技术点是「token 数不可加」——两处都必须对整串重新计数。

对 AI 工程最实用的一条是**「截断必须在结构单位上做，而不是在字符数上做」**。任何把长文本塞进模型输入的系统都会遇到：按字符截断会产出半句话、半个 JSON、半个标签，而模型看到的输入就是脏的。做法是先定义「这个格式里最小的合法单位」（句子 / 行 / 标签），再只在这些单位之间截断；字符级只作为最后兜底。

另一条是「清洗先于截断」这条顺序要求。它很容易被颠倒，因为两个动作各自都合理、都能单独跑通——但颠倒后 strip 规则会静默失效。判据是：**后面那步是否假设了前面那步建立的某个不变量？** 若假设了（这里是「标签完整」），顺序就是硬约束，必须写进 docstring 或测试。

最后一条是 tokenizer 的非可加性。任何「A 的 token 数 + B 的 token 数 ≤ 预算所以 A+B 安全」的推理都是错的——BPE 在拼接边界会重新合并。正确做法是每次对**将要输出的整串**计数，这也解释了为什么这段代码里到处都是「重算整个 candidate」而不是增量累加。
