---
project: ragflow
feature: dataset-artifact
title: 知识库产物与技能检索
page_type: feature
schema_version: 1
refs:
  - path: internal/service/dataset_artifact_service.go
    symbols: [DatasetArtifactService, HasWiki, WikiPageItem]
  - path: internal/service/skill_search.go
    symbols: [SkillSearchService, SearchRequest, SearchResponse]
  - path: internal/service/dataset_structure_graph.go
    symbols: [StructureGraphNode, DocumentStructureGraphTemplate]
---

# 知识库产物与技能检索

## 一句话职责

知识编译（摄取流水线里的 Wiki / 技能 / 结构图）产出的一切**不落在关系库里，而是作为普通行写进文档引擎的租户索引**——以 `compile_kwd` / `type_kwd` 这类关键字区分种类；`DatasetArtifactService` 负责按这些关键字**读回来、投影成人可读的形状**，`SkillSearchService` 则在同一个引擎上以「技能空间」为独立索引做关键词/向量/混合检索。

## 机制是什么

这一层的核心事实是：**产物与原文共处一个索引，靠关键字区分，没有独立的表。**

摄取流水线编译出的东西（wiki 页面、wiki 实体、wiki 关系、技能、结构图节点、导航簇）全都写进 `ragflow_<tenant_id>` 这一个文档索引。区分它们的手段是一个字段：

```go
const (
    CompileKwdWikiPage     = "wiki_page"
    CompileKwdWikiEntity   = "wiki_entity"
    CompileKwdWikiRelation = "wiki_relation"
    CompileKwdWikiAlter    = "wiki_alter"
    CompileKwdSkill        = "skill"
    CompileKwdSkillAll     = "skill_all"
    CompileKwdDatasetNav   = "dataset_nav"
    defaultWikiGraphTopN   = 512
    maxWikiGraphTopN       = 1024
)
```

**这个设计的收益很大**：产物与原文可以被同一个检索请求一起搜到（一个 chunk 查询就能既命中原文也命中 wiki 页）；不需要为新产物建表、写迁移、加索引。**代价也同样大**，而这一页几乎每一处实现细节都是这个代价的体现：

1. **「同一条记录被写了几份」变成一个需要显式过滤的问题** ——同一份 wiki 页既有数据集级的合并行，也有每个源文档一份的原始行，必须靠 `available_int` 区分（下面细讲）。
2. **每条记录的内容必须塞进一个通用字段。** wiki 页的正文、实体的属性、关系的端点全都进 `content_with_weight`，读的时候再 `json.Unmarshal` 解回来。于是**「结构化数据」在存储层是「字符串」，类型安全由读端自行保证**。
3. **产物之间没有外键，只有名字。** wiki 关系用 `from_entity_kwd`/`to_entity_kwd` 指实体**名**而不是 id，于是「端点能不能对应上一个实体」要靠读取后归一（`normalizeRelationEndpoints`）。

## 怎么实现的

**`DatasetArtifactService` 里最关键的代码是那个私有字段 `docEngine` 与它的 nil 处理。**

```go
type DatasetArtifactService struct {
    // docEngine, when set, overrides the global engine returned by engine.Get()
    // for compiled-search calls. Tests inject a fake here; production leaves it
    // nil and falls back to the global engine.
    docEngine engine.DocEngine
}
```

**这个结构体只有一个字段，而且生产环境恒为 nil。** 它存在的全部理由是「测试要能注入假引擎」——而注释直接这么说了。

而使用它的地方有一段更值得看的写法：

```go
func (s *DatasetArtifactService) searchCompiledWithMatch(...) ([]map[string]interface{}, int64, error) {
    // Read the optional injected engine only when the receiver is non-nil so
    // that callers passing a nil *DatasetArtifactService (which the wiki-graph
    // error-path tests rely on) keep getting a clean error instead of a nil
    // dereference.
    var docEngine engine.DocEngine
    if s != nil {
        docEngine = s.docEngine
    }
    if docEngine == nil {
        docEngine = engine.Get()
    }
    if docEngine == nil {
        return nil, 0, fmt.Errorf("document engine is not initialized")
    }
```

**这段注释暴露了一个真实的契约：「调用方可以传 nil 接收者，并且必须得到一个错误而不是 panic」。** 一个 `nil` 的 `*DatasetArtifactService` 在 Go 里通常是「程序员 bug」，但这里被提升为「被测试依赖的行为」。于是方法在 `nil` 接收者上仍然可用（只要不碰字段），这就是为什么必须先 `if s != nil` 才能读 `s.docEngine`。

**这是一个值得记的取舍**：它让「服务未装配」这条错误路径可测试，但代价是**每个方法都得处理 nil 接收者**（否则只在那条路径上 panic）。只有 `searchCompiledWithMatch` 做了，其它方法都直接 `s.docEngine` 访问——所以真正的 nil 安全性只覆盖了「读方法的第一步」这一层。

**`searchCompiledWithMatch` 的一个动作顺序是必需的：先拷调用方 filter，再钉 dataset 范围。**

```go
// Copy caller filters first, then pin the dataset scope so kb_id always wins
// even if a caller passes its own kb_id.
merged := make(map[string]interface{}, len(filter)+1)
for k, v := range filter {
    merged[k] = v
}
merged["kb_id"] = []string{datasetID}
```

**「先拷再钉」的顺序保证结果是确定的：无论调用方传了什么 `kb_id`，最终都是 `datasetID`。** 若反向（先钉再拷），调用方就能覆盖它。

**这是一个「越权防护的正确写法」**：不是校验调用方参数合法（那要枚举所有可能），而是**无条件覆盖**。在 `graphRowSearch` 里是同一段逻辑的复制：

```go
merged := make(map[string]interface{}, len(filter)+1)
for k, v := range filter {
    merged[k] = v
}
merged["kb_id"] = []string{datasetID}
```

**两处独立的复制而不是一个共享的辅助函数** ——这是「同一规则写两遍」的小规模版本，而它在两个不同的文件里（`dataset_artifact_service.go` 与 `dataset_structure_graph.go`）。

**`HasWiki` 是这一类「存在性判断」的样板，而它的形状说明了为什么产物查询必须走 `total`。**

```go
func (s *DatasetArtifactService) HasWiki(ctx context.Context, tenantID, datasetID string) (bool, error) {
    _, total, err := s.searchCompiled(ctx, tenantID, datasetID,
        map[string]interface{}{"type_kwd": []string{CompileKwdWikiPage}}, nil, 0, 1, nil)
    if err != nil {
        return false, err
    }
    return total > 0, nil
}
```

**四处：**

1. **`limit=1`** ——只要知道有没有，不需要内容。
2. **用的是 `total` 而不是「返回了几行」** ——因为引擎的 `total` 是命中总数，与 `limit` 无关。这比「取一行看有没有」更准确（尤其在有过滤条件时）。
3. **`selectFields=nil`** ——不取任何字段。传 nil 让引擎只做计数。
4. **`HasSkill` 的判据是 `compile_kwd: skill_all`**，而 `HasWiki` 是 `type_kwd: wiki_page`。**两个函数用两个不同的字段名判断「有没有」** ——这是产物种类在存储层没有统一 schema 的直接后果（下面详述）。

**`ListWikiPages` 的 `available_int` 过滤是这一页最需要解释的一处。**

```go
func (s *DatasetArtifactService) ListWikiPages(ctx context.Context, tenantID, datasetID, pageType, topic, keywords string, page, pageSize int) ([]WikiPageItem, int64, error) {
    // Only surface the merged dataset-level pages. Each unique (page_type, slug)
    // can also have a per-document source row (available_int=0); without this
    // filter the same entity/concept would appear once per source doc. Python's
    // list_wiki_pages has no such duplication because its writer emits one row
    // per page, so mirror that by selecting the merged rows (available_int=1).
    filter := map[string]interface{}{
        "type_kwd":      []string{CompileKwdWikiPage},
        "available_int": 1, // merged dataset-level rows only (see engine available_int handling)
    }
```

**注释把来龙去脉讲清了：同一个 `(page_type, slug)` 存在两种行** ——一份是「数据集级合并行」（`available_int=1`），一份是「每个源文档一份的原始行」（`available_int=0`）。不过滤就会「同一个实体在每个源文档里出现一次」。

**`available_int` 原本是「这个 chunk 是否参与检索」的标记**（`agentic_rag` 的 grep/BM25 服务都用它过滤），而在这里被**复用成「这是合并行还是原始行」的区分**。这是一个字段承载两个正交语义的典型例子——而注释末尾那句「see engine available_int handling」把解释推给了引擎层。

**所以「产物列表」的正确性依赖三个默契**：写入方按 `available_int` 打标记、引擎按 `available_int=1` 默认可见、读取方再显式加一次。三处各自不知道另外两处的细节。

**`keywords` 的搜索走的是「批量扫描 + 内存匹配」，而注释给出了为什么要这样。**

```go
// Wiki list search is intentionally metadata-based (substring over
// title/slug/summary): those fields exist on every row and behave the same
// across document-store backends, unlike full-text token fields. The engine
// has no cross-field substring filter, so scan the filtered rows in
// batches, match in memory, then apply the requested page slice — exactly
// what Python list_wiki_pages does for keywords.
kw := strings.ToLower(keywords)
matched := make([]WikiPageItem, 0)
const batchSize = 1000
for scanOffset := 0; ; scanOffset += batchSize {
    batch, _, err := s.searchCompiled(ctx, tenantID, datasetID, filter,
        wikiPageSelectFields, scanOffset, batchSize, wikiPageOrderBy())
    if err != nil {
        return nil, 0, err
    }
    if len(batch) == 0 {
        break
    }
    for _, item := range wikiPageItemsFromChunks(batch) {
        if wikiPageItemMatchesKeyword(item, kw) {
            matched = append(matched, item)
        }
    }
    if len(batch) < batchSize {
        break
    }
}
```

**四点：**

1. **理由写得很清楚：「那些字段每一行都有，并且在不同的文档存储后端行为一致，而全文分词字段不是」。** 这是「跨后端一致性」压倒「查询效率」的选择——一个 `LIKE '%kw%'` 在 ES 与 Infinity 上的表达方式不同，而**内存里 `strings.Contains` 两边一样**。
2. **批量 1000 + `len(batch) < batchSize` 作为结束条件** ——标准的「不知道总数时靠短读判断扫完」。注意它在「恰好 1000 的整数倍」时会多扫一次空的（`len(batch) == 0` 再 break），无害但多一次往返。
3. **`total` 是 `len(matched)`** ——**即「内存里匹配到的总数」**，而不是引擎给的 `total`（那个是过滤后的行数，不含关键词约束）。这是必需的：引擎无法提供这个数。
4. **代价是「每次都全扫」** ——一个有十万 wiki 页的数据集，每翻一页都要扫全表。这是一个**用「后端一致性」换「线性扫描」的明确取舍**，且代码注释承认它是照搬 Python 行为。

**`WikiPageItem` 有一个字段被 `json:"-"` 掉了。**

```go
type WikiPageItem struct {
    Slug         string   `json:"slug"`
    Title        string   `json:"title"`
    PageType     string   `json:"page_type"`
    Topic        string   `json:"topic"`
    Summary      string   `json:"summary"`
    SourceDocIDs []string `json:"-"`
}
```

**`SourceDocIDs` 出现在结果里但不出现在 JSON** ——因为它是**给权限过滤用的**（见下 `ListWikiPagesWithSourceAccess`），不是给前端渲染的。它被保留在结构体里而不是让调用方自己再查一次，是一个「顺手带上」的字段。

**而 `ListWikiPagesWithSourceAccess` 的注释点明了这条权限语义的关键边界。**

```go
func (s *DatasetArtifactService) ListWikiPagesWithSourceAccess(ctx context.Context, tenantID, datasetID, pageType, topic, keywords string, page, pageSize int, canAccess func([][]string) ([]bool, error)) ([]WikiPageItem, int64, error) {
    if canAccess == nil {
        return s.ListWikiPages(ctx, tenantID, datasetID, pageType, topic, keywords, page, pageSize)
    }
    ...
```

注释：

> The access callback receives each engine batch's source-document lists and returns one visibility result per page. A page with no source documents is hidden when a callback is supplied.

**两条规则都很关键：**

1. **回调按「批次」被调用，而不是按页** ——因为 `[][]string` 一次传一批页的源文档列表，让回调可以批量查权限（一次数据库查询处理 1000 页）。
2. **「没有任何源文档的页，在给了回调时被隐藏」** ——这是一个**失败关闭（fail-closed）**的选择：一个不知道来源的产物，不给权限就无法判断该不该看，于是默认不给看。

**第 2 条是这一页的安全核心**：它意味着「合并行」若没有正确填 `source_doc_ids`，在启用权限过滤时会**整体消失**。而 `WikiPageItem.SourceDocIDs` 就是为此存在的。

**`GetWikiGraph` 的 `topN` 有两次夹取。**

```go
defaultWikiGraphTopN   = 512
maxWikiGraphTopN       = 1024
```

**默认 512、上限 1024。** 「默认值在中间而不是两端」这种设计意味着调用方不传时会得到一个中等规模的图——足够看出结构，又不会因为 hub 节点爆炸。而**上限 1024 是防「一次查太多」的保护**，与 `graphExpansionCap = 4096`（结构图展开上限）是两套不同的预算。

**`dataset_structure_graph.go` 的常量把两件事的「预算」区分开了：**

```go
const (
    graphKeywordCandidates = 16   // keyword candidate rows
    graphExpansionCap      = 4096 // hub-node expansion cap
)
```

- **`graphKeywordCandidates = 16`** ——关键词先只取 16 个候选实体，再围绕它们展开。
- **`graphExpansionCap = 4096`** ——从候选出发的邻域展开最多 4096 条边/实体。

**这是一个「两阶段预算」的典型形态**：先窄（找到真正的核心节点），再宽（把它的邻域拉进来）。注释用 hub-node 一词点明了约束的来源——**有些实体的度数极高（一个「公司」实体可能连几千条关系），若不设上限，一个关键词命中 hub 就会把整个图拉进来。**

**`StructureGraphNode` 是一个无类型的 map。**

```go
// StructureGraphNode is a projected entity in the structure graph response.
type StructureGraphNode map[string]interface{}

// StructureGraphRelation is a projected relation in the structure graph response.
type StructureGraphRelation map[string]interface{}
```

**两个都是 `map[string]interface{}` 的类型别名** ——于是「结构图节点有哪些字段」完全靠投影函数（`projectEntity` / `projectRelation`）定义，而消费端只能靠字符串键取。

**这与 `DocumentStructureGraphTemplate` 形成鲜明对比，后者是完整的结构体：**

```go
type DocumentStructureGraphTemplate struct {
    TemplateID   string                   `json:"template_id"`
    TemplateName string                   `json:"template_name"`
    Kind         string                   `json:"kind"`
    Entities     []StructureGraphNode     `json:"entities"`
    Relations    []StructureGraphRelation `json:"relations"`
}
```

**外层是结构体、内层是 map 混用**是这套产物代码的普遍形态：**「桶」有固定形状（模板 id/名/种类 + 两组节点），而「节点」的形状随产物种类而变**（wiki 实体有 `mention_count`、结构图节点可能没有）。用 map 让「不同种类的节点字段不同」不需要一个巨大的联合结构体，代价是**所有字段访问都要 `graphStr(node["name"])` 这样的防御式取法**。

**`projectEntity` 展示了这个投影的容错层次。**

```go
func projectEntity(row map[string]interface{}) StructureGraphNode {
    payload := graphLoadPayload(row)
    if payload == nil {
        return nil
    }
    name := ""
    for _, k := range []string{"name", "text", "term", "title"} {
        if v, ok := payload[k].(string); ok && strings.TrimSpace(v) != "" {
            name = strings.TrimSpace(v)
            break
        }
    }
    if name == "" || graphIsInvalidSentinel(name) {
        return nil
    }
    typ := "other"
    if v, ok := payload["type"].(string); ok && strings.TrimSpace(v) != "" {
        typ = strings.TrimSpace(v)
    }
```

**四处：**

1. **`graphLoadPayload` 把 `content_with_weight` 里的 JSON 解开** ——参数为 nil 表示解不开（空串或坏 JSON），于是这个节点被**丢弃**（`return nil`）。
2. **名字的四个候选键**（`name` / `text` / `term` / `title`）——不同类型的产物把名字放在不同的键里。
3. **`graphIsInvalidSentinel(name)` 过滤掉「名为 unknown/none/null/nan/n/a/undefined/other 的实体」。**

```go
func graphIsInvalidSentinel(v string) bool {
    if v == "" { return true }
    lv := strings.ToLower(v)
    for _, bad := range []string{"unknown", "none", "null", "nan", "n/a", "undefined", "other"} {
        if lv == bad { return true }
    }
    return false
}
```

**这是一份「LLM 会输出什么垃圾值」的实测清单。** 七个词全是模型在抽不到实体时的常见填充，而 `"other"` 是**唯一一个既是合法类型又是垃圾名字的词**——它出现在名字位就被丢掉，出现在类型位则是默认值。

4. **类型的默认值是 `"other"`**，与上面「other 是垃圾名字」形成**同一个词在名字位被拒、在类型位被接受**的不对称。

**`normalizeRelationEndpoints` 处理「关系端点指名字、而名字有别名」的问题。**

```go
func normalizeRelationEndpoints(entities []StructureGraphNode, relations []StructureGraphRelation) []StructureGraphRelation
```

**关系用 `from_entity_kwd`/`to_entity_kwd` 存**实体名**（不是 id），于是「端点能不能落到一个实体上」必须靠名字匹配。而实体有 `alias`，名字大小写也可能不同。这个函数的作用就是把这些变体归一到同一套（`endpointTerms` 把名字与别名都收进来，`normalizeEndpointKey` 归一大小写与空白）。

**这是「用名字当外键」的直接代价**：每次读图都要做一次名字归一，而且归一规则（大小写、空白、别名）必须与写入方一致。

**`skill_search.go` 走的是另一条路径：技能有自己的索引名。**

```go
func SkillIndexName(tenantID, spaceID string) string {
    spaceID = normalizeSpaceID(spaceID)
    spaceID = strings.ToLower(spaceID)
    replacer := strings.NewReplacer("-", "_", "/", "_", "\\", "_", " ", "_", ".", "_", ":", "_")
    sanitizedSpaceID := replacer.Replace(spaceID)

    // Generate unique, deterministic suffix from full IDs to avoid collisions
    // Use SHA-256 hash of the combined tenantID and sanitizedSpaceID
    hash := sha256.Sum256([]byte(tenantID + "_" + sanitizedSpaceID))
    hashStr := hex.EncodeToString(hash[:])[:16] // Take first 16 hex chars (64-bit entropy)

    // Use full IDs if they fit within reasonable length, otherwise use hash to ensure uniqueness
    const maxIDLen = 32 // Maximum length for each ID component
    uniqueTenant := tenantID
    if len(tenantID) > maxIDLen {
        uniqueTenant = tenantID[:maxIDLen] + "_" + hashStr[:8]
    }
    uniqueSpace := sanitizedSpaceID
    if len(sanitizedSpaceID) > maxIDLen {
        uniqueSpace = sanitizedSpaceID[:32] + "_" + hashStr[8:16]
    }

    return fmt.Sprintf("skill_%s_%s", uniqueTenant, uniqueSpace)
}
```

**这段的每一处都在处理一个具体的索引名约束：**

1. **六个字符被替换成 `_`**（`-`、`/`、`\`、空格、`.`、`:`）——索引名的合法字符集比租户 id 的字符集窄。
2. **`strings.ToLower`** ——避免「大小写不同的两个 spaceID 变成两个索引」。
3. **SHA-256 前 16 位十六进制（64 bit）作为**确定性**后缀** ——注释说明了目的：「Determine unique suffix to avoid collisions」。而「确定性」是关键：**同一个 spaceID 每次算出的索引名必须相同**，所以不能用随机数。
4. **超长时截断到 32 字符 + 挂上 hash 片段。** 注释说「Use full IDs if they fit within reasonable length, otherwise use hash to ensure uniqueness」——**截断会丢信息，所以必须补 hash 才能保持唯一性。**

**这里有一处值得指出的不对称**：`uniqueTenant` 用的是 `tenantID`（**未归一化**），而 `uniqueSpace` 用的是 `sanitizedSpaceID`（已小写 + 替换）。于是**同一租户的索引名里，租户部分可能含特殊字符**——若索引名对字符集有严格限制，租户 id 里的 `-` 就会成为一个隐患。注释没有说明为什么两侧处理不同。

**`Search` 的三分支分派是「先看配置，再看查询」。**

```go
hasEmbdConfig := config.EmbdID != ""

switch {
case config.VectorSimilarityWeight == 0 || !hasEmbdConfig || req.Query == "":
    // Pure keyword search (BM25)
    searchType = "keyword"
    threshold := config.SimilarityThreshold
    if req.Query == "" {
        threshold = 0 // Disable threshold for list all
    }
    results, err = s.keywordSearch(...)
case config.VectorSimilarityWeight == 1 && req.Query != "":
    // Pure vector search (skip if query is empty)
    searchType = "vector"
    results, err = s.vectorSearch(...)
    if err != nil {
        common.Warn("Vector search failed, falling back to keyword search", zap.Error(err))
        searchType = "keyword"
        results, err = s.keywordSearch(...)
    }
default:
    // Hybrid search (fallback to keyword if query is empty)
    ...
}
```

**四处：**

1. **第一支收集了三种「必须退回关键词」的情况**：向量权重为 0、没有嵌入模型、查询为空。注释逐条说明了理由。
2. **查询为空时把 `threshold` 置 0** ——「列出全部」时不能按相似度过滤，否则空查询什么都匹配不到。
3. **纯向量与混合两支都有「失败降级到关键词」** ——`vectorSearch` 的失败被显式降级（打 warn 后改调 `keywordSearch`）。
4. **而 `searchType` 字段会如实报告最终用了哪种** ——所以「请求了 hybrid 但实际是 keyword」在响应里可见。**这是一个很好的可观测性设计**：降级不是隐藏的，而是响应的一部分。

**`convertChunksToResults` 里的 `sortResults` 是一个 O(n²) 冒泡排序。**

```go
func sortResults(results []entity.SkillSearchResult) {
    // Simple bubble sort for now, could use sort.Slice
    for i := 0; i < len(results); i++ {
        for j := i + 1; j < len(results); j++ {
            if results[j].Score > results[i].Score {
                results[i], results[j] = results[j], results[i]
            }
        }
    }
}
```

**注释自己承认了：「could use sort.Slice」。** 这是这一页唯一一处「作者留了改进注记」的代码。

**它的实际影响**：`convertChunksToResults` 的输入上限是 `TopN: 100`，于是 `100² = 10000` 次比较——可以忽略。**但注释里的「for now」意味着它在一个更大的输入上会成为问题**，而这取决于 `TopN` 是否永远是 100。

而这里有一个需要指出的**语义问题**：`sortResults` 会把所有结果按 `Score` 降序重排，**而引擎返回的顺序已经是排序过的**。于是这个排序在正常情况下是幂等的空操作，只在「分数相同」时改变顺序——**而它用的是「交换而不稳定」的冒泡，于是等分项的相对顺序被反转破坏**。分页依赖稳定顺序时这会表现为「翻页出现重复或遗漏」。

**`getEmbedding` 是技能检索唯一的模型依赖点。**

```go
vector, err := s.getEmbedding(ctx, query, config.EmbdID, tenantID)
if err != nil {
    common.Warn("Vector search: failed to get embedding, will fallback to keyword search", ...)
    return nil, fmt.Errorf("failed to get embedding: %w", err)
}
```

**注释说「will fallback to keyword search」，而这个函数**自己**只返回错误，降级由调用方（`Search`）做。** 这是一处「注释描述的是上层行为」——从函数内部看它只是失败，从调用链看它是降级链的一环。**这类注释在纯函数上会误导读者**（以为函数内部有降级）。

**`GetDocumentGraph` 与 `GetDatasetStructure` 是两个入口，而它们共用 `keywordSubgraph` 的展开逻辑。**

`keywordSubgraph` 是「先取 16 个候选实体，再按边界扩展」的实现，而它的关键结构是**一个显式的前沿（frontier）迭代**：

```go
cond["knowledge_graph_kwd"] = []string{"relation"}
cond["to_entity_kwd"] = slices.Sorted(maps.Keys(ancestorFrontier))
relMap, _, err := graphRowSearch(ctx, tenantID, datasetID, graphRelationFields, cond, nil, 0, graphExpansionCap-len(relations), nil)
```

**四处：**

1. **`slices.Sorted(maps.Keys(frontier))`** ——把 map 的键排序后传给引擎。**排序在这里是必需的**：否则同一个图查询会产出不确定的过滤条件顺序（影响跨引擎行为一致性），也可能影响缓存。
2. **`graphExpansionCap-len(relations)`** ——**剩余预算**而不是总量。这是「全局上限」的正确实现（每轮取剩余额度，而不是每轮取全量）。
3. **`cond["to_entity_kwd"]` 只按**入边**扩展（`to`），即「哪些关系指向我的前沿」。** 只有这一侧被展开——是一个有意的方向性选择（向上游找父节点）。
4. **`rowHasEnabledSource` 过滤掉「没有有效来源」的行**：

```go
for _, row := range relMap {
    if !rowHasEnabledSource(row, map[string]bool{}) {
        continue
    }
```

**「没有有效来源的产物在结构图里不出现」** ——这与 `ListWikiPagesWithSourceAccess` 的「无源文档就隐藏」是同一条安全原则的两种落点（一处是权限回调，一处是数据完整性）。

**`dedupEntities` 与 `seenRel` 是两套去重键。**

```go
key := graphStr(edge["from"]) + "\x00" + graphStr(edge["to"]) + "\x00" + graphStr(edge["type"])
if seenRel[key] { continue }
```

**用 `\x00` 作为分隔符** ——因为名字里不会出现 NUL，于是「A\x00B」不会被「AB」或「A B」误撞。**这是一个小而正确的做法**（用 join 拼复合键时必须用一个不可能出现在分量里的分隔符）。

而关系键包含 `type`，与 `_build_degree_map` 里那种「按两端排序归一」的做法不同——**这里是有向的、带类型的**，因为结构图是有向图。

**`datasetStructureSupported` 是唯一一处「按引擎类型开关功能」。**

```go
func datasetStructureSupported() bool {
    switch engine.GetEngineType() {
    case "infinity", "elasticsearch", "opensearch", "":
        return true
    default:
        return false
    }
}
```

**四处：**

1. **四个值被允许**（含空串）——空串意味着「引擎还没初始化」，此时允许（因为错误会在真正用时出现）。
2. **其它引擎一律不支持**，`errDatasetStructureUnsupported` 会**指名是哪个引擎**：

```go
func errDatasetStructureUnsupported() error {
    return fmt.Errorf("dataset structure graph unsupported on doc engine %q", engine.GetEngineType())
}
```

**「把引擎名拼进错误消息」是这类功能开关的正确做法**——用户看到的是「在你的引擎上不支持」，而不是「功能坏了」。

3. **`resolveDatasetStructureKind` 做别名归一**（`graph`/`knowledge_graph` → `knowledge_graph`，`mindmap`/`mind_map` → `mind_map`），且注释点明与 Python 的 `_DATASET_STRUCTURE_KIND_ALIASES` 对齐。
4. **未知 kind 落到 `additionalDatasetStructureKinds[k]`**（一个包级表），而不是报错——于是「插件式注册新 kind」与「非法 kind 变成空串」共用同一分支。**这是一个「扩展点与错误处理合并」的写法**，好处是新 kind 不需要改 switch，坏处是拼错的 kind 静默变成空串。

## 关键取舍

**源码事实：** 知识编译产物（`wiki_page` / `wiki_entity` / `wiki_relation` / `wiki_alter` / `skill` / `skill_all` / `dataset_nav`）全部作为普通行写进租户文档索引，靠 `compile_kwd` / `type_kwd` 字段区分，没有独立表。

**源码事实：** `DatasetArtifactService` 只有一个字段 `docEngine`（生产环境恒 nil，仅供测试注入）；`searchCompiledWithMatch` 在接收者为 nil 时也能返回错误而不是 panic，注释说明这是「wiki-graph 错误路径测试所依赖的行为」。

**源码事实：** 读方法的第一步是「先拷调用方 filter，再无条件覆盖 `kb_id = datasetID`」，保证调用方无法越权扩大数据集范围；`dataset_artifact_service.go` 与 `dataset_structure_graph.go` 各写一遍同一逻辑。

**源码事实：** `HasWiki` 用 `type_kwd: wiki_page`、`HasSkill` 用 `compile_kwd: skill_all` —— 两个「有没有」判断用两个不同的字段名；都用 `total`（而非返回行数）判存在，且 `limit=1`、`selectFields=nil`。

**源码事实：** `ListWikiPages` 用 `available_int=1` 只取「数据集级合并行」，注释说明同一 `(page_type, slug)` 还有 `available_int=0` 的每文档原始行，不过滤会重复出现；`available_int` 原本是「chunk 是否参与检索」的标记，这里被复用为「合并行 vs 原始行」。

**源码事实：** `keywords` 搜索不用全文检索，而是「按 1000 行批量扫描 + 内存 `strings.Contains`」，注释给出的理由是「这些字段每行都有、跨后端行为一致，而全文分词字段不是」，并注明这是照搬 Python 行为；代价是每次翻页全表扫描，`total` 由内存匹配数重算。

**源码事实：** `WikiPageItem.SourceDocIDs` 是 `json:"-"`（给权限过滤用，不返回前端）；`ListWikiPagesWithSourceAccess` 按批次调用回调（`canAccess([][]string) ([]bool, error)`），且「无源文档的页在给了回调时被隐藏」（fail-closed）。

**源码事实：** `StructureGraphNode` / `StructureGraphRelation` 是 `map[string]interface{}` 别名，节点字段由 `projectEntity` / `projectRelation` 投影定义；外层 `DocumentStructureGraphTemplate` 是结构体（模板 id/名/kind + entities/relations）。

**源码事实：** `graphIsInvalidSentinel` 过滤七种 LLM 填充词（unknown / none / null / nan / n/a / undefined / other）；`other` 在名字位被丢弃、在类型位作为默认值——同一个词两种处理。

**源码事实：** 关系端点用实体**名**而非 id（`from_entity_kwd` / `to_entity_kwd`），因此读图时必须 `normalizeRelationEndpoints` 做名字/别名/大小写归一。

**源码事实：** 图查询有两级预算：`graphKeywordCandidates = 16`（候选实体）与 `graphExpansionCap = 4096`（邻域展开上限，注释点明「hub-node expansion cap」），且每轮取 `graphExpansionCap-len(relations)` 即**剩余额度**；前沿扩展只沿入边（`to_entity_kwd`）；`slices.Sorted(maps.Keys(...))` 保证过滤值顺序确定；关系去重键用 `\x00` 分隔的三元组（含 `type`，有向）。

**源码事实：** `rowHasEnabledSource` 会丢弃「没有有效来源」的行；与 `ListWikiPagesWithSourceAccess` 的「无源文档即隐藏」是同一条原则的两种落点。

**源码事实：** `wikiGraph` 的 `topN` 默认 512、上限 1024；`SkillIndexName` 用 `sha256(tenantID + "_" + sanitizedSpaceID)` 前 16 个十六进制作为**确定性**后缀，超 32 字符时截断并挂 hash 片段保证唯一；但租户部分用未归一的 `tenantID`、空间部分用小写并替换六种字符的 `sanitizedSpaceID`，两侧处理不对称。

**源码事实：** `SkillSearchService.Search` 的三分支是「向量权重为 0 / 无嵌入模型 / 空查询 → 关键词；权重为 1 且非空 → 向量（失败降级关键词）；其余 → 混合（失败降级关键词）」；空查询把 `threshold` 置 0；`SearchResponse.SearchType` 如实报告最终使用的策略，使降级可见。

**源码事实：** `convertChunksToResults` 用 O(n²) 冒泡 `sortResults` 重排分数（注释自承认「could use sort.Slice」），输入上限 `TopN: 100`；该排序非稳定，会打乱等分项的相对顺序。

**源码事实：** 索引不存在时 `Search` 返回空结果 + `CodeSuccess`（打 warn），而不是报错；配置缺失时用一份硬编码默认配置（`VectorSimilarityWeight: 0.3`、`SimilarityThreshold: 0.2`、四字段权重、`TopK: 10`）。

**源码事实：** `datasetStructureSupported` 只允许 infinity / elasticsearch / opensearch / 空串四种引擎，其它引擎返回指名引擎的错误；`resolveDatasetStructureKind` 对未知 kind 回退到 `additionalDatasetStructureKinds[k]`（拼错即静默为空串）。

**架构推论：** 「产物与原文共用一个索引」是一个**用「检索统一性」换「schema 清晰度」**的取舍。它让「一次查询同时命中原文与产物」变得免费，但让**每一条产物记录都必须伪装成 chunk**：内容塞进 `content_with_weight`、种类塞进 `compile_kwd`、可见性塞进 `available_int`。这类设计的核心风险不是性能，而是**字段语义被复用后失去唯一含义**——`available_int` 同时是「可检索」与「合并行」就是实例。**要长期维持这类设计，必须有一份「每个共享字段在各产物种类下的含义」的表，而这份表在没有独立 schema 时无处存放。**

**架构推论：** 「名字当外键」在产物层是必然选择（产物是可读的、跨库的、面向人的），而它的代价全部落在读端：每次读图都要归一名字与别名。这与关系库里「id 当外键」形成对照：**关系库把一致性成本放在写端（写时校验外键存在），产物层把成本放在读端（读时归一）。** 后者的好处是写入永不失败（LLM 抽出什么就存什么），坏处是**坏数据在库里长期存在，只在读取时被丢弃**——这就是 `graphIsInvalidSentinel` 这份「垃圾值清单」存在的原因。

**架构推论：** 「无来源的产物在启用权限时被隐藏」是一条正确的 fail-closed 规则，而它对写入方提出了一个硬要求：**`source_doc_ids` 必须正确填写**。这条要求没有在读取端强制执行（只是不填就看不到），于是它会以「某些页面在权限模式下消失」这种难以定位的形式暴露。任何 fail-closed 的设计都应当配套**写入端的完整性校验**，否则默认路径能看到、权限路径看不到的差异会被误判为权限 bug。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/service/dataset_artifact_service.go` | `DatasetArtifactService` | 产物读取服务（唯一字段 `docEngine` 供测试注入） |
| `internal/service/dataset_artifact_service.go` | `HasWiki` / `HasSkill` | 存在性判断（`limit=1` + `total`，两个种类用两个不同字段名） |
| `internal/service/dataset_artifact_service.go` | `WikiPageItem` | wiki 页摘要（`SourceDocIDs` 不返回前端，供权限过滤） |
| `internal/service/dataset_artifact_service.go` | `ListWikiPages` | 列表：`available_int=1` 只取合并行；`keywords` 走批量扫描 + 内存匹配 |
| `internal/service/dataset_artifact_service.go` | `ListWikiPagesWithSourceAccess` | 按批次回调做权限过滤；无源文档则隐藏 |
| `internal/service/dataset_artifact_service.go` | `searchCompiled` / `searchCompiledWithMatch` | 统一查询入口（拷 filter 再钉 `kb_id`、nil 接收者安全） |
| `internal/service/dataset_artifact_service.go` | `GetWikiGraph` / `GetWikiAlteration` | wiki 图与变更日志读取（`topN` 默认 512 / 上限 1024） |
| `internal/service/dataset_artifact_service.go` | `CompileKwdWikiPage` 等常量组 | 产物种类的关键字（产物与原文共用索引的区分依据） |
| `internal/service/skill_search.go` | `SkillSearchService` / `Search` | 技能检索（三分支策略 + 逐支降级，`SearchType` 如实上报） |
| `internal/service/skill_search.go` | `SearchRequest` / `SearchResponse` | 请求（`TenantID` 来自上下文，非请求体）与响应（`search_type` 说明实际策略） |
| `internal/service/skill_search.go` | `SkillIndexName` / `normalizeSpaceID` | 独立索引命名（确定性 sha256 后缀 + 超长截断补 hash） |
| `internal/service/skill_search.go` | `keywordSearch` / `vectorSearch` / `hybridSearch` | 三种检索实现（字段权重 `name^10 / tags^5 / description^3 / content^1`） |
| `internal/service/skill_search.go` | `convertChunksToResults` / `sortResults` | chunk → 结果投影；O(n²) 冒泡排序（注释自承认可换 `sort.Slice`） |
| `internal/service/skill_search.go` | `getEmbedding` | 查询向量化（失败由调用方降级到关键词） |
| `internal/service/dataset_structure_graph.go` | `StructureGraphNode` / `StructureGraphRelation` | 图节点/边的投影类型（`map[string]interface{}` 别名） |
| `internal/service/dataset_structure_graph.go` | `DocumentStructureGraphTemplate` | 按模板分桶（模板元数据是结构体，节点是 map） |
| `internal/service/dataset_structure_graph.go` | `graphRowSearch` / `graphLoadPayload` | 原行查询与 `content_with_weight` 解析 |
| `internal/service/dataset_structure_graph.go` | `projectEntity` / `projectRelation` | 投影（名字四候选键、类型默认 `other`、无效哨兵丢弃） |
| `internal/service/dataset_structure_graph.go` | `graphIsInvalidSentinel` | 七种 LLM 垃圾值名单 |
| `internal/service/dataset_structure_graph.go` | `normalizeRelationEndpoints` / `endpointTerms` | 名字当外键后的端点归一 |
| `internal/service/dataset_structure_graph.go` | `keywordSubgraph` | 两阶段预算的邻域展开（16 候选 → 4096 上限，沿入边、剩余额度） |
| `internal/service/dataset_structure_graph.go` | `datasetStructureSupported` / `resolveDatasetStructureKind` | 引擎能力开关（指名错误）与 kind 别名归一 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：internal/service/dataset_artifact_service.go](https://github.com/infiniflow/ragflow/blob/main/internal/service/dataset_artifact_service.go)
- [ragflow 源码：internal/service/skill_search.go](https://github.com/infiniflow/ragflow/blob/main/internal/service/skill_search.go)
- [Elasticsearch 文档：index 命名与限制](https://www.elastic.co/guide/en/elasticsearch/reference/current/indices-create-index.html)

## 与主核 / AI 的关系

产物层与主核（多模态数据库内核）的交集在**「派生数据该不该有自己的 schema」**。四条经验。

**第一，让派生数据与原始数据共用存储，必须为每个被复用的字段写下「它在每种数据下的含义」。** 这里的 `available_int` 原本是「chunk 是否参与检索」，被复用成「合并行还是原始行」；于是任何新读法都必须同时知道两套语义。主核里做「索引项复用页内空间」「系统表行复用普通行格式」时同理：**一个字段承载两个正交语义，短期省一次迁移，长期让每处读写都要附带解释。** 若确实要共用，正确的做法是**把语义写进一个集中的常量/注释块并让读端引用它**，而不是散落在各处的行内注释。

**第二，fail-closed 的设计必须配套写入端的完整性校验。** 「无来源即隐藏」是正确的默认，但它把「写入方漏填 `source_doc_ids`」的后果推迟到「权限模式下的神秘缺失」。主核里凡是「缺失即拒绝」的路径（缺少校验和的页、缺少可见性映射的行）都应当同时提供**写入期的断言或后台校验任务**，否则这个 fail-closed 会以「时好时坏」的形式被误判为并发问题。

**第三，名字当外键的成本必须落在读端一次性归一，而不是散在很多判断里。** 这里 `normalizeRelationEndpoints` 是一个集中的归一函数，`graphIsInvalidSentinel` 是一个集中的垃圾值清单——这两处集中化是对的。主核里若把「外部标识」当键（例如把对象路径当主键），同样应当把「归一规则」与「非法值清单」各收敛成一个函数，**否则每个消费点都会各写一套近似规则，而它们迟早不一致。**

**第四，功能按后端能力开关时，错误消息必须指名后端。** `errDatasetStructureUnsupported` 把引擎类型拼进消息，于是「不支持」是一个可诊断的事实而不是「坏了」。主核里做「某能力仅在特定后端可用」（例如向量索引只在支持 ANN 的引擎上、物化视图只在支持列存的引擎上）时同理：**能力开关要能被观测到，且错误要说明「在哪不可用」**，否则运维会把它当成故障排查。
