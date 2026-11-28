---
project: ragflow
feature: doc-item
title: 文档条目与解析结果
page_type: feature
schema_version: 1
refs:
  - path: internal/parser/parser/doc_item.go
    symbols: [DocItem, TextItem, TableItem, ImageItem]
  - path: internal/parser/parser/doc_item.go
    symbols: [PositionKind, RawItem]
  - path: internal/parser/parser/parse_result.go
    symbols: [ParseResult, NewTextJSONItem]
---

# 文档条目与解析结果

## 一句话职责

定义解析产物的**统一形状**：四类条目（文本 / 表格 / 图片 / 原样透传）加一套「两套词汇共用五个槽位」的位置表示，并在整个转换链路里保证**线上 JSON 形状逐字节不变**——因为下游（分块器、索引器）读的仍是历史上那个 `map[string]any`。

## 机制是什么

这里要解决的问题是**类型化与向后兼容的矛盾**。

解析器内部当然希望有类型：文本有 `text`、表格有一个 `TableData` 结构、图片有图片数据或媒体引用。但下游已经按「松散 map + `doc_type_kwd` 判别键」写了很久，索引器、分块器、持久化都依赖那个键名与形状。

所以这套设计的做法是：**内部用类型，边界用 map，中间加一层显式投影**。

```go
type DocItem interface {
	// ToMap projects the item onto the historical wire shape. Callers must not
	// mutate the returned map's shared Extra contents.
	ToMap() map[string]any
}
```

而注释把设计意图写得很清楚：

> The design mirrors the existing typed chunk AST (`schema.ChunkDoc`) but lives in the parser package so **the parser layer does not import ingestion**. Ad-hoc pipeline keys (layout, page_number, _pdf_positions, sheet_index, media_order, parent_table_id, ...) that are not worth modelling as named fields are passed through verbatim via the `Extra` map, keeping `ToMap` a 1:1 projection of the historical map shape.

**三个约束同时成立**：

1. **分层不倒置** —— `parser` 不能 import `ingestion`，所以类型化 AST 放在 `parser`。
2. **不要为管线私有键建模** —— `layout` / `page_number` / `_pdf_positions` 这类键没有稳定语义，硬编码成字段会让类型定义被管线细节绑架。它们走 `Extra` 透传。
3. **投影必须 1:1** —— 不是「尽力保留」，而是「形状完全相同」。所以 `ToMap` 的每个分支都要仔细处理「某个字段在原文里到底存在不存在」。

第三条是最容易做错的地方，而这份实现里有三处专门为它写的分支（见下）。

**第二个机制是位置的双词汇。**

```go
// The single wire shape is a 5-tuple array, but it carries TWO
// vocabularies that share the same five slots:
//   - AbsoluteBox:  [page, left, right, top, bottom]  (PDF layout bbox)
//   - RowColumnBox: [sheet, rowStart, rowEnd, colStart, colEnd] (spreadsheet)
// `Kind()` is a method on the concrete type (not a stored field) so it can
// never disagree with the type.
```

**「五个槽位、两套含义、PDF 与表格的元组都是五个数字，无法从形状区分」** —— 这是本页最本质的困难。解法是**用一个外部的判别信号（`sheet_index` / `sheet`）来决定词汇**，而不是猜。

## 怎么实现的

### 四类条目：三个具名 + 一个逃生口

**`TextItem` 最简：**

```go
func (t TextItem) ToMap() map[string]any {
	m := map[string]any{
		"text":     t.Text,
		DocTypeKey: DocTypeText,
	}
	return mergeExtra(m, t.Extra)
}
```

**它总是写 `text`** —— 这一点在后面会成为一个问题的根源（见 `DocItemFromMap` 的 text 分支）。

**`TableItem` 有三个可选字段，每个都是「空则省略」：**

```go
m := map[string]any{
	"table":    t.Table,
	DocTypeKey: DocTypeTable,
}
if t.Sheet != "" {
	m["sheet"] = t.Sheet
}
if len(t.Positions) > 0 {
	m["positions"] = positionsToWire(t.Positions)
}
```

**「空则省略」而不是「写空值」** —— 因为下游可能用「键存在」判断，`"sheet": ""` 与「没有 sheet」在语义上不同。

**`ImageItem` 对 `image` 键的处理是本页最精细的一段：**

```go
// A ref-only image must not resurrect an empty "image" key: consumers use
// the key's absence to tell deferred bytes from an inline payload. Any
// present reference is emitted even when malformed, so the materializer
// can reject it instead of the conversion dropping it.
if i.Image != "" || i.Media == nil {
	m["image"] = i.Image
}
if i.Media != nil {
	m["media_ref"] = i.Media
}
```

**四处**：

1. **「只有媒体引用、没有内联字节」的图片不能凭空产生一个空 `image` 键** —— 因为下游靠「`image` 键在不在」区分「字节延迟加载」与「内联载荷」。

2. **`|| i.Media == nil` 这个附加条件** —— 意思是「没有 media 时，即使 image 是空串也要写」。为什么？因为**若两者都没有，那这条就是个空条目**，而 `ToMap` 应当忠实反映「原文有这个 `image` 键（值是空串）」。换句话说：`Image` 为空且无 `Media` 时，写 `"image": ""` 才是忠实的。

3. **「malformed 的引用也要发射」** —— 让下游的物化器（materializer）报错，而不是在转换层静默丢弃。**这是「错误要在能处理它的地方暴露」的判断**：转换层不知道什么是合法引用。

4. **`media_ref` 与 `image` 可以并存** —— 所以一个条目可以既带内联字节又带引用。

**`RawItem` 是逃生口：**

```go
// RawItem is a passthrough for items the parser constructs as an opaque map
// (the email content item, or any parser still emitting map literals during
// the migration). ToMap returns the map unchanged, so the wire shape is
// preserved exactly. The discriminator lives in the map under DocTypeKey.
type RawItem struct {
	M map[string]any
}

func (r RawItem) ToMap() map[string]any { return r.M }
```

**「迁移期仍有解析器直接产出 map 字面量」** —— 所以类型化不能强制，必须允许「原样透传」。而 `RawItem` 的存在让迁移可以增量进行。

### `MarshalJSON`：为下游的 json 往返而写

```go
// MarshalJSON projects each item onto its wire shape so that downstream
// consumers that round-trip the payload through encoding/json (the chunker
// decodes inputs["json"] via json.Marshal -> json.Unmarshal into []ChunkDoc)
// observe the historical map shape unchanged.
func (t TextItem) MarshalJSON() ([]byte, error)  { return json.Marshal(t.ToMap()) }
func (t TableItem) MarshalJSON() ([]byte, error) { return json.Marshal(t.ToMap()) }
func (i ImageItem) MarshalJSON() ([]byte, error) { return json.Marshal(i.ToMap()) }
func (r RawItem) MarshalJSON() ([]byte, error)   { return json.Marshal(r.M) }
```

**四处**：

1. **没有 `MarshalJSON` 的话，`json.Marshal(TextItem{...})` 会输出 `{"Text": "...", "Extra": {...}}`** —— 字段名首字母大写、且外面包一层 struct。而下游期待的是 `{"text": "...", "doc_type_kwd": "text"}`。

2. **「下游会 json 往返」是具体的事实** —— 注释点名了 chunker 的解码路径（`json.Marshal` → `json.Unmarshal` 到 `[]ChunkDoc`）。所以只要这条路径存在，`MarshalJSON` 就必须实现。

3. **`RawItem.MarshalJSON` 直接 `json.Marshal(r.M)`** —— 因为它本来就是 map。

4. **这四个方法让 `DocItem` 在 JSON 边界上「看起来就是 map」** —— 所以类型化对下游完全透明。**这是「类型化不泄漏」的正确做法。**

### `DocItemFromMap`：四个分支，每个都有一个「不可以强行类型化」的理由

```go
func DocItemFromMap(m map[string]any) DocItem {
	if m == nil {
		return RawItem{}
	}
	switch docType, _ := m[DocTypeKey].(string); docType {
	case DocTypeText:
		// Only build a TextItem when the map actually carries a text field.
		// A text-typed map without a text key (e.g. an email metadata item
		// whose body is gated by fields) must round-trip through ToMap
		// unchanged; TextItem.ToMap always writes "text", so forcing it here
		// would invent a text key the original map lacked.
		if _, ok := m["text"]; ok {
			return textItemFromMap(m)
		}
		return RawItem{M: m}
	case DocTypeImage:
		if _, ok := m["image"]; ok {
			return imageItemFromMap(m)
		}
		if entity.MediaRefFromAny(m["media_ref"]) != nil {
			return imageItemFromMap(m)
		}
		return RawItem{M: m}
	case DocTypeTable:
		if _, ok := m["table"]; ok {
			return tableItemFromMap(m)
		}
		return RawItem{M: m}
	default:
		return RawItem{M: m}
	}
}
```

**五处**：

1. **text 分支：有 `text` 键才类型化** —— 而理由是本页最精细的一条：`TextItem.ToMap` **总是写 `text`**，所以把一个「没有 `text` 键的 text 类型 map」强行类型化，会在 `ToMap` 时**凭空造出一个原始 map 没有的键**。

   注释举了具体例子：「an email metadata item whose body is gated by fields」。即邮件解析器产出的元数据条目，`doc_type_kwd = "text"` 但内容按字段门控，可能没有 `text`。

2. **image 分支：有 `image` 键** 或 **有可解析的 `media_ref`** —— 两者之一即可类型化。这与 `ToMap` 的写键规则正好互反：`ToMap` 在「`Image != "" || Media == nil`」时写 `image`。所以「有个 `media_ref` 但没有 `image`」时走类型化，`ToMap` 会输出 `media_ref` 而不输出 `image` —— 对称。

3. **table 分支：有 `table` 键** —— 而 `tableItemFromMap` 里还有第二道判断（见下）。

4. **`default` 与原样透传** —— 未知判别键都留 `RawItem`。

5. **`m == nil` 返回 `RawItem{}`（`M` 是 nil）** —— 所以 `RawItem.ToMap()` 返回 nil map。而 `ItemsToMaps` 里会跳过 nil item，但 `RawItem{}` 本身不是 nil 接口。**这是一个边角情况**：`ToMap()` 返回 nil，调用方需要能处理。

**`tableItemFromMap` 的第二道判断最有价值：**

```go
td, ok := m["table"].(*entity.TableData)
// A table-typed map whose "table" value is not a *entity.TableData (e.g. a
// JSON-decoded map[string]any) cannot be coerced into a TableItem without
// silently dropping the data — TableItem.ToMap would emit "table": null.
// Fall back to RawItem, preserving the map verbatim, exactly like the
// text/image branches do when their canonical field is absent.
if !ok {
	return RawItem{M: m}
}
```

**「JSON 解码出来的 `table` 是 `map[string]any` 而不是 `*entity.TableData`，强行类型化会让 `ToMap` 输出 `"table": null`」** —— 因为 `TableItem.Table` 是 `*entity.TableData` 类型，赋值不进去。

**这是一个很具体的类型断言陷阱**：`map[string]any` 里的值可以是任意类型，而结构体字段有确定类型。**所以「能取到键」不等于「能填进字段」。**

### `Extra` 的组装与合并：两个函数保证「规范字段永远赢」

```go
func extraFromMap(m map[string]any, canonical ...string) map[string]any {
	skip := make(map[string]struct{}, len(canonical))
	for _, c := range canonical {
		skip[c] = struct{}{}
	}
	extra := make(map[string]any)
	for k, v := range m {
		if _, ok := skip[k]; ok {
			continue
		}
		extra[k] = v
	}
	if len(extra) == 0 {
		return nil
	}
	return extra
}
```

```go
// mergeExtra copies Extra entries onto m without clobbering canonical keys
// that were already set. Canonical typed fields always win; ad-hoc pipeline
// keys (layout, page_number, level, ...) are added only when not already
// present, so a stray Extra key can never shadow text/table/image/doc_type_kwd.
func mergeExtra(m, extra map[string]any) map[string]any {
	for k, v := range extra {
		if _, ok := m[k]; !ok {
			m[k] = v
		}
	}
	return m
}
```

**四处**：

1. **`extraFromMap` 用 `skip` 集合排除规范字段** —— 所以「规范字段不会同时出现在 `Extra` 与结构体里」。否则序列化时会冲突。

2. **空 `Extra` 返回 `nil`** —— 所以「没有额外键」是可判定的（`nil` 而不是空 map）。这让 `mergeExtra` 的循环在无额外键时不做任何事。

3. **`mergeExtra` 的「不覆盖」方向是关键** —— 注释：「a stray Extra key can never shadow text/table/image/doc_type_kwd」。**即「规范字段 > Extra」的优先级在双向都成立**：从 map 拆到结构体时排除规范字段，从结构体拼回 map 时规范字段优先。

4. **`mergeExtra` 直接修改并返回 `m`** —— 所以它依赖调用方传入的是一个新构造的 map（`ToMap` 里正是这样）。若传入共享 map 会污染。

**而 `imageItemFromMap` 的 `skip` 是动态构造的：**

```go
pos := wireToPositions(m["positions"], hasSheetIndex(m))
media := entity.MediaRefFromAny(m["media_ref"])
skip := []string{"text", "image", DocTypeKey, "sheet"}
if media != nil {
	skip = append(skip, "media_ref")
}
if pos != nil {
	skip = append(skip, "positions")
}
```

**四处**：

1. **`media_ref` 只有在成功解析时才从 `Extra` 里排除** —— 若 `MediaRefFromAny` 返回 nil（引用畸形），它就留在 `Extra` 里，于是 `ToMap` 会把它原样透传出去（而不写到 `Media` 字段）。

   **这是一处精细的一致**：`ToMap` 只在 `Media != nil` 时写 `media_ref`，所以把畸形引用留在 `Extra` 里才能保证「原文有就输出有」。

2. **`positions` 同理** —— 解析成功才从 `Extra` 排除，否则原样透传。这直接对应 `wireToPositions` 的注释：「A nil / non-5-tuple input returns nil so the caller leaves the original value in `Extra` verbatim（positions-dropping regression guard）」。

3. **`sheet` 无条件排除** —— 因为它被填进了具名字段 `Sheet`。

4. **`skip` 的顺序无关** —— 它只是用来建集合。

### 位置的 inbound / outbound 两个缝

```go
// wireToPositions is the in-bound seam: it converts a raw `positions` value
// into structured Positions. A nil / non-5-tuple input returns nil so the
// caller leaves the original value in Extra verbatim (positions-dropping
// regression guard). It never panics.
func wireToPositions(raw any, hasSheet bool) Positions {
	if hasSheet {
		return rowColumnBoxesFrom(raw)
	}
	return absoluteBoxesFrom(raw)
}
```

**三处**：

1. **判别依据是外部信号 `hasSheet`，不是元组形状** —— 注释在 `hasSheetIndex` 上说明了为什么：

> it mirrors the downstream `isSpreadsheetChunk` (task/indexdoc) **exactly** — `sheet_index` present and non-nil, or a non-empty `"sheet"` string — so the parser's classification and the indexer's spreadsheet/PDF decision use **identical criteria and can never diverge**. A present-but-nil `sheet_index` is not a spreadsheet signal.

**「与下游完全一致的判据」是关键**：若这里用一套判据、索引器用另一套，同一个条目会在解析侧被当成表格、在索引侧被当成 PDF。而「present-but-nil 不算信号」是一处必须对齐的细节。

2. **`nil` 或非五元组返回 nil，让调用方保留原值** —— 这是「宁可透传也不丢」的守卫。注释称之为「positions-dropping regression guard」，说明这是一个曾经真实发生过的回归。

3. **「never panics」显式承诺** —— 因为输入来自任意 map，可能嵌套任意类型。

**而四个 `xxxFrom` 函数都遵循同一个失败契约：**

```go
func absoluteBoxesFromTyped(v [][]float64) Positions {
	out := make(Positions, 0, len(v))
	for _, t := range v {
		if len(t) != 5 {
			return nil
		}
		...
	}
	if len(out) == 0 {
		return nil
	}
	return out
}
```

**「任一元素不合格就整体返回 nil」** —— 不是「跳过坏元素」。理由：一个部分解析的 `Positions` 会让 `ToMap` 输出一个「少了几条」的数组，而原始数组有全部条目。**所以整体失败比部分成功更忠实。**

而 `len(out) == 0` 的二次检查是为了「非空输入但全被处理成零条」的情形（实际上循环里已经 return nil，所以这更像是防御）。

**outbound 方向的 `positionsToWire` 保留了两种容器类型：**

```go
//   - AbsoluteBox (PDF)  -> [][]any  (int page + float64 coords), matching the
//     shape normalizeDeepDocPositions already emitted and keeping the page a
//     genuine int (a [][]float64 container would coerce it to float64).
//   - RowColumnBox (xlsx/csv) -> [][]float64, matching the historical
//     spreadsheet wire the ingestion indexer consumes.
```

**四处**：

1. **PDF 用 `[][]any` 而表格用 `[][]float64`** —— 因为 PDF 的元组里 page 是 **int**，若统一成 `[][]float64` 会把它变成 float64（JSON 里 `1` 与 `1.0` 不同）。

2. **注释明确这不是正确性要求**：「Preserving each producer's historical container keeps the wire unchanged and avoids churning the existing `[][]float64` assertions；**it is NOT a correctness requirement**」。因为下游会把所有全数字数组统一解成 `[][]float64`。

   **这是一处诚实的说明**：作者知道两种容器的差异在下游会被抹平，保留它只是为了「不惊动既有断言」。

3. **`p[0].Kind()` 决定整个切片的方向** —— 注释：「A mixed slice (first element decides) cannot occur — a single item is either PDF or spreadsheet — so the `[][]any` fallback below is purely defensive and is never reached in practice」。

   **「理论上不会发生」的类型断言 + 防御分支** —— 而它还是写了。

4. **`positionsToWireAny` 在所有分支里都是兜底** —— 所以「类型断言失败」不会丢数据。

**`hasSheetIndex` 是唯一的判别实现：**

```go
func hasSheetIndex(m map[string]any) bool {
	if v, ok := m["sheet_index"]; ok && v != nil {
		return true
	}
	sheet, ok := m["sheet"].(string)
	return ok && sheet != ""
}
```

**两处**：

1. **`ok && v != nil`** —— 「present-but-nil 不算」。
2. **`sheet` 非空串也算** —— 所以「有 `sheet` 但没有 `sheet_index`」也走表格词汇。**这与 `TableItem.ToMap` 写 `sheet` 的规则（空则省略）配套**：一个 TableItem 只要 `Sheet != ""` 就会写出该键，于是再读回来时 `hasSheetIndex` 为真。

### `ParseResult`：删掉两个曾经存在的字段

```go
type ParseResult struct {
	File     map[string]any
	JSON     []DocItem
	Err      error
	Warnings []string
}

func (r ParseResult) Produced() bool {
	return r.Err == nil && (r.JSON != nil || r.File != nil)
}
```

**而注释解释了两个被删掉的字段：**

```go
// The owning Parser component hardcodes the wire `output_format` to "json"
// for every family; that value is a property of the component boundary, not
// of the parser result, so it is no longer carried on ParseResult. Backend
// markdown responses are normalized to JSON inside the parser package before
// they reach the component, so no raw markdown travels on this struct either.
```

**三处**：

1. **`output_format` 从 `ParseResult` 上移除了** —— 因为「恒为 json」是组件边界的属性，不是解析结果的属性。**「常量不该是字段」这个判断是对的**：一个所有实例都相同的字段不携带信息。

2. **markdown 在 parser 包内就被归一成 JSON** —— 所以 markdown 形态不外泄。这与 `DocItem` 的设计目标一致（对外只有结构化条目）。

3. **`Produced()` 替代了历史上的 `OutputFormat != ""` 判断** —— 注释明确：「It replaces the historical `OutputFormat != ""` success check」。

**这处替代带来一个语义变化**：以前「有 output_format」= 成功，现在「`Err == nil` 且（`JSON` 或 `File` 非 nil）」= 成功。所以**「有 File 无 JSON」也算成功**（例如只产出元数据的情形）。

**`File` 的注释澄清了一处容易误以为的事：**

> It is **not** an augmented copy of the upstream file descriptor.

**「它不是上游文件描述符的扩充副本」** —— 所以不要期待能从 `File` 里读到原始文件名/大小。它只放解析后端自己产出的元数据（`outline`、`page_count`、`format`）。

**而 `JSON` 的注释按族列出了形状差异：**

> PDF emits `text` + `doc_type_kwd` items (and optional `image` / `layout` / `positions` fields); Markdown / HTML / text emit normalized `{text, doc_type_kwd}` items; image emits an image payload and, after ingestion enhancement, its VLM description.

**三族三形状** —— 所以「条目形状取决于解析族」是显式的契约而不是隐含的。

**`NewTextJSONItem` 与 `NewTableDataJSONItem` 是两个规范构造器：**

```go
func NewTextJSONItem(text string) DocItem {
	return TextItem{Text: text}
}

func NewTableDataJSONItem(td *entity.TableData, sheet string, positions Positions) DocItem {
	return TableItem{Table: td, Sheet: sheet, Positions: positions}
}
```

**`NewTextJSONItem` 不设 `Extra`** —— 所以 `ToMap` 出来只有 `text` + `doc_type_kwd`。**这正是 `DocItemFromMap` 的 text 分支要求的形状**（有 `text` 键），所以往返一致。

**`NewTableDataJSONItem` 的注释说明了它与历史上 HTML 字符串的差别：**

> It carries the parsed `TableData` contract (caption, header rows, cell text already normalized the same way the legacy chunker normalized HTML `<td>`/`<th>` text) instead of the legacy HTML markup string, so the chunker reads rows directly without re-parsing any markup.

**「单元格文本已按历史 chunker 相同的规则归一」** —— 所以「不重新解析 HTML」的前提是「归一结果与当初解析 HTML 得到的相同」。**这是一处必须逐字节对齐的兼容性契约。**

### `MapsToItems` / `ItemsToMaps`：两个方向的批量转换

```go
func MapsToItems(maps []map[string]any) []DocItem {
	out := make([]DocItem, 0, len(maps))
	for _, m := range maps {
		out = append(out, DocItemFromMap(m))
	}
	return out
}

func ItemsToMaps(items []DocItem) []map[string]any {
	if items == nil {
		return nil
	}
	out := make([]map[string]any, 0, len(items))
	for _, it := range items {
		if it == nil {
			continue
		}
		out = append(out, it.ToMap())
	}
	return out
}
```

**四处**：

1. **`MapsToItems` 的用途是「迁移期适配」** —— 注释：「Parsers that accumulate results as `[]map[string]any` use it at the assignment site (`JSON: MapsToItems(items)`) so their internal logic is unchanged while the field gains the `DocItem` type」。

   **这是一处「最小侵入的类型化」**：解析器内部继续用 map 累积，只在赋给 `JSON` 字段时转一次。所以迁移不需要重写解析器。

2. **`ItemsToMaps` 的用途是「边界投影」** —— 注释：「The ingestion boundary calls this once so downstream consumers keep reading `[]map[string]any` unchanged」。

   **所以整个链路是**：解析器内部 map → `MapsToItems` → `[]DocItem` → `ItemsToMaps` → 下游 map。中间那段是类型化的，两端都是 map。**这就是「类型化不泄漏」的完整形态。**

3. **`ItemsToMaps(nil)` 返回 nil 而不是空切片** —— 而 `ItemsToMaps(空切片)` 返回空切片。**两者被区分**，与 `Produced()` 里 `r.JSON != nil` 的判断配套（nil 与空切片在 `Produced` 下都算「有 JSON」？不——`r.JSON != nil` 对空切片为真，对 nil 为假）。

   所以「`JSON` 是空的但非 nil」会通过 `Produced()`。**这是一处宽松的判定**：只要解析器建了空切片就算成功（即使没有条目）。

4. **`ItemsToMaps` 跳过 nil item，`MapsToItems` 却不跳过 nil map** —— 因为 nil map 会被 `DocItemFromMap` 转成 `RawItem{}`（一个有效但空的条目），而 nil `DocItem` 接口值无法调用 `ToMap()`（会 panic）。

## 关键取舍

**源码事实：** `DocItem` 是「内部类型化、边界 map 化」的接口；`ToMap` 的契约是**1:1 投影**历史 map 形状，而不是「尽力保留」。

**源码事实：** 管线私有键（`layout` / `page_number` / `_pdf_positions` / `sheet_index` / `media_order` / `parent_table_id`）走 `Extra` 原样透传，不建模成字段——避免类型定义被管线细节绑架。

**源码事实：** 类型化 AST 放在 `parser` 包而不是 `ingestion`，因为「the parser layer does not import ingestion」（分层不能倒置）。

**源码事实：** 四个条目类型里 `RawItem` 是逃生口，为「迁移期仍产出 map 字面量的解析器」保留原样透传。

**源码事实：** 每个条目类型都实现了 `MarshalJSON`，因为下游（chunker）会对 `inputs["json"]` 做 `json.Marshal → json.Unmarshal 到 []ChunkDoc` 的往返；不实现的话结构体会被序列化成 `{"Text": ..., "Extra": ...}` 而不是历史 map 形状。

**源码事实：** `TextItem.ToMap` **总是写 `text`**，所以 `DocItemFromMap` 只在 map 里**有 `text` 键**时才构造 `TextItem`——否则会凭空造出原始 map 没有的键（例：「body 按字段门控的邮件元数据条目」）。这一条是本页最精细的兼容性判断。

**源码事实：** `ImageItem.ToMap` 的写键条件是 `i.Image != "" || i.Media == nil`——「只有 media_ref 的图片不能凭空产生空 `image` 键」，因为下游用「`image` 键在不在于」区分「延迟加载的字节」与「内联载荷」；畸形引用也要发射，让物化器报错而不是转换层静默丢弃。

**源码事实：** `tableItemFromMap` 要求 `m["table"]` 是 `*entity.TableData`，否则退回 `RawItem`——因为 JSON 解码出来的 `map[string]any` 填不进该字段，强行类型化会让 `ToMap` 输出 `"table": null`（静默丢数据）。

**源码事实：** `extraFromMap` 把规范字段排除在 `Extra` 之外；`mergeExtra` 不覆盖已设的键；两者共同保证「规范字段 > Extra」在双向都成立（「a stray Extra key can never shadow text/table/image/doc_type_kwd」）。

**源码事实：** `imageItemFromMap` 的 `skip` 里 `media_ref` / `positions` 只在**成功解析**时才加入——解析失败时它们留在 `Extra` 里原样透传，与 `ToMap` 的写键规则形成对称，保证「原文有就输出有」。

**源码事实：** `wireToPositions` 用外部信号 `hasSheetIndex` 决定位置词汇，不猜元组形状；`hasSheetIndex` 的判据（`sheet_index` present 且非 nil，或非空 `sheet` 字符串）**与下游 `isSpreadsheetChunk` 完全一致**，注释强调「can never diverge」；present-but-nil 不算信号。

**源码事实：** 四个 `xxxFrom` 转换函数都遵循「任一元素不合格则整体返回 nil」——因为部分解析的 `Positions` 会让 `ToMap` 输出少几条，而原文有全部条目；整体失败比部分成功更忠实，且 nil 让调用方保留原值（注释称之为 positions-dropping regression guard）。

**源码事实：** `positionsToWire` 按 `p[0].Kind()` 决定容器类型：PDF 用 `[][]any`（保住 page 是 int，`[][]float64` 会把它变成 `1.0`）、表格用 `[][]float64`；注释明确这一处「**NOT a correctness requirement**」——保留只是为了避免惊动既有的 `[][]float64` 断言，因为下游 `decodeStructuredValue` 会把所有全数字数组统一解成 `[][]float64`。

**源码事实：** `ParseResult` 删掉了历史上的 `output_format`（因为它恒为 `"json"`，属于组件边界而非解析结果）与 markdown 载荷（在 parser 包内就归一成 JSON）；`Produced()` 替代 `OutputFormat != ""`，判据是「`Err == nil` 且（`JSON` 或 `File` 非 nil）」。

**源码事实：** `ParseResult.File` 「不是上游文件描述符的扩充副本」，只放解析后端自己产出的元数据（`outline` / `page_count` / office 的 `format`）。

**源码事实：** `NewTableDataJSONItem` 携带的 `TableData` 里，单元格文本「已按历史 chunker 解析 HTML `<td>`/`<th>` 时相同的规则归一」——所以「不重新解析 markup」的前提是逐字节等价的归一结果。

**源码事实：** `Produced()` 里 `r.JSON != nil` 对**空切片为真**，所以「建了空切片但没条目」也算成功；`ItemsToMaps(nil)` 返回 nil 而 `ItemsToMaps(空切片)` 返回空切片；`ItemsToMaps` 跳过 nil item 而 `MapsToItems` 不跳过 nil map（后者会被转成 `RawItem{}`，前者会 panic）。

**架构推论：** 这套设计的核心手法是「**类型只存在于中间，两端都是 map**」。解析器内部可以继续写 map（迁移成本为 0），下游继续读 map（改动为 0），而中间那段获得类型检查。代价是**两个投影函数必须精确**：`ToMap` 与 `DocItemFromMap` 的每一处「写不写这个键」都要互相对称，否则往返会改变数据。

**这个代价具体体现在三条规则上**（text 必须有 `text` 键才类型化、image 的写键条件、table 的类型断言），而三条都是「为了避免 `ToMap` 造出原始数据没有的东西」。**判据是「投影是否可能无中生有」**：只要某个 `ToMap` 分支会写一个可选键，对应方向的类型化就必须先确认该键原本存在。

**架构推论：** `hasSheetIndex` 与下游 `isSpreadsheetChunk` 「判据必须完全一致」这条要求，说明了**同一个分类决策在两个包里的危险**。这类重复判定的正确做法只有两种：把判定实现放在一个地方供两方调用，或把两个实现的判据逐字对齐并在两侧注释互相引用。这份实现选了后者（受分层限制无法共享），并用注释点名了对方的具体函数名——这是让「对齐」可被审查的关键。

**架构推论：** 「整体返回 nil 而不是跳过坏元素」这个失败契约值得单独记。它的思路是：**转换的忠实性是对「整个值」而言，不是对「每个元素」而言**。一个少了三条的 `Positions` 数组在语义上已经不是原来的数组，所以宁可整体放弃（让原值透传）也不要部分保留。这与「尽力而为的解析」（如 JSON 容错抽取）是相反取向，而分野在于**是否有地方能保存原值**：能保存则整体放弃更好，不能保存则部分保留更好。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/parser/parser/doc_item.go` | `DocItem` | 「内部类型化、边界 map 化」的接口（`ToMap`） |
| `internal/parser/parser/doc_item.go` | `TextItem` | 文本条目（`text` + `Extra`） |
| `internal/parser/parser/doc_item.go` | `TableItem` | 表格条目（`TableData` + sheet + positions） |
| `internal/parser/parser/doc_item.go` | `ImageItem` | 图片条目（内联字节 / 媒体引用二选一或并存） |
| `internal/parser/parser/doc_item.go` | `RawItem` | 原样透传的逃生口 |
| `internal/parser/parser/doc_item.go` | `MapsToItems` / `ItemsToMaps` | 两个方向的批量转换（链路两端都是 map） |
| `internal/parser/parser/doc_item.go` | `DocItemFromMap` | 按判别键构造（四个分支，每个都有「不可强行类型化」的理由） |
| `internal/parser/parser/doc_item.go` | `textItemFromMap` / `imageItemFromMap` / `tableItemFromMap` | 三个具名类型的拆解 |
| `internal/parser/parser/doc_item.go` | `extraFromMap` / `mergeExtra` | `Extra` 的双向组装与「规范字段优先」 |
| `internal/parser/parser/doc_item.go` | `stringField` | 类型断言的字符串取值 |
| `internal/parser/parser/doc_item.go` | `PositionKind` | 位置词汇判别（绝对框 / 行列框） |
| `internal/parser/parser/doc_item.go` | `Position` / `AbsoluteBox` / `RowColumnBox` | 三个位置类型的 `Wire()` 与 `Kind()` |
| `internal/parser/parser/doc_item.go` | `Positions` | 位置列表 |
| `internal/parser/parser/doc_item.go` | `SpreadsheetPositions` | 生产者侧的显式词汇选择 |
| `internal/parser/parser/doc_item.go` | `hasSheetIndex` | 词汇判别（与下游 `isSpreadsheetChunk` 完全一致） |
| `internal/parser/parser/doc_item.go` | `wireToPositions` / `positionsToWire` | inbound / outbound 两个缝 |
| `internal/parser/parser/doc_item.go` | `absoluteBoxesFrom` / `rowColumnBoxesFrom` | 四种「整体失败」的转换 |
| `internal/parser/parser/doc_item.go` | `positionFloat` / `toInt` | 数值宽松转换 |
| `internal/parser/parser/parse_result.go` | `ParseResult` | 解析结果（`File` / `JSON` / `Err` / `Warnings`） |
| `internal/parser/parser/parse_result.go` | `Produced` | 成功判据（替代 `OutputFormat != ""`） |
| `internal/parser/parser/parse_result.go` | `ParseResultProducer` | 解析器统一契约 |
| `internal/parser/parser/parse_result.go` | `DocTypeKey` / `DocTypeText` / `DocTypeTable` / `DocTypeImage` | 判别键与三个取值 |
| `internal/parser/parser/parse_result.go` | `NewTextJSONItem` | 规范文本条目构造（不设 `Extra`） |
| `internal/parser/parser/parse_result.go` | `NewTableDataJSONItem` | 规范表格条目构造（已归一的单元格文本） |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 文档：支持的文档格式](https://ragflow.io/docs/dev/configure_knowledge_base)
- [论文：PDF 结构化抽取的版面与位置建模](https://arxiv.org/abs/2307.12559)
- [Go 文档：encoding/json 与自定义 MarshalJSON](https://pkg.go.dev/encoding/json)

## 与主核 / AI 的关系

「位置的两套词汇共用五个槽位」与主核（多模态数据库内核）的**多态类型（polymorphic type）**是同一类问题：同一个存储槽在不同类型下有不同解释。Postgres 的 `anyelement` / `anyarray` 就是靠**类型参数**显式携带解释，而 SQLite 的 `BLOB` 则是「解释由使用者决定」——后者正是这里 `positions` 字段的处境。

**而这里的选择是「加一个外部判别信号」而不是「把解释写进值里」**。这与 SQLite 的做法一致（它靠 `typeof()` 与列声明来解决），代价是**判别信号一旦缺失就无法解释**。而 `hasSheetIndex` 必须与下游完全一致的注释，就是在为这个代价付款。

第二处对照是 `MarshalJSON` 与主核的**行/列格式转换层**。两者都是「内部表示与外部表示不同，必须在边界转换」。主核的 `heap_form_tuple` / `heap_deform_tuple` 是纯函数式的双向转换，而这里的 `ToMap` / `DocItemFromMap` 也是。差别在主核的转换由类型元组（`TupleDesc`）驱动、是**完全通用**的；这里按判别键分四个分支、是**特化**的。判据是「表示的变体是否可枚举」：可枚举则可分派，不可枚举则必须靠元数据驱动。

第三处是「整体返回 nil 而非跳过坏元素」与主核的**输入校验**。`COPY FROM` 遇到坏行会拒绝整批（除非 `ON_ERROR ignore`）。默认行为是「整批失败」，因为部分导入会让「表里的数据量」与「源文件的行数」不再可推断。这与 `Positions` 的整体放弃完全同构：**部分成功的代价是「无法从结果推断输入」**。

对 RAG 工程的实际意义在于：解析产物的形状是整条流水线的地基，而它最容易被「随手加个字段」破坏。这份实现给出的三条经验可直接用：**类型化只放中间、两端保持 map**（迁移成本与下游改动都为 0）、**投影函数的每个可选键都要与反方向的类型化条件对称**（否则往返会无中生有）、以及**同一分类决策在两个包里重复时必须逐字对齐并互相点名**（`hasSheetIndex` ↔ `isSpreadsheetChunk`）。
