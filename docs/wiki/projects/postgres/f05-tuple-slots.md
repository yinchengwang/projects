---
project: postgres
feature: tuple-slots
page_type: feature
schema_version: 1
refs:
  - path: src/backend/executor/execTuples.c
    symbols: [MakeTupleTableSlot, ExecStoreVirtualTuple, tts_buffer_heap_store_tuple, tts_virtual_init]
---

# TupleTableSlot 表示

## 一句话职责

`TupleTableSlot` 是执行器内部承载「一行」的统一句柄，它把元组放在哪、由谁负责释放这件事收敛进一张 `TupleTableSlotOps` 虚表，让上层节点只按「取 slot、取列」编程。

## 机制是什么

执行器在计划节点之间传递的是 slot 指针而不是元组本身。一个 slot 同时携带五样东西：指向实现行为的 `tts_ops`、行描述 `tts_tupleDescriptor`、当前行的 `tts_values`/`tts_isnull` 数组，以及 `tts_tid`/`tts_tableOid` 两个身份字段。有了这层，「这行的数据住在别处」与「这行的数据归我」的区别由 slot 自己记账，上层不必知道。

slot 有四种内置形态，各自对应一种「行的存放位置 + 释放责任」：Virtual（只有 Datum/isnull 数组，数组即权威数据）、HeapTuple（palloc 出来的物理元组）、MinimalTuple（不含系统列的精简物理元组）、BufferHeapTuple（元组位于共享缓冲的页里，slot 持有该页的 pin）。四种形态由 `TTSOpsVirtual`、`TTSOpsHeapTuple`、`TTSOpsMinimalTuple`、`TTSOpsBufferHeapTuple` 四张 const 虚表区分，`tts_ops` 指针本身就是形态判别依据（`TTS_IS_VIRTUAL` 等宏按指针相等比较）。

两套「延迟」机制并存。其一是**取值延迟**：`tts_values`/`tts_isnull` 不是存行时填满，而是第一次访问某列时才从物理元组解形变，且是增量的——已解出的列不重解。其二是**物化延迟**：virtual slot 里的 pass-by-reference Datum 初始指向下游节点的存储或 per-tuple 上下文，只有当它必须独立存在时才把数据复制进 slot 自己的内存上下文。

本页只讲 slot 的表示与 slot 之间的所有权/释放语义。它**不负责**：一行是否对本事务可见（快照与 heap AM 的判断）、元组在页内的字节布局与行标识、buffer pin 如何取得与页如何被换出（缓冲池），以及执行期状态对象的分配与释放（`EState`/`ExprContext` 的生命周期另页）。

## 怎么实现的

1. **创建**。`MakeTupleTableSlot` 按 `tts_ops->base_slot_size` 加上两个按列数计算的数组，一次性 `palloc0` 出整块内存：`tts_values` 紧跟在 slot 本体之后，`tts_isnull` 再往后并按 `TYPEALIGN(8, ...)` 对齐（源码注释说明 `populate_isnull_array()` 一次处理 8 个元素）。传入的 `flags` 先与 `~TTS_FLAGS_TRANSIENT` 相与，调用方无法把「空」「自有元组」这类瞬时状态固化进 slot。新 slot 置 `TTS_FLAG_EMPTY`；给了 `tupleDesc` 就额外置 `TTS_FLAG_FIXED` 并 `PinTupleDesc` 增加一次引用。若 `TTS_FLAG_OBEYS_NOT_NULL_CONSTRAINTS` 打开，则把 `tupleDesc->firstNonGuaranteedAttr`（第一个可空列的下标）抄进 `tts_first_nonguaranteed`，供后续形变时跳过必然非空的列。最后调 `tts_ops->init(slot)` 让具体形态做自己的初始化。
2. **虚表**。`TupleTableSlotOps` 是纯虚表：`init`/`release`/`clear` 管生命周期，`getsomeattrs`/`getsysattr`/`is_current_xact_tuple` 管取值，`materialize` 管独立化，`copyslot` 管复制，`get_heap_tuple`/`get_minimal_tuple`/`copy_heap_tuple`/`copy_minimal_tuple` 管物理元组形态的导出。「不支持」的回调可以是 NULL——virtual slot 的 `get_heap_tuple` 为 NULL，buffer heap slot 的 `get_minimal_tuple` 为 NULL——调用方经 `ExecFetchSlotHeapTuple` 这类统一入口分派到 copy 版本并置 `shouldFree`。
3. **虚拟形态**。`tts_virtual_init` 是空实现：virtual slot 的额外字段只有 `data`，只在物化之后才用得上，初始化阶段不必动作。写入协议分三步：先 `ExecClearTuple` 把 slot 标记为空，直接把值写进 `tts_values`/`tts_isnull`，最后调 `ExecStoreVirtualTuple` 清掉 `TTS_FLAG_EMPTY` 并把 `tts_nvalid` 置为 `natts`。源码注释直说这样安排「avoids one round of data copying」。virtual slot 的 `getsomeattrs` 直接 `elog(ERROR)`——它的数组天生就是满的；`getsysattr` 与 `is_current_xact_tuple` 也各自报错，因为虚拟行既没有系统列也没有事务信息。
4. **自有元组形态**。`tts_heap_store_tuple` 在接元组时先做一次 `heap_datum_check`（源码注释：与其在每次复制和每次增量形变时都检查，不如在元组进入 slot 时校验一次），然后清空旧内容、挂上新元组、清 `tts_nvalid`、记 `tts_tid`，并按 `shouldFree` 决定是否置 `TTS_FLAG_SHOULDFREE`。`shouldFree` 为假时所有权留在下游节点的 slot 里，上层必须保证比下游更早放弃该元组，否则应自行 `heap_copytuple` 留一份副本。
5. **页内元组形态**。`tts_buffer_heap_store_tuple` 是这里最讲究的一处。页内元组本身不可 `pfree`，因此「自有元组」与「持有 buffer pin」互斥：`TTS_FLAG_SHOULDFREE` 置位时断言 slot 不再持有 buffer。pin 分两种模式：`transfer_pin` 为假（`ExecStoreBufferHeapTuple`）时 slot 自己 `IncrBufferRefCount` 一次；为真（`ExecStorePinnedBufferHeapTuple`）时把调用方的 pin 接管过来，调用方不得再释放。若新 buffer 与 slot 原先持有的 buffer 相同，源码刻意**不做** release/re-pin——注释说这在 seqscan 里是常见情形，重新拿一次 pin 是白费。真实调用方是 `src/backend/access/heap/heapam.c` 的 `heap_getnextslot`，它把 `&scan->rs_ctup` 与 `scan->rs_cbuf` 交给 slot。
6. **释放与物化**。`tts_buffer_heap_clear` 分两种情况：slot 已物化（元组是自有副本）则 `heap_freetuple` 并断言此时没有 buffer；否则若 buffer 有效则 `ReleaseBuffer`。`tts_buffer_heap_materialize` 反向做这件事：把元组复制进 slot 自己的内存上下文，释放 buffer pin，最后才置 `TTS_FLAG_SHOULDFREE`。源码注释解释了顺序：先释放 buffer 再置自有标志，是为了避免出现「slot 既自有元组又持有 buffer」的瞬时状态，从而不违反断言。virtual slot 的物化是另一条路：`tts_virtual_materialize` 先逐列计算总长度（expanded 对象会被 flatten 成自持数据），再一次 `MemoryContextAlloc` 分配整块，再逐列拷入并把 `tts_values` 改指向新空间。
7. **延迟取值**。`slot_getattr`/`slot_getsomeattrs` 只在 `attnum > slot->tts_nvalid` 时才调 `ops->getsomeattrs`。物理元组形态的 `getsomeattrs` 走 `slot_deform_heap_tuple`：它标了 `pg_always_inline`，把每次解形后的字节偏移记在 slot 自己的 `off` 字段里，于是第二次取列只从上次位置继续；元组列数不足时调 `slot_getmissingattrs` 按描述符里的缺省值补齐，请求列数超出描述符则报错。

## 关键取舍

- **用虚表而不是标志位分派形态。** 源码事实：四种形态各有一张 `const TupleTableSlotOps`，`tts_ops` 指针同时充当形态标识。放弃「在每个取数点写 `if (slot->tts_flags & ...)`」的直白做法，换来新增 slot 形态不必改核心代码；代价是每次取列多一次间接调用、每个 slot 多一个指针，且调用方必须经 `slot->tts_ops->` 取值而不能直接摸 `tts_values`，否则会绕过 `getsomeattrs` 的延迟解形。
- **取值延迟 + 增量解形。** 源码事实：`tts_nvalid` 与各形态私有的 `off` 字段一起记录解形进度，`slot_deform_heap_tuple` 明确标为 `pg_always_inline` 以便让各形态的 `offp` 被优化掉。放弃「store 时一次解出所有列」的简单实现，换取只投影两列的查询不必解析整行；代价是每个 slot 多两个状态字段、取列路径上多一次比较，缺列场景还要走 `slot_getmissingattrs` 补值。
- **slot 持有 buffer pin 直到被清空，而不是取到元组就放 pin。** 源码事实：`tts_buffer_heap_clear` 负责 `ReleaseBuffer`，`tts_buffer_heap_store_tuple` 只在 buffer 变化时才重新 pin。放弃「取出元组立刻释放 pin」的写法（那会读到已被换出、已失效的页），代价是被 pin 的页在 slot 清空前不能被缓冲池换出，扫描期间会抬高换出压力；替代方案是每次扫描把元组拷出来再放 pin，等于每行多一次内存拷贝——这也是 `tts_buffer_heap_materialize` 只在明确要求独立化时才做的原因。
- **virtual slot 物化时先算总长再一次性分配。** 源码注释写明这样「good for cache hit ratio, but more importantly requires only memory allocation/deallocation」。放弃逐列单独分配的简单写法，换取一次分配一次释放；代价是要先把所有列的长度与对齐算一遍，并为 expanded 对象额外走一次 flatten。
- **materialize 之后必须先释放 buffer 才置自有标志。** 源码事实：`tts_buffer_heap_materialize` 的注释明确说明这一顺序是为避免「自有 slot 不持有 buffer」这条断言被瞬时状态违反，并承认「若 `ReleaseBuffer()` 报错，我们会泄漏拷贝出来的元组，但这似乎是无害的」。放弃「先置标志后释放」的直觉写法，换取断言始终成立；代价是承认了一条极小概率的泄漏路径。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/executor/execTuples.c` | `MakeTupleTableSlot` | 按 ops 的 `base_slot_size` 一次分配 slot 与两个数组，掩掉瞬时标志，置空/固定标志并 pin 描述符 |
| `src/backend/executor/execTuples.c` | `ExecStoreVirtualTuple` | 虚拟元组写入协议的收尾动作：清掉空标志并把 `tts_nvalid` 置满，省掉一轮数据拷贝 |
| `src/backend/executor/execTuples.c` | `tts_buffer_heap_store_tuple` | 页内元组入槽：管理 buffer pin 的取得与接管、同页免重 pin 的优化，并维持「自有元组」与「持有 pin」互斥 |
| `src/backend/executor/execTuples.c` | `tts_virtual_init` | virtual 形态的初始化钩子；空实现，因为 `data` 字段只在物化之后才被使用 |

## 与主核 / AI 的关系

主核当前没有等价的执行器行槽层：`engine/db/src/query/exec.h` 的 `QueryResult` 直接把结果行存成格式化的字符串数组加 NULL 位图，行在执行期即被物化。要注意概念同名不同层——主核 `engine/db/src/storage/page.h` 里的 slot 指「页内槽号」，与本页的执行器行槽不是一回事。若主核后续引入投影/表达式算子，可借鉴「形态变体 + ops 虚表 + 取值延迟」的分层，让行数据的存放位置与释放责任随形态变化，而不外泄到算子代码里。

## 参考

- [PostgreSQL 官方源码：TupleTableSlot 实现与各形态 ops](https://github.com/postgres/postgres/blob/master/src/backend/executor/execTuples.c) —— 支撑 slot 家族、延迟解形与 pin 归属的源码事实
- [PostgreSQL 官方源码：TupleTableSlot 与 TupleTableSlotOps 定义](https://github.com/postgres/postgres/blob/master/src/include/executor/tuptable.h) —— 支撑虚表回调集合、标志位语义与四种内置形态
- [PostgreSQL 官方源码：heap 扫描把页内元组存入 buffer slot](https://github.com/postgres/postgres/blob/master/src/backend/access/heap/heapam.c) —— 支撑 `ExecStoreBufferHeapTuple` 的真实调用方
