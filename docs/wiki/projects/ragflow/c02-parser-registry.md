---
project: ragflow
feature: parser-registry
title: 解析器注册与按类型分派
page_type: feature
schema_version: 1
refs:
  - path: internal/parser/parser/parser_type.go
    symbols: [TryParser, GetParser, RegisterParserFactory, HasParseMethod]
  - path: internal/parser/parser/text_parser.go
    symbols: [TextParser, NewTextParser]
  - path: internal/parser/parser/doc_parser.go
    symbols: [DOCParser]
---

# 解析器注册与按类型分派

## 一句话职责

把「文件类型 + 内容 + 配置的方法名」解析成一个具体的 `ParseResultProducer`：先查注册表（社区内置之外由版本注入）、再走内置 switch、并在构造后**用内容自检重新判定格式**——若自检结论与请求不同，换解析器并返回实际类型。

## 机制是什么

这个注册表要解决的不只是「按扩展名分派」，而是三层叠加的分派：

| 层级 | 判据 | 谁决定 |
|---|---|---|
| 1. 方法覆盖 | `parse_method` 配置 | 用户模板 |
| 2. 版本注入 | `FileType` 键 | 企业版 `init()` |
| 3. 内置 switch | `FileType` | 社区代码 |
| 4. 内容自检 | `TryParse(data)` | 解析器自己 |

**顺序是“配置 > 注入 > 内置”，而第 4 层在拿到解析器之后才运行**——这是本页最不显然的一处：**先按你声明的类型造一个解析器，再问它「你觉得这字节是什么类型」**。

而它能这样设计的原因在 `TryParser` 的注释里：

```go
// TryParser optionally identifies a format before full parsing by inspecting
// recognition structures. FileTypeOTHER means unknown: keep the requested type.
type TryParser interface {
	TryParse(data []byte) utility.FileType
}
```

**「未知就保持请求的类型」** —— 所以自检的失败方向是「不改变」，而不是「报错」或「猜一个」。

**第二个机制是 `textOnly` 这个布尔，它一路传到底。**

```go
func GetParser(fileType utility.FileType, data []byte) (ParseResultProducer, utility.FileType, error) {
	return selectParser(fileType, data, false)
}
func GetTextOnlyParser(fileType utility.FileType, data []byte) (ParseResultProducer, utility.FileType, error) {
	return selectParser(fileType, data, true)
}
```

而 `GetTextOnlyParser` 的注释说明了它的三个调用场景与一个后果：

> GetTextOnlyParser selects a parser and effective file type for readable-text extraction: previews, attachments, agent uploads and CLI parsing. **Embedded media is skipped even when extraction is nested inside an ingestion run (email attachments)**. These paths also work without an injected run media cache.

**「嵌套在摄取流程内的提取（邮件附件）也跳过内嵌媒体」** —— 这是关键的语义：`textOnly` **不是由调用场景决定的，而是由调用方显式选择的**，所以「邮件附件」这个场景即使在摄取内部也走 textOnly。

## 怎么实现的

### `selectParser`：先造、再自检、必要时重造

```go
func selectParser(fileType utility.FileType, data []byte, textOnly bool) (ParseResultProducer, utility.FileType, error) {
	p, err := newParser(fileType, textOnly)
	if err != nil {
		return nil, fileType, err
	}
	if probe, ok := p.(TryParser); ok {
		if actual := probe.TryParse(data); actual != "" && actual != utility.FileTypeOTHER && actual != fileType {
			p, err = newParser(actual, textOnly)
			return p, actual, err
		}
	}
	return p, fileType, nil
}
```

五处：

1. **先构造再自检，不是先自检再构造** —— 因为 `TryParse` 是**解析器的方法**而不是包的函数。所以「判定格式」的能力属于具体解析器，必须造出来才能问。

   **代价是一次可能的浪费构造**（若自检结论不同，第一个实例被丢弃）。收益是「识别逻辑与解析逻辑同源」——哪个解析器最清楚这种格式的特征？它自己。

2. **四个条件做“是否换”的判断** —— `actual != "" && actual != FileTypeOTHER && actual != fileType`。所以空串、`OTHER`、与请求相同 三种都不触发重造。

3. **`return p, actual, err` 三个值一起返回** —— 所以**调用方能知道「请求的类型」与「实际用的类型」不同**。这不是错误，是事实上报（b02 的 `Invoke` 就据此打警告）。

4. **重造失败时 `p` 可能是 nil 且 `err` 非 nil** —— 此时返回 `(nil, actual, err)`。**注意返回的 `fileType` 是 `actual` 而不是原请求的** —— 这有点微妙：报告「实际类型」即使构造失败。

5. **`textOnly` 在两次构造中都传** —— 所以自检切换不会丢失只读语义。

### `newParser`：注册表优先，内置 switch 兜底

```go
func newParser(fileType utility.FileType, textOnly bool) (ParseResultProducer, error) {
	if factory, ok := parserFactories[fileType]; ok {
		return factory(textOnly)
	}
	switch fileType {
	case utility.FileTypePPTX:
		p := NewPPTXParser()
		p.textOnly = textOnly
		return p, nil
	case utility.FileTypePPT:
		p := NewPPTParser()
		p.pptx.textOnly = textOnly
		return p, nil
	...
```

六处：

1. **注册表先查** —— 注释说明了意图：「Lookups consult this map before the built-in switch so an edition can add parsers for its own file types without the community switch changing」。

   **「企业版加解析器不改社区 switch」** —— 这是「注册表 vs 集中式 switch」的经典权衡：注册表避免了代码冲突，但**让「有哪些解析器」变得不可静态枚举**。

2. **`PPT` 的写法不同** —— `p.pptx.textOnly = textOnly`（穿过一层内嵌）。所以 `PPTParser` 里包了一个 `PPTXParser`。

   **这暴露了一个事实**：PPT 是「旧二进制格式，用转换后走 PPTX 路径」的实现，而 `textOnly` 必须传到内层才生效。

3. **不同解析器接收 `textOnly` 的方式不同** —— 大部分是 `p.textOnly = textOnly`，而 `XLS`/`CSV`/`PDF`/`TXT`/`JSON`/`EMAIL`/`VISUAL`/`AURAL` **完全不接收**。

   **所以 `textOnly` 对这几个族无效** —— 不是因为忽略了，而是它们的实现里没有「抽取媒体」这一步（PDF 的媒体抽取可能在别的层）。

4. **`Markdown` 有一处特殊处理**：

```go
case utility.FileTypeMarkdown:
	p, err := NewMarkdownParser(GoMarkdown)
	if err != nil {
		return nil, err
	}
	p.textOnly = textOnly
	p.mediaRefs = !textOnly
	return p, nil
```

   **`mediaRefs = !textOnly`** —— 所以「非 textOnly 时才产出媒体引用」。这解释了为什么 markdown 里内嵌的图片在 textOnly 模式下不出现：**它不是被过滤掉的，而是根本没被采集**。

5. **`XLSX` 可能是唯一会构造失败的族** —— `NewXLSXParser("")` 返回 `(p, err)`。但**只在这里，`XLS` 走 `NewXLSParser("")` 也返回两值**。

   而 `PPT` 族用 `NewPPTParser()`（一值）。

   三处不一致的构造签名，可能是因为底层库的初始化路径不同。

6. **`default` 返回错误** —— 「unsupported file type: %s」。所以「未知类型」在 `newParser` 层就失败了，而 `FileTypeOTHER` 的处理**不在这里**（b02 的 `dispatchParse` 在 `FileTypeOTHER` 时直接返回零值，不调 `GetParser`）。

   **这解释了 b02 注释里那句「the dispatch then takes text-page mode without consulting `parser.GetParser`」** —— 因为 `newParser(FileTypeOTHER)` 会报错。

### 方法覆盖：`GetParserWithMethod` 与 `HasParseMethod`

```go
func GetParserWithMethod(ft utility.FileType, method string, data []byte, textOnly bool) (ParseResultProducer, utility.FileType, error) {
	method = strings.ToLower(strings.TrimSpace(method))
	if factory, ok := parseMethodFactories[ft][method]; ok {
		p, err := factory(textOnly)
		if err != nil {
			return nil, ft, err
		}
		return p, ft, nil
	}
	return selectParser(ft, data, textOnly)
}
```

三处：

1. **方法名归一化（小写 + TrimSpace）** —— 而注释说「Methods are matched case-insensitively」。所以 `"MinerU"` 与 `"mineru"` 等价。

2. **命中方法就**不**做内容自检** —— 直接返回 `(p, ft, nil)`。**这是有意的**：用户显式指定了后端，就不该被自检结论推翻。所以「配置的方法」优先级高于「内容自检」。

3. **未命中则回退到 `selectParser`** —— 也就是说：**配了一个没注册的方法名，会静默走默认路径**。

   **这是一处需要知道的行为**：写了 `parse_method: "mineru"` 但该版本没注册 MinerU，不会报错，只会用默认解析器。而 b02 的 `dispatchParse` 里那段 `!parser.HasParseMethod(fileType, method)` 正是为了在这种情形下（把「未注册的方法名」当远程后端）做 IO 追踪——**所以「未注册」这个状态是被显式检查的**，只是不作为错误。

```go
func HasParseMethod(ft utility.FileType, method string) bool {
	method = strings.ToLower(strings.TrimSpace(method))
	if method == "" {
		return false
	}
	_, ok := parseMethodFactories[ft][method]
	return ok
}
```

**两处**：

1. **空方法名返回 false** —— 而 `parseMethodFactories[ft][""]` 本来也查不到（因为注册时 `method == ""` 被拒绝）。所以这个显式判断是冗余的但正确。
2. **`parseMethodFactories[ft]` 若 `ft` 不存在返回 nil map** —— 而 nil map 的下标读是安全的（Go 允许）。所以不需要预判。

**注册函数对参数的处理是一致的：**

```go
func RegisterParseMethodFactory(ft utility.FileType, method string, factory func(textOnly bool) (ParseResultProducer, error)) {
	method = strings.ToLower(strings.TrimSpace(method))
	if ft == "" || method == "" || factory == nil {
		return
	}
	byMethod := parseMethodFactories[ft]
	if byMethod == nil {
		byMethod = map[string]func(textOnly bool) (ParseResultProducer, error){}
		parseMethodFactories[ft] = byMethod
	}
	byMethod[method] = factory
}
```

三处：

1. **静默拒绝非法参数（不报错、不 panic）** —— 因为注册发生在 `init()` 里，报错只能 panic。
2. **`byMethod` 懒创建** —— 所以「只注册一个方法」不会为所有类型建空 map。
3. **重复注册会覆盖** —— 后注册的赢。所以企业版能替换内置处理器。

**而 `parserFactories` 的注册更简单（没有 method 维度）：**

```go
func RegisterParserFactory(ft utility.FileType, factory func(textOnly bool) (ParseResultProducer, error)) {
	if ft == "" {
		return
	}
	parserFactories[ft] = factory
}
```

**而它的注释说明了用途与风险**：「Enterprise editions call this from `init()` to inject parsers for their extra file types」。

**风险是**：`RegisterParserFactory(FileTypePDF, ...)` 会**替换内置 PDF 解析器**，因为注册表先查。所以这个接口既能「加」也能「换」—— 而名字叫 Register 不叫 Override。**这是一处命名与能力不完全匹配的地方。**

### 两个并行的注册表，没有顺序依赖

```go
var parserFactories = map[utility.FileType]func(textOnly bool) (ParseResultProducer, error){}
var parseMethodFactories = map[utility.FileType]map[string]func(textOnly bool) (ParseResultProducer, error){}
```

**五个事实值得记：**

1. **两个 map 都是包级变量，无锁** —— 所以「注册必须在 `init()` 里做」是**硬约束**而非建议。注释两处都写了「Register factories during package initialization」。**若在运行期注册，会有数据竞争**（Go 的 map 并发读写在 race detector 下必然报错）。

2. **`parserFactories` 只有一层，`parseMethodFactories` 是两层** —— 所以「方法覆盖」是「类型 + 方法名」的二维键。

3. **两者都被 `newParser` 或 `GetParserWithMethod` 查询，但优先级不同**：`GetParserWithMethod` 先查方法表、查不到才进 `selectParser`→`newParser`（后者才查类型表）。**所以「类型注册」与「方法注册」是两个独立机制，方法表在前。**

4. **`parserFactories` 的注释在变量上，`parseMethodFactories` 没有** —— 一处小不一致。

5. **没有「列举所有已注册类型」的函数** —— 所以外部无法枚举能力。**这是注册表方案的一个已知代价**：能力集不在静态代码里。

### `TextParser`：最简但注释最长的解析器

```go
// TextParser (port-rag-flow-pipeline-to-go.md Phase 2.5 Slice 1).
//
// The python rag/flow/parser/parser.py:_code path (L1066) routes
// .txt / .py / .js / .java / .c / .cpp / .h / .php / .go / .ts / .sh
// / .cs / .kt / .sql files through deepdoc.parser.TxtParser. The Go
// side needs a parser for these families so `text&code` resolves to a
// real ParseResultProducer.
//
// TextParser decodes and normalizes text&code input, then emits one parser
// unit. Chunk boundaries belong to GeneralChunker, not to this parser.
type TextParser struct{}
```

**四处**：

1. **注释列了 13 个扩展名** —— 说明「代码文件也走文本路径」是一个明确的决定，而不是遗漏。

2. **「so `text&code` resolves to a real ParseResultProducer」** —— 说明这个类的存在是为了**满足接口契约**：`text&code` 族需要一个真实的解析器，否则注册表里就没有它。

3. **「Chunk boundaries belong to GeneralChunker, not to this parser」** —— 归责声明。**这解释了为什么它不做任何切分**：切分是下游的事。

4. **`TextParser struct{}` 是空结构体** —— 零大小。所以 `NewTextParser()` 不分配内存（Go 的空结构体地址可共享）。

```go
func (p *TextParser) ParseWithResult(ctx context.Context, filename string, data []byte) ParseResult {
	decoded, encName := charset.DecodeToUTF8(data, "text/plain")
	return ParseResult{
		File: map[string]any{
			"name":     filename,
			"size":     len(data),
			"encoding": encName,
		},
		JSON: []DocItem{NewTextJSONItem(normalizeTextNewlines(string(decoded)))},
	}
}
```

**四处**：

1. **`charset.DecodeToUTF8(data, "text/plain")` 返回 `(bytes, 编码名)`** —— 所以**编码探测是必需的**，因为 `.txt` / `.py` 等在无 BOM 时无法从字节判断编码。而 `encName` 被写进 `File["encoding"]`，让下游知道「原文件是什么编码」。

2. **`len(data)` 是原始字节大小，不是解码后的大小** —— 所以是「文件大小」而不是「正文字符数」。

3. **单条目输出**：`[]DocItem{NewTextJSONItem(...)}` —— 整个文件一个文本条目。所以「一个 txt 文件 = 一个解析单元」。

4. **`normalizeTextNewlines` 折 CRLF 与孤立 CR 为 LF** —— 注释：「while preserving the rest of the document content for GeneralChunker」。**所以归一只做换行，不做其他清洗**——因为下游要按 `\n` 切。

   而 `if s == "" { return s }` 的早退是必需的：`strings.ReplaceAll` 对空串本来也无害，但早退省两次分配。

**这里有一个值得注意的取舍**：`TextParser` 不返回 `Err`，即使是空文件。所以「空 txt」会产出「一个空文本条目」，而 `Produced()` 判为成功（`JSON != nil`）。

**这与 b05 的 `validateTokenizerOutputs` 是对应的**：一个空文本条目会产出空 `content_ltks`，而 `requiresFullTextTokens` 判「无摘要无正文 → 不需要词元」，所以校验能过。**整条链路对空文档是一致的宽容。**

### `DOCParser`：本页最复杂的一个，因为它在两种表示之间选

`DOCParser` 是旧二进制 `.doc` 的解析器，它展示了「同一个文档有多种文本视图时如何选」这个问题的处理。

```go
func (p *DOCParser) ParseWithResult(ctx context.Context, filename string, data []byte) ParseResult {
	if err := checkWPSLicense(ctx, filename); err != nil {
		return ParseResult{Err: err}
	}
	irJSON, mdText, plainText, _, err := docExtract(data, "doc")
	if err != nil {
		if errors.Is(err, deepdocoffice.ErrOfficeCGORequired) {
			return ParseResult{Err: fmt.Errorf("%w: %s", ErrOfficeCGORequired, filename)}
		}
		return ParseResult{Err: fmt.Errorf("doc extract: %w", err)}
	}
```

三处：

1. **`checkWPSLicense` 在最前** —— 所以文档格式的解析有**许可前置条件**（WPS 依赖）。而它返回 `ParseResult{Err: ...}` 而不是别的错误类型。

2. **`docExtract` 是变量而不是函数调用**：

```go
// docExtract is the native office_oxide extraction seam for the DOC family.
var docExtract = deepdocoffice.OpenAndExtract
```

   **「seam」这个措辞说明它是测试替换点**（与 b03 的 `extractorChatInvoker` 同一手法）。

3. **错误分两类**：`ErrOfficeCGORequired` 被包装成自己的哨兵（`ErrOfficeCGORequired`），其余包装成 `"doc extract: %w"`。

   **为什么要区分？** 因为「缺少 CGO 编译」是一个**部署问题**，而「抽取失败」是**文档问题**。前者需要运维动作（重新编译），后者可能只是文件损坏。而 `errors.Is` 用的是 sentinel 比对，所以下层必须用 `%w` 包装才能被识别。

**然后是「结构化 IR 优先、扁平文本兜底」的核心判断：**

```go
if strings.TrimSpace(irJSON) != "" {
	var sink *officeMediaSink
	if !p.textOnly {
		sink = newOfficeMediaSink(ctx, officeMediaFormatDOCX)
	}
	sections := buildDOCXJSONSectionsCore(irJSON, sink, !p.RemoveHeaderFooter)
	if sink != nil && sink.err != nil {
		return ParseResult{Err: sink.err}
	}
	if len(sections) > 0 || irChanged || (p.RemoveHeaderFooter && len(buildDOCXJSONSectionsCore(irJSON, nil, true)) > 0) {
		...
		return ParseResult{
			File:     map[string]any{"name": filename, "format": "doc"},
			JSON:     MapsToItems(sections),
			Warnings: sink.warnings(),
		}
	}
}
```

**五处**：

1. **注释说明了为什么偏好 IR**：

> Prefer the structured IR, which office_oxide recovers table (and list) structure for. Emitting structured sections lets the chunker render tables as HTML (#20918) instead of losing them to plain text — **the legacy path flattened the whole document (tables included) into a single text item**. Fall back to the flattened text only when the IR yields no usable sections, so a document whose IR is empty/unusable still keeps its content (PlainText / ToMarkdown) rather than silently producing 0 chunks.

   **「旧路径把整个文档（含表格）压成一个文本条目」** —— 所以 IR 路径的价值就是**保住表格结构**。而 `#20918` 是一个具体 issue 编号。

2. **`textOnly` 只影响 sink 的创建** —— 所以「不产出媒体」时仍走 IR 路径，只是没有媒体落盘。

3. **`buildDOCXJSONSectionsCore(irJSON, sink, !p.RemoveHeaderFooter)`** —— 第三个参数是「是否保留头尾」，即 `remove_header_footer` 取反。

4. **接受 IR 路径的条件是三选一**：`len(sections) > 0` **或** `irChanged` **或** 「移除头尾后仍有节」。

   **第三条最微妙**：`p.RemoveHeaderFooter && len(buildDOCXJSONSectionsCore(irJSON, nil, true)) > 0` —— 即「开着头尾移除，但按不removing的方式还能产出节」。注释解释：

> If RemoveHeaderFooter dropped the only sections in the IR, keep the structured path rather than falling back to flattened plain text.

   **「头尾移除把唯一的一节也删掉了，此时仍应保留结构化路径（返回一个空文本条目），而不是退回扁平文本」** —— 因为退回扁平文本会把用户明确要求删掉的头尾**重新带回来**。

   **这是一处很深的判断**：用户的意图（删头尾）比「有内容」更重要。

5. **`len(sections) == 0` 时造一个空文本条目**：

```go
if len(sections) == 0 {
	sections = []map[string]any{{"text": "", "doc_type_kwd": "text"}}
}
```

   所以「结构化路径产出 0 节」不是一个失败，而是一个**合法的空文档**。而这里用的是**手写 map 字面量**而不是 `NewTextJSONItem` —— 然后统一走 `MapsToItems(sections)`。**这正好是 `RawItem` 存在的场景**（迁移期 map 字面量）。

**兜底路径：三个文本视图选最长，但 IR 有结构时优先。**

```go
func selectDocTextView(irJSON, irText, mdText, plainText string) string {
	if strings.TrimSpace(irText) != "" && irHasStructuredContent(irJSON) {
		return irText
	}
	best := irText
	if len(strings.TrimSpace(mdText)) > len(strings.TrimSpace(best)) {
		best = mdText
	}
	if len(strings.TrimSpace(plainText)) > len(strings.TrimSpace(best)) {
		best = plainText
	}
	return best
}
```

三处：

1. **「IR 有表格或列表结构就无条件用它」** —— 优先级高于长度。而注释给了理由：

> The IR is preferred when it carries table/list structure (that content is unique to the IR view and would be **silently dropped under a pure length comparison** — a prose-heavy document can make PlainText longer than the IR even though the IR is more complete).

   **「纯按长度比较会静默丢掉只存在于 IR 的表格内容」** —— 因为扁平化后的 IR 可能比 PlainText 短。

2. **长度比较用的是 `len(strings.TrimSpace(...))`** —— 所以用**去空白后的字节长度**，而不是原始长度。所以「一坨空格」不会让某个视图获胜。

3. **`irHasStructuredContent` 的实现是字符串包含**：

```go
func irHasStructuredContent(irJSON string) bool {
	return strings.Contains(irJSON, `"type":"table"`) ||
		strings.Contains(irJSON, `"type":"list"`)
}
```

   **用 `strings.Contains` 而不是解析 JSON** —— 因为只关心「有没有」而不关心结构。注释说明了这个键的来历：「Detection keys on the canonical `"type":"table"` / `"type":"list"` field emitted by office_oxide」。

   **风险是**「文本内容里恰好出现 `"type":"table"`」会误判 —— 但它是 JSON 字符串，这个序列出现在内容里需要转义，所以实际风险很低。

**最后是 `extractDocText` 的「双重空判」：**

```go
if strings.TrimSpace(text) == "" && strings.TrimSpace(mdText) == "" {
	return "", "", fmt.Errorf("doc extract: no text content")
}
```

**注释解释了为什么这样判**：

> Fail loud when the document yielded no usable text in any view. The `OpenAndExtract` contract **swallows per-view errors**, so an empty selected view plus an empty Markdown view is the only signal we get that the document has no extractable text — surfacing it avoids silently producing 0 chunks (Finding A). A legitimately empty payload (e.g. a scanned-image doc with OCR off) is rare and **failing is safer than silent data loss**.

**四处**：

1. **下层「吞掉每个视图的错误」** —— 所以这一层只能靠「两个视图都空」来推断失败。**这是「下层丢失错误信息」导致上层只能启发式判断的代价。**

2. **两个视图（选中的 + markdown）都空才报错** —— 用交集而不是单一看法，因为「选中视图空但 markdown 不空」说明还有内容。

3. **`Finding A` 是一个内部编号** —— 说明这是评审发现的缺陷。

4. **「合法空载荷（如扫描件关 OCR）罕见，报错比静默丢数据安全」** —— 这是一处明确的判断：**把「可能是合法的空」当成错误，好过静默产出 0 块。**

### `RemoveTOC` 路径：把文本重新切成行再过滤

```go
if p.RemoveTOC {
	lines := strings.Split(text, "\n")
	items := make([]map[string]any, 0, len(lines))
	for _, line := range lines {
		items = append(items, map[string]any{"text": line, "doc_type_kwd": "text"})
	}
	items = removeContentsTable(items, isEnglishItems(items))
	if len(items) == 0 {
		return ParseResult{
			File: map[string]any{"name": filename, "format": "doc"},
			JSON: []DocItem{NewTextJSONItem("")},
		}
	}
	var sb strings.Builder
	for i, it := range items {
		if i > 0 { sb.WriteString("\n") }
		if s, ok := it["text"].(string); ok { sb.WriteString(s) }
	}
	text = sb.String()
}
```

**四处**：

1. **`removeContentsTable` 接受的是「条目列表」而不是文本** —— 所以必须先把文本拆成行、造条目、过滤、再拼回文本。**这是一次「为了复用按条目工作的函数」而做的往返转换。**

2. **过滤后为空时返回空文本条目** —— 与 IR 路径的处理一致。

3. **拼回时用 `if i > 0 { sb.WriteString("\n") }`** —— 所以是「行间插入换行」而不是「每行后加换行」。**这两种拼法对末尾的处理不同**（前者不产生尾随换行），而 `strings.Split` 的逆运算正是前者。

4. **`isEnglishItems(items)` 被调用两次**（一次在 `removeContentsTable` 前，一次在 IR 路径的 `removeTOCWord`）—— 两处都在判断「目录是否英文排版」，因为中英文目录的结构不同。

## 关键取舍

**源码事实：** 分派是四层叠加：`parse_method` 配置（`GetParserWithMethod` 先查）> 版本注入的 `parserFactories` > 内置 `switch` > 构造后的内容自检（`TryParser.TryParse`）。

**源码事实：** `selectParser` **先构造解析器再自检**，因为 `TryParse` 是解析器的方法——「判定格式」的能力属于具体解析器。代价是可能浪费一次构造。

**源码事实：** 自检的换器条件是四个：`actual != "" && actual != FileTypeOTHER && actual != fileType`。而 `TryParser` 的契约是「`FileTypeOTHER` 表示未知，保持请求的类型」——所以自检的失败方向是「不改变」。

**源码事实：** `selectParser` 返回「实际类型」，让调用方能知道「请求类型 ≠ 实际类型」（b02 据此打警告）。

**源码事实：** 命中 `parse_method` 时**不做内容自检**（直接 `return p, ft, nil`）——用户显式指定后端优先于自检结论。未命中时静默回退到默认路径（**配了未注册的方法名不会报错**）。

**源码事实：** `parserFactories` 由企业版在 `init()` 里注入，注释称「an edition can add parsers for its own file types without the community switch changing」；但它**也能替换内置族**（`RegisterParserFactory(FileTypePDF, ...)` 会覆盖 PDF），而名字只叫 Register。

**源码事实：** 两个注册表都是**无锁包级 map**，注释两处都要求「Register factories during package initialization」——运行期注册会有数据竞争。

**源码事实：** 没有「枚举已注册类型」的接口，所以能力集不可静态列举。

**源码事实：** `HasParseMethod("")` 返回 false（显式判断 + 注册时拒绝空方法名，两层冗余）；`parseMethodFactories[ft]` 对不存在的 ft 返回 nil map，而 Go 允许对 nil map 做下标读，所以不需要预判。

**源码事实：** 各解析器接收 `textOnly` 的方式不一致：多数 `p.textOnly = textOnly`，`PPT` 要穿透到内层 `p.pptx.textOnly`，而 `XLS`/`CSV`/`PDF`/`TXT`/`JSON`/`EMAIL`/`VISUAL`/`AURAL` **完全不接收**（它们的实现里没有媒体抽取步骤）。

**源码事实：** `Markdown` 族用 `p.mediaRefs = !textOnly`——所以 textOnly 下内嵌图片不是「被过滤」而是「根本没采集」。

**源码事实：** `newParser(FileTypeOTHER)` 会返回 `unsupported file type` 错误——所以「纯文本页模式」必须在调 `GetParser` 之前分流（b02 的 `dispatchParse` 正是这样做的）。

**源码事实：** `TextParser` 是空结构体（零大小）；注释列了 13 个扩展名并声明「Chunk boundaries belong to GeneralChunker, not to this parser」；编码探测结果写入 `File["encoding"]`，`size` 用原始字节数而非解码后长度。

**源码事实：** `TextParser` 输出**单一条目**，且不返回 `Err`（空文件产出空文本条目仍算成功）；`normalizeTextNewlines` 只折 CRLF/CR 为 LF，不做其他清洗（因为下游按 `\n` 切）。

**源码事实：** `DOCParser` 有三层判断：许可前置（`checkWPSLicense`）→ 结构化 IR 优先（保住表格，`#20918`）→ 扁平文本兜底（三视图选最长，但 IR 有 `"type":"table"` / `"type":"list"` 结构时无条件优先）。

**源码事实：** IR 路径的接受条件是三选一：有节 / IR 被修改过 / 「开着头尾移除但按不removing仍能产出节」。第三条的意图是「用户明确要求删头尾时，宁可返回空文档也不退回扁平文本把删掉的内容带回来」。

**源码事实：** `docExtract` 是变量而非函数（测试替换缝）；错误区分 `ErrOfficeCGORequired`（部署问题）与一般抽取失败（文档问题），靠 `errors.Is` 与 `%w` 包装。

**源码事实：** `extractDocText` 在下层「吞掉每视图错误」的前提下，只能用「选中视图与 markdown 都空」推断失败；注释明确「合法空载荷（扫描件关 OCR）罕见，报错比静默丢数据安全」（`Finding A`）。

**源码事实：** `removeContentsTable` 按条目工作，所以 `RemoveTOC` 路径把文本拆成行、造条目、过滤、再拼回（行间插换行，不产生尾随换行）。

**架构推论：** 「先构造再自检」这个顺序揭示了一条关于能力归属的判断：**识别与解析应该同源**。哪个组件最清楚某种格式的特征？解析它的人。代价是一次可能的浪费构造，而收益是「识别逻辑永远不会与解析逻辑脱节」——如果格式的 magic bytes 判断变了，它必然在改解析器时被发现。

**架构推论：** 「配了未注册的方法名静默走默认路径」是一处危险但可辩解的行为。可辩解之处在于**版本的注册表是编译期事实**，而配置文件可能在版本间复制——硬失败会让「降级主版本后配置失效」。危险之处在于**用户以为在用 MinerU 而实际用了 DeepDOC**。b02 里那段 `!parser.HasParseMethod(...)` 的 IO 追踪说明作者知道这个状态存在，并选择「记录它」而不是「拒绝它」。

**架构推论：** `DOCParser` 的三视图选择是本页最有教学价值的部分，因为它展示了**「长度」作为质量代理指标的失效条件**。IR 扁平化后的长度可能短于 PlainText，但信息更完整——所以「最长」这个启发式必须被一个「结构存在性」的判据覆盖。**判据是「不同的视图携带的信息是否是可比的」**：不可比则不能用同一度量。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/parser/parser/parser_type.go` | `TryParser` | 可选的内容自检接口（`FileTypeOTHER` = 未知） |
| `internal/parser/parser/parser_type.go` | `GetParser` | 摄取路径入口（`textOnly = false`） |
| `internal/parser/parser/parser_type.go` | `GetTextOnlyParser` | 只读文本路径入口（预览 / 附件 / Agent 上传 / CLI） |
| `internal/parser/parser/parser_type.go` | `selectParser` | 构造 → 自检 → 必要时重造 |
| `internal/parser/parser/parser_type.go` | `newParser` | 注册表优先 + 内置 switch（14 个族） |
| `internal/parser/parser/parser_type.go` | `RegisterParserFactory` | 版本注入（也能替换内置族） |
| `internal/parser/parser/parser_type.go` | `RegisterParseMethodFactory` | 按 (类型, 方法名) 注册覆盖 |
| `internal/parser/parser/parser_type.go` | `HasParseMethod` | 方法是否已注册（b02 的 IO 追踪判据） |
| `internal/parser/parser/parser_type.go` | `GetParserWithMethod` | 方法覆盖优先，未命中回退默认 |
| `internal/parser/parser/parser_type.go` | `parserFactories` / `parseMethodFactories` | 两个无锁包级注册表 |
| `internal/parser/parser/text_parser.go` | `TextParser` | `text&code` 族解析器（零大小结构体） |
| `internal/parser/parser/text_parser.go` | `NewTextParser` | 构造 |
| `internal/parser/parser/text_parser.go` | `TextParser.ParseWithResult` | 解码 + 归一 + 单条目输出 |
| `internal/parser/parser/text_parser.go` | `normalizeTextNewlines` | 只折 CRLF/CR |
| `internal/parser/parser/doc_parser.go` | `DOCParser` | 旧二进制 `.doc` 族（含 `wordParserOptions`） |
| `internal/parser/parser/doc_parser.go` | `NewDOCParser` / `NewDOCTextParser` | 两种模式构造 |
| `internal/parser/parser/doc_parser.go` | `DOCParser.ConfigureFromSetup` | 接收 DSL 的 `doc` 族配置 |
| `internal/parser/parser/doc_parser.go` | `DOCParser.ParseWithResult` | 许可校验 → IR 优先 → 扁平兜底 |
| `internal/parser/parser/doc_parser.go` | `extractDocText` | 三视图选择 + 「双空即失败」 |
| `internal/parser/parser/doc_parser.go` | `selectDocTextView` | 结构优先于长度 |
| `internal/parser/parser/doc_parser.go` | `irHasStructuredContent` | 靠 `"type":"table"` / `"type":"list"` 字符串包含 |
| `internal/parser/parser/doc_parser.go` | `docExtract` | 原生抽取的测试替换缝 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 文档：解析方法与支持的格式](https://ragflow.io/docs/dev/configure_knowledge_base)
- [Go 文档：init 函数与包级初始化顺序](https://go.dev/ref/spec#Package_initialization)
- [论文：文档格式识别与 magic bytes](https://www.rfc-editor.org/rfc/rfc6838)

## 与主核 / AI 的关系

「注册表 + switch 兜底」与主核（多模态数据库内核）的**访问方法（access method）注册**（`pg_am`）是同一个机制：核心提供默认实现，扩展可以注册自己的。而两者的差别在于**注册的可枚举性**：主核把 `pg_am` 做成一张系统表，所以「有哪些访问方法」是可查询的、可依赖的、可在 SQL 里选；这里的注册表是内存 map，没有枚举接口。

**这个差别的后果很具体**：主核的 `CREATE INDEX ... USING gin` 若写了不存在的 `gin`，报错「access method does not exist」；这里的 `parse_method` 写了未注册的名字，静默走默认。**判据是「注册是否是一等公民」**：是一等公民（进系统表）则能校验引用，是内部实现（内存 map）则引用只能静默降级。

第二处对照是 `selectParser` 的「先构造再自检」与主核的**类型输入函数**（`typinput`）。主核在读入一个字面量时，「判定类型」是先验的（来自列的 `atttypid`），不需要构造任何东西来探测。而这里的场景是「类型未知但字节已知」，所以必须用 `tryParse` 这类**嗅探**手段。

**这是「类型信息从哪里来」的根本分野**：有模式（schema）则类型是声明；无模式（任意上传文件）则类型必须从字节推断。**后者永远只能是启发式**，所以 `FileTypeOTHER` 这个「未知」状态是必需的。

第三处是 `irHasStructuredContent` 的字符串包含判断与主核的**统计信息**（`pg_statistic`）——不，更贴近的是「用 `strings.Contains` 检测结构」与主核的**快速路径判断**（如 `heap_page_has_free_space` 只读页头的一个字段而不解析整页）。两者都是「用最廉价的信号做判断」，而可接受性的前提是**误判的代价小于完整检查的成本**。这里误判的代价是「多用一个视图」，很轻。

对 RAG 工程的实际意义在于：解析器分派是所有 RAG 系统都要做的一层，而它的常见病是「扩展了就再也收不回」。这份实现给出的三条经验可直接用：**识别与解析同源**（让解析器自己判断格式，逻辑不会脱节）、**用户显式指定应优先于自动探测**（`parse_method` 命中就不自检）、以及**多视图选择不能用长度当唯一代理**（扁平化会丢掉只存在于结构化视图的信息）。
