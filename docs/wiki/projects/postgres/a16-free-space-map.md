---
project: postgres
feature: free-space-map
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/freespace/freespace.c
    symbols: [GetPageWithFreeSpace, RecordPageWithFreeSpace, RecordAndGetPageWithFreeSpace]
---

# 空闲空间映射 FSM

## 一句话职责

FSM 以可快速搜索的近似记录，帮助写入路径找到可能容纳新数据的关系页。

## 机制是什么

Free Space Map 是关系 FSM fork 中的页级空闲空间摘要树：叶节点对应主关系中的堆块，每项用一个字节量化可用空间；上层节点保存子树最大类别，供搜索剪枝。它服务于堆表及需要 FSM 的索引访问方法，减少为找空位而逐页探测的成本。

它不是页内空间的权威事实，也不预留空间或锁定候选页。并发插入、页变化和近似量化会令候选失准；调用方必须取得目标页锁后检查实际空间，不足时更新 FSM 并重试，找不到候选则可扩展关系。页布局与 tuple 插入仍由相应访问方法负责，FSM 不替代它们。

## 怎么实现的

空间量化在 `freespace.c` 的 `fsm_space_avail_to_cat` 与 `fsm_space_needed_to_cat` 中完成。可用量向下归类，需求向上归类，减少把不足的页误当成确定可用；最高类别专门表示至少 `MaxFSMRequestSize` 的空间。因而记录有损，读回类别下界也只是估值。

FSM 页组成多层树。`fsm_get_location` 将 heap block 映射到叶页和槽位；`fsm_get_parent`、`fsm_get_child` 在各层间导航；`fsm_logical_to_physical` 将逻辑层级地址映射为 FSM fork 中深度优先排列的物理块。`fsm_search` 从根开始，按每页子树最大类别向下选择候选。若叶项指向关系末尾之外，会清掉该项并重新搜索；若下层实际最大值与上层摘要不一致，则回写上层并从根重试。

`GetPageWithFreeSpace` 将字节需求转为类别后发起查找。`RecordPageWithFreeSpace` 只更新叶项；提高可用空间时，上层摘要不一定立即可见，`FreeSpaceMapVacuum` 或 `FreeSpaceMapVacuumRange` 才递归重算上层。`RecordAndGetPageWithFreeSpace` 合并更新与再搜索，优先尝试同一 FSM 页内的合适槽位以减少锁操作和搜索距离。

FSM 页经缓冲管理器读取并由 buffer lock 保护；更新是提示型数据而非 WAL 保护的业务事实。`fsm_readbuf` 以可清零处理的读取模式面对损坏或撕裂页，并在并发初始化时取得排他锁后复核新页状态。搜索和更新之间允许竞争，过时的摘要会由后续搜索修补或 vacuum 重建。裁剪关系时由 `FreeSpaceMapPrepareTruncateRel` 处理尾部记录与物理截断准备，避免让候选越过主 fork 尾部。

## 关键取舍

FSM 用字节类别与分层最大值换取紧凑存储和树搜索速度，代价是误差、过时摘要及偶发重试。若换成每页精确字节数并实时维护所有祖先节点，空间和写入锁竞争都会增加；若换成每次扫描主关系页，则无需摘要维护，却会把寻位开销转为大量随机读。FSM 选择容忍可修复的假阳性/假阴性，由页锁后的真实检查兜底。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/freespace/freespace.c` | `GetPageWithFreeSpace` | 将空间需求转成类别并查询候选页 |
| `src/backend/storage/freespace/freespace.c` | `RecordPageWithFreeSpace` | 更新某个 heap block 对应的叶槽 |
| `src/backend/storage/freespace/freespace.c` | `RecordAndGetPageWithFreeSpace` | 合并候选页状态更新与再搜索 |
| `src/backend/storage/freespace/freespace.c` | `fsm_search`、`fsm_vacuum_page` | 树搜索与上层摘要维护 |

## 参考

- [PostgreSQL 官方文档：空闲空间映射](https://www.postgresql.org/docs/current/storage-fsm.html)
- [PostgreSQL 官方文档：数据库物理存储布局（FSM fork）](https://www.postgresql.org/docs/current/storage.html)

## 与主核 / AI 的关系

在主核中，FSM 是存储层提供的候选定位提示，写入路径仍需在缓冲池页锁下验证和修改实际页。它不构成事务可见性、空间预留或持久化承诺；AI 可用它解释“为什么找页很快但仍可能重试”，不应把 FSM 记录当作当前页空间的精确测量。
