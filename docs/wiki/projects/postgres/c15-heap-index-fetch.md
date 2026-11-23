---
project: postgres
feature: heap-index-fetch
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/heap/heapam_indexscan.c
    symbols: [heapam_index_scan_begin]
---

# Heap 回表取行

## 一句话职责

Heap index fetch 消费索引 AM 提供的 TID，读取对应 heap 页并沿 HOT 链寻找符合当前 snapshot 的 tuple。

## 机制是什么

索引访问方法负责返回下一条索引 TID；heap AM 的 index-scan callback 保存每次扫描的页 pin、当前 block 与 VM pin，并把 TID 转成 heap tuple 或可验证的 index-only 结果。普通 index scan 要将找到的 heap tuple 放入 buffer-backed slot；index-only scan 先查 VM，只有 page 未 all-visible 时才回表检查 tuple 可见性。

本页只讲 TID 到 heap 页/tuple 的取回、HOT 链遍历及必要的可见性过滤，不负责索引如何搜索键或产生/排序 TID，也不负责通用 MVCC 可见性算法本身。TID 可能指向 HOT chain 根：`heap_hot_search_buffer` 读取 line pointer，跟随 redirect 或 `t_ctid` 链，找到首个 snapshot 可见成员；如果链上全都不可见且确认 globally dead，plain index scan 可以把 kill-entry 标记返回给 index AM 的后续调用。

## 怎么实现的

1. `heapam_index_scan_begin` 分配 `IndexScanHeapData`，初始化 heap/VM buffer 为无效并记录只读状态；按 `xs_want_itup` 选择普通 tuple 或 index-only 的 `xs_getnext_slot` callback。
2. 每次取行先由 table/index 层获取下一条 TID，保存在 `scan->xs_heaptid`。普通 index scan 进入 heap fetch；index-only scan 先用 TID block 查询 VM，如果 all-visible 就直接把 index 返回的值写入 slot、并取 page predicate lock，不读 heap tuple。
3. 需要 heap page 时，`heapam_index_heap_fetch` 比较目标 block 与缓存的 `xs_blk`；换块时释放旧 pin、`ReadBuffer` 新页并首次 pin 时可尝试 on-access prune。之后持 buffer share lock 调用 `heap_hot_search_buffer`，检查 line pointer、chain 关联和 snapshot 可见性。
4. `heap_hot_search_buffer` 仅在链起点允许跟随 LP_REDIRECT；对 normal tuple 检查前后版本 xmin/xmax 关系及 snapshot。若 tuple 标为 HOT-updated，则使用其 `t_ctid` 的 offset 继续同一 block 链；不满足条件、链断裂或到达非 normal item 即停止。
5. 找到可见版本时把 tuple header、长度、table OID 与实际 chain member TID 写入结果。普通 scan 的 slot 保留 buffer pin，后续调用或 scan end 才释放；index-only 仅需验证并立刻释放内容锁，VM 命中则完全不取 heap tuple。
6. 整条 HOT 链都不可见且 `all_dead` 成立时，plain scan 设置 `kill_prior_tuple`，下次向 index AM 请求下一条 TID 时让 AM 处理 kill hint。恢复过程中不做此优化，以免错误删除仍需由 MVCC/standby 读取的 index entry。

## 关键取舍

- **索引 entry 指向 HOT chain root，heap fetch 负责找当前可见版本。** 这让 HOT 更新不必为每个新版本增加索引项，代价是一次 index hit 可能要检查多个 heap tuple/line pointer；若索引为每个版本都记 entry，更新索引维护成本更高，但可减少链遍历。此页不讲 HOT 链如何生成，只消费其存储链接。
- **普通 index scan 在 slot 生命周期内保留 heap buffer pin，内容锁只在读取时短持。** pin 防止页被回收/替换，share content lock 仅保护访问时的页内容；放弃把 tuple 复制到独立内存的成本较低路径，换取 buffer-backed slot 直接引用页面数据。代价是 slot/扫描器必须按生命周期释放 pin，scan end 是兜底清理点。
- **index-only 依 VM all-visible 跳过 heap 访问，但 VM 未命中则回退回表。** 源码 caller 是 `heapam_index_getnext_slot`：VM 标志为 false 时调用 `heapam_index_only_heap_fetch`，访问失败会继续下一个 TID或在访问页限额下退出。放弃无条件只依赖索引内容的路径，以避免 page 不能保证全局可见时返回不合 snapshot 的结果；回退仍需一次 heap I/O 与 tuple 检查。
- **kill-entry 是延迟提示并避开恢复期。** 全链 globally dead 时只设置 index scan descriptor 标志，由下一次 index AM getnext 消费；恢复期直接跳过。代价是 dead entry 不一定在当前步骤立即清掉；立即修改索引会让 heap AM 越过 index AM 接口边界，也可能在 recovery/MVCC 状态下不安全。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/heap/heapam_indexscan.c` | `heapam_index_scan_begin` | 建立 per-scan heap/VM pin 状态并绑定普通或 index-only callback |
| `src/backend/access/heap/heapam_indexscan.c` | `heapam_index_getnext_slot` | 消费 index TID、选择普通回表或 VM 驱动的 index-only 分支 |
| `src/backend/access/heap/heapam_indexscan.c` | `heapam_index_heap_fetch` | 缓存当前 heap block、持 share lock 搜 HOT 链并填充 slot/可见性结果 |
| `src/backend/access/heap/heapam_indexscan.c` | `heap_hot_search_buffer` | 从 root TID 跟随 redirect/HOT ctid 链，返回 snapshot 可见 tuple |
| `src/backend/access/heap/heapam_indexscan.c` | `heapam_index_kill_item` | 在非恢复期把全 dead 链的 kill hint 交给后续 index AM 调用 |
| `src/backend/access/heap/heapam_indexscan.c` | `heapam_index_scan_end` | 释放扫描末尾仍持有的 heap 与 VM page pins |

## 与主核 / AI 的关系

暂无直接对应。主核的索引扫描应将“索引给出定位符”与“heap 侧取回并按 snapshot 验证”分层；只有明确满足 index-only 可见性条件时才可以跳过 heap 访问。

## 参考

- [PostgreSQL 官方源码：heap index scan](https://github.com/postgres/postgres/blob/master/src/backend/access/heap/heapam_indexscan.c) —— 支撑 TID 回表、HOT 链与 index-only VM 分支
- [PostgreSQL 官方源码：table access method 索引接口](https://github.com/postgres/postgres/blob/master/src/include/access/tableam_indexscan.h) —— 支撑 heap AM 消费 index TID 的边界
- [PostgreSQL 官方文档：索引与 index-only scan](https://www.postgresql.org/docs/current/indexes-index-only-scans.html) —— 支撑 VM all-visible 使 index-only 扫描免于 heap 访问的条件
