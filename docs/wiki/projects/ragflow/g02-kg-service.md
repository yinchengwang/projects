---
project: ragflow
feature: kg-service
title: 知识图谱服务与探索
page_type: feature
schema_version: 1
refs:
  - path: internal/agentic_rag/kg_service.go
    symbols: [KgService, GetKgService, KgExploreRequest, KgEntity]
  - path: internal/agentic_rag/kg_service.go
    symbols: [KgAdapter]
---

# 知识图谱服务与探索

## 一句话职责

给 agentic 检索一个**多跳图行走**的工具：用查询匹配几个种子实体，沿关系边跳两跳，把访问到的子图与它背后的原文段落一起交回给模型；而这条行走**从不调用聊天模型**——在这个 ReAct 循环里模型自己就是证据的裁判，一个工具持有聊天模型会越权，而「只返回答案」会剥掉答案必须引用的段落。

## 机制是什么

模块头注释把三条边界一次性声明了：

> 图谱行走从种子实体行开始，沿关系行跳出去，把子图加上它背后的原文段落交回给模型。
>
> 这条行走**从不调用聊天模型**。在这个 ReAct 循环里**模型已经自己证据的裁判**，一个工具不持有聊天模型，而一个只有答案的结果会剥掉答案必须引用的段落。因此这条行走**总是返回子图与它的段落**。

三条推理：

1. **模型自己就是裁判** ——所以工具不该做「总结」这个动作。
2. **工具不持有聊天模型** ——这是一个结构性约束（工具层没有模型引用），不是风格选择。
3. **只有答案会剥掉段落** ——所以「返回答案」这个看似更友好的做法反而是错的：模型的答案必须能引用原文，而一段总结把可引用的粒度毁掉了。

这三条合起来定义了这个工具的形态：**它返回原料，不返回结论**。

而「两跳」这个深度是常量决定的，`kgHops = 2`。而它的配套常量说明每一步都在做取舍：

```go
kgSeeds       = 2   // top-N entities matched directly to the question
kgSeedPool    = 64  // KNN candidate pool before the mention_count re-sort
kgSeedSim     = 0.8 // dense seed similarity floor
kgHops        = 2   // relation hops out from the seeds
kgNeighbors   = 128 // cap on neighbour entity rows resolved per hop
kgRelLimit    = 32  // relations fetched per endpoint filter
kgMaxPassages = 6   // source passages returned per walk
```

七个常量构成一条「预算链」：2 个种子 → 每跳 32 条关系 → 每跳最多 128 个邻居 → 6 段原文。而 `kgSeedPool = 64` 配 `kgSeeds = 2` 说明种子选择是「先取 64 个候选、再按 `mention_count` 重排取前 2」——**先宽后窄**。

## 怎么实现的

**服务是一个「可安装的接口 + 一个 stub 默认值 + 一个实现」的三件套。**

```go
type KgService interface {
    Explore(ctx context.Context, req KgExploreRequest) (KgExploreResult, error)
    DocOutline(ctx context.Context, req KgDocOutlineRequest) (KgExploreResult, error)
}

var (
    kgServiceMu   sync.RWMutex
    kgServiceImpl KgService = stubKgService{}
)
```

而 `stubKgService` 的两个方法都返回 `ErrKgServiceMissing`：

```go
var ErrKgServiceMissing = errors.New("kg service not registered — call agentic_rag.SetKgService(...) at boot")
```

所以「没安装服务」不是 nil panic 或空结果，而是**一个带明确指引的错误**——而错误消息里直接写了怎么修（`call agentic_rag.SetKgService(...)` at boot）。

而 `SetKgService(nil)` 会**恢复 stub**而不是把字段设成 nil：

```go
if svc == nil {
    kgServiceImpl = stubKgService{}
    return
}
```

这是「永远不要让接口变量是 nil」的防御——因为调用方（工具层）是直接调 `GetKgService().Explore(...)`，若拿到 nil 会 panic 而不是得到一个可处理的错误。

而 `KgAdapter` 的注释里有一句关键的性质声明：

> `KgAdapter` 通过文档引擎走编译后的图行。它**无状态且可安全跨 goroutine 共享**。

这条声明解释了为什么它能被装成一个全局单例：它只持有一个 `docEngine` 与一个可选的 `seedEncoder`，两者都在构造/注入之后不再改变。

**`seedEncoder` 是可选的，而它的缺失被定义成「功能降级」而不是「错误」。**

```go
// seedEncoder embeds the query for the dense seed search. nil (or a nil
// vector) degrades the seed search to a keyword match on the entity rows,
// so the walk still works on a deployment without a tenant embedder.
```

而 `seedVector` 里那个 `defer` 值得单读：

```go
func (a *KgAdapter) seedVector(ctx context.Context, tenantID, text string) []float64 {
    if a.seedEncoder == nil { return nil }
    defer func() {
        // A misconfigured embedding service must degrade to keyword seeding,
        // never take the turn down.
        _ = recover()
    }()
    vec := a.seedEncoder(ctx, tenantID, text)
    if len(vec) == 0 { return nil }
    return vec
}
```

**`recover()` 吞掉 panic**，注释给了理由：「一个配置错误的嵌入服务必须降级到关键词种子，绝不能把这一轮弄死」。

这是一个**刻意的降级**：`a.seedEncoder` 是外部注入的函数（可能是一个请求第三方服务的闭包），而它 panic（例如 nil map 访问、空指针解引用）会让整个检索轮失败。既然走关键词路径也能工作，那么 panic 就该被降级。

但代价也真实：**一个反复 panic 的 encoder 不会让任何人失败**，只会在日志里……其实连日志都没有（`_ = recover()` 连值都不取）。这是一个可以改进的地方——至少该 WARN 一次并带上 panic 值。

**`seedSearch` 的两个分支对应「有向量」与「无向量」，而两者都用 `mention_count` 重排。**

```go
if len(seedVec) > 0 {
    dense := &enginetypes.MatchDenseExpr{
        VectorColumnName:  common.VectorColumnName(len(seedVec)),
        EmbeddingData:     seedVec,
        EmbeddingDataType: "float",
        DistanceType:      "cosine",
        TopN:              kgSeedPool,
        ExtraOptions:      map[string]interface{}{"similarity": kgSeedSim},
    }
    rows := a.kgSearchRaw(ctx, kbID, docs, "entity", scopeKwd, nil, []interface{}{dense}, "mention_count_int", kgSeedPool)
    return kgTopMentionCount(rows, kgSeeds)
}
return a.kgSearch(ctx, kbID, docs, "entity", text, kgSeeds, scopeKwd, nil, "mention_count_int", kgSeedPool)
```

两条分支的差别不只是「用不用向量」，还有**谁做最后的筛选**：

| 分支 | 步骤 |
|---|---|
| 有向量 | KNN 取 64 个候选（`similarity >= 0.8`）→ **Go 侧**按 `mention_count` 重排 → 取前 2 |
| 无向量 | 关键词匹配 → **引擎侧** `orderDesc = "mention_count_int"` → 取前 2 |

所以有向量时排序在 Go 里做，无向量时排序下推到引擎。为什么？因为 KNN 与 `orderDesc` 两个排序维度在同一个查询里无法同时表达——`MatchDenseExpr` 已经指定了按相似度排序，所以 `orderDesc` 传的是 `""`（无排序），然后 `kgTopMentionCount` 在本地重排。

这是一个「两个排序维度冲突」的具体处置，而它带来一个隐含的语义：**向量的相似度门槛（0.8）先筛，`mention_count` 只用于在已过门槛的候选里挑**。所以「一个向量相似的冷门实体」可能输给「一个向量不那么相似但被提到很多次的实体」——后者更可能是真正的主题。

**`graphRowsExist` 与其它所有查询的关键区别是「它传播错误」。**

```go
// graphRowsExist reports whether one dataset holds ANY compiled entity row in
// the given scope. Unlike kgSearchRaw it PROPAGATES the engine error: the caller
// uses the answer to decide whether an empty walk is a dataset-level absence
// (which disables the tool) or a transient backend failure (which must not), and
// a swallowed error would silently turn the latter into the former.
```

这个理由值得完整引用：**调用方用这个答案来决定「空行走是数据集级缺失（会禁用工具）」还是「暂时性后端故障（不该禁用）」**，而一个被吞掉的错误会把后者静默变成前者。

所以同一个包里两类查询函数的错误策略相反：`kgSearchRaw` / `kgSearch` 吞错误返回空切片（因为在行走过程中，「这一跳没找到」是一个合理结果），而 `graphRowsExist` 必须传播（因为它的答案会**永久性地禁用工具**）。

这是一个很好的判据实例：**同一个错误在不同用途下的严重性完全不同**——「临时查不到」可以吞，「永久禁用」不能。

**「空行走」的三种归因，而代码把它们逐个区分了。**

```go
if len(entities) == 0 && len(relations) == 0 {
    if len(docs) > 0 {
        return empty, nil           // doc 作用域的空 = 查询未命中
    }
    for _, kbID := range datasetIDs {
        has, probeErr := a.graphRowsExist(ctx, kbID, nil, "")
        if probeErr != nil {
            return empty, probeErr  // 探测失败 = 基础设施错误
        }
        if has {
            return empty, nil       // 有图但这个查询没种上 = 查询未命中
        }
    }
    return KgExploreResult{GraphAbsent: true}, nil   // 确认无图 = 数据集级缺失
}
```

三种归因的注释各有一句：

1. **doc 作用域的空** ——「一个 doc 作用域的行走（模型传了 `doc_scope`）最多只能证明**那些**文档没有实体行——它对数据集什么也没说，所以它**不得**报告数据集级缺失：那会因为一次作用域内的未命中而禁用整个对话的 `graph_explore`」。
2. **探测失败** ——「传播一个探测失败，而不是把基础设施错误误读成『没有图』」。
3. **只有一次成功且什么也没找到的探测才是数据集级缺失**。

而 `KgExploreResult.GraphAbsent` 的字段注释把这个区分提升成了契约：

> `GraphAbsent` **只在**绑定作用域可证明完全不含编译实体行时为 true——那是一个**数据集级事实，在一个对话内不会变**，这正是让工具能禁用自身的东西。一个 `GraphAbsent` 为 false 的空行走是查询级未命中（图存在但这个查询没种上任何东西）或基础设施失败：**两者都不得禁用工具**，因为一个措辞更好的查询仍可能命中。

所以「能否禁用工具」的判据是**这个事实会不会在该对话内改变**。数据集有没有图不会变；查询能不能种上会变。

这与 g01 里「`ConversationKey` 让 compiled-knowledge 工具记录一次数据集级缺失然后自我移除直到 30 分钟过期」是同一个机制的供给端——这一页提供「可证明缺失」的判定，那一页负责「据此禁用」。

**`Explore` 的行走循环有三层去重，每层针对不同的重复。**

```go
seenNames := map[string]bool{}      // kbID + ":" + lowercase(name)
...
key := rel.ID                        // 关系按行 id 去重
if key == "" { key = rel.From + "|" + rel.To + "|" + rel.Type }
```

三处：

1. **实体按 `kbID + ":" + strings.ToLower(e.Name)` 去重** ——注意键里有 `kbID`，因为「同一个名字在两个数据集里是两个实体」。而 `ToLower` 让 `Apple` 与 `apple` 合并。
2. **关系优先按行 `id` 去重，无 id 时退化成 `from|to|type` 三元组** ——注释：「好让来自不同行的同一个端点对能存活」。这句话有点绕：它的意思是「若两行的端点对相同但 id 不同，两者都保留」——因为不同的关系行可能有不同的 `source_chunk_ids`，丢掉一个就丢了证据。
3. **邻居集合按 `strings.ToLower(name)` 去重但保留原始大小写** ——`neighSet[strings.ToLower(name)] = name`，一个 map 从 lowercase 到原始名。所以既能去重又能保住展示形式。

第 2 点的设计值得一提：**去重键的选择决定了「什么算重复」**。按 id 去重意味着「同一对实体之间的两条关系（不同类型，或来自不同文档）是两条」；按三元组去重会把它们合并成一条。所以优先用 id 是**更保守**（保留更多）的选择。

**跳数循环里有一个「用一个反转的 map 查已访问名」的写法。**

```go
seenLower := map[string]bool{}
for key := range seenNames {
    if strings.HasPrefix(key, kbID+":") {
        seenLower[strings.TrimPrefix(key, kbID+":")] = true
    }
}
```

所以每一跳开始时都要**从全局 `seenNames` 里筛出本 `kbID` 的名字并剥掉前缀**，建一个临时集合。这是「一个带命名空间的全局集合 + 一个需要不带命名空间的局部视图」的转换成本——每一跳 O(已见实体数)。

而它的必要性来自 `seenNames` 的键设计（`kbID:name`）：若键只是 `name`，那么跨数据集的同名实体会被误合并。所以这里用**每一次的 O(n) 转换**换取「命名空间正确」。

**`evidencePassages` 有一个「按文档分组再按组取」的公平性设计。**

```go
for _, group := range groups {          // groups 按 docID 分组，保持首次出现顺序
    if len(out) >= kgMaxPassages { break }
    remaining := kgMaxPassages - len(out)
    for _, p := range a.loadPassages(ctx, datasetIDs, group.ids, remaining) {
        out = append(out, p)
    }
}
```

所以是**逐个文档轮流取**，而不是「把所有 chunk id 一次性查然后取前 6」。注释没直接说理由，但这个结构的效果很清楚：**避免一篇文档占满全部 6 个段落位**。若一次性查，那么一篇贡献了 10 个 chunk id 的文档会独占前 6 个（因为 `loadPassages` 会按 `limit` 截断）。

所以这里的「分组 → 轮转」是一个**多样性保证**：一次行走返回的段落应覆盖多个文档，而不是同一篇的连续片段。这与 g01 里 `servedLedger` 按查询计数而不是按块计数是同一条原则——**避免一个维度上的量大压倒其他维度**。

**`loadPassages` 的两个失败方向都是「返回 nil 而不报错」。**

```go
res, err := a.docEngine.Search(ctx, ...)
if err != nil || res == nil {
    return nil
}
```

所以段落加载失败不会让整个行走失败——因为「子图已经拿到了」，段落是附加品。这是一个合理的分级：核心产物（实体 + 关系）与附加产物（原文段落）的失败严重性不同。

而内容被 `truncateRunes(content, kgPassageRunes)` 截断到 1500 字符，且**只取 `content_with_weight` 非空的**（`if strings.TrimSpace(content) == "" { continue }`）。

**`seedSearch` 里 `similarity` 作为 `ExtraOptions` 而不是独立字段。**

```go
ExtraOptions: map[string]interface{}{"similarity": kgSeedSim},
```

这是引擎层约定的形状（`ExtraOptions` 是一个开放的 map 供各引擎自己解读），所以这里只是把「相似度门槛 0.8」透传下去。而 `common.VectorColumnName(len(seedVec))` 说明**列名按维度派生**——同一份代码支持不同维度的嵌入模型，因为列名是 `vector_column_name(dim)` 这种形式。

**`DocOutline` 走的是完全不同的路径，而它的注释解释了为什么便宜。**

> 它返回每个文档编译后的图，一次搞定。编译器处理的每个文档都**只带一个 `graph` 行**，把它的全部实体与关系放在一起，所以调用方用一次抓取的代价拿到一篇长文档的整个骨架，而不是逐块扫描。实体按 `mention_count` 降序返回——**枢纽在前**。

所以这是「编译期就把整个文档的图打包成一行」的收益：读一篇长文档的结构不需要扫它的所有块。而 `kgOutlineMaxDocs = 3` / `kgOutlineMaxEntities = 15` / `kgOutlineMaxRelations = 20` 三个上限有一个明确的成本论证：

> `kgOutlineMaxEntities` / `kgOutlineMaxRelations` 把骨架限制在模型**真的能用**的范围内：那些枢纽。一个 outline 进入 ReAct 记录并**在之后每一个回合被重新发送**，所以一个 60 实体的倾倒比它本要替代的深读更贵。`mention_count` 排序已经把有用的实体放在前面，所以砍掉尾巴是免费的。

「一个 outline 进入 ReAct 记录并在之后每一个回合被重新发送」——这是 agentic 系统里一个反复出现的成本结构：**任何进入上下文的工具结果都是按轮计费的**。而 g01 里那个「探测结果不作为提示词文本」的理由正是同一条（「它会在本回合的每次调用上被重新计费」）。

两个上限的注释说的是「砍掉尾巴是免费的」，因为排序已经保证了头部有用——所以截断的损失是「可能错过一些不重要的实体」而不是「随机丢失」。

## 关键取舍

**源码事实：** 行走**从不调用聊天模型**，因为「在这个 ReAct 循环里模型已经是自己证据的裁判，一个工具不持有聊天模型，而只有答案的结果会剥掉答案必须引用的段落」；因此总是返回子图 + 段落。

**源码事实：** 预算链是 `kgSeeds=2` / `kgSeedPool=64` / `kgSeedSim=0.8` / `kgHops=2` / `kgNeighbors=128` / `kgRelLimit=32` / `kgMaxPassages=6`；种子选择是「先取 64 候选再按 `mention_count` 重排取前 2」——先宽后窄。

**源码事实：** 服务是「接口 + stub 默认值 + 实现」三件套；`SetKgService(nil)` **恢复 stub 而不是设 nil**，所以调用方永远不会拿到 nil 接口；未安装时的错误消息直接给出修复指引。

**源码事实：** `KgAdapter` 声明为「无状态且可安全跨 goroutine 共享」（只持 `docEngine` 与注入后不变的 `seedEncoder`），因此可以装成全局单例。

**源码事实：** `seedVector` 用 `defer recover()` **吞掉 encoder 的 panic**，「一个配置错误的嵌入服务必须降级到关键词种子，绝不能把这一轮弄死」——而 `_ = recover()` 连值都不取，所以没有日志。

**源码事实：** 有向量时 KNN 按相似度取 64 个候选后**在 Go 侧**按 `mention_count` 重排（`orderDesc` 传 `""`，因为 `MatchDenseExpr` 已占用排序维度）；无向量时排序下推到引擎（`orderDesc = "mention_count_int"`）。

**源码事实：** `graphRowsExist` 显式**传播**引擎错误（与同包其他查询相反），因为「一个被吞掉的错误会静默把暂时性故障变成『数据集级缺失』」，而后者会**永久禁用工具**。

**源码事实：** 空行走的三种归因被显式区分：doc 作用域的空 → 查询未命中（**不得**报告数据集级缺失，否则一次作用域内未命中会禁用整个对话的工具）；探测失败 → 传播错误；只有「探测成功且什么也没找到」→ `GraphAbsent: true`。

**源码事实：** `GraphAbsent` 的契约是「**数据集级事实，在一个对话内不会变**」——这正是让工具能自我禁用又能被重新启用的判据；false 的空行走可能是查询级未命中或基础设施失败，两者都不得禁用工具。

**源码事实：** 实体按 `kbID + ":" + lowercase(name)` 去重（命名空间必须带 `kbID`，否则跨数据集同名实体被误合并），因此每跳要 O(已见实体数) 转换出一个不带前缀的局部视图。

**源码事实：** 关系**优先按行 id 去重**、无 id 时退化成 `from|to|type` 三元组——按 id 是更保守的选择（同一对实体之间来自不同文档的两条关系都会保留，各自带自己的 `source_chunk_ids`）。

**源码事实：** `evidencePassages` 先按 `docID` 分组、再**逐文档轮流取**直到 `kgMaxPassages`，所以一篇贡献很多 chunk id 的文档不会独占全部段落位。

**源码事实：** `loadPassages` 的引擎错误与段落空内容都**返回 nil 而不报错**——段落是附加产物，核心产物（实体 + 关系）已经拿到。

**源码事实：** `DocOutline` 依赖「编译器为每个文档只写一个 `graph` 行」，所以一次抓取拿到整个骨架；`kgOutlineMaxEntities=15` / `kgOutlineMaxRelations=20` 的理由是「一个 outline 进入 ReAct 记录并在之后每一个回合被重新发送，60 实体的倾倒比它替代的深读更贵」。

**架构推论：** 「返回原料而不返回结论」这个选择在 agentic 系统里是**必须**的，而理由不是「工具不该聪明」而是**引用链**：模型的答案要能指回原文，而任何在工具内部做的总结都会把可引用的粒度毁掉。这与纯 RAG 里「检索器返回块、生成器写答案」的分工一致——**工具负责找到，模型负责判断**。而一旦工具开始判断，答案的可审计性就没了。

**架构推论：** 「同一个错误在不同用途下的严重性不同」这条判据在这一页体现得非常清楚：`kgSearch` 吞错误（行走中「这一跳没找到」是正常结果），`graphRowsExist` 传播错误（它的答案会永久禁用工具）。任何「一个查询函数的返回值被用作控制决策」的地方都必须传播错误——因为**吞掉错误等于把「不知道」当成「没有」**，而「没有」会触发一个不可逆的动作。

**架构推论：** 「空结果的归因」是 agentic 工具设计的核心难点。这一页给了三种归因和一条判据（「这个事实在对话内会不会变」）。而这条判据可以推广到任何「工具可自我禁用」的设计：**只有当「缺失」是一个不会因为换一个查询而改变的稳定事实时，才允许据此禁用**。否则一次措辞不佳的查询就会永久关闭一个能力。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/agentic_rag/kg_service.go` | `KgService` | `Explore` + `DocOutline` 两方法接口 |
| `internal/agentic_rag/kg_service.go` | `SetKgService` / `GetKgService` | 可安装服务，nil 恢复 stub |
| `internal/agentic_rag/kg_service.go` | `KgExploreRequest` | 租户 / 数据集 / 可选的 doc 作用域 / 查询 |
| `internal/agentic_rag/kg_service.go` | `KgExploreResult` | 子图 + 段落 + `GraphAbsent` 三态 |
| `internal/agentic_rag/kg_service.go` | `KgEntity` | 含 `MentionCount` 作为中心性代理 |
| `internal/agentic_rag/kg_service.go` | `KgRelation` / `KgPassage` | 关系行与原文段落 |
| `internal/agentic_rag/kg_service.go` | `KgAdapter` | 无状态图行走实现 |
| `internal/agentic_rag/kg_service.go` | `seedVector` | 可选编码器，panic 降级为关键词 |
| `internal/agentic_rag/kg_service.go` | `seedSearch` | 有向量走 KNN+本地重排，无向量走引擎排序 |
| `internal/agentic_rag/kg_service.go` | `graphRowsExist` | 唯一**传播错误**的查询，因答案会禁用工具 |
| `internal/agentic_rag/kg_service.go` | `evidencePassages` | 按文档分组轮流取，保证段落多样性 |
| `internal/agentic_rag/kg_service.go` | `loadPassages` | 失败返回 nil，段落是附加产物 |
| `internal/agentic_rag/kg_service.go` | `DocOutline` | 一次抓取整个文档骨架，枢纽在前 |
| `internal/agentic_rag/kg_service.go` | `kgHops` 等七个常量 | 行走的预算链 |
| `internal/agentic_rag/kg_service.go` | `ErrKgServiceMissing` | 未安装服务的错误，含修复指引 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：agentic_rag/kg_service.go](https://github.com/infiniflow/ragflow/blob/main/internal/agentic_rag/kg_service.go)
- [GraphRAG 论文：图索引驱动的检索增强](https://arxiv.org/abs/2404.16130)
- [LightRAG 论文：双级检索](https://arxiv.org/abs/2410.05779)

## 与主核 / AI 的关系

主核里最接近的是**图索引与多跳查询**：从索引里取根、沿边扩展、保证每一跳的预算，而「同一个节点名在不同 schema / namespace 里是不同实体」是一条硬规则。这里的 `kbID + ":" + lower(name)` 就是那条规则的实现——**命名空间必须在去重键里**。

**差别在于「返回什么」。** 数据库的多跳查询返回**行**，调用方自己拼；而这里返回的除了实体与关系，还必须有**原文段落**，因为 LLM 的答案要引用文本。所以这个服务在「图查询结果」之外多了一层「回读原文」的工作（`evidencePassages` / `loadPassages`），而这一层在主核里是不存在的——数据库的「原文」就是行本身。

与 c01/c02/c03（另一项目的图算法页，不适用于此）的区别需要说清：这一页的图**不是**跑社区划分的图，而是编译期产出的实体/关系行，用于 **局部多跳**检索。所以它没有模块度、没有剪枝，只有种子匹配 + 两跳扩展 + 原文回读。

与 g01（agentic 主循环）的关系是供给方与被统计方：`graph_explore` 是 g01 里 `ConversationKey` 记录「数据集级缺失」的两个工具之一；而 g01 的台账里，图探索的输出算 **served 而不算 retrieved**（因为它不渲染完整块内容）。

与 g03（精确串检索）的关系是互补通道：图行走回答「这两个概念之间有什么关系」，grep 回答「原文里有没有这个词」。两者都返回段落，但路径完全不同（一个是关系边、一个是字面匹配）。

与 g04（结构导航）的关系：`DocOutline` 返回的「一篇文档的整个骨架」正是 `navigate_structure` 要钻的那棵树的数据源之一。

对 AI / agent 工程最实用的一条是**「工具返回原料、模型做判断」**这条分工，以及它背后的**引用链**论证。任何 RAG/agent 系统里，工具内部一旦做了总结，答案就无法指回原文，于是「这个结论从哪来」变成不可验证。判据是：**这个工具的消费者需要引用吗？** 需要就不能在工具里做抽象。

另一条是「空结果的三种归因」与它配套的判据（**这个缺失在对话内会不会变**）。这条判据几乎可以直接照搬：任何「据此禁用某个能力」的判断，都必须建立在一个稳定事实上。而「工具调用失败」看起来像「没有」，但它可能是网络的、可能是措辞的——**把「不知道」当成「没有」并采取不可逆动作，是这类设计最常见的错**。

最后一条是 `DocOutline` 那三个上限背后的成本结构：**任何进入 ReAct 上下文的工具结果都是按轮计费的**。所以工具输出的「信息密度」比「信息量」重要——一个 60 实体的倾倒比它想替代的深读更贵，因为它会被重新发送 20 次。这个算术值得写进任何 agent 工具的设计说明里。
