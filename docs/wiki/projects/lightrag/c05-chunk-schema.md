---
project: lightrag
feature: chunk-schema
title: 块结构与标题上下文
page_type: feature
schema_version: 1
refs:
  - path: lightrag/chunk_schema.py
    symbols: [normalize_chunk_heading, _clean_and_cap_headings, format_heading_context]
  - path: lightrag/chunk_schema.py
    symbols: [normalize_chunk_sidecar, strip_internal_multimodal_markup_for_extraction]
---

# 块结构与标题上下文

## 一句话职责

三件共享的规范化：把两代不同的标题表示（嵌套字典 / 三个扁平字段）压成同一个 `{"level", "heading", "parent_headings"}` 结构；把 `sidecar` 的 `refs` 补齐成「至少一个元素」以避免下游到处判空；以及从块内容里剥掉解析器内部标识符——只给抽取 LLM 看干净文本，**不改存储中的 `content`**。

## 机制是什么

这块代码只做规范化，但它解决的是三个「两处消费者必须看到同一份数据」的问题：

1. **标题有两代格式**。`"P"` 分块器（c04）产出嵌套的 `heading` 字典，而历史存档里是 `heading: str` + `parent_headings: list` + `level: int` 三个扁平字段。查询期与抽取期都要读标题，若各写一套解析就会漂移。所以 `normalize_chunk_heading` 是**唯一入口**。
2. **`sidecar.refs` 可能是缺失的**。单来源的块不需要 `refs`（它自己的 `type`/`id` 就是全部），但下游（如文档删除时的缓存清理）要遍历 `refs`。所以规范化时**主动造出 `[{"type", "id"}]`**，让下游不必判字段是否存在。
3. **块内容里嵌着解析器内部标识符**。`<table id="tb-doc-0007" format="json" caption="温度统计">` 里的 `tb-doc-0007` 是解析器的内部 id，若送进抽取 prompt，LLM 会把它当成一个实体抽出来——于是图谱里出现一堆「tb-doc-0007」这样的噪声节点。

第三条的解法有一个关键约束：**只剥给 LLM 看的那一份，存储里的原样不动**。因为引用回链（`<cite refid>`）要靠原 id 解析。

## 怎么实现的

### 标题归一化

**`normalize_chunk_heading` 的输入判定只看「`heading` 是不是 `dict`」。**

```python
nested = dp.get("heading")
if isinstance(nested, dict):
    heading_text = str(nested.get("heading") or "").strip()
    parents_raw = nested.get("parent_headings") or []
    level_raw = nested.get("level", 0)
else:
    heading_text = str(nested or "").strip()
    parents_raw = dp.get("parent_headings") or []
    level_raw = dp.get("level", 0)
```

四处：

1. **判据是 `isinstance(nested, dict)`** —— 所以「新版嵌套」与「旧版扁平」靠一个类型检查分流。这比检查版本号或字段组合简单，且对「两种格式混写」也有效（`heading` 是 dict 时忽略顶层的 `parent_headings`）。

2. **`nested.get("level", 0)` 有默认值** —— 所以嵌套形式缺 `level` 时按 0 处理。

3. **`str(nested or "").strip()`** —— 扁平分支里 `heading` 可能是 `None`，`None or ""` 得到 `""`。

4. **`parents_raw` 的两支都允许非 list**（后面判 `isinstance(parents_raw, list)`）。所以「`parent_headings` 是字符串」这种畸形输入会被当成「没有父标题」而不是崩。

而 `parent_headings` 的清洗：

```python
parent_headings: list[str] = []
if isinstance(parents_raw, list):
    for entry in parents_raw:
        text = str(entry or "").strip()
        if text:
            parent_headings.append(text)

try:
    level = int(level_raw or 0)
except (TypeError, ValueError):
    level = 0
```

三处：

1. **空项被丢弃**（`if text`）。所以 `["A", "", "B"]` 变成 `["A", "B"]`——而这对面包屑是有意义的（空层级不该占位）。

2. **`int(level_raw or 0)` 包 `try`** —— 所以 `level="abc"` 得到 0 而不是崩。注意 `int(0.7)` 会**截断成 0**（不四舍五入），所以浮点 level 会静默变成 0。

3. **`level_raw or 0`** —— 所以 `level=None`、`level=""`、`level=0` 都得到 0。

最后那个「全空则返回 `None`」：

```python
if not heading_text and not parent_headings and level == 0:
    return None
```

**注意条件是三者的「与」** —— 所以只要有任何一项非空就返回字典。这意味着一个「有 `level=3` 但没标题文本」的块会返回 `{"level": 3, "heading": "", "parent_headings": []}`——**非 `None` 但无内容**。

这与文档说的「Empty / missing inputs collapse to `None`」有细微差别：`level != 0` 不算 empty。所以下游若判 `if normalized:` 会得到「有标题」的正向结论，但渲染出来是空字符串。这处不一致由 `format_*` 函数兜住（它们对空列表返回 `""`），但如果下游只看「是否是 `None`」就会误判。

### 标题清洗：一条字符级流水线

**`_clean_heading_text` 的顺序不能换，而注释逐条解释了原因。**

```python
def _clean_heading_text(text: str) -> str:
    text = text.replace("→", " ")
    text = "".join(ch for ch in text
                   if ch in _HEADING_KEEP_WS
                   or unicodedata.category(ch) not in _HEADING_STRIP_CATEGORIES)
    text = _HEADING_WHITESPACE_RE.sub(" ", text)
    return text.strip()
```

四步，每步一个明确目的：

1. **`replace("→", " ")` 在最前** —— 因为 `→`（U+2192）是面包屑分隔符。若一个标题**内部**含有 `→`，它会被 `" → "` 的反向拆分当成多一层。注释说得很直接：「would forge an extra level when the breadcrumb is split back on " → "」。**所以这是一个防伪造的措施**，针对的是 c04 里 `operate._truncate_section_context` 那个「把面包屑拆回层级」的操作。

2. **按 Unicode 类别剥 `Cc`（控制符）与 `Cf`（格式符）** —— 注释列举了具体内容：NUL、零宽标记（ZWSP/ZWNJ/ZWJ/WORD JOINER/BOM 全是 `Cf`）、方向控制符。这些「只会给 LLM prompt 加 token 噪声」。

3. **`_HEADING_KEEP_WS` 的豁免是本页最细的一处**：

```python
_HEADING_KEEP_WS = frozenset("\t\n\r\f\v")
```

   注释解释了为什么需要它：「The only Cc chars that `\s` folds into a space; keep them through the strip pass so they become a space below instead of gluing adjacent words. (Cannot use `str.isspace()` for this: Python treats `\x1c`-`\x1f` as whitespace but `\s` does not, so those would survive the strip — an explicit set matches the regex.)」

   **这里的推理链值得完整复述**：
   - `\t`/`\n`/`\r`/`\f`/`\v` 属于 `Cc` 类别，若不豁免就会被第 2 步剥掉。
   - 剥掉之后 `"A\nB"` 变成 `"AB"`——两个词粘在一起。
   - 但第 3 步的 `\s+` 会把它们折成一个空格，所以**必须让它们活到第 3 步**。
   - 而**不能用 `str.isspace()`** 判豁免：Python 认为 `\x1c`-`\x1f`（文件/组/记录/单元分隔符）是 whitespace，但正则的 `\s` **不认为**——于是它们若被豁免就会留到最终结果里，成为不可见的垃圾字符。

   **所以「豁免集合必须与第 3 步的正则完全一致」，而不能用语言内置的 isspace 语义。** 这是一处「两个相似 API 的语义差」导致的必须显式列表。

4. **折空白放在最后** —— 注释：「Collapse LAST so the spaces introduced above (→, kept whitespace controls, and any gap left around a removed control char) all fold into one space」。即第 1 步引入的空格与第 2 步留下的空隙都要被折掉。

**一个具体的例子**：`"第1章\x00 →\t第2节"` 会变成
- 第 1 步：`"第1章\x00  \t第2节"`（`→` 变空格）
- 第 2 步：`"第1章  \t第2节"`（`\x00` 是 `Cc` 被剥）
- 第 3 步：`"第1章 第2节"`
- `strip`：`"第1章 第2节"`

注意 **CJK 之间不插空格**（`"第1章"` 内部原样），所以对中文标题安全。

**`_truncate_heading_level` 与 `_clean_and_cap_headings` 的三步顺序。**

```python
def _truncate_heading_level(text: str, max_chars: int) -> str:
    if max_chars <= 0 or len(text) <= max_chars:
        return text
    return text[: max_chars - 1].rstrip() + "…"
```

三处：

1. **`max_chars <= 0` 表示禁用截断** —— 这是文档明确说的（「set `<= 0` to disable」）。所以 0 与负数都是「不限」而不是「截到 0」。
2. **`text[: max_chars - 1]` 预留一个字符给省略号** —— 所以结果长度恰为 `max_chars`。这个细节容易被漏（写成 `[:max_chars] + "…"` 会超一个）。
3. **`.rstrip()` 在加省略号之前** —— 防止 `"abcde "` 截成 `"abc "` 再变 `"abc …"`。

```python
def _clean_and_cap_headings(headings: list[str], max_heading_len: int) -> list[str]:
    return [_truncate_heading_level(c, max_heading_len)
            for c in (_clean_heading_text(h) for h in headings) if c]
```

**三步顺序被注释明确固定**：「Order matters: clean → drop empties → cap」。

为什么顺序重要？因为**清洗可能把标题变成空**（全是控制字符），而截断不会。所以必须先清洗再判空——否则一个「全零宽字符」的标题会过掉判空、截断后仍是零宽字符、最后进面包屑成为不可见的一层。

而 `if c` 在生成器里（外层推导的过滤），所以它过滤的是**清洗后**的结果。

### 两个面包屑渲染函数

**`format_parent_headings` 与 `format_heading_context` 的唯一差别是「要不要加自己」。**

```python
def format_parent_headings(dp, *, max_heading_len=DEFAULT_HEADING_LEVEL_MAX_CHARS) -> str:
    normalized = normalize_chunk_heading(dp)
    if not normalized:
        return ""
    cleaned = _clean_and_cap_headings(normalized["parent_headings"], max_heading_len)
    return HEADING_BREADCRUMB_SEP.join(cleaned)

def format_heading_context(dp, *, max_heading_len=DEFAULT_HEADING_LEVEL_MAX_CHARS) -> str:
    normalized = normalize_chunk_heading(dp)
    if not normalized:
        return ""
    chain = list(normalized["parent_headings"])
    if normalized["heading"]:
        chain.append(normalized["heading"])
    cleaned = _clean_and_cap_headings(chain, max_heading_len)
    return HEADING_BREADCRUMB_SEP.join(cleaned)
```

四处：

1. **`HEADING_BREADCRUMB_SEP = " → "` 被导出**，注释说明了理由：「Shared so the extraction-side token budgeter (see `operate._truncate_section_context`) can split the rendered breadcrumb back into levels without a drifting magic string」。

   **这是「渲染与解析共享同一个常量」的正确做法**：字符串常量定义一处，渲染方与拆分方都引用它，改一处两边同步。若各写字面量，改一个就会出现「渲染用 ` → ` 而拆分用 `→ `」这种静默错位。

2. **`format_parent_headings` 是查询期用的**（只要祖先链），`format_heading_context` 是抽取期用的（要完整面包屑，让 LLM 知道这段文本属于哪一节）。

3. **两个函数都返回 `""` 而不是 `None`** —— 所以调用方可以 `if breadcrumb:` 直接判空。

4. **`if normalized["heading"]:` 的守卫** —— 空标题不追加。所以一个「有 `level` 无文本」的块（前面提过的情形）在这里得到「只有父链」的结果——**这恰好是对的**：`format_heading_context` 退化成 `format_parent_headings`。

   **这是那处「`normalize` 返回非 `None` 但无内容」不一致被兜住的地方。**

**`max_heading_len` 的默认值来自常量 `DEFAULT_HEADING_LEVEL_MAX_CHARS`**，注释强调「one runaway title cannot bloat the query context before its token truncation」。

即：**每个层级都截**，而不是整体截。这个区别很实际——整体截会把深层标题全砍掉，而逐层截保留了层级结构与每层的可辨识前缀。

### `sidecar` 归一化

**`normalize_chunk_sidecar` 的核心是「`refs` 永远非空」。**

```python
_SIDECAR_TYPES = frozenset({"block", "drawing", "table", "equation"})

...
refs_raw = sidecar.get("refs")
refs: list[dict[str, str]] = []
if isinstance(refs_raw, list):
    for entry in refs_raw:
        if not isinstance(entry, dict):
            continue
        ref_type = str(entry.get("type") or "").strip()
        ref_id = str(entry.get("id") or "").strip()
        if ref_type in _SIDECAR_TYPES and ref_id:
            refs.append({"type": ref_type, "id": ref_id})
if not refs:
    refs = [{"type": sidecar_type, "id": sidecar_id}]
```

五处：

1. **四个允许的 type** —— 与 c04 里 `"P"` 只产出 `block` 不同，这里允许 `drawing`/`table`/`equation`，因为多模态链路的其他分块器可能产出它们。

2. **主 `id` 与 `type` 必须合法**（`if sidecar_type not in _SIDECAR_TYPES or not sidecar_id: return None`），否则整个 sidecar 被判无效。

3. **`refs` 里的每一项单独校验，非法项被跳过**（`continue`）而不让整个 sidecar 失效。所以部分畸形的 `refs` 会保留合法的那些。

4. **`if not refs:` 时用主 id 造一条** —— 这是文档强调的行为：「`refs` is always materialized as a list with at least the primary id. Single-source chunks therefore land in storage with `refs=[{type,id}]` so downstream consumers don't need to special-case the field's presence」。

   **所以「规范化」在这里的语义是把「可选」变成「必有」**。代价是存储里多了一份冗余（`id` 与 `refs[0].id` 相同），换来下游零判空。

5. **注意 `refs` 为空列表时也会被填充** —— 不只是「缺失」。所以 `refs: []` 与 `refs: None` 等价。

### 抽取用的标记剥离

**四个正则，每类元素的保留策略不同。**

```python
_CITE_RE = re.compile(r"<cite\b[^>]*>(.*?)</cite>", flags=re.IGNORECASE | re.DOTALL)
_CITE_REFID_ATTR_RE = re.compile(r'\s+refid\s*=\s*"[^"]*"', flags=re.IGNORECASE)
_DRAWING_RE = re.compile(r"<drawing\b([^>]*)/>", flags=re.IGNORECASE)
_EQUATION_RE = re.compile(r"<equation\b([^>]*)>(.*?)</equation>", flags=re.IGNORECASE | re.DOTALL)
_TABLE_RE = re.compile(r"<table\b([^>]*)>(.*?)</table>", flags=re.IGNORECASE | re.DOTALL)
```

三处：

1. **`DOTALL` 只用在容器标签上**（`cite`/`equation`/`table`），因为它们的正文可能跨行。`drawing` 是自闭合的，不需要。

2. **`_CITE_REFID_ATTR_RE` 要求前置空白**（`\s+refid`）—— 注释说明理由：「so the surrounding attribute layout (e.g. `type="table"`) stays intact」。若不要求前置空白，`refid` 可能与上一个属性粘连。

3. **`drawing` 的属性组是 `([^>]*)` 而自闭合用 `/>`** —— 所以它必须严格匹配 `<drawing ... />` 形态。若解析器产出 `<drawing ...>`（不带斜杠），这个正则不匹配，标签会原样留下。

**三类元素各自保留什么，构成一张策略表。**

| 元素 | 保留 | 丢弃 | 无 caption 时 |
|---|---|---|---|
| `<drawing>` | `caption` | `id`/`path`/`src`/`format` | **整个标签删掉** |
| `<table>` | `format`/`caption`/正文 | `id` | 保留（删 `id`） |
| `<equation>` | `format`/`caption`/正文 | `id` | 保留（删 `id`） |
| `<cite>` | 可见文本 | 整个包装（默认） | — |

```python
def _replace_drawing(match):
    attrs = _attrs_to_dict(match.group(1))
    caption = attrs.get("caption", "")
    if not caption.strip():
        return ""
    return f"<drawing{_format_attrs([('caption', caption)])} />"
```

**`drawing` 无 caption 时整个删掉** —— 理由在注释里：「Tags without any caption are removed entirely so they don't pollute extraction input」。一张没有标题的图片对文本抽取毫无信息，留着只是个空标签。

而 `table`/`equation` 保留正文，所以即便无 caption 也有信息（表格的行数据、公式的 latex）。

**`_attrs_to_dict` 与 `_format_attrs` 的分工暴露了一处信息损失。**

```python
_ATTR_RE = re.compile(r'(\w+)\s*=\s*"((?:[^"\\]|\\.)*)"')

def _attrs_to_dict(attr_string: str) -> dict[str, str]:
    return {match.group(1).lower(): match.group(2) for match in _ATTR_RE.finditer(attr_string)}

def _format_attrs(pairs: list[tuple[str, str]]) -> str:
    return "".join(f' {k}="{v}"' for k, v in pairs if v)
```

三处：

1. **`_attrs_to_dict` 把键小写** —— 所以 `Caption="x"` 与 `caption="x"` 等价。但**输出时用小写**，所以原大小写丢失（无害）。

2. **`_format_attrs` 过滤空值**（`if v`）—— 所以 `caption=""` 不会输出 `caption=""`。这与 `_replace_drawing` 的 `if not caption.strip()` 配合：先判空、再过滤。

3. **字典推导会丢重复键** —— 一个标签若有 `id="a" id="b"`（畸形），只保留后者。这是无害的容错。

   而 `_format_attrs` 接的是 `list[tuple]` 而非 dict，**所以调用方能控制顺序**（`format` 在 `caption` 前）。若用 dict 输出，顺序会是插入序——两者实际一致，但 list 让意图显式。

**`_ATTR_RE` 的转义处理：「complex quoting is rare in parser output」。**

```python
r'(\w+)\s*=\s*"((?:[^"\\]|\\.)*)"'
```

`((?:[^"\\]|\\.)*)` 匹配「非引号非反斜杠的任何字符」或「反斜杠+任意字符」。所以它**支持转义引号**（`\"`）但**不还原转义**——`_attrs_to_dict` 拿到的值里仍带 `\"`，而 `_format_attrs` 原样写回。所以往返是自洽的（不重复转义），但下游若要用 caption 的真实值需要自己反转义。

**`strip_internal_multimodal_markup_for_extraction` 的两个模式与顺序。**

```python
if not content:
    return content
if keep_cite_tag:
    cleaned = _CITE_REFID_ATTR_RE.sub("", content)
else:
    cleaned = _CITE_RE.sub(lambda m: m.group(1), content)
cleaned = _DRAWING_RE.sub(_replace_drawing, cleaned)
cleaned = _TABLE_RE.sub(_replace_table, cleaned)
cleaned = _EQUATION_RE.sub(_replace_equation, cleaned)
return cleaned
```

四处：

1. **`keep_cite_tag` 的两支做的事完全不同**：默认（抽取路径）**删整个 wrapper 只留文本**；`True`（多模态分析路径）**只删 `refid` 属性**，保留 wrapper。

   注释解释了为什么抽取路径要删 wrapper：「so the extractor does not surface it as a noisy structural entity」。而 `True` 路径要保 wrapper 是因为「the VLM/LLM can tell visible reference labels (e.g. "Table 1") apart from inline prose」。

   **同一个 `<cite>` 在两个消费者眼里价值相反**：抽取器觉得它是噪声（抽出一个叫「Table 1」的实体毫无意义），而多模态分析器需要它来区分「这是图注」与「这是正文里的引用」。

2. **顺序是 `cite` → `drawing` → `table` → `equation`**。`cite` 必须最先，因为它可能在别的元素里？实际不会（`cite` 是行内引用）。这里的顺序大概只是随意的——但**有一个隐含约束**：若 `<cite>` 里包含 `<table>`（不太可能），先剥 cite 会让 table 的 `refid` 已经消失，于是 `_replace_table` 拿不到 `id` 可以删（无害）。

3. **`_CITE_RE.sub(lambda m: m.group(1), content)`** —— 用函数而不是 `r"\1"`。因为 `m.group(1)` 就是可见文本，避免 `\1` 与后续反斜杠的转义问题。

4. **`if not content: return content`** —— 空串原样返回（`""` 而不是 `None`）。这个早退避免后面的四次 `sub` 白跑。

**注意这里没有处理「未闭合标签」**：一个只有开头 `<table ...>` 而没有 `</table>` 的内容，四个 `sub` 都不匹配，所以原样返回——**内部的 `id` 会漏进 prompt**。这是正则方案的固有局限（无法解析不完整 XML）。

## 关键取舍

**源码事实：** `normalize_chunk_heading` 靠 `isinstance(dp.get("heading"), dict)` 分流两代格式；`parent_headings` 的非 list 输入被当作「无父标题」；空项被丢弃；`int(level_raw or 0)` 包 `try`（畸形得 0，浮点被截断）。

**源码事实：** 「全空返回 `None`」的条件是三者同时为空，所以 `level != 0` 但无标题文本的块返回**非 `None` 的空结构**——与文档「Empty / missing inputs collapse to `None`」有细微差别。

**源码事实：** `_clean_heading_text` 四步顺序固定：`→` 换空格 → 剥 `Cc`/`Cf` → 折空白 → `strip`。第一步防伪造（`→` 是面包屑分隔符，标题内部含它会凭空多一层）；剥类别覆盖 NUL/零宽标记/方向控制符。

**源码事实：** `_HEADING_KEEP_WS = frozenset("\t\n\r\f\v")` 必须显式列出，且**不能用 `str.isspace()`**——因为 Python 认为 `\x1c`-`\x1f` 是 whitespace 而正则 `\s` 不认为，用 `isspace` 判豁免会让这些字符存活到最终结果。

**源码事实：** `_truncate_heading_level` 用 `text[:max_chars-1].rstrip() + "…"` 保证结果长度恰为 `max_chars`；`max_chars <= 0` 表示**禁用截断**（不是「截到 0」）。

**源码事实：** `_clean_and_cap_headings` 的三步顺序被注释固定为「clean → drop empties → cap」，因为清洗可能把标题变成空而截断不会。

**源码事实：** `HEADING_BREADCRUMB_SEP = " → "` 被导出，让渲染方（本模块）与拆分方（`operate._truncate_section_context`）共享同一常量，避免魔法字符串漂移。

**源码事实：** `format_parent_headings`（查询期，只要祖先链）与 `format_heading_context`（抽取期，祖先 + 自身）只在「要不要追加 `heading`」上不同，且都返回 `""` 而非 `None`；每个层级单独截断（而非整体截），保留层级结构与每层可辨识前缀。

**源码事实：** `normalize_chunk_sidecar` 要求 `type ∈ {block, drawing, table, equation}` 且 `id` 非空，否则整个 sidecar 访回 `None`；`refs` 里非法项**逐条跳过**而不让整体失效；`refs` 缺失或为空时**造出 `[{"type": sidecar_type, "id": sidecar_id}]`**，使下游无需判空（代价是存储里 `id` 与 `refs[0].id` 冗余）。

**源码事实：** `strip_internal_multimodal_markup_for_extraction` 的保留策略按元素分：`drawing` 只留 `caption`（**无 caption 时整个标签删掉**）、`table`/`equation` 留 `format`+`caption`+正文（无 caption 也保留）、`cite` 默认**删整个 wrapper 只留可见文本**而 `keep_cite_tag=True` 时**只删 `refid` 属性**。

**源码事实：** `<cite>` 在两个消费者眼里价值相反：抽取路径删 wrapper 以免抽出「Table 1」这类噪声实体，多模态分析路径保 wrapper 以区分图注与正文。文档明确要求「callers must NOT mutate the stored chunk `content`」——引用回链要靠原 id。

**源码事实：** `_CITE_REFID_ATTR_RE` 要求前置空白（`\s+refid`）以保持相邻属性布局；`_DRAWING_RE` 严格匹配 `/>`（解析器若产出不带斜杠的开标签则不匹配）；`_ATTR_RE` 支持 `\"` 转义但不还原，往返自洽。

**源码事实：** 未闭合标签（有 `<table ...>` 无 `</table>`）不会被任何 `sub` 匹配，所以内部 `id` 会漏进 prompt——正则方案的固有局限。

**架构推论：** 这三件事被放在一个模块里，共同点是**「两份消费者必须看到同一份解释」**。标题：查询期与抽取期都要读；sidecar：存储层与缓存清理都要遍历；markup：抽取器与多模态分析器都要看同一段文本的不同视图。把规范化收在一处，是把「一致性」从约定变成结构——只要两边都调这个函数，就不可能出现「查询期按扁平格式读、抽取期按嵌套格式读」这种错位。

**架构推论：** 「清洗 → 判空 → 截断」这个顺序，以及 `_HEADING_KEEP_WS` 那个必须显式列出的集合，都是同一类问题的两面：**字符级处理里每一步都可能改变后续步骤的判据**。若先截断再清洗，一个「全零宽 + 长文本」的标题会被截成一个看起来正常但全是不可见字符的串。若用 `isspace` 判豁免，`\x1c` 会漏网。所以这类代码的正确性无法靠「看起来对」保证，只能靠把每一步的意图写进注释——这份实现做到了。

**架构推论：** `refs` 从可选变必有，是「用冗余换下游简洁」的典型。它的代价是存储里 `id` 与 `refs[0].id` 重复（一次写入的空间、一次读取的歧义：若两者不一致该信谁？）。源码没有处理这个歧义——`normalize_chunk_sidecar` 直接用主 `id` 覆盖了 `refs` 缺失的情形，但若 `refs` 非空且第一条 `id` 与主 `id` 不同，它**原样保留两者**。所以「不一致」是可能的，且没有校验。

**架构推论：** `cite` 的两套处理揭示了一个更普遍的现象：**同一个语法元素对不同消费者有相反的价值**。这不是可以调和的，只能让消费者各自选择视图。`keep_cite_tag` 这个开关的存在，说明框架承认了「一份内容、多个视图」的需要，而不是强行统一。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/chunk_schema.py` | `normalize_chunk_heading` | 两代标题格式压成同一嵌套结构 |
| `lightrag/chunk_schema.py` | `_HEADING_WHITESPACE_RE` | 空白折叠正则 |
| `lightrag/chunk_schema.py` | `_HEADING_STRIP_CATEGORIES` | 剥除的 Unicode 类别（`Cc` / `Cf`） |
| `lightrag/chunk_schema.py` | `_HEADING_KEEP_WS` | 必须豁免的空白控制符集合 |
| `lightrag/chunk_schema.py` | `_clean_heading_text` | 四步清洗（防 `→` 伪造、剥类别、折空白、strip） |
| `lightrag/chunk_schema.py` | `_truncate_heading_level` | 单层硬截断（预留省略号位） |
| `lightrag/chunk_schema.py` | `_clean_and_cap_headings` | clean → drop empties → cap |
| `lightrag/chunk_schema.py` | `HEADING_BREADCRUMB_SEP` | 渲染与拆分共享的分隔符常量 |
| `lightrag/chunk_schema.py` | `format_parent_headings` | 查询期面包屑（仅祖先链） |
| `lightrag/chunk_schema.py` | `format_heading_context` | 抽取期面包屑（祖先 + 自身） |
| `lightrag/chunk_schema.py` | `_SIDECAR_TYPES` | 四种合法 sidecar 类型 |
| `lightrag/chunk_schema.py` | `normalize_chunk_sidecar` | 校验并保证 `refs` 非空 |
| `lightrag/chunk_schema.py` | `_CITE_RE` / `_CITE_REFID_ATTR_RE` | `<cite>` 的两种剥离形态 |
| `lightrag/chunk_schema.py` | `_DRAWING_RE` / `_TABLE_RE` / `_EQUATION_RE` | 三种元素的标签正则 |
| `lightrag/chunk_schema.py` | `_attrs_to_dict` / `_format_attrs` | 属性解析与重建（键小写、空值过滤） |
| `lightrag/chunk_schema.py` | `_replace_drawing` | 只留 caption，无 caption 则整删 |
| `lightrag/chunk_schema.py` | `_replace_table` / `_replace_equation` | 删 `id`，留 `format`/`caption`/正文 |
| `lightrag/chunk_schema.py` | `strip_internal_multimodal_markup_for_extraction` | 两条路径（抽取 / 多模态）的标记剥离 |

## 参考

- [lightrag 源码：chunk_schema.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/chunk_schema.py)
- [Unicode 标准：通用类别 Cc 与 Cf](https://www.unicode.org/reports/tr44/#General_Category_Values)
- [Python 文档：unicodedata.category](https://docs.python.org/3/library/unicodedata.html#unicodedata.category)
- [Python 文档：re 模块的 DOTALL 与转义](https://docs.python.org/3/library/re.html)

## 与主核 / AI 的关系

标题面包屑与主核（多模态数据库内核）的**命名空间路径**（`schema.table.column`）是同一类结构：一个多级限定名用于把对象放回它的上下文里。主核的路径是 schema 强制的（每级必须是已存在的对象），而这里的标题链来自解析器、**任何一级都可能缺失或为空**。所以主核可以直接拼接，这里必须先清洗、判空、截断。

**而 `→` 防伪造那条与主核的 `quote_identifier` 是同一个问题**：一个值里含有分隔符，若不转义/剥除，反解析时就会多出一级。主核用双引号包裹标识符来消除歧义，这里选择剥掉分隔符——因为标题是自然语言，包裹引号会很难看。**两种解法都成立，判据是「分隔符在值里出现的可能性与消除成本」**。

第二处对照是 `refs` 的「可选变必有」，与主核的**系统列**（`ctid`/`xmin`/`xmax`）思路一致：系统列总是存在，所以查询不需要判空。主核的代价是每行多几个字段；这里的代价是 `id` 与 `refs[0].id` 冗余。

第三处是 markup 剥离与主核的**`pg_dump` 输出参数**（`--no-comments`、`--no-privileges` 之类）。两者都是「同一份数据在不同消费者面前要有不同视图」：`pg_dump` 的消费者可能是另一个数据库或人，所以要能剥掉内部信息；这里的消费者是 LLM，所以要剥掉解析器 id。**共同点是「视图切换必须是无损的」**——原数据保留，只改送给谁看什么。

对 AI 工程的实际意义在于：块结构是分块与抽取之间的契约，而它最容易被忽略的正是「规范化」这一层。这份实现给出的三条经验可直接用：**两代格式要在一个函数里归一**（否则调用方各写一套就会漂移）、**渲染与解析要共享同一个分隔符常量**（否则改一处就静默错位）、以及**内部标识符必须剥掉再给 LLM**（否则它们会变成图谱里的噪声实体）。
