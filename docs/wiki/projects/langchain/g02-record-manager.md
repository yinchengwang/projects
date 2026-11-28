---
project: langchain
feature: record-manager
title: 记录管理器
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/indexing/base.py
    symbols: [RecordManager, InMemoryRecordManager, _Record]
  - path: libs/core/langchain_core/indexing/base.py
    symbols: [UpsertResponse, DeleteResponse, DocumentIndex]
---

# 记录管理器

## 一句话职责

用一个「键 → (分组, 更新时间)」的外部簿记，把「上次索引时见过哪些文档」这件事从向量库里搬出来——于是任意只支持「按 id 增删」的后端都能做增量索引，代价是这份簿记本身变成了一个可能不一致的分布式状态。

## 机制是什么

`RecordManager` 是一个 ABC，构造只收一个 `namespace`。它要持久化的东西极简：`_Record` 是个 `TypedDict`，只有两个键——`group_id: str | None` 与 `updated_at: float`。所以它记的不是文档内容，而是「这个 id 属于哪个源、什么时候写的」。

类 docstring 把整个抽象的定位与两条约束写全了，值得逐句读：

**定位**：「记录管理器跟踪哪些文档已被写进 `VectorStore` 以及它们何时被写入。索引 API 为每份文档算哈希，把哈希连同写入时间与 source id 一起存进记录管理器。之后的索引轮次里，索引 API 可以查记录管理器来判断哪些已经索引过、哪些没有」。

**收益**：「这个抽象的主要好处是它能跨很多向量库工作。要被支持，一个 `VectorStore` 只需具备按 ID 添加与删除文档的能力」。

**约束一**：「它依赖时间戳来判断哪些文档已经索引、哪些没有。这意味着时间戳必须**单调递增**。时间戳应当是该**服务端**测量的时间，以尽量减少问题」。

**约束二**：「记录管理器目前是与向量库**分开实现**的，这意味着整个系统变成分布式的，可能产生一致性方面的问题。例如，写记录管理器成功，但对应的写 `VectorStore` 失败」。

这两条约束就是这一页的全部张力所在：一条是「时间必须单调」，另一条是「两处写入无法原子」。

## 怎么实现的

**六个抽象方法都是成对的同步/异步。** `create_schema` / `acreate_schema`、`get_time` / `aget_time`、`update` / `aupdate`、`exists` / `aexists`、`list_keys` / `alist_keys`、`delete_keys` / `adelete_keys` —— 一共十二个抽象方法，实际是六件事。

这不是「顺手给了异步版」的客气：每一个同步方法的 docstring 都在异步版里被**复制一份并只改首句**。所以抽象层强制实现者面对「这两条路径都要能走通」这件事，而不是先写同步、异步留待将来。对一个要服务数据库后端（pgvector 之类）的抽象来说，异步是原生形态，同步是包装——所以两者都得在契约里。

`create_schema` 的存在说明这个抽象**预期落在有 schema 的存储上**。`InMemoryRecordManager` 的实现体只有一个 docstring、什么都不做，docstring 说「内存版建 schema 只是确保结构已初始化」——但代码里连初始化都没有，因为 `self.records` 在 `__init__` 里就建好了。所以这是一个「抽象要求、内存实现用空操作满足」的方法，它存在的意义是把「你可能需要建表」这件事提到契约层。

**`get_time` 的 docstring 是整份契约里最重的一句**：「从服务端取当前时间作为高分辨率时间戳！从服务端取时间以确保单调时钟很重要，否则清理旧文档时可能**丢数据**」。

为什么丢数据？因为清理逻辑是「找出记录里早于某个时间戳、且本轮没出现的文档，删掉」。若客户端时钟比记录簿所在机器的时钟快，那么「刚刚写入的记录」会带着一个**未来**时间戳；下一轮清理时用客户端时钟做基准，那些记录看起来就是「很久以前写的」——于是被误删。所以这个时间戳必须与簿记同源（同一台机器的时钟），才能保证「写入时间 < 清理基准」这个关系稳定成立。

`InMemoryRecordManager.get_time` 返回 `time.time()`，`aget_time` 直接调 `get_time()`。内存版没有「服务端」，所以只能用本地时钟——而本地时钟恰好就是簿记所在进程，于是同源自洽。这个实现看起来简陋，但它满足了契约要求的那个性质。

**`a*` 方法在内存版里全是同步实现套壳。** `aupdate` 调 `update`；`aexists` 调 `exists`；`alist_keys` 调 `list_keys`；`adelete_keys` 调 `delete_keys`。所以「异步」在这里没有真正的事件循环让步——内存操作本来就是瞬时的。

对照 `DocumentIndex` 的默认异步实现：它用 `run_in_executor(None, self.upsert, items, **kwargs)` ——把同步方法丢进线程池。所以同一份代码里，一个抽象用「直接调同步」表达异步，另一个用「丢线程池」表达异步。前者因为内存操作不该阻塞，后者因为子类的同步实现可能是阻塞 I/O。

**`update` 的两个前置校验。** `InMemoryRecordManager.update` 在写之前做两件事：

```python
if group_ids and len(keys) != len(group_ids):
    raise ValueError("Length of keys must match length of group_ids")
```

第一条是**位置对应**校验。`group_ids` 与 `keys` 是两个平行序列，靠下标对齐；长度不等意味着「某些 key 没有分组、或某些分组没有 key」，无论哪种都是调用方的 bug。抽象层的 docstring 也把这条写进了 `Raises` 段。

第二条在循环内：

```python
for index, key in enumerate(keys):
    group_id = group_ids[index] if group_ids else None
    if time_at_least and time_at_least > self.get_time():
        raise ValueError("time_at_least must be in the past")
    self.records[key] = {"group_id": group_id, "updated_at": self.get_time()}
```

`time_at_least` 的语义在抽象层 docstring 里写得比代码更详细：「实现可以用它来可选地验证时间戳**确实是**至少这个时间——例如，用来验证 postgres 数据库里的时间等于或大于给定时间戳，若不是就报错。这是为了帮助防止时钟漂移问题，因为时间可能不是单调递增的！」

所以这个参数是**调用方给出的自证**：调用方说「我认为现在是 T」，实现方拿自己的时钟验证「我的时间 >= T」；不满足就说明两处时钟已经漂了，宁可报错也不能写下一个会误导后续清理的时间戳。

内存版的检查放在循环内而不是循环外，有一个副作用：每写一条 key 都重新调一次 `get_time()`，所以**同一个批次里不同 key 的 `updated_at` 可能不同**（批越长差得越多）。从清理的角度看这其实是好事——它让时间戳更接近真实写入时刻，而不是批次开始时刻。但从「一批应当是原子的」角度看，这又是一个不一致点。抽象层没有规定一批必须同一时间戳，所以两种实现都合法。

**`list_keys` 的四个过滤条件与它们的真假判定。** 实现是逐条 `continue`：

```python
if before and data["updated_at"] >= before: continue
if after and data["updated_at"] <= after: continue
if group_ids and data["group_id"] not in group_ids: continue
result.append(key)
...
if limit: return result[:limit]
```

两件事需要注意。

第一，`before` / `after` 的边界是**严格不等**：`before` 保留 `<`（`>=` 跳过），`after` 保留 `>`（`<=` 跳过）。docstring 的措辞是「before: 列出更新时间**早于**此时刻的记录」，与严格的 `<` 一致。所以 `after=T` 与 `before=T` 都不会包含「恰好等于 T」的记录——意味着「时间戳恰好相等」这个边界不属于任何一个区间。对清理场景这没问题（它要的是「上一轮之前」），但调用方若想「取 T 之后（含 T）」就得自己减一个 epsilon。

第二，`if before` / `if after` / `if group_ids` / `if limit` 都是**真假判定**而不是 `is not None`。于是 `before=0.0` 被当成「没给」，`limit=0` 被当成「没给」（返回全部而不是空列表）。前者在 Unix 时间戳语义下无害（0 表示 1970 年，没有记录会早于它），后者是一个真实的陷阱：想「只要数量、不要内容」的调用方拿到的是全量。

**`exists` 与 `delete_keys` 的宽容度方向相反。** `exists` 返回与 `keys` 等长的 bool 列表——它必须回答每一个 key，因为调用方用它做位置对齐的批量判断。而 `delete_keys` 对不存在的 key **静默跳过**（`if key in self.records: del`）。

这两个方向各有理由：`exists` 的位置对应让它成为「批量筛选」的基础；`delete_keys` 的宽容让它成为**幂等**操作——重跑一次删除不会因为「已经删过了」而失败。这也与 `DeleteResponse` 的 docstring 一致（后文）。

**`UpsertResponse` 的文档写清了「部分失败时响应变成 ill defined」。** 这是这一层最有意思的设计说明。原文：「若无失败，failed 列表为空，且 succeeded 列表里的 ID 顺序会与输入文档顺序一致。若有失败，响应就变成**定义不良的**，API 的使用者无法确定哪个生成的 ID 对应哪份输入文档。建议使用者显式地给被索引的条目附加 ID，以避免这个问题」。

所以这是一个被**明确承认的语义缺口**：批量 upsert 靠位置对齐，一旦有失败，成功项与输入项的位置关系就断了。而文档给出的解法不是「补一个映射字段」，而是「请你自己传 ID」——因为只有在输入就带 ID 的情况下，调用方才不需要从响应里反推。

这条 doc 值得当作范例：它没有假装接口是完备的，而是**画出它的失效边界并给出规避方式**。

**`DeleteResponse` 的四字段全是可选，且「不存在不算失败」。** 它是 `TypedDict, total=False`——所以实现可以只返回其中一部分。docstring 对每个字段都写了同一句话的前半：`num_deleted` 与 `succeeded` 「如果返回，只应包含**实际的**删除。若 ID 一开始就不存在，它不应被计入」。

而 `failed` 那段用的是警告块：「删除一个不存在的 ID **不是**失败」，`num_failed` 是失败计数。

这组约定让「删除」成为幂等操作的形式化版本：重复删除同一个 ID，第一次算成功，第二次什么也不算（不是成功、也不是失败）。这也解释了 `indexing/api.py` 里那句「只在明确失败时才报错」为什么成立——因为「没删到东西」在这个契约里根本不是一个可报告的结果。

**`DocumentIndex` 是另一个抽象，且是 `beta`。** 它继承 `BaseRetriever`（所以既能检索也能被索引），带 `@beta(message="Added in 0.2.29. The abstraction is subject to change.")`。三个操作对：`upsert`/`aupsert`、`delete`/`adelete`、`get`/`aget`。

它与 `RecordManager` 的分工是清楚的：`RecordManager` 记「什么被索引过」，`DocumentIndex` 是「被索引的那个东西」的一种可选后端形态（文档型的索引，不一定是向量库）。所以 `indexing/api.py` 才能同时接受 `VectorStore` 与 `DocumentIndex` 作为写入目标。

`DocumentIndex` 的三条 doc 约定也很讲究：

- `get` 「**不应**在某些 ID 找不到时抛异常」，且「使用者不应假设返回文档的顺序与输入 ID 的顺序一致——应当依赖返回文档里的 ID 字段」。所以这里的位置对应是**显式放弃**的：答案本身就是无序的，调用方必须自己按 ID 对齐。
- `delete` 「不带任何参数调用应当抛 `ValueError`」——所以「删全部」必须是显式意图（例如 `delete_all=True` 这样的 kwarg），不能靠「忘传参数」意外达成。
- `upsert` 「若给了 ID 且内容已存在，应当用新数据更新；若不存在，应当添加」。

所以这套接口在设计上是**显式优于隐式**的：任何「删全部」的破坏性动作都必须显式表达，任何「按位置对齐」的假设都必须换成按 ID 对齐。

## 关键取舍

**源码事实：** `_Record` 只存 `group_id` 与 `updated_at`，不存内容、不存哈希的明文。哈希是「键」本身。

**源码事实：** `get_time` 的 docstring 明确要求「从服务端取时间以确保单调时钟，否则清理旧文档时可能丢数据」。

**源码事实：** `update` 有 `time_at_least` 参数，语义是「验证本地时间 >= 调用方给的时间」，不满足报错。抽象层 doc 说这是为了防时钟漂移。

**源码事实：** 内存版把 `time_at_least` 检查放在**循环内**，所以同批次内不同 key 的 `updated_at` 可能不同。

**源码事实：** `list_keys` 的 `before`/`after` 是严格不等；四个过滤都用真假判定，所以 `0.0` 与 `0` 会被当成「没给」。

**源码事实：** `delete_keys` 对不存在的 key 静默跳过；`DeleteResponse` 的 doc 明写「删除不存在的 ID 不是失败」。

**源码事实：** `UpsertResponse` 的 doc 承认「有失败时响应定义不良」，并建议调用方显式传 ID。

**架构推论：** 「把状态搬出向量库」这个选择，是「支持任意后端」与「获得一致性」之间的直接交换。把记录管理外置之后，任何只支持按 id 增删的后端都能用——这是覆盖面的巨大扩张。代价是两处写入不再有事务边界，所以 doc 里那条「写记录管理器成功但写向量库失败」的例子不是假想，而是这个架构的固有失败模式。任何增量索引系统都会面对同一道题：状态放外面（通用但不一致），还是放里面（一致但不通用）。

**架构推论：** 「时间戳必须由簿记所在机器给出」这条约束，本质是**让两个独立演进的时钟退化成同一个**。它看起来是技术细节，实际是这套设计的正确性支点：整个「哪些该删」的判定都建立在「写入时间 < 清理基准」这个序关系上，而这个序关系只有在同一时钟下才成立。这也解释了为什么 `time_at_least` 是一个**校验**参数而不是**赋值**参数——它给实现方一次机会去发现「你的时钟和我的不一致」。

**架构推论：** `UpsertResponse` 那条「部分失败即 ill defined」的文档，是一种很成熟的接口态度：承认位置对应的能力有边界，并把边界写在契约里。多数批量接口会含糊地暗示「响应对应于输入」，于是调用方在部分失败时按位置取值，拿到错误的结果却不会报错。写清失效条件比补一个 `id → 结果` 的映射更省，因为前者只需要文档，后者需要所有实现都改。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/indexing/base.py` | `RecordManager` | 记录管理器抽象与两条约束的文档 |
| `libs/core/langchain_core/indexing/base.py` | `RecordManager.get_time` | 要求服务端时钟、否则清理会丢数据 |
| `libs/core/langchain_core/indexing/base.py` | `RecordManager.update` | 批量 upsert，含 `time_at_least` 时钟校验 |
| `libs/core/langchain_core/indexing/base.py` | `RecordManager.list_keys` | 按时间与分组筛选，供清理判定 |
| `libs/core/langchain_core/indexing/base.py` | `RecordManager.delete_keys` | 幂等删除，不存在的静默跳过 |
| `libs/core/langchain_core/indexing/base.py` | `InMemoryRecordManager` | 六对方法的无 I/O 实现，异步即同步套壳 |
| `libs/core/langchain_core/indexing/base.py` | `InMemoryRecordManager.update` | 位置对应校验与循环内的时钟校验 |
| `libs/core/langchain_core/indexing/base.py` | `InMemoryRecordManager.list_keys` | 严格不等边界与真假判定的过滤 |
| `libs/core/langchain_core/indexing/base.py` | `_Record` | 每条记录只有分组与更新时间两个字段 |
| `libs/core/langchain_core/indexing/base.py` | `UpsertResponse` | 承认部分失败时响应定义不良 |
| `libs/core/langchain_core/indexing/base.py` | `DeleteResponse` | 全可选字段，删除不存在的 ID 不算失败 |
| `libs/core/langchain_core/indexing/base.py` | `DocumentIndex` | beta 的文档型索引抽象，位置对应被显式放弃 |

## 参考

- [langchain 源码：indexing/base.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/indexing/base.py)
- [langchain API 参考：RecordManager](https://python.langchain.com/api_reference/core/indexing/langchain_core.indexing.base.RecordManager.html)
- [langchain 文档：如何索引文档并处理去重](https://python.langchain.com/docs/how_to/indexing/)

## 与主核 / AI 的关系

主核里最接近的是「回收站/墓碑表」与「垃圾回收的外部状态」：要判断一个对象「已经没人引用、可以回收」，靠的是引用计数或标记表——这份状态本身也有存储成本、也可能不一致。`RecordManager` 就是那份标记表的对应物，它的 `updated_at` 相当于「最近一次被引用的时间」，`list_keys(before=...)` 相当于「找出一段时间没被引用的」。

但有一处关键差别值得对比：主核里 GC 的状态与数据在**同一个存储、同一个事务**里，所以「标记」与「删除」不会各说各话。这里两者是分开的，于是 doc 里那条「写记录管理器成功但写向量库失败」就成了一个需要人承担的残留状态。**这也是这一页最值得带走的判断：增量索引的复杂度不来自算法，而来自它必须维护一份位于别处、且没有事务保护的状态。**

与 g01（索引 API）分工对照：g01 讲的是「怎么用这份簿记」（比对两侧差集、按 cleanup 模式决定清理范围），这一页讲的是「这份簿记长什么样、有哪些实现必须满足的性质」。理解这一点才能明白 g01 为什么反复强调「`source_id_key` 必须给」——那是让 `_Record` 里的 `group_id` 有值的前提。

与 goose 的 `RolloutCompressionTrigger` 那套相比：两处都在维护「冷度」判定，但 goose 用的是文件系统的 `modified` 时间（同一台机器、同一个文件系统），不需要额外的单调时钟约束。这里因为簿记可能住在外部数据库，就不得不把「时间从哪来」提到契约层。

对 AI 工程最实用的一条是「时间戳必须与簿记同源」。任何用「时间先后」判断「谁该过期」的系统，都要保证做判断的时间和被判断的时间来自同一个时钟。跨机器、跨容器、跨进程的部署里，这一点几乎总会有人写错，而症状是「偶尔误删刚写入的数据」——一种很难复现、很难归因的故障。把「请传时间给我、我来验证」做成接口参数（`time_at_least`），比让每个实现自己想办法要可靠得多。

另一条是 `UpsertResponse` 那条 doc 的态度。批量写接口在部分失败时的响应语义，几乎是所有框架都会含糊带过的地方。这里的做法是**承认它 ill defined 并给出规避方式**（显式传 ID），而不是继续暗示「响应对应输入」。给接口画清楚它的失效边界，比假装它总是可用更有价值。
