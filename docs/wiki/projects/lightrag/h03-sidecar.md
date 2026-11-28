---
project: lightrag
feature: sidecar
title: sidecar 写入与占位符
page_type: feature
schema_version: 1
refs:
  - path: lightrag/sidecar/ir.py
    symbols: [IRDoc, IRBlock, IRPosition]
  - path: lightrag/sidecar/placeholders.py
    symbols: [render_template, render_table_tag, table_body_for_rows]
  - path: lightrag/sidecar/backfill.py
    symbols: [backfill_chunk_sidecars]
---

# sidecar 写入与占位符

## 一句话职责

把「各家解析引擎的输出」统一成一份**进程内中间表示**（`IRDoc`），再由唯一的写者把它渲染成规范化的 `*.parsed/` 目录；而页面正文里的图、表、公式先以 `{{TBL:k}}` 这类**占位符**出现，等 id 分配完才替换成规范标签——另外给那些「不经过这套格式」的分块策略补一条回溯通道，让它们的块也能指回自己来自哪一行 `blocks.jsonl`。

## 机制是什么

三个文件各占一个位置：

- `ir.py`（238 行）定义中间表示：`IRDoc` → `IRBlock` → 表格/图/公式四类子对象，加 `IRPosition` 与 `AssetSpec`。
- `placeholders.py`（128 行）定义占位符语法与四个渲染函数。
- `backfill.py`（227 行）是反向通道：给没有 provenance 的分块**重建** provenance。

**为什么要进程内 IR 而不是序列化中间格式？** `ir.py` 的模块 docstring 给了三条理由：

> - **一个可执行规范点。** `writer.py` 是唯一知道 id 格式、占位标签、blockid 计算、`asset_dir` 真值的地方。
> - **引擎适配器只做翻译**；它们从不内嵌关于磁盘格式的知识。
> - 下面的 dataclass 覆盖规范契约，并在条目级对象上留一个 `extras` 逃生口，好让引擎特有的信号（rowspan、OCR 置信度……）能透传而不引起规范改动。

所以这一层要解决的是「N 个引擎 × 1 份格式」的组合爆炸：如果每个适配器都自己拼 JSON，格式细节会散在 N 处并且必然漂移；现在是 N 个适配器 + 1 个写者，格式只在写者里实现一次。

**占位符的理由是「id 还没分配」。** `id` 形如 `tb-<doc_hash>-0001`，而 `doc_hash` 依赖合并文本，合并文本又依赖块内容——所以正文渲染时不可能知道 `id`。解法是正文先写占位符，等 id 分配完再由 `render_template` 一次性替换。这是「两阶段渲染」，与模板引擎的「先编译后填充」同构。

四条占位符语法：

| token | 含义 | 是否进 `*.json` |
|---|---|---|
| `{{TBL:k}}` | 表格 | 是（`tables.json`） |
| `{{IMG:k}}` | 图 | 是（`drawings.json`） |
| `{{EQ:k}}` | 块级公式（`is_block=True`） | 是（`equations.json`） |
| `{{EQI:k}}` | 行内公式（`is_block=False`） | **否**，渲染时不带 id |

## 怎么实现的

**`IRDoc` / `IRBlock` / `IRPosition` 的分层是按「谁拥有谁」定的，而它决定了一条重要的写入规则。**

`IRDoc` 拥有 `blocks: list[IRBlock]` 与 `assets: list[AssetSpec]`；`IRBlock` 拥有 `tables` / `drawings` / `equations` 三个列表；而 `IRTable` 等条目对象各有一个 `placeholder_key`。

所以「占位符的键在同一个块里唯一」是一条可检查的局部性质——`ir.py` docstring 写：

> 写者在 id 分配之后展开这些模板。**适配器必须为每个条目发出恰好一个占位符**；同一个键的多个正文内占位符不被支持。

「一个条目一个占位符」这条限制是简化的关键：若允许一个表格在正文里出现两次，那两处必须渲染成同一个 `id`，而 `id` 是按「出现顺序」编号的（`table_seq`），于是第二次出现要么复用第一个 id（语义正确但需要额外映射）要么得到新 id（那就有两张表）。禁止这种情况让 `table_seq` 与占位符一一对应。

**「未声明的 token 是源文本而不是占位符」这条规则记录了一次行为变更。**

> 一个键不在同一个块上声明的 token **是源文本，不是占位符**：写者原样保留它（**它过去会被渲染成空字符串**）。写者无法区分一个作者写的 token 与一个生成出来但键撞上了它的事件，所以把一个源文本复制进 `content_template` 的适配器**必须分配避开那段文本里每个已有 token 的键**；原生 Markdown 构建器用 `PLACEHOLDER_TOKEN_RE` 在整个文档范围内预留它们。

这条很有价值，它同时说明了三件事：

1. **行为已从「替换成空串」改成「原样保留」**——因为前者会**静默删除用户数据**（一个恰好写着 `{{TBL:x}}` 的段落会消失）。
2. **语义模糊无法在写者处解决**：写者看到 `{{TBL:x}}` 无法区分「我的占位符」与「用户的字面文本」。
3. **责任因此转移给适配器**：必须分配不冲突的键，而做法是用**同一个正则**扫描源文本并预留。

第 3 点是关键：`PLACEHOLDER_TOKEN_RE` 的注释说明了这个复用：「第 1 组是 token 类型，第 2 组是占位符键。**必须让生成的键避开作者写的 token 的适配器用同一个模式扫描**，所以写者会展开的 token 永远是他们预留过的那些」。

所以「同一个正则两处用」不是巧合，而是**保证「预留集合」与「展开集合」完全重合**的必要条件。若两处各写一份正则，一个微小的差异就会让某些 token 被展开但没被预留——那正是静默改数据的路径。

**`render_table_tag` 里有一句「所以没有适配器能发出提前结束标签的单元格」。**

```python
if fmt == "json":
    body = escape_json_table_body(body)
return (
    f'<table id="{xml_attr_escape(table_id)}" '
    f'format="{xml_attr_escape(fmt)}">{body}</table>'
)
```

docstring：「一个 `json` body 在这里被 `escape_json_table_body` 处理，**所以没有适配器能发出一个会提前结束标签的单元格文本 `</table>`——无论 body 是从 `rows` 编码来的还是逐字提供的**」。

这是「把不变量放在写者里」的具体做法：h02 那页讲过 `</table>` 出现在 body 里会让外层解析提前结束，而正确做法是写者统一转义。这里进一步指出：**即使适配器自己拼好了 JSON body 传进来，也逃不过这一层**——所以不变量不需要每个适配器都记得。

而 `table_body_for_rows` 只是 `json.dumps(rows, ensure_ascii=False)` ——它**不做**转义，转义在 `render_table_tag` 里做。所以两个函数的职责是分开的：「编码」与「放进标签」。若 `table_body_for_rows` 自己转义，而调用方又传给 `render_table_tag`，就会转义两次——第一次的 `<\/table>` 在第二次时不含 `</table>`，所以幂等（h02 里说过 `escape_json_table_body` 是幂等的），但这是运气好而不是设计。

**`render_equation_tag` 是唯一有两种形态的渲染函数，而「行内不带 id」这条规则有下游后果。**

```python
if eq_id is None:
    return f'<equation format="latex"{caption_attr(caption)}>{latex}</equation>'
return (f'<equation id="{xml_attr_escape(eq_id)}" format="latex"{caption_attr(caption)}>{latex}</equation>')
```

docstring：「行内公式（`eq_id is None`）：`<equation format="latex">latex</equation>` ——**没有 id，永远不写进 `equations.json`**」。

所以「是否进 json」不是由某个标志单独决定的，而是由**有没有 id** 决定的——一个 `id` 都没有的元素自然无法被其他对象引用，也就不需要元数据文件。这个设计让「行内公式」不需要任何额外的排除逻辑。

而 `IRDrawing` 的 docstring 记录了另一个「字段有但不使用」的情形：

> `path` 是只在本地有意义的：`asset_ref=""` → 写者渲染 `path=""`。`src` 携带原始引用（远端 URL / docx 外部链接目标）**只作为元数据——它从不被解析到磁盘**。

所以一个图有两种位置信息：`path`（本地资产目录里的相对路径，可打开）与 `src`（原始远端地址，只供追溯）。分开的理由很清楚：一个 docx 里嵌入的图没有远端地址，一个网页抓来的图在本地也没路径。

**`writer.py` 的 `_equation` 里有一处「适配器写错了也别留下悬空 token」的容错。**

```python
def _equation(key: str) -> str:
    eq = equations_by_key.get(key)
    if eq is None:
        return f"{{{{EQ:{key}}}}}"
    if not eq.is_block:
        # Adapter mistake: an EQ token should only be used for block
        # equations. Treat as inline to avoid a dangling token.
        return render_equation_tag(None, eq.latex, eq.caption)
    ...
```

注释：「适配器错误：`EQ` token 只该用于块级公式。**当作行内处理以避免悬空 token**」。

所以这里的容错方向是「降级成可用的产物」而不是「报错」。理由：`EQ` 用于 `is_block=False` 的公式是一个明显的适配器 bug，但公式内容本身是有效的——渲染成不带 id 的行内形式是正确的产物，只是不会进 `equations.json`。这个降级比抛异常好，因为抛异常会让整个文档解析失败，而降级的损失只是「这个公式没有独立元数据」。

而两个 `if eq is None: return f"{{{{EQ:{key}}}}}"` 分支则是「保留原样」——与 `ir.py` 里「未声明的 token 是源文本」一致。

**`AssetSpec.source` 的三态是「文件可能已经在位」这个现实的直接编码。**

```python
source: Path | bytes | None = None
```

docstring：

> - `Path` 指向磁盘上已存在的文件（写者复制它）；
> - `bytes` 载荷（写者倾倒它）；
> - `None` 当文件已经在 `<assets_dir>/<suggested_name>` 就位时（例如原生 docx 解析器在抽取期间就写了资产）；写者随后**只记录它的大小而不碰它**。

所以这是三个不同来源的同一种资产。第三种最关键：一个解析器可能在做抽取时顺手把图落盘了，此时再复制一遍是浪费；而写者仍需要知道它存在（为了 `asset_dir` 真值与元数据）。

而「carrier protocol」那段说明了引用链：「一个图通过 `ref` 引用资产；写者把那个解析成资产目录里的一个具体文件名，并把结果写到 `drawings.json`（完整相对路径）**与** `blocks.jsonl` 里的 `<drawing path>` 属性**两处**」。

所以同一个路径要写进两个地方，而这两个地方必须一致——这正是「写者是唯一知道格式的地方」的价值：一致性由单点保证。

**`_drawing` 里的 `block_drawing_path_style` 是一个输出格式开关。**

```python
if not filename: path = ""
elif block_drawing_path_style == "basename_only": path = filename
else: path = f"{asset_prefix}{filename}"
```

`_VALID_BLOCK_DRAWING_PATH_STYLES = {"with_prefix", "basename_only"}` —— 两种风格：带前缀的完整相对路径，或只有文件名。

为什么需要这个开关？因为消费方不同：一个从 `blocks.jsonl` 所在目录读取的消费方需要相对路径，而一个把资产都平铺到同一处的消费方只需要文件名。所以这是一个「同一份数据、两种寻址约定」的兼容点。

**`backfill.py` 解决的是一个完全不同的问题：跨策略的 provenance 缺失。**

模块 docstring 开头把场景说清了：

> `P`（paragraph_semantic）策略在每个 chunk 上记录一个 `sidecar` 字段，把它映射回解析期 `*.blocks.jsonl` 里它的源行。`F` / `R` / `V` 策略分块合并后的文档文本时**没有那段 provenance**，但每个 chunk 带一个私有的 `_source_span`——它的内容在分块器收到的合并文本里的半开 `[start, end)` 字符偏移。当文档被解析成 LightRAG sidecar 格式（`blocks.jsonl` 存在）时，`backfill_chunk_sidecars` 重建那段合并文本、记录每个块的位置，并通过把 `_source_span` 映射到它重叠的块上给每个 chunk 附加 `sidecar`。

所以这不是「生成 provenance」而是「**从字符偏移反推 provenance**」：块边界在 `blocks.jsonl` 里，chunk 边界在 `_source_span` 里，两者的交叠就是答案。

而它成立的前提写在「Span contract」小节：

> 合并文本从 `blocks.jsonl` **完全可复现**：`write_sidecar` 与 `load_lightrag_document_content` 都把它构造成 `"\n\n".join(raw_content for content rows where content.strip())`。我们在这里重建同一个字符串，所以一个 chunk 的 `_source_span` 直接索引进它。

「完全可复现」这四个字是全部正确性的基础——若两边构造方式有任何差异（分隔符、过滤条件、顺序），偏移就全部错位，而错位是静默的（不会抛，只会指向错误的块）。

**三种策略的 span 精度不同，而校验因此是两级的。**

docstring：

> F/R 发出逐字匹配的 span（span 文本等于 chunk 内容）；F 在字符边界切窗口，所以这对多字节文本也成立。**V 的 `SemanticChunker` 用单个空格重新拼接句子，所以一个 V chunk 的内容可能与它的 span 文本只差空白**；span 校验因此接受逐字匹配**或**去空白匹配（每个空白字符都删掉，而不只是折成一个空格）。一个两者的不匹配的 span 被当作**不存在**。

所以校验策略是「宽松到能容纳唯一已知的偏差，但不对其他偏差让步」。而「去空白」而不是「折叠空白」这个选择有具体理由：「删除所有空白（而不是把连续空白折成一个空格）让两边即使 V 在原本相邻的句子之间插了一个空格也能对齐」。

`_normalize_text` 就是那一步：

```python
return "".join(text.split())
```

`str.split()` 无参调用按任意连续空白切分并丢弃空串，`join("")` 就把所有空白删掉了。

**`_chunk_source_span` 的四道检查，而每一道都把「不可用」映射成 `None`。**

```python
span = chunk.get("_source_span")
if not isinstance(span, dict): return None
try:
    start = int(span["start"]); end = int(span["end"])
except (KeyError, TypeError, ValueError): return None
if start < 0 or end <= start or end > len(merged): return None
body = chunk.get("content", "")
if not isinstance(body, str): return None
source_text = merged[start:end]
if source_text != body and _normalize_text(source_text) != _normalize_text(body): return None
return start, end
```

四道：形状（是不是 dict）、可转换（`int` 能不能转）、范围（非负、非空、不越界）、**内容一致性**（逐字或去空白）。

第四道是最重要的：它证明「这个 span 真的指向这段内容」，而不仅仅是「这个 span 在范围内」。没有它的话，一个错误的偏移会静默指向一段无关文本，于是 chunk 被归到错误的块上——而那种错误在检索时表现为「引用来源不对」，极难归因。

而 `start >= 0` 与 `end > start` 这两条看似显然，实际是防 `_source_span` 来自不可信来源（它可能被序列化/反序列化过）。

**`_covered_blockids` 用半开区间重叠测试，而注释点明「间隙匹配不到任何东西」。**

```python
if start < o_end and o_start < end and blockid and blockid not in seen:
```

`start < o_end and o_start < end` 是半开区间重叠的标准写法（注意都是严格不等）。而注释：「半开区间上的重叠测试；**分隔符（间隙）匹配不到任何东西**」。

这解释了 `_build_block_spans` 里那个细心的地方：

```python
if parts: cursor += len(_BLOCK_SEPARATOR)
start = cursor
end = start + len(content)
```

分隔符的长度被计入 `cursor` 但**不属于任何 span**。所以一个 chunk 若恰好落在两个块之间的 `\n\n` 上，`covered` 会是空——而代码里对此有兜底：

```python
if not covered:
    # The span landed entirely on separator gaps -- should not happen for
    # non-empty content, but guard rather than emit an empty ref.
    raise ChunkBlockMatchError(...)
```

注释说「对非空内容这不该发生，但守卫而不是发出一个空 ref」。为什么「不该发生」？因为一个非空的 chunk 内容不可能只由 `\n\n` 组成（那道「去空白后不等」的校验会拦住纯空白内容）。所以这条守卫是防御性的。

**而 `backfill_chunk_sidecars` 的失败处理是这一页最硬的决定：没有可用 span 就抛错让文档 FAILED。**

```python
source_span = _chunk_source_span(chunk, merged)
if source_span is None:
    # No usable span: provenance is impossible. FAIL the document rather
    # than guess (the old text-matching fallback could not disambiguate a
    # genuine cross-block match from a whitespace-glue artifact).
    raise ChunkBlockMatchError(
        chunk_order_index=int(chunk.get("chunk_order_index", -1)),
        chunk_preview=body,
        blocks_path=blocks_path,
    )
```

注释说明了为什么不用旧的文本匹配兜底：「**旧的文本匹配兜底无法区分一次真正的跨块匹配与一次空白粘合的人造产物**」。

这是「宁可失败也不要猜」的典型论证：文本匹配的假阳性会给出**看起来合理但实际错误**的 provenance，而错误的 provenance 比没有 provenance 更糟——因为下游（删除、清除、引用）会基于它做破坏性操作。g04 那页的「不完整锚点会授权不完整的清除」是同一类问题。

而 docstring 明确加了一句：「**任何内容都不能豁免一个 chunk 于这一点**（其中的一个 U+FFFD 是源自己的字符）」——所以「内容怪异」不是跳过的理由。

**跳过规则被精确地分成三类，而顺序有讲究。**

```python
if normalize_chunk_sidecar(chunk) is not None: continue     # 已有有效 sidecar
body = chunk.get("content", "")
if not isinstance(body, str) or not body.strip(): continue    # 空内容
```

前两类跳过：已有 provenance 的（P / 多模态）与空内容的。而空内容跳过的理由显然：空 chunk 没有 `_source_span` 可言。

注意「已有 sidecar」用的是 `normalize_chunk_sidecar(chunk) is not None` ——一个**规范性检查**而不是「有没有 `sidecar` 键」。所以一个畸形 sidecar 会被当成「没有」并走 backfill 路径，而不是被跳过。

**`_load_content_blocks` 里有一处「按类型跳过头行」与「按索引跳过头行」的对比，值得记。**

```python
# Note: ``utils_pipeline.load_lightrag_document_content`` (which produces the
# merged text the chunker actually received) skips line 0 *by index*; here we
# skip *by type* instead. The two agree only because ``writer.write_sidecar``
# always emits the meta header as the very first line -- under that invariant
# skip-by-type is equivalent, and it stays correct even if a stray non-content
# row ever appears mid-file.
```

所以两处实现不同（索引 vs 类型），而它们只在「元数据头永远是第一行」这个不变量下等价。注释判断：按类型更鲁棒（中间出现杂散的非 content 行也不会错）。

但这里有一个隐含的脆弱点值得指出：**「等价」是单向的**。按类型跳过在「头行是 meta」时与按索引一致；但若 `load_lightrag_document_content` 那边改成按类型，而 `check` 那边仍按索引，一旦头行不是 meta 就会错位。所以这条注释实际上记录了一个**跨模块的不变量依赖**——而它只写在一侧的注释里。

**`_load_content_blocks` 的容错是「静默跳过」而不是报错，而这对一个「重建合并文本」的函数是必需的。**

```python
try: obj = json.loads(line)
except json.JSONDecodeError: continue
if not isinstance(obj, dict) or obj.get("type") != "content": continue
content = obj.get("content", "")
if not isinstance(content, str): continue
```

三层：JSON 坏了跳过、不是 content 行跳过、content 不是字符串跳过。

若这里报错，那么一个含一行坏数据的 `blocks.jsonl` 会让整个文档的 backfill 失败——而 backfill 是可选增强（没有它 chunk 仍能工作）。所以静默跳过是合适的：坏行会让合并文本短一块，导致后续的 span 校验失败，而那个失败会**带上下标与预览**地抛出——错误信息比在加载处抛要丰富得多。

这是一个「延后报错以获得更好诊断」的例子：加载处的错误只能说「第 137 行不是合法 JSON」，而校验处的错误能说「order 42 的 chunk 内容与其 span 不匹配，预览是 …」。

## 关键取舍

**源码事实：** 用进程内 IR 而非序列化中间格式的三条理由：写者是唯一知道 id 格式/占位标签/blockid 计算/`asset_dir` 真值的地方；适配器只做翻译；条目级对象有 `extras` 逃生口以防规范churn。

**源码事实：** 四条占位符 token（`TBL`/`IMG`/`EQ`/`EQI`），且「适配器必须为每个条目发出恰好一个占位符」——同一个键的多个正文占位符不被支持。

**源码事实：** 一个键未被同块声明的 token **是源文本、原样保留**（过去会被渲染成空串，即静默删数据）；写者无法区分作者写的 token 与键撞上的生成 token，所以责任转移给适配器：用**同一个** `PLACEHOLDER_TOKEN_RE` 在文档范围内预留。

**源码事实：** `render_table_tag` 对 `json` body 统一走 `escape_json_table_body`，「所以没有适配器能发出提前结束标签的单元格」——不变量放在写者里，适配器不必各自记得。

**源码事实：** 行内公式不带 id，因此**天然**不进 `equations.json`——「是否进 json」由「有没有 id」决定而不靠额外标志。

**源码事实：** `IRDrawing` 的 `path`（本地资产相对路径）与 `src`（原始远端引用，只作元数据、从不解析到磁盘）分开。

**源码事实：** `AssetSpec.source` 三态：`Path`（复制）、`bytes`（倾倒）、`None`（已在位，只记大小不碰它）。

**源码事实：** `block_drawing_path_style` 有 `with_prefix` / `basename_only` 两种风格，为两种消费方的寻址约定服务。

**源码事实：** `_equation` 对「`EQ` token 用在 `is_block=False` 的公式上」这种适配器错误**降级为行内**而不是报错——「以避免悬空 token」，损失仅是这个公式没有独立元数据。

**源码事实：** `backfill` 成立的前提是合并文本「完全可复现」：`"\n\n".join(raw_content for content rows where content.strip())`，两侧（写者与 `load_lightrag_document_content`）必须一致，否则偏移全部静默错位。

**源码事实：** span 校验两级：逐字匹配或**去空白**匹配（删除所有空白，不是折叠），后者专为 V 的 `SemanticChunker`——它用单空格重拼句子。

**源码事实：** 没有可用 `_source_span` 的 chunk **抛 `ChunkBlockMatchError` 让文档 FAILED**，因为「旧的文本匹配兜底无法区分真正的跨块匹配与空白粘合的人造产物」；「任何内容都不能豁免」，含 U+FFFD 也不行。

**源码事实：** `_build_block_spans` 把 `\n\n` 的长度计入游标但**不归任何 span**，所以落在间隙上的 chunk 会得到空的 `covered` 并被守卫拦下。

**源码事实：** `_load_content_blocks` 按 `type` 跳过头行而 `load_lightrag_document_content` 按索引跳；两者只在「meta 头永远是第一行」这个不变量下等价。

**源码事实：** 加载期的坏行**静默跳过**而不是报错，因为 backfill 是可选增强，而「延后到 span 校验处失败」能给出带 `chunk_order_index` 与内容预览的错误。

**架构推论：** 「两阶段渲染」是「id 依赖内容、内容里要放 id」这个循环依赖的标准解法。这里 `doc_hash` 依赖合并文本、合并文本依赖块内容、块内容里要放 `tb-<doc_hash>-…` 的标签——所以正文必须先写占位符、后替换。任何「对象需要引用自己所属整体的标识」的场景都会撞上同一个环，而解法都是先占位后填充。

**架构推论：** 「未声明的 token 原样保留」这个改动值得单独强调，因为它把一次**数据破坏**转成了**显示问题**。原来的「渲染成空串」是静默删除用户文本；现在的「原样保留」最多是正文里露出一段 `{{TBL:x}}`。这类改动在评审时经常被当成「边界情况处理」，但它实际是把失败从「不可见」移到「可见」——而这类改动优先级应该很高，因为不可见的失败永远不会被报告。

**架构推论：** `backfill` 的「宁可 FAILED 也不猜」在 RAG 系统里有额外分量：provenance 不只是元数据，它是**删除与清除的依据**。一条错误的 provenance 会让「删除文档 A」的操作去动文档 B 的数据，或者在清除时留下孤儿。所以这一层的严格度必须高于「展示用元数据」的严格度——同一种数据在不同用途下需要不同的可靠性等级。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/sidecar/ir.py` | `IRDoc` | 顶层 IR，`write_sidecar` 的唯一输入 |
| `lightrag/sidecar/ir.py` | `IRBlock` | 一个内容块与它的子对象列表 |
| `lightrag/sidecar/ir.py` | `IRPosition` | 块级位置，`origin` 可按位置覆盖文档默认 |
| `lightrag/sidecar/ir.py` | `IRTable` | 表格条目，`rows`（JSON）或 `html` 二选一 |
| `lightrag/sidecar/ir.py` | `IRDrawing` | 图条目，`path` 本地 / `src` 原始引用分开 |
| `lightrag/sidecar/ir.py` | `IREquation` | 公式条目，`is_block=False` 即行内不进 json |
| `lightrag/sidecar/ir.py` | `AssetSpec` | 资产三态来源（`Path` / `bytes` / `None`） |
| `lightrag/sidecar/placeholders.py` | `PLACEHOLDER_TOKEN_RE` | 四类 token 的单一正则，预留与展开共用 |
| `lightrag/sidecar/placeholders.py` | `render_template` | 按 token 类型分派到四个渲染器 |
| `lightrag/sidecar/placeholders.py` | `render_table_tag` | 统一转义 json body，防提前结束标签 |
| `lightrag/sidecar/placeholders.py` | `render_equation_tag` | 块级带 id、行内不带 id 两种形态 |
| `lightrag/sidecar/placeholders.py` | `xml_attr_escape` | 四个 XML 属性字符的转义 |
| `lightrag/sidecar/placeholders.py` | `table_body_for_rows` | 只编码不转义，转义留给标签渲染 |
| `lightrag/sidecar/backfill.py` | `backfill_chunk_sidecars` | 从 `_source_span` 反推 provenance 的总入口 |
| `lightrag/sidecar/backfill.py` | `_build_block_spans` | 重建合并文本并记录每块 span，间隙不归任何块 |
| `lightrag/sidecar/backfill.py` | `_chunk_source_span` | 四道检查，逐字或去空白比对 |
| `lightrag/sidecar/backfill.py` | `_normalize_text` | 删除全部空白（不是折叠），容纳 V 的空格重拼 |
| `lightrag/sidecar/backfill.py` | `_covered_blockids` | 半开区间重叠，按序去重 |
| `lightrag/sidecar/backfill.py` | `_load_content_blocks` | 按 type 跳过 meta 头，坏行静默跳过 |

## 参考

- [lightrag 源码：sidecar/ir.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/sidecar/ir.py)
- [lightrag 源码：sidecar/placeholders.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/sidecar/placeholders.py)
- [lightrag 源码：sidecar/backfill.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/sidecar/backfill.py)
- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)

## 与主核 / AI 的关系

主核里最接近的是**逻辑记录格式与物理表示的分离**：上层写「一条记录有这些列、这些值」，存储层负责把它变成页、槽、偏移。这一层的 `IRDoc` 就是那个逻辑格式，`writer.py` 就是那个「唯一知道物理布局的地方」。

**差别在于 id 的分配时机。** 主核可以用单调递增的行号或物理位置做标识，因为它知道整体布局；而这里 `doc_hash` 依赖合并文本，合并文本又包含要渲染的标签——所以 id 只能在内容已经确定之后分配，但内容里又要放 id。这个环通过两阶段渲染绕开，而主核没有这个问题（它没有「内容里嵌自身标识」的需求）。

与 h01（多模态上下文）/ h02（表格标记）的关系是一条链：本页的 `render_table_tag` 产出 h02 能解析的标签；h02 的 `escape_json_table_body` 被本页用来保证 body 不含 `</table>`；h01 的 `find_target_span` 依赖本页写出的 `id="tb-…"`。

与 c05（chunk schema）的关系：`backfill` 产出的 `sidecar` 字段形状由 `normalize_chunk_sidecar` 校验，而它是那边的下游契约。

对 AI 工程最实用的一条是**「占位符与字面量必须能在同一个位置上区分」**。任何「先写模板后替换」的系统都会遇到用户文本恰好长得像模板的情况。这里的处理值得照抄：**把「认不出的 token」当字面量保留**（而不是替换成空），**并让生成方用同一个正则预留键空间**。前者避免静默删数据，后者避免生成与预留不一致。

另一条是 `backfill` 的「宁可 FAILED 不猜」。provenance / 血缘这类元数据在 RAG 系统里是**破坏性操作的依据**（删除、清除、重索引），所以它的错误代价远高于它的缺失代价。判据是：**这条元数据会被用来做不可逆操作吗？** 会的话，缺失必须报错，不可猜测。

最后一条是「延后报错以获得更好诊断」。`_load_content_blocks` 跳过坏行而不报错，最终在 span 校验处给出「order 42 的 chunk 不匹配，预览是 …」。同样的失败信息在加载处只有「第 137 行坏了」，而在校验处能指出**哪个下游对象受影响**。所以错误应该在「掌握上下文最多的那一层」抛出——哪怕那一层离根因更远。
