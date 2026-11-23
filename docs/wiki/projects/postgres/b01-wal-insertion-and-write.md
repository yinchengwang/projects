---
project: postgres
feature: wal-insertion-and-write
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/transam/xloginsert.c
    symbols: [XLogBeginInsert, XLogRegisterBuffer, XLogInsert]
  - path: src/backend/access/transam/xlog.c
    symbols: [XLogInsertRecord, XLogWrite, XLogFlush]
---

# WAL 记录插入与写出

## 一句话职责

该机制把资源管理器描述的页变更和附带数据编码为可校验、可重放的 WAL record，并将其插入共享 WAL 缓冲区，按需写到持久存储。

## 机制是什么

构造层与共享插入层分工：`xloginsert.c` 接收主数据、buffer 引用及块附属数据，组装 record header、块描述、必要的 full-page image（FPI）和 CRC；`xlog.c` 为完整链表分配 WAL 逻辑空间、并发复制到共享 WAL buffer，并提供写出与 flush 路径。`XLogInsert` 返回 record 末端 LSN，可用于更新受影响数据页 LSN，遵守 WAL-before-data。

这页只解释 record 组装、插入、WAL buffer 及基础写出/flush，不负责 checkpoint 如何选择 REDO 点、刷脏页和更新控制文件（见 b02），也不负责启动恢复中的 record reader、redo 执行或归档取回。调用方持有并锁定修改中的数据页；WAL 层记录其身份和变化，不替代 buffer 内容锁或事务提交状态语义。

## 怎么实现的

1. 调用方以 `XLogBeginInsert` 开始一次后端私有构造，随后用 `XLogRegisterBuffer` 登记页块身份、页内容和 flags，并以注册 data 接口提供主数据/块数据。`registered_buffer` 保存 `RelFileLocator`、fork、block、page、flags 及块数据链；一般需在关键区外预留额外数组空间。
2. `XLogInsert` 的 `XLogRecordAssemble` 把注册内容转换为 `XLogRecData` 链。它依据 REDO 点及 full-page-write 状态判断是否需要 FPI；标准页在页头空间边界有效时可省略零填充空洞，且可按配置尝试压缩。块头记录 FPI 是否需应用、块载荷长度及 relation/block 身份；记录长度受 `XLogRecordMaxSize` 限制，数据 CRC 在插入层填好前序链后补齐。
3. `XLogInsertRecord` 在关键区先取得一把 WAL insertion LWLock。该共享锁组允许其他后端并行复制各自记录；短暂持有的 `insertpos_lck` 仅保护 `CurrBytePos` / `PrevBytePos`，将已对齐的记录长度从可用 WAL byte 位置预留出来，再换算成包含页头跳过规则的 LSN。
4. 预留后，`CopyXLogRecordToWAL` 根据记录流跨越 WAL 页时切片复制，并在续页写入 continuation 标记与剩余长度。首次初始化或回收 WAL buffer slot 由 `AdvanceXLInsertBuffer` / `GetXLogBuffer` 协作；必要的页映射变更受 `WALBufMappingLock` 保护。写入者通过每把 insertion lock 的 `insertingAt` 发布进度，避免 flush 把未完整复制的数据当作可写数据。
5. `XLogFlush` 对目标 LSN 做快速已刷检查；否则在拿 `WALWriteLock` 前调用 `WaitXLogInsertionsToFinish` 等待目标之前的在途插入完成。写出请求可以合并后来追加的数据，只有持有写锁的写者调用 `XLogWrite` 将 WAL buffer 写入段文件并按同步方法执行 fsync；写出失败在关键区转为 PANIC。flush 完成后推进共享写/刷结果并唤醒等待者。恢复期间不执行普通 WAL flush，而更新最小恢复点。

## 关键取舍

- **预留串行、复制并行（源码事实）**：`ReserveXLogInsertLocation` 在短自旋锁区内更新全局 byte 指针，避免将整条 record 复制序列化；代价是仍要协调位置和按页空间映射。若整个插入全程使用单一全局互斥锁，竞争热点会覆盖大块内存复制。
- **有限多把 insertion locks（源码事实）**：`NUM_XLOGINSERT_LOCKS` 在 `xlog.c` 定义为 8；它分散并发插入竞争，但 flush 前需遍历进度槽等待。取更多槽可提高并行空间，却增加 flush 检查开销；这是源码注释陈述的权衡。
- **等待指定 LSN 前的插入，不是等待所有写者（源码事实）**：`WaitXLogInsertionsToFinish` 读取每锁 `insertingAt`，并在持有 `WALWriteLock` 前等待，以避免插入者因淘汰旧 WAL buffer 反过来需要该写锁而死锁。非目标尾部插入可继续，不必把所有后续记录都纳入等待。
- **一次 flush 尝试可顺带刷更多 WAL（源码事实）**：`XLogFlush` 允许在拿到写锁后推进请求边界，并在提交路径可配置延迟以凑 group commit；减少 fsync 次数的代价是个别事务确认延迟增加。`RecordTransactionCommit` 在 `xact.c` 是提交持久化请求的实际调用路径；不要把“record 已插入”说成“事务已持久提交”。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/transam/xloginsert.c` | `XLogBeginInsert` | 校验当前不在恢复且本次构造未重复开始，建立私有构造状态 |
| `src/backend/access/transam/xloginsert.c` | `XLogRegisterBuffer` | 把已修改页的逻辑块身份、页指针与 image/data flags 登记为块引用 |
| `src/backend/access/transam/xloginsert.c` | `XLogInsert` | 汇总注册数据、生成 record 并调用低层插入；返回记录末端 LSN |
| `src/backend/access/transam/xlog.c` | `XLogInsertRecord` | 校验 full-page-write 竞态，预留 WAL 位置并协调并行复制 |
| `src/backend/access/transam/xlog.c` | `XLogWrite` | 在 `WALWriteLock` 下将共享 WAL buffer 写入 WAL 段并处理同步请求 |
| `src/backend/access/transam/xlog.c` | `XLogFlush` | 等待目标区域插入完成并保证目标 LSN 已刷到持久存储 |

## 与主核 / AI 的关系

暂无直接对应。阅读主核日志路径时，可借鉴“先建稳定记录、再发布位置、最后建立持久化屏障”的区分，但不能把主核自身的 WAL、buffer 或事务语义等同 PostgreSQL 实现。

## 参考

- [PostgreSQL 文档：WAL 简介](https://www.postgresql.org/docs/current/wal-intro.html) —— 说明 WAL 先行写入与数据文件恢复的基本约束。
- [PostgreSQL 官方源码：xloginsert.c](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/xloginsert.c) —— 支撑 record 组装、块引用及 full-page image 的实现事实。
- [PostgreSQL 官方源码：xlog.c](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/xlog.c) —— 支撑插入位置预留、并发复制与 flush 顺序。
