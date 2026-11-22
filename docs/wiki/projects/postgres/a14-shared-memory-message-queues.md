---
project: postgres
feature: shared-memory-message-queues
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/ipc/shm_mq.c
    symbols: [shm_mq_create, shm_mq_send, shm_mq_receive]
---

# 共享内存消息队列

## 一句话职责

shm_mq 在共享内存环形缓冲区上提供单发送者到单接收者的消息传递。

## 机制是什么

队列本体嵌入调用方提供的共享内存区域，通常可位于 DSM 中；队列并不自行创建或拥有 DSM 段。每个队列只允许一个 sender 和一个 receiver，双方都需要 PostgreSQL 的 PGPROC，以使用进程 latch 等待/唤醒。队列负责字节环、消息长度边界、背压与对端 detach 处理，不负责序列化消息内容或管理多发送者/多接收者拓扑。

## 怎么实现的

`shm_mq_create` 初始化队列头、环缓冲区大小、读写原子计数及端点身份槽。发送/接收句柄是进程私有对象，可在 attach 时关联 DSM，以便在 DSM detach 前先脱离队列；也可关联后台 worker 句柄，以识别对端启动失败。

环区用单调读写字节计数表示未读范围，写入端只能写空闲区，读取端只能推进已消费位置。各自拥有的计数以原子 64 位访问，配合内存屏障保证数据可见性；端点指针由短暂自旋锁保护。`shm_mq_send` 封装为向量发送，消息先写入 `Size` 长度字，再写 payload。满环时阻塞模式等待进程 latch 后续写；nowait 模式返回 WOULD_BLOCK，调用者须以相同参数重试，不能中途改消息。写入端会批量发布已写字节以减少缓存行争用和 latch 唤醒，`force_flush` 可要求立即发布。接收端依长度读取完整消息；连续且不跨环边界的数据可直接返回环内指针，大消息或环绕消息则复制到后端本地缓冲。发送方必须明确每条消息的结束边界。

## 关键取舍

- **SPSC 专用而非 MPMC**：读写端各自单写计数，避免每个字节操作都锁住队列；换成多生产者/消费者需额外仲裁序列与同步。
- **环区零拷贝优先**：连续消息可直接引用共享区，减少复制；换成始终复制会简化调用者生命周期，却提高带宽与分配成本。
- **批量发布字节**：降低原子写与 latch 成本；换成每次写入立即通知会更低延迟但有更多同步开销。
- **阻塞与 nowait 双模式**：支持简单同步流程及事件循环整合；非阻塞调用一旦开始必须延续同一消息，不能任意取消。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/ipc/shm_mq.c` | `shm_mq_create` | 初始化队列本体及环缓冲区元数据 |
| `src/backend/storage/ipc/shm_mq.c` | `shm_mq_send` / `shm_mq_sendv` | 发送一条消息或向量消息并处理背压 |
| `src/backend/storage/ipc/shm_mq.c` | `shm_mq_receive` | 接收完整消息并处理等待/复制 |
| `src/backend/storage/ipc/shm_mq.c` | `shm_mq_attach` | 建立本进程端点句柄 |

## 参考

- [PostgreSQL 官方文档：后台工作进程](https://www.postgresql.org/docs/18/bgworker.html)
- [PostgreSQL 18 官方文档：并行查询概念](https://www.postgresql.org/docs/18/parallel-query.html)
- [PostgreSQL 源码：shm_mq.c](https://github.com/postgres/postgres/blob/master/src/backend/storage/ipc/shm_mq.c)

## 与主核 / AI 的关系

shm_mq 常用于协调 PostgreSQL 后台工作进程，是 DSM 之上的消息原语而非独立内存管理器。AI 可把长度前缀、环区读写游标和 latch 视作流控协议；不能将其误描述成任意多端队列或持久消息代理。
