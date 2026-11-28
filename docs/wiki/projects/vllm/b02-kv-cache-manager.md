---
project: vllm
feature: kv-cache-manager
title: 单类型 KV cache 管理器
page_type: feature
schema_version: 1
refs:
  - path: vllm/v1/core/single_type_kv_cache_manager.py
    symbols: [SingleTypeKVCacheManager, FullAttentionManager, SlidingWindowManager]
  - path: vllm/v1/core/single_type_kv_cache_manager.py
    symbols: [RSWAManager, MambaManager, CircularBufferManager]
---

# 单类型 KV cache 管理器

## 一句话职责

一个 KV cache 组对应一个管理器实例：它决定这个组「怎么查前缀命中、怎么分配块、什么块可以提前丢掉」，而**每种注意力类型的答案都不一样**——全注意力要保住整段前缀，滑窗只保最后 N 个 token，线性注意力（Mamba）只保几个状态块，循环缓冲索性只占一块。

## 机制是什么

`SingleTypeKVCacheManager` 是抽象基类，但它抽象出来的不是「一个算法」，而是**一组策略钩子**。子类通过覆写静态方法影响全局决策，通过覆写实例方法影响单请求行为：

| 层级 | 方法（举例） | 谁在何时调用 |
|---|---|---|
| 类级策略 | `prefix_cacheable`、`max_admission_blocks`、`max_num_blocks_per_req` | 启动期算容量、每步调度前判准入 |
| 实例行为 | `find_longest_cache_hit`、`allocate_new_blocks`、`remove_skipped_blocks` | 每步、每请求 |

**「策略用 `@classmethod`、行为用实例方法」这个划分是有意义的**：容量规划发生在**还没有任何请求**的时候（启动期算每请求最多占多少块，据此决定池子大小），那时没有实例可用，只能问类；而前缀命中查询发生在具体请求上，需要知道该请求已分配了哪些块。

管理器的参数用 `Params` 这个**冻结 dataclass** 承载：

```python
@dataclass(frozen=True, slots=True)
class Params:
    """The manager's allocation parameters; managers without any inherit
    this empty set. Subclasses must stay frozen dataclasses so configs
    hash and compare by value."""
```

注释里的「must stay frozen … so configs hash and compare by value」是一条硬约束：**`ManagerConfig` 要靠值哈希与比较**，因为协调器需要判断「两个组的策略是否相同、能否共用块表」。可变的参数对象会让哈希失效。

派生出的三个具体管理器差别极大：

| 管理器 | 每请求块数 | 前缀缓存 | 关键机制 |
|---|---|---|---|
| `FullAttentionManager` | 随序列增长 | 支持，可细粒度 | 从首块起找最长连续命中 |
| `SlidingWindowManager` | 有上界（窗口大小） | 支持但需连续窗口 | 从右往左找连续窗口 |
| `RSWAManager` | 有上界（前缀+窗口） | 继承全注意力 | 挖掉中间「gap」块 |
| `MambaManager` | 常数级（2 + 投机块） | 支持（可选检查点） | 状态块，中间态被置空 |
| `CircularBufferManager` | 恒为 1 | 不支持 | 环形缓冲，自己寻址 |

## 怎么实现的

### 基类的三块共同骨架

**一，块大小被 DCP 放大。**

```python
self.dcp_world_size = dcp_world_size if kv_cache_group.dcp_sharded else 1
self.group_block_size = kv_cache_group.block_size
self.block_size = kv_cache_group.block_size * self.dcp_world_size
```

decode 上下文并行会把每个块切到多个 rank 上，于是**逻辑块大小变成物理块大小乘以 world size**。注释分别标注了「DCP 缩放前的组块大小」与「实际分配用的块大小」——两者同时保留，因为对外报告与对内分配用的是不同粒度。

而 `scheduler_block_size` 是**所有组块大小的最小公倍数**，作为调度粒度；它不一定是本组块大小的倍数关系反向成立（本组块大小是它的因数）。

**二，块表是「按请求的块列表」，且被显式排除在 GC 之外。**

```python
self.req_to_blocks: defaultdict[str, list[KVCacheBlock]] = defaultdict(untracked_list)
```

注释解释了为什么：

> Blocks hold no strong reference back to these lists or their owner, so the lists cannot be part of a cycle and are hidden from the cyclic GC, which would otherwise visit every block of every request.

**这里是「谁说没有循环引用，谁就得自己去 untrack」**。GC 无法自动判断「这个 list 里的块不会回指这个 list」，所以只能由作者断言并显式豁免。省下的是每轮 GC 遍历所有请求所有块的开销——在一个每步都要处理几百请求的系统里这是实打实的成本。

**三，三种「未命中」的返回约定。** 基类里几个方法都有「返回 `None` 表示不做这个判断」的约定：

- `get_num_blocks_to_reserve(...) -> int | None`：返回 `None` 表示「用基于分配的准入检查」。
- `max_admission_blocks(...) -> int | None`：`None` 表示「不限，请求会一直持有所有块直到结束」。
- `reachable_block_mask(...) -> list[bool] | None`：`None` 表示「不受对齐约束」。

**`None` 在这里不是「没数据」而是「没有这条规则」**，需要调用方判断。这种设计省掉了「空实现 vs 真返回」的区分，代价是每个调用点都要处理 `None` 分支。

### `max_admission_blocks`：启动期与运行期共用同一个公式

这是基类里设计得最讲究的一处。它的 docstring 写明用途：

> Single source of truth for both startup pool sizing (`max_allocated_blocks_per_request`) and the runtime admission gate, so requests admitted by startup can also be admitted at runtime.

也就是说**同一个函数算出来的数，既用来决定池子要多大，又用来在运行时判断某个请求能不能进来**。这两处必须一致，否则会出现「启动时算下来能跑，运行时却进不来」的死锁式故障——池子按 A 大小建，准入却按 B 判断，B > A 时永远等不到足够的块。

`SlidingWindowManager` 给出了这个上界的推导（注释写得很清楚）：滑窗在分块预填期间「持有最后 `sliding_window-1` 个已计算 token 的 KV 加上在途 token」，不会超过 `max_model_len`，再加上多模块投机解码需要多留的 `extra_retained_tokens`。最后 `cdiv(num_tokens, block_size) + 1` ——**那个 `+1` 的注释是这一整段里最实用的细节**：

> +1 because the sliding window may not start from the beginning of the block. E.g. block size 4 and num_token 4 needs two blocks [XXCD][EF] to store the 6-token window [CDEF].

窗口不与块边界对齐，所以 N 个 token 的窗口可能跨 N/block_size + 1 个块。

`MambaManager` 的公式则完全不看 token 数：

```python
if params.enable_prefix_caching:
    return 2 + params.num_speculative_blocks + params.num_prefill_checkpoint_blocks
return 1 + params.num_speculative_blocks
```

**常数级**——因为 Mamba 的「状态」是固定大小的，不随序列增长。而 `max_num_blocks_per_req` 却仍然要覆盖完整序列长度（`cdiv(max_len, block_size) + num_speculative_blocks`），注释解释：「块表行是按位置索引的，即使只有 2 + num_speculative_blocks 个状态块同时在位（更早的状态被 `remove_skipped_blocks` 置空）」。

**「同时在位的块数」与「块表行长度」是两个不同的量**，这是 Mamba 这条路上最容易搞错的地方。

### `FullAttentionManager`：从首块起找最长连续命中

它的 `find_longest_cache_hit` 是两段式。

**第一段（固定块）：** 从第一个块开始逐个查 `block_pool.get_cached_block(...)`，一旦查不到就 `break`。注释点明为什么可以 break：「A missing block implies every later block misses too (chained hashes)」——**链式哈希让「中途缺失」等价于「后面全缺」**，于是这个循环是 O(命中长度) 而不是 O(序列长度)。

**第二段（细粒度）：** 只在 `fine_grained`（`alignment_tokens < block_size` 且能整除）时执行。它把「第一个未命中的满块」内部按 `alignment_tokens` 切成若干份，**从高到低**探测内部边界：

```python
for fine_idx in range(max_partial_idx - 1, first_partial_idx - 1, -1):
    cached_tail = block_pool.get_cached_block(block_hashes[fine_idx], kv_cache_group_ids)
    if not cached_tail:
        continue
    ...
    hit_length = (fine_idx + 1) * alignment_tokens
    break
```

**从高到低是为了取最长命中**，第一个命中即最优。

最后两步归一化很关键：

```python
hit_length -= hit_length % alignment_tokens
num_blocks = cdiv(hit_length, block_size)
for computed in computed_blocks:
    del computed[num_blocks:]
```

先把命中长度向下对齐到 `alignment_tokens`（**不能返回违反对齐约束的长度**），再据此算出满块数并**截断块列表**。截断是必要的：细粒度命中可能在块内部结束，但块列表只能以块为单位。

`cache_blocks` 里也有一处对应逻辑：块大小大于哈希块大小时，额外调 `_cache_partial_tail_block` 缓存「落在块内部的提示词尾部」。**这是「能命中到块内部」的另一半实现**——前面负责查，这里负责登记。

### `SlidingWindowManager`：从右往左找连续窗口

滑窗的命中判定与全注意力正相反：**它不需要从序列开头命中，只需要最后 N 个 token 有连续的块**。

```python
"""Search from right to left and early stop when a match is found."""
for i in range(max_num_blocks - 1, -1, -1):
    if cached_block := block_pool.get_cached_block(...):
        ...
        computed[i] = cached
        num_contiguous_blocks += 1
        if num_contiguous_blocks >= sliding_window_contiguous_blocks:
            del computed[i + num_contiguous_blocks:]
            match_found = True
            break
    else:
        num_contiguous_blocks = 0
```

三个细节：

**一，未命中的位置填 `null_block` 而不是留空。** 初始的 `computed_blocks` 是 `[block_pool.null_block] * max_num_blocks`。docstring 给了例子：「sliding window manager should return a list like ([NULL, NULL, KVCacheBlock(7), KVCacheBlock(8)]) for block size 4 and sliding window 8」。**块表必须按位置索引**，被跳过的位置要占位，否则后面的索引就错位了。

**二，`num_contiguous_blocks = 0` 的重置。** 一旦断开连续就归零，所以最终命中的是「最靠右的一段连续窗口」，而不是「所有命中块的个数」。

**三，对齐检查只在第一个命中块上做：**

```python
if (num_contiguous_blocks == 0
        and block_size != alignment_tokens
        and ((i + 1) * block_size) % alignment_tokens != 0):
    continue
```

`num_contiguous_blocks == 0` 说明这是窗口的右边界——**右边界必须落在对齐点上**，因为右侧是新生成的内容，不能与别的组错位。而窗口内部各块的对齐无关紧要（它们只是被读，不参与后续拼接）。

还有一段 `TODO` 承认了复杂度问题：当前是 O(最大块数)，作者指出可以按 `sliding_window_contiguous_blocks` 步进优化成 O(n/k + k)，「which is good for low cache hit rate scenarios」。**这类 TODO 把「已知更优解」写进代码，比留在 issue 里更不可能被遗忘。**

`prefix_cacheable` 也不总是 `True`：

```python
return not (params.bounded_replay or params.rebuilt_by_replay)
```

两个参数分别表示「DeepSeek-V4.1 的重放窗口」与「会被别的组重放重建」。**「放弃前缀缓存」在这里是正确的选择**：既然窗口内容会被重放重新算出来，缓存它就是浪费块。

### `RSWAManager`：挖中间的洞

它继承全注意力，只覆写 `remove_skipped_blocks`。docstring 给出了精确的公式：

> Gap = blocks entirely within [ceil(prefix_len / block_size) * block_size, max(prefix_len, processed_computed_tokens - rswa_window))

也就是**提示词结束后、当前解码窗口开始前的那一段**——这段 KV 对注意力已经无用（既不在前缀里也不在窗口里），可以释放。

释放方式是「用 `null_block` 替换」而不是从列表里删除：

> Freed blocks are replaced with null_block in req_to_blocks so the block_table passed to FA4 is valid (null_block KV is all-zero; rswa_mask_mod marks gap positions as non-visible so FA4 skips them).

**这是 null 块最实在的用途**：块表要保持位置有效，但位置上的内容可以指向一个「全零且被 mask 掉」的占位块。所以 `null_block` 不是「省略」的写法，而是一个**真正可被 GPU 读取但内容无害的块**。

`_remove_blocks_in_range` 的实现有一处不太直觉的细节：

```python
for i in range(last_block - 1, first_block - 1, -1):
    if blocks[i] == self._null_block:
        break
    freed.append(blocks[i])
    blocks[i] = self._null_block
```

**倒序遍历**，注释解释「Iterates backward so newly-evictable tail blocks are reached even after earlier blocks in the range were nulled in a prior call」。而且遇到已经为 null 就 `break`——**说明这个区间总是从右端开始变空的**，左边的 null 表示「本次调用之前就已经空到那里」。用 break 而不是 continue，避免了每次调用都扫一遍已经空掉的前缀。

### `MambaManager`：状态块与「本轮不算、下轮再说」

Mamba 的块不是「token 的 KV」，而是「状态快照」。这带来几处独特实现。

**`has_positionally_stable_blocks` 是个属性而非类常量，且依赖前缀缓存开关：**

```python
@property
def has_positionally_stable_blocks(self) -> bool:
    # With prefix caching, Mamba can null interior states and relocate
    # speculative blocks in place. Without it, blocks keep their positions.
    return not self.prefix_caching
```

**「块的位置是否稳定」取决于是否开了前缀缓存**——因为它们会「置空中间状态」与「就地搬移投机块」。这个属性是给外部（如 KV 连接器）判断「能否按位置定位块」用的。

**拒绝「本步内刚被别的请求缓存过的块」。** 这是 `get_num_blocks_to_allocate` 里的一段：

```python
if (len(new_computed_blocks) > 0
        and new_computed_blocks[-1].block_hash in self.cached_blocks_this_step):
    # Mamba can't rely on blocks generated by other requests in the current step
    # To put it in the next step, we return num_gpu_blocks + 1 so
    # that kv_cache_manager will think there is no enough blocks to allocate now
    # and don't schedule it in the current step.
    return self.block_pool.num_gpu_blocks + 1
```

**返回一个大于池子容量的数，就能让调度器认为「块不够」从而把请求挪到下一步。** 这是一个用不变量做信号的手法——比新增一个「请稍后」的返回类型更省事，但代价是**读代码的人必须知道这个魔法数字的含义**。注释把它写清了，否则这行是无法维护的。

**「内部检查点」的条件判断极其细致。** `_needs_internal_checkpoint` 有七个条件与一个中间变量：

```python
checkpoint_idx = cdiv(query_end, self.block_size) - 2
reuses_resumed_block = (
    query_start % self.block_size != 0
    and checkpoint_idx == query_start // self.block_size
)
```

`reuses_resumed_block` 处理「从块中间恢复」：检查点落在被恢复的那个块里，那个块是该请求私有的，且在 forward 覆盖它之前被读取。这个条件的两个合取项都在说同一件事——「query 起点不在块边界上」且「检查点就是起点所在的块」。

最终判定要求：有检查点空间、`query_end >= prefill_end`、`is_mamba_prefill_checkpoint_valid(...)`（外部函数做对齐校验）、索引非负，以及「目标位置要么可复用恢复块、要么越界、要么是 null、要么在投机块范围内」。

**七个条件的合取没有缩写**，这是对的：这类判定一旦出错就是静默的性能退化或状态损坏，宁可写长。

### `CircularBufferManager`：把「不适用」写成显式声明

它的实现几乎是「全部否定」：

```python
zeroes_new_blocks: ClassVar[bool] = False
uses_slot_mapping: ClassVar[bool] = False
block_table_token_alignment: ClassVar[int | None] = None

@classmethod
def prefix_cacheable(cls, config: ManagerConfig) -> bool:
    return False

@classmethod
def max_allocated_blocks_per_request(...) -> int:
    # The ring occupies one block per request for its whole lifetime.
    return 1

@classmethod
def max_num_blocks_per_req(...) -> int:
    return 1
```

**三个类变量分别是「不擦零」「不用 slot 映射」「无对齐约束」**，每一条都在关掉上层的一个通用机制。`uses_slot_mapping = False` 的注释说得很清楚：「Groups that address their pages themselves (raw storage, ring buffers) take none.」——**环形缓冲自己寻址，不需要 worker 那边再算一遍 slot 映射。**

类 docstring 里还有一条容量约束：

> `block_size` is the ring capacity. It must exceed the compression ratio by the speculative lookahead: a speculative step stores all of its rows, drafts included, before acceptance is known, while the next step still reads the open group's committed keys from the ring.

**环的容量必须大于「压缩比 + 投机前瞻」**，否则投机的行会覆盖掉下一步还要读的数据。这条约束无法在代码里检查（它取决于模型与配置），只能写成文档——**而当约束只能写在 docstring 里时，它是不会自动被 enforce 的。**

## 关键取舍

**源码事实：** 类级策略（`prefix_cacheable`、`max_admission_blocks`）用 `@classmethod`，因为启动期算容量时还没有实例；`Params` 必须是冻结 dataclass 以便按值哈希比较。

**源码事实：** `max_admission_blocks` 的 docstring 明确它是「启动期池子大小与运行期准入门的单一真源」。

**源码事实：** `req_to_blocks` 用 `defaultdict(untracked_list)`，因为块不回指列表、不存在循环，显式排除在 GC 之外。

**源码事实：** 滑窗命中从右往左找，未命中位置填 `null_block`，且右边界必须落在对齐点上。

**源码事实：** `RSWAManager` 释放中间块时用 `null_block` 替换而非删除，保证块表按位置有效；清理区间倒序遍历并在遇 null 时 break。

**源码事实：** `MambaManager` 遇到「本步内刚被别的请求缓存的块」时返回 `num_gpu_blocks + 1` 迫使调度器推迟。

**源码事实：** `MambaManager.max_num_blocks_per_req` 要覆盖完整序列长度，而 `max_allocated_blocks_per_request` 只是常数——「在位块数」与「块表行长度」不同。

**源码事实：** `CircularBufferManager` 关掉了擦零、slot 映射与对齐约束，且环容量必须大于「压缩比 + 投机前瞻」（只能写在 docstring 里）。

**架构推论：** 「一个基类 + 五种差异极大的子类」能成立的前提是**基类抽象的是钩子而不是流程**。所有分发点都只是「查某张表、算某个界、释放某段块」，没有一条需要子类重写整个步骤。如果哪天要加一种「块的生命周期完全不同」的注意力类型（比如块可以被跨请求转移），这个基类就会撑不住——那时需要的是另一种组织方式，而不是再加一个子类。

**架构推论：** `None` 作为「没有这条规则」的语义，让基类免于为「不适用」提供空实现。但它把判断责任推给了调用方，且这个责任是隐式的——**新增一个返回 `None` 的方法时，没有任何机制提醒调用方要处理它**。

**架构推论：** `MambaManager` 那个「返回 `num_gpu_blocks + 1` 表示请稍后」的做法，是**用数值不变量编码控制流**。它在注释写清楚的当下是可读的，但一旦有人改了池子容量的计算方式（比如允许 `num_gpu_blocks + 1` 成为合法结果），这个信号就会失效。**用魔法值传递语义，总是要赌「没人会改那个范围的语义」。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/v1/core/single_type_kv_cache_manager.py` | `SingleTypeKVCacheManager` | 单类型管理器的抽象基类与策略钩子 |
| `vllm/v1/core/single_type_kv_cache_manager.py` | `SingleTypeKVCacheManager.Params` | 冻结的参数载体，供按值哈希 |
| `vllm/v1/core/single_type_kv_cache_manager.py` | `max_admission_blocks` | 启动期与运行期共用的每请求块上限 |
| `vllm/v1/core/single_type_kv_cache_manager.py` | `max_num_blocks_per_req` | 块表行长度（不同于在位块数） |
| `vllm/v1/core/single_type_kv_cache_manager.py` | `find_longest_cache_hit` | 各类型的前缀命中查询契约 |
| `vllm/v1/core/single_type_kv_cache_manager.py` | `get_num_blocks_to_allocate` | 准入判定与分配量计算 |
| `vllm/v1/core/single_type_kv_cache_manager.py` | `remove_skipped_blocks` | 释放已无用的块 |
| `vllm/v1/core/single_type_kv_cache_manager.py` | `_remove_blocks_in_range` | 区间释放，倒序并遇 null 停止 |
| `vllm/v1/core/single_type_kv_cache_manager.py` | `FullAttentionManager` | 从首块起找最长命中，支持细粒度 |
| `vllm/v1/core/single_type_kv_cache_manager.py` | `FullAttentionManager.find_longest_cache_hit` | 两段式：连续满块 + 块内边界探测 |
| `vllm/v1/core/single_type_kv_cache_manager.py` | `_cache_partial_tail_block` | 登记落在块内部的提示词尾部 |
| `vllm/v1/core/single_type_kv_cache_manager.py` | `SlidingWindowManager` | 滑窗：右侧连续窗口命中 |
| `vllm/v1/core/single_type_kv_cache_manager.py` | `RSWAManager` | 挖掉前缀与窗口之间的 gap 块 |
| `vllm/v1/core/single_type_kv_cache_manager.py` | `MambaManager` | 状态块管理、检查点与推迟调度 |
| `vllm/v1/core/single_type_kv_cache_manager.py` | `MambaManager._needs_internal_checkpoint` | 七条件合取的检查点判定 |
| `vllm/v1/core/single_type_kv_cache_manager.py` | `CircularBufferManager` | 环形缓冲，恒占一块，无前缀缓存 |
| `vllm/v1/core/single_type_kv_cache_manager.py` | `reachable_block_mask` | 供 worker 侧判断哪些位置可读 |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：v1/core/single_type_kv_cache_manager.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/core/single_type_kv_cache_manager.py)
- [vllm 文档：前缀缓存与滑窗](https://docs.vllm.ai/en/latest/features/automatic_prefix_caching.html)
- [Mamba 论文：线性时间序列建模（Gu & Dao, 2023）](https://arxiv.org/abs/2312.00752)

## 与主核 / AI 的关系

「一个基类 + 多个策略子类」与主核里「一种存储访问方法（AM）接口 + 多种索引实现（B-tree / hash / GiST）」是同一层设计。主核用 `amhandler` 注册每种 AM 支持的操作集合，vllm 用类变量（`uses_slot_mapping`、`zeroes_new_blocks`）声明能力。**两者都在做「能力声明」，但主核的声明是可查询的注册表，vllm 的是编译期常量**——后者更快，但无法在运行时新增。

`null_block` 的用法在主核里有一个精确对应物：**空闲页占位**。数据库的页表项在页被释放后仍保留一个占位（或指向「全零页」），这样按地址索引不会越界或错位。**「位置必须连续有效，内容可以无意义」这个需求，在存储与 KV cache 里长成了同一种东西。**

Mamba 那条「返回超出容量的数表示请稍后」的建议，值得与主核的「返回错误码而非魔法值」对照。数据库用 `EAGAIN` / `EINTR` 这类显式错误码表达「我现在不能做」，因为它要跨进程、跨语言传递；vllm 在进程内用数值信号，因为它省一次判断。**边界越窄，用魔法值越安全；边界越宽，越需要显式类型。**

对 agent / RAG 工程的实际含义有四条。其一，滑窗模型的命中要求「右侧连续」，所以在长对话里改中间内容不会让滑窗缓存全废，但改最近 N 个 token 会。其二，Mamba 类模型的每请求显存是常数级，这使它的并发上限远高于全注意力——**「能不能跑很多并发」取决于模型架构，不只是硬件**。其三，`remove_skipped_blocks` 会把不用的块换成一个全零占位块，所以「块表里有 N 项」不等于「占用了 N 块显存」。其四，环形缓冲不支持前缀缓存，意味着同一个系统提示词在每一步都要重算——这类模型不适合提示词很长的场景。
