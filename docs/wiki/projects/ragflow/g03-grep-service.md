---
project: ragflow
feature: grep-service
title: 精确串检索服务
page_type: feature
schema_version: 1
refs:
  - path: internal/agentic_rag/grep_service.go
    symbols: [GrepAdapter, NewGrepAdapter]
---

# 精确串检索服务

## 一句话职责

给 agentic 检索一条**字面匹配**通道：把模型写的正则下推给引擎（Elasticsearch 的 `regexp` 查询、Infinity 的 `regex()` 过滤），让「原文里到底有没有这个词」变成可回答的；而这一页真正的技术内容是**当下推被引擎拒绝时怎么办**——三层降级，每一层都保留「我是近似结果」的可见标记。

## 机制是什么

`GrepAdapter` 只有一个字段：

```go
type GrepAdapter struct {
    docEngine engine.DocEngine
}
```

注释声明它「无状态且可安全跨 goroutine 共享」，而能力探测靠一个**窄接口断言**：

```go
type regexpSearchable interface {
    SearchByRegexp(ctx context.Context, req *enginetypes.RegexpSearchRequest) (*enginetypes.SearchResult, error)
}
```

注释：「`*elasticsearch.Engine` 与 `*infinity.Engine` 实现它；其他引擎没有。」所以「这个引擎能不能做原生正则」不是一个配置项，而是一次**类型断言**——能力由实现决定，不需要维护一张能力表。

而 `Grep` 对「引擎不支持」的处理是**直接报错而不是返回空**：

```go
if se, ok := g.docEngine.(regexpSearchable); ok {
    ...
}
// Engines without native regex support cannot match on chunk content; surface
// an explicit error rather than returning (empty) regex "matches".
return nil, runtime.ErrRegexpNotSupported
```

注释说得很直：「一个没有原生正则支持的引擎无法在块内容上匹配；**显式报错而不是返回（空的）正则『匹配』**」。

这条很重要：**「空结果」与「做不到」在语义上完全不同**——前者是「没有匹配」，后者是「我没查」。把一个能力缺失表达成空结果，会让模型以为「原文里没有这个词」，而那是一个它会据此下结论的错误事实。

**这是这一页的主线**：整个 `Grep` 的实现几乎全部围绕「让『做不到』不被误读成『没有』」展开。

## 怎么实现的

**`Grep` 的四道前置检查，而第一道的注释解释了一个「验在 Go 侧」的必要。**

```go
if strings.TrimSpace(req.Pattern) == "" {
    return nil, fmt.Errorf("grep: pattern cannot be empty")
}
// Validate the regex on the Go side regardless of pushdown path.
if _, err := regexp.Compile("(?i)" + req.Pattern); err != nil {
    return nil, fmt.Errorf("grep: invalid regex: %w", err)
}
```

第二道的关键是 `(?i)` 前缀：**引擎侧大小写不敏感，但 Go 侧也必须用同样的大小写语义去编译**。若 Go 侧按大小写敏感编译，那么一个「只有在忽略大小写时才合法」的模式（极少见但存在，例如某些字符类边界）会在 Go 侧通过而引擎侧行为不同。

而「regardless of pushdown path」这句说明了为什么要在 Go 侧验：**三段降级路径里最后一段是「在 Go 内存里过滤」**，所以这个模式最终一定会在 Go 侧被使用，而不是只作为引擎查询的输入。

**过滤条件里 `must_not exists compile_kwd` 有一个具体的理由。**

```go
filter := map[string]interface{}{
    "available_int": 1,
    "must_not":      map[string]interface{}{"exists": "compile_kwd"},
}
```

注释：

> 只搜普通文档文本块：`available_int=1` 且无 `compile_kwd`（它标记知识编译产物，例如 `wiki_page`、`hypergraph`、`mindmap`、`list`、`timeline`）。**知识产物是派生内容，不是用户问的原文散文，而其中几类超过 ES 的 keyword term 长度上限。**

两条理由：

1. **语义** ——派生内容不是原文，搜它等于答非所问。
2. **技术** ——那些产物的内容可能超过 `keyword` 字段的 term 长度上限，所以把它们排除掉也避免了引擎错误。

`available_int = 1` 是「这一块仍然可用」（例如分摊到已删除文档的块会是 0）。

**`engineCallContext` 解决了一个流式 ReAct 的具体问题。**

```go
func engineCallContext(ctx context.Context) (context.Context, context.CancelFunc) {
    if ctx.Err() == nil {
        return context.WithCancel(ctx)
    }
    return context.WithCancel(context.WithoutCancel(ctx))
}
```

注释：

> 用一个没有 per-query 超时的派生引擎上下文（运行级预算限制整个运行）。**eino 的流式 ReAct 可能在模型发出 `tool_calls` 之后就把一个已经取消的上下文交给工具**；一个已取消的父上下文不得杀死引擎查询，所以那种情况下回退到一个全新的上下文。

所以问题是：**上下文取消的「原因」与「时机」在流式架构里会错位**。模型发出工具调用后，那一轮的流可能已经被判定结束，于是 ctx 被取消，但工具调用还要跑。若工具继承这个已取消的 ctx，查询立刻失败。

而解法是「已取消就换一个不带取消的派生上下文」。这与 g01 里 `finalizeAnswer` 的 `context.WithoutCancel` 是同一种手法，但**触发条件不同**：那里是无条件脱离（因为兜底必然在预算耗尽后触发），这里是**有条件**（仅当已取消时）——因为正常情况下应该保留取消传播。

**`Grep` 的 `Sort` 与 `list_chunks` 共享排序语义。**

```go
Sort: sortExprFromFields(req.Sort), // same reading order as list_chunks
```

所以「grep 的结果」与「list_chunks 的结果」按同一个顺序返回。这是一个**一致性要求**：同一个文档通过两条通道读到的块，顺序应当一致，否则模型看到「第 3 块」在不同工具里指向不同位置。

**下推失败的消息里带的提示是这一页最有价值的一段注释。**

```go
if err != nil {
    return nil, fmt.Errorf("grep: %w — %v.%s", runtime.ErrRegexpPushdown, err, regexpPushdownHint(req.Pattern))
}
```

而 `regexpPushdownHint` 的注释：

> 这个失败几乎从不是语法错误：Elasticsearch 把模式编译成一个受 `max_determinized_states` 约束的自动机，而每个模式已经被包在 `.*(...).*` 里，所以一个对 CJK 文本的有界重复会确定化成远超预算的状态——
> `Determinizing .*(关雎.{0,20}杀).* would require more than 10000 effort.`
>
> **措辞比看起来更重要**：一个 ReAct 循环把工具错误当作「那个查询结束了」然后继续，而一个原始的 `query_shard_exception` 给模型什么也没有。**在一次观察到的运行里**，模式被拒绝、模型用一个什么也不匹配的模式重试了一次、然后结束了这一轮、完全没有产出任何可交付物。**提示必须说该写什么来代替。**

这段注释值得单独抄下来，因为它记录的是一条**可复现的失败链**：

1. 模式被引擎拒绝（因为自动机状态预算，而不是语法错）。
2. 错误消息是 `query_shard_exception` 的原文——对模型毫无用处。
3. 模型重试一次，用一个「不报错但什么也不匹配」的模式（因为它不知道真实原因是「太贵」）。
4. 然后放弃整轮，零产出。

所以「工具错误消息的质量」在 agentic 系统里是一个**功能性**问题，不是体验问题——**一个没有可操作信息的错误会让模型浪费掉剩下的预算并放弃**。

而 `regexpPushdownHint` 按四种模式分别给出具体替代写法：

| 检测到的模式 | 提示 |
|---|---|
| 有界重复 `{n,m}` | 「重试时**去掉**有界重复 `...`：引擎把模式编译成有界自动机，而对 CJK 文本的重复会耗尽它（ES 把每个模式包在 `.*(...).*` 里，那要更多状态）。用 `.*` 连接词条（如 `关雎.*杀`）或把它们列为分支」 |
| 反向引用 `\1` | 「重试时**去掉**反向引用：引擎不支持——改用重复分支」 |
| lookaround `(?=` / `(?<!` | 「重试时**去掉** lookaround：引擎不支持——把约束表达成普通词条」 |
| 其他 | 「重试一个**更便宜**的模式：字面词条用 `.*` 连接、最多 2-3 个分支、没有有界重复——这个模式对引擎来说编译太贵了」 |

四条提示全部是「**告诉我该写什么来代替**」，而不是「你的模式不合法」。这是一个重要的模式：**当工具的失败原因是「资源预算」而不是「语义非法」时，唯一的可操作建议是「换个更便宜的等价写法」**。

**三层降级是这一页的核心，而三层的排序依据是「最便宜最忠实优先」。**

```go
// degradeGrep recovers from a pushdown the engine refused, in three steps,
// cheapest and most faithful first.
```

**第一层：清洗后重试。**

```go
if safe, changed := luceneSafeRegexp(req.Pattern); changed {
    retry := req
    retry.Pattern = safe
    if chunks, retryErr := svc.Grep(ctx, retry); retryErr == nil {
        return chunks, pushdownWarning(pushErr, "the engine accepted a sanitized form of the regexp; local Go regexp filtering restored the original match semantics"), nil
    }
}
```

而 `luceneSafeRegexp` 的每次改写都标注了「保语言」还是「放宽」：

```go
safe = strings.ReplaceAll(safe, `\b`, "")     // 放宽
safe = strings.ReplaceAll(safe, `\B`, "")     // 放宽
safe = strings.ReplaceAll(safe, `\A`, "^")    // 保语言
safe = strings.ReplaceAll(safe, `\z`, "$")    // 保语言
safe = strings.ReplaceAll(safe, `\Z`, "$")    // 保语言
safe = luceneNamedGroupRe.ReplaceAllString(safe, "(")  // 保语言: 命名组 → 普通组
```

docstring 的论证：

> 每次改写要么保持语言相同（`\A` → `^`、`\z` → `$`、命名组 → 普通组），要么**放宽**它（`\b`、`\B` → 空），这对引擎侧的预过滤是安全的，**因为调用方会用原始模式重新过滤**。携带 lookaround 或内联标志的模式被原样保留：它们无法在不改变匹配语义的前提下表达，所以它们走词法路径并承担那条覆盖度警告。

所以「放宽」之所以安全，是因为**客户端会重新过滤**——引擎只负责「别漏掉可能的块」，精确性由 Go 侧恢复。`\b` 删掉后引擎侧会返回更多块，而 Go 侧用原始 `\b` 模式再筛一遍，结果完全正确。

而 lookaround 与内联标志被**排除在清洗之外**：

```go
if luceneLookaroundRe.MatchString(pattern) || luceneInlineFlagRe.MatchString(pattern) {
    return pattern, false
}
```

两条注释说明了为什么不能处理：

- **lookaround**：「Lucene 的方言没有 lookahead/lookbehind。**不重写**——没有等价物能保住匹配什么」。
- **内联标志**：「Lucene 把它们当参数而不是模式内的东西；**丢掉它们会静默翻转大小写敏感性**」。

第二条是一个特别具体的风险：`(?i)` 若被简单删掉，模式就变成大小写敏感的，而结果是**静默变少**（不是多也不报错）。所以「放弃清洗、走词法路径」比「删掉标志」正确。

**第二层：按顶层分支拆开分别下推，而它的论证是一个关于自动机的洞见。**

```go
//  2. Split the top-level alternation and push each branch as its own regexp.
//     State explosion is an automaton PRODUCT: `.*(A.*B|C.*D|E.*F).*` compiles
//     into one machine whose state count multiplies the branches. Pushed
//     separately, each branch stays small. Splitting is EXACT — the union of
//     `.*(A.*B).*`, `.*(C.*D).*` — is precisely what the combined pattern
//     matches — so this step keeps full regexp semantics and full coverage. A
//     branch that still fails leaves a genuine hole, which the notice reports.
```

关键洞见：「状态爆炸是一个自动机**乘积**」——一个含 3 个分支的模式编译成**一台机器**，而它的状态数是分支的乘积（不是加法）。分开推，每台机器都小。

而「拆分是精确的」这一点很重要：`.*(A|C).*` 匹配的集合恰好等于 `.*(A).*` 与 `.*(C).*` 的并集。所以这一步**既保留完整正则语义又保留完整覆盖**——不像第一层有放宽、第三层有损失。

**第三层：词法预过滤，而它明确承认覆盖度有损。**

```go
//  3. Lexical prefilter. A pattern (or branch) the engine keeps refusing is
//     reduced to its literal terms and handed to the BM25 service. The local
//     regexp filter restores precision, but recall now rides on the prefilter's
//     top-N — hence the notice: an empty result is NOT proof the corpus lacks
//     the phrase.
```

所以第三层的契约是「**精确性由本地恢复、召回率取决于预过滤的 top-N**」。而它的后果被写进了给模型的提示里（那段 `APPROXIMATE` 警告）。

三层的「性质」可以列成一张表，而这正是这一页的结构：

| 层 | 手段 | 正则语义 | 覆盖度 | 需要告知模型 |
|---|---|---|---|---|
| 1 | 清洗 Lucene 不支持的结构 | **放宽**（`\b`→空） | **完整** | 是（说明语义由本地恢复） |
| 2 | 拆顶层分支分别推 | **精确** | **完整** | 是（除非某分支仍失败） |
| 3 | 转 BM25 词法预过滤 | 精确（本地重筛） | **有损**（受 top-N 限制） | **必须**（空结果不证明语料没有） |

**而 `pushdownWarning` 的存在理由是「模型必须能区分真正的正则结果与降级结果」。**

```go
// pushdownWarning keeps the original pushdown rejection visible whenever a
// fallback produces results. The model must distinguish a real regexp result
// from a result obtained after the engine rejected the requested pattern.
```

这一点是本页最深的设计判断：**一个降级成功的结果，其正确性等级与原始路径不同**，所以它必须被标记。若三层降级全部静默成功，模型会认为「这就是完整正则匹配的结果」，而第三层的覆盖度损失会让它得出「语料里没有更多了」的错误结论。

所以降级不是「尽量让它跑通」，而是「**跑通但降低可信度并声明**」。

**而三层降级里没有「静默返回空」。**

```go
return nil, "", pushErr
```

三层都失败时**返回最初的 pushdown 错误**，注释：

> pushdown 错误在没有一层产出任何东西时被返回，所以模型仍然得到「用一个更便宜的模式重试」的提示，而不是一个静默的空结果。

所以最后的兜底是「把第一层的错误给模型」而不是「返回空」。

**`Grep` 的第一段里还有一处「不允许内存 RE2 兜底」的明确声明。**

```go
// Path 1: native regex pushdown (Elasticsearch, Infinity). With
// content_with_weight mapped as a searchable keyword field, native regexp
// matching is the sole path; there is intentionally no in-memory RE2
// fallback (a pushdown failure is a real error the caller should surface,
// not silently "no matches").
```

「**故意没有内存 RE2 兜底**」——因为一个下推失败是一个真错误，调用方应当暴露它，而不是静默变成「没有匹配」。

注意这与 `degradeGrep` 的存在看似矛盾：一个说「没有内存兜底」，另一个说「三层降级」。实际上不矛盾——`Grep` 本身（直接调用路径）不做降级；`degradeGrep` 是**上层**（`grep_chunks` 工具）拿着 `pushErr` 再调用 `svc.Grep` 的封装。所以责任分层是：

- `GrepAdapter.Grep` ——纯执行，失败就报错。
- `degradeGrep` ——策略层，拿错误后尝试三种更便宜的路径。

这个分层很好：**执行器不猜调用方的意图，策略层负责降级**。而 `degradeGrep` 的签名里显式接收 `pushErr error`——它需要那个错误来构造最终提示。

**`Grep` 的返回是 `translateGrepChunks(res.Chunks)` 而不是直接返回引擎结果。**

```go
return translateGrepChunks(res.Chunks), nil
```

所以引擎的 `map[string]interface{}` 被转成 `runtime.RetrievalChunk`——这是工具层与引擎层的类型边界。

**`splitTopLevelAlternation` 与括号深度有关，而 `grepDegradeMaxSplits = 8` 是一个上限。**

```go
// grepDegradeMaxSplits caps how far an exploding pattern is split by
// alternation: past this the pattern is fragmented enough that a lexical
// prefilter is the better tool.
```

所以「拆得太多就不拆了，改用词法」——因为拆成 8 个以上分支意味着「这个模式本身就不是一个短语，而是一堆候选」，那时 BM25 是更合适的工具。

而 `regexpLiteralTerms` 的注释解释了「为什么顶层分支优先」：

> 每个顶层分支一个查询（最多 `max` 个）——**分支本身就是意图的措辞**——正则语法被剥掉只留词。**顶层分支优先**（一个组内的内层交替保持为一个查询，那更接近模型本来的措辞）；一个没有顶层交替的模式退化成在每个 `|` 上拆。

所以这里有一个关于「意图」的判断：`(A|B).*C` 的顶层有两个分支，而 `X.*(A|B).*Y` 的顶层只有一个（括号内的交替算一个）。前者拆成两个查询更接近模型的意思，后者整体当一个查询更接近。

**`grepDegradeTopNFactor = 4` 的注释说明了「多取为什么安全、少取为什么危险」。**

```go
// grepDegradeTopNFactor widens the prefilter's per-query pool: the caller
// re-filters with the real regexp, so over-fetching costs only ranking noise
// while under-fetching loses recall the caveat cannot recover.
```

「调用方会用真正的正则再筛，所以**多取只花排名噪声，而少取丢失的召回是那条警告无法挽回的**」。

这是「精度与召回不对称」的一个清晰表述：因为本地重筛存在，精度可以靠多取修复；而召回一旦漏掉就永久丢失。所以预过滤应当**宽进**。

**`ListByDocIDs` 的两条读路径与「全部读取」的语义。**

`ListByDocIDs` 的第一条路径用 `Pattern: ".*"` + `ReturnAll: true`：

```go
Pattern:      ".*",
Sort:         sortExprFromFields(req.Sort), // reading order: doc_id, page_num_int, chunk_order_int
SelectFields: req.SelectFields,
ReturnAll:    true,
```

所以「列出一篇文档的所有块」被实现成「用正则 `.*` 匹配全部」。这是复用正则通道的一个巧妙做法，但注释也点出了它的前提：`content_with_weight` 在当前映射上是一个**可搜索的 keyword 字段**。

而第二条路径是一个**纯元数据过滤**，注释说明了它的三个好处：

```go
// Path 2: a PURE METADATA FILTER. It answers the same question on every index
// generation, keeps chunks longer than a keyword's ignore_above limit
// visible, and needs no regex support from the engine. Reading order is
// restored in Go by toChunks.
```

三条：

1. **在每一代索引上都回答同一个问题** ——包括那些 `content_with_weight` 还是 `text` 且 `index:false` 的老索引。
2. **超过 keyword 的 `ignore_above` 上限的块仍然可见** ——这是第一层清洗做不到的（`.*` 下推在超长块上也会因为 keyword 长度上限失败）。
3. **不需要引擎支持正则**。

而这条路径会截断，且截断被显式记录：

```go
if res.Total > int64(len(res.Chunks)) {
    common.WarnCtx(ctx, "agentic_rag: list_chunks deep read TRUNCATED by the engine result window",
        zap.String("path", deepReadPathFilterSearch),
        zap.Int("returned", len(res.Chunks)), zap.Int64("total", res.Total),
        zap.Int("limit", deepReadFetchLimit))
}
```

而注释解释了为什么必须记录：

> 过滤路径在 `deepReadFetchLimit` 处分页，而下推要求全部，所以一篇块数超过这个限的文档会**静默丢失它的尾巴**。说出来：**调用方的邻居算术假设一个完整的索引，而一次静默的短读与一篇短文档不可区分。**

「一次静默的短读与一篇短文档不可区分」——这句话抓住了这一页反复出现的那个问题的最锋利版本。而它还有第二层：`调用方的邻居算术假设一个完整的索引`，即 `list_chunks` 之后的逻辑会按「第 N 块的前后邻居」推理，而缺了尾巴会让那个推理错。

**而两条路径的失败处理都很细：下推失败后先查 `ectx.Err()`。**

```go
if cerr := ectx.Err(); cerr != nil {
    return nil, fmt.Errorf("list_chunks: deep-read recall: %w", err)
}
```

注释：「一次被取消或超时的运行**不得为同一个引擎的第二次查询付费**：下面的降级是给一个引擎拒绝了的查询，而不是给一个从来没有机会跑的查询」。

所以「下推失败」有两种：引擎拒绝（值得降级重试）与运行已取消（不该再查）。而判据是**错误是不是由取消引起的**——这是这一页里第二处「同一个错误要分辨成因」。

## 关键取舍

**源码事实：** 能力探测用窄接口断言（`regexpSearchable`）而不是配置项或能力表；只有 Elasticsearch 与 Infinity 实现它。

**源码事实：** 引擎不支持原生正则时**显式报错**（`ErrRegexpNotSupported`）而不是返回空——注释理由：「一个没有原生正则支持的引擎无法在块内容上匹配；显式报错而不是返回（空的）正则『匹配』」。

**源码事实：** 正则在 Go 侧用 `(?i)` 前缀**无条件**预编译校验，注释说明「regardless of pushdown path」——因为降级链的最后一段会在 Go 内存里用同一个模式过滤。

**源码事实：** 过滤条件排除 `compile_kwd`（知识编译产物）有两条理由：语义（派生内容不是原文散文）与技术（其中几类超过 ES 的 keyword term 长度上限）。

**源码事实：** `engineCallContext` 在 ctx **已取消**时改用 `context.WithoutCancel` 重新派生——因为「eino 的流式 ReAct 可能在模型发出 `tool_calls` 之后就把一个已经取消的上下文交给工具」。

**源码事实：** 三层降级「最便宜最忠实优先」：①`luceneSafeRegexp` 清洗（`\A`→`^`、`\z`→`$`、`\Z`→`$`、命名组→普通组**保语言**；`\b`/`\B`→空**放宽**，因调用方会用原模式重筛）；②按顶层交替拆开分别推（**精确**，因为状态爆炸是自动机**乘积**而拆分是集合并集）；③转 BM25 词法预过滤（精度由本地恢复、**召回受 top-N 限制**）。

**源码事实：** `luceneSafeRegexp` **拒绝处理** lookaround 与内联标志——lookaround「没有等价物能保住匹配什么」，内联标志「丢掉它们会**静默翻转大小写敏感性**」。

**源码事实：** `degradeGrep` 三层都没产出时**返回最初的 pushdown 错误**，所以模型仍得到「用更便宜的模式重试」的提示而不是静默空结果。

**源码事实：** `regexpPushdownHint` 按四种模式给出具体替代写法（有界重复 / 反向引用 / lookaround / 其他），且注释记录了一条**实测失败链**：「模式被拒绝、模型用一个什么也不匹配的模式重试一次、然后结束这一轮、零产出」——结论是「提示必须说该写什么来代替」。

**源码事实：** 下推失败的常见原因是**自动机状态预算**而非语法错误：ES 把模式编译成受 `max_determinized_states` 约束的自动机，且每个模式已被包在 `.*(...).*` 里，所以对 CJK 文本的有界重复会确定化成远超预算的状态。

**源码事实：** `pushdownWarning` 让降级结果**带上原始拒绝原因**，因为「模型必须能区分真正的正则结果与在引擎拒绝所请求模式后得到的结果」。

**源码事实：** `grepDegradeTopNFactor = 4` 的理由是「多取只花排名噪声，而少取丢失的召回是那条警告无法挽回的」。

**源码事实：** `grepDegradeMaxSplits = 8` 之后改用词法——「拆到这个程度说明这个模式不是短语而是一堆候选」。

**源码事实：** `Grep` 本身**故意没有内存 RE2 兜底**（下推失败是真错误，调用方应暴露）；降级在 `degradeGrep` 策略层，其签名显式接收 `pushErr`。

**源码事实：** `RegexpSearchRequest.Sort` 与 `list_chunks` 共享阅读顺序（`doc_id` / `page_num_int` / `chunk_order_int`），所以两条通道读到的块顺序一致。

**源码事实：** `ListByDocIDs` 的第二条路径是**纯元数据过滤**，三条好处：在每一代索引上都有效、不受 keyword `ignore_above` 上限影响、不需要引擎支持正则。

**源码事实：** 过滤路径在 `deepReadFetchLimit` 处分页，超限时**显式 WARN** 记录截断——「调用方的邻居算术假设一个完整的索引，而一次静默的短读与一篇短文档不可区分」。

**源码事实：** 下推失败后先查 `ectx.Err()`：若是取消/超时导致则直接返回错误，**不降级**——「一次被取消的运行不得为同一个引擎的第二次查询付费」。

**架构推论：** 这一页真正解决的是一个**分类学**问题：「没有匹配」/「做不到」/「太贵」/「被取消」四种失败必须区分，而它们的正确响应完全不同（空结果 / 报错 / 换更便宜的等价写法 / 直接终止）。把它们压成「工具调用失败」一个类别，会逼模型在信息缺失下猜——而实测已经证明它会猜错并耗尽预算。**判据是：这个失败是「关于世界的答案」还是「关于工具的状态」？** 前者可以作为结果给模型，后者必须让它知道该怎么改。

**架构推论：** 三层降级的排序依据（「最便宜最忠实优先」）不是性能排序而是**保真度排序**：第一层完整覆盖但有放宽、第二层精确且完整、第三层有损失。而它说明了一个可推广的原则：**降级要按「损失从小到大」而不是「实现难度从易到难」**。按难度排会把最有损的路径放在前面，而按保真度排保证每次降级都取到当前可用的最好结果。

**架构推论：** 「降级成功必须标记」是这一页最该抄走的一条。`pushdownWarning` 让每一次降级都带上原始拒绝原因，因为**降级结果的正确性等级与原始路径不同**。一个静默的降级会把「近似召回」伪装成「完整匹配」，而模型会据此断定「语料里没有更多了」。这与 g01 里「`evidence_in_hand` 失败时 13/20 已有正确文档却未被引用」是同一类问题的另一面——**系统给出了一个可信度不同的事实，而没有说明它的可信度**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/agentic_rag/grep_service.go` | `GrepAdapter` | 无状态正则检索适配器，只持 `docEngine` |
| `internal/agentic_rag/grep_service.go` | `NewGrepAdapter` | 构造，包装一个文档引擎 |
| `internal/agentic_rag/grep_service.go` | `regexpSearchable` | 能力探测的窄接口，仅 ES 与 Infinity 实现 |
| `internal/agentic_rag/grep_service.go` | `Grep` | 原生正则下推，失败即报错、无内存兜底 |
| `internal/agentic_rag/grep_service.go` | `ListByDocIDs` | 两条读路径：`.*` 下推 与 纯元数据过滤 |
| `internal/agentic_rag/grep_service.go` | `fetchScoped` | 两条读路径共用，截断时显式 WARN |
| `internal/agentic_rag/grep_service.go` | `degradeGrep` | 三层降级策略，显式接收 `pushErr` |
| `internal/agentic_rag/grep_service.go` | `luceneSafeRegexp` | 清洗 Lucene 不支持的结构，保语言或放宽 |
| `internal/agentic_rag/grep_service.go` | `grepBySplitAlternation` | 按顶层分支分别下推，精确且完整 |
| `internal/agentic_rag/grep_service.go` | `regexpLiteralTerms` | 降到 BM25 词法查询，顶层分支优先 |
| `internal/agentic_rag/grep_service.go` | `pushdownWarning` | 降级结果必须标记原始拒绝原因 |
| `internal/agentic_rag/grep_service.go` | `regexpPushdownHint` | 按四种模式给出具体替代写法 |
| `internal/agentic_rag/grep_service.go` | `engineCallContext` | ctx 已取消时改用无取消派生 |
| `internal/agentic_rag/grep_service.go` | `grepDegradeMaxSplits` | 8，超过则改用词法预过滤 |
| `internal/agentic_rag/grep_service.go` | `grepDegradeTopNFactor` | 4，预过滤宽进因为本地会重筛 |
| `internal/agentic_rag/grep_service.go` | `isGraphChunkContent` | 过滤图块内容，避免把关系行当原文 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：agentic_rag/grep_service.go](https://github.com/infiniflow/ragflow/blob/main/internal/agentic_rag/grep_service.go)
- [Elasticsearch 文档：regexp 查询](https://www.elastic.co/guide/en/elasticsearch/reference/current/query-dsl-regexp-query.html)
- [RE2 语法（Go 正则实现）](https://github.com/google/re2/wiki/Syntax)

## 与主核 / AI 的关系

主核里最接近的是**全文检索与 LIKE / regex 下推**：`EXPLAIN` 会告诉你「这个谓词能否下推到索引」，不能下推就退化成顺序扫描。而这里的三层降级就是「谓词下推失败后的三条退化路径」——区别是数据库的退化**语义完全等价**（顺序扫描给出同样的行），而这里的第三层**有损**（召回受 top-N 限制）。

**差别在于「退化是否可等价」。** 数据库的正则下推失败可以退化成逐行匹配，结果集完全相同，只是慢。而这里的第三层退化到一个 BM25 预过滤再本地重筛，**结果集可能更小**——因为预过滤有自己的 top-N。所以这里必须给模型一条「空结果不证明语料没有」的警告，而数据库不需要给用户任何警告（只是变慢）。**只有当退化会改变结果集时才需要「降级告知」这个机制。**

与 g02（知识图谱）的关系是互补通道：图行走回答「这两个概念之间有什么关系」，grep 回答「原文里有没有这个词」。两者都返回原文段落，但一个靠关系边、一个靠字面匹配。而 `compile_kwd` 那个过滤条件说明两套通道要**排除对方的产物**：grep 排除编译产物（那些是图编译的中间物），图行走则专门读它们。

与 g01（agentic 主循环）的关系有两处：`instrumentedTool` 里的 `retrievalaccount.AccountToolOutput` 把 `grep_chunks` 归为 **shallow read**（片段窗口），所以它不喂 `docIDLedger`——「通过它们看到一篇文档是定位而不是检索」；而 g01 的看门狗 `locateWatch` 存在的目的正是「模型只用 locate 腿而从不深读」。

与 e04 / f06 的关系是「上限与超时」的第三层：引擎用 `recursionLimit` 管图深度、workflowx 用 `maxIterations` 管迭代、而这里用引擎自身的 `max_determinized_states` 管一个查询的复杂度——三者都是「防止无限消耗」，但前两个是我们自己的常量，第三个是**别人的预算**，所以我们只能适应它（降级），不能配置它。

对 AI / agent 工程最实用的一条是**错误消息要给出「该写什么来代替」**。这一页的 `regexpPushdownHint` 是这条原则的密集示范：四种失败模式各自给出一个具体的、可粘贴的替代写法。而它背后的实测证据很有说服力——一个只报 `query_shard_exception` 的工具让模型浪费了一次重试然后放弃整轮。**判据是：模型读完这条错误后，能不能立刻写出一个不同的调用？** 不能的话这条错误就没用。

另一条是「失败的四分类」。把「空结果 / 做不到 / 太贵 / 被取消」区分开，是 agentic 工具有效性的关键。多数实现的 `catch (e) { return [] }` 会把前两种都变成第三种的一个特例，而模型会认为「查了但没有」。**「做不到」必须报错，「太贵」必须给替代方案，「被取消」必须终止**——三种响应不能互换。

最后一条是「降级要标记」。任何近似、缓存、超时兜底、预过滤的结果，其可信度与正常路径不同，而这个差别**必须出现在给模型的文本里**，不能只写在日志里。因为模型是根据文本做决定的，日志是给人看的——而这里需要一个给模型看的「可信度」字段。
