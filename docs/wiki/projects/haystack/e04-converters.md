---
project: haystack
feature: converters
title: 格式转换器
page_type: feature
schema_version: 1
refs:
  - path: haystack/components/converters/markdown.py
    symbols: [MarkdownToDocument]
  - path: haystack/components/converters/html.py
    symbols: [HTMLToDocument]
  - path: haystack/components/converters/docx.py
    symbols: [DOCXToDocument, DOCXTableFormat]
---

# 格式转换器

## 一句话职责

把三种文件格式变成纯文本 `Document`：Markdown 用 `markdown-it-py` 渲染成纯文本（可选剥掉 YAML frontmatter、可选把表格压成单行），HTML 用 `trafilatura` 抽正文（自动剔导航/广告），DOCX 用 `python-docx` 遍历 body 元素（段落、表格、文本框），且**三者对元数据的处理方式是同一套骨架**。

## 机制是什么

| 转换器 | 底层库 | 「文本」的定义 | 保留的结构信息 |
|---|---|---|---|
| `MarkdownToDocument` | `markdown-it-py` + 自定义 renderer | 渲染后的纯文本 | frontmatter 进元数据 |
| `HTMLToDocument` | `trafilatura.extract` | **正文抽取**后的文本 | 无（只剩纯文本） |
| `DOCXToDocument` | `python-docx` | body 元素逐个转字符串 | 表格转 Markdown/CSV、分页符转 `\f`、文档属性进元数据 |

**三者共用的骨架是同一段循环**：`get_bytestream_from_source` → 解码 → 转换 → 合并元数据 → 造 `Document`，每一步失败都 `logger.warning` 并 `continue`（跳过这个文件而不是整个批次）。

## 怎么实现的

### 一、共同的失败处理：跳过而非中断

**三个 `run` 的结构几乎逐行相同，而它们都有三层 `try/except`。**

```
try: bytestream = get_bytestream_from_source(source)
except Exception as e: logger.warning("Could not read {source}. Skipping it. Error: {error}", ...); continue

try:
    file_content = bytestream.data.decode(encoding)
    text = parser.render(file_content)
except Exception as conversion_e:
    logger.warning("Failed to extract text from {source}. Skipping it. Error: {error}", ...); continue
```

**两层分开的理由是「失败的性质不同」**：读不到文件（路径错、权限）与解析失败（格式坏、编码错）是两个独立的问题，分开记录让日志能直接告诉你是谁的问题。

而 `continue` 的选择意味着**批量转换不会因为一个坏文件全废**。这与 `g01`（摄取流水线）那种「变换缓存失败就重跑」的容错取向一致，但与 `f04`（joiners）里那种「分数非法就报错」不同。

**`HTMLToDocument` 多了一道「抽取结果为 `None`」的检查。**

```
text = extract(html_content, **merged_extraction_kwargs)
...
if text is None:
    logger.warning("No text could be extracted from {source}. Skipping it.", source=source)
    continue
```

**`trafilatura.extract` 在「判定为无正文」时返回 `None` 而不是空串**——于是它把「抽取成功但内容为空」与「根本没有正文」区分开了。若不检查，`Document(content=None, ...)` 会构造出一个 `content` 为 `None` 的文档，而下游的 `DocumentSplitter` 会因此在 `run` 里抛 `ValueError`。

**这是三个转换器里唯一一个显式处理「空结果」的**，而理由在于 HTML 的不可控性最高（一个纯图片页、一个只有导航的页面都可能抽不出正文）。

**`HTMLToDocument` 还有一道「输入本身就是空」的检查。**

```
if not bytestream.data:
    logger.warning("Skipping {source} because it is empty.", source=source)
    continue
```

放在解码之前——因为空字节流无法判定编码。

### 二、`MarkdownToDocument`：两张自定义渲染规则

**默认渲染走 `RendererPlain`，也就是「输出纯文本、不带任何标记」。**

```
parser = MarkdownIt(renderer_cls=RendererPlain)
```

**而 `table_to_single_line=True` 时它改写了四条渲染规则。**

```
if self.table_to_single_line:
    parser.enable("table")
    renderer = parser.renderer
    renderer.rules["table_open"] = _render_table_boundary
    renderer.rules["table_close"] = _render_table_boundary
    renderer.rules["th_open"] = _render_table_cell_open
    renderer.rules["td_open"] = _render_table_cell_open
```

两个函数各自做一件极小的事：

```
def _render_table_cell_open(tokens, idx, _options, _env) -> str:
    """Insert a space before every table cell except the first."""
    is_first_table_cell = tokens[idx - 1].type == "tr_open" and tokens[idx - 2].type == "thead_open"
    return "" if is_first_table_cell else " "

def _render_table_boundary(_tokens, _idx, _options, _env) -> str:
    """Separate the table from surrounding content with a newline."""
    return "\n"
```

**`_render_table_cell_open` 那个「前两个 token 是不是 `thead_open` → `tr_open`」的判断，是为了识别「表头行的第一个单元格」并省掉它的前导空格**。这样输出以单元格内容开头而不是以空格开头。

**而它只判断了表头行、没有判断普通行的第一个单元格** ——所以普通行的第一格前面会有空格。这是这个实现的一个小不一致，但结果仍然是一行可读的文本（` "a b" "c d"` 而不是 `"a b c d"` 那种粘在一起的形态）。

**为什么需要这个功能：** 一张表格渲染成纯文本后，每个单元格之间可能没有分隔（`RendererPlain` 只输出文本），于是「相邻两格的文字会粘在一起」。插入空格就是最低成本的修复——不追求可解析，只求可读。

**`extract_frontmatter` 的实现有四层错误处理，而每层的语义都不同。**

```
match = _FRONTMATTER_PATTERN.match(file_content)
if not match: return file_content, {}

frontmatter_text = match.group("frontmatter")
try:
    frontmatter = json.loads(json.dumps(yaml.safe_load(frontmatter_text), default=str)) or {}
except yaml.YAMLError: ... return file_content, {}
except (TypeError, ValueError): ... return file_content, {}

if not isinstance(frontmatter, dict): ... return file_content, {}
return file_content[match.end():], frontmatter
```

四个要点：

1. **`json.loads(json.dumps(..., default=str))` 这个往返是「把任意 YAML 值规整成 JSON 可表示」**。`default=str` 让 `datetime`、自定义对象变成字符串；`json.loads` 再把它读回来。于是元数据里不会出现 Python 特有类型——而这很重要，因为**元数据要能被序列化进 YAML 管道配置与文档存储**。
2. **`or {}` 处理「frontmatter 是空的」** —— 一个只有 `---\n---` 的文件，`yaml.safe_load` 返回 `None`。
3. **三类错误分别捕获且都「保留为内容」**：YAML 语法错（`yaml.YAMLError`）、无法转成 JSON（`TypeError`/`ValueError`）、以及**解析成功但不是映射**（`not isinstance(frontmatter, dict)`）。
4. **失败时都返回 `file_content, {}`** —— 也就是说**整段 frontmatter 保留在正文里**。这是一个正确的降级：宁可让 `---` 出现在文本里，也不要静默丢掉用户的内容。

第 3 条的措辞值得记：那个警告写的是 `expected a mapping, got {kind}`，明确说出了期望与实际类型。而 frontmatter 本应是 `key: value` 映射，一个 YAML 列表或标量在那里出现是异常输入。

**正则本身处理了 CRLF 与行尾空格。**

```
_FRONTMATTER_PATTERN = re.compile(r"\A---[ \t]*\r?\n(?P<frontmatter>.*?)^---[ \t]*(?:\r?\n|$)", re.DOTALL | re.MULTILINE)
```

`\A` 锚定文件开头（不是任意行首）；`[ \t]*\r?\n` 允许 `---` 后面有空白与 CRLF；`(?P<frontmatter>.*?)` 非贪婪配合 `DOTALL`；结尾 `(?:\r?\n|$)` 允许文件以 `---` 直接结束。

### 三、`HTMLToDocument`：把「正文抽取」交给 trafilatura

**它的核心只有一行调用，而所有的可调性都通过 `extraction_kwargs` 透传。**

```
merged_extraction_kwargs = {**self.extraction_kwargs, **(extraction_kwargs or {})}
...
text = extract(html_content, **merged_extraction_kwargs)
```

**两处 `extraction_kwargs` 的合并顺序是「运行期覆盖构造期」。** 于是同一个组件实例可以在不同调用里用不同参数——一个实例级默认 + 一次调用级覆盖的模式。

**`encoding` 的默认值是 `None`，而它有一句需要读两遍的文档。**

```
:param encoding: The encoding to use when decoding HTML files. If None (the default), Trafilatura detects the encoding. If detection produces incorrect characters, install `trafilatura[all]` for additional encoding detection support, or set this parameter if you know the encoding.
```

也就是说：

- **`encoding=None` → 不解码，把 `bytes` 直接交给 `extract`**（`html_content = html_bytes.decode(self.encoding) if self.encoding is not None else html_bytes`）。
- 让 trafilatura 自己检测编码，而**检测能力取决于是否装了 `trafilatura[all]`**（额外依赖）。

**这是一个「默认走库的自动检测、但允许显式覆盖」的设计，而它的文档诚实地承认了自动检测可能出错**（"If detection produces incorrect characters"）。这种「不假装完美」的措辞值得记。

**它没有保留任何结构信息。** 与 Markdown 版（frontmatter 进元数据）和 DOCX 版（表格、文档属性）不同，HTML 版产出的是纯文本——因为 `trafilatura` 的定位就是「提取正文」，它会剔掉导航、侧栏、脚注。

这是一条明确的取舍：**要「干净的正文」就必须放弃「结构」**。而需要 HTML 结构（标题层级）的场景应当用 `e02` 那页的 `HTMLHeaderSplitter` 类组件（虽然本仓的 haystack 里没有，但那是另一类职责）。

### 四、`DOCXToDocument`：遍历 body、保留三种结构

**它的 `_extract_elements` 遍历 `document.element.body` 的原始 XML 元素，而不是用 `python-docx` 的高层 API 遍历段落。**

```
for element in document.element.body:
    if isinstance(element, _Comment): continue
    if element.tag.endswith("p"):
        paragraph = Paragraph(element, document)
        ...
    elif element.tag.endswith("tbl"):
        table = docx.table.Table(element, document)
        ...
```

**为什么不用 `document.paragraphs`？** 因为它**丢掉了表格**（只返回段落），而这里要按「文档中的实际顺序」交替处理段落与表格。直接遍历 body 的元素能保住顺序。

**`element.tag.endswith("p")` 而不是 `== "w:p"`** —— 因为标签带命名空间前缀（`{http://...}p`），用 `endswith` 避免绑定具体前缀。

**`_Comment` 被显式跳过。** DOCX 的批注也是 body 里的元素，但它不是内容。

**分页符的处理是这一页最细的一段，而它自己承认了局限。**

```
if paragraph.contains_page_break:
    para_text = self._process_paragraph_with_page_breaks(paragraph)
```

```
for pb_index, page_break in enumerate(paragraph.rendered_page_breaks):
    if pb_index == 0:
        if page_break.preceding_paragraph_fragment:
            para_text += self._process_links_in_paragraph(page_break.preceding_paragraph_fragment)
        para_text += "\f"
        if page_break.following_paragraph_fragment:
            para_text += self._process_links_in_paragraph(page_break.following_paragraph_fragment)
    else:
        para_text += "\f"
```

注释把局限说得很清楚：

> `following_paragraph_fragment` 包含该段落剩余的全部文本。但如果剩余的段落跨过多个分页符，它**不包含后面那些分页符**，所以我们只能在下面的 `else` 块里把它们追加到文本末尾。这不理想，但这个情形应该非常罕见，大概够用。

**也就是说：一个段落里跨多个分页符时，分页符会被移到段落末尾**（位置不准，但数量对）。这是一个「已知不精确但接受」的实现——而它**把分页符转成 `\f`**，与 `e01`/`e02` 那两页的切分器约定完全一致。

**这条约定是关键：DOCX 转换器产出的 `\f` 让 `DocumentSplitter` 的 `split_by="page"` 与 `page_number` 元数据能正常工作。** 三个模块通过 `\f` 这个字符串起来了。

**文本框的处理有一段「避免重复」的逻辑。**

```
for text_box in element.iter(_TEXT_BOX_CONTENT_TAG):
    # Skip the `mc:Fallback` copy, or the same text is emitted twice.
    if any(ancestor.tag == _FALLBACK_TAG for ancestor in text_box.iterancestors()):
        continue
```

**为什么会有两份？** DOCX 里的文本框（drawing）为兼容性会写两份：一份是 `mc:Choice`（新格式），一份是 `mc:Fallback`（旧格式的替代表示）。两者内容相同。若都提取，同一段文字会出现在结果里两次。

注释把这个原因写在了代码里（"or the same text is emitted twice"），而清理方式很精准：**只跳过祖先链里有 `mc:Fallback` 的那些**。

**文本框里的内容也被递归处理**（里面的段落与表格各自走同样的转换），而它们被 append 到**承载它的段落之后**。

**表格转 Markdown 有两个细节，而第二个是「转义顺序」的问题。**

```
def _escape_markdown_cell(text: str) -> str:
    text = _MARKDOWN_CELL_BREAK_PATTERN.sub(" ", text)
    # The backslash run in front of the pipe is doubled first, so a backslash the
    # cell already contains cannot consume the escape.
    return _MARKDOWN_CELL_PIPE_PATTERN.sub(lambda match: match.group(1) * 2 + r"\|", text)
```

两件事：

1. **把单元格内的换行压成空格** —— 否则一个跨多段的单元格会把 Markdown 行从中间断掉。注释写得直白：「a cell spanning several paragraphs arrives with newlines in it, which would end the row in the middle」。
2. **竖线转义，且先把已有的反斜杠加倍** —— 注释解释了原因：**若单元格里本来就有反斜杠，它会「吃掉」我们新加的转义**。于是 `\|` 前面的反斜杠序列先翻倍，再加 `\|`。

第 2 点是标准的转义正确性问题，而它的正确处理方式只有这一种（先转义转义符本身）。

**列宽对齐是这一页唯一「为了好看」的代码。**

```
for row in table.rows:
    for i, cell in enumerate(row.cells):
        cell_text = self._escape_markdown_cell(self._cell_text(cell).strip())
        max_col_widths[i] = max(max_col_widths[i], len(cell_text))
...
md_row = [self._escape_markdown_cell(...).ljust(max_col_widths[j]) for j, cell in enumerate(row.cells)]
markdown.append("| " + " | ".join(md_row) + " |")
if i == 0:
    separator = ["-" * max_col_widths[j] for j in range(len(row.cells))]
    markdown.append("| " + " | ".join(separator) + " |")
```

**注意它算列宽时用的是「转义后」的文本长度**（注释写明「on the escaped text that is written out」）——因为表格最终输出的是转义后的文本，用原文长度算会让列宽偏窄。

**而 `ljust` 用 `len()` 而不是显示宽度** ——对 CJK 字符会算错（一个汉字占两列但 `len` 返回 1）。这是一个已知的近似，输出仍然合法（只是对齐不好看）。

**CSV 格式走的是标准库。**

```
csv_writer = csv.writer(csv_output, quoting=csv.QUOTE_MINIMAL)
for row in table.rows:
    csv_writer.writerow([self._cell_text(cell).strip() for cell in row.cells])
csv_string = csv_output.getvalue().strip()
```

`QUOTE_MINIMAL` 只在必要时加引号，而**转义交给 `csv` 模块**——这是与 Markdown 路径的关键差别：Markdown 要手工转义（因为格式本身没有转义规范），CSV 有标准库。

**而两种格式的默认值是 CSV**（`table_format: str | DOCXTableFormat = DOCXTableFormat.CSV`）。这个默认值得记：CSV 对表格来说是「无损且可解析」的表示，而 Markdown 表格有对齐假设（第一行是表头、列数一致）。

**元数据方面，DOCX 版多一个嵌套键 `docx`。**

```
docx_metadata = asdict(self._get_docx_metadata(document=docx_document))
merged_metadata = {**bytestream.meta, **metadata, "docx": docx_metadata}
```

**`asdict` 把 `DOCXMetadata` 这个 dataclass 转成字典，然后塞在一个键下** ——而不是平铺到顶层。这个选择是对的：平铺会与用户元数据冲突（`title`、`author` 这类键太常见了），嵌套则不会。

而 `DOCXMetadata` 的 15 个字段来自 `core_properties`，其中 `created` / `last_printed` / `modified` 是 `str | None` 且注释说明是「ISO 格式化字符串」——**datetime 被转成了字符串**，理由与 Markdown 那页的 `json.dumps(default=str)` 一致：元数据要可序列化。

### 五、三者共用的元数据合并

**三个转换器的合并顺序完全一致。**

Markdown 版（多一项 frontmatter）：

```
merged_metadata = {**bytestream.meta, **frontmatter, **metadata}
```

HTML / DOCX 版：

```
merged_metadata = {**bytestream.meta, **metadata}
```

**优先级是「用户显式传的 `meta` 最高」，而 frontmatter 在中间。**

这个顺序有实际后果：**若文件的 frontmatter 里有 `title`，而调用时 `meta={"title": "..."}`，用户的值胜出**。而 ByteStream 自带元数据（文件路径、编码）优先级最低——它们只是「文件的事实」，不应覆盖用户意图。

**`store_full_path` 的处理三者一致。**

```
if not self.store_full_path and (file_path := bytestream.meta.get("file_path")):
    merged_metadata["file_path"] = os.path.basename(file_path)
```

**默认只存文件名而不是完整路径** —— 这是一个安全与可移植性的考虑（完整路径会泄露目录结构，且换机器后失效）。

而 `normalize_metadata(meta=meta, sources_count=len(sources))` 负责把「单个字典」或「字典列表」统一成「与 sources 等长的列表」，然后 `zip(..., strict=True)` 保证长度匹配。

## 关键取舍

**源码事实：** 三个转换器共用同一骨架：读 ByteStream → 解码 → 转换 → 合并元数据 → 造 `Document`；读失败与转换失败分两层 `try/except`，都 `continue`（跳过单个文件而非中断批次）。

**源码事实：** 只有 `HTMLToDocument` 检查抽取结果为 `None`——因为 `trafilatura.extract` 用 `None` 表达「判定为无正文」，若不检查会产出 `content=None` 的文档，让下游切分器抛错。

**源码事实：** `MarkdownToDocument.table_to_single_line` 通过改写四条渲染规则实现，插入的空格只跳过「表头行第一格」，普通行的第一格仍有前导空格。

**源码事实：** frontmatter 解析走 `json.loads(json.dumps(..., default=str))` 往返，把任意 YAML 值规整成 JSON 可表示——因为元数据需要可序列化。

**源码事实：** frontmatter 解析的四类失败（YAML 语法错、无法转 JSON、不是映射、无 frontmatter）**都保留整段内容在正文里**，不静默丢弃。

**源码事实：** `HTMLToDocument` 的 `encoding=None` 表示「不解码，把 bytes 交给 trafilatura 自动检测」，而文档诚实地写明「检测可能出错」以及需要额外依赖 `trafilatura[all]`。

**源码事实：** `DOCXToDocument` 遍历 `document.element.body` 的原始 XML 元素而非 `document.paragraphs`，因为后者丢掉表格、无法保住段落与表格的实际顺序。

**源码事实：** DOCX 的分页符被转成 `\f`，与 `DocumentSplitter` 的 `split_by="page"` 及 `page_number` 元数据约定一致；一个段落跨多个分页符时分页符会被移到段末（注释承认「不理想但罕见」）。

**源码事实：** 文本框要跳过祖先链里有 `mc:Fallback` 的那些，否则同一段文字会因为 DOCX 的兼容性双写而出现两次。

**源码事实：** Markdown 表格转义要**先把已有反斜杠加倍**再加 `\|`——否则单元格里原有的反斜杠会吃掉转义。

**源码事实：** DOCX 元数据塞在 `docx` 这一个键下而非平铺，避免 `title`/`author` 这类常见键与用户元数据冲突。

**源码事实：** 元数据合并优先级是「用户 `meta` > frontmatter > ByteStream 元数据」；`store_full_path=False` 时只保留文件名。

**架构推论：** 三个转换器之间的差别，本质是**「保留多少结构」与「文本多干净」的权衡**：

- HTML 版把一切都交给 `trafilatura`，得到最干净的正文，但结构全丢。
- Markdown 版保留 frontmatter 进元数据，正文则被压成纯文本。
- DOCX 版保留最多（表格格式、分页符、文档属性、文本框），代价是代码量最大（508 行 vs 156 行）。

而它们共享的那个 `\f` 约定说明一件重要的事：**格式转换器的输出契约不只是「一段文本」，还包括「文本里有哪些可被下游识别的标记」**。分页符、表格形态、frontmatter 都是这种标记。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/components/converters/markdown.py` | `MarkdownToDocument` | Markdown → 纯文本，可选表格单行与 frontmatter 提取 |
| `haystack/components/converters/markdown.py` | `_render_table_cell_open` / `_render_table_boundary` | 改写渲染规则给单元格插空格、给表格插换行 |
| `haystack/components/converters/markdown.py` | `_FRONTMATTER_PATTERN` | frontmatter 正则，处理 CRLF 与行尾空白 |
| `haystack/components/converters/markdown.py` | `_extract_frontmatter` | 四类失败都保留内容，JSON 往返规整类型 |
| `haystack/components/converters/html.py` | `HTMLToDocument` | trafilatura 抽取正文，`None` 结果跳过 |
| `haystack/components/converters/html.py` | `extraction_kwargs` 合并 | 运行期覆盖构造期 |
| `haystack/components/converters/docx.py` | `DOCXToDocument` | 遍历 body，段落/表格/文本框 |
| `haystack/components/converters/docx.py` | `DOCXTableFormat` | Markdown / CSV 两种表格表示，默认 CSV |
| `haystack/components/converters/docx.py` | `_extract_elements` | 按文档实际顺序交替处理段落与表格 |
| `haystack/components/converters/docx.py` | `_process_paragraph_with_page_breaks` | 分页符转 `\f`，多分页符时移到段末 |
| `haystack/components/converters/docx.py` | `_extract_text_boxes` | 跳过 `mc:Fallback` 副本避免重复 |
| `haystack/components/converters/docx.py` | `_escape_markdown_cell` | 换行压空格 + 反斜杠加倍再转义竖线 |
| `haystack/components/converters/docx.py` | `_table_to_markdown` / `_table_to_csv` | 手工对齐 vs 标准库转义 |
| `haystack/components/converters/docx.py` | `DOCXMetadata` | 15 个 core_properties 字段，日期转 ISO 字符串 |

## 参考

- [haystack 源码：converters/markdown.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/converters/markdown.py)
- [haystack 源码：converters/docx.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/converters/docx.py)
- [trafilatura 文档：extract](https://trafilatura.readthedocs.io/en/latest/corefunctions.html#extract)
- [markdown-it-py 文档](https://markdown-it-py.readthedocs.io/)
- [haystack 文档：转换器](https://docs.haystack.deepset.ai/docs/converters)

## 与主核 / AI 的关系

格式转换器与主核里的**外部表 / 数据加载**是同一个职责：把外部表示翻译成内部类型。数据库的 `COPY FROM ... CSV` 也面临同样的三件事——编码、分隔与转义、以及「哪些元信息要保留」。

而 `\f` 这个约定与主核的**行分隔符**类似：两者都是「用文本里的一个特殊字符表达结构边界」，从而让「按结构切分」在纯文本层可行。差别是数据库用 `\n` 表达行（结构不可嵌套），这里用 `\f` 表达页（页是平的，所以可行）。

对 agent 工程的直接价值有三条。其一，**转换器的输出契约包含「文本里的标记」，不只是文本本身**。DOCX 版把分页符转成 `\f`，正是为了让下游切分器的「按页切」与「页号元数据」能工作——一个不转的转换器会让整条链的这一段失效。其二，**转义必须先把转义符本身转义**。Markdown 表格的竖线转义要先加倍反斜杠，否则单元格里原有的反斜杠会吃掉转义——这类错误只在特定输入上出现，很难在测试里覆盖全。其三，**「抽取不到内容」要显式区分**。`trafilatura` 用 `None` 表达它，而下游对 `None` 与空串的反应完全不同（前者在切分器里抛错）；转换器是唯一能拦住这个差异的位置。
