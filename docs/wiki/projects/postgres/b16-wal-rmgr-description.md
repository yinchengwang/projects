---
project: postgres
feature: wal-rmgr-description
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/rmgrdesc/xactdesc.c
    symbols: [ParseCommitRecord, ParseAbortRecord, ParsePrepareRecord]
  - path: src/backend/access/rmgrdesc/heapdesc.c
    symbols: [heap_desc, heap2_desc, heap_xlog_deserialize_prune_and_freeze]
  - path: src/backend/access/rmgrdesc/xlogdesc.c
    symbols: [xlog_desc, xlog2_desc, get_checksum_state_string]
  - path: src/backend/access/rmgrdesc/nbtdesc.c
    symbols: [btree_desc, btree_identify]
  - path: src/backend/access/rmgrdesc/gindesc.c
    symbols: [gin_desc, gin_identify]
  - path: src/backend/access/rmgrdesc/hashdesc.c
    symbols: [hash_desc, hash_identify]
---

# WAL record 描述器

## 一句话职责

rmgr description 层把 record 的二进制 payload 与 `xl_info` 解码成可读的操作类型和字段文本，供 `pg_waldump` 等诊断工具理解 WAL，但不执行 redo。

## 机制是什么

WAL record 的公共头只有 rmgr ID、info 位与 payload；具体 payload 的二进制布局由各 rmgr 定义。resource manager vtable 的 `rm_identify(uint8 info)` 不看 record payload，只把 `xl_info` 转成操作名（如 `SPLIT_L`、`COMMIT`）；`rm_desc(StringInfo, XLogReaderState *)` 再从解码后的 record 读取结构体、block data、FPI 标志等字段，追加到输出缓冲区。`pg_waldump` 会借这组回调把 raw WAL 转成可审阅的描述。描述器只读 record buffer，输出文本，不回写 WAL，也不调用 redo apply。

本机制边界是**record 识别与描述**。`xactdesc.c` 的 `ParseCommitRecord` / `ParseAbortRecord` / `ParsePrepareRecord` 是共享的反序列化辅助：按 `xinfo` flags 顺序解释可选字段并填入 parsed 结构，供 backend redo 与 frontend `pg_waldump` 共用；各 `*_desc` 再把识别出的字段格式化为字符串。它不负责 rmgr vtable 如何注册、如何分派（b15），也不负责 record 写入时如何组装 payload（b01），更不负责 redo 将页面改成什么状态（b03）。

所有 rmgr 的描述函数通常由统一的 `RmgrTable` 指针调用，实际通用调用方对描述函数的 `StringInfo` 追加结果与 identifier 文本做组合；工具层提供 record/block 的通用摘要。各 rmgr 描述器独立按自己的 payload schema 解释数据，因此跨版本兼容和格式正确性依赖对应源码与 WAL 格式保持同步；未知 `xl_info` 分支通常不追加细节，而不是尝试“猜”结构体。

## 怎么实现的

1. **先基于 info 字节识别类型。** 各 `*_identify` 对 `info & ~XLR_INFO_MASK` 做 switch，映射到稳定的英文短标签。比如 `btree_identify` 将 insert leaf/upper/meta/post、split、dedup、vacuum 等类型映射为对应字符串；`gin_identify` 与 `hash_identify` 也各自只看自己的 opcode 域。匹配不到的类型返回 `NULL`，不尝试从 payload 推断。
2. **再按 rmgr 格式解读 main data。** `heap_desc` 从 `XLogRecGetData(record)` 取 heap 主数据，先剥出 info mask，再从对应 `xl_heap_*` 结构读 offset、xmax、flags 等字段。`xlog_desc` 对 checkpoint、parameter change、end-of-recovery 等不同 record 解析不同结构，checkpoint 会输出 redo LSN、timeline、WAL level、next XID/epoch、commit timestamp xid 范围与 checksum state。
3. **需要时解析 block data。** 描述器通过 `XLogRecHasBlockData` / `XLogRecGetBlockData` 读取附加块 payload；heap prune record 的 freeze plans、redirect/dead/unused offset 数组是变长序列，`heap_xlog_deserialize_prune_and_freeze` 按 flags 顺序逐个消费游标并返回数组切片，`heap2_desc` 再把计数与数组内容格式化。`btree_desc` 的 vacuum/delete 同样把 block payload 交给 `delvacuum_desc` 解释 offset 与 posting TID 更新列表。
4. **结构体对齐不可假设。** `ParseCommitRecord` 一类 xact parser 根据 `xl_xact_xinfo.xinfo` 有条件地移动 `data` 指针，依次解释 db/tablespace、subxid、relfile locator、stats、invalidation、2PC 和 replication origin 等段；origin 字段前注释明确说此处不保证对齐，所以先 `memcpy` 到局部结构，再逐字段读取。prepare parser 则根据长度字段和 `MAXALIGN` 在 buffer 中定位 GID、subxids、relation locators、stats、messages。
5. **格式化的是人读信息，不是规范化对象。** `xact_desc` 调 parser 后将时间、父/子 XID、关系路径、stats 与 replication origin 输出到 `StringInfo`；`heap_desc` 输出 infobits 组合；`xlog_desc` 把 checksum enum 映成 `on` / `off` / `inprogress-*`；GIN recompress leaf 描述器会按 action 类型跳过插入段或 TID 数组，并对未知 action 输出未知值后停止解析该压缩数据段。
6. **特殊记录可以没有细节。** XLOG FPI 等分支明确留空，因为 record block image 与通用 block ref 足以表达，或 rmgr 主 payload 没有附加字段；`pg_waldump` 的通用 block ref 另由 `XLogRecGetBlockRefInfo` 处理，而不是所有信息塞进 rm_desc。

并发方面，描述函数只在调用方提供的已解码 record 上同步读内存，没有共享可变状态、LWLock 或等待者；生命周期由调用者持有 record buffer。错误边界来自结构字段本身：格式不匹配会出现错误描述或越界风险，所以 parser 必须跟 WAL schema 共同维护，而不是用 heuristics 宽容猜测。

## 关键取舍

**把 `identify` 与 `desc` 分开，而不是一次返回完整描述结构。** `identify` 只依赖 `xl_info`，可独立给出类别名；`desc` 需要完整 `XLogReaderState` 与 payload。放弃的是「一个调用就得到完整、强类型 record 对象」的接口，代价是调用者需组合两个结果；换来的是轻量识别可以在无需解码 payload 时使用，也使 rmgr vtable 固定为两个清晰回调。替代方案需要为所有 rmgr 建统一 tagged union，而每类 record 字段差异很大。

**解析结果直接格式化到 `StringInfo`，不再构建跨 rmgr 通用 AST。** 输出面向诊断，人类可读字符串足够，避免多一次分配与 schema 层；代价是文本格式没有强类型保证，消费者不能把它当稳定的机器协议。真实调用者是 `pg_waldump`：它以 identify 标签和描述文本输出 WAL inspection；redo 代码并不消费这些字符串。

**只由 rmgr 专属 parser 解释变长数据，不尝试通用猜测。** `ParseCommitRecord` 根据 `xinfo` flag 顺序推进 payload 指针，heap prune helper 依据 flags 逐段反序列化；错误布局不是“能尽量读出多少是多少”的场景。放弃的是跨版本的宽松猜解能力，换来明确的字段边界与较低的误读风险。代价是 WAL schema 改变时描述器必须同步更新；如 origin payload 未对齐，源码的 `memcpy` 临时副本就是具体安全处理。

**块引用与 main-data 描述分工，而非把所有信息重复打印。** `XLogRecGetBlockRefInfo` 独立处理每个 block tag、fork、块号、FPI 长度；rm_desc 只补充 rmgr 语义。放弃的是单一描述字符串自给自足，换来通用工具一次实现、所有 rmgr 共享的 block 格式。真实 caller 是 `pg_waldump` 的 record 输出路径，它可按 pretty/detailed 参数选择通用 blkref 展示；rmgr 描述器不负责此布局。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/rmgrdesc/xactdesc.c` | `ParseCommitRecord` | 依 commit record 的 `xinfo` 位逐段定位可选字段，把原始变长 payload 转成 `xl_xact_parsed_commit` 视图，供 formatter 与 redo 共用 |
| `src/backend/access/rmgrdesc/xactdesc.c` | `ParseAbortRecord` | 按 abort record 的实际布局提取子 XID、关系、统计信息、2PC 与 origin 字段；未对齐的 origin 先复制到本地结构 |
| `src/backend/access/rmgrdesc/xactdesc.c` | `ParsePrepareRecord` | 用 prepare record 的长度/计数字段和 `MAXALIGN` 计算 GID、子 XID、关系及消息数组的边界 |
| `src/backend/access/rmgrdesc/heapdesc.c` | `heap_desc` | 识别 heap 主 record 的 insert/delete/update/lock/truncate 等类型并输出关键 tuple offset、xmax、flags 与 invalidation 元数据 |
| `src/backend/access/rmgrdesc/heapdesc.c` | `heap2_desc` | 描述 heap2 的 prune/freeze、多行插入、锁更新、新 CID 等 record，并在 prune 时展示反序列化的变长页内动作 |
| `src/backend/access/rmgrdesc/heapdesc.c` | `heap_xlog_deserialize_prune_and_freeze` | 按 `XLHP_HAS_*` flags 消费 prune payload，切出 freeze plan、redirect、dead、unused 数组给 formatter 和 redo 共用 |
| `src/backend/access/rmgrdesc/xlogdesc.c` | `xlog_desc` | 将 checkpoint、parameter-change、end-of-recovery 等核心 WAL 记录的结构化字段转换成可读文本 |
| `src/backend/access/rmgrdesc/xlogdesc.c` | `xlog2_desc` | 把第二类 XLOG opcode 的 checksum 状态 payload 转成状态字符串 |
| `src/backend/access/rmgrdesc/xlogdesc.c` | `get_checksum_state_string` | 把 checksum 状态枚举映为 `on`、`off`、`inprogress-on/off`；四个 `PG_DATA_CHECKSUM_*` 之外的值会先撞上 `Assert(false)`，所以断言构建下是中止而不是返回字符串，非断言构建才落到 `return "?"` |
| `src/backend/access/rmgrdesc/nbtdesc.c` | `btree_desc` | 描述 B-tree split、insert、dedup、vacuum、unlink、reuse 等 record 的结构字段，必要时拆 block data 的 offset/TID 列表 |
| `src/backend/access/rmgrdesc/nbtdesc.c` | `btree_identify` | 把 B-tree 的 info opcode 映为 pg_waldump 展示的类型名，不读取 payload |
| `src/backend/access/rmgrdesc/gindesc.c` | `gin_desc` | 输出 GIN 插入、split、vacuum、列表页等 record 的 flags/子页信息，并按压缩 segment action 解释变长叶页载荷 |
| `src/backend/access/rmgrdesc/gindesc.c` | `gin_identify` | 把 GIN info opcode 映为操作类别字符串，未识别的 opcode 返回 NULL |
| `src/backend/access/rmgrdesc/hashdesc.c` | `hash_desc` | 解读 hash 索引初始化、插入、split、squeeze、delete 与 vacuum record 的具体参数 |
| `src/backend/access/rmgrdesc/hashdesc.c` | `hash_identify` | 把 hash info opcode 映为类型名，与 payload formatter 分离 |

## 参考

- [PostgreSQL 官方源码：xactdesc.c](https://github.com/postgres/postgres/blob/master/src/backend/access/rmgrdesc/xactdesc.c) —— commit/abort/prepare record 的公共解析与描述
- [PostgreSQL 官方源码：heapdesc.c](https://github.com/postgres/postgres/blob/master/src/backend/access/rmgrdesc/heapdesc.c) —— heap/heap2 record 与 prune payload 反序列化
- [PostgreSQL 文档：`pg_waldump`](https://www.postgresql.org/docs/current/pgwaldump.html) —— 使用 WAL record 的识别和人类可读描述进行离线检查

## 与主核 / AI 的关系

暂无直接对应。
