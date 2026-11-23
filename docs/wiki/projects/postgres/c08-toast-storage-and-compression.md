---
project: postgres
feature: toast-storage-and-compression
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/heap/heaptoast.c
    symbols: [heap_toast_insert_or_update, heap_toast_delete]
  - path: src/backend/access/common/toast_internals.c
    symbols: [toast_save_datum, toast_compress_datum]
  - path: src/backend/access/common/detoast.c
    symbols: [detoast_attr, detoast_attr_slice]
  - path: src/backend/access/table/toast_helper.c
    symbols: [toast_tuple_try_compression]
  - path: src/backend/access/common/toast_compression.c
    symbols: [pglz_compress_datum]
---

# TOAST 外置与压缩

## 一句话职责

TOAST 将过大的变长属性压缩或移到关联 toast relation 中分块存放，并在读取时按需重组完整值或所需切片。

## 机制是什么

heap tuple 必须留在目标页可容纳的尺寸内。`heap_toast_insert_or_update` 根据列的 `attstorage` 策略和 `toast_tuple_target` 先尝试压缩，再把适合外置的 varlena 存到 toast relation；主 heap tuple 保留指向 toast table、value ID、原始尺寸和存储尺寸/压缩标记的 external pointer。toast relation 以 value ID、chunk sequence 和 chunk data 保存分片，相关索引支持按 value 定位。

`detoast_attr` 识别 on-disk external、inline compressed、short header、indirect 与 expanded 表示并返回可用 varlena；`detoast_attr_slice` 对普通外置值可只取请求区间，对压缩值则按压缩方法决定取足够前缀还是全部分片。UPDATE/DELETE 时旧 external value 只有在不再被新 tuple 复用后才清除；这里不负责普通 tuple 页布局、descriptor 编码或 VM 可见性位维护。

## 怎么实现的

1. heap 插入/更新准备阶段调用 `heap_toast_insert_or_update`，将候选新旧 tuple 用 TupleDesc 解构成 Datum/null 数组，并初始化 `ToastTupleContext` 与每列大小、storage/compression 及旧 external 引用状态。输入 tuple 不被原地改写；若无属性需变化直接返回原 tuple，否则重建临时 tuple。
2. 计算带 null bitmap/header 的 tuple target 数据长度后，第一轮选最大候选列：EXTENDED 可以尝试 inline 压缩，EXTERNAL 不参与压缩；单个值压后仍超过 target 时可立即外置。第二轮将仍 inline 的 EXTENDED/EXTERNAL 外置；第三轮才尝试压缩 MAIN；最后只有仍超过 MAIN 目标时才外置 MAIN，尽量保留 MAIN 值在行内。
3. `toast_tuple_try_compression` 调用 `toast_compress_datum`，成功时替换 Datum、记录临时内存归属及新大小；失败则标记该列不可压缩，避免后续重复尝试。压缩方法由列 compression 属性或 `default_toast_compression` 选择 PGLZ/LZ4；PGLZ 路径先按策略输入范围拒绝不合适值，再分配上界缓冲并调用 `pglz_compress`。
4. `toast_compress_datum` 不因压缩器报告成功就一律保留结果，还比较实际压缩尺寸是否超过最小节省量，以免 header/alignment padding 抵消压缩。满足条件时在 compressed varlena 元数据记原始尺寸与 compression ID，否则释放压缩副本并报告不可压缩。
5. `toast_save_datum` 打开 toast relation 与索引，准备 external size/raw size/value ID 和 compression ID；需要保留旧 value ID 时先确认来自同一 toast relation，否则生成新 ID。它按 toast chunk 最大尺寸拆分数据，每块插入 toast heap 并更新 ready 索引，最后返回引用该值的 on-disk pointer。
6. `detoast_attr` 对 on-disk pointer 取回全部分片，若其内容压缩则再解压；inline compressed 直接解压，short varlena 扩成常规 header。`detoast_attr_slice` 校验 offset/length 溢出：未压缩外置值直接只取对应 slice；PGLZ 压缩 slice 可根据解压前缀推算足够 compressed bytes，LZ4 当前路径需获取完整压缩内容；解压后复制所需区间。
7. UPDATE 清理 `ToastTupleContext` 时删除被新 tuple 替换的旧 external values；DELETE 将旧 tuple 解构并对 on-disk external pointer 定位 chunk 后逐条 heap delete，speculative 删除则走 abort speculative。TOAST relation 与索引锁保持到事务结束，以便与直接 reindex 操作协调。

## 关键取舍

- 源码事实：压缩与外置顺序区分 EXTENDED、EXTERNAL、MAIN；MAIN 仅在前面策略仍无法满足目标时才外置。架构推论：优先保持 MAIN 列行内可快速读取，代价是个别 tuple 可能要更激进处理其他列；若所有列一视同仁外置，行更小但常见值访问会增加 TOAST I/O。
- 源码事实：PGLZ/LZ4 压缩成功后仍有严格净尺寸复核，压缩结果不足以抵消头部及对齐开销就释放。架构推论：避免把无收益压缩标记带入读取路径；若只相信压缩器返回值，会增加存储尺寸或解压成本而没有空间收益。
- 源码事实：TOAST 值用序列号分块，完整读取重建全部 chunk，而 slice 读取可只取所需范围；压缩 PGLZ slice可按所需前缀估算数据量，LZ4 当前实现取全体压缩内容。架构推论：未压缩值和可前缀解压格式获得较低随机读取 I/O，复杂压缩格式以较简单安全的全量获取作为代价；为任意位置随机切片设计独立块压缩会增加元数据与 chunk 管理复杂度。
- 源码事实：TOAST 删除从旧 tuple 的 external pointer 找到 value ID，并删关联 chunk；更新路径通过旧 external 标记判断是否需要清理。架构推论：引用随 heap 版本/事务生命周期协调，避免新版本仍引用时过早删除；若新旧版本无共享判定就直接删，会破坏仍需该 toast 值的 tuple。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/heap/heaptoast.c` | `heap_toast_insert_or_update` | 按存储策略压缩/外置变长列，并在需要变化时重建候选 heap tuple。 |
| `src/backend/access/heap/heaptoast.c` | `heap_toast_delete` | 解构旧 heap tuple 并为其 external 字段启动 toast 值回收。 |
| `src/backend/access/common/toast_internals.c` | `toast_save_datum` | 生成或复用 value ID、把 payload 切块写入 toast heap/index 并构造 external pointer。 |
| `src/backend/access/common/toast_internals.c` | `toast_compress_datum` | 按指定/default compression method 压缩，并过滤实际节省不足的结果。 |
| `src/backend/access/common/detoast.c` | `detoast_attr` | 将 external、compressed、short、indirect 或 expanded 表示展开成调用方可用值。 |
| `src/backend/access/common/detoast.c` | `detoast_attr_slice` | 对 external/compressed 值按请求区间恢复数据，避免未压缩外置值全量读取。 |
| `src/backend/access/table/toast_helper.c` | `toast_tuple_try_compression` | 管理单列压缩成功后的替换、内存释放标记和失败后的跳过标志。 |
| `src/backend/access/common/toast_compression.c` | `pglz_compress_datum` | 应用 PGLZ 输入尺寸范围和压缩策略，将结果编码成 compressed varlena。 |

## 与主核 / AI 的关系

主核存在独立对象存储与模态数据管理，但没有 PostgreSQL varlena 外置、toast chunk relation 或 PGLZ/LZ4 tuple 压缩的直接对应。可借鉴“主记录保留外部 payload 引用、按需取片”的思路；不应把 TOAST 当作主核 blob/向量对象存储实现。

## 参考

- [PostgreSQL 文档：TOAST](https://www.postgresql.org/docs/current/storage-toast.html) —— 说明大字段压缩与外置存储、指针和切片访问。
- [PostgreSQL 官方源码：heap TOAST 流程](https://github.com/postgres/postgres/blob/master/src/backend/access/heap/heaptoast.c) —— 支撑 tuple 尺寸处理与压缩/外置的分阶段策略。
