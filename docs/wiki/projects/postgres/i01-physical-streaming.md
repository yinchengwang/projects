---
project: postgres
feature: physical-streaming
page_type: feature
schema_version: 1
refs:
  - path: src/backend/replication/walsender.c
    symbols: [StartReplication, XLogSendPhysical, WalSndLoop]
  - path: src/backend/replication/walreceiver.c
    symbols: [WalReceiverMain, XLogWalRcvProcessMsg]
---

# 物理流复制

## 一句话职责

物理流复制把上游已 fsync 的 WAL 字节按 LSN 连续搬给下游，下游边收边写并回送 write/flush/apply 三个进度，形成一条可测量延迟的单向字节流。

## 机制是什么

它是**原始 WAL 字节的搬运**，不是另一种日志格式：上游 `walsender` 读 WAL 文件与 WAL buffer，把一段连续字节封成 CopyData 消息；下游 `walreceiver` 连上游、协商起点与 timeline、把字节写进本地 `pg_wal`、fsync 后回送进度。因为传的是同一个字节流，两侧必须同源——`WalReceiverMain` 用 `IDENTIFY_SYSTEM` 拿到的 sysid 与自己不符就报错，不允许跨库复制。

三个位置贯穿整条链路：`sentPtr`（上游已发出）、`LogstreamResult.Write`（下游已写盘）、`LogstreamResult.Flush`（下游已 fsync）。只有 flush 位置才算下游真正持有该段 WAL。

本页边界：只讲物理字节流的起点协商、分片发送、接收写盘、进度反馈、超时与 keepalive。**不负责**逻辑解码（WAL→事务变更，属「逻辑解码」页）、同步复制的等待队列语义（属「同步复制」页）、恢复期 redo 重放（属「WAL 恢复与归档取回」页）、复制槽的持久化与保留策略（属「复制槽」页）。时间线切换本身由 WAL 基础设施提供，本页只负责把它接入复制会话。

## 怎么实现的

1. **walsender 起点协商**。`StartReplication` 先为物理复制分配一个 WAL reader（`XLogReaderAllocate`，段开闭回调指向 `WalSndSegmentOpen` / `wal_segment_close`），再按命令里的 slot 名 `ReplicationSlotAcquire`；若是逻辑槽直接拒绝。flush 位置按角色取：级联备库用 `GetStandbyFlushRecPtr`，主库用 `GetFlushRecPtr`。客户端显式给了 timeline 且与当前 flush timeline 不同时，读 `readTimeLineHistory` 并用 `tliSwitchPoint` 定位分叉点，置 `sendTimeLineIsHistoric`。起点若是未来位置（`FlushPtr < cmd->startpoint`）直接报错，不允许请求尚未 fsync 的 WAL。通过后设 `sentPtr`、`SyncRepInitConfig()`、`replication_active = true`，进入 `WalSndLoop(XLogSendPhysical)`。

2. **walsender 主循环**。`WalSndLoop` 是 `for (;;)`：`ResetLatch` → `CHECK_FOR_INTERRUPTS` → 配置重载 → `ProcessRepliesIfAny` 收下游反馈。收齐 CopyDone（自己已发也收到）且输出缓冲空则退出。输出缓冲空时调 `send_data()`，否则把 `WalSndCaughtUp` 置 false。随后 `pq_flush_if_writable` 非 0 就直接 `WalSndShutdown`。若已追平且无待发数据：CATCHUP 状态切到 STREAMING（源码注释说明这一状态变更对用户与同步复制都重要），收到 SIGUSR2 则走 `WalSndDone`。之后检查 `wal_sender_timeout`、shutdown 超时、按需发 keepalive，最后 `WalSndWait` 阻塞——仅在未收齐 CopyDone 时订阅 socket 可读，有待发数据再加可写。

3. **发送切片**。`XLogSendPhysical` 先按角色算 `SendRqstPtr`：historic timeline 只能发到分叉点；级联备库取 `GetStandbyFlushRecPtr`，并在期间检测"已被提升"或"timeline 已变"——若变成 historic，读新 timeline 历史得出 `sendTimeLineValidUpto` 并只发到那里；主库取 `GetFlushRecPtr`。接着 `LagTrackerWrite` 记下本 LSN 的近似写出时刻。historic 且已到分叉点则关段、发 CopyDone、置 `streamingDoneSending`、返回。`SendRqstPtr <= sentPtr` 说明无活可干，置 `WalSndCaughtUp` 返回。切片上限是常量 `MAX_SEND_SIZE`（定义在 `walsender.c` 顶部，值为 `XLOG_BLCKSZ * 16`）；超出时**向下回退到页边界**。消息头三个 int64 是 dataStart / walEnd / sendtime（sendtime 留到最后才填），接着先从 WAL buffer 读（`WALReadFromBuffers`），剩余再走 `WALRead` 读文件，`CheckXLogRemoved` 校验段未被删。级联场景下若共享内存标记了 needreload，关段后跳到 retry 重读。最后填 sendtime、`pq_putmessage_noblock(PqMsg_CopyData)`、推进 `sentPtr` 并在自旋锁下写入 `MyWalSnd->sentPtr`。

4. **walsender 收反馈**。`ProcessRepliesIfAny` 用非阻塞读字节：EOF 视为协议违例并 `proc_exit(0)`，无数据就返回；消息类型只接受 CopyData（大包上限）/ CopyDone / Terminate（小包上限），其它一律 FATAL。CopyData 里解出的 write/flush/apply/replyTime/replyRequested 由 `ProcessStandbyReplyMessage` 处理：三个位置各自用 `LagTrackerRead` 算延迟；当 apply、flush 都已追平 `sentPtr` 且三者连续两条消息都不变时清掉延迟采样（源码注释说明这避免空闲期显示陈旧延迟）；下游要求回执就 `WalSndKeepalive`；写回共享内存（自旋锁下更新 write/flush/apply/lag/replyTime）。非级联 walsender 调 `SyncRepReleaseWaiters` 放行同步复制的等待者；若持有槽且 flush 位置有效，物理槽走 `PhysicalConfirmReceivedLocation` 推进 xmin 地平线。

5. **walreceiver 启动与协商**。`WalReceiverMain` 先在共享内存里把 `WalRcv` 状态从 STARTING 转成 CONNECTING（若已是 STOPPING/STOPPED 则广播条件变量后退出；若显示仍在跑则 PANIC），登记 pid，从共享结构拷出 conninfo、slotname、是否临时槽、`receiveStart` 与 `receiveStartTLI`，注册 `WalRcvDie` 退出清理，按需装载 `libpqwalreceiver`，并在连上之前把 conninfo/host/port 清空、把 `ready_to_display` 置真（源码注释说明这是为了不在连接前泄露连接串）。真正连接成功后写回真实连接信息。主循环每轮先 `IDENTIFY_SYSTEM` 校验 sysid 与 timeline（上游 timeline 落后于本地恢复 timeline 就报错），再看请求起点是否超过上游 flush 位置：只在同 timeline 且缺口不超过一个 WAL 段时等待，等待用 `wal_retrieve_retry_interval` 打盹、用 `wal_receiver_timeout` 兜底，超时报错让启动进程换别的 WAL 来源；否则补 timeline 历史文件、按需建临时槽，再发 `START_REPLICATION`。

6. **接收与写盘**。`XLogWalRcvProcessMsg` 按类型分派：WALData 包头是三个 int64（dataStart / walEnd / sendTime），先 `ProcessWalSndrMessage` 更新"上游已到哪"，再把剩余字节交给 `XLogWalRcvWrite`；Keepalive 包头是两个 int64 加一个字节，长度必须精确相等，下游被要求回执时立刻 `XLogWalRcvSendReply`；其它类型一律协议违例。写盘按 WAL 段循环切块：跨段先关旧段，用 `XLogFileInit` 建/取新段，`pg_pwrite` 写到段内偏移，失败时按"写不出就当磁盘满"补 errno 后 `ereport(PANIC)`。写完推进 `LogstreamResult.Write`、用带内存屏障的原子写发布 `WalRcv->writtenUpto`、唤醒等待 standby-write LSN 的进程，并在恰好跨段时立刻关闭该段以便尽早起归档。

7. **fsync 与回送**。`XLogWalRcvFlush` 在 `Flush < Write` 时做 `issue_xlog_fsync`，成功后才把 `LogstreamResult.Flush` 抬到 Write，并在自旋锁下更新共享状态——下游只有在 fsync 之后才会把 flush 位置报给上游，所以上游的"已确认"语义确有落盘支撑。

## 关键取舍

- **分片切点回退到 WAL 页边界（源码事实）**。`MAX_SEND_SIZE` 只是一个上限，超出时 `endptr` 要向下取整到 `XLOG_BLCKSZ` 的整数倍。源码注释给出的理由不是性能而是正确性：walreceiver 依赖"一条 WAL record 不会被拆进两条消息"，而超长 record 本来就在页边界被拆成 continuation record，所以页边界永远是安全切点。代价是单条消息可能明显小于上限，吞吐只能靠循环补回来。
- **可发送的上界取"已 fsync 的 flush 位置"，而不是"已写入位置"（源码事实）**。源码注释直说：发未落盘的 WAL 是不安全的——主库崩溃重启后不能有备库已经应用过主库丢失的 WAL。代价是复制多等一次 fsync 的延迟，换来崩溃后不会分叉。
- **等待用 latch + socket 事件而不是轮询（源码事实）**。`WalSndWait` 的订阅集合按状态最小化：未收齐 CopyDone 才订阅可读，输出缓冲有积压才加可写；超时由 `WalSndComputeSleeptime` 结合 `wal_sender_timeout` 算。若改成固定轮询，空闲期也要持续占用 CPU 唤醒。
- **下游写盘失败按 PANIC 而不是 ERROR 处理（源码事实）**。`XLogWalRcvWrite` 写不进段文件时 `ereport(PANIC)`，而本页其它协议问题多为 ERROR。原因是下游在逻辑上已经"承诺"接收这条流，继续运行意味着本地 WAL 出现静默空洞；宁可拉倒重启进程，让启动进程重新从可用来源取 WAL。
- **（架构推论，非源码论断）跨库复制在协议层被彻底排他**。sysid 校验把这条通道限定在同一簇血脉内，所以物理流的语义天然与逻辑复制不同：它不需要理解表结构，也无法跨不同实例搬运数据。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/replication/walsender.c` | `StartReplication` | 起点协商：分配 WAL reader、取槽、定 flush 位置与 timeline、拒绝超前起点，进主循环 |
| `src/backend/replication/walsender.c` | `WalSndLoop` | 主循环：收反馈、调发送回调、刷输出缓冲、状态迁移、超时与 keepalive、阻塞等待 |
| `src/backend/replication/walsender.c` | `XLogSendPhysical` | 按角色定发送上界、按页边界切片、读 WAL buffer 与文件、发 CopyData、推进 sentPtr |
| `src/backend/replication/walreceiver.c` | `WalReceiverMain` | 下游主进程：状态登记、连接上游、sysid/timeline 校验、起点等待、建临时槽、循环取流 |
| `src/backend/replication/walreceiver.c` | `XLogWalRcvProcessMsg` | 收包分派：WALData 解头后交写盘，Keepalive 校验精确长度并按需回执 |

## 与主核 / AI 的关系

暂无直接对应。主核若要做"日志流复制"，可借鉴两点协议设计：发送上界必须绑在**持久化确认**之后，以及进度反馈要分成 write / flush / apply 三层而不是一个位置——这直接决定下游能提供多强的持久性承诺。不能据此推断主核采用 CopyData 子协议、latch 等待或页边界切片；也不能把本页的 `sentPtr` 语义搬成主核的 LSN 语义。

## 参考

- [PostgreSQL 文档：流复制协议（CopyBoth / START_REPLICATION）](https://www.postgresql.org/docs/current/protocol-replication.html) —— 支撑消息类型、CopyData 载荷字段与反馈消息格式。
- [PostgreSQL 文档：预写日志与流复制的可靠性](https://www.postgresql.org/docs/current/warm-standby.html) —— 支撑"只发送已落盘 WAL"与下游三级进度（write/flush/apply）的语义。
- [PostgreSQL 文档：复制相关参数（wal_sender_timeout / wal_receiver_timeout / wal_retrieve_retry_interval）](https://www.postgresql.org/docs/current/runtime-config-replication.html) —— 支撑本页的超时、重试与 keepalive 行为。
