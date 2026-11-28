---
project: langchain
feature: markup-splitter
title: 标记语言与结构化文档分块
page_type: feature
schema_version: 1
refs:
  - path: libs/text-splitters/langchain_text_splitters/markdown.py
    symbols: [MarkdownHeaderTextSplitter, MarkdownTextSplitter]
  - path: libs/text-splitters/langchain_text_splitters/html.py
    symbols: [HTMLHeaderTextSplitter, HTMLSectionSplitter]
  - path: libs/text-splitters/langchain_text_splitters/json.py
    symbols: [RecursiveJsonSplitter]
---

# 标记语言与结构化文档分块

## 一句话职责

按**文档自身的结构**切分：Markdown 按标题层级、HTML 按标签层级、JSON 按对象层次——重点不在「切得均匀」，而在**每一块都带上它的结构位置**（这个段落属于哪一级标题、在哪个 JSON 路径下），因为这份位置会随块一起写进 `metadata`，成为后续检索时可以过滤的字段。

## 机制是什么

四个切分器，两种截然不同的取向：

| 类 | 取向 | 产物 |
|---|---|---|
| `MarkdownTextSplitter` | **字符层面的近似**：只是把 Markdown 的分隔符预设喂给递归切分器 | 纯文本块，**无结构元数据** |
| `MarkdownHeaderTextSplitter` | **结构层面**：维护一个标题栈，每块带上当前标题路径 | `Document`，`metadata` 是标题层级 |
| `ExperimentalMarkdownSyntaxTextSplitter` | 同上但保留原始空白，且多识别代码块与水平线 | `Document`，`metadata` 含 `Code` |
| `HTMLHeaderTextSplitter` | **DOM 层面**：DFS 遍历，按 `h1`–`h6` 维护活动标题集合 | `Document`，`metadata` 是标题文本 |
| `HTMLSectionSplitter` | XSLT 归一化 + 递归切分 | 结构归一后再按字符切 |
| `RecursiveJsonSplitter` | **路径层面**：按嵌套键路径重组子对象 | dict 或 JSON 字符串，**保留键路径** |

## 怎么实现的

### 一、`MarkdownTextSplitter`：极其朴素的一行

```
def __init__(self, **kwargs):
    separators = self.get_separators_for_language(Language.MARKDOWN)
    super().__init__(separators=separators, **kwargs)
```

它**没有自己的一行切分逻辑**——只是继承 `RecursiveCharacterTextSplitter` 并换成 Markdown 的分隔符预设。

这个朴素对照很关键：**它按 Markdown 的语法元素切，但不产出任何结构信息**。切出来的块和普通文本块没区别，检索时无法按「这段属于哪一节」过滤。要那个能力就必须换 `MarkdownHeaderTextSplitter`。

### 二、`MarkdownHeaderTextSplitter`：一个标题栈 + 一个键值对驱动

**构造参数里的排序是为了「先匹配更长的前缀」。**

```
self.headers_to_split_on = sorted(headers_to_split_on, key=lambda split: len(split[0]), reverse=True)
```

因为 `##` 是 `#` 的前缀——若按 `#` 先判，`## Title` 会被认成「一级标题，标题文本是 `# Title`」。**按长度降序保证 `##` 先于 `#` 被尝试**。

**`split_text` 维护五个状态变量。**

| 变量 | 作用 |
|---|---|
| `current_content` | 正在累积的行 |
| `current_metadata` | 当前块该带的标题元数据**快照** |
| `header_stack` | 已见过的标题（level、name、data） |
| `initial_metadata` | **当前有效的标题路径**（name → data） |
| `in_code_block` / `opening_fence` | 代码块状态 |

**代码块的处理是本页第一个细节：代码块内部不做标题识别。**

```
if stripped_line.startswith("```") and stripped_line.count("```") == 1:
    in_code_block = True; opening_fence = "```"
elif stripped_line.startswith("~~~"):
    in_code_block = True; opening_fence = "~~~"
elif stripped_line.startswith(opening_fence):
    in_code_block = False; opening_fence = ""
if in_code_block: current_content.append(stripped_line); continue
```

注意第一个条件的 `count("```") == 1`：**只有单侧的 ` ``` ` 才开块**（` ```foo ``` ` 这种同行闭合的不算）。而 `elif stripped_line.startswith(opening_fence)` 用记录的围栏类型判断闭合，于是 ` ``` ` 不会被 ` ~~~ ` 闭合。

这个处理解决的是一个真实问题：**Markdown 文档里的代码常常含 `#` 注释**，若不屏蔽，一行 `# 这是注释` 会被当成一级标题，把整个结构打乱。

**`stripped_line = "".join(filter(str.isprintable, stripped_line))` 这一步在剥离不可打印字符。** 它排在 `strip()` 之后、标题判定之前，于是「形似标题但前面有零宽字符」的行也能被正确识别。这是一个容易被忽略的健壮性处理——从网页复制来的 Markdown 常带零宽空格。

**标题识别的判据是「以分隔符开头，且后面是空格或已到行尾」。**

```
is_standard_header = stripped_line.startswith(sep) and (len(stripped_line) == len(sep) or stripped_line[len(sep)] == " ")
```

两个条件都必要：只有分隔符没有文本（`len == len(sep)`，即孤立的 `#`）也算标题；否则**必须紧跟一个空格**——这样 `#hashtag` 不会被当成标题。这是 Markdown 规范的要求，而代码照做了。

**栈操作是「弹出所有层级 ≥ 当前的，再压入当前」。**

```
while header_stack and header_stack[-1]["level"] >= current_header_level:
    popped = header_stack.pop()
    if popped["name"] in initial_metadata: initial_metadata.pop(popped["name"])
header_stack.append({"level": ..., "name": ..., "data": header_text})
initial_metadata[name] = header["data"]
```

这个逻辑保证了 `initial_metadata` 里只有**当前有效路径**：遇到一个 `##` 时，之前那个 `##` 与它后面的 `###` 都从元数据里被移除，而 `#` 保留。

**用 `>=` 而不是 `>` 是这条逻辑的核心。** 若用 `>`，同级的第二个 `##` 不会被移除，于是元数据里 `##` 的位置还是第一个的标题——那一块的元数据就错了。

**`custom_header_patterns` 支持非 `#` 的标题形态。** 例如 `{"**": 1, "***": 2}` 让 `**Header**` 算一级标题。`_is_custom_header` 用一条带前后否定断言的正则：

```
^ESCAPED_SEP(?!ESCAPED_SEP)(.+?)(?<!ESCAPED_SEP)ESCAPED_SEP$
```

`(?!...)` 与 `(?<!...)` 保证**恰好一个分隔符在两端**——`***Header***` 不会被 `**` 当成标题（因为开头是 `***`，第一个 `**` 后面还是 `*`）。

还有一条额外校验：**内容不能全由分隔符字符组成**。`if content and not all(c in sep for c in content.replace(" ", ""))`——于是 `**` 与 `**` 之间的空内容或纯 `*` 不算标题。

**`aggregate_lines_to_chunks` 把逐行结果合并成块，而它的合并条件有三个分支。**

1. **元数据完全相同** → 直接追加（中间用 `"  \n"`，即「两个空格 + 换行」，Markdown 里的硬换行）。
2. **元数据不同，但满足四个条件** → 也追加，并**把当前块的元数据更新成新行的**：

```
len(aggregated_chunks[-1]["metadata"]) < len(line["metadata"])     # 新行标题更深（元数据项更多）
and aggregated_chunks[-1]["content"].split("\n")[-1][0] == "#"     # 上一行是标题行
and not self.strip_headers                                          # 且没剥除标题
```

四个条件合起来描述一个特定情形：**没剥除标题时，一个标题行后面紧跟它的内容**。此时标题行与后续内容合并成一块，而元数据取更深的那份。

这段代码的注释写着 `# may be issues if other metadata is present`——**作者自己标出了这个条件的脆弱之处**：它靠「元数据项数」判断层级深度，而若元数据里混入了非标题字段（比如上游 loader 加了 `source`），这个比较就失效了。这是一个诚实的技术债标注。

**`return_each_line` 决定是否跳过后面的合并。** 为真时每行一个 `Document`；为假时走 `aggregate_lines_to_chunks`。

### 三、`ExperimentalMarkdownSyntaxTextSplitter`：保留原始空白

类名的 `Experimental` 与文档里那句「这是 `MarkdownHeaderTextSplitter` 的一次重新实现，做法有显著变化」说明它是第二代。

**它的关键差别从一行代码就能看出。**

```
raw_lines = text.splitlines(keepends=True)
```

`keepends=True` —— **保留行尾换行符**。而 `MarkdownHeaderTextSplitter` 用的是 `text.split("\n")` 再 `"\n".join(...)`，那个往返会规范化换行。于是这个版本能**原样保留原文的空行与缩进**。

这个差别的实际价值在于：Markdown 里空行是段落分隔、缩进是嵌套列表的标志，规范化会丢掉这些信息。

**它多识别两类结构：代码块与水平线。**

```
header_match = self._match_header(raw_line)
code_match = self._match_code(raw_line)
horz_match = self._match_horz(raw_line)
if header_match: ...
elif code_match:
    self._complete_chunk_doc()
    self.current_chunk.page_content = self._resolve_code_chunk(raw_line, raw_lines)
    self.current_chunk.metadata["Code"] = code_match.group(1)
    self._complete_chunk_doc()
elif horz_match: self._complete_chunk_doc()
else: self.current_chunk.page_content += raw_line
```

**代码块被单独成块，且语言标签写进 `metadata["Code"]`。** `_match_code` 用 `r"^```(.*)"` / `r"^~~~(.*)"` 捕获围栏后的语言名，`_resolve_code_chunk` 一直吞到配对围栏。于是「一个代码块 = 一个 `Document`，且带 `Code: python`」——这对检索很有用（可以按语言过滤）。

**水平线（`***` / `---` / `___`）直接结束当前块。** 这比按标题切更细，因为水平线常用来分隔主题。

**`_complete_chunk_doc` 只在内容非空时才产出，并把标题栈写进元数据。**

```
if chunk_content and not chunk_content.isspace():
    for depth, value in self.current_header_stack:
        header_key = self.splittable_headers.get("#" * depth)
        if header_key is not None: self.current_chunk.metadata[header_key] = value
    self.chunks.append(self.current_chunk)
self.current_chunk = Document(page_content="")
```

注意 `splittable_headers.get("#" * depth)` 可能返回 `None`（若 user 只声明了部分层级），此时那一级被跳过——**于是元数据里只出现声明过的层级**。

**`return_each_line` 的实现比上一代更直接，而且代码里带一句自嘲式注释：**

```
# I don't see why `return_each_line` is a necessary feature of this splitter.
# It's easy enough to do outside of the class and the caller can have more control over it.
```

也就是说作者认为这个参数应该由调用方自己做，而不是内建。保留它是为了接口兼容。这种「我不同意这个参数但我留着」的注释，比悄悄实现要好。

**`_resolve_header_stack` 与上一代的栈逻辑等价但写法不同。**

```
for i, (depth, _) in enumerate(self.current_header_stack):
    if depth >= header_depth:
        self.current_header_stack = self.current_header_stack[:i]; break
self.current_header_stack.append((header_depth, header_text))
```

用「找到第一个 ≥ 的位置并截断」，等价于「弹掉所有 ≥ 的」。而它用 `break` 保证了只截断一次。

### 四、`HTMLHeaderTextSplitter`：DFS + 活动标题集合

**它用的是显式栈的 DFS，而不是递归。**

```
stack = [body]
while stack:
    node = stack.pop()
    children = list(node.children)
    stack.extend(child for child in reversed(children) if isinstance(child, tag_cls))
```

`reversed(children)` 是关键——**栈是 LIFO，若不反过来压入，遍历顺序会是倒的**。这一行保证输出顺序与文档顺序一致。

**`active_headers` 的值是三元组 `(header_text, level, dom_depth)`，而 `dom_depth` 是第二个清理判据。**

它的用途在非标题分支里：

```
headers_out_of_scope = [k for k, (_, _, d) in active_headers.items() if dom_depth < d]
for key in headers_out_of_scope: del active_headers[key]
```

也就是说**当遍历走出某个标题节点的 DOM 深度范围时，那个标题就失效了**。这是纯层级（`h1`/`h2` 的数值）之外的第二道判断——因为 HTML 的标题层级与 DOM 嵌套不总一致（`<h2>` 可能挂在 `<div>` 里，与 `<h1>` 同级）。

**标题节点会被单独产出为一个 `Document`，且不进入累积块。**

```
if tag in self.header_tags:
    if not self.return_each_element:
        doc = finalize_chunk()
        if doc: yield doc
    level = int(tag[1:])           # h1→1, h2→2
    headers_to_remove = [k for k, (_, lvl, d) in active_headers.items() if lvl >= level]
    ...
    active_headers[header_name] = (node_text, level, dom_depth)
    header_meta = {k: v[0] for k, v in active_headers.items()}
    yield Document(page_content=node_text, metadata=header_meta)
```

**标题文本自身成为一个 `Document`**——这意味着检索时「Introduction」这个词的块就是标题那一行。文档例子里确实是这样：`Document with metadata={"Main Topic": "Introduction"} and content="Introduction"`。

这个选择有争议但有其道理：标题本身是极短且语义强的文本，单独成块在检索里常常有用（问「有哪些章节」时能命中）。

**`int(tag[1:])` 有一个 `except ValueError: level = 9999` 的兜底。** 于是 `<h1>` 之外的自定义标签（如果用户传了）不会崩，而是被当成最深层级——于是它会清空所有活动标题。

**`node_text` 的构造只取直接子文本节点。**

```
text_elements = [str(child).strip() for child in _find_all_strings(node, recursive=False)]
node_text = " ".join(elem for elem in text_elements if elem)
```

`recursive=False` 意味着**只取这个节点自己的直接文本，不含后代文本**。于是每个节点的文本都被其祖先与自身各处理一次，而累积块靠 `"  \n".join` 拼起来。

这是一个「每个节点只贡献自己的直接文本」的设计，避免了同一段文本在不同层级被重复产出——若用 `recursive=True`，`<body>` 的文本会包含全文，于是整个文档被重复 N 次。

**`finalize_chunk` 的两处守卫都是「空则不要」。**

```
if not current_chunk: return None
final_text = "  \n".join(line for line in current_chunk if line.strip())
...
if not final_text.strip(): return None
```

第一道挡「没累积任何东西」，第二道挡「累积的全是空白」。

### 五、`HTMLSectionSplitter`：先归一结构，再按字符切

**它与上面那个不是同一个路子，而是「两步走」。**

```
def split_documents(self, documents):
    ...
    results = self.create_documents(texts, metadatas=metadatas)
    text_splitter = RecursiveCharacterTextSplitter(**self.kwargs)
    return text_splitter.split_documents(results)
```

也就是说：**先用 XSLT 把 HTML 里的「看起来像标题的样式」转成真正的标题标签，再按 `RecursiveCharacterTextSplitter` 切。**

配套的是那个 XSLT 路径：

```
self.xslt_path = (pathlib.Path(__file__).parent / "xsl/converting_to_header.xslt").absolute()
```

**这个设计解决的是 HTML 的一个常见现实：很多文档的标题不是 `<h1>` 而是「大字号 + 加粗的 `<div>`」。** 靠标签名识别会漏掉它们；靠样式（`font-size`）识别才可行，而这正是 XSLT 能做的事。

而那个 `#TITLE#` 占位符替换说明这套 XSLT 干的是「把文档标题填到各节里」：

```
for key in chunk.metadata:
    if chunk.metadata[key] == "#TITLE#": chunk.metadata[key] = metadata["Title"]
```

于是**未声明 `Title` 的文档会在这一步崩**——而这是一个隐含的输入要求。

`create_documents` 用 `metadata = {**metadata, **chunk.metadata}` 合并，**后者胜**：于是块自带的标题元数据会覆盖上游 loader 的同名键。

### 六、`RecursiveJsonSplitter`：按键路径重组子对象

**它的核心是三个静态/实例方法的协作。**

1. **`_json_size(data)` = `len(json.dumps(data))`**——**用序列化长度当尺寸**。这与分块器基类的 `length_function` 概念相同，但这里硬编码成 JSON 序列化后的字符数（而不是 token）。理由是这个切分器的产物仍是 JSON，而 JSON 的体积就是它占的空间。
2. **`_set_nested_dict(d, path, value)`** 沿 `path` 逐层 `setdefault` 建字典，最后一项赋值为 `value`。
3. **`_list_to_dict_preprocessing`** 把**所有列表**（递归地）转成「下标字符串为键的字典」。

第 3 个是关键的可选能力：**JSON 里的列表无法按「键路径」切分**（一个数组的第 1 项与第 2 项之间没有名字），所以要么整块保留、要么先转成字典再切。这个 flag 暴露给调用方（`convert_lists`），默认 `False`。

**`_json_split` 是递归函数，逐键判断「装得下就塞、装不下就递归」。**

```
for key, value in data.items():
    new_path = [*current_path, key]
    chunk_size = self._json_size(chunks[-1])
    size = self._json_size({key: value})
    remaining = self.max_chunk_size - chunk_size
    if size < remaining:
        self._set_nested_dict(chunks[-1], new_path, value)      # 塞进当前块
    else:
        if chunk_size >= self.min_chunk_size: chunks.append({})  # 当前块够大了，开新块
        self._json_split(value, new_path, chunks)                # 递归进子结构
```

**`min_chunk_size` 是这一页第二个关键设计，它的作用是避免产生大量碎片。**

若没有 `chunk_size >= min_chunk_size` 这个条件，那么每当一个键装不下就新开一块——于是一份深层嵌套的 JSON 会被切成几十个很小的小块。有了它，**只有当前块已经「差不多满了」才新开**；否则就在现有块里继续递归，把子结构塞进去。

而 `min_chunk_size` 的默认值是 `max(max_chunk_size - 200, 50)`——**「上限减两百，但不低于五十」**。

**那个 `<` 而不是 `<=` 的判定值得注意**：`size < remaining` 才算装得下。所以恰好等于剩余空间的键会进入递归分支。

**`elif current_path: self._set_nested_dict(chunks[-1], current_path, data)` 处理叶子值。** 也就是说标量与非空字典以外的值（列表若未转换）在这里被塞进当前块。

**`split_json` 的输入校验有一个专门为「顶层列表」准备的提示。**

```
if json_data is not None and not isinstance(json_data, dict):
    msg = f"json_data must be a dict, got {type(json_data).__name__}."
    if is_list_input and not convert_lists:
        msg += " Top-level lists can be split by passing convert_lists=True."
    raise TypeError(msg)
```

**错误消息会根据具体情形追加一句解法**——恰是这个情形（传了顶层列表但没开 `convert_lists`）最可能的成因。而 `None` 是特例：`json_data is not None` 的条件让它**直接通过**，不进校验；随后 `_json_split(None)` 返回 `[{}]`，最后「若最后一块为空则弹出」得到空列表。

**「若最后一块为空则弹出」这个清理是必需的。** `_json_split` 的初始 `chunks` 是 `[{}]`，若所有内容都装进了有内容的部分，最后可能有空块残留。

`split_text` 只是把 `split_json` 的结果逐块 `json.dumps`，`ensure_ascii` 可关（中文不转义）。而 `create_documents` 的签名把 `metadatas` 放在 `convert_lists` 之后——**与基类 `create_documents` 的参数顺序不同**，这是一个接口不一致的小瑕疵。

## 关键取舍

**源码事实：** `MarkdownTextSplitter` 没有任何自己的切分逻辑，只是把 Markdown 分隔符预设喂给递归切分器——因此**不产出结构元数据**。

**源码事实：** `MarkdownHeaderTextSplitter` 把 `headers_to_split_on` 按分隔符长度降序排序，保证 `##` 先于 `#` 被尝试。

**源码事实：** 代码块内部不做标题识别，且只有「单侧围栏」才开块（`count("```") == 1`），闭合用记录的围栏类型判断——因为 Markdown 代码里的 `#` 注释会被误认为标题。

**源码事实：** 标题栈用 `>=` 弹出，于是同级的第二个标题会替换掉第一个——若用 `>`，元数据会保留错误的同级标题。

**源码事实：** `custom_header_patterns` 用前后否定断言保证恰好一个分隔符，且额外校验内容不全由分隔符组成。

**源码事实：** `aggregate_lines_to_chunks` 的第二个分支靠「元数据项数」判层级深度，作者自己用注释标出「若混入其它元数据会出问题」。

**源码事实：** `ExperimentalMarkdownSyntaxTextSplitter` 用 `splitlines(keepends=True)` 保留原始空白，并把代码块单独成块且写 `metadata["Code"]`；它的文档明确说这是 `MarkdownHeaderTextSplitter` 的重写，并且作者用注释表明他不认为 `return_each_line` 有必要。

**源码事实：** `HTMLHeaderTextSplitter` 用显式栈 DFS 且 `reversed(children)` 保证文档顺序；`active_headers` 除了标题层级还用 `dom_depth` 清理（因为 HTML 的标题层级与 DOM 嵌套不总一致）。

**源码事实：** 标题节点自身被单独产出一个 `Document`，不进入累积块。

**源码事实：** `node_text` 只取直接子文本（`recursive=False`），避免同一段文本在不同层级重复产出。

**源码事实：** `HTMLSectionSplitter` 先用 XSLT 把「样式像标题的节点」转成标题标签，再交给递归切分器——解决「标题不一定是 `<h1>`」这个现实。

**源码事实：** `RecursiveJsonSplitter` 用 JSON 序列化长度当尺寸，用 `min_chunk_size`（默认上限减 200，不低于 50）避免产生大量碎片，用 `convert_lists` 把列表转成下标键字典以便能按键路径切分。

**源码事实：** `split_json` 对顶层列表的错误消息会按情形追加 `convert_lists=True` 的解法提示；`None` 输入返回空列表而非报错。

**架构推论：** 这一组切分器的共同点不是算法，而是**它们都在把「结构位置」变成元数据**。Markdown 的标题路径、HTML 的标题集合、JSON 的键路径，三者最终都落在 `metadata` 里。

这件事的分量在于：**分块破坏的是「上下文」**——一句话被切走后读者不知道它属于哪一节。而补回上下文只有两条路：要么把标题文本拼进块内容（`strip_headers=False` 时就是这么做的），要么把标题写进元数据、让检索侧按它过滤或拼装。langchain 两条都支持，而元数据这条是更干净的做法——**内容保持原样，位置另存一处**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/text-splitters/langchain_text_splitters/markdown.py` | `MarkdownTextSplitter` | 仅换分隔符预设，无结构元数据 |
| `libs/text-splitters/langchain_text_splitters/markdown.py` | `MarkdownHeaderTextSplitter` | 标题栈 + 逐行元数据，按长度降序匹配 |
| `libs/text-splitters/langchain_text_splitters/markdown.py` | `MarkdownHeaderTextSplitter.split_text` | 五行状态机：累积、元数据快照、栈、路径、代码块 |
| `libs/text-splitters/langchain_text_splitters/markdown.py` | `aggregate_lines_to_chunks` | 按元数据相同与否合并，含作者标注的脆弱分支 |
| `libs/text-splitters/langchain_text_splitters/markdown.py` | `_is_custom_header` | 前后否定断言保证单分隔符，排除纯分隔符内容 |
| `libs/text-splitters/langchain_text_splitters/markdown.py` | `ExperimentalMarkdownSyntaxTextSplitter` | 保留原始空白，代码块与水平线单独成块 |
| `libs/text-splitters/langchain_text_splitters/markdown.py` | `_resolve_code_chunk` | 吞到配对围栏，语言名写进 `metadata["Code"]` |
| `libs/text-splitters/langchain_text_splitters/html.py` | `HTMLHeaderTextSplitter` | DFS 遍历 + 活动标题集合（含 `dom_depth`） |
| `libs/text-splitters/langchain_text_splitters/html.py` | `_generate_documents` | 显式栈 DFS，`reversed(children)` 保序 |
| `libs/text-splitters/langchain_text_splitters/html.py` | `HTMLSectionSplitter` | XSLT 归一化 + 递归切分的两步流程 |
| `libs/text-splitters/langchain_text_splitters/html.py` | `create_documents`（Section 版） | `#TITLE#` 占位替换 + 元数据后者胜 |
| `libs/text-splitters/langchain_text_splitters/json.py` | `RecursiveJsonSplitter` | 按键路径递归切分，产物仍为 JSON |
| `libs/text-splitters/langchain_text_splitters/json.py` | `_json_split` | 装得下就塞、装不下递归，受 `min_chunk_size` 约束 |
| `libs/text-splitters/langchain_text_splitters/json.py` | `_list_to_dict_preprocessing` | 列表 → 下标键字典，使列表可按键路径切 |
| `libs/text-splitters/langchain_text_splitters/json.py` | `split_json` | 类型校验 + 顶层列表专项提示 + 尾部空块清理 |

## 参考

- [langchain 源码：markdown.py](https://github.com/langchain-ai/langchain/blob/master/libs/text-splitters/langchain_text_splitters/markdown.py)
- [langchain 源码：html.py](https://github.com/langchain-ai/langchain/blob/master/libs/text-splitters/langchain_text_splitters/html.py)
- [langchain 源码：json.py](https://github.com/langchain-ai/langchain/blob/master/libs/text-splitters/langchain_text_splitters/json.py)
- [Markdown 规范：标题与代码块](https://spec.commonmark.org/)
- [langchain 文档：按标题切分](https://python.langchain.com/docs/how_to/markdown_header_metadata_splitter/)

## 与主核 / AI 的关系

这套「结构切分 + 元数据保留」与主核里**按表/分区/索引边界切分数据**的思路一致：数据库也会把数据按分区键切块，而分区键本身就是随数据一起保留的元数据，用于后续过滤。两者相同的地方是「切分边界由结构决定」，不同的地方是：主核的分区是**物理的**（数据真的存放在不同文件里），而这里的切分是**逻辑的**（原文还在，只是产出多个 `Document`）。

对 agent 工程的直接价值有三条。其一，**「切分破坏上下文」是分块的头号问题，而元数据是解**。把标题路径写进 `metadata`，检索时既能按它过滤（只在某节里搜），也能在送给模型前把它拼回块前面——两条路都比把标题塞进正文更干净。其二，**用结构切分必须处理「结构被误识别」**。Markdown 代码块里的 `#` 注释就是典型：不屏蔽代码块，整份文档的结构会被几行注释撕碎。其三，**层级判定不能只看标签数值**。HTML 里 `<h2>` 可能挂在任意 `<div>` 下，纯按 `h1`–`h6` 的数值管理标题栈会出错；`HTMLHeaderTextSplitter` 加上 `dom_depth` 作为第二判据，正是因为这个。
