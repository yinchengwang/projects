---
project: vllm
feature: parallel-layers
title: 张量并行算子
page_type: feature
schema_version: 1
refs:
  - path: vllm/model_executor/layers/linear.py
    symbols: [RowParallelLinear, QKVParallelLinear, MergedColumnParallelLinear]
  - path: vllm/model_executor/layers/vocab_parallel_embedding.py
    symbols: [VocabParallelEmbedding, ParallelLMHead]
  - path: vllm/model_executor/layers/rotary_embedding/__init__.py
    symbols: [get_rope]
---

# 张量并行算子

## 一句话职责

四个算子承担了整个模型的张量并行：`RowParallelLinear` 切输入维并 all-reduce 输出，`QKVParallelLinear` 与 `MergedColumnParallelLinear` 切输出维并按分片加载权重，`VocabParallelEmbedding` 切词表维——而 `get_rope` 用一张缓存表把十几种位置编码变体收进一个工厂函数。

## 机制是什么

模型代码只写「我要一个 QKV 合并投影，输入 `hidden_size`，头数这么多」（见 d02），具体切多少完全由这四个算子决定。所以**并行知识全部在这里**，而且它分成三层，每层的关注点不同：

1. **形状推导**：这个 rank 上参数应该是多大。（`num_heads`、`input_size_per_partition`）
2. **权重加载**：全局权重文件里的哪一段落到本地参数上。（`weight_loader`）
3. **前向通信**：算完之后要不要跨卡汇集。（`all_reduce` / `gather_output`）

三层里最容易出错的是第二层，因为它涉及**两套坐标系的换算**：全局（checkpoint 里的形状）与本地（本 rank 的参数形状）。源码里用两套命名明确区分——`total_num_heads` / `total_num_kv_heads` 是全局，`num_heads` / `num_kv_heads` 是本地。**读这段代码时若混淆这两套，形状推导会看起来自相矛盾。**

第三层的分工有一条硬规律：**列并行（切输出）不需要通信，行并行（切输入）必须通信**。因为列并行每个 rank 算的是输出的不同切片，可以直接拼；行并行每个 rank 算的是部分和，必须相加。

## 怎么实现的

**`RowParallelLinear`：偏置只由 rank 0 融合进 GEMM。**

前向的计算顺序是：拿本 rank 的输入分片 → 矩阵乘 → all-reduce。

```python
bias_ = None if (self.tp_rank > 0 or self.skip_bias_add) else self.bias
```

**注释把理由写得很直白**：「Only fuse bias add into GEMM for rank 0 (this ensures that bias will not get added more than once in TP>1 case)」。若每个 rank 都加一次偏置，all-reduce 之后偏置会被叠成 `tp_size` 倍——**这是一个静默的数值错误**，输出不会崩，只是整体偏大。

这行代码还把 `skip_bias_add` 揉进同一个条件：要让调用方自己加偏置时，就别在这里加。

构造期有一条互斥校验：`not reduce_results and (bias and not skip_bias_add)` 直接抛 `ValueError`，消息是「When not reduce the results, adding bias to the results can lead to incorrect results」。

**逻辑是：不做 all-reduce 时，每个 rank 只有部分和，此时代码若还加了完整偏置，结果就是错的。** 所以「不归约」与「加偏置」不能同时成立，除非偏置被显式延后（`skip_bias_add`）。

**`input_is_parallel` 决定要不要再切一次输入。**

为真时直接用输入；为假时沿最后一维切成 `tp_size` 份并取本 rank 那份，而且**调了 `.contiguous()`**。

**这个 `.contiguous()` 不是多余的**：`split` 出来的切片在内存上不连续，而后续的量化 GEMM 通常要求连续输入。省略它的后果取决于 kernel 实现——可能正常、可能变慢、可能报错。

`input_size_per_partition = divide(input_size, self.tp_size)`，而 `output_size_per_partition = output_size`（不切）。**这两个字段的不对称就是行并行与列并行的定义。**

`divide()` 这个辅助函数会在不整除时抛错，所以「输入维必须被 TP 整除」是被强制的，而输出维不受约束——这与上面 `output_partition_sizes = [output_size]` 一致。

**权重加载用「参数自己声明的维度」而不是算子自己判断。**

```python
input_dim = getattr(param, "input_dim", None)
is_sharded_weight = getattr(param, "is_sharded_weight", False)
if input_dim is not None and not is_sharded_weight:
    shard_size = param_data.shape[input_dim]
    start_idx = self.tp_rank * shard_size
    loaded_weight = loaded_weight.narrow(input_dim, start_idx, shard_size)
```

**切分维度来自 `param` 的属性，而不是算子的字段。** 这是必要的：量化参数（权重、scale、zero_point）有的沿输入维切、有的整体复制，只有创建参数的那一方知道。`is_sharded_weight` 则表达「这份参数在磁盘上已经是分片后的形状」，此时不该再切。

紧跟着一个针对标量的特例：「Special case for loading scales off disk, which often do not have a shape (such as in the case of AutoFP8)」。**零维张量先 `reshape(1)` 再加载**，因为某些量化方案把标量 scale 存成裸值。

最后统一走 `param.load_row_parallel_weight(...)` —— 把真正的拷贝逻辑交给参数类型，算子只负责算偏移。

**`QKVParallelLinear` 的核心是两套尺寸的并存。**

构造函数里先算本地头数：

```python
self.num_heads = divide(self.total_num_heads, tp_size)
if tp_size >= self.total_num_kv_heads:
    self.num_kv_heads = 1
    self.num_kv_head_replicas = divide(tp_size, self.total_num_kv_heads)
else:
    self.num_kv_heads = divide(self.total_num_kv_heads, tp_size)
    self.num_kv_head_replicas = 1
```

然后算 `output_sizes`：

```python
self.output_sizes = [
    self.num_heads * self.head_size * tp_size,      # q
    self.num_kv_heads * self.head_size * tp_size,   # k
    self.num_kv_heads * self.v_head_size * tp_size, # v
]
```

**每一项都乘了 `tp_size`。** 因为 `num_heads` 是本地值，乘回去就还原成**全局**尺寸。而 `StackedColumnParallelLinear` 基类会把每个 `output_size` 除以 `tp_size`——**所以「先乘再除」不是冗余，而是用全局尺寸表达意图，让基类统一切分。**

这解释了一个容易读错的地方：`output_sizes` 存的是全局值，而 `_get_shard_offset_mapping` 与 `_get_shard_size_mapping` 用的是本地值：

```python
shard_size_mapping = {
    "q": self.num_heads * self.head_size,
    "k": self.num_kv_heads * self.head_size,
    "v": self.num_kv_heads * self.v_head_size,
}
```

**同一个类里，一个字段是全局的、两个方法是本地的。** 区分依据是用途：`output_sizes` 交给基类去切，所以用全局；两个 mapping 用于「在本地参数里定位某一片」，所以用本地。

**`v_head_size` 是一个独立参数，默认等于 `head_size`。**

这支持了「V 的头维度与 Q/K 不同」的模型（DeepSeek 系 MLA 前的结构就是这样）。注意 `_get_shard_offset_mapping` 里 v 的偏移是 `(num_heads + num_kv_heads) * head_size`，而 v 的大小用 `v_head_size`——**偏移只累加 q 与 k 的 `head_size`，因为 v 是最后一段，它自己的维度不影响自己的起点**。

**`num_kv_head_replicas` 这个字段存在，但在这个类里只赋值不使用。**

它表达「KV 头在多少个 rank 之间被复制」。真正消费它的是加载逻辑（在基类与 `_load_fused_module_from_checkpoint` 里），本类只负责算出来。**这是一个「算在这里、用在那里」的字段**，读代码时若只在 `forward` 里找它会找不到。

**`_load_fused_module_from_checkpoint` 处理「checkpoint 里已经是融合 QKV」的情形。**

docstring 给了具体例子（Phi-3-mini）。逻辑是：没有 shard_id 时，用**全局**尺寸把加载进来的融合权重切成 q/k/v 三段，再分别走正常的分片加载。

```python
shard_offsets = [
    ("q", 0, self.total_num_heads * self.head_size),
    ("k", self.total_num_heads * self.head_size, self.total_num_kv_heads * self.head_size),
    ("v", (self.total_num_heads + self.total_num_kv_heads) * self.head_size, ...),
]
```

**这里必须用 `total_*`**，因为切的是磁盘上的全局张量。若误用 `num_*`（本地值），切出来的段会小 `tp_size` 倍，然后 `narrow` 会因越界或形状不符而失败——或者更糟：在 `tp_size=1` 时一切正常，只在多卡时炸。

**这一类「全局/本地混淆」的 bug 有典型的症状：单卡测试全过，多卡才失败。** 源码用命名的严格性（`total_` 前缀）来防守，而这是唯一的手段。

**`MergedColumnParallelLinear` 与 QKV 的差别只有两处，但都重要。**

第一，`shard_ids = tuple(range(len(output_sizes)))` —— 用**整数索引**而不是字符串名。因为它合并的是任意数量的同构投影（门控 + 上投影就是两个），没有语义名字可给。

第二，构造期有一条硬断言：`assert all(output_size % self.tp_size == 0 for output_size in self.output_sizes)`。**每个分片都必须能被 TP 整除**，否则切不开。QKV 那边靠 `divide()` 逐个检查，效果相同但粒度更细（能报出是哪一段不行）。

**`padding_size` 是一个有意思的补充参数。**

```python
if padding_size:
    self.output_sizes = output_sizes + [padding_size * self.tp_size]
```

它**在真实分片之后追加一段填充**，docstring 说明：「Extra output rows on each TP rank, appended after the real output_sizes. These rows are zeroed and are not checkpoint shards.」

**所以填充段是一个「假装存在的第四片」**：它参与形状计算（让总输出维变成某个对齐值），但不参与权重加载（`shard_ids` 是按原始 `output_sizes` 算的，填充段的 id 不在里面）。

注意填充尺寸也乘了 `tp_size`——每个 rank 追加 `padding_size` 行。注释说「appended after the real output_sizes」，所以填充在最后，不影响真实分片的偏移。

**为什么需要填充？** 因为某些融合 kernel（如 `SiluAndMul` 之后紧接着的量化 GEMM）要求输出维对齐到某个倍数。**用「多算一些零」换取「能用一个更快的 kernel」，是典型的用算力换实现简洁的取舍。**

**`VocabParallelEmbedding` 把词表拆成「原始 + 新增」两段，各自填充。**

这是全篇最精巧的机制。`__init__` 里：

```python
self.org_vocab_size_padded = pad_vocab_size(self.org_vocab_size, self.padding_size)
self.num_embeddings_padded = pad_vocab_size(
    self.org_vocab_size_padded + self.num_added_embeddings, self.padding_size
)
```

其中 `num_added_embeddings = num_embeddings - org_vocab_size` —— 这是 **LoRA 新增的词表项**。

docstring 解释了为什么分两段：为了让 LoRA 新增的嵌入「always at the end of TP-sharded tensors」，并且「we shard base embeddings and LoRA embeddings separately (both padded), and place them in the same tensor」。

**收益是：新增 LoRA 词表项时，原有分片的偏移不用重算。** 若混在一起切，加一个 token 就会让所有分片边界移动，已加载的权重全部错位。

`VocabParallelEmbeddingShardIndices` 这个 dataclass 把八个数（原始/新增 × 起止 × 是否填充）打包起来，并提供七个派生属性。它的 `__post_init__` 有**九条断言**，把「填充区间必须包含未填充区间」这类不变式全部显式化。

**九条断言看似冗长，但它们约束的是「四个区间的一致关系」**：每个区间有序、未填充在填充内、长度关系成立。这类多区间一致性最容易在改动时破坏，而断言把错误点定在构造时。

**前向有两条路径，而 `use_fused_embedding` 是分界。**

非融合路径：先 `get_masked_input_and_mask` 算出「不属于本 rank 的 token 掩码」与「把全局 token id 映射到本地索引的偏移」，查表，`masked_fill_` 把非本地的位置清零，再 all-reduce。

融合路径：直接调 `ops.vocab_parallel_embedding(...)`，传进去六个参数（权重、原始区间起止、原始填充数、新增区间起止），让融合 kernel 一次做完。

`get_masked_input_and_mask` 本身被 `@torch.compile(dynamic=True)` 装饰，注释说「torch.compile will fuse all of the pointwise ops below into a single kernel」。所以**即使是非融合路径，掩码与偏移的计算也被编译成一个 kernel**。

**掩码的含义值得说清**：每个 token 只属于一个 rank，其余 rank 在这个位置查出来的值必须清零，然后 all-reduce 才有正确的求和效果。**这就是「用加法实现「谁有谁出」」**。

**FP8 的嵌入有一个整数技巧。**

```python
if is_fp8(output_dtype):
    output_parallel = output_parallel.view(torch.int8)
...
return output_parallel.view(output_dtype)
```

注释解释：「Each vocab token has one owner, so FP8 bytes can use int8 SUM.」

**因为每个 token 只有一个 rank 有非零值，其余都是 0，所以 all-reduce 退化成「挑出那一个非零值」而不是真的求和。** 此时用整数加法是精确的，而浮点加法在 `0 + x` 之外不会出错但也没必要承担浮点归约的成本与不确定性。

**这也是「批不变性」的一个隐含保障：整数加法顺序无关，浮点加法不是。** 这段代码让词表并行的嵌入天然满足确定性。

`reduce_results` 为假时跳过 all-reduce；`parallel_group` 非空时用它而不是全局的 TP 组——**后者支持「词表并行度 ≠ 张量并行度」这类配置**。

**权重加载里的三个分支，第一个最容易被忽略。**

```python
if output_dim is None:
    # Parameters without an output dimension are copied onto all GPUs.
    ...
    copy_weight_(param.data, loaded_weight)
    return
```

**没有 `output_dim` 属性的参数被全量复制到所有卡。** 因为只有「沿输出维切分」的参数才需要按分片加载，其余（如层归一化的 scale、某些标量）本来就该每卡一份。

第二个分支是 packed 情形：`packed_dim == output_dim` 时按 `packed_factor` 缩放偏移与长度。这是为「多个逻辑词表项打包进一个物理元素」的量化方案服务的。

第三是常规路径：断言磁盘上的权重长度等于 `org_vocab_size`（**注意是原始的，不是填充后的**），`narrow` 出本 rank 的段，拷进参数前部，**然后把尾部清零**：

```python
param[loaded_weight.shape[0]:].data.fill_(0)
```

**尾部就是填充区**。清零是必须的——否则填充区留着未初始化内存，虽然理论上不会被查到（掩码会挡），但一旦偏移算错就会变成垃圾值输出。

**`ParallelLMHead` 是 `VocabParallelEmbedding` 的子类，差别在于它不做归约。**

它继承了全部切分与加载逻辑，只改前向（`forward` 直接返回本地 logits，不 all-reduce）并新增 `tie_weights`。

构造期有一处针对量化方法的能力检查：

```python
is_embedding_layer = not isinstance(self, ParallelLMHead)
quant_method_implements_embedding = method_has_implemented_embedding(type(quant_method))
if is_embedding_layer and not quant_method_implements_embedding:
    raise NotImplementedError(...)
```

**同一个基类被两种用途使用：查表（需要 `embedding` 方法）与产出 logits（只需要 `apply`）。** 所以检查要按子类区分——注释说得很清楚：「If we are making an embedding layer, then our quantization linear method must implement the embedding operation. If we are another layer type like ParallelLMHead, this is not important.」

`tie_weights` 在 d02 里已经被调用（`lm_head.tie_weights(model.embed_tokens)`），这里提供实现。**绑定之后两者共享同一份参数，所以嵌入层为「末卡也要建」的那个条件（d02）在这里闭合。**

**`get_rope` 用一个七元组做缓存键，且键在分支之前算好。**

```python
key = (head_size, rotary_dim, max_position, is_neox_style,
       rope_parameters_args, dtype, mrope_positions_factor)
if key in _ROPE_DICT:
    return _ROPE_DICT[key]
```

**键在 `if/elif` 链之前计算**，所以十几种变体共用同一张缓存表——只要参数完全相同，无论走哪个分支都返回同一个实例。

`rope_parameters_args` 是把字典转成 `tuple(items())`，且**列表值先转成元组**：

```python
rope_parameters_tuple = {
    k: tuple(v) if isinstance(v, list) else v
    for k, v in rope_parameters.items()
}
```

注释说「Transforms every value that is a list into a tuple for caching calls」。**因为 `list` 不可哈希而 `tuple` 可哈希**——若不做这层转换，任何带列表参数的配置（如 `mrope_section`）都会让键计算抛 `TypeError`。

**注意这不是「为缓存做的优化」，而是「为缓存做的正确性修正」**：不加这层，功能直接崩。

**`rotary_dim` 的推导有两条来源与一条范围校验。**

```python
if rotary_dim := rope_parameters.get("rope_dim", None):
    pass
else:
    partial_rotary_factor = rope_parameters.get("partial_rotary_factor", 1.0)
    if partial_rotary_factor <= 0.0 or partial_rotary_factor > 1.0:
        raise ValueError(...)
    rotary_dim = int(head_size * partial_rotary_factor)
```

显式给了 `rope_dim` 就用它；否则用 `head_size × partial_rotary_factor`。后者有范围校验，前者没有——**因为 `rope_dim` 是明确指定的，信任调用方；而 `partial_rotary_factor` 是比例，超出 0~1 无意义。**

**注意它用的是 `int(...)` 截断而不是四舍五入。** 所以 `partial_rotary_factor` 稍小于 1 时可能得到一个不整齐的 `rotary_dim`，而这个值会进缓存键——**两个浮点值的微小差异会导致缓存不命中并重建实例**，虽然功能正确。

**分派逻辑的分支层级是「先按 scaling_type，再按参数细分」。**

`default` 分支里有三层：有 `mrope_section` 就建 `MRotaryEmbedding`（多模态三维位置）；有 `use_fope` 就建 `FourierRotaryEmbedding`；否则建基础的 `RotaryEmbedding`。

`dynamic` 分支里按「有 `alpha`」与「有 `factor`」分成两族，**都不满足时抛错**，消息是「Dynamic rope scaling must contain either 'alpha' or 'factor' field」。

**这两处体现的判据是：`scaling_type` 只给出「大类」，具体实现还要看配了哪些参数。** 所以**只校验 `scaling_type` 是不够的**，必须校验到参数一级——否则用户能在配置里指定一个「类型对但参数缺」的组合，然后在第一次前向时崩在 `rope_parameters["factor"]` 的 `KeyError` 上。

其余分支（`llama3`、`linear`、`ntk`、`yarn`、`proportional`、`mllama4`、`bailing_mrope`）都是「直接从字典取必需参数并构造」。**注意它们用的是 `rope_parameters["factor"]` 这种直接索引，取不到就是 `KeyError` 而不是友好的 `ValueError`** —— 与 `dynamic` 分支的显式校验形成对比，说明**校验粒度在同一个函数里并不一致**。

**`_ROPE_DICT` 是一个永不清理的模块级字典。**

模块级的 `dict[tuple[Any, ...], RotaryEmbedding]`，只在末尾写入，从不删除。**在一个进程里这没问题**（不同的 rope 配置数量有限，通常个位数到几十），但如果有人在测试里大量构造不同配置，它会持续增长。

这是「用无限缓存换取实现简单」的取舍，而它之所以成立，是因为**配置空间在实践中很小**。若这个假设变了（比如支持从请求里动态指定 rope 参数），它就会变成内存泄漏。

## 关键取舍

**源码事实：** `RowParallelLinear` 只让 rank 0 把偏置融合进 GEMM，注释说明否则 TP>1 时偏置会被叠加多次；「不归约 + 加偏置」的组合在构造期直接抛 `ValueError`。

**源码事实：** `RowParallelLinear` 的 `input_size_per_partition` 被切、`output_size_per_partition` 不切；`input_is_parallel` 为假时沿最后一维切分并调 `.contiguous()`。

**源码事实：** 权重加载的切分维度取自 `param` 的 `input_dim` 属性而非算子字段；`is_sharded_weight` 为真时跳过切分；零维标量先 `reshape(1)` 再加载。

**源码事实：** `QKVParallelLinear` 的 `output_sizes` 存的是乘回 `tp_size` 后的**全局**尺寸（交给基类切分），而 `_get_shard_offset_mapping`/`_get_shard_size_mapping` 用**本地**尺寸；`_load_fused_module_from_checkpoint` 切磁盘张量时用 `total_*` 全局尺寸。

**源码事实：** KV 头在 `tp_size >= total_num_kv_heads` 时复制（`num_kv_heads=1`、`num_kv_head_replicas=divide(tp_size, total)`），否则切分。

**源码事实：** `v_head_size` 独立于 `head_size`，默认为后者；v 的偏移只累加 q 与 k 的 `head_size`。

**源码事实：** `MergedColumnParallelLinear` 用整数 `shard_ids`，并有「每个分片必须被 TP 整除」的断言；`padding_size` 在真实分片之后追加零行，不参与权重加载。

**源码事实：** `VocabParallelEmbedding` 把原始词表与 LoRA 新增词表分开填充、分开切分，使新增项始终落在所有分片的末尾；`ShardIndices` 的 `__post_init__` 用九条断言约束四个区间的一致关系。

**源码事实：** 嵌入前向在 FP8 时把输出视图成 `int8` 再 all-reduce，注释说明「每个 token 只有一个 owner，所以可以用 int8 SUM」；`get_masked_input_and_mask` 被 `torch.compile(dynamic=True)` 包裹以融合成一个 kernel。

**源码事实：** 无 `output_dim` 属性的参数被全量复制到所有卡；加载后参数尾部（填充区）被清零。

**源码事实：** `ParallelLMHead` 要求量化方法实现 `embedding` 才允许当嵌入层用，作为 LM head 时无此要求。

**源码事实：** `get_rope` 的缓存键在分支之前计算，`rope_parameters` 里的列表先转元组（否则不可哈希）；`rotary_dim` 用 `int()` 截断；`dynamic` 分支缺参数时抛 `ValueError`，其余分支直接索引字典（缺参数是 `KeyError`）。

**源码事实：** `_ROPE_DICT` 是模块级字典，只写不删。

**架构推论：** 「先乘 `tp_size` 再让基类除」这个写法看似冗余，实际是**用全局尺寸声明意图**。若直接写本地尺寸，基类会再除一次，得到错误结果。**这种「同一个量在两处以不同单位出现」的地方，命名是唯一防线**——`total_` 前缀不是风格问题，是正确性标记。

**架构推论：** 「全局/本地混淆」的故障模式极其恶劣：单卡全过、多卡才炸，且症状是形状不符或数值偏差。**一个团队若要在多处维护两套坐标系，最该做的是让名字自带单位**——这也是为什么 `output_sizes`（全局）与 `shard_size_mapping`（本地）必须在同一次阅读里被明确区分。

**架构推论：** 偏置只在 rank 0 融合，是把「归约语义」与「算子语义」分开处理的必然结果。**行并行的输出是部分和，任何「加一次就该够」的量都不能在每个 rank 上加。** 同样的道理适用于 loss 的缩放、正则项、位置编码的偏置——凡是「全局面量」在并行路径上都要指定唯一所有者。

**架构推论：** 「LoRA 新增词表追加在末尾」这条设计，本质是**让增量数据不移动存量数据的边界**。它在存储系统里对应「追加写而非原地重排」，在版本管理里对应「新增字段而非改字段顺序」。**代价是要求所有新增项都能容忍排在末尾**，若某个 LoRA 需要插入中间语义位置，这套设计就不成立。

**架构推论：** `dynamic` 分支显式校验参数而其余分支直接索引，说明**同一函数内的校验强度会随作者注意力漂移**。这不是大问题，但它解释了为什么「配置错误有时是友好的 `ValueError`、有时是裸 `KeyError`」——用户看到后者时往往以为是内部 bug。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/model_executor/layers/linear.py` | `LinearBase` | 线性层公共基类，承载量化方法与参数创建 |
| `vllm/model_executor/layers/linear.py` | `ReplicatedLinear` | 不切分的线性层 |
| `vllm/model_executor/layers/linear.py` | `ColumnParallelLinear` | 切输出维的列并行层 |
| `vllm/model_executor/layers/linear.py` | `StackedColumnParallelLinear` | 多分片列并行的基类 |
| `vllm/model_executor/layers/linear.py` | `MergedColumnParallelLinear` | 按整数 shard id 合并的分片列并行 |
| `vllm/model_executor/layers/linear.py` | `MergedColumnParallelLinear.__init__` | 分片整除断言与 `padding_size` 追加 |
| `vllm/model_executor/layers/linear.py` | `QKVParallelLinear` | QKV 合并投影，KV 头可切可分 |
| `vllm/model_executor/layers/linear.py` | `QKVParallelLinear.__init__` | 全局与本地两套尺寸的推导 |
| `vllm/model_executor/layers/linear.py` | `QKVParallelLinear._get_shard_offset_mapping` | 本地坐标系下的分片偏移 |
| `vllm/model_executor/layers/linear.py` | `QKVParallelLinear._load_fused_module_from_checkpoint` | 切磁盘上已融合的 QKV 权重 |
| `vllm/model_executor/layers/linear.py` | `RowParallelLinear` | 切输入维，输出 all-reduce |
| `vllm/model_executor/layers/linear.py` | `RowParallelLinear.forward` | 偏置仅 rank 0 融合，可选不足归约 |
| `vllm/model_executor/layers/linear.py` | `RowParallelLinear.weight_loader` | 按参数自声明维度切分，标量特例 |
| `vllm/model_executor/layers/vocab_parallel_embedding.py` | `VocabParallelEmbeddingShardIndices` | 四个区间的打包与九条断言 |
| `vllm/model_executor/layers/vocab_parallel_embedding.py` | `get_masked_input_and_mask` | 全局 token id 到本地索引的偏移与掩码 |
| `vllm/model_executor/layers/vocab_parallel_embedding.py` | `VocabParallelEmbedding` | 词表并行嵌入，原始与新增分开填充 |
| `vllm/model_executor/layers/vocab_parallel_embedding.py` | `VocabParallelEmbedding.forward` | 融合与非融合两条路径，FP8 用 int8 求和 |
| `vllm/model_executor/layers/vocab_parallel_embedding.py` | `VocabParallelEmbedding.weight_loader` | 三个分支的加载与尾部清零 |
| `vllm/model_executor/layers/vocab_parallel_embedding.py` | `ParallelLMHead` | 词表并行输出头，复用切分与加载 |
| `vllm/model_executor/layers/vocab_parallel_embedding.py` | `ParallelLMHead.tie_weights` | 与嵌入层共享参数 |
| `vllm/model_executor/layers/rotary_embedding/__init__.py` | `get_rope` | 位置编码工厂，七元组缓存键 |
| `vllm/model_executor/layers/rotary_embedding/__init__.py` | `_ROPE_DICT` | 模块级实例缓存，只写不删 |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：layers/linear.py](https://github.com/vllm-project/vllm/blob/main/vllm/model_executor/layers/linear.py)
- [vllm 源码：layers/vocab_parallel_embedding.py](https://github.com/vllm-project/vllm/blob/main/vllm/model_executor/layers/vocab_parallel_embedding.py)
- [Megatron-LM 论文：张量并行](https://arxiv.org/abs/1909.08053)

## 与主核 / AI 的关系

**「列并行不通信、行并行要归约」与主核的并行聚合分工同源。** 数据库做并行聚合时，各 worker 算部分和然后汇总（相当于行并行）；而做并行扫描取不同区段时直接拼接（相当于列并行）。**判据是「每个单元算的是整体的一部分，还是整体的一部分和」**——前者可以拼，后者必须加。

**「偏置只能加一次」与主核的「只在一处扣减」纪律一致。** 数据库记账时若每个副本都扣一次就成了 N 倍；分布式计数若每节点都加总是重复。**任何「全局面量」在并行路径上都必须指定唯一所有者或做归约去重**，而这两种方案里「唯一所有者」更简单也更少出错。

**「增量数据追加在末尾」与主核的追加写同源。** 数据库的 WAL 是追加写、堆表新增元组追加在页尾，都是为了让**存量数据的偏移不变**。词表并行把 LoRA 项追加在末尾，收益完全相同：加载新 LoRA 时原有分片不必重算。**代价是所有读者都要知道「有效区在哪个范围」，这需要额外的边界元数据（这里的 `org_vocab_end_index` 就是它）。**

**「缓存键必须可哈希」与主核把配置规范化后做计划缓存同类。** 数据库准备语句时把 SQL 文本规范化（去空白、统一别名）再算哈希；`get_rope` 把字典转成有序元组再算键。**两者都在解决「结构化配置无法直接当键」的问题，而键的规范化规则本身就是缓存正确性的一部分。**

对 agent 工程最实用的五条。其一：**并行切分要区分全局与本地两套尺寸并用命名标记**（`total_` 前缀），混用的 bug 只在多卡暴露。其二：**全局面量在并行路径上只能有一个所有者**，否则会被重复累加且不报错。其三：**增量数据追加在末尾**，别让新数据移动存量边界。其四：**缓存键的规范化是正确性问题而非优化**（列表转元组、浮点精度、字段顺序）。其五：**同一函数内的参数校验强度要一致**，否则用户会在 `ValueError` 与 `KeyError` 之间随机遇到。
