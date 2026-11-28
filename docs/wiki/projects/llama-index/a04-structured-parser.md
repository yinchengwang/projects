---
project: llama-index
feature: structured-parser
title: 结构化文档解析
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/node_parser/file/markdown.py
    symbols: [MarkdownNodeParser]
  - path: llama-index-core/llama_index/core/node_parser/file/html.py
    symbols: [HTMLNodeParser]
  - path: llama-index-core/llama_index/core/node_parser/relational/base_element.py
    symbols: [BaseElementNodeParser, Element, TableOutput]
---

# 结构化文档解析

## 一句话职责

切分依据从「长度」换成「文档自有结构」的三个解析器：`MarkdownNodeParser` 按标题层级切节并把祖先标题路径写进元数据；`HTMLNodeParser` 按标签名分组切段并把标签名写进元数据；`BaseElementNodeParser` 把文档抽成 `Element` 序列，**把表格做成「一句摘要 + 一份原文」的双节点**——摘要进索引、原文留给生成。

## 机制是什么

前两个解析器（Markdown / HTML）回答的是同一个问题：**文档自己已经声明了结构，为什么还要按 token 猜边界？** 它们的产物形状也相似——一个节点对应一个结构单元（一个章节 / 一串同名标签），并把「这个单元在结构中的位置」写进 `metadata`。

第三个解析器的野心大得多。它看到的结构不只是「段落与标题」，而是**异质的元素序列**：文本段、表格、标题。文本可以随便切，表格不行——一个被切成两半的表格既不能嵌入也不能被模型理解。它的解法是「一个元素变成两类节点」：

| 节点 | 文本内容 | 元数据 | 进什么索引 |
|---|---|---|---|
| `IndexNode` | 表格的**自然语言摘要** | `col_schema`（列结构，排除嵌入） | 摘要检索（索引节点自身） |
| `TextNode` | 摘要 + 表格原文（markdown 或原始文本） | `table_df`、`table_summary`（两者都排除嵌入与 LLM） | 向量索引 |

两者**共享同一个 id**（`IndexNode.index_id` 与 `TextNode.id_` 是同一个字符串）。这个共享 id 是全篇的机关：检索命中摘要后，靠它把原文节点取回来。于是「用小而语义化的摘要做检索键，用大而精确的原文做回答材料」这条分离，与 a02 的句子窗口是同一个思路的另一种实现。

## 怎么实现的

### Markdown：按标题切节

**状态只有三个变量。**

```
current_section = ""
header_stack: List[tuple[int, str]] = []   # (markdown 层级, 标题文本)
code_block = False
```

逐行扫描，三个分支：

**第一支：围栏代码块的开关。**

```
if line.lstrip().startswith("```"):
    code_block = not code_block
    current_section += line + "\n"
    continue
```

**`line.lstrip()` 而不是 `line`** ——缩进在代码块里的围栏缩进也合法。而这一支**在任何情况下都优先于标题匹配**，于是代码块里以 `#` 开头的行（Python 注释、shell 注释）不会被当成标题。没有这个保护，一份带注释的代码文档会被切成大量假章节，而这些假章节的标题会污染 `header_path`。

注意它是一个**布尔翻转**而非栈——即不处理嵌套围栏（本来也不该嵌套）。副作用是：一个未闭合的围栏会让后面所有内容都被当成代码，于是**后续所有标题都失效，整份文档变成一个巨大的节点**。这是这套机制最脆弱的失败模式。

**第二支：标题匹配。**

```
header_match = re.match(r"^(#+)\s(.*)", line)
```

要求 `#` 之后**恰好一个空白**再跟内容。于是 `#标题`（无空格）不匹配，`#######`（七个 `#`）会匹配成 7 级（超出 Markdown 规范的 6 级，但正则不管）。`Re` 的 `^` 与整行匹配，所以标题必须顶格——缩进的 `#` 不是标题。

匹配上时先**把上一节落袋**：

```
if current_section.strip():
    markdown_nodes.append(self._build_node_from_split(
        current_section.strip(), node, self.header_path_separator.join(h[1] for h in header_stack[:-1])))
```

**`header_stack[:-1]` 是关键细节：路径记录的是「祖先」，不含当前这一节的标题。** 因为这一节的标题已经在节点正文的第一行了，路径要表达的是「它在文档树里的位置」。用 `[:-1]` 而不是把整条栈都拼进去，避免了「标题在正文里出现一次、在路径里又出现一次」的重复。

**然后维护栈，注释写得非常清楚：**

```
header_level = len(header_match.group(1))
header_text = header_match.group(2)

# Compare against top-of-stack item's markdown level.
# Pop headers of equal or higher markdown level; not necessarily current stack size / depth.
# Hierarchy depth gets deeper one level at a time, but markdown headers can jump from H1 to H3, for example.
while header_stack and header_stack[-1][0] >= header_level:
    header_stack.pop()
header_stack.append((header_level, header_text))
```

**比较的是「栈顶的层级」与「新标题的层级」，用 `>=` 弹出。** 这处理了两种不规范写法：

- **同级重复**：连续两个 `## 小节`，第二个到来时栈顶是 `(2, 第一个)`，`2 >= 2` 弹掉，于是第二个成为同级兄弟而不是第一个的子节点。
- **跳级**：`# 大标题` 之后直接 `### 小节`，栈顶 `(1, ...)` 不满足 `1 >= 3`，于是 `###` 被**压到 `#` 之下**，路径是 `大标题/小节`。也就是说**层级数字只用于比较，不用于决定深度**——深度由「弹出到第一个层级更小的祖先」这个动作定义。

注释特意点出「not necessarily current stack size / depth」，正是为了说明这件反常的事：栈深度与标题层级数字并不对应。若用 `header_level` 直接作索引（比如「把栈截断到 `header_level - 1` 个元素」），跳级写法就会产生空位或错位。

**然后这一节的正文从标题行本身开始：**

```
current_section = "#" * header_level + f" {header_text}\n"
```

**节点正文包含自己的标题行**（用原始层级重建，而不是原样复制那一行——所以 `# 标题` 与 `#  标题`（两个空格）都会被规范化成 `# 标题`）。这个选择让每个节点是自解释的：单独看一个节点也能知道它属于哪一节。

**第三支：普通行累加**（`current_section += line + "\n"`）。

循环末尾再落袋一次最后一节，逻辑与标题分支里的落袋完全相同。

**`_build_node_from_split` 是「切一段 + 写路径」。**

```
node = build_nodes_from_splits([text_split], node, id_func=self.id_func)[0]

if self.include_metadata:
    separator = self.header_path_separator
    node.metadata["header_path"] = (
        # ex: "/header1/header2/" || "/"
        separator + header_path + separator if header_path else separator
    )
```

三个细节：

1. **每次只传一个片段给工厂**，于是 `build_nodes_from_splits` 里那条「循环外只建一次 SOURCE 关系」的优化在这里**完全失效**——每个章节都重算一次父文档的全文哈希，N 个章节就是 N 次。这不是 bug，但它是「节点工厂被逐段调用」时的性能后果，值得记。
2. **`header_path` 的形态是 `"/a/b/"`**——**前后都带分隔符**。于是「顶层的节」得到 `"/"`（只有分隔符），而 `/a/b/` 可以无歧义地前缀匹配出「`/a/` 下的所有节」。这个前缀友好的格式是为「按章节过滤检索」服务的：想只搜某一章，用 `header_path` 的前缀匹配即可。
3. **`include_metadata` 为假时连 `header_path` 都不写**。于是「不要元数据」这个开关会顺带丢掉结构信息——节点仍然按章节切，但不再知道自己属于哪一节。

### HTML：按标签分组

**核心是一个「连续同名标签合并」的状态机。**

```
last_tag = None
current_section = ""
tags = soup.find_all(self.tags)
for tag in tags:
    tag_text = self._extract_text_from_tag(tag)
    if isinstance(tag, Tag) and (tag.name == last_tag or last_tag is None):
        last_tag = tag.name
        current_section += f"{tag_text.strip()}\n"
    else:
        if current_section.strip():
            html_nodes.append(self._build_node_from_split(current_section.strip(), node, {"tag": last_tag}))
        if isinstance(tag, Tag):
            last_tag = tag.name
        current_section = f"{tag_text}\n"
```

判据是**「当前标签名与上一个相同」**：相同就继续累积（于是 `<li><li><li>` 三个列表项合成一个节点），不同就落袋并开新节。

`DEFAULT_TAGS` 是 `["p", "h1".."h6", "li", "b", "i", "u", "section"]`。把这个列表想清楚就能看出产物的粒度：一组连续段落一个节点、一个标题单独一个节点（因为标题名与前后段落不同）、一组连续列表项一个节点。

**`_extract_text_from_tag` 里那条 `continue` 是防重复计数的关键。**

```
if isinstance(tag, Tag):
    for elem in tag.children:
        if isinstance(elem, NavigableString):
            if elem.strip():
                texts.append(elem.strip())
        elif isinstance(elem, Tag):
            if elem.name in self.tags:
                continue          # 子标签也在待抽取列表里，跳过以免重复
            else:
                texts.append(elem.get_text().strip())
```

因为 `soup.find_all(self.tags)` **会把嵌套的匹配标签也一起返回**。例如 `<section><p>正文</p></section>`：`find_all` 返回 `section` 与 `p` 两个标签，`_extract_text_from_tag(section)` 若把 `p` 的文本也取进来，同一段正文就会出现两次（一次在 `section` 的节点里、一次在 `p` 的节点里）。

**跳过「名字也在 `tags` 里」的子标签，正是为了避开这个重复。** 而这里也埋着一个真实后果：那个 `section` 标签的文本因此**只含它自己的直接文本**，若它只有子标签没有直接文本，`tag_text` 就是空串——代码里为此专门加了「跳过空节」的判断：

```
# Skip empty sections (e.g. a container tag whose only children
# are themselves extracted tags) to avoid emitting blank nodes.
if current_section.strip():
```

**这条注释解释的正是上面那个机制的下游症状**：容器标签被剥空后会产生空节点，所以必须过滤。

`metadata` 只写一个 `{"tag": last_tag}`。**注意 `last_tag` 是「被落袋那一节所属的标签名」**——由于落袋发生在「标签名变化」时，此时 `last_tag` 还是上一节的标签。这个变量的命名有点误导（它既是「上一个」也是「当前累积中」的标签名），但在两个分支里都恰好是对的。

### 元素解析：表格是主角

**三个数据模型定义了「元素」的形状。**

```
class Element(BaseModel):
    id: str
    type: str
    element: Any                      # 原文（文本段 / 表格对象）
    title_level: Optional[int] = None # 若是标题，层级
    table_output: Optional[TableOutput] = None
    table: Optional[Any] = None       # 完美表格的 DataFrame
    markdown: Optional[str] = None
    page_number: Optional[int] = None

class TableOutput(BaseModel):
    summary: str
    table_title: Optional[str] = None
    table_id: Optional[str] = None
    columns: List[TableColumnOutput]

class TableColumnOutput(BaseModel):
    col_name: str
    col_type: str
    summary: Optional[str] = None
```

`Element.type` 有三个取值被代码用到：`"text"`、`"table"`、`"table_text"`。后两个的区分很重要：

- **`"table"`** = 完美解析出的表格，`element.table` 是一个 `DataFrame`；
- **`"table_text"`** = **解析不完美的表格**，只能保留原始文本。

这个区分是为了「宁可给模型一份乱格式的原文，也不要丢掉一张表」。而下游处理两支不同（见下）。

`Element` 用 `Any` 而不是联合类型（`element: Any`、`table: Any`）是为了让子类能塞进 pandas 对象而不触发 pydantic 校验——`model_config = ConfigDict(arbitrary_types_allowed=True)` 就是为此开的。

**表格摘要走「结构化输出 + 文本降级」两步。**

```
index = SummaryIndex.from_documents([Document(text=table_context)])
query_engine = index.as_query_engine(llm=llm, output_cls=TableOutput)
try:
    response = await query_engine.aquery(summary_query_str)
    if isinstance(response, PydanticResponse):
        return response.response
    else:
        raise ValueError(f"Expected PydanticResponse, got {type(response)}")
except (ValidationError, ValueError):
    # 走了校验失败 —— 改用纯文本完成，填 summary、留空其余字段
    query_engine = index.as_query_engine(llm=llm)
    response_txt = await query_engine.aquery(summary_query_str)
    return TableOutput(summary=str(response_txt), columns=[])
```

第一路要求模型**按 `TableOutput` 的 schema 输出结构化结果**（含 summary、table_title、table_id、columns）。若模型没照 schema 输出（`ValidationError`）或返回形态不对（`ValueError`），走第二路：**再问一次，只要一段自由文本**，塞进 `summary`、`columns` 留空。

三点值得注意：

1. **两路都是「再调一次 LLM」**——降级不是解析同一份输出，而是**重跑一遍查询**。代价是校验失败时成本翻倍。
2. **`columns` 留空**的后果会传到下游：`col_schema` 变成空串，表格的「列结构描述」就没了。于是降级路径产出的表格节点质量明显更低。
3. **摘要通过 `SummaryIndex` 而不是直接调 LLM**，这个绕路让「摘要也走一遍索引+查询引擎」——它借用了 RAG 自己的 machinery 来做一次「单文档问答」。笨重，但复用了所有重试、回调、结构化输出的基础设施。

**`_get_context_lines` 是「给表格找标题」的启发式，而它的判据非常土。**

```
lines = element_text.strip().split("\n")
context_lines = lines[-num_lines:] if from_end else lines[:num_lines]
has_table_context = any(line.lower().strip().startswith("table") for line in context_lines)
if has_table_context:
    return "\n".join(context_lines)
return None
```

**只有「某一行以小写化后的 `table` 开头」才认为这段文本是表格的上下文（标题 / 表注）**，然后把它接到表格原文前面一起送去摘要。`from_end` 决定取前 3 行还是后 3 行——前一元素取末尾（挨着表格）、后一元素取开头。

这个启发式的问题很直接：**英文限定**。一份中文文档里「表 3：销售数据」永远不会匹配 `startswith("table")`，于是表格永远拿不到标题。而且它按 `num_lines=3` 只看 3 行——标题离表格超过 3 行就丢。

它是「能用但不通用」的典型：一个为了某个文档集调出来的规则，被写进了核心库。这类启发式在新语言/新格式上失效时，症状是「表格摘要质量莫名下降」，而不会报任何错。

**`extract_table_summaries` 与 `aextract_table_summaries` 是两份近乎逐行相同的实现。**

`_parse_nodes` / `_aparse_nodes` 同理（后者只多一个 `await`）。这是这套代码里**最明显的重复**：同步版本内部用 `asyncio_run(summary_co)` 把异步任务同步化，于是两份代码的唯一实质差别是「`await run_jobs(...)` 还是 `asyncio_run(run_jobs(...))`」。这类重复的代价是「改一处忘另一处」，而它恰好发生在最容易改的逻辑上（重试策略、并发控制）。

两个版本里还有一处细小的不一致：同步版在 `isinstance(response, PydanticResponse)` 不成立时**主动 `raise ValueError`** 以进入降级路径；异步版则直接 `cast(PydanticResponse, response).response`——**不做运行时检查，靠 `cast` 静默假设**。于是同一份代码在同步与异步下对「返回形态异常」的行为不同：前者降级、后者可能在后续访问 `.response` 时抛 `AttributeError`。这是一个真实的双实现漂移。

**`get_base_nodes_and_mappings` 与 `get_nodes_and_objects` 是两次筛选。**

```
node_dict = {node.node_id: node for node in nodes}
node_mappings = {}
base_nodes = []
nonbase_node_ids = set()
for node in nodes:
    if isinstance(node, IndexNode):
        node_mappings[node.index_id] = node_dict[node.index_id]
        nonbase_node_ids.add(node.index_id)
for node in nodes:
    if node.node_id not in nonbase_node_ids:
        base_nodes.append(node)
return base_nodes, node_mappings
```

第一遍：对每个 `IndexNode`，按它的 `index_id` 在 `node_dict` 里查出**被指向的那个节点**，记进映射并把该 id 标记为「非基础」。第二遍：所有不在「非基础」集合里的节点留下。

**能这样做的前提就是前面说的共享 id**：`IndexNode.index_id` 恰好等于 `TextNode.node_id`，于是 `node_dict[node.index_id]` 命中。也就是说「索引节点指向的原文节点」是靠一个**字符串约定**表达的，而不是靠显式的指针或关系枚举。

第二层 `get_nodes_and_objects` 再把 `IndexNode` 挑出来当「对象」：

```
for node in base_nodes:
    if isinstance(node, IndexNode):
        node.obj = node_mappings[node.index_id]
        objects.append(node)
    else:
        nodes.append(node)
```

**于是同一批产物被分成两路：纯文本节点（进向量索引）与「索引节点 + 其 obj」的对象列表（进摘要/对象检索）。** `obj` 字段是运行时挂上去的，不是节点 schema 的一部分。

**`get_nodes_from_elements` 是真正的装配，它的骨架是「文本攒批、遇表冲刷」。**

```
cur_text_el_buffer: List[str] = []
for element in elements:
    if element.type in ("table", "table_text"):
        if len(cur_text_el_buffer) > 0:
            cur_text_nodes = self._get_nodes_from_buffer(cur_text_el_buffer, node_parser)
            nodes.extend(cur_text_nodes)
            cur_text_el_buffer = []
        ... 建表格节点 ...
    else:
        cur_text_el_buffer.append(str(element.element))
```

**文本元素不是逐个变节点，而是先攒成 buffer，遇表格才 flush。** 理由很实在：连续的文本段应当由 `nested_node_parser`（默认 `SentenceSplitter`）**一起切**，否则每个段落都会独立成节点（过碎），且跨段落的句子无法合并。`_get_nodes_from_buffer` 用 `"\n\n".join(buffer)` 把它们拼成一份临时 `Document` 再切。

循环结束后**还要再 flush 一次**——末尾可能有一批没有遇到表格的文本。

**表格序列化成 markdown 是手写的，且注释解释了为什么不调库。**

```
# We serialize the table as markdown as it allow better accuracy
# We do not use the table_df.to_markdown() method as it generate
# a table with a token hungry format.
table_md = "|"
for col_name, col in table_df.items():
    table_md += f"{col_name}|"
table_md += "\n|"
for col_name, col in table_df.items():
    table_md += f"---|"
table_md += "\n"
for row in table_df.itertuples():
    table_md += "|"
    for col in row[1:]:
        table_md += f"{col}|"
    table_md += "\n"
```

标题行与分隔行各是一轮 `for col_name, col in table_df.items()`——**`col_name` 在分隔行那一轮没被使用**，纯粹借这个循环来数「有几个列」以输出对应数量的 `---|`。让一个循环只为了计数而忽略 `as` 绑定，是这段代码里最不优雅的一处（写成 `for _ in table_df.columns` 更直白）。

**「`to_markdown()` 生成的格式太耗 token」** ——这是用具体理由拒绝便利 API 的例子。手写版本没有列对齐（不按最宽列名补空格）、没有多余空白，产物形如「竖线 + 列名 + 竖线」的标题行、紧随的全 `---|` 分隔行、再逐行「竖线 + 单元格值 + 竖线」，每行只花必要的 token。

注意 `row[1:]` 跳过 `itertuples()` 的首元素（`Index`）。默认 `itertuples()` 会把 DataFrame 的索引作为第一个字段，跳过它才能与列头对齐。**如果 DataFrame 的索引本身是有意义的（比如就是第一列数据），这份序列化就会丢掉它**——一个依赖「默认 RangeIndex」的隐含假设。

`"table_text"` 分支则直接 `table_md = str(element.element)`，保留那份不完美的原始文本。

**表格摘要的拼装是一段固定模板。**

```
col_schema = "\n\n".join([str(col) for col in table_output.columns])
table_summary = str(table_output.summary)
if table_output.table_title:
    table_summary += ",\nwith the following table title:\n" + str(table_output.table_title)
table_summary += ",\nwith the following columns:\n"
for col in table_output.columns:
    table_summary += f"- {col.col_name}: {col.summary}\n"
```

也就是说摘要由四段拼成：LLM 的 `summary`、可选的表标题、一句「with the following columns:」、以及**逐列一行**的「列名: 列摘要」。最后一行的 `col.summary` 若为 `None`（列分析没给出摘要），这里会渲染成字面的 `None`——一个小的展示瑕疵。

这段的产物同时充当「索引节点的正文」与「文本节点的开头」。**它的质量完全取决于 LLM 的结构化输出**——降级路径下 `columns` 为空，这段就只剩一句摘要。

**偏移的计算又一次是 `find` 重建。**

```
if ref_doc_text:
    start_char_idx = ref_doc_text.find(str(element.element))
    if start_char_idx >= 0:
        end_char_idx = start_char_idx + len(str(element.element))
    else:
        start_char_idx = None
        end_char_idx = None
```

与 a01 父类里那个 `find` 同一手法，但这里有两个额外缺陷：**没有游标**（多个内容相同的表格会全部指向第一次出现的位置）、**失败时置 `None`**（下游若直接用会得到 `None` 参与运算）。而 `ref_doc_text` 是可选参数，不传时也是 `None`——于是「偏移有没有」取决于调用方是否传了原文。

**共享 id 的构造只有两行，但它是整套双节点机制的枢纽。**

```
# shared index_id and node_id
node_id = str(uuid.uuid4())
index_node = IndexNode(..., index_id=node_id, ...)
text_node = TextNode(id_=node_id, text=table_str, ...)
nodes.extend([index_node, text_node])
```

**用同一个 uuid 分别塞进两个不同的字段**（`IndexNode.index_id` 与 `TextNode.id_`），于是两个节点在 id 空间里「重合」。这就是 `get_base_nodes_and_mappings` 里那次字典查找能成立的全部原因。

而两个节点的**元数据排除设置是不同**的，这个差异精确反映了它们的用途：

```
index_node: excluded_embed_metadata_keys=["col_schema"]          # 列结构不进嵌入
text_node:  excluded_embed_metadata_keys=["table_df", "table_summary"],
            excluded_llm_metadata_keys=["table_df", "table_summary"]
```

- 索引节点的正文是摘要，**列结构作为元数据存在但不参与嵌入**——因为列结构是给「表格理解型检索」用的结构化信息，不该污染摘要的语义向量。
- 文本节点的正文已经是「摘要 + 原文」，所以 `table_summary` 与 `table_df` 两份额外副本**既不进嵌入也不进 LLM**——否则同样的内容会被重复计算三次 token。

**收尾有两件事：继承父节点元数据、丢弃空节点。**

```
for node in nodes:
    if node_inherited and node_inherited.metadata:
        node.metadata.update(node_inherited.metadata)
        node.excluded_embed_metadata_keys = node_inherited.excluded_embed_metadata_keys
        node.excluded_llm_metadata_keys = node_inherited.excluded_llm_metadata_keys
return [node for node in nodes if len(node.get_content(metadata_mode=MetadataMode.NONE)) > 0]
```

元数据是**父节点的键覆盖子节点**（`node.metadata.update(node_inherited.metadata)`）——与 a01 父类里「子节点优先」的方向**相反**。两个排除列表则是整体替换。这个不一致值得记：同一个库里「父 vs 子元数据谁赢」有两套规则，取决于哪一层在合并。

空节点过滤用的是 `MetadataMode.NONE` 的内容长度——于是「正文为空但元数据丰富」的节点**会被丢掉**。对纯元数据节点（比如只有 `col_schema` 的）这意味着它活不下来；它之所以能活，是因为索引节点的 `text` 是摘要（不会为空）。

## 关键取舍

**源码事实：** `MarkdownNodeParser` 用三个状态变量（当前节、标题栈、代码块开关），围栏代码块优先于标题匹配，于是代码里的 `#` 不会被当成标题。

**源码事实：** 标题栈的弹出判据是「栈顶层级的 `>=` 新层级」，比较的是层级数字而非栈深；于是 H1→H3 跳级时 H3 被压到 H1 之下，深度由「弹出到第一个层级更小的祖先」定义（注释特意点明 "not necessarily current stack size / depth"）。

**源码事实：** `header_path` 取 `header_stack[:-1]`（不含本节自己的标题），形态是前后各带分隔符的 `"/a/b/"`，顶层节得到 `"/"`；节点正文包含自己的标题行且层级被规范化。`include_metadata=False` 时连 `header_path` 都不写。

**源码事实：** `HTMLNodeParser` 按「连续同名标签」合并，判据是 `tag.name == last_tag`；`_extract_text_from_tag` 跳过「名字也在 `tags` 里」的子标签以避免重复计数，其后果是容器标签可能被剥空，因此专门加了跳过空节的判断（注释明确说明这一点）。

**源码事实：** `Element.type` 区分 `"table"`（完美解析，带 DataFrame）与 `"table_text"`（不完美，只保留原文），下游两支的处理不同——前者手写 markdown 序列化，后者用原始文本。

**源码事实：** 表格摘要要求模型按 `TableOutput` schema 结构化输出；`ValidationError` / `ValueError` 时**再调一次 LLM**只要自由文本，塞进 `summary` 且 `columns` 留空——降级路径的列结构信息丢失。

**源码事实：** `_get_context_lines` 只在「某行小写后以 `table` 开头」时认为该文本是表格上下文，且只看 3 行；这是英文限定且距离敏感的启发式。

**源码事实：** `extract_table_summaries` 与 `aextract_table_summaries`、`_parse_nodes` 与 `_aparse_nodes` 是近乎逐行重复的两份实现；其中同步版在返回形态不符时主动 `raise ValueError` 进入降级，异步版用 `cast` 静默假设，**两版行为已漂移**。

**源码事实：** 表格节点用**同一个 uuid** 分别作为 `IndexNode.index_id` 与 `TextNode.id_`，「索引节点指向原文节点」靠这个字符串约定表达，`get_base_nodes_and_mappings` 的字典查找据此成立。

**源码事实：** 两个共享 id 的节点排除设置不同：索引节点只把 `col_schema` 排除出嵌入；文本节点把 `table_df` 与 `table_summary` 同时排除出嵌入与 LLM（避免同一内容算三遍 token）。

**源码事实：** `get_nodes_from_elements` 把连续文本元素攒进 buffer，遇表格才用 `nested_node_parser`（默认 `SentenceSplitter`）flush 成节点，循环末尾再 flush 一次。

**源码事实：** 表格 markdown 是手写序列化，注释明确拒绝 `to_markdown()` 因其「token 饥饿」；`row[1:]` 跳过 `itertuples()` 的索引字段，隐含假设 DataFrame 用默认 RangeIndex。

**源码事实：** 表格偏移用 `ref_doc_text.find(str(element.element))` 重建，没有游标（同内容的多个表格会全指第一处），失败时置 `None`；`ref_doc_text` 未传时同样为 `None`。

**源码事实：** 收尾时父节点元数据**覆盖**子节点（`node.metadata.update(node_inherited.metadata)`），与 a01 父类里「子节点优先」的方向相反；两个排除列表整体替换；随后按 `MetadataMode.NONE` 内容长度过滤空节点。

**架构推论：** 「文档自带结构就用结构，不要用长度猜」这件事在 Markdown/HTML 上是纯收益——层级与标签是作者声明的，代价只是多做一层解析。而它把「块边界的确定性」从统计性的（按 token 预算）变成结构性的（按标题/标签），下游因此可以对 `header_path` 做前缀过滤这类精确操作。

**架构推论：** 表格做成「摘要节点 + 原文节点共享 id」的结构，是把 a02 句子窗口那条「检索单位 ≠ 生成单位」推广到了异构元素。摘要短而语义化，适合做检索键；原文长而精确，适合做回答材料。共享 id 是这个映射的全部实现——没有关系枚举、没有图结构，只有一个字符串。

**架构推论：** 用「字符串约定」而不是「显式关系」来表达「索引节点指向原文节点」，省掉了 schema 改动，但也意味着这个约定**无法被校验**。若某天索引节点的 `index_id` 没有对应的 `node_id`，`node_dict[node.index_id]` 会 `KeyError`——错误发生在装配后的筛选阶段，离出错点很远。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/node_parser/file/markdown.py` | `MarkdownNodeParser` | 按标题切节；标题栈维护祖先路径、围栏代码块保护 |
| `llama-index-core/llama_index/core/node_parser/file/html.py` | `HTMLNodeParser` | 按连续同名标签分组；跳过嵌套匹配标签以防重复计数 |
| `llama-index-core/llama_index/core/node_parser/file/html.py` | `_extract_text_from_tag` | 抽取标签文本，排除子标签名在 `tags` 里的内容 |
| `llama-index-core/llama_index/core/node_parser/file/html.py` | `DEFAULT_TAGS` | 默认抽取的标签集合，决定产物粒度 |
| `llama-index-core/llama_index/core/node_parser/relational/base_element.py` | `BaseElementNodeParser` | 元素抽提的抽象基类：表格摘要、双节点装配、两路筛选 |
| `llama-index-core/llama_index/core/node_parser/relational/base_element.py` | `Element` | 元素模型：类型、原文、标题层级、表格产物与 DataFrame |
| `llama-index-core/llama_index/core/node_parser/relational/base_element.py` | `TableOutput` / `TableColumnOutput` | 表格摘要的结构化输出 schema（列名、列类型、列摘要） |
| `llama-index-core/llama_index/core/node_parser/relational/base_element.py` | `get_nodes_from_elements` | 文本攒批、表格双节点装配、共享 id、偏移重建 |
| `llama-index-core/llama_index/core/node_parser/relational/base_element.py` | `get_base_nodes_and_mappings` | 靠共享 id 把索引节点与其原文节点分离 |

## 参考

- [llama-index 源码：node_parser/file/markdown.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/node_parser/file/markdown.py)
- [llama-index 源码：node_parser/relational/base_element.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/node_parser/relational/base_element.py)
- [llama-index 文档：基于元素的分块](https://docs.llamaindex.ai/en/stable/module_guides/loading/node_parsers/modules/)
- [CommonMark 规范：ATX 标题](https://spec.commonmark.org/0.30/#atx-heading)

## 与主核 / AI 的关系

「按结构而不是按长度切」在主核里对应的是**按存储结构而不是按页边界读取**：读一张表不该按 8KB 页切，读一棵 B-tree 不该按行偏移切。这里有四条经验。

**第一，结构信息应当写进元数据并做成可过滤的形态。** `header_path` 的 `"/a/b/"` 形态（前后带分隔符）让前缀匹配就能筛出「某一章下的所有节点」。主核里若给派生行写「来源路径」（库/表/分区），同样应当做成前缀友好的规范字符串，而不是分散的三个字段——前者一次 `LIKE` 就能筛，后者要三段拼接。

**第二，异构元素必须允许「一份数据、两种形态、共享身份」。** 表格的摘要节点与原文节点共享 id，是这个设计能成立的全部。主核里对应的是「索引条目与数据块共享同一个定位」（a02 里也提过：B-tree 条目指向数据页）。关键是**共享的是身份而不是内容**，否则就要维护两份可能不同步的副本。

**第三，「拒绝便利 API 因为输出太啰嗦」是一个正当理由。** `to_markdown()` 被手写序列化替代，注释说明理由是 token 开销。主核里同样存在「库函数的输出格式不适合下游」的情况（JSON 序列化的多余空白、日志的冗余前缀）。判断标准是「下游的预算是硬约束吗」——对 LLM 上下文，token 就是硬约束。

**第四，双实现（同步与异步）必然漂移，而漂移点通常在最细的分支上。** 这里同步版做了运行时类型检查并降级，异步版用 `cast` 跳过，于是「返回形态异常」在两版下的行为不同。主核里若为同一逻辑维护 sync/async 两份（或 C/Go 两份），应当把**分支逻辑抽成共享函数**，两份只保留「怎么等待」的差异。

还有一条关于启发式的：`_get_context_lines` 用「行是否以 `table` 开头」来认表格标题，是**英文限定且距离敏感**的规则。这类规则在核心库里应当要么可配置（正则 + 语言无关的默认），要么明确标为「适用于英文 PDF 场景」。否则它在别的语种上失效时，症状只是「摘要质量变差」，没有任何错误可循。
