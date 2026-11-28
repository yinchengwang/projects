---
project: ragflow
feature: chapter-plan
title: 章节规划与 PDF 切片
page_type: feature
schema_version: 1
refs:
  - path: internal/parser/splitter/plan.go
    symbols: [PlanAdaptiveChapters, sanitizeTitle, newSectionCoalescer]
  - path: internal/parser/splitter/slice.go
    symbols: [SlicePDF]
---

# 章节规划与 PDF 切片

## 一句话职责

对超大 PDF（默认超过 200 页）先算出一份**全覆盖、无重叠、无空洞**的章节切片清单——有书签就按书签的层级切、超大节再下钻、碎节就近合并；然后按清单用 pdfium 逐段抽出独立合法的子 PDF，供并发 OCR。

## 机制是什么

这个模块的存在理由是一条纯工程的约束：**一整个 500 页 PDF 走 OCR 会超时或爆内存**。所以必须切成几段并行。而一旦要切，就必须回答三个问题：

1. **切在哪？** 自然边界是书签（PDF outline）。但书签不可靠——可能没有、可能只有一级、可能页码错乱。
2. **切多大？** 太小则段数过多（每段都有 OCR 冷启动成本），太大则失去并行的意义。
3. **如何保证不丢页？** 这是最关键的一条：切错了漏掉一页，那一页的内容就永久缺失，而**没有任何人能发现**（因为总页数是唯一可核对的量）。

第三点决定了整个实现的形式：**最后一步是强制连续化**。

```go
// PlanAdaptiveChapters produces an ordered list of non-overlapping, contiguous
// chapter slices covering pages [0, totalPages-1].
...
// 6. Guarantees 100% continuous coverage of [0, totalPages-1] with zero gaps and zero duplicates.
func PlanAdaptiveChapters(totalPages int, outlines []doctype.Outline, opts SplitOptions) []ChapterPlan {
```

**注意这条保证是「规则 6」，写在文档里而不是靠实现自然成立**。而实现它的手段是 `buildContinuousPlans`——一个在最后**强行改写 `start`/`end`** 的规整函数。

**第二个机制是「两个阈值的三段触发」。**

```go
if totalPages < opts.ThresholdPages || totalPages <= opts.MaxPages {
	return []ChapterPlan{{Index: 0, Title: "Full Document", StartPage: 0, EndPage: totalPages - 1, PageCount: totalPages}}
}
```

**默认 `ThresholdPages = 200`、`MaxPages = 120`、`MinPages = 30`**，所以：

| 页数 | 行为 |
|---|---|
| ≤ 120 | 整篇一章（"Full Document"） |
| 121 ~ 199 | **也是整篇一章**（因为 `< 200`） |
| ≥ 200 | 走自适应切分 |

**「121~199 页也整篇一章」是 `ThresholdPages` 的作用** —— 它把「该不该切」与「切多大」分成两个独立判断。所以调 `MaxPages` 不会意外触发切分，必须先过 `ThresholdPages`。

**第三个机制是「书签层级的下钻」。**

书签只取**最浅一级**（`minLevel`）作为章边界：

```go
minLevel, topOutlines := findTopLevelOutlines(validOutlines)
```

然后某个章超 `MaxPages` 时，**只下钻一级**（`minLevel+1`）：

```go
if sec.end-sec.start+1 > opts.MaxPages {
	coalescer.flush()
	subSlices := partitionOversizedSection(sec, validOutlines, minLevel+1, opts)
```

**而 `partitionOversizedSection` 内部不再递归**——若下钻后仍超，直接按 `MaxPages` 定长窗口切：

```go
if subEp-subSp+1 > opts.MaxPages {
	subCoalescer.addFixedWindows(subSp, subEp, subTitle)
}
```

**所以下钻深度上限是 2 层**（`minLevel` 与 `minLevel+1`）。这是一处有意的简化：更深的书签层级信噪比更低，而下钻两次后剩下的问题只能靠定长切。

## 怎么实现的

### `SplitOptions.Normalize`：一处自指的默认值

```go
func (opts SplitOptions) Normalize() SplitOptions {
	if opts.MinPages <= 0 {
		opts.MinPages = DefaultMinPages
	}
	if opts.MaxPages <= opts.MinPages {
		opts.MaxPages = opts.MinPages + DefaultMaxPages
	}
	if opts.ThresholdPages <= 0 {
		opts.ThresholdPages = DefaultThresholdPages
	}
	if opts.MaxConcurrency <= 0 {
		opts.MaxConcurrency = DefaultMaxConcurrency
	}
	return opts
}
```

四处：

1. **`MaxPages <= MinPages` 时兜到 `MinPages + 120`** —— 这是**唯一一处「相对默认值」**：其他三项都是绝对值（30 / 200 / 2）。而这里的 `DefaultMaxPages` 被当作「增量」而非「值」。

   **这是一处容易误读的地方**：如果用户设 `MinPages=50, MaxPages=40`，结果是 `MaxPages=170`（= 50+120），而不是 120。而注释说 `MaxPages` 的默认是 120——两处语义不同。

2. **顺序有依赖**：`MaxPages` 的判断依赖 `MinPages` 已经被规整，所以两段顺序不能交换。

3. **`<=` 而不是 `<`** —— 所以 `MaxPages == MinPages` 也算非法（会导致合并器无法前进）。

4. **没有校验 `ThresholdPages` 与 `MaxPages` 的关系** —— 所以可以配出「阈值 50、最大 500」这种「50 页就触发切分但一段最多 500 页」的组合，实际效果等于不切。**这不是 bug，只是配置空间里存在无意义的组合。**

### `sanitizeTitle`：为文件名而净化

```go
var sanitizeTitleRe = regexp.MustCompile(`[\\/*?:"<>|\r\n\t]+`)

func sanitizeTitle(title string) string {
	cleaned := sanitizeTitleRe.ReplaceAllString(title, " ")
	cleaned = strings.TrimSpace(cleaned)
	if len(cleaned) > 40 {
		cleaned = strings.TrimSpace(cleaned[:40])
	}
	return cleaned
}
```

三处：

1. **正则里的字符集是 Windows 文件名非法字符 + 控制字符**（`\ / * ? : " < > |` 加 `\r \n \t`）。所以标题**最终会进文件名或日志**。

   而 `:` 与 `?` 在 URL 里也有特殊含义——所以这个集合同时覆盖了「文件名」与「URL 片段」两种用途。

2. **替换成空格而不是删除** —— 避免 `A/B` 变成 `AB`。

3. **截断到 40 字节** —— 而 `cleaned[:40]` 是**按字节切片**，不是按 rune！所以中文标题会被切在 UTF-8 字符中间，产生一个非法字节序列。

   **这是一处真实的缺陷**：`len(cleaned) > 40` 判的也是字节数。一个 14 个汉字（42 字节）的标题会被切成 13 个完整汉字 + 1 个不完整字节的前缀。

   而 `strings.TrimSpace` 对非法 UTF-8 不做补救（它只检查 rune 边界上的空白）。

   **注意这个缺陷在当前路径上的实际影响**：`sanitizeTitle` 的产物用于 `ChapterPlan.Title`，而 `ChapterPlan` 的注释说 `Title` 是「Derived chapter title or label」。若它只用于日志/展示，非法字节会被 Go 的打印替换成 `\ufffd`；若它被用作文件名，会产生一个截断的文件。**所以影响取决于下游**，但这是一个应当按 rune 截断的地方。

### `findTopLevelOutlines`：一个 `minLevel+1` 的兜底

```go
func findTopLevelOutlines(validOutlines []doctype.Outline) (int, []doctype.Outline) {
	minLevel := math.MaxInt
	for _, o := range validOutlines {
		if o.Level < minLevel { minLevel = o.Level }
	}
	var l1 []doctype.Outline
	for _, o := range validOutlines {
		if o.Level == minLevel { l1 = append(l1, o) }
	}
	if len(l1) == 0 && minLevel+1 < 10 {
		for _, o := range validOutlines {
			if o.Level == minLevel+1 { l1 = append(l1, o) }
		}
	}
	return minLevel, l1
}
```

三处：

1. **`minLevel+1 < 10`** —— 这个 `10` 是**深度上限的硬编码**，而它的含义能推出来：PDF 书签层级理论上可达 20+，但实践中很少超过 10。所以「最浅一级不存在条目」时下探一级，但不下探到荒谬的深度。

   **而 `len(l1) == 0` 在这个场景下几乎不可能为真**——因为 `minLevel` 本身就是从这些条目里取的最小值，所以至少有一条 `Level == minLevel`。

   **所以这个兜底分支实际上是死代码**（除非 `Level` 字段有负值或存在别的异常）。但 `minLevel` 的初始化是 `math.MaxInt`，若 `validOutlines` 为空则 `minLevel` 保持 `MaxInt`——而调用方已经在前面判过 `len(validOutlines) == 0` 并返回。

   **所以这整个兜底分支是双重不可达**。

2. **`minLevel` 在 `len(validOutlines)==0` 时是 `math.MaxInt`** —— 而 `minLevel+1` 会**溢出回绕**成 `math.MinInt`。若真的走到这条路径，`minLevel+1 < 10` 为真（负值），然后内层循环找 `Level == minLevel+1`（一个负的天文数字）——找不到。所以还是返回空。

   **这是一处「用哨兵大值做累加」的隐患**，虽然当前不可达。

3. **两级循环而不是一次遍历** —— 因为需要先算 `minLevel` 再筛。**可以合并成一次（用一个切片记录并清空）**，但当前写法更清晰。

### `buildInitialSections`：书签 → 页范围的转换

```go
func buildInitialSections(outlines []doctype.Outline, totalPages int) []rawSection {
	var sections []rawSection
	for i, item := range outlines {
		sp := item.PageNumber - 1
		if i == 0 {
			// Include pre-chapter pages (cover, front matter) in the first chapter.
			sp = 0
		}
		ep := totalPages - 1
		if i+1 < len(outlines) {
			nextPage := outlines[i+1].PageNumber - 1
			if nextPage > sp {
				ep = nextPage - 1
			} else {
				ep = sp
			}
		}
		if ep < sp {
			ep = sp
		}
		sections = append(sections, rawSection{start: sp, end: ep, title: sanitizeTitle(item.Title)})
	}
	return sections
}
```

五处：

1. **`PageNumber - 1`** —— 书签页码是 1-based，转 0-based。

2. **第一章从第 0 页开始** —— 注释：「Include pre-chapter pages (cover, front matter) in the first chapter」。**所以封面与前言不会成为独立的章**，而是被并进第一章。

   **这是一处「保证覆盖」的关键处理**：若第一章从书签所在页开始，`[0, sp-1]` 就成了空洞。而后面 `buildContinuousPlans` 的 `i == 0` 分支会再次强制 `start = 0`——两道保险。

3. **`nextPage > sp` 时才 `ep = nextPage - 1`，否则 `ep = sp`** —— 所以「两个书签在同一页或后一个页码更小（书签乱序）」时，前一个章退化成单页。

   **乱序书签是真实的**：`filterAndSortOutlines` 已经按 `PageNumber` 排序，所以 `nextPage < sp` **不应该发生**。但代码仍判了——因为 `buildInitialSections` 也可以被别的调用方传未排序的输入。

   **这是一处防御性分支**，而它的存在让「书签乱序时章会塌成单页」这个行为成为可能（若没有前置排序）。

4. **`if ep < sp { ep = sp }`** —— 第二道保险。

5. **标题 `sanitizeTitle(item.Title)`** —— 而 `item.Title` 可能为空（书签无标题），于是标题是空串，后面会由 coalescer 或 `buildContinuousPlans` 兜成 `"Pages x-y"` / `"Part 01"`。

### `partitionOversizedSection`：下钻一级或定长切

```go
func partitionOversizedSection(sec rawSection, validOutlines []doctype.Outline, subLevel int, opts SplitOptions) []rawSection {
	sp, ep, title := sec.start, sec.end, sec.title

	var subItems []doctype.Outline
	for _, o := range validOutlines {
		if o.Level == subLevel && (sp+1) <= o.PageNumber && o.PageNumber <= (ep+1) {
			subItems = append(subItems, o)
		}
	}
	if len(subItems) == 0 {
		c := newSectionCoalescer(opts)
		c.addFixedWindows(sp, ep, title)
		return c.result()
	}
	...
```

四处：

1. **子书签的筛选条件是「`Level == subLevel` 且页码落在章范围内」** —— 注意 `(sp+1) <= o.PageNumber <= (ep+1)` 是**1-based 比较**，与 `sp`/`ep` 的 0-based 混用。

   所以写成了 `sp+1 <= PageNumber <= ep+1`，等价于 `sp <= PageNumber-1 <= ep`，即 0-based 的 `[sp, ep]`。**逻辑正确但读起来绕**。

2. **只取 `Level == subLevel` 一层** —— 不取更深层。所以下钻严格一级。

3. **无子书签时直接定长切** —— 意味着「这一章内没有任何同级书签」时退化为窗口切分。

4. **子章标题会带上父标题前缀**：

```go
subTitle := sanitizeTitle(sItem.Title)
if title != "" && !strings.Contains(subTitle, title) {
	subTitle = title + ": " + subTitle
}
```

   **`!strings.Contains(subTitle, title)` 的守卫** —— 避免「子标题已含父标题」时重复拼接（如父「第一章 概述」、子「第一章 概述 背景」）。

   **这个守卫是必要的**，因为 PDF 书签的子项标题常常天然包含父项文本（部分生成器就是这么写的）。

   而拼接用 `": "` 分隔 —— 所以最终标题形如「第一章 概述: 背景」。

### `sectionCoalescer`：三个指针字段做「前向追加」

```go
type sectionCoalescer struct {
	opts      SplitOptions
	slices    []rawSection
	curStart  *int
	curEnd    *int
	curTitles []string
}
```

**`*int` 而不是 `int` + `bool`** —— 用 nil 表示「没有累积中的段」。**这是 Go 里「可选值」的一种常见写法**，代价是每次读写都要解引用。

```go
func (c *sectionCoalescer) add(sp, ep int, title string) {
	cnt := ep - sp + 1
	if c.curStart == nil {
		startVal := sp
		endVal := ep
		c.curStart = &startVal
		c.curEnd = &endVal
		c.curTitles = []string{title}
		return
	}
	curLen := *c.curEnd - *c.curStart + 1
	combinedLen := ep - *c.curStart + 1
	shouldCoalesce := combinedLen <= c.opts.MaxPages &&
		(curLen < c.opts.MinPages || cnt < c.opts.MinPages || combinedLen <= c.opts.MinPages)
	if shouldCoalesce {
		*c.curEnd = ep
		c.curTitles = append(c.curTitles, title)
	} else {
		c.flush()
		...
	}
}
```

四处：

1. **`startVal := sp` 然后取址** —— 必需的，因为 `c.curStart = &sp` 会取到**循环变量的地址**（在 `for` 里这是共享变量）。这是 Go 里「循环变量取址」的经典陷阱，而这里通过显式局部变量避开。

   **注意 `add` 的参数不是循环变量**（`sp`/`ep` 是值参数），所以即使 `&sp` 也安全——但这个写法已经成为习惯。

2. **`shouldCoalesce` 是一个「与」+「三选一」**：

   - 硬条件：`combinedLen <= MaxPages`（合并后不超上限）
   - 软条件（三选一）：当前段短于 `MinPages` **或** 新段短于 `MinPages` **或** 合并后仍不超过 `MinPages`

   **第三个条件 `combinedLen <= MinPages` 是关键**：即使两段都 ≥ `MinPages`，只要合并后仍 ≤ `MinPages` 也合并。

   但这条**几乎不可能独立成立**：若 `curLen >= MinPages` 且 `cnt >= MinPages`，则 `combinedLen >= 2*MinPages > MinPages`。所以第三个条件只在「前两个都不成立时」被求值，而那时它必为假。

   **所以第三个条件实际上是冗余的**（给定前两个为假）。

   **这是一处逻辑上的多余项**，无害但会让读者困惑。

3. **`*c.curEnd = ep` 而不是 `*c.curEnd += cnt`** —— 等价（因为段是连续的）。用 `= ep` 更直接。

4. **`c.curTitles` 累积所有被合并段的标题**，最后 `strings.Join(curTitles, " + ")` —— 所以合并后的标题是「A + B + C」。

   **这是一处「保留全部来源信息」的做法**：与其选一个代表，不如列出全部。而它带来的问题是标题会变长（3 段合并就有 3 个标题）。

   而 `flush` 里对空标题有兜底：

```go
title := strings.Join(c.curTitles, " + ")
if title == "" {
	title = fmt.Sprintf("Pages %d-%d", *c.curStart+1, *c.curEnd+1)
}
```

   **注意 `strings.Join(["", "", ""], " + ")` 得到 `" +  + "`** —— 不是空串！所以空标题的兜底只在「`curTitles` 为空切片」时触发，而 `add` 总是至少加一个元素。

   **所以「三个空标题合并」会产出 `" +  + "` 这样的标题**，而不是 `"Pages x-y"`。这是一处小的不一致。

### `buildContinuousPlans`：强制连续化

这是本页最重要的函数，因为它是「零空洞」保证的落地处。

```go
func buildContinuousPlans(slices []rawSection, totalPages int) []ChapterPlan {
	if len(slices) == 0 { return nil }
	var plans []ChapterPlan
	curr := 0
	for i, s := range slices {
		start := s.start
		end := s.end
		if i == 0 {
			start = 0
		} else if start != curr {
			start = curr
		}
		if end < start { end = start }
		if i == len(slices)-1 {
			end = totalPages - 1
		}
		...
		curr = end + 1
	}
	if curr < totalPages && len(plans) > 0 {
		last := &plans[len(plans)-1]
		last.EndPage = totalPages - 1
		last.PageCount = last.EndPage - last.StartPage + 1
	}
	return plans
}
```

六处：

1. **四个强制改写**：
   - `i == 0` → `start = 0`（覆盖首页）
   - `start != curr` → `start = curr`（**消除所有空洞**）
   - `end < start` → `end = start`（保证非空）
   - 最后一段 → `end = totalPages - 1`（覆盖末页）

   **`start != curr` 这一条是整个保证的核心**：`curr` 是「上一段结束的下一页」，所以任何 `start > curr` 的空洞都被填掉。

   **它的副作用是「重叠也被消除」**：若 `start < curr`（下一段起点早于上段结束），也被改成 `curr`。所以「重叠」与「空洞」用同一句代码处理。

2. **`curr = end + 1`** —— 所以 `curr` 始终是「下一段的理想起点」。

3. **末尾的二次修补是冗余的** —— 因为循环里最后一段已经 `end = totalPages - 1`，所以循环结束后 `curr = totalPages`，`curr < totalPages` 为假。

   **这是一处死代码**（防御性的），而它与循环内的 `i == len(slices)-1` 分支功能重复。

4. **`Title` 的空兜底用 `"Part %02d"`** —— 与 `planFixedWindows` 的 `"Part %02d (Pages %d-%d)"` 不同（后者含页码）。所以「兜底标题」有两种格式。

5. **`PageCount` 在最后一段的二次修补里被重算** —— 但循环内已经算过。所以也是一处重复。

6. **`plans` 的长度等于 `slices` 的长度** —— 不改写数量。所以「合并/切分的结果数量」被完全接受，只有边界被强制。**这是一个重要的性质**：连续化不会意外增删章节。

### `planFixedWindows`：无书签时的基线

```go
func planFixedWindows(totalPages, windowSize int) []ChapterPlan {
	var plans []ChapterPlan
	idx := 0
	for start := 0; start < totalPages; start += windowSize {
		end := start + windowSize - 1
		if end >= totalPages { end = totalPages - 1 }
		plans = append(plans, ChapterPlan{
			Index: idx,
			Title: fmt.Sprintf("Part %02d (Pages %d-%d)", idx+1, start+1, end+1),
			...
		})
		idx++
	}
	return plans
}
```

三处：

1. **`windowSize` 未做 `<= 0` 的保护** —— 而 `Normalize` 保证了 `MaxPages > MinPages > 0`，所以调用方只要走过 `Normalize` 就安全。

   **若有人直接调 `planFixedWindows(100, 0)` 会死循环**（`start += 0`）。所以这是一个**依赖前置规整的私有函数**——而它是小写（包内可见），所以这个依赖是局部可控的。

2. **`Title` 同时含序号与页码** —— 比 `buildContinuousPlans` 的兜底更详细。

3. **`idx` 与 `start` 并行递增** —— 因为窗口不等长（末窗口可能更短），所以不能由 `start` 算出 `idx`。

### `filterAndSortOutlines`：两道过滤 + 稳定排序

```go
func filterAndSortOutlines(outlines []doctype.Outline, totalPages int) []doctype.Outline {
	var valid []doctype.Outline
	for _, o := range outlines {
		if o.PageNumber > 0 && o.PageNumber <= totalPages {
			valid = append(valid, o)
		}
	}
	sort.SliceStable(valid, func(i, j int) bool {
		if valid[i].PageNumber != valid[j].PageNumber {
			return valid[i].PageNumber < valid[j].PageNumber
		}
		return valid[i].Level < valid[j].Level
	})
	return valid
}
```

三处：

1. **过滤掉页码越界的书签** —— `PageNumber <= 0` 或 `> totalPages`。**这处理了「书签页码错乱」这类真实问题**（有些 PDF 的书签页码基于印刷页码而非 PDF 页序号）。

   但注意：**它只是丢弃**，不做任何尝试映射。所以「书签页码系统性地偏移 3 页」的 PDF 会丢掉前 3 个书签并让所有边界错位。

   **而错位的边界仍被 `buildContinuousPlans` 强行连续化** —— 所以不会漏页，只是章的划分与内容不符。

2. **同页码时按 `Level` 升序** —— 所以「同一页有多个层级的书签」时，浅的在前。这影响 `buildInitialSections` 的相邻计算：前一个浅书签的 `ep` 会是 `nextPage - 1`，若下一个书签同页则 `nextPage == sp`，走 `ep = sp` 分支，变成单页章。

3. **`SliceStable` 而不是 `Slice`** —— 而比较函数已经包含 `PageNumber` 与 `Level` 两个键。**若两者都相同则视为相等，`SliceStable` 保留原顺序（`Slice` 不保证）。**

   所以「同一页同层级的多条书签」会按 PDF 中的原始顺序排列 —— 这是对的，因为那代表了文档的自然顺序。

## 关键取舍

**源码事实：** 触发条件是两个独立阈值：`totalPages < ThresholdPages`（默认 200）**或** `totalPages <= MaxPages`（默认 120）时整篇一章。所以 121~199 页的文档也整篇一章——「该不该切」与「切多大」是分开的判断。

**源码事实：** `PlanAdaptiveChapters` 的文档规则 6 明确承诺「100% continuous coverage of [0, totalPages-1] with zero gaps and zero duplicates」，而其落地手段是 `buildContinuousPlans` 的四个强制改写。

**源码事实：** `SplitOptions.Normalize` 里 `MaxPages <= MinPages` 时兜到 `MinPages + DefaultMaxPages`（**相对增量**），而其他三项兜到绝对值。所以 `MinPages=50, MaxPages=40` 得到 `MaxPages=170` 而不是 120。顺序有依赖（`MaxPages` 的规整依赖 `MinPages` 已被规整）。

**源码事实：** `Normalize` 不校验 `ThresholdPages` 与 `MaxPages` 的关系，所以「阈值 50、最大 500」这种组合合法但实际等于不切。

**源码事实：** `sanitizeTitle` 的正则是「Windows 文件名非法字符 + 控制字符」（`\ / * ? : " < > |` 与 `\r \n \t`），替换成空格；**超过 40 时按 `cleaned[:40]` 字节截断**——对中文标题会切在 UTF-8 字符中间产生非法字节序列。

**源码事实：** `findTopLevelOutlines` 的 `len(l1) == 0` 兜底分支是**双重不可达**：`minLevel` 取自同一集合的最小值，所以至少一条命中；且 `validOutlines` 为空时调用方已提前返回。而 `minLevel` 初始化为 `math.MaxInt`，若真的进入该分支则 `minLevel+1` 会溢出回绕。

**源码事实：** `buildInitialSections` 强制第一章从第 0 页开始（「Include pre-chapter pages (cover, front matter) in the first chapter」），从而封面/前言不会成为独立章也不会成空洞；`buildContinuousPlans` 的 `i == 0` 分支是第二道保险。

**源码事实：** 书签下钻**只做一级**：先用 `minLevel` 切顶层章，超 `MaxPages` 的章用 `minLevel+1` 下钻，下钻后仍超的**直接走定长窗口**（不再递归）。所以深度上限是 2。

**源码事实：** `partitionOversizedSection` 的子书签筛选条件写成 `(sp+1) <= o.PageNumber <= (ep+1)`——0-based `sp`/`ep` 与 1-based `PageNumber` 混用，逻辑正确但读起来绕。

**源码事实：** 子章标题会拼上父标题前缀，且有 `!strings.Contains(subTitle, title)` 守卫——因为 PDF 书签的子项标题常常天然包含父项文本。

**源码事实：** `sectionCoalescer.add` 的 `shouldCoalesce` 是「合并后 ≤ MaxPages」**且**「三选一：当前段 < MinPages / 新段 < MinPages / 合并后 ≤ MinPages」。第三个条件在前两个为假时**必为假**（两段都 ≥ MinPages 则合并后 ≥ 2*MinPages），所以是冗余项。

**源码事实：** `curlStart`/`curEnd` 用 `*int` 表示「可选」，并用局部变量取址（避开 Go 的循环变量取址陷阱——虽然参数是值传递不存在该陷阱）。

**源码事实：** `sectionCoalescer.flush` 把合并段的标题 `strings.Join(curTitles, " + ")`，而空标题的兜底只判 `title == ""`——三个空标题会产出 `" +  + "` 而不是 `"Pages x-y"`。

**源码事实：** `buildContinuousPlans` 的四个强制改写：`i==0 → start=0`；`start != curr → start=curr`（统一处理空洞与重叠）；`end < start → end=start`；最后一段 `end = totalPages-1`。且 `plans` 长度等于 `slices` 长度——**连续化不会增删章节。**

**源码事实：** `buildContinuousPlans` 循环后的二次修补（`curr < totalPages` 时改末段 `EndPage`）是**死代码**，因为循环内最后一段已经覆盖到 `totalPages-1`。

**源码事实：** `planFixedWindows` 对 `windowSize <= 0` 无保护（会死循环），它依赖调用方已走 `Normalize`；`Title` 格式 `"Part %02d (Pages %d-%d)"` 与 `buildContinuousPlans` 的兜底 `"Part %02d"` 不同。

**源码事实：** `filterAndSortOutlines` 丢弃页码越界（`<=0` 或 `> totalPages`）的书签，**不做任何映射尝试**——所以「书签页码系统性偏移」的 PDF 会丢掉部分书签并让边界错位（但不漏页，因为连续化兜底）。

**源码事实：** `filterAndSortOutlines` 用 `SliceStable` 且比较键是（`PageNumber`, `Level`），所以同页同层级的多条书签按原顺序保留。

**源码事实：** `SlicePDF` 只做参数校验（非空、`startPage >= 0`、`endPage >= startPage`）后委托给 `pdfium.ExtractPageRange`，并位于 `//go:build cgo` 文件里；非 cgo 构建另有 `slice_nocgo.go` 返回 `ErrSplitterNotSupported`。

**架构推论：** 「最后一步强制连续化」是这类切分算法的正确形态。它把「切得对不对」与「切得全不全」解耦：中间所有策略（书签切分、下钻、合并、定长窗口）都只负责产出**尽量合理**的边界，而不管覆盖性；最后由一个纯规整函数保证 `[0, N-1]` 全覆盖。**这个分离让每个策略都不必自己操心漏页**——而漏页恰恰是这里最不可接受的失败。

**架构推论：** 「下钻深度上限 2 层」是一条基于信噪比的判断。PDF 书签的三级及以下常常是「段落级」而非「章节级」，按它们切会产出大量 1-2 页的段，而每段都有 OCR 冷启动成本。**判据是「边界的粒度是否与处理单元的经济性匹配」**：段太小则固定开销主导。

**架构推论：** `ThresholdPages` 与 `MaxPages` 分开是必要的。若只有 `MaxPages`，那么一个 130 页的文档会被切成 2 段——而 130 页走单次 OCR 完全可行，切分反而增加总时间。**把「要不要切」单独做成阈值，避免「为了限制段大小而引入不必要的并行」。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/parser/splitter/plan.go` | `PlanAdaptiveChapters` | 六条规则的自适应章节规划入口 |
| `internal/parser/splitter/plan.go` | `GetPDFPageCount` | 页数读取（失败或异常范围返回 0） |
| `internal/parser/splitter/plan.go` | `sanitizeTitle` | 标题净化（文件名安全 + 40 字节截断） |
| `internal/parser/splitter/plan.go` | `rawSection` | 中间段（`start`/`end`/`title`） |
| `internal/parser/splitter/plan.go` | `findTopLevelOutlines` | 取最浅层级书签（含不可达兜底） |
| `internal/parser/splitter/plan.go` | `buildInitialSections` | 书签 → 页范围（首页强制从 0 起） |
| `internal/parser/splitter/plan.go` | `partitionOversizedSection` | 下钻一级或定长切 |
| `internal/parser/splitter/plan.go` | `sectionCoalescer` | 前向追加的合并器（`*int` 表可选） |
| `internal/parser/splitter/plan.go` | `sectionCoalescer.add` | 合并判据（含一个冗余项） |
| `internal/parser/splitter/plan.go` | `sectionCoalescer.addFixedWindows` | 定长窗口切（标题带页码） |
| `internal/parser/splitter/plan.go` | `buildContinuousPlans` | **零空洞保证的落地处** |
| `internal/parser/splitter/plan.go` | `planFixedWindows` | 无书签时的基线切分 |
| `internal/parser/splitter/plan.go` | `filterAndSortOutlines` | 越界丢弃 + 稳定排序 |
| `internal/parser/splitter/types.go` | `ChapterPlan` | 切片结果（序号 / 标题 / 起止页 / 页数） |
| `internal/parser/splitter/types.go` | `SplitOptions` | 四个旋钮（`MinPages` / `MaxPages` / `ThresholdPages` / `MaxConcurrency`） |
| `internal/parser/splitter/types.go` | `DefaultSplitOptions` | 30 / 120 / 200 / 2 |
| `internal/parser/splitter/types.go` | `SplitOptions.Normalize` | 规整（`MaxPages` 用相对增量） |
| `internal/parser/splitter/types.go` | `ErrSplitterNotSupported` | 非 cgo 构建的哨兵错误 |
| `internal/parser/splitter/slice.go` | `SlicePDF` | 参数校验后委托 pdfium 抽页范围 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 文档：大文档的章节切分](https://ragflow.io/docs/dev/configure_knowledge_base)
- [PDF 规范：文档大纲（Document Outline）](https://opensource.adobe.com/dc-acrobat-sdk-docs/pdfstandards/PDF32000_2008.pdf)
- [论文：长文档 OCR 的分段并行](https://arxiv.org/abs/2304.02797)

## 与主核 / AI 的关系

「最后强制连续化」与主核（多模态数据库内核）的**范围分区裁剪**（partition pruning）是同一个问题的反面：分区的目标是**证明某些分区可以不读**，所以必须保证「分区集合覆盖全表且不重叠」——否则裁剪会漏数据。而这里的连续化是**主动修正**边界，主核的分区则由 DDL 严格约束（`FOR VALUES FROM ... TO ...` 且重叠的分区会被拒绝）。

**判据是「边界由谁产生」**：由用户 DDL 声明则可在建表时校验；由算法从不可靠的启发式（PDF 书签）推导则只能在最后修正。后者必然需要「规整」这一步。

第二处对照是 `sanitizeTitle` 的 40 字节截断与主核的 `varchar(n)`。主核的 `varchar(n)` 按**字符**计长度（`pg_mbstrlen`），所以一个 14 汉字的串在 `varchar(40)` 里装得下。这里按字节截断，所以装不下。**两者都叫「长度上限 40」，含义完全不同**——这正是「同名不同义」这一类缺陷的典型。

第三处是「下钻深度上限」与主核的**递归计划深度限制**（`max_stack_depth`）。两者都是「防止递归/下钻走得太深」，而判据不同：主核防的是栈溢出（硬性技术约束，默认 2MB 栈），这里防的是「切得太细导致段数爆炸」（经济性约束，硬编码 10 与隐式的 2 层）。**前者是安全阀，后者是调优参数**——而这里把调优参数写成了魔法数。

对 RAG 工程的实际意义在于：大文档处理是 RAG 里唯一「必须并行、且切分错误不可被发现」的环节。这份实现给出的三条经验可直接用：**切分策略与覆盖保证必须解耦**（让最后一个纯函数负责零空洞）、**要不要切与切多大要分开判断**（避免为限制段大小引入不必要的并行）、以及**凡是有「40 字符」这类上限的地方都要明确单位是字节还是字符**（中英文混排下差别 3 倍）。
