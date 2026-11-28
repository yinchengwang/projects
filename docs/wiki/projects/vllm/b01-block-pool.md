---
project: vllm
feature: block-pool
title: 块池与前缀哈希
page_type: feature
schema_version: 1
refs:
  - path: vllm/v1/core/block_pool.py
    symbols: [BlockPool, BlockHashToBlockMap]
  - path: vllm/v1/core/kv_cache_utils.py
    symbols: [KVCacheBlock, BlockHashListWithBlockSize, get_block_hash]
---

# 块池与前缀哈希

## 一句话职责

`BlockPool` 是 GPU 上那几万块 KV cache 的**物理分配器**：它同时维护一条空闲链表、一张「内容哈希 → 块」的索引表，以及每个块的引用计数，从而让「不同请求共享同一段前缀」这件事在物理层成立。

## 机制是什么

KV cache 的本质是一大片固定大小的显存，按「块」切分。要让多个请求共享前缀，需要三层东西对上：

| 层 | 载体 | 回答的问题 |
|---|---|---|
| 数据 | `KVCacheBlock` | 这块显存现在归谁、内容是什么 |
| 索引 | `BlockHashToBlockMap` | 这段内容在不在池里 |
| 分配 | `FreeKVCacheBlockQueue` | 该给新请求哪块 |

**「哈希是内容哈希链，不是地址哈希」是这套设计的核心。** 每个块的哈希由「父块哈希 + 本块 token + 额外键」算出（`hash_block_tokens`），于是**哈希相同等价于「从序列开头到这里的全部内容都相同」**。这个链式结构让前缀匹配退化成「逐个块比哈希」，不需要逐 token 比较。

而块在池里有三种身份，由两个字段区分：

| 状态 | `ref_cnt` | `block_hash` | 在空闲队列里 |
|---|---|---|---|
| 正在被请求使用 | > 0 | 可能有 | 否 |
| 已缓存可复用 | 0 | 有 | 是（作为淘汰候选） |
| 纯空闲 | 0 | `None` | 是 |

**「`ref_cnt` 为 0 但有哈希」是最微妙的状态**：这块内容已经没人需要，但它的哈希还留在索引里等着被命中。任何新请求命中它就会 `touch` 一次把 `ref_cnt` 抬回 1，从而把它从空闲队列里摘出来。若一直没命中，它会在队列头部被淘汰——淘汰动作要把它从哈希索引里删掉（`_maybe_evict_cached_block`）。

## 怎么实现的

### 块的元数据：故意不用对象引用来做链表

`KVCacheBlock` 是 `@dataclass(slots=True)`，字段分三组：身份（`block_id`）、生命周期（`ref_cnt`、`is_null`、`pool`）、哈希（`block_hash`、`block_hash_num_tokens`），加上链表指针（`prev_free_block`、`next_free_block`）。

三处设计值得看：

**一，`pool` 是 `weakref.ref`。** 注释写着「Deferred frees may contain blocks from multiple pools. Weak to avoid ref cycles.」——延迟释放的块可能来自不同的池，若持强引用会形成「池 ↔ 块」循环。`BlockPool.__init__` 里也是先建 `self._weak_self = weakref.ref(self)` 再传给每个块。

**二，`__repr__` 里打印的是 `block_id` 而不是块对象。** 注释直说原因：「avoid calling `__repr__` on KVCacheBlock object recursively」。链表的环状引用一旦被 `repr` 递归展开就是无限循环。

**三，`set_block_hash` 带一个强断言。** 它要求 `block_hash is None and block_hash_num_tokens is None`，消息是「The block already has a hash. This should not happen.」。**这个断言把「哈希只能设一次」变成不可违反的规则**——唯一的合法例外（部分块升满块）在 `cache_full_blocks` 里显式处理为「先删旧哈希再插新哈希」。

`block_hash_num_tokens` 是「这个哈希覆盖了多少前缀 token」。满块时等于块边界，**部分块可以停在一个块的中间**——这是支持「前缀命中到块内部」的前提。

### BlockHashToBlockMap：一个哈希对应多个块

这张表的接口只有五个方法，但内部用了两个字典：

```python
self._cache: dict[BlockHashWithGroupId, int] = gc_untrack({})
self._dup_ids: dict[BlockHashWithGroupId, dict[int, None]] = {}
```

`_cache` 存「键 → 一个 block_id」，`_dup_ids` 存「键 → 多个 block_id」。**分成两个字典是为了让 `_cache` 里不含容器**——注释说明 `gc_untrack` 的收益前提是「字典里只有原子值」，Python 3.14+ 不再自动 untrack 容器字典，所以要把嵌套容器挪出去。

而「一个哈希对应多个块」这件事本身是被迫的，docstring 里的 NOTE 讲清了原因：

> We currently don't de-duplicate the blocks in the cache, meaning that if a block becomes full and is cached, we don't check if there is already an identical block in the cache. This is because we want to make sure the allocated block IDs won't change so that block tables are append-only.

**不去重是为了保住「块表只追加」这条性质。** 如果发现重复就把已分配的块换掉，块表就得改写；而块表被 CUDA graph、注意力元数据等多处引用，改写代价极高。**宁可让同一个哈希对应多个物理块，也不动已分配的块 ID。**

`insert` 的实现用一个巧妙的 `setdefault`：

```python
block_id = self._cache.setdefault(key, block.block_id)
if block_id != block.block_id:
    self._dup_ids.setdefault(key, {block_id: None})[block.block_id] = None
```

第一次插入时 `setdefault` 生效、返回的就是自己，不建 `_dup_ids`；第二次起才把「原有那个」与「新的这个」都记进 `_dup_ids`。**所以 `_dup_ids` 只在真出现重复时才存在**，常见的无重复路径完全不多付代价。

`pop` 要在两种结构里分别处理，且删掉一个后要从 `_dup_ids` 里挑另一个补进 `_cache`（`self._cache[key] = next(iter(dup_ids))`），最后在只剩一个时把 `_dup_ids` 条目删掉——**否则会残留「长度 1 的 dup 记录」，让 `_cache` 与 `_dup_ids` 状态不一致**。

`get_one_block(key)` 返回「任意一个」，注释明写「If there are duplicated blocks, we return the first block in the cache」。**不保证返回最新或最热的那个**——因为使命中就够了，选哪个是性能细节而非正确性问题。

### 哈希键的拼接：`BlockHashWithGroupId`

`get_block_hash(key)` 的实现是 `BlockHash(key[:-4])`，`get_group_id(key)` 是 `int.from_bytes(key[-4:], "big")`。

也就是说**键 = 哈希字节 + 4 字节大端组 ID**，拼在同一个 bytes 里。这样做的好处是索引只需要一层字典查找（不用 `dict[hash][group]` 两层）。`cache_full_blocks` 里有一处为效率做的对应优化：

```python
group_suffix = make_block_hash_with_group_id(BlockHash(b""), kv_cache_group_id)
...
block_hash_with_group_id = BlockHashWithGroupId(block_hash + group_suffix)
```

注释写着「Appended per block below: much cheaper than packing the group id each time」——**把组 ID 的后缀预先算好，循环里只做字节拼接**。这类微优化在每步都要跑的路径上是有意义的。

### 分配与释放：三个语义不同的入口

**`get_new_blocks(num_blocks)` 是唯一真正分配的地方。** 先检查够不够（不够抛 `ValueError`），从空闲队列头部弹出 N 个，然后**按四种组合内联展开**：

```python
if self.enable_caching:
    if self.metrics_collector:
        ...  # 淘汰 + ref_cnt=1 + 上报
    else:
        for block in ret:
            # Most blocks were never cached, so skip the call when possible.
            if block.block_hash is not None:
                self._maybe_evict_cached_block(block)
            assert block.ref_cnt == 0
            block.ref_cnt = 1
else:
    ...  # 无缓存，只需 ref_cnt=1（可选上报）
```

注释「In order to only iterate the list once, we duplicated code a bit」——**为了不跑两遍循环，把四种情况各写一遍**。而且无 metrics 且开缓存的那一支还加了一个 `if block.block_hash is not None` 的短路：注释说「Most blocks were never cached」，所以多数块可以跳过一次函数调用。

`assert block.ref_cnt == 0` 在每一支里都有：**从空闲队列弹出的块必然没人用**。这是一个几乎免费的不变量检查。

**`unpin_blocks(blocks, on_reuse)` 是「延迟释放」。** 它减引用计数，但即使减到 0 也不立即让块可被覆盖，而是登记一个 `on_reuse` 回调：

```python
block.ref_cnt -= 1
self._reuse_watchers[block.block_id] = on_reuse
if block.ref_cnt == 0:
    released.append(block)
self.free_block_queue.append_n(released)
```

docstring 说明了用途：「The blocks become last-resort eviction candidates regardless of prefix caching: they join the tail of the free queue and count as free. `on_reuse` fires when `get_new_blocks` hands a block out (or the cache is reset), so the caller can stop reading it.」

**这是一个「异步读取者」的设计**：释放方说「我不用了」，但仍有人可能在读（例如异步保存到磁盘、或另一个进程在 DMA），所以块进入「可以回收但最好晚点回收」的队列尾部，并在真被重用时通过回调通知读者停止。`get_new_blocks` 里的 `_notify_reuse(ret)` 就是触发点。

**`touch(blocks)` 是「命中复用」。** 对每个块：跳过 null 块；`ref_cnt == 0` 就从空闲队列里摘掉；`ref_cnt += 1`；上报访问。

**「`ref_cnt == 0` 才摘队列」这句注释解释了一个反直觉的点**：`ref_cnt > 0` 的块本来就不在空闲队列里，所以只有从 0 变 1 时才需要摘。

**`deprioritize_blocks(blocks)` 是队列的第三档。** docstring：「Deprioritize free blocks so they are evicted after uncached blocks but before normal LRU-cached blocks, in FIFO order.」

于是空闲队列其实有三个区段，淘汰顺序从先到后是：**未缓存的块 → 被降级的块 → 正常 LRU 缓存块**。实现靠 `insert_before_cached(moved)`——把块插到「缓存块区段的头部之前」。`_cached_head` 这个字段的存在就是为了让这个 O(1) 插入成为可能（注释：「Maintained so middle-tier blocks can be spliced before it in O(1)」）。

### 前缀哈希在块大小不一致时的降级

`BlockHashListWithBlockSize` 处理一个具体问题：**不同 KV cache 组的块大小可能不同**，而哈希只按最小的 `hash_block_size` 算。

它的解法极简：**取目标块内的最后一个细粒度哈希。** 因为每个哈希已经链式覆盖了整个前缀，所以「第 k 个 `hash_block_size` 边界处的哈希」恰好就是「覆盖到那个位置的任意更大块」的索引。

docstring 里画了表格说明（块大小 16 转 32）：16 的哈希序列 `A B C D`，转成 32 则取 `B`、`D`——正是每两个取后一个。实现就是 `self.block_hashes[(idx + 1) * self.scale_factor - 1]`。

`__init__` 里有一句 `assert target_block_size % hash_block_size == 0`——**只支持整数倍放大**。缩小的情形不支持，因为信息已经丢了。

`__getitem__` 同时支持 `int` 与 `slice`，且 slice 走 `range(start, stop, step)` 逐个取值——**这个类要被当成列表用**，所以索引协议必须完整（`__len__` / `__getitem__` / `__iter__` 都实现了）。

### 哈希种子的安全考量

`NONE_HASH` 是「第一个块的父哈希」，也就是哈希链的起点。它的种子选择揭示了一处安全判断：

- **加密哈希**（如 SHA-256）用固定默认种子 `"vllm-none-hash"`，于是**不同进程对相同内容算出相同哈希**，因此可以跨节点共享前缀缓存。
- **非加密哈希**（`xxhash` 等）保留进程级随机种子，因为「可预测的种子会让攻击者能离线预计算碰撞块」（注释引用了 issue 12621）。

注释还特意点明：固定种子**不削弱 SHA-256 的抗碰撞性**（那与种子是否保密无关），而「有意隔离缓存」用 `cache_salt` 这个独立机制。

**这段注释本身就是一个完整的设计说明**：它回答了「为什么不统一种子」「为什么固定种子不危险」「隔离该用什么手段」三个问题。

### 两个特殊块：null 块与部分块

**`null_block` 是块 ID 0 的占位块。** 构造时从空闲队列里 `popleft()` 出来并置 `is_null = True`。注释警告：「The ref_cnt of null_block is not maintained, needs special care to avoid freeing it.」

它的用途是「某处需要一块但不需要真实内容」——例如滑窗注意力的稀疏块、被 mask 掉的块。`touch` 里对 `is_null` 直接 `continue`，`is_block_writable` 里要求 `not block.is_null`。**在共享池里放一个不参与引用计数的特殊成员，是一种容易出错但省事的做法**——所有涉及计数器的地方都必须记得跳过它。

**`is_block_writable(block)` 是一条三元判断**：

```python
return not block.is_null and block.ref_cnt == 1 and block.block_hash is None
```

三个条件分别排除：占位块、有别的使用者、内容已被缓存（被缓存意味着别人可能命中它，改写会污染缓存）。**「可以写」被定义成「我独占且它还没有公开身份」，这是 copy-on-write 的判据。**

## 关键取舍

**源码事实：** 前缀哈希是链式的（父哈希 + 本块 token + 额外键），因此哈希相等等价于前缀内容相等。

**源码事实：** 哈希索引不去重——同一个哈希可以对应多个物理块，为的是保住「块表只追加」。

**源码事实：** `ref_cnt == 0` 但仍有 `block_hash` 的块是「淘汰候选但可被命中」态，命中时 `touch` 把它摘出空闲队列。

**源码事实：** `unpin_blocks` 不立即让块可覆盖，而是登记 `on_reuse` 回调并在块真被分配出去时通知。

**源码事实：** `get_new_blocks` 为「单次遍历」把四种配置组合的循环内联复制了一遍。

**源码事实：** 空闲队列有三档淘汰顺序：未缓存 → 降级 → 正常 LRU 缓存；`_cached_head` 用于 O(1) 中间插入。

**源码事实：** 块大小不一致时取「目标块内最后一个细粒度哈希」，且只支持整数倍放大。

**源码事实：** `NONE_HASH` 的种子对加密哈希固定、对非加密哈希随机，注释解释了两者的安全差异。

**架构推论：** 「链式内容哈希」是这个设计的灵魂。它把「前缀是否相同」这个本来需要逐 token 比较的问题，变成了一次字典查找；代价是**哈希必须按顺序算、不能并行**（第 n 个块的哈希依赖第 n-1 个）。而 `hash_block_tokens` 上挂的 LRU cache 正是为缓解这个串行依赖。

**架构推论：** 「不去重以保块表只追加」是一条**用空间换不可变性**的取舍。去重能省显存，但会让块 ID 变化、块表失效。这里选了保守方向，代价是同样的前缀可能占多份物理块。**当某个数据结构被大量外部引用时，「不改它」比「省一份内存」更重要。**

**架构推论：** null 块这种「特殊成员混在普通集合里」的做法，会让每个操作都多一个 `is_null` 分支。它省下了一次「用 Optional 还是空块」的设计决策，代价是**所有计数逻辑都必须记得跳过它**——`set_block_hash` 的断言、`touch` 的 continue、`is_block_writable` 的否定条件都是这个代价的账单。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/v1/core/block_pool.py` | `BlockPool` | 块的分配、释放、缓存与淘汰 |
| `vllm/v1/core/block_pool.py` | `BlockPool.__init__` | 建块、建空闲队列、留出 null 块 |
| `vllm/v1/core/block_pool.py` | `BlockHashToBlockMap` | 内容哈希到物理块的索引，支持一哈希多块 |
| `vllm/v1/core/block_pool.py` | `BlockHashToBlockMap.insert` | `setdefault` 判别，仅在重复时建 dup 表 |
| `vllm/v1/core/block_pool.py` | `BlockPool.get_new_blocks` | 唯一分配入口，四种组合内联 |
| `vllm/v1/core/block_pool.py` | `BlockPool.unpin_blocks` | 延迟释放 + 复用回调 |
| `vllm/v1/core/block_pool.py` | `BlockPool.touch` | 命中复用，`ref_cnt` 从 0 变 1 时摘出空闲队列 |
| `vllm/v1/core/block_pool.py` | `BlockPool.deprioritize_blocks` | 中间档淘汰优先级 |
| `vllm/v1/core/block_pool.py` | `BlockPool._maybe_evict_cached_block` | 淘汰时清哈希并删索引 |
| `vllm/v1/core/block_pool.py` | `BlockPool.cache_full_blocks` | 满块入索引，处理部分块升满块 |
| `vllm/v1/core/block_pool.py` | `BlockPool.cache_partial_block` | 部分块哈希（前缀命中到块内） |
| `vllm/v1/core/block_pool.py` | `BlockPool.is_block_writable` | 独占且未公开才可写 |
| `vllm/v1/core/kv_cache_utils.py` | `KVCacheBlock` | 块元数据与链表指针 |
| `vllm/v1/core/kv_cache_utils.py` | `KVCacheBlock.set_block_hash` | 哈希只能设一次的强断言 |
| `vllm/v1/core/kv_cache_utils.py` | `get_block_hash` | 从复合键里剥出哈希字节 |
| `vllm/v1/core/kv_cache_utils.py` | `BlockHashListWithBlockSize` | 块大小不同时的哈希粒度转换 |
| `vllm/v1/core/kv_cache_utils.py` | `hash_block_tokens` | 链式哈希计算 |
| `vllm/v1/core/kv_cache_utils.py` | `FreeKVCacheBlockQueue` | 双向链表空闲队列，支持 O(1) 中间摘除 |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：v1/core/block_pool.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/core/block_pool.py)
- [vllm 文档：自动前缀缓存](https://docs.vllm.ai/en/latest/features/automatic_prefix_caching.html)
- [vLLM 论文：PagedAttention（Kwon et al., 2023）](https://arxiv.org/abs/2309.06180)

## 与主核 / AI 的关系

块池与主核里「缓冲池 + 页表」的分工几乎逐层对应：都是「一大片固定大小的存储 + 一个空闲链表 + 一张从身份到位置的索引」。差别在**身份的来源**：主核的索引键是物理地址（页号），vllm 的索引键是**内容哈希**。

这个差别决定了复用的含义。主核的复用是「同一个页被多个事务读」；vllm 的复用是「不同请求恰好有相同前缀」。**后者是跨对象的，所以它需要一个内容标识而不是位置标识。** 主核若要做「相同内容去重」，就要引入 vllm 这一套链式哈希——这正是多模态存储的一个自然延伸方向。

`unpin_blocks` 的「延迟释放 + 复用回调」与主核里「脏页必须刷完才能复用」是同一类问题：**块已经没人需要，但还有人在读**。主核靠「刷盘完成」这个状态位控制，vllm 靠一个回调登记表。前者是同步的、由引擎自己掌握；后者是异步的、由调用方决定读多久。**当读的持续时间不可控时，只能把「何时安全」的责任交出去。**

「不去重以保块表只追加」这条取舍值得与主核的「不做页内更新」对照。许多数据库引擎（包括主核的某些路径）选择 append-only，理由同样是「已发布的位置不能变」。**在有多方引用同一份定位信息时，不可变性的价值高于空间利用率。**

对 agent / RAG 工程的实际含义有四条。其一，共享系统提示词能直接命中前缀缓存，但**要求 token 序列完全一致**——多一个空格、少一个换行都会让哈希链断开，从第一个不同的块起全部失效。其二，命中的粒度是块，所以「只在块边界之前相同」的部分不会被复用。其三，缓存是 LRU + 三档优先级，长会话会挤掉别人的缓存，且被降级的块（如异步卸载中的）淘汰得更早。其四，null 块不参与引用计数，任何自己实现的池操作都必须显式跳过它，否则会把占位块放回空闲队列。
