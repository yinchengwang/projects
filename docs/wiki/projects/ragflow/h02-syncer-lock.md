---
project: ragflow
feature: syncer-lock
title: 同步锁与检查点
page_type: feature
schema_version: 1
refs:
  - path: internal/syncer/connector_lock.go
    symbols: [ConnectorLock, NewConnectorLock, TryLock]
  - path: internal/syncer/checkpoint_store.go
    symbols: [SyncCheckpointStore, LoadSyncCheckpoint]
---

# 同步锁与检查点

## 一句话职责

这两个机制共同保证「同一个连接器与知识库的组合在同一时刻只被一个执行者推进，且推进到哪里是可恢复的」：`ConnectorLock` 用「进程内 map + Redis 租约」两层锁做到互斥，`SyncCheckpointStore` 用一个可替换的存储后端做到断点续跑。

## 机制是什么

同步任务的本质是「从上游按窗口拉数据、逐条写库」。这两件事各自都需要一个保证：

**互斥：** 同一个连接器 + 同一个知识库不能被两个 worker 同时推进。否则两个执行者会各自持有一份自己以为正确的进度，重复拉同一批文件、重复写入。`ConnectorLock` 锁的粒度正是「连接器 × 知识库」这个二元组——不是连接器（同一连接器可以往多个库同步），也不是任务（同一组合可能因为重试而出现多个任务）。

**可续：** 一个同步窗口可能包含上万个文件，跑到一半进程被杀是常态。`SyncCheckpointStore` 保存「当前窗口 + 游标 + 已提交到第几批」，恢复时从断点继续而不是从窗口开头重来。

两者被刻意做成独立的两件事：锁管「同时」，检查点管「之后」。锁释放了不代表进度丢了，检查点还在；检查点过期了也不代表可以并发跑。

## 怎么实现的

**锁是两层的，且顺序固定：先本地、再分布式。**

`ConnectorLock` 有三个字段承担这件事：`holder`（进程级 UUID，`NewConnectorLock` 时生成一次）、`local`（`map[string]struct{}`，进程内已占用的 key）、`redis`（`map[string]*kvrocks.DistributedLock`，已获得的分布式锁句柄）。

`TryLock(connectorID, kbID)` 的流程：

1. 拼 key：`syncer:connector-lock:<connectorID>:<kbID>`。
2. **先在 `mu` 保护下查 `local`**：命中就返回「没拿到」。未命中就把 key 写进 `local` 并放锁。
3. 查 `kvrocks.Get()`：全局客户端为空时直接返回「拿到了」——**这是纯进程内的降级模式**，本地 map 就是全部互斥能力。
4. 客户端存在时创建 `DistributedLock(key, holder, connectorLockTTL, 0)`：TTL 24 小时，阻塞超时 0（不等待）。创建失败（`globalClient` 为空时构造函数返回 `nil`）要回滚本地条目并返回错误。
5. `Acquire` 出错要回滚本地条目并包装错误返回；`Acquire` 返回 `false`（别人持有）同样回滚并返回「没拿到」。
6. 成功了才把锁句柄存进 `redis` map，返回租约。

**注意第 2 步的本地占位发生在分布式尝试之前。** 这是一个刻意的顺序：两个 goroutine 同时抢同一个 key 时，只有一个能写进 `local`，另一个立刻失败，于是根本不会去打扰 Redis。代价是**本地条目必须在所有失败分支上回滚**——代码里三处 `delete(l.local, key)` 正是为此，少一处就会把 key 永久占住。

**`holder` 是进程级而不是任务级。** 这个细节决定了崩溃恢复的行为。`Acquire` 的实现里第一步是 `DeleteIfEqual(lockKey, lockValue)`——先删掉「本 holder 留下的同 key 锁」，再 `SetNX` 抢。所以一个进程重启后（新 `holder`）不会误删别人的锁，而同一进程内重复抢同一个 key 会先清掉自己上次的残留。若某个进程在持锁期间被杀，Redis 里会留下一条 24 小时后才过期的锁，**新进程只能等它过期或由运维清理**。

**租约里的 `ExpiresAt` 是算出来的，没有续约机制。** `newConnectorLockLease` 返回 `time.Now().Add(connectorLockTTL)`，即「此刻 + 24 小时」。全仓没有对 `DistributedLock` 做续期的地方，所以这个时间只是「理论上限」而非「实际保证」：一个跑超过 24 小时的同步任务，其 Redis 锁会自然过期，此时另一个执行者就能拿到锁并并发推进。**互斥保证的有效期等于锁 TTL，而任务时长不受它约束**——这是本例最重要的一条边界。

**`Unlock` 先删本地再释放远端，且释放失败只返回布尔值。** 实现里在同一把锁内取走 `redis[key]` 与 `local[key]`，然后（如果拿到句柄）调 `Release`。`Release` 的返回值被丢弃——因为 `Unlock` 的签名没有错误返回。所以「释放分布式锁失败」是静默的，只能等 TTL 过期。

**nil 接收者是安全的。** `TryLock` 与 `Unlock` 开头都判 `l == nil`，分别返回「没拿到」与直接返回。这与 Syncer 各处的 nil 安全风格一致。

**检查点存储是一个三方法接口，默认实现全在内存。**

`SyncCheckpointStore` 只有 `LoadSyncCheckpoint` / `SaveSyncCheckpoint` / `DeleteSyncCheckpoint` 三个方法，全部带 `context.Context`、按 `taskID` 存取。`memorySyncCheckpointStore` 是唯一的开源实现：一把 `sync.Mutex` 加一个 `map[string]SyncCheckpointState`。

**三个方法都先查 `ctx.Err()`。** 这是「已取消的上下文不执行任何副作用」的硬性前置：`Load` 返回错误、`Save` 不写、`Delete` 不删。

**深拷贝是双向的，这是本实现的核心。** `cloneSyncCheckpointState` 把 `WindowStart` 与 `Checkpoint` 两个指针字段各复制一份（`Checkpoint` 里嵌套的 `UpdatedAt` 也单独复制）。`Save` 存的是拷贝，`Load` 返回的也是拷贝。于是：

- 调用方修改返回值不会污染存储里的状态；
- 调用方保存后再改自己的结构体，存储里的状态不变。

少了任何一侧的拷贝，都会出现「我改了本地变量，存储里悄悄变了」这种极难排查的串档。

**`Load` 找不到时返回 `(nil, nil)` 而不是错误。** 这个约定让调用方能用 `if state != nil` 区分「没有断点」与「读失败」，而不用靠错误字符串判断。

**存储后端是可替换的，选择发生在 `Syncer` 构造期。** `Syncer.New` 先建内存实现，再看消息队列引擎是否实现 `SyncCheckpointStore` 接口——实现了就换掉。所以「检查点存在哪」由部署选择决定：内存实现意味着**跨进程不共享**，只有同进程内的重试能续跑。

**调用方如何使用检查点，决定了它的语义边界。** 从 `SyncRunner` 的用法能看出三件事：

- `prepareCheckpoint` 读出来的状态要**先验身份再采纳**：`TaskID`、`ConnectorID`、`KBID` 三者全等且 `WindowEnd` 非零才算有效续跑点，否则视为不存在并新建初始状态（`Version=1`、`NextCommitSeq=1`）。这把「上一次的残留状态属于另一个任务」这种情况挡在外面。
- `resetCheckpointForRestart` 处理「保存的恢复锚点已失效」（上游游标过期）：`RestartCount` 加一、清空 `Checkpoint`、`NextCommitSeq` 归 1、四个计数清零，但**保留窗口**（`WindowStart`/`WindowEnd` 不动）。也就是「同一个时间窗重扫一遍」，而不是开启新窗口。
- 完成时 `DeleteSyncCheckpoint` 用的是 `context.WithoutCancel(ctx)`——把任务本身的取消从清理动作里摘掉。删除失败只记 warning 与一条任务事件，不影响任务被判成功。

## 关键取舍

**源码事实：** `TryLock` 先写 `local` 再抢 Redis，所有失败分支都要回滚本地条目；漏一处就会把 key 永久占住，且因为 `Unlock` 也从同一个 map 删，只能靠 `Unlock` 补救。

**源码事实：** `holder` 是 `NewConnectorLock` 生成的进程级 UUID，`Acquire` 前先 `DeleteIfEqual` 清掉本 holder 的残留，因此同进程内重抢不会自锁，跨进程则依赖 TTL 过期。

**源码事实：** 锁 TTL 是 24 小时，`ConnectorLockLease.ExpiresAt` 只是 `now + TTL`，全仓没有续约逻辑，`Unlock` 也不返回释放失败。

**源码事实：** 检查点存储在 `Save` 与 `Load` 两侧都做深拷贝（含嵌套指针），`Load` 缺失返回 `(nil, nil)`，三个方法都先查 `ctx.Err()`。

**源码事实：** 检查点是否被采纳由调用方校验三个 ID 全等 + `WindowEnd` 非零决定；重启重置保留时间窗、只清游标与计数并递增 `RestartCount`。

**架构推论：** 「进程内 map + 分布式租约」是这类互斥的标准两层结构，但它带来一个隐性成本：**本地 map 是权威的「我已经拿了」，Redis 是权威的「别的进程没拿」**。两者不一致时（例如 Redis 抢到但本地已被别人占）必须以后者失败为准并回滚前者。所以这套实现里所有 Redis 失败路径都在回滚本地——方向是对的。

**架构推论：** 锁的粒度选「连接器 × 知识库」而不是「任务」，意味着**同一组合的多个任务会自然串行化**。这正是期望的行为：重试产生的新任务会撞上旧任务的锁，被推迟而不是并发跑。代价是锁的持有时间等于整个任务时长，一旦任务卡住，同组合的新任务就全被挡住。

**架构推论：** 检查点接口只有三个方法、没有版本迁移、没有 CAS（比较并设置），因此它假设「同一 taskID 只有一个写者」。这个假设由锁保证——**两个机制在这里是耦合的**：去掉锁，检查点的 `Save` 就会变成后写覆盖先写，`NextCommitSeq` 的递增也会失效。读这两页必须一起看，单独看任何一个都会高估它的保证。

**架构推论：** 记录 `RestartCount` 而不无限重试（上限来自 `MaxAnchorRestartCount`），把「锚点失效」从「偶发故障」分类成「可能持续的状态」。这是正确的：如果上游每次都让锚点失效，无限递归重启只会烧掉配额。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/syncer/connector_lock.go` | `ConnectorLocker` | 锁的能力接口：TryLock + Unlock |
| `internal/syncer/connector_lock.go` | `ConnectorLock` | 本地 map + 分布式锁句柄的两层结构 |
| `internal/syncer/connector_lock.go` | `NewConnectorLock` | 生成进程级 holder UUID |
| `internal/syncer/connector_lock.go` | `TryLock` | 先本地占位、再抢 Redis，失败逐层回滚 |
| `internal/syncer/connector_lock.go` | `ConnectorLock.Unlock` | 双 map 清理 + 忽略释放失败 |
| `internal/syncer/connector_lock.go` | `ConnectorLockLease` | 租约：现在 + 24 小时，无续约 |
| `internal/syncer/connector_lock.go` | `connectorLockKey` | 锁键：连接器 + 知识库二元组 |
| `internal/syncer/checkpoint_store.go` | `SyncCheckpointStore` | 检查点三方法接口 |
| `internal/syncer/checkpoint_store.go` | `LoadSyncCheckpoint` | 读检查点，缺失返回 `(nil, nil)` |
| `internal/syncer/checkpoint_store.go` | `memorySyncCheckpointStore` | 内存实现，读写两侧深拷贝 |
| `internal/syncer/checkpoint_store.go` | `cloneSyncCheckpointState` | 嵌套指针的逐层复制 |
| `internal/syncer/sync_runner.go` | `prepareCheckpoint` | 三 ID 校验后采纳或新建 |
| `internal/syncer/sync_runner.go` | `resetCheckpointForRestart` | 保留窗口、清游标、递增重启计数 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：syncer/connector_lock.go](https://github.com/infiniflow/ragflow/blob/main/internal/syncer/connector_lock.go)
- [Redis 文档：SETNX 与分布式锁](https://redis.io/docs/latest/commands/setnx/)
- [Kvrocks 文档](https://kvrocks.apache.org/docs/)

## 与主核 / AI 的关系

「锁 + 检查点」的组合与主核里「页锁 + WAL」的分工几乎一一对应：锁保证同一时刻只有一个写者，日志保证中断后可重放。差别在于**保证强度**：主核的锁与日志都在同一个进程内、由同一个引擎统一管理，因此可以做到「崩溃后自动回放到最后一条已提交记录」；这里的锁在 Redis、检查点在另一个可替换后端，两者之间没有原子性——可能出现「锁已释放但检查点没写」或反过来。

**这正是「分布式系统里没有免费的原子性」的具体样子。** 主核能保证「要么全成要么全败」，是因为它拥有全部状态；这里把状态拆到 Redis 与检查点后端两处，就只能退化成「各自尽力 + 靠 TTL 与重试兜底」。

`DeleteIfEqual` 这个做法值得单独对照主核的版本校验：两者都是「只有我持有的那份还生效时才操作」。主核用版本号做 CAS，这里用 `holder` 字符串做相等比较。前者的好处是单调可比较（能判断新旧），后者只能判断相等——所以它无法区分「我的锁被清理了」与「我的锁从来不存在」，只能统一处理成「删一下试试」。

对 agent / RAG 工程的实际含义有三条。其一，**检查点的可靠性取决于后端选择**：默认的内存实现在进程重启后就丢，只有同进程内重试能续跑；要跨进程续跑必须让消息队列引擎实现那个接口。其二，**互斥的有效期是 24 小时**，超长同步任务会失去互斥保护，设计分片或分批时应把单任务时长控制在 TTL 之内。其三，`RestartCount` 上限是「锚点失效」的熔断器，调大它不会让失效的上游变可用，只会让失败来得更晚。
