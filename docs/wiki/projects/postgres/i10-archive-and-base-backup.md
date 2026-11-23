---
project: postgres
feature: archive-and-base-backup
page_type: feature
schema_version: 1
refs:
  - path: src/backend/backup/basebackup.c
    symbols: [SendBaseBackup, perform_base_backup]
  - path: src/backend/access/transam/xlogfuncs.c
    symbols: [pg_backup_start, pg_backup_stop]
  - path: src/backend/postmaster/pgarch.c
    symbols: [pgarch_archiveXlog, PgArchiverMain]
---

# 归档与基础备份

## 一句话职责

WAL 归档、会话级备份模式接口与 `BASE_BACKUP` 物理复制是三个协作但独立的机制：archiver 尝试将已完成的 WAL 段交给配置的归档目标，备份模式接口管理起止点与备份状态，`BASE_BACKUP` 协议则传送数据目录快照及可选 WAL。

## 机制是什么

三者解决的是协作恢复链上的不同问题，不构成无条件保证。基础备份在备份模式下记录 `startpoint`/`starttli`，提供物理数据副本；`BASE_BACKUP` 可按请求选项附带 WAL（`INCLUDE_WAL`），否则不会把后续 WAL 一并打包。archiver 根据 `.ready` 状态和配置执行归档回调，失败会重试并在本轮放弃后再试，并不保证归档目标已持久保存全部所需 WAL。恢复所需的 WAL 连续性取决于归档配置、成功交付、外部保留策略及备份选项；没有归档或其他保留机制时，WAL 可能被回收覆盖。

本页负责三个协作但不等同的机制：archiver 进程的 `.ready` 状态机与重试策略；`pg_backup_start`/`pg_backup_stop` 的会话级备份模式接口；`BASE_BACKUP` 协议下 `SendBaseBackup`/`perform_base_backup` 的传送与一致性处理。不负责 WAL 记录本身的写入与 flush（b01/b02），不负责恢复期如何取回并重放（b03 的 `RestoreArchivedFile`），也不负责流复制协议（i01）。只讨论各自的数据/WAL 传送与边界，不把三者概括为无条件归档或可恢复保证。

## 怎么实现的

1. `PgArchiverMain` 在 `AuxiliaryProcessMainCommon` 后设置信号（SIGHUP/SIGTERM/SIGUSR1/SIGUSR2），把自己登记到 `PgArch->pgprocno` 以便后端能唤醒，分配 `arch_files` 工作区与按优先级比较的 `arch_heap`（容量 `NUM_FILES_PER_DIRECTORY_SCAN`，源码为 64），加载 archive 库后进 `pgarch_MainLoop`。
2. `pgarch_MainLoop` 每轮先 `ProcessPgArchInterrupts`（使 `archive_command`/`archive_library` 的配置变更尽快生效），再 `pgarch_ArchiverCopyLoop` 归档当前所有待归档文件，最后在 latch 上等待 `PGARCH_AUTOWAKE_INTERVAL`（源码 60 秒）超时、被唤醒或 postmaster 死亡；收到 SIGUSR2 后再跑一轮即退出。
3. `pgarch_ArchiverCopyLoop` 反复取 `pgarch_readyXlog` 返回的最老 `.ready` 文件。若 WAL 段本身已不存在（被回收）而只剩状态文件，就先 `unlink` 孤儿 `.ready`，连续 `NUM_ORPHAN_CLEANUP_RETRIES`（3 次）失败才放弃本轮；正常路径调 `pgarch_archiveXlog`，成功则 `pgarch_archiveDone` 改名为 `.done` 并上报统计，失败则重试，累计 `NUM_ARCHIVE_RETRIES`（3 次）后放弃本轮（下次循环再试）。
4. `pgarch_readyXlog` 优先复用上次目录扫描缓存的候选（并用 `stat` 确认 `.ready` 仍在），否则扫描 `pg_wal/archive_status` 用 `arch_heap` 取优先级最高的一批。排序把 `.history` 文件视为比任何段更早、时间线 ID 小者更早，因此旧时间线的段优先归档——源码注释认为这至少不比其它策略差。
5. `pgarch_archiveXlog` 自行安装 `sigsetjmp` 异常处理，因为 archiver 处在异常栈底部，普通 `ERROR` 会变成 FATAL 导致进程重启。它把归档活动写进 ps display，重置 `archive_context` 后调 `ArchiveCallbacks->archive_file_cb`；捕获到 ERROR 时清理锁/超时/等待状态、`FlushErrorState` 并返回 false，让循环重试该文件而不是重启进程。
6. `pg_backup_start` 拒绝同一会话重复开始备份，把 `backup_state` 与 `tablespace_map` 分配在 `TopMemoryContext` 子上下文中（因为要活到 `pg_backup_stop`），注册 abort handler 后调 `do_pg_backup_start`，返回 `startpoint`。`pg_backup_stop` 校验备份确在进行，调 `do_pg_backup_stop`，再用 `build_backup_content` 生成 `backup_label`，返回 `stoppoint`、`backup_label`、`tablespace_map` 三元组并删除上下文。
7. `SendBaseBackup` 是 `BASE_BACKUP` 协议入口：解析选项、置 `WALSNDSTATE_BACKUP`，要求增量备份必须已先 `UPLOAD_MANIFEST`；随后按需叠加 sink 链——`bbsink_copystream_new`（决定是否发给客户端）、`BaseBackupGetSink`（服务端委托目标）、throttle、gzip/lz4/zstd、progress——并用 `PG_TRY/PG_FINALLY` 保证失败时 `bbsink_cleanup`。
8. `perform_base_backup` 置 `backup_started_in_recovery`、初始化 manifest，`basebackup_progress_wait_checkpoint` 后 `do_pg_backup_start` 进入备份模式并拿到 `startptr`/`starttli`；随后整个主体被 `PG_ENSURE_ERROR_CLEANUP(do_pg_abort_backup, ...)` 包住，避免异常路径泄漏备份计数。它逐个 tablespace 生成 tar：主数据目录先写 `backup_label`、可选 `tablespace_map`，再 `sendDir`，最后在 `ControlFileLock` 保护下读 `pg_control` 并按 `PG_CONTROL_FILE_SIZE`（8192）补零发送。
9. 若请求包含 WAL，主目录的 tar 故意不结束，等 `do_pg_backup_stop` 得到 `stoppoint`/`stoptli` 后，扫 `pg_wal` 收集 `startsegno` 到 `endsegno` 范围内的段与 `.history` 文件，用 `compareWalFileNames` 排序、`CheckXLogRemoved` 校验连续性，再逐段追加入同一 tar；`same timeline` 之外的杂散段也会被带上，源码注释认为危害很小。

## 关键取舍

- **源码事实：**归档顺序严格按最老优先。源码注释给出两个理由：维持恢复所需的 WAL 链连续性，以及最老的段会最先成为 checkpoint 回收候选。若按最新优先，段可能在发出前就被回收。
- **源码事实：**`pgarch_archiveXlog` 自建异常处理而非让 archiver 重启。源码注释指出 ERROR 在 archiver 会变成 FATAL；自行捕获避免频繁重启，代价是要手工清理 `LWLockReleaseAll`、`ConditionVariableCancelSleep`、`pgaio_error_cleanup` 等状态，且归档模块若有额外残留无法保证清除。
- **源码事实：**`BASE_BACKUP` 的主目录 tar 在 `includewal` 时延迟收尾，用同一 tar 追加 WAL 段。放弃「数据与 WAL 分成两个归档」的简单结构，换来客户端能拿到自包含的一包；代价是必须保证主目录确实最后发送（源码以 `Assert(lnext(state.tablespaces, lc) == NULL)` 守住）。
- **源码事实：**`pg_control` 在主目录其它文件之后才发送，且读取时持 `ControlFileLock` 共享锁。若先读再打包，备份期间 control file 可能已变化，导致 `pg_control` 与其余文件不一致；放在最后缩小了这个窗口。
- **源码事实：**`perform_base_backup` 用 `PG_ENSURE_ERROR_CLEANUP` 而非 `PG_TRY/PG_FINALLY` 处理 `do_pg_backup_start` 之后的错误。源码注释说明目的是不泄漏 backup counter——若只在正常路径调 `do_pg_backup_stop`，异常时后端会永久处于「备份中」状态。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/backup/basebackup.c` | `SendBaseBackup` | 解析 `BASE_BACKUP` 选项、装配 sink 链并以异常安全方式驱动打包 |
| `src/backend/backup/basebackup.c` | `perform_base_backup` | 进入备份模式、逐 tablespace 打包、发送 control file 并可选追加 WAL 段 |
| `src/backend/access/transam/xlogfuncs.c` | `pg_backup_start` | 建立会话级备份上下文并调用 WAL 层进入备份模式，返回起始 LSN |
| `src/backend/access/transam/xlogfuncs.c` | `pg_backup_stop` | 结束备份、生成 `backup_label`，返回停止 LSN 与该文件内容 |
| `src/backend/postmaster/pgarch.c` | `pgarch_archiveXlog` | 在自带异常处理下调用归档回调，把单个 WAL 段送到外部存储 |
| `src/backend/postmaster/pgarch.c` | `PgArchiverMain` | archiver 进程入口：信号设置、共享槽登记、加载归档库并进入主循环 |

## 与主核 / AI 的关系

暂无直接对应。

## 参考

- [PostgreSQL 文档：连续归档与时间点恢复](https://www.postgresql.org/docs/current/continuous-archiving.html) —— 支撑基础备份加 WAL 归档的恢复模型。
- [PostgreSQL 文档：pg_backup_start / pg_backup_stop](https://www.postgresql.org/docs/current/functions-admin.html) —— 支撑会话级备份模式接口的语义。
- [PostgreSQL 官方源码：basebackup.c](https://github.com/postgres/postgres/blob/master/src/backend/backup/basebackup.c) —— 支撑 BASE_BACKUP 打包流程与一致性处理。
- [PostgreSQL 官方源码：pgarch.c](https://github.com/postgres/postgres/blob/master/src/backend/postmaster/pgarch.c) —— 支撑 `.ready` 状态机、重试与异常处理策略。
