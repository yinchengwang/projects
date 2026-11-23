---
project: postgres
feature: logical-tapes
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/sort/logtape.c
    symbols: [LogicalTapeSetCreate, LogicalTapeWrite, LogicalTapeRead]
---

# 逻辑磁带

## 一句话职责

逻辑磁带把排序所需的多个顺序流复用在同一个临时 BufFile 中，并通过回收已读块把临时空间压到接近实际数据量。

## 机制是什么

外部归并天然需要“多条输入带 + 一条输出带”，但若每条带各建一个文件，峰值磁盘占用会显著高于数据量。logtape.c 在单一逻辑文件内维护多个 `LogicalTape`，每个带的数据由块链串起；块头 trailer 记录前驱块与后继块号，末块用负值编码有效字节数，因此块链既能顺序读，也能在 free 时回收。

它与 tuplesort 的分工是：tuplesort 决定何时生成 run、选哪条输入/输出带、如何多路归并；逻辑磁带只负责带内顺序读写、块分配回收和底层缓冲，不理解元组或比较器。它也不负责临时文件跨 OS 大小限制的拼接，那是 BufFile 的职责。

## 怎么实现的

1. `LogicalTapeSetCreate` 建立 `LogicalTapeSet`，初始化已分配/已写出块计数、空闲块最小堆与预分配开关。串行排序创建普通临时 BufFile；并行 worker 在共享 fileset 下按 worker 号创建文件；leader 暂不建文件，等 `LogicalTapeImport` 拼接到首个 worker 文件。
2. 写入时 `LogicalTapeWrite` 惰性分配 BLCKSZ 缓冲。首次写从 tapeset 取块并设该带 firstBlockNumber，trailer 的 `prev` 置 -1；缓冲满时先分配下一块号，把本块 `next` 指向新块、写出本块，再以旧块号作为新块 `prev`，从而保持双向链。
3. 块号来自 `ltsGetBlock`：优先从空闲最小堆取回收块，否则扩展 `nBlocksAllocated`。空闲块堆让写者倾向使用最低块号，注释说明这是为改善写访问局部性所做的选择。
4. 写完一段后 `LogicalTapeRewindForRead` 把剩余脏块刷出，为末块设置有效字节数，然后切换到读状态并按可用内存设置读缓冲。读缓冲可跨多个块，因此一次 `ltsReadFillBuffer` 能连续读入多条带块，减少逐块寻道；读时 trailer 被剥离，缓冲中只剩连续有效数据。
5. `LogicalTapeRead` 在缓冲耗尽时继续填充，按需跨块把数据复制给调用者；请求超出可用数据时返回实际字节数，实现早 EOF 语义。
6. 回收路径在块被完全消费后 `ltsReleaseBlock` 把块号压回最小堆；非 frozen 带读过的块可立即复用，frozen 带（最终结果带）保留全部块，供多次或反向读取。`LogicalTapeSetForgetFreeSpace` 在最终归并前关闭回收记录，避免最后一轮边读边写时做无意义的空间管理。

## 关键取舍

- **源码事实：**所有逻辑带共用一个底层文件，成块分配与回收，空闲块以最小堆管理，倾向于复用最低块号；只有最终输出带会 freeze 保留。**架构推论：**以块级簿记和跨带寻道为代价，把峰值磁盘占用降到接近数据量本身；每带独立文件实现更简单，但输入与输出并存时至少需要近两倍空间。
- **源码事实：**读缓冲可容纳多个块，`LogicalTapeRewindForRead` 的 buffer_size 会在 BLCKSZ 与上限之间取整，frozen 带固定用单块小缓冲。**架构推论：**非随机读取时用大缓冲改善顺序 I/O，随机/反向访问时用小缓冲避免无谓内存占用；统一使用大缓冲会让 frozen 结果带无谓占用 workMem。
- **源码事实：**预分配块号（`TAPE_WRITE_PREALLOC_MIN`/`MAX`）只在并发多带写入时启用，预分配不落盘，必要时写零填补空洞。**架构推论：**批量预留块号减少多带并发写造成的碎片，代价是文件可能出现稀疏空洞；完全顺序即时分配更简单，但并发写者交错时块归属更零散。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/sort/logtape.c` | `LogicalTapeSetCreate` | 建立带集合、空闲块堆并创建或延迟创建底层 BufFile。 |
| `src/backend/utils/sort/logtape.c` | `LogicalTapeWrite` | 按块追加写入带内数据并维护块链前驱/后继。 |
| `src/backend/utils/sort/logtape.c` | `LogicalTapeRead` | 从多块读缓冲按需取值并实现早 EOF 返回。 |

## 参考

- [PostgreSQL 官方源码：逻辑磁带实现](https://github.com/postgres/postgres/blob/master/src/backend/utils/sort/logtape.c) —— 对照块链、块回收与多块读缓冲。
- [PostgreSQL 文档：work_mem 与临时文件](https://www.postgresql.org/docs/current/runtime-config-resource.html#GUC-WORK-MEM) —— 说明排序内存阈值与落盘临时文件的配置语境。

## 与主核 / AI 的关系

暂无直接对应。可借鉴“单文件多逻辑流 + 已读块回收”的空间复用思路；主核若自建外部排序，应先确定块格式与回收协议，再谈复用。
