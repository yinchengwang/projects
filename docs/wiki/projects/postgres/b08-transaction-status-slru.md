---
project: postgres
feature: transaction-status-slru
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/transam/slru.c
    symbols: [SimpleLruReadPage, SimpleLruWritePage]
---

# SLRU 状态页缓存实现

## 一句话职责

SLRU 为按页组织、可循环回收的事务辅助数据提供共享内存页缓存，并协调页替换、I/O 与同步请求。

## 机制是什么

SLRU 是共享页缓存抽象，不定义页里的业务语义。调用方以 `SlruDesc`/`SlruOpts` 配置目录、段名、bank buffer 数量、同步 handler 和页序比较函数；共享数据记录每 slot 的页号、`page_status`、dirty 位、LRU 计数、buffer lock 与 bank lock。CLOG、SUBTRANS、MultiXact 等组件各自解释缓存页内容及何时截断。

本页只讲读写缓存、页替换、物理 I/O 与同步抽象，不讲具体 XID 提交状态（b09）、XID 分配和查询语义，也不讲 MultiXact 的 offset/member 布局（b06）。Bank lock 保护相应分区内共享元数据，单 buffer lock 协调该 slot 的 I/O；实际磁盘分段命名和页内编码由配置者决定。

## 怎么实现的

1. 调用方取得目标页所在 bank 的 exclusive LWLock 后调用 `SimpleLruReadPage`。`SlruSelectLRUPage` 先查该 bank 是否已有目标页，否则选 EMPTY 或最久未用可回收 slot；最新页不会被淘汰，若候选正 I/O 中且无更优候选则等待并重试。
2. 命中页若仍 read-in-progress，或调用者不允许读取 write-in-progress 页，则用 `SimpleLruWaitIO` 等待 slot I/O 完成后重查状态。允许读写并发的调用者可根据自身数据语义选择 `write_ok`。
3. 未命中时先将 slot 标为 `READ_IN_PROGRESS` 并取得 per-buffer exclusive lock，再释放 bank lock执行物理读；读完重新取得 bank lock发布 VALID 或 EMPTY 状态，释放 buffer lock。失败在状态一致后经 callback 报告，避免持有共享控制锁做慢 I/O。
4. `SimpleLruWritePage` 调用内部写路径：bank lock 下检查页 dirty、标记 WRITE_IN_PROGRESS 并先清 dirty，再取得 buffer lock、释放 bank lock写盘。I/O 期间若并发写者再次修改页会重新置 dirty；完成时回收 VALID 状态，失败则恢复 dirty 后报错。
5. checkpoint 可用 `SimpleLruWriteAll` 写出所有脏页，并将文件同步任务交给配置的 sync handler；是否每页需要 WAL 先刷由 `group_lsn` 配置决定（例如 pg_xact 使用，其他 SLRU 不使用）。因此 SLRU 是页缓存和 I/O 管理层，既不负责事务日志提交，也不单独制定 WAL-before-data 语义。

## 关键取舍

- **bank 控制锁与 per-buffer I/O 锁分离（源码事实）**：慢磁盘 I/O 期间释放 bank lock，其他 slot 仍可查找/管理；代价是必须在 I/O 前后明确状态并重新验证 slot 页号。若持 bank lock 跨越 I/O，某页慢读写会阻塞同 bank 全部缓存操作。
- **分 bank 的简单 LRU 而非全局哈希/精细置换结构（源码事实）**：页请求只扫描目标 bank，降低元数据和同步复杂度；代价是候选范围局部化，替换不是全局最优。最新页通过 `latest_page_number` 排除，减少热点尾页来回换入换出。
- **写入时先清 dirty，允许并发 redirty（源码事实）**：写盘开始后新修改会重新置 dirty；写成功不能错误地清除写入期间产生的新脏状态。若写期间禁止所有更新，需要让写者持有更长的独占锁，造成额外等待。
- **共用页缓存，不合并具体状态语义（架构推论，源码结构支持）**：各 SLRU 用相同页生命周期管理而提供不同命名、截断和错误回调，意味着共享缓存模块可复用但不能由其单独推断事务状态；替代为一个专用通用事务表会把不同 wraparound 与记录格式强行耦合。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/transam/slru.c` | `SimpleLruReadPage` | 查缓存或选 slot，协调在途 I/O 并读取缺页 |
| `src/backend/access/transam/slru.c` | `SimpleLruWritePage` | 将指定有效页交给内部脏页写出流程 |

## 与主核 / AI 的关系

暂无直接对应。SLRU 可类比为面向辅助元数据的页缓存抽象，但主核若有页管理不能因此推定采用同一分 bank LRU 或锁协议。

## 参考

- [PostgreSQL 官方源码：SLRU 实现](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/slru.c) —— 支撑 bank/slot 状态、读写 I/O 及 checkpoint 写出。
- [PostgreSQL 官方源码：SLRU 结构定义](https://github.com/postgres/postgres/blob/master/src/include/access/slru.h) —— 支撑共享 slot 元数据、锁与调用配置接口。
- [PostgreSQL 文档：资源消耗配置](https://www.postgresql.org/docs/current/runtime-config-resource.html) —— 提供事务状态缓存配置背景，具体缓存机制以源码为准。
