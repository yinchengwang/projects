---
project: vllm
feature: attention-contract
title: 注意力后端契约
page_type: feature
schema_version: 1
refs:
  - path: vllm/v1/attention/backend.py
    symbols: [AttentionBackend, AttentionImpl, AttentionMetadataBuilder]
  - path: vllm/v1/attention/backend.py
    symbols: [AttentionMetadata, AttentionLayer, CommonAttentionMetadata]
---

# 注意力后端契约

## 一句话职责

用「三个类 + 一份元数据」把十几种注意力实现（FlashAttention、FlashInfer、FlexAttention、MLA、SSM、CPU）统一起来：后端类回答「我有什么能力」，实现类做单层前向，构建器类把批次信息变成该后端内核认识的元数据。

## 机制是什么

注意力是推理里最吃显存带宽、也最讲究实现的一环。不同硬件、不同模型、不同 KV cache 布局需要不同的 kernel，而 kernel 之间的接口差异极大：有的自己写 KV cache，有的要求先写再用；有的返回 softmax 的 log-sum-exp，有的不返回；有的能跑 CUDA graph，有的完全不能。

vllm 的解法是**把差异全部挤进「能力声明」**，把共性留在三个抽象类里：

- **`AttentionBackend`** 是一个几乎只有 `@classmethod` 的**能力清单**。它没有实例状态，唯一的作用是回答「这个后端能不能干某件事」。
- **`AttentionImplBase` / `AttentionImpl` / `MLAAttentionImpl`** 持有单层的 state（head 数、scale、量化尺度），负责一次前向。
- **`AttentionMetadataBuilder`** 把**批级**信息转成**后端专属**的元数据。

这个三分法的判据是**生命周期**：能力是全局的、元数据是每批的、实现是每层的。三者频率不同，所以拆开。

## 怎么实现的

**能力声明是一张巨大的布尔表，而它被折叠成一个统一校验函数。**

`AttentionBackend` 定义了十几个「支持什么」的类方法，几乎全部默认 `False`：`is_mla`、`supports_sink`、`supports_alibi_sqrt`、`supports_mm_prefix`、`is_sparse`、`supports_sliding_window`、`supports_rswa`、`supports_non_causal`、`supports_batch_invariance`、`supports_hisparse`、`is_ssm`。默认 `False` 意味着**新后端必须显式声明能力**，不能靠继承「顺手」获得。

而 `validate_configuration` 是这个清单的**唯一消费方**，它接收十几个参数，逐条比对，最后返回一个**原因列表**而不是布尔值。

这个设计选择值得单独说：**返回原因列表而非布尔**，让选择器能告诉用户「为什么这个后端不行」。若返回布尔，用户只能看到「没有可用后端」，而实际原因可能是「head_size 不支持」或「compute capability 不够」——两者的排查方向完全不同。

里面有两条判据是**双向的**：`use_mla != cls.is_mla()` 与 `use_sparse != cls.is_sparse()`。所以「模型要 MLA 但后端不是 MLA」和「模型不要 MLA 但后端只能 MLA」都会被拒。**能力声明不只是「我能做」，还隐含「我只做这个」**——这一点对 MLA 与 sparse 是成立的，因为它们改变了 KV 的物理布局，无法与其他模式混用。

**一条能力默认值是通过另一个能力推导出来的，而不是固定值。**

`supports_device_cpu_query_lens_mismatch()` 的实现是 `return not cls.is_ssm()`。docstring 解释了原因：自适应验证会在 device 上裁剪 draft，导致 device 侧的 query 起点与 CPU 侧不一致；而 SSM 后端的循环状态规划是基于 CPU 侧每请求边界的，裁剪后就不匹配了。所以**默认支持，只有 SSM 例外**。

这是一处「默认从别的属性推导」的实例，它的风险是**隐式耦合**：`is_ssm` 的语义一旦变化，这个能力位也跟着变，而改动者可能没意识到。

**block size 的判定有一处「MultipleOf 语义被放宽」的注释。**

`supports_block_size` 检查框架级 block size 是否是内核要求的整数倍。注释说明了放宽的理由：「With hybrid_blocks feature, the framework-level block size only needs to be a multiple of the kernel's requirement, even if the kernel requires a fixed block_size.」——**内核要固定大小，但框架层只要整数倍就够**，因为框架会在内部再切分。

这条放宽是通过 `MultipleOf` 这个 dataclass 表达的：一个支持列表里可以混放「固定值」与「整数倍约束」。`get_preferred_block_size` 在默认值不被支持时，取支持列表里最小值。

**注意力类型是一个 `StrEnum`，理由是 `torch.compile`。**

`AttentionType` 有四个值：`DECODER`、`ENCODER`、`ENCODER_ONLY`、`ENCODER_DECODER`。注释写明「Use string to be compatible with `torch.compile`」——**编译后的图里不能出现自定义对象**，所以枚举必须是字符串。

`supports_attn_type` 的默认实现只认 `DECODER`。所以一个纯编码器后端不覆盖这个方法就会被判为不支持编码器注意力——**默认值是最窄的那个**，这与整体「默认 `False`」的取向一致。

**`CommonAttentionMetadata` 是跨后端共享的批级信息，它的字段分三类。**

第一类是**每个后端都要的**：`query_start_loc`（每请求在 query 张量里的起点）、`seq_lens`、`num_reqs`、`num_actual_tokens`、`block_table_tensor`、`slot_mapping`。

第二类是**成对保留 CPU 与 GPU 版本的**：`query_start_loc` 与 `query_start_loc_cpu`、`seq_lens` 与 `seq_lens_cpu_upper_bound`、`encoder_seq_lens` 与 `encoder_seq_lens_cpu`。docstring 只说了「For many of the tensors we keep both GPU and CPU versions」，但实际原因在别处注释里给出：**有些规划必须在 CPU 上做（拿不到 GPU 张量的值而不做同步），而有些计算必须用 device 侧的真值**。

`seq_lens_cpu_upper_bound` 的 docstring 把这条边界写得很清楚：「Precise for prefill rows and for all rows outside async spec decode; optimistic for async-spec decode rows (assumes every draft was accepted). Not safe for kernels that need exact per-row context lengths on decode rows.」——**一个「上界」字段，可能在某种场景下是所有行都正确的，也可能是乐观的**，用错就会读到不足的上下文。

第三类是**可选的后端专属字段**：`dcp_local_seq_lens`、`rswa_prefix_lens`、`mm_req_doc_ranges`、`is_prefilling`、`positions`。它们全部默认为 `None`，含义在 docstring 里逐条解释。

**`mm_req_doc_ranges` 的注释里有一句不起眼但重要的约束**：「A request's ranges must not overlap.」——双向注意力区间不能重叠，这是后端实现的前置条件，而它写在字段注释里而不是校验里。

**两个缓存字段带 `_` 前缀，暗示它们不是契约的一部分。**

`_num_computed_tokens_cache` 与 `_token_to_req_indices_cache` 是惰性计算的缓存。`compute_num_computed_tokens` 就是 `seq_lens - query_lens`，用 device 侧张量算，缓存一次。

`token_to_req_indices` 更复杂，它的注释解释了一个真实的陷阱：「Built from the device query_start_loc: adaptive verification decides the per-request draft split on device, so the CPU copy carries the right total but not the right per-request boundaries.」——**总量在 CPU 侧是对的，但每请求的边界只有 device 侧是对的**。所以这个映射必须用 device 张量建，而 `num_mapped_tokens` 却取自 CPU（因为总量是对的）。

**「总量对、边界错」这一类不一致，是 CPU/GPU 双份元数据的固有风险**，代码里用 `supports_device_cpu_query_lens_mismatch` 这个能力位来表达「这个后端能不能容忍它」。

**`unpadded` 的存在说明填充是常态而非例外。**

它按「真实请求数」与「真实 token 数」切掉尾部填充。方法上方有一条 TODO：「remove once we have FULL-CG spec-decode support」——**填充是为了 CUDA graph 的固定形状服务的**，等 spec-decode 支持了全图捕获，就不需要了。

所以这个字段不是「设计选择」而是「当前实现阶段的产物」，并且作者用 TODO 标记了它的临时性。这类「为性能妥协而临时引入、并留下明确移除条件」的代码，是全库最值得学习的部分。

**CUDA graph 支持被建模成四档而不是布尔。**

`AttentionCGSupport` 有四个值：`ALWAYS`、`UNIFORM_BATCH`、`UNIFORM_SINGLE_TOKEN_DECODE`、`NEVER`。前三个的区别是**批形状的自由度**：任意批、只要每请求 query 长度相同的批、只要纯单 token decode 的批。

这是一个精确的建模：CUDA graph 捕获的是固定形状的计算图，能重放的批形状范围决定了这个后端能用多少图加速。四档比布尔多表达了两种中间状态，而这两种正是投机解码（1+N 个 token）与混合批（prefill + decode）的关键场景。

`_cudagraph_support` 前面有一条注释：「Do not access directly. Call get_cudagraph_support() instead.」——**类变量私有、由类方法暴露**，这样某些 builder 可以在运行时按配置算出更精确的值。

**`get_varlen_cudagraph_max_query_len` 是这套模型的一个精细补充。**它回答「这个 builder 的全图捕获能重放的最大每请求 query 长度」。docstring 里把约束条件写得很细：真实请求长度在 1 到 L 之间、填充请求长度为 0、且「分批里只要有 prefill 就永不走这些图」。

**`reorder_batch_threshold` 表示「这个后端会重排批次」。**

非 `None` 表示会重排，值表示「会被拉到批首的 query 长度」。默认是 `None`（不重排）。

`_init_reorder_batch_threshold` 有两处调整逻辑，都值得看：

其一，若支持「投机即解码」（spec-as-decode），阈值会取「基础阈值」与 `max_decode_query_len` 的较大者。`max_decode_query_len` 自己的 docstring 说明它把「验证请求的 token 数」与「drafter 额外槽位」都算进去了，且**所有读这个值的地方都读同一个函数**，所以不会漂移。

其二，若开了 decode context parallelism（DCP）且该 backend 不支持 varlen DCP，则阈值被压回 1——**除非 KV cache 本身是 DCP 复制而非分片的**。这里有一行类型检查注释：「Non-attention specs default to dcp_sharded=False, hence the type check.」所以一个非 Attention 的 spec 会被当成「复制型」从而保留原始阈值。

**`AttentionImplBase` 用 `__new__` 做了一次跨库初始化。**

这不是常见的写法。`__new__` 里做三件事：拿 DCP 的 world size 与 rank（用 `get_dcp_world_size_and_rank(...)`，带一个布尔参数表示「配置为空或 DCP 已开」）；拿 PCP 的 world size 与 rank（**用 `try/except AssertionError` 兜底成 1/0**）；然后设置 `need_to_return_lse_for_decode = dcp_world_size > 1 and can_return_lse_for_decode`。

用 `__new__` 的理由注释写在方法里：「use `__new__` so that all subclasses will call this」——**子类不一定会调 `super().__init__`，但一定会走 `__new__`**。这是保证「所有实现都被注入 DCP 状态」的唯一可靠方式。

代价是**初始化分散在两处**（`__new__` 与 `__init__`），任何只看 `__init__` 的人都会漏掉这些字段。

**`lse_base_on_e` 是一个会静默出错的浮点约定。**

它标记后端返回的 log-sum-exp 用的是自然对数还是以 2 为底。注释写得毫不含糊：「The DCP combine kernel ... branches on this via its `IS_BASE_E` constexpr; getting it wrong silently corrupts the cross-shard softmax denominator.」

**「静默污染跨分片的 softmax 分母」**——错误的后果是数值细微偏差，不会报错、不会崩溃，只是输出质量下降。这类约定最危险，因为它无法通过「跑通」来验证。

**`supports_quant_query_input` 的收益写在注释里。**当后端支持预量化 query 时，注意力层会**先量化**再把 query 传给后端，从而让 `torch.compile` 把量化与前序算子融合。所以这个能力位的真正作用不是「能不能量化」，而是「能不能让编译器多做一次融合」——**能力位承载了编译优化的机会**。

**融合能力的判断被拆成五个独立方法。**

`fused_output_quant_supported`、`fused_qk_norm_rope_kvcache_supported`、`fused_qk_norm_mrope_kvcache_supported`、`fused_rope_kvcache_supported`、`set_interleaved_v_cache`。

拆开是因为**这五种融合的可行性互不蕴含**：一个后端可能能融合 RoPE 与 KV 写入，但不能融合 QK-norm。每个方法都被一个对应的融合 pass 消费（`AttnFusionPass`、`QkNormRopeKvCachePattern`、`RopeKVCacheFusionPass`），所以粒度与 pass 一一对应。

`set_interleaved_v_cache` 的注释说明了时序要求：「called ... when it is constructed, i.e. before any KV cache is written, so fused and unfused paths stay consistent」。**一旦有 KV 写过，切换布局就来不及了**。

**MLA 有独立的实现基类，而且它的量化支持是「全部支持」。**

`MLAAttentionImpl.fused_output_quant_supported` 直接返回「这几种量化键都在白名单里」，注释解释了原因：「Since MLA quantization is done manually in forward_impl (common code), all MLA backends support it by default.」

**这是一处「因为实现方式统一，所以能力可以统一」**——普通注意力后端各自调自己的 kernel 做量化，所以能力参差；MLA 的量化在公共代码里手工做，所以所有 MLA 后端天然一致。

`forward_mha`（预填充，MHA 风格）与 `forward_mqa`（解码，MQA 风格）是两个不同的前向接口，只有后者是 `@abstractmethod`。所以**MLA 后端可以不实现预填充的 MHA 路径**，此时由 `supports_dense_mha_prefill` 表达「我没有这条路，请全部走 MQA」——注释说明 sparse 实现若没有 dense 预填充路径，就对所有请求走 top-k MQA。

**`_q_scale` 等九个尺度字段定义在 `AttentionLayer` 这个 Protocol 上。**

Protocol 而不是 ABC，意味着它是**结构化契约**：任何有这些属性和 `forward` 的对象都能被传到实现层。这是为了让量化尺度的存放方式不被基类固定——FP8 KV cache 需要 scale，普通精度不需要。

**`subclass_attention_backend_with_overrides` 做了「保持身份」的处理。**

动态创建子类时，它显式设置 `__module__` 与 `__qualname__`，注释说明理由：「attention groups are keyed by `full_cls_name()`, so two backends of the same name from different modules must not share one group.」

所以**类的名字是分组键**。若两个同名后端来自不同模块而被错误地合并到同一组，KV cache 的共享就会出错。这让「动态生成子类」这件看起来无关紧要的便利函数，承担了正确性责任。

## 关键取舍

**源码事实：** `AttentionBackend` 的能力方法几乎全部默认 `False`；`validate_configuration` 返回**字符串原因列表**而不是布尔；`use_mla`/`use_sparse` 采用双向相等判定。

**源码事实：** `supports_device_cpu_query_lens_mismatch` 的默认值是 `not cls.is_ssm()`，即从另一个能力推导。

**源码事实：** `supports_block_size` 允许框架级 block size 是内核要求的整数倍（注释说明是 hybrid blocks 的放宽），`MultipleOf` 用来表达「整数倍」约束。

**源码事实：** `AttentionType` 是 `StrEnum`，注释说明是为了兼容 `torch.compile`；`supports_attn_type` 默认只支持 `DECODER`。

**源码事实：** `CommonAttentionMetadata` 为多个张量同时保留 device 与 CPU 版本；`seq_lens_cpu_upper_bound` 在异步投机解码行上是乐观上界，不安全用于需要精确行长度的内核。

**源码事实：** `token_to_req_indices` 必须用 device 侧 `query_start_loc` 构建，因为自适应验证在 device 上决定每请求的 draft 切分；而 `num_mapped_tokens` 取 CPU 侧总量。

**源码事实：** `AttentionCGSupport` 是四档枚举而非布尔；`_cudagraph_support` 私有并由 `get_cudagraph_support` 暴露。

**源码事实：** `AttentionImplBase.__new__` 负责注入 DCP/PCP 状态，注释说明选 `__new__` 是为了让所有子类都被覆盖；PCP 获取失败时用 `AssertionError` 兜底成 world=1。

**源码事实：** `lse_base_on_e` 决定 log-sum-exp 的底，注释明写取错会「静默污染跨分片的 softmax 分母」。

**源码事实：** 五种张量融合能力各自独立声明，与各自的融合 pass 一一对应；`set_interleaved_v_cache` 必须在任何 KV 写入之前调用。

**源码事实：** `MLAAttentionImpl.fused_output_quant_supported` 默认支持多种量化键，因为 MLA 的量化在公共代码里手工完成；`forward_mha` 非抽象、`forward_mqa` 抽象。

**源码事实：** `AttentionLayer` 是 Protocol；`subclass_attention_backend_with_overrides` 显式设置 `__module__` 与 `__qualname__`，因为注意力分组以 `full_cls_name()` 为键。

**架构推论：** 「能力清单 + 原因列表」的组合，把「能不能用」与「为什么不能用」放在同一个返回值里。这是**可选组件系统**的关键设计：没有它，用户只能看到「没有可用后端」，然后逐项猜测。判据是**这个失败可解释吗**——可解释的失败必须带原因，不可解释的才允许只给布尔。

**架构推论：** 双向判定（`use_mla != is_mla`）反映的是「能力声明同时是排他声明」。对改变数据布局的特性（MLA、sparse）这是正确的；对只影响性能的特性（sink、alibi_sqrt）则不该双向。**区分「改变语义」与「改变代价」的能力位，决定了它该单向上限还是双向相等。**

**架构推论：** `__new__` 注入是一个「绕开子类不合作」的手法。它的收益是保证覆盖，代价是**初始化分散在两处且不易察觉**。替代方案是强制子类调一个 `super().__init__` 并要求签名一致，但那会破坏「不同后端参数不同」的现状。**当契约无法统一时，用 `__new__` 抢在子类之前跑，是最后的办法。**

**架构推论：** 「静默错误比崩溃更危险」这条判据解释了为什么 `lse_base_on_e` 有如此详细的注释。数值约定的错误不会让测试失败，只会让输出质量轻微下降——**这类问题只能靠文档与代码审查防守，不能靠测试**。凡是「错了也不会报错」的常量，其注释密度应当高于平均。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `vllm/v1/attention/backend.py` | `AttentionType` | 四种注意力类型，字符串形式以兼容编译 |
| `vllm/v1/attention/backend.py` | `MultipleOf` | 表达「整数倍」约束的 block size 描述 |
| `vllm/v1/attention/backend.py` | `AttentionBackend` | 后端能力清单，几乎全部默认不支持 |
| `vllm/v1/attention/backend.py` | `AttentionBackend.validate_configuration` | 能力比对，返回不可用的原因列表 |
| `vllm/v1/attention/backend.py` | `AttentionBackend.supports_block_size` | 允许框架块大小是内核要求整数倍 |
| `vllm/v1/attention/backend.py` | `AttentionBackend.get_preferred_block_size` | 默认值不可用时取最小可支持值 |
| `vllm/v1/attention/backend.py` | `AttentionBackend.supports_device_cpu_query_lens_mismatch` | 从 `is_ssm` 推导的默认能力 |
| `vllm/v1/attention/backend.py` | `CommonAttentionMetadata` | 跨后端共享的批级元数据 |
| `vllm/v1/attention/backend.py` | `CommonAttentionMetadata.compute_num_computed_tokens` | device 侧算已计算 token 数并缓存 |
| `vllm/v1/attention/backend.py` | `CommonAttentionMetadata.token_to_req_indices` | 用 device 边界建 token 到请求的映射 |
| `vllm/v1/attention/backend.py` | `CommonAttentionMetadata.unpadded` | 切掉 CUDA graph 填充，带移除 TODO |
| `vllm/v1/attention/backend.py` | `AttentionCGSupport` | CUDA graph 支持的四档建模 |
| `vllm/v1/attention/backend.py` | `max_decode_query_len` | 统一的「最宽 decode」判据来源 |
| `vllm/v1/attention/backend.py` | `AttentionMetadataBuilder` | 批级元数据构建器基类 |
| `vllm/v1/attention/backend.py` | `AttentionMetadataBuilder._init_reorder_batch_threshold` | 按投机与 DCP 配置调整重排阈值 |
| `vllm/v1/attention/backend.py` | `AttentionMetadataBuilder.build_for_cudagraph_capture` | 图捕获专用的元数据构建入口 |
| `vllm/v1/attention/backend.py` | `AttentionLayer` | 结构化协议，承载量化尺度与层前向 |
| `vllm/v1/attention/backend.py` | `AttentionImplBase` | 实现基类，含五个能力位与两个并行度字段 |
| `vllm/v1/attention/backend.py` | `AttentionImplBase.__new__` | 在子类构造前注入 DCP/PCP 状态 |
| `vllm/v1/attention/backend.py` | `AttentionImpl` | 标准实现的抽象前向接口与量化模式 |
| `vllm/v1/attention/backend.py` | `AttentionImpl.fused_qk_norm_rope_kvcache_supported` | 五类融合能力之一 |
| `vllm/v1/attention/backend.py` | `MLAAttentionImpl` | MLA 实现基类，两个前向接口 |
| `vllm/v1/attention/backend.py` | `MLAAttentionImpl.do_kv_cache_update` | MLA 的 KV 写入，走 `concat_and_cache_mla` |
| `vllm/v1/attention/backend.py` | `subclass_attention_backend_with_overrides` | 动态子类并保持类身份以正确分组 |

## 参考

- [vllm 源码仓库](https://github.com/vllm-project/vllm)
- [vllm 源码：vllm/v1/attention/backend.py](https://github.com/vllm-project/vllm/blob/main/vllm/v1/attention/backend.py)
- [vllm 文档：注意力后端与配置](https://docs.vllm.ai/en/latest/design/kernel/paged_attention.html)
- [FlashAttention 论文（Dao et al., 2022）](https://arxiv.org/abs/2205.14135)

## 与主核 / AI 的关系

**「能力清单 + 原因列表」与主核的算子系统同构。** 数据库的访问方法（AM）也要声明它支持什么（`amcanorder`、`amcanunique`、`amclusterable`），优化器据此决定能不能用这个索引。差别是数据库把这些声明存进系统目录（可查、可跨版本对比），vllm 放在类方法里（进程内、重启重建）。**声明位置的差别决定了「可审计性」**：数据库能事后查「为什么选了这条计划」，vllm 只能靠日志与异常信息。

**「双向判定 vs 单向上限」与数据库的算子绑定同源。** 数据库里 `=` 运算符绑定特定类型，不能用于不匹配的类型（双向约束）；而 `canhash` 这类标志只表示「能用」，不表示「只能用」（单向上限）。vllm 对 MLA 与 sparse 做双向、对 sink 与 alibi 做单向，这个区分是精确的：**改变数据物理布局的能力必须互斥，只影响性能的能力可以共存。**

**「CPU/GPU 双份元数据」与主核的统计信息近似性同类。** 数据库的统计信息是采样的、可能过时，所以优化器必须知道「这是估计不是真值」。vllm 的 `seq_lens_cpu_upper_bound` 明确标注「在某种场景下是乐观上界」，不许需要精确值的内核使用。**给近似值明确的适用边界，比让它看起来像真值更安全。**

**「静默错误需要高注释密度」与主核的字节序/对齐约定一致。** 数据库里页格式的字节序、字段对齐写错不会报错，只会让数据读出来是垃圾。这类约定的共同特征是**无法通过功能测试发现**，因此必须靠文档与代码审查防守。

对 agent 工程最实用的五条。其一：**能力位要成对设计**——既表达「我能做什么」，也表达「我不能做什么」，否则选择器只能靠排除法猜。其二：**校验失败要返回原因列表**，让上层能区分「硬件不支持」与「配置冲突」。其三：**近似值必须标注适用边界**，别让「上界」被当成「真值」使用。其四：**静默出错的常量要写重度注释**（如 log-sum-exp 的底），它不会被任何测试抓到。其五：**临时妥协要留明确移除条件**（`unpadded` 的 TODO 是范例），否则临时方案会永久化。
