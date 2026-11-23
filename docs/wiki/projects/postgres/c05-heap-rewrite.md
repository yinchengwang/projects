---
project: postgres
feature: heap-rewrite
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/heap/rewriteheap.c
    symbols: [begin_heap_rewrite, rewrite_heap_tuple, end_heap_rewrite]
---

# Heap 重写

## 一句话职责

Heap 重写把源堆中仍需保留的 tuple 重新写入目标堆，并在新 TID 变化时维护旧更新链到新位置的对应关系。

## 机制是什么

`begin_heap_rewrite` 建立 `RewriteState`，将源/目标关系、冻结 cutoff、目标块位置、批量写状态和 update-chain 映射表放在独立 memory context 中。上层按扫描结果逐个提交存活或 recently-dead tuple；`rewrite_heap_tuple` 复制事务字段，按冻结 cutoff 冻结可冻结版本，再写入新 heap。映射用于修复仍需保留的更新链 `t_ctid`，因为重写后原 tuple 的物理位置已改变。

这是重写数据面的机制，不负责决定 CLUSTER 排序键、VACUUM FULL 的命令语义、表锁获取或索引重建策略；调用方负责扫描源数据、判断 dead tuple 并传入正确 cutoff。重写结束会写出目标缓冲页、冲刷 bulk writer 并清理状态。这里讲的是 heap rewrite 及新旧映射，不讲普通 tuple 插入布局或 TOAST 的独立压缩规则。

## 怎么实现的

1. `begin_heap_rewrite` 在子 `MemoryContext` 中分配 `RewriteStateData`，记录 `rs_old_rel`、`rs_new_rel`、`rs_oldest_xmin`、`rs_freeze_xid`、`rs_cutoff_multi`、目标起始块和 bulk write 状态；建立 `rs_unresolved_tups` 与 `rs_old_new_tid_map` 两张 hash 表处理可能乱序遇到的更新链。
2. `rewrite_heap_tuple` 将源 tuple 的事务字段拷至临时目标 tuple，清理不应原样继承的状态位并调用 `heap_freeze_tuple`；先把新 tuple 的 `t_ctid` 设为无效，表示写入时默认自指，若能解析更新后继再改为后继的新位置。
3. 对指向另一个仍可能存活版本的 tuple，按 `(update xid, old ctid)` 查映射。若后继已写入，直接设置新 `t_ctid` 并删除映射；若后继尚未出现，则把旧 TID 与 tuple 副本放进 unresolved 表，暂缓写入。
4. 写出一个后继版本后得到新 TID。若其旧版本还不是可立即丢弃的 dead tuple，就查 unresolved 表并回溯写出等待中的前驱、修复其新 `t_ctid`；否则记录“前驱 old TID → 后继 new TID”以待后续前驱出现。该处理通过循环逐步解析连续链。
5. `raw_heap_insert` 负责把 tuple 写入目标 heap 的页缓冲并更新新 TID；若数据仍含外部 TOAST 引用或过大，先经 toaster 为新关系处理。页面由 `smgr_bulk` 批量写出，减少逐页普通写入开销。
6. `end_heap_rewrite` 将仍 unresolved 的 tuple 作为安全回退写出并清除其无效 `t_ctid`，随后写最后一页、结束 bulk writer、结束 logical rewrite 映射记录，并删除整个 rewrite memory context，统一释放状态与映射。

## 关键取舍

- 源码事实：tuple 顺序可能使更新链后继先于前驱，rewrite 用 unresolved tuple 暂存和 old-to-new TID hash 表解决先后顺序；收尾时未匹配项也写出而不是丢弃。架构推论：内存与 hash 查找换取链关系正确迁移；假定输入总按链顺序排列可省状态，但 `CLUSTER` 等扫描顺序未必保持版本链顺序，可能生成悬空或错误 ctid。
- 源码事实：对 still-recently-dead 的更新链版本可能保留并链接；死 tuple 由 caller 的 `rewrite_heap_dead_tuple` 路径登记，映射匹配使用 update xid 与 TID。架构推论：必须区别“对本次 rewrite 可删除”与“某些 snapshot 仍可能需要的旧版本”；一律只留最新版本会破坏并发 snapshot 所需版本链。
- 源码事实：重写中对达到 cutoff 的 xmin/xmax 可直接冻结，并以 `smgr_bulk` 缓冲目标页。架构推论：在完整重写时顺带减少未来 vacuum 冻结工作，且批量 I/O 提升连续写入效率；若完全保留旧事务年龄，会增加后续维护负担，若逐页普通写则失去顺序批量写带来的调度优势。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/heap/rewriteheap.c` | `begin_heap_rewrite` | 创建 rewrite 生命周期状态、目标 bulk writer 与更新链映射结构。 |
| `src/backend/access/heap/rewriteheap.c` | `rewrite_heap_tuple` | 复制/冻结 tuple 状态、在新旧 TID 间修复更新链并提交目标写入。 |
| `src/backend/access/heap/rewriteheap.c` | `end_heap_rewrite` | 处理未解析的保守残项，写尾页并释放 writer 与全部 rewrite 状态。 |

## 与主核 / AI 的关系

主核没有 PostgreSQL 式 heap tuple 多版本链与 CLUSTER/VACUUM FULL 的统一 rewrite 层。可以参考将跨关系迁移映射和批量写入封装在单次 rewrite state 中；不能认为主核已经采用 `t_ctid` 更新链或同样的冻结规则。

## 参考

- [PostgreSQL 文档：VACUUM FULL](https://www.postgresql.org/docs/current/sql-vacuum.html) —— 说明 FULL vacuum 重写表并获得更紧凑布局的行为。
- [PostgreSQL 官方源码：heap rewrite](https://github.com/postgres/postgres/blob/master/src/backend/access/heap/rewriteheap.c) —— 支撑 rewrite state、tuple 映射和收尾流程。
