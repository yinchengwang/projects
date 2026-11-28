---
project: ragflow
feature: chunker-components
title: 四种分块组件
page_type: feature
schema_version: 1
refs:
  - path: internal/ingestion/component/chunker/token.go
    symbols: [TokenChunkerComponent, tokenChunkerParam]
  - path: internal/ingestion/component/chunker/title.go
    symbols: [TitleChunkerComponent, outlineEntry]
  - path: internal/ingestion/component/chunker/general.go
    symbols: [GeneralChunkerComponent]
  - path: internal/ingestion/component/chunker/group.go
    symbols: [GroupTitleChunkerComponent]
---

# 四种分块组件

## 一句话职责

四个分块器解决四类不同的「块边界从哪来」的问题：`Token` 按 token 预算切（边界来自预算）、`Title` 按标题层级切（边界来自文档结构）、`General` 按文件族选策略（边界来自格式惯例）、`GroupTitle` 在同一章节内按 token 合并小片（边界来自章节 + 预算的双约束）。

## 机制是什么

分块的核心问题是**边界判据从哪来**。四种组件给的是四个不同答案，而它们的选择不该由用户随便挑，应由**文件类型与内容形态**决定：

| 组件 | 边界来源 | 适用 | 关键约束 |
|---|---|---|---|
| `Token` | 分隔符列表 + token 预算 | 任意文本、Markdown、HTML | 纯长度驱动，不理解结构 |
| `Title` | 层级正则 / PDF 大纲 / 手工标题 | 有明确章节结构的文档 | 结构驱动，可能产出极短块 |
| `General` | 文件族自带惯例（PDF/DOCX/MD/表格） | 「不知道用什么」时的默认 | 策略按扩展名分派 |
| `GroupTitle` | 章节边界内按 token 合并 | 章节很碎（如 PDF 逐行） | 双约束，先定章节再合片 |

**第二个共同问题是「上游可能给什么」。** 上游（b02 的 Parser、或另一个 Chunker）可能输出三种格式：`markdown`/`text`/`html` 的**纯文本**，或 `json`/`chunks` 的**结构化条目**。`Token` 与 `GroupTitle` 都要按这两种形态分路走，而且**纯文本路径会丢掉媒体信息**（注释说得很清楚：「Plain payloads emit only "text"; structured payloads (chunks/json) also carry doc_type_kwd and img_id」）。

第三条是 **`Token` 必须能接在另一个 Chunker 后面**，而这一点有过一个真实的 bug：注释提到 `#16812`——「a TitleChunker → TokenChunker chain silently discarded the chapter-level chunks and re-chunked the raw parser output」。所以 `Token` 的分派里明确优先消费 `upstream.Chunks`（若上游已是 chunks）。

## 怎么实现的

### `TokenChunkerComponent`：参数是纯长度旋钮

```go
type tokenChunkerParam struct {
	schema.TokenChunkerParam
}
```

**它只是内嵌了 schema 里的参数结构** —— 所以「参数定义在一个地方，应用逻辑在另一个地方」。

参数里两个值值得看：

```go
// DelimiterMode selects the chunking strategy.
// Allowed value: "delimiter". The legacy "token_size" value is accepted for
// backward compatibility and normalized to "delimiter" (it is behaviorally
// identical at runtime; see Validate). The single-chunk "one" behavior is
// provided by the separate OneChunker component.
DelimiterMode string `json:"delimiter_mode"`
```

**三处**：

1. **合法的只有一个值**（`"delimiter"`），而 `"token_size"` 只为兼容保留并归一。所以这个「模式」字段实际上已经退化成一个常量。

2. **`"one"`（整篇一块）被拆成独立组件** —— 所以「模式字段」里不再包含它。**这是一次正确的拆分**：整篇一块不需要任何切分逻辑，与按分隔符切分毫无共同点。

3. **`OverlappedPercent` 的区间是 `[0, 90]`，且接受 `[0,1)` 的小数输入并归一**：

```go
// OverlappedPercent is the overlap percentage in [0, 90] — an integer-like float in the
// same range so DSL templates written for the pipeline work
// out of the box  A [0,1) fraction input is also
// accepted and normalized to this scale by tokenChunkerParam.Update
// (via normalizeOverlappedPercent).
OverlappedPercent float64 `json:"overlapped_percent"`
```

   **「同时接受百分比与小数」是一处兼容性妥协**：老 DSL 模板写 `0.2`（20%），新约定写 `20`，两者都得能用。归一化函数把 `[0,1)` 解释成小数、把 `>= 1` 解释成百分数。

   而 90 的上界是硬约束——重叠接近 100% 会让块几乎不前进。

**两个上下文窗口参数是「给媒体块附上周围文本」**：

```go
// TableContextSize is the number of surrounding tokens to attach
// to table chunks. 0 disables.
TableContextSize int `json:"table_context_size"`

// ImageContextSize is the number of surrounding tokens to attach
// to image chunks. 0 disables.
ImageContextSize int `json:"image_context_size"`
```

**这两个参数只对结构化 payload 有意义**，因为纯文本路径已经把媒体信息丢了。

**`Invoke` 的分派是一个五路 switch，而它的 `default` 分支才是最有信息量的。**

```go
switch upstream.OutputFormat {
case schema.PayloadFormatMarkdown:
	if upstream.MarkdownResult == nil { return emptyOutputs(), nil }
	return c.invokeTextPayload(ctx, *upstream.MarkdownResult, delimPattern, childrenPattern), nil
case schema.PayloadFormatText:
	...
case schema.PayloadFormatHTML:
	...
default:
	// Port of — when the upstream emitted
	// chunks (output_format == "chunks", e.g. a TitleChunker
	// feeding into this TokenChunker), consume those chunks rather
	// than the raw parser json_result. Otherwise fall back to the
	// structured json_result. This fixes #16812 where a
	// TitleChunker → TokenChunker chain silently discarded the
	// chapter-level chunks and re-chunked the raw parser output.
	var items []schema.ChunkDoc
	if upstream.OutputFormat == schema.PayloadFormatChunks {
		items = upstream.Chunks
	} else {
		items = upstream.JSONResult
	}
	...
```

五处：

1. **三种纯文本格式共用同一个处理函数**（`invokeTextPayload`）—— 因为对分块而言，Markdown/HTML/纯文本的区别只在于分隔符是否匹配，而它们都是「一整串字符串」。

2. **`nil` 结果返回 `emptyOutputs()` 而不是报错** —— 所以「上游没给这种格式的内容」是合法的（可能是格式字段与实际内容不一致）。

3. **`default` 分支同时处理 `json` 与 `chunks`** —— 而它内部再判一次 `OutputFormat == PayloadFormatChunks` 来决定数据源。**所以外面那个 switch 实际上只区分了「三种纯文本 vs 其余」**。

4. **`#16812` 这个 bug 的形态值得记**：TitleChunker → TokenChunker 的链里，TokenChunker 不看 `upstream.Chunks` 而直接读 `JSONResult`，于是**章节级分块被丢弃、原始解析结果被重新切**。用户看到的是「标题分块没生效」。

   **修法是在分派处显式优先 `Chunks`。** 这个 bug 之所以能存在，是因为「两个字段都可能存在，选哪个不是显然的」。

5. **PDF 引擎的按需重取**：

```go
engine, engErr := newPDFEngineFromUpstream(ctx, db, upstream)
if engErr != nil {
	common.Warn("TokenChunker: could not open PDF for on-demand cropping", zap.Error(engErr))
}
if engine != nil {
	defer engine.Close()
}
```

   **注释说明了用途**：「Re-acquire the source PDF (if the Parser forwarded storage refs) so image/table sections are cropped on demand rather than carried through the wire. Best-effort: a nil engine simply skips cropping.」

   这与 b02 里「不把二进制跨组件传递」是同一条设计的后半段：上游只传引用，这里按需重新打开。而**失败只警告不报错**，因为切图是增强而非必需。

**并发与合并顺序的契约写在文档里：**

```go
// Concurrency: text payloads are fanned across 4 goroutines by
// primary-delimiter segment; structured JSON/chunks payloads fan
// across items. Merge is by input index (plan §8 R8): the i-th
// goroutine's output occupies slot i, regardless of completion order.
//
// Timeout: honours ctx cancellation only — there is no inner @timeout
// decorator equivalent (plan §8 R1).
```

三处：

1. **扇出粒度不同**：纯文本按「主分隔符切出的段」，结构化按「条目」。所以两种路径的并行度语义不同。

2. **「Merge is by input index」** —— 这是**顺序保证**：`i` 号任务的输出占据 `i` 号槽位，与完成顺序无关。

   **这与 b01 里「结果必须按权威顺序收集」是同一条纪律的又一次出现**，而这里还给了它一个编号（「plan §8 R8」），说明它是设计文档里的条目而非临时决定。

3. **「honours ctx cancellation only」** —— 只有 ctx 取消，没有内部超时。**这是一处明确的取舍**：加了内部超时会让长文档在正常处理中被误杀，而取消是用户显式的。

### `TitleChunkerComponent`：三路标题来源

它的核心是「标题从哪来」，有三种来源：

```go
type outlineEntry struct {
	title string
	level int
}
```

```go
// outlineEntry is one PDF bookmark/heading from the parser-supplied
// outline, mirroring extract_pdf_outlines' (text, level, page)
// tuple. The page is unused by title detection.
```

**注释说明两件事**：它是 PDF 大纲（bookmark）的条目，对应 Python 版的 `(text, level, page)` 三元组，但**页码被丢弃**——因为标题检测只用「文本 + 层级」。

而另外两种来源是 `levels`（层级正则）与手工标题。`levels` 的结构是 `[][]string`：

```go
// parseLevels accepts a [[string]] representation — the natural
// JSON form — and rebroadcasts the strings verbatim. Stricter
// validation lives in schema.TitleChunkerParam.Validate.
func parseLevels(in []any) [][]string {
```

**「宽松解析 + 严格校验分离」是一处刻意的分工**：`parseLevels` 只负责把 JSON 的 `[]any` 转成 `[][]string`（跳过非法项），而合法性由 `Validate()` 判。所以「解析」不会因为一个坏元素而整体失败。

**而 `selectLevelGroup` 是一个「多组候选，用命中数选最优」的机制：**

```go
// selectLevelGroup returns the regex-list family (rawLevels) with the
// highest heading hit count across the input lines.
func selectLevelGroup(lines []string, rawLevels [][]string) []string {
	...
	for i, group := range rawLevels {
		for _, line := range lines {
			text := trim(line)
			if text == "" { continue }
			// A bullet-looking line cannot count as a heading hit for any
			// family, and a long body paragraph does not count either.
			if notHeadingCandidate(text) { continue }
			for _, pattern := range group {
				if re := compileLevelPattern(pattern); re != nil {
					if re.MatchString(text) { hits[i]++; break }
				}
			}
		}
	}
```

五处：

1. **候选是「多组正则」，每组代表一种标题风格**（如「第X章 / 第X节」vs「1. / 1.1」）。所以配置里可以同时给多种风格。

2. **胜出判据是「命中该风格的行数最多」** —— 所以是**自适应选择**：不需要用户知道自己的文档用哪种标题格式。

3. **`notHeadingCandidate` 这个前置过滤是关键**：注释说「A bullet-looking line cannot count as a heading hit for any family, and a long body paragraph does not count either」。

   **两道过滤分别对应两种误判**：项目符号行（`- xxx`）看起来像标题但通常是列表项；长正文段落含冒号/编号时可能匹配标题正则但明显不是标题。

   而 `headingTooLong`（在 `notHeadingCandidate` 里被调用）就是那道长度闸。

4. **`break` 在命中第一个 pattern 后** —— 所以一组里多个 pattern 都命中同一行只算一次。**这避免「正则多的那组获胜」这个偏差**。

5. **`bestIdx < 0` 返回 nil** —— 所以「所有组都零命中」时返回 nil，调用方必须处理「没选出标题风格」的情形（通常会退化成整篇一块）。

**`hierarchy` 与 `chunk_token_cap` 是两个正交的约束：**

```go
if v, ok := schema.NumericFromAny(conf["hierarchy"]); ok {
	n := int(v)
	p.TitleChunkerParam.Hierarchy = &n
}
```

**`*int` 而不是 `int`** —— 因为「未设置」与「设为 0」不同。未设置时走默认层级；设为 0 可能表示「不限制」。

**而 `table_context_size` / `image_context_size` 的解析比 `Token` 版本严格得多：**

```go
for _, field := range []struct {
	name   string
	target *int
}{
	{"table_context_size", &p.TableContextSize},
	{"image_context_size", &p.ImageContextSize},
} {
	raw, exists := conf[field.name]
	if !exists { continue }
	value, ok := schema.NumericFromAny(raw)
	if !ok || math.IsNaN(value) || math.IsInf(value, 0) || value < 0 ||
		math.Trunc(value) != value || value >= float64(^uint(0)>>1) {
		return fmt.Errorf("%s must be a non-negative integer", field.name)
	}
	*field.target = int(value)
}
```

**四个拒绝条件**：`NaN`、`Inf`、负数、非整数，以及「超过最大 int」（`float64(^uint(0)>>1)`）。**这是本组里最严的一处数值校验**，而 `Token` 的同一参数只做了 `int(value)`（无校验）。

**两处同一参数的校验强度不同**，大概来自演化顺序。而这处严格的理由是「溢出会让 `int(value)` 得到未定义结果（Go 的 float→int 转换在溢出时未定义，不是 clamp）」。

### `GeneralChunkerComponent`：按文件族选策略

```go
type generalStrategy uint8

const (
	generalStrategyText generalStrategy = iota
	generalStrategyPDF
	generalStrategyDOCX
	generalStrategyMarkdown
	generalStrategySpreadsheet
)

func generalStrategyForFileType(fileType string) generalStrategy {
	switch strings.ToLower(strings.TrimSpace(fileType)) {
	case "pdf":
		return generalStrategyPDF
	case "docx":
		return generalStrategyDOCX
	case "md":
		return generalStrategyMarkdown
	case "xls", "xlsx", "csv":
		return generalStrategySpreadsheet
	default:
		return generalStrategyText
	}
}
```

四处：

1. **五种策略，而 `default` 是纯文本** —— 所以「不认识的文件类型」用通用文本策略，而不是报错。

2. **`generalStrategyText` 是 `iota` 的零值** —— 所以「未初始化」与「text」等价。这是一处「零值即默认」的设计，省掉了初始化。

3. **`inferGeneralFileType` 先看 `upstream.Name` 再看 `upstream.File.Name`，然后取扩展名** —— 所以族判定在分块层又做了一次（b02 已经做过一次）。

   **两次判定的理由能推出来**：b02 的判定结果是 `file_type` 字段（会传到下游），但可能缺失（比如上游不是 Parser）。所以这里的 `inferGeneralFileType` 是兜底。

4. **`isKnownGeneralFileType` 列了 18 种** —— 而策略只有 5 种。所以「已知类型」与「有专门策略」是两件事：已知但不专门处理的走 text 策略。

**默认参数是「按换行切、512 token」：**

```go
func (generalChunkerParam) Defaults() generalChunkerParam {
	return generalChunkerParam{
		ChunkTokenSize:     512,
		Delimiters:         []string{"\n"},
		ChildrenDelimiters: []string{},
	}
}
```

**注意 `Defaults()` 是值接收者**（`func (generalChunkerParam) Defaults()`）—— 所以它不修改接收者，直接返回一个新值。而 `Token`/`Title` 的 `defaultsToken`/`defaultsTitle` 是自由函数。**三处同一概念三种写法**，但都无害。

**`normalizeGeneralDelimiterValue` 兼容两种形态：**

```go
// normalizeGeneralDelimiterValue accepts both the canonical list form and
// the legacy single-string form. A legacy string is split into Unicode
// rune delimiters, except backtick-wrapped tokens which remain one delimiter.
func normalizeGeneralDelimiterValue(value any) ([]string, bool) {
	switch value := value.(type) {
	case string:
		return splitGeneralDelimiterString(value), true
	case []string:
		return append([]string(nil), value...), true
	case []any:
```

四处：

1. **`string` 形态被按 rune 拆开** —— 所以 `"；。"` 变成 `["；", "。"]`。这是老 DSL 的语义（一个字符串里塞多个分隔符）。

2. **反引号包裹的 token 保持为一个分隔符** —— 所以 `` "`\\n`" `` 是正则而不是「` 和 `\` 和 `n`」三个字符。

   这解释了 `Token` 参数文档里那句：「Strings wrapped in backticks (e.g., `"`\\n`"`) denote user-defined regex split points」。

3. **返回 `(value, ok)` 而不是只返回值** —— 所以「不认识这个类型」与「解析出空列表」区分得开。**这是「零值有歧义时用 ok 布尔」的标准做法。**

4. **`append([]string(nil), value...)` 做拷贝** —— 避免与配置 map 共享底层数组。

**`Update` 里 `delimiters` 与 `delimiter` 两个键都接受：**

```go
if value, ok := conf["delimiters"]; ok {
	if delimiters, recognized := normalizeGeneralDelimiterValue(value); recognized {
		p.Delimiters = delimiters
	}
} else if value, ok := conf["delimiter"]; ok {
	if delimiters, recognized := normalizeGeneralDelimiterValue(value); recognized {
		p.Delimiters = delimiters
	}
}
```

**`delimiters`（复数）优先，`delimiter`（单数）是遗留别名。** 而 `else if` 保证两者都配时只有一个生效。

### `GroupTitleChunkerComponent`：双约束

它的核心是「先定章节，再在章节内合并到合适的 token 量」。

**第一步：选目标层级。**

```go
// resolveTargetLevel picks the n-th smallest heading level from the
// per-line `levels` vector (n = hierarchy).
func resolveTargetLevel(levels []int, hierarchy int) int {
	tiers := make([]int, 0, len(levels))
	seen := make(map[int]bool)
	for _, l := range levels {
		if l > 0 && l < bodyLevel && !seen[l] {
			seen[l] = true
			tiers = append(tiers, l)
		}
	}
	...
	// ascending — 手写插入排序
	if hierarchy < 1 { hierarchy = 1 }
	if hierarchy > len(tiers) { hierarchy = len(tiers) }
	return tiers[hierarchy-1]
}
```

四处：

1. **`l > 0 && l < bodyLevel`** —— `bodyLevel` 是「正文」的哨兵值。所以正文行（无标题）被排除在层级统计之外。

2. **去重 + 手写插入排序** —— 没有用 `sort.Ints`。这是一处可优化但不影响正确性的实现（层级数量很小）。

3. **`hierarchy` 被夹到 `[1, len(tiers)]`** —— 所以「层级设得比实际深」会被夹到最深一层，而不是返回 0（不分块）。

4. **返回 `tiers[hierarchy-1]`** —— 第 n 小的那个层级。所以 `hierarchy=1` 用最粗的标题切，`hierarchy=2` 用第二级。

**第二步：给每行分配 section id。**

```go
// _buildSectionIDs computes, for each input level, the section id
// (`sid`) under which that record falls. Each heading encounter
// increments sid by 1; body lines share the active sid.
func buildSectionIDs(levels []int, targetLevel int) []int {
	secIDs := make([]int, len(levels))
	sid := 0
	for i, lvl := range levels {
		if i > 0 && targetLevel > 0 && lvl <= targetLevel {
			sid++
		}
		secIDs[i] = sid
	}
	return secIDs
}
```

三处：

1. **`i > 0` 保证第 0 行不触发新章节** —— 所以「文档以标题开头」时该标题仍属于 sid 0。

2. **`lvl <= targetLevel`** —— 所以「比目标层级更粗的标题」也算新章节边界。这是对的：一个 `#` 标题在任何 `##` 层级切分下都是新章节。

3. **`targetLevel > 0` 的守卫** —— 当目标是 0（无法确定层级）时，所有行都是 sid 0，即**整篇一个章节**。

**第三步：在章节内按 token 合并，而合并条件是一个两段式判据。**

```go
shouldMerge := len(currentGroup) > 0 &&
	currentGroup[0].isText() &&
	(tkCnt < minGroupTokens || (tkCnt < maxGroupTokens && secID == lastSID))
```

四处：

1. **`tkCnt < minGroupTokens`（32）时无条件合并** —— 所以极小的块会被强制合并，**即使跨章节**。

   **这是「小块比跨章节更糟」的判断**：一个 5 token 的块几乎无法检索（没有语义），而把它并到邻章节只是轻微的结构污染。

2. **`tkCnt < maxGroupTokens`（1024）**且**同章节**时合并 —— 所以上限是 1024 token。

   **两个常量 `minGroupTokens = 32` 与 `maxGroupTokens = 1024` 定义了「可接受块大小」的区间。**

3. **`currentGroup[0].isText()`** —— 所以组里第一条必须是文本。这保证「媒体块不吸收文本」。

4. **非文本记录（表格/图片）单独成组**：

```go
if !rec.isText() {
	if len(currentGroup) > 0 {
		recordGroups = append(recordGroups, append([]lineRecord(nil), currentGroup...))
	}
	recordGroups = append(recordGroups, []lineRecord{rec})
	currentGroup = currentGroup[:0]
	tkCnt = 0
	lastSID = -2
	continue
}
```

   **`lastSID = -2` 是一个哨兵**：它的作用是「重置章节追踪」，让下一行无论 sid 是多少都无法通过 `secID == lastSID`（因为 sid 从 0 开始递增，不可能为 -2）。

   **所以媒体块之后的下一条文本一定开新组**。这是对的：媒体块已经独立成组，后面的文本不该试图并入它。

5. **`currentGroup[:0]` 而不是 `nil`** —— 复用底层数组，避免重新分配。而 `append([]lineRecord(nil), currentGroup...)` 在追加前**拷贝**，避免后续 `[:0]` 影响已追加的组。

   **这两个操作必须配对**：拷贝 + 截断才安全。若只截断不拷贝，所有已追加的组会共享同一底层数组并被后来的写入覆盖。

**`joinGroupText` 精确复刻了 Python 版的拼接：**

```go
// joinGroupText mirrors `"".join(record["text"] + "\n" for
// record in records)` — every record's text followed by a newline
// (including the last), matching the text join exactly
func joinGroupText(g []lineRecord) string {
	var sb strings.Builder
	for _, r := range g {
		sb.WriteString(r.text)
		sb.WriteByte('\n')
	}
	return sb.String()
}
```

**「including the last」是关键**：末尾也补换行。若漏掉，块文本会与 Python 版差一个字符，从而让「同一份文档在两版实现下产出不同的块 id」（因为块 id 是文本哈希）。**这是一处「为了逐字节等价而保留冗余换行」的细节。**

## 关键取舍

**源码事实：** `Token` 的 `DelimiterMode` 只有一个合法值 `"delimiter"`，`"token_size"` 仅为兼容而保留并归一；整篇一块（`"one"`）被拆成独立组件。

**源码事实：** `OverlappedPercent` 的区间是 `[0, 90]`，同时接受 `[0,1)` 的小数输入并归一到百分制（`normalizeOverlappedPercent`），以兼容老 DSL 模板。

**源码事实：** `Token.Invoke` 的 switch 只区分「三种纯文本格式」与「其余」；纯文本走 `invokeTextPayload`（共用），其余在 `default` 里再判 `PayloadFormatChunks` 以决定数据源。

**源码事实：** `#16812` 的真实 bug：`TitleChunker → TokenChunker` 链里 `Token` 不看 `upstream.Chunks` 而直接读 `JSONResult`，导致章节级分块被丢弃并重新切原始解析结果；修法是在分派处显式优先 `Chunks`。

**源码事实：** 纯文本路径只产出 `text`，结构化路径还带 `doc_type_kwd` / `img_id`——所以走纯文本路径会丢掉媒体信息。

**源码事实：** `Token` 会按需重新打开源 PDF（`newPDFEngineFromUpstream`）以便裁剪图片/表格区块；打开失败只警告不报错（切图是增强而非必需），成功则 `defer engine.Close()`。

**源码事实：** `Token` 的并发契约是「纯文本按主分隔符段扇出 4 个 goroutine、结构化按条目扇出；合并按输入下标（`plan §8 R8`），第 i 个输出占第 i 槽位，与完成顺序无关」；超时只认 ctx 取消，无内部超时（`plan §8 R1`）。

**源码事实：** `Title` 的标题来源有三种：PDF 大纲（`outlineEntry`，对应 Python 版 `(text, level, page)` 但页码被丢弃）、层级正则、手工标题。

**源码事实：** `parseLevels` 只做宽松转换（跳过非法元素），合法性交给 `schema.TitleChunkerParam.Validate`——解析与校验分离，所以一个坏元素不会让整体失败。

**源码事实：** `selectLevelGroup` 用「命中该正则组的行数最多」自适应选出标题风格，候选是多组正则；命中前过 `notHeadingCandidate`（排除项目符号行与过长段落）且每行只计一次（命中一个 pattern 后 `break`，避免「正则多的组获胜」偏差）；零命中返回 nil。

**源码事实：** `Title` 的 `table_context_size` / `image_context_size` 校验比 `Token` 严格：拒绝 NaN / Inf / 负数 / 非整数 / 超出 int 范围（因为 Go 的 float→int 溢出是未定义行为，不是 clamp）。

**源码事实：** `hierarchy` 用 `*int`（区分「未设置」与「设为 0」）。

**源码事实：** `General` 有五策略，`generalStrategyText` 是 `iota` 零值（未初始化即纯文本）；`generalStrategyForFileType` 的 `default` 也是 text；`isKnownGeneralFileType` 列 18 种已知类型而策略只 5 种。

**源码事实：** `General` 在分块层**再判一次**文件族（`inferGeneralFileType` 从 `Name` 或 `File.Name` 取扩展名），是对上游 `file_type` 缺失的兜底。

**源码事实：** `General` 默认「按换行切、512 token」；`Defaults()` 是值接收者（不改接收者）。

**源码事实：** `normalizeGeneralDelimiterValue` 接受 list 与 legacy string 两形态，string 按 rune 拆成多分隔符，反引号包裹的 token 保持为一个（正则）；返回 `(value, ok)` 让「类型不认识」与「解析出空」区分开；list 形态做拷贝。

**源码事实：** `General.Update` 里 `delimiters` 优先、`delimiter` 是遗留别名，`else if` 保证只一个生效。

**源码事实：** `GroupTitle` 用两段式判据：`tkCnt < minGroupTokens`（32）时**无条件合并（即使跨章节）**，否则要求 `tkCnt < maxGroupTokens`（1024）**且同章节**。

**源码事实：** `GroupTitle` 的媒体块（非文本）独立成组，并把 `lastSID` 置为 `-2` 哨兵，使下一条文本必定开新组；`currentGroup[:0]` 复用数组与追加前 `append(nil, ...)` 拷贝必须配对，否则已追加的组会被后续写入覆盖。

**源码事实：** `resolveTargetLevel` 用 `l > 0 && l < bodyLevel` 排除正文行，去重后手写插入排序，`hierarchy` 被夹到 `[1, len(tiers)]`（超出会被夹到最深一层而不是返回「不分块」）。

**源码事实：** `buildSectionIDs` 用 `i > 0` 保证首行不触发新章节，`lvl <= targetLevel` 让更粗的标题也算边界，`targetLevel == 0` 时整篇一个章节。

**源码事实：** `joinGroupText` **每条记录后都补换行（含最后一条）**，注释称之为「matching the text join exactly」——逐字节等价是必要的，因为块 id 是文本哈希。

**架构推论：** 四个组件的分工本质是「边界判据的三种来源 + 一种组合」：纯预算（Token）、纯结构（Title）、按格式惯例（General）、结构与预算的联合（GroupTitle）。这个分类值得记，因为它揭示了一件事——**没有任何单一判据能同时满足「块够大」与「块语义完整」**：纯预算会切断章节，纯结构会产出 5 token 的碎块，联合约束才是可用的形态。GroupTitle 的存在本身就是对这个矛盾的承认。

**架构推论：** `#16812` 那个 bug 的形态很有代表性：「两个字段都可能存在，选哪个不是显然的」。这类 bug 的修法总是「在分派点显式声明优先级」，而它的预防只能靠**把「谁优先」写进注释**。这份实现对上游格式的分派写得很啰嗦，正是这类 bug 之后的结果。

**架构推论：** 「逐字节等价」这条要求（`joinGroupText` 的末尾换行）说明块 id 是**文本的直接函数**。这带来一个约束：任何影响块文本的细节（多一个换行、少一个空格）都会改变块 id，从而让「增量更新」把整篇文档当成全新的。**判据是「块 id 的输入是什么」**——若包含文本，则实现的微小差异会破坏跨版本一致性。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/ingestion/component/chunker/token.go` | `tokenChunkerParam` | 长度旋钮（分隔符模式 / 预算 / 重叠 / 媒体上下文） |
| `internal/ingestion/component/chunker/token.go` | `TokenChunkerComponent` | 按预算切分 |
| `internal/ingestion/component/chunker/token.go` | `NewTokenChunker` | 分隔符校验 + 参数更新 + 构造期校验 |
| `internal/ingestion/component/chunker/token.go` | `TokenChunkerComponent.Invoke` / `invoke` | 五路分派（纯文本三路 + 结构化） |
| `internal/ingestion/component/chunker/token.go` | `decodeChunkerFromUpstream` | 上游解码（`[]parser.DocItem` 快路径） |
| `internal/ingestion/component/chunker/token.go` | `stripChunkerRuntimeTimestamps` | 剥离运行期时间戳字段 |
| `internal/ingestion/component/chunker/token.go` | `validateUpstreamMediaRefs` | 校验媒体引用 |
| `internal/ingestion/component/chunker/title.go` | `titleChunkerParam` / `Update` | 标题来源与媒体窗口（严格数值校验） |
| `internal/ingestion/component/chunker/title.go` | `outlineEntry` | PDF 大纲条目（`title` + `level`，页码丢弃） |
| `internal/ingestion/component/chunker/title.go` | `parseLevels` | 宽松转换（校验交给 schema） |
| `internal/ingestion/component/chunker/title.go` | `selectLevelGroup` | 多组正则按命中数自适应选优 |
| `internal/ingestion/component/chunker/title.go` | `notHeadingCandidate` / `headingTooLong` / `notBullet` | 标题候选过滤 |
| `internal/ingestion/component/chunker/title.go` | `outlineSimilarity` | 大纲标题与行文本的 bigram Jaccard |
| `internal/ingestion/component/chunker/title.go` | `matchLayoutLevel` / `matchRegexLevel` | 两种层级判定路径 |
| `internal/ingestion/component/chunker/general.go` | `generalChunkerParam` / `Defaults` | 默认「换行切、512 token」 |
| `internal/ingestion/component/chunker/general.go` | `normalizeGeneralDelimiterValue` / `splitGeneralDelimiterString` | 兼容 list 与 legacy string |
| `internal/ingestion/component/chunker/general.go` | `GeneralChunkerComponent` | 按文件族选策略 |
| `internal/ingestion/component/chunker/general.go` | `generalStrategy` / `generalStrategyForFileType` | 五策略分派（零值即 text） |
| `internal/ingestion/component/chunker/general.go` | `inferGeneralFileType` / `isKnownGeneralFileType` | 兜底族判定与已知类型清单 |
| `internal/ingestion/component/chunker/general.go` | `chunkPDF` | PDF 专用策略 |
| `internal/ingestion/component/chunker/group.go` | `GroupTitleChunkerComponent` | 章节内按 token 合并 |
| `internal/ingestion/component/chunker/group.go` | `resolveTargetLevel` | 选第 n 小层级作为章节边界 |
| `internal/ingestion/component/chunker/group.go` | `buildSectionIDs` | 每行的章节 id |
| `internal/ingestion/component/chunker/group.go` | `invokeGroup` | 章节分组的入口 |
| `internal/ingestion/component/chunker/group.go` | `groupRecords` | 两段式合并判据（32 / 1024） |
| `internal/ingestion/component/chunker/group.go` | `joinGroupText` | 逐字节等价拼接（含末尾换行） |
| `internal/ingestion/component/chunker/group.go` | `isPlainTextFormat` | 纯文本 payload 判定 |
| `internal/ingestion/component/chunker/group.go` | `buildChunksFromRecordGroups` | 组装块（含 `root_chunk_as_heading` 与媒体身份） |
| `internal/ingestion/component/schema/chunker.go` | `TokenChunkerParam` / `TitleChunkerParam` | 参数结构与校验 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 文档：分块方法](https://ragflow.io/docs/dev/configure_knowledge_base)
- [论文：检索单元粒度对 RAG 的影响](https://arxiv.org/abs/2402.19473)
- [论文：基于标题层级的文档切分](https://aclanthology.org/2021.findings-acl.213/)

## 与主核 / AI 的关系

四种分块与主核（多模态数据库内核）的**行存储与列存储的选择**有相似结构：都在「同一份数据、不同的物理切分方式」，而选择依据是「后续访问模式」。分块的选择依据是「后续检索模式」：纯向量检索不在乎块边界（它按相似度取），而关键词/图检索在乎（它按字段匹配）。

**但有一个关键差别：主核可以在同一份数据上同时建行存与列存两种副本；分块只能选一种**——因为块是检索的基本单位，同一份文本被切成两套块会产生两套互不相干的索引。**所以分块的选择是单向的，这解释了为什么有四种组件而不是「一个组件支持四种模式」**。

第二处对照是 `joinGroupText` 的「逐字节等价」与主核的**校验和**。两者都是「对内容的精确表示」，任何差异都改变后续的所有派生标识。主核用 CRC 检测意外差异；这里用「与 Python 版逐字节一致」来保证**跨实现**的块 id 一致。后者的难度更大——它要求两份独立实现产生同一串字节。

第三处是 `groupRecords` 的 `lastSID = -2` 哨兵与主核里各种哨兵值（`InvalidTransactionId`、`-1` 父指针）。**哨兵的作用都是「一个不可能的正常值」**，让后续判断必然失败。差别在于这里的 `-2` 是「章节 id 从 0 递增」这个局部不变量提供的，而主核的哨兵由类型范围提供。

对 RAG 工程的实际意义在于：分块是 RAG 里被讨论得最多但最容易被简化的一步。这份实现给出的三条经验可直接用：**边界判据有三种来源，没有一种是充分的**（预算/结构/惯例，必须按文件类型选）、**小块比跨章节更糟**（所以 GroupTitle 允许 `tk_min` 优先于章节约束）、以及**块 id 若由文本哈希得出，任何拼接细节都会破坏增量更新**（末尾换行不能省）。
