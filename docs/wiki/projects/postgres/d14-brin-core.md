---
project: postgres
feature: brin-core
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/brin/brin.c
    symbols: [brininsert, brinbeginscan]
---

# BRIN 扫描与维护

## 一句话职责

BRIN 以堆页范围为单位保存摘要，快速排除不可能匹配的范围并把候选堆页交给 bitmap 扫描。

## 机制是什么

BRIN 的索引 tuple 汇总一个连续 heap page range，而不是逐行存键。`brininsert` 将新行值并入对应范围已有摘要；范围尚未 summarize 时不需要立刻建索引 tuple。扫描通过 reverse map 找到每个范围摘要，调用 opclass consistent procedure 判定是否可能匹配，命中范围内的页加入 TID bitmap。

BRIN 不负责精确返回行，也不保证摘要能排除所有不匹配页；不摘要或 placeholder 范围采取保守策略，仍纳入扫描。它不负责 heap 的可见性与最终谓词判定，也不负责单行索引定位；摘要范围维护、autosummarize 请求与扫描候选生成是其边界。

## 怎么实现的

1. `brininsert` 通过 statement 级 `IndexInfo.ii_AmCache` 保存 `BrinInsertState`，其中包含 `BrinRevmap`、`BrinDesc` 及 `pages_per_range`；由 heap TID 算出对应范围起点。
2. 若启用 autosummarize，插入新范围首条 tuple 时可向 autovacuum 请求汇总前一范围。随后 reverse map 查询当前范围 tuple；若该范围尚未汇总，当前插入无需改变 BRIN 索引。
3. 已有摘要时按共享 buffer 锁读取、deform 成 `BrinMemTuple`，把新值并入各列摘要。若摘要未变则解锁；若要更新则先复制旧 tuple、形成新 tuple，释放锁后由 `brin_doupdate` 尝试更新。
4. `brin_doupdate` 失败表示竞争期间 reverse map 指向或 tuple 状态变化；调用者 reset 临时 memory context 后从 reverse map 重新开始，以合并并发插入值，避免丢掉任一摘要贡献。
5. `brinbeginscan` 读取索引配置并建立 reverse map 访问态和 `BrinDesc`。`bringetbitmap` 遍历 heap 范围起点，取出摘要；无 tuple 或 placeholder 则无条件把范围加入 bitmap，否则逐列执行 null 检查及 opclass consistent 函数，任何列能排除时跳过该范围。
6. 确认候选范围后，将范围内 heap block 逐页加到 `TIDBitmap`。VACUUM cleanup 可扫描并汇总当前未汇总范围；常规 bulk delete 不逐行删除 BRIN tuple，因为索引并无逐行条目。

## 关键取舍

- 源码事实：范围未汇总时扫描必须返回该范围全部页；这维持无漏行正确性，代价是新范围或缺失摘要期间可能退化为更宽 heap 扫描。选择乐观排除会漏掉尚未写入摘要的新值。
- 源码事实：扫描输出 bitmap 中的页而非精确 TID；范围摘要只提供可能性判断，避免为每行保存索引项，代价是范围内无关行也要由 heap 扫描与上层条件过滤。
- 源码事实：并发更新失败后重启并重新读取摘要，而不是沿用旧副本重试原更新；这避免覆盖另一个 inserter 已合并的值，代价是竞争时重复读与重算。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/brin/brin.c` | `brininsert` | 将插入行值合并到范围摘要，并处理 autosummarize 与并发重试 |
| `src/backend/access/brin/brin.c` | `brinbeginscan` | 为扫描初始化 pages-per-range、reverse map 及摘要描述符 |

## 参考

- [PostgreSQL 文档：BRIN 索引](https://www.postgresql.org/docs/current/brin.html) —— 说明 BRIN 范围摘要、扫描与 summarize/vacuum 行为。
- [PostgreSQL 官方源码：BRIN 主流程](https://github.com/postgres/postgres/blob/master/src/backend/access/brin/brin.c) —— 支撑插入更新、reverse map 扫描和 bitmap 输出流程。

## 与主核 / AI 的关系

暂无直接对应。BRIN 适合作为主核范围摘要索引的参照；需要自己的摘要失效、未汇总回退和并发合并契约。
