---
project: postgres
feature: table-access-method-heap
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/table/tableam.c
    symbols: [table_beginscan_catalog, table_slot_callbacks]
  - path: src/backend/access/heap/heapam_handler.c
    symbols: [GetHeapamTableAmRoutine]
  - path: src/backend/access/heap/heapam.c
    symbols: [heap_beginscan, heap_getnext, heap_insert]
---

# Heap TableAM 访问

## 一句话职责

Heap TableAM 把通用表访问请求分派到 heap 实现，并把 heap 的扫描、slot 与插入能力接入执行器可调用的表访问契约。

## 机制是什么

`TableAmRoutine` 是关系访问方法的函数表，覆盖 slot 回调、扫描生命周期、单元组读取与修改等能力。relcache 为关系设置 `rd_tableam`；普通上层代码通过 `table_*` 包装器使用这些回调，而不是把每一种表访问方法都写进执行器。Heap 的静态函数表 `heapam_methods` 将这些接口指向 heapam 实现；`GetHeapamTableAmRoutine` 返回这个常驻对象。

该页只讲 TableAM 接线与契约层：heap 是如何提供一组回调、通用包装如何把调用送到它。不展开具体元组可见性、更新/删除、页内剪枝、VM 或 WAL 细节；这些属于各自机制。索引访问方法也不在这里，索引扫描在此仅作为表 AM 提供取回 heap 元组回调的调用背景。

## 怎么实现的

1. relcache 在初始化关系访问方法时，通过 handler OID 调用 handler 函数取得 `TableAmRoutine`；`GetTableAmRoutine` 检查返回值类型并断言必需回调不为空。heap 的 handler 路径最终取得 `heapam_methods`。
2. `heapam_methods` 静态登记回调：扫描族指向 `heap_beginscan`、`heap_endscan`、`heap_getnextslot` 等；插入回调 `heapam_tuple_insert` 从 slot 取出 heap tuple 后调用 `heap_insert`，并把写入后的 TID 复制回 slot。函数表生命周期为服务器进程生命周期，关系只持有其指针。
3. 上层扫描通过 `table_beginscan` 一类包装器提供 snapshot、scan key 和 flags；公共入口校验调用方不得传内部扫描类型位，再派发到 `rel->rd_tableam->scan_begin`。目录扫描专用 `table_beginscan_catalog` 选择 catalog snapshot、注册其生命周期，并启用相应扫描选项。
4. `heap_beginscan` 分配 heap 专用 `HeapScanDescData`，保存 relation、snapshot、keys、flags 和可选并行描述符；它增加 relation 引用，初始化扫描状态。若 snapshot 不是 MVCC snapshot，就关闭 page-at-a-time 模式；非法 historic snapshot 对非目录关系会报错。
5. `initscan` 固定本次扫描的块数，并按标志/关系大小建立或释放 bulk-read strategy、设置同步扫描起点和 read stream。并行扫描以共享并行描述符给出的块数/状态为准。heap 旧式 `heap_getnext` 仍可直接使用，但先检查关系确实是 heap AM；通用路径 `heap_getnextslot` 则将当前 tuple 放入调用方 slot。
6. 结束或 rescan 时释放缓冲 pin、read stream、strategy、临时 snapshot 与扫描描述符；扫描对象持有的 relation 引用在结束时归还。扫描页内容访问时由内部取元组路径取得/释放 buffer lock，返回的数据依赖 pin 的生命周期。

## 关键取舍

- 源码事实：TableAM 采用常驻函数表与统一包装器；`GetTableAmRoutine` 在调试断言中要求核心回调齐备。架构推论：相较在执行器中按 AM 类型写分支，这让新 AM 能在共同契约下接入，但必须实现大量一致的生命周期与错误语义；把回调都做成可选会把缺失能力推迟为运行期分支和失败。
- 源码事实：heap 同时保留 `heap_getnext` 这种 heap 专用直接接口，并提供 `scan_getnextslot` 供通用 TableAM 使用；旧式入口主动检查 `rd_tableam`。架构推论：兼容直接调用者可以渐进迁移，但增加了双入口维护成本；不保留直接接口会要求所有历史调用点同步改造，而无检查的旧入口则可能把非 heap 扫描描述符错当成 heap 状态。
- 源码事实：`heap_beginscan` 会在非 MVCC snapshot 下禁用 page-at-a-time；该入口还有针对非目录 historic snapshot 的错误分支。架构推论：页批处理依赖 MVCC 语义，而不是所有 snapshot 均可套用的纯性能开关；若强行沿用，单页可见性假设可能不成立。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/table/tableam.c` | `table_beginscan_catalog` | 为 catalog 建立已注册的 catalog snapshot，再以公共 TableAM 扫描入口传入固定扫描选项。 |
| `src/backend/access/table/tableam.c` | `table_slot_callbacks` | 按 relation 的 `rd_tableam` 选择 slot ops；对外表和 view/分区表提供兼容的回退 slot 类型。 |
| `src/backend/access/heap/heapam_handler.c` | `GetHeapamTableAmRoutine` | 返回静态 heap 回调表，使 relcache 可将 heap 能力挂到关系上。 |
| `src/backend/access/heap/heapam.c` | `heap_beginscan` | 分配并初始化 heap 专用扫描描述符，绑定 relation、snapshot 与扫描参数。 |
| `src/backend/access/heap/heapam.c` | `heap_getnext` | 执行 heap 专用的下一元组读取，并防止调用者把非 heap AM 描述符传入。 |
| `src/backend/access/heap/heapam.c` | `heap_insert` | 执行 heap 插入回调最终落到的物理 tuple 插入流程；本页只以它说明分派终点。 |

## 与主核 / AI 的关系

当前主核以多模态对象和独立存储模块组织能力，没有 PostgreSQL 式 `TableAmRoutine` 可插拔堆表协议的直接对应物。对照时可借鉴“稳定契约与存储实现分离”的边界，但不能据此推断主核存在 heap tuple 或相同回调生命周期。

## 参考

- [PostgreSQL 文档：表访问方法](https://www.postgresql.org/docs/current/tableam.html) —— 介绍表访问方法的扩展入口与 SQL 可见行为。
- [PostgreSQL 官方源码：TableAmRoutine 契约](https://github.com/postgres/postgres/blob/master/src/include/access/tableam.h) —— 支撑函数表回调与扫描契约的说明。
