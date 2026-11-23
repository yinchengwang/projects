---
project: postgres
feature: synchronized-sequential-scan
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/common/syncscan.c
    symbols: [ss_get_location, ss_report_location]
---

# 同步顺序扫描

## 一句话职责

同步顺序扫描在共享内存中发布每个关系最近的顺序扫描位置，让新加入的扫描尽量从相近位置开始以复用缓存页。

## 机制是什么

`syncscan.c` 维护以 `RelFileLocator` 标识关系的共享扫描位置表。一个固定长度 LRU 项存放 relation locator 与最后上报的 block；开始扫描时读取该位置，前进中的扫描则间隔性更新它。参与者不需要彼此识别，也不会逐页等待同伴。

本机制只共享近似扫描起点和进度，不共享扫描结果、不调度 backend，也不保证所有扫描长期保持同速或读取完全相同的页面时序。顺序扫描调用方负责环形覆盖全关系；VACUUM 截断导致已记录 block 超出当前关系长度时，入口回退到 block 0。该机制与 buffer ring 配合可提高共同扫描时的缓存复用机会，但不保证目标页仍在缓存中。

## 怎么实现的

1. `SyncScanShmemRequest` 申请 `SizeOfScanLocations(SYNC_SCAN_NELEM)` 共享空间；`SyncScanShmemInit` 将 `ss_scan_locations_t` 初始化为双向 LRU，空项使用无效 locator 和 block 值。表项含 `prev`/`next`，`head`/`tail` 管理最近使用关系。
2. 普通 heap scan 在启用 `allow_sync` 且 `synchronize_seqscans` 为真时调用 `ss_get_location(rel, relnblocks)`。该函数持 `SyncScanLock` 排他锁，调用 `ss_search` 查找/创建关系条目并将其移到 LRU 头部，若位置已不在当前关系块范围内则以 0 开始。
3. `ss_report_location` 只对 `SYNC_SCAN_REPORT_INTERVAL` 对齐的 block 尝试发布；它用 `LWLockConditionalAcquire`，拿不到锁便跳过更新，不让进度报告阻塞扫描。成功时更新位置并提升条目到 LRU 头。
4. heap 扫描在向前推进 block 时上报；向后扫描不发布位置，以免倒退覆盖其他正向扫描的前沿。扫描从选定位置绕到关系末尾后再从 block 0 扫至起点，仍覆盖完整关系。
5. LRU 容量固定为 `SYNC_SCAN_NELEM`；新关系满表时复用尾项。关系太多或锁竞争都可能导致提示被替换/漏报，随后扫描仍正确，只是缓存协作效果变弱。

## 关键取舍

- **同步起点，不同步执行进度。** 源码注释明确指出慢客户端可能让 follower 进度远落后，因此机制刻意不持续把 scans 捆在一起。放弃的是严格协同和同页 rendezvous，换取慢扫描不会拖住快扫描；若每页协调，等待/锁操作会成为新的序列化点。
- **报告是稀疏且可丢失的提示。** `SYNC_SCAN_REPORT_INTERVAL` 取与 BLCKSZ 相关的页间隔；报告还使用条件锁，失败直接忽略。源码注释指出漏报仅令后来者稍落后，页也可能仍在 OS cache。放弃精确位置降低锁竞争；强制每页、阻塞式更新会给所有顺序扫描增加共享锁成本。
- **固定小型 LRU，而非按所有关系动态分配。** 源码事实是共享结构按 `SYNC_SCAN_NELEM` 申请，查询/更新线性遍历并移到表头。放弃无淘汰的长期记录，以有界共享内存服务同时扫描的少数大表；表项被淘汰只影响起点提示，不影响扫描完整性。具体常量由本源码定义，不推作通用性能保证。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/common/syncscan.c` | `ss_get_location` | 在锁下查询/创建关系 LRU 条目，返回可用于当前关系的起点 |
| `src/backend/access/common/syncscan.c` | `ss_report_location` | 按间隔以条件锁尽力发布正向扫描前沿，避免报告阻塞读者 |
| `src/backend/access/common/syncscan.c` | `ss_search` | 查找/复用条目、更新位置并维护 LRU 次序 |
| `src/backend/access/heap/heapam.c` | `heapgettup_advance_block` | 正向推进 heap scan 时报告位置，并在回绕至起点后结束一轮 |

## 与主核 / AI 的关系

暂无直接对应。可将其视为多 worker 顺序读取时的尽力缓存协作提示，而非 barrier、工作窃取或一致性协议；即使没有共享起点，扫描仍应完整并保持自身语义。

## 参考

- [PostgreSQL 官方源码：同步顺序扫描实现](https://github.com/postgres/postgres/blob/master/src/backend/access/common/syncscan.c) —— 支撑共享 LRU、锁策略与位置报告间隔
- [PostgreSQL 官方源码：heap 顺序扫描](https://github.com/postgres/postgres/blob/master/src/backend/access/heap/heapam.c) —— 支撑扫描起点接入与环形前进时的位置上报
