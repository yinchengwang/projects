---
project: postgres
feature: btree-page-maintenance
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/nbtree/nbtpage.c
    symbols: [_bt_initmetapage, _bt_vacuum_needs_cleanup]
  - path: src/backend/access/nbtree/nbtsplitloc.c
    symbols: [_bt_findsplitloc]
  - path: src/backend/access/nbtree/nbtutils.c
    symbols: [_bt_mkscankey, _bt_killitems]
  - path: src/backend/access/nbtree/nbtdedup.c
    symbols: [_bt_dedup_pass, _bt_bottomupdel_pass]
---

# B-tree 页维护与去重

## 一句话职责

B-tree 页维护初始化索引元数据、为写入选择分裂边界，并在页内回收或压缩索引项以控制空间增长。

## 机制是什么

叶页既承载键与 heap TID，也可能带有 dead line pointer 或把重复键 TID 合并为 posting tuple；metapage 保存根位置、层级和清理历史等结构信息。split 选择需考虑插入项、左右页空间和后缀截断，dedup 与 bottom-up deletion 则是减缓叶页因重复值或 UPDATE 版本 churn 而膨胀的维护路径。

本机制不覆盖完整的树插入传播与扫描过程。`_bt_mkscankey` 虽在 `nbtutils.c` 构造扫描/插入比较用键，本页只把它作为页维护相关的键表示工具提及；`_bt_killitems` 根据扫描观察到的 TID 可回收状态设置 dead hint，真正安全删除仍需依靠 vacuum/table-AM 与可见性规则，不能仅凭扫描命中就物理删除。

## 怎么实现的

1. `_bt_initmetapage` 初始化 page header、`BTMetaPageData` 中 root/level、fast root/level、cleanup 统计及等值图像标志，并将 page opaque 标为 metapage。
2. vacuum cleanup 可通过 `_bt_vacuum_needs_cleanup` 读取 metapage 当前版本与上次清理统计，决定在没有 bulk-delete 删除项时是否仍需执行清理；旧版本元数据在读取时需兼容升级。
3. 写入空间不足时 `_bt_findsplitloc` 枚举可能的分割位置，估算新 tuple 纳入后的左右可用空间，并在空间均衡优先的前提下考虑 suffix truncation；最右页还参考 leaf fillfactor，避免递增键插入产生不合适的填充状态。
4. 扫描在离开叶页前可将确认不可见的 TID 记录在 `BTScanOpaque.killedItems`，`_bt_killitems` 重新获取目标页写锁并核对 item 是否仍在原位；若 tuple 已移动或页面变化便放弃标记，避免错标并发修改后的记录。
5. 插入遇到页面空间压力时，`_bt_bottomupdel_pass` 把重复键形成的候选区间交给 table-AM 判断哪些 heap TID 已可删；空间目标只是建议，随后按剩余空间决定是否跳过去重。
6. `_bt_dedup_pass` 将重复键的多个 heap TID 收拢为 posting list tuple，保留键值并压缩 TID 存储；涉及页修改时按 B-tree WAL 与页锁协议写回。若回收不足，调用方仍可继续 split。

## 关键取舍

- **源码事实：**bottom-up deletion 让 table-AM 判断可删除 TID，并且其返回对是否避免 split 是 advisory；dedup 在没有足够空间时可继续让调用方 split。**架构推论：**把 heap tuple 可见性裁决留给 table-AM，避免索引 AM 自行推断 MVCC；代价是要跨层协作，若 B-tree 自行判定删除会缺少 heap/table-AM 的完整安全语义。
- **源码事实：**dead hint 先按扫描记录的位置回访页面，若 TID 已移动或不再匹配则放弃处理。**架构推论：**宁可漏掉一次可回收机会，也不因并发 split/插入而误标有效项；无条件按旧 offset 写 dead 标记实现更省检查，但可能破坏索引内容。
- **源码事实：**split 点平衡插入后的空闲空间，并对最右页另行考虑 fillfactor 与后缀截断。**架构推论：**目标不是让两边 tuple 数相等，而是减少新项所在一侧很快再次分裂的风险；固定中点拆分更简单，但会忽略 tuple 尺寸与右端追加负载。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/nbtree/nbtpage.c` | `_bt_initmetapage` | 写入初始 metapage 内容与 page opaque 标志。 |
| `src/backend/access/nbtree/nbtpage.c` | `_bt_vacuum_needs_cleanup` | 根据 metapage 版本和上次清理状态判断额外 cleanup 需求。 |
| `src/backend/access/nbtree/nbtsplitloc.c` | `_bt_findsplitloc` | 根据空间、填充目标和截断机会选择切分位置。 |
| `src/backend/access/nbtree/nbtutils.c` | `_bt_mkscankey` | 从 tuple/关系元数据构造 B-tree 比较键，供定位代码使用。 |
| `src/backend/access/nbtree/nbtutils.c` | `_bt_killitems` | 重核扫描记录的页内位置并设置可回收 dead hint。 |
| `src/backend/access/nbtree/nbtdedup.c` | `_bt_dedup_pass` | 把重复键项转为更紧凑的 posting list 表示。 |
| `src/backend/access/nbtree/nbtdedup.c` | `_bt_bottomupdel_pass` | 将候选重复键 TID 交由 table-AM 安全删除以回收页空间。 |

## 参考

- [PostgreSQL 文档：B-tree 索引](https://www.postgresql.org/docs/current/btree.html) —— 说明 B-tree 键、重复项与索引行为。
- [PostgreSQL 官方源码：B-tree 去重](https://github.com/postgres/postgres/blob/master/src/backend/access/nbtree/nbtdedup.c) —— 对照 posting list 与 bottom-up deletion 的页空间回收实现。

## 与主核 / AI 的关系

暂无直接对应。可借鉴“索引维护提出候选、heap/table-AM 决定可删性”的职责切分，具体删除条件需由主核自身事务与可见性协议定义。
