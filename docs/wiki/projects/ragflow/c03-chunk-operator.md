---
project: ragflow
feature: chunk-operator
title: 分块算子链
page_type: feature
schema_version: 1
refs:
  - path: internal/parser/chunk/chunk_type.go
    symbols: [Operator, ChunkData, ChunkContext]
  - path: internal/parser/chunk/split.go
    symbols: [SplitOperator, NewSplitOperator]
  - path: internal/parser/chunk/postprocess.go
    symbols: [PostprocessOperator]
  - path: internal/parser/chunk/options.go
    symbols: [ChunkOptions]
---

# 分块算子链

## 一句话职责

把一段文本跑成一组块：算子按 `Prepare → Execute → Finish` 三段式串起来，通过一个共享的 `ChunkContext` 传递中间结果；切分算子按四种策略产块，后处理算子做「合并小块 + 过滤」。

## 机制是什么

这套算子链的核心是**一个共享上下文 + 三段式生命周期**：

```go
type Operator interface {
	Prepare(ctx *ChunkContext) error
	Execute(ctx *ChunkContext) error
	Finish(ctx *ChunkContext) error
	String() string
}
```

而 `ChunkContext` 的字段本身就是**算子链的流水线槽位**：

```go
type ChunkContext struct {
	Origin string // raw text
	TextAfterPreprocess string // text after preprocess operator
	SplitChunks []ChunkData // chunks after split operator
	ResultChunks []ChunkData // final or intermediate chunks
}
```

**四个字段对应四个阶段**：`Origin`（入口原文）→ `TextAfterPreprocess`（预处理产物）→ `SplitChunks`（切分产物）→ `ResultChunks`（最终产物）。

**这是一个显式的「按阶段命名字段」设计**，而不是「一个 `data` 字段装所有东西」。好处是每个算子读哪个槽位、写哪个槽位一目了然，而且**「谁覆盖谁」是结构性的**：切分算子写 `SplitChunks`，后处理算子读 `SplitChunks` 写 `ResultChunks`。

代价是**这套结构只能表达一条固定的流水线**。若某天要跑两条并行分支再合并，这四个字段就不够用了——需要把 `ResultChunks` 变成 map 或者引入中间命名。

**第二个机制是 `Prepare/Execute/Finish` 三段的实际分工，而这份实现里 `Prepare` 几乎全是空的。**

```go
func (o *SplitOperator) Prepare(ctx *ChunkContext) error {
	return nil
}

func (o *SplitOperator) Finish(ctx *ChunkContext) error {
	return nil
}
```

**三个算子里 `Prepare` 与 `Finish` 全部返回 nil，只有 `Execute` 有内容。** 所以「三段式」当前是**预留框架**——配置解析被放进了构造函数（`NewSplitOperator(config)`），而不是 `Prepare`。

**这是一处值得记的不一致**：接口声明了 `Prepare(ctx)` 接收 **`ChunkContext`**，而配置来自构造函数。所以「准备阶段」本该做的事（从 DSL stage config 读参数）已经提前到构造期。

**后果**：`Prepare` 的签名里那个 `ctx` 参数是「未来可能用来读运行期状态」的预留位，而现在它永远不被使用。**这意味着算子实例与「一次运行」是解耦的**——一个算子可以被多次 `Execute`（只要上下文不同），而 `Prepare` 若真做了「按运行配置化」就不能如此。

**第三个机制是长度单位：rune 而不是字节。**

这个决定在好几处显式写着，而且每一处都给了理由：

```go
// bufRunes tracks the buffer length in runes: target and the per-chunk
// overflow check are rune-based, so the running total must be too.
// buf.Len() is bytes and inflates 3x for CJK, which flushes early and
// leaves every merged chunk far below target_size.
var bufRunes int
```

**「`buf.Len()` 是字节数，中文会膨胀 3 倍，于是过早 flush，每个合并后的块都远低于 `target_size`」** —— 这是一个具体的、可以量化的 bug。

同样在 `splitByLength` 的注释里：

> Sizing is by rune count (not bytes), so multi-byte (e.g. CJK) text is windowed by character.

**所以整个算子链的长度语义统一是「字符数」**。这是一条必须贯穿的纪律：只要有一个地方用了字节数，就会在这个语言混杂的语料上出错。

## 怎么实现的

### `ChunkData`：三个字段 + 一个派生字段

```go
type ChunkData struct {
	Content  string                 `json:"content"`
	Size     int                    `json:"size"`
	Index    int                    `json:"index,omitempty"`
	Metadata map[string]interface{} `json:"metadata,omitempty"`
}
```

四处：

1. **`Size` 与 `Content` 是冗余的** —— `Size` 可以由 `len([]rune(Content))` 算出。而它作为字段存在，是因为**不同策略对 `Size` 的定义不同**：
   - `splitByLength` 写 `Size: end - start`（窗口 rune 数）
   - `splitSentences` / `splitByParagraph` **完全不写**（保持零值）
   - 后处理算子的末尾统一写 `chunks[i].Size = len(chunks[i].GetContent())`——**注意这里是字节数！**

   **所以 `Size` 的语义在链内不一致**：切分阶段可能是「窗口 rune 数」或「零」，后处理之后变成「字节数」。

   而 `PostprocessOperator.Execute` 的那一句是唯一的统一赋值点：

```go
for i := range chunks {
	chunks[i].Index = i
	chunks[i].Size = len(chunks[i].GetContent())
}
```

   这解释了 `splitByParagraph` 那段注释为什么提到「只有在后处理阶段重新编号时那些间隙才消失」——**后处理既重新编号也重算 `Size`，所以它是「规格化」的最后一道。**

2. **`GetContent()` 是 nil 安全的** —— `if c == nil { return "" }`。所以 `(*ChunkData)(nil).GetContent()` 不 panic。这是一处防御（`chunks[i].Size = len(chunks[i].GetContent())` 的写法依赖它，虽然那里 `i` 一定在范围内）。

3. **`Index` 带 `omitempty`** —— 而 `Index` 是 `int`，所以 `omitempty` 会省略 `0`。**这意味着第一个块在 JSON 里没有 `index` 字段。** 若下游按「字段存在」判断，会漏掉第 0 块。

4. **`Metadata` 带 `omitempty`** —— 且值是 `map[string]interface{}`，所以 nil map 被省略。

**而 `Metadata` 的唯一内容是 `language`：**

```go
Metadata: map[string]interface{}{"language": DetectLanguage(content)}
```

**所以每个块都带自己的语言判定**。这是一处「逐块判定而不是全文判定」——因为一个混排文档的不同片段可以是不同语言。

### `SplitOperator`：四种策略，而 `length` 是唯一完整的

```go
func (o *SplitOperator) Execute(ctx *ChunkContext) error {
	text := ctx.TextAfterPreprocess
	if o.strategy == "" {
		o.strategy = "sentence"
	}
	switch o.strategy {
	case "sentence":
		ctx.SplitChunks = o.splitSentences(text)
	case "char":
		ctx.SplitChunks = o.splitByChar(text)
	case "paragraph":
		ctx.SplitChunks = o.splitByParagraph(text)
	case "length":
		ctx.SplitChunks = o.splitByLength(text)
	default:
		ctx.SplitChunks = o.splitSentences(text)
	}
	return nil
}
```

三处：

1. **`o.strategy = "sentence"` 写在 `Execute` 里** —— 即「惰性默认值」。而 `ChunkOptions` 的注释也说了「an unset strategy degrades to "sentence" inside the operator」。

   **在 `Execute` 里改接收者字段** 意味着这个算子第一次运行后就带上了默认值——所以它对「多次 Execute」不是无状态的，但结果一致（因为默认值只写一次且总相同）。

2. **`default` 分支也是 `sentence`** —— 所以未知策略不报错，静默降级。这与 `NewSplitOperator` 只对 `chunk_size` / `overlap` 的非法值报错形成对比。

3. **`text := ctx.TextAfterPreprocess`** —— 只读这个字段，**不读 `Origin`**。所以「预处理算子必须在切分之前」是一个硬前置条件，否则 `TextAfterPreprocess` 为空。

   **这是一处隐式的顺序约束**：字段名说明了依赖，但代码不检查。若只挂 `SplitOperator` 不挂预处理算子，`TextAfterPreprocess` 是空串，切分产出零块——**静默空结果**。

**`splitSentences`：手写扫描器，逐 rune 前进。**

```go
for i < len(text) {
	matchedBound := ""
	for _, bound := range sentenceBoundaries {
		if bound != "" && i+len(bound) <= len(text) && text[i:i+len(bound)] == bound {
			matchedBound = bound
			break
		}
	}
	if matchedBound != "" {
		if buf.Len() > 0 {
			content := strings.TrimSpace(buf.String())
			if content != "" {
				chunks = append(chunks, ChunkData{...})
			}
			buf.Reset()
		}
		i += len(matchedBound)
	} else {
		r, size := utf8.DecodeRuneInString(text[i:])
		buf.WriteRune(r)
		i += size
	}
}
```

六处：

1. **分隔符被丢弃** —— `i += len(matchedBound)` 跳过它，不写进 `buf`。所以句末标点不在块里。

   **这是「切分即丢弃」的语义**，与 `keepSeparators` 字段（在 `NewSplitOperator` 里被解析）**没有联用**！

   **这是一处真实的未实现**：`o.keepSeparators` 被读入 `SplitOperator`，在 `String()` 里被打印，但 `splitSentences` **从不检查它**。

2. **`sentenceBoundaries` 是硬编码的 8 个**：

```go
var sentenceBoundaries = []string{"。", "！", "？", ".", "!", "?", ";", "\n"}
```

   **中英文混排的一句话边界集合**，包含中文句号/叹号/问号与英文对应符号，加 `;` 与 `\n`。

   **注意没有中文分号「；」与中文逗号「，」** —— 所以中文用分号分隔的句子不会被切开。这是一处不对称（英文有 `;`，中文没有对应项），而 `boundaries` 配置项也没有被 `splitSentences` 使用（同样只被解析与打印）。

   **所以「boundaries 配置」当前是一个装饰**：解析进字段、打印进 `String()`、但不参与行为。

3. **`break` 在第一个命中的分隔符** —— 而 `sentenceBoundaries` 里没有「一个分隔符是另一个的前缀」的情形，所以顺序无关。

4. **`if buf.Len() > 0` 才产块** —— 所以连续的多个分隔符（`. . .`）不会产出空块。而 `content != ""` 又过了一遍 `TrimSpace` 后的判空。

5. **`utf8.DecodeRuneInString` 而不是 `text[i]`** —— 所以按 rune 前进，中文不会被切成半个字节。

   **但 `writeRune(r)` 之后 `i += size`** —— 这里的 `size` 来自 `DecodeRuneInString`，所以是「该 rune 的字节数」。**这是正确的**，但要注意 `buf` 累积的是 bytes 而 `i` 是字节偏移，两者一致。

6. **末尾的 flush 与循环内是同一段代码复制** —— 没有抽成小的闭包。**这是一处重复**，而两处必须完全一致（否则最后一块的处理会与其它块不同）。

**`splitByChar`：逐字成块，且语言判定用前缀和优化。**

```go
cjkSuffix := make([]int, n+1)
letterSuffix := make([]int, n+1)
for i := n - 1; i >= 0; i-- {
	cjkSuffix[i], letterSuffix[i] = cjkSuffix[i+1], letterSuffix[i+1]
	if unicode.Is(unicode.Han, runes[i]) { cjkSuffix[i]++ }
	if unicode.IsLetter(runes[i]) { letterSuffix[i]++ }
}
```

三处：

1. **注释说明了理由**：「Each chunk's language is `DetectLanguage` over the remaining suffix from that rune onward. Precompute suffix (cjk, letter) counts in one reverse pass so every chunk decides in O(1) instead of re-scanning O(n) runes.」

   **「从该 rune 往后到文本末尾」** —— 所以第 i 个块的语言判定用的是 `text[i:]`，而不是它自己的那一个字符（一个字符判不出语言）。

   **这是一处很聪明的近似**：用「剩余文本的语言分布」代理「这个字的上下文语言」。它避免了为每个块重扫 O(n)，代价是「同一段混合文本里靠前的字会用更长的后缀」。

2. **`n+1` 长度的数组** —— 为了 `cjkSuffix[n]` 的零值作为边界。

3. **两个计数器而不是调用 `DetectLanguage`** —— 所以 `languageFromCounts` 是 `DetectLanguage` 的可分解形式。**这是「把判定逻辑拆成可增量维护的形式」以便优化。**

**`splitByLength`：唯一同时用 `chunkSize` 与 `overlap` 的策略。**

```go
chunkSize := o.chunkSize
if chunkSize <= 0 {
	chunkSize = defaultLengthChunkSize
}
overlap := max(o.overlap, 0)
if overlap >= chunkSize {
	overlap = chunkSize - 1
}
step := chunkSize - overlap
```

三处：

1. **`chunkSize <= 0` 兜到 256** —— 而注释说「chunkSize defaults to defaultLengthChunkSize when not positive」。
2. **`overlap` 被夹到 `[0, chunkSize-1]`** —— 注释说明了为什么：

> overlap is clamped to [0, chunkSize-1] so **the window always advances by at least one rune and the function terminates**.

   **「保证步长 ≥ 1」是终止性条件**。若 `overlap == chunkSize`，`step = 0` → 死循环。**这是一处必须的 clamp，而不是可选的美化。**

3. **`if end == len(runes) { break }`** —— 早退避免最后一次迭代后再算一遍 `start += step` 并多跑一轮判断。**这是无害的优化**（即使不 break，`start += step` 后循环条件也会失败，除非 step 恰好让它小于 len）。

   实际上这里有个细节：若 `step` 很小而窗口到末尾，下一轮 `start < len(runes)` 仍成立，会产出一个**比 chunkSize 短的尾部窗口**——这是「重叠窗口滑到末尾」的正常行为，而不是重复。

**而 `Size` 只在 `splitByLength` 被写：** `Size: end - start`。所以「末窗口」的 `Size` 会小于 `chunkSize`。

**`splitByParagraph` 的 `Index` 修正有一段很好的注释：**

```go
// Index is the chunk's position in the output, the same as for
// the other strategies. Using the paragraph's position in the
// split left gaps wherever a blank line was skipped (a two-chunk
// document came back as 0 and 3), and those gaps only disappeared
// when a postprocess stage happened to re-index the result.
Index:    len(chunks),
```

**「一个两块文档回来是 0 和 3」** —— 这是一个具体的症状描述。而「只有在后处理阶段重新编号时那些间隙才消失」说明**这个 bug 曾经依赖后处理来掩盖**，即「不挂后处理算子就会暴露」。

**这是一处「缺陷被下游无意中治愈」的典型案例**，而复用它的风险是：任何绕过后处理的调用方都会看到带间隙的 `Index`。

### `PostprocessOperator`：合并 + 过滤 + 规格化

```go
func (o *PostprocessOperator) Execute(chunkCtx *ChunkContext) error {
	chunks := chunkCtx.SplitChunks
	if len(chunks) == 0 {
		return nil
	}
	if o.merge != nil {
		chunks = o.mergeChunks(chunks)
	}
	if o.filter != nil {
		chunks = o.filterChunks(chunks)
	}
	// Re-index
	for i := range chunks {
		chunks[i].Index = i
		chunks[i].Size = len(chunks[i].GetContent())
	}
	chunkCtx.ResultChunks = chunks
	return nil
}
```

四处：

1. **顺序固定：先合并后过滤** —— 而顺序有实际后果：先合并让小块变大，再过滤长度就能保住「合并后达到 `min_length`」的块。若反过来，单独的小块会先被删掉。

   **「先合并后过滤」是正确的顺序**，而它没有在注释里说明——是一处「显然但不显式」的设计。

2. **`len(chunks) == 0` 时直接返回且**不写 `ResultChunks` —— 所以 `ResultChunks` 保持之前的值（通常是 nil）。**这意味着空输入不会清空已有的 `ResultChunks`。** 若上下文被复用（多次 Execute），这可能是个隐患；但实际上每次运行都是新上下文。

3. **两个配置都是可选的**（`o.merge != nil` / `o.filter != nil`）—— 所以「只过滤不合并」与「只合并不过滤」都合法，而「两者都不配」时后处理只做规格化。

   **「两者都不配仍然有意义」这一点值得注意**：`PostprocessOperator` 的最小作用是「重新编号 + 重算 `Size`」。所以即使不需要合并过滤，挂它也是有价值的——**它是「规格化」算子**。

4. **`chunks := chunkCtx.SplitChunks` 是切片赋值** —— 而 `mergeChunks` / `filterChunks` 都返回**新切片**。所以原 `SplitChunks` 不被修改。

   **但元素是值类型 `ChunkData`**，所以「新切片里的元素是拷贝」——除了 `Metadata` 是 map（引用类型），所以合并时若修改 `bufMeta` 会影响到源块。

   而 `mergeChunks` 里恰恰有这个隐患：

```go
if buf.Len() == 0 {
	buf.WriteString(c.Content)
	bufRunes = cRunes
	bufMeta = c.Metadata      // ← 直接引用源块的 map
	firstIndex = c.Index
}
```

   **`bufMeta = c.Metadata` 是引用赋值**，接下来的 `for k, v := range c.Metadata { bufMeta[k] = v }` 会往里写：

```go
if c.Metadata != nil && bufMeta == nil {
	bufMeta = make(map[string]interface{})
}
for k, v := range c.Metadata {
	bufMeta[k] = v
}
```

   **注意那个守卫的顺序有问题**：`bufMeta == nil` 的检查发生在 `bufMeta = c.Metadata` **之后**，而那时如果源块的 `Metadata` 非 nil，`bufMeta` 就已经不是 nil 了。所以 `make` 分支只在**合并块的第一个源块 `Metadata` 为 nil** 时触发。

   于是「第一个源块的 metadata 非 nil」时，`bufMeta` 就是**源块的 map**，后续 `bufMeta[k] = v` 会**写进源块的 metadata**。若同一个块对象被别处引用（例如 `SplitChunks` 仍持有它），会看到被污染的数据。

   **这是一处真实的别名/污染缺陷**，而它的表现是「合并后的块的 metadata 里多了本该属于后续块的键」。

**`mergeChunks` 的合并条件里那个 `+1` 很值得看：**

```go
if bufRunes+cRunes+1 > target {
	// flush
} else {
	buf.WriteString(" ")
	buf.WriteString(c.Content)
	bufRunes += cRunes + 1
}
```

**`+1` 是「连接用的那个空格」的 rune 数** —— 因为合并时插入一个空格，所以计数必须包含它。**这是一处精确的记账**：若忘了 `+1`，合并结果会超过 `target` 一个字符（每次合并）。而 `splitByLength` 的 `Size` 记账没有这个问题（它不插分隔符）。

**而 `cRunes >= target` 的单块独立成块分支：**

```go
if cRunes >= target {
	if buf.Len() > 0 {
		// flush buffer
	}
	merged = append(merged, c)
	firstIndex = i + 1
	continue
}
```

四处：

1. **`>=` 而不是 `>`** —— 一块恰好 `target` 长时也独立成块。所以「合并目标」的上界是「严格小于 target」。
2. **`firstIndex = i + 1`** —— 而下一块若开启新 buffer 会用 `firstIndex = c.Index`，所以这个赋值**实际无用**。

   **这是一处死赋值**：`firstIndex` 在下一次进入「buf 为空」分支时会被 `c.Index` 覆盖，进入「`cRunes >= target`」分支时也会被 `i + 1` 覆盖，进入「`buf` 非空」分支时不被读。所以 `firstIndex = i + 1` 从未生效。

3. **`merged = append(merged, c)` 直接把原块放进去** —— 所以「超长块」不进 buffer、不拷贝、不改 `Size`。

4. **`buf.Reset()` 但 `bufRunes = 0` 与 `bufMeta = nil` 也重置** —— 三处都重置，否则下一个 buffer 的记账会带上旧的。

**`filterChunks` 的四个条件顺序：**

```go
if o.filter.DropEmpty && strings.TrimSpace(c.Content) == "" {
	continue
}
l := len([]rune(c.Content))
if o.filter.MinLength > 0 && l < o.filter.MinLength {
	continue
}
if o.filter.DropDuplicates {
	if _, dup := seen[c.Content]; dup {
		continue
	}
	seen[c.Content] = struct{}{}
}
```

三处：

1. **先判空再算长度** —— 因为空串的 rune 长度是 0，也会被 `min_length` 删掉，所以「先判空」只是省一次 `[]rune` 分配。
2. **`len([]rune(...))` 而不是 `len(c.Content)`** —— **这里的 `l` 是 rune 数，而 `PostprocessOperator.Execute` 末尾那句 `Size = len(...)` 是字节数**。两处单位不同，而都叫「长度」。

   **这是一处真实的单位不一致**：`MinLength` 按 rune 判，`Size` 存字节。

3. **去重用 `map[string]struct{}`** —— 而注释说明「order-preserving: the first occurrence is kept and later chunks with identical content are dropped」。所以是「保留第一个」。

   而 `seen` 在 `DropDuplicates` 为假时保持 nil —— 所以三个分支的 `if o.filter.DropDuplicates` 检查是必要的（不能省）。

**`NewPostprocessOperator` 的默认值写法有一处不一致：**

```go
if m, ok := config["merge"].(map[string]interface{}); ok {
	op.merge = &mergeConfig{}
	if ts, ok := m["target_size"].(float64); ok {
		op.merge.TargetSize = int(ts)
	} else {
		op.merge.TargetSize = 500
	}
}
```

**`target_size` 缺失时兜到 500，而这个 500 与 `mergeChunks` 里的 `if target <= 0 { target = 500 }` 重复** —— 两处硬编码同一个数。若改一处不改另一处，行为会不一致。

而 `mergeChunks` 那处兜底是必需的（因为 `TargetSize` 可以是 0 或负数，若配置里写了 `target_size: 0`）。

**「`target_size: 0` 会被当成 500」** —— 而语义上 `0` 应该是「不合并」。这是一处「零值被当作默认值」的歧义：**要关闭合并必须删掉整个 `merge` 配置块**。

### `ChunkOptions`：只有生产调用方用的类型化配置

```go
type ChunkOptions struct {
	NormalizeNewlines bool
	StripWhitespace   bool
	RemoveEmptyLines  bool
	SplitStrategy string
	MergeTargetSize int
	FilterMinLength int
}
```

而注释说明了它的范围：

> It intentionally models only the option subset used by the current production call sites.

**六处**：

1. **三个预处理开关的语义在注释里写清了**：「Any combination of the three is honoured; all false means "no preprocess stage" (the engine skips the stage rather than running an identity preprocess)」。

   **「全 false 就跳过阶段，而不是跑一个恒等预处理」** —— 这是一处「不值得跑」的判断：恒等阶段会浪费一次遍历。

2. **`MergeTargetSize` / `FilterMinLength` 的单位被明确写成 rune**：

```go
// MergeTargetSize > 0 enables the merge step; its value is the
// per-chunk budget in runes (Unicode code points).
MergeTargetSize int
// FilterMinLength > 0 drops chunks shorter than that (rune count).
FilterMinLength int
```

   **「> 0 才启用」** —— 所以 `0` 是「关闭」而不是「目标为 0」。这与 `merge` 配置块里 `target_size: 0` 被当成 500 的处理**相反**。

   **同一语义的两种约定**：类型化选项里 0 = 关闭，DSL map 里 0 = 用默认。**这是一处迁移期的双约定。**

3. **`SplitStrategy` 的注释也重复了「default to sentence」** —— 第三处重复这个默认值（`ChunkOptions`、`SplitOperator.Execute`、以及 `Execute` 的 `default` 分支）。

4. **`validate()` 只检查两个字段非负** —— 而注释说「an option set that fails validation will not produce meaningful results at run time」。所以是「便宜的一致性检查」。

5. **`SplitStrategy` 不校验** —— 所以「未知策略」在 `validate` 层不报错，在 `Execute` 里静默降级 sentence。**两处的宽容度一致**。

6. **`validate` 是小写方法** —— 所以它只在包内可用。**这暗示 `ChunkOptions` 的校验由包内的某个入口调用，而不是外部调用方自己调。**

## 关键取舍

**源码事实：** 算子接口是 `Prepare/Execute/Finish` 三段式 + `String()`；而这份实现里**所有算子的 `Prepare` 与 `Finish` 都返回 nil**，配置解析被放在构造函数里（`NewSplitOperator(config)` / `NewPostprocessOperator(config)`）。所以「三段式」当前是预留框架。

**源码事实：** `Prepare(ctx *ChunkContext)` 接收上下文但从不使用——算子实例因此与「一次运行」解耦，可被多次 `Execute`。

**源码事实：** `ChunkContext` 的四个字段就是流水线槽位：`Origin` → `TextAfterPreprocess` → `SplitChunks` → `ResultChunks`。「谁读哪个槽」是结构性约定，但**代码不检查前置算子是否挂上**——只挂 `SplitOperator` 时 `TextAfterPreprocess` 为空，静默产出零块。

**源码事实：** `ChunkData.Size` 的语义在链内不一致：`splitByLength` 写「窗口 rune 数」，`splitSentences`/`splitByParagraph` 不写（零值），而后处理算子的末尾统一写 `len(content)`（**字节数**）。所以「规格化」发生在后处理。

**源码事实：** `ChunkData.Index` 带 `omitempty` 且是 `int`，所以**第一个块的 JSON 里没有 `index` 字段**。

**源码事实：** `SplitOperator` 解析了 `boundaries` 与 `keep_separators`，`String()` 会打印它们，但 `splitSentences` **从不使用**——分隔符被硬编码为 8 个（`。！？.!?;\n`），且分隔符本身被**丢弃**（`i += len(matchedBound)` 不写入 buffer）。中文分号「；」不在集合里，所以中英不对称。

**源码事实：** 未知 `strategy` 在 `Execute` 的 `default` 分支静默降级为 `sentence`；`o.strategy = "sentence"` 这个惰性默认值写在 `Execute` 里（改接收者字段）。

**源码事实：** `splitSentences` 用 `utf8.DecodeRuneInString` 逐 rune 前进；末尾 flush 与循环内 flush 是**复制粘贴的两段代码**。

**源码事实：** `splitByChar` 用「后缀前缀和」把语言判定降到 O(1)：注释说明「Each chunk's language is `DetectLanguage` over the remaining suffix from that rune onward」——用剩余文本的语言分布代理该字的上下文语言。

**源码事实：** `splitByLength` 的 `overlap` 被夹到 `[0, chunkSize-1]`，理由是「the window always advances by at least one rune and the function terminates」——**这是终止性条件**，不是美化。

**源码事实：** `splitByParagraph` 的 `Index` 用 `len(chunks)` 而非段落下标，注释记录了原症状：「a two-chunk document came back as 0 and 3」，且「those gaps only disappeared when a postprocess stage happened to re-index the result」——**缺陷曾被下游无意中治愈**。

**源码事实：** `PostprocessOperator.Execute` 的顺序固定为「合并 → 过滤 → 重新编号并重算 `Size`」；`len(chunks) == 0` 时**不写 `ResultChunks`**（保持旧值/nil）。

**源码事实：** 后处理的两个配置块都可选，所以「两者都不配」时它的唯一作用是「重新编号 + 重算 `Size`」——它同时是一个规格化算子。

**源码事实：** `mergeChunks` 里 `bufMeta = c.Metadata` 是**引用赋值**，随后 `for k, v := range c.Metadata { bufMeta[k] = v }` 会写进源块的 map——**别名污染**。而 `if bufMeta == nil { make(...) }` 的守卫在 `bufMeta = c.Metadata` 之后，所以只在第一个源块 metadata 为 nil 时触发。

**源码事实：** `mergeChunks` 的合并条件里的 `+1` 是「连接空格」的 rune 数（`bufRunes+cRunes+1 > target`，写入时同样 `+= cRunes + 1`）——精确记账，否则每次合并都会超 target 一个字符。

**源码事实：** `cRunes >= target` 用 `>=`，所以恰好等于 target 的块也独立成块；该分支末尾的 `firstIndex = i + 1` 是**死赋值**（后续三个分支都会覆盖或不再读它）。

**源码事实：** `filterChunks` 的长度判据用 rune 数（`len([]rune(...))`），而 `Execute` 末尾存的 `Size` 是字节数——**两处单位不同但都叫「长度」**。

**源码事实：** `TargetSize` 的默认值 500 在 `NewPostprocessOperator` 与 `mergeChunks` 里**硬编码两次**；`target_size: 0` 会被当成 500（用默认），所以要关闭合并必须删掉整个 `merge` 块。

**源码事实：** `ChunkOptions` 里 `MergeTargetSize > 0` / `FilterMinLength > 0` 表示「启用」，所以**类型化选项里 0 = 关闭，而 DSL map 里 `target_size: 0` = 用默认**——迁移期的双约定。

**源码事实：** `ChunkOptions` 的注释说「all false means no preprocess stage (the engine skips the stage rather than running an identity preprocess)」；`validate()` 只检查两个非负，不校验 `SplitStrategy`（与 `Execute` 的静默降级一致）。

**架构推论：** 这套算子链的价值不在三个算子本身，而在**把「阶段」变成可命名的东西**。`TextAfterPreprocess` / `SplitChunks` / `ResultChunks` 三个字段名把流水线的中间态固化成类型系统的一部分——所以「某算子在读旧数据」这种 bug 会表现为「字段是空的」而不是「结果不对」。代价是**只能表达线性链**。

**架构推论：** `Prepare` 与 `Finish` 全为空这件事，说明**「三段式接口」的成立条件比想象中弱**。它真正的用途是给未来的「按运行配置化」与「释放资源」留位，而当前所有配置都进了构造期。这带来一个隐含约束：**算子实例可以被共享/复用**（因为无运行期状态）。若将来真把配置搬回 `Prepare`，就必须重新审视「算子是否可共享」。

**架构推论：** `Size` 的单位不一致（切分阶段 rune / 后处理阶段字节）是一个典型的「同名不同义」缺陷。它的危险性在于**两端都是 `int`**，所以编译器无法发现。修法只能是「统一为 rune」或「改名」（如 `SizeRunes` / `SizeBytes`）。而这份实现选择了「靠后处理统一覆盖」——这让「不挂后处理」的路径保留错误的 `Size`。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/parser/chunk/chunk_type.go` | `Operator` | 三段式算子接口 + `String()` |
| `internal/parser/chunk/chunk_type.go` | `ChunkData` | 单块（内容 / 长度 / 序号 / 元数据） |
| `internal/parser/chunk/chunk_type.go` | `ChunkData.GetContent` | nil 安全的内容读取 |
| `internal/parser/chunk/chunk_type.go` | `ChunkContext` | 四个流水线槽位 |
| `internal/parser/chunk/split.go` | `SplitOperator` | 四种切分策略 |
| `internal/parser/chunk/split.go` | `NewSplitOperator` | 从 DSL 配置解析（含 `chunk_size` / `overlap` 报错） |
| `internal/parser/chunk/split.go` | `SplitOperator.Execute` | 策略分派（惰性默认 + 静默降级） |
| `internal/parser/chunk/split.go` | `sentenceBoundaries` | 硬编码的 8 个句子边界（配置项未使用） |
| `internal/parser/chunk/split.go` | `splitSentences` | 逐 rune 扫描 + 丢弃分隔符 |
| `internal/parser/chunk/split.go` | `splitByChar` | 逐字成块 + 后缀前缀和语言判定 |
| `internal/parser/chunk/split.go` | `splitByLength` | 定长窗口 + 重叠（clamp 保证终止） |
| `internal/parser/chunk/split.go` | `splitByParagraph` | 按行切 + `Index` 修正 |
| `internal/parser/chunk/split.go` | `defaultLengthChunkSize` | 未配置时的窗口大小 256 |
| `internal/parser/chunk/postprocess.go` | `PostprocessOperator` | 合并 + 过滤 + 规格化 |
| `internal/parser/chunk/postprocess.go` | `NewPostprocessOperator` | 两个可选配置块的解析（含 500 默认） |
| `internal/parser/chunk/postprocess.go` | `PostprocessOperator.Execute` | 固定顺序 + 统一重编号与 `Size` |
| `internal/parser/chunk/postprocess.go` | `mergeChunks` | 贪心合并（rune 记账 + `+1` 空格 + 别名隐患） |
| `internal/parser/chunk/postprocess.go` | `filterChunks` | 空 / 最短长度 / 去重（保首个） |
| `internal/parser/chunk/postprocess.go` | `mergeConfig` / `filterConfig` | 两个配置结构 |
| `internal/parser/chunk/options.go` | `ChunkOptions` | 生产调用方的类型化选项（rune 单位） |
| `internal/parser/chunk/options.go` | `ChunkOptions.validate` | 非负校验（不校验策略名） |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 文档：分块方法](https://ragflow.io/docs/dev/configure_knowledge_base)
- [论文：固定窗口与重叠对检索质量的影响](https://arxiv.org/abs/2402.19473)
- [Unicode 文档：字素簇与码点](https://www.unicode.org/reports/tr29/)

## 与主核 / AI 的关系

「算子链 + 共享上下文」与主核（多模态数据库内核）的**执行器节点（plan node）**是同一类结构：都是「按顺序跑一串算子，每个算子读上一环节的输出」。而两者的差异在**契约的强度**：主核的节点有明确的 `TupleTableSlot` 契约（谁产出 slot、谁消费 slot、slot 的所有权归谁），而这里的 `ChunkContext` 只有字段名的约定。

**这个差异的后果很具体**：主核的 `ExecProcNode` 保证「每个节点恰好被调用一次以获取一行」，而这里没有任何机制防止「忘记挂预处理算子」——于是 `TextAfterPreprocess` 是空串，静默产出零块。**判据是「槽位的生产者是否被强制声明」**：声明了则可检查，只靠命名则不可。

第二处对照是 `mergeChunks` 的 `bufMeta = c.Metadata` 引用赋值与主核的**元组拷贝与最小拷贝语义**。主核的 `ExecCopySlotMinimalTuple` 在有引用需要共享时是**显式**的（函数名就说了），而在需要独占时必须 `heap_copytuple`。这里没有任何标记区分「共享」与「独占」，于是共享 `Metadata` 变成了意外。**判据是「值类型里是否嵌了引用类型」**：嵌了就必然有共享，必须显式拷贝。

第三处是「rune 而不是字节」的贯穿与主核的**字符集与编码处理**。主核在 `varchar(n)` 里按字符计长度（`pg_mbstrlen`），正是同一考虑。而两处都必须在所有相关计算点保持一致——`mergeChunks` 里那段「`buf.Len()` 是字节数，中文膨胀 3 倍」的注释，就是这条纪律被违反过一次的证据。

对 RAG 工程的实际意义在于：分块的实现细节决定了检索质量的上限，而它的错误往往静默。这份实现给出的三条经验可直接用：**长度单位必须全局统一在字符数**（字节数在中英混排里会系统性地错）、**规格化（重编号 + 重算长度）要作为独立阶段**（这样其它阶段可以不必各自正确）、以及**配置项解析了却不用是最危险的静默缺陷**（`boundaries` 与 `keep_separators` 都在打印自己而不生效）。
