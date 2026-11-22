---
project: postgres
feature: smgr-md-storage
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/smgr/smgr.c
    symbols: [smgropen]
  - path: src/backend/storage/smgr/md.c
    symbols: [mdcreate, mdextend]
---

# 关系物理存储（smgr 与 md）

## 一句话职责

smgr 将关系文件操作路由到存储实现，md 实现把逻辑块映射到磁盘上的关系段文件。

## 机制是什么

`SMgrRelation` 是后端内代表物理关系文件的句柄，smgr 按关系定位信息缓存句柄，并通过存储管理器方法表分派创建、打开、扩展、读取、写入、截断和同步等操作。默认 md 存储实现将关系的 fork 划分为固定上限大小的段文件，使逻辑块号可映射到段号及段内偏移。

这层处理关系文件的物理组织，不解释页内 tuple、不决定缓冲页是否驻留，也不制定 WAL 原子性规则。buffer manager 管理共享页缓存及脏页；WAL 负责变更日志与恢复顺序；md 提供它们下方的文件块 I/O 接口。`smgropen` 的句柄缓存也不是 relcache 的元数据缓存，二者生命周期和键空间不同。

## 怎么实现的

smgr 的 `smgropen` 以关系 locator 查找或创建 `SMgrRelation`，将实现方法绑定到该句柄。调用者经 smgr 接口请求块操作后，方法表将请求送到 md。md 针对每个 fork 维护已打开段的描述向量 `MdfdVec`；其条目保存文件句柄和段编号等信息，实际文件打开最终经 VFD 接口完成。

创建关系时 `mdcreate` 建立首段文件并登记段状态。扩展时 `mdextend` 依据块号计算段位置；若目标块越过当前段容量，路径会创建/打开后续段并写入块数据。多块扩展可采用批量接口降低逐块开销。读取、写入和块数查询同样先定位相应段，再由 VFD 对文件偏移执行操作。删除、截断或同步会更新段文件状态，并处理打开句柄与脏段追踪。

调用通常由 buffer manager 或关系管理上层发起；md 的文件写入本身不自动替代 WAL 先行写出要求。后端内的 SMgrRelation/段句柄按后端生命周期管理，特定共享缓存或辅助进程场景可直接打开句柄；不能假设一个后端的句柄状态被其他进程共享。

## 关键取舍

- 以固定容量分段而非单个无限增长文件，便于文件系统兼容、块号寻址和段级操作；换成单文件需面对极大文件限制及更复杂的文件系统差异。
- smgr 采用方法分派，而不是让 buffer manager 直接调用 POSIX 文件接口，从而保留替代存储实现边界；代价是多一层间接调用。
- 关系块 I/O 与缓存、WAL 解耦，便于职责清晰；若 md 同时维护脏页和事务日志语义，会造成层间耦合并重复状态管理。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/smgr/smgr.c` | `smgropen` | 获取或创建关系物理句柄 |
| `src/backend/storage/smgr/md.c` | `mdcreate` | 创建 fork 的物理文件 |
| `src/backend/storage/smgr/md.c` | `mdextend` | 按逻辑块扩展关系段 |
| `src/backend/storage/smgr/md.c` | `MdfdVec` | 表示已打开关系段及其 VFD |
| `src/backend/storage/file/fd.c` | `PathNameOpenFile` | md 使用的底层文件句柄接口 |

## 参考

- [PostgreSQL 官方文档：数据库物理存储](https://www.postgresql.org/docs/current/storage-file-layout.html)
- [PostgreSQL 官方文档：数据库页布局](https://www.postgresql.org/docs/current/storage-page-layout.html)
- [PostgreSQL 源码：smgr.c](https://github.com/postgres/postgres/blob/master/src/backend/storage/smgr/smgr.c)

## 与主核 / AI 的关系

主核可借鉴关系逻辑句柄与具体文件组织实现分层，以及逻辑块到分段偏移的映射。AI 负载不会改变该层的存储语义；向量、嵌入等数据若进入关系页，仍需遵从上层页格式与 WAL 协议。
