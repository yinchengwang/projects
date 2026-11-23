---
project: postgres
feature: btree-scan-key-preprocessing
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/nbtree/nbtpreprocesskeys.c
    symbols: [_bt_preprocess_keys]
---

# B-tree 扫描键预处理

## 一句话职责

键预处理把调用者提供的 B-tree scan keys 规整成扫描中可正确比较、可判定继续边界的私有约束。

## 机制是什么

`_bt_preprocess_keys` 将通用 scan 的 `keyData[]` 复制到 `BTScanOpaque` 的私有数组，按索引属性顺序调整策略与标志，尝试去除冗余约束，并标记哪些键是向前或向后继续扫描所必需的。扫描执行使用输出键做初始定位和页内逐项判定。

它负责键约束规整而不负责遍历索引页或实现比较器；页定位由 B-tree 搜索模块完成。函数无法使用缺失的跨类型比较支持函数来证明某些键冗余或矛盾时，会保留更多检查条件以保证正确性，可能牺牲效率但不应漏掉匹配项。

## 怎么实现的

1. 若 `BTScanOpaque.numberOfKeys` 已非零，函数视为本次 rescan 已处理而直接返回；否则初始化 `qual_ok` 并处理无键扫描的快速路径。
2. 有数组键时先构造部分规整的数组键副本；必要时扩展输出键缓冲区并记录输入到输出键的映射，以便保持 order support procedure 对应关系。
3. 按属性逐组处理输入 ScanKey，校验键属性顺序，并应用索引列的 DESC、NULLS FIRST 选项；DESC 会转换比较策略。无法匹配的 NULL 条件可将 `qual_ok` 置为 false。
4. 每列按等值、上下界等策略筛选约束：等值键可淘汰同列冗余范围；可比较时保留最紧的上下界，遇到互相矛盾的条件则标记扫描无结果。
5. 依据复合键排序规则设置 `SK_BT_REQFWD` / `SK_BT_REQBKWD`，使执行器能在当前方向确定何时可以安全终止，而不是把所有后续列上的过滤条件都误认为停止条件。
6. 对缺少跨类型比较函数、不能安全比较的冗余条件保留额外检查键；数组键与 skip scan 可能合成隐式数组约束，再由后续 primitive scan 推进。

## 关键取舍

- **源码事实：**函数只在每次 `btrescan` 周期首次调用时处理，并复制键而不覆盖调用者的 `scan->keyData[]`。**架构推论：**私有副本允许改变策略和参数排序，也避免 rescan 参数变化污染上层数据；原地改写更省内存，但无法可靠保留调用者输入供后续重新解释。
- **源码事实：**跨类型比较支持不完整时，函数可能无法消除冗余键或识别矛盾，会把未能证明的条件留作后续检查。**架构推论：**保守地多做比较，代价是额外扫描过滤工作；若凭不完整信息强行删键，可能漏报矛盾之外的真实匹配结果，破坏查询正确性。
- **源码事实：**required 标记按扫描方向与前导键排列规则设置；非必需的后续列条件可过滤当前项，但不一定意味着更后面的项都不匹配。**架构推论：**将“拒绝当前 tuple”和“可停止整段扫描”分开，换取正确终止；把所有 quals 都视为可停止条件会在复合索引上跳过合法结果。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/nbtree/nbtpreprocesskeys.c` | `_bt_preprocess_keys` | 复制并压缩 ScanKey，生成方向必需标志、数组约束及不可匹配状态。 |

## 参考

- [PostgreSQL 文档：多列索引](https://www.postgresql.org/docs/current/indexes-multicolumn.html) —— 说明 B-tree 前导列约束对搜索范围的影响。
- [PostgreSQL 官方源码：B-tree 键预处理](https://github.com/postgres/postgres/blob/master/src/backend/access/nbtree/nbtpreprocesskeys.c) —— 对照冗余键、required 标志和矛盾检测。

## 与主核 / AI 的关系

暂无直接对应。它体现了优化阶段可以压缩约束但必须保留无法证明安全可删除的条件；主核不应把 B-tree 专属策略标记直接照搬。
