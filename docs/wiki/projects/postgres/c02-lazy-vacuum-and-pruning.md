---
project: postgres
feature: lazy-vacuum-and-pruning
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/heap/vacuumlazy.c
    symbols: [heap_vacuum_rel, lazy_scan_heap]
  - path: src/backend/access/heap/pruneheap.c
    symbols: [heap_page_prune_opt, heap_page_prune_and_freeze]
---

# Lazy VACUUM 与页内剪枝

## 一句话职责

Lazy VACUUM 分批扫描堆页并回收可安全处理的死版本，页内剪枝则在访问或维护时机整理单页中的 HOT 链和空间。

## 机制是什么

`heap_vacuum_rel` 初始化 `LVRelState`：收集关系与索引、冻结及可见性判定 cutoff、页数、死项存储和并行 vacuum 状态，然后调用 `lazy_scan_heap`。首次扫描按页检查元组，按需要回收或冻结，并收集仍需索引清理的 dead TID；当死项缓冲逼近上限时，先做一轮索引批量删除和堆页后处理，再继续扫描。扫描结束后再清理剩余 dead TID、更新 FSM，并完成索引 cleanup。

`heap_page_prune_opt` 是堆访问中的机会式入口：只在非 recovery、页上 `pd_prune_xid` 表明可能有可处理版本，且全局可见性测试允许回收、页空间触发启发式条件时尝试。它先 pin VM 页，再尝试非等待的 cleanup lock；竞争失败就放弃本次剪枝，不阻塞持有者。此页解释扫描/剪枝的触发、锁与流程，不讲 VM 位的具体存储表示或 MVCC 算法。

## 怎么实现的

1. `heap_vacuum_rel` 创建并填充 `LVRelState`，打开索引，按 vacuum 参数决定 index vacuum/cleanup、截断和 failsafe 行为；先取 `OldestXmin` 等 cutoff 与全局可见性测试状态，再确定 rel 扫描范围，确保后续冻结 XID 统计不会漏掉扫描范围内旧事务。
2. `lazy_scan_heap` 建立 maintenance read stream。回调根据 vacuum 模式、VM 状态及阈值选择可跳过页或必须访问页；遇 wraparound failsafe 时会清掉 read stream 的 buffer strategy，之后不再限制于原策略。
3. 每取一页，VACUUM pin 对应 VM 页，尝试 `ConditionalLockBufferForCleanup`。成功时可完整执行 `lazy_scan_prune`；失败时降级到共享锁下的 `lazy_scan_noprune`，仅收集可安全统计的内容。若 aggressive vacuum 因 XID/MXID 推进要求不能降级完成，则释放共享锁并等待 cleanup lock，再完整处理。失败/新页等分支会释放 page pin/锁并按需要更新统计。
4. 完整处理把 dead TID 累积到受内存预算控制的 `TidStore`。达到预算即暂停 heap 首轮扫描，先执行 `lazy_vacuum`：索引 AM 批量删除对应索引项，再对堆页执行回收；结束后继续扫描。这样不要求把全表死 TID 一次性放入内存。
5. `heap_page_prune_opt` 先用 `PageGetPruneXid` 与 `GlobalVisTest` 快速排除不可能剪枝页；空间阈值使用无锁页读作为启发式。满足条件后 pin VM 页，并用条件 cleanup lock；拿锁后重新检查空间，避免按陈旧估计执行。持锁时填充 `PruneFreezeParams`，以 `PRUNE_ON_ACCESS` 原因调用统一处理例程。
6. `heap_page_prune_and_freeze` 在 buffer cleanup lock 与所需 VM pin 下先规划状态变化，再进入 critical section 应用 line pointer 改动、冻结和必要的 VM/page 标记，并将堆页与 VM 变化纳入相应 WAL 记录。它计算新 prune horizon 并返回回收计数；机会式剪枝结束后释放锁，只有 newly-all-visible 等确需场景才更新 FSM。

## 关键取舍

- 源码事实：机会式入口用 `ConditionalLockBufferForCleanup`，拿不到锁便不等待；VACUUM 扫描同样优先尝试 cleanup lock，失败时以 `lazy_scan_noprune` 降级，aggressive vacuum 必须完成时才等待。架构推论：选择让普通访问避免被长时间清理锁拖住，代价是本轮可能少回收一些版本；强制每次访问阻塞等待会提高即时回收率，却可能把扫描延迟放大为锁等待。具体退化 caller 是 `heap_page_prune_opt` 自身的条件锁失败分支及 `lazy_scan_heap` 中 `lazy_scan_noprune` 路径。
- 源码事实：dead TID 有 `TidStoreMemoryUsage` 上限，达到预算就先调用 `lazy_vacuum` 清理索引和堆，再继续第一遍扫描。架构推论：两阶段批处理限制峰值内存，代价是可能多轮扫描索引/堆并增加 I/O；若全程保存所有死 TID 可减少阶段切换，但内存用量随关系死项规模增长。
- 源码事实：剪枝规划与实际页修改分开，实际应用落在 critical section；必要时 WAL 记录同时携带 heap/VM 变化。架构推论：把复杂判定放在不可中断修改区之外，便于保证 redo 对应真实状态转换；若先改页再逐项决定，错误或中断更难维持页与 WAL 一致。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/heap/vacuumlazy.c` | `heap_vacuum_rel` | 建立 vacuum 关系状态、cutoff 与索引工作，并启动 lazy scan。 |
| `src/backend/access/heap/vacuumlazy.c` | `lazy_scan_heap` | 通过 read stream 扫页、争取 cleanup lock、积累死 TID，必要时穿插索引/堆清理轮次。 |
| `src/backend/access/heap/pruneheap.c` | `heap_page_prune_opt` | 以 prune horizon 和页空间作触发条件，非阻塞地尝试对当前 heap page 剪枝。 |
| `src/backend/access/heap/pruneheap.c` | `heap_page_prune_and_freeze` | 规划并应用单页 line pointer、冻结和相关可见性状态变更。 |

## 与主核 / AI 的关系

主核没有 PostgreSQL heap tuple 版本队列或 lazy VACUUM 的直接等价物。可借鉴的是维护工作按内存预算分批推进、短期访问机会式清理、锁竞争时退化而非无条件等待的工程模式；不可把这些视为主核对象回收或事务语义。

## 参考

- [PostgreSQL 文档：VACUUM](https://www.postgresql.org/docs/current/sql-vacuum.html) —— 说明 VACUUM 的 SQL 选项及其作为例行维护的作用。
- [PostgreSQL 官方源码：lazy VACUUM 扫描](https://github.com/postgres/postgres/blob/master/src/backend/access/heap/vacuumlazy.c) —— 支撑扫描、死项缓存及分阶段清理流程。
