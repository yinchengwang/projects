---
project: ragflow
feature: tool-registry
title: 平台工具注册与检索工具
page_type: feature
schema_version: 1
refs:
  - path: internal/agent/tool/registry.go
    symbols: [Factory, BuildByName]
  - path: internal/agent/tool/retrieval.go
    symbols: [RetrievalTool, NewRetrievalTool]
---

# 平台工具注册与检索工具

## 一句话职责

把四十多个外部能力的构造方式压成一张 `map[string]Factory`，再用 `BuildByName` 处理**名字的三种来源**（Agent 可见名 / Canvas 组件名 / 注册键名）之间的映射与冲突；而 `RetrievalTool` 是这张表里最特殊的一个——它不构造一个「外部服务客户端」，而是构造一个把 `dataset_ids`、`retrieval_from`、`use_kg` 等一堆检索参数收进**默认值**的工具实例，因为同一个 DSL 里可以有多个 Retrieval 节点，每个都要用不同的数据集。

## 机制是什么

`Factory` 的定义只有一行：

```go
type Factory func(params map[string]any) (einotool.BaseTool, error)
```

docstring 划清了一个容易混淆的边界：

> 按 DSL / Agent 可见名和可选的节点级配置构造一个工具实例。**配置 map 属于 Agent 节点 / DSL，而不是模型发出的 function-call 参数。**

这句话是理解整个工具层的钥匙。「两套参数」是这里最核心的区分：

| | 节点级 params | 模型给的 args |
|---|---|---|
| 来源 | DSL 里 `obj.params` | 模型调工具时生成的 JSON |
| 何时定 | 编译期 | 运行期 |
| 谁定 | 用户 / 画布 | 模型 |
| 例子 | `dataset_ids`、`api_key`、`top_n` 默认值 | `query`、`keywords` |

`Factory` 只处理左边那一列。所以「`top_n` 既可以出现在 DSL 也可以出现在模型参数里」这件事是**有意的重叠**：DSL 给默认值，模型可以覆盖。

而这张注册表有 **48 个键**，但它们不是 48 个工具。数一下实际能力：

- **搜索类**（duckduckgo / google / tavily / searxng / perplexity / youcom / search1api / wikipedia / arxiv / pubmed / google_scholar / querit …）
- **数据类**（akshare / tushare / wencai / yahoo_finance / jin10 / qweather / bgpt / keenable / sofya / specstream / deepl）
- **本地能力**（code_exec / crawler / web_crawler / email / execute_sql / exesql / wiki_query）
- **检索类**（retrieval / search_my_dataset / search_my_dateset / hybrid_search / vector_search / bm25_search / dataset_navigation_by_tree）

而键数多于工具数，因为**同一个实现被注册成多个名字**：

```go
"execute_sql": buildExeSQLTool,
"exesql":      buildExeSQLTool,
"google_scholar":          buildGoogleScholarTool,
"google_scholar_search":   buildGoogleScholarTool,
"crawler":                 noConfig("crawler", ...),
"web_crawler":             noConfig("web_crawler", ...),
"wikipedia":               buildWikipediaTool,
"wikipedia_search":        buildWikipediaTool,
"search_my_dataset":       buildRetrievalTool,
"search_my_dateset":       buildRetrievalTool,   // 注意拼写
```

所以这张表同时承担两个职责：**能力清单**与**别名表**。而别名存在的理由是历史（Python 版的命名）与前端（Canvas 组件名的连写形式）。

## 怎么实现的

**`noConfig` 是「不接受任何节点级参数」的显式声明，而它是一个行为选择而不是类型事实。**

```go
func noConfig(name string, fn func() einotool.BaseTool) Factory {
    return func(params map[string]any) (einotool.BaseTool, error) {
        if len(params) != 0 {
            return nil, fmt.Errorf("agent tool: tool %q does not accept node-level params", name)
        }
        return fn(), nil
    }
}
```

它把「零参数构造」变成一个**会在传参时报错**的工厂。所以 `code_exec` / `crawler` / `wiki_query` / `hybrid_search` 这几个被明确标成了「不该有节点级配置」。

这与每一类工具自己的 builder 形成对照：`buildTavilyTool` 之类会逐个读 key 并给默认值，而 `noConfig` 直接拒收。这个设计的好处是：**用户把 `top_n` 写到 `code_exec` 的 params 上时会得到明确错误，而不是被静默忽略**。

**`BuildByName` 处理了三件事：名字归一、组件名回退、元数据剥离。而第二件事藏着一个「信任谁」的判断。**

```go
key := normalizeToolName(name)
factory, ok := registry[key]

var componentKey string
if params != nil {
    if componentName, _ := params["component_name"].(string); componentName != "" {
        componentKey = normalizeToolName(componentName)
    }
}
```

`normalizeToolName` 先小写 + trim，再查 `canvasToolNames` 那张 18 条的别名表（`"codeexec"` → `"code_exec"`、`"tavilysearch"` → `"tavily"`、`"perplexitysearch"` → `"perplexity"`…）。注意别名表的键是**去掉了下划线的连写形式**——因为 Canvas 的组件名习惯是 `TavilySearch` 这种驼峰，小写后就是 `tavilysearch`。

然后是两条分支，注释把它们的目的写得很清楚：

```go
if !ok {
    // Canvas DSLs often give each retrieval instance a unique
    // function_name while keeping the component_name as "Retrieval".
    // Fall back to the component_name when the agent-visible name is
    // not itself a registered tool.
    if componentKey != "" {
        factory, ok = registry[componentKey]
        key = componentKey
    }
    ...
} else if componentKey != "" && componentKey != key {
    // The agent-visible name matches a registered tool, but the DSL
    // says this node is a different component (e.g., a Retrieval
    // instance whose function_name collides with "wikipedia"). Trust
    // the component_name so we build the intended tool, not the
    // registered one the function_name happens to match.
    if altFactory, altOk := registry[componentKey]; altOk {
        factory = altFactory
        key = componentKey
    }
}
```

第二条分支是这里最有价值的一段：**当「Agent 可见名」与「组件名」指向不同工具时，信任组件名**。而注释给了一个具体的冲突场景：「一个 Retrieval 实例的 `function_name` 恰好撞上 `wikipedia`」。

这揭示了一个设计事实：`function_name` 是**模型看到的名字**（用户可以自定义成任何东西，包括撞上注册键），而 `component_name` 是**画布节点的类型**（由前端下拉框产生，是受控集合）。所以后者比前者可信。而如果反过来信任 `function_name`，那么用户给检索工具起名叫 `wikipedia` 就会真的构造出 wikipedia 工具——一个纯粹因为命名撞车导致的能力错乱。

**元数据剥离的规则不是「全删」也不是「全留」，而是按工具类型分。**

```go
cleanParams := params
if cleanParams != nil {
    cleanParams = maps.Clone(cleanParams)
    delete(cleanParams, "component_name")
    if _, isRetrieval := retrievalRegistryKeys[key]; !isRetrieval {
        delete(cleanParams, "function_name")
        delete(cleanParams, "description")
    }
}
return factory(cleanParams)
```

三条规则：

1. **`component_name` 一律删** ——它是画布元数据，任何工具都不该看到。
2. **`function_name` / `description` 对非检索工具删** ——它们也是 DSL 元数据，「不是构造函数的参数」。
3. **对检索工具保留** ——因为每个 Retrieval 实例要**对外宣称自己的 DSL 定义名**（模型看到的是 `function_name` 而不是 `retrieval`）。

而 `retrievalRegistryKeys` 是一个 3 元素集合：

```go
var retrievalRegistryKeys = map[string]struct{}{
    "retrieval":         {},
    "search_my_dataset": {},
    "search_my_dateset": {},
}
```

注释说明了它为什么必须列出**所有**别名：「All registered retrieval aliases count — a DSL using `function_name` `search_my_dataset` resolves to that registry key, not `retrieval`」。所以这里的判据不能是 `key == "retrieval"`，必须是集合成员。这是一个真实的陷阱：如果写成前者，那么通过 `search_my_dataset` 别名构造的检索工具会丢掉自己的名字。

**`maps.Clone` 而不是原地删。**

`cleanParams = maps.Clone(cleanParams)` 保证调用方传进来的 `params` 不被改动。这与 f02 里 `deepCopyDSL` 的理由一样：DSL 的解码结果还可能被别处使用（例如日志、错误消息、回写给前端）。

**`ValidateSearchFilters` 的调用位置说明它是所有工具的共同前置校验。**

```go
if err := ValidateSearchFilters(params, false); err != nil {
    return nil, runtime.NewUserFacingError(fmt.Sprintf("agent tool %s: %s", name, err))
}
```

两件事值得注意：

1. **它在「工厂分发之前」调用** ——所以它校验的是**原始 params**（还没剥离元数据），覆盖所有工具而不只是搜索类。
2. **它返回的是 `UserFacingError`** ——说明这类失败（过滤器参数不合法）是「可以直接告诉用户的」，而 f03 讲过 `UserFacingError` 的契约正是「不带画布执行前缀直接返回客户端」。

**`buildRetrievalTool` 用「白名单循环」拒绝不认识的键，而白名单有 22 项。**

```go
for key := range params {
    switch key {
    case "dataset_ids", "kb_ids", "top_n", "rerank_candidates_count", "top_k", "similarity_threshold",
        "keywords_similarity_weight", "use_kg", "rerank_id", "empty_response",
        "highlight", "meta_data_filter", "retrieval_from", "memory_ids",
        "kb_vars", "cross_languages", "function_name", "description", "meta",
        "inputs", "outputs", "user_id":
    default:
        return nil, fmt.Errorf("agent tool: retrieval tool does not accept node-level param %s", key)
    }
}
```

这是一个**严格的接收面**：任何白名单外的键都直接失败。这与 `noConfig` 同一种风格（「不接受的参数要报错」），但它允许 22 项。

注意白名单里混着两类东西：真正的检索参数（`dataset_ids` / `top_k` / `use_kg`…）与 **DSL 元数据**（`function_name` / `description` / `meta` / `inputs` / `outputs` / `kb_vars`）。后者出现在白名单里不是因为工具会用它，而是因为**模板会把这些一起塞进 params**，所以必须容忍。而 `function_name` / `description` 会被真正读出来用（见下）。

**`RetrievalTool` 的 `defaults` struct 字段几乎全部是 `schema:"-"`。**

```go
type retrievalArgs struct {
    Query                    string         `json:"query" doc:"..." required:"true"`
    DatasetIDs               []string       `json:"dataset_ids,omitempty" schema:"-"`
    KBIDs                    []string       `json:"kb_ids,omitempty" schema:"-"`
    MemoryIDs                []string       `json:"memory_ids,omitempty" schema:"-"`
    UserID                   string         `json:"user_id,omitempty" schema:"-"`
    TopN                     int            `json:"top_n,omitempty" schema:"-"`
    ...
    FunctionName string `json:"-"`
    Description  string `json:"-"`
}
```

**只有一个字段暴露给模型：`query`。** 其余全部 `schema:"-"`，两个元数据字段连 `json` tag 都是 `-`。

所以同一个 struct 被用作两个东西：

1. **模型的工具 schema** ——只有 `query` 一个必填参数，因为其余都由 DSL 决定。
2. **节点级默认值的容器** ——`defaults retrievalArgs` 存 DSL 传来的 `dataset_ids` / `top_n` 等。

这个复用是刻意的，而 `schema:"-"` 是实现手段：一个 struct 的字段既可以是「模型可填的参数」也可以是「只在服务端装配时填的默认值」，用 tag 区分。而 `FunctionName` / `Description` 用 `json:"-"`（比 `schema:"-"` 更强）来表明它们**既不进模型的 schema 也不进模型的参数 JSON**，纯粹是服务端配置。

**`NewRetrievalToolWithDefaults` 做了两件名字解析的事。**

```go
if len(defaults.DatasetIDs) == 0 && len(defaults.KBIDs) != 0 {
    defaults.DatasetIDs = append([]string(nil), defaults.KBIDs...)
}
name := defaults.FunctionName
if strings.TrimSpace(name) == "" {
    name = retrievalToolName
}
desc := defaults.Description
if strings.TrimSpace(desc) == "" {
    desc = retrievalToolDescription
}
```

第一件：`kb_ids` 是 `dataset_ids` 的别名，而复制用 `append([]string(nil), ...)` 而不是直接赋值——避免两个字段共享底层数组（后续任一处 append 会互相影响）。

第二件：名字与描述都有兜底。而默认名是：

```go
// retrievalToolName preserves the Python typo ("dateset") for backward
// compatibility with existing Canvas DSLs that reference the tool by name.
const retrievalToolName = "search_my_dateset"
```

**为了兼容而保留一个拼写错误。** 注释说明得直接：既有 Canvas DSL 按这个名字引用这个工具，所以改不了。而注册表里那个 `"search_my_dateset"` 键也是同一个拼写——而它旁边还有一个拼写正确的 `"search_my_dataset"`。所以两个拼写都注册，都能解析，而默认的是**错的那个**（因为历史数据用的是它）。

这是一类很常见的工程事实：**一个拼写错误被写进用户可见的数据之后，就成了契约**。正确处置不是「修正它」，而是「保留旧的、新增正确的、把旧的那个设为默认以兼容」。

**`retrievalArgs` 有 19 个字段，而 `InvokableRun` 的校验顺序按「便宜到贵」排列。**

大致顺序（读代码可确认）：

1. `mergeDefaults` ——先把节点默认值合并进来。
2. `resolveRetrievalQuery(ctx, args.Query)` ——解析查询（可能含变量引用）。
3. `resolveRetrievalUserID` ——解析用户。
4. **打一条 debug 日志** ——记录本次的全部参数（含 `zap.Float64p` 处理可空指针）。
5. `if args.Query == ""` → 返回 `EmptyResponse`（**不是错误**）。
6. `if args.UseKG` → 返回 `Stub: true` 加 `ErrGraphRAGNotSupported`。
7. `if args.RetrievalFrom == ""` → 同 (5)，返回 `EmptyResponse`。
8. `retrieval_from` 不在 `{"dataset","memory"}` → 报错。
9. `dataset` 但无 `dataset_ids` → 报错；`memory` 但无 `memory_ids` → 报错。
10. 解析 dataset ids、解析 metadata filter。
11. 按 `retrieval_from` 分派到 `GetMemoryRetrievalService()` 或 `GetRetrievalService()`。

而这个顺序里有三个值得单说的判断：

**其一：空查询返回 `EmptyResponse` 而不是错误。**

```go
if args.Query == "" {
    return stubJSONWithErr(retrievalResult{FormalizedContent: args.EmptyResponse})
}
```

`args.EmptyResponse` 是 DSL 配的一句话（例如「没有找到相关内容」）。所以「模型传了空 query」不是失败，而是**返回一个由用户配置的空结果文本**。这是给模型的反馈，而不是给调用栈的错误。

**其二：`use_kg=true` 返回一个「成功返回但带错误字段」的形态。**

```go
if args.UseKG {
    return stubJSON(retrievalResult{
        Stub:  true,
        Error: ErrGraphRAGNotSupported.Error(),
    }), ErrGraphRAGNotSupported
}
```

注意它**同时**返回了一个 JSON 字符串与一个 error。所以调用方拿到的是「一个含有 `_ERROR` 字段的结果」+「一个错误值」。而 `ErrGraphRAGNotSupported` 的文案是中文的：「GraphRAG 检索暂不支持，请使用 Python Canvas 或关闭 use_kg」。

这是「同一故障两个受众」的又一处实例（f03 讲过这个原则）：error 值给框架（会记日志/中断），`_ERROR` 字段给模型（**模型可以据此改变策略**，例如关掉 `use_kg` 重试）。如果只返回 error，模型看不到原因；如果只返回 `_ERROR`，框架不会记录。

**其三：`retrieval_from` 是必填的，而它的来源不是模型。**

```go
if args.RetrievalFrom == "" {
    return stubJSONWithErr(retrievalResult{FormalizedContent: args.EmptyResponse})
}
```

而 `retrieval_from` 在 schema 里是 `schema:"-"`，所以模型不能填它——必须由 DSL 提供。于是「DSL 没配 `retrieval_from`」的表现是「每次调用都返回空结果文本」，而不是一个明确的配置错误。这是一个**静默降级**：用户看到的是「检索不到东西」，而真实原因是「节点配置缺一个必填项」。

**`AllowDenseFallback: new(false)` 在 `RetrievalTool` 与 `AgenticSearchTool` 里都出现了，而它有一个明确的语义。**

`new(false)` 是 Go 里取「指向 false 的指针」的写法（`new(T)` 分配零值并返回指针，而 `false` 是 `bool` 的零值）。所以它是**显式地关掉稠密兜底**。

而 `RetrievalTool` 里那段注释解释了为什么：

> Agentic: no dense-only rescue — **a row the query's own words never matched is not evidence the model asked for**.

这句话是这一页最深的一条设计判断：在纯向量检索里，「用向量相似度兜底」是提高召回的标准做法；但在 **agentic** 检索里，模型已经明确表达了自己的查询意图，所以「一个查询词完全没匹配上、只是向量近的块」不该被塞给模型——那会污染它的证据判断。所以 `AllowDenseFallback` 被硬关。

而 `AgenticSearchTool`（g04 会详述）里也是 `new(false)`，同一个理由。

**`RetrievalTool` 在最后做了一件「顺带记录」的事，而它跨越到了 canvas 层。**

```go
if state, sErr := runtime.GetStateFromContext(ctx); sErr == nil && state != nil && len(chunks) > 0 && args.RetrievalFrom == "dataset" {
    state.SetRetrievalReferences(referenceChunksFromRetrieval(chunks), referenceDocAggsFromRetrieval(chunks))
}
```

注释：「把 chunks 记进 canvas 状态，好让 Agent 的流后引用接地调用能读到它们。记录是 best-effort——当 canvas 状态没挂上（例如单元测试）时静默跳过。」

四点：

1. **它是 best-effort** ——拿不到 state 就跳过，不报错。这让工具可以脱离画布单独测试。
2. **只在 `retrieval_from == "dataset"` 时记** ——memory 检索的结果不进引用列表。
3. **只在有 chunks 时记** ——空结果不覆盖上一次的引用。
4. **它写的是 `state` 而不是返回值** ——所以引用信息走的是**旁路**（context 上的状态）而不是工具输出。理由显然：模型不需要看到引用元数据，但下游的引用接地步骤需要。

这与 f03 里「横切事实放 context」是同一条原则的应用：**工具的输出给模型，工具的副产物给系统**。

**`mergeDefaults` 的规则是「模型给的非零值优先，否则用 DSL 默认值」。**

```go
if len(args.DatasetIDs) == 0 && len(args.KBIDs) != 0 {
    args.DatasetIDs = append([]string(nil), args.KBIDs...)
}
if len(args.DatasetIDs) == 0 && len(r.defaults.DatasetIDs) != 0 {
    args.DatasetIDs = append([]string(nil), r.defaults.DatasetIDs...)
}
```

注意第一段：**模型给 `kb_ids` 也会被映射到 `dataset_ids`**。所以两个字段名在模型侧也互为别名，而优先级是「模型的 `dataset_ids` > 模型的 `kb_ids` > DSL 的 `dataset_ids`」。

而每一处都用 `append([]string(nil), ...)` 复制。这不是洁癖：`r.defaults` 是**跨多次调用共享**的（同一个工具实例被模型反复调用），若直接赋值，那么任何后续对 `args.DatasetIDs` 的 append 会写进 `r.defaults.DatasetIDs` 的底层数组——**一次调用污染后续所有调用的默认值**。

这是一个共享默认值 struct 时的经典陷阱，而每一处（`DatasetIDs` / `MemoryIDs` / `CrossLanguages` / `MetaDataFilter` 的 `maps.Clone`）都做了防护。

## 关键取舍

**源码事实：** `Factory` 的 docstring 划清了「节点级配置（DSL 给的）」与「模型发出的 function-call 参数」是两套东西，而 `Factory` 只处理前者。

**源码事实：** 注册表有 **48 个键**但对应约 40 个实际能力，因为同一实现被注册成多个名字（`execute_sql`/`exesql`、`google_scholar`/`google_scholar_search`、`crawler`/`web_crawler`、`wikipedia`/`wikipedia_search`、`search_my_dataset`/`search_my_dateset`）。

**源码事实：** `noConfig` 把「零参数构造」变成**会报错**的工厂：传任何 params 都返回「不接受节点级参数」。`code_exec` / `crawler` / `web_crawler` / `wiki_query` / `hybrid_search` / `vector_search` / `bm25_search` / `dataset_navigation_by_tree` 都用它。

**源码事实：** `normalizeToolName` 先小写 + trim 再查 18 条的 `canvasToolNames` 别名表，键是**去掉下划线的连写形式**（`"tavilysearch"` → `"tavily"`），因为 Canvas 组件名是小写驼峰。

**源码事实：** `BuildByName` 在「Agent 可见名」与「`component_name`」指向不同工具时**信任 `component_name`**——注释给的具体冲突场景是「一个 Retrieval 实例的 `function_name` 恰好撞上 `wikipedia`」。

**源码事实：** 元数据剥离按工具类型分：`component_name` 一律删；`function_name` / `description` 对非检索工具删、**对检索工具保留**（每个实例要对外宣称自己的 DSL 定义名）。判定用 `retrievalRegistryKeys`（3 个别名）而不是 `key == "retrieval"`，因为通过别名构造的检索工具也必须保留名字。

**源码事实：** `cleanParams` 走 `maps.Clone`，不修改调用方传进来的 params。

**源码事实：** `ValidateSearchFilters(params, false)` 在工厂分发**之前**调用，且返回 `runtime.NewUserFacingError`——所以这类失败可直接给用户看（f03 的 `UserFacingError` 契约）。

**源码事实：** `buildRetrievalTool` 用 **22 项白名单循环**严格拒收未知键，而白名单里混着真检索参数与 DSL 元数据（`function_name` / `description` / `meta` / `inputs` / `outputs` / `kb_vars`）。

**源码事实：** `retrievalArgs` 的 19 个字段里**只有 `query` 对模型可见**，其余全是 `schema:"-"`；`FunctionName` / `Description` 连 `json` tag 都是 `-`。所以一个 struct 同时充当「模型 schema」与「节点默认值容器」。

**源码事实：** 默认工具名是 `search_my_dateset`——**为了兼容既有 Canvas DSL 而保留的拼写错误**；`search_my_dataset` 也被注册，但默认用错的那个。

**源码事实：** `NewRetrievalToolWithDefaults` 用 `append([]string(nil), ...)` 复制 `KBIDs`，避免与 `DatasetIDs` 共享底层数组。

**源码事实：** `InvokableRun` 的校验按便宜到贵排列，且「空 query」与「未配 `retrieval_from`」都返回 `EmptyResponse` **而不是错误**；`retrieval_from` 不在两个合法值里才报错。

**源码事实：** `use_kg=true` **同时**返回一个带 `_ERROR` 字段的 JSON 结果**和**一个 error 值——前者给模型（模型可据此改策略），后者给框架（框架会记录）。

**源码事实：** `AllowDenseFallback: new(false)` 被硬关，注释理由是「一个查询自己的词从未匹配上的行，不是模型所要的证据」。

**源码事实：** 检索成功后**best-effort** 把 chunks 写进 canvas 状态（`SetRetrievalReferences`），仅当 `retrieval_from == "dataset"` 且有结果；拿不到状态就静默跳过。

**源码事实：** `mergeDefaults` 的优先级是「模型的 `dataset_ids` > 模型的 `kb_ids` > DSL 默认值」，且每个 slice 都复制——因为 `r.defaults` 跨多次调用共享，直接赋值会让一次调用污染后续默认值。

**架构推论：** 这张注册表的真正难点不在「有 48 个键」，而在**名字的来源有三种且会冲突**：DSL 的 `function_name`（用户可任意改、可撞车）、DSL 的 `component_name`（前端受控集合、可信）、以及注册表键（内部事实）。三者的可信度排序解释了那两条分支的写法——**信任受控来源而不是自由来源**。任何「用户可见的可变名字」与「内部枚举名」并存的地方都需要这条规则。

**架构推论：** 「零参数工具要报错而不是忽略多余参数」这个选择值得照抄。多数实现会让多余参数静默通过（反正构造时不用它），而后果是**用户以为配置生效了**（他把 `top_n` 写在 `code_exec` 上，期待限制输出，而它被无声丢弃）。报错把「配置无效」变成可见事实，代价是一个本来能跑通的 DSL 现在跑不通——而那个 DSL 本来就是用户配错了。

**架构推论：** 同一个 struct 既当「模型 schema」又当「默认值容器」，用 `schema:"-"` 区分，是 Go 类型系统有限时的合理压缩。代价是：**读这个 struct 无法一眼看出哪些字段模型能填**，必须逐个看 tag。收益是默认值的合并逻辑（`mergeDefaults`）可以用和模型参数完全一样的字段名与类型，不需要两套平行的 struct 加一层转换。判据是：**两套东西的字段集重叠度是否接近 100%？** 是的话共用 struct 加 tag 更好；否则该分开。

**架构推论：** 「空输入返回用户可配的空文本」而不是错误，是 agentic 工具的一条重要惯例。模型会犯错（传空 query、传不存在的 id），而工具的正确响应不是让整个 run 崩掉，而是**返回一个模型能读懂的反馈**。这一层里三种情况都这么做（空 query、未配 `retrieval_from`、`use_kg` 不支持），而只有「配置本身非法」（未知参数、非法 `retrieval_from` 值）才报真错误。**判据是：这个错误是模型的错还是配置的错？** 模型的错给反馈，配置的错要炸。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/agent/tool/registry.go` | `Factory` | `func(params) (BaseTool, error)`，只处理节点级配置 |
| `internal/agent/tool/registry.go` | `registry` | 48 个键的实现表（含别名） |
| `internal/agent/tool/registry.go` | `BuildByName` | 名字归一 + 组件名回退 + 元数据剥离 |
| `internal/agent/tool/registry.go` | `BuildAll` | 批量构造，params 按三种键序查找 |
| `internal/agent/tool/registry.go` | `normalizeToolName` | 小写 + trim + 查 Canvas 别名表 |
| `internal/agent/tool/registry.go` | `canvasToolNames` | 18 条连写名 → 注册键的别名表 |
| `internal/agent/tool/registry.go` | `noConfig` | 零参数工厂，多余参数报错 |
| `internal/agent/tool/registry.go` | `retrievalRegistryKeys` | 三个检索别名，用于判定「是否保留元数据」 |
| `internal/agent/tool/registry.go` | `buildRetrievalTool` | 22 项白名单严格接收，读 DSL 默认值 |
| `internal/agent/tool/registry.go` | `strictInt` | 严格整数解析，拒绝非整值与越界 |
| `internal/agent/tool/retrieval.go` | `RetrievalTool` | `defaults` + 名字/描述，按 `retrieval_from` 分派 |
| `internal/agent/tool/retrieval.go` | `NewRetrievalTool` | 无默认值构造 |
| `internal/agent/tool/retrieval.go` | `NewRetrievalToolWithDefaults` | 合并 DSL 默认值，`kb_ids` → `dataset_ids` |
| `internal/agent/tool/retrieval.go` | `retrievalArgs` | 19 字段，只有 `query` 对模型可见 |
| `internal/agent/tool/retrieval.go` | `retrievalToolName` | 保留 Python 拼写错误以兼容既有 DSL |
| `internal/agent/tool/retrieval.go` | `ErrGraphRAGNotSupported` | 中文文案，同时给模型与框架 |
| `internal/agent/tool/retrieval.go` | `mergeDefaults` | 模型值优先，每个 slice 复制防污染 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：agent/tool/registry.go](https://github.com/infiniflow/ragflow/blob/main/internal/agent/tool/registry.go)
- [ragflow 源码：agent/tool/retrieval.go](https://github.com/infiniflow/ragflow/blob/main/internal/agent/tool/retrieval.go)
- [eino 文档：工具组件](https://www.cloudwego.io/docs/eino/core_modules/components/tool/)

## 与主核 / AI 的关系

主核里最接近的是**函数注册表 / 过程目录**：`pg_proc` 里每一行是一个可被调用的函数，有名字、参数类型、实现入口，而 `pg_proc` 的唯一约束是 `(proname, pronamespace, proargtypes)` 三者组合——所以**同名不同参数是可以共存的**。这里的注册表是它的简化版：只有名字一个键，所以重名只能靠别名表映射而不是重载。

**差别在于「谁可信」。** 主核的函数名字由 DDL 定义、由目录管理，所以「名字 → 实现」是确定的单向映射。而这里的名字有三个来源（用户自定义的 `function_name`、前端的 `component_name`、内部注册键），且用户可任意改第一个。所以 `BuildByName` 那段「信任 `component_name`」的逻辑，在数据库里根本不会出现——因为不存在「用户给函数起别名并撞上系统函数」这种情况。**输入可信度不同，查找逻辑的复杂度就不同。**

与 f01（画布）的关系是数据来源：`CanvasComponentObj.ComponentName` 就是这里当 `component_name` 来用的那个值。所以 f01 说「组件名匹配 `agent/component/<name>.py` 的类名」，而这一页说「这个名字不可信，真名要靠注册表归一」。

与 f03（Agent 运行时）的关系有两处直接连接：`runtime.NewUserFacingError` 在这里被用来包装参数校验失败；`runtime.GetStateFromContext` 被用来写检索引用。所以这一页是 f03 契约的**消费者**。

与 g04（结构导航与 agentic 检索工具）的关系最紧：`hybrid_search` / `vector_search` / `bm25_search` 三个注册键都指向 `NewAgenticSearchTool`，而那个工具的实现与语义在 g04 里。所以这一页解释「为什么这三个键都长得一样」，g04 解释「它们内部怎么把 mode 变成融合权重」。

对 AI / agent 工程最实用的一条是**「两套参数必须显式分开」**。任何 agent 工具框架都有「节点配置」与「模型参数」两类输入，而混在一起的后果是：模型可能填一个本该由配置决定的字段（于是绕过安全边界），或者配置被当成模型参数送进 schema（于是模型看到一堆不该看到的字段）。这一层的做法——`Factory` 只收前者、`schema:"-"` 隐藏后者——是标准解。

另一条是 `noConfig` 那种「多余参数报错」。它把「用户配错了」变成启动期错误而不是运行期的静默行为差异，这与 e04 里「`recursionLimit` 太小会让正常的多轮工具调用被误判为死循环」是同一类问题的另一面：**配置与行为的错位要么在启动时暴露，要么在事后追查，而启动时暴露便宜得多。**

最后一条是 `new(false)` 那个「关掉稠密兜底」的判断。它值得单独记住，因为它**反了向量检索的默认直觉**：在 RAG 里「用向量相似度找近似内容」通常是对的，但在 agentic 检索里，模型已经明确说了要什么，而「查询词完全没匹配上、只是向量近」的块会污染它的证据判断。判据是：**这次检索是「找相关内容」还是「执行模型的具体请求」？** 前者可以兜底，后者不行。
