---
project: postgres
feature: read-stream-prefetch
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/aio/read_stream.c
    symbols: [read_stream_begin_impl, read_stream_next_buffer, read_stream_end]
---

# 扫描预读流

## 一句话职责

ReadStream 把扫描者提供的块序列变成有界、可提前启动并可合并的缓冲区读取。

## 机制是什么

它处于扫描/维护代码与缓冲管理及 AIO 读接口之间：调用方以回调逐块提供关系块号，ReadStream 缓冲已固定的 Buffer，并提前调用底层批量读取操作；消费端仍按流顺序取 Buffer。它不决定扫描谓词、块访问顺序，也不保证数据一定命中缓存或磁盘操作必然异步。临时关系受限：不能读取其他会话的临时表，因为当前后端看不到其本地缓冲区。

其目标是降低小块逐次读取的等待并提供相邻 I/O 合并机会，不是关系缓存替代物，也不是通用任意文件流。

## 怎么实现的

`read_stream_begin_impl` 按关系/表空间及维护标志确定可并发 I/O 数，并据 `io_combine_limit`、缓冲策略 pin 限额构造有限环形缓冲区及 I/O 队列。回调返回下一块号，`InvalidBlockNumber` 表示结束；也可为每个缓冲区附带私有数据。实现分别维护缓冲区环队列与已启动但尚待完成的 I/O 队列，避免把“已 pin 可交付”与“仍需等待 I/O”混为一事。

`read_stream_next_buffer` 以有序方式取出缓冲区，同时继续向前探测并通过 StartReadBuffers/WaitReadBuffers 一类接口启动、等待操作。相邻块可合并，未相邻块各自形成请求；look-ahead 和合并距离分开调节：缓存命中时逐步收缩，发生需要等待的 miss 时提高预读距离以尝试增加并发，发生需 I/O 的块流时提高合并收益。全缓存且无每块私有数据时有专门快速路径；它只同步探测一步，遇到 miss 后退出快速路径。`read_stream_end` 负责结束流并释放其持有的缓冲区/状态；消费者可以提前终止。

## 关键取舍

- **有界 pin 与有限前瞻**：以最多 pin 的缓冲量约束资源占用；换成无界预读会扩大缓存压力，并可能挤出其他工作集。
- **分别控制合并与预读**：即使很浅前瞻也可能合并较大的读取；换成单一距离参数，会把并发等待隐藏与 I/O 大小优化绑定。
- **自适应而非固定深度**：命中流减少无收益前瞻，miss 等待时尝试加深；换成固定深度，可能在缓存热路径增加管理开销，或在冷路径预读不足。
- **按顺序消费**：内部可并行启动 I/O，但对调用者保持块流顺序；不负责重排扫描语义。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/aio/read_stream.c` | `read_stream_begin_impl` | 计算队列、pin 与 I/O 并发限制并创建流 |
| `src/backend/storage/aio/read_stream.c` | `read_stream_next_buffer` | 拉取块号、预读、等待并按序交付 Buffer |
| `src/backend/storage/aio/read_stream.c` | `read_stream_end` | 结束读取流并清理其资源 |

## 参考

- [PostgreSQL 18 官方文档：异步 I/O](https://www.postgresql.org/docs/18/runtime-config-resource.html#GUC-IO-METHOD)
- [PostgreSQL 18 官方文档：资源使用配置（I/O 并发）](https://www.postgresql.org/docs/18/runtime-config-resource.html)
- [PostgreSQL 源码：read_stream.c](https://github.com/postgres/postgres/blob/master/src/backend/storage/aio/read_stream.c)

## 与主核 / AI 的关系

它属于 PostgreSQL 存储读路径的预取编排层；不改变 SQL 扫描逻辑或缓冲池一致性协议。AI 侧可将回调块序列、队列与 I/O 队列分别视作供给、待消费数据和执行中的工作，而不能据此推断任意存储读都已并行完成。
