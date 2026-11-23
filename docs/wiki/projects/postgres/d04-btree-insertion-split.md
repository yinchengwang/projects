---
project: postgres
feature: btree-insertion-split
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/nbtree/nbtree.c
    symbols: [btinsert]
  - path: src/backend/access/nbtree/nbtinsert.c
    symbols: [_bt_doinsert, _bt_finish_split]
---

# B-tree 插入与分裂

## 一句话职责

B-tree 插入在有序树中安放新索引项，并在空间不足时通过可恢复的页分裂维持树结构。

## 机制是什么

公开 AM 回调 `btinsert` 将输入值形成 IndexTuple、构造搜索键，再交给 `_bt_doinsert` 执行定位、唯一性检查和页内插入。若目标页空间不足，递归插入逻辑拆分页面并将分隔信息向父层传播；父层处理未完成时，页面标志让后续访问能够继续完成 split。

本页聚焦插入与 split 完成，不涵盖扫描键规整、metapage 初始化策略、dedup 与 bottom-up deletion 的完整策略。唯一性检查会通过扫描冲突键和 heap 可见性相关判断协调并发插入，不是单纯在叶页比较等值键；页面锁保护结构修改，事务锁/等待用于唯一性冲突处理。

## 怎么实现的

1. `btinsert` 依索引 descriptor 调用 `index_form_tuple` 构造记录并填入 heap TID，建立用于搜索的 `BTScanInsert` 键后进入 `_bt_doinsert`。
2. `_bt_doinsert` 搜索目标叶页并建立插入状态；若唯一性检查需要执行，会检查相同键对应项并可能等待并发事务，等待后重新验证，确保并发提交不会绕过唯一约束。
3. 在叶页插入前先处理已标记 dead 的项；当页面有空间时直接放入新 tuple 并记录 WAL。页面空间不足时，尝试 bottom-up deletion 或 dedup 等调用方维护路径后再判断是否仍需 split（本页不展开其算法）。
4. split 路径以 `_bt_findsplitloc` 选择切分点，分配新右页，调整旧页、右页的 high key、sibling link 与页面内容，并通过 WAL 原子记录结构变化。页锁保护同时修改的页面；父页沿搜索栈逐层更新。
5. 如果父页面也需要分裂，插入过程递归向上；根页分裂会创建新根并更新 metapage 根位置。分裂若中途未完成，页标记 `BTP_INCOMPLETE_SPLIT` 留待后续完成。
6. 后续搜索遇到 incomplete split 时调用 `_bt_finish_split`，把缺失的 downlink 写入父页并清除待完成状态；若再次遇到父页空间不足，继续递归 split。这样即使先前操作未完成整条传播，后续操作仍可恢复结构一致性。

## 关键取舍

- **源码事实：**B-link 页保留 sibling link 和 high key，搜索可右移；split 被标记 incomplete 后由后续操作完成父级链接。**架构推论：**允许 split 暂时分阶段完成，减少持有多层页锁的时间；若必须一次锁住整条根到叶路径直到所有父级更新，锁竞争和死锁风险都会增加，但 split 中断后就需要另设全局恢复机制。
- **源码事实：**`_bt_doinsert` 对唯一性冲突使用事务冲突检查并在需要时等待、重查，而不是只依赖页面锁。**架构推论：**页面锁只保护物理页，无法阻止另一事务在不同时间插入相同逻辑键；若不等待并重验，就可能错误接受并发重复键。
- **源码事实：**split 选择会考虑插入项尺寸、两侧空间与后缀截断机会；最右页还按填充因子目标处理。**架构推论：**牺牲绝对均分或只按 tuple 数平分，换取实际可用空间更平衡，并避免递增键插入时后续页过度填满；纯等量拆分可能让新 tuple 所在侧很快再次分裂。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/nbtree/nbtree.c` | `btinsert` | 构造 B-tree 插入 tuple 与搜索键并进入核心插入。 |
| `src/backend/access/nbtree/nbtinsert.c` | `_bt_doinsert` | 执行唯一性处理、目标页插入和必要的结构更新。 |
| `src/backend/access/nbtree/nbtinsert.c` | `_bt_finish_split` | 恢复 incomplete split 并完成父级 downlink 传播。 |

## 参考

- [PostgreSQL 文档：B-tree 索引](https://www.postgresql.org/docs/current/btree.html) —— 说明 B-tree 键排序与索引行为。
- [PostgreSQL 官方源码：B-tree 插入与分裂](https://github.com/postgres/postgres/blob/master/src/backend/access/nbtree/nbtinsert.c) —— 对照叶页插入及 incomplete split 完成路径。

## 与主核 / AI 的关系

暂无直接对应。可参照其“分阶段结构修改 + 后续修复”的并发维护思路；主核是否采用类似策略需以自身页格式、日志协议和恢复约束为准。
