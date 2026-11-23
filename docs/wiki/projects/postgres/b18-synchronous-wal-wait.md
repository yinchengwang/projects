---
project: postgres
feature: synchronous-wal-wait
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/transam/xlogwait.c
    symbols: [WaitForLSN, WaitLSNWakeup, WaitLSNCleanup]
---

# WAL/LSN 等待与唤醒

## 一句话职责

LSN 等待器把 backend 对某条 WAL 进度线的等待注册到共享内存，由真正推进 replay/write/flush 位置的进程在越过目标时以 latch 唤醒它。

## 机制是什么

写入/回放事务会产生不同阶段的 WAL 进度：主库有 flush LSN，standby 有 replay、receive-write、receive-flush LSN。某些操作要等指定的 WAL 位置已达到阶段条件；若每个请求都扫描所有 backend，会让进度推进路径成本随 waiter 数量线性增长。`xlogwait.c` 为每种 `WaitLSNType` 维护一棵按目标 LSN 排序的 pairing heap，并缓存 `minWaitedLSN[type]`。唤醒者先以原子最小值做无锁快速排除，只有可能有 waiter 到期时才取 `WaitLSNLock` 处理 heap。

本页描述的是**等待指定 LSN 到达并唤醒**，不定义同步复制策略：哪些 standby 被认为同步、需要等待多少个副本、`synchronous_commit` 的策略都由 `syncrep.c`（WAL 发送/同步复制层）决定。`WaitForLSN` 的四种类型是 primary flush 与 standby replay/write/flush，不含「同步副本确认应用到 LSN」这种策略语义。它也不负责 WAL 生成、写盘或 redo 本身，推进进度的是 xlog writer、walreceiver 与 startup recovery；等待器只记录条件并睡眠。

共享结构 `WaitLSNState` 中，`procInfos[]` 每个 backend 一项（包含目标 `waitLSN`、类型、proc number、heap membership flag 与 heap node），`waitersHeap[]` 和 `procInfos[]` 在 `WaitLSNLock` 下维护；`minWaitedLSN[]` 是带 memory barrier 的 atomic 快速摘要。每进程最多等待一种 LSN 类型。等待者睡在自己的 `MyLatch` 上，因此唤醒是通知「重新检查进度」而非把条件结果直接交给它。

## 怎么实现的

1. **注册 waiter。** `WaitForLSN(type, targetLSN, timeout)` 确认共享状态已初始化、proc number 有效，先注册共享内存退出清理 callback（顺序保证它在 `ProcKill` 前移除 waiter）。`addLSNWaiter` 在 `WaitLSNLock` 排他锁下填入 `procInfos[MyProcNumber]`，把节点插入对应 heap，更新该类型最小目标 LSN 的原子缓存；解锁后留 injection point 供竞态测试。
2. **重查当前阶段位置。** 每轮用 `GetCurrentLSNForWaitType` 获取相应进度：replay 直接读 replay LSN；standby write/flush 取接收位置与 replay 位置两者的最大值，因为 replay 到达的 WAL 已存在于 base backup、归档还原或此前 streaming 数据中，不必要求 walreceiver 再接收一遍；primary flush 读本机 flush LSN。随后比对 `targetLSN <= currentLSN`，避免目标恰好在注册与睡眠之间到达造成丢失唤醒。
3. **睡眠及恢复结束处理。** 若仍未到目标，先处理 interrupt，再用 `WaitLatch(MyLatch, WL_LATCH_SET | WL_POSTMASTER_DEATH [| WL_TIMEOUT])` 等待。standby 型 wait 每轮还检查 `RecoveryInProgress()`；恢复结束时先删除 waiter，再重新读一次最终位置，只有 promotion 已触发且目标已达才返回 success，否则返回 `NOT_IN_RECOVERY`。postmaster death 返回 FATAL；deadline 到期后返回 timeout。
4. **进度推进者选择性唤醒。** `WaitLSNWakeup` 读取 `minWaitedLSN`；若它比当前进度还大就无需取锁。否则 `wakeupWaiters` 在排他锁下从 heap 最小值开始弹出所有已到期 waiter（每批最多 `WAKEUP_PROC_STATIC_ARRAY_SIZE`，源码常量为 16），标记 `inHeap=false` 并更新 min cache；释放锁之后才逐个 `SetLatch`，避免在共享锁内做较慢的唤醒操作。批次满时循环处理剩余 waiter。
5. **移除与 stale wakeup 复原。** 正常结束、timeout、recovery 结束、事务 abort 都调用清理；`WaitLSNCleanup` 依据本进程记录类型调用 `deleteLSNWaiter`，而 backend 退出 callback 兜底清理。由于 walreceiver 的 `writtenUpto/flushedUpto` 在新 timeline 或 streaming restart 时可能回退，已被 waker 移出 heap 的 waiter 可能收到 stale latch；`WaitForLSN` 发现 `inHeap=false` 就重新注册并重读进度，再决定睡眠或返回，避免遗漏重启后的新一轮推进。

真实唤醒者分属三类：`xlogrecovery.c` 每条 WAL record apply 后以 `lastReplayedEndRecPtr` 唤醒 standby replay/write/flush（replay 到达也意味着已写且已 flush）；`walreceiver.c` 收到并写入 WAL 后唤醒 standby write，fsync 并更新共享 flush 位置后唤醒 standby flush；primary `xlog.c` 在 flush 完成且释放高竞争 WAL 锁之后唤醒 primary flush。promotion 时传入 `InvalidXLogRecPtr` 表示清空该类型所有 waiter。

## 关键取舍

**按 LSN 排序的 heap 加最小值缓存，而不是每次进度变化扫描全体进程。** 插入/删除 waiter 需要 `WaitLSNLock`，但常态进度事件仅需一次原子比较；只有最小目标已到期才扫描 heap 顶部。放弃的是纯无锁的 heap 更新能力，换来总唤醒成本随「到期 waiter 数」而不是所有注册 waiter 数增长。代价是共享 heap 仍可能有锁竞争；`minWaitedLSN` memory barrier 只能用于跳过无关唤醒，不能替代 heap 锁。

**在 `WaitLSNLock` 下摘 waiter，但把 `SetLatch` 延后到解锁后。** 放弃的是状态变更与唤醒动作同一临界区的原子性，换取较短锁持有时间。源码注释证明这安全：`procLatch` 不会被释放，最坏只是误设置一个 latch 或设置到已经不再等待的 process；waiter 醒来会自己重新读当前 LSN 并检查条件。

**把 wakeup 定义成「提示重新检查」，并允许 waiter 重注册，而不是相信每次通知都代表条件单调满足。** standby receive 位置在新 timeline/stream restart 时会重置到较低点；已经被弹出 heap 的请求可能醒来时目标仍不满足。实现检测 `inHeap == false` 后重新插入并再读当前位置，代价是少数情况下额外一次锁与循环，换来不会把旧进度误当成新 timeline 的到达证明。相关退化路径真实 caller 是 `walreceiver.c` 的 `RequestXLogStreaming()`：源码注释点明重启会重种 `writtenUpto` / `flushedUpto`。

**promotion/恢复终止时清空 standby waiter，而不是让它无限等待。** `WaitLSNWakeup(type, InvalidXLogRecPtr)` 会跳过 fast-path 并移出该类型所有 waiter；waiter 再根据稳定的最终位置与 `PromoteIsTriggered()` 判断成功还是 `NOT_IN_RECOVERY`。放弃的是进程角色变化后继续等待同一 standby 进度的语义，换来明确终局与资源清理。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/transam/xlogwait.c` | `WaitForLSN` | 注册 waiter 并在 latch 循环中重读对应 LSN 进度，处理 timeout、promotion/recovery 终止、stale wakeup 重注册与 postmaster death |
| `src/backend/access/transam/xlogwait.c` | `WaitLSNWakeup` | 根据每种等待类型的原子最小目标做无锁排除，再调用 heap 批量摘除已满足项并在锁外 latch 唤醒 |
| `src/backend/access/transam/xlogwait.c` | `WaitLSNCleanup` | 从共享 pairing heap 中移除当前 backend 尚未完成的 waiter，供 abort 与 backend exit 路径确保不会留下悬挂 PGPROC 槽位引用 |

## 参考

- [PostgreSQL 文档：`WAIT` 命令](https://www.postgresql.org/docs/current/sql-wait.html) —— 用户选择 standby replay/write/flush 或 primary flush 的等待入口
- [PostgreSQL 文档：同步复制](https://www.postgresql.org/docs/current/warm-standby.html#SYNCHRONOUS-REPLICATION) —— 同步副本策略与等待指定进度机制的边界
- [PostgreSQL 官方源码：xlogwait.c](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/xlogwait.c) —— waiter heap、LSN 重查与 latch 唤醒实现

## 与主核 / AI 的关系

暂无直接对应。
