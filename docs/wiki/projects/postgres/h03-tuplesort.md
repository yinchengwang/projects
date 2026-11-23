---
project: postgres
feature: tuplesort
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/sort/tuplesort.c
    symbols: [tuplesort_begin_common, tuplesort_performsort, tuplesort_gettuple_common]
  - path: src/backend/utils/sort/tuplesortvariants.c
    symbols: [tuplesort_begin_heap]
---

# 元组外部排序

## 一句话职责

tuplesort 把一批元组在内存内排好序，内存不足时生成有序 run 落到逻辑磁带并做多路归并，再按需逐条取回。

## 机制是什么

排序状态按变体提供比较、写入、读取回调，tuplesort.c 负责通用批处理、内存记账、run 生成、归并和取数。小输入直接在 `memtuples` 数组中排序后顺序返回；超过 `workMem` 则建立逻辑磁带集合，把每次内存排序结果作为一个 run 写出，随后用堆做多路归并。

边界上它不实现底层临时文件块链和缓冲预读（那是逻辑磁带层），也不负责具体列比较语义（由 SortSupport 与变体 comparator 提供）。调用者负责提供输入元组与排序键定义；排序不改变元组本身的 MVCC 可见性，也不决定 executor 何时使用哪种排序节点。若调用者请求随机访问，最终结果必须物化为可倒带/后向读取的单个 run；否则可在归并最后一遍边归并边返回。

## 怎么实现的

1. `tuplesort_begin_common` 建立 `TupleSort main` 与 `TupleSort sort` 内存上下文，分配排序状态，设定至少 64KB 的 workMem 下限，并初始化每批次的 tuple 上下文、数组与 slab 等状态。并行排序时拒绝带 RANDOMACCESS 的协调组合。
2. `tuplesort_begin_heap` 调用通用初始化后配置 heap 变体的 `comparetup`、`writetup`、`readtup` 及 `arg`；按排序键逐列建立 `SortSupport`，用 `PrepareSortSupportFromOrderingOp` 取得比较器，并在仅有单键且未启用缩写键时启用 `onlyKey` 优化。
3. 输入阶段按状态追加元组到内存数组：若仍能容纳于内存且未 LACKMEM，则继续累积；有界排序在超过目标数量或内存不足时转成限界堆；否则调用 `inittapes`、`dumptuples` 切换到外部排序，把已累积元组排序后写入当前输出磁带并标记 run 结束。
4. `tuplesort_performsort` 按当前状态收尾。TSS_INITIAL 的内存排序直接排数组；并行 worker 必须写出自己唯一的最终 run；leader 接管 worker 磁带并归并。TSS_BUILDRUNS 先 `dumptuples` 刷尽剩余元组，再 `mergeruns`。
5. `mergeruns` 分配 slab、缩小 `memtuples` 为每个输入磁带的堆槽，重分布 workMem 作为磁带缓冲；每遍把上一遍输出磁带转为输入并 rewind。无随机访问且每个输入磁带只剩一个 run 时，进入 `TSS_FINALMERGE` 边归并边取数；否则遍历 `mergeonerun` 生成新的物化结果磁带并 freeze。
6. `tuplesort_gettuple_common` 按状态取数：内存结果按 `current` 索引扫描；物化磁带结果读取带长度前缀的记录并复用 slab 槽；最终归并状态反复取堆顶、从对应源磁带读下一条并替换堆顶。读完一个 run 时 `tuplesort_heap_delete_top` 收缩堆，全部读完返回 false。

## 关键取舍

- **源码事实：**内存不足后按 workMem 决定 merge order，run 写到逻辑磁带；若不需要随机访问且已接近每个磁带单 run，则跳过最后一次物化归并，边归并边输出。**架构推论：**省掉一整轮结果写盘/读盘，代价是结果磁带保持输入状态、生命周期与取数调用绑定；全量物化实现更简单，但会多一次数据往返。
- **源码事实：**归并使用堆中每个输入 run 的当前首元组，M 路归并内存只需约 M 个元组，另将剩余 workMem 作为磁带预读缓冲；代码注释说明预读是为改善临时文件访问局部性。**架构推论：**以少量堆内存和较多磁带缓冲换取顺序 I/O；完全依赖逐条随机读盘会让每次归并都等待寻道。
- **源码事实：**`inittapestate` 先从可用内存扣除 `maxTapes * TAPE_BUFFER_OVERHEAD`，`tuplesort_merge_order` 根据可用内存计算归并度，并保证至少 `MINORDER`（源码宏值为 6）。**架构推论：**更大的归并度减少归并遍历次数但分摊到每个磁带的缓冲更少；固定最小归并度避免极小内存下退化成大量 pass。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/sort/tuplesort.c` | `tuplesort_begin_common` | 建上下文、算 workMem、初始化通用批次与并行状态。 |
| `src/backend/utils/sort/tuplesort.c` | `tuplesort_performsort` | 依状态完成内存排序、run 写出或归并并置结果状态。 |
| `src/backend/utils/sort/tuplesort.c` | `tuplesort_gettuple_common` | 从内存数组、物化磁带或最终归并堆取下一元组。 |
| `src/backend/utils/sort/tuplesortvariants.c` | `tuplesort_begin_heap` | 配置 heap 变体回调并为各排序列建立 SortSupport。 |

## 参考

- [PostgreSQL 文档：work_mem 资源消耗](https://www.postgresql.org/docs/current/runtime-config-resource.html#GUC-WORK-MEM) —— 说明排序可用内存上限的配置来源。
- [PostgreSQL 官方源码：tuplesort 外部排序](https://github.com/postgres/postgres/blob/master/src/backend/utils/sort/tuplesort.c) —— 对照 run 生成、归并和取数状态机。

## 与主核 / AI 的关系

暂无直接对应。主核可借鉴以内存阈值切换内存排序与外部归并的分层接口；磁带块管理与比较器契约须由主核自身临时存储和类型系统实现。
