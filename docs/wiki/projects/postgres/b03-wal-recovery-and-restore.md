---
project: postgres
feature: wal-recovery-and-restore
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/transam/xlogrecovery.c
    symbols: [PerformWalRecovery, InitWalRecovery]
  - path: src/backend/access/transam/xlogutils.c
    symbols: [XLogReadBufferForRedo]
  - path: src/backend/access/transam/xlogarchive.c
    symbols: [RestoreArchivedFile]
  - path: src/backend/access/transam/timeline.c
    symbols: [readTimeLineHistory]
---

# WAL 恢复与归档取回

## 一句话职责

启动恢复协调器依据检查点定位需要重放的 WAL，按可用来源取得记录并调用资源管理器 redo，使数据页状态追到恢复目标。

## 机制是什么

启动阶段先从控制文件和可选 backup label 确认 checkpoint、redo 起点、目标 timeline 与恢复模式，再由 `PerformWalRecovery` 按顺序读取 WAL、判断恢复目标并应用记录。恢复读取不是一个单纯本地文件循环：归档模式可从 archive 取回段，standby 还可经 walreceiver 取流；timeline history 用来判断不同时间线间的有效区间和切换合法性。

本页负责 startup redo、redo buffer 辅助、archive restore 与 timeline，不讲正常运行期间 WAL 插入、buffer 写出或 checkpoint 编排；通用 record 解码接口见 b04，热备查询与恢复冲突取消见 b05。`XLogReadBufferForRedo` 是 redo handler 使用的页访问帮助函数，不自行决定资源管理器操作语义。

## 怎么实现的

1. `InitWalRecovery` 在 `StartupXLOG` 已读控制文件后检查 signal 文件、恢复参数和 backup label，设置目标 TLI / REDO 位置；初始化 `XLogReaderState` 与 prefetcher，并保留是否来自 archive、standby 的启动状态。
2. `PerformWalRecovery` 从 REDO 位置开始取第一条记录；若 redo 指针指向 checkpoint 前的 `XLOG_CHECKPOINT_REDO`，先验证类型。主循环处理恢复目标、暂停/延迟和中断，然后把记录交给 `ApplyWalRecord`。
3. 每条记录先推进 nextXid，并在 checkpoint/end-of-recovery 记录时核验 timeline 切换；恢复控制共享状态更新后，特殊 XLOG 逻辑与该记录的资源管理器 `rm_redo` 回调依序运行。回放成功才更新 last replayed LSN，供恢复位置等待者观察。
4. redo handler 按 WAL 块引用调用 `XLogReadBufferForRedo`。其扩展路径核对 `WILL_INIT` 与零页模式；若 FPI 标为需应用，则恢复页图像、设页面 LSN 并标脏；否则拿 buffer 锁，依据当前 page LSN 返回 `BLK_DONE`、`BLK_NEEDS_REDO` 或页不存在结果。
5. 获取 WAL 时，startup 的 page-read callback 进入 `WaitForWALToBecomeAvailable` 状态机：archive/pg_wal、standby stream 及失败后的 timeline 重扫和退避重试；不在 standby 的本地/归档检索失败可表示 WAL 尾端。归档恢复经 `RestoreArchivedFile` 用临时文件名调用 restore command，校验文件存在和预期段大小后才作为候选；归档成功优先于本地同名段，避免备份目录里的旧段覆盖归档正确版本。
6. 对非初始 timeline，`readTimeLineHistory` 解析历史文件中的父 timeline 和 switch LSN，验证 ID 递增、目标 timeline 高于父链；timeline 切换处要求记录及读取区间符合 history。archive 拿到的 history 文件会保留到 `pg_wal` 供后续查询。

## 关键取舍

- **按来源状态机取数而不是把恢复耦合到单一介质（源码事实）**：同一恢复读取面可依次查 archive、本地 WAL 或 standby stream；代价是需要处理来源失败、重试、promotion 及 timeline 变动。若只读本地文件，归档恢复和 standby 无法复用统一 redo 循环。
- **归档优先并写临时名（源码事实）**：`RestoreArchivedFile` 不直接覆盖本地 WAL；只有成功后再通过 `KeepFileRestoredFromArchive` 安装，保留失败时回退到本地剩余 WAL 的机会。若先覆盖，restore command 取回错误/不完整段可能破坏仅有可用副本。
- **FPI 与 page-LSN 检查分流（源码事实）**：有效 `BKPIMAGE_APPLY` 标志时恢复经过 CRC 校验的 image，保护 crash 中部分写入的磁盘页；无 FPI 时只在 record LSN 高于页 LSN时请求 rmgr redo。前者会增加 WAL 体量，后者依赖页状态可信及各 redo handler 正确维护 LSN。
- **timeline 文件缺失时假定无父链（源码事实）**：`readTimeLineHistory` 对 ENOENT 返回仅含目标 TLI 的列表；已有文件则严格解析格式和顺序。缺失时可支持没有 history 文件的初始时间线，但错误缺失可能限制识别父 timeline 的能力；不能把此 fallback 写成任意分叉都可安全跟随。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/transam/xlogrecovery.c` | `InitWalRecovery` | 解析启动恢复输入并建立 reader/prefetcher 及恢复起点 |
| `src/backend/access/transam/xlogrecovery.c` | `PerformWalRecovery` | 驱动按序记录重放、恢复目标判定与进度状态维护 |
| `src/backend/access/transam/xlogutils.c` | `XLogReadBufferForRedo` | 将记录块引用转换为 redo handler 可用的 buffer 与 redo action |
| `src/backend/access/transam/xlogarchive.c` | `RestoreArchivedFile` | 调用外部恢复命令取归档段并检查可用性/大小 |
| `src/backend/access/transam/timeline.c` | `readTimeLineHistory` | 读取并验证时间线父链和切换点 |

## 与主核 / AI 的关系

暂无直接对应。恢复时先定位持久化日志边界再重放的通用思想可作参照；不能推断主核采用 PostgreSQL 的 timeline、归档命令或页面重做协议。

## 参考

- [PostgreSQL 文档：连续归档与时间点恢复](https://www.postgresql.org/docs/current/continuous-archiving.html) —— 支撑基础备份、归档 WAL 与恢复目标关系。
- [PostgreSQL 官方源码：xlogrecovery.c](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/xlogrecovery.c) —— 支撑启动 redo 和恢复来源状态机。
- [PostgreSQL 官方源码：timeline.c](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/timeline.c) —— 支撑时间线历史文件解析和 fork 点语义。
