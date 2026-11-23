---
project: postgres
feature: btree-scan-search
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/nbtree/nbtree.c
    symbols: [btgettuple, btbeginscan]
  - path: src/backend/access/nbtree/nbtsearch.c
    symbols: [_bt_search, _bt_compare]
  - path: src/backend/access/nbtree/nbtreadpage.c
    symbols: [_bt_readpage, _bt_start_array_keys]
---

# B-tree 扫描与搜索

## 一句话职责

B-tree 扫描把有序索引键约束转成叶页上的候选 heap TID，并按扫描方向逐项推进。

## 机制是什么

扫描入口分为 AM 私有初始化、键规整、根到叶定位和叶页匹配读取。`_bt_search` 负责找到可能包含目标键的第一张叶页，`_bt_compare` 按 opclass 比较函数、排序方向、NULL 规则及必要时 heap TID 比较页内键；`_bt_readpage` 收集当前叶页符合 scan keys 的 TID。后续通过右/左 sibling 页面推进，键约束允许时可停止扫描。

扫描机制不负责插入、页分裂或键预处理本身；预处理由相邻机制规整键，插入另有写路径。B-tree AM 的普通 tuple 扫描声明非 lossy；executor 随后仍须按 snapshot 对 heap tuple 做可见性判断。数组键可能驱动多个 primitive scan，bitmap 回调则由专门路径输出 TID。

## 怎么实现的

1. `btbeginscan` 经 `RelationGetIndexScan` 建立通用描述符，并分配 `BTScanOpaqueData` 保存当前位置、数组键和标记位置等私有状态；扫描键副本在 rescan 阶段准备。
2. `btgettuple` 首次调用 `_bt_first`，之后按方向调用 `_bt_next`；若先前返回项被 executor 标为 kill prior tuple，则暂存页内位置，离开页前再尝试标记 dead hint。
3. `_bt_first` 使用已规整的 scan keys 构造插入型搜索键，调用 `_bt_search` 自根向叶下降。搜索期间持有目标页 pin 和读锁；父页锁在下降完成后释放。并发分裂时根据 high key/right-link 右移重新定位。
4. `_bt_compare` 逐个比较搜索键和 index tuple 属性，处理 NULL、DESC 方向、截断属性及必要的 heap TID 次序；它依赖调用者提供按索引列顺序组织的键。
5. `_bt_readpage` 在调用者已 pin 并持有当前页读锁时检查页内 tuple 和 scan keys，将匹配项装入 `BTScanOpaque` 的当前位置数组，更新还能否向左/右继续的标志；数组键状态会触发 primitive scan 切换。
6. 页面无更多匹配项时扫描沿 sibling 链移动并再次读取，遇到必需边界约束已不可能匹配时停止。完成后 `btendscan` 释放页 pin 与 AM 私有状态，公共 indexam 层结束 descriptor 生命周期。

## 关键取舍

- **源码事实：**内部页搜索返回可能含首个匹配项的叶页，并通过 high key 与右链处理并发移动；找到叶页时不保留父页锁。**架构推论：**缩短下降路径上同时持有的锁数量，但正确性依靠 B-link sibling 结构和重新检查边界；若持有整条根到叶锁链，更新可能被长扫描阻塞。
- **源码事实：**`btgettuple` 对 heap 扫描维护 killed item 提示，且 rescan 决定是否可提前 drop leaf pin；index-only、非 MVCC snapshot 和 bitmap 扫描不采用同一释放条件。**架构推论：**普通 MVCC heap scan 能尽早释放叶页 pin，降低 VACUUM 等 cleanup lock 的等待；一概提前释放会破坏其他扫描类别所需的并发安全条件。
- **源码事实：**GIN 采用 bitmap 接口，而 B-tree 支持逐 TID 的 `amgettuple`，B-tree bitmap 路径也会把 TID 加入 TIDBitmap。**架构推论：**保留逐项接口便于有序扫描与 index-only 行为；若只返回 bitmap，将失去直接按索引序输出的接口语义。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/nbtree/nbtree.c` | `btgettuple` | 驱动 primitive scan、推进并返回下一条 heap TID。 |
| `src/backend/access/nbtree/nbtree.c` | `btbeginscan` | 建立通用 descriptor 并初始化 B-tree 私有扫描状态。 |
| `src/backend/access/nbtree/nbtsearch.c` | `_bt_search` | 从根下降并在并发右移后定位目标叶页。 |
| `src/backend/access/nbtree/nbtsearch.c` | `_bt_compare` | 用 opclass 比较规则比较搜索键与页内 index tuple。 |
| `src/backend/access/nbtree/nbtreadpage.c` | `_bt_readpage` | 检查当前叶页并缓存匹配 TID 及方向边界。 |
| `src/backend/access/nbtree/nbtreadpage.c` | `_bt_start_array_keys` | 根据数组键状态启动/调整后续数组扫描位置。 |

## 参考

- [PostgreSQL 文档：索引扫描类型](https://www.postgresql.org/docs/current/index-scanning.html) —— 解释 tuple 与 bitmap 索引扫描接口及其执行差异。
- [PostgreSQL 官方源码：B-tree 搜索](https://github.com/postgres/postgres/blob/master/src/backend/access/nbtree/nbtsearch.c) —— 对照根到叶搜索、页间移动和键比较实现。

## 与主核 / AI 的关系

暂无直接对应。B-tree 的有序页搜索可以作为主核索引路径参照，但扫描返回 TID 不等同于主核执行器完成行可见性过滤。
