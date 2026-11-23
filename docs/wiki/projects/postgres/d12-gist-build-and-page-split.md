---
project: postgres
feature: gist-build-and-page-split
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/gist/gistbuild.c
    symbols: [gistbuild]
  - path: src/backend/access/gist/gistutil.c
    symbols: [gistfillbuffer, gistfitpage]
---

# GiST 构建与页拆分

## 一句话职责

GiST 构建把堆行转成索引项并形成多层树，页填充工具检查容量并按指定顺序写入索引 tuple。

## 机制是什么

构建器可选排序自底向上、普通逐项插入，或在非排序构建中采用缓冲构建；排序构建要求所有索引列提供 sort support。无论路径如何，internal tuple 由 opclass union support 形成子树摘要，分裂时由 picksplit support 决定分组。

本文涵盖构建策略选择及页 tuple 填充/容量核验，不负责运行期并发插入的锁协议，也不定义某个 opclass 的摘要正确性。`gistfitpage` 做向量大小可容纳性判断，不实施 fillfactor；页面空间策略和向上插入 downlink 由更高层调用流程配合。

## 怎么实现的

1. `gistbuild` 检查新索引为空，初始化 `GISTSTATE` 与每 tuple 临时 memory context，并依据 reloptions 选择 buffering 模式；若所有 key opclass 都有 sort support，则优先采用排序 build（除显式强制 buffering 的情况）。
2. 排序路径扫描 heap，将压缩后的 index tuple 放入 `tuplesort`，完成排序后由 `gist_indexsortbuild` 分层缓存页并逐层 flush。每个分组产生 union downlink，若当前层累积页需分组则调用 GiST split 算法，再将父 downlink 加入上层。
3. 非排序路径初始化 root 并扫描 heap，回调逐条构造 tuple；缓冲未启用时直接 `gistdoinsert`，缓冲启用时先下行到节点缓冲，缓冲达到阈值后级联清空。buffering 仅在估算缓存与 `maintenance_work_mem` 约束允许时切换。
4. `gistfitpage` 累加每个 tuple 的 `IndexTupleSize` 和 line pointer 空间，并与 `GiSTPageSize` 比较；若不合适，`gistSplit` 递归分组，直到各组适合或单 tuple 本身过大而报错。
5. `gistfillbuffer` 将向量按传入 offset 顺序逐项 `PageAddItem`；`InvalidOffsetNumber` 表示追加到空页起点或已有最大 offset 之后。添加失败立即报错，说明调用者应先完成容量判断。
6. 普通运行期页拆分由 `gistplacetopage` 建立左右页及 downlink，并在 critical section 写页与 WAL；build 期间无并发扫描，代码可采用较简单的拆分标记处理，最终按构建路径的 WAL 要求记录新页。

## 关键取舍

- 源码事实：有 sort support 的 opclass 可走排序自底向上构建；没有时回退到逐条插入及可选 buffering。排序有机会批量形成紧凑层级，但需要排序能力和临时排序状态；逐条插入兼容性更广，代价是反复树下降和页面更新。
- 源码事实：缓冲构建先累计中间节点 tuple，再分批向下清空，且据缓存规模及维护内存选择 level step；若无法满足约束就回退普通插入。替代的一律 buffering 将强制占用较大内存并可能使子树超出缓存，失去减少 I/O 的效果。
- 源码事实：`gistfitpage` 注释明确暂不考虑 fillfactor；它回答的是当前 tuple 集是否能装入页，而不是构建时预留空间策略。将二者合并会把容量正确性检查与负载策略耦合，改变调用者当前空间判断语义。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/gist/gistbuild.c` | `gistbuild` | 选择 build mode、初始化状态并驱动 heap 扫描和建树 |
| `src/backend/access/gist/gistutil.c` | `gistfillbuffer` | 按 offset 把 tuple 向量逐项写入目标页 |
| `src/backend/access/gist/gistutil.c` | `gistfitpage` | 以 tuple 与 line pointer 总空间判断分组能否适配一页 |

## 参考

- [PostgreSQL 文档：GiST 索引](https://www.postgresql.org/docs/current/gist.html) —— 介绍 GiST 构建所依赖的可扩展摘要键与 split 支持函数。
- [PostgreSQL 官方源码：GiST 构建](https://github.com/postgres/postgres/blob/master/src/backend/access/gist/gistbuild.c) —— 支撑排序构建、缓冲构建及策略回退细节。

## 与主核 / AI 的关系

暂无直接对应。主核索引构建可参考批量形成层级、容量预检和资源约束回退的思想；具体 GiST 支持过程不可直接复用。
