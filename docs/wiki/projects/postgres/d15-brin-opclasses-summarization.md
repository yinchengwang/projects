---
project: postgres
feature: brin-opclasses-summarization
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/brin/brin_minmax_multi.c
    symbols: [AssertCheckRanges, AssertCheckExpandedRanges]
  - path: src/backend/access/brin/brin_bloom.c
    symbols: [bloom_init, bloom_add_value]
---

# BRIN 摘要算子类

## 一句话职责

BRIN 摘要算子类把范围内列值压缩为可快速判定的摘要，并在容量与过滤精度之间控制保守程度。

## 机制是什么

minmax-multi 将范围表示为多个互不重叠区间及单点值，离群值可独立记录，不必把整个 min/max 区间拉宽；达到容量时可合并相近区间，扩大局部覆盖但仍不漏掉原值。`AssertCheckRanges` 与 `AssertCheckExpandedRanges` 在断言构建中验证数组顺序、区间边界和不重叠约束。

Bloom 算子类为每范围维护 Bloom filter：原值先由类型 hash procedure 转成 uint32，再用两个种子哈希派生多个位位置；查询等值项检测所有相应位。Bloom 的阳性可能是假阳性，因此只支持保守候选过滤，不负责精确相等判定。两种摘要都不负责堆行可见性和最终 SQL 条件检查。

## 怎么实现的

1. minmax-multi 的内存态 `Ranges` 维护 `nranges/nsorted/nvalues/maxvalues` 与 `values[]`；磁盘 `SerializedRanges` 保存类型、区间/点计数和边界值。单点只占一个 Datum，一般区间占 min/max 两个边界。
2. 新值加入前先执行 `ensure_free_space_in_buffer`，必要时去重；仍满时将区间展开成 `ExpandedRange`，按边界排序合并重叠，再用 opclass distance procedure 计算相邻间隙，保留较大 gap、合并较近区间以压缩容量。
3. 写盘序列化前把边界值控制在目标容量内，并按数据类型长度和 byval/byref 规则复制；`AssertCheckRanges` 检查 Range 数组，`AssertCheckExpandedRanges` 检查扩展表示的边界顺序和非重叠性质。
4. Bloom 的 `bloom_init` 根据预计 distinct 数和 false-positive rate 计算 bit 数、字节数及哈希数，分配 varlena `BloomFilter` 并清零位图；超出单页预算的过滤器直接报错。
5. `bloom_add_value` 在首个非 NULL 值到来时初始化 filter，之后调用 opclass hash procedure 得到 uint32，再用两个固定种子的 hash 派生若干 bit；仅在某个位从 0 变 1 时标记摘要已更新。
6. 查询一致性函数对每个 equality scan key 重算 hash 和位集合，只要一个位为 0 即排除范围；全为 1 则保留候选范围，实际匹配仍需 heap recheck。多个同配置 filter 的 union 用逐字节 OR 合并。

## 关键取舍

- 源码事实：minmax-multi 用多个区间承受比单一 min/max 更高的摘要状态成本，以减少离群值扩大整个范围导致的误扫描；容量不足时按距离贪心合并近邻，牺牲区间间隙精度来保证有界存储。distance 只是 opclass 提供的度量，合并不改变摘要正确性。
- 源码事实：Bloom 存 hash 而非原始类型值，并用双哈希派生多位置，避免每个值存储完整类型数据及多次独立昂贵 hash；代价是哈希碰撞与 Bloom 假阳性带来额外候选扫描，不能当精确 membership 结果。
- 源码事实：Bloom filter 大小受单页容量检查约束，参数倾向在尺寸与目标假阳率间取舍；更低假阳率需要更多 bits，可能让索引 tuple 无法装入页。把超限 filter 静默截断会破坏无假阴性保证，因此采用错误而非有损截断。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/brin/brin_minmax_multi.c` | `AssertCheckRanges` | 在断言构建中校验内存摘要区间与单点数组的不变量 |
| `src/backend/access/brin/brin_minmax_multi.c` | `AssertCheckExpandedRanges` | 校验压缩/合并过程中展开区间的边界有效及相邻不重叠 |
| `src/backend/access/brin/brin_bloom.c` | `bloom_init` | 按 distinct 估计和目标假阳率创建并校验 filter 大小 |
| `src/backend/access/brin/brin_bloom.c` | `bloom_add_value` | hash 新值并把派生位写入范围 Bloom 摘要 |

## 参考

- [PostgreSQL 文档：BRIN 索引与算子类](https://www.postgresql.org/docs/current/brin.html) —— 说明 BRIN 摘要由 opclass 定义，并用于范围级过滤。
- [PostgreSQL 官方源码：BRIN minmax-multi](https://github.com/postgres/postgres/blob/master/src/backend/access/brin/brin_minmax_multi.c) —— 支撑多区间表示、边界校验和压缩取舍。
- [PostgreSQL 官方源码：BRIN Bloom](https://github.com/postgres/postgres/blob/master/src/backend/access/brin/brin_bloom.c) —— 支撑哈希、位图初始化及 Bloom 摘要插入。

## 与主核 / AI 的关系

暂无直接对应。minmax-multi 与 Bloom 可作为主核摘要索引的设计样例，但必须保留无假阴性约束与最终精确过滤职责。
