---
project: ragflow
feature: structure-and-search-tools
title: 结构导航与 agentic 检索工具
page_type: feature
schema_version: 1
refs:
  - path: internal/agentic_rag/tool_navigate_structure.go
    symbols: [NavigateStructureTool]
  - path: internal/agent/tool/agentic_search.go
    symbols: [AgenticSearchTool, NewAgenticSearchTool]
---

# 结构导航与 agentic 检索工具

## 一句话职责

把「先定位到文档，再钻进文档的编译结构」压成一个可选两步也可一步走的工具，并提供 hybrid / vector / BM25 三种块检索模式——前者让模型沿结构树找到最相关的实体与关系，再把 chunk 指针交给 `list_chunks` 深读；后者在块召回后可把命中句及邻句剪成更短的证据片段。它们共同服务一个目标：**先缩小范围，再把更少但更相关的上下文交给模型**。

## 机制是什么

`NavigateStructureTool` 的包注释先给了它的完整使用契约：

> 钻进文档的编译结构（编译时构建的实体/关系大纲），返回一个 query 聚焦的 `<structure_navigation>` 大纲与 chunk 指针，模型可以把这些指针交给 `list_chunks` 做深读。
>
> 典型用法：先调 `navigate_tree` 找到文档，再取它的 `doc_id` 调 `navigate_structure` 看文档内部。但也可以**只给 query**：它会自己在数据集的编译导航树上路由问题，所以无需先调 `navigate_tree`。

所以这个工具既支持明确的两步链：

```
navigate_tree → doc_id → navigate_structure → chunk_ids → list_chunks
```

也支持**一跳捷径**：

```
navigate_structure(query) → 内部路由候选文档 → 结构大纲
```

一跳捷径的代价在 `navQueryRouteDocs = 3` 的注释里被明说：

> 两步契约（`navigate_tree` → `navigate_structure`）是一个真实的**采用门槛**：query-only 调用移除了这个门槛，但代价是钻开几篇被路由到的文档，而不是模型自己选的一篇。

所以这不是「两种等价写法」，而是一个真实的 UX 取舍：**两步更精确，一步更容易上手但最多钻 3 篇**。

而 `navigate_structure` 返回的是**结构提纲**而非正文：

> 返回一个 XML `<structure_navigation>`，每个可读的结构一条 `<doc>`，每个 `<structure>` 里是一组 `- name (type): description [chunks: c1,c2]` 行。把 chunk 指针传给 `list_chunks`（`anchor_chunk_ids`）。

所以它的结构是**目录 + 指针**，不是内容本身。理由是成本：结构小、便宜；正文大、昂贵。模型先读小纲要再决定哪些块值得深读。

另一个工具 `AgenticSearchTool` 则直接在块上查，但暴露三种**不同的检索模式**：

| 工具名 | keyword 权重 | vector 权重 | 含义 |
|---|---:|---:|---|
| `hybrid_search` | 0.7 | 0.3 | 关键词 + 向量混合 |
| `vector_search` | 0.0 | 1.0 | 纯向量 |
| `bm25_search` | 1.0 | 0.0 | 纯关键词 |

注释说明「底层 `RetrievalRequest.KeywordsSimilarityWeight` 是 keyword 的份额，vector 份额是它的补数」。所以 `0.7` 的含义不是「70% keyword + 70% vector」，而是 `keyword=0.7, vector=1-0.7=0.3`。

这三者都挂在同一个检索服务上，区别只是**融合权重**——而它们的共同下游是 `GetRetrievalService()`。

## 怎么实现的

**`NavigateStructureTool.Info` 的 JSON Schema 是模型与工具之间的契约，而它的 `query` 是唯一必填项。**

```json
{
  "properties": {
    "query": {"type":"string", "description":"REQUIRED: ..."},
    "doc_id": {"type":"string", "description":"OPTIONAL: ..."},
    "doc_ids": {"type":"array", "maxItems":10},
    "kind": {"type":"string", "description":"catalog / mindmap / graph ..."},
    "keywords": {"type":"string"},
    "dataset_ids": {"type":"array", "maxItems":10}
  },
  "required": ["query"]
}
```

值得注意：`doc_id` 与 `doc_ids` 同时存在，而 `doc_ids` 的优先级更高（`len(args.DocIDs)==0 && args.DocID!=""` 才把单个 id 包成切片）。这样旧调用可以传单个 id，新调用可以批量传列表。

而 `doc_ids` / `dataset_ids` 都写了 `maxItems: 10`，但**实际执行时再次截断**：

```go
if len(docIDs) > 10 {
    docIDs = docIDs[:10]
}
```

这是「schema 约束 + 运行期再约束」的双层做法。schema 是给模型的提示（模型可能不遵守），运行期截断才是真正的保证。

**查询-only 路径内部调用 `routeDocIDsForQuery`，跨数据集去重并限制最多 3 篇。**

```go
func routeDocIDsForQuery(ctx context.Context, query string, datasetIDs, docScope []string, limit int) []string {
    if limit <= 0 { limit = navQueryRouteDocs }
    seen := map[string]bool{}
    ...
    tenants := DatasetTenants(ctx, datasetIDs)
    for _, kbID := range datasetIDs {
        routed, err := navStructureRoute(ctx, tenants[kbID], kbID, query, docScope, limit)
        ...
        for _, pair := range routed {
            did := strings.TrimSpace(pair[0])
            if did == "" || seen[did] { continue }
            seen[did] = true
            out = append(out, did)
            if len(out) >= limit { return out }
        }
    }
    return out
}
```

三个边界：

1. **每个数据集用自己的 tenant index** ——共享数据集可能在分享者的租户索引里，不是会话租户。
2. **docScope 推进路由器** ——所以候选排名预算花在当前会话允许的文档之内，而不是先全局排名再事后过滤。
3. **跨数据集按 doc_id 去重** ——同一文档只钻一次。

而路由错误是**debug 并跳过这个数据集**，不是整个工具失败：一个数据集的导航索引暂时坏了，其他数据集仍可返回候选。

**`drillKeptNodes` 的「相关性 beam」是一个有界树搜索。**

```go
const (
    structMaxDepth     = 3
    structMaxNodes     = 10
    structBranchK      = 2
    structRelevanceMin = 1
)
```

每层的算法：

1. 对当前 frontier 上的节点用 `nodeRelevance` 打分（query terms 与 name / description 的匹配数）。
2. 按分数降序稳定排序。
3. 只留 `score >= 1` 的节点，最多取前 2（`structBranchK`）。
4. 下一层只展开这 2 个节点的 children。
5. 深度最多到 3 层。

这是一种**beam search**：不是遍历整棵树，也不是只取单一路径，而是每层保留最相关的前 K 个分支。`sort.SliceStable` 保证同分时维持原有顺序，于是相同的结构树与 query 得到可复现的 outline。

而 `structMaxNodes = 10` 与 `structMaxDepth = 3` 是**两个独立上限**：深度限制纵向展开，节点数限制总的输出规模。最后 `collectChunkIDs(..., capN=4)` 还会限制 chunk 指针数，所以模型拿到的不是整树，而是「最多 10 个结构节点 + 最多 4 个 chunk 指针」。

**结构树的实体关系从编译产物里恢复，而不是重新查询实体表。**

`engineStructureReader.ReadStructure` 读取 `knowledge_graph_kwd` 的编译结构行，`parseCompiledStructure` 再把 blob 转成 `structureNode` / `structureRel`。这条路径特别处理两种存储形态：

- 一个压缩的 `graph` blob（实体与关系一起）；
- 单独的 `entity` / `relation` 行。

注释说默认 `catalogKinds` 同时接受 tree / timeline / page_index **以及** graph / entity / relation / knowledge_graph，是为了「默认『展示这篇文档的结构』会读取所有编译形状行，而不是静默丢掉图 blob」。

所以这不是一个「只读目录索引」工具，而是兼容了**多种编译时代际格式**的结构读取器。`kind` 参数可以把范围缩窄到 `catalog`、`mindmap` 或 `graph`，但省略时不丢任何已知形状。

**`AgenticSearchTool` 的构造只决定 mode 与 weight，执行时补齐可信 scope。**

```go
func NewAgenticSearchTool(mode string) *AgenticSearchTool {
    weight := 0.7
    switch mode {
    case toolVectorSearch: weight = 0.0
    case toolBM25Search:   weight = 1.0
    }
    return &AgenticSearchTool{mode: mode, weight: weight, defaults: hybridSearchArgs{TopN: 12}}
}
```

`TopN` 默认 12。`InvokableRun` 收到模型参数后：

```go
if args.TopN <= 0 {
    args.TopN = a.defaults.TopN
}

svc := GetRetrievalService()
tenantID := canvasTenantID(ctx)
datasetIDs := canvasDatasetIDs(ctx, args.KbIDs)
```

第二段注释是安全边界：

> 通过 `canvasDatasetIDs` 路由，让一个可信的 `WithScope` 注入（若存在）与模型提供的 `kb_ids` **取交集**，防止越权到会话 KB 之外。在没有注入的 Canvas runtime 里，显式 `kb_ids` 仍然是权威值，保留原有行为。

所以数据集范围的信任顺序是：**服务端注入范围 > 模型请求范围**。注入后模型只能缩小范围，不能扩大范围。这与 f04 对 `component_name` 的信任高于 `function_name` 是同一个原则：受控来源压过自由输入。

若 `svc == nil` / `tenantID == ""` / `datasetIDs` 为空，工具返回 `jsonChunksEmpty()` 与 nil error——也就是**成功的空结果**。这是一个重要的区别：缺少检索服务不是 `ErrRetrievalServiceMissing` 那种错误（f04 的普通 RetrievalTool 会报），而对 AgenticSearchTool 来说「无作用域」被当作「没有块」。

构造请求时还有四个硬编码：

```go
RetrievalRequest{
    Query:                    strings.TrimSpace(args.Query + " " + args.Keywords),
    TopN:                     args.TopN,
    TopK:                     args.TopN * 4,
    SimilarityThreshold:      &similarityThreshold, // 0.2
    AllowDenseFallback:       new(false),
    KeywordsSimilarityWeight: &weight,
    DocScope:                 args.DocScope,
    TenantID:                 tenantID,
    ExcludeCompiled:          true,
}
```

- **`TopK = TopN × 4`** ——先多取 4 倍候选，让底层融合与过滤后仍有 `TopN` 个结果。
- **`SimilarityThreshold = 0.2`** ——低于它的向量匹配不返回。
- **`AllowDenseFallback = false`** ——不会在 keyword 查询为空时退回纯稠密检索；g01 / g04 解释了 agentic 模式下为什么要这个约束。
- **`ExcludeCompiled = true`** ——只查原始 chunk，排除编译产物。`navigate_structure` 专门查编译结构，这条工具专门查原文块，两者互补。

**关键词 narrowing 是检索之后的第二次过滤，而不是一开始就改查询。**

```go
chunks, err := svc.Search(ctx, dao.DB, req)
...
if args.Keywords != "" {
    chunks = narrowByKeywords(chunks, args.Keywords)
}
```

`narrowByKeywords` 把 chunk 切成句子，找出含任一关键词的句子并保留**前后各一个邻句**，然后按内容 MD5 去重。

为什么不是把关键词直接并入检索 query？因为那会改变召回——而这个 narrowing 的目标是**不改召回，只把返回给模型的内容变短**。所以先照常检索，再把结果内容裁成更聚焦的窗口。

`splitKeywords` 有一个值得注意的启发式：若逗号分隔的关键词少于 3 个，就退化成空格切词后组成相邻 bigram（如 `"entity extraction"` → `"entity extraction"`）。这样短关键词列表里的词不会被当成彼此无关的独立词。

## 关键取舍

**源码事实：** `NavigateStructureTool` 支持显式两步（`navigate_tree → doc_id → navigate_structure`）与 query-only 一步路由；query-only 的代价是最多钻开 3 篇被路由到的文档，而非模型选定的一篇（`navQueryRouteDocs = 3`）。

**源码事实：** 工具返回的是 XML 结构大纲 + chunk 指针，明确要求模型再调 `list_chunks(anchor_chunk_ids)` 做深读——目录便宜、正文昂贵，先缩小再深读。

**源码事实：** JSON Schema 只把 `query` 列为必填；`doc_id` 与 `doc_ids` 并存，后者优先；schema 的 `maxItems: 10` 在运行期再次截断为 10，schema 是提示，运行期是保证。

**源码事实：** `routeDocIDsForQuery` 每个数据集使用自己的 tenant index（共享数据可能在分享者租户），docScope 推进路由预算，跨数据集按 doc_id 去重；单个数据集路由失败只记 debug 并跳过。

**源码事实：** `drillKeptNodes` 是 beam search：每层 score≥1，最多留 top-2，最多 3 层；`sort.SliceStable` 保证同分时结果可复现；结构节点最多 10 个，chunk 指针最多 4 个。

**源码事实：** 默认 catalog 读取 tree/timeline/page_index 与 graph/entity/relation/knowledge_graph 两种编译形状，避免「默认展示文档结构」时静默丢图 blob；兼容压缩 graph blob 与逐行 entity/relation 两种存储形态。

**源码事实：** `AgenticSearchTool` 的融合权重是 hybrid `0.7`（keyword）/vector `0.0`/BM25 `1.0`；默认 `TopN=12`，请求 `TopK=TopN×4`，相似度阈值 0.2，`AllowDenseFallback=false`，`ExcludeCompiled=true`。

**源码事实：** `canvasDatasetIDs(ctx, args.KbIDs)` 的范围规则是「服务端注入范围与模型给出的 kb_ids 取交集」——模型可缩小、不可扩大；没有注入时才以显式 kb_ids 为准。

**源码事实：** 缺 service / tenant / dataset scope 时 `AgenticSearchTool` 返回 JSON 空结果与 nil error——是「成功的空结果」；与 f04 普通 `RetrievalTool` 缺 service 抛 `ErrRetrievalServiceMissing` 的行为不同。

**源码事实：** 关键词 narrowing 在检索之后执行，切句后保留匹配句及前后邻句、按 MD5 去重——「不改变召回，只缩短交给模型的正文」。

**源码事实：** `splitKeywords` 在逗号词少于 3 个时改用空格词相邻 bigram，以避免少量短词被当作彼此无关的独立查询词。

**架构推论：** `navigate_structure` 的「结构目录 + chunk 指针」与 `AgenticSearchTool` 的「直接块召回」是两种互补的上下文经济策略：前者先付很小的结构成本再决定读什么，后者一次取块但可按关键词缩短正文。判据是：**问题是否需要文档内部关系/层级？** 需要就先导航，否则直接搜索块。

**架构推论：** `NavigateStructureTool` 的 schema 限制与运行时截断同时存在，说明「模型参数声明不是安全边界」。模型可能不遵守 `maxItems: 10`，所以执行处仍截断；任何让模型提供 `doc_ids`、`dataset_ids` 等数组范围的工具都应在运行时再次 enforce。

**架构推论：** `AgenticSearchTool` 的范围交集体现「模型可以缩小服务端授权范围，但不能扩大」这一通用原则。任何模型可见的 resource ID 都应该与服务端绑定的 scope 求交，而不是相信模型参数。

**架构推论：** 检索后再 narrowing 与检索前改 query 的差别是「召回」与「呈现」两个职责。改 query 会改变找到哪些块；裁内容只改变模型读到块的多少。要减少 token 成本且不损召回，应在拿到候选后裁剪，而不是把候选集合本身缩小。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/agentic_rag/tool_navigate_structure.go` | `NavigateStructureTool` | 持有租户 / 数据集范围，输出结构 XML |
| `internal/agentic_rag/tool_navigate_structure.go` | `NewNavigateStructureTool` | 构造时注入会话绑定 scope |
| `internal/agentic_rag/tool_navigate_structure.go` | `Info` | JSON Schema；query 唯一必填，数组 maxItems=10 |
| `internal/agentic_rag/tool_navigate_structure.go` | `InvokableRun` | 包装 `guardedToolRun`，把错误转成模型可读结果 |
| `internal/agentic_rag/tool_navigate_structure.go` | `routeDocIDsForQuery` | query-only 路由，最多 3 文档，按 doc id 去重 |
| `internal/agentic_rag/tool_navigate_structure.go` | `drillKeptNodes` | 3 层、每层 top-2 的结构 beam search |
| `internal/agentic_rag/tool_navigate_structure.go` | `parseCompiledStructure` | 兼容 graph blob 与 entity/relation 行 |
| `internal/agentic_rag/tool_navigate_structure.go` | `collectChunkIDs` | 每个结构最多 4 个 chunk 指针 |
| `internal/agent/tool/agentic_search.go` | `AgenticSearchTool` | 三种融合模式，共用同一检索服务 |
| `internal/agent/tool/agentic_search.go` | `NewAgenticSearchTool` | mode → keyword 权重，TopN 默认 12 |
| `internal/agent/tool/agentic_search.go` | `InvokableRun` | 服务端 scope 取交集、构造检索请求、结果后 narrowing |
| `internal/agent/tool/agentic_search.go` | `narrowByKeywords` | 匹配句 + 前后邻句，MD5 去重 |
| `internal/agent/tool/agentic_search.go` | `splitKeywords` | 短逗号列表转相邻 bigram |
| `internal/agent/tool/agentic_search.go` | `marshalSearchResult` | chunk payload 转 JSON 工具结果 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：agentic_rag/tool_navigate_structure.go](https://github.com/infiniflow/ragflow/blob/main/internal/agentic_rag/tool_navigate_structure.go)
- [ragflow 源码：agent/tool/agentic_search.go](https://github.com/infiniflow/ragflow/blob/main/internal/agent/tool/agentic_search.go)
- [eino 文档：工具组件](https://www.cloudwego.io/docs/eino/core_modules/components/tool/)

## 与主核 / AI 的关系

主核里最接近的是**索引树导航与覆盖索引**：数据库的 B-tree 内部节点只给「往哪走」的键，不把整行数据塞在导航页里；叶子上才有行指针或数据。`navigate_structure` 返回结构 outline 与 chunk 指针，`list_chunks` 再按指针取正文——就是同样的两段式：**导航信息先给、正文按需取**。

**差别在于「导航树是什么」。** B-tree 是按键排序的树，所以路径是有序且可证明完备的；这里的结构树是语义关系（实体 / 关系 / 编译结构），beam search 是启发式，只保留每层 top-2 且最多 3 层。它牺牲完整性换上下文成本——因为一个把整棵结构树都塞进上下文的工具会比直接读块还贵。

与 g02（KG Service）的关系：`kg_service.DocOutline` 是另一条「一次获取文档整体结构」路径，而 `NavigateStructureTool` 把目录 / 图形状还原为 query-focused 的 outline，并附加 chunk pointers。前者是数据读取服务，后者是交给模型的裁剪呈现。

与 g03（grep）的关系是「结构定位 vs 字面定位」：结构工具能靠实体/关系路径定位到 chunk 指针，即使查询词和原文不完全相同；grep 只对原文做精确串匹配，适合用户给出确切短语或标识符时交叉验证。

与 g01（agentic 主循环）的关系：两种工具的输出都会被 `instrumentedTool` 记进 served/deep-read 台账。结构工具给大纲与 chunk pointers，不等于正文被打开；真正的 deep read 仍是 `list_chunks`。这给 agentic 评估提供了「发现候选」与「读入证据」的边界。

对 AI / agent 工程最实用的一条是 **「导航结果带指针，不带整段内容」**。这让模型可以先做一个低成本的结构选择，然后只深读它选中的段落。与数据库覆盖索引一样，导航结构携带的是「定位所需信息」，正文仍然按需读取。

另一条是「一跳工具也要有明确上限」。query-only 调用最多路由 3 篇、结构 beam 每层最多 2 个且最多 3 层、每个 outline 最多 10 个节点 / 4 个 chunk 指针。每个界限都避免一个看似便利的工具变成「一次把全库结构倾倒给模型」。

最后一条是对服务端 scope 做交集，而不是依赖模型守约。`dataset_ids` / `doc_ids` 都是模型可控参数，但只能在会话已绑定的范围内缩小查询。**模型负责选择对象，服务端负责限定它可以选择的集合。**
