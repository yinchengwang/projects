---
project: ragflow
feature: chunk-store-es
title: ES 与 OpenSearch 块存储
page_type: feature
schema_version: 1
refs:
  - path: internal/engine/elasticsearch/chunk.go
    symbols: [SearchResponse]
  - path: internal/engine/opensearch/chunk.go
    symbols: [SearchResponse]
---

# ES 与 OpenSearch 块存储

## 一句话职责

两个近三千行的块存储实现，逐条函数名几乎一一对应，而 `SearchResponse` 这个结构体在两边**逐个字段、连注释都相同**——它们的差异不在「返回什么」，而在**怎么把「文本腿 + 向量腿」这个双路检索交给两个不同的 Lucene 封装**。

## 机制是什么

两个后端的函数清单可以直接对齐：

| 职责 | Elasticsearch | OpenSearch |
|---|---|---|
| 建索引 | `CreateChunkStore` | `CreateChunkStore` |
| 写入（等刷新） | `InsertChunks` → `refresh="wait_for"` | 同 |
| 写入（不等刷新） | `InsertChunksNoRefresh` → `refresh=""` | 同 |
| 写入主体 | `insertChunks(..., refresh)` | 同 |
| 更新 | `UpdateChunks` / `updateSingleChunk` / `updateChunksByQuery` | 同 |
| 页码权重 | `AdjustChunkPagerank` | 同 |
| 删除 | `DeleteChunks` | 同 |
| 检索 | `Search` | `Search` |
| 正则检索 | `SearchByRegexp` | 同 |
| 深分页 | `searchAfterCursor` / `searchAfterPaginate` | 同 |
| 分数提取 | `KNNScores` / `GetScores` | 同 |

**而两边共用的东西被显式抽到一个内部包里**，包名就叫 `esoscommon`（ES/OS common）。那个包里第一条函数注释把抽出来的动机写得毫不含糊：

> `SanitizeString` 之前在 Elasticsearch 与 OpenSearch 引擎里逐字节重复；现在两边都调这份共享实现，**这样两个后端就不会漂移**。

**所以这一页的正确读法是：文件里剩下的每一处「不一样」都是有意保留的。** 差异可以归成四类：

| 差异类 | 具体表现 |
|---|---|
| **客户端与调用方式** | ES 用 `elasticsearch.Client` 的 typed API；OS 用 `opensearch.Client.Perform` 拼路径 + `opensearchapi` |
| **双路检索的融合机制** | ES 用顶层 `knn` 参数与 `boost`；OS 用 `hybrid` 查询 + `search_pipeline` 的 `normalization-processor` |
| **相似度分值空间** | ES 用 `knn.similarity`（余弦口径）；OS 用 `cosinesimil` 的 `score = (1+cos)/2` 且需 clamp |
| **深分页的前置条件** | ES **要求**显式 `OrderBy`；OS 允许合成一个兜底排序 |

**而这四类里，第三类是最容易出错、也最能说明「为什么不能合并」的一处。**

## 怎么实现的

### 一、`SearchResponse`：唯一逐字相同的东西

```
type SearchResponse struct {
	Hits struct {
		Total struct {
			Value int64 `json:"value"`
		} `json:"total"`
		Hits []struct {
			ID        string                 `json:"_id"`
			Index     string                 `json:"_index"`
			Score     float64                `json:"_score"`
			Source    map[string]interface{} `json:"_source"`
			Fields    map[string]interface{} `json:"fields"` // ES 9.x stores dense_vector here
			Highlight map[string]interface{} `json:"highlight,omitempty"`
			// Sort is populated when the request body specifies a `sort`
			// clause. The last hit's Sort is the cursor for the next
			// search_after request 鈥?without it, deep pagination can't
			// advance.
			Sort []interface{} `json:"sort,omitempty"`
		} `json:"hits"`
	} `json:"hits"`
	Aggregations map[string]interface{} `json:"aggregations"`
}
```

**这份结构体被逐字复制到两个包里，注释连「ES 9.x stores dense_vector here」这句都保留在 OpenSearch 那份。**

**它没有被搬进 `esoscommon` 的原因可以从 `Fields` 那行的注释推断出来：** 这个结构描述的是**线上格式**，而线上格式是各家版本自己的事。**共享一个「线上格式的本地镜像」会让两个后端被迫在同一个 PR 里一起升级** —— 而它们本来就允许各自落后。

**而 `Fields` 这一支揭示了一个跨版本的问题。** ES 9.x 把 `dense_vector` 从 `_source` 挪到了 `fields`（因为向量太大不该随文档返回），于是「取回向量」这件事在 8.x 与 9.x 上是不同的路径。**两份结构都留了 `Fields` 字段，说明两边都已按「向量可能在 `fields` 里」处理。**

**而 `Sort` 那行注释直接说明了它的用途：**

> 当请求体指定 `sort` 子句时填充。**最后一个命中的 `Sort` 就是下一次 `search_after` 请求的游标 —— 没有它深分页无法推进。**

**这是一条把「响应结构」与「分页协议」绑在一起的说明**：`Sort` 不是为了展示，而是为了当游标。

### 二、ES：`knn` 是顶层参数，融合靠 `boost`

**ES 的 `Search` 把向量检索编成一个独立的顶层 `knn` 键。**

```
knnQuery := map[string]interface{}{
	"field":          vectorFieldName,
	"query_vector":   matchDense.EmbeddingData,
	"k":              k,
	"num_candidates": numCandidates,
	"similarity":     similarity,
	"filter":         cloneFilterSnapshot(boolQuery),
}
queryBody["knn"] = knnQuery
```

**而文本腿被塞进同一个 bool 的 `must`，然后用 `boost` 调和两腿权重。**

```
if hasVectorMatch {
	boolMap["boost"] = 1.0 - vectorSimilarityWeight
}
```

**这行 `boost` 前面有一段注释，说的是「不加会怎样」：**

> 加权求和融合把文本腿按 `(1 - w)` 缩放、与向量腿的 `w` 成对；而纯文本查询没有这种配对，所以 bool 过滤器包装**不能**施加常数 boost —— **否则每个 BM25 分数都会被悄悄乘上 0.5。**
>
> 记录它是因为**这个 boost 决定了第一阶段的排序**，从而决定哪些候选能活过调用方的 size 上限。

**两个要点：**

1. **`boost` 只在 `hasVectorMatch` 时加** —— 因为纯文本查询没有配对的那一半。
2. **它是「第一阶段排序」的决策者** —— 于是它错了一次，错的不是分数显示，而是**哪些块被选中**。

**`filter` 用 `cloneFilterSnapshot` 快照化。**

```
// cloneFilterSnapshot freezes the bool query's shape at the point the kNN clause
// is built. The reference does the same by passing bool_query.to_dict() as the
// kNN filter; the rank_feature clauses are appended to the same bool afterwards
// and belong to the scored query only.
```

**「`rank_feature` 子句随后被追加到同一个 bool，而它们只属于被打分的查询。」**

**这是一个非常细的区分：kNN 的 `filter` 必须是「结构过滤」（kb_id、available_int 那些），不能包含「打分用的 rank_feature」。** 而由于两者共用同一个 bool 对象，必须**在合适的时刻做一份快照**，否则后来追加的 rank_feature 会漏进 filter 里。

**而 `cloneFilterSnapshot` 的实现只做两层浅拷贝 + 对四个子句数组 `slices.Clone`：**

```
for _, key := range []string{"must", "should", "filter", "must_not"} {
	if clauses, ok := inner[key].([]interface{}); ok {
		innerClone[key] = slices.Clone(clauses)
	}
}
```

**「为什么是这四个」：** ES 的 bool 查询恰好有这四个子句位，而 `append` 到其中任一个都会修改底层数组（若容量够）——**所以只拷贝容器不够，还要拷贝每个子句列表。**

**这是一个「浅拷贝陷阱」的正确处理**：拷贝 map 却不拷贝切片，`append` 会写进共享的数组。

**最后是「纯向量查询不要把 bool 当打分查询」。**

```
// A DENSE-ONLY request (no text expression: the vector-only search and the
// knowledge-compile lookups) leaves the top-level query unset. The same scope
// conditions already ride inside knn.filter, so repeating them as a scored query
// adds one scoring pass per search AND blends that score into _score ——
// which is exactly the number a vector-only caller reads as the cosine similarity.
```

**「把范围条件再当一次打分查询，会多一遍打分，而且把那个分数混进 `_score` —— 而 `_score` 正是纯向量调用方当作余弦相似度读的那个数。」**

**这是全页最锋利的取舍：** 同一个 bool 既可以是「过滤器」也可以是「打分器」，而把它同时当两者会**污染调用方读的分数**。

### 三、OpenSearch：`knn` 是子句，融合靠 `search_pipeline`

**OS 的向量检索被编成一个查询子句，而且注释直接点明两边 DSL 不同：**

```
// Build the KNN query as an OpenSearch query sub-clause —— not ES's
// top-level "knn" parameter (the OpenSearch Python SDK has no KNN
// encapsulation, and its DSL for KNN differs from Elasticsearch's).
```

**kNN 的 filter 只装结构过滤，文本腿被刻意排除：**

```
// The knn filter holds only the structural filters (kb_id,
// available_int, ...). The text query is deliberately kept out of it:
// it's scored as its own leg in the hybrid query below, not used to
// pre-filter knn candidates (mirrors Python opensearch_conn.py).
```

**而 `must_not` 必须带上 —— 这一行注释记录了一个真实的遗漏风险：**

```
// Carry the exclusions too: CompilationFilter emits must_not
// clauses (e.g. page_type_kwd), and dropping them here would let
// the vector leg return documents the text leg excludes.
```

**「若不带上排除子句，向量腿会返回文本腿排除掉的文档」** —— 一个典型的「过滤器只搬了一半」缺陷。

**双路同时在时，走 `hybrid` 查询 + 内联 `search_pipeline`。**

```
case hybrid:
	queryBody["query"] = map[string]interface{}{
		"hybrid": map[string]interface{}{
			"queries": []interface{}{keywordQuery, map[string]interface{}{"knn": knnQuery}},
		},
	}
	queryBody["search_pipeline"] = map[string]interface{}{
		"phase_results_processors": []interface{}{
			map[string]interface{}{
				"normalization-processor": map[string]interface{}{
					"normalization": map[string]interface{}{"technique": "min_max"},
					"combination": map[string]interface{}{
						"technique": "arithmetic_mean",
						"parameters": map[string]interface{}{"weights": hybridWeights},
					},
				},
			},
		},
	}
```

**这是一种与 ES 完全不同的融合哲学：**

| | ES | OS |
|---|---|---|
| 融合位置 | **客户端构造请求体**（`boost` 加权） | **服务端 pipeline**（`normalization-processor`） |
| 归一化 | 无（BM25 与余弦各自口径） | `min_max` |
| 合并 | 由 `boost` 隐式缩放 | `arithmetic_mean` + 显式 `weights` |
| 权重载体 | 一个标量 `vectorSimilarityWeight` | `[2]jsonDouble` 投射到 2-单纯形 |

**OS 这套需要把权重投射到「和为 1 的二维单纯形」上，而这件事由 `parseHybridWeights` 干：**

```
// parseHybridWeights parses a "textWeight,vectorWeight" pair and projects it
// onto the 2-simplex [0.0, 1.0]^2 summing to 1.0. Any malformed, non-finite, or
// non-positive-sum input falls back to equal weights [0.5, 0.5].
```

**而它的边界处理非常完整，值得逐个记：**

```
if len(parts) != 2 { return defaultWeights }
textW, err0 := strconv.ParseFloat(...)
vecW, err1 := strconv.ParseFloat(...)
if err0 != nil || err1 != nil || math.IsNaN(textW) || math.IsInf(textW, 0) || math.IsNaN(vecW) || math.IsInf(vecW, 0) {
	return defaultWeights
}
if textW < 0 { textW = 0 }
if vecW < 0 { vecW = 0 }
total := textW + vecW
if total <= 0 { return defaultWeights }
if vecW == 0 { return [2]jsonDouble{1.0, 0.0} }
if textW == 0 { return [2]jsonDouble{0.0, 1.0} }
bm25W := textW / total
return [2]jsonDouble{jsonDouble(bm25W), jsonDouble(1.0 - bm25W)}
```

**五个决定，每个都在防一类具体输入：**

1. **`len != 2` 兜底** —— 词数不对。
2. **`NaN` / `Inf` 兜底** —— 浮点毒值（`ParseFloat` 会接受 `NaN` 与 `Inf` 的字面写法）。
3. **负值钳到 0** —— 而不是拒绝。
4. **`total <= 0` 兜底** —— 两个都是 0（否则除以零）。
5. **单边为 0 时直接返回 `(1,0)` / `(0,1)`** —— **这两个特例是为了避免「用 `1 - bm25W` 算出 `1 - 1 = 0` 之外的浮点误差」**，同时也保证「纯文本」与「纯向量」是精确的端点值。

**而 `bm25W := textW / total` 与 `jsonDouble(1.0 - bm25W)` 里，「向量权重」是**减出来的**而不是除出来的。** 这让两个权重严格和为 1。

**`jsonDouble` 这个类型本身也是一个后端特有的坑：**

```
// jsonDouble serializes as a JSON floating-point literal with an explicit
// decimal point so schema-less OpenSearch XContent maps (such as inline
// search_pipeline parameters) always decode it as java.lang.Double rather
// than java.lang.Integer.
```

**「无 schema 的 XContent map 会把 `0` 解成 `Integer` 而不是 `Double`」** —— 于是 `weights: [0.5, 0.5]` 若某个值是 `1`（而不是 `1.0`），服务端会拿到 `Integer` 并可能因此报类型错误。

**`MarshalJSON` 的实现因此做两件事：拒绝非有限值、若序列化结果没有小数点则补 `.0`。**

```
if math.IsNaN(v) || math.IsInf(v, 0) { return nil, fmt.Errorf(...) }
s := strconv.FormatFloat(v, 'f', -1, 64)
if !strings.ContainsRune(s, '.') { s += ".0" }
```

**这是一个「用自定义序列化器弥补无类型 schema 的坑」的实例** —— 而它存在的唯一理由是 OS 的 pipeline 参数走的是无 schema 路径。

### 四、相似度分值空间：OS 必须做一次变换与一次 clamp

**ES 侧的关键词是 `similarity`：**

```
similarity := 0.0
if matchDense.ExtraOptions != nil {
	if sim, ok := matchDense.ExtraOptions["similarity"].(float64); ok {
		similarity = sim
	}
}
...
"similarity": similarity,
```

**ES 的 `knn.similarity` 直接就是余弦下限**，不需要换算。

**而 OS 侧有三层：**

**第一层：从三个可能的键名里取阈值。**

```
// denseSimilarityThreshold extracts the requested cosine similarity floor from
// a MatchDenseExpr. Callers pass the threshold under "similarity" (retrieval,
// memory, skill, kg), "threshold" (Infinity convention), or "min_score"
// (knowledge_compile reader).
for _, key := range []string{"similarity", "threshold", "min_score"} {
```

**「三个键名来自三个不同的调用方」** —— 这是「统一接口下各家约定不统一」的直接证据。**而 `threshold` 被标注为「Infinity 的约定」**，说明调用方把 Infinity 的写法也传到 OS 上了。

**第二层：余弦阈值 → OS 的 `cosinesimil` 分值空间。**

```
// cosineThresholdToOpenSearchScore converts a cosine similarity threshold in
// [-1, 1] into OpenSearch's cosinesimil score space: score = (1 + cos) / 2.
// Values above 1.0 are preserved above 1.0 so top-level min_score cleanly
// filters out every hit without raising an OpenSearch validation error.
func cosineThresholdToOpenSearchScore(sim float64) float64 {
	return (1.0 + sim) / 2.0
}
```

**「`cosinesimil` 的分值空间是 `(1 + cos) / 2`，即 `[0, 1]` 而不是 `[-1, 1]`」** —— 所以阈值必须换算。

**而「大于 1.0 的值保留大于 1.0」是为了「用 `min_score` 干净地滤掉全部命中，而不是触发校验错误」。** 这是一个**故意的越界**：用一个不可能满足的阈值表达「什么都不返回」。

**第三层：clamp 到一个 float32 的边界常量。**

```
// maxOpenSearchKNNClauseMinScore is the highest float32 value strictly below
// 1.0. OpenSearch's knn clause `min_score` rejects values > 1.0 with 400 and
// evaluates radial score comparisons with float32 strictness that excludes
// exact-identical vectors (score == 1.0) at min_score=1.0. Clamping a
// cosine=1.0 threshold to this boundary keeps identical vectors eligible while
// still excluding non-identical vectors.
const maxOpenSearchKNNClauseMinScore = 0.99999994
```

**这段注释是整页技术密度最高的地方，三句话讲了三个事实：**

1. **`min_score > 1.0` 会返回 400** —— 所以前面「保留大于 1.0」只适用于**顶层** `min_score`，不适用于 knn 子句的 `min_score`。
2. **OS 用 float32 严格比较** —— 于是 `score == 1.0` 在 `min_score = 1.0` 时被判为**不满足**。
3. **`0.99999994` 是「严格小于 1.0 的最大 float32」** —— 用它当阈值能让「完全相同向量」通过，同时排除其它。

**而它不是随便取的：`0.99999994` 正是 float32 里 1.0 的前一个可表示值（`1 - 2^-24` 的近似）。**

**两处 `min_score` 的用法不同，这也是代码里显式分开写的：**

**knn 子句内（会触发 radial search，与 `k` 互斥）：**
```
delete(knnClause, "k")
knnClause["min_score"] = clauseMinScore
```
**注释说明了为什么必须删 `k`：**

> 把 `min_score` 放进 knn 子句与 `k` 互斥，会触发 radial search；而顶层 `min_score` 会在 `cosinesimil` 分值空间里对 k 个最近邻做后置过滤，**效果与 ES 的 `knn.similarity` 语义一致，且没有 radius 搜索的开销。**

**于是两处用法是「带过滤的 radial 搜索」与「先取 k 再后置过滤」的差别，而 OS 只在 memory 索引上选前者。**

**顶层（纯向量路径）：**
```
case hasVectorMatch && !memoryVectorImpossible:
	queryBody["query"] = map[string]interface{}{"knn": knnQuery}
	if hasPositiveDenseThreshold {
		queryBody["min_score"] = cosineThresholdToOpenSearchScore(denseSim)
	}
```

**而这里「顶层 `min_score` 不用 clamp」** —— 因为顶层不会 400，越界反而能表达「全部滤掉」。

**还有一个 `memoryVectorImpossible` 的分支，逻辑更绕但很实用：**

```
if isMemoryIndex && hybrid && hasPositiveDenseThreshold {
	if denseSim > 1.0 {
		memoryVectorImpossible = true
		hybrid = false
	} else if knnClause, ok := knnQuery[...]; ok {
		clauseMinScore := cosineThresholdToOpenSearchScore(denseSim)
		if clauseMinScore >= 1.0 { clauseMinScore = maxOpenSearchKNNClauseMinScore }
		delete(knnClause, "k")
		knnClause["min_score"] = clauseMinScore
	}
}
```

**注释解释了为什么要动 knn 腿而不是只加顶层过滤：**

> Memory 检索直接用 `docEngine.Search()`，没有第二遍 rerank / postThreshold。**在 OS 的 hybrid 查询里文本腿与 knn 腿是并集（union），所以一个未过滤的 knn 腿即使文本腿一条都没命中，也会漏出低相似度的消息。**

**「并集」这个词是关键**：hybrid 不是交集，所以「只过滤文本腿」等于什么都没过滤。

### 五、深分页：ES 要求显式排序，OS 允许合成

**两边都有同样的触发条件。**

```
useSearchAfter := limit > 0 && (offset+limit > common.MAX_RESULT_WINDOW) && !hasDense
```

**`MAX_RESULT_WINDOW = 10000`**，而「有向量匹配时不走 search_after」两边一致。

**但 ES 多一个前置条件 —— 必须有显式 `OrderBy`：**

```
// The preconditions mirror the Python reference (rag/utils/es_conn.py):
//   - explicit OrderBy is required (search_after needs a stable cursor,
//     and _score / KNN-similarity sorts are not unique enough to be safe)
//   - no dense/KNN match (knn queries do not honour `search_after` in the
//     same way and the Python path explicitly disallows them here)
hasDense := hasVectorMatch
hasExplicitSort := req.OrderBy != nil && len(req.OrderBy.Fields) > 0
useSearchAfter := limit > 0 && (offset+limit > common.MAX_RESULT_WINDOW) && hasExplicitSort && !hasDense
```

**「`_score` 与 KNN 相似度排序不够唯一，不足以保证游标安全」** —— 这是 `search_after` 的固有限制：**游标必须能唯一确定位置，而分数会并列。**

**OS 则自己合成一个兜底的确定性排序，注释列出了三个步骤：**

```
// resolveSearchAfterSort builds a deterministic, fielddata-free sort order
// for deep pagination:
//   - when no explicit OrderBy was supplied on a relevance-scored query, lead
//     with _score desc so deep text results keep relevance order and non-zero
//     hit scores;
//   - finish with the index's mapped keyword primary-key field ("skill_id" for
//     skill indices, "id" otherwise) rather than _id so search_after relies on
//     doc_values instead of heap fielddata / indices.id_field_data.enabled;
//   - when spanning multiple indices in one request, add _index as final
//     tiebreaker so identical document IDs across indices remain strictly ordered.
```

**三步各自防一个具体问题：**

| 步骤 | 防什么 |
|---|---|
| `_score` desc 打头 | 深页结果丢失相关性顺序、分数被清零 |
| 用 `id` / `skill_id` 而非 `_id` 兜底 | `_id` 排序要靠 heap fielddata（`indices.id_field_data.enabled=false` 时直接失败） |
| 跨索引时加 `_index` | 不同索引里同 id 文档的严格全序 |

**「`_id` 排序要靠堆 fielddata」是一个只有 Lucene 系才知道的坑** —— 而 ragflow 的选择是「显式用 doc_values 里的 keyword 主键」。**`unmapped_type: "keyword"` 也是为此**：在没有这个字段的索引里不报错而是当作缺失。

### 六、两个「等刷新 vs 不等刷新」的写入

**`InsertChunks` 与 `InsertChunksNoRefresh` 共用一个 `insertChunks`，差别只有一个字符串。**

```
func (e *Engine) InsertChunks(...) { return e.insertChunks(ctx, ..., "wait_for") }
func (e *Engine) InsertChunksNoRefresh(...) { return e.insertChunks(ctx, ..., "") }
```

**而 `InsertChunks` 的注释把「为什么调用方要读回来」讲清楚了：**

> 它在返回前等索引刷新，于是**紧接着读块（块 API、debug 端点）的调用方能马上看到**。**摄取必须用 `InsertChunksNoRefresh`** —— 见那个方法里的说明。

**而那个方法的注释给了实测数据：**

> 它们会在索引的正常刷新周期可见（KB 索引出厂带 `refresh_interval=1000ms`），所以差别最多一秒的可见性 —— **对比每次写入纯等待约 0.5 秒中位数（p90 为 1.0 秒，最大 5.0 秒）的额外延迟。**
>
> 这是摄取路径的契约，也与 Python 一致：Python 摄取用 `refresh=False`，而它的 API 默认仍是 `"wait_for"`。

**「0.5 秒中位 vs 最多 1 秒可见性延迟」** —— 这就是全部理由。

**而两侧的批量刷出阈值也一致，注释同样给出了原因：**

```
// Bulk requests are executed in bounded batches so a single large document
// (thousands of chunks) never sends a payload above the Elasticsearch HTTP
// body limit (default http.max_content_length = 100MB), which surfaces as a
// 413 Request Entity Too Large. We flush a batch by chunk count AND byte
// size to stay well within the limit.
const (
	bulkBatchChunks = 500
	bulkBatchBytes  = 20 * 1024 * 1024 // 20MB, comfortably under the 100MB default
)
```

**「按条数**和**字节数各刷一次」** —— 两个维度都必须有，因为「500 条」可能是 500 个小块，也可能是 500 个大块。

**而 bulk 的错误处理是本页最值得记的一处工程细节：**

```
// Check for errors in bulk response. ES reports a 200 at the top level
// even when individual bulk items fail, so we must inspect the items and
// surface the reasons instead of returning success while the chunks were
// silently dropped.
if hasErrs, ok := bulkResponse["errors"].(bool); ok && hasErrs {
	...
}
```

**「ES 在顶层返回 200，即使个别 bulk 项失败」** —— 于是「不看 `items` 就当成功」会让块静默丢失。**这是一个只有写过 bulk 的人才会提前想到的检查。**

### 七、`AdjustChunkPagerank`：两边都用脚本，而写入方式不同

**两边的 painless 脚本一字不差，包括 kb_id 校验与钳位逻辑。**

```
if (ctx._source.kb_id == null || !ctx._source.kb_id.equals(params.kb_id)) {
	ctx.op = 'noop';
} else {
	double current = 0.0;
	if (ctx._source.containsKey(params.field) && ctx._source[params.field] != null) { ... }
	double next = current + params.delta;
	if (next < params.min_weight) { next = params.min_weight; }
	if (next > params.max_weight) { next = params.max_weight; }
	if (next <= 0.0) { ctx._source.remove(params.field); }
	else { ctx._source[params.field] = next; }
}
```

**四个决定：**

1. **`kb_id` 不符时 `ctx.op = 'noop'`** —— 不报错，而是**什么都不做**，然后由 Go 侧把 `noop` 转成「这个块不属于该数据集」的错误。
2. **`currentValue` 兼容 `Number` 与可解析的字符串** —— 因为 `pagerank_fea` 的类型可能因动态映射而不同。
3. **钳位到 `[min_weight, max_weight]`** —— 上下界由调用方给。
4. **`next <= 0.0` 时**删除**这个字段而不是写 0** —— 于是「没有 pagerank」与「pagerank 为 0」在存储层被区分开。

**而两边唯一的差别是提交方式。**

**ES 用 typed `UpdateRequest` + `RetryOnConflict`：**
```
retryOnConflict := 3
req := esapi.UpdateRequest{
	Index: indexName, DocumentID: chunkID, Body: bytes.NewReader(body),
	RetryOnConflict: &retryOnConflict,
}
```

**OS 用自己拼的 HTTP POST + query 参数，并带一条注释解释 `retry_on_conflict` 的用途：**
```
// Concurrent feedback on the same chunk must not surface as a 409, so ask
// the engine to retry the scripted update on version conflicts.
updateResp, err := e.doUpdate(ctx, indexName, chunkID, bytes.NewReader(body), "", map[string]string{
	"retry_on_conflict": "3",
})
```

**「同一块的并发反馈不能被暴露成 409」** —— 这是 pagerank 调整的真实场景：多个查询同时命中同一块并各加一点权重。

**而 `noop` 的检测两边都做了：**

```
if updateResp.Result == "noop" { return fmt.Errorf("chunk %s does not belong to dataset %s", chunkID, kbID) }
```

**「用 `noop` 来表达归属错误」是一个巧妙但隐晦的通道** —— 脚本无法返回业务错误，只能改 `ctx.op`，于是 Go 侧把「没改动」解释成「不属于我」。

## 关键取舍

**源码事实：** `SearchResponse` 在两份文件里逐字相同（含「ES 9.x stores dense_vector here」与 `Sort` 游标注释），且没有被搬进共享包 —— 因为它描述的是**线上格式**，两个后端允许各自落后。

**源码事实：** 共享部分被抽到 `internal/engine/internal/esoscommon`，`SanitizeString` 的注释写明动机：「两边都调这份共享实现，这样两个后端就不会漂移」。

**源码事实：** ES 的向量检索是**顶层 `knn` 参数**，文本腿进 bool 的 `must` 并用 `boolMap["boost"] = 1.0 - vectorSimilarityWeight` 调和；注释警告「纯文本查询不该加这个 boost，否则每个 BM25 分数被悄悄乘 0.5」，并指出该 boost「决定第一阶段排序，从而决定哪些候选活过 size 上限」。

**源码事实：** ES 用 `cloneFilterSnapshot` 在构造 kNN 子句时**冻结 bool 的形状**，因为 `rank_feature` 子句随后被追加到同一个 bool、且只属于打分查询；快照对 `must`/`should`/`filter`/`must_not` 四个数组各做 `slices.Clone`（浅拷贝 map 不够，`append` 会写进共享数组）。

**源码事实：** ES 纯向量请求**不设顶层 `query`**（DENSE-ONLY），因为范围条件已在 `knn.filter` 里，再当打分查询会多一遍打分并把那个分数混进 `_score` —— 而 `_score` 正是纯向量调用方读作余弦相似度的数。

**源码事实：** OS 的 kNN 是**查询子句**而非顶层参数（注释：OpenSearch Python SDK 没有 KNN 封装，且其 DSL 与 ES 不同）；kNN filter 只装结构过滤，**必须带上 `must_not`** —— 否则向量腿会返回文本腿排除掉的文档。

**源码事实：** OS 双路同时在时用 `hybrid` 查询 + 内联 `search_pipeline` 的 `normalization-processor`（`min_max` 归一化 + `arithmetic_mean` 合并 + 显式权重），而 ES 用客户端 `boost` 加权 —— 融合位置不同（服务端 vs 客户端）。

**源码事实：** `parseHybridWeights` 把权重投射到和为 1 的二维单纯形，五道兜底（词数、NaN/Inf、负值钳 0、总和 ≤ 0、单边为 0 直接返回端点），且「向量权重」是 `1 - bm25W` **减出来的**而非除出来的。

**源码事实：** `jsonDouble` 自定义序列化补 `.0`，因为「无 schema 的 XContent map 会把它解成 `java.lang.Integer` 而不是 `Double`」；同时拒绝 NaN/Inf。

**源码事实：** OS 的相似度需要两次变换：余弦 `[-1,1]` → `cosinesimil` 的 `(1+cos)/2`；且 knn 子句 `min_score` 必须 clamp 到 `0.99999994`（严格小于 1.0 的最大 float32）—— 因为 OS 用 float32 严格比较，`score == 1.0` 在 `min_score = 1.0` 时被判为不满足，而完全相同的向量应当保留。

**源码事实：** 阈值键名有三个：`similarity`（retrieval/memory/skill/kg）、`threshold`（Infinity 约定）、`min_score`（knowledge_compile 读者）—— 说明调用方把不同后端的约定也传到了同一个入口。

**源码事实：** OS 的 `memoryVectorImpossible` 分支：阈值 `> 1.0` 时**丢弃向量腿**（因为没有任何余弦能超过 1.0，而 OS 的子句 `min_score > 1.0` 会 400）；否则把 knn 子句改成 `min_score` 并**删掉 `k`**（两者互斥、会触发 radial search）。理由：OS hybrid 的两腿是**并集**，未过滤的 knn 腿会漏出低相似度消息。

**源码事实：** 深分页触发条件两边相同（`offset+limit > 10000` 且无向量匹配），但 ES **额外要求显式 `OrderBy`**（「`_score` 与 KNN 相似度排序不够唯一」）；OS 用 `resolveSearchAfterSort` 合成确定性排序：`_score desc` 打头、用 `id`/`skill_id`（而非 `_id`，避免 heap fielddata）、跨索引加 `_index` 兜底。

**源码事实：** `InsertChunks`（`refresh="wait_for"`）供读回场景，`InsertChunksNoRefresh`（`refresh=""`）供摄取；注释给了实测理由：「约 0.5 秒中位（p90 1.0 秒，最大 5.0 秒）纯等待延迟」对「最多 1 秒的可见性延迟」。

**源码事实：** bulk 批处理按「条数与字节数两个维度」刷出（500 条 / 20MB），因为 500 条可能是 500 个小块也可能是 500 个大块；且**必须检查 `items` 里的逐项错误**——「ES 在顶层返回 200，即使个别 bulk 项失败」。

**源码事实：** `AdjustChunkPagerank` 的 painless 脚本两边逐字相同：kb_id 不符时 `ctx.op='noop'`、`currentValue` 兼容 Number 与字符串、钳位到 `[min,max]`、`next <= 0` 时**删除字段**而非写 0。差别只在提交方式（ES 用 `RetryOnConflict: 3`，OS 用 query 参数 `retry_on_conflict=3`），注释理由统一：「同一块的并发反馈不能被暴露成 409」。

**架构推论：** 这两个文件不合并的理由不是「代码不同」而是「**线上契约不同**」。ES 与 OS 的 `knn` 位置、分值空间、pipeline 机制、`_id` 排序能力都在版本间变化，而 ragflow 必须同时支持一批不同的服务端版本。**把「能共享的算法」抽出来（`esoscommon`）、把「不能共享的协议」留在原地，是唯一不会被迫同步升级的形态。** 代价是近三千行的重复 —— 而重复的部分恰好是「与协议交互」的部分，它的正确性本来就该由各自的集成测试保证。

**架构推论：** 三处「同一个概念在不同后端有两个名字」的痕迹值得记：`similarity` / `threshold` / `min_score`；`_score` / `SCORE`；`query_vector` / `vector`。**当一个接口同时服务多家后端时，最危险的不是「少一个能力」，而是「同一个参数在不同后端含义不同」。** `denseSimilarityThreshold` 用三个键名轮询就是在做这件事的补偿 —— 而它轮询的是**调用方**的多样性（不同模块各自沿用自己那家的约定），不是后端的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/engine/elasticsearch/chunk.go` | `SearchResponse` | ES 线上格式的本地镜像（含 `Fields` 与 `Sort`） |
| `internal/engine/elasticsearch/chunk.go` | `CreateChunkStore` | 三种 mapping（memory/skill/default），default 只显式声明 `tag_feas` |
| `internal/engine/elasticsearch/chunk.go` | `insertChunks` / `InsertChunks` / `InsertChunksNoRefresh` | 三分支刷新策略与两维批量刷出 |
| `internal/engine/elasticsearch/chunk.go` | `Search` | 顶层 `knn` + `boost` 调和 + DENSE-ONLY 不设 query |
| `internal/engine/elasticsearch/chunk.go` | `cloneFilterSnapshot` | 冻结 bool 形状，避免 rank_feature 漏进 kNN filter |
| `internal/engine/elasticsearch/chunk.go` | `AdjustChunkPagerank` | painless 脚本 + `RetryOnConflict: 3` |
| `internal/engine/elasticsearch/chunk.go` | `searchAfterCursor` / `searchAfterPaginate` | 游标式深分页（与 OS 共用的纯循环） |
| `internal/engine/opensearch/chunk.go` | `SearchResponse` / `mapToSearchResponse` | 同结构 + 从 `map` 反序列化的适配 |
| `internal/engine/opensearch/chunk.go` | `Search` | `hybrid` 查询 + `search_pipeline` 融合 |
| `internal/engine/opensearch/chunk.go` | `jsonDouble` | 强制浮点字面量（补 `.0`） |
| `internal/engine/opensearch/chunk.go` | `parseHybridWeights` | 权重投射到 2-单纯形，五道兜底 |
| `internal/engine/opensearch/chunk.go` | `denseSimilarityThreshold` | 三个键名轮询取阈值 |
| `internal/engine/opensearch/chunk.go` | `cosineThresholdToOpenSearchScore` | `(1+cos)/2` 换算 |
| `internal/engine/opensearch/chunk.go` | `maxOpenSearchKNNClauseMinScore` | `0.99999994`，float32 严格比较的边界 |
| `internal/engine/opensearch/chunk.go` | `resolveSearchAfterSort` | 合成确定性排序（`_score` / `id` / `_index`） |
| `internal/engine/internal/esoscommon/chunkfuncs.go` | `BuildRankFeatureQuery` | `emitPagerankRankFeature` 区分两边是否重复计 pagerank |
| `internal/engine/internal/esoscommon/chunkfuncs.go` | `CalculateScores` | `scoreColumn` + `pagerankField` 两路累加 |
| `internal/engine/internal/esoscommon/esoscommon.go` | `SanitizeString` | 从两份重复抽出的共享实现 |
| `internal/engine/internal/esoscommon/elide_vector.go` | `ElideQueryVector` | 日志里把向量替换成 `<elided>` |
| `internal/engine/internal/esoscommon/elide_vector.go` | `QueryVectorFieldName` / `VectorFieldName` | `query_vector`（ES）与 `vector`（OS）两个名字 |
| `internal/common/constants.go` | `MAX_RESULT_WINDOW` / `SearchAfterBatchSize` | 10000 / 1000 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：elasticsearch/chunk.go](https://github.com/infiniflow/ragflow/blob/main/internal/engine/elasticsearch/chunk.go)
- [ragflow 源码：opensearch/chunk.go](https://github.com/infiniflow/ragflow/blob/main/internal/engine/opensearch/chunk.go)
- [Elasticsearch 文档：kNN 检索与 similarity](https://www.elastic.co/guide/en/elasticsearch/reference/current/knn-search.html)
- [OpenSearch 文档：hybrid search 与 normalization processor](https://docs.opensearch.org/latest/vector-search/vector-search-techniques/hybrid-search/)
- [OpenSearch 文档：paginate results 与 search_after](https://docs.opensearch.org/latest/search-plugins/searching-data/paginate/)

## 与主核 / AI 的关系

这两个后端的关系，与主核里**两种索引访问方法（B-tree 与 GiST）** 的关系恰好相反。

**主核要求所有访问方法对同一查询给出语义一致的结果** —— 因为 SQL 的语义是统一的，「索引结果必须等于全表扫描结果」。**而 ES 与 OS 从来给不出相同的分数**：它们的 BM25 实现细节、`cosinesimil` 与 `knn.similarity` 的口径、归一化方式都不同，于是**同一个查询在两个后端上返回的候选集可以不同**。

**ragflow 接受了这件事，但把它限制在一个地方：`CalculateScores`。** 分数在两边都落到 `_score` 这个统一出口，而 `SearchResult.Chunks []map[string]interface{}` 是唯一的上层契约。**「语义不一致但出口一致」** —— 这是所有多后端系统的现实选择。

**「`min_score` 的 float32 严格比较」这个坑，在数据库里对应的是「索引里存的 `float4` 与查询里的 `float8` 比较」**：类型窄化会让边界判断偏移一格，而这类问题只在极值附近暴露。`0.99999994` 这个常量是「把那个偏移量显式写出来」的做法 —— **比注释写「注意浮点误差」要可靠得多**。

**「`_id` 排序要靠 heap fielddata」对应数据库里「用未建索引的列排序」**：两者都会退化成全扫描或失败，而正确做法都是「用一个有 doc_values / 有索引的替代键」。**`unmapped_type: "keyword"` 则对应数据库的 `COALESCE` 或默认值** —— 让「字段缺失」不成为错误。

**`ElideQueryVector` 这个日志脱敏在数据库里没有对应物**，因为数据库的查询计划里没有「1024 维浮点数组」这种东西。**它是向量检索特有的日志问题**：一条 debug 日志能带上几百 KB，而注释坦白了它的不精确：「这个形状变化是日志脱敏的预期副作用，不是无损变换」。

对 agent 工程的直接价值有三条。其一，**双路检索的融合位置决定可控性**。客户端加权（ES 的 `boost`）简单但受限于「分数口径不统一」；服务端 pipeline（OS 的 `normalization-processor`）能做真正的 min-max 归一化，代价是必须把权重投射成合法参数并处理无 schema 的类型坑。其二，**「并集」是 hybrid 的默认语义**。若只在一腿上做过滤，另一腿会漏出被排除的文档——这是 `must_not` 必须搬进 knn filter 的原因。其三，**bulk 的顶层状态码不可信**。任何「批量写入」API 若支持逐项错误，就必须检查逐项结果，否则数据会静默丢失。
