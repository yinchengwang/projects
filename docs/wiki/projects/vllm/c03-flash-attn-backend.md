---
project: vllm
feature: flash-attn-backend
title: FlashAttention 后端
page_type: feature
schema_version: 1
refs:
  - path: vllm/v1/attention/backends/flash_attn.py
    symbols: [FlashAttentionBackend, FlashAttentionMetadata]
---

# FlashAttention 后端

## 一句话职责

把 FlashAttention 库的二、三、四代差异、SM90 与 SM100 的 kernel 分歧、XPU 的另一套约束，全部收进一个后端类：能力声明按硬件与版本分叉，元数据构建器在「AOT 调度 / CUDA graph / 批不变性」三个开关之间做取舍。

## 机制是什么

这是上一页那套契约最有代表性的实现，也是**一个后端要同时支持四个大版本、三种设备、十余种组合**的实例。

它的复杂度不在于调 kernel，而在于**同一份代码在不同 FA 版本下语义不同**。最明显的一处是 CUDA graph 支持：

FA3 与 FA4 能重放任意批形状，FA2 不行——因为 FA2 有一个「`max_query_len=1` 的打包 GQA」特殊路径，用这个形状捕获的图无法用于混合 prefill-decode 批。源码注释把这段反直觉的行为直说了：

> For FA2, a graph is captured with max_query_len=1 ... then these graphs will not work for mixed prefill-decode (unlike FA3). This is due to special max_query_len=1 packed-GQA handling in FA2.
> In summary if we are running with spec decodes the graphs would work for mixed prefill-decode and uniform-decode. But for non-spec decodes the graphs would not work for mixed prefill-decode; sorta the inverse of UNIFORM_SINGLE_TOKEN_DECODE.

**「开了投机解码反而能用混合批，不开反而不能」** ——这条只有在 FA2 上成立的怪现象，被压成了一个 `UNIFORM_BATCH` 档位，并留了一条明确的 TODO 说明这是权宜之计。这是全页最值得记住的一段：**它承认抽象不够用，并说明了用哪个档位「够用」**。

## 怎么实现的

**KV cache 写入是独立动作，不是前向的一部分。**

`forward_includes_kv_cache_update = False` 与 `supports_kv_cache_update_bypass = True` 两个字段一起表达了这件事：FlashAttention 的 `forward` 不做 KV 写入，写入由单独的 `do_kv_cache_update` 完成。

这样安排的收益是**融合的机会**：KV 写入可以与 RoPE、QK-norm 融合成一次 kernel（上一页讲的五种融合能力因此才有意义）。若写入必须在 attention kernel 内部完成，这些融合就无从做起。

代价是**调用方必须记得调两次**，且顺序不能错——先写 cache 再做 attention。这个顺序由引擎层保证，而不是由后端自己保证。

**block size 的选择有三层，每层的依据都不同。**

第一层是 SM90 + FA4 + FP8 KV + `head_size == 512` 的特例，返回固定 64。注释说理由是「The SM90 FP8-KV-dequant kernel uses a 64-token TMA tile/page」——**TMA 的页大小决定了 block 大小**，这不是偏好而是硬约束。

第二层是 XPU：默认 `MultipleOf(64)`，但**只有用户显式要求了非 64 倍数时才降级成 `MultipleOf(16)`**。注释解释了这个反常的写法：「Block-size heuristics that read only the supported sizes ... would pick sizes below 64, so advertise 16 only when the user explicitly asked for a non-64-multiple block size.」

**为了不让启发式算法选到较慢的 16，平时不告诉它 16 是合法的。** 这是一个「能力声明影响他人决策」的实例：声明得越全，下游的自动选择反而越可能挑到次优值。

第三层是默认的 `MultipleOf(16)`。

`get_preferred_block_size` 与它对应：有 SM90 特例就取「默认值与 64 的较大者」，XPU 同理，否则回落到基类逻辑。

**head_size 的支持是阶梯式的。**

先要求 `head_size % 8 == 0`（对齐）。然后 `≤ 256` 一律支持；`> 256` 时，XPU 支持到 512，FA4 支持到 512，其余不支持。

**FA2 / FA3 不支持 512**，只有 FA4 与 XPU 可以。这个分界线与后面的 FP8 组合约束一致。

**能力位里有两处依赖运行时探测。**

`supports_per_head_quant_scales` 返回 `fa_version >= 3`；`supports_sink` 要求 `is_flash_attn_varlen_func_available()` 且 `flash_attn_supports_sinks()`。

**这些是「装了什么版本」决定的，不是「声明了什么」决定的**。所以同一个 `FlashAttentionBackend` 类在两台机器上的能力可能不同——这让能力清单从静态变成了「静态 + 环境」。

**`supports_combination` 是唯一返回具体理由的地方，而它的四条理由都指向真实的 kernel 限制。**

第一条：SM90 + 量化 KV + `head_size == 512` 时，block size 必须能被 64 整除，且**不支持块稀疏**。理由是这条 kernel 的 TMA 页约束。

第二条：有 sink 但 compute capability 低于 9.0 → 拒绝。sink 只在 Hopper 及以上可用。

第三条：`mm_prefix`（多模态双向注意力）+ 量化 KV + SM90 → 拒绝，理由写着「requires Triton」。所以这条路径在 SM90 上只能用 Triton 实现（另一个后端），FlashAttention 不接。

第四条：量化 KV 与 `flash_attn_supports_kv_cache_dtype` 的细粒度校验（带 head_size、head_size_v、has_sinks 参数）。失败时的理由是一个**完整的可用条件陈述**：「FP8 KV requires FA3 (SM90), equal-d512 FA4 (SM90), or FA4 (SM100).」

**这四条理由的共同价值是：用户看到的是「为什么」，而不是「不支持」。** 一个 FP8 配置在 SM80 上失败时，用户能立刻知道不是版本问题而是硬件代次问题。

**`FlashAttentionMetadata` 用一张 ASCII 图钉住了三个容易混淆的长度。**

```
|---------- N-1 iteration --------|
|---------------- N iteration ---------------------|
|- tokenA -|......................|-- newTokens ---|
|---------- context_len ----------|
|-------------------- seq_len ---------------------|
|                                   |-- query_len ---|
```

`context_len` 是已有的历史，`query_len` 是本次新算的，`seq_len` 是两者之和。**这三个词在推理代码里被反复混用**，而它们的混淆会导致「多算了 KV 或漏算了 KV」，症状是输出质量下降而非崩溃。用一张图钉住，比写三段文字有效。

**builder 的 `_cudagraph_support` 是一个类属性，但在类定义时就求值了。**

```python
_cudagraph_support = (
    AttentionCGSupport.ALWAYS
    if get_flash_attn_version() in (3, 4) or current_platform.is_xpu()
    else AttentionCGSupport.UNIFORM_BATCH
)
```

所以「这个后端支持哪种图」在**导入模块时**就定下来了，运行期不能变。这与上一页讲的「`get_cudagraph_support` 是类方法、某些 builder 可以按配置算更精确的值」形成对照——这里覆盖了类方法但只是返回这个常量，**等于放弃了按配置细化的能力**。

这是有意的：FA 版本在一个进程内固定，按配置细化没有收益。

**AOT 调度有三个前置条件，任一不满足就关掉。**

`aot_schedule = self.aot_schedule and not fast_build and not envs.VLLM_BATCH_INVARIANT`。

`self.aot_schedule` 在构造时定为 `fa_version == 3`——**只有 FA3 有 AOT 调度**。`fast_build` 关掉它是因为「投机解码只有少数几次迭代，不值得这个开销」。批不变性关掉它是因为「调度会随 `max_seqlen_q/k` 变化」，而批不变性要求同一批在不同形状下结果一致。

**这是三种不同性质的否决**：版本能力、成本权衡、正确性要求。三者混在一个布尔里，读代码时要知道每个条件为什么在。

**滑窗要在所有层上一致，AOT 才能用。**

`build()` 里有一段惰性初始化：首次调用时收集模型里所有 `FlashAttentionImpl` 的滑窗配置，若恰好一种就把它当 AOT 的滑窗参数，若多于一种就**永久关掉这个 builder 的 AOT**（同时写 `self.aot_schedule = False`）。

注释说明了为什么必须在 `build()` 而不是 `__init__` 里做：「the layers are constructed」——**builder 构造时层还没建好**，拿不到滑窗配置。

`_maybe_symmetrize_window` 是配套的修正：因果滑窗表示为 `(w, 0)`，非因果时改成 `(w, w)`，否则双向 query 只能看到前向窗口。全注意力 `(-1, -1)` 与已对称的窗口不动。

**调度元数据的持久化有一个正确性细节。**

`_store_scheduler_metadata` 在启用全图时把新算的元数据写进预分配的缓冲区，然后**把尾部清零**。注释是 woosuk 的原话：「Otherwise, some thread blocks may use the invalid scheduler metadata and overwrite the output buffer.」

**这是图捕获场景的典型陷阱**：缓冲区固定大小，本次只用了前 n 项，后 n+1 项是上次残留的值。若不清理，多余的工作线程会按垃圾元数据去写输出，**覆盖掉正确结果**——不崩溃、不报错，只是结果错。

**`causal` 张量的 dtype 检查带了一条解释为什么不能就地转换的错误消息。**

若 `causal` 是张量且 dtype 不是 `int32`，抛 `ValueError`，消息里写着：「Casting here would allocate a fresh tensor each build() and break FULL CUDA graph replay; allocate the buffer as int32.」

**「在这里转换会破坏图重放」**——因为图捕获时记录的是固定张量的地址，重新分配会换掉地址。这类约束只能靠拒绝执行来传达，就地容错反而是错的。

**`VLLM_BATCH_INVARIANT` 有三处影响，都是为了让结果与批形状无关。**

第一，`max_seq_len` 被强制成 `max_model_len`（而不是本批的实际最大值）——**让 kernel 的循环上界恒定**。

第二，`max_num_splits = 1`——禁止 split-KV，因为划分数会影响浮点累加顺序。

第三，关掉 AOT 调度（上面已述）。

**三条合起来的目标是「同一段前缀在不同批里算出完全相同的浮点数」**，这是确定性推理的要求，代价是性能（恒定的 `max_model_len` 意味着大量无用计算）与失去 split-KV 的并行度。

**元数据的构建里有两处「预分配 + 循环内拷贝」的模式，都是为了图捕获。**

`mm_prefix_query_range_tensor`（多模态双向范围）：先在 CPU 侧算，再 `copy_(..., non_blocking=True)` 到预分配的 GPU 缓冲，然后切片。

`rswa_prefix_lens`（R-SWA 前缀长度）同样：注释说「copy prefix lengths into persistent buffers (outside the compiled region) so forward() never allocates during CUDA graph capture」，并且 `rswa_window_tensor` 也是预分配的「so no CPU→CUDA copy is needed inside forward() during CUDA graph capture」。

**共同点是：所有会分配内存的动作都必须发生在 `build()` 里，不能发生在 `forward()` 里。** 因为 `forward` 会在图里被重放，重放时不能有任何分配。

**`forward` 的第一件事是处理「没有元数据」的情形。**

`if attn_metadata is None: return output.fill_(0)` —— 注释写着「Profiling run」。**显存测量跑**用的是这条路径：它只需要跑一遍整个模型看占多少显存，不需要正确结果，所以直接填零。

这也解释了一个常见困惑：为什么 profiling 阶段的速度与真实推理不同——它走的是完全不同的一条路。

**`forward` 里有一段关于 CPU 开销的警告，措辞很重。**

> With piece-wise CUDA graphs, this method is executed in eager-mode PyTorch. Thus, we need to be careful about any CPU overhead in this method. For example, `view` and `slice` (or `[:n]`) operations are surprisingly slow even in the case they do not invoke any GPU ops. Minimize the PyTorch ops in this method as much as possible.

**分段 CUDA graph 下，attention 的 forward 仍跑在 eager 模式里**——所以它是整条链路里少数「CPU 开销会直接变成延迟」的地方。这解释了为什么这个函数里出现了大量手写的参数提取而不是优雅的封装。

**stride 规范化是一处针对 TMA 的修补。**

从 KV cache 里切出 key/value 后，调 `canonicalize_singleton_dim_strides` 修正「size-1 维度上的退化 stride」。注释给了具体触发情形：「e.g. num_kv_heads=1 with TP」，以及原因：「FA3/4 on H100+ uses TMA, which requires ≥16-byte stride alignment」。

并且它只在真的改动了时才打 debug 日志——**说明这是一个理论上应该很少命中的补丁**，命中时值得记录。

**SM90 的 FP8 路径有一个能力上的反转，并在构造期就拒绝了一个组合。**

`uses_sm90_fa4_fp8_kv_dequant` 为真时，`supports_quant_query_input` 被强制关掉。注释解释：「FA4's SM90 d512 FP8-KV path consumes native FP16/BF16 Q. Other FA4 FP8 paths (notably SM100) require Q, K, and V to share an FP8 dtype.」

所以同一个「FA4 + FP8 KV」在 SM90 与 SM100 上对 query 精度的要求相反：SM90 要原精度 Q，SM100 要 FP8 Q。**「支持 FP8」这一条能力位在两种硬件上含义不同**，这就是为什么它必须按实例改而不是按类声明。

紧接着该路径与 DCP 冲突，构造期直接 `NotImplementedError`，理由是「DCP context attention is non-causal, which the SM90 FP8 MMA kernel rejects at dispatch time」。**在构造期拒绝比在首次前向时失败要好**——失败点越早，用户越容易对上配置。

**`do_kv_cache_update` 里有一条容易被误读的注释。**

它说明为什么不对 key/value 做 `[:num_actual_tokens]` 切片：「the `reshape_and_cache_flash` op uses the slot_mapping's shape to determine the number of actual tokens」。所以**切片责任在 op 内部，不在调用方**。若有人「顺手补上切片」，返回值不变但会多两次拷贝。

## 关键取舍

**源码事实：** `forward_includes_kv_cache_update = False` 且 `supports_kv_cache_update_bypass = True`，KV 写入由独立的 `do_kv_cache_update` 完成。

**源码事实：** block size 三层选择：SM90+FA4+FP8+512 头固定 64；XPU 默认 `MultipleOf(64)`，仅在用户显式要求非 64 倍数时才声明 `MultipleOf(16)`；其余 `MultipleOf(16)`。

**源码事实：** `head_size` 支持要求 8 对齐，≤256 无条件支持，>256 需 XPU 或 FA4。

**源码事实：** `supports_per_head_quant_scales` 与 `supports_sink` 依赖运行时探测的库版本与能力，因此同类在不同机器上能力不同。

**源码事实：** `supports_combination` 返回四条具体理由：SM90 d512 FP8 要求 block 能被 64 整除且不支持块稀疏、sink 需 SM≥9.0、mm_prefix + 量化 KV + SM90 需走 Triton、量化 KV 需要特定版本与硬件的组合（理由里点名了三种可用组合）。

**源码事实：** `FlashAttentionMetadata` 用 ASCII 图定义 `context_len` / `query_len` / `seq_len` 三者关系。

**源码事实：** `_cudagraph_support` 是类定义时求值的类属性，FA2 得到 `UNIFORM_BATCH`，FA3/FA4/XPU 得到 `ALWAYS`，并留有解释 FA2 特殊行为与待审计的 TODO。

**源码事实：** AOT 调度需同时满足「FA3」「非 fast_build」「非批不变性」；滑窗配置多于一种时永久关闭该 builder 的 AOT，且该判定延迟到首次 `build()` 因为层尚未构造。

**源码事实：** `_store_scheduler_metadata` 在复用缓冲区时把尾部清零，注释说明否则多余线程块会用无效元数据覆盖输出。

**源码事实：** `causal` 张量非 `int32` 时抛错，错误消息说明就地转换会破坏全图重放。

**源码事实：** `VLLM_BATCH_INVARIANT` 强制 `max_seq_len` 为 `max_model_len`、`max_num_splits = 1`、并关闭 AOT。

**源码事实：** `forward` 在 `attn_metadata is None` 时直接填零返回，对应 profiling 运行；函数内注释警告 eager 模式下的 CPU 开销，`view`/切片都有成本。

**源码事实：** SM90 FA4 FP8 反量化路径要求原精度 Q 因而关闭 `supports_quant_query_input`，且在 DCP>1 时构造期即抛 `NotImplementedError`。

**架构推论：** 「能力声明按实例改而非按类声明」是同一个后端要覆盖多代硬件的必然结果。代价是**能力不再是类的不变量**，任何缓存了能力结果的地方都可能过时。收益是同一个类名能在所有设备上工作，用户不必知道后端内部的分叉。**这条与上一页「`_run_kv_cache_dtype` 必须进缓存键」是同一种风险的两面。**

**架构推论：** 「AOT 调度」的三个关闭条件性质各异，却共用一个布尔。这在实际代码里很常见，因为它省掉了状态组合。代价是**关掉之后无法从外部知道是哪个原因**——排查「为什么没走 AOT」只能靠读代码。更细的做法是把关闭原因记进元数据，但那会增加每次构建的开销。

**架构推论：** 「预分配 + 循环内拷贝」是图捕获场景的通用模式，它的本质是**把内存分配从重放路径移到准备路径**。凡是会在图里重放的函数，都必须遵守「不分配」这条纪律——而这条纪律无法被类型系统表达，只能靠注释与代码审查。

**架构推论：** 「构造期拒绝不支持的组合」优于「首次前向时失败」。前者让错误发生在配置阶段（用户刚填完参数），后者发生在跑了几分钟后。**失败点越靠前，归因成本越低**，这条在容器启动、连接建立、模型加载、请求处理这条链上普遍成立。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/v1/attention/backends/flash_attn.py` | `FlashAttentionBackend` | 能力声明，按 FA 版本与硬件分叉 |
| `vllm/v1/attention/backends/flash_attn.py` | `FlashAttentionBackend.get_supported_kernel_block_sizes` | 三层 block size 选择 |
| `vllm/v1/attention/backends/flash_attn.py` | `FlashAttentionBackend._get_xpu_kernel_block_sizes` | XPU 上按用户意图决定是否声明 16 |
| `vllm/v1/attention/backends/flash_attn.py` | `FlashAttentionBackend._get_sm90_fa4_fp8_kv_block_size` | SM90 TMA 页约束导致的固定 64 |
| `vllm/v1/attention/backends/flash_attn.py` | `FlashAttentionBackend.supports_head_size` | 头维度支持的阶梯判定 |
| `vllm/v1/attention/backends/flash_attn.py` | `FlashAttentionBackend.supports_combination` | 返回四条具体不可用理由 |
| `vllm/v1/attention/backends/flash_attn.py` | `FlashAttentionMetadata` | 元数据，含三个长度的 ASCII 图 |
| `vllm/v1/attention/backends/flash_attn.py` | `FlashAttentionMetadataBuilder` | 元数据构建器，含 CUDA graph 档位 |
| `vllm/v1/attention/backends/flash_attn.py` | `FlashAttentionMetadataBuilder._get_scheduler_metadata` | AOT 调度元数据构造 |
| `vllm/v1/attention/backends/flash_attn.py` | `FlashAttentionMetadataBuilder._store_scheduler_metadata` | 复用缓冲区并把尾部清零 |
| `vllm/v1/attention/backends/flash_attn.py` | `FlashAttentionMetadataBuilder.build` | 主构建入口，含 AOT 与批不变性取舍 |
| `vllm/v1/attention/backends/flash_attn.py` | `_get_sliding_window_configs` | 收集所有层的滑窗配置 |
| `vllm/v1/attention/backends/flash_attn.py` | `_maybe_symmetrize_window` | 非因果时把滑窗对称化 |
| `vllm/v1/attention/backends/flash_attn.py` | `FlashAttentionImpl` | 单层实现，含 SM90 与 SM100 的路径分叉 |
| `vllm/v1/attention/backends/flash_attn.py` | `FlashAttentionImpl.forward` | 主前向，含 profiling 短路与 stride 规范化 |
| `vllm/v1/attention/backends/flash_attn.py` | `FlashAttentionImpl.do_kv_cache_update` | 独立的 KV 写入 |
| `vllm/v1/attention/backends/flash_attn.py` | `FlashAttentionImpl._forward_with_dcp` | 解码上下文并行的前向分支 |
| `vllm/v1/attention/backends/flash_attn.py` | `FA4DenseAttentionKernel` | JIT 编译的 FA4 稠密 kernel 封装 |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：backends/flash_attn.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/attention/backends/flash_attn.py)
- [FlashAttention 论文（Dao et al., 2022）](https://arxiv.org/abs/2205.14135)
- [FlashAttention 仓库](https://github.com/Dao-AILab/flash-attention)

## 与主核 / AI 的关系

**「同一个类在不同硬件上能力不同」与主核的编译期特性检测同源。** 数据库在编译期探测 CPU 是否支持 AVX2、是否支持原子操作，然后把结果编进代码路径。差别是数据库的探测发生在构建时（一次），vllm 的探测发生在导入时（每次启动）。**探测时机决定了「二进制可不可以跨机拷贝」**——这也是为什么 vllm 的 wheel 必须在目标机上装。

**「能力声明影响下游启发式」与主核的代价估算相互影响同类。** XPU 那处「平时不声明 16」的做法，本质是**不让优化器看到它会选错的选择**。数据库里也有类似手法：不给优化器某些索引的代价信息，避免它做出次优选择。**这是一种「通过隐藏选项来引导决策」的隐式控制**，有效但难以察觉——读代码的人会以为「只支持 64」。

**「AOT 调度 / 图捕获 / 批不变性」三者的互斥，与主核的隔离级别与并行度冲突类似。** 数据库里「串行化隔离」与「高并发」不可兼得；这里「批次无关的确定性结果」与「split-KV 并行加速」不可兼得。**这类「正确性与性能直接对冲」的选择必须显式建模**，否则会在某次优化中悄悄丢掉正确性。

**「尾部清零避免覆盖输出」与主核的缓冲区复用纪律一致。** 数据库复用页时也要清零或标记有效长度，否则残留数据会被当成有效内容。**复用缓冲区的安全性完全依赖于「有效长度」是否被严格执行**，而这一点在任何系统里都只能靠纪律维持。

对 agent 工程最实用的五条。其一：**同一套代码在不同库版本下语义可能不同**（FA2 的图支持反直觉），版本探测结果要写进日志而不是只在失败时暴露。其二：**能力声明要按硬件而非按类**，否则同一份配置在两种卡上行为不一致。其三：**图重放路径里不能分配内存**，所有预分配必须前移到构建阶段。其四：**复用缓冲区必须清零或严格按长度使用**，否则残留元数据会静默覆盖结果。其五：**不支持的组合要在构造期拒绝**，别等到跑了几分钟才失败。
