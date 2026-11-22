---
project: postgres
feature: wait-event-sets
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/ipc/waiteventset.c
    symbols: [CreateWaitEventSet, AddWaitEventToSet, WaitEventSetWait]
---

# 等待事件集

## 一句话职责

WaitEventSet 将 latch、socket、超时和 postmaster 存活等异构等待源统一为一次可中断的事件等待。

## 机制是什么

事件集允许一个后端在一次等待调用中监视多个事件，并通过平台相关后端实现 poll/epoll/kqueue 等等待能力。核心目标包括避免检查 latch 后到进入系统等待之间丢失唤醒，以及使信号处理与 socket 就绪等待可协调。

它是等待原语抽象，不是线程调度器、网络协议栈或任务队列。Latch 表达进程内/进程间通知意图；socket 由调用者拥有并管理；`WaitEventSetWait` 返回就绪事件，调用者决定处理逻辑。它也不保证事件对应的业务操作成功，只报告等待条件变化。

## 怎么实现的

`CreateWaitEventSet` 分配集合及容量、平台事件数组和资源所有者清理信息。`AddWaitEventToSet` 把事件类型、socket、latch 及用户数据绑定到集合项，并更新后端等待设施所需的登记状态。集合可以复用，避免循环中重复创建底层 poller 对象。

`WaitEventSetWait` 首先处理已设置 latch、超时等可直接满足的条件，再进入平台等待调用。实现通过 latch 的自管通知机制与系统事件等待整合，关键在于把检查和睡眠之间的竞态封闭：若通知在进入等待前发生，也必须能够被观察。返回数组包含命中事件的类型及关联数据；一次调用可能返回多个事件，也可能因 timeout 返回零个。

不同平台的等待后端会维护自己的 fd 注册或唤醒 fd，且处理信号、中断和重复等待的细节不同。集合的创建、删除、关闭 socket 等生命周期需与所有者协同；WaitEventSet 不拥有 socket 的业务生命周期。调用者应在返回后重新检查真实状态，因为就绪通知不等价于完整读写。

## 关键取舍

- 统一接口隐藏 poller 平台差异，代价是抽象需覆盖不同平台事件能力；若业务代码直接调用 poll/epoll，平台代码会扩散且 latch 竞态容易各自处理不一致。
- 复用事件集降低重复注册成本，但增加状态生命周期管理；每次等待临时建集更简单，却在高频循环产生额外分配和系统调用。
- 允许多个事件同时返回而非只报告首个命中，改善处理吞吐；调用方需正确处理一批事件与部分状态变化。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/ipc/waiteventset.c` | `CreateWaitEventSet` | 创建等待集合 |
| `src/backend/storage/ipc/waiteventset.c` | `AddWaitEventToSet` | 登记一种等待源 |
| `src/backend/storage/ipc/waiteventset.c` | `WaitEventSetWait` | 等待并返回就绪事件 |
| `src/include/storage/waiteventset.h` | `WaitEventSet` | 对外等待集合接口定义 |

## 参考

- [PostgreSQL 官方文档：等待事件](https://www.postgresql.org/docs/current/monitoring-stats.html#WAIT-EVENT-TABLE)
- [PostgreSQL 官方文档：Latch 接口](https://www.postgresql.org/docs/current/xfunc-c.html#WAIT-FOR-EVENTS)
- [PostgreSQL 源码：waiteventset.c](https://github.com/postgres/postgres/blob/master/src/backend/storage/ipc/waiteventset.c)

## 与主核 / AI 的关系

主核中的网络服务、后台任务可采用事件集模型合并 I/O 与内部通知等待；需自行保证通知不会在检查与睡眠间丢失。AI 推理任务的完成队列可作为事件来源，但 WaitEventSet 本身不提供推理调度和结果语义。
