---
project: ragflow
feature: agentic-run
title: Agentic 检索主循环
page_type: feature
schema_version: 1
refs:
  - path: internal/agentic_rag/agent.go
    symbols: [Run, NewDocIDLedger, NewServedLedger, NewChunkReadLedger]
---

# Agentic 检索主循环

## 一句话职责

让模型自己决定查什么：把检索工具交出去，跑一个最多 120 轮的 ReAct 循环，而全程用**三类台账**记账——哪些文档被打开过、哪些只是被看到过、多少块被读进上下文——因为「模型自己找证据」这类系统的失效形态和纯向量检索完全不同：它通常不是「检索不到」，而是「找到了却没读」或者「读了同一篇 35 次」。

## 机制是什么

一段对话里的一次 `Run` 只回答**一轮**用户输入。这一点在 `Input.Messages` 的字段注释里被强调：

> `Messages` 是对话历史加上当前用户消息——**一次 `Run` 一轮**。下一个用户输入是**下一个 `Run`**（`AsyncChat` 每个请求被重新进入），所以调用方传进来的历史**必须已经是压缩过的**：每轮一个最终答案，绝不是产生它的那个 ReAct 轨迹。

所以「轨迹留在会话里」是错的——下一轮会把整个 ReAct 过程当成历史重新计费。这条约束被写进了字段注释而不是只在某处代码里实现，说明它是**调用方义务**而不是内部实现细节。

而 `Run` 的一轮由四段组成，顺序本身就是设计：

1. **编译期的证据探测**（`prefetchCompiledEvidence`）——先检索一次，然后问模型「从这次检索里编译出的 claims 是否已经能回答」。能就直接返回，不进循环。
2. **包装工具并跑 ReAct 循环**——每个工具被 `instrumentedTool` 包一层（计时、记账、看门狗）。
3. **最便宜的恢复**——若循环以叙述性文字或裸工具调用结束，从会话里取出模型自己已经写好的答案，而不是再调一次模型。
4. **确定性的兜底合成**（`finalizeAnswer`）——只有当前两步都没拿到答案时才跑，且用**脱离取消**的上下文。

第 1 步与第 3、4 步的存在理由是同一个：**一个 ReAct 循环最贵的失败不是崩溃，而是「跑完了但什么也没给出」**。所以整条管线在四个位置都在尝试「用更便宜的方式拿到一个答案」。

## 怎么实现的

**`defaultMaxIterations = 120` 的注释记录了一次基于实测的上调。**

```go
const defaultMaxIterations = 120
```

> `defaultMaxIterations` 限制了 ReAct 循环在 agent 必须回答之前的轮数。它**从原来的 50 提高**了，因为多步基准问题（多次跳转、差一的算术）经常需要超过 50 个模型回合，而**重复检索防护（todo 3）防止了额外预算被浪费在重复的相同搜索上**。

所以这不是「调大一点更安全」，而是**两个改动配套**：预算提高，同时加了一个防止预算被滥用的机制。只说前半句会读成「随便放大」，而后半句说明了为什么放大是安全的。

而 `agenticMaxIterations` 的三级解析与夹紧：

```go
func agenticMaxIterations(explicit int) int {
    if explicit > 0 { return explicit }
    n := envIntDefault(common.EnvAgenticMaxIterations, defaultMaxIterations)
    if n < 1 { return 1 }
    return n
}
```

注释解释夹紧：「零意味着『永不循环』，把每次运行变成立即失败」。所以 1 是下界——`1` 意味着「至少给模型一次工具调用机会」。

**三类台账的分工是这一页最核心的内容，而它们的定义各自带着自己的成立理由。**

**其一：`docIDLedger` 记「哪些文档被**完整内容**工具打开过」，而它存的是**文件名主干而不是 doc_id**。**

```go
// docIDLedger accumulates the set of documents a run's FULL-CONTENT tools
// (search_chunks, list_chunks) surfaced. Benchmarks (BrowseComp-Plus) score
// retrieval recall against this union, not just the cited subset, so the
// tally is taken as results come back. Only the tools' rendered doc_name is
// stored, normalized to its file stem (see recordedDocIDs): a benchmark
// corpus keys its evidence on file stems (e.g. "5412" from "5412.md"), which
// the opaque doc_id cannot match. Locate-only tools (search_bm25_chunks,
// grep_chunks) do NOT feed the ledger — they surface a snippet window, not
// the document.
```

三个决定：

1. **只收完整内容工具** ——`search_bm25_chunks` / `grep_chunks` 只给一个 `<match_snippet>` 窗口，所以「通过它们看到一篇文档」是**定位**而不是**检索**。
2. **基准按主干名打分** ——所以存 `"5412"` 而不是不透明的 `doc_id`。
3. **记账发生在「结果回来的那一刻」** ——不是等引用列表，因为基准算的是「这一轮拉过什么」，而引用只是「答案引用了什么」。

第 3 点是这个模块的立论：**引用召回 ≠ 检索召回**。`Run` 里那条注释说得更直：「Citation payloads only carry what the final answer quotes, but benchmarks score retrieval recall against everything the agent ever pulled」。

**其二：`chunkReadLedger` 把「读了多少块」按深度分成两档，而存 ID 的理由是一段实测记录。**

```go
// chunkReadLedger accumulates HOW MANY chunks a run's retrieval tools put in
// front of the model, split by read depth:
//   - DEEP reads (list_chunks, search_chunks): full chunk content rendered — the
//     expensive reads;
//   - SHALLOW reads (grep_chunks, search_bm25_chunks): <match_snippet> windows
//     only — the cheap triage reads.
```

而为什么在计数之外还要存 ID：

> 光有计数回答不了失败运行真正提出的那个问题：**那个装着答案的段落究竟到过模型面前吗？** 在 browsecomp 的 retry 批次上实测，**20 个 `evidence_in_hand` 失败里有 13 个曾有装着 gold 的文档被服务给这次运行，其中只有 6 个引用了它，0 个把它交出去**——这就是「读了然后静默丢掉」与「从没读过」的差别，而一个总数（q1021：458 块）分不出这两者。标识符是原始事实；**一个未被引用的已读块是否重要是审计者的判断，不是这个台账的**。

这段是这一页最有分量的文字，因为它把「为什么要多存一列」讲成了一个**可复现的测量结论**：13/20 的失败用例里，正确文档已经到达模型面前，而模型没引用它。所以问题的性质是「读了没用上」，不是「检索不到」。

而台账的边界被明确划住：「是否重要是审计者的判断，不是这个台账的」。所以它只提供事实（哪些 ID 被读了），不做判断。

**其三：`servedLedger` 的计数单位是**查询**而不是块，而它的注释记录了一次被修正的度量 bug。**

```go
// servedLedger records the documents the retrieval tools served to the model,
// counting - per document - the DISTINCT QUERIES that returned it. The
// served-unread watchdog diffs it against the deep-read document set
// (RetrievedDocIDs, filled only by full-content tools) and names the documents
// that kept surfacing and were never opened. A document served once in a
// passing sweep is noise; one several different searches returned is a
// candidate.
```

而「为什么不能按块计数」有一段带日期的实测：

> 单位是**查询**，绝不是块。按块计服务次数会让权重变成「一次结果里带了这篇文档的多少块」的函数：**2026-10-05 在一个十问题运行的 q1093 上实测**，这次运行自己已锁定的文档达到了 **35 次块服务**，而它本该考虑的那些未读线索都停在 2-4 次，于是普查的前五名是**一篇重复的文档**，而它存在的意义（点名那些线索）从来没进过名单。**按不同查询计数把同一篇文档降到 8**——仍然是最响的，因为这次运行确实问了它 8 次，但通胀消失了。

这是「度量口径错了一个维度」的典型：按块计会让「一次搜索返回了同一文档的 35 个块」被算成 35 次服务，而按查询计是 1 次。而修正后的 8 仍然最高，说明**真实信号是「问了它 8 次」而不是「它被返回了 35 块」**。

而 `Add` 的去重是 `(query, docID)` 对：

```go
// Add records one served document (the benchmark's file stem) under the query
// that returned it. Repeats of the same (query, document) pair collapse: the
// weight is how many different searches insisted on the document, not how many
// times one search was re-run.
```

注意后半句：「不是一次搜索被重跑了多少次」。所以重试同一个查询也不增加权重——这是一个**语义上正确的选择**：重跑同一个查询不产生新信息。

`Docs()` 的排序是「服务次数降序 + id 升序」，所以「谁最响」是确定性的。

**而 `servedLedger` 走 context 而不是参数，注释说明了原因。**

```go
// The serve ledger rides the context: every retrieval tool's serve point
// records what it put in front of the model, so the served-unread watchdog
// can name the documents the retrieval kept insisting on.
ctx = withServedLedger(ctx, in.Serves)
```

所以工具不需要知道台账存在——它在 `instrumentedTool` 里被包一层，那条 wrapper 里有 `unread *servedUnreadWatch` 引用。这是一个**旁路记账**：工具的输出给模型，同时被记账。这与 f04 里「检索工具 best-effort 写 canvas 状态」是同一种手法。

**三家工具工具表是从「一个叶子包」里读的，而那个包为什么必须是叶子有一段 import 环的解释。**

```go
acc := retrievalaccount.AccountToolOutput(name, out)
```

而 `retrievalaccount` 的包注释：

> 它**故意是一个叶子包**。进程内的引擎从它们的工具 wrapper 喂这些规则，而 mode-8 runner 只能通过 MCP 结果看到同样的文本；**共用一个实现是让两个引擎的基准行可比的唯一办法**。叶子必须是它所在的位置：`internal/agentic_rag` 拉进 `internal/dao`，而它 import `internal/codexagent`，所以 `codexagent` 不能反过来 import 引擎。

三层推理：

1. **两个引擎需要同一套分类规则** ——否则它们的基准行不可比。
2. **第二个引擎只能看到 MCP 结果文本** ——所以规则必须能只靠**渲染后的文本**工作。
3. **所以它必须在一个两个引擎都能 import 的叶子包** ——而 `agentic_rag → dao → codexagent` 这条链让 `codexagent` 无法反向 import。

第 2 点解释了这个包的实现形态：它全部靠**正则匹配渲染后的 XML**（`<chunk ` / `doc_name="..."` / `chunk_id="..."`），而不是靠结构化数据。这是一个「两个消费者只有一个能拿到结构」时的必然选择——**把共享逻辑下沉到最弱的信息形态**。

而 `AccountToolOutput` 的 docstring 补了一条防御：「一个不渲染任何语料文档的工具（`web_search` 渲染 url/title）什么也不算，所以**一个网页命中永远不会虚增检索召回**」。

**`instrumentedTool` 的日志策略是「args 全量记录」，而理由很实在。**

```go
fields := []zap.Field{
    zap.String("tool", name),
    zap.Float64("cost_ms", float64(cost.Milliseconds())),
    // Args are logged IN FULL: a truncated copy makes a call impossible to
    // reproduce after the fact. Tool args are bounded by construction
    // (queries, ids), so the volume cost is a few kilobytes per call at most.
    zap.String("args", args),
}
```

「一个截断的副本会让一次调用无法在事后复现」——而成本有界（查询与 id），所以全记。这是一个很干净的判断：**日志的价值在于事后能否复现，而截断恰好摧毁这个价值**。

**看门狗的注入位置是这一页最有洞察的一处。**

```go
if err == nil && t.watch != nil {
    // The locate watchdog rides the result, not the prompt: a rule read
    // once at the top of the system prompt competes with everything else
    // there, while this line appears in the tool result the model is
    // looking at when it decides what to query next.
    if nudge := t.watch.observe(name); nudge != "" {
        out += "\n" + nudge
        ...
    }
}
```

「看门狗**搭在结果上，不是提示词上**：写在 system prompt 顶部的一条规则会和那里的所有东西竞争，而这一行出现在模型正在看的工具结果里，恰好在它决定下一步查什么的时候。」

这是一条关于**LLM 注意力分配**的判断：system prompt 会被反复重读但注意力被稀释，而工具结果就在当前决策的焦点位置。代价是它污染了工具输出（模型会看到一段不是检索结果的内容），所以只在触发时注入（`nudge != ""`）。

而两个看门狗（`locateWatch` / `servedUnreadWatch`）在**社区版里是空实现**：

```go
// The community edition ships no locate/served-unread watchdogs: they live in
// the *_ee.go files. These stubs keep the shared agent loop compiling, and both
// observers return "" so no reminder is ever appended to a tool result.
type locateWatch struct{}
type servedUnreadWatch struct{}
```

所以「看门狗」这个能力被放在了 EE（企业版）文件里，而 OSS 用空 struct 保证同一个循环能编译、且永不注入提醒。这是一个 build-tag 分层的干净例子：**接口在共享代码里，行为在版本特有的文件里**。

**`Run` 的编译期证据探测在循环之前，而「顺序本身就是要点」这句话展开了三段推理。**

```go
// The compiled-evidence probe runs BEFORE the loop: search once, then ask the
// model whether the claims COMPILED FROM what that search returned already
// answer the question. A refusal falls back to the loop, which is the only
// place that decides what to search next.
//
// The ordering is the point. The search is the same hybrid chunk retrieval the
// locate tools run, so it works identically on a corpus with compiled
// artifacts and one without; what differs is only what is offered next. With
// claims, the turn can terminate on a single model call over them - the claims
// carry their own verbatim evidence, so no raw chunk has to be read. Without
// them (or when the model judges them insufficient) the loop runs exactly as
// it would have, and the model reads chunks itself.
```

而它不做提示词的第二个理由更锋利：

```go
// The probe is a control signal, not prompt text: an insufficient block is
// dropped here rather than pinned into the system instruction, where it
// would be re-billed on every call of the turn.
```

「一个不足的块在这里被丢弃，而不是钉进系统指令里，那样**它会在本回合的每次调用上被重新计费**」。所以「探测失败」的处置是**丢弃**，而不是「留着供模型参考」——因为在 ReAct 循环里，system prompt 每一轮都要重发。

**探测的三条结局各有独立的日志，而第三条注释解释了为什么这条日志不可省。**

```go
case strings.TrimSpace(answer) != "":
    // The loop is skipped entirely, so this line is the only record
    // that the turn never read a chunk: a review of tool usage must
    // be able to tell it from a turn the model gave up on.
    common.InfoCtx(ctx, "agentic_rag: answered from compiled claims", ...)
```

「循环被完全跳过，所以这一行是**这一轮从未读过任何块**的唯一记录：一次工具使用审查必须能把它与『模型放弃的那一轮』区分开」。

所以这条 INFO 日志承担了一个**审计功能**：一个零工具调用的成功轮次与一个零工具调用的失败轮次，在 `ToolCallCounts` 里完全一样，只有这条日志能区分。

**`llmRetryMax` 从 3 降到 1，注释给的是一次实验结论。**

```go
// Lowered from 3 to 1 on 2026-09-17 for the slow-plan experiment: a slow
// endpoint that fails at the send phase fails the same way on every attempt,
// so the extra attempts only bought latency (and, with the old 300s per-call
// budget, ran whole questions into the wall-clock deadline).
const llmRetryMax = 1
```

「一个在发送阶段就失败的慢端点，**每次尝试都以同样方式失败**，所以额外的尝试只买到延迟」。这是一个可推广的判据：**若失败发生在「还没开始生成」的阶段，重试没有意义**——因为重试不改变发送条件。

而重试配置有两条守卫：

```go
//  1. Retries are refused once partial content has streamed out
//     (OutputMessage != nil): adk forwards stream deltas to the client in
//     real time and only defers the retry decision, so a retried mid-stream
//     failure would duplicate already-emitted text.
//  2. context.Canceled / DeadlineExceeded are never retried — the 300s
//     Generate budget already spent is not doubled by another attempt.
```

第一条是流式的固有问题：**已经发出去的增量不能收回**。所以「流到一半失败」不能重试，否则用户会看到重复文本。这是流式系统与批式系统在重试策略上的根本差别。

第二条是预算的算术：一次 300s 的调用已花掉的时间不会因为重试而回来，重试只会让总时长翻倍。

**`ExecuteSequentially: false` 被显式写出，而注释解释「为什么是安全的」。**

```go
// Execute same-round tool calls (multiple tool_calls in one model
// response) concurrently rather than one-after-another. This is
// eino's default (false), but we state it explicitly so the
// parallel intent is not accidental. All of these tools
// (think/todo_write/grep_chunks/search_bm25_chunks/search_semantic_chunks/list_chunks/run_javascript/search_metadata)
// are concurrency-safe: they keep no shared mutable state across
// calls (run_javascript creates a fresh goja VM per invocation).
```

「这是 eino 的默认值，但我们显式写出来，**好让并行意图不是偶然的**」。这句值得记：一个依赖默认值的行为，在关键处应该显式声明，因为它承载了「这些工具是并发安全的」这个**已验证的前提**。

而前提本身被逐条点出：`run_javascript` 每次调用创建新的 goja VM。

**`UnknownToolsHandler` 的存在理由是一个很具体的「共享提示词 vs 按会话注册」的矛盾。**

```go
// A tool that is not registered is not a reason to abort the run.
//
// web_search is only registered when the conversation enabled internet, but the
// shared prompt mentions it, so the model sometimes calls it anyway. Without this
// handler eino raises "tool web_search not found in toolsNode indexes" and the
// whole turn dies — the question then scores zero even though the corpus tools
// were working. Answering with a tool result lets the model switch legs and keep
// reading.
```

矛盾结构：**提示词是共享的**（一份模板服务所有会话），但**工具是按会话注册的**（只有开了联网才有 `web_search`）。于是模型会按提示词去调一个不存在的工具，而 eino 的默认行为是直接抛错终止。

解法的实质是：**把「工具不存在」从致命错误降级成一个工具结果**。这样模型看到「没有这个工具」就能换一条腿继续。而 `Input.Tools` 的字段注释也说了为什么 `web_search` 不走 `Tools`：

> `web_search` 工具**不**在这里传：它从运行的上下文注入（见 `WithWebSearch`），这才是让一个会话的联网能力不泄漏到另一个会话的东西。

**`abortedTurn` 在恢复之前被捕获，而注释解释了这个顺序的必要性。**

```go
// abortedTurn is captured BEFORE recovery clears runErr: an aborted turn
// whose answer was recovered is still an aborted turn. It issued a tool
// call whose result never arrived, and that unmatched call replays as a
// provider error on the next turn, so the rollback below must run even
// when the carried answer makes the turn's RESULT usable.
abortedTurn := runErr != nil
```

关键洞见：**「结果可用」与「会话状态可用」是两件事**。一次运行可以因为「模型已经写好了答案但最后一个工具调用没返回」而中止，此时答案可以拿来用，但**那个没有结果配对的工具调用会留在会话里**，下一轮再放给 provider 时会报错。所以必须回滚会话（`discardFailedTurn`），即使结果本身已经救回来了。

这是一个「状态污染」的实例：把「这一轮的产物」与「会话的历史」分开看，前者可救、后者必须清算。

**兜底合成用 `context.WithoutCancel`，而理由是「它最常因为预算耗尽而触发」。**

```go
// Detach from the run's cancellation entirely: the fallback fires most
// often BECAUSE the shared wall-clock budget expired, and a deadline
// expiry and a client hang-up are indistinguishable to the derived
// context (both close the same Done channel) — keeping cancellation
// would reintroduce the failure this call exists to survive. The
// unbounded-work risk that WithoutCancel normally carries is covered
// by the bounded budget attached here.
```

三步推理：

1. **兜底触发的最常见原因就是「预算耗尽」** ——所以继承取消等于兜底必然也失败。
2. **超时与客户端挂断在 `Done` 通道上不可区分** ——两者都只是 close，所以无法「只忽略超时但保留挂断」。
3. **`WithoutCancel` 的无界风险由附加的 `finalizeTimeout` 覆盖** ——所以不是真的无界。

这是一个「必须刻意违反上下文取消传播」的场景，而它把违反的代价也交代了（有用 timeout 兜住）。

**最后的三路 switch 让「合成成功/失败」与「循环原本成功/失败」的四种组合被显式处理。**

```go
switch {
case synthErr == nil:
    final = synth; runErr = nil
case runErr == nil:
    runErr = synthErr
default:
    runErr = fmt.Errorf("%w; %w", runErr, synthErr)
}
```

而去重的那条注释说明为什么不 `append`：

> 替换而不是追加：追加会让读者看到一段叙述文字后面跟着真正的答案。`Run` 结尾的单次答案发射承载它。

所以 `final` 这个变量是**唯一**要发出去的东西，而 `emit(ctx, in.OnDelta, final, "")` 只在最后被调一次——循环过程中的增量走 `consumeAgentEvents` 那条路。

## 关键取舍

**源码事实：** 一个 `Run` 只答一轮；`Input.Messages` 的历史**必须已经压缩**（每轮一个最终答案，绝不是 ReAct 轨迹），这是调用方义务，写进了字段注释。

**源码事实：** `defaultMaxIterations = 120`，是从 50 上调的；上调的依据是「多步基准问题常需超过 50 个模型回合」，而配套的「重复检索防护」是让这个预算不被浪费的原因。

**源码事实：** `agenticMaxIterations` 三级解析（显式 > `EnvAgenticMaxIterations` > 默认），并夹紧到最小 1——「零意味着永不循环，把每次运行变成立即失败」。

**源码事实：** `docIDLedger` 只收**完整内容工具**（`search_chunks` / `list_chunks`），`search_bm25_chunks` / `grep_chunks` **不喂它**——因为后者给的是一个 `<match_snippet>` 窗口，「通过它们看到一篇文档是定位而不是检索」。

**源码事实：** `docIDLedger` 存归一化的**文件名主干**而不是 `doc_id`，因为「基准语料在文件主干上键它的证据（`5412` 来自 `5412.md`），而不透明的 `doc_id` 匹配不上」。

**源码事实：** `chunkReadLedger` 分 DEEP（完整块内容）与 SHALLOW（片段窗口）两档，且除计数外还存 **ID**；理由是实测——「browsecomp retry 批次 20 个 `evidence_in_hand` 失败里 13 个曾有装着 gold 的文档被服务给这次运行，6 个引用了它，0 个把它交出去」；「一个总数（q1021：458 块）分不出『读了丢掉』与『没读过』」。

**源码事实：** `servedLedger` 的计数单位是**不同查询数**而不是块数；注释记录了 2026-10-05 的度量 bug——按块计让一篇已锁定文档达到 35 次而其他线索只有 2-4 次，普查前五名成了一篇重复文档；按查询计降到 8，「仍然最响，但通胀消失了」。同一 `(query, doc)` 对去重，所以重跑同一查询不增权重。

**源码事实：** `servedLedger` 通过 `context` 传递（`withServedLedger`），所以工具不知道台账存在——记账是旁路的。

**源码事实：** 三类分类规则住在 `internal/retrievalaccount` 这个**叶子包**，因为「进程内引擎从工具 wrapper 喂规则，而 mode-8 runner 只能通过 MCP 结果看到同样的文本」；叶子是必须的，因为 `agentic_rag → dao → codexagent` 让 `codexagent` 无法反向 import 引擎。实现形态因此全部是**对渲染后 XML 的正则匹配**。

**源码事实：** `AccountToolOutput` 明确「一个不渲染任何语料文档的工具（`web_search` 渲染 url/title）什么也不算，所以一个网页命中永远不会虚增检索召回」。

**源码事实：** `instrumentedTool` 把工具 args **全量**记进日志，理由「一个截断的副本会让一次调用无法在事后复现」，而「工具 args 按构造有界（查询、id），所以体量成本每次调用最多几 KB」。

**源码事实：** 两个看门狗（`locateWatch` / `servedUnreadWatch`）的提醒**搭在工具结果上而不是 system prompt 上**——「写在 system prompt 顶部的一条规则会和那里的所有东西竞争，而这一行出现在模型正在看、正在决定下一步查什么的工具结果里」。

**源码事实：** 两个看门狗在社区版是**空实现**（`stub.go` 里 `type locateWatch struct{}` 与永远返回 `""` 的 `observe`）；真实行为在 `*_ee.go`。接口在共享代码里、行为在版本文件里。

**源码事实：** 编译期证据探测在循环**之前**，而「顺序就是要点」：「检索本身与 locate 工具跑的是同一个混合块检索，所以在有编译产物的语料与没有的语料上表现一致，不同的是接下来提供什么」。

**源码事实：** 探测结果是**控制信号而不是提示词文本**：「一个不足的块在这里被丢弃，而不是钉进系统指令，那样它会在本回合的每次调用上被重新计费」。

**源码事实：** 「从编译 claims 作答」的成功分支必须打一条 INFO 日志，因为「循环被完全跳过，所以这一行是这一轮从未读过任何块的唯一记录」——「工具使用审查必须能把它与模型放弃的那一轮区分开」。

**源码事实：** `llmRetryMax = 1`，2026-09-17 从 3 降低；理由「一个在发送阶段失败的慢端点每次尝试都以同样方式失败，额外尝试只买到延迟」。

**源码事实：** 重试的两条守卫：**一旦已有部分内容流出（`OutputMessage != nil`）就拒绝重试**（否则会重复已发出的文本）；`context.Canceled` / `DeadlineExceeded` 从不重试（已花掉的 300s 预算不会因重试而回来）。

**源码事实：** `ExecuteSequentially: false` 被显式写出，因为「这是 eino 的默认值，但我们显式声明好让并行意图不是偶然的」，并逐条列出并发安全前提（`run_javascript` 每次调用新建 goja VM）。

**源码事实：** `UnknownToolsHandler` 存在是因为「共享提示词提到 `web_search`，但它只在该会话开启联网时注册」——不处理会让 eino 抛「tool not found」并让整轮失败。

**源码事实：** `Input.Tools` 不含 `web_search`，它从运行上下文注入（`WithWebSearch`），「这才是让一个会话的联网能力不泄漏到另一个会话的东西」。

**源码事实：** `abortedTurn` 在恢复清空 `runErr` **之前**被捕获，因为「一次答案被救回来的中止轮次仍然是中止轮次」——它发出的工具调用从未收到结果，那个不配对的调用在下一轮会重放成 provider 错误，所以回滚必须执行。

**源码事实：** 兜底合成用 `context.WithoutCancel` 加独立的 `finalizeTimeout`，理由「兜底最常因为共享的 wall-clock 预算耗尽而触发，而超时与客户端挂断在派生上下文里不可区分（两者都只是关同一个 `Done` 通道）」；`WithoutCancel` 的无界风险由附加的 timeout 覆盖。

**源码事实：** 合成与循环的四组合被一个三路 `switch` 显式处理，且合成结果**替换**而非追加 `final`（「追加会让读者看到一段叙述文字后面跟着真正的答案」）。

**架构推论：** 三类台账的存在说明 **agentic 检索的失效不是「找不到」，而是三种更细的形态**：找到了但没读（`served` 高而 `docID` 无）、读了但没用（`deep` 有而引用无）、把预算花在重复上（同一 `(query, doc)` 反复）。**每一种都需要一个不同口径的计数**，而「一个检索召回率」这种单指标完全无法区分它们。任何做 agentic 检索评估的系统都该先问：我的台账能不能区分这三种？

**架构推论：** `servedLedger` 那次度量口径的修正（块 → 查询）是一条可推广的纪律：**统计单位必须与「信息增量」对齐**。一次查询返回同一文档的 35 个块，与一次查询返回它 1 个块，在「模型又被提醒了一次」这个意义上等价。按块计会按「结果大小」加权，而权重应该按「独立的催促次数」计。判据是：**这个计数在变大时，究竟是「事情变多了」还是「同一件事变大」？**

**架构推论：** 看门狗「搭结果不搭提示词」是一条关于 LLM 注意力的具体结论，值得抄进任何 agent 设计。system prompt 是**静态的、被稀释的、每轮重付 token 的**；工具结果在**当前决策焦点上**。所以「行为纠正」类指令应当以「在恰当的时刻出现在恰当的位置」为准，而不是以「写在最权威的位置」为准。而代价也真实：它污染工具输出，所以必须只在触发时注入。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/agentic_rag/agent.go` | `Input` | 一轮运行的完整入参，含四类台账指针 |
| `internal/agentic_rag/agent.go` | `Run` | 探测 → 循环 → 恢复 → 合成的四段 |
| `internal/agentic_rag/agent.go` | `NewDocIDLedger` | 完整内容工具打开过的文档主干集合 |
| `internal/agentic_rag/agent.go` | `NewChunkReadLedger` | 深/浅两档块计数 + ID 集合 |
| `internal/agentic_rag/agent.go` | `NewServedLedger` | 按不同查询计权重的已服务文档台账 |
| `internal/agentic_rag/agent.go` | `defaultMaxIterations` | 120，从 50 上调 |
| `internal/agentic_rag/agent.go` | `agenticMaxIterations` | 显式 > 环境变量 > 默认，夹紧到 1 |
| `internal/agentic_rag/agent.go` | `agentModelRetryConfig` | 两条守卫：流后不重试、取消不重试 |
| `internal/agentic_rag/agent.go` | `llmRetryMax` | 1，2026-09-17 从 3 降低 |
| `internal/agentic_rag/agent.go` | `instrumentedTool` | 计时 + 记账 + 看门狗注入，args 全量日志 |
| `internal/agentic_rag/agent.go` | `unknownToolResult` | 未注册工具降级成工具结果而非致命错误 |
| `internal/agentic_rag/agent.go` | `finalizeAnswer` | 确定性兜底合成，`WithoutCancel` + 独立预算 |
| `internal/agentic_rag/agent.go` | `withCancellationCause` | 补回 `ctx.Err()` 丢掉的取消原因（如 token 耗尽） |
| `internal/agentic_rag/stub.go` | `locateWatch` / `servedUnreadWatch` | 社区版空实现，`observe` 永远返回空串 |
| `internal/retrievalaccount/account.go` | `AccountToolOutput` | 两个引擎共用的分类规则，只靠渲染文本 |
| `internal/retrievalaccount/account.go` | `DocNameStem` | 把 `a/b/75314.md` 归一成 `75314` |
| `internal/retrievalaccount/account.go` | `fullContentDocTools` | 只有完整内容工具喂 `RetrievedDocs` |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：agentic_rag/agent.go](https://github.com/infiniflow/ragflow/blob/main/internal/agentic_rag/agent.go)
- [ReAct：推理与行动的交织（Yao et al., 2022）](https://arxiv.org/abs/2210.03629)
- [eino ADK 文档](https://www.cloudwego.io/docs/eino/core_modules/eino_adk/)

## 与主核 / AI 的关系

主核里最接近的是**查询计划与执行统计**：优化器给一个计划，执行器给实际行数，而 DBA 的工作是判断「计划错了」还是「统计陈旧」。这里的三类台账是同一件事在 agentic 检索上的形态——它们不判断对错，只提供**可分辨的事实**。

**差别在于「什么是标准答案」。** 数据库有客观的实际行数，所以「估计 vs 实际」的比值有明确含义。而 agentic 检索里，一次运行「该读多少块」没有客观值——所以这三类台账全部是**相对量**（`served` 高而 `deep` 低 = 只看到没读），靠**同一运行内部的比值**而不是绝对值给出信号。这也是为什么 `servedLedger` 要做「按查询计权重」这种修正：它的信号是**排序**（谁最该被点开），而不是绝对水平。

与 g02（知识图谱服务）/ g03（精确串检索）的关系是工具层的供给方：这三类台账统计的 `search_chunks` / `grep_chunks` / `graph_explore` 等工具，其实现分别在那两页。

与 f03（Agent 运行时）的关系有两处：`runtime.EmitAgentMessage` 是 `OnDelta` 的兜底（`Input.OnDelta` 为 nil 时的路径）；而 `withCancellationCause` 补回的「token 耗尽」这类原因，正是 f03 里 `DeferredStreamError` 保留 `Err` 的同一个动机——**取消原因不能被吞掉**。

与 e04（Pregel 引擎）/ f06（workflowx）的分层：那两页的 `recursionLimit` 与 `maxIterations` 都是「防止无限消耗」的上限，但这一页的 120 是**模型回合数**（一次工具调用算一轮），粒度与图节点/超步完全不同。三者的关系是：图的上限管结构深度，循环的上限管同一节点内的迭代，这里的上限管模型与工具的往复次数。

对 AI / agent 工程最实用的一条是**「台账要能区分失效形态」**。多数 agent 评估只有「任务成功率」一个数字，而这一页证明了至少还要三个：被服务但没被打开的文档数、被读了但没被引用的块数、同一文档被不同查询催促的次数。前两个都在失败运行里很高，而它们的修法相反——前者要改工具结果的可读性/引导，后者要改「既然读了就别丢」的提示策略。

另一条是 `UnknownToolsHandler` 那个降级。**共享提示词 + 按会话注册的工具**是一个几乎必然的组合（提示词是模板、能力是套餐），而它的失效形态是「模型按提示词调一个不存在的工具」。默认行为（抛错终止整轮）把一次「模型走错一条腿」升级成「整题零分」。正确做法是把「工具不存在」当一个正常工具结果返回，让模型自己换路子——**这是把「框架的硬错误」重新分类成「模型可恢复的反馈」**。

最后一条是 `abortedTurn` 那个顺序。它提醒的是：**「本轮结果可用」不等于「会话状态可继续」**。一次运行可能因为「答案已写出但最后一个工具调用未返回」而中止，答案可以拿走，但那个不配对的 tool_call 会在下一轮变成 provider 错误。任何带会话/历史重放的 agent 系统都要在「救回产物」之后仍然清算状态——否则错误会推迟一轮才爆发，而那时已经无法归因。
