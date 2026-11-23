---
project: redis
feature: quicklist
page_type: feature
schema_version: 1
refs:
  - path: src/quicklist.c
    symbols: [quicklistPush, quicklistCreate, quicklistDelIndex, quicklistReplaceAtIndex]
---

# quicklist 分片链表

## 一句话职责

quicklist 把一长串元素切成若干个 listpack 分片、用双向链表串起来：既拿到了 listpack 的紧凑编码，又拿到了链表式的 O(1) 两端插入与删除，还能在分片之间做合并来控制碎片。

## 机制是什么

**结构是「链表 + 分片」两层。** quicklist 本身只有 `head` / `tail` 两个指针、`len`（分片数）、`count`（元素总数）、`alloc_size`（占用字节）、`fill`、`compress` 几个字段。每个分片是一个 `quicklistNode`，持有 `entry` 指针、`sz`（字节数）、`count`（元素数）、`prev` / `next`、`container` 与 `encoding`。

分片有两种**容器**：`QUICKLIST_NODE_CONTAINER_PACKED`（内容是一整个 listpack，元素可以是数字或字符串）与 `QUICKLIST_NODE_CONTAINER_PLAIN`（内容是单个大对象的裸字节，元素数恒为 1）。**编码**有两种：`QUICKLIST_NODE_ENCODING_RAW`（未压缩）与 `QUICKLIST_NODE_ENCODING_LZF`（LZF 压缩过）。

**`fill` 决定分片何时切分。** `fill >= 0` 时它是**元素个数**上限；`fill < 0` 时它是**字节上限**，具体数值查 `optimization_level[]`——这个数组是 `{4096, 8192, 16384, 32768, 65536}`，注释写明最大只能是 64k，原因是「the largest possible limit is 64k so that the 16-bit count field of the listpack can't overflow」。`fill = -1` 对应 4KB、`-2` 对应 8KB、`-3` 对应 16KB、`-4` 对应 32KB、`-5` 对应 64KB。

**大元素单独成片。** `SIZE_SAFETY_LIMIT` 是 8192：超过这个尺寸的元素不塞进 listpack，而是独占一个 PLAIN 分片。

**压缩深度决定压哪些分片。** `compress` 是「距两端的深度」：深度为 0 表示完全不压缩；深度为 1 表示只有中间的（既不是头也不是尾的）分片才压缩；深度越大越靠近两端也会被压。头尾分片**永远不压缩**——源码里到处是 `assert(node->prev && node->next)` 这样的断言来保证这一点。

本页只讲分片链表这一层。分片内部的紧凑编码是 e05；列表命令的语义是 f02。

## 怎么实现的

**创建：`quicklistCreate()`。** 用 `zmalloc_usable` 分配结构体并同时拿到实际可用字节数；`head = tail = NULL`、`len = 0`、`count = 0`、`alloc_size = 实际可用字节`、`compress = 0`、`fill = -2`、`bookmark_count = 0`。`quicklistNew(fill, compress)` 是「创建 + 设参数」的组合。

**参数钳制。** `quicklistSetFill` 把 fill 钳在 `FILL_MAX`（`(1 << (QL_FILL_BITS-1))-1`）与 -5 之间——上界受 listpack 的 16 位元素计数限制，下界 -5 对应 `optimization_level` 的最后一级 64KB。`quicklistSetCompressDepth` 把 compress 钳在 0 与 `COMPRESS_MAX`（`(1 << QL_COMP_BITS)-1`）之间，负数一律归 0。

**上限计算：`quicklistNodeLimit(fill, &size, &count)`。** 默认 `size = SIZE_MAX`、`count = UINT_MAX`；`fill >= 0` 时只给 `count` 赋值（`fill == 0` 时特殊处理成 1，注释写「Ensure that one node have at least one entry」），`fill < 0` 时只给 `size` 赋值成 `quicklistNodeNegFillLimit(fill)`。

`quicklistNodeNegFillLimit(fill)` 用 `offset = (-fill) - 1` 去索引 `optimization_level`，超出数组范围就钳到最后一级。

**能不能塞进去：`_quicklistNodeAllowInsert`。** 三道否决：节点为空、节点是 PLAIN、或者这个元素本身是大元素——都直接返回 0。否则估算 `new_sz = node->sz + sz + SIZE_ESTIMATE_OVERHEAD`（`SIZE_ESTIMATE_OVERHEAD` 是 8，源码注释解释这是**故意高估**：「We prefer an overestimation, which would at worse lead to a few bytes below the lowest limit of 4k」），再交给 `quicklistNodeExceedsLimit` 判断。

`quicklistNodeExceedsLimit` 的判定顺序值得注意：**先看字节上限，再看元素数上限**。如果这次是字节上限模式，就只比字节；如果是元素数模式，则先检查字节有没有突破 `SIZE_SAFETY_LIMIT`（突破也算超限，源码注释说「which is safe, see comments next to optimization_level and SIZE_SAFETY_LIMIT」），再比元素数。

**能不能合并：`_quicklistNodeAllowMerge(a, b, fill)`。** PLAIN 节点不参与合并；估算的合并后大小是 `a->sz + b->sz - 7`——源码注释解释这个 7：「approximate merged listpack size (- 7 to remove one listpack header/trailer, see LP_HDR_SIZE and LP_EOF)」，即两个 listpack 合并后省掉一份头部与结束标记。

**写入：`quicklistPush(quicklist, value, sz, where)`。** 它先做两个断言：「The head and tail should never be compressed (we don't attempt to decompress them)」——`quicklist->head->encoding != QUICKLIST_NODE_ENCODING_LZF`、`quicklist->tail->encoding != QUICKLIST_NODE_ENCODING_LZF`。然后按 `where` 分派到 `quicklistPushHead` / `quicklistPushTail`。

`quicklistPushTail` 的三条路径：

1. **大元素**：`__quicklistInsertPlainNode(quicklist, quicklist->tail, value, sz, 1)`——插一个 PLAIN 分片到尾部，返回 1。
2. **尾分片还能装**：`quicklist->tail->entry = lpAppend(quicklist->tail->entry, value, sz)`，然后 `quicklistNodeUpdateSz` 重算分片字节数、`quicklistUpdateAllocSize` 更新总占用。
3. **尾分片装不下**：新建一个分片，`node->entry = lpAppend(lpNew(0), value, sz)`，再 `_quicklistInsertNodeAfter` 挂到链表尾部。

两条路径最后都会 `quicklist->count++` 与 `quicklist->tail->count++`，返回值是「新分片是否被创建」（对比 `orig_tail != quicklist->tail`）。

**删除：`quicklistDelIndex(quicklist, node, p)`。** 函数的注释先声明了一条前提：「quicklistDelIndex() *requires* uncompressed nodes because you already had to get *p from an uncompressed node somewhere.」然后：

- 若是 PLAIN 分片，直接 `__quicklistDelNode` 删掉整个分片，返回 1。
- 否则 `node->entry = lpDelete(node->entry, *p, p)`（**`p` 是进出参**——`lpDelete` 会把被删元素的右邻位置写回来），更新分片大小与总占用，`node->count--`；若分片被删空则 `__quicklistDelNode` 删掉该分片。
- 最后 `quicklist->count--`，返回值表示「整个分片是否被删除」。

调用方 `quicklistDelEntry` 拿到这个返回值后修正迭代器：注释写明「after delete, the zi is now invalid for any future usage」，并把 `iter->current` 指向被删分片的前驱或后继、`iter->offset` 相应调整；源码还留下了一段说明为什么某些情况下 offset 不需要动：「[1, 2, 3] => delete offset 1 => [1, 3]: next element still offset 1」。

**替换：`quicklistReplaceAtIndex(quicklist, index, data, sz)`。** 它先 `quicklistInitIteratorEntryAtIdx` 定位到目标下标，定位成功就调 `quicklistReplaceEntry` 并返回 1，失败返回 0。

`quicklistReplaceEntry` 的分支是本页最复杂的部分：

1. **常规路径**：分片不是 PLAIN、新元素不是大元素、且 `lpReplace` 成功——直接替换 listpack 里的元素，更新分片大小与总占用，然后 `quicklistCompress` 重新评估压缩。
2. **PLAIN 分片且新元素仍是大元素**：`zfree` 旧数据、`zmalloc(sz)` 新分配、`memcpy` 进去、更新大小。
3. **PLAIN 分片但新元素变小**：走 `quicklistInsertAfter` 插入新元素，再 `__quicklistDelNode` 删掉整个旧 PLAIN 分片。
4. **节点已满或新元素是大元素**：先把 `node->dont_compress = 1`（注释「Prevent compression in __quicklistInsertNode()」），若目标不在分片末尾则用 `_quicklistSplitNode` **拆分**分片；建一个新分片插到原分片之后，若拆过则再把拆出的那半插到新分片之后，`quicklist->count++`；最后删掉被替换的旧元素，再 `_quicklistMergeNodes` **合并**回去。源码注释解释了为什么合并后还要再调 `quicklistCompress`：「We can't know if the current node and its sibling nodes are correctly compressed, and we don't know if they are within the range of compress depth」。

无论走哪条分支，最后都 `resetIterator(iter)`——注释写明「we reset iterator to forbid use of iterator after insert」。

**压缩：`__quicklistCompressNode`。** 三道门槛：节点带 `dont_compress` 标志就跳过；`assert(node->prev && node->next)` 保证既不是头也不是尾；`node->sz < MIN_COMPRESS_BYTES`（48 字节）不压——注释写「Don't bother compressing small values」。压缩用 LZF，若 `lzf_compress` 返回 0（数据不可压缩）或压缩后大小满足 `lzf->sz + MIN_COMPRESS_IMPROVE >= node->sz`（`MIN_COMPRESS_IMPROVE` 是 8）就放弃——**压缩收益不到 8 字节就不值得**。

压缩深度的控制在 `__quicklistCompress` 里：从头尾各走 `compress` 步，深度内的节点不压，深度外的才压。函数开头有两道早退：`quicklist->len == 0` 直接返回；`!quicklistAllowsCompression(quicklist) || quicklist->len < (unsigned int)(quicklist->compress * 2)` 直接返回——注释解释：「If length is less than our compress depth (from both sides), we can't compress anything.」

**真实 caller。**

- **列表命令。** `src/t_list.c` 用 `quicklistNew(server.list_max_listpack_size, server.list_compress_depth)` 建对象；`LPUSH` / `RPUSH` 走 `quicklistPush(subject->ptr, buf, strlen(buf), pos)`；`LSET` 走 `quicklistReplaceAtIndex(ql, index, vstr, vlen)`。
- **配置项。** `src/config.c` 里 `list-max-listpack-size`（别名 `list-max-ziplist-size`，默认值 -2）与 `list-compress-depth`（默认值 0）决定 `fill` 与 `compress`。这两个配置项都是 `MODIFIABLE_CONFIG`，运行时可以改。
- **持久化。** `src/rdb.c` 用 `quicklistNew(server.list_max_listpack_size, ...)` 建列表，并把每个分片按容器类型分别存成 listpack 或 plain node。
- **对象层。** `src/object.c` 的 `createQuicklistObject(fill, compress)` 调 `quicklistNew` 并把编码标成 `OBJ_ENCODING_QUICKLIST`。

**退化与回退路径。**

- **压缩失败的回退是「保持原样」。** LZF 压缩失败或收益不足时 `__quicklistCompressNode` 返回 0，节点保持 `QUICKLIST_NODE_ENCODING_RAW`——数据无损，只是没省下空间。
- **解压失败是有专门返回码的异常路径。** `__quicklistDecompressNode` 里 `lzf_decompress2(...) == 0` 时注释写「Someone requested decompress, but we can't decompress. Not good.」，它释放临时缓冲并返回 0。
- **头尾不能压是硬约束。** `quicklistPush` 与 `__quicklistCompressNode` 都有断言保证头尾不是 LZF 编码；`__quicklistCompress` 开头还断言 `quicklist->head->recompress == 0 && quicklist->tail->recompress == 0`——**头尾永远保持解压状态**，代价是最新的数据不享受压缩收益。
- **删除要求分片已解压。** `quicklistDelIndex` 的注释明确要求传入的是未压缩分片，因为元素指针本来就只能从未压缩分片里取到。
- **分片数太少时压缩深度不生效。** `quicklist->len < compress * 2` 时整条链都不压缩——一个只有 1 个分片的 quicklist 无论 `compress` 设多大都不会被压缩。
- **元素计数溢出由 `fill` 钳制来防。** `quicklistSetFill` 的上界 `FILL_MAX` 就是为了让单分片的元素数不撞上 listpack 的 16 位计数上限；`optimization_level` 最大 64k 也是同一个理由。

## 关键取舍

**源码事实：** quicklist 是双向链表套 listpack 分片，两种容器（PACKED / PLAIN）、两种编码（RAW / LZF）。**源码事实：** `fill` 正值按元素数分片、负值按字节分片，字节档位是 4KB 到 64KB 五档。**源码事实：** 超过 8192 字节的元素独占一个 PLAIN 分片。**源码事实：** 头尾分片永不压缩，压缩深度是「距两端的距离」。**源码事实：** 小于 48 字节不压，压缩收益不足 8 字节放弃。**源码事实：** 替换操作可能触发分片拆分与合并。**源码事实：** 合并时估算大小要减 7（一份头部加结束标记）。

**架构推论：** 把「链表」和「紧凑编码」两层叠在一起，用分片切分同时拿到 O(1) 两端操作和低内存开销，代价是**同一份逻辑要在两个层次上各实现一遍**——插入要判断「塞进现有分片还是新开分片」，删除要判断「分片是否被删空」，替换要处理「拆分 + 合并」这一整套。**架构推论：** 「头尾永不压缩」是一条典型的**为常见路径优化**的取舍：头部是最新写入的数据，最可能被立刻读到，解压一次的成本远高于省下的空间。**架构推论：** 用 LZF 这种轻量压缩而不是 zlib，是因为列表元素多是短字符串、需要极快的解压速度；代价是压缩率低，且对不可压缩数据要白付一次扫描成本——所以才有「收益不足 8 字节就放弃」这条阈值。**架构推论：** 「允许 split + merge」让分片尺寸能自我修复，避免长期增删之后留下大量半满的小分片；代价是替换路径的复杂度显著上升，`quicklistReplaceEntry` 有四条分支、跨越了两个层面的操作。主核是多写者引擎，**quicklist 的整套逻辑不可照搬**：它假设整条链在单线程下被独占修改，而主核的列表段必然是并发写的；主核更适合「不可变段 + 段目录」的形态，让分段成为写入时确定的事实，而不是写入后自我修复的结果。AI 侧若把对话历史或上下文块存成 Redis 列表，用 `list-max-listpack-size` 调大分片会省内存但增加复制与 RDB 载入时的搬移量；更实际的风险是 `list-compress-depth` 默认是 0，也就是**默认完全不压缩**——上下文文本通常有相当的可压缩性，调成 1 通常能省下可观空间，代价是读时多一次 LZF 解压。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/quicklist.c` | `quicklistCreate` | 创建空 quicklist，设定默认的 fill、压缩深度与占用记账起点 |
| `src/quicklist.c` | `quicklistPush` | 两端写入入口：校验头尾未压缩后分派到头插 / 尾插 |
| `src/quicklist.c` | `quicklistDelIndex` | 删除指定分片内的指定元素，返回该分片是否被整体删除 |
| `src/quicklist.c` | `quicklistReplaceAtIndex` | 按下标替换元素，内部可能触发分片拆分与合并 |

## 参考

- [Redis 源码：quicklist.c](https://github.com/redis/redis/blob/unstable/src/quicklist.c)
- [Redis 源码：quicklist.h](https://github.com/redis/redis/blob/unstable/src/quicklist.h)
- [Redis 文档：数据类型（List）](https://redis.io/docs/latest/develop/data-types/lists/)

## 与主核 / AI 的关系

quicklist 对主核最有价值的**不是**它的链表结构，而是「**分片 + 自我修复**」这个组合：主核的多模态记录常常是一段连续的消息或文档片段，如果按固定大小切段并允许段在删改后自动拆分与合并，就能同时避免「一个巨大段导致任何修改都要搬动全部数据」和「大量小段导致每个元素一个指针开销」这两个极端。这条思路可以移植到主核的段式存储上，但**必须重新设计并发语义**：Redis 的 split / merge 假设整条链没有并发写者，主核的段合并需要行锁或版本号保护。

另一条是「**收益不足就不压缩**」这条阈值纪律：主核若对向量块或大文本块做压缩，同样需要一条「压缩后收益低于 X 字节就放弃」的规则，否则在不可压缩数据上会白付 CPU 却只换来更大体积。

不可照搬的是**头尾永不压缩**这条优化——它建立在「头尾是热点」的假设上，主核的段访问分布未必如此（AI 侧常见的模式是随机访问中间的历史片段），按访问热度决定压缩优先级需要额外的采样统计，成本远高于 Redis 这条静态规则。AI 侧还要注意：Redis 列表的**元素必须是字符串或数字**，若把向量直接塞进列表，只能以二进制字符串形式存放，内存统计与压缩行为都会与预期不符。
