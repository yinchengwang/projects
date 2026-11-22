---
project: postgres
feature: fd-cache-file-io
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/file/fd.c
    symbols: [PathNameOpenFile, FileClose, pg_fsync]
---

# 虚拟文件描述符与文件 I/O

## 一句话职责

VFD 把后端逻辑文件句柄与有限的操作系统文件描述符解耦，使大量文件可按需打开、关闭与重开。

## 机制是什么

PostgreSQL 后端在运行中可能同时引用关系段文件、排序/哈希临时文件以及其他文件；操作系统允许单进程持有的 fd 数量有限。VFD 为调用者提供稳定的 `File` 标识，并在内部维护文件路径、打开标志、当前 OS fd、访问状态等元数据。达到安全 fd 上限时，机制可回收最近最少使用的已打开 fd，之后访问时再按记录的信息重开。

这层管理的是文件句柄资源与通用文件操作，不负责关系逻辑、缓冲池页替换或事务持久性协议。关系文件的段定位由 smgr/md 负责；缓冲页刷盘由 buffer manager 发起；WAL 与 checkpoint 决定何时必须保证日志/数据顺序。`pg_fsync` 提供同步落盘操作接口，但并不单独定义数据库级恢复保证。

## 怎么实现的

打开入口 `PathNameOpenFile` 委托带权限版本完成打开前准备，包括 fd 预算控制与 LRU 回收；成功后以虚拟编号索引 VFD 元数据。LRU 记录由内部链表维护，实际 OS fd 可关闭而逻辑 VFD 仍然有效。再次读写时，底层操作在需要时重新打开路径并恢复适当状态；因此调用者必须持续使用 VFD API，而不能把底层 OS fd 当成稳定身份。

`FileClose` 关闭底层 fd 并释放 VFD 项关联资源。关闭还要处理临时文件生命周期；临时文件是否删除取决于其创建/标记属性，不应把普通文件关闭等同于删除。文件操作封装还处理 seek/read/write、错误传播和跨平台差异。`pg_fsync` 按平台能力选择同步实现，并遵循配置的写穿透策略；其输入是实际 fd，而上层调用负责选定正确的持久化时点。

并发边界主要是后端私有 VFD 状态：一般由拥有它的进程操作，不是供多个后端共享的文件句柄表。外部 fd 使用方通过预算保留接口协调上限，避免 VFD 误以为全部 fd 都可回收。资源所有者及错误清理路径负责释放适当文件资源；本机制不替调用者建立事务回滚语义。

## 关键取舍

- 以逻辑句柄换取可控的 OS fd 占用，代价是重开时需保留足够元数据且某些访问会增加系统调用。
- 选择 LRU 回收而非拒绝新打开，可让工作集大于 fd 限额的后端继续工作；若改成始终保持所有 fd 打开，容易触及进程/系统限制；若每次 I/O 都无条件开关文件，则会产生显著额外开销。
- 将文件持久化接口与数据库写入顺序分离，避免 VFD 擅自推断事务或 checkpoint 完成状态；调用者必须正确安排同步时机。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/file/fd.c` | `PathNameOpenFile` | 建立虚拟文件句柄并进入 fd 预算管理 |
| `src/backend/storage/file/fd.c` | `ReleaseLruFiles` | 回收可回收的 OS fd |
| `src/backend/storage/file/fd.c` | `FileClose` | 关闭并清理 VFD |
| `src/backend/storage/file/fd.c` | `pg_fsync` | 封装同步文件刷新 |

## 参考

- [PostgreSQL 官方文档：文件布局](https://www.postgresql.org/docs/current/storage-file-layout.html)
- [PostgreSQL 官方文档：可靠性与 WAL](https://www.postgresql.org/docs/current/wal-reliability.html)
- [PostgreSQL 源码：fd.c](https://github.com/postgres/postgres/blob/master/src/backend/storage/file/fd.c)

## 与主核 / AI 的关系

VFD 适用于需要跨多种文件类型复用的资源约束管理；它不构成主核的关系存储格式，也不提供 AI 特定能力。主核若复用此思路，应保留稳定逻辑句柄与可回收底层资源的分层，同时明确句柄有效期和同步责任。
