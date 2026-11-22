---
project: postgres
feature: aio-io-methods
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/aio/aio.c
    symbols: [pgaio_io_acquire, pgaio_io_release]
  - path: src/backend/storage/aio/method_worker.c
    symbols: [pgaio_worker_submit]
  - path: src/backend/storage/aio/method_io_uring.c
    symbols: [pgaio_uring_check_capabilities, pgaio_uring_context_shmem_size]
---

# 异步 I/O 请求与执行后端

## 一句话职责

PostgreSQL AIO 为底层读写提供有界请求句柄与统一完成管理，再由可选执行方法提交或执行请求。

## 机制是什么

通用层（`aio.c`）管理每后端的 I/O handle 生命周期、结果、回调、资源所有者与等待引用；I/O 调用经由缓冲管理、smgr、md、fd 等层传递 handle，最终由方法后端执行。方法是可替换的执行后端，不是上层读流策略：read stream 决定何时预取/合并，AIO 决定请求如何提交与完成。

worker 方法通过共享提交队列把可安全重开文件的操作交给专用 worker；队列满或不能安全转交的请求会同步执行。io_uring 方法使用 Linux io_uring。启用方法受构建、平台、内核/库能力及配置约束；不能从存在实现源码推断该后端在任意平台始终可用。AIO 也不保证每个请求都真正异步执行。

## 怎么实现的

`pgaio_io_acquire` 在当前后端空闲句柄不足时等待已有 I/O 释放；非阻塞版本可返回 NULL。每后端限制最多一个已借出但尚未提交/释放的 handle，以便等待在途操作完成后能够保证有句柄可用。未使用的已借 handle 必须 `pgaio_io_release`。start 接口把请求置为 staged，非批处理时可立即提交；进入 batch mode 时先暂存后统一提交。请求启动后调用者不再直接持有该 handle；可在启动前取得等待引用，并通过完成结果接口获知成功/错误。ResourceOwner 清理负责错误路径上的引用与资源回收。

worker 后端 `pgaio_worker_submit` 先准备 staged 请求，再在提交队列锁可得时逐项入队并唤醒空闲 worker。锁竞争或队列容量不足时，剩余请求改由当前进程同步执行。worker 执行本身也有同步执行条件（例如涉及本地引用、无法安全重开文件时），不是无条件异步。

io_uring 后端先探测内核/liburing 能否以用户提供内存初始化 ring；若支持则计算共享上下文和 ring 所需主共享内存，并初始化每进程 ring 上下文。该探测通过尝试创建 ring 判断能力；失败意味着不能采用该共享内存映射优化，并不等于 PostgreSQL AIO 整体不可用。后端仍需处理 ring 提交、完成及资源清理。

## 关键取舍

- **统一 handle API 与多个执行方法分层**：存储层不绑定 worker 或 io_uring；换成每个存储层直接调用 OS 异步 API，会让平台差异渗入缓冲/文件路径。
- **有限 handle 池与背压**：资源不足时等待或返回空；换成无限排队会把内存与内核请求压力转为不可控积压。
- **worker 队列退化同步执行**：保证队列竞争/满时仍能推进；换成只允许排队会增加失败分支或阻塞等待。
- **io_uring 用户内存能力探测**：减少每个 ring 的额外映射数量，但依赖较新的内核/库能力；不能把优化能力当作 AIO 正确性的前提。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/aio/aio.c` | `pgaio_io_acquire` / `pgaio_io_acquire_nb` | 取得有界 handle，资源紧张时等待或返回空 |
| `src/backend/storage/aio/aio.c` | `pgaio_io_release` | 归还未实际使用的 handle |
| `src/backend/storage/aio/method_worker.c` | `pgaio_worker_submit` | 提交到共享队列、唤醒 worker 或同步执行 |
| `src/backend/storage/aio/method_io_uring.c` | `pgaio_uring_check_capabilities` | 探测用户提供 ring 内存支持 |
| `src/backend/storage/aio/method_io_uring.c` | `pgaio_uring_context_shmem_size` | 计算 ring 上下文共享内存需求 |

## 参考

- [PostgreSQL 18 官方文档：异步 I/O 配置](https://www.postgresql.org/docs/18/runtime-config-resource.html#GUC-IO-METHOD)
- [PostgreSQL 18 官方文档：AIO 相关配置参数](https://www.postgresql.org/docs/18/runtime-config-resource.html)
- [Linux 内核文档：io_uring](https://docs.kernel.org/io_uring/index.html)
- [PostgreSQL 源码：AIO 子系统说明](https://github.com/postgres/postgres/blob/master/src/backend/storage/aio/README.md)

## 与主核 / AI 的关系

AIO 是 PostgreSQL 存储读写路径的统一提交/完成基础设施，worker 与 io_uring 是后端实现选择。AI 需分别识别预取策略、请求句柄生命周期与具体执行后端，不能把“请求已提交”说成“已完成”，也不能断言所有路径都异步。
