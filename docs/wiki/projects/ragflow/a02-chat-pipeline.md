---
project: ragflow
feature: chat-pipeline
title: 对话编排流水线
page_type: feature
schema_version: 1
refs:
  - path: internal/service/chat_pipeline.go
    symbols: [ChatPipelineService, AsyncChat, AsyncChatResult, ThinkEvent]
  - path: internal/service/chat_pipeline.go
    symbols: [TurnUsage, kbPrompt]
  - path: internal/service/chat_pipeline.go
    symbols: [messageFitIn]
---

# 对话编排流水线

## 一句话职责

`AsyncChat` 把「一次问答」拆成十一个阶段——入口校验、引擎分派、模型解析、SQL 直查尝试、查询改写、检索、知识拼装、token 预算裁剪、驱动 LLM、流式输出、答案装饰——并**用 `AsyncChatResult` 这一个结构承载从「思考片段」到「最终答案 + 引用 + 用量」的所有产出**，让同一套编排同时供给 OpenAI 兼容端点与站内对话端点。

## 机制是什么

这一页的核心问题不是「怎么调 LLM」，而是**如何把一个分辨率极高的过程塞进一个返回通道**。

一次回答的产出实际上有七类完全不同的东西：思考文本（`Reasoning`）、可见答案（`Answer`）、引用映射（`Reference`）、音频（`AudioBinary`）、提示词回显（`Prompt`）、结构化步骤（`ThinkEvent`）、token 账（`TurnUsage`）。而调用方是个 SSE 流或者一个非流式响应——**它只能收一种类型的消息**。

`AsyncChatResult` 就是这个「多路复用」的容器：所有字段都可选，`Final` 标记收尾。于是同一条 channel 里会流过：
- 若干只带 `Answer` 的增量（正文 delta）；
- 若干只带 `Reasoning`（或只带 `EndToThink`）的思考片段；
- 反馈里带 `ThinkEvent` 的工具步骤；
- 最后一条带 `Final: true` 的完整答案 + 引用 + `Usage` + `ElapsedMs`。

**这个设计的代价是「合法组合」没有类型表达**——`Answer: ""` + `Final: false` + `EndToThink: true` 是合法的一条（「思考段结束」），而 `Answer` 为空且 `Final` 为真的语义是「空回答」。两者靠字段组合区分，读代码时必须逐处推断。

第二个机制是**预算链**：模型的上下文窗口被切成三份——系统提示 + 引用规则、知识段、答案预留——而这三个数的计算分散在四个函数里（`completionReserve` / `knowledgeTokenBudget` / `messageFitIn` / `clampChatConfigMaxTokens`），彼此还要保持一致。这是这一页最需要「一起读」的部分。

## 怎么实现的

**`ChatPipelineService` 只有三个依赖，因为它的职责是编排而不是持有能力。**

```go
type ChatPipelineService struct {
    ModelFactory *ModelFactory
    MetadataSvc  *MetadataService
    kbDAO        *dao.KnowledgebaseDAO
}
```

注释说明了它的定位：「It owns the core retrieval → generation pipeline (AsyncChat, AsyncChatSolo) and all their supporting helpers. Callers (OpenAIChatService, ChatSessionService) compose it to avoid code duplication.」——**它是被组合的，不是被继承的**，两个端点共用它。

**`AsyncChat` 的签名本身就在宣告「这是个流式函数」。**

```go
func (s *ChatPipelineService) AsyncChat(
    ctx context.Context,
    chat *entity.Chat,
    messages []map[string]interface{},
    stream bool,
    kwargs map[string]interface{},
) (results <-chan AsyncChatResult, runErr error) {
```

**返回 `<-chan` 而不是 `chan` 是一个刻意的约束**：调用方只能读、不能写、也不能关。配合函数内部的 `defer close(out)`，通道的生命周期完全由生产者控制。而 `runErr` 只覆盖**同步阶段**的错误（入口校验、引擎分派），异步阶段的错误被转成 `Answer: "**ERROR**: ..."` 的结果流。

**这个「同步错误走返回值、异步错误走数据流」的分裂是必要的，但很容易被忽略**：调用方若只检查 `runErr == nil` 就以为成功，会漏掉后面流里的错误。看 `defer` 就明白作者知道这一点——成功路径才记日志：

```go
defer func() {
    if runErr == nil && results != nil {
        results = logChatCompletion(ctx, chat, messages, stream, results)
    }
}()
```

**它把 `results` 重新绑定成一个包装通道**（用于记完成日志），于是「读通道的人」与「记日志的人」共用同一条流。这是一个很 Go 的做法，但也意味着**日志钩子只在成功路径上挂载**——失败时什么都不记。

**三个阶段在 goroutine 之外，因为它们必须同步返回错误。**

```
Phase 1: Entry Validation           ← 同步，错误走 runErr
  messages 非空、最后一条是 user、数据集访问校验、合规校验
  合规失败 → 立刻构造一个只含错误文本的 channel 返回

引擎分派                              ← 同步，错误走 runErr
  level = resolveReasoningLevel(...)
  selectedEngine = selectAgenticEngine(level, hasKBs)
  engineCodex 需双重前置条件，否则降级
  engineAgenticRAG / engineCodex → 直接转交，不走后续阶段

!hasKBs && !useWebSearch → AsyncChatSolo（纯 LLM）
```

**合规校验那段的分支值得单独看，因为它是「错误也走数据流」的样板：**

```go
if err := compliance.ValidateQuestion(ctx, question); err != nil {
    if ctx.Err() != nil {
        out := make(chan AsyncChatResult)
        close(out)
        return out, nil
    }
    out := make(chan AsyncChatResult, 2)
    if stream {
        out <- AsyncChatResult{Answer: err.Error()}
    }
    out <- AsyncChatResult{Answer: err.Error(), Final: true}
    close(out)
    return out, nil
}
```

**两处细节：**

1. **`ctx.Err() != nil` 时返回一个立即可关闭的空通道**，不带任何结果。理由是：请求已被取消，往通道里塞东西只会让调用方写出一个本该被丢弃的回答。
2. **`make(chan AsyncChatResult, 2)` 的容量正好是「流式 2 条 / 非流式 1 条」**。这不是随手写的数字——它保证了这些 send **永不阻塞**，于是在「同步阶段就该返回」的路径上不需要 goroutine。缓冲容量在这里承担了「不需要并发」的证明。

**`selectAgenticEngine` 的四支判断顺序是刻意的，注释写了为什么。**

```go
func selectAgenticEngine(level int, hasKBs bool) agenticEngine {
    if level == reasoningLevelCodex {
        return engineCodex
    }
    if !hasKBs {
        return ""
    }
    if level == reasoningLevelAgentic {
        return engineAgenticRAG
    }
    // 0 (off) and the harness depths 1..4 stay on the pipeline.
    return ""
}
```

注释解释了「mode 8 为何故意绕过 `!hasKBs` 这道闸」：Codex 是通用助手，可以无知识库对话，而它的 MCP 检索工具在无 scope 的调用上会返回错误（而不是空 scope 查询）——所以 level 8 必须**在 `!hasKBs` 返回之前**被处理。

**「先判 Codex，再判 hasKBs」这个顺序与「先判 hasKBs，再判 Codex」在行为上不同**，而差异只体现在「无知识库 + level 8」这一个组合上。这是一处「顺序即语义」的实现。

**而 Codex 还有两道降级闸门，都写在注释里：**

```go
if selectedEngine == engineCodex {
    if !codexConfigured() {
        common.WarnCtx(ctx, "codex (mode 8) requested but no codex.endpoint/mcp_public_base is configured; using the regular pipeline", ...)
        selectedEngine = ""
    } else if !s.codexResponsesCapable(ctx, userID, chat) {
        common.WarnCtx(ctx, "codex (mode 8) requires a Responses-API dialog model; using the regular pipeline", ...)
        selectedEngine = ""
    }
}
```

注释解释了降级的正当性：「the honest reading of 'the engine it asked for is not available here', mirroring how a level-5 turn with no KBs drops to the regular branch」——**降级而不是报错**，且明确打 warn 日志。这是一个「引擎不可用时的正确降级形状」：不泄漏未配置的引擎、不让用户看到内部错误、但要留下可定位的日志。

**`level` 的来源是「请求优先、配置兜底」，两个字段名同形。**

```go
func resolveReasoningLevel(kwargs map[string]interface{}, promptConfig map[string]interface{}) int {
    if kwargs != nil {
        if v, ok := kwargs["reasoning"]; ok {
            if n, ok2 := asInt64(v); ok2 { return int(n) }
        }
    }
    if promptConfig != nil {
        if v, ok := promptConfig["reasoning"]; ok {
            if n, ok2 := asInt64(v); ok2 { return int(n) }
        }
    }
    return 0
}
```

注释说明了值域：「an integer 0..4 (0 = off, 1..4 = low/medium/high/ultra), reasoningLevelAgentic for the smart-reasoning agent, or reasoningLevelCodex for the Codex engine」。以及为什么是数字而不是布尔：「Frontend sends `Number(getThinkingLevel())`, so the raw value is numeric, not a bool」。

**`kwargs` 与 `promptConfig` 都是无类型的 `map[string]interface{}`，于是「同一份配置」在两个地方用同名键读两遍。** 三级查找（请求 → 配置 → 默认）在这套代码里是通用模式，`resolveReferenceMetadata` 里更是有三层显式编号：

```go
// Layer 1: prompt_config["reference_metadata"] (config).
// Layer 2: kwargs["reference_metadata"] (request, takes precedence).
// Layer 3: legacy request keys (kwargs).
```

**注意「Layer 2 覆盖 Layer 1」靠的是遍历 `resolved[k] = v` 逐键覆盖，而不是整块替换** ——于是「请求只给了一个子键」时，配置里的其它子键仍保留。这是「合并」而不是「替换」，语义更细，但也意味着**调用方无法用请求「清空」某个配置项**（只能设成零值）。

**`AsyncChat` 的异步部分在一个 goroutine 里跑完整条流水线，输出通道容量 16。**

```go
out := make(chan AsyncChatResult, 16)
go func() {
    defer close(out)
    timer := common.NewTimer()
    timer.Start()
    sendResult := func(res AsyncChatResult) bool {
        if ctx.Err() != nil { return false }
        select {
        case out <- res: return true
        case <-ctx.Done(): return false
        }
    }
```

**`sendResult` 的三重保护值得逐条看：**

1. **先查 `ctx.Err()`** ——已取消就立刻返回 false，省掉一次 select。
2. **`select` 里同时等「通道可写」与「ctx 完成」** ——这是 Go 里「向可能无人读的通道发送且要尊重取消」的标准写法。没有它，调用方提前退出会让生产者永久阻塞在 `out <- res` 上（通道满了之后），goroutine 泄漏。
3. **返回 bool 让调用方 `return`** ——所有调用点都是 `if !send(...) { return }`。**这是一个把「取消传播」做成「函数返回值」的显式设计**，每个阶段都必须检查，否则取消不会生效。

**代价是：** 每次发结果都要写三行 `if !send(...) { return }`，全文出现了几十次。这是 channel + context 组合的必然开销，而它的好处是**取消可以在任意一次发送时生效，不需要额外的 watcher**。

**`kbPrompt` 是知识拼装的唯一入口，它的三处细节都是「引用可点」。

```go
func (s *ChatPipelineService) kbPrompt(kbinfos map[string]interface{}, maxTokens int) []string {
    chunksRaw, ok := kbinfos["chunks"].([]map[string]interface{})
    if !ok || len(chunksRaw) == 0 || maxTokens <= 0 { return nil }

    var result []string
    usedTokenCount := 0
    for i, ck := range chunksRaw {
        c := getMapString(ck, "content_with_weight", "content")
        if c == "" { continue }

        // The id is the chunk's pool position, which is also its index in the
        // reference the client receives (chunksFormat(chunksRaw)), so a marker copied
        // from a block opens exactly that passage. 0-based: the first block is "ID: 0".
        cnt := fmt.Sprintf("\nID: %d", i)
        cnt += drawNode("Title", getMapString(ck, "docnm_kwd", "document_name"))
        cnt += drawNode("URL", getMapString(ck, "url"))
        if meta, ok := ck["document_metadata"].(map[string]interface{}); ok {
            keys := make([]string, 0, len(meta))
            for k := range meta { keys = append(keys, k) }
            sort.Strings(keys)
            for _, k := range keys {
                cnt += drawNode(k, meta[k])
            }
        }
        cnt += "\n└── Content:\n"
        cnt += c
        result = append(result, cnt)

        usedTokenCount += graph.NumTokensFromString(cnt)
        if float64(maxTokens)*0.97 < float64(usedTokenCount) {
            common.Warn("Not all the retrieval into prompt", zap.Int("kept", len(result)), zap.Int("total", len(chunksRaw)))
            break
        }
    }
    return result
}
```

**四处设计点：**

**第一，`ID: i` 是「池位置」而不是 chunk 的真实 id。** 注释把这条契约讲得很清楚：「The id is the chunk's pool position, which is also its index in the reference the client receives (`chunksFormat(chunksRaw)`)，so a marker copied from a block opens exactly that passage」。

**这意味着模型从知识块里抄一个 `ID: 3` 出来，客户端拿这个 3 就能定位到 `Reference` 里的第 4 条。** 这个约定把「引用」变成一个整数索引，代价是**它依赖 `kbPrompt` 与 `chunksFormat` 使用同一份 `chunksRaw` 且索引一致**——两处任何一方改了顺序（比如重排、过滤空内容），编号就会错位。

而 `kbPrompt` 里有一个**破坏这个约定的地方**：

```go
if c == "" { continue }
```

**内容为空的 chunk 被跳过，但 `i` 是 `range` 的下标，所以后续块的 `ID` 仍然是原始下标。** 这实际上是对的（因为 `chunksFormat` 也按原始下标生成），但如果 `chunksFormat` 过滤了空块，索引就不一致了。**这条契约只在两处行为完全一致时成立，而代码里没有任何断言或注释保证这一点。**

**第二，元数据键被 `sort.Strings` 排序。** 理由与 LLM 无关——**排序让同一份输入产出 byte 相同的提示词**，这对缓存（`cache_key`）与可复现性是必需的。若按键的哈希顺序遍历，同样的知识块会生成不同的提示词，缓存命中率随机波动。

**第三，元数据被渲染成与 Title/URL 同级的节点。** `document_metadata` 里任意键都进提示词，没有白名单。这意味着**元数据字段的数量直接影响知识段体积**，而 `knowledgeTokenBudget` 里算的是「系统提示 + 引用规则 + 预留」，**没有把元数据的开销算进去**——它只能靠这里的 0.97 阈值兜底（保住 97% 预算，多的截断）。

**第四，`float64(maxTokens)*0.97 < float64(usedTokenCount)` 是一个「留 3% 余量」的软阈值。**

**为什么是 0.97 而不是 1.0？** 因为 token 计数与实际渲染后的提示词会有误差（`NumTokensFromString` 是估算或分词器计数），留 3% 吸收这部分偏差。而 `break` 之后**已经 append 的块保留**，所以实际保留可能略超阈值（最后一块整块计入）。**这是一个「宁可略超也不截断半块」的取舍**——半块内容会让引用指向一个不完整的段落。

它的后果是：**`usedTokenCount` 返回给下游的数是「实际用掉的」，而预算检查用的是 0.97 倍。** 于是「预算 1000」实际可能只装了 970 左右。

**`messageFitIn` 是最后一层裁剪，而它的三档策略有一个明确的优先级。**

```go
func (s *ChatPipelineService) messageFitIn(messages []map[string]interface{}, maxTokens int) (int, []map[string]interface{}) {
    if maxTokens <= 0 { maxTokens = 8192 }

    // Step 1: everything fits.
    totalTokens := s.countAllTokens(messages)
    if totalTokens < maxTokens { return totalTokens, messages }

    // Step 2: keep all system messages + the last message.
    result := make([]map[string]interface{}, 0)
    for _, m := range messages {
        if role, _ := m["role"].(string); role == "system" { result = append(result, m) }
    }
    if len(messages) > 1 { result = append(result, messages[len(messages)-1]) }

    totalTokens = s.countAllTokens(result)
    if totalTokens < maxTokens { return totalTokens, result }

    // Step 3: trim content to fit.
    ll := graph.NumTokensFromString(s.stringContent(result[0]))
    ll2 := graph.NumTokensFromString(s.stringContent(result[len(result)-1]))
    total := ll + ll2
    if total <= 0 { return 0, result }

    if len(result) == 1 {
        result[0]["content"] = graph.TrimContentToTokenLimit(s.stringContent(result[0]), maxTokens)
        return s.countAllTokens(result), result
    }

    if float64(ll)/float64(total) > 0.8 {
        preservedLast := min(ll2, maxTokens)
        result[len(result)-1]["content"] = graph.TrimContentToTokenLimit(s.stringContent(result[len(result)-1]), preservedLast)
        remaining := max(0, maxTokens-preservedLast)
        result[0]["content"] = graph.TrimContentToTokenLimit(s.stringContent(result[0]), remaining)
    } else {
        preservedSystem := min(ll, maxTokens)
        result[0]["content"] = graph.TrimContentToTokenLimit(s.stringContent(result[0]), preservedSystem)
        remaining := max(0, maxTokens-preservedSystem)
        result[len(result)-1]["content"] = graph.TrimContentToTokenLimit(s.stringContent(result[len(result)-1]), remaining)
    }

    return s.countAllTokens(result), result
}
```

**三档分别是「全留」「只留系统 + 最后一条」「按比例裁剪两条」**，而第三档的 `> 0.8` 判据是「系统占大头就把余量先给最后一条」，否则「先保系统」。

**这里有四处值得记的问题与设计：**

**第一，Step 2 会丢掉中间的对话历史。** 它只保留 system + **最后一条**。于是一次超出预算的请求会**静默地把多轮历史全删掉**——用户看到的是「模型忘了我刚才说的话」，而没有任何提示。

**第二，`if len(messages) > 1` 这个条件很微妙。** 若 `messages` 只有一条（比如只有 user，没有 system），那么 `result` 在 Step 2 之后是**空列表**（没有 system 可加，且 `len(messages) > 1` 为假）。接着 Step 3 的 `result[0]` 就会**索引越界 panic**。

**走一遍确认：** `messages` 长度 1、内容超预算 → Step 1 返回（因为 `totalTokens < maxTokens` 才返回，超预算则继续）→ Step 2：`role != "system"` 所以不 append；`len(messages) > 1` 为假所以也不 append → `result` 为空 → `countAllTokens([])` 为 0 → `0 < maxTokens` 成立 → **返回 `(0, [])`**。

**所以不会 panic，但会返回「空消息列表 + 0 token」**。对只有一条超长 user 消息的场景，结果是**提示词整个消失**——模型会收到一个没有内容的请求。这是一个真实的边界缺陷，而它被 Step 2 之后的「再次检查是否装得下」意外地保护住（不会走到 Step 3）。

**第三，`> 0.8` 的判据用的是「系统 token / 两条总和」，而不是「系统 token / maxTokens」。** 于是判据的含义是「两条里系统占了八成以上」，与「预算里系统占了八成」不同。前者在「两条都很小但没超」的情况下无意义（但那种情况 Step 1 就返回了）。

**第四，`TrimContentToTokenLimit` 是唯一的实际裁剪手段，它作用在单条消息的 `content` 上。** 这意味着**消息数量不变、只改内容长度**——系统提示被截断后可能丢掉关键指令（比如「不要说谎」这类放在末尾的约束），而代码没有任何「保留头尾、裁中间」的机制。

**`completionReserve` 与 `knowledgeTokenBudget` 是预算链的上游。**

```go
func completionReserve(cfg *modelModule.ChatConfig, modelMaxOutput, modelMaxTokens int) int {
    if cfg != nil && cfg.MaxTokens != nil && *cfg.MaxTokens > 0 {
        return *cfg.MaxTokens
    }
    reserve := defaultCompletionReserve
    if modelMaxOutput > 0 { reserve = modelMaxOutput }
    return min(reserve, modelMaxTokens/4)
}
```

注释说明了三层回退：「the chat's max_tokens when set, honoured exactly; otherwise the model's catalog output limit, else `defaultCompletionReserve`, both capped at a quarter of the window so a large catalog limit cannot take it all」。

**「capped at a quarter」是关键保护**：模型的目录输出上限可能是 16K，而窗口只有 8K——若直接用它作预留，知识预算就变负数了。**四分之一这个比例把「输出预留」限制在一个不会挤死输入的范围。**

```go
func (s *ChatPipelineService) knowledgeTokenBudget(modelMaxTokens int, systemTemplate string, kwargs map[string]interface{}, quote bool, reserve int) int {
    params := maps.Clone(kwargs)
    delete(params, "knowledge")
    used := graph.NumTokensFromString(s.formatPrompt(systemTemplate, params)) + reserve
    if quote {
        used += graph.NumTokensFromString(ragprompts.CitationPrompt(""))
    }
    insertions := max(strings.Count(systemTemplate, "{knowledge}"), 1)
    return max(modelMaxTokens-used, 0) / insertions
}
```

**四处精确之处：**

1. **`maps.Clone` + `delete(params, "knowledge")`** ——量系统提示时**故意把知识段排除**，否则就是「用包含知识的长度去算知识还能占多少」的循环。注释写明：「The system prompt is measured as formatPrompt renders it from kwargs, with the knowledge left out, so a prompt parameter carrying a large value is counted」。**这次删除只是为了度量，不影响真实渲染** —— 这是一个很干净的度量手法。

2. **`quote` 为真时把引用规则的 token 也算进已用** ——因为那些规则会追加在系统提示之后。这里传的是 `CitationPrompt("")`（空知识），即只算规则本身的长度。

3. **`insertions := max(strings.Count(systemTemplate, "{knowledge}"), 1)`** ——**预算要按插入点数量平分**。注释解释了两种情形：「formatPrompt writes the passages at every `{knowledge}` of the template, and AsyncChat appends them once to a template without one」——即模板里可能有多处 `{knowledge}`（各插一份），也可能一处都没有（则在末尾追加一份）。`max(..., 1)` 让「一处都没有」时按一份算。

4. **`max(modelMaxTokens-used, 0)` 保证不为负**，再除以插入点数。

**这套算法有一个隐含假设值得指出：它把「知识段会被插入 N 处、每处 N 分之一」当成预算分配方式，但 `kbPrompt` 返回的是一整份块列表，调用方把整份插到每一处还是只插一处，决定了实际消耗是「N 倍」还是「1 份」。** 若模板里有两处 `{knowledge}` 而调用方在每处都插整份，实际消耗是预算的 2 倍——这条约束在代码里看不到，只能靠模板作者守规矩。

**`clampChatConfigMaxTokens` 是预算链的最后一环，它可能直接报错。**

```go
func clampChatConfigMaxTokens(cfg *modelModule.ChatConfig, modelMaxTokens, usedTokenCount int) (int, bool, error) {
    if usedTokenCount >= modelMaxTokens {
        return 0, false, fmt.Errorf("prompt uses %d tokens, leaving no completion capacity for model max context length %d", usedTokenCount, modelMaxTokens)
    }
    if cfg == nil || cfg.MaxTokens == nil { return 0, false, nil }
    adjusted := *cfg.MaxTokens
    if adjusted <= 0 { cfg.MaxTokens = nil; return 0, false, nil }
    remainingTokens := modelMaxTokens - usedTokenCount
    if adjusted > remainingTokens { adjusted = remainingTokens }
    cfg.MaxTokens = &adjusted
    return adjusted, true, nil
}
```

**三处：**

1. **`usedTokenCount >= modelMaxTokens` 直接报错** ——提示词把窗口占满时，没有「截断」这条路，只能失败。错误消息说明了原因（「leaving no completion capacity」）。
2. **它**修改**传入的 `cfg`**（`cfg.MaxTokens = &adjusted`）——一个副作用函数，而名字里的 `clamp` 掩盖了这个副作用。
3. **`adjusted <= 0` 时把 `MaxTokens` 设为 `nil`**，而不是 0。因为 nil 表示「用模型默认」，0 在多数驱动里是非法值。

**这三个数（reserve / knowledgeBudget / usedTokenCount）的关系是这一页的核心不变量：**

```
modelMaxTokens  =  reserve（答案）+  knowledge（知识段）+  system+prompt（系统提示与用户提示）
usedTokenCount  =  system + knowledge 实际占用
clamp          校验 usedTokenCount < modelMaxTokens，否则失败
```

而 `messageFitIn` 是**第四道**独立裁剪，它拿的 `maxTokens` 是另一个数（注释里说 95% 预算）。**这四道预算机制分散在四个函数、被四个不同阶段调用，没有一个统一的地方列出它们的关系。** 这是这一页最值得指出的结构问题。

**`TurnUsage` 是 token 账的聚合，而它的注释解释了「为什么这个账必须由流水线算」。**

```go
type TurnUsage struct {
    PromptTokens     int `json:"prompt_tokens"`
    CompletionTokens int `json:"completion_tokens"`
    TotalTokens      int `json:"total_tokens"`
    LLMCalls         int `json:"llm_calls"`
    // LLMTurns is the per-call split the aggregate above sums up: one entry per
    // LLM call (ReAct iteration, repair turn, audit pass, synthesis), with its
    // input/output tokens, the model that served it and when it started. A
    // benchmark reads it to see WHERE a question's cost went, not just how much
    // it was. Omitted by backends that do not report it.
    LLMTurns []tokenizer.CallUsage `json:"llm_turns,omitempty"`
}
```

注释写清了必要性：「only the pipeline sees every LLM call the ReAct loop and its delivery gate made — the response's own token counts cover the final answer alone」。**响应体的 usage 只统计最后一次调用，而一次 agentic 轮次可能有十几次调用**——所以账必须在编排层算。

**`AsyncChatResult` 里那一组「文档级计数」字段，是这套代码最独特的部分。**

```go
// RetrievedDocIDs is every document identifier (doc_id and doc_name) the
// turn's retrieval tools RETRIEVED - opened with full chunk content
// (list_chunks, search_chunks, search_semantic_chunks), deduplicated and
// sorted. A document seen only through a <match_snippet> window
// (search_bm25_chunks, grep_chunks) was LOCATED, not retrieved, and does
// NOT appear here: the two sets answer different questions, and the
// benchmark's Recall (%) scores against this one.
RetrievedDocIDs []string `json:"retrieved_docids,omitempty"`
// ServedDocIDs is every document identifier the turn's retrieval tools
// SURFACED: the full-content tools above PLUS the snippet-only locators
// (search_bm25_chunks, grep_chunks), ranked most-served first.
ServedDocIDs []string `json:"served_docids,omitempty"`
```

**「检索到」与「被呈现」是两个不同的集合**，注释用实测数据论证了这个区分为什么必要：

> Measured 2026-10-05: q1093 of a ten-question run reported 6 ids here while its retrieval had served 270 documents - reading this field as "what the agent saw" understates the context by an order of magnitude.

**「把 `RetrievedDocIDs` 读成『agent 看到的东西』会低估上下文一个数量级」** ——这条注释的价值在于它给出的是**实测数字**而不是判断。这类「两个近义指标必须分开、否则会被误读」的字段设计，是做评测系统时最容易漏的一层。

配套的还有一组：

```go
// DeepReadChunks / ShallowReadChunks count the chunks the turn's
// retrieval tools put in front of the model, split by read depth: deep =
// full chunk content (list_chunks, search_chunks), shallow =
// <match_snippet> windows (grep_chunks, search_bm25_chunks).
DeepReadChunks    int `json:"deep_read_chunks,omitempty"`
ShallowReadChunks int `json:"shallow_read_chunks,omitempty"`
```

注释给出理由：「They show where a run's context weight came from — document reading vs triage traffic — which raw tool call counts cannot distinguish」。

**`AnswerIsAuthoritative` 是一个不做序列化的内部控制位。**

```go
// Internal-only: set when Answer is the run's authoritative full text —
// <think> stretch included, citation handles already compacted against the
// shipped reference — so the session writer must persist it instead of the
// accumulated stream deltas, which still carry the pre-compaction handles.
AnswerIsAuthoritative bool
```

**它没有 JSON tag**（对比同结构里其它字段都带）。注释说明了为什么需要它：**流式累积的文本里还带着「压缩前的引用句柄」，而 `Answer` 是压缩后的权威版本**——会话写入者必须存后者。若没有这个标记，写入者只能猜。

**这是一处「同一个字段在不同模式下含义不同，用一个私有布尔表达」的设计**：`Answer` 在增量块里是「这一片」，在最终块里是「全文」，而「全文」这个身份无法从内容判断，只能靠标记。

**`ThinkEvent` 的注释记录了「类型重复定义」造成过的真实事故。**

```go
// ThinkEvent is one structured agentic reasoning step as it goes over
// the wire (the SSE `think_event` field). This is the ONE definition, so a
// producer that needs structured steps aliases this type instead of
// carrying a second copy bridged field by field — that older shape compiled
// for as long as one side stayed a subset, and a field added on the
// producer side then reached the client as nothing at all.
//
// The JSON tags below ARE the client contract; TestThinkEventWireKeys pins them.
type ThinkEvent struct {
    Kind string `json:"kind"`
    Stage string `json:"stage,omitempty"`
    Tool string `json:"tool,omitempty"`
    Args string `json:"args,omitempty"`
    Status string `json:"status,omitempty"`
    Reason string `json:"reason,omitempty"`
    Cause string `json:"cause,omitempty"`
    Results int `json:"results,omitempty"`
    Documents int `json:"documents,omitempty"`
    Sources []string `json:"sources,omitempty"`
    DurationMS int64 `json:"duration_ms,omitempty"`
    Summary string `json:"summary"`
}
```

**「两份结构逐字段桥接」这个旧形态的问题被讲得很准：只要生产方是消费方的子集，编译就能通过；而当生产方新增一个字段时，它到达客户端后变成「什么都没有」。** 这是一个**类型系统无法发现的契约破坏**——两侧各有一份定义，编译器只检查每份各自的正确性。

解法是把定义收到一处，并**用测试钉住 JSON 键名**（`TestThinkEventWireKeys`）。这是一条很实用的做法：**线上契约靠测试锁，不靠注释**。

`Summary` 是唯一没有 `omitempty` 的可选字段（`json:"summary"`），说明它总是会被填充——它是「思考块里那句人话」，而其它字段都是按需的。

**流式输出里的「思考段」处理暴露了一个真实的两难。**

```go
if thinkState.ExitReasoning() {
    for _, d := range FlushRemaining(thinkState) {
        if d.Kind == ThinkDeltaText && d.Value != "" { ...发送... }
    }
    if !send(AsyncChatResult{Answer: "", EndToThink: true}) { return ctx.Err() }
}
```

**`EndToThink` 是一个「零内容信号」** ——`Answer: ""` 且 `Final: false`，唯一作用是告诉客户端「思考段结束了，接下来的内容是正文」。它必须存在，因为**正文增量和思考增量在同一条通道上，客户端需要知道何时切换渲染位置**。

注释还说明了一处与 Python 的差异：

```go
// Flush remaining state matching Python's final flush order
// (think_buffer → marker → answer_buffer → pending_after_close)
// Python has no Reasoning field — all text is Answer.
```

**Go 版把 `Reasoning` 独立成字段，而 Python 把所有文本塞进 `Answer` 并用 `<think>` 标签界定。** `AsyncChatResult.Reasoning` 的注释解释了为什么不再需要标签：

> Mirrors Python's _async_chat_streamly, which wraps reasoning_content in `<think>`…</think>` markers so _stream_with_think_delta can route it via in_think state. In Go the driver already separates the two streams, so we surface them as separate fields directly instead of merging-then-re-splitting.

**「先合并再用标签重新切开」被换成了「直接分两个字段」** ——这是一个明确的行为改进，代价是**两条流的时序关系必须由流水线自己维护**（`StartToThink` / `EndToThink` 就是为此而生的信号）。

## 关键取舍

**源码事实：** `ChatPipelineService` 仅持三个依赖（`ModelFactory` / `MetadataService` / `kbDAO`），被 `OpenAIChatService` 与 `ChatSessionService` 组合而非继承；`AsyncChat` 返回 `<-chan AsyncChatResult`（只读单向通道），同步阶段错误走 `runErr`，异步阶段错误被转成 `Answer: "**ERROR**: ..."` 的结果流。

**源码事实：** 合规校验失败时返回一个**容量恰为 2** 的通道（流式塞 2 条、非流式塞 1 条）并立刻 close，因此不需要 goroutine；若 `ctx.Err() != nil` 则返回空通道不塞任何结果。

**源码事实：** `selectAgenticEngine` 的判断顺序是「先 Codex → 再 `!hasKBs` → 再 agentic」，因为 Codex 可以无知识库对话，而它的 MCP 检索工具在无 scope 调用上会返回错误（而非空 scope 查询）；`engineCodex` 另有两道降级闸（未配置 `codex.endpoint`、模型不支持 Responses API），降级时打 warn 并退回常规流水线。

**源码事实：** `resolveReasoningLevel` 从 `kwargs["reasoning"]` 优先、`promptConfig["reasoning"]` 兜底，值是整数（0–4 为深度、`reasoningLevelAgentic`、`reasoningLevelCodex`），因为前端发的是 `Number(getThinkingLevel())`。

**源码事实：** `sendResult` 每次发送前先查 `ctx.Err()`，再用 `select` 同时等「通道可写」与 `ctx.Done()`，返回 bool 让调用点 `return`；输出通道容量 16。日志钩子 `logChatCompletion` 只在 `runErr == nil && results != nil` 时挂载（失败路径不记完成日志）。

**源码事实：** `kbPrompt` 用 `"\nID: %d"` 以下标 `i` 作为引用编号，注释说明它与 `chunksFormat(chunksRaw)` 的索引必须一致，从而「从知识块里抄出来的标记能精确打开对应段落」；元数据键经 `sort.Strings` 排序以保证同输入同提示词；预算用 `float64(maxTokens)*0.97` 的软阈值，超限 `break` 但保留已入选块（宁可略超也不截断半块）。

**源码事实：** `kbPrompt` 跳过 `content` 为空的 chunk 但仍使用原始下标 `i` 作为 ID；这条「两处索引一致」的契约在代码里没有断言或注释保证。

**源码事实：** `messageFitIn` 三档：全留 → 只留 system + 最后一条 → 按比例裁剪两条（`ll/total > 0.8` 时优先保最后一条，否则优先保 system）；Step 2 会静默丢弃全部中间历史。当 `messages` 只有一条且超预算时，Step 2 产出空列表，Step 3 前的二次检查会返回 `(0, [])`——提示词整个消失。

**源码事实：** `completionReserve` 三层回退（`cfg.MaxTokens` → 模型目录输出上限 → 默认值），且**上限锁在 `modelMaxTokens/4`**，避免目录上限吃掉整个窗口。

**源码事实：** `knowledgeTokenBudget` 用 `maps.Clone` + `delete(params, "knowledge")` 度量不含知识段的系统提示（只为度量，不影响真实渲染）；`quote` 时把 `CitationPrompt("")` 的长度也计入已用；按 `max(strings.Count(systemTemplate, "{knowledge}"), 1)` 对剩余预算做平分。

**源码事实：** `clampChatConfigMaxTokens` 在 `usedTokenCount >= modelMaxTokens` 时直接报错（消息说明「no completion capacity」），否则把 `cfg.MaxTokens` 截到剩余量；`adjusted <= 0` 时设为 `nil`（表示用模型默认）而不是 0。该函数有副作用（改写传入的 `cfg`）。

**源码事实：** `TurnUsage.LLMTurns` 按调用拆分（ReAct 迭代 / 修复轮 / 审计 / 合成），因为「响应体自带 usage 只覆盖最终答案」，而 agentic 轮次有十几次调用。

**源码事实：** `AsyncChatResult` 区分 `RetrievedDocIDs`（被完整内容打开的文档）与 `ServedDocIDs`（含 `<match_snippet>` 定位的文档），注释给出实测数字（q1093 报 6 个而实际服务 270 个文档）论证「把前者读成『agent 看到的』会低估一个数量级」；另有 `DeepReadChunks` / `ShallowReadChunks` 按读取深度拆分。

**源码事实：** `AnswerIsAuthoritative` 无 JSON tag（内部字段），因为流式累积文本仍带「压缩前的引用句柄」，而最终 `Answer` 是压缩后的权威版本，会话写入者必须据此选择。

**源码事实：** `ThinkEvent` 的注释记录了「生产方与消费方各自持有一份结构、逐字段桥接」的旧形态事故：只要一侧是另一侧的子集编译就能通过，而新增字段到客户端时变成「什么都没有」；解法是把定义收到一处并用 `TestThinkEventWireKeys` 钉住 JSON 键名。

**源码事实：** Go 版把 `Reasoning` 独立成字段而不再用 `<think>` 标签包裹（Python 是「先合并再按标签切开」），因此需要 `StartToThink` / `EndToThink` 两个信号维护两条流的时序。

**架构推论：** 用「一个可选字段齐全的容器 + 一个 `Final` 标志」表达多路复用的流，是 SSE 场景里最常见也最难维护的形状——**合法组合没有类型表达**，客户端必须按字段组合推断意图，服务端每加一种产出就多一组隐式规则。更安全的形式是给容器一个 `Kind` 判别字段（像 `ThinkDelta` 已经做的那样），让「这一条是什么」变成显式数据。`EndToThink` 以「空 Answer + 只置标志」表达，正是缺少判别字段的症状。

**架构推论：** token 预算被拆成四个独立函数、由四个阶段分别调用，而这四个数之间存在一个必须成立的不等式（`reserve + knowledge + prompt ≤ window`）。**没有任何地方把这组关系写下来**。这类「多个局部正确的估算器共同保护一个全局不变量」的结构，是最容易在参数调整时失衡的——因为每处改动单独看都合理。

**架构推论：** 「请求优先、配置兜底」的三级查找（`resolveReasoningLevel`、`resolveReferenceMetadata`）在这套代码里反复出现，而它靠 `map[string]interface{}` 逐键合并没有类型约束。好处是新增一个覆盖项不需要改任何类型；代价是**一个键名拼错会静默地不生效**，且「请求能否清空配置项」这个语义从未被定义（逐键合并永远只能覆盖、不能删除）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/service/chat_pipeline.go` | `ChatPipelineService` | 编排服务（持 `ModelFactory` / `MetadataService` / `kbDAO`，被两个端点组合） |
| `internal/service/chat_pipeline.go` | `AsyncChat` | 十一个阶段的完整编排；返回只读结果通道 |
| `internal/service/chat_pipeline.go` | `AsyncChatSolo` | 无知识库且无联网时的纯 LLM 快路径 |
| `internal/service/chat_pipeline.go` | `AsyncChatResult` | 多路复用容器（答案 / 思考 / 引用 / 音频 / 步骤 / 用量 / 文档计数） |
| `internal/service/chat_pipeline.go` | `ThinkEvent` | 结构化思考步骤的唯一定义（JSON 键名被测试钉住） |
| `internal/service/chat_pipeline.go` | `TurnUsage` | 一轮所有 LLM 调用的 token 聚合与逐次拆分 |
| `internal/service/chat_pipeline.go` | `kbPrompt` | 知识块拼装（`ID: i` 引用编号、元数据排序、0.97 软阈值） |
| `internal/service/chat_pipeline.go` | `messageFitIn` | 最后一道裁剪（三档 + `> 0.8` 优先级判据） |
| `internal/service/chat_pipeline.go` | `completionReserve` | 答案预留（三层回退，封顶窗口四分之一） |
| `internal/service/chat_pipeline.go` | `knowledgeTokenBudget` | 知识预算（排除知识段度量、计入引用规则、按插入点平分） |
| `internal/service/chat_pipeline.go` | `clampChatConfigMaxTokens` | 预算终检（占满则报错，否则截 `MaxTokens`） |
| `internal/service/chat_pipeline.go` | `selectAgenticEngine` / `resolveReasoningLevel` | 引擎分派与推理等级解析（顺序即语义） |
| `internal/service/chat_pipeline.go` | `resolveReferenceMetadata` | 三层引用元数据合并（配置 → 请求 → 遗留键） |
| `internal/service/chat_pipeline.go` | `formatPrompt` | `{key}` 占位符替换，残留 `{knowledge}` / `{quote}` 替换为空格 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：internal/service/chat_pipeline.go](https://github.com/infiniflow/ragflow/blob/main/internal/service/chat_pipeline.go)
- [Server-Sent Events 规范](https://html.spec.whatwg.org/multipage/server-sent-events.html)
- [OpenAI Chat Completions API](https://platform.openai.com/docs/api-reference/chat)

## 与主核 / AI 的关系

对话编排层与主核（多模态数据库内核）的交集在**「把多路结果复用进单条通道」与「预算分配」**这两件事上，而后者是两者共同的难题。四条经验。

**第一，多路复用的流必须有判别字段，不能靠「哪些字段非空」推断类型。** 这里 `EndToThink` 用「空 Answer + 只置标志」表达一种控制语义，而 `AnswerIsAuthoritative` 用私有布尔表达「这条 Answer 是全文还是增量」——两者都是「字段组合承载类型」的症状。主核里做「把多种事件复用进一条日志流/变更流」时同理：**每条记录应当有一个显式的 kind，客户端按 kind 分派**。否则每加一种事件就多一组隐式规则，而错判的后果通常是「静默地少渲染一段」。

**第二，预算分配一旦拆到多处，就必须有一处写清不等式。** 这里有四个函数各管一段预算，它们共同保护 `reserve + knowledge + prompt ≤ window`，却没有一处声明这个约束。主核里做内存预算（缓冲池 + 排序区 + 临时表）时会遇到完全相同的结构：**每个配额单独看都合理，加起来超了却只在最坏情况下暴露**。加固办法是把全局预算做成一个显式的「分配器」，让每一段从同一个额度和同一种记账方式里扣。

**第三，实测数字写进注释，比判断性论断有效得多。** `RetrievedDocIDs` 那段注释用「q1093 报 6 个而实际服务 270 个文档」把「两个近义指标读混会低估一个数量级」这件事变得可验证。主核里凡是「两个看起来相似但语义不同的指标」（命中率 vs 命中数、逻辑读 vs 物理读、扫描行数 vs 返回行数），都值得写下这种量级对比——**因为它能让下一个人在误读之前停下来。**

**第四，线上契约靠测试钉，不靠两份定义并存。** `ThinkEvent` 的注释记录了「两侧各一份结构、逐字段桥接」的事故形态：编译器只检查每份各自正确，一侧是另一侧子集时一切通过，而新增字段则静默丢失。主核里做「服务端结构体 ↔ 客户端契约」的映射时同理：**定义只能有一处，键名用测试锁住**。多一份副本的收益是「各自演进」，代价是两者永远在不知不觉中脱钩。
