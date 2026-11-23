---
project: postgres
feature: wal-archive-backup-interfaces
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/transam/xlogfuncs.c
    symbols: [pg_backup_start, pg_backup_stop, pg_walfile_name]
---

# 备份与归档接口

## 一句话职责

`xlogfuncs.c` 把在线备份起止、WAL 文件名解析等能力暴露为 SQL 函数，并将会话级输入校验、备份上下文与底层 WAL 协调流程接起来。

## 机制是什么

对 base backup 而言，外部工具需要一对可配对的开始/结束操作：开始时取得 restore 所需起点信息、强制建立正确 checkpoint/FPI 边界并生成 `backup_label` 所需状态；结束时取得 stop LSN、timeline、label 与 tablespace map，并可选等待归档。SQL 函数 `pg_backup_start` / `pg_backup_stop` 只做接口适配；实际协调工作由 `xlog.c` 的 `do_pg_backup_start` / `do_pg_backup_stop` 完成。`xlogfuncs.c` 明确说明 `pg_backup_stop` 返回的 `backup_label` 与 `tablespace_map` 由调用者写入备份目标目录，数据库端不会替外部工具写这两个文件。

本页只负责**面向 SQL/运维的备份接口和回调生命周期**，不讲 WAL writer 如何缓存、写文件或 flush（b01），也不讲 restore/recovery 从 archive 取回 WAL 的流程（b03）。`pg_walfile_name` 也不只是备份工具的附属：它把给定 LSN 映射到当前 timeline 与 segment 大小下的 WAL 文件名，不切段、不归档、不下载，也不验证目标文件是否已经存在。

归档侧是另一条独立链路，**不在本页展开**。`archive_command` 与 `archive_library` 是同一个归档出口的两种实现选择（同时设置会被拒绝），由 archiver 进程按 `.ready` 状态调用模块回调执行——那套状态机、重试策略与回调异常处理属 `i10-archive-and-base-backup` 的机制范围，本页只负责 SQL 侧的备份起止与WAL 文件名换算。本页提到的「归档完成」仅指 `pg_backup_stop(waitforarchive => true)` 会等待所需 WAL 段被归档，不表示归档目标一定已持久保存。

## 怎么实现的

1. **开始函数校验 session 状态并建立长生命周期数据。** `pg_backup_start` 把 SQL label 转成 C 字符串，调用 `get_backup_status()` 拒绝同一 session 已经运行的另一个 backup；`backup_state` 与 `tablespace_map` 放入 `TopMemoryContext` 子级的 `backupcontext`，因为它们要跨 SQL 调用存活到 `pg_backup_stop`。然后注册 persistent abort handler，调用 `do_pg_backup_start`，返回 startpoint LSN。
2. **底层 start 在共享 WAL 状态中置位。** `do_pg_backup_start` 在所有 WAL insertion locks 持有期间递增 `runningBackups`，以与 `XLogInsertRecord` 互锁；这确保在线备份期间启用 full-page writes，且强制 checkpoint 先清掉可能被拷走但没有相应 WAL 的脏页。它在主库侧先切 WAL segment，之后发 `CHECKPOINT_FORCE | CHECKPOINT_WAIT`（可按 fast 请求加 `CHECKPOINT_FAST`），读取 checkpoint location、redo startpoint、timeline 和 full-page-writes 状态，再构造 tablespace map 与 backup state。失败路径由 `PG_ENSURE_ERROR_CLEANUP(do_pg_abort_backup, ...)` 撤销 running-backup 状态。
3. **stop 校验与解除 session backup 状态。** SQL `pg_backup_stop` 要求当前 session 状态为 `SESSION_BACKUP_RUNNING`，再调 `do_pg_backup_stop`。底层在 WAL insertion lock 保护下递减 `runningBackups` 并清 `sessionBackupState`；注释强调这段不能插入 `CHECK_FOR_INTERRUPTS()`，否则异常退出可能让共享计数与 session 状态不一致。
4. **主库与 standby 的停止信息不同。** 主库写 `XLOG_BACKUP_END`，以 startpoint 与 segment 切换形成可识别的结束位置，再建立 backup history 文件；可选 `waitforarchive` 等待相关 WAL segment 被归档。standby 不写 end-of-backup WAL record，返回 `pg_control` 的 minimum recovery point；同时验证 backup 期间没有 promotion，也检查恢复到的 WAL 都在 full_page_writes 保护下，否则报错并指出该备份不可用。
5. **SQL 返回后释放只属于接口层的内存。** `pg_backup_stop` 用 `backup_state->stoppoint`、label 与 tablespace map 组复合返回值，再将 session variables 置空并删除 `backupcontext`。start/stop 的配对约束由底层 `sessionBackupState` 执行；调用 `pg_backup_start` 后必须在同一 session 成功 stop，或使用 abort handler 清理。
6. **WAL 文件名函数只做位置换算。** `pg_walfile_name` 在 recovery 中拒绝调用；否则按 `wal_segment_size` 计算 segment number，再用 `GetWALInsertionTimeLine()` 与 `XLogFileName` 生成当前 insertion timeline 的 24 位文件名。邻近的 `pg_walfile_name_offset` 还返回段内 offset。函数不访问归档目录，也不检查传入 LSN 是否已写出。

并发边界是全局备份计数与每 session 状态两层：前者由 WAL insertion lock 保护，避免在线写 WAL 与开关 FPI 发生竞态；后者由当前调用 session 的备份状态管理。底层函数注释把权限检查明确交给 caller；SQL 权限则由常规 GRANT 机制控制。

## 关键取舍

**开始在线备份时强制 checkpoint 与 FPI，而不是只返回一个 LSN。** 备份工具会并发复制数据页，页面可能恰好被写到一半而形成 torn page。源码注释说明，只有 backup interval 的首次 WAL 写入带 full-page image，redo 才能修复这种部分页；checkpoint 则保证开始点前的 dirty page 不会在 backup 期间被复制、却没有相应 WAL。代价是 backup start 可能触发较重的 checkpoint 与 WAL 切段；更轻的方案不能满足页面一致性约束。

**返回 label/map 内容而不直接创建文件。** `pg_backup_stop` 把 `backup_label` 与 `tablespace_map` 作为 SQL 结果返回，源码明确由调用者负责把文件写进将被 restore 的目录。放弃的是数据库端对所有备份工具输出路径的统一控制，换来备份接收器可写入自己的临时目录、远端对象存储或流式目标；替代方案会让 server 进程承担任意目标文件写入与权限管理。

**主库写 backup-end 记录并要求唯一 startpoint；standby 不写同类记录。** 主库要让后续 restore 能从 WAL/backup history 匹配该次备份，所以并发备份会通过 `lastBackupStart` 与再次 checkpoint 确保不同起点；注释承认替代方案是给每个 backup 生成独立唯一 ID，但当前认为多一次 checkpoint 成本可接受。standby 则没有正常 WAL insertion，依靠 `pg_control` 的 minimum recovery point；放弃的是相同历史文件/结束记录语义，因此其 backup stop 会额外验证 promotion 与 FPI 条件。真实分支 caller 即 `do_pg_backup_stop` 的 `RecoveryInProgress()` 判断。

**`pg_walfile_name` 拒绝 recovery 状态下调用，而不是尝试从 replay timeline 猜文件。** 函数明确 `ereport(ERROR)`，因为函数使用的是当前 WAL insertion timeline 与 segment size；恢复期间这不等于安全可用的读/archive timeline。放弃的是 standby 上查询任意 LSN 对应 WAL 文件名的便利，换来不让调用方误把本机 insertion filename 当成归档恢复文件。该回退由真实 caller `pg_walfile_name` 本体中的 recovery 检查执行。

**备份状态挂在会话上（`TopMemoryContext` 子上下文）而不是每次 SQL 调用独立。** 源码事实：`pg_backup_start` 把 `backup_state` 与 `tablespace_map` 放进跨调用存活的 `backupcontext`，并注册 persistent abort handler；`pg_backup_stop` 结束时才删除该上下文。放弃的是「无状态、每次调用各取所需」的简单接口形态——那要求调用者自己保管 startpoint 与 tablespace map，而外部备份工具通常在两个进程/两段脚本里分别执行 start 与 stop，状态无处安放。代价是 server 必须为每个会话维护这份内存，并在会话异常退出时靠 abort handler 撤销 `runningBackups` 计数；若只在正常 stop 路径递减，客户端中途断开就会让进程永久处于备份中状态。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/transam/xlogfuncs.c` | `pg_backup_start` | SQL 开始接口：拒绝重复 session backup、创建跨调用存活的 `BackupState`/tablespace map、注册 abort 清理回调并调用底层 start |
| `src/backend/access/transam/xlogfuncs.c` | `pg_backup_stop` | SQL 结束接口：要求该 session 正在 backup，调用底层 stop，打包 stop LSN/label/map 返回值并销毁 session backup 内存上下文 |
| `src/backend/access/transam/xlogfuncs.c` | `pg_walfile_name` | 将给定 LSN 用当前 segment 大小和 insertion timeline 换算成文件名；recovery 时明确拒绝，并不执行归档操作 |

## 参考

- [PostgreSQL 文档：基础备份 SQL 函数](https://www.postgresql.org/docs/current/functions-admin.html) —— `pg_backup_start` / `pg_backup_stop` 的用户可见调用契约
- [PostgreSQL 文档：连续归档与基础备份](https://www.postgresql.org/docs/current/continuous-archiving.html) —— backup label、WAL 归档与恢复所需关系
- [PostgreSQL 官方源码：xlogfuncs.c](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/xlogfuncs.c) —— SQL 接口适配与 session 状态管理

## 与主核 / AI 的关系

暂无直接对应。
