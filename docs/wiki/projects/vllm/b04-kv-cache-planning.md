---
project: vllm
feature: kv-cache-planning
title: KV cache 容量规划
page_type: feature
schema_version: 1
refs:
  - path: vllm/v1/core/kv_cache_planning.py
    symbols: [DefaultKVCacheConfigBuilder, unify_kv_cache_spec_page_size]
  - path: vllm/v1/core/kv_cache_coordinator.py
    symbols: [KVCacheCoordinator]
---

# KV cache 容量规划

## 一句话职责

启动时回答一个问题——「剩下的显存能放多少块，每层怎么分组」，并让所有 rank 对同一个答案达成一致（`kv_cache_planning.py`）；运行期回答另一个问题——「这张混合注意力的模型，最长命中到底有多长」（`kv_cache_coordinator.py`）。

## 机制是什么

这两件事看起来一个在启动、一个在运行，实际是**同一个决策的两半**：

| 阶段 | 需要什么 | 谁提供 |
|---|---|---|
| 启动 | 每层页大小统一、分层分组、块数 | `DefaultKVCacheConfigBuilder` + 规划函数 |
| 运行 | 跨组的统一命中长度 | `KVCacheCoordinator.find_longest_cache_hit` |

**启动阶段的硬约束是「所有 rank 必须拿到同一份配置」**，而各 rank 的可用显存、甚至层构成都可能不同（流水线并行下每个 stage 只有部分层）。这就是为什么规划不是「各 rank 自己算」，而是「合并全局视图 → 规划 → 按最紧的那个 rank 收缩」。

**运行阶段的硬约束是「命中长度必须是所有组都能接受的同一个值」**。全注意力组能命中 800 token，滑窗组只能命中 320，那么最终命中就是 320——因为**块表要按组分别填，但「算到第几个 token」只有一个**。

## 怎么实现的

### 第一步：把各层的页大小统一

`unify_kv_cache_spec_page_size` 要解决的问题是：混合注意力模型里不同层的「一块」物理大小可能不同。而 BlockPool 只能管一种大小的块（后面 `_get_kv_cache_groups_uniform_page_size` 的注释把这条列为假设之一：「Physical memory per block: Must be the same across all KV cache groups. Breaking this assumption is non-trivial due to memory fragmentation concerns」）。

统一到最大值，三条路：

**一，整除就放大块大小。** `max_page_size % layer_page_size == 0` 时 `new_block_size = layer_spec.block_size * ratio`。这是首选，因为它不浪费内存——只是让小块在 token 维度上变大。

**二，Mamba 层只能填充。** 注释解释得最清楚：「MambaSpec's page size is determined by its state shapes and does not scale with block_size, so pad the page instead.」——**Mamba 的页大小来自状态张量的形状，改块大小对它毫无影响**，所以只能把物理页补齐到最大值。

**三，非 MLA 注意力层在不整除时也填充。** 注意条件是 `isinstance(layer_spec, AttentionSpec) and not isinstance(layer_spec, MLAAttentionSpec)`——**MLA 被显式排除**，理由是：

> MLA is excluded because sparse MLA indexes the cache in whole token rows (see `flat_kv_row_view`), so its block stride can only be padded by its own row-aligned `alignment`, not to an arbitrary page size.

也就是 MLA 的块步长只能按行对齐填充，不能填到任意大小。于是它走 `else` 分支抛 `NotImplementedError`。

**这个异常是有意被上游捕获的**：docstring 写明「`get_kv_cache_groups` catches it to try the full-allocation fallback (e.g. MLA next to an incompatible sliding-window draft)」。**「无法统一」不是终局失败，而是切换到另一条策略的信号**——这个分层比直接报错给用户要好。

填充后的页用带步长的视图读：docstring 提到「the padded page is read through a strided view」。**「物理占满但逻辑只读一部分」靠 stride 实现**，这是让不同逻辑大小共存于同一池子的关键手段。

### 第二步：分层分组

`can_share_block_table(layer_configs)` 有三个合取条件：

```python
all(lc.manager_config == first.manager_config for lc in layer_configs.values())
and len({(spec.block_size, spec.dcp_sharded) for spec in specs}) == 1
and first.manager_cls.can_share_block_table(specs)
```

**第一个条件是「策略相同」**，且因为 `ManagerConfig` 是 `ManagerConfig(cls, Params(...))` 而 `Params` 是冻结 dataclass（b02 那页讲过），这个 `==` 是**按值比较**。策略不同就不能共用块表，因为它们对同一张表的语义理解不同。

**第二个条件是「块大小与 DCP 分片一致」**。

**第三个条件是「管理器自己认可」**——把最后一道判断交给管理器。例如 `MambaManager.can_share_block_table` 要求所有层的 `page_size_bytes`、`state_alignment`、`tp_replicated` 各自唯一：

```python
return (
    len({spec.page_size_bytes for spec in mamba_specs}) == 1
    and len({spec.state_alignment for spec in mamba_specs}) == 1
    and len({spec.tp_replicated for spec in mamba_specs}) == 1
)
```

注释：「Each state block holds one state, so every layer's page must match.」——**Mamba 的块表是「每行一个状态块」，若各层页大小不同就无法共用一行**。

`_bucket_layers` 的分组算法是**贪心 + 按层顺序**：对每一层，试着加入已有桶（要求加进去后仍满足 `can_share_block_table`），不行就新开一个桶。

**「按层顺序」这点值得注意**：它产生的是「连续段」分组，而不是「把同类的都聚到一起」。因为模型里注意力层通常是周期性重复的（10 个全注意力 + 20 个滑窗，可能是 `(1 full, 2 sw) × 10`），按顺序贪心刚好把周期性结构识别成 3 个组。而 `_get_kv_cache_groups_uniform_page_size` 的注释把这个模式讲得非常明确：

> The KVCacheManager allocates different block tables for each of the 3 layers in the pattern, and repeats each of them 10 times to generate the block_table for the 30 layers in the model.

**一个组是一个「模式位置」，不是一个「层集合」。** 这个认识决定了块表的存储方式：只需要存 3 张表，每张用 10 次。

### 第三步：算块数，并让所有 rank 对齐

`get_kv_cache_configs` 的 docstring 直接列了五步，是全文件最好的导读：

1. 合并所有 worker 的 KV cache spec，得到整个模型的规格。
2. 按整模型的层比例分组（同时做混合模型的规格统一）。
3. 用「投影到每个 worker 的组」做 auto-fit 与内存检查（考虑 PP 分片）。
4. 按分组策略为每个 worker 生成配置。
5. **把每个 worker 的块数改成所有 worker 中最小的那个**，并按比例缩减张量大小以避免分配无用内存。

第 5 步是全文最重要的设计。实现里有一处容易忽略但很关键的细节：

```python
min_num_blocks = min(cfg.num_blocks for cfg in kv_cache_configs)
for i, kv_cache_config in enumerate(kv_cache_configs):
    if kv_cache_config.num_blocks == min_num_blocks:
        continue
    # Re-plan with exactly the smallest rank's block count, so strides and
    # offsets stay consistent with the shrunken allocation.
    kv_cache_configs[i] = builder.get_kv_cache_config_from_groups(
        vllm_config, kv_cache_config.kv_cache_groups, min_num_blocks,
    )
```

**不是「把 num_blocks 字段改小」，而是「用最小的块数重新规划一遍」。** 注释说明了原因：`strides` 与 `offsets` 必须与新分配保持一致。若只改字段，张量描述与实际分配就会错位——**这类错误在启动时不会暴露，会在一段时间后以越界读写的形式出现。**

**`num_gpu_blocks_override` 的处理揭示了一处一致性要求**：

```python
# If `num_gpu_blocks_override` is set, the cache size that will actually
# be allocated is decoupled from the profiled `available_memory`. Reflect
# that in `available_memory` here so auto-fit, the admission check, and
# the per-worker config builder all plan against the same effective
# capacity.
adjusted_memory.append(override * bytes_per_block)
```

**override 必须回写成「等效可用显存」**，否则 auto-fit 按真实显存算、准入检查按真实显存判、而实际分配按 override——三者不一致。**这是「同一个量有多个消费者时，必须让它们读同一个值」的典型例子。**

**null 块要被预留**：

```python
# Reserve the null block BlockPool permanently holds back, so auto-fit and
# the capacity check both plan against usable blocks. Allocation below
# still uses the full memory.
check_memory = [avail_mem - _pool_bytes_per_block_from_builder(...) ...]
```

**检查用 `可用 - 一块`，分配用 `可用`。** 前者保守（保证至少真能跑），后者用满。这个不对称是有意的：BlockPool 会永久扣住一块当 `null_block`（b01 那页讲过），所以可用于请求的块数永远少一块。

`shrink_kv_cache_configs` 里同样的 `(num_blocks - 1)` 又出现一次，注释明确「The null block is held back by the BlockPool, as in the capacity check of `get_kv_cache_configs`」。**同一个不变量在两个地方各写一遍**——这是「没有单一真源」的痕迹，改动时必须两处同步。

### 第四步：预热后按实测收缩

`finalize_extensible_kv_cache` 处理的是「规划时还不知道 CUDA graph 与预热会吃掉多少显存」。

```python
block_counts = (times.num_kv_blocks for times in compilation_times if times.num_kv_blocks is not None)
num_blocks = min(block_counts, default=scheduler_kv_cache_config.num_blocks)
num_blocks = min(num_blocks, scheduler_kv_cache_config.num_blocks)
```

**两次 `min`**：一次在所有 rank 的实测值里取最小（跨 rank 一致），一次与调度器原计划取最小（不放大）。取最小而不是取平均，因为**任何 rank 内存不够都会让整批失败**。

然后若 `original_max_model_len == -1`（用户没指定，让引擎自动定），就**再 fit 一次**：

```python
# The context length was fitted to the reserved capacity, before CUDA
# graphs and warmup took their share; fit it again to the measured one.
```

**「拟合两次」是因为第一次的输入是估算值。** 这个细节很实在：自动定长时先按预留容量算一遍，等真实开销测出来后再按实测算一遍，否则宣称的上下文长度会大于实际能跑的。

`shrink_kv_cache_configs` 的张量缩放用整数运算：

```python
replace(tensor, size=num_blocks * tensor.size // kv_cache_config.num_blocks)
```

注释「their strides and offsets keep describing the reservation」——**只缩 size，不动 strides/offsets**，因为它们描述的是「预留描述」而非「实际占用」。

### 运行期：协调器如何调和多个组

`KVCacheCoordinator` 的 `__init__` 做三件事，第一件是一串断言：

```python
assert scheduler_block_size % hash_block_size == 0 and all(
    scheduler_block_size % g.block_size == 0
    for g in kv_cache_config.kv_cache_groups
)
```

**调度粒度必须是哈希粒度与每个组块大小的公倍数**，否则「按调度粒度推进」与「按组块粒度填表」就对不齐。注释直接标明它是「LCM of all group block sizes」。

然后建 `BlockPool`（b01）与一组 `single_type_managers`（b02）——**协调器是这两者的组合点**，它自己不实现任何块管理逻辑。

### 定点迭代：混合模型的命中长度

`HybridKVCacheCoordinator.find_longest_cache_hit` 是全文件最核心的算法，docstring 把它的收敛性论证写清了：

> Each attention type either accepts the current candidate length or reduces it. If any type reduces the length, restart checks over all types. This converges because length monotonically decreases and is bounded below by 0.

也就是：**候选长度在每一轮只会变小或不变，且下界是 0，所以必然终止。**

循环体里有一个关键优化：

```python
if issubclass(manager_cls, FullAttentionManager) and cached_blocks is not None:
    # Full attention is downward-closed: we only need to look up cached blocks once;
    # on subsequent iterations just trim to the (reduced) current hit length.
    continue
```

**「全注意力是向下封闭的」**：如果它能命中 L 个 token，那它一定能命中任何小于 L 的长度。所以第二轮起不用重查，只要裁剪。**这个性质来自全注意力的语义（保留全部前缀），而不是来自实现细节**——它是这个优化成立的唯一理由。

组顺序也是刻意排的：

```python
# Put full attention first: its efficient left-to-right scan provides
# a tighter initial bound, reducing work for subsequent groups.
self.attention_groups.sort(key=lambda g: not issubclass(g.manager_cls, FullAttentionManager))
```

**先跑全注意力是为了给后面的组一个更小的 `max_length`**，从而让滑窗那类 O(块数) 的扫描少走一些。这是「用便宜的高质量估算缩小后续搜索空间」的经典手法。

而 `is_simple_hybrid` 的短路更直接：

```python
is_simple_hybrid = len(self.attention_groups) == 2 and issubclass(self.attention_groups[0].manager_cls, FullAttentionManager)
...
if is_simple_hybrid:
    break
```

**「1 个全注意力 + 1 个其他」只需一轮**：全注意力给出上界，另一个组要么接受要么降低，降低后再跑一遍全注意力也只会得到同一个降低后的值（向下封闭）。所以第二轮必然收敛。

### `longest_hit_length` 与 `hit_length` 的差

这是整个函数里最微妙的一处。循环里维护两个变量：

- `hit_length`：当前候选（每轮会被降低）
- `longest_hit_length`：任何组单独报告过的最大值

最后：

```python
# Uncached shared prefix detection: if any attn. group cached a longer
# prefix than the reconciled hit, it is an uncached common prefix across
# requests that a sparse-retention group hasn't cached yet.
num_uncached_common_prefix_tokens = longest_hit_length - hit_length
```

**这个差值不是一个错误，而是一个有用信息**：某组（比如全注意力）已经缓存了 800 token 的前缀，但稀疏保留组（比如 Mamba 的检查点）只缓存到 320，于是最终命中是 320，而中间那 480 token 是「多请求共享、但稀疏组还没存」的公共前缀。

**这个信息让上层可以做一件优化**：既然这段前缀是跨请求共享的，那把它在稀疏组里补存下来是值得的——**一次投入，后续所有请求都命中**。`num_uncached_common_prefix_tokens` 就是给这个决策用的输入。

**把「两个不同组的不一致」从潜在错误转化为可用的优化信号**，是这个函数设计上最漂亮的一笔。

### 细粒度命中的开关与对齐

```python
self.enable_partial_hash_hits = (
    allow_partial_hash_hits
    and partial_hash_hits_enabled(
        kv_cache_config.kv_cache_groups, hash_block_size, dcp_world_size,
        [type(manager) for manager in self.single_type_managers],
    )
)
```

注释：「Fine-grained hash hits require compatible cache managers in every group.」——**细粒度是「要么全支持、要么全不支持」的全局开关**，因为它改变的是返回值的对齐粒度：

```python
@property
def _cache_hit_alignment_tokens(self) -> int:
    return self.hash_block_size if self.enable_partial_hash_hits else self.scheduler_block_size
```

**命中长度必须对齐到某个粒度，而这个粒度是全局的**。若一半组按哈希块对齐、一半按调度块对齐，协调器就无法给出单一数字。

算完之后还要把它**下发给每个管理器**：

```python
cache_hit_alignment_tokens = self._cache_hit_alignment_tokens
for manager in self.single_type_managers:
    manager.cache_hit_alignment_tokens = cache_hit_alignment_tokens
```

而 `SingleTypeKVCacheManager.__init__` 里对这个字段的注释是：「Hybrid fine-grained lookup may lower this after all participating managers have been validated by the coordinator.」——**初始化时给一个默认值，协调器验证完后回填真值**。这是一处「两阶段配置」。

### 两个防御性断言

```python
# Only groups that participate in prefix caching must satisfy the
# divisibility constraint; groups that opt out (e.g. circular buffers
# such as the GLM-5.3-Flash indexer tail) are scratch and excluded.
cacheable_block_sizes = [manager.block_size for manager, group in zip(...) if group.prefix_cacheable]
assert all(block_size % hash_block_size == 0 for block_size in cacheable_block_sizes), (...)
```

**不参与前缀缓存的组被排除在整除约束之外**——因为它们的块从不会被哈希，谈不上对齐。这个断言的错误消息把两边的值都打了出来（`block_sizes` 与 `hash_block_size`），排障时可直接比对。

另一处是 `assert self.attention_groups, "HybridKVCacheCoordinator requires at least one cacheable group."`——**「混合协调器一个可缓存组都没有」是配置错误**，早失败。

`find_longest_cache_hit_per_group` 是同一算法的**逐组版本**，返回 `(blocks_per_group, hit_lengths_per_group)`。它的存在是为了诊断：**当协调后的命中比预期短时，你需要知道是哪个组拖了后腿**。协调版本给出答案，逐组版本给出原因。

### Builder：把决策权让给模型

`DefaultKVCacheConfigBuilder` 的 docstring 有两层意思。第一层是免责：「This class is part of the experimental `KVCacheConfigBuilder` interface and is subject to change.」第二层是**边界声明**：

> Subclasses override only the hooks they need. The planning flow and its invariants (`num_gpu_blocks_override`, null-block reservation, auto-fit, admission checks, cross-rank convergence) stay in the Core-owned `get_kv_cache_configs`.

也就是说**这五个不变量不可被子类推翻**：override 的等效化、null 块预留、自动定长、准入检查、跨 rank 收敛。子类只能改「怎么分组」与「怎么从组生成配置」。

而分组本身还有一层让权：

```python
if not vllm_config.model_config.supports_kv_cache_grouping:
    return get_kv_cache_groups(vllm_config, layer_configs)
```

**模型只要声明 `SupportsKVCacheGrouping`，就自己负责分组。** 因为只有模型作者知道哪几层真的可以共用块表——自动推断终究是猜。但这条路与 HiSparse 冲突，直接抛 `ValueError` 并给出解决建议（「Remove one of them.」）。

## 关键取舍

**源码事实：** `unify_kv_cache_spec_page_size` 优先放大块大小，Mamba 与非 MLA 注意力在不整除时填充页，MLA 无法填充而抛 `NotImplementedError`，由上游捕获后走全分配兜底。

**源码事实：** 分组是「贪心 + 按层顺序」，一个组代表一个「模式位置」而非一组层。

**源码事实：** 跨 rank 对齐的方式是「用最小的块数**重新规划**」，而不是改字段，以保证 strides 与 offsets 一致。

**源码事实：** `num_gpu_blocks_override` 会被换算成等效可用显存，让 auto-fit、准入检查与配置生成读同一个值。

**源码事实：** null 块在容量检查时被扣掉（`avail - 一块`），在分配时用满；`shrink_kv_cache_configs` 里重复了这个 `-1`。

**源码事实：** 预热后会再取所有 rank 实测块数的最小值，并在自动定长时**再 fit 一次**。

**源码事实：** 混合命中用定点迭代，全注意力优先排序且第二轮起只裁剪（向下封闭），简单 1+1 组合一轮即收敛。

**源码事实：** `longest_hit_length - hit_length` 被解释为「跨请求共享但稀疏组未缓存的公共前缀 token 数」。

**源码事实：** 细粒度命中是全局开关，且会把对齐粒度回填给每个管理器。

**架构推论：** 启动规划最容易被低估的是「一致性」而不是「算得准」。块数算少一点只是浪费显存，算不一致会让某个 rank 分配失败或越界。**所以这个文件里大量代码是在传递与对齐同一个数字，而不是在优化公式。**

**架构推论：** 「无法统一页大小 → 抛异常 → 上游捕获 → 换策略」这条链比「直接报错」好，但也意味着**失败路径的复杂度被移到了调用方**。若将来再加一种不能填充的层类型，兜底路径也要相应扩展，否则用户会收到一个写着 `NotImplementedError` 的堆栈。

**架构推论：** 把「组间命中长度不一致」的差值暴露为 `num_uncached_common_prefix_tokens`，是把内部摩擦转成外部价值。**多数系统会把这种不一致当噪音丢掉；把它命名并返回，才有机会被上层利用。**

**架构推论：** `DefaultKVCacheConfigBuilder` 的「不变量不可覆盖、钩子可以覆盖」划分得很清楚，但它是**靠 docstring 声明的**而不是靠结构强制的——子类完全可以覆写整个 `get_kv_cache_groups` 而绕过所有检查。**当扩展点靠文档约束时，纪律只能靠 review 保证。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/v1/core/kv_cache_planning.py` | `DefaultKVCacheConfigBuilder` | 默认分组与配置生成，钩子可被覆写 |
| `vllm/v1/core/kv_cache_planning.py` | `unify_kv_cache_spec_page_size` | 把各层页大小统一到最大值 |
| `vllm/v1/core/kv_cache_planning.py` | `can_share_block_table` | 三层合取判断能否共表 |
| `vllm/v1/core/kv_cache_planning.py` | `_bucket_layers` | 按层顺序贪心分桶 |
| `vllm/v1/core/kv_cache_planning.py` | `get_kv_cache_configs` | 五步规划主流程与跨 rank 收敛 |
| `vllm/v1/core/kv_cache_planning.py` | `_check_enough_kv_cache_memory` | 容量不足时的可操作错误信息 |
| `vllm/v1/core/kv_cache_planning.py` | `_auto_fit_max_model_len` | 按可用显存反推可服务的上下文长度 |
| `vllm/v1/core/kv_cache_planning.py` | `resolve_mamba_block_size` | Mamba 块大小取非 Mamba 可缓存块大小的最大公约数 |
| `vllm/v1/core/kv_cache_planning.py` | `get_global_kv_cache_groups` | 合并所有 worker 的规格后分组 |
| `vllm/v1/core/kv_cache_planning.py` | `finalize_extensible_kv_cache` | 预热实测后收缩块数并重新定长 |
| `vllm/v1/core/kv_cache_planning.py` | `shrink_kv_cache_configs` | 按比例缩张量，只改 size 不动 strides |
| `vllm/v1/core/kv_cache_coordinator.py` | `KVCacheCoordinator` | 组合块池与各单类型管理器的门面 |
| `vllm/v1/core/kv_cache_coordinator.py` | `HybridKVCacheCoordinator` | 多组命中的定点迭代调和 |
| `vllm/v1/core/kv_cache_coordinator.py` | `HybridKVCacheCoordinator.find_longest_cache_hit` | 迭代、裁剪与未缓存公共前缀检测 |
| `vllm/v1/core/kv_cache_coordinator.py` | `find_longest_cache_hit_per_group` | 逐组独立评估，用于诊断 |
| `vllm/v1/core/kv_cache_coordinator.py` | `verify_and_split_kv_cache_groups` | 组排序与可缓存组筛选 |
| `vllm/v1/core/kv_cache_coordinator.py` | `_cache_hit_alignment_tokens` | 全局命中对齐粒度 |
| `vllm/v1/core/kv_cache_coordinator.py` | `get_num_cacheable_tokens` | 未来可命中的最长已定稿前缀 |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：v1/core/kv_cache_planning.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/core/kv_cache_planning.py)
- [vllm 源码：v1/core/kv_cache_coordinator.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/core/kv_cache_coordinator.py)
- [vllm 文档：显存与 KV cache 容量调节](https://docs.vllm.ai/en/latest/configuration/conserving_memory.html)

## 与主核 / AI 的关系

启动规划与主核里「共享缓冲区要开多大」是同一类问题，但主核的答案通常是配置项（`shared_buffers`），而 vllm 是**从剩余显存反推**。反推的代价就是要处理「各实例看到的剩余量不同」——主核靠运维人员给统一配置避开这个问题，vllm 没得选，只能自己收敛。

「用最小的块数重新规划而非改字段」这条，与主核里「`ALTER TABLE` 改成新表再切换」是同一思路的反面：**数据结构被多处引用时，改描述必须与实际布局一起改，不能只改描述**。数据库用「重建」保证一致，这里用「重新规划」保证一致。

「预热后再 fit 一次」与主核里「先估代价再实测、依据实测调参」相通：查询计划器先按统计信息估算，执行时可以修正。**估算与实测必须区分，且要允许实测推翻估算。**

定点迭代的收敛论证与主核里「锁升级重试直到无冲突」结构相同：**单调递减 + 有下界 = 必然终止**。这类论证比「加一个重试上限」可靠得多，因为它不依赖运气。

对 agent / RAG 工程的实际含义有四条。其一，启动时若看到 `No available memory for the cache blocks` 或「KV cache 大于可用显存」，可用的解法是降 `max_model_len` 或降 `gpu_memory_utilization`，错误消息里给了估算出的最大可服务长度。其二，混合注意力模型的命中长度由**最受限的那个组**决定，所以「全注意力能命中很长」不代表实际命中很长。其三，`num_uncached_common_prefix_tokens` 大于 0 说明存在跨请求共享前缀但未被稀疏组缓存，这是可优化的点。其四，`num_gpu_blocks_override` 会同时改变容量检查与分配，用它调小容量时不要指望「检查按真实显存算」。
