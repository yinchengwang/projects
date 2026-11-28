---
project: lightrag
feature: table-markup
title: 表格标记与 HTML 行拆分
page_type: feature
schema_version: 1
refs:
  - path: lightrag/table_markup.py
    symbols: [parse_table_tag, detect_table_format, serialize_html_rows]
  - path: lightrag/chunker/_html_rowspan.py
    symbols: [RowspanSlicer, build_rowspan_slicer, _header_row_count]
---

# 表格标记与 HTML 行拆分

## 一句话职责

把 `<table id="…" format="…">…</table>` 这一种标签当成一种**可切分的结构**来处理：先能可靠地识别它、判断 body 是 JSON 还是 HTML、把 HTML 表按 `<tr>` 连同 `<thead>`/`<tbody>`/`<tfoot>` 包装一起枚举出来并能重新拼回去；再解决按行切开后唯一真正的难点——**`rowspan` 单元格跨了切分边界时，切出的每一片都必须自己补齐被合并的内容**。

## 机制是什么

两个文件的分工很清晰：

- `table_markup.py`（227 行）是**共享原语**：正则表达式、格式判定、行枚举、行重序列化。模块 docstring 说明了它存在的理由：「这两个调用点都需要识别 post-rewrite 的 `<table id="…" format="…">…</table>`、判断 body 是 JSON 还是 HTML、枚举行级单位（JSON 列表项或 HTML `<tr>` 连同它们的包装）、并重新序列化一个行子集而保留结构包装。**把这些正则与辅助函数放在一处，避免任一消费者演进时出现细微漂移**」。
- `chunker/_html_rowspan.py`（340 行）是**一个问题的完整解**：`rowspan` 跨切分边界的补齐。它自己有一份严格得多的 HTML 语法，而不是复用那两个宽松正则。

所以「为什么同一个模块里有两套 HTML 正则」这个问题有明确答案：**枚举行只需要宽松（认得 `<tr>` 就够）；而要移动单元格就必须严格（必须知道每个单元格精确的列位置与跨行范围）**。宽松正则足以「切」，不足以保证「切完还对齐」。

`rowspan` 问题的本质写在模块 docstring 的第一段：

> 行切分在一对 `<tr>` 之间切开表，但一个 `rowspan="N"` 的单元格覆盖了后面 `N - 1` 行。原样切开，**这个单元格声称覆盖它这一片并不持有的行，而下一片的前几行那个列上就没有单元格，于是它们的值在表头下整体左移一列**。

所以症状是「值错位」而不是「数据丢失」——这比丢数据更难发现，因为表看起来仍然结构完整、行数也对。

## 怎么实现的

**`TABLE_TAG_RE` 的非贪婪匹配带一个硬性前置条件，而注释把它写成了写者的义务。**

```python
TABLE_TAG_RE = re.compile(r"<table\s+(?P<attrs>[^>]*)>(?P<body>.*?)</table>", re.DOTALL)
```

注释：

> body 是非贪婪匹配的，正如这个标签的其他每个读者所做，**所以 body 绝不能持有文本 `</table>`**。一个 JSON body 通过下面的 `dumps_json_table_body` 守住这个承诺；**不要用别的任何方式写它**。

这是一个「格式不变量由写者保证」的例子：解析器无法区分「body 内部的 `</table>`」与「真正的结束」，所以它假设不存在，并把责任推给写者。而为了让这个假设可被写者满足，配套给了 `escape_json_table_body`。

`escape_json_table_body` 的机制很精巧：

```python
def escape_json_table_body(body: str) -> str:
    return _CLOSING_TABLE_TAG_RE.sub(r"<\\\1", body)
```

把每个 `</table>`（任意大小写）写成 `<\/table>`。docstring 解释为什么这在 JSON 里是安全的：「在合法 JSON 里那段文本只能出现在字符串内，而那里 `\/` 会解码回 `/`，所以 `json.loads` 返回同一个值。**幂等**」。

而它明确划出了「不要多转义」的边界：

> 别的什么都**不要**转义。单元格常规地持有配平的内部标记（`<sup>…</sup>`、`<equation>…</equation>`），而其他扫描器会在整个块上配对它们；**重写那些闭合标签会让开启标签去配一个后来的、真正的标签**。

所以转义范围必须精确等于「会让外层解析出错的那一个模式」。多转义一个 `<` 就会破坏内部标记的配对——这与 g03 里 `_sanitize_record_value` 的原则完全一样：**净化作用域必须精确等于失败集合**。

**`_TABLE_FORMAT_RE` 与 `_TABLE_ID_RE` 的负向后瞻拦的是「长得像但不是」的属性名。**

```python
_TABLE_FORMAT_RE = re.compile(r"""(?<![\w-])format\s*=\s*["'](?P<fmt>[^"']+)["']""")
_TABLE_ID_RE = re.compile(r"""(?<![\w-])id\s*=\s*["'](?P<id>[^"']+)["']""")
```

`(?<![\w-])` 排除了两种前缀：纯词字符（`gridformat`、`valid`）与连字符连接的（`data-format`、`data-id`）。注释里逐一点名：「负向后瞻拒绝只作为另一个属性名尾部的 `id`——既包括词字符前缀（`grid`、`valid`），也包括连字符连接的（`data-id`）——所以只有独立的 `id` 属性会命中」。

这个问题在没有负向后瞻时会静默出错：`data-id="x"` 会被读成 `id="x"`，于是表格被追溯到错误的 sidecar 条目。

**`detect_table_format` 是「先看显式属性，再看 body」的两级判定，而它的返回类型是三值。**

```python
match = _TABLE_FORMAT_RE.search(attrs or "")
if match:
    fmt = match.group("fmt").strip().lower()
    if fmt in {"json", "html"}: return fmt
    return None
stripped = (body or "").lstrip()
if stripped.startswith(("[", "{")): return "json"
if "<tr" in stripped.lower(): return "html"
return None
```

三点：

1. **显式属性一旦出现就独占判定权**——若属性写了 `format="csv"`（不在两个合法值里），直接返回 `None` 而**不去嗅探 body**。这是「显式声明优先于推断」的正确写法：写者明确声明了一个不支持的格式，猜它是 `json` 只会让错误延后。
2. **JSON 靠首字符判**（`[` 或 `{`，先 `lstrip` 掉空白）。
3. **HTML 靠 `"<tr" in ...lower()`** ——注意是 `in` 不是 `startswith`，因为 HTML 表 body 通常以空白或 `<thead>` 开头。

而 `None` 的语义在 docstring 里写明：「其他任何情况都是未知，**调用方应退回字符切分**」。

**`split_html_rows` 的核心是「用一次正则同时扫描包装标签与行」，这是它能正确归属的前提。**

```python
HTML_ROW_PARTS_RE = re.compile(
    r"(?P<wrap></?(?:thead|tbody|tfoot)\b[^>]*>)" r"|(?P<tr><tr\b[^>]*>.*?</tr>)",
    re.DOTALL | re.IGNORECASE,
)
```

一个交替正则，两个命名组。于是 `finditer` 按**文本顺序**产出包装标签和行，而循环维护一个 `current_wrapper` 状态：

```python
if wrap is not None:
    tag = HTML_WRAPPER_TAG_RE.match(wrap)
    if tag:
        slash = tag.group("slash")
        name = tag.group("name").lower()
        if slash == "/":
            if current_wrapper == name: current_wrapper = ""
        else: current_wrapper = name
elif tr is not None:
    rows.append((current_wrapper, tr))
```

闭合标签的处理有一个条件：`if current_wrapper == name` ——只有**名字匹配**时才清空。为什么需要这个条件？因为嵌套或错配的闭合标签不该清掉当前状态；`if current_wrapper == name` 让一个孤立的 `</tbody>` 不产生副作用。

而它把自己能力边界写得很清楚：

> 空白、caption、注释、`<colgroup>` 以及行包装之外的任何其他文本**被丢弃**——这是一个正则抽取器，不是完整 DOM 解析器。**包装属性（例如 `<thead class="…">`）在重新发出时也被丢弃**；分块输出使用裸包装标签。

所以「有损」是被声明并接受的：分块产物不需要保留 class/style，因为那些对检索与生成无用。

而返回 `None` 的条件是「一行都没找到」，docstring：「`None` 表示『没找到行』，好让调用方退到字符切分」。

**`serialize_html_rows` 是 `split_html_rows` 的逆，而它的算法是「连续同包装名合并成一个块」。**

```python
current_wrapper = ""
for wrapper, tr in rows:
    if wrapper != current_wrapper:
        if current_wrapper: parts.append(f"</{current_wrapper}>")
        if wrapper: parts.append(f"<{wrapper}>")
        current_wrapper = wrapper
    parts.append(tr)
if current_wrapper: parts.append(f"</{current_wrapper}>")
```

所以它只做状态迁移，不做分组——因为 `split_html_rows` 输出的序列本身就是按文本顺序的，所以「连续的相同包装名」自然会合并。这也意味着**它可以接受任意子集**（这正是切分需要的）：只要传入的行序列保持原相对顺序，重新拼出的 HTML 就是合法的。

而「`""`（无包装）发出裸 `<tr>…</tr>`」这一条让混合情况也能处理：表 body 里既有无包装行也有 `<tbody>` 包裹的行时，输出会正确地在两者之间开关标签。

**现在进入 `rowspan` 那个问题，而它的答案从一条极端的纪律开始：全有或全无。**

模块 docstring 的第二条规则：

> **每张表全有或全无。** 只有当一个表的整个 body 匹配下面的严格语法时才修复它。其他任何情况都**绝不部分解释**：builder 返回 `None`，调用方按原样切分这些行，这正是这个模块存在之前发生的事。**绝不添加按行或按单元格的兜底：从一张只被部分理解的表构建出来的布局会把一个单元格移到错误的列。**

这条纪律的理由值得展开：一个「部分理解」的布局会产出**看似合法但列错位**的结果，而列错位比不修复更糟——不修复至少是「已知的不对齐」，部分修复是「未知的不对齐」。所以宁可全部放弃。

而第三条与第四条规则补充了另外两个约束：

- **语法是封闭的**：「新的 HTML 形式通过扩展语法与它的测试来支持，**不是通过在扫描器里容忍它们**」。所以不支持的情况必须显式报 `_Unsupported` 而不是尽力解析。
- **工作有界**：「布局在 `LAYOUT_BUDGET` 步后停止，所以一个敌意表格无法让它时间或内存上变成平方级」。`LAYOUT_BUDGET = 200_000`。
- **重复的单元格要花 token**：「一个重复的单元格花掉行切分器从未计数的 token。**调用方必须对每个序列化后的切片重新检查它的上限**」。

最后一条是一个跨组件的责任移交：这个模块让切片变长了，所以它必须提醒上游重新校验。

**严格语法的第一层是「HTML 的空白集合与 Python 的 `\s` 不同」。**

```python
_WS = " \t\n\f\r"
_W = r"[ \t\n\f\r]"
```

注释：「HTML 的 ASCII 空白。**Python 的 `\s` 也匹配 U+00A0 这类字符，而 HTML 把 U+00A0 读作属性名的一部分，所以这里从不使用它**」。

所以 `_WS` 是显式的字符集而不是 `\s`。这是一个具体的正确性差异：一个含不换行空格的属性会被 HTML 解析器当成名字的一部分，而 `\s` 会在那里切分，导致属性名解析错位。

同一段还有：

```python
_FLAGS = re.DOTALL | re.IGNORECASE | re.ASCII
```

`re.ASCII` 让 `\w` 等简写在 ASCII 范围生效，与 HTML 的行为对齐。

**`_ATTRS` 的构造解决了一个经典问题：属性值里的 `>` 不该结束标签。**

```python
_ATTRS = (
    rf"""(?:{_W}+{_NAME}(?:{_W}*={_W}*"""
    rf"""(?:"[^"]*"|'[^']*'|[^ \t\n\f\r"'=<>`]+))?)*{_W}*"""
)
```

注释：「一个开始标签的属性：带引号的值被整段消费，所以其中的 `>` 不会结束标签」。

对比一下 `table_markup.py` 里那个宽松的 `[^>]*` —— 后者遇到 `<td title="a>b">` 就会在 `>` 处截断。**这就是「宽松解析器只能枚举、严格解析器才能重排」的具体区别。**

**`_check_cell_content` 是「证明这个单元格真的是单元格」的检查，而它的写法很有一套。**

```python
def _check_cell_content(inner: str) -> None:
    pos = inner.find("<")
    while pos != -1:
        following = inner[pos + 1 : pos + 2]
        if following.isascii() and (following.isalpha() or following == "/"):
            tag = _INNER_TAG_RE.match(inner, pos)
            if tag is None:
                raise _Unsupported("a tag inside a cell that cannot be read")
            name = tag.group("name").translate(_ASCII_LOWER)
            if name in _CELL_ENDING_TAGS:
                raise _Unsupported("table markup inside a cell")
            if name in _OPAQUE_TAGS:
                raise _Unsupported(f"a <{name}> element inside a cell")
            pos = inner.find("<", tag.end())
        elif following in ("!", "?"):
            raise _Unsupported("a declaration inside a cell")
        else:
            pos = inner.find("<", pos + 1)
```

四个分支：

1. **`<` 后面是字母或 `/`** → 是一个标签，用 `_INNER_TAG_RE` 尝试配对。配不上就报错。
2. **标签名在 `_CELL_ENDING_TAGS` 里**（`table thead tbody tfoot tr td th caption col colgroup`）→ 「单元格内有表格标记」→ 报错。理由：一个嵌套表或一个「省略了结束标签的单元格」都会让后续解析错位。
3. **标签名在 `_OPAQUE_TAGS` 里**（`script style textarea title xmp iframe noembed noframes noscript plaintext svg math template select`）→ 报错。注释解释这张表的来历：「HTML 不把其内容读作标记的元素（所以在它里面的 `</td>` 是文本），以及那些改变其后内容解析方式的元素。**第一行是切换 HTML tokenizer 状态的元素的完整列表**」。
4. **`<` 后面是 `!` 或 `?`** → 声明或处理指令 → 报错。
5. **其他** → 「一个不开启任何标签的 `<` 是文本」，跳过。

第 2 与第 3 条合起来在证明一件很强的事：**「结束这个单元格的那个 `</td>` 不在嵌套元素的带引号属性值里」**。因为若每个内部标签都被要求完整、且不允许表格标记与不透明元素，那么「单元格内容」与「单元格结束」之间的对应关系就是确定的。

而注释里那句「`_CELL_ENDING_TAGS` 也覆盖『结束标签被省略的单元格，下一个开始标签落在了它里面』」——HTML 允许省略 `</td>`，而那种情况下 `inner` 会吞掉后面的内容，于是在这里被拒绝。所以这个检查同时挡住了两种畸形。

**`_header_row_count` 做的是「验证 body 的形状」，而它的判据是一条正则签名。**

```python
_BODY_SHAPE_RE = re.compile(r"(?:Hr*h)?(?:r+|Br+b)")
_SHAPE_CODE = {"thead": "H", "tbody": "B", "tfoot": "F"}
```

它先把整个 body 里的行与包装按顺序编成一个签名串（`H` = `<thead>`、`h` = `</thead>`、`r` = 行、`B`/`b` = `<tbody>` 开关），然后要求匹配那条正则。

所以支持的形状是严格的：**可选的 `<thead>` 块 + body 行（裸行，或全部包在一个 `<tbody>` 里）**。而 `tfoot` 会编出 `F`，永远匹配不上 → 报错。

三条前置检查也很硬：

```python
if "<!--" in body: raise _Unsupported("an HTML comment")
if HTML_ROW_PARTS_RE.sub("", body).strip(_WS): raise _Unsupported("content outside the rows")
```

第一条拒绝任何 HTML 注释——因为注释里可能有 `</table>` 或看起来像标签的东西，而一个正则抽取器无法安全跳过。第二条把「行与包装之外的内容」找出来：用同一个正则**删掉**所有行与包装，剩下的非空白就是「别的东西」（caption、colgroup、裸文本），直接拒绝。

第二条的写法很巧：不是去枚举「允许什么」，而是「删掉已知的，看还剩什么」。这与 `_vectors_are_expected` 里「从枚举推导未完成状态」是同一种 fail-safe 方向。

而返回值：

```python
return signature.index("h") - 1 if "h" in signature else 0
```

`signature.index("h")` 是 `</thead>` 的位置，减 1 就是 `<thead>` 的行数（因为 `H` 在位置 0，行紧跟其后）。没有 `</thead>` 则 header 行数为 0。

**`_layout` 是整页最核心的算法：它算出「每一行的每列上有什么」。**

状态是 `active: dict[int, tuple[int, int, _Cell]]`（列 → (列, 覆盖到的最后一行, 单元格)）。逐行处理：

```python
for col in [c for c, span in active.items() if span[1] < row]:
    del active[col]
occupied: set[int] = set()
for col, _last, cell in active.values():
    work += 1 + cell.colspan
    if work > LAYOUT_BUDGET: raise _Unsupported("too many overlapping spans")
    occupied.update(range(col, col + cell.colspan))
carried.append(list(active.values()))
open_tag, cells, close_tag = _parse_row(tr)
```

四步：清掉已过期的 span、算出被占的列集合、**把 `active` 快照存进 `carried`**、解析本行。

然后为每个单元格找列位置：

```python
col = 0
for cell in cells:
    while col in occupied: col += 1
    if cell.colspan > 1 and occupied:
        work += cell.colspan
        if work > LAYOUT_BUDGET: raise _Unsupported("too many overlapping spans")
        if not occupied.isdisjoint(range(col + 1, col + cell.colspan)):
            raise _Unsupported("a cell overlaps a span from an earlier row")
    placed.append((col, cell))
    if cell.rowspan > 1:
        has_spans = True
        active[col] = (col, min(row + cell.rowspan - 1, last_row), cell)
    col += cell.colspan
```

关键在 `while col in occupied: col += 1` ——**单元格从左边填进第一个空位**。这正是 HTML 表格布局的实际规则。而 `colspan > 1` 时还要检查「它跨的范围内没有已占列」，否则就是真正的重叠（无法用「左移找空位」化解）。

而 `min(row + cell.rowspan - 1, last_row)` 把 `rowspan` 截到表的末尾——所以一个声称 `rowspan="100"` 但表只有 5 行的单元格被当成覆盖到第 5 行，而不是越界。

最后一处检查是「空洞」：

```python
if occupied and max(occupied) >= col:
    covered = len(occupied) + sum(cell.colspan for _col, cell in placed)
    if covered <= max(occupied):
        raise _Unsupported("an empty column left of a spanned cell")
```

注释的解释：「单元格从左填满空闲列，所以**当且仅当一个被携带的 span 落在本行自己最后一个单元格之外的列上**时这行才有空洞。一个被重复进这样一行的单元格会落到空洞的列上」。

所以这个检查的动机是「避免造出另一个错位」：如果一个 span 覆盖了第 5 列，而本行只有 3 个单元格填到第 3 列，那么第 4 列是空的、第 5 列有内容。此时往这行重复一个跨行单元格会落在哪一列？无法确定——所以放弃整张表。

**`RowspanSlicer.slice` 是最终产出，而它的逻辑短到可以逐行读。**

```python
for index in range(start, end):
    wrapper, tr = self._rows[index]
    row = index - self._header_rows
    if row < 0:
        out.append((wrapper, tr))
        continue
    open_tag, placed, close_tag = self._parsed[row]
    left = end - index                                  # 本片内从这行起还剩几行
    pieces = [(col, cell.with_rowspan(min(cell.rowspan, left))) for col, cell in placed]
    changed = any(cell.rowspan > left for _col, cell in placed)
    if index == start:
        for col, last, cell in self._carried[row]:
            pieces.append((col, cell.with_rowspan(min(last - row + 1, left))))
            changed = True
    if changed:
        pieces.sort(key=lambda piece: piece[0])
        tr = open_tag + "".join(html for _col, html in pieces) + close_tag
    out.append((wrapper, tr))
```

四件事：

1. **header 行原样返回**（`row < 0`）。所以 `<thead>` 不会被子片重排——它是每个切片共享的表头。
2. **跨行超过本片的格被裁剪**：`min(cell.rowspan, left)`。所以一片的最后一行上的 `rowspan="3"` 变成 `rowspan="1"`（或干脆去掉属性，见下）。
3. **只有切片的起始行补携带格**（`if index == start`）。这是正确性核心：一个从上一片跨过来的格只需要在**本片第一行**出现，因为它在后续行上已经由 `_layout` 的 `placed` 处理了。
4. **只在真的变了的时候重拼**（`changed`），否则原样返回 `tr`。所以「不需要修复的行」逐字不变——这也是 docstring 承诺的「`<thead>` 行与不需要改动的行原样返回」。

而 `pieces.sort(key=lambda piece: piece[0])` 是必需的：携带格是**追加**到 `pieces` 末尾的，但它们的列位置可能在本行已有单元格之间或之前。不排序就会让单元格顺序错乱，而顺序错了列位置就全错。

**`_Cell.with_rowspan` 的三行代码里有一个容易被忽略的细节。**

```python
def with_rowspan(self, rowspan: int) -> str:
    if rowspan == self.rowspan: return self.html
    span = f' rowspan="{rowspan}"' if rowspan > 1 else ""
    return f"<{self.tag}{self.attrs}{span}>{self.inner}</{self.tag}>"
```

三点：

1. **`if rowspan == self.rowspan: return self.html`** ——不变就返回原始字符串，保证「不需要改动的部分逐字不变」。
2. **`rowspan > 1` 才写属性** ——`rowspan="1"` 在 HTML 里等价于没有属性，所以省掉。这让「被裁剪到 1」的单元格退化成一个普通单元格。
3. **`self.attrs` 是「去掉 `rowspan` 之后的属性」** ——`_parse_cell` 里只把 `colspan` 留在 `kept` 里：

```python
if name not in _SPAN_LIMITS:
    kept.append(attr.group(0)); continue
if name in spans: raise _Unsupported(f"a cell repeats {name}")
spans[name] = _span_value(name, attr)
if name == "colspan": kept.append(attr.group(0))
```

所以 `colspan` 保留在 `attrs` 里，`rowspan` 被抽出去单独管理。这是必要的：`rowspan` 的值要重算，其余属性要原样保留。

**`_parse_cell` 里有一句关于复杂度的注释，值得单独看。**

```python
# ``attrs`` matched the grammar, so the attributes tile it from the start
# and only whitespace can follow them; scanning that tail is quadratic.
for attr in _ATTR_RE.finditer(attrs.rstrip(_WS)):
```

注释说的是「扫描那个尾部是平方级的」——所以它先 `rstrip` 掉尾随空白再 `finditer`。为什么原样会平方级？因为 `_ATTR_RE` 的模式以 `{_W}+`（一个或多个空白）开头，而一个长尾随空白上每个位置都会尝试匹配一次并失败，配合回溯就退化成平方。

而「属性从头铺满 `attrs`，只有空白能跟在后头」这个论证是正确性依据：因为 `_ATTRS` 的构造是严格的，所以 `attrs` 一定由属性序列加尾随空白构成，不存在中间的垃圾。

**`build_rowspan_slicer` 的入口判断是一个便宜的短路。**

```python
if not any(w != "thead" and "rowspan" in tr.lower() for w, tr in rows):
    return None
```

所以第一个过滤是**纯字符串检查**：body 行里有没有出现 `rowspan`。没有就直接返回 `None`，连严格解析都不做。这是必要的性能考虑——绝大多数表格没有合并单元格，而严格语法解析比 `in` 贵得多。

而它只在 body 行里找（`w != "thead"`），因为 header 行不会被裁剪。

然后：

```python
try:
    header_rows = _header_row_count(body, len(rows))
    parsed, carried, has_spans = _layout([tr for _w, tr in rows[header_rows:]])
except _Unsupported as exc:
    logger.warning(
        "Table %s: rowspan cells are not repaired across row slices (%s); "
        "a slice that starts inside a span may misalign its columns",
        table_label, exc,
    )
    return None
if not has_spans:
    return None
```

三件事：**头部行数先算**（因为 `_layout` 只用 body 行）、解析失败**只记一条警告并返回 `None`**（不抛，因为调用方有可靠的降级路径）、以及 `has_spans` 为假时也返回 `None`（有 `rowspan` 文字但没有真正跨行的格，此时无需修复）。

而那条警告消息把**后果**写进去了：「一个从 span 内部开始的切片可能让它的列错位」。所以运维看到这条日志时知道影响是什么，而不只是「修复被跳过了」。

**`_span_value` 里有一个防 `int` 溢出的细节。**

```python
if not (raw.isascii() and raw.isdecimal()):
    raise _Unsupported(f"{name} is not a positive integer")
limit = _SPAN_LIMITS[name]
value = int(raw) if len(raw) <= len(str(limit)) else limit + 1
if not 1 <= value <= limit:
    raise _Unsupported(f"{name} is outside 1..{_SPAN_LIMITS[name]}")
```

注释：「先比较位数：`int` 拒绝一个非常长的数字串」。所以 `len(raw) <= len(str(limit))` 是一道防 DoS 的守卫——`int("9"*10000)` 在 Python 3.11+ 会抛 `ValueError`（有位数上限），而在更早版本会真的算出一个巨大整数。用位数比较绕开两者。

而 `_SPAN_LIMITS = {"rowspan": 65534, "colspan": 1000}` 是 HTML 规范定义的最大值，注释写明「**一个更大的值不被夹紧——整张表留着不修复**」。所以超限不是「截断到最大值继续」，而是「放弃这张表」——与「全有或全无」一致。

**`_parse_row` 末尾那条检查是「行内除单元格之外不能有别的东西」。**

```python
while (match := _CELL_RE.match(inner, pos)) is not None:
    cells.append(_parse_cell(match)); pos = match.end()
if inner[pos:].strip(_WS):
    raise _Unsupported("row content that is not a complete cell")
```

所以文本节点（哪怕是空白之外的一个字）都会导致拒绝。而在 HTML 里 `<tr>` 内出现裸文本是合法的（虽然少见），所以这是「缩小支持范围换取确定性」的一个具体取舍。

注意 `strip(_WS)` 用的是 HTML 空白而不是 `str.strip()` —— 所以一个含 U+00A0 的「空白」会被当成真实内容而触发拒绝。这是保守侧的错误。

**`LAYOUT_BUDGET = 200_000` 的注释说明了它的量级依据。**

```python
# Upper bound on the layout's work for one table: spans carried into each row
# plus the columns they cover, summed over the rows. Tables with a few label
# columns stay far below it.
LAYOUT_BUDGET = 200_000
```

而 `work` 在两处累加：携带的 span（`work += 1 + cell.colspan`）与新的 `colspan` 格（`work += cell.colspan`）。超限就 `raise _Unsupported("too many overlapping spans")` —— 同样是「放弃整张表」。

所以这个预算把「一个恶意的行数×列数组合」限制在可控范围内。而它的设计是「累加实际处理的单元格数」，不是一个模糊的「行数上限」——因为真正的成本取决于重叠程度。

## 关键取舍

**源码事实：** `TABLE_TAG_RE` 的 body 非贪婪匹配，**要求 body 里绝不出现 `</table>`**；这个不变量由写者通过 `dumps_json_table_body` / `escape_json_table_body` 保证，docstring 明写「不要用别的任何方式写它」。

**源码事实：** `escape_json_table_body` 只把 `</table>` 写成 `<\/table>`（JSON 里安全且幂等），**其余一律不转义**，因为「单元格常规地持有配平的内部标记，重写那些闭合标签会让开启标签配到后来的真标签」。

**源码事实：** `_TABLE_FORMAT_RE` / `_TABLE_ID_RE` 用 `(?<![\w-])` 拒绝 `gridformat`、`valid`、`data-format`、`data-id` 这类「尾部像但整体不是」的属性名。

**源码事实：** `detect_table_format` 三值：显式属性一旦出现就独占判定（写 `format="csv"` 返回 `None` 而不去嗅探 body）；无属性时 JSON 看首字符、HTML 看是否含 `<tr`；`None` 表示调用方应退回字符切分。

**源码事实：** `split_html_rows` 用一个交替正则同时扫描包装标签与行，所以能按文本顺序正确归属；闭合标签只在**名字匹配**时清空状态。它明确是有损的：空白、caption、注释、`<colgroup>` 被丢弃，包装属性（如 `class`）也不保留。

**源码事实：** `serialize_html_rows` 只做状态迁移不做分组，所以能接受任意保序子集——这正是「切分后重拼」需要的性质。

**源码事实：** `rowspan` 修复的纪律是**每张表全有或全无**：body 不完整匹配严格语法就整表不修复（返回 `None`），「绝不添加按行或按单元格的兜底：从一张只被部分理解的表构建的布局会把单元格移到错误的列」。

**源码事实：** 严格语法用 HTML 的 ASCII 空白集（`" \t\n\f\r"`）而非 Python 的 `\s`，因为「Python 的 `\s` 也匹配 U+00A0，而 HTML 把 U+00A0 读作属性名的一部分」。

**源码事实：** `_ATTRS` 让带引号的属性值整段消费，所以 `>` 在属性值里不会结束标签——这是宽松正则（`[^>]*`）做不到的，也是「枚举行」与「重排单元格」需要两套解析器的根本原因。

**源码事实：** `_check_cell_content` 拒绝单元格内的表格标记与全部不透明元素（`script style textarea … svg math template select`），从而证明「结束这个单元格的 `</td>` 不在嵌套元素的带引号属性值里」；它也顺带挡住 HTML 允许的「省略 `</td>`」。

**源码事实：** `_header_row_count` 把 body 的形状编成签名并要求匹配 `(?:Hr*h)?(?:r+|Br+b)`，即「可选一个 `<thead>` + 裸行或单个 `<tbody>`」；含 HTML 注释、`tfoot`、或行外任何非空白内容都拒绝。

**源码事实：** `_layout` 里单元格「从左填进第一个空位」（`while col in occupied`），`rowspan` 被 `min(..., last_row)` 截到表末；`colspan` 跨到已占列则拒绝；行内出现「被携带 span 之外的列有空洞」也拒绝。

**源码事实：** `RowspanSlicer.slice` 只在切片**起始行**补携带格，且补完必须 `sort` 按列重排；header 行与「不需要改动的行」逐字返回原串。

**源码事实：** `_Cell.with_rowspan` 在值不变时返回原始 `html`，且 `rowspan <= 1` 时**省略该属性**（等价于无合并）；`colspan` 保留在 `attrs` 里而 `rowspan` 被抽出去单独管理。

**源码事实：** `_span_value` 先比较位数再 `int`，因为「`int` 拒绝一个非常长的数字串」；超限与 `_SPAN_LIMITS`（`rowspan` 65534、`colspan` 1000）之外的值**不被夹紧，而是整表不修复**。

**源码事实：** `LAYOUT_BUDGET = 200_000` 累加的是「携带 span 数 + 它们覆盖的列数 + 新 colspan 格的列数」之和，超限即放弃整表——预算按实际工作量而非行数计。

**源码事实：** `build_rowspan_slicer` 先用纯字符串检查（body 行里有无 `rowspan`）短路，绝大多数无合并的表连严格解析都不做；失败只记一条**含后果说明**的警告（「一个从 span 内部开始的切片可能让它的列错位」）。

**架构推论：** 「全有或全无」是这一层唯一自洽的选择。部分修复会产出「看起来对齐但其实错列」的表——而错列比不对齐更难发现，因为表结构完整、行数正确、只是值错位。所以正确的降级不是「尽力修一部分」，而是「退回修复之前的行为」——即原样切分。这个模式在任何「重排结构」的代码里都该采用：**理解不完整时的输出必须是「原样」，而不是「部分改动」。**

**架构推论：** 两个模块用两套 HTML 解析器不是重复而是分层。`table_markup.py` 只需要**枚举**行（宽松足够，且宽松的失败模式是「少枚举一行」），`_html_rowspan.py` 需要**重排**单元格（必须知道精确列位置，宽松的失败模式是「静默错列」）。判据是：**下游动作是「读」还是「写」？** 读可以宽松，写必须严格——因为读错了你能看见，写错了你看不见。

**架构推论：** 「语法封闭」这条纪律的实际含义是：不支持的形式要**报错并退回**，而不是「尽力容忍」。因为容忍会在扫描器里积累特例，而每一个特例都会改变「什么算支持」的边界——最终没人知道哪些输入会产生正确结果。显式的 `_Unsupported` 加一条带后果的警告，把不确定性限制在一个可枚举的集合里。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/table_markup.py` | `TABLE_TAG_RE` | 严格表标签正则，body 非贪婪 |
| `lightrag/table_markup.py` | `escape_json_table_body` | 只转义 `</table>`，幂等且保留内部标记 |
| `lightrag/table_markup.py` | `dumps_json_table_body` | 所有 JSON body 的唯一合法写出口 |
| `lightrag/table_markup.py` | `detect_table_format` | 三级判定，`None` 表示退回字符切分 |
| `lightrag/table_markup.py` | `extract_table_id` | 负向后瞻只认独立 `id` 属性 |
| `lightrag/table_markup.py` | `parse_table_tag` | 解析 JSON 表，非列表或坏 JSON 返回 `None` |
| `lightrag/table_markup.py` | `split_html_rows` | 交替正则枚举行并归属包装 |
| `lightrag/table_markup.py` | `serialize_html_rows` | 按包装名状态迁移重拼，接受任意保序子集 |
| `lightrag/table_markup.py` | `HTML_ROW_PARTS_RE` | 包装标签与行的联合扫描器 |
| `lightrag/chunker/_html_rowspan.py` | `build_rowspan_slicer` | 全有或全无的入口，先字符串短路 |
| `lightrag/chunker/_html_rowspan.py` | `RowspanSlicer` | 按行范围切片，补齐跨边界的单元格 |
| `lightrag/chunker/_html_rowspan.py` | `RowspanSlicer.slice` | 起始行补携带格，裁剪超界的 rowspan |
| `lightrag/chunker/_html_rowspan.py` | `_layout` | 算出每行每列的单元格布局与被携带的 span |
| `lightrag/chunker/_html_rowspan.py` | `_header_row_count` | 用形状签名验证 body 只含一个 thead 与一组 body 行 |
| `lightrag/chunker/_html_rowspan.py` | `_check_cell_content` | 拒绝单元格内的表格标记与不透明元素 |
| `lightrag/chunker/_html_rowspan.py` | `_ATTRS` | 带引号属性值整段消费，`>` 不结束标签 |
| `lightrag/chunker/_html_rowspan.py` | `_Cell.with_rowspan` | 值不变返回原串，`<=1` 时省略属性 |
| `lightrag/chunker/_html_rowspan.py` | `LAYOUT_BUDGET` | 按实际工作量计的上界，超限放弃整表 |
| `lightrag/chunker/_html_rowspan.py` | `_WS` | HTML 专用空白集，不用 Python 的 `\s` |

## 参考

- [lightrag 源码：table_markup.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/table_markup.py)
- [lightrag 源码：chunker/_html_rowspan.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/chunker/_html_rowspan.py)
- [HTML 规范：表格模型](https://html.spec.whatwg.org/multipage/tables.html)
- [HTML 规范：tokenizer 状态](https://html.spec.whatwg.org/multipage/parsing.html#tokenization)

## 与主核 / AI 的关系

主核里最接近的是**变长记录的跨页存储**：一条记录可能跨多个页，读取时要把多个页拼回一条完整记录，而写入时不能让一条记录的一半落在一页上。`rowspan` 跨切分边界是同一个问题的**结构版**：一个逻辑单元（合并单元格）跨了物理边界（行切分），而切分后每一片必须能独立解读。

**差别在于「补」的方向。** 日志/记录的跨页是把碎片**拼回去**（读时拼接），而这里是**复制**（写时补齐）——因为切分后的小块要能独立送入下游（嵌入、生成），不允许再拼。所以代价是重复内容占用 token，而这一点被 docstring 明确写成了调用方的责任：「一个重复的单元格花掉行切分器从未计数的 token，调用方必须对每个序列化后的切片重新检查它的上限」。

与 h01（多模态上下文）的关系是同一个原则的两次应用：那边说「标签是原子单位，不能切半」，这边说「合并单元格是原子单位，不能切成两片互不相认」。两者都在处理「结构单位与物理边界不对齐」。

与 c02/c03（分块）的关系是这一层服务的上游：行切分器决定在哪里切，本页决定「切完之后如何让每片自洽」。

对 AI 工程最实用的一条是**「部分理解时的输出必须是原样」**。任何对结构做重排的代码（表格、代码块、JSON、Markdown 列表）都有「只理解了一半」的情形。此时有两个选项：尽力修一部分，或原样返回。前者更诱人，但它产出的畸形往往**看起来正确**——列错位、括号配错、层级错一行。而原样返回的畸形是**已知的、被记录过的**（这里是那条警告）。判据是：**这个错误能被下游发现吗？** 不能的话，就必须退回原样。

另一条是「宽松解析器读、严格解析器写」。同一个 HTML，枚举行可以用 `[^>]*` 级别的正则，而移动单元格必须用能处理引号内 `>`、能证明嵌套完整性的解析器。混用会静默出错，而错的方向恰好是「写坏」。所以正确做法是让写路径配一个更严格的解析器，而不是努力把一个宽松解析器改到足够严。

最后一条是 `escape_json_table_body` 的「只转义那一个模式」。在容器里嵌内容时，最常见也最危险的做法是「把这个格式的元字符全转义一遍」——那会破坏内容里合法嵌套的内层结构。正确做法是先确定「外层的解析器会在什么序列上提前结束」，只转义那一个。这个判断在嵌套 shell 引用、嵌套 JSON-in-JSON、SQL-in-string 里全都一样。
