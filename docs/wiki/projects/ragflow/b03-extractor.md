---
project: ragflow
feature: extractor
title: 实体关系抽取组件
page_type: feature
schema_version: 1
refs:
  - path: internal/ingestion/component/extractor.go
    symbols: [ExtractorComponent, extractorJob]
  - path: internal/ingestion/component/extractor_tag.go
    symbols: [OrderedTagWeights]
---

# 实体关系抽取组件

## 一句话职责

对分块列表做**可组合的 LLM 富化**：按配置依次跑「恢复模式 / 关键词 / 标签 / 其余（问句、摘要、元数据）」四个阶段，每个阶段独立开关、独立进度窗口、独立失败策略，最终把结果写回每块的字段上。

## 机制是什么

这个组件看起来只是「调 LLM 抽东西」，但它的复杂度来自一个更根本的问题：**多阶段富化之间的依赖顺序与失败语义**。

四个阶段并不平等：

| 阶段 | 依赖 | 失败策略 | 为什么 |
|---|---|---|---|
| resume（附加模式） | 无 | 硬失败 | 它是外部注入的完整阶段（文件 b06 的 claims 走这里），失败即运行失败 |
| keywords | 无（只用原文） | 池化，单块失败跳过 | 关键词只是增强，缺一块不影响该块的检索 |
| tags | 依赖标题 + **刚抽出的关键词** | 大部分吞掉，最后统一判 `ctx.Err()` | 标签是「锦上添花」，但任务被取消必须能发现 |
| remaining | 依赖前两者 | 池化，单块失败跳过 | 问句/摘要/元数据都是可缺的 |

**顺序不能换**，注释写明了理由：

> The attached mode runs first so its important_kwd and metadata are available to downstream stages without triggering generic summary shadowing.

**「summary shadowing」是这里最具体的约束**：若通用摘要先跑，它写进 `summary` 字段，而附加模式的摘要就会覆盖它（或者反过来）。所以附加模式必须先写，后续阶段要读就拿到真值。

第二阶段（tags）还有另一条依赖：

> Phase 2: Tag phase (if enabled), benefiting from title and freshly extracted keywords.

**即「标签可以引用关键词」**——关键词先出，标签阶段把它们当输入的一部分。

## 怎么实现的

### 构造：开关的三态规则

```go
func resolveExtractToggle(group map[string]any, name string, topN int) (bool, error) {
	v, ok := group["enabled"]
	if !ok {
		return topN > 0, nil
	}
	enabled := false
	if b, ok := v.(bool); ok {
		enabled = b
	} else {
		enabled = mapInt(v) == 1
	}
	if enabled && topN < 1 {
		return false, fmt.Errorf("extractor: %s.enabled requires top_n >= 1", name)
	}
	return enabled, nil
}
```

五处：

1. **键缺失 → `topN > 0`** —— 这是**兼容旧 DSL**：注释说「A group without the key is a legacy DSL: enabled derives from top_n > 0, matching the pre-switch behavior of saved canvases」。所以老画布（没有 `enabled` 键但有 `top_n`）的行为完全不变。

2. **`enabled: true` 且 `top_n < 1` → 报错** —— 注释：「A group carrying an explicit `enabled: true` with `top_n < 1` is a contradictory config and fails here」。**这是「显式矛盾必须报错」的判断**：用户说「要抽但抽 0 个」，那不是「关掉」而是「配错」。

3. **`enabled` 接受 bool 与数值** —— `mapInt(v) == 1`。所以 `1`/`true` 都开，`0`/`false`/`2` 都关。**注意 `2` 也关**（因为 `== 1` 而不是 `!= 0`）。这与 Python 的 `bool(2)` 为真不同——是一处刻意的严格。

4. **错误在构造期返回** —— 与 b02 的 `Check()` 同一原则：配置错误变成画布编译失败，而不是运行时静默跳过。

5. **`resolveExtractToggle` 只用于 keywords/questions/tags** —— summary 与 metadata 直接读 `enabled`（见构造器里 `if v, ok := sumRaw["enabled"].(bool); ok`）。**所以这两组的开关不支持数值**。

   这个不对称大概来自演化顺序：先有 summary/metadata（只认 bool），后来给前三组统一了三态规则。

### 四种富化各自的目标

**关键词 / 问句 / 摘要**是三种「文本衍生字段」，各自的 prompt 与写入字段不同：

```go
func (c *ExtractorComponent) runAutoKeywords(ctx context.Context, db *gorm.DB, in extractorInputs, ck map[string]any, chunkText string) error
func (c *ExtractorComponent) runAutoQuestions(ctx context.Context, db *gorm.DB, in extractorInputs, ck map[string]any, chunkText string) error
func (c *ExtractorComponent) runAutoSummary(ctx context.Context, db *gorm.DB, in extractorInputs, ck map[string]any, chunkText string) error
```

**三个函数签名完全相同** —— 所以它们的差别全在内部：prompt、解析、写入字段。这是「同一模式的三次实例化」，而共用签名让它们可以被同一个池化器调度。

**摘要那个 prompt 里有一段很硬的要求**：

```
Create a concise and faithful summary of the provided text content.

## Requirements
- The summary MUST strictly rely on the provided text without hallucinating.
- The summary MUST be in the same language as the original text content.
- Be concise and focus on the main ideas, omitting redundant details.
- Output summary ONLY.
```

三处：

1. **「MUST strictly rely on the provided text without hallucinating」** —— 摘要的最大风险是模型加入外部知识，所以显式禁止。
2. **「MUST be in the same language as the original text content」** —— 因为摘要若变成英文，中文库的检索就对不上了。**这是「元数据必须与主体同语言」的硬约束。**
3. **「Output summary ONLY」** —— 防止模型输出「以下是摘要：……」这种前言。

### 池化执行：`extractorJob` 与共享池

```go
type extractorJob func() error

var extractorPool = utility.NewWorkerPool[extractorJob, struct{}](
	...
)

const defaultExtractorConcurrency = 10
```

四处：

1. **`extractorJob` 是一个 `func() error`** —— 极简的闭包类型。所以每个任务都是「捕获了上下文的闭包」，而不是带参数的结构体。

2. **池是包级变量** —— 所以**所有 Extractor 实例共享同一个池**。这与 b01「每次运行一个执行器」不同，是刻意的：解析/抽取是 CPU+网络密集型，池化能跨文档复用 worker。

3. **并发默认 10**（`defaultExtractorConcurrency`），而 `extractorConcurrency()` 会读环境变量覆盖。

4. **`awaitFutures(ctx, futs, w)` 统一等待并推进进度**：

```go
func awaitFutures(ctx context.Context, futs []utility.WorkerPoolFuture[extractorJob, struct{}], w progress.Window) error
```

   所以「提交一批任务 + 等全部完成」是唯一的使用模式。**没有流式消费**——这与「每块独立、结果直接写回 chunk map」的设计一致。

### 失败的处理：三种不同的降级

**第一种：单块 LLM 调用失败 → 跳过该块，不失败整批。**

```go
func resolveChunkCallError(ctx context.Context, idx int, phase string, err error, completedAny bool, skipped *atomic.Int64) error
```

```go
func checkAllChunksSkipped(phase string, total int, skipped int64) error
```

**`checkAllChunksSkipped` 的名字说明了判据**：只有「**全部**块都被跳过」才报错。所以一个阶段里有个别块失败是可接受的（那些块的字段保持空），但整阶段全失败说明是系统性问题（模型不可达、配置错），必须失败。

**这是一个很实用的分级**：区分「局部失败」（跳过）与「系统性失败」（报错）。判据是**失败率是否 100%**。

**第二种：标签阶段吞掉所有错误，最后统一检查 ctx。**

```go
// The tag phase swallows per-chunk failures, so a task stopped mid-run
// could otherwise report success. This mirrors the old node-level
// WithTimeout post-check: never report success on a dead context.
if err := ctx.Err(); err != nil {
	return nil, fmt.Errorf("extractor: %w", err)
}
```

**这条注释解释了一个真实的风险**：吞掉错误意味着可能「什么都没做但报成功」，所以必须在最后补一次取消检查。

**这是「宽容的失败处理会掩盖取消」这个陷阱的显式应对**。

**第三种：超时用 `runtime.WithTimeout` 包住单次调用，超时只记错误不失败。**

```go
timeoutErr := runtime.WithTimeout(ctx, taggerTimeout, func(timeoutCtx context.Context) error {
	resp, err := inv.Chat(timeoutCtx, extractorChatRequest{...})
	if err != nil {
		common.Error("extractor tags: LLM call failed", err)
		return nil    // ← 注意：返回 nil 而不是 err
	}
	result = parseTaggerResponse(resp.Content, topN)
	return nil
})
if timeoutErr != nil {
	common.Error("extractor tags: LLM timeout", timeoutErr)
}
```

**两处「返回 nil」是关键**：内层闭包把调用错误吞掉（返回 nil），所以 `WithTimeout` 只在**真超时**时返回非 nil。因此两种失败都以「只记日志」收场，这个块的标签为空。

**这是一处彻底的「标签不阻塞」策略。**

### 标签阶段的两阶段设计：本地匹配优先，LLM 兜底

`runAutoTags` 的结构是一条**分流**：

```go
for _, d := range in.chunks {
	if ctx.Err() != nil { break }
	matched := matchAndTagChunk(d, indexed, tok, topN)
	if matched != nil {
		if isHighConfidenceMatch(matched) {
			examples = append(examples, *matched)
		}
	} else {
		docsToTag = append(docsToTag, d)
	}
}
if err := c.runLLMTags(ctx, db, in, docsToTag, indexed, examples, w); err != nil { ... }
```

四处：

1. **本地匹配成功的块不进 `docsToTag`** —— 所以**不调 LLM**。而本地匹配的产物直接写在 chunk 上（`matchAndTagChunk` 内部就那么做，因为返回 `*schema.TaggedChunk` 但实际也改了 `chunk`）。

2. **高置信匹配额外进 `examples`** —— 所以**同一份文档里已经匹配好的块，会成为后续 LLM 打标签的示范例**。

   **这是一个很聪明的冷启动策略**：不需要外部样例库，文档自己的前几个块就能当 few-shot 例子。

3. **`ctx.Err() != nil` 时 `break` 而不是 `return`** —— 所以取消时会把已处理的部分保留，然后继续跑 `runLLMTags`（那里只有 `docsToTag` 里已收集的部分）。

4. **`runLLMTags` 里 `examples` 与 `indexed.examples` 是两套** —— `llmTagChunk` 优先用前者，空了才用索引里的（注释：「Cold-start fallback: sample real examples deterministically using text hash seed」）。

**本地匹配的算法本身很值得一看**（`matchAndTagChunk`）：

```go
for w, freq := range chunkWordFreq {
	idf, exists := idx.idfs[w]
	if !exists || idf <= 1e-6 { continue }
	tfBoost := 1.0 + 0.3*math.Log(1.0+float64(freq))
	for _, docID := range idx.postings[w] {
		candidateInterIDF[docID] += idf
		candidateTFScore[docID] += idf * tfBoost
		candidateMatchedTokens[docID]++
	}
}
```

四处：

1. **倒排索引 + IDF 累加** —— `candidateInterIDF[docID] += idf` 是「分子」，而分母是该样例的总 IDF（`exTotalIDF`）。

2. **注释说明了 `candidateInterIDF` 的用途**：「Pure IDF sum, ensures coverage <= 1.0」。**即分子用纯 IDF（不带 TF 加权）才能保证覆盖率不超 1。**

3. **`candidateTFScore` 才是带 TF 加权的显著性分数** —— 所以有**两个分数**：一个用于「是否通过阈值」（coverage），一个用于「排序选 top」。

   **这是一个重要的分离**：阈值判据必须可归一化（所以用纯 IDF 比），排序判据可以带权重（用 TF 加权）。

4. **不对称覆盖率**：`Coverage(E) = InterIDF / ExTotalIDF` —— 注释说「asymmetric」，即**分母是样例的 IDF 总量，不是块的**。所以一个很短的块若命中了很长的样例里的高频词，覆盖率仍然低。

**1-token 样例有特殊规则：**

```go
if exTokens >= 2 {
	if matchedTokens < 2 { continue }
} else {
	// For 1-token examples, require high IDF (exTotal >= singleTokenMinIDF)
	if exTotal < singleTokenMinIDF { continue }
}
```

**即：多 token 样例要求至少命中 2 个不同词，单 token 样例要求该词的总 IDF ≥ 1.5。** 两条都是防「一个常见词就匹配到整类标签」的误判。

**而 `singleTokenCoverageThreshold = 0.999`** —— 单 token 样例要求覆盖率几乎为 1（因为只有一个 token，命中就是全命中）。

### `OrderedTagWeights`：给 map 加确定性序列化

```go
type OrderedTagWeights map[string]int

func (m OrderedTagWeights) MarshalJSON() ([]byte, error) {
	if m == nil { return []byte("null"), nil }
	kvs := make([]kv, 0, len(m))
	for k, v := range m { kvs = append(kvs, kv{k, v}) }
	sort.Slice(kvs, func(i, j int) bool {
		if kvs[i].v != kvs[j].v { return kvs[i].v > kvs[j].v }
		return kvs[i].k < kvs[j].k
	})
	var buf bytes.Buffer
	buf.WriteByte('{')
	for i, item := range kvs {
		if i > 0 { buf.WriteByte(',') }
		keyBytes, _ := json.Marshal(item.k)
		buf.Write(keyBytes); buf.WriteByte(':'); buf.WriteString(strconv.Itoa(item.v))
	}
	buf.WriteByte('}')
	return buf.Bytes(), nil
}
```

五处：

1. **自定义 `MarshalJSON` 覆盖默认的 map 序列化** —— 而 Go 的 map 序列化顺序是**随机的**（因为 map 迭代顺序在 Go 里未定义）。

   **这在搜索索引里是致命的**：若同一份数据两次写入产生不同的 JSON 字符串，那么「同样的标签集合」会在引擎里成为两个不同的值，聚合/去重都会出错。

2. **排序规则是「分数降序，同分按标签名升序」** —— 所以完全确定。**同分时的次级键是必需的**，否则同分项的顺序仍不稳定。

3. **手写 buffer 而不是 `json.Marshal(map)`** —— 因为要用排好序的序列。而 `json.Marshal(item.k)` 用于正确转义 key（中文标签、含引号的标签都能处理）。

4. **`if m == nil { return []byte("null") }`** —— nil map 序列化成 `null` 而不是 `{}`。**所以「没有标签」与「标签映射为空」在存储里不同**。这个区分对「读回来时是 nil 还是空 map」有影响。

5. **`sort.Slice` 不是 `sort.SliceStable`** —— 因为比较函数是全序的（分数+名字），不存在平局，所以稳定性无关。

**注释说它是「represents a tag-to-weight mapping that preserves score-descending order when marshaled to JSON for Elasticsearch storage」** —— 所以这个类型的存在理由完全是为了存储层。

### `parseTaggerResponse`：三层容错 + 点号净化

```go
raw = strings.TrimSpace(common.StripThinkTrailing(raw))
if strings.Contains(raw, "**ERROR**") {
	common.Warn("extractor tags: LLM returned **ERROR**")
	return nil
}
obj, ok := tryParseJSONObject(raw)
if !ok {
	obj = jsonRepairExtract(raw)
	if obj == nil { return nil }
}
```

四处：

1. **`StripThinkTrailing` 先剥「思考尾巴」** —— 推理模型会在正式输出后追加思考内容，或反之。这个函数专门剥掉它。

2. **`**ERROR**` 是一个约定的哨兵** —— 所以 prompt 可能要求模型在无法判断时输出这个标记。遇到它就返回 nil（不打标签）。

3. **两段解析**：先严格 `tryParseJSONObject`，失败再 `jsonRepairExtract`（取第一个 `{` 到最后一个 `}` 之间的子串）。**第二段是「从散文里捞出 JSON」**。

4. **`jsonRepairExtract` 的实现是本页最粗的一处**：

```go
start := strings.Index(raw, "{")
end := strings.LastIndex(raw, "}")
if start < 0 || end <= start { return nil }
candidate := raw[start : end+1]
```

   **它假设「第一个 `{` 到最后一个 `}` 就是完整 JSON」**。若模型输出 `{"a":1} 以及 {"b":2}`，会取到 `{"a":1} 以及 {"b":2}` —— 一个非法 JSON。而它不修复这个，所以 `jsonRepairExtract` 实际只在「前后有散文、中间一个对象」时有效。

**然后是净化与截断：**

```go
cleanKey := strings.ReplaceAll(strings.TrimSpace(k), ".", "_")
if score > 0 && cleanKey != "" {
	if existing, ok := result[cleanKey]; !ok || score > existing {
		result[cleanKey] = score
	}
}
```

四处：

1. **点号被替换成下划线** —— 注释说这是「dot sanitization」。**理由能推出来**：标签会成为 Elasticsearch 的字段名或检索词，而 `.` 在 ES 里是**嵌套路径分隔符**。所以 `"深度学习.模型"` 这样的标签会把 ES 的映射搞坏。

   这是一处**为存储层的约束而改写数据**的实例。

2. **`score > 0` 过滤** —— 0 分标签被丢弃（它们没有语义）。

3. **同名标签取最高分** —— 因为点号替换可能让两个不同标签变成同名（`a.b` 与 `a_b`）。取高分而不是后者覆盖，是一个偏保守的选择。

4. **超 `topN` 时按「分数降序、名字升序」截断** —— 与 `OrderedTagWeights` 的序列化顺序完全一致，所以「保留的就是序列化后排在前面的」。

### 模型解析：三层缓存与「空 llm_id」的陷阱

```go
// The cache must capture the model that actually produced the text, not the raw
// llm_id override: when llm_id is empty the pipeline falls back to the tenant's
// default chat model, so keying on the empty override would collapse every
// tenant-default run onto one bucket and serve extractions from a model the
// tenant no longer uses.
func extractorCacheModelID(ctx context.Context, db *gorm.DB, llmID string) string {
	if llmID != "" { ... }
```

**这段注释是本页最重要的一处工程判断，值得完整复述它的逻辑链**：

1. 缓存键必须包含「哪个模型产出了这段文本」。
2. 但 `llm_id` 可能为空——此时系统会解析成「租户的默认模型」。
3. 若键用空 `llm_id`，那么**所有用默认模型的租户会撞到同一个缓存桶**。
4. 更糟的是：租户后来换了默认模型，缓存仍会返回**旧模型**的抽取结果。

**所以「空」不是「无」，而是一个必须被解析成具体身份的间接引用。** 这与 b01 里 `parseBaselineAnchor` 记嵌入模型是同一个纪律。

而解析失败返回 `""`：

> A resolution failure returns "" — `chunkcache.Key` turns an empty modelID into an empty key, which disables caching for that chunk rather than risking a cross-model hit.

**「解析不出来就禁用缓存」** —— 宁可慢也不错。这是一处正确的失败方向。

**`extractorModelCache` 用 context 传递：**

```go
ctx = withExtractorModelCache(ctx)
...
if cache := extractorModelCacheFromCtx(ctx); cache != nil {
	cache.target(ctx, db, in.llmID)
	cache.ctxLen(ctx, db, in.llmID)
}
```

**注释说明了「预热」的理由**：

> Warm the cache before fan-out so the (parallel) chunk jobs read a populated entry: the resolution happens exactly once here, eliminating any first-batch resolution herd. A failed resolution is not cached, so the first chunk's callRaw still surfaces it and the run fails fast.

**「resolution herd」是一个很具体的并发问题**：N 个并行任务同时发现缓存是空的，于是同时去查数据库解析模型。预热把它消掉。

而「失败的解析不进缓存」保证错误能在第一个块上暴露 —— 否则会变成 N 个任务各自失败一次。

### `resumeHook`：一个不外露的扩展点

```go
// resumeHook is an extraction mode: the enabled gate feeding the progress
// split and the phase run itself. attachResumeHook attaches it at
// construction; a nil hook means no mode runs. The hook stays unexported:
// it is this package's internal phase wiring, not a public extension point.
type resumeHook interface {
	Enabled() bool
	Run(ctx context.Context, db *gorm.DB, in extractorInputs, w progress.Window) error
}
```

三处：

1. **两个方法的接口**：`Enabled()` 决定是否参与进度切分，`Run` 是真执行。**「让 Enabled 参与进度切分」这个设计很重要**——否则启用模式的运行会看到进度条只走了 3/4。

2. **注释明确它不是公开扩展点**：「The hook stays unexported: it is this package's internal phase wiring, not a public extension point」。

   **这是一条有价值的边界声明**：Go 里小写类型在同包内可见，但语义上是内部的。把它写进注释防止别人误用。

3. **`nil hook 意味着不跑任何模式** —— 所以「没有启用任何附加模式」是一个合法的常态。

### `extractorChatInvoker`：可替换的 LLM 调用缝

```go
type extractorChatInvoker interface {
	Chat(ctx context.Context, req extractorChatRequest) (*extractorChatResponse, error)
}

var extractorChatInvokerMu sync.RWMutex
var defaultExtractorChatInvoker extractorChatInvoker = &einoExtractorChatInvoker{}

func SetExtractorChatInvoker(inv extractorChatInvoker) { ... }
```

四处：

1. **包级变量 + `RWMutex`** —— 这是「测试替换点」的 Go 写法。注释在 `ExtractorComponent` 上说：「The single mutable package-level seam (`extractorChatInvoker`) is guarded by a RWMutex; tests swap it via `SetExtractorChatInvoker`」。

2. **「single mutable package-level seam」这个措辞说明作者数过**：只有一个包级可变状态，所以并发安全审查只需看这一处。

3. **两个目标解析钩子**（`extractorChatTargetResolverMu` + `extractorChatTargetResolverOverride`）—— 用于测试注入「用哪个模型」。

4. **`einoExtractorChatInvoker` 是真实现** —— 走 eino 框架。所以这里抽象出来是为了让测试不依赖真实模型。

### 输出契约：`chunks` + `output_format` + `_ERROR`

```go
func (c *ExtractorComponent) Outputs() map[string]string {
	return map[string]string{
		"chunks":        "Extraction results — input chunks, each enriched with modular extraction fields (important_kwd, question_kwd, tag_feas, summary, metadata).",
		"output_format": "Always \"chunks\". Parity marker for downstream consumers.",
		"_ERROR":        "Optional short-circuit error message (reserved for the future TOC branch and other error paths).",
	}
}
```

三处：

1. **五个字段名被明确列出**（`important_kwd` / `question_kwd` / `tag_feas` / `summary` / `metadata`）—— 所以这是「原地富化」而非「产出新对象」。

2. **`output_format` 恒为 `"chunks"`** —— 注释说是「Parity marker」，即与 Python 版对齐的标记。

3. **`_ERROR` 是「为未来保留」** —— 注释直说「reserved for the future TOC branch」。**所以这是一个声明了但当前不使用的输出。**

   这解释了一个细节：`Invoke` 的返回里从不含 `_ERROR`。所以下游若依赖它来判断错误，会永远看到「没有错误」。

## 关键取舍

**源码事实：** 四个阶段固定顺序：resume（附加模式）→ keywords → tags → remaining（问句/摘要/元数据）。resume 必须最先，否则通用摘要会「shadowing」（覆盖或抢先写入 `summary`）；tags 必须在 keywords 之后，因为标签 prompt 可以引用刚抽出的关键词。

**源码事实：** `resolveExtractToggle` 的三态规则：键缺失 → 由 `top_n > 0` 推导（兼容旧 DSL）；`enabled` 可接受 bool 与数值；`enabled: true` 且 `top_n < 1` **报错**（显式矛盾必须失败）。而 summary / metadata 只读 bool `enabled`，不支持数值。

**源码事实：** 所有配置错误都在构造期返回，成为画布编译失败（「caught at build time rather than mid-run」）。

**源码事实：** `extractorJob` 是 `func() error`，池是**包级共享**的，默认并发 10；所有任务走「提交一批 + `awaitFutures` 等全部」模式，没有流式消费。

**源码事实：** 失败分级判据是**失败率**：单块 LLM 调用失败跳过该块（`resolveChunkCallError`）；只有**全部**块都被跳过（`checkAllChunksSkipped`）才报错。

**源码事实：** 标签阶段吞掉几乎全部错误（内层闭包 `return nil`、超时只记日志），所以**最后必须补一次 `ctx.Err()` 检查**，注释写明「never report success on a dead context」。

**源码事实：** 标签是**两阶段**：先用倒排索引做本地匹配（`matchAndTagChunk`），未匹配的块才走 LLM；本地高置信匹配的块还会被收集成 few-shot 例子给 LLM 阶段用（文档自举的冷启动）。

**源码事实：** 本地匹配用**两个分数**：`candidateInterIDF`（纯 IDF 和）用于「是否过阈值」，注释写明「ensures coverage <= 1.0」；`candidateTFScore`（IDF * TF boost）用于排序取 top。判据必须可归一化、排序可以带权重，这是刻意分离的。

**源码事实：** 覆盖率是**不对称**的：`Coverage(E) = 分子 / 样例总 IDF`（不是块的总 IDF）。多 token 样例要求至少命中 2 个不同词；单 token 样例要求该词总 IDF ≥ 1.5 且覆盖率阈值 0.999。

**源码事实：** `OrderedTagWeights` 自定义 `MarshalJSON` 以「分数降序 + 标签名升序」输出，因为 Go 的 map 迭代顺序未定义——顺序不稳定会让「同一标签集合」在搜索索引里成为不同值，破坏聚合与去重。nil map 序列化为 `null`（与 `{}` 区分）。

**源码事实：** `parseTaggerResponse` 三层容错：先 `StripThinkTrailing`，再识别 `**ERROR**` 哨兵返回 nil，再 `tryParseJSONObject` 失败则 `jsonRepairExtract`（取第一个 `{` 到最后一个 `}`）。后者只在「前后散文 + 中间一个对象」时有效，多对象输出会取到非法串。

**源码事实：** 标签 key 的点号被替换成下划线（dot sanitization）——因为 `.` 是 Elasticsearch 的嵌套路径分隔符，会破坏映射；替换后可能撞名，取较高分。

**源码事实：** 缓存键必须用**解析后**的模型身份而不是原始 `llm_id`：空 `llm_id` 会让所有用默认模型的租户撞到同一个缓存桶，且租户换默认模型后仍会返回旧模型的结果；解析失败返回 `""` 从而**禁用**该块缓存（宁可慢也不错）。

**源码事实：** 模型解析用 `context` 传递的每运行缓存，并在 fan-out **之前预热**，以消除「resolution herd」（N 个并行任务同时发现缓存为空）。失败的解析不进缓存，以便第一个块暴露错误。

**源码事实：** `resumeHook` 的两个方法让 `Enabled()` 参与进度切分；注释明确它**不是公开扩展点**（「this package's internal phase wiring」）。

**源码事实：** 唯一的包级可变状态是 `extractorChatInvoker`（加 `RWMutex`），注释称之为「single mutable package-level seam」，供测试替换。

**源码事实：** 输出是**原地富化**：`chunks`（带五个新增字段）+ `output_format`（恒为 `"chunks"`）+ `_ERROR`（声明但当前不使用，「reserved for the future TOC branch」）。

**架构推论：** 这四阶段的排序约束本质是**数据依赖**，但代码里没有显式的依赖图——只有注释。若将来新增第五个阶段，它该插在哪里完全取决于作者是否读到了那条「summary shadowing」注释。**这是「隐式 DAG」的典型风险**：顺序约束靠注释传达，编译器不检查。

**架构推论：** 「失败率判据」而不是「任一失败即失败」是这个组件的核心取舍。它承认**富化字段是可选的**：一个块缺关键词，检索仍能用它的正文；但若所有块都失败，那就是系统问题。这个分级让「模型偶发超时」不会毁掉整个文档摄取。而代价是：**部分失败是静默的**（只记日志，块的字段为空），运维需要看统计才知道失败率。

**架构推论：** 「空 llm_id 必须解析成具体身份才能做缓存键」这条纪律可以推广成一般规则：**任何间接引用都要在进入缓存/持久化之前被解引用。** 否则「默认值」会变成一个共享桶，而桶里的内容会随默认值漂移变成脏数据。这与 b01 里「基线必须记实际嵌入模型」是同一条纪律的两处应用。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/ingestion/component/extractor.go` | `ExtractorComponent` | 抽取组件（参数 + 附加模式钩子） |
| `internal/ingestion/component/extractor.go` | `NewExtractorComponent` | 解析 DSL 参数 + 三态开关 + 构造期校验 |
| `internal/ingestion/component/extractor.go` | `resolveExtractToggle` | 开关三态规则（缺失→top_n、bool/数值、矛盾报错） |
| `internal/ingestion/component/extractor.go` | `resumeHook` | 附加模式的内部扩展点（参与进度切分） |
| `internal/ingestion/component/extractor.go` | `Invoke` | 四阶段编排 + 模型缓存预热 + 收尾取消检查 |
| `internal/ingestion/component/extractor.go` | `resolveInputs` | 从 inputs / Globals 解析运行参数 |
| `internal/ingestion/component/extractor.go` | `extractorJob` | 极简闭包任务类型 |
| `internal/ingestion/component/extractor.go` | `extractorPool` | 包级共享 worker 池（默认并发 10） |
| `internal/ingestion/component/extractor.go` | `awaitFutures` | 等一批任务并推进进度窗口 |
| `internal/ingestion/component/extractor.go` | `resolveChunkCallError` | 单块失败的分类与计数 |
| `internal/ingestion/component/extractor.go` | `checkAllChunksSkipped` | 「全跳过才报错」的失败率判据 |
| `internal/ingestion/component/extractor.go` | `runAutoKeywords` / `runAutoQuestions` / `runAutoSummary` | 三种文本衍生字段（同签名不同实现） |
| `internal/ingestion/component/extractor.go` | `runAutoKeywordsPool` | 关键词阶段池化入口 |
| `internal/ingestion/component/extractor.go` | `runRemainingExtractions` | 问句 / 摘要 / 元数据汇总阶段 |
| `internal/ingestion/component/extractor.go` | `extractorLLMCacheKey` | 每块缓存键（用块 id 而非文本） |
| `internal/ingestion/component/extractor.go` | `extractorCacheModelID` | 缓存键用解析后的模型身份（空 llm_id 必须解引用） |
| `internal/ingestion/component/extractor.go` | `extractorModelCache` / `withExtractorModelCache` | 每运行的模型解析缓存（context 传递） |
| `internal/ingestion/component/extractor.go` | `extractorChatInvoker` / `SetExtractorChatInvoker` | 唯一的包级可变缝（测试替换点） |
| `internal/ingestion/component/extractor.go` | `renderExtractorPrompt` | `{{ topn }}` 模板替换 |
| `internal/ingestion/component/extractor_tag.go` | `OrderedTagWeights` | 确定性 JSON 序列化（分数降序 + 名字升序） |
| `internal/ingestion/component/extractor_tag.go` | `runAutoTags` | 两阶段标签（本地匹配 → LLM 兜底） |
| `internal/ingestion/component/extractor_tag.go` | `runLLMTags` | 未匹配块提交到共享池 |
| `internal/ingestion/component/extractor_tag.go` | `matchAndTagChunk` | 倒排索引 + 不对称覆盖率 + 1-token 特殊规则 |
| `internal/ingestion/component/extractor_tag.go` | `llmTagChunk` | 单块 LLM 打标签（含冷启动样例） |
| `internal/ingestion/component/extractor_tag.go` | `buildTaggerPrompt` | 标签 prompt 组装 |
| `internal/ingestion/component/extractor_tag.go` | `parseTaggerResponse` | 三层容错 + 点号净化 + topN 截断 |
| `internal/ingestion/component/extractor_tag.go` | `detectTextLanguage` | 按 Kana / Hangul / CJK 优先级判语言 |
| `internal/ingestion/component/extractor_tag.go` | `loadTagFileIndexed` | 租户内解析标签源文件（IDOR 防护） |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 文档：自动关键词与问题](https://ragflow.io/docs/dev/configure_knowledge_base)
- [论文：TF-IDF 与倒排索引检索](https://nlp.stanford.edu/IR-book/)
- [Elasticsearch 文档：点号在字段名中的含义](https://www.elastic.co/guide/en/elasticsearch/reference/current/field-names.html)

## 与主核 / AI 的关系

抽取组件与主核（多模态数据库内核）的**触发器 / 派生列**是同一类东西：都在「主数据写入后追加派生数据」。而两者的失败策略相反：主核的 `GENERATED ALWAYS` 派生列若计算失败，**整个 INSERT 失败**（因为派生列与基础列必须一致）；这里的富化字段**允许缺失**，块照样落库。

**这个差别的根源是「派生字段是否是契约的一部分」**：主核的派生列被 `CHECK` 与索引依赖，缺了会让约束不成立；这里的 `important_kwd` 只是检索增强，缺了索引仍能工作。**判据是「下游是否假设它一定存在」**——这里的下游必须能处理空字段，所以允许跳过。

第二处对照是 `OrderedTagWeights` 的确定性序列化与主核的**索引键排序**。B-tree 要求键的全序（比较函数必须一致且传递），而这里的 JSON 序列化要求一个全序（分数 + 名字）。两者都是「为了可比较而强加次序」。差别在**后果的可见性**：B-tree 的比较函数不一致会让索引损坏并报错；这里顺序不稳定只会让同一标签集合变成两个值，**静默**破坏聚合。

第三处是「空 llm_id 必须解引用」与主核的**角色（role）解析**。数据库的 `SET ROLE DEFAULT` 也是一个间接引用，而 `pg_stat_activity` 记录的是**实际生效的角色**而不是 `DEFAULT`。理由完全相同：日志/统计/缓存里存间接引用，事后就无法判断当时到底是谁。

对 RAG 工程的实际意义在于：LLM 富化是「用钱换质量」的环节，而它的设计质量取决于**失败粒度**。这份实现给出的三条经验可直接用：**阶段顺序要按数据依赖排，而且必须写进注释**（隐式 DAG 编译器不检查）、**富化失败按失败率分级**（个别跳过、全失败报错）、以及**缓存键必须用解引用后的身份**（空值会变成共享桶，桶内容随默认值漂移变脏）。
