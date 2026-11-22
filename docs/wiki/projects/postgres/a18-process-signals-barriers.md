---
project: postgres
feature: process-signals-barriers
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/ipc/procsignal.c
    symbols: [ProcSignalInit, SendProcSignal, EmitProcSignalBarrier]
---

# 进程信号与屏障

## 一句话职责

ProcSignal 将带原因标记的进程通知与可等待的全局状态屏障统一到共享进程槽机制中。

## 机制是什么

ProcSignal 是 PostgreSQL 进程间通知设施。共享槽按 ProcNumber 索引，登记目标 PID、若干信号原因标志和屏障代数；操作系统信号（主要是 SIGUSR1）负责唤醒目标，目标随后在安全的常规执行路径中处理共享标志。单次信号可合并，因此该设施传递的是“有某类工作待处理”的提示，而不是可靠计数队列。

屏障是更强的确认协议：调用方先使相关共享状态可见，再发布屏障类型；每个参与进程在安全路径中执行该类型预先定义的处理动作，并发布已吸收的 generation，调用方可等待所有参与者达到指定代数。它不是由调用方提供任意回调的机制，也不自动保证任意共享状态已处理；调用方须选择能覆盖目标状态的屏障类型。ProcSignal 不负责定义业务状态、不执行通用失效动作，也不是所有数据库锁或事务同步的通用替代品。

## 怎么实现的

共享内存中的 `ProcSignalHeader` 持有全局屏障代数和 `ProcSignalSlot` 数组。槽含原子 PID、取消密钥、受自旋锁保护的原因标志、屏障检查掩码、已确认代数及条件变量。`ProcSignalInit` 使用当前进程槽，清除遗留标志并登记 PID；设置 PID 与读取全局代数间使用内存屏障，避免新加入进程漏掉已经发布的屏障。退出回调清空 PID、把代数置为最大值以免等待者卡在已退出槽，并唤醒条件变量等待者。

`SendProcSignal` 可由 ProcNumber 快速定位，也可按 PID 搜索。它在槽锁下核对 PID 并置相应原因标志，再发送 SIGUSR1；同一原因短时间重复置位可能合并，失败时返回错误而不是保证投递。信号处理器只设置待处理状态，实际工作延迟到安全的中断处理路径。

`EmitProcSignalBarrier` 对所有槽发布指定类型检查位，递增全局 generation，再逐槽设置 barrier 原因并唤醒已登记进程。进程在 `ProcessProcSignalBarrier` 路径按该类型执行固定处理动作，并在动作完成后更新自身 `pss_barrierGeneration`、广播条件变量。`WaitForProcSignalBarrier` 逐槽等待目标 generation；它读取代数而非检查掩码，因为掩码会先于实际动作完成而清除。结束时再执行内存屏障，约束调用方之后的共享状态访问顺序。调用方必须确保屏障类型对应的固定动作正是所需处理步骤。

屏障可能中断所有后端，源码明确不适合高频调用。并发加入者因初始化时读取最新 generation 而可视为已具备此前可见状态；退出槽通过最大代数越过等待。屏障调用者需要负责在发出前发布状态，并确保对应类型的处理动作覆盖真正要同步的缓存或全局状态。

## 关键取舍

原因位图加进程信号避免为每种事件维护独立消息队列，且可以自然合并重复通知；代价是接收端必须能从共享状态判断要做什么，不能依赖通知次数。若改为每次事件都写入可靠队列，事件顺序与计数更明确，却需要队列容量、背压和回收协议。屏障代数为“所有进程已观察状态”提供确认，但广播与等待成本随进程数增长，因此只适用于不频繁的全局协调。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/ipc/procsignal.c` | `ProcSignalInit` | 注册当前进程槽及初始屏障状态 |
| `src/backend/storage/ipc/procsignal.c` | `SendProcSignal` | 设置原因标志并唤醒指定进程 |
| `src/backend/storage/ipc/procsignal.c` | `EmitProcSignalBarrier` | 发布检查类型、代数并广播 |
| `src/backend/storage/ipc/procsignal.c` | `WaitForProcSignalBarrier`、`ProcessProcSignalBarrier` | 等待确认及在安全上下文吸收屏障 |

## 参考

- [PostgreSQL 源码 API 文档：ProcSignal 实现](https://doxygen.postgresql.org/procsignal_8c.html)
- [PostgreSQL 官方文档：后台进程](https://www.postgresql.org/docs/current/bgworker.html)

## 与主核 / AI 的关系

屏障使主核能在某些全局状态变更后确认参与进程已经执行对应处理，但屏障协议本身不解释某种状态的语义。AI 应区分操作系统唤醒、原因标志和业务处理完成三个阶段，也不能把普通 ProcSignal 当作确认式消息。
