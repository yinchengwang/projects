---
project: ragflow
feature: pipeline-executor
title: 摄取任务执行器
page_type: feature
schema_version: 1
refs:
  - path: internal/ingestion/task/pipeline_executor.go
    symbols: [PipelineExecutor, PipelineResult, NewPipelineExecutor]
  - path: internal/ingestion/task/pipeline_executor.go
    symbols: [insertChunksForIngestion, parseBaselineAnchor]
---

# 摄取任务执行器

## 一句话职责

一次文档摄取的**唯一入口**：载入画布 DSL → 跑流水线拿到块 → 规整／补父块／打 Pagerank → 分批写索引（非末批不等刷新）→ 对账编译产物 → 清运行期缓存，最后把「还需要调用方自己做的记账」（元数据、块计数、token 消耗、已写 ID 集合）打包成 `PipelineResult` 交出去。

## 机制是什么

这个执行器处在一个**没有事务域**的位置上：一次摄取要写三类存储——MySQL（文档状态、块历史版本、流水线日志）、搜索引擎（块索引，Infinity/ES）、对象存储（解析器抽出的图片）。三者没有共同的事务，所以任何一步失败都留下**跨存储的部分状态**。

于是整个执行器的设计都在回答同一个问题：**失败之后，系统该认为「这次摄取发生了多少」？** 它的答案由三件事共同构成：

1. **先写数据、后认账。** 块先进索引，然后才更新文档计数与状态。所以崩溃时最坏情况是「索引里有块但文档计数没涨」——这可以在重跑时自愈（重写同一批块，upsert 语义）。
2. **块历史基线（baseline）在块写入的同一次调用里记。** 见 `recordParseBaseline`。所以「一条块有历史版本 0」与「这条块进了索引」是同一个动作的两半。
3. **失败补偿用删除 + 墓碑（tombstone），不用回滚。** 索引写失败时 `compensateFailedIndexWrite` 把已写的 id 删掉，再给块历史追加墓碑，让「这次尝试写过的块」在历史里也标记为已取消。

第三条是最不显然的：**墓碑必须带着本次运行已经预定的那个数据集版本号**。注释把理由写得很直接——一次失败的运行不该独占一个版本号，而删除操作也不需要新版本号，因为 `AppendTombstones` 会在每条块的最后一行之后写一条，历史本身已经能正确排序这次取消。

## 怎么实现的

### 构造：`NewPipelineExecutor` 把注入点一次性接好

```go
func NewPipelineExecutor(
	taskCtx *TaskContext,
	canvasID string,
	docBulkSize int,
	deps *ingestion.Deps,
) (*PipelineExecutor, error) {
	if err := validateTaskContext(taskCtx); err != nil {
		return nil, err
	}
	if strings.TrimSpace(canvasID) == "" {
		return nil, fmt.Errorf("pipeline executor: empty canvas id")
	}
	if deps == nil {
		return nil, fmt.Errorf("pipeline executor: nil dependency bundle")
	}
	anchor := &parseBaselineAnchor{}
	...
}
```

四处：

1. **`deps` 装的是进程级单例**（文档引擎、数据库句柄）。注释明确：「the executor never resolves them itself」，即执行器不做依赖发现，全部在服务装配点解析。这让执行器可以在测试里被完全替换（见下面三个 `With*` 注入点）。

2. **`validateTaskContext` 的五个检查里，只有「知识库 id」是有条件必需的**：

```go
// A debug (dry-run) context carries no knowledgebase (see
// TaskContext.IsDebug), so it must not be required to supply one.
if !taskCtx.IsDebug() && taskCtx.Doc.KbID == "" {
	return fmt.Errorf("pipeline executor: empty document knowledgebase id")
}
```

**这条注释是「调试运行」这个概念进入校验层的唯一痕迹**：调试运行不落库，所以不需要知识库。其余四项（文档 id、文档名、租户 id）两种模式都要。

3. **`anchor` 在构造时就创建并传给两个 insert 函数**，所以**整个运行共享一个锚点**。这是后面所有版本号一致性的根。

4. **`indexWriter` 用两个 insert 函数分别构造**：普通批次用 `insertChunksForIngestion`，末批用 `insertFinalChunksForIngestion`。两者的差别只在末批等索引刷新。

### 两种写块函数：差别只在「等不等刷新」

```go
// Refreshing every batch adds unnecessary latency. The writer uses the regular
// inserter for its final batch, so all preceding batches become searchable
// before the document task is acknowledged and its completion event is sent.
func insertChunksForIngestion(eng engine.DocEngine, db *gorm.DB, language string, meta chunkversion.SnapshotMeta, anchor *parseBaselineAnchor) InsertFunc {
	return func(ctx context.Context, chunks []map[string]any, baseName string, datasetID string) ([]string, error) {
		var (
			ids []string
			err error
		)
		if bulk, ok := eng.(noRefreshChunkInserter); ok {
			ids, err = bulk.InsertChunksNoRefresh(ctx, chunks, baseName, datasetID, language)
		} else {
			ids, err = eng.InsertChunks(ctx, chunks, baseName, datasetID, language)
		}
		recordParseBaseline(ctx, db, anchor, datasetID, meta, chunks, err)
		...
```

四处：

1. **类型断言做能力探测**（`eng.(noRefreshChunkInserter)`）—— 这是 Go 里「可选能力」的标准写法：接口 `noRefreshChunkInserter` 单独定义，引擎实现了就走快路径，没实现就退回普通路径。**与 Python 里用函数属性探测能力是同一个思路**，但这里由编译器保证签名正确。

2. **「末批等刷新」的理由不是「最后一次必须一致」，而是「让前面所有不等刷新的批次在前台可见」**：批次之间的块只有在刷新后才可搜索，所以末批的刷新是**一次性的冲刷**，把前面所有批次的写入一并变可见。这也解释了为什么注释强调「before the document task is acknowledged and its completion event is sent」——顺序上必须在「任务完成」之前。

3. **`recordParseBaseline` 在 insert 之后调用，且把 `err` 传进去**。所以「插入成功才记基线」这条规则不由 `recordParseBaseline` 内部的判断决定，而由**调用点传 err** 决定——函数第一句就是 `if insertErr != nil || len(chunks) == 0 || db == nil { return }`。

4. **`language` 一路传下去**，注释给了理由：「The first write creates the chunk store, and on Infinity that fixes the fulltext analyzer, so the dataset language has to travel with it」。**这是一处不可逆的首次写入**：分析器在创建时固定，之后改数据集语言不会生效。所以 `datasetLanguage(taskCtx)` 在构造时就被求值并闭包捕获——**即使在运行中途改了数据集语言，这次运行也用构造时的值**。

### `parseBaselineAnchor`：一次运行一个版本号

```go
type parseBaselineAnchor struct {
	mu      sync.Mutex
	version int64
}

func (a *parseBaselineAnchor) number(ctx context.Context, db *gorm.DB, datasetID string) int64 {
	if a == nil || db == nil || datasetID == "" {
		return 0
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.version > 0 {
		return a.version
	}
	version, err := datasetversion.ReserveVersion(ctx, db, datasetID)
	if err != nil {
		common.Warn("Ingestion: failed to reserve a dataset version for the parse baseline", ...)
		return 0
	}
	a.version = version
	return version
}
```

五处：

1. **「一次运行只预定一次」的理由是版本号消耗与历史行数不匹配**：

> It is reserved once per run and shared by every batch: a parse writes its chunks batch by batch, and a reservation per batch would spend a number each time, pushing the dataset's version far past the few history rows it actually gains.

   **这是一处很具体的账目考虑**：一个 100 批的文档若每批预定一个版本，数据集版本号会跳 100，而实际历史只多了几十行。版本号不是时间戳，它要能对应「有哪些版本」——所以跳号会造成「有版本号但没有任何行描述它」的空洞。

2. **`sync.Mutex` 是必需的，因为批次可能并发写**：

> Batches may be written concurrently, so the number is taken under a lock: two batches must not reserve twice.

   **注意这个锁保护的是「只预定一次」这个语义，不是数据一致性**——`a.version > 0` 的检查与赋值之间有竞争窗口，没有锁会预定两次。

3. **失败返回 0 而不是抛错** —— 注释：「Zero means no number could be taken; the baselines are then skipped (a chunk with no history is still a chunk)」。**「块有历史」是观测能力，「块存在」是主体能力，前者失败不该拖垮后者。**

4. **`a == nil` 也被判** —— 所以调用方可以传 nil anchor，得到 0（基线跳过）。这让「不需要历史的调用路径」不用造一个假 anchor。

5. **`Version()` 用同一把锁读** —— 避免数据竞争（Go 里即使读也要同步，否则 race detector 会报）。

### `recordParseBaseline`：为什么必须现在记

```go
// Without it a chunk nobody edits has no history at all, and a content version
// could not put it back once the document is removed: the parser writes the
// engine row and moves on. A failure only logs — the chunk content is already
// indexed, and the history is an observation of it rather than a precondition.
//
// The anchor is taken before the parse finishes because the run's version row is
// recorded once the whole document is written and has to include every baseline
// it covers; it is reserved rather than recorded because a version row per batch
// would fill the history with entries describing a run that has not finished.
func recordParseBaseline(ctx context.Context, db *gorm.DB, anchor *parseBaselineAnchor, datasetID string, meta chunkversion.SnapshotMeta, chunks []map[string]any, insertErr error) {
```

**这段注释是本页最重要的推理，值得逐句拆：**

1. **为什么要在写块时就记基线**：「a chunk nobody edits has no history at all, and a content version could not put it back once the document is removed」。即：没有人编辑过的块若没有基线，那么文档被删除后想「按内容版本恢复」就无从恢复——因为历史里根本没有这条块的第一版。

2. **为什么失败只记日志**：「the chunk content is already indexed, and the history is an observation of it rather than a precondition」。**这是「历史是观测、不是前提」的判断**——与 `parseBaselineAnchor.number` 返回 0 的处理是同一个原则。

3. **为什么「预定」而不「立即记录版本行」**：「a version row per batch would fill the history with entries describing a run that has not finished」。所以运行结束时才写那一条版本行，而基线先挂在一个**已预定但尚未描述**的号上。这中间的窗口靠 `DatasetVersion` 字段交给调用方补齐。

4. **`meta` 参数把「用哪个嵌入模型」也记进基线**，`datasetSnapshotMeta` 的注释说明了为什么必须一致：

> The embedding model is the dataset's embd_id, the same id the editor records when it rewrites a chunk: a baseline and an edit of the same chunk must agree about their producer, or a restore would refuse one of the two rows and silently re-embed it.

**这是「同一块的两次历史必须承认同一个生产者」的契约**。不一致的后果是恢复时拒绝其中一行，然后静默重嵌入——一次不报错的性能与质量损失。

### `Execute`：五个阶段，两个分支

```go
dsl, correctedID, err := s.loadDSLFunc(ctx, s.canvasID)
if err != nil { return nil, err }
if correctedID != "" { s.canvasID = correctedID }
var savePreview func(context.Context)
if !s.taskCtx.IsDebug() {
	s.persistResolvedPipeline(context.WithoutCancel(ctx))
	ctx, savePreview = component.PrepareDocumentPreview(ctx, s.deps.DB, s.taskCtx.Doc)
}

pipelineOutput, pipelineDSL, err := s.runPipelineFunc(ctx, dsl)
if err != nil { return nil, err }

if s.taskCtx.IsDebug() {
	return s.collectDebugOutput(ctx, pipelineOutput, start)
}
result, err := s.processOutput(ctx, pipelineOutput, start)
```

六处：

1. **`loadDSLFunc` 返回 `(dsl, correctedID, err)`** —— `correctedID` 是「画布 id 被修正过」的信号。所以画布可以改名/迁移而旧引用仍能跑。

2. **`context.WithoutCancel(ctx)` 出现两次** —— 在 `persistResolvedPipeline` 与 `recordPipelineLog`。**理由能推出来**：取消发生后仍然需要记「这次被取消了」这条日志；若用继承取消的 ctx，写库会立刻失败。

3. **`PrepareDocumentPreview` 返回一个 `savePreview` 闭包** —— 所以预览数据的写盘被推迟到 `processOutput` 成功之后（`if savePreview != nil { savePreview(ctx) }`）。**顺序上它必须在块落库之后**，否则预览里会有指向不存在块的 id。

4. **调试分支在 `processOutput` **之前**返回** —— 所以调试运行完全不走「规整/父块/写索引/对账」这条链。这是**结构上的隔离**，不是靠 if 打补丁。

5. **`pipelineDSL != ""` 与 `PipelineLogID != nil && *... != ""` 的双重判断** —— 只有真的创建过日志行才更新它。

6. **`terminalDuration` 的兜底**：

```go
var terminalDuration *float64
if result != nil {
	terminalDuration = &result.Duration
} else {
	// A successful run with no chunks never wrote a terminal duration
	// to the document, so recompute from the same process_begin_at
	// anchor here instead of letting the log copy the stale mid-run
	// value.
	d := s.terminalDuration(start)
	terminalDuration = &d
}
```

   **「成功但没有块」是一次真实的路径**（`processOutput` 里 `if len(chunks) == 0 { s.purgeRunState(ctx); return nil, nil }`）。若此时用「进度快照里的耗时」，那是一个**运行中途**的值——比真实短。所以用同一个 `process_begin_at` 锚点重算。

### `processOutput`：十步，每步都可失败

顺序是：规整块 → 拿嵌入 token 消耗 → 收集托管元数据键 → 处理块 → 物化父块 → 聚合表格元数据 → 标记 wiki 产物隐藏 → **重载文档状态** → 打可用性 → 载入旧编译状态 → 写索引 → 对账编译产物 → 持久化 Wiki MAP 状态 → 清运行期缓存。

其中四处值得单独看。

**第一处：重载文档状态是必要的，因为快照会过期。**

```go
// Reload status immediately before write: LoadFromIngestionTask copied a
// snapshot that may be stale if BatchUpdateDocumentStatus ran mid-pipeline.
docStatus := s.taskCtx.Doc.Status
if s.deps.DB != nil {
	if persisted, err := dao.NewDocumentDAO().GetByID(ctx, s.deps.DB, s.taskCtx.Doc.ID); err == nil && persisted != nil {
		docStatus = persisted.Status
		s.taskCtx.Doc.Status = persisted.Status
	} else if err != nil {
		common.Warn(...)
	}
}
applyDocumentAvailability(chunks, docStatus)
```

**这是「读自己的快照可能已经过期」的标准场景**：任务上下文里的文档是在任务开始时载入的，而流水线运行期间别的协程可能改过文档状态（`BatchUpdateDocumentStatus`）。若用旧快照，一个已经被禁用的文档会写入 `available_int=1` 的块，**那些块会进检索结果**。

**注意 `err == nil && persisted != nil` 才用持久值**；查询失败只记 warn 并沿用快照——即「查不到就用旧的」，而不是「查不到就不写」。这是一处「宁可可能错也不阻塞」的选择。

**第二处：旧编译状态必须在写新块**之前**载入。**

```go
oldCompiledProductIDs, oldCompiledVariants, oldCompiledTaskTypes, err := s.loadDocumentCompiledState(ctx)
```

```go
// loadDocumentCompiledState snapshots the previous successful document
// compiler generation before the new pipeline output is written. Reading first
// avoids relying on immediate search visibility after a bulk index write.
```

**「先读」的理由是可见性延迟**：批量写入后索引不一定立刻可见，所以「写完再读旧状态」读到的可能是混合视图。先读就完全不依赖可见性。

**第三处：写索引失败时的补偿是一条完整的链。**

```go
indexWriteErr := s.indexWriter.Write(ctx, indexChunks)
...
if indexWriteErr != nil {
	if cleanupErr := s.compensateFailedIndexWrite(ctx, indexChunks); cleanupErr != nil {
		return nil, fmt.Errorf("write chunks: %w; compensate partial index write: %v", indexWriteErr, cleanupErr)
	}
	return nil, fmt.Errorf("write chunks: %w", indexWriteErr)
}
```

**注意两个错误被并列返回**（`;` 连接）。这是对的：**补偿失败时原始错误更重要，但补偿失败本身也必须可见**——否则运维会以为只是写入失败，而不知道「还有残留行没删掉」。

而 `compensateFailedIndexWrite` 里三段注释说明了墓碑为什么必须带已预定的版本号：

> The tombstones carry the number the run already reserved for its baselines: a run that fails must not take a version of its own, and the removal does not need one — `AppendTombstones` writes one past the latest row of each chunk, so the chunk history already orders the removal after the baseline it cancels, and carving out a fresh dataset number only pushes the dataset's counter past a version nothing describes.

三处推理：

1. **失败运行不该独占版本号** —— 与「预定一次」的节俭原则一致。
2. **删除不需要新版本号** —— 因为墓碑写在每条块历史的**最后一行之后**，历史本身就给出了顺序。
3. **另开一个新版本号只会让数据集计数器越过一个「没有任何行描述」的号** —— 即空洞。

而「失败发生在第一批之前」时 `Version()` 是 0，所以不写墓碑 —— 注释：「there is no baseline to cancel, so nothing is written」。

**第四处：清缓存的位置是「所有可失败步骤都成功之后」。**

```go
// Persist is now fully durable: every failure-capable step above
// (index write, compiled-product reconcile, Wiki active-MAP state) has
// succeeded. Only now drop the run-scoped cache entries this task produced,
// so a failure in any of those steps leaves chunk results and parser media
// intact for retry.
s.purgeRunState(ctx)
```

**这是「缓存也是重试所需的输入」这个判断**：若在写索引之前就清缓存，那么写索引失败后重试会**重新解析**（因为解析结果缓存没了），而媒体（图片）也可能被重新上传。所以清缓存必须是最后一步。

### `PipelineResult`：把记账交出去

```go
// PipelineResult is the outcome of a pipeline run: chunks have been
// indexed, and these bookkeeping inputs remain for the caller to apply to
// document state (metadata merge + chunk/token counter bumps).
```

四处：

1. **`OriginalChunkIDs` 与 `WrittenChunkIDs` 是两套账，注释解释了为什么不能合并**：

> `WrittenChunkIDs` is every engine row id this run wrote: source chunks, the parent rows materialised from them and the compiled products. The document-state finalizer diffs it against the chunk history to tombstone the chunks a re-parse dropped. It must hold the complete written set: diffing the source-only set would tombstone the parent and compiled rows the same run just wrote, and a content restore could then never bring them back.

   **即：`OriginalChunkIDs` 用于「文档的块计数」（要排除父块与编译产物），`WrittenChunkIDs` 用于「tombstone 对账」（必须包含全部已写行）。**

   若两者混用：用 source-only 做对账，会把这次刚写的父块与编译产物**标成已删除**，而内容恢复再也拿不回它们。

2. **`countOriginalChunkIDs` 按 id 去重而不是按长度**：

> After ProcessChunksForPipeline, every chunk carries an `id` field computed from xxhash(text+docID). Chunks with identical text share the same id and the search engine treats them as upserts, so the effective stored chunk count is the number of unique ids — not len(chunks). Mirrors the index-side deduplication that happens at write time.

   **这是「计数必须与存储语义一致」**：存储里同文本块是 upsert（后写覆盖前写），所以块数 = 唯一 id 数。若按 `len(chunks)` 计，计数会虚高。

3. **`DatasetVersion` 是 0 就是「没有基线」** —— 与 `anchor.number` 的返回 0 语义一致，且 `0` 让调用方知道「这次跑没有可挂的版本」。

4. **`CompiledVariants` / `CompiledTaskTypes` 是「要唤醒数据集级消费者合并哪些产物」的集合**，而 `mergeCompiledVariants` 的注释说明了为什么要与上一代取并集：

> A reparse that removes a compiler template still needs to notify the dataset consumer so it can retract stale merged products; a document that never had compiler products produces an empty set and does not wake the dataset consumer at all.

   **重解析删掉了某个编译模板后，仍必须通知消费者去撤回旧的合并产物** —— 因为撤回动作需要由「曾经有过」这一事实触发。

### 事件在最后才发，理由与刷新有关

```go
// All chunks are now persisted. Defer the dataset-level post-processing
// notification until the enclosing ingestion task reaches COMPLETED. The
// index writes intentionally do not wait for refresh, so publishing here lets
// the consumer race the final task bookkeeping and read an incomplete view of
// the document's products.
eventVariants := mergeCompiledVariants(oldCompiledVariants, compiledVariants(chunks))
eventTaskTypes := mergeTaskTypes(oldCompiledTaskTypes, compiledTaskTypes(chunks))
```

**这里是「只构造、不发布」** —— 结果被打包进 `PipelineResult`，由上层在任务到达 COMPLETED 之后才发事件。理由写得清楚：索引写不等刷新，若在这里发布，消费者可能读到不完整视图。

**这与「末批等刷新」形成一个完整的时序设计**：末批刷新发生在 `indexWriter.Write` 内部（`withFinalInsertFunc`），然后 `processOutput` 才返回；上层再把任务标 COMPLETED 并发事件。所以「事件发出时，所有块已可搜索」是有保证的。

## 关键取舍

**源码事实：** 执行器不做依赖发现——`deps` 是服务装配点解析好的进程级单例（引擎 + 数据库），并有 `WithInsertFunc` / `WithDeleteChunksFunc` / `WithLoadDSLFunc` / `WithRunPipelineFunc` / `WithProgressSink` 五个注入点供测试替换。

**源码事实：** `validateTaskContext` 的五项检查里，「知识库 id」仅非调试运行必需（注释写明调试上下文不带知识库）；其余四项（文档 id / 文档名 / 租户 id / 非 nil 上下文）两种模式都要。

**源码事实：** 普通批次走 `InsertChunksNoRefresh`（类型断言 `noRefreshChunkInserter` 做能力探测），末批走普通 `InsertChunks` 等刷新。末批刷新的目的是让**前面所有批次**在任务确认前变为可搜索。

**源码事实：** 数据集语言随首次写入传下去，因为在 Infinity 上首次写会**固定全文分析器**；`datasetLanguage(taskCtx)` 在构造时求值并被闭包捕获，所以运行中途改语言不影响本次。

**源码事实：** `parseBaselineAnchor` 用 `sync.Mutex` 保证「一次运行只预定一个版本号」；预定失败返回 0，基线跳过，不抛错；nil anchor 与空 datasetID 都返回 0。

**源码事实：** 「一次运行一个版本号」的理由是账目：每批预定会让数据集版本号远超实际历史行数，留下「有号但无行描述」的空洞；运行结束的那条版本行记在同一个号上，所以一次解析只推进一个版本。

**源码事实：** `recordParseBaseline` 由调用点传 `insertErr` 决定是否记；失败只记日志，因为「块内容已索引，历史是对它的观测而非前提」。`meta` 记嵌入模型等派生字段的来源，与编辑器改块时记的值必须一致，否则恢复会拒绝其中一行并静默重嵌入。

**源码事实：** `Execute` 里 `context.WithoutCancel` 用于 `persistResolvedPipeline` 与 `recordPipelineLog`——取消后仍需落库。`savePreview` 闭包推迟到 `processOutput` 成功之后才执行。

**源码事实：** 调试运行在 `processOutput` **之前**返回，结构上隔离，不落库、不上传媒体、不写流水线日志。

**源码事实：** 写索引**之前**会重新从数据库载入文档状态（任务上下文的快照可能已被 `BatchUpdateDocumentStatus` 改过）；载入失败只记 warn 并沿用快照。

**源码事实：** 旧编译状态必须在写新块**之前**载入，因为批量写后索引不一定立刻可见，先读避免读到混合视图。

**源码事实：** 索引写失败时补偿是「删已写 id + 追加墓碑」，两个错误并列返回；墓碑带本次已预定的版本号，理由是一次失败的运行不该独占版本号、删除不需要新号（墓碑写在每条历史最后一行之后）、另开新号会造空洞；若本次没预定过号（第一批之前就失败）则不写墓碑。

**源码事实：** 清运行期缓存放在**所有可失败步骤成功之后**，因为解析结果缓存与媒体是重试所需的输入；提前清会让重试重新解析与重新上传。

**源码事实：** `PipelineResult` 里 `OriginalChunkIDs`（只含源块，驱动文档块计数）与 `WrittenChunkIDs`（含父块与编译产物，供 tombstone 对账）**不能混用**；混用会把本次刚写的父块/编译产物标成已删除，内容恢复再也拿不回。

**源码事实：** 块计数按唯一 id 数而非 `len(chunks)`，因为同文本块的 id 是 `xxhash(text+docID)`、引擎按 upsert 处理，计数必须与存储语义一致。

**源码事实：** `CompiledVariants` / `CompiledTaskTypes` 与上一代取**并集**——重解析删掉某个编译模板后仍须通知消费者撤回旧合并产物。

**源码事实：** 数据集级后处理事件在此处只**构造不发布**，由上层在任务到达 COMPLETED 后发；否则消费者会与最终记账竞争并读到不完整视图。

**架构推论：** 这个执行器是「跨存储无事务」的教科书式应对：它不追求原子性，而是追求**每一步失败后状态可解释、可重试**。判据是三件事——写入顺序（数据先、账后）、历史与主体同一次调用、失败补偿用删除+墓碑而非回滚。这套组合让「崩溃」在任何一点都留下一个能被下一次运行或恢复流程理解的状态，而不是一个需要人工猜的状态。

**架构推论：** 「预定版本号 vs 记录版本行」这个分离是纯账目设计。版本号是资源（可耗尽、会跳号），版本行是证据（描述一个已完成的运行）。把两者绑在一起会让「运行中途失败」也必须留下一行不完整的证据；把它们分开则可以用「预留 + 完成时补记」的经典两阶段模式——代价是中间窗口里存在一个「有号无行」的状态，需要靠墓碑与 `DatasetVersion` 字段把两侧接上。

**架构推论：** 「历史是观测不是前提」这条判断决定了整个基线机制的失败方向：所有与基线相关的失败都降级为日志，从不阻塞块写入。这是对的——因为块的**主体价值**是能被检索到，历史的价值是「以后能恢复/回滚」。让观测失败拖垮主体，就本末倒置了。反过来也一样：任何把「可观测性」做成硬依赖的系统，都会在观测组件故障时全站不可用。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/ingestion/task/pipeline_executor.go` | `PipelineExecutor` | 执行器状态（任务上下文、索引写入器、历史锚点、注入点） |
| `internal/ingestion/task/pipeline_executor.go` | `NewPipelineExecutor` | 校验上下文、构造锚点与两个写入函数 |
| `internal/ingestion/task/pipeline_executor.go` | `PipelineResult` | 交给调用方继续记账的产物 |
| `internal/ingestion/task/pipeline_executor.go` | `validateTaskContext` | 五项上下文校验（知识库仅非调试必需） |
| `internal/ingestion/task/pipeline_executor.go` | `Execute` | 载入 DSL → 跑流水线 → 调试分支 / 落库分支 |
| `internal/ingestion/task/pipeline_executor.go` | `processOutput` | 规整 → 父块 → 可用性 → 写索引 → 对账 → 清缓存 |
| `internal/ingestion/task/pipeline_executor.go` | `collectDebugOutput` | 调试运行：只返回块，不写任何存储 |
| `internal/ingestion/task/pipeline_executor.go` | `insertChunksForIngestion` | 非末批写入（能力探测走不刷新路径） |
| `internal/ingestion/task/pipeline_executor.go` | `insertFinalChunksForIngestion` | 末批写入（等刷新） |
| `internal/ingestion/task/pipeline_executor.go` | `parseBaselineAnchor` | 一次运行一个数据集版本号（带锁） |
| `internal/ingestion/task/pipeline_executor.go` | `recordParseBaseline` | 为刚写入的块记版本 0（失败只日志） |
| `internal/ingestion/task/pipeline_executor.go` | `compensateFailedIndexWrite` | 删部分写入 + 追加墓碑（带已预定版本号） |
| `internal/ingestion/task/pipeline_executor.go` | `countOriginalChunkIDs` | 按唯一 id 计数（匹配 upsert 语义） |
| `internal/ingestion/task/pipeline_executor.go` | `writtenChunkIDs` | 全部已写行 id（供 tombstone 对账） |
| `internal/ingestion/task/pipeline_executor.go` | `mergeCompiledVariants` | 与上一代取并集（重解析也能撤回） |
| `internal/ingestion/task/pipeline_executor.go` | `purgeRunState` | 清解析/媒体缓存（仅在所有写成功之后） |
| `internal/ingestion/task/pipeline_executor.go` | `terminalDuration` | 从 `process_begin_at` 锚点重算耗时 |
| `internal/ingestion/task/pipeline_executor.go` | `datasetSnapshotMeta` | 基线的派生字段来源（嵌入模型须一致） |
| `internal/ingestion/task/pipeline_executor.go` | `buildLogDSL` | 持久化只存 DSL 定义，不存运行数据 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 文档：文档解析与分块](https://ragflow.io/docs/dev/configure_knowledge_base)
- [论文：双阶段提交与分布式事务的替代方案](https://dl.acm.org/doi/10.1145/58448.58477)
- [论文：幂等写入与至少一次语义](https://arxiv.org/abs/2104.00480)

## 与主核 / AI 的关系

这个执行器与主核（多模态数据库内核）的**事务管理器**处理的正是同一个问题，而两者的答案完全相反：主核用 WAL + 两阶段提交把「三类存储的一致」做成一个原子操作；这里承认做不到原子，改用「写入顺序 + 补偿 + 可发现的历史」。

**差别不在于技术能力，而在于能否把所有写入者纳入同一事务域。** 主核的存储层全是它自己管的（页、WAL、索引），所以能保证原子；这里的搜索引擎与对象存储是**外部系统**（Infinity/ES、MinIO），各自有独立的事务模型，无法参与一个共同的两阶段提交。**判据是「写入目标是否都在自己的控制域内」**：在内则可以原子，在外则只能靠补偿。

第二处对照是「预定版本号 vs 记录版本行」与主核的**事务 ID 分配**。主核的 `xid` 也是先分配后使用（`GetCurrentTransactionId` 在事务开始时就分配），而 WAL 里的提交记录才让它「有意义」。这与这里的「预留 + 完成时补记」是同一模式。差别在主核的空洞靠 `xid` 的单调性与 CLOG 的 `CLOG_STATUS` 填充，这里的空洞靠墓碑与 `DatasetVersion` 字段。

第三处是「历史是观测不是前提」与主核的**统计信息**（`pg_statistic`）。两者都是「关于数据的元数据」，都在失败时降级而不阻塞主体操作——`ANALYZE` 失败不会让表不可写。**这是同一条架构纪律**：可观测性的价值是辅助决策，它不该成为可用性的前提。

对 RAG 工程的实际意义在于：摄取是 RAG 里唯一「多存储、长流程、易中断」的环节，而它的失败处理几乎决定了系统的可用性。这份实现给出的三条经验可直接用：**数据先写、记账后做**（崩溃时最坏情况是「多了数据少了账」，而重跑能自愈）、**补偿用删除+标记而非回滚**（外部存储无法回滚，只能追加否定事实）、以及**清缓存要放在所有写成功之后**（缓存本身是重试的输入，提前清会让重试变成重算）。
