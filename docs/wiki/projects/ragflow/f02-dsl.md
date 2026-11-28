---
project: ragflow
feature: dsl
title: DSL 抽取与归一
page_type: feature
schema_version: 1
refs:
  - path: internal/agent/dsl/extract.go
    symbols: [ExtractComponentParams, ExtractMode, ExtractPrologue]
  - path: internal/agent/dsl/normalize.go
    symbols: [NormalizeForCanvas, NormalizeForRun]
---

# DSL 抽取与归一

## 一句话职责

同一份用户 DSL 要被两拨完全不同的人读：前端要它来**画图**，编译期要它来**执行**——而这两拨人对同一份数据的合法范围不同。这个包的回答是**两个入口函数、一条带开关的流水线**：`NormalizeForCanvas` 只做前端能理解的事（补图、修句柄、把后端私有的节点类型藏回前端的名字），`NormalizeForRun` 额外允许**折掉遗留的子节点、把 Iteration 改名成 Parallel**，因为它的输出永远不会回到前端。

## 机制是什么

包注释先声明了「唯一规范线形状」：

```
{
  "globals":   { ... },
  "graph":     { "nodes": [...], "edges": [...] },   // React-Flow layout
  "variables": { ... },
  "components": { "<Name>:<UUID>": {                 // execution topology
    "downstream": [...], "upstream": [...],
    "obj": { "component_name": "Name", "params": {...} }
  }},
  "path": [...], "retrieval": {...}, "history": [...]
}
```

然后一句话点破了这个形状的**历史成因**：

> Go 服务端的代码（handler/service/Compile）读 `components` 块——Python 服务端（`agent/canvas.py`）也一样。`graph` 块由 React-Flow 前端消费来渲染画布。**任何一侧在某一行上缺失都是可能的**（例如一个从 Python 服务端手工导出的 v1 存根有 `components` 但没有 `graph`；而 Go 移植版测试套件里的 v1 fixture 既有 `graph` 也有 `components` 但内部布局约定略有不同）。

所以「同一份数据里有两套图表示」不是设计，是**两个服务端 + 一个前端各自需要的东西被塞进同一个 JSON** 的结果。`components` 是执行拓扑（谁接谁、每个节点什么参数），`graph` 是绘制拓扑（节点坐标、连线句柄、React-Flow 的 `parentId` 分组）。它们**可能互相不一致**，因为历史上它们分别由不同的写入路径产生。

这解释了为什么这个包的核心职责是「归一」而不是「校验」：**输入本来就可能缺一半，而缺哪一半取决于它来自哪个服务端**。

而两个入口函数的差别，注释写得比任何文档都清楚：

> `NormalizeForCanvas` 是每一个面向 Go 前端的路径的解码边界入口（`handler.AgentHandler`、`service.AgentService` 的 create/update/publish/reset、版本读取）……**它必须不把 runtime-only 的节点类型（例如 `Parallel` / `parallelNode`）泄漏进 `graph.nodes`，也不得重写用户写的 DSL 语义。runtime-only 的折叠住在 `NormalizeForRun` 里。**

> `NormalizeForRun` 与 `NormalizeForCanvas` 不同，它**允许把遗留的 LoopItem / IterationItem 子节点折掉，并把 Iteration 改名成 Parallel**，因为返回的 map 永远不会回到前端。

所以这是一条**信息损失的分界线**：面向用户的路径不许丢信息（用户下次还会编辑它）；面向运行时的路径可以丢（它只用一次）。把这条线落在「同一个私有函数 + 一个布尔」上，是页 8 这种设计的标准形态。

## 怎么实现的

**`normalize` 的第一件事是深拷贝，而理由不是洁癖。**

```go
out := deepCopyDSL(dsl)
```

注释：

> 归一流水线**原地**重写 `graph.edges[*].sourceHandle` / `targetHandle`、删除 `components` 条目、并改写 `components[*].obj.component_name`。没有深拷贝的话，复用原始解码 map 的调用方会观察到副作用。

所以深拷贝保护的是「调用方还拿着原 map」这个事实。在面向用户的路径上这一点尤其重要，因为 handler 往往要把原 DSL 一起返回给前端。

**第 (1) 步修 React-Flow 的句柄 id，而它的判据是「白名单两个值、其余一律不动」。**

```go
// Only rewrite plain "start"/"end" conventions. Agent/tool
// handles carry semantic info we must not stomp.
if src, _ := m["sourceHandle"].(string); src == "end" || src == "start" {
    m["sourceHandle"] = "start"
}
if dst, _ := m["targetHandle"].(string); dst == "start" || dst == "end" {
    m["targetHandle"] = "end"
}
```

所以不是「把 source 改成 A、target 改成 B」，而是**两个方向都先把值限定在 `{"start","end"}` 里，然后各自收敛到一个固定值**：`sourceHandle` 一律收敛成 `"start"`，`targetHandle` 一律收敛成 `"end"`。

这个收敛方向看起来反直觉（React-Flow 里 source 通常是输出侧），但代码注释给出了前端的实际约定：

> Source/target handle ids match the front-end's React Flow convention (`web/src/pages/agent/hooks/use-add-node.ts`): **source node renders its OUTPUT handle with id = "start"; target node renders its INPUT handle with id = `"end"`**.

所以前端把一个节点的输出句柄命名为 `"start"`、输入句柄命名为 `"end"`——**名字指的是「这条边在流程中的角色」而不是「句柄在节点上的位置」**。而后端只需要把任何 `start`/`end` 的混用收敛到这个约定。

而 `src == "end" || src == "start"` 这个双重判断才是「不 stomp 语义」的实现：只有值落在这两个白名单词里才改写，其余（例如 Agent 的 `tool_1` 出口、工具的具名句柄）一律跳过。注释说的「Agent/tool handles carry semantic info we must not stomp」就是这个意思——**限制判据而不是限制位置**。

**第 (2) 步在 `graph.nodes` 缺失时从前端不可见的 `components` 反向构造一张图，而排序的理由是一次真实的 bug。**

```go
// Go's map iteration is randomised. Sort the component ids before
// iterating so the layout (x = 50 + i*350) is a stable function of
// the input dsl, not of Go's runtime iteration lottery. Two
// normalize passes over the same dsl also used to produce
// `components` and `graph.nodes` in different orders, which broke
// the dslToGraph equality invariant; sorting here closes that gap.
```

这段注释值一整段：它把「为什么必须排序」讲成了一个**实际被打破的不变式**。Go 的 `map` 迭代顺序是随机的，而布局是 `x = 50 + i*350`——所以同一份 DSL 两次归一可能给出两份节点顺序不同的图。后果不是「看起来乱」，而是 `dslToGraph` 的**相等性不变式**被打破：同一个逻辑图的两个表示不再字节相等，于是任何基于比较的判断（缓存命中、幂等性检查、diff）都会失效。

**而「确定顺序」之外还有第二个不变式：字段必须总存在，即使为空。**

```go
// Always emit `data.form` (even when empty) so the React
// Flow node shape is byte-equal between the Python v1
// fallback (which reads `obj.params` and may be `{}`) and
// the Go v2 path.
```

以及 `normalized` 里对应的那句：「Always emit `params` (even when empty) so the normalised component shape matches the Python v1 server byte-for-byte.」

所以这个函数的正确性标准不是「前端能渲染」，而是**「Python v1 服务端产出的形状与 Go v2 产出的形状字节相等」**。这就是为什么会给空值也写字段：漏掉一个 `form: {}` 会让两条路径的输出在 JSON 层面不同，而那正是 `dslToGraph` 相等性检查失败的原因。

这两个不变式合起来解释了这个函数的真实职责：它不只是「补一张图」，而是**在两个服务端的输出之间建立字节级可比性**。

**第 (3) 步把后端私有的节点类型藏回前端的名字，而它有五个改写点而不是一个。**

```go
func repairParallelLeaksForCanvas(dsl map[string]any) {
    // components 层：obj.component_name、comp.name
    // graph.nodes 层：node.type、data.label、data.name
}
```

五个点：`components[*].obj["component_name"]`、`components[*]["name"]`、`graph.nodes[*]["type"]`（`"parallelNode"` → `"iterationNode"`）、`data["label"]`、`data["name"]`。

为什么要改五个地方？因为一个节点的名字在这份 DSL 里**有五个副本**，而只要漏一个，前端上就会显示成 `Parallel`（用户从没见过的词）或者渲染成后端私有节点类型。这是一个典型的「同一个事实的多个镜像点」问题——`NormalizeForCanvas` 的职责就是把这五个副本重新对齐成一个。

而注释点明了它的动机不是兼容性而是**不泄漏实现细节**：

> 这保持了响应载荷可渲染，而**不暴露后端实现细节**。

所以「Parallel」是运行时的内部概念，而「Iteration」是产品词汇。同一个东西两个名字，用户只该看到后者。

**第 (4) 步只在 `foldLegacy` 为真时执行，而它包含两件事。**

```go
if foldLegacy {
    foldLegacyLoopVariants(out)
    rewriteLegacyIterationAliases(out)
}
```

**`foldLegacyLoopVariants` 的循环里有一处「宁可保留也不删」的判断，值得单读。**

```go
parentID, ok := childOf[childID]
if !ok {
    // No parent visible in the graph via parentId
    // mapping. Keep the child -- deleting it could
    // leave dangling downstream references in the
    // parent component.
    continue
}
```

所以「找不到父节点」时**不删子节点**。理由很实：删掉它可能让父组件的 `downstream` 里留下指向不存在节点的引用。也就是说，这个折叠步骤的失败方向是「留着多余的东西」（无害）而不是「制造悬空引用」（有害）。

而父指针的来源是 `graph.nodes[*].parentId`——一个 React-Flow 的**节点级**字段，注释特意标注「verified in testdata/all.json」。所以「哪个节点属于哪个 Loop」这个分组事实**只存在于绘制拓扑里**，不在执行拓扑里。这解释了为什么折叠只在 `NormalizeForRun` 里做：执行拓扑本身不知道自己有多少个 LoopItem 子节点，它得去读 `graph`。

而注释说明了这个前提的脆弱性：

> 这一步用 `graph.nodes[*].parentId` 来发现父子关系；**如果 `graph` 仍然缺失，折叠退化成纯改名**（`component_name`：`"Iteration"` → `"Parallel"`；`LoopItem`/`IterationItem` 的名字留在 `components` 里，而下游的 compile/expand 路径必须容忍它们）。

所以折叠有两个完成度等级：有 `graph` 时能真正合并子节点，没 `graph` 时只能改名并**把「容忍遗留子节点」这个负担转嫁给下游编译路径**。这是一个诚实的降级声明，而不是假装两种输入都一样。

**合并 downstream 的顺序与「把子节点从目标列表里删掉」这一步。**

```go
childDS := toStringSlice(childCompDownstream(comp))
merged := mergeDownstream(toStringSlice(parentComp["downstream"]), childDS)
merged = removeFromSlice(merged, childID)
parentComp["downstream"] = stringsToAny(merged)
```

三件事：

1. **把子节点的 downstream 并进父节点** ——因为折叠后子节点不再存在，它的出口必须挂到父节点上，否则图断掉。
2. **从父节点的 downstream 里删掉 `childID`** ——注释：「子节点是入口节点，不是执行目标，所以一旦折叠它就不能出现在任何边里」。这是折叠正确性的关键：父节点的 downstream 原本可能写着「先跑 LoopItem」，折叠后那个 id 就不存在了。
3. **`stringsToAny(merged)`** ——把 `[]string` 转成 `[]any` 再存。注释解释了原因：「结果被存成 `[]any`（不是 `[]string`），好让消费者可以 `parent["downstream"].([]any)` 类型断言而不丢数据」。这是一个很窄的兼容性要求：DSL 是 JSON 解出来的，所有数组天然是 `[]any`，若这里写回 `[]string`，下游那个类型断言会失败。

**改名步骤重写两个 key 与 graph 节点的三个字段。**

```go
if obj, ok := comp["obj"].(map[string]any); ok {
    obj["component_name"] = componentNameParallel
}
comp["name"] = componentNameParallel
```

注释：「`component_name` 住在 `obj.component_name`（v1 形状）或 `name`（Go 扁平形状）——我们把两个 key 都重写以求稳」。

所以**两种形状同时存在**（v1 嵌套 vs Go 扁平），而代码不知道手里这个是哪一种，于是两个都写。这是一个「不确定输入形状时写两个位置」的实用做法，代价是可能造出同时有两个名字副本的对象。

而 graph 节点那三行把 label / name / type 一起改，注释给出因果链：「也重写 graph 节点的 label，好让 React-Flow 渲染器的 `componentNameToNodeTypeMap` 查找（`"Parallel"` → `"parallelNode"`）在下次重绘时成功」。这句话很值得注意：**「下次重绘」意味着这份归一结果会回到前端**——而这与「runtime-only 折叠」不该泄漏到前端的原则表面冲突。实际不会，因为 `NormalizeForRun` 的输出不写回数据库；但这段注释描述的是一条**只为内部一致性服务**的改写。

**`rewriteLegacyIterationAliases` 是递归遍历整个 DSL 把字符串里的旧引用换掉。**

```go
var legacyIterationAliasPattern = regexp.MustCompile(`IterationItem:[A-Za-z0-9_:-]+@(item|index)\b`)
```

它遍历 `map[string]any` 的每个值：字符串直接替换、`map[string]any` 递归、`[]any` 逐项。注释：

> 把对遗留 `IterationItem` 子节点的合成输出的 runtime-only 引用重写回 `CanvasState` 暴露的现代 `item`/`index` 别名。**这只在 runtime 归一的副本上跑，从不在面向前端的画布视图上跑。**

所以折叠把子节点删掉之后，**任何引用「那个子节点的第 item 个输出」的模板字符串都会指向不存在的节点**。这个遍历就是在修这些引用。而它是全 DSL 无差别递归的——包括 `params` 里的每一个字符串。这意味着它**没有 schema 知识**，只能靠正则认出形如 `IterationItem:<uuid-ish>@item` 的引用。

**`extract.go` 的四个函数，而它们的错误处理精度差异很大。**

`ExtractComponentParams` 与 `ExtractComponentInputForm` 走同一条导航链（`navigateToComponent` → `obj` → `params`），但**`ExtractComponentParams` 在 `params` 缺失时返回 `nil, nil`**：

```go
if !exists || rawParams == nil {
    return nil, nil
}
```

而 `ExtractComponentInputForm` 在 `params.inputs` 缺失时报 `ErrMissingInputForm`：

```go
if !exists || rawForm == nil {
    return nil, fmt.Errorf("%w: component %q has no params.inputs", ErrMissingInputForm, componentID)
}
```

这个差异的逻辑是：调试处理器**可以接受一个没有参数的组件**（它就是来跑一下看看），但**不能接受一个没有 input form 的 Begin 组件**（那说明 DSL 本身坏了）。所以前者「没有就是空」，后者「没有就是错」。

**而两处对「类型错误」的处理都是「不collapse」。**

```go
// Type errors (params or inputs is e.g. a list or a string) are NOT
// collapsed into ErrMissingInputForm -- they would mask a contract
// violation in the DSL and let DebugComponent run against corrupt
// data. CodeRabbit PR review #1 on PR #16403.
```

这条注释直接引用了一次 code review：有人要求把「类型错」也归到「缺失」，作者拒绝了，理由是**那会让调试处理器跑在损坏的数据上**。区分「没有这个字段」与「这个字段类型不对」在这里是有意义的：前者是可选，后者是契约违反。

**而 `ExtractPrologue` / `ExtractMode` 完全放弃了这个区分。**

```go
obj, _ := comp["obj"].(map[string]any)
params, _ := obj["params"].(map[string]any)
s, _ := params["prologue"].(string)
return s, nil
```

三处 `_` 把类型断言错误全部吞掉，于是「obj 不是 map」与「prologue 是 int」与「一切正常但没有 prologue」都返回空字符串。这两个函数只报一种错：`FindBeginComponentID` 找不到 Begin。

这个不对称是刻意的取舍：`prologue` / `mode` 是**可选展示字段**（开场白、对话模式），拿不到就当作没配；而 `params` / `inputs` 是**执行必需的**，缺了必须报。但代价是：一个把 `prologue` 写成数字的 DSL 会被静默当成「没有开场白」，而没有任何日志。

## 关键取舍

**源码事实：** 规范线形状里**同时**有 `graph`（React-Flow 绘制拓扑）与 `components`（执行拓扑），两者可能互相缺失、也可能不一致；原因是有两个服务端各读自己那份，而历史导出的存根可能只有一半。

**源码事实：** `normalize` 第一步就 `deepCopyDSL`，因为后续所有改写都是原地的（句柄、删 `components` 条目、改 `component_name`），不拷贝会让调用方观察到副作用。

**源码事实：** `NormalizeForCanvas` 明确承诺「不把 runtime-only 节点类型泄漏进 `graph.nodes`、不重写用户写的 DSL 语义」；runtime-only 的折叠只在 `NormalizeForRun` 里，因为后者的输出不回前端。

**源码事实：** `repairParallelLeaksForCanvas` 要改**五个**副本（`obj.component_name` / `comp.name` / `node.type` / `data.label` / `data.name`），因为同一个节点名在 DSL 里有五个镜像点；动机是「不暴露后端实现细节」。

**源码事实：** `foldLegacyLoopVariants` 在「找不到父节点」时**保留子节点不删**，理由是删掉可能在父组件的 `downstream` 里留下悬空引用——失败方向选「留多余」而非「造悬空」。

**源码事实：** 父子关系只来自 `graph.nodes[*].parentId`（React-Flow 节点级字段）；`graph` 缺失时折叠退化成**纯改名**，并把「容忍遗留 LoopItem/IterationItem」的负担转嫁给下游 compile/expand 路径。

**源码事实：** 折叠时把子节点 downstream 并进父节点，并从父节点 downstream 里删掉 `childID`（「子节点是入口节点，不是执行目标」）；合并结果显式转成 `[]any`，好让下游 `parent["downstream"].([]any)` 断言不丢数据。

**源码事实：** 改名同时写 `obj.component_name` 与 `comp["name"]`，因为 v1 嵌套形状与 Go 扁平形状两种都存在，代码不区分、两个都写。

**源码事实：** `rewriteLegacyIterationAliases` 用正则 `IterationItem:[A-Za-z0-9_:-]+@(item|index)\b` **无 schema 地递归遍历整个 DSL 的每个字符串**，只为了修那些指向已删除子节点输出的模板引用。

**源码事实：** `ExtractComponentParams` 在 `params` 缺失时返回 `nil, nil`（无参组件是合法的），而 `ExtractComponentInputForm` 在 `params.inputs` 缺失时报 `ErrMissingInputForm`（Begin 缺 input form 说明 DSL 坏了）。

**源码事实：** 两处都把「类型错误」单独报 `ErrMalformedDSL` 而**不**归入「缺失」，注释明确引用了一次 code review 并说明理由：「那会掩盖 DSL 里的契约违反，并让 `DebugComponent` 跑在损坏的数据上」。

**源码事实：** `ExtractPrologue` / `ExtractMode` 用三个 `_` 吞掉全部类型断言错误，于是「obj 不是 map」「prologue 是数字」「没配 prologue」三者都返回空串，且无日志。

**架构推论：** 「一份数据、两个读者、两个归一入口」这个结构是**多人协作演化的必然产物**，不是设计。前端要绘制（需要坐标与句柄），运行时执行（需要拓扑与参数），而两边的写入路径历史上各自演化。正确处置不是「合并成一种表示」，而是**在边界处按读者分别归一、并明确哪一侧允许丢信息**——把「谁能看到什么」变成显式的两个函数，比试图让一份数据同时满足两个消费者可靠得多。

**架构推论：** 元数据会以多个副本存在（这里一个节点名有五个位置），而副本必然漂移。`repairParallelLeaksForCanvas` 的存在就是这个漂移的补丁：每当内部改名（Iteration → Parallel），所有副本都要重新对齐。判据是：**同一个事实存在于 N 个位置时，每次改名都需要 N-m 处修改**；减少 N 才是根治，而对齐 N 只是止血。这一层选的是止血，因为净化 DSL 形状会破坏前端契约。

**架构推论：** 归一函数的失败方向必须按「哪种损失更贵」逐个决定，而不是统一选一边。这里的三个例子各不相同：找不到父节点 → 保留（悬空引用更贵）；`params` 缺失 → 返回 nil（调试可以无参）；`inputs` 缺失 → 报错（契约违反）。**把这三个都做成「报错」或都做成「宽容」，都会在某一侧造成更难查的故障。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/agent/dsl/normalize.go` | `NormalizeForCanvas` | 面向前端的归一，不泄漏 runtime-only 节点类型 |
| `internal/agent/dsl/normalize.go` | `NormalizeForRun` | 运行期归一，允许折掉遗留子节点并改名 |
| `internal/agent/dsl/normalize.go` | `deepCopyDSL` | 深拷贝，因为后续改写全是原地操作 |
| `internal/agent/dsl/normalize.go` | `enforceHandleIds` | React-Flow 句柄规范化，跳过工具/Agent 自定义句柄 |
| `internal/agent/dsl/normalize.go` | `buildGraphFromComponents` | 从执行拓扑反向构造确定性的默认布局图 |
| `internal/agent/dsl/normalize.go` | `repairParallelLeaksForCanvas` | 五个副本从 Parallel 改回 Iteration |
| `internal/agent/dsl/normalize.go` | `foldLegacyLoopVariants` | 折掉 `LoopItem` / `IterationItem`，改 `Iteration` → `Parallel` |
| `internal/agent/dsl/normalize.go` | `buildParentMap` | 从 `graph.nodes[*].parentId` 建父子映射 |
| `internal/agent/dsl/normalize.go` | `rewriteLegacyIterationAliases` | 无 schema 递归替换遗留输出引用 |
| `internal/agent/dsl/extract.go` | `ExtractComponentParams` | 取 `obj.params`，缺失返回 `nil, nil` |
| `internal/agent/dsl/extract.go` | `ExtractComponentInputForm` | 取 `obj.params.inputs`，缺失报专用错误 |
| `internal/agent/dsl/extract.go` | `ExtractPrologue` | 取 Begin 的 `prologue`，三个 `_` 吞掉所有类型错 |
| `internal/agent/dsl/extract.go` | `ExtractMode` | 取 Begin 的 `mode`，同上 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：agent/dsl/normalize.go](https://github.com/infiniflow/ragflow/blob/main/internal/agent/dsl/normalize.go)
- [ragflow 源码：agent/dsl/extract.go](https://github.com/infiniflow/ragflow/blob/main/internal/agent/dsl/extract.go)
- [React Flow 文档：节点与边](https://reactflow.dev/learn/concepts/core-concepts)

## 与主核 / AI 的关系

主核里最接近的是**查询重写（query rewrite）**：同一条用户 SQL 要经过解析、语义检查、然后被改写成另一棵可执行的树，而原始文本要保留下来供日志与错误消息使用。这里的两级归一与之同构：`NormalizeForCanvas` 相当于「保持原文可再次解析」，`NormalizeForRun` 相当于「生成执行计划，此时可以做不可逆的简化」。

**差别在于「是否存在规范形式」。** 数据库的 SQL 有明确的语法、明确的语义，所以「重写前的树」与「重写后的树」在语义上等价是可证明的。而这里的两份 DSL 之间**没有等价性保证**：`NormalizeForRun` 折叠子节点、改节点名，这些变换是**有损的**，且它的正确性依赖「下游 compile/expand 认得改后的形状」这个口头约定而不是类型检查。所以数据库的优化器可以放心丢信息，这里必须把「允许丢」限制在「输出不回前端」这条硬边界上。

与 f01（画布与组件工厂）的关系是直接的上下游：f01 讲 `Canvas` 这个已经读进来的结构，这一页讲「读进来之前/之后如何被规范化」。而两页共享同一个关键事实：**执行拓扑与绘制拓扑是两套表示**——f01 里那是 `Canvas.Components` 与 `NodeParents` 的关系，这里那是 `components` 与 `graph.nodes` 的关系。

与 f03（Agent 运行时）的关系：`rewriteLegacyIterationAliases` 把引用改写成「`CanvasState` 暴露的 `item` / `index` 别名」，所以这两个名字的语义由运行时定义，不是 DSL 定义。

对 AI / agent 工程最实用的一条是**「同一份配置的多个副本必须显式对齐」**。一个 agent 节点的名字在 DSL 里出现五次，而每次内部改名都要改五处。任何「可视化编辑器 + 后端执行器」的组合都会产生这个问题：前端持有渲染数据，后端持有执行数据，两边的字段名与语义都会漂。可行做法不是「试图只存一份」（那会打破某一方的契约），而是**在边界处写一个显式的对齐函数，并把「只允许在某个方向丢信息」写成注释与函数名**。

另一条是 `foldLegacyLoopVariants` 那个「找不到父节点就不删」的选择。在多阶段数据变换里，**失败方向的选择比失败处理本身更重要**：这里选「留下多余的东西」，因为多余的会被下游忽略，而悬空引用会让下游在对不存在的 id 上失败。判据是：**哪种残留更难被下游容忍？** 留多余的通常无害，留悬空的一定有害。

最后一条是 `ExtractPrologue` / `ExtractMode` 那种「三个 `_` 吞掉一切」的反面教材价值。它说明「可选字段的宽松解析」会静默地把**类型错误**也当成「没配」——而这个静默没有任何日志。正确做法是把「缺失」（正常）与「类型错」（异常）分开处理，哪怕最终都返回空值，也要在后者留一条日志。同包里 `ExtractComponentInputForm` 对这条的处理就是正确的示范，而注释里那句「CodeRabbit PR review #1」说明这个区分曾被人主动要求放弃、作者拒绝了。
