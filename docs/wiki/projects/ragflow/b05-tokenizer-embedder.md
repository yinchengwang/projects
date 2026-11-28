---
project: ragflow
feature: tokenizer-embedder
title: 分词与嵌入组件
page_type: feature
schema_version: 1
refs:
  - path: internal/ingestion/component/tokenizer.go
    symbols: [TokenizerComponent, Embedder, EmbeddingResult]
  - path: internal/ingestion/component/tokenizer.go
    symbols: [NewTokenizerComponent]
---

# 分词与嵌入组件

## 一句话职责

给块算两种「可检索表示」：全文检索用的**词元字段**（正文/标题/关键词/问句各一套 token 与其细粒度变体），以及向量检索用的**嵌入向量**（内容向量按标题权重合并成最终向量），并把两者挂到块的字段上，最后校验「该有的都有」。

## 机制是什么

这个组件是「文本 → 检索表示」的转换器，而它的复杂度全在**两个检索方法各自的隐性契约**上：

| 方法 | 产出 | 契约 |
|---|---|---|
| `full_text` | `content_ltks` / `content_sm_ltks` / `title_tks` / `important_tks` / `question_tks` 等 | 词元必须与检索时的分析器**同语言**，否则匹配不到 |
| `embedding` | 每块一个 `q_<dim>_vec` 字段 | 向量必须来自**数据集配置的那个模型**，否则检索时不可比 |

第二个契约是本页最重的部分。它的核心是**三个可选接口**（`Embedder` 必修，`EmbedderTrimmer` / `EmbedderMaxResolver` 可选）：

```go
type Embedder interface {
	MaxTokens() int
	BatchSize() int
	Encode(ctx context.Context, texts []string) ([]EmbeddingResult, error)
}

type EmbedderTrimmer interface {
	Trim(text string) (trimmed string, tokens int)
}

type EmbedderMaxResolver interface {
	ResolveMaxTokens() int
}
```

而注释把「为什么需要 `EmbedderTrimmer`」说得非常具体：

> When present the component uses it for every truncation, because cl100k and a model's own tokenizer disagree by up to ~2% and **the sign of the disagreement depends on the content**: a chunk that cl100k scores just under the limit can be over it in the model's tokenizer, **which the provider answers with a 400 and, before this, the loss of the whole document.**

**这是本页最重要的一条事实**：截断若用错分词器，会有一个 2% 的误差，而**误差方向不确定**。所以「按 cl100k 数刚好不到上限」的块，在真模型的分词器下可能超限——超限的请求被 provider 拒绝（400），而在引入 `EmbedderTrimmer` 之前，**一个块的超限会导致整个文档失败**。

**为什么是「整个文档」而不是「那个块」？** 因为批处理：如果一批里有一个超限输入，provider 拒绝整批（注释在别处写得很清楚：「A provider rejects the WHOLE batch when any input carries no content token」）。

**所以这个可选接口的价值不在精度，而在可用性**——它把「偶发的一个长块毁掉一个文档」变成「截断一点内容」。

## 怎么实现的

### 分词路径：五个字段各自独立分支

```go
func tokenizeChunks(chunks []schema.ChunkDoc, titleStem string, language string) error {
	tok := tokenizer.New(language)
	for i := range chunks {
		ck := &chunks[i]
		titleTk, err := tok.Tokenize(titleStem)
		...
		ck.TitleTks = titleTk
		ck.TitleSmTks = titleSmTk

		if q := ck.Questions; q != "" { ... }
		if kw := ck.Keywords; kw != "" { ... }
		if s := strings.TrimSpace(ck.Summary); s != "" { ... } else if t := ...; ... { ... }
	}
	return nil
}
```

五处：

1. **`tok` 在循环外构造一次** —— 因为 `tokenizer.New(language)` 会建词干分析器（Snowball），构造成本高。这也意味着**语言在一次运行内固定**。

2. **`titleStem` 是「文件名去扩展名 + 从块里声明的标题 token」**，在 `Invoke` 里算：

```go
titleStem := titleExtRE.ReplaceAllString(name, "")
if toks := declaredTitleTokens(chunks); len(toks) > 0 {
	titleStem = strings.TrimSpace(titleStem + " " + strings.Join(toks, " "))
}
```

   **所以「标题」不是文件名本身，而是「文件名 + 块里已有的标题信息」**。这样同一个文档下的所有块共享同一份标题 token，检索时用标题词能命中全篇。

3. **每个可选字段独立判空** —— 所以「有问句没关键词」是合法组合。

4. **关键词按「英文逗号」切，且保留空元素**：

```go
// Split keywords on the ENGLISH COMMA ONLY. The keyword_prompt
// contract specifies "delimited by ENGLISH COMMA", so CJK commas,
// semicolons and newlines stay part of the keyword rather than
// acting as separators. strings.Split also preserves empty
// elements, matching "a,,b".split(",") == ["a","","b"]
rawParts := strings.Split(kw, ",")
kwdParts := make([]string, len(rawParts))
for i, part := range rawParts {
	// Empty parts are intentionally preserved to match the contract
	// tested by TestTokenizerComponent_ImportantKwd_PreservesEmptyElements.
	kwdParts[i] = sanitizeKeywordTerm(part)
}
```

   **四处**：

   - **只认英文逗号** —— 中文逗号、分号、换行都留在关键词内部。理由是上游 prompt 明确约定「delimited by ENGLISH COMMA」。
   - **保留空元素** —— `"a,,b"` 切成三个元素，中间是空串。注释说这是「to match the contract」，且有测试锁住。
   - **`sanitizeKeywordTerm` 逐项净化** —— 见下。
   - **这一处「保留空元素」是个很值得记的决定**：从数据质量看空元素是垃圾，但从「与 Python 版逐字节对齐」看它是必须的（因为数组元素数不同会让序列化结果不同）。

5. **摘要优先、正文兜底** —— 而这里有一处与 Python 版的有意分歧：

```go
// Keep Go: skip whitespace-only summaries so they don't shadow
// the real Text. truthy check treats
// "   " as present and blanks out content_ltks; Go is more sensible.
if s := strings.TrimSpace(ck.Summary); s != "" {
```

   **Python 版的 `if ck["summary"]:` 会把只含空格的摘要当成「有摘要」**，于是用它（空白的）替换掉 `content_ltks`，把真正的正文 token 抹掉。Go 版用 `TrimSpace` 修掉了。

   注释明确标注「Keep Go」——这是一处**有意不追求对齐**的地方，而且理由充分：对齐一个 bug 没有意义。

**而「tokenize 返回空则回退到原文」这个模式出现了四次：**

```go
if st == "" {
	st = s
}
ck.ContentLtks = st
```

**四次**（标题、问句、关键词、正文都有类似的兜底或至少正文有）。**理由是「空的词元字段等于不可检索」**，而回退到原文至少让「按原文子串匹配」还能工作。

### `sanitizeKeywordTerm`：ES 字段长度硬限制

```go
const esKeywordMaxTermBytes = 32766

func sanitizeKeywordTerm(term string) string {
	term = strings.TrimSpace(term)
	if term == "" { return "" }
	if len(term) <= esKeywordMaxTermBytes { return term }
	length := 0
	end := 0
	for _, r := range term {
		size := utf8.RuneLen(r)
		if length+size > esKeywordMaxTermBytes { break }
		length += size
		end += size
	}
	return strings.TrimRightFunc(term[:end], unicode.IsSpace)
}
```

五处：

1. **32766 是 Lucene/ES 的 `BytesRef` 上限（32766 = 32768 - 2）** —— 超过它的 keyword 字段会被引擎拒绝。

2. **按 rune 累加而不是直接 `term[:n]`** —— 因为必须**在 UTF-8 字符边界截断**。若按字节硬切，会产生一个「半个汉字」的非法 UTF-8 序列。

3. **`end` 累加的是「已接受的字节数」，而 `term[:end]` 是按字节偏移切片** —— 这是对的，因为 rune 累加的正好是前缀的字节长度。

4. **`TrimRightFunc(term[:end], unicode.IsSpace)`** —— 截断后去掉尾随空格。因为切断可能正好落在空格之后。

5. **返回空串而不是错误** —— 所以一个超长关键词会被静默截短。**这是唯一的选择**：报错会让整篇文档失败，而截短只是丢一点关键词信息。

### `cleanEmbedText`：先把表格标签洗掉再判空

```go
var tableStructureTagRe = regexp.MustCompile(`</?(table|td|caption|tr|th)( [^<>]{0,12})?>`)

// tableStructureTagRe mirrors the tokenizer's cleaned_txt step: table-structure tags are replaced
// with a space before the emptiness check and before the text is embedded,
// so a chunk that is nothing but an empty table carries no embeddable
// content. The attribute group is deliberately bounded to 12 non-tag
// characters, exactly like the pattern: a tag with longer attributes
// survives the strip. Do not "fix" the bound — it is parity, not a bug.
func cleanEmbedText(raw string) string {
	return strings.TrimSpace(tableStructureTagRe.ReplaceAllString(raw, " "))
}
```

四处：

1. **`{0,12}` 这个上界是刻意的** —— 注释明确：「a tag with longer attributes survives the strip. Do not "fix" the bound — it is parity, not a bug」。

   **这是一处「不要修」的注释**：它知道这个正则不完美（带长属性的 `<table class="..." data-...>` 不会被剥掉），但因为要与 Python 版一致，所以保留。

   这条注释的价值在于**挡住未来某位维护者的「善意修复」**——修了会让 Go 版与 Python 版产出不同结果。

2. **替换成空格而不是删除** —— 避免把 `<td>a</td><td>b</td>` 变成 `ab`（会合并两个格的内容）。

3. **清洗后的文本既是「是否为空」的判据，也是真正嵌入的内容** —— 注释：「The stripped text is both the emptiness check and what gets embedded」。

   **这让「空表格块」既不进批也不留空向量**，是一致的行为。

### 嵌入路径：七段，每段都有一个具体的失败模式在防守

**第一段：解析器与模型。**

```go
resolver := c.resolver
if resolver == nil {
	resolver = DefaultEmbedderResolver
}
if resolver == nil {
	return nil, 0, fmt.Errorf("tokenizer: embedding requested but no embedder resolver configured")
}
embedder, embdID, err := resolver(ctx, tenantID, kbID)
```

两处：

1. **两级回退**：显式（仅测试）→ `DefaultEmbedderResolver`（由组合根在 `init()` 注入）→ 报错。

   **`DefaultEmbedderResolver` 是包级变量且「在本叶包里为 nil」**，注释说明了原因：

> It is nil in this leaf package — which must not import internal/service (see the EMBEDDING MODEL RESOLUTION note above) — and is injected by the composition root

   **这是「分层依赖不能反向」的具体代价**：这个包需要嵌入器，但嵌入器的解析需要 `internal/service`，而叶包不能依赖它。所以用「组合根注入」打破。

2. **`embdID` 是「数据集绑定的嵌入模型 id」** —— 注释在 `EmbedderResolver` 类型上写得很清楚：

> The returned embdID is the stable string the tokenizer keys its per-chunk embedding cache on, so a KB whose embedding model changes yields a different key and never serves a stale vector.

   **「模型换了 → 键变了 → 不会返回旧向量」**。这与 b03 的「空 llm_id 必须解引用」是同一条纪律（缓存键必须反映真实生产者）。

**第二段：逐块准备文本，三路跳过。**

```go
raw := concatFields(ck, c.param.Fields)
txt := cleanEmbedText(raw)
if txt == "" {
	if raw == "" {
		fieldsResolvedNoText++
	}
	continue
}
```

三处：

1. **`concatFields` 按配置的 `fields` 拼接** —— 所以「嵌什么」是可配的（正文、问句、关键词、摘要、或任意额外字段）。

2. **两段判空区分两种原因** —— 注释写得很细：

> Two different causes reach this branch and only one is a defect: no configured field produced text at all (misconfiguration), versus a chunk that really is only table tags or whitespace (the supported empty-table case, which must stay silent). Only a literally empty `concatFields` result is the first — whitespace-only content is still content a field produced.

   **`raw == ""` 才计为「配置问题」**，因为纯空白说明字段确实有内容。所以「空表格」不报警，「配置的字段全空」才报警。

3. **`continue` 让该块不进批** —— 所以它的向量为空，而 `validateTokenizerOutputs` 会因此放行（见下）。

**第三段：截断，而截断结果为空时的兜底很关键。**

```go
trunc := trimForEmbedding(embedder, txt)
// A provider rejects the WHOLE batch when any input carries no content
// token — vLLM answers 400 "Input validation error: `inputs` cannot
// be empty" — so an empty input must never reach the batch. The
// EmbedderTrimmer seam promises nothing about the result, so a trimmer
// that returns no text for non-empty input is handled by embedding the
// pre-trim text: that keeps this chunk's vector requirement intact
// (validateTokenizerOutputs decides embeddability on the pre-trim text)
// and lets the embedder's own over-limit ladder re-cut it if the
// untrimmed text is too long.
if strings.TrimSpace(trunc) == "" {
	emptyAfterTrim++
	trunc = txt
}
```

四处：

1. **「空输入绝不能进批」是硬约束** —— 注释给了具体的错误码（vLLM 的 `Input validation error: inputs cannot be empty`）。

2. **`EmbedderTrimmer` 契约不保证返回非空** —— 所以「trimmer 对非空输入返回空」是一个必须处理的路径。处理方式是**嵌入未截断的原文**。

3. **这个兜底是双赢的**：它保住该块的向量要求（`validateTokenizerOutputs` 按**截断前**的文本来判「该不该有向量」），同时让嵌入器自己的「超限阶梯」重新切。

   **注意「按截断前的文本判」这个设计很关键**：若校验也按截断后判，那么一个被 trimmer 清空的块会被认为「本来就不该有向量」——而实际它有内容。

4. **`emptyAfterTrim` 单独计数并报警** —— 因为这是「trimmer 行为异常」的信号，不报警就永远发现不了。

**第四段：每块缓存，键包含「实际嵌入的文本」。**

```go
if chunkID, ok := ck.GetExtraString("id"); ok && embdID != "" && store != nil {
	if cached, hit := chunkcache.Get(ctx, store, chunkcache.Key("emb", embdID, chunkID, trunc)); hit {
		...
	}
}
```

**注释解释了为什么键里要带文本**：

> The key includes the text that is actually embedded — after field selection and truncation — not just the chunk id. A tokenizer-config or model change alters the embedded input; reusing the prior vector for that case would serve a stale embedding for up to the cache TTL. embdID must be non-empty or every model would collapse onto one key and served vectors could come from a different model.

**三处**：

1. **键 = (embdID, chunkID, trunc)** —— 三个都必要：模型换了、块换了、嵌入输入变了，任一变化都必须重算。
2. **`embdID` 为空会让所有模型撞一个键** —— 与 b03 的「空 llm_id」是同一个陷阱。
3. **`store != nil` 才查缓存** —— 没配 redis 时缓存降级为 no-op，注释在别处说「every chunk is embedded normally」。

**第五段：标题向量只编一次。**

```go
if trimmedName == "" {
	common.Warn("Tokenizer: empty name provided from upstream, embedding will skip title weighting")
} else {
	finish := runtime.TrackIO(ctx, "Tokenizer", "Embedding title", "chunks=1")
	titleResults, err := embedder.Encode(ctx, []string{name})
	...
}
```

**而这段前面有一段「不要对齐」的注释，是本页第二重要的判断：**

```go
// go_intentional (A3): when the upstream name is empty we skip title
// weighting entirely (hasTitleVec stays false, so the merged vector is the
// content vector alone). From the end-user perspective an empty title must
// not contribute the filename embedding weight; the DSL instead
// computes 0.1*emb(""), injecting an undefined bias into every chunk. Go's
// skip is the correct behavior (go_intentional). Do NOT "align" this to
// the DSL.
```

**逻辑链**：Python DSL 版在文件名为空时仍会算 `0.1 * emb("")`，而 `emb("")` 是一个**没有定义的向量**——于是「空标题」会给每个块的向量注入一个固定的偏置。Go 版选择完全跳过标题加权。

**「emb("") 是未定义行为」这一点很具体**：多数嵌入服务对空串会返回全零或报错，而无论哪种，「0.1 倍的全零」只是无害的（但若是报错，整个文档失败）。真正的问题是**它让「空标题文档」与「无标题加权文档」的向量不可比**。

**而标题编码用的是 `name` 原文（不 TrimSpace）**：

> Encode the raw name verbatim (no TrimSpace) so the embedding matches the upstream tokenizer contract, which passes the name through without trimming. The empty-name guard above still uses TrimSpace to match the upstream empty-name check.

**「判空用 TrimSpace、编码用原文」是一处精细的对齐**——两个地方用的是不同的判定，因为它们对齐的是 Python 版的两处不同行为。

**第六段：批处理循环，每批都校验返回向量数。**

```go
batchSize := embedder.BatchSize()
if batchSize <= 0 {
	return nil, 0, fmt.Errorf("tokenizer: embedder reported non-positive batch size %d", batchSize)
}
embedTotal := cacheHits + len(texts)
for start := 0; start < len(texts); start += batchSize {
	...
	batchResults, err := embedder.Encode(ctx, texts[start:end])
	...
	if len(batchResults) != end-start {
		return nil, 0, fmt.Errorf("tokenizer: encode returned %d vectors for %d chunks", len(batchResults), end-start)
	}
```

四处：

1. **`batchSize <= 0` 显式报错** —— 因为 `start += batchSize` 会死循环（0）或倒退（负）。
2. **`embedTotal = cacheHits + len(texts)`** —— 进度基数是「所有块」，不是「本批」。所以进度条覆盖缓存命中的部分。**这解释了一个后续的特殊分支**（见下）。
3. **返回数量校验** —— 少一条就报错。因为向量要按 `pairs[i]` 回填到 `chunks[idx]`，数量不对会错位。
4. **进度上报 `ReportComponentFraction` + `ReportProgressDetail`** —— 分数与离散位置（`chunk:embed`）都报，与 b02 的 PDF 页进度是同一手法。

**第七段：合并与回填。**

```go
merged := append([]float64(nil), re.content...)
if hasTitleVec {
	merged, err = mergeEmbeddingVectors(titleVec, re.content, titleWeight)
}
chunks[i].SetExtraValue(common.VectorColumnName(len(merged)), merged)
```

四处：

1. **`append([]float64(nil), re.content...)` 先拷贝** —— 因为「没标题向量」时 `merged` 就是这个内容向量。若不拷贝，`chunks` 会持有缓存里的同一底层数组，后续改动会污染缓存。**这是防御性拷贝**，而 `mergeEmbeddingVectors` 内部会新建数组，所以只在 `hasTitleVec == false` 时需要。

2. **向量字段名由维度决定**（`common.VectorColumnName(len(merged))`）—— 所以「维度」进入了字段名。这解释了为什么不能「降级用别的模型」：字段名会变，检索侧找不到。

3. **`mergeEmbeddingVectors` 做维度校验** —— 标题向量与内容向量维度不一致就报错（不同模型混用会走到这里）。

4. **回填缓存只对「新嵌入」的块做** —— 注释：「Backfill the per-chunk cache only for freshly embedded content; cache hits are left untouched. The key intentionally omits title weighting: the content vector is title-weight independent, so identical chunk content reuses across runs even when only the filename weight changes.」

   **「键不含标题权重」是一处正确的设计**：缓存存的是**内容向量**（不含标题），而标题加权在每次运行时现算。所以改 `filename_embd_weight` 不会让缓存失效——这是对的，因为权重变化不需要重嵌入。

### 校验：诚实地区分「该有没有」

```go
func validateTokenizerOutputs(chunks []schema.ChunkDoc, searchMethods, fields []string, kbID string) error {
	needFullText := slices.Contains(searchMethods, "full_text")
	needEmbedding := shouldHaveEmbedding(searchMethods, kbID)
	if !needFullText && !needEmbedding { return nil }
	for i := range chunks {
		if needFullText && requiresFullTextTokens(chunks[i]) {
			if strings.TrimSpace(chunks[i].ContentLtks) == "" || ... {
				return fmt.Errorf("tokenizer: chunk[%d] missing full_text tokens", i)
			}
		}
		if needEmbedding && requiresEmbeddingVector(chunks[i], fields) {
			if !hasEmbeddingVector(chunks[i]) {
				return fmt.Errorf("tokenizer: chunk[%d] missing embedding vector", i)
			}
		}
	}
	return nil
}
```

四处：

1. **`if !needFullText && !needEmbedding { return nil }` 在最前** —— 两种方法都不用时**完全跳过校验**。所以「只要 chunk 结构」的管线不会因为空字段失败。

2. **`requiresFullTextTokens` 与 `requiresEmbeddingVector` 是「该不该有」的判据**：

```go
func requiresFullTextTokens(ck schema.ChunkDoc) bool {
	return strings.TrimSpace(ck.Summary) != "" || strings.TrimSpace(schema.ContextualText(ck)) != ""
}
```

   **即「有摘要或正文才有词元要求」**。

   而 `requiresEmbeddingVector` 的注释说明了它必须用与嵌入路径**同一个清洗**：

> requiresEmbeddingVector uses the same cleaning as the embed path (cleanEmbedText): a chunk whose cleaned text is empty was skipped from the embedding batch and therefore legitimately carries no vector.

   **「校验与执行的判据必须一致」** —— 若校验用未清洗的文本，那么一个「只有表格标签」的块会被要求有向量，而它永远不会被嵌入（因为清洗后为空），于是校验必然失败。

3. **`shouldHaveEmbedding` 里有一个 `kbID != ""` 的守卫，注释标了 `go_intentional (A4)`**：

> Each dataset configures its own embedding model, so an empty kb_id (e.g. a canvas-debug dry run) must NOT fall back to the tenant's default embedding model — doing so would produce vectors a dataset cannot actually use at retrieval time. This is a deliberate, go_intentional divergence. Do NOT "align" this to a path that injects a default embedding.

   **「调试运行不嵌入」不是省事，而是因为产出的向量**在检索时**用不上**——它来自一个数据集没配的模型。

4. **报错信息带块下标** —— 所以失败能定位到具体块，而不是「某个块缺向量」。

### `EmbeddingResult`：把 token 用量带回来

```go
type EmbeddingResult struct {
	Vector     []float64
	TokenCount int
}
```

而 `embedChunks` 返回 `([]schema.ChunkDoc, int, error)` —— 第二个是**累计 token 消耗**（标题 + 所有内容批），最终写进 `out["embedding_token_consumption"]`。

**「模型报的用量」而不是「自己数的用量」** —— 所以计费/统计用的是 provider 的值。这与 d02 里「prompt_tokens 靠自己 encode 数」形成对比（那里没有权威来源）。

## 关键取舍

**源码事实：** `Embedder` 是必修接口（`MaxTokens` / `BatchSize` / `Encode`），`EmbedderTrimmer` 与 `EmbedderMaxResolver` 是可选扩展（类型断言探测）；`trimForEmbedding` 有 trimmer 就用它，否则用 cl100k 截断。

**源码事实：** cl100k 与模型自己的分词器**最多差约 2% 且误差方向依内容而变**；超限的请求被 provider 拒（400），而在有 `EmbedderTrimmer` 之前这会**毁掉整个文档**（因为批里一条超限拒整批）。

**源码事实：** 未实现 `EmbedderMaxResolver` 时退到 `embedder.MaxTokens()`；注释明确「猜 8192 会超出所有窗口更小的模型」。

**源码事实：** `tokenizeChunks` 在循环外构造一次 tokenizer（词干分析器构造成本高），语言在一次运行内固定。

**源码事实：** `titleStem` = 文件名去扩展名 + 由块声明的标题 token 拼接，全篇共享。

**源码事实：** 关键词只按**英文逗号**切（中文逗号/分号/换行留在关键词内，因为上游 prompt 约定「delimited by ENGLISH COMMA」），且**保留空元素**（`"a,,b"` → 三元素，有测试锁住）。

**源码事实：** `sanitizeKeywordTerm` 上限 32766 字节（ES keyword 字段上限），按 rune 累加以在 UTF-8 边界截断，截后 `TrimRightFunc` 去尾空格，超长时静默截短而非报错。

**源码事实：** `tableStructureTagRe` 的属性匹配上界 `{0,12}` 是刻意保留的（带长属性的标签不会被剥），注释明写「Do not "fix" the bound — it is parity, not a bug」；替换为空格避免合并相邻格内容；清洗后的文本同时用作「判空」与「实际嵌入内容」。

**源码事实：** 「摘要只含空白」时 Go 版跳过（用 `TrimSpace`），而 Python DSL 版的 truthy 判断会把空白摘要当有值、抹掉正文 token；注释标 `Keep Go`，是有意不对齐。

**源码事实：** `DefaultEmbedderResolver` 在本叶包内为 nil（因为不能依赖 `internal/service`），由组合根在 `init()` 注入；显式 resolver 仅测试用。

**源码事实：** `embdID` 是数据集绑定的嵌入模型 id，用作每块缓存键的一部分——数据集换模型会得到不同键，永不复用旧向量。

**源码事实：** 空嵌入输入**绝不能进批**（vLLM 返回 400 `inputs cannot be empty`）；`EmbedderTrimmer` 契约不保证返回非空，所以 trimmer 对非空输入返回空时**改为嵌入未截断的原文**，且 `validateTokenizerOutputs` 按**截断前**文本判「该不该有向量」。

**源码事实：** 每块缓存键 = `("emb", embdID, chunkID, 实际嵌入文本)`；键**不含标题权重**，因为缓存存的是内容向量、标题加权每次现算。

**源码事实：** 文件名空时**完全跳过标题加权**（`go_intentional A3`），而 Python DSL 会算 `0.1*emb("")` 注入未定义偏置；注释明确「Do NOT "align" this to the DSL」。标题编码用原文（不 TrimSpace），而判空用 TrimSpace（分别对齐 Python 两处行为）。

**源码事实：** `batchSize <= 0` 显式报错（否则循环死循环/倒退）；每批校验返回向量数等于输入数（否则回填会错位）；进度基数 `cacheHits + len(texts)`，缓存全命中时批循环不跑，需单独上报终值（`finding ④`）。

**源码事实：** 无标题向量时先 `append([]float64(nil), re.content...)` 拷贝，避免 `chunks` 持有缓存底层数组。

**源码事实：** 向量字段名含维度（`common.VectorColumnName(len(merged))`），所以「换模型降级」会让检索侧找不到字段；`mergeEmbeddingVectors` 校验标题与内容向量维度一致。

**源码事实：** `validateTokenizerOutputs` 在两种检索方法都不需要时**整体跳过**；`requiresEmbeddingVector` 用与嵌入路径相同的 `cleanEmbedText`，注释说明「清洗后为空被跳过，因此合法地没有向量」。

**源码事实：** `shouldHaveEmbedding` 要求 `kbID != ""`（`go_intentional A4`）——调试运行不嵌入，因为产出的向量在检索时用不上。

**架构推论：** 这个组件的核心防御对象是**「一个异常输入毁掉整批」**。三条不同的防线都指向同一个失败模式：空输入不进批、超限用模型自己的分词器截断、trimmer 出错退回原文。这个模式在嵌入 API 里极常见（批处理是性能的必要条件，而批处理放大了单条输入的风险）。**判据是「批是否原子」**：若一批全成或全败，则任何单条异常都必须被前置消除。

**架构推论：** 三处 `go_intentional` 标注（A3 空标题、A4 无 kb_id、cleanEmbedText 的 `{0,12}`）构成一份**「有意不对齐」的清单**。这类清单的价值极高：它把「看起来像 bug 但与上游一致」与「真的该修」区分开。没有它，维护者会不断「顺手修好」从而破坏跨实现一致性；有了它，修复必须显式推翻一条已记录的判断。

**架构推论：** 缓存键的三要素（模型 id、块 id、实际嵌入文本）体现了缓存键的通用原则：**必须包含所有影响输出结果的输入**。这里特别值得注意的是「含截断后的文本而非块 id」——因为块的嵌入输入取决于 `fields` 配置与截断策略，同一个块在不同配置下要产生不同向量。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/ingestion/component/tokenizer.go` | `TokenizerComponent` | 分词 + 嵌入组件 |
| `internal/ingestion/component/tokenizer.go` | `NewTokenizerComponent` / `NewTokenizerComponentWithResolver` | 构造（生产 / 带测试 resolver） |
| `internal/ingestion/component/tokenizer.go` | `TokenizerComponent.Invoke` | 主流程：解码 → 序号 → 分词 → 嵌入 → 校验 |
| `internal/ingestion/component/tokenizer.go` | `Embedder` | 必修接口（上限 / 批大小 / 编码） |
| `internal/ingestion/component/tokenizer.go` | `EmbedderTrimmer` | 可选：用模型自己的分词器截断 |
| `internal/ingestion/component/tokenizer.go` | `EmbedderMaxResolver` | 可选：解析真实窗口大小 |
| `internal/ingestion/component/tokenizer.go` | `EmbeddingResult` | 向量 + provider 报的 token 用量 |
| `internal/ingestion/component/tokenizer.go` | `EmbedderResolver` / `DefaultEmbedderResolver` | 组合根注入的模型解析器 |
| `internal/ingestion/component/tokenizer.go` | `trimForEmbedding` / `resolveEmbedderMaxTokens` | 两层能力探测 |
| `internal/ingestion/component/tokenizer.go` | `EmbedderResolver` 注释块 | 模型永远来自数据集 `embd_id`，不由 DSL 配置 |
| `internal/ingestion/component/tokenizer.go` | `sanitizeKeywordTerm` | ES keyword 字段的字节上限截断 |
| `internal/ingestion/component/tokenizer.go` | `cleanEmbedText` / `tableStructureTagRe` | 剥表格标签（`{0,12}` 边界为对齐保留） |
| `internal/ingestion/component/tokenizer.go` | `embedChunks` | 七段嵌入（解析 → 准备 → 截断 → 缓存 → 标题 → 批处理 → 回填） |
| `internal/ingestion/component/tokenizer.go` | `truncateForEmbedding` | cl100k 兜底截断 |
| `internal/ingestion/component/tokenizer.go` | `mergeEmbeddingVectors` | 标题权重线性合并 + 维度校验 |
| `internal/ingestion/component/tokenizer.go` | `tokenizeChunks` | 五个字段各自独立分支 |
| `internal/ingestion/component/tokenizer.go` | `declaredTitleTokens` | 块里声明的标题 token |
| `internal/ingestion/component/tokenizer.go` | `concatFields` | 按配置字段拼接嵌入输入 |
| `internal/ingestion/component/tokenizer.go` | `shouldHaveEmbedding` | 是否需要嵌入（含 `kbID != ""` 守卫） |
| `internal/ingestion/component/tokenizer.go` | `validateTokenizerOutputs` | 输出完整性校验（与嵌入路径同一清洗判据） |
| `internal/ingestion/component/tokenizer.go` | `requiresFullTextTokens` / `requiresEmbeddingVector` | 「该不该有」的判据 |
| `internal/ingestion/component/tokenizer.go` | `decodeTokenizerFromUpstream` | 上游解码 |
| `internal/ingestion/component/tokenizer.go` | `chunksFromTokenizerUpstream` / `textPayloadToChunks` | 纯文本 payload 兜底成块 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 文档：向量与全文混合检索](https://ragflow.io/docs/dev/configure_knowledge_base)
- [论文：MTEB 嵌入模型评测与维度差异](https://arxiv.org/abs/2210.07316)
- [Elasticsearch 文档：keyword 字段与字节上限](https://www.elastic.co/guide/en/elasticsearch/reference/current/keyword.html)

## 与主核 / AI 的关系

嵌入维度进入字段名（`q_<dim>_vec`）这件事，与主核（多模态数据库内核）的**类型系统**是同一个机制：数据的物理表示参与了它的身份。主核里 `int4` 与 `int8` 是同名列的不同类型，`ALTER TYPE` 需要重写表；这里是「换模型 = 换字段名 = 检索侧找不到」。

**这个设计是对的**：它让「模型不匹配」变成**显式的找不到**，而不是「用 768 维向量去和 1024 维索引比对」这种静默错误。判据是「错误要显式还是静默」——维度进字段名把静默错误变成了显式缺失。

第二处对照是 `EmbedderTrimmer` 与主核的**精度与范围校验**。数据库在写入前校验值是否落在列类型范围内（`numeric` 的精度/标度、`varchar(n)` 的长度），用的是**声明的元数据**；这里的 cl100k 是一个**近似的替代品**，误差 2% 且方向不定。所以它不是「校验」，而是「猜」，而猜错的代价是整批被拒。

**这就是为什么这个可选接口值得单独存在**：它把「猜」换成了「问」。判据是「是否存在权威的本地实现」——有则委托，无则近似并留出安全边际。

第三处是 `go_intentional` 标注与主核里**兼容性注释**（`/* XXXX: ... */`）。两者都是「此处行为看似诡异但有原因」的标记。差别在主核的注释通常指向一个标准条款或历史邮件线程，而这里的注释指向「与 Python DSL 版的对齐」——一个**内部实现**而不是外部契约。

对 RAG 工程的实际意义在于：嵌入是 RAG 里唯一「一次失败会放大到整篇文档」的环节。这份实现给出的三条经验可直接用：**批处理的失败粒度必须与输入的失败粒度对齐**（要么逐条重试，要么前置消除所有可预见的单条失败）、**截断要用目标模型自己的分词器**（近似分词器的误差方向不定）、以及**缓存键要含实际输入而非块标识**（同一个块在不同配置下产出不同向量）。
