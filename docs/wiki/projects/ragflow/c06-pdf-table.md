---
project: ragflow
feature: pdf-table
title: PDF 表格重建
page_type: feature
schema_version: 1
refs:
  - path: internal/deepdoc/parser/pdf/table/table_construct.go
    symbols: [ConstructTable, StripCaptionFromCells]
  - path: internal/parser/tableutil/table_html.go
    symbols: [ParseTableHTML, RenderTableHTML, BuildTableData]
---

# PDF 表格重建

## 一句话职责

把表格识别器（TSR）吐出的一堆**带行列标注的单元格**整理成行列网格、裁掉孤立行列、算合并跨度、标记被吞并的格子，然后渲染成 HTML——同时提供反向的 HTML → 结构化 `TableData` 解析，让两个方向可以往返。

## 机制是什么

表格在 PDF 里同样是「没有结构」的：TSR（表格结构识别）模型的输出是「这些框里有哪些是单元格、它们大概在哪」——**行列关系是靠标注（`R`/`C`）或坐标推断的**，而模型会犯错：

- 多输出空行/空列（phantom band）
- 把跨页表格的边缘识别成独立列
- 把表头的行号标错
- 把障碍物（图上标注）当成单元格

所以 `constructTableCore` 的主体不是「组装」，而是**清理**：

```go
rows = CleanupOrphanColumns(rows)
rows = DropAllEmptyRows(rows)
rows = CleanupOrphanRows(rows)
```

**三步的顺序有讲究**：先清列（因为「孤立列」是整个列在其它行都没内容）、再删空行（TSR 的假行）、最后清行（行级的孤立）。而注释说明了这个顺序的后果：「Cleanup may drop rows/columns, so recompute `item.Grid` and `item.Rows` after both passes」。**因为每一步都可能改变维度，所以缓存必须在三步之后重建。**

**第二个机制是「两个方向的对称」。**

| 方向 | 函数 | 输入 | 输出 |
|---|---|---|---|
| HTML → 结构 | `ParseTableHTML` | `<table>` 文本 | `*entity.TableData` |
| 结构 → HTML | `RenderTableHTML` | `*entity.TableData` | `<table>` 文本 |

**而这个对称是必需的，因为整条链路里表格被两种形式交替使用**：

- **解析器**（c02 的 `DOCParser`）从 office 文档拿到 IR，转成 HTML 让分块器渲染；
- **分块器**把 HTML 转回结构化行以便直接读单元格；
- **持久化**把 HTML 存进块文本，**而块 id 是那串文本的哈希**。

最后一条是关键的约束：注释写着「the chunk's Text with it, and the chunk id is a hash of that rendered text」——**所以 `RenderTableHTML` 的输出必须逐字节稳定**，否则同一张表在两次运行中会得到不同块 id。

**第三个机制是「被吞并格子的显式标记」。**

```go
type origin struct {
	row, col int
}
```

而 `buildGrid` 的注释把这个模型讲得很清楚：

> A merged cell (ColSpan/RowSpan > 1) occupies its origin slot and the engulfed positions become Covered markers pointing back at the origin; overlapping spans clamp rather than double-count.

**「覆盖位不是空位，而是指回原位的标记」** —— 这让网格保持 dense（每个逻辑位置恰好出现一次），而不需要「填充空格子」，后者会丢失「这个位置是被吞并的」这个信息。

## 怎么实现的

### `ParseTableHTML`：三个结构特例

**第一处：`<tfoot>` 被单独收集、最后追加。**

```go
var footRows [][]CellInput
var footFlags []bool
inFoot := false
...
if isHTMLElement(n, "tfoot") && !inFoot {
	inFoot = true
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		walk(c)
	}
	inFoot = false
	return
}
...
walk(doc)
rows = append(rows, footRows...)
headerFlags = append(headerFlags, footFlags...)
```

**四处**：

1. **`inFoot` 是遍历状态** —— 因为 `walk` 是递归的，`tfoot` 的子节点会被同一函数处理，靠这个布尔区分「现在在收集脚」。

2. **`!inFoot` 的守卫防嵌套** —— 一个 `tfoot` 里若又出现 `table`（含 `tfoot`），不会重复进入。

3. **`footRows` 在两个地方被追加**：`walk` 结束时，以及**每个 `table` 节点结束时**：

```go
if isHTMLElement(n, "table") && !inFoot {
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		walk(c)
	}
	rows = append(rows, footRows...)
	headerFlags = append(headerFlags, footFlags...)
	footRows, footFlags = nil, nil
	return
}
```

   **注释说明了为什么要在 `table` 边界追加**：「A table's footer goes below that table's own body, not below every table the fragment holds」。

   **所以一个片段里有两个表时，第一个表的 foot 会在第一个表结束时被追加**，而不是等到整个片段结束。

4. **理由注释给了历史依据**：「HTML 4 required it before `<tbody>`, so its rows are collected apart and placed last; read in source order, a `"Total"` row came before the data it sums」。

   **这是一处真实的版面修正**：`<tfoot>` 在源码里出现在 `<tbody>` 之前（HTML 4 的 DTD 要求），但语义上是「页脚」。直接按源码顺序读会让「合计」行排在数据之前。

**第二处：`<tr>` 里不再递归。**

```go
if isHTMLElement(n, "tr") {
	var cells []CellInput
	anyTh := false
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		if isHTMLElement(c, "td") || isHTMLElement(c, "th") {
			...
		}
	}
	...
	// Do not recurse into the row's cells: a nested <table> inside a
	// cell is folded into that cell's text by cellText, not parsed as
	// a separate row.
	return
}
```

**三处**：

1. **只取直接子节点**（`n.FirstChild` 循环，不递归）—— 所以 `td` 里嵌套的 `table` 不会被当成行。

2. **嵌套表格的文本被折叠进外层单元格** —— 而折叠由 `cellText` 实现（它会下降穿透所有非惰性元素）。

3. **`anyTh` 决定「这一行是表头」** —— 只要有一个 `th` 就算。**这是一处宽松判据**：混合 `td`/`th` 的行整行都当表头。

**第三处：`<caption>` 被单独取出。**

```go
if isHTMLElement(n, "caption") {
	caption = cellText(n)
	return
}
```

**`return` 而不是继续下降** —— 所以 caption 的文本不会混进任何行。而它被赋给局部变量，最后传给 `BuildTableData`。

**而入口处有一段「补 table 包裹」：**

```go
lower := strings.ToLower(s)
if strings.Contains(lower, "<tr") && !strings.Contains(lower, "<table") {
	s = "<table>" + s + "</table>"
}
```

**三处**：

1. **只有「有 `<tr>` 但没有 `<table>`」才补** —— 因为 `html.Parse` 对裸 `<tr>` 会把它放到 body 下（而不是自动建 table），于是 `walk` 找不到 `table` 边界，`tfoot` 的处理也会失效。

2. **`strings.ToLower(s)` 对整个字符串做一次拷贝** —— 对大表格是一次可观的开销。**这是为了大小写不敏感匹配**。

3. **`strings.Contains(lower, "<table")` 不检查 `"</table"`** —— 所以「有开标签没有闭标签」的畸形输入不会被补包裹，交给 `html.Parse` 的容错处理。

### `BuildTableData`：表头行数的一个默认值

```go
headerCount := 0
for headerCount < len(headerFlags) && headerFlags[headerCount] {
	headerCount++
}
if len(rows) > 0 && headerCount == 0 {
	headerCount = 1
}
```

**三处**：

1. **表头是「前导连续的真值段」** —— 一旦遇到非表头行就停。所以「表头、数据、又一行表头」的表格只把第一个也算进去。

2. **`headerCount == 0` 且有行时兜到 1** —— 所以「没有任何 `th`」的表格把**第一行**当表头。

   **这是一处有意的乐观默认**：没有显式表头的表格，第一行大概率是列名。而代价是「第一行是数据」的表格会被误标表头。

   注释只说「defaulting to 1 when the table has any rows」，没给理由。

3. **`headerCount` 可能超过实际行数** —— 因为 `headerFlags` 的长度等于 `rows` 的长度（两处 `append` 都成对），所以不会。但若调用方传不一致的切片，`headerCount` 会大于 `len(rows)`，而 `RenderTableHTML` 里 `isHeader := i < td.HeaderRows` 对 `i` 在范围内仍成立——**所以多余的表头数不影响渲染**。

**然后是跨度的双向 clamp：**

```go
for r := range rows {
	for i := range rows[r] {
		rows[r][i].ColSpan = clampSpan(rows[r][i].ColSpan, maxColSpan)
		rows[r][i].RowSpan = clampSpan(rows[r][i].RowSpan, maxRowSpan)
	}
}
```

**注释说明了为什么**：「to keep a hostile or malformed span from driving a multi-gigabyte allocation inside `buildGrid`'s width pre-pass」。

**「一个恶意的 `rowspan="1000000"` 会让宽度预扫描分配巨量内存」** —— 而 clamp 是 O(1) 的防护。

### `buildGrid`：两遍扫描，而第二遍有容量精确计算

**第一遍（预扫描）只为算宽度，但它必须模拟主循环的全部放置规则。**

```go
covered := make([]map[int]origin, len(rows))
width := 0
for r, row := range rows {
	col := 0
	ci := 0
	for {
		if _, ok := covered[r][col]; ok {
			col++
		} else if ci < len(row) {
			c := row[ci]
			ci++
			cs := spanOf(c.ColSpan)
			rs := spanOf(c.RowSpan)
			col += cs
			if rs > 1 {
				for dr := 1; dr < rs; dr++ {
					rr := r + dr
					if rr >= len(covered) { break }
					if covered[rr] == nil { covered[rr] = map[int]origin{} }
					for dc := 0; dc < cs; dc++ {
						covered[rr][col-1-dc] = origin{r, col - cs}
					}
				}
			}
		} else {
			next := nextCoveredCol(covered[r], col)
			if next < 0 { break }
			col = next
		}
		if col > width { width = col }
	}
}
```

**五处**：

1. **`covered` 在预扫描里就被填满** —— 所以主循环直接复用它。**这是一处「预扫描顺带做实事」的设计**：它不是纯粹的测量，而是产生了主循环需要的输入。

2. **`covered[r][col]` 对 nil map 的下标读是安全的**（Go 允许），所以第一行的 `covered[0]` 是 nil 时 `if _, ok := covered[r][col]; ok` 直接为假。**这省掉了初始化判断。**

3. **`for dc := 0; dc < cs; dc++ { covered[rr][col-1-dc] = origin{r, col - cs} }`** —— 把「本行的 `[col-cs, col-1]` 这段跨度」在后续行标为被覆盖，指针回原位 `(r, col-cs)`。

   而注释在 `BuildTableData` 对应的主循环处说明了「The back-pointer names the origin slot (`col`), not the engulfed position (`col+dc`)」 —— **往回指原位，而不是指向自己**。

4. **`else` 分支的 `nextCoveredCol` 是关键** —— 注释：

> No real cell at this column. An ancestor may still own columns further right, so jump to the next covered column instead of stopping; otherwise the row is exhausted.

   **「这一行的实体格用完了，但祖先的 rowspan 还可能占据更右边的列」** —— 若直接 `break`，会漏掉那些被覆盖的位置，宽度就偏小。

5. **`width` 只增不减** —— 所以所有行的最大到达列决定总宽。

**为什么必须模拟全部规则？** 注释给了具体后果：

> If the width only summed each row's own colspan (the legacy computation), an ancestor rowspan would push a later row's cells past the width and **its trailing real cells would be silently dropped by the column guard** — both losing data and regressing the legacy Rows text view.

**「祖先的 rowspan 会把后续行的格子推到宽度之外，于是行尾的真实格子会被列守卫静默丢弃」** —— 这是从旧实现继承来的具体缺陷，而修法是「预扫描也走 rowspan 逻辑」。

**第二遍的容量计算是一处精细的优化：**

```go
realCols := 0
for _, c := range row {
	realCols += spanOf(c.ColSpan)
}
grid[r] = make([]entity.TableCell, 0, realCols+len(covered[r]))
```

**注释解释了为什么不用 `width`**：「a wide row elsewhere must not make every short row allocate megabytes of unused backing capacity」。

**「一行有 1000 列而这一行只有 3 格时，按 `width` 分配会浪费 1000 格」** —— 而 `realCols + len(covered[r])` 是**精确值**：真实格贡献 `colspan` 个位置，被覆盖位贡献 `len(covered[r])` 个。所以「append 永不超出容量」。

**第二遍的主循环里有两处 `col++` 的「空转」：**

```go
if ci >= len(row) {
	// No more real cells in this source row, but an ancestor's
	// rowspan may still own columns further right: keep advancing
	// so those Covered markers are emitted instead of stopping
	// early (which would drop a rowspan continuation and break the
	// dense-grid invariant).
	col++
	continue
}
```

**三处**：

1. **`col++` 而不是 `break`** —— 为了继续发出「更右边的被覆盖位」。若 `break`，那些位置就消失了，网格不再 dense。

2. **注释用了「dense-grid invariant」这个说法** —— 说明 dense 是一个**显式不变量**，而不是偶然性质。

3. **这个分支只在「本行真实格用完」时才走** —— 所以它靠 `col < width` 循环条件自然终止。

**而「不对行做填充」是一条明确的设计：**

```go
// Rows are NOT padded to a uniform width: a row that genuinely has
// fewer cells than the widest row keeps its actual (ragged) length so
// CellTexts() reproduces the legacy row-major Rows exactly. Only
// merged cells (colspan/rowspan) introduce Covered markers; a real
// gap is simply a shorter row, which both the consumer text view and
// HTML render handle without padding artifacts.
```

**四处**：

1. **「不填充」是为了让 `CellTexts()` 精确复现旧的行主序数组** —— 所以是**兼容性约束**，不只是美学选择。

2. **「真实缺口就是一个更短的行」** —— 所以 ragged 是被接受的形态，而不是需要规整的异常。

3. **只有合并格才产生 `Covered` 标记** —— 所以「短行」与「被合并吞并」是两种不同的短，用「有没有 `Covered`」区分。

4. **`RenderTableHTML` 能处理 ragged** —— 因为它逐格渲染、遇 `Covered` 就跳过，不依赖行等长。

**最后是两处收尾：**

```go
// Clamp stored spans to the grid so every claimed position is backed.
for r := range grid {
	w := len(grid[r])
	for c := range grid[r] {
		cell := &grid[r][c]
		if cell.Covered { continue }
		if cell.RowSpan > len(grid)-r { cell.RowSpan = len(grid) - r }
		if cell.ColSpan > w-c { cell.ColSpan = w - c }
	}
}
// Trim trailing rows that are completely empty
for len(grid) > 0 && len(grid[len(grid)-1]) == 0 {
	grid = grid[:len(grid)-1]
}
```

**三处**：

1. **跨度回剪** —— 注释：「so every claimed position is backed」。所以「声明的跨度不能超出真实存在的格子」。

   **注意这个回剪发生在「网格建好之后」**，而前面 `clampSpan` 只防「恶意大值」。**所以是两道防护**：粗的（`maxRowSpan`）防 OOM，细的（按实际网格）防虚假声明。

2. **尾部的空行被裁掉** —— 而注释强调「A row that only carries Covered markers is a genuine rowspan continuation and MUST stay」：

   **「只有 `Covered` 标记的行必须保留」** —— 因为若删掉它，某个原位的 `RowSpan` 就超出了实际行数。**所以判定条件是 `len(grid[len(grid)-1]) == 0`（完全空）而不是「没有真实格」。**

3. **`for` 循环而不是找一个边界** —— 因为可能有多行连续为空。

### `RenderTableHTML`：逐字节稳定的输出

```go
b.WriteString("<table>")
if td.Caption != "" {
	b.WriteString("<caption>")
	b.WriteString(html.EscapeString(td.Caption))
	b.WriteString("</caption>")
}
b.WriteString("\n")
for i, row := range td.Grid {
	isHeader := i < td.HeaderRows
	b.WriteString("<tr>")
	for _, cell := range row {
		if cell.Covered { continue }
		tag := "td"
		if isHeader { tag = "th" }
		...
	}
	b.WriteString("</tr>")
	b.WriteString("\n")
}
b.WriteString("</table>")
b.WriteString("\n")
```

**六处**：

1. **输出是手工拼字符串**（`strings.Builder` + `WriteString`），不是用 `html/template` —— 所以**格式完全确定**：属性的引号、空格位置、换行位置都是写死的。

   **这是「块 id 是文本哈希」这个约束的直接后果**：任何格式上的不确定性都会让同一张表产生不同块 id。

2. **`html.EscapeString` 用于 caption 与 cell 文本** —— 所以 `<`/`>`/`&` 被转义。**这是必需的正确性**（否则内容里的 `<` 会破坏标记结构）。

3. **`Covered` 格被跳过** —— 因为它们的语义由原位的 `colspan`/`rowspan` 表达。

4. **`isHeader := i < td.HeaderRows`** —— 所以表头是「前 N 行」而不是「带 `th` 的行」。**这与 `BuildTableData` 计算的 `headerCount` 完全对应**（前导连续段），所以往返一致。

5. **`colspan`/`rowspan` 只在 `> 1` 时写出** —— 所以没有合并的表格 HTML 里不含这两个属性。

6. **每个 `</tr>` 后都有一个 `\n`，`</table>` 后也有一个 `\n`** —— 所以输出以换行结尾。**这与 `ParseTableHTML` 的输入容错配合**（它不在乎空白）。

   而 `<table>` 后**没有**换行（`b.WriteString("\n")` 在 caption 判断之后）—— 所以无 caption 时是 `<table>\n`；有 caption 时是 `<table><caption>...</caption>\n`。**这个细节也必须稳定。**

### `ConstructTable` 的 EE 分支与 OSS 路径

```go
func ConstructTable(cells []pdf.TSRCell, boxes []pdf.TextBox, caption string, item *pdf.TableItem) string {
	if item != nil && item.RenderVH {
		if item.GridUnified {
			return ConstructTableVH(gridToStreamCells(item.Grid), nil, caption)
		}
		if len(item.VHPageSpans) > 0 {
			return ConstructTableVHPaged(item.Cells, boxes, item.VHPageSpans, caption)
		}
		cropBoxes := make([]pdf.TextBox, len(boxes))
		for i := range boxes {
			cropBoxes[i] = BoxToCropSpace(boxes[i], item.Scale, item.CropOffX, item.CropOffY)
		}
		return ConstructTableVH(item.Cells, cropBoxes, caption)
	}
	return constructTableCore(cells, boxes, caption, item)
}
```

**四处**：

1. **OSS 永远走 `constructTableCore`** —— 注释明确：「All branches inside the RenderVH block are EE-only: no OSS-built TableItem sets RenderVH」。

2. **三个 EE 子分支的优先级**：`GridUnified`（已统一网格）> `VHPageSpans`（跨页）> 默认（裁剪空间转换）。

3. **`BoxToCropSpace` 的坐标转换** —— 因为 TSR 在裁剪后的图上跑，而文本框坐标是原页面的。所以默认分支要转。

4. **`gridToStreamCells` 会先 `dropEmptyColumns`** —— 注释：「phantom/edge bands the EE V/H renderer must not emit」。**所以 EE 渲染器不能容忍空列**，而 `buildGrid` 的「不填充」策略会让空列存在。

   **这是一处两个渲染器的需求差异**：`RenderTableHTML` 能处理 ragged 与空列，而 V/H 渲染器不能。

**而 `StripCaptionFromCells` 是「防御纵深」的一层：**

```go
for i := range cells {
	t := strings.TrimSpace(cells[i].Text)
	if t == "" { continue }
	// Clear cells that match caption patterns (e.g. "表1", "Table 1").
	if IsCaptionBox(t, "") {
		cells[i].Text = ""
	}
}
```

**而 `constructTableCore` 的第一句就是调它**，注释说：

> Strip caption-like text from cells (defense-in-depth: fillCellTextFromBoxes may include caption text that doesn't match isCaptionBox patterns).

**「上游的 `fillCellTextFromBoxes` 可能把不匹配 caption 模式的水印文本也带进来」** —— 所以这里再剥一次。而「清空文本而不删格」是关键：**保留格子的几何**（否则行列结构会变）。

**而那个空函数的注释本身就是一处说明：**

```go
// Second pass: if the first row (lowest Y) has all-numeric/numbering text
// (e.g. "1", "1.", "一"), it's likely a caption numbering line — clear it.
// But don't clear actual numeric data cells.
// This pass is intentionally conservative — only clears clearly-non-data text.
}
```

**这是 `StripCaptionFromCells` 的末尾注释，描述了一个「第二趟」处理——但代码里只有一趟。** 所以注释描述的是**未实现或已删除的**逻辑。这是一处需要在页面上如实指出的事实：注释与代码不符。

## 关键取舍

**源码事实：** `constructTableCore` 的主体是三步清理（`CleanupOrphanColumns` → `DropAllEmptyRows` → `CleanupOrphanRows`），顺序固定；因为每步都会改变维度，所以 `item.Grid` 与 `item.Rows` 必须在三步**之后**重建。

**源码事实：** `ParseTableHTML` 有三处结构特例：`<tfoot>` 被单独收集并**在每个 `table` 边界**追加（不只是片段末尾），因为「a table's footer goes below that table's own body」；`<tr>` 的内部**不递归**（嵌套 `table` 折叠进外层单元格文本）；`<caption>` 被单独取出且不再下降。

**源码事实：** `<tfoot>` 分离收集的理由是「HTML 4 required it before `<tbody>`, so its rows are collected apart and placed last; read in source order, a `"Total"` row came before the data it sums」——直接按源码顺序读会让合计行排在数据前。

**源码事实：** 只要一行里有任一 `<th>`，整行被标为表头行（`anyTh`）；`BuildTableData` 的 `headerCount` 是**前导连续表头段**，且**没有表头时兜到 1**（有行即把第一行当表头）。

**源码事实：** 跨度有**两道防护**：`clampSpan` 到 `[1, maxSpan]` 防「恶意大值驱动 `buildGrid` 宽度预扫描的 GB 级分配」；建网格后再按实际网格回剪（「so every claimed position is backed」）。

**源码事实：** `buildGrid` 两遍扫描，而**预扫描不是纯测量**——它顺带填满 `covered` 供主循环复用；宽度必须模拟全部放置规则（含祖先 rowspan），否则「an ancestor rowspan would push a later row's cells past the width and its trailing real cells would be silently dropped by the column guard」（这是从旧实现继承的具体缺陷）。

**源码事实：** `covered[r][col]` 用 nil map 的下标读（Go 允许），省掉初始化判断；`nextCoveredCol` 的 `else` 分支存在的理由是「本行实体格用完但祖先 rowspan 仍占更右列，直接 break 会漏掉它们」。

**源码事实：** 第二遍的容量按 `realCols + len(covered[r])` 精确计算，而非按 `width`——因为「a wide row elsewhere must not make every short row allocate megabytes of unused backing capacity」。

**源码事实：** 「行**不**填充到统一宽度」是兼容性约束：「so `CellTexts()` reproduces the legacy row-major Rows exactly」；只有合并格产生 `Covered` 标记，「a real gap is simply a shorter row」。

**源码事实：** 尾部裁剪只删「完全空」的行（`len == 0`），而「只有 `Covered` 标记的行必须保留」——否则某原位的 `RowSpan` 会超出实际行数。

**源码事实：** `RenderTableHTML` 手工拼字符串（不用模板），属性引号/空格/换行位置全写死——因为「the chunk's Text with it, and the chunk id is a hash of that rendered text」，输出必须逐字节稳定；表头判据是「前 `HeaderRows` 行」而非「含 `th` 的行」，与 `BuildTableData` 的 `headerCount` 完全对应，所以往返一致。

**源码事实：** `colspan`/`rowspan` 只在 `> 1` 时写出；`<table>` 后无换行（无 caption 时是 `<table>\n`），`</table>` 后有换行——这些细节同样必须稳定。

**源码事实：** `ParseTableHTML` 入口对「有 `<tr>` 但无 `<table>`」的输入补包裹（否则 `html.Parse` 不会建 table 边界）；它对整个字符串 `ToLower` 做一次拷贝（大表格的可观开销）。

**源码事实：** `ConstructTable` 的 `RenderVH` 分支全部是 EE-only（「no OSS-built TableItem sets RenderVH」），OSS 永远走 `constructTableCore`；三个 EE 子分支优先级为 `GridUnified` > `VHPageSpans` > 默认（含 `BoxToCropSpace` 坐标转换）。

**源码事实：** `gridToStreamCells` 先 `dropEmptyColumns`，因为「phantom/edge bands the EE V/H renderer must not emit」——`RenderTableHTML` 能容忍 ragged 与空列，V/H 渲染器不能。

**源码事实：** `StripCaptionFromCells` 在 `constructTableCore` 第一句调用，注释称之为「defense-in-depth（`fillCellTextFromBoxes` may include caption text that doesn't match `isCaptionBox` patterns）」，且「清空文本而不删格」以保住几何。

**源码事实：** `StripCaptionFromCells` 的末尾注释描述了一个「第二趟：若首行全是数字/编号则清掉」的处理，**而代码里只有一趟**——注释与代码不符。

**架构推论：** 「HTML ↔ 结构」的双向转换在这里不是可选能力，而是链路的必需形态。原因是表格被两种消费者需要不同的形状：**LLM 需要 HTML**（它能读标记结构，且表格渲染成 HTML 比 CSV 更易懂），**而分块器需要单元格数组**（要按行读、要算位置、要拼块文本）。所以只能在两者之间往返，而往返的**稳定性**（同一张表两次往返得到同一串字节）就成了硬约束。

**判据是「这个表示是否被用作标识的一部分」**：一旦渲染文本参与哈希（块 id），格式稳定性就从「美观」升级为「正确性」。

**架构推论：** 「`Covered` 标记指回原位」这个模型比「填充空格子」强得多。填充会丢失「这个位置是被吞并的」这一信息，于是下游无法区分「空单元格」与「被合并覆盖」——而这个区别直接影响：
- 是否该渲染 `<td></td>`
- 是否该计入列数
- 表格转文本时该跳过还是输出空

**判据是「语义差异是否需要保留」**：需要则用标记，不需要则填充更简单。

**架构推论：** 「行不填充到统一宽度」与「`Covered` 标记」是两个相反方向的规整：前者保留原始的不规则，后者补齐被吞并的位置。**这看似矛盾，实则分工明确**：`raw` 的缺口是「这行确实少几个格」（真实数据形态），`Covered` 是「这几格已被别的格占据」（结构事实）。**判据是「这个短是信息还是缺陷」**：真实缺口是信息（该保留），合并吞并是结构（该补齐）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/deepdoc/parser/pdf/table/table_construct.go` | `ConstructTable` | 渲染入口（EE 分支 + OSS 核心） |
| `internal/deepdoc/parser/pdf/table/table_construct.go` | `constructTableCore` | 三步清理 + 算跨度 + 渲染 |
| `internal/deepdoc/parser/pdf/table/table_construct.go` | `StripCaptionFromCells` | 剥离 caption 类文本（清空不删格） |
| `internal/deepdoc/parser/pdf/table/table_construct.go` | `gridToStreamCells` / `dropEmptyColumns` | EE V/H 渲染器的输入规整 |
| `internal/deepdoc/parser/pdf/table/table_construct.go` | `columnIsEmpty` | 整列为空判定 |
| `internal/deepdoc/parser/pdf/table/table_construct.go` | `BoxHeaderSet` | 按文本框的 `H` 标注定表头行 |
| `internal/deepdoc/parser/pdf/table/table_construct.go` | `FillCellTextFromAnnotations` | 按 `R`/`C` 标注填格文本 |
| `internal/deepdoc/parser/pdf/table/table_construct.go` | `CleanupOrphanColumns` | 孤立列合并/删除 |
| `internal/deepdoc/parser/pdf/table/table_construct.go` | `CleanupOrphanRows` | 孤立行合并/删除 |
| `internal/deepdoc/parser/pdf/table/table_construct.go` | `DropAllEmptyRows` | 删完全空行 |
| `internal/deepdoc/parser/pdf/table/table_construct.go` | `mergeOrphanCell` / `mergeColumn` / `removeColumn` | 合并原语 |
| `internal/deepdoc/parser/pdf/table/table_construct.go` | `countNonEmptyCells` / `checkAdjacentColumns` / `calculateMergeDistance` | 孤立列判据 |
| `internal/deepdoc/parser/pdf/table/table_construct.go` | `isDataSourceBox` / `dataSourceRe` | 数据来源说明行的识别 |
| `internal/deepdoc/parser/pdf/table/table_construct.go` | `tableRegionBox` / `mergedTablePages` | 表格区域与跨页页号 |
| `internal/parser/tableutil/table_html.go` | `ParseTableHTML` | HTML → `TableData`（三处结构特例） |
| `internal/parser/tableutil/table_html.go` | `BuildTableData` | 表头行数 + 跨度 clamp + 建网格 |
| `internal/parser/tableutil/table_html.go` | `buildGrid` | 两遍扫描（预扫描填 `covered` + 精确容量） |
| `internal/parser/tableutil/table_html.go` | `RenderTableHTML` | 逐字节稳定的 HTML 输出 |
| `internal/parser/tableutil/table_html.go` | `cellText` | 单元格可见文本（`<br>` → 换行、惰性元素跳过） |
| `internal/parser/tableutil/table_html.go` | `CellInput` / `origin` | 输入格与「覆盖位指回的原位」 |
| `internal/parser/tableutil/table_html.go` | `clampSpan` / `spanOf` | 跨度规整 |
| `internal/parser/tableutil/table_html.go` | `nextCoveredCol` | 跳到下一个被覆盖列 |
| `internal/parser/tableutil/table_html.go` | `attrInt` | 属性解析（含上限截断） |
| `internal/parser/tableutil/table_html.go` | `isInertElement` / `isHTMLElement` | 惰性元素与命名空间判定 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 文档：表格与结构化输出](https://ragflow.io/docs/dev/configure_knowledge_base)
- [论文：表格结构识别（TSR）综述](https://arxiv.org/abs/2110.00061)
- [HTML 标准：表格模型与 tfoot 位置](https://html.spec.whatwg.org/multipage/tables.html)

## 与主核 / AI 的关系

「`Covered` 标记指回原位」与主核（多模态数据库内核）的**行头空位（null bitmap）**是同一个建模问题：一个位置上「没有值」可能意味着三件不同的事——「值是 NULL」「这个位置被别的东西占了」「这个位置不存在」。主核用 `null bitmap` + `infomask` 区分前两者，用「元组长度」区分第三者。

**这里的三个状态正好对应**：`Covered`（被别的东西占了）、`Text == ""`（值是空）、「行更短」（位置不存在）。**这个三态区分是必需的**，因为它们的下游行为完全不同：`Covered` 跳过不渲染，空值渲染空 `<td>`，短行没有 `<td>` 可渲染。

第二处对照是「渲染输出逐字节稳定」与主核的**预处理语句缓存键**。主核缓存的是「SQL 字符串的精确文本」——所以 `SELECT 1` 与 `SELECT  1`（两个空格）是不同的缓存键。两者都**把文本当标识**，所以文本的每个字节都必须确定。**判据是「文本是否参与标识」**：参与则必须规范化到唯一形式，且该形式的每个细节（空格、引号、换行）都要写死。

第三处是 `buildGrid` 的「预扫描顺带做实事」与主核的**计划阶段构建执行状态**。主核的 `ExecutorStart` 在计划阶段就建好 `PlanState` 树，执行阶段只是遍历它——同样是一次「测量时顺带产出后续需要的结构」。**判据是「这次遍历能否同时产出副产品」**：能则省一趟，但代价是两处逻辑必须严格一致（注释里那句「The placement here mirrors the main loop below exactly」就是在维护这个一致性）。

对 RAG 工程的实际意义在于：表格是 RAG 里「结构信息密度最高但最容易丢」的内容。这份实现给出的三条经验可直接用：**表格必须双向可转换且输出逐字节稳定**（一旦渲染文本参与哈希，格式就成了契约）、**被合并吞并的位置要用显式标记而不是空值**（三态差异必须有表示）、以及**行不填充到统一宽度**（真实的缺口是信息，补齐会破坏与旧视图的等价性）。
