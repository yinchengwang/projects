# A/B/C 组抽检报告

> 执行者：独立审阅者（非 A/B/C 三组页面 writer）
> 源码锚点：`postgres@9b5d4e3331753b3399e84965093109cac8f364bf`
> 评分依据：P3 计划 §4.2 的六项评分表（机制边界 / 实现映射 / 机制深度 / 关键取舍 / 来源质量 / RAG 切块），每项 0/1
> 抽样页：A 组 5 页、B 组 5 页、C 组 5 页，共 15 页

## 评分表

| 页 | 边界 | 映射 | 深度 | 取舍 | 来源 | 切块 | 合计 | 主要问题 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| a05-lock-manager-deadlock.md | 1 | 1 | 1 | 0 | 1 | 1 | 5 | 取舍无源码/权威支持，且漏掉死锁检测的真实触发点 |
| a10-lwlocks.md | 1 | 1 | 1 | 1 | 1 | 1 | 6 | — |
| a13-dynamic-shared-memory.md | 1 | 1 | 1 | 1 | 1 | 1 | 6 | — |
| a16-free-space-map.md | 1 | 1 | 1 | 1 | 1 | 1 | 6 | — |
| a19-parallel-query-dsm.md | 1 | 1 | 1 | 0 | 1 | 1 | 5 | 取舍不足两项；回退路径未点名真实 caller |
| b01-wal-insertion-and-write.md | 1 | 1 | 1 | 1 | 1 | 1 | 6 | — |
| b05-standby-recovery-conflict.md | 1 | 1 | 1 | 1 | 1 | 1 | 6 | — |
| b08-transaction-status-slru.md | 1 | 1 | 1 | 1 | 1 | 1 | 6 | — |
| b13-transaction-lifecycle-and-status.md | 1 | 0 | 1 | 1 | 1 | 1 | 5 | `TransactionIdDidAbort` 的子事务语义描述错误 |
| b17-wal-archive-backup-interfaces.md | 0 | 0 | 1 | 1 | 1 | 1 | 4 | 正文扩写到归档模块，超出 refs 与本页机制边界 |
| c02-lazy-vacuum-and-pruning.md | 1 | 1 | 1 | 1 | 1 | 1 | 6 | — |
| c06-tuple-descriptor-layout.md | 1 | 1 | 1 | 1 | 1 | 1 | 6 | — |
| c10-synchronized-sequential-scan.md | 1 | 1 | 1 | 1 | 1 | 1 | 6 | — |
| c12-heap-wal-redo.md | 1 | 1 | 1 | 1 | 1 | 1 | 6 | — |
| c15-heap-index-fetch.md | 1 | 1 | 1 | 1 | 1 | 1 | 6 | — |

满分页 **12/15**；≥3 项扣分页 **0**；无评分项在 ≥3 页同时失败，**未达系统性缺陷门槛**。

## 逐页问题

### 严重

**b13-transaction-lifecycle-and-status.md 第 25、35、55 行**（原「只判断明确记录的 abort 状态」）
源码对照：`src/backend/access/transam/transam.c` 的 `TransactionIdDidAbort` 对
`TRANSACTION_STATUS_SUB_COMMITTED` 会经 `SubTransGetParent` 递归判断父事务；XID 早于
`TransactionXmin` 时直接返回 true；取不到父 XID 时 elog WARNING 后同样返回 true。只有普通
`TRANSACTION_STATUS_ABORTED` 分支是基于显式 abort 标记。crash 后仍为 `IN_PROGRESS` 的状态返回 false。
因此「只判断明确记录的 abort 状态」不适用于整个函数，映射项记 0。

**b17-wal-archive-backup-interfaces.md 第 23、46、48–54 行**
正文详述 `pgarch_archiveXlog` 的 `sigsetjmp` 异常处理、归档回调重试、shell `system()` 退出码策略与
archive command/library 互斥，但 front-matter 只有 `xlogfuncs.c` 的三个 SQL 符号，源码落点表也只列这三个。
上述行为实际位于 `src/backend/postmaster/pgarch.c` 与 `src/backend/archive/shell_archive.c`。正文事实可核，
但 refs 未映射该部分，边界与映射各记 0。

### 中等

**a05-lock-manager-deadlock.md 第 35 行**：把「冲突立即失败」「不检测则永久等待」「按需搜索减少常态成本」
并列陈述，未区分源码事实与架构推论，也未给出触发检查的真实 caller。源码显示 `DeadLockCheck` 可重排等待队列，
无法消解时返回 `DS_HARD_DEADLOCK`；检查由 `proc.c` 的 `CheckDeadLock` 在 `DEADLOCK_TIMEOUT` 超时后调用。
取舍项记 0。

**a19-parallel-query-dsm.md 第 35 行**：列出 DSM+TOC 与复制 leader 地址空间、固定 C 结构的比较，但未明确区分
源码事实与架构推论，未充分给出两项有证据的取舍。`InitializeParallelDSM` 在无法处理中断、session DSM handle
不可用或 `dsm_create` 返回 NULL 时将 `nworkers` 置 0 并可用 backend-private TOC；`LaunchParallelWorkers` 注释
明确要求调用方容忍实际启动数少于请求数。文中「具体 caller 决定」未点名 caller，取舍项记 0。

### 轻微

**a10-lwlocks.md 第 54 行**：「等待公平性」是类比建议，不是 PostgreSQL LWLock 的已实现保证；源码
`LWLockRelease` / `LWLockWakeup` 是唤醒后重新尝试获取，不是直接授锁。页面第 27 行已正确说明醒后重新竞争，
故不构成实现语义错误。

**b17 第 46 行**：归档命令信号/非零退出的处理策略属未映射到本页 refs 的实现断言；参考链接覆盖用户文档，
未覆盖 vtable / `sigsetjmp` / signal exit-level 的具体实现。

## 重点错误核查

- **符号名当语义**：未发现已核验核心符号的函数体与页面职责说明存在实质冲突。
- **数字无出处**：b01 的 `NUM_XLOGINSERT_LOCKS = 8` 对应 `xlog.c` 宏；a13 的 DSM 引用计数对应 `dsm.c`
  结构注释与创建/销毁逻辑；FSM 上限来自 `MaxFSMRequestSize` 宏；c10 的 `SYNC_SCAN_NELEM` 与报告间隔对应
  `syncscan.c` 常量。
- **架构推论冒充源码事实**：本次抽样页多以「源码事实 / 架构推论」分开标注，未发现明显冒充。

## 来源核验限制

已抽查的 PostgreSQL 官方文档链接返回 HTTP 200；其余链接未逐一发 HTTP GET（脚本批量 HEAD 在非交互
PowerShell 下报环境错误）。因此来源分只代表抽查链接可达且来源权威、与论点相关，**不声称全部外链均验证可达**。

## 处理结果

本报告的三处扣分项已按固定 checkout 源码修订并复跑双门禁：

- `a05`：把「等待期间触发」收窄为 `DEADLOCK_TIMEOUT` 一次性超时驱动，补 `ProcSleep` 循环里
  `got_deadlock_timeout` 才调 `CheckDeadLock` 的触发点，取舍段改为「源码事实 / 架构推论」分列。
- `a19`：补三条 `nworkers = 0` 的源码事实（无法处理中断、拿不到 session DSM handle、`dsm_create` 返回 NULL），
  并说明第三种情况下 TOC 建在 `TopMemoryContext` 私有内存上、结构不变只是无 worker，取舍改为事实与推论分列。
- `b13`：把「只认显式 abort」改为三条判定路径（显式 aborted 直接 true；sub-committed 递归查父、XID 早于
  `TransactionXmin` 或无父登记时也判 true；crash 遗留 `IN_PROGRESS` 判 false）。
- `b17`：删去归档模块实现细节，收敛为「归档侧不在本页展开，属 i10 机制范围」的边界声明，并把该取舍项替换为
  本页 refs 可覆盖的「备份状态挂在会话上」取舍。

修订后：`wiki_lint` 与 `wiki_refs` 目录模式均为「检查 152 页，失败 0 页」。
