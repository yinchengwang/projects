---
project: postgres
feature: index-and-bitmap-scans
page_type: feature
schema_version: 1
refs:
  - path: src/backend/executor/nodeIndexscan.c
    symbols: [ExecIndexScan, ExecIndexBuildScanKeys, ExecIndexEvalRuntimeKeys]
  - path: src/backend/executor/nodeIndexonlyscan.c
    symbols: [IndexOnlyNext]
  - path: src/backend/executor/nodeBitmapHeapscan.c
    symbols: [BitmapHeapNext, BitmapHeapRecheck]
---

# 索引与位图扫描执行

## 一句话职责

执行器把规划器给出的索引条件转成扫描键，驱动索引 AM 逐项或批量取 TID，并按需回表与重检。

## 机制是什么

`IndexScan`、`IndexOnlyScan`、`BitmapHeapScan` 三个节点共用同一套执行器侧约定：规划器把 `indexqual` 与可能的 `indexorderby` 交给节点，节点在初始化时把它们编译成 `ScanKey` 数组与运行时键信息，之后每次取数只做轻量推进。索引算法本身（B-tree 比较、GIN posting list 等）由 AM 承担，本层不重复实现。

三者差别在取数方式：普通索引扫描逐 TID 回表并做 MVCC 可见性判断；index-only 扫描先查可见性图，页面全可见时直接用索引返回的元组而跳过 heap 访问；bitmap 扫描由下层 `BitmapIndexScan` 一次性生成 `TIDBitmap`，再由 heap 扫描节点按页取 tuple，并用 lossy 标志决定是否重检。节点本身不决定用哪个索引，那是规划器与 path 选择的职责。

## 怎么实现的

1. `ExecIndexBuildScanKeys` 把 `indexqual` 归入五类：常量比较直接填 `ScanKey`；非常量比较建立 `IndexRuntimeKeyInfo`，值留待运行时求值；`RowCompareExpr` 建立头部键加子键数组；`ScalarArrayOpExpr` 在 AM 支持 `amsearcharray` 时置 `SK_SEARCHARRAY`，否则建立 `IndexArrayKeyInfo` 逐元素驱动；`NullTest` 直接填好键。
2. 常量键在初始化阶段即固定，运行时键则延后。`ExecIndexEvalRuntimeKeys` 在每次 rescan 前切到 `ecxt_per_tuple_memory`，对每个键表达式求值并写回对应 `ScanKey`；`ExecReScanIndexScan` 先 `ResetExprContext` 再重算全部运行时键，因此外层每换一行都不积累内存。
3. `ExecIndexScan` 若存在运行时键且尚未就绪，先用一次 `ExecReScan` 完成首轮求值，然后走 `ExecScan`；有 `iss_NumOrderByKeys` 时改用 `IndexNextWithReorder`，否则用 `IndexNext`。前者依赖 `pairingheap` 重排队列，用于 `amcanorderbyop` 索引返回距离时的排序校正。
4. `IndexOnlyNext` 在首次调用时惰性 `index_beginscan(..., index_only=true, ...)`，并在 `xs_want_itup` 断言下要求 AM 返回索引元组；运行时键就绪后 `index_rescan` 才把键交给 AM。取到元组后若 `xs_recheck` 为真，用 `recheckqual` 对 `ecxt_scantuple` 重检，失败则计数丢弃；若存在 lossy 的 ORDER BY 距离则直接报不支持。
5. index-only 路径的实际"免回表"由 table AM 决定：`heapam_index_getnext_slot` 先对 TID 所在页查 `VM_ALL_VISIBLE`，全可见时只做 `PredicateLockPage` 再填充索引元组；页面不全可见则回退 `heapam_index_only_heap_fetch` 取 heap 元组，若反复回退超过 `xs_visited_pages_limit` 就提前结束扫描。
6. `BitmapHeapNext` 首次调用走 `BitmapTableScanSetup`：`MultiExecProcNode` 取下层 bitmap，并行时由首个 worker 建 `TIDBitmap` 并用原子状态与条件变量唤醒其他 worker，然后 `tbm_begin_iterate` 建立迭代器、`table_beginscan_bm` 建立 heap 扫描描述符。
7. 取数循环调 `table_scan_bitmap_next_tuple`，同时得到 `recheck` 与 lossy/exact 页统计；`recheck` 为真时用 `bitmapqualorig` 逐 tuple 重检，失败则清空槽并继续。`BitmapHeapRecheck` 是 EvalPlanQual 路径上的同构重检入口，两者都只做条件复核，不重新构建 bitmap。

## 关键取舍

- **源码事实：**index-only 扫描是否真正免回表由 `VM_ALL_VISIBLE` 决定，页面不全可见时回退 heap fetch，并用 `xs_visited_pages_limit` 上限防止长时间回退。**架构推论：**以可见性图为前提换取大幅减少 heap 访问，代价是页面未 all-visible 时退化成普通回表；若不做回退，就只能要求 VACUUM 先把所有页置为 all-visible，否则会读不到未标记页上的可见元组。
- **源码事实：**bitmap 扫描把 lossy 信息随 tuple 一起返回，`BitmapHeapNext` 仅在 `recheck` 为真时逐 tuple 复核，`BitmapHeapRecheck` 复用同一 `bitmapqualorig`。**架构推论：**以少量重复判定换取 bitmap 可用整页压缩表示、显著缩小内存；若要求 bitmap 始终精确到 TID，页级 lossy 压缩就无法使用，大表扫描的 bitmap 内存会按 TID 数增长。
- **源码事实：**运行时键在每次 rescan 时于 `ecxt_per_tuple_memory` 内重新求值，常量键则在初始 build 阶段固定。**架构推论：**把非常量键的执行成本放到外层每行的扫描入口，避免每次取元组都求值；若逐元组求值，嵌套循环内层扫描的表达式开销会成倍放大。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/executor/nodeIndexscan.c` | `ExecIndexScan` | 首轮运行时键准备后经 `ExecScan` 驱动普通索引扫描取数。 |
| `src/backend/executor/nodeIndexscan.c` | `ExecIndexBuildScanKeys` | 把索引条件分类编译为 ScanKey、运行时键与数组键结构。 |
| `src/backend/executor/nodeIndexscan.c` | `ExecIndexEvalRuntimeKeys` | 在 per-tuple 内存中求值非常量键并写回 ScanKey。 |
| `src/backend/executor/nodeIndexonlyscan.c` | `IndexOnlyNext` | 惰性建立 index-only 扫描、重检 lossy 条件并返回索引元组。 |
| `src/backend/executor/nodeBitmapHeapscan.c` | `BitmapHeapNext` | 建立并迭代 TIDBitmap，按 lossy 标志逐 tuple 重检后返回。 |
| `src/backend/executor/nodeBitmapHeapscan.c` | `BitmapHeapRecheck` | EvalPlanQual 路径上以原始 bitmap 条件复核元组。 |

## 参考

- [PostgreSQL 文档：索引扫描类型](https://www.postgresql.org/docs/current/index-scanning.html) —— 说明索引扫描、index-only 与 bitmap 扫描的执行差异。
- [PostgreSQL 文档：位图扫描与 lossy 重检](https://www.postgresql.org/docs/current/using-explain.html) —— 解释 Bitmap Heap Scan 的 recheck 与 lossy 统计含义。
- [PostgreSQL 官方源码：bitmap heap 扫描节点](https://github.com/postgres/postgres/blob/master/src/backend/executor/nodeBitmapHeapscan.c) —— 对照 bitmap 迭代、并行共享状态与重检循环。

## 与主核 / AI 的关系

暂无直接对应。主核若实现 bitmap 扫描，可借鉴「页级 lossy 压缩 + 逐 tuple 重检」与「可见性图先行、未命中回退」两条边界；索引算法本身不在本层。
