---
project: ragflow
feature: knowledge-compile
title: 知识编译：排产、消费与落盘
page_type: feature
schema_version: 1
refs:
  - path: internal/ingestion/knowledge_compile/writer.go
    symbols: [Writer, StructureBucket, pageCommitter]
  - path: internal/ingestion/knowledge_compile/scheduler.go
    symbols: [Claimer, Publisher, ClaimResult]
  - path: internal/ingestion/knowledge_compile/consumer.go
    symbols: [Consumer]
  - path: internal/ingestion/component/knowledge_compiler/tree/claims.go
    symbols: [Claim, EvidenceRef, ExtractClaimsForChunks]
---

# 知识编译：排产、消费与落盘

## 一句话职责

把「某个文档的抽取刚完成」这件事变成数据集级的产物：**排产**把文档事件写进持久化积压表并配一个租约，**消费**由一个 worker 按「每数据集至多一个租约」领走一批并跑各变体的合并，**落盘**把合并结果写成 `available_int=1` 的数据集级行——三者之间靠 MySQL 行锁串行化，因为它没有跨存储事务可用。

## 机制是什么

这里要解决的是一个**跨进程、跨存储的串行化问题**：文档级抽取在 worker A 的进程里完成，而「把 N 个文档的产物合并成一份数据集级产物」必须串行（否则两个 worker 会各自基于不完整的输入算出不同的合并结果）。

三类存储各自的能力边界决定了整个设计：

| 存储 | 能保证什么 | 不能保证什么 |
|---|---|---|
| MySQL | 行锁、事务、`FOR UPDATE` | 与另两类存储的原子性 |
| DocEngine（ES/Infinity） | 单次批量写/删 | 跨「删 + 插」的事务边界 |
| NATS | 一次推送 | 可靠性（可能丢消息） |

**所以三条设计原则贯穿三层：**

1. **MySQL 是唯一的调度权威，NATS 只是唤醒。** `Publisher` 的注释写明：「MySQL is the system of record; NATS notify is only a best-effort wake-up」。而 `notify` 失败时只警告「workers will poll」——**丢消息只增加延迟，不丢任务**。
2. **租约（lease）而不是队列。** 领走一批是「backlog → inflight 的移动，不是拷贝」，配上 `claim_owner` / `claim_token` / `claim_expires_at`。所以「谁在处理」是可查的，「处理到哪」由 token 校验。
3. **无法原子就靠「可重建」。** `ProjectWikiGraph` 的注释把这条讲得最直白：「the DocEngine has no cross-delete/insert transaction boundary, so the graph is rebuilt from a consistent read and the old graph is dropped first. **The graph is reconstructible, so the lack of atomicity is accepted.**」

第三条是本页的核心判断：**缺少原子性可以用，前提是这个产物能从源数据重算**。而「能否重算」决定了哪些地方可以接受部分状态。

## 怎么实现的

### 排产：`publishEntry` 里的 gap lock 与一个已修过的坑

```go
// KnowledgeCompileDataset.dataset_id is the PRIMARY KEY, so the SELECT ... FOR
// UPDATE below also takes an InnoDB gap lock for a not-yet-existing dataset;
// concurrent publishers for the same dataset serialize on that lock and the
// loser observes the row already present. FirstOrCreate then reliably finds
// or inserts exactly one row, and the append below runs under the same row
// lock so concurrent appends from different publishers cannot interleave.
err := s.db.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
	row := entity.KnowledgeCompileDataset{
		DatasetID: datasetID,
		TenantID:  tenantID,
	}
	if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
		Where("dataset_id = ?", datasetID).
		FirstOrCreate(&row).Error; err != nil {
		return err
	}
```

四处：

1. **`FOR UPDATE` 在「行还不存在」时会拿 gap lock** —— 这是 InnoDB 的语义，而它正好解决了「首次发布」的并发问题：两个并发 publisher 会串行，后到的那个看到行已存在。

   **这是一个必须依赖存储引擎内幕的设计**：若换成不支持 gap lock 的引擎（MySQL 的 READ COMMITTED 在部分场景、或 Postgres 上用 `ON CONFLICT`），这段就不成立。

2. **`FirstOrCreate` 的 `row` 必须先填好 `DatasetID`/`TenantID`** —— 注释标了 `M19`：「a raw-string Where alone leaves DatasetID blank on the created row」。**这是 GORM 的语义**：只传字符串 `Where` 时，插入的行没有主键值。

3. **`if row.TenantID == "" { row.TenantID = tenantID }`** —— 兜底已有行为空的情况（历史数据）。

4. **追加与状态变更都在同一事务里** —— 所以「积压多了但状态没变」不可能发生。

**状态迁移有一处刻意的例外：**

```go
// Surface a pending state unless a worker is already running (a live lease
// means the consumer is mid-merge on this KB; a pending transition would
// wrongly hide that). completed -> pending when new work arrives.
if row.State != DatasetStateRunning {
	row.State = DatasetStatePending
}
```

**「Running 时不改成 Pending」是必须的**：一个 worker 正在合并这个 KB，若发布新事件把它改成 Pending，前端会看到「等待中」而实际正在跑。**这是一处「状态字段既要反映生命周期又要反映可用性」的冲突，解法是让有租约的 Running 优先。**

**而 `Publish` 与 `notify` 被合并成一个接口的动机很直接：**

```go
// Publish is the single producer entry point so a publisher never forgets to
// Notify after enqueue (the two are now one atomic call site).
```

**「两个步骤必须一起做，就合成一个入口」** —— 这是一条通用纪律：靠文档要求「记得调 notify」迟早会漏。

### 领取：`claimRow` 是唯一的原子点

```go
func (s *mysqlScheduler) claimRow(ctx context.Context, tx *gorm.DB, datasetID string) (ClaimResult, bool, error) {
	var row entity.KnowledgeCompileDataset
	if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
		Where("dataset_id = ?", datasetID).First(&row).Error; err != nil { ... }
	now := time.Now()
	liveLease := row.ClaimOwner != "" && row.ClaimExpiresAt != nil && row.ClaimExpiresAt.After(now)
	if liveLease {
		return ClaimResult{}, false, nil
	}
	var backlog []entity.KnowledgeCompileEntry
	if err := tx.Table((entity.KnowledgeCompileBacklog{}).TableName()).
		Where("dataset_id = ?", datasetID).Order("queue_position ASC").
		Limit(s.claimBatchSize).Find(&backlog).Error; err != nil { ... }
	if len(backlog) == 0 {
		return ClaimResult{}, false, nil
	}
	...
	if err := appendQueueEntries(tx, (entity.KnowledgeCompileInflight{}).TableName(), datasetID, backlog); err != nil { ... }
	if err := tx.Where("dataset_id = ? AND queue_position IN ?", datasetID, positions).
		Delete(&entity.KnowledgeCompileBacklog{}).Error; err != nil { ... }
	...
	row.ClaimOwner = s.holder
	row.ClaimToken = generateHolder()
	exp := now.Add(s.leaseTTL)
	row.ClaimExpiresAt = &exp
	row.State = DatasetStateRunning
	row.ErrorMsg = ""
	row.Progress = 0
	row.CurrentPhase = "claiming"
	row.ProgressMsg = ""
```

六处：

1. **`liveLease` 的三段判断** —— `ClaimOwner != "" && ClaimExpiresAt != nil && After(now)`。所以「没有 owner」「没有到期时间」「已过期」都视为可领取。**三个条件缺一不可**，因为字段可能是分步写入的。

2. **「先写 inflight 再删 backlog」的顺序** —— 注释里 `ClaimResult` 说明这是「a move not a copy」。而顺序很重要：先插后删时，若中途崩溃会**短暂重复**（同一批同时在两表里），下次 `reclaimOne` 能把它并回去；反过来若先删后插，崩溃会**永久丢失**。

   **「宁可重复不可丢失」是这一步的正确选择**，因为下游合并是幂等的。

   （唯一键上若 (doc_id, event_type) 重复，`appendQueueEntries` 的实现需要处理冲突——但即使插入失败，事务会整体回滚，backlog 也不会被删。所以顺序的安全性来自事务而非表约束。）

3. **`Order("queue_position ASC") + Limit(claimBatchSize)`** —— 所以是 FIFO。`claimBatchSize` 由 `defaultClaimBatch` 决定。

4. **`positions` 是「本批的 queue_position 列表」** —— 删 backlog 用 `queue_position IN ?` 而不是 `(doc_id, event_type) IN ?`。**用位置删更安全**：同一 (doc, event) 可能在 backlog 里出现多次（多次发布），按二元组删会删掉不该删的。

   而 `Ack` 删 inflight 用的是 `(doc_id, event_type) IN ?`（因为没有 position 了）。**两处的键不同，是各自表结构决定的结果。**

5. **`ClaimToken = generateHolder()`** —— 每次领取都生成新 token。所以「上一次领的 token」在重新领取后立刻失效。**这是「租约被抢走后旧 worker 必须自证失效」的全部机制。**

6. **领取时把 `Progress` 归零、`CurrentPhase = "claiming"`、清 `ErrorMsg`** —— 所以每次领取都是一次「全新的可见状态」，不会把上次失败的进度带到这次。

**`TryClaim` 的扫描与回收是两步，而顺序不能反：**

```go
// 1) a ready batch with no live lease.
if id, ok, e := s.findClaimableID(ctx, tx, now); e != nil { ... } else if ok { ...claimRow... }
// 2) reclaim an expired lease back into backlog, then claim it.
if id, ok, e := s.reclaimOne(ctx, tx, now); e != nil { ... } else if ok { ...claimRow... }
```

**「先找空闲的，再回收过期的」** —— 理由能推出来：回收是更贵的操作（要 requeue + 清租约），而空闲数据集通常更多。**先做便宜的事**。

而 `TryClaim` 显式指定了隔离级别：

```go
}, &sql.TxOptions{Isolation: sql.LevelReadCommitted})
```

**这是本页唯一显式设置隔离级别的地方** —— 而 `Claim` / `Ack` / `WithDatasetLock` 都用默认级别。**理由大概是**：`TryClaim` 的扫描是「多行查询 + 后续逐行加锁」，用 ReadCommitted 能避免间隙锁把「扫描」这一步变成阻塞其他数据集的大锁。

**`findClaimableID` 的排序透露了优先级策略：**

```go
Order("priority DESC, updated_at ASC").
Limit(1).
```

**「优先级降序、同优先级最久未更新的优先」** —— 后者就是 FIFO 的等价物（用 `updated_at` 而不是入队时间）。**这防的是「某个数据集饥饿」**。

**`reclaimOne` 逐行处理并且重新校验：**

```go
for _, row := range rows {
	var cur entity.KnowledgeCompileDataset
	if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).Where("dataset_id = ?", row.DatasetID).First(&cur).Error; err != nil { ... }
	if cur.ClaimExpiresAt != nil && cur.ClaimExpiresAt.After(now) {
		continue
	}
	reclaimed, err := requeueInflight(tx, cur.DatasetID)
	...
	if reclaimed == 0 {
		continue
	}
```

四处：

1. **先批量查候选（不带锁），再逐个加锁重查** —— 因为「扫描时看到的过期状态可能在加锁前被别人回收了」。所以**加锁后必须重判**（`cur.ClaimExpiresAt.After(now)`）。
2. **`reclaimed == 0` 时 `continue`** —— 若 inflight 已空（别的 worker 抢先回收并 ack 了），则不重复清租约。
3. **清租约三件事一起做**：`ClaimOwner = ""`、`ClaimToken = ""`、`ClaimExpiresAt = nil`，并把状态置 `Pending`。**token 必须清空**，否则被回收的 worker 拿旧 token 还能通过校验。
4. **只回收一个就返回** —— 所以每次 `TryClaim` 最多回收一个数据集，然后立刻尝试领取它。

**`WithDatasetLock` 是整个串行化的核心机制：**

```go
// The claim token passed to fn is read within this transaction, so the check in
// fn does not need (and must not attempt) a separate DB read that would deadlock
// against the held row lock.
func (s *mysqlScheduler) WithDatasetLock(ctx context.Context, datasetID string, fn func(claimToken string) error) error {
	if s.db == nil {
		return fn("")
	}
	return s.db.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		row := entity.KnowledgeCompileDataset{DatasetID: datasetID}
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
			Where("dataset_id = ?", datasetID).
			FirstOrCreate(&row).Error; err != nil {
			return err
		}
		return fn(row.ClaimToken)
	})
}
```

四处：

1. **「claimToken 在事务内读，所以 fn 内不得另开连接再读」** —— 注释明确「must not attempt a separate DB read that would deadlock against the held row lock」。**这是一条极具体的禁令**：另开连接读同一行会等锁，而这一行正被自己持有 → 自死锁。

2. **`fn` 在这个事务里执行** —— 所以「token 校验 + 破坏性写入」在同一个行锁下。**这就是关闭 TOCTOU 的办法**：不是校验后再写，而是「校验与写都在锁内」。

3. **`fn` 返回错误会回滚事务** —— 注释：「any DB write performed inside fn is rolled back, so no partial side effect persists on an aborted write」。

4. **`s.db == nil` 时直接 `fn("")`** —— 测试/无库场景不串行化。

### 消费：`Consumer` 的两个 goroutine 与三种退出

```go
stopHb := make(chan struct{})
hbFailed := make(chan struct{}, 1)
go func() {
	t := time.NewTicker(c.heartbeat)
	for {
		select {
		case <-stopHb: return
		case <-ctx.Done(): return
		case <-t.C:
			alive, e := c.scheduler.TouchClaim(ctx, datasetID, cr.Token, c.ttl)
			if e != nil || !alive {
				select {
				case hbFailed <- struct{}{}:
				default:
				}
				return
			}
		}
	}
}()

done := make(chan struct{})
var batchErr error
go func() {
	defer close(done)
	batchErr = c.processBatch(ctx, cr.TenantID, datasetID, cr.Token, cr.Entries, logTaskTypes)
}()

select {
case <-ctx.Done(): ... return    // graceful shutdown, no ack
case <-hbFailed: ... return      // lease lost, no ack
case <-done: close(stopHb)
}
```

六处：

1. **心跳用 `hbFailed`（容量 1 且 `default` 分支）** —— 所以「失败信号只发一次，不会因为生成者多而阻塞」。**这是「信号只需一次」的正确 channel 用法。**

2. **`heartbeat = 20s`，`ttl = 2m`** —— 心跳是租约的 1/6，所以容忍 5 次丢心跳。

3. **`batchErr` 写在 goroutine 里、读在 `<-done` 之后** —— `close(done)` 建立了 happens-before，所以没有数据竞争。**这个模式（结果变量 + done channel）比返回 channel 更简单。**

4. **三种退出的语义完全不同**：
   - **ctx 取消 → 不 ack**，注释：「graceful shutdown: leave inflight for reclamation, do not ack」。批次留在 inflight，等过期回收后重跑。
   - **心跳失败 → 不 ack**，注释：「lease lost: do not ack; sweeper/redelivery reprocesses (idempotent)」。**这里显式承认了「会重复处理」，并靠幂等性兜住。**
   - **批次失败 → 不 ack，只 `SetError`**。

5. **`errClaimSuperseded` 被特殊处理：直接 return，不记错误** —— 因为「租约被抢走」不是失败，是正常竞争。**若不区分，日志会被「竞争失败」刷满，掩盖真正的错误。**

6. **`startDatasetCompileLog` 失败只警告** —— 所以「日志表写不进去」不阻塞编译。而 `finishDatasetCompileLog` 也全部只警告。**这是「可观测性不是前提」的又一次应用**（与 b01 的基线一致）。

**`withWriteLock` 只有一行逻辑，但它是全页最关键的一行：**

```go
func (c *Consumer) withWriteLock(ctx context.Context, kb, token string, fn func() error) error {
	return c.scheduler.WithDatasetLock(ctx, kb, func(currentToken string) error {
		if currentToken != token {
			return errClaimSuperseded
		}
		return fn()
	})
}
```

**「比较 token 然后写」在锁内执行** —— 所以一个被 `CancelInflight` 撤销的 worker 一定在写之前发现。**这是 `CancelInflight` 能「不做 token 校验就撤销」的前提**：撤销本身不需要校验（它是权威的），而 worker 自己会在写之前校验。

**`RebuildDataset` 的五步顺序是「关闭两个不同的竞态」：**

```go
// 1. set the rewrite pause (in-process only; clears via defer on every path);
// 2. CancelInflight invalidates any in-flight claim before clearing storage.
// 3. DeleteMerged + DropWikiGraph clear the old merged + graph state INSIDE the
//    same per-dataset row lock (WithDatasetLock).
// 4. enumerate every doc and republish;
// 5. clear the local pause.
```

四处：

1. **pause 必须是进程内（`c.rebuildPause` map）** —— 因为它是「本实例不再领这个 KB」的本地意愿，跨进程无效。而跨进程的互斥靠 `CancelInflight` + 行锁。

   而 `defer` 清理的理由注释写得很清楚：「a failure mid-rebuild can never leave the dataset permanently paused (which would drop every future claim）」。**「永久暂停 = 永久丢弃所有后续 claim」，这是最坏的失败模式。**

2. **`CancelInflight` 在清存储**之前** —— 顺序不能反**：若先清存储再撤销，一个正在跑的 worker 可能在清除之后写入，把旧结果复活。

3. **清存储与 worker 的写都在同一把行锁下** —— 注释解释了它如何关闭 TOCTOU：「a worker can never repopulate cleared state with old results because it cannot hold the lock concurrently with the clear」。

4. **`mode` 参数保留但两者走同一条路径** —— 注释：「today both take the same clean-and-rebuild path, with mode retained for future differential strategies」。**这是一处「声明了未来能力但当前未实现」的接口。**

### 落盘：`Writer` 的九个方法，每个都在补一个存储缺口

`Writer` 接口的方法不是按「产物类型」分的，而是按**「存储能力不足时需要什么补偿」**分的：

| 方法 | 补的是什么缺口 |
|---|---|
| `WriteMerged` | 正常 upsert（`available_int=1`） |
| `DeleteDocLevelForDocs` | 文档删除时清它的 `available_int=0` 行 |
| `StripMergedSources` | 删文档后从别人的 `source_doc_ids` 里摘掉它 |
| `ProjectWikiGraph` | 无跨删/插事务 → 先删后建（靠可重建） |
| `WriteMergedStructure` | 结构产物的桶级合并 |
| `DeleteStructureForDocs` | 幽灵清理（G3） |
| `DeleteMergedForVariant` | 模板被删后撤回它的合并产物 |
| `DeleteMerged` | 全量重建前清场 |

**这个分类揭示了一件事：文档删除在「合并后的产物」上不是一次删除，而是三件事** —— 删自己的行、从别人的来源列表里摘掉自己、删掉因此变空的行。

而 `StripMergedSources` 的注释把这个三段讲全了：

> It searches the merged set once, rewrites the source array of every non-empty survivor in a single update pass, and deletes (in one call) any product whose array became empty.

**「一次搜索、一次批量改写、一次批量删除」** —— 所以是 O(1) 次引擎调用而不是 O(N) 次。

**`deleteMerged` 的过滤条件注释里有一句很关键的话：**

> The `match_kwd` guard is the in-memory safety net; **the structural filter is the source of truth**.

**「内存里的守卫只是安全网，结构过滤才是权威」** —— 这是「双重防护」的典型说法：结构过滤（`kb_id` + `available_int=1` + `compile_kwd` 变体）是可信的判据，而 `match_kwd` 只是防止误伤。

**而删除之前必须先「命名」这些页：**

```go
func (w engineWriter) DeleteMergedWikiPages(ctx context.Context, tenant, kb string, ids []string) error {
	...
	// The pages are named before they go: a delete leaves nothing behind to say
	// which page it was, and the version timeline needs that to put the page back.
	w.recordWikiPageRemovals(ctx, tenant, kb, ids, pageRemovalByCompilation)
	_, err := eng.DeleteChunks(...)
```

**「删除不留痕迹，而版本时间线要靠痕迹才能把页放回去」** —— 所以先记删除提交，再删。**这是「审计必须在删除之前」的具体理由**，而不是抽象要求。

**而两种删除原因被定义成常量：**

```go
const (
	pageRemovalByCompilation = "Removed by artifact compilation"
	pageRemovalByDocDelete   = "Removed because its source document was deleted"
)
```

**「注释把一次移除与一次编辑区分开」** —— 所以「为什么消失」是历史里的一等信息。

**`recordWikiPageRemovals` 里有一段关于 `SelectFields` 的具体坑：**

```go
// The shape columns are what names the page: WikiPageCategory reads
// entity_type_kwd (new rows) or page_type_kwd (old ones), and
// CompilationRowType reads compile_kwd/type_kwd. Without them a row the
// compiler wrote looks like a non-page and is dropped below.
SelectFields: []string{"id", "slug_kwd", "page_type_kwd", "entity_type_kwd", "compile_kwd", "type_kwd", "title_kwd", "content_with_weight", "source_doc_ids"},
```

**「少选一个字段会让行看起来不像页，于是被当成非页静默丢弃」** —— 这是一处很典型的部分投影陷阱：投影列表同时是「性能优化」与「判定输入」，两者耦合。

而注释还写了批处理的理由：「One attribution lookup per batch, not per page」。**200 条一批，作者归属只查一次。**

**`StructureBucket` 里 `VecCount` 的语义是一条容易漏的细节：**

```go
Vector         []float32
VecCount       int    // number of vectors folded into Vector (for true mean)
```

**「为真均值而记录折叠了几个向量」** —— 所以 `Vector` 不是简单求和，而是「累加后除以 `VecCount`」。**这解释了为什么需要这个字段**：增量合并时旧桶的向量已是均值，再加新向量后要按加权重算，而不是 `(old + new)/2`。

### claims：证据门是「一句话校验」的另一半

tree 的 claims 抽取是这套机制里最细的一环，它的核心是**「模型必须给逐字引文，而我们用子串匹配验证它」**。

```go
type Claim struct {
	Name           string        `json:"name"`
	Description    string        `json:"description"`
	Type           string        `json:"type"`
	SourceChunkIDs []string      `json:"source_chunk_ids"`
	Evidence       []EvidenceRef `json:"evidence,omitempty"`
}

type EvidenceRef struct {
	Quote   string `json:"quote"`
	ChunkID string `json:"chunk_id"`
	Start   int    `json:"start"`
	End     int    `json:"end"`
}
```

而 `EvidenceRef` 的注释说明了 `Start`/`End` 的来源：

> Offsets are only known after validation, so a claim parsed from the model carries a zero span until `ValidateClaims` resolves it against the source text.

**「偏移不是模型给的，是校验时算出来的」** —— 所以 `Start`/`End` 是**事实**而不是**声明**。这是一处很干净的分离。

**`EvidenceGateMode` 的两种模式，而默认是「软」：**

```go
// EvidenceGateSoft keeps the claim with its evidence dropped — the default,
// because enumeration / multi-hop questions degrade badly when facts are
// discarded (see claim_evidence.md §4.2).
EvidenceGateSoft EvidenceGateMode = "soft"
// EvidenceGateHard drops the claim along with its evidence, the behaviour
// the ISC paper validated on single-document dialogue QA.
EvidenceGateHard EvidenceGateMode = "hard"
```

**三处**：

1. **软门保留 claim 但丢掉证据** —— 而理由是「枚举/多跳问题在事实被丢弃时退化严重」。**即：一个没有引文的 claim 在枚举题里仍有用，而丢了就是永久丢了。**

2. **硬门两者都丢，且它「在 ISC 论文上验证过」** —— 所以两个模式的取舍依据是**不同的评测场景**，不是「哪个更严格」。

3. **默认是软** —— 所以「宁可留着没出处的事实」是默认取向。**这与「宁缺勿错」相反，是「这个任务的错更贵还是缺更贵」的具体判断。**

而 `ParseEvidenceGateMode` 的兜底是「任何不认识的值都用默认」，**不是报错** —— 所以配置写错会静默降级成软门。**这与 b03 的「显式矛盾必须报错」形成对比**：那里报错是因为配置本身自相矛盾（`enabled: true` 但 `top_n = 0`），这里只是值不认识。

**prompt 里那段「表格必须保留原样」是最值得抄的一条：**

```
- For tables, infoboxes and bullet lists, the quote MUST be the raw cell/row
  text exactly as it appears — keep its separators and order. Do NOT turn a
  table row like "Starring | Penn Badgley | Elizabeth Lail" into a sentence
  like "Starring Penn Badgley Elizabeth Lail"; that is a paraphrase and will
  be rejected.
```

**「模型最自然的动作（把表格读成句子）恰好是我们拒绝的」** —— 所以必须显式禁止，而且给了具体的反例。**这是「校验规则必须在 prompt 里预告」的实例**：模型知道会被拒，就不会做。

**批量与归属：`claimBatchSize = 4`，且「幻觉 id 必须过滤」。**

```go
// Keep only ids that are actually in this batch: a hallucinated id
// points at text the model never saw, so its quote could never be
// validated.
src := make([]string, 0, len(it.SourceChunkIDs))
for _, id := range it.SourceChunkIDs {
	if batchIDs[id] {
		src = append(src, id)
	}
}
```

**四处**：

1. **过滤依据是「这个 id 在本批里吗」** —— 因为模型只见了本批的文本，引用批外 id 必然是幻觉。

2. **`claimBatchSize = 4`** —— 所以一批四次块一个调用。而 prompt 里明确「the cap is per chunk, not per batch」。

3. **`it.Type = "claim"` 被强制设置** —— 不信任模型给的 type。

4. **`Description` 空则回退到 `Name`** —— 因为「用于检索的重述」缺失时，claim 本身仍可作检索文本。

**而校验只在批内做，理由是一句很硬的话：**

```go
// Validate only against this batch's text: a gate that could scan the whole
// document would let a quote match an unrelated chunk that merely happens to
// contain the same sentence, and would then attribute the claim to it.
```

**「全文档范围的校验会把 claim 归属到错误的块」** —— 因为同一句话可能出现在多个块里。所以**校验范围必须与「模型可见范围」一致**。

**归属恢复的逻辑是「从证据反推来源」：**

```go
for _, c := range claims {
	id := ""
	if len(c.SourceChunkIDs) > 0 {
		id = c.SourceChunkIDs[0]
	} else {
		for _, e := range c.Evidence {
			if batchIDs[e.ChunkID] {
				id = e.ChunkID
				break
			}
		}
	}
	if id == "" {
		rfcommon.Warn("tree: dropped claim with no attributable source", zap.String("claim", c.Name))
		continue
	}
	c.SourceChunkIDs = []string{id}
	out[id] = append(out[id], c)
}
```

**三处**：

1. **优先用模型给的 id，其次用「引文被定位到哪个块」** —— 因为「引文定位成功」是比模型声明更强的事实。
2. **两者都无则丢弃** —— 注释：「filing it under an arbitrary chunk would cite text it never came from」。**「宁可丢也不随便归」**。
3. **最终 `SourceChunkIDs` 被截成单元素** —— 因为 claim 归到一个块。而模型可能给多个 id，那些信息被丢弃。

**`ValidateClaims` 的核心是「定位 + 排序保证确定性」：**

```go
candidates := []string{e.ChunkID}
if textByID[e.ChunkID] == "" {
	candidates = make([]string, 0, len(textByID))
	for id := range textByID {
		candidates = append(candidates, id)
	}
	// Map iteration order is random, so sort: with two chunks
	// holding the same sentence the gate must pick the same one on
	// every run, otherwise a recompile repoints the quote.
	sort.Strings(candidates)
}
```

**四处**：

1. **优先用引文自带的 `chunk_id`** —— 只有在它不在本批时才回退到全批扫描。
2. **`sort.Strings` 保证确定性** —— 注释把后果说得很具体：「otherwise a recompile repoints the quote」，即**重编译会让同一条 claim 的引文指向不同的块**。这会让「两次编译的产物不可比」。
3. **`break` 在第一个命中后** —— 所以按排序后第一个匹配的块。
4. **`verified += len(kept)`** —— 统计的是**证据条数**，不是 claim 条数。

**`LocateEvidence` 的两个字节级细节都值得记：**

```go
endNorm := pos + len(nQuote) - 1
if endNorm >= len(idxMap) { return 0, 0, false }
end := len(text)
if endNorm+1 < len(idxMap) {
	end = idxMap[endNorm+1]
}
return idxMap[pos], end, true
```

而注释：

> End is the original offset just past the match: the offset of the next normalized byte, or the text length when the match runs to the end. **Adding 1 to the last matched byte instead would cut a multi-byte rune in half whenever the match ends inside one.**

**「用下一个字节的原始偏移当结束位置，而不是末字节 +1」** —— 否则结束位置可能落在一个多字节字符中间。

而 `normalizeForMatch` 的注释记了一个真实的 bug：

> The map is indexed with byte offsets, because that is what `strings.Index` returns. **Recording one entry per rune misaligned every lookup as soon as the text held a multi-byte rune, so CJK quotes resolved to the wrong span — or failed to match at all.**

**「每个 rune 记一条会让 CJK 文本的所有偏移错位」** —— 这是一个「按字符 vs 按字节」的经典混淆，而它的后果是**中文引文要么定位错要么完全匹配不到**。

**而 `claimExtractionPrompt` 的注释说明它与模板版本必须同步：**

```go
// claimExtractionPrompt is the built-in claim-extraction prompt. It must stay in
// sync with the template-declared version; a divergence here silently changes
// what the tree is summarised from, making the built-in and template-declared
// versions' trees incomparable.
```

**「内置 prompt 与模板声明的 prompt 一旦分叉，两者产出的树就不可比」** —— 因为 prompt 决定了抽什么 claim，而树是从 claim 归纳的。**这是「同一逻辑的两份拷贝必须同步」的代价，而这里选择显式记录它。**

**而 `ExtractClaimsForChunks` 的整体失败策略是「最佳努力」：**

```go
// Extraction is best-effort: a failing batch is skipped and simply contributes
// no claims, which makes the tree fall back to raw text for it. A missing LLM
// client disables extraction entirely.
```

**「失败的批不产 claim，树就退回原始文本」** —— 所以抽取出问题不会让树建不出来，只是树的质量降到「用原文」。**这是「降级而非失败」的一贯策略。**

**最后，进度上报的动机很具体：**

```go
// Announce the fan-out shape before the first LLM call: the batches take
// model latency, so without this line the log would sit silent on
// "building tree" for tens of seconds.
```

**「不报这一行，日志会在『building tree』上静默几十秒」** —— 这是「长任务要报起点」的具体理由，而不是抽象的可观测性要求。

## 关键取舍

**源码事实：** 三层职责：`Publisher` 写持久化积压 + 唤醒；`Claimer` 领租约并管心跳/进度/取消；`Writer` 写数据集级产物。MySQL 是**调度权威**，NATS 只是唤醒（`notify` 失败只警告「workers will poll」，丢消息只增延迟不丢任务）。

**源码事实：** `publishEntry` 依赖「`dataset_id` 是主键 + `SELECT FOR UPDATE` 对不存在的行拿 gap lock」来让并发发布串行化；`row` 必须先在结构体上填 `DatasetID`/`TenantID`（只传字符串 `Where` 会让插入行主键为空，标记 `M19`）。

**源码事实：** 发布时只有当 `row.State != Running` 才改成 `Pending`——「有活租约说明 worker 正在合并，置 Pending 会错误地隐藏这一点」。

**源码事实：** `Publish` 与 `notify` 合并成一个入口，理由是「a publisher never forgets to Notify after enqueue」。

**源码事实：** `claimRow` 是唯一原子点：`FOR UPDATE` → 拒「活租约」（`ClaimOwner != "" && ClaimExpiresAt != nil && After(now)` 三条都满足才算活）→ 取 `queue_position ASC` 限量 → **先写 inflight 再删 backlog**（崩溃时宁可短暂重复也不丢）→ 带新 `ClaimToken` 盖租约 → 重置进度与错误。

**源码事实：** 删 backlog 用 `queue_position IN ?`，而 `Ack` 删 inflight 用 `(doc_id, event_type) IN ?`——两处键不同，是表结构决定的结果（inflight 没有保留 position 语义）。

**源码事实：** `TryClaim` 先找空闲（`findClaimableID`，排序 `priority DESC, updated_at ASC`）再回收过期（`reclaimOne`），且是全仓唯一显式设 `LevelReadCommitted` 的地方。

**源码事实：** `reclaimOne` 先无锁批量查候选、再逐个 `FOR UPDATE` 重查并重判 `ClaimExpiresAt`；`reclaimed == 0` 时 `continue`；清租约必须同时清 `ClaimOwner` / `ClaimToken` / `ClaimExpiresAt`（token 不清空则被回收的 worker 仍能通过校验）。

**源码事实：** `WithDatasetLock` 在事务内 `FOR UPDATE` 取行并把 `row.ClaimToken` 传给 `fn`；注释明确 **`fn` 内不得另开连接重读该行**（会与自持的行锁死锁）；`fn` 返回错误会回滚其所有 DB 写入。

**源码事实：** `Consumer` 默认 `ttl=2m`、`heartbeat=20s`（租约的 1/6，容忍 5 次丢心跳）、`pollInterval=2s`、`sweepInterval=30s`、`mergeThreshold=0.85`。

**源码事实：** `Run` 三种唤醒：NATS（直接领该数据集）、poll ticker、sweep ticker（sweep 同时负责崩溃恢复）；channel 关闭时置 `notifyCh = nil` 从而只走轮询。

**源码事实：** `processClaim` 起心跳 goroutine（`hbFailed` 容量 1 + `default`，信号只发一次）与批次 goroutine（`close(done)` 建立 happens-before），然后三种退出语义：ctx 取消 → 不 ack（留 inflight 等回收）；心跳失败 → 不 ack（「sweeper/redelivery reprocesses (idempotent)」）；批次失败 → 不 ack 且 `SetError`（「never ack what we failed to merge」）。`errClaimSuperseded` 直接返回不记错误（竞争不是失败）。

**源码事实：** 数据集编译日志的 `start`/`update`/`finish` 全部失败只警告，不阻塞编译。

**源码事实：** `withWriteLock` 的全部内容是「在行锁内比较 token 再执行 `fn`」——这是 `CancelInflight` 能「不做 token 校验就撤销」的前提：撤销是权威的，而 worker 在写之前自证失效。

**源码事实：** `RebuildDataset` 五步固定顺序：置进程内 pause（`defer` 清，否则「永久暂停 = 永久丢弃所有未来 claim」）→ `CancelInflight` → 在同一行锁内清存储 → 枚举全部文档重新发布 → 清 pause。`mode` 参数保留但两条路径相同。

**源码事实：** `Writer` 的方法按「补偿哪种存储缺口」分类；文档删除在合并产物上是三件事：删自己的行、从别人的 `source_doc_ids` 摘掉自己、删掉因此变空的行（`StripMergedSources` 一次搜索 + 一次批量改写 + 一次批量删除）。

**源码事实：** `DeleteMerged` 的注释明确「`match_kwd` 是内存安全网，**结构过滤才是权威**」。

**源码事实：** 删除 wiki 页**之前**先记删除提交，因为「a delete leaves nothing behind to say which page it was, and the version timeline needs that to put the page back」；两种删除原因被定义成常量以区分「移除」与「编辑」。

**源码事实：** `recordWikiPageRemovals` 的 `SelectFields` 必须含形状列（`entity_type_kwd` / `page_type_kwd` / `compile_kwd` / `type_kwd`），否则「compiler 写的行看起来不像页，于是被静默丢弃」；批大小 200 且「每批只查一次作者归属而不是每页查」。

**源码事实：** `StructureBucket.VecCount` 记录「折叠了几个向量」，使增量合并能算真均值而不是 `(old+new)/2`。

**源码事实：** `EvidenceGateMode` 默认 `soft`（保留 claim 丢证据），理由是「枚举/多跳问题在事实被丢弃时退化严重」；`hard` 丢两者，是「ISC 论文在单文档对话问答上验证的行为」。`ParseEvidenceGateMode` 对任何不认识的值回退默认而**不报错**。

**源码事实：** `Claim` 的 `Start`/`End` 不是模型给的，而是 `ValidateClaims` 定位后算出的——「偏移在校验后才可知，所以模型解析出的 claim 携带零宽度 span」。

**源码事实：** claim 批量 `claimBatchSize = 4`；「幻觉 id」按「在不在本批」过滤；`Type` 被强制设为 `"claim"`；`Description` 空则回退 `Name`。

**源码事实：** 校验只在**本批文本**内做——「全文档范围的校验会让引文匹配到恰好含同句的不相关块，并把 claim 归属给它」。

**源码事实：** 归属恢复优先用模型给的 id、其次用「引文被定位到的块」，两者都无则**丢弃**（「filing it under an arbitrary chunk would cite text it never came from」）；最终 `SourceChunkIDs` 截为单元素。

**源码事实：** `ValidateClaims` 在回退到全批扫描时对候选 id 做 `sort.Strings`，理由是「Map 迭代顺序随机，两个块含同一句时必须每次选同一个，否则重编译会把引文重新指向别的块」。

**源码事实：** `LocateEvidence` 的结束位置取「下一个归一化字节的原始偏移」（或文本末尾），因为「末字节 +1 会在匹配结束于多字节字符内部时把它切成两半」。

**源码事实：** `normalizeForMatch` 用**字节偏移**建映射，注释记了一个真实 bug：「每个 rune 记一条会在文本含多字节字符时让所有查找错位，于是 CJK 引文要么解析到错误 span 要么根本匹配不到」。

**源码事实：** `claimExtractionPrompt` 注释要求与模板声明的版本保持同步——「一旦分叉会静默改变树所归纳的内容，使内置版与模板版的树不可比」；prompt 里显式要求表格引文保留原始单元格/行文本与分隔符（「把表格行读成句子是 paraphrase，会被拒」）。

**源码事实：** `ExtractClaimsForChunks` 是**最佳努力**：失败的批不产 claim，树退回原始文本；无 LLM 客户端则完全禁用抽取。首个 LLM 调用之前先报扇出形状，因为「否则日志会在 building tree 上静默几十秒」。

**架构推论：** 整个三层结构是「用 MySQL 行锁做分布式互斥」的教科书式实现，而它成立的关键是**把「谁在处理」写成持久字段而不是内存状态**。`claim_owner` / `claim_token` / `claim_expires_at` 三个字段让「租约」变成可查、可续、可回收、可吊销的对象；而 token 的更换让「吊销」不需要广播——旧 worker 在写之前自证失效即可。判据是「互斥的判据能否落到持久存储」：能则不需要协调服务。

**架构推论：** 三种退出语义（取消 / 丢租约 / 批次失败）全都**不 ack**，这是同一个原则的三种表现：**ack 只表示「这批已经合并成功」**。把「不 ack」用于所有非成功路径，配合过期回收与幂等合并，就得到了「至少一次」的语义。代价是可能重复处理；收益是不需要「恰好一次」的分布式事务。

**架构推论：** 「只在本批文本内校验引文」这条约束的价值容易被低估。它同时解决了两个问题：① 归属正确性（引文只能在它真正来自的块里被定位）；② 校验成本（O(批大小) 而不是 O(文档)）。而它带来的限制是「跨块的 claim 无法被验证」——所以 prompt 里要求 claim 原子化（一句话一个事实），从源头上避免跨块。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/ingestion/knowledge_compile/scheduler.go` | `Publisher` | 生产者侧：Provision + Publish（入队与唤醒合一） |
| `internal/ingestion/knowledge_compile/scheduler.go` | `Claimer` | 消费者侧：领取 / 续租 / 进度 / 取消 / 行锁 |
| `internal/ingestion/knowledge_compile/scheduler.go` | `ClaimResult` | 数据集 + 租户 + 冻结的一批 + token |
| `internal/ingestion/knowledge_compile/scheduler.go` | `BacklogEntry` | 文档事件（`DocID` / 变体 / 任务类型） |
| `internal/ingestion/knowledge_compile/scheduler.go` | `newScheduler` | 生产构造（租约默认 2 分钟） |
| `internal/ingestion/knowledge_compile/scheduler.go` | `publishEntry` | gap lock + `FirstOrCreate` + 追加 + 状态迁移 |
| `internal/ingestion/knowledge_compile/scheduler.go` | `claimRow` | 唯一原子点：拒活租约 → 移批 → 盖租约 |
| `internal/ingestion/knowledge_compile/scheduler.go` | `TryClaim` | 先找空闲再回收过期（唯一显式 ReadCommitted） |
| `internal/ingestion/knowledge_compile/scheduler.go` | `findClaimableID` | 可领取数据集的选取与排序 |
| `internal/ingestion/knowledge_compile/scheduler.go` | `reclaimOne` | 过期租约回收（逐个加锁重判） |
| `internal/ingestion/knowledge_compile/scheduler.go` | `WithDatasetLock` | 行锁内执行 `fn` 并传 token（自死锁禁令） |
| `internal/ingestion/knowledge_compile/scheduler.go` | `Ack` | token 校验后删 inflight |
| `internal/ingestion/knowledge_compile/scheduler.go` | `notify` / `SubscribeNotify` | 最佳努力唤醒（失败退化为轮询） |
| `internal/ingestion/knowledge_compile/scheduler.go` | `FakeScheduler` | 测试替身（同接口 + 行级互斥） |
| `internal/ingestion/knowledge_compile/consumer.go` | `Consumer` | 消费者（scheduler / reader / writer / 租约参数 / 墓碑 / pause） |
| `internal/ingestion/knowledge_compile/consumer.go` | `NewConsumer` | 默认装配（含心跳与阈值） |
| `internal/ingestion/knowledge_compile/consumer.go` | `Run` | 三条唤醒路径的循环 |
| `internal/ingestion/knowledge_compile/consumer.go` | `processClaim` | 心跳 + 批次 goroutine + 三种退出 |
| `internal/ingestion/knowledge_compile/consumer.go` | `withWriteLock` | 锁内 token 校验（吊销的落地处） |
| `internal/ingestion/knowledge_compile/consumer.go` | `RebuildDataset` | 五步全量重建（关两个竞态） |
| `internal/ingestion/knowledge_compile/consumer.go` | `processBatch` | 按变体分派合并 |
| `internal/ingestion/knowledge_compile/writer.go` | `Writer` | 落盘接口（九方法各补一种存储缺口） |
| `internal/ingestion/knowledge_compile/writer.go` | `StructureBucket` | 数据集级结构合并单元（含 `VecCount`） |
| `internal/ingestion/knowledge_compile/writer.go` | `pageCommitter` | wiki 页提交的记录接口 |
| `internal/ingestion/knowledge_compile/writer.go` | `DeleteMergedWikiPages` | 先记移除再删（版本时间线可复原） |
| `internal/ingestion/knowledge_compile/writer.go` | `recordWikiPageRemovals` | 移除记录（形状列 + 批内一次归属查询） |
| `internal/ingestion/knowledge_compile/writer.go` | `datasetTokenizer` | 数据集级行与解析块共用分词器（同语言） |
| `internal/ingestion/component/knowledge_compiler/tree/claims.go` | `Claim` | 一条 claim + 逐字证据 |
| `internal/ingestion/component/knowledge_compiler/tree/claims.go` | `EvidenceRef` | 引文 + 归属块 + 校验后算出的偏移 |
| `internal/ingestion/component/knowledge_compiler/tree/claims.go` | `ExtractClaimsForChunks` | 最佳努力的批量抽取 |
| `internal/ingestion/component/knowledge_compiler/tree/claims.go` | `extractClaimsForBatch` | 单批抽取 + 归属恢复 |
| `internal/ingestion/component/knowledge_compiler/tree/claims.go` | `ValidateClaims` | 证据门（soft / hard）与定位结果统计 |
| `internal/ingestion/component/knowledge_compiler/tree/claims.go` | `LocateEvidence` | 空白归一化匹配 + 原始偏移换算 |
| `internal/ingestion/component/knowledge_compiler/tree/claims.go` | `normalizeForMatch` | 字节对齐的偏移映射 |
| `internal/ingestion/component/knowledge_compiler/tree/claims.go` | `EvidenceGateMode` / `ParseEvidenceGateMode` | 门的两种模式与容错解析 |
| `internal/ingestion/component/knowledge_compiler/tree/claims.go` | `claimExtractionPrompt` | 内置抽取契约（须与模板版本同步） |
| `internal/ingestion/component/knowledge_compiler/tree/claims.go` | `renderClaimSource` | 带 `[CHUNK_ID: x (TARGET)]` 标记的批文本 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 文档：RAPTOR 与知识编译](https://ragflow.io/docs/dev/configure_knowledge_base)
- [论文：RAPTOR——递归摘要树](https://arxiv.org/abs/2401.18059)
- [论文：ISC——用逐字引文约束 LLM 输出](https://arxiv.org/abs/2402.14445)

## 与主核 / AI 的关系

这套「租约 + inflight 表 + 过期回收」与主核（多模态数据库内核）的**行锁与事务**是同一目标的两个层级：主核的行锁在一个进程内、几十微秒量级；这里的租约跨进程、分钟量级。而两者的核心差别在于**持有者可能死亡**：数据库事务的持有者是活的连接，断了就回滚；这里的 worker 可能被 kill -9，所以必须靠**过期时间**让锁自动释放。

**这就是为什么它不能直接用数据库的行锁**：`SELECT FOR UPDATE` 持有的锁在进程消失后由连接断开释放，但一个「合并要跑五分钟」的任务不能靠一条长事务持锁——那样连接池会被打满，且崩溃后回滚的代价极大。租约用「短锁 + 长租约」把两个时间尺度分开：行锁只在「取批」与「写盘」这两个瞬间持有，中间的合并过程靠租约保护。

第二处对照是「先写 inflight 再删 backlog」与主核 WAL 的**「先写日志再改页」**。两者的顺序考虑完全一致：中间崩溃时宁可留下「多出来的记录」（可清理）也不要留下「丢失的记录」（不可恢复）。而幂等性是把重复当作可接受状态的前提。

第三处是「无法原子就靠可重建」与主核的**重组（VACUUM / CLUSTER）**。主核的 `CLUSTER` 也是「先建新的再换、崩溃后可能两者并存」——因为表可以被重建。两者都接受「中间态可能不完整」，前提是**产物能从源数据重算**。判据是「这个产物是不是派生数据」：派生则可重建，基底数据不可以。

对 RAG 工程的实际意义在于：多文档的「全局产物合并」是 RAG 系统里最容易被做成不可靠的一环——它天然需要串行，而串行的自然实现（一个进程独占）会变成单点。这份实现给出的三条经验可直接用：**把「谁在处理」写进持久字段**（内存状态无法跨进程吊销）、**token 之于租约如同版本号之于 CAS**（吊销靠自证而非广播）、以及**ack 只表示成功**（所有非成功路径留给回收 + 幂等重跑）。
