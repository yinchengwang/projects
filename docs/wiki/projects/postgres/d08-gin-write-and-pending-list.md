---
project: postgres
feature: gin-write-and-pending-list
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/gin/gininsert.c
    symbols: [ginEntryInsert, ginbuild]
  - path: src/backend/access/gin/ginfast.c
    symbols: [ginHeapTupleFastInsert, ginInsertCleanup]
---

# GIN 写入与 pending list

## 一句话职责

GIN 写入把 heap tuple 拆成倒排键项，并可先追加到 pending list，再批量并入主索引以降低逐项写树成本。

## 机制是什么

GIN 将一个 heap tuple 的多个属性键映射为 entry tree 中的键，并在键下维护对应 heap TID 集合。`ginbuild` 以 bulk-build 方式扫描 heap 并填充索引；常规 `aminsert` 则可按 fastupdate 配置将收集到的键/TID 先写入 pending list，稍后统一合并到常规 entry/posting 结构。

pending list 是查询必须考虑的索引组成部分，不是可忽略的缓存；搜索会同时查 pending list 与主索引。它不负责决定 heap tuple 的事务可见性，回表后的 heap/table-AM 仍执行可见性判断。`ginEntryInsert` 的定义在 `gininsert.c`；pending-list cleanup 位于 `ginfast.c`，会调用该入口把聚合键写入主索引。

## 怎么实现的

1. GIN AM 插入入口收集一个 heap tuple 的属性键，生成按键聚合的 `GinTupleCollector`，其中包含对应 IndexTuple/TID 数据；未启用 fastupdate 时，键逐项走主索引插入路径。
2. fastupdate 路径调用 `ginHeapTupleFastInsert`。它计算是否能追加至当前 tail page；若收集批次超过单页容量或 tail 空间不足，则先构造 sublist，再在 metapage 独占 buffer lock 下接到 pending list 尾部。
3. 追加页内容时持有 metapage 与 tail buffer 的排他锁，并更新 `GinMetaPageData` 的 head/tail、tailFreeSize、pending page/tuple 计数。修改按 `RelationNeedsWAL` 选择 WAL 记录；写入时也检查 Serializable predicate conflict。
4. pending list 达到清理阈值等条件时，可触发 `ginInsertCleanup`。常规插入触发的 cleanup 以条件页锁尝试独占 metapage；若另一个 cleanup 正在运行便退出，让并发清理者继续处理。VACUUM、ANALYZE 或显式清理等强制路径则等待取得锁。
5. cleanup 记录开始时 tail 作为处理边界，读取 pending pages 并用 `BuildAccumulator` 聚合同键 TID；到达 flush 条件时先释放当前 pending page 锁，将聚合键经 `ginEntryInsert` 写入主索引，然后重新加锁并检查该页在未锁期间是否新增条目。
6. 确认已转入主索引的 pending pages 通过 shiftList 从链表移除并回收；强制清空会继续处理新追加内容，普通清理则至少处理开始时已观察到的 tail。清理结束后可更新 FSM，临时聚合状态在操作 memory context 中释放。

## 关键取舍

- **源码事实：**pending list 允许把多条写入先追加为线性页链，cleanup 再用 accumulator 聚合同键并批量插入；GIN 查询会先扫描 pending list 再扫描主索引。**架构推论：**以查询需额外扫描未清理内容、并可能有 cleanup 延迟为代价，减少每条 heap tuple 都随机更新主索引树的成本；若完全禁止缓冲写入，可降低读路径状态，却增加写放大与页锁竞争。
- **源码事实：**普通插入触发 cleanup 若抢不到 metapage 页锁就直接返回，强制清理则阻塞等待。**架构推论：**后台/普通写者避免在并发 cleanup 时堆积等待，VACUUM 和用户显式清理则提供必须清空的完成语义；所有场景都阻塞会延长写事务，所有场景都立即退出则无法满足强制清理调用者的预期。
- **源码事实：**cleanup 先记住初始 tail，避免持续并发追加使普通清理无法结束；强制清空会忽略该界限继续处理。**架构推论：**将有界进度与“清空到空”语义分开，可维持普通插入进度并为维护命令提供完整清理；若所有清理追逐最新 tail，持续写入下可能长期不结束。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/gin/gininsert.c` | `ginEntryInsert` | 函数定义于此，负责将键及 TID 集合并入 GIN 主结构；cleanup 从 `ginfast.c` 调用。 |
| `src/backend/access/gin/gininsert.c` | `ginbuild` | heap bulk build 入口，初始化并执行 GIN 索引构建。 |
| `src/backend/access/gin/ginfast.c` | `ginHeapTupleFastInsert` | 把收集的索引项追加到 pending list 并维护 metapage/WAL。 |
| `src/backend/access/gin/ginfast.c` | `ginInsertCleanup` | 聚合 pending entries、写入主索引并移除已处理页面。 |

## 参考

- [PostgreSQL 文档：GIN 索引](https://www.postgresql.org/docs/current/gin.html) —— 说明 GIN 的倒排结构及 fastupdate/pending list 行为。
- [PostgreSQL 官方源码：GIN pending list](https://github.com/postgres/postgres/blob/master/src/backend/access/gin/ginfast.c) —— 对照追加、清理和并发锁处理。

## 与主核 / AI 的关系

暂无直接对应。pending list 可作为“前台追加、后台批量归并”索引写优化的参照；主核采用前需评估查询合并成本、事务可见性和崩溃恢复一致性。
