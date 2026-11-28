---
project: ragflow
feature: pdf-layout
title: PDF 版面重建
page_type: feature
schema_version: 1
refs:
  - path: internal/deepdoc/parser/pdf/layout/layout.go
    symbols: [TextMerge, FinalReadingOrderMerge, FilterWatermarkBoxes]
  - path: internal/deepdoc/parser/pdf/layout/boxes_sections.go
    symbols: [BoxesToSections, SectionsToJSON]
  - path: internal/deepdoc/parser/pdf/layout/dehyphen.go
    symbols: [Dehyphenate]
  - path: internal/deepdoc/parser/pdf/layout/combined_column.go
    symbols: [AssignColumn]
---

# PDF 版面重建

## 一句话职责

把 PDF 里一堆**互不相关、无阅读顺序**的文本框，还原成「按阅读顺序排列的段落」：先判栏数并给每个框分配栏号，再按栏内自上而下排序，然后合并同一行（横向）与相邻行（纵向），同时清掉水印、重复检测的碎片、断词连字符，最后输出带位置标签的章节。

## 机制是什么

PDF 的 KV 事实是：**它不存阅读顺序**。它存的是一串「在坐标 (x, y) 处用某字体画某个字形」的指令。所以「段落」「行」「栏」这些概念在 PDF 里**完全不存在**——它们全是从几何坐标反推出来的。

于是版面重建要解决三个层次的问题，而它们的难度递增：

| 层次 | 问题 | 判据 |
|---|---|---|
| 1. 栏 | 这一页有几栏？ | 垂直白带（gutter）的位置与宽度 |
| 2. 行 | 哪些框在同一行？ | 垂直中心接近 + 高度相近 |
| 3. 阅读顺序 | 栏内自上而下，栏间从左到右 | 栏号 → top → x0 |

**最困难的是第 1 层**，因为「栏」与「表格列」在几何上完全一样（都是并排的窄块），而「栏」与「缩进」也容易混（都是 x0 不等）。这份实现为此付出了本页最大的代码量（`combined_column.go` 1005 行）。

而它的解法是一个**分层投票**：

1. **gap 检测**（垂直投影直方图上找低覆盖带）给出一个 k。
2. 若 `k >= 2` 但**列很窄**（`< tableMaxColFrac` = 页宽的 22%）→ 判定为**表格单元**，返回 1（单块）。
3. 若 `k == 2` → **不信任**（「It is often a fake gutter from indentation/line-width variation」），交给下一层。
4. 若 `k >= 3` 且列宽足够 → 信任，按 KMeans(k) 分区。
5. **平衡门**（balance gate）：强制 k=2 的 KMeans，要求两个簇各含 ≥ `minModeFrac` 的行且间隔 ≥ `minSepFrac * width`。平衡才算真两栏，不平衡（KMeans 常见假分裂）→ 仍是 1。

**第 3 步「k==2 不可信」是本页最有意思的判断**：两栏是最常见的版面，而「假 gutter」也最常见于两栏——所以恰好是 k=2 时无法靠 gap 判断，必须靠「行分布的平衡性」这个更强的信号。

**然后还有一层文档级修正**：若**多数页**判为 1 栏，则**整篇强制 1 栏**。理由是「indent-heavy single-column pages」（如刑法的目录与条文页）会被逐页检测误判成 2-3 栏，而它们的共同特征是「行尾对齐但行首参差」。

## 怎么实现的

### `AssignColumn`：多数票优先于逐页结果

```go
perPage := make([]pageColumn, len(sortedPages))
colCount := map[int]int{}
var firstK []int
for pi, pg := range sortedPages {
	k, cents := detectColumnCount(boxes, pageGroups[pg])
	perPage[pi] = pageColumn{k: k, cents: cents}
	if colCount[k] == 0 {
		firstK = append(firstK, k)
	}
	colCount[k]++
}
modeK, modeN := firstK[0], colCount[firstK[0]]
for _, k := range firstK {
	if colCount[k] > modeN {
		modeK, modeN = k, colCount[k]
	}
}
if modeK == 1 {
	for i := range result {
		result[i].ColID = 0
	}
	return result
}
```

六处：

1. **`firstK` 只记录「首次出现的 k」** —— 这是为了**让平票时的结果确定**。而注释说得很清楚：

> Deterministic majority matching Python's `_assign_column global_cols` (`Counter(page_cols.values()).most_common(1)`): the most frequent per-page column count wins; **on a tie the FIRST page's count wins** (Python keeps dict insertion order). Iterate in page order so the result never depends on Go map iteration order.

   **「按页码顺序迭代」是必需的**，否则 Go 的 map 迭代顺序随机会让平票结果不稳定。**这是一处为「确定性」而做的具体设计。**

2. **`modeK == 1` 时整篇 `ColID = 0`** —— 所以单栏文档的阅读顺序就是「按页 + top + x0」。

3. **多栏多数时保留逐页结果** —— 注释：「A multi-column majority keeps the per-page detector result (those documents are genuinely multi-column throughout)」。

   **这里有一个隐含的不对称**：单栏多数时**覆盖**逐页结果，多栏多数时**信任**逐页结果。理由是「单栏是安全的默认，误判多栏会打乱顺序；而多栏文档的逐页差异通常是合理的（某些页是整页图）」。

4. **`detectColumnCount` 返回 `(k, cents)` 并被缓存** —— 所以第二次循环不必重算。**这是一处必要的优化**（检测里有 KMeans）。

5. **`result` 先 `copy(result, boxes)` 再改 `ColID`** —— 所以是**值拷贝**（`pdf.TextBox` 是结构体）。而 `boxes` 本身不被修改。

6. **`assignColIDs(boxes, result, indices, cached.k, cached.cents)` 传两个切片** —— 用 `boxes` 读原文、写 `result`。**这种「读源写目标」的双切片模式避免了原地修改的别名问题。**

**而 `tableMaxColFrac` 与 `minColLineFrac` 这两个常量的注释里带着实测数据：**

```go
// minColLineFrac: a column holding fewer than this fraction of the page's
// lines (or zero lines) is not a real reading column — it is a spurious
// gutter sliver (an indented block, a stray caption, an empty kmeans
// centroid). Drop it so the detector does not over-split.
//
// The threshold is set with margin below the smallest genuine column ratio
// observed on the 70-page labeled corpus: the sparsest real double's minority
// column is ~17.7% of lines, and the only real triple's columns are each
// >=22%. 12% prunes genuine outliers (e.g. a 4-line footnote, 7.3%) without
// touching those.
const minColLineFrac = 0.12
```

**「70 页标注语料上实测：最稀疏的真两栏其少数栏占 17.7%，唯一的三栏各 ≥22%；12% 能剪掉真实离群（4 行脚注 7.3%）而不动它们」** —— 这是一处**带测量依据的阈值**，而且它明确说了「留了余量」。

**这类注释在版面算法里价值极高**，因为阈值调参没有理论最优，只能靠语料。

**还有三个「防灾难」的常量：**

```go
const maxColumnCount = 4      // 防止「单页被报成 7 栏以上」
const maxPageExtent = 14400.0 // 单个行坐标的合理上限（镜像 pdf-inspector 的守卫）
const maxBins = 65536         // 直方图分配的硬上限（防止畸形页触发 OOM 级分配）
```

**三处**：

1. **`maxColumnCount = 4`** —— 注释说「Gap voting can over-split a single page into many spurious gutters (e.g. first-line indentation), so we bound the count to the old detector's best-k cap of `min(4, n)`」。**所以这是一个「旧实现已经有的上限」，不是新发明的。**

2. **`maxPageExtent = 14400.0`** —— 「A line wider than this is treated as a malformed coordinate（mirrors pdf-inspector's `MAX_PAGE_EXTENT=14400` guard）」。**14400 是 200 英寸 × 72 pt，即一个 PDF 页的物理上限。**

3. **`maxBins = 65536`** —— 「so a malformed (ballooned) page extent cannot trigger an OOM-scale allocation」。**一个被膨胀的页宽 × 细 bin 会让直方图数组爆掉。**

   而 `columnHistogramBins` 的处理是**加宽 bin** 而不是截断宽度 —— 所以「畸形页」仍能被检测，只是分辨率降低。**这比直接拒绝更好。**

### `FilterWatermarkBoxes`：用「同页重复」识别水印

```go
func FilterWatermarkBoxes(boxes []pdf.TextBox) []pdf.TextBox {
	type key struct {
		page int
		text string
	}
	counts := make(map[key]int, len(boxes))
	for _, b := range boxes {
		if !isSingleAsciiAlnum(b.Text) {
			continue
		}
		counts[key{b.PageNumber, b.Text}]++
	}
	promoted := make(map[key]struct{}, len(counts))
	for k, n := range counts {
		if n >= watermarkBoxesMinOccurrences {
			promoted[k] = struct{}{}
		}
	}
	if len(promoted) == 0 {
		return boxes
	}
	out := make([]pdf.TextBox, 0, len(boxes))
	for _, b := range boxes {
		if isSingleAsciiAlnum(b.Text) {
			if _, drop := promoted[key{b.PageNumber, b.Text}]; drop {
				continue
			}
		}
		out = append(out, b)
	}
	return out
}
```

**五处**：

1. **只针对「单个 ASCII 字母或数字」的框** —— 而 `isSingleAsciiAlnum` 的实现显式判了 `len(s) == 1` 与三个 ASCII 区间。

   而注释解释了为什么不包括 CJK：

> CJK is unconstrained: a single box's bytes may be ≥1 even when the rune count is 1, so `len(box.Text) == 1` already excludes CJK fonts (which are typically 3 bytes per glyph). A box that 姓名 — the canonical Chinese name field — survives verbatim even when the rest of the document is contaminated.

   **「`len == 1` 天然排除了 CJK（3 字节/字），所以中文水印不会被误删」** —— 这是一处**用字节长度作为语言守卫**的巧妙之处，同时也是它的局限（中文水印清不掉）。

2. **计数键是 `(页号, 文本)`** —— **按页分组而不是全文档**。理由：水印在每一页重复（每页出现 10+ 次），但全文档层面会有更多误报（同一个字母在不同页出现很正常）。

3. **阈值 `watermarkBoxesMinOccurrences = 4`** —— 注释里给了依据：

> Legitimate short content ("1.", "Q.", "A.") rarely appears more than once per page outside of dense content (table rows, list items), and even there the same label rarely reaches this threshold. **A watermark tile places the same glyph at 10+ x-intercepts across the page**, so the threshold is set conservatively above that floor to keep false-positive rate negligible.

   **「水印铺贴会在页面上把同一字形放在 10+ 个横向位置」** —— 所以 4 是一个保守下限。

   **但这意味着「表格里同一页出现 4 次 `1`」会被误删。** 而注释承认了 dense content 的风险，判断是「even there the same label rarely reaches this threshold」。

4. **两趟扫描** —— 先计数、再删除。**而且两趟都必须重新判 `isSingleAsciiAlnum`**，因为第二趟要遍历全部框。

5. **`promoted` 为空时早退返回原切片** —— 避免为「没有水印」的文档分配新切片。

   **注意早退返回的是 `boxes`（同一个切片），而不是拷贝** —— 所以「没有水印」时调用方拿到的是**同一个底层数组**。若调用方后续修改，会影响上游持有的切片。**这是一处别名风险**，但与 `DedupIdenticalText` 等函数的写法（也返回原切片）一致。

### `TextMerge`：横向合并同一行

```go
if math.Abs(util.BoxYDis(cur, nxt)) < mh/3 {
	cur.X1 = nxt.X1
	cur.Top = (cur.Top + nxt.Top) / 2
	cur.Bottom = (cur.Bottom + nxt.Bottom) / 2
	cur.Text += nxt.Text
	i++
}
```

四处：

1. **判据是「垂直中心距离 < 中位行高的 1/3」** —— 所以是**相对阈值**，而不是固定 pt。这让不同字号的文档都能正确处理。

2. **`cur.X1 = nxt.X1`（不是取 max）** —— 因为已经在排序的序列上，`nxt` 在右边，所以直接覆盖。**但若 `nxt.X1 < cur.X1`（乱序），会缩小范围。**

3. **`Top`/`Bottom` 取平均而不是并集** —— 所以合并后的框高度是两框高度的平均。**注释没解释，但这个选择的后果是「合并后的框垂直位置居中于两框之间」。**

4. **`cur.Text += nxt.Text`（**不加空格**）** —— 而这是关键：横向相邻的框通常是同一行的不同字距，所以不加空格是对的（英文单词之间本来就由框间空白表征）。

   **但这也意味着「两个框是同一行的两个词」时会被拼成 `word1word2`** —— 若它们之间本来该有空格。而实际 PDF 里同行不同框之间通常有空格字符或者靠位置推断，所以这是一个已知的近似。

**而 `TextMerge` 的跳过条件里有一处「非对称默认值」的注释：**

```go
// Python: b.get("layoutno", "0") != b_.get("layoutno", "1") —
// asymmetric defaults mean empty/missing layoutno never merge horizontally.
if cur.LayoutNo != nxt.LayoutNo || cur.LayoutNo == "" || nxt.LayoutNo == "" ||
	cur.LayoutType == pdf.LayoutTypeTable || cur.LayoutType == pdf.LayoutTypeFigure || cur.LayoutType == pdf.LayoutTypeEquation {
	break
}
```

**「Python 版用 `b.get("layoutno", "0")` 与 `b_.get("layoutno", "1")` 两个不同的默认值，于是「两者都缺失」时 `"0" != "1"` 为真从而不合并」** —— 这是一个**用默认值差异编码「两个空值不等于」**的技巧，而 Go 版用 `cur.LayoutNo == "" || nxt.LayoutNo == ""` 显式表达同一语义。**后者清楚得多。**

**而表格/图/公式一律不横向合并** —— 因为它们内部的几何结构有意义（表格的行列、公式的上下标），合并会破坏它。

### `NaiveVerticalMerge`：英文页的特殊处理

```go
mh := medianHeights[pg]
if pageEnglish[pg] {
	// Python: for is_english documents chars are cleared so
	// mean_height becomes 0 and _naive_vertical_merge skips every
	// pair (gap > 0). Mirror that for English pages DIRECTLY — do not
	// fall back to the (positive) char-derived median height, or real
	// English pages would still merge and 'linexxx' rows (eval_*)
	// concatenate into one giant line. The old guard `if mh <= 0`
	// never fired for real pages because their char-derived median
	// height is always positive.
	mh = 0
} else if mh <= 0 {
	mh = util.MedianHeight(bxs)
}
```

**四处**：

1. **英文页强制 `mh = 0`** —— 注释说明了原委：Python 版清空了 char 信息所以 `mean_height` 变 0，于是纵向合并不发生。**Go 版直接把这个后果编码成 `mh = 0`。**

2. **为什么不该纵向合并英文页** —— 「or real English pages would still merge and `'linexxx'` rows (`eval_*`) concatenate into one giant line」。**英文文档的相邻行本来就该是一个段落**，而纵向合并会把它变成一个巨大的单行。**所以「纵向合并」只对中文（无空格、依赖行内换行）有意义。**

   这是一处**语言相关的版面假设**：中文排版的行是「断行」而非「段落」，所以纵向合并才是正确的段落重建；英文排版的行是「折行」，纵向合并会破坏段落结构。

3. **`mh` 参与判据的方式是「gap > mh」** —— 所以 `mh = 0` 时任何正 gap 都不合并（除了恰好 gap == 0 的完全重叠框）。注释说「skips every pair (gap > 0)」。

4. **`else if mh <= 0` 才回退到实测中位高** —— 所以「未提供 medianHeights」时用 `util.MedianHeight(bxs)`。

**而宽度 `mw` 的回退值是 8：**

```go
mw := medianWidths[pg]
if mw <= 0 {
	mw = 8 // 回退值：有字符信息时取 np.median([...])，否则取 8
}
```

**8 pt 是一个「字符宽度」的量级** —— 所以它是「没有字符信息时的最小可辨宽度」。

### `FinalReadingOrderMerge`：四键排序，而它是阅读顺序的唯一决定点

```go
sort.Slice(boxes, func(i, j int) bool {
	bi, bj := boxes[i], boxes[j]
	if bi.PageNumber != bj.PageNumber { return bi.PageNumber < bj.PageNumber }
	if bi.ColID != bj.ColID { return bi.ColID < bj.ColID }
	if bi.Top != bj.Top { return bi.Top < bj.Top }
	return bi.X0 < bj.X0
})
```

**四处**：

1. **四个键的优先级就是阅读顺序的语义**：页 → 栏 → 垂直位置 → 水平位置。**这就是「栏内自上而下，栏间从左到右」的全部实现。**

2. **`sort.Slice` 而不是 `SliceStable`** —— 因为四键组合在浮点坐标下几乎不会相等，所以稳定性无关。但**浮点相等是可能的**（完全相同坐标的两个框），此时顺序不确定。

   **这是一处潜在的非确定性**：`Top` 与 `X0` 都是 `float64`，两个框恰好同坐标时顺序随机。而注释没提这个。

3. **它 `sort` 的是 `boxes` 本身（原地）** —— 与其他函数「返回新切片」的写法不同。**这是一处不一致：调用方必须知道这个函数会修改入参。**

4. **按 `ColID` 而不是按「栏的 x 中心」** —— 所以栏的物理位置与阅读顺序无关。**这意味着「右栏在左」的版面（如某些古籍竖排）不会被正确处理。**

### `Dehyphenate`：三个条件都必须同时成立

```go
func canDehyphenate(prev, curr pdf.TextBox) bool {
	if prev.PageNumber != curr.PageNumber { return false }
	if prev.ColID != curr.ColID { return false }
	if !endsWithBrokenHyphen(prev.Text) { return false }
	if !startsWithWordContinuation(curr.Text) { return false }
	return linesAreVerticallyAdjacent(prev, curr)
}
```

**五个条件**，每个都在排除一种误判：

1. **同页** —— 跨页的断词**不合并**，因为跨页会破坏 `Positions` 的连续性（且书上跨页断词通常有页码干扰）。
2. **同栏** —— 跨栏的「上一行末尾有连字符」与「下一栏开头是小写」不是同一个词。
3. **`endsWithBrokenHyphen`** —— 而它要求「连字符前至少有一个字母」（`unicode.IsLetter(rb)`）。所以「孤立的 `-`」不算断词。
4. **`startsWithWordContinuation`** —— 后一框必须以**小写字母**开头。注释：「A new sentence starts with an uppercase letter, so a lowercase start strongly implies the previous line's hyphen was a real break rather than a dash」。

   **这是一处语言相关的判据**：中文没有大小写，所以这条对中文永不成立——**即中文断词不会被 `Dehyphenate` 处理**。这与「中文排版不断词」的事实一致。

5. **`linesAreVerticallyAdjacent`** —— `gap <= lineH * 2.5`，其中 `lineH` 取两框高的**较大者**。

   **`2.5` 倍行高是一个「容忍双倍行距」的值** —— 所以双倍行距的正文仍能合并，而「隔了一个空行」的（约 3 倍）不会。

**而 `isBrokenHyphenRune` 收了 5 个码位：**

```go
switch r {
case '\uFFFD',  // replacement char (unresolved glyph)
	'\u00AD',   // soft hyphen
	'-',        // hyphen-minus
	'\u2010',   // hyphen
	'\u2011':   // non-breaking hyphen
	return true
}
```

**注释说明了真实成因**：「Some PDFs (e.g. Elsevier journals set in CharisSIL) encode the line-break hyphen with a glyph whose Unicode mapping the extractor fails to resolve, so the boundary shows up as U+FFFD or U+00AD」。

**「字体里的连字符字形无法映射，提取出来变成替换字符」** —— 这是一个具体的、由字体子集化导致的缺陷，而它的修复必须从「识别这五类码位」开始。

**而 `cleanIntraWordHyphens` 处理「同一个框内部」的残留：**

```go
func cleanIntraWordHyphens(s string) string {
	if !strings.ContainsRune(s, '\uFFFD') && !strings.ContainsRune(s, '\u00AD') && !strings.ContainsRune(s, '-') {
		return s
	}
	s = garbageHyphenRE.ReplaceAllString(s, "")
	s = lineBreakHyphenRE.ReplaceAllString(s, "$1$2")
	return s
}
```

**两处**：

1. **先删垃圾（`garbageHyphenRE`），再拼真连字符（`lineBreakHyphenRE`）** —— 顺序不能反：若先拼真连字符，垃圾码位还在原位，会干扰匹配。

2. **`lineBreakHyphenRE` 的关键是「连字符后必须有空格」**：

```go
var lineBreakHyphenRE = regexp.MustCompile(`(\p{L})[-\u2010\u2011\u2043]\s(\p{L})`)
```

**注释**：「It fires only when a space follows the hyphen, so in-word hyphens without a space ("lithium-ion", "well-known") survive」。

**「换成 `$1$2` 即只保留两个字母、丢掉连字符与空格」** —— 所以 `re- covery` → `recovery`，而 `lithium-ion` 不受影响。

**这条正则的判据是「空格」而不是「小写」** —— 所以 `Well- Known` 也会被拼成 `WellKnown`。**这是一处比 `canDehyphenate` 宽松的规则**：框内的断词不需要小写判据，因为「连字符 + 空格 + 字母」在正文里几乎只出现在断词处。

### `BoxesToSections`：三条位置来源，四道降级

```go
positions := b.Positions
if len(positions) > 0 {
	first, last := positions[0], positions[len(positions)-1]
	if len(first.PageNumbers) > 0 && len(last.PageNumbers) > 0 {
		fromPage := first.PageNumbers[0]
		toPage := last.PageNumbers[len(last.PageNumbers)-1]
		if fromPage == toPage {
			posTag = util.FormatPositionTag(fromPage, first.Left, last.Right, first.Top, last.Bottom)
		} else {
			posTag = util.FormatPositionTagRange(fromPage, toPage, ...)
		}
	}
} else if len(b.Pages) > 0 {
	...
} else {
	toPage, resolved := ResolvePageSpan(b.PageNumber, b.Bottom, pageHeights)
	...
}
```

**四处**：

1. **三条位置来源的优先级**：`positions`（逐页几何）> `b.Pages`（显式跨页跨度）> `pageHeights` 推断。**越具体的来源越优先。**

   而注释解释了第二条：「Box carries an explicit page span (e.g. a cross-page merged table). Use it directly instead of inferring from geometry, so the section records every page the box occupies」。

2. **单页必须用 `@@N` 形式而不是 `@@N-N`** —— 注释：「A one-page section must keep the `@@N` form: `@@N-N` round-trips through `ExtractPositions` as two identical page numbers」。**这是一处往返一致性要求**（与 c01 的「投影 1:1」同一纪律）。

3. **`ResolvePageSpan` 处理「框底坐标超出页高」** —— 即一个框跨到了下一页。而它按「减掉当前页高、移到下一页、剩余量作为新 bottom」的循环算。

   而它的兜底是：「Unknown or invalid next page height — extend by the last known height once and stop（Python: `_line_tag` while-loop break path）」。

   **「延一页然后停」** —— 所以一个坐标异常巨大的框最多跨一页，不会横扫整个文档。

4. **`if len(positions) == 0` 时才合成一个 `Position`** —— 所以「已有 positions」时不覆盖。**这是一处「不替换已有更精确信息」的守卫。**

**而 `SectionsToJSON` 有一处表格特例：**

```go
if j == 0 && s.LayoutType == pdf.LayoutTypeTable && len(pageNumbers) > 1 {
	// A merged table's primary bounds belong to its first page.
	pageNumbers = pageNumbers[:1]
}
```

**「合并表格的首个 bounds 只属于第一页」** —— 因为表格的 `pages` 数组含全部跨页，但第一段的位置只描述第一页的范围。**若全带上去，下游会把整个跨页范围当成第一段的范围。**

## 关键取舍

**源码事实：** PDF 不存阅读顺序，所以「栏 / 行 / 段落」全是从几何坐标反推。三层的判据分别是「垂直白带位置与宽度」/「垂直中心距离 + 高度」/「栏号 → top → x0」。

**源码事实：** 栏检测是**分层投票**：gap 检测给 k → 若列宽 `< tableMaxColFrac`（0.22 页宽）判为表格单元返回 1 → 若 `k == 2` **不信任**（「often a fake gutter from indentation/line-width variation」）交给平衡门 → 若 `k >= 3` 且列宽足够则信任 → 平衡门要求两簇各含 ≥ `minModeFrac` 行且间隔 ≥ `minSepFrac * width`。

**源码事实：** 文档级修正：若**多数页**判为 1 栏则整篇强制 `ColID = 0`（单栏），多栏多数时保留逐页结果。单栏多数时**覆盖**、多栏多数时**信任**——不对称是有理由的（单栏是安全默认，误判多栏会打乱顺序）。

**源码事实：** 多数票的实现为了确定性而按页码顺序迭代并记录 `firstK`，因为「on a tie the FIRST page's count wins (Python keeps dict insertion order)」；注释明确「Iterate in page order so the result never depends on Go map iteration order」。

**源码事实：** `minColLineFrac = 0.12` 的取值有实测依据：70 页标注语料上「最稀疏的真两栏其少数栏占 17.7%，唯一的三栏各 ≥22%，12% 能剪掉真实离群（4 行脚注 7.3%）」。

**源码事实：** 三个防灾难常量：`maxColumnCount = 4`（旧实现已有的上限）、`maxPageExtent = 14400`（200 英寸 × 72pt，镜像 pdf-inspector 的守卫）、`maxBins = 65536`（防畸形页触发 OOM 级分配）。而 `columnHistogramBins` 对膨胀的页宽是**加宽 bin**而不是截断宽度——所以畸形页仍可检测，只是分辨率降低。

**源码事实：** `FilterWatermarkBoxes` 只针对**单个 ASCII 字母或数字**的框（`len(s) == 1` 天然排除 3 字节/字的 CJK），按 `(页号, 文本)` **按页**计数，阈值 4（注释依据：「A watermark tile places the same glyph at 10+ x-intercepts across the page」）。所以**中文水印清不掉**，而「表格里同一页出现 4 次 `1`」会被误删。

**源码事实：** `FilterWatermarkBoxes` 在无水印时**返回原切片**（不是拷贝），与其他去重函数的写法一致，构成别名风险。

**源码事实：** `TextMerge` 的判据是「垂直中心距离 < 中位行高的 1/3」（相对阈值，与字号无关）；合并时 `X1` 直接覆盖（不取 max）、`Top/Bottom` 取平均、`Text` **不加空格拼接**。

**源码事实：** `TextMerge` 显式表达 Python 版「非对称默认值」的语义（Python 用 `b.get("layoutno","0")` vs `b.get("layoutno","1")` 让「两者都缺失」不合并，Go 版用 `cur.LayoutNo == "" || nxt.LayoutNo == ""`）；表格/图/公式一律不横向合并。

**源码事实：** `NaiveVerticalMerge` 对英文页**强制 `mh = 0`**，注释解释「Mirror that for English pages DIRECTLY — do not fall back to the (positive) char-derived median height, or real English pages would still merge and `'linexxx'` rows concatenate into one giant line」。**这是语言相关的版面假设**：中文的行是「断行」（纵向合并才能重建段落），英文的行是「折行」（纵向合并会破坏段落）。

**源码事实：** `FinalReadingOrderMerge` 用四键 `(PageNumber, ColID, Top, X0)` 排序——**这是阅读顺序的唯一决定点**；它**原地 sort 入参**（与全页其他函数「返回新切片」的写法不一致）；用 `sort.Slice` 而非 `SliceStable`，所以两框同坐标时顺序不确定。

**源码事实：** `canDehyphenate` 五个条件全部必须成立：同页、同栏、`endsWithBrokenHyphen`（连字符前至少一个字母，所以孤立 `-` 不算）、`startsWithWordContinuation`（**必须小写字母开头**）、`linesAreVerticallyAdjacent`（`gap <= lineH * 2.5`，容忍双倍行距）。

**源码事实：** 「小写开头」这条对中文永不成立（中文无大小写），所以**中文断词不被 `Dehyphenate` 处理**——与「中文排版不断词」一致。

**源码事实：** `isBrokenHyphenRune` 收 5 个码位（`U+FFFD` 替换字符 / `U+00AD` 软连字符 / `-` / `U+2010` / `U+2011`），成因注释给了具体来源：「Elsevier journals set in CharisSIL encode the line-break hyphen with a glyph whose Unicode mapping the extractor fails to resolve」。

**源码事实：** `cleanIntraWordHyphens` 先删垃圾再拼真连字符（顺序不能反）；`lineBreakHyphenRE` 的判据是「连字符后必须有空格」，所以 `re- covery` → `recovery` 而 `lithium-ion` 保留；但 `Well- Known` 也会被拼成 `WellKnown`（比 `canDehyphenate` 宽松）。

**源码事实：** `Dehyphenate` 必须**在 `FinalReadingOrderMerge` 之后**（这样相邻行在切片里相邻且同栏）、**在 `NaiveVerticalMerge` 之前**（否则纵向合并会先插入那个空格）。

**源码事实：** `BoxesToSections` 的三条位置来源优先级是 `positions` > `b.Pages` > `pageHeights` 推断；单页**必须**用 `@@N` 形式（`@@N-N` 经 `ExtractPositions` 会往返成两个相同页号）；`ResolvePageSpan` 对「未知或非法下一页高」的兜底是「延一页然后停」。

**源码事实：** `SectionsToJSON` 对跨页合并表格的**首个** position 只取第一页的页号（「A merged table's primary bounds belong to its first page」）。

**架构推论：** 「k == 2 不可信」是这套算法最有洞察的一处。它承认**最常出现的版面恰好也是最难判的版面**：两栏很常见，而「假 gutter」（缩进造成的垂直白带）在两栏页面上同样常见。所以 gap 这个信号在 k=2 时判别力最低——必须换用「行分布的平衡性」这个正交信号。**判据是「当某个信号的最常见取值与最常见的误判取值重合时，该信号失效」**。

**架构推论：** 「文档级单栏多数覆盖逐页结果」这个做法放弃了对「逐页真实差异」的建模，换取了「少数畸形页不能破坏整篇」。代价是「一个真正的单栏与多栏混合文档会被全部当成单栏」。**判据是「逐页检测的误判率是否高于混合版面的出现率」**——对绝大多数文档成立。

**架构推论：** 大量常量带着语料实测说明（`0.12` 的 17.7%/22%/7.3%、`4` 的「10+ x-intercepts」、`2.5` 的双倍行距）。这类阈值**没有理论最优**，只能靠标注语料校准。而注释的价值在于**让后继者知道「改这个数之前需要什么样的反例」**——否则调参会变成随机试错。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/deepdoc/parser/pdf/layout/layout.go` | `DedupIdenticalText` | 完全相同文本的重复检测 |
| `internal/deepdoc/parser/pdf/layout/layout.go` | `DedupSubstringOverlaps` | OCR 双检测碎片的子串去重 |
| `internal/deepdoc/parser/pdf/layout/layout.go` | `boxInsideTolerant` | 容忍 Y 抖动的包含判定 |
| `internal/deepdoc/parser/pdf/layout/layout.go` | `FilterWatermarkBoxes` | 按 (页号, 单字符) 重复计数删水印 |
| `internal/deepdoc/parser/pdf/layout/layout.go` | `isSingleAsciiAlnum` | 单 ASCII 字母/数字判定（天然排除 CJK） |
| `internal/deepdoc/parser/pdf/layout/layout.go` | `TextMerge` | 横向合并在同一行的框 |
| `internal/deepdoc/parser/pdf/layout/layout.go` | `NaiveVerticalMerge` | 纵向合并（英文页禁用） |
| `internal/deepdoc/parser/pdf/layout/layout.go` | `FinalReadingOrderMerge` | 四键排序（阅读顺序的唯一决定点） |
| `internal/deepdoc/parser/pdf/layout/layout.go` | `groupBoxesByPage` | 按页分组（页号 + 索引列表） |
| `internal/deepdoc/parser/pdf/layout/layout.go` | `shouldMergeBoxes` / `mergeTwoBoxes` | 纵向合并的判据与实现 |
| `internal/deepdoc/parser/pdf/layout/layout.go` | `processPageBoxes` / `mergeColumnBoxes` | 单页处理与按栏合并 |
| `internal/deepdoc/parser/pdf/layout/layout.go` | `moveLeadingTitlesFirst` | 把前置标题移到前面 |
| `internal/deepdoc/parser/pdf/layout/layout.go` | `MergeSameBullet` | 同一项目符号的续行合并 |
| `internal/deepdoc/parser/pdf/layout/layout.go` | `pageNumSuffixPattern` | 页号后缀识别 |
| `internal/deepdoc/parser/pdf/layout/boxes_sections.go` | `ResolvePageSpan` | 跨页框的页跨度推断（延一页则停） |
| `internal/deepdoc/parser/pdf/layout/boxes_sections.go` | `BoxesToSections` | 三条位置来源 + 四道降级 |
| `internal/deepdoc/parser/pdf/layout/boxes_sections.go` | `NormalizeSectionPositions` | 从 tag 解析出 `Positions`（幂等） |
| `internal/deepdoc/parser/pdf/layout/boxes_sections.go` | `SectionsToJSON` | 输出 Python 兼容的 dict（含 `_pdf_positions`） |
| `internal/deepdoc/parser/pdf/layout/boxes_sections.go` | `InlinePNGDataURL` | base64 → data URI（含合法性校验） |
| `internal/deepdoc/parser/pdf/layout/dehyphen.go` | `Dehyphenate` | 断词重连（必须在读序合并后、纵向合并前） |
| `internal/deepdoc/parser/pdf/layout/dehyphen.go` | `canDehyphenate` | 五个条件全部必须成立 |
| `internal/deepdoc/parser/pdf/layout/dehyphen.go` | `endsWithBrokenHyphen` / `startsWithWordContinuation` | 两端判据 |
| `internal/deepdoc/parser/pdf/layout/dehyphen.go` | `linesAreVerticallyAdjacent` | `gap <= lineH * 2.5` |
| `internal/deepdoc/parser/pdf/layout/dehyphen.go` | `isBrokenHyphenRune` | 五个断词码位 |
| `internal/deepdoc/parser/pdf/layout/dehyphen.go` | `cleanIntraWordHyphens` | 框内残留（先删垃圾再拼） |
| `internal/deepdoc/parser/pdf/layout/combined_column.go` | `AssignColumn` | 逐页检测 + 单栏多数覆盖 |
| `internal/deepdoc/parser/pdf/layout/combined_column.go` | `detectColumnCount` | 分层投票（gap → 表格门 → 平衡门） |
| `internal/deepdoc/parser/pdf/layout/combined_column.go` | `gapColumnCount` / `gapColumnWidths` | 垂直投影直方图与列宽 |
| `internal/deepdoc/parser/pdf/layout/combined_column.go` | `detectColumnCount2D` / `detectColumnCountMedian` | 两个备用探测器 |
| `internal/deepdoc/parser/pdf/layout/combined_column.go` | `balancedBodyK2` | 平衡门（k=2 的正交判据） |
| `internal/deepdoc/parser/pdf/layout/combined_column.go` | `columnHistogramBins` | 直方图 bin 宽与数量（含 `maxBins` 保护） |
| `internal/deepdoc/parser/pdf/layout/combined_column.go` | `robustPageExtent` / `clusteredExtent` | 页宽估计（含离群裁剪上限） |
| `internal/deepdoc/parser/pdf/layout/combined_column.go` | `kmeansK2PlusPlus` / `kmeansCentroids` | KMeans 实现 |
| `internal/deepdoc/parser/pdf/layout/combined_column.go` | `minColLineFrac` / `tableMaxColFrac` / `maxColumnCount` / `maxPageExtent` / `maxBins` | 带实测依据的阈值常量 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow DeepDOC 介绍](https://ragflow.io/blog/deepdoc)
- [论文：PDF 多栏版面的阅读顺序重建](https://arxiv.org/abs/2103.10045)
- [PDF 规范：坐标系统与页面几何](https://opensource.adobe.com/dc-acrobat-sdk-docs/pdfstandards/PDF32000_2008.pdf)

## 与主核 / AI 的关系

「栏数检测」与主核（多模态数据库内核）的**基数估计**（cardinality estimation）是同一类问题：都在「从一个便宜的统计量推断一个结构性事实」，而两者都面临「最常见的情形恰好最难判」的困境。主核的 `n_distinct` 估计在「均匀分布」上很准、在「高度倾斜」上失准；这里的 gap 检测在「k >= 3」上很准、在「k == 2」上失准。**两份实现的应对方式相同：在不可靠的区间换用正交信号**（主核换直方图/MCV，这里换平衡门）。

第二处对照是「文档级多数覆盖」与主核的**默认统计信息 vs 逐表统计**。主核的 `pg_class.reltuples` 是一个粗粒度估计，而逐列的 `pg_statistic` 更精确；当逐列统计缺失时优化器回退到粗粒度值。但主核是「精确优先」，这里是「粗粒度优先」（单栏多数时覆盖逐页结果）。**这个方向相反是因为两者的失败代价不同**：主核误用粗粒度只是计划略差，而这里误判多栏会让整篇阅读顺序打乱。

第三处是「`FilterWatermarkBoxes` 用 `len(s)==1` 作为语言守卫」与主核里各种「用长度/类型做快速判别」的短路。两者都是「用一个便宜的特征排除大多数情形」。而它的局限相同：**它只处理了英文水印**。判据是「这个便宜特征是否恰好与需要处理的子集重合」——这里重合，所以有效；但它也让「中文水印」永久不被处理。

对 RAG 工程的实际意义在于：PDF 版面重建是整条链路里**唯一无法用「更好的模型」代替的环节**——因为它是几何推理，而 LLM 看不到坐标。这份实现给出的三条经验可直接用：**最常出现的版面往往最难判**（两栏与假 gutter 重合，必须换信号）、**语言相关的版面假设要显式编码**（中文纵向合并、英文不合并）、以及**阈值必须带语料实测依据**（否则后继者无法判断该不该调）。
